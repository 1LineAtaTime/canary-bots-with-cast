/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_data.cpp — data loading + static lookups (BOT_NAV_REALISM Phase 11 module split).
//
// Everything that reads the world/DB/config once and answers questions about it
// afterwards: hunt scripts, city POIs, NPC approach anchors, city routes, travel
// positions, equipment/imbuement/forge tables, and the route-lookup helpers
// (detectNearestPOI, findBestRouteSource, findCityRoute, startCityRoute).
//
// Originally carved out as "bot_hunt.cpp", which was a misnomer — it never held
// the hunt phase machine. Renamed when the real hunt machine got its own TU.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#pragma GCC diagnostic ignored "-Wunused-function"

// Defined near equipBot further down; declared here because the equipment loader above it
// prints the forge-tier sanity map at load time.
static uint8_t botForgeTierForLevel(uint32_t lv);


// ============================================================================
// Hunting system (Phase 4)
// ============================================================================

void BotEngine::loadHuntData() {
	// Closed-loop verification line: fires at startup + every /cavebot reload so
	// journalctl shows the effective whole-bot limits computed from the pct keys.
	g_logger().info("[BotEngine] pct caps: density inner={} mid={} outer={}, advstone chest+dummy={} (of botPlayersOnline={})",
		pctOfBotTotal(BOT_DENSITY_CAP_INNER_LIMIT_PCT),
		pctOfBotTotal(BOT_DENSITY_CAP_MID_LIMIT_PCT),
		pctOfBotTotal(BOT_DENSITY_CAP_OUTER_LIMIT_PCT),
		pctOfBotTotal(BOT_ADV_STONE_CHEST_DUMMY_CAP_PCT),
		g_configManager().getNumber(BOT_PLAYERS_ONLINE));

	// BOT_CSV Step 5: authored data comes from data/bot/authored/ (CSV), not MySQL.
	// This is the ONLY data entry point, reached from cold boot and from /cavebot reload
	// alike (game_functions.cpp:1169), always on a freshly constructed engine (§4).
	// A failure poisons the engine (activateBot/wakeBot refuse) rather than quietly
	// running the population on nothing.
	g_logger().info("[BotEngine] Loading authored bot data from {} ...", BOT_AUTHORED_DIR);
	reloadBotData();
}

std::string BotEngine::reloadBotData() {
	const int64_t t0 = botMonoMs();

	// ---- 1. parse EVERYTHING into temporaries; any BotCsvError -> zero mutation ----
	uint32_t nextId = 0;
	std::vector<HuntScript> newScripts;
	std::unordered_map<uint32_t, std::vector<BotPOI>> newPois;
	std::unordered_map<uint32_t, CityRouteGraph> newGraphs;
	std::vector<Waypoint> newAdvStone;
	std::unordered_map<uint32_t, std::vector<std::pair<Position, std::string>>> newTravelPos;
	std::unordered_map<uint32_t, std::string> newTravelNames;
	std::unordered_map<uint32_t, BotEquipment> newEquip;
	try {
		if (!std::filesystem::is_directory(BOT_AUTHORED_DIR)) {
			// Fail LOUD, unlike the chat loader's warn-and-disable (bot_chat.cpp:38-41):
			// a missing authored dir is a wrong cwd or a botched deploy, never a
			// legitimate empty state. The canary process cwd is /home/tibia/canary.
			throw BotCsvError { BOT_AUTHORED_DIR, 0, 0,
				"authored data directory missing (cwd must be the server root)" };
		}
		nextId = loadMetaCsv();
		loadHuntScriptsCsv(newScripts, nextId);
		loadCityPOIsCsv(newPois);
		loadCityRoutesCsv(newGraphs, newAdvStone);
		loadTravelPositionsCsv(newTravelPos, newTravelNames);
		loadEquipmentCsv(newEquip);
	} catch (const BotCsvError& e) {
		const std::string err = e.format();
		lastFailedDataLoadMs_ = OTSYS_TIME();
		lastDataLoadError_ = err;
		// There is no "serve the previous data" branch: the old engine was already
		// destroyed before this ran (§4). A bad file poisons the engine — loud, and
		// recoverable by fixing the file and running /cavebot reload again. The offline
		// validate.py (§8) and the optional csvcheck pre-flight (§4.1) keep it rare.
		dataPoisoned_ = true;
		dataPoisonReason_ = err;
		g_logger().error("[BOT_CSV] LOAD FAILED — refusing ALL bot activation until the "
			"file is fixed and /cavebot reload is run again. {}", err);
		return fmt::format("LOAD FAILED — bot activation refused. {}", err);
	}

	// ---- 2. COMMIT (nothing to guard: the members are empty, the engine is new) ----
	// currentPOI is the only raw pointer into this storage. Today this loop is a NO-OP:
	// bots_ is empty here, because the engine is freshly constructed at every call site
	// (§4) — which is exactly why requirement 3 removed the use-after-free hazard rather
	// than defending against it. Kept as insurance so that if anyone ever adds an
	// in-place reload path, the dangling-pointer SIGSEGV is already prevented.
	uint32_t poiCleared = 0;
	for (auto& b : bots_) {
		if (b.currentPOI != nullptr) {
			b.currentPOI = nullptr;
			poiCleared++;
		}
	}
	// Reload drops every in-flight ice-fishing session; the waypoint data behind them is
	// about to be replaced. (Moved here from the old loadHuntData head — the commit point
	// is where it belongs: a FAILED load must not drop sessions.)
	iceFishing_.clear();
	std::swap(huntScripts_, newScripts);
	std::swap(cityPOIs_, newPois);
	std::swap(cityRouteGraphs_, newGraphs);
	std::swap(adventurerStoneRoute_, newAdvStone);
	std::swap(travelPositions_, newTravelPos);
	std::swap(travelTownNames_, newTravelNames);
	std::swap(equipmentData_, newEquip);
	nextScriptId_ = nextId;

	// ---- 3. derived builders — the same set and order as the old loadHuntData tail.
	// Nothing holds raw pointers into these; buildZPortalGraph is a cache hit when the
	// map is unchanged. In-flight huntScriptIds are NOT revalidated here: every lookup
	// is a linear scan and the existing "script vanished -> abort/reroll" branches
	// handle staleness (audited — see bot_reloaddata_and_panicreset.md A.3).
	// BOT_ROUTE_SPLICE: clear then rebuild against the routes just swapped in. reloadBotData only
	// ever runs on a freshly constructed engine, so these are already empty -- this is insurance
	// against anyone adding an in-place reload path later, the same reasoning as the currentPOI
	// sweep above.
	routeSpliceCache_.clear();
	routeSpliceClean_.clear();
	s_routeSpliceRoutes = 0;
	s_routeSpliceWpsSaved = 0;
	buildTravelDestinationsCache();
	buildNpcApproachAnchors();
	buildZPortalGraph();
	// AFTER buildZPortalGraph on purpose: that builder sweeps the map in this same function, which
	// is the proof the map is loaded and the splice gates can query real tiles here.
	spliceCityRoutesAtLoad();
	buildTravelArriveTargets();
	buildSpellTables();
	buildConjureTables();
	buildSupportSpellTables();
	buildBotHouseIndex();
	rebuildFidgetItemPool();
	loadBotChatPhrases();

	dataPoisoned_ = false;
	dataPoisonReason_.clear();
	lastGoodDataLoadMs_ = OTSYS_TIME();

	size_t pairTotal = 0, poiTotal = 0, travelTotal = 0;
	for (const auto& kv : cityRouteGraphs_) {
		for (const auto& p : kv.second.pairs) { pairTotal += p.second.size(); }
	}
	for (const auto& kv : cityPOIs_) { poiTotal += kv.second.size(); }
	for (const auto& kv : travelPositions_) { travelTotal += kv.second.size(); }
	uint32_t botsOnHunts = 0;
	for (const auto& b : bots_) {
		if (b.huntScriptId != 0) { botsOnHunts++; }
	}
	const int64_t elapsed = botMonoMs() - t0;
	g_logger().info("[BOT_DATA_RELOAD] complete scripts={} pois={} routePairs={} travelPos={} "
		"equipment={} advStoneWps={} currentPOI_cleared={} elapsed_ms={}",
		huntScripts_.size(), poiTotal, pairTotal, travelTotal, equipmentData_.size(),
		adventurerStoneRoute_.size(), poiCleared, elapsed);
	return fmt::format("Reloaded authored data in {} ms: scripts={} pois={} routePairs={} "
		"travelPos={} equipment={} currentPOI_cleared={}. Bots currently on hunt scripts: {} "
		"(stale ids re-resolve next tick via the existing abort/reroll branches).",
		elapsed, huntScripts_.size(), poiTotal, pairTotal, travelTotal,
		equipmentData_.size(), poiCleared, botsOnHunts);
}

uint32_t BotEngine::loadMetaCsv() {
	const auto t = BotCsvTable::load(fmt::format("{}/meta.csv", BOT_AUTHORED_DIR),
		{ "key", "value" }, {});
	int64_t formatVersion = -1, nextId = -1;
	for (size_t i = 0; i < t.rowCount(); i++) {
		const std::string key = botCsvLower(botCsvTrim(t.raw(i, "key")));
		if (key == "format_version") {
			if (formatVersion != -1) { t.fail(i, "key", "duplicate key 'format_version'"); }
			formatVersion = t.getInt(i, "value", 1, 1000000);
		} else if (key == "next_script_id") {
			if (nextId != -1) { t.fail(i, "key", "duplicate key 'next_script_id'"); }
			nextId = t.getInt(i, "value", 1, UINT32_MAX);
		} else {
			t.fail(i, "key", fmt::format("unknown meta key '{}'", key));
		}
	}
	if (formatVersion != 1) {
		throw BotCsvError { t.fileName(), 0, 0,
			fmt::format("format_version {} unsupported (expected 1) or missing", formatVersion) };
	}
	if (nextId <= 0) {
		throw BotCsvError { t.fileName(), 0, 0, "missing next_script_id" };
	}
	return static_cast<uint32_t>(nextId);
}

void BotEngine::loadHuntScriptsCsv(std::vector<HuntScript>& out, uint32_t nextScriptId) {
	const std::string dir = BOT_AUTHORED_DIR;
	const auto t = BotCsvTable::load(dir + "/hunt_scripts.csv",
		{ "id", "name", "town_id", "min_level", "max_level", "vocation_mask",
		  "keep_distance_ek", "keep_distance_ms", "keep_distance_ed", "keep_distance_rp",
		  "enabled", "is_quest", "script_category" },
		// Known-passthrough: importer identity + display columns the runtime does not
		// consume. Listed so the unrecognized-column check stays fatal for real typos.
		// BOT_LURE_KITE: min_monsters is OPTIONAL on purpose. The code deploy ships
		// without the column in the CSV at all, so rolling the binary back cannot
		// poison the loader (an unrecognized header column is fatal — bot_csv.cpp).
		// The column arrives later in a data-only commit.
		{ "source", "source_file", "town_name", "script_type", "min_monsters" });

	// Parse ALL scripts (incl. disabled) so a disabled script with a corrupt waypoint
	// file still fails loudly; the enabled filter is applied at the end.
	std::vector<HuntScript> all;
	std::unordered_set<uint32_t> seenIds;
	for (size_t i = 0; i < t.rowCount(); i++) {
		HuntScript s;
		s.id = static_cast<uint32_t>(t.getInt(i, "id", 1, UINT32_MAX));
		if (!seenIds.insert(s.id).second) {
			t.fail(i, "id", fmt::format("duplicate script id {}", s.id));
		}
		if (s.id >= nextScriptId) {
			t.fail(i, "id", fmt::format("id {} >= meta.csv next_script_id {} (ids are never reused; "
				"bump next_script_id via the importer, never by hand-picking an id)", s.id, nextScriptId));
		}
		s.name = t.raw(i, "name");
		if (s.name.empty()) {
			t.fail(i, "name", "empty script name");
		}
		s.townId = static_cast<uint32_t>(t.getInt(i, "town_id", 0, UINT32_MAX));
		s.levelMin = static_cast<uint32_t>(t.getInt(i, "min_level", 0, 100000));
		s.levelMax = static_cast<uint32_t>(t.getInt(i, "max_level", 0, 100000));
		s.vocationMask = static_cast<uint32_t>(t.getInt(i, "vocation_mask", 0, UINT32_MAX));
		// getIntOr, not getInt: absent column and empty cell both mean "no lure".
		s.minMonsters = static_cast<uint8_t>(t.getIntOr(i, "min_monsters", 0, 20, 0));
		s.keepDistanceEK = static_cast<uint8_t>(t.getInt(i, "keep_distance_ek", 0, 255));
		s.keepDistanceMS = static_cast<uint8_t>(t.getInt(i, "keep_distance_ms", 0, 255));
		s.keepDistanceED = static_cast<uint8_t>(t.getInt(i, "keep_distance_ed", 0, 255));
		s.keepDistanceRP = static_cast<uint8_t>(t.getInt(i, "keep_distance_rp", 0, 255));
		s.enabled = t.getInt(i, "enabled", 0, 1) != 0;
		s.isQuest = t.getInt(i, "is_quest", 0, 1) != 0;
		s.scriptCategory = t.raw(i, "script_category");
		// Reservation key = lowercased script name — unchanged from the SQL loader (the
		// spawn_group column was dropped in the per-vocation hunt-route de-dup; see the
		// long WARNING comment in the pre-CSV bot_data.cpp:70-85 / git history for why
		// this is NOT one-bot-per-physical-spawn and why that is accepted).
		s.spawnGroup = s.name;
		std::transform(s.spawnGroup.begin(), s.spawnGroup.end(), s.spawnGroup.begin(), ::tolower);
		all.push_back(std::move(s));
	}

	// ---- per-script waypoint files: <dir>/hunt_waypoints/<id>.csv, line order = seq ----
	for (auto& script : all) {
		const std::string wpPath = fmt::format("{}/hunt_waypoints/{}.csv", dir, script.id);
		const auto wt = BotCsvTable::load(wpPath, // missing file -> BotCsvError (enabled or not)
			{ "phase", "waypoint_type", "pos_x", "pos_y", "pos_z" },
			// label: viewers read it from the FILE (Milestone 2); deliberately NOT in the
			// ABI Waypoint struct — adding it would force a full rebuild.
			{ "label", "extra_data" });
		int lastRank = 0;
		for (size_t i = 0; i < wt.rowCount(); i++) {
			const std::string phase = botCsvLower(botCsvTrim(wt.raw(i, "phase")));
			int rank = 0;
			if (phase == "travel_to") { rank = 1; }
			else if (phase == "hunt_patrol") { rank = 2; }
			else if (phase == "travel_from") { rank = 3; }
			else { wt.fail(i, "phase", fmt::format("unknown phase '{}'", phase)); }
			if (rank < lastRank) {
				wt.fail(i, "phase", "phase blocks out of canonical order "
					"(travel_to, hunt_patrol, travel_from — any subset, never interleaved)");
			}
			lastRank = rank;

			const Position pos(
				static_cast<uint16_t>(wt.getInt(i, "pos_x", 0, 65535)),
				static_cast<uint16_t>(wt.getInt(i, "pos_y", 0, 65535)),
				static_cast<uint8_t>(wt.getInt(i, "pos_z", 0, 15)));
			const WaypointType wpType = botCsvWaypointType(wt, i, "waypoint_type");
			const std::string extraData = wt.raw(i, "extra_data");

			Waypoint wp(pos, wpType);
			wp.extraData = extraData;
			// itemId derivation — UNCHANGED from the SQL loader (parity is gated by the
			// Step-5 dump diff): MACHETE default; USE_WITH tries stoi and falls back to 0
			// for non-numeric extra_data (e.g. "tile_item:21104,21105", NPC names).
			if (wpType == WaypointType::MACHETE) {
				wp.itemId = MACHETE_ITEM_ID;
			} else if (wpType == WaypointType::USE_WITH && !extraData.empty()) {
				try { wp.itemId = static_cast<uint16_t>(std::stoi(extraData)); }
				catch (...) { wp.itemId = 0; }
			}
			wp.isWalkOnFc = isWalkOnFcTile(wp.pos);
			if (rank == 1) { script.travelToWaypoints.push_back(wp); }
			else if (rank == 2) { script.patrolWaypoints.push_back(wp); }
			else { script.travelFromWaypoints.push_back(wp); }
		}
	}

	// ---- orphan waypoint files (a file with no matching script id is a stale edit) ----
	for (const auto& entry : std::filesystem::directory_iterator(dir + "/hunt_waypoints")) {
		const std::string fname = entry.path().filename().string();
		if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".csv") {
			continue; // .bak/.tmp generations etc.
		}
		const std::string stem = fname.substr(0, fname.size() - 4);
		uint32_t id = 0;
		try { id = static_cast<uint32_t>(std::stoul(stem)); }
		catch (...) {
			throw BotCsvError { entry.path().string(), 0, 0, "non-numeric waypoint filename" };
		}
		if (!seenIds.count(id)) {
			throw BotCsvError { entry.path().string(), 0, 0,
				fmt::format("orphan waypoint file — no script id {} in hunt_scripts.csv", id) };
		}
	}

	// ---- targets: comma-encoded multi-names split exactly as the SQL loader did ----
	const auto tt = BotCsvTable::load(dir + "/hunt_targets.csv", { "script_id", "monster_name" }, {});
	std::unordered_map<uint32_t, HuntScript*> byId;
	for (auto& s : all) {
		byId[s.id] = &s;
	}
	for (size_t i = 0; i < tt.rowCount(); i++) {
		const uint32_t sid = static_cast<uint32_t>(tt.getInt(i, "script_id", 1, UINT32_MAX));
		const auto it = byId.find(sid);
		if (it == byId.end()) {
			tt.fail(i, "script_id", fmt::format("no script id {} in hunt_scripts.csv", sid));
		}
		const std::string names = tt.raw(i, "monster_name");
		std::istringstream ss(names);
		std::string name;
		while (std::getline(ss, name, ',')) {
			const auto start = name.find_first_not_of(" \t");
			const auto end = name.find_last_not_of(" \t");
			if (start != std::string::npos) {
				name = name.substr(start, end - start + 1);
				std::transform(name.begin(), name.end(), name.begin(), ::tolower);
				it->second->targetNames.push_back(name);
			}
		}
	}

	// ---- enabled filter LAST (so disabled scripts' files were still validated) ----
	out.clear();
	for (auto& s : all) {
		if (s.enabled) {
			out.push_back(std::move(s));
		}
	}
	if (!csvQuiet_) {
		g_logger().info("[BotEngine] Loaded {} hunt scripts with waypoints and targets", out.size());
	}
}

