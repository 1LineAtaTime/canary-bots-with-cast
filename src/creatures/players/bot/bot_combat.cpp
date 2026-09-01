/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_combat.cpp — combat, spells/AoE/runes, PvP realism, gang-PK, random PK
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
// Gang-PK alpha-strike (Feature 1) — file-scope shared state (no ABI change).
// Wiped on reload; per-guid entries cleared at every member-teardown path via leaveGang().
// ============================================================================
// GangTileClaim / GangMember / GangSession and the s_gang* maps are hoisted to
// bot_engine_impl.hpp — the virtual simulator in bot_tick.cpp reads them too.

// True if any tile-occupant creature (incl. clientless bots — getCreatureCount sees them, unlike
// getTopVisibleCreature) stands on the tile.
static inline bool gangTileOccupied(const Position& p) {
	auto t = g_game().map.getTile(p);
	return t && t->getCreatureCount() > 0;
}









// ============================================================================
// Vocation helpers
// ============================================================================

uint8_t BotEngine::getBaseVocation(uint8_t vocId) const {
	// Promoted vocations: 5=master sorc, 6=elder druid, 7=royal paladin, 8=elite knight
	if (vocId >= 5 && vocId <= 8) return vocId - 4;
	if (vocId >= 1 && vocId <= 4) return vocId;
	return 4; // default knight
}

int32_t BotEngine::getAttackRange(uint8_t baseVoc) const {
	switch (baseVoc) {
		case 1: return 3; // sorcerer
		case 2: return 3; // druid
		case 3: return 5; // paladin
		case 4: return 1; // knight
		default: return 1;
	}
}

// ============================================================================
// Keep-distance helpers
// ============================================================================

int32_t BotEngine::getEffectiveKeepDistance(const BotState& bot) const {
	if (bot.huntScriptId == 0) return 0;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) {
			switch (getBaseVocation(bot.vocationId)) {
				case 1: return static_cast<int32_t>(s.keepDistanceMS);
				case 2: return static_cast<int32_t>(s.keepDistanceED);
				case 3: return static_cast<int32_t>(s.keepDistanceRP);
				case 4: return static_cast<int32_t>(s.keepDistanceEK);
				default: return 0;
			}
		}
	}
	return 0;
}

bool BotEngine::findThreatCentroid(BotState& bot, int32_t keepDist, Position& outThreatPos, int32_t& outNearestDist) {
	auto player = bot.getPlayer();
	if (!player) return false;

	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script) return false;

	// Uses cached spectators (target selection cadence, 600ms TTL).
	refreshSpectatorCacheIfStale(bot);

	int32_t nearestDist = 999;
	Position nearestPos;

	for (uint32_t mid : bot.cachedMonsterIds) {
		auto creature = g_game().getCreatureByID(mid);
		if (!creature || creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto cpos = creature->getPosition();
		if (cpos.z != bot.currentPos.z) continue;

		// Name match against hunt targets.
		// Empty targetNames means "attack all monsters" — only honored during PATROLLING,
		// not for quests, and not for traveling-category scripts (City Walks etc. — they
		// have empty targetNames + empty patrolWaypoints, so falling through to "attack
		// all" would farm wildlife indefinitely on the road — Fix #7).
		std::string monsterName = creature->getName();
		std::transform(monsterName.begin(), monsterName.end(), monsterName.begin(), ::tolower);
		bool match = false;
		if (script->targetNames.empty()) {
			match = (bot.huntPhase == HuntPhase::PATROLLING && !script->isQuest
			         && script->scriptCategory != "traveling");
		} else {
			for (const auto& t : script->targetNames) {
				if (monsterName == t) { match = true; break; }
			}
		}
		// Quest retaliation — a monster attacking the bot counts as a threat for keep-distance
		// purposes too, otherwise a mage would hold position against something actively hitting it.
		if (!match && botIsQuestRetaliationTarget(script, player->getID(), creature)) {
			match = true;
		}
		if (!match) continue;

		int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cpos.x)),
								std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cpos.y)));
		if (dist < keepDist && dist < nearestDist) {
			nearestDist = dist;
			nearestPos = cpos;
		}
	}

	if (nearestDist >= keepDist) return false;
	outThreatPos = nearestPos;
	outNearestDist = nearestDist;
	return true;
}

bool BotEngine::getRetreatStep(BotState& bot, const Position& threatPos, Direction& outDir) {
	auto player = bot.getPlayer();
	if (!player) return false;

	const Position& pos = bot.currentPos;
	int32_t ox = static_cast<int32_t>(pos.x) - static_cast<int32_t>(threatPos.x);
	int32_t oy = static_cast<int32_t>(pos.y) - static_cast<int32_t>(threatPos.y);

	// Build candidate directions that move AWAY from the threat
	std::vector<Direction> candidates;

	// Primary directions: move in the direction of the offset (away from threat)
	if (ox > 0) candidates.push_back(DIRECTION_EAST);
	else if (ox < 0) candidates.push_back(DIRECTION_WEST);
	if (oy > 0) candidates.push_back(DIRECTION_SOUTH);
	else if (oy < 0) candidates.push_back(DIRECTION_NORTH);

	// Diagonals that move away
	if (ox >= 0 && oy >= 0) candidates.push_back(DIRECTION_SOUTHEAST);
	if (ox >= 0 && oy <= 0) candidates.push_back(DIRECTION_NORTHEAST);
	if (ox <= 0 && oy >= 0) candidates.push_back(DIRECTION_SOUTHWEST);
	if (ox <= 0 && oy <= 0) candidates.push_back(DIRECTION_NORTHWEST);

	// On top of threat — try all
	if (ox == 0 && oy == 0) {
		candidates = {DIRECTION_NORTH, DIRECTION_SOUTH, DIRECTION_EAST, DIRECTION_WEST,
					  DIRECTION_NORTHEAST, DIRECTION_NORTHWEST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST};
	}

	// Shuffle for variety
	auto rng = std::mt19937(std::random_device{}());
	std::shuffle(candidates.begin(), candidates.end(), rng);

	for (auto dir : candidates) {
		Position newPos = pos;
		switch (dir) {
			case DIRECTION_NORTH: newPos.y--; break;
			case DIRECTION_SOUTH: newPos.y++; break;
			case DIRECTION_EAST: newPos.x++; break;
			case DIRECTION_WEST: newPos.x--; break;
			case DIRECTION_NORTHEAST: newPos.x++; newPos.y--; break;
			case DIRECTION_NORTHWEST: newPos.x--; newPos.y--; break;
			case DIRECTION_SOUTHEAST: newPos.x++; newPos.y++; break;
			case DIRECTION_SOUTHWEST: newPos.x--; newPos.y++; break;
			default: continue;
		}
		// Use same walkability check as A* pathfinding
		auto tile = g_game().map.canWalkTo(player, newPos);
		if (tile) {
			// Reject any step that would change z (FC tiles, teleports, AND height-based ramps)
			if (wouldChangeZ(pos, newPos)) continue;
			outDir = dir;
			return true;
		}
	}
	return false;
}


// ============================================================================
// Self-defense scan (Phase 3)
// ============================================================================

void BotEngine::doSelfDefense(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Handle ignored attacker (outleveled)
	if (bot.ignoredAttackerId > 0) {
		auto ignored = g_game().getCreatureByID(bot.ignoredAttackerId);
		if (!ignored || ignored->isRemoved() || ignored->getHealth() <= 0) {
			bot.ignoredAttackerId = 0;
			return;
		}
		auto target = ignored->getAttackedCreature();
		if (!target || target->getID() != player->getID()) {
			bot.ignoredAttackerId = 0;
			return;
		}
		// Hit back once (50% chance)
		if (!bot.ignoredHitBack) {
			bot.ignoredHitBack = true;
			if (uniform_random(1, 2) == 1) {
				castSpell(bot, ignored);
			}
		}
		return;
	}

	// Scan for players attacking us — uses cached spectators (target selection).
	// Cache TTL 600ms matches Gesior's b_possible_targets refresh cadence.
	std::shared_ptr<Creature> attacker;
	refreshSpectatorCacheIfStale(bot);
	int32_t bestDist = 999;
	for (uint32_t pid : bot.cachedPlayerIds) {
		auto creature = g_game().getCreatureByID(pid);
		if (!creature) continue;
		if (creature->getID() == player->getID()) continue;
		if (creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto p = creature->getPlayer();
		if (!p) continue;
		if (!p->isPzLocked()) continue;  // Stale target — PZ-lock expired, not actively PvPing
		auto target = creature->getAttackedCreature();
		if (!target || target->getID() != player->getID()) continue;
		auto cpos = creature->getPosition();
		if (cpos.z != bot.currentPos.z) continue;
		int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cpos.x)),
								std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cpos.y)));
		if (dist < bestDist) {
			bestDist = dist;
			attacker = creature;
		}
	}

	if (!attacker) {
		// No one targeting us in spectator range.
		// If already in combat/fleeing, stay while attacker still has PZ-lock (PVP flag)
		if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING) {
			if (bot.attackerId > 0) {
				auto storedAttacker = g_game().getCreatureByID(bot.attackerId);
				if (storedAttacker && !storedAttacker->isRemoved() && storedAttacker->getHealth() > 0) {
					auto atkPlayer = storedAttacker->getPlayer();
					if (atkPlayer && atkPlayer->isPzLocked()) {
						// Attacker still PZ-locked — stay in combat, chase stored attacker
						if (bot.state == BotAIState::COMBAT) {
							chaseTarget(bot, storedAttacker);
						}
						return;
					}
					// Attacker lost PZ-lock — exit combat
					castLog(bot, "COMBAT END: Attacker PZ-lock expired");
					exitCombat(bot);
					return;
				}
			}
			castLog(bot, "COMBAT END: Attacker dead or gone");
			exitCombat(bot);
		}
		return;
	}

	// Already in combat — update attacker and refresh "last seen" timestamp
	if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING) {
		bot.attackerId = attacker->getID();
		s_lastAttackerSeenTime[bot.guid] = OTSYS_TIME(); // Attacker confirmed in spectator range
		return;
	}

	// PKing — if our PK target retaliates, stay in PK_ATTACK (don't switch to self-defense)
	if (bot.state == BotAIState::PK_ATTACK && bot.pkTarget == attacker->getID()) {
		return;
	}

	// Remember if we were PKing (need to clean up if we fight/flee, but not if we ignore)
	bool wasPKing = (bot.state == BotAIState::PK_ATTACK);

	// Enter combat — make decision
	// Clear return walk on new combat entry
	s_returnPos.erase(bot.guid);
	s_returnStartTime.erase(bot.guid);

	bot.combatStartTime = OTSYS_TIME();
	bot.lastCombatProgress = OTSYS_TIME();
	bot.combatHpCheckTime = OTSYS_TIME();
	bot.combatHpBaseline = 0;
	bot.combatStalemateCount = 0;
	bot.attackerId = attacker->getID();
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	bot.preCombatPos = bot.currentPos;
	bot.hasPCPos = true;
	bot.pvpManaSpent = 0;

	// Level-based combat decision
	int32_t botLevel = player->getLevel();
	int32_t attackerLevel = 0;
	auto ap = attacker->getPlayer();
	if (ap) attackerLevel = ap->getLevel();

	bool outleveling = (attackerLevel > 0 && botLevel >= attackerLevel * 2);

	if (outleveling) {
		// 17% fight, 83% ignore
		if (uniform_random(1, 6) == 1) {
			bot.state = BotAIState::COMBAT;
			bot.combatDecision = "fight";
			tryEmitChat(bot, player, "combat", /*channelId=*/0);
		} else {
			bot.ignoredAttackerId = attacker->getID();
			bot.ignoredHitBack = false;
			bot.combatStartTime = 0;
			bot.attackerId = 0;
			return;
		}
	} else {
		// 50% fight, 50% flee
		if (uniform_random(1, 2) == 1) {
			bot.state = BotAIState::COMBAT;
			bot.combatDecision = "fight";
			tryEmitChat(bot, player, "combat", /*channelId=*/0);
		} else {
			bot.state = BotAIState::FLEEING;
			bot.combatDecision = "flee";
			// Clean flee-to-PZ state for fresh calculation
			bot.hasFleeTarget = false;
			bot.fleeDirectional = false;
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			tryEmitChat(bot, player, "flee", /*channelId=*/0);
		}
	}

	// Clean up PK state if we were PKing and decided to fight/flee (ignore already returned above)
	if (wasPKing) {
		castLog(bot, fmt::format("PK interrupted by {} — switching to self-defense ({})",
			attacker->getName(), bot.combatDecision));
		bot.pkTarget = 0;
	}

	castLog(bot, fmt::format("COMBAT: {} vs {} (lv{} vs lv{}) decision={}",
		player->getName(), attacker->getName(), botLevel, attackerLevel, bot.combatDecision));
}

// ============================================================================
// Combat state (Phase 3)
// ============================================================================

void BotEngine::doCombat(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	auto attacker = g_game().getCreatureByID(bot.attackerId);
	if (!attacker || attacker->isRemoved() || attacker->getHealth() <= 0) {
		castLog(bot, "COMBAT END: Attacker dead or gone");
		exitCombat(bot);
		return;
	}

	// 120s no-progress timeout — extend if attacker still PVP-flagged
	if (OTSYS_TIME() - bot.lastCombatProgress > 120000) {
		auto atkPlayer = attacker->getPlayer();
		if (atkPlayer && atkPlayer->isPzLocked()) {
			// Attacker still PVP-flagged — reset progress timer, keep fighting
			bot.lastCombatProgress = OTSYS_TIME();
			castLog(bot, "COMBAT: 120s no-progress but attacker still PVP-flagged — persisting");
		} else {
			castLogError(bot, "COMBAT END: 120s timeout (no progress)");
			exitCombat(bot);
			return;
		}
	}

	// HP stalemate detection: every 60s check if HP changed <10% for both sides
	int64_t now = OTSYS_TIME();
	if (now - bot.combatHpCheckTime >= 60000) {
		bot.combatHpCheckTime = now;
		int32_t botHpPct = player->getMaxHealth() > 0 ? (player->getHealth() * 100 / player->getMaxHealth()) : 100;
		int32_t atkHpPct = 100;
		if (attacker->getMaxHealth() > 0) {
			atkHpPct = attacker->getHealth() * 100 / attacker->getMaxHealth();
		}
		if (bot.combatHpBaseline > 0) {
			int32_t baselineBotHp = bot.combatHpBaseline >> 16;
			int32_t baselineAtkHp = bot.combatHpBaseline & 0xFFFF;
			bool botStale = std::abs(botHpPct - baselineBotHp) < 10;
			bool atkStale = std::abs(atkHpPct - baselineAtkHp) < 10;
			if (botStale && atkStale) {
				bot.combatStalemateCount++;
				if (bot.combatStalemateCount >= 5) {
					castLog(bot, "COMBAT END: 5min stalemate (no HP change)");
					exitCombat(bot);
					return;
				}
			} else {
				bot.combatStalemateCount = 0;
				bot.combatHpBaseline = (botHpPct << 16) | (atkHpPct & 0xFFFF);
			}
		} else {
			bot.combatHpBaseline = (botHpPct << 16) | (atkHpPct & 0xFFFF);
			bot.combatStalemateCount = 0;
		}
	}

	auto tpos = attacker->getPosition();

	// Check if attacker lost PZ-lock (PVP flag) — sole exit criterion
	auto combatAtkPlayer = attacker->getPlayer();
	bool combatAtkPzLocked = combatAtkPlayer && combatAtkPlayer->isPzLocked();
	if (!combatAtkPzLocked) {
		// Attacker lost PZ-lock — 3s grace period then exit
		auto ntIt = s_combatNoTargetSince.find(bot.guid);
		if (ntIt == s_combatNoTargetSince.end()) {
			s_combatNoTargetSince[bot.guid] = OTSYS_TIME();
			return;
		}
		if (OTSYS_TIME() - ntIt->second < 3000) {
			// During grace: still chase but don't attack (prevent white skull)
			chaseTarget(bot, attacker);
			return;
		}
		s_combatNoTargetSince.erase(bot.guid);
		castLog(bot, "COMBAT END: Attacker PZ-lock expired");
		exitCombat(bot);
		return;
	} else {
		s_combatNoTargetSince.erase(bot.guid);
	}

	// Z-level pursuit: target changed floors
	if (tpos.z != bot.currentPos.z) {
		if (bot.fcState == FloorChangeState::NONE) {
			// Cooldown: 1s between z-pursuit attempts (fast chase)
			auto pursuitIt = s_lastZPursuitTime.find(bot.guid);
			if (pursuitIt != s_lastZPursuitTime.end() && OTSYS_TIME() - pursuitIt->second < 1000) {
				return;
			}
			// No attempt limit — Lua had none. Protection = 120s no-progress + 5min stalemate timeouts
			bool goDown = tpos.z > bot.currentPos.z;
			castLog(bot, fmt::format("COMBAT Z-PURSUIT: Target at z={}, bot at z={}, going {}",
				tpos.z, bot.currentPos.z, goDown ? "down" : "up"));
			startFloorChange(bot, goDown, tpos);
			bot.lastCombatProgress = OTSYS_TIME();
			s_lastZPursuitTime[bot.guid] = OTSYS_TIME();
		}
		return;
	}

	// Track target's position while on same z (for transition selection if they change floors)
	s_targetLastSameZPos[bot.guid] = tpos;
	// Record target position in breadcrumb trail for door tracking
	s_targetTrail[bot.guid].add(tpos);

	// BOT_PVP_REALISM: dynamic flee — bail to FLEEING when low on HP (gesior parity:
	// a human runs instead of trading to the death). With haste + walls this is what
	// produces the emergent "running around" the fight.
	int32_t combatHpPct = player->getMaxHealth() > 0 ? (player->getHealth() * 100 / player->getMaxHealth()) : 100;
	if (combatHpPct <= pvpCfg_.fleeHpPct) {
		castLog(bot, fmt::format("COMBAT->FLEE: HP {}% <= {}%", combatHpPct, pvpCfg_.fleeHpPct));
		bot.state = BotAIState::FLEEING;
		bot.combatDecision = "flee";
		bot.hasFleeTarget = false;
		bot.fleeDirectional = false;
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		bot.combatStartTime = OTSYS_TIME();
		if (player->getAttackedCreature() == attacker) player->setAttackedCreature(nullptr);
		pvpCastBestHaste(bot, player);
		return;
	}

	// Same z-level: attack only if attacker still PZ-locked (prevents white skull on unflagged target)
	if (combatAtkPzLocked) {
		if (player->getAttackedCreature() != attacker) {
			player->setAttackedCreature(attacker);
		}
		// Haste mid-fight when hurt so we can keep pace / reposition.
		if (combatHpPct <= pvpCfg_.hasteHpPct) {
			pvpCastBestHaste(bot, player);
		}
		castSpell(bot, attacker);
		bot.lastPvpAttackTime = OTSYS_TIME();
		// Engaging magic wall: drop one beyond the target to deny a straight escape.
		pvpTryPlaceWall(bot, attacker, /*fleeing=*/false);
	} else {
		// Clear attack target — don't hit unflagged players
		if (player->getAttackedCreature() == attacker) {
			player->setAttackedCreature(nullptr);
		}
	}
	pvpReposition(bot, attacker);
	bot.lastCombatProgress = OTSYS_TIME();
}

void BotEngine::doFleeing(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	auto attacker = g_game().getCreatureByID(bot.attackerId);
	if (!attacker || attacker->isRemoved() || attacker->getHealth() <= 0) {
		castLog(bot, "FLEE END: Attacker dead or gone");
		exitCombat(bot);
		return;
	}

	// Check if attacker lost PZ-lock (PVP flag) — sole exit criterion
	auto fleeAttackerPlayer = attacker->getPlayer();
	bool attackerPzLocked = fleeAttackerPlayer && fleeAttackerPlayer->isPzLocked();
	if (!attackerPzLocked) {
		// Attacker lost PZ-lock — 3s grace period then exit
		auto ntIt = s_combatNoTargetSince.find(bot.guid);
		if (ntIt == s_combatNoTargetSince.end()) {
			s_combatNoTargetSince[bot.guid] = OTSYS_TIME();
			return;
		}
		if (OTSYS_TIME() - ntIt->second < 3000) {
			return; // Still in grace period — keep fleeing
		}
		s_combatNoTargetSince.erase(bot.guid);
		castLog(bot, "FLEE END: Attacker PZ-lock expired");
		exitCombat(bot);
		return;
	} else {
		s_combatNoTargetSince.erase(bot.guid);
	}

	// 30s flee timeout — extend if attacker still PVP-flagged
	if (OTSYS_TIME() - bot.combatStartTime > 30000) {
		auto attackerPlayer = attacker->getPlayer();
		if (attackerPlayer && attackerPlayer->isPzLocked()) {
			// Attacker still PVP-flagged — keep fleeing, reset timer
			bot.combatStartTime = OTSYS_TIME();
		} else {
			castLogError(bot, "FLEE END: 30s timeout, attacker PVP flag expired");
			exitCombat(bot);
			return;
		}
	}

	// Check if already in PZ — safe, just wait for timeout/flag expiry
	auto myTile = g_game().map.getTile(bot.currentPos);
	if (myTile && myTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
		return; // Safe in PZ, timeout or flag expiry will exit combat
	}

	// BOT_PVP_REALISM: active-flee behaviors, run every exposed tick.
	//  1) execute a pending vacated-tile wall (placed once we've stepped off the tile),
	//  2) haste to actually outrun the chaser,
	//  3) drop a magic wall between us and the chaser to break pursuit.
	pvpRunPendingWall(bot);
	pvpCastBestHaste(bot, player);
	if (attacker->getPosition().z == bot.currentPos.z) {
		pvpTryPlaceWall(bot, attacker, /*fleeing=*/true);
	}

	// If following a city route (flee-to-PZ or hunt return), continue
	if (bot.followingCityRoute) {
		if (!followCityRoute(bot)) {
			// Route completed — should be at or near PZ now
			castLog(bot, "FLEE: Reached PZ destination via route");
		}
		return;
	}

	// First time in flee without a target — determine flee path
	if (!bot.hasFleeTarget) {
		bot.hasFleeTarget = true;

		// Option 1: If hunting, use hunt's travelFromWaypoints to return to town
		if (bot.huntScriptId > 0) {
			const HuntScript* script = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { script = &s; break; }
			}
			if (script && !script->travelFromWaypoints.empty()) {
				// Load hunt return waypoints into city route system
				bot.cityRouteWps = script->travelFromWaypoints;
				bot.cityRouteIdx = 0;
				bot.followingCityRoute = true;
				castLog(bot, fmt::format("FLEE: Following {} hunt return waypoints to town",
					script->travelFromWaypoints.size()));
				// Release hunt reservation so other bots can use this spawn
				activeHunts_.erase(bot.huntScriptId);
				for (auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
						activeSpawnGroups_.erase(s.spawnGroup);
						break;
					}
				}
				bot.huntScriptId = 0;
				bot.huntTargetId = 0;
				return;
			}
		}

		// Option 2: Use city routes to nearest PZ (temple/depot/boat).
		// BOT_PVP_REALISM: a pz-locked bot physically cannot enter a PZ — routing there
		// just strands it at the edge. Skip the PZ route while locked and run+haste+wall
		// instead; once the lock expires this re-evaluates and heads to safety.
		bool pzBlocked = pvpCfg_.enablePzAwareFlee && isBotPzLocked(bot);
		if (pzBlocked) {
			castLog(bot, "FLEE: pz-locked — skipping PZ route, running until lock expires");
		}
		if (!pzBlocked && bot.townId > 0) {
			auto graphIt = cityRouteGraphs_.find(bot.townId);
			if (graphIt != cityRouteGraphs_.end()) {
				// Find the PZ destination with the closest route source
				std::string bestDest;
				int32_t bestSourceDist = INT32_MAX;
				for (const std::string& dst : {"temple", "depot", "boat"}) {
					std::string src = findBestRouteSource(bot.townId, bot.currentPos, dst, {});
					if (src.empty()) continue;
					auto poiIt = graphIt->second.pois.find(src);
					if (poiIt == graphIt->second.pois.end()) continue;
					int32_t d = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(poiIt->second.x))
						+ std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(poiIt->second.y))
						+ std::abs(static_cast<int32_t>(bot.currentPos.z) - static_cast<int32_t>(poiIt->second.z)) * 10;
					if (d < bestSourceDist) {
						bestSourceDist = d;
						bestDest = dst;
					}
				}
				if (!bestDest.empty() && startCityRoute(bot, "", bestDest)) {
					castLog(bot, fmt::format("FLEE: Navigating to PZ via city route to '{}'", bestDest));
					return;
				}
			}
		}

		// Option 3: No route available — fall back to directional flee
		castLog(bot, "FLEE: No PZ route found, using directional flee");
		bot.fleeDirectional = true;
	}

	// Directional flee fallback (original behavior)
	if (!player->listWalkDir.empty()) return;

	auto apos = attacker->getPosition();
	if (apos.z != bot.currentPos.z) return;

	int32_t dx = static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(apos.x);
	int32_t dy = static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(apos.y);
	int32_t dist = std::max(std::abs(dx), std::abs(dy));
	if (dist == 0) dist = 1;

	Position fleePos;
	fleePos.x = static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.x) + (dx * FLEE_DISTANCE / dist));
	fleePos.y = static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.y) + (dy * FLEE_DISTANCE / dist));
	fleePos.z = bot.currentPos.z;

	goTo(bot, fleePos);
}

