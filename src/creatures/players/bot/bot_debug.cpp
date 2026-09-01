/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_debug.cpp — debug stream: per-bot grid, event log, heartbeat, cast-chat status text
//
// BOT_NAV_REALISM Phase 12 module split. Compiles into the SAME libbot_engine.so
// as bot_engine.cpp, so /cavebot reload is unchanged. Shared includes, engine-local
// types and the BotEngine class declaration all live in bot_engine_impl.hpp.
//
// Assembled from several disjoint ranges of bot_engine.cpp, kept in their original
// top-to-bottom order. Every range reported zero inbound AND zero outbound symbols
// from tools/botnavsim/module_promote.py before any of them was carved.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

// ============================================================================
// Debug stream — per-bot grid + event log (toggled via executeCommand)
// Storage lives OUTSIDE BotState to avoid ABI churn. Activated by Lua reload
// flow via Game.botCommand(name, "debug on"). Emits to journalctl with the
// canonical "[BOT:DBG]" (heartbeat) and "[BOT:EVT]" (action) prefixes.
// ============================================================================

static inline bool isDebugOn(uint32_t guid) {
	auto it = s_debugConfigs.find(guid);
	return it != s_debugConfigs.end() && it->second.enabled;
}


// ---- Navigation event tracking (writes to bot_nav_events table) ----












// ============================================================================
// Cast Chat debug logging
// ============================================================================

void BotEngine::castLog(BotState& bot, const std::string& msg) {
	if (!bot.verboseLog) return;

	auto player = bot.getPlayer();
	if (!player) return;

	player->sendChannelMessage("BotAI", msg, TALKTYPE_CHANNEL_O, CHANNEL_CAST);
	g_logger().info("[BotEngine] {}: {}", player->getName(), msg);
}

void BotEngine::castLogError(BotState& bot, const std::string& msg) {
	if (!bot.verboseLog) return;

	auto player = bot.getPlayer();
	if (!player) return;

	player->sendChannelMessage("BotAI", msg, TALKTYPE_CHANNEL_R1, CHANNEL_CAST);
	g_logger().warn("[BotEngine] {}: {}", player->getName(), msg);
}