void BotEngine::loadCityPOIsCsv(std::unordered_map<uint32_t, std::vector<BotPOI>>& out) {
	const auto t = BotCsvTable::load(fmt::format("{}/city_pois.csv", BOT_AUTHORED_DIR),
		{ "town_id", "name", "pos_x", "pos_y", "pos_z", "poi_type", "enabled" },
		// weight: SELECTed-but-never-assigned in the SQL loader — the no-read is kept
		// AS-IS (do not start honoring it during a migration; separate decision).
		{ "weight" });
	out.clear();
	std::set<std::pair<uint32_t, std::string>> seen;
	uint32_t count = 0;
	for (size_t i = 0; i < t.rowCount(); i++) {
		if (t.getInt(i, "enabled", 0, 1) == 0) {
			continue; // parity with WHERE enabled = 1
		}
		const uint32_t townId = static_cast<uint32_t>(t.getInt(i, "town_id", 0, UINT32_MAX));
		BotPOI poi;
		poi.name = t.raw(i, "name");
		if (poi.name.empty()) {
			t.fail(i, "name", "empty POI name");
		}
		if (!seen.insert({ townId, poi.name }).second) {
			t.fail(i, "name", fmt::format("duplicate POI ({}, '{}')", townId, poi.name));
		}
		poi.pos = Position(
			static_cast<uint16_t>(t.getInt(i, "pos_x", 0, 65535)),
			static_cast<uint16_t>(t.getInt(i, "pos_y", 0, 65535)),
			static_cast<uint8_t>(t.getInt(i, "pos_z", 0, 15)));
		const std::string typeStr = botCsvLower(botCsvTrim(t.raw(i, "poi_type")));
		if (typeStr == "depot") { poi.type = POIType::DEPOT; }
		else if (typeStr == "depot_outside") { poi.type = POIType::DEPOT_OUTSIDE; }
		else if (typeStr == "temple") { poi.type = POIType::TEMPLE; }
		else if (typeStr == "boat") { poi.type = POIType::BOAT; }
		else if (typeStr == "shop") { poi.type = POIType::SHOP; }
		else if (typeStr == "npc") { poi.type = POIType::NPC; }
		else if (typeStr == "adventurer_stone") { poi.type = POIType::ADVENTURER_STONE; }
		else {
			// The SQL loader's silent else -> DEPOT fallback dies here, same stroke as
			// parseWaypointType's NODE fallback.
			t.fail(i, "poi_type", fmt::format("unknown poi_type '{}'", typeStr));
		}
		out[townId].push_back(std::move(poi));
		count++;
	}
	if (!csvQuiet_) {
		g_logger().info("[BotEngine] Loaded {} POIs for {} towns", count, out.size());
	}
}

void BotEngine::loadCityRoutesCsv(std::unordered_map<uint32_t, CityRouteGraph>& outGraphs,
                                  std::vector<Waypoint>& outAdvStone) {
	const std::string dir = BOT_AUTHORED_DIR;
	const auto rt = BotCsvTable::load(dir + "/city_routes.csv",
		{ "town_id", "source_name", "enabled" }, {});

	struct RouteDef {
		uint32_t townId = 0;
		std::string sourceName;
		bool enabled = true;
		size_t defLine = 0;               // for error messages
		std::vector<Waypoint> wps;        // filled from the town file
		bool hasWps = false;
	};
	std::vector<RouteDef> defs;                       // file order == legacy DB id order
	std::map<uint32_t, std::vector<size_t>> byTown;   // townId -> defs indices, in file order
	std::set<std::pair<uint32_t, std::string>> identity;
	for (size_t i = 0; i < rt.rowCount(); i++) {
		RouteDef d;
		d.townId = static_cast<uint32_t>(rt.getInt(i, "town_id", 0, UINT32_MAX));
		d.sourceName = rt.raw(i, "source_name");
		if (d.sourceName.empty()) {
			rt.fail(i, "source_name", "empty source_name");
		}
		if (!identity.insert({ d.townId, d.sourceName }).second) {
			rt.fail(i, "source_name", fmt::format("duplicate route ({}, '{}')", d.townId, d.sourceName));
		}
		d.enabled = rt.getInt(i, "enabled", 0, 1) != 0;
		d.defLine = rt.sourceLine(i);
		byTown[d.townId].push_back(defs.size());
		defs.push_back(std::move(d));
	}

	// ---- per-town waypoint files: exactly one town_<id>.csv per distinct town_id ----
	for (const auto& [townId, defIdxs] : byTown) {
		const std::string path = fmt::format("{}/city_route_waypoints/town_{}.csv", dir, townId);
		const auto wt = BotCsvTable::load(path, // missing file for a listed town -> error
			{ "source_name", "waypoint_type", "pos_x", "pos_y", "pos_z" },
			{ "action_label" });
		// group rows by consecutive source_name; a name reappearing after a different
		// one means the file was hand-scrambled -> hard error
		std::unordered_map<std::string, size_t> defBySource;
		for (const size_t di : defIdxs) {
			defBySource[defs[di].sourceName] = di;
		}
		std::unordered_set<std::string> closedGroups;
		std::string current;
		RouteDef* def = nullptr;
		for (size_t i = 0; i < wt.rowCount(); i++) {
			const std::string sname = wt.raw(i, "source_name");
			if (sname != current) {
				if (!closedGroups.insert(sname).second) {
					wt.fail(i, "source_name", fmt::format(
						"non-consecutive waypoint group '{}' (its rows must be contiguous)", sname));
				}
				const auto it = defBySource.find(sname);
				if (it == defBySource.end()) {
					wt.fail(i, "source_name", fmt::format(
						"orphan waypoint group '{}' — no matching city_routes.csv row for town {}",
						sname, townId));
				}
				current = sname;
				def = &defs[it->second];
				def->hasWps = true;
			}
			const Position pos(
				static_cast<uint16_t>(wt.getInt(i, "pos_x", 0, 65535)),
				static_cast<uint16_t>(wt.getInt(i, "pos_y", 0, 65535)),
				static_cast<uint8_t>(wt.getInt(i, "pos_z", 0, 15)));
			const WaypointType wpType = botCsvWaypointType(wt, i, "waypoint_type");
			const std::string actionLabel = wt.raw(i, "action_label");
			uint16_t itemId = 0;
			// UNCHANGED from the SQL loader: USE_WITH tries stoi on action_label, 0 on failure.
			if (wpType == WaypointType::USE_WITH && !actionLabel.empty()) {
				try { itemId = static_cast<uint16_t>(std::stoi(actionLabel)); }
				catch (...) { itemId = 0; }
			}
			auto& newWp = def->wps.emplace_back(pos, wpType, itemId, actionLabel);
			newWp.isWalkOnFc = isWalkOnFcTile(pos);
		}
	}

	// ---- orphan town files (a file for a town with no routes is a stale edit) ----
	for (const auto& entry : std::filesystem::directory_iterator(dir + "/city_route_waypoints")) {
		const std::string fname = entry.path().filename().string();
		if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".csv") {
			continue; // .bak/.tmp generations
		}
		uint32_t townId = 0;
		if (fname.rfind("town_", 0) != 0) {
			throw BotCsvError { entry.path().string(), 0, 0, "route-waypoint filename must be town_<id>.csv" };
		}
		try { townId = static_cast<uint32_t>(std::stoul(fname.substr(5, fname.size() - 9))); }
		catch (...) {
			throw BotCsvError { entry.path().string(), 0, 0, "non-numeric town id in filename" };
		}
		if (!byTown.count(townId)) {
			throw BotCsvError { entry.path().string(), 0, 0,
				fmt::format("orphan town file — no city_routes.csv row has town_id {}", townId) };
		}
	}

	// ---- build graphs: two-pass POI extraction, semantics identical to the SQL loader
	// (source-side first-wins, then destination-side fill). PARITY: a route row with NO
	// waypoint group loads as nothing — today's INNER JOIN made ~20 such rows invisible
	// and the CSV loader must not "fix" that (guide §3.4). ----
	outGraphs.clear();
	outAdvStone.clear();
	struct ParsedRoute { uint32_t townId; std::string dst; std::vector<Waypoint>* wps; };
	std::vector<ParsedRoute> parsedRoutes;
	uint32_t totalPairs = 0;
	uint32_t skippedUnparsed = 0;  // source_name with no 'town|src~dst:' shape
	// Contested POI names whose routes disagree about the position by enough to matter.
	struct PoiConflict {
		uint32_t townId;
		std::string name;
		Position kept;
		Position other;
		uint32_t spread;
	};
	std::vector<PoiConflict> poiConflicts;
	for (auto& def : defs) {
		if (!def.enabled || !def.hasWps) {
			continue;
		}
		// town_id=0 sentinel = global/shared route. Only the Adventurer's Stone tour is
		// legal there.
		if (def.townId == 0) {
			if (def.sourceName.find("adventurer_stone") != std::string::npos) {
				outAdvStone = def.wps;
				if (!csvQuiet_) {
					g_logger().info("[BotEngine] Loaded global adventurer_stone route ({} waypoints)",
						outAdvStone.size());
				}
			} else {
				// STRICTNESS UPGRADE #1 (guide §2.5): the SQL loader WARNed and skipped an
				// unknown global route; that silence hid data errors. Zero live rows trip this.
				throw BotCsvError { fmt::format("{}/city_routes.csv", dir), def.defLine, 0,
					fmt::format("unknown global (town_id=0) route source_name '{}' — only "
						"adventurer_stone routes may be global", def.sourceName) };
			}
			continue;
		}
		// Parse "townname|source~destination:" -> src, dst (lowercased)
		const auto pipePos = def.sourceName.find('|');
		std::string afterPipe = pipePos == std::string::npos ? "" : def.sourceName.substr(pipePos + 1);
		if (!afterPipe.empty() && afterPipe.back() == ':') {
			afterPipe.pop_back();
		}
		const auto tildePos = afterPipe.find('~');
		if (pipePos == std::string::npos || tildePos == std::string::npos) {
			// PARITY, not a strictness upgrade. The plan asserted "zero live rows trip
			// this" — that was WRONG: 20 enabled routes (the depot|<town>|<n>: forms)
			// have no '~', which is exactly why 1831 route rows yield the 1810 pairs the
			// journal reports. The SQL loader `continue`s past them, so we skip too.
			// Throwing here would abort the whole load on good production data, and
			// "clean up the dead rows" is a behavior change that does not belong inside
			// a migration whose gate is a byte-identical dump.
			skippedUnparsed++;
			if (skippedUnparsed <= 3 && !csvQuiet_) {
				g_logger().warn("[BOT_CSV] city_routes.csv:{}: source_name '{}' is not "
					"'town|src~dst:' — route skipped (pre-existing; matches the SQL loader)",
					def.defLine, def.sourceName);
			}
			continue;
		}
		std::string src = botCsvLower(afterPipe.substr(0, tildePos));
		std::string dst = botCsvLower(afterPipe.substr(tildePos + 1));

		auto& graph = outGraphs[def.townId];
		graph.pairs[src][dst] = def.wps;
		totalPairs++;

		// Pass 1: POI position from the SOURCE side (first NODE/STAND waypoint, else
		// front()). File order decides, and file order is legacy id order.
		//
		// FIRST-WINS IS DELIBERATELY KEPT. The migration plan called for flipping this
		// to overwrite ("a changed POI position is silently never picked up"), but that
		// bug was about cityRouteGraphs_ never being cleared across reloads, and the
		// fresh-engine design already fixed it — every load rebuilds from scratch.
		//
		// What remains is that 143 (town, src) names are claimed by more than one route,
		// and the routes DISAGREE about where the POI is. Measured on live data: 27 agree
		// exactly, 25 are within 3 tiles, 67 within 10 — but 24 are further apart, the
		// worst being 'meriana.exit' in Liberty Bay at 148 tiles. Flipping to last-wins
		// would silently relocate ~116 POIs on no principle better than "the other one",
		// so it is not a fix. The actual defect is that the disagreement is invisible.
		// So: keep the deterministic winner, and REPORT the conflicts (below).
		if (!def.wps.empty()) {
			Position poiPos = def.wps.front().pos;
			for (const auto& wp : def.wps) {
				if (wp.type == WaypointType::NODE || wp.type == WaypointType::STAND) {
					poiPos = wp.pos;
					break;
				}
			}
			const auto existing = graph.pois.find(src);
			if (existing == graph.pois.end()) {
				graph.pois[src] = poiPos;
			} else {
				// Contested name. Only report a MEANINGFUL disagreement — a couple of
				// tiles is normal drift between two hand-recorded routes and would
				// drown the real cases in noise.
				const Position& kept = existing->second;
				const uint32_t spread = std::max(
					static_cast<uint32_t>(std::abs(static_cast<int>(kept.x) - static_cast<int>(poiPos.x))),
					static_cast<uint32_t>(std::abs(static_cast<int>(kept.y) - static_cast<int>(poiPos.y))));
				if (spread > 10 || kept.z != poiPos.z) {
					// One NAME can be contested by many routes (meriana.exit has six), so
					// collapse to the worst disagreement per (town, name) — otherwise the
					// count is per-route and the "worst offenders" list prints the same
					// POI five times, which is what the first version of this did.
					auto existingConflict = std::find_if(poiConflicts.begin(), poiConflicts.end(),
						[&](const PoiConflict& c) { return c.townId == def.townId && c.name == src; });
					if (existingConflict == poiConflicts.end()) {
						poiConflicts.push_back({ def.townId, src, kept, poiPos, spread });
					} else if (spread > existingConflict->spread) {
						existingConflict->other = poiPos;
						existingConflict->spread = spread;
					}
				}
			}
		}
		parsedRoutes.push_back({ def.townId, dst, &def.wps });
	}
	// Pass 2: fill MISSING POI positions from the DESTINATION side (last waypoint).
	for (const auto& pr : parsedRoutes) {
		if (pr.wps->empty()) {
			continue;
		}
		auto& graph = outGraphs[pr.townId];
		if (graph.pois.find(pr.dst) == graph.pois.end()) {
			graph.pois[pr.dst] = pr.wps->back().pos;
		}
	}
	if (!poiConflicts.empty() && !csvQuiet_) {
		std::sort(poiConflicts.begin(), poiConflicts.end(),
			[](const PoiConflict& a, const PoiConflict& b) { return a.spread > b.spread; });
		g_logger().warn("[BOT_CSV] {} contested route POI name(s) disagree about position by >10 "
			"tiles or across floors. The FIRST route in file order wins; the others are "
			"ignored. Worst offenders:", poiConflicts.size());
		for (size_t i = 0; i < poiConflicts.size() && i < 5; i++) {
			const auto& c = poiConflicts[i];
			g_logger().warn("[BOT_CSV]   town {} '{}' kept ({},{},{}) ignored ({},{},{}) spread={}",
				c.townId, c.name, c.kept.x, c.kept.y, c.kept.z,
				c.other.x, c.other.y, c.other.z, c.spread);
		}
	}
	if (skippedUnparsed > 0 && !csvQuiet_) {
		g_logger().warn("[BOT_CSV] {} city route(s) skipped for an unparseable source_name "
			"(pre-existing; this is why {} rows yield {} pairs)",
			skippedUnparsed, totalPairs + skippedUnparsed, totalPairs);
	}
	if (!csvQuiet_) {
		g_logger().info("[BotEngine] Loaded {} city route pairs across {} towns",
			totalPairs, outGraphs.size());
	}
}

