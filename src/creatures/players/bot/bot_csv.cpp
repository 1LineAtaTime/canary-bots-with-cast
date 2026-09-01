/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_csv.cpp — BOT_CSV: tolerant CSV reader, minimal-quoting writer, atomic
// file replace, strict enum tables, and the canonical authored-data dump.
//
// The parser is lenient-LEXICAL / strict-SEMANTIC (guide §2): it forgives
// anything an editor/OS/git can do to the bytes (BOM, CRLF/LF/lone-CR/mixed,
// blank lines, '#' comments, stray whitespace, header order/case) and refuses
// anything that changes meaning (unknown/missing header column, wrong field
// count, bad integer, unknown enum, unterminated quote). A refusal throws
// BotCsvError and the caller aborts the ENTIRE load — no skip-row mode exists.
//
// File-scope `static` is fine in THIS file (single TU); the shared surface is
// declared in bot_engine_impl.hpp per the inline-never-static rule.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#include <fcntl.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wunused-function"

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

std::string botCsvTrim(const std::string& s) {
	const size_t b = s.find_first_not_of(" \t");
	if (b == std::string::npos) {
		return {};
	}
	const size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

std::string botCsvLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return s;
}

std::string botCsvFormatTs(int64_t msEpoch) {
	if (msEpoch <= 0) {
		return "never";
	}
	const time_t secs = static_cast<time_t>(msEpoch / 1000);
	struct tm tmBuf {};
	localtime_r(&secs, &tmBuf);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
	return buf;
}

std::string BotCsvError::format() const {
	if (line == 0) {
		return fmt::format("{}: {}", file, reason);
	}
	if (col == 0) {
		return fmt::format("{}:{}: {}", file, line, reason);
	}
	return fmt::format("{}:{}:{}: {}", file, line, col, reason);
}

// ----------------------------------------------------------------------------
// Lexer — one physical record per call.
//
// Terminators: CRLF, LF and lone CR all end a record (mixed within one file is
// fine); a missing terminator on the last record is fine. Blank lines and
// lines whose first non-whitespace char is '#' are skipped between records.
// Unquoted fields are trimmed (provably lossless on today's data — the writer
// quotes any field a trim would change, keeping it true). Quoted fields are
// verbatim, "" -> ", and may span lines (terminators normalize to \n inside).
// Whitespace between a closing quote and the next delimiter is tolerated;
// anything else there, a quote mid-way through an unquoted field, or EOF
// inside quotes, is a hard error.
// ----------------------------------------------------------------------------