void BotEngine::doPKAttack(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	auto target = g_game().getCreatureByID(bot.pkTarget);
	if (!target || target->isRemoved() || target->getHealth() <= 0) {
		castLog(bot, "PK: Target dead or gone");
		exitPK(bot);
		return;
	}

	// Hard max PK duration: 300s (5 minutes)
	if (OTSYS_TIME() - bot.combatStartTime > 300000) {
		castLog(bot, "PK END: 5min max duration reached");
		exitPK(bot);
		return;
	}

	// No-progress timeout (target unreachable, stuck, etc.)
	if (OTSYS_TIME() - bot.lastCombatProgress > PK_TIMEOUT * 1000) {
		castLogError(bot, "PK: Timeout, giving up");
		if (!s_gangByGuid.count(bot.guid)) { // gang members never solo-reengage their raid victim
			s_reengageTarget[bot.guid] = bot.pkTarget;
			s_reengageUntil[bot.guid] = OTSYS_TIME() + 60000;
		}
		exitPK(bot);
		return;
	}

	// HP stalemate detection: every 60s check if HP changed <10% for both sides
	int64_t now = OTSYS_TIME();
	if (now - bot.combatHpCheckTime >= 60000) {
		bot.combatHpCheckTime = now;
		int32_t botHpPct = player->getMaxHealth() > 0 ? (player->getHealth() * 100 / player->getMaxHealth()) : 100;
		int32_t tgtHpPct = 100;
		if (target->getMaxHealth() > 0) {
			tgtHpPct = target->getHealth() * 100 / target->getMaxHealth();
		}
		if (bot.combatHpBaseline > 0) {
			int32_t baselineBotHp = bot.combatHpBaseline >> 16;
			int32_t baselineTgtHp = bot.combatHpBaseline & 0xFFFF;
			bool botStale = std::abs(botHpPct - baselineBotHp) < 10;
			bool tgtStale = std::abs(tgtHpPct - baselineTgtHp) < 10;
			if (botStale && tgtStale) {
				bot.combatStalemateCount++;
				if (bot.combatStalemateCount >= 3) {
					castLog(bot, "PK END: 3min stalemate (no HP change)");
					exitPK(bot);
					return;
				}
			} else {
				bot.combatStalemateCount = 0;
				bot.combatHpBaseline = (botHpPct << 16) | (tgtHpPct & 0xFFFF);
			}
		} else {
			bot.combatHpBaseline = (botHpPct << 16) | (tgtHpPct & 0xFFFF);
			bot.combatStalemateCount = 0;
		}
	}

	auto tpos = target->getPosition();

	// PZ check.
	auto tile = g_game().map.getTile(tpos);
	if (tile && tile->hasFlag(TILESTATE_PROTECTIONZONE)) {
		// Gang member: the victim reached safety — it either DIED (respawned in the temple PZ) or
		// fled into a depot/temple. The raid is over: disband and do NOT camp the edge or re-engage
		// (the per-victim cooldown prevents re-jumping). Normal 1:1 PK still waits at the edge below.
		if (s_gangByGuid.count(bot.guid)) {
			s_reengageTarget.erase(bot.guid);
			s_reengageUntil.erase(bot.guid);
			castLog(bot, "GANG PK: victim reached a PZ (dead/fled) — disbanding");
			exitPK(bot); // calls leaveGang internally
			return;
		}
		if (!player->isPzLocked()) {
			castLog(bot, "PK: Our PVP flag expired while waiting at PZ");
			exitPK(bot);
			return;
		}
		// Keep progress timer fresh — don't let PK_TIMEOUT trigger while waiting
		bot.lastCombatProgress = OTSYS_TIME();
		return; // Wait at PZ edge, resume automatically when target exits PZ
	}

	// Exit PK if neither side has PZ-lock (fight is truly over)
	// Grace period: skip for first 5s so bot can land initial attack and create PZ-lock
	if (!player->isPzLocked() && OTSYS_TIME() - bot.combatStartTime > 5000) {
		auto targetPlayer = target->getPlayer();
		if (targetPlayer && !targetPlayer->isPzLocked()) {
			castLog(bot, "PK END: Neither side has PZ-lock, fight over");
			exitPK(bot);
			return;
		}
	}

	// Leash — extended range for PK commands (30 tiles)
	if (tpos.z == bot.currentPos.z) {
		int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
								std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y)));
		if (dist > 30) {
			castLogError(bot, "PK: Target too far, giving up");
			if (!s_gangByGuid.count(bot.guid)) { // gang members never solo-reengage their raid victim
				s_reengageTarget[bot.guid] = bot.pkTarget;
				s_reengageUntil[bot.guid] = OTSYS_TIME() + 60000;
			}
			exitPK(bot);
			return;
		}
	}

	// Z-level pursuit: target changed floors
	if (tpos.z != bot.currentPos.z) {
		if (bot.fcState == FloorChangeState::NONE) {
			// Cooldown: 1s between z-pursuit attempts (fast chase)
			auto pursuitIt = s_lastZPursuitTime.find(bot.guid);
			if (pursuitIt != s_lastZPursuitTime.end() && OTSYS_TIME() - pursuitIt->second < 1000) {
				return;
			}
			// No attempt limit — Lua had none. Protection = 120s no-progress + PK timeout
			bool goDown = tpos.z > bot.currentPos.z;
			castLog(bot, fmt::format("PK Z-PURSUIT: Target at z={}, bot at z={}, going {}",
				tpos.z, bot.currentPos.z, goDown ? "down" : "up"));
			startFloorChange(bot, goDown, tpos);
			bot.lastCombatProgress = OTSYS_TIME();
			s_lastZPursuitTime[bot.guid] = OTSYS_TIME();
		}
		return;
	}

	// Track target's position while on same z (for transition selection if they change floors)
	s_targetLastSameZPos[bot.guid] = tpos;
	// Record target position in breadcrumb trail for door tracking
	s_targetTrail[bot.guid].add(tpos);

	// Same rule as a normal player: no weapon auto-attack while standing in a protection zone, nor
	// against a target in one. Clear the engine attack-target if either is in a PZ (the bot keeps
	// repositioning out / waits at the edge) so it never deals weapon damage from a PZ.
	{
		auto selfTile = g_game().map.getTile(bot.currentPos);
		auto tgtTile = g_game().map.getTile(tpos);
		bool blockedByPz = (selfTile && selfTile->hasFlag(TILESTATE_PROTECTIONZONE))
			|| (tgtTile && tgtTile->hasFlag(TILESTATE_PROTECTIONZONE));
		if (blockedByPz) {
			if (player->getAttackedCreature()) player->setAttackedCreature(nullptr);
		} else if (player->getAttackedCreature() != target) {
			// Same z-level: set attacked creature for weapon auto-attacks
			player->setAttackedCreature(target);
			// chaseMode=true auto-sets followCreature which walks to dist=0.
			// For keepDist bots, clear follow — our chaseTarget handles positioning.
			int32_t kd = getEffectiveKeepDistance(bot);
			if (kd > 0) {
				player->setFollowCreature(nullptr);
			}
		}
	}

	castSpell(bot, target);
	// BOT_PVP_REALISM: as the aggressor, wall a fleeing victim's straight escape, and
	// reposition like a human (knights circle, distance vocs kite) instead of standing still.
	if (s_gangByGuid.count(bot.guid)) {
		// Gang member (Feature 1): coordinated escape-tile wall-box (one-bot-per-tile, never on an
		// ally's line of fire) + ED paralyze on a fleeing victim. Replaces the 1:1 single wall.
		maintainGangBox(bot);
		gangParalyzeIfFleeing(bot);
	} else {
		pvpTryPlaceWall(bot, target, /*fleeing=*/false);
	}
	pvpReposition(bot, target);

	// PZ-lock tracking (lastCombatProgress NOT reset here — let stalemate/timeout detect stuck fights)
	bot.lastPvpAttackTime = OTSYS_TIME();
}

// ============================================================================
// Combat helpers (Phase 3)
// ============================================================================

// ============================================================================
// BOT_LURE_KITE — kite-backtrack.
//
// A keep-distance bot that has run out of room (A* retreat found nothing, the
// single-step retreat failed, or the 15-tile waypoint drift cap refused the
// retreat) used to STAND STILL and tank the pack. It now retraces the patrol
// waypoints it has ALREADY WALKED — ground the bot demonstrably crossed, so it is
// walkable and on the right floor — ping-ponging over that stretch until the pack
// is dead, then resuming the patrol at the waypoint it is physically standing on.
//
// Three things make this safe rather than a new source of oscillation:
//   * while kiting, the kite IS the retreat — the stock PHASE-1 retreat is skipped,
//     so the two cannot fight each other for the same tick's movement;
//   * a direction is only taken if it INCREASES distance to the nearest threat, and
//     the burst path is rejected if it would pass through the threat's keep-distance
//     band — walking back into the pack at equal move speed is the obvious failure;
//   * both of scanAndAttackMonster's target-abandonment checks are suppressed while
//     kiting (see the comments there), which is exactly why the give-up exit arms
//     botKiteCooldownMs: without it a give-up could re-arm on the next tick and
//     starve those checks forever, leaving only the 5400s/12600s safety ceiling.
// ============================================================================

void BotEngine::endKiteBacktrack(BotState& bot, bool gaveUp, const char* reason) {
	auto it = s_kite.find(bot.guid);
	if (it == s_kite.end() || !it->second.active) return;
	auto& run = it->second;

	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}

	// Resume where the bot PHYSICALLY IS, not where it was when the kite started. The
	// cursor is the waypoint it last walked to, so the forward route from there is short
	// and known-walkable; jumping back to the pre-kite index would hand followWaypoints a
	// head tens of tiles away and let its 30s per-waypoint stuck timer eat the difference.
	// (The lookahead skip may then fast-forward through the re-walked stretch — benign,
	// and arguably what a player would do.)
	if (!gaveUp && script && run.cursor < script->patrolWaypoints.size()) {
		bot.huntWaypointIdx = run.cursor;
		bot.huntWaypointSkipCount = 0;
		// The per-waypoint stuck timer self-resets on an index change, but not if the
		// cursor happens to equal the index it already held.
		s_routeWpTimer.erase(bot.guid);
	}

	const int64_t cooldownUntil = gaveUp ? OTSYS_TIME() + lureCfg_.kiteCooldownMs : 0;
	castLog(bot, fmt::format("KITE: exit ({}) legs={} resumeWp={}", reason, run.legs,
		bot.huntWaypointIdx));
	g_logger().info("[BotEngine] KITE {} script={} legs={} elapsed={}s resumeWp={} reason={}",
		bot.name, bot.huntScriptId, run.legs,
		run.startMs > 0 ? (OTSYS_TIME() - run.startMs) / 1000 : 0, bot.huntWaypointIdx, reason);

	KiteRun fresh;
	fresh.cooldownUntilMs = cooldownUntil;
	run = fresh;
}

// Blocked in the current direction — either A* found nothing or the burst would dive into
// the pack. Reverse ONLY if the other end of the window actually improves our distance to
// the threat; otherwise there is nothing to gain by running into it, so give up and let the
// stock stand-and-fight take over (with the give-up cooldown armed, so the suppressed
// target-abandonment checks get their turn).
// Returns true if the run continues (caller owns the tick), false if the run ended.
bool BotEngine::kiteReverseOrGiveUp(BotState& bot, KiteRun& run,
                                    const std::vector<Waypoint>& wps, const Position& threatPos) {
	if (++run.pathFails < 2) return true; // one blocked tick is noise, not a decision
	run.pathFails = 0;
	const size_t alt = (run.dir < 0)
		? std::min(run.cursor + 1, run.anchorIdx)
		: (run.cursor > run.minIdx ? run.cursor - 1 : run.cursor);
	const auto& altWp = wps[std::min(alt, wps.size() - 1)];
	const int32_t curThreatDist = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(threatPos.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(threatPos.y)));
	const int32_t altThreatDist = std::max(
		std::abs(static_cast<int32_t>(altWp.pos.x) - static_cast<int32_t>(threatPos.x)),
		std::abs(static_cast<int32_t>(altWp.pos.y) - static_cast<int32_t>(threatPos.y)));
	if (alt == run.cursor || altThreatDist <= curThreatDist) {
		endKiteBacktrack(bot, /*gaveUp=*/true, "no_direction_improves");
		return false;
	}
	run.dir = static_cast<int8_t>(-run.dir);
	++run.legs;
	run.cursor = std::min(alt, wps.size() - 1);
	return true;
}

bool BotEngine::tryKiteBacktrack(BotState& bot, const Position& threatPos, int32_t keepDist) {
	if (!lureCfg_.kiteEnable) return false;
	auto player = bot.getPlayer();
	if (!player) return false;

	// Only a hunting bot on a patrol has a trail of already-walked waypoints to use.
	if (bot.state != BotAIState::HUNTING || bot.huntPhase != HuntPhase::PATROLLING) {
		clearKiteRun(bot.guid);
		return false;
	}
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script || script->patrolWaypoints.empty()) {
		clearKiteRun(bot.guid);
		return false;
	}
	const auto& wps = script->patrolWaypoints;

	auto& run = s_kite[bot.guid];
	const int64_t now = OTSYS_TIME();

	// Staleness stamp: every other condition in this gate is equally TRUE for a NEW hunt
	// on a DIFFERENT script (a virtual reroll during hibernation produces exactly that),
	// while the cursors would still index the OLD script's patrol — an out-of-bounds read
	// that no amount of phase checking would catch.
	if (run.active && (run.scriptId != bot.huntScriptId
	                   || run.cursor >= wps.size() || run.minIdx >= wps.size()
	                   || run.anchorIdx > wps.size())) {
		KiteRun fresh;
		fresh.cooldownUntilMs = run.cooldownUntilMs;
		run = fresh;
	}

	if (!run.active) {
		if (now < run.cooldownUntilMs) return false;
		// huntWaypointIdx == size is a real state: the lap-wrap reset lives BELOW the
		// target gate in doHuntPatrol, so the index stays one past the end for the whole
		// duration of a fight at the lap boundary. And at index 0 the already-walked
		// stretch belongs to the PREVIOUS lap, which this window model cannot represent.
		// Post-wrap kiting is therefore unavailable by design.
		if (bot.huntWaypointIdx == 0 || bot.huntWaypointIdx >= wps.size()) return false;

		// Build the window: walk backwards while the ground stays same-floor, is not a
		// floor-change/teleport waypoint (kiting through a ladder would drop the pack and
		// can strand the bot), and stays inside the span cap.
		size_t minIdx = bot.huntWaypointIdx;
		for (size_t i = bot.huntWaypointIdx; i-- > 0;) {
			const auto& w = wps[i];
			if (w.pos.z != bot.currentPos.z) break;
			if (isFloorChangeType(w.type) || w.isWalkOnFc) break;
			const int32_t d = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(w.pos.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(w.pos.y)));
			if (d > lureCfg_.kiteMaxSpanTiles) break;
			minIdx = i;
			if (bot.huntWaypointIdx - i >= static_cast<size_t>(std::max(1, lureCfg_.kiteDepthWps))) break;
		}
		// Fewer than two usable waypoints is not a corridor to run in — keep the old
		// stand-and-fight behaviour rather than inventing a one-tile shuffle.
		if (bot.huntWaypointIdx - minIdx < 2) return false;

		run = KiteRun {};
		run.active = true;
		run.scriptId = bot.huntScriptId;
		run.anchorIdx = bot.huntWaypointIdx;
		run.minIdx = minIdx;
		run.cursor = bot.huntWaypointIdx - 1;
		run.dir = -1;
		run.startMs = now;
		castLog(bot, fmt::format("KITE: enter window wp {}..{} (cursor {})",
			run.minIdx, run.anchorIdx, run.cursor));
	}

	// --- give-up bounds ---------------------------------------------------------------
	if (run.legs > static_cast<uint8_t>(std::max(1, lureCfg_.kiteMaxLegs))) {
		endKiteBacktrack(bot, /*gaveUp=*/true, "max_legs");
		return false;
	}
	if (now - run.startMs > lureCfg_.kiteMaxMs) {
		endKiteBacktrack(bot, /*gaveUp=*/true, "timeout");
		return false;
	}
	// Drifted off the corridor entirely (PHASE 3 approach can do this between threats).
	const auto& cursorWp = wps[run.cursor];
	int32_t distToCursor = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cursorWp.pos.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cursorWp.pos.y)));
	if (cursorWp.pos.z != bot.currentPos.z || distToCursor > lureCfg_.kiteMaxSpanTiles) {
		endKiteBacktrack(bot, /*gaveUp=*/true, "off_corridor");
		return false;
	}
	// No-progress watchdog. A kite that is not closing on its cursor is not kiting, and
	// riding the full kiteMaxMs to find that out costs 45s of suppressed target
	// abandonment for nothing (measured live: most give-ups were `legs=0 timeout`).
	// Anything stalled this long has already been through the reversal logic twice.
	if (run.bestDistMs == 0 || distToCursor < run.bestDist) {
		run.bestDist = static_cast<int32_t>(distToCursor);
		run.bestDistMs = now;
	} else if (now - run.bestDistMs > KITE_NO_PROGRESS_MS) {
		endKiteBacktrack(bot, /*gaveUp=*/true, "no_progress");
		return false;
	}

	// --- advance the cursor when we reach it, ping-ponging inside the window ----------
	if (distToCursor <= 1) {
		size_t next = run.cursor;
		if (run.dir < 0) {
			if (run.cursor > run.minIdx) {
				next = run.cursor - 1;
			} else {
				run.dir = 1;
				++run.legs;
				next = std::min(run.cursor + 1, run.anchorIdx);
			}
		} else {
			if (run.cursor + 1 <= run.anchorIdx) {
				next = run.cursor + 1;
			} else {
				run.dir = -1;
				++run.legs;
				next = run.cursor > run.minIdx ? run.cursor - 1 : run.cursor;
			}
		}
		run.cursor = std::min(next, wps.size() - 1);
		// New cursor, new baseline: the watchdog measures progress toward the waypoint
		// currently being walked to, and every advance legitimately increases the distance.
		run.bestDistMs = 0;
		distToCursor = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wps[run.cursor].pos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wps[run.cursor].pos.y)));
	}

	// Don't interrupt an in-flight walk, and honour the shared retreat cooldown so
	// spells still get their tick between bursts (same contract as the stock retreat).
	if (!player->listWalkDir.empty()) return true;
	const auto rIt = s_retreatUntil.find(bot.guid);
	if (rIt != s_retreatUntil.end() && now < rIt->second) return true;

	// --- walk a 2-step burst toward the cursor ----------------------------------------
	const Position target = wps[run.cursor].pos;
	FindPathParams fpp;
	fpp.fullPathSearch = false;
	fpp.clearSight = false;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = 12;
	fpp.minTargetDist = 0;
	fpp.maxTargetDist = 1;

	std::vector<Direction> dirList;
	if (!g_game().map.getPathMatching(player, target, dirList,
			FrozenPathingConditionCall(target), fpp)) {
		if (kiteReverseOrGiveUp(bot, run, wps, threatPos)) return true;
		return false;
	}
	// NOTE: pathFails is NOT reset here. A path can be found and then rejected below
	// for diving into the pack, and resetting on "A* succeeded" would zero the counter
	// every tick — so the rejection path could never reach its reversal threshold, which
	// is the stand-still bug re-created through a different door. It resets only once a
	// walk is actually issued.

	size_t steps = std::min(dirList.size(), static_cast<size_t>(2));
	dirList.resize(steps);
	// Same FC guard the stock retreat uses: the window excludes floor-change WAYPOINTS,
	// but the A* path between two same-z waypoints can still cross a floor-change TILE.
	if (hasFloorChangeTileInPath(bot.currentPos, dirList)) {
		endKiteBacktrack(bot, /*gaveUp=*/true, "fc_tile_in_path");
		return false;
	}
	// Reject a burst that would walk us THROUGH the pack. Simulated step by step, because a
	// two-step path can dive past a monster and come out the far side looking fine at both
	// endpoints.
	//
	// The threshold is RELATIVE to where we already stand, not an absolute keep-distance
	// band. An absolute band is unsatisfiable exactly when the kite matters most: a bot
	// cornered at distance 1 with keepDist 3 has every escape step land at distance 2,
	// inside the band, so every burst was rejected and the bot stood still for the full
	// 45s timeout while believing it was kiting. Measured live 2026-08-20: 8 of 12 runs
	// ended `legs=0 reason=timeout`. Allowing any step that does not bring us CLOSER than
	// we already are keeps the "don't dive past the monster" guarantee and still lets a
	// cornered bot leave.
	{
		Position sim = bot.currentPos;
		const int32_t band = std::max(1, keepDist > 0 ? keepDist - 1 : 1);
		const int32_t startDist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(threatPos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(threatPos.y)));
		const int32_t floorDist = std::min(band, startDist);
		for (const auto d : dirList) {
			sim = getNextPosition(d, sim);
			const int32_t td = std::max(
				std::abs(static_cast<int32_t>(sim.x) - static_cast<int32_t>(threatPos.x)),
				std::abs(static_cast<int32_t>(sim.y) - static_cast<int32_t>(threatPos.y)));
			if (sim.z == threatPos.z && td < floorDist) {
				// This way dives into the pack. Treat it exactly like a failed path — the
				// reversal logic below is what decides whether the other direction is any
				// better, and gives up if neither is. Returning here without doing that was
				// the second half of the stand-still bug: pathFails grew forever and nothing
				// ever reconsidered the direction.
				if (kiteReverseOrGiveUp(bot, run, wps, threatPos)) return true;
				return false;
			}
		}
	}

	run.pathFails = 0; // a real step was taken; earlier blocked/rejected ticks are history
	botStartAutoWalk(bot, player, dirList);
	// Recording progress is not cosmetic: scanAndAttackMonster's Check 1 fires whenever
	// lastCombatProgress did not change this tick, and the stock retreat sets it for
	// exactly this reason.
	bot.lastCombatProgress = now;
	s_retreatUntil[bot.guid] = now + static_cast<int64_t>(steps) * 300LL + 300LL;
	if (bot.tickCounter % 10 == 0) {
		castLog(bot, fmt::format("KITE: {} steps -> wp {} ({},{},{}) dir={} legs={}",
			steps, run.cursor, target.x, target.y, target.z, static_cast<int>(run.dir), run.legs));
	}
	return true;
}