void BotEngine::loadTravelPositionsCsv(
	std::unordered_map<uint32_t, std::vector<std::pair<Position, std::string>>>& outPositions,
	std::unordered_map<uint32_t, std::string>& outNames) {
	const std::string dir = BOT_AUTHORED_DIR;

	// town_mapping.csv first — the join target
	const auto mt = BotCsvTable::load(dir + "/town_mapping.csv",
		{ "source_name", "canary_town_id" }, {});
	std::unordered_map<std::string, uint32_t> mapping; // exact source_name -> town id
	for (size_t i = 0; i < mt.rowCount(); i++) {
		const std::string sname = mt.raw(i, "source_name");
		if (sname.empty()) {
			mt.fail(i, "source_name", "empty source_name");
		}
		if (!mapping.emplace(sname, static_cast<uint32_t>(mt.getInt(i, "canary_town_id", 1, UINT32_MAX))).second) {
			mt.fail(i, "source_name", fmt::format("duplicate mapping for '{}'", sname));
		}
	}

	const auto t = BotCsvTable::load(dir + "/travel_positions.csv",
		{ "source_name", "pos_x", "pos_y", "pos_z" }, {});
	outPositions.clear();
	outNames.clear();
	for (size_t i = 0; i < t.rowCount(); i++) {
		const std::string sourceName = t.raw(i, "source_name");
		const auto mapIt = mapping.find(sourceName);
		if (mapIt == mapping.end()) {
			// DELIBERATE LENIENT EXCEPTION (guide §2.5): the SQL loader's INNER JOIN
			// silently dropped unmapped rows (26 exist, 25 load). WARN-and-skip preserves
			// that semantics without blocking cold boot on legacy data cleanup. Exactly
			// ONE such line is expected in the journal until Milestone 2 cleans the row.
			if (!csvQuiet_) {
				g_logger().warn("[BOT_CSV] travel position '{}' has no town mapping — skipped", sourceName);
			}
			continue;
		}
		const uint32_t townId = mapIt->second;
		const Position pos(
			static_cast<uint16_t>(t.getInt(i, "pos_x", 0, 65535)),
			static_cast<uint16_t>(t.getInt(i, "pos_y", 0, 65535)),
			static_cast<uint8_t>(t.getInt(i, "pos_z", 0, 15)));

		// Route-POI derivation and canonical-row handling — UNCHANGED from the SQL loader.
		std::string routePOI = "boat";
		const std::string lowerSource = botCsvLower(sourceName);
		if (lowerSource.find("carpet") != std::string::npos) {
			routePOI = "carpet";
		}
		auto& positions = outPositions[townId];
		const auto town = g_game().map.towns.getTown(townId);
		const std::string townName = town ? town->getName() : "";
		const bool isCanonical = !townName.empty() && strcasecmp(sourceName.c_str(), townName.c_str()) == 0;
		if (isCanonical) {
			positions.insert(positions.begin(), { pos, routePOI });
			outNames[townId] = sourceName;
			g_logger().info("[BotEngine] Travel position for town {} = '{}' ({},{},{}) poi={}",
				townId, sourceName, pos.x, pos.y, pos.z, routePOI);
		} else {
			positions.push_back({ pos, routePOI });
			if (outNames.find(townId) == outNames.end()) {
				outNames[townId] = sourceName;
			}
			g_logger().info("[BotEngine] Travel position (alt) for town {} = '{}' ({},{},{}) poi={}",
				townId, sourceName, pos.x, pos.y, pos.z, routePOI);
		}
	}
	uint32_t total = 0;
	for (const auto& [id, positions] : outPositions) {
		total += positions.size();
	}
	if (!csvQuiet_) {
		g_logger().info("[BotEngine] Loaded {} travel positions for {} towns", total, outPositions.size());
	}
}

void BotEngine::loadEquipmentCsv(std::unordered_map<uint32_t, BotEquipment>& out) {
	const auto t = BotCsvTable::load(fmt::format("{}/equipment.csv", BOT_AUTHORED_DIR),
		{ "level", "vocation", "slot_head", "slot_armor", "slot_legs", "slot_feet",
		  "slot_right", "slot_left", "slot_backpack" }, {});
	out.clear();
	for (size_t i = 0; i < t.rowCount(); i++) {
		const uint32_t level = static_cast<uint32_t>(t.getInt(i, "level", 1, 100000));
		const uint32_t voc = static_cast<uint32_t>(t.getInt(i, "vocation", 0, 9));
		// Slots are OPTIONAL and default to 0. Parity: the SQL loader read these with
		// getNumber<uint16_t>(), which yields 0 for NULL, and 463 live rows have a NULL
		// slot_left (two-handed weapons carry no shield). Requiring a value here would
		// reject good production data — found by the offline validator, not by review.
		BotEquipment eq;
		eq.head = static_cast<uint16_t>(t.getIntOr(i, "slot_head", 0, 65535, 0));
		eq.armor = static_cast<uint16_t>(t.getIntOr(i, "slot_armor", 0, 65535, 0));
		eq.legs = static_cast<uint16_t>(t.getIntOr(i, "slot_legs", 0, 65535, 0));
		eq.feet = static_cast<uint16_t>(t.getIntOr(i, "slot_feet", 0, 65535, 0));
		eq.weapon = static_cast<uint16_t>(t.getIntOr(i, "slot_right", 0, 65535, 0)); // right = weapon hand
		eq.shield = static_cast<uint16_t>(t.getIntOr(i, "slot_left", 0, 65535, 0));  // left  = shield hand
		eq.backpack = static_cast<uint16_t>(t.getIntOr(i, "slot_backpack", 0, 65535, 0));
		if (!out.emplace(level * 10 + voc, eq).second) {
			t.fail(i, "level", fmt::format("duplicate equipment key (level {}, vocation {})", level, voc));
		}
	}
	if (!csvQuiet_) {
		g_logger().info("[BotEngine] Loaded {} equipment loadouts", out.size());
	}

	// One-shot forge-tier map sanity dump — VERBATIM MOVE of the block previously at the
	// tail of loadEquipmentData (bot_data.cpp:744-751). Content unchanged.
	g_logger().info("[BotEngine] Forge-tier map: "
		"lv100={} lv249={} lv250={} lv375={} lv500={} lv600={} lv700={} lv800={} lv900={} lv1050={} lv1200={}",
		botForgeTierForLevel(100), botForgeTierForLevel(249), botForgeTierForLevel(250),
		botForgeTierForLevel(375), botForgeTierForLevel(500), botForgeTierForLevel(600),
		botForgeTierForLevel(700), botForgeTierForLevel(800), botForgeTierForLevel(900),
		botForgeTierForLevel(1050), botForgeTierForLevel(1200));
}

// Everything derived from the world / live registries / config that must be rebuilt after
// authored data is (re)loaded. This is the tail of the pre-CSV loadHuntData, moved intact.
// Nothing here holds a raw pointer into the authored containers, and buildZPortalGraph is a
// cache hit whenever the map file is unchanged.
void BotEngine::rebuildDerivedTables() {
	// Reload drops every in-flight ice-fishing session; the waypoint data behind them was
	// just replaced.
	iceFishing_.clear();

	// BOT_NAV_REALISM Phase 8: derive NPC approach tiles from the live world. Must run AFTER
	// the POI/route swap (it reuses the town gate + temple positions).
	buildNpcApproachAnchors();

	// TRUE MULTI-FLOOR: whole-map portal graph for the cross-z route planner. Sector-cache
	// sweep, idempotent; emits [ZGRAPH] portals=N ... built in T ms.
	buildZPortalGraph();

	// Build dynamic spell tables from server registry + Lua file parsing
	buildSpellTables();

	// BOT_SUPPLY_REALISM: rune/ammo conjure spells, parsed from the shipped Lua the same way
	// the attack-rune tables are. Must run AFTER the spell registry is available.
	buildConjureTables();

	// Ambient support spells (haste/light/shield/heals/cures). Same ordering requirement as
	// buildConjureTables, and it additionally cross-references getHealSpells().
	buildSupportSpellTables();

	// BOT_HOUSE_VISIT: which houses belong to bots, by town. Rebuilt every load to pick up
	// ownership changes.
	buildBotHouseIndex();

	rebuildFidgetItemPool();

	// BOT_LIVENESS_PACK Phase C.2 + D: load chat phrase corpus.
	loadBotChatPhrases();
}

void BotEngine::rebuildFidgetItemPool() {
	// BOT_LIVENESS_PACK: build the fidget-drop item pool from NPC shops that trade in GOLD.
	//
	// ItemType.buyPrice is currency-BLIND: npcs.cpp loadShop stores the raw shop number
	// regardless of the NPC's currency (npcs.hpp:39 currencyId, default ITEM_GOLD_COIN=3031).
	// So token traders (e.g. Yana sells "of destruction" weapons for 50 GOLD TOKENS) would
	// otherwise inject expensive items into the "<1000 gp" pool. We therefore walk the NPC
	// registry directly and only accept items sold by a GOLD-currency NPC for
	// 0 < buyPrice < threshold that are normal carryable items (pickupable + movable;
	// containers + stackables allowed). General, not a hardcoded list — any non-gold
	// currency (silver/gold token, future platinum/crystal shops) is auto-excluded.
	// The shop walk is iterative + depth-capped against malformed datapack childShop nesting.
	// NpcTypes register at g_npcs().load() (before Game::start) and persist across reload.
	
	const uint32_t maxValue = static_cast<uint32_t>(g_configManager().getNumber(BOT_FIDGET_MAX_ITEM_VALUE_GP));
	safeFidgetItemIds_.clear();
	std::unordered_set<uint16_t> seen;
	for (const auto& [npcName, npcType] : g_npcs().getNpcTypes()) {
		if (!npcType || npcType->getCurrencyId() != ITEM_GOLD_COIN) {
			continue;  // skip token / non-gold traders — their prices aren't in gp
		}
		// Iterative walk of the shop tree (top-level blocks + nested childShop menus).
		std::vector<std::pair<const std::vector<ShopBlock>*, int>> stack;
		stack.emplace_back(&npcType->getShopItems(), 0);
		while (!stack.empty()) {
			const auto* blocks = stack.back().first;
			const int depth = stack.back().second;
			stack.pop_back();
			if (depth > 8) {
				continue;  // pathological nesting guard
			}
			for (const auto& sb : *blocks) {
				if (sb.itemBuyPrice > 0 && sb.itemBuyPrice < maxValue) {
					const ItemType& it = Item::items.getItemType(sb.itemId);
					if (it.id != 0 && it.pickupable && it.movable
					    && seen.insert(sb.itemId).second) {
						safeFidgetItemIds_.push_back(sb.itemId);
					}
				}
				if (!sb.childShop.empty()) {
					stack.emplace_back(&sb.childShop, depth + 1);
				}
			}
		}
	}
	g_logger().info("[BotEngine] Loaded {} gold-NPC-buyable fidget item IDs (max buy price {} gp)",
		safeFidgetItemIds_.size(), maxValue);
}

void BotEngine::buildTravelDestinationsCache() {
	// Build all-to-all travel destination cache. Sources = all towns with travel_positions.
	// Destinations = same set, FILTERED to towns that have ANY route ending at "depot"
	// OR "temple" (matches the live arrival fallback chain in doTraveling phase 3 at
	// line ~16498-16501, which tries arrivalPOI→depot, arrivalPOI→temple, ""→depot,
	// ""→temple — the empty "" triggers findBestRouteSource auto-detection of any
	// source POI). So Farmine with only carpet→temple, or a town with only
	// shop→depot, both qualify.
	// Runs AFTER the commit in reloadBotData(), so cityRouteGraphs_ and travelPositions_
	// are both the freshly-swapped-in containers. (Pre-CSV this relied on loadCityRoutes
	// calling loadTravelPositions from its tail; the swap now commits both together.)
	auto townHasArrivalRoute = [this](uint32_t townId) -> bool {
		auto it = cityRouteGraphs_.find(townId);
		if (it == cityRouteGraphs_.end()) return false;
		for (const auto& srcAndDsts : it->second.pairs) {
			const auto& dstMap = srcAndDsts.second;
			if (dstMap.count("depot") || dstMap.count("temple")) return true;
		}
		return false;
	};
	std::vector<uint32_t> allSources;
	std::vector<uint32_t> validDests;
	allSources.reserve(travelPositions_.size());
	validDests.reserve(travelPositions_.size());
	for (const auto& tp : travelPositions_) {
		uint32_t id = tp.first;
		allSources.push_back(id);
		if (townHasArrivalRoute(id)) validDests.push_back(id);
	}
	travelDestinationsCache_.clear();
	travelDestinationsCache_.reserve(allSources.size());
	for (uint32_t src : allSources) {
		auto& dsts = travelDestinationsCache_[src];
		dsts.reserve(validDests.size());
		for (uint32_t dst : validDests) {
			if (dst != src) dsts.push_back(dst);
		}
	}
	g_logger().info("[BotEngine] Built all-to-all travel destinations: {} sources × {} reachable destinations ({} towns pruned for missing boat→depot/carpet→depot route)",
		allSources.size(), validDests.size(), allSources.size() - validDests.size());
}


// Phase 8 town gate — defined further down with the other graph helpers; declared here because
// buildNpcApproachAnchors (immediately below) is compiled before that definition.
static bool botNavGraphEnabledForTown(uint32_t townId);



