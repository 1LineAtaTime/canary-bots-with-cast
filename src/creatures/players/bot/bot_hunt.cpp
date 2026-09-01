/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_hunt.cpp — hunt phase machine (PREPARING -> TRAVEL_TO -> PATROLLING -> LEAVING -> RESUPPLYING)
//
// BOT_NAV_REALISM Phase 11 module split. Compiles into the SAME libbot_engine.so
// as bot_engine.cpp, so /cavebot reload is unchanged. Shared includes, engine-local
// types and the BotEngine class declaration all live in bot_engine_impl.hpp.
//
// Carved out only after tools/botnavsim/module_promote.py reported zero external
// dependencies for this range.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

bool BotEngine::tryStartHunt(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return false;

	int32_t level = player->getLevel();

	// Find eligible hunts (level-only filter — vocation and town are not filtered)
	// Quest bots pick quest scripts, regular bots pick hunt scripts
	// Traveling scripts are handled separately (not through tryStartHunt)
	std::string wantedCategory = bot.isQuestBot ? "quest" : "hunt";

	std::vector<const HuntScript*> eligible;
	for (const auto& script : huntScripts_) {
		if (!script.enabled) continue;
		if (script.patrolWaypoints.empty()) continue;

		// Script category filter — quest bots get quests, regular bots get hunts
		if (script.scriptCategory != wantedCategory) continue;

		// Level filter
		if (script.levelMin > 0 && level < static_cast<int32_t>(script.levelMin)) continue;
		if (script.levelMax > 0 && level > static_cast<int32_t>(script.levelMax)) continue;

		if (script.isQuest || script.scriptCategory == "quest") {
			// Quests are shared, not reserved: a walkthrough is not a spawn to camp, so the
			// only gate is how soon the next bot may start the same one.
			if (botQuestOnCooldown(script.id)) continue;
		} else {
			// 1-bot-per-spawn
			if (activeHunts_.count(script.id)) continue;
			if (!script.spawnGroup.empty() && activeSpawnGroups_.count(script.spawnGroup)) continue;
		}
		if (isScriptPlayerClaimed(script.id, script.spawnGroup)) continue; // player spawn-claim
		if (isScriptHuntRepelled(script)) continue; // hunt-flagged player nearby

		// Empty targetNames is allowed — bot attacks all monsters during PATROLLING

		eligible.push_back(&script);
	}

	// Quest bots with no eligible quests fall back to regular hunts
	if (eligible.empty() && bot.isQuestBot) {
		for (const auto& script : huntScripts_) {
			if (!script.enabled) continue;
			if (script.patrolWaypoints.empty()) continue;
			if (script.scriptCategory != "hunt") continue;
			if (script.levelMin > 0 && level < static_cast<int32_t>(script.levelMin)) continue;
			if (script.levelMax > 0 && level > static_cast<int32_t>(script.levelMax)) continue;
			if (activeHunts_.count(script.id)) continue;
			if (!script.spawnGroup.empty() && activeSpawnGroups_.count(script.spawnGroup)) continue;
			if (isScriptPlayerClaimed(script.id, script.spawnGroup)) continue; // player spawn-claim
			if (isScriptHuntRepelled(script)) continue; // hunt-flagged player nearby
			// Empty targetNames is allowed — bot attacks all monsters during PATROLLING
			eligible.push_back(&script);
		}
	}

	if (eligible.empty()) return false;

	int idx = uniform_random(0, static_cast<int>(eligible.size()) - 1);
	const HuntScript* selected = eligible[idx];
	const bool selectedIsQuest = selected->isQuest || selected->scriptCategory == "quest";

	// Reserve (hunts) or stamp the shared-quest cooldown
	if (selectedIsQuest) {
		botStampQuestStart(selected->id);
	} else {
		activeHunts_[selected->id] = bot.guid;
		if (!selected->spawnGroup.empty()) {
			activeSpawnGroups_[selected->spawnGroup] = bot.guid;
		}
	}

	// Set up hunt state
	bot.huntScriptId = selected->id;
	logHuntAssign(bot, selected->id);
	bot.huntTownId = selected->townId;
	bot.huntStartTime = OTSYS_TIME();
	// A quest must not be cut mid-route by the ordinary hunt clock — it runs its three
	// phases once and is bounded by the quest safety ceiling, with QUEST_END_MARGIN_SEC
	// reserved so travel_to and travel_from actually fit. See the budget table in
	// bot_engine_impl.hpp.
	bot.huntEndTime = selectedIsQuest
		? botQuestHuntEndTime(bot.huntStartTime)
		: bot.huntStartTime + uniform_random(g_configManager().getNumber(BOT_HUNT_TIME_MIN_SEC), g_configManager().getNumber(BOT_HUNT_TIME_MAX_SEC)) * 1000LL;
	bot.huntKillCount = 0;
	bot.huntWaypointIdx = 0;
	bot.huntPatrolCycles = 0;
	bot.huntTargetId = 0;
	bot.huntChaseFailCount = 0;
	bot.huntIgnoredMonsters.clear();
	bot.huntWaypointSkipCount = 0;

	// If hunt is in different town, travel there first
	if (selected->townId != bot.townId) {
		bot.pendingHuntAfterTravel = true;
		startTravel(bot, selected->townId);
		g_logger().info("[BotEngine] {} starting hunt '{}' — traveling to town {}",
			player->getName(), selected->name, selected->townId);
		return true;
	}

	// Same town — begin preparing phase
	beginHuntPhase(bot, HuntPhase::PREPARING);
	g_logger().info("[BotEngine] {} starting hunt '{}' — same town {} (level={})",
		player->getName(), selected->name, bot.townId, level);
	return true;
}

bool BotEngine::tryStartCityWalk(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return false;

	// Find eligible traveling scripts that start from this bot's town
	std::vector<const HuntScript*> eligible;
	for (const auto& script : huntScripts_) {
		if (!script.enabled) continue;
		if (script.scriptCategory != "traveling") continue;
		if (script.townId != bot.townId) continue;

		// Travel waypoints stored in travelToWaypoints (single phase)
		if (script.travelToWaypoints.empty()) continue;

		// 1-bot-per-route reservation
		if (activeHunts_.count(script.id)) continue;
		if (!script.spawnGroup.empty() && activeSpawnGroups_.count(script.spawnGroup)) continue;
		if (isScriptPlayerClaimed(script.id, script.spawnGroup)) continue; // player spawn-claim
		if (isScriptHuntRepelled(script)) continue; // hunt-flagged player nearby

		eligible.push_back(&script);
	}

	if (eligible.empty()) return false;

	// Pick random route
	const HuntScript* selected = eligible[uniform_random(0, static_cast<int>(eligible.size()) - 1)];

	// Reserve
	activeHunts_[selected->id] = bot.guid;
	if (!selected->spawnGroup.empty()) {
		activeSpawnGroups_[selected->spawnGroup] = bot.guid;
	}

	// Set up as a hunt with TRAVEL_TO phase — one-shot (no patrol, no resupply)
	bot.huntScriptId = selected->id;
	logHuntAssign(bot, selected->id);
	bot.huntTownId = selected->townId;
	bot.huntStartTime = OTSYS_TIME();
	bot.huntEndTime = bot.huntStartTime + 30 * 60 * 1000LL; // 30 min timeout
	bot.huntKillCount = 0;
	bot.huntWaypointIdx = 0;
	bot.huntPatrolCycles = 0;
	bot.huntTargetId = 0;
	bot.huntChaseFailCount = 0;
	bot.huntIgnoredMonsters.clear();
	bot.huntWaypointSkipCount = 0;

	// Start directly in TRAVEL_TO phase (skip PREPARING — no depot/shop needed)
	castLog(bot, fmt::format("CITY WALK START: '{}'", selected->name));
	beginHuntPhase(bot, HuntPhase::TRAVEL_TO);
	return true;
}

void BotEngine::beginHuntPhase(BotState& bot, HuntPhase phase, size_t preChosenPatrolIdx) {
	bot.state = BotAIState::HUNTING;
	bot.huntPhase = phase;
	bot.huntWaypointIdx = 0;
	// One safeTeleportLanding rewind per route entry. Deliberately NOT reset when this is the
	// nested PATROLLING self-call from the TRAVEL_TO case, so one logical route entry cannot
	// refill the budget twice within a single call stack.
	if (!(phase == HuntPhase::PATROLLING && preChosenPatrolIdx != kNoPatrolIdx)) {
		resetTpRewindBudget(bot.guid);
	}
	// Opportunistic ice-fishing cleanup. Covers the routine `time up -> LEAVING` exit in
	// doHuntPatrol, which does NOT go through abortHunt. This is not a funnel — 16 sites assign
	// bot.huntPhase directly — so it is early cleanup, not the guarantee; tickIceFishSession's
	// own deadline/adjacency/combat checks are what make a missed erase harmless.
	endIceFishSession(bot, "hunt phase change");
	// BOT_CORPSE_LOOT: same opportunistic-cleanup contract as the line above — not a
	// funnel, just early cleanup. Every loot pass re-validates its gate and re-locks its
	// weak_ptrs, so a missed clear is harmless.
	clearLootState(bot.guid);

	// Clear city route state between phases
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.pathFailCount = 0;

	auto player = bot.getPlayer();

	switch (phase) {
		case HuntPhase::PREPARING:
			// Initialize preparation state machine (depot + shop visits)
			bot.prepareStartTime = OTSYS_TIME();
			bot.prepareStep = 0;  // 0=walk_depot
			bot.prepareWaitUntil = 0;
			bot.prepareHasTarget = false;
			castLog(bot, "PREPARE: Starting preparation (depot + shop)");
			break;

		case HuntPhase::TRAVEL_TO: {
			// Check if we need to teleport to first patrol waypoint
			const HuntScript* script = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { script = &s; break; }
			}
			if (!script || (script->patrolWaypoints.empty() && script->scriptCategory != "traveling")) {
				abortHunt(bot, "no patrol waypoints");
				return;
			}

			// Follow travel_to waypoints if they exist
			if (!script->travelToWaypoints.empty()) {
				bot.huntWaypointIdx = 0;
				s_huntTravelStart[bot.guid] = OTSYS_TIME();
				castLog(bot, fmt::format("TRAVEL_TO: Following {} travel waypoints to spawn",
					script->travelToWaypoints.size()));
				// Don't chain — let doHuntTravel() handle the waypoint following
				break;
			}

			// No travel_to waypoints — if >30 tiles from the patrol ENTRY wp, teleport there.
			// Measured against the entry waypoint, not wp[0]: testing wp[0] and then starting
			// at a different index is the bug this phase exists to close. NOTE this branch
			// re-enters beginHuntPhase recursively, so the index has to be threaded through
			// the nested call.
			size_t entryIdx = botPatrolEntryIdx(script->patrolWaypoints, *script);
			auto& entryWp = script->patrolWaypoints[entryIdx].pos;
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(entryWp.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(entryWp.y)));
			if (dist > 30 || bot.currentPos.z != entryWp.z) {
				if (player) {
					Position landing = safeTeleportLanding(bot, entryWp, &script->patrolWaypoints,
						&entryIdx, "huntStart");
					BOT_TELEPORT(player, landing, true);
					bot.currentPos = landing;
					castLog(bot, fmt::format("TRAVEL_TO: Teleported to patrol entry wp {}/{} ({},{},{})",
						entryIdx + 1, script->patrolWaypoints.size(), landing.x, landing.y, landing.z));
					trackNavEvent("hunt_teleport_patrol", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
						bot.townId, "travel_to", fmt::format("no travel wps, dist={} entry={}", dist, entryIdx));
				}
			}

			// Go directly to patrolling, entering at the waypoint we just placed the bot on
			beginHuntPhase(bot, HuntPhase::PATROLLING, entryIdx);
			break;
		}

		case HuntPhase::PATROLLING:
			// BOT_NAV_REALISM Phase 4b: route phase desync. Ported from evejs-living-universe,
			// which scatters each pilot along its own route at population-build time so a fleet
			// never moves in lockstep. Jitter (4a) varies WHICH tiles a bot walks and lane (7)
			// varies WHERE in the street, but bots entering the same patrol loop together still
			// travel as a temporal cohort. Starting at a random loop phase fixes that.
			//
			// The original comment here claimed the scatter was "free because the bot is
			// teleported/placed at the patrol start anyway". That was false at all four
			// teleport sites: they place the bot on patrolWaypoints[0] and this case then
			// re-rolled a DIFFERENT index, so the bot was told to walk to a waypoint it had
			// not been placed near — often hundreds of tiles and several floors away, which
			// followWaypoints' 200-tile sanity guard then aborted on the next tick.
			//
			// Ownership therefore moves to the caller: a site that physically places the bot
			// passes the index it placed it on. Callers that don't (and the ones that never
			// move the bot) keep the roll via the defaulted sentinel.
			bot.huntWaypointIdx = 0;
			{
				const HuntScript* pscript = nullptr;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { pscript = &s; break; }
				}
				if (pscript) {
					bot.huntWaypointIdx =
						(preChosenPatrolIdx != kNoPatrolIdx
							&& preChosenPatrolIdx < pscript->patrolWaypoints.size())
						? preChosenPatrolIdx
						: botPatrolEntryIdx(pscript->patrolWaypoints, *pscript);
				}
			}
			bot.huntPatrolCycles = 0;
			bot.lastKillTime = 0;
			castLog(bot, fmt::format("HUNT: Starting patrol (script={}, startWp={})",
				bot.huntScriptId, bot.huntWaypointIdx));
			break;

		case HuntPhase::LEAVING: {
			// Follow travelFromWaypoints (or recovery waypoints) back to town
			s_leavingPhaseStart[bot.guid] = OTSYS_TIME();
			s_leavingWpTimer.erase(bot.guid);
			const HuntScript* script = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { script = &s; break; }
			}
			bot.huntWaypointIdx = 0;
			if (bot.isRecoveryRoute && !bot.recoveryWaypoints.empty()) {
				castLog(bot, fmt::format("LEAVING: Following {} recovery waypoints back to town",
					bot.recoveryWaypoints.size()));
			} else if (script && !script->travelFromWaypoints.empty()) {
				castLog(bot, fmt::format("LEAVING: Following {} return waypoints back to town",
					script->travelFromWaypoints.size()));
			} else {
				// No travel_from — try recovery route, then teleport
				if (!findNearestRecoveryRoute(bot)) {
					castLog(bot, "LEAVING: No travelFrom waypoints and no recovery route, teleporting");
					trackNavEvent("leaving_teleport", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
						bot.townId, "hunt_leaving", "no travelFrom or recovery route");
					teleportToTemple(bot);
					beginHuntPhase(bot, HuntPhase::RESUPPLYING);
				}
			}
			break;
		}

		case HuntPhase::RESUPPLYING:
			// A route may legitimately end in a different town than it started in — that is
			// what NPC_INTERACT and TELEPORT waypoints are for. Everything below this point
			// (depot POI lookup, shop city routes) keys off bot.townId, so it has to describe
			// where the bot actually IS, not where the script says it began.
			//
			// The hibernated path already does this; only the live half was missing. Four of
			// the five callers reach here right after teleportToTemple, which resyncs inline,
			// so this is a no-op for them (syncTownIdToPos self-guards on detected==townId and
			// on detected==0, the Adventurer's-Stone island carve-out). The walked-completion
			// caller is the one that needed it.
			syncTownIdToPos(bot);
			// Initialize resupply with preparation state machine (depot + shop)
			bot.huntResupplyStart = OTSYS_TIME();
			bot.huntResupplyPhase = 0;
			bot.resupplyRerolled = false;
			bot.prepareStartTime = OTSYS_TIME();
			bot.prepareStep = 0;  // 0=walk_depot
			bot.prepareWaitUntil = 0;
			bot.prepareHasTarget = false;
			castLog(bot, "RESUPPLY: Starting resupply (depot + shop)");
			break;
	}
}

void BotEngine::doHunting(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Safety timeout — the absolute ceiling, measured from huntStartTime and applied on EVERY
	// phase. huntStartTime is not reset at the PATROLLING->LEAVING transition, so this is what
	// actually bounds a quest's return leg; the per-phase budgets have to fit inside it (see the
	// budget table in bot_engine_impl.hpp). Quests get a longer ceiling because a walkthrough is
	// one linear route, not a loop that can be cut anywhere.
	{
		const HuntScript* safetyScript = nullptr;
		for (const auto& s : huntScripts_) {
			if (s.id == bot.huntScriptId) { safetyScript = &s; break; }
		}
		// Three ceilings, one reader. A party hunt's own clock (PARTY_HUNT_TIME_MIN/MAX, 7200-10800s)
		// is LONGER than the ordinary hunt ceiling, so before 2026-08-05 every party hunt was cut at
		// exactly 3600s here and never reached its own huntEndTime — those two constants were dead
		// config. isPartyHuntLeader is the right predicate and not partyHuntId > 0: the latter is
		// equally true for support bots, and the pair is always cleared atomically (dissolvePartyHunt
		// / exitPartyHuntMode), with no leadership hand-off path — a dying leader dissolves the whole
		// party rather than promoting anyone. Followers never reach here at all; they carry state
		// PARTY, which dispatches to doParty/doPartyHunt, not doHunting.
		const int32_t safetyTimeout = bot.isPartyHuntLeader
			? PARTY_SAFETY_TIMEOUT
			: (botScriptIsQuest(safetyScript) ? QUEST_SAFETY_TIMEOUT : HUNT_SAFETY_TIMEOUT);
		if (OTSYS_TIME() - bot.huntStartTime > safetyTimeout * 1000LL) {
			abortHunt(bot, "safety timeout");
			return;
		}
	}

	switch (bot.huntPhase) {
		case HuntPhase::PREPARING:
			doHuntPrepare(bot);
			break;
		case HuntPhase::TRAVEL_TO:
			doHuntTravel(bot);
			break;
		case HuntPhase::PATROLLING:
			doHuntPatrol(bot);
			break;
		case HuntPhase::LEAVING:
			doHuntLeaving(bot);
			break;
		case HuntPhase::RESUPPLYING:
			doHuntResupply(bot);
			break;
	}
}