std::string BotEngine::buildStatusDetail(BotState& bot) {
	auto now = OTSYS_TIME();

	// Death pause takes priority over state
	if (bot.deathPauseUntil > 0 && now < bot.deathPauseUntil) {
		int32_t remaining = static_cast<int32_t>((bot.deathPauseUntil - now) / 1000);
		return fmt::format("IDLE (death pause) -- {}s remaining", remaining);
	}

	// Adventurer's Stone trip overrides bot.state for status text since the trip
	// handler short-circuits state dispatch — bot.state stays at IDLE during the trip
	if (bot.advStoneActive) {
		const char* phaseName = "?";
		switch (bot.advStonePhase) {
			case 0: phaseName = "walking dungeon"; break;
			case 1: phaseName = "dwelling in dungeon"; break;
			case 2: phaseName = "stepping on forcefield"; break;
		}
		size_t total = adventurerStoneRoute_.size();
		// Phase 5d: clamp display index to total. advStoneRouteIdx can transiently
		// equal `total` after the route walker runs the final waypoint; without clamp
		// the status would show e.g. "wp 17/16" which the user (correctly) flagged as
		// confusing. Clamping to total shows "wp 16/16" until phase=2 fires.
		uint16_t displayIdx = std::min<uint16_t>(bot.advStoneRouteIdx, static_cast<uint16_t>(total > 0 ? total - 1 : 0));
		if (bot.advStonePhase == 1 && bot.advStoneDwellUntil > 0 && now < bot.advStoneDwellUntil) {
			int32_t remaining = static_cast<int32_t>((bot.advStoneDwellUntil - now) / 1000);
			return fmt::format("ADVSTONE [{}, wp {}/{}] -- from town {}, {}s remaining",
				phaseName, displayIdx + 1, total, bot.advStoneStartTownId, remaining);
		}
		return fmt::format("ADVSTONE [{}, wp {}/{}] -- from town {}",
			phaseName, displayIdx + 1, total, bot.advStoneStartTownId);
	}

	// Personality mid-walk pause annotation. Gated on the reliable BotState field first
	// (a non-paused/fresh/reused-guid bot always has this at 0, so a leaked map entry can
	// never render a phantom pause). Only IDLE/DWELLING/TRAVELING can pause (state allowlist
	// in botStartAutoWalk), so we only append it in those arms below.
	std::string pauseAnnotation;
	if (bot.pendingWalkPauseEventId != 0) {
		auto pit = walkPauseInfo_.find(bot.guid);
		if (pit != walkPauseInfo_.end()) {
			int64_t elapsed = std::min(now - pit->second.first, pit->second.second) / 1000;
			int64_t totalSec = pit->second.second / 1000;
			pauseAnnotation = fmt::format(" [PAUSED personality: {}s/{}s]", elapsed, totalSec);
		}
	} else {
		// Lazy GC: field is 0 (pause ended) but an entry lingered — drop it.
		walkPauseInfo_.erase(bot.guid);
	}

	// BOT_AMBIENT_ROAM. A roamer's bot.state is an ordinary IDLE/DWELLING and its currentPOI is
	// deliberately null, so the generic arms below rendered "DWELLING at unknown" — technically
	// true and useless. The session is the real answer to "what is this bot doing", so it is read
	// here, ahead of the state switch, exactly as the AdvStone trip is below.
	if (auto rit = s_roam.find(bot.guid); rit != s_roam.end()) {
		const RoamRun& run = rit->second;
		std::string task;
		if (run.defendTargetId != 0) {
			std::string mname;
			if (auto t = g_game().getCreatureByID(run.defendTargetId)) mname = t->getName();
			task = mname.empty() ? "fighting back" : fmt::format("fighting {}", mname);
		} else if (run.suspended) {
			task = "suspended (busy elsewhere)";
		} else if (run.retiring) {
			task = "leaving";
		} else if (run.phase == RoamPhase::WALKING) {
			task = fmt::format("walking to ({},{},{})", run.dest.x, run.dest.y, run.dest.z);
		} else {
			const int32_t remaining = run.dwellUntilMs > now
				? static_cast<int32_t>((run.dwellUntilMs - now) / 1000) : 0;
			task = fmt::format("idle -- {}s remaining", remaining);
		}
		return fmt::format("AMBIENT ROAM - {} (leg {}){}", task, run.legs, pauseAnnotation);
	}

	switch (bot.state) {
		case BotAIState::IDLE: {
			std::string town = bot.townName.empty() ? "unknown" : bot.townName;
			// Track idle start time
			if (s_idleStartTime.find(bot.guid) == s_idleStartTime.end()) {
				s_idleStartTime[bot.guid] = now;
			}
			int32_t idleElapsed = static_cast<int32_t>((now - s_idleStartTime[bot.guid]) / 1000);
			if (bot.nextRerollTime > 0 && now < bot.nextRerollTime) {
				int32_t rerollIn = static_cast<int32_t>((bot.nextRerollTime - now) / 1000);
				return fmt::format("IDLE in {} -- {}s idle, reroll in {}s{}", town, idleElapsed, rerollIn, pauseAnnotation);
			}
			return fmt::format("IDLE in {} -- {}s idle{}", town, idleElapsed, pauseAnnotation);
		}
		case BotAIState::DWELLING: {
			s_idleStartTime.erase(bot.guid); // Reset idle timer when dwelling
			// "unknown" meant only "no POI pointer", which is the normal state for every activity
			// that owns its own side-table run rather than a POI — so a fishing bot, a house
			// visitor and a bot standing at a depot locker all rendered identically and said
			// nothing. Name the activity that is actually running; the POI type stays the answer
			// when there is one.
			std::string poiStr;
			if (bot.currentPOI) {
				poiStr = poiTypeName(bot.currentPOI->type);
			} else if (auto fit = s_fishing.find(bot.guid); fit != s_fishing.end()) {
				poiStr = fit->second.phase == FishPhase::FISHING ? "fishing"
					: (fit->second.phase == FishPhase::RETURNING ? "fishing (heading home)" : "fishing (on the way)");
			} else if (isIceFishing(bot.guid)) {
				poiStr = "ice fishing";
			} else if (auto hit = s_houseRuns.find(bot.guid); hit != s_houseRuns.end()) {
				switch (hit->second.mode) {
					case HouseMode::HIRELING:
						poiStr = hit->second.hirelingName.empty()
							? "house (hireling)" : fmt::format("house (hireling {})", hit->second.hirelingName);
						break;
					case HouseMode::DUMMY:  poiStr = "house (training dummy)"; break;
					case HouseMode::LOCKER: poiStr = "house (locker)"; break;
					default:                poiStr = "house (idle)"; break;
				}
			} else if (bot.hasDepotTarget) {
				poiStr = "depot locker";
			} else {
				poiStr = "unknown";
			}
			if (bot.dwellUntil > 0 && now < bot.dwellUntil) {
				int32_t remaining = static_cast<int32_t>((bot.dwellUntil - now) / 1000);
				return fmt::format("DWELLING at {} -- {}s remaining{}", poiStr, remaining, pauseAnnotation);
			}
			return fmt::format("DWELLING at {}{}", poiStr, pauseAnnotation);
		}
		case BotAIState::HUNTING: {
			// Clear idle tracking
			s_idleStartTime.erase(bot.guid);
			std::string scriptName;
			const HuntScript* hs = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { scriptName = s.name; hs = &s; break; }
			}
			if (scriptName.empty()) scriptName = fmt::format("id={}", bot.huntScriptId);
			const char* phase = huntPhaseName(bot.huntPhase);
			size_t totalWps = 0;
			if (hs) {
				switch (bot.huntPhase) {
					case HuntPhase::TRAVEL_TO: totalWps = hs->travelToWaypoints.size(); break;
					case HuntPhase::PATROLLING: totalWps = hs->patrolWaypoints.size(); break;
					case HuntPhase::LEAVING: totalWps = hs->travelFromWaypoints.size(); break;
					default: break;
				}
			}
			// Time tracking: elapsed / max hunt duration
			std::string timeStr;
			if (bot.huntStartTime > 0) {
				int32_t elapsed = static_cast<int32_t>((now - bot.huntStartTime) / 1000);
				if (bot.huntEndTime > bot.huntStartTime) {
					int32_t maxDur = static_cast<int32_t>((bot.huntEndTime - bot.huntStartTime) / 1000);
					timeStr = fmt::format(" ({}s / {}s)", elapsed, maxDur);
				} else {
					timeStr = fmt::format(" ({}s elapsed)", elapsed);
				}
			}
			// BOT_LURE_KITE: the heartbeat grid is the primary debug surface for these two,
			// so their state has to be visible here and not only in the journal.
			std::string lureStr;
			{
				const auto lIt = s_lure.find(bot.guid);
				if (lIt != s_lure.end() && lIt->second.phase != LurePhase::Off) {
					lureStr = lIt->second.phase == LurePhase::Luring
						? fmt::format(" [lure {}/{}]", lIt->second.count,
							effectiveMinMonsters(bot, hs))
						: fmt::format(" [engage {} trig={}]", lIt->second.count, lIt->second.lastTrigger);
				}
				const auto kIt = s_kite.find(bot.guid);
				if (kIt != s_kite.end() && kIt->second.active) {
					lureStr += fmt::format(" [kite wp{}<-{} legs={}]",
						kIt->second.cursor, kIt->second.anchorIdx, kIt->second.legs);
				}
			}
			if (totalWps > 0) {
				return fmt::format("HUNTING '{}' {} wp {}/{} -- {} kills{}{}",
					scriptName, phase, bot.huntWaypointIdx, totalWps, bot.huntKillCount, timeStr,
					lureStr);
			}
			return fmt::format("HUNTING '{}' {} -- {} kills{}",
				scriptName, phase, bot.huntKillCount, timeStr);
		}
		case BotAIState::TRAVELING: {
			s_idleStartTime.erase(bot.guid);
			// Track travel start time
			if (s_travelStartTime.find(bot.guid) == s_travelStartTime.end()) {
				s_travelStartTime[bot.guid] = now;
			}
			int32_t travelElapsed = static_cast<int32_t>((now - s_travelStartTime[bot.guid]) / 1000);
			std::string destName;
			auto nameIt = travelTownNames_.find(bot.travelDestTownId);
			if (nameIt != travelTownNames_.end()) destName = nameIt->second;
			else destName = fmt::format("town {}", bot.travelDestTownId);
			return fmt::format("TRAVELING to {} -- {} ({}s elapsed){}", destName,
				bot.travelPhase.empty() ? "starting" : bot.travelPhase, travelElapsed, pauseAnnotation);
		}
		case BotAIState::COMBAT: {
			s_idleStartTime.erase(bot.guid);
			s_travelStartTime.erase(bot.guid); s_travelDestPOI.erase(bot.guid); s_travelSrcPOI.erase(bot.guid); s_lastRouteEndPos.erase(bot.guid); s_travelArriveTarget.erase(bot.guid);
			int32_t elapsed = static_cast<int32_t>((now - bot.combatStartTime) / 1000);
			return fmt::format("COMBAT ({}) -- {}s elapsed",
				bot.combatDecision.empty() ? "fight" : bot.combatDecision, elapsed);
		}
		case BotAIState::FLEEING: {
			int32_t elapsed = static_cast<int32_t>((now - bot.combatStartTime) / 1000);
			return fmt::format("FLEEING -- {}s elapsed", elapsed);
		}
		case BotAIState::PK_ATTACK: {
			int32_t elapsed = static_cast<int32_t>((now - bot.combatStartTime) / 1000);
			return fmt::format("PK_ATTACK -- {}s elapsed", elapsed);
		}
		case BotAIState::PARTY: {
			s_idleStartTime.erase(bot.guid);
			// Track party start time
			if (s_partyStartTime.find(bot.guid) == s_partyStartTime.end()) {
				s_partyStartTime[bot.guid] = now;
			}
			int32_t partyElapsed = static_cast<int32_t>((now - s_partyStartTime[bot.guid]) / 1000);
			auto leaderIt = s_partyLeaderId.find(bot.guid);
			std::string leaderName = "?";
			if (leaderIt != s_partyLeaderId.end()) {
				auto lc = g_game().getCreatureByID(leaderIt->second);
				if (lc) leaderName = lc->getName();
			}
			auto bp = bot.getPlayer();
			auto target = bp ? bp->getAttackedCreature() : nullptr;
			if (target) {
				return fmt::format("PARTY — following {}, target={} ({}s elapsed)", leaderName, target->getName(), partyElapsed);
			}
			return fmt::format("PARTY — following {} ({}s elapsed)", leaderName, partyElapsed);
		}
		default:
			return std::string(botStateName(bot.state));
	}
}

