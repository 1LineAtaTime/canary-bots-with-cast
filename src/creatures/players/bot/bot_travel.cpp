/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_travel.cpp — boat/carpet travel system (Phase 5)
//
// BOT_NAV_REALISM Phase 11 module split. Compiles into the SAME libbot_engine.so
// as bot_engine.cpp, so /cavebot reload is unchanged. Shared includes, engine-local
// types and the BotEngine class declaration all live in bot_engine_impl.hpp.
//
// Carved out only after tools/botnavsim/module_promote.py reported zero external
// dependencies for this range.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

// ============================================================================
// Travel system (Phase 5)
// ============================================================================

void BotEngine::startTravel(BotState& bot, uint32_t destTownId) {
	// 2026-06-10 drift cleanup: reset hunt-phase scratch state BEFORE the early-return
	// guard, so a pre-existing state=TRAVELING + huntPhase=RESUPPLYING strand (observed
	// for Freya guid=65106 overnight: 2/69 wake events showed this pattern) gets cleaned
	// up on ANY startTravel call — including no-op same-town calls. Safe because at every
	// startTravel callsite either (a) the bot is about to enter a fresh PREPARING phase
	// on arrival (cross-town hunt + return-home), or (b) no hunt is active (random travel
	// reroll). These four fields aren't read during TRAVELING state, so the reset is inert
	// in the normal path and only matters when cleaning up a stale strand.
	bot.huntPhase = HuntPhase::PREPARING;
	bot.huntResupplyStart = 0;
	bot.prepareStep = 0;
	bot.prepareWaitUntil = 0;

	if (destTownId == bot.townId) {
		castLog(bot, fmt::format("TRAVEL: Already in town {}, skipping", bot.townName));
		return;
	}
	bot.travelDestTownId = destTownId;
	bot.state = BotAIState::TRAVELING;
	bot.travelWaitUntil = 0;
	bot.pathFailCount = 0;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.travelDestVerified = false;
	bot.triedRouteSources.clear();
	bot.lastRouteDestination.clear();
	bot.travelSpreadTarget = Position(); // BOT_LIVENESS_PACK Phase C.6: fresh spread pick per trip
	s_travelFcRecoveryCount.erase(bot.guid);
	s_travelStartTime[bot.guid] = OTSYS_TIME();

	// Get destination travel position from MySQL table (randomly picks boat or carpet)
	auto [destPos, destPOI] = getTravelPosition(destTownId);
	bot.travelBoatPos = destPos;
	s_travelDestPOI[bot.guid] = destPOI;
	// BOT_TRAVEL_ARRIVE_MIX: decide NOW where this journey ends, not on arrival. Unconditional
	// and eager on purpose -- see s_travelArriveTarget in bot_engine_impl.hpp for why a lazy or
	// conditional write would leak a stale target into the NEXT journey.
	s_travelArriveTarget[bot.guid] = pickTravelArrivalTarget(bot, destTownId);

	// Get source city travel position (walk to this via city route)
	auto [srcPos, srcPOI] = getTravelPosition(bot.townId);
	bot.travelSrcBoatPos = srcPos;
	s_travelSrcPOI[bot.guid] = srcPOI;
	if (bot.travelSrcBoatPos.x > 0) {
		// No teleporting — bot will walk via city route
		bot.travelPhase = "walk_to_boat";
		castLog(bot, fmt::format("TRAVEL: Walking to {} at ({},{},{}) to travel to town {} (dest={})",
			srcPOI, bot.travelSrcBoatPos.x, bot.travelSrcBoatPos.y, bot.travelSrcBoatPos.z, destTownId, destPOI));
	} else {
		// No source boat — try recovery route to navigate back to a known town first
		if (findNearestRecoveryRoute(bot)) {
			return; // navigating via recovery route, will re-evaluate after
		}
		// Recovery failed — skip walking, go directly to NPC interaction phase (teleport)
		bot.travelSrcBoatPos = Position();
		bot.travelPhase = "at_boat";
		bot.travelWaitUntil = OTSYS_TIME() + uniform_random(3, 10) * 1000LL;
		castLog(bot, fmt::format("TRAVEL: No boat position for town {}, waiting to teleport to town {}",
			bot.townId, destTownId));
	}
}

