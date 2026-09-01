/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_waypoint.cpp — unified waypoint following - city routes, travel, leaving, patrol
//
// BOT_NAV_REALISM Phase 11 module split. Compiles into the SAME libbot_engine.so
// as bot_engine.cpp, so /cavebot reload is unchanged. Shared includes, engine-local
// types and the BotEngine class declaration all live in bot_engine_impl.hpp.
//
// Carved out only after tools/botnavsim/module_promote.py reported zero external
// dependencies for this range.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#include "map/house/house.hpp"   // House::getId — shrine hits are tagged with their house

// ============================================================================
// Unified waypoint-following function — used by city routes, travel, leaving, patrol
// ============================================================================

BotEngine::WaypointFollowResult BotEngine::followWaypoints(
	BotState& bot,
	const std::vector<Waypoint>& waypoints,
	size_t& waypointIdx,
	uint32_t& skipCount,
	const WaypointFollowConfig& config)
{
	WaypointFollowResult result;
	if (waypoints.empty()) { result.inProgress = false; return result; }

	auto player = bot.getPlayer();
	if (!player) { result.inProgress = false; return result; }

	// Global stuck timeout: if no waypoint progress for globalTimeoutMs, abort
	if (config.globalTimeoutMs > 0) {
		auto& progress = s_routeProgress[bot.guid];
		if (progress.second == 0 || progress.first != waypointIdx) {
			progress = {waypointIdx, OTSYS_TIME()};
		}
		int64_t noProgressDuration = OTSYS_TIME() - progress.second;
		if (noProgressDuration > config.globalTimeoutMs) {
			castLogError(bot, fmt::format("{}: No progress for {:.0f}min (wp {}/{}) — aborting",
				config.logPrefix, noProgressDuration / 60000.0, waypointIdx + 1, waypoints.size()));
			trackNavEvent("route_stuck", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
				bot.townId, "waypoint",
				fmt::format("wp {}/{} stuck {:.0f}min", waypointIdx + 1, waypoints.size(), noProgressDuration / 60000.0));
			s_routeProgress.erase(bot.guid);
			s_routeWpTimer.erase(bot.guid);
			waypointIdx = waypoints.size();
			result.inProgress = false;
			result.aborted = true;
			return result;
		}
	}

	// Z-change grace: after a recent z-change, suppress navigation/stuck detection.
	// BUT allow walkOnFc arrival detection — the critical evidence (lastPos.z == wp.z)
	// only exists on the FIRST tick after z-change. If we suppress that tick, the arrival
	// can never fire (lastPos gets updated to new z on subsequent ticks).
	if (config.zChangeGraceMs > 0) {
		auto zGraceIt = s_lastZChangeTime.find(bot.guid);
		if (zGraceIt != s_lastZChangeTime.end() &&
			OTSYS_TIME() - zGraceIt->second < config.zChangeGraceMs) {
			// During grace: check walkOnFc arrival for current waypoint
			if (waypointIdx < waypoints.size() && waypoints[waypointIdx].isWalkOnFc) {
				auto& graceWp = waypoints[waypointIdx];
				auto& gwp = graceWp.pos;
				int32_t lastDx = std::abs(static_cast<int32_t>(bot.lastPos.x) - static_cast<int32_t>(gwp.x));
				int32_t lastDy = std::abs(static_cast<int32_t>(bot.lastPos.y) - static_cast<int32_t>(gwp.y));
				bool lastTickAtOrNearWp = (lastDx <= 1 && lastDy <= 1 && bot.lastPos.z == gwp.z);
				castLog(bot, fmt::format("{}: GRACE-FC wp {}/{} ({},{},{}) walkOnFc=1 lastPos=({},{},{}) curPos=({},{},{}) nearWp={} zDiff={}",
					config.logPrefix, waypointIdx + 1, waypoints.size(), gwp.x, gwp.y, gwp.z,
					bot.lastPos.x, bot.lastPos.y, bot.lastPos.z,
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					lastTickAtOrNearWp ? 1 : 0, bot.currentPos.z != gwp.z ? 1 : 0));
				if (lastTickAtOrNearWp && bot.currentPos.z != gwp.z) {
					// Bot just stepped on stair/FC tile — this IS the arrival
					auto gracePlayer = bot.getPlayer();
					if (gracePlayer && !gracePlayer->listWalkDir.empty()) {
						gracePlayer->listWalkDir.clear();
						gracePlayer->stopEventWalk();
					}
					handleActionWaypoint(bot, graceWp);
					if (graceWp.type == WaypointType::LEVITATE_UP || graceWp.type == WaypointType::LEVITATE_DOWN) {
						castLevitateSpell(bot, graceWp);
					}
					castLog(bot, fmt::format("{}: Reached {} wp {}/{} ({},{},{})",
						config.logPrefix, waypointTypeName(graceWp.type), waypointIdx + 1, waypoints.size(),
						gwp.x, gwp.y, gwp.z));
					waypointIdx++;
					skipCount = 0;
					bot.pathFailCount = 0;
					result.advanced = true;
				}
			}
			return result; // grace: suppress navigation/stuck detection
		}
	}

	// Action waypoint pause: 500ms delay after USE/DOOR/LADDER/MACHETE/etc., or the longer
	// greet-and-wait an NPC_INTERACT sets from handleActionWaypoint. Member, not a local static —
	// see bot_engine_impl.hpp.
	{
		auto pauseIt = s_actionWpPauseUntil.find(bot.guid);
		if (pauseIt != s_actionWpPauseUntil.end()) {
			if (OTSYS_TIME() < pauseIt->second) {
				return result; // still pausing after action waypoint
			}
			s_actionWpPauseUntil.erase(pauseIt);
		}
	}

	// Ice-fishing hold. A PURE QUERY on purpose: the session is driven from the per-bot supply
	// slot in bot_tick.cpp, which runs in every awake state, while this gate only stops the bot
	// from walking off the hole it is working. Sits here rather than in doHuntPatrol so the
	// marker holds in patrol, travel_to, travel_from and city routes alike. Every caller treats a
	// default-constructed result (inProgress=true, advanced=false) as "still walking, wait" —
	// see doHuntTravel/doHuntLeaving/followCityRoute.
	if (isIceFishing(bot.guid)) {
		return result;
	}

	// Distance sanity: abort if bot is >200 tiles from current waypoint (wrong city/route).
	// EXEMPT TELEPORT-type waypoints — they are explicit cheat-teleports designed to fire
	// from arbitrary distance (cross-continent NPC teleports the bot cannot replicate on
	// foot). The while-loop below handles them via internalTeleport regardless of distance.
	if (waypointIdx < waypoints.size() &&
		waypoints[waypointIdx].type != WaypointType::TELEPORT) {
		auto& sanityWp = waypoints[waypointIdx].pos;
		int32_t sanityDist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(sanityWp.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(sanityWp.y)));
		if (sanityDist > 200) {
			// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 5. A route can carry its own bridge
			// across the gap that trips this guard: script 1403's travel_to is
			// [0]=node(840 tiles away), [1]=teleport synth_bridge. Aborting throws away the
			// one waypoint designed to fix the problem.
			//
			// Head of route only (waypointIdx == 0). That is the case that actually fires — a
			// bot starting a route in the wrong town — and it makes "skipping past authored
			// content" structurally impossible, because there is no content behind index 0.
			// Lookahead 4: head-of-route bridges sit at seq 1-3 by construction.
			bool skipped = false;
			if (waypointIdx == 0) {
				const size_t lookahead = std::min<size_t>(4, waypoints.size());
				for (size_t i = 1; i < lookahead; i++) {
					if (waypoints[i].type != WaypointType::TELEPORT) continue;
					// Bridge-coherence: the route must genuinely continue from where the
					// teleport lands. A malformed bridge falls through to today's abort.
					if (i + 1 >= waypoints.size()) break;
					int32_t contDist = std::max(
						std::abs(static_cast<int32_t>(waypoints[i + 1].pos.x) - static_cast<int32_t>(waypoints[i].pos.x)),
						std::abs(static_cast<int32_t>(waypoints[i + 1].pos.y) - static_cast<int32_t>(waypoints[i].pos.y)));
					if (contDist > 200) break;
					castLog(bot, fmt::format("{}: SKIP-TO-TP wp {}/{} — {} tiles from wp 1, bridging via ({},{},{})",
						config.logPrefix, i + 1, waypoints.size(), sanityDist,
						waypoints[i].pos.x, waypoints[i].pos.y, waypoints[i].pos.z));
					trackNavEvent("route_skip_to_tp", bot, bot.huntScriptId, getHuntScriptName(bot, huntScripts_),
						bot.townId, config.logPrefix, fmt::format("dist={} tpIdx={}", sanityDist, i));
					waypointIdx = i;
					skipped = true;
					break;
				}
			}
			if (!skipped) {
				castLogError(bot, fmt::format("{}: Aborting route — {} tiles from wp {}/{} at ({},{},{})",
					config.logPrefix, sanityDist, waypointIdx + 1, waypoints.size(),
					sanityWp.x, sanityWp.y, sanityWp.z));
				waypointIdx = waypoints.size();
				result.inProgress = false;
				result.aborted = true;
				return result;
			}
		}
	}

	// NOTE: Do NOT early-return when listWalkDir is non-empty here.
	// We check arrival FIRST so the bot can seamlessly transition between waypoints.

	while (waypointIdx < waypoints.size()) {
		auto& waypoint = waypoints[waypointIdx];
		auto& wp = waypoint.pos;

		// Route-declared shrine return town. Stamped as soon as the waypoint becomes current —
		// i.e. BEFORE the bot steps on the flame, which is the only ordering that works, since
		// shrine_exit.lua reads the storage inside the step-on MoveEvent. Idempotent, so this is
		// safe to evaluate every tick, and it deliberately OVERRIDES the home-town fallback the
		// wake/teleport-jump hooks stamp: the route knows the answer, the bot doesn't.
		if (!waypoint.extraData.empty()) {
			botStampShrineReturnMarker(player, waypoint.extraData, "route-marker");
		}

		// TELEPORT waypoints fire immediately — no walking, no arrival check.
		// Used for cross-continent NPC teleports the bot cannot replicate on foot.
		if (waypoint.type == WaypointType::TELEPORT) {
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}
			// route = nullptr on purpose: never rewind at a TELEPORT waypoint. A synth bridge
			// exists precisely because the gap is unwalkable, so rewinding to the near side
			// would leave the index behind the bot and either strand it or loop it back
			// through this same teleport. Spiral-near-destination is the only correct repair,
			// and it is sufficient — wp[i] and wp[i+1] share a position for a synth bridge.
			Position landing = safeTeleportLanding(bot, wp, nullptr, nullptr, "teleportWp");
			BOT_TELEPORT(player, landing, true);
			bot.currentPos = landing;
			bot.lastPos = landing;
			castLog(bot, fmt::format("{}: TELEPORT wp {}/{} → ({},{},{})",
				config.logPrefix, waypointIdx + 1, waypoints.size(), landing.x, landing.y, landing.z));
			waypointIdx++;

			// The importer emits `teleport(P)` followed by `stand(P)` — the same tile — for 196
			// waypoint pairs across 56 scripts. Normally harmless: the bot is already standing
			// there, so the stand arrives instantly and advances.
			//
			// It stops being harmless when P is a FLOOR-CHANGE tile. safeTeleportLanding refuses to
			// drop the bot on one (it would trigger an involuntary z-change) and spirals it a few
			// tiles off — and then this stand, with arrivalDist 0, walks it straight back onto the
			// stair it was just moved off. The bot falls a floor and the route derails.
			//
			// Narrow on purpose: FLOORCHANGE only, NOT every relocation. isUnsafeWakeTile also
			// rejects TILESTATE_TELEPORT and MoveEvent tiles, and those are exactly what a
			// bidirectional mystic-flame hub looks like — a route that teleports to a hub and then
			// steps onto it is doing that deliberately, and skipping the stand there would strand
			// the bot instead of saving it.
			if (landing != wp && waypointIdx < waypoints.size()) {
				const auto& nextWp = waypoints[waypointIdx];
				if (nextWp.pos == wp && nextWp.type == WaypointType::STAND) {
					auto desiredTile = g_game().map.getTile(wp);
					if (desiredTile && desiredTile->hasFlag(TILESTATE_FLOORCHANGE)) {
						castLog(bot, fmt::format(
							"{}: TPSAFE skipped duplicate stand wp {}/{} on floor-change tile ({},{},{}) — landed at ({},{},{})",
							config.logPrefix, waypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z,
							landing.x, landing.y, landing.z));
						trackNavEvent("tp_dup_stand_skipped", bot, bot.huntScriptId,
							getHuntScriptName(bot, huntScripts_), bot.townId, config.logPrefix,
							fmt::format("wp={} pos={},{},{}", waypointIdx + 1, wp.x, wp.y, wp.z));
						waypointIdx++;
					}
				}
			}
			skipCount = 0;
			bot.pathFailCount = 0;
			result.advanced = true;
			s_actionWpPauseUntil[bot.guid] = OTSYS_TIME() + 500;
			return result;
		}

		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wp.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wp.y)));

		bool walkOnFc = waypoint.isWalkOnFc;

		// Type-based arrival distance
		int32_t arrivalDist;
		if (walkOnFc) {
			arrivalDist = 3;
		} else if (waypoint.type == WaypointType::USE_WITH || waypoint.type == WaypointType::MACHETE ||
				   waypoint.type == WaypointType::LADDER || waypoint.type == WaypointType::ROPE ||
				   waypoint.type == WaypointType::DOOR) {
			arrivalDist = 1;
		} else if (waypoint.type == WaypointType::STAND || waypoint.type == WaypointType::HOLE ||
				   waypoint.type == WaypointType::STAIRS_UP || waypoint.type == WaypointType::STAIRS_DOWN) {
			arrivalDist = 0;
		} else if (waypoint.type == WaypointType::NPC_INTERACT) {
			// NODE-like: walk up to the NPC, THEN greet. Was 3 (deliberately, since 866ef825c,
			// reasoning from Canary's talk range of 4) — but that disagreed with the navigation
			// target below, which aimed at the exact tile, so the bot routed to the NPC and
			// declared arrival three tiles short. See NPC_APPROACH_GRACE_MS for the fallback that
			// keeps the old <=3 behavior available for waypoints the bot genuinely cannot reach.
			arrivalDist = 1;
		} else {
			arrivalDist = (waypoint.type == WaypointType::NODE) ? 1 : 0;
		}

		// Z-check: walk-on FC arrival requires bot to have just walked onto the FC tile
		// (bot.lastPos was on/adjacent to wp at same z, and now bot.currentPos.z differs).
		// Without this, any STAND/NODE waypoint on a stair tile falsely "arrives" from
		// anywhere on the map whenever bot.z != wp.z.
		bool arrived;
		if (walkOnFc) {
			int32_t lastDx = std::abs(static_cast<int32_t>(bot.lastPos.x) - static_cast<int32_t>(wp.x));
			int32_t lastDy = std::abs(static_cast<int32_t>(bot.lastPos.y) - static_cast<int32_t>(wp.y));
			bool lastTickAtOrNearWp = (lastDx <= 1 && lastDy <= 1 && bot.lastPos.z == wp.z);
			arrived = lastTickAtOrNearWp && (bot.currentPos.z != wp.z);
		} else {
			arrived = (dist <= arrivalDist && bot.currentPos.z == wp.z);
		}

		// Lua MoveEvent teleport step-on: bot was exactly on the wp last tick and is now
		// >arrivalDist away. The global posDiff>10 jump detector misses small teleports
		// (e.g. Lion's Rock entrance: 6-tile jump) and Lua MoveEvent tiles lack
		// TILESTATE_TELEPORT, so isWalkOnFcTile returns false. This rule catches both.
		if (!arrived) {
			bool teleportedFromWp = (bot.lastPos.x == wp.x && bot.lastPos.y == wp.y
									&& bot.lastPos.z == wp.z) && (dist > arrivalDist);
			if (teleportedFromWp) arrived = true;
		}

		// NPC_INTERACT approach grace. Tightening arrival to 1 means a waypoint the bot cannot
		// actually stand next to — authored across a wall, or on a non-PZ tile an NPC is standing
		// on, where canWalkthrough(npc) is false — would now burn the 30s per-wp stuck timeout and
		// be SKIPPED, losing the greet entirely. Settle for the old <=3 behavior after a grace
		// period so the worst case is "same as before, 8s later" rather than a lost interaction.
		//
		// The clock is keyed by the waypoint POSITION (see s_npcApproachStart) so it cannot
		// inherit a stale timestamp across a hunt-phase transition and fire before the bot has
		// walked at all.
		if (!arrived && waypoint.type == WaypointType::NPC_INTERACT) {
			const uint64_t wpKey = packPosU64(wp);
			auto& approach = s_npcApproachStart[bot.guid];
			if (approach.first != wpKey) {
				approach = {wpKey, OTSYS_TIME()};
			} else if (dist <= 3 && bot.currentPos.z == wp.z
			           && OTSYS_TIME() - approach.second > NPC_APPROACH_GRACE_MS) {
				castLog(bot, fmt::format(
					"{}: NPC wp {}/{} unreachable after {}s — greeting from {} tiles ({},{},{})",
					config.logPrefix, waypointIdx + 1, waypoints.size(),
					NPC_APPROACH_GRACE_MS / 1000, dist, wp.x, wp.y, wp.z));
				arrived = true;
			}
		}

		if (arrived) {
			// Reached waypoint — clear walk queue for seamless transition
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}

			// Handle action waypoints (use_with, machete, npc_interact, ladder, levitate) and the
			// extra_data markers. didAction is load-bearing — see the pause gate below.
			const bool didAction = handleActionWaypoint(bot, waypoint);

			// Levitate: cast spell on arrival
			if (waypoint.type == WaypointType::LEVITATE_UP || waypoint.type == WaypointType::LEVITATE_DOWN) {
				castLevitateSpell(bot, waypoint);
			}

			castLog(bot, fmt::format("{}: Reached {} wp {}/{} ({},{},{})",
				config.logPrefix, waypointTypeName(waypoint.type), waypointIdx + 1, waypoints.size(),
				wp.x, wp.y, wp.z));
			if (waypoint.type == WaypointType::NPC_INTERACT) {
				s_npcApproachStart.erase(bot.guid);
			}
			waypointIdx++;
			skipCount = 0;
			bot.pathFailCount = 0;
			result.advanced = true;

			// ---- BOT_NAV_REALISM Phase 10: human-jitter pack ----
			// Town/travel flavor only — never during hunt-combat, party, PvP or a floor
			// change (same state allowlist as the observed walk-pause). Both behaviors are
			// config-gated and default off; they reuse the action-waypoint pause gate above.
			if ((bot.state == BotAIState::IDLE || bot.state == BotAIState::DWELLING
			     || bot.state == BotAIState::TRAVELING)
			    && bot.attackerId == 0 && bot.huntTargetId == 0
			    && bot.fcState == FloorChangeState::NONE
			    && (waypoint.type == WaypointType::NODE || waypoint.type == WaypointType::STAND)) {

				// (a) U-turn "forgot something": walk back a few waypoints, pause, resume.
				// Guards: per-bot 10-min cooldown (no chain-loops), needs >=8 waypoints of
				// history, and every rewound waypoint must share the bot's current z so the
				// walk-back can never cross a floor-change leg.
				const int32_t uturnPct = g_configManager().getNumber(BOT_JITTER_UTURN_PCT);
				static std::unordered_map<uint32_t, int64_t> s_lastUturnMs;
				if (uturnPct > 0 && waypointIdx >= 8 && uniform_random(1, 100) <= uturnPct) {
					const int64_t nowMs = OTSYS_TIME();
					auto uIt = s_lastUturnMs.find(bot.guid);
					if (uIt == s_lastUturnMs.end() || nowMs - uIt->second > 600000) {
						const size_t back = static_cast<size_t>(uniform_random(3, 8));
						const size_t target = waypointIdx - back;
						bool sameZ = true;
						for (size_t i = target; i < waypointIdx && i < waypoints.size(); i++) {
							if (waypoints[i].pos.z != bot.currentPos.z) { sameZ = false; break; }
						}
						if (sameZ) {
							s_lastUturnMs[bot.guid] = nowMs;
							waypointIdx = target;
							s_actionWpPauseUntil[bot.guid] = nowMs + uniform_random(2000, 5000);
							s_jitterUturnCount++;
							castLog(bot, fmt::format("{}: [JITTER] u-turn — walking back {} wps (forgot something)",
								config.logPrefix, back));
							return result;
						}
					}
				}

				// (c) Mid-route destination change ("changed my mind"). CITY ROUTES ONLY —
				// aborting a travel leg would strand the bot mid-journey, and hunt phases have
				// their own state machine. Fires once per bot per 10 min, and only in the MIDDLE
				// THIRD of the route so it reads as a change of mind rather than a stutter at the
				// start or a failure at the destination. Clearing is delegated to the caller,
				// which already tears the route down on !inProgress; doIdle then picks a fresh POI.
				//
				// The margin is a FRACTION of route length, not a fixed waypoint count: measured
				// against live data, city routes average 12.1 waypoints and only 3 of 1831 have
				// >=40, so an absolute ">=20 from both ends" guard could never fire.
				const int32_t rerollPct = g_configManager().getNumber(BOT_JITTER_REROLL_PCT);
				const size_t routeLen = waypoints.size();
				const size_t rerollMargin = std::max<size_t>(2, routeLen / 3);
				static std::unordered_map<uint32_t, int64_t> s_lastMidRouteRerollMs;
				if (bot.followingCityRoute && rerollPct > 0 && routeLen >= 6
				    && waypointIdx >= rerollMargin && (routeLen - waypointIdx) >= rerollMargin
				    && uniform_random(1, 100) <= rerollPct) {
					const int64_t nowMs = OTSYS_TIME();
					auto rIt = s_lastMidRouteRerollMs.find(bot.guid);
					if (rIt == s_lastMidRouteRerollMs.end() || nowMs - rIt->second > 600000) {
						s_lastMidRouteRerollMs[bot.guid] = nowMs;
						s_jitterRerollCount++;
						// Brief pause before re-deciding, so the bot visibly hesitates instead of
						// pivoting mid-stride.
						bot.nextRerollTime = nowMs + uniform_random(1200, 3000);
						castLog(bot, fmt::format("{}: [JITTER] changed destination mid-route at wp {}/{}",
							config.logPrefix, waypointIdx, routeLen));
						result.inProgress = false;
						result.aborted = true;
						return result;
					}
				}

				// (b) Dwell + look around: stop briefly and turn in place, like a player
				// getting their bearings mid-walk.
				const int32_t dwellPct = g_configManager().getNumber(BOT_JITTER_DWELL_PCT);
				if (dwellPct > 0 && uniform_random(1, 100) <= dwellPct) {
					int32_t dMin = g_configManager().getNumber(BOT_JITTER_DWELL_MIN_MS);
					int32_t dMax = g_configManager().getNumber(BOT_JITTER_DWELL_MAX_MS);
					if (dMax < dMin) dMax = dMin;
					const int32_t dwellMs = uniform_random(dMin, dMax);
					s_actionWpPauseUntil[bot.guid] = OTSYS_TIME() + dwellMs;
					static const Direction kJitterTurnDirs[4] = {
						DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST
					};
					g_game().internalCreatureTurn(player, kJitterTurnDirs[uniform_random(0, 3)]);
					s_jitterDwellCount++;
					castLog(bot, fmt::format("{}: [JITTER] dwell {}ms + look around", config.logPrefix, dwellMs));
					return result;
				}
			}

			// Pause 500ms after action waypoints (server needs time to process).
			//
			// `didAction ||` is NOT cosmetic. NODE/STAND deliberately fall through to the
			// `continue` below, which re-enters the loop with the ALREADY-INCREMENTED index on
			// this same tick and navigates toward the next waypoint. An extra_data marker
			// (`fish:`/`tool:`) can sit on a STAND — script 28's 25 ice-hole stands do — and
			// without this the bot would start walking away on the very tick it began working the
			// tile, then the session's own adjacency guard would tear it down having done nothing.
			if (didAction || (waypoint.type != WaypointType::NODE && waypoint.type != WaypointType::STAND)) {
				// std::max, NOT a plain assign. handleActionWaypoint may have just set a LONGER
				// hold on this same tick — NPC_INTERACT's 3-10s greet-and-wait does exactly that —
				// and overwriting it with 500ms silently reduces that feature to a no-op.
				auto& pauseUntil = s_actionWpPauseUntil[bot.guid];
				pauseUntil = std::max(pauseUntil, OTSYS_TIME() + 500);
				return result;
			}
			if (waypointIdx < waypoints.size() && waypoints[waypointIdx].pos.z != wp.z) {
				return result;
			}
			continue;
		}

		// NOT arrived yet

		// Look-ahead skip (patrol only): if bot drifted past current waypoint during combat
		if (config.enableLookaheadSkip) {
			int32_t bestSkipIdx = -1;
			size_t scanLimit = std::min(waypointIdx + static_cast<size_t>(PATROL_LOOKAHEAD_MAX), waypoints.size());
			for (size_t i = waypointIdx + 1; i < scanLimit; i++) {
				auto& futureWp = waypoints[i];
				if (isFloorChangeType(futureWp.type)) break;
				// Never skip PAST an NPC stop. The scan only rejects floor-change types, so a bot
				// that drifted within PATROL_LOOKAHEAD_DIST of a later waypoint could jump over an
				// NPC_INTERACT — handleActionWaypoint is only called on arrival, so the greet (and
				// whatever the NPC does in response) was silently lost. Pre-existing; it matters
				// more now that these waypoints are a real stop rather than a drive-by.
				if (futureWp.type == WaypointType::NPC_INTERACT) break;
				if (i + 1 < waypoints.size()) {
					int32_t dz = std::abs(static_cast<int32_t>(waypoints[i + 1].pos.z) - static_cast<int32_t>(futureWp.pos.z));
					if (dz > 0) break;
				}
				if (futureWp.pos.z != bot.currentPos.z) break;
				if (isAtPosition(bot.currentPos, futureWp.pos, PATROL_LOOKAHEAD_DIST)) {
					bestSkipIdx = static_cast<int32_t>(i);
				}
			}
			if (bestSkipIdx >= 0) {
				int32_t skipped = bestSkipIdx - static_cast<int32_t>(waypointIdx);
				castLog(bot, fmt::format("{}: Look-ahead skip {} wp(s) ({}->{}/{}) — bot at ({},{},{})",
					config.logPrefix, skipped, waypointIdx + 1, bestSkipIdx + 1, waypoints.size(),
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
				waypointIdx = static_cast<size_t>(bestSkipIdx);
				skipCount = 0;
				result.advanced = true;
				return result;
			}
		}

		// Z-mismatch handling
		if (bot.currentPos.z != wp.z) {
			// WalkOnFc waypoints at different z: the stair/FC tile exists at (wp.x, wp.y) on
			// the bot's z-level too. Navigate there using goTo (maxDist=0 triggers allowFc),
			// bot steps on the FC tile, server changes z, grace-based walkOnFc arrival fires.
			if (walkOnFc) {
				Position sameZTarget(wp.x, wp.y, bot.currentPos.z);
				auto sameZTile = g_game().map.getTile(sameZTarget);
				bool hasFc = sameZTile && (sameZTile->hasFlag(TILESTATE_FLOORCHANGE) || sameZTile->hasFlag(TILESTATE_TELEPORT));
				// When already walking to the FC tile, return silently — no log spam every tick.
				// Only log on the first attempt (walking=0) when we actually initiate the goTo.
				if (hasFc && !player->listWalkDir.empty()) return result;
				// Pre-check (applies for hasFc=0 too): if the NEXT waypoint's z already matches
				// bot.currentPos.z, the bot has completed this z-transition. Two scenarios:
				//   (a) hasFc=1: bot drifted to destination z via a nearby FC tile during combat —
				//       goTo(sameZTarget) would route through the REVERSE staircase and undo it.
				//   (b) hasFc=0: bot stepped on the hole AT (wp.x,wp.y,wp.z) and landed at
				//       (wp.x,wp.y,wp.z±1); the landing tile has no FC flag. Grace-FC arrival
				//       missed it because lastPos was 2+ tiles back (multi-step walk in one tick).
				// In both cases the transition is done — advance directly.
				size_t nextIdx = waypointIdx + 1;
				if (nextIdx < waypoints.size() && waypoints[nextIdx].pos.z == bot.currentPos.z) {
					if (!player->listWalkDir.empty()) {
						player->listWalkDir.clear();
						player->stopEventWalk();
					}
					castLog(bot, fmt::format("{}: Z-REDIRECT wp {}/{}: bot already at destination z={} (next wp z={} matches), advancing",
						config.logPrefix, waypointIdx + 1, waypoints.size(),
						bot.currentPos.z, waypoints[nextIdx].pos.z));
					waypointIdx++;
					skipCount = 0;
					bot.pathFailCount = 0;
					return result;
				}
				castLog(bot, fmt::format("{}: Z-REDIRECT wp {}/{} ({},{},{}) -> sameZ ({},{},{}) hasFc={}",
					config.logPrefix, waypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z,
					sameZTarget.x, sameZTarget.y, sameZTarget.z, hasFc ? 1 : 0));
				if (hasFc) {
					if (goTo(bot, sameZTarget, 0)) {
						bot.pathFailCount = 0;
					} else {
						// goTo failed (FC tile unreachable from current position).
						// Count failures so we eventually skip this waypoint and let the
						// teleport-to-start fallback handle the z-transition.
						bot.pathFailCount++;
						if (bot.pathFailCount >= 50) {
							castLogError(bot, fmt::format("{}: Skipping unreachable wp {}/{} at ({},{},{}) (Z-REDIRECT pathfails={} FC target ({},{},{}) unreachable)",
								config.logPrefix, waypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z,
								bot.pathFailCount, sameZTarget.x, sameZTarget.y, sameZTarget.z));
							waypointIdx++;
							skipCount = 0;
							bot.pathFailCount = 0;
						}
					}
					return result;
				}
			}
			// Non-walkOnFc or no FC tile on bot's z: skip after repeated failures
			skipCount++;
			if (skipCount >= 10) {
				castLogError(bot, fmt::format("{}: Skipping unreachable wp {}/{} at ({},{},{}) (z mismatch: bot z={})",
					config.logPrefix, waypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z, bot.currentPos.z));
				waypointIdx++;
				skipCount = 0;
				continue;
			}
			return result;
		}

		// If still walking toward this waypoint, wait
		if (!player->listWalkDir.empty()) return result;

		// Per-waypoint time-based stuck detection
		auto& wpTimer = s_routeWpTimer[bot.guid];
		if (wpTimer.second == 0 || wpTimer.first != waypointIdx) {
			wpTimer = {waypointIdx, OTSYS_TIME()};
			castLog(bot, fmt::format("{}: Walking to {} wp {}/{} at ({},{},{})",
				config.logPrefix, waypointTypeName(waypoint.type), waypointIdx + 1, waypoints.size(),
				wp.x, wp.y, wp.z));
		}
		int64_t wpElapsed = OTSYS_TIME() - wpTimer.second;

		// PZ-lock awareness: don't count as stuck if waiting for PZ lock to expire
		if (wpElapsed > 5000 && isBotPzLocked(bot)) {
			auto wpTile = g_game().map.getTile(wp);
			if (wpTile && wpTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
				if (config.globalTimeoutMs > 0) {
					auto& progress = s_routeProgress[bot.guid];
					progress.second = OTSYS_TIME();
				}
				return result;
			}
		}

		// Skip waypoint after per-wp stuck timeout
		if (wpElapsed > config.perWpStuckMs) {
			castLogError(bot, fmt::format("{}: Skipping stuck {} wp {}/{} at ({},{},{}) ({}s elapsed)",
				config.logPrefix, waypointTypeName(waypoint.type), waypointIdx + 1, waypoints.size(),
				wp.x, wp.y, wp.z, wpElapsed / 1000));
			waypointIdx++;
			skipCount = 0;
			bot.pathFailCount = 0;
			continue;
		}

		// Navigation: determine maxDist based on waypoint type
		int32_t maxDist;
		if (walkOnFc) {
			maxDist = 0;
		} else if (waypoint.type == WaypointType::USE_WITH || waypoint.type == WaypointType::MACHETE ||
				   waypoint.type == WaypointType::LADDER || waypoint.type == WaypointType::ROPE ||
				   waypoint.type == WaypointType::DOOR) {
			maxDist = 1;
		} else if (waypoint.type == WaypointType::STAND || waypoint.type == WaypointType::HOLE ||
				   waypoint.type == WaypointType::STAIRS_UP || waypoint.type == WaypointType::STAIRS_DOWN) {
			maxDist = 0;
		} else if (waypoint.type == WaypointType::NPC_INTERACT) {
			// Named explicitly rather than falling into the `else` below, which gave it 0 — the
			// NPC's own tile. These waypoints are routinely authored ON the NPC, and a creature
			// occupies it: outside a protection zone Player::canWalkthrough returns false for an
			// NPC, so maxDist 0 is simply unreachable there. Adjacency always is.
			maxDist = 1;
		} else {
			maxDist = (waypoint.type == WaypointType::NODE) ? 1 : 0;
		}

		// Teleport STAND: step onto teleport tiles with FLAG_NOLIMIT (patrol specific)
		if (config.enableTeleportStand && waypoint.type == WaypointType::STAND) {
			auto navTile = g_game().map.getTile(wp);
			if (navTile && navTile->hasFlag(TILESTATE_TELEPORT)) {
				if (dist <= 1) {
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
					castLog(bot, fmt::format("{}: Stepping onto teleport tile wp {}/{} at ({},{},{})",
						config.logPrefix, waypointIdx + 1, waypoints.size(), wp.x, wp.y, wp.z));
					g_game().internalMoveCreature(player, dir, FLAG_NOLIMIT);
					return result;
				} else {
					goToWithDoors(bot, wp, 1, waypoint.type);
					return result;
				}
			}
		}

		// NODE waypoints: pick a tile from the 9-sqm area (center + 8 adjacent).
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
			bool laneTileTakeable = false; // hoisted: also drives the [LANE] attribution below
			if (g_configManager().getBoolean(BOT_LANE_ENABLE)) {
				// BOT_NAV_REALISM Phase 7: persistent lane. Order candidates so the bot's held
				// lateral side (perpendicular to travel direction) comes first, then the exact
				// waypoint (center), then the rest. The walkability + goTo checks below then pick
				// the first that works, so narrow streets and occupied tiles auto-fall-back to
				// center — no separate corridor detection needed. Center walkers (side==0) just go
				// to the waypoint tile as before.
				const int8_t side = botRouteLaneSide(bot);
				const int32_t sdx = (wp.x > bot.currentPos.x) - (wp.x < bot.currentPos.x);
				const int32_t sdy = (wp.y > bot.currentPos.y) - (wp.y < bot.currentPos.y);
				const int32_t px = -sdy * side; // perpendicular-of-travel, scaled by lane side
				const int32_t py = sdx * side;
				const int32_t prefX = static_cast<int32_t>(wp.x) + px;
				const int32_t prefY = static_cast<int32_t>(wp.y) + py;

				// Phase 7 crowd safety (Opus #5): a lane tile is only preferred if it is actually
				// takeable RIGHT NOW — not creature-occupied, not a floor-change/teleport tile,
				// not a door, and not already claimed by another bot in this same tick. Otherwise
				// it ranks as an ordinary candidate and the bot falls back to the waypoint centre.
				// Without this, bots on a shared route in a crowd all target one offset tile,
				// collide, and churn pathFailCount — the failure mode lanes are supposed to fix.
				if (side != 0) {
					Position lanePos(static_cast<uint16_t>(prefX), static_cast<uint16_t>(prefY), wp.z);
					if (!s_laneReservedThisTick.count(botTileKey(lanePos))) {
						const auto& laneTile = g_game().map.getTile(lanePos);
						if (laneTile
							&& !laneTile->hasFlag(TILESTATE_BLOCKPATH)
							&& !laneTile->hasFlag(TILESTATE_FLOORCHANGE)
							&& !laneTile->hasFlag(TILESTATE_TELEPORT)
							&& laneTile->getTopVisibleCreature(player) == nullptr
							&& !laneTile->hasFlag(TILESTATE_BLOCKSOLID)) {
							laneTileTakeable = true;
							s_laneReservedThisTick.insert(botTileKey(lanePos)); // claim it
						}
					} else {
						s_laneReserveClash++;
					}
				}

				auto rank = [&](const Position& c) -> int {
					if (laneTileTakeable && c.x == prefX && c.y == prefY) return 0; // lane tile first
					if (c.x == wp.x && c.y == wp.y) return 1;                        // then exact waypoint
					return 2;                                                        // then the rest
				};
				std::stable_sort(candidates, candidates + count,
					[&](const Position& a, const Position& b) { return rank(a) < rank(b); });
			} else {
				// Legacy: re-randomize the endpoint each waypoint (superseded by lane + A* jitter).
				for (int i = count - 1; i > 0; i--) {
					int j = uniform_random(0, i);
					std::swap(candidates[i], candidates[j]);
				}
			}
			int chosenIdx = -1;
			for (int i = 0; i < count; i++) {
				auto candidateTile = g_game().map.getTile(candidates[i]);
				if (candidateTile && !candidateTile->hasFlag(TILESTATE_BLOCKPATH)) {
					if (goTo(bot, candidates[i], 0)) {
						navOk = true;
						chosenIdx = i;
						break;
					}
				}
			}
			// Phase 7 telemetry: with the lane sort, index 0 IS the lane tile for an
			// off-centre walker, so a win at 0 means the bot really walked its lane;
			// anything else means it fell back (narrow street / blocked / occupied).
			// Phase 7 telemetry: the lane tile was genuinely walked only if it passed the
			// takeable checks above AND won the candidate loop (rank 0). Anything else — a
			// blocked/occupied/reserved lane tile, or a lane tile the pathfinder couldn't reach —
			// is a centre fallback, which is the designed behavior in narrow or crowded streets.
			if (navOk && g_configManager().getBoolean(BOT_LANE_ENABLE) && botRouteLaneSide(bot) != 0) {
				if (laneTileTakeable && chosenIdx == 0) {
					s_laneOffsetUsed++;
				} else {
					s_laneCenterFallback++;
				}
			}
		} else {
			navOk = goTo(bot, wp, maxDist);
		}

		if (!navOk && config.wideSearchOnFail) {
			// Planner legs only. The chunked walker cannot solve a dogleg — it aims at a
			// straight-line point that lands inside the obstacle — and a leg's first waypoint is
			// normally a DOOR, so that is the common case here rather than an edge case. One
			// cooldown-gated wide search per bot; see goToWide.
			navOk = goToWide(bot, wp, maxDist);
		}
		if (!navOk) {
			// Try opening a door in the way (once), then retry pathfinding
			if (waypoint.type != WaypointType::NODE) {
				if (tryOpenDoors(bot, player, wp)) {
					navOk = goTo(bot, wp, maxDist);
				}
				if (!navOk) {
					tryAttackBlockingMonster(bot);
				}
			}
			bot.pathFailCount++;
			skipCount++;

			// NPC squeeze: A* has already failed to route around (pathFailCount climbing).
			// If a stationary/wandering NPC is the chokepoint in a 1-tile corridor, step
			// past it. Gated at >=3 so it only fires once routing-around has genuinely
			// failed; the helper itself only steps when the blocker is an NPC AND the tile
			// beyond opens progress, so it never plows through players/bots or into a wall.
			if (bot.pathFailCount >= 3 && tryStepPastBlockingNpc(bot, wp)) {
				bot.pathFailCount = 0;
				return result;
			}

			// PZ fallback: in protection zones, try walking onto creature-occupied tiles
			if (waypoint.type == WaypointType::NODE && bot.pathFailCount >= 5) {
				auto myTile = g_game().map.getTile(bot.currentPos);
				if (myTile && myTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
					FindPathParams fpp;
					fpp.fullPathSearch = true;
					fpp.clearSight = false;
					fpp.allowDiagonal = true;
					fpp.keepDistance = false;
					fpp.maxSearchDist = PATH_MAX_DIST;
					fpp.minTargetDist = 0;
					fpp.maxTargetDist = 0;
					std::vector<Direction> dirList;
					if (g_game().map.getPathMatching(player, wp, dirList, FrozenPathingConditionCall(wp), fpp)) {
						botStartAutoWalk(bot, player,dirList);
					}
				}
			}

			if (bot.pathFailCount >= 50) {
				int32_t pathDist = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wp.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wp.y)));
				castLogError(bot, fmt::format("{}: Skipping stuck {} wp {}/{} at ({},{},{}) (pathfails={} dist={} bot=({},{},{}))",
					config.logPrefix, waypointTypeName(waypoint.type), waypointIdx + 1, waypoints.size(),
					wp.x, wp.y, wp.z, bot.pathFailCount, pathDist,
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
				waypointIdx++;
				skipCount = 0;
				bot.pathFailCount = 0;
				continue;
			}
		} else {
			bot.pathFailCount = 0;
		}
		return result;
	}

	// Completed all waypoints
	result.inProgress = false;
	s_routeWpTimer.erase(bot.guid);
	s_routeProgress.erase(bot.guid);
	return result;
}