void BotEngine::chaseTarget(BotState& bot, const std::shared_ptr<Creature>& target) {
	auto player = bot.getPlayer();
	if (!player) return;

	auto tpos = target->getPosition();
	if (tpos.z != bot.currentPos.z) return; // different floor, skip

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	int32_t attackRange = getAttackRange(baseVoc);
	int32_t keepDist = getEffectiveKeepDistance(bot);

	int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y)));

	// === KEEP-DISTANCE: UNIFIED KITING ===
	// Three phases: (1) Retreat from ANY nearby monster too close,
	//               (2) Deadband — stay put if in [keepDist, keepDist+1] from target,
	//               (3) Approach target if too far (bot must get into attack range).
	if (keepDist > 0) {
		int32_t safeMax = std::max(keepDist + 1, attackRange);

		// CRITICAL: chaseMode=true causes setAttackedCreature to auto-set followCreature,
		// which walks the bot to dist=0 and completely overrides our keepDistance logic.
		if (player->getFollowCreature()) {
			player->setFollowCreature(nullptr);
		}

		// Don't interrupt active walking — let current steps complete
		if (!player->listWalkDir.empty()) return;

		// After a retreat burst, pause 1 tick to allow spell casting
		auto retreatIt = s_retreatUntil.find(bot.guid);
		if (retreatIt != s_retreatUntil.end() && OTSYS_TIME() < retreatIt->second) {
			return; // cooldown active — stay put, let spells fire
		}
		s_retreatUntil.erase(bot.guid);

		// === PHASE 1: Check ALL nearby monsters for threats (not just attacked target) ===
		// This fixes bots only keeping distance from the targeted monster while
		// another monster walks right up to dist=0.
		// Uses cached spectators (target selection cadence, 600ms TTL).
		Position nearestThreatPos;
		int32_t nearestThreatDist = 999;
		refreshSpectatorCacheIfStale(bot);
		for (uint32_t mid : bot.cachedMonsterIds) {
			auto creature = g_game().getCreatureByID(mid);
			if (!creature || creature->isRemoved() || creature->getHealth() <= 0) continue;
			auto cpos = creature->getPosition();
			if (cpos.z != bot.currentPos.z) continue;
			int32_t d = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cpos.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cpos.y)));
			if (d < keepDist && d < nearestThreatDist) {
				nearestThreatDist = d;
				nearestThreatPos = cpos;
			}
		}

		bool hasThreat = (nearestThreatDist < keepDist);
		if (hasThreat) {
			auto kIt = s_kite.find(bot.guid);
			if (kIt != s_kite.end()) kIt->second.clearTicks = 0;
		}

		if (hasThreat) {
			// A monster is too close — retreat takes priority over everything

			// BOT_LURE_KITE: if a kite run is already live, IT is the retreat. Checked
			// before the drift cap and before the A* retreat so the two cannot fight over
			// the same tick's movement — the stock retreat pulls toward "away from the
			// threat", the kite toward "back along the trail", and alternating between
			// them is how the bot would end up never reaching a cursor waypoint at all.
			if (botIsKiting(bot.guid)) {
				if (tryKiteBacktrack(bot, nearestThreatPos, keepDist)) return;
				// Kite just ended (gave up); fall through to the stock retreat below.
			}

			// Waypoint drift cap: don't kite >15 tiles from patrol waypoint
			if (bot.state == BotAIState::HUNTING && bot.huntPhase == HuntPhase::PATROLLING) {
				const HuntScript* script = nullptr;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { script = &s; break; }
				}
				if (script && bot.huntWaypointIdx < script->patrolWaypoints.size()) {
					auto wpPos = script->patrolWaypoints[bot.huntWaypointIdx].pos;
					int32_t wpDist = std::max(
						std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wpPos.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wpPos.y)));
					if (wpDist > 15) {
						// BOT_LURE_KITE: drifted too far to retreat further, which used to
						// mean "stand here and take it". Running back along the waypoints
						// already walked is both an escape and a way back onto the route.
						if (tryKiteBacktrack(bot, nearestThreatPos, keepDist)) return;
						if (bot.tickCounter % 30 == 0) {
							castLog(bot, fmt::format("KEEPDIST: Too far from waypoint ({}), not retreating", wpDist));
						}
						return;
					}
				}
			}

			// A* retreat from nearest threat to [keepDist, safeMax] band
			FindPathParams fpp;
			fpp.fullPathSearch = false;
			fpp.clearSight = true;
			fpp.allowDiagonal = true;
			fpp.keepDistance = true;
			fpp.maxSearchDist = 12;
			fpp.minTargetDist = keepDist;
			fpp.maxTargetDist = safeMax;

			std::vector<Direction> dirList;
			if (g_game().map.getPathMatching(player, nearestThreatPos, dirList,
					FrozenPathingConditionCall(nearestThreatPos), fpp)) {
				size_t stepsToTake = std::min(dirList.size(), static_cast<size_t>(2));
				dirList.resize(stepsToTake);
				if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
				botStartAutoWalk(bot, player,dirList);
				bot.lastCombatProgress = OTSYS_TIME();
				s_retreatUntil[bot.guid] = OTSYS_TIME() + static_cast<int64_t>(stepsToTake) * 300LL + 300LL;
				if (bot.tickCounter % 10 == 0) {
					castLog(bot, fmt::format("KEEPDIST: Retreat {} steps from ({},{},{}) dist={} (bot=({},{},{}) kd={})",
						stepsToTake,
						nearestThreatPos.x, nearestThreatPos.y, nearestThreatPos.z, nearestThreatDist,
						bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, keepDist));
				}
				return;
				}
			}

			// A* failed — try single-step retreat
			Direction retreatDir;
			if (getRetreatStep(bot, nearestThreatPos, retreatDir)) {
				botStartAutoWalk(bot, player,{retreatDir});
				bot.lastCombatProgress = OTSYS_TIME();
				s_retreatUntil[bot.guid] = OTSYS_TIME() + 600LL;
				return;
			}
			// Cornered: A* found no retreat and no single step helps. Before accepting
			// the position, try running back along the patrol waypoints already walked —
			// that ground is known-walkable and leads away from the spawn.
			if (tryKiteBacktrack(bot, nearestThreatPos, keepDist)) return;

			// Cornered — accept position, keep attacking
			return;
		}

		// BOT_LURE_KITE: no threat inside keepDist any more. Give the pack a few ticks to
		// prove it is really gone (a monster stepping in and out of the band must not end
		// the run), then resume the patrol at the waypoint the bot is standing on.
		if (botIsKiting(bot.guid)) {
			auto& kr = s_kite[bot.guid];
			if (++kr.clearTicks >= 3) {
				endKiteBacktrack(bot, /*gaveUp=*/false, "threat_clear");
			} else {
				return; // hold position this tick rather than starting a fresh approach
			}
		}

		// === PHASE 2: Deadband check — in safe band with LOS, stay put ===
		bool hasLOS = g_game().map.isSightClear(bot.currentPos, tpos, true);
		if (dist >= keepDist && dist <= safeMax && hasLOS) {
			s_inRangeSince.erase(bot.guid);
			s_inRangeAttackSnapshot.erase(bot.guid);
			return;
		}

		// === PHASE 3: Approach target if too far (or no LOS) ===
		// The bot must get within [keepDist, safeMax] of the target to attack.
		// Use fullPathSearch + larger maxSearchDist for reliable approach.
		{
			FindPathParams fpp;
			fpp.fullPathSearch = true; // find optimal path, not just first match
			fpp.clearSight = true;
			fpp.allowDiagonal = true;
			fpp.keepDistance = true;
			fpp.maxSearchDist = PATH_MAX_DIST;
			fpp.minTargetDist = keepDist;
			fpp.maxTargetDist = safeMax;

			std::vector<Direction> dirList;
			if (g_game().map.getPathMatching(player, tpos, dirList,
					FrozenPathingConditionCall(tpos), fpp)) {
				// Approach: take up to 4 steps since we're far and no nearby threats
				size_t stepsToTake = std::min(dirList.size(), static_cast<size_t>(4));
				dirList.resize(stepsToTake);
				if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
					botStartAutoWalk(bot, player,dirList);
					bot.lastCombatProgress = OTSYS_TIME();
					if (bot.tickCounter % 10 == 0) {
						castLog(bot, fmt::format("KEEPDIST: Approach {} steps to ({},{},{}) dist={} (bot=({},{},{}) kd={})",
							stepsToTake, tpos.x, tpos.y, tpos.z, dist,
							bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, keepDist));
					}
					return;
				}
			}

			// Fallback: retry without clearSight (may be behind obstacle)
			fpp.clearSight = false;
			dirList.clear();
			if (g_game().map.getPathMatching(player, tpos, dirList,
					FrozenPathingConditionCall(tpos), fpp)) {
				size_t stepsToTake = std::min(dirList.size(), static_cast<size_t>(4));
				dirList.resize(stepsToTake);
				if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
					botStartAutoWalk(bot, player,dirList);
					bot.lastCombatProgress = OTSYS_TIME();
					return;
				}
			}
		}
		return; // keepDist > 0 always returns here, never falls through to melee code
	}

	// === MELEE / NON-KEEPDIST LOGIC (keepDist == 0) ===
	bool inRangeWithSight = dist <= attackRange &&
		g_game().map.isSightClear(bot.currentPos, tpos, true);
	if (inRangeWithSight) {
		if (dist <= 1) {
			s_inRangeSince.erase(bot.guid);
			s_inRangeAttackSnapshot.erase(bot.guid);
			return;
		}
		// Ranged distance: track whether spells are landing (detect wall/window blocking)
		int64_t now = OTSYS_TIME();
		auto rangeIt = s_inRangeSince.find(bot.guid);
		if (rangeIt == s_inRangeSince.end()) {
			s_inRangeSince[bot.guid] = now;
			s_inRangeAttackSnapshot[bot.guid] = bot.lastAttackTime;
			return;
		}
		bool attacked = bot.lastAttackTime > s_inRangeAttackSnapshot[bot.guid];
		if (attacked) {
			s_inRangeSince[bot.guid] = now;
			s_inRangeAttackSnapshot[bot.guid] = bot.lastAttackTime;
			return;
		}
		if (now - rangeIt->second < 3000) {
			return;
		}
		castLog(bot, "CHASE STALE: Forcing melee approach");
	} else {
		s_inRangeSince.erase(bot.guid);
		s_inRangeAttackSnapshot.erase(bot.guid);
	}

	// Don't interrupt active walking
	if (!player->listWalkDir.empty()) return;

	// Stale-range override (only when keepDist == 0)
	bool staleRange = s_inRangeSince.count(bot.guid) > 0 &&
		OTSYS_TIME() - s_inRangeSince[bot.guid] >= 3000 &&
		bot.lastAttackTime <= s_inRangeAttackSnapshot[bot.guid];

	// Pathfind to target
	FindPathParams fpp;
	fpp.fullPathSearch = true;
	fpp.clearSight = staleRange ? false : true;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = PATH_MAX_DIST;
	fpp.minTargetDist = 0;
	fpp.maxTargetDist = staleRange ? 1 : attackRange;

	std::vector<Direction> dirList;
	if (g_game().map.getPathMatching(player, tpos, dirList, FrozenPathingConditionCall(tpos), fpp)) {
		if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
			botStartAutoWalk(bot, player,dirList);
			bot.lastCombatProgress = OTSYS_TIME();
		}
	} else {
		// Fallback: retry without clearSight (Lua pattern)
		fpp.clearSight = false;
		dirList.clear();
		if (g_game().map.getPathMatching(player, tpos, dirList, FrozenPathingConditionCall(tpos), fpp)) {
			if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
				botStartAutoWalk(bot, player,dirList);
				bot.lastCombatProgress = OTSYS_TIME();
			}
		} else {
			// Pathfinding fully failed — try doors then blocker
			// 1. Trail-based: check if target walked through a door that's now closed
			if (tryOpenDoorsOnTrail(bot, player)) {
				// Door found on trail — opened or walking to it
			}
			// 2. Directional: check tiles between bot and target for closed doors
			else if (tryOpenDoors(bot, player, tpos)) {
				// Door opened in direction of target
			}
			// 3. Blocker: try attacking blocking monster
			else {
				tryAttackBlockingMonster(bot);
			}
		}
	}
}

