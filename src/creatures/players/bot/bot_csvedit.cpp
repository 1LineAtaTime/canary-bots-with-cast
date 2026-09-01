/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_csvedit.cpp — BOT_CSV Milestone 2: in-game editing of the authored CSVs.
//
// These are the `_global` commands behind /cavebot huntadd|huntdel|routeadd|
// routedel|poiadd|poidel|poiupdate|targetadd|targetdel and the viewers. They
// replace the 46 direct db.query/db.storeQuery sites bot_cavebot.lua used to
// carry; the Lua side keeps argument parsing, town resolution and message
// formatting and hands us already-normalized, pipe-delimited arguments.
//
// THREE RULES, all load-bearing:
//
// 1. SEQ IS LINE ORDER. There is no seq column, so "insert at seq N" is
//    literally "insert a line at index N of that phase/route block". The
//    `UPDATE ... SET seq = seq +/- 1` renumbering pairs the Lua editor used to
//    run do not move here — they cease to exist.
//
// 2. NO AUTO-RELOAD. An edit writes the file and stops. Calling reloadBotData()
//    on the LIVE engine would refill the containers underneath running bots,
//    which is exactly the in-place path the migration avoided — and it would
//    resurrect the BotState::currentPOI use-after-free that the fresh-engine
//    design made structurally impossible. The admin runs /cavebot reload, which
//    is what every one of these commands has always told them to do.
//
// 3. READ-MODIFY-WRITE IS GUARDED. Every edit re-parses its target file with
//    the same strict parser the loader uses (a file that does not parse refuses
//    the edit), and re-checks the file's mtime immediately before the rename so
//    a concurrent `git pull` cannot be silently half-overwritten.
//
// No locking anywhere: executeCommand and tick() are both on the single
// game-logic dispatcher thread.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#pragma GCC diagnostic ignored "-Wunused-function"

namespace {

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

// Split on '|'. Verified safe against live data: zero script names, POI names,
// monster names or route src/dst segments contain a pipe.
std::vector<std::string> splitPipe(const std::string& s) {
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		const size_t p = s.find('|', start);
		if (p == std::string::npos) {
			out.push_back(botCsvTrim(s.substr(start)));
			break;
		}
		out.push_back(botCsvTrim(s.substr(start, p - start)));
		start = p + 1;
	}
	return out;
}

bool parsePos(const std::string& s, Position& out) {
	int x = 0, y = 0, z = 0;
	if (std::sscanf(s.c_str(), "%d,%d,%d", &x, &y, &z) != 3) {
		return false;
	}
	if (x < 0 || x > 65535 || y < 0 || y > 65535 || z < 0 || z > 15) {
		return false;
	}
	out = Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
	return true;
}

// ---------------------------------------------------------------------------
// Raw line access.
//
// Edits work on PHYSICAL LINES, not on parsed records, so blank lines, '#'
// phase banners and any comment a human added survive an edit untouched. The
// strict parse still runs first: it is the gate, not the transport.
// ---------------------------------------------------------------------------

struct RawFile {
	std::vector<std::string> lines;   // no trailing newline on each
	std::filesystem::file_time_type mtime {};
	bool ok = false;
};

RawFile readRaw(const std::string& path, std::string& err) {
	RawFile f;
	std::error_code ec;
	f.mtime = std::filesystem::last_write_time(path, ec);
	if (ec) {
		err = fmt::format("{}: cannot stat file", path);
		return f;
	}
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) {
		err = fmt::format("{}: cannot open file", path);
		return f;
	}
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	// Normalize CRLF/CR on read so an edited file comes back out as LF. Matches the
	// loader's tolerance and keeps a Windows-edited file from showing as one huge diff.
	std::string cur;
	for (size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		if (c == '\r') {
			if (i + 1 < text.size() && text[i + 1] == '\n') {
				i++;
			}
			f.lines.push_back(cur);
			cur.clear();
		} else if (c == '\n') {
			f.lines.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) {
		f.lines.push_back(cur);
	}
	f.ok = true;
	return f;
}

// Atomic write + the mtime re-check that closes the concurrent-pull window.
bool writeRaw(const std::string& path, const RawFile& f, std::string& err) {
	std::error_code ec;
	const auto now = std::filesystem::last_write_time(path, ec);
	if (ec) {
		err = fmt::format("{}: cannot re-stat file before write", path);
		return false;
	}
	if (now != f.mtime) {
		err = fmt::format("{}: file changed on disk during the edit (a git pull?) — "
			"edit REFUSED and discarded, nothing was written", path);
		return false;
	}
	std::string body;
	for (const auto& l : f.lines) {
		body += l;
		body += '\n';
	}
	if (!botCsvAtomicWrite(path, body, err)) {
		return false;
	}
	g_logger().warn("[BOT_CSV_EDIT] {} modified in-game — the server working tree is now dirty; "
		"commit+push from the server before the next deploy", path);
	return true;
}

// Enum validation for WRITES.
//
// The loader's strict enum runs at LOAD time, which is far too late for an editor:
// without this, `huntadd ... teleprot` cheerfully wrote a file that the next
// /cavebot reload would then refuse, poisoning the engine. An editor that can write
// a file its own loader rejects is worse than no editor. Caught by testing, not review.
//
// Kept in lockstep with botCsvWaypointType() in bot_csv.cpp — same strings, and the
// same deliberate exclusions (lever/label/conditional are DB-enum leftovers that
// nothing uses and that parseWaypointType silently mapped to NODE).
bool isValidWaypointType(const std::string& t) {
	static const std::unordered_set<std::string> kTypes = {
		"node", "stand", "ladder", "rope", "hole", "shovel", "stairs_up", "stairs_down",
		"door", "action", "levitate_up", "levitate_down", "machete", "use_with",
		"npc_interact", "teleport",
	};
	return kTypes.count(t) != 0;
}

bool isValidPoiType(const std::string& t) {
	static const std::unordered_set<std::string> kTypes = {
		"depot", "depot_outside", "temple", "boat", "shop", "npc", "adventurer_stone",
	};
	return kTypes.count(t) != 0;
}

