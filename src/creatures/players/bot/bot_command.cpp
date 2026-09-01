/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_command.cpp — admin/debug command interface (/cavebot ...)
//
// BOT_NAV_REALISM Phase 11 module split. Compiles into the SAME libbot_engine.so
// as bot_engine.cpp, so /cavebot reload is unchanged. Shared includes, engine-local
// types and the BotEngine class declaration all live in bot_engine_impl.hpp.
//
// Carved out only after tools/botnavsim/module_promote.py reported zero external
// dependencies for this range.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#include "game/movement/teleport.hpp" // Teleport::getDestPos — tpscan reports where a teleporter actually goes

// ============================================================================
// Command interface (Phase 6)
// ============================================================================

std::string BotEngine::executeCommand(const std::string& botName, const std::string& command) {
	// Global commands — don't need a specific bot
	if (command == "schedule on") {
		schedulerEnabled_ = true;
		return "Population scheduler enabled.";
	}
	if (command == "schedule off") {
		schedulerEnabled_ = false;
		return "Population scheduler disabled (manual control only).";
	}
	if (command == "schedule status") {
		time_t rawTime = std::time(nullptr);
		struct tm* timeInfo = std::localtime(&rawTime);
		int hour = timeInfo->tm_hour;
		int minute = timeInfo->tm_min;
		int32_t pct = getSchedulePercent(hour, minute);
		int32_t total = static_cast<int32_t>(countTotalBots());
		int32_t target = total * pct / 100 + populationJitter_;
		target = std::max(0, std::min(target, total));
		return fmt::format("Scheduler: {} | Time: {:02d}:{:02d} | Target: {} ({}%+{}) | Active: {} / {}",
			schedulerEnabled_ ? "ON" : "OFF", hour, minute, target, pct,
			populationJitter_, countActiveBots(), total);
	}

	// ---- BOT_CSV Milestone 2: authored-data editors + file-reading viewers ----
	// These carry what bot_cavebot.lua's 46 direct db.query sites used to do. Checked
	// early and by exact "csv" prefix so they cannot shadow a bot name.
	{
		std::string csvReply;
		if (handleCsvEditCommand(command, csvReply) || handleCsvViewCommand(command, csvReply)
		    || handleCsvPositionsCommand(command, csvReply)
		    || handleCsvCheckCommand(command, csvReply)) {
			return csvReply;
		}
	}

	// ---- BOT_CSV: canonical authored-data dump (the MySQL->CSV parity gate) ----
	// /cavebot <anybot> datadump <tag>  — writes data/bot/dumps/<tag>.txt (gitignored).
	// Migration scaffolding: capture a dump on the MySQL loaders, then again on the CSV
	// loaders, and diff. buildAuthoredDataDump() must NOT be edited between the two
	// captures — identical serialization code on both sides is the whole point.
	if (command.substr(0, 8) == "datadump") {
		std::string tag = command.size() > 9 ? command.substr(9) : "";
		tag.erase(std::remove_if(tag.begin(), tag.end(),
			[](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-'; }),
			tag.end());
		if (tag.empty()) {
			tag = "dump";
		}
		std::error_code ec;
		std::filesystem::create_directories("data/bot/dumps", ec);
		const std::string path = fmt::format("data/bot/dumps/{}.txt", tag);
		const std::string dump = buildAuthoredDataDump();
		std::string err;
		if (!botCsvAtomicWrite(path, dump, err)) {
			return fmt::format("datadump FAILED: {}", err);
		}
		return fmt::format("Dumped {} bytes to {} (scripts={} poiTowns={} routeTowns={} equip={}).",
			dump.size(), path, huntScripts_.size(), cityPOIs_.size(), cityRouteGraphs_.size(),
			equipmentData_.size());
	}

	// ---- proximity: on-demand liveness diagnostic ----
	// Same output as the 60s periodic [PROXIMITY] log. Useful when the user wants
	// an immediate snapshot without waiting for the next periodic emission.
	if (command == "proximity") {
		return buildProximityReport();
	}

	// ---- population: on-demand per-town bot count by state ----
	if (command == "population") {
		// Append the tick-phase histogram (Phase 1 acceptance signal) to the population report.
		return buildPopulationReport() + "\n" + buildTickPhaseHistogram();
	}

	// ---- pathtest: BOT_NAV_REALISM Phase 2 parity gate. Runs N random queries from an
	// awake bot's position through BOTH the stock server pathfinder and the pathcore kernel
	// (at mask=0 for parity, and mask=3 to measure jitter route inflation). Logs a summary. ----
	if (command == "pathtest" || command.substr(0, 9) == "pathtest ") {
		int queries = 60;
		if (command.size() > 9) {
			try { queries = std::clamp(std::stoi(command.substr(9)), 1, 500); } catch (...) {}
		}
		// Pick an awake bot as the creature/context for canWalkTo + getTileWalkCost.
		std::shared_ptr<Player> ctx;
		Position start;
		for (auto& b : bots_) {
			if (b.active && !b.hibernated) {
				if (auto p = b.getPlayer()) { ctx = p; start = b.currentPos; break; }
			}
		}
		if (!ctx) {
			return "pathtest: no awake bot available as pathfinding context (wake one near you first).";
		}
		FindPathParams fpp;
		fpp.fullPathSearch = true;
		fpp.clearSight = false;
		fpp.allowDiagonal = true;
		fpp.keepDistance = false;
		fpp.maxSearchDist = PATH_MAX_DIST;
		fpp.minTargetDist = 0;
		fpp.maxTargetDist = 3;

		int valid = 0, parity = 0, kernelOnlyFail = 0, serverOnlyFail = 0, bothFail = 0;
		int64_t sumServerLenPaired = 0, sumKernelJitterLen = 0, jitterSamples = 0, sumAbsDiff = 0;
		for (int i = 0; i < queries; i++) {
			// random walkable target within ~10-30 tiles, same z
			Position tgt = start;
			tgt.x = static_cast<uint16_t>(static_cast<int>(start.x) + uniform_random(-30, 30));
			tgt.y = static_cast<uint16_t>(static_cast<int>(start.y) + uniform_random(-30, 30));
			// Skip targets within maxTargetDist of start: the kernel legitimately returns an EMPTY
			// path there (start already satisfies the target predicate), which botJitterPath reports
			// as failure while the server reports success — a false kernelOnlyFail (Opus review).
			if (std::max(Position::getDistanceX(start, tgt), Position::getDistanceY(start, tgt)) <= fpp.maxTargetDist) {
				continue;
			}
			if (!g_game().map.canWalkTo(ctx, tgt)) {
				continue;
			}
			valid++;
			std::vector<Direction> serverDirs, kernel0Dirs, kernel3Dirs;
			bool sOk = g_game().map.getPathMatchingCond(ctx, tgt, serverDirs, FrozenPathingConditionCall(tgt), fpp);
			bool k0Ok = botJitterPath(ctx, start, tgt, fpp, botNavSeed(ctx->getGUID()), 0, kernel0Dirs);
			bool k3Ok = botJitterPath(ctx, start, tgt, fpp, botNavSeed(ctx->getGUID()), 3, kernel3Dirs);
			if (sOk && k0Ok) {
				int diff = std::abs(static_cast<int>(serverDirs.size()) - static_cast<int>(kernel0Dirs.size()));
				sumAbsDiff += diff;
				if (diff <= 1) parity++;
				// Pair the jitter numerator/denominator over the SAME queries (k3 succeeded), else a
				// mask-3 budget failure would subtract a superset sum → negative inflation (Opus review).
				if (k3Ok) {
					sumKernelJitterLen += static_cast<int64_t>(kernel3Dirs.size());
					sumServerLenPaired += static_cast<int64_t>(serverDirs.size());
					jitterSamples++;
				}
			} else if (sOk && !k0Ok) {
				kernelOnlyFail++;
			} else if (!sOk && k0Ok) {
				serverOnlyFail++;
			} else {
				bothFail++;
			}
		}
		int comparable = parity + (valid - parity - kernelOnlyFail - serverOnlyFail - bothFail);
		double parityPct = comparable > 0 ? 100.0 * parity / comparable : 0.0;
		double avgDiff = comparable > 0 ? static_cast<double>(sumAbsDiff) / comparable : 0.0;
		double jitterInflationPct = (sumServerLenPaired > 0)
			? 100.0 * (static_cast<double>(sumKernelJitterLen) - static_cast<double>(sumServerLenPaired)) / static_cast<double>(sumServerLenPaired)
			: 0.0;
		std::string report = fmt::format(
			"[PATHTEST] queries={} valid={} comparable={} parity(len-diff<=1)={} ({:.1f}%) avgLenDiff={:.2f} "
			"| kernelOnlyFail={} serverOnlyFail={} bothFail={} | jitterLenInflation={:.1f}% (n={})",
			queries, valid, comparable, parity, parityPct, avgDiff,
			kernelOnlyFail, serverOnlyFail, bothFail, jitterInflationPct, jitterSamples);
		g_logger().info("{}", report);
		return report;
	}

	// ---- pathbench: cost of the jitter kernel, per pathfinding call ----
	//
	// This answers the question the plan wanted a whole-server CPU A/B for: is the
	// bot_pathcore kernel (binary heap + hash map) more expensive than the server's
	// SIMD open-set scan, and what does mask>0 add on top? A server-wide A/B cannot
	// answer it here — the debug awake cap is 5-10 bots in one city, far too few for
	// a CPU delta to clear the noise floor, and the earlier attempt to run one at ~350
	// awake bots produced an invalid result (unequal arms). Measuring per call needs
	// one awake bot and isolates the kernel from every other bot subsystem.
	//
	// Fairness: targets are chosen once and all three variants walk the SAME list, and
	// a warm-up pass runs first so tile-cache warmth is not credited to whoever ran
	// first. Timing is monotonic (botMonoUs).
	if (command == "pathbench" || command.substr(0, 10) == "pathbench ") {
		int queries = 200;
		if (command.size() > 10) {
			try { queries = std::clamp(std::stoi(command.substr(10)), 10, 2000); } catch (...) {}
		}
		std::shared_ptr<Player> ctx;
		Position start;
		for (auto& b : bots_) {
			if (b.active && !b.hibernated) {
				if (auto p = b.getPlayer()) { ctx = p; start = b.currentPos; break; }
			}
		}
		if (!ctx) {
			return "pathbench: no awake bot available as pathfinding context (wake one first).";
		}

		FindPathParams fpp;
		fpp.fullPathSearch = true;
		fpp.clearSight = false;
		fpp.allowDiagonal = true;
		fpp.keepDistance = false;
		fpp.maxSearchDist = PATH_MAX_DIST;
		fpp.minTargetDist = 0;
		fpp.maxTargetDist = 3;

		std::vector<Position> targets;
		for (int attempts = 0; attempts < queries * 25 && static_cast<int>(targets.size()) < queries; attempts++) {
			Position tgt = start;
			tgt.x = static_cast<uint16_t>(static_cast<int>(start.x) + uniform_random(-30, 30));
			tgt.y = static_cast<uint16_t>(static_cast<int>(start.y) + uniform_random(-30, 30));
			if (std::max(Position::getDistanceX(start, tgt), Position::getDistanceY(start, tgt)) <= fpp.maxTargetDist) {
				continue;
			}
			if (!g_game().map.canWalkTo(ctx, tgt)) {
				continue;
			}
			targets.push_back(tgt);
		}
		if (targets.empty()) {
			return "pathbench: no reachable targets around the context bot.";
		}

		const uint32_t seed = botNavSeed(ctx->getGUID());
		auto runServer = [&]() {
			int ok = 0;
			const int64_t t0 = botMonoUs();
			for (const auto& t : targets) {
				std::vector<Direction> d;
				if (g_game().map.getPathMatchingCond(ctx, t, d, FrozenPathingConditionCall(t), fpp)) ok++;
			}
			return std::make_pair(botMonoUs() - t0, ok);
		};
		auto runKernel = [&](int32_t mask) {
			int ok = 0;
			const int64_t t0 = botMonoUs();
			for (const auto& t : targets) {
				std::vector<Direction> d;
				if (botJitterPath(ctx, start, t, fpp, seed, mask, d)) ok++;
			}
			return std::make_pair(botMonoUs() - t0, ok);
		};

		runServer();               // warm-up: pay tile-cache costs before anything is timed
		auto [usSrv, okSrv] = runServer();
		auto [usK0, okK0] = runKernel(0);
		auto [usK3, okK3] = runKernel(3);

		const auto n = static_cast<double>(targets.size());
		const double perSrv = usSrv / n, perK0 = usK0 / n, perK3 = usK3 / n;
		std::string report = fmt::format(
			"[PATHBENCH] n={} | server={:.1f}us/call ok={} | kernel mask=0 {:.1f}us/call ok={} ({:+.0f}% vs server) "
			"| kernel mask=3 {:.1f}us/call ok={} ({:+.0f}% vs mask=0)",
			targets.size(), perSrv, okSrv, perK0, okK0,
			perSrv > 0 ? 100.0 * (perK0 - perSrv) / perSrv : 0.0,
			perK3, okK3,
			perK0 > 0 ? 100.0 * (perK3 - perK0) / perK0 : 0.0);
		g_logger().info("{}", report);
		return report;
	}

	// ---- dumpnav: BOT_NAV_REALISM Phase 3. Snapshot a region's bot-walkability +
	// FC/teleport/PZ/door flags + walk cost into a binary NavDump for the offline
	// simulator (tools/botnavsim). Syntax: dumpnav x1,y1,x2,y2,z1,z2 ----
	if (command.substr(0, 8) == "dumpnav ") {
		int x1, y1, x2, y2, z1, z2;
		if (std::sscanf(command.c_str() + 8, "%d,%d,%d,%d,%d,%d", &x1, &y1, &x2, &y2, &z1, &z2) != 6) {
			return "dumpnav: syntax is 'dumpnav x1,y1,x2,y2,z1,z2'";
		}
		if (x2 < x1) std::swap(x1, x2);
		if (y2 < y1) std::swap(y1, y2);
		if (z2 < z1) std::swap(z1, z2);
		// Bound the region so a fat-fingered range can't OOM the box.
		if ((x2 - x1) > 400 || (y2 - y1) > 400 || (z2 - z1) > 15) {
			return "dumpnav: region too large (max 400x400x16).";
		}
		// canWalkTo needs a creature context; use any awake bot (matches what the
		// kernel's server adapter sees). Wake one first if none are awake.
		std::shared_ptr<Player> ctx;
		for (auto& b : bots_) {
			if (b.active && !b.hibernated) {
				if (auto p = b.getPlayer()) { ctx = p; break; }
			}
		}
		if (!ctx) {
			return "dumpnav: no awake bot as context (wake one first).";
		}
		botnav::NavDump dump;
		dump.x1 = x1; dump.y1 = y1; dump.z1 = z1;
		dump.x2 = x2; dump.y2 = y2; dump.z2 = z2;
		dump.allocate();
		uint32_t walkable = 0, fc = 0;
		for (int z = z1; z <= z2; z++) {
			for (int y = y1; y <= y2; y++) {
				for (int x = x1; x <= x2; x++) {
					Position pos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
					const auto& tile = g_game().map.getTile(pos.x, pos.y, pos.z);
					if (!tile) {
						continue; // leave flags=0 (no ground)
					}
					botnav::NavTile& nt = dump.tiles[dump.index(x, y, z)];
					if (tile->getGround()) {
						nt.flags |= botnav::NAV_HAS_GROUND;
					}
					if (tile->hasFlag(TILESTATE_FLOORCHANGE)) {
						nt.flags |= botnav::NAV_FLOORCHANGE;
					}
					if (tile->hasFlag(TILESTATE_TELEPORT)) {
						nt.flags |= botnav::NAV_TELEPORT;
					}
					if (tile->hasFlag(TILESTATE_PROTECTIONZONE)) {
						nt.flags |= botnav::NAV_PROTECTIONZONE;
					}
					// v2: closed (openable) doors — the runtime opens these, so the
					// offline sim must treat them as walkable-at-extra-cost instead
					// of walls (a v1 dump baked castle/prison interiors shut, which
					// hid every stair behind a door from the multi-floor gate).
					if (const auto* tileItems = tile->getItemList()) {
						auto& doorTable = getDoorTable();
						for (const auto& item : *tileItems) {
							const uint16_t did = item->getID();
							// KEY-LOCKED doors are NOT NAV_DOOR: the offline sim models NAV_DOOR as
							// walkable-at-cost-60 ("the runtime opens them"), which is false for these
							// — bots have no key logic. Flagging them made zcheck pass routes the live
							// bot cannot walk. They stay unflagged, so the sim sees them as walls,
							// which is exactly what they are to a bot.
							if (isKeyLockedDoorId(did)) {
								continue;
							}
							if (doorTable.find(did) != doorTable.end()) {
								nt.flags |= botnav::NAV_DOOR;
								break;
							}
						}
					}
					// bot-walkability = same predicate the kernel adapter uses.
					const auto& wt = g_game().map.canWalkTo(ctx, pos);
					if (wt) {
						nt.flags |= botnav::NAV_WALKABLE;
						walkable++;
						int32_t wc = static_cast<int32_t>(AStarNodes::getTileWalkCost(ctx, wt));
						nt.walkCost = static_cast<uint8_t>(wc > 255 ? 255 : (wc < 0 ? 0 : wc));
					}
					if (nt.flags & (botnav::NAV_FLOORCHANGE | botnav::NAV_TELEPORT)) {
						fc++;
					}
				}
			}
		}
		// Town anchors (temples) that fall in the region — useful context for the sim.
		for (const auto& [townId, town] : g_game().map.towns.getTowns()) {
			auto tp = town->getTemplePosition();
			if (dump.inBounds(tp.x, tp.y, tp.z)) {
				dump.anchors.push_back({ tp.x, tp.y, tp.z, town->getName() + ":temple" });
			}
		}
		// v2: exact portal section — same classifier as the live portal graph, so
		// the offline zroute gate validates against precisely what the engine
		// will plan with (kind, direction AND landing; USE-item mechanisms like
		// sewers/ropes/shovels that v1 could not represent are included).
		collectNavPortalsInRegion(x1, y1, z1, x2, y2, z2, dump.portals);
		std::string path = fmt::format("/home/tibia/canary/navdump_{}_{}_{}.bin", x1, y1, z1);
		if (!dump.writeFile(path)) {
			return fmt::format("dumpnav: FAILED to write {}", path);
		}
		std::string report = fmt::format(
			"[DUMPNAV] v2 region ({},{},{})-({},{},{}) tiles={} walkable={} fc={} portals={} anchors={} → {} ({} KB)",
			x1, y1, z1, x2, y2, z2, dump.tiles.size(), walkable, fc, dump.portals.size(), dump.anchors.size(), path,
			(dump.tiles.size() * 2) / 1024);
		g_logger().info("{}", report);
		return report;
	}

	// ---- zplan: TRUE MULTI-FLOOR — plan a cross-z route from a position (or the
	// first awake bot) and print every hop. Headless verification surface for the
	// portal graph before any live walk depends on it.
	// Syntax: zplan x,y,z              (from first awake bot's position)
	//         zplan x1,y1,z1 x2,y2,z2  (explicit from → to) ----
	if (command.substr(0, 6) == "zplan ") {
		if (!zGraphReady_) {
			return "zplan: portal graph not built ([ZGRAPH] missing from startup log?)";
		}
		int fx = 0, fy = 0, fz = 0, tx = 0, ty = 0, tz = 0;
		Position from, to;
		const int n = std::sscanf(command.c_str() + 6, "%d,%d,%d %d,%d,%d", &fx, &fy, &fz, &tx, &ty, &tz);
		if (n == 6) {
			from = Position(static_cast<uint16_t>(fx), static_cast<uint16_t>(fy), static_cast<uint8_t>(fz));
			to = Position(static_cast<uint16_t>(tx), static_cast<uint16_t>(ty), static_cast<uint8_t>(tz));
		} else if (n == 3) {
			std::shared_ptr<Player> ctx;
			for (auto& b : bots_) {
				if (b.active && !b.hibernated) {
					if (auto p = b.getPlayer()) {
						from = b.currentPos;
						ctx = p;
						break;
					}
				}
			}
			if (!ctx) {
				return "zplan: no awake bot as origin (wake one or pass from + to).";
			}
			to = Position(static_cast<uint16_t>(fx), static_cast<uint16_t>(fy), static_cast<uint8_t>(fz));
		} else {
			return "zplan: syntax is 'zplan x,y,z' or 'zplan x1,y1,z1 x2,y2,z2'";
		}
		// `-v` traces every leg the planner prices ([ZLEGCOST] in the journal), which is how a
		// NO-ROUTE gets localised to the exact leg + branch that returned -1.
		const bool trace = command.find(" -v") != std::string::npos;
		std::vector<botnav::ZRouteHop> hops;
		zPlanTrace_ = trace;
		const bool planned = zPlanFullRoute(from, to, hops);
		zPlanTrace_ = false;
		if (!planned) {
			std::string bl;
			for (const auto& [key, expiry] : s_zPortalBlacklist) {
				bl += fmt::format(" ({},{},{})", static_cast<uint16_t>(key >> 24) & 0xFFFF,
					static_cast<uint16_t>(key >> 8) & 0xFFFF, static_cast<uint8_t>(key & 0xFF));
			}
			return fmt::format("[ZPLAN] ({},{},{}) -> ({},{},{}) : NO-ROUTE (portals={} blacklisted={}{}){}",
				from.x, from.y, from.z, to.x, to.y, to.z, zGraph_.size(), s_zPortalBlacklist.size(), bl,
				trace ? " [ZLEGCOST traced to journal]" : " (add -v to trace legs)");
		}
		std::string out = fmt::format("[ZPLAN] ({},{},{}) -> ({},{},{}) : {} hop(s)\n",
			from.x, from.y, from.z, to.x, to.y, to.z, hops.size());
		Position at = from;
		for (size_t i = 0; i < hops.size(); i++) {
			const auto& p = hops[i].portal;
			out += fmt::format("  hop {}: {} {} at ({},{},{}) legCheb={} lands ({},{},{})\n",
				i + 1, botnav::zPortalKindName(p.kind), p.goesDown ? "DOWN" : "UP",
				p.pos.x, p.pos.y, p.pos.z, botnav::zCheb(at, p.pos),
				p.landing.x, p.landing.y, p.landing.z);
			at = p.landing;
		}
		out += fmt::format("  final legCheb={}", botnav::zCheb(at, to));
		g_logger().info("{}", out);
		return out;
	}

	// ---- route: the COMPLETE tile-by-tile route, not just the z-hops `zplan` prints.
	// Walks the planned route leg by leg through the SAME pathfinder and the SAME 512-node
	// budget the live walker gets, so a leg that fails here is a leg the bot genuinely cannot
	// walk — no optimistic approximation, and no offline-dump divergence (this sees live doors,
	// creatures and house flags).
	//
	// On a failed leg it also prices the straight-line chunk target goTo() would have aimed at,
	// which is what separates "target is truly unreachable" from "chunking aimed into an
	// obstacle and gave up on a route that exists".
	//
	// The trailing `wide` switches the leg tracer from goTo()'s chunked 512-node walk to ONE
	// direct unchunked 4096-node / PATH_WIDE_DIST search — i.e. exactly planScopedWalk()'s
	// tier 2. Two different walkers now exist, so the diagnostic must be able to say which one it
	// is modelling; the default stays the generic walker, because reporting a route the ordinary
	// bot cannot take is the precise failure this command was built to eliminate.
	// Syntax: route x,y,z                   (from first awake bot's position)
	//         route x1,y1,z1 x2,y2,z2       (explicit from -> to)
	//         route ... wide                (model the planner's tier 2 instead) ----
	if (command.substr(0, 6) == "route ") {
		if (!zGraphReady_) {
			return "route: portal graph not built ([ZGRAPH] missing from startup log?)";
		}
		int fx = 0, fy = 0, fz = 0, tx = 0, ty = 0, tz = 0;
		Position from, to;
		std::shared_ptr<Player> mover;
		for (auto& b : bots_) {
			if (b.active && !b.hibernated) {
				if (auto p = b.getPlayer()) {
					mover = p;
					from = b.currentPos;
					break;
				}
			}
		}
		if (!mover) {
			return "route: no awake bot to use as pathfinding context (wake one first).";
		}
		const bool wideMode = command.size() >= 5 && command.compare(command.size() - 5, 5, " wide") == 0;
		const int n = std::sscanf(command.c_str() + 6, "%d,%d,%d %d,%d,%d", &fx, &fy, &fz, &tx, &ty, &tz);
		if (n == 6) {
			from = Position(static_cast<uint16_t>(fx), static_cast<uint16_t>(fy), static_cast<uint8_t>(fz));
			to = Position(static_cast<uint16_t>(tx), static_cast<uint16_t>(ty), static_cast<uint8_t>(tz));
		} else if (n == 3) {
			to = Position(static_cast<uint16_t>(fx), static_cast<uint16_t>(fy), static_cast<uint8_t>(fz));
		} else {
			return "route: syntax is 'route x,y,z' or 'route x1,y1,z1 x2,y2,z2'";
		}

		std::vector<botnav::ZRouteHop> hops;
		const bool planned = zPlanFullRoute(from, to, hops);
		std::string out = fmt::format("[ROUTE] ({},{},{}) -> ({},{},{}) : {} (mover={}, walker={})\n",
			from.x, from.y, from.z, to.x, to.y, to.z,
			planned ? fmt::format("{} hop(s)", hops.size()) : "NO Z-ROUTE (tracing direct leg only)",
			mover->getName(),
			wideMode ? "planner tier 2 (direct, 4096 nodes, dist<=200)" : "generic goTo (chunked, 512 nodes, dist<=50)");

		// Build the leg list exactly as the walker experiences it: current position to the first
		// portal tile, each landing to the next portal tile, and the last landing to the target.
		struct Leg { Position a, b; std::string label; };
		std::vector<Leg> legs;
		Position at = from;
		if (planned) {
			for (size_t i = 0; i < hops.size(); i++) {
				const auto& p = hops[i].portal;
				legs.push_back({ at, p.pos, fmt::format("leg {} -> hop {} ({} {})", i + 1, i + 1,
					botnav::zPortalKindName(p.kind), p.goesDown ? "DOWN" : "UP") });
				at = p.landing;
			}
		}
		legs.push_back({ at, to, "final leg -> destination" });

		int32_t totalTiles = 0;
		int failedLegs = 0;
		for (size_t i = 0; i < legs.size(); i++) {
			const auto& L = legs[i];
			std::vector<Position> tiles;
			std::string note;
			// wide: ONE unchunked search to the true leg end, which is what the planner's tier 2
			// actually issues. Default: the chunked replay of goTo().
			const bool ok = wideMode
				? botTraceLegPath(mover, L.a, L.b, 0, tiles, /*wide=*/true)
				: botTraceLegWalk(mover, L.a, L.b, 0, tiles, note);
			if (ok) {
				totalTiles += static_cast<int32_t>(tiles.size());
				out += fmt::format("  {}: ({},{},{}) -> ({},{},{}) = {} tiles OK{}\n",
					L.label, L.a.x, L.a.y, L.a.z, L.b.x, L.b.y, L.b.z, tiles.size(),
					note.empty() ? "" : fmt::format("  [{}]", note));
				std::string tl;
				for (size_t t = 0; t < tiles.size(); t++) {
					tl += fmt::format("{}({},{},{})", t ? " " : "", tiles[t].x, tiles[t].y, tiles[t].z);
				}
				out += fmt::format("      {}\n", tl);
			} else {
				failedLegs++;
				out += fmt::format("  {}: ({},{},{}) -> ({},{},{}) = *** NO PATH *** ({})\n",
					L.label, L.a.x, L.a.y, L.a.z, L.b.x, L.b.y, L.b.z,
					note.empty() ? "no detail" : note);
				// Show how far it DID get — a leg that walks 30 tiles then stops is a different
				// bug from one that cannot take a single step.
				if (!tiles.empty()) {
					out += fmt::format("      reached {} tiles before stalling, last=({},{},{})\n",
						tiles.size(), tiles.back().x, tiles.back().y, tiles.back().z);
				}
				// Diagnose: is the target itself unreachable, or did chunking aim into a wall?
				// Only meaningful for the chunked walker — tier 2 never aims at a chunk target.
				if (wideMode) {
					out += "      (wide mode: one direct search, no chunk target to price)\n";
					continue;
				}
				const int32_t sdx = static_cast<int32_t>(L.b.x) - static_cast<int32_t>(L.a.x);
				const int32_t sdy = static_cast<int32_t>(L.b.y) - static_cast<int32_t>(L.a.y);
				const int32_t d = std::max(std::abs(sdx), std::abs(sdy));
				if (d > CHUNK_DIST) {
					const double ratio = static_cast<double>(CHUNK_DIST) / d;
					Position chunk = L.a;
					chunk.x = L.a.x + static_cast<uint16_t>(sdx * ratio);
					chunk.y = L.a.y + static_cast<uint16_t>(sdy * ratio);
					std::vector<Position> ct;
					const bool chunkOk = botTraceLegPath(mover, L.a, chunk, 3, ct);
					out += fmt::format("      cheb={} > CHUNK_DIST={} -> goTo() would aim at ({},{},{}) which is {}\n",
						d, CHUNK_DIST, chunk.x, chunk.y, chunk.z, chunkOk ? "REACHABLE" : "ALSO UNREACHABLE");
				} else {
					out += fmt::format("      cheb={} <= CHUNK_DIST={} (no chunking; target genuinely unreachable)\n",
						d, CHUNK_DIST);
				}
			}
		}
		out += fmt::format("  TOTAL {} tiles across {} leg(s), {} failed", totalTiles, legs.size(), failedLegs);
		g_logger().info("{}", out);
		return out;
	}

	// ---- zgraph: TRUE MULTI-FLOOR — portal graph stats + portals near a position ----
	// ---- shrines: what the runtime shrine scan actually finds ----
	// FORCE-FILLS every town's memo rather than reporting only what bots happened to populate, so
	// the whole picture is one command and can be diffed against an offline OTBM scan in one go.
	// Run this BEFORE raising botShrineVisitPct: it separates "the scan is wrong" from "the walk
	// is wrong", and the stand positions it prints are valid `/cavebot <bot> goto x,y,z` arguments
	// for testing the walk on its own.
	if (command == "shrines" || command.rfind("shrines ", 0) == 0) {
		const size_t sp = command.find(' ');
		return describeShrines(sp == std::string::npos ? "" : command.substr(sp + 1));
	}

	// ---- fishspots: what the fishing-spot index actually holds ----
	// The direct check that the water-id list matches the live map. Run this BEFORE raising
	// botFishPct: it separates "the index is wrong" from "the walk is wrong", and the stand
	// positions it prints are valid `/cavebot <bot> goto x,y,z` arguments for testing the walk
	// on its own.
	if (command == "fishspots" || command.substr(0, 10) == "fishspots ") {
		if (fishingSpots_.empty()) {
			return "fishspots: index empty — the graph build found no fishable water "
			       "(check [BotFish] at startup; a v1 zgraph cache would also do this)";
		}
		uint32_t wantTown = 0;
		if (command.size() > 10) {
			wantTown = static_cast<uint32_t>(std::atoi(command.c_str() + 10));
		}
		uint32_t total = 0;
		for (const auto& [t, v] : fishingSpots_) {
			total += static_cast<uint32_t>(v.size());
		}
		std::string out = fmt::format("[BotFish] {} spots across {} towns\n", total, fishingSpots_.size());
		for (const auto& [townId, spots] : fishingSpots_) {
			if (wantTown != 0 && townId != wantTown) {
				continue;
			}
			std::string townName = "?";
			if (auto town = g_game().map.towns.getTown(townId)) {
				townName = town->getName();
			}
			out += fmt::format("  town {} ({}): {} spot(s)\n", townId, townName, spots.size());
			const size_t show = wantTown != 0 ? std::min<size_t>(spots.size(), 12) : std::min<size_t>(spots.size(), 2);
			for (size_t i = 0; i < show; ++i) {
				out += fmt::format("    stand ({},{},{})  water ({},{},{})\n",
					spots[i].stand.x, spots[i].stand.y, spots[i].stand.z,
					spots[i].water.x, spots[i].water.y, spots[i].water.z);
			}
		}
		g_logger().info("{}", out);
		return out;
	}


	// ---- BOT_ROUTE_SPLICE audit. READ-ONLY: it splices a COPY and reports, changing nothing.
	//
	// This exists because the offline report (tools/bot_route_splice/report.py) has no map and so
	// cannot run gates 6a/6b/6c at all — it reports every window that passes the geometric rules,
	// a strict superset. Quoting its output as a verdict is exactly how a bad Edron splice got
	// presented as real. This command is the one that actually knows.
	//
	// Syntax: splice <townId>              audit every direct route in the town
	//         splice <townId> <src> <dst>  audit ONE route through the FULL resolution, including
	//                                      the multi-hop chain fallback, so a chain splice that
	//                                      spans the join POI can be checked on demand (the
	//                                      town-wide form cannot enumerate chains — they are
	//                                      per-bot randomised).
	// ---- BOT_TRAVEL_ARRIVE_MIX audit. READ-ONLY: dumps what a landing bot could be sent to in a
	// town, and the effective weights. This is the check that a shop roll in Roshamuul has nothing
	// to resolve to BEFORE anyone wonders why the realised split is depot-heavy.
	if (command == "arrive" || command.substr(0, 7) == "arrive ") {
		int townId = -1;
		if (command.size() > 7) {
			std::sscanf(command.c_str() + 7, "%d", &townId);
		}
		auto& cm = g_configManager();
		const int32_t wD = static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_DEPOT_PCT));
		const int32_t wT = static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_TEMPLE_PCT));
		const int32_t wS = static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_SHOP_PCT));
		const int32_t wO = static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_ARRIVE_OTHER_PCT));
		std::string out = fmt::format("[ARRIVE] weights depot={} temple={} shop={} other={} (sum={})\n",
			wD, wT, wS, wO, wD + wT + wS + wO);
		size_t withShops = 0;
		for (const auto& [tid, t] : travelArriveTargets_) {
			if (!t.shops.empty()) {
				withShops++;
			}
			if (townId >= 0 && static_cast<int>(tid) != townId) {
				continue;
			}
			out += fmt::format("  town {:>2}  shops[{}]: {}\n", tid, t.shops.size(),
				t.shops.empty() ? std::string("(none -- shop rolls fall back to depot)")
				                : fmt::format("{}", fmt::join(t.shops, ", ")));
			out += fmt::format("           other[{}]: {}\n", t.others.size(),
				t.others.empty() ? std::string("(none -- other rolls fall back to depot)")
				                 : fmt::format("{}", fmt::join(t.others, ", ")));
		}
		out += fmt::format("  {} of {} town(s) have any shop authored\n", withShops, travelArriveTargets_.size());
		g_logger().info("{}", out);
		return out;
	}

	if (command == "splice" || command.substr(0, 7) == "splice ") {
		int townId = 0;
		char srcBuf[64] = { 0 };
		char dstBuf[64] = { 0 };
		const int n = command.size() > 7
			? std::sscanf(command.c_str() + 7, "%d %63s %63s", &townId, srcBuf, dstBuf)
			: 0;
		if (n < 1) {
			return "splice: syntax is 'splice <townId>' or 'splice <townId> <src> <dst>'";
		}
		auto graphIt = cityRouteGraphs_.find(static_cast<uint32_t>(townId));
		if (graphIt == cityRouteGraphs_.end()) {
			return fmt::format("splice: town {} has no city routes loaded", townId);
		}

		std::vector<std::string> accepted;
		std::vector<std::string> rejected;
		size_t routesSeen = 0;
		size_t routesTouched = 0;
		size_t wpsCut = 0;

		auto auditOne = [&](const std::string& src, const std::string& dst,
		                    const std::vector<Waypoint>& wps, const std::string& chainKey) {
			routesSeen++;
			std::vector<Waypoint> copy = wps; // never touch the graph's own vector
			const size_t before = copy.size();
			std::vector<std::string> acc;
			std::vector<std::string> rej;
			const size_t cut = spliceRouteDetours(copy, chainKey.empty()
				? fmt::format("{}~{}", src, dst) : chainKey, &acc, &rej);
			if (cut > 0) {
				routesTouched++;
				wpsCut += cut;
				accepted.insert(accepted.end(), acc.begin(), acc.end());
				accepted.push_back(fmt::format("    ^ {} wps -> {}", before, copy.size()));
			}
			rejected.insert(rejected.end(), rej.begin(), rej.end());
		};

		if (n >= 3) {
			std::vector<Waypoint> scratch;
			std::string chainKey;
			const auto* route = resolveCityRoute(static_cast<uint32_t>(townId), srcBuf, dstBuf,
				scratch, chainKey);
			if (!route) {
				return fmt::format("splice: town {} has no route {} -> {} (direct or chained)",
					townId, srcBuf, dstBuf);
			}
			auditOne(srcBuf, dstBuf, *route, chainKey);
		} else {
			for (const auto& [src, dstMap] : graphIt->second.pairs) {
				for (const auto& [dst, wps] : dstMap) {
					auditOne(src, dst, wps, fmt::format("{}~{}", src, dst));
				}
			}
		}

		std::string out = fmt::format("[SPLICE] town {} — {} route(s) audited, {} would change, "
			"{} waypoint(s) cut, {} window(s) declined\n",
			townId, routesSeen, routesTouched, wpsCut, rejected.size());
		for (const auto& line : accepted) {
			out += "  ACCEPT " + line + "\n";
		}
		// Declines are the more informative half: they name which gate said no, which is how a
		// probe that is silently rejecting EVERYTHING (the inert-feature failure) is told apart
		// from one that is discriminating properly.
		for (size_t k = 0; k < rejected.size() && k < 40; k++) {
			out += "  " + rejected[k] + "\n";
		}
		if (rejected.size() > 40) {
			out += fmt::format("  ... and {} more declined window(s)\n", rejected.size() - 40);
		}
		g_logger().info("{}", out);
		return out;
	}

	if (command == "zgraph" || command.substr(0, 7) == "zgraph ") {
		if (!zGraphReady_) {
			return "zgraph: portal graph not built";
		}
		const uint64_t reachTotal = zReachHits_ + zReachMisses_;
		std::string out = fmt::format(
			"[ZGRAPH] portals={} blacklisted={} componentAnchors={}\n"
			"[ZREACH] cache entries={}/{} hits={} misses={} hit%={}\n",
			zGraph_.size(), s_zPortalBlacklist.size(), zPortalComponent_.size(),
			zReachCache_.size(), Z_REACH_CACHE_MAX, zReachHits_, zReachMisses_,
			reachTotal ? (zReachHits_ * 100 / reachTotal) : 0);
		int px = 0, py = 0, pz = 0;
		if (std::sscanf(command.c_str() + 6, " %d,%d,%d", &px, &py, &pz) == 3) {
			const Position c(static_cast<uint16_t>(px), static_cast<uint16_t>(py), static_cast<uint8_t>(pz));
			int shown = 0;
			zGraph_.forEachOnFloorNear(c.z, c, 30, [&](uint32_t, const botnav::ZPortal& p) {
				if (shown++ < 20) {
					out += fmt::format("  {} {} ({},{},{}) -> ({},{},{})\n",
						botnav::zPortalKindName(p.kind), p.goesDown ? "DOWN" : "UP",
						p.pos.x, p.pos.y, p.pos.z, p.landing.x, p.landing.y, p.landing.z);
				}
			});
			out += fmt::format("  ({} portals within 30 tiles of ({},{},{}))", shown, c.x, c.y, c.z);
		}
		return out;
	}

	// ---- npcapproach: BOT_NAV_REALISM Phase 8 — inspect the approach tiles derived for an NPC.
	// Headless test surface for increment 1 (and the resolver in increment 2) so the anchor data
	// can be verified from the bot_commands queue before any live caller depends on it. ----
	if (command.substr(0, 12) == "npcapproach ") {
		const std::string npcName = command.substr(12);
		auto it = npcAnchors_.find(npcName);
		if (it == npcAnchors_.end()) {
			return fmt::format("npcapproach: no anchors for '{}' (total anchored NPC names: {})",
				npcName, npcAnchors_.size());
		}
		std::string out = fmt::format("[NPC_APPROACH] '{}' — {} instance(s)\n", npcName, it->second.size());
		for (const auto& a : it->second) {
			out += fmt::format("  npc=({},{},{}) town={} tiles={}:",
				a.npcPos.x, a.npcPos.y, a.npcPos.z, a.townId, a.approachTiles.size());
			for (const auto& t : a.approachTiles) {
				out += fmt::format(" ({},{},{})", t.x, t.y, t.z);
			}
			out += "\n";
		}
		// Exercise the resolver end-to-end with a real bot so reservations are observable before
		// any production caller depends on it: resolve twice and confirm the second bot is handed
		// a DIFFERENT tile (i.e. the first bot's claim was honoured).
		BotState* b1 = nullptr;
		BotState* b2 = nullptr;
		for (auto& b : bots_) {
			if (!b.active) continue;
			if (!b1) { b1 = &b; continue; }
			b2 = &b;
			break;
		}
		if (b1) {
			Position t1;
			bool fb1 = false;
			if (resolveNpcApproach(*b1, npcName, t1, fb1)) {
				out += fmt::format("  resolve#1 guid={} -> ({},{},{}) fallback={}\n",
					b1->guid, t1.x, t1.y, t1.z, fb1 ? 1 : 0);
			}
			if (b2) {
				Position t2;
				bool fb2 = false;
				if (resolveNpcApproach(*b2, npcName, t2, fb2)) {
					out += fmt::format("  resolve#2 guid={} -> ({},{},{}) fallback={} distinctFrom#1={}\n",
						b2->guid, t2.x, t2.y, t2.z, fb2 ? 1 : 0,
						(t1.x != t2.x || t1.y != t2.y || t1.z != t2.z) ? "YES" : "NO");
				}
				releaseNpcApproach(b2->guid);
			}
			releaseNpcApproach(b1->guid); // don't leave test claims behind
		}
		out += fmt::format("  activeReservations={} overflowEvents={}\n",
			s_approachReservations.size(), s_approachOverflow);
		g_logger().info("{}", out);
		return out;
	}

	// ---- cache: BOT_NAV_REALISM Phase 5 — per-name cache hit rate + recompute cost ----
	if (command == "cache") {
		std::string out = "[CACHE] name | hits | misses | hit% | avgUs | worstUs\n";
		for (size_t i = 0; i < static_cast<size_t>(BotCacheId::COUNT); i++) {
			const auto& st = botCacheStats_[i];
			const uint64_t total = st.hits + st.misses;
			out += fmt::format("  {} | {} | {} | {}% | {} | {}\n",
				kBotCacheNames[i], st.hits, st.misses,
				total ? (st.hits * 100 / total) : 0,
				st.misses ? (st.totalUs / st.misses) : 0,
				st.worstUs);
		}
		g_logger().info("{}", out);
		return out;
	}

	// ---- PERF STRESS HARNESS: window telemetry ----
	//
	// Output is single-line JSON so tools/botperf/collect.py can parse it straight out of the
	// bot_commands.result column. Split across three commands on purpose: one blob carrying
	// histograms + phases + config would exceed even the raised 32000-byte result cap, and the
	// truncation is silent (it clips mid-string and the JSON then parses as garbage). Config is
	// NOT duplicated here -- the harness calls the existing `_global botcfg` for that.
	if (command == "perfreset") {
		botPerf_.reset();
		botPerf_.windowStartMs = OTSYS_TIME();
		return "{\"ok\":true,\"cmd\":\"perfreset\"}";
	}

	if (command == "perfstat") {
		const auto& p = botPerf_;
		const int64_t windowMs = p.windowStartMs ? (OTSYS_TIME() - p.windowStartMs) : 0;
		auto hist = [](const char* name, const BotPerfHist& h) {
			std::string b;
			for (size_t i = 0; i < kPerfBucketCount; i++) {
				if (i) { b += ","; }
				b += std::to_string(h.buckets[i]);
			}
			return fmt::format(
				"\"{}\":{{\"n\":{},\"mean\":{:.3f},\"p50\":{},\"p95\":{},\"p99\":{},"
				"\"max\":{:.3f},\"buckets\":[{}]}}",
				name, h.count, h.meanMs(), h.pctileMs(0.50), h.pctileMs(0.95), h.pctileMs(0.99),
				static_cast<double>(h.worstUs) / 1000.0, b);
		};
		std::string out = "{";
		out += fmt::format("\"cmd\":\"perfstat\",\"window_ms\":{},\"ticks\":{},", windowMs, p.ticks);
		out += hist("body_wall", p.bodyWall) + ",";
		out += hist("body_cpu", p.bodyCpu) + ",";
		out += hist("gap_wall", p.gapWall) + ",";
		out += hist("virtual_tick", p.virtualTickWall) + ",";
		out += hist("flush_nav", p.flushNavWall) + ",";
		out += fmt::format(
			"\"counters\":{{\"wakes_teleport\":{},\"budget_served\":{},\"budget_deferred\":{},"
			"\"budget_observed_exempt\":{},\"tick_slow\":{},\"gap_slow\":{}}},",
			p.wakesTeleport, p.budgetServed, p.budgetDeferred, p.budgetObservedExempt,
			p.tickSlowCrossings, p.gapSlowCrossings);
		out += fmt::format(
			"\"gauges\":{{\"awake\":{},\"hibernated\":{},\"anchors\":{},\"probes\":{},"
			"\"real_players\":{}}},",
			p.gaugeAwake, p.gaugeHibernated, p.gaugeAnchors, p.gaugeProbes, p.gaugeRealPlayers);
		// bucket_bounds ship with the payload so the analysis side never has to hardcode them
		// and silently drift from the engine.
		out += "\"bucket_bounds_ms\":[";
		for (size_t i = 0; i < std::size(kPerfBucketBoundsMs); i++) {
			if (i) { out += ","; }
			out += std::to_string(kPerfBucketBoundsMs[i]);
		}
		out += "]}";
		g_logger().info("[PERFSTAT] {}", out);
		return out;
	}

	if (command == "perfphases") {
		const auto& p = botPerf_;
		std::string out = fmt::format("{{\"cmd\":\"perfphases\",\"ticks\":{},\"phases\":[", p.ticks);
		for (size_t i = 0; i < static_cast<size_t>(BotTickPhase::COUNT); i++) {
			const auto& h = p.phase[i];
			if (i) { out += ","; }
			out += fmt::format(
				"{{\"name\":\"{}\",\"n\":{},\"mean\":{:.3f},\"p95\":{},\"p99\":{},"
				"\"max\":{:.3f},\"total_ms\":{:.1f}}}",
				kBotTickPhaseNames[i], h.count, h.meanMs(), h.pctileMs(0.95), h.pctileMs(0.99),
				static_cast<double>(h.worstUs) / 1000.0,
				static_cast<double>(h.sumUs) / 1000.0);
		}
		out += "],\"worst_ticks\":[";
		bool first = true;
		for (size_t i = 0; i < kPerfWorstTicks; i++) {
			const auto& w = p.worst[i];
			if (w.bodyWallMs <= 0) { continue; }
			if (!first) { out += ","; }
			first = false;
			// body_cpu against body_wall is the diagnosis: near-equal means the tick was
			// computing, far-below means it was blocked or preempted.
			out += fmt::format(
				"{{\"body_wall\":{},\"body_cpu\":{},\"phase\":\"{}\",\"phase_ms\":{},\"awake\":{}}}",
				w.bodyWallMs, w.bodyCpuMs,
				w.phase < static_cast<uint8_t>(BotTickPhase::COUNT)
					? kBotTickPhaseNames[w.phase] : "?",
				w.phaseMs, w.awake);
		}
		out += "]}";
		g_logger().info("[PERFPHASES] {}", out);
		return out;
	}

	// ---- PERF STRESS HARNESS: probe bots ----
	//
	// A probe is an ordinary bot flagged as if a human were cast-watching it. That one flag makes
	// every observer gate treat it as a camera, so it wakes neighbours and arms observed-tier
	// behaviour exactly as a real player standing there would -- which is the whole point: it
	// reproduces the manual "log in and walk around" ritual with no client.
	if (command == "probe list") {
		if (s_probeBots.empty()) { return "[PROBE] none"; }
		std::string out = fmt::format("[PROBE] {} active\n", s_probeBots.size());
		for (uint32_t guid : s_probeBots) {
			auto it = guidToIndex_.find(guid);
			if (it == guidToIndex_.end()) { continue; }
			const auto& b = bots_[it->second];
			out += fmt::format("  {} guid={} pos=({},{},{}) hib={} pinned={}\n",
				b.name, guid, b.currentPos.x, b.currentPos.y, b.currentPos.z,
				b.hibernated ? "yes" : "no",
				s_debugPinned.count(guid) ? "yes" : "no");
		}
		return out;
	}

	if (command == "probe clear") {
		const size_t n = clearAllProbeBots();
		return fmt::format("[PROBE] cleared {} probe(s)", n);
	}

	// Mass hibernate: the harness needs a deterministic floor before every window, and only a
	// per-bot `hibernate` existed. Paced by the same Lua monitor, so this is a request, not an
	// instant state change -- the harness polls until the count settles.
	if (command == "hibernateall") {
		const uint32_t n = hibernateAllEligibleBots();
		return fmt::format("[PROBE] hibernated {} bot(s)", n);
	}

	// ---- tpscan: read-only tile inspector for teleport / floor-change tiles ----
	// BOT_TELEPORT_TILE_SAFETY Phase 0. Answers three diagnostic questions that cannot be
	// settled from source: (D2) does a walk-on teleporter actually exist near a given spot
	// and WHERE does it send you (destPos, not just presence — a fixed destination means
	// every bot using it lands in the same town); (D3) do the adventurer's-stone forcefields
	// carry TILESTATE_TELEPORT; (D1) what item really sits on a reported trapdoor tile, and
	// would the unsafe-wake mask reject it.
	//
	// Prints the CENTER tile in full regardless, plus every tile in radius that carries a
	// floor-change or teleport flag. Anything else is omitted so the output stays readable.
	// Pure read-only: no tile, item or bot state is modified.
	if (command.substr(0, 7) == "tpscan ") {
		int px = 0, py = 0, pz = 0, radius = 8;
		const int parsed = std::sscanf(command.c_str() + 6, " %d,%d,%d %d", &px, &py, &pz, &radius);
		if (parsed < 3) {
			return "usage: tpscan <x>,<y>,<z> [radius=8]";
		}
		radius = std::max(0, std::min(radius, 15)); // bound the output
		const Position center(static_cast<uint16_t>(px), static_cast<uint16_t>(py), static_cast<uint8_t>(pz));

		// Same mask chooseWakePosition/chooseSafePartyFollowPos reject on (isUnsafeWakeTile,
		// bot_waypoint.cpp). Reported per tile so D1 is answerable directly from the output
		// rather than by eyeballing item ids against items.xml.
		constexpr uint32_t kUnsafeWakeMask =
			TILESTATE_BLOCKSOLID | TILESTATE_BLOCKPATH |
			TILESTATE_IMMOVABLEBLOCKSOLID | TILESTATE_IMMOVABLEBLOCKPATH |
			TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT |
			TILESTATE_DEPOT | TILESTATE_MAGICFIELD;

		auto describeTile = [&](const Position& p, bool isCenter) -> std::string {
			auto tile = g_game().map.getTile(p);
			if (!tile) {
				return isCenter ? fmt::format("  ({},{},{}) NO TILE (void)\n", p.x, p.y, p.z) : std::string();
			}
			const bool fc = tile->hasFlag(TILESTATE_FLOORCHANGE);
			const bool tp = tile->hasFlag(TILESTATE_TELEPORT);
			if (!isCenter && !fc && !tp) {
				return std::string();
			}

			std::string flags;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_DOWN)) flags += "FC_DOWN ";
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_NORTH)) flags += "FC_N ";
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH)) flags += "FC_S ";
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_EAST)) flags += "FC_E ";
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_WEST)) flags += "FC_W ";
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) flags += "FC_S_ALT ";
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) flags += "FC_E_ALT ";
			if (tp) flags += "TELEPORT ";
			if (tile->hasFlag(TILESTATE_PROTECTIONZONE)) flags += "PZ ";
			if (tile->hasFlag(TILESTATE_BLOCKSOLID)) flags += "BLOCKSOLID ";
			if (tile->hasFlag(TILESTATE_BLOCKPATH)) flags += "BLOCKPATH ";
			if (tile->hasFlag(TILESTATE_DEPOT)) flags += "DEPOT ";
			if (tile->hasFlag(TILESTATE_MAGICFIELD)) flags += "MAGICFIELD ";
			if (flags.empty()) flags = "-";

			std::string out = fmt::format("  ({},{},{}){} flags=[{}]\n",
				p.x, p.y, p.z, isCenter ? " <-- CENTER" : "", flags);

			// Destination is the whole point for D2 — presence alone doesn't tell us whether
			// the flame actually goes anywhere (an unset ATTR_TELE_DEST silently no-ops in
			// Teleport::addThing).
			if (const auto& teleport = tile->getTeleportItem()) {
				const Position& dest = teleport->getDestPos();
				const bool valid = (dest.x != 0 || dest.y != 0 || dest.z != 0);
				out += fmt::format("      teleport dest=({},{},{}) {}\n",
					dest.x, dest.y, dest.z,
					valid ? "VALID" : "*** UNSET (0,0,0) — Teleport::addThing silently no-ops ***");
			}

			if (const auto& ground = tile->getGround()) {
				const int32_t aid = static_cast<int32_t>(ground->getAttribute<uint16_t>(ItemAttribute_t::ACTIONID));
				out += fmt::format("      ground id={} '{}'{}\n",
					ground->getID(), ground->getName(),
					aid > 0 ? fmt::format(" aid={}{}", aid, g_moveEvents().hasActionId(aid) ? " [MOVEEVENT]" : "") : "");
			}
			if (const auto* items = tile->getItemList()) {
				for (const auto& item : *items) {
					const int32_t aid = static_cast<int32_t>(item->getAttribute<uint16_t>(ItemAttribute_t::ACTIONID));
					out += fmt::format("      item   id={} '{}'{}\n",
						item->getID(), item->getName(),
						aid > 0 ? fmt::format(" aid={}{}", aid, g_moveEvents().hasActionId(aid) ? " [MOVEEVENT]" : "") : "");
				}
			}
			if (g_moveEvents().hasPosition(p)) {
				out += "      [MOVEEVENT registered on this exact position]\n";
			}
			out += fmt::format("      unsafeWakeMask={} (a bot must never be TELEPORTED onto this tile if YES)\n",
				tile->hasFlag(kUnsafeWakeMask) ? "YES" : "no");
			return out;
		};

		std::string out = fmt::format("[TPSCAN] center=({},{},{}) radius={}\n", center.x, center.y, center.z, radius);
		out += describeTile(center, /*isCenter=*/true);
		uint32_t hits = 0;
		for (int dy = -radius; dy <= radius; ++dy) {
			for (int dx = -radius; dx <= radius; ++dx) {
				if (dx == 0 && dy == 0) continue; // already printed as CENTER
				const Position p(static_cast<uint16_t>(px + dx), static_cast<uint16_t>(py + dy),
					static_cast<uint8_t>(pz));
				std::string desc = describeTile(p, /*isCenter=*/false);
				if (!desc.empty()) {
					out += desc;
					hits++;
				}
			}
		}
		out += fmt::format("[TPSCAN] {} floor-change/teleport tile(s) within radius {}\n", hits, radius);
		g_logger().info("{}", out);
		return out;
	}

	// ---- botcfg: dump every BOT_NAV_REALISM tunable as actually loaded ----
	// Generated from the same X-macro table as the enumerators and the load calls, so this
	// listing cannot drift from what the server really registered. This is the check that a
	// key is wired end to end: an enumerator paired with the wrong Lua key, or a config.lua
	// entry that never reaches the engine, both show up here as a wrong/default value.
	// Why is a position considered in-town (and therefore NOT roam-suppressed)? The verdict is
	// three rules deep and the failure mode is silent — roam simply keeps running and nothing says
	// which rule decided that. Answers it directly rather than by inference.
	if (command.rfind("intown ", 0) == 0) {
		int x = 0, y = 0, z = 0;
		if (std::sscanf(command.c_str() + 7, "%d,%d,%d", &x, &y, &z) != 3) {
			return "usage: /cavebot _global intown <x>,<y>,<z>";
		}
		const Position p(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
		auto tile = g_game().map.getTile(p);
		const bool pz = tile && tile->hasFlag(TILESTATE_PROTECTIONZONE);

		std::string townLine = "  rule2 temple: no town within radius\n";
		const auto& walkZ = getCityWalkZ();
		for (const auto& [id, town] : g_game().map.towns.getTowns()) {
			if (id == 0 || !town) continue;
			const auto tp = town->getTemplePosition();
			const int32_t d = std::max(std::abs(x - static_cast<int32_t>(tp.x)),
			                           std::abs(y - static_cast<int32_t>(tp.y)));
			if (d > ROAM_TOWN_RADIUS) continue;
			auto wit = walkZ.find(id);
			const int32_t cityZ = (wit != walkZ.end()) ? static_cast<int32_t>(wit->second)
			                                          : static_cast<int32_t>(tp.z);
			townLine = fmt::format("  rule2 temple: town {} '{}' temple=({},{},{}) dist={} cityZ={} dz={} -> {}\n",
				id, town->getName(), tp.x, tp.y, tp.z, d, cityZ, z - cityZ,
				(z - cityZ == 0 || z - cityZ == -1) ? "IN TOWN" : ((z - cityZ == 1 && pz) ? "IN TOWN (pz)" : "no"));
			break;
		}

		int32_t npcCount = 0;
		std::string npcNames;
		for (const auto& [name, instances] : npcAnchors_) {
			for (const auto& a : instances) {
				if (a.npcPos.z != p.z) continue;
				const int32_t d = std::max(std::abs(x - static_cast<int32_t>(a.npcPos.x)),
				                           std::abs(y - static_cast<int32_t>(a.npcPos.y)));
				if (d <= ROAM_TOWN_NPC_RADIUS) {
					if (npcCount < 4) npcNames += fmt::format("{}@{} ", name, d);
					npcCount++;
				}
			}
		}
		return fmt::format("[INTOWN] ({},{},{}) verdict={}\n  rule1 pz: {}\n{}  rule3 npcs: {} within {} on z{} (need 2) {}\n"
			"  huntRepelPts={} playerHuntFlags={}",
			x, y, z, isInTownArea(p) ? "IN TOWN (roam allowed)" : "OUT OF TOWN (roam suppressed if flagged)",
			pz ? "yes -> IN TOWN" : "no",
			townLine, npcCount, ROAM_TOWN_NPC_RADIUS, z, npcNames,
			huntRepelPts_.size(), playerHuntEngaged_.size());
	}

	// ---- BOT_AMBIENT_ROAM observability ----
	if (command == "roam") {
		std::string out = buildRoamReport();
		g_logger().info("{}", out);
		return out;
	}

	// A synthetic anchor, so the feature is drivable with nobody logged in. This is a deliverable
	// rather than a convenience: an anchor is a real player or a cast-watched bot, neither of
	// which exists in a headless run, so without it not one line of roam can be exercised from
	// the bot_commands queue.
	if (command == "roamanchor off") {
		roamDebugAnchor_ = Position();
		return "[ROAM] debug anchor cleared";
	}
	if (command.rfind("roamanchor ", 0) == 0) {
		int x = 0, y = 0, z = 0;
		if (std::sscanf(command.c_str() + 11, "%d,%d,%d", &x, &y, &z) != 3
			|| x <= 0 || y <= 0 || z < 0 || z > 15) {
			return "usage: /cavebot _global roamanchor <x>,<y>,<z> | roamanchor off";
		}
		roamDebugAnchor_ = Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
		return fmt::format("[ROAM] debug anchor set to ({},{},{})", x, y, z);
	}

	// BOT_CORPSE_LOOT counters. Read-only; the acceptance signal is openedAdj+openedWalk
	// against the kill count, plus guardSuppressed staying non-zero whenever real players
	// are actually around (it proves the public-cleanup guard is live, not merely coded).
	if (command == "lootstats") {
		const uint64_t opened = s_lootStats.openedAdj + s_lootStats.openedWalk + s_lootStats.openedPublic;
		size_t liveRuns = 0, claimedNow = 0, blockedNow = 0;
		for (const auto& [guid, run] : s_lootRun) {
			if (run.hasTarget) liveRuns++;
			claimedNow += run.claimed.size();
			blockedNow += run.blocked.size();
		}
		return fmt::format(
			"[LOOT] enable={} walk={} publicCleanup={} radius={} windowMs={} scanMs={}\n"
			"[LOOT] opened={} (adj={} walk={} public={})\n"
			"[LOOT] census={} claimed={} walkArrived={} walkFail={} blacklisted={}\n"
			"[LOOT] guardSuppressed={} runsStarted={} walkDropped={}\n"
			"[LOOT] adjArmed={} adjCancelled={}\n"
			"[LOOT] live: runs={} states={} claimed={} blocked={}",
			lootCfg_.enable ? "true" : "false", lootCfg_.walk ? "true" : "false",
			lootCfg_.publicCleanup ? "true" : "false", lootCfg_.radius,
			lootCfg_.windowMs, lootCfg_.scanMs,
			opened, s_lootStats.openedAdj, s_lootStats.openedWalk, s_lootStats.openedPublic,
			s_lootStats.censusPasses, s_lootStats.claimed, s_lootStats.walkArrived,
			s_lootStats.walkFail, s_lootStats.blacklisted,
			s_lootStats.guardSuppressed, s_lootStats.runsStarted, s_lootStats.walkDropped,
			s_lootStats.adjArmed, s_lootStats.adjCancelled,
			liveRuns, s_lootRun.size(), claimedNow, blockedNow);
	}

	// ---- activity: the BOT_ACTIVITY_PCT tables, as configured and as realised ----
	// Answers the question the config file cannot: a percentage is what a bot ATTEMPTS,
	// and HUNT/PARTY are finite pools whose attempts routinely fail, so nominal and
	// realised legitimately differ. Showing both is what keeps the numbers honest.
	if (command == "activity") {
		return activityReport();
	}

	if (command == "botcfg") {
		std::string out = "[BOT_CFG] BOT_NAV_REALISM keys (src/config/bot_config_keys.hpp)\n";
#define BOT_CFG_DUMP_INT(enumKey, luaKey, defaultValue) \
	out += fmt::format("  {:<34} = {}\n", luaKey, g_configManager().getNumber(enumKey));
#define BOT_CFG_DUMP_BOOL(enumKey, luaKey, defaultValue) \
	out += fmt::format("  {:<34} = {}\n", luaKey, g_configManager().getBoolean(enumKey) ? "true" : "false");
#define BOT_CFG_DUMP_STR(enumKey, luaKey, defaultValue) \
	out += fmt::format("  {:<34} = \"{}\"\n", luaKey, g_configManager().getString(enumKey));
		BOT_NAV_REALISM_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += "[BOT_CFG] BOT_SUPPLY_REALISM keys (src/config/bot_config_keys.hpp)\n";
		BOT_ACTIVITY_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += "[BOT_CFG] BOT_MARKET_ACCEPT keys (src/config/bot_config_keys.hpp)\n";
		BOT_MARKET_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += "[BOT_CFG] BOT_PARTY_TRAIL keys (src/config/bot_config_keys.hpp)\n";
		BOT_PARTY_TRAIL_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += "[BOT_CFG] BOT_AMBIENT_ROAM keys (src/config/bot_config_keys.hpp)\n";
		BOT_AMBIENT_ROAM_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += "[BOT_CFG] BOT_LURE_KITE keys (src/config/bot_config_keys.hpp)\n";
		BOT_LURE_KITE_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += "[BOT_CFG] BOT_CORPSE_LOOT keys (src/config/bot_config_keys.hpp)\n";
		BOT_CORPSE_LOOT_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		out += fmt::format("[BOT_CFG] BOT_ACTIVITY_PCT keys{}\n", activityTableStatusSuffix());
		BOT_ACTIVITY_PCT_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
		// NOTE: this dump is a THIRD site the X-macro does not generate. The table produces the
		// ConfigKey_t enumerator and the configmanager load call automatically, but every table
		// has to be listed here by hand — so a new table loads correctly and is simply INVISIBLE
		// to `/cavebot botcfg` until it is added. That was missed once for BOT_SHRINE and the
		// symptom was exactly the failure this command exists to rule out: keys that are live in
		// the engine but unverifiable from the outside. Two of the keys below are TABLE B rows,
		// so they print here rather than with the other nine.
		out += "[BOT_CFG] BOT_SHRINE keys (src/config/bot_config_keys.hpp)\n";
		BOT_SHRINE_CONFIG_KEYS(BOT_CFG_DUMP_INT, BOT_CFG_DUMP_BOOL, BOT_CFG_DUMP_STR)
#undef BOT_CFG_DUMP_INT
#undef BOT_CFG_DUMP_BOOL
#undef BOT_CFG_DUMP_STR
		g_logger().info("{}", out);
		return out;
	}

	// ---- BOT_PARTY_INVITE_RENDEZVOUS debug/observability ----

	// reloadconfig: re-read config.lua at runtime (stock canary's own /reload config, which needs
	// a god character). Exposed here so the bot_commands queue can retune a switch headlessly
	// instead of forcing a full restart that would evict every logged-in player and bot.
	if (command == "reloadconfig") {
		const bool ok = g_configManager().reload();
		// The engine reads its tunables from 5s-refreshed caches, so force one now rather than
		// leaving the caller to guess whether the new values are live yet.
		refreshLivenessCfgIfStale(0); // 0 = ignore the 5s cadence, refresh right now
		return fmt::format("Config reload {}. botPartyInviteEnable={} botPartyRvEnable={} "
			"botPartyFollowDist={}",
			ok ? "OK" : "FAILED",
			g_configManager().getBoolean(BOT_PARTY_INVITE_ENABLE) ? "true" : "false",
			g_configManager().getBoolean(BOT_PARTY_RV_ENABLE) ? "true" : "false",
			g_configManager().getNumber(BOT_PARTY_FOLLOW_DIST));
	}

	// invites: dump the acceptance machine + every online leader's pending bot invitees.
	if (command == "invites") {
		std::string out = fmt::format("[PINVITE] pending={} (enable={} pollMs={})\n",
			s_pendingInvites.size(), inviteCfg_.enable ? "true" : "false", inviteCfg_.pollMs);
		const int64_t nowMs = OTSYS_TIME();
		for (const auto& [botGuid, pi] : s_pendingInvites) {
			auto it = guidToIndex_.find(botGuid);
			auto inviterCreature = g_game().getCreatureByID(pi.inviterCreatureId);
			auto inviter = inviterCreature ? inviterCreature->getPlayer() : nullptr;
			out += fmt::format("  bot='{}' inviter='{}' phase={} inMs={}\n",
				it != guidToIndex_.end() ? bots_[it->second].name : "?",
				inviter ? inviter->getName() : "(gone)",
				pi.phase == InvitePhase::HOLDING ? "HOLDING" : "ACCEPT_WAIT",
				pi.actAtMs - nowMs);
		}
		out += "[PINVITE] live invitee lists (leader -> bot invitees):\n";
		for (const auto& [id, p] : g_game().getPlayers()) {
			if (!p) continue;
			auto party = p->getParty();
			if (!party || party->getLeader() != p) continue;
			for (const auto& inv : party->getInvitees()) {
				if (inv && inv->isBotPlayer()) {
					out += fmt::format("  '{}' -> '{}'\n", p->getName(), inv->getName());
				}
			}
		}
		out += fmt::format("[PINVITE] counters: detected={} accepted={} declined(partyHunt={} "
			"holdExpired={} assembling={}) staleCleared={}\n",
			s_prv.detected, s_prv.accepted, s_prv.declinedPartyHunt,
			s_prv.declinedHoldExpired, s_prv.declinedAssembling, s_prv.staleCleared);
		g_logger().info("{}", out);
		return out;
	}

	// assemblies: dump the walk-in supervisor.
	if (command == "assemblies") {
		if (s_partyAssembly.empty()) {
			return fmt::format("No active assemblies. (rvEnable={} walked={} teleFallback={})",
				asmCfg_.enable ? "true" : "false", s_prv.asmWalked, s_prv.asmTeleFallback);
		}
		const int64_t nowMs = OTSYS_TIME();
		std::string out;
		for (const auto& [id, a] : s_partyAssembly) {
			std::string leaderName = "?";
			if (a.kind == RvKind::HUMAN_LED) {
				auto c = g_game().getCreatureByID(a.leaderCreatureId);
				if (c) leaderName = c->getName();
			} else if (auto it = guidToIndex_.find(a.leaderGuid); it != guidToIndex_.end()) {
				leaderName = bots_[it->second].name;
			}
			out += fmt::format("Assembly #{} kind={} leader='{}' anchor=({},{},{}) town={} ageMs={}\n",
				id, a.kind == RvKind::HUMAN_LED ? "human" : "bot", leaderName,
				a.anchor.x, a.anchor.y, a.anchor.z, a.anchorTownId, nowMs - a.startedMs);
			for (const auto& m : a.members) {
				auto it = guidToIndex_.find(m.guid);
				const char* ph = m.phase == RvPhase::FINISHING ? "FINISHING"
					: m.phase == RvPhase::TRAVELLING ? "TRAVELLING"
					: m.phase == RvPhase::WALKING_IN ? "WALKING_IN"
					: m.phase == RvPhase::ARRIVED ? "ARRIVED" : "FAILED";
				if (it != guidToIndex_.end()) {
					const auto& mb = bots_[it->second];
					out += fmt::format("  '{}' phase={} phaseAgeMs={} pos=({},{},{}) dist={} state={}\n",
						mb.name, ph, nowMs - m.phaseSinceMs,
						mb.currentPos.x, mb.currentPos.y, mb.currentPos.z,
						std::max(std::abs(static_cast<int32_t>(mb.currentPos.x) - static_cast<int32_t>(a.anchor.x)),
						         std::abs(static_cast<int32_t>(mb.currentPos.y) - static_cast<int32_t>(a.anchor.y))),
						botStateName(mb.state));
				}
			}
		}
		g_logger().info("{}", out);
		return out;
	}

	// ---- partyinfo: Show all active party hunts ----
	if (command == "partyinfo") {
		if (s_partyHuntMembers.empty()) {
			return "No active party hunts.";
		}
		std::string result;
		for (const auto& [huntId, members] : s_partyHuntMembers) {
			auto leaderIt = s_partyHuntLeaderGuid.find(huntId);
			auto scriptIt = s_partyHuntScript.find(huntId);
			std::string leaderName = "?";
			std::string scriptName = "?";
			if (leaderIt != s_partyHuntLeaderGuid.end()) {
				auto lidx = guidToIndex_.find(leaderIt->second);
				if (lidx != guidToIndex_.end()) {
					auto p = bots_[lidx->second].getPlayer();
					if (p) leaderName = p->getName();
				}
			}
			if (scriptIt != s_partyHuntScript.end()) {
				for (const auto& s : huntScripts_) {
					if (s.id == scriptIt->second) { scriptName = s.name; break; }
				}
			}
			auto dcIt = s_partyHuntDeathCount.find(huntId);
			auto kcIt = s_partyHuntKillCount.find(huntId);
			result += fmt::format("  Party #{}: leader={} script='{}' members={} kills={} deaths={}\n",
				huntId, leaderName, scriptName, members.size(),
				kcIt != s_partyHuntKillCount.end() ? kcIt->second : 0,
				dcIt != s_partyHuntDeathCount.end() ? dcIt->second : 0);
			for (uint32_t mguid : members) {
				auto midx = guidToIndex_.find(mguid);
				if (midx != guidToIndex_.end()) {
					auto& mb = bots_[midx->second];
					auto mp = mb.getPlayer();
					static const char* roleNames[] = {"none","TANK","HEALER","DPS_MAGE","DPS_RANGED"};
					const char* role = mb.partyRole < 5 ? roleNames[mb.partyRole] : "?";
					result += fmt::format("    {} (guid={}) role={} state={} pos=({},{},{})\n",
						mp ? mp->getName() : "?", mguid, role, botStateName(mb.state),
						mb.currentPos.x, mb.currentPos.y, mb.currentPos.z);
				}
			}
		}
		return fmt::format("Active party hunts ({}):\n{}", s_partyHuntMembers.size(), result);
	}

	// ---- partystop <name>: Dissolve a specific bot's party hunt ----
	if (command.size() > 10 && command.substr(0, 10) == "partystop ") {
		std::string targetName = command.substr(10);
		for (auto& bot : bots_) {
			auto p = bot.getPlayer();
			if (p && p->getName() == targetName) {
				if (bot.partyHuntId == 0) {
					return fmt::format("Bot '{}' is not in a party hunt.", targetName);
				}
				uint32_t phId = bot.partyHuntId;
				dissolvePartyHunt(phId, "admin_stop");
				return fmt::format("Dissolved party hunt #{} for bot '{}'.", phId, targetName);
			}
		}
		return fmt::format("Bot '{}' not found.", targetName);
	}

	// ---- whohunts <search>: Show which bot has a hunt script reserved ----
	if (command.substr(0, 8) == "whohunts") {
		std::string search = command.length() > 9 ? command.substr(9) : "";
		// Lowercase the search term
		std::transform(search.begin(), search.end(), search.begin(), ::tolower);

		std::string result;
		int matches = 0;
		for (const auto& [scriptId, botGuid] : activeHunts_) {
			// Find script name
			std::string scriptName;
			for (const auto& s : huntScripts_) {
				if (s.id == scriptId) { scriptName = s.name; break; }
			}
			// Filter by search term if provided
			if (!search.empty()) {
				std::string nameLower = scriptName;
				std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
				if (nameLower.find(search) == std::string::npos) continue;
			}
			// Find bot name
			std::string botPlayerName = "?";
			for (const auto& bot : bots_) {
				if (bot.guid == botGuid) {
					auto p = bot.getPlayer();
					if (p) botPlayerName = p->getName();
					break;
				}
			}
			result += fmt::format("  script {} '{}' -> {} (guid={})\n", scriptId, scriptName, botPlayerName, botGuid);
			matches++;
		}
		// Quests hold no reservation (they are shared under a cooldown), so activeHunts_ says
		// nothing about who is running one. Report them by bot STATE instead, or this command
		// is blind to exactly the scripts the quest work is meant to be verified with.
		int questMatches = 0;
		for (const auto& s : huntScripts_) {
			if (!(s.isQuest || s.scriptCategory == "quest")) continue;
			if (!search.empty()) {
				std::string nameLower = s.name;
				std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
				if (nameLower.find(search) == std::string::npos) continue;
			}
			std::string runners;
			for (const auto& b : bots_) {
				if (b.huntScriptId != s.id) continue;
				if (!runners.empty()) runners += ", ";
				auto p = b.getPlayer();
				runners += p ? p->getName() : fmt::format("guid={}(hibernated)", b.guid);
			}
			auto cdIt = s_questLastStart.find(s.id);
			const int64_t sinceSec = cdIt == s_questLastStart.end()
				? -1 : (OTSYS_TIME() - cdIt->second) / 1000;
			if (runners.empty() && sinceSec < 0) continue;
			result += fmt::format("  quest {} '{}' -> {} (lastStart={}s ago{})\n",
				s.id, s.name, runners.empty() ? "nobody" : runners,
				sinceSec < 0 ? 0 : sinceSec,
				botQuestOnCooldown(s.id) ? ", ON COOLDOWN" : "");
			questMatches++;
		}

		if (matches == 0 && questMatches == 0) {
			return search.empty() ? "No active hunt reservations." : fmt::format("No active hunts matching '{}'.", search);
		}
		return fmt::format("Active hunts ({} reserved, {} quests):\n{}", matches, questMatches, result);
	}

	// ---- claimspawn <creatureId> [name...]: a real player claims the spawn they stand in ----
	// Detects the hunt script at the player's position (or by name), kicks the bot reserving
	// it, and reserves it for the player for 1h (in-memory). Available to ALL real players.
	if (command.substr(0, 10) == "claimspawn") {
		std::string args = command.length() > 11 ? command.substr(11) : "";
		uint32_t cid = 0;
		std::string nameArg;
		{
			size_t sp = args.find(' ');
			std::string idTok = (sp == std::string::npos) ? args : args.substr(0, sp);
			try { cid = std::stoul(idTok); } catch (...) { return "Invalid claim request."; }
			if (sp != std::string::npos) {
				nameArg = args.substr(sp + 1);
				size_t b = nameArg.find_first_not_of(" \t");
				size_t e = nameArg.find_last_not_of(" \t");
				nameArg = (b == std::string::npos) ? "" : nameArg.substr(b, e - b + 1);
			}
		}

		auto creature = g_game().getCreatureByID(cid);
		auto player = creature ? creature->getPlayer() : nullptr;
		if (!player) return "Could not resolve your character.";
		if (player->isBotPlayer()) return "Bots cannot claim spawns.";
		uint32_t ownerGuid = player->getGUID();
		std::string ownerName = player->getName();

		// BOT_AMBIENT_ROAM: stamp "this player is hunting" HERE, before any of the resolution or
		// cooldown checks that can bail out below. Typing the command is the announcement; whether
		// the engine can map their spawn to a known script, or whether an anti-grief cooldown eats
		// this particular attempt, is our bookkeeping and should not decide whether ambient
		// strangers wander into the spawn they are working. Outside a town this suppresses roam
		// injection around them for the same hour a claim would last.
		markPlayerHuntEngaged(ownerGuid);

		// Anti-grief cooldown (only successful claims stamp it — checked before any work)
		int64_t now = OTSYS_TIME();
		auto cdIt = lastClaimByGuid_.find(ownerGuid);
		if (cdIt != lastClaimByGuid_.end() && now - cdIt->second < PLAYER_CLAIM_COOLDOWN_MS) {
			int secLeft = static_cast<int>((PLAYER_CLAIM_COOLDOWN_MS - (now - cdIt->second)) / 1000) + 1;
			return fmt::format("Please wait {}s before claiming another spawn.", secLeft);
		}

		// Resolve the target script — explicit name, or geometry from the player's position
		const HuntScript* target = nullptr;
		if (!nameArg.empty()) {
			std::string needle = nameArg;
			std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
			const HuntScript* exact = nullptr;
			std::vector<const HuntScript*> partial;
			for (const auto& s : huntScripts_) {
				if (s.scriptCategory != "hunt" || !s.enabled || s.patrolWaypoints.empty()) continue;
				std::string n = s.name;
				std::transform(n.begin(), n.end(), n.begin(), ::tolower);
				if (n == needle) { exact = &s; break; }
				if (n.find(needle) != std::string::npos) partial.push_back(&s);
			}
			if (exact) target = exact;
			else if (partial.size() == 1) target = partial[0];
			else if (partial.empty()) return fmt::format("No hunt spawn matches '{}' — nothing reserved, but you are now flagged as "
					"hunting for the next hour so ambient bots will stay away while you are outside a "
					"town. /cavebot release to clear.", nameArg);
			else {
				std::string list;
				for (size_t i = 0; i < partial.size() && i < 8; i++) list += fmt::format("  {}\n", partial[i]->name);
				return fmt::format("'{}' matches {} spawns — be more specific:\n{}", nameArg, partial.size(), list);
			}
		} else {
			int32_t bestDist = 0, runnerUpDist = 0;
			const HuntScript* runnerUp = nullptr;
			const HuntScript* best = detectClaimableScript(player->getPosition(), bestDist, runnerUp, runnerUpDist);
			if (!best || bestDist > PLAYER_CLAIM_MAX_DIST) {
				return "You are not standing in a recognized hunt spawn, so no spawn was reserved — but you are "
					   "now flagged as hunting for the next hour, so ambient bots will stay away while you "
					   "are outside a town. Use /cavebot release to clear it, or /cavebot claim <name> to "
					   "reserve a specific spawn.";
			}
			if (runnerUp && (runnerUpDist - bestDist) <= PLAYER_CLAIM_AMBIGUITY_SLACK) {
				return fmt::format("Ambiguous — you're between '{}' and '{}'. Use /cavebot claim <name>.", best->name, runnerUp->name);
			}
			target = best;
		}

		// Already claimed by a DIFFERENT player (direct script or spawnGroup sibling)?
		auto existing = playerClaims_.find(target->id);
		if (existing != playerClaims_.end() && now < existing->second.expiresAt && existing->second.guid != ownerGuid) {
			int minLeft = static_cast<int>((existing->second.expiresAt - now) / 60000) + 1;
			return fmt::format("'{}' is already claimed by {} (~{} min left).", target->name, existing->second.ownerName, minLeft);
		}
		if (!target->spawnGroup.empty()) {
			for (const auto& [sid, claim] : playerClaims_) {
				if (sid == target->id || now >= claim.expiresAt) continue;
				if (claim.spawnGroup == target->spawnGroup && claim.guid != ownerGuid) {
					int minLeft = static_cast<int>((claim.expiresAt - now) / 60000) + 1;
					return fmt::format("That spawn is already claimed by {} (~{} min left).", claim.ownerName, minLeft);
				}
			}
		}

		// One claim per player — release any previous claim this player holds (move semantics)
		for (auto it = playerClaims_.begin(); it != playerClaims_.end();) {
			if (it->second.guid == ownerGuid) it = playerClaims_.erase(it);
			else ++it;
		}

		// Record the claim FIRST, then kick — so abortHunt's recovery scan honors the claim (B1)
		PlayerSpawnClaim claim;
		claim.guid = ownerGuid;
		claim.ownerName = ownerName;
		claim.scriptId = target->id;
		claim.spawnGroup = target->spawnGroup;
		claim.expiresAt = now + PLAYER_CLAIM_DURATION_MS;
		playerClaims_[target->id] = claim;
		lastClaimByGuid_[ownerGuid] = now;

		std::string kickedName;
		bool kicked = kickSpawnHolder(*target, fmt::format("claimed by player {}", ownerName), kickedName);

		g_logger().info("[BotEngine] CLAIM: script {} '{}' by '{}' (guid={}) kicked='{}'",
			target->id, target->name, ownerName, ownerGuid, kicked ? kickedName : "");

		int durMin = static_cast<int>(PLAYER_CLAIM_DURATION_MS / 60000);
		if (kicked && !kickedName.empty())
			return fmt::format("Claimed '{}' for {} min. Kicked bot {} to temple.", target->name, durMin, kickedName);
		if (kicked)
			return fmt::format("Claimed '{}' for {} min. Cleared a stale reservation.", target->name, durMin);
		return fmt::format("Claimed '{}' for {} min. No bot was hunting it.", target->name, durMin);
	}

	// ---- releasespawn <creatureId>: owner releases their own claim (does NOT stamp cooldown) ----
	if (command.substr(0, 12) == "releasespawn") {
		std::string idTok = command.length() > 13 ? command.substr(13) : "";
		uint32_t cid = 0;
		try { cid = std::stoul(idTok); } catch (...) { return "Invalid release request."; }
		auto creature = g_game().getCreatureByID(cid);
		auto player = creature ? creature->getPlayer() : nullptr;
		if (!player) return "Could not resolve your character.";
		uint32_t ownerGuid = player->getGUID();
		// BOT_AMBIENT_ROAM: this is the explicit undo for the hunt flag as well. Without it a
		// player who mistyped a claim stays roam-suppressed for the full hour while the command
		// that exists to undo it silently does nothing — and since the flag is stamped even when
		// the claim fails to resolve, the mistyped case is precisely the one that needs a way out.
		playerHuntEngaged_.erase(ownerGuid);
		std::string names;
		int released = 0;
		for (auto it = playerClaims_.begin(); it != playerClaims_.end();) {
			if (it->second.guid == ownerGuid) {
				std::string sn;
				for (const auto& s : huntScripts_) { if (s.id == it->second.scriptId) { sn = s.name; break; } }
				if (!names.empty()) names += ", ";
				names += sn.empty() ? std::to_string(it->second.scriptId) : sn;
				it = playerClaims_.erase(it);
				released++;
			} else {
				++it;
			}
		}
		if (released == 0) return "You have no active spawn claim.";
		return fmt::format("Released your claim on {}.", names);
	}

	// ---- listclaims: show active player spawn-claims (god) ----
	if (command == "listclaims") {
		int64_t now = OTSYS_TIME();
		std::string out;
		int n = 0;
		for (auto it = playerClaims_.begin(); it != playerClaims_.end();) {
			if (now >= it->second.expiresAt) { it = playerClaims_.erase(it); continue; }
			std::string sn;
			for (const auto& s : huntScripts_) { if (s.id == it->second.scriptId) { sn = s.name; break; } }
			int minLeft = static_cast<int>((it->second.expiresAt - now) / 60000) + 1;
			out += fmt::format("  script {} '{}' -> {} (~{} min left)\n", it->second.scriptId, sn, it->second.ownerName, minLeft);
			n++;
			++it;
		}
		if (n == 0) return "No active player spawn-claims.";
		return fmt::format("Active player spawn-claims ({}):\n{}", n, out);
	}

	// ---- clearclaim <name>: admin force-release a player spawn-claim (god) ----
	if (command.substr(0, 10) == "clearclaim") {
		std::string nameArg = command.length() > 11 ? command.substr(11) : "";
		size_t b = nameArg.find_first_not_of(" \t");
		size_t e = nameArg.find_last_not_of(" \t");
		nameArg = (b == std::string::npos) ? "" : nameArg.substr(b, e - b + 1);
		if (nameArg.empty()) return "Usage: clearclaim <hunt name>";
		std::string needle = nameArg;
		std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
		int64_t now = OTSYS_TIME();
		std::vector<std::pair<uint32_t, std::string>> matches; // scriptId, name (currently-claimed only)
		for (const auto& [sid, claim] : playerClaims_) {
			if (now >= claim.expiresAt) continue;
			std::string sn;
			for (const auto& s : huntScripts_) { if (s.id == sid) { sn = s.name; break; } }
			std::string nl = sn;
			std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
			if (nl == needle || nl.find(needle) != std::string::npos) matches.push_back({sid, sn});
		}
		if (matches.empty()) return fmt::format("No active claim matches '{}'.", nameArg);
		if (matches.size() > 1) {
			std::string list;
			for (auto& m : matches) list += fmt::format("  {}\n", m.second);
			return fmt::format("'{}' matches {} claims — be more specific:\n{}", nameArg, matches.size(), list);
		}
		playerClaims_.erase(matches[0].first);
		return fmt::format("Cleared player claim on '{}'.", matches[0].second);
	}

	// ---- party create <leaderCreatureId> <vocList>: Create party with bots ----
	// `party createbot` is the identical path with the real-player requirement lifted, so the
	// production formation code can be driven headlessly (a bot stands in for the human).
	if (command.substr(0, 13) == "party create " || command.substr(0, 16) == "party createbot ") {
		const bool allowBotLeader = (command.substr(0, 16) == "party createbot ");
		std::string args = command.substr(allowBotLeader ? 16 : 13);
		// Parse: <leaderCreatureId> <vocList>
		auto spacePos = args.find(' ');
		if (spacePos == std::string::npos) {
			return "Usage: party create <leaderCreatureId> <vocList>";
		}
		uint32_t leaderCreatureId = 0;
		try { leaderCreatureId = std::stoul(args.substr(0, spacePos)); }
		catch (...) { return "Invalid leader creature ID."; }

		std::string vocListStr = args.substr(spacePos + 1);
		// Lowercase for case-insensitive parsing
		std::transform(vocListStr.begin(), vocListStr.end(), vocListStr.begin(), ::tolower);

		// BOT_PARTY_INVITE_RENDEZVOUS grammar:
		//   party create <id> <vocList> [<min>,<max>] [teleport]
		// Parsed HERE rather than in Lua because both talkactions (/party and /cavebot party)
		// pass their raw argument string through unchanged, so one C++ parse covers both — and
		// the level range feeds findBotsForParty, which is C++ anyway.
		uint32_t minLevelOverride = 0;
		uint32_t maxLevelOverride = 0;
		bool forceTeleport = false;

		// `teleport` — the escape hatch back to today's instant assembly.
		for (const char* kw : { " teleport", "\tteleport" }) {
			auto kwPos = vocListStr.rfind(kw);
			if (kwPos != std::string::npos && kwPos + std::strlen(kw) == vocListStr.size()) {
				forceTeleport = true;
				vocListStr.erase(kwPos);
				break;
			}
		}

		// `[min,max]` — an explicit window that REPLACES the level-derived one.
		if (auto lb = vocListStr.find('['); lb != std::string::npos) {
			auto rb = vocListStr.find(']', lb);
			auto comma = vocListStr.find(',', lb);
			if (rb == std::string::npos || comma == std::string::npos || comma > rb) {
				return "Bad level range. Use [min,max], e.g. /party ek,ed,ms [100,1500]";
			}
			try {
				minLevelOverride = std::stoul(vocListStr.substr(lb + 1, comma - lb - 1));
				maxLevelOverride = std::stoul(vocListStr.substr(comma + 1, rb - comma - 1));
			} catch (...) {
				return "Bad level range. Use [min,max], e.g. /party ek,ed,ms [100,1500]";
			}
			if (minLevelOverride == 0 || maxLevelOverride < minLevelOverride) {
				return fmt::format("Bad level range [{},{}] — min must be >= 1 and <= max.",
					minLevelOverride, maxLevelOverride);
			}
			vocListStr.erase(lb, rb - lb + 1);
		}
		// Trim whatever the removals left behind.
		while (!vocListStr.empty() && (vocListStr.back() == ' ' || vocListStr.back() == '\t')) {
			vocListStr.pop_back();
		}

		// Parse comma-separated vocation list
		std::vector<uint8_t> requestedVocs;
		std::stringstream ss(vocListStr);
		std::string token;
		while (std::getline(ss, token, ',')) {
			// Trim whitespace
			size_t s = token.find_first_not_of(" \t");
			size_t e = token.find_last_not_of(" \t");
			if (s == std::string::npos) continue;
			token = token.substr(s, e - s + 1);
			uint8_t voc = parseVocShorthand(token);
			if (voc == 0) {
				return fmt::format("Unknown vocation '{}'. Use: ek, ed, ms, rp.", token);
			}
			requestedVocs.push_back(voc);
		}

		if (requestedVocs.empty()) {
			return "No vocations specified. Use: ek, ed, ms, rp (comma-separated).";
		}
		// BOT_PARTY_INVITE_RENDEZVOUS: no size cap. Some quests need a full team, so the only
		// bound is how many eligible bots actually exist in the requested level range — the
		// reply below reports the shortfall rather than refusing up front.

		// Get leader player
		auto leaderCreature = g_game().getCreatureByID(leaderCreatureId);
		auto leaderPlayer = leaderCreature ? leaderCreature->getPlayer() : nullptr;
		if (!leaderPlayer) {
			return "Leader player not found.";
		}
		if (leaderPlayer->isBotPlayer() && !allowBotLeader) {
			return "Bot players cannot create parties.";
		}

		auto leaderPos = leaderPlayer->getPosition();
		uint32_t leaderLevel = leaderPlayer->getLevel();

		// Open tiles are only needed when we materialise everyone next to the leader. A walk-in
		// party needs no pre-cleared space, which is what makes a large quest party possible in
		// a temple doorway at all.
		const bool teleportAssembly = forceTeleport || !asmCfg_.enable;
		std::vector<Position> openTiles;
		if (teleportAssembly) {
			openTiles = findOpenTilesAround(leaderPos, static_cast<int32_t>(requestedVocs.size()));
			if (openTiles.size() < requestedVocs.size()) {
				return fmt::format("Not enough open space around you (need {} tiles, found {}). "
					"Drop the 'teleport' argument and they will walk to you instead.",
					requestedVocs.size(), openTiles.size());
			}
		}

		// Find bots for each requested vocation
		std::unordered_set<uint32_t> usedGuids;
		struct SelectedBot {
			uint32_t guid;
			uint8_t vocId;
		};
		std::vector<SelectedBot> selectedBots;

		for (uint8_t vocId : requestedVocs) {
			auto candidates = findBotsForParty(vocId, leaderLevel, 1, usedGuids,
				minLevelOverride, maxLevelOverride, /*preferActive=*/true);
			if (candidates.empty()) {
				// Undo any already-activated bots
				for (const auto& sel : selectedBots) {
					auto selIt = guidToIndex_.find(sel.guid);
					if (selIt != guidToIndex_.end()) {
						exitPartyMode(bots_[selIt->second]);
					}
				}
				return fmt::format("Could not find available {} within level range ({}-{}).",
					vocShortName(vocId), leaderLevel * 2 / 3, leaderLevel * 3 / 2);
			}
			usedGuids.insert(candidates[0]);
			selectedBots.push_back({candidates[0], vocId});
		}

		// Create party if leader doesn't have one
		auto party = leaderPlayer->getParty();
		if (!party) {
			party = Party::create(leaderPlayer);
			if (!party) {
				return "Failed to create party.";
			}
		}

		// Walk-in preparation: everything activateBotForParty does EXCEPT the summon teleport.
		// A hibernated recruit is woken with s_proximityWake TRUE, which is what makes
		// chooseWakePosition relocate it OFF-SCREEN from any observer near the leader — the whole
		// point being that nobody watches a party member pop into existence. It then walks in.
		auto prepareWalkInMember = [&](uint32_t guid) -> bool {
			auto it = guidToIndex_.find(guid);
			if (it == guidToIndex_.end()) return false;
			auto& mb = bots_[it->second];
			s_partyWasInactive[guid] = !mb.active;

			if (mb.hibernated) {
				s_forceWakeGuid = guid;   // bypass the density band for an explicit party wake
				s_proximityWake = true;   // ...but still wake OFF-SCREEN and walk in
				if (!wakeBot(guid)) return false;
			} else if (!mb.active) {
				if (!activateBot(guid)) return false;
			}
			auto mp = mb.getPlayer();
			if (!mp || mp->isRemoved()) return false;

			releasePartyMemberActivity(mb, "party_command");
			mp->health = mp->healthMax;
			mp->mana = mp->getMaxMana();
			g_game().addCreatureHealth(mp);
			g_game().addPlayerMana(mp);

			s_partyLeaderId[guid] = leaderCreatureId;
			s_partyPrevSecureMode[guid] = mp->secureMode;
			mp->setSecureMode(true);
			mb.state = BotAIState::IDLE;   // the assembly supervisor drives it from here
			mb.dwellUntil = 0;
			mb.activatedAt = 0;            // never let the 60s activation fallback teleport it
			return true;
		};

		// Activate bots and add to party
		std::string names;
		int32_t walkingIn = 0;
		for (size_t i = 0; i < selectedBots.size(); i++) {
			auto& sel = selectedBots[i];
			// CRASH FIX: openTiles is only populated for the teleport path. Indexing it in the
			// walk-in path read past the end of an EMPTY vector — undefined behaviour that
			// reported a formed party with no members and then segfaulted the server.
			if (teleportAssembly) {
				if (!activateBotForParty(sel.guid, leaderCreatureId, openTiles[i])) {
					continue;
				}
			} else if (!prepareWalkInMember(sel.guid)) {
				continue;
			}

			// Get the bot player to add to party
			auto botIt = guidToIndex_.find(sel.guid);
			if (botIt == guidToIndex_.end()) continue;
			auto& memberBot = bots_[botIt->second];
			auto botPlayer = memberBot.getPlayer();
			if (!botPlayer) continue;

			// Add to party (invite then join). Human-led parties join at commit time: the human is
			// watching the party list, and a party containing a real player is sweep-proof.
			party->invitePlayer(botPlayer);
			if (!party->joinParty(botPlayer)) continue;

			if (!teleportAssembly) {
				enrollHumanLedMember(memberBot, leaderPlayer);
				walkingIn++;
			}

			if (!names.empty()) names += ", ";
			names += botPlayer->getName();
			names += " (";
			names += vocShortName(sel.vocId);
			names += ")";
		}

		if (names.empty()) {
			return "Could not bring any bot into the party (all candidates failed to wake/join).";
		}

		// Enable shared experience
		party->setSharedExperience(leaderPlayer, true);

		g_logger().info("[BotEngine] Party created by '{}' with {} bots: {}",
			leaderPlayer->getName(), selectedBots.size(), names);

		// Shared exp cannot activate across a wide requested range — say so rather than let it
		// look broken. Canary's window is ceil(highest/mult) .. floor(lowest*mult).
		std::string shareWarn;
		if (minLevelOverride > 0 && maxLevelOverride > 0) {
			const float mult = g_configManager().getFloat(PARTY_SHARE_RANGE_MULTIPLIER);
			if (mult > 0.0f && static_cast<float>(maxLevelOverride) / mult
			    > static_cast<float>(minLevelOverride) * mult) {
				shareWarn = fmt::format(" Note: shared exp needs all levels within a x{:.1f} band, "
					"so [{},{}] is too wide — the party forms but shared exp stays inactive.",
					mult, minLevelOverride, maxLevelOverride);
			}
		}
		if (walkingIn > 0) {
			return fmt::format("Party formed — {} bot(s) on their way (they wake off-screen and "
				"walk to you): {}.{} Say '/cavebot party leave' to dismiss.",
				walkingIn, names, shareWarn);
		}
		return fmt::format("Party formed! Bots: {}.{} Say '/cavebot party leave' to dismiss.",
			names, shareWarn);
	}

	// ---- party leave <leaderCreatureId>: Dismiss all party bots ----
	if (command.substr(0, 12) == "party leave ") {
		uint32_t leaderCreatureId = 0;
		try { leaderCreatureId = std::stoul(command.substr(12)); }
		catch (...) { return "Invalid leader creature ID."; }

		int32_t dismissed = 0;
		// Find all bots following this leader
		std::vector<uint32_t> toExit;
		for (const auto& [guid, leaderId] : s_partyLeaderId) {
			if (leaderId == leaderCreatureId) {
				toExit.push_back(guid);
			}
		}

		for (uint32_t guid : toExit) {
			auto it = guidToIndex_.find(guid);
			if (it != guidToIndex_.end()) {
				exitPartyMode(bots_[it->second]);
				dismissed++;
			}
		}

		if (dismissed > 0) {
			g_logger().info("[BotEngine] Party dismissed: {} bots by leader creature {}",
				dismissed, leaderCreatureId);
			return fmt::format("Dismissed {} party bot(s).", dismissed);
		}
		return "No party bots found.";
	}

	// Hibernation per-bot commands: handled before the live-player loop so they work
	// even when the bot is hibernated (no Player in g_game()).
	if (command == "hibernate" || command == "wake") {
		for (auto& bot : bots_) {
			if (bot.name != botName) continue;
			if (command == "hibernate") {
				if (bot.hibernated) return fmt::format("Bot '{}' is already hibernated.", botName);
				return hibernateBot(bot.guid)
					? fmt::format("Bot '{}' hibernated.", botName)
					: fmt::format("Bot '{}' could not be hibernated (in-flight death?).", botName);
			} else { // wake
				if (!bot.hibernated) return fmt::format("Bot '{}' is already awake.", botName);
				// Explicit admin wake: bypass the density gate (single-shot flag).
				s_forceWakeGuid = bot.guid;
				s_proximityWake = false;  // EXPLICIT /cavebot wake — no off-screen relocation/sparkle
				return wakeBot(bot.guid)
					? fmt::format("Bot '{}' woken at ({},{},{}).", botName,
						bot.currentPos.x, bot.currentPos.y, bot.currentPos.z)
					: fmt::format("Bot '{}' could not be woken (DB load failed).", botName);
			}
		}
		return fmt::format("Bot '{}' not found.", botName);
	}

	// Hibernated-bot handling: status returns a brief preserved-state summary; partyhunt
	// is allowed because tryStartPartyHunt handles waking internally; other commands
	// require a wake first (live Player needed for navigation/combat ops).
	for (auto& bot : bots_) {
		if (bot.name != botName) continue;
		if (bot.hibernated) {
			// PERF HARNESS: `probe on` is allowed through for the same reason partyhunt is --
			// it handles the wake itself. Waking here and BREAKING (rather than returning) hands
			// the now-awake bot to the normal per-bot handler below, which does the actual
			// arming. Without this the harness could never arm a probe at all: at steady state
			// essentially every bot is hibernated, which is precisely when the command is needed.
			if (command == "probe on" || command == "probe watch on") {
				s_forceWakeGuid = bot.guid;   // explicit admin wake: bypass the density gate
				s_proximityWake = false;      // no off-screen relocation or login sparkle
				if (!wakeBot(bot.guid)) {
					return fmt::format("Bot '{}' could not be woken to act as a probe (DB load failed).",
						botName);
				}
				break;
			}
			// `probe off` and `probe status` need no live Player -- answer them here rather than
			// forcing a pointless wake.
			if (command == "probe off") {
				s_probeBots.erase(bot.guid);
				s_debugPinned.erase(bot.guid);
				if (auto pl = bot.getPlayer()) { pl->setSyntheticCastViewers(0); }
				return fmt::format("Bot '{}' probe OFF (was hibernated).", botName);
			}
			if (command == "probe status") {
				return fmt::format("Bot '{}' HIBERNATED, probe={} pinned={}", botName,
					s_probeBots.count(bot.guid) ? "yes" : "no",
					s_debugPinned.count(bot.guid) ? "yes" : "no");
			}
			if (command == "status") {
				return fmt::format("Bot '{}' is HIBERNATED at ({},{},{}). Preserved: state={} hunt=[script={} phase={} kills={}] partyHuntId={}. Use 'wake' to resume.",
					botName, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					botStateName(bot.state),
					bot.huntScriptId, static_cast<int>(bot.huntPhase), bot.huntKillCount,
					bot.partyHuntId);
			}
			if (command == "partyhunt" || (command.size() > 10 && command.substr(0, 10) == "partyhunt ")) {
				// Hibernated initiator → tryStartPartyHunt dispatches to virtualTryStartPartyHunt.
				// The party forms virtually (no wake). For admin/spectator use we then call
				// wakeBot to trigger the cascade and make the team visible.
				if (bot.partyHuntId > 0) {
					return fmt::format("Bot '{}' already in party hunt #{}.", botName, bot.partyHuntId);
				}
				int32_t forceScriptId = -1; // BOT_PARTY_CAP: -1 = "admin, no script" so the cap bypass can tell an
				                     // operator command from an autonomous roll (which passes 0).
				                     // Downstream consumers all test > 0, so -1 behaves as "none".
				if (command.size() > 10) {
					try { forceScriptId = std::stoi(command.substr(10)); } catch (...) {}
				}
				if (tryStartPartyHunt(bot, forceScriptId)) {
					uint32_t partyId = bot.partyHuntId;
					uint32_t scriptId = bot.huntScriptId;
					// Trigger wake cascade so the operator can immediately spectate.
					// Explicit wake — arm the density-gate bypass (the EK is normally
					// hibernated AT its hunt area, >midRadius from every anchor, so the
					// band rule would deterministically gate this and the team would
					// never materialize while the message claimed "woken").
					bool spectateWoken = true;
					if (bot.hibernated) {
						s_forceWakeGuid = bot.guid;
						s_proximityWake = false;  // EXPLICIT spectate wake — operator wants the EK now, not walking in
						spectateWoken = wakeBot(bot.guid);
					}
					return fmt::format("Bot '{}' started party hunt #{} (script={}{}).",
						botName, partyId, scriptId,
						spectateWoken ? ", woken for spectating" : ", WAKE FAILED — use '/cavebot <name> wake'");
				}
				return fmt::format("Bot '{}' failed to start party hunt (no ED available or no eligible scripts).", botName);
			}
			return fmt::format("Bot '{}' is hibernated. Use '/cavebot {} wake' first.", botName, botName);
		}
		break;
	}

	for (auto& bot : bots_) {
		auto player = bot.getPlayer();
		if (!player || player->getName() != botName) continue;

		const char* stateStr = botStateName(bot.state);

		// ---- debug ...: per-bot debug stream control ----
		// debug on | debug off | debug snapshot <ms> | debug grid on|off | debug events on|off | debug status
		if (command == "debug" || command.substr(0, 6) == "debug ") {
			std::string rest = command.size() > 6 ? command.substr(6) : "";
			return dbgHandleCommand(&bot, rest);
		}

		// ---- BOT_SUPPLY_REALISM manual triggers ----
		// The automatic paths are chance- and interval-gated, which makes them slow to observe on
		// purpose. These fire the same code with those two gates skipped, so a behaviour can be
		// exercised on demand. Everything that would make the action ILLEGAL still applies (item
		// missing, spell on cooldown, bot hunting), and the reason comes back in the reply rather
		// than failing silently.
		// ---- house [<houseId>] / houseinfo [<houseId>|near] ----
		// Same role `fish` and `advstone dummy` play: exercise the feature deterministically while
		// the population-wide roll is still 0. An explicit id skips the sampler, which is how a
		// specific interior (one with a known dummy, one with a known locker) gets tested — and
		// houseinfo is what confirms the harvest actually saw that furniture, including whether a
		// house's own door is one the planner treats as key-locked.
		if (command == "house" || command.rfind("house ", 0) == 0) {
			const size_t sp = command.find(' ');
			return forceHouseVisit(bot, sp == std::string::npos ? "" : command.substr(sp + 1));
		}
		if (command == "houseinfo" || command.rfind("houseinfo ", 0) == 0) {
			const size_t sp = command.find(' ');
			return describeHouseInterior(bot, sp == std::string::npos ? "" : command.substr(sp + 1));
		}
		if (command == "shrine" || command.rfind("shrine ", 0) == 0) {
			const size_t sp = command.find(' ');
			return forceShrineVisit(bot, sp == std::string::npos ? "" : command.substr(sp + 1));
		}
		if (command == "fish") {
			std::string msg;
			return forceFishingTrip(bot, msg)
				? fmt::format("Bot '{}' {}.", bot.name, msg)
				: fmt::format("Bot '{}' cannot fish: {}.", bot.name, msg);
		}
		// Ice fishing is waypoint-driven (a `fish:` extra_data marker on a hunt stand), so unlike
		// `fish` there is no spot index to consult. Two forms:
		//   fishice            work the ice next to the bot
		//   fishice waypoint   jump to the nearest `fish:` waypoint and work that
		// Both run until the hole closes, so one call shows the whole pick -> cast -> transform
		// cycle. `icefish` is accepted as an alias — both orderings get typed.
		if (command == "fishice" || command == "icefish"
		    || command.rfind("fishice ", 0) == 0 || command.rfind("icefish ", 0) == 0) {
			const size_t sp = command.find(' ');
			const std::string arg = (sp == std::string::npos) ? "" : command.substr(sp + 1);
			if (!arg.empty() && arg != "waypoint" && arg != "wp") {
				return fmt::format("Usage: fishice [waypoint] — got '{}'", arg);
			}
			return fmt::format("Bot '{}' {}.", bot.name,
				forceIceFish(bot, /*useWaypoint=*/!arg.empty()));
		}
		if (command == "eat") {
			std::string msg = "unknown";
			return tryEatFood(bot, /*force=*/true, &msg)
				? fmt::format("Bot '{}' {}.", bot.name, msg)
				: fmt::format("Bot '{}' did not eat: {}.", bot.name, msg);
		}
		// `potion` defaults to mana (the common test); `potion health` for the other kind.
		if (command == "potion" || command.substr(0, 7) == "potion ") {
			const bool wantHealth = command.size() > 7 && command.substr(7) == "health";
			std::string msg = "unknown";
			return tryDrinkPotion(bot, /*force=*/true, /*preferMana=*/!wantHealth, &msg)
				? fmt::format("Bot '{}' {}.", bot.name, msg)
				: fmt::format("Bot '{}' did not drink: {}.", bot.name, msg);
		}
		if (command == "rune") {
			std::string msg = "unknown";
			return tryCraftRune(bot, /*force=*/true, &msg)
				? fmt::format("Bot '{}' conjured: {}.", bot.name, msg)
				: fmt::format("Bot '{}' did not conjure: {}.", bot.name, msg);
		}
		// `support` forces one ambient cast; `supportlist` shows the whole pool and why each
		// entry is or is not castable right now. The forced cast still refuses while hunting —
		// that exclusion is the feature's scope, not pacing — so a "did not cast" answer on a
		// hunting bot is correct, not a bug.
		if (command == "support") {
			std::string msg = "unknown";
			return trySupportSpell(bot, /*force=*/true, &msg)
				? fmt::format("Bot '{}' cast: {}.", bot.name, msg)
				: fmt::format("Bot '{}' did not cast: {}.", bot.name, msg);
		}
		// Whole fishing run in one line, including the self-defense sub-state. This is the
		// acceptance surface for "does it fight back and then go back to fishing" — the phase,
		// the target it is fighting and the combat time owed back are otherwise invisible.
		// ---- BOT_AMBIENT_ROAM per-bot debug ----
		if (command == "roamcast on" || command == "roamcast off" || command == "roamcast status") {
			if (command == "roamcast on") roamCastMuted_.erase(bot.guid);
			else if (command == "roamcast off") roamCastMuted_.insert(bot.guid);
			return fmt::format("Bot '{}': roam cast digest {}.", bot.name,
				roamCastMuted_.count(bot.guid) ? "OFF (muted)" : "ON");
		}
		if (command == "roam status") {
			auto rit = s_roam.find(bot.guid);
			if (rit == s_roam.end()) {
				return fmt::format("Bot '{}': no roam session (ledgered={}).",
					bot.name, s_roamLedger.count(bot.guid) ? "yes" : "no");
			}
			const auto& r = rit->second;
			return fmt::format("Bot '{}': roam {} legs={} dest=({},{},{}) home=({},{},{}) "
				"age={}s fails={}{}{}",
				bot.name, r.phase == RoamPhase::WALKING ? "WALKING" : "DWELLING", r.legs,
				r.dest.x, r.dest.y, r.dest.z, r.homePos.x, r.homePos.y, r.homePos.z,
				(OTSYS_TIME() - r.startedMs) / 1000, r.failStreak,
				r.suspended ? " SUSPENDED" : "", r.defendTargetId ? " FIGHTING" : "");
		}
		if (command == "roam end") {
			if (!isRoaming(bot.guid)) return fmt::format("Bot '{}': not roaming.", bot.name);
			endRoamSession(bot, RoamEnd::INVARIANT);
			return fmt::format("Bot '{}': roam session ended.", bot.name);
		}
		if (command == "roamregion") {
			// Region is keyed on the anchor, not on the bot — report the one this bot would use.
			Position anchor = roamDebugAnchor_;
			int32_t best = anchor.x > 0 ? std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(anchor.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(anchor.y))) : INT32_MAX;
			for (const auto& a : roamAnchorPts_) {  // must match live roam targeting, not the camera list
				const int32_t d = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(a.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(a.y)));
				if (d < best) { best = d; anchor = a; }
			}
			if (anchor.x == 0) return "[ROAM] no anchor (set one with /cavebot _global roamanchor)";
			const auto* reg = getRoamRegion(anchor);
			if (!reg) return fmt::format("[ROAM] no region at ({},{},{}) — barren, or the "
				"one-cold-build-per-tick budget deferred it; retry", anchor.x, anchor.y, anchor.z);
			std::map<int, int> perZ;
			for (const auto& p : reg->dests) perZ[p.z]++;
			std::string zs;
			for (auto& [z, n] : perZ) zs += fmt::format(" z{}={}", z, n);
			return fmt::format("[ROAM] region anchor=({},{},{}) dests={} cells={} portals={} "
				"build={}ms cache={}/{} floors:{}",
				anchor.x, anchor.y, anchor.z, reg->dests.size(), reg->cells, reg->portals,
				reg->buildMs, roamReachHits_, roamReachMisses_, zs);
		}

		if (command == "fishinfo") {
			auto fit = s_fishing.find(bot.guid);
			if (fit == s_fishing.end()) {
				return fmt::format("Bot '{}': no fishing run.", bot.name);
			}
			const auto& run = fit->second;
			const int64_t now = OTSYS_TIME();
			const char* phase = run.phase == FishPhase::TRAVEL ? "TRAVEL"
				: (run.phase == FishPhase::FISHING ? "FISHING" : "RETURNING");
			std::string tgt = "none";
			if (run.defendTargetId != 0) {
				auto c = g_game().getCreatureByID(run.defendTargetId);
				tgt = fmt::format("{} (id={}, {}s in)", c ? c->getName() : "gone",
					run.defendTargetId, (now - run.defendSinceMs) / 1000);
			}
			return fmt::format(
				"Bot '{}': fish phase={} casts={} left={}s stand=({},{},{}) water=({},{},{}) | "
				"defend target={} kills={} owed={}s",
				bot.name, phase, run.casts, (run.until - now) / 1000,
				run.stand.x, run.stand.y, run.stand.z, run.water.x, run.water.y, run.water.z,
				tgt, run.defendKills, run.defendOwedMs / 1000);
		}
		if (command == "supportlist") {
			const std::string out = describeSupportPool(bot);
			g_logger().info("{}", out);
			return out;
		}
		// Reports the idle clock the rune gate reads. `rune` bypasses that gate, so without this
		// there is no way to see WHY an unforced conjure is not happening.
		if (command == "idleclock") {
			const auto it = s_idleStationary.find(bot.guid);
			const bool running = it != s_idleStationary.end() && it->second.since != 0;
			return fmt::format("Bot '{}': idle={} for {}ms, fishing={}, rune gate {} (needs 10000ms idle or fishing)",
				bot.name, running ? "yes" : "no",
				running ? OTSYS_TIME() - it->second.since : 0,
				isAmbientFishing(bot.guid) ? "yes" : "no",
				(isAmbientFishing(bot.guid) || isIdleInPlaceFor(bot, 10000)) ? "OPEN" : "CLOSED");
		}

		// ---- status: Full state report ----
		if (command == "status") {
			static const char* huntPhaseNames[] = { "PREPARING", "TRAVEL_TO", "PATROLLING", "LEAVING", "RESUPPLYING" };
			std::string huntInfo = "none";
			if (bot.huntScriptId > 0) {
				auto hpIdx = static_cast<uint8_t>(bot.huntPhase);
				const char* hpStr = hpIdx < 5 ? huntPhaseNames[hpIdx] : "?";
				std::string scriptName;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { scriptName = s.name; break; }
				}
				huntInfo = fmt::format("script={} '{}' phase={} kills={} wp={}", bot.huntScriptId,
					scriptName, hpStr, bot.huntKillCount, bot.huntWaypointIdx);
			}
			return fmt::format("Bot '{}': state={} pos=({},{},{}) active={} town={}({}) voc={} hp={}/{} mana={}/{} hunt=[{}] target={} fc={} travel={}",
				botName, stateStr,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				bot.active ? "yes" : "no",
				bot.townName.empty() ? "none" : bot.townName, bot.townId, bot.vocationId,
				player->getHealth(), player->getMaxHealth(),
				player->getMana(), player->getMaxMana(),
				huntInfo,
				bot.huntTargetId > 0 ? "yes" : "no",
				static_cast<int>(bot.fcState),
				bot.travelDestTownId > 0 ? fmt::format("town={} phase={}", bot.travelDestTownId, bot.travelPhase) : "none");
		}

		// ---- pos: Short position report ----
		if (command == "pos") {
			return fmt::format("Bot '{}': ({},{},{}) state={}", botName,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, stateStr);
		}

		// ---- endhunt: force hunt-time expiration so PATROLLING transitions to LEAVING
		// (TEMP debug command — used to test hibernate/wake handoff in LEAVING/RESUPPLYING
		// phases without waiting 30-180 min for natural hunt expiration)
		//
		// NO LONGER IMMEDIATE for an ordinary hunt. Since 2026-08-05 doHuntPatrol only acts on the
		// clock once the patrol lap has closed (huntWaypointIdx >= patrolWaypoints.size()), so this
		// marks the hunt ready to leave and the transition happens at the end of the current lap.
		// Deliberately NOT jumping the cursor to the lap end here: that would enter travel_from with
		// the bot standing somewhere else, which is precisely the bug the lap-boundary change fixes,
		// reintroduced inside the debug path. Quests still transition on the next tick.
		if (command == "endhunt") {
			if (bot.huntScriptId == 0) return fmt::format("Bot '{}' is not hunting.", botName);
			bot.huntEndTime = OTSYS_TIME() - 1;
			const HuntScript* ehScript = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { ehScript = &s; break; }
			}
			if (botScriptIsQuest(ehScript)) {
				return fmt::format("Bot '{}' huntEndTime forced to past — quest, so next tick transitions to LEAVING.", botName);
			}
			const size_t ehTotal = ehScript ? ehScript->patrolWaypoints.size() : 0;
			return fmt::format(
				"Bot '{}' marked ready to leave (huntEndTime forced to past). Ordinary hunt: transitions "
				"to LEAVING at the end of the current patrol lap — currently wp {}/{}.",
				botName, bot.huntWaypointIdx + 1, ehTotal);
		}

		// ---- lure: inspect, or force-arm, BOT_LURE_KITE for one bot ----
		// `lure`      — dump live lure + kite state
		// `lure <n>`  — session-only min_monsters override (0 disables for this bot)
		// `lure off`  — drop the override, back to whatever the script says
		// The override exists so a spawn can be tested without editing authored data;
		// it lives in a side map, so /cavebot reload clears it.
		if (command == "lure" || command.rfind("lure ", 0) == 0) {
			if (command.size() > 5) {
				const std::string arg = command.substr(5);
				if (arg == "off") {
					s_lureOverride.erase(bot.guid);
					clearLureRun(bot.guid);
					return fmt::format("Bot '{}': lure override cleared.", botName);
				}
				int n = -1;
				if (std::sscanf(arg.c_str(), "%d", &n) != 1 || n < 0 || n > 20) {
					return "usage: /cavebot <bot> lure [<0-20>|off]";
				}
				s_lureOverride[bot.guid] = static_cast<uint8_t>(n);
				clearLureRun(bot.guid); // re-arm against the new number on the next tick
				return fmt::format(
					"Bot '{}': lure override = {} (botLureEnable={}). {}",
					botName, n, lureCfg_.enable ? "true" : "false",
					n == 0 ? "0 disables luring for this bot."
					       : "Arms on the next PATROLLING tick, level gate bypassed.");
			}
			const HuntScript* lScript = nullptr;
			for (const auto& sc : huntScripts_) {
				if (sc.id == bot.huntScriptId) { lScript = &sc; break; }
			}
			std::string out = fmt::format("[LURE] {} script={} enable={} want={}\n",
				botName, bot.huntScriptId, lureCfg_.enable ? "true" : "false",
				effectiveMinMonsters(bot, lScript));
			if (lScript) {
				out += fmt::format("  script min_monsters={} min_level={} category={} quest={}\n",
					lScript->minMonsters, lScript->levelMin, lScript->scriptCategory,
					lScript->isQuest ? 1 : 0);
			}
			auto lp = bot.getPlayer();
			out += fmt::format("  eligible={} level={} partyLeader={} override={}\n",
				lureEligible(bot, lScript, lp) ? "YES" : "no",
				lp ? lp->getLevel() : 0,
				(bot.isPartyHuntLeader && bot.partyHuntId > 0) ? "yes" : "no",
				s_lureOverride.count(bot.guid) ? std::to_string(s_lureOverride[bot.guid]) : "none");
			const auto lIt = s_lure.find(bot.guid);
			if (lIt == s_lure.end() || lIt->second.phase == LurePhase::Off) {
				out += "  state: not running\n";
			} else {
				const auto& r = lIt->second;
				out += fmt::format("  state: {} pack={} peak={} supportAggro={} elapsed={}s "
					"engagements={} lastTrigger={} hold={}\n",
					r.phase == LurePhase::Luring ? "LURING" : "ENGAGING",
					r.count, r.peak, r.supportAggro,
					r.startMs > 0 ? (OTSYS_TIME() - r.startMs) / 1000 : 0,
					r.engagements, r.lastTrigger,
					r.holdUntilMs > 0 ? "yes" : "no");
			}
			const auto kIt = s_kite.find(bot.guid);
			if (kIt == s_kite.end() || !kIt->second.active) {
				out += fmt::format("  kite: idle (enable={}{})\n",
					lureCfg_.kiteEnable ? "true" : "false",
					(kIt != s_kite.end() && kIt->second.cooldownUntilMs > OTSYS_TIME())
						? fmt::format(", cooldown {}ms", kIt->second.cooldownUntilMs - OTSYS_TIME())
						: "");
			} else {
				const auto& k = kIt->second;
				out += fmt::format("  kite: cursor={} window={}..{} dir={} legs={} elapsed={}s\n",
					k.cursor, k.minIdx, k.anchorIdx, static_cast<int>(k.dir), k.legs,
					k.startMs > 0 ? (OTSYS_TIME() - k.startMs) / 1000 : 0);
			}
			return out;
		}

		// ---- stop: Halt everything ----
		if (command == "stop") {
			if (bot.huntScriptId > 0) {
				// Release hunt reservation directly — don't use abortHunt which tries recovery routes
				// and can re-reserve the script via findNearestRecoveryRoute
				s_leavingPhaseStart.erase(bot.guid);
				s_leavingWpTimer.erase(bot.guid);
				s_huntTravelStart.erase(bot.guid);
				activeHunts_.erase(bot.huntScriptId);
				for (auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
						activeSpawnGroups_.erase(s.spawnGroup);
						break;
					}
				}
				if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
					dissolvePartyHunt(bot.partyHuntId, "stop_command");
				}
				castLogError(bot, fmt::format("HUNT STOP: script={} kills={}", bot.huntScriptId, bot.huntKillCount));
				trackNavEvent("hunt_abort", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
					bot.townId, "hunt", fmt::format("reason=stop command kills={}", bot.huntKillCount));
				teleportToTemple(bot);
				bot.huntScriptId = 0;
				bot.huntKillCount = 0;
				bot.huntIgnoredMonsters.clear();
				bot.recoveryWaypoints.clear();
				bot.isRecoveryRoute = false;
			}
			bot.state = bot.active ? BotAIState::IDLE : BotAIState::INACTIVE;
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			// `stop` abandons the current walk AND any active fishing session, so release both
			// claims with it. clearFishingRun is a superset of clearPlannerWalk; the narrower call
			// left a session orphaned here, because only doDwelling ticks one and `stop` forces
			// IDLE.
			clearFishingRun(bot.guid);
			endHouseVisit(bot.guid, "stop"); // same reasoning: only doDwelling ticks a visit
			endShrineVisit(bot.guid, "stop");
			bot.hasDepotTarget = false;
			bot.returningHome = false;
			bot.attackerId = 0;
			bot.pkTarget = 0;
			bot.ignoredAttackerId = 0;
			bot.ignoredHitBack = false;
			bot.combatDecision.clear();
			bot.combatStartTime = 0;
			bot.lastCombatProgress = 0;
			bot.pvpManaSpent = 0;
			bot.huntTargetId = 0;
			bot.travelDestTownId = 0;
			bot.travelPhase.clear();
			bot.pendingHuntAfterTravel = false;
			bot.travelDestVerified = false;
			bot.triedRouteSources.clear();
			bot.lastRouteDestination.clear();
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			bot.pathFailCount = 0;
			bot.stopCooldownUntil = OTSYS_TIME() + 300000; // 5 min cooldown — no auto hunt/travel
			resetFloorChange(bot);
			// Adventurer's Stone trip cleanup
			if (bot.advStoneActive) {
				castLog(bot, "STOP: aborting Adventurer's Stone trip");
				endAdventurerStoneTrip(bot);
			}
			// Clear engine-level targets so the player stops auto-attacking and following
			player->setAttackedCreature(nullptr);
			player->setFollowCreature(nullptr);
			player->stopWalk();
			return fmt::format("Bot '{}' stopped (5 min cooldown, use 'resume' to cancel).", botName);
		}

		// ---- advstone [chest|dummy [<weaponId>]|waypoint]: Manually start an Adv Stone trip ----
		// Bot must be standing in a temple PZ. Optional sub-arg forces the dwell mode for
		// this trip (one-shot; resets after consumption). For dummy, an optional 2nd arg
		// forces a specific Lasting Exercise weapon id (e.g. 35290 for wand, 35288 for bow).
		// Without args, mode and weapon are randomly rolled per production behavior.
		//   advstone                 -> random mode
		//   advstone chest           -> force chest dwell
		//   advstone dummy           -> force dummy, random weapon
		//   advstone dummy 35290     -> force dummy with wand
		//   advstone waypoint        -> force route waypoint dwell
		if (command == "advstone" || command.rfind("advstone ", 0) == 0) {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			if (bot.advStoneActive) return fmt::format("Bot '{}' is already on a trip.", botName);
			// Parse optional mode arg + optional weapon id (for dummy mode only).
			std::string modeArg;
			std::string weaponArg;
			if (command.size() > 9) {
				std::string rest = command.substr(9);
				auto sp = rest.find(' ');
				if (sp == std::string::npos) {
					modeArg = rest;
				} else {
					modeArg = rest.substr(0, sp);
					weaponArg = rest.substr(sp + 1);
				}
			}
			if (!modeArg.empty()) {
				if (modeArg == "chest") s_forceAdvStoneNextMode = 1;
				else if (modeArg == "dummy") s_forceAdvStoneNextMode = 2;
				else if (modeArg == "waypoint" || modeArg == "wp") s_forceAdvStoneNextMode = 3;
				else return fmt::format("Unknown advstone mode '{}'. Valid: chest, dummy, waypoint.", modeArg);
			}
			if (!weaponArg.empty()) {
				if (s_forceAdvStoneNextMode != 2) {
					return fmt::format("advstone: weapon id arg '{}' only valid with mode 'dummy'.", weaponArg);
				}
				try {
					int parsed = std::stoi(weaponArg);
					if (parsed <= 0 || parsed > 65535) {
						return fmt::format("advstone: invalid weapon id '{}'.", weaponArg);
					}
					s_forceAdvStoneNextWeapon = static_cast<uint16_t>(parsed);
				} catch (...) {
					return fmt::format("advstone: weapon id '{}' is not a number.", weaponArg);
				}
			}
			if (startAdventurerStoneTrip(bot)) {
				return fmt::format("Bot '{}' Adventurer's Stone trip started from town {}{}{}.",
					botName, bot.advStoneStartTownId,
					modeArg.empty() ? "" : fmt::format(" (forced mode={})", modeArg),
					weaponArg.empty() ? "" : fmt::format(" (forced weapon={})", weaponArg));
			}
			s_forceAdvStoneNextMode = 0;   // clear on failure
			s_forceAdvStoneNextWeapon = 0; // clear on failure
			return fmt::format("Bot '{}' failed to start trip (not in temple PZ, pz-locked, or route not loaded).", botName);
		}

		// ---- resume: Resume normal AI from IDLE ----
		// ---- pin: hold a bot still for observation (debug mode) ----
		// A pinned bot never self-assigns work (doActivityReroll returns early) and is skipped
		// by the Lua hibernation loop, so it stays awake and idle until told to do something.
		// This is what makes `reload debug,<names>` usable: without it the bot picks a hunt
		// mid-observation and a manual `goto` gets silently overwritten. Unlike `stop`, it
		// does not expire.
		// ---- PERF STRESS HARNESS: per-bot probe control ----
		//
		// `probe on` marks this bot as if a human were cast-watching it, which is what makes the
		// whole harness work without a client: getCastViewerCount() folds the synthetic count in,
		// so the hibernation guard, both anchor lists (C++ and bot_hibernation.lua), botWalkObserved,
		// the roam watched test and the chat observer gate all treat it as a camera.
		//
		// It also PINS the bot. Without that the probe self-assigns hunts, boards boats and
		// teleports to patrol starts, so it wanders off the itinerary mid-window and silently moves
		// the anchor the measurement depends on. (findBotsForParty and onBotInvited now honour pin
		// too, so a probe can no longer be conscripted into somebody else's party either.)
		// `probe watch on|off` -- observe a bot WITHOUT pinning it.
		//
		// The itinerary probe (`probe on`) is pinned so it stays where the scenario put it. A
		// hunt watch is the opposite case: the bot is already doing the expensive thing, and the
		// only missing ingredient is a camera. Pinning it would clear its hunt script and walk
		// target -- destroying the very workload being measured.
		//
		// This is what makes observed HUNTING measurable unattended: ask whohunts which bots hold
		// a hunt reservation, watch those, and the observer IS the hunter. No spawn coordination,
		// no probe parked in a spawn hoping something happens, and no probe-death problem.
		// Watching a hibernated hunter also materialises its virtual hunt into a real one, which
		// is exactly the expensive path a player walking into a spawn would trigger.
		if (command == "probe watch on" || command == "probe watch off") {
			if (command == "probe watch off") {
				s_probeBots.erase(bot.guid);
				if (auto pl = bot.getPlayer()) { pl->setSyntheticCastViewers(0); }
				return fmt::format("Bot '{}' probe-watch OFF.", botName);
			}
			if (!player) {
				return fmt::format("Bot '{}' has no Player -- cannot watch.", botName);
			}
			s_probeBots.insert(bot.guid);
			player->setSyntheticCastViewers(1);
			return fmt::format("Bot '{}' probe-watch ON (unpinned) hunt={} phase={} at ({},{},{}).",
				botName, bot.huntScriptId, static_cast<int>(bot.huntPhase),
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
		}

		if (command == "probe on" || command == "probe off" || command == "probe status") {
			if (command == "probe status") {
				return fmt::format("Bot '{}' probe={} pinned={} syntheticViewers={}",
					botName, s_probeBots.count(bot.guid) ? "yes" : "no",
					s_debugPinned.count(bot.guid) ? "yes" : "no",
					player ? player->getCastViewerCount() : 0);
			}
			if (command == "probe off") {
				s_probeBots.erase(bot.guid);
				s_debugPinned.erase(bot.guid);
				if (player) { player->setSyntheticCastViewers(0); }
				return fmt::format("Bot '{}' probe OFF (unpinned).", botName);
			}
			// probe on. A hibernated bot has no live Player in the online map, so wake it first --
			// otherwise the flag would land on nothing and the probe would be silently inert.
			if (bot.hibernated) {
				wakeBot(bot.guid);
				player = bot.getPlayer();
			}
			if (!player) {
				return fmt::format("Bot '{}' has no Player — cannot arm as probe.", botName);
			}
			s_probeBots.insert(bot.guid);
			// Same teardown `pin on` performs: drop whatever work it had picked up.
			s_debugPinned.insert(bot.guid);
			bot.stopCooldownUntil = 0;
			bot.huntScriptId = 0;
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			bot.state = bot.active ? BotAIState::IDLE : BotAIState::INACTIVE;
			player->setSyntheticCastViewers(1);
			return fmt::format("Bot '{}' probe ON (pinned, observed) at ({},{},{}).",
				botName, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
		}

		// `probe teleport x,y,z` — move the probe AND fire the wake burst a real player's teleport
		// would fire. Game::internalTeleport deliberately gates that burst on !isBotPlayer(), and
		// that guard is left alone: relaxing it would make EVERY autonomous bot teleport (hunt
		// teleport-to-start, teleport home, roam home, boat travel, party assembly) fire a 100-tile
		// burst at an uncontrolled position, and would re-enter wakeBotsInRadius, whose burst
		// accumulator is explicitly not re-entrant. Firing it here instead keeps bursts to exactly
		// the ones the harness asked for.
		if (command.rfind("probe teleport ", 0) == 0) {
			if (!s_probeBots.count(bot.guid)) {
				return fmt::format("Bot '{}' is not a probe — run `probe on` first.", botName);
			}
			int x = 0, y = 0, z = 0;
			if (std::sscanf(command.c_str() + 15, "%d,%d,%d", &x, &y, &z) != 3) {
				return "Invalid format. Use: probe teleport x,y,z";
			}
			const Position dest(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
				static_cast<uint8_t>(z));
			BOT_TELEPORT(player, dest, true);
			bot.currentPos = dest;
			bot.lastPos = dest;
			if (uint32_t newTownId = findNearestTown(dest); newTownId > 0) {
				bot.townId = newTownId;
				auto town = g_game().map.towns.getTown(newTownId);
				bot.townName = town ? town->getName() : fmt::format("town {}", newTownId);
			}
			const uint32_t woke = wakeBotsInRadius(dest, 100);
			return fmt::format("Bot '{}' probe-teleported to ({},{},{}) town={} woke={}",
				botName, x, y, z, bot.townName, woke);
		}

		if (command == "pin on" || command == "pin off" || command == "pin status") {
			if (command == "pin on") {
				s_debugPinned.insert(bot.guid);
				bot.stopCooldownUntil = 0; // pin supersedes the 5-min stop cooldown
				bot.huntScriptId = 0;
				bot.hasWalkTarget = false;
				bot.currentPOI = nullptr;
				bot.state = bot.active ? BotAIState::IDLE : BotAIState::INACTIVE;
				return fmt::format("Bot '{}' PINNED — no auto tasks, no hibernation.", botName);
			}
			if (command == "pin off") {
				s_debugPinned.erase(bot.guid);
				return fmt::format("Bot '{}' unpinned — normal AI resumes.", botName);
			}
			return fmt::format("Bot '{}' pinned={} (total pinned={})",
				botName, s_debugPinned.count(bot.guid) ? "yes" : "no", s_debugPinned.size());
		}

		if (command == "resume") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			bot.state = BotAIState::IDLE;
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			bot.stopCooldownUntil = 0; // Clear stop cooldown
			return fmt::format("Bot '{}' resumed (IDLE).", botName);
		}

		// ---- active: Force-activate an inactive bot (teleport to temple) ----
		if (command == "active") {
			if (bot.active) return fmt::format("Bot '{}' is already active.", botName);
			if (activateBot(bot.guid)) {
				return fmt::format("Bot '{}' activated.", botName);
			}
			return fmt::format("Bot '{}' failed to activate.", botName);
		}

		// ---- active x,y,z: Teleport to position, activate, enable cast ----
		if (command.size() >= 6 && command.substr(0, 7) == "active ") {
			auto args = command.substr(7);
			int x = 0, y = 0, z = 0;
			if (sscanf(args.c_str(), "%d,%d,%d", &x, &y, &z) == 3 && x > 0 && y > 0 && z >= 0) {
				Position targetPos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
				// If already active, just teleport
				if (bot.active) {
					BOT_TELEPORT(player, targetPos, true);
					bot.currentPos = targetPos;
					syncTownIdToPos(bot);
					return fmt::format("Bot '{}' teleported to ({},{},{}).", botName, x, y, z);
				}
				// Activate: set state, teleport to admin position, restore HP, enable cast
				bot.active = true;
				bot.state = BotAIState::IDLE;
				bot.tickCounter = botInitialTickPhase(bot.guid);  // guid-phased (Phase 1) — not 0

				BOT_TELEPORT(player, targetPos, true);
				bot.currentPos = targetPos;
				syncTownIdToPos(bot);

				player->health = player->healthMax;
				player->mana = player->getMaxMana();
				g_game().addCreatureHealth(player);
				g_game().addPlayerMana(player);

				player->setFightMode(FIGHTMODE_ATTACK);
				player->setCastBroadcasting(true);
				auto& db = Database::getInstance();
				db.executeQuery(fmt::format(
					"INSERT INTO `cast_broadcasters` (`player_id`, `player_name`) VALUES ({}, {}) "
					"ON DUPLICATE KEY UPDATE `player_name` = {}",
					bot.guid, db.escapeString(player->getName()), db.escapeString(player->getName())));

				g_logger().info("[BotEngine] Activated bot '{}' at ({},{},{}) via admin command",
					player->getName(), x, y, z);
				return fmt::format("Bot '{}' activated at ({},{},{}) with cast ON.", botName, x, y, z);
			}
			return fmt::format("Usage: active x,y,z — got '{}'", args);
		}

		// ---- inactive: Force-deactivate bot (true offline — removed from world) ----
		if (command == "inactive") {
			if (!bot.active) {
				return fmt::format("Bot '{}' already inactive.", botName);
			}
			forceDeactivateBot(bot.guid);
			return fmt::format("Bot '{}' deactivated — removed from world.", botName);
		}

		// ---- verbose on/off (also: log on/off) ----
		if (command == "verbose on" || command == "log on") {
			bot.verboseLog = true;
			bot.verboseLogManual = true;
			return fmt::format("Bot '{}' verbose logging enabled (manual).", botName);
		}
		if (command == "verbose off" || command == "log off") {
			bot.verboseLog = false;
			bot.verboseLogManual = false;
			return fmt::format("Bot '{}' verbose logging disabled.", botName);
		}

		// ---- debug_kills <N>: Set per-bot hunt kill limit (0 = time-based only) ----
		if (command.size() > 12 && command.substr(0, 12) == "debug_kills ") {
			uint32_t limit = 0;
			try { limit = std::stoul(command.substr(12)); } catch (...) {}
			bot.huntDebugKillLimit = limit;
			if (limit == 0) {
				return fmt::format("Bot '{}' hunt mode: time-based (30-180 min, no kill limit).", botName);
			}
			return fmt::format("Bot '{}' debug hunt kill limit set to {}.", botName, limit);
		}

		// ---- hunt [name|id]: Start a hunt ----
		if (command == "hunt" || (command.size() > 5 && command.substr(0, 5) == "hunt ")) {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			if (bot.huntScriptId > 0) {
				std::string scriptName;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { scriptName = s.name; break; }
				}
				return fmt::format("Bot '{}' already hunting '{}' (script={}).", botName, scriptName, bot.huntScriptId);
			}

			if (command == "hunt") {
				// No args - pick random eligible
				if (tryStartHunt(bot)) {
					std::string scriptName;
					for (const auto& s : huntScripts_) {
						if (s.id == bot.huntScriptId) { scriptName = s.name; break; }
					}
					return fmt::format("Bot '{}' started hunt '{}' (script={}).", botName, scriptName, bot.huntScriptId);
				}
				return fmt::format("Bot '{}' no eligible hunts found.", botName);
			}

			// Hunt with name/ID filter
			auto args = command.substr(5);
			uint32_t scriptId = 0;
			try { scriptId = std::stoul(args); } catch (...) {}

			if (scriptId == 0) {
				// Search by name substring (case-insensitive)
				std::string argsLower = args;
				std::transform(argsLower.begin(), argsLower.end(), argsLower.begin(), ::tolower);
				// Find best (shortest name) match for specificity
				size_t bestLen = std::string::npos;
				for (const auto& script : huntScripts_) {
					std::string nameLower = script.name;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
					if (nameLower.find(argsLower) != std::string::npos) {
						if (script.name.size() < bestLen) {
							bestLen = script.name.size();
							scriptId = script.id;
						}
					}
				}
			}

			if (scriptId == 0) {
				return fmt::format("Hunt not found: '{}'", args);
			}

			// Find the script and check eligibility
			const HuntScript* target = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == scriptId) { target = &s; break; }
			}
			if (!target) return fmt::format("Hunt script {} not found.", scriptId);
			if (!target->enabled) return fmt::format("Hunt '{}' is disabled.", target->name);
			if (target->patrolWaypoints.empty() && target->scriptCategory != "traveling") return fmt::format("Hunt '{}' has no patrol waypoints.", target->name);
			// If another bot has this hunt reserved, stop them and free the reservation
			if (activeHunts_.count(scriptId)) {
				uint32_t existingGuid = activeHunts_[scriptId];
				if (existingGuid != bot.guid) {
					auto existIt = guidToIndex_.find(existingGuid);
					if (existIt != guidToIndex_.end()) {
						auto& existBot = bots_[existIt->second];
						s_leavingPhaseStart.erase(existBot.guid);
						s_leavingWpTimer.erase(existBot.guid);
						s_huntTravelStart.erase(existBot.guid);
						if (existBot.isPartyHuntLeader && existBot.partyHuntId > 0) {
							dissolvePartyHunt(existBot.partyHuntId, "hunt_reassign");
						}
						castLogError(existBot, fmt::format("HUNT EVICTED: script={} kills={} (reassigned to {})",
							existBot.huntScriptId, existBot.huntKillCount, botName));
						trackNavEvent("hunt_abort", existBot, existBot.huntScriptId, getHuntScriptName(existBot, huntScripts_),
							existBot.townId, "hunt", fmt::format("reason=evicted for {} kills={}", botName, existBot.huntKillCount));
						teleportToTemple(existBot);
						existBot.huntScriptId = 0;
						existBot.huntKillCount = 0;
						existBot.huntIgnoredMonsters.clear();
						existBot.recoveryWaypoints.clear();
						existBot.isRecoveryRoute = false;
						existBot.state = existBot.active ? BotAIState::IDLE : BotAIState::INACTIVE;
						existBot.hasWalkTarget = false;
						existBot.currentPOI = nullptr;
					}
					activeHunts_.erase(scriptId);
				}
			}
			if (!target->spawnGroup.empty() && activeSpawnGroups_.count(target->spawnGroup)) {
				uint32_t existingGuid = activeSpawnGroups_[target->spawnGroup];
				if (existingGuid != bot.guid) {
					auto existIt = guidToIndex_.find(existingGuid);
					if (existIt != guidToIndex_.end()) {
						auto& existBot = bots_[existIt->second];
						s_leavingPhaseStart.erase(existBot.guid);
						s_leavingWpTimer.erase(existBot.guid);
						s_huntTravelStart.erase(existBot.guid);
						if (existBot.isPartyHuntLeader && existBot.partyHuntId > 0) {
							dissolvePartyHunt(existBot.partyHuntId, "hunt_reassign");
						}
						castLogError(existBot, fmt::format("HUNT EVICTED: script={} kills={} (spawnGroup reassigned to {})",
							existBot.huntScriptId, existBot.huntKillCount, botName));
						trackNavEvent("hunt_abort", existBot, existBot.huntScriptId, getHuntScriptName(existBot, huntScripts_),
							existBot.townId, "hunt", fmt::format("reason=spawnGroup evicted for {} kills={}", botName, existBot.huntKillCount));
						teleportToTemple(existBot);
						existBot.huntScriptId = 0;
						existBot.huntKillCount = 0;
						existBot.huntIgnoredMonsters.clear();
						existBot.recoveryWaypoints.clear();
						existBot.isRecoveryRoute = false;
						existBot.state = existBot.active ? BotAIState::IDLE : BotAIState::INACTIVE;
						existBot.hasWalkTarget = false;
						existBot.currentPOI = nullptr;
					}
					activeSpawnGroups_.erase(target->spawnGroup);
				}
			}

			// Admin forced hunt — skip level/vocation checks (only auto-hunt uses filters)

			// Reserve and start. Quests take no reservation — they are shared under a
			// cooldown, and a stray activeHunts_ entry here would be invisible to the
			// auto-picker (which no longer consults it for quests) while still blocking
			// recovery searches.
			const bool targetIsQuest = target->isQuest || target->scriptCategory == "quest";
			if (targetIsQuest) {
				botStampQuestStart(target->id);
			} else {
				activeHunts_[target->id] = bot.guid;
				if (!target->spawnGroup.empty()) {
					activeSpawnGroups_[target->spawnGroup] = bot.guid;
				}
			}

			bot.huntScriptId = target->id;
			logHuntAssign(bot, target->id);
			bot.huntTownId = target->townId;
			bot.huntStartTime = OTSYS_TIME();
			bot.huntEndTime = targetIsQuest
				? botQuestHuntEndTime(bot.huntStartTime)
				: bot.huntStartTime + uniform_random(g_configManager().getNumber(BOT_HUNT_TIME_MIN_SEC), g_configManager().getNumber(BOT_HUNT_TIME_MAX_SEC)) * 1000LL;
			bot.huntKillCount = 0;
			bot.huntWaypointIdx = 0;
			bot.huntPatrolCycles = 0;
			bot.huntTargetId = 0;
			bot.huntChaseFailCount = 0;
			bot.huntIgnoredMonsters.clear();
			bot.huntWaypointSkipCount = 0;

			if (target->townId != bot.townId) {
				bot.pendingHuntAfterTravel = true;
				startTravel(bot, target->townId);
				return fmt::format("Bot '{}' hunting '{}' — traveling to town {} first.", botName, target->name, target->townId);
			}

			if (target->scriptCategory == "traveling") {
				castLog(bot, fmt::format("CITY WALK START: '{}'", target->name));
				beginHuntPhase(bot, HuntPhase::TRAVEL_TO);
				return fmt::format("Bot '{}' started city walk '{}' (script={}).", botName, target->name, target->id);
			}
			beginHuntPhase(bot, HuntPhase::PREPARING);
			return fmt::format("Bot '{}' started hunt '{}' (script={}, town={}).", botName, target->name, target->id, target->townId);
		}

		// ---- invitebot <otherBot>: send a REAL party invite from this bot to another bot ----
		// The click-invite path needs a game client, which no headless test can drive. This issues
		// the identical call the client does (Game::playerInviteToParty -> Party::invitePlayer), so
		// the acceptance machine cannot tell the two apart — everything mechanical about
		// click-invite is covered by this command. Only the UX/feel needs a human.
		if (command.size() > 10 && command.substr(0, 10) == "invitebot ") {
			std::string otherName = command.substr(10);
			// Same exact-name linear scan the rest of this handler uses — there is no name index.
			BotState* otherPtr = nullptr;
			for (auto& b : bots_) {
				if (b.name == otherName) { otherPtr = &b; break; }
			}
			if (!otherPtr) return fmt::format("Bot '{}' not found.", otherName);
			auto& otherBot = *otherPtr;
			if (!bot.active) return fmt::format("Inviter bot '{}' is not active.", botName);
			if (!otherBot.active || otherBot.hibernated) {
				return fmt::format("Target bot '{}' must be awake (active={} hibernated={}). "
					"Wake it first.", otherBot.name, otherBot.active, otherBot.hibernated);
			}
			auto inviterPlayer = bot.getPlayer();
			auto inviteePlayer = otherBot.getPlayer();
			if (!inviterPlayer || !inviteePlayer) return "Both bots need a live Player.";
			if (inviteePlayer->getParty()) {
				return fmt::format("'{}' is already in a party.", otherBot.name);
			}
			// Never reuse a party we do not lead (the BOT_PARTY_LEAK_FIX invariant).
			if (auto stale = inviterPlayer->getParty(); stale && stale->getLeader() != inviterPlayer) {
				reclaimStaleCanaryParty(bot.guid, "invitebot");
			}
			auto party = inviterPlayer->getParty();
			if (!party) party = Party::create(inviterPlayer);
			if (!party) return "Failed to create the test party.";
			if (!party->invitePlayer(inviteePlayer)) {
				return fmt::format("invitePlayer refused for '{}'.", otherBot.name);
			}
			// An all-bot party has no real player to shield it from sweepStaleCanaryParties, so
			// exempt both ends for 15 minutes or the sweep would reclaim the test mid-run.
			const int64_t ttl = OTSYS_TIME() + 15 * 60 * 1000;
			s_inviteDebugKeepAlive[bot.guid] = ttl;
			s_inviteDebugKeepAlive[otherBot.guid] = ttl;
			g_logger().info("[BotEngine] [PINVITE] invitebot: '{}' invited '{}' (real Party::invitePlayer)",
				bot.name, otherBot.name);
			return fmt::format("'{}' invited '{}'. Watch [PINVITE]; inviteEnable={}.",
				bot.name, otherBot.name, inviteCfg_.enable ? "true" : "false");
		}

		// ---- partytest <vocList> [min,max] [teleport]: drive the PRODUCTION /party formation
		// headlessly with this bot standing in for the human leader. Same code path the player
		// command uses, minus the real-player requirement — which is exactly the path that read
		// past an empty openTiles vector and segfaulted the server, so it needs a test that does
		// not require a client. ----
		if (command.size() > 10 && command.substr(0, 10) == "partytest ") {
			if (!bot.active || bot.hibernated) {
				return fmt::format("Bot '{}' must be awake to stand in as leader.", botName);
			}
			auto leaderP = bot.getPlayer();
			if (!leaderP) return fmt::format("Bot '{}' has no live Player.", botName);
			s_inviteDebugKeepAlive[bot.guid] = OTSYS_TIME() + 15 * 60 * 1000;
			return executeCommand("*", fmt::format("party createbot {} {}",
				leaderP->getID(), command.substr(10)));
		}

		// ---- disbandparty: tear down this bot's party and release its members ----
		if (command == "disbandparty") {
			auto leaderPlayer = bot.getPlayer();
			if (!leaderPlayer) return fmt::format("Bot '{}' has no live Player.", botName);
			auto party = leaderPlayer->getParty();
			if (!party) return fmt::format("Bot '{}' is not in a party.", botName);
			// exitPartyMode each bot member first so engine state and Canary state come down
			// together — and so the exit-in-place semantics are what the test observes.
			std::vector<uint32_t> memberGuids;
			for (const auto& m : party->getMembers()) {
				if (m && m->isBotPlayer()) memberGuids.push_back(m->getGUID());
			}
			for (uint32_t g : memberGuids) {
				if (auto it = guidToIndex_.find(g); it != guidToIndex_.end()) {
					exitPartyMode(bots_[it->second]);
				}
			}
			dropAssemblyMember(bot.guid, "disbandparty");
			s_inviteDebugKeepAlive.erase(bot.guid);
			for (uint32_t g : memberGuids) s_inviteDebugKeepAlive.erase(g);
			if (leaderPlayer->getParty()) party->disband();
			return fmt::format("Disbanded '{}' party ({} bot member(s) released in place).",
				botName, memberGuids.size());
		}

		// ---- partyhunt [script_id]: Force a bot to start a party hunt (ROUND2 E: ANY vocation —
		// the leader is elected EK > RP > initiator, so the named bot may end up a support) ----
		if (command == "partyhunt" || (command.size() > 10 && command.substr(0, 10) == "partyhunt ")) {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			if (bot.partyHuntId > 0) {
				return fmt::format("Bot '{}' already in party hunt #{}.", botName, bot.partyHuntId);
			}
			int32_t forceScriptId = -1; // BOT_PARTY_CAP: see the hibernated handler above — -1 = admin, no script.
			if (command.size() > 10) {
				try { forceScriptId = std::stoi(command.substr(10)); } catch (...) {}
			}
			// For hibernated bots, skip abortHunt (its teleportToTemple needs a live Player);
			// tryStartPartyHunt clears the virtualSim hunt reservation itself before waking.
			if (bot.huntScriptId > 0 && !bot.hibernated) {
				abortHunt(bot, "admin partyhunt command");
			}
			if (!bot.hibernated) {
				bot.state = BotAIState::IDLE;
				bot.hasWalkTarget = false;
				bot.stopCooldownUntil = 0;
			}
			if (tryStartPartyHunt(bot, forceScriptId)) {
				return fmt::format("Bot '{}' started party hunt #{} (script={}).",
					botName, bot.partyHuntId, bot.huntScriptId);
			}
			return fmt::format("Bot '{}' failed to start party hunt (no ED available or no eligible scripts).", botName);
		}

		// ---- travel <city name or id> [wp_number]: Inter-city travel (or teleport to route waypoint) ----
		if (command.size() > 7 && command.substr(0, 7) == "travel ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto args = command.substr(7);

			// Split off trailing number if present (e.g. "thais 5" → townStr="thais", wpNum=5)
			std::string townStr = args;
			int32_t wpNum = -1;
			auto lastSpace = args.rfind(' ');
			if (lastSpace != std::string::npos) {
				std::string maybeName = args.substr(0, lastSpace);
				std::string maybeNum = args.substr(lastSpace + 1);
				bool isNumber = !maybeNum.empty() && std::all_of(maybeNum.begin(), maybeNum.end(), ::isdigit);
				if (isNumber) {
					townStr = maybeName;
					wpNum = std::stoi(maybeNum);
				}
			}

			uint32_t destId = 0;
			// Try numeric ID first (only if no wp number was split off)
			if (wpNum < 0) {
				try { destId = std::stoul(townStr); } catch (...) {}
			}

			// Try town name (exact then partial, case-insensitive)
			if (destId == 0) {
				std::string argsLower = townStr;
				std::transform(argsLower.begin(), argsLower.end(), argsLower.begin(), ::tolower);

				// Pass 1: exact match
				for (const auto& [id, town] : g_game().map.towns.getTowns()) {
					if (id == 0) continue;
					std::string townLower = town->getName();
					std::transform(townLower.begin(), townLower.end(), townLower.begin(), ::tolower);
					if (townLower == argsLower) {
						destId = id;
						break;
					}
				}

				// Pass 2: partial/substring match (shortest name wins)
				if (destId == 0) {
					size_t bestLen = std::string::npos;
					for (const auto& [id, town] : g_game().map.towns.getTowns()) {
						if (id == 0) continue;
						std::string townLower = town->getName();
						std::transform(townLower.begin(), townLower.end(), townLower.begin(), ::tolower);
						if (townLower.find(argsLower) != std::string::npos && town->getName().size() < bestLen) {
							bestLen = town->getName().size();
							destId = id;
						}
					}
				}
			}

			if (destId == 0) return fmt::format("Town not found: '{}'", townStr);

			// If waypoint number provided, teleport to that waypoint in the route
			if (wpNum >= 0) {
				// Look up the depot→boat route in the bot's CURRENT town (walk_to_boat)
				const auto* toBoatRoute = findCityRoute(bot.townId, "depot", "boat");
				// Also look up the boat→depot route in the DESTINATION town (walk_from_boat)
				const auto* fromBoatRoute = findCityRoute(destId, "boat", "depot");

				// Try source town depot→boat first
				if (toBoatRoute && wpNum < static_cast<int32_t>(toBoatRoute->size())) {
					auto& wp = (*toBoatRoute)[wpNum];
					BOT_TELEPORT(player, wp.pos, true);
					bot.currentPos = wp.pos;
					return fmt::format("Bot '{}' teleported to wp {}/{} of {}|depot~boat ({},{},{}) type={}.",
						botName, wpNum, toBoatRoute->size(), bot.townName,
						wp.pos.x, wp.pos.y, wp.pos.z, waypointTypeName(wp.type));
				}

				// If wpNum exceeds source route, try destination boat→depot
				if (fromBoatRoute) {
					// Offset: subtract source route size to index into dest route
					int32_t destIdx = toBoatRoute ? wpNum - static_cast<int32_t>(toBoatRoute->size()) : wpNum;
					if (destIdx >= 0 && destIdx < static_cast<int32_t>(fromBoatRoute->size())) {
						auto destTown = g_game().map.towns.getTown(destId);
						std::string destName = destTown ? destTown->getName() : "?";
						auto& wp = (*fromBoatRoute)[destIdx];
						BOT_TELEPORT(player, wp.pos, true);
						bot.currentPos = wp.pos;
						return fmt::format("Bot '{}' teleported to wp {}/{} of {}|boat~depot ({},{},{}) type={}.",
							botName, destIdx, fromBoatRoute->size(), destName,
							wp.pos.x, wp.pos.y, wp.pos.z, waypointTypeName(wp.type));
					}
				}

				// Out of range — show what's available
				std::string info;
				if (toBoatRoute) {
					info += fmt::format("{}|depot~boat: {} wps (0-{})", bot.townName, toBoatRoute->size(), toBoatRoute->size() - 1);
				}
				if (fromBoatRoute) {
					auto destTown = g_game().map.towns.getTown(destId);
					if (!info.empty()) info += ", ";
					info += fmt::format("{}|boat~depot: {} wps (0-{})", destTown ? destTown->getName() : "?",
						fromBoatRoute->size(), fromBoatRoute->size() - 1);
				}
				if (info.empty()) info = "No depot~boat or boat~depot routes found";
				return fmt::format("Waypoint {} out of range. Available: {}", wpNum, info);
			}

			// Normal travel (no waypoint number)
			auto destPos = getTravelPosition(destId).first;
			if (destPos.x == 0) {
				auto town = g_game().map.towns.getTown(destId);
				return fmt::format("No travel position for '{}' (town {}).", town ? town->getName() : "?", destId);
			}

			if (destId == bot.townId) {
				auto town = g_game().map.towns.getTown(destId);
				return fmt::format("Bot '{}' is already in {} (town {}).", botName, town ? town->getName() : "?", destId);
			}
			startTravel(bot, destId);
			auto town = g_game().map.towns.getTown(destId);
			return fmt::format("Bot '{}' traveling to {} (town {}).", botName, town ? town->getName() : "?", destId);
		}

		// ---- goto x,y,z: Walk to coordinates ----
		if (command.size() > 5 && command.substr(0, 5) == "goto ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto args = command.substr(5);
			int x, y, z;
			if (sscanf(args.c_str(), "%d,%d,%d", &x, &y, &z) == 3) {
				bot.walkTarget = Position(x, y, z);
				bot.hasWalkTarget = true;
				bot.currentPOI = nullptr;
				bot.pathFailCount = 0;
				bot.state = BotAIState::IDLE;
				// Seam 1 of 2 into the scoped route planner. An admin asking a specific bot to walk
				// to specific coordinates is the manual test trigger for it, and is rare enough to
				// need no rate limiting. Keyed on the exact target, so any other subsystem that
				// retargets this bot drops the claim automatically.
				s_plannerWalk[bot.guid] = bot.walkTarget;
				// The 2-minute stale-target guard keys on a HASH of the target tile, so re-issuing
				// a goto to the SAME tile would otherwise inherit the previous attempt's start time
				// and self-clear on the very next tick (observed: target set 16:06:05.768, cleared
				// 16:06:06.179 — 411ms). A newly issued walk always gets a fresh clock.
				s_walkTargetTimer.erase(bot.guid);
				return fmt::format("Bot '{}' walking to ({},{},{}).", botName, x, y, z);
			}
			return "Invalid goto format. Use: goto x,y,z";
		}

		// ---- teleport x,y,z: Instant teleport ----
		if (command.size() > 9 && command.substr(0, 9) == "teleport ") {
			auto args = command.substr(9);
			int x, y, z;
			if (sscanf(args.c_str(), "%d,%d,%d", &x, &y, &z) == 3) {
				Position dest(x, y, z);
				BOT_TELEPORT(player, dest, true);
				bot.currentPos = dest;
				// Update townId based on new position
				uint32_t newTownId = findNearestTown(dest);
				if (newTownId > 0) {
					bot.townId = newTownId;
					auto town = g_game().map.towns.getTown(newTownId);
					bot.townName = town ? town->getName() : fmt::format("town {}", newTownId);
				}
				return fmt::format("Bot '{}' teleported to ({},{},{}) town={}.", botName, x, y, z, bot.townName);
			}
			return "Invalid teleport format. Use: teleport x,y,z";
		}

		// ---- navigate <poi>: Walk to POI in current city ----
		if (command.size() > 9 && command.substr(0, 9) == "navigate ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto args = command.substr(9);
			std::string argsLower = args;
			std::transform(argsLower.begin(), argsLower.end(), argsLower.begin(), ::tolower);

			auto& pois = getCityPOIs();
			auto it = pois.find(bot.townId);
			if (it == pois.end()) return fmt::format("No POIs defined for town {}.", bot.townId);

			// Find matching POI by type keyword or name substring
			const BotPOI* bestPoi = nullptr;
			for (const auto& poi : it->second) {
				std::string poiNameLower = poi.name;
				std::transform(poiNameLower.begin(), poiNameLower.end(), poiNameLower.begin(), ::tolower);

				// Match by POI type keyword
				if ((argsLower == "depot" && poi.type == POIType::DEPOT) ||
					(argsLower == "temple" && poi.type == POIType::TEMPLE) ||
					(argsLower == "boat" && poi.type == POIType::BOAT) ||
					(argsLower == "shop" && poi.type == POIType::SHOP) ||
					(argsLower == "npc" && poi.type == POIType::NPC)) {
					bestPoi = &poi;
					break;
				}
				// Match by name substring
				if (poiNameLower.find(argsLower) != std::string::npos) {
					bestPoi = &poi;
					break;
				}
			}

			if (!bestPoi) return fmt::format("POI '{}' not found in town {}.", args, bot.townId);

			bot.walkTarget = bestPoi->pos;
			bot.hasWalkTarget = true;
			bot.currentPOI = nullptr;
			bot.pathFailCount = 0;
			bot.followingCityRoute = false;
			bot.pendingNavDest = argsLower; // City route destination (e.g. "depot", "temple", "boat")
			bot.state = BotAIState::IDLE;
			return fmt::format("Bot '{}' navigating to '{}' ({},{},{}).", botName, bestPoi->name,
				bestPoi->pos.x, bestPoi->pos.y, bestPoi->pos.z);
		}

		// ---- poi: Detect nearest POI ----
		if (command == "poi") {
			auto& pois = getCityPOIs();
			auto it = pois.find(bot.townId);
			if (it == pois.end()) return fmt::format("No POIs for town {}.", bot.townId);

			const BotPOI* nearest = nullptr;
			int32_t nearestDist = INT32_MAX;
			for (const auto& poi : it->second) {
				int32_t dist = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(poi.pos.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(poi.pos.y)));
				if (dist < nearestDist) {
					nearestDist = dist;
					nearest = &poi;
				}
			}

			if (nearest) {
				return fmt::format("Nearest POI: '{}' ({},{},{}) dist={}", nearest->name,
					nearest->pos.x, nearest->pos.y, nearest->pos.z, nearestDist);
			}
			return "No POIs found.";
		}

		// ---- stairs up|down: Floor change ----
		if (command == "stairs up" || command == "stairs down") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			bool goDown = (command == "stairs down");
			Position targetPos = bot.currentPos;
			targetPos.z = goDown ? targetPos.z + 1 : targetPos.z - 1;
			startFloorChange(bot, goDown, targetPos);
			return fmt::format("Bot '{}' searching for stairs {} from z={}", botName,
				goDown ? "down" : "up", bot.currentPos.z);
		}

		// ---- scan [radius]: Scan for floor-change tiles ----
		if (command == "scan" || (command.size() > 5 && command.substr(0, 5) == "scan ")) {
			int32_t radius = FC_SCAN_RADIUS;
			if (command.size() > 5) {
				try { radius = std::stoi(command.substr(5)); } catch (...) {}
			}
			auto upTrans = findZTransitions(bot.currentPos, radius, false);
			auto downTrans = findZTransitions(bot.currentPos, radius, true);
			std::string result = fmt::format("Scan at ({},{},{}) radius={}:\n",
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, radius);
			result += fmt::format("  UP transitions: {}\n", upTrans.size());
			for (const auto& t : upTrans) {
				result += fmt::format("    {} at ({},{},{}) dist={}\n", t.type, t.pos.x, t.pos.y, t.pos.z, t.dist);
			}
			result += fmt::format("  DOWN transitions: {}\n", downTrans.size());
			for (const auto& t : downTrans) {
				result += fmt::format("    {} at ({},{},{}) dist={}\n", t.type, t.pos.x, t.pos.y, t.pos.z, t.dist);
			}
			return result;
		}

		// ---- step <dir>: Force single step ----
		if (command.size() > 5 && command.substr(0, 5) == "step ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto dirStr = command.substr(5);
			Direction dir;
			if (dirStr == "n" || dirStr == "north") dir = DIRECTION_NORTH;
			else if (dirStr == "s" || dirStr == "south") dir = DIRECTION_SOUTH;
			else if (dirStr == "e" || dirStr == "east") dir = DIRECTION_EAST;
			else if (dirStr == "w" || dirStr == "west") dir = DIRECTION_WEST;
			else if (dirStr == "ne" || dirStr == "northeast") dir = DIRECTION_NORTHEAST;
			else if (dirStr == "nw" || dirStr == "northwest") dir = DIRECTION_NORTHWEST;
			else if (dirStr == "se" || dirStr == "southeast") dir = DIRECTION_SOUTHEAST;
			else if (dirStr == "sw" || dirStr == "southwest") dir = DIRECTION_SOUTHWEST;
			else return fmt::format("Unknown direction: '{}'. Use: n/s/e/w/ne/nw/se/sw", dirStr);

			g_game().playerMove(player->getID(), dir);
			return fmt::format("Bot '{}' stepped {}.", botName, dirStr);
		}

		// ---- routeinfo: read-only dump of the bot's live navigation state ----
		// BOT_TELEPORT_TILE_SAFETY diagnostic. The Feyrist->Thais trip walks its city route
		// correctly all the way to the Feyrist exit shrine, then freezes at the earth hub one
		// tile from the flame waypoint the route already contains. The open question is whether
		// the route SURVIVED the shrine teleport or was wiped by the unexpected-jump branch
		// (bot_tick.cpp), and castLog only reaches cast viewers, so this surfaces it on demand.
		// Pure read-only: touches no bot state.
		if (command == "routeinfo") {
			std::string out = fmt::format("[ROUTEINFO] '{}' state={} town={} pos=({},{},{})\n",
				botName, static_cast<int>(bot.state), bot.townId,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
			out += fmt::format("  followingCityRoute={} cityRouteIdx={} cityRouteWps={} lastRouteDestination='{}'\n",
				bot.followingCityRoute ? 1 : 0, bot.cityRouteIdx, bot.cityRouteWps.size(),
				bot.lastRouteDestination);
			if (!bot.cityRouteWps.empty()) {
				const size_t from = bot.cityRouteIdx > 1 ? bot.cityRouteIdx - 2 : 0;
				const size_t to = std::min(bot.cityRouteIdx + 3, bot.cityRouteWps.size());
				for (size_t i = from; i < to; ++i) {
					const auto& w = bot.cityRouteWps[i];
					out += fmt::format("   {}{:>3}: {:<10} ({},{},{}){}{}\n",
						i == bot.cityRouteIdx ? "->" : "  ", i, waypointTypeName(w.type),
						w.pos.x, w.pos.y, w.pos.z,
						w.isWalkOnFc ? " [walkOnFc]" : "",
						w.extraData.empty() ? "" : fmt::format(" [{}]", w.extraData));
				}
			}
			out += fmt::format("  travelPhase='{}' travelDestTown={} hasWalkTarget={} walkTarget=({},{},{})\n",
				bot.travelPhase, bot.travelDestTownId, bot.hasWalkTarget ? 1 : 0,
				bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z);
			out += fmt::format("  huntScriptId={} huntPhase={} huntWaypointIdx={} pathFailCount={} fcState={}\n",
				bot.huntScriptId, static_cast<int>(bot.huntPhase), bot.huntWaypointIdx,
				bot.pathFailCount, static_cast<int>(bot.fcState));
			out += fmt::format("  nearShrineHub={} ShrineEntrance(storage)={}\n",
				botNearShrineHub(bot.currentPos) ? 1 : 0,
				player ? player->getStorageValue(STORAGE_SHRINE_ENTRANCE) : -999);
			g_logger().info("{}", out);
			return out;
		}

		// ---- loot: BOT_CORPSE_LOOT run state for this bot ----
		if (command == "loot") {
			auto it = s_lootRun.find(bot.guid);
			if (it == s_lootRun.end()) {
				return fmt::format("[LOOT] '{}' has no loot state (enable={} phase={})",
					botName, lootCfg_.enable ? "true" : "false", static_cast<int>(bot.huntPhase));
			}
			const auto& run = it->second;
			const int64_t now = OTSYS_TIME();
			const bool windowOpen = (bot.lastKillTime > 0 && now - bot.lastKillTime <= lootCfg_.windowMs)
				|| now < run.windowUntilMs;
			std::string out = fmt::format(
				"[LOOT] '{}' gate={} phase={} target={} window={} (sinceKill={}ms untilMs={})\n",
				botName, lootGatePasses(bot, player) ? "PASS" : "BLOCK",
				static_cast<int>(bot.huntPhase), bot.huntTargetId,
				windowOpen ? "OPEN" : "closed",
				bot.lastKillTime > 0 ? now - bot.lastKillTime : -1,
				run.windowUntilMs > now ? run.windowUntilMs - now : 0);
			out += fmt::format("[LOOT] candidates={} claimed={} blocked={} censusAge={}ms\n",
				run.candidates.size(), run.claimed.size(), run.blocked.size(),
				run.lastCensusMs ? now - run.lastCensusMs : -1);
			if (run.hasTarget) {
				out += fmt::format("[LOOT] walking to ({},{},{}) dist={} fails={} deadlineIn={}ms\n",
					run.pos.x, run.pos.y, run.pos.z,
					std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(run.pos.x)),
					         std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(run.pos.y))),
					run.fails, run.deadlineMs - now);
			} else {
				out += "[LOOT] no active walk run\n";
			}
			if (const auto& pend = run.adjPending.lock()) {
				out += fmt::format("[LOOT] adjacent pending id={} at ({},{},{}) opensIn={}ms\n",
					pend->getID(), run.adjPendingPos.x, run.adjPendingPos.y, run.adjPendingPos.z,
					run.adjOpenAtMs - now);
			}
			for (const auto& [weakCorpse, pos] : run.candidates) {
				const auto& c = weakCorpse.lock();
				if (!c) continue;
				out += fmt::format("  cand id={} at ({},{},{}) owner={} items={} lit={}\n",
					c->getID(), pos.x, pos.y, pos.z, c->getCorpseOwner(), c->size(),
					c->hasLootHighlight() ? 1 : 0);
			}
			g_logger().info("{}", out);
			return out;
		}

		// ---- use x,y,z: Use item at position (for testing sewer/ladder interaction) ----
		if (command.size() > 4 && command.substr(0, 4) == "use ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto args = command.substr(4);
			int x = 0, y = 0, z = 0;
			if (sscanf(args.c_str(), "%d,%d,%d", &x, &y, &z) == 3 && x > 0 && y > 0 && z >= 0) {
				Position usePos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
				auto tile = g_game().map.getTile(usePos);
				if (!tile) return fmt::format("No tile at ({},{},{}).", x, y, z);

				// Find a usable item: check stacked items first (doors, etc.), then ladder, sewer, ground
				std::shared_ptr<Item> useItem;
				const auto* tileItems = tile->getItemList();
				if (tileItems && !tileItems->empty()) {
					useItem = *tileItems->rbegin(); // Top stacked item (most likely what player wants to use)
				}
				if (!useItem) useItem = findLadderItem(usePos);
				if (!useItem) useItem = findSewerItem(usePos);
				if (!useItem) {
					if (auto ground = tile->getGround()) {
						useItem = ground;
					}
				}
				if (!useItem) return fmt::format("No item to use at ({},{},{}).", x, y, z);

				g_actions().useItem(player, usePos, 0, useItem, false);
				return fmt::format("Bot '{}' used item {} at ({},{},{}).", botName, useItem->getID(), x, y, z);
			}
			return "Usage: use x,y,z";
		}

		// ---- tileinfo x,y,z: Dump tile items and flags for diagnostics ----
		if (command.size() > 9 && command.substr(0, 9) == "tileinfo ") {
			auto args = command.substr(9);
			int x = 0, y = 0, z = 0;
			if (sscanf(args.c_str(), "%d,%d,%d", &x, &y, &z) == 3 && x > 0 && y > 0 && z >= 0) {
				Position tilePos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
				auto tile = g_game().map.getTile(tilePos);
				if (!tile) return fmt::format("No tile at ({},{},{}).", x, y, z);

				std::string result = fmt::format("Tile ({},{},{}) flags: ", x, y, z);
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_DOWN)) result += "FC_DOWN ";
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_NORTH)) result += "FC_NORTH ";
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH)) result += "FC_SOUTH ";
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_EAST)) result += "FC_EAST ";
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_WEST)) result += "FC_WEST ";
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) result += "FC_SOUTH_ALT ";
				if (tile->hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) result += "FC_EAST_ALT ";
				if (tile->hasFlag(TILESTATE_PROTECTIONZONE)) result += "PZ ";
				if (!tile->hasFlag(TILESTATE_FLOORCHANGE)) result += "(none) ";

				if (auto ground = tile->getGround()) {
					const auto& it = Item::items[ground->getID()];
					result += fmt::format("| Ground: id={} name='{}' isLadder={} type={} fc={}",
						ground->getID(), it.name, it.isLadder() ? "yes" : "no",
						static_cast<int>(it.type), it.floorChange);
				} else {
					result += "| Ground: none";
				}

				const auto* items = tile->getItemList();
				if (items && !items->empty()) {
					int idx = 0;
					for (const auto& item : *items) {
						const auto& it = Item::items[item->getID()];
						result += fmt::format(" | Item[{}]: id={} name='{}' isLadder={} type={} fc={}",
							idx++, item->getID(), it.name, it.isLadder() ? "yes" : "no",
							static_cast<int>(it.type), it.floorChange);
						if (idx >= 10) { result += " | ... (truncated)"; break; }
					}
				} else {
					result += " | Items: none";
				}
				return result;
			}
			return "Usage: tileinfo x,y,z";
		}

		// ---- scandoors [radius]: Scan nearby tiles for closed doors ----
		if (command.substr(0, 9) == "scandoors") {
			int32_t radius = 15;
			if (command.size() > 10) {
				radius = std::atoi(command.substr(10).c_str());
				if (radius < 1) radius = 1;
				if (radius > 30) radius = 30;
			}
			auto& doorTable = getDoorTable();
			std::string result = fmt::format("Scanning {}x{} area around ({},{},{}) for doors:",
				radius*2+1, radius*2+1, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
			int found = 0;
			for (int32_t ox = -radius; ox <= radius && found < 20; ox++) {
				for (int32_t oy = -radius; oy <= radius && found < 20; oy++) {
					Position checkPos(bot.currentPos.x + ox, bot.currentPos.y + oy, bot.currentPos.z);
					auto tile = g_game().map.getTile(checkPos);
					if (!tile) continue;
					auto items = tile->getItemList();
					if (!items) continue;
					for (auto& item : *items) {
						if (doorTable.count(item->getID())) {
							result += fmt::format(" | DOOR id={} at ({},{},{})",
								item->getID(), checkPos.x, checkPos.y, checkPos.z);
							found++;
						}
					}
				}
			}
			if (found == 0) result += " NONE found";
			return result;
		}

		// ---- placeitem <id> <x,y,z>: Place an item for testing ----
		if (command.size() > 10 && command.substr(0, 10) == "placeitem ") {
			auto args = command.substr(10);
			int itemId = 0, x = 0, y = 0, z = 0;
			if (sscanf(args.c_str(), "%d %d,%d,%d", &itemId, &x, &y, &z) == 4 && itemId > 0) {
				Position itemPos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
				auto tile = g_game().map.getTile(itemPos);
				if (!tile) return fmt::format("No tile at ({},{},{})", x, y, z);
				auto newItem = Item::CreateItem(static_cast<uint16_t>(itemId), 1);
				if (!newItem) return fmt::format("Failed to create item {}", itemId);
				g_game().internalAddItem(tile, newItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
				return fmt::format("Placed item {} at ({},{},{})", itemId, x, y, z);
			}
			return "Usage: placeitem <id> <x,y,z>";
		}

		// ---- removeitem <id> <x,y,z>: Remove an item for testing ----
		if (command.size() > 11 && command.substr(0, 11) == "removeitem ") {
			auto args = command.substr(11);
			int itemId = 0, x = 0, y = 0, z = 0;
			if (sscanf(args.c_str(), "%d %d,%d,%d", &itemId, &x, &y, &z) == 4 && itemId > 0) {
				Position itemPos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
				auto tile = g_game().map.getTile(itemPos);
				if (!tile) return fmt::format("No tile at ({},{},{})", x, y, z);
				auto items = tile->getItemList();
				if (!items) return "No items on tile";
				for (auto& item : *items) {
					if (item->getID() == static_cast<uint16_t>(itemId)) {
						g_game().internalRemoveItem(item);
						return fmt::format("Removed item {} from ({},{},{})", itemId, x, y, z);
					}
				}
				return fmt::format("Item {} not found on tile", itemId);
			}
			return "Usage: removeitem <id> <x,y,z>";
		}

		// ---- pk <target name>: Force PK attack ----
		if (command.size() > 3 && command.substr(0, 3) == "pk ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto targetName = command.substr(3);
			auto target = g_game().getPlayerByName(targetName);
			if (!target) return fmt::format("Player '{}' not found or offline.", targetName);
			if (target->getID() == player->getID()) return "Cannot PK self.";

			bot.pkTarget = target->getID();
			bot.combatDecision = "fight";
			bot.combatStartTime = OTSYS_TIME();
			bot.lastCombatProgress = OTSYS_TIME();
			bot.combatHpCheckTime = OTSYS_TIME();
			bot.combatHpBaseline = 0;
			bot.combatStalemateCount = 0;
			bot.pvpManaSpent = 0;
			player->setSecureMode(false);  // Allow attacking unmarked players
			bot.state = BotAIState::PK_ATTACK;
			return fmt::format("Bot '{}' attacking player '{}'.", botName, target->getName());
		}

		// ---- info: Show detailed state info ----
		if (command == "info") {
			auto upTrans = findZTransitions(bot.currentPos, 5, false);
			auto downTrans = findZTransitions(bot.currentPos, 5, true);
			std::string result = fmt::format("Bot '{}' info:\n", botName);
			result += fmt::format("  State: {} | Active: {} | Town: {} | Voc: {}\n",
				stateStr, bot.active ? "yes" : "no", bot.townId, bot.vocationId);
			result += fmt::format("  Pos: ({},{},{}) | HP: {}/{} | Mana: {}/{}\n",
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				player->getHealth(), player->getMaxHealth(),
				player->getMana(), player->getMaxMana());
			result += fmt::format("  Walk target: {} | FC state: {}\n",
				bot.hasWalkTarget ? fmt::format("({},{},{})", bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z) : "none",
				static_cast<int>(bot.fcState));
			result += fmt::format("  Nearby stairs: {} up, {} down (radius=5)\n", upTrans.size(), downTrans.size());
			if (bot.huntScriptId > 0) {
				std::string scriptName;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { scriptName = s.name; break; }
				}
				result += fmt::format("  Hunt: '{}' script={} kills={} wp={}\n",
					scriptName, bot.huntScriptId, bot.huntKillCount, bot.huntWaypointIdx);
			}
			if (auto icf = iceFishing_.find(bot.guid); icf != iceFishing_.end()) {
				const auto& s = icf->second;
				const auto& tile = g_game().map.getTile(s.target);
				const auto& ground = tile ? tile->getGround() : nullptr;
				result += fmt::format("  IceFish: target=({},{},{}) ground={} casts={} picks={} ends_in={}s\n",
					s.target.x, s.target.y, s.target.z, ground ? ground->getID() : 0,
					s.casts, s.picks, std::max<int64_t>(0, (s.endsAtMs - OTSYS_TIME()) / 1000));
			}
			return result;
		}

		// ---- sequence <poi1,poi2,...>: Multi-leg navigation ----
		if (command.size() > 9 && command.substr(0, 9) == "sequence ") {
			if (!bot.active) return fmt::format("Bot '{}' is not active.", botName);
			auto args = command.substr(9);

			// Parse comma-separated POI names
			auto& pois = getCityPOIs();
			auto poiIt = pois.find(bot.townId);
			if (poiIt == pois.end()) return fmt::format("No POIs for town {}.", bot.townId);

			// Find the first POI in the sequence and navigate to it
			// (simplified: navigate to first POI; full sequence queue not yet implemented)
			std::istringstream ss(args);
			std::string poiName;
			std::vector<std::string> poiNames;
			while (std::getline(ss, poiName, ',')) {
				auto start = poiName.find_first_not_of(" \t");
				auto end = poiName.find_last_not_of(" \t");
				if (start != std::string::npos) {
					poiNames.push_back(poiName.substr(start, end - start + 1));
				}
			}

			if (poiNames.empty()) return "No POI names in sequence.";

			// Navigate to first POI
			std::string firstLower = poiNames[0];
			std::transform(firstLower.begin(), firstLower.end(), firstLower.begin(), ::tolower);
			const BotPOI* firstPoi = nullptr;
			for (const auto& poi : poiIt->second) {
				std::string poiNameLower = poi.name;
				std::transform(poiNameLower.begin(), poiNameLower.end(), poiNameLower.begin(), ::tolower);
				if ((firstLower == "depot" && poi.type == POIType::DEPOT) ||
					(firstLower == "temple" && poi.type == POIType::TEMPLE) ||
					(firstLower == "boat" && poi.type == POIType::BOAT) ||
					(firstLower == "shop" && poi.type == POIType::SHOP) ||
					poiNameLower.find(firstLower) != std::string::npos) {
					firstPoi = &poi;
					break;
				}
			}

			if (!firstPoi) return fmt::format("First POI '{}' not found.", poiNames[0]);

			bot.walkTarget = firstPoi->pos;
			bot.hasWalkTarget = true;
			bot.currentPOI = nullptr;
			bot.pathFailCount = 0;
			bot.state = BotAIState::IDLE;
			return fmt::format("Bot '{}' navigating to '{}' (first of {} POIs in sequence).", botName,
				firstPoi->name, poiNames.size());
		}

		// ---- routes: List available routes (from travel destinations) ----
		if (command == "routes") {
			auto& dests = getTravelDestinations();
			auto it = dests.find(bot.townId);
			if (it == dests.end()) return fmt::format("No travel routes from town {}.", bot.townId);

			std::string result = fmt::format("Routes from town {}:\n", bot.townId);
			for (uint32_t destId : it->second) {
				auto town = g_game().map.towns.getTown(destId);
				result += fmt::format("  → {} (town {})\n", town ? town->getName() : "?", destId);
			}
			return result;
		}

		return fmt::format("Unknown command: '{}'. Available per-bot: status, pos, info, stop, resume, "
			"active [x,y,z], inactive, hibernate, wake, "
			"hunt [name|id], endhunt, debug_kills <N>, partyhunt [scriptId], advstone [chest|dummy [wepId]|wp], "
			"house [houseId], houseinfo [houseId|near], "
			"travel <city> [wp#], goto x,y,z, teleport x,y,z, navigate <poi>, sequence <pois>, poi, routes, "
			"stairs up|down, step <dir>, scan [radius], pk <name>, "
			"use x,y,z, tileinfo x,y,z, scandoors [radius], placeitem <id> <x,y,z>, removeitem <id> <x,y,z>, "
			"log on|off, verbose on|off, debug on|off|status|grid on|off|events on|off|snapshot <ms>, "
			"probe on|off|status, probe teleport x,y,z. "
			"Global (no botname): reload [debug,N|debug off], datadump <tag>, routewp|routeadd|routedel, huntwp|huntadd|huntdel, "
			"hunttarget|targetadd|targetdel, poi|poiadd|poidel|poiupdate, whohunts [search], "
			"perfstat, perfphases, perfreset, probe list|clear, hibernateall, "
			"partyinfo, partystop <name>, schedule on|off|status, simulate route|hunt|poi.",
			command);
	}

	return fmt::format("Bot '{}' not found.", botName);
}