void BotEngine::castSpell(BotState& bot, const std::shared_ptr<Creature>& target) {
	auto player = bot.getPlayer();
	if (!player || !target) return;

	// Same rule as a normal player: you cannot perform a hostile action while standing in a
	// protection zone, nor against a target standing in one. Gate ALL bot spell/rune attacks on it
	// so a bot never attacks from (or into) a PZ — applies to every combat state, not just gangs.
	{
		auto selfTile = g_game().map.getTile(bot.currentPos);
		if (selfTile && selfTile->hasFlag(TILESTATE_PROTECTIONZONE)) return;
		auto tgtTile = g_game().map.getTile(target->getPosition());
		if (tgtTile && tgtTile->hasFlag(TILESTATE_PROTECTIONZONE)) return;
	}

	// Minimum interval between spell attempts (prevents spam when server rejects spell)
	int64_t now = OTSYS_TIME();
	if (bot.lastAttackTime > 0 && now - bot.lastAttackTime < 2000) {
		return;
	}

	// Gang-PK single-target lock + nuke-hold (Feature 1). A gang member only ever nukes the
	// victim with single-target spells/runes (no AoE -> no collateral on nearby bots/players).
	// In engage-then-trap mode (openMode 1) nukes are held until the trap is up.
	//
	// A fishing bot defending its shore joins the same latch. The AoE bystander check further
	// down is gated on `targetIsPlayer`, so a MONSTER-target wave or GFB goes out with no check
	// at all — fine in a remote spawn, not fine at a town shoreline, which is the most
	// bystander-dense place a bot ever fights. Clipping one real player on an open-PvP world
	// means an unjustified attack, a white skull and a PZ-lock on a bot that was only fishing.
	const bool gangSingleTarget = s_gangNoAoe.count(bot.guid) > 0 || isFishDefending(bot.guid);
	if (gangSingleTarget) {
		auto sgIt = s_gangByGuid.find(bot.guid);
		if (sgIt != s_gangByGuid.end()) {
			auto seIt = s_gangSessions.find(sgIt->second);
			if (seIt != s_gangSessions.end() && now < seIt->second.nukeHoldUntil) return; // trap-first hold
		}
	}

	// Suppress all attack attempts during z-change grace period
	auto zIt = s_lastZChangeTime.find(bot.guid);
	if (zIt != s_lastZChangeTime.end() && now - zIt->second < Z_CHANGE_GRACE_MS) {
		if (now - zIt->second < 100) {
			castLog(bot, fmt::format("Z_GRACE: Suppressing attacks for {}ms after floor change (z={}->{})",
				Z_CHANGE_GRACE_MS, bot.lastPos.z, bot.currentPos.z));
		}
		return;
	}

	auto tpos = target->getPosition();
	if (tpos.z != bot.currentPos.z) return;
	if (!g_game().map.isSightClear(bot.currentPos, tpos, true)) return;

	int32_t level = player->getLevel();
	int32_t mlevel = player->getMagicLevel();
	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	bool targetIsPlayer = target->getPlayer() != nullptr;

	// Runes for all vocations except knights (SD further restricted to MS/ED)
	bool canUseRunes = (baseVoc != 4);

	// ================================================================
	// Unified scoring: evaluate ALL attack options, pick highest total damage
	// ================================================================
	enum AttackType : uint8_t { NONE, AOE_SPELL, AOE_RUNE, SINGLE_SPELL, SD_RUNE };
	AttackType bestType = NONE;
	double bestScore = 0;

	// Option data for the winner
	const ResolvedSpell* winnerAoeSpell = nullptr;
	Direction winnerAoeDir = DIRECTION_NORTH;
	const ResolvedSpell* winnerSingleSpell = nullptr;
	uint16_t winnerRuneId = 0;
	Position winnerRunePos;

	// --- Option 1: AoE spells --- (skipped entirely for gang members — single-target only)
	if (!gangSingleTarget) {
		Direction aoeDir = DIRECTION_NORTH;
		double aoeScore = 0;
		const ResolvedSpell* aoeSpell = selectAoeSpell(bot, aoeDir, nullptr, &aoeScore,
			targetIsPlayer ? target : nullptr);
		// BOT_PVP_REALISM: bias AoE/wave higher vs a lone player so bots actually throw
		// waves/AoE for flair instead of only SD. The bystander-safety fallback further
		// below still prevents hitting other bots / innocent players.
		if (aoeSpell && targetIsPlayer && pvpCfg_.enableAoeBias) {
			aoeScore *= (1.0 + static_cast<double>(pvpCfg_.aoeBiasPct) / 100.0);
		}
		if (aoeSpell && aoeScore > bestScore) {
			bestScore = aoeScore;
			bestType = AOE_SPELL;
			winnerAoeSpell = aoeSpell;
			winnerAoeDir = aoeDir;
		}
	}

	// --- Option 2: AoE runes (dynamically resolved from Lua files) --- (skip for gang members)
	if (canUseRunes && !gangSingleTarget) {
		for (const auto& rune : resolvedAoeRunes_) {
			auto runeItem = findRuneInBackpack(player, rune.runeId);
			if (!runeItem) continue;

			// Check rune level/maglevel requirements + cooldowns via server spell system
			auto runeSpell = g_spells().getRuneSpell(rune.runeId);
			if (runeSpell) {
				if (level < static_cast<int32_t>(runeSpell->getLevel())) continue;
				if (static_cast<uint32_t>(mlevel) < runeSpell->getMagicLevel()) continue;
				if (player->hasCondition(CONDITION_SPELLCOOLDOWN, runeSpell->getSpellId())) continue;
				if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, runeSpell->getGroup())) continue;
			}

			auto [pos, count, weightedCount] = findBestRunePosition(bot, rune.combatType);
			// For PvP: if no monsters but target player is in range, use their position
			if (count < RUNE_AOE_MIN_TARGETS && targetIsPlayer && target) {
				auto tpos = target->getPosition();
				int32_t tdist = std::max(
					std::abs(static_cast<int32_t>(tpos.x) - static_cast<int32_t>(bot.currentPos.x)),
					std::abs(static_cast<int32_t>(tpos.y) - static_cast<int32_t>(bot.currentPos.y)));
				if (tpos.z == bot.currentPos.z && tdist <= RUNE_RANGE &&
					g_game().map.isSightClear(bot.currentPos, tpos, true)) {
					pos = tpos;
					count = 1;
					weightedCount = 1.0;
				}
			}
			if (count < RUNE_AOE_MIN_TARGETS) continue;

			double dmg = estimateDamage(level, mlevel, rune.avgMlCoef, rune.avgConst);
			double score = dmg * weightedCount;
			if (targetIsPlayer && pvpCfg_.enableAoeBias) {
				score *= (1.0 + static_cast<double>(pvpCfg_.aoeBiasPct) / 100.0);
			}
			if (score > bestScore) {
				bestScore = score;
				bestType = AOE_RUNE;
				winnerRuneId = rune.runeId;
				winnerRunePos = pos;
			}
		}
	}

	// --- Option 3: Single-target spells ---
	double singleScore = 0;
	const ResolvedSpell* singleSpell = selectAttackSpell(bot, target, &singleScore);
	if (singleSpell && singleScore > bestScore) {
		bestScore = singleScore;
		bestType = SINGLE_SPELL;
		winnerSingleSpell = singleSpell;
	}

	// --- Option 4: SD rune ---
	if (canUseRunes && resolvedSdRune_.runeId > 0 && (baseVoc == 1 || baseVoc == 2)) {
		auto sdItem = findRuneInBackpack(player, resolvedSdRune_.runeId);
		if (sdItem) {
			auto sdSpell = g_spells().getRuneSpell(resolvedSdRune_.runeId);
			bool sdReady = true;
			if (sdSpell) {
				if (level < static_cast<int32_t>(sdSpell->getLevel())) sdReady = false;
				if (static_cast<uint32_t>(mlevel) < sdSpell->getMagicLevel()) sdReady = false;
				if (player->hasCondition(CONDITION_SPELLCOOLDOWN, sdSpell->getSpellId())) sdReady = false;
				if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, sdSpell->getGroup())) sdReady = false;
			}
			int32_t sdResist = getElementResistance(target, resolvedSdRune_.combatType);
			if (sdReady && sdResist < 50) {
				int32_t dist = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y)));
				if (dist <= RUNE_RANGE) {
					double sdDmg = estimateDamage(level, mlevel, resolvedSdRune_.avgMlCoef, resolvedSdRune_.avgConst);
					sdDmg *= (100.0 - static_cast<double>(sdResist)) / 100.0;
					if (sdDmg > bestScore) {
						bestScore = sdDmg;
						bestType = SD_RUNE;
					}
				}
			}
		}
	}

	// PvP AOE safety: if AOE would hit non-target players/bots, fall back to single-target
	if (targetIsPlayer && (bestType == AOE_SPELL || bestType == AOE_RUNE)) {
		bool hasBystander = false;
		auto playerSpectators = Spectators().find<Player>(bot.currentPos, false, 7, 7, 7, 7);
		for (const auto& p : playerSpectators) {
			if (p.get() == player.get()) continue;
			if (p.get() == target.get()) continue;
			if (p->getHealth() <= 0) continue;
			auto ppos = p->getPosition();
			if (ppos.z != bot.currentPos.z) continue;
			bool inAoe = false;
			if (bestType == AOE_SPELL && winnerAoeSpell) {
				inAoe = spellHits(bot.currentPos, ppos, *winnerAoeSpell, winnerAoeDir);
			} else if (bestType == AOE_RUNE) {
				int32_t rdist = std::max(
					std::abs(static_cast<int32_t>(ppos.x) - static_cast<int32_t>(winnerRunePos.x)),
					std::abs(static_cast<int32_t>(ppos.y) - static_cast<int32_t>(winnerRunePos.y)));
				inAoe = (rdist <= 3);
			}
			if (inAoe) { hasBystander = true; break; }
		}
		if (hasBystander) {
			if (winnerSingleSpell) {
				bestType = SINGLE_SPELL;
			} else if (resolvedSdRune_.runeId > 0 && (baseVoc == 1 || baseVoc == 2)) {
				bestType = SD_RUNE;
			} else {
				bestType = NONE;
			}
		}
	}

	// ================================================================
	// Execute the winning option
	// ================================================================
	if (bestType == NONE) return;

	switch (bestType) {
		case AOE_SPELL: {
			// Debug: capture affected tiles + targets pre-cast
			if (auto* dcfg = getDebugCfg(bot.guid); dcfg && dcfg->eventsEnabled && winnerAoeSpell) {
				auto tiles = dbgComputeAoeTiles(bot.currentPos, *winnerAoeSpell, winnerAoeDir);
				dbgRecordPreCast(bot, fmt::format("AOE_SPELL words={} type={} dir={} size={} inner={} area={} score={:.0f}",
					winnerAoeSpell->words, aoeAreaTypeName(winnerAoeSpell->aoeAreaType),
					dirShort(winnerAoeDir), winnerAoeSpell->aoeAreaSize, winnerAoeSpell->aoeInnerSize,
					winnerAoeSpell->areaPatternName.empty() ? "?" : winnerAoeSpell->areaPatternName,
					bestScore), tiles);
			}
			castAoeSpell(bot, winnerAoeSpell, winnerAoeDir);
			break;
		}
		case AOE_RUNE: {
			auto runeItem = findRuneInBackpack(player, winnerRuneId);
			if (runeItem) {
				// Debug: capture 3x3 area around rune target pre-cast
				if (auto* dcfg = getDebugCfg(bot.guid); dcfg && dcfg->eventsEnabled) {
					std::vector<Position> tiles;
					for (int32_t dx = -1; dx <= 1; ++dx) {
						for (int32_t dy = -1; dy <= 1; ++dy) {
							tiles.push_back(Position(
								static_cast<uint16_t>(static_cast<int32_t>(winnerRunePos.x) + dx),
								static_cast<uint16_t>(static_cast<int32_t>(winnerRunePos.y) + dy),
								winnerRunePos.z));
						}
					}
					dbgRecordPreCast(bot, fmt::format("AOE_RUNE id={} center=({},{},{}) score={:.0f}",
						winnerRuneId, winnerRunePos.x, winnerRunePos.y, winnerRunePos.z, bestScore), tiles);
				}
				if (executeRune(bot, runeItem, winnerRunePos, nullptr)) {
					castLog(bot, fmt::format("RUNE_AOE: id={} -> pos ({},{},{}) score={:.0f} (bot={},{},{})",
						winnerRuneId,
						winnerRunePos.x, winnerRunePos.y, winnerRunePos.z, bestScore,
						bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
				} else {
					castLog(bot, fmt::format("RUNE_AOE_FAIL: id={} -> pos ({},{},{})",
						winnerRuneId,
						winnerRunePos.x, winnerRunePos.y, winnerRunePos.z));
					// Cancel pending precast on failure
					if (auto* dcfg = getDebugCfg(bot.guid); dcfg) dcfg->pending.active = false;
					if (auto* dcfg2 = getDebugCfg(bot.guid)) {
						dbgEmitEvent(bot, dcfg2, "rune_aoe_fail",
							fmt::format("id={} pos=({},{},{})", winnerRuneId, winnerRunePos.x, winnerRunePos.y, winnerRunePos.z));
					}
					bot.lastAttackTime = OTSYS_TIME(); // throttle on failure too
				}
			}
			break;
		}
		case SINGLE_SPELL: {
			// Mana restore — PvP is budgeted, PvE is unlimited
			const auto& instantSpellCheck = g_spells().getInstantSpell(winnerSingleSpell->words);
			uint32_t manaCost = 0;
			if (instantSpellCheck) {
				manaCost = instantSpellCheck->getManaCost(player);
			}
			if (player->getMana() < manaCost) {
				if (targetIsPlayer) {
					uint32_t maxMana = player->getMaxMana();
					uint32_t budget = maxMana * 5 / 2;
					if (bot.pvpManaSpent < budget) {
						player->mana = player->getMaxMana();
						g_game().addPlayerMana(player);
						bot.pvpManaSpent += maxMana;
					} else {
						return; // PvP mana budget exceeded
					}
				} else {
					player->mana = player->getMaxMana();
					g_game().addPlayerMana(player);
				}
			}

			// Ensure target is set (spells that use getAttackedCreature() need this)
			if (player->getAttackedCreature() != target) {
				player->setAttackedCreature(target);
			}

			// Face the target for directional spells
			if (winnerSingleSpell->needDirection) {
				Direction dir = getDirectionTo(bot.currentPos, tpos);
				if (dir > DIRECTION_WEST) {
					int_fast32_t dx = Position::getOffsetX(bot.currentPos, tpos);
					int_fast32_t dy = Position::getOffsetY(bot.currentPos, tpos);
					if (std::abs(dx) >= std::abs(dy)) {
						dir = dx < 0 ? DIRECTION_EAST : DIRECTION_WEST;
					} else {
						dir = dy > 0 ? DIRECTION_NORTH : DIRECTION_SOUTH;
					}
				}
				g_game().internalCreatureTurn(player, dir);
			}

			// Cast via server spell system
			std::string words = winnerSingleSpell->words;
			auto result = g_spells().playerSaySpell(player, words);
			bot.lastAttackTime = OTSYS_TIME();

			if (result == TALKACTION_BREAK) {
				bot.lastCombatProgress = OTSYS_TIME();
				if (targetIsPlayer) {
					bot.lastPvpAttackTime = OTSYS_TIME();
				}
				player->saySpell(TALKTYPE_SAY, words, false);
				castLog(bot, fmt::format("SPELL: {} -> {} (hp={}/{}) dist={} score={:.0f} (bot={},{},{} dir={})",
					words, target->getName(), target->getHealth(), target->getMaxHealth(),
					std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y))),
					bestScore,
					player->getPosition().x, player->getPosition().y, player->getPosition().z,
					dirToStr(player->getDirection())));
				// Debug: single-target spell — affected tile is target's position
				if (auto* dcfg = getDebugCfg(bot.guid); dcfg && dcfg->eventsEnabled) {
					dbgRecordPreCast(bot, fmt::format("SINGLE_SPELL words={} target={}#{} dist={} score={:.0f}",
						words, target->getName(), target->getID(),
						std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y))),
						bestScore), { tpos });
				}
			} else {
				castLog(bot, fmt::format("SPELL_FAIL: {} -> {} dist={} result={} (bot={},{},{} target={},{},{})",
					words, target->getName(),
					std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y))),
					static_cast<int>(result),
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					tpos.x, tpos.y, tpos.z));
				if (auto* dcfg = getDebugCfg(bot.guid)) {
					dbgEmitEvent(bot, dcfg, "spell_fail",
						fmt::format("words={} target={}#{} result={}",
							words, target->getName(), target->getID(), static_cast<int>(result)));
				}
			}
			break;
		}
		case SD_RUNE: {
			auto sdItem = findRuneInBackpack(player, resolvedSdRune_.runeId);
			if (sdItem) {
				// Debug: SD rune is single-target at tpos
				if (auto* dcfg = getDebugCfg(bot.guid); dcfg && dcfg->eventsEnabled) {
					dbgRecordPreCast(bot, fmt::format("SD_RUNE id={} target={}#{} score={:.0f}",
						resolvedSdRune_.runeId, target->getName(), target->getID(), bestScore), { tpos });
				}
				if (executeRune(bot, sdItem, tpos, target)) {
					castLog(bot, fmt::format("RUNE_SD: sudden_death -> {} (hp={}/{}) dist={} score={:.0f}",
						target->getName(), target->getHealth(), target->getMaxHealth(),
						std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y))),
						bestScore));
				} else {
					castLog(bot, fmt::format("RUNE_SD_FAIL: sudden_death -> {}",
						target->getName()));
					if (auto* dcfg = getDebugCfg(bot.guid); dcfg) dcfg->pending.active = false;
					bot.lastAttackTime = OTSYS_TIME();
				}
			}
			break;
		}
		default: break;
	}
}

const ResolvedSpell* BotEngine::selectAttackSpell(BotState& bot, const std::shared_ptr<Creature>& target,
	double* outBestScore) {
	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	auto player = bot.getPlayer();
	if (!player) return nullptr;

	int32_t level = player->getLevel();
	int32_t mlevel = player->getMagicLevel();
	const auto& spells = resolvedSingleSpells_[baseVoc];

	auto tpos = target->getPosition();
	int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y)));
	bool targetIsPlayer = target->getPlayer() != nullptr;

	// Select best spell by estimated damage
	const ResolvedSpell* bestSpell = nullptr;
	double bestDmg = 0;

	for (const auto& spell : spells) {
		if (static_cast<int32_t>(spell.level) > level) continue;
		if (spell.level > 0 && spell.level < 10 && level >= 10) continue;
		if (spell.range > 0 && dist > spell.range) continue;
		// Check server-managed cooldowns (primary + secondary group)
		if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell.spellId)) continue;
		if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell.group)) continue;
		if (spell.secondaryGroup != SPELLGROUP_NONE &&
			player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell.secondaryGroup)) continue;

		// Check target resistance (skip for player targets — PvP has different mechanics)
		if (!targetIsPlayer) {
			int32_t resist = getElementResistance(target, spell.combatType);
			if (resist >= 50) continue;      // immune or heavily resisted — skip
		}

		// Score by estimated damage
		double dmg;
		if (spell.usesSkillFormula) {
			// Knight/paladin skill-based spells: use server's weapon damage formula
			auto weapon = player->getWeapon();
			int32_t attackValue = weapon ? weapon->getAttack() : 7;
			int32_t attackSkill = weapon ? player->getWeaponSkill(weapon) : player->getSkillLevel(SKILL_SWORD);
			dmg = static_cast<double>(Weapons::getMaxWeaponDamage(level, attackSkill, attackValue, 1.0f, true)) * 0.5;
		} else {
			dmg = (spell.avgMlCoef > 0)
				? estimateDamage(level, mlevel, spell.avgMlCoef, spell.avgConst)
				: static_cast<double>(level) / 5.0; // fallback
		}

		// Apply resistance/weakness modifier
		if (!targetIsPlayer) {
			int32_t resist = getElementResistance(target, spell.combatType);
			dmg *= (100.0 - static_cast<double>(resist)) / 100.0;
		}

		if (dmg > bestDmg) {
			bestDmg = dmg;
			bestSpell = &spell;
		}
	}

	if (outBestScore) *outBestScore = bestDmg;
	return bestSpell;
}

// ============================================================================
// AoE spell system
// ============================================================================

// Matrix-driven hit predicate. Uses the actual AREA_<name> array parsed from
// register_spells.lua, so it matches the server's spell engine exactly.
//
// The matrix is defined in the array's "facing" orientation (cardinal matrix is
// N-facing; diagonal matrix is one diagonal — typically NW-facing). To check a hit
// for arbitrary direction, we map the target's world offset (dx, dy) back to matrix
// coordinates by rotating, then index the matrix.
//
// The anchor cell ({3} or {2}) marks the spell's center tile in world space:
//   - needDirection=true: spell center = caster + 1 step in dir
//   - needDirection=false: spell center = caster (self-area)
//
// Hit predicate: cell == 1 (always a hit) || cell == 3 (anchor is also a hit for
// directional waves — the target tile takes damage). Cell == 2 is NEVER a hit
// (RING-style caster marker with hole around it).
bool BotEngine::isInAreaMatrix(const Position& casterPos, const Position& targetPos,
	const AreaMatrix* cardinal, const AreaMatrix* diagonal,
	Direction dir, bool needDirection) {
	bool isDiagonal = (dir & DIRECTION_DIAGONAL_MASK) != 0;
	const AreaMatrix* m = isDiagonal && diagonal ? diagonal : cardinal;
	if (!m || m->cells.empty()) return false;

	int32_t dx = static_cast<int32_t>(targetPos.x) - static_cast<int32_t>(casterPos.x);
	int32_t dy = static_cast<int32_t>(targetPos.y) - static_cast<int32_t>(casterPos.y);

	// Offset target relative to spell center (which is the anchor cell in the matrix).
	// For needDirection waves, spell center = caster + 1 step forward; subtract that
	// forward step so dx/dy become offsets from the anchor instead of the caster.
	if (needDirection && !isDiagonal) {
		switch (dir) {
			case DIRECTION_NORTH: dy += 1; break;  // spell center is 1 N of caster
			case DIRECTION_SOUTH: dy -= 1; break;
			case DIRECTION_EAST:  dx -= 1; break;
			case DIRECTION_WEST:  dx += 1; break;
			default: break;
		}
	}
	// For diagonal needDirection waves, the diagonal matrix already encodes the
	// caster-to-anchor offset in its geometry (the {3} sits at the corner), so we
	// don't apply an extra step here — the rotation below handles it.

	// Rotate world (dx, dy) into the matrix's native (N-facing) coordinate system.
	// Convention: the matrix is defined with col=right-of-caster (+x_canonical), row=behind-
	// caster (+y_canonical, with anchor at higher row). To look up a world target, rotate so
	// "forward of caster" in world maps to -y_canonical in the matrix.
	//   N: caster forward = world -y      → identity         (x_can = dx,  y_can = dy)
	//   S: caster forward = world +y      → 180° flip        (x_can = -dx, y_can = -dy)
	//   E: caster forward = world +x      → world +x = -y_can, world +y = +x_can
	//                                       (x_can = dy,  y_can = -dx)
	//   W: caster forward = world -x      → world +x = +y_can, world +y = -x_can
	//                                       (x_can = -dy, y_can = dx)
	int32_t rx = 0, ry = 0;
	if (!isDiagonal) {
		switch (dir) {
			case DIRECTION_NORTH: rx = dx;  ry = dy;  break;
			case DIRECTION_SOUTH: rx = -dx; ry = -dy; break;
			case DIRECTION_EAST:  rx = dy;  ry = -dx; break;
			case DIRECTION_WEST:  rx = -dy; ry = dx;  break;
			default: return false;
		}
	} else {
		// Diagonal: matrix is NW-facing. Other diagonals are mirrors.
		// World "NW" direction in world coords is (-1, -1). For NW, no transform.
		// For NE (+1, -1): flip x. For SW (-1, +1): flip y. For SE (+1, +1): flip both.
		switch (dir) {
			case DIRECTION_NORTHWEST: rx = dx;  ry = dy;  break;
			case DIRECTION_NORTHEAST: rx = -dx; ry = dy;  break;
			case DIRECTION_SOUTHWEST: rx = dx;  ry = -dy; break;
			case DIRECTION_SOUTHEAST: rx = -dx; ry = -dy; break;
			default: return false;
		}
	}

	int32_t row = m->centerRow + ry;
	int32_t col = m->centerCol + rx;
	if (row < 0 || row >= static_cast<int32_t>(m->cells.size())) return false;
	const auto& matrixRow = m->cells[static_cast<size_t>(row)];
	if (col < 0 || col >= static_cast<int32_t>(matrixRow.size())) return false;
	uint8_t v = matrixRow[static_cast<size_t>(col)];
	return v == 1 || v == 3;
}

bool BotEngine::spellHits(const Position& casterPos, const Position& targetPos,
	const ResolvedSpell& spell, Direction dir) {
	if (spell.cardinalMatrix) {
		return isInAreaMatrix(casterPos, targetPos, spell.cardinalMatrix,
			spell.diagonalMatrix, dir, spell.needDirection);
	}
	return isInAoeArea(casterPos, targetPos, spell.aoeAreaType, dir,
		spell.aoeAreaSize, spell.aoeInnerSize);
}

bool BotEngine::runeHits(const Position& casterPos, const Position& targetPos,
	const ResolvedRune& rune, Direction dir) {
	if (rune.cardinalMatrix) {
		// Runes don't have needDirection — they target a specific tile, area is centered there.
		// Pass needDirection=false; the matrix anchor (caster tile in non-directional context)
		// becomes the target tile here, which the server places at the rune's target position.
		return isInAreaMatrix(casterPos, targetPos, rune.cardinalMatrix,
			rune.diagonalMatrix, dir, false);
	}
	// Fallback: legacy CIRCLE predicate via isInAoeArea. AoE runes typically use CIRCLE.
	return isInAoeArea(casterPos, targetPos, AoeAreaType::CIRCLE, dir, 3);
}