void BotEngine::logHeartbeat(BotState& bot) {
	std::string detail = buildStatusDetail(bot);

	// Append position and PZ status to all heartbeats
	bool inPZ = false;
	auto hbTile = g_game().map.getTile(bot.currentPos);
	if (hbTile) inPZ = hbTile->hasFlag(TILESTATE_PROTECTIONZONE);
	castLog(bot, fmt::format("STATUS: {} [pos=({},{},{}) {}]", detail,
		bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
		inPZ ? "PZ" : "noPZ"));
}


// ============================================================================
// Debug stream implementation — heartbeat + 15x11 grid + events + spell impact
//
// Activated via executeCommand("debug on") from the Lua reload flow. Emits to
// journalctl with two canonical prefixes:
//   [BOT:DBG] = heartbeat snapshot (state, grid, mobs, timing)
//   [BOT:EVT] = per-event line (state transition, action result, spell, FC, etc.)
//
// Per-bot config lives in s_debugConfigs to avoid ABI changes to BotState. The
// flag is checked once per tick per active bot — when disabled (the default for
// 199 of 200 bots in debug,1 mode), the cost is one hashmap lookup (~30ns).
// ============================================================================


std::string BotEngine::dbgRenderGrid(const BotState& bot) {
	std::string s;
	s.reserve(11 * (2 * DBG_GRID_HALF_X + 1) * 2 + 64);
	for (int32_t dy = -DBG_GRID_HALF_Y; dy <= DBG_GRID_HALF_Y; ++dy) {
		for (int32_t dx = -DBG_GRID_HALF_X; dx <= DBG_GRID_HALF_X; ++dx) {
			char sym;
			if (dx == 0 && dy == 0) {
				sym = '@';
			} else {
				Position p(static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.x) + dx),
					static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.y) + dy),
					bot.currentPos.z);
				auto tile = g_game().map.getTile(p);
				sym = gridSymbolForTile(p, tile.get());
			}
			s += sym;
			s += ' ';
		}
		if (dy < DBG_GRID_HALF_Y) s += '\n';
	}
	return s;
}