// ---- BOT_NAV_REALISM Phase 8 increment 1: NPC approach anchors ----
// Runtime construction, no schema and no offline builder (AzerothCore/CMaNGOS generate their
// TravelNode graph in-engine the same way). For every live NPC in a rolled-out town, precompute
// the tiles a bot may stand on to interact with it: walkable, Chebyshev <= 3 (talk range), same z,
// not creature-occupied, never a floor-change/teleport tile, and with clear line of sight to the
// NPC. That LOS check is what solves "the shopkeeper is behind a counter" — the tiles in front of
// the counter pass, the unreachable ones behind it don't.
// Nothing consumes this map yet (increment 2 adds the resolver); building it is inert.
void BotEngine::buildNpcApproachAnchors() {
	npcAnchors_.clear();
	npcNamesByTown_.clear(); // rebuilt below; a stale entry would hand out a dead NPC name
	if (g_configManager().getString(BOT_NAV_GRAPH_TOWNS).empty()) {
		return; // feature gate off — don't pay the build cost at all
	}

	constexpr int32_t APPROACH_RADIUS = 3;   // Tibia talk range
	constexpr size_t MAX_APPROACH_TILES = 6; // K
	uint32_t npcsSeen = 0, npcsAnchored = 0, npcsNoTile = 0, tilesKept = 0;

	for (const auto& npc : g_game().getNpcs()) {
		if (!npc || npc->isRemoved()) {
			continue;
		}
		npcsSeen++;
		const Position npcPos = npc->getPosition();

		// Assign a town by nearest temple (same z-weighted idiom as detectNearestPOI).
		uint32_t bestTown = 0;
		int32_t bestDist = INT32_MAX;
		for (const auto& [townId, town] : g_game().map.towns.getTowns()) {
			if (!town) {
				continue;
			}
			const auto tpos = town->getTemplePosition();
			const int32_t d = std::abs(static_cast<int32_t>(npcPos.x) - static_cast<int32_t>(tpos.x))
				+ std::abs(static_cast<int32_t>(npcPos.y) - static_cast<int32_t>(tpos.y))
				+ std::abs(static_cast<int32_t>(npcPos.z) - static_cast<int32_t>(tpos.z)) * 10;
			if (d < bestDist) {
				bestDist = d;
				bestTown = townId;
			}
		}
		if (bestTown == 0 || !botNavGraphEnabledForTown(bestTown)) {
			continue; // bounds the build to towns already rolled out
		}

		// Scan the 7x7 around the NPC for legal standing tiles.
		struct Cand { Position pos; int32_t dist; };
		std::vector<Cand> cands;
		for (int32_t dx = -APPROACH_RADIUS; dx <= APPROACH_RADIUS; dx++) {
			for (int32_t dy = -APPROACH_RADIUS; dy <= APPROACH_RADIUS; dy++) {
				if (dx == 0 && dy == 0) {
					continue; // the NPC's own tile
				}
				Position cand(static_cast<uint16_t>(static_cast<int32_t>(npcPos.x) + dx),
				              static_cast<uint16_t>(static_cast<int32_t>(npcPos.y) + dy),
				              npcPos.z);
				const auto& tile = g_game().map.getTile(cand);
				if (!tile || !tile->getGround()) {
					continue;
				}
				if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) {
					continue;
				}
				if (isWalkOnFcTile(cand)) {
					continue; // never park a bot on stairs/a teleport
				}
				// LOS to the NPC — this is what rejects tiles behind a counter/wall.
				if (!g_game().map.isSightClear(cand, npcPos, true)) {
					continue;
				}
				cands.push_back({ cand, std::max(std::abs(dx), std::abs(dy)) });
			}
		}
		if (cands.empty()) {
			npcsNoTile++;
			continue; // status quo for this NPC — nothing computes approach tiles today anyway
		}
		std::sort(cands.begin(), cands.end(),
			[](const Cand& a, const Cand& b) { return a.dist < b.dist; });

		NpcAnchor anchor;
		anchor.npcPos = npcPos;
		anchor.townId = bestTown;
		for (size_t i = 0; i < cands.size() && i < MAX_APPROACH_TILES; i++) {
			anchor.approachTiles.push_back(cands[i].pos);
			tilesKept++;
		}
		npcAnchors_[npc->getName()].push_back(std::move(anchor));
		npcNamesByTown_[bestTown].push_back(npc->getName());
		npcsAnchored++;
	}

	g_logger().info("[NPC_ANCHORS] npcsSeen={} anchored={} noLegalTile={} tilesKept={} names={} towns={}",
		npcsSeen, npcsAnchored, npcsNoTile, tilesKept, npcAnchors_.size(), npcNamesByTown_.size());
}

// ---- Phase 8 increment 2: approach-tile selection with cross-tick reservations ----
// Reservations are SOFT and TTL'd (not per-tick like the lane set) because walking to a counter
// spans many ticks. Two bots therefore never target the same tile in front of a shopkeeper, and a
// crashed/aborted walk self-heals when the TTL lapses.

void BotEngine::releaseNpcApproach(uint32_t guid) {
	std::erase_if(s_approachReservations, [guid](const auto& kv) { return kv.second.guid == guid; });
}

bool BotEngine::resolveNpcApproach(const BotState& bot, const std::string& npcName,
                                   Position& outTile, bool& outIsFallback) {
	outIsFallback = false;
	auto it = npcAnchors_.find(npcName);
	if (it == npcAnchors_.end() || it->second.empty()) {
		return false;
	}
	// Same-named NPCs exist in several towns — take the instance nearest this bot.
	const NpcAnchor* best = nullptr;
	int32_t bestDist = INT32_MAX;
	for (const auto& a : it->second) {
		const int32_t d = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(a.npcPos.x))
			+ std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(a.npcPos.y))
			+ std::abs(static_cast<int32_t>(bot.currentPos.z) - static_cast<int32_t>(a.npcPos.z)) * 10;
		if (d < bestDist) {
			bestDist = d;
			best = &a;
		}
	}
	if (!best) {
		return false;
	}

	const int64_t now = OTSYS_TIME();
	auto claim = [&](const Position& p) {
		s_approachReservations[botTileKey(p)] = ApproachReservation { bot.guid, now + APPROACH_RESERVE_MS };
		outTile = p;
	};
	auto takeable = [&](const Position& p) -> bool {
		auto rit = s_approachReservations.find(botTileKey(p));
		if (rit != s_approachReservations.end() && rit->second.expiresAt > now
		    && rit->second.guid != bot.guid) {
			return false; // another bot is walking to this tile
		}
		const auto& tile = g_game().map.getTile(p);
		if (!tile) {
			return false;
		}
		const auto& occupant = tile->getTopVisibleCreature(nullptr);
		return occupant == nullptr; // someone already standing there
	};

	// Preferred: a precomputed approach tile (in talk range, LOS to the NPC), nearest first.
	for (const auto& p : best->approachTiles) {
		if (takeable(p)) {
			claim(p);
			return true;
		}
	}

	// Overflow — every proper tile is taken. Walk up to the nearest free tile within Chebyshev 5
	// even without line of sight and wait there, like a player queueing at a busy counter.
	// Deliberately never teleports.
	s_approachOverflow++;
	outIsFallback = true;
	const Position& npcPos = best->npcPos;
	for (int32_t r = 1; r <= 5; r++) {
		for (int32_t dx = -r; dx <= r; dx++) {
			for (int32_t dy = -r; dy <= r; dy++) {
				if (std::max(std::abs(dx), std::abs(dy)) != r) {
					continue; // ring only
				}
				Position p(static_cast<uint16_t>(static_cast<int32_t>(npcPos.x) + dx),
				           static_cast<uint16_t>(static_cast<int32_t>(npcPos.y) + dy), npcPos.z);
				const auto& tile = g_game().map.getTile(p);
				if (!tile || !tile->getGround()) {
					continue;
				}
				if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) {
					continue;
				}
				if (isWalkOnFcTile(p) || !takeable(p)) {
					continue;
				}
				claim(p);
				return true;
			}
		}
	}
	return false; // fully boxed in — caller keeps its existing behavior
}

std::pair<Position, std::string> BotEngine::getTravelPosition(uint32_t townId) const {
	auto it = travelPositions_.find(townId);
	if (it != travelPositions_.end() && !it->second.empty()) {
		// Pick a random entry (e.g. 50/50 boat vs carpet)
		return it->second[uniform_random(0, static_cast<int32_t>(it->second.size()) - 1)];
	}
	return {Position(), "boat"};
}

// Forward declaration — defined alongside applyBotForgeTiers below; used by the
// one-shot sanity dump at the end of loadEquipmentData.

void BotEngine::equipBot(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	auto voc = player->getVocation();
	uint32_t baseVoc = voc ? voc->getBaseId() : 4; // default to paladin if unknown
	uint32_t level = std::min(static_cast<uint32_t>(player->getLevel()), 500u);
	uint32_t key = level * 10 + baseVoc;
	auto it = equipmentData_.find(key);
	if (it == equipmentData_.end()) {
		g_logger().warn("[BotEngine] EQUIP: No equipment data for '{}' (lv={}, baseVoc={}, key={})",
			player->getName(), level, baseVoc, key);
		return;
	}

	const auto& eq = it->second;

	struct SlotDef { Slots_t slot; uint16_t itemId; const char* name; };
	SlotDef slots[] = {
		{CONST_SLOT_HEAD, eq.head, "head"},
		{CONST_SLOT_ARMOR, eq.armor, "armor"},
		{CONST_SLOT_LEGS, eq.legs, "legs"},
		{CONST_SLOT_FEET, eq.feet, "feet"},
		{CONST_SLOT_LEFT, eq.weapon, "weapon"},      // weapon in left hand
		{CONST_SLOT_RIGHT, eq.shield, "shield"},     // shield in right hand
		{CONST_SLOT_BACKPACK, eq.backpack, "backpack"},
	};

	int updated = 0;
	for (auto& s : slots) {
		if (s.itemId == 0) continue;
		auto existing = player->getInventoryItem(s.slot);
		if (existing && existing->getID() == s.itemId) continue; // already correct

		// Remove existing item in slot
		if (existing) {
			g_game().internalRemoveItem(existing);
		}

		// Create and assign new item
		auto item = Item::CreateItem(s.itemId, 1);
		if (item) {
			player->addThing(static_cast<int32_t>(s.slot), item);
			// Verify it was actually placed
			auto verify = player->getInventoryItem(s.slot);
			if (verify && verify->getID() == s.itemId) {
				updated++;
			} else {
				g_logger().warn("[BotEngine] EQUIP: addThing failed for '{}' slot={} item={}",
					player->getName(), s.name, s.itemId);
			}
		} else {
			g_logger().warn("[BotEngine] EQUIP: CreateItem failed for '{}' slot={} item={}",
				player->getName(), s.name, s.itemId);
		}
	}

	if (updated > 0) {
		g_logger().info("[BotEngine] Equipped {} items for '{}' (lv={}, baseVoc={})",
			updated, player->getName(), level, baseVoc);
	}

	// Universal dwarven ring (active form, 3099) — all bots, every voc/level. The engine's
	// ItemTransformationMap force-normalizes CreateItem(3099) to the de-equip form 3097, so
	// we create it, place it in the slot, then transform IN PLACE to the active 3099
	// (Game::transformItem uses setID for same-type items — no revert, the same path real
	// players take on equip). Its 1h decay is then frozen permanently by the bot guard in
	// Decay::startDecay (timer never hits 0 / vanishes). Idempotent: 3099 is a stable fixed
	// point (no transformEquipTo) so re-activation skips and reload won't ping-pong it.
	{
		auto ringSlot = player->getInventoryItem(CONST_SLOT_RING);
		if (!ringSlot || ringSlot->getID() != ITEM_DWARVEN_RING_ACTIVATED) {
			if (ringSlot) {
				g_game().internalRemoveItem(ringSlot);
			}
			auto ring = Item::CreateItem(ITEM_DWARVEN_RING_ACTIVATED, 1); // normalized to 3097
			if (ring) {
				player->addThing(static_cast<int32_t>(CONST_SLOT_RING), ring);
				auto placed = player->getInventoryItem(CONST_SLOT_RING);
				if (placed && placed->getID() != ITEM_DWARVEN_RING_ACTIVATED) {
					g_game().transformItem(placed, ITEM_DWARVEN_RING_ACTIVATED); // in-place -> active form
				}
				auto verify = player->getInventoryItem(CONST_SLOT_RING);
				if (!verify || verify->getID() != ITEM_DWARVEN_RING_ACTIVATED) {
					g_logger().warn("[BotEngine] RING: dwarven ring failed to equip for '{}' (got {})",
						player->getName(), verify ? verify->getID() : 0);
				}
			}
		}
	}

	// Load ammo into quiver if present and empty
	auto shieldItem = player->getInventoryItem(CONST_SLOT_RIGHT);
	if (shieldItem && shieldItem->isQuiver()) {
		auto container = shieldItem->getContainer();
		if (container && container->empty()) {
			auto ammo = Item::CreateItem(35901, 100); // 100x diamond arrow (non-decaying)
			if (ammo) {
				g_game().internalAddItem(container, ammo, INDEX_WHEREEVER);
				g_logger().info("[BotEngine] Added 100 diamond arrows to quiver for '{}'", player->getName());
			}
		}
	}

	// Wipe the backpack to clear accumulated junk (quest items like Ewer with Holy
	// Water from Lion's Rock, looted clutter, etc.) so the seed blocks below produce
	// a deterministic loadout every activation. Bots use bank balance for shop
	// purchases (set to 100000 in bot_players_setup.sql), so destroying the bag's
	// gold/loot is fine. Runes re-seed immediately below (empty() guard now satisfied),
	// exercise weapons re-seed at max 14400 charges via the per-id findItemOfType guard.
	// Only fires from activateBot (line 2194) and the reload-reactivate path (line 4309).
	// NOT from wakeBot — hibernation preserves the Player object as-is via the
	// hibernationPool_ fast-path, so the bag persists across proximity/teleport wakes.
	{
		auto bpExisting = player->getInventoryItem(CONST_SLOT_BACKPACK);
		if (bpExisting) {
			g_game().internalRemoveItem(bpExisting);  // destroys container + all sub-items
		}
		auto fresh = Item::CreateItem(2854, 1);  // standard backpack (items.xml:5931)
		if (fresh) {
			player->addThing(static_cast<int32_t>(CONST_SLOT_BACKPACK), fresh);
			// Refresh the cast-viewer openContainers[0] entry — the old backpack's
			// shared_ptr held there was just invalidated. See player.cpp:12544
			// (setCastBroadcasting) for the broadcast-enable side of the same hook.
			if (player->isCastBroadcasting()) {
				const auto &freshBp = player->getBackpack();
				if (freshBp) {
					player->addContainer(0, freshBp);
				}
			}
		}
	}

	// Load runes into backpack if present and empty
	auto bpItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	if (bpItem) {
		auto bpContainer = bpItem->getContainer();
		if (bpContainer && bpContainer->empty()) {
			// Add 1 of each rune — charges never consumed for bots (isBotPlayer check in spells.cpp)
			// 3180 = magic wall rune (BOT_PVP_REALISM: level 32 + ML 9 gate enforced at cast time).
			// 3165 = paralyze rune (gang-PK: ED-only, level 54 + ML 18, fired at a fleeing victim).
			static const uint16_t runeIds[] = {3161, 3191, 3155, 3180, 3165}; // avalanche, great fireball, sudden death, magic wall, paralyze
			for (uint16_t runeId : runeIds) {
				auto rune = Item::CreateItem(runeId, 1);
				if (rune) {
					g_game().internalAddItem(bpContainer, rune, INDEX_WHEREEVER);
				}
			}
			g_logger().info("[BotEngine] Added runes (ava/gfb/sd/mwall/para) to backpack for '{}'", player->getName());
		}

		// Ensure-present (idempotent): backpacks stocked before the paralyze rune existed are
		// skipped by the empty() guard above — top them up so every bot has a paralyze rune
		// for the gang-PK ED behavior. Per-id check mirrors the Lasting Exercise pattern.
		if (bpContainer && g_game().findItemOfType(player, 3165, true, -1) == nullptr) {
			auto para = Item::CreateItem(3165, 1);
			if (para) {
				g_game().internalAddItem(bpContainer, para, INDEX_WHEREEVER);
			}
		}

		// Lasting Exercise weapons for Adv Stone Mode 2 dwell. Per-id idempotent so we
		// never duplicate on re-activation. 14400 charges = ~8h of continuous training;
		// daily server restart re-creates them at max.
		if (bpContainer) {
			int added = 0;
			for (uint16_t weaponId : kLastingExerciseIds) {
				if (g_game().findItemOfType(player, weaponId, true, -1) != nullptr) continue;
				auto weapon = Item::CreateItem(weaponId, 1);
				if (!weapon) continue;
				weapon->setAttribute(ItemAttribute_t::CHARGES, static_cast<int64_t>(kLastingExerciseCharges));
				if (g_game().internalAddItem(bpContainer, weapon, INDEX_WHEREEVER) == RETURNVALUE_NOERROR) {
					++added;
				}
			}
			if (added > 0) {
				g_logger().info("[BotEngine] Added {} Lasting Exercise weapons to '{}'", added, player->getName());
			}
		}

		// ---- BOT_SUPPLY_REALISM: nested supply bag ----
		//
		// Why NESTED and not just more items at top level: the main backpack (2854) holds exactly
		// 20 (items.xml containersize), and this bot already carries 13 seeded items (5 runes +
		// 8 Lasting Exercise weapons). Seven more would fill it exactly, leaving nothing for what
		// the new behaviours PRODUCE — and an overflowing addItem falls through to
		// internalAddItem(tile, ..., FLAG_NOLIMIT), which bypasses the client stack cap and piles
		// items on one sqm forever. The bag costs 1 top-level slot and frees six for output.
		//
		// It holds INPUTS ONLY. Pre-seeding output stacks in here would be inert: queryDestination
		// (player.cpp) dequeues the MAIN backpack first, scans only its direct item list, and
		// takes its free-slot fallback before the nested bag is ever reached — so produced items
		// land at top level regardless. What actually prevents litter is spare slots, and the
		// arithmetic works out: sorcerer/druid conjured runes stack onto the five rune stacks
		// already at top level (zero new slots), leaving fish, flasks and paladin ammo — 3-4
		// types against 6 free, each then stacking to 100.
		//
		// No idempotency guard by id: this whole block runs after the unconditional backpack
		// wipe. Do NOT "fix" that with findItemOfType(player, 2854, ...) — it would match the
		// OUTER backpack, since both are the same item.
		if (bpContainer) {
			auto supplyBag = Item::CreateItem(2854, 1);
			if (supplyBag && g_game().internalAddItem(bpContainer, supplyBag, INDEX_WHEREEVER) == RETURNVALUE_NOERROR) {
				if (const auto& bag = supplyBag->getContainer()) {
					auto seed = [&](uint16_t id, uint16_t count) {
						if (auto item = Item::CreateItem(id, count)) {
							g_game().internalAddItem(bag, item, INDEX_WHEREEVER);
						}
					};
					// Fishing: rod + one worm. Both are infinite for bots — the worm is never
					// consumed thanks to the isBotPlayer guard in fishing.lua, and its presence is
					// also what makes that script's addSkillTries branch fire.
					seed(3483, 1);   // fishing rod
					seed(3492, 1);   // worm
					// Conjuring: blank rune, likewise never consumed (guard in register_spells.lua).
					seed(3147, 1);   // blank rune
					// Food. Never consumed (guard in foods.lua). Loops the SAME array the picker
					// uses, because two hand-maintained lists silently drifted: the picker rolled
					// cheese/meat that were never seeded, and tryEatFood re-arms its 60s interval
					// before checking inventory, so half of all attempts burned the window doing
					// nothing. The nested bag has ample room, so seeding all of them is free.
					for (uint16_t foodId : kBotFoods) {
						seed(foodId, 1);
					}
					// Potions for this bot's vocation/level. Never consumed (guard in potions.lua),
					// but the empty flask IS created for real and stacks at top level.
					// Mirrors the gating in potions.lua so a bot never carries one it cannot drink.
					const uint32_t lvl = player->getLevel();
					if (baseVoc == 4) {                       // knight
						seed(lvl >= 200 ? 23375 : lvl >= 130 ? 7643 : lvl >= 80 ? 239 : lvl >= 50 ? 236 : 266, 1);
						seed(lvl >= 50 ? 237 : 268, 1);
					} else if (baseVoc == 3) {                // paladin
						seed(lvl >= 130 ? 23374 : lvl >= 80 ? 7642 : lvl >= 50 ? 236 : 266, 1);
						seed(lvl >= 80 ? 238 : lvl >= 50 ? 237 : 268, 1);
					} else {                                  // sorcerer / druid
						seed(266, 1);
						seed(lvl >= 130 ? 23373 : lvl >= 80 ? 238 : lvl >= 50 ? 237 : 268, 1);
					}
					// The bot's signature conjure output, pre-seeded so repeated casts merge into
					// one stack instead of taking a fresh slot each time. Sorc/druid picks are
					// usually already at top level; paladins need theirs here.
					if (auto idx = guidToIndex_.find(player->getGUID()); idx != guidToIndex_.end()) {
						if (const auto* cs = signatureConjure(bots_[idx->second])) {
							if (g_game().findItemOfType(player, cs->conjureId, true, -1) == nullptr) {
								seed(cs->conjureId, 1);
							}
						}
					}
				}
			}
			// Free-slot + capacity assertion. A silent overflow is exactly what this seeding is
			// designed to prevent, so make the margin visible rather than assumed. Capacity is a
			// mechanically DIFFERENT failure mode from slots — Player::queryAdd rejects on weight
			// with open slots, and bots carry no HasInfiniteCapacity flag.
			const size_t topFree = bpContainer->capacity() > bpContainer->size()
				? bpContainer->capacity() - bpContainer->size() : 0;
			if (topFree < 3) {
				g_logger().warn("[BotSupply] '{}' has only {} free top-level slot(s) after seeding — "
				                "produced items may spill to the ground", player->getName(), topFree);
			}
		}
	}

	// Apply permanent tier-3 ("Powerful") imbuements to bots level >= 50.
	// Idempotent — skips items whose imbuement slots are already populated.
	if (player->getLevel() >= 50) {
		applyBotImbuements(player, baseVoc);
	}

	// Apply level-scaled forge tier to weapon/head/armor/legs/feet.
	// No-op for bots under level 150 (tier 0) and for non-classifiable items.
	applyBotForgeTiers(player);
}