namespace {

bool lexRecord(const std::string& text, const std::string& file, size_t& pos, size_t& line,
               std::vector<std::string>& outFields, size_t& outLine) {
	const size_t n = text.size();

	// Skip blank lines and '#' comment lines. Every iteration either returns or advances
	// `pos` past a line terminator, so this always terminates.
	//
	// A '#' only starts a comment when it is the first non-blank character of a line AND
	// unquoted — a field legitimately beginning with '#' is quoted by botCsvField(), so
	// `"#1 spot",node,...` is data, not a comment.
	while (pos < n) {
		size_t p = pos;
		while (p < n && (text[p] == ' ' || text[p] == '\t')) {
			p++;
		}
		if (p >= n) {
			pos = n;
			return false; // trailing whitespace only
		}
		if (text[p] == '#') {
			while (p < n && text[p] != '\r' && text[p] != '\n') {
				p++;
			}
			if (p >= n) {
				pos = n;
				return false; // trailing comment with no terminator
			}
		}
		if (text[p] == '\r' || text[p] == '\n') {
			if (text[p] == '\r' && p + 1 < n && text[p + 1] == '\n') {
				p++;
			}
			pos = p + 1;
			line++;
			continue;
		}
		pos = p; // a real record starts here
		break;
	}
	if (pos >= n) {
		return false;
	}

	outFields.clear();
	outLine = line;

	while (true) {
		// ---- one field ----
		// leading whitespace (also the lead-in to a quoted field)
		size_t ws = pos;
		while (ws < n && (text[ws] == ' ' || text[ws] == '\t')) {
			ws++;
		}
		std::string field;
		if (ws < n && text[ws] == '"') {
			// quoted field — verbatim between quotes, "" -> "
			pos = ws + 1;
			bool closed = false;
			while (pos < n) {
				const char c = text[pos];
				if (c == '"') {
					if (pos + 1 < n && text[pos + 1] == '"') {
						field += '"';
						pos += 2;
						continue;
					}
					pos++;
					closed = true;
					break;
				}
				if (c == '\r') {
					field += '\n';
					pos++;
					if (pos < n && text[pos] == '\n') {
						pos++;
					}
					line++;
					continue;
				}
				if (c == '\n') {
					field += '\n';
					pos++;
					line++;
					continue;
				}
				field += c;
				pos++;
			}
			if (!closed) {
				throw BotCsvError { file, outLine, outFields.size() + 1, "unterminated quote" };
			}
			// OVERTURNED rule (guide §2.1): whitespace after the closing quote is
			// tolerated; any other content is still ambiguous data -> error.
			while (pos < n && (text[pos] == ' ' || text[pos] == '\t')) {
				pos++;
			}
			if (pos < n && text[pos] != ',' && text[pos] != '\r' && text[pos] != '\n') {
				throw BotCsvError { file, outLine, outFields.size() + 1,
					"content after closing quote (only whitespace is allowed there)" };
			}
			outFields.push_back(std::move(field));
		} else {
			// unquoted field — up to delimiter/terminator, then trimmed
			pos = ws; // leading whitespace already conceptually trimmed
			const size_t start = pos;
			while (pos < n && text[pos] != ',' && text[pos] != '\r' && text[pos] != '\n') {
				if (text[pos] == '"' && pos != start) {
					throw BotCsvError { file, outLine, outFields.size() + 1,
						"quote inside unquoted field (quote the whole field instead)" };
				}
				pos++;
			}
			outFields.push_back(botCsvTrim(text.substr(start, pos - start)));
		}
		// ---- delimiter or end of record ----
		if (pos >= n) {
			line++;
			return true; // missing final terminator: fine
		}
		const char c = text[pos];
		if (c == ',') {
			pos++;
			continue;
		}
		if (c == '\r' && pos + 1 < n && text[pos + 1] == '\n') {
			pos += 2;
		} else {
			pos++; // lone \r or \n
		}
		line++;
		return true;
	}
}

} // namespace

// ----------------------------------------------------------------------------
// BotCsvTable
// ----------------------------------------------------------------------------

