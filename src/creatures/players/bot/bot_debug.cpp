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

// BOT_TEST_HARNESS Phase 2 (plan §3.2 "[BOT:EVT] fix"): [BOT:EVT] lines used to carry only a
// relative `t=` on the cycle-cached OTSYS_TIME clock and no bot name, so they could not be joined
// to anything. Appended (never inserted) so positional readers of `t=`/`evtGap=`/kind keep
// working: ` bot='Name' guid=N ems=<epoch ms>`. merge.py already resolves `bot='...'`/`guid=N`.
static std::string dbgEvtSuffix(const BotState& bot) {
	return fmt::format(" bot='{}' guid={} ems={}", bot.name, bot.guid, botAudEpochMs());
}


// ---- Navigation event tracking (writes to bot_nav_events table) ----












// ============================================================================
// Cast Chat debug logging
// ============================================================================

void BotEngine::castLog(BotState& bot, const std::string& msg) {
	// BOT_TEST_HARNESS: remember the last decision line of a WATCHED bot as the "why" of its next
	// state record — before the verboseLog gate, so it works without a cast viewer. One load when off.
	// NAMED bots only (audNamed, never `watch *`): no map insert + string copy per castLog for
	// the whole population. `*` state records therefore say reason=unlogged.
	if (audNamed(AUD_STATE | AUD_PROBE, bot.guid)) [[unlikely]] {
		botAudNoteLog(bot.guid, msg);
	}
	if (!bot.verboseLog) return;

	auto player = bot.getPlayer();
	if (!player) return;

	player->sendChannelMessage("BotAI", msg, TALKTYPE_CHANNEL_O, CHANNEL_CAST);
	g_logger().info("[BotEngine] {}: {}", player->getName(), msg);
}