// Apply tier-3 ("Powerful") imbuements to all imbuable slots on a bot's equipped gear.
// Priority per slot: Critical (Strike III) -> Life Leech (Vampirism III) -> vocation-appropriate.
// Honors items.xml category/tier acceptance (hasImbuementType) and prevents duplicate categories.
// Uses duration 0xFFFFFF (~194 days of in-combat decay ticks — effectively permanent for any
// plausible server uptime). Skips ImbuementDecay registration so the 1Hz scheduler stays idle
// for these effectively-permanent items. Stat add/remove on item swap is handled by the existing
// postRemove/postAddNotification -> onPlayerDe/Equip MoveEvent chain (movement.cpp:559,660).
void BotEngine::applyBotImbuements(const std::shared_ptr<Player>& player, uint32_t baseVoc) {
	// Tier-3 imbuement IDs are discovered once per .so load via a static cache.
	// imbuements.xml currently has 69 sequential entries (1..69). Cap at 69 to avoid
	// the off-by-one "Imbuement 70 not found" warning from getImbuement.
	// Map: category id -> imbuement id (tier 3). First match wins (cat 0 = elemental damage
	// has 5 entries but we never use it as a priority, so the ambiguity is harmless).
	static const std::unordered_map<uint16_t, uint16_t> tier3IdByCategory = [] {
		std::unordered_map<uint16_t, uint16_t> out;
		for (uint16_t id = 1; id <= 69; ++id) {
			Imbuement* imb = g_imbuements().getImbuement(id);
			if (!imb) continue;
			if (imb->getBaseID() != 3) continue;
			uint16_t catId = imb->getCategory();
			if (out.find(catId) == out.end()) {
				out[catId] = id;
			}
		}
		g_logger().info("[BotEngine] Discovered {} tier-3 imbuement categories", out.size());
		return out;
	}();
	if (tier3IdByCategory.empty()) {
		return;
	}

	// Category constants (mirror data/XML/imbuements.xml <category> ids).
	constexpr uint16_t CAT_ELEMENTAL_DAMAGE = 0;
	constexpr uint16_t CAT_LIFE_LEECH       = 1;
	constexpr uint16_t CAT_MANA_LEECH       = 2;
	constexpr uint16_t CAT_CRITICAL         = 3;
	constexpr uint16_t CAT_PROT_DEATH       = 4;
	constexpr uint16_t CAT_PROT_EARTH       = 5;
	constexpr uint16_t CAT_PROT_FIRE        = 6;
	constexpr uint16_t CAT_PROT_ICE         = 7;
	constexpr uint16_t CAT_PROT_ENERGY      = 8;
	constexpr uint16_t CAT_PROT_HOLY        = 9;
	constexpr uint16_t CAT_SPEED            = 10;
	constexpr uint16_t CAT_SKILL_AXE        = 11;
	constexpr uint16_t CAT_SKILL_SWORD      = 12;
	constexpr uint16_t CAT_SKILL_CLUB       = 13;
	constexpr uint16_t CAT_SKILL_SHIELD     = 14;
	constexpr uint16_t CAT_SKILL_DISTANCE   = 15;
	constexpr uint16_t CAT_SKILL_MAGIC      = 16;
	constexpr uint16_t CAT_SKILL_FIST       = 18;
	(void)CAT_ELEMENTAL_DAMAGE; (void)CAT_SKILL_FIST;

	// All-protections list — used as armor/legs/shield fallback after lifeleech/critical.
	const std::vector<uint16_t> ALL_PROTECTIONS = {
		CAT_PROT_DEATH, CAT_PROT_EARTH, CAT_PROT_FIRE, CAT_PROT_ICE, CAT_PROT_ENERGY, CAT_PROT_HOLY
	};

	auto buildSlotPriority = [&](uint32_t voc, Slots_t slot) -> std::vector<uint16_t> {
		const bool isKnight  = (voc == 4);
		const bool isPaladin = (voc == 3);
		const bool isMage    = (voc == 1 || voc == 2);

		std::vector<uint16_t> p;
		auto pushHead = [&]() { p.push_back(CAT_CRITICAL); p.push_back(CAT_LIFE_LEECH); };

		if (slot == CONST_SLOT_LEFT) { // weapon
			pushHead();
			if (isKnight) {
				p.push_back(CAT_SKILL_SWORD);
				p.push_back(CAT_SKILL_AXE);
				p.push_back(CAT_SKILL_CLUB);
				p.push_back(CAT_MANA_LEECH);
			} else if (isPaladin) {
				p.push_back(CAT_SKILL_DISTANCE);
				p.push_back(CAT_MANA_LEECH);
			} else if (isMage) {
				p.push_back(CAT_MANA_LEECH);
				p.push_back(CAT_SKILL_MAGIC);
			}
		} else if (slot == CONST_SLOT_HEAD) {
			pushHead();
			if (isKnight) {
				p.push_back(CAT_SKILL_SWORD);
				p.push_back(CAT_SKILL_AXE);
				p.push_back(CAT_SKILL_CLUB);
				p.push_back(CAT_SKILL_SHIELD);
				p.push_back(CAT_MANA_LEECH);
			} else if (isPaladin) {
				p.push_back(CAT_SKILL_DISTANCE);
				p.push_back(CAT_MANA_LEECH);
			} else if (isMage) {
				p.push_back(CAT_MANA_LEECH);
				p.push_back(CAT_SKILL_MAGIC);
			}
		} else if (slot == CONST_SLOT_ARMOR || slot == CONST_SLOT_LEGS) {
			pushHead();
			for (auto c : ALL_PROTECTIONS) p.push_back(c);
		} else if (slot == CONST_SLOT_FEET) {
			pushHead();
			p.push_back(CAT_SPEED);
		} else if (slot == CONST_SLOT_RIGHT) { // shield
			pushHead();
			p.push_back(CAT_SKILL_SHIELD);
			for (auto c : ALL_PROTECTIONS) p.push_back(c);
		}
		return p;
	};

	const Slots_t inventorySlots[] = {
		CONST_SLOT_HEAD, CONST_SLOT_ARMOR, CONST_SLOT_LEGS,
		CONST_SLOT_FEET, CONST_SLOT_LEFT, CONST_SLOT_RIGHT,
	};

	constexpr uint32_t BOT_IMBUE_DURATION = 0xFFFFFF;
	uint32_t totalApplied = 0;
	for (auto slot : inventorySlots) {
		auto item = player->getInventoryItem(slot);
		if (!item) continue;
		const uint8_t slotCount = item->getImbuementSlot();
		if (slotCount == 0) continue;

		const auto priorities = buildSlotPriority(baseVoc, slot);
		if (priorities.empty()) continue;

		for (uint8_t subIdx = 0; subIdx < slotCount; ++subIdx) {
			ImbuementInfo existing;
			if (item->getImbuementInfo(subIdx, &existing) && existing.imbuement) {
				continue; // slot already populated (persisted across reload)
			}
			for (uint16_t catId : priorities) {
				auto idIt = tier3IdByCategory.find(catId);
				if (idIt == tier3IdByCategory.end()) continue;
				const uint16_t imbId = idIt->second;
				if (item->hasImbuementCategoryId(catId)) continue;
				if (!item->hasImbuementType(static_cast<ImbuementTypes_t>(catId), 3)) continue;
				Imbuement* imb = g_imbuements().getImbuement(imbId);
				if (!imb) continue;
				if (item->getParent() == player) {
					player->addItemImbuementStats(imb);
				}
				item->setImbuement(subIdx, imbId, BOT_IMBUE_DURATION);
				++totalApplied;
				break;
			}
		}
	}

	if (totalApplied > 0) {
		g_logger().info("[BotEngine] Applied {} powerful imbuements to '{}' (lv={}, baseVoc={})",
			totalApplied, player->getName(), player->getLevel(), baseVoc);
	}
}

// Apply level-scaled forge tier to a bot's equipped gear so Fatal/Momentum/Ruse/
// Transcendence/Amplification chance formulas return non-zero.
// Slot mapping (verified in combat.cpp:2502 + player.cpp:10378/10461/12057 + multipliers at FEET):
//   LEFT  -> Fatal (Onslaught)
//   HEAD  -> Momentum
//   ARMOR -> Ruse (Dodge)
//   LEGS  -> Transcendence
//   FEET  -> Amplification (multiplies the other four chances)
// setTier is a silent no-op on items with upgradeClassification == 0 (non-tierable), but we
// guard explicitly to keep the log line accurate.
// Map a bot level to a realistic forge tier (capped at 6 — tier 7-10 gear is
// implausible for simulated players). Bands mirror the level distribution in
// tools/bot_population_generator/generate.py LEVEL_TIERS (keep in sync):
//   level         forge tier   band intent
//   <100          0            8-100   (newbie, no forge)
//   100-249       1            100-250
//   250-374       2 \ 250-500 band -> 2-3, split at 375
//   375-499       3 /
//   500-599       3 \ 500-700 band -> 3-4, split at 600
//   600-699       4 /
//   700-799       4 \ 700-900 band -> 4-5, split at 800
//   800-899       5 /
//   900-1049      5 \ 900+ bands  -> 5-6, split at 1050
//   >=1050        6 /
// Strict `<` cutoffs throughout so exact boundary levels (100/250/500/700/900)
// land in the higher band, matching the generator's tier column.
static uint8_t botForgeTierForLevel(uint32_t lv) {
	if (lv < 100)  return 0;
	if (lv < 250)  return 1;
	if (lv < 375)  return 2;
	if (lv < 600)  return 3;  // 375-499 (2-3 band) + 500-599 (3-4 band) both -> 3
	if (lv < 800)  return 4;  // 600-699 (3-4 band) + 700-799 (4-5 band) both -> 4
	if (lv < 1050) return 5;  // 800-899 (4-5 band) + 900-1049 (5-6 band) both -> 5
	return 6;
}

void BotEngine::applyBotForgeTiers(const std::shared_ptr<Player>& player) {
	const uint32_t lv = player->getLevel();
	const uint8_t tier = botForgeTierForLevel(lv);
	if (tier == 0) return;

	static const Slots_t tierSlots[] = {
		CONST_SLOT_LEFT,
		CONST_SLOT_HEAD,
		CONST_SLOT_ARMOR,
		CONST_SLOT_LEGS,
		CONST_SLOT_FEET,
	};
	uint32_t applied = 0;
	for (auto slot : tierSlots) {
		auto item = player->getInventoryItem(slot);
		if (!item) continue;
		if (item->getClassification() == 0) continue;
		if (item->getTier() == tier) continue;
		item->setTier(tier);
		++applied;
	}
	if (applied > 0) {
		g_logger().info("[BotEngine] Applied tier {} to {} slot(s) for '{}' (lv={})",
			tier, applied, player->getName(), lv);
	}
}