bool BotEngine::followCityRoute(BotState& bot) {
	if (!bot.followingCityRoute || bot.cityRouteWps.empty()) return false;

	static std::unordered_map<uint32_t, uint32_t> s_routeSkipCount;
	auto& skipCount = s_routeSkipCount[bot.guid];
	WaypointFollowConfig config;
	config.logPrefix = "ROUTE";
	config.globalTimeoutMs = 300000;
	config.perWpStuckMs = 30000;
	config.zChangeGraceMs = 500;

	auto result = followWaypoints(bot, bot.cityRouteWps, bot.cityRouteIdx, skipCount, config);

	if (!result.inProgress) {
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		s_routeSkipCount.erase(bot.guid);
		return false;
	}
	return true;
}

bool BotEngine::tryAttackBlockingMonster(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return false;

	// Scan adjacent tiles for monsters
	auto pos = bot.currentPos;
	auto spectators = Spectators().find<Monster>(pos, false, 1, 1, 1, 1);

	for (const auto& spec : spectators) {
		auto monster = spec->getMonster();
		if (!monster || monster->isRemoved() || monster->getHealth() <= 0) continue;

		// Found a blocking monster — attack it
		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(monster->getPosition().x)),
			std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(monster->getPosition().y)));

		if (dist <= 1 && pos.z == monster->getPosition().z) {
			auto creature = monster->getCreature();
			// Set as attacked creature so weapon auto-attacks work
			player->setAttackedCreature(creature);
			castLog(bot, fmt::format("BLOCKED: Attacking {} at ({},{},{})",
				monster->getName(), monster->getPosition().x, monster->getPosition().y, monster->getPosition().z));
			castSpell(bot, creature);
			return true;
		}
	}

	return false;
}