void BotEngine::castLogError(BotState& bot, const std::string& msg) {
	if (audNamed(AUD_STATE | AUD_PROBE, bot.guid)) [[unlikely]] {
		botAudNoteLog(bot.guid, msg);
	}
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
			g_logger().info("{} t={:.1f}s STUCK pos=({},{},{}) duration={:.1f}s walkTarget=({},{},{}) chebyshev={}{}",
				EVT_TAG, tSinceStart,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				walkAge / 1000.0,
				bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z, pathLen, dbgEvtSuffix(bot));
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
	// The bot name + epoch-ms ride at the END of the line (dbgEvtSuffix) so every existing
	// consumer that reads `t=` / `evtGap=` / the kind positionally keeps working.
	g_logger().info("{} t={:.1f}s evtGap={}ms {} {}{}",
		EVT_TAG, tSinceStart, gap, kind, fields, dbgEvtSuffix(bot));
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
	g_logger().info("{} t={:.1f}s spell_precast {} tiles={} expected({})=[{}]{}",
		EVT_TAG, tSinceStart, descriptor, areaTiles.size(), expectedCount, expected, dbgEvtSuffix(bot));

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
	g_logger().info("{} t={:.1f}s spell_result {} elapsed={}ms hits({})=[{}] missed({})=[{}] totalDmg={}{}",
		EVT_TAG, tSinceStart, pc.descriptor, elapsed, hitCount, hits, missCount, misses, totalDamage, dbgEvtSuffix(bot));

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
		g_logger().info("{} t=0.0s debug_session_start bot={} guid={} snapMs={} ems={}",
			EVT_TAG, player ? player->getName() : "?", bot->guid, cfg.snapshotMs, botAudEpochMs());
		return "debug ON";
	}
	if (rest == "off") {
		bool wasEnabled = cfg.enabled;
		if (wasEnabled) {
			auto player = bot->getPlayer();
			g_logger().info("{} debug_session_end bot={} guid={} ems={}",
				EVT_TAG, player ? player->getName() : "?", bot->guid, botAudEpochMs());
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


// ============================================================================
// BOT_TEST_HARNESS Phase 2 — audit stream, engine side
//
// Declarations + the rules this code lives by are in bot_engine_impl.hpp (search
// "BOT_TEST_HARNESS Phase 2"). The writer is the main-binary BotAuditSink. Nothing here runs
// off the dispatcher and nothing here owns a thread.
// ============================================================================

namespace {
	constexpr size_t AUD_FLUSH_AT = 256 * 1024;      // flush mid-tick past this many bytes
	constexpr size_t AUD_BUF_CAP = 8 * 1024 * 1024;  // hard cap if the sink never drains

	struct AudKindName {
		const char* name;
		uint32_t bit;
	};
	constexpr AudKindName kAudKinds[] = {
		{ "probe", AUD_PROBE },     { "state", AUD_STATE },     { "wake", AUD_WAKE },
		{ "hib", AUD_HIB },         { "walkpause", AUD_WALKPAUSE }, { "path", AUD_PATH },
		{ "pathout", AUD_PATHOUT }, { "decide", AUD_DECIDE },   { "cmd", AUD_CMD },
		{ "census", AUD_CENSUS },
	};

	// Same order as BotEngine::ActOutcome (bot_engine_impl.hpp).
	constexpr const char* kActOutcomeNames[] = {
		"dwell", "poi", "poi_fail", "hunt", "hunt_fail", "party", "party_fail",
		"travel", "city_walk", "travel_fail", "rounding_tail",
	};

	std::string audLower(std::string_view s) {
		std::string out(s);
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	std::string audTrim(std::string_view s) {
		size_t b = 0;
		size_t e = s.size();
		while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
			++b;
		}
		while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
			--e;
		}
		return std::string(s.substr(b, e - b));
	}

	std::vector<std::string> audSplitCsv(std::string_view csv) {
		std::vector<std::string> out;
		size_t start = 0;
		while (start <= csv.size()) {
			size_t comma = csv.find(',', start);
			if (comma == std::string_view::npos) {
				comma = csv.size();
			}
			std::string tok = audTrim(csv.substr(start, comma - start));
			if (!tok.empty()) {
				out.push_back(std::move(tok));
			}
			start = comma + 1;
		}
		return out;
	}

	std::string audMaskNames(uint32_t mask) {
		std::string out;
		for (const auto& k : kAudKinds) {
			if (mask & k.bit) {
				if (!out.empty()) {
					out.push_back(',');
				}
				out += k.name;
			}
		}
		return out.empty() ? std::string("(none)") : out;
	}
}

int64_t botAudEpochMs() {
	return BotAuditSink::epochMs();
}

void botAudBegin(const char* kind, const BotState* bot) {
	if (s_audBuf.size() > AUD_BUF_CAP) {
		// The sink has not drained for a very long time (it never should: auditTick flushes every
		// tick). Never grow without bound — count and drop.
		s_audDropped += static_cast<uint64_t>(std::count(s_audBuf.begin(), s_audBuf.end(), '\n'));
		s_audBuf.clear();
	}
	auto out = std::back_inserter(s_audBuf);
	fmt::format_to(out, R"({{"v":1,"ts":{},"seq":{},"src":"srv","k":"{}")",
		BotAuditSink::epochMs(), BotAuditSink::get().nextSeq(), kind);
	if (bot) {
		fmt::format_to(out, R"(,"guid":{},"cid":{},"bot":)", bot->guid, 0x10000000u + bot->guid);
		BotAuditSink::appendJsonString(s_audBuf, bot->name);
		fmt::format_to(out, R"(,"pos":[{},{},{}],"st":"{}","hib":{})",
			bot->currentPos.x, bot->currentPos.y, static_cast<int>(bot->currentPos.z),
			botStateName(bot->state), bot->hibernated ? 1 : 0);
	}
}

void botAudEnd() {
	s_audBuf += "}\n";
	if (s_audBuf.size() >= AUD_FLUSH_AT) {
		botAudFlush();
	}
}

void botAudFlush() {
	// s_audAlive: false once this module's statics are being torn down (process exit can run
	// ~BotEngine after them) — never touch a destroyed buffer.
	if (!s_audAlive || s_audBuf.empty()) {
		return;
	}
	BotAuditSink::get().write(s_audBuf); // no-op (discard) while the sink is disabled
	s_audBuf.clear();
}

void botAudStr(const char* key, std::string_view v) {
	s_audBuf += ",\"";
	s_audBuf += key;
	s_audBuf += "\":";
	BotAuditSink::appendJsonString(s_audBuf, v);
}

void botAudInt(const char* key, int64_t v) {
	fmt::format_to(std::back_inserter(s_audBuf), R"(,"{}":{})", key, v);
}

void botAudPos(const char* key, const Position& p) {
	fmt::format_to(std::back_inserter(s_audBuf), R"(,"{}":[{},{},{}])", key, p.x, p.y, static_cast<int>(p.z));
}

void botAudNoteLog(uint32_t guid, const std::string& msg) {
	auto& w = s_audBot[guid];
	w.lastLog.assign(BotAuditSink::utf8Prefix(msg, 200)); // never split a multi-byte character
	w.lastLogMs = botAudEpochMs();
}

void botAudWalkPause(const BotState& bot, bool observed, int32_t ms, uint8_t walkBias, uint32_t nThisRoute, size_t steps,
                     const char* at) {
	botAudBegin("walkpause", &bot);
	botAudStr("tier", observed ? "observed" : "unobserved");
	// #120: "issue" = decided when the walk was issued; "midwalk" = decided on the first tick an
	// already-running walk became observed (the walk was stopped in place and resumes after it).
	botAudStr("at", at ? at : "issue");
	botAudInt("ms", ms);
	botAudInt("walkBias", walkBias);
	botAudInt("nThisRoute", nThisRoute);
	botAudInt("steps", static_cast<int64_t>(steps));
	botAudEnd();
}

void botAudPathout(const BotState& bot, const char* outcome, const std::string& route, size_t idx, size_t n,
                   const char* reason, int64_t extra) {
	botAudBegin("pathout", &bot);
	botAudStr("outcome", outcome);
	botAudStr("route", route);
	botAudInt("wp", static_cast<int64_t>(idx));
	botAudInt("wps", static_cast<int64_t>(n));
	if (reason) {
		botAudStr("reason", reason);
	}
	if (extra >= 0) {
		botAudInt("val", extra);
	}
	if (bot.hasWalkTarget) {
		botAudPos("walk", bot.walkTarget);
	}
	botAudEnd();
}

void botAudDecide(const BotState& bot, const char* what, std::string_view chosen, int64_t candidates, std::string_view detail) {
	botAudBegin("decide", &bot);
	botAudStr("what", what);
	botAudStr("chosen", chosen);
	if (candidates >= 0) {
		botAudInt("n", candidates);
	}
	if (!detail.empty()) {
		botAudStr("detail", detail);
	}
	botAudEnd();
}

void botAudReroll(const BotState& bot, int roll, int total, const int (&w)[5], bool huntOnCD, bool pzLocked, uint8_t outcome) {
	const char* chosen = outcome < std::size(kActOutcomeNames) ? kActOutcomeNames[outcome] : "none";
	botAudBegin("decide", &bot);
	botAudStr("what", "reroll");
	botAudStr("chosen", chosen);
	botAudInt("roll", roll);
	botAudInt("total", total);
	fmt::format_to(std::back_inserter(s_audBuf), R"(,"w":{{"dwell":{},"poi":{},"hunt":{},"party":{},"travel":{}}})",
		w[0], w[1], w[2], w[3], w[4]);
	// Reject reasons doActivityReroll already computed (no new work): bins zeroed by eligibility.
	std::string rej;
	if (huntOnCD) {
		rej += "hunt_cooldown";
	}
	if (pzLocked) {
		rej += rej.empty() ? "pz_locked" : ",pz_locked";
	}
	if (!rej.empty()) {
		botAudStr("rej", rej);
	}
	if (bot.hasWalkTarget) {
		botAudPos("walk", bot.walkTarget);
	}
	if (bot.currentPOI) {
		botAudStr("poi", bot.currentPOI->name);
	}
	if (bot.huntScriptId) {
		botAudInt("hunt", bot.huntScriptId);
	}
	if (bot.travelDestTownId) {
		botAudInt("travelTo", bot.travelDestTownId);
	}
	const int64_t gnow = OTSYS_TIME();
	if (bot.dwellUntil > gnow) {
		botAudInt("dwellMs", bot.dwellUntil - gnow);
	}
	botAudEnd();
}

void BotAudPathScope::emit() noexcept {
	try {
		const uint64_t us = botMonoUs() - t0;
		botAudBegin("path", bot);
		botAudStr("planner", planner);
		botAudPos("src", src);
		botAudPos("dst", dst);
		botAudInt("arrival", arrival);
		botAudStr("result", res);
		if (len >= 0) {
			botAudInt("len", len);
		}
		if (expanded >= 0) {
			botAudInt("expanded", expanded);
		}
		fmt::format_to(std::back_inserter(s_audBuf), R"(,"ms":{:.3f})", static_cast<double>(us) / 1000.0);
		botAudEnd();
	} catch (...) {
		// a destructor must not throw (bad_alloc while formatting would terminate the server)
	}
}

const BotState* BotEngine::auditBotByGuid(uint32_t guid) const {
	auto it = guidToIndex_.find(guid);
	return it != guidToIndex_.end() ? &bots_[it->second] : nullptr;
}

void BotEngine::auditRefreshCache(uint32_t gen) {
	auto& sink = BotAuditSink::get();
	s_aud.gen = gen;
	const std::string kinds = sink.kinds();
	const std::string watch = sink.watch();
	uint32_t mask = 0;
	if (sink.enabled()) {
		for (const auto& tok : audSplitCsv(kinds)) {
			const std::string k = audLower(tok);
			for (const auto& kn : kAudKinds) {
				if (k == "all" || k == "*" || k == kn.name) {
					mask |= kn.bit;
				}
			}
		}
	}
	s_aud.probeMs = std::max<int64_t>(100, sink.probeMs());
	s_aud.censusMs = std::max<int64_t>(1000, sink.censusMs());
	s_aud.watchAll = false;
	s_aud.watchNamesLower.clear();
	for (const auto& tok : audSplitCsv(watch)) {
		const std::string w = audLower(tok);
		if (w == "*") {
			s_aud.watchAll = true;
		} else {
			s_aud.watchNamesLower.push_back(w);
		}
	}
	s_aud.mask = mask;
	s_aud.watch.clear();
	if (mask == 0) {
		s_audBot.clear();
		s_audFirstActPending = 0;
		return;
	}
	auditResolveWatch(botAudEpochMs());
	std::erase_if(s_audBot, [](const auto& kv) { return !s_aud.watchAll && !s_aud.watch.contains(kv.first); });
	s_audFirstActPending = 0;
	for (const auto& [g, ab] : s_audBot) {
		if (ab.wakeAtMs != 0) s_audFirstActPending++;
	}
	s_audLastCensusMs = 0; // first census right after (re)enabling
	// Settings marker: tells a reader exactly when a window started and what it was recording.
	botAudBegin("audit", nullptr);
	botAudStr("kinds", audMaskNames(mask));
	botAudStr("watch", watch);
	botAudInt("watchResolved", static_cast<int64_t>(s_aud.watch.size()));
	botAudInt("gen", gen);
	botAudEnd();
}

void BotEngine::auditResolveWatch(int64_t nowMs) {
	s_aud.lastResolveMs = nowMs;
	s_aud.watch.clear();
	if (s_aud.watchNamesLower.empty()) {
		return;
	}
	for (const auto& b : bots_) {
		if (b.name.empty()) {
			continue;
		}
		const std::string ln = audLower(b.name);
		for (const auto& w : s_aud.watchNamesLower) {
			if (w == ln) {
				s_aud.watch.insert(b.guid);
				break;
			}
		}
	}
}

void BotEngine::auditTick() {
	const uint32_t gen = BotAuditSink::get().generation();
	if (gen != s_aud.gen) [[unlikely]] {
		auditRefreshCache(gen);
	}
	if (s_aud.mask == 0) [[likely]] {
		if (!s_audBuf.empty()) {
			botAudFlush();
		}
		return;
	}
	const int64_t now = botAudEpochMs();
	// Re-resolve names periodically: a watched bot can register/unregister after the toggle.
	if (!s_aud.watchNamesLower.empty() && now - s_aud.lastResolveMs >= 5000) {
		auditResolveWatch(now);
	}
	if (s_aud.mask & (AUD_PROBE | AUD_STATE | AUD_PATHOUT)) {
		if (s_aud.watchAll) {
			// "*": state + oscillation for every ACTIVE bot (a compare per bot per tick). The probe
			// (and path/walkpause, via audFor) only for bots named explicitly.
			for (auto& b : bots_) {
				if (b.active) {
					auditVisit(b, now, s_aud.watch.contains(b.guid));
				}
			}
		} else {
			for (const uint32_t g : s_aud.watch) {
				auto it = guidToIndex_.find(g);
				if (it != guidToIndex_.end()) {
					auditVisit(bots_[it->second], now, true);
				}
			}
		}
	}
	if (audOn(AUD_CENSUS) && now - s_audLastCensusMs >= s_aud.censusMs) {
		s_audLastCensusMs = now;
		auditCensus(now);
	}
	if (s_audDropped > 0) {
		botAudBegin("audit", nullptr);
		botAudInt("dropped", static_cast<int64_t>(s_audDropped));
		botAudEnd();
		s_audDropped = 0;
	}
	botAudFlush();
}

// One watched bot, once per engine tick (100 ms). Covers live AND hibernated bots, so the
// virtual-sim twin's state changes are recorded by the very same diff as the live handler's.
void BotEngine::auditVisit(BotState& b, int64_t now, bool probe) {
	auto& w = s_audBot[b.guid];

	// ---- state: sampled diff (there is no central setter: ~95 direct assignments) ----
	const bool phaseChanged = w.ph != b.huntPhase && (b.state == BotAIState::HUNTING || w.st == BotAIState::HUNTING);
	if (w.init && (w.st != b.state || phaseChanged) && (s_aud.mask & AUD_STATE)) {
		botAudBegin("state", &b);
		botAudStr("from", botStateName(w.st));
		botAudStr("to", botStateName(b.state));
		if (phaseChanged || b.state == BotAIState::HUNTING) {
			botAudStr("pfrom", huntPhaseName(w.ph));
			botAudStr("pto", huntPhaseName(b.huntPhase));
		}
		const bool fresh = !w.lastLog.empty() && now - w.lastLogMs <= 2000;
		botAudStr("reason", fresh ? std::string_view(w.lastLog) : std::string_view(b.hibernated ? "virtual-sim" : "unlogged"));
		botAudStr("detect", "tick-diff");
		botAudEnd();
	}
	w.init = true;
	w.st = b.state;
	w.ph = b.huntPhase;

	// ---- pathout osc: period-2/3 position cycling (awake bots; live position) ----
	if ((s_aud.mask & AUD_PATHOUT) && !b.hibernated) {
		const auto player = b.getPlayer();
		const Position p = player ? player->getPosition() : b.currentPos;
		if (w.ringN == 0 || w.ring[0] != p) {
			const bool p2 = w.ringN >= 2 && w.ring[1] == p;
			const bool p3 = w.ringN >= 3 && w.ring[2] == p && w.ring[1] != p;
			w.osc2 = p2 ? static_cast<uint8_t>(std::min<int>(w.osc2 + 1, 250)) : 0;
			w.osc3 = p3 ? static_cast<uint8_t>(std::min<int>(w.osc3 + 1, 250)) : 0;
			for (int i = 3; i > 0; --i) {
				w.ring[i] = w.ring[i - 1];
			}
			w.ring[0] = p;
			if (w.ringN < 4) {
				w.ringN++;
			}
			// 4 consecutive returns = A B A B A B (period 2) or A B C A B C A (period 3)
			const int period = w.osc2 >= 4 ? 2 : (w.osc3 >= 4 ? 3 : 0);
			if (period != 0 && now - w.oscLastEmitMs >= 10000) {
				w.oscLastEmitMs = now;
				botAudBegin("pathout", &b);
				botAudStr("outcome", "osc");
				botAudInt("period", period);
				botAudInt("returns", period == 2 ? w.osc2 : w.osc3);
				botAudPos("a", w.ring[1]);
				botAudPos("b", w.ring[0]);
				if (b.hasWalkTarget) {
					botAudPos("walk", b.walkTarget);
				}
				botAudEnd();
				w.osc2 = 0;
				w.osc3 = 0;
			}
		}
	}

	// ---- probe ----
	if (probe && (s_aud.mask & AUD_PROBE) && now - w.lastProbeMs >= s_aud.probeMs) {
		w.lastProbeMs = now;
		auditProbe(b, w, now);
	}
}

void BotEngine::auditProbe(BotState& b, BotAuditBotState& w, int64_t now) {
	const auto player = b.hibernated ? nullptr : b.getPlayer();
	botAudBegin("probe", &b);
	botAudStr("ph", huntPhaseName(b.huntPhase));
	botAudInt("fc", static_cast<int>(b.fcState));
	botAudInt("tgt", b.huntTargetId);
	botAudInt("atk", b.attackerId);
	if (b.hasWalkTarget) {
		botAudPos("walk", b.walkTarget);
	}
	botAudInt("pathLen", player ? static_cast<int64_t>(player->listWalkDir.size()) : -1);
	botAudInt("awake", player ? 1 : 0);
	if (player) {
		// Same observer predicate as botWalkObserved, recomputed here WITHOUT touching its
		// 500 ms cache so the probe cannot shift walk-pause timing.
		bool observed = false;
		if (!currentAnchors_.empty()) {
			// Named copy, exactly like botWalkObserved: iterating find()'s result on the temporary
			// directly could dangle (find returns a reference into the temporary).
			auto spectators = Spectators().find<Player>(player->getPosition(), true);
			for (const auto& s : spectators) {
				const auto other = s ? s->getPlayer() : nullptr;
				if (!other || other->getID() == player->getID()) {
					continue;
				}
				if (!other->isBotPlayer() || other->getCastViewerCount() > 0) {
					observed = true;
					break;
				}
			}
		}
		botAudInt("obs", observed ? 1 : 0);
		botAudInt("cast", player->getCastViewerCount());
		const int32_t maxHp = player->getMaxHealth();
		botAudInt("hpPct", maxHp > 0 ? player->getHealth() * 100 / maxHp : 0);
		const Position lp = player->getPosition();
		if (lp != b.currentPos) {
			botAudPos("live", lp);
		}
	}
	botAudInt("tickUs", w.tickUs);
	if (b.huntScriptId) {
		botAudInt("hunt", b.huntScriptId);
		botAudInt("wp", static_cast<int64_t>(b.huntWaypointIdx));
		botAudInt("kills", b.huntKillCount);
	}
	if (b.followingCityRoute) {
		botAudInt("routeIdx", static_cast<int64_t>(b.cityRouteIdx));
		botAudInt("routeLen", static_cast<int64_t>(b.cityRouteWps.size()));
	}
	if (!b.travelPhase.empty()) {
		botAudStr("travel", b.travelPhase);
	}
	if (b.wakeQuietTicks) {
		botAudInt("quiet", b.wakeQuietTicks);
	}
	const int64_t gnow = OTSYS_TIME();
	if (b.nextRerollTime > gnow) {
		botAudInt("rerollInMs", b.nextRerollTime - gnow);
	}
	if (b.dwellUntil > gnow) {
		botAudInt("dwellMs", b.dwellUntil - gnow);
	}
	if (b.pendingWalkPauseEventId != 0) {
		botAudInt("pausing", 1);
	}
	if (!w.lastLog.empty()) {
		botAudStr("lastAct", w.lastLog);
		botAudInt("lastActAgeMs", now - w.lastLogMs);
	}
	botAudEnd();
}

// Called from the tick loop the first time a watched bot runs its AI after a wake.
void BotEngine::auditWakeFirstAct(BotState& bot) {
	auto it = s_audBot.find(bot.guid);
	if (it == s_audBot.end() || it->second.wakeAtMs == 0) {
		return;
	}
	const int64_t dt = botAudEpochMs() - it->second.wakeAtMs;
	it->second.wakeAtMs = 0;
	if (s_audFirstActPending > 0) s_audFirstActPending--;
	if (!audFor(AUD_WAKE, bot.guid)) {
		return;
	}
	botAudBegin("wake", &bot);
	botAudStr("event", "first_act");
	botAudInt("firstActMs", dt);
	botAudEnd();
}

// Whole-population aggregate (plan R10): every registered bot, hibernated included. ~1000
// struct reads every botAuditCensusSec; no Player lookups except one per awake bot (cast count).
void BotEngine::auditCensus(int64_t /*nowMs*/) {
	uint32_t active = 0, awake = 0, hib = 0, cast = 0, paused = 0;
	uint32_t byState[9] = {};
	uint32_t phaseAwake[5] = {};
	uint32_t phaseHib[5] = {};
	std::map<uint32_t, std::pair<uint32_t, uint32_t>> byTown; // townId -> (awake, hib)
	struct HuntAgg {
		uint32_t n = 0, patrolling = 0, kills = 0, pctN = 0;
		uint64_t pctSum = 0;
	};
	HuntAgg vh;
	HuntAgg lh;
	std::unordered_map<uint32_t, size_t> patrolLen;
	patrolLen.reserve(huntScripts_.size());
	for (const auto& s : huntScripts_) {
		patrolLen[s.id] = s.patrolWaypoints.size();
	}
	for (const auto& b : bots_) {
		if (!b.active) {
			continue;
		}
		active++;
		if (b.aiPaused) {
			paused++;
		}
		byState[std::min<size_t>(static_cast<size_t>(b.state), 8)]++;
		auto& t = byTown[b.townId];
		if (b.hibernated) {
			hib++;
			t.second++;
		} else {
			awake++;
			t.first++;
			if (auto p = b.getPlayer(); p && p->getCastViewerCount() > 0) {
				cast++;
			}
		}
		if (b.state == BotAIState::HUNTING) {
			const auto pi = std::min<size_t>(static_cast<size_t>(b.huntPhase), 4);
			if (b.hibernated) {
				phaseHib[pi]++;
			} else {
				phaseAwake[pi]++;
			}
			HuntAgg& h = b.hibernated ? vh : lh;
			h.n++;
			h.kills += b.huntKillCount;
			if (b.huntPhase == HuntPhase::PATROLLING) {
				h.patrolling++;
				auto it = patrolLen.find(b.huntScriptId);
				if (it != patrolLen.end() && it->second > 0) {
					h.pctSum += std::min<size_t>(b.huntWaypointIdx, it->second) * 100 / it->second;
					h.pctN++;
				}
			}
		}
	}
	botAudBegin("census", nullptr);
	botAudInt("reg", static_cast<int64_t>(bots_.size()));
	botAudInt("active", active);
	botAudInt("awake", awake);
	botAudInt("hibn", hib);
	botAudInt("cast", cast);
	botAudInt("paused", paused);
	auto out = std::back_inserter(s_audBuf);
	s_audBuf += R"(,"state":{)";
	bool first = true;
	for (size_t i = 0; i < 9; ++i) {
		if (byState[i] == 0) {
			continue;
		}
		fmt::format_to(out, R"({}"{}":{})", first ? "" : ",", botStateName(static_cast<BotAIState>(i)), byState[i]);
		first = false;
	}
	s_audBuf += R"(},"phase":{)";
	first = true;
	for (size_t i = 0; i < 5; ++i) {
		if (phaseAwake[i] == 0 && phaseHib[i] == 0) {
			continue;
		}
		fmt::format_to(out, R"({}"{}":[{},{}])", first ? "" : ",", huntPhaseName(static_cast<HuntPhase>(i)),
			phaseAwake[i], phaseHib[i]);
		first = false;
	}
	s_audBuf += R"(},"town":{)";
	first = true;
	for (const auto& [town, ah] : byTown) {
		fmt::format_to(out, R"({}"{}":[{},{}])", first ? "" : ",", town, ah.first, ah.second);
		first = false;
	}
	s_audBuf += "}";
	const auto hunt = [&out](const char* key, const HuntAgg& h) {
		fmt::format_to(out, R"(,"{}":{{"n":{},"patrolling":{},"kills":{},"wpPct":{}}})", key, h.n, h.patrolling,
			h.kills, h.pctN ? static_cast<int64_t>(h.pctSum / h.pctN) : -1);
	};
	hunt("vhunt", vh); // virtual (hibernated) hunting twins
	hunt("lhunt", lh); // live (awake) hunters
	botAudInt("anchors", static_cast<int64_t>(currentAnchorPts_.size()));
	botAudInt("realPlayers", botPerf_.gaugeRealPlayers);
	botAudInt("wakeTried60s", wakeTried60s_);
	botAudInt("wakeGated60s", wakeGated60s_);
	botAudInt("wakeGranted60s", wakeGranted60s_);
	botAudInt("wakeDeferred60s", wakeDeferred60s_);
	botAudEnd();
}

// `/cavebot audit on|off|status|watch <bot>|watch clear|unwatch <bot>|kinds <list>|config|flush|mark <text>`
// Runtime toggles live in the main-binary sink: in memory, they survive /cavebot reload and reset
// on restart. config.lua (botAudit*) stays the source of the defaults; `config` re-applies it.
std::string BotEngine::auditCommand(const std::string& argsIn) {
	auto& sink = BotAuditSink::get();
	const std::string args = audTrim(argsIn);
	const auto sp = args.find(' ');
	const std::string verb = audLower(args.substr(0, sp));
	std::string rest = sp == std::string::npos ? std::string() : audTrim(args.substr(sp + 1));
	if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
		rest = rest.substr(1, rest.size() - 2);
	}
	std::string note;

	if (verb.empty() || verb == "status") {
		// status reply below
	} else if (verb == "on") {
		sink.setEnabled(true);
		note = "audit ON";
	} else if (verb == "off") {
		botAudFlush();
		sink.setEnabled(false);
		note = "audit OFF";
	} else if (verb == "watch") {
		const std::string lr = audLower(rest);
		if (rest.empty()) {
			// list only
		} else if (lr == "clear" || lr == "none") {
			sink.setWatch("");
			note = "watch list cleared";
		} else {
			bool known = rest == "*";
			for (const auto& b : bots_) {
				if (!known && audLower(b.name) == lr) {
					known = true;
				}
			}
			sink.addWatch(rest);
			note = fmt::format("watching '{}'{}", rest, known ? "" : " (no registered bot has that name yet)");
		}
	} else if (verb == "unwatch") {
		note = sink.removeWatch(rest) ? fmt::format("unwatched '{}'", rest) : fmt::format("'{}' was not watched", rest);
	} else if (verb == "kinds") {
		if (rest.empty()) {
			return "usage: /cavebot audit kinds all | <comma list of probe,state,wake,hib,walkpause,path,pathout,decide,cmd,census>";
		}
		std::string unknown;
		for (const auto& tok : audSplitCsv(rest)) {
			const std::string k = audLower(tok);
			bool ok = k == "all" || k == "*";
			for (const auto& kn : kAudKinds) {
				ok = ok || k == kn.name;
			}
			if (!ok) {
				if (!unknown.empty()) {
					unknown.push_back(',');
				}
				unknown += k;
			}
		}
		sink.setKinds(rest);
		note = unknown.empty() ? std::string("kinds set") : "kinds set (UNKNOWN, ignored: " + unknown + ")";
	} else if (verb == "config") {
		sink.applyConfig();
		note = "config.lua botAudit* re-applied (runtime toggles discarded)";
	} else if (verb == "flush") {
		botAudFlush();
		note = "flushed";
	} else if (verb == "mark") {
		// Scenario markers for the harness ("begin <runId>" / "end <runId>"): written while on.
		if (sink.enabled()) {
			botAudBegin("mark", nullptr);
			botAudStr("text", rest);
			botAudEnd();
			botAudFlush();
			note = "marked";
		} else {
			note = "audit is off - mark not written";
		}
	} else {
		return "usage: /cavebot audit on|off|status|watch <bot>|watch clear|unwatch <bot>|kinds <list>|config|flush|mark <text>";
	}

	// Apply now rather than at the next tick, so the reply below shows the effective state and the
	// `audit` settings marker lands before anything recorded under the new settings.
	const uint32_t gen = sink.generation();
	if (gen != s_aud.gen) {
		auditRefreshCache(gen);
		botAudFlush();
	}
	std::string resolved;
	for (const uint32_t g : s_aud.watch) {
		if (const BotState* b = auditBotByGuid(g)) {
			resolved += fmt::format("{}{}({}{})", resolved.empty() ? "" : ",", b->name, g, b->hibernated ? ",hib" : "");
		}
	}
	return fmt::format("{}{}\n[BOT_AUDIT] engine: kinds={} watchResolved={}{} probeMs={} buffered={}B",
		note.empty() ? std::string() : "[BOT_AUDIT] " + note + "\n", sink.status(), audMaskNames(s_aud.mask),
		resolved.empty() ? std::string("(none)") : resolved, s_aud.watchAll ? " +ALL(*)" : "", s_aud.probeMs,
		s_audBuf.size());
}
