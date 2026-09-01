/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// bot_engine_impl.hpp — INTERNAL implementation header for libbot_engine.so.
//
// BOT_NAV_REALISM Phase 11 (module split). This holds the shared includes, the
// engine-local types/constants, and the `class BotEngine` declaration, so the
// engine can be built from several translation units that all compile into the
// SAME single .so — the hot-reload story is unchanged.
//
// This is NOT the ABI boundary. That is still bot_engine_interface.hpp, which
// the main binary sees. Nothing here is visible outside the .so, so adding a
// member to BotEngine is still not an ABI change.
//
// Translation units: bot_engine.cpp (core/nav/hunt/combat/...), bot_chat.cpp.
// ============================================================================

/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bot/bot_engine_interface.hpp"
#include "creatures/players/bot/bot_pathcore.hpp" // BOT_NAV_REALISM Phase 2: dependency-free A* kernel
#include "creatures/players/bot/bot_navdump.hpp" // BOT_NAV_REALISM Phase 3: offline-sim map dump format
#include "creatures/players/bot/bot_zcore.hpp" // TRUE MULTI-FLOOR: dependency-free portal graph + z-route planner
#include "creatures/players/imbuements/imbuements.hpp"
#include "io/iologindata.hpp"
#include "creatures/creature.hpp"
#include "creatures/players/player.hpp"
#include "creatures/players/vocations/vocation.hpp"
#include "creatures/monsters/monster.hpp"
#include "creatures/monsters/monsters.hpp"
#include "creatures/npcs/npcs.hpp"
#include "creatures/npcs/npc.hpp" // Phase 8: live Npc instances (getName/getPosition) for approach anchors
#include "game/game.hpp"
#include "map/map.hpp"
#include "map/utils/astarnodes.hpp"
#include "map/spectators.hpp"
#include "items/tile.hpp"
#include "lib/logging/logger.hpp"
#include "utils/tools.hpp"
#include "database/database.hpp"
#include "database/botdatabasetasks.hpp" // bundle 6: dedicated bot-DB worker (was databasetasks.hpp)
#include "utils/const.hpp"
#include "lua/creature/actions.hpp"
#include "lua/creature/movement.hpp"
#include "creatures/combat/spells.hpp"
#include "creatures/players/grouping/party.hpp"
#include "config/configmanager.hpp"
#include "items/weapons/weapons.hpp"
#include <unordered_set>
#include <queue>
#include <deque> // BOT_PARTY_TRAIL_FOLLOW: LeaderTrail breadcrumb ring
#include <tuple>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "creatures/interactions/chat.hpp"
#include "game/scheduling/dispatcher.hpp"
#include <numeric>  // std::iota (PERF_INVESTIGATION_2026-05-24 Phase B union-find)
#include <fstream>
#include <regex>
#include <chrono> // botMonoMs (JITTER FIX 2026-06-10)

// JITTER FIX 2026-06-10: OTSYS_TIME() returns a value cached once per dispatcher
// cycle — every duration measured with it inside one tick/task reads 0 (TICK_SLOW,
// PROCBOT_SLOW, WAKE_SLOW, HIB_SLOW and the Phase-A virtualTick budget were all
// structurally dead). Jitter instrumentation + the VT budget use this real
// monotonic clock instead; game-logic timestamps stay on the cached OTSYS_TIME().
static inline int64_t botMonoMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// State name lookup for BotAIState (used in logs, status, etc.)
static const char* botStateName(BotAIState state) {
	static const char* names[] = {
		"INACTIVE", "IDLE", "DWELLING", "COMBAT", "FLEEING", "TRAVELING", "PK_ATTACK", "HUNTING", "PARTY"
	};
	auto idx = static_cast<uint8_t>(state);
	return idx < 9 ? names[idx] : "UNKNOWN";
}

static const char* huntPhaseName(HuntPhase phase) {
	static const char* names[] = { "PREPARING", "TRAVEL_TO", "PATROLLING", "LEAVING", "RESUPPLYING" };
	auto idx = static_cast<uint8_t>(phase);
	return idx < 5 ? names[idx] : "UNKNOWN";
}

static const char* poiTypeName(POIType type) {
	static const char* names[] = { "depot", "depot_outside", "temple", "boat", "shop", "npc" };
	auto idx = static_cast<uint8_t>(type);
	return idx < 6 ? names[idx] : "unknown";
}

// Debug stream helpers (used by castSpell and other call sites mid-file).
// Full definitions in the implementation block at end of file.
struct BotDebugCfg;
static const char* dirShort(Direction d) {
	switch (d) {
		case DIRECTION_NORTH: return "N";
		case DIRECTION_EAST:  return "E";
		case DIRECTION_SOUTH: return "S";
		case DIRECTION_WEST:  return "W";
		case DIRECTION_NORTHEAST: return "NE";
		case DIRECTION_NORTHWEST: return "NW";
		case DIRECTION_SOUTHEAST: return "SE";
		case DIRECTION_SOUTHWEST: return "SW";
		default: return "?";
	}
}
static const char* aoeAreaTypeName(AoeAreaType t) {
	switch (t) {
		case AoeAreaType::CIRCLE:       return "CIRCLE";
		case AoeAreaType::WAVE4:        return "WAVE4";
		case AoeAreaType::SQUAREWAVE5:  return "SQUAREWAVE5";
		case AoeAreaType::MELEE_CIRCLE: return "MELEE_CIRCLE";
		case AoeAreaType::BEAM5:        return "BEAM5";
		case AoeAreaType::RING:         return "RING";
	}
	return "?";
}

// Returns the element resistance percentage for a creature against a combat type.
// 0 = neutral, 100 = immune, -10 = 10% weakness, 50 = 50% reduction.
// For non-monsters (players), returns 0 (no element resistance).
static int32_t getElementResistance(const std::shared_ptr<Creature>& creature, CombatType_t combatType) {
	if (creature->isImmune(combatType)) return 100;
	auto monster = creature->getMonster();
	if (monster) {
		auto& elementMap = monster->getMonsterType()->info.elementMap;
		auto it = elementMap.find(combatType);
		if (it != elementMap.end()) return it->second;
	}
	return 0;
}

// ============================================================================
// Lua Spell/Rune File Parser — extracts combat metadata at init
// ============================================================================

struct ParsedSpellMeta {
	std::string words;
	CombatType_t combatType = COMBAT_NONE;
	bool isAoe = false;
	std::string areaPattern;          // primary arg to createCombatArea — e.g. "AREA_SHORTWAVE3"
	std::string diagonalAreaPattern;  // optional second arg — e.g. "AREADIAGONAL_WAVE4"
	bool usesSkillFormula = false;
	double minMlCoef = 0, maxMlCoef = 0;
	double minConst = 0, maxConst = 0;
};

struct ParsedRuneMeta {
	uint16_t runeId = 0;
	CombatType_t combatType = COMBAT_NONE;
	bool isAoe = false;
	std::string areaPattern;
	std::string diagonalAreaPattern;
	double minMlCoef = 0, maxMlCoef = 0;
	double minConst = 0, maxConst = 0;
};

// Source-of-truth area matrix, parsed from data/scripts/lib/register_spells.lua.
// Each AREA_<name> declaration is loaded once at engine init; spells reference these
// by name through ResolvedSpell::cardinalMatrix / diagonalMatrix pointers.
//
// Cell values:
//   0 = not in spell area
//   1 = hit tile
//   2 = anchor marker for RING-style spells (e.g. AREA_RING1_BURST3, AREA_BALANCED_BRAWL).
//       Treated as NOT a hit (the caster's tile / hole center).
//   3 = anchor marker for everything else. IS a hit (the spell's center tile, the target
//       tile for needDirection waves).
//
// centerRow/centerCol = location of the anchor marker ({2} or {3}) in the matrix.
// maxForwardExtent / maxSideExtent = farthest hit tile from the anchor, used to derive
//   the spectator scan radius dynamically (replaces the hardcoded scanRadius=5 in
//   countAoeTargets, which silently truncated AREA_WAVE10 and other large patterns).
struct AreaMatrix {
	std::vector<std::vector<uint8_t>> cells;
	int32_t centerRow = 0;
	int32_t centerCol = 0;
	int32_t maxRowExtent = 0;
	int32_t maxColExtent = 0;
	uint8_t anchorValue = 3;  // 2 or 3
};

static CombatType_t parseCombatType(const std::string& str) {
	if (str.find("PHYSICALDAMAGE") != std::string::npos) return COMBAT_PHYSICALDAMAGE;
	if (str.find("FIREDAMAGE") != std::string::npos) return COMBAT_FIREDAMAGE;
	if (str.find("EARTHDAMAGE") != std::string::npos) return COMBAT_EARTHDAMAGE;
	if (str.find("ENERGYDAMAGE") != std::string::npos) return COMBAT_ENERGYDAMAGE;
	if (str.find("ICEDAMAGE") != std::string::npos) return COMBAT_ICEDAMAGE;
	if (str.find("HOLYDAMAGE") != std::string::npos) return COMBAT_HOLYDAMAGE;
	if (str.find("DEATHDAMAGE") != std::string::npos) return COMBAT_DEATHDAMAGE;
	return COMBAT_NONE;
}

static AoeAreaType areaPatternToType(const std::string& pat) {
	if (pat.find("SQUARE1X1") != std::string::npos) return AoeAreaType::MELEE_CIRCLE;
	if (pat.find("RING") != std::string::npos) return AoeAreaType::RING; // AREA_RING<N>_BURST<M> — has empty inner hole
	if (pat.find("SQUAREWAVE") != std::string::npos) return AoeAreaType::SQUAREWAVE5;
	if (pat.find("WAVE") != std::string::npos) return AoeAreaType::WAVE4;
	if (pat.find("BEAM") != std::string::npos) return AoeAreaType::BEAM5;
	return AoeAreaType::CIRCLE;
}

// For AREA_RING<innerName>_BURST<burstName>: returns innerName+1 (Chebyshev exclusion bound).
// Returns 0 for non-RING patterns (no inner hole — default for CIRCLE/WAVE/BEAM/SQUARE).
// The "+1" matches the actual array geometry: AREA_RING1_BURST3 has a 3x3 empty center
// (tiles at Chebyshev<=1), so the smallest hit tile is at Chebyshev==2 == innerName+1.
static int32_t areaPatternToInnerSize(const std::string& pat) {
	std::regex ringRe("RING(\\d+)");
	std::smatch m;
	if (std::regex_search(pat, m, ringRe)) {
		return std::stoi(m[1].str()) + 1;
	}
	return 0;
}

static int32_t areaPatternToSize(const std::string& pat) {
	// AREA_RING<inner>_BURST<burst>: outer Chebyshev bound is burst+1 (matches actual array geometry).
	// For AREA_RING1_BURST3 (9x9 with 3x3 hole): burst=3, outer Chebyshev bound = 4.
	std::smatch m;
	std::regex burstRe("BURST(\\d+)");
	if (std::regex_search(pat, m, burstRe)) {
		return std::stoi(m[1].str()) + 1;
	}
	// Extract trailing digits from patterns like AREA_CIRCLE5X5, AREA_WAVE4, AREA_BEAM7
	std::regex sizeRe("(\\d+)(?:X\\d+)?\\s*[,)]");
	if (std::regex_search(pat, m, sizeRe)) {
		return std::stoi(m[1].str());
	}
	// Fallback: parse the area constant name
	if (pat.find("1X1") != std::string::npos) return 1;
	if (pat.find("2X2") != std::string::npos) return 2;
	if (pat.find("3X3") != std::string::npos) return 3;
	if (pat.find("4X4") != std::string::npos) return 4;
	if (pat.find("5X5") != std::string::npos) return 5;
	if (pat.find("6X6") != std::string::npos) return 6;
	// Waves/beams: AREA_WAVE4 = depth 4, AREA_BEAM7 = depth 7
	std::regex numRe("(\\d+)$");
	std::string name = pat;
	if (std::regex_search(name, m, numRe)) {
		return std::stoi(m[1].str());
	}
	return 3; // default
}

// Parse data/scripts/lib/register_spells.lua for AREA_<name> = { { ... }, { ... }, ... }
// declarations. Returns a map keyed by area name (e.g. "AREA_SHORTWAVE3") → AreaMatrix.
// Robust to:
//   - trailing commas after rows or after the last row
//   - blank lines / comments interleaved
//   - whitespace variation in indentation
//   - identifiers without AREA_ prefix (e.g. CrossBeamArea3X2) — still parsed, just unused
//   - matrices with {2} caster marker instead of {3} (RING-style)
//   - matrices with neither {2} nor {3} → center defaulted to geometric middle (rare)
// Uses a simple line-by-line state machine — no nested brace counting needed because
// the file is well-formatted (each row on its own line).
static std::unordered_map<std::string, AreaMatrix> parseLuaAreaMatrices(const std::string& filePath) {
	std::unordered_map<std::string, AreaMatrix> result;
	std::ifstream file(filePath);
	if (!file.is_open()) {
		g_logger().warn("[BotEngine] Could not open {} — area matrix lookup disabled", filePath);
		return result;
	}

	std::regex declRe("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*\\{\\s*(?:--.*)?$");
	std::regex rowRe("^\\s*\\{([^}]*)\\}");          // captures inside of one row
	std::regex closeRe("^\\s*\\}");                  // closing brace of the matrix

	std::string line;
	std::string currentName;
	std::vector<std::vector<uint8_t>> currentRows;
	bool collecting = false;

	auto finalizeMatrix = [&]() {
		if (currentName.empty() || currentRows.empty()) {
			currentName.clear();
			currentRows.clear();
			collecting = false;
			return;
		}
		AreaMatrix m;
		m.cells = currentRows;
		// Locate anchor: prefer {3} (spell center for most patterns), fall back to {2} (RING caster).
		// If neither is present (rare), default the center to the geometric middle.
		bool found = false;
		for (size_t r = 0; r < m.cells.size() && !found; ++r) {
			for (size_t c = 0; c < m.cells[r].size() && !found; ++c) {
				if (m.cells[r][c] == 3) {
					m.centerRow = (int32_t)r;
					m.centerCol = (int32_t)c;
					m.anchorValue = 3;
					found = true;
				}
			}
		}
		if (!found) {
			for (size_t r = 0; r < m.cells.size() && !found; ++r) {
				for (size_t c = 0; c < m.cells[r].size() && !found; ++c) {
					if (m.cells[r][c] == 2) {
						m.centerRow = (int32_t)r;
						m.centerCol = (int32_t)c;
						m.anchorValue = 2;
						found = true;
					}
				}
			}
		}
		if (!found) {
			m.centerRow = (int32_t)m.cells.size() / 2;
			m.centerCol = m.cells.empty() ? 0 : (int32_t)m.cells[0].size() / 2;
			m.anchorValue = 0;
		}
		// Compute extent — farthest hit tile from anchor in row/col axes.
		// Used by countAoeTargets to derive scanRadius (matrix-driven, not hardcoded).
		for (size_t r = 0; r < m.cells.size(); ++r) {
			for (size_t c = 0; c < m.cells[r].size(); ++c) {
				uint8_t v = m.cells[r][c];
				if (v != 1 && v != 3) continue;
				int32_t dr = std::abs((int32_t)r - m.centerRow);
				int32_t dc = std::abs((int32_t)c - m.centerCol);
				if (dr > m.maxRowExtent) m.maxRowExtent = dr;
				if (dc > m.maxColExtent) m.maxColExtent = dc;
			}
		}
		result[currentName] = std::move(m);
		currentName.clear();
		currentRows.clear();
		collecting = false;
	};

	auto parseRow = [](const std::string& body) -> std::vector<uint8_t> {
		std::vector<uint8_t> row;
		std::string tok;
		for (char ch : body) {
			if (ch == ',' || ch == ' ' || ch == '\t') {
				if (!tok.empty()) {
					try { row.push_back((uint8_t)std::stoi(tok)); } catch (...) {}
					tok.clear();
				}
			} else if (ch >= '0' && ch <= '9') {
				tok += ch;
			}
			// ignore everything else (comments, etc.)
		}
		if (!tok.empty()) {
			try { row.push_back((uint8_t)std::stoi(tok)); } catch (...) {}
		}
		return row;
	};

	while (std::getline(file, line)) {
		std::smatch m;
		if (!collecting) {
			if (std::regex_search(line, m, declRe)) {
				currentName = m[1].str();
				currentRows.clear();
				collecting = true;
			}
			continue;
		}
		// In collecting state. A row line, then eventually a close-brace line.
		if (std::regex_search(line, m, rowRe)) {
			currentRows.push_back(parseRow(m[1].str()));
			continue;
		}
		if (std::regex_search(line, m, closeRe)) {
			finalizeMatrix();
			continue;
		}
		// Comment / blank line — ignore and stay in collecting state.
	}
	// In case file ends without a trailing close-brace (shouldn't happen, defensive).
	if (collecting) finalizeMatrix();

	return result;
}

static std::unordered_map<std::string, ParsedSpellMeta> parseLuaSpellFiles(const std::string& dir) {
	std::unordered_map<std::string, ParsedSpellMeta> result;
	namespace fs = std::filesystem;
	if (!fs::exists(dir)) return result;

	std::regex combatTypeRe("COMBAT_PARAM_TYPE\\s*,\\s*(COMBAT_\\w+)");
	// Capture both the cardinal area (group 1) and optional diagonal area (group 2)
	// from createCombatArea(AREA_X) or createCombatArea(AREA_X, AREADIAGONAL_X).
	std::regex areaRe("createCombatArea\\s*\\(\\s*(AREA_\\w+)(?:\\s*,\\s*([A-Za-z_][A-Za-z0-9_]*))?");
	// Fallback: some spells pass AREA_ constants via wrapper functions (e.g. energy_wave.lua)
	std::regex areaFallbackRe("[,(]\\s*(AREA_\\w+)");
	std::regex wordsRe("spell:words\\s*\\(\\s*\"([^\"]+)\"");
	std::regex callbackRe("CALLBACK_PARAM_(\\w+)");
	// Match maglevel coefficient patterns in onGetFormulaValues:
	// Patterns: (maglevel * X) + Y  or  maglevel * X  (with optional surrounding parens/ops)
	std::regex mlMinRe("local\\s+min\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+).*?\\+\\s*([\\d.]+)");
	std::regex mlMaxRe("local\\s+max\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+).*?\\+\\s*([\\d.]+)");
	// Simpler patterns without constant: maglevel * X (no + constant)
	std::regex mlMinSimpleRe("local\\s+min\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+)");
	std::regex mlMaxSimpleRe("local\\s+max\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+)");

	for (const auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".lua") continue;
		std::ifstream file(entry.path());
		if (!file.is_open()) continue;
		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		ParsedSpellMeta meta;
		std::smatch m;

		// Combat type
		if (!std::regex_search(content, m, combatTypeRe)) continue;
		meta.combatType = parseCombatType(m[1].str());
		if (meta.combatType == COMBAT_NONE) continue;

		// Spell words
		if (!std::regex_search(content, m, wordsRe)) continue;
		meta.words = m[1].str();

		// Area (optional — if present, it's an AoE spell)
		if (std::regex_search(content, m, areaRe)) {
			meta.isAoe = true;
			meta.areaPattern = m[1].str();
			if (m.size() > 2 && m[2].matched) {
				meta.diagonalAreaPattern = m[2].str();
			}
		} else if (std::regex_search(content, m, areaFallbackRe)) {
			// Wrapper function pattern: createCombat(AREA_BEAM8, ...) → createCombatArea(area)
			meta.isAoe = true;
			meta.areaPattern = m[1].str();
		}

		// Formula type
		if (std::regex_search(content, m, callbackRe)) {
			std::string cb = m[1].str();
			if (cb == "SKILLVALUE") {
				meta.usesSkillFormula = true;
			}
		} else {
			continue; // No callback = condition-only spell (curse, envenom, etc.) — skip
		}

		// Damage coefficients (maglevel-based spells only)
		if (!meta.usesSkillFormula) {
			if (std::regex_search(content, m, mlMinRe)) {
				meta.minMlCoef = std::stod(m[1].str());
				meta.minConst = std::stod(m[2].str());
			} else if (std::regex_search(content, m, mlMinSimpleRe)) {
				meta.minMlCoef = std::stod(m[1].str());
				meta.minConst = 0;
			}
			if (std::regex_search(content, m, mlMaxRe)) {
				meta.maxMlCoef = std::stod(m[1].str());
				meta.maxConst = std::stod(m[2].str());
			} else if (std::regex_search(content, m, mlMaxSimpleRe)) {
				meta.maxMlCoef = std::stod(m[1].str());
				meta.maxConst = 0;
			}
		}

		result[meta.words] = meta;
	}
	return result;
}

static std::unordered_map<uint16_t, ParsedRuneMeta> parseLuaRuneFiles(const std::string& dir) {
	std::unordered_map<uint16_t, ParsedRuneMeta> result;
	namespace fs = std::filesystem;
	if (!fs::exists(dir)) return result;

	std::regex combatTypeRe("COMBAT_PARAM_TYPE\\s*,\\s*(COMBAT_\\w+)");
	std::regex areaRe("createCombatArea\\s*\\(\\s*(AREA_\\w+)(?:\\s*,\\s*([A-Za-z_][A-Za-z0-9_]*))?");
	std::regex areaFallbackRe("[,(]\\s*(AREA_\\w+)");
	std::regex runeIdRe("rune:runeId\\s*\\(\\s*(\\d+)");
	std::regex mlMinRe("local\\s+min\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+).*?\\+\\s*([\\d.]+)");
	std::regex mlMaxRe("local\\s+max\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+).*?\\+\\s*([\\d.]+)");
	std::regex mlMinSimpleRe("local\\s+min\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+)");
	std::regex mlMaxSimpleRe("local\\s+max\\s*=.*(?:maglevel|magicLevel|magLevel)\\s*\\*\\s*([\\d.]+)");

	for (const auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".lua") continue;
		std::ifstream file(entry.path());
		if (!file.is_open()) continue;
		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		ParsedRuneMeta meta;
		std::smatch m;

		if (!std::regex_search(content, m, combatTypeRe)) continue;
		meta.combatType = parseCombatType(m[1].str());
		if (meta.combatType == COMBAT_NONE) continue;

		if (!std::regex_search(content, m, runeIdRe)) continue;
		meta.runeId = static_cast<uint16_t>(std::stoi(m[1].str()));

		if (std::regex_search(content, m, areaRe)) {
			meta.isAoe = true;
			meta.areaPattern = m[1].str();
			if (m.size() > 2 && m[2].matched) {
				meta.diagonalAreaPattern = m[2].str();
			}
		} else if (std::regex_search(content, m, areaFallbackRe)) {
			meta.isAoe = true;
			meta.areaPattern = m[1].str();
		}

		if (std::regex_search(content, m, mlMinRe)) {
			meta.minMlCoef = std::stod(m[1].str());
			meta.minConst = std::stod(m[2].str());
		} else if (std::regex_search(content, m, mlMinSimpleRe)) {
			meta.minMlCoef = std::stod(m[1].str());
			meta.minConst = 0;
		}
		if (std::regex_search(content, m, mlMaxRe)) {
			meta.maxMlCoef = std::stod(m[1].str());
			meta.maxConst = std::stod(m[2].str());
		} else if (std::regex_search(content, m, mlMaxSimpleRe)) {
			meta.maxMlCoef = std::stod(m[1].str());
			meta.maxConst = 0;
		}

		result[meta.runeId] = meta;
	}
	return result;
}

// Resolved spell: dynamically built from server registry + Lua parsing
struct ResolvedSpell {
	std::string words;
	CombatType_t combatType = COMBAT_NONE;
	int32_t range = 0;           // from server Spell API
	uint32_t level = 0;          // from server Spell API
	uint32_t magicLevel = 0;     // from server Spell API
	bool needDirection = false;  // from server Spell API
	bool isAoe = false;
	AoeAreaType aoeAreaType = AoeAreaType::CIRCLE;
	int32_t aoeAreaSize = 3;     // for RING: outer Chebyshev bound (burstName+1); for CIRCLE: radius; for WAVE/BEAM: depth
	int32_t aoeInnerSize = 0;    // for RING: inner Chebyshev exclusion (innerName+1); 0 = no inner hole (default for non-RING)
	int32_t minTargets = 2;
	bool usesSkillFormula = false;
	double avgMlCoef = 0;        // (minCoef + maxCoef) / 2
	double avgConst = 0;         // (minConst + maxConst) / 2
	uint16_t spellId = 0;        // for cooldown checks
	SpellGroup_t group = SPELLGROUP_NONE;
	SpellGroup_t secondaryGroup = SPELLGROUP_NONE;
	// Source-of-truth area matrices from register_spells.lua. When non-null, the
	// engine uses matrix lookup (isInAreaMatrix) for hit prediction instead of
	// the AoeAreaType case dispatch — exact match to the server's actual area.
	const AreaMatrix* cardinalMatrix = nullptr;
	const AreaMatrix* diagonalMatrix = nullptr;
	std::string areaPatternName;  // diagnostic — what AREA_<name> this spell uses
};

// Resolved rune: dynamically built from Lua parsing + server rune registry
struct ResolvedRune {
	uint16_t runeId = 0;
	CombatType_t combatType = COMBAT_NONE;
	bool isAoe = false;
	double avgMlCoef = 0;
	double avgConst = 0;
	const AreaMatrix* cardinalMatrix = nullptr;
	const AreaMatrix* diagonalMatrix = nullptr;
};

// Maps POI type to city route destination name
static const char* poiTypeToRouteName(POIType type) {
	switch (type) {
		case POIType::DEPOT:
		case POIType::DEPOT_OUTSIDE: return "depot";
		case POIType::TEMPLE: return "temple";
		case POIType::BOAT: return "boat";
		case POIType::SHOP: return "shop";
		case POIType::NPC: return "npc";
		// WATER deliberately falls through to "": a shoreline has no city route, so the walk
		// is the scoped planner's job. The caller MUST clear pendingNavDest itself when this
		// returns "" — it only overwrites on a non-empty name, so a stale destination from the
		// previous walk would otherwise be inherited.
		default: return "";
	}
}

// Real Tibia healing formula: random value between min and max based on level + magic level
// Each spell has coefficients from the server's actual spell scripts
struct RealHealSpell {
	int32_t level;       // minimum level required
	std::string name;    // spell words
	int32_t cd;          // cooldown in seconds
	// Formula: min = level * lvlCoefMin + magicLevel * mlCoefMin + baseMin
	//          max = level * lvlCoefMax + magicLevel * mlCoefMax + baseMax
	double lvlCoefMin, mlCoefMin, baseMin;
	double lvlCoefMax, mlCoefMax, baseMax;

	int32_t calcHeal(int32_t playerLevel, int32_t magicLevel) const {
		int32_t minHeal = static_cast<int32_t>(playerLevel * lvlCoefMin + magicLevel * mlCoefMin + baseMin);
		int32_t maxHeal = static_cast<int32_t>(playerLevel * lvlCoefMax + magicLevel * mlCoefMax + baseMax);
		if (maxHeal <= minHeal) return minHeal;
		return minHeal + (std::rand() % (maxHeal - minHeal + 1));
	}
};

// ============================================================================
// Time-of-day population schedule — breakpoints and interpolation
// ============================================================================

// Master switch for the time-of-day percentage curve. When false (default),
// getSchedulePercent always returns 100, so the population scheduler treats
// TARGET_ONLINE as the desired active count regardless of wall-clock hour.
// Flip to true to re-enable the SCHEDULE_POINTS interpolation. The on/off
// scheduler mechanism (debug-mode "schedule off") is independent of this flag.
static constexpr bool TIME_OF_DAY_SCHEDULE_ENABLED = false;

struct SchedulePoint { int hour; int percent; };
static constexpr SchedulePoint SCHEDULE_POINTS[] = {
	{ 0, 60 },   // 0-5: night
	{ 6, 30 },   // 6-8: early morning
	{ 9, 40 },   // 9-11: morning
	{12, 60 },   // 12-15: afternoon
	{16, 75 },   // 16-19: evening
	{20, 100 },  // 20-23: prime time
};
static constexpr int NUM_SCHEDULE_POINTS = 6;
static constexpr int RAMP_MINUTES = 30;

// Returns target percent for a given hour:minute, with 30-min ramps between brackets
static int getSchedulePercent(int hour, int minute) {
	if constexpr (!TIME_OF_DAY_SCHEDULE_ENABLED) {
		return 100; // always-100% bypass: scheduler activates all bots up to TARGET_ONLINE
	}
	int minuteOfDay = hour * 60 + minute;

	// Find current bracket (largest start time <= minuteOfDay)
	int currentIdx = NUM_SCHEDULE_POINTS - 1;
	for (int i = NUM_SCHEDULE_POINTS - 1; i >= 0; i--) {
		if (minuteOfDay >= SCHEDULE_POINTS[i].hour * 60) {
			currentIdx = i;
			break;
		}
	}

	// Next bracket (wraps around to 0)
	int nextIdx = (currentIdx + 1) % NUM_SCHEDULE_POINTS;
	int nextStartMinute = SCHEDULE_POINTS[nextIdx].hour * 60;
	if (nextIdx <= currentIdx) nextStartMinute += 24 * 60; // wrap past midnight

	int minutesUntilNext = nextStartMinute - minuteOfDay;

	int currentPercent = SCHEDULE_POINTS[currentIdx].percent;
	int nextPercent = SCHEDULE_POINTS[nextIdx].percent;

	// Within 30-min ramp zone? Linearly interpolate.
	if (minutesUntilNext <= RAMP_MINUTES && minutesUntilNext > 0) {
		float progress = 1.0f - static_cast<float>(minutesUntilNext) / RAMP_MINUTES;
		return currentPercent + static_cast<int>((nextPercent - currentPercent) * progress);
	}

	return currentPercent;
}

// Equipment loadout data (loaded from data/bot/authored/equipment.csv)
struct BotEquipment {
	uint16_t head = 0;
	uint16_t armor = 0;
	uint16_t legs = 0;
	uint16_t feet = 0;
	uint16_t weapon = 0; // DB slot_right → CONST_SLOT_LEFT (weapon hand)
	uint16_t shield = 0; // DB slot_left → CONST_SLOT_RIGHT (shield hand)
	uint16_t backpack = 0; // DB slot_backpack → CONST_SLOT_BACKPACK
};

// BotEngine — concrete implementation of IBotEngine, compiled into libbot_engine.so
// ---- BOT_NAV_REALISM Phase 5: named cache instrumentation (types) ----
// Per-name hit/miss/cost stats for the engine's TTL-cached queries — the *visibility* half of the
// playerbots AI_VALUE pattern. The counters themselves are BotEngine members (see botCacheHit /
// botCacheMiss below): a file-scope static here would give every translation unit its own copy and
// silently fork the stats once the engine spans several TUs.
enum class BotCacheId : uint8_t { Spectators = 0, WalkObserved = 1, COUNT = 2 };
static constexpr const char* kBotCacheNames[] = { "spectators", "walk-observed" };
struct BotCacheStat {
	uint64_t hits = 0;    // served from cache (TTL still valid)
	uint64_t misses = 0;  // recomputed
	uint64_t totalUs = 0; // cumulative recompute cost
	uint64_t worstUs = 0; // slowest single recompute
};
static inline uint64_t botMonoUs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}


// ============================================================================
// PERF STRESS HARNESS: continuous tick telemetry
// ============================================================================
//
// The engine already logs OUTLIERS ([TICK_SLOW] >150ms, [GAP_SLOW] >500ms) and already
// attributes them well ([PROCBOT_SLOW] names the bot, [ZPLAN_SLOW] the z-planner, and the
// dispatcher's own [CYCLE_SLOW] names the task plus dbsync=/await=). What is missing is the
// DISTRIBUTION -- you cannot regression-gate on a count of warnings, because that count is
// bimodal and dominated by LXC scheduler noise.
//
// Two instruments close that gap:
//
//  1. Fixed-bucket histograms. O(1), allocation-free, one array increment per tick.
//     Percentiles are interpolated from bucket edges, so the tail is coarse -- which is fine:
//     the question is "did p95 move from 40ms to 90ms", not "is it 41 or 44".
//
//  2. A CPU-time clock alongside the wall clock for the tick body. This is the important one.
//     A 3.6s tick body at 9% process CPU on a 2-core host with loadavg ~6.85 is equally
//     consistent with (a) the tick genuinely computing, (b) the tick BLOCKED on a synchronous
//     call, and (c) the tick merely PREEMPTED by co-tenant load. Wall clock alone cannot tell
//     them apart, and guessing wrong sends the investigation the wrong way -- "buy more cores"
//     versus "fix an engine bug". With both clocks:
//         body_wall ~= body_cpu                -> computing; the phase register names it
//         body_wall >> body_cpu, dbsync > 0    -> blocked on sync DB (this IS reachable from
//                                                 tick(): doPopulationManagement -> activateBot
//                                                 -> db.storeQuery)
//         body_wall >> body_cpu, dbsync == 0   -> preempted by the host
//
// CLOCK_THREAD_CPUTIME_ID is the correct clock, not CLOCK_PROCESS_CPUTIME_ID: tick() always runs
// on the single serial dispatcher thread (Game::botStartTickLoop registers it as a cycleEvent,
// TaskGroup::Serial) and the engine never fans work out to the thread pool. The process clock
// would fold in the DB task pool, network threads and async workers, contaminating body_cpu with
// unrelated work and destroying exactly the discrimination above.
static inline uint64_t botThreadCpuUs() {
	timespec ts {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL
		+ static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

// ---- Tick deadline for tier 3-5 work (operator priority order) ----
//
// Priority, highest first: server internals > player-related server internals > awake bots WITHIN
// visible distance > awake bots outside it > hibernated bots. Tiers 3-5 may be pushed to the next
// tick; tiers 1-2 may not. The server must never freeze and players must never feel one.
//
// The z-planner is tier 3-4 work that could run half a second inside a 100ms tick, blocking tiers
// 1-2 for its whole duration. The count budget (BOT_AWAKE_PATHFIND_PER_TICK_*) decides WHOSE TURN
// comes first; it cannot bound how long any single bot runs. This does.
//
// Deliberately NOT exempting observed bots. The count budget exempts them, correctly -- being
// visible earns a place at the FRONT of the bot queue. It must not earn the right to HOLD the
// tick: an observed bot running its plan to completion freezes the very player watching it, and
// everyone else, because tiers 1-2 outrank every bot. Deferring costs that bot one tick (~100ms),
// which nobody perceives. Observed bots go first, never longer.
inline int64_t s_tickBodyStartMs = 0;        // stamped at the top of BotEngine::tick
inline uint64_t s_zPlanDeferrals = 0;        // cumulative, for [ZPLAN_DEFER]
inline int64_t s_zPlanDeferLogMs = 0;
// Consecutive deferrals per bot. If this climbs, work is being STARVED rather than merely paced --
// that is the "bots look frozen" failure mode, and it is meant to be visible as a number here
// rather than something noticed in game.
// Value is {consecutive deferrals, last-deferred ms}. The timestamp is what makes the reported
// count honest: an entry is only cleared when that bot's plan PASSES the gate, so a bot that
// defers and then hibernates (or simply stops planning) keeps its entry until it wakes and plans
// again. Measured live: 95 entries against 61 awake bots -- bounded, but it reads as "95 bots are
// stuck right now", which is exactly the wrong impression for the one signal that is supposed to
// distinguish pacing from starvation. Stale entries are pruned on report instead of hooking every
// lifecycle exit, which would be more places to forget.
struct ZDeferStreak {
	uint32_t count = 0;
	int64_t lastMs = 0;
};
inline std::unordered_map<uint32_t, ZDeferStreak> s_zPlanDeferStreak;
inline uint32_t s_zPlanDeferWorstStreak = 0;
inline uint32_t s_zPlanDeferWorstGuid = 0;
// Only a bot that deferred within this window counts as "currently deferring".
inline constexpr int64_t Z_DEFER_STALE_MS = 5000;

// Don't START a plan once the tick has already spent this long. Chosen against measured cost: a
// plan now runs well under 100ms typically, with a ~500ms tail, so a tick that has already used
// 30ms cannot afford one. Tune from [ZPLAN_DEFER] and [TICK_SLOW] together -- if deferrals climb
// while tick bodies stay small, the budget is too tight.
inline constexpr int64_t Z_PLAN_TICK_BUDGET_MS = 30;

// How long ONE search may run once started. The entry gate above cannot bound this: a plan that
// begins just under the tick budget still runs to completion, and one measured live at 3284ms did
// exactly that. 15ms sits above the Z_PLAN_SLOW_MS warn line, so a healthy plan never trips it and
// only the pathological tail is cut.
inline constexpr int64_t Z_PLAN_SEARCH_BUDGET_MS = 15;

// Bucket upper bounds in ms (roughly Fibonacci, then coarse past 1s). A sample lands in the
// first bucket whose bound it does not exceed; anything above the last bound lands in the
// overflow bucket, so the array is one longer than this list.
inline constexpr int64_t kPerfBucketBoundsMs[] = {
	0, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 1000, 1600, 2600, 4200
};
inline constexpr size_t kPerfBucketCount = std::size(kPerfBucketBoundsMs) + 1;

struct BotPerfHist {
	uint64_t buckets[kPerfBucketCount] = {};
	uint64_t count = 0;
	uint64_t sumUs = 0;   // microsecond resolution so sub-ms phases still sum meaningfully
	int64_t worstUs = 0;

	void addUs(int64_t us) {
		const int64_t ms = us / 1000;
		size_t i = 0;
		while (i < std::size(kPerfBucketBoundsMs) && ms > kPerfBucketBoundsMs[i]) {
			i++;
		}
		buckets[i]++;
		count++;
		sumUs += static_cast<uint64_t>(us < 0 ? 0 : us);
		if (us > worstUs) {
			worstUs = us;
		}
	}

	// Interpolated percentile in ms. Returns the UPPER bound of the bucket the requested rank
	// falls in -- deliberately pessimistic, and stable across runs, which is what a regression
	// gate needs. The overflow bucket reports the observed worst instead of "infinity".
	[[nodiscard]] int64_t pctileMs(double p) const {
		if (count == 0) {
			return 0;
		}
		const uint64_t target = static_cast<uint64_t>(static_cast<double>(count) * p);
		uint64_t seen = 0;
		for (size_t i = 0; i < kPerfBucketCount; i++) {
			seen += buckets[i];
			if (seen >= target) {
				return (i < std::size(kPerfBucketBoundsMs))
					? kPerfBucketBoundsMs[i]
					: worstUs / 1000;
			}
		}
		return worstUs / 1000;
	}

	[[nodiscard]] double meanMs() const {
		return count ? (static_cast<double>(sumUs) / static_cast<double>(count) / 1000.0) : 0.0;
	}
};

// The nine tick-top phases that carried no timing at all, plus the per-bot loop and the two that
// already had ad-hoc timers (kept here so one dump shows the whole tick). Order matches the call
// order in BotEngine::tick so the dump reads top to bottom.
enum class BotTickPhase : uint8_t {
	LivenessCfg = 0, StaleParties, LeaderTrails, PartyInvites, PartyAssembly, AmbientRoam,
	PendingReplies, PopulationMgmt, AnchorsRefresh, VirtualTick, BotLoop, FlushNavEvents,
	COUNT
};
inline constexpr const char* kBotTickPhaseNames[] = {
	"liveness_cfg", "stale_parties", "leader_trails", "party_invites", "party_assembly",
	"ambient_roam", "pending_replies", "population_mgmt", "anchors_refresh", "virtual_tick",
	"bot_loop", "flush_nav_events"
};
static_assert(std::size(kBotTickPhaseNames) == static_cast<size_t>(BotTickPhase::COUNT),
	"phase name table must track BotTickPhase");

// Worst individual ticks in the window, kept so a dump can name the phase that dominated the
// spike rather than only reporting that a spike happened. Tiny fixed ring; no allocation.
struct BotPerfWorstTick {
	int64_t bodyWallMs = 0;
	int64_t bodyCpuMs = 0;
	int64_t phaseMs = 0;
	uint8_t phase = 0;
	uint32_t awake = 0;
};
inline constexpr size_t kPerfWorstTicks = 8;

struct BotPerfStats {
	// Window bookkeeping
	int64_t windowStartMs = 0;
	uint64_t ticks = 0;

	// Dual-clock body, plus the wall-clock companions.
	BotPerfHist bodyWall, bodyCpu, gapWall, virtualTickWall, flushNavWall;

	// Per-phase WALL only. Per-phase CPU was considered and cut: a blocking phase already shows
	// as a wall spike in that phase against a flat body_cpu, so the extra clock_gettime calls
	// per tick would buy no additional discriminating power.
	BotPerfHist phase[static_cast<size_t>(BotTickPhase::COUNT)];

	BotPerfWorstTick worst[kPerfWorstTicks] = {};

	// Counters. Deliberately few: hibernate/wake totals and the state mix are already emitted
	// every 300ms by the Lua hibernation monitor ("500 bots: hibernated=477 awake=23, 1 real
	// players"), which collect.py scrapes -- duplicating them in C++ would add fields to keep in
	// sync for no new information. Only wakesTeleport is here, because the burst is engine-side
	// and has no Lua counterpart.
	uint64_t wakesTeleport = 0;
	uint64_t budgetServed = 0, budgetDeferred = 0, budgetObservedExempt = 0;
	uint64_t tickSlowCrossings = 0, gapSlowCrossings = 0;

	// Gauges, sampled ~1Hz
	int64_t lastGaugeMs = 0;
	uint32_t gaugeAwake = 0, gaugeHibernated = 0, gaugeAnchors = 0, gaugeProbes = 0;
	uint32_t gaugeRealPlayers = 0;

	void reset() {
		*this = BotPerfStats {};
	}

	void noteWorst(int64_t bodyWallMs, int64_t bodyCpuMs, uint8_t ph, int64_t phMs, uint32_t awake) {
		size_t minIdx = 0;
		for (size_t i = 1; i < kPerfWorstTicks; i++) {
			if (worst[i].bodyWallMs < worst[minIdx].bodyWallMs) {
				minIdx = i;
			}
		}
		if (bodyWallMs > worst[minIdx].bodyWallMs) {
			worst[minIdx] = BotPerfWorstTick { bodyWallMs, bodyCpuMs, phMs, ph, awake };
		}
	}
};

// RAII wall timer for one tick phase. Reads the steady clock twice; at a 100ms cadence the
// extra reads per tick are tens of microseconds per second.
struct BotPhaseTimer {
	BotPerfHist &h;
	int64_t &outUs;
	uint64_t start;
	BotPhaseTimer(BotPerfHist &hist, int64_t &out) :
		h(hist), outUs(out), start(botMonoUs()) { }
	~BotPhaseTimer() {
		outUs = static_cast<int64_t>(botMonoUs() - start);
		h.addUs(outUs);
	}
};

// Whole-bot count from a percentage config key (density caps, concurrency caps). Stateless, so a
// per-TU copy is harmless.
static inline uint32_t pctOfBotTotal(const ConfigKey_t pctKey) {
	const double pct = g_configManager().getFloat(pctKey);
	const double total = static_cast<double>(g_configManager().getNumber(BOT_PLAYERS_ONLINE));
	const double bots = pct * total / 100.0 + 1e-3;
	return bots <= 0.0 ? 0 : static_cast<uint32_t>(bots);
}

// ============================================================================
// Engine-local constants and pure helpers (BOT_NAV_REALISM Phase 11).
//
// These were anonymous-namespace blocks inside bot_engine.cpp. They are shared
// by several engine modules, and anonymous-namespace symbols have internal
// linkage — which is exactly what made the first bot_liveness carve-out fail to
// compile. Hoisted here so any engine translation unit can use them.
//
// Everything below is STATELESS (constants + pure functions), so the per-TU copy
// an anonymous namespace in a header produces is harmless. The two mutable
// debug-override vars that used to live here (s_forceAdvStoneNextMode /
// ...NextWeapon) are BotEngine members instead — duplicating those would have
// silently forked state between modules.
// ============================================================================

namespace {
	struct AdventurerStoneTempleRange {
		Position fromPos;
		Position toPos;
		uint32_t townId;
	};
	// Mirrors config.Temples in adventurers_stone.lua (data-otservbr-global), minus
	// Ankrahmun/Darashia: those temples' PZ rectangle z does not match CITY_WALK_Z, so
	// bots arriving at the regular Temple POI position would never satisfy the range
	// check. (Ankrahmun PZ z=8 vs walkZ=7; Darashia PZ z=1 tower vs walkZ=7.)
	// Update this list if the upstream action.xml file changes.
	constexpr AdventurerStoneTempleRange kAdventurerStoneTempleRanges[] = {
		{ Position(32727, 31632, 7),  Position(32736, 31639, 7),  5  },  // Ab'Dendriel
		{ Position(32358, 31777, 7),  Position(32364, 31787, 7),  6  },  // Carlin
		{ Position(32642, 31921, 11), Position(32656, 31929, 11), 7  },  // Kazordoon
		{ Position(32365, 32231, 7),  Position(32374, 32243, 7),  8  },  // Thais
		{ Position(32953, 32072, 7),  Position(32963, 32081, 7),  9  },  // Venore
		{ Position(33208, 31803, 8),  Position(33225, 31819, 8),  11 },  // Edron
		{ Position(33018, 31514, 11), Position(33032, 31531, 11), 12 },  // Farmine
		{ Position(32313, 32818, 7),  Position(32322, 32830, 7),  14 },  // Liberty Bay
		{ Position(32590, 32740, 7),  Position(32600, 32750, 7),  15 },  // Port Hope
		{ Position(32209, 31130, 7),  Position(32215, 31136, 7),  16 },  // Svargrond
		{ Position(32785, 31275, 7),  Position(32789, 31279, 7),  17 },  // Yalahar
		{ Position(33444, 31313, 9),  Position(33452, 31324, 9),  18 },  // Gray Beach
		{ Position(33586, 31895, 6),  Position(33602, 31902, 6),  20 },  // Rathleton
		{ Position(33510, 32360, 6),  Position(33516, 32365, 6),  21 },  // Roshamuul
		{ Position(33916, 31474, 5),  Position(33926, 31480, 5),  22 },  // Issavi
	};
	// Catches DB/code drift: if rows are added or removed without updating, build fails.
	static_assert(std::size(kAdventurerStoneTempleRanges) == 15,
		"kAdventurerStoneTempleRanges should contain 15 entries (17 from action.xml minus Ankrahmun/Darashia)");

	constexpr Position kAdventurerStoneDest = Position(32210, 32300, 6);
	constexpr Position kAdventurerStoneForcefield = Position(32210, 32292, 6);
	// BOT_LIVENESS_PACK Phase A.4: AdvStone island bbox check used by POI crowd-cap
	// + Phase-B density-cap exemptions lives at file scope ~line 2176 (function
	// `isOnAdvStoneIsland`). Don't duplicate it here.
	// Verified from data-otservbr-global/lib/core/storages.lua:1806
	// Storage.Quest.U9_80.AdventurersGuild.Stone = 52130
	constexpr int32_t STORAGE_ADVENTURERS_GUILD_STONE = 52130;

	// ---- Elemental shrine hubs (BOT_TELEPORT_TILE_SAFETY) ----
	// Exact mirror of the AdventurersGuild storage bridge above, for the four elemental
	// shrines. data-otservbr-global/scripts/movements/teleport/shrine_entrance.lua teleports a
	// player from one of 52 entrance tiles (13 towns x 4 elements) to that element's hub AND
	// sets Storage.ShrineEntrance to the town's INDEX; shrine_exit.lua then reads that index to
	// pick which town the hub's mystic flames send the player back to.
	//
	// Bots reach a hub two ways that both SKIP the entrance tile — a script `synth_bridge`
	// TELEPORT waypoint, and the Feyrist exit portal (feyrist_exit.lua aid 24999-25002, whose
	// four destinations ARE these hubs). With the storage unset, shrine_exit.lua falls through
	// to `player:getTown():getTemplePosition()`, which silently does nothing for a bot player —
	// verified live 2026-08-02: the bot stands on the flame indefinitely. This is the exact
	// failure the adv-stone bridge at bot_engine.cpp:812-824 already documents for aid:4253.
	//
	// Verified from data-otservbr-global/lib/core/storages.lua:116
	constexpr int32_t STORAGE_SHRINE_ENTRANCE = 30060;

	// shrine_entrance.lua destinations (ice / earth / fire / energy).
	constexpr Position kShrineHubs[] = {
		Position(32192, 31419, 2),   // ice
		Position(32972, 32227, 7),   // earth
		Position(32911, 32336, 15),  // fire
		Position(33059, 32716, 5),   // energy
	};

	// Canary town id for each index of shrine_exit.lua's `exitDestination` list, IN ORDER.
	// The storage value is an INDEX into that hardcoded 13-entry table, NOT a town id (this is
	// the one asymmetry with the adv-stone bridge, whose storage IS a town id). An out-of-range
	// value makes shrine_exit.lua evaluate Position(nil) and throw, so every write must be
	// clamped to 1..13 — see botShrineIndexForTown.
	constexpr uint32_t kShrineTownOrder[] = {
		6,  // 1  Carlin
		8,  // 2  Thais
		9,  // 3  Venore
		5,  // 4  Ab'Dendriel
		7,  // 5  Kazordoon
		13, // 6  Darashia
		10, // 7  Ankrahmun
		11, // 8  Edron
		14, // 9  Liberty Bay
		15, // 10 Port Hope
		16, // 11 Svargrond
		17, // 12 Yalahar
		20, // 13 Oramond
	};
	static_assert(std::size(kShrineTownOrder) == 13,
		"kShrineTownOrder must match shrine_exit.lua's 13-entry exitDestination list");

	// Dwell sub-activity landmarks on the Adv Stone island (z=7 dungeon section).
	// Both tiles are themselves non-walkable — bots stand on adjacent walkable tiles.
	constexpr Position kAdvStoneRewardChest = Position(32192, 32292, 7);
	constexpr Position kAdvStoneExerciseDummy = Position(32196, 32296, 7);

	// Lasting Exercise weapon item IDs (max-charge variant: 14400 charges each).
	// Bots get all 8 added to backpack on activation; one is picked at random per Mode 2 trip.
	constexpr uint16_t kLastingExerciseIds[] = {
		35285, // sword (melee)
		35286, // axe (melee)
		35287, // club (melee)
		44067, // shield (melee)
		50295, // wraps/fist (melee)
		35288, // bow (ranged, allowFarUse)
		35289, // rod (ranged, allowFarUse)
		35290, // wand (ranged, allowFarUse)
	};
	constexpr uint16_t kLastingExerciseCharges = 14400;

	inline bool isMeleeExerciseWeapon(uint16_t id) {
		return id == 35285 || id == 35286 || id == 35287 || id == 44067 || id == 50295;
	}

	// Minimum Chebyshev distance from the dummy when a ranged weapon is picked for
	// mode-2 training. Without this bias, trySetDummy's collectFreeWithLOS scan
	// returns ALL valid LOS tiles in the 11x11 radius-5 grid and selectAdvStoneSubActivity
	// picks uniformly — adjacent (distance 1) tiles are equally likely as far ones.
	// allowFarUse=true means the weapon mechanically works at any distance, but a bot
	// holding a bow standing next to a dummy looks cosmetically wrong. Setting min=3
	// excludes the 5x5 block centred on the dummy, leaving ~16 candidates for typical
	// dungeon geometry. Fall-back-to-all-candidates path in trySetDummy preserves trip
	// completion if a cramped layout has no distance-3+ free tiles.
	constexpr int32_t kRangedDummyMinDist = 3;

	// Fast-test mode: clamps ALL Adv Stone dwells to 30s so a debug,1 run cycles through
	// all 3 modes quickly. Toggle to true for fast iteration; production uses false:
	//   mode 0/1 -> 60-300s, mode 2 (dummy training) -> 180-1800s (3-30 min).
	constexpr bool kAdvStoneFastTestMode = false;

	// Force-mode override for the NEXT Adv Stone trip (one-shot). Set via the
	// `advstone <chest|dummy|waypoint>` bot command to deterministically test each
	// sub-activity instead of waiting for a random roll. Reset to 0 after consumption.
	// 0 = random (production behavior), 1 = chest, 2 = dummy, 3 = waypoint
	uint8_t s_forceAdvStoneNextMode = 0;
	// Force-weapon override for the NEXT dummy trip (one-shot). Set via
	// `advstone dummy <itemId>` to deterministically pick e.g. a ranged weapon.
	// 0 = random pick from kLastingExerciseIds.
	uint16_t s_forceAdvStoneNextWeapon = 0;

	inline int32_t advStoneDwellSecs(uint8_t mode) {
		if (kAdvStoneFastTestMode) return 30;
		// Config-driven per BOT_LIVENESS_PACK Phase A.5. mode==1 (chest) was previously
		// sharing mode==0's [60,300] range — extended to give visible reward-chest dwell.
		if (mode == 2) {
			return uniform_random(g_configManager().getNumber(BOT_ADV_STONE_DWELL_DUMMY_MIN_SEC),
			                      g_configManager().getNumber(BOT_ADV_STONE_DWELL_DUMMY_MAX_SEC));
		}
		if (mode == 1) {
			return uniform_random(g_configManager().getNumber(BOT_ADV_STONE_DWELL_CHEST_MIN_SEC),
			                      g_configManager().getNumber(BOT_ADV_STONE_DWELL_CHEST_MAX_SEC));
		}
		return uniform_random(g_configManager().getNumber(BOT_ADV_STONE_DWELL_IDLE_MIN_SEC),
		                      g_configManager().getNumber(BOT_ADV_STONE_DWELL_IDLE_MAX_SEC));
	}

	uint32_t findAdventurerStoneTownAt(const Position& pos) {
		for (const auto& r : kAdventurerStoneTempleRanges) {
			if (pos.x >= r.fromPos.x && pos.x <= r.toPos.x
					&& pos.y >= r.fromPos.y && pos.y <= r.toPos.y
					&& pos.z == r.fromPos.z) {
				return r.townId;
			}
		}
		return 0;
	}
} // anonymous namespace

namespace {
	// Phase 6: index-based progression. Every VIRTUAL_MS_PER_WAYPOINT, advance
	// bot's waypoint index by 1 and snap currentPos to that waypoint. No distance
	// math, no path simulation — bots virtually progress at a uniform 1 wp / 5s.
	// Wake places the bot at exactly that waypoint (always reachable since the
	// script author put it there).
	constexpr int64_t VIRTUAL_MS_PER_WAYPOINT = 5000;
	constexpr int64_t VIRTUAL_POS_SAVE_THROTTLE_MS = 300000; // 5 min between persistence UPDATEs
	constexpr int32_t VIRTUAL_POS_SAVE_MIN_DRIFT = 5;        // tiles; skip UPDATE if drift < this
}

namespace {
	bool isFreeWalkableTile(const Position& p) {
		auto tile = g_game().map.getTile(p);
		if (!tile) return false;
		if (tile->hasFlag(TILESTATE_BLOCKSOLID)) return false;
		if (auto creatures = tile->getCreatures(); creatures && !creatures->empty()) return false;
		return true;
	}

	std::vector<Position> collectAdjacentFree(const Position& center) {
		std::vector<Position> out;
		static const std::pair<int, int> offsets[] = {
			{-1, -1}, {0, -1}, {1, -1},
			{-1,  0},          {1,  0},
			{-1,  1}, {0,  1}, {1,  1},
		};
		for (const auto& [dx, dy] : offsets) {
			Position p(static_cast<uint16_t>(static_cast<int32_t>(center.x) + dx),
			           static_cast<uint16_t>(static_cast<int32_t>(center.y) + dy),
			           center.z);
			if (isFreeWalkableTile(p)) out.push_back(p);
		}
		return out;
	}

	// Radius-N scan around center on same z. Pruned by isSightClear so ranged shots can land.
	std::vector<Position> collectFreeWithLOS(const Position& center, int radius) {
		std::vector<Position> out;
		for (int dy = -radius; dy <= radius; ++dy) {
			for (int dx = -radius; dx <= radius; ++dx) {
				if (dx == 0 && dy == 0) continue;
				Position p(static_cast<uint16_t>(static_cast<int32_t>(center.x) + dx),
				           static_cast<uint16_t>(static_cast<int32_t>(center.y) + dy),
				           center.z);
				if (!isFreeWalkableTile(p)) continue;
				if (!g_game().map.isSightClear(p, center, true)) continue;
				out.push_back(p);
			}
		}
		return out;
	}
}

namespace {

constexpr const char* DBG_TAG = "[BOT:DBG]";
constexpr const char* EVT_TAG = "[BOT:EVT]";

// dirShort() and aoeAreaTypeName() are defined as static functions at the top
// of this file (forward-needed by castSpell and other mid-file call sites).

// Pick a one-char grid symbol for a creature. Lowercase = monster (first letter of name),
// 'N' = NPC, 'P' = player. Returns 0 for "no creature on tile".
char gridSymbolForCreature(const std::shared_ptr<Creature>& c) {
	if (!c) return 0;
	if (c->getPlayer()) return 'P';
	if (c->getNpc()) return 'N';
	if (c->getMonster()) {
		const auto& name = c->getName();
		if (!name.empty()) {
			char ch = name[0];
			if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32); // tolower
			return ch;
		}
		return 'm';
	}
	return 0;
}

// Tile symbol resolution: returns one char for the grid cell at (x,y,z) relative to bot.
// '?' = unloaded, '@' = bot, creature symbols, then '*' = floor change, 'F' = damage field,
// '#' = blocked, '.' = walkable. The caller plots '@' for the bot's own tile.
char gridSymbolForTile(const Position& pos, Tile* tile) {
	if (!tile) return '?';
	// Creatures first (most informative)
	if (auto creatures = tile->getCreatures(); creatures && !creatures->empty()) {
		// Priority: player > npc > monster
		std::shared_ptr<Creature> pick;
		for (const auto& c : *creatures) {
			if (!c) continue;
			if (c->getPlayer()) { pick = c; break; }
			if (c->getNpc()) { pick = c; }
			else if (!pick && c->getMonster()) { pick = c; }
		}
		if (auto sym = gridSymbolForCreature(pick); sym != 0) return sym;
	}
	if (tile->hasFlag(TILESTATE_FLOORCHANGE) || tile->hasFlag(TILESTATE_TELEPORT)) return '*';
	if (tile->hasFlag(TILESTATE_MAGICFIELD)) return 'F';
	if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) return '#';
	return '.';
}

} // namespace

// BOT_NAV_REALISM Phase 8 increment 3: every bot teleport is a moment the bot does something a
// real player cannot, so each is counted by its enclosing function (__func__ — a manual tag could
// drift out of date). The counter itself is a BotEngine member (teleportSites_), NOT a file-scope
// static: as a static in this shared header each engine TU would tally into its own map and the
// 5-min summary would silently under-report every module except the one that prints it. The macro
// resolves botCountTeleport() against `this`, so it is usable only inside BotEngine members —
// which is exactly where every teleport call site lives.
#define BOT_TELEPORT(...) (botCountTeleport(__func__), g_game().internalTeleport(__VA_ARGS__))

// ---- Hunt/nav helpers shared across engine modules (Phase 11) ----
// Stateless: constants and pure functions, so the per-TU copy is harmless.
static constexpr uint16_t MACHETE_ITEM_ID = 3308;

// ---- Floor-change mechanism item ids (TRUE MULTI-FLOOR: hoisted from
// bot_engine.cpp file scope so the portal-graph builder in bot_zgraph.cpp uses
// the exact same id sets as the runtime FC state machine). Stateless consts —
// per-TU copies are harmless.
static constexpr uint16_t SEWER_ITEM_ID = 435; // standard sewer grate (USE to go down)
static constexpr uint16_t SHOVEL_ITEM_ID = 3457; // shared: bot_nav.cpp builds USE_WITH z-leg waypoints
// Shovel hole ground IDs (from register_actions.lua)
static const std::vector<uint16_t> SHOVEL_HOLE_IDS = { 593, 606, 608, 867, 21341 };
// Rope spot ground IDs (from register_actions.lua)
static const std::vector<uint16_t> ROPE_SPOT_IDS = { 386, 421, 12935, 12936, 14238, 17238, 21501, 21965, 21966, 21967, 21968, 23363 };
// Common ladder IDs that have floorchange="down" but NOT type="ladder" in items.xml.
// These are USE items for going UP (the server action handles the teleport).
static const std::unordered_set<uint16_t> LADDER_ITEM_IDS = { 433, 482, 483, 1948, 1968, 5542, 9116, 12799, 17230, 20474, 20475 };
static inline bool isLadderItemId(uint16_t id) {
	return LADDER_ITEM_IDS.count(id) > 0;
}

// TRUE MULTI-FLOOR: the hop the FC state machine is currently executing on
// behalf of the z-route planner. Engine-side state (NOT in BotState — no ABI
// change): keyed by guid in BotEngine::s_plannedFc.
struct ZPlannedHop {
	botnav::ZPortal portal {};
	int64_t plannedAt = 0;
};
static uint32_t botNavSeed(uint32_t guid) {
	static const int64_t s_navSeedEpoch = OTSYS_TIME();
	uint64_t mixed = (static_cast<uint64_t>(guid) * 0x9E3779B97F4A7C15ULL)
		^ static_cast<uint64_t>(s_navSeedEpoch);
	mixed ^= mixed >> 32;
	return static_cast<uint32_t>(mixed);
}
static inline uint64_t botTileKey(const Position& p) {
	return (static_cast<uint64_t>(p.x) << 24) | (static_cast<uint64_t>(p.y) << 8) | static_cast<uint64_t>(p.z);
}
static bool isWalkOnFcTile(const Position& pos) {
	auto tile = g_game().map.getTile(pos);
	return tile && (tile->hasFlag(TILESTATE_FLOORCHANGE) || tile->hasFlag(TILESTATE_TELEPORT));
}
// True when `p` is on/near one of the four elemental shrine hubs. Radius, not equality: the
// hunt scripts' landings sit a few tiles off the exact hub tile (e.g. script 17/18 end at
// (33063,32716,5), 4 tiles from the energy hub), and the Feyrist exit portal drops the bot
// one tile diagonal from the earth hub.
inline bool botNearShrineHub(const Position& p, int32_t radius = 12) {
	for (const auto& hub : kShrineHubs) {
		if (p.z != hub.z) continue;
		const int32_t d = std::max(std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(hub.x)),
		                           std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(hub.y)));
		if (d <= radius) return true;
	}
	return false;
}

// Map a canary town id to shrine_exit.lua's 1-based exitDestination index. ALWAYS returns a
// value in 1..13 — an out-of-range storage value would make shrine_exit.lua evaluate
// Position(nil) and throw, so the "no shrine entrance in this town" case (Dawnport, Farmine,
// Krailos, Roshamuul, Issavi, Feyrist) falls back to the geographically nearest shrine town
// rather than to something unrepresentable. Per the user, routes shouldn't reach a hub from
// those towns at all, so the fallback firing is itself a signal — hence the warn log.
inline uint8_t botShrineIndexForTown(uint32_t townId) {
	for (size_t i = 0; i < std::size(kShrineTownOrder); ++i) {
		if (kShrineTownOrder[i] == townId) return static_cast<uint8_t>(i + 1);
	}
	// Unmapped: nearest shrine town by temple position.
	uint8_t best = 2; // Thais — safe, central default if even the temple lookup fails
	auto srcTown = g_game().map.towns.getTown(townId);
	if (srcTown) {
		const Position src = srcTown->getTemplePosition();
		int32_t bestDist = INT32_MAX;
		for (size_t i = 0; i < std::size(kShrineTownOrder); ++i) {
			auto t = g_game().map.towns.getTown(kShrineTownOrder[i]);
			if (!t) continue;
			const Position tp = t->getTemplePosition();
			const int32_t d = std::max(std::abs(static_cast<int32_t>(src.x) - static_cast<int32_t>(tp.x)),
			                           std::abs(static_cast<int32_t>(src.y) - static_cast<int32_t>(tp.y)));
			if (d < bestDist) { bestDist = d; best = static_cast<uint8_t>(i + 1); }
		}
	}
	g_logger().warn("[SHRINE] town {} has no shrine entrance — falling back to exitDestination index {}",
		townId, best);
	return best;
}

// Stamp Storage.ShrineEntrance so the hub's mystic flames send this bot back to a real town.
// Exact mirror of the adv-stone bridge (bot_engine.cpp:812-824): the bot never physically
// walked an entrance tile, so we supply the state the MoveEvent expects. The value is derived
// from the bot's DB HOME town — that is the same intent as shrine_exit.lua's own fallback
// (`player:getTown():getTemplePosition()`), just expressed as an index so it lands on a
// hardcoded-valid mainland position instead of a temple position that is invalid for some
// bot towns. Idempotent: re-stamping the same value is skipped so this can sit on a hot path.
inline void botStampShrineIndex(const std::shared_ptr<Player>& player, uint8_t idx, const char* site) {
	if (!player || idx < 1 || idx > std::size(kShrineTownOrder)) return;
	if (player->getStorageValue(STORAGE_SHRINE_ENTRANCE) == static_cast<int32_t>(idx)) return;
	player->addStorageValue(STORAGE_SHRINE_ENTRANCE, static_cast<int32_t>(idx));
	g_logger().info("[SHRINE] {} — stamped ShrineEntrance={} (town {}) via {}",
		player->getName(), idx, kShrineTownOrder[idx - 1], site);
}

inline void botStampShrineReturn(const std::shared_ptr<Player>& player, const char* site) {
	if (!player) return;
	const auto& town = player->getTown();
	botStampShrineIndex(player, botShrineIndexForTown(town ? town->getID() : 0), site);
}

// Route-declared return town: `shrine_return:<idx>` in a waypoint's extra_data.
//
// The town CANNOT be derived from the route's own town_id — verified live: both
// bot_city_routes.town_id and bot_hunt_scripts.town_id are 26 (Feyrist) for every route that
// walks a bot onto a hub flame. That is the OWNING town, not the destination, and Feyrist has
// no shrine entrance at all. Nor can it come from the bot: the same hub serves bots from every
// town, and which town a given ROUTE returns to is a property of how that route was authored
// (the Feyrist hunt scripts go out via Port Hope; the Feyrist city routes walk back through
// Thais). So the route declares it, the same way tile_item:/fish:/tool: markers work.
inline bool botStampShrineReturnMarker(const std::shared_ptr<Player>& player,
                                       const std::string& extraData, const char* site) {
	static const std::string kPrefix = "shrine_return:";
	if (extraData.rfind(kPrefix, 0) != 0) return false;
	int parsed = 0;
	try {
		parsed = std::stoi(extraData.substr(kPrefix.size()));
	} catch (...) {
		g_logger().warn("[SHRINE] malformed marker '{}' — ignoring", extraData);
		return false;
	}
	if (parsed < 1 || parsed > static_cast<int>(std::size(kShrineTownOrder))) {
		g_logger().warn("[SHRINE] marker '{}' out of range 1..{} — ignoring",
			extraData, std::size(kShrineTownOrder));
		return false;
	}
	botStampShrineIndex(player, static_cast<uint8_t>(parsed), site);
	return true;
}

static WaypointType parseWaypointType(const std::string& s) {
	if (s == "node") return WaypointType::NODE;
	if (s == "stand") return WaypointType::STAND;
	if (s == "ladder") return WaypointType::LADDER;
	if (s == "rope") return WaypointType::ROPE;
	if (s == "hole") return WaypointType::HOLE;
	if (s == "shovel") return WaypointType::HOLE;  // "shovel" and "hole" both mean dig-down
	if (s == "stairs_up") return WaypointType::STAIRS_UP;
	if (s == "stairs_down") return WaypointType::STAIRS_DOWN;
	if (s == "door") return WaypointType::DOOR;
	if (s == "action") return WaypointType::ACTION;
	if (s == "levitate_up") return WaypointType::LEVITATE_UP;
	if (s == "levitate_down") return WaypointType::LEVITATE_DOWN;
	if (s == "machete") return WaypointType::MACHETE;
	if (s == "use_with") return WaypointType::USE_WITH;
	if (s == "npc_interact") return WaypointType::NPC_INTERACT;
	if (s == "teleport") return WaypointType::TELEPORT;
	return WaypointType::NODE;
}

// ============================================================================
// BOT_CSV — authored-data CSV infrastructure (definitions in bot_csv.cpp).
//
// Lenient-LEXICAL / strict-SEMANTIC split: byte-level noise (BOM, CRLF/LF/lone-CR/mixed,
// blank lines, '#' comments, whitespace around unquoted fields, header order/case, '+' on
// ints) is normalized silently; anything that changes MEANING (unknown/missing/duplicate
// header column, wrong field count, bad integer, unknown enum, unterminated quote,
// duplicate key, orphan/missing file, phase order) throws BotCsvError and the WHOLE load
// aborts. There is no skip-row-and-continue mode.
//
// See implementation_plans/BOT_CSV_WAYPOINTS_IMPLEMENTATION_GUIDE.md §2.
// ============================================================================

inline constexpr const char* BOT_AUTHORED_DIR = "data/bot/authored";

struct BotCsvError {
	std::string file;
	size_t line = 0;   // 1-based physical line; 0 = whole-file problem
	size_t col = 0;    // 1-based field number; 0 = whole-line problem
	std::string reason;
	std::string format() const; // "file:line:col: reason" (omitting zero parts)
};

class BotCsvTable {
public:
	// Parses the whole file up front; throws BotCsvError on ANY lexical or header
	// violation. requiredCols/optionalCols are lowercase; a header column outside both
	// sets is fatal unless it starts with '_' (declared annotation column).
	static BotCsvTable load(const std::string& path,
	                        const std::vector<std::string>& requiredCols,
	                        const std::vector<std::string>& optionalCols);

	size_t rowCount() const {
		return rows_.size();
	}
	bool hasColumn(const std::string& col) const {
		return colIndex_.count(col) != 0;
	}
	// Field text: quoted fields verbatim, unquoted fields trimmed. Returns "" for an
	// optional column absent from this file.
	const std::string& raw(size_t row, const std::string& col) const;
	// Strict integer: trimmed, optional leading '+'/'-', digits only, range-checked.
	int64_t getInt(size_t row, const std::string& col, int64_t minVal, int64_t maxVal) const;
	// As getInt, but an empty/absent value yields def (for optional columns).
	int64_t getIntOr(size_t row, const std::string& col, int64_t minVal, int64_t maxVal, int64_t def) const;
	size_t sourceLine(size_t row) const {
		return rows_[row].sourceLine;
	}
	const std::string& fileName() const {
		return file_;
	}
	// Uniform semantic-error exit for loaders: throws BotCsvError at this row.
	[[noreturn]] void fail(size_t row, const std::string& col, const std::string& reason) const;

	// Public so callers can declare-then-assign (`BotCsvTable t; ... t = load(...)`)
	// around a try block. A default-constructed table is EMPTY, not invalid: rowCount()
	// is 0 and every accessor is index-driven off it, so nothing can read past the end.
	// load() remains the only thing that ever validates, which is the real invariant.
	BotCsvTable() = default;

private:
	struct Row {
		std::vector<std::string> fields;
		size_t sourceLine = 0;
	};
	std::string file_;
	std::unordered_map<std::string, size_t> colIndex_; // lowercased header name -> field slot
	std::vector<Row> rows_;
	inline static const std::string kEmpty {};
};

// Helpers shared by reader, writer and loaders (defined in bot_csv.cpp).
std::string botCsvTrim(const std::string& s);
std::string botCsvLower(std::string s);
// Writer: minimal quoting — quote iff the field contains , " CR LF, has leading/trailing
// whitespace (keeps unquoted-field trimming lossless), or starts with '#'.
std::string botCsvField(const std::string& s);
std::string botCsvFormatTs(int64_t msEpoch); // "YYYY-mm-dd HH:MM:SS" or "never"
// tmp + fsync(file) + one gitignored .bak + rename + fsync(dir). Never in-place.
bool botCsvAtomicWrite(const std::string& path, const std::string& content, std::string& errOut);
// STRICT enum: exactly the strings parseWaypointType recognizes; anything else throws.
// This is the check that kills parseWaypointType's silent NODE fallback above.
WaypointType botCsvWaypointType(const BotCsvTable& t, size_t row, const std::string& col);

// ---- NPC approach reservations (Phase 8) — type + constant shared by engine modules ----
struct ApproachReservation {
	uint32_t guid = 0;
	int64_t expiresAt = 0;
};
// Must cover the WHOLE walk, not just the decision. The claim is stamped once, when the NPC
// candidate is chosen, and its job is to stop two bots converging on the same counter spot.
// At 45s that held only while NPC visits were capped at 40 tiles on one floor; visits are now
// eligible town-wide (150 tiles, 7 floors) with a 240s planner walk budget, so a 45s claim
// expired mid-walk on exactly the long cross-floor visits the planner exists for — and another
// bot could then legally claim the same tile while the first was still en route.
//
// Matches the planner budget in bot_tick.cpp (240s) plus the NPC dwell (up to 60s) and slack.
// The claim is also released explicitly now on every abandonment path (see clearPlannerWalk), so
// a generous TTL no longer means a long-lived leak.
constexpr int64_t APPROACH_RESERVE_MS = 300000;

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// 2026-06-12: single-shot force-wake exemption from the density gate. Set immediately
// before an EXPLICIT wake (cast-viewer login via getHibernatedBotGuidByName, partyhunt
// support assembly, /cavebot wake admin command); consumed (cleared) by the next
// shouldGateWake call regardless of guid match, so it can never linger past one wake
// attempt. Without this, the outerLimitPct=0 band rule turns those paths into
// deterministic permanent failures for any bot >midRadius from all anchors (the old
// outer cap only failed them under real density pressure). A cast-woken bot becomes
// an anchor itself on viewer attach, so the bypass is self-consistent. Same file-scope
// thread_local pattern + rationale as s_inPartyCascade above.
inline thread_local uint32_t s_forceWakeGuid = 0;

// BOT_AMBIENT_ROAM: single-shot "this wake is a roam injection" flag. Same contract and the same
// unconditional-reset discipline as s_forceWakeGuid directly above, and for the same reason — a
// flag that survives one gate decision would hand the elevated limit to an unrelated proximity
// wake, of which the Lua loop fires up to 5 every 300ms.
//
// It is NOT a second force flag. s_forceWakeGuid bypasses the density cap outright (an operator
// asked for this bot); s_roamWakeGuid raises the cap by botRoamReserveSlots and requires the
// roam-specific reserve arm to pass as well. Roam injection must therefore never set BOTH.
//
// Consumed alongside s_forceWakeGuid at the very top of shouldGateWake, BEFORE either early
// return. wakeBot also has three refusals that run before shouldGateWake is reached at all
// (poisoned data, unknown guid, not hibernated), so the injector clears this unconditionally
// after its wakeBot call too — see injectRoamBot.
inline thread_local uint32_t s_roamWakeGuid = 0;

// BOT_LIVENESS (2026-06-13): single-shot "is this a proximity (walk-by / cast-watch)
// wake?" flag, consumed (read + reset to true) at the top of wakeBot. Set false by the
// TELEPORT path (wakeBotsInRadius) and EXPLICIT paths (wakeAllHibernatedBots, /cavebot
// wake, partyhunt assembly/spectate) immediately before their wakeBot call; the party
// cascade re-asserts the parent's value before each member wake. Default true so the
// Lua proximity loop (Game.botWake) and the cast-viewer login wake (naturally excluded
// because the watched bot is not yet an anchor) get the proximity treatment. Only the
// proximity path does off-screen relocation + login sparkle + the short quiet window.
// Same set-immediately-before-wakeBot contract as s_forceWakeGuid. Dispatcher-only.
inline bool s_proximityWake = true;

// ---- Teleport wake: fast tier for bots the arriving player can actually SEE ----
//
// A woken bot APPEARS immediately -- wakeBot materialises it synchronously inside the burst loop
// (internalPlaceCreature then BOT_TELEPORT), in the same dispatcher task as the player's own map
// redraw. What wakeQuietTicks delays is only its first AI TICK, so a bot with a long quiet window
// stands there as a statue: visibly present, completely still.
//
// Teleport wakes previously took the LONG stagger, 3 + (guid*7)%30 ticks at 100ms = 300ms..3.2s,
// while merely walking past a bot gave it 1 + (guid*7)%4 = 100..400ms. That is backwards: arriving
// somewhere is exactly when the world most needs to look alive, and it is what the operator
// reported as "standing still for a second, 2 or 3".
//
// The long stagger was correct when it was written: every wake then carried a synchronous
// 997-name IN query (PlayerBadge::getPlayersInfoByAccount) costing 10-15ms on the dispatcher, so
// compressing a burst meant compressing that blocking too. That query is gone (measured 661 per
// 30 minutes -> 0), which is what makes a fast tier affordable now.
//
// Scope is deliberately narrow: ONLY the quiet window changes, and only for bots inside the
// arriving player's viewport. Selection order, density caps and the population are untouched, so
// exactly the same bots wake -- they simply stop being statues where it shows.
inline Position s_burstCenter {};
inline bool s_burstCenterValid = false;

// Margin on the visible box. 0 = strictly on screen. 2 hedges against the player's first step
// revealing a frozen bot at the edge, matching WAKE_OFFSCREEN_MARGIN elsewhere.
inline constexpr int WAKE_VISIBLE_MARGIN = 2;

// Exact ProtocolGame::canSee box against ONE position, plus the surface/underground rules.
//
// A plain radius check will not do: candidate collection is deliberately z-agnostic, so a bot 3
// tiles away but 5 floors down would qualify while being completely invisible. And this must test
// the BURST CENTRE, not the anchor list -- wouldBeSeenByAnchor asks "can ANY anchor see this",
// which would fast-tier a bot sitting on some distant cast viewer's screen.
inline bool botVisibleFrom(const Position& eye, const Position& p, int margin = WAKE_VISIBLE_MARGIN) {
	if (eye.z <= MAP_INIT_SURFACE_LAYER) {
		if (p.z > MAP_INIT_SURFACE_LAYER) {
			return false;  // surface eye cannot see underground
		}
	} else if (std::abs(static_cast<int>(eye.z) - static_cast<int>(p.z)) > MAP_LAYER_VIEW_LIMIT) {
		return false;      // underground eye sees only +/- MAP_LAYER_VIEW_LIMIT floors
	}
	const int offsetz = static_cast<int>(eye.z) - static_cast<int>(p.z);  // signed: a uint8 diff wraps
	const int px = static_cast<int>(p.x), py = static_cast<int>(p.y);
	const int ex = static_cast<int>(eye.x), ey = static_cast<int>(eye.y);
	return px >= ex - MAP_MAX_CLIENT_VIEW_PORT_X + offsetz - margin
		&& px <= ex + (MAP_MAX_CLIENT_VIEW_PORT_X + 1) + offsetz + margin
		&& py >= ey - MAP_MAX_CLIENT_VIEW_PORT_Y + offsetz - margin
		&& py <= ey + (MAP_MAX_CLIENT_VIEW_PORT_Y + 1) + offsetz + margin;
}

// Fast tier: 1..2 ticks = 100-200ms. Floored at 1, never 0 -- quiet==0 skips the fast path in
// tick() entirely and drops the bot into the isTickDue phase wait, which is up to TICK_FREQ_IDLE
// (10 ticks = 1s) and therefore SLOWER than 1.
inline constexpr uint8_t WAKE_QUIET_VISIBLE_BASE_TICKS = 1;
inline constexpr uint8_t WAKE_QUIET_VISIBLE_SPREAD_TICKS = 2;

inline std::unordered_map<uint32_t, std::pair<size_t, int64_t>> s_leavingWpTimer; // guid → {waypointIdx, startTime}

inline std::unordered_map<uint32_t, int64_t> s_leavingPhaseStart;                // guid → phase start time

inline std::unordered_map<uint32_t, int64_t> s_huntTravelStart;                  // guid → TRAVEL_TO phase start time

// Every activate/wake path seeds tickCounter with a per-bot phase instead of 0, so bots that
// activate or wake in the same batch don't share tick phase and collide on the same %TICK_FREQ_*
// dispatcher windows forever. 30 = lcm(2,3,5,10) covers all TICK_FREQ_* cadences; *7 decorrelates
// sequential guids (guids are near-sequential in the DB). See BOT_NAV_REALISM_PLAN.md Phase 1.
inline constexpr uint32_t BOT_TICK_PHASE_LCM = 30;

inline uint32_t botInitialTickPhase(uint32_t guid) {
	return (guid * 7u) % BOT_TICK_PHASE_LCM;
}

inline std::unordered_map<uint32_t, uint32_t> s_partyLeaderId;       // guid → leader creature ID

// BOT_PARTY_INVITE_RENDEZVOUS: PARTY_MAX_BOTS (was 4) is GONE. A player may conscript as many
// bots as they ask for — some quests need a full team. Human-led members are also exempt from
// botPartyMaxPct by construction (that cap's numerator is s_botToPartyHunt, which human-led
// parties never populate); see the comment at partyCapAllows.

// Party hunt session tracking
inline std::unordered_map<uint32_t, std::vector<uint32_t>> s_partyHuntMembers;   // partyHuntId → member guids

inline std::unordered_map<uint32_t, uint32_t> s_partyHuntScript;                  // partyHuntId → huntScriptId

inline std::unordered_map<uint32_t, uint32_t> s_partyHuntLeaderGuid;              // partyHuntId → EK leader guid

inline std::unordered_map<uint32_t, uint32_t> s_partyHuntDeathCount;              // partyHuntId → total deaths

inline std::unordered_map<uint32_t, uint32_t> s_partyHuntKillCount;               // partyHuntId → total kills

struct NavEvent {
	std::string eventType;
	uint32_t huntScriptId = 0;
	std::string huntScriptName;
	int32_t routeTownId = 0;
	std::string routeType;
	std::string townName;
	std::string botName;
	std::string context;
};

inline std::vector<NavEvent> s_pendingNavEvents;

inline void trackNavEvent(const std::string& eventType,
                          const BotState& bot,
                          uint32_t huntScriptId = 0,
                          const std::string& huntScriptName = "",
                          int32_t routeTownId = 0,
                          const std::string& routeType = "",
                          const std::string& context = "") {
	auto player = bot.getPlayer();
	std::string botName = player ? player->getName() : "unknown";
	std::string townName = bot.townName;
	int32_t effectiveTownId = routeTownId > 0 ? routeTownId : static_cast<int32_t>(bot.townId);
	s_pendingNavEvents.push_back({eventType, huntScriptId, huntScriptName,
	                              effectiveTownId, routeType, townName, botName, context});
}

inline std::string getHuntScriptName(const BotState& bot, const std::vector<HuntScript>& scripts) {
	for (const auto& s : scripts) {
		if (s.id == bot.huntScriptId) return s.name;
	}
	return "";
}

inline const char* waypointTypeName(WaypointType t) {
	switch (t) {
		case WaypointType::NODE: return "node";
		case WaypointType::STAND: return "stand";
		case WaypointType::LADDER: return "ladder";
		case WaypointType::ROPE: return "rope";
		case WaypointType::HOLE: return "hole";
		case WaypointType::STAIRS_UP: return "stairs_up";
		case WaypointType::STAIRS_DOWN: return "stairs_down";
		case WaypointType::DOOR: return "door";
		case WaypointType::ACTION: return "action";
		case WaypointType::LEVITATE_UP: return "levitate_up";
		case WaypointType::LEVITATE_DOWN: return "levitate_down";
		case WaypointType::MACHETE: return "machete";
		case WaypointType::USE_WITH: return "use_with";
		case WaypointType::NPC_INTERACT: return "npc_interact";
		case WaypointType::TELEPORT: return "teleport";
		default: return "unknown";
	}
}

// Server-side Provider for botnav::findPath.
struct BotTileQueryAdapter {
	std::shared_ptr<Creature> creature;
	uint32_t navSeed = 0;
	int32_t jitterMask = 0;

	// canWalkTo returns nullptr for blocked/FC tiles (except the botAllowFcPos
	// whitelisted destination), so FC-avoidance is inherited here for free.
	int32_t tileWalkCost(const Position& pos) const {
		const auto& tile = g_game().map.canWalkTo(creature, pos);
		if (!tile) {
			return -1;
		}
		return static_cast<int32_t>(AStarNodes::getTileWalkCost(creature, tile));
	}

	// Deterministic per-bot per-tile noise in [0, jitterMask]. xor-mul-shift hash.
	int32_t jitter(const Position& pos) const {
		if (jitterMask == 0) {
			return 0;
		}
		uint32_t h = navSeed
			^ (static_cast<uint32_t>(pos.x) * 0x9E3779B1u)
			^ (static_cast<uint32_t>(pos.y) * 0x85EBCA77u);
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		return static_cast<int32_t>(h & static_cast<uint32_t>(jitterMask));
	}

	bool sightClear(const Position& from, const Position& to) const {
		return g_game().map.isSightClear(from, to, true);
	}
};

// Engine-owned reusable node pool (single-threaded dispatcher → one is enough).
inline botnav::PathNodePool s_botPathPool(botnav::PC_DEFAULT_MAX_NODES);

// Convert a forward tile path (start-exclusive) into the Direction list order
// that Player::startAutoWalk consumes (reverse of forward step order).
inline void botPathToDirs(const Position& startPos, const std::vector<Position>& path, std::vector<Direction>& outDirs) {
	outDirs.clear();
	Position prev = startPos;
	for (const auto& p : path) {
		const int dx = static_cast<int>(p.x) - static_cast<int>(prev.x);
		const int dy = static_cast<int>(p.y) - static_cast<int>(prev.y);
		Direction d = DIRECTION_NONE;
		if (dx == 1 && dy == 0) d = DIRECTION_EAST;
		else if (dx == -1 && dy == 0) d = DIRECTION_WEST;
		else if (dx == 0 && dy == 1) d = DIRECTION_SOUTH;
		else if (dx == 0 && dy == -1) d = DIRECTION_NORTH;
		else if (dx == 1 && dy == -1) d = DIRECTION_NORTHEAST;
		else if (dx == 1 && dy == 1) d = DIRECTION_SOUTHEAST;
		else if (dx == -1 && dy == -1) d = DIRECTION_NORTHWEST;
		else if (dx == -1 && dy == 1) d = DIRECTION_SOUTHWEST;
		if (d != DIRECTION_NONE) outDirs.push_back(d);
		prev = p;
	}
	std::reverse(outDirs.begin(), outDirs.end());
}

// Run the pathcore kernel with per-bot jitter against live tiles. Fills dirList
// in startAutoWalk order. Returns true on success. Mirrors the server fpp.
inline bool botJitterPath(const std::shared_ptr<Player>& player, const Position& startPos, const Position& target, const FindPathParams& fpp, uint32_t navSeed, int32_t jitterMask, std::vector<Direction>& dirList) {
	BotTileQueryAdapter adapter { player, navSeed, jitterMask };
	botnav::PathParams pp;
	pp.fullPathSearch = fpp.fullPathSearch;
	pp.allowDiagonal = fpp.allowDiagonal;
	pp.keepDistance = fpp.keepDistance;
	pp.clearSight = fpp.clearSight;
	pp.maxSearchDist = fpp.maxSearchDist;
	pp.minTargetDist = fpp.minTargetDist;
	pp.maxTargetDist = fpp.maxTargetDist;
	std::vector<Position> posPath;
	if (!botnav::findPath(startPos, target, posPath, pp, adapter, s_botPathPool)) {
		return false;
	}
	botPathToDirs(startPos, posPath, dirList);
	return !dirList.empty();
}

inline uint8_t parseVocShorthand(const std::string& s) {
	if (s == "ek" || s == "knight" || s == "elite knight") return 4;
	if (s == "ed" || s == "druid" || s == "elder druid") return 2;
	if (s == "ms" || s == "sorcerer" || s == "master sorcerer") return 1;
	if (s == "rp" || s == "paladin" || s == "royal paladin") return 3;
	return 0;
}

inline const char* vocShortName(uint8_t baseVoc) {
	switch (baseVoc) {
		case 1: return "MS";
		case 2: return "ED";
		case 3: return "RP";
		case 4: return "EK";
		default: return "??";
	}
}

// Find open walkable tiles around a position (for summoning bots)
inline std::vector<Position> findOpenTilesAround(const Position& center, int32_t count) {
	std::vector<Position> tiles;
	static const int32_t offsets[][2] = {
		{-1, 0}, {1, 0}, {0, -1}, {0, 1},
		{-1, -1}, {1, -1}, {-1, 1}, {1, 1},
	};
	for (const auto& off : offsets) {
		if (static_cast<int32_t>(tiles.size()) >= count) break;
		Position checkPos(center.x + off[0], center.y + off[1], center.z);
		auto tile = g_game().map.getTile(checkPos);
		if (tile && !tile->hasFlag(TILESTATE_BLOCKPATH) && !tile->hasFlag(TILESTATE_FLOORCHANGE)
			&& !tile->getTopVisibleCreature(nullptr)) {
			tiles.push_back(checkPos);
		}
	}
	return tiles;
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Track doors that failed to open (key/quest doors, etc.) — skip retries for 60 seconds
inline std::unordered_map<Position, int64_t> s_failedDoors;

inline constexpr int64_t DOOR_RETRY_COOLDOWN_MS = 60000;

// Per-bot breadcrumb trail of recent target positions (for door tracking during chase)
inline constexpr size_t TARGET_TRAIL_SIZE = 30;

struct TargetTrail {
	std::array<Position, TARGET_TRAIL_SIZE> positions {};
	size_t head = 0;
	size_t count = 0;
	void add(const Position& pos) {
		// Don't add duplicate consecutive positions
		if (count > 0) {
			size_t lastIdx = (head + TARGET_TRAIL_SIZE - 1) % TARGET_TRAIL_SIZE;
			if (positions[lastIdx] == pos) return;
		}
		positions[head] = pos;
		head = (head + 1) % TARGET_TRAIL_SIZE;
		if (count < TARGET_TRAIL_SIZE) count++;
	}
	void clear() { head = 0; count = 0; }
};

inline std::unordered_map<uint32_t, TargetTrail> s_targetTrail;

// Effective jitter mask: config value clamped to [0,7] (Opus review — a larger
// mask widens the A* frontier and risks exceeding the node budget).
inline int32_t botNavJitterMaskClamped() {
	int32_t m = g_configManager().getNumber(BOT_NAV_JITTER_MASK);
	return m < 0 ? 0 : (m > 7 ? 7 : m);
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Find a random walkable tile near a POI position within radius.
// If requirePZ is true, only returns PZ tiles. If false, only non-PZ tiles.
// If either is -1, no PZ filter is applied.
inline Position findRandomTileNear(const Position& center, int32_t radius, int pzFilter = -1) {
	std::vector<Position> candidates;
	for (int32_t dx = -radius; dx <= radius; dx++) {
		for (int32_t dy = -radius; dy <= radius; dy++) {
			Position pos;
			pos.x = static_cast<uint16_t>(static_cast<int32_t>(center.x) + dx);
			pos.y = static_cast<uint16_t>(static_cast<int32_t>(center.y) + dy);
			pos.z = center.z;

			auto tile = g_game().map.getTile(pos);
			if (!tile) continue;
			if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) continue;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE)) continue;
			bool isPZ = tile->hasFlag(TILESTATE_PROTECTIONZONE);
			if (pzFilter == 1 && !isPZ) continue;
			if (pzFilter == 0 && isPZ) continue;
			candidates.push_back(pos);
		}
	}
	if (candidates.empty()) return Position();
	return candidates[uniform_random(0, static_cast<int32_t>(candidates.size()) - 1)];
}

// Find a random non-PZ tile adjacent to PZ boundary near a reference position.
// This simulates "just outside the depot/boat" — the PZ edge.
inline Position findPZBoundaryTile(const Position& center, int32_t searchRadius) {
	std::vector<Position> candidates;
	static constexpr int32_t adj[] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	static constexpr int32_t adjY[] = { -1, 0, 1, 0, -1, 1, 1, -1 };

	for (int32_t dx = -searchRadius; dx <= searchRadius; dx++) {
		for (int32_t dy = -searchRadius; dy <= searchRadius; dy++) {
			Position pos;
			pos.x = static_cast<uint16_t>(static_cast<int32_t>(center.x) + dx);
			pos.y = static_cast<uint16_t>(static_cast<int32_t>(center.y) + dy);
			pos.z = center.z;

			auto tile = g_game().map.getTile(pos);
			if (!tile) continue;
			if (tile->hasFlag(TILESTATE_PROTECTIONZONE)) continue; // Must be non-PZ
			if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) continue;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE)) continue;

			// Check if any adjacent tile is PZ (boundary condition)
			bool adjacentToPZ = false;
			for (int d = 0; d < 8; d++) {
				Position neighbor;
				neighbor.x = static_cast<uint16_t>(static_cast<int32_t>(pos.x) + adj[d]);
				neighbor.y = static_cast<uint16_t>(static_cast<int32_t>(pos.y) + adjY[d]);
				neighbor.z = pos.z;
				auto nTile = g_game().map.getTile(neighbor);
				if (nTile && nTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
					adjacentToPZ = true;
					break;
				}
			}
			if (adjacentToPZ) {
				candidates.push_back(pos);
			}
		}
	}
	if (candidates.empty()) return Position();
	return candidates[uniform_random(0, static_cast<int32_t>(candidates.size()) - 1)];
}

// Adventurer's Stone island + dungeon bbox (route_id=19689 waypoint extent + 5-tile margin).
// Bots here must NOT have townId snapped to Rookgaard (the geographically-nearest temple) —
// they retain origin town so the Adv Stone return MoveEvent (uses advStoneStartTownId, NOT
// townId) ends at the bot's birth temple. Without this guard, syncTownIdToPos sets townId=3
// (Rookgaard) which has no boat → startTravel falls into findNearestRecoveryRoute loop.
inline bool isOnAdvStoneIsland(const Position& pos) {
	if (pos.x < 32187 || pos.x > 32216) return false;
	if (pos.y < 32270 || pos.y > 32310) return false;
	return pos.z == 6 || pos.z == 7;
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Per-bot target HP tracking for the 10-second HP-not-decreasing timeout
// Key: bot player ID → (last observed HP, last time HP decreased)
inline std::unordered_map<uint32_t, std::pair<int32_t, int64_t>> s_targetHpTracker;

// Key: bot player ID → creature ID of the tracked target (reset on target change)
inline std::unordered_map<uint32_t, uint32_t> s_lastTrackedTargetId;

// Per-bot z-change timestamp for grace period (suppress attacks after floor change)
inline std::unordered_map<uint32_t, int64_t> s_lastZChangeTime;

// Per-bot consecutive FC scan failure count (stop after 5 until reroll)
inline std::unordered_map<uint32_t, uint8_t> s_fcConsecutiveFailures;

// Per-bot depot locker blacklist (positions that failed to reach)
inline std::unordered_map<uint32_t, std::vector<Position>> s_depotBlacklist;

// ---------------------------------------------------------------------------
// BOT_SHRINE_IDLE — reward / imbuing shrines as ambient POI destinations
// ---------------------------------------------------------------------------
//
// Discovery is a RUNTIME SCAN, deliberately: findNearbyShrines is a near-clone of
// findReachableDepotLocker (bot_waypoint.cpp) and inherits everything that function already
// learned the hard way — the z-1..z+1 band with a cross-z distance penalty so a same-z
// candidate STRICTLY outranks a cross-z one, the "free non-FC adjacency" scoring that keeps a
// bot off stair tiles A* refuses to path onto, occupancy, and demote-don't-exclude ranking.
// Building a swept index instead would have meant a ZCACHE_VERSION bump and a ~21s synchronous
// dispatcher freeze on the next boot to re-derive facts this scan gets for free.
//
// There is no TILESTATE for a shrine, so the predicate is an item-id compare against the tile's
// item list.
//
// THE AUTHORITY FOR THESE IDS IS THE LUA ACTION REGISTRATION, NOT THE MAP. That distinction was
// got wrong once and it cost ~90% of the feature's coverage, so it is worth stating plainly: the
// first version of this list was derived from an OTBM scan and excluded every id the scan did not
// find, on the reasoning that they were "unplaced store variants". The OTBM holds the map's
// ORIGINAL furniture; house contents are restored at runtime from the tile_store blobs, so a
// store-bought shrine standing in a house can NEVER appear in an OTBM scan. Measured after the
// fact: 153 houses hold a gilded imbuing shrine (25182/25183) and 133 a shiny reward shrine
// (25722/25723), against 33 shrines placed on the map — so the omitted ids were the overwhelming
// majority. The same blind spot is why buildHouseInteriorIndex cannot see them either.
//
// Registered by (dataPackDirectory = "data-otservbr-global", coreDirectory = "data"):
//   data/scripts/actions/objects/daily_reward_shrine.lua
//       -> 25720, 25721, 25722, 25723, 25802, 25803
//   data-otservbr-global/scripts/actions/object/imbuement_shrine.lua
//       -> 25060, 25061, 25174, 25175, 25182, 25183, 24964
//
// 25174/25175 and 25720/25721 currently have zero placements anywhere, and are listed anyway:
// they are registered and store-purchasable, so a placement tomorrow must not require a code
// change. That is the same inference error in reverse.
//
// 24964 is deliberately NOT here: it is an "imbuing crystal", a Shaper QUEST ITEM, not a shrine.
// data-canary's variant of the action registers a different set (25103/25104/25202); it is not
// the active data pack, so those ids are decorative here and are excluded.
inline constexpr uint16_t kRewardShrineIds[]  = { 25720, 25721, 25722, 25723, 25802, 25803 };
inline constexpr uint16_t kImbuingShrineIds[] = { 25060, 25061, 25174, 25175, 25182, 25183 };

// 1 = reward, 2 = imbuing. 0 means "not a shrine tile". Kept as a small int rather than an enum
// class because it indexes ShrineMemo's arrays and crosses into BotPOI-adjacent code.
inline constexpr uint8_t SHRINE_KIND_REWARD  = 1;
inline constexpr uint8_t SHRINE_KIND_IMBUING = 2;

// Wider than the locker scan's +-MAP_MAX_VIEW_PORT_X (11). Measured against a full OTBM scan of
// all 58 map-placed shrines: with BOTH the depot and temple anchors scanned and merged, radius 15
// at dz<=1 reaches the imbuing shrine of 17/17 towns and the reward shrine of 16/17 (Carlin's
// nearest reward shrine sits at z=9, two floors under its z=7 anchors, outside the z band). At
// radius 11 the imbuing coverage drops to 16/17 — Kazordoon's is 14 tiles out.
inline constexpr int32_t SHRINE_SCAN_RADIUS = 15;

// P1 (bot-centred) radius. MAP_MAX_VIEW_PORT_X, the same figure findReachableDepotLocker uses and
// for the same reason: the semantic is "a shrine the bot can SEE". Anything further out is P2's
// job. Deliberately smaller than SHRINE_SCAN_RADIUS because P1 cannot be memoized — it is
// bot-relative, so it runs per gated reroll, while P2 runs once per town for the engine's life.
inline constexpr int32_t SHRINE_LOCAL_RADIUS = MAP_MAX_VIEW_PORT_X;

// findReachableDepotLocker uses 2*MAP_MAX_VIEW_PORT_X + 1 because that is ITS radius: the penalty
// only guarantees "any same-z beats any cross-z" if it exceeds the largest in-band Manhattan
// distance. Ours is a wider scan, so the constant is rescaled with it. Leaving it at the locker's
// value would let a near cross-z shrine outrank a far same-z one and reintroduce the up/down FC
// churn that penalty exists to kill.
inline constexpr int32_t SHRINE_CROSS_Z_PENALTY = 2 * SHRINE_SCAN_RADIUS + 1;

// A shrine is always within ~15 of a town anchor, but the BOT can be arbitrarily far from that
// anchor while findNearestTown still returns the town — see the Adventurer's Stone island case
// where a bot's nearest town resolved to Rookgaard from across the world. Without this gate the
// only bound on the walk is the 240s stale-target teardown, i.e. a self-limiting four-minute
// failure loop. Same figure and rationale as NPC_VISIT_MAX_DIST (bot_poi.cpp).
inline constexpr int32_t SHRINE_OFFER_MAX_CHEB = 150;

// Which way to look to see `target` from `from`. Shared by the shrine POI and the house SHRINE
// sub-activity, because both must agree on it: internalCreatureTurn is 4-DIRECTIONAL, so the
// stand tile is always a CARDINAL neighbour of the shrine by construction on both paths, and
// this collapses to the one correct direction. A diagonal stand tile could not face a shrine
// at all, which is why neither path is allowed to pick one.
inline Direction shrineFacingDir(const Position& from, const Position& target) {
	if (target.y < from.y) return DIRECTION_NORTH;
	if (target.y > from.y) return DIRECTION_SOUTH;
	if (target.x < from.x) return DIRECTION_WEST;
	return DIRECTION_EAST;
}

// One shrine tile plus the tile the bot will stand on to face it.
struct ShrineSpot {
	Position shrine;
	Position stand;
};

// Result of one scan pass. Indexed by kind-1, so [0] = reward, [1] = imbuing.
//
// House hits are SEPARATED here rather than filtered out by each caller, and the split is at
// scan time on purpose: `spot`/`found` are the best NON-house hit, so a house shrine structurally
// cannot leak into the town memo or the shrine POI no matter what a caller does. `houseSpot` is
// the best hit that sits on a house tile, with the owning houseId.
//
// The two are reached by different machinery and that is not a detail. A house shrine needs the
// door opened, a tile claim, botHouseMaxOccupants, and — the one that strands a bot —
// s_houseExitPlanner for the walk back OUT, which only endHouseVisit grants. So the shrine POI
// consumes only the non-house half, while the forced command ranks both and DELEGATES to a house
// visit when the house one is nearer. That way "go to the closest shrine" means what an operator
// expects without a shrine walk ever owning a house it cannot leave.
struct ShrineScanResult {
	ShrineSpot spot[2];              // best NON-house hit per kind
	bool       found[2] = { false, false };
	ShrineSpot houseSpot[2];         // best hit sitting on a house tile
	bool       houseFound[2] = { false, false };
	uint32_t   houseId[2] = { 0, 0 };
	// A shrine that WAS found but has no usable cardinal stand tile — every side blocked by a
	// wall or by furniture. Kept so the caller can EXPLAIN the fallback instead of silently
	// walking past it: a reward shrine three tiles away, boxed in by a wall behind, a side table
	// in front and a comfy chair on each side, is indistinguishable from "the search is broken"
	// unless the command says which it was.
	Position   blockedPos[2];
	bool       blockedFound[2] = { false, false };
};

// Per-town memo of where the shrines are. NO TTL and nothing persisted: a shrine is map
// furniture, a pure function of the map, so a stale answer is not a thing that can happen while
// the engine lives. `/cavebot reload` constructs a fresh engine and therefore drops this, which
// IS the invalidation path — a per-town refill is milliseconds.
//
// `scanned` is what makes a NEGATIVE result cheap, and it is load-bearing: Bounac and Feyrist
// have no shrine within 140-206 tiles and Carlin has no reachable reward shrine, so without a
// cached "no" every reroll that passes the gate in those towns would re-run a ~5.8k-tile scan
// forever.
struct ShrineMemo {
	ShrineSpot spot[2];
	bool       found[2] = { false, false };
	bool       scanned = false;
};
inline std::unordered_map<uint32_t /* townId */, ShrineMemo> s_shrineMemo;

// Per-bot blacklist of shrine tiles this bot failed to reach. Mirrors s_depotBlacklist, and it is
// REQUIRED rather than a nicety: the giveup path inserts the POI name into visitedPOIs and so
// self-suppresses for a generation, but the 240s stale-target teardown does NOT touch visitedPOIs.
// With exactly one shrine per kind per town, a shrine that paths plausibly and never completes
// would otherwise be re-picked by the same bot indefinitely, burning a four-minute walk each time.
// Per-bot rather than global, like the depot one, so a single bot's bad start position cannot
// blind every other bot in the town.
inline std::unordered_map<uint32_t, std::vector<Position>> s_shrineBlacklist;

inline void blacklistShrine(uint32_t guid, const Position& pos) {
	auto& bl = s_shrineBlacklist[guid];
	for (const auto& p : bl) {
		if (p.x == pos.x && p.y == pos.y && p.z == pos.z) return;
	}
	bl.push_back(pos);
}

inline bool isShrineBlacklisted(uint32_t guid, const Position& pos) {
	auto it = s_shrineBlacklist.find(guid);
	if (it == s_shrineBlacklist.end()) return false;
	for (const auto& p : it->second) {
		if (p.x == pos.x && p.y == pos.y && p.z == pos.z) return true;
	}
	return false;
}

inline void clearShrineBlacklist(uint32_t guid) {
	s_shrineBlacklist.erase(guid);
}

// BOT_NAV_REALISM Phase 7: persistent lane side, -1 left / 0 center / +1 right. Derived
// deterministically from the bot's stable walkBias personality, so a bot is consistently
// a left / center / right-hand walker across ALL its routes (more realistic than the old
// per-waypoint 9-sqm re-randomize, and stateless — no per-route bookkeeping). Center-
// weighted (~25/50/25) so most bots walk normally and the offset is a minority flavor that
// spreads a shared route without everyone crabbing to one edge. Only consulted when
// botLaneEnable is true (default off — deployed dormant, enabled during a live-observation
// session since the followWaypoints layer can't be navsim-validated offline).
inline int8_t botRouteLaneSide(const BotState& bot) {
	const uint8_t wb = bot.walkBias();
	if (wb <= 3) {
		return -1;
	}
	if (wb >= 12) {
		return 1;
	}
	return 0;
}

// BOT_NAV_REALISM Phase 10 (human-jitter pack): aggregate counters, emitted in the 5-min
// state summary instead of per-event logging (which would spam at 500 bots). Per-bot detail
// still goes to castLog, which is verbose/cast-viewer gated.
inline uint32_t s_jitterDwellCount = 0;

inline uint32_t s_jitterUturnCount = 0;

inline uint32_t s_jitterRerollCount = 0; // Phase 10 item 2: mid-route destination changes

// BOT_NAV_REALISM Phase 7 telemetry: how often a lane-offset bot actually walked its lane
// tile vs. fell back to the waypoint centre (narrow street / occupied / unwalkable). Makes
// the lane layer verifiable from the journal alone — the offline navsim only covers the
// kernel, not followWaypoints. Emitted in the 5-min state summary.
inline uint32_t s_laneOffsetUsed = 0;

inline uint32_t s_laneCenterFallback = 0;

inline uint32_t s_laneReserveClash = 0; // lane tile lost to another bot's reservation this tick

// BOT_NAV_REALISM Phase 7 crowd safety: lane tiles claimed this tick, so two bots walking the
// same route never target the same offset tile in one dispatcher window. Same pattern as
// s_partyFollowReservedThisTick; cleared at the top of tick(). Packed x/y/z key.
inline std::unordered_set<uint64_t> s_laneReservedThisTick;

// BOT_NAV_REALISM Phase 4b (route phase desync) telemetry: how many route entries were
// phase-scattered and the summed start offset (as % of route length) so the 5-min summary
// can report a mean. Proves the scatter engages and isn't degenerate (all near 0%).
inline uint32_t s_desyncScattered = 0;

inline uint32_t s_desyncPhaseSumPct = 0;

// Entries suppressed by the quest / discontinuity guards below (returned 0 instead of a
// scattered index). Reported in the 5-min summary so the guard is visibly firing rather
// than silently indistinguishable from "desync is off".
inline uint32_t s_desyncSuppressed = 0;

// Walk-range-restricted entries (the after-travel site only — see botPatrolEntryIdx's nearPos).
// `Matched` = the bot walked into its patrol from where it stood, no teleport. `None` = no legal
// candidate was within range and the caller fell back to the unrestricted roll + bridge. The ratio
// is the acceptance signal for the gratuitous-teleport fix: `None` should track the ~101 of 215
// scripts that structurally have no near candidate, and `Matched` should be non-zero from the first
// hunt entry onward.
inline uint32_t s_desyncNearMatched = 0;

inline uint32_t s_desyncNearNone = 0;

// True when a patrol waypoint is a positional discontinuity — a point the bot cannot walk
// PAST on foot, so no index beyond it is a legal scatter target.
//
// TELEPORT is always one. USE_WITH is overloaded and needs the sub-form checked
// (handleActionWaypoint):
//   itemId > 0                  -> useItemEx with a created item; a shovel or rope MOVES
//                                  the bot, so treat as a discontinuity
//   itemId == 0, "tile_item:"   -> harvest node (Oramond juicy roots); provably stationary
//   itemId == 0, generic        -> lever/switch, and the carpet/wagon/shrine cases the
//                                  teleport detector treats as expected route TPs
// Unknown levers are assumed to move the bot: conservative in the safe direction.
inline bool botIsRouteDiscontinuity(const Waypoint& wpt) {
	if (wpt.type == WaypointType::TELEPORT) {
		return true;
	}
	if (wpt.type != WaypointType::USE_WITH) {
		return false;
	}
	if (wpt.itemId == 0 && wpt.extraData.rfind("tile_item:", 0) == 0) {
		return false;
	}
	return true;
}

// Sentinel for beginHuntPhase's optional pre-chosen PATROLLING entry index: "no index
// supplied, roll one". SIZE_MAX can never be a valid waypoint index.
inline constexpr size_t kNoPatrolIdx = SIZE_MAX;

// BOT_NAV_REALISM Phase 4b: pick the patrol-loop entry index for a bot. Returns 0 (loop head,
// legacy behavior) when desync is off / the loop is too short / no safe tile exists; otherwise a
// random plain-NODE index so bots entering the same loop don't travel as a lockstep cohort.
// Shared by BOTH patrol-entry paths — the live beginHuntPhase AND the hibernated virtual sim.
// (Live testing showed the virtual sim is actually the DOMINANT path: with most bots hibernated,
// they transition to PATROLLING virtually and resume it on wake without ever calling
// beginHuntPhase, so desyncing only the live path would have left the common case in lockstep.)
//
// `nearPos`/`nearMaxDist` (2026-08-05) restrict the roll to candidates the bot could WALK to from
// where it already stands, and return kNoPatrolIdx when no such candidate exists so the caller can
// decide what to do. Exactly ONE caller passes them: the after-travel patrol entry in doHuntPatrol's
// TRAVEL_TO completion. That site is the only one of four that reaches here with the bot having
// actually walked an authored travelToWaypoints route to its terminus — a terminus authored to land
// it near the patrol head (script 2085 "Falcons": travel_to seq 80 is 3 tiles from hunt_patrol seq 0,
// same z). Without this filter the unrestricted roll picked an index elsewhere in the loop and the
// caller's >30-tile bridge teleported the bot there, discarding the whole walk: measured over the 215
// enabled non-quest hunt scripts with both phases, 87.7% of legal rolls land >30 tiles or off-z from
// the arrival point, and 206 of 215 scripts exceed 50%. The other three entry sites teleport
// regardless of index (no travel route, or the travel budget is spent), so desync is free there and
// they deliberately do NOT pass these.
//
// The filter lives HERE rather than at the call site because this function owns the legality rules
// (the `limit` discontinuity cutoff and the NODE/isWalkOnFc test); re-deriving them at the call site
// would fork the candidate set. Distance is a spatial cutoff and `limit` a sequence cutoff — they are
// independent ANDs over the same candidate, no interaction.
inline size_t botPatrolEntryIdx(const std::vector<Waypoint>& patrolWaypoints, const HuntScript& script,
                               const Position* nearPos = nullptr, int32_t nearMaxDist = 0) {
	if (!g_configManager().getBoolean(BOT_ROUTE_PHASE_DESYNC)) {
		// Desync off: entry is the authored loop head, and any teleport the caller then does is
		// caused by real geometry, not by a roll. Nothing for the near-filter to fix.
		return 0;
	}
	const size_t n = patrolWaypoints.size();
	if (n < 4) {
		return 0;
	}
	// A quest is a linear one-shot walkthrough, not a loop — entering it halfway is
	// meaningless even where the tiles happen to be reachable. The engine already encodes
	// this elsewhere: the virtual sim treats a patrol wrap as "quest complete". Scattering
	// one also silently broke that check, since a quest entered at index 45 of 122 was
	// declared finished after walking only the tail.
	if (script.isQuest || script.scriptCategory == "quest") {
		s_desyncSuppressed++;
		return 0;
	}
	// Everything at or beyond the first positional discontinuity is on the far side of a
	// jump the pathfinder cannot make, so it is not a legal entry point. 59 enabled scripts
	// carry one of these inside hunt_patrol.
	size_t limit = n;
	for (size_t i = 0; i < n; i++) {
		if (botIsRouteDiscontinuity(patrolWaypoints[i])) {
			limit = i;
			break;
		}
	}
	if (limit < 4) {
		s_desyncSuppressed++;
		return 0;
	}
	const size_t start = static_cast<size_t>(uniform_random(0, static_cast<int32_t>(limit) - 1));
	for (size_t probe = 0; probe < limit; probe++) {
		const size_t cand = (start + probe) % limit;
		const auto& wpt = patrolWaypoints[cand];
		// Never scatter onto an FC/ladder/door/action tile — those must be entered deliberately.
		if (wpt.type == WaypointType::NODE && !wpt.isWalkOnFc) {
			// Walk-range restriction, when the caller asked for one. Same-floor and within
			// nearMaxDist Chebyshev of where the bot actually is, i.e. reachable on foot without
			// the caller having to teleport. Mirrors the caller's own bridge test so the two can
			// never disagree about what counts as "close enough".
			if (nearPos != nullptr) {
				if (wpt.pos.z != nearPos->z) {
					continue;
				}
				const int32_t cd = std::max(
					std::abs(static_cast<int32_t>(wpt.pos.x) - static_cast<int32_t>(nearPos->x)),
					std::abs(static_cast<int32_t>(wpt.pos.y) - static_cast<int32_t>(nearPos->y)));
				if (cd > nearMaxDist) {
					continue;
				}
			}
			if (cand > 0) {
				s_desyncScattered++;
				s_desyncPhaseSumPct += static_cast<uint32_t>(cand * 100 / n);
			}
			if (nearPos != nullptr) {
				s_desyncNearMatched++;
			}
			return cand;
		}
	}
	if (nearPos != nullptr) {
		// No legal candidate within walking range. Signal the caller rather than returning 0 —
		// index 0 is not necessarily near either, and the caller needs to distinguish "walk to
		// this one" from "fall through to the unrestricted roll and bridge as before".
		s_desyncNearNone++;
		return kNoPatrolIdx;
	}
	s_desyncSuppressed++;
	return 0;
}

// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 3/4 telemetry. Aggregated into the 5-min state
// summary rather than logged per event — castLog is cast-viewer gated and would be invisible
// for the population at large.
inline uint32_t s_tpSafeRepairs = 0;    // unsafe landing relocated by the spiral/temple tail
inline uint32_t s_tpSafeRewinds = 0;    // unsafe landing resolved by rewinding the route
inline uint32_t s_tpSafeRefused = 0;    // rewind refused (budget spent, or an action in the span)
inline uint32_t s_tpSafeLastResort = 0; // tail exhausted every option — should stay 0
inline uint32_t s_tpSafeWakeRepairs = 0;      // chooseWakePosition fell through to the tail
inline uint32_t s_tpSafeFormationRepairs = 0; // party formation placement repaired

// Per-bot rewind allowance, one per route entry. Reset wherever a route entry happens
// (beginHuntPhase except its nested PATROLLING self-call, findNearestRecoveryRoute,
// startCityRoute, activateBot) and erased in deactivateBot. `inline`, not `static`: a
// file-scope static here would give each of the 15 engine TUs its own copy.
inline std::unordered_map<uint32_t, uint8_t> s_tpRewindBudget;

inline void resetTpRewindBudget(uint32_t guid) {
	s_tpRewindBudget[guid] = 1;
}

// Quests are SHARED between bots, not 1-bot-reserved like a hunt spawn: a linear
// walkthrough is not a spawn to camp, so several bots may be inside one at a time. The only
// gate is how soon the NEXT bot may start it. scriptId -> last start time.
//
// Deliberately NOT erased in deactivateBot: this is keyed by script, not by bot, and a bot
// hibernating must not reset a cooldown that exists to space out OTHER bots. Bounded by the
// script count. `/cavebot reload` dlcloses the .so and wipes it, which resets every quest to
// "never started" — self-healing and bounded by the small quest-bot population.
inline std::unordered_map<uint32_t, int64_t> s_questLastStart;

inline bool botQuestOnCooldown(uint32_t scriptId) {
	auto it = s_questLastStart.find(scriptId);
	if (it == s_questLastStart.end()) return false;
	const int64_t cooldownMs =
		static_cast<int64_t>(g_configManager().getNumber(BOT_QUEST_SCRIPT_COOLDOWN_SEC)) * 1000LL;
	return (OTSYS_TIME() - it->second) < cooldownMs;
}

inline void botStampQuestStart(uint32_t scriptId) {
	s_questLastStart[scriptId] = OTSYS_TIME();
}

// Per-bot depot walk retry counter (idle ticks without arriving)
inline std::unordered_map<uint32_t, uint8_t> s_depotWalkRetries;

inline constexpr int64_t LEAVING_PHASE_MAX_MS = 300000;  // 5 min max total in LEAVING

inline constexpr int64_t HUNT_TRAVEL_MAX_MS = 300000;    // 5 min max in TRAVEL_TO before teleport to spawn

// How far the bot may be from its chosen patrol ENTRY waypoint before the engine bridges the gap
// with a teleport instead of walking it. Named because two things must agree on it: the bridge test
// itself, and botPatrolEntryIdx's walk-range filter (nearPos/nearMaxDist) which exists precisely to
// stop the desync roll from manufacturing a gap this test then has to bridge. If they disagree, the
// filter picks an entry the bridge still teleports to and the fix is silently inert.
inline constexpr int32_t HUNT_PATROL_ENTRY_BRIDGE_DIST = 30;

// How long a bot may spend trying to reach an NPC_INTERACT waypoint before it settles for
// "close enough" (<=3 tiles, same z) and greets from there. This is the pre-2026-08-05 behavior
// kept as a floor: NPC_INTERACT used to arrive at <=3 unconditionally, so any route whose
// waypoint is authored somewhere the bot cannot actually stand still completes — just 8s later
// instead of instantly. Canary's NPC isInTalkRange default is 4, so a greet from 3 still lands.
inline constexpr int64_t NPC_APPROACH_GRACE_MS = 8000;

// ============================================================================
// Quest phase budgets. A quest runs travel_to -> hunt_patrol -> travel_from ONCE, and the
// walkthroughs are long, so the hunt-tuned budgets cut them mid-route — a skipped phase by
// another name.
//
// THE NUMBERS ARE NOT INDEPENDENT. doHunting applies an absolute, phase-agnostic ceiling
// measured from huntStartTime (bot_hunt.cpp, "safety timeout"), and huntStartTime is NOT reset
// at the PATROLLING->LEAVING transition. So every per-phase budget below has to fit INSIDE that
// ceiling or it is fiction. The original 300s margin against a 3600s ceiling meant a quest
// patrol ran to the 55-minute mark and LEAVING got five minutes, not the twenty its own
// constant claimed — and abortHunt does not walk travel_from, so the authored return leg was
// lost again for exactly the long quests that need it.
//
// Worst case with the values below:
//   PREPARING   <= 300s          (RESUPPLY_TIMEOUT, its own independent clock)
//   TRAVEL_TO   <= 1200s         (QUEST_TRAVEL_MAX_MS)
//   PATROLLING  until huntStartTime + (5400 - 1800) = 3600s   (huntEndTime, ABSOLUTE)
//   LEAVING     <= 1200s         -> worst case ends at 4800s
//   CEILING        5400s         (QUEST_SAFETY_TIMEOUT)      => 600s slack.
//
// If you change ONE of these, redo that table.
//
// ---------------------------------------------------------------------------
// Absolute hunt ceilings — the same interlock, for the other two script kinds.
// All three are read at exactly ONE place: doHunting's safety check in bot_hunt.cpp.
//
// ORDINARY HUNT (HUNT_SAFETY_TIMEOUT = 5400):
//   PREPARING   <= 300s   (RESUPPLY_TIMEOUT, own clock)
//   TRAVEL_TO   <= 300s   (HUNT_TRAVEL_MAX_MS, own clock)
//   PATROLLING  until huntEndTime = huntStartTime + botHuntTimeMin..MaxSec (1200-2400s),
//               PLUS the remainder of the current patrol lap — the loop-boundary hunt end
//               only acts on the clock once huntWaypointIdx >= patrolWaypoints.size().
//               This is the ONLY phase with no per-phase clock of its own.
//   LEAVING     <= 300s   (LEAVING_PHASE_MAX_MS, own clock)
//   RESUPPLYING <= 300s   (RESUPPLY_TIMEOUT, own clock)
//   The binding constraint is NOT the live path (which lands near 3000s) but the VIRTUAL sim,
//   which enforces none of the above and whose joint worst case is 4030s. => 1370s slack.
//
// PARTY HUNT (PARTY_SAFETY_TIMEOUT = 12600):
//   PATROLLING  until huntEndTime = huntStartTime + PARTY_HUNT_TIME_MIN..MAX (7200-10800s)
//   + the same four 300s phase clocks + a lap remainder  => 1800s reserved above the max.
//   Before 2026-08-05 this was HUNT_SAFETY_TIMEOUT, which is LOWER than PARTY_HUNT_TIME_MIN,
//   so no party hunt ever reached its own huntEndTime.
// ---------------------------------------------------------------------------
// ============================================================================
inline constexpr int32_t QUEST_SAFETY_TIMEOUT = 5400;      // 90 min absolute ceiling (same value as hunts since 2026-08-05, independent constant)
inline constexpr int32_t QUEST_END_MARGIN_SEC = 1800;      // 30 min reserved for TRAVEL_TO + LEAVING
inline constexpr int64_t QUEST_TRAVEL_MAX_MS = 1200000;    // 20 min in TRAVEL_TO
inline constexpr int64_t QUEST_LEAVING_MAX_MS = 1200000;   // 20 min in LEAVING

// The four quest-budget sites and the enableTeleportStand site all resolve `script` from a
// linear scan that can legitimately yield nullptr (stale or disabled huntScriptId; doHuntLeaving
// also runs with script==nullptr on the recovery-route path). One helper so the null check
// cannot be forgotten at one of them.
inline bool botScriptIsQuest(const HuntScript* script) {
	return script && (script->isQuest || script->scriptCategory == "quest");
}

// The PATROLLING->LEAVING trigger for a quest. Four writers set this (tryStartHunt, the virtual
// picker, /cavebot force-assign, and restoreSingleBotState — the last of which was missed when
// the quest budget first shipped, so a quest bot that hibernated and woke silently reverted to
// the ordinary ~25-minute hunt clock). One helper so the ceiling and the margin can never be
// paired inconsistently again.
inline int64_t botQuestHuntEndTime(int64_t huntStartTime) {
	return huntStartTime + static_cast<int64_t>(QUEST_SAFETY_TIMEOUT - QUEST_END_MARGIN_SEC) * 1000LL;
}

// A quest bot fights back on EVERY leg — travel_to, hunt_patrol and travel_from alike — against
// anything that attacks it, whatever the script's target list says. A hunt only engages its
// listed targets, and only while patrolling.
//
// Retaliation, not aggression: the monster must currently be targeting this bot. A quest is a
// linear one-shot walkthrough, so farming everything in reach would stall the route; walking past
// anything that leaves the bot alone is the point.
//
// Deliberate limits, so they are not rediscovered as bugs:
//   - AoE splash from a monster targeting someone else does not match (its attacked-creature is
//     the other target). Catching that needs a damage hook, which is a much larger change.
//   - A monster that hits once and then retargets stops matching immediately. That is the same
//     rule as "walk past anything that ignores the bot", applied consistently.
// `botCreatureId` is the bot player's creature id (player->getID()), passed in because every
// caller already holds the Player — resolving it per scanned creature would be pure waste.
inline bool botIsQuestRetaliationTarget(const HuntScript* script, uint32_t botCreatureId,
                                        const std::shared_ptr<Creature>& creature) {
	if (!botScriptIsQuest(script) || !creature) return false;
	auto attacked = creature->getAttackedCreature();
	return attacked && attacked->getID() == botCreatureId;
}

// Pack (x,y,z) into uint64_t for tile-set dedup. x uint16 bits 47-32, y uint16 bits 31-16,
// z bits 7-0 — collision-free for valid map coords. Shared by the party-follow reservation
// set (mirrors chooseWakePosition's local packPos so both sets key tiles identically).
inline uint64_t packPosU64(const Position& p) {
	return (static_cast<uint64_t>(p.x) << 32) | (static_cast<uint64_t>(p.y) << 16)
		| static_cast<uint64_t>(p.z);
}

inline std::unordered_map<uint32_t, int64_t> s_retreatUntil;   // guid → time when retreat cooldown expires

inline std::unordered_map<uint32_t, int64_t> s_approachCooldown; // guid → earliest time to re-pathfind approach

inline void blacklistDepotLocker(uint32_t guid, const Position& pos) {
	auto& bl = s_depotBlacklist[guid];
	for (const auto& p : bl) {
		if (p.x == pos.x && p.y == pos.y && p.z == pos.z) return;
	}
	bl.push_back(pos);
}

inline void clearDepotBlacklist(uint32_t guid) {
	s_depotBlacklist.erase(guid);
	s_depotWalkRetries.erase(guid);
}

inline bool isFloorChangeType(WaypointType t) {
	return t == WaypointType::LADDER || t == WaypointType::ROPE ||
		   t == WaypointType::HOLE || t == WaypointType::STAIRS_UP ||
		   t == WaypointType::STAIRS_DOWN || t == WaypointType::LEVITATE_UP ||
		   t == WaypointType::LEVITATE_DOWN;
}

// Check if stepping from `from` to `to` would change the bot's z-level.
// Covers FLOORCHANGE/TELEPORT tiles AND the height-based ramp/cliff mechanic
// from game.cpp:1576-1605 (internalMoveCreature) that silently changes z when
// walking near tiles with height >= 3.
inline bool wouldChangeZ(const Position& currentPos, const Position& destPos) {
	// 1. FLOORCHANGE / TELEPORT tile check (stairs, holes, teleporters)
	if (isWalkOnFcTile(destPos)) return true;

	// 2. Height-based ramp/cliff mechanic (replicates game.cpp:1576-1605)
	// Only applies to non-diagonal movement in the server code
	bool diag = (currentPos.x != destPos.x && currentPos.y != destPos.y);
	if (!diag) {
		// Try go up: current tile is tall (height >= 3), tile above+forward has ground
		if (currentPos.z != 8) {
			auto curTile = g_game().map.getTile(currentPos);
			if (curTile && curTile->hasHeight(3)) {
				auto aboveTile = g_game().map.getTile(destPos.x, destPos.y, destPos.z - 1);
				if (aboveTile && aboveTile->getGround() && !aboveTile->hasFlag(TILESTATE_BLOCKSOLID)) {
					if (!aboveTile->hasFlag(TILESTATE_FLOORCHANGE)) {
						return true; // server would move bot up
					}
				}
			}
		}
		// Try go down: dest tile has no ground, tile below has height >= 3
		if (currentPos.z != 7 && currentPos.z == destPos.z) {
			auto destTile = g_game().map.getTile(destPos);
			if (!destTile || (!destTile->getGround() && !destTile->hasFlag(TILESTATE_BLOCKSOLID))) {
				auto belowTile = g_game().map.getTile(destPos.x, destPos.y, destPos.z + 1);
				if (belowTile && belowTile->hasHeight(3)) {
					return true; // server would move bot down
				}
			}
		}
	}
	return false;
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Per-bot depot locker wait-before-reroll timer (20-60s after reaching locker)
inline std::unordered_map<uint32_t, int64_t> s_depotLockerRerollTime;

// Per-bot depot dwell walk target (for 20% PZ roam / 10% step outside after reroll)
inline std::unordered_map<uint32_t, Position> s_depotDwellWalkTarget;

inline std::unordered_map<uint32_t, int32_t> s_depotDwellWalkFails;

inline std::unordered_map<uint32_t, int32_t> s_travelFcRecoveryCount;                 // guid → FC recovery attempts

inline std::unordered_map<uint32_t, int64_t> s_travelStartTime;                       // guid → travel start time

inline std::unordered_map<uint32_t, std::string> s_travelDestPOI;                     // guid → "boat"/"carpet" (destination)
// BOT_TRAVEL_ARRIVE_MIX: guid → the POI this journey is heading for once it lands ("depot",
// "temple", "ammo", "north.gate", …). Written ONCE and unconditionally in startTravel, read by
// BOTH the live arrival handler and the hibernated virtual twin.
//
// Why stored rather than rolled at arrival: a bot woken mid-journey must not re-roll and walk
// somewhere its hibernated self was not heading (feedback_live_twin_must_match). Why written in
// startTravel rather than lazily: the VIRTUAL arrived handler does not erase the travel maps
// (bot_tick.cpp, "arrived" branch clears travelPhase/travelDestTownId only), so a lazily-written
// entry would survive a virtual arrival and be read by the NEXT journey — including a hunt
// journey, silently defeating the hunt-forces-depot rule. An unconditional write in startTravel
// makes a stale entry unreachable. Erased everywhere s_travelDestPOI is.
inline std::unordered_map<uint32_t, std::string> s_travelArriveTarget;

inline std::unordered_map<uint32_t, std::string> s_travelSrcPOI;                      // guid → "boat"/"carpet" (source)

inline std::unordered_map<uint32_t, Position> s_lastRouteEndPos;                      // guid → last waypoint of completed route

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// s_partyLeaderId and the party-hunt session maps are in bot_engine_impl.hpp.
inline std::unordered_map<uint32_t, Position> s_lastLeaderPos;        // guid → last known leader pos

inline std::unordered_map<uint32_t, int64_t>  s_lastPartyHealTime;   // guid → druid party heal cooldown


inline std::unordered_map<uint32_t, bool>     s_partyWasInactive;    // guid → was inactive before party

inline std::unordered_map<uint32_t, bool>     s_partyPrevSecureMode; // guid → previous secureMode state

inline constexpr int32_t PARTY_LEASH_DIST = 7;      // max Chebyshev dist from leader while chasing

inline constexpr int32_t PARTY_TELEPORT_DIST = 15;   // teleport to leader if further

inline constexpr int32_t PARTY_FOLLOW_DIST = 2;      // stay within this dist of leader

inline constexpr uint8_t PARTY_ROLE_TANK       = 1;  // EK — leads, tanks, challenges

inline constexpr uint8_t PARTY_ROLE_HEALER     = 2;  // ED — heals EK + party, offensive when safe

inline constexpr uint8_t PARTY_ROLE_DPS_MAGE   = 3;  // MS — AoE damage, SD runes

inline constexpr uint8_t PARTY_ROLE_DPS_RANGED = 4;  // RP — ranged DPS, holy AoE

inline std::unordered_map<uint32_t, uint32_t> s_botToPartyHunt;                   // guid → partyHuntId (reverse)

inline uint32_t s_nextPartyHuntId = 1;

// Party hunt constants
inline constexpr int32_t PARTY_HUNT_MIN_LEVEL = 80;            // min level to INITIATE a party hunt (any vocation)

// ROUND2 E: party hunts are open to all vocations and the leader is ELECTED (EK > RP > initiator),
// so the script-level tolerance can no longer assume an EK tank is soaking the spawn. The x3 value
// was calibrated for EK-led parties (tank + healer + DPS); applying it to a mage-led party -- which
// by construction has no tank at all -- is the lethal case. Expressed as num/den integers to keep
// the comparison in integer math.
struct PartyLevelTolerance { int32_t num; int32_t den; };
inline constexpr PartyLevelTolerance partyLevelToleranceFor(uint8_t leaderBaseVoc) {
	switch (leaderBaseVoc) {
		case 4: return { 3, 1 };   // EK leads: tank + full support, the calibrated value
		case 3: return { 2, 1 };   // RP leads: can hold a front line, but no exeta and no EK HP pool
		default: return { 3, 2 };  // MS/ED leads: no tank in the party at all
	}
}

inline constexpr int32_t PARTY_HUNT_HEALER_HP_THRESHOLD = 80;  // heal EK when below 80% HP

inline constexpr int32_t PARTY_HUNT_HEALER_SELF_THRESHOLD = 60; // ED self-heal at 60%

inline constexpr int32_t PARTY_HUNT_SUPPORT_FOLLOW_DIST = 3;   // support bots stay ~3 tiles from EK

inline constexpr int32_t PARTY_HUNT_SUPPORT_TELEPORT_DIST = 12; // teleport if >12 tiles from EK (rare safety net)

// ============================================================================
// BOT_PARTY_INVITE_RENDEZVOUS — one assembly machine, two kinds
//
// A party assembles by TRAVELLING rather than by teleporting. BOT_LED_HUNT is the
// reviewed BOT_PARTY_RENDEZVOUS design (leader holds where it stands, members wind
// down gracefully and converge, a barrier starts the hunt). HUMAN_LED serves /party,
// /cavebot party and click-invite: no hold and no barrier, because the leader is a
// human who is already playing — each member converges independently.
//
// The two kinds differ in ONE load-bearing way beyond the barrier: when they join the
// Canary party. HUMAN_LED joins at commit time, because the human is watching the party
// list AND because reclaimStaleCanaryParty refuses any party containing a real player,
// which makes an assembling human-led party sweep-proof. BOT_LED_HUNT must wait until
// ARRIVED — an all-bot party mid-assembly matches sweepStaleCanaryParties' staleness
// predicate exactly (active bot, no partyHuntId membership marker, state != PARTY) and
// would be reclaimed out from under the assembly.
// ============================================================================
// A member already this close to the anchor on the same floor just walks; anything further is
// staged off-screen near the leader first. Deliberately generous: a genuine town-scale walk
// reads as natural, while a cross-world trip nobody can watch is pure waiting.
inline constexpr int32_t ASSEMBLY_WALK_FROM_DIST = 40;
// Same value chooseWakePosition uses for its own off-screen test (it keeps a function-local
// copy): expand the viewport box by 2 so a bot is not placed one tile past the edge, where an
// observer stepping toward it would reveal the pop-in on the very next tick.
inline constexpr int ASSEMBLY_OFFSCREEN_MARGIN = 2;
// Hard post-condition on a staging tile: a vetted result further than this from the leader is
// REJECTED rather than used. Without it a safety-vet that relocates can silently strand the
// recruit — observed live at 873-1146 tiles, which then became a cross-world walk.
inline constexpr int32_t ASSEMBLY_STAGE_MAX_DIST = 25;

enum class RvKind : uint8_t { BOT_LED_HUNT, HUMAN_LED };
enum class RvPhase : uint8_t { FINISHING, TRAVELLING, WALKING_IN, ARRIVED, FAILED };

struct RvMember {
	uint32_t guid = 0;
	RvPhase phase = RvPhase::FINISHING;
	int64_t phaseSinceMs = 0;
	int64_t travelSinceMs = 0;    // TRAVELLING entry — the maxMs budget clock
	uint8_t role = 0;             // BOT_LED_HUNT only
	bool wasHibernated = false;
	bool canaryJoined = false;    // HUMAN_LED: joined at commit, before ARRIVED
};

struct PartyAssembly {
	uint32_t assemblyId = 0;
	RvKind kind = RvKind::BOT_LED_HUNT;
	uint32_t partyHuntId = 0;      // BOT_LED_HUNT only (0 for HUMAN_LED)
	uint32_t leaderGuid = 0;       // BOT_LED_HUNT: the bot leader's guid
	uint32_t leaderCreatureId = 0; // HUMAN_LED: the human's creature id
	uint32_t anchorTownId = 0;
	Position anchor {};            // leader's live position, refreshed every supervisor pass
	int64_t startedMs = 0;
	std::vector<RvMember> members;
};
inline std::unordered_map<uint32_t /*assemblyId*/, PartyAssembly> s_partyAssembly;
inline std::unordered_map<uint32_t /*guid*/, uint32_t /*assemblyId*/> s_rvMember;
inline uint32_t s_nextAssemblyId = 1;

// Invite acceptance. ACCEPT_WAIT = the human-like pause before answering; HOLDING = the bot
// was mid-fight (or mid-death) when the invite landed and finishes that first.
enum class InvitePhase : uint8_t { ACCEPT_WAIT, HOLDING };
struct PendingInvite {
	uint32_t inviterCreatureId = 0;
	InvitePhase phase = InvitePhase::ACCEPT_WAIT;
	int64_t detectedMs = 0;
	int64_t actAtMs = 0;        // accept time (ACCEPT_WAIT) or hold deadline (HOLDING)
	int64_t lastHoldLogMs = 0;  // rate-limits the [PINVITE] hold line to 10s
};
inline std::unordered_map<uint32_t /*botGuid*/, PendingInvite> s_pendingInvites;
inline int64_t s_lastInvitePollMs = 0;

// Ex-party members that were NEVER logged in before being conscripted. findBotsForParty ranks
// !active bots highest (tier 1), and hibernated bots keep active=true, so without this the
// logged-in population would ratchet upward with every party. When the ordinary hibernation
// rules come for one of these, hibernateBot routes it to deactivateBot instead — same result
// from the world's point of view, but countActiveBots() recovers.
inline std::unordered_set<uint32_t> s_reclaimToInactive;

// Guids in an `invitebot` debug party. An all-bot party has no real player to shield it, so
// sweepStaleCanaryParties would reclaim the test party mid-test; this set exempts it.
inline std::unordered_map<uint32_t /*guid*/, int64_t /*expiresMs*/> s_inviteDebugKeepAlive;

// [PINVITE]/[PARTYRV] counters, dumped on the existing 60s cadence.
struct PartyRvStats {
	uint32_t detected = 0, accepted = 0, declinedPartyHunt = 0, declinedHoldExpired = 0;
	uint32_t declinedAssembling = 0, staleCleared = 0;
	uint32_t asmStarted = 0, asmWalked = 0, asmTeleFallback = 0, asmFinalizedVirtual = 0;
	uint64_t assembleMsTotal = 0; uint32_t assembleCount = 0;
	uint32_t pvpAssistEngagements = 0; // party joined the leader's PvP fight
	uint32_t asmStaged = 0;            // staged off-screen near the leader instead of travelling
};
inline PartyRvStats s_prv;

// Axis-aware "visible-area" leash — the client viewport is X=8 / Y=6 (MAP_MAX_CLIENT_VIEW_PORT_*),
// so a single Chebyshev leash of 7 still allows dy=7 = OFF the leader's screen vertically (confirmed
// live: ED support drifted to dy=7 on Nightmare Isles). Keep supports comfortably on the leader's
// screen on BOTH axes with a 1-tile margin inside the viewport. This is the SOFT constraint enforced
// by walking/repositioning; the Chebyshev-12 teleport above is the hard snap-back safety net.
inline constexpr int32_t PARTY_HUNT_VIEWPORT_LEASH_X = 7;

inline constexpr int32_t PARTY_HUNT_VIEWPORT_LEASH_Y = 5;

// True if `from` is inside the leader's client viewport (with margin) — i.e. on the cast viewer's screen.
inline bool withinViewportLeash(const Position& from, const Position& leaderPos) {
	return std::abs(static_cast<int32_t>(from.x) - static_cast<int32_t>(leaderPos.x)) <= PARTY_HUNT_VIEWPORT_LEASH_X
		&& std::abs(static_cast<int32_t>(from.y) - static_cast<int32_t>(leaderPos.y)) <= PARTY_HUNT_VIEWPORT_LEASH_Y;
}

// P5: a support's standing tile must be within the viewport leash AND have a CLEAR SHOT to the EK
// (no wall between). isSightClear is the SAME engine LOS primitive AreaCombat::getList() uses to
// decide which tiles a great-fireball/avalanche rune actually hits (combat.cpp:2153) — so "within
// leader shot" == "could land a spell on the EK's tile". Reuses existing functionality, no new LOS.
// Caller must ensure `from` and `leaderPos` are same-z (true at the retreat/AoE sites).
inline bool withinLeaderShot(const Position& from, const Position& leaderPos) {
	return withinViewportLeash(from, leaderPos)
		&& g_game().map.isSightClear(from, leaderPos, true);
}

// Target for a bot in a HUMAN-led party. Strict two-level priority, per the user's rule:
//   1. the human leader's own target (monster, same floor as the member, within leash of human)
//   2. else a monster ADJACENT to the human (Chebyshev 1) - i.e. physically attacking them
//   3. else nothing. A human-led party does not pick its own fights.
// Never PvPs. Re-evaluated every tick, so it follows the human's target changes immediately.
inline std::shared_ptr<Creature> pickEkTankTarget(const std::shared_ptr<Player>& ekPlayer,
	const std::shared_ptr<Player>& humanLeader) {
	if (!ekPlayer || !humanLeader) return nullptr;
	const Position ekPos = ekPlayer->getPosition();
	const Position humanPos = humanLeader->getPosition();
	auto cheb = [](const Position& a, const Position& b) {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
			std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	};
	// Never turn on our own side. The leader can misclick one of its own bots, and a mirrored
	// target propagates to every member — so a single stray click would have the whole party
	// beating one of its own. isPartner covers members; the leader and self are explicit.
	auto friendlyFire = [&](const std::shared_ptr<Creature>& c) -> bool {
		if (!c) return true;
		if (c->getID() == ekPlayer->getID() || c->getID() == humanLeader->getID()) return true;
		if (auto cp = c->getPlayer()) {
			if (humanLeader->isPartner(cp) || ekPlayer->isPartner(cp)) return true;
		}
		return false;
	};

	// Primary: the leader's explicit target, same floor as the member, within leash of the leader.
	// PLAYERS AND BOTS QUALIFY, not just monsters: if the leader targets or attacks someone, the
	// party joins in. Previously this was monster-only, so a leader in PvP fought alone.
	if (auto ht = humanLeader->getAttackedCreature();
		ht && ht->getHealth() > 0 && !ht->isRemoved() && !friendlyFire(ht)
		&& ht->getPosition().z == ekPos.z && cheb(ht->getPosition(), humanPos) <= PARTY_LEASH_DIST) {
		return ht;
	}

	// PvP assist fallback: somebody is ATTACKING the leader. Wider than the monster rule below
	// (which demands Chebyshev 1) but still bounded to the leash box, so a mage shelling from 5
	// tiles is answered while an 8-tile sniper is not — deliberately not a griefing radar.
	{
		auto specs = Spectators().find<Creature>(humanPos, false,
			PARTY_LEASH_DIST, PARTY_LEASH_DIST, PARTY_LEASH_DIST, PARTY_LEASH_DIST);
		std::shared_ptr<Creature> bestAtk;
		uint32_t bestAtkId = 0xFFFFFFFFu;
		for (const auto& s : specs) {
			if (!s || s->isRemoved() || s->getHealth() <= 0) continue;
			if (!s->getPlayer()) continue;                 // monsters use the adjacency rule below
			if (friendlyFire(s)) continue;
			if (s->getPosition().z != ekPos.z) continue;
			if (s->getAttackedCreature() != humanLeader) continue;
			if (s->getID() < bestAtkId) { bestAtkId = s->getID(); bestAtk = s; }
		}
		if (bestAtk) return bestAtk;
	}
	// Fallback (user rule, 2026-08-10): ONLY a monster physically on top of the human — Chebyshev
	// distance exactly 1, i.e. one of the 8 adjacent tiles including diagonals. The old fallback
	// was "nearest monster to the EK within PARTY_LEASH_DIST (7) of the human", which is what made
	// a human-led party engage everything on screen whenever the human had no target of its own.
	// The whole party inherits this choice: the ED/MS/RP role fns mirror the EK's target, so
	// narrowing it here is the single change that fixes all of them.
	//
	// Adjacency is deliberately the ONLY test — we do NOT additionally require the monster to have
	// the human as its attacked creature. In a spawn the human is usually the only real player on
	// screen, so nearly every aggroed monster targets them; "targets the human at any range" would
	// degenerate straight back to "attack everything in leash". Conversely a monster standing
	// beside the human while chewing on a bot is still a physical threat to the human and can
	// retarget on any tick (exeta res makes monster targets deliberately unstable).
	//
	// Tie-break is lowest creature ID, NOT nearest-to-self: it is member-independent, so every
	// member that consults this helper focus-fires the SAME monster instead of each picking its
	// own nearest one.
	//
	// NOTE (deliberate override): P7's `2c281db4d` chose the nearest-monster fallback so the EK
	// would roll onto the next monster after a kill and clear a spawn unattended. That is the
	// behaviour being removed here, at the user's explicit request. Autonomous bot-led hunts are
	// unaffected — they acquire targets through scanAndAttackMonster in doHuntPatrol, and this
	// helper has exactly one caller, the human-led EK branch of doPartyFollow.
	auto specs = Spectators().find<Monster>(humanPos, false, 1, 1, 1, 1);
	std::shared_ptr<Creature> best;
	uint32_t bestId = 0xFFFFFFFFu;
	for (const auto& s : specs) {
		if (s->isRemoved() || s->getHealth() <= 0) continue;
		if (s->getPosition().z != ekPos.z) continue;
		if (friendlyFire(s)) continue;
		if (cheb(s->getPosition(), humanPos) != 1) continue; // belt-and-braces on the spectator box
		if (s->getID() < bestId) {
			bestId = s->getID();
			best = s;
		}
	}
	return best;
}

inline constexpr int64_t PARTY_HUNT_TIME_MIN = 7200;           // 2 hours min (seconds)

inline constexpr int64_t PARTY_HUNT_TIME_MAX = 10800;          // 3 hours max (seconds)

inline constexpr int32_t PARTY_HUNT_AOE_EVAL_RADIUS = 5;       // tiles to check for AoE reposition

// Party hunt follower z-change delay: per-follower detection (immune to tick processing order)
inline std::unordered_map<uint32_t, uint8_t>  s_followerLastLeaderZ;     // follower guid → last seen leader z

inline std::unordered_map<uint32_t, int64_t>  s_followerZChangeDetected; // follower guid → when follower first saw z-diff

// P2 (rapid-stairs robustness): unified separation timer. The old z-equality reset zeroed the
// teleport delay whenever the leader's z momentarily matched the follower's (rapid down-then-up
// stair-hop) → the teleport never fired and the follower was stranded. We instead track WHEN the
// follower first became "separated" (different z, OR same z but beyond the follow band) and clear it
// only on TRUE co-location — so a transient z-bounce no longer resets progress.
inline std::unordered_map<uint32_t, int64_t>  s_followerSeparatedSince;     // follower guid → when separation began

inline std::unordered_map<uint32_t, int64_t>  s_partyFollowTeleportCooldown; // follower guid → earliest next teleport

inline std::unordered_map<uint32_t, uint8_t>  s_followerLastTeleLeaderZ;     // follower guid → leader z at our last teleport

inline std::unordered_map<uint32_t, int64_t>  s_followerLeaderZStamp;        // follower guid → when leader last changed z (active-hop detection)

inline constexpr int64_t PARTY_FOLLOW_Z_SETTLE_MS    = 1500; // separation must persist this long before teleport (user: 1.5s)

inline constexpr int64_t PARTY_FOLLOW_TELE_COOLDOWN_MS = 3000; // min gap between teleports (stationary-leader bounce → no spam/freeze)

inline constexpr int64_t PARTY_FOLLOW_TELE_MIN_GAP_MS  = 500;  // hard-min gap even when a genuine NEW leader floor-change bypasses the cooldown

inline constexpr int64_t PARTY_FOLLOW_Z_HOP_WINDOW_MS  = 1200; // leader counts as "actively hopping" within this window of a z-change → transient co-location does NOT clear the separation timer

// Party-follow teleport de-collision: tiles chosen by chooseSafePartyFollowPos this tick.
// Cleared once per tick() (tick top) — bounds multiple same-party supports teleporting in
// one tick to distinct tiles, without ever touching burstReservedTiles_ (whose lifecycle is
// the wake burst, cleared only by beginWakeBurst). Per-tick scope → never grows unbounded.
inline std::unordered_set<uint64_t> s_partyFollowReservedThisTick;

// P8 walk-fight guard: set by doPartyHunt when the role fn (AoE reposition) queued a walk this tick;
// CONSUMED (erased) by followPartyHuntLeader, which then skips its cohesion-walk logic so it never
// stomps the role fn's walk. Consume-once semantics → no cross-tick/cross-party staleness.
inline std::unordered_set<uint32_t> s_roleWalkedThisTick;

// P8 inc2: sticky cardinal formation slot. Per-support EK-relative offset (cardinal preferred, diagonal
// fallback) so supports HOLD a stable position aligned with the EK instead of wandering. Re-rolled at
// most every PARTY_FORMATION_REROLL_MS, or when the slot becomes invalid. s_partyFormationClaims is a
// per-tick set (cleared with s_partyFollowReservedThisTick) of packed (partyHuntId, offset) so ED & MS
// never claim the same slot.
inline std::unordered_map<uint32_t, std::pair<int8_t, int8_t>> s_partyFormationOffset;

inline std::unordered_map<uint32_t, int64_t> s_lastSlotRollMs;

inline std::unordered_set<uint64_t> s_partyFormationClaims;

inline constexpr int64_t PARTY_FORMATION_REROLL_MS = 3000;

// ============================================================================
// BOT_PARTY_TRAIL_FOLLOW — leader breadcrumb trail state
// (implementation_plans/BOT_PARTY_TRAIL_FOLLOW.md)
//
// All engine-local and declared `inline` — NEVER `static` at file scope here: a
// `static` compiles fine and then gives each of the 15 engine TUs its OWN copy,
// silently forking the trail. No BotState / bot_engine_interface.hpp change.
// ============================================================================

enum class TrailNodeKind : uint8_t { STEP = 0, ZHOP = 1, JUMP = 2 };

// Each node is self-contained: prePos = leader position at sample N-1, postPos = at sample N.
// The redundancy with the previous node's postPos is deliberate — the ZHOP executor needs both
// endpoints without walking the deque, and pruning can never orphan a hop's "from" tile.
struct TrailNode {
	uint32_t seq = 0; // monotonic, never reused (contiguous: pruning only pops the front)
	TrailNodeKind kind = TrailNodeKind::STEP;
	Position prePos {}; // "P" — where the leader stood before this transition
	Position postPos {}; // "L" — where it ended up
	int64_t recordedAtMs = 0;
	bool portalResolved = false; // ZHOP only; false => synthetic INFERRED replay
	botnav::ZPortal portal {}; // ZHOP only: the mechanism the leader used
};

struct LeaderTrail {
	std::deque<TrailNode> nodes; // capped at trailCfg_.maxNodes
	uint32_t nextSeq = 1;
	Position lastPos {};
	int64_t lastRecordMs = 0;
};

// Demand registry: followers register their leader here each time they run (10s TTL); the
// recorder at the top of tick() samples every wanted leader at full engine cadence (200ms),
// independent of the leader's own isTickDue phase — sampling inside processBot would drop to
// 400ms (TICK_FREQ_WALKING) and miss steps.
struct TrailWant {
	uint32_t botGuid = 0; // last registrant (diagnostic only)
	uint32_t creatureId = 0; // leader's creature id, for the tick-top lookup
	int64_t expiresMs = 0;
};

inline std::unordered_map<uint32_t /*leaderGuid*/, LeaderTrail> s_leaderTrail;
inline std::unordered_map<uint32_t /*leaderGuid*/, TrailWant> s_trailWanted;
inline constexpr int64_t TRAIL_WANT_TTL_MS = 10000;

// Per-follower cursor into its leader's trail (leader at node 40, ED at 36, MS at 28).
struct FollowerCursor {
	uint32_t leaderGuid = 0;
	uint32_t seq = 0; // next trail node to reach
	int64_t startedMs = 0;
	int64_t lastProgressMs = 0; // cursor advanced OR distance-to-node shrank
	int32_t lastDist = 0;
	uint8_t zhopAttempts = 0;
	int64_t jumpWaitUntilMs = 0; // JUMP grace: stand on the pre-jump tile, see if the map teleports us
	// ROUND2 A3: per-follower cosmetic stagger before starting an FC session, so N followers
	// eligible on the same tick do not all issue startFloorChange in that tick. Replaces the old
	// EXCLUSIVE 3s portal claim, which serialized the whole party through one transition.
	int64_t zhopStartAfterMs = 0;
};
inline std::unordered_map<uint32_t /*followerGuid*/, FollowerCursor> s_followerCursor;

// ZHOP executor session. portalResolved is copied from the trail node at session start and
// gates the fcGiveUpOnPlannedTrans blacklist-skip: only a hop with a confirmed mechanism
// behind it (curated waypoint type or portal-graph match) may skip the quarantine — a
// synthetic INFERRED guess matched nothing, so its repeated failures are exactly the signal
// zBlacklistPortal exists to record.
struct FollowerZHopSession {
	uint32_t leaderGuid = 0;
	uint32_t nodeSeq = 0;
	Position expectedPre {}; // "P"
	Position expectedLanding {}; // "L" — its z is the hard success gate
	int64_t startedAtMs = 0;
	uint8_t attempts = 0;
	bool portalResolved = false;
};
inline std::unordered_map<uint32_t /*followerGuid*/, FollowerZHopSession> s_followerZHopSession;


// Five-way executor result, NOT a bool: DECLINED_FAR ("the STEP walker will close this gap")
// must never be conflated with GIVE_UP ("count this against the teleport watchdog").
enum class TrailZHopResult : uint8_t {
	IN_PROGRESS = 0, // FC session started/running — handleFloorChange owns the next ticks
	APPROACHING = 1, // walking the last 3-6 tiles to P, or waiting out another bot's claim
	SUCCEEDED = 2, // bot.currentPos.z == node.postPos.z — advance the cursor
	DECLINED_FAR = 3, // >6 tiles from P — not the executor's job
	GIVE_UP = 4, // attempts exhausted / cannot stage — hand off to the teleport watchdog
};

// Two full FC sessions per node on top of the FC machine's own internal retry ladder, then
// GIVE_UP (the teleport watchdog takes over, cursor NOT advanced). This cap is what replaces
// zBlacklistPortal's anti-livelock role for party replays — the executor never blacklists a
// RESOLVED portal the leader traversed seconds ago (s_zPortalBlacklist is engine-wide for 10
// minutes; poisoning it would reroute every bot on the server). Do NOT remove this cap.
inline constexpr uint8_t ZHOP_MAX_SESSION_ATTEMPTS = 2;

// Soft claim on a portal tile so 3-4 supports don't lunge at the same ladder in one tick.
// The 3s expiry is deliberately a hardcoded constant, NOT a config key: it is a
// collision-avoidance detail with no operational reason to tune, and it must stay far below
// the FC session budget. A claimed portal yields APPROACHING (wait a tick, don't burn an
// attempt); the claim expires, so it cannot deadlock.

// Bounded leader wait: leaderGuid -> when the current hold started. Cleaned in
// dissolvePartyHunt only (shared, leader-keyed — same lifetime as s_leaderTrail).
inline std::unordered_map<uint32_t /*leaderGuid*/, int64_t> s_partyWaitStartMs;

// Config cache, refreshed on the existing 5s cadence in refreshLivenessCfgIfStale — no
// g_configManager().getNumber() on a hot path.
struct PartyTrailCfg {
	bool enable = false;
	bool humanLead = true;
	int32_t stuckMs = 30000;
	int32_t maxLagTiles = 40;
	int32_t maxNodes = 256;
	int32_t horizon = 10;
	int32_t maxAgeMs = 15000;
	int32_t waitDist = 20;
	int32_t waitMaxMs = 20000;
	int32_t maxPartyPct = 20; // BOT_PARTY_CAP: max pct of logged-in bots party-bound; 0 = uncapped
	// BOT_PARTY_INVITE_RENDEZVOUS: human-led start-walking threshold. NOT
	// PARTY_HUNT_SUPPORT_FOLLOW_DIST (3) — that is the trail's arrival/retire ring.
	int32_t followDist = PARTY_FOLLOW_DIST;
};
inline PartyTrailCfg trailCfg_;

// BOT_PARTY_INVITE_RENDEZVOUS config caches, refreshed on the same 5s cadence as trailCfg_.
struct PartyInviteCfg {
	bool enable = false;
	int32_t pollMs = 1000;
	int32_t acceptMinMs = 1500;
	int32_t acceptMaxMs = 4500;
	int32_t holdMaxMs = 30000;
};
inline PartyInviteCfg inviteCfg_;

// ============================================================================
// BOT_CORPSE_LOOT — a hunting bot opens the corpses it killed so the client's
// "this corpse can be looted" highlight goes away.
//
// The highlight is Container::m_lootHighlightActive (container.hpp), read by
// exactly ONE place in the whole tree — Container::getSpecialCategory — and
// pushed to clients by ProtocolGame::AddItem. Clearing it is a single call,
// clearLootHighlight(nullptr), which is byte-for-byte what stock canary does
// when a real player opens a corpse (lua/creature/actions.cpp:376). The nullptr
// matters: with a player argument the update goes only to that player, and bots
// are clientless, so passing the bot would send nothing to a watching human.
//
// THE 10-SECOND PROBLEM, which shapes everything below. Item::setID strips
// CORPSEOWNER on every decay stage while the highlight flag survives, so 746 of
// 802 monster corpse types become highlighted for EVERY player 10 seconds after
// the kill and stay lit for the next 300-600s. That is both why the feature is
// worth having and why the census cannot sit behind the combat gate: in a dense
// spawn huntTargetId is set almost continuously, and a ranged bot's corpses are
// never adjacent, so a claim captured only between fights would never happen and
// every ranged kill would leak. Hence three passes:
//   census        — every tick, combat included, no pathfinding, claims corpses
//                   while CORPSEOWNER still identifies them as ours
//   open-adjacent — every tick, combat included, no movement
//   walk-and-open — only between fights, budgeted, blacklists what it cannot reach
// ============================================================================
inline constexpr int32_t LOOT_STEP_MS      = 400; // min gap between goTo calls for one corpse
inline constexpr uint8_t LOOT_MAX_FAILS    = 3;   // path failures before a corpse is blacklisted
inline constexpr size_t  LOOT_CLAIM_CAP    = 32;  // dense spawns mint corpses fast
inline constexpr size_t  LOOT_BLOCK_CAP    = 16;
inline constexpr int64_t LOOT_OBS_CACHE_MS = 500;
// Mid-fight WALKING was tried, measured and withdrawn; see section 7c of the plan doc.
// What continues during a fight is the census (so a ranged bot's kills stay claimable
// past the 10s CORPSEOWNER strip) and the adjacent open (zero movement). Only the
// detour is gated off, because it had no arbiter against chaseTarget — for a melee bot
// keep_distance is 0, so there was nothing to arbitrate with at all, and the EK simply
// alternated between walking to a corpse and walking back to its target.

// Config cache, refreshed on the existing 5s cadence in refreshLivenessCfgIfStale.
struct CorpseLootCfg {
	bool enable = false;
	bool walk = true;
	bool publicCleanup = true;
	int32_t radius = 7;
	int32_t windowMs = 20000;
	int32_t scanMs = 750;
	int32_t maxWalkMs = 4000;
	int32_t delayMinMs = 300, delayMaxMs = 800;
};
inline CorpseLootCfg lootCfg_;

// Per-bot loot state. A side table, NOT BotState: BotState lives in
// bot_engine_interface.hpp (the ABI boundary), so a field there would force a full
// rebuild for every future tweak. Same shape as FishingRun and the roam session.
struct LootRun {
	// --- walk pass ---
	std::weak_ptr<Container> corpse;
	Position pos;
	int64_t deadlineMs = 0, nextStepMs = 0;
	uint8_t fails = 0;
	bool hasTarget = false;
	// --- adjacent pass: one pending corpse + its human pause ---
	std::weak_ptr<Container> adjPending;
	Position adjPendingPos;
	int64_t adjOpenAtMs = 0;
	// --- census ---
	int64_t lastCensusMs = 0;
	int64_t windowUntilMs = 0;  // refreshed on OPEN, never on scan (see tickCorpseWalk)
	std::vector<std::pair<std::weak_ptr<Container>, Position>> candidates;
	// Identity keys, never positions: monsters respawn on fixed spawn tiles, so a
	// position key would suppress every future corpse there for the whole hunt.
	// A stored weak_ptr pins the control block, so address reuse cannot alias a live
	// entry and lock() == candidate is a sound comparison. No time expiry: an identity
	// entry cannot collide with a future corpse, and a short expiry would only make a
	// blacklisted corpse get re-walked ~20 times over its 10-minute life.
	std::vector<std::weak_ptr<Container>> claimed;
	std::vector<std::weak_ptr<Container>> blocked;
	// Dedicated real-player-on-screen cache for public cleanup. NOT botWalkObserved:
	// that one skips the bot itself and counts cast-watched bots, and a cast viewer
	// cannot own a corpse, so it is wrong in both directions for this purpose.
	int64_t obsCacheMs = 0;
	bool obsCache = false;
	// Set when a walk run reaches its corpse, so the adjacent pass — which is the only
	// thing that ever actually opens anything — can attribute the open to the walk.
	// Without this `openedWalk` is unreachable and the counters cannot answer the one
	// question that matters: does the walk pass ever deliver a bot to a corpse?
	std::weak_ptr<Container> deliveredByWalk;
};
inline std::unordered_map<uint32_t, LootRun> s_lootRun;

struct CorpseLootStats {
	uint64_t censusPasses = 0, claimed = 0, openedAdj = 0, openedWalk = 0;
	uint64_t openedPublic = 0, walkFail = 0, blacklisted = 0, guardSuppressed = 0;
	uint64_t walkArrived = 0, threatBlocked = 0;
	// Reason counters. Their absence is what hid the oscillation for three rounds:
	// "interruption drops must not blacklist" was right, but it quietly became
	// "interruption drops are not counted either", so thousands of runs ended down a
	// path no counter could see while walkFail=40 suggested the pass was barely used.
	// Rule going forward: every endLootRun exit needs a reason.
	uint64_t runsStarted = 0, walkDropped = 0, adjArmed = 0, adjCancelled = 0;
};
inline CorpseLootStats s_lootStats;

// ============================================================================
// BOT_LURE_KITE — two hunt-combat behaviours, one config table, one state block.
//
// LURE. A hunting bot walks its patrol HOLDING FIRE while monsters aggro and trail
// it, engages the whole pack once enough have gathered, then goes back to luring.
// Armed by hunt_scripts.csv `min_monsters` + a level gate, or unconditionally for a
// party hunt (where the leader's silence is what keeps the supports silent — they
// mirror leaderPlayer->getAttackedCreature(), which is null while it lures).
//
// THE THING THAT MAKES LURE DANGEROUS: keep-distance retreat lives inside
// chaseTarget, which is only reachable from scanAndAttackMonster's ACTIVE-TARGET
// branch. Holding fire means no target, which means no retreat — a luring mage does
// not kite, it walks. That is why the engage trigger list is nine entries long and
// why botLureHpFloorPct exists; do not treat any of them as belt-and-braces.
//
// KITE. A keep-distance bot that has backed as far as it can (cornered, or stopped
// by the 15-tile waypoint drift cap) used to stand still and tank. It now retraces
// the patrol waypoints it ALREADY WALKED — known-walkable ground, by construction —
// ping-ponging over that stretch until the pack dies, then resumes the patrol at the
// waypoint it is physically standing on.
//
// Both states live HERE, not in BotState, for the reason LootRun documents above:
// BotState is the ABI boundary, so every future tweak would force a full rebuild
// instead of a ~21s .so build. `inline`, never `static` — a file-scope static would
// give each of the 17 engine TUs its own copy and silently fork these timers.
// ============================================================================
struct LureKiteCfg {
	// lure
	bool enable = false;
	int32_t levelFactorPct = 130;
	bool partyAlways = true;
	int32_t partyDefaultMin = 3;
	int32_t radius = 7;         // clamped to MONSTER_SCAN_RADIUS at load
	int32_t maxPack = 12;
	int32_t paceDist = 6;
	int32_t paceMaxMs = 4000;
	int32_t maxMs = 60000;
	int32_t hpFloorPct = 70;
	int32_t blockedMs = 3000;
	int32_t contactMs = 1500;
	int32_t decayMs = 6000;
	// kite
	bool kiteEnable = false;
	int32_t kiteDepthWps = 6;
	int32_t kiteMaxSpanTiles = 30;
	int32_t kiteMaxLegs = 6;
	int32_t kiteMaxMs = 45000;
	int32_t kiteCooldownMs = 12000;
};
inline LureKiteCfg lureCfg_;

enum class LurePhase : uint8_t { Off = 0, Luring = 1, Engaging = 2 };

// What tickLure tells doHuntPatrol to do with the rest of its tick.
enum class LureVerdict : uint8_t {
	Inactive,  // not armed — run the pre-feature path verbatim
	Luring,    // hold fire, keep walking waypoints
	Pace,      // hold fire, stand still (tail of the pack is drifting)
	Engage,    // fight: the pre-feature path, minus waypoint advancement
};

struct LureRun {
	LurePhase phase = LurePhase::Off;
	// Staleness stamp. The per-tick gate compares this to bot.huntScriptId: every other
	// gate condition (HUNTING + PATROLLING + script + armed) is equally TRUE for a NEW
	// hunt on a DIFFERENT script, which a virtual reroll during hibernation can produce.
	uint32_t scriptId = 0;
	int64_t startMs = 0;        // re-armed whenever the pack is empty
	int64_t paceUntilMs = 0;
	int64_t contactSinceMs = 0; // sustained melee contact (self)
	int64_t supportSinceMs = 0; // sustained contact on a party support
	int64_t decaySinceMs = 0;   // pack below its peak
	int64_t holdUntilMs = 0;    // hunt-end lure hold (0 = not holding)
	int64_t lastMoveMs = 0;     // for the body-blocked trigger
	Position lastMovePos;
	size_t startWpIdx = 0;
	uint8_t peak = 0;
	uint8_t count = 0;          // last census, for the heartbeat line
	uint8_t supportAggro = 0;
	uint8_t lastTrigger = 0;    // which engage trigger fired (telemetry)
	uint32_t engagements = 0;
};
inline std::unordered_map<uint32_t, LureRun> s_lure;

// `/cavebot <bot> lure <n>` — session-only per-bot min_monsters override, so a spawn can
// be tested without editing authored data. Side map so /cavebot reload clears it for free.
inline std::unordered_map<uint32_t, uint8_t> s_lureOverride;

struct KiteRun {
	bool active = false;
	uint32_t scriptId = 0;      // same staleness stamp as LureRun
	size_t cursor = 0;          // patrol waypoint currently being walked to
	size_t minIdx = 0;          // back edge of the ping-pong window
	size_t anchorIdx = 0;       // forward edge = huntWaypointIdx at entry
	int8_t dir = -1;            // -1 = walking back, +1 = walking forward
	uint8_t legs = 0;           // direction reversals used
	uint8_t pathFails = 0;      // consecutive A* failures toward the cursor
	uint8_t clearTicks = 0;     // consecutive threat-free ticks
	int64_t startMs = 0;
	int64_t cooldownUntilMs = 0; // set ONLY on a give-up exit (see botKiteCooldownMs)
	// Closest this run has ever been to its current cursor, and when. Drives the
	// no-progress watchdog: a kite that is not closing on its cursor is not kiting.
	int32_t bestDist = 0;
	int64_t bestDistMs = 0; // 0 = baseline unset (fresh run, or the cursor just advanced)
};
inline std::unordered_map<uint32_t, KiteRun> s_kite;

// How long a kite may fail to close on its cursor before it gives up. Deliberately far
// below botKiteMaxMs: the timeout is the outer bound for a kite that IS working, this is
// the inner bound for one that is not, and every tick between the two is a tick with both
// target-abandonment checks suppressed for nothing.
inline constexpr int64_t KITE_NO_PROGRESS_MS = 8000;

// True while this bot is mid-kite. Read by scanAndAttackMonster to suppress BOTH
// target-abandonment checks: damage is legitimately slow while repositioning, and
// Check 1's reachability probe runs from the FLEEING position, so a target around a
// corner reads as unreachable and gets blacklisted for the rest of the lap. The kite's
// own maxLegs/maxMs plus the give-up cooldown are what bound this.
inline bool botIsKiting(uint32_t guid) {
	const auto it = s_kite.find(guid);
	return it != s_kite.end() && it->second.active;
}

inline void clearLureRun(uint32_t guid) { s_lure.erase(guid); }

// Kite state is erased EXCEPT the give-up cooldown, which must outlive the run it
// ended — that is the entire point of it.
inline void clearKiteRun(uint32_t guid, bool keepCooldown = true) {
	const auto it = s_kite.find(guid);
	if (it == s_kite.end()) return;
	const int64_t cd = it->second.cooldownUntilMs;
	if (!keepCooldown || cd == 0) {
		s_kite.erase(it);
		return;
	}
	KiteRun fresh;
	fresh.cooldownUntilMs = cd;
	it->second = fresh;
}

// Per-HUNT hygiene: run state only. The `/cavebot <bot> lure <n>` override deliberately
// SURVIVES — it is a "make this bot lure so I can watch it" switch, and a bot rerolls
// hunts every 20-40 minutes, so clearing it here silently disarmed the test bot on its
// next reroll (observed live 2026-08-20). Only `lure off` and unregisterBot drop it.
inline void clearLureKiteState(uint32_t guid) {
	clearLureRun(guid);
	clearKiteRun(guid, /*keepCooldown=*/false);
}

// Full teardown, including the debug override. Bot is leaving the engine entirely.
inline void clearLureKiteStateFull(uint32_t guid) {
	clearLureKiteState(guid);
	s_lureOverride.erase(guid);
}

struct PartyAssemblyCfg {
	bool enable = false;
	int32_t maxMs = 300000;       // TRAVEL+WALK budget from TRAVELLING entry
	int32_t finishMaxMs = 300000; // graceful wind-down cap; must stay >= LEAVING_PHASE_MAX_MS
};
inline PartyAssemblyCfg asmCfg_;

// [PTRAIL] 60s summary counters. Unconditional g_logger line — castLog is verboseLog-gated
// (bot_debug.cpp), so cast-only logging is NOT a usable acceptance baseline. partyTele counts
// every firing of the four existing party teleport branches (the apples-to-apples baseline,
// shipped BEFORE the feature is enabled); watchdogTele counts only teleports that fired after
// a trail cursor gave up. formationTele and respawnTele are the two ACCEPTED teleports
// (formation assembly, death-respawn rejoin), counted separately so the acceptance metric
// cannot flatter itself by hiding them.
struct PtrailCounters {
	uint32_t legs = 0, zhopOk = 0, zhopFail = 0, jumpTele = 0;
	uint32_t watchdogTele = 0, partyTele = 0, formationTele = 0, respawnTele = 0;
	uint32_t zhopAbandoned = 0; // ROUND2 A4: session dropped on convergence without a landing-z match
	int64_t lastEmitMs = 0;
};
inline PtrailCounters s_ptrail;

// BOT_PARTY_LEAK_FIX telemetry.
struct PartyLeakCounters {
	uint32_t dematerialized = 0, reclaimed = 0, orphanCleared = 0;
	int64_t lastSweepMs = 0;
};
inline PartyLeakCounters s_partyLeak;
inline uint32_t s_partyCapRefusals = 0; // BOT_PARTY_CAP telemetry
inline constexpr int64_t PARTY_LEAK_SWEEP_MS = 60000;

// ROUND2 A4: retire a ZHOP session at a CONVERGENCE site (cursor retired, direct-path supersede,
// party exit). SUCCEEDED is only ever produced on re-entry while still separated, but a successful
// hop lands the follower NEXT to the leader -> not separated -> the session used to be erased with
// no counter at all, which is why production read zhopOk=0 while hops were visibly working.
// Counts SUCCESSES ONLY: a session can also be dropped with the hop never completed (e.g. the leader
// came back down to our floor); scoring that as a failure would fabricate failures, and every genuine
// failure is already counted at the GIVE_UP sites.
inline void trailRetireZHopSession(uint32_t guid, uint8_t curZ) {
	auto it = s_followerZHopSession.find(guid);
	if (it == s_followerZHopSession.end()) {
		return;
	}
	if (curZ == it->second.expectedLanding.z) {
		s_ptrail.zhopOk++;
	} else {
		s_ptrail.zhopAbandoned++;
	}
	s_followerZHopSession.erase(it);
}

// R7 diagnostic one-shot: party-hunt support guids already logged as standing on a
// floorchange/teleport tile (the Cormaya ladder-block symptom). Re-arms when the bot
// leaves the tile, so each stall episode logs once.
inline std::unordered_set<uint32_t> s_partyFollowOnFcLogged;

// P8 inc2: compute a support's sticky cardinal formation slot (absolute tile). Cardinal (same x OR y as
// the EK) preferred so waves/beams line up; diagonal fallback when no cardinal is walkable/in-LOS. Sticky
// for >=PARTY_FORMATION_REROLL_MS unless the slot becomes invalid → the support HOLDS position instead of
// wandering. ED & MS take distinct slots via the per-tick s_partyFormationClaims set.
inline Position desiredFormationSlot(uint32_t guid, const Position& curPos, const Position& leaderPos,
	int32_t keepDist, bool preferRing, uint32_t partyHuntId) {
	const int32_t D = std::max(2, preferRing ? keepDist + 1 : keepDist);
	const int64_t nowMs = OTSYS_TIME();
	auto claimKey = [&](int32_t dx, int32_t dy) -> uint64_t {
		return (static_cast<uint64_t>(partyHuntId) << 32)
			| (static_cast<uint64_t>(static_cast<uint8_t>(dx + 100)) << 8)
			| static_cast<uint64_t>(static_cast<uint8_t>(dy + 100));
	};
	auto slotValid = [&](int32_t dx, int32_t dy, uint64_t key) -> bool {
		Position p(static_cast<uint16_t>(leaderPos.x + dx), static_cast<uint16_t>(leaderPos.y + dy), leaderPos.z);
		if (p != curPos && s_partyFormationClaims.count(key)) return false; // taken by the other support
		if (!withinLeaderShot(p, leaderPos)) return false;                  // viewport + LOS to EK
		auto tile = g_game().map.getTile(p);
		if (!tile) return false;
		if (tile->hasFlag(TILESTATE_BLOCKPATH | TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) return false;
		if (auto cs = tile->getCreatures(); cs && !cs->empty() && p != curPos) return false;
		return true;
	};
	// Keep the current sticky offset if still valid (or not yet due for a re-roll).
	auto offIt = s_partyFormationOffset.find(guid);
	auto rollIt = s_lastSlotRollMs.find(guid);
	bool canReroll = (rollIt == s_lastSlotRollMs.end()) || (nowMs - rollIt->second >= PARTY_FORMATION_REROLL_MS);
	if (offIt != s_partyFormationOffset.end()) {
		int32_t dx = offIt->second.first, dy = offIt->second.second;
		if (slotValid(dx, dy, claimKey(dx, dy)) || !canReroll) {
			s_partyFormationClaims.insert(claimKey(dx, dy));
			return Position(static_cast<uint16_t>(leaderPos.x + dx), static_cast<uint16_t>(leaderPos.y + dy), leaderPos.z);
		}
	}
	// Monster cluster centroid (prefer the slot facing the monsters). Distance-independent.
	int64_t sumX = 0, sumY = 0; int32_t mc = 0;
	{
		auto specs = Spectators().find<Monster>(leaderPos, false, 6, 6, 6, 6);
		for (const auto& s : specs) {
			if (s->isRemoved() || s->getHealth() <= 0 || s->getPosition().z != leaderPos.z) continue;
			sumX += s->getPosition().x; sumY += s->getPosition().y; mc++;
		}
	}
	const bool haveCluster = mc > 0;
	const Position cluster(haveCluster ? static_cast<uint16_t>(sumX / mc) : leaderPos.x,
		haveCluster ? static_cast<uint16_t>(sumY / mc) : leaderPos.y, leaderPos.z);
	// (Re)roll: rank cardinals (preferred) then diagonals. Try the desired distance D first, then
	// progressively closer (D-1, D-2, >=2) so narrow/walled spawns still yield a valid slot instead of
	// clumping on the EK (P8 inc2-fix). Tie-break toward the monster cluster (face the monsters).
	struct Cand { int32_t dx, dy; bool cardinal; };
	int32_t bestDx = 0, bestDy = -D; double bestScore = -1e9; bool found = false;
	for (int32_t dd = D; dd >= 2 && !found; --dd) {
		const Cand cands[] = {
			{ 0, -dd, true }, { 0, dd, true }, { -dd, 0, true }, { dd, 0, true },
			{ -dd, -dd, false }, { dd, -dd, false }, { -dd, dd, false }, { dd, dd, false },
		};
		for (const auto& c : cands) {
			if (!slotValid(c.dx, c.dy, claimKey(c.dx, c.dy))) continue;
			double score = c.cardinal ? 100.0 : 0.0;
			if (haveCluster) {
				Position p(static_cast<uint16_t>(leaderPos.x + c.dx), static_cast<uint16_t>(leaderPos.y + c.dy), leaderPos.z);
				score -= std::max(std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(cluster.x)),
					std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(cluster.y)));
			}
			if (score > bestScore) { bestScore = score; bestDx = c.dx; bestDy = c.dy; found = true; }
		}
	}
	if (found) {
		s_partyFormationOffset[guid] = { static_cast<int8_t>(bestDx), static_cast<int8_t>(bestDy) };
		s_lastSlotRollMs[guid] = nowMs;
		s_partyFormationClaims.insert(claimKey(bestDx, bestDy));
		return Position(static_cast<uint16_t>(leaderPos.x + bestDx), static_cast<uint16_t>(leaderPos.y + bestDy), leaderPos.z);
	}
	return leaderPos; // no valid slot — caller's normal follow handles it
}

inline std::unordered_map<uint32_t, int64_t> s_spreadCooldown;  // guid → earliest time to try spread again

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Fix B: Stale-range detection (bot "in range" but can't attack through walls/windows)
inline std::unordered_map<uint32_t, int64_t> s_inRangeSince;

inline std::unordered_map<uint32_t, int64_t> s_inRangeAttackSnapshot;

// Fix C: Post-combat PK immunity (prevent re-entry against same target)
inline std::unordered_map<uint32_t, uint32_t> s_lastFoughtCreature;

inline std::unordered_map<uint32_t, int64_t> s_lastCombatExitTime;

// Fix D: Return walk after combat
inline std::unordered_map<uint32_t, Position> s_returnPos;

inline std::unordered_map<uint32_t, int64_t> s_returnStartTime;

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
inline std::unordered_map<uint32_t, int64_t> s_pzRoamNextTime; // guid -> next mill-around action

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Per-bot last position of target when on same z-level (for transition selection)
inline std::unordered_map<uint32_t, Position> s_targetLastSameZPos;

// Per-bot last time attacker was seen in spectator range (60s memory window, matches Lua)
inline std::unordered_map<uint32_t, int64_t> s_lastAttackerSeenTime;

// Per-bot timestamp when attacker stopped targeting (grace period before combat exit)
inline std::unordered_map<uint32_t, int64_t> s_combatNoTargetSince;

// BOT_PVP_REALISM per-bot transient cooldowns (cleared on death/exit, wiped on reload).
inline std::unordered_map<uint32_t, int64_t> s_pvpDanceCd;   // next allowed dance/strafe step

inline std::unordered_map<uint32_t, int64_t> s_pvpHasteCd;   // next allowed haste attempt

inline std::unordered_map<uint32_t, int64_t> s_pvpWallCd;    // next allowed wall placement

// Two-tick fleeing wall: {wallTile, deadlineMs}. Set on the step tick, executed once
// the bot has physically vacated the tile (so the wall never lands under the bot).
inline std::unordered_map<uint32_t, std::pair<Position, int64_t>> s_pvpPendingWall;

struct GangTileClaim { uint32_t guid = 0; int64_t expiry = 0; };

struct GangMember {
	uint32_t guid = 0;
	Position stagePos;     // PZ-edge tile the member waits on before the burst
	Position attackPos;    // non-PZ tile it steps out to (adjacent for surrounders, ranged otherwise)
	bool ready = false;    // arrived at stagePos
	bool unjustified = false; // owns a skull-reservation slot (victim was unskulled)
	bool surrounder = false;  // level > victim.level/2 -> takes an adjacent tile and boxes
	Position heldTile;     // sticky surround tile (anti-thrash)
	int8_t heldTicks = 0;
	bool teleportFailed = false; // summon teleport skipped (stage tile occupied) -> abort this member
};

struct GangSession {
	uint32_t id = 0;
	uint32_t targetId = 0;
	std::vector<GangMember> members;
	int64_t burstDeadline = 0;
	bool engaged = false;       // barrier released -> all members burst
	bool noAoe = true;          // members are forced single-target (only ever hit the victim)
	uint8_t openMode = 0;       // 0 = nuke-first, 1 = engage-then-trap (50/50, per session)
	int64_t nukeHoldUntil = 0;  // mode 1: suppress nukes until this time (trap first)
	std::unordered_map<uint64_t, GangTileClaim> wallClaims; // packedPos -> who walls it
};

inline std::unordered_map<uint32_t, GangSession> s_gangSessions; // sessionId -> session

inline std::unordered_map<uint32_t, uint32_t>    s_gangByGuid;   // member guid -> sessionId

inline std::unordered_set<uint32_t>              s_gangNoAoe;    // guids forced single-target in castSpell

inline std::unordered_map<uint32_t, int64_t>     s_gangNextScan; // guid -> next initiation scan time

inline std::unordered_map<uint32_t, int32_t>     s_gangVictimLastDist; // guid -> last-tick dist (flee detect)

inline std::unordered_map<uint32_t, int32_t>     s_gangFleeStreak;     // guid -> consecutive dist-increase ticks

inline std::unordered_map<uint32_t, Position>    s_gangVictimLastPos;  // guid -> last-seen victim pos (moving detect)

inline std::unordered_map<uint32_t, int64_t>     s_gangBoxRollNext;    // guid -> next box wall-roll time

inline uint32_t s_gangSessionSeq = 1;

inline int32_t  s_gangPlannedUnjustified = 0; // reserved skull budget across all live gangs

inline int64_t  s_gangLastInitTickMs = 0;     // soft guard: at most ~one new gang formed per tick

// GLOBAL (cross-session) reservation of every live raid's stage + attack tiles -> owner guid.
// Prevents two concurrent raids from seating bots on the same sqm (which the FLAG_NOLIMIT summon
// teleport would happily STACK, leaving an invisible bot still attacking when the top one dies).
inline std::unordered_map<uint64_t, uint32_t> s_gangClaimedTiles; // packedPos -> owner guid

inline uint64_t gangPackPos(const Position& p) {
	return (static_cast<uint64_t>(p.x) << 32) | (static_cast<uint64_t>(p.y) << 16) | p.z;
}

// True if `cid` is already the victim of a live raid (one-raid-per-victim guard).
inline bool gangVictimAlreadyTargeted(uint32_t cid) {
	for (const auto& [id, s] : s_gangSessions) {
		if (s.targetId == cid) return true;
	}
	return false;
}

// Recent gang-mates: after a raid, members stay white-skulled for a while. This remembers who
// raided together (botGuid -> {allyGuid -> expiryMs}) so they never vigilante / random-PK each
// other, WITHOUT disabling bot-on-bot vigilante against non-allies. Outlives the gang session
// (the brawl risk is AFTER exitPK). Lazily pruned; wiped on reload (statics reset).
inline std::unordered_map<uint32_t, std::unordered_map<uint32_t, int64_t>> s_gangRecentAllies;

inline constexpr int64_t GANG_ALLY_TTL_MS = 20 * 60 * 1000; // ~white-skull lifetime + buffer

inline bool gangIsRecentAlly(uint32_t botGuid, uint32_t otherGuid) {
	auto it = s_gangRecentAllies.find(botGuid);
	if (it == s_gangRecentAllies.end()) return false;
	auto jt = it->second.find(otherGuid);
	if (jt == it->second.end()) return false;
	if (OTSYS_TIME() > jt->second) { it->second.erase(jt); return false; } // expired -> prune
	return true;
}

// Per-victim raid cooldown (victim player GUID -> expiryMs): once a victim is jumped it can't be
// jumped again until this expires (config botGangVictimCooldownSec, default 1 day). Lazily pruned;
// wiped on reload. GUIDs are stable + never reused, so this safely survives the victim's death.
inline std::unordered_map<uint32_t, int64_t> s_gangVictimCooldown;

inline bool gangVictimOnCooldown(uint32_t victimGuid) {
	auto it = s_gangVictimCooldown.find(victimGuid);
	if (it == s_gangVictimCooldown.end()) return false;
	if (OTSYS_TIME() >= it->second) { s_gangVictimCooldown.erase(it); return false; } // expired -> prune
	return true;
}

// PK re-engage: track target after give-up for potential re-engagement (declared here so leaveGang
// can clear it). guid -> target creature ID / expiry timestamp.
inline std::unordered_map<uint32_t, uint32_t> s_reengageTarget;

inline std::unordered_map<uint32_t, int64_t> s_reengageUntil;

// The ONLY teardown path for a gang member. Releases its skull reservation + wall claims and
// GCs the session when empty. Called from burst hand-off, staging abort, exitPK, and every
// deactivate/death path (mirrors the s_pvp* static cleanup).
inline void leaveGang(uint32_t guid) {
	s_gangNoAoe.erase(guid);
	// A gang member must never carry a solo re-engage onto its (now-finished) raid victim.
	s_reengageTarget.erase(guid);
	s_reengageUntil.erase(guid);
	auto si = s_gangByGuid.find(guid);
	if (si == s_gangByGuid.end()) return;
	auto se = s_gangSessions.find(si->second);
	if (se != s_gangSessions.end()) {
		auto& s = se->second;
		for (auto it = s.members.begin(); it != s.members.end(); ++it) {
			if (it->guid == guid) {
				if (it->unjustified && s_gangPlannedUnjustified > 0) s_gangPlannedUnjustified--;
				// Release this member's global stage + attack tile reservations (owner-guarded).
				auto rel = [&](const Position& p) {
					auto ct = s_gangClaimedTiles.find(gangPackPos(p));
					if (ct != s_gangClaimedTiles.end() && ct->second == guid) s_gangClaimedTiles.erase(ct);
				};
				rel(it->stagePos);
				rel(it->attackPos);
				s.members.erase(it);
				break;
			}
		}
		for (auto it = s.wallClaims.begin(); it != s.wallClaims.end();) {
			it = (it->second.guid == guid) ? s.wallClaims.erase(it) : std::next(it);
		}
		if (s.members.empty()) s_gangSessions.erase(se);
	}
	s_gangByGuid.erase(si);
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Find a random PZ-safe tile reachable from bot's current position without leaving PZ.
// Uses BFS constrained to PZ tiles only, guaranteeing the path stays within the depot.
inline Position findRandomReachablePZTile(const Position& startPos) {
	std::vector<Position> candidates;
	std::unordered_set<uint64_t> visited;

	auto posKey = [](const Position& p) -> uint64_t {
		return (static_cast<uint64_t>(p.x) << 32) | (static_cast<uint64_t>(p.y) << 16) | p.z;
	};

	std::queue<Position> queue;
	queue.push(startPos);
	visited.insert(posKey(startPos));

	static constexpr int32_t BFS_MAX_TILES = 200;
	static constexpr int32_t MIN_DIST = 3;
	int32_t tilesVisited = 0;

	static constexpr int32_t dx[] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	static constexpr int32_t dy[] = { -1, 0, 1, 0, -1, 1, 1, -1 };

	while (!queue.empty() && tilesVisited < BFS_MAX_TILES) {
		Position cur = queue.front();
		queue.pop();
		tilesVisited++;

		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(cur.x) - static_cast<int32_t>(startPos.x)),
			std::abs(static_cast<int32_t>(cur.y) - static_cast<int32_t>(startPos.y)));
		if (dist >= MIN_DIST) {
			candidates.push_back(cur);
		}

		for (int d = 0; d < 8; d++) {
			Position next;
			next.x = static_cast<uint16_t>(static_cast<int32_t>(cur.x) + dx[d]);
			next.y = static_cast<uint16_t>(static_cast<int32_t>(cur.y) + dy[d]);
			next.z = startPos.z;

			uint64_t key = posKey(next);
			if (visited.count(key)) continue;
			visited.insert(key);

			auto tile = g_game().map.getTile(next);
			if (!tile) continue;
			if (!tile->hasFlag(TILESTATE_PROTECTIONZONE)) continue;
			if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) continue;

			queue.push(next);
		}
	}

	if (candidates.empty()) return Position();
	return candidates[uniform_random(0, static_cast<int32_t>(candidates.size()) - 1)];
}

// BFS from bot position through walkable PZ tiles to find reachable non-PZ tiles at PZ boundary.
// Returns a random one from the candidates found (guarantees pathfinding reachability).
inline Position findClosestNonPZTile(const Position& startPos) {
	std::vector<Position> candidates;
	std::unordered_set<uint64_t> visited;

	auto posKey = [](const Position& p) -> uint64_t {
		return (static_cast<uint64_t>(p.x) << 32) | (static_cast<uint64_t>(p.y) << 16) | p.z;
	};

	static constexpr int32_t dx[] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	static constexpr int32_t dy[] = { -1, 0, 1, 0, -1, 1, 1, -1 };

	std::queue<Position> queue;
	queue.push(startPos);
	visited.insert(posKey(startPos));

	static constexpr int32_t BFS_MAX_TILES = 300;
	int32_t tilesVisited = 0;

	while (!queue.empty() && tilesVisited < BFS_MAX_TILES) {
		Position cur = queue.front();
		queue.pop();
		tilesVisited++;

		for (int d = 0; d < 8; d++) {
			Position next;
			next.x = static_cast<uint16_t>(static_cast<int32_t>(cur.x) + dx[d]);
			next.y = static_cast<uint16_t>(static_cast<int32_t>(cur.y) + dy[d]);
			next.z = startPos.z;

			uint64_t key = posKey(next);
			if (visited.count(key)) continue;
			visited.insert(key);

			auto tile = g_game().map.getTile(next);
			if (!tile) continue;
			if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_BLOCKPATH)) continue;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE)) continue;

			if (!tile->hasFlag(TILESTATE_PROTECTIONZONE)) {
				// Found a reachable non-PZ tile at PZ boundary
				candidates.push_back(next);
				// Don't expand further from non-PZ tiles — we want tiles right at the boundary
				continue;
			}

			// PZ tile — keep expanding
			queue.push(next);
		}
	}

	if (candidates.empty()) return Position();
	return candidates[uniform_random(0, static_cast<int32_t>(candidates.size()) - 1)];
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
struct BotDebugCfg {
	bool enabled = false;
	bool gridEnabled = true;
	bool eventsEnabled = true;
	int32_t snapshotMs = 2000;
	int64_t debugStartTime = 0;     // OTSYS_TIME when "debug on" was issued
	int64_t lastSnapshot = 0;       // last heartbeat emission timestamp
	int64_t lastBotTickTime = 0;    // last time THIS bot was processed (for tickGap)
	int64_t lastEventTime = 0;      // last event emission (for inter-event gap)
	int64_t lastWalkTargetSetTime = 0;  // for stuck-detector
	Position lastWalkTarget;

	// Spell-impact tracking: capture before cast, evaluate next tick
	struct PendingCast {
		bool active = false;
		int64_t emittedAt = 0;             // when pre-cast frame was logged
		std::string descriptor;             // "AOE_SPELL exori dir=N words=exori"
		std::vector<Position> areaTiles;   // post-rotation affected tiles
		std::unordered_map<uint32_t, std::tuple<std::string, int32_t, int32_t>> preTargets;
		// creatureId → {name, dx, dy} captured at cast time (dx/dy relative to bot)
		std::unordered_map<uint32_t, int32_t> preHp;     // creatureId → HP at cast time
		std::unordered_map<uint32_t, int32_t> preMaxHp;  // creatureId → MaxHP at cast time
	};
	PendingCast pending;
};

inline std::unordered_map<uint32_t, BotDebugCfg> s_debugConfigs;

inline BotDebugCfg* getDebugCfg(uint32_t guid) {
	auto it = s_debugConfigs.find(guid);
	return it != s_debugConfigs.end() && it->second.enabled ? &it->second : nullptr;
}

// Debug-mode dimensions: 15 wide × 11 tall, bot at center (7L/7R, 5U/5D)
inline constexpr int32_t DBG_GRID_HALF_X = 7;

inline constexpr int32_t DBG_GRID_HALF_Y = 5;

inline int64_t s_lastNavEventFlush = 0;

inline constexpr int64_t NAV_EVENT_FLUSH_INTERVAL_MS = 30000; // flush every 30s

inline void flushNavEvents() {
	if (s_pendingNavEvents.empty()) return;
	auto now = OTSYS_TIME();
	if (now - s_lastNavEventFlush < NAV_EVENT_FLUSH_INTERVAL_MS) return;
	s_lastNavEventFlush = now;

	// Aggregate events by natural key (event_type, hunt_script_id, route_town_id, route_type)
	struct AggKey {
		std::string eventType;
		uint32_t huntScriptId;
		int32_t routeTownId;
		std::string routeType;
		bool operator==(const AggKey& o) const {
			return eventType == o.eventType && huntScriptId == o.huntScriptId
				&& routeTownId == o.routeTownId && routeType == o.routeType;
		}
	};
	struct AggKeyHash {
		size_t operator()(const AggKey& k) const {
			size_t h = std::hash<std::string>{}(k.eventType);
			h ^= std::hash<uint32_t>{}(k.huntScriptId) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.routeTownId) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<std::string>{}(k.routeType) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};
	struct AggValue {
		std::string huntScriptName;
		std::string townName;
		std::string botName;
		std::string context;
		uint32_t count = 0;
	};

	std::unordered_map<AggKey, AggValue, AggKeyHash> aggregated;
	for (const auto& ev : s_pendingNavEvents) {
		AggKey key{ev.eventType, ev.huntScriptId, ev.routeTownId, ev.routeType};
		auto& val = aggregated[key];
		val.count++;
		// Keep the latest event's metadata
		val.huntScriptName = ev.huntScriptName;
		val.townName = ev.townName;
		val.botName = ev.botName;
		val.context = ev.context;
	}

	// Build one async query per aggregated event (typically ~10-20 upserts, not thousands)
	auto& db = Database::getInstance();
	for (const auto& [key, val] : aggregated) {
		std::string eventTypeStr = db.escapeString(key.eventType);
		std::string huntIdStr = std::to_string(key.huntScriptId);
		std::string huntNameStr = db.escapeString(val.huntScriptName);
		std::string routeTownStr = std::to_string(key.routeTownId);
		std::string routeTypeStr = db.escapeString(key.routeType);
		std::string townStr = !val.townName.empty() ? db.escapeString(val.townName) : "NULL";
		std::string botNameStr = db.escapeString(val.botName);
		std::string ctxStr = !val.context.empty() ? db.escapeString(val.context) : "NULL";
		std::string countStr = std::to_string(val.count);

		g_botDatabaseTasks().execute(fmt::format(
			"INSERT INTO `bot_nav_events` "
			"(`event_type`, `hunt_script_id`, `hunt_script_name`, `route_town_id`, "
			"`route_type`, `town_name`, `bot_name`, `context`, `event_count`) "
			"VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}) "
			"ON DUPLICATE KEY UPDATE "
			"`event_count` = `event_count` + {}, "
			"`hunt_script_name` = {}, "
			"`town_name` = {}, "
			"`bot_name` = {}, "
			"`context` = {}, "
			"`last_seen` = CURRENT_TIMESTAMP",
			eventTypeStr,
			huntIdStr, huntNameStr, routeTownStr,
			routeTypeStr, townStr, botNameStr, ctxStr, countStr,
			countStr,
			huntNameStr, townStr, botNameStr, ctxStr
		));
	}
	s_pendingNavEvents.clear();
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Check if any step in a direction list would land on a floor-change tile.
// Only checks FLOORCHANGE/TELEPORT flags — NOT height-based ramps, because many
// valid paths in mountainous terrain (Kazordoon, etc.) pass through height-3 tiles
// without actually triggering z-changes. Height-based z-changes during walks are
// handled by Fix 3 (clear walk queue on unexpected z-change in tick).
inline bool hasFloorChangeTileInPath(const Position& start, const std::vector<Direction>& dirs) {
	Position pos = start;
	for (auto dir : dirs) {
		switch (dir) {
			case DIRECTION_NORTH: pos.y--; break;
			case DIRECTION_SOUTH: pos.y++; break;
			case DIRECTION_EAST: pos.x++; break;
			case DIRECTION_WEST: pos.x--; break;
			case DIRECTION_NORTHEAST: pos.x++; pos.y--; break;
			case DIRECTION_NORTHWEST: pos.x--; pos.y--; break;
			case DIRECTION_SOUTHEAST: pos.x++; pos.y++; break;
			case DIRECTION_SOUTHWEST: pos.x--; pos.y++; break;
			default: break;
		}
		if (isWalkOnFcTile(pos)) return true;
	}
	return false;
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// PERF_INVESTIGATION_2026-05-24 Phase B (2026-06-01): file-scope thread_local for the
// party-cascade flag. Lives outside the class so both wakeBot (which sets/clears it
// around the cascade loop) and shouldGateWake (which reads it to exempt cascade
// members from the density cap) can access it without passing a parameter through
// every interim caller. Dispatcher-thread-only; thread_local is belt-and-braces.
inline thread_local bool s_inPartyCascade = false;

// Phase B: per-cluster cap-hit log rate-limiter (5s per cluster centroid). Keyed by
// packed centroid coords. Dispatcher-thread-only; never accessed from elsewhere.
inline std::unordered_map<uint64_t, int64_t> s_lastDensityCapHitLogMs;

// JITTER DIAGNOSTIC (the jitter root-cause analysis §3.4):
// wakeBotsInRadius burst accumulator. Reset at start of each radius call,
// summed by every wakeBot call within. Logged at end of radius if exceeded.
//
// INVARIANT: wakeBotsInRadius is NOT re-entrant — the reset-at-entry would destroy
// the partial state of an outer burst. Current code paths cannot nest (single
// dispatcher thread, no callback chains re-enter wakeBotsInRadius), so this is safe.
// If a future change introduces nesting, switch to a thread-local stack of bursts.
inline int64_t s_wakeBurstAccumMs = 0;

inline int64_t s_wakeBurstMaxSingleMs = 0;

inline uint32_t s_wakeBurstCount = 0;

inline const char* s_wakeBurstCallSite = "proximity";  // set to "teleport" by wakeBotsInRadius

// Player-proximity weighting telemetry (2026-06-15). Reset in the 60s [PROXBIAS] summary.
inline uint64_t s_proxSelNear = 0, s_proxSelMid = 0, s_proxSelFar = 0;   // chosen-candidate tier

inline int64_t s_lastProxBiasLogMs = 0;      // rate-limiter for per-selection [PROXBIAS] logs

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Per-bot last-movement timestamp for stuck detection (attack blockers after 3s immobile)
inline std::unordered_map<uint32_t, int64_t> s_lastMoveTime;

// Per-bot z-pursuit ATTEMPT timestamp (cooldown between attempts)
inline std::unordered_map<uint32_t, int64_t> s_lastZPursuitTime;

// Tracks last successful floor change per bot: {pre-FC position (old z), post-FC position (new z)}
// Used for z-mismatch recovery: if a route completes on wrong z, bot walks back through the FC
inline std::unordered_map<uint32_t, std::pair<Position, Position>> s_lastFcPositions;

// Periodic heartbeat — last timestamp per bot (for 60s status messages)
inline std::unordered_map<uint32_t, int64_t> s_lastHeartbeat;

// Bug fix: safety nets for stuck detection
inline std::unordered_map<uint32_t, std::pair<uint64_t, int64_t>> s_walkTargetTimer;  // guid → {targetHash, startTime}

inline uint64_t s_proxHuntNearEligible = 0, s_proxHuntNearReserved = 0;  // hunt near-player supply

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------

// Reroll outcome counters (reset each 5-min summary)
inline uint32_t s_rerollDwell = 0, s_rerollPoi = 0, s_rerollPoiFail = 0;

inline uint32_t s_rerollHunt = 0, s_rerollHuntFail = 0, s_rerollTravel = 0, s_rerollTravelSame = 0;

inline uint32_t s_rerollCityWalk = 0, s_rerollTotal = 0;

// BOT_ACTIVITY_PCT telemetry. Every terminal path in doActivityReroll increments exactly one
// of these, so the outcome counters SUM TO s_rerollTotal. The old summary did not: rolls that
// ended in an untelemetered dwell tail simply vanished (measured 40 of 466 missing), which is
// how a 22% bin delivering 0% stayed invisible for months.
inline uint32_t s_rerollParty = 0, s_rerollPartyFail = 0;
// Cumulative-since-load mirrors of the window counters above. Never reset, so the realised
// distribution can be read at any time without waiting for (or racing) a 5-minute flush.
// Index order matches ActOutcome below.
inline uint64_t s_actCum[11] = {0,0,0,0,0,0,0,0,0,0,0};
inline uint32_t s_rerollTravelFail = 0, s_rerollRoundingTail = 0;
// Per-bin ineligibility (bin zeroed BEFORE the roll: hunt/party cooldown, pz-lock).
inline uint32_t s_rerollIneligHunt = 0, s_rerollIneligParty = 0, s_rerollIneligPz = 0;

// Table validity, recomputed on every load/reloadconfig. A bad table must not be a startup log
// line that scrolls away on a 500-bot server -- it stays visible in `/cavebot botcfg` and in the
// 5-minute REROLL summary until it is fixed.
inline bool     s_actTableValid = true;
inline int32_t  s_actTableSum   = 100;
inline bool     s_poiTableValid = true;
inline int32_t  s_poiTableSum   = 100;
inline bool     s_houseTableValid = true;
inline int32_t  s_houseTableSum   = 100;

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
inline constexpr int64_t HEARTBEAT_INTERVAL_MS = 60000;

inline std::unordered_map<uint32_t, int64_t> s_staleWalkStart;   // guid → when stale listWalkDir first detected

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
inline constexpr int32_t RUNE_RANGE = 7;

inline constexpr int32_t RUNE_AOE_MIN_TARGETS = 2;

// Direction to short string for debug logging
inline const char* dirToStr(Direction d) {
	switch (d) {
		case DIRECTION_NORTH: return "N";
		case DIRECTION_EAST: return "E";
		case DIRECTION_SOUTH: return "S";
		case DIRECTION_WEST: return "W";
		case DIRECTION_NORTHEAST: return "NE";
		case DIRECTION_NORTHWEST: return "NW";
		case DIRECTION_SOUTHEAST: return "SE";
		case DIRECTION_SOUTHWEST: return "SW";
		default: return "?";
	}
}

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Per-bot blocker retry count in WALKING_TO floor change state
inline std::unordered_map<uint32_t, uint8_t> s_fcBlockerRetries;

// Failed approaches to the PLANNED portal, per bot. Separate from s_fcBlockerRetries because a
// blocked path and an unreachable portal need different budgets: see fcGiveUpOnPlannedTrans.
inline std::unordered_map<uint32_t, uint8_t> s_fcPlannedApproachFails;

// Tool item IDs for action system — SHOVEL_ITEM_ID hoisted to bot_engine_impl.hpp
// (the synthetic z-leg in bot_nav.cpp builds a USE_WITH waypoint from it).
inline constexpr uint16_t ROPE_ITEM_ID = 3003;

// --------------------------------------------------------------------------
// Shared engine-wide definitions (Phase 11 module split)
// Hoisted from bot_engine.cpp by tools/botnavsim/module_promote.py.
// `inline` (not `static`) so every engine TU shares ONE instance — a `static`
// here would compile and then silently fork the state per module.
// --------------------------------------------------------------------------
// Status tracking timestamps (outside BotState to avoid ABI change)
inline std::unordered_map<uint32_t, int64_t> s_idleStartTime;     // guid → when bot entered IDLE

// s_travelStartTime already defined at line ~755
inline std::unordered_map<uint32_t, int64_t> s_partyStartTime;    // guid → when bot joined party

class BotEngine : public IBotEngine {
public:
	// Declared FIRST because zReachCache_ below uses it, and a member's TYPE must already be
	// known at its point of declaration. Public because bot_zgraph.cpp's anonymous-namespace
	// LocalReach shares the engine's flood cache by pointer and must name this type.
	using ZReachSet = std::shared_ptr<const std::unordered_set<uint64_t>>;

private:

	// NPC approach-tile claims (Phase 8). Members so every module sees the same
	// reservations — a per-TU copy would let two modules hand out the same tile.
	std::unordered_map<uint64_t, ApproachReservation> s_approachReservations;
	uint32_t s_approachOverflow = 0; // times every proper tile was taken (queueing at a counter)

	// Route/graph state shared by the hunt + nav modules (Phase 11: members, not
	// file-scope statics — a per-TU copy would fork this state between modules).
	uint32_t s_graphHopSum = 0;
	uint32_t s_graphMultiHopCount = 0;
	// BOT_ROUTE_SPLICE telemetry. Class members, deliberately: a file-scope `static` in this
	// header gives every TU its own copy, so bot_data.cpp would increment one and bot_tick.cpp's
	// periodic block would read another and report zero forever.
	uint32_t s_routeSpliceRoutes = 0;
	uint32_t s_routeSpliceWpsSaved = 0;
	// key "<townId>|<src>[>mid]>dst" -> spliced waypoints. Only routes that CHANGED are stored.
	std::unordered_map<std::string, std::vector<Waypoint>> routeSpliceCache_;
	// Same keys, for routes evaluated and found to need no splice. Distinguishes "nothing to do"
	// from "not evaluated yet", which matters for lazily-evaluated multi-hop chains: without it an
	// unspliceable chain would re-run its gates on every single route start.
	std::unordered_set<std::string> routeSpliceClean_;
	// BOT_TRAVEL_ARRIVE_MIX: townId -> what a landing bot may be sent to. Rebuilt on every load.
	struct TravelArriveTargets {
		std::vector<std::string> shops;   // ammo, bank, food, loot*, potions, runes, tools, trainer(s)
		std::vector<std::string> others;  // gates, exits, quarters, towers, bridge, …
	};
	std::unordered_map<uint32_t, TravelArriveTargets> travelArriveTargets_;
	// Realised arrival-class split, for the periodic telemetry. Class members, not file-scope
	// statics: a static in this shared header forks per TU, so bot_travel.cpp would increment one
	// copy and bot_tick.cpp's telemetry block would read another and report zero forever.
	uint32_t s_arriveDepotCount = 0;
	uint32_t s_arriveTempleCount = 0;
	uint32_t s_arriveShopCount = 0;
	uint32_t s_arriveOtherCount = 0;
	uint32_t s_arriveFallbackCount = 0; // rolled non-depot, but nothing resolved -> fell back
	std::unordered_map<uint32_t, std::string> s_lastRouteSource;
	std::unordered_map<uint32_t, std::pair<size_t, int64_t>> s_routeProgress;
	std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>> s_routeWpTimer;
	// Post-action waypoint hold: guid -> time the bot may resume walking. Written by
	// followWaypoints (bot_waypoint.cpp) AND by handleActionWaypoint (bot_tick.cpp) — which is
	// why it is a member and not a function-local static: NPC_INTERACT's greet-and-wait is set
	// from the latter and read by the former.
	std::unordered_map<uint32_t, int64_t> s_actionWpPauseUntil;

	// NPC_INTERACT approach clock: guid -> {packed waypoint position, first-attempt time}.
	//
	// Keyed by POSITION, not by waypoint index, and deliberately NOT reusing s_routeWpTimer.
	// That map is keyed by guid alone and only resets when its stored index differs from the
	// current one — but bot.huntWaypointIdx is reset to 0 by every beginHuntPhase and the map is
	// never erased on a phase transition (only on a new city route, a global-timeout abort, or
	// route completion). So a bot interrupted at index 0 leaves a stale {0, oldTimestamp} that
	// the next phase starting at index 0 silently inherits. Reading elapsed time from it would
	// let the grace below fire on the very first tick, greeting without ever walking — the exact
	// inversion of what it exists to prevent.
	std::unordered_map<uint32_t, std::pair<uint64_t, int64_t>> s_npcApproachStart;

	// ---- TRUE MULTI-FLOOR: portal graph + z-route planner state ----
	// Members (not file-scope statics in the shared header) so all engine TUs
	// share ONE instance — the Phase-11 rule.
	botnav::ZPortalGraph zGraph_; // whole-map portal graph, built in loadHuntData
	bool zGraphReady_ = false;
	// Per-floor connected-component id for every portal ANCHOR (pos and landing), keyed by
	// botTileKey. Built alongside zGraph_ by flood-filling one floor at a time and projecting
	// the result down to just the anchors on that floor — the per-tile label map is transient
	// and freed before the next floor, so peak memory is one floor's walkable cells rather than
	// the whole map (~18M cells in a sparse coordinate space would need a hash map, ~700MB-1GB
	// resident, which is exactly the RAM the non-materializing sector sweep exists to avoid).
	//
	// This is what makes portal SELECTION correct. With a Chebyshev-only leg model the planner
	// happily picked stairs 7 tiles away whose landing was 65 tiles from the target in a
	// DISCONNECTED region of that floor, over the ladder 48 tiles away that actually connects
	// (proven live: Thais temple -> boat/Captain Bluebear).
	//
	// Component ids are LOCAL to a floor and must never be compared across z — legCost only
	// ever compares same-z positions, which planZRoute guarantees.
	std::unordered_map<uint64_t, uint32_t> zPortalComponent_;
	// Tile keys of every FORCE-PLACED portal landing (see ZPortal::landingForced). These were
	// never validated walkable, so the planner must test the exact tile rather than let
	// zLabelOf/LocalReach fall back to an 8-neighbour lookup — that fallback is correct for a
	// portal's SOURCE tile (a stair never floods through itself; you stand beside it) but for an
	// unvalidated landing it would borrow a neighbour's verdict and call a wall reachable.
	std::unordered_set<uint64_t> zForcedLandings_;

	// ---- Shared reachability-flood cache ----
	// The ~30ms cost of a cross-floor plan is almost entirely the END-leg flood (measured:
	// startFlood=16132 cells from the Thais temple, targetFlood=188 from an enclosed room). A
	// flood is a pure function of (origin tile, radius) and the map — it does NOT depend on which
	// bot asked, nor on that bot's portal blacklist — so it is safe to share across every bot.
	// One bot paying 30ms to leave the temple means the next 200 bots leaving the same temple pay
	// nothing.
	//
	// Deliberately caching the FLOOD rather than the finished route: a route also depends on the
	// per-bot exclusions (blacklist, just-used-portal guard), so a shared route cache would hand
	// one bot a plan that is invalid for another. The flood has no such coupling.
	std::unordered_map<uint64_t, std::pair<ZReachSet, int64_t>> zReachCache_; // originKey -> {set, expiry}
	uint64_t zReachHits_ = 0, zReachMisses_ = 0;
	std::unordered_map<uint64_t, int64_t> s_zPortalBlacklist; // botTileKey(portal pos) -> expiry (OTSYS_TIME ms)
	// guid -> {portal tile key, expiry}: the portal this bot most recently traversed. The same-z
	// graph fallback must not immediately re-take it, or the bot ping-pongs across one staircase
	// (observed: z7->z6->z7->z6 on (32361,32243) until the stale-target timeout fired). Per-bot
	// and short-lived, so it never quarantines a portal for anyone else.
	std::unordered_map<uint32_t, std::pair<uint64_t, int64_t>> s_zLastPortalUsed;
	std::unordered_map<uint32_t, ZPlannedHop> s_plannedFc; // guid -> hop the FC machine should execute
	// 5-min summary counters ([ZROUTE] line)
	uint32_t s_zPlanOk = 0, s_zPlanFail = 0, s_zHopOk = 0, s_zHopFail = 0, s_zLegWalks = 0;
	// One-shot planner tracing: when set, zPlanFullRoute's legCost logs every leg it prices with
	// the branch that decided it ([ZLEGCOST]). Diagnostic only — set by `/cavebot zplan ... -v`
	// around a single call, never left on (it is O(legs) log lines per plan).
	bool zPlanTrace_ = false;

	// Teleport-fallback KPI (Phase 8 increment 3). Member, not a file-scope static — see the
	// BOT_TELEPORT macro comment above this class.
	std::unordered_map<std::string, uint32_t> teleportSites_;
	inline void botCountTeleport(const char* site) {
		teleportSites_[site]++;
	}

	// Debug overrides for the Adventurer's-Stone trip (set via /cavebot advstone ...).
	// Members, not file-scope statics: as statics in a shared header each engine TU
	// would get its own copy and the override would not be seen by the module that reads it.
	uint8_t s_forceAdvStoneNextMode = 0;
	uint16_t s_forceAdvStoneNextWeapon = 0;

	// ---- Phase 11 module split: state shared across engine translation units ----
	// These were file-scope statics. Once the engine spans several TUs a `static` would give each
	// TU its OWN copy, silently forking the state; as members they are shared automatically through
	// this header, and since this header is NOT the ABI boundary (bot_engine_interface.hpp is),
	// adding members here is still not an ABI change.
	// PERF STRESS HARNESS: continuous tick telemetry for the whole measurement window.
	// A BotEngine member (not a file-scope inline) so a /cavebot reload builds a fresh engine
	// and therefore a fresh window -- no stale counters can survive a reload and silently
	// contaminate the run that follows.
	BotPerfStats botPerf_;

	// Clear every probe flag and unpin. Idempotent, and called from three places on purpose:
	// the harness `finally` block, engine construction, and /cavebot reload -- a crashed run must
	// never be able to leave the live world with permanent fake observers holding bots awake.
	//
	// Iterates bots_ rather than g_game().getPlayers() because a hibernated bot is NOT in the
	// online player map; its Player is held by hibernationPool_ and reached through the BotState
	// weak_ptr. Falling back to the pool directly covers the case where playerRef was reset.
	size_t clearAllProbeBots() {
		const size_t n = s_probeBots.size();
		for (uint32_t guid : s_probeBots) {
			std::shared_ptr<Player> pl;
			if (auto it = guidToIndex_.find(guid); it != guidToIndex_.end()) {
				pl = bots_[it->second].getPlayer();
			}
			if (!pl) {
				if (auto pit = hibernationPool_.find(guid); pit != hibernationPool_.end()) {
					pl = pit->second;
				}
			}
			if (pl) {
				pl->setSyntheticCastViewers(0);
			}
			s_debugPinned.erase(guid);
		}
		s_probeBots.clear();
		return n;
	}

	BotCacheStat botCacheStats_[static_cast<size_t>(BotCacheId::COUNT)] {};
	inline void botCacheHit(BotCacheId id) {
		botCacheStats_[static_cast<size_t>(id)].hits++;
	}
	inline void botCacheMiss(BotCacheId id, uint64_t us) {
		auto& st = botCacheStats_[static_cast<size_t>(id)];
		st.misses++;
		st.totalUs += us;
		if (us > st.worstUs) {
			st.worstUs = us;
		}
	}
	// guid -> {pauseStartMs, pauseDurationMs} for the observed mid-walk pause.
	std::unordered_map<uint32_t, std::pair<int64_t, int64_t>> walkPauseInfo_;

	// guid -> mount roll for this session. `wants` is the botMountChancePct coin flip,
	// `mountId` the model drawn from the bot's owned catalog; both are re-drawn only on a
	// genuinely new session (see MOUNT_REROLL_MIN_INTERVAL_MS). Engine-internal on purpose:
	// BotState lives in bot_engine_interface.hpp (the ABI boundary), so a field there would
	// force a full rebuild — a BotEngine member does not.
	struct MountRoll {
		bool wants = false;
		uint8_t mountId = 0;
		int64_t lastRollMs = 0;
	};
	std::unordered_map<uint32_t, MountRoll> botMountWants_;
public:
	BotEngine() = default;
	// The destructor is the ONE point every teardown passes through: BotEngineLoader::reload()
	// and ::unload() both funnel into destroyBotEngine() -> `delete engine`. Draining here means
	// no reload path — talkaction, bot_commands queue, debug,N, debug off — can leave a scheduled
	// .so lambda alive to fire after dlclose. Doing it in a specific deactivate function is what
	// failed before: the correct loop existed in deactivateAll(), which nothing ever called.
	~BotEngine() override {
		cancelPendingDispatcherEvents();
	}

	// Public because bot_zgraph.cpp's anonymous-namespace flood-fill predicate needs it, and a
	// free function cannot reach a private member. Safe to expose: a pure static predicate over
	// item IDs with no state.
	static bool isKeyLockedDoorId(uint16_t id); // bots carry no keys — these are permanent walls
	// Public for the same reason: bot_zgraph.cpp's door-bridge search is an anonymous-namespace
	// free function and needs the closedId -> openId table to recognise a door.
	static const std::unordered_map<uint16_t, uint16_t>& getDoorTable();


	// IBotEngine overrides
	void registerBot(const std::shared_ptr<Player>& player) override;
	void unregisterBot(uint32_t guid) override;
	bool activateBot(uint32_t guid) override;
	bool deactivateBot(uint32_t guid) override;
	void forceDeactivateBot(uint32_t guid) override;
	void forceDeactivateBotForReload(uint32_t guid) override;
	bool reactivateBotForReload(uint32_t guid) override;
	void deactivateAll() override;
	// Cancels every dispatcher event this engine owns. Called from ~BotEngine (the unconditional
	// teardown funnel) and from deactivateAll. See the definition for the rule new scheduling
	// call sites must follow.
	void cancelPendingDispatcherEvents();
	void pauseBotForDeath(uint32_t guid) override;
	void tick() override;
	uint32_t countActiveBots() const override;
	uint32_t countTotalBots() const override;
	BotState* getBotState(uint32_t guid) override;
	std::string getStatusText(uint32_t guid) override;
	void loadHuntData() override;
	std::string executeCommand(const std::string& botName, const std::string& command) override;

	void saveAllStates() override;
	void restoreAllStates() override;
	void clearPersistedStates() override;

	void setBotAIPaused(uint32_t guid, bool paused) override;
	void setAllBotsAIPaused(bool paused) override;

	// Hibernation
	bool hibernateBot(uint32_t guid) override;
	bool wakeBot(uint32_t guid) override;
	std::vector<uint32_t> getHibernatedBotGuids() const override;
	uint32_t getHibernatedBotGuidByName(const std::string &name) const override;
	std::shared_ptr<Player> getHibernatedBotPlayer(uint32_t guid) const override {
		auto it = hibernationPool_.find(guid);
		return (it != hibernationPool_.end()) ? it->second : nullptr;
	}
	void onPlayerSayNearBots(uint32_t playerId, const Position& pos, const std::string& text) override;
	void onPlayerPmToBot(uint32_t botGuid, uint32_t playerId, const std::string& text) override;
	uint32_t hibernateAllEligibleBots() override;
	uint32_t wakeAllHibernatedBots() override;
	uint32_t wakeBotsInRadius(const Position& pos, int radius) override;
	bool recoverOrphanForReload(uint32_t guid, const std::shared_ptr<Player>& player) override;

	// Liveness diagnostics (2026-06-09). buildProximityReport: per-anchor inventory of
	// bots in each density ring + wake outcomes in last 60s + state distribution.
	// buildPopulationReport: per-town bot count by state. broadcastAdminLog: log to
	// system journal + sendTextMessage to all isAccessPlayer() (GMs/GODs).
	std::string buildProximityReport();
	std::string buildPopulationReport() const;
	// BOT_NAV_REALISM Phase 1: distribution of active bots across the 30 tick-phase buckets.
	// Should be roughly uniform once guid-phasing is in effect (acceptance signal).
	std::string buildTickPhaseHistogram() const;
	void broadcastAdminLog(const std::string& multiline);
	std::vector<std::string> getActiveBotNames() const override;
	std::vector<uint32_t> getActiveBotGuids() const override;
	size_t getHibernatedBotCount() const override;

private:
	// Materialize Player from DB and place in world (shared by activateBot reload-from-offline
	// and wakeBot). Returns shared_ptr<Player> on success, nullptr on failure.
	std::shared_ptr<Player> materializePlayerFromDb(BotState &bot);

	// v2 virtual simulator (BOT_HIBERNATION_V2.md): advances hibernated bots' state
	// machine + waypoint index in pure C++ memory (no engine ops).
	// Phase 6: index-based progression replaces the prior distance-interpolation model.
	// PERF_INVESTIGATION_2026-05-24 Phase A (2026-05-31): called every BotEngine::tick
	// with a wall-clock budget computed from load EWMA; rolling index across bots_ so
	// each hibernated bot is touched ~every 20s smeared across 200 ticks instead of
	// bursting once per 5s. budget_ms=0 means defer entirely this tick.
	size_t advanceWaypointIdx(uint32_t guid, size_t currentIdx, size_t totalSize, int64_t elapsed_ms);
	void virtualTick(int64_t budget_ms);
	void virtualAdvanceIdle(BotState &bot, int64_t elapsed_ms);
	void virtualAdvanceDwelling(BotState &bot, int64_t elapsed_ms);
	void virtualAdvanceTraveling(BotState &bot, int64_t elapsed_ms);
	void maybeQueueVirtualPositionSave(BotState &bot);
	// Shared status for virtualTickCityRoute. NotActive = no route loaded;
	// StillWalking = advanced one index, currentPos updated; JustCompleted = route exhausted
	// this tick, cleared state, caller MUST do its own phase transition before returning.
	enum class VirtualRouteStatus { NotActive, StillWalking, JustCompleted };
	VirtualRouteStatus virtualTickCityRoute(BotState &bot, int64_t elapsed_ms);
	// Phase 2/3 — declared now for forward references; implemented later
	void virtualAdvanceHunting(BotState &bot, int64_t elapsed_ms);
	void virtualAdvancePartyHunt(BotState &bot, int64_t elapsed_ms);
	void virtualAdvanceAdvStone(BotState &bot, int64_t elapsed_ms);
	bool virtualTryStartHunt(BotState &bot);
	bool virtualTryStartPartyHunt(BotState &bot, int32_t forceScriptId = 0);
	void dissolveVirtualPartyHunt(uint32_t partyHuntId, const std::string& reason);
	void materializeCanaryParty(uint32_t partyHuntId);

private:
	// AI state handlers (Phase 1)
	void processBot(BotState& bot);
	void doIdle(BotState& bot);
	void doDwelling(BotState& bot);

	// Journal line emitted whenever a bot reserves a NEW hunt script. Skipped on party
	// state restore (already logged at original assignment) and on DB restore at startup
	// (rehydrating saved state, not a new reservation). Format documented in cpp.
	void logHuntAssign(const BotState& bot, uint32_t scriptId) const;

	// Combat handlers (Phase 3)
	void doSelfDefense(BotState& bot);
	void doCombat(BotState& bot);
	void doFleeing(BotState& bot);
	void doPKAttack(BotState& bot);
	void chaseTarget(BotState& bot, const std::shared_ptr<Creature>& target);

	// Spectator cache (Gesior b_possible_targets pattern, 2026-05-27).
	// Refreshes bot.cachedPlayerIds + bot.cachedMonsterIds at most every 600ms (3 ticks).
	// Used for TARGET SELECTION sites; AOE damage paths bypass and call find() fresh.
	static constexpr int64_t SPECTATOR_CACHE_TTL_MS = 600;
	void refreshSpectatorCacheIfStale(BotState& bot);
	void castSpell(BotState& bot, const std::shared_ptr<Creature>& target);
	const ResolvedSpell* selectAttackSpell(BotState& bot, const std::shared_ptr<Creature>& target,
		double* outBestScore = nullptr);
	void doHealing(BotState& bot);

	// AoE spell system
	// Dynamic spell tables (built at init from server registry + Lua files)
	void buildSpellTables();
	std::vector<ResolvedSpell> resolvedSingleSpells_[5]; // index by baseVoc 1-4
	std::vector<ResolvedSpell> resolvedAoeSpells_[5];
	std::vector<ResolvedRune> resolvedAoeRunes_;
	ResolvedRune resolvedSdRune_;
	static bool isInAoeArea(const Position& botPos, const Position& targetPos,
		AoeAreaType areaType, Direction dir, int32_t areaSize, int32_t innerSize = 0);
	// Matrix-driven hit predicate using parsed Lua arrays. Picks cardinal vs diagonal matrix
	// based on dir. For needDirection=true, the {3}/{2} anchor maps to caster+1 forward; for
	// non-directional self-area, it maps to the caster. Falls back to false if no matrix.
	static bool isInAreaMatrix(const Position& casterPos, const Position& targetPos,
		const AreaMatrix* cardinal, const AreaMatrix* diagonal,
		Direction dir, bool needDirection);
	// Helper that prefers matrix lookup when bound on the spell, else falls back to
	// the legacy enum predicate. Use this in place of raw isInAoeArea at call sites
	// that have a ResolvedSpell* in scope.
	static bool spellHits(const Position& casterPos, const Position& targetPos,
		const ResolvedSpell& spell, Direction dir);
	static bool runeHits(const Position& casterPos, const Position& targetPos,
		const ResolvedRune& rune, Direction dir);
	Direction findBestWaveDirection(BotState& bot, const ResolvedSpell* spell,
		const std::vector<std::shared_ptr<Creature>>& nearby, int32_t& outCount,
		double* outWeightedCount = nullptr);
	int32_t countAoeTargets(BotState& bot, const ResolvedSpell* spell, Direction dir,
		std::vector<std::shared_ptr<Creature>>& outTargets, double* outWeightedCount = nullptr);
	const ResolvedSpell* selectAoeSpell(BotState& bot, Direction& outBestDir,
		int32_t* outBestCount = nullptr, double* outBestScore = nullptr,
		const std::shared_ptr<Creature>& pvpTarget = nullptr);
	void castAoeSpell(BotState& bot, const ResolvedSpell* spell, Direction bestDir);

	// Rune system — uses real server RuneSpell::executeUse() path
	std::shared_ptr<Item> findRuneInBackpack(const std::shared_ptr<Player>& player, uint16_t runeItemId);
	std::tuple<Position, int32_t, double> findBestRunePosition(BotState& bot, CombatType_t combatType);
	bool executeRune(BotState& bot, const std::shared_ptr<Item>& runeItem,
		const Position& targetPos, const std::shared_ptr<Creature>& targetCreature);

	// Unified damage estimation
	double estimateDamage(int32_t level, int32_t mlevel, double avgMlevelCoef, double avgConstant);

	void exitCombat(BotState& bot);
	void exitPK(BotState& bot);
	void checkVigilante(BotState& bot);
	void checkRandomPK(BotState& bot);

	// Hunting handlers (Phase 4)
	void doHunting(BotState& bot);
	bool tryStartHunt(BotState& bot);
	bool tryStartCityWalk(BotState& bot);
	// preChosenPatrolIdx: PATROLLING entry index chosen by the caller. Defaulted, so every
	// existing call site keeps today's behaviour (a fresh botPatrolEntryIdx roll). The four
	// teleport sites in doHuntTravel pass the index they actually placed the bot on —
	// without that the bot lands on one waypoint and is told to walk to another.
	void beginHuntPhase(BotState& bot, HuntPhase phase, size_t preChosenPatrolIdx = kNoPatrolIdx);
	void doHuntPrepare(BotState& bot);
	void doHuntTravel(BotState& bot);
	void doHuntLeaving(BotState& bot);
	void doHuntPatrol(BotState& bot);
	void doHuntResupply(BotState& bot);
	bool advanceHuntWaypoint(BotState& bot);
	bool navigateToHuntWaypoint(BotState& bot);
	void scanAndAttackMonster(BotState& bot);
	bool hasNearbyReachableTargets(BotState& bot);

	// --- BOT_LURE_KITE ---------------------------------------------------------
	// Pack size this bot should gather before engaging. Script column first, party
	// fallback second, 0 = not armed. Honours the `/cavebot <bot> lure <n>` override.
	uint8_t effectiveMinMonsters(const BotState& bot, const HuntScript* script) const;
	// Full arming test, re-evaluated EVERY tick (see the gate contract in the state
	// block): a dissolved party or a hibernation-time script reroll must disarm on the
	// same tick, not whenever some cleanup site happens to run.
	bool lureEligible(const BotState& bot, const HuntScript* script,
		const std::shared_ptr<Player>& player) const;
	// One pass over the spectator cache that answers three questions at once: how many
	// monsters are aggroed on ME, how many on a PARTY SUPPORT, and how far the tail of
	// my own pack has drifted. Aggro test is getAttackedCreature(), the same signal
	// botIsQuestRetaliationTarget uses.
	uint8_t censusLuredPack(BotState& bot, const HuntScript* script, int32_t& outMaxDist,
		int32_t& outNearestDist, uint8_t& outSupportAggro, bool& outSupportContact);
	LureVerdict tickLure(BotState& bot, const HuntScript* script);
	void forceLureEngage(BotState& bot, uint8_t trigger, const char* reason);
	// Alive + aggroed on me or on a party support. Drives the hunt-end lure hold, which
	// must not release into LEAVING while the pack is still on the healer.
	uint8_t lurePackAliveAggroed(BotState& bot, const HuntScript* script);

	// Cornered / drift-capped keep-distance kiting along already-walked waypoints.
	// Returns true if it owns the tick's movement (caller must not walk).
	bool tryKiteBacktrack(BotState& bot, const Position& threatPos, int32_t keepDist);
	// Shared exit: resumes the patrol at the waypoint the bot is standing on.
	void endKiteBacktrack(BotState& bot, bool gaveUp, const char* reason);
	// Blocked in the current direction (no path, or the burst would dive into the pack):
	// reverse if the other end of the window improves distance to the threat, else give up.
	// Returns true if the run continues.
	bool kiteReverseOrGiveUp(BotState& bot, KiteRun& run, const std::vector<Waypoint>& wps,
		const Position& threatPos);
	void endHunt(BotState& bot);
	void abortHunt(BotState& bot, const std::string& reason);
	void finishResupplyAndReroll(BotState& bot);
	bool findNearestRecoveryRoute(BotState& bot);

	// Player spawn-claim helpers (see playerClaims_)
	bool isScriptPlayerClaimed(uint32_t scriptId, const std::string& spawnGroup);
	const HuntScript* detectClaimableScript(const Position& pos, int32_t& bestDist, const HuntScript*& runnerUp, int32_t& runnerUpDist);
	bool kickSpawnHolder(const HuntScript& script, const std::string& reason, std::string& kickedName);

	// Travel handlers (Phase 5)
	void doTraveling(BotState& bot);
	void startTravel(BotState& bot, uint32_t destTownId);

	// Floor change (Phase 2)
	void startFloorChange(BotState& bot, bool goDown, const Position& targetPos);
	void handleFloorChange(BotState& bot);
	std::vector<ZTransition> findZTransitions(const Position& center, int32_t radius, bool goDown);
	Position computeFloorChangeDest(const Position& fcPos, bool goDown);
	void resetFloorChange(BotState& bot);
	// Returns true = give-up handled (caller must NOT advance fcTransIdx); false = legacy path.
	// `site` tags the [ZBLACKLIST] entry written on the give-up branch, so callers routed through
	// this shared gate keep their own identity in the journal.
	bool fcGiveUpOnPlannedTrans(BotState& bot, const ZTransition& trans, uint8_t maxRetries,
	                            const char* site = "planned_unreachable");
	// Walk as close as the map allows (prefers 0, degrades to maxDist). See definition.
	bool goToClosest(BotState& bot, const Position& target, int32_t maxDist = 3,
	                 WaypointType wpType = WaypointType::NODE);

	// ---- Scoped route planner (bot_nav.cpp) ----
	//
	// The expensive "plan a whole journey" navigation, deliberately kept OFF the per-tick movement
	// path that every bot walks. goTo() is called by ~500 bots per tick, so journey planning there
	// cost a measured 5733ms single-bot dispatcher stall; here it has exactly three callers, all
	// rare by construction — the `/cavebot <bot> goto x,y,z` admin command, the NPC visit, and the
	// fishing trip (the latter two are one weighted choice per reroll interval, and both ship
	// disabled at botNpcVisitPct/botFishPct = 0).
	// That rarity is what replaces rate limiting; do not widen the caller set without replacing it.
	// Named planNpcVisitWalk until it grew past one consumer.
	bool planScopedWalk(BotState& bot, const Position& target, int32_t maxDist = 3);
	// Leg-lifecycle helpers for planScopedWalk. See their definitions in bot_nav.cpp.
	void logPlannerLegSpent(BotState& bot, size_t idxBefore, size_t size, const char* prefix);
	bool plannerLegRebuildExhausted(BotState& bot, const Position& target);
	// Watchdog for the one signal that hid this whole class of bug: nav reports "started" while
	// nothing is queued and the bot does not move. Keyed by guid; {since, lastWarn, at}.
	struct NavStartedIdle { int64_t since = 0; int64_t lastWarn = 0; Position at; };
	std::unordered_map<uint32_t, NavStartedIdle> s_navStartedButIdle;
	void noteNavStartedButIdle(BotState& bot);

	// guid -> the walkTarget the planner owns. SELF-INVALIDATING by design: the consumption site
	// honours the entry only while it still equals bot.walkTarget, so any other subsystem that
	// retargets the bot (bot_hunt.cpp's depot-recovery walk sets walkTarget too) silently drops
	// the planner claim without having to know this map exists. A bare set keyed on guid would
	// have to be cleared at ~40 `hasWalkTarget = false` sites across 8 TUs, and one missed site
	// would hand an unrelated walk to the expensive path.
	std::unordered_map<uint32_t, Position> s_plannerWalk;
	// The hop tier 3 is currently executing, kept SEPARATE from s_plannedFc (which the FC state
	// machine owns) plus the target it was derived for, so a retarget re-plans instead of walking
	// to a stale portal.
	struct PlannerHop {
		ZPlannedHop hop;
		Position forTarget {};
	};
	std::unordered_map<uint32_t, PlannerHop> s_plannedHop;
	bool isPlannerWalk(const BotState& bot) const;
	void clearPlannerWalk(uint32_t guid);

	// ---- Same-floor leg: the route expressed as [DOOR, DOOR, …] + destination ----
	// The same shape ensureZLegPlan builds for a portal approach, applied to an ordinary
	// same-floor walk. Each door is a leg boundary because a closed door IS a wall to A* — you
	// cannot compute one path through it — so the route must be segmented there. Unlike chunking,
	// the boundaries sit at real obstacles, and followWaypoints opens each door on arrival.
	struct PlannerLeg {
		std::vector<Waypoint> wps;
		size_t idx = 0;
		uint32_t skipCount = 0;
		Position forTarget {};
		uint8_t z = 0;
	};
	std::unordered_map<uint32_t, PlannerLeg> s_plannerLeg;
	// How many times the planner has rebuilt a leg for the CURRENT target, and which target that
	// is. A leg that exhausts by skipping every waypoint gets re-derived, and plannerLegDoors is
	// deterministic over unchanged geometry — so it finds the identical door and fails identically,
	// forever. Before this, the only thing that ended the cycle was the unrelated 240s stale-target
	// guard, i.e. it was bounded by coincidence rather than by design. Past the cap the planner
	// returns false instead of re-deriving, which hands the caller's own pathFailCount give-up a
	// fast, diagnosable exit.
	struct PlannerLegRebuilds {
		Position forTarget;
		uint32_t count = 0;
	};
	std::unordered_map<uint32_t, PlannerLegRebuilds> s_plannerLegRebuilds;
	static constexpr uint32_t PLANNER_LEG_MAX_REBUILDS = 3;

	// Blocked-walk detector. A queued autowalk step that fails is NOT dropped by the server:
	// creature.cpp re-issues the same blocked step indefinitely (it only sends the player a
	// cancel message, which a bot has no client to receive). So "walk queue is not draining while
	// the bot has not moved" is the only reliable signal, and it is strictly better than a timer:
	// a bot merely shoved aside by another creature still drains its queue.
	struct PlannerStuck {
		Position lastPos {};
		size_t lastQueue = 0;
		int64_t sinceMs = 0;
	};
	std::unordered_map<uint32_t, PlannerStuck> s_plannerStuck;
	// Wall-clock, not a tick count: the per-bot processing rate varies with load and the awake
	// budget, so N ticks is not a fixed duration. 1s is long enough to ride out a passing
	// creature and short enough to feel responsive.
	static constexpr int64_t PLANNER_STUCK_MS = 1000;
	bool plannerWalkBlocked(BotState& bot, const std::shared_ptr<Player>& player);
	// Drop cached routing state and cancel the in-flight walk, KEEPING the planner claim.
	void plannerReplan(BotState& bot, const std::shared_ptr<Player>& player);

	// ---- Shared door-list cache ----
	// Cached per LEG and shared across bots, so the second bot to walk between two points reuses
	// the first one's door discovery. Caching the DOOR LIST rather than the tile path is
	// deliberate, and mirrors why zReachCache_ caches floods rather than routes: a door list is a
	// property of the static map plus door states and stays valid for minutes, whereas a tile
	// path is invalidated constantly by creatures moving, so a shared path cache would hand out
	// stale routes. Reuse is highest on portal→portal legs, which are literally the same two
	// tiles for every bot; start/end legs have bot-specific origins and mostly miss.
	struct DoorPathEntry {
		Position from {}, to {};
		std::vector<Position> doors;
		bool reachable = false;
		int64_t expiry = 0;
	};
	std::unordered_map<uint64_t, DoorPathEntry> s_doorPathCache;
	uint64_t s_doorPathHits = 0, s_doorPathMisses = 0;
	static constexpr int64_t DOOR_PATH_TTL_MS = 60000;
	static constexpr size_t DOOR_PATH_CACHE_MAX = 512;
	// Planner legs reach much further than the 24-tile door-bridge search, so they get their own
	// bounds sized to PATH_WIDE_DIST rather than reusing Z_DOOR_BRIDGE_*.
	static constexpr int32_t PLANNER_LEG_RADIUS = 200;
	static constexpr uint32_t PLANNER_LEG_BUDGET = 20000;
	// Door-permissive reachability + the doors on the route, in walking order. Returns false when
	// the target cannot be reached on this floor even treating openable doors as open — which is
	// exactly the "this same-z target still needs floor hops" signal.
	// `bypassCache` forces a fresh BFS and overwrites the entry. Required after a stall: the walk
	// failed against reality, so re-deriving from a cached answer computed BEFORE the failure
	// returns the identical bad route and the bot loops against it until the 60s TTL expires.
	bool plannerLegDoors(const Position& from, const Position& target, std::vector<Position>& outDoors,
	                     bool bypassCache = false);
	// Bots whose next plannerLegDoors call must ignore the cache (set by plannerReplan, consumed
	// once by planScopedWalk).
	std::unordered_set<uint32_t> s_plannerForceFresh;
	// First closed-but-openable door standing between `from` and `target`, in walking order.
	bool zFindBlockingDoor(const Position& from, const Position& target, Position& outDoor);
	// Every openable door ON the route, in walking order (empty when the way is clear).
	bool zFindDoorsOnPath(const Position& from, const Position& target, std::vector<Position>& outDoors);

	// ---- Synthetic z-leg: the current hop expressed as ordinary WAYPOINTS ----
	//
	// A planned portal hop is walked as [DOOR, DOOR, ...] + one terminal portal waypoint, then
	// handed to the SAME followWaypoints machinery that walks authored hunt and city routes.
	// That machinery already knows how to open a DOOR on arrival, use a LADDER/ROPE, step onto a
	// HOLE/STAIRS, fire a TELEPORT, and use_with a shovel or sewer grate — so multi-floor
	// execution stops being a parallel implementation and becomes a caller of the existing one.
	//
	// Every ZPortalKind maps onto an existing WaypointType, so this needs no ABI change.
	struct ZLegPlan {
		std::vector<Waypoint> wps;
		size_t idx = 0;
		uint32_t skipCount = 0;
		Position forPortalPos {}; // staleness key: the hop this leg was built for
	};
	std::unordered_map<uint32_t, ZLegPlan> s_zLegPlan;
	ZLegPlan& ensureZLegPlan(BotState& bot, const Position& from, const botnav::ZPortal& hop);
	// Last-resort leg tier: walk to a blocking door and open it. See definition in bot_nav.cpp.
	bool tryBridgeDoorLeg(BotState& bot, const std::shared_ptr<Player>& player, const Position& target);
	bool tryUseLadder(BotState& bot, const Position& ladderPos, bool goDown);
	void castLevitateSpell(BotState& bot, const Waypoint& waypoint);
	// Returns true iff an extra_data marker (`fish:` / `tool:`) fired. The caller at
	// bot_waypoint.cpp:218 MUST take the action pause on true: the arrival branch does not return
	// for NODE/STAND waypoints, it `continue`s and navigates to the next waypoint on the same
	// tick, which would walk the bot away from the tile it just started working.
	bool handleActionWaypoint(BotState& bot, const Waypoint& waypoint);
	// Parses `<prefix><itemId>` / `<prefix><itemId>@<dx>,<dy>` (tool:) and `<prefix><dx>,<dy>`
	// (fish:). Offsets default to 0,0. Returns false on a malformed payload.
	static bool parseOffsetMarker(const std::string& extra, const std::string& prefix,
	                              int32_t& dx, int32_t& dy, uint16_t& itemId);

	// ============================================================================
	// BOT_SUPPLY_REALISM (bot_supply.cpp) — potions, food, rune crafting, fishing
	// ============================================================================
	//
	// All per-bot and independent: there is NO cap on how many bots do any of these at once
	// (unlike hunting, where activeHunts_ reserves a script to one bot). Nothing here reserves
	// anything, and fishing deliberately holds no approach-tile claim — two bots aiming at the
	// same shore are separated by the same thing that separates them anywhere else, A* treating
	// an occupied tile as blocked.

	// Drink/eat. Called from the OUTER per-bot loop next to doHealing, NOT from processBot:
	// processBot early-returns on death-pause, on every floor change, and on AdvStone trips, so
	// hooking it there would silently skip exactly the states this is supposed to cover.
	void maybeUseSupplies(BotState& bot);

	// Forceable cores behind the automatic behaviours. `force` skips the chance roll and the
	// per-bot interval floor but NOT the things that would make the action illegal (missing item,
	// spell cooldown, hunting). Each fills outMsg with a human-readable result so the /cavebot
	// commands can say exactly why nothing happened instead of failing silently.
	bool tryDrinkPotion(BotState& bot, bool force, bool preferMana, std::string* outMsg = nullptr);
	bool tryEatFood(BotState& bot, bool force, std::string* outMsg = nullptr);
	bool tryCraftRune(BotState& bot, bool force, std::string* outMsg = nullptr);
	// Sends the bot on a fishing trip right now, exactly as a winning POI reroll would.
	bool forceFishingTrip(BotState& bot, std::string& outMsg);
	// Stable storage for the BotPOI a forced trip points currentPOI at — the reroll path uses
	// bot_poi.cpp's s_dynamicPOIs slot, which is file-static there and not reachable from here.
	std::unordered_map<uint32_t, BotPOI> s_forcedFishPoi;

	// The ONE food list. It used to be duplicated — kFoods here and two hand-typed seed() calls
	// in equipBot — and the two silently disagreed: the picker rolled cheese/meat that were never
	// seeded, so half of all attempts failed. That is not just log noise, because tryEatFood
	// re-arms the 60s interval BEFORE checking inventory, which halved the real eating cadence.
	// equipBot loops over this same array, so the seed set and the pick set cannot drift again.
	// Plain foods only: no vocation or level gate in foods.lua, so any bot can eat any of them.
	static constexpr uint16_t kBotFoods[] = {
		3582,  // ham
		3602,  // brown bread
		3577,  // meat
		3607,  // cheese
	};
	// s_pendingFishSpot is declared next to FishingSpot itself (below), which this block predates.
	// Conjure the bot's signature rune/ammo. Same outer-loop slot. Returns true iff a rune was
	// actually conjured, so the caller can skip the support-spell attempt in the same slot —
	// one spell per bot per slot, never a double cast.
	//
	// SCOPE (changed 2026-08-03): rune crafting is no longer "any awake state except hunting".
	// It fires only when the bot is genuinely IDLE IN PLACE for >RUNE_IDLE_MIN_MS, or while it
	// is actively FISHING. A player conjures runes standing around town or waiting on a fishing
	// spot — not while walking somewhere. The vacated "everywhere but hunting" slot now belongs
	// to the support spells below.
	bool maybeCraftRunes(BotState& bot);

	// ---- Idle-in-place clock ----
	// Engine-local, NOT a BotState field (BotState is the ABI boundary), and deliberately NOT
	// reusing BotState::fidgetStationarySince: that one is maintained only inside maybeFidgetDrop,
	// which runs from doIdle/doDwelling only, so it goes stale the moment a bot leaves those two
	// states and cannot back a 10s threshold.
	//
	// Updated from the SAME outer-loop slot as the supply hooks — the only slot that runs for
	// every awake bot in EVERY state, which is exactly what a stationary clock needs.
	struct IdleStationary {
		int64_t since = 0;   // when the current continuous idle stop began; 0 = not idle
		Position at;         // position at the last sample — the load-bearing reset check
	};
	std::unordered_map<uint32_t, IdleStationary> s_idleStationary;
	void updateIdleStationaryClock(BotState& bot);
	bool isIdleInPlaceFor(const BotState& bot, int64_t ms) const;

	// ============================================================================
	// Ambient support spells (bot_supply.cpp) — any awake state EXCEPT hunting
	// ============================================================================
	//
	// The behaviour rune crafting used to have. Self-target, non-aggressive instants from the
	// support/healing groups: haste, light, magic shield, invisibility, regeneration, heals and
	// cures. Cast while travelling, walking a POI route, visiting an NPC, idling or fishing —
	// listWalkDir is deliberately NOT checked, because a bot keeping utani hur up on the road is
	// the whole point. Verified: a non-aggressive self-target instant never touches listWalkDir,
	// stopWalk, cancelNextWalk, stopEventWalk or setNextActionTime, so casting mid-step cannot
	// disturb the walk (the older "conjuring mid-step would cancel the walk" comment in
	// tryCraftRune was wrong on its premise).
	//
	// NO condition gating: a bot may re-cast a buff it already has, or a cure for a condition it
	// does not have. Both are harmless — Creature::addCondition refreshes a same-(type,id,subId)
	// condition in place rather than stacking it, and a dispel with nothing to dispel is a no-op.
	// Pacing therefore comes entirely from the interval + chance roll.
	struct SupportSpell {
		std::string words;
		uint32_t level = 0;
		uint32_t vocMask = 0;  // bit per base vocation id
	};
	std::vector<SupportSpell> resolvedSupport_[5];  // indexed by base vocation 1-4
	void buildSupportSpellTables();                 // walks the live registry; called from loadHuntData
	bool trySupportSpell(BotState& bot, bool force, std::string* outMsg = nullptr);
	bool maybeSupportSpell(BotState& bot);
	// `/cavebot <bot> supportlist` — the bot's pool with a per-entry reason it is ineligible now.
	std::string describeSupportPool(const BotState& bot) const;

	// One conjure spell per bot, fixed at registration. Real players conjure their preferred
	// rune repeatedly rather than cycling all nine a paladin can cast, and it keeps each bot's
	// output to a single stackable item id.
	struct ConjureSpell {
		std::string words;
		uint16_t conjureId = 0;   // what it produces (pre-seeded so it stacks)
		uint16_t reagentId = 0;   // 0 or 3147; anything else means it eats a real item
		uint32_t level = 0;
		uint32_t vocMask = 0;     // bit per base vocation id
	};
	std::vector<ConjureSpell> conjureSpells_;
	void buildConjureTables();                 // parses data/scripts/spells/conjuring/*.lua
	const ConjureSpell* signatureConjure(const BotState& bot) const;

	// Per-bot pacing. Engine-internal maps rather than BotState fields, for the same reason the
	// fishing run is: BotState is the ABI boundary.
	std::unordered_map<uint32_t, int64_t> s_nextPotionMs;
	std::unordered_map<uint32_t, int64_t> s_nextFoodMs;
	std::unordered_map<uint32_t, int64_t> s_nextRuneCraftMs;
	std::unordered_map<uint32_t, int64_t> s_nextSupportSpellMs;

	// ---- Fishing run: a 3-phase sub-activity keyed by guid ----
	// Deliberately NOT fields on BotState: that struct is the ABI boundary, so putting it there
	// would force a full rebuild for every later tweak.
	enum class FishPhase : uint8_t { TRAVEL = 0, FISHING = 1, RETURNING = 2 };
	struct FishingRun {
		FishPhase phase = FishPhase::TRAVEL;
		Position water, stand, home;
		int64_t until = 0;        // when the casting session ends
		int64_t nextCastMs = 0;
		uint32_t casts = 0;
		// NB: rune crafting keys off phase == FISHING (isAmbientFishing below), not off the run
		// merely existing — TRAVEL and RETURNING are ordinary walks and must not qualify.
		// Deadline for closing the last few tiles onto `stand`. POI arrival fires within
		// POI_ARRIVAL_DIST (3), and from three tiles short the cast is usually still legal, so
		// without this the bot simply fishes from wherever it happened to stop — which throws away
		// the whole point of choosing the closest walkable tile to the water. Bounded so an
		// occupied or newly-blocked stand tile costs a few seconds, not the session.
		int64_t approachUntil = 0;

		// ---- Self-defense against monsters (a sub-state of the FISHING phase) ----
		// A bot standing at the shore while something chews on it reads as a bug — the exact
		// complaint the ice-fishing session already answers by bailing (see tickIceFishSession).
		// Ambient fishing instead FIGHTS BACK and resumes casting, because unlike ice fishing it
		// owns a multi-minute session, a spot claim and a walk home that are all worth keeping.
		// Deliberately NOT a COMBAT state transition: the guard in processBot tears the whole run
		// down the moment the bot leaves DWELLING, so entering COMBAT would abandon the trip.
		uint32_t defendTargetId = 0;  // monster being fought right now (0 = not engaged)
		// Per-ENGAGEMENT clock, reset on every new target. Chained attackers from a pack must each
		// get a fresh abort budget, or the third one inherits a nearly-spent timer.
		int64_t defendSinceMs = 0;
		// Combat time owed back to `until`, accumulated per kill and paid out on resume, so a
		// fishing trip is not silently consumed by the fights it had to win.
		int64_t defendOwedMs = 0;
		uint32_t defendKills = 0;     // telemetry
	};
	// ========================================================================
	// BOT_AMBIENT_ROAM — bots staged just off a player's screen that wander
	// nearby reachable tiles, across floors, dwelling at each stop.
	//
	// Modelled on FishingRun directly above: a per-guid side table driven from
	// doIdle/doDwelling, NOT a new BotAIState. A roamer is an ordinary IDLE or
	// DWELLING bot to every other subsystem, which is what lets it keep normal
	// combat, chat, supply and hibernation behaviour for free.
	// ========================================================================
	enum class RoamPhase : uint8_t { WALKING, DWELLING };

	// Staging band, in tiles from the anchor. The client viewport is 8 wide by 6 tall with an
	// asymmetric +1 on the east/south edges, so with the off-screen margin the "seen" box reaches
	// 11 tiles east. 13 is the first ring that clears it on every axis, and safe placement can
	// still spiral up to 3 tiles toward the anchor — which is why the injector asks for a wider
	// margin rather than trusting this floor alone. 19 keeps the walk-in down to a few seconds.
	static constexpr int32_t ROAM_STAGE_MIN_DIST = 13;
	static constexpr int32_t ROAM_STAGE_MAX_DIST = 19;
	// How far a fight may drag the bot from where it engaged before it stops chasing. Anchored to
	// the engagement point and NOT to the leg destination: a leg dest can be twenty tiles away, so
	// leashing against it would let a chase cross the whole region, possibly straight at the player.
	static constexpr int32_t ROAM_DEFEND_LEASH = 7;
	// A position change larger than this between consecutive ticks is a teleport, not a walk.
	// Above any legitimate step (the walk scheduler moves one tile) and above the multi-step burst
	// chaseTarget can issue, so an ordinary fight never reads as displacement.
	static constexpr int32_t ROAM_DISPLACE_TILES = 10;
	// How soon to retry after a leg pick comes up empty. Short, because the usual cause is a
	// transient region-cache miss rather than a bot with nowhere to go.
	static constexpr int64_t ROAM_LEG_RETRY_MS = 1500;
	// How long a retiring roamer may keep walking while it looks for somewhere out of view. On
	// expiry it stops being ours anyway -- endRoamSession refuses to hibernate an observed bot, so
	// the worst case is a bot left awake under normal AI, never a body vanishing on screen.
	static constexpr int64_t ROAM_RETIRE_MAX_MS = 45000;
	// Flood-cache tuning. A flood result is TERRAIN, which does not change, so it is cached far
	// longer than the region that consumes it: the region expires quickly to follow a moving
	// player, then re-derives itself from floods that are still perfectly valid.
	static constexpr int64_t ROAM_FLOOD_TTL_MS = 120000;
	static constexpr size_t ROAM_FLOOD_CACHE_MAX = 512;
	// Each admitted portal costs one extra flood on a cold build.
	static constexpr uint32_t ROAM_MAX_PORTAL_FLOODS = 6;
	static constexpr size_t ROAM_REGION_CACHE_MAX = 64;

	// Why a session can end. Drives both the release policy and the [ROAM] telemetry tag.
	enum class RoamEnd : uint8_t {
		NO_ANCHOR,    // player left the neighbourhood → hibernate + restore home
		SESSION_TTL,  // rotation, so the same faces don't orbit a stationary player forever
		FAILSTREAK,   // too many unroutable legs
		REGION_GONE,  // the reachable region evaporated (floor change under us, map edge)
		INVARIANT,    // something else took the bot (admin, party, gang)
		SPAWN_CLAIMED,// a hunt started around it — its own reason, not a borrowed NO_ANCHOR, or
		              // evictions and genuine anchor departures become one number in the summary
	};

	struct RoamRun {
		RoamPhase phase = RoamPhase::WALKING;
		Position dest;      // current leg target
		// Pre-injection position. Restored on release so the feature does not, over hours,
		// drain the distributed population into the places players stand.
		Position homePos;
		Position lastSeenPos;      // displacement detection (GM teleport, /cavebot teleport)
		uint32_t anchorClusterIdx = 0;
		int64_t startedMs = 0;     // session TTL base
		int64_t phaseSinceMs = 0;
		int64_t dwellUntilMs = 0;
		uint32_t legs = 0;
		uint8_t failStreak = 0;
		// SUSPENDED covers PvP combat and conscription: the session is not torn down, because a
		// bot that fought and won should go back to wandering, and because tearing down while the
		// bot stays awake is what stranded the reserve in an earlier revision.
		bool suspended = false;
		// A session that WANTS to end but cannot yet, because the bot is on somebody's screen.
		// Hibernating there would delete the body in plain view -- the same pop-out at the near end
		// that off-screen staging avoids at the far end. A retiring roamer keeps walking, preferring
		// destinations out of view, and is retired the moment it is unobserved.
		bool retiring = false;
		RoamEnd retireWhy = RoamEnd::SESSION_TTL;
		int64_t retireSinceMs = 0;
		// ---- monster self-defense (ported from FishingRun; see tickRoamDefense) ----
		uint32_t defendTargetId = 0;
		int64_t defendSinceMs = 0;
		Position defendOrigin;     // leash anchor = where the bot stood when it engaged
		uint32_t defendKills = 0;
	};
	std::unordered_map<uint32_t, RoamRun> s_roam;

	// Accounting ledger, deliberately OUTLIVING the behavioural session above.
	// A bot that stops roaming but stays awake near the player still occupies the reserve slot it
	// was granted; dropping it from the count the moment its session ended would make the slot
	// look free while the bot still consumed density, and the next injection would push the ring
	// over its true ceiling. Erased only when the bot is no longer awake-and-active.
	struct RoamLedgerEntry { int64_t grantedMs = 0; };
	std::unordered_map<uint32_t, RoamLedgerEntry> s_roamLedger;

	// A cached, shared, bounded set of tiles reachable from an anchor — the "where may a roamer
	// walk" oracle. One per anchor, reused by every roamer serving it; rebuilding per bot is what
	// would make the flood cost scale with roamer count instead of with player count.
	struct RoamRegion {
		Position anchor;
		// Every vetted reachable tile: walkable, connected to the anchor, not a house interior,
		// not a doorway. Serves BOTH leg destinations and staging candidates — staging is derived
		// at injection time rather than cached, because off-screen-ness depends on where every
		// anchor is standing right now and would go stale inside the region's own TTL.
		std::vector<Position> dests;
		int64_t builtMs = 0;
		int64_t expiryMs = 0;
		uint32_t cells = 0;    // telemetry: flooded cells
		uint32_t portals = 0;  // telemetry: admitted cross-floor portals
		int64_t buildMs = 0;   // telemetry: wall time of the last cold build
	};
	std::unordered_map<uint64_t, RoamRegion> roamRegions_;  // packed anchor tile key → region
	// Private on purpose: sharing zReachCache_ would publish radius-20 flood sets under keys the
	// z-planner also uses (its cache key is the origin tile alone, TTL 10 minutes), handing
	// zPlanFullRoute a truncated world view and manufacturing false "no route" verdicts.
	std::unordered_map<uint64_t, std::pair<ZReachSet, int64_t>> roamReachCache_;
	uint64_t roamReachHits_ = 0, roamReachMisses_ = 0;
	int64_t lastRoamRegionBuildTick_ = 0;  // one cold build per tick, server-wide

	// Synthetic anchor for headless testing. The feature is inert without a real player or a
	// cast-watched bot, neither of which exists in an automated run, so this is a deliverable
	// rather than a convenience — see /cavebot _global roamanchor.
	Position roamDebugAnchor_;

	const RoamRegion* getRoamRegion(const Position& anchor);  // bot_zgraph.cpp (LocalReach scope)

	// BOT_ROUTE_SPLICE. Elides z-excursion detours (walk into the temple, walk straight back out)
	// from a city-route waypoint list, in place; returns waypoints removed. Defined in
	// bot_zgraph.cpp because its three map-backed gates sit at file scope there — same reason
	// getRoamRegion lives there for LocalReach. Player-free on purpose: loadCityRouteCore runs for
	// hibernated bots too, and a probe needing a Player would splice only for awake ones.
	// `outLog` collects one line per accepted splice, `rejectLog` one per declined window with the
	// gate that declined it; both optional (the /cavebot audit passes both).
	size_t spliceRouteDetours(std::vector<Waypoint>& wps, const std::string& tag,
	                          std::vector<std::string>* outLog = nullptr,
	                          std::vector<std::string>* rejectLog = nullptr);

	// Spliced form of `original`, or `original` when there is nothing to splice. Never mutates the
	// graph's own vector: /cavebot csv dumps cityRouteGraphs_ directly and its parity gate is
	// byte-identical output. bot_data.cpp.
	const std::vector<Waypoint>* spliceRouteCached(uint32_t townId, const std::string& key,
	                                               const std::vector<Waypoint>& original);
	// Precompute every authored direct pair at load, so no gate ever runs on a tick. Chains are
	// per-bot randomised, cannot be enumerated, and splice lazily through the same cache.
	void spliceCityRoutesAtLoad();

	// BOT_TRAVEL_ARRIVE_MIX. Per-town lists of what a landing bot can be sent to besides the
	// depot and temple, built once at load from the route graph. Pure graph data — no map
	// queries — so it has no ordering constraint against the map-dependent builders.
	// (TravelArriveTargets itself is defined beside its member below: a member's TYPE must be
	// declared before the member, even inside a class.)
	void buildTravelArriveTargets();
	// Roll the arrival POI for a journey about to start. Player-free; called from startTravel.
	std::string pickTravelArrivalTarget(const BotState& bot, uint32_t destTownId) const;
	void noteTravelArrivalClass(const std::string& poi);
	void tickAmbientRoam(int64_t nowMs);                      // supervisor, from tick() top
	bool tickRoamSession(BotState& bot);                      // from doDwelling; true = owned tick
	bool roamDriveWalk(BotState& bot);                        // from doIdle; true = owned tick
	bool tryRoamArrival(BotState& bot);                       // pre-consumption arrival hook
	bool tickRoamDefense(BotState& bot, RoamRun& run);        // monster self-defense
	void clearRoamDefense(BotState& bot, RoamRun& run);
	bool roamPickNextLeg(BotState& bot, RoamRun& run);
	bool injectRoamBot(size_t clusterIdx, int64_t nowMs);
	void endRoamSession(BotState& bot, RoamEnd why);
	bool roamSessionInvariantsHold(BotState& bot, const RoamRun& run) const;
	void sweepRoamLedger(int64_t nowMs);
	static const char* roamEndName(RoamEnd why);
	std::string buildRoamReport();
	bool isRoaming(uint32_t guid) const { return s_roam.count(guid) != 0; }
	// A roam session that is actually DRIVING the bot. A suspended session is deliberately NOT
	// counted: the supervisor skips suspended runs (including their TTL), so treating one as
	// active in doActivityReroll would freeze that bot's rerolls indefinitely.
	bool isRoamingActive(uint32_t guid) const {
		auto it = s_roam.find(guid);
		return it != s_roam.end() && !it->second.suspended;
	}
	// Mirrors isFishDefending: castSpell forces single-target while this is true, because the
	// AoE bystander check is gated on a PLAYER target and a roamer fights amid town crowds.
	bool isRoamDefending(uint32_t guid) const {
		auto it = s_roam.find(guid);
		return it != s_roam.end() && it->second.defendTargetId != 0;
	}

	std::unordered_map<uint32_t, FishingRun> s_fishing;
	void startFishingRun(BotState& bot);       // called from the WATER POI-arrival branch
	bool tickFishingRun(BotState& bot);        // true = consumed the tick
	void clearFishingRun(uint32_t guid);       // every interruption path + normal completion
	bool isFishing(uint32_t guid) const { return s_fishing.count(guid) != 0; }
	// True only while the bot is actively fighting a monster that attacked it at the shore.
	// castSpell reads this to force single-target (see the singleTargetOnly latch): the AoE
	// bystander check there is gated on a PLAYER target, so a monster-target wave at a town
	// shoreline — the most bystander-dense place a bot ever fights — would go out unchecked.
	bool isFishDefending(uint32_t guid) const {
		auto it = s_fishing.find(guid);
		return it != s_fishing.end() && it->second.defendTargetId != 0;
	}
	// Nearest monster whose current target is THIS bot. Not "any monster nearby" on purpose —
	// that would send fishing bots off to pick fights instead of defending themselves.
	std::shared_ptr<Creature> pickFishingThreat(const BotState& bot) const;
	// Drives one tick of the fight. True = it owned the tick (caller returns true).
	bool tickFishingDefense(BotState& bot, FishingRun& run);
	// Drop the engagement (engine target, follow, huntTargetId, chase pacing) without disturbing
	// the rest of the run. Safe on a bot that is not engaged.
	void clearFishingDefense(BotState& bot, FishingRun& run);
	// The ONE way a run leaves the shore for home. Extracted from the session-end path because a
	// bare `phase = RETURNING` does nothing: tickFishingRun returns false for any phase but
	// FISHING, so the walk is really this whole block of state (walkTarget/IDLE/planner claim).
	void beginFishingReturn(BotState& bot, FishingRun& run, const char* reason);
	// Chase ceiling, measured from the stand tile — a bot that follows a fleeing monster across
	// the map has abandoned its spot in everything but name.
	static constexpr int32_t FISH_DEFEND_LEASH = 7;
	// Per-engagement fight budget. Long enough for a bad matchup, short enough that a bot cannot
	// be pinned at one shore forever by something it cannot kill.
	static constexpr int64_t FISH_DEFEND_MAX_MS = 90000;
	// Below this, stop fighting and walk home. doHealing runs at 200ms for every awake bot
	// regardless of state, so the walk is survivable.
	static constexpr int32_t FISH_DEFEND_RETREAT_HP_PCT = 35;
	// Stricter than isFishing: true only while the bot is STANDING AT the water casting its rod.
	// This is the second of the two gates that let a bot conjure runes (the other is the idle
	// clock) — a fishing bot legitimately still holds hasWalkTarget/currentPOI pointing at its
	// water POI, which the idle clock correctly treats as an errand, so fishing needs its own test.
	bool isAmbientFishing(uint32_t guid) const {
		auto it = s_fishing.find(guid);
		return it != s_fishing.end() && it->second.phase == FishPhase::FISHING;
	}

	// ---- The ONE cast ----
	// Every tool-on-ground use in the engine goes through here: ambient fishing, ice fishing, and
	// any `tool:` waypoint marker. Creates a TEMP tool exactly like the machete/rope/shovel paths
	// (bot_tick.cpp handleActionWaypoint, the FC machine) instead of pulling a real one out of the
	// bag — the Lua actions never consume or transform the tool, so a temp item drives the very
	// same server path a player's click drives. Faces the target and resolves the target's GROUND
	// stackpos so STACKPOS_USETARGET picks the ground and not something lying on top of it.
	// Returns false only if the item could not be created or the action refused it; the caller
	// decides what that means (ambient tears down its run, ice ends its session).
	bool castToolAt(BotState& bot, uint16_t toolId, const Position& target);

	// ---- Ice fishing: a hold-in-place session driven by a `fish:` waypoint marker ----
	// Distinct from FishingRun on purpose. FishingRun owns bot.walkTarget/dwellUntil/planner
	// claims and walks the bot through a multi-minute TRAVEL->FISHING->RETURNING trip; this is a
	// few seconds of standing still at a hunt waypoint the bot already reached, after which the
	// patrol must continue. Sharing the struct would mean ice-only fields on it and dragging ice
	// sessions through ambient-only interruption logic that assumes a trip.
	struct IceFishSession {
		Position target;        // the ice tile (7200 closed / 7236 open)
		int64_t endsAtMs = 0;   // hard deadline — the safety net the design rests on
		int64_t nextUseMs = 0;
		uint16_t casts = 0, picks = 0;
		// Manual sessions (`/cavebot <bot> fishice`) run until the hole actually closes rather
		// than for a patrol-sized slice, so an admin watching gets the whole pick->catch cycle
		// instead of a session that may expire mid-way on an unlucky roll. endsAtMs is still set,
		// as a safety cap only.
		bool untilClosed = false;
	};
	std::unordered_map<uint32_t, IceFishSession> iceFishing_;
	void beginIceFishSession(BotState& bot, const Position& target, bool untilClosed = false);
	// `/cavebot <bot> fishice [waypoint]` — adjacent ice, or jump to the nearest `fish:` waypoint.
	std::string forceIceFish(BotState& bot, bool useWaypoint);
	bool tickIceFishSession(BotState& bot);    // true = hold position, session still running
	void endIceFishSession(BotState& bot, const char* reason);
	bool isIceFishing(uint32_t guid) const { return iceFishing_.count(guid) != 0; }

	// ---- TRUE MULTI-FLOOR (bot_zgraph.cpp) ----
	void buildZPortalGraph(); // whole-map scan (live tiles + BasicTile cache), called from loadHuntData
	// Portal-graph disk cache. Rebuilding costs ~21s synchronously on the dispatcher thread,
	// freezing the whole server on every /cavebot reload; the graph depends only on the map,
	// so it is persisted and reloaded. Keyed on map size+mtime — if that cannot be read we
	// rebuild rather than risk a stale graph mis-routing every bot.
	bool loadZGraphCache();
	void saveZGraphCache();

	// ---- BOT_SUPPLY_REALISM: fishing-spot index (built in bot_zgraph.cpp) ----
	//
	// A fishable water tile plus a vetted tile to stand on. Harvested from buildZPortalGraph's
	// existing whole-map sweep (the ground-id read is already paid for there) and persisted in
	// the same disk cache, so /cavebot reload LOADS it rather than rescanning. Doing this as a
	// standalone Map::getTile pass would materialize tiles out of the BasicTile cache — see
	// bot_zgraph.cpp's header on why that costs hundreds of MB.
	struct FishingSpot {
		Position water;   // the tile the rod is used on
		Position stand;   // walkable, never a floor-change tile; NOT required to be adjacent to
		                  // water — anywhere within Actions::canUseFar's <7,5> box with clear LOS
		                  // to `water` qualifies (see findNearbyFishingSpot in bot_zgraph.cpp)
		uint32_t townId = 0;
	};
	std::unordered_map<uint32_t, std::vector<FishingSpot>> fishingSpots_; // townId -> spots
	// The spot chosen when a trip STARTED, carried through the walk so arrival uses the vetted
	// water/stand pair instead of re-deriving one from wherever the bot happened to halt.
	// POI_ARRIVAL_DIST is 3, so "arrived" can be three tiles off the stand tile, and re-deriving
	// there throws away the index's walkability + line-of-sight vetting.
	std::unordered_map<uint32_t, FishingSpot> s_pendingFishSpot;
	// ---- Per-bot claims on the stand AND the water tile ----
	// Fishing shipped with NO reservations, matching every other supply behaviour — a deliberate
	// choice, reversed here because two bots can otherwise cast into the same water from two
	// different stands (A* collision only ever kept them off the same STAND tile).
	// ONE map for both roles: a stand tile must pass zWalkableAt and a water tile must pass
	// zCellIsClearWater, and fishable water ids are blockSolid — which is exactly what makes
	// zWalkableAt reject them — so no tile can ever hold both roles and the keys cannot collide.
	struct FishClaim {
		uint32_t guid = 0;
		int64_t expiresAt = 0;
	};
	// Must outlast the WHOLE round trip, the same lesson APPROACH_RESERVE_MS' own comment records:
	// the walk out (up to the 240s planner budget) + up to botFishDurationMaxSec (420s) of casting
	// + the walk home (the RETURNING leg re-claims the planner, another 240s). 900s + 60s slack.
	static constexpr int64_t FISH_CLAIM_MS = 960000;
	std::unordered_map<uint64_t, FishClaim> s_fishClaims; // botTileKey(stand|water) -> claim
	// True when the stand OR water tile is claimed by a DIFFERENT bot. Read-only, so it is safe
	// from selectFishingSpot/findNearbyFishingSpot's const context; byGuid lets a bot re-check a
	// spot it already holds without blocking on itself.
	bool isFishSpotClaimed(const FishingSpot& spot, uint32_t byGuid) const;
	// Stamps both tiles. Mutates, so it is never called from the const query path — the call sites
	// are bot_poi.cpp's candidate block and forceFishingTrip.
	void claimFishingSpot(uint32_t guid, const FishingSpot& spot);
	// Drops every claim this guid holds — keyed by guid like releaseNpcApproach, so no caller ever
	// has to remember which tiles it took. Safe on a bot holding none.
	void releaseFishingSpot(uint32_t guid);

	uint32_t zFishDropped_ = 0; // spots removed by the component reachability filter (telemetry)
	void buildFishingSpotIndex(const std::vector<Position>& waterCells);
	// Live nearest-CASTABLE-water search around a position, expanding-ring, same floor only. This
	// is the AUTHORITY for which tile a bot casts at; the prebuilt index above is only a coarse
	// "which town has water" travel hint. Rings outward over WALKABLE STAND tiles (starting at the
	// bot's own position), and for each ring checks the whole Actions::canUseFar <7,5> throw box
	// for clear water rather than only the water's 8 immediate neighbours — a bot need not touch
	// the water to fish it. Uses the non-materializing BasicTile primitives for classification and
	// only spends a materializing LOS probe on cells that already passed that filter (bounded by
	// FISH_LOS_PROBE_BUDGET in bot_zgraph.cpp), so a bot already near a shore pays almost nothing.
	// Radius 20 bounds the STAND search only — actual water can be up to 7/5 tiles further out —
	// and sits well inside the precedent set by Z_DOOR_BRIDGE_RADIUS (24).
	//
	// Two phases since the shore-hugging fix: phase one (the ring search) only proves water is
	// reachable at all and is still bot-centred, so a bot far from any water still stops searching
	// at the same ring it always did — that is what keeps the far case cheap and unchanged. Phase
	// two then re-centres on the water tile found and hugs the shore, bounded to that water's own
	// <=7,5 throw box, so it costs nothing extra in the far case either. See the function's own
	// header in bot_zgraph.cpp for the full contract.
	//
	// Still SAME FLOOR ONLY per call — `from.z` never varies within one call. selectFishingSpot is
	// what retries this on neighbouring floors (botFishZBand-bounded, zGraphReady_-gated, and only
	// once the same-floor index has also come up empty), by calling this function again with a
	// z-shifted `from`.
	static constexpr int32_t FISH_LOCAL_RADIUS = 20;
	// Ceiling on materializing isSightClear probes for ONE selection. Shared by the live scan
	// (bot_zgraph.cpp) and the portal-anchored cross-floor phase (bot_supply.cpp), which
	// threads a single budget across all its candidates. Same idiom as Z_DOOR_BRIDGE_BUDGET.
	static constexpr int32_t FISH_LOS_PROBE_BUDGET = 500;
	bool findNearbyFishingSpot(const Position& from, int32_t maxRadius, FishingSpot& out) const;
	// Same search, sharing a caller-supplied LOS-probe budget instead of a fresh
	// FISH_LOS_PROBE_BUDGET per call. Exists for selectFishingSpot's portal-anchored cross-floor
	// phase, which probes up to FISH_PORTAL_CANDIDATES_MAX candidate portal landings in ONE
	// selection call — without a shared budget that would be N fresh budgets instead of one
	// ceiling. The 3-arg overload is unchanged: it just supplies its own fresh budget.
	bool findNearbyFishingSpot(const Position& from, int32_t maxRadius, FishingSpot& out,
	                           int32_t& losBudget) const;
	// Nearest eligible spot for this bot, honouring botFishMaxDist / botFishMaxDz / botFishZBand
	// and the shared isCrowded() check. False when the bot's town has none in reach. Tries same
	// floor before other floors, and the live scan before the prebuilt index, in that strict
	// priority order — see the function's own header in bot_supply.cpp.
	bool selectFishingSpot(const BotState& bot, FishingSpot& out) const;
	// Plan the next hop of a cross-z route from the bot's position toward target.
	// Returns false when the graph has no route (caller falls back to the legacy
	// greedy FC scan). On success `out` is the FIRST portal to traverse.
	bool zPlanNextHop(BotState& bot, const Position& target, ZPlannedHop& out, bool forceGraph = false);
	// Full-route variant used by the /cavebot zplan debug command.
	bool zPlanFullRoute(const Position& from, const Position& target, std::vector<botnav::ZRouteHop>& hops, bool forceGraph = false, uint32_t forGuid = 0);
	// `site` tags which failure path quarantined the portal (logged as [ZBLACKLIST] site=...), so a
	// spurious-blacklist source can be found from the journal alone.
	void zBlacklistPortal(const Position& portalPos, const char* site = "other");
	bool zPortalBlacklisted(const Position& portalPos);
	// dumpnav v2: collect exact portals inside a region (same classifier as the graph build).
	void collectNavPortalsInRegion(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2, std::vector<botnav::NavPortal>& out);

	// ========================================================================
	// BOT_HOUSE_VISIT — awake bots visit bot-owned houses (bot_house.cpp)
	// ========================================================================
	//
	// An awake bot in a town walks to a bot-owned house there, opens the door, and idles at one
	// interior tile, optionally greeting a hireling, standing at a locker, or training at a dummy.
	//
	// SAME FLOOR ONLY, and that is a product decision rather than a limitation to be lifted later:
	// the idle tile, the locker and the dummy are all on the floor the bot walks in on. It also
	// keeps the feature clear of the cross-floor planner stall that holds botFishZBand at 0.
	//
	// The interior INDEX is harvested in bot_zgraph.cpp, riding buildZPortalGraph's whole-map sweep
	// and persisted in the same disk cache — the fishing-spot rule: whole-map-sweep code lives with
	// the sweep, whatever consumes it. Everything else (which house, which tile, claims, the run
	// itself) is feature policy and lives in bot_house.cpp.

	// One house's interior, bucketed by floor. Only the entry floor is ever read today, but the
	// harvest is floor-keyed anyway because the sweep visits every z regardless — filtering during
	// the harvest would cost the same and throw information away.
	struct HouseFloor {
		std::vector<Position> idleTiles;   // walkable, not FC/teleport, nothing on top
		std::vector<Position> dummyTiles;  // exercise dummies (ItemType::isDummy)
		std::vector<Position> lockerTiles; // depot/locker items (ItemType::isDepot)
	};
	struct HouseInterior {
		uint32_t townId = 0;
		Position entry;                                  // House::getEntryPosition()
		std::unordered_map<uint8_t, HouseFloor> floors;  // z -> contents
	};
	// houseId -> interior. Built by the sweep, persisted at ZCACHE_VERSION 10.
	std::unordered_map<uint32_t, HouseInterior> houseInteriors_;
	// Raw per-cell harvest handed from the sweep to buildHouseInteriorIndex, mirroring how
	// waterCells is handed to buildFishingSpotIndex.
	struct HouseCell {
		uint32_t houseId = 0;
		Position pos;
		bool walkable = false;
		bool dummy = false;
		bool locker = false;
	};
	void buildHouseInteriorIndex(const std::vector<HouseCell>& cells); // bot_zgraph.cpp

	// townId -> bot-owned house ids. Built from g_game().map.houses.getHouses() at loadHuntData
	// time — NOT a map sweep, so unlike the interior index this one lives in bot_house.cpp.
	std::unordered_map<uint32_t, std::vector<uint32_t>> botHousesByTown_;
	void buildBotHouseIndex();
	bool isBotOwnedHouse(uint32_t houseId) const;

	// What the bot decided to do inside, chosen when the POI is offered so the walk has a concrete
	// destination the planner can route to.
	// SHRINE is appended, never inserted: HouseMode is persisted nowhere, but it IS logged as an
	// integer by [HOUSE_VISIT_START], so renumbering would silently reinterpret old journal lines.
	enum class HouseMode : uint8_t { IDLE = 0, HIRELING = 1, DUMMY = 2, LOCKER = 3, SHRINE = 4 };
	enum class HousePhase : uint8_t { APPROACH = 0, IDLE = 1 };
	struct HouseRun {
		uint32_t houseId = 0;
		HouseMode mode = HouseMode::IDLE;
		HousePhase phase = HousePhase::APPROACH;
		Position tile;             // the exact tile to stand on
		Position feature;          // dummy/locker/hireling position (mode != IDLE)
		std::string hirelingName;  // mode == HIRELING
		uint16_t trainWeaponId = 0;
		bool trainingActive = false;
		int64_t until = 0;         // when the idle window ends
		// Deadline for closing the last tiles onto `tile` once inside, mirroring FishingRun's
		// approachUntil. Armed the first tick the bot stands on a tile of this house, NOT at walk
		// start — the walk in is bounded by the planner's own 240s stale-target budget.
		int64_t settleUntil = 0;
	};
	std::unordered_map<uint32_t, HouseRun> s_houseRuns; // guid -> run
	bool isHouseVisiting(uint32_t guid) const { return s_houseRuns.count(guid) != 0; }
	// True once the bot is physically inside the house it claimed — the gate for arming the settle
	// deadline, for the displacement guard, and for the exact-tile arrival test.
	bool isInsideRunHouse(const BotState& bot) const;

	// Claims. Separate from s_approachReservations on purpose: that table is released by
	// clearPlannerWalk, which fires on ordinary POI arrival — exactly when a house visitor still
	// needs its tile held. All three are read-filtered on expiresAt, so an exit path that misses
	// endHouseVisit costs a TTL rather than a permanently consumed slot.
	struct HouseClaim {
		uint32_t guid = 0;
		int64_t expiresAt = 0;
	};
	// Walk in (up to the 240s planner budget) + botHouseIdleMaxSec (600s) + slack.
	static constexpr int64_t HOUSE_CLAIM_MS = 900000;
	std::unordered_map<uint64_t, HouseClaim> s_houseTileClaims;  // botTileKey(tile|dummy) -> claim
	std::unordered_map<uint32_t, std::vector<HouseClaim>> s_houseOccupants; // houseId -> visitors
	bool isHouseTileClaimed(const Position& tile, uint32_t byGuid) const;
	uint32_t houseOccupantCount(uint32_t houseId, uint32_t excludeGuid) const;
	void releaseHouseClaims(uint32_t guid);

	// Offer/arrival/lifecycle. Mirrors the fishing run's shape one-for-one.
	bool pickHouseVisit(const BotState& bot, HouseRun& out) const; // candidate for the POI roll
	// Commit the claims a pick chose. Split from pickHouseVisit so a candidate can be OFFERED to
	// the weighted roll without reserving anything — a candidate that loses must leave nothing
	// behind, which is the discipline the NPC-visit slot already follows.
	void claimHouseVisit(uint32_t guid, const HouseRun& run);
	// Arrival, for BOTH entry points: a POI-driven visit (currentPOI set) and a forced
	// `/cavebot house` (currentPOI null). Returns true when it consumed the tick. Gated on the run
	// rather than on currentPOI precisely because the two differ there, and the 3-tile generic
	// arrival would otherwise eat the forced one while the bot was still outside the building.
	bool tryHouseArrival(BotState& bot);
	void startHouseVisit(BotState& bot);      // called by tryHouseArrival once standing on the tile
	bool tickHouseVisit(BotState& bot);       // true = consumed the tick
	void endHouseVisit(uint32_t guid, const char* reason);
	void stopHouseTrainingIfActive(BotState& bot);
	// One-shot: set by endHouseVisit, consumed by the next doActivityReroll whatever it rolls, so
	// the walk OUT of the house gets the scoped planner (door handling) exactly once.
	std::unordered_set<uint32_t> s_houseExitPlanner;
	// /cavebot house [<houseId>] — force a visit now, bypassing botHouseVisitPct.
	std::string forceHouseVisit(BotState& bot, const std::string& arg);
	// /cavebot houseinfo [<houseId>|near] — dump the cached interior + door ids.
	std::string describeHouseInterior(const BotState& bot, const std::string& arg) const;

	// ---- BOT_SHRINE_IDLE (bot_shrine.cpp) ----
	//
	// Shape is the house visit's, not the fishing run's, and that is deliberate. A shrine visit
	// reserves ONE exact tile so the bot can face the furniture; fishing tolerates the generic
	// 3-tile POI arrival only because it re-scans live water and carries an approach grace, and
	// neither of those exists here. So: run created at offer time in APPROACH, an early arrival
	// hook in doIdle gated on the run rather than on currentPOI (a forced visit has no POI), an
	// exact-tile test, and a settle deadline.
	enum class ShrinePhase : uint8_t { APPROACH = 0, IDLE = 1 };
	struct ShrineRun {
		Position shrine;                              // blocking furniture — what we face
		Position stand;                               // the reserved cardinal-adjacent tile
		uint8_t  kind = 0;                            // SHRINE_KIND_REWARD / _IMBUING
		ShrinePhase phase = ShrinePhase::APPROACH;
		int64_t  settleUntil = 0;                     // armed on first contact, like HouseRun's
		int64_t  until = 0;                           // idle window end
		int64_t  nextGlanceMs = 0;                    // next cosmetic re-face
	};
	std::unordered_map<uint32_t, ShrineRun> s_shrineRuns;
	bool isShrineVisiting(uint32_t guid) const { return s_shrineRuns.count(guid) != 0; }

	// Walk in (up to the 240s planner budget) + botShrineSettleSec (45s) + botShrineIdleMaxSec
	// (240s) = 525s, plus slack. Same one-number-for-the-whole-trip discipline as FISH_CLAIM_MS
	// (960s) and HOUSE_CLAIM_MS (900s); below ~530s the claim would lapse mid-dwell.
	static constexpr int64_t SHRINE_CLAIM_MS = 600000;
	// botTileKey(stand tile) -> claim. Keyed by (guid, kind) in the VALUE, not just guid: a single
	// reroll offers both kinds and therefore takes two claims, so a guid-only release fired when
	// one kind loses the roll would free the winning kind's claim too.
	struct ShrineClaim {
		uint32_t guid = 0;
		uint8_t  kind = 0;
		int64_t  expiresAt = 0;
	};
	std::unordered_map<uint64_t, ShrineClaim> s_shrineTileClaims;
	bool isShrineTileClaimed(const Position& tile, uint32_t byGuid) const;
	uint32_t shrineOccupantCount(const Position& shrine, uint32_t excludeGuid) const;
	void claimShrineSpot(uint32_t guid, uint8_t kind, const ShrineSpot& spot);
	void releaseShrineClaim(uint32_t guid, uint8_t kind);   // one kind
	void releaseShrineClaims(uint32_t guid);                // every kind, for teardown

	// The scan itself. One pass collects BOTH kinds — never call it per kind.
	bool findNearbyShrines(const Position& from, int32_t radius, ShrineScanResult& out) const;
	// Memo-backed lookup for a town; fills s_shrineMemo on first use (both anchors, both kinds).
	const ShrineMemo& shrineMemoForTown(uint32_t townId);
	// P1 of the two-tier lookup: ONE bot-centred live scan, run once per reroll and shared by both
	// kinds. findNearbyShrines already fills both kinds in a single pass, so calling it per kind
	// would double the cost for nothing. Mirrors the fishing ladder (bot_supply.cpp): local live
	// scan first, prebuilt lookup only as a fallback.
	void findLocalShrines(const BotState& bot, ShrineScanResult& out) const;
	// Candidate for the POI roll. Commits nothing — claiming is the caller's job, so a candidate
	// that loses the weighted roll leaves nothing behind. `local` is P1's result for this reroll.
	bool selectShrineSpot(const BotState& bot, uint8_t kind, const ShrineScanResult& local,
	                      ShrineSpot& out);

	bool tryShrineArrival(BotState& bot);
	void startShrineVisit(BotState& bot);
	bool tickShrineVisit(BotState& bot);
	void endShrineVisit(uint32_t guid, const char* reason);
	// /cavebot <bot> shrine [reward|imbuing] — force a visit now, bypassing botShrineVisitPct.
	std::string forceShrineVisit(BotState& bot, const std::string& arg);
	// /cavebot shrines [<town>] — force-fill every town's memo and dump it.
	std::string describeShrines(const std::string& arg);

	// Navigation helpers (Phase 1+2)
	bool goTo(BotState& bot, const Position& target, int32_t maxDist = -1);
	// Trace the exact tiles the live walker would take on one leg, at the stock node budget.
	// Backs `/cavebot route`; see the definition in bot_nav.cpp for the parity contract.
	// `wide` selects the fallback search profile (4096-node pool + PATH_WIDE_DIST box) that
	// goTo() escalates to when the chunked search fails.
	bool botTraceLegPath(const std::shared_ptr<Player>& mover, const Position& from, const Position& to,
		int32_t arrivalDist, std::vector<Position>& outTiles, bool wide = false);
	// Replays goTo()'s full chunk-walk-repath loop over a leg — the faithful version, since the
	// live walker never solves a long leg in a single search.
	bool botTraceLegWalk(const std::shared_ptr<Player>& mover, const Position& from, const Position& to,
		int32_t arrivalDist, std::vector<Position>& outTiles, std::string& outNote);
	bool goToWithDoors(BotState& bot, const Position& target, int32_t maxDist = -1, WaypointType wpType = WaypointType::NODE);
	// One cooldown-gated unchunked wide search. Planner legs only — see the definition for why
	// the cooldown is load-bearing rather than defensive.
	bool goToWide(BotState& bot, const Position& target, int32_t maxDist);
	bool isAtPosition(const Position& a, const Position& b, int32_t range = 3) const;
	bool tryOpenDoors(BotState& bot, const std::shared_ptr<Player>& player, const Position& targetPos);
	bool tryOpenDoorAt(BotState& bot, const std::shared_ptr<Player>& player, const Position& doorPos);
	bool tryOpenAdjacentDoor(BotState& bot, const std::shared_ptr<Player>& player);
	bool tryOpenDoorsOnTrail(BotState& bot, const std::shared_ptr<Player>& player);
	bool tryPathToHuntDoor(BotState& bot, const std::shared_ptr<Player>& player);

	// City route navigation
	void loadCityRoutesCsv(std::unordered_map<uint32_t, CityRouteGraph>& outGraphs,
	                       std::vector<Waypoint>& outAdvStone);
	std::string detectNearestPOI(uint32_t townId, const Position& pos) const;
	std::string findBestRouteSource(uint32_t townId, const Position& pos, const std::string& dstPOI,
		const std::unordered_set<std::string>& excluded) const;
	const std::vector<Waypoint>* findCityRoute(uint32_t townId, const std::string& src, const std::string& dst) const;
	// Player-free core: looks up route, populates bot.cityRouteWps/Idx, sets followingCityRoute.
	// Used by live startCityRoute (which adds castLog) and by virtual sim phase handlers.
	// Resolve src->dst the way loadCityRouteCore does (direct pair, then multi-hop chain), so the
	// /cavebot splice audit and the runtime cannot disagree about what a route IS. `chainKey`
	// receives "src>dst" or "src>mid>dst" — the splice cache keys on it, and it must carry the
	// whole chain because multi-hop tie-breaks are shuffled per botSeed.
	const std::vector<Waypoint>* resolveCityRoute(uint32_t townId, const std::string& src,
	                                              const std::string& dst, std::vector<Waypoint>& scratch,
	                                              std::string& chainKey, uint32_t botSeed = 0) const;
	bool loadCityRouteCore(BotState& bot, const std::string& srcPOI, const std::string& dstPOI);
	bool startCityRoute(BotState& bot, const std::string& srcPOI, const std::string& dstPOI);
	bool followCityRoute(BotState& bot);

	// Unified waypoint-following (used by city routes, travel, leaving, patrol)
	struct WaypointFollowConfig {
		int32_t globalTimeoutMs = 300000;
		int32_t perWpStuckMs = 30000;
		bool enableLookaheadSkip = false;
		bool enableTeleportStand = false;
		int32_t zChangeGraceMs = 500;
		std::string logPrefix = "WP";
		// Planner legs only. When a waypoint's ordinary chunked walk fails, escalate ONCE to the
		// unchunked wide search (4096 nodes, PATH_WIDE_DIST box) — the same tier planScopedWalk's
		// direct search uses. A leg's first waypoint is usually a DOOR, and a door reachable only
		// by a dogleg is precisely what the chunked walker cannot solve: it aims at a straight-line
		// interpolated point that lands inside the building. Left false for hunt/city/quest routes,
		// whose per-tick population is exactly what made the old goTo-level wide search a 5.7s
		// dispatcher stall (see goToWide's own cooldown).
		bool wideSearchOnFail = false;
	};
	struct WaypointFollowResult {
		bool inProgress = true;
		bool advanced = false;
		bool aborted = false;
	};
	WaypointFollowResult followWaypoints(BotState& bot, const std::vector<Waypoint>& waypoints,
		size_t& waypointIdx, uint32_t& skipCount, const WaypointFollowConfig& config);

	// Monster blocking
	bool tryAttackBlockingMonster(BotState& bot);

	// NPC blocking — squeeze past an NPC standing in a narrow corridor when A* has
	// found no route around it (e.g. Roshamuul temple). See implementation for the
	// "no other choice" + forward-progress guards.
	bool tryStepPastBlockingNpc(BotState& bot, const Position& target);

	// Depot locker
	Position findReachableDepotLocker(BotState& bot);

	// Wake-from-hibernation safe-position picker. If virtualPos is unsafe (wall,
	// FC, teleport, depot box, magic field, etc.), walks back through the bot's
	// route chain to find a safe prior waypoint. Falls back to town temple if
	// no safe waypoint exists. See implementation comment for the per-state
	// route mapping.
	// Now non-const + takes non-const BotState because Phase F rewinds the bot's
	// waypoint index (cityRouteIdx / huntWaypointIdx / advStoneRouteIdx) atomically
	// with the chosen wake position. Without the rewind, live AI's followWaypoints
	// sanity check would trip after wake when virtualPos was unsafe and walkBack chose
	// a much earlier waypoint than the current index.
	// BOT_LIVENESS (2026-06-13): proximityWake=true additionally relocates a bot that
	// would wake INSIDE a viewer's screen to an upstream off-screen route NODE so it
	// walks into view (HUNTING patrol/leaving + AdvStone only — the routes that survive
	// hibernation). Non-qualifying states fall through to on-screen placement + the
	// wakeBot login sparkle. Teleport/explicit wakes pass false (no relocation).
	Position chooseWakePosition(BotState& bot, const Position& virtualPos, bool proximityWake);

	// FC-safe placement check shared by chooseWakePosition and chooseSafePartyFollowPos:
	// true if a bot must NOT be teleport-placed on tile p (wall/blocking, any FLOORCHANGE
	// direction, teleport tile, depot box, magic field, quest/aid MoveEvent tile, or a
	// pathfinding-rejected stack). Extracted so party placement reuses the exact wake-safety
	// mask, not a weaker copy.
	bool isUnsafeWakeTile(BotState& bot, const Position& p);

	// FC-safe placement of a party support near the leader. Like chooseWakePosition's spiral
	// but de-collides against a caller-owned per-tick set (NOT burstReservedTiles_) and falls
	// back to `center` (stacking) rather than the temple — a follower must never be flung to
	// temple. Used by the party-follow teleport paths so supports never land on a ladder/hole
	// and never stack on the leader's exact tile.
	Position chooseSafePartyFollowPos(BotState& bot, const Position& center,
		std::unordered_set<uint64_t>& reservedThisTick);

	// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 3/4. Shared spiral/temple/login tail for every
	// "this tile is unusable, find another" path. `reserved` is by reference because the three
	// callers have deliberately different reservation scopes (wake burst / per-tick party set /
	// per-call temporary); `allowTempleFallback` is false for party follows, which must stack
	// near the leader rather than be flung to temple. Returns an unset Position on total
	// failure so the caller decides.
	Position safePlacementTail(BotState& bot, const Position& center,
		std::unordered_set<uint64_t>& reserved, bool allowWideRings, bool allowTempleFallback,
		const char* site);

	// Vet a BOT_TELEPORT destination. internalTeleport skips queryDestination, so a bot placed
	// on a floor-change tile just stands there while the follower thinks it arrived. Pass
	// route+idx to allow a bounded backward rewind (index is rewound atomically with the
	// returned position); pass nullptr at sites that teleport onto a TELEPORT waypoint, where
	// rewinding across an unwalkable synth bridge would strand or loop the bot.
	Position safeTeleportLanding(BotState& bot, const Position& desired,
		const std::vector<Waypoint>* route, size_t* idx, const char* site);

	// BOT_LIVENESS (2026-06-13): true if position p falls inside the CLIENT viewport
	// (ProtocolGame::canSee, protocolgame.cpp:1943 — asymmetric 8/6 box, signed offsetz,
	// surface/underground cross-z cutoffs) of ANY current anchor (real player or
	// cast-watched bot, from currentAnchorPts_). margin expands the box on all sides to
	// account for an anchor walking toward p before the bot can move. Used to decide
	// off-screen wake placement (margin>0) and the login sparkle (margin=0).
	bool wouldBeSeenByAnchor(const Position& p, int margin) const;

	// POI selection
	const BotPOI* selectNextPOI(BotState& bot);

	// BOT_LIVENESS_PACK Phase C: per-tick liveness behaviors (turn-in-place, idle
	// drop-and-pickup fidget, AFK loitering micro-actions). Called from doIdle +
	// doDwelling. All gated against combat/walking/FC/dummy-training states.
	void tickLivenessBehaviors(BotState& bot);

	// BOT_LIVENESS_PACK: idle litter drop. Called every tick from doIdle + doDwelling.
	// When a bot has been stationary >=1s in IDLE/DWELLING with no errand (and not in
	// combat/PvP/hunt/FC/dummy-training), rolls a chance to drop ONE cheap NPC-buyable
	// item (safeFidgetItemIds_) on its tile. Drop-only: no pickup. One roll per stop,
	// at most one drop per awake session (fidgetDroppedThisWake, reset in wakeBot).
	void maybeFidgetDrop(BotState& bot);

	// Minimum gap between full mount re-rolls (dismount + fresh botMountChancePct flip +
	// fresh model) for one guid. Must clear the hibernate/wake churn cycle, not just the
	// average case: bot_hibernation.lua's HYSTERESIS_MS = 30000 is the per-cycle hibernate
	// delay, and commit 8125052a4 measured live proximity-wake oscillation at ~26-31s end to
	// end for cast-watched bots sitting at the edge of a viewer's radius. 120s clears two
	// back-to-back cycles with margin, so an observed bot never visibly swaps mounts
	// mid-session, while a bot genuinely away for minutes still gets fresh variety.
	static constexpr int64_t MOUNT_REROLL_MIN_INTERVAL_MS = 120000;

	// Draw a fresh mount intent + model for a genuinely new session. Called from the three
	// reconnect paths (activateBot / wakeBot / reactivateBotForReload); self-throttling, so
	// no caller needs to know whether its wake was churn or real.
	void rollMountForReconnect(BotState& bot, const std::shared_ptr<Player>& player);

	// Steady-state retry: mount the bot the moment it is outside a PZ and still unmounted.
	// Player::toggleMount(true) hard-fails inside a protection zone while
	// toggleMountInProtectionZone is false, and most bots reconnect standing in town, so the
	// roll at reconnect is not enough on its own — without this the intent is simply lost.
	void tryOpportunisticMount(BotState& bot, const std::shared_ptr<Player>& player);

	// BOT_LIVENESS_PACK Phase C.5: wrapper around player->startAutoWalk that may
	// insert a short pause before issuing the walk. Gated against combat / FC /
	// hunt-target so chase paths never pause. Hard cap of botWalkPauseMaxPerRoute
	// pauses per route (bot.pausesThisRoute, reset on doActivityReroll).
	void botStartAutoWalk(BotState& bot, const std::shared_ptr<Player>& player,
	                      const std::vector<Direction>& dirs);

	// True if a real player (non-bot) or a cast-watched bot is on `bot`'s screen.
	// Result is cached per-bot for ~500ms (walkObservedCache) so the observed-tier
	// walk pause doesn't run a Spectators::find<Player> scan on every walk step.
	bool botWalkObserved(BotState& bot, const std::shared_ptr<Player>& player);

	// BOT_LIVENESS_PACK Phase C.2 + D: load chat templates from
	// data/bot_chat/phrases.json. Populates chatCatalog_ + tradeCatalog_.
	void loadBotChatPhrases();

	// Fix #11: hibernated-bot channel chat helper. Called per-bot inside virtualTick.
	// Emits World Chat (3) + Advertising (5) ONLY — never Local Chat (TALKTYPE_SAY)
	// since hibernated bots are off-world and have no spectators. Gated by
	// livenessCfg_.hibernatedChatEnabled.
	void tickHibernatedChat(BotState& bot);

	// Emit a chat message in the given category. Picks a template (with anti-repeat
	// LRU check), runs placeholder substitution, gates by observer presence for
	// local-say categories, and calls the appropriate emit path:
	//   channelId == 0: g_game().internalCreatureSay TALKTYPE_SAY (local radius)
	//   channelId  > 0: g_chat().talkToChannel (global to channel subscribers)
	// Returns true if a message was actually emitted (chance-roll passed + observer
	// present if required + template found + substitution succeeded).
	bool tryEmitChat(BotState& bot, const std::shared_ptr<Player>& player,
	                  const std::string& category, uint16_t channelId = 0);
	// v2: renders one template — %city/%town/%vocation/%item/%price/%stack plus
	// level-gated %creature/%spawn and %level. Returns "" when a placeholder is
	// unsatisfiable (e.g. no level-coherent creature) so the caller rerolls the
	// template instead of emitting a level-incoherent or half-empty line.
	std::string renderChatTemplate(const BotState& bot, const std::shared_ptr<Player>& player,
	                               const std::string& tmpl, uint8_t botLane);
	// Hash of the lowercased rendered line — key for the global anti-repeat map
	// and the bot_chat_emissions.text_hash telemetry column.
	static uint32_t hashRenderedChat(const std::string& text);

	// Adventurer's Stone trip (POIType::ADVENTURER_STONE)
	bool startAdventurerStoneTrip(BotState& bot);
	void doAdventurerStone(BotState& bot);
	void endAdventurerStoneTrip(BotState& bot);
	// Clear ALL Adventurer's-Stone overlay fields + stop the Lua training loop, WITHOUT
	// touching state / cityRoute / visitedPOIs (callers own those). Shared by
	// endAdventurerStoneTrip and party recruitment (setupSupport / setupVirtualSupport) so a
	// bot pulled into a party while mid-AdvStone-trip stops running doAdventurerStone (which
	// preempts the state switch in processBot) and actually reaches doParty.
	void clearAdvStoneState(BotState& bot);
	// Pass requireZ != 0 to constrain to NODE waypoints on that floor (e.g. require z=7
	// when the sub-activity will go to chest/dummy and the bot can't cross floors mid-
	// phase-1 — see BOT_LIVENESS_PACK Phase A.5). Returns 0 if no candidate matches.
	uint16_t pickAdventurerStoneIdleIdx(uint8_t requireZ = 0) const;
	void selectAdvStoneSubActivity(BotState& bot);
	void stopAdvStoneTrainingIfActive(BotState& bot);

	// Vocation helpers
	uint8_t getBaseVocation(uint8_t vocId) const;
	int32_t getAttackRange(uint8_t baseVoc) const;

	// Keep-distance helpers
	int32_t getEffectiveKeepDistance(const BotState& bot) const;
	bool findThreatCentroid(BotState& bot, int32_t keepDist, Position& outCentroid, int32_t& outNearestDist);
	bool getRetreatStep(BotState& bot, const Position& threatPos, Direction& outDir);

	// BOT_PVP_REALISM helpers (see bot_engine.cpp "PvP realism" section)
	void pvpResolveHasteSpells();                                       // init: build resolvedHaste_
	int32_t getPvpKeepDistance(uint8_t baseVoc) const;                  // desired PvP distance by voc
	bool pvpCastBestHaste(BotState& bot, const std::shared_ptr<Player>& player); // strongest castable haste
	bool pvpDanceStep(BotState& bot, const std::shared_ptr<Player>& player,
		const Position& targetPos, int32_t desiredDist);               // monster-style single-step dance/strafe
	void pvpReposition(BotState& bot, const std::shared_ptr<Creature>& target); // player-aware combat movement
	bool pvpTryPlaceWall(BotState& bot, const std::shared_ptr<Creature>& target, bool fleeing); // magic wall geometry
	bool pvpRunPendingWall(BotState& bot);                             // two-tick: place wall on vacated tile

	// Teleport helpers
	void teleportToTemple(BotState& bot);

	// Equipment helpers
	void loadEquipmentCsv(std::unordered_map<uint32_t, BotEquipment>& out);
	void equipBot(BotState& bot);
	void applyBotImbuements(const std::shared_ptr<Player>& player, uint32_t baseVoc);
	void applyBotForgeTiers(const std::shared_ptr<Player>& player);

	// Item search helpers
	std::shared_ptr<Item> findLadderItem(const Position& pos);
	std::shared_ptr<Item> findSewerItem(const Position& pos);

	// Town detection from position
	uint32_t findNearestTown(const Position& pos) const;
	// Sync bot.townId to findNearestTown(bot.currentPos). No-op if detection fails
	// or already matches. Used at every site where currentPos changes without an
	// accompanying townId update (admin teleport, activateBot, restore, periodic
	// safety-net). See §54.5 in BOT_SYSTEM_DOCS for the desync bug this fixes.
	void syncTownIdToPos(BotState& bot);

	// Cast Chat debug logging — sends to cast viewers
	void castLog(BotState& bot, const std::string& msg);
	void castLogError(BotState& bot, const std::string& msg); // red text for failures
	std::string buildStatusDetail(BotState& bot); // build status string for heartbeat/look
	void logHeartbeat(BotState& bot); // periodic 60s status summary

	// Debug stream — heartbeat snapshot + event log for one bot (toggled via "debug on")
	void dbgEmitHeartbeat(BotState& bot, BotDebugCfg& cfg);
	void dbgEmitEvent(const BotState& bot, BotDebugCfg* cfg,
		const std::string& kind, const std::string& fields);
	std::string dbgRenderGrid(const BotState& bot);
	void dbgEmitMobList(const BotState& bot);
	// Spell-impact tracking
	std::vector<Position> dbgComputeAoeTiles(const Position& botPos, AoeAreaType areaType,
		Direction dir, int32_t areaSize, int32_t innerSize = 0);
	// Spell-aware overlay (uses matrix lookup when bound — accurate per parsed Lua arrays)
	std::vector<Position> dbgComputeAoeTiles(const Position& botPos, const ResolvedSpell& spell,
		Direction dir);
	void dbgRecordPreCast(BotState& bot, const std::string& descriptor,
		const std::vector<Position>& areaTiles);
	void dbgEmitPostCastIfDue(BotState& bot);
	std::string dbgHandleCommand(BotState* bot, const std::string& rest); // "debug ..." verb dispatch

	// Party system (Phase 9) — human-led party follow
	void doParty(BotState& bot);
	void doPartyFollow(BotState& bot);  // renamed from old doParty body
	void doPartyHealing(BotState& bot, const std::shared_ptr<Party>& party,
		const std::shared_ptr<Player>& leader);
	// minLevelOverride/maxLevelOverride: 0,0 keeps the derived playerLevel*2/3..*3/2 window;
	// non-zero comes from the /party [min,max] argument. preferActive demotes never-logged-in
	// (!active) candidates from FIRST choice to LAST — human-led recruitment consumes
	// already-logged-in bots first so a party does not ratchet the population upward, while
	// tier-1 stays available as the overflow pool a large quest party needs.
	std::vector<uint32_t> findBotsForParty(uint8_t baseVocId, uint32_t playerLevel,
		uint32_t count, const std::unordered_set<uint32_t>& excludeGuids,
		uint32_t minLevelOverride = 0, uint32_t maxLevelOverride = 0,
		bool preferActive = false);

	// ROUND2 E: shared by BOTH formation paths (live tryStartPartyHunt and virtual
	// virtualTryStartPartyHunt). The recruitment shape and the election order live here exactly
	// once so the live and virtual twins cannot drift — a divergence there ships a feature that
	// looks implemented while being inert for the ~95% of parties that form hibernated.
	struct PartyRoster {
		uint32_t ek = 0, ed = 0, ms = 0, rp = 0;
		uint32_t slotFor(uint8_t baseVoc) const {
			switch (baseVoc) { case 4: return ek; case 2: return ed; case 1: return ms; case 3: return rp; }
			return 0;
		}
		size_t supportCount(uint32_t leaderGuid) const {
			size_t n = 0;
			for (uint32_t g : { ek, ed, ms, rp }) { if (g != 0 && g != leaderGuid) n++; }
			return n;
		}
	};
	// Recruits one candidate of every base vocation EXCEPT the initiator's own, chaining the
	// exclusion set exactly as the old per-script loop did. Level window is the INITIATOR's: it is
	// the only level known before the leader is elected, and it keeps the whole roster mutually
	// level-compatible whoever wins.
	PartyRoster recruitPartyRoster(const BotState& initiator, int32_t initLevel,
		size_t* bestEd = nullptr, size_t* bestMs = nullptr, size_t* bestRp = nullptr);
	// EK > RP > initiator. Returns a guid that is always present in the roster or the initiator.
	uint32_t electPartyLeader(const PartyRoster& roster, uint32_t initiatorGuid);
	bool activateBotForParty(uint32_t guid, uint32_t leaderCreatureId, const Position& summonPos);
	void exitPartyMode(BotState& bot);

	// ---- BOT_PARTY_INVITE_RENDEZVOUS ----
	// Shared teardown: everything setupSupport does to free a bot from whatever it was doing
	// (hunt reservation + spawn group, AdvStone, fishing, house visit, ice fishing, travel
	// triplet, walk state, attacked creature). Extracted so the invite path, the assembly
	// deadline teardown, and the reclaim-to-inactive routing cannot drift apart.
	void releasePartyMemberActivity(BotState& bot, const char* reason);

	// Invite detection + acceptance (bot_party.cpp).
	void tickPartyInvites(int64_t nowMs);
	void onBotInvited(BotState& bot, const std::shared_ptr<Player>& inviter,
		const std::shared_ptr<Party>& party);
	bool acceptPartyInvite(BotState& bot, const std::shared_ptr<Player>& inviter,
		const std::shared_ptr<Party>& party);
	void declinePartyInvite(BotState& bot, const std::shared_ptr<Party>& party, const char* reason);

	// Assembly supervisor + enrolment (bot_party.cpp).
	void tickPartyAssembly(int64_t nowMs);
	// Drives an assembling member's walk from INSIDE its own per-tick pass, mirroring
	// handleGangStaging. Returns true = consumed the tick, so no other IDLE behaviour runs.
	bool handleAssemblyStaging(BotState& bot);
	uint32_t enrollHumanLedMember(BotState& bot, const std::shared_ptr<Player>& leader);
	bool assemblyActiveForPartyHunt(uint32_t partyHuntId) const;
	void dropAssemblyMember(uint32_t guid, const char* reason);
	RvPhase pickAssemblyEntryPhase(const BotState& bot, uint32_t anchorTownId) const;
	// Off-screen staging tile near `anchor` for a member that is too far to walk. Returns an
	// invalid position (x==0) when nothing off-screen and safe could be found.
	Position chooseAssemblyStagingPos(BotState& bot, const Position& anchor);
	// Decides a member's approach and performs the staging teleport if one is warranted. Single
	// entry point so enrolment, the FINISHING transition and the death-rejoin cannot diverge.
	void beginAssemblyApproach(BotState& bot, const PartyAssembly& asmb, RvMember& m);
	void failAssemblyMemberToTeleport(BotState& bot, const Position& anchor, const char* reason);

	// Autonomous party hunt system (Phase 10)
	bool tryStartPartyHunt(BotState& bot, int32_t forceScriptId = 0);
	// ROUND2 E: formation once the leader is elected and the roster recruited (see tryStartPartyHunt).
	// BOT_PARTY_LEAK_FIX: Canary Party lifetime = AWAKE party lifetime.
	// BOT_PARTY_CAP: may a new party of `prospectiveMembers` bots form right now?
	bool partyCapAllows(uint32_t prospectiveMembers, const char* path);
	void dematerializeCanaryParty(uint32_t partyHuntId, const char* reason);
	std::shared_ptr<Player> resolveBotPlayer(uint32_t guid);
	bool reclaimStaleCanaryParty(uint32_t guid, const char* site);
	void sweepStaleCanaryParties();
	bool formPartyWithLeader(BotState& leader, const std::shared_ptr<Player>& leaderPlayer,
		const PartyRoster& roster, uint32_t initiatorGuid, int32_t forceScriptId,
		size_t rosterBestEd, size_t rosterBestMs, size_t rosterBestRp);
	void dissolvePartyHunt(uint32_t partyHuntId, const std::string& reason);
	void exitPartyHuntMode(BotState& bot);
	void doPartyHunt(BotState& bot);
	void doPartyHuntHealer(BotState& bot, BotState* leaderBot);
	void doPartyHuntDpsMage(BotState& bot, BotState* leaderBot);
	void doPartyHuntDpsRanged(BotState& bot, BotState* leaderBot);
	void followPartyHuntLeader(BotState& bot, const std::shared_ptr<Player>& leader, BotState* leaderBot);
	bool tryPartyMemberSpread(BotState& bot, const Position& leaderPos, int32_t keepDist);
	bool tryPartyAoeReposition(BotState& bot, BotState* leaderBot);
	void tryCastChallenge(BotState& bot);
	bool tryCastUtamoVita(BotState& bot);  // mana shield for ED self-protection
	int32_t getPartyHuntKeepDistance(const BotState& bot, BotState* leaderBot) const;

	// BOT_PARTY_TRAIL_FOLLOW (implementation_plans/BOT_PARTY_TRAIL_FOLLOW.md)
	void recordLeaderTrails(int64_t nowMs); // tick-top: sample every wanted leader's footsteps
	void emitPtrailSummaryIfDue(int64_t nowMs); // unconditional 60s [PTRAIL] telemetry line
	// true = "I moved / am moving this tick" (caller returns, teleport skipped);
	// false = "give up, use the existing teleport". Reached ONLY from the teleport
	// branches of followPartyHuntLeader / doPartyFollow.
	bool tryFollowLeaderTrail(BotState& bot, const std::shared_ptr<Player>& leader);
	// Record-time ZHOP mechanism capture: curated waypoint type wins, else portal graph,
	// else synthetic INFERRED (portalResolved stays false).
	void resolveTrailZHopPortal(uint32_t leaderGuid, TrailNode& node);
	// Replays node's floor transition by seeding s_plannedFc and entering the existing FC
	// machine. Owns only the last mile (<=3 tiles to P starts a session; 3-6 walks in; >6
	// declines).
	TrailZHopResult executeTrailZHop(BotState& bot, const TrailNode& node, uint32_t leaderGuid);
	// Bounded leader wait: true = pause waypoint ADVANCEMENT this tick (doHuntPatrol keeps
	// fighting/luring above the gate). Bot party-hunt leaders only; humans are never held.
	bool partyLeaderShouldHoldForStragglers(BotState& bot);

	// Population scheduling
	void doPopulationManagement();

	// Autonomous activity reroll
	void doActivityReroll(BotState& bot);
	// BOT_ACTIVITY_PCT. The short dwell taken when a bin is ineligible or its attempt fails --
	// the mechanism that stops a failure donating its share to a neighbouring bin. Uses
	// botActivityFallbackDwellMin/MaxSec, deliberately shorter than a CHOSEN dwell.
	void rerollFallbackDwell(BotState& bot, const char* reason);
	// Terminal outcomes of one doActivityReroll invocation. Exactly one is recorded per
	// invocation, which is what makes the counters sum to the reroll total.
	enum class ActOutcome : uint8_t {
		Dwell = 0, Poi, PoiFail, Hunt, HuntFail, Party, PartyFail,
		Travel, CityWalk, TravelFail, RoundingTail
	};
	static void recordActOutcome(ActOutcome o) { s_actCum[static_cast<size_t>(o)]++; }
	std::string activityReport() const;
	// Sums TABLE A and complains loudly if it is not 100. Does NOT rescale (that would run
	// numbers the operator never wrote) and does NOT refuse to boot.
	void validateActivityTable(int32_t d, int32_t p, int32_t h, int32_t pa, int32_t t);
	void validatePoiTable();
	void validateHouseTable();
	// " -- TABLE INVALID (sum=NN, must be 100)" or "" -- appended to the botcfg section header.
	std::string activityTableStatusSuffix() const;

	// PZ-blocked roaming (Feature 2): while genuinely pz-locked, mill around non-PZ tiles
	// (turn-in-place / short walks) since depot/temple/boat PZs are unreachable. Returns true
	// if it consumed the tick (caller should not run other idle/dwell logic).
	bool handlePzRoam(BotState& bot);

	// Gang-PK alpha-strike (Feature 1).
	void checkGangJump(BotState& bot);        // initiator scan from doIdle
	bool handleGangStaging(BotState& bot);    // stage->barrier->burst; true = consumed the tick
	void maintainGangBox(BotState& bot);      // surround + escape-tile magic walls (from doPKAttack)
	void gangParalyzeIfFleeing(BotState& bot); // ED-only paralyze on a fleeing victim (from doPKAttack)
	bool pvpPlaceWallAt(BotState& bot, const Position& tile); // place a magic wall on a specific tile

	// State persistence helpers (used by saveAllStates, restoreAllStates, activateBot, forceDeactivateBot)
	void saveSingleBotState(const BotState& bot);
	void restoreSingleBotState(BotState& bot, std::shared_ptr<DBResult> result);

	// Tick frequency check
	bool isTickDue(const BotState& bot) const;

	// PZ-lock check (bot-tracked, since isPzLocked() doesn't work for bots)
	bool isBotPzLocked(const BotState& bot) const;

	// Population scheduling state
	int64_t lastPopulationTick_ = 0;
	int32_t populationJitter_ = 0;
	int32_t lastJitterHour_ = -1;
	bool schedulerEnabled_ = true;
	int64_t engineStartTime_ = 0;

	// Wake-burst stagger: 2026-05-27 — switched from counter+cap (only spread first 10
	// bots) to GUID-based deterministic stagger. Each woken bot picks wakeQuietTicks
	// from (guid * 7) % WAKE_STAGGER_SPREAD_TICKS + WAKE_QUIET_BASE_TICKS, so 200 bots
	// in the same wake burst spread their first AI tick deterministically across the
	// configured window. 30 ticks × 200ms engine tick = 6 second spread, avg 3s.
	uint8_t recentWakeStagger_ = 0;  // legacy counter kept for diagnostics
	static constexpr uint8_t WAKE_QUIET_BASE_TICKS = 3;
	static constexpr uint8_t WAKE_STAGGER_CAP = 10;  // legacy counter cap (unused for stagger now)
	static constexpr uint8_t WAKE_STAGGER_SPREAD_TICKS = 30;  // GUID-stagger spread (30 ticks @ 200ms = 6s)
	// BOT_LIVENESS wake-realism (2026-06-13): proximity-path wakes (walk-by / cast-watch,
	// ≤5 bots/300ms via the Lua proximity loop) use a MUCH shorter quiet window so bots
	// near a viewer start acting in ~200-800ms instead of 0.6-6.4s. The big GUID stagger
	// above is retained for TELEPORT/EXPLICIT mass-wakes (login bursts) where 200 bots
	// would otherwise run first-AI on one dispatcher window (the 5s GAP_SLOW cascade).
	// The small spread (×4) still decorrelates the ≤5 bots in a single proximity tick.
	static constexpr uint8_t WAKE_QUIET_PROX_BASE_TICKS = 1;    // 200ms floor
	static constexpr uint8_t WAKE_QUIET_PROX_SPREAD_TICKS = 4;  // +0..3 ticks → 200-800ms

	// Wake-burst occupancy dedup: tiles reserved by chooseWakePosition in the current
	// wake burst. Cleared by beginWakeBurst() at the start of wakeBotsInRadius and
	// wakeAllHibernatedBots. Defends against the case where two bots in the same burst
	// both pick the same spread tile if their chooseWakePosition calls overlap before
	// either is placed. Sonnet-reviewed: mutable so chooseWakePosition (const) records.
	mutable std::unordered_set<uint64_t> burstReservedTiles_;
	void beginWakeBurst() { burstReservedTiles_.clear(); }

	// PERF_INVESTIGATION_2026-05-24 Phase B (2026-06-01): density-capped wake.
	//
	// Anchor clustering: real players + cast-watched bots within
	// botDensityAnchorClusterRadius tiles of each other merge into a single
	// AnchorCluster at the centroid. Per cluster, three concentric ring counters
	// track how many bots are currently awake within each ring (inner/mid/outer).
	// shouldGateWake checks the bot's currentPos against each cluster's rings
	// innermost-first; if any ring is at cap, the wake is gated.
	//
	// Thread safety: all access from the dispatcher thread (BotEngine::tick,
	// wakeBot, wakeBotsInRadius). No atomic needed.
	//
	// Refresh cadence: top of BotEngine::tick (100ms) and top of wakeBotsInRadius
	// (if cache older than 50ms). Cache lifetime is short enough that player
	// movement (max ~5 tiles/sec) cannot meaningfully invalidate it.
	struct AnchorCluster {
		Position centroid;
		uint32_t anchorCount = 0;
		std::array<uint32_t, 3> counts = {0, 0, 0};  // [inner, mid, outer] currently-awake bot counts
		std::array<uint32_t, 3> peakCounts = {0, 0, 0};  // peak since last [DENSITY] periodic log
		// BOT_AMBIENT_ROAM: the subset of `counts` that is roam-ledgered. A roamer is counted in
		// BOTH arrays, and the organic gate arm subtracts this — that subtraction is what makes
		// the reserve a true addition ("inner 3+3") rather than a redistribution. Attribution is
		// by cluster CENTROID for every array and every gate arm; mixing centroid counting with
		// nearest-raw-anchor gating would index one array under two different partitions.
		std::array<uint32_t, 3> roamCounts = {0, 0, 0};
		// Whether ANY member of this cluster is a roam-eligible anchor, and which one. Recorded at
		// build time from the member list — see the note there for why a radius proxy against the
		// centroid is not equivalent. Roam targets THIS position rather than the global-nearest
		// eligible anchor, so a roamer can never be injected for one cluster and charged to another.
		bool hasRoamAnchor = false;
		Position roamAnchor;
	};
	std::vector<AnchorCluster> currentAnchors_;
	// Raw (unclustered) anchor positions from the same refresh — used by the
	// outerLimitPct=0 hard band rule in shouldGateWake. Centroids are the wrong
	// geometry for an absolute no-wake verdict: per-cluster band checks dead-zone
	// bots standing next to a SECOND player 51-100 tiles from the first, and
	// merged-cluster centroid displacement both over- and under-gates near edge
	// anchors (2026-06-12 adversarial review blocker).
	std::vector<Position> currentAnchorPts_;
	// BOT_AMBIENT_ROAM: the subset of currentAnchorPts_ that ambient roamers may serve.
	//
	// Deliberately a SEPARATE list rather than a filter on the one above. currentAnchorPts_ feeds
	// wouldBeSeenByAnchor and the density cap, and both must keep seeing EVERY anchor: a hunting
	// player is still a pair of eyes, so staging must still avoid their screen even though no
	// roamer should be sent to them. Removing them from the shared list would let roamers pop into
	// view of the very people this suppression exists to leave alone.
	std::vector<Position> roamAnchorPts_;

	// BOT_AMBIENT_ROAM: the COMPLEMENT of the list above — every anchor that is working a spawn
	// right now. Eligibility alone is not enough to keep wanderers out of a spawn, because it only
	// decides which anchor an injection is RAISED FOR. Staging is bounded by the cluster centroid
	// (midRadius = 50) and legs wander freely inside the roam region, so a roamer serving a
	// perfectly eligible anchor can still be placed, or walk, straight into a hunt being worked
	// 40 tiles away — a different failure from the one roamAnchorPts_ prevents, and one that a
	// per-cluster eligibility flag cannot see at all, since the two anchors need not share a
	// cluster. Geometry is therefore enforced separately: no staging tile, and no leg destination,
	// may land within ROAM_SUPPRESS_KEEPOUT of any of these, and a roamer already inside one when
	// a hunt starts is retired out.
	//
	// A STRICT SUBSET of the ineligible anchors, not their complement: a quest walkthrough patrols
	// with the same phase as a spawn but is an authored route through towns and roads, so it stops
	// ATTRACTING wanderers (targeting) without dragging a 30-tile no-roam bubble across a city
	// (geometry). Everywhere else that distinction matters the codebase draws it the same way —
	// see the `!script->isQuest` guards on the two hunt-target name matches.
	std::vector<Position> roamSuppressedPts_;
	// Wider than a viewport (a bot 12 tiles away is still pulling the same monsters) and narrower
	// than the 50-tile cluster radius, so a player standing at the far edge of a hunter's cluster
	// keeps their own wanderers. Not a config key: this is the geometry of a spawn, not an
	// operator preference, and every roam tunable that IS one lives in config.lua already.
	static constexpr int32_t ROAM_SUPPRESS_KEEPOUT = 30;
	bool roamInKeepout(const Position& p) const;
	// Live gauge, refreshed every anchor rebuild, NOT a running total: a suppressed anchor is
	// skipped silently on every one of the ten ticks a second it exists, so a counter would report
	// the duration of one hunt as thousands of events. Same lesson as the z-planner deferral
	// telemetry (4e33a49d2) — report what is true now.
	uint32_t roamSuppressedNow_ = 0;

	// Positions of hunt-flagged players who are OUT of town. The roam suppression stops wanderers
	// arriving; this stops the engine steering HUNTERS at them, which it otherwise actively does —
	// the hunt selector weights candidate spawns by proximity to anchors, so a claimed player is a
	// magnet by default. Players only; a patrolling bot already holds its spawn through
	// activeHunts_ and needs no repulsion.
	std::vector<Position> huntRepelPts_;
	// Radius around such a player inside which a spawn stops being offered. Matches the density
	// cap's mid ring, i.e. roughly "the neighbourhood you can see trouble coming from".
	static constexpr int32_t HUNT_REPEL_TILES = 50;
	bool isScriptHuntRepelled(const HuntScript& s) const;
	// Repel telemetry. Without it an inert gate is indistinguishable from a gate with nothing to
	// do — which is exactly how the clear-after-populate bug survived both writing and a first
	// review, until someone read the execution order. Reported in the 60s [ROAM] summary.
	mutable uint32_t huntRepelEvaluated_ = 0, huntRepelRejected_ = 0;
	// Peak repel-point count over the reporting window. `pts` alone is instantaneous while eval and
	// rejected accumulate, so a window could legitimately print "pts=0 eval=1862" — the gate ran
	// 1862 times and the flag then lapsed before the line was emitted. That reads as a
	// contradiction, which is the last thing this particular counter should read as.
	uint32_t huntRepelPtsPeak_ = 0;
	int64_t anchorsRefreshedAt_ = 0;
	void refreshAnchorsIfStale(int64_t maxAgeMs);
	bool shouldGateWake(uint32_t guid);

	// Data
	std::vector<BotState> bots_;
	std::unordered_map<uint32_t, size_t> guidToIndex_; // guid -> index in bots_
	std::unordered_map<uint32_t, std::shared_ptr<Player>> dyingBots_; // guid -> player ref during death pause

	// Hibernation pool: keeps the Player object alive while the bot is hibernated, so
	// wakeBot doesn't need a synchronous DB load (the bottleneck causing map-lag during
	// player-traversal wake bursts). Player is unlinked from g_game().getPlayers() and
	// from the world map, but its in-memory state (inventory, stats, conditions, etc.)
	// is preserved. ~5MB/bot × 200 = ~1GB extra RAM steady-state.
	std::unordered_map<uint32_t, std::shared_ptr<Player>> hibernationPool_;

	// Hunt data (loaded from MySQL at startup)
	std::vector<HuntScript> huntScripts_;
	std::unordered_map<uint32_t, uint32_t> activeHunts_;  // scriptId -> botGuid
	std::unordered_map<std::string, uint32_t> activeSpawnGroups_; // spawnGroup -> botGuid

	// Player spawn-claim (in-memory only; lost on /cavebot reload + restart — by design).
	// A real player standing in a hunt spawn can claim it: kicks the bot reserving it and
	// blocks bot assignment of that script (and its spawnGroup) for 1h. See isScriptPlayerClaimed.
	struct PlayerSpawnClaim {
		uint32_t guid = 0;          // owner player guid (stable; NOT the transient creature id)
		std::string ownerName;      // cached for messages/listing
		uint32_t scriptId = 0;
		std::string spawnGroup;     // may be empty
		int64_t expiresAt = 0;      // absolute OTSYS_TIME() ms deadline
	};
	std::unordered_map<uint32_t, PlayerSpawnClaim> playerClaims_;   // scriptId -> claim
	std::unordered_map<uint32_t, int64_t> lastClaimByGuid_;         // ownerGuid -> last successful claim ms (anti-grief)
	// A real player who has told us they are hunting, whether or not the claim resolved to a known
	// script. The user's rule is deliberately broad: someone who types the claim command has
	// announced intent, and a typo or an unmapped spawn should not cost them the quiet they asked
	// for. Same 1h lifetime as a claim, swept by the same pass.
	std::unordered_map<uint32_t, int64_t> playerHuntEngaged_;       // ownerGuid -> expiry ms
	// Chebyshev distance from a town temple that still counts as "in town", where ambient traffic
	// is wanted even while a hunt is claimed. Only past this does the suppression bite.
	// A TEMPLE PLAZA radius, not a city radius. 60 was wrong by a wide margin: a player hunting in
	// open terrain 46 tiles from the Silvertides temple, with no protection zone and zero NPCs
	// around, was classified as standing in town and never suppressed. Town identity is carried by
	// rules 1 and 3 (protection zone, NPC cluster) — the ones that describe what a town actually
	// IS — and this rule now only covers the square immediately around a temple, where those two
	// can both miss on a bare paved tile.
	static constexpr int32_t ROAM_TOWN_RADIUS = 25;
	// "Shopkeepers are around me." Deliberately tighter than the temple radius: a lone quest NPC
	// parked in a dungeon must not turn that dungeon into a town.
	static constexpr int32_t ROAM_TOWN_NPC_RADIUS = 30;

	// ---- Cast-chat roam digest ----
	// A cast viewer can see bodies arrive and leave but has no idea WHY, so the roam layer is
	// invisible to exactly the audience it exists for. The clutter risk is real, so the emission
	// rule is deliberately conservative: report on CHANGE (a roamer arrived or left, which is the
	// genuinely interesting moment) and otherwise heartbeat once a minute. A static scene therefore
	// costs one line per minute regardless of how many roamers are in it, and nothing at all when
	// there are none.
	//
	// The roster hash covers WHO is nearby, not what each is doing — hashing tasks too would fire a
	// line on every leg change, which at three roamers is a line every few seconds and precisely
	// the clutter this shape avoids. Current tasks still ride the line whenever it is emitted.
	static constexpr int64_t ROAM_CAST_HEARTBEAT_MS = 60000;
	static constexpr int32_t ROAM_CAST_RADIUS = 30;
	static constexpr size_t ROAM_CAST_MAX_LISTED = 4;
	std::unordered_map<uint32_t, std::pair<uint64_t, int64_t>> roamCastLast_;  // watched guid -> {rosterHash, lastEmitMs}
	std::unordered_set<uint32_t> roamCastMuted_;                               // /cavebot <bot> roamcast off
	void tickRoamCastDigest(int64_t nowMs);
	std::string roamTaskBrief(const RoamRun& run) const;
	bool isInTownArea(const Position& p) const;
	bool isPlayerHuntEngaged(uint32_t guid) const;
	// BOT_AMBIENT_ROAM: "is this bot working a monster spawn right now" — the question the roam
	// suppression has always meant to ask. Its own hunt phase answers it for a solo hunter and for
	// a party LEADER; a party MEMBER has no hunt of its own and is answered by its leader.
	// outScript, when given, receives the script actually being worked (the LEADER's for a party
	// member) so the caller can tell a monster spawn from a quest walkthrough without repeating the
	// leader derivation. Left null on a false return.
	bool isBotSpawnEngaged(const BotState& b, const HuntScript** outScript = nullptr) const;
	uint32_t partyLeaderWorkedScriptId(const BotState& b) const;
	void markPlayerHuntEngaged(uint32_t guid);
	static constexpr int64_t PLAYER_CLAIM_DURATION_MS = 3600 * 1000LL; // 1 hour
	static constexpr int64_t PLAYER_CLAIM_COOLDOWN_MS = 30 * 1000LL;   // anti-grief: min gap between a player's claims
	static constexpr int32_t PLAYER_CLAIM_MAX_DIST = 8;               // Chebyshev tiles from a patrol wp to count as "inside" the spawn
	static constexpr int32_t PLAYER_CLAIM_AMBIGUITY_SLACK = 4;        // runner-up within this many tiles of best => ambiguous

	// Area matrix table (parsed from data/scripts/lib/register_spells.lua at engine init).
	// Used by isInAreaMatrix for exact-source-of-truth AOE hit prediction.
	std::unordered_map<std::string, AreaMatrix> areaMatrices_;

	// City POIs (loaded from MySQL at startup)
	std::unordered_map<uint32_t, std::vector<BotPOI>> cityPOIs_; // townId -> POIs

	// City route graphs (loaded from MySQL at startup)
	std::unordered_map<uint32_t, CityRouteGraph> cityRouteGraphs_; // townId -> graph

	// BOT_NAV_REALISM Phase 8 increment 1: NPC approach anchors, built at runtime from live NPCs.
	// Solves "the shopkeeper stands behind a counter you cannot walk onto": for each NPC we
	// precompute the tiles a bot may legally stand on to talk to it (walkable, within talk range,
	// clear line of sight). Keyed by NPC name -> instances, so same-named NPCs in different towns
	// coexist and callers pick the nearest. Rebuilt on every /cavebot reload via loadHuntData().
	struct NpcAnchor {
		Position npcPos;
		uint32_t townId = 0;
		std::vector<Position> approachTiles; // ranked nearest-first, K <= 6
	};
	std::unordered_map<std::string, std::vector<NpcAnchor>> npcAnchors_;
		// townId -> anchored NPC names in that town. Lets the POI picker offer "visit a random NPC"
	// as ONE candidate slot without creating bot_city_pois rows: the anchors are already
	// rebuilt every reload, so this index is free and can never drift from them.
	std::unordered_map<uint32_t, std::vector<std::string>> npcNamesByTown_;
	// Debug-pinned bots: never self-assign work (doActivityReroll returns early) and never
	// hibernate (the Lua hibernation loop skips them via `pin status`). Set by
	// `/cavebot <name> pin on`, which `reload debug,<names>` issues for each debug bot.
	// A member rather than a BotState field: BotState is the ABI boundary.
	std::unordered_set<uint32_t> s_debugPinned;
	// PERF STRESS HARNESS: probe bots. A probe is an ordinary bot flagged as if a human were
	// cast-watching it (Player::setSyntheticCastViewers), which makes every observer gate in the
	// engine AND in bot_hibernation.lua treat it as a camera -- so it wakes neighbours, exempts
	// them from the AI budget and arms the observed-tier behaviours, exactly as a real player
	// walking around would. Probes are also pinned, so they never wander off their itinerary.
	// A member for the same reason s_debugPinned is one: BotState is the ABI boundary.
	std::unordered_set<uint32_t> s_probeBots;
	// guid -> NPC name this bot is walking to for a social visit, so the arrival handler knows
	// who to greet. A member (not a BotState field) because BotState is the ABI boundary; the
	// POI picker and the IDLE arrival handler live in different TUs but share this one instance.
	std::unordered_map<uint32_t, std::string> s_pendingNpcVisit;
	void buildNpcApproachAnchors();

	// Phase 8 increment 2: pick a legal tile to stand on to talk to `npcName`, claiming it so no
	// other bot walks to the same spot. Returns false only when the NPC has no anchors at all.
	// `outIsFallback` reports that every proper approach tile was taken and we settled for a
	// nearby tile without line of sight (walk-up-and-wait) — never a teleport.
	bool resolveNpcApproach(const BotState& bot, const std::string& npcName,
	                        Position& outTile, bool& outIsFallback);
	// Drop every approach-tile claim held by this bot (arrival, abort, hibernate, death).
	void releaseNpcApproach(uint32_t guid);

	// BOT_LIVENESS_PACK Phase C.3: safe-fidget item allowlist, loaded once at engine
	// init from bot_market_item_prices WHERE marketable=1 AND market_max > 0 AND
	// market_max < botFidgetMaxItemValueGp. Quest / soulbound / untradeable items
	// have marketable=0 in the catalog so they're auto-excluded.
	std::vector<uint16_t> safeFidgetItemIds_;

	// BOT_LIVENESS_PACK perf hotfix: g_configManager().getNumber() takes a mutex
	// per call, which at 500 bots × 5Hz × ~8 reads per tickLivenessBehaviors
	// produced ~22% CPU just in config plumbing (gdb caught the dispatcher in
	// ConfigManager::getInstance at saturation). Cache all liveness-pack config
	// values in a struct, refresh every 5s. All hot-path reads now consult the
	// cache, not g_configManager.
	struct LivenessCfg {
		int32_t poiWeightDepot = 40, poiWeightDepotOutside = 20, poiWeightTemple = 10;
		int32_t poiWeightBoat = 20, poiWeightShop = 10, poiWeightNpc = 8, poiWeightAdvStone = 10;
		// Master switch for the scoped route planner (see BOT_NPC_VISIT_PCT). 0 = the NPC
		// candidate is never even sampled, so the feature costs nothing while disabled.
		int32_t npcVisitPct = 0;
		// BOT_SUPPLY_REALISM. Same "0 = never sampled" contract as npcVisitPct for fishPct.
		int32_t poiWeightWater = 15;
		// BOT_HOUSE_VISIT. Same contract again for houseVisitPct.
		int32_t poiWeightHouse = 0;
		int32_t houseVisitPct = 0;
		int32_t houseIdleMinSec = 120, houseIdleMaxSec = 600;
		int32_t houseMaxDist = 150, houseMaxOccupants = 2;
		int32_t houseSettleSec = 45;
		// BOT_SHRINE_IDLE. Same "0 = never sampled" contract again for shrineVisitPct. The two
		// weights are TABLE B rows and are included in validatePoiTable's sum-to-100.
		int32_t poiWeightRewardShrine = 0, poiWeightImbuingShrine = 0;
		int32_t shrineVisitPct = 0;
		int32_t shrineMaxOccupants = 2;
		int32_t shrineIdleMinSec = 60, shrineIdleMaxSec = 240;
		int32_t shrineSettleSec = 45;
		// BOT_ACTIVITY_PCT TABLE C. Replaced the single houseSubActivityPct dial.
		int32_t houseIdlePct = 26, houseHirelingPct = 20, houseDummyPct = 20, houseLockerPct = 19;
		int32_t houseShrinePct = 15;
		int32_t fishPct = 0;
		int32_t fishMaxDist = 150, fishMaxDz = 7;
		int32_t fishCastIntervalMinMs = 1000, fishCastIntervalMaxMs = 2000;
		int32_t fishDurationMinSec = 120, fishDurationMaxSec = 420;
		int32_t fishMaxSpotsPerTown = 64, fishZBand = 1;
		int32_t potionChancePct = 8, potionMinIntervalMs = 20000;
		int32_t foodChancePct = 6, foodMinIntervalMs = 60000;
		int32_t runeCraftChancePct = 10;
		int32_t runeCraftIntervalMinMs = 8000, runeCraftIntervalMaxMs = 25000;
		bool runeCraftRefillMana = true;
		int32_t supportSpellChancePct = 35;
		int32_t supportSpellIntervalMinMs = 15000, supportSpellIntervalMaxMs = 45000;
		int32_t dwellRerollMinSec = 60, dwellRerollMaxSec = 300;
		int32_t dwellPoiMinSec = 180, dwellPoiMaxSec = 900;
		int32_t dwellNpcMinSec = 15, dwellNpcMaxSec = 60;
		int32_t dwellPostTravelSec = 60;
		int32_t advStoneDwellIdleMinSec = 60, advStoneDwellIdleMaxSec = 300;
		int32_t advStoneDwellChestMinSec = 300, advStoneDwellChestMaxSec = 1200;
		int32_t advStoneDwellDummyMinSec = 180, advStoneDwellDummyMaxSec = 1800;
		int32_t mountChancePct = 60;
		int32_t poiCrowdCapCount = 3, poiCrowdCapRadius = 2;
		// Player-proximity weighting (2026-06-15) — bias hibernated bots' next task/location
		// toward routes/towns near real-player / cast-watched-bot anchors. See §BOT_PLAYER_PROXIMITY_WEIGHTING.
		bool proxEnabled = true, proxAwake = false;
		int32_t proxBaselineWeight = 1, proxNearTiles = 100, proxMidTiles = 350;
		int32_t proxBonusNear = 18, proxBonusMid = 6, proxSampleCap = 8, proxTravelCatBonus = 25;
		int32_t turnInPlaceChancePct = 20, turnInPlaceIntervalTicks = 25;
		int32_t walkPauseChancePct = 2, walkPauseMinMs = 400, walkPauseMaxMs = 5000, walkPauseMaxPerRoute = 3;
		int32_t walkPauseObservedChancePct = 12, walkPauseObservedMinMs = 500, walkPauseObservedMaxMs = 20000, walkPauseObservedMaxPerRoute = 8;
		int32_t fidgetChancePct = 25, fidgetIntervalMinSec = 60, fidgetIntervalMaxSec = 300;
		int32_t fidgetMaxItemValueGp = 500;
		int32_t chatCooldownMinMs = 30000, chatCooldownMaxMs = 300000;
		int32_t worldChatIntervalMinMs = 1200000, worldChatIntervalMaxMs = 2400000;
		int32_t advertisingIntervalMinMs = 300000, advertisingIntervalMaxMs = 600000;
		int32_t chatAntiRepeatRingSize = 8;
		int32_t chatMasterChancePct = 100;  // Fix #10: master "try to talk" gate
		bool chatVerboseLog = false;
		bool telemetryEnabled = false;  // gate best-effort bot_chat_emissions writes
		bool personalityRerollOnRestart = true;
		bool hibernatedChatEnabled = true;  // Fix #11: hibernated bots emit channels
		// Count of non-bot, non-removed Players (i.e. real human players online).
		// Refreshed alongside config every 5s. Used to gate channel chat: when 0
		// real players are online, channel posts have no human reader, so we
		// silence them entirely to save dispatcher cycles. Cast viewers watching
		// a bot still see local-say chat from awake bots they're watching.
		int32_t realPlayerCount = 0;
		int64_t lastRefreshMs = 0;
	};
	mutable LivenessCfg livenessCfg_;
	// Non-const: also re-staggers bot chat timers on realPlayerCount 0->N
	// transition (legacy login-flood fix, 2026-06-04). The cached LivenessCfg
	// mutation alone would still fit a const + mutable cache pattern, but
	// re-jittering bots_ doesn't.
	void refreshLivenessCfgIfStale(int64_t maxAgeMs = 5000);

	// ---- BOT_CORPSE_LOOT (see the block above LootRun in this header) ----
	// Shared gate preamble for all three passes, so the "never while a real player is
	// in the party" belt provably covers every one of them.
	bool lootGatePasses(BotState& bot, const std::shared_ptr<Player>& player);
	bool lootOwnerIsOurs(const std::shared_ptr<Player>& player, uint32_t ownerId);
	bool lootRealPlayerOnScreen(BotState& bot, const std::shared_ptr<Player>& player);
	bool botMayOpenCorpse(BotState& bot, const std::shared_ptr<Player>& player,
		const std::shared_ptr<Container>& corpse);
	void openCorpse(BotState& bot, const std::shared_ptr<Container>& corpse,
		const Position& at, const char* mode);
	void tickCorpseCensus(BotState& bot);
	bool tickCorpseOpenAdjacent(BotState& bot);
	bool tickCorpseWalk(BotState& bot);
	bool lootStepTo(BotState& bot, const std::shared_ptr<Player>& player, const Position& target);
	// cancelWalk stops the queued route too. Ending the RUN is not enough: the bot keeps
	// walking to a corpse we already gave up on, and while that route drains it also
	// blocks combat's own retreat (chaseTarget yields to a non-empty listWalkDir exactly
	// like we do). Arrival is the one ending that must NOT cancel.
	void endLootRun(BotState& bot, bool blacklist, bool cancelWalk = true);
	void clearLootState(uint32_t guid);

	// BOT_CHAT_LIVENESS_V2: chat template corpus, loaded from
	// data/bot_chat/phrases.json at engine init / reload. Categories: idle,
	// banter (ex-world_chat), combat, flee, depot, npc_greet, travel,
	// advertising. Each bucket: per-category emission chance + template list.
	// Templates support %placeholder substitution (%city, %town, %vocation,
	// %item, %price, %stack, %creature, %spawn, %level). Advertising templates
	// carry a parallel intent tag so a bot stays in its buy/sell lane
	// (personalitySeed parity) instead of selling X then buying X back.
	struct ChatBucket {
		uint8_t chance = 0;
		std::vector<std::string> templates;
		// Parallel to templates; only populated for advertising.
		// 0 = neutral, 1 = sell, 2 = buy. Empty vector = all neutral.
		std::vector<uint8_t> intents;
	};
	std::unordered_map<std::string, ChatBucket> chatCatalog_;

	// Curated trade item list for Advertising %item/%price substitution.
	// v2: loaded from phrases.json "trade_items" (103 curated entries with
	// community aliases + live-market price ranges); the old 30-item hardcoded
	// table remains as fallback when the JSON lacks the section.
	struct TradeEntry {
		std::string name;
		int32_t priceMinGp;
		int32_t priceMaxGp;
		uint8_t intent = 0;  // 0=both, 1=sell-only, 2=buy-only
		std::vector<std::string> aliases;  // "ssa", "mpa", ... used ~40% of rolls
	};
	std::vector<TradeEntry> tradeCatalog_;

	// BOT_CHAT_LIVENESS_V2: chat tunables from phrases.json _meta.config.
	// Deliberately NOT in config.lua / LivenessCfg: new ConfigManager keys live
	// in the main binary and would force a full rebuild; this struct reloads
	// with the corpus on /cavebot reload.
	struct ChatCfg {
		uint8_t worldChatMode = 1;       // 0=off, 1=local (default), 2=channel (legacy)
		int32_t banterSharePct = 60;     // local slot: % of attempts using banter vs idle
		int32_t dedupWindowAdvertisingMs = 300000;
		int32_t dedupWindowBanterMs = 600000;
		int32_t playerThrottleMinMs = 20000, playerThrottleMaxMs = 45000;
		int32_t replyChancePct = 60;     // local-say keyword reply chance
		int32_t pmReplyChancePct = 90;   // PM reply chance
		int32_t replyDelayMinMs = 1500, replyDelayMaxMs = 5000;  // human typing delay
		int32_t replyCooldownPerBotMs = 60000;
	};
	ChatCfg chatCfg_;


	// BOT_PVP_REALISM: tunables for realistic PvP movement + field-rune play.
	// Hot-reloadable from phrases.json _meta.config (same mechanism as ChatCfg —
	// no config.lua key / full rebuild needed). Each enable* flag independently
	// disables a behavior at runtime via /cavebot reload so PvP regressions can be
	// bisected without a server restart. All defaults ON.
	struct PvpCfg {
		bool enableReposition = true;  // distance-voc kite away from the human attacker
		bool enableDance = true;       // knight melee dance + mage lateral strafe
		bool enableHaste = true;       // cast best-available haste on flee / low-HP
		bool enableMagicWall = true;   // place magic wall runes (block escape / pursuit)
		bool enablePzAwareFlee = true; // don't route to an unreachable PZ while pz-locked
		bool enableAoeBias = true;     // bias scoring toward AoE/wave vs a lone player
		int32_t mageKeepDist = 3;      // sorcerer/druid desired PvP distance
		int32_t paladinKeepDist = 4;   // paladin desired PvP distance
		int32_t danceChancePct = 18;   // per-eligible-tick chance to dance/strafe
		int32_t danceCooldownMs = 900; // min interval between dance steps
		int32_t hasteHpPct = 50;       // cast haste in combat below this HP%
		int32_t hasteCooldownMs = 4000;// min interval between haste attempts
		int32_t fleeHpPct = 30;        // COMBAT->FLEEING when HP drops below this %
		int32_t wallChancePct = 70;    // chance to place a wall when the trigger fires
		int32_t wallCooldownMs = 5000; // min interval between wall placements per bot
		int32_t aoeBiasPct = 35;       // % score bonus applied to AoE/wave vs a player
	};
	PvpCfg pvpCfg_;

	// BOT_PVP_REALISM: per-baseVoc haste spell words, strongest-castable first.
	// Resolved at init from the server spell registry (vocation gating). Index by
	// baseVoc 1-4; entry 0 unused.
	std::vector<std::string> resolvedHaste_[5];

	// Lexicon for %creature/%spawn level-gated substitution (fallback when the
	// bot has no active hunt). level_min/max of 0 = unknown bracket.
	struct LexCreature { std::string name; int32_t levelMin = 0, levelMax = 0; };
	struct LexSpawn { std::string name; std::string town; int32_t levelMin = 0, levelMax = 0; };
	std::vector<LexCreature> lexCreatures_;
	std::vector<LexSpawn> lexSpawns_;

	// Phase F keyword replies: trigger group -> reply lines.
	std::unordered_map<std::string, std::vector<std::string>> replyCatalog_;

	// Phase C global anti-repeat: hash of final RENDERED lowercased text ->
	// last emit OTSYS_TIME. Scoped to advertising + banter only (short social
	// tokens like "gg" must stay repeatable across bots). Pruned on insert;
	// resets on /cavebot reload (fresh engine instance) by design.
	std::unordered_map<uint32_t, int64_t> recentGlobalChat_;

	// Phase D per-player flood throttle: real player creature ID -> last time
	// ANY bot emitted ambient local chat near them. Pruned on access.
	std::unordered_map<uint32_t, int64_t> playerChatThrottle_;

	// Phase F pending replies (delayed so replies feel typed, not instant).
	struct PendingReply {
		int64_t fireAtMs = 0;
		uint32_t botGuid = 0;
		uint32_t playerId = 0;   // creature id of the player being answered
		bool isPm = false;
		std::string group;       // replyCatalog_ key
	};
	std::vector<PendingReply> pendingReplies_;
	void processPendingReplies(int64_t now);
	static std::string classifyReplyTrigger(const std::string& text, bool isPm);

	// Adventurer's Stone shared route (single global route, town_id=0 in DB)
	std::vector<Waypoint> adventurerStoneRoute_;

	// Travel positions (loaded from data/bot/authored/travel_positions.csv at startup,
	// joined against town_mapping.csv)
	// Multiple positions per town supported (e.g. Farmine boat + carpet, Kazordoon boat + carpet)
	// Each entry is {position, routePOI} where routePOI is "boat" or "carpet"
	std::unordered_map<uint32_t, std::vector<std::pair<Position, std::string>>> travelPositions_;
	std::unordered_map<uint32_t, std::string> travelTownNames_; // townId -> source_name (e.g. "Edron")

	// All-to-all travel destination map. Built once at end of loadTravelPositions() from
	// (travelPositions_ ∩ cityRouteGraphs_ with boat→depot or carpet→depot).
	// Replaces the legacy hardcoded neighbor allowlist. Bots virtualize transit, so any
	// (src, dest) pair works as long as src has travel_positions and dest has both
	// travel_positions AND a boat→depot/carpet→depot city route for walk_from_boat.
	std::unordered_map<uint32_t, std::vector<uint32_t>> travelDestinationsCache_;

	// VT_LAG false-positive fix (2026-06-08): per-bot last virtualTick advance timestamp.
	// Was a static local inside virtualTick(); moved to instance member so hibernateBot
	// can reset it on awake→hibernated transition. Without the reset, a bot that woke
	// (player nearby) for N minutes then re-hibernated would trigger a spurious N-minute
	// VT_LAG on its next virtualTick visit, because virtualTick skips non-hibernated
	// bots and so the stored timestamp froze at the last hibernated-visit time.
	std::unordered_map<uint32_t, int64_t> lastVirtualAdvanceMs_;

	// Liveness diagnostics (2026-06-09): wake-outcome counters over a sliding 60s window.
	// Reset by [PROXIMITY] periodic log every 60s. Tells us, when the user observes few
	// awake bots at a depot/temple, whether wakes are being TRIED at all (population
	// issue), GATED by density cap (cap saturated), or silently failing in placement
	// (chooseWakePosition/walkBack returned a bad position).
	uint32_t wakeTried60s_ = 0;
	uint32_t wakeGated60s_ = 0;
	uint32_t wakeGranted60s_ = 0;
	int64_t wakeStatsWindowStartMs_ = 0;

	// HUNT phase-transition diagnostics (2026-06-10). Triangulates why hibernated bots
	// appear to freeze in HUNT state overnight despite virtualAdvanceHunting being called
	// every virtualTick visit. huntPhaseEnteredMs_ tracks when each bot last changed
	// huntPhase; lastHuntStuckWarnMs_ rate-limits per-bot stuck warnings so a chronic
	// stuck bot doesn't spam. Captures (a) when transitions actually fire (info line)
	// and (b) which bots have been frozen in a phase > 30 min (warn).
	std::unordered_map<uint32_t, int64_t> huntPhaseEnteredMs_;
	std::unordered_map<uint32_t, int64_t> lastHuntStuckWarnMs_;

	// advanceWaypointIdx leftover-ms accumulator (2026-06-10 fix). Was: integer-floor
	// `steps = elapsed_ms / 5000` discarded sub-5s remainders, freezing the index when
	// per-bot elapsed_ms ≈ 100ms (observed virtualTick throughput). Now: accumulate the
	// fractional ms across visits; after 50 visits at 100ms each, advance 1 wp. Per-bot
	// (single key). Leftover bleeds across hunt-phase boundaries (up to 4999ms head-start
	// on first wp of new phase) — bounded approximation; cleared on phase transition in
	// virtualAdvanceHunting end-of-fn. ~4KB at 500 bots. Sonnet-reviewed.
	std::unordered_map<uint32_t, int64_t> waypointLeftoverMs_;

	// Equipment data (loaded from data/bot/authored/equipment.csv)
	// Key = level * 10 + baseVocation (e.g. level 50, voc 1 → 501)
	std::unordered_map<uint32_t, BotEquipment> equipmentData_;

	// ---- BOT_CSV: authored-data load state ----
	// A load failure poisons the engine: every call site constructs a FRESH BotEngine
	// (BotEngineLoader::reload destroys the old one before loadHuntData runs), so there
	// is no previous data to fall back to. Refusing all activation is the loud, safe
	// state — the server stays up and serves logins while an admin fixes the file.
	bool dataPoisoned_ = false;
	std::string dataPoisonReason_;
	int64_t lastPoisonLogMs_ = 0;      // throttles the refusal ERROR to ~1/60s
	int64_t lastGoodDataLoadMs_ = 0;   // OTSYS_TIME() of last committed load
	int64_t lastFailedDataLoadMs_ = 0;
	std::string lastDataLoadError_;
	uint32_t nextScriptId_ = 0;        // meta.csv high-water mark; ids are never reused

	// Set while csvcheck runs a throwaway parse (the /cavebot reload pre-flight). The
	// loaders are the same code either way, so without this every reload would log the
	// full "Loaded N ..." block twice and double the conflict warnings.
	bool csvQuiet_ = false;

	// Canonical serialization of the loaded authored data. Added while the loaders were
	// still MySQL and left UNTOUCHED across the CSV cutover, so the before/after dumps
	// come from identical serialization code — that is what makes the parity gate honest.
	std::string buildAuthoredDataDump() const;

	// ---- BOT_CSV loaders (definitions in bot_data.cpp) ----
	// Each populates a PASSED-IN temporary and throws BotCsvError on any violation;
	// only reloadBotData() commits, and only after ALL of them succeed. No coldBoot or
	// force parameter: every call site constructs a fresh engine (BotEngineLoader::reload
	// destroys the old one first), so there is no previous data to fall back to and no
	// old container to shrink-guard against.
	std::string reloadBotData();
	uint32_t loadMetaCsv();
	void loadHuntScriptsCsv(std::vector<HuntScript>& out, uint32_t nextScriptId);
	// Rebuilt after the commit, from the world / live registries / config.
	void rebuildDerivedTables();
	void rebuildFidgetItemPool();
	void buildTravelDestinationsCache();

	// ---- BOT_CSV Milestone 2: in-game CSV editing (definitions in bot_csvedit.cpp) ----
	// Both return true if they consumed the command. Edits write the file and STOP —
	// they never call reloadBotData() on the live engine (that would refill containers
	// under running bots and resurrect the currentPOI use-after-free); the admin runs
	// /cavebot reload, exactly as every one of these commands already instructs.
	bool handleCsvEditCommand(const std::string& command, std::string& reply);
	bool handleCsvViewCommand(const std::string& command, std::string& reply);
	bool handleCsvPositionsCommand(const std::string& command, std::string& reply);
	bool handleCsvCheckCommand(const std::string& command, std::string& reply);
	uint32_t csvResolveScriptId(const std::string& name, std::string& err) const;

	// Tick frequency constants
	// 2026-05-27: Adopted Gesior ThaisWar bot's 5Hz tick architecture. At 30+
	// concurrent active bots, the 100ms→200ms tick interval halves per-bot CPU
	// overhead while the modulo cadence constants below preserve the previous
	// wall-clock response times for non-combat work AND match Gesior's exact
	// cadences for combat/healing/walking (see research/gesior/think.lua).
	// Combat scan cadence relaxes 300ms→600ms (Gesior's tested-stable rate at
	// 15-30 active bots in city PvP, no reported jitter).
	static constexpr uint32_t TICK_FREQ_COMBAT = 3;       // every 600ms — Gesior updatePossibleTargets
	static constexpr uint32_t TICK_FREQ_WALKING = 2;      // every 400ms — Gesior idle route step
	static constexpr uint32_t TICK_FREQ_IDLE = 10;        // every 2s    — preserves prior wall-clock
	static constexpr uint32_t TICK_FREQ_IDLE_SCAN = 5;    // every 1s    — preserves prior wall-clock
	static constexpr uint32_t TICK_FREQ_HEAL = 2;         // every 200ms (tick() is a cycleEvent(100)) — Gesior useBestHealing
	static constexpr int32_t ENGINE_TICK_INTERVAL = 200;  // 5Hz, was 10Hz (Gesior parity)

	// Config constants
	static constexpr int32_t PATH_MAX_DIST = 50;
	// The node budget Map::getPathMatchingCond gives the REAL walker. The bot A* pool is 4096;
	// any diagnostic that wants to reproduce live reachability must search with this instead,
	// or it reports routes the bot cannot actually walk.
	static constexpr int32_t STOCK_PATH_MAX_NODES = 512;
	// Bounding-box reach for the wide fallback search. PATH_MAX_DIST=50 is a box around the
	// START, so any target beyond 50 tiles is unreachable by construction, however walkable the
	// route is. The fallback needs a box that actually contains cross-town targets.
	static constexpr int32_t PATH_WIDE_DIST = 200;
	// Above this Chebyshev distance goTo() paths to a straight-line interpolated point instead
	// of the real target. Shared with `/cavebot route` so the diagnostic reports the same chunk
	// target the walker actually aims at.
	static constexpr int32_t CHUNK_DIST = 12;
	static constexpr int32_t WAYPOINT_DIST = 20;
	static constexpr int32_t POI_ARRIVAL_DIST = 3;
	static constexpr int32_t STUCK_THRESHOLD = 5;
	// AdvStone in-dungeon dwell window. Was 300-1800 (5-30 min); reduced to 60-180
	// (1-3 min) so bots visibly cycle through the dungeon rather than appearing stuck.
	static constexpr int32_t POI_DWELL_MIN = 60;
	static constexpr int32_t POI_DWELL_MAX = 180;
	static constexpr int32_t CHAT_CHANCE = 300;
	// ---- Population Scheduler Toggles ----
	static constexpr bool POPULATION_TIME_OF_DAY_ENABLED = false;  // false = 100% target always (ignore hour-based %)
	static constexpr bool POPULATION_RAMP_ENABLED = false;         // false = activate/deactivate all at once (no 2-per-tick limit)

	static constexpr int32_t LOG_INTERVAL = 300;
	static constexpr int32_t HUNT_CHANCE_PER_TICK = 200;
	static constexpr int32_t TRAVEL_CHANCE_PER_TICK = 1000;
	static constexpr int32_t COMBAT_LEASH_DIST = 20;
	static constexpr int32_t HEAL_THRESHOLD_PCT = 75;
	static constexpr int32_t HEAL_EMERGENCY_PCT = 30;        // below this, long-CD emergency heals unlocked
	static constexpr int32_t HEAL_LONG_CD_THRESHOLD_S = 30;  // spells with cd > this are emergency-only (e.g. exura gran ico @ 600s)
	static constexpr int32_t HEAL_COOLDOWN = 1;
	static constexpr int32_t PK_TIMEOUT = 60;
	// Absolute ceiling for an ordinary hunt, measured from huntStartTime. Raised 3600 -> 5400 on
	// 2026-08-05; see the "Absolute hunt ceilings" table above bot_engine_impl.hpp's quest budgets
	// for the arithmetic that forces this value. Two independent reasons:
	//   1. The VIRTUAL sim enforces none of the live per-phase budgets — it just walks the index at
	//      VIRTUAL_MS_PER_WAYPOINT. Joint worst case over the enabled non-quest scripts (2081
	//      "Asura Palace Cave -2", travel_to 131 + travel_from 135 wps) is 1330s of legs + a 2400s
	//      patrol clock + 300s resupply = 4030s. 8 of 220 scripts exceeded 3600 and were therefore
	//      aborted on their FIRST live tick after waking, whatever the bot was doing.
	//   2. The loop-boundary hunt end (doHuntPatrol) deliberately lets PATROLLING run past
	//      huntEndTime by the remainder of the current lap, and PATROLLING is the only phase with
	//      no per-phase clock of its own.
	// COST, stated plainly: a bot genuinely stuck in PATROLLING now squats its spawn reservation
	// for up to 90 min instead of 60. The other four phases are each independently capped at
	// RESUPPLY_TIMEOUT / HUNT_TRAVEL_MAX_MS / LEAVING_PHASE_MAX_MS and are NOT loosened by this.
	static constexpr int32_t HUNT_SAFETY_TIMEOUT = 5400;
	// A party hunt sets huntEndTime from PARTY_HUNT_TIME_MIN/MAX (7200-10800s), which the 3600s
	// hunt ceiling always preempted — so every party hunt died at exactly 1h via
	// abortHunt("safety timeout") and those two constants were dead config. This ceiling is
	// PARTY_HUNT_TIME_MAX + 1800s for TRAVEL_TO + LEAVING + RESUPPLYING. Selected in doHunting on
	// bot.isPartyHuntLeader — followers never reach doHunting at all (state PARTY dispatches to
	// doParty/doPartyHunt), and isPartyHuntLeader is always cleared atomically with partyHuntId.
	static constexpr int32_t PARTY_SAFETY_TIMEOUT = 12600;
	static constexpr int32_t HUNT_HP_STUCK_TIMEOUT = 10; // seconds: abandon target if HP not decreasing
	static constexpr int32_t Z_CHANGE_GRACE_MS = 2000;  // 2s grace: no attack attempts after z-change
	static constexpr int32_t HUNT_STUCK_THRESHOLD = 30;
	static constexpr int32_t PATROL_LOOKAHEAD_DIST = 3;   // Chebyshev tiles for look-ahead matching
	static constexpr int32_t PATROL_LOOKAHEAD_MAX = 8;    // Max waypoints to scan ahead
	static constexpr int32_t HUNT_COOLDOWN_MIN = 600;
	static constexpr int32_t HUNT_COOLDOWN_MAX = 1800;
	// HUNT_TIME_MIN/MAX moved to config (BOT_HUNT_TIME_MIN_SEC/MAX_SEC) on 2026-06-09 so
	// hunt duration can be tuned via config.lua without a rebuild. Read at hunt start in
	// tryStartHunt / virtualTryStartHunt.
	static constexpr int32_t RESUPPLY_TIMEOUT = 300;
	static constexpr int32_t FLEE_DISTANCE = 10;
	static constexpr int32_t PZ_LOCK_DURATION = 900;
	static constexpr int32_t MONSTER_SCAN_RADIUS = 10; // MAP_MAX_VIEW_PORT_X(11) - 1: A* maxSearchDist + partyhunt AOE
	// Per-axis client-viewport half-extents used for combat-detection Spectators::find scans
	// (targeting, threat detection, debug mob list). Set to engine client viewport - 1 so
	// the bot only considers monsters comfortably inside the player's visible screen.
	// If this proves too tight in practice, drop the -1 to match the engine's client viewport.
	static constexpr int32_t MONSTER_SCAN_RADIUS_X = MAP_MAX_CLIENT_VIEW_PORT_X - 1; // 7
	static constexpr int32_t MONSTER_SCAN_RADIUS_Y = MAP_MAX_CLIENT_VIEW_PORT_Y - 1; // 5
	static constexpr int32_t FC_SCAN_RADIUS = 12;
	static constexpr int32_t FC_TIMEOUT = 15;
	// ---- TRUE MULTI-FLOOR tunables (engine constants, hot-reloadable with the .so;
	// deliberately NOT config.lua — retuning must not need a full rebuild) ----
	static constexpr int32_t Z_LEG_MAX = 80; // max same-floor leg between hops (planner radius)
	static constexpr int32_t Z_FC_ENGAGE_DIST = 10; // enter the FC machine when this close to the planned portal
	static constexpr int64_t Z_BLACKLIST_MS = 10 * 60 * 1000; // failed-portal quarantine
	// How long a bot refuses to re-enter the portal it just traversed. Long enough to break a
	// ping-pong, short enough that a genuine there-and-back route is only briefly delayed.
	static constexpr int64_t Z_LAST_PORTAL_GUARD_MS = 60 * 1000;
	// Cell budget for the bounded local BFS that answers the two END legs of a route
	// (start->first portal, last landing->target). Those positions are arbitrary and cannot be
	// pre-labelled, so they get one flood each, memoized per plan call.
	//
	// Must cover a full Z_LEG_MAX-radius disc: (2*80+1)^2 = 25,921 cells worst case. It was
	// first set to 4,000 and that was too small — a 48-tile leg across open Thais streets
	// exhausted it, the ladder to the boat was wrongly reported unreachable, and the planner
	// detoured through a pointless up-then-immediately-down pair of portals to get around a leg
	// it could actually walk. Sized to the real bound now; [ZPLAN_SLOW] reports when a plan
	// exceeds Z_PLAN_SLOW_MS so the cost stays visible.
	static constexpr uint32_t Z_END_LEG_BUDGET = 30000;
	static constexpr int64_t Z_PLAN_SLOW_MS = 25; // warn threshold for one zPlanFullRoute call
	// Flood-cache lifetime and size. Connectivity only changes on a map edit or reload (doors are
	// modelled as passable either way), so the TTL exists to bound staleness and memory, not for
	// correctness. Each entry is at most Z_END_LEG_BUDGET uint64s (~240KB), so the cap bounds the
	// cache at roughly 30MB.
	static constexpr int64_t Z_REACH_TTL_MS = 10 * 60 * 1000;
	static constexpr size_t Z_REACH_CACHE_MAX = 128;
	// Door-bridge search bounds, and how long a bot keeps trying to reach ONE door before
	// abandoning it. The existing s_failedDoors cooldown only covers "reached the door and the
	// open failed" — it does not cover "never got to the door at all", which is precisely the
	// case here, so a separate timeout is required or the bot retries the same door forever.
	static constexpr int32_t Z_DOOR_BRIDGE_RADIUS = 24;
	static constexpr uint32_t Z_DOOR_BRIDGE_BUDGET = 3000;
	static constexpr int64_t Z_DOOR_BRIDGE_TIMEOUT_MS = 10000;
	// guid -> {door tile key, first-attempt time}: the door this bot is currently bridging to.
	std::unordered_map<uint32_t, std::pair<uint64_t, int64_t>> s_doorBridgeAttempt;
	uint32_t s_zDoorBridgeOk = 0, s_zDoorBridgeGiveup = 0;
	static constexpr int32_t Z_MAX_HOPS = 8;

	// ---- Autonomous Activity Reroll Config ----
	// All values below are runtime-configurable via config.lua keys (see
	// BOT_SYSTEM_DOCS.md Phase A.2). The static constants
	// here are kept only as named accessors with the canonical defaults; the
	// actual call sites read via g_configManager().getNumber(BOT_*). Changing
	// values requires /reload config (or a server restart).
	// Defaults retained inline so a config-load failure / missing key falls back
	// to the documented values.
	static constexpr int32_t REROLL_IDLE_WEIGHT_POST_ACTIVATION = 5;  // % dwell on first reroll after activation/reload (not config-exposed; constant)

	// Inactive bot position — staging area for inactive bots
	static constexpr Position INACTIVE_POS { 31970, 32283, 7 };

	// Static data
	const std::unordered_map<uint32_t, std::vector<BotPOI>>& getCityPOIs();
	void loadCityPOIsCsv(std::unordered_map<uint32_t, std::vector<BotPOI>>& out);
	static const std::unordered_map<uint32_t, uint8_t>& getCityWalkZ();
	void loadTravelPositionsCsv(
		std::unordered_map<uint32_t, std::vector<std::pair<Position, std::string>>>& outPositions,
		std::unordered_map<uint32_t, std::string>& outNames);
	std::pair<Position, std::string> getTravelPosition(uint32_t townId) const;
	const std::unordered_map<uint32_t, std::vector<uint32_t>>& getTravelDestinations();
	int getPOIWeight(POIType type) const;
	// Player-proximity weighting helpers (2026-06-15) — definitions follow refreshAnchorsIfStale.
	int32_t minChebToAnchor(const Position& p) const;
	int32_t proximityBonus(int32_t minCheb) const;
	int32_t sampledMinChebForScript(const HuntScript& s) const;
	int32_t minChebToTown(uint32_t townId);
	static size_t weightedPick(const std::vector<int32_t>& w);
	void recordProxSelection(int32_t chosenDist, const char* kind, uint32_t guid, const std::string& pick);
	static const std::vector<RealHealSpell>& getHealSpells(uint8_t baseVoc);
};