// Preparation phase: walk to depot, wait, walk to shop, wait, then advance to TRAVEL_TO
void BotEngine::doHuntPrepare(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// 5-minute timeout for preparation
	if (OTSYS_TIME() - bot.prepareStartTime > RESUPPLY_TIMEOUT * 1000LL) {
		bot.prepareStep = 5; // signal done
		if (bot.huntPhase == HuntPhase::PREPARING) {
			castLogError(bot, "PREPARE: Timeout, skipping to travel");
			beginHuntPhase(bot, HuntPhase::TRAVEL_TO);
		}
		// For RESUPPLYING, the caller (doHuntResupply) handles it
		return;
	}

	// Step 0: Walk to depot using city routes
	if (bot.prepareStep == 0) {
		if (!bot.prepareHasTarget) {
			// Try city route to "depot" first
			if (!bot.followingCityRoute && startCityRoute(bot, "", "depot")) {
				bot.prepareHasTarget = true;
				// Also find the depot POI position for arrival detection
				auto graphIt = cityRouteGraphs_.find(bot.townId);
				if (graphIt != cityRouteGraphs_.end()) {
					auto poiIt = graphIt->second.pois.find("depot");
					if (poiIt != graphIt->second.pois.end()) {
						bot.prepareTarget = poiIt->second;
					}
				}
				castLog(bot, "PREPARE: Following city route to depot");
			} else {
				// Fallback: find depot POI and walk directly
				auto& allPOIs = getCityPOIs();
				auto it = allPOIs.find(bot.townId);
				if (it != allPOIs.end()) {
					for (const auto& poi : it->second) {
						if (poi.type == POIType::DEPOT) {
							bot.prepareTarget = poi.pos;
							bot.prepareHasTarget = true;
							castLog(bot, fmt::format("PREPARE: Walking directly to depot at ({},{},{})",
								poi.pos.x, poi.pos.y, poi.pos.z));
							break;
						}
					}
				}
				if (!bot.prepareHasTarget) {
					bot.prepareStep = 2;
					return;
				}
			}
		}

		// Follow city route if active
		if (bot.followingCityRoute) {
			bool following = followCityRoute(bot);
			if (following) return; // Still navigating
			// Route completed — check if we're at depot
		}

		// Check if we arrived at depot area (use route graph POI position OR prepareTarget)
		auto graphIt = cityRouteGraphs_.find(bot.townId);
		Position depotPos = bot.prepareTarget;
		if (graphIt != cityRouteGraphs_.end()) {
			auto poiIt = graphIt->second.pois.find("depot");
			if (poiIt != graphIt->second.pois.end()) depotPos = poiIt->second;
		}
		if (isAtPosition(bot.currentPos, depotPos, 5) ||
			(bot.prepareHasTarget && isAtPosition(bot.currentPos, bot.prepareTarget, 3) &&
			bot.currentPos.z == bot.prepareTarget.z)) {
			// Arrived at depot area — find a reachable depot locker
			Position lockerPos = findReachableDepotLocker(bot);
			if (lockerPos.x > 0) {
				bot.prepareTarget = lockerPos;
				bot.prepareHasTarget = true;
				bot.pathFailCount = 0;
				bot.prepareStep = 1; // Walk to locker
				castLog(bot, fmt::format("PREPARE: At depot area, walking to locker at ({},{},{})",
					lockerPos.x, lockerPos.y, lockerPos.z));
			} else {
				// No locker found — just wait here
				bot.prepareStep = 1;
				bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
				bot.prepareHasTarget = false;
				castLog(bot, fmt::format("PREPARE: At depot (no locker found), waiting {}s",
					(bot.prepareWaitUntil - OTSYS_TIME()) / 1000));
			}
			return;
		}

		// Not following a city route — try direct walk as fallback
		if (!bot.followingCityRoute && player->listWalkDir.empty()) {
			bool ok = goToWithDoors(bot, bot.prepareTarget, POI_ARRIVAL_DIST);
			if (!ok) {
				tryAttackBlockingMonster(bot);
				bot.pathFailCount++;
				if (bot.pathFailCount % 30 == 1) {
					castLogError(bot, fmt::format("PREPARE: Stuck walking to depot fails={}",
						bot.pathFailCount));
				}
				if (bot.pathFailCount >= 150) {
					castLogError(bot, "PREPARE: Cannot reach depot, skipping");
					bot.prepareStep = 2;
					bot.prepareHasTarget = false;
					bot.pathFailCount = 0;
				}
			} else {
				bot.pathFailCount = 0;
			}
		}
		return;
	}

	// Step 1: Walk to depot locker, then wait
	if (bot.prepareStep == 1) {
		// If we have a wait timer set, just wait
		if (bot.prepareWaitUntil > 0) {
			if (OTSYS_TIME() < bot.prepareWaitUntil) return;
			// Done at depot — go to shop
			bot.prepareStep = 2;
			bot.prepareHasTarget = false;
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			castLog(bot, "PREPARE: Depot done, heading to shop");
			return;
		}

		// Walking to locker
		if (bot.prepareHasTarget) {
			if (!player->listWalkDir.empty()) return;

			// Track walk retries — after 5 idle ticks, blacklist locker and try a new one
			s_depotWalkRetries[bot.guid]++;
			if (s_depotWalkRetries[bot.guid] > 5) {
				blacklistDepotLocker(bot.guid, bot.prepareTarget);
				if (s_depotBlacklist[bot.guid].size() >= 5) {
					castLog(bot, "PREPARE: Tried 5 lockers, waiting here");
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
					bot.prepareHasTarget = false;
					bot.pathFailCount = 0;
					clearDepotBlacklist(bot.guid);
					return;
				}
				Position newLocker = findReachableDepotLocker(bot);
				if (newLocker.x != 0) {
					castLog(bot, fmt::format("PREPARE: Retrying with locker {}/5 at ({},{},{})",
						s_depotBlacklist[bot.guid].size() + 1,
						newLocker.x, newLocker.y, newLocker.z));
					bot.prepareTarget = newLocker;
					bot.pathFailCount = 0;
					s_depotWalkRetries[bot.guid] = 0;
				} else {
					castLog(bot, "PREPARE: No more lockers, waiting here");
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
					bot.prepareHasTarget = false;
					bot.pathFailCount = 0;
					clearDepotBlacklist(bot.guid);
					return;
				}
			}

			// If locker is on a different z-level, trigger floor change first
			if (bot.currentPos.z != bot.prepareTarget.z && bot.fcState == FloorChangeState::NONE) {
				bool goDown = bot.prepareTarget.z > bot.currentPos.z;
				castLog(bot, fmt::format("PREPARE: Locker at z={}, bot at z={}, floor change {}",
					bot.prepareTarget.z, bot.currentPos.z, goDown ? "DOWN" : "UP"));
				startFloorChange(bot, goDown, bot.prepareTarget);
				return;
			}
			if (bot.fcState != FloorChangeState::NONE) return;

			// Check if adjacent to locker (dist <= 1)
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.prepareTarget.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.prepareTarget.y)));

			if (dist <= 1 && bot.currentPos.z == bot.prepareTarget.z) {
				// Adjacent to locker — say something and start wait timer
				tryEmitChat(bot, player, "depot", /*channelId=*/0);
				bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
				bot.prepareHasTarget = false;
				clearDepotBlacklist(bot.guid);
				castLog(bot, fmt::format("PREPARE: At depot locker, waiting {}s",
					(bot.prepareWaitUntil - OTSYS_TIME()) / 1000));
				return;
			}

			// Try goTo the locker position directly with maxDist=1 (stop adjacent)
			bool walked = goTo(bot, bot.prepareTarget, 1);
			if (!walked) {
				bot.pathFailCount++;
				if (bot.pathFailCount % 5 == 1) {
					castLogError(bot, fmt::format("PREPARE: Walk to locker fail {} dist={} pos=({},{},{})->({},{},{})",
						bot.pathFailCount, dist, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
						bot.prepareTarget.x, bot.prepareTarget.y, bot.prepareTarget.z));
				}
				if (bot.pathFailCount >= 5) {
					// Can't reach locker — just wait here
					castLog(bot, "PREPARE: Can't reach locker, waiting here");
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
					bot.prepareHasTarget = false;
					bot.pathFailCount = 0;
				}
			} else {
				bot.pathFailCount = 0;
			}
		} else {
			// No target and no wait timer — shouldn't happen, skip to shop
			bot.prepareStep = 2;
		}
		return;
	}

	// Step 2: Walk to shop using city routes
	if (bot.prepareStep == 2) {
		if (!bot.prepareHasTarget) {
			// Try city route to "potions" (shop) first
			if (!bot.followingCityRoute && startCityRoute(bot, "depot", "potions")) {
				bot.prepareHasTarget = true;
				auto graphIt = cityRouteGraphs_.find(bot.townId);
				if (graphIt != cityRouteGraphs_.end()) {
					auto poiIt = graphIt->second.pois.find("potions");
					if (poiIt != graphIt->second.pois.end()) {
						bot.prepareTarget = poiIt->second;
					}
				}
				castLog(bot, "PREPARE: Following city route to shop");
			} else {
				// Fallback: find shop POI and walk directly
				auto& allPOIs = getCityPOIs();
				auto it = allPOIs.find(bot.townId);
				if (it != allPOIs.end()) {
					for (const auto& poi : it->second) {
						if (poi.type == POIType::SHOP) {
							bot.prepareTarget = poi.pos;
							bot.prepareHasTarget = true;
							castLog(bot, fmt::format("PREPARE: Walking directly to shop at ({},{},{})",
								poi.pos.x, poi.pos.y, poi.pos.z));
							break;
						}
					}
				}
				if (!bot.prepareHasTarget) {
					// No depot->potions route AND no SHOP POI for this town. Silent until now,
					// which made the cast log read as a bug: "Depot done, heading to shop" was
					// followed one ~100ms tick later by "TRAVEL_TO: ...", with the skip itself
					// invisible.
					castLog(bot, fmt::format("PREPARE: No shop route or POI in town {} — skipping shop",
						bot.townName.empty() ? std::to_string(bot.townId) : bot.townName));
					bot.prepareStep = 5;
					return;
				}
			}
		}

		// Follow city route if active
		if (bot.followingCityRoute) {
			bool following = followCityRoute(bot);
			if (following) return;
		}

		// Check if we arrived at shop
		if (isAtPosition(bot.currentPos, bot.prepareTarget, 3) &&
			bot.currentPos.z == bot.prepareTarget.z) {
			bot.prepareStep = 3;
			bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(10, 60) * 1000LL;
			bot.prepareHasTarget = false;
			g_game().internalCreatureSay(player, TALKTYPE_SAY, "hi", false);
			castLog(bot, fmt::format("PREPARE: At shop, waiting {}s",
				(bot.prepareWaitUntil - OTSYS_TIME()) / 1000));
			return;
		}

		// Walk toward shop (fallback direct walk)
		if (!bot.followingCityRoute && player->listWalkDir.empty()) {
			bool ok = goToWithDoors(bot, bot.prepareTarget, POI_ARRIVAL_DIST);
			if (!ok) {
				tryAttackBlockingMonster(bot);
				bot.pathFailCount++;
				if (bot.pathFailCount % 30 == 1) {
					castLogError(bot, fmt::format("PREPARE: Stuck walking to shop fails={}",
						bot.pathFailCount));
				}
				if (bot.pathFailCount >= 150) {
					castLogError(bot, "PREPARE: Cannot reach shop, skipping");
					bot.prepareStep = 5;
					bot.prepareHasTarget = false;
					bot.pathFailCount = 0;
				}
			} else {
				bot.pathFailCount = 0;
			}
		}
		return;
	}

	// Step 3: Waiting at shop
	if (bot.prepareStep == 3) {
		if (OTSYS_TIME() < bot.prepareWaitUntil) return;
		bot.prepareStep = 4;
		castLog(bot, "PREPARE: Shop done");
		return;
	}

	// Step 4: Exit shop — only if we can't reach the first few travel_to waypoints
	if (bot.prepareStep == 4) {
		if (!bot.prepareHasTarget && !bot.followingCityRoute) {
			// Check if any of the first 5 travel_to (or patrol) waypoints are reachable via pathfinding
			bool canReach = false;
			const HuntScript* script = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { script = &s; break; }
			}
			const std::vector<Waypoint>* wps = nullptr;
			if (script && !script->travelToWaypoints.empty()) {
				wps = &script->travelToWaypoints;
			} else if (script && !script->patrolWaypoints.empty()) {
				wps = &script->patrolWaypoints;
			}
			if (wps && player) {
				size_t checkCount = std::min(wps->size(), static_cast<size_t>(5));
				for (size_t i = 0; i < checkCount; i++) {
					auto& wp = (*wps)[i].pos;
					std::vector<Direction> dirs;
					// Gesior-aligned reachability probe — 50 nodes is plenty for a
					// boolean "can we reach this from current town?" check across the
					// first 5 travel-to waypoints. Was 200 nodes which is overkill for
					// a probe (full nav uses PATH_MAX_DIST/4096-node pool separately).
					if (player->getPathTo(wp, dirs, 0, 1, true, true, 50)) {
						canReach = true;
						break;
					}
				}
			}

			if (canReach) {
				bot.prepareStep = 5;
				castLog(bot, "PREPARE: Preparation complete");
				return;
			}

			// Can't reach travel_to — try exit-potions route first, fall back to potions→depot
			if (startCityRoute(bot, "potions", "exit-potions") || startCityRoute(bot, "potions", "depot")) {
				bot.prepareHasTarget = true;
				castLog(bot, "PREPARE: Exiting shop via potions->depot route");
			} else {
				castLog(bot, "PREPARE: No shop exit route, proceeding directly");
				bot.prepareStep = 5;
				return;
			}
		}

		if (bot.followingCityRoute) {
			bool following = followCityRoute(bot);
			if (following) return;
			bot.prepareHasTarget = false;
		}

		bot.prepareStep = 5;
		castLog(bot, "PREPARE: Exited shop, preparation complete");
		return;
	}

	// Step 5: Done — advance depends on current phase
	if (bot.prepareStep == 5) {
		if (bot.huntPhase == HuntPhase::PREPARING) {
			beginHuntPhase(bot, HuntPhase::TRAVEL_TO);
		}
		// For RESUPPLYING, the caller (doHuntResupply) handles the next step
	}
}