BotCsvTable BotCsvTable::load(const std::string& path,
                              const std::vector<std::string>& requiredCols,
                              const std::vector<std::string>& optionalCols) {
	BotCsvTable t;
	t.file_ = path;

	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) {
		// Fail LOUD, unlike the chat loader's warn-and-disable: a missing authored file
		// means a wrong cwd or a botched deploy, never a legitimate empty state.
		// (The canary process cwd is /home/tibia/canary — guide §1.7(3).)
		throw BotCsvError { path, 0, 0, "cannot open file (missing? cwd must be the server root)" };
	}
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
	    && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.erase(0, 3); // UTF-8 BOM
	}

	size_t pos = 0;
	size_t line = 1;
	std::vector<std::string> fields;
	size_t recLine = 0;

	// ---- header ----
	if (!lexRecord(text, path, pos, line, fields, recLine)) {
		throw BotCsvError { path, 0, 0, "empty file — expected a header row" };
	}
	if (fields.size() > 1 && fields.back().empty()) {
		fields.pop_back(); // one trailing delimiter tolerated (guide §2.2)
	}
	// Check the whole-header SHAPE first. A ';'- or tab-delimited file lands as one giant
	// cell, and the per-column loop below would report it as "unrecognized header column
	// 'phase;waypoint_type;...'" — technically true, useless to a human. Delimiter
	// sniffing stays rejected (ambiguity is worse than a clear error); this precise
	// message is what replaces it.
	if (fields.size() == 1
	    && (fields[0].find(';') != std::string::npos || fields[0].find('\t') != std::string::npos)) {
		throw BotCsvError { path, recLine, 0,
			"the header looks ';'- or tab-delimited; the field separator must be a comma" };
	}
	std::vector<bool> ignoredSlot(fields.size(), false);
	std::vector<bool> haveRequired(requiredCols.size(), false);
	for (size_t i = 0; i < fields.size(); i++) {
		const std::string name = botCsvLower(botCsvTrim(fields[i]));
		if (name.empty()) {
			throw BotCsvError { path, recLine, i + 1, "empty header column name" };
		}
		if (name[0] == '_') {
			ignoredSlot[i] = true; // declared annotation column — accepted and ignored
			continue;
		}
		if (t.colIndex_.count(name)) {
			throw BotCsvError { path, recLine, i + 1, fmt::format("duplicate header column '{}'", name) };
		}
		bool known = false;
		for (size_t r = 0; r < requiredCols.size(); r++) {
			if (name == requiredCols[r]) {
				haveRequired[r] = true;
				known = true;
				break;
			}
		}
		if (!known) {
			for (const auto& o : optionalCols) {
				if (name == o) {
					known = true;
					break;
				}
			}
		}
		if (!known) {
			// KEPT STRICT (guide §2.2): a typo'd column silently ignored is the exact
			// silent-wrong-load class this migration removes.
			throw BotCsvError { path, recLine, i + 1,
				fmt::format("unrecognized header column '{}' (prefix with '_' if it is a deliberate annotation column)", name) };
		}
		t.colIndex_[name] = i;
	}
	for (size_t r = 0; r < requiredCols.size(); r++) {
		if (!haveRequired[r]) {
			std::string reason = fmt::format("missing required header column '{}'", requiredCols[r]);
			// Delimiter sniffing is REJECTED (guide §2.2) — but be kind about the
			// obvious case: a ';' or tab-delimited header lands as one giant cell.
			if (fields.size() == 1
			    && (fields[0].find(';') != std::string::npos || fields[0].find('\t') != std::string::npos)) {
				reason += " — the header looks ';'- or tab-delimited; the field separator must be a comma";
			}
			throw BotCsvError { path, recLine, 0, reason };
		}
	}
	const size_t expected = fields.size();

	// ---- data rows ----
	while (lexRecord(text, path, pos, line, fields, recLine)) {
		if (fields.size() == expected + 1 && fields.back().empty()) {
			fields.pop_back(); // one trailing delimiter tolerated
		}
		if (fields.size() != expected) {
			throw BotCsvError { path, recLine, 0,
				fmt::format("expected {} fields, got {}", expected, fields.size()) };
		}
		Row row;
		row.sourceLine = recLine;
		row.fields = fields;
		// Ignored (annotation) slots keep their data; raw() simply never resolves them.
		(void)ignoredSlot;
		t.rows_.push_back(std::move(row));
	}
	return t;
}

const std::string& BotCsvTable::raw(size_t row, const std::string& col) const {
	const auto it = colIndex_.find(col);
	if (it == colIndex_.end()) {
		return kEmpty; // optional column absent from this file
	}
	return rows_[row].fields[it->second];
}

int64_t BotCsvTable::getInt(size_t row, const std::string& col, int64_t minVal, int64_t maxVal) const {
	const std::string v = botCsvTrim(raw(row, col));
	if (v.empty()) {
		fail(row, col, "empty value in required integer column");
	}
	size_t i = 0;
	if (v[0] == '+' || v[0] == '-') {
		i = 1;
	}
	if (i >= v.size()) {
		fail(row, col, fmt::format("non-numeric value '{}'", v));
	}
	for (size_t k = i; k < v.size(); k++) {
		if (!std::isdigit(static_cast<unsigned char>(v[k]))) {
			fail(row, col, fmt::format("non-numeric value '{}'", v));
		}
	}
	int64_t out = 0;
	try {
		out = std::stoll(v);
	} catch (...) {
		fail(row, col, fmt::format("integer overflow in '{}'", v));
	}
	if (out < minVal || out > maxVal) {
		fail(row, col, fmt::format("value {} out of range [{}..{}]", out, minVal, maxVal));
	}
	return out;
}