// Step past an NPC standing in a narrow corridor when A* has found no route around it.
// Engine-local fix for "bot stuck behind an NPC" (e.g. Roshamuul temple). The caller only
// invokes this once normal A* (which still treats creatures as walls outside PZ) has
// repeatedly failed — i.e. there is genuinely no path around. We then force ONE step onto
// the NPC's tile with FLAG_IGNOREBLOCKCREATURE (ignores the NPC but still honours walls,
// solid items and fields — strictly safer than FLAG_NOLIMIT). Next tick, normal A* from the
// NPC tile (start tile is excluded by Map::canWalkTo) routes the bot forward.
//
// Guards (so it never plows through players/bots in open areas and never steps into a dead
// end — this is why broadening A* universally was reverted in b0121c811):
//   1. the blocker tile must contain ONLY NPC creatures (no monster / player / bot);
//   2. wouldChangeZ()==false — never force-step onto a stair/ramp/teleport;
//   3. forward-progress — some neighbour of the NPC tile (other than where we came from)
//      must be walkable, creature-free, and strictly closer to the target. If the real
//      blockage is a wall beyond the NPC, this fails and we do not step.
bool BotEngine::tryStepPastBlockingNpc(BotState& bot, const Position& target) {
	auto player = bot.getPlayer();
	if (!player) return false;

	const Position pos = bot.currentPos;
	if (pos.z != target.z) return false; // z-transitions handled by FC machine / waypoints

	auto chebyshev = [](const Position& a, const Position& b) -> int32_t {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
						std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	};
	const int32_t curDist = chebyshev(pos, target);
	if (curDist == 0) return false;

	// A tile the bot could occupy: walkable ground, no solid/immovable/path block, no
	// z-change from `from`, and not blocked by any non-ghost creature.
	auto isOpenStep = [&](const Position& from, const Position& p) -> bool {
		auto tile = g_game().map.getTile(p);
		if (!tile || !tile->getGround()) return false;
		if (tile->hasFlag(TILESTATE_BLOCKSOLID) || tile->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID) ||
			tile->hasFlag(TILESTATE_BLOCKPATH)) return false;
		if (wouldChangeZ(from, p)) return false;
		if (auto cs = tile->getCreatures(); cs) {
			for (const auto& c : *cs) {
				if (!c->isInGhostMode()) return false;
			}
		}
		return true;
	};

	// Candidate offsets toward the target — only those that reduce Chebyshev distance.
	const int32_t sx = (target.x > pos.x) - (target.x < pos.x);
	const int32_t sy = (target.y > pos.y) - (target.y < pos.y);
	std::vector<std::pair<int32_t, int32_t>> offsets;
	if (sx != 0 && sy != 0) offsets.emplace_back(sx, sy);
	if (sx != 0) offsets.emplace_back(sx, 0);
	if (sy != 0) offsets.emplace_back(0, sy);

	for (const auto& [ox, oy] : offsets) {
		const Position npcPos(static_cast<uint16_t>(pos.x + ox), static_cast<uint16_t>(pos.y + oy), pos.z);
		auto npcTile = g_game().map.getTile(npcPos);
		if (!npcTile || !npcTile->getGround()) continue;
		if (npcTile->hasFlag(TILESTATE_BLOCKSOLID) || npcTile->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID)) continue;
		if (wouldChangeZ(pos, npcPos)) continue; // never force-step onto a stair/ramp/teleport

		// Blocker must be exactly NPC(s): at least one NPC and no monster/player/bot.
		auto cs = npcTile->getCreatures();
		if (!cs || cs->empty()) continue;
		bool hasNpc = false, otherBlocker = false;
		for (const auto& c : *cs) {
			if (c->isInGhostMode()) continue;
			if (c->getNpc()) {
				hasNpc = true;
			} else {
				otherBlocker = true; // monster, player, or bot — not our case
				break;
			}
		}
		if (!hasNpc || otherBlocker) continue;

		// Forward-progress: stepping onto the NPC tile must open a route — some neighbour of
		// the NPC tile (other than where we came from) is walkable, creature-free, and
		// strictly closer to the target than we are now.
		bool progresses = false;
		for (int dx = -1; dx <= 1 && !progresses; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				if (dx == 0 && dy == 0) continue;
				const Position f(static_cast<uint16_t>(npcPos.x + dx), static_cast<uint16_t>(npcPos.y + dy), npcPos.z);
				if (f.x == pos.x && f.y == pos.y) continue;     // not back where we came from
				if (chebyshev(f, target) >= curDist) continue;  // must make progress
				if (isOpenStep(npcPos, f)) { progresses = true; break; }
			}
		}
		if (!progresses) continue;

		const Direction dir = getDirectionTo(pos, npcPos);
		if (dir == DIRECTION_NONE) continue;
		const ReturnValue ret = g_game().internalMoveCreature(player, dir, FLAG_IGNOREBLOCKCREATURE | FLAG_IGNOREFIELDDAMAGE);
		if (ret == RETURNVALUE_NOERROR) {
			castLog(bot, fmt::format("[NPCSTEP] squeezed past NPC at ({},{},{}) toward ({},{},{})",
				npcPos.x, npcPos.y, npcPos.z, target.x, target.y, target.z));
			// Unconditional low-rate telemetry — NPCSTEP is rare by design (only fires at a
			// genuine NPC chokepoint after A* failed). Lets us confirm organic firing without
			// per-bot verbose mode.
			g_logger().info("[NPCSTEP] {} squeezed past NPC at ({},{},{}) toward ({},{},{})",
				player->getName(), npcPos.x, npcPos.y, npcPos.z, target.x, target.y, target.z);
			return true;
		}
	}

	return false;
}