void BotEngine::doHuntTravel(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Fix #6 (Nizzley): helper for traveling-script "end gracefully as IDLE" path.
	// Unifies the three sites below where the old code would unconditionally
	// flip to PATROLLING. For a "traveling" scriptCategory (City Walks, etc.)
	// with empty patrolWaypoints + empty targetNames, PATROLLING is wrong — it
	// triggers the all-monsters-attack fallback at scanAndAttackMonster and the
	// bot stops dead farming wildlife instead of completing the journey.
	auto endTravelingAsIdle = [&](const HuntScript* s, std::string_view reason) {
		s_huntTravelStart.erase(bot.guid);
		if (s) {
			activeHunts_.erase(bot.huntScriptId);
			if (!s->spawnGroup.empty()) activeSpawnGroups_.erase(s->spawnGroup);
			castLog(bot, fmt::format("CITY WALK: '{}' ending as IDLE ({})", s->name, reason));
		}
		bot.huntScriptId = 0;
		bot.state = BotAIState::IDLE;
		bot.hasWalkTarget = false;
		bot.nextRerollTime = OTSYS_TIME() + uniform_random(30, 120) * 1000;
	};

	// Resolve current script once — used by both timeout and downstream branches.
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}

	// Time-based TRAVEL_TO phase timeout — teleport to spawn after 5 min (20 for a quest: the
	// walkthrough legs are long, and quests now fight back on the way, which costs time. Fits
	// inside QUEST_SAFETY_TIMEOUT — see the budget table in bot_engine_impl.hpp).
	const int64_t travelMaxMs = botScriptIsQuest(script) ? QUEST_TRAVEL_MAX_MS : HUNT_TRAVEL_MAX_MS;
	auto travelIt = s_huntTravelStart.find(bot.guid);
	if (travelIt != s_huntTravelStart.end() && OTSYS_TIME() - travelIt->second > travelMaxMs) {
		// Fix #6: traveling-category scripts (City Walks) — end as IDLE instead of
		// flipping to PATROLLING. They have no patrolWaypoints / no targetNames, so
		// PATROLLING ends up wildlife-farming. The bot probably got most of the way
		// to the destination; just call it done.
		if (script && script->scriptCategory == "traveling") {
			endTravelingAsIdle(script, fmt::format("travel timeout {}s", travelMaxMs / 1000));
			return;
		}
		// Site B: unconditional by design — the travel budget is spent, so the bot is placed
		// at the patrol entry regardless of distance. Still must land on the index it will
		// actually start from.
		size_t timeoutEntryIdx = kNoPatrolIdx;
		if (script && !script->patrolWaypoints.empty()) {
			timeoutEntryIdx = botPatrolEntryIdx(script->patrolWaypoints, *script);
			Position landing = safeTeleportLanding(bot, script->patrolWaypoints[timeoutEntryIdx].pos,
				&script->patrolWaypoints, &timeoutEntryIdx, "huntStartTimeout");
			BOT_TELEPORT(player, landing, true);
			bot.currentPos = landing;
			castLogError(bot, fmt::format("TRAVEL_TO: Timeout ({}s), teleported to patrol entry wp {}/{} ({},{},{})",
				travelMaxMs / 1000, timeoutEntryIdx + 1, script->patrolWaypoints.size(),
				landing.x, landing.y, landing.z));
			trackNavEvent("hunt_travel_timeout", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
				bot.townId, "travel_to", fmt::format("timeout={}s entry={}",
					travelMaxMs / 1000, timeoutEntryIdx));
		}
		s_huntTravelStart.erase(bot.guid);
		beginHuntPhase(bot, HuntPhase::PATROLLING, timeoutEntryIdx);
		return;
	}

	if (!script) {
		s_huntTravelStart.erase(bot.guid);
		// No script to read patrol waypoints from — IDLE is safer than PATROLLING-with-no-waypoints.
		bot.huntScriptId = 0;
		bot.state = BotAIState::IDLE;
		bot.nextRerollTime = OTSYS_TIME() + 30000;
		return;
	}

	auto& travelWps = script->travelToWaypoints;
	if (travelWps.empty()) {
		// Fix #6: same defense — traveling scripts with no travel waypoints
		// shouldn't flip to PATROLLING either.
		if (script->scriptCategory == "traveling") {
			endTravelingAsIdle(script, "no travel waypoints");
			return;
		}
		// No travel waypoints — check distance to the patrol ENTRY wp (site C)
		size_t entryIdx = kNoPatrolIdx;
		if (!script->patrolWaypoints.empty()) {
			entryIdx = botPatrolEntryIdx(script->patrolWaypoints, *script);
			auto& entryWp = script->patrolWaypoints[entryIdx].pos;
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(entryWp.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(entryWp.y)));
			if (dist > 30 || bot.currentPos.z != entryWp.z) {
				Position landing = safeTeleportLanding(bot, entryWp, &script->patrolWaypoints,
					&entryIdx, "huntStartNoTravel");
				BOT_TELEPORT(player, landing, true);
				bot.currentPos = landing;
				castLog(bot, fmt::format("TRAVEL_TO: Teleported to patrol entry wp {}/{} ({},{},{})",
					entryIdx + 1, script->patrolWaypoints.size(), landing.x, landing.y, landing.z));
				trackNavEvent("hunt_teleport_patrol", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
					bot.townId, "travel_to", fmt::format("no travel wps, dist={} entry={}", dist, entryIdx));
			}
		}
		beginHuntPhase(bot, HuntPhase::PATROLLING, entryIdx);
		return;
	}

	// Quests fight back while travelling. TRAVEL_TO had no combat at all — a hunt is heading to a
	// spawn it will camp anyway, but a quest walks long authored routes through hostile ground and
	// must not be chewed on while it walks. Retaliation only (botIsQuestRetaliationTarget), so the
	// bot never goes looking for a fight and the route keeps moving.
	//
	// Pausing waypoint advancement while a target is held mirrors doHuntLeaving. The time this
	// costs is why quests get QUEST_TRAVEL_MAX_MS rather than the hunt's 5 minutes.
	if (botScriptIsQuest(script)) {
		scanAndAttackMonster(bot);
		if (bot.huntTargetId > 0) return;
	}

	// ROUND2 A1: a party leader waits for stragglers in EVERY phase, not just PATROLLING. Placed
	// after the combat/target early-returns above, so the leader still fights and lures while it
	// holds — only waypoint ADVANCEMENT pauses. No-op for solo hunters and human-led parties.
	if (partyLeaderShouldHoldForStragglers(bot)) {
		return;
	}

	// Follow travel_to waypoints using unified waypoint system
	{
		WaypointFollowConfig config;
		config.logPrefix = "TRAVEL_TO";
		config.globalTimeoutMs = 0; // handled by HUNT_TRAVEL_MAX_MS above
		config.perWpStuckMs = 30000;
		config.zChangeGraceMs = 500;
		// Step onto genuine teleport tiles (mystic flames, forcefields) with FLAG_NOLIMIT. This
		// was PATROL-only, so a `stand` on a teleport tile was never actually taken on a travel
		// leg. Quests walk all three legs through such tiles by design. Quest-only, so no hunt
		// route changes behavior. botScriptIsQuest is null-safe.
		config.enableTeleportStand = botScriptIsQuest(script);
		auto result = followWaypoints(bot, travelWps, bot.huntWaypointIdx,
			bot.huntWaypointSkipCount, config);
		if (result.inProgress) return;
		// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 0. followWaypoints' sanity abort sets
		// waypointIdx = waypoints.size() AND result.aborted — so without this check the
		// "completed all travel waypoints" branch below reads an abort as an arrival and
		// teleports the bot to the patrol start anyway. doHuntPatrol already checks this;
		// TRAVEL_TO and LEAVING never did.
		if (result.aborted) {
			s_huntTravelStart.erase(bot.guid);
			trackNavEvent("travel_route_aborted", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
				bot.townId, "travel_to", fmt::format("wps={}", travelWps.size()));
			abortHunt(bot, "travel route aborted");
			return;
		}
	}

	// Completed all travel waypoints (or past the end)
	if (bot.huntWaypointIdx >= travelWps.size()) {
		s_huntTravelStart.erase(bot.guid);
		// Traveling scripts (city walks): arrival at destination — update town and end
		if (script->scriptCategory == "traveling") {
			// Find destination town from script name ("City Walk: Thais to Venore" → Venore)
			// The script's townId is the SOURCE town, but we need the destination.
			// For now, keep the bot at its current position and just end the walk.
			castLog(bot, fmt::format("CITY WALK: Completed '{}' — arrived at destination", script->name));

			// Release reservation
			activeHunts_.erase(bot.huntScriptId);
			if (!script->spawnGroup.empty()) {
				activeSpawnGroups_.erase(script->spawnGroup);
			}

			bot.huntScriptId = 0;
			bot.state = BotAIState::IDLE;
			bot.hasWalkTarget = false;
			bot.nextRerollTime = OTSYS_TIME() + uniform_random(30, 120) * 1000; // dwell at destination
			return;
		}

		// Check distance to the patrol ENTRY waypoint (site D)
		//
		// This is the ONE patrol-entry site of four reached with the bot having actually WALKED an
		// authored travelToWaypoints route to its terminus — and that terminus is authored to land
		// it near the patrol head. So unlike the three placement sites, a teleport here is not
		// something the engine was going to do anyway: it is created by the phase-desync roll
		// landing elsewhere in the loop. Ask for an entry the bot can walk to first, and only fall
		// back to the unrestricted roll (and the bridge below) when the loop genuinely has no legal
		// candidate within range — which is the case for roughly 101 of the 215 enabled non-quest
		// scripts, where travel_to does not reach the spawn and a bridge is needed regardless.
		size_t entryIdx = kNoPatrolIdx;
		if (!script->patrolWaypoints.empty()) {
			entryIdx = botPatrolEntryIdx(script->patrolWaypoints, *script,
				&bot.currentPos, HUNT_PATROL_ENTRY_BRIDGE_DIST);
			if (entryIdx == kNoPatrolIdx) {
				entryIdx = botPatrolEntryIdx(script->patrolWaypoints, *script);
			}
			auto& entryWp = script->patrolWaypoints[entryIdx].pos;
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(entryWp.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(entryWp.y)));
			if (dist > HUNT_PATROL_ENTRY_BRIDGE_DIST || bot.currentPos.z != entryWp.z) {
				Position landing = safeTeleportLanding(bot, entryWp, &script->patrolWaypoints,
					&entryIdx, "huntStartAfterTravel");
				BOT_TELEPORT(player, landing, true);
				bot.currentPos = landing;
				castLog(bot, fmt::format("TRAVEL_TO: Teleported to patrol entry wp {}/{} ({},{},{}) ({} tiles away, no walkable entry)",
					entryIdx + 1, script->patrolWaypoints.size(), landing.x, landing.y, landing.z, dist));
				trackNavEvent("hunt_teleport_patrol", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
					bot.townId, "travel_to", fmt::format("after travel wps, dist={} entry={}", dist, entryIdx));
			} else {
				castLog(bot, fmt::format("TRAVEL_TO: Walking into patrol at wp {}/{} ({},{},{}) — {} tiles, no teleport",
					entryIdx + 1, script->patrolWaypoints.size(), entryWp.x, entryWp.y, entryWp.z, dist));
			}
		}
		s_huntTravelStart.erase(bot.guid);
		beginHuntPhase(bot, HuntPhase::PATROLLING, entryIdx);
	}
}

void BotEngine::doHuntLeaving(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Get the hunt script. Resolved BEFORE the phase timeout because the timeout budget is
	// quest-dependent — the same resolve-once-at-top shape doHuntTravel uses. NOTE `script` may
	// legitimately be nullptr here (the recovery-route path below tolerates it), which is why
	// every read goes through botScriptIsQuest rather than dereferencing directly.
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}

	// Time-based LEAVING phase timeout — teleport home after 5 min (20 for a quest). This is the
	// authored return leg, which for a quest is the whole point of the phase, so it gets the same
	// budget as travel_to. Fits inside QUEST_SAFETY_TIMEOUT — see bot_engine_impl.hpp.
	const int64_t leavingMaxMs = botScriptIsQuest(script) ? QUEST_LEAVING_MAX_MS : LEAVING_PHASE_MAX_MS;
	auto now = OTSYS_TIME();
	auto phaseIt = s_leavingPhaseStart.find(bot.guid);
	if (phaseIt != s_leavingPhaseStart.end() && now - phaseIt->second > leavingMaxMs) {
		castLogError(bot, fmt::format("LEAVING: Phase timeout ({}s), teleporting to temple",
			leavingMaxMs / 1000));
		trackNavEvent("leaving_timeout", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
			bot.townId, "hunt_leaving",
			fmt::format("script={} timeout={}s", bot.huntScriptId, leavingMaxMs / 1000));
		s_leavingPhaseStart.erase(bot.guid);
		s_leavingWpTimer.erase(bot.guid);
		teleportToTemple(bot);
		beginHuntPhase(bot, HuntPhase::RESUPPLYING);
		return;
	}

	if (!script && !(bot.isRecoveryRoute && !bot.recoveryWaypoints.empty())) {
		teleportToTemple(bot);
		beginHuntPhase(bot, HuntPhase::RESUPPLYING);
		return;
	}

	// Determine which waypoints to use for return trip
	const std::vector<Waypoint>* returnWps = nullptr;

	if (bot.isRecoveryRoute && !bot.recoveryWaypoints.empty()) {
		returnWps = &bot.recoveryWaypoints;
	} else if (script && !script->travelFromWaypoints.empty()) {
		returnWps = &script->travelFromWaypoints;
	} else {
		// No return waypoints — try recovery route, then teleport
		if (!findNearestRecoveryRoute(bot)) {
			castLog(bot, "LEAVING: No travelFrom waypoints and no recovery route, teleporting");
			trackNavEvent("leaving_teleport", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
				bot.townId, "hunt_leaving", "no travelFrom or recovery route (doHuntLeaving)");
			teleportToTemple(bot);
			beginHuntPhase(bot, HuntPhase::RESUPPLYING);
		}
		return;
	}

	// Scan and attack hunt targets while leaving (clears path blockers)
	scanAndAttackMonster(bot);

	// If actively fighting a target, pause waypoint navigation
	if (bot.huntTargetId > 0) return;

	// ROUND2 A1: same leader-hold gate as PATROLLING/TRAVEL_TO. Note the hold's 20s episode is spent
	// inside this phase's own LEAVING_PHASE_MAX_MS budget — bounded and deliberate.
	if (partyLeaderShouldHoldForStragglers(bot)) {
		return;
	}

	// Follow return waypoints using unified waypoint system
	{
		WaypointFollowConfig config;
		config.logPrefix = "LEAVING";
		config.globalTimeoutMs = 0; // handled by LEAVING_PHASE_MAX_MS above
		config.perWpStuckMs = 30000;
		config.zChangeGraceMs = 500;
		// See the matching comment in doHuntTravel. `script` is legitimately nullptr on the
		// recovery-route path that reaches here, which is why this goes through the null-safe
		// helper rather than dereferencing.
		config.enableTeleportStand = botScriptIsQuest(script);
		auto result = followWaypoints(bot, *returnWps, bot.huntWaypointIdx,
			bot.huntWaypointSkipCount, config);
		if (result.inProgress) return;
		// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 0. Without this the abort falls into the
		// "Returned to town" branch below: the bot announces arrival, releases its hunt
		// reservation and walks to a depot POI in a town it is not standing in.
		//
		// The recovery-state clear is load-bearing, not cosmetic. abortHunt deliberately
		// leaves isRecoveryRoute/recoveryWaypoints set while a recovery route is live (it
		// returns early once findNearestRecoveryRoute succeeds) and only clears them on the
		// no-route branch. So the ONLY thing that ever cleared an interrupted recovery route
		// was the buggy "Returned to town" branch, incidentally. Drop that without replacing
		// it and the stale route gets reused on the bot's NEXT hunt, via the
		// isRecoveryRoute check at the top of this function.
		if (result.aborted) {
			s_leavingPhaseStart.erase(bot.guid);
			s_leavingWpTimer.erase(bot.guid);
			trackNavEvent("leaving_route_aborted", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
				bot.townId, "hunt_leaving",
				fmt::format("recovery={} wps={}", bot.isRecoveryRoute ? 1 : 0, returnWps->size()));
			castLogError(bot, "LEAVING: Route aborted — teleporting to temple and resupplying");
			activeHunts_.erase(bot.huntScriptId);
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
					activeSpawnGroups_.erase(s.spawnGroup);
					break;
				}
			}
			bot.recoveryWaypoints.clear();
			bot.isRecoveryRoute = false;
			bot.huntScriptId = 0;
			teleportToTemple(bot);
			beginHuntPhase(bot, HuntPhase::RESUPPLYING);
			return;
		}
	}

	// Completed all return waypoints
	if (bot.huntWaypointIdx >= returnWps->size()) {
		s_leavingPhaseStart.erase(bot.guid);
		s_leavingWpTimer.erase(bot.guid);
		if (bot.isRecoveryRoute) {
			// Recovery route complete — release reservation and navigate to depot
			activeHunts_.erase(bot.huntScriptId);
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
					activeSpawnGroups_.erase(s.spawnGroup);
					break;
				}
			}
			bot.recoveryWaypoints.clear();
			bot.isRecoveryRoute = false;
			bot.huntScriptId = 0;
			bot.state = BotAIState::IDLE;
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;

			// Find depot POI and navigate there via city route
			auto& allPOIs = getCityPOIs();
			auto poiIt = allPOIs.find(bot.townId);
			if (poiIt != allPOIs.end()) {
				for (const auto& poi : poiIt->second) {
					if (poi.type == POIType::DEPOT || poi.type == POIType::DEPOT_OUTSIDE) {
						bot.walkTarget = poi.pos;
						bot.hasWalkTarget = true;
						bot.pendingNavDest = "depot";
						castLog(bot, fmt::format("RECOVERY: Returned to town, navigating to depot ({},{},{})",
							poi.pos.x, poi.pos.y, poi.pos.z));
						break;
					}
				}
			}
			if (!bot.hasWalkTarget) {
				castLog(bot, "RECOVERY: Returned to town (no depot POI found)");
			}
		} else {
			castLog(bot, "LEAVING: Returned to town, starting resupply");
			beginHuntPhase(bot, HuntPhase::RESUPPLYING);
		}
	}
}

// ============================================================================
// BOT_LURE_KITE — lure mode. See the state block in bot_engine_impl.hpp for the
// design; the short version is that a bot walks its patrol holding fire until a
// pack has gathered, then fights it, then goes back to luring.
// ============================================================================