std::string BotEngine::detectNearestPOI(uint32_t townId, const Position& pos) const {
	auto it = cityRouteGraphs_.find(townId);
	if (it == cityRouteGraphs_.end()) return "";

	std::string best;
	int32_t bestDist = 999;
	for (auto& [name, p] : it->second.pois) {
		int32_t d = std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(p.x))
			+ std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(p.y))
			+ std::abs(static_cast<int32_t>(pos.z) - static_cast<int32_t>(p.z)) * 10;
		if (d < bestDist) {
			bestDist = d;
			best = name;
		}
	}
	return best;
}

// Phase 8: reverse-reachability. Returns every POI that can reach `dst` within maxHops by
// chaining authored routes. Used so findBestRouteSource can pick a source whose path to the
// destination is a multi-hop CHAIN, not just a single authored pair — otherwise the auto-detect
// path (srcPOI empty, which is the common case) bails before multi-hop routing is ever consulted.
// One reverse BFS instead of a forward BFS per candidate; town graphs are tiny (~5-15 POIs).
static std::unordered_set<std::string> botPoisReachingDst(const CityRouteGraph& graph,
                                                          const std::string& dst, int maxHops) {
	std::unordered_set<std::string> reaching;
	std::vector<std::string> frontier { dst };
	for (int hop = 0; hop < maxHops && !frontier.empty(); hop++) {
		std::vector<std::string> next;
		for (const auto& srcAndDsts : graph.pairs) {
			const std::string& cand = srcAndDsts.first;
			if (reaching.count(cand) || cand == dst) {
				continue;
			}
			for (const auto& target : frontier) {
				if (srcAndDsts.second.find(target) != srcAndDsts.second.end()) {
					reaching.insert(cand);
					next.push_back(cand);
					break;
				}
			}
		}
		frontier.swap(next);
	}
	return reaching;
}

std::string BotEngine::findBestRouteSource(uint32_t townId, const Position& pos,
	const std::string& dstPOI, const std::unordered_set<std::string>& excluded) const {
	auto it = cityRouteGraphs_.find(townId);
	if (it == cityRouteGraphs_.end()) return "";

	struct Candidate { std::string name; int32_t dist; };
	std::vector<Candidate> candidates;

	for (const auto& [srcName, dstMap] : it->second.pairs) {
		if (srcName == dstPOI) continue;         // skip self-routes
		if (excluded.count(srcName)) continue;    // skip already-tried sources
		if (dstMap.find(dstPOI) == dstMap.end()) continue; // no route to destination

		auto poiIt = it->second.pois.find(srcName);
		if (poiIt == it->second.pois.end()) continue;
		int32_t d = std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(poiIt->second.x))
			+ std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(poiIt->second.y))
			+ std::abs(static_cast<int32_t>(pos.z) - static_cast<int32_t>(poiIt->second.z)) * 10;
		candidates.push_back({ srcName, d });
	}

	// BOT_NAV_REALISM Phase 8: no POI has a DIRECT authored route to the destination. Before
	// giving up (which sends the caller to a teleport fallback), retry allowing multi-hop chains:
	// accept any source that can reach dstPOI by chaining authored routes. loadCityRouteCore then
	// stitches the legs together.
	if (candidates.empty() && botNavGraphEnabledForTown(townId)) {
		const auto reaching = botPoisReachingDst(it->second, dstPOI, 3);
		for (const auto& srcName : reaching) {
			if (srcName == dstPOI || excluded.count(srcName)) continue;
			auto poiIt = it->second.pois.find(srcName);
			if (poiIt == it->second.pois.end()) continue;
			int32_t d = std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(poiIt->second.x))
				+ std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(poiIt->second.y))
				+ std::abs(static_cast<int32_t>(pos.z) - static_cast<int32_t>(poiIt->second.z)) * 10;
			candidates.push_back({ srcName, d });
		}
	}

	if (candidates.empty()) return "";
	std::sort(candidates.begin(), candidates.end(),
		[](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });

	return candidates[0].name;
}

const std::vector<Waypoint>* BotEngine::findCityRoute(uint32_t townId, const std::string& src, const std::string& dst) const {
	auto it = cityRouteGraphs_.find(townId);
	if (it == cityRouteGraphs_.end()) return nullptr;

	// Direct route
	auto srcIt = it->second.pairs.find(src);
	if (srcIt != it->second.pairs.end()) {
		auto dstIt = srcIt->second.find(dst);
		if (dstIt != srcIt->second.end()) {
			return &dstIt->second;
		}
	}

	return nullptr;
}

// Player-free core. Looks up the route in cityRouteGraphs_, copies waypoints into
// bot.cityRouteWps, sets followingCityRoute. Mirrors the source-resolution and
// destination-tracking semantics of the live path; the only thing the live wrapper
// adds is castLog (which requires a Player).
// ---- BOT_NAV_REALISM Phase 8: multi-hop city routing over the existing pair data ----
// `bot_city_routes` stores POINT-TO-POINT pairs, so a bot could only walk src->dst when that exact
// pair had been authored; every other request fell through to a teleport fallback. This treats the
// stored pairs as EDGES of a per-town graph and BFS-searches for a chain (src -> X -> dst),
// concatenating the legs — the playerbots TravelNode idea applied to data we already own, with no
// new schema and no offline builder. Legs join exactly at the shared POI tile, so the concatenation
// is geometrically continuous and each leg is already a validated, walkable route.
static bool botNavGraphEnabledForTown(uint32_t townId) {
	const std::string cfg = g_configManager().getString(BOT_NAV_GRAPH_TOWNS);
	if (cfg.empty()) {
		return false;
	}
	if (cfg == "all") {
		return true;
	}
	// csv of town ids
	const std::string needle = std::to_string(townId);
	size_t pos = 0;
	while (pos < cfg.size()) {
		size_t comma = cfg.find(',', pos);
		std::string tok = cfg.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		// trim spaces
		while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
		while (!tok.empty() && tok.back() == ' ') tok.pop_back();
		if (tok == needle) {
			return true;
		}
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
	return false;
}

// BOT_NAV_REALISM Phase 9 (junction branching): `botSeed` randomises the order in which each
// node's neighbours are expanded. BFS still returns a SHORTEST chain, but when several chains tie
// on hop count, different bots discover different ones — so bots heading to the same destination
// don't all funnel through the same intermediate POI. This is the city half of "junction
// branching": equal-cost alternatives sampled per bot, rather than a fixed map iteration order.
static bool findCityRouteMultiHop(const CityRouteGraph& graph, const std::string& src,
                                  const std::string& dst, std::vector<Waypoint>& out,
                                  int maxHops, size_t maxWaypoints, int& hopsOut,
                                  uint32_t botSeed = 0, std::vector<std::string>* chainOut = nullptr) {
	if (src.empty() || dst.empty() || src == dst) {
		return false;
	}
	if (graph.pairs.find(src) == graph.pairs.end()) {
		return false;
	}
	// BFS over POI names (edges are authored routes). Shortest hop-count wins, which keeps the
	// concatenated walk as short as the data allows.
	std::unordered_map<std::string, std::string> prev;
	std::unordered_map<std::string, int> depth;
	std::vector<std::string> queue;
	size_t head = 0;
	queue.push_back(src);
	depth[src] = 0;
	bool found = false;
	while (head < queue.size() && !found) {
		const std::string cur = queue[head++];
		const int curDepth = depth[cur];
		if (curDepth >= maxHops) {
			continue;
		}
		auto it = graph.pairs.find(cur);
		if (it == graph.pairs.end()) {
			continue;
		}
		// Phase 9: expand neighbours in a per-bot order so tied-length chains differ per bot.
		std::vector<const std::string*> neighbours;
		neighbours.reserve(it->second.size());
		for (const auto& nextPair : it->second) {
			neighbours.push_back(&nextPair.first);
		}
		if (botSeed != 0 && neighbours.size() > 1) {
			// Deterministic per-bot shuffle (same bot always resolves a tie the same way, so its
			// route is stable across ticks; different bots diverge).
			uint32_t h = botSeed ^ static_cast<uint32_t>(curDepth * 0x9E3779B1u);
			for (size_t i = neighbours.size(); i > 1; i--) {
				h ^= h << 13; h ^= h >> 17; h ^= h << 5; // xorshift32
				std::swap(neighbours[i - 1], neighbours[h % i]);
			}
		}
		for (const std::string* nextPtr : neighbours) {
			const std::string& next = *nextPtr;
			if (depth.find(next) != depth.end()) {
				continue; // already reached at an equal-or-shorter depth
			}
			depth[next] = curDepth + 1;
			prev[next] = cur;
			if (next == dst) {
				found = true;
				break;
			}
			queue.push_back(next);
		}
	}
	if (!found) {
		return false;
	}
	// Rebuild the POI chain dst <- ... <- src.
	std::vector<std::string> chain;
	for (std::string n = dst;;) {
		chain.push_back(n);
		auto p = prev.find(n);
		if (p == prev.end()) {
			break;
		}
		n = p->second;
	}
	std::reverse(chain.begin(), chain.end());
	if (chain.size() < 3) {
		return false; // that's a direct pair — the caller already tried it
	}
	out.clear();
	for (size_t i = 0; i + 1 < chain.size(); i++) {
		auto legIt = graph.pairs.find(chain[i]);
		if (legIt == graph.pairs.end()) {
			return false;
		}
		auto wpIt = legIt->second.find(chain[i + 1]);
		if (wpIt == legIt->second.end()) {
			return false;
		}
		for (const auto& w : wpIt->second) {
			out.push_back(w);
			if (out.size() > maxWaypoints) {
				return false; // absurdly long chain — let the caller fall back
			}
		}
	}
	hopsOut = static_cast<int>(chain.size()) - 1;
	if (chainOut) {
		*chainOut = chain;
	}
	return !out.empty();
}


// Resolve src -> dst exactly as loadCityRouteCore does: the authored direct pair first, then the
// multi-hop chain fallback. Factored out so the `/cavebot splice` audit sees the SAME waypoint list
// the runtime will act on rather than an approximation of it — a diagnostic that models the walker
// only approximately is how `/cavebot route`'s parity traps got written in the first place.
//
// Returns a pointer to the graph's own vector for a direct pair, or to `scratch` for a chain, or
// nullptr if neither exists. `chainKey` receives "src>dst" or "src>mid>dst": the identity of the
// resolved route, which the splice cache keys on. It must include the whole chain because
// findCityRouteMultiHop shuffles neighbour expansion per botSeed, so two bots can legitimately
// resolve the same src->dst to different chains.
const std::vector<Waypoint>* BotEngine::resolveCityRoute(uint32_t townId, const std::string& src,
                                                         const std::string& dst,
                                                         std::vector<Waypoint>& scratch,
                                                         std::string& chainKey,
                                                         uint32_t botSeed) const {
	chainKey.clear();
	if (const auto* direct = findCityRoute(townId, src, dst)) {
		chainKey = fmt::format("{}>{}", src, dst);
		return direct;
	}
	if (!botNavGraphEnabledForTown(townId)) {
		return nullptr;
	}
	auto graphIt = cityRouteGraphs_.find(townId);
	if (graphIt == cityRouteGraphs_.end()) {
		return nullptr;
	}
	int hops = 0;
	std::vector<std::string> chain;
	if (!findCityRouteMultiHop(graphIt->second, src, dst, scratch, 3, 400, hops, botSeed, &chain)) {
		return nullptr;
	}
	for (const auto& poi : chain) {
		if (!chainKey.empty()) {
			chainKey += ">";
		}
		chainKey += poi;
	}
	return &scratch;
}


// BOT_ROUTE_SPLICE. Return the detour-spliced version of `original`, or `original` itself when the
// route has nothing to splice. Never mutates the graph's own vector — that is load-bearing: the
// `/cavebot csv` dump reads cityRouteGraphs_ directly, and the authored-CSV migration's parity gate
// is byte-identical output. The spliced copy lives here instead.
//
// `key` is the route's IDENTITY, "src>dst" for an authored pair or "src>mid>dst" for a chain. The
// chain has to be in there: findCityRouteMultiHop shuffles neighbour expansion per botSeed, so two
// bots can legitimately resolve the same src->dst to different chains with different splices.
//
// Two maps, not one. A miss in `routeSpliceCache_` is ambiguous on its own — it means either "no
// splice" or "not evaluated yet" — and for lazily-evaluated chains those differ. `routeSpliceClean_`
// records "evaluated, nothing to do", so an unspliceable chain pays its gates once rather than on
// every route start.
const std::vector<Waypoint>* BotEngine::spliceRouteCached(uint32_t townId, const std::string& key,
                                                          const std::vector<Waypoint>& original) {
	const std::string full = fmt::format("{}|{}", townId, key);
	auto hit = routeSpliceCache_.find(full);
	if (hit != routeSpliceCache_.end()) {
		return &hit->second;
	}
	if (routeSpliceClean_.count(full)) {
		return &original;
	}
	std::vector<Waypoint> copy = original;
	std::vector<std::string> log;
	const size_t cut = spliceRouteDetours(copy, full, &log);
	if (cut == 0) {
		routeSpliceClean_.insert(full);
		return &original;
	}
	s_routeSpliceRoutes++;
	s_routeSpliceWpsSaved += static_cast<uint32_t>(cut);
	for (const auto& line : log) {
		g_logger().info("[SPLICE] {}", line);
	}
	auto ins = routeSpliceCache_.emplace(full, std::move(copy));
	return &ins.first->second;
}

// Precompute every authored direct pair at load, so the runtime never pays a gate on a tick and the
// journal carries one auditable inventory per boot. Runs in reloadBotData's derived-builder step,
// beside buildZPortalGraph — which sweeps the map in the same function, so the map is provably
// loaded here.
//
// Chains are deliberately NOT precomputed: they are per-bot randomised and cannot be enumerated.
// They splice lazily through the same spliceRouteCached and land in the same two maps.

// ============================================================================
// BOT_TRAVEL_ARRIVE_MIX — where a bot goes when it lands in a destination town.
//
// Before this, every arrival walked to the depot; the temple was reached only when no depot route
// resolved, and shops never at all. Bots did reach temples and shops eventually, but as a LATER
// idle reroll, so the moment of stepping off the boat always looked identical.
// ============================================================================

namespace {

// The shop-class route endpoints, as they are actually spelled in the authored data.
// `trainer` (singular) is NOT a typo to be normalised away — Yalahar authors it that way
// (`yalahar|depot~trainer:`) while every other town uses `trainers`. Both are the same kind of
// place, so both belong in this set; leaving the singular out would have quietly filed Yalahar's
// trainers under "other".
const std::unordered_set<std::string>& botArriveShopNames() {
	static const std::unordered_set<std::string> names = {
		"ammo", "bank", "food", "loot", "potions", "runes", "tools", "trainer", "trainers",
	};
	return names;
}

bool botArriveIsShop(const std::string& poi) {
	if (botArriveShopNames().count(poi)) {
		return true;
	}
	// The loot traders are authored per-NPC: loot.yanni, loot.oiriz, loot.habdel, loot.azil,
	// loot.morpel, loot.romella.
	return poi.rfind("loot.", 0) == 0;
}

// Endpoints that are neither shops nor somewhere we want to send a landing bot.
bool botArriveIsExcluded(const std::string& poi) {
	if (poi.empty()) {
		return true;
	}
	// Core arrival/egress POIs are handled by their own buckets, or are where the bot already is.
	if (poi == "depot" || poi == "temple" || poi == "boat" || poi == "carpet") {
		return true;
	}
	// `depot.2` (Venore) is a second depot. It is a perfectly good place, but filing it under
	// "other" would let an "other" roll produce a depot arrival and blur the measured split.
	if (poi.rfind("depot", 0) == 0) {
		return true;
	}
	// `exit-potions` (Edron) is a route EGRESS marker, not a destination anyone travels to.
	if (poi.rfind("exit-", 0) == 0) {
		return true;
	}
	// Defensive: the loader skips malformed `town|n:` rows before they ever become endpoints
	// (bot_data.cpp, the "is not 'town|src~dst:'" branch), so this matches nothing today. Kept
	// because the cost is one scan and the failure it prevents is a bot routed to a non-place.
	return poi.find('|') != std::string::npos || poi.find(' ') != std::string::npos;
}

} // namespace

// Build, per town, the set of POIs a landing bot could be sent to. BFS from the arrival POIs
// (`boat`, `carpet`) over the authored pair graph to depth 3 — the SAME bound
// findCityRouteMultiHop applies from loadCityRouteCore, so this index can never promise a target
// the router will then refuse to deliver.
void BotEngine::buildTravelArriveTargets() {
	travelArriveTargets_.clear();
	size_t townsWithShops = 0;
	size_t townsWithOthers = 0;
	for (const auto& [townId, graph] : cityRouteGraphs_) {
		std::unordered_set<std::string> seen;
		std::vector<std::string> frontier;
		for (const char* arrival : { "boat", "carpet" }) {
			if (graph.pairs.count(arrival)) {
				seen.insert(arrival);
				frontier.emplace_back(arrival);
			}
		}
		for (int hop = 0; hop < 3 && !frontier.empty(); hop++) {
			std::vector<std::string> next;
			for (const auto& node : frontier) {
				auto it = graph.pairs.find(node);
				if (it == graph.pairs.end()) {
					continue;
				}
				for (const auto& [dst, wps] : it->second) {
					if (seen.insert(dst).second) {
						next.push_back(dst);
					}
				}
			}
			frontier.swap(next);
		}
		TravelArriveTargets t;
		for (const auto& poi : seen) {
			if (botArriveIsExcluded(poi)) {
				continue;
			}
			if (botArriveIsShop(poi)) {
				t.shops.push_back(poi);
			} else {
				t.others.push_back(poi);
			}
		}
		// Deterministic order so a given config produces a reproducible split across restarts;
		// the per-arrival pick is random, the list is not.
		std::sort(t.shops.begin(), t.shops.end());
		std::sort(t.others.begin(), t.others.end());
		if (!t.shops.empty()) {
			townsWithShops++;
		}
		if (!t.others.empty()) {
			townsWithOthers++;
		}
		travelArriveTargets_[townId] = std::move(t);
	}
	g_logger().info("[ARRIVE] travel arrival index: {} town(s), {} with shops, {} with other POIs",
		travelArriveTargets_.size(), townsWithShops, townsWithOthers);
}

// Roll where this journey should end. Returns a concrete POI name; never empty.
//
// Weight walk rather than four independent percentage gates. That matches how TABLE A and TABLE B
// are consumed elsewhere in this engine, and it means an operator who edits one number without
// rebalancing the others gets a proportionally shifted split instead of a silent cliff.
// Bump the realised-split counter for a resolved arrival. Classifying here rather than at the
// two call sites keeps the live handler and the virtual twin counting the same way.
void BotEngine::noteTravelArrivalClass(const std::string& poi) {
	if (poi == "depot") {
		s_arriveDepotCount++;
	} else if (poi == "temple") {
		s_arriveTempleCount++;
	} else if (botArriveIsShop(poi)) {
		s_arriveShopCount++;
	} else {
		s_arriveOtherCount++;
	}
}

std::string BotEngine::pickTravelArrivalTarget(const BotState& bot, uint32_t destTownId) const {
	// HUNT-BOUND: forced to the depot. Arrival hands straight to HuntPhase::PREPARING, whose first
	// act is a depot route and whose third is depot->potions. Rolling a shop here would walk the
	// bot across town only to walk it back, which reads as a bug, not as realism — and the
	// "restock before hunting" behaviour this feature might seem to add already exists inside
	// PREPARING.
	if (bot.pendingHuntAfterTravel) {
		return "depot";
	}
	// PARTY ASSEMBLY: same reasoning, different clock. A member travelling to an assembly anchor
	// holds everyone else at the barrier until it arrives, and arrival is only recognised once the
	// arrival route completes. A rolled shop lengthens assembly for the whole party.
	if (s_rvMember.count(bot.guid)) {
		return "depot";
	}

	auto& cm = g_configManager();
	const int32_t wDepot = std::max<int32_t>(0, static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_DEPOT_PCT)));
	const int32_t wTemple = std::max<int32_t>(0, static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_TEMPLE_PCT)));
	const int32_t wShop = std::max<int32_t>(0, static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_SHOP_PCT)));
	const int32_t wOther = std::max<int32_t>(0, static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_OTHER_PCT)));
	const int32_t total = wDepot + wTemple + wShop + wOther;
	if (total <= 0) {
		return "depot"; // all weights zeroed — uniform_random(1,0) is UB, and depot is the safe pick
	}

	auto idx = travelArriveTargets_.find(destTownId);
	const TravelArriveTargets* targets = (idx != travelArriveTargets_.end()) ? &idx->second : nullptr;

	int32_t roll = uniform_random(1, total);
	if ((roll -= wDepot) <= 0) {
		return "depot";
	}
	if ((roll -= wTemple) <= 0) {
		return "temple";
	}
	if ((roll -= wShop) <= 0) {
		if (targets && !targets->shops.empty()) {
			return targets->shops[uniform_random(0, static_cast<int32_t>(targets->shops.size()) - 1)];
		}
		return "depot"; // no shop authored in this town — 9 of 18 are like this
	}
	if (targets && !targets->others.empty()) {
		return targets->others[uniform_random(0, static_cast<int32_t>(targets->others.size()) - 1)];
	}
	return "depot";
}