int64_t BotCsvTable::getIntOr(size_t row, const std::string& col, int64_t minVal, int64_t maxVal, int64_t def) const {
	if (botCsvTrim(raw(row, col)).empty()) {
		return def;
	}
	return getInt(row, col, minVal, maxVal);
}

void BotCsvTable::fail(size_t row, const std::string& col, const std::string& reason) const {
	throw BotCsvError { file_, rows_[row].sourceLine, 0, fmt::format("column '{}': {}", col, reason) };
}

// ----------------------------------------------------------------------------
// Writer + atomic replace (Milestone 1 users: datadump; Milestone 2: the editor)
// ----------------------------------------------------------------------------

std::string botCsvField(const std::string& s) {
	bool quote = s.find_first_of(",\"\r\n") != std::string::npos;
	if (!quote && !s.empty()) {
		const char f = s.front();
		const char b = s.back();
		// Leading/trailing whitespace would be trimmed on read-back; '#' first would
		// read as a comment line. Quoting preserves both verbatim.
		quote = f == ' ' || f == '\t' || b == ' ' || b == '\t' || f == '#';
	}
	if (!quote) {
		return s;
	}
	std::string out = "\"";
	for (const char c : s) {
		if (c == '"') {
			out += '"';
		}
		out += c;
	}
	out += '"';
	return out;
}

bool botCsvAtomicWrite(const std::string& path, const std::string& content, std::string& errOut) {
	namespace fs = std::filesystem;
	const std::string tmp = path + ".tmp";

	// 1. write + fsync the temp file (same tmp-then-rename idiom as the z-graph cache
	//    writer at bot_zgraph.cpp:1757-1761, plus fsync of file AND directory).
	{
		const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			errOut = fmt::format("open('{}') failed: {}", tmp, strerror(errno));
			return false;
		}
		size_t off = 0;
		while (off < content.size()) {
			const ssize_t w = ::write(fd, content.data() + off, content.size() - off);
			if (w <= 0) {
				errOut = fmt::format("write('{}') failed: {}", tmp, strerror(errno));
				::close(fd);
				return false;
			}
			off += static_cast<size_t>(w);
		}
		if (::fsync(fd) != 0) {
			errOut = fmt::format("fsync('{}') failed: {}", tmp, strerror(errno));
			::close(fd);
			return false;
		}
		::close(fd);
	}

	// 2. one .bak generation (gitignored — Step 0)
	std::error_code ec;
	if (fs::exists(path, ec)) {
		fs::copy_file(path, path + ".bak", fs::copy_options::overwrite_existing, ec);
		// best-effort: a failed .bak must not block the write itself
	}

	// 3. atomic replace + directory fsync so the rename itself is durable
	fs::rename(tmp, path, ec);
	if (ec) {
		std::error_code ec2;
		fs::remove(tmp, ec2);
		errOut = fmt::format("rename('{}' -> '{}') failed: {}", tmp, path, ec.message());
		return false;
	}
	const std::string dir = fs::path(path).parent_path().string();
	const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
	if (dfd >= 0) {
		::fsync(dfd);
		::close(dfd);
	}
	return true;
}

// ----------------------------------------------------------------------------
// Strict enum table
// ----------------------------------------------------------------------------