// Check if a target position falls within a spell's area pattern for a given direction
bool BotEngine::isInAoeArea(const Position& botPos, const Position& targetPos,
	AoeAreaType areaType, Direction dir, int32_t areaSize, int32_t innerSize) {
	int32_t dx = static_cast<int32_t>(targetPos.x) - static_cast<int32_t>(botPos.x);
	int32_t dy = static_cast<int32_t>(targetPos.y) - static_cast<int32_t>(botPos.y);

	switch (areaType) {
		case AoeAreaType::MELEE_CIRCLE: {
			// AREA_SQUARE1X1: all 8 adjacent tiles (Chebyshev distance <= 1)
			return std::max(std::abs(dx), std::abs(dy)) <= 1 && (dx != 0 || dy != 0);
		}
		case AoeAreaType::CIRCLE: {
			// Tibia "CIRCLE" arrays are actually Manhattan diamonds, not Euclidean disks.
			// Verified against AREA_CIRCLE3X3 (Manhattan radius 3), CIRCLE5X5 (radius 5),
			// CIRCLE6X6 (radius 6) — the leading size matches Manhattan radius exactly.
			// Euclidean predicate over-counted corners (e.g. (+3,+3) for size=5: Euclidean=4.24,
			// Manhattan=6, actual array=0).
			return (std::abs(dx) + std::abs(dy)) <= areaSize && (dx != 0 || dy != 0);
		}
		case AoeAreaType::RING: {
			// AREA_RING<innerName>_BURST<burstName>: empty 3x3 hole + Manhattan-shaped outer ring.
			// Looking at the actual array for RING1_BURST3, the outer boundary is a Manhattan
			// diamond: |dx|+|dy| <= burstName+2 (= areaSize+1, since areaSize=burstName+1).
			// Inner hole: Chebyshev <= innerName (= innerSize-1, since innerSize=innerName+1).
			// Predicate: cheb >= innerSize AND manhattan <= areaSize+1.
			int32_t cheb = std::max(std::abs(dx), std::abs(dy));
			int32_t manhattan = std::abs(dx) + std::abs(dy);
			return cheb >= innerSize && manhattan <= areaSize + 1;
		}
		case AoeAreaType::WAVE4: {
			// {3} marker = SPELL CENTER (caster + 1 step forward for needDirection waves).
			// fwd=1 is the spell center tile; fwd=2+ extend further forward.
			// Shape varies by areaSize (trailing digit of AREA_<name>):
			//   size=3 (AREA_SHORTWAVE3): fwd 1-3. fwd=1 side==0; fwd=2,3 side<=1.
			//     { 1, 1, 1 },  row 0 → fwd=3
			//     { 1, 1, 1 },  row 1 → fwd=2
			//     { 0, 3, 0 },  row 2 → fwd=1 (center only)
			//   size=4 (AREA_WAVE4): fwd 1-4. fwd=1 side==0; fwd=2,3 side<=1; fwd=4 side<=2.
			//     { 1, 1, 1, 1, 1 },  fwd=4 (widest)
			//     { 0, 1, 1, 1, 0 },  fwd=3
			//     { 0, 1, 1, 1, 0 },  fwd=2
			//     { 0, 0, 3, 0, 0 },  fwd=1 (center)
			// AREADIAGONAL_WAVE4 (NW-facing): triangle pattern, dist 1-6 with caster at corner.
			int32_t fwd, side;
			bool isDiagonal = (dir & DIRECTION_DIAGONAL_MASK) != 0;

			if (!isDiagonal) {
				// Cardinal directions: rotate (dx, dy) so "forward" is in the direction
				switch (dir) {
					case DIRECTION_NORTH: fwd = -dy; side = std::abs(dx); break;
					case DIRECTION_SOUTH: fwd = dy;  side = std::abs(dx); break;
					case DIRECTION_EAST:  fwd = dx;  side = std::abs(dy); break;
					case DIRECTION_WEST:  fwd = -dx; side = std::abs(dy); break;
					default: return false;
				}
				if (areaSize == 3) {
					// SHORTWAVE3 (Strong Ice Wave / Strong Terra Wave): no fwd=4 row.
					if (fwd < 1 || fwd > 3) return false;
					if (fwd == 1) return side == 0;
					return side <= 1;
				}
				// AREA_WAVE4 default: fwd 1-4 with widest row at fwd=4.
				if (fwd < 1 || fwd > 4) return false;
				if (fwd == 1) return side == 0;
				if (fwd == 4) return side <= 2;
				return side <= 1;
			} else {
				// Diagonal: transform to (fwd, perp) along diagonal axis
				// For NW direction (base): target at (-dx, -dy) relative to caster
				// Forward = distance along diagonal, perpendicular = deviation
				int32_t fdx = dx, fdy = dy;
				switch (dir) {
					case DIRECTION_NORTHWEST: fdx = -dx; fdy = -dy; break;
					case DIRECTION_NORTHEAST: fdx = dx;  fdy = -dy; break;
					case DIRECTION_SOUTHWEST: fdx = -dx; fdy = dy;  break;
					case DIRECTION_SOUTHEAST: fdx = dx;  fdy = dy;  break;
					default: return false;
				}
				// AREADIAGONAL_WAVE4: triangle shape, center at targetPos (1 diagonal step)
				// Server offset: range is 1-6, not 1-5
				if (fdx < 0 || fdy < 0) return false;
				int32_t dist = fdx + fdy;
				if (dist < 1 || dist > 6) return false;
				if (dist == 1) return true; // center marker
				return fdx <= 5 && fdy <= 5;
			}
		}
		case AoeAreaType::SQUAREWAVE5: {
			// AREA_SQUAREWAVE5 (north-facing base pattern):
			//   { 1, 1, 1 },  row 0: forward=4, sideways <= 1
			//   { 1, 1, 1 },  row 1: forward=3, sideways <= 1
			//   { 1, 1, 1 },  row 2: forward=2, sideways <= 1
			//   { 0, 1, 0 },  row 3: forward=1, sideways = 0
			//   { 0, 3, 0 },  caster
			// AREADIAGONAL_SQUAREWAVE5 (NW-facing):
			//   { 1, 1, 1, 0, 0 },
			//   { 1, 1, 1, 0, 0 },
			//   { 1, 1, 1, 0, 0 },
			//   { 0, 0, 0, 1, 0 },
			//   { 0, 0, 0, 0, 3 },
			int32_t fwd, side;
			bool isDiagonal = (dir & DIRECTION_DIAGONAL_MASK) != 0;

			if (!isDiagonal) {
				switch (dir) {
					case DIRECTION_NORTH: fwd = -dy; side = std::abs(dx); break;
					case DIRECTION_SOUTH: fwd = dy;  side = std::abs(dx); break;
					case DIRECTION_EAST:  fwd = dx;  side = std::abs(dy); break;
					case DIRECTION_WEST:  fwd = -dx; side = std::abs(dy); break;
					default: return false;
				}
				// Server places area center at targetPos (1 step forward)
				// fwd=1,2 are center marker + narrow tip, data rows at fwd=3,4,5
				if (fwd < 1 || fwd > 5) return false;
				if (fwd <= 2) return side == 0; // center marker + narrow tip
				return side <= 1; // fwd=3,4,5: 3 tiles wide
			} else {
				int32_t fdx = dx, fdy = dy;
				switch (dir) {
					case DIRECTION_NORTHWEST: fdx = -dx; fdy = -dy; break;
					case DIRECTION_NORTHEAST: fdx = dx;  fdy = -dy; break;
					case DIRECTION_SOUTHWEST: fdx = -dx; fdy = dy;  break;
					case DIRECTION_SOUTHEAST: fdx = dx;  fdy = dy;  break;
					default: return false;
				}
				// AREADIAGONAL_SQUAREWAVE5: center at targetPos (1 diagonal step)
				// Server offset: range is 1-5, not 1-4
				if (fdx < 0 || fdy < 0) return false;
				int32_t dist = fdx + fdy;
				if (dist < 1 || dist > 5) return false;
				if (dist <= 2) return true; // center marker + first diagonal step
				return fdx <= 4 && fdy <= 4;
			}
		}
		case AoeAreaType::BEAM5: {
			// AREA_BEAM5: 5 tiles forward in a straight line, 1 tile wide
			// Server center at targetPos (1 step forward), beam extends 4 more = fwd 1-5
			switch (dir) {
				case DIRECTION_NORTH: return dx == 0 && dy >= -5 && dy <= -1;
				case DIRECTION_SOUTH: return dx == 0 && dy >= 1  && dy <= 5;
				case DIRECTION_EAST:  return dy == 0 && dx >= 1  && dx <= 5;
				case DIRECTION_WEST:  return dy == 0 && dx >= -5 && dx <= -1;
				default: return false;
			}
		}
		default:
			return false;
	}
}

// Find the direction that maximizes targets hit by a wave spell
Direction BotEngine::findBestWaveDirection(BotState& bot, const ResolvedSpell* spell,
	const std::vector<std::shared_ptr<Creature>>& nearby, int32_t& outCount,
	double* outWeightedCount) {
	Direction bestDir = DIRECTION_NORTH;
	outCount = 0;
	double bestWeighted = 0;
	auto combatType = spell->combatType;

	// Only test cardinal directions (N=0, E=1, S=2, W=3) — real players can only
	// face cardinal directions when casting wave spells. Diagonal directions use
	// different area matrices (AREADIAGONAL_*) that don't match expected behavior.
	for (uint8_t d = 0; d <= DIRECTION_WEST; d++) {
		auto dir = static_cast<Direction>(d);
		int32_t count = 0;
		double weighted = 0;
		for (const auto& creature : nearby) {
			if (spellHits(bot.currentPos, creature->getPosition(), *spell, dir)) {
				// LOS check — server's AreaCombat::getList() checks isSightClear per tile (combat.cpp:2153)
				if (g_game().map.isSightClear(bot.currentPos, creature->getPosition(), true)) {
					count++;
					int32_t resist = getElementResistance(creature, combatType);
					weighted += (100.0 - static_cast<double>(resist)) / 100.0;
				}
			}
		}
		if (weighted > bestWeighted) {
			bestWeighted = weighted;
			outCount = count;
			bestDir = dir;
		}
	}
	if (outWeightedCount) *outWeightedCount = bestWeighted;
	return bestDir;
}

// Count valid AoE targets in the spell's area, filtering by immunity
int32_t BotEngine::countAoeTargets(BotState& bot, const ResolvedSpell* spell, Direction dir,
	std::vector<std::shared_ptr<Creature>>& outTargets, double* outWeightedCount) {
	auto player = bot.getPlayer();
	if (!player) return 0;

	outTargets.clear();
	auto combatType = spell->combatType;
	double weighted = 0;

	// Determine scan radius — prefer matrix extents (source of truth), fall back to enum-based.
	// Add +1 forward for needDirection waves (spell center is caster + 1 forward, so the
	// farthest hit tile is at (maxExtent + 1) from caster in the forward axis).
	int32_t scanRadius;
	if (spell->cardinalMatrix) {
		int32_t baseR = std::max(spell->cardinalMatrix->maxRowExtent,
			spell->cardinalMatrix->maxColExtent);
		scanRadius = baseR + (spell->needDirection ? 1 : 0);
		if (scanRadius < 1) scanRadius = 1;
	} else {
		switch (spell->aoeAreaType) {
			case AoeAreaType::MELEE_CIRCLE: scanRadius = 1; break;
			case AoeAreaType::WAVE4:        scanRadius = 5; break;
			case AoeAreaType::SQUAREWAVE5:  scanRadius = 5; break;
			case AoeAreaType::BEAM5:        scanRadius = 5; break;
			case AoeAreaType::CIRCLE:       scanRadius = spell->aoeAreaSize; break;
			default: scanRadius = spell->aoeAreaSize; break;
		}
	}

	// Scan for monsters only (exclude players, bots, NPCs)
	auto spectators = Spectators().find<Monster>(bot.currentPos, false,
		scanRadius, scanRadius, scanRadius, scanRadius);

	for (const auto& creature : spectators) {
		if (creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto cpos = creature->getPosition();
		if (cpos.z != bot.currentPos.z) continue;

		// Resistance check — skip creatures with >=50% resistance to this damage type
		int32_t resist = getElementResistance(creature, combatType);
		if (resist >= 50) continue;

		// Area check
		if (!spellHits(bot.currentPos, cpos, *spell, dir)) continue;

		// LOS check — server's AreaCombat::getList() checks isSightClear per tile (combat.cpp:2153)
		if (!g_game().map.isSightClear(bot.currentPos, cpos, true)) continue;

		outTargets.push_back(creature);
		weighted += (100.0 - static_cast<double>(resist)) / 100.0;
	}

	if (outWeightedCount) *outWeightedCount = weighted;
	return static_cast<int32_t>(outTargets.size());
}

// Select the best AoE spell — returns nullptr if single-target should be used
// Optionally outputs the target count and estimated total damage score for unified comparison
const ResolvedSpell* BotEngine::selectAoeSpell(BotState& bot, Direction& outBestDir,
	int32_t* outBestCount, double* outBestScore,
	const std::shared_ptr<Creature>& pvpTarget) {
	auto player = bot.getPlayer();
	if (!player) return nullptr;

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	int32_t level = player->getLevel();
	int32_t mlevel = player->getMagicLevel();
	const auto& aoeSpells = resolvedAoeSpells_[baseVoc];
	if (aoeSpells.empty()) return nullptr;

	const ResolvedSpell* bestSpell = nullptr;
	double bestScore = 0;
	int32_t bestCount = 0;
	Direction bestDir = DIRECTION_NORTH;

	std::vector<std::shared_ptr<Creature>> targets;

	for (const auto& spell : aoeSpells) {
		if (static_cast<int32_t>(spell.level) > level) continue;
		// Skip trivially weak spells (infir tier) for bots over level 10
		if (spell.level > 0 && spell.level < 10 && level >= 10) continue;

		// Check server-managed cooldowns (primary + secondary group)
		if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell.spellId)) continue;
		if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell.group)) continue;
		if (spell.secondaryGroup != SPELLGROUP_NONE &&
			player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell.secondaryGroup)) continue;

		int32_t count = 0;
		Direction spellDir = DIRECTION_NORTH;
		double weightedCount = 0;

		if (spell.aoeAreaType == AoeAreaType::WAVE4 || spell.aoeAreaType == AoeAreaType::SQUAREWAVE5
			|| spell.aoeAreaType == AoeAreaType::BEAM5) {
			auto combatType = spell.combatType;
			int32_t scanRadius = 5;

			auto spectators = Spectators().find<Monster>(bot.currentPos, false,
				scanRadius, scanRadius, scanRadius, scanRadius);

			std::vector<std::shared_ptr<Creature>> nearby;
			for (const auto& creature : spectators) {
				if (creature->isRemoved() || creature->getHealth() <= 0) continue;
				auto cpos = creature->getPosition();
				if (cpos.z != bot.currentPos.z) continue;
				if (getElementResistance(creature, combatType) >= 50) continue;
				nearby.push_back(creature);
			}

			// Include PvP target in wave direction evaluation
			if (pvpTarget && pvpTarget->getHealth() > 0) {
				auto tpos = pvpTarget->getPosition();
				if (tpos.z == bot.currentPos.z) {
					nearby.push_back(pvpTarget);
				}
			}
			spellDir = findBestWaveDirection(bot, &spell, nearby, count, &weightedCount);
		} else {
			count = countAoeTargets(bot, &spell, DIRECTION_NORTH, targets, &weightedCount);
			// Include PvP target in non-wave AOE scoring
			if (pvpTarget && pvpTarget->getHealth() > 0) {
				auto tpos = pvpTarget->getPosition();
				if (tpos.z == bot.currentPos.z &&
					spellHits(bot.currentPos, tpos, spell, DIRECTION_NORTH) &&
					g_game().map.isSightClear(bot.currentPos, tpos, true)) {
					count++;
					weightedCount += 1.0;
				}
			}
		}

		if (count < spell.minTargets) continue;

		// Score: estimated damage per target * resistance-weighted count
		double dmgPerTarget;
		if (spell.usesSkillFormula) {
			auto weapon = player->getWeapon();
			int32_t attackValue = weapon ? weapon->getAttack() : 7;
			int32_t attackSkill = weapon ? player->getWeaponSkill(weapon) : player->getSkillLevel(SKILL_SWORD);
			dmgPerTarget = static_cast<double>(Weapons::getMaxWeaponDamage(level, attackSkill, attackValue, 1.0f, true)) * 0.5;
		} else {
			dmgPerTarget = (spell.avgMlCoef > 0)
				? estimateDamage(level, mlevel, spell.avgMlCoef, spell.avgConst)
				: static_cast<double>(level) / 5.0;
		}
		double score = dmgPerTarget * weightedCount;
		if (score > bestScore) {
			bestScore = score;
			bestCount = count;
			bestSpell = &spell;
			bestDir = spellDir;
		}
	}

	outBestDir = bestDir;
	if (outBestCount) *outBestCount = bestCount;
	if (outBestScore) *outBestScore = bestScore;
	return bestSpell;
}

// Cast an AoE spell, applying damage to all valid targets in the area
void BotEngine::castAoeSpell(BotState& bot, const ResolvedSpell* spell, Direction bestDir) {
	auto player = bot.getPlayer();
	if (!player || !spell) return;

	// Turn bot to face optimal direction (instant, same tick)
	g_game().internalCreatureTurn(player, bestDir);

	// Check mana cost before casting — restore if needed
	const auto& instantSpellAoe = g_spells().getInstantSpell(spell->words);
	if (instantSpellAoe) {
		uint32_t manaCost = instantSpellAoe->getManaCost(player);
		if (player->getMana() < manaCost) {
			// For AoE spells during PvP, check budget
			if (bot.attackerId > 0 && g_game().getPlayerByID(bot.attackerId) != nullptr) {
				uint32_t maxMana = player->getMaxMana();
				uint32_t budget = maxMana * 5 / 2;
				if (bot.pvpManaSpent < budget) {
					player->mana = player->getMaxMana();
					g_game().addPlayerMana(player);
					bot.pvpManaSpent += maxMana;
				} else {
					return; // PvP mana budget exceeded
				}
			} else {
				player->mana = player->getMaxMana();
				g_game().addPlayerMana(player);
			}
		}
	}

	// Re-sync position before counting (bot.currentPos may be stale from tick start)
	bot.currentPos = player->getPosition();

	// Count targets BEFORE casting (spell may kill some — post-cast count would underreport)
	std::vector<std::shared_ptr<Creature>> preCastTargets;
	countAoeTargets(bot, spell, bestDir, preCastTargets);
	int32_t preCastCount = static_cast<int32_t>(preCastTargets.size());

	// Skip cast if no targets at actual position (prevents wasted spells)
	if (preCastCount == 0) return;

	bool hitPlayer = false;
	for (const auto& t : preCastTargets) {
		if (t->getPlayer()) { hitPlayer = true; break; }
	}

	// Cast via server spell system — handles area damage, effects, cooldowns, mana, everything
	std::string words = spell->words;
	auto result = g_spells().playerSaySpell(player, words);

	if (result == TALKACTION_BREAK) {
		int64_t now = OTSYS_TIME();
		bot.lastAttackTime = now;
		bot.lastCombatProgress = now;
		if (hitPlayer) bot.lastPvpAttackTime = now;

		// Broadcast spell words visually
		player->saySpell(TALKTYPE_SAY, words, false);
		auto serverPos = player->getPosition();
		auto spellTargetPos = getNextPosition(bestDir, serverPos);
		castLog(bot, fmt::format("AOE: {} -> {} targets (bot={},{},{} dir={} spellHit={},{},{})",
			words, preCastCount,
			serverPos.x, serverPos.y, serverPos.z,
			dirToStr(bestDir),
			spellTargetPos.x, spellTargetPos.y, spellTargetPos.z));
	}
}

// ============================================================================
// Rune system — uses real server RuneSpell::executeUse() path
// ============================================================================

double BotEngine::estimateDamage(int32_t level, int32_t mlevel, double avgMlevelCoef, double avgConstant) {
	return static_cast<double>(level) / 5.0 + static_cast<double>(mlevel) * avgMlevelCoef + avgConstant;
}

std::shared_ptr<Item> BotEngine::findRuneInBackpack(const std::shared_ptr<Player>& player, uint16_t runeItemId) {
	auto bpItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	if (!bpItem) return nullptr;
	auto container = bpItem->getContainer();
	if (!container) return nullptr;
	for (uint32_t i = 0; i < container->size(); i++) {
		auto item = container->getItemByIndex(i);
		if (item && item->getID() == runeItemId) {
			return item;
		}
	}
	return nullptr;
}

std::tuple<Position, int32_t, double> BotEngine::findBestRunePosition(BotState& bot, CombatType_t combatType) {
	auto player = bot.getPlayer();
	if (!player) return {{}, 0, 0};

	// Scan radius = rune range (7) for all monsters on same z
	auto spectators = Spectators().find<Monster>(bot.currentPos, false,
		RUNE_RANGE, RUNE_RANGE, RUNE_RANGE, RUNE_RANGE);

	// Collect non-resistant monsters with their resistance weight
	struct MonsterTarget {
		Position pos;
		double weight; // (100 - resist) / 100 — >1.0 for weaknesses, <1.0 for partial resistance
	};
	std::vector<MonsterTarget> monsters;
	for (const auto& creature : spectators) {
		if (creature->isRemoved() || creature->getHealth() <= 0) continue;
		if (creature->getPlayer()) continue; // skip players
		auto cpos = creature->getPosition();
		if (cpos.z != bot.currentPos.z) continue;
		int32_t resist = getElementResistance(creature, combatType);
		if (resist >= 50) continue;
		double weight = (100.0 - static_cast<double>(resist)) / 100.0;
		monsters.push_back({cpos, weight});
	}

	if (monsters.empty()) return {{}, 0, 0};

	Position bestPos;
	int32_t bestCount = 0;
	double bestWeighted = 0;

	// Test each monster position as candidate rune landing center
	for (const auto& candidate : monsters) {
		// Check rune range (Chebyshev distance ≤ 7) from bot
		int32_t distX = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(candidate.pos.x));
		int32_t distY = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(candidate.pos.y));
		if (std::max(distX, distY) > RUNE_RANGE) continue;

		// Check LOS from bot to rune landing position
		if (!g_game().map.isSightClear(bot.currentPos, candidate.pos, true)) continue;

		// Count monsters in 3x3 area (Chebyshev distance ≤ 1 from center)
		int32_t count = 0;
		double weighted = 0;
		for (const auto& m : monsters) {
			int32_t dx = std::abs(static_cast<int32_t>(candidate.pos.x) - static_cast<int32_t>(m.pos.x));
			int32_t dy = std::abs(static_cast<int32_t>(candidate.pos.y) - static_cast<int32_t>(m.pos.y));
			if (std::max(dx, dy) <= 1) {
				count++;
				weighted += m.weight;
			}
		}

		if (weighted > bestWeighted) {
			bestWeighted = weighted;
			bestCount = count;
			bestPos = candidate.pos;
		}
	}

	return {bestPos, bestCount, bestWeighted};
}

bool BotEngine::executeRune(BotState& bot, const std::shared_ptr<Item>& runeItem,
	const Position& targetPos, const std::shared_ptr<Creature>& targetCreature) {
	auto player = bot.getPlayer();
	if (!player || !runeItem) return false;

	auto runeSpell = g_spells().getRuneSpell(runeItem->getID());
	if (!runeSpell) {
		castLog(bot, fmt::format("RUNE_ERR: No RuneSpell found for itemId={}", runeItem->getID()));
		return false;
	}

	// Restore mana if needed (unlimited for PvE hunting)
	uint32_t manaCost = runeSpell->getMana();
	if (player->getMana() < manaCost) {
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
	}

	// executeUse handles validation, cooldowns, combat, everything
	bool success = runeSpell->executeUse(player, runeItem, player->getPosition(),
		targetCreature, targetPos, true);

	if (success) {
		int64_t now = OTSYS_TIME();
		bot.lastAttackTime = now;
		bot.lastCombatProgress = now;
	}

	return success;
}

// ============================================================================
// PvP realism — player-aware movement, haste, and magic-wall play
//
// Goal: make bot vs. player fights look human instead of two creatures frozen
// on adjacent SQMs. Knights circle their target (ported from Monster::getDanceStep),
// distance vocations kite away from the human (PvP keep-distance — note the hunt
// keep-distance is 0 when not hunting), everyone hastes to chase/escape, and
// capable bots drop magic-wall runes to block an escape or a pursuit. All gated
// by PvpCfg (hot-reloadable, each behavior independently disableable).
// ============================================================================

void BotEngine::pvpResolveHasteSpells() {
	for (int v = 0; v < 5; ++v) resolvedHaste_[v].clear();
	// Candidate haste words, strongest first. Vocation gating is read from the
	// server spell registry (vocMap) so we only keep what each voc can actually cast.
	static const char* kCandidates[] = { "utani gran hur", "utani tempo hur", "utani hur" };
	for (int baseVoc = 1; baseVoc <= 4; ++baseVoc) {
		uint16_t promVocId = static_cast<uint16_t>(baseVoc + 4); // promoted vocation id
		for (const char* words : kCandidates) {
			const auto& spell = g_spells().getInstantSpell(words);
			if (!spell) continue;
			const auto& vocMap = spell->getVocMap();
			if (vocMap.find(promVocId) == vocMap.end()) continue;
			resolvedHaste_[baseVoc].emplace_back(words);
		}
		g_logger().info("[BotEngine] Voc {}: {} haste spell(s) resolved", baseVoc, resolvedHaste_[baseVoc].size());
	}
}

int32_t BotEngine::getPvpKeepDistance(uint8_t baseVoc) const {
	switch (baseVoc) {
		case 1: case 2: return pvpCfg_.mageKeepDist;  // sorcerer / druid
		case 3: return pvpCfg_.paladinKeepDist;       // paladin
		case 4: return 0;                             // knight = melee
		default: return 0;
	}
}