void BotEngine::doTraveling(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// PZ-blocked: a pz-locked bot cannot board a boat (boat tile is a PZ). Abort travel and go
	// IDLE so the pz-aware reroll keeps it roaming non-PZ space until the 60s lock clears, rather
	// than hanging at the boat-NPC PZ until the 5-min timeout teleports it. No temple teleport
	// here — we just stop and let PZROAM take over.
	if (player->isPzLocked()) {
		castLog(bot, "PZROAM: pz-locked mid-travel — aborting travel, going IDLE");
		s_travelStartTime.erase(bot.guid); s_travelDestPOI.erase(bot.guid); s_travelSrcPOI.erase(bot.guid); s_travelArriveTarget.erase(bot.guid);
		s_lastRouteEndPos.erase(bot.guid); s_routeProgress.erase(bot.guid); s_travelFcRecoveryCount.erase(bot.guid);
		if (bot.huntScriptId > 0) {
			activeHunts_.erase(bot.huntScriptId);
			for (auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) { activeSpawnGroups_.erase(s.spawnGroup); break; }
			}
			bot.huntScriptId = 0;
			bot.pendingHuntAfterTravel = false;
		}
		bot.travelDestTownId = 0;
		bot.travelPhase.clear();
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		bot.state = BotAIState::IDLE;
		bot.hasWalkTarget = false;
		bot.nextRerollTime = OTSYS_TIME() + uniform_random(5, 15) * 1000;
		return;
	}

	// Global travel timeout: 15 minutes max in TRAVELING state
	static constexpr int64_t TRAVEL_TIMEOUT_MS = 300000;  // 5 min
	auto& travelStart = s_travelStartTime[bot.guid];
	if (travelStart == 0) travelStart = OTSYS_TIME();
	if (OTSYS_TIME() - travelStart > TRAVEL_TIMEOUT_MS) {
		castLogError(bot, fmt::format("TRAVEL: Global timeout ({}min), teleporting to temple and going IDLE",
			TRAVEL_TIMEOUT_MS / 60000));
		trackNavEvent("travel_timeout", bot, 0, "", bot.townId, "travel",
			fmt::format("dest_town={} phase={}", bot.travelDestTownId, bot.travelPhase));
		s_travelStartTime.erase(bot.guid); s_travelDestPOI.erase(bot.guid); s_travelSrcPOI.erase(bot.guid); s_lastRouteEndPos.erase(bot.guid);
		s_travelArriveTarget.erase(bot.guid);
		s_routeProgress.erase(bot.guid);
		s_travelFcRecoveryCount.erase(bot.guid);
		// Release hunt reservation if this travel was for a pending hunt
		if (bot.huntScriptId > 0) {
			activeHunts_.erase(bot.huntScriptId);
			for (auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
					activeSpawnGroups_.erase(s.spawnGroup);
					break;
				}
			}
			bot.huntScriptId = 0;
			bot.pendingHuntAfterTravel = false;
		}
		teleportToTemple(bot);
		bot.travelDestTownId = 0;
		bot.travelPhase.clear();
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		bot.state = BotAIState::IDLE;
		bot.hasWalkTarget = false;
		bot.nextRerollTime = OTSYS_TIME() + uniform_random(10, 30) * 1000;
		return;
	}

	// Phase 1: Walk to source boat NPC via city route
	if (bot.travelPhase == "walk_to_boat") {
		if (bot.travelSrcBoatPos.x == 0) {
			bot.travelPhase = "at_boat";
			bot.travelWaitUntil = OTSYS_TIME() + uniform_random(3, 10) * 1000LL;
			return;
		}

		// Follow city route FIRST — must complete ALL waypoints before boat check
		if (bot.followingCityRoute) {
			// Save last waypoint before followCityRoute clears them on completion
			Position lastRouteWp = bot.cityRouteWps.empty() ? Position() : bot.cityRouteWps.back().pos;
			bool following = followCityRoute(bot);
			if (following) return;
			castLog(bot, "TRAVEL: City route to boat complete");
			// Store last waypoint for proximity check (route wps were cleared by followCityRoute)
			if (lastRouteWp.x > 0) {
				s_lastRouteEndPos[bot.guid] = lastRouteWp;
			}
			bot.followingCityRoute = false;
			// Mark this route's source as tried (for smart fallback to next POI)
			auto srcIt = s_lastRouteSource.find(bot.guid);
			if (srcIt != s_lastRouteSource.end() && !srcIt->second.empty()) {
				bot.triedRouteSources.insert(srcIt->second);
				s_lastRouteSource.erase(srcIt);
			} else {
				// Fallback: guess source from current position
				std::string usedSrc = detectNearestPOI(bot.townId, bot.currentPos);
				if (!usedSrc.empty()) {
					bot.triedRouteSources.insert(usedSrc);
				}
			}
			// Fall through to proximity check
		}

		// Use last route waypoint for proximity check (route may end at different z than travelSrcBoatPos)
		Position boatTarget = bot.travelSrcBoatPos;
		auto endPosIt = s_lastRouteEndPos.find(bot.guid);
		if (endPosIt != s_lastRouteEndPos.end()) {
			boatTarget = endPosIt->second;
		} else if (!bot.cityRouteWps.empty()) {
			boatTarget = bot.cityRouteWps.back().pos;
		}

		// Check if arrived near boat NPC (only after route is done or no route active)
		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(boatTarget.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(boatTarget.y)));

		if (dist <= 3 && bot.currentPos.z == boatTarget.z) {
			// BOT_LIVENESS_PACK Phase C.6: walk_to_boat tile spread. If another
			// creature is already standing on the exact boat NPC tile, walk one
			// extra step to a nearby safe + unoccupied tile (radius=1 ring) before
			// saying "hi". Captain NPC isInTalkRange default is 4 tiles, so
			// radius=1 is well within range — "hi" still reaches the captain from
			// the spread tile. Reuses the kUnsafeWakeMask filter logic so we
			// reject FC / TELEPORT / DEPOT / MAGICFIELD / move-event tiles.
			// Only fires if bot is exactly on boatTarget (route stopped on the
			// STAND waypoint per commit 99863bf7c). Idempotent — once
			// travelSpreadTarget is set, subsequent ticks skip the scan and use
			// the already-picked tile.
			if (bot.travelSpreadTarget.x == 0 && bot.currentPos == boatTarget) {
				auto isBoatSpreadOccupied = [&](const Position& p) -> bool {
					auto t = g_game().map.getTile(p);
					if (!t) return true;
					if (t->hasFlag(TILESTATE_BLOCKSOLID)) return true;
					if (t->hasFlag(TILESTATE_FLOORCHANGE)) return true;
					if (t->hasFlag(TILESTATE_TELEPORT)) return true;
					if (t->hasFlag(TILESTATE_DEPOT)) return true;
					if (t->hasFlag(TILESTATE_MAGICFIELD)) return true;
					if (g_moveEvents().hasPosition(p)) return true;
					if (auto cs = t->getCreatures(); cs && !cs->empty()) return true;
					return false;
				};
				if (isBoatSpreadOccupied(boatTarget)) {
					// boatTarget itself crowded — scan 8 neighbors at radius 1.
					static const std::pair<int, int> kRing1[] = {
						{-1,-1}, {0,-1}, {1,-1},
						{-1, 0},         {1, 0},
						{-1, 1}, {0, 1}, {1, 1},
					};
					std::vector<Position> picks;
					for (const auto& [dx, dy] : kRing1) {
						Position p(
							static_cast<uint16_t>(static_cast<int32_t>(boatTarget.x) + dx),
							static_cast<uint16_t>(static_cast<int32_t>(boatTarget.y) + dy),
							boatTarget.z);
						if (!isBoatSpreadOccupied(p)) picks.push_back(p);
					}
					if (!picks.empty()) {
						bot.travelSpreadTarget = picks[uniform_random(0, static_cast<int32_t>(picks.size()) - 1)];
						castLog(bot, fmt::format("TRAVEL: Boat tile crowded, spread to ({},{},{})",
							bot.travelSpreadTarget.x, bot.travelSpreadTarget.y, bot.travelSpreadTarget.z));
					}
					// If picks empty: every adjacent tile is also occupied/unsafe.
					// Fall through and stack on boatTarget — Tibia allows stacking
					// in PZ via FLAG_IGNOREBLOCKCREATURE.
				}
			}

			// If we picked a spread target and haven't walked to it yet, walk to it now.
			if (bot.travelSpreadTarget.x != 0 && bot.currentPos != bot.travelSpreadTarget) {
				if (goTo(bot, bot.travelSpreadTarget, 0)) return; // still walking
				// Path failed — accept stacking and fall through.
				bot.travelSpreadTarget = Position();
			}

			// Clear any leftover route/recovery state
			s_travelFcRecoveryCount.erase(bot.guid);
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			g_game().internalCreatureSay(player, TALKTYPE_SAY, "hi", false);
			bot.travelPhase = "at_boat";
			bot.travelWaitUntil = OTSYS_TIME() + uniform_random(3, 10) * 1000LL;
			castLog(bot, fmt::format("TRAVEL: At boat NPC, saying hi, waiting {}s",
				(bot.travelWaitUntil - OTSYS_TIME()) / 1000));
			return;
		}

		// Try to start city route to "boat" — retry every 5s if failed
		if (!bot.followingCityRoute) {
			// 5-minute stuck safety valve
			auto& walkToBoatProgress = s_routeProgress[bot.guid];
			if (walkToBoatProgress.second == 0) {
				walkToBoatProgress = {SIZE_MAX, OTSYS_TIME()};
			}
			int64_t walkToBoatStuck = OTSYS_TIME() - walkToBoatProgress.second;
			if (walkToBoatStuck > 300000) {  // 5 minutes
				if ((bot.tickCounter % 600) == 0) {
					castLogError(bot, fmt::format("STUCK: No route to boat in {} for {:.0f}min — bot suspended",
						bot.townName, walkToBoatStuck / 60000.0));
					trackNavEvent("travel_no_route_boat", bot, 0, "", bot.townId, "travel_to_boat",
						fmt::format("stuck {:.0f}min", walkToBoatStuck / 60000.0));
				}
				return;
			}

			// PZ-locked: reset stuck timer — can't approach boat NPC in PZ
			if (isBotPzLocked(bot)) {
				walkToBoatProgress.second = OTSYS_TIME();
				if ((bot.tickCounter % 600) == 0) {
					castLog(bot, fmt::format("TRAVEL: PZ-locked, waiting for lock to expire before approaching boat ({:.0f}s left)",
						(PZ_LOCK_DURATION * 1000 - (OTSYS_TIME() - bot.lastPvpAttackTime)) / 1000.0));
				}
			}

			if (bot.travelWaitUntil == 0 || OTSYS_TIME() >= bot.travelWaitUntil) {
				// Use the source POI type chosen at travel start (boat or carpet)
				// Auto-detect source (picks closest POI — temple if at temple, depot if at depot)
				std::string srcPOI = s_travelSrcPOI.count(bot.guid) ? s_travelSrcPOI[bot.guid] : "boat";
				bool routeFound = startCityRoute(bot, "", srcPOI);
				if (!routeFound) routeFound = startCityRoute(bot, "", "temple");
				if (routeFound) {
					castLog(bot, fmt::format("TRAVEL: Following city route to {}", srcPOI));
					bot.travelWaitUntil = 0;
					s_routeProgress.erase(bot.guid);
				} else {
					if (bot.travelWaitUntil == 0) {
						castLog(bot, "TRAVEL: No city route to boat/carpet/temple, retrying in 5s");
					}
					bot.travelWaitUntil = OTSYS_TIME() + 5000;
				}
			}
		}
		return;
	}

	// Phase 2: At boat NPC, waiting before teleport
	if (bot.travelPhase == "at_boat") {
		if (OTSYS_TIME() < bot.travelWaitUntil) return;

		// Teleport to destination boat position
		if (bot.travelBoatPos.x > 0) {
			// Spiral only (route = nullptr): the boat position is DB-sourced with no route
			// context to rewind through, and docks are legitimately PZ.
			Position landing = safeTeleportLanding(bot, bot.travelBoatPos, nullptr, nullptr, "boatDock");
			BOT_TELEPORT(player, landing, true);
			bot.currentPos = landing;
			castLog(bot, fmt::format("TRAVEL: Teleported to destination boat at ({},{},{})",
				landing.x, landing.y, landing.z));
		}

		// Update bot's current town for navigation (do NOT change player->town — preserve DB town for death)
		auto town = g_game().map.towns.getTown(bot.travelDestTownId);
		if (town) {
			bot.townId = bot.travelDestTownId;
			auto nameIt = travelTownNames_.find(bot.travelDestTownId);
			bot.townName = nameIt != travelTownNames_.end() ? nameIt->second : town->getName();
		}

		// Clear any stale city route state from walk_to_boat
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		bot.pathFailCount = 0;

		bot.travelDestVerified = false;
		bot.travelPhase = "walk_from_boat";
		bot.travelWaitUntil = OTSYS_TIME() + 2000; // brief pause after teleport
		return;
	}

	// Phase 2.5: Bot woke up at the destination boat tile with travelPhase="teleported".
	// This phase only exists in the virtual simulator (virtualAdvanceTraveling line 2987-2993)
	// — live at_boat skips it and goes straight to walk_from_boat after the teleport. When a
	// hibernated bot's virtual sim has advanced at_boat → teleported and the player approaches
	// within the 2-5s "settling" window, the pre-wake hook materializes the bot here with
	// no live-AI handler. Bridge to walk_from_boat so the depot route can start.
	if (bot.travelPhase == "teleported") {
		if (OTSYS_TIME() < bot.travelWaitUntil) return;
		bot.travelDestVerified = false;
		bot.travelPhase = "walk_from_boat";
		bot.travelWaitUntil = OTSYS_TIME() + 2000;
		return;
	}

	// Phase 3: Walk from destination boat to depot via city route (no fallbacks)
	if (bot.travelPhase == "walk_from_boat") {
		if (OTSYS_TIME() < bot.travelWaitUntil) return;

		// STEP 1: Verify destination ONCE (before starting any route)
		if (!bot.travelDestVerified) {
			// Use the position we actually teleported to — don't re-randomize
		Position destBoat = bot.travelBoatPos;
			if (destBoat.x > 0) {
				int32_t distToDestBoat = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(destBoat.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(destBoat.y)));
				if (distToDestBoat > 10 || bot.currentPos.z != destBoat.z) {
					// NOT at destination — retry full origin city route
					castLog(bot, fmt::format(
						"TRAVEL: Not at destination boat (dist={}, z={}/{}), retrying origin route",
						distToDestBoat, bot.currentPos.z, destBoat.z));
					bot.followingCityRoute = false;
					bot.cityRouteWps.clear();
					bot.cityRouteIdx = 0;
					bot.travelPhase = "walk_to_boat";
					bot.travelWaitUntil = OTSYS_TIME() + 5000;
					return;
				}
			}
			bot.travelDestVerified = true;
			castLog(bot, "TRAVEL: Destination verified, starting boat->depot route");
		}

		// STEP 2: Follow city route boat->depot
		if (bot.followingCityRoute) {
			bool following = followCityRoute(bot);
			if (following) return;
			castLog(bot, "TRAVEL: City route to depot complete");
			bot.followingCityRoute = false;
			bot.travelPhase = "arrived";
			return;
		}

		// STEP 3: Try starting city route boat->depot (retry every 5s)
		// 5-minute stuck safety valve: if no route found after 5min, suspend pathfinding
		auto& travelProgress = s_routeProgress[bot.guid];
		if (travelProgress.second == 0) {
			travelProgress = {SIZE_MAX, OTSYS_TIME()};
		}
		int64_t travelStuckDuration = OTSYS_TIME() - travelProgress.second;
		if (travelStuckDuration > 300000) {  // 5 minutes
			if ((bot.tickCounter % 600) == 0) {
				castLogError(bot, fmt::format("STUCK: No route boat->depot in {} for {:.0f}min — bot suspended",
					bot.townName, travelStuckDuration / 60000.0));
			}
			return;  // Don't retry pathfinding
		}

		// PZ-locked: reset stuck timer — can't reach depot in PZ
		if (isBotPzLocked(bot)) {
			travelProgress.second = OTSYS_TIME();
			if ((bot.tickCounter % 600) == 0) {
				castLog(bot, fmt::format("TRAVEL: PZ-locked, waiting for lock to expire before depot ({:.0f}s left)",
					(PZ_LOCK_DURATION * 1000 - (OTSYS_TIME() - bot.lastPvpAttackTime)) / 1000.0));
			}
		}

		if (bot.travelWaitUntil == 0 || OTSYS_TIME() >= bot.travelWaitUntil) {
			// Use the POI type that was chosen when the travel was initiated (boat or carpet)
			std::string arrivalPOI = s_travelDestPOI.count(bot.guid) ? s_travelDestPOI[bot.guid] : "boat";
			// BOT_TRAVEL_ARRIVE_MIX. The rolled destination is tried FIRST; everything below it is
			// the pre-existing chain, unchanged. That ordering is what makes this safe: the new
			// attempt is strictly prepended, and depot resolves in all 18 towns, so "a bot always
			// arrives somewhere" still holds by construction.
			const std::string target = s_travelArriveTarget.count(bot.guid)
				? s_travelArriveTarget[bot.guid] : std::string("depot");
			bool routeFound = false;
			if (target != "depot") {
				routeFound = startCityRoute(bot, arrivalPOI, target);
				if (!routeFound) {
					routeFound = startCityRoute(bot, "", target);
				}
				if (routeFound) {
					noteTravelArrivalClass(target);
					castLog(bot, fmt::format("TRAVEL: arriving at '{}' (rolled) via {}", target, arrivalPOI));
				} else {
					// Nothing to resolve to — the 9 shop-less towns land here. Fall through to the
					// depot chain and count it, so the telemetry can show how much of the
					// configured split the authored data is actually able to honour.
					s_arriveFallbackCount++;
					castLog(bot, fmt::format("TRAVEL: no route to rolled '{}' — falling back to depot", target));
				}
			}
			if (!routeFound) routeFound = startCityRoute(bot, arrivalPOI, "depot");
			if (!routeFound) routeFound = startCityRoute(bot, arrivalPOI, "temple");
			if (!routeFound) routeFound = startCityRoute(bot, "", "depot");
			if (!routeFound) routeFound = startCityRoute(bot, "", "temple");
			if (routeFound) {
				if (target == "depot") {
					noteTravelArrivalClass(target);
				}
				castLog(bot, fmt::format("TRAVEL: Following {} route to depot/temple", arrivalPOI));
				bot.travelWaitUntil = 0;
				s_routeProgress.erase(bot.guid);
			} else {
				if (bot.travelWaitUntil == 0) {
					castLog(bot, "TRAVEL: No city route to depot/temple, retrying in 5s");
				}
				bot.travelWaitUntil = OTSYS_TIME() + 5000;
			}
		}
		return;
	}

	// Phase 4: Arrived at destination
	if (bot.travelPhase == "arrived") {
		s_routeProgress.erase(bot.guid);
		// Captured BEFORE the erases below — the depot-locker block at the end of this handler
		// needs to know what this journey was heading for, and by then the entry is gone.
		const bool arrivedAtDepot = !s_travelArriveTarget.count(bot.guid)
			|| s_travelArriveTarget[bot.guid] == "depot";
		s_travelStartTime.erase(bot.guid); s_travelDestPOI.erase(bot.guid); s_travelSrcPOI.erase(bot.guid); s_lastRouteEndPos.erase(bot.guid);
		s_travelArriveTarget.erase(bot.guid);
		s_travelFcRecoveryCount.erase(bot.guid);
		castLog(bot, fmt::format("TRAVEL: Arrived at town {}", bot.townId));

		// If pending hunt, start it
		if (bot.pendingHuntAfterTravel && bot.huntScriptId > 0) {
			bot.pendingHuntAfterTravel = false;
			bot.travelPhase.clear();
			bot.travelDestTownId = 0;
			beginHuntPhase(bot, HuntPhase::PREPARING);
			return;
		}

		// Clear travel state — transition to IDLE (no dwell)
		bot.travelPhase.clear();
		bot.travelDestTownId = 0;
		bot.hasWalkTarget = false;
		bot.currentPOI = nullptr;

		// Clear stale depot state from previous town
		s_depotLockerRerollTime.erase(bot.guid);
		s_depotDwellWalkTarget.erase(bot.guid);
		s_depotDwellWalkFails.erase(bot.guid);
		bot.hasDepotTarget = false;
		clearDepotBlacklist(bot.guid);

		// Find depot locker to walk to (handled by doIdle depot-walk logic).
		// BOT_TRAVEL_ARRIVE_MIX: only when this journey was actually heading for the depot.
		// findReachableDepotLocker scans +-11 tiles, so after a temple or shop arrival it usually
		// finds nothing and degrades to plain IDLE anyway — but banks and shops frequently sit
		// within 11 tiles of a depot, and without this guard those arrivals would immediately
		// beeline to the locker, silently cancelling the roll that sent them there.
		Position lockerPos = arrivedAtDepot ? findReachableDepotLocker(bot) : Position();
		if (lockerPos.x > 0) {
			bot.hasDepotTarget = true;
			bot.idleDepotTarget = lockerPos;
			s_depotWalkRetries[bot.guid] = 0;
			castLog(bot, fmt::format("TRAVEL: Walking to depot locker at ({},{},{})",
				lockerPos.x, lockerPos.y, lockerPos.z));
		}
		bot.state = BotAIState::IDLE;
		bot.nextRerollTime = OTSYS_TIME() + g_configManager().getNumber(BOT_DWELL_POST_TRAVEL_SEC) * 1000;
	}
}