// Wake-from-hibernation safe-position picker. The virtual sim may have advanced
// bot.currentPos to a non-walkable POI tile (depot box, boat NPC tile, adv stone
// tile, FC tile). internalTeleport with FLAG_NOLIMIT would happily place the bot
// on top of that — bot then "arrives" with no walkable adjacent tile and gets
// stuck. This helper validates virtualPos and walks back through the bot's
// route chain until a safe waypoint is found, falling back to town temple.
//
// Per-state route mapping:
//   HUNTING + PATROLLING/LEAVING        → script's waypoint array (patrol/travelFrom)
//   HUNTING + TRAVEL_TO/PREPARING/RESUPPLYING → bot.cityRouteWps
//   TRAVELING (city walk)               → bot.cityRouteWps
//   AdvStone active                     → adventurerStoneRoute_ (engine-shared)
//   PARTY follower                      → leader's currentPos if safe
//   PARTY leader (isPartyHuntLeader)    → same as solo HUNTING
//   IDLE/DWELLING/COMBAT/FLEEING        → no chain → temple
bool BotEngine::wouldBeSeenByAnchor(const Position& p, int margin) const {
	// Exact port of ProtocolGame::canSee (protocolgame.cpp:1921-1944), evaluated per anchor
	// with a symmetric `margin` expansion. An anchor is a real player or a cast-watched bot;
	// for a cast viewer the WATCHED bot's position IS the camera center (the viewer shares
	// the watched Player object), so currentAnchorPts_ (built in refreshAnchorsIfStale, real
	// players + bots with getCastViewerCount()>0) is exactly the right camera set. offsetz
	// MUST be signed (a uint8 z-difference would wrap to 255 and shift the box off-map); the
	// surface (z<=7 can't see z>7) and underground (|dz|<=MAP_LAYER_VIEW_LIMIT) cutoffs and
	// the asymmetric +1 east/south edge all mirror canSee exactly.
	for (const auto& a : currentAnchorPts_) {
		if (a.z <= MAP_INIT_SURFACE_LAYER) {
			if (p.z > MAP_INIT_SURFACE_LAYER) {
				continue;  // surface anchor cannot see underground
			}
		} else if (a.z >= MAP_INIT_SURFACE_LAYER + 1) {
			if (std::abs(static_cast<int>(a.z) - static_cast<int>(p.z)) > MAP_LAYER_VIEW_LIMIT) {
				continue;  // underground anchor sees only +/- MAP_LAYER_VIEW_LIMIT floors
			}
		}
		const int offsetz = static_cast<int>(a.z) - static_cast<int>(p.z);  // signed
		const int px = static_cast<int>(p.x), py = static_cast<int>(p.y);
		const int ax = static_cast<int>(a.x), ay = static_cast<int>(a.y);
		if (px >= ax - MAP_MAX_CLIENT_VIEW_PORT_X + offsetz - margin
			&& px <= ax + (MAP_MAX_CLIENT_VIEW_PORT_X + 1) + offsetz + margin
			&& py >= ay - MAP_MAX_CLIENT_VIEW_PORT_Y + offsetz - margin
			&& py <= ay + (MAP_MAX_CLIENT_VIEW_PORT_Y + 1) + offsetz + margin) {
			return true;
		}
	}
	return false;
}