bool BotEngine::pvpCastBestHaste(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!pvpCfg_.enableHaste || !player) return false;
	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	if (baseVoc < 1 || baseVoc > 4) return false;
	const auto& cands = resolvedHaste_[baseVoc];
	if (cands.empty()) return false;
	if (player->hasCondition(CONDITION_HASTE)) return false; // already hasted

	int64_t now = OTSYS_TIME();
	auto cdIt = s_pvpHasteCd.find(bot.guid);
	if (cdIt != s_pvpHasteCd.end() && now < cdIt->second) return false;

	int32_t level = player->getLevel();
	for (const auto& words : cands) {
		const auto& spell = g_spells().getInstantSpell(words);
		if (!spell) continue;
		if (level < static_cast<int32_t>(spell->getLevel())) continue;
		if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell->getSpellId())) continue;
		if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell->getGroup())) continue;

		uint32_t manaCost = spell->getManaCost(player);
		if (player->getMana() < manaCost) {
			// Haste draws from the same PvP mana budget as damage, but cheaply
			// (we only charge the spell's cost, not a full refill).
			uint32_t budget = player->getMaxMana() * 5 / 2;
			if (bot.pvpManaSpent >= budget) return false;
			player->mana = player->getMaxMana();
			g_game().addPlayerMana(player);
			bot.pvpManaSpent += manaCost;
		}
		std::string w = words;
		auto result = g_spells().playerSaySpell(player, w);
		if (result == TALKACTION_BREAK) {
			s_pvpHasteCd[bot.guid] = now + pvpCfg_.hasteCooldownMs;
			player->saySpell(TALKTYPE_SAY, w, false);
			castLog(bot, fmt::format("PVP_HASTE: {} (pvpManaSpent={}/{})",
				w, bot.pvpManaSpent, player->getMaxMana() * 5 / 2));
			return true;
		}
	}
	return false;
}

// Single-step "dance" around the target while preserving the exact distance,
// LOS and walkability — the same idea as Monster::getDanceStep, gated by a
// per-tick probability + cooldown. desiredDist 1 = knight circling adjacent;
// desiredDist == band = mage/paladin lateral strafe to dodge.
bool BotEngine::pvpDanceStep(BotState& bot, const std::shared_ptr<Player>& player,
	const Position& targetPos, int32_t desiredDist) {
	if (!pvpCfg_.enableDance || !player) return false;
	if (!player->listWalkDir.empty()) return false; // don't interrupt an active step
	if (bot.currentPos.z != targetPos.z) return false;

	int64_t now = OTSYS_TIME();
	auto cdIt = s_pvpDanceCd.find(bot.guid);
	if (cdIt != s_pvpDanceCd.end() && now < cdIt->second) return false;

	const Position& pos = bot.currentPos;
	int32_t curDist = std::max(std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(targetPos.x)),
	                           std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(targetPos.y)));
	if (curDist != desiredDist) return false; // only dance when exactly at the desired distance

	// Probability gate — most ticks we hold position (and pause re-rolls briefly).
	if (uniform_random(1, 100) > pvpCfg_.danceChancePct) {
		s_pvpDanceCd[bot.guid] = now + pvpCfg_.danceCooldownMs;
		return false;
	}

	// Cardinal candidates that keep the SAME chebyshev distance to the target,
	// stay on the same floor, are walkable/empty, and keep line of sight.
	std::vector<Direction> dirs;
	auto tryDir = [&](Direction d, int32_t nx, int32_t ny) {
		int32_t nd = std::max(std::abs(nx - static_cast<int32_t>(targetPos.x)),
		                      std::abs(ny - static_cast<int32_t>(targetPos.y)));
		if (nd != desiredDist) return;
		Position np(static_cast<uint16_t>(nx), static_cast<uint16_t>(ny), pos.z);
		if (wouldChangeZ(pos, np)) return;
		if (!g_game().map.canWalkTo(player, np)) return;
		if (!g_game().map.isSightClear(np, targetPos, true)) return;
		dirs.push_back(d);
	};
	tryDir(DIRECTION_NORTH, static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y) - 1);
	tryDir(DIRECTION_SOUTH, static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y) + 1);
	tryDir(DIRECTION_EAST,  static_cast<int32_t>(pos.x) + 1, static_cast<int32_t>(pos.y));
	tryDir(DIRECTION_WEST,  static_cast<int32_t>(pos.x) - 1, static_cast<int32_t>(pos.y));

	if (dirs.empty()) {
		s_pvpDanceCd[bot.guid] = now + pvpCfg_.danceCooldownMs;
		return false;
	}
	Direction chosen = dirs[uniform_random(0, static_cast<int32_t>(dirs.size()) - 1)];
	botStartAutoWalk(bot, player, { chosen });
	s_pvpDanceCd[bot.guid] = now + pvpCfg_.danceCooldownMs;
	bot.lastCombatProgress = now;
	return true;
}

// Player-aware combat positioning. Replaces chaseTarget() for the attacking ticks
// of a PvP fight so distance vocations actually back off the human (hunt
// keep-distance is 0 when not hunting) and knights circle instead of standing still.
void BotEngine::pvpReposition(BotState& bot, const std::shared_ptr<Creature>& target) {
	auto player = bot.getPlayer();
	if (!player || !target) return;
	if (!pvpCfg_.enableReposition) { chaseTarget(bot, target); return; }

	auto tpos = target->getPosition();
	if (tpos.z != bot.currentPos.z) { chaseTarget(bot, target); return; } // z-pursuit handled upstream

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	int32_t keepDist = getPvpKeepDistance(baseVoc);
	int32_t attackRange = getAttackRange(baseVoc);
	int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
	                        std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y)));

	// --- Knight / melee: close to adjacency, then circle the target ---
	if (keepDist <= 0) {
		if (dist <= 1) {
			pvpDanceStep(bot, player, tpos, 1);
			return;
		}
		chaseTarget(bot, target); // aggressive close-in reuses the proven chase path
		return;
	}

	// --- Distance vocations: hold keepDist from the HUMAN, kite + strafe ---
	int32_t safeMax = std::max(keepDist + 1, attackRange);
	if (player->getFollowCreature()) player->setFollowCreature(nullptr);
	if (!player->listWalkDir.empty()) return;

	auto rIt = s_retreatUntil.find(bot.guid);
	if (rIt != s_retreatUntil.end() && OTSYS_TIME() < rIt->second) return;
	s_retreatUntil.erase(bot.guid);

	if (dist < keepDist) {
		// Too close — retreat to the [keepDist, safeMax] band.
		FindPathParams fpp;
		fpp.fullPathSearch = false;
		fpp.clearSight = true;
		fpp.allowDiagonal = true;
		fpp.keepDistance = true;
		fpp.maxSearchDist = 12;
		fpp.minTargetDist = keepDist;
		fpp.maxTargetDist = safeMax;
		std::vector<Direction> dirList;
		if (g_game().map.getPathMatching(player, tpos, dirList, FrozenPathingConditionCall(tpos), fpp)) {
			size_t steps = std::min(dirList.size(), static_cast<size_t>(2));
			dirList.resize(steps);
			if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
				botStartAutoWalk(bot, player, dirList);
				bot.lastCombatProgress = OTSYS_TIME();
				s_retreatUntil[bot.guid] = OTSYS_TIME() + static_cast<int64_t>(steps) * 300LL + 300LL;
				return;
			}
		}
		Direction rd;
		if (getRetreatStep(bot, tpos, rd)) {
			botStartAutoWalk(bot, player, { rd });
			bot.lastCombatProgress = OTSYS_TIME();
			s_retreatUntil[bot.guid] = OTSYS_TIME() + 600LL;
		}
		return;
	}

	if (dist >= keepDist && dist <= safeMax && g_game().map.isSightClear(bot.currentPos, tpos, true)) {
		// In the safe band with LOS — occasionally strafe sideways, else hold and cast.
		pvpDanceStep(bot, player, tpos, dist);
		return;
	}

	// Too far / no LOS — approach back into the band.
	FindPathParams fpp;
	fpp.fullPathSearch = true;
	fpp.clearSight = true;
	fpp.allowDiagonal = true;
	fpp.keepDistance = true;
	fpp.maxSearchDist = PATH_MAX_DIST;
	fpp.minTargetDist = keepDist;
	fpp.maxTargetDist = safeMax;
	std::vector<Direction> dirList;
	if (g_game().map.getPathMatching(player, tpos, dirList, FrozenPathingConditionCall(tpos), fpp)) {
		size_t steps = std::min(dirList.size(), static_cast<size_t>(3));
		dirList.resize(steps);
		if (!hasFloorChangeTileInPath(bot.currentPos, dirList)) {
			botStartAutoWalk(bot, player, dirList);
			bot.lastCombatProgress = OTSYS_TIME();
		}
	}
}

// Place a magic wall (rune 3180) to block a straight-line escape or pursuit.
//  - ENGAGING (fleeing=false): wall one tile BEYOND the opponent on the bot->opponent
//    line, so the opponent cannot run straight away.
//  - FLEEING (fleeing=true): if a gap already exists, wall the tile between bot and
//    chaser immediately; if the chaser is adjacent, record a pending wall on the bot's
//    current tile and place it next tick once the bot has stepped off it (can't place
//    a wall on your own SQM).
bool BotEngine::pvpTryPlaceWall(BotState& bot, const std::shared_ptr<Creature>& target, bool fleeing) {
	auto player = bot.getPlayer();
	if (!pvpCfg_.enableMagicWall || !player || !target) return false;
	// Rune requirements: level 32 + magic level 9 (any vocation). Gate up-front so a
	// doomed cast never burns the shared 2s attack-group cooldown.
	if (player->getLevel() < 32 || player->getMagicLevel() < 9) return false;

	int64_t now = OTSYS_TIME();
	auto cdIt = s_pvpWallCd.find(bot.guid);
	if (cdIt != s_pvpWallCd.end() && now < cdIt->second) return false;

	auto tpos = target->getPosition();
	if (tpos.z != bot.currentPos.z) return false;
	int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x)),
	                        std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y)));

	// Probability gate (brief reroll delay so we don't roll every tick).
	if (uniform_random(1, 100) > pvpCfg_.wallChancePct) {
		s_pvpWallCd[bot.guid] = now + 1000;
		return false;
	}

	auto clampStep = [](int32_t v) -> int32_t { return v > 0 ? 1 : (v < 0 ? -1 : 0); };

	Position wallTile;
	if (!fleeing) {
		// Wall one tile beyond the opponent on the bot->opponent line.
		int32_t sx = clampStep(static_cast<int32_t>(tpos.x) - static_cast<int32_t>(bot.currentPos.x));
		int32_t sy = clampStep(static_cast<int32_t>(tpos.y) - static_cast<int32_t>(bot.currentPos.y));
		if (sx == 0 && sy == 0) return false;
		wallTile = Position(static_cast<uint16_t>(static_cast<int32_t>(tpos.x) + sx),
		                    static_cast<uint16_t>(static_cast<int32_t>(tpos.y) + sy), tpos.z);
	} else {
		// Direction from bot toward the chaser.
		int32_t sx = clampStep(static_cast<int32_t>(tpos.x) - static_cast<int32_t>(bot.currentPos.x));
		int32_t sy = clampStep(static_cast<int32_t>(tpos.y) - static_cast<int32_t>(bot.currentPos.y));
		if (sx == 0 && sy == 0) return false;
		if (dist <= 1) {
			// Chaser adjacent — can't wall our own tile; record it and place after we move.
			s_pvpPendingWall[bot.guid] = { bot.currentPos, now + 2000 };
			return false;
		}
		// Gap already exists — wall the tile one step toward the chaser (between us).
		wallTile = Position(static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.x) + sx),
		                    static_cast<uint16_t>(static_cast<int32_t>(bot.currentPos.y) + sy), bot.currentPos.z);
	}

	// Validity: in range, LOS, open walkable floor, not a field/stair/PZ, unoccupied.
	int32_t wdist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wallTile.x)),
	                         std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wallTile.y)));
	if (wdist > RUNE_RANGE) return false;
	if (!g_game().map.isSightClear(bot.currentPos, wallTile, true)) return false;
	auto wt = g_game().map.getTile(wallTile);
	if (!wt) return false;
	if (wt->hasFlag(TILESTATE_FLOORCHANGE) || wt->hasFlag(TILESTATE_PROTECTIONZONE)) return false;
	if (wt->getTopVisibleCreature(nullptr) != nullptr) return false; // occupied — placement would fail/waste
	if (!g_game().map.canWalkTo(player, wallTile)) return false;     // must be an open path tile to matter

	auto rune = findRuneInBackpack(player, 3180);
	if (!rune) return false;
	if (!executeRune(bot, rune, wallTile, nullptr)) return false;

	bot.lastPvpAttackTime = now; // placing IS a hostile action — keep bot-side pz-lock state honest
	s_pvpWallCd[bot.guid] = now + pvpCfg_.wallCooldownMs;
	castLog(bot, fmt::format("PVP_WALL: magic wall at ({},{},{}) mode={} (bot=({},{},{}) target=({},{},{}))",
		wallTile.x, wallTile.y, wallTile.z, fleeing ? "flee" : "engage",
		bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, tpos.x, tpos.y, tpos.z));
	return true;
}

// Execute a pending fleeing wall once the bot has physically vacated the tile.
bool BotEngine::pvpRunPendingWall(BotState& bot) {
	auto it = s_pvpPendingWall.find(bot.guid);
	if (it == s_pvpPendingWall.end()) return false;
	auto player = bot.getPlayer();
	int64_t now = OTSYS_TIME();
	Position wallTile = it->second.first;
	int64_t deadline = it->second.second;

	// Expired, engine gone, or wall behavior disabled — drop it.
	if (!player || !pvpCfg_.enableMagicWall || now > deadline) {
		s_pvpPendingWall.erase(it);
		return false;
	}
	// Still standing on the tile (move not yet resolved) — wait one more tick.
	if (bot.currentPos == wallTile) return false;
	// We've moved off it — consume the pending request regardless of outcome below.
	s_pvpPendingWall.erase(it);

	if (wallTile.z != bot.currentPos.z) return false;
	int32_t wdist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wallTile.x)),
	                         std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wallTile.y)));
	if (wdist > RUNE_RANGE) return false;
	if (!g_game().map.isSightClear(bot.currentPos, wallTile, true)) return false;
	auto wt = g_game().map.getTile(wallTile);
	if (!wt) return false;
	if (wt->hasFlag(TILESTATE_FLOORCHANGE) || wt->hasFlag(TILESTATE_PROTECTIONZONE)) return false;
	if (wt->getTopVisibleCreature(nullptr) != nullptr) return false;
	if (!g_game().map.canWalkTo(player, wallTile)) return false;

	auto rune = findRuneInBackpack(player, 3180);
	if (!rune) return false;
	if (!executeRune(bot, rune, wallTile, nullptr)) return false;

	bot.lastPvpAttackTime = now;
	s_pvpWallCd[bot.guid] = now + pvpCfg_.wallCooldownMs;
	castLog(bot, fmt::format("PVP_WALL: magic wall on vacated tile ({},{},{}) (bot now=({},{},{}))",
		wallTile.x, wallTile.y, wallTile.z, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
	return true;
}

// ============================================================================
// Gang-PK alpha-strike (Feature 1) — implementation
// ============================================================================

// Place a magic wall on a SPECIFIC tile (gang box). Standalone copy of pvpTryPlaceWall's validity
// checks for an explicit tile — pvpTryPlaceWall stays unchanged for the 1:1 path.
bool BotEngine::pvpPlaceWallAt(BotState& bot, const Position& tile) {
	auto player = bot.getPlayer();
	if (!pvpCfg_.enableMagicWall || !player) return false;
	if (player->getLevel() < 32 || player->getMagicLevel() < 9) return false;
	int64_t now = OTSYS_TIME();
	auto cdIt = s_pvpWallCd.find(bot.guid);
	if (cdIt != s_pvpWallCd.end() && now < cdIt->second) return false;
	if (tile.z != bot.currentPos.z) return false;
	int32_t wdist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tile.x)),
	                         std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tile.y)));
	if (wdist > RUNE_RANGE) return false;
	if (!g_game().map.isSightClear(bot.currentPos, tile, true)) return false;
	auto wt = g_game().map.getTile(tile);
	if (!wt) return false;
	if (wt->hasFlag(TILESTATE_FLOORCHANGE) || wt->hasFlag(TILESTATE_PROTECTIONZONE)) return false;
	if (wt->getTopVisibleCreature(nullptr) != nullptr) return false;
	if (!g_game().map.canWalkTo(player, tile)) return false;
	auto rune = findRuneInBackpack(player, 3180);
	if (!rune) return false;
	if (!executeRune(bot, rune, tile, nullptr)) return false;
	bot.lastPvpAttackTime = now;
	s_pvpWallCd[bot.guid] = now + pvpCfg_.wallCooldownMs;
	castLog(bot, fmt::format("GANG_WALL: magic wall at ({},{},{})", tile.x, tile.y, tile.z));
	return true;
}