uint8_t BotEngine::effectiveMinMonsters(const BotState& bot, const HuntScript* script) const {
	const int32_t cap = std::max(1, lureCfg_.maxPack);
	// Debug override wins over data: `/cavebot <bot> lure 4` is how a spawn gets tested
	// without editing authored data, and it must be able to arm a script that ships 0.
	const auto ov = s_lureOverride.find(bot.guid);
	if (ov != s_lureOverride.end()) {
		return ov->second == 0 ? 0 : static_cast<uint8_t>(std::min<int32_t>(ov->second, cap));
	}
	if (script && script->minMonsters > 0) {
		return static_cast<uint8_t>(std::min<int32_t>(script->minMonsters, cap));
	}
	// A party hunt lures whatever the script says. Without this fallback the "always
	// lure for a team hunt" rule would be dead code, because every script ships 0.
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0 && lureCfg_.partyAlways) {
		return static_cast<uint8_t>(std::clamp(lureCfg_.partyDefaultMin, 1, cap));
	}
	return 0;
}

bool BotEngine::lureEligible(const BotState& bot, const HuntScript* script,
                             const std::shared_ptr<Player>& player) const {
	if (!lureCfg_.enable || !script || !player) return false;
	if (bot.state != BotAIState::HUNTING || bot.huntPhase != HuntPhase::PATROLLING) return false;
	// A quest is a linear one-shot and a "traveling" script has no spawn to farm —
	// the same two carve-outs the attack-all rule in scanAndAttackMonster makes.
	if (script->isQuest || script->scriptCategory == "quest"
	    || script->scriptCategory == "traveling") return false;
	if (script->patrolWaypoints.empty()) return false;
	if (effectiveMinMonsters(bot, script) == 0) return false;

	// Party path skips the level gate entirely: the party IS the qualification.
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0 && lureCfg_.partyAlways) return true;
	// A debug override is an explicit human decision; do not second-guess it on level.
	if (s_lureOverride.count(bot.guid)) return true;
	// levelMin == 0 can never qualify on the level path: the ratio would be vacuous and
	// a level-8 bot would lure a dragon spawn.
	if (script->levelMin == 0) return false;
	return static_cast<int64_t>(player->getLevel()) * 100
	       >= static_cast<int64_t>(script->levelMin) * lureCfg_.levelFactorPct;
}

uint8_t BotEngine::censusLuredPack(BotState& bot, const HuntScript* script, int32_t& outMaxDist,
                                   int32_t& outNearestDist, uint8_t& outSupportAggro,
                                   bool& outSupportContact) {
	outMaxDist = 0;
	outNearestDist = 999;
	outSupportAggro = 0;
	outSupportContact = false;
	auto player = bot.getPlayer();
	if (!player || !script) return 0;

	const uint32_t myCreatureId = player->getID();
	const uint32_t myPartyHunt = bot.partyHuntId;

	refreshSpectatorCacheIfStale(bot);
	uint8_t count = 0;
	for (uint32_t mid : bot.cachedMonsterIds) {
		auto creature = g_game().getCreatureByID(mid);
		if (!creature || creature->isRemoved() || creature->getHealth() <= 0) continue;
		const auto cpos = creature->getPosition();
		if (cpos.z != bot.currentPos.z) continue;

		// Who is it chewing on? One resolve, three answers — the same getAttackedCreature
		// signal botIsQuestRetaliationTarget uses. No A* reachability probe here: a
		// monster that is actively chasing us is reachable by construction, and a probe
		// per monster per tick is exactly the cost this feature cannot afford.
		auto attacked = creature->getAttackedCreature();
		if (!attacked) continue;

		const int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cpos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cpos.y)));

		if (attacked->getID() == myCreatureId) {
			if (dist > lureCfg_.radius) continue;
			// Name filter mirrors scanAndAttackMonster so "worth gathering" and "worth
			// attacking" cannot disagree. Empty targetNames = attack-all, and we are
			// PATROLLING + non-quest + non-traveling by lureEligible.
			bool nameMatch = script->targetNames.empty();
			if (!nameMatch) {
				const char* mn = creature->getName().c_str();
				for (const auto& tn : script->targetNames) {
					if (strcasecmp(mn, tn.c_str()) == 0) { nameMatch = true; break; }
				}
			}
			if (!nameMatch) continue;
			++count;
			outMaxDist = std::max(outMaxDist, dist);
			outNearestDist = std::min(outNearestDist, dist);
			continue;
		}

		// Aggro that landed on a party support. Supports mirror the leader's target, so
		// while the leader lures they are silent AND undefended (monster aggro never
		// enters BotAIState::COMBAT — that path is PvP-only). This is what trigger 6
		// reads. NOTE the id spaces differ: the test above compares CREATURE ids, but
		// s_botToPartyHunt is keyed by GUID.
		if (myPartyHunt == 0) continue;
		auto ap = attacked->getPlayer();
		if (!ap) continue; // monster infighting, or a summon — not our problem
		const auto phIt = s_botToPartyHunt.find(ap->getGUID());
		if (phIt == s_botToPartyHunt.end() || phIt->second != myPartyHunt) continue;
		++outSupportAggro;
		const auto spos = attacked->getPosition();
		if (spos.z == cpos.z
		    && std::max(std::abs(static_cast<int32_t>(spos.x) - static_cast<int32_t>(cpos.x)),
		                std::abs(static_cast<int32_t>(spos.y) - static_cast<int32_t>(cpos.y))) <= 1) {
			outSupportContact = true;
		}
	}
	return count;
}

uint8_t BotEngine::lurePackAliveAggroed(BotState& bot, const HuntScript* script) {
	int32_t maxD = 0, nearD = 0;
	uint8_t supportAggro = 0;
	bool supportContact = false;
	const uint8_t mine = censusLuredPack(bot, script, maxD, nearD, supportAggro, supportContact);
	// Support-aggroed monsters count: the hunt-end hold must not release into LEAVING
	// while the pack it deliberately built is still eating the healer.
	return static_cast<uint8_t>(std::min<int32_t>(255, mine + supportAggro));
}

void BotEngine::forceLureEngage(BotState& bot, uint8_t trigger, const char* reason) {
	auto& run = s_lure[bot.guid];
	if (run.phase == LurePhase::Engaging) return;
	run.phase = LurePhase::Engaging;
	run.lastTrigger = trigger;
	run.paceUntilMs = 0;
	run.contactSinceMs = 0;
	run.supportSinceMs = 0;
	run.decaySinceMs = 0;
	++run.engagements;
	auto player = bot.getPlayer();
	// Unconditional info line, not castLog: castLog is verboseLog-gated, so it is not a
	// usable acceptance baseline from journalctl.
	g_logger().info("[BotEngine] LURE ENGAGE {} script={} pack={} peak={} elapsed={}s trigger={} ({})",
		player ? player->getName() : std::to_string(bot.guid), bot.huntScriptId,
		run.count, run.peak,
		run.startMs > 0 ? (OTSYS_TIME() - run.startMs) / 1000 : 0, trigger, reason);
}

LureVerdict BotEngine::tickLure(BotState& bot, const HuntScript* script) {
	// Cheapest possible exit for the disabled case: this runs for every hunting bot on
	// every tick, and with the feature off it must not even hash a guid. The s_lure
	// emptiness test is what still lets an operator disable it live (via
	// _global reloadconfig) without leaving runs stranded mid-lure.
	if (!lureCfg_.enable) {
		if (!s_lure.empty()) clearLureRun(bot.guid);
		return LureVerdict::Inactive;
	}
	auto player = bot.getPlayer();

	// THE GATE. Re-evaluated every tick rather than relying on cleanup sites, because
	// beginHuntPhase is explicitly NOT a funnel (16 sites assign huntPhase directly) —
	// so a missed cleanup must be harmless, not a latent bug. The scriptId stamp is the
	// part that makes that true: every other condition here is equally satisfied by a
	// NEW hunt on a DIFFERENT script, which a virtual reroll during hibernation produces.
	if (!lureEligible(bot, script, player)) {
		clearLureRun(bot.guid);
		return LureVerdict::Inactive;
	}
	{
		const auto it = s_lure.find(bot.guid);
		if (it != s_lure.end() && it->second.scriptId != bot.huntScriptId) {
			clearLureRun(bot.guid);
		}
	}

	const int64_t now = OTSYS_TIME();
	auto& run = s_lure[bot.guid];
	if (run.phase == LurePhase::Off) {
		run = LureRun {};
		run.phase = LurePhase::Luring;
		run.scriptId = bot.huntScriptId;
		run.startMs = now;
		run.startWpIdx = bot.huntWaypointIdx;
		run.lastMoveMs = now;
		run.lastMovePos = bot.currentPos;
		// Drop anything we were already fighting, so the walk can start clean.
		if (bot.huntTargetId > 0) {
			bot.huntTargetId = 0;
			if (player) player->setAttackedCreature(nullptr);
		}
		castLog(bot, fmt::format("LURE: armed, target pack {}", effectiveMinMonsters(bot, script)));
	}

	int32_t maxDist = 0, nearestDist = 999;
	uint8_t supportAggro = 0;
	bool supportContact = false;
	const uint8_t pack = censusLuredPack(bot, script, maxDist, nearestDist, supportAggro, supportContact);
	run.count = pack;
	run.supportAggro = supportAggro;
	run.peak = std::max(run.peak, pack);

	// --- ENGAGING: fight until the screen is clear, then re-arm the lure -------------
	if (run.phase == LurePhase::Engaging) {
		const bool clear = bot.huntTargetId == 0 && pack == 0 && supportAggro == 0
			&& (bot.lastKillTime == 0 || now - bot.lastKillTime > 2000);
		if (clear) {
			run.phase = LurePhase::Luring;
			run.startMs = now;
			run.startWpIdx = bot.huntWaypointIdx;
			run.peak = 0;
			run.holdUntilMs = 0;
			run.lastMoveMs = now;
			run.lastMovePos = bot.currentPos;
			castLog(bot, "LURE: pack cleared, luring again");
			return LureVerdict::Luring;
		}
		return LureVerdict::Engage;
	}

	// --- LURING: hold fire, keep walking, decide when to turn and fight -------------
	const int32_t keepDist = getEffectiveKeepDistance(bot);
	const int32_t contactBand = std::max(1, keepDist > 0 ? keepDist - 1 : 1);

	// Movement bookkeeping for the body-blocked trigger.
	if (bot.currentPos.x != run.lastMovePos.x || bot.currentPos.y != run.lastMovePos.y
	    || bot.currentPos.z != run.lastMovePos.z) {
		run.lastMovePos = bot.currentPos;
		run.lastMoveMs = now;
	}
	const bool stalled = (now - run.lastMoveMs) > lureCfg_.blockedMs;

	// An empty screen must not burn the lure clock — otherwise a long stretch of quiet
	// patrol spends the whole budget and the bot "engages" nothing.
	if (pack == 0) {
		run.startMs = now;
		run.peak = 0;
		run.decaySinceMs = 0;
	}

	// Sustained-condition timers (each is "how long has this been true").
	const bool inContact = pack > 0 && nearestDist <= contactBand;
	run.contactSinceMs = inContact ? (run.contactSinceMs == 0 ? now : run.contactSinceMs) : 0;
	run.supportSinceMs = supportContact ? (run.supportSinceMs == 0 ? now : run.supportSinceMs) : 0;
	run.decaySinceMs = (pack > 0 && run.peak > pack)
		? (run.decaySinceMs == 0 ? now : run.decaySinceMs) : 0;

	const uint8_t want = effectiveMinMonsters(bot, script);
	const int32_t hpPct = (player && player->getMaxHealth() > 0)
		? static_cast<int32_t>(player->getHealth() * 100 / player->getMaxHealth()) : 100;

	uint8_t trigger = 0;
	const char* reason = nullptr;
	if (pack >= want) {                                       trigger = 1; reason = "pack_full"; }
	else if (hpPct < lureCfg_.hpFloorPct) {                   trigger = 2; reason = "hp_floor"; }
	else if (now - run.startMs > lureCfg_.maxMs) {            trigger = 3; reason = "timeout"; }
	else if (bot.huntWaypointIdx < run.startWpIdx) {          trigger = 4; reason = "lap_wrap"; }
	else if (stalled && pack > 0) {                           trigger = 5; reason = "blocked"; }
	else if (run.supportSinceMs > 0
	         && now - run.supportSinceMs > lureCfg_.contactMs) { trigger = 6; reason = "support_contact"; }
	else if (supportAggro >= 2) {                             trigger = 6; reason = "support_aggro"; }
	else if (run.decaySinceMs > 0
	         && now - run.decaySinceMs > lureCfg_.decayMs) {  trigger = 7; reason = "shedding"; }
	else if (run.contactSinceMs > 0
	         && now - run.contactSinceMs > lureCfg_.contactMs && stalled) {
		// Cornered WHILE luring. This is the one that matters most for a keep-distance
		// bot: it has no retreat while holding fire (chaseTarget is unreachable without
		// a target), so the only way to get its kiting back is to start fighting.
		trigger = 8; reason = "contact_stalled";
	}
	// Party members below the HP floor — the leader is the only one who can end the lure.
	if (trigger == 0 && bot.partyHuntId > 0) {
		for (const auto& [guid, phId] : s_botToPartyHunt) {
			if (phId != bot.partyHuntId || guid == bot.guid) continue;
			const auto idx = guidToIndex_.find(guid);
			if (idx == guidToIndex_.end()) continue;
			auto mp = bots_[idx->second].getPlayer();
			if (!mp || mp->getMaxHealth() <= 0) continue;
			if (mp->getHealth() * 100 / mp->getMaxHealth() < lureCfg_.hpFloorPct) {
				trigger = 6; reason = "member_hp";
				break;
			}
		}
	}

	if (trigger != 0) {
		forceLureEngage(bot, trigger, reason);
		return LureVerdict::Engage;
	}

	if (bot.tickCounter % 10 == 0) {
		castLog(bot, fmt::format("LURE: {}/{} pack maxD={} nearD={} t={}s",
			pack, want, maxDist, nearestDist == 999 ? -1 : nearestDist,
			(now - run.startMs) / 1000));
	}

	// Pace: stand still so the TAIL catches up. Never while anything is close — walking
	// is the only spacing a luring bot has, so standing still next to a monster is how
	// it dies. Bounded so a monster stuck behind a wall cannot freeze the patrol.
	if (pack > 0 && maxDist >= lureCfg_.paceDist && nearestDist > contactBand + 1) {
		if (run.paceUntilMs == 0) run.paceUntilMs = now + lureCfg_.paceMaxMs;
		if (now < run.paceUntilMs) return LureVerdict::Pace;
	} else {
		run.paceUntilMs = 0;
	}
	return LureVerdict::Luring;
}