void BotEngine::dbgEmitMobList(const BotState& bot) {
	// List monsters within MONSTER_SCAN_RADIUS_X/Y (matches engine's combat-detection range)
	auto spectators = Spectators().find<Creature>(bot.currentPos, false,
		MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_Y, MONSTER_SCAN_RADIUS_Y);
	int32_t monsterCount = 0, npcCount = 0, playerCount = 0;
	std::string mobLines, npcLines, playerLines;
	auto botPos = bot.currentPos;
	for (const auto& c : spectators) {
		if (!c || c->getHealth() <= 0) continue;
		auto p = c->getPosition();
		if (p.z != botPos.z) continue;
		int32_t dx = static_cast<int32_t>(p.x) - static_cast<int32_t>(botPos.x);
		int32_t dy = static_cast<int32_t>(p.y) - static_cast<int32_t>(botPos.y);
		if (dx == 0 && dy == 0) continue; // skip self
		int32_t dist = std::max(std::abs(dx), std::abs(dy));
		bool losOk = g_game().map.isSightClear(botPos, p, true);
		bool isTargeted = (bot.huntTargetId == c->getID() || bot.attackerId == c->getID() || bot.pkTarget == c->getID());
		auto pl = c->getPlayer();
		auto nc = c->getNpc();
		auto mc = c->getMonster();
		if (mc) {
			monsterCount++;
			int32_t hpPct = c->getMaxHealth() > 0 ? (c->getHealth() * 100 / c->getMaxHealth()) : 0;
			mobLines += fmt::format("{} {}#{} d={} dx={:+d} dy={:+d} hp%={} los={} tgt={}\n",
				DBG_TAG, c->getName(), c->getID(), dist, dx, dy, hpPct,
				losOk ? "ok" : "blk", isTargeted ? "yes" : "no");
		} else if (nc) {
			npcCount++;
			npcLines += fmt::format("{} {} d={} dx={:+d} dy={:+d}\n",
				DBG_TAG, c->getName(), dist, dx, dy);
		} else if (pl) {
			playerCount++;
			Skulls_t skull = pl->getSkull();
			const char* skullStr = "NONE";
			switch (skull) {
				case SKULL_WHITE: skullStr = "WHITE"; break;
				case SKULL_RED:   skullStr = "RED"; break;
				case SKULL_BLACK: skullStr = "BLACK"; break;
				default: break;
			}
			playerLines += fmt::format("{} {} d={} dx={:+d} dy={:+d} skull={}\n",
				DBG_TAG, pl->getName(), dist, dx, dy, skullStr);
		}
	}
	g_logger().info("{} mobs={} npcs={} players={}", DBG_TAG, monsterCount, npcCount, playerCount);
	if (monsterCount > 0) g_logger().info("{}", mobLines.substr(0, mobLines.size() - 1));
	if (npcCount > 0)     g_logger().info("{}", npcLines.substr(0, npcLines.size() - 1));
	if (playerCount > 0)  g_logger().info("{}", playerLines.substr(0, playerLines.size() - 1));
}