// Is this physical line a real data record (not blank, not a '#' comment)?
bool isDataLine(const std::string& line) {
	const std::string t = botCsvTrim(line);
	return !t.empty() && t[0] != '#';
}

// Index of the header line (first data line in the file).
size_t headerIndex(const RawFile& f) {
	for (size_t i = 0; i < f.lines.size(); i++) {
		if (isDataLine(f.lines[i])) {
			return i;
		}
	}
	return 0;
}

// First field of a record line, unquoted+trimmed — enough to group rows by
// phase (hunt waypoints) or by source_name (route waypoints) without a full parse.
std::string firstField(const std::string& line) {
	const std::string t = botCsvTrim(line);
	if (t.empty()) {
		return {};
	}
	if (t[0] == '"') {
		std::string out;
		for (size_t i = 1; i < t.size(); i++) {
			if (t[i] == '"') {
				if (i + 1 < t.size() && t[i + 1] == '"') {
					out += '"';
					i++;
					continue;
				}
				break;
			}
			out += t[i];
		}
		return out;
	}
	const size_t c = t.find(',');
	return botCsvTrim(c == std::string::npos ? t : t.substr(0, c));
}

} // namespace

// ---------------------------------------------------------------------------
// Script / town resolution shared by the edit commands
// ---------------------------------------------------------------------------

// Resolve a hunt script by name (case-insensitive, exact first then unique
// substring) against hunt_scripts.csv. Returns 0 and fills `err` on miss or
// ambiguity. Reads the FILE, not huntScripts_, so it also finds disabled
// scripts and picks up unapplied edits — matching the old SQL behaviour.
uint32_t BotEngine::csvResolveScriptId(const std::string& name, std::string& err) const {
	const std::string path = fmt::format("{}/hunt_scripts.csv", BOT_AUTHORED_DIR);
	BotCsvTable t;
	try {
		t = BotCsvTable::load(path, { "id", "name" },
			{ "town_id", "min_level", "max_level", "vocation_mask", "keep_distance_ek",
			  "keep_distance_ms", "keep_distance_ed", "keep_distance_rp", "enabled",
			  "is_quest", "script_category", "source", "source_file", "town_name", "script_type",
			  "min_monsters" });
	} catch (const BotCsvError& e) {
		err = e.format();
		return 0;
	}
	const std::string want = botCsvLower(name);
	uint32_t exact = 0;
	std::vector<std::pair<uint32_t, std::string>> partial;
	for (size_t i = 0; i < t.rowCount(); i++) {
		const std::string n = t.raw(i, "name");
		const std::string ln = botCsvLower(n);
		const uint32_t id = static_cast<uint32_t>(t.getInt(i, "id", 1, UINT32_MAX));
		if (ln == want) {
			exact = id;
			break;
		}
		if (ln.find(want) != std::string::npos) {
			partial.emplace_back(id, n);
		}
	}
	if (exact != 0) {
		return exact;
	}
	if (partial.empty()) {
		err = fmt::format("no hunt script matching '{}'", name);
		return 0;
	}
	if (partial.size() > 1) {
		std::string list;
		for (size_t i = 0; i < partial.size() && i < 6; i++) {
			list += (i ? ", " : "") + partial[i].second;
		}
		err = fmt::format("'{}' is ambiguous ({} matches: {}{})", name, partial.size(), list,
			partial.size() > 6 ? ", ..." : "");
		return 0;
	}
	return partial[0].first;
}

// ---------------------------------------------------------------------------
// The command dispatcher. Returns true if `command` was one of ours.
// ---------------------------------------------------------------------------