// Cancel every dispatcher event this engine still owns.
//
// MUST run before the engine is destroyed and libbot_engine.so is dlclose()d. A scheduled
// lambda defined in the .so is doubly unsafe once that happens: it captured `this` (freed by
// destroyBotEngine, which runs BEFORE dlclose), and its own code lives in the unmapped text
// segment. Firing one crashed the server — SIGSEGV in botStartAutoWalk's lambda, reached from
// Dispatcher::executeScheduledEvents, faulting inside guidToIndex_.find.
//
// stopEvent is a no-op for an id that already fired (the callback clears the field first).
//
// !!! ANY new g_dispatcher() scheduling call in a bot_*.cpp TU MUST either register its event
// !!! id here, or route through a core-binary trampoline like Game::restartBotTickLoop —
// !!! otherwise it silently reopens this crash. That is exactly how the original bug happened:
// !!! this drain loop existed and was correct, but the reload path never reached it.
void BotEngine::cancelPendingDispatcherEvents() {
	uint32_t cancelled = 0;
	for (auto& bot : bots_) {
		if (bot.pendingWalkPauseEventId != 0) {
			g_dispatcher().stopEvent(bot.pendingWalkPauseEventId);
			bot.pendingWalkPauseEventId = 0;
			++cancelled;
		}
	}
	walkPauseInfo_.clear();
	if (cancelled > 0) {
		// Positive proof the drain ran, so a clean reload is verifiable as "cancelled N" rather
		// than merely "did not crash this time".
		g_logger().info("[BotEngine] teardown: cancelled {} pending dispatcher event(s)", cancelled);
	}
}

void BotEngine::deactivateAll() {
	cancelPendingDispatcherEvents();
	for (auto& bot : bots_) {
		if (bot.active) {
			forceDeactivateBot(bot.guid);
		}
	}
}