void BotEngine::dbgEmitHeartbeat(BotState& bot, BotDebugCfg& cfg) {
	auto player = bot.getPlayer();
	if (!player) return;
	int64_t now = OTSYS_TIME();
	double tSinceStart = (now - cfg.debugStartTime) / 1000.0;
	int64_t tickGap = cfg.lastBotTickTime > 0 ? (now - cfg.lastBotTickTime) : 0;
	int64_t snapGap = cfg.lastSnapshot > 0 ? (now - cfg.lastSnapshot) : 0;

	int32_t hp = player->getHealth();
	int32_t maxHp = player->getMaxHealth();
	int32_t hpPct = maxHp > 0 ? (hp * 100 / maxHp) : 0;
	int32_t mana = player->getMana();
	int32_t maxMana = player->getMaxMana();

	// Header line
	g_logger().info("{} t={:.1f}s bot={} st={} pos=({},{},{}) hp={}/{}({}%) mp={}/{} active={} tickGap={}ms snapGap={}ms",
		DBG_TAG, tSinceStart, player->getName(), botStateName(bot.state),
		bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
		hp, maxHp, hpPct, mana, maxMana, bot.active ? 1 : 0,
		tickGap, snapGap);

	// Hunt / FC / walk info
	std::string aux;
	if (bot.huntScriptId > 0) {
		const char* phaseName = "?";
		auto idx = static_cast<uint8_t>(bot.huntPhase);
		if (idx < 5) phaseName = huntPhaseName(bot.huntPhase);
		aux += fmt::format(" hunt={} phase={} wp={} kills={}",
			bot.huntScriptId, phaseName, bot.huntWaypointIdx, bot.huntKillCount);
	}
	if (bot.hasWalkTarget) {
		int64_t walkAge = cfg.lastWalkTargetSetTime > 0 ? (now - cfg.lastWalkTargetSetTime) : 0;
		aux += fmt::format(" walk=({},{},{}) walkAge={}ms",
			bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z, walkAge);
	}
	if (bot.fcState != FloorChangeState::NONE) {
		aux += fmt::format(" fc={}", static_cast<int>(bot.fcState));
	}
	if (bot.attackerId > 0) aux += fmt::format(" attacker={}", bot.attackerId);
	if (bot.huntTargetId > 0) aux += fmt::format(" target={}", bot.huntTargetId);
	if (!aux.empty()) g_logger().info("{}{}", DBG_TAG, aux);

	// Grid (15x11 around bot, 7L/7R, 5U/5D)
	if (cfg.gridEnabled) {
		auto grid = dbgRenderGrid(bot);
		g_logger().info("{} grid 15x11 z={} (legend: @=me .=walk #=block *=fc F=field N=npc P=player r/c/d=mob 1st-letter)",
			DBG_TAG, bot.currentPos.z);
		// Split grid by newlines and prefix each row with DBG_TAG
		size_t start = 0;
		while (start < grid.size()) {
			size_t end = grid.find('\n', start);
			std::string row = end == std::string::npos ? grid.substr(start) : grid.substr(start, end - start);
			g_logger().info("{}   {}", DBG_TAG, row);
			if (end == std::string::npos) break;
			start = end + 1;
		}
	}

	// Mobs / npcs / players list
	dbgEmitMobList(bot);

	// Stuck detector: walk target unchanged + position unchanged for >5s
	if (bot.hasWalkTarget && cfg.lastWalkTargetSetTime > 0) {
		int64_t walkAge = now - cfg.lastWalkTargetSetTime;
		if (walkAge > 5000 && cfg.lastWalkTarget == bot.walkTarget) {
			// Estimate path length to walk target (Chebyshev)
			int32_t pdx = std::abs(static_cast<int32_t>(bot.walkTarget.x) - static_cast<int32_t>(bot.currentPos.x));
			int32_t pdy = std::abs(static_cast<int32_t>(bot.walkTarget.y) - static_cast<int32_t>(bot.currentPos.y));
			int32_t pathLen = std::max(pdx, pdy);
			g_logger().info("{} t={:.1f}s STUCK pos=({},{},{}) duration={:.1f}s walkTarget=({},{},{}) chebyshev={}",
				EVT_TAG, tSinceStart,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				walkAge / 1000.0,
				bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z, pathLen);
		}
	}
	// Refresh walk-target tracking
	if (bot.hasWalkTarget) {
		if (!(cfg.lastWalkTarget == bot.walkTarget)) {
			cfg.lastWalkTarget = bot.walkTarget;
			cfg.lastWalkTargetSetTime = now;
		}
	} else {
		cfg.lastWalkTargetSetTime = 0;
	}
}