Position BotEngine::chooseWakePosition(BotState& bot, const Position& virtualPos, bool proximityWake) {
	// BOT_LIVENESS off-screen walk-in tunables (2026-06-13):
	//   WAKE_OFFSCREEN_MARGIN — expand the viewport box so a bot isn't placed one tile past
	//     the edge, where an anchor stepping toward it would reveal a pop-in next tick.
	//   MAX_OFFSCREEN_REWIND_TILES — cap the backward route jump so the bot walks in from
	//     just off-screen (~4-7s) rather than trekking from far away. If the nearest
	//     off-screen NODE exceeds this, fall through to on-screen placement + login sparkle.
	static constexpr int WAKE_OFFSCREEN_MARGIN = 2;
	static constexpr int MAX_OFFSCREEN_REWIND_TILES = 16;

	// Unsafe-tile mask + MoveEvent/aid/pathfinding checks live in isUnsafeWakeTile (shared
	// with chooseSafePartyFollowPos so party placement uses the exact same safety, not a
	// weaker copy).
	auto isUnsafe = [&](const Position& p) -> bool { return isUnsafeWakeTile(bot, p); };

	// Pack (x, y, z) into uint64_t for hash dedup (file-scope packPosU64).
	auto packPos = [](const Position& p) -> uint64_t { return packPosU64(p); };

	// Tile is occupied if a live creature stands on it OR a prior bot in this wake
	// burst reserved it via chooseWakePosition.
	auto isOccupiedOrReserved = [&](const Position& p) -> bool {
		if (burstReservedTiles_.count(packPos(p))) return true;
		auto tile = g_game().map.getTile(p);
		if (!tile) return false;
		if (auto creatures = tile->getCreatures(); creatures && !creatures->empty()) {
			return true;
		}
		return false;
	};

	// Ring-by-ring spiral outward from center (radius 1..3) for a safe + unoccupied tile.
	// Same z only — we don't want to place the bot on a different floor than its route
	// expects. Returns Position() on failure (caller decides fallback).
	auto spiralFindFree = [&](const Position& center) -> Position {
		for (int r = 1; r <= 3; ++r) {
			for (int dy = -r; dy <= r; ++dy) {
				for (int dx = -r; dx <= r; ++dx) {
					if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring r only
					Position p(static_cast<uint16_t>(center.x + dx),
						static_cast<uint16_t>(center.y + dy), center.z);
					if (isUnsafe(p)) continue;
					if (isOccupiedOrReserved(p)) continue;
					return p;
				}
			}
		}
		return Position();
	};

	// Reserve and return a tile so subsequent chooseWakePosition calls in this burst
	// don't pick the same spread tile.
	auto reserveAndReturn = [&](const Position& p) -> Position {
		if (p.x > 0) burstReservedTiles_.insert(packPos(p));
		return p;
	};

	// BOT_LIVENESS (2026-06-13): like the NODE-only walkBack below, but returns the NEAREST
	// upstream NODE that is safe AND outside every anchor's viewport, within
	// MAX_OFFSCREEN_REWIND_TILES of virtualPos. Returns {Position(),0} if the nearest
	// off-screen NODE is farther than the cap (the bot would visibly trek in — caller falls
	// through to on-screen placement + login sparkle) or none exists. NODE-only for the same
	// reason as walkBack: internalTeleport placement doesn't fire onStepIn MoveEvents.
	auto walkBackOffScreen = [&](const std::vector<Waypoint>& wps, size_t idx) -> std::pair<Position, size_t> {
		if (wps.empty()) return {Position(), 0};
		const ssize_t startIdx = static_cast<ssize_t>(std::min(idx, wps.size())) - 1;
		for (ssize_t i = startIdx; i >= 0; --i) {
			if (wps[i].type != WaypointType::NODE) continue;
			const Position& wp = wps[i].pos;
			const int cheb = std::max(std::abs(static_cast<int>(wp.x) - static_cast<int>(virtualPos.x)),
									  std::abs(static_cast<int>(wp.y) - static_cast<int>(virtualPos.y)));
			if (cheb > MAX_OFFSCREEN_REWIND_TILES) return {Position(), 0};  // nearest off-screen NODE too far
			if (isUnsafe(wp)) continue;
			if (wouldBeSeenByAnchor(wp, WAKE_OFFSCREEN_MARGIN)) continue;   // still on-screen → keep walking back
			return {wp, static_cast<size_t>(i)};
		}
		return {Position(), 0};
	};

	// Fast path: virtualPos is safe.
	if (!isUnsafe(virtualPos)) {
		// BOT_LIVENESS (2026-06-13): a proximity wake (walk-by / cast-watch) that would land
		// the bot INSIDE a viewer's screen → relocate UPSTREAM along the bot's route to an
		// off-screen NODE so it walks into view instead of popping in. Only HUNTING
		// patrol/leaving + AdvStone qualify — those routes survive hibernation; cityRouteWps
		// is cleared on hibernate, so TRAVELING / IDLE-route bots have no route to rewind and
		// fall through to on-screen placement + the wakeBot login sparkle. The waypoint index
		// is rewound atomically (same contract as the unsafe-branch walkBack) so live AI's
		// followWaypoints sanity check doesn't trip after wake.
		if (proximityWake && !currentAnchorPts_.empty() && wouldBeSeenByAnchor(virtualPos, WAKE_OFFSCREEN_MARGIN)) {
			std::pair<Position, size_t> wb{Position(), 0};
			bool rewindHunt = false, rewindAdv = false;
			if (bot.state == BotAIState::HUNTING) {
				const HuntScript* script = nullptr;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { script = &s; break; }
				}
				if (script) {
					if (bot.huntPhase == HuntPhase::PATROLLING) {
						const size_t startIdx = (bot.huntWaypointIdx == 0 && !script->patrolWaypoints.empty())
							? script->patrolWaypoints.size() : bot.huntWaypointIdx;
						wb = walkBackOffScreen(script->patrolWaypoints, startIdx);
						rewindHunt = true;
					} else if (bot.huntPhase == HuntPhase::LEAVING) {
						if (bot.isRecoveryRoute && !bot.recoveryWaypoints.empty()) {
							wb = walkBackOffScreen(bot.recoveryWaypoints, bot.huntWaypointIdx);
						} else {
							wb = walkBackOffScreen(script->travelFromWaypoints, bot.huntWaypointIdx);
						}
						rewindHunt = true;
					}
					// TRAVEL_TO/PREPARING/RESUPPLYING use cityRouteWps (cleared on hibernate) → no walk-in.
				}
			} else if (bot.advStoneActive) {
				wb = walkBackOffScreen(adventurerStoneRoute_, bot.advStoneRouteIdx);
				rewindAdv = true;
			}
			if (wb.first.x > 0) {
				if (rewindHunt) bot.huntWaypointIdx = wb.second;
				if (rewindAdv) bot.advStoneRouteIdx = static_cast<uint16_t>(wb.second);
				if (isOccupiedOrReserved(wb.first)) {
					Position spread = spiralFindFree(wb.first);
					if (spread.x > 0) return reserveAndReturn(spread);
				}
				return reserveAndReturn(wb.first);
			}
			// No off-screen walk-in available → fall through to on-screen placement;
			// wakeBot emits the login sparkle for this proximity wake.
		}
		// If occupied or already reserved by another bot in this burst, spread to an
		// adjacent free tile. If no free tile within radius 3, fall through to using
		// virtualPos anyway (stacking is preferable to misplacement).
		if (isOccupiedOrReserved(virtualPos)) {
			Position spread = spiralFindFree(virtualPos);
			if (spread.x > 0) return reserveAndReturn(spread);
		}
		return reserveAndReturn(virtualPos);
	}

	// Walk back through a waypoint array starting from idx-1 down to 0. Returns
	// {pos, chosenIdx} on success or {Position(), 0} on no-safe-found. Phase F:
	// caller uses chosenIdx to rewind the corresponding bot index field atomically
	// with the chosen position (so live AI's followWaypoints 200-tile sanity check
	// doesn't trip after wake).
	//
	// Only NODE waypoints are accepted as wake placement targets. STAND and all
	// action-required types (STAIRS_UP/DOWN, LADDER, ROPE, HOLE, LEVITATE_*, DOOR,
	// ACTION, USE_WITH, MACHETE, NPC_INTERACT, TELEPORT) need an explicit step or
	// action to fire their effect. internalTeleport placement during wake doesn't
	// trigger onStepIn MoveEvents, so a bot placed on a step-required tile is
	// stuck — the route advances its waypoint index thinking it's "at" the wp but
	// the floor change / action never fired. STAND in particular is often used to
	// mark the action position itself (top of stairs, on the rope spot, etc.), so
	// it's also excluded — the upstream NODE is always the correct wake target.
	auto walkBack = [&](const std::vector<Waypoint>& wps, size_t idx) -> std::pair<Position, size_t> {
		if (wps.empty()) return {Position(), 0};
		const ssize_t startIdx = static_cast<ssize_t>(std::min(idx, wps.size())) - 1;
		for (ssize_t i = startIdx; i >= 0; --i) {
			if (wps[i].type != WaypointType::NODE) continue;
			if (isUnsafe(wps[i].pos)) continue;
			return {wps[i].pos, static_cast<size_t>(i)};
		}
		return {Position(), 0};
	};

	Position candidate;

	if (bot.state == BotAIState::HUNTING) {
		const HuntScript* script = nullptr;
		for (const auto& s : huntScripts_) {
			if (s.id == bot.huntScriptId) { script = &s; break; }
		}
		if (script) {
			switch (bot.huntPhase) {
				case HuntPhase::PATROLLING: {
					// Wrap-around: idx=0 means we just wrapped, so check wps[size-1] first
					std::pair<Position, size_t> wb;
					if (bot.huntWaypointIdx == 0 && !script->patrolWaypoints.empty()) {
						wb = walkBack(script->patrolWaypoints, script->patrolWaypoints.size());
					} else {
						wb = walkBack(script->patrolWaypoints, bot.huntWaypointIdx);
					}
					candidate = wb.first;
					if (candidate.x > 0) bot.huntWaypointIdx = wb.second;
					break;
				}
				case HuntPhase::LEAVING: {
					// LEAVING uses recoveryWaypoints if isRecoveryRoute, else travelFrom
					std::pair<Position, size_t> wb;
					if (bot.isRecoveryRoute && !bot.recoveryWaypoints.empty()) {
						wb = walkBack(bot.recoveryWaypoints, bot.huntWaypointIdx);
					} else {
						wb = walkBack(script->travelFromWaypoints, bot.huntWaypointIdx);
					}
					candidate = wb.first;
					if (candidate.x > 0) bot.huntWaypointIdx = wb.second;
					break;
				}
				case HuntPhase::TRAVEL_TO:
				case HuntPhase::PREPARING:
				case HuntPhase::RESUPPLYING: {
					// These phases use cityRouteWps (city navigation), NOT the script's
					// hunt waypoints. The hunt waypoints kick in only at PATROLLING.
					auto wb = walkBack(bot.cityRouteWps, bot.cityRouteIdx);
					candidate = wb.first;
					if (candidate.x > 0) bot.cityRouteIdx = wb.second;
					break;
				}
			}
		}
	} else if (bot.state == BotAIState::TRAVELING) {
		auto wb = walkBack(bot.cityRouteWps, bot.cityRouteIdx);
		candidate = wb.first;
		if (candidate.x > 0) bot.cityRouteIdx = wb.second;
	} else if (bot.state == BotAIState::IDLE && bot.followingCityRoute && !bot.cityRouteWps.empty()) {
		// Phase D: IDLE bots may now have a loaded city route (POI walks, depot walks,
		// navigate). Mirror the TRAVELING branch so wakes mid-route don't fall back to
		// temple — walk back through the route to the nearest safe upstream waypoint.
		auto wb = walkBack(bot.cityRouteWps, bot.cityRouteIdx);
		candidate = wb.first;
		if (candidate.x > 0) bot.cityRouteIdx = wb.second;
	} else if (bot.advStoneActive) {
		auto wb = walkBack(adventurerStoneRoute_, bot.advStoneRouteIdx);
		candidate = wb.first;
		if (candidate.x > 0) bot.advStoneRouteIdx = static_cast<uint16_t>(wb.second);
	} else if (bot.partyLeaderGuid != 0 && !bot.isPartyHuntLeader) {
		// Party follower: snap to leader's position if safe.
		auto lit = guidToIndex_.find(bot.partyLeaderGuid);
		if (lit != guidToIndex_.end()) {
			const auto& leader = bots_[lit->second];
			if (!isUnsafe(leader.currentPos)) candidate = leader.currentPos;
		}
	}

	if (candidate.x > 0) return reserveAndReturn(candidate);

	// Final fallback: town temple — always known-walkable. Spread here too because
	// many bots may fall back to the same temple simultaneously (mass-wake or stale
	// virtualPos wave) → pile on the exact same tile without the spiral.
	auto town = g_game().map.towns.getTown(bot.townId);
	if (town) {
		Position templePos = town->getTemplePosition();
		if (templePos.x > 0) {
			if (isOccupiedOrReserved(templePos)) {
				Position spread = spiralFindFree(templePos);
				if (spread.x > 0) return reserveAndReturn(spread);
			}
			return reserveAndReturn(templePos);
		}
	}

	// Widened last resort before conceding. This used to `return virtualPos;` — the very tile
	// the mask had already rejected — on the theory that the caller's own ladder would cope.
	// It doesn't: wakeBot teleports to whatever this returns. Rings 4-5 + temple + login run
	// only here, so the common wake path keeps its rings-1-3 cost (the awake-bot cap exists
	// for exactly that reason).
	{
		Position widened = safePlacementTail(bot, virtualPos, burstReservedTiles_,
			/*allowWideRings=*/true, /*allowTempleFallback=*/true, "wake");
		if (widened.x > 0) {
			s_tpSafeWakeRepairs++;
			return widened;
		}
	}

	// Absolute last resort: return the original (unsafe) virtualPos so the bot is placed
	// somewhere rather than nowhere. safePlacementTail has already warn-logged this.
	return virtualPos;
}