bool BotEngine::handleCsvEditCommand(const std::string& command, std::string& reply) {
	const auto starts = [&command](const char* p) {
		const size_t n = std::strlen(p);
		return command.size() >= n && command.compare(0, n, p) == 0;
	};
	const auto argsAfter = [&command](const char* p) {
		return botCsvTrim(command.substr(std::strlen(p)));
	};

	// ===================================================================
	// huntwpadd <script>|<phase>|<seq>|<type>|<x,y,z>|<label>|<extra>
	// huntwpdel <script>|<phase>|<seq>
	// ===================================================================
	if (starts("csvhuntwpadd ") || starts("csvhuntwpdel ")) {
		const bool add = starts("csvhuntwpadd ");
		const auto a = splitPipe(argsAfter(add ? "csvhuntwpadd " : "csvhuntwpdel "));
		if (a.size() < 3) {
			reply = "usage: csvhuntwpadd <script>|<phase>|<seq>|<type>|<x,y,z>[|label|extra]";
			return true;
		}
		std::string err;
		const uint32_t sid = csvResolveScriptId(a[0], err);
		if (sid == 0) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		const std::string phase = botCsvLower(a[1]);
		if (phase != "travel_to" && phase != "hunt_patrol" && phase != "travel_from") {
			reply = fmt::format("[cavebot] invalid phase '{}'", a[1]);
			return true;
		}
		int seq = 0;
		try {
			seq = std::stoi(a[2]);
		} catch (...) {
			reply = fmt::format("[cavebot] invalid seq '{}'", a[2]);
			return true;
		}
		if (seq < 0) {
			reply = "[cavebot] seq must be >= 0";
			return true;
		}

		const std::string path = fmt::format("{}/hunt_waypoints/{}.csv", BOT_AUTHORED_DIR, sid);
		// GATE: the file must parse before we are allowed to touch it.
		try {
			BotCsvTable::load(path, { "phase", "waypoint_type", "pos_x", "pos_y", "pos_z" },
				{ "label", "extra_data" });
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] edit REFUSED — {} does not parse: {}", path, e.format());
			return true;
		}
		RawFile f = readRaw(path, err);
		if (!f.ok) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}

		// Locate the phase block by physical line. Line order IS seq, so the Nth data
		// line of the block is seq N; comments and blank lines are skipped when counting
		// but stay exactly where they are.
		const size_t hdr = headerIndex(f);
		std::vector<size_t> blockRows;   // indices of data lines in this phase
		size_t blockEnd = f.lines.size(); // insert point when appending past the end
		for (size_t i = hdr + 1; i < f.lines.size(); i++) {
			if (!isDataLine(f.lines[i])) {
				continue;
			}
			if (botCsvLower(firstField(f.lines[i])) == phase) {
				blockRows.push_back(i);
				blockEnd = i + 1;
			}
		}

		if (add) {
			const std::string type = a.size() > 3 && !a[3].empty() ? botCsvLower(a[3]) : "stand";
			if (!isValidWaypointType(type)) {
				reply = fmt::format("[cavebot] unknown waypoint type '{}' — refusing to write a "
					"row the loader would reject. Valid: node, stand, ladder, rope, hole, shovel, "
					"stairs_up, stairs_down, door, action, machete, use_with, npc_interact, "
					"teleport, levitate_up, levitate_down", type);
				return true;
			}
			Position pos;
			if (a.size() < 5 || !parsePos(a[4], pos)) {
				reply = "[cavebot] invalid or missing position (expected x,y,z)";
				return true;
			}
			const std::string label = a.size() > 5 ? a[5] : "";
			const std::string extra = a.size() > 6 ? a[6] : "";
			const std::string row = fmt::format("{},{},{},{},{},{},{}",
				botCsvField(phase), botCsvField(type), pos.x, pos.y, static_cast<int>(pos.z),
				botCsvField(label), botCsvField(extra));
			size_t at;
			if (blockRows.empty()) {
				// New phase block. Append at the end of the file; the loader requires
				// canonical block order, so warn if this would violate it rather than
				// writing a file that will not load.
				at = f.lines.size();
				const int rank = phase == "travel_to" ? 1 : (phase == "hunt_patrol" ? 2 : 3);
				int lastRank = 0;
				for (size_t i = hdr + 1; i < f.lines.size(); i++) {
					if (!isDataLine(f.lines[i])) {
						continue;
					}
					const std::string p = botCsvLower(firstField(f.lines[i]));
					lastRank = p == "travel_to" ? 1 : (p == "hunt_patrol" ? 2 : 3);
				}
				if (rank < lastRank) {
					reply = fmt::format("[cavebot] refusing: '{}' must come before the blocks already "
						"in the file (canonical order is travel_to, hunt_patrol, travel_from). "
						"Add it by hand, or add the first waypoint of each phase in order.", phase);
					return true;
				}
			} else if (static_cast<size_t>(seq) >= blockRows.size()) {
				at = blockEnd;   // append to the end of this phase block
			} else {
				at = blockRows[seq];
			}
			f.lines.insert(f.lines.begin() + static_cast<long>(at), row);
			if (!writeRaw(path, f, err)) {
				reply = fmt::format("[cavebot] {}", err);
				return true;
			}
			reply = fmt::format("[cavebot] Added [{}] ({},{},{}) at {} seq {} for script {} "
				"({} -> {} waypoints in phase). /cavebot reload to apply.",
				type, pos.x, pos.y, static_cast<int>(pos.z), phase,
				std::min<size_t>(static_cast<size_t>(seq), blockRows.size()), sid,
				blockRows.size(), blockRows.size() + 1);
			return true;
		}

		// delete
		if (blockRows.empty()) {
			reply = fmt::format("[cavebot] script {} has no waypoints in phase {}", sid, phase);
			return true;
		}
		if (static_cast<size_t>(seq) >= blockRows.size()) {
			reply = fmt::format("[cavebot] seq {} out of range: phase {} has {} waypoints (0..{})",
				seq, phase, blockRows.size(), blockRows.size() - 1);
			return true;
		}
		const std::string removed = f.lines[blockRows[seq]];
		f.lines.erase(f.lines.begin() + static_cast<long>(blockRows[seq]));
		if (!writeRaw(path, f, err)) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		reply = fmt::format("[cavebot] Deleted {} seq {} from script {} ({} -> {} waypoints in "
			"phase). Removed: {}. /cavebot reload to apply.",
			phase, seq, sid, blockRows.size(), blockRows.size() - 1, removed);
		return true;
	}

	// ===================================================================
	// routewpadd <townId>|<src>|<dst>|<seq>|<type>|<x,y,z>[|action_label]
	// routewpdel <townId>|<src>|<dst>|<seq>
	// ===================================================================
	if (starts("csvroutewpadd ") || starts("csvroutewpdel ")) {
		const bool add = starts("csvroutewpadd ");
		const auto a = splitPipe(argsAfter(add ? "csvroutewpadd " : "csvroutewpdel "));
		if (a.size() < 4) {
			reply = "usage: csvroutewpadd <townId>|<src>|<dst>|<seq>|<type>|<x,y,z>";
			return true;
		}
		uint32_t townId = 0;
		int seq = 0;
		try {
			townId = static_cast<uint32_t>(std::stoul(a[0]));
			seq = std::stoi(a[3]);
		} catch (...) {
			reply = "[cavebot] invalid townId or seq";
			return true;
		}
		if (seq < 0) {
			reply = "[cavebot] seq must be >= 0";
			return true;
		}

		// Find the route's source_name in city_routes.csv: the town's rows whose
		// "<town>|<src>~<dst>:" tail matches, so the caller never has to know the
		// prefix or the trailing colon.
		std::string err;
		const std::string routesPath = fmt::format("{}/city_routes.csv", BOT_AUTHORED_DIR);
		std::string sourceName;
		try {
			const auto rt = BotCsvTable::load(routesPath, { "town_id", "source_name", "enabled" }, {});
			const std::string wantTail = fmt::format("|{}~{}", botCsvLower(a[1]), botCsvLower(a[2]));
			for (size_t i = 0; i < rt.rowCount(); i++) {
				if (static_cast<uint32_t>(rt.getInt(i, "town_id", 0, UINT32_MAX)) != townId) {
					continue;
				}
				if (rt.getInt(i, "enabled", 0, 1) == 0) {
					continue;
				}
				std::string n = botCsvLower(rt.raw(i, "source_name"));
				if (!n.empty() && n.back() == ':') {
					n.pop_back();
				}
				if (n.size() >= wantTail.size()
				    && n.compare(n.size() - wantTail.size(), wantTail.size(), wantTail) == 0) {
					sourceName = rt.raw(i, "source_name");
					break;
				}
			}
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] edit REFUSED — {}", e.format());
			return true;
		}
		if (sourceName.empty()) {
			reply = fmt::format("[cavebot] No enabled route '{}~{}' for town {}.", a[1], a[2], townId);
			return true;
		}

		const std::string path = fmt::format("{}/city_route_waypoints/town_{}.csv",
			BOT_AUTHORED_DIR, townId);
		try {
			BotCsvTable::load(path, { "source_name", "waypoint_type", "pos_x", "pos_y", "pos_z" },
				{ "action_label" });
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] edit REFUSED — {} does not parse: {}", path, e.format());
			return true;
		}
		RawFile f = readRaw(path, err);
		if (!f.ok) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		const size_t hdr = headerIndex(f);
		std::vector<size_t> blockRows;
		size_t blockEnd = f.lines.size();
		for (size_t i = hdr + 1; i < f.lines.size(); i++) {
			if (!isDataLine(f.lines[i])) {
				continue;
			}
			if (firstField(f.lines[i]) == sourceName) {
				blockRows.push_back(i);
				blockEnd = i + 1;
			}
		}
		if (blockRows.empty()) {
			reply = fmt::format("[cavebot] route '{}' has no waypoint block in {}", sourceName, path);
			return true;
		}

		if (add) {
			const std::string type = a.size() > 4 && !a[4].empty() ? botCsvLower(a[4]) : "stand";
			if (!isValidWaypointType(type)) {
				reply = fmt::format("[cavebot] unknown waypoint type '{}' — refusing to write a "
					"row the loader would reject.", type);
				return true;
			}
			Position pos;
			if (a.size() < 6 || !parsePos(a[5], pos)) {
				reply = "[cavebot] invalid or missing position (expected x,y,z)";
				return true;
			}
			const std::string actionLabel = a.size() > 6 ? a[6] : "";
			const std::string row = fmt::format("{},{},{},{},{},{}",
				botCsvField(sourceName), botCsvField(type), pos.x, pos.y,
				static_cast<int>(pos.z), botCsvField(actionLabel));
			const size_t at = static_cast<size_t>(seq) >= blockRows.size() ? blockEnd : blockRows[seq];
			f.lines.insert(f.lines.begin() + static_cast<long>(at), row);
			if (!writeRaw(path, f, err)) {
				reply = fmt::format("[cavebot] {}", err);
				return true;
			}
			reply = fmt::format("[cavebot] Added wp [{}] ({},{},{}) at seq {} for {} ({} -> {}). "
				"/cavebot reload to apply.", type, pos.x, pos.y, static_cast<int>(pos.z),
				std::min<size_t>(static_cast<size_t>(seq), blockRows.size()), sourceName,
				blockRows.size(), blockRows.size() + 1);
			return true;
		}

		if (static_cast<size_t>(seq) >= blockRows.size()) {
			reply = fmt::format("[cavebot] seq {} out of range: route has {} waypoints (0..{})",
				seq, blockRows.size(), blockRows.size() - 1);
			return true;
		}
		if (blockRows.size() == 1) {
			reply = fmt::format("[cavebot] refusing: that is the LAST waypoint of '{}'. A route row "
				"with no waypoint block is invisible to the loader; delete the route from "
				"city_routes.csv instead if that is what you want.", sourceName);
			return true;
		}
		const std::string removed = f.lines[blockRows[seq]];
		f.lines.erase(f.lines.begin() + static_cast<long>(blockRows[seq]));
		if (!writeRaw(path, f, err)) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		reply = fmt::format("[cavebot] Deleted wp seq {} from {} ({} -> {}). Removed: {}. "
			"/cavebot reload to apply.", seq, sourceName, blockRows.size(),
			blockRows.size() - 1, removed);
		return true;
	}

	// ===================================================================
	// poiadd <townId>|<name>|<x,y,z>|<type>     poidel <townId>|<name>
	// poiupdate <townId>|<name>|<x,y,z>
	// ===================================================================
	if (starts("csvpoiadd ") || starts("csvpoidel ") || starts("csvpoiupdate ")) {
		const char* pfx = starts("csvpoiadd ") ? "csvpoiadd "
			: (starts("csvpoidel ") ? "csvpoidel " : "csvpoiupdate ");
		const std::string mode = starts("csvpoiadd ") ? "add"
			: (starts("csvpoidel ") ? "del" : "update");
		const auto a = splitPipe(argsAfter(pfx));
		if (a.size() < 2) {
			reply = "usage: csvpoiadd <townId>|<name>|<x,y,z>|<type>";
			return true;
		}
		uint32_t townId = 0;
		try {
			townId = static_cast<uint32_t>(std::stoul(a[0]));
		} catch (...) {
			reply = "[cavebot] invalid townId";
			return true;
		}
		const std::string name = a[1];
		if (name.empty()) {
			reply = "[cavebot] POI name may not be empty";
			return true;
		}

		const std::string path = fmt::format("{}/city_pois.csv", BOT_AUTHORED_DIR);
		std::string err;
		try {
			BotCsvTable::load(path,
				{ "town_id", "name", "pos_x", "pos_y", "pos_z", "poi_type", "enabled" }, { "weight" });
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] edit REFUSED — {} does not parse: {}", path, e.format());
			return true;
		}
		RawFile f = readRaw(path, err);
		if (!f.ok) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}

		// Identity is (town_id, name), matching the table's unique key.
		size_t found = SIZE_MAX;
		const size_t hdr = headerIndex(f);
		for (size_t i = hdr + 1; i < f.lines.size(); i++) {
			if (!isDataLine(f.lines[i])) {
				continue;
			}
			// town_id is field 0, name is field 1 — parse just those two.
			const std::string line = botCsvTrim(f.lines[i]);
			const size_t c1 = line.find(',');
			if (c1 == std::string::npos) {
				continue;
			}
			uint32_t t = 0;
			try {
				t = static_cast<uint32_t>(std::stoul(botCsvTrim(line.substr(0, c1))));
			} catch (...) {
				continue;
			}
			if (t != townId) {
				continue;
			}
			// name may be quoted; reuse the field reader on the remainder
			const std::string rest = line.substr(c1 + 1);
			if (botCsvLower(firstField(rest)) == botCsvLower(name)) {
				found = i;
				break;
			}
		}

		if (mode == "del") {
			if (found == SIZE_MAX) {
				reply = fmt::format("[cavebot] POI '{}' not found in town {}.", name, townId);
				return true;
			}
			f.lines.erase(f.lines.begin() + static_cast<long>(found));
			if (!writeRaw(path, f, err)) {
				reply = fmt::format("[cavebot] {}", err);
				return true;
			}
			reply = fmt::format("[cavebot] Deleted POI '{}' from town {}. /cavebot reload to apply.",
				name, townId);
			return true;
		}

		Position pos;
		if (a.size() < 3 || !parsePos(a[2], pos)) {
			reply = "[cavebot] invalid or missing position (expected x,y,z)";
			return true;
		}
		if (mode == "update") {
			if (found == SIZE_MAX) {
				reply = fmt::format("[cavebot] POI '{}' not found in town {}.", name, townId);
				return true;
			}
			// Preserve poi_type / weight / enabled exactly: rewrite only the 3 pos fields.
			const std::string old = botCsvTrim(f.lines[found]);
			std::vector<std::string> fields;
			{
				// minimal RFC-4180 split of the existing row
				std::string cur;
				bool q = false;
				for (size_t i = 0; i < old.size(); i++) {
					const char c = old[i];
					if (q) {
						if (c == '"') {
							if (i + 1 < old.size() && old[i + 1] == '"') {
								cur += '"';
								i++;
							} else {
								q = false;
							}
						} else {
							cur += c;
						}
					} else if (c == '"') {
						q = true;
					} else if (c == ',') {
						fields.push_back(cur);
						cur.clear();
					} else {
						cur += c;
					}
				}
				fields.push_back(cur);
			}
			if (fields.size() < 7) {
				reply = fmt::format("[cavebot] POI row for '{}' has {} fields, expected >= 7 — "
					"edit REFUSED", name, fields.size());
				return true;
			}
			fields[2] = std::to_string(pos.x);
			fields[3] = std::to_string(pos.y);
			fields[4] = std::to_string(static_cast<int>(pos.z));
			std::string row;
			for (size_t i = 0; i < fields.size(); i++) {
				row += (i ? "," : "") + botCsvField(fields[i]);
			}
			f.lines[found] = row;
			if (!writeRaw(path, f, err)) {
				reply = fmt::format("[cavebot] {}", err);
				return true;
			}
			reply = fmt::format("[cavebot] Updated POI '{}' to ({},{},{}) in town {}. "
				"/cavebot reload to apply.", name, pos.x, pos.y, static_cast<int>(pos.z), townId);
			return true;
		}

		// add
		if (found != SIZE_MAX) {
			reply = fmt::format("[cavebot] POI '{}' already exists in town {} — use poiupdate.",
				name, townId);
			return true;
		}
		const std::string type = a.size() > 3 && !a[3].empty() ? botCsvLower(a[3]) : "depot";
		if (!isValidPoiType(type)) {
			reply = fmt::format("[cavebot] unknown POI type '{}' — refusing to write a row the "
				"loader would reject. Valid: adventurer_stone, boat, depot, depot_outside, npc, "
				"shop, temple", type);
			return true;
		}
		const std::string row = fmt::format("{},{},{},{},{},{},,1", townId, botCsvField(name),
			pos.x, pos.y, static_cast<int>(pos.z), botCsvField(type));
		// Keep the file grouped by town: insert after the town's last row, else append.
		size_t at = f.lines.size();
		for (size_t i = hdr + 1; i < f.lines.size(); i++) {
			if (!isDataLine(f.lines[i])) {
				continue;
			}
			const std::string line = botCsvTrim(f.lines[i]);
			const size_t c1 = line.find(',');
			if (c1 == std::string::npos) {
				continue;
			}
			try {
				if (static_cast<uint32_t>(std::stoul(botCsvTrim(line.substr(0, c1)))) == townId) {
					at = i + 1;
				}
			} catch (...) {
			}
		}
		f.lines.insert(f.lines.begin() + static_cast<long>(at), row);
		if (!writeRaw(path, f, err)) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		reply = fmt::format("[cavebot] Added POI '{}' [{}] at ({},{},{}) for town {}. "
			"/cavebot reload to apply.", name, type, pos.x, pos.y, static_cast<int>(pos.z), townId);
		return true;
	}

	// ===================================================================
	// targetadd <script>|<monster>      targetdel <script>|<monster>
	// ===================================================================
	if (starts("csvtargetadd ") || starts("csvtargetdel ")) {
		const bool add = starts("csvtargetadd ");
		const auto a = splitPipe(argsAfter(add ? "csvtargetadd " : "csvtargetdel "));
		if (a.size() < 2 || a[1].empty()) {
			reply = "usage: csvtargetadd <script>|<monster name>";
			return true;
		}
		std::string err;
		const uint32_t sid = csvResolveScriptId(a[0], err);
		if (sid == 0) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		const std::string monster = a[1];
		const std::string path = fmt::format("{}/hunt_targets.csv", BOT_AUTHORED_DIR);
		try {
			BotCsvTable::load(path, { "script_id", "monster_name" }, {});
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] edit REFUSED — {} does not parse: {}", path, e.format());
			return true;
		}
		RawFile f = readRaw(path, err);
		if (!f.ok) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		const size_t hdr = headerIndex(f);
		size_t lastForScript = SIZE_MAX;
		size_t match = SIZE_MAX;
		uint32_t countForScript = 0;
		for (size_t i = hdr + 1; i < f.lines.size(); i++) {
			if (!isDataLine(f.lines[i])) {
				continue;
			}
			const std::string line = botCsvTrim(f.lines[i]);
			const size_t c1 = line.find(',');
			if (c1 == std::string::npos) {
				continue;
			}
			uint32_t rid = 0;
			try {
				rid = static_cast<uint32_t>(std::stoul(botCsvTrim(line.substr(0, c1))));
			} catch (...) {
				continue;
			}
			if (rid != sid) {
				continue;
			}
			lastForScript = i;
			countForScript++;
			if (botCsvLower(firstField(line.substr(c1 + 1))) == botCsvLower(monster)) {
				match = i;
			}
		}

		if (add) {
			if (match != SIZE_MAX) {
				reply = fmt::format("[cavebot] '{}' is already a target of script {}.", monster, sid);
				return true;
			}
			const std::string row = fmt::format("{},{}", sid, botCsvField(monster));
			const size_t at = lastForScript == SIZE_MAX ? f.lines.size() : lastForScript + 1;
			f.lines.insert(f.lines.begin() + static_cast<long>(at), row);
			if (!writeRaw(path, f, err)) {
				reply = fmt::format("[cavebot] {}", err);
				return true;
			}
			reply = fmt::format("[cavebot] Added target '{}' to script {} ({} -> {} target rows). "
				"/cavebot reload to apply.", monster, sid, countForScript, countForScript + 1);
			return true;
		}
		if (match == SIZE_MAX) {
			reply = fmt::format("[cavebot] '{}' is not a target of script {}.", monster, sid);
			return true;
		}
		f.lines.erase(f.lines.begin() + static_cast<long>(match));
		if (!writeRaw(path, f, err)) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		reply = fmt::format("[cavebot] Deleted target '{}' from script {} ({} target rows remaining). "
			"/cavebot reload to apply.", monster, sid, countForScript - 1);
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// Viewers — read the FILES, not the loaded containers.
//
// Deliberate: the files show UNAPPLIED edits (the same semantics the old SQL
// viewers had, since they read the tables the editor wrote), and they can show
// `label` / `action_label`, which are NOT in the ABI Waypoint struct. Adding a
// field there would force a full binary rebuild; reading the file is free.
// ---------------------------------------------------------------------------