WaypointType botCsvWaypointType(const BotCsvTable& t, size_t row, const std::string& col) {
	// STRICT: exactly the strings parseWaypointType (bot_engine_impl.hpp:1138) recognizes.
	// The DB enum also allowed 'lever'/'label'/'conditional' — zero live rows use them and
	// parseWaypointType silently mapped them (and any typo) to NODE. That silent fallback
	// is the bug this migration kills: unknown string here = hard load error.
	// parseWaypointType itself is untouched (it has other callers).
	static const std::unordered_map<std::string, WaypointType> kMap = {
		{ "node", WaypointType::NODE },
		{ "stand", WaypointType::STAND },
		{ "ladder", WaypointType::LADDER },
		{ "rope", WaypointType::ROPE },
		{ "hole", WaypointType::HOLE },
		{ "shovel", WaypointType::HOLE }, // alias kept: both mean dig-down
		{ "stairs_up", WaypointType::STAIRS_UP },
		{ "stairs_down", WaypointType::STAIRS_DOWN },
		{ "door", WaypointType::DOOR },
		{ "action", WaypointType::ACTION },
		{ "levitate_up", WaypointType::LEVITATE_UP },
		{ "levitate_down", WaypointType::LEVITATE_DOWN },
		{ "machete", WaypointType::MACHETE },
		{ "use_with", WaypointType::USE_WITH },
		{ "npc_interact", WaypointType::NPC_INTERACT },
		{ "teleport", WaypointType::TELEPORT },
	};
	const std::string v = botCsvLower(botCsvTrim(t.raw(row, col)));
	const auto it = kMap.find(v);
	if (it == kMap.end()) {
		t.fail(row, col, fmt::format("unknown waypoint_type '{}' (rejected: lever/label/conditional and typos — the old silent NODE fallback is dead)", v));
	}
	return it->second;
}

// ----------------------------------------------------------------------------
// Canonical dump — the cutover gate. Committed at Step 2, UNTOUCHED at Step 5.
// Sort keys make the output order-insensitive to container hash order EXCEPT
// where order is semantic (waypoint vectors; travel-position entry order, where
// index 0 = the canonical town entry).
// ----------------------------------------------------------------------------