bool BotEngine::isUnsafeWakeTile(BotState& bot, const Position& p) {
	// Tile flags that indicate "do not place a bot here" — wall, blocking item,
	// any FC direction (would trigger involuntary z-change), teleport tile (would
	// auto-warp), depot box (non-walkable item), magic field (damage on step).
	constexpr uint32_t kUnsafeWakeMask =
		TILESTATE_BLOCKSOLID | TILESTATE_BLOCKPATH |
		TILESTATE_IMMOVABLEBLOCKSOLID | TILESTATE_IMMOVABLEBLOCKPATH |
		TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT |
		TILESTATE_DEPOT | TILESTATE_MAGICFIELD;

	if (p.x == 0) return true; // unset position
	auto tile = g_game().map.getTile(p);
	if (!tile) return true; // void / no tile
	if (tile->hasFlag(kUnsafeWakeMask)) return true;
	// Reject tiles with custom teleport-style step scripts (adv stone forcefield aid:4253,
	// quest-gated teleports like sorcerer guild Thais aid:5555, schrodingers island
	// aid:15998/15999, etc.). Two registration patterns we care about:
	//   1. Position-based: some quest teleports register the exact xyz
	//   2. ActionId-based: most teleport scripts register an aid and items carry that aid
	// We deliberately do NOT check hasItemId/hasUniqueId — those would catch innocent
	// step handlers (depot floor message tiles, etc.) that bots must legitimately walk on.
	if (g_moveEvents().hasPosition(p)) return true;
	// Check ground item (e.g. magic forcefield aid:4253 is set on the tile's ground)
	if (auto ground = tile->getGround()) {
		int32_t aid = static_cast<int32_t>(ground->getAttribute<uint16_t>(ItemAttribute_t::ACTIONID));
		if (aid > 0 && g_moveEvents().hasActionId(aid)) return true;
	}
	if (auto items = tile->getItemList()) {
		for (const auto& item : *items) {
			int32_t aid = static_cast<int32_t>(item->getAttribute<uint16_t>(ItemAttribute_t::ACTIONID));
			if (aid > 0 && g_moveEvents().hasActionId(aid)) return true;
		}
	}
	// Catch-all for door / lever / use_with item targets that aren't tile-flagged
	// but reject pathfinding via blocking items in their stack.
	auto pl = bot.getPlayer();
	if (pl) {
		if (tile->queryAdd(0, pl, 1, FLAG_PATHFINDING) != RETURNVALUE_NOERROR) return true;
	}
	return false;
}