void BotEngine::dbgEmitEvent(const BotState& bot, BotDebugCfg* cfg,
	const std::string& kind, const std::string& fields) {
	if (!cfg || !cfg->eventsEnabled) return;
	double tSinceStart = (OTSYS_TIME() - cfg->debugStartTime) / 1000.0;
	int64_t gap = cfg->lastEventTime > 0 ? (OTSYS_TIME() - cfg->lastEventTime) : 0;
	cfg->lastEventTime = OTSYS_TIME();
	auto player = bot.getPlayer();
	std::string botName = player ? player->getName() : "?";
	g_logger().info("{} t={:.1f}s evtGap={}ms {} {}",
		EVT_TAG, tSinceStart, gap, kind, fields);
}

// Enumerate tiles affected by an AoE spell — uses isInAoeArea over a bounding box
std::vector<Position> BotEngine::dbgComputeAoeTiles(const Position& botPos,
	AoeAreaType areaType, Direction dir, int32_t areaSize, int32_t innerSize) {
	std::vector<Position> tiles;
	// Bounding box: largest AoE pattern is BEAM5 (length 5) + WAVE4 (4 forward, 2 wide).
	// Scan ±7 around the bot to be safe. (RING uses Chebyshev <= areaSize, also covered.)
	for (int32_t dx = -7; dx <= 7; ++dx) {
		for (int32_t dy = -7; dy <= 7; ++dy) {
			if (dx == 0 && dy == 0) continue;
			Position p(static_cast<uint16_t>(static_cast<int32_t>(botPos.x) + dx),
				static_cast<uint16_t>(static_cast<int32_t>(botPos.y) + dy),
				botPos.z);
			if (isInAoeArea(botPos, p, areaType, dir, areaSize, innerSize)) {
				tiles.push_back(p);
			}
		}
	}
	return tiles;
}

// Matrix-aware overlay — when the spell has a parsed matrix, use exact array
// lookup so the [BOT:DBG] spell_area_overlay grid matches the server's actual hit set.
// Bounding box derived from the matrix's max extent (caps at 10 for safety).
std::vector<Position> BotEngine::dbgComputeAoeTiles(const Position& botPos,
	const ResolvedSpell& spell, Direction dir) {
	std::vector<Position> tiles;
	if (!spell.cardinalMatrix) {
		return dbgComputeAoeTiles(botPos, spell.aoeAreaType, dir,
			spell.aoeAreaSize, spell.aoeInnerSize);
	}
	int32_t bound = std::max(spell.cardinalMatrix->maxRowExtent,
		spell.cardinalMatrix->maxColExtent) + (spell.needDirection ? 1 : 0) + 1;
	if (bound > 10) bound = 10;
	for (int32_t dx = -bound; dx <= bound; ++dx) {
		for (int32_t dy = -bound; dy <= bound; ++dy) {
			if (dx == 0 && dy == 0) continue;
			Position p(static_cast<uint16_t>(static_cast<int32_t>(botPos.x) + dx),
				static_cast<uint16_t>(static_cast<int32_t>(botPos.y) + dy),
				botPos.z);
			if (spellHits(botPos, p, spell, dir)) {
				tiles.push_back(p);
			}
		}
	}
	return tiles;
}