void BotEngine::doHuntPatrol(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Script resolved FIRST, before any other gate. Two things below need it: the quest carve-out
	// in the hunt-end decision, and the lap-completion test. It used to be resolved further down,
	// after the combat and kill-linger early-returns, which made a quest-aware hunt-end check
	// impossible to write at the top of the function.
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}

	// ---- Hunt-end decision -------------------------------------------------------------------
	// Evaluated BEFORE scanAndAttackMonster and the target/linger returns on purpose. The lap
	// test below is a pure read of state the waypoint follower already maintains, so it does not
	// need to reach any later block — and it must not, because scanAndAttackMonster runs first and
	// can re-acquire a target every tick in a dense spawn (Falcon Bastion), holding huntTargetId>0
	// indefinitely. A check that lived past that gate would simply never be evaluated there.
	const bool timeUp = OTSYS_TIME() > bot.huntEndTime;
	const bool killLimitReached = bot.huntDebugKillLimit > 0 && bot.huntKillCount >= bot.huntDebugKillLimit;

	// A patrol is a LOOP that the clock used to cut at an arbitrary index. travel_from, however, is
	// authored as the CONTINUATION of the patrol's terminal waypoint (script 2085 "Falcons":
	// hunt_patrol seq 76 is 3 tiles from travel_from seq 0, same z). Entering it from anywhere else
	// hands followWaypoints a route whose head can be tens of tiles and a floor away, and its only
	// answer to a cross-floor waypoint is to skip — so the cursor marched through the authored exit
	// path until it hit a teleport waypoint or the 300s phase timeout. Waiting for the lap to close
	// is what makes travel_from start where the script says it starts.
	//
	// NOT for quests: a quest is a linear one-shot, its clock is an absolute deadline inside the
	// quest budget table, and it already leaves at patrol completion by construction.
	// NOT for the debug kill limit: `/cavebot <bot> killlimit` exists to end a hunt promptly, and
	// deferring it would cost a full lap. It therefore still reproduces the old mid-lap entry — a
	// debug knob, never set in production (huntDebugKillLimit defaults 0).
	const bool lapComplete = script && bot.huntWaypointIdx >= script->patrolWaypoints.size();
	const bool questScript = botScriptIsQuest(script);
	bool endNow = killLimitReached
		|| (timeUp && (questScript || lapComplete
		               // No script or no patrol to walk: there is no lap that can ever complete, so
		               // the clock has to be honoured here or the bot rides to the absolute ceiling
		               // and gets aborted instead of walking its authored return leg. A null script
		               // is a documented outcome (stale or disabled huntScriptId).
		               || !script || script->patrolWaypoints.empty()));

	// BOT_LURE_KITE: hunt-end lure hold. A lure-armed bot is LURING most of the time
	// between engagements, so the clock expiring mid-lure is the COMMON case, not an
	// edge. Left alone, the bot enters LEAVING holding a pack it deliberately built and
	// will never target — the attack-all rule is PATROLLING-only, so on an empty
	// targetNames script it would walk the whole authored return leg being chewed on,
	// with keep-distance retreat inert (no target => chaseTarget never runs).
	//
	// So: force the engagement, then defer the hunt end until the pack is down. Bounded
	// by botLureMaxMs (60s default) against the ordinary-hunt interlock, which already
	// tolerates "the remainder of the current patrol lap" and carries ~1370s of slack.
	// NOT for the debug kill limit (`/cavebot killlimit` must stay prompt) and NOT when
	// we are near the absolute safety ceiling — a party hunt only reserves ~600s there.
	if (endNow && !killLimitReached && script && lureCfg_.enable) {
		const int64_t ceilingMs = static_cast<int64_t>(
			bot.partyHuntId > 0 ? PARTY_SAFETY_TIMEOUT : HUNT_SAFETY_TIMEOUT) * 1000LL;
		const bool nearCeiling = bot.huntStartTime > 0
			&& OTSYS_TIME() > bot.huntStartTime + ceilingMs - 120000LL;
		auto lureIt = s_lure.find(bot.guid);
		if (!nearCeiling && lureIt != s_lure.end() && lureIt->second.phase != LurePhase::Off) {
			auto& run = lureIt->second;
			if (run.holdUntilMs == 0) {
				run.holdUntilMs = OTSYS_TIME() + lureCfg_.maxMs;
				forceLureEngage(bot, 9, "hunt_end_hold");
			}
			if (OTSYS_TIME() < run.holdUntilMs && lurePackAliveAggroed(bot, script) > 0) {
				endNow = false; // fight it out first; the clock is re-checked next tick
			}
		}
	}

	if (endNow) {
		g_logger().info("[BotEngine] {} hunt ended (kills={}, elapsed={}s, reason={}, pos={},{},{})",
			player->getName(), bot.huntKillCount, (OTSYS_TIME() - bot.huntStartTime) / 1000,
			killLimitReached ? "kill_limit" : "time",
			bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
		// Track successful hunt in DB (only if bot actually killed something).
		// JITTER FIX 2026-06-12: async via the dedicated bot-DB worker — this was the
		// last direct sync executeQuery on a runtime hot path (fired on every hunt
		// completion; measured 135ms incl. lock wait on the dispatcher, soak44).
		// Fire-and-forget counter increment, no read-back, ordering irrelevant.
		if (bot.huntKillCount > 0 && bot.huntScriptId > 0) {
			// BOT_CSV: hunt scripts are AUTHORED data and now live in
			// data/bot/authored/hunt_scripts.csv; these two counters are GENERATED
			// telemetry and stay in MySQL, split out into bot_hunt_script_stats.
			//
			// INSERT ... ON DUPLICATE KEY UPDATE, not a bare UPDATE: the first completion
			// for a newly imported script id has no stats row yet, and a bare UPDATE
			// would silently update zero rows and lose the count forever.
			g_botDatabaseTasks().execute(fmt::format(
				"INSERT INTO `bot_hunt_script_stats` (`script_id`, `successful_hunts`, `total_kills`) "
				"VALUES ({}, 1, {}) ON DUPLICATE KEY UPDATE "
				"`successful_hunts` = `successful_hunts` + 1, `total_kills` = `total_kills` + {}",
				bot.huntScriptId, bot.huntKillCount, bot.huntKillCount));
		}
		beginHuntPhase(bot, HuntPhase::LEAVING);
		return;
	}

	// BOT_LURE_KITE: decide, BEFORE the scan, whether this tick is spent gathering a
	// pack or fighting one. Verdict semantics:
	//   Inactive — not armed; everything below runs exactly as it did pre-feature.
	//   Luring   — hold fire and keep walking waypoints (no target is acquired at all,
	//              which is also what keeps party supports silent: they mirror the
	//              leader's attacked-creature).
	//   Pace     — hold fire and stand still, letting the tail of the pack catch up.
	//   Engage   — fight: identical to the pre-feature path.
	const LureVerdict lureVerdict = tickLure(bot, script);
	const bool luring = lureVerdict == LureVerdict::Luring || lureVerdict == LureVerdict::Pace;

	// Always scan for monsters — unless we are deliberately holding fire.
	if (!luring) {
		scanAndAttackMonster(bot);
	}

	// BOT_CORPSE_LOOT: the census and the adjacent open run on EVERY tick, combat
	// included, and deliberately ABOVE the target gate. CORPSEOWNER survives only ~10s
	// (Item::setID strips it on the first decay stage), and in a dense spawn
	// huntTargetId is set almost continuously — so a claim captured only between fights
	// would never happen for a ranged bot and every one of its kills would leak a
	// publicly-highlighted corpse. Neither call walks or pathfinds.
	tickCorpseCensus(bot);
	// The adjacent pass OWNS the tick while it is opening or waiting out its open pause.
	// Discarding this return value was the whole bug: the pause returned false, the tick
	// fell through to followWaypoints, and the bot walked off the very corpse it was
	// about to open — so the pending open failed its adjacency test, was cancelled
	// silently, and the corpse was re-picked and walked back to. That is the
	// back-and-forth seen on cast with no monsters alive. Safe to return here: combat
	// (scanAndAttackMonster, chase included) already ran above, and everything below is
	// movement that the pause must suppress for at most botLootDelayMaxMs.
	if (tickCorpseOpenAdjacent(bot)) return;

	// The walk pass runs above the target gate only so it can drop a live run promptly
	// when combat starts; it refuses to walk while huntTargetId is set.
	//
	// BOT_LURE_KITE: the WALK pass is the only one suppressed while luring — it is the
	// only one that moves the bot, and detouring to a corpse mid-lure would shed the
	// pack. The census and the adjacent open above keep running on purpose: the census
	// is what claims the PREVIOUS engagement's corpses inside the ~10s CORPSEOWNER
	// window, and skipping it would leak them for nothing.
	if (!luring && tickCorpseWalk(bot)) return;

	// Pace: the tail of the pack is drifting toward the edge of the screen, so stand
	// still and let it close. Returns here rather than falling through to the waypoint
	// follower — standing still IS the behaviour. Bounded by botLurePaceMaxMs, and
	// never entered while anything is already close (see tickLure).
	if (lureVerdict == LureVerdict::Pace) return;

	// If we have a target, don't advance waypoints. (While luring there is no target by
	// construction, so this gate is transparent and the patrol keeps moving.)
	if (bot.huntTargetId > 0) return;

	// Linger near recent kills — don't advance if reachable targets still nearby.
	// Skipped while luring: lingering is for a bot that just finished a fight, and a
	// luring bot must not stop next to a corpse with a live pack on its heels.
	if (!luring && bot.lastKillTime > 0 && OTSYS_TIME() - bot.lastKillTime < 2000) {
		if (hasNearbyReachableTargets(bot)) return;
	}

	// This bail-out deliberately stays HERE, below the scan, rather than moving up next to the
	// script resolve. Hoisting it would newly skip scanAndAttackMonster on the tick a hunt has a
	// stale or disabled huntScriptId, changing behaviour that has nothing to do with this fix. The
	// only thing that had to move up is the CLOCK, and the endNow branch above already covers the
	// !script / empty-patrol case explicitly.
	if (!script || script->patrolWaypoints.empty()) return;

	// Handle patrol cycle completion (wrap around waypoints)
	if (bot.huntWaypointIdx >= script->patrolWaypoints.size()) {
		if (script->isQuest || script->scriptCategory == "quest") {
			// A quest is ONE route: travel_to -> hunt_patrol -> travel_from, run once. This
			// used to call abortHunt, which either hands the bot a recovery route or
			// teleports it to temple — so the script's authored return leg was dead code for
			// every quest in the database. Go to LEAVING and walk it.
			//
			// abortHunt also dissolved an active party hunt on the way out; going straight to
			// LEAVING skips that, so do it explicitly. (tryStartPartyHunt should no longer be
			// able to pick a quest at all, but this is the cheap belt to that braces.)
			castLog(bot, fmt::format("QUEST: Patrol complete for '{}' — walking travel_from", script->name));
			if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
				dissolvePartyHunt(bot.partyHuntId, "quest_patrol_complete");
				bot.isPartyHuntLeader = false;
				bot.partyHuntId = 0;
				bot.partyRole = 0;
				bot.partyLeaderGuid = 0;
			}
			bot.isRecoveryRoute = false;
			bot.recoveryWaypoints.clear();
			beginHuntPhase(bot, HuntPhase::LEAVING);
			return;
		}
		bot.huntPatrolCycles++;
		bot.huntWaypointIdx = 0;
		bot.huntIgnoredMonsters.clear();
		if (bot.huntPatrolCycles >= 3 && bot.huntKillCount == 0) {
			abortHunt(bot, "3 cycles with 0 kills");
			return;
		}
	}

	// BOT_PARTY_TRAIL_FOLLOW: bounded leader wait — a party-hunt leader pauses waypoint
	// ADVANCEMENT (only) while an awake member straggles. Everything above this line already
	// ran: hunt-time expiry, scanAndAttackMonster, kill lingering — so the EK keeps fighting,
	// casting exeta res and holding the lure. No-op for solo hunters and human-led parties.
	if (partyLeaderShouldHoldForStragglers(bot)) {
		return;
	}

	// Follow patrol waypoints using unified waypoint system
	{
		WaypointFollowConfig config;
		config.logPrefix = "PATROL";
		config.globalTimeoutMs = 0; // hunt time is handled above
		config.perWpStuckMs = 30000;
		config.zChangeGraceMs = 500;
		config.enableLookaheadSkip = true;
		config.enableTeleportStand = true;
		auto result = followWaypoints(bot, script->patrolWaypoints, bot.huntWaypointIdx,
			bot.huntWaypointSkipCount, config);
		if (result.aborted) {
			abortHunt(bot, "stuck at waypoint");
		}
	}
}

bool BotEngine::advanceHuntWaypoint(BotState& bot) {
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script || script->patrolWaypoints.empty()) return false;

	auto& waypoints = script->patrolWaypoints;
	if (bot.huntWaypointIdx >= waypoints.size()) {
		// Quest scripts are one-shot — walk the authored return leg, same as doHuntPatrol.
		// NOTE this whole function is currently dead code (declared and defined, zero
		// callers); kept in sync so it does not become a trap if it is ever wired back up.
		//
		// NOT SYNCED with the 2026-08-05 lap-boundary hunt end: doHuntPatrol now defers a non-quest
		// hunt's clock to this same `idx >= size()` boundary so travel_from starts at the patrol
		// terminus. If this function is ever wired back up, it needs that branch too — otherwise
		// whichever caller uses it reintroduces mid-lap entry into travel_from.
		if (script->isQuest || script->scriptCategory == "quest") {
			castLog(bot, fmt::format("QUEST: Patrol complete for '{}' — walking travel_from", script->name));
			bot.isRecoveryRoute = false;
			bot.recoveryWaypoints.clear();
			beginHuntPhase(bot, HuntPhase::LEAVING);
			return false;
		}

		// Completed a patrol cycle
		bot.huntPatrolCycles++;
		bot.huntWaypointIdx = 0;
		bot.huntIgnoredMonsters.clear(); // reset ignored monsters each cycle

		// Abort if 3+ cycles with zero kills
		if (bot.huntPatrolCycles >= 3 && bot.huntKillCount == 0) {
			abortHunt(bot, "3 cycles with 0 kills");
			return false;
		}
		return true;
	}

	auto& waypoint = waypoints[bot.huntWaypointIdx];
	auto& wp = waypoint.pos;

	// TELEPORT waypoints fire immediately — no walking, no arrival check.
	if (waypoint.type == WaypointType::TELEPORT) {
		auto player = bot.getPlayer();
		if (player) {
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}
			// route = nullptr — see the matching comment in followWaypoints' TELEPORT branch.
			Position landing = safeTeleportLanding(bot, wp, nullptr, nullptr, "patrolTeleportWp");
			BOT_TELEPORT(player, landing, true);
			bot.currentPos = landing;
			bot.lastPos = landing;
			castLog(bot, fmt::format("PATROL: TELEPORT wp {}/{} → ({},{},{})",
				bot.huntWaypointIdx + 1, waypoints.size(), landing.x, landing.y, landing.z));
		}
		bot.huntWaypointIdx++;
		bot.huntWaypointSkipCount = 0;
		return true;
	}

	bool walkOnFc = isWalkOnFcTile(wp);

	// Arrival distance by waypoint type
	int32_t arrivalDist;
	if (walkOnFc) {
		arrivalDist = 3;
	} else if (waypoint.type == WaypointType::STAND || waypoint.type == WaypointType::HOLE ||
			   waypoint.type == WaypointType::STAIRS_UP || waypoint.type == WaypointType::STAIRS_DOWN) {
		arrivalDist = 0;
	} else if (waypoint.type == WaypointType::MACHETE || waypoint.type == WaypointType::USE_WITH ||
			   waypoint.type == WaypointType::LADDER || waypoint.type == WaypointType::ROPE) {
		arrivalDist = 1;
	} else if (waypoint.type == WaypointType::NPC_INTERACT) {
		arrivalDist = 3;
	} else {
		arrivalDist = 1;
	}

	// Walk-on FC: arrival requires bot to have just walked onto the FC tile
	// (bot.lastPos on/adjacent to wp at same z, now bot.currentPos.z differs).
	bool arrived;
	if (walkOnFc) {
		int32_t lastDx = std::abs(static_cast<int32_t>(bot.lastPos.x) - static_cast<int32_t>(wp.x));
		int32_t lastDy = std::abs(static_cast<int32_t>(bot.lastPos.y) - static_cast<int32_t>(wp.y));
		bool lastTickAtOrNearWp = (lastDx <= 1 && lastDy <= 1 && bot.lastPos.z == wp.z);
		arrived = lastTickAtOrNearWp && (bot.currentPos.z != wp.z);
	} else {
		bool zOk = (bot.currentPos.z == wp.z || arrivalDist == 1);
		arrived = isAtPosition(bot.currentPos, wp, arrivalDist) && zOk;
	}

	// Lua MoveEvent teleport step-on: bot was exactly on the wp last tick and is now
	// further than arrivalDist away. Catches small same-z teleports (Lion's Rock entrance:
	// 6-tile jump) that the posDiff>10 detector misses and Lua-only teleport tiles that
	// lack TILESTATE_TELEPORT.
	if (!arrived) {
		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wp.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wp.y)));
		bool teleportedFromWp = (bot.lastPos.x == wp.x && bot.lastPos.y == wp.y
								&& bot.lastPos.z == wp.z) && (dist > arrivalDist);
		if (teleportedFromWp) arrived = true;
	}

	if (arrived) {
		// Levitate: cast spell on arrival
		if (waypoint.type == WaypointType::LEVITATE_UP || waypoint.type == WaypointType::LEVITATE_DOWN) {
			castLevitateSpell(bot, waypoint);
		}

		// Handle action waypoints (machete, use_with, npc_interact)
		handleActionWaypoint(bot, waypoint);

		castLog(bot, fmt::format("PATROL: Reached {} wp {}/{} ({},{},{})",
			waypointTypeName(waypoint.type), bot.huntWaypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z));
		bot.huntWaypointIdx++;
		bot.huntWaypointSkipCount = 0;
		return true;
	}

	// --- Forward look-ahead: if bot drifted past current waypoint (e.g., chasing monster),
	// check if we're already near an upcoming same-z waypoint and skip ahead ---
	{
		int32_t bestSkipIdx = -1;
		size_t scanLimit = std::min(
			bot.huntWaypointIdx + static_cast<size_t>(PATROL_LOOKAHEAD_MAX),
			waypoints.size()
		);

		for (size_t i = bot.huntWaypointIdx + 1; i < scanLimit; i++) {
			auto& futureWp = waypoints[i];

			// STOP at floor-change waypoints (stairs, holes, ladders, etc.)
			if (isFloorChangeType(futureWp.type)) break;

			// STOP at implicit FC or teleport (next wp has different z)
			if (i + 1 < waypoints.size()) {
				int32_t dz = std::abs(
					static_cast<int32_t>(waypoints[i + 1].pos.z) -
					static_cast<int32_t>(futureWp.pos.z));
				if (dz > 0) break;
			}

			// Must be same z as bot
			if (futureWp.pos.z != bot.currentPos.z) break;

			// Check proximity with relaxed distance
			if (isAtPosition(bot.currentPos, futureWp.pos, PATROL_LOOKAHEAD_DIST)) {
				bestSkipIdx = static_cast<int32_t>(i);
				// Keep scanning — pick the FURTHEST match
			}
		}

		if (bestSkipIdx >= 0) {
			int32_t skipped = bestSkipIdx - static_cast<int32_t>(bot.huntWaypointIdx);
			castLog(bot, fmt::format("PATROL: Look-ahead skip {} wp(s) ({}->{}/{}) — bot at ({},{},{})",
				skipped, bot.huntWaypointIdx + 1, bestSkipIdx + 1, waypoints.size(),
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
			bot.huntWaypointIdx = static_cast<size_t>(bestSkipIdx);
			bot.huntWaypointSkipCount = 0;
			return false;  // let navigateToHuntWaypoint walk the final 1-3 tiles forward
		}
	}

	return false;
}

bool BotEngine::navigateToHuntWaypoint(BotState& bot) {
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script || script->patrolWaypoints.empty()) return false;

	auto& waypoints = script->patrolWaypoints;
	if (bot.huntWaypointIdx >= waypoints.size()) return false;

	auto& waypoint = waypoints[bot.huntWaypointIdx];
	auto& wp = waypoint.pos;

	// Z-transition needed (but NOT for walk-on FCs — those are handled pre-FC on same z)
	bool walkOnFc = isWalkOnFcTile(wp);

	// Z-mismatch: if not a walk-on FC tile, skip waypoint after repeated failures
	if (bot.currentPos.z != wp.z && !walkOnFc) {
		bot.huntWaypointSkipCount++;
		if (bot.huntWaypointSkipCount >= 10) {
			castLogError(bot, fmt::format("PATROL: Skipping unreachable wp {}/{} at ({},{},{}) (z mismatch: bot z={})",
				bot.huntWaypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z, bot.currentPos.z));
			bot.huntWaypointIdx++;
			bot.huntWaypointSkipCount = 0;
		}
		return true;
	}

	// Navigate on same z (or walk-on FC pre-transition)
	if (bot.currentPos.z == wp.z) {
		// Teleport STAND waypoints: A* can't route to teleport tiles (magic forcefield etc.)
		// Walk to adjacent (dist=1) then step on with internalMoveCreature
		bool isTeleportWp = false;
		{
			auto navTile = g_game().map.getTile(wp);
			if (navTile && navTile->hasFlag(TILESTATE_TELEPORT)) {
				isTeleportWp = true;
			}
		}
		if (isTeleportWp && waypoint.type == WaypointType::STAND) {
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wp.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wp.y)));
			if (dist <= 1) {
				auto tpPlayer = bot.getPlayer();
				if (!tpPlayer) return false;
				// Adjacent — step onto teleport tile with FLAG_NOLIMIT
				int32_t dx = static_cast<int32_t>(wp.x) - static_cast<int32_t>(bot.currentPos.x);
				int32_t dy = static_cast<int32_t>(wp.y) - static_cast<int32_t>(bot.currentPos.y);
				Direction dir;
				if (dx == 0 && dy == -1) dir = DIRECTION_NORTH;
				else if (dx == 1 && dy == 0) dir = DIRECTION_EAST;
				else if (dx == 0 && dy == 1) dir = DIRECTION_SOUTH;
				else if (dx == -1 && dy == 0) dir = DIRECTION_WEST;
				else if (dx == 1 && dy == -1) dir = DIRECTION_NORTHEAST;
				else if (dx == 1 && dy == 1) dir = DIRECTION_SOUTHEAST;
				else if (dx == -1 && dy == 1) dir = DIRECTION_SOUTHWEST;
				else dir = DIRECTION_NORTHWEST;
				castLog(bot, fmt::format("PATROL: Stepping onto teleport tile wp {}/{} at ({},{},{}) from dist={}",
					bot.huntWaypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z, dist));
				g_game().internalMoveCreature(tpPlayer, dir, FLAG_NOLIMIT);
			} else {
				// Not adjacent yet — walk to within 1 tile
				goToWithDoors(bot, wp, 1, waypoint.type);
			}
			return true;
		}

		// NODE waypoints: pick a random tile from the 9-sqm area (center + 8 adjacent) and pathfind to it
		// Other waypoints: pathfind directly to the waypoint position
		bool navOk = false;
		if (waypoint.type == WaypointType::NODE) {
			Position candidates[9];
			int count = 0;
			for (int32_t dx = -1; dx <= 1; dx++) {
				for (int32_t dy = -1; dy <= 1; dy++) {
					Position pos = wp;
					pos.x += dx;
					pos.y += dy;
					candidates[count++] = pos;
				}
			}
			// Fisher-Yates shuffle
			for (int i = count - 1; i > 0; i--) {
				int j = uniform_random(0, i);
				std::swap(candidates[i], candidates[j]);
			}
			for (int i = 0; i < count; i++) {
				auto candidateTile = g_game().map.getTile(candidates[i]);
				if (candidateTile && !candidateTile->hasFlag(TILESTATE_BLOCKPATH)) {
					if (goToWithDoors(bot, candidates[i], 0)) {
						navOk = true;
						break;
					}
				}
			}
		} else {
			// Walk-on FC: pathfind directly onto the FC tile (A* patched for bots)
			int32_t navDist;
			if (walkOnFc) {
				navDist = 0;
			} else if (waypoint.type == WaypointType::STAND || waypoint.type == WaypointType::HOLE ||
					   waypoint.type == WaypointType::STAIRS_UP || waypoint.type == WaypointType::STAIRS_DOWN) {
				navDist = 0;
			} else {
				navDist = 1;
			}
			navOk = goToWithDoors(bot, wp, navDist, waypoint.type);
		}
		if (!navOk) {
			if (waypoint.type != WaypointType::NODE) {
				tryAttackBlockingMonster(bot);
			}
			bot.huntWaypointSkipCount++;
			if (bot.huntWaypointSkipCount >= static_cast<uint32_t>(HUNT_STUCK_THRESHOLD)) {
				abortHunt(bot, "stuck at waypoint");
				return false;
			}
			if (bot.huntWaypointSkipCount > 5) {
				castLogError(bot, fmt::format("PATROL: Skipping stuck {} wp {}/{} at ({},{},{})", waypointTypeName(waypoint.type), bot.huntWaypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z));
				trackNavEvent("patrol_wp_stuck", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
					bot.townId, "patrol",
					fmt::format("wp {}/{} type={}", bot.huntWaypointIdx + 1, waypoints.size(), waypointTypeName(waypoint.type)));
				bot.huntWaypointIdx++;
			}
		}
	}

	return true;
}