Position BotEngine::chooseSafePartyFollowPos(BotState& bot, const Position& center,
	std::unordered_set<uint64_t>& reservedThisTick) {
	auto occupiedOrReserved = [&](const Position& p) -> bool {
		if (reservedThisTick.count(packPosU64(p))) return true;
		auto tile = g_game().map.getTile(p);
		if (!tile) return false;
		if (auto cs = tile->getCreatures(); cs && !cs->empty()) return true;
		return false;
	};
	auto take = [&](const Position& p) -> Position {
		if (p.x > 0) reservedThisTick.insert(packPosU64(p));
		return p;
	};

	// Prefer the leader's exact tile when it is FC-safe and free.
	if (!isUnsafeWakeTile(bot, center) && !occupiedOrReserved(center)) {
		return take(center);
	}
	// Spiral rings 1-3 (same z) for the nearest FC-safe, unoccupied, unreserved tile so a
	// support never lands on the leader's ladder/hole/teleport tile nor stacks on the leader.
	for (int r = 1; r <= 3; ++r) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring r only
				Position p(static_cast<uint16_t>(center.x + dx),
					static_cast<uint16_t>(center.y + dy), center.z);
				if (isUnsafeWakeTile(bot, p)) continue;
				if (occupiedOrReserved(p)) continue;
				return take(p);
			}
		}
	}
	// Rings 1-3 exhausted. Widen to 4-5 before conceding, but WITHOUT the temple/login leg:
	// a follower belongs near its leader, and flinging it to temple is what the party-hunt
	// supports-inert fix was written to stop.
	{
		Position widened = safePlacementTail(bot, center, reservedThisTick,
			/*allowWideRings=*/true, /*allowTempleFallback=*/false, "partyFollow");
		if (widened.x > 0) {
			s_tpSafeFormationRepairs++;
			return widened;
		}
	}

	// No safe free tile within radius 5 (extremely cramped) — place at center anyway. This is
	// no worse than the pre-fix unconditional teleport-to-leader, and stacking near the leader
	// is preferable to flinging a follower to temple.
	return take(center);
}

// ============================================================================
// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 3/4 — shared safe-placement tail.
//
// Game::internalTeleport calls map.moveCreature directly and never queryDestination, so a
// bot dropped on a TILESTATE_FLOORCHANGE tile just STANDS there while the waypoint follower
// believes it arrived. Every raw BOT_TELEPORT destination therefore has to be vetted.
//
// One helper, three callers with genuinely different reservation scopes, so the set is
// passed by reference rather than assumed:
//   chooseWakePosition        -> the engine member burstReservedTiles_ (whole wake burst)
//   chooseSafePartyFollowPos  -> the caller's per-tick set
//   safeTeleportLanding       -> a per-call temporary
//
// allowTempleFallback is likewise not cosmetic. chooseSafePartyFollowPos deliberately does
// NOT send a follower to temple ("stacking near the leader is preferable"), and routing it
// through a tail that ends in temple -> login would reverse that shipped decision.
// ============================================================================
Position BotEngine::safePlacementTail(BotState& bot, const Position& center,
	std::unordered_set<uint64_t>& reserved, bool allowWideRings, bool allowTempleFallback,
	const char* site) {
	auto occupiedOrReserved = [&](const Position& p) -> bool {
		if (reserved.count(packPosU64(p))) return true;
		auto tile = g_game().map.getTile(p);
		if (!tile) return false;
		if (auto cs = tile->getCreatures(); cs && !cs->empty()) return true;
		return false;
	};
	auto take = [&](const Position& p) -> Position {
		if (p.x > 0) reserved.insert(packPosU64(p));
		return p;
	};
	// Rings 1-3 is the primary spiral and keeps its existing per-wake cost. Rings 4-5 only
	// run on the last-resort path (allowWideRings), which by construction is rare — the
	// project has a documented hard cap on simultaneous awake bots for exactly this reason.
	const int maxRing = allowWideRings ? 5 : 3;
	for (int r = 1; r <= maxRing; ++r) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring r only
				Position p(static_cast<uint16_t>(static_cast<int32_t>(center.x) + dx),
					static_cast<uint16_t>(static_cast<int32_t>(center.y) + dy), center.z);
				if (isUnsafeWakeTile(bot, p)) continue;
				if (occupiedOrReserved(p)) continue;
				return take(p);
			}
		}
	}
	if (allowTempleFallback) {
		if (auto town = g_game().map.towns.getTown(bot.townId)) {
			Position templePos = town->getTemplePosition();
			if (templePos.x > 0 && !isUnsafeWakeTile(bot, templePos)) {
				if (!occupiedOrReserved(templePos)) return take(templePos);
				for (int r = 1; r <= 3; ++r) {
					for (int dy = -r; dy <= r; ++dy) {
						for (int dx = -r; dx <= r; ++dx) {
							if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
							Position p(static_cast<uint16_t>(static_cast<int32_t>(templePos.x) + dx),
								static_cast<uint16_t>(static_cast<int32_t>(templePos.y) + dy), templePos.z);
							if (isUnsafeWakeTile(bot, p)) continue;
							if (occupiedOrReserved(p)) continue;
							return take(p);
						}
					}
				}
			}
		}
		if (auto pl = bot.getPlayer()) {
			Position loginPos = pl->getLoginPosition();
			if (loginPos.x > 0 && !isUnsafeWakeTile(bot, loginPos) && !occupiedOrReserved(loginPos)) {
				return take(loginPos);
			}
		}
	}
	// Nothing worked. Return an unset position so the caller decides — and make the residual
	// case visible instead of silent.
	s_tpSafeLastResort++;
	g_logger().warn("[TPSAFE] {} could not place bot '{}' safely near ({},{},{}) — caller falls back",
		site, bot.name, center.x, center.y, center.z);
	return Position();
}

// Vet a teleport destination, and if it is unsafe find something better.
//
//  1. destination already safe -> use it
//  2. route rewind: walk backward to the nearest safe plain NODE, rewinding *idx with it
//  3. spiral rings 1-3, then (last resort) 4-5 -> temple -> login
//  4. nothing worked -> return `desired` and warn, so the bot is never left unplaced
Position BotEngine::safeTeleportLanding(BotState& bot, const Position& desired,
	const std::vector<Waypoint>* route, size_t* idx, const char* site) {
	if (!isUnsafeWakeTile(bot, desired)) {
		return desired;
	}

	// --- 2. backward rewind ---------------------------------------------------------
	// Only when the caller supplied a route. Sites that teleport ONTO a TELEPORT waypoint
	// pass nullptr on purpose: a synth bridge exists precisely because the migration tool
	// found the gap unwalkable, so rewinding to the near side would strand the bot or loop
	// it back through the same teleport forever.
	if (route && idx && !route->empty() && *idx > 0 && *idx <= route->size()) {
		auto& budget = s_tpRewindBudget[bot.guid];
		if (budget > 0) {
			constexpr size_t kMaxRewindWps = 12;
			constexpr int32_t kMaxRewindTiles = 30;
			const size_t startIdx = *idx - 1;
			const size_t floorIdx = startIdx >= kMaxRewindWps ? startIdx - kMaxRewindWps + 1 : 0;
			for (size_t i = startIdx + 1; i-- > floorIdx; ) {
				const auto& wpt = (*route)[i];
				if (wpt.type != WaypointType::NODE || wpt.isWalkOnFc) continue;
				int32_t d = std::max(
					std::abs(static_cast<int32_t>(wpt.pos.x) - static_cast<int32_t>(desired.x)),
					std::abs(static_cast<int32_t>(wpt.pos.y) - static_cast<int32_t>(desired.y)));
				if (d > kMaxRewindTiles) break; // monotonically further the more we rewind
				if (isUnsafeWakeTile(bot, wpt.pos)) continue;
				// Refuse to rewind ACROSS an action waypoint. followWaypoints processes every
				// index in order on the way forward and handleActionWaypoint has no
				// already-visited memory, so a span containing one of these would fire it a
				// second time — burning a charge, re-taking a ladder, or re-casting levitate.
				// DOOR is genuinely idempotent (tryOpenDoorAt on an open door is a no-op).
				bool spanSafe = true;
				for (size_t j = i; j < *idx; j++) {
					switch ((*route)[j].type) {
						case WaypointType::USE_WITH:
						case WaypointType::LADDER:
						case WaypointType::ROPE:
						case WaypointType::MACHETE:
						case WaypointType::NPC_INTERACT:
						case WaypointType::TELEPORT:
						case WaypointType::LEVITATE_UP:
						case WaypointType::LEVITATE_DOWN:
							spanSafe = false;
							break;
						default:
							break;
					}
					if (!spanSafe) break;
				}
				if (!spanSafe) break;
				budget--;
				s_tpSafeRewinds++;
				// Index and position are written together — no caller can take one without
				// the other.
				*idx = i;
				castLog(bot, fmt::format("TPSAFE: {} unsafe landing ({},{},{}) — rewound to wp {} ({},{},{})",
					site, desired.x, desired.y, desired.z, i + 1, wpt.pos.x, wpt.pos.y, wpt.pos.z));
				return wpt.pos;
			}
			s_tpSafeRefused++;
		} else {
			s_tpSafeRefused++;
		}
	}

	// --- 3. spiral / temple / login -------------------------------------------------
	std::unordered_set<uint64_t> localReserved;
	Position tail = safePlacementTail(bot, desired, localReserved, /*allowWideRings=*/true,
		/*allowTempleFallback=*/true, site);
	if (tail.x > 0) {
		s_tpSafeRepairs++;
		castLog(bot, fmt::format("TPSAFE: {} unsafe landing ({},{},{}) — placed at ({},{},{})",
			site, desired.x, desired.y, desired.z, tail.x, tail.y, tail.z));
		return tail;
	}

	// --- 4. give up, visibly --------------------------------------------------------
	return desired;
}