bool BotEngine::handleCsvViewCommand(const std::string& command, std::string& reply) {
	const auto starts = [&command](const char* p) {
		const size_t n = std::strlen(p);
		return command.size() >= n && command.compare(0, n, p) == 0;
	};
	const auto argsAfter = [&command](const char* p) {
		return botCsvTrim(command.substr(std::strlen(p)));
	};
	// Cap output: these go back through a talkaction message, and a 1,854-waypoint
	// script would otherwise flood the client off the screen.
	constexpr size_t kMaxRows = 60;

	// ---- csvhuntwp <script>[|<phase>] ----
	if (starts("csvhuntwp ")) {
		const auto a = splitPipe(argsAfter("csvhuntwp "));
		std::string err;
		const uint32_t sid = csvResolveScriptId(a[0], err);
		if (sid == 0) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		const std::string wantPhase = a.size() > 1 ? botCsvLower(a[1]) : "";
		BotCsvTable t;
		try {
			t = BotCsvTable::load(fmt::format("{}/hunt_waypoints/{}.csv", BOT_AUTHORED_DIR, sid),
				{ "phase", "waypoint_type", "pos_x", "pos_y", "pos_z" }, { "label", "extra_data" });
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] {}", e.format());
			return true;
		}
		std::string out = fmt::format("[cavebot] script {} waypoints (file: hunt_waypoints/{}.csv)\n",
			sid, sid);
		std::unordered_map<std::string, uint32_t> seqOf;
		size_t shown = 0, total = 0;
		for (size_t i = 0; i < t.rowCount(); i++) {
			const std::string ph = botCsvLower(t.raw(i, "phase"));
			const uint32_t seq = seqOf[ph]++;
			if (!wantPhase.empty() && ph != wantPhase) {
				continue;
			}
			total++;
			if (shown >= kMaxRows) {
				continue;
			}
			shown++;
			const std::string label = t.raw(i, "label");
			const std::string extra = t.raw(i, "extra_data");
			out += fmt::format("  {} [{}] {} ({},{},{}){}{}\n", seq, ph, t.raw(i, "waypoint_type"),
				t.raw(i, "pos_x"), t.raw(i, "pos_y"), t.raw(i, "pos_z"),
				label.empty() ? "" : " " + label,
				extra.empty() ? "" : " extra=" + extra);
		}
		if (total > shown) {
			out += fmt::format("  ... {} more (showing first {})\n", total - shown, shown);
		}
		reply = out;
		return true;
	}

	// ---- csvhunttarget <script> ----
	if (starts("csvhunttarget ")) {
		std::string err;
		const uint32_t sid = csvResolveScriptId(argsAfter("csvhunttarget "), err);
		if (sid == 0) {
			reply = fmt::format("[cavebot] {}", err);
			return true;
		}
		BotCsvTable t;
		try {
			t = BotCsvTable::load(fmt::format("{}/hunt_targets.csv", BOT_AUTHORED_DIR),
				{ "script_id", "monster_name" }, {});
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] {}", e.format());
			return true;
		}
		std::string out = fmt::format("[cavebot] targets of script {} (file: hunt_targets.csv)\n", sid);
		uint32_t n = 0;
		for (size_t i = 0; i < t.rowCount(); i++) {
			if (static_cast<uint32_t>(t.getInt(i, "script_id", 1, UINT32_MAX)) != sid) {
				continue;
			}
			out += fmt::format("  {}\n", t.raw(i, "monster_name"));
			n++;
		}
		if (n == 0) {
			out += "  (none — the script hunts whatever it meets)\n";
		}
		reply = out;
		return true;
	}

	// ---- csvpoi <townId> ----
	if (starts("csvpoi ")) {
		uint32_t townId = 0;
		try {
			townId = static_cast<uint32_t>(std::stoul(argsAfter("csvpoi ")));
		} catch (...) {
			reply = "[cavebot] invalid townId";
			return true;
		}
		BotCsvTable t;
		try {
			t = BotCsvTable::load(fmt::format("{}/city_pois.csv", BOT_AUTHORED_DIR),
				{ "town_id", "name", "pos_x", "pos_y", "pos_z", "poi_type", "enabled" }, { "weight" });
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] {}", e.format());
			return true;
		}
		std::string out = fmt::format("[cavebot] POIs for town {} (file: city_pois.csv)\n", townId);
		uint32_t n = 0;
		for (size_t i = 0; i < t.rowCount(); i++) {
			if (static_cast<uint32_t>(t.getInt(i, "town_id", 0, UINT32_MAX)) != townId) {
				continue;
			}
			out += fmt::format("  {} [{}] ({},{},{}){}\n", t.raw(i, "name"), t.raw(i, "poi_type"),
				t.raw(i, "pos_x"), t.raw(i, "pos_y"), t.raw(i, "pos_z"),
				t.getInt(i, "enabled", 0, 1) == 0 ? " DISABLED" : "");
			n++;
		}
		if (n == 0) {
			out += "  (none)\n";
		}
		reply = out;
		return true;
	}

	// ---- csvroutewp <townId>[|<src>~<dst>] ----
	if (starts("csvroutewp ")) {
		const auto a = splitPipe(argsAfter("csvroutewp "));
		uint32_t townId = 0;
		try {
			townId = static_cast<uint32_t>(std::stoul(a[0]));
		} catch (...) {
			reply = "[cavebot] invalid townId";
			return true;
		}
		// No pair given: list the town routes from city_routes.csv.
		if (a.size() < 2 || a[1].empty()) {
			BotCsvTable rt;
			try {
				rt = BotCsvTable::load(fmt::format("{}/city_routes.csv", BOT_AUTHORED_DIR),
					{ "town_id", "source_name", "enabled" }, {});
			} catch (const BotCsvError& e) {
				reply = fmt::format("[cavebot] {}", e.format());
				return true;
			}
			std::string out = fmt::format("[cavebot] routes for town {} (file: city_routes.csv)\n", townId);
			uint32_t n = 0;
			for (size_t i = 0; i < rt.rowCount(); i++) {
				if (static_cast<uint32_t>(rt.getInt(i, "town_id", 0, UINT32_MAX)) != townId) {
					continue;
				}
				if (n < kMaxRows) {
					out += fmt::format("  {}{}\n", rt.raw(i, "source_name"),
						rt.getInt(i, "enabled", 0, 1) == 0 ? " DISABLED" : "");
				}
				n++;
			}
			if (n > kMaxRows) {
				out += fmt::format("  ... {} more\n", n - kMaxRows);
			}
			if (n == 0) {
				out += "  (none)\n";
			}
			reply = out;
			return true;
		}
		// Pair given: dump that route waypoints from the town file.
		std::string src = a[1], dst;
		const size_t tilde = src.find('~');
		if (tilde == std::string::npos) {
			reply = "[cavebot] expected src~dst";
			return true;
		}
		dst = botCsvLower(src.substr(tilde + 1));
		src = botCsvLower(src.substr(0, tilde));
		BotCsvTable t;
		try {
			t = BotCsvTable::load(fmt::format("{}/city_route_waypoints/town_{}.csv",
					BOT_AUTHORED_DIR, townId),
				{ "source_name", "waypoint_type", "pos_x", "pos_y", "pos_z" }, { "action_label" });
		} catch (const BotCsvError& e) {
			reply = fmt::format("[cavebot] {}", e.format());
			return true;
		}
		const std::string wantTail = fmt::format("|{}~{}", src, dst);
		std::string out = fmt::format("[cavebot] route {}~{} in town {} "
			"(file: city_route_waypoints/town_{}.csv)\n", src, dst, townId, townId);
		uint32_t seq = 0, total = 0;
		for (size_t i = 0; i < t.rowCount(); i++) {
			std::string n = botCsvLower(t.raw(i, "source_name"));
			if (!n.empty() && n.back() == ':') {
				n.pop_back();
			}
			if (n.size() < wantTail.size()
			    || n.compare(n.size() - wantTail.size(), wantTail.size(), wantTail) != 0) {
				continue;
			}
			total++;
			if (seq < kMaxRows) {
				const std::string al = t.raw(i, "action_label");
				out += fmt::format("  {} {} ({},{},{}){}\n", seq, t.raw(i, "waypoint_type"),
					t.raw(i, "pos_x"), t.raw(i, "pos_y"), t.raw(i, "pos_z"),
					al.empty() ? "" : " " + al);
			}
			seq++;
		}
		if (total == 0) {
			out += "  (no waypoints — check the route name with /cavebot routewp <town>)\n";
		} else if (total > kMaxRows) {
			out += fmt::format("  ... {} more\n", total - kMaxRows);
		}
		reply = out;
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// csvpositions — machine-readable list, one record per line:
//     x,y,z,<type>[,<name>]
// The name field is LAST so a comma inside it cannot shift any earlier field.
//
// Exists for /cavebot simulate, which walks an admin through a waypoint list by
// teleporting them. The human-readable viewers above are for eyeballing; this
// one is for parsing, so it stays free of decoration (an "ERR <reason>" first
// line on failure).
// ---------------------------------------------------------------------------

bool BotEngine::handleCsvPositionsCommand(const std::string& command, std::string& reply) {
	const char* kPfx = "csvpositions ";
	if (command.compare(0, std::strlen(kPfx), kPfx) != 0) {
		return false;
	}
	const auto a = splitPipe(botCsvTrim(command.substr(std::strlen(kPfx))));
	const std::string kind = a.empty() ? "" : botCsvLower(a[0]);
	std::string out;

	try {
		if (kind == "hunt" && a.size() >= 2) {
			std::string err;
			const uint32_t sid = csvResolveScriptId(a[1], err);
			if (sid == 0) {
				reply = "ERR " + err;
				return true;
			}
			const std::string wantPhase = a.size() > 2 ? botCsvLower(a[2]) : "";
			const auto t = BotCsvTable::load(
				fmt::format("{}/hunt_waypoints/{}.csv", BOT_AUTHORED_DIR, sid),
				{ "phase", "waypoint_type", "pos_x", "pos_y", "pos_z" }, { "label", "extra_data" });
			for (size_t i = 0; i < t.rowCount(); i++) {
				if (!wantPhase.empty() && botCsvLower(t.raw(i, "phase")) != wantPhase) {
					continue;
				}
				out += fmt::format("{},{},{},{}\n", t.raw(i, "pos_x"), t.raw(i, "pos_y"),
					t.raw(i, "pos_z"), t.raw(i, "waypoint_type"));
			}
		} else if (kind == "route" && a.size() >= 4) {
			uint32_t townId = 0;
			try {
				townId = static_cast<uint32_t>(std::stoul(a[1]));
			} catch (...) {
				reply = "ERR invalid townId";
				return true;
			}
			const std::string wantTail = fmt::format("|{}~{}", botCsvLower(a[2]), botCsvLower(a[3]));
			const auto t = BotCsvTable::load(
				fmt::format("{}/city_route_waypoints/town_{}.csv", BOT_AUTHORED_DIR, townId),
				{ "source_name", "waypoint_type", "pos_x", "pos_y", "pos_z" }, { "action_label" });
			for (size_t i = 0; i < t.rowCount(); i++) {
				std::string n = botCsvLower(t.raw(i, "source_name"));
				if (!n.empty() && n.back() == ':') {
					n.pop_back();
				}
				if (n.size() < wantTail.size()
				    || n.compare(n.size() - wantTail.size(), wantTail.size(), wantTail) != 0) {
					continue;
				}
				out += fmt::format("{},{},{},{}\n", t.raw(i, "pos_x"), t.raw(i, "pos_y"),
					t.raw(i, "pos_z"), t.raw(i, "waypoint_type"));
			}
		} else if (kind == "poi" && a.size() >= 2) {
			uint32_t townId = 0;
			try {
				townId = static_cast<uint32_t>(std::stoul(a[1]));
			} catch (...) {
				reply = "ERR invalid townId";
				return true;
			}
			const auto t = BotCsvTable::load(fmt::format("{}/city_pois.csv", BOT_AUTHORED_DIR),
				{ "town_id", "name", "pos_x", "pos_y", "pos_z", "poi_type", "enabled" }, { "weight" });
			for (size_t i = 0; i < t.rowCount(); i++) {
				if (static_cast<uint32_t>(t.getInt(i, "town_id", 0, UINT32_MAX)) != townId) {
					continue;
				}
				if (t.getInt(i, "enabled", 0, 1) == 0) {
					continue;
				}
				out += fmt::format("{},{},{},{},{}\n", t.raw(i, "pos_x"), t.raw(i, "pos_y"),
					t.raw(i, "pos_z"), t.raw(i, "poi_type"), t.raw(i, "name"));
			}
		} else {
			reply = "ERR usage: csvpositions hunt|<script>[|phase]  route|<townId>|<src>|<dst>  poi|<townId>";
			return true;
		}
	} catch (const BotCsvError& e) {
		reply = "ERR " + e.format();
		return true;
	}

	reply = out.empty() ? "ERR no positions found" : out;
	return true;
}

// ---------------------------------------------------------------------------
// csvcheck — parse the whole authored tree into throwaway temporaries and
// report OK or the first error. Mutates NOTHING.
//
// This is the /cavebot reload pre-flight (guide §4.1). It exists because
// BotEngineLoader::reload() destroys the engine BEFORE loadHuntData() parses
// anything, so a malformed CSV has no previous data to fall back on: it poisons
// the fresh engine and every bot refuses to activate until someone fixes the
// file. Running the same parse on the STILL-LIVE engine first turns that
// outage into a refused reload with the population untouched.
//
// bot_system.lua calls this over the existing Game.botCommand("_global", ...)
// channel before it force-deactivates anything — no new talkaction, no new
// IBotEngine virtual, nothing an admin has to learn.
//
// CAVEAT worth knowing: the check runs in the CURRENTLY LOADED .so. If the .so
// itself is changing in this deploy, this validates with the previous parser.
// It is a sanity check, not a proof — tools/bot_csv/validate.py is the real
// gate, and it runs before the files ever reach the server.
// ---------------------------------------------------------------------------

bool BotEngine::handleCsvCheckCommand(const std::string& command, std::string& reply) {
	if (command != "csvcheck") {
		return false;
	}
	// Suppress the loaders' own logging for this throwaway pass, and restore it even
	// if a loader throws — otherwise one bad file would silence every later real load.
	struct QuietGuard {
		bool& flag;
		explicit QuietGuard(bool& f) : flag(f) {
			flag = true;
		}
		~QuietGuard() {
			flag = false;
		}
	} guard(csvQuiet_);

	std::vector<HuntScript> scripts;
	std::unordered_map<uint32_t, std::vector<BotPOI>> pois;
	std::unordered_map<uint32_t, CityRouteGraph> graphs;
	std::vector<Waypoint> advStone;
	std::unordered_map<uint32_t, std::vector<std::pair<Position, std::string>>> travelPos;
	std::unordered_map<uint32_t, std::string> travelNames;
	std::unordered_map<uint32_t, BotEquipment> equipment;

	// botMonoMs(), not OTSYS_TIME(): OTSYS_TIME is the dispatcher's cached tick clock, so
	// both reads land in the same cycle and every check would report "0ms" — which is
	// what the first version did for a 45,090-waypoint parse.
	const int64_t t0 = botMonoMs();
	try {
		if (!std::filesystem::is_directory(BOT_AUTHORED_DIR)) {
			reply = fmt::format("{}: authored data directory missing (cwd must be the server root)",
				BOT_AUTHORED_DIR);
			return true;
		}
		const uint32_t nextId = loadMetaCsv();
		loadHuntScriptsCsv(scripts, nextId);
		loadCityPOIsCsv(pois);
		loadCityRoutesCsv(graphs, advStone);
		loadTravelPositionsCsv(travelPos, travelNames);
		loadEquipmentCsv(equipment);
	} catch (const BotCsvError& e) {
		// Returned verbatim: bot_system.lua aborts the reload on anything not "OK".
		reply = e.format();
		return true;
	}

	size_t pairs = 0;
	for (const auto& kv : graphs) {
		pairs += kv.second.pairs.size();
	}
	reply = fmt::format("OK scripts={} poiTowns={} routeTowns={} routeSrc={} travelTowns={} "
		"equip={} in {}ms", scripts.size(), pois.size(), graphs.size(), pairs,
		travelPos.size(), equipment.size(), botMonoMs() - t0);
	return true;
}