void BotEngine::scanAndAttackMonster(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// If we have a current target, check if still valid
	if (bot.huntTargetId > 0) {
		auto target = g_game().getCreatureByID(bot.huntTargetId);
		if (!target || target->isRemoved() || target->getHealth() <= 0) {
			// Target dead — count kill
			bot.huntKillCount++;
			bot.lastKillTime = OTSYS_TIME();
			// BOT_CORPSE_LOOT: force a census on the very next tick. This is what bounds
			// adjacent-open latency to ~one tick and catches the corpse of a monster killed
			// at max range just before the patrol walks on. Guarded so a disabled feature
			// never allocates a map entry.
			if (lootCfg_.enable) s_lootRun[bot.guid].lastCensusMs = 0;
			// Track party hunt kills for death-abort threshold
			if (bot.partyHuntId > 0) {
				auto kcIt = s_partyHuntKillCount.find(bot.partyHuntId);
				if (kcIt != s_partyHuntKillCount.end()) {
					kcIt->second++;
				}
			}
			bot.huntTargetId = 0;
			bot.huntChaseFailCount = 0;
			s_retreatUntil.erase(bot.guid);
			s_approachCooldown.erase(bot.guid);
			player->setAttackedCreature(nullptr);
			if (bot.huntDebugKillLimit > 0) {
				castLog(bot, fmt::format("KILL: Monster killed! (kills={}/{})",
					bot.huntKillCount, bot.huntDebugKillLimit));
			} else {
				castLog(bot, fmt::format("KILL: Monster killed! (kills={})", bot.huntKillCount));
			}
			return;
		}

		auto tpos = target->getPosition();
		if (tpos.z != bot.currentPos.z) {
			// Target on different z — give up and clear ignored monsters
			// since reachability changes on a different z-level
			bot.huntTargetId = 0;
			bot.huntChaseFailCount = 0;
			bot.huntIgnoredMonsters.clear();
			s_retreatUntil.erase(bot.guid);
			player->setAttackedCreature(nullptr);
			return;
		}

		// Re-set attacked creature each tick so engine auto-attacks keep firing
		// But suppress during z-change grace period (server blocks attacks after floor change)
		{
			auto zIt = s_lastZChangeTime.find(bot.guid);
			bool inZGrace = (zIt != s_lastZChangeTime.end() && OTSYS_TIME() - zIt->second < Z_CHANGE_GRACE_MS);
			if (!inZGrace && player->getAttackedCreature() != target) {
				player->setAttackedCreature(target);
			}
		}

		// Track combat progress before attempting attack/chase
		int64_t progressBefore = bot.lastCombatProgress;

		// Party hunt EK: cast exeta res (challenge) to hold aggro before attacking
		if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
			tryCastChallenge(bot);
		}

		// Attack and chase
		castSpell(bot, target);
		chaseTarget(bot, target);

		// --- Check 1: Pathfinding reachability ("There is no way.") ---
		// BOT_LURE_KITE: suppressed mid-kite. The probe runs from the bot's CURRENT
		// (fleeing) position, so a target the bot is deliberately backing away from —
		// typically around a corner by then — reads as unreachable and gets dropped into
		// huntIgnoredMonsters for the rest of the lap. Kite bursts set lastCombatProgress
		// exactly like the stock retreat does, which already covers burst ticks; this
		// covers the retreat-cooldown ticks in between, where nothing records progress.
		// Bounded by the kite's own maxLegs/maxMs plus botKiteCooldownMs, which is set on
		// give-up exits precisely so this suppression cannot be re-armed forever.
		if (!botIsKiting(bot.guid) && bot.lastCombatProgress == progressBefore) {
			// No spell cast AND no walk started this tick.
			// Use the server's own pathfinding to determine if target is truly unreachable.
			// This is the same getPathMatching() that returns RETURNVALUE_THEREISNOWAY.
			auto tpos2 = target->getPosition();

			// Quick LOS check — if clear line of sight, target is reachable
			bool canReach = g_game().map.isSightClear(bot.currentPos, tpos2, true);

			// If LOS blocked (wall), check if walkable path exists around obstacle
			if (!canReach) {
				uint8_t baseVoc2 = getBaseVocation(bot.vocationId);
				int32_t range2 = getAttackRange(baseVoc2);
				int32_t keepDist2 = getEffectiveKeepDistance(bot);
				FindPathParams fpp;
				fpp.fullPathSearch = true;
				fpp.clearSight = false;
				fpp.allowDiagonal = true;
				fpp.keepDistance = false;
				fpp.maxSearchDist = PATH_MAX_DIST;
				fpp.minTargetDist = keepDist2 > 0 ? keepDist2 : 0;
				fpp.maxTargetDist = range2;

				std::vector<Direction> dirList;
				canReach = g_game().map.getPathMatching(player, tpos2, dirList,
					FrozenPathingConditionCall(tpos2), fpp);
			}

			if (!canReach) {
				// No LOS and no walkable path — "There is no way." → abandon immediately
				castLogError(bot, fmt::format("CHASE_FAIL: {} (id={}) at ({},{},{}) — no path (There is no way.)",
					target->getName(), bot.huntTargetId,
					tpos2.x, tpos2.y, tpos2.z));
				bot.huntIgnoredMonsters.insert(bot.huntTargetId);
				bot.huntTargetId = 0;
				bot.huntChaseFailCount = 0;
				s_targetHpTracker.erase(bot.guid);
				s_lastTrackedTargetId.erase(bot.guid);
				player->setAttackedCreature(nullptr);
				return;
			}
			// Path exists but no progress this tick — keep trying (cooldown, walking, etc.)
		} else {
			bot.huntChaseFailCount = 0;
		}

		// --- Check 2: HP-not-decreasing timeout (10 seconds) ---
		int32_t currentTargetHp = target->getHealth();
		int64_t now = OTSYS_TIME();

		// Initialize or reset tracking on target change
		auto trackedIt = s_lastTrackedTargetId.find(bot.guid);
		if (trackedIt == s_lastTrackedTargetId.end() || trackedIt->second != bot.huntTargetId) {
			s_targetHpTracker[bot.guid] = { currentTargetHp, now };
			s_lastTrackedTargetId[bot.guid] = bot.huntTargetId;
		} else {
			auto& [lastHp, lastDecreaseTime] = s_targetHpTracker[bot.guid];
			if (currentTargetHp < lastHp) {
				// HP decreased — update tracking
				lastHp = currentTargetHp;
				lastDecreaseTime = now;
			}
			// Don't count z-change grace period against HP timeout
			{
				auto zIt2 = s_lastZChangeTime.find(bot.guid);
				if (zIt2 != s_lastZChangeTime.end() && now - zIt2->second < Z_CHANGE_GRACE_MS) {
					lastDecreaseTime = now;
				}
			}
			// BOT_LURE_KITE: same exemption, same reason. Damage is legitimately slow
			// while the bot is repositioning between 2-step bursts, so the 10s
			// no-HP-decrease rule would abandon a perfectly valid target purely for
			// being kited. Bounded by the kite's own timeouts + give-up cooldown.
			if (botIsKiting(bot.guid)) {
				lastDecreaseTime = now;
			}
			// Check if 10 seconds passed with no HP decrease while actively attacking
			if (now - lastDecreaseTime > HUNT_HP_STUCK_TIMEOUT * 1000 && bot.lastCombatProgress > 0) {
				castLogError(bot, fmt::format("CHASE_FAIL: {} (id={}) HP stuck at {} for {}s, abandoning",
					target->getName(), bot.huntTargetId, currentTargetHp, HUNT_HP_STUCK_TIMEOUT));
				bot.huntIgnoredMonsters.insert(bot.huntTargetId);
				bot.huntTargetId = 0;
				bot.huntChaseFailCount = 0;
				s_targetHpTracker.erase(bot.guid);
				s_lastTrackedTargetId.erase(bot.guid);
				player->setAttackedCreature(nullptr);
				return;
			}
		}

		return;
	}

	// No target — scan for monsters
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script) return;

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	int32_t attackRange = getAttackRange(baseVoc);
	int32_t keepDist = getEffectiveKeepDistance(bot);

	// Uses cached spectators (target selection cadence, 600ms TTL).
	refreshSpectatorCacheIfStale(bot);

	std::shared_ptr<Creature> bestTarget;
	int32_t bestDist = 999;
	bool bestIsSafe = false; // whether bestTarget is already at safe distance

	for (uint32_t mid : bot.cachedMonsterIds) {
		auto creature = g_game().getCreatureByID(mid);
		if (!creature || creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto cpos = creature->getPosition();
		if (cpos.z != bot.currentPos.z) continue;

		uint32_t cid = creature->getID();
		if (bot.huntIgnoredMonsters.count(cid)) continue;

		// PERF_INVESTIGATION_2026-05-24 Tier 1-C: swap per-creature std::transform
		// lowercase + == compare for strcasecmp. The target list is pre-lowered at
		// script load time (bot_engine.cpp:11091), so we don't need to lowercase
		// the live name — strcasecmp handles case-insensitive compare in one pass
		// with no allocation. Prior profile showed asLowerCaseString 28.66%,
		// tolower 27.62% in samples where hunt scan dominated.
		// Empty targetNames means "attack all monsters" — only honored during PATROLLING,
		// not for quests, and not for traveling-category scripts (Fix #7 — see findThreatCentroid).
		const char* monsterNameC = creature->getName().c_str();

		bool nameMatch = false;
		if (script->targetNames.empty()) {
			nameMatch = (bot.huntPhase == HuntPhase::PATROLLING && !script->isQuest
			             && script->scriptCategory != "traveling");
		} else {
			for (const auto& targetName : script->targetNames) {
				if (strcasecmp(monsterNameC, targetName.c_str()) == 0) {
					nameMatch = true;
					break;
				}
			}
		}
		// A quest fights back on every leg against whatever is attacking it, regardless of the
		// target list. Checked after the name match so the cheap path wins for hunts.
		if (!nameMatch && botIsQuestRetaliationTarget(script, player->getID(), creature)) {
			nameMatch = true;
		}
		if (!nameMatch) continue;

		// Verify monster is reachable: walkable path + LOS from destination
		// clearSight=true ensures FrozenPathingConditionCall checks isSightClear(testPos, targetPos)
		// so A* only accepts positions where the bot can actually see/hit the monster
		{
			FindPathParams fpp;
			fpp.fullPathSearch = true;
			fpp.clearSight = true;
			fpp.allowDiagonal = true;
			fpp.keepDistance = false;
			fpp.maxSearchDist = MONSTER_SCAN_RADIUS;
			fpp.minTargetDist = 0;  // reachability: can path through close tiles
			fpp.maxTargetDist = attackRange;

			std::vector<Direction> dirList;
			if (!g_game().map.getPathMatching(player, cpos, dirList,
					FrozenPathingConditionCall(cpos), fpp)) {
				continue; // no walkable path — skip
			}
		}

		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cpos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cpos.y)));

		// When keepDistance active, prefer targets already at safe distance
		// to avoid picking melee-range monsters that force immediate retreat
		if (keepDist > 0) {
			bool isSafe = (dist >= keepDist && dist <= attackRange);
			if (isSafe && !bestIsSafe) {
				bestTarget = creature; bestDist = dist; bestIsSafe = true;
			} else if (isSafe == bestIsSafe && dist < bestDist) {
				bestTarget = creature; bestDist = dist; bestIsSafe = isSafe;
			}
		} else {
			if (dist < bestDist) {
				bestDist = dist;
				bestTarget = creature;
			}
		}
	}

	if (bestTarget) {
		bot.huntTargetId = bestTarget->getID();
		bot.huntChaseFailCount = 0;

		// Set attacked creature so the engine auto-attack loop fires (weapon attacks)
		player->setAttackedCreature(bestTarget);

		// For keepDist bots: chaseMode=true auto-sets followCreature which walks
		// the bot to dist=0, completely overriding our keepDistance retreat logic.
		// Clear follow so only our chaseTarget() controls positioning.
		int32_t kd = getEffectiveKeepDistance(bot);
		if (kd > 0) {
			player->setFollowCreature(nullptr);
		}

		castLog(bot, fmt::format("TARGET: {} at ({},{},{}) dist={}",
			bestTarget->getName(),
			bestTarget->getPosition().x, bestTarget->getPosition().y, bestTarget->getPosition().z,
			bestDist));
	}
}