void BotEngine::dbgRecordPreCast(BotState& bot, const std::string& descriptor,
	const std::vector<Position>& areaTiles) {
	auto* cfg = getDebugCfg(bot.guid);
	if (!cfg || !cfg->eventsEnabled) return;
	auto& pc = cfg->pending;
	pc.active = true;
	pc.emittedAt = OTSYS_TIME();
	pc.descriptor = descriptor;
	pc.areaTiles = areaTiles;
	pc.preTargets.clear();
	pc.preHp.clear();
	pc.preMaxHp.clear();

	// Snapshot creatures currently on affected tiles
	std::string expected;
	int32_t expectedCount = 0;
	for (const auto& tilePos : areaTiles) {
		auto tile = g_game().map.getTile(tilePos);
		if (!tile) continue;
		auto creatures = tile->getCreatures();
		if (!creatures) continue;
		for (const auto& c : *creatures) {
			if (!c || c->getHealth() <= 0) continue;
			if (c.get() == bot.getPlayer().get()) continue; // skip self
			uint32_t cid = c->getID();
			int32_t dx = static_cast<int32_t>(tilePos.x) - static_cast<int32_t>(bot.currentPos.x);
			int32_t dy = static_cast<int32_t>(tilePos.y) - static_cast<int32_t>(bot.currentPos.y);
			pc.preTargets[cid] = { c->getName(), dx, dy };
			pc.preHp[cid] = c->getHealth();
			pc.preMaxHp[cid] = c->getMaxHealth();
			if (expectedCount > 0) expected += ", ";
			expected += fmt::format("{}#{}@({:+d},{:+d}):hp{}", c->getName(), cid, dx, dy, c->getHealth());
			expectedCount++;
		}
	}
	double tSinceStart = (OTSYS_TIME() - cfg->debugStartTime) / 1000.0;
	g_logger().info("{} t={:.1f}s spell_precast {} tiles={} expected({})=[{}]",
		EVT_TAG, tSinceStart, descriptor, areaTiles.size(), expectedCount, expected);

	// Render an overlay grid showing affected tiles around bot (1 tile = '+', tile-with-creature = 'X')
	if (cfg->gridEnabled) {
		std::unordered_map<uint64_t, char> overlay;
		auto posKey = [](int32_t dx, int32_t dy) {
			return (static_cast<uint64_t>(static_cast<uint32_t>(dx + 1000)) << 32) |
				static_cast<uint32_t>(dy + 1000);
		};
		for (const auto& p : areaTiles) {
			int32_t dx = static_cast<int32_t>(p.x) - static_cast<int32_t>(bot.currentPos.x);
			int32_t dy = static_cast<int32_t>(p.y) - static_cast<int32_t>(bot.currentPos.y);
			if (std::abs(dx) > DBG_GRID_HALF_X || std::abs(dy) > DBG_GRID_HALF_Y) continue;
			char sym = '+';
			auto tile = g_game().map.getTile(p);
			if (tile) {
				auto creatures = tile->getCreatures();
				if (creatures && !creatures->empty()) {
					for (const auto& c : *creatures) {
						if (c && c->getHealth() > 0 && c.get() != bot.getPlayer().get()) {
							sym = 'X';
							break;
						}
					}
				}
			}
			overlay[posKey(dx, dy)] = sym;
		}
		g_logger().info("{} spell_area_overlay (+=affected X=hit-creature):", DBG_TAG);
		for (int32_t dy = -DBG_GRID_HALF_Y; dy <= DBG_GRID_HALF_Y; ++dy) {
			std::string row;
			for (int32_t dx = -DBG_GRID_HALF_X; dx <= DBG_GRID_HALF_X; ++dx) {
				char ch;
				if (dx == 0 && dy == 0) ch = '@';
				else {
					auto it = overlay.find(posKey(dx, dy));
					if (it != overlay.end()) ch = it->second;
					else ch = '.';
				}
				row += ch;
				row += ' ';
			}
			g_logger().info("{}   {}", DBG_TAG, row);
		}
	}
}