Position BotEngine::findReachableDepotLocker(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return {};

	struct LockerInfo {
		Position pos;
		int32_t dist;
		bool occupied;
		bool crossZ;
		bool hasFreeNonFcAdj;
	};
	std::vector<LockerInfo> lockers;

	// Cross-z distance penalty: must exceed the maximum Manhattan distance reachable within
	// the ±MAP_MAX_VIEW_PORT_X scan area (= 2*MAP_MAX_VIEW_PORT_X) so ANY same-z locker
	// STRICTLY outranks ANY cross-z locker on the distance axis. Off-floor lockers stay in
	// the list (Darashia-style z7-POI/z8-locker depots must remain reachable) but are only
	// chosen when no same-z locker exists — this stops the cross-z FC up/down churn.
	static constexpr int32_t kCrossZDistPenalty = 2 * MAP_MAX_VIEW_PORT_X + 1;

	// Scan server viewport area (±11 tiles, 23x23) for depot lockers using TILESTATE_DEPOT flag
	// Uses MAP_MAX_VIEW_PORT_X/Y — same range as Creature::canSee() and spectator system
	// Prefer same-z lockers; fall back to z±1 if none found at current z
	std::vector<uint8_t> zLevels = { bot.currentPos.z };
	if (bot.currentPos.z < 15) zLevels.push_back(bot.currentPos.z + 1);
	if (bot.currentPos.z > 0) zLevels.push_back(bot.currentPos.z - 1);

	for (uint8_t scanZ : zLevels) {
		for (int dx = -MAP_MAX_VIEW_PORT_X; dx <= MAP_MAX_VIEW_PORT_X; dx++) {
			for (int dy = -MAP_MAX_VIEW_PORT_Y; dy <= MAP_MAX_VIEW_PORT_Y; dy++) {
				Position checkPos(
					static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.x) + dx),
					static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.y) + dy),
					scanZ);

				auto tile = g_game().map.getTile(checkPos);
				if (!tile) continue;

				// Use TILESTATE_DEPOT flag — catches all depot locker types
				if (!tile->hasFlag(TILESTATE_DEPOT)) continue;

				bool occupied = false;
				auto topCreature = tile->getTopCreature();
				if (topCreature && topCreature->getID() != player->getID()) {
					occupied = true;
				}
				const bool crossZ = (scanZ != bot.currentPos.z);
				int32_t d = std::abs(dx) + std::abs(dy);
				// Prefer same-z lockers (strict penalty so same-z always wins on distance)
				if (crossZ) d += kCrossZDistPenalty;
				lockers.push_back({ checkPos, d, occupied, crossZ, false });
			}
		}
		// Always scan all z-levels — z+20 distance penalty already prefers same-z.
		// This ensures lockers on other floors are found when same-z ones are blacklisted.
	}

	if (lockers.empty()) {
		castLog(bot, fmt::format("DEPOT: No lockers found in 23x23 area at z={} (checked ±1 z)", bot.currentPos.z));
		return {};
	}

	// Filter out blacklisted lockers (ones the bot previously failed to reach)
	auto blIt = s_depotBlacklist.find(bot.guid);
	if (blIt != s_depotBlacklist.end() && !blIt->second.empty()) {
		auto& bl = blIt->second;
		lockers.erase(std::remove_if(lockers.begin(), lockers.end(),
			[&bl](const LockerInfo& l) {
				for (const auto& p : bl) {
					if (p.x == l.pos.x && p.y == l.pos.y && p.z == l.pos.z) return true;
				}
				return false;
			}), lockers.end());
		if (lockers.empty()) {
			castLog(bot, fmt::format("DEPOT: All lockers blacklisted ({} tried), none available",
				bl.size()));
			return {};
		}
	}

	static const std::vector<std::pair<int, int>> adjacentOffsets = {
		{0, -1}, {0, 1}, {-1, 0}, {1, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}
	};

	// Compute, per locker, whether it has a "clean" approach tile: an adjacent tile with
	// ground, no BLOCKSOLID, no creature, AND no FLOORCHANGE. The FLOORCHANGE exclusion is
	// the fix for "circles the stairs as if it had no STAND waypoint" — goTo(locker,1) uses
	// A* which rejects FLOORCHANGE tiles, so a locker whose only free neighbour is a stair
	// tile can never be settled adjacent. Such lockers are DEMOTED, not excluded (a depot
	// with only a stair-flanked locker still hands one back via the sort fallthrough).
	for (auto& locker : lockers) {
		for (const auto& [ox, oy] : adjacentOffsets) {
			Position adjPos(
				static_cast<uint16_t>(static_cast<int32_t>(locker.pos.x) + ox),
				static_cast<uint16_t>(static_cast<int32_t>(locker.pos.y) + oy),
				locker.pos.z);
			auto adjTile = g_game().map.getTile(adjPos);
			if (!adjTile || !adjTile->getGround()) continue;
			if (adjTile->hasFlag(TILESTATE_BLOCKSOLID)) continue;
			if (adjTile->hasFlag(TILESTATE_FLOORCHANGE)) continue; // A* won't path onto a stair tile
			if (adjTile->getTopCreature()) continue;
			locker.hasFreeNonFcAdj = true;
			break;
		}
	}

	// Score: clean non-FC reachable adjacency first (kills the stair-circling), then
	// unoccupied, then closest (cross-z already penalised into dist → same-z preferred,
	// killing the up/down churn). Demotions, not exclusions — a locker is always returned.
	std::sort(lockers.begin(), lockers.end(), [](const LockerInfo& a, const LockerInfo& b) {
		if (a.hasFreeNonFcAdj != b.hasFreeNonFcAdj) return a.hasFreeNonFcAdj; // clean adj first
		if (a.occupied != b.occupied) return !a.occupied;                    // unoccupied first
		return a.dist < b.dist;                                              // then closest
	});

	const auto& best = lockers.front();
	castLog(bot, fmt::format("DEPOT: Selected locker ({},{},{}) of {} — freeNonFcAdj={} crossZ={} dist={}",
		best.pos.x, best.pos.y, best.pos.z, lockers.size(),
		best.hasFreeNonFcAdj ? "Y" : "N", best.crossZ ? "Y" : "N", best.dist));
	return best.pos;
}


// BOT_SHRINE_IDLE: find the nearest reward shrine and imbuing shrine around `from`.
//
// This is findReachableDepotLocker with one predicate swapped, and the structure is copied rather
// than generalised on purpose — the two differ in three small ways that a shared helper would have
// to carry as flags anyway (item-list predicate vs TILESTATE, cardinal-only vs any adjacency, and
// a wider radius with a rescaled cross-z penalty), and the locker version is load-bearing enough
// that refactoring it underneath a new caller is the riskier edit.
//
// What is inherited, and why each matters here too:
//   * z-1..z+1 with SHRINE_CROSS_Z_PENALTY folded into the distance, so ANY same-z candidate
//     strictly outranks ANY cross-z one. Shrines live inside depot and temple buildings, which
//     routinely span a floor — a measured 10 of 17 towns have their nearest shrine off the town's
//     walk z — so the band is required, and the penalty is what stops the up/down FC churn.
//   * "free non-FC adjacency": the stand tile must not be a floor-change tile, because A* refuses
//     to path onto one and the bot would circle the stairs forever. Same fix, same reason.
//   * demote, don't exclude — a shrine with only an awkward stand tile still gets returned.
//
// Cardinal-only adjacency is the one deliberate divergence: a locker is OPENED, which works from a
// diagonal, but a shrine is FACED, and internalCreatureTurn is 4-directional. A diagonal stand
// tile cannot look at the shrine, which is the entire point of the feature.
//
// Cost is ~31x31x3 getTile calls per anchor. getTile materializes from the BasicTile cache, so
// this is NOT something to call per bot per tick — callers go through shrineMemoForTown, which
// runs it at most once per town for the life of the engine.
bool BotEngine::findNearbyShrines(const Position& from, int32_t radius, ShrineScanResult& out) const {
	struct Cand {
		Position shrine;
		Position stand;
		int32_t  dist = 0;
		bool     hasStand = false;
		bool     occupied = false;
		uint32_t houseId = 0;   // 0 = not on a house tile
	};
	// [0] reward, [1] imbuing — index is kind-1 throughout.
	std::vector<Cand> cands[2];
	bool any = false;

	auto shrineKindAt = [](const std::shared_ptr<Tile>& tile) -> uint8_t {
		const auto* items = tile->getItemList();
		if (!items) return 0;
		for (const auto& item : *items) {
			const uint16_t id = item->getID();
			for (const uint16_t r : kRewardShrineIds) {
				if (id == r) return SHRINE_KIND_REWARD;
			}
			for (const uint16_t i : kImbuingShrineIds) {
				if (id == i) return SHRINE_KIND_IMBUING;
			}
		}
		return 0;
	};

	// Cardinals only, and in a fixed order so the pick is deterministic for a given map.
	static const std::pair<int32_t, int32_t> kCardinals[4] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };

	std::vector<uint8_t> zLevels = { from.z };
	if (from.z < MAP_MAX_LAYERS - 1) zLevels.push_back(static_cast<uint8_t>(from.z + 1));
	if (from.z > 0) zLevels.push_back(static_cast<uint8_t>(from.z - 1));

	for (uint8_t scanZ : zLevels) {
		for (int32_t dx = -radius; dx <= radius; dx++) {
			for (int32_t dy = -radius; dy <= radius; dy++) {
				Position checkPos(
					static_cast<uint16_t>(static_cast<int32_t>(from.x) + dx),
					static_cast<uint16_t>(static_cast<int32_t>(from.y) + dy),
					scanZ);
				auto tile = g_game().map.getTile(checkPos);
				if (!tile) continue;
				const uint8_t kind = shrineKindAt(tile);
				if (kind == 0) continue;
				// House hits are TAGGED, not dropped. A shrine inside a house still cannot be a
				// shrine-POI destination — that walk owns none of the door handling, tile claim,
				// botHouseMaxOccupants or, the one that actually strands a bot,
				// s_houseExitPlanner, which only endHouseVisit grants — but the CALLER needs to
				// know one is there. The forced command uses that to delegate into a house visit
				// instead of silently walking 22 tiles past a shrine 2 tiles away, which is
				// exactly what it did when this was a filter.
				const auto& house = tile->getHouse();
				const uint32_t hid = house ? house->getId() : 0;

				Cand c;
				c.houseId = hid;
				c.shrine = checkPos;
				c.dist = std::abs(dx) + std::abs(dy);
				if (scanZ != from.z) c.dist += SHRINE_CROSS_Z_PENALTY;

				for (const auto& [ox, oy] : kCardinals) {
					Position adjPos(
						static_cast<uint16_t>(static_cast<int32_t>(checkPos.x) + ox),
						static_cast<uint16_t>(static_cast<int32_t>(checkPos.y) + oy),
						checkPos.z);
					auto adjTile = g_game().map.getTile(adjPos);
					if (!adjTile || !adjTile->getGround()) continue;
					if (adjTile->hasFlag(TILESTATE_BLOCKSOLID)) continue;
					// NOT BLOCKPATH. An earlier version rejected it, reasoning that A* refuses such
					// tiles -- wrong, and measurably so. TILESTATE_BLOCKPATH is the protobuf `avoid`
					// flag (a routing preference), and `/cavebot route` proves our own pathfinder
					// walks ONTO a comfy chair and a side table and crosses other furniture on the
					// way. Rejecting them reported a shrine ringed by chairs as "boxed in" when a
					// bot could have reached it perfectly well.
					if (adjTile->hasFlag(TILESTATE_FLOORCHANGE)) continue; // A* won't path onto a stair
					if (adjTile->hasFlag(TILESTATE_TELEPORT)) continue;
					c.stand = adjPos;
					c.hasStand = true;
					// An occupied stand tile is a DEMOTION, not a rejection: the occupant may well
					// have moved on by the time this bot arrives, and rejecting here would make a
					// one-approach shrine permanently invisible the moment anyone stood on it.
					c.occupied = adjTile->getTopCreature() != nullptr;
					if (!c.occupied) break; // prefer a free cardinal; keep looking if this one is taken
				}
				if (!c.hasStand) {
					// A HOUSE shrine is NOT judged here. This function's stand test is cardinal-only
					// and knows nothing about house membership, while the house path resolves stands
					// with houseStandsAround/houseTileStandable — all eight sides, live, the same
					// predicates the dummy and locker modes use, and it accepts a diagonal. Rejecting
					// a house shrine on this weaker test meant the house code never got asked, so a
					// shrine it could actually have reached was reported "boxed in". Tag it and let
					// the owner of that question answer it.
					if (hid != 0) {
						if (!out.houseFound[kind - 1]) {
							out.houseSpot[kind - 1].shrine = checkPos;
							out.houseSpot[kind - 1].stand = checkPos; // resolved by the house path
							out.houseId[kind - 1] = hid;
							out.houseFound[kind - 1] = true;
							any = true;
						}
						continue;
					}
					// Non-house shrine with no usable side: genuinely unreachable, and worth saying
					// so rather than walking silently past it.
					if (!out.blockedFound[kind - 1]) {
						out.blockedPos[kind - 1] = checkPos;
						out.blockedFound[kind - 1] = true;
					}
					continue;
				}
				cands[kind - 1].push_back(c);
			}
		}
	}

	auto rank = [](std::vector<Cand>& v) {
		std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) {
			if (a.occupied != b.occupied) return !a.occupied; // unoccupied first
			return a.dist < b.dist;                           // then closest (cross-z penalised)
		});
	};
	for (int32_t k = 0; k < 2; k++) {
		// Partition rather than filter. Both halves are ranked by the same penalised distance, so
		// a caller that wants "nearest of either" can compare them directly.
		std::vector<Cand> plain, inHouse;
		for (const auto& c : cands[k]) {
			(c.houseId != 0 ? inHouse : plain).push_back(c);
		}
		if (!plain.empty()) {
			rank(plain);
			out.spot[k].shrine = plain.front().shrine;
			out.spot[k].stand = plain.front().stand;
			out.found[k] = true;
			any = true;
		}
		if (!inHouse.empty()) {
			rank(inHouse);
			out.houseSpot[k].shrine = inHouse.front().shrine;
			out.houseSpot[k].stand = inHouse.front().stand;
			out.houseId[k] = inHouse.front().houseId;
			out.houseFound[k] = true;
			any = true;
		}
	}
	return any;
}