bool BotEngine::hasNearbyReachableTargets(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return false;

	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script) return false;

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	int32_t attackRange = getAttackRange(baseVoc);
	int32_t keepDist = getEffectiveKeepDistance(bot);

	// Uses cached spectators (target selection cadence, 600ms TTL).
	refreshSpectatorCacheIfStale(bot);

	for (uint32_t mid : bot.cachedMonsterIds) {
		auto creature = g_game().getCreatureByID(mid);
		if (!creature || creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto mpos = creature->getPosition();
		if (mpos.z != bot.currentPos.z) continue;
		if (bot.huntIgnoredMonsters.count(creature->getID())) continue;

		// Name match.
		// Empty targetNames means "attack all monsters" — only honored during PATROLLING,
		// not for quests, and not for traveling-category scripts (Fix #7 — see findThreatCentroid).
		std::string monsterName = creature->getName();
		std::transform(monsterName.begin(), monsterName.end(), monsterName.begin(), ::tolower);
		bool nameMatch = false;
		if (script->targetNames.empty()) {
			nameMatch = (bot.huntPhase == HuntPhase::PATROLLING && !script->isQuest
			             && script->scriptCategory != "traveling");
		} else {
			for (const auto& targetName : script->targetNames) {
				if (monsterName == targetName) { nameMatch = true; break; }
			}
		}
		// Quest retaliation — same rule as scanAndAttackMonster, so "is anything worth staying
		// for" agrees with "is anything worth attacking".
		if (!nameMatch && botIsQuestRetaliationTarget(script, player->getID(), creature)) {
			nameMatch = true;
		}
		if (!nameMatch) continue;

		// Verify monster is reachable: walkable path + LOS from destination
		{
			FindPathParams fpp;
			fpp.fullPathSearch = true;
			fpp.clearSight = true;
			fpp.allowDiagonal = true;
			fpp.keepDistance = false;
			fpp.maxSearchDist = MONSTER_SCAN_RADIUS;
			fpp.minTargetDist = 0;  // reachability: can path through close tiles
			fpp.maxTargetDist = attackRange;

			std::vector<Direction> dirList;
			if (!g_game().map.getPathMatching(player, mpos, dirList,
					FrozenPathingConditionCall(mpos), fpp)) {
				continue; // no walkable path — skip
			}
		}

		return true;
	}
	return false;
}

void BotEngine::endHunt(BotState& bot) {
	auto player = bot.getPlayer();

	// BOT_LURE_KITE hygiene. Correctness does NOT depend on this — both features
	// re-validate their own gate every tick and self-clear (beginHuntPhase is
	// explicitly not a funnel). This just keeps stale entries from lingering.
	clearLureKiteState(bot.guid);

	// Release reservation
	activeHunts_.erase(bot.huntScriptId);
	for (auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
			activeSpawnGroups_.erase(s.spawnGroup);
			break;
		}
	}

	if (player) {
		player->setAttackedCreature(nullptr);
	}

	castLog(bot, fmt::format("HUNT END: script={} kills={}", bot.huntScriptId, bot.huntKillCount));

	// Dissolve party hunt if this EK was leading one
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
		dissolvePartyHunt(bot.partyHuntId, "hunt_end");
		bot.isPartyHuntLeader = false;
		bot.partyHuntId = 0;
		bot.partyRole = 0;
		bot.partyLeaderGuid = 0;
	}

	// Teleport home if far
	int32_t dist = 999;
	if (player) {
		auto town = player->getTown();
		if (town) {
			auto templePos = town->getTemplePosition();
			dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(templePos.x)),
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(templePos.y)));
		}
	}
	if (dist > 50) {
		if (!findNearestRecoveryRoute(bot)) {
			teleportToTemple(bot);
		} else {
			return; // navigating back via recovery route
		}
	}

	// Clear hunt state
	bot.huntScriptId = 0;
	bot.huntKillCount = 0;
	bot.huntTargetId = 0;
	bot.huntIgnoredMonsters.clear();
	bot.huntCooldownUntil = OTSYS_TIME() + uniform_random(HUNT_COOLDOWN_MIN, HUNT_COOLDOWN_MAX) * 1000LL;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.recoveryWaypoints.clear();
	bot.isRecoveryRoute = false;

	bot.state = BotAIState::IDLE;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	bot.nextRerollTime = OTSYS_TIME() + uniform_random(10, 30) * 1000;
}

void BotEngine::abortHunt(BotState& bot, const std::string& reason) {
	auto player = bot.getPlayer();

	// BOT_LURE_KITE hygiene. Correctness does NOT depend on this — both features
	// re-validate their own gate every tick and self-clear (beginHuntPhase is
	// explicitly not a funnel). This just keeps stale entries from lingering.
	clearLureKiteState(bot.guid);

	s_leavingPhaseStart.erase(bot.guid);
	s_leavingWpTimer.erase(bot.guid);
	s_huntTravelStart.erase(bot.guid);
	endIceFishSession(bot, "hunt aborted");
	activeHunts_.erase(bot.huntScriptId);
	for (auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
			activeSpawnGroups_.erase(s.spawnGroup);
			break;
		}
	}

	if (player) {
		player->setAttackedCreature(nullptr);
	}

	castLogError(bot, fmt::format("HUNT ABORT: script={} reason='{}' kills={}",
		bot.huntScriptId, reason, bot.huntKillCount));
	trackNavEvent("hunt_abort", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
		bot.townId, "hunt",
		fmt::format("reason={} kills={}", reason, bot.huntKillCount));

	// Dissolve party hunt if this EK was leading one
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
		dissolvePartyHunt(bot.partyHuntId, fmt::format("hunt_abort:{}", reason));
		bot.isPartyHuntLeader = false;
		bot.partyHuntId = 0;
		bot.partyRole = 0;
		bot.partyLeaderGuid = 0;
	}

	// Clean up stale hunt combat state BEFORE recovery (these don't belong to recovery)
	bot.huntTargetId = 0;
	bot.huntKillCount = 0;
	bot.huntIgnoredMonsters.clear();
	bot.huntScriptId = 0; // Clear before recovery route check — prevents stale state in doActivityReroll

	// Try recovery route (except for PK threat — urgent, just teleport)
	if (reason == "pk_threat" || !findNearestRecoveryRoute(bot)) {
		teleportToTemple(bot);
	} else {
		return; // navigating back via recovery route (beginHuntPhase sets s_leavingPhaseStart)
	}

	bot.huntCooldownUntil = OTSYS_TIME() + uniform_random(300, 600) * 1000LL;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.recoveryWaypoints.clear();
	bot.isRecoveryRoute = false;

	bot.state = BotAIState::IDLE;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	bot.nextRerollTime = OTSYS_TIME() + uniform_random(5, 15) * 1000;
}

void BotEngine::doHuntResupply(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// 5-minute overall timeout
	if (OTSYS_TIME() - bot.huntResupplyStart > RESUPPLY_TIMEOUT * 1000LL) {
		castLogError(bot, "RESUPPLY: Timeout, finishing");
		finishResupplyAndReroll(bot);
		return;
	}

	// PZ-blocked: don't walk the depot/shop legs into a protection zone while pz-locked (the
	// real 60s server lock blocks PZ entry). Defer until it clears; the 5-min timeout above
	// remains a valid backstop in the pathological repeated-relock case.
	if (player->isPzLocked()) return;

	// Run preparation state machine (depot + shop visits)
	doHuntPrepare(bot);

	// Intercept after depot wait (prepareStep transitions from 1→2):
	// Reroll BEFORE visiting shops to avoid wasted shop trips if switching cities
	if (bot.prepareStep == 2 && !bot.resupplyRerolled) {
		bot.resupplyRerolled = true;
		castLog(bot, "RESUPPLY: At depot, re-rolling hunt");

		// Release old hunt reservation
		uint32_t oldScriptId = bot.huntScriptId;
		activeHunts_.erase(bot.huntScriptId);
		for (auto& s : huntScripts_) {
			if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
				activeSpawnGroups_.erase(s.spawnGroup);
				break;
			}
		}

		// Clear hunt state for reroll
		bot.huntScriptId = 0;
		bot.huntKillCount = 0;
		bot.huntTargetId = 0;
		bot.huntIgnoredMonsters.clear();

		// BOT_PARTY_INVITE_RENDEZVOUS trap #1 - tryStartHunt has NO party guard of its own, so a
		// member winding down for a party assembly would start a brand new hunt right here and
		// never converge. End the hunt instead and let the supervisor observe the release.
		if (s_rvMember.count(bot.guid) > 0) { endHunt(bot); return; }

		// botResupplyRehuntPct: go straight back out instead of returning to TABLE A.
		// NOTE both LIVE resupply sites use this key; the VIRTUAL (hibernated) resupply path in
		// bot_tick.cpp has no equivalent roll and always returns to TABLE A. That asymmetry is
		// pre-existing and deliberate -- a hibernated bot's re-hunt is decided by the virtual
		// simulator, not here -- but it means this key describes awake bots only.
		if (uniform_random(1, 100) <= static_cast<int32_t>(g_configManager().getNumber(BOT_RESUPPLY_REHUNT_PCT))
		&& tryStartHunt(bot)) {
			// tryStartHunt handles everything:
			// - Different town → startTravel() already called
			// - Same town → beginHuntPhase(PREPARING) resets prepareStep=0,
			//   runs full depot→shop→travel_to cycle (depot visit will be fast since we're there)
			return;
		}

		// No new hunt — end hunting, go IDLE
		endHunt(bot);
		return;
	}

	// If prepareStep reached 4 (done) without reroll (shouldn't normally happen),
	// use fallback reroll
	if (bot.prepareStep >= 4) {
		castLog(bot, "RESUPPLY: Complete, re-rolling (fallback)");
		finishResupplyAndReroll(bot);
	}
}

void BotEngine::finishResupplyAndReroll(BotState& bot) {
	// Reset stuck-loop counters on reroll
	s_fcConsecutiveFailures.erase(bot.guid);
	clearDepotBlacklist(bot.guid);
	clearLureKiteState(bot.guid); // BOT_LURE_KITE hygiene (see unregisterBot)

	// Dissolve party hunt if EK leader is resupplying
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
		dissolvePartyHunt(bot.partyHuntId, "resupply_reroll");
		bot.isPartyHuntLeader = false;
		bot.partyHuntId = 0;
		bot.partyRole = 0;
		bot.partyLeaderGuid = 0;
	}

	uint32_t oldScriptId = bot.huntScriptId;

	// Release old hunt
	activeHunts_.erase(bot.huntScriptId);
	for (auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
			activeSpawnGroups_.erase(s.spawnGroup);
			break;
		}
	}

	// Try to start a new hunt
	bot.huntScriptId = 0;
	bot.huntKillCount = 0;
	bot.huntTargetId = 0;
	bot.huntIgnoredMonsters.clear();

	// BOT_PARTY_INVITE_RENDEZVOUS trap #1 (second site) - same reasoning as doHuntResupply.
	if (s_rvMember.count(bot.guid) > 0) { endHunt(bot); return; }

	// botResupplyRehuntPct -- see the note at the other call site in doHuntResupply.
	if (uniform_random(1, 100) <= static_cast<int32_t>(g_configManager().getNumber(BOT_RESUPPLY_REHUNT_PCT))
		&& tryStartHunt(bot)) {
		// Started a new hunt — if it's the same script, resume it.
		//
		// This used to go straight to PATROLLING with no teleport, on the theory that the
		// bot was still "at spawn". It is not: the only caller of finishResupplyAndReroll is
		// doHuntResupply, so the bot has already walked home and resupplied and is standing
		// in TOWN. Jumping to PATROLLING there re-rolled a random entry index with no regard
		// for the bot's actual position — the same landing/index mismatch as sites A-D, just
		// without a teleport to hide it. TRAVEL_TO is the phase that already knows how to
		// close a long, possibly cross-floor gap to the patrol entry.
		if (bot.huntScriptId == oldScriptId) {
			beginHuntPhase(bot, HuntPhase::TRAVEL_TO);
		}
		return;
	}

	// Otherwise go back to full random IDLE behavior
	endHunt(bot);
}

// ============================================================================
// BOT_CORPSE_LOOT — open the corpses we killed so the loot highlight goes away
// ----------------------------------------------------------------------------
// Design notes live above `struct LootRun` in bot_engine_impl.hpp. The short
// version: the highlight is one bool on the Container, clearing it is one stock
// call, and the whole difficulty is that CORPSEOWNER — the only thing that says
// "this kill was mine" — is stripped ~10s after the kill while the highlight
// survives for another 300-600s.
// ============================================================================

// Player corpses are never ours and must never be touched: they carry no
// CORPSEOWNER, so being publicly lootable (and publicly highlighted) is stock
// behaviour, and a player coming back for their own corpse needs that marker.
// The whole decay chain is walked once from the two base ids, because the later
// stages have different ids and would otherwise slip past an id == 4240 test.
static bool isPlayerCorpseId(uint16_t id) {
	static const std::unordered_set<uint16_t> chain = [] {
		std::unordered_set<uint16_t> out;
		for (uint16_t seed : { static_cast<uint16_t>(ITEM_MALE_CORPSE), static_cast<uint16_t>(ITEM_FEMALE_CORPSE) }) {
			uint16_t cur = seed;
			for (int guard = 0; guard < 16 && cur != 0; ++guard) {
				if (!out.insert(cur).second) break; // cycle guard
				const int32_t next = Item::items[cur].decayTo;
				if (next <= 0) break;
				cur = static_cast<uint16_t>(next);
			}
		}
		return out;
	}();
	return chain.count(id) > 0;
}

static int32_t lootCheb(const Position& a, const Position& b) {
	return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
	                std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
}

void BotEngine::clearLootState(uint32_t guid) {
	s_lootRun.erase(guid);
}

// Shared preamble for census / adjacent / walk. Kept as one function so the
// "never while a real player is in the party" belt cannot be forgotten by one pass.
bool BotEngine::lootGatePasses(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!lootCfg_.enable) return false;
	if (bot.huntPhase != HuntPhase::PATROLLING) return false;
	if (!player || player->isRemoved() || player->getHealth() <= 0) return false;
	if (bot.deathPauseUntil > OTSYS_TIME()) return false;
	// A human in the party is using the highlight as their own loot marker. Note this is
	// belt-and-braces: party-hunt supports and human-led members carry BotAIState::PARTY
	// and never reach doHuntPatrol at all.
	if (const auto& party = player->getParty()) {
		auto isReal = [](const std::shared_ptr<Player>& p) { return p && !p->isBotPlayer(); };
		if (isReal(party->getLeader())) return false;
		for (const auto& m : party->getMembers()) {
			if (isReal(m)) return false;
		}
		for (const auto& i : party->getInvitees()) {
			if (isReal(i)) return false;
		}
	}
	return true;
}

// Mirrors Player::canOpenCorpse / Party::canOpenCorpse, restricted to bots. We cannot
// call canOpenCorpse itself because we bypass Actions::internalUseItem entirely.
bool BotEngine::lootOwnerIsOurs(const std::shared_ptr<Player>& player, uint32_t ownerId) {
	if (ownerId == 0 || !player) return false;
	if (ownerId == player->getID()) return true;
	const auto& party = player->getParty();
	if (!party) return false;
	const auto& ownerCreature = g_game().getCreatureByID(ownerId);
	const auto& ownerPlayer = ownerCreature ? ownerCreature->getPlayer() : nullptr;
	return ownerPlayer && ownerPlayer->isBotPlayer() && ownerPlayer->getParty() == party;
}