// Initiator scan: a bot standing in a PZ tries to recruit nearby idle bots to jump an exposed
// victim just outside the PZ. Sets up a shared GangSession; members are driven by handleGangStaging.
void BotEngine::checkGangJump(BotState& bot) {
	if (!g_configManager().getBoolean(BOT_GANG_ENABLE)) return;
	if (bot.state != BotAIState::IDLE) return;
	if (s_gangByGuid.count(bot.guid)) return;
	auto player = bot.getPlayer();
	if (!player) return;
	int64_t now = OTSYS_TIME();

	auto scIt = s_gangNextScan.find(bot.guid);
	if (scIt != s_gangNextScan.end() && now < scIt->second) return;
	s_gangNextScan[bot.guid] = now + g_configManager().getNumber(BOT_GANG_SCAN_COOLDOWN_MS);

	if (now == s_gangLastInitTickMs) return; // soft one-gang-per-tick guard

	if (bot.lastDeathTime > 0 && now - bot.lastDeathTime < 60000) return;
	if (player->isPzLocked()) return; // cooling down — don't start a new raid right after a fight

	// A raid SUMMONS its attackers to assemble near the victim, so the initiator no longer needs to
	// stand in a PZ or be adjacent to the victim — it just has to be an idle bot in the victim's
	// town that detects an exposed player/bot near a PZ edge. Cheap pre-gate: only proceed if an
	// observer/anchor (real player or cast-watched bot) is within town range of this initiator.
	const int32_t VICTIM_TOWN_RANGE = 150; // Chebyshev tiles — keeps a raid within the initiator's town
	refreshAnchorsIfStale(5000);
	if (currentAnchorPts_.empty()) return;
	if (minChebToAnchor(bot.currentPos) > VICTIM_TOWN_RANGE) return;

	// Skull cap (current) + kill-limit headroom (mirror checkRandomPK).
	uint32_t skulled = 0, active = 0;
	for (const auto& b : bots_) {
		if (!b.active) continue;
		active++;
		auto p = b.getPlayer();
		if (p && p->getSkull() >= SKULL_WHITE) skulled++;
	}
	{
		uint8_t dayKills = 0, weekKills = 0, monthKills = 0;
		auto killTime = time(nullptr);
		for (const auto& kill : player->unjustifiedKills) {
			auto diff = killTime - kill.time;
			if (diff <= 4 * 60 * 60) dayKills++;
			if (diff <= 7 * 24 * 60 * 60) weekKills++;
			if (diff <= 30 * 24 * 60 * 60) monthKills++;
		}
		int32_t dayRed = g_configManager().getNumber(DAY_KILLS_TO_RED);
		int32_t weekRed = g_configManager().getNumber(WEEK_KILLS_TO_RED);
		int32_t monthRed = g_configManager().getNumber(MONTH_KILLS_TO_RED);
		bool oneFromRed = player->getSkull() < SKULL_RED &&
			(dayKills >= dayRed - 1 || weekKills >= weekRed - 1 || monthKills >= monthRed - 1);
		bool oneFromBlack = player->getSkull() < SKULL_BLACK &&
			(dayKills >= 2 * dayRed - 1 || weekKills >= 2 * weekRed - 1 || monthKills >= 2 * monthRed - 1);
		if (oneFromRed || oneFromBlack) return;
	}

	// Find a victim: an exposed player (or bot) standing just outside a PZ within this initiator's
	// town. Real players are scanned town-wide (few of them); bots via the local spectator cache.
	const bool targetPlayers = g_configManager().getBoolean(BOT_GANG_TARGET_PLAYERS);
	const int32_t victimBand = static_cast<int32_t>(g_configManager().getNumber(BOT_GANG_VICTIM_BAND));
	auto victimEligible = [&](const std::shared_ptr<Creature>& c) -> bool {
		if (!c || c->getID() == player->getID()) return false;
		if (c->isRemoved() || c->getHealth() <= 0) return false;
		auto vp = c->getPlayer();
		if (!vp) return false;
		if (vp->getGroup() && vp->getGroup()->access) return false;
		if (gangVictimAlreadyTargeted(c->getID())) return false; // one raid per victim at a time
		if (gangVictimOnCooldown(vp->getGUID())) return false;   // already jumped recently (cooldown)
		// Never raid a bot that belongs to somebody's PARTY. Observed live: a five-bot raid formed
		// on a druid that was following a real player, which yanked it out of PARTY state and
		// wrecked the player's party. The attacker-side filter has excluded party members since
		// 7d712c54f, but victim selection never had ANY party check — so a member could still be
		// the target. Covers human-led members (s_partyLeaderId), members mid-walk-in (s_rvMember),
		// and autonomous party-hunt members (s_botToPartyHunt).
		if (vp->isBotPlayer()) {
			const uint32_t vguid = vp->getGUID();
			if (s_partyLeaderId.count(vguid) > 0 || s_rvMember.count(vguid) > 0
			    || s_botToPartyHunt.count(vguid) > 0) {
				return false;
			}
			// Belt-and-braces on engine state, in case a map entry was cleared but the bot is
			// still following: PARTY is the authoritative "this bot belongs to someone" marker.
			if (auto vbIt = guidToIndex_.find(vguid); vbIt != guidToIndex_.end()) {
				const auto& vb = bots_[vbIt->second];
				if (vb.state == BotAIState::PARTY || vb.partyHuntId != 0) return false;
			}
		}
		auto vpos = c->getPosition();
		auto vt = g_game().map.getTile(vpos);
		if (!vt || vt->hasFlag(TILESTATE_PROTECTIONZONE)) return false; // must be OUTSIDE a PZ
		// Only raid victims who are IN A TOWN (within its footprint near the temple), never out in
		// the wilderness / hunting grounds. Bots are summoned to a hidden town PZ and walk in.
		auto vtown = g_game().map.towns.getTown(findNearestTown(vpos));
		if (!vtown) return false;
		int32_t dTemple = std::max(std::abs(static_cast<int32_t>(vpos.x) - static_cast<int32_t>(vtown->getTemplePosition().x)),
		                           std::abs(static_cast<int32_t>(vpos.y) - static_cast<int32_t>(vtown->getTemplePosition().y)));
		if (dTemple > 70) return false; // outside the town footprint
		return true;
	};

	std::shared_ptr<Creature> victim;
	bool victimIsBot = false;
	int32_t realInRange = 0;

	// 1) Real players, town-wide (the "raid on an exposed player" path). Pick the nearest eligible.
	if (targetPlayers) {
		int32_t oddsP = std::max(1, static_cast<int32_t>(g_configManager().getNumber(BOT_GANG_ODDS_VS_PLAYER)));
		std::shared_ptr<Player> best;
		int32_t bestD = 1 << 30;
		for (const auto& [id, pl] : g_game().getPlayers()) {
			if (!pl || pl->isBotPlayer()) continue;
			int32_t d = std::max(std::abs(static_cast<int32_t>(pl->getPosition().x) - static_cast<int32_t>(bot.currentPos.x)),
			                     std::abs(static_cast<int32_t>(pl->getPosition().y) - static_cast<int32_t>(bot.currentPos.y)));
			if (d > VICTIM_TOWN_RANGE) continue;
			realInRange++;
			if (!victimEligible(pl)) continue;
			if (d < bestD) { bestD = d; best = pl; }
		}
		if (best && uniform_random(1, oddsP) == 1) { victim = best; victimIsBot = false; }
	}
	// 2) Bot victim via the local spectator cache (fallback / when no real player is exposed).
	if (!victim) {
		refreshSpectatorCacheIfStale(bot);
		int32_t oddsB = std::max(1, static_cast<int32_t>(g_configManager().getNumber(BOT_GANG_ODDS_VS_BOT)));
		for (uint32_t pid : bot.cachedPlayerIds) {
			auto c = g_game().getCreatureByID(pid);
			if (!victimEligible(c) || !c->getPlayer()->isBotPlayer()) continue;
			if (uniform_random(1, oddsB) != 1) continue;
			victim = c;
			victimIsBot = true;
			break;
		}
	}
	if (!victim) return;
	(void)realInRange;
	// A bot victim still needs a real observer near it; a real-player victim is self-observed.
	if (victimIsBot && g_configManager().getBoolean(BOT_GANG_REQUIRE_OBSERVER)) {
		int32_t mc = minChebToAnchor(victim->getPosition());
		if (mc < 0 || mc > 12) return;
	}

	const bool victimSkulled = victim->getSkull() >= SKULL_WHITE;
	const int32_t victimLevel = victim->getPlayer()->getLevel();
	const Position V = victim->getPosition();

	// Gang size: at least minSize attackers, NO upper cap — we summon as many as needed (the gang
	// "raid" brings the required attacker bots in from the town to assemble near the victim).
	const int32_t minSize = std::max(2, static_cast<int32_t>(g_configManager().getNumber(BOT_GANG_MIN_SIZE)));

	// Gather candidate attackers: awake, non-combat bots in the initiator's town, ON THE SAME FLOOR
	// and within walking range of the victim (they WALK in — no teleport), nearest-to-victim first.
	struct GangCand { uint32_t guid; int32_t dist; };
	std::vector<GangCand> cands;
	const int32_t GANG_WALK_MAX = 35; // Chebyshev tiles a recruit may be from the victim (walkable in)
	// The initiator is just the scout — it joins only if it itself qualifies (same floor + in range).
	// Members are the nearest qualifying bots so every attacker can actually walk in.
	for (auto& b : bots_) {
		if (!b.active || b.hibernated) continue;
		if (b.partyHuntId != 0) continue;
		if (s_gangByGuid.count(b.guid)) continue;
		if (b.townId != bot.townId) continue; // same town as the initiator
		if (b.currentPos.z != V.z) continue;  // same floor (walk-in, no cross-z pathing)
		if (b.state == BotAIState::COMBAT || b.state == BotAIState::PK_ATTACK
		    || b.state == BotAIState::FLEEING || b.state == BotAIState::PARTY) continue;
		// BOT_PARTY_INVITE_RENDEZVOUS: never conscript a bot that belongs to a human-led party.
		// The state check above catches ARRIVED members (PARTY), but one still WALKING_IN is
		// state-IDLE with partyHuntId==0 and would otherwise be seatable — and gang staging
		// preempts earlier in doIdle than assembly staging does, so gang would win the race and
		// burst the member into PK_ATTACK on its way to its leader.
		if (s_rvMember.count(b.guid) > 0 || s_partyLeaderId.count(b.guid) > 0) continue;
		auto bp = b.getPlayer();
		if (!bp || bp->isPzLocked()) continue;
		if (bp->getID() == victim->getID()) continue; // never recruit the victim
		int32_t d = std::max(std::abs(static_cast<int32_t>(b.currentPos.x) - static_cast<int32_t>(V.x)),
		                     std::abs(static_cast<int32_t>(b.currentPos.y) - static_cast<int32_t>(V.y)));
		if (d > GANG_WALK_MAX) continue; // too far to walk in within the assembly window
		cands.push_back({ b.guid, d });
	}
	std::sort(cands.begin(), cands.end(), [](const GangCand& a, const GangCand& b) { return a.dist < b.dist; });
	std::vector<uint32_t> memberGuids;
	for (auto& c : cands) {
		if (static_cast<int32_t>(memberGuids.size()) >= minSize) break;
		memberGuids.push_back(c.guid);
	}
	if (static_cast<int32_t>(memberGuids.size()) < minSize) return; // not enough attackers in range

	const int32_t plannedUnjust = victimSkulled ? 0 : static_cast<int32_t>(memberGuids.size());
	if (active > 0 && static_cast<int32_t>(skulled + s_gangPlannedUnjustified + plannedUnjust) * 100 > static_cast<int32_t>(active) * 5) {
		return; // would exceed the 5% skull cap
	}

	// Seat members. Collect every usable ATTACK tile near the victim (non-PZ, walkable, LOS to the
	// victim), closest first, and pair each with a distinct nearby PZ STAGE tile (members teleport
	// to the stage, then step/walk out to the attack tile on the burst). This yields many seats so
	// a 5+ raid forms even when the victim's nearest PZ edge is short.
	auto tileUsable = [&](const Position& p) -> bool {
		if (p == V) return false; // never the victim's own sqm
		auto t = g_game().map.getTile(p);
		if (!t) return false;
		if (t->hasFlag(TILESTATE_PROTECTIONZONE) || t->hasFlag(TILESTATE_FLOORCHANGE)) return false;
		if (t->hasFlag(TILESTATE_BLOCKSOLID) || t->hasFlag(TILESTATE_BLOCKPATH)) return false;
		if (t->getCreatureCount() > 0) return false;                 // occupied (no stacking)
		if (s_gangClaimedTiles.count(gangPackPos(p))) return false;  // claimed by another raid
		if (!g_game().map.isSightClear(p, V, true)) return false;
		return true;
	};
	std::vector<std::pair<Position, int32_t>> attackList; // (tile, distToVictim)
	const int32_t AR = victimBand + 5;
	for (int32_t dx = -AR; dx <= AR; dx++) {
		for (int32_t dy = -AR; dy <= AR; dy++) {
			if (dx == 0 && dy == 0) continue;
			Position ap(static_cast<uint16_t>(static_cast<int32_t>(V.x) + dx), static_cast<uint16_t>(static_cast<int32_t>(V.y) + dy), V.z);
			if (!tileUsable(ap)) continue;
			attackList.push_back({ ap, std::max(std::abs(dx), std::abs(dy)) });
		}
	}
	std::sort(attackList.begin(), attackList.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

	// Seat members on distinct attack tiles near the victim. They WALK to these from wherever they
	// currently are (no teleport) and the barrier holds until all are in range, so they strike at
	// once. Distinct tiles + the global claim below keep two bots off the same sqm.
	std::vector<GangMember> seated;
	size_t ai = 0;
	for (uint32_t guid : memberGuids) {
		if (ai >= attackList.size()) break;
		int32_t lvl = 0;
		for (auto& b : bots_) {
			if (b.guid != guid) continue;
			auto bp = b.getPlayer();
			lvl = bp ? bp->getLevel() : static_cast<int32_t>(b.cachedLevel);
			break;
		}
		bool surrounder = (victimLevel <= 0) || (lvl > victimLevel / 2);
		GangMember m;
		m.guid = guid;
		m.attackPos = attackList[ai++].first; // walks here from its current spot, attacks from here
		m.surrounder = surrounder;
		m.unjustified = !victimSkulled;
		seated.push_back(m);
	}
	if (static_cast<int32_t>(seated.size()) < minSize) return; // not enough seats — abort, no commit

	// Commit the session.
	GangSession sess;
	sess.id = s_gangSessionSeq++;
	sess.targetId = victim->getID();
	sess.members = seated;
	// Assembly window: members WALK in from nearby (≤35 sqm), so a short window is enough before
	// forcing the strike / giving up on stragglers.
	sess.burstDeadline = now + std::max<int64_t>(15000, g_configManager().getNumber(BOT_GANG_STAGE_WINDOW_MS));
	sess.engaged = false;
	sess.noAoe = true;
	sess.openMode = static_cast<uint8_t>(uniform_random(0, 1)); // 50/50 nuke-first vs engage-then-trap
	sess.nukeHoldUntil = 0;
	// Remember the raid party as recent allies (mutually) so they never turn on each other after the
	// kill, even once the session is gone and they're still white-skulled.
	const int64_t allyExpiry = now + GANG_ALLY_TTL_MS;
	for (auto& a : sess.members) {
		for (auto& b : sess.members) {
			if (a.guid != b.guid) s_gangRecentAllies[a.guid][b.guid] = allyExpiry;
		}
	}
	// Per-victim cooldown: this victim can't be jumped again until it expires (configurable).
	s_gangVictimCooldown[victim->getPlayer()->getGUID()] =
		now + g_configManager().getNumber(BOT_GANG_VICTIM_COOLDOWN_SEC) * 1000LL;
	for (auto& m : sess.members) {
		s_gangByGuid[m.guid] = sess.id;
		s_gangNoAoe.insert(m.guid);
		if (m.unjustified) s_gangPlannedUnjustified++;
		// Reserve this member's attack tile globally so no other raid seats a bot on the same sqm.
		s_gangClaimedTiles[gangPackPos(m.attackPos)] = m.guid;
		for (auto& b : bots_) {
			if (b.guid != m.guid) continue;
			// Release any hunt/travel reservation so joining the raid doesn't strand a hunt lock.
			if (b.huntScriptId > 0) {
				activeHunts_.erase(b.huntScriptId);
				for (auto& s : huntScripts_) if (s.id == b.huntScriptId && !s.spawnGroup.empty()) { activeSpawnGroups_.erase(s.spawnGroup); break; }
				b.huntScriptId = 0;
			}
			b.pendingNavDest.clear();
			b.hasWalkTarget = false;
			b.currentPOI = nullptr;
			b.followingCityRoute = false;
			b.travelDestTownId = 0;
			b.travelPhase.clear();
			b.state = BotAIState::IDLE;
			b.dwellUntil = 0;
			// No teleport: handleGangStaging walks the member from its CURRENT position to its attack
			// tile, then the barrier fires the synchronized strike once all are in range.
			break;
		}
	}
	const uint8_t logMode = sess.openMode;
	// Diagnostic: a raid must never contain a party-bound bot on EITHER side. Live, four members of
	// a human-led party were seated as attackers against the fifth even though two independent
	// filters (state==PARTY and s_partyLeaderId) should have excluded them — cause not yet
	// established, so make it impossible to miss next time rather than assuming it is fixed.
	{
		std::string partyBound;
		for (uint32_t g : memberGuids) {
			if (s_partyLeaderId.count(g) > 0 || s_rvMember.count(g) > 0 || s_botToPartyHunt.count(g) > 0) {
				if (!partyBound.empty()) partyBound += ",";
				partyBound += std::to_string(g);
			}
		}
		if (!partyBound.empty()) {
			g_logger().error("[GANG] BUG: raid seated PARTY-BOUND attackers guids=[{}] victim='{}' "
				"— the candidate filter was bypassed", partyBound, victim->getName());
		}
	}
	s_gangSessions[sess.id] = std::move(sess);
	s_gangLastInitTickMs = now;
	g_logger().info("[GANG] formed RAID size={} victim='{}' ({}) mode={} town={} (skulled={}) — attackers walk in",
		seated.size(), victim->getName(), victimIsBot ? "bot" : "player", logMode, bot.townId, victimSkulled);
}

// Stage->barrier->burst for one gang member. Returns true if it consumed the tick.
bool BotEngine::handleGangStaging(BotState& bot) {
	auto sgIt = s_gangByGuid.find(bot.guid);
	if (sgIt == s_gangByGuid.end()) return false;
	auto seIt = s_gangSessions.find(sgIt->second);
	if (seIt == s_gangSessions.end()) { leaveGang(bot.guid); return false; }
	auto player = bot.getPlayer();
	if (!player) { leaveGang(bot.guid); return false; }
	GangSession& sess = seIt->second;
	int64_t now = OTSYS_TIME();

	GangMember* me = nullptr;
	for (auto& m : sess.members) if (m.guid == bot.guid) { me = &m; break; }
	if (!me) { leaveGang(bot.guid); return false; }

	// Validate the victim is still alive, online, and not ducked into a PZ. Hard assembly lifetime:
	// if nobody has struck by the deadline + grace, give up (frees the victim for a future raid).
	auto victim = g_game().getCreatureByID(sess.targetId);
	bool ok = victim && !victim->isRemoved() && victim->getHealth() > 0;
	if (ok) {
		auto vt = g_game().map.getTile(victim->getPosition());
		if (!vt || vt->hasFlag(TILESTATE_PROTECTIONZONE)) ok = false; // victim ducked into a PZ
	}
	if (!ok || player->isPzLocked() || bot.state != BotAIState::IDLE || now > sess.burstDeadline + 12000) {
		leaveGang(bot.guid);
		return false;
	}

	// Walk in from the member's CURRENT position toward the victim. "Ready" = standing OUTSIDE a PZ,
	// in range + LOS of the victim (able to open). The non-PZ requirement is critical: a bot cannot
	// legally attack from within a protection zone (Canary blocks it), so a member must fully step
	// out of the depot/temple PZ before it bursts — exactly like normal 1:1 PvP engagement. (Once it
	// lands its first hit it becomes pz-locked and can no longer re-enter a PZ.) The barrier holds
	// until ALL members are ready (or the deadline) so the whole raid strikes at once.
	const Position vpos = victim->getPosition();
	Position cur = player->getPosition();
	int32_t dToVic = std::max(std::abs(static_cast<int32_t>(cur.x) - static_cast<int32_t>(vpos.x)),
	                          std::abs(static_cast<int32_t>(cur.y) - static_cast<int32_t>(vpos.y)));
	auto curTile = g_game().map.getTile(cur);
	bool curInPz = curTile && curTile->hasFlag(TILESTATE_PROTECTIONZONE);
	bool canHit = !curInPz && (cur.z == vpos.z) && dToVic <= RUNE_RANGE && g_game().map.isSightClear(cur, vpos, true);

	if (!sess.engaged) {
		me->ready = canHit;
		// Chase the victim's CURRENT position (not a stale attack tile) so a moving victim is hunted
		// down. canHit stops the approach once in range + LOS on a non-PZ tile.
		if (!canHit && player->listWalkDir.empty()) goToWithDoors(bot, vpos, 1);
		bool allReady = true;
		for (auto& m : sess.members) if (!m.ready) { allReady = false; break; }
		if (allReady || now >= sess.burstDeadline) {
			sess.engaged = true;
			if (sess.openMode == 1) sess.nukeHoldUntil = now + 1500;
		} else {
			return true; // still assembling (walking in toward the victim)
		}
	}

	// Engaged: strike when in range+LOS; otherwise keep closing on the victim (bounded by the grace).
	if (!canHit) {
		if (player->listWalkDir.empty()) goToWithDoors(bot, vpos, 1);
		return true;
	}

	bot.currentPos = cur;
	player->setSecureMode(false);
	bot.state = BotAIState::PK_ATTACK;
	bot.pkTarget = sess.targetId;
	bot.combatStartTime = now;
	bot.lastCombatProgress = now;
	bot.combatHpCheckTime = now;
	bot.combatHpBaseline = 0;
	bot.combatStalemateCount = 0;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	bot.pvpManaSpent = 0;
	bot.preCombatPos = bot.currentPos;
	bot.hasPCPos = true;
	bot.lastAttackTime = 0;            // pass the 2s castSpell gate for the opening nuke
	bot.lastPvpAttackTime = now;
	player->setAttackedCreature(victim); // weapon auto-attack engage (only ever the victim)
	s_returnPos.erase(bot.guid);
	s_returnStartTime.erase(bot.guid);

	if (sess.openMode == 0) {
		castSpell(bot, victim);
		g_logger().info("[GANG] BURST guid={} mode=0 nuke-first on '{}'", bot.guid, victim->getName());
	} else {
		g_logger().info("[GANG] BURST guid={} mode=1 engage-then-trap on '{}'", bot.guid, victim->getName());
	}
	// Keep the session ALIVE — maintainGangBox + paralyze run from doPKAttack for its duration.
	return true;
}

// Surround + escape-tile magic-wall maintenance, called from doPKAttack for gang members only.
void BotEngine::maintainGangBox(BotState& bot) {
	auto sgIt = s_gangByGuid.find(bot.guid);
	if (sgIt == s_gangByGuid.end()) return;
	auto seIt = s_gangSessions.find(sgIt->second);
	if (seIt == s_gangSessions.end()) { leaveGang(bot.guid); return; }
	GangSession& sess = seIt->second;
	auto player = bot.getPlayer();
	if (!player) return;
	auto victim = g_game().getCreatureByID(sess.targetId);
	if (!victim || victim->isRemoved() || victim->getHealth() <= 0) return;
	const Position V = victim->getPosition();
	if (V.z != bot.currentPos.z) return;
	int64_t now = OTSYS_TIME();

	// Reap expired wall claims (frees tiles of dead/stuck claimants).
	for (auto it = sess.wallClaims.begin(); it != sess.wallClaims.end();) {
		it = (now > it->second.expiry) ? sess.wallClaims.erase(it) : std::next(it);
	}

	GangMember* me = nullptr;
	for (auto& m : sess.members) if (m.guid == bot.guid) { me = &m; break; }
	if (!me) return;
	// Keep the box body engaged — but never auto-attack from inside a PZ or against a victim in one.
	if (me->surrounder) {
		auto selfTile = g_game().map.getTile(bot.currentPos);
		auto vTile = g_game().map.getTile(V);
		bool pzBlocked = (selfTile && selfTile->hasFlag(TILESTATE_PROTECTIONZONE))
			|| (vTile && vTile->hasFlag(TILESTATE_PROTECTIONZONE));
		if (pzBlocked) { if (player->getAttackedCreature()) player->setAttackedCreature(nullptr); }
		else player->setAttackedCreature(victim);
	}

	// Wall-capable check + per-bot box-roll throttle (so we don't roll every 100ms tick).
	if (player->getLevel() < 32 || player->getMagicLevel() < 9) return;
	auto wcd = s_pvpWallCd.find(bot.guid);
	if (wcd != s_pvpWallCd.end() && now < wcd->second) return;
	auto brIt = s_gangBoxRollNext.find(bot.guid);
	if (brIt != s_gangBoxRollNext.end() && now < brIt->second) return;

	auto clampStep = [](int32_t v) -> int32_t { return v > 0 ? 1 : (v < 0 ? -1 : 0); };
	// Tiles to NEVER wall: the tile adjacent to the victim on each ranged member's line of fire
	// (a magic wall there blocks BLOCKPROJECTILE and would blind that attacker).
	auto isProtected = [&](const Position& W) -> bool {
		for (auto& m : sess.members) {
			if (m.surrounder) continue;
			Position R;
			bool found = false;
			for (auto& b : bots_) if (b.guid == m.guid) { R = b.currentPos; found = true; break; }
			if (!found) continue;
			int32_t dR = std::max(std::abs(static_cast<int32_t>(R.x) - static_cast<int32_t>(V.x)),
			                      std::abs(static_cast<int32_t>(R.y) - static_cast<int32_t>(V.y)));
			if (dR <= 1) continue;
			Position blockTile(static_cast<uint16_t>(V.x + clampStep(static_cast<int32_t>(R.x) - static_cast<int32_t>(V.x))),
			                   static_cast<uint16_t>(V.y + clampStep(static_cast<int32_t>(R.y) - static_cast<int32_t>(V.y))), V.z);
			if (W == blockTile) return true;
		}
		return false;
	};

	static const int32_t dxA[] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	static const int32_t dyA[] = { -1, 0, 1, 0, -1, 1, 1, -1 };
	std::vector<Position> openTiles;
	for (int d = 0; d < 8; d++) {
		Position W(static_cast<uint16_t>(V.x + dxA[d]), static_cast<uint16_t>(V.y + dyA[d]), V.z);
		auto t = g_game().map.getTile(W);
		if (!t) continue;
		if (t->hasFlag(TILESTATE_PROTECTIONZONE) || t->hasFlag(TILESTATE_FLOORCHANGE)) continue;
		if (t->getTopVisibleCreature(nullptr) != nullptr) continue; // body already blocks it
		if (!g_game().map.canWalkTo(player, W)) continue;            // not open (already a wall, etc.)
		if (isProtected(W)) continue;
		int32_t wd = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(W.x)),
		                      std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(W.y)));
		if (wd > RUNE_RANGE) continue;
		if (!g_game().map.isSightClear(bot.currentPos, W, true)) continue;
		uint64_t key = gangPackPos(W);
		auto ci = sess.wallClaims.find(key);
		if (ci != sess.wallClaims.end() && ci->second.guid != bot.guid && now < ci->second.expiry) continue;
		openTiles.push_back(W);
	}
	if (openTiles.empty()) return;

	int32_t wallPct = static_cast<int32_t>(g_configManager().getNumber(BOT_GANG_WALL_CHANCE_PCT));
	if (uniform_random(1, 100) > wallPct) {
		s_gangBoxRollNext[bot.guid] = now + 1000; // throttle only — DO NOT burn the wall cooldown
		return;
	}
	Position W = openTiles[uniform_random(0, static_cast<int32_t>(openTiles.size()) - 1)];
	sess.wallClaims[gangPackPos(W)] = { bot.guid, now + 2000 };
	pvpPlaceWallAt(bot, W);
	s_gangBoxRollNext[bot.guid] = now + 500;
}