void BotEngine::spliceCityRoutesAtLoad() {
	const int64_t start = OTSYS_TIME();
	size_t routes = 0;
	for (const auto& [townId, graph] : cityRouteGraphs_) {
		for (const auto& [src, dstMap] : graph.pairs) {
			for (const auto& [dst, wps] : dstMap) {
				routes++;
				spliceRouteCached(townId, fmt::format("{}>{}", src, dst), wps);
			}
		}
	}
	g_logger().info("[SPLICE] precomputed {} authored route(s) in {}ms — {} spliced, {} waypoint(s) "
		"removed, {} left unchanged",
		routes, OTSYS_TIME() - start, s_routeSpliceRoutes, s_routeSpliceWpsSaved,
		routeSpliceClean_.size());
}

bool BotEngine::loadCityRouteCore(BotState& bot, const std::string& srcPOI, const std::string& dstPOI) {
	// Clear tried sources if destination changed
	if (bot.lastRouteDestination != dstPOI) {
		bot.triedRouteSources.clear();
		bot.lastRouteDestination = dstPOI;
	}

	// Determine source: explicit or smart auto-detect (finds closest untried POI with a route to dstPOI)
	std::string src = srcPOI.empty()
		? findBestRouteSource(bot.townId, bot.currentPos, dstPOI, bot.triedRouteSources)
		: srcPOI;
	if (src.empty() || src == dstPOI) return false;

	const auto* route = findCityRoute(bot.townId, src, dstPOI);

	// BOT_NAV_REALISM Phase 8: no direct pair authored — try a multi-hop chain through the pair
	// graph (src -> X -> dst) before giving up and letting the caller teleport. `multiHop` is a
	// local, but its contents are copied into bot.cityRouteWps immediately below.
	std::vector<Waypoint> multiHop;
	// BOT_ROUTE_SPLICE: the route's identity, "src>dst" for an authored pair, "src>mid>dst" for a
	// chain. The splice cache keys on it and the chain must be in there — tie-breaks are shuffled
	// per bot, so two bots can resolve the same src->dst to different chains.
	std::string spliceKey = fmt::format("{}>{}", src, dstPOI);
	if (!route && botNavGraphEnabledForTown(bot.townId)) {
		auto graphIt = cityRouteGraphs_.find(bot.townId);
		if (graphIt != cityRouteGraphs_.end()) {
			int hops = 0;
			std::vector<std::string> chain;
			// Phase 9: pass the bot's nav seed so equal-length chains are sampled per bot.
			if (findCityRouteMultiHop(graphIt->second, src, dstPOI, multiHop, 3, 400, hops,
					botNavSeed(bot.guid), &chain)) {
				spliceKey.clear();
				for (const auto& poi : chain) {
					if (!spliceKey.empty()) {
						spliceKey += ">";
					}
					spliceKey += poi;
				}
				s_graphMultiHopCount++;
				s_graphHopSum += static_cast<uint32_t>(hops);
				castLog(bot, fmt::format("ROUTE: multi-hop {} -> {} via {} legs ({} waypoints)",
					src, dstPOI, hops, multiHop.size()));
				route = &multiHop;
			}
		}
	}
	if (!route) return false;

	// BOT_ROUTE_SPLICE. Authored city routes are frequently a naive concatenation of two legs
	// through the temple -- farmine|carpet~depot IS carpet~temple + temple~depot -- so the bot
	// walks down into the temple and straight back out mid-journey. Substitute the spliced list,
	// which is precomputed for authored pairs and computed once on first use for chains. Returns
	// `route` unchanged when there is nothing to splice, which is the common case.
	route = spliceRouteCached(bot.townId, spliceKey, *route);

	bot.cityRouteWps = *route;
	bot.cityRouteIdx = 0;
	bot.followingCityRoute = true;
	s_lastRouteSource[bot.guid] = src;
	s_routeWpTimer.erase(bot.guid);
	s_routeProgress.erase(bot.guid);
	return true;
}

bool BotEngine::startCityRoute(BotState& bot, const std::string& srcPOI, const std::string& dstPOI) {
	if (!loadCityRouteCore(bot, srcPOI, dstPOI)) return false;
	// New route entry — refill the safeTeleportLanding rewind allowance.
	resetTpRewindBudget(bot.guid);
	castLog(bot, fmt::format("ROUTE: {} -> {} ({} waypoints)",
		s_lastRouteSource[bot.guid], dstPOI, bot.cityRouteWps.size()));
	return true;
}

// ============================================================================
// Static data — POIs, city walk Z, boat positions, doors, spells
// ============================================================================

const std::unordered_map<uint32_t, std::vector<BotPOI>>& BotEngine::getCityPOIs() {
	return cityPOIs_;
}

const std::unordered_map<uint32_t, uint8_t>& BotEngine::getCityWalkZ() {
	static const std::unordered_map<uint32_t, uint8_t> walkZ = {
		{5, 7}, {6, 7}, {7, 11}, {8, 7}, {9, 7}, {10, 7}, {11, 8}, {12, 10},
		{13, 7}, {14, 7}, {15, 7}, {16, 7}, {17, 7}, {18, 9}, {19, 8}, {20, 6},
		{21, 6}, {22, 5}, {24, 7}, {25, 7}, {26, 7}, {27, 14}, {28, 7}, {29, 7},
		{30, 7}, {31, 5},
	};
	return walkZ;
}

// Random travel destination graph — which cities each town's boat connects to
const std::unordered_map<uint32_t, std::vector<uint32_t>>& BotEngine::getTravelDestinations() {
	// Returns the cache built once at the end of loadTravelPositions(). Each town can
	// travel directly to every other town that has a valid boat→depot or carpet→depot
	// city route (validated at build time). The legacy hardcoded neighbor allowlist
	// (Roshamuul → {Rathleton} only, etc.) is gone — bots virtualize transit, so any
	// (src, dest) pair is allowed as long as the destination has a walk_from_boat route.
	// O(1) lookup, same call-site semantics as before.
	return travelDestinationsCache_;
}

// KEY-LOCKED door IDs (the KeyDoorTable "locked doors" block below). A bot has no key
// inventory logic anywhere in this codebase, so it can NEVER open these — useItem just fails,
// the door gets cooldown-blacklisted, and the leg fails permanently.
//
// They are still in getDoorTable() (it is the openId lookup, used when a door IS openable), so
// callers that need "can a bot get through here?" must consult this set instead. Notably the
// navdump: it flagged every door tile NAV_DOOR, and the offline simulator then modelled all of
// them as walkable-at-cost-60 — which is why zcheck reported 0 violations on routes the live
// bot could not walk. Verified live: a 4912 sits 2 tiles from Captain Bluebear's approach tile.
bool BotEngine::isKeyLockedDoorId(uint16_t id) {
	static const std::unordered_set<uint16_t> locked = {
		1628, 1631, 1650, 1653, 1668, 1671, 1682, 1691, 4912, 4913, 5097, 5106,
		5115, 5124, 5133, 5136, 5139, 5142, 5277, 5280, 5732, 5735, 6191, 6194,
		6248, 6251, 6799, 6801, 6891, 6900, 7033, 7042, 7711, 7714, 8249, 8252,
		8351, 8354, 9351, 9354, 9551, 9560, 9858, 9867, 11136, 11143, 11232,
		11241, 13135, 17560, 17569, 17700, 17709, 17993, 18002, 20444, 20453,
		23873, 23875, 28364, 28366, 30772, 30774, 37982, 37984, 44914, 44916
	};
	return locked.count(id) > 0;
}