bool BotEngine::lootRealPlayerOnScreen(BotState& bot, const std::shared_ptr<Player>& player) {
	auto& run = s_lootRun[bot.guid];
	const int64_t now = OTSYS_TIME();
	if (run.obsCacheMs != 0 && now - run.obsCacheMs < LOOT_OBS_CACHE_MS) return run.obsCache;
	run.obsCacheMs = now;
	bool seen = false;
	for (const auto& spectator : Spectators().find<Player>(player->getPosition(), true)) {
		const auto& other = spectator ? spectator->getPlayer() : nullptr;
		if (!other || other->getID() == player->getID()) continue;
		if (!other->isBotPlayer()) { seen = true; break; }
	}
	run.obsCache = seen;
	return seen;
}

// THE accept predicate. Every pass goes through this one function — the adjacent pass
// runs during combat in shared spawns, so a partial copy of these conditions there would
// be exactly how a bot ends up clearing an unaffiliated real player's fresh corpse.
bool BotEngine::botMayOpenCorpse(BotState& bot, const std::shared_ptr<Player>& player,
	const std::shared_ptr<Container>& corpse) {
	if (!corpse || !player) return false;
	if (!corpse->isCorpse() || corpse->isRewardCorpse()) return false;
	if (corpse->empty() || !corpse->hasLootHighlight()) return false;
	if (isPlayerCorpseId(corpse->getID())) return false;

	auto& run = s_lootRun[bot.guid];
	for (const auto& w : run.blocked) {
		if (w.lock() == corpse) return false;
	}

	const uint32_t owner = corpse->getCorpseOwner();
	if (owner != 0) {
		return lootOwnerIsOurs(player, owner);
	}
	// owner == 0: either stripped by the first decay stage (~10s) or never set. Claimed
	// means we saw it while it was still provably ours, which is the only key that
	// outlives the strip.
	for (const auto& w : run.claimed) {
		if (w.lock() == corpse) return true;
	}
	if (!lootCfg_.publicCleanup) return false;
	// Ownerless corpses are lootable by anyone, so opening one is what a passing real
	// player does — but not in front of a human who may be coming back for it.
	if (lootRealPlayerOnScreen(bot, player)) {
		s_lootStats.guardSuppressed++;
		return false;
	}
	return true;
}

void BotEngine::openCorpse(BotState& bot, const std::shared_ptr<Container>& corpse,
	const Position& at, const char* mode) {
	const bool wasPublic = corpse->getCorpseOwner() == 0;
	// nullptr, NOT the bot: with a player argument Item::sendUpdateToClient targets only
	// that player (or their party), and a clientless bot would send nothing at all. With
	// nullptr every spectator re-renders the tile item and the highlight disappears for
	// all of them. No loot is removed — the corpse stays fully lootable.
	corpse->clearLootHighlight();

	auto& run = s_lootRun[bot.guid];
	// The walk window is refreshed HERE, on a successful open, and nowhere else. Refreshing
	// on scan success would let a single unreachable corpse in radius hold the window open
	// forever via 750ms pick-drop churn.
	run.windowUntilMs = OTSYS_TIME() + lootCfg_.windowMs;

	// Attribute by HOW the bot got here, not by who owns the corpse now. Classifying on
	// ownership made every open past the 10s strip read as "public" and left openedWalk
	// structurally unreachable, so the counters could never show whether walking worked.
	const bool viaWalk = !run.deliveredByWalk.expired() && run.deliveredByWalk.lock() == corpse;
	if (viaWalk) {
		s_lootStats.openedWalk++;
		run.deliveredByWalk.reset();
	} else if (wasPublic) {
		s_lootStats.openedPublic++;
	} else {
		s_lootStats.openedAdj++;
	}
	(void)mode;
	// A sweep of several corpses holds waypoint advancement for seconds at a time, and
	// s_routeWpTimer is wall-clock, so without this a sweep can push a waypoint past
	// perWpStuckMs (30s) and fire a spurious "Skipping stuck wp".
	if (auto it = s_routeWpTimer.find(bot.guid); it != s_routeWpTimer.end()) {
		it->second.second = OTSYS_TIME();
	}
	castLog(bot, fmt::format("LOOT: opened corpse {} at ({},{},{}) mode={}",
		corpse->getID(), at.x, at.y, at.z, mode));
}

// Ends the active WALK run. `blacklist` distinguishes "this corpse defeated us" from
// "something interrupted us": a run dropped because combat resumed must NOT be
// blacklisted, or a 30s fight would permanently condemn a perfectly reachable corpse.
void BotEngine::endLootRun(BotState& bot, bool blacklist, bool cancelWalk) {
	auto& run = s_lootRun[bot.guid];
	if (cancelWalk && run.hasTarget) {
		// Same pattern exitCombat uses. Dropping only the bookkeeping left the bot
		// finishing a detour it had already abandoned, with combat's retreat blocked
		// behind it for the whole route.
		if (const auto& p = bot.getPlayer()) {
			p->listWalkDir.clear();
			p->stopEventWalk();
		}
	}
	if (blacklist) {
		if (const auto& c = run.corpse.lock()) {
			if (run.blocked.size() >= LOOT_BLOCK_CAP) run.blocked.erase(run.blocked.begin());
			run.blocked.emplace_back(c);
			s_lootStats.blacklisted++;
		}
	}
	if (run.hasTarget) {
		// The per-waypoint stuck timer is wall-clock and keeps accumulating while a loot
		// run holds the tick, so back-to-back runs could push a waypoint past perWpStuckMs
		// (30s) and fire a spurious "Skipping stuck wp". Same trick the PZ-lock branch in
		// followWaypoints uses on s_routeProgress. Only touched if an entry already exists.
		if (auto it = s_routeWpTimer.find(bot.guid); it != s_routeWpTimer.end()) {
			it->second.second = OTSYS_TIME();
		}
	}
	run.corpse.reset();
	run.hasTarget = false;
	run.fails = 0;
	run.deadlineMs = 0;
	run.nextStepMs = 0;
}

void BotEngine::tickCorpseCensus(BotState& bot) {
	const auto& player = bot.getPlayer();
	if (!lootGatePasses(bot, player)) return;

	auto& run = s_lootRun[bot.guid];
	const int64_t now = OTSYS_TIME();
	if (run.lastCensusMs != 0 && now - run.lastCensusMs < lootCfg_.scanMs) return;
	run.lastCensusMs = now;
	s_lootStats.censusPasses++;

	std::erase_if(run.claimed, [](const std::weak_ptr<Container>& w) { return w.expired(); });
	std::erase_if(run.blocked, [](const std::weak_ptr<Container>& w) { return w.expired(); });
	run.candidates.clear();

	// 7 = the client viewport. The corpse lands on the MONSTER's tile, not near the bot,
	// so anything tighter silently skips most mage/paladin kills.
	const int32_t radius = std::clamp(lootCfg_.radius, 1, 12);
	const Position& me = bot.currentPos;
	for (int32_t ox = -radius; ox <= radius; ox++) {
		const int32_t px = static_cast<int32_t>(me.x) + ox;
		if (px < 0 || px > 65535) continue;
		for (int32_t oy = -radius; oy <= radius; oy++) {
			const int32_t py = static_cast<int32_t>(me.y) + oy;
			if (py < 0 || py > 65535) continue;
			const Position pos(static_cast<uint16_t>(px), static_cast<uint16_t>(py), me.z);
			const auto& tile = g_game().map.getTile(pos);
			if (!tile) continue;
			const auto* items = tile->getItemList();
			if (!items || items->empty()) continue;
			for (const auto& item : *items) {
				const auto& corpse = item ? item->getContainer() : nullptr;
				if (!corpse || !corpse->isCorpse()) continue;
				// Claim FIRST, filter second: the claim is the whole reason this pass runs
				// during combat, and it is only possible while CORPSEOWNER still exists.
				if (lootOwnerIsOurs(player, corpse->getCorpseOwner()) && !isPlayerCorpseId(corpse->getID())) {
					bool known = false;
					for (const auto& w : run.claimed) {
						if (w.lock() == corpse) { known = true; break; }
					}
					if (!known) {
						if (run.claimed.size() >= LOOT_CLAIM_CAP) run.claimed.erase(run.claimed.begin());
						run.claimed.emplace_back(corpse);
						s_lootStats.claimed++;
					}
				}
				if (!botMayOpenCorpse(bot, player, corpse)) continue;
				run.candidates.emplace_back(corpse, pos);
			}
		}
	}
}

bool BotEngine::tickCorpseOpenAdjacent(BotState& bot) {
	const auto& player = bot.getPlayer();
	if (!lootGatePasses(bot, player)) return false;

	auto& run = s_lootRun[bot.guid];
	const int64_t now = OTSYS_TIME();

	// A corpse already picked and waiting out its human pause.
	if (const auto& pending = run.adjPending.lock()) {
		const bool stillGood = botMayOpenCorpse(bot, player, pending)
			&& run.adjPendingPos.z == bot.currentPos.z
			&& lootCheb(bot.currentPos, run.adjPendingPos) <= 1;
		if (stillGood) {
			// Still waiting: HOLD the tick. Returning false here is what let the patrol
			// walk the bot away mid-pause.
			if (now < run.adjOpenAtMs) return true;
			openCorpse(bot, pending, run.adjPendingPos, "adj");
			run.adjPending.reset();
			return true;
		}
		// Armed but no longer openable — count it, so a pause dying un-opened is visible
		// instead of silent.
		s_lootStats.adjCancelled++;
	}
	run.adjPending.reset();

	for (const auto& [weakCorpse, pos] : run.candidates) {
		if (pos.z != bot.currentPos.z || lootCheb(bot.currentPos, pos) > 1) continue;
		const auto& corpse = weakCorpse.lock();
		if (!corpse || !botMayOpenCorpse(bot, player, corpse)) continue;
		// Without this pause an EK's corpses unsparkle the instant they hit the ground,
		// which reads as a script rather than a player looting between swings. The pause
		// is fine; what was missing is that somebody has to own the bot for its duration.
		run.adjPending = corpse;
		run.adjPendingPos = pos;
		run.adjOpenAtMs = now + uniform_random(
			std::min(lootCfg_.delayMinMs, lootCfg_.delayMaxMs),
			std::max(lootCfg_.delayMinMs, lootCfg_.delayMaxMs));
		s_lootStats.adjArmed++;

		// If a walk run delivered us to THIS corpse, attribute the open to the walk. The
		// old arrival branch in tickCorpseWalk could never fire — this pass arms the pause
		// on the same tick the bot becomes adjacent, one function earlier — which is why
		// walkArrived read 0 across 977 opens. That was a measurement artifact, not proof
		// that walking never worked.
		if (run.hasTarget) {
			if (run.corpse.lock() == corpse) {
				run.deliveredByWalk = corpse;
				s_lootStats.walkArrived++;
				endLootRun(bot, false, /*cancelWalk=*/false); // arrived; nothing to cancel
			} else {
				endLootRun(bot, false); // a nearer corpse superseded the run
			}
		}

		// Stop the feet. Walking is event-driven, so owning the tick is not enough: a bot
		// mid-stride toward a patrol waypoint keeps going and strolls out of range before
		// the pause expires. Never touch the feet in combat — those belong to chaseTarget.
		if (bot.huntTargetId == 0) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
		return true;
	}
	return false;
}

// One bounded A* straight to the corpse, deliberately NOT goTo(). goTo chunks long
// routes and, when the direct search fails, retries against two RANDOM offset targets
// and reports success — so a bot that could not reach a corpse would walk a couple of
// tiles to one side, re-plan from there, and drift back and forth in front of it. That
// was the visible "tries several times before it succeeds" behaviour. A corpse is at
// most botLootRadius away, well inside one search, so none of goTo's machinery is
// wanted here: either there is a path to a tile adjacent to the corpse or the run fails
// honestly and the fail cap retires it.
bool BotEngine::lootStepTo(BotState& bot, const std::shared_ptr<Player>& player, const Position& target) {
	FindPathParams fpp;
	fpp.fullPathSearch = true;
	fpp.clearSight = false;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = 20;
	fpp.minTargetDist = 0;
	fpp.maxTargetDist = 1;

	std::vector<Direction> dirList;
	if (!g_game().map.getPathMatchingCond(player, target, dirList, FrozenPathingConditionCall(target), fpp)) {
		return false;
	}
	if (dirList.empty()) return true; // already adjacent; the adjacent pass takes it
	botStartAutoWalk(bot, player, dirList);
	return true;
}

bool BotEngine::tickCorpseWalk(BotState& bot) {
	// false disables this pass outright rather than running it in drop-everything mode,
	// which would burn a pick+drop cycle every census for nothing.
	if (!lootCfg_.walk) return false;
	const auto& player = bot.getPlayer();
	if (!lootGatePasses(bot, player)) return false;

	auto& run = s_lootRun[bot.guid];
	const int64_t now = OTSYS_TIME();

	// A floor change owns the bot's feet outright; never compete with it.
	// Interruptions drop the run WITHOUT blacklisting — see endLootRun.
	if (bot.fcState != FloorChangeState::NONE) {
		if (run.hasTarget) { s_lootStats.walkDropped++; endLootRun(bot, false); }
		return false;
	}
	// NO detours while fighting. The census and the adjacent open still run in combat —
	// that is what keeps a ranged bot's kills claimable past the 10s CORPSEOWNER strip
	// and what opens corpses the bot already stands next to. Only walking is withdrawn,
	// because it had no arbiter against chaseTarget: both yield to a non-empty
	// listWalkDir, so whichever found the feet free won that step, and an EK (keep
	// distance 0, so lootDestSafeInCombat was a no-op) alternated between corpse and
	// target indefinitely. The claim outlives the strip precisely so the walk can wait.
	if (bot.huntTargetId > 0) {
		if (run.hasTarget) { s_lootStats.walkDropped++; endLootRun(bot, false); }
		return false;
	}
	// Belt: with the adjacent pass owning its own tick this should be unreachable, but if
	// a pause is armed the walk must not start a competing route.
	if (!run.adjPending.expired()) {
		if (run.hasTarget) { s_lootStats.walkDropped++; endLootRun(bot, false); }
		return false;
	}
	// lastKillTime is set when the leader's TARGET dies, with no damage-attribution check,
	// so this covers party kills too; windowUntilMs extends it across corpse chains.
	const bool windowOpen = (bot.lastKillTime > 0 && now - bot.lastKillTime <= lootCfg_.windowMs)
		|| now < run.windowUntilMs;
	if (!windowOpen) {
		if (run.hasTarget) { s_lootStats.walkDropped++; endLootRun(bot, false); }
		return false;
	}

	if (run.hasTarget) {
		const auto& corpse = run.corpse.lock();
		if (!corpse || !botMayOpenCorpse(bot, player, corpse)) {
			s_lootStats.walkDropped++;
			endLootRun(bot, false); // gone, already opened, or newly filtered out
		} else if (run.pos.z == bot.currentPos.z && lootCheb(bot.currentPos, run.pos) <= 1) {
			// Arrived. The adjacent pass takes it from here and credits walkArrived at its
			// arm site — it runs first, so crediting here would double count.
			return true;
		} else if (now >= run.deadlineMs) {
			s_lootStats.walkFail++;
			endLootRun(bot, true);
		} else {
			// Never re-path while a walk is already queued. startAutoWalk REPLACES the
			// route, so re-issuing it every LOOT_STEP_MS restarted the walk mid-stride and
			// was visible on cast as the bot stuttering back and forth in front of the
			// corpse. followWaypoints takes exactly this precaution. It also makes combat
			// movement strictly win: whatever the chase logic queued runs to completion.
			if (!player->listWalkDir.empty()) return true;
			if (now >= run.nextStepMs) {
				run.nextStepMs = now + LOOT_STEP_MS;
				if (!lootStepTo(bot, player, run.pos)) {
					if (++run.fails >= LOOT_MAX_FAILS) {
						s_lootStats.walkFail++;
						endLootRun(bot, true);
						return false;
					}
				}
			}
			return true;
		}
	}

	// Pick the nearest reachable-looking candidate that is not already adjacent.
	int32_t bestDist = std::numeric_limits<int32_t>::max();
	std::shared_ptr<Container> best;
	Position bestPos;
	for (const auto& [weakCorpse, pos] : run.candidates) {
		if (pos.z != bot.currentPos.z) continue;
		const int32_t dist = lootCheb(bot.currentPos, pos);
		if (dist <= 1 || dist >= bestDist) continue;
		const auto& corpse = weakCorpse.lock();
		if (!corpse || !botMayOpenCorpse(bot, player, corpse)) continue;
		// Cheap wall reject before we ever hand this to the pathfinder.
		if (!g_game().map.isSightClear(bot.currentPos, pos, true)) continue;
		best = corpse;
		bestPos = pos;
		bestDist = dist;
	}
	if (!best) return false;

	run.corpse = best;
	run.pos = bestPos;
	run.hasTarget = true;
	s_lootStats.runsStarted++;
	run.fails = 0;
	run.nextStepMs = 0;
	run.deadlineMs = now + lootCfg_.maxWalkMs;
	return true;
}