std::string BotEngine::buildAuthoredDataDump() const {
	std::string out;
	out.reserve(1 << 22);
	const auto wpLine = [&out](const char* tag, const Waypoint& w) {
		out += fmt::format("{} type={} pos={},{},{} item={} extra={} fc={}\n",
			tag, static_cast<int>(w.type), w.pos.x, w.pos.y, w.pos.z,
			w.itemId, w.extraData, w.isWalkOnFc ? 1 : 0);
	};

	// hunt scripts, sorted by id
	std::vector<const HuntScript*> scripts;
	scripts.reserve(huntScripts_.size());
	for (const auto& s : huntScripts_) {
		scripts.push_back(&s);
	}
	std::sort(scripts.begin(), scripts.end(),
		[](const HuntScript* a, const HuntScript* b) { return a->id < b->id; });
	out += fmt::format("scripts n={}\n", scripts.size());
	for (const auto* s : scripts) {
		out += fmt::format(
			"script id={} name={} town={} lvl={}..{} vocmask={} kd={},{},{},{} quest={} cat={} group={}\n",
			s->id, s->name, s->townId, s->levelMin, s->levelMax, s->vocationMask,
			s->keepDistanceEK, s->keepDistanceMS, s->keepDistanceED, s->keepDistanceRP,
			s->isQuest ? 1 : 0, s->scriptCategory, s->spawnGroup);
		// BOT_LURE_KITE: appended ONLY when set. Emitting it unconditionally would
		// shift every line of the dump and destroy its value as a byte-identical
		// parity gate against the pre-feature build.
		if (s->minMonsters > 0) {
			out += fmt::format("script_lure id={} min={}\n", s->id, s->minMonsters);
		}
		for (const auto& w : s->travelToWaypoints) {
			wpLine(" t>", w);
		}
		for (const auto& w : s->patrolWaypoints) {
			wpLine(" p>", w);
		}
		for (const auto& w : s->travelFromWaypoints) {
			wpLine(" f>", w);
		}
		auto targets = s->targetNames;
		std::sort(targets.begin(), targets.end());
		// Manual join, not fmt::join — that needs <fmt/ranges.h>, and this TU should not
		// depend on whether the bot_engine PCH happens to pull it in.
		out += " targets=";
		for (size_t i = 0; i < targets.size(); i++) {
			if (i != 0) {
				out += '|';
			}
			out += targets[i];
		}
		out += '\n';
	}

	// city POIs: towns sorted, entries sorted by name (unique per town)
	std::vector<uint32_t> poiTowns;
	for (const auto& kv : cityPOIs_) {
		poiTowns.push_back(kv.first);
	}
	std::sort(poiTowns.begin(), poiTowns.end());
	for (const uint32_t townId : poiTowns) {
		auto entries = cityPOIs_.at(townId);
		std::sort(entries.begin(), entries.end(),
			[](const BotPOI& a, const BotPOI& b) { return a.name < b.name; });
		for (const auto& p : entries) {
			out += fmt::format("poi town={} name={} pos={},{},{} type={}\n",
				townId, p.name, p.pos.x, p.pos.y, p.pos.z, static_cast<int>(p.type));
		}
	}

	// route graphs: towns sorted; route POIs sorted by name; pairs sorted by (src,dst)
	std::vector<uint32_t> routeTowns;
	for (const auto& kv : cityRouteGraphs_) {
		routeTowns.push_back(kv.first);
	}
	std::sort(routeTowns.begin(), routeTowns.end());
	for (const uint32_t townId : routeTowns) {
		const auto& graph = cityRouteGraphs_.at(townId);
		std::vector<std::pair<std::string, Position>> pois(graph.pois.begin(), graph.pois.end());
		std::sort(pois.begin(), pois.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [name, pos] : pois) {
			out += fmt::format("routepoi town={} name={} pos={},{},{}\n",
				townId, name, pos.x, pos.y, pos.z);
		}
		std::vector<std::pair<std::string, std::string>> pairKeys;
		for (const auto& [src, dsts] : graph.pairs) {
			for (const auto& [dst, wps] : dsts) {
				pairKeys.emplace_back(src, dst);
			}
		}
		std::sort(pairKeys.begin(), pairKeys.end());
		for (const auto& [src, dst] : pairKeys) {
			const auto& wps = graph.pairs.at(src).at(dst);
			out += fmt::format("route town={} {}~{} n={}\n", townId, src, dst, wps.size());
			for (const auto& w : wps) {
				wpLine("  r>", w);
			}
		}
	}

	// global adventurer-stone route (vector order is semantic)
	out += fmt::format("advstone n={}\n", adventurerStoneRoute_.size());
	for (const auto& w : adventurerStoneRoute_) {
		wpLine(" a>", w);
	}

	// travel positions: towns sorted; ENTRY ORDER KEPT (index 0 = canonical entry)
	std::vector<uint32_t> travelTowns;
	for (const auto& kv : travelPositions_) {
		travelTowns.push_back(kv.first);
	}
	std::sort(travelTowns.begin(), travelTowns.end());
	for (const uint32_t townId : travelTowns) {
		const auto nameIt = travelTownNames_.find(townId);
		out += fmt::format("travel town={} name={} entries=", townId,
			nameIt != travelTownNames_.end() ? nameIt->second : "");
		for (const auto& [pos, poi] : travelPositions_.at(townId)) {
			out += fmt::format("({},{},{},{})", pos.x, pos.y, pos.z, poi);
		}
		out += '\n';
	}

	// equipment sorted by key (level*10+voc)
	std::vector<uint32_t> eqKeys;
	for (const auto& kv : equipmentData_) {
		eqKeys.push_back(kv.first);
	}
	std::sort(eqKeys.begin(), eqKeys.end());
	for (const uint32_t key : eqKeys) {
		const auto& e = equipmentData_.at(key);
		out += fmt::format("equip key={} head={} armor={} legs={} feet={} weapon={} shield={} backpack={}\n",
			key, e.head, e.armor, e.legs, e.feet, e.weapon, e.shield, e.backpack);
	}
	return out;
}