const std::unordered_map<uint16_t, uint16_t>& BotEngine::getDoorTable() {
	// Complete door table generated from data/libs/tables/doors.lua
	// Includes ALL door types: key, custom, quest, and level doors
	static const std::unordered_map<uint16_t, uint16_t> doors = {
		// KeyDoorTable — locked doors (lockedDoor -> openDoor)
		{1628, 1630}, {1631, 1633}, {1650, 1652}, {1653, 1655},
		{1668, 1670}, {1671, 1673}, {1682, 1684}, {1691, 1693},
		{4912, 4911}, {4913, 4914}, {5097, 5099}, {5106, 5108},
		{5115, 5117}, {5124, 5126}, {5133, 5135}, {5136, 5138},
		{5139, 5141}, {5142, 5144}, {5277, 5279}, {5280, 5282},
		{5732, 5734}, {5735, 5737}, {6191, 6193}, {6194, 6196},
		{6248, 6250}, {6251, 6253}, {6799, 6796}, {6801, 6798},
		{6891, 6893}, {6900, 6902}, {7033, 7035}, {7042, 7044},
		{7711, 7713}, {7714, 7716}, {8249, 8251}, {8252, 8254},
		{8351, 8353}, {8354, 8356}, {9351, 9353}, {9354, 9356},
		{9551, 9553}, {9560, 9562}, {9858, 9860}, {9867, 9869},
		{11136, 11138}, {11143, 11145}, {11232, 11234}, {11241, 11243},
		{13135, 13137}, {17560, 17562}, {17569, 17571}, {17700, 17702},
		{17709, 17711}, {17993, 17995}, {18002, 18004}, {20444, 20445},
		{20453, 20454}, {23873, 23877}, {23875, 23878}, {28364, 28368},
		{28366, 28369}, {30772, 30776}, {30774, 30777}, {37982, 37985},
		{37984, 37986}, {44914, 44917}, {44916, 44918},
		// KeyDoorTable — closed doors (closedDoor -> openDoor)
		{1629, 1630}, {1632, 1633}, {1651, 1652}, {1654, 1655},
		{1669, 1670}, {1672, 1673}, {1683, 1684}, {1692, 1693},
		{5007, 4911}, {5006, 4914}, {5098, 5099}, {5107, 5108},
		{5116, 5117}, {5125, 5126}, {5134, 5135}, {5137, 5138},
		{5140, 5141}, {5143, 5144}, {5278, 5279}, {5281, 5282},
		{5733, 5734}, {5736, 5737}, {6192, 6193}, {6195, 6196},
		{6249, 6250}, {6252, 6253}, {6795, 6796}, {6797, 6798},
		{6892, 6893}, {6901, 6902}, {7034, 7035}, {7043, 7044},
		{7712, 7713}, {7715, 7716}, {8250, 8251}, {8253, 8254},
		{8352, 8353}, {8355, 8356}, {9352, 9353}, {9355, 9356},
		{9552, 9553}, {9561, 9562}, {9859, 9860}, {9868, 9869},
		{11137, 11138}, {11144, 11145}, {11233, 11234}, {11242, 11243},
		{13136, 13137}, {17561, 17562}, {17570, 17571}, {17701, 17702},
		{17710, 17711}, {17994, 17995}, {18003, 18004}, {20443, 20445},
		{20452, 20454}, {23874, 23877}, {23876, 23878}, {28365, 28368},
		{28367, 28369}, {30773, 30776}, {30775, 30777}, {37981, 37985},
		{37983, 37986}, {44913, 44917}, {44915, 44918},
		// CustomDoorTable (closedDoor -> openDoor)
		{1638, 1639}, {1640, 1641}, {1656, 1657}, {1658, 1659},
		{1685, 1686}, {1694, 1695}, {2177, 2178}, {2179, 2180},
		{5082, 5083}, {5084, 5085}, {5100, 5101}, {5109, 5110},
		{5118, 5119}, {5127, 5128}, {5283, 5284}, {5285, 5286},
		{5514, 5515}, {5516, 5517}, {6197, 6198}, {6199, 6200},
		{6254, 6255}, {6256, 6257}, {6894, 6895}, {6903, 6904},
		{7036, 7037}, {7045, 7046}, {7054, 7055}, {7056, 7057},
		{7717, 7718}, {7719, 7720}, {8255, 8256}, {8257, 8258},
		{8357, 8358}, {8359, 8360}, {9357, 9358}, {9359, 9360},
		{9554, 9555}, {9563, 9564}, {11705, 11708}, {11714, 11716},
		{12035, 12036}, {12249, 12250}, {15687, 15688}, {17563, 17564},
		{17572, 17573}, {17703, 17704}, {17712, 17713}, {17996, 17997},
		{18005, 18006}, {18025, 18026}, {20446, 20447}, {20455, 20456},
		{24541, 24542}, {24543, 24544}, {24903, 28520},
		{30833, 30837}, {30834, 30837}, {30835, 30838}, {30836, 30838},
		{30849, 30853}, {30850, 30854}, {30851, 30855}, {30852, 30856},
		{31494, 31496}, {31495, 31497}, {31663, 31664}, {31665, 31666},
		{33271, 33272}, {33273, 33274}, {33633, 33636}, {33635, 33637},
		{34221, 34222}, {34223, 34224}, {15890, 15891}, {15892, 15893},
		{22502, 22503}, {22504, 22505}, {39660, 39666}, {39661, 39667},
		{48495, 48497}, {48496, 48498}, {48499, 48501}, {48500, 48502},
		{48520, 48524}, {48522, 48526}, {48528, 48530}, {48529, 48531},
		{49678, 49682}, {49679, 49682}, {49680, 49683}, {49681, 49683},
		{49684, 49688}, {49685, 49688}, {49686, 49689}, {49687, 49689},
		// QuestDoorTable (closedDoor -> openDoor)
		{1642, 1643}, {1644, 1645}, {1660, 1661}, {1662, 1663},
		{1674, 1675}, {1676, 1677}, {1689, 1690}, {1698, 1699},
		{5104, 5105}, {5113, 5114}, {5122, 5123}, {5131, 5132},
		{5287, 5288}, {5289, 5290}, {5749, 5748}, {6201, 6202},
		{6203, 6204}, {6258, 6259}, {6260, 6261}, {6898, 6899},
		{6907, 6908}, {7040, 7041}, {7049, 7050}, {7721, 7722},
		{7723, 7724}, {8259, 8260}, {8261, 8262}, {8361, 8362},
		{8363, 8364}, {9361, 9362}, {9363, 9364}, {9558, 9559},
		{9567, 9568}, {9865, 9866}, {9874, 9875}, {11148, 11149},
		{11239, 11240}, {11248, 11249}, {17567, 17568}, {17576, 17577},
		{17707, 17708}, {17716, 17717}, {18000, 18001}, {18009, 18010},
		{20450, 20451}, {20459, 20460}, {22506, 22507}, {22508, 22509},
		{28658, 28885}, {28659, 28886}, {30041, 30042}, {30043, 30044},
		{30045, 30046}, {30047, 30048}, {31568, 31569}, {31570, 31571},
		{36547, 36548}, {39351, 39353}, {39352, 39354}, {42744, 42745},
		// LevelDoorTable (closedDoor -> openDoor)
		{1646, 1647}, {1648, 1649}, {1664, 1665}, {1666, 1667},
		{1678, 1679}, {1680, 1681}, {1687, 1688}, {1696, 1697},
		{5102, 5103}, {5111, 5112}, {5120, 5121}, {5129, 5130},
		{5291, 5292}, {5293, 5294}, {6205, 6206}, {6207, 6208},
		{6262, 6263}, {6264, 6265}, {6896, 6897}, {6905, 6906},
		{7038, 7039}, {7047, 7048}, {7725, 7726}, {7727, 7728},
		{8263, 8264}, {8265, 8266}, {8365, 8366}, {8367, 8368},
		{9365, 9366}, {9367, 9368}, {9556, 9557}, {9565, 9566},
		{9863, 9864}, {9872, 9873}, {11139, 11140}, {11146, 11147},
		{11237, 11238}, {11246, 11247}, {12033, 12034}, {17565, 17566},
		{17574, 17575}, {17705, 17706}, {17714, 17715}, {17998, 17999},
		{18007, 18008}, {20448, 20449}, {20457, 20458},
		{30033, 30035}, {30034, 30036}, {30037, 30039}, {30038, 30040},
	};
	return doors;
}

int BotEngine::getPOIWeight(POIType type) const {
	// Reads cached values — refresh handled per-tick. See refreshLivenessCfgIfStale.
	const auto& c = livenessCfg_;
	switch (type) {
		case POIType::DEPOT:             return c.poiWeightDepot;
		case POIType::DEPOT_OUTSIDE:     return c.poiWeightDepotOutside;
		case POIType::TEMPLE:            return c.poiWeightTemple;
		case POIType::BOAT:              return c.poiWeightBoat;
		case POIType::SHOP:              return c.poiWeightShop;
		case POIType::NPC:               return c.poiWeightNpc;
		case POIType::ADVENTURER_STONE:  return c.poiWeightAdvStone;
		case POIType::WATER:             return c.poiWeightWater;
		case POIType::HOUSE:             return c.poiWeightHouse;
		case POIType::REWARD_SHRINE:     return c.poiWeightRewardShrine;
		case POIType::IMBUING_SHRINE:    return c.poiWeightImbuingShrine;
	}
	// No `default:` — deliberately. With one, a POIType added later and forgotten here compiles
	// cleanly, loads its config keys, builds its index, reports real data in its debug command, and
	// then contributes 0 to every weighted roll: the candidate can essentially never win, and
	// nothing in the build or the journal says why. Enumerating every case instead turns that into
	// a -Wswitch compile error, which is the only signal this failure mode has.
	return 0;
}

// Build dynamic spell tables from server registry + Lua file parsing
void BotEngine::buildSpellTables() {
	// Parse register_spells.lua matrices first — spells reference these via cardinal/diagonal
	// matrix pointers, so they must be loaded before resolved spells are built.
	areaMatrices_ = parseLuaAreaMatrices("data/scripts/lib/register_spells.lua");
	g_logger().info("[BotEngine] Parsed {} area matrices from register_spells.lua",
		areaMatrices_.size());

	// Parse Lua files for combat metadata
	auto parsedSpells = parseLuaSpellFiles("data/scripts/spells/attack");
	auto parsedRunes = parseLuaRuneFiles("data/scripts/runes");

	g_logger().info("[BotEngine] Parsed {} attack spell + {} rune definitions from Lua files",
		parsedSpells.size(), parsedRunes.size());

	// Build per-vocation spell lists from server registry + Lua metadata
	for (int baseVoc = 1; baseVoc <= 4; baseVoc++) {
		resolvedSingleSpells_[baseVoc].clear();
		resolvedAoeSpells_[baseVoc].clear();
		uint16_t promVocId = static_cast<uint16_t>(baseVoc + 4); // promoted vocation ID

		for (const auto& [words, spell] : g_spells().getInstantSpells()) {
			// Must be available to this promoted vocation
			const auto& vocMap = spell->getVocMap();
			if (vocMap.find(promVocId) == vocMap.end()) continue;

			// Must be aggressive attack spell
			if (!spell->getAggressive()) continue;
			auto grp = spell->getGroup();
			if (grp != SPELLGROUP_ATTACK && grp != SPELLGROUP_ULTIMATESTRIKES
				&& grp != SPELLGROUP_BURSTS_OF_NATURE && grp != SPELLGROUP_GREAT_BEAMS)
				continue;

			// Join with parsed Lua metadata (key = spell words)
			auto metaIt = parsedSpells.find(spell->getWords());
			if (metaIt == parsedSpells.end()) continue;

			const auto& meta = metaIt->second;
			ResolvedSpell rs;
			// From server spell registry:
			rs.words = spell->getWords();
			rs.range = spell->getRange();
			rs.level = spell->getLevel();
			rs.magicLevel = spell->getMagicLevel();
			rs.needDirection = spell->getNeedDirection();
			rs.spellId = spell->getSpellId();
			rs.group = spell->getGroup();
			rs.secondaryGroup = spell->getSecondaryGroup();
			// From Lua parsing:
			rs.combatType = meta.combatType;
			rs.isAoe = meta.isAoe;
			rs.usesSkillFormula = meta.usesSkillFormula;
			rs.avgMlCoef = (meta.minMlCoef + meta.maxMlCoef) / 2.0;
			rs.avgConst = (meta.minConst + meta.maxConst) / 2.0;
			// Area mapping:
			if (meta.isAoe) {
				rs.aoeAreaType = areaPatternToType(meta.areaPattern);
				rs.aoeAreaSize = areaPatternToSize(meta.areaPattern);
				rs.aoeInnerSize = areaPatternToInnerSize(meta.areaPattern); // 0 for non-RING
				rs.minTargets = 1; // allow AoE on single targets for behavior variety
				rs.areaPatternName = meta.areaPattern;
				// Bind matrix pointers if the parsed areas exist in our matrix table.
				// Falls back to enum-based predicate when matrix is null.
				if (auto it = areaMatrices_.find(meta.areaPattern); it != areaMatrices_.end()) {
					rs.cardinalMatrix = &it->second;
				}
				if (!meta.diagonalAreaPattern.empty()) {
					if (auto it = areaMatrices_.find(meta.diagonalAreaPattern); it != areaMatrices_.end()) {
						rs.diagonalMatrix = &it->second;
					}
				}
			}

			if (meta.isAoe) {
				resolvedAoeSpells_[baseVoc].push_back(rs);
			} else {
				resolvedSingleSpells_[baseVoc].push_back(rs);
			}
		}
		g_logger().info("[BotEngine] Voc {}: {} single-target + {} AoE attack spells",
			baseVoc, resolvedSingleSpells_[baseVoc].size(), resolvedAoeSpells_[baseVoc].size());
	}

	// Build resolved rune lists from parsed Lua data
	resolvedAoeRunes_.clear();
	resolvedSdRune_ = ResolvedRune{};
	for (const auto& [runeId, meta] : parsedRunes) {
		ResolvedRune rr;
		rr.runeId = runeId;
		rr.combatType = meta.combatType;
		rr.isAoe = meta.isAoe;
		rr.avgMlCoef = (meta.minMlCoef + meta.maxMlCoef) / 2.0;
		rr.avgConst = (meta.minConst + meta.maxConst) / 2.0;
		if (auto it = areaMatrices_.find(meta.areaPattern); it != areaMatrices_.end()) {
			rr.cardinalMatrix = &it->second;
		}
		if (!meta.diagonalAreaPattern.empty()) {
			if (auto it = areaMatrices_.find(meta.diagonalAreaPattern); it != areaMatrices_.end()) {
				rr.diagonalMatrix = &it->second;
			}
		}
		if (runeId == 3155) {
			resolvedSdRune_ = rr; // Sudden Death rune
		} else if (meta.isAoe) {
			resolvedAoeRunes_.push_back(rr);
		}
	}
	g_logger().info("[BotEngine] Resolved {} AoE runes + SD rune (id={})",
		resolvedAoeRunes_.size(), resolvedSdRune_.runeId);

	// BOT_PVP_REALISM: resolve haste spell words per vocation (strongest castable first).
	pvpResolveHasteSpells();

	// Self-check: log a few key matrices so we can verify parser correctness in journalctl.
	for (const char* name : {"AREA_SHORTWAVE3", "AREA_WAVE4", "AREA_WAVE5", "AREA_WAVE7",
			"AREA_SQUAREWAVE5", "AREA_BEAM5", "AREA_RING1_BURST3"}) {
		auto it = areaMatrices_.find(name);
		if (it == areaMatrices_.end()) {
			g_logger().info("[BotEngine] AreaMatrix {}: NOT FOUND", name);
			continue;
		}
		const auto& m = it->second;
		g_logger().info("[BotEngine] AreaMatrix {}: rows={} cols={} center=({},{}) anchor={{{}}} extent=row{}/col{}",
			name, m.cells.size(), m.cells.empty() ? 0 : m.cells[0].size(),
			m.centerRow, m.centerCol, (int)m.anchorValue,
			m.maxRowExtent, m.maxColExtent);
	}
}

const std::vector<RealHealSpell>& BotEngine::getHealSpells(uint8_t baseVoc) {
	// Spells ordered weakest to strongest (selection iterates backwards to pick strongest first)
	// Formulas taken directly from data/scripts/spells/healing/*.lua
	static const std::vector<RealHealSpell> mageHeals = {
		// exura (Light Healing) — level 8, cd 1s
		{8,   "exura",          1, 0.2, 1.4,   8,  0.2, 1.795, 11},
		// exura gran (Intense Healing) — level 20, cd 1s
		{20,  "exura gran",     1, 0.2, 3.184, 20,  0.2, 5.59,  35},
		// exura vita (Ultimate Healing) — level 30, cd 1s
		{30,  "exura vita",     1, 0.2, 6.8,   42,  0.2, 12.9,  90},
		// exura max vita (Restoration) — level 300, cd 6s
		{300, "exura max vita", 6, 0.28, 12.908, 61.6, 0.28, 15.106, 110.6},
	};
	static const std::vector<RealHealSpell> paladinHeals = {
		// exura (Light Healing) — level 8, cd 1s
		{8,   "exura",          1, 0.2, 1.4,  8,   0.2, 1.795, 11},
		// exura gran (Intense Healing) — level 20, cd 1s
		{20,  "exura gran",     1, 0.2, 3.184, 20,  0.2, 5.59,  35},
		// exura san (Divine Healing) — level 35, cd 1s
		{35,  "exura san",      1, 0.2, 7.22, 44,  0.2, 12.79, 79},
		// exura gran san (Salvation) — level 60, cd 1s
		{60,  "exura gran san", 1, 0.2, 12.0, 75,  0.2, 20.0,  125},
	};
	static const std::vector<RealHealSpell> knightHeals = {
		// exura ico (Wound Cleansing) — level 8, cd 1s
		{8,   "exura ico",      1, 0.2, 4.0,  25,  0.2, 7.95,  51},
		// exura med ico (Fair Wound Cleansing) — level 300, cd 1s
		{300, "exura med ico",  1, 0.4, 8.0,  50,  0.4, 15.9,  102},
		// exura gran ico (Intense Wound Cleansing) — level 80, cd 600s
		{80,  "exura gran ico", 600, 0.2, 70.0, 438, 0.2, 92.0, 544},
	};
	static const std::vector<RealHealSpell> empty;
	switch (baseVoc) {
		case 1: case 2: return mageHeals;
		case 3: return paladinHeals;
		case 4: return knightHeals;
		default: return empty;
	}
}

// (Legacy hardcoded CHAT_* arrays + getRandomChat removed in
// BOT_CHAT_LIVENESS_V2 — all call sites now go through tryEmitChat and the
// phrases.json corpus, gaining the observer gate, cooldowns and anti-repeat.)