// ED-only paralyze rune on a fleeing victim, called from doPKAttack for gang members only.
void BotEngine::gangParalyzeIfFleeing(BotState& bot) {
	auto sgIt = s_gangByGuid.find(bot.guid);
	if (sgIt == s_gangByGuid.end()) return;
	if (getBaseVocation(bot.vocationId) != 2) return; // druid / elder druid only
	auto seIt = s_gangSessions.find(sgIt->second);
	if (seIt == s_gangSessions.end()) return;
	auto player = bot.getPlayer();
	if (!player) return;
	auto victim = g_game().getCreatureByID(seIt->second.targetId);
	if (!victim || victim->isRemoved() || victim->getHealth() <= 0) return;
	const Position V = victim->getPosition();
	if (V.z != bot.currentPos.z) return;

	// Paralyze when the victim is MOVING (walking/kiting/circling), not only fleeing straight away:
	// fire if the victim's position changed since this bot's last check.
	auto& lastPos = s_gangVictimLastPos[bot.guid];
	bool moving = (lastPos.x != 0) && !(lastPos == V);
	lastPos = V;
	if (!moving) return;

	int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(V.x)),
	                        std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(V.y)));
	if (dist > RUNE_RANGE) return;
	auto rune = findRuneInBackpack(player, 3165);
	if (!rune) return;
	auto rs = g_spells().getRuneSpell(3165);
	if (!rs) return;
	if (player->getLevel() < static_cast<int32_t>(rs->getLevel())) return;
	if (static_cast<uint32_t>(player->getMagicLevel()) < rs->getMagicLevel()) return;
	if (player->hasCondition(CONDITION_SPELLCOOLDOWN, rs->getSpellId())) return;
	if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, rs->getGroup())) return;
	if (!g_game().map.isSightClear(bot.currentPos, V, true)) return;
	int32_t pct = static_cast<int32_t>(g_configManager().getNumber(BOT_GANG_PARALYZE_CHANCE_PCT));
	if (uniform_random(1, 100) > pct) return;
	if (executeRune(bot, rune, V, victim)) {
		bot.lastPvpAttackTime = OTSYS_TIME();
		g_logger().info("[GANG] PARALYZE guid={} on moving victim '{}'", bot.guid, victim->getName());
	}
}

void BotEngine::doHealing(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	int32_t hp = player->getHealth();
	int32_t maxHp = player->getMaxHealth();
	if (maxHp <= 0) return;

	// Three-mode heal ladder (selected by HP and paralysis status):
	//   dispelOnly  — paralyzed at full HP: pick strongest heal sharing the minimum CD
	//                 (cures paralysis without burning long cooldowns; all heal spells in
	//                 Canary set COMBAT_PARAM_DISPEL=CONDITION_PARALYZE).
	//   emergency   — HP <= HEAL_EMERGENCY_PCT: unlock long-CD heals (e.g. exura gran ico).
	//   routine     — HP <= HEAL_THRESHOLD_PCT: only short-CD heals (skip emergency spells).
	bool paralyzed = player->hasCondition(CONDITION_PARALYZE) &&
		bot.state != BotAIState::COMBAT && bot.state != BotAIState::PK_ATTACK;
	int32_t hpPct = hp * 100 / maxHp;
	bool needHeal  = hpPct <= HEAL_THRESHOLD_PCT;
	bool emergency = hpPct <= HEAL_EMERGENCY_PCT;
	if (!needHeal && !paralyzed) return;

	int64_t now = OTSYS_TIME();
	if (now - bot.lastHealTime < HEAL_COOLDOWN * 1000) return;

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	auto& healSpells = getHealSpells(baseVoc);

	if (healSpells.empty()) {
		castLog(bot, fmt::format("HEAL: No heal spells for voc={} (base={})", bot.vocationId, baseVoc));
		bot.lastHealTime = now; // prevent spam
		return;
	}

	int32_t level = player->getLevel();

	bool dispelOnly = paralyzed && !needHeal;
	int32_t maxAcceptableCd;
	if (dispelOnly) {
		// Shortest-CD heals only — free dispel without burning long CDs (e.g. exura max vita 6s, gran ico 600s)
		maxAcceptableCd = INT32_MAX;
		for (const auto& s : healSpells) if (s.cd < maxAcceptableCd) maxAcceptableCd = s.cd;
	} else if (emergency) {
		maxAcceptableCd = INT32_MAX;  // unlock long-CD emergency heals
	} else {
		maxAcceptableCd = HEAL_LONG_CD_THRESHOLD_S;  // routine: skip emergency-only spells
	}

	// Pick strongest available spell (iterate backwards = strongest first)
	// Server manages cooldowns via CONDITION_SPELLCOOLDOWN
	for (int i = static_cast<int>(healSpells.size()) - 1; i >= 0; i--) {
		if (level < healSpells[i].level) continue;
		if (healSpells[i].cd > maxAcceptableCd) continue;

		const auto& instantSpell = g_spells().getInstantSpell(healSpells[i].name);
		if (!instantSpell) continue;
		if (player->hasCondition(CONDITION_SPELLCOOLDOWN, instantSpell->getSpellId())) continue;
		if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, instantSpell->getGroup())) continue;

		// Check mana cost before casting — restore if needed (PvP budgeted, PvE unlimited)
		uint32_t manaCost = instantSpell->getManaCost(player);
		if (player->getMana() < manaCost) {
			bool inPvP = bot.attackerId > 0 && g_game().getPlayerByID(bot.attackerId) != nullptr;
			if (inPvP) {
				uint32_t maxMana = player->getMaxMana();
				uint32_t budget = maxMana * 5 / 2;
				if (bot.pvpManaSpent < budget) {
					player->mana = player->getMaxMana();
					g_game().addPlayerMana(player);
					bot.pvpManaSpent += maxMana;
				} else {
					continue; // PvP mana budget exceeded, try cheaper spell
				}
			} else {
				player->mana = player->getMaxMana();
				g_game().addPlayerMana(player);
			}
		}

		// Cast via server spell system — handles heal amount, mana deduction, cooldowns
		std::string words = healSpells[i].name;
		auto result = g_spells().playerSaySpell(player, words);
		if (result == TALKACTION_BREAK) {
			bot.lastHealTime = now;
			player->saySpell(TALKTYPE_SAY, words, false);
			const char* mode = dispelOnly ? " (paralysis cure)" : (emergency ? " (emergency)" : "");
			castLog(bot, fmt::format("HEAL: {} ({}/{}){}", words,
				player->getHealth(), player->getMaxHealth(), mode));
			return;
		}
	}
}

void BotEngine::exitCombat(BotState& bot) {
	// BOT_LURE_KITE hygiene. Correctness does NOT depend on this — both features
	// re-validate their own gate every tick and self-clear (beginHuntPhase is
	// explicitly not a funnel). This just keeps stale entries from lingering.
	clearLureKiteState(bot.guid);

	auto player = bot.getPlayer();
	if (player) {
		player->setAttackedCreature(nullptr);
		player->setFollowCreature(nullptr);
		player->setSecureMode(true);
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
	}

	castLog(bot, "COMBAT END: Exiting combat");

	// Post-combat PK immunity: remember who we fought (before clearing attackerId)
	if (bot.attackerId > 0) {
		s_lastFoughtCreature[bot.guid] = bot.attackerId;
		s_lastCombatExitTime[bot.guid] = OTSYS_TIME();
	}

	// Return walk: save pre-combat position if not resuming a hunt
	if (bot.hasPCPos && bot.huntScriptId == 0) {
		s_returnPos[bot.guid] = bot.preCombatPos;
		s_returnStartTime[bot.guid] = OTSYS_TIME();
	}

	bot.combatDecision.clear();
	bot.combatStartTime = 0;
	bot.lastCombatProgress = 0;
	bot.attackerId = 0;
	bot.ignoredAttackerId = 0;
	bot.ignoredHitBack = false;
	bot.pvpManaSpent = 0;
	bot.hasPCPos = false;
	bot.combatStalemateCount = 0;

	// Clear flee-to-PZ state
	bot.hasFleeTarget = false;
	bot.fleeDirectional = false;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.triedRouteSources.clear();
	bot.lastRouteDestination.clear();

	// Adventurer's Stone trip cleanup — combat interrupting a trip should abort it
	if (bot.advStoneActive) {
		castLog(bot, "COMBAT END: aborting Adventurer's Stone trip");
		endAdventurerStoneTrip(bot);
	}

	// Clear z-pursuit + combat tracking
	s_lastZPursuitTime.erase(bot.guid);
	s_targetLastSameZPos.erase(bot.guid);
	s_lastAttackerSeenTime.erase(bot.guid);
	s_combatNoTargetSince.erase(bot.guid);
	s_targetTrail.erase(bot.guid);
	s_inRangeSince.erase(bot.guid);
	s_inRangeAttackSnapshot.erase(bot.guid);
	s_retreatUntil.erase(bot.guid);
	s_approachCooldown.erase(bot.guid);
	s_spreadCooldown.erase(bot.guid);
	s_pvpDanceCd.erase(bot.guid);
	s_pvpHasteCd.erase(bot.guid);
	s_pvpWallCd.erase(bot.guid);
	s_pvpPendingWall.erase(bot.guid);
	s_gangVictimLastDist.erase(bot.guid);
	s_gangFleeStreak.erase(bot.guid);
	s_gangBoxRollNext.erase(bot.guid);
	s_gangVictimLastPos.erase(bot.guid);
	leaveGang(bot.guid); // Feature 1: drop gang membership if combat ends via exitCombat

	// Resume hunting if in progress
	if (bot.huntScriptId > 0) {
		bot.state = BotAIState::HUNTING;
		return;
	}

	bot.state = BotAIState::IDLE;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
}

void BotEngine::exitPK(BotState& bot) {
	auto player = bot.getPlayer();
	if (player) {
		player->setAttackedCreature(nullptr);
		player->setFollowCreature(nullptr);
		player->setSecureMode(true);
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
	}

	// Post-combat PK immunity: remember who we fought (before clearing pkTarget)
	if (bot.pkTarget > 0) {
		s_lastFoughtCreature[bot.guid] = bot.pkTarget;
		s_lastCombatExitTime[bot.guid] = OTSYS_TIME();
	}

	// Save return position for post-PK walk-back (non-hunting bots only)
	if (bot.hasPCPos && bot.huntScriptId == 0) {
		s_returnPos[bot.guid] = bot.preCombatPos;
		s_returnStartTime[bot.guid] = OTSYS_TIME();
	}

	bot.pkTarget = 0;
	bot.pvpManaSpent = 0;
	bot.combatStalemateCount = 0;

	// Clear z-pursuit + combat tracking
	s_lastZPursuitTime.erase(bot.guid);
	s_targetLastSameZPos.erase(bot.guid);
	s_lastAttackerSeenTime.erase(bot.guid);
	s_combatNoTargetSince.erase(bot.guid);
	s_targetTrail.erase(bot.guid);
	s_inRangeSince.erase(bot.guid);
	s_inRangeAttackSnapshot.erase(bot.guid);
	s_retreatUntil.erase(bot.guid);
	s_approachCooldown.erase(bot.guid);
	s_spreadCooldown.erase(bot.guid);
	s_pvpDanceCd.erase(bot.guid);
	s_pvpHasteCd.erase(bot.guid);
	s_pvpWallCd.erase(bot.guid);
	s_pvpPendingWall.erase(bot.guid);
	s_gangVictimLastDist.erase(bot.guid);
	s_gangFleeStreak.erase(bot.guid);
	s_gangBoxRollNext.erase(bot.guid);
	s_gangVictimLastPos.erase(bot.guid);
	leaveGang(bot.guid); // Feature 1: end gang membership when this member's PK ends

	// Adventurer's Stone trip cleanup — PK interrupting a trip should abort it
	if (bot.advStoneActive) {
		castLog(bot, "PK END: aborting Adventurer's Stone trip");
		endAdventurerStoneTrip(bot);
	}

	bot.state = BotAIState::IDLE;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
}

void BotEngine::checkVigilante(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	int64_t now = OTSYS_TIME();
	if (now - bot.lastPKerScanTime < 2000) return;
	bot.lastPKerScanTime = now;

	// Don't attack within 60s of respawn
	if (bot.lastDeathTime > 0 && now - bot.lastDeathTime < 60000) return;

	// Cooling down: a bot that just fought (pz-locked) does NOT initiate new PvP — it chills/roams
	// until the flag clears. This stops a gang from vigilante-ing each other right after a kill.
	if (player->isPzLocked()) return;

	// Don't attack from PZ
	auto myTile = g_game().map.getTile(bot.currentPos);
	if (myTile && myTile->hasFlag(TILESTATE_PROTECTIONZONE)) return;

	// 5% skull cap: skip if >=5% of active bots already have any skull (white or higher)
	{
		uint32_t skulled = 0, active = 0;
		for (const auto& b : bots_) {
			if (!b.active) continue;
			active++;
			auto p = b.getPlayer();
			if (p && p->getSkull() >= SKULL_WHITE) skulled++;
		}
		if (active > 0 && skulled * 100 >= active * 5) return;
	}

	// Post-combat PK immunity: don't re-engage recently fought targets
	uint32_t vigFoughtId = 0; int64_t vigFoughtTime = 0;
	{ auto fi = s_lastFoughtCreature.find(bot.guid);
	  if (fi != s_lastFoughtCreature.end()) { vigFoughtId = fi->second;
	    auto ei = s_lastCombatExitTime.find(bot.guid);
	    vigFoughtTime = ei != s_lastCombatExitTime.end() ? ei->second : 0; } }

	// Uses cached spectators (target selection cadence, 600ms TTL).
	refreshSpectatorCacheIfStale(bot);

	for (uint32_t pid : bot.cachedPlayerIds) {
		auto creature = g_game().getCreatureByID(pid);
		if (!creature) continue;
		if (creature->getID() == player->getID()) continue;
		if (creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto p = creature->getPlayer();
		if (!p) continue;
		if (p->getGroup() && p->getGroup()->access) continue;
		// Never vigilante a recent gang-mate (a raid leaves all members white-skulled — without this
		// they'd hunt each other after the kill). Bots DO still vigilante any other skulled bot.
		if (gangIsRecentAlly(bot.guid, p->getGUID())) continue;
		// Never vigilante a bot that belongs to somebody's party. Party PvP assist leaves members
		// white-skulled, which would otherwise make them bait — and as PARTY-state bots they do
		// not run doSelfDefense, so it would be a one-sided fight against a target that heals
		// through it and only ends on a stalemate timeout.
		if (s_partyLeaderId.count(p->getGUID()) > 0 || s_botToPartyHunt.count(p->getGUID()) > 0) continue;

		Skulls_t skull = creature->getSkull();
		if (skull < SKULL_WHITE) continue;

		auto tpos = creature->getPosition();
		if (tpos.z != bot.currentPos.z) continue;

		auto tTile = g_game().map.getTile(tpos);
		if (tTile && tTile->hasFlag(TILESTATE_PROTECTIONZONE)) continue;

		// Level-based ignore: if bot outlevels PKer by 6x+, skip entirely
		int32_t pkerLevel = p->getLevel();
		int32_t botLevel = player->getLevel();
		if (pkerLevel > 0 && botLevel >= pkerLevel * 6) continue;

		uint32_t cid = creature->getID();

		// Post-combat immunity: skip recently fought targets (120s cooldown)
		if (vigFoughtId > 0 && cid == vigFoughtId && now - vigFoughtTime < 120000) continue;

		// Already seen?
		if (bot.seenPKers.count(cid)) continue;

		// 5% vigilante roll
		if (uniform_random(1, 20) == 1) {
			player->setSecureMode(false);  // Allow attacking skulled player
			bot.state = BotAIState::COMBAT;
			bot.combatDecision = "fight";
			bot.combatStartTime = now;
			bot.lastCombatProgress = now;
			bot.attackerId = cid;
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			bot.preCombatPos = bot.currentPos;
			bot.hasPCPos = true;
			bot.pvpManaSpent = 0;

			// Clear return walk on new combat entry
			s_returnPos.erase(bot.guid);
			s_returnStartTime.erase(bot.guid);

			castLog(bot, fmt::format("VIGILANTE: Attacking PKer {} (skull={})",
				creature->getName(), static_cast<int>(creature->getSkull())));
			return;
		} else {
			bot.seenPKers[cid] = now;
		}
	}

	// Clean up seenPKers: remove entries for PKers who left visible range 60s+ ago
	for (auto it = bot.seenPKers.begin(); it != bot.seenPKers.end(); ) {
		auto pker = g_game().getCreatureByID(it->first);
		bool visible = false;
		if (pker && !pker->isRemoved()) {
			auto ppos = pker->getPosition();
			int32_t d = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(ppos.x)),
								 std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(ppos.y)));
			visible = (d <= 7 && ppos.z == bot.currentPos.z);
		}
		if (visible) {
			it->second = now;
			++it;
		} else if (now - it->second > 60000) {
			it = bot.seenPKers.erase(it);
		} else {
			++it;
		}
	}
}

// ============================================================================
// Random PK (Phase 3)
// ============================================================================

void BotEngine::checkRandomPK(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player || bot.state != BotAIState::IDLE) return;

	int64_t now = OTSYS_TIME();

	// Don't PK within 60s of dying
	if (bot.lastDeathTime > 0 && now - bot.lastDeathTime < 60000) return;

	// Cooling down: a pz-locked bot (just fought) doesn't start a new PK — it chills until clear.
	if (player->isPzLocked()) return;

	// Don't PK from PZ
	auto myTile = g_game().map.getTile(bot.currentPos);
	if (myTile && myTile->hasFlag(TILESTATE_PROTECTIONZONE)) return;

	// 5% skull cap: skip if >=5% of active bots already have any skull (white or higher)
	uint32_t skulled = 0, active = 0;
	for (const auto& b : bots_) {
		if (!b.active) continue;
		active++;
		auto p = b.getPlayer();
		if (p && p->getSkull() >= SKULL_WHITE) skulled++;
	}
	if (active > 0 && skulled * 100 >= active * 5) return;

	// Skip if bot is 1 kill away from red or black skull in any timeframe
	{
		uint8_t dayKills = 0, weekKills = 0, monthKills = 0;
		auto killTime = time(nullptr);
		for (const auto& kill : player->unjustifiedKills) {
			auto diff = killTime - kill.time;
			if (diff <= 4 * 60 * 60) dayKills++;
			if (diff <= 7 * 24 * 60 * 60) weekKills++;
			if (diff <= 30 * 24 * 60 * 60) monthKills++;
		}
		int32_t dayRed = g_configManager().getNumber(DAY_KILLS_TO_RED);
		int32_t weekRed = g_configManager().getNumber(WEEK_KILLS_TO_RED);
		int32_t monthRed = g_configManager().getNumber(MONTH_KILLS_TO_RED);

		bool oneFromRed = player->getSkull() < SKULL_RED &&
			(dayKills >= dayRed - 1 || weekKills >= weekRed - 1 || monthKills >= monthRed - 1);
		bool oneFromBlack = player->getSkull() < SKULL_BLACK &&
			(dayKills >= 2 * dayRed - 1 || weekKills >= 2 * weekRed - 1 || monthKills >= 2 * monthRed - 1);

		if (oneFromRed || oneFromBlack) {
			// 1/100 override chance — still PK despite being close to skull upgrade
			if (uniform_random(1, 100) != 1) {
				return; // 99% skip
			}
		}
	}

	// Post-combat PK immunity: don't re-engage recently fought targets
	uint32_t pkFoughtId = 0; int64_t pkFoughtTime = 0;
	{ auto fi = s_lastFoughtCreature.find(bot.guid);
	  if (fi != s_lastFoughtCreature.end()) { pkFoughtId = fi->second;
	    auto ei = s_lastCombatExitTime.find(bot.guid);
	    pkFoughtTime = ei != s_lastCombatExitTime.end() ? ei->second : 0; } }

	// Find random target from spectators — uses cached spectators (target selection cadence, 600ms TTL).
	refreshSpectatorCacheIfStale(bot);
	std::vector<std::shared_ptr<Player>> candidates;
	for (uint32_t pid : bot.cachedPlayerIds) {
		auto creature = g_game().getCreatureByID(pid);
		if (!creature) continue;
		if (creature->getID() == player->getID()) continue;
		if (creature->isRemoved() || creature->getHealth() <= 0) continue;
		auto p = creature->getPlayer();
		if (!p) continue;
		if (p->getGroup() && p->getGroup()->access) continue;
		if (gangIsRecentAlly(bot.guid, p->getGUID())) continue; // never PK a recent gang-mate
		auto tpos = creature->getPosition();
		if (tpos.z != bot.currentPos.z) continue;
		auto tTile = g_game().map.getTile(tpos);
		if (tTile && tTile->hasFlag(TILESTATE_PROTECTIONZONE)) continue;
		// Post-combat immunity: skip recently fought targets (120s cooldown)
		if (pkFoughtId > 0 && creature->getID() == pkFoughtId && now - pkFoughtTime < 120000) continue;
		candidates.push_back(p);
	}
	if (candidates.empty()) return;

	// Pick random target and roll appropriate chance
	auto& target = candidates[uniform_random(0, static_cast<int>(candidates.size()) - 1)];
	bool isBot = target->isBotPlayer();
	int32_t chance = isBot ? 4000 : 400;
	if (uniform_random(1, chance) != 1) return;

	// Enter PK_ATTACK
	player->setSecureMode(false);  // Allow attacking unmarked players
	bot.state = BotAIState::PK_ATTACK;
	bot.pkTarget = target->getID();
	bot.combatStartTime = now;
	bot.lastCombatProgress = now;
	bot.combatHpCheckTime = now;
	bot.combatHpBaseline = 0;
	bot.combatStalemateCount = 0;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	bot.pvpManaSpent = 0;
	bot.preCombatPos = bot.currentPos;
	bot.hasPCPos = true;

	// Clear return walk on new PK entry
	s_returnPos.erase(bot.guid);
	s_returnStartTime.erase(bot.guid);

	castLog(bot, fmt::format("RANDOM PK: Targeting {} ({})",
		target->getName(), isBot ? "bot" : "player"));
}