void BotEngine::dbgEmitPostCastIfDue(BotState& bot) {
	auto* cfg = getDebugCfg(bot.guid);
	if (!cfg) return;
	auto& pc = cfg->pending;
	if (!pc.active) return;
	int64_t elapsed = OTSYS_TIME() - pc.emittedAt;
	// Give the cast 200ms to land (typical animation/damage latency).
	if (elapsed < 200) return;

	// For each pre-target, read current HP and compute delta. A creature missing now
	// (cid lookup fails) is presumed dead → assume full pre-HP damage.
	std::string hits, misses;
	int32_t hitCount = 0, missCount = 0;
	int32_t totalDamage = 0;
	for (const auto& [cid, info] : pc.preTargets) {
		const auto& [name, dx, dy] = info;
		auto creature = g_game().getCreatureByID(cid);
		int32_t preHp = pc.preHp.count(cid) ? pc.preHp.at(cid) : 0;
		if (!creature || creature->getHealth() <= 0) {
			// Creature died (or vanished). Count as hit for the damage value.
			int32_t dmg = preHp;
			totalDamage += dmg;
			if (hitCount > 0) hits += ", ";
			hits += fmt::format("{}#{}@({:+d},{:+d}):dmg={}(KILLED)", name, cid, dx, dy, dmg);
			hitCount++;
		} else {
			int32_t nowHp = creature->getHealth();
			int32_t dmg = preHp - nowHp;
			if (dmg > 0) {
				totalDamage += dmg;
				if (hitCount > 0) hits += ", ";
				hits += fmt::format("{}#{}@({:+d},{:+d}):dmg={}({}->{})", name, cid, dx, dy, dmg, preHp, nowHp);
				hitCount++;
			} else {
				if (missCount > 0) misses += ", ";
				misses += fmt::format("{}#{}@({:+d},{:+d}):dmg=0(hp={})", name, cid, dx, dy, nowHp);
				missCount++;
			}
		}
	}
	double tSinceStart = (OTSYS_TIME() - cfg->debugStartTime) / 1000.0;
	g_logger().info("{} t={:.1f}s spell_result {} elapsed={}ms hits({})=[{}] missed({})=[{}] totalDmg={}",
		EVT_TAG, tSinceStart, pc.descriptor, elapsed, hitCount, hits, missCount, misses, totalDamage);

	pc.active = false;
	pc.preTargets.clear();
	pc.preHp.clear();
	pc.preMaxHp.clear();
	pc.areaTiles.clear();
}

std::string BotEngine::dbgHandleCommand(BotState* bot, const std::string& rest) {
	if (!bot) return "no-bot";
	auto& cfg = s_debugConfigs[bot->guid];
	if (rest.empty() || rest == "status") {
		return fmt::format("debug: enabled={} grid={} events={} snapMs={} pendingCast={}",
			cfg.enabled ? "yes" : "no",
			cfg.gridEnabled ? "yes" : "no",
			cfg.eventsEnabled ? "yes" : "no",
			cfg.snapshotMs,
			cfg.pending.active ? "yes" : "no");
	}
	if (rest == "on") {
		cfg.enabled = true;
		cfg.debugStartTime = OTSYS_TIME();
		cfg.lastSnapshot = 0;
		cfg.lastBotTickTime = 0;
		cfg.lastEventTime = 0;
		cfg.pending.active = false;
		auto player = bot->getPlayer();
		g_logger().info("{} t=0.0s debug_session_start bot={} guid={} snapMs={}",
			EVT_TAG, player ? player->getName() : "?", bot->guid, cfg.snapshotMs);
		return "debug ON";
	}
	if (rest == "off") {
		bool wasEnabled = cfg.enabled;
		if (wasEnabled) {
			auto player = bot->getPlayer();
			g_logger().info("{} debug_session_end bot={} guid={}",
				EVT_TAG, player ? player->getName() : "?", bot->guid);
		}
		cfg.enabled = false;
		cfg.pending.active = false;
		return wasEnabled ? "debug OFF" : "debug already off";
	}
	if (rest == "grid on")    { cfg.gridEnabled = true;   return "grid ON"; }
	if (rest == "grid off")   { cfg.gridEnabled = false;  return "grid OFF"; }
	if (rest == "events on")  { cfg.eventsEnabled = true; return "events ON"; }
	if (rest == "events off") { cfg.eventsEnabled = false; return "events OFF"; }
	if (rest.substr(0, 9) == "snapshot ") {
		try {
			int32_t ms = std::stoi(rest.substr(9));
			if (ms < 100) ms = 100;
			if (ms > 60000) ms = 60000;
			cfg.snapshotMs = ms;
			return fmt::format("snapshot interval = {} ms", ms);
		} catch (...) {
			return "snapshot: invalid ms";
		}
	}
	return fmt::format("unknown debug verb '{}'. valid: on, off, status, grid on|off, events on|off, snapshot <ms>", rest);
}

