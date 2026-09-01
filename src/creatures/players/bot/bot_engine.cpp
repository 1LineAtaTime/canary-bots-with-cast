/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_engine.cpp — bot lifecycle: registration, activation, hibernation/wake,
// spawn-claim, population scheduling, liveness diagnostics, state persistence and
// the .so factory functions.
//
// BOT_NAV_REALISM Phase 11: the shared includes / types / class declaration now
// live in bot_engine_impl.hpp so the engine can span multiple translation units
// that still link into the single hot-reloadable libbot_engine.so. The chat
// subsystem lives in bot_chat.cpp.
//
// Phase 12 split this file again — what used to be "lifecycle, tick, virtual sim,
// combat/PvP" is now four TUs: the tick loop and v2 virtual simulator moved to
// bot_tick.cpp, combat/spells/PvP/gang-PK to bot_combat.cpp, the debug stream and
// cast-chat status text to bot_debug.cpp, and the static data tables to
// bot_data.cpp (which already owned the DB/config loaders).
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"


// PARTY_HUNT lifecycle coupling (2026-06-14): mirror of s_inPartyCascade for the HIBERNATE
// side. hibernateBot blocks an independent (non-leader) party member from hibernating on its
// own — a support's lifecycle must follow its leader (leader hibernate → whole-team cascade;
// leader wake → whole-team wake). But the leader cascade itself hibernates members by calling
// hibernateBot(memberGuid) recursively, which would hit that very block. This flag is set
// (RAII) around the cascade loop so the recursive member-hibernate calls bypass the block.
static thread_local bool s_inPartyHibernateCascade = false;
// Phase B: last [DENSITY] periodic-summary timestamp. 60s cadence.
static int64_t s_lastDensityPeriodicLogMs = 0;
// Per-bot last city route source POI (for accurate triedRouteSources tracking)
// Per-bot route waypoint stuck detection: tracks {waypoint index, start time}
// Per-bot route progress tracking: tracks {last-advanced waypoint index, timestamp of last advance}
// Used for 5-minute stuck safety valve — if no waypoint progress, bot is suspended




// ---- BOT_NAV_REALISM Phase 8 increment 3: teleport-fallback KPI ----
// Every bot teleport is a moment the bot does something a real player cannot. Counting them by the
// enclosing function (via __func__, so a manual tag can never drift out of date) establishes the
// baseline increment 4 is measured against, and shows which classes actually fire in practice.
// Pure instrumentation — behavior unchanged. The comma operator preserves internalTeleport's
// return value, so call sites that check it are unaffected.




// BOT_NAV_REALISM Phase 8 telemetry: multi-hop city routes served + summed leg count (the 5-min
// summary reports the mean). Each multi-hop route served is one teleport fallback avoided.
// Declared here (not next to the graph helpers further down) because tick() reads them.






// ---- LEAVING phase / TRAVEL_TO timers live in bot_engine_impl.hpp (shared by modules) ----

static constexpr int64_t LEAVING_WP_STUCK_MS = 30000;    // 30s max per waypoint

// ---- Party system state (stored outside BotState to avoid ABI change) ----


// ---- Autonomous party hunt system ----
// Party hunt roles
static constexpr uint8_t PARTY_ROLE_NONE       = 0;


static constexpr int32_t PARTY_HUNT_MAX_DEATHS = 3;            // dissolve after 3 party deaths with 0 kills
[[maybe_unused]] static constexpr int32_t PARTY_HUNT_AOE_MIN_IMPROVEMENT = 30;  // P8: retired (cast-in-place); kept for reference





// Personality mid-walk pause display info (outside BotState to avoid ABI change).
// guid → {startMs, durationMs}. Lifecycle is anchored to bot.pendingWalkPauseEventId:
// written when a pause is scheduled, erased everywhere that field returns to 0. The
// display gate in buildStatusDetail checks pendingWalkPauseEventId != 0 first, so a
// residual leaked entry (bounded ≤1/guid) can never render a phantom pause.

static int64_t s_lastProxPeriodicLogMs = 0;  // 60s [PROXBIAS] summary timer

// ---- Keep-distance retreat cooldown ----

// SEWER_ITEM_ID / SHOVEL_HOLE_IDS / ROPE_SPOT_IDS hoisted to bot_engine_impl.hpp
// (TRUE MULTI-FLOOR: shared with the portal-graph builder in bot_zgraph.cpp).

static constexpr uint16_t PICKAXE_ITEM_ID = 3456;


// ============================================================================
// Bot registration and lifecycle
// ============================================================================

void BotEngine::registerBot(const std::shared_ptr<Player>& player) {
	if (!player) return;

	// PERF STRESS HARNESS backstop. A probe flag lives on the Player and survives /cavebot
	// reload, but s_probeBots does not -- a fresh engine would have no record of the flag and
	// could never clear it, leaving a permanent fake observer holding its neighbours awake.
	// Every registration path funnels through here, so clearing it here closes that leak for
	// good. The harness re-arms its probes explicitly after a reload.
	player->setSyntheticCastViewers(0);

	uint32_t guid = player->getGUID();
	auto it = guidToIndex_.find(guid);
	if (it != guidToIndex_.end()) {
		bots_[it->second].playerRef = player;
		return;
	}

	BotState bot;
	bot.guid = guid;
	bot.name = player->getName();
	bot.playerRef = player;
	bot.active = false;
	bot.state = BotAIState::INACTIVE;
	bot.vocationId = static_cast<uint8_t>(player->getVocationId());
	bot.cachedLevel = static_cast<uint32_t>(player->getLevel());
	bot.townId = 0;

	// BOT_LIVENESS_PACK Phase B: derive 16-bit personality seed from a proper hash
	// mix of guid + server-start epoch. Naive (guid << 32) | epoch would correlate
	// across sequential guids in the bot account range — use a 64-bit golden-ratio
	// multiplier and high-bit fold. Per-restart freshness comes entirely from the
	// epoch mix-in. Seed is in-memory only; never persisted.
	static const int64_t s_serverStartEpoch = OTSYS_TIME();
	const uint64_t mixed = (static_cast<uint64_t>(guid) * 0x9E3779B97F4A7C15ULL)
	                       ^ static_cast<uint64_t>(s_serverStartEpoch);
	bot.personalitySeed = static_cast<uint16_t>((mixed >> 16) ^ (mixed >> 32));

	// BOT_LIVENESS_PACK Phase C.8 — STAGGER FIX (hotfix after 100% CPU at boot):
	// At 500 bots × all-zero timers, every dwell-tick after activation fired chat
	// emits in unison, each fanning out to ~500 channel subscribers = ~60k packet
	// sends in 1 second → 42s GAP_SLOW. Initialize per-bot timers to a random
	// offset across the full interval window so the first emit per bot is
	// uniformly distributed in time, not bursting at boot.
	const int64_t now0 = OTSYS_TIME();
	bot.nextWorldChatTime   = now0 + uniform_random(0, static_cast<int32_t>(g_configManager().getNumber(BOT_WORLD_CHAT_INTERVAL_MAX_MS)));
	bot.nextAdvertisingTime = now0 + uniform_random(0, static_cast<int32_t>(g_configManager().getNumber(BOT_ADVERTISING_INTERVAL_MAX_MS)));
	bot.fidgetStationarySince = 0;
	bot.fidgetRolledThisStop  = false;
	bot.fidgetDroppedThisWake = false;
	bot.nextTurnInPlaceTime = now0 + uniform_random(0, 30000);

	// Channel auto-join (Phase C.8). HOTFIX: gate by chattyness — only bots in
	// the top ~25% subscribe. At 500 bots that's ~125 subscribers per channel
	// instead of 500, reducing fanout 4×. Bots that don't subscribe also won't
	// emit because tryEmitChat -> g_chat().talkToChannel -> ChatChannel::talk()
	// fails the membership check (returns silently). Net: fewer broadcasters +
	// fewer recipients, same per-bot semantics for the chatty subset.
	if (bot.chattyness() >= 12) {  // 12..15 of 0..15 = ~25%
		g_chat().addUserToChannel(player, /*World Chat*/ 3);
		g_chat().addUserToChannel(player, /*Advertising*/ 5);
	}

	// JITTER FIX: stagger lastVirtualPosSave init across the 5-min throttle window.
	// Without this, all 200 bots register at startup with lastVirtualPosSave=0, so
	// after VIRTUAL_POS_SAVE_THROTTLE_MS (5min) ALL bots simultaneously fire DB writes
	// in maybeQueueVirtualPositionSave, causing ~190 async UPDATE enqueues on the
	// dispatcher in one virtualTick = predictable 600-900ms stall every 5 minutes.
	// Initializing to (now - random 0..5min) spreads the first save across the full
	// throttle window, and subsequent saves stay staggered naturally.
	bot.lastVirtualPosSave = OTSYS_TIME() - uniform_random(0, 300000);

	size_t index = bots_.size();
	bots_.push_back(std::move(bot));
	guidToIndex_[guid] = index;

	bots_[index].currentPos = player->getPosition();

	g_logger().info("[BotEngine] Registered bot '{}' (guid={}, voc={})",
		player->getName(), guid, bots_[index].vocationId);

	// Surface "loaded in engine" to the PHP @cast list (out-of-process via MySQL).
	// Independent of hibernation state and the botPlayersShowAsOnline config flag —
	// see data-otservbr-global/migrations/60.lua for the table contract.
	g_botDatabaseTasks().execute(fmt::format(
		"INSERT IGNORE INTO `bot_active_players` (`player_id`) VALUES ({})", guid));
}

void BotEngine::unregisterBot(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return;

	// BOT_LURE_KITE hygiene. Correctness does NOT depend on this — both features
	// re-validate their own gate every tick and self-clear (beginHuntPhase is
	// explicitly not a funnel). This just keeps stale entries from lingering.
	clearLureKiteStateFull(guid);

	// Fix #2: remove bot from all channel `users` maps BEFORE dropping the hibernation
	// pool ref. ChatChannel::users holds shared_ptr<Player>, so without this the Player
	// object lingers as a refcounted ghost in channel maps even after unregister. Most
	// visible on /cavebot reload (500 bots unregistered → 500 leaked Player refs in
	// channels 3 + 5 + cast + any others). Bounded but accumulates across reload cycles.
	if (auto player = it != guidToIndex_.end() ? bots_[it->second].playerRef.lock() : nullptr) {
		g_chat().removeUserFromAllChannels(player);
	} else if (auto pooled = hibernationPool_.find(guid); pooled != hibernationPool_.end() && pooled->second) {
		g_chat().removeUserFromAllChannels(pooled->second);
	}

	// Drop hibernation pool ref so the Player destructor fires when this bot is unregistered.
	hibernationPool_.erase(guid);

	size_t index = it->second;
	size_t lastIndex = bots_.size() - 1;
	if (index != lastIndex) {
		std::swap(bots_[index], bots_[lastIndex]);
		guidToIndex_[bots_[index].guid] = index;
	}
	bots_.pop_back();
	guidToIndex_.erase(it);

	// The mid-walk pause lambda keys off guidToIndex_, which we just dropped — it will
	// early-return without erasing walkPauseInfo_, so clean it up here to avoid a leak
	// (and a stale entry surviving guid reuse).
	walkPauseInfo_.erase(guid);
	// Same reasoning for the mount roll: unregisterBot is the only path that retires a guid
	// outright, so without this the map would outlive its bots and a reused guid would
	// inherit a stranger's mount intent (and its re-roll throttle).
	botMountWants_.erase(guid);

	// Mirror to bot_active_players (see registerBot). Async so /cavebot reload
	// unregistering 200 bots doesn't stall the dispatcher on sync DB writes.
	g_botDatabaseTasks().execute(fmt::format(
		"DELETE FROM `bot_active_players` WHERE `player_id` = {}", guid));
}

// Shared helper: load Player from DB by name and place in world. Used by both
// activateBot (true-offline reload path) and wakeBot (hibernation wake path).
// On success, bot.playerRef is updated and the new Player is returned.
std::shared_ptr<Player> BotEngine::materializePlayerFromDb(BotState &bot) {
	if (bot.name.empty()) return nullptr;

	auto newPlayer = std::make_shared<Player>(nullptr);
	// Same reason as Game.loadBotPlayer: flag before the load so the badge/title/cyclopedia
	// skip applies. This is the hibernation-pool-miss path, which is the HOT one -- it runs on
	// every wake that cannot be served from the pool.
	newPlayer->setBotPlayer(true);
	if (!IOLoginData::loadPlayerByName(newPlayer, bot.name, false)) {
		g_logger().warn("[BotEngine] materializePlayerFromDb: failed to reload '{}' from DB", bot.name);
		return nullptr;
	}
	newPlayer->setBotPlayer(true);
	newPlayer->setOnline(true);
	newPlayer->initBotBaseSpeed();
	newPlayer->setChaseMode(true);
	newPlayer->setSecureMode(true);

	static constexpr Position INACTIVE_POS { 31970, 32283, 7 };
	Position loginPos = newPlayer->getLoginPosition();
	bool hasRealLoginPos = loginPos.x > 0 && loginPos.y > 0 &&
	    !(loginPos.x == INACTIVE_POS.x && loginPos.y == INACTIVE_POS.y);
	bool placed = false;
	if (hasRealLoginPos) {
		placed = g_game().internalPlaceCreature(newPlayer, loginPos, false, true);
	}
	if (!placed) {
		if (!g_game().map.getTile(INACTIVE_POS))
			g_game().map.getOrCreateTile(INACTIVE_POS, true);
		placed = g_game().internalPlaceCreature(newPlayer, INACTIVE_POS, false, true);
	}
	if (!placed) {
		auto town = newPlayer->getTown();
		if (town) placed = g_game().internalPlaceCreature(newPlayer, town->getTemplePosition(), false, true);
	}
	if (!placed) {
		g_logger().warn("[BotEngine] materializePlayerFromDb: could not place '{}' in world", bot.name);
		return nullptr;
	}
	g_game().addCreatureCheck(newPlayer);
	newPlayer->registerCreatureEvent("BotDeath");
	bot.playerRef = newPlayer;
	return newPlayer;
}

bool BotEngine::activateBot(uint32_t guid) {
	// BOT_CSV: a failed authored-data load poisons the engine — no bot may activate on
	// empty or partial data. ERROR on the first refusal and every ~60s, so the journal
	// says WHY the world is empty; the server itself stays up and keeps serving logins
	// while an admin fixes the file and runs /cavebot reload (success clears the poison).
	// Guarding here covers the population scheduler, which funnels through activateBot.
	if (dataPoisoned_) {
		const int64_t now = OTSYS_TIME();
		if (lastPoisonLogMs_ == 0 || now - lastPoisonLogMs_ >= 60000) {
			lastPoisonLogMs_ = now;
			g_logger().error("[BOT_CSV] REFUSING bot {}: authored data failed to load ({})",
				"activation", dataPoisonReason_);
		}
		return false;
	}
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;

	auto& bot = bots_[it->second];
	if (bot.active) return false;

	// If bot was truly removed from world (true-offline deactivation), reload it from DB.
	// Also re-materialize if the Player still exists but is flagged removed — this catches
	// the death-loop bug (2026-05-22 Leander Frostborn / Keira Mistwalker investigation):
	// bot dies mid-hunt, Player::despawn() (or another path) sets removed=true but doesn't
	// clear bot.playerRef. Without this isRemoved() guard, activateBot reuses the dead
	// Player, processBot's isRemoved check fires next tick, bot deactivates, scheduler
	// re-activates with the same dead Player, loops at 1Hz until restart.
	auto player = bot.getPlayer();
	if (!player || player->isRemoved()) {
		player = materializePlayerFromDb(bot);
		if (!player) return false;
	}

	bot.active = true;
	bot.state = BotAIState::IDLE;
	bot.tickCounter = botInitialTickPhase(bot.guid);  // guid-phased (Phase 1) — not 0
	bot.cachedLevel = static_cast<uint32_t>(player->getLevel());  // refresh for v2 virtual reroll
	// Share of bots eligible for quest scripts. Deterministic per guid so a bot keeps its
	// role across reconnects. Was hardcoded (guid % 20 == 0); the %100 form is what makes an
	// arbitrary percentage expressible, and at the default 5 it selects a same-sized but
	// different subset — quest-bot identity is recomputed here and persisted nowhere.
	{
		const int32_t questPct = std::clamp(
			static_cast<int32_t>(g_configManager().getNumber(BOT_QUEST_BOT_PCT)), 0, 100);
		bot.isQuestBot = (static_cast<int32_t>(bot.guid % 100) < questPct);
	}

	// Set town from player's DB assignment
	auto town = player->getTown();
	if (town) {
		bot.townId = town->getID();
		auto nameIt = travelTownNames_.find(town->getID());
		bot.townName = (nameIt != travelTownNames_.end()) ? nameIt->second : town->getName();
	}

	// BOT_LIVENESS_PACK Phase C.1: per-bot mount roll (botMountChancePct, default 60),
	// drawing from the bot's owned mount set — the full 235-mount catalog seeded by
	// tools/bot_population_generator/generate.py. The roll and the retry that makes it
	// stick live in rollMountForReconnect (bot_liveness.cpp); this is a true-offline
	// reconnect, so deactivateBot has already dropped the previous roll and it re-rolls
	// unconditionally.
	rollMountForReconnect(bot, player);

	// Bot stays at its current/loaded position (loginPosition from DB).
	// No random teleport — bot continues from wherever it was.
	bot.currentPos = player->getPosition();
	bot.lastPos = bot.currentPos;

	// Correct townId if the DB position drifted away from the player's home town
	// (e.g. virtual sim moved the bot across cities while hibernated, server restart).
	syncTownIdToPos(bot);

	// Fresh safeTeleportLanding rewind allowance for this activation.
	resetTpRewindBudget(bot.guid);

	// Reset HP and mana
	player->health = player->healthMax;
	player->mana = player->getMaxMana();
	g_game().addCreatureHealth(player);
	g_game().addPlayerMana(player);

	// Equip gear for level and vocation
	equipBot(bot);

	// Clear all state (including depot reroll timer + walk target)
	s_depotLockerRerollTime.erase(bot.guid);
	s_depotDwellWalkTarget.erase(bot.guid);
	s_depotDwellWalkFails.erase(bot.guid);
	bot.deathPauseUntil = 0;
	bot.hasWalkTarget = false;
	bot.pendingNavDest.clear();
	bot.walkTarget = Position();
	bot.currentPOI = nullptr;
	bot.pathFailCount = 0;
	bot.consecutivePOIFails = 0;
	bot.visitedPOIs.clear();
	bot.dwellUntil = 0;
	bot.hasDepotTarget = false;
	bot.returningHome = false;
	bot.lastChatTick = 0;
	bot.lastLogTick = 0;

	// Clear combat state
	bot.attackerId = 0;
	bot.pkTarget = 0;
	bot.combatDecision.clear();
	bot.combatStartTime = 0;
	bot.lastCombatProgress = 0;
	bot.lastAttackTime = 0;
	bot.lastHealTime = 0;
	bot.pvpManaSpent = 0;
	bot.ignoredAttackerId = 0;
	bot.ignoredHitBack = false;
	bot.defenseScanCooldown = 0;
	bot.hasPCPos = false;
	bot.seenPKers.clear();

	// Clear hunt state
	bot.huntScriptId = 0;
	bot.huntKillCount = 0;
	bot.huntTargetId = 0;
	bot.huntCooldownUntil = 0;
	bot.huntIgnoredMonsters.clear();
	bot.huntDebugKillLimit = 0;

	// Clear travel state
	bot.travelDestTownId = 0;
	bot.travelPhase.clear();
	bot.pendingHuntAfterTravel = false;
	bot.travelDestVerified = false;
	bot.triedRouteSources.clear();
	bot.lastRouteDestination.clear();

	// Clear city route state
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;

	// Clear floor change state
	resetFloorChange(bot);

	// Start with logging off — auto-enabled when cast viewers join
	bot.verboseLog = false;
	bot.verboseLogManual = false;

	player->setFightMode(FIGHTMODE_ATTACK);

	// Enable cast broadcasting so @cast viewers can watch this bot. DB INSERT goes async
	// via DatabaseTasks so /cavebot reload (200 activates in succession) doesn't stall the
	// dispatcher on serial DB writes. In-game cast routing is in-memory above.
	player->setCastBroadcasting(true);
	auto& db = Database::getInstance();
	g_botDatabaseTasks().execute(fmt::format(
		"INSERT INTO `cast_broadcasters` (`player_id`, `player_name`) VALUES ({}, {}) "
		"ON DUPLICATE KEY UPDATE `player_name` = {}",
		guid, db.escapeString(player->getName()), db.escapeString(player->getName())));

	// Initialize reroll timer (short delay before first autonomous activity)
	bot.nextRerollTime = OTSYS_TIME() + uniform_random(5, 15) * 1000;
	bot.postActivationReroll = true;

	// Restore saved AI state if available (e.g. bot was deactivated mid-hunt by scheduler)
	auto stateResult = db.storeQuery(fmt::format(
		"SELECT `ai_state`, `hunt_script_id`, `hunt_phase`, `waypoint_idx`, `kill_count`, "
		"`travel_dest_town_id` FROM `bot_state_persistence` WHERE `guid`={}", guid));
	if (stateResult) {
		restoreSingleBotState(bot, stateResult);
		g_botDatabaseTasks().execute(fmt::format("DELETE FROM `bot_state_persistence` WHERE `guid`={}", guid));
		bot.activatedAt = 0; // already has a task — no 1-min fallback needed
		g_logger().info("[BotEngine] Activated bot '{}' (guid={}, town={}, pos=({},{},{}) — state restored)",
			player->getName(), guid, bot.townId, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
	} else {
		// No saved state — start 1-min fallback timer in case bot is in a bad position
		bot.activatedAt = OTSYS_TIME();
		g_logger().info("[BotEngine] Activated bot '{}' (guid={}, town={}, pos=({},{},{}))",
			player->getName(), guid, bot.townId, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
	}
	return true;
}

bool BotEngine::deactivateBot(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;

	auto& bot = bots_[it->second];
	if (!bot.active) return false;
	if (bot.state != BotAIState::IDLE && bot.state != BotAIState::DWELLING) return false;

	// BOT_PARTY_LEAK_FIX: last exit before the Player handle goes. deactivateBot had NO party
	// handling at all, so a bot carrying a stale Canary party (engine state already zero, so the
	// partyHuntId-keyed cleanups elsewhere skip it) took the party into oblivion with it — and if
	// it was the Canary leader, the members were left holding a party nothing can ever disband.
	reclaimStaleCanaryParty(guid, "deactivateBot");
	endIceFishSession(bot, "deactivated");
	clearLootState(bot.guid);

	// BOT_PARTY_INVITE_RENDEZVOUS §3.9b: same invitation hole as hibernateBot — reclaim handles
	// party MEMBERSHIP, not pending invitations, and the removeCreature below is also
	// isLogout=false. Without this the shared_ptr in the inviter's inviteList outlives
	// removePlayer entirely.
	if (auto invPlayer = bot.getPlayer()) {
		invPlayer->clearPartyInvitations();
	}
	if (s_pendingInvites.erase(guid) > 0) {
		s_prv.staleCleared++;
		g_logger().info("[BotEngine] [PINVITE] stale-cleared: bot='{}' site=deactivateBot", bot.name);
	}
	s_inviteDebugKeepAlive.erase(guid);

	auto& db = Database::getInstance();

	auto player = bot.getPlayer();
	if (player) {
		// Save current position to players table before removing from world
		auto pos = player->getPosition();
		g_botDatabaseTasks().execute(fmt::format(
			"UPDATE `players` SET `posx`={}, `posy`={}, `posz`={} WHERE `id`={}",
			pos.x, pos.y, pos.z, guid));

		// True offline: remove bot from game world (like a player disconnect)
		player->setCastBroadcasting(false);
		// JITTER FIX: async DELETE — sync executeQuery on dispatcher caused multi-second
		// stalls when MySQL was slow (Sonnet-diagnosed Finding 1 root cause). The
		// in-memory setCastBroadcasting(false) already disconnects viewers; DB cleanup
		// is non-critical and can be async.
		g_botDatabaseTasks().execute(fmt::format("DELETE FROM `cast_broadcasters` WHERE `player_id` = {}", guid));
		g_game().removeCreature(player, false);
		g_game().removePlayer(player);
		player->setOnline(false);
		bot.playerRef = std::weak_ptr<Player>(); // release reference
	}

	// Cleanup stuck-loop counters
	s_fcConsecutiveFailures.erase(guid);
	s_lastRouteSource.erase(guid);
	s_lastHeartbeat.erase(guid);
	s_depotLockerRerollTime.erase(guid);
	s_depotDwellWalkTarget.erase(guid);
	s_depotDwellWalkFails.erase(guid);
	s_lastFcPositions.erase(guid);
	s_leavingPhaseStart.erase(guid);
	s_leavingWpTimer.erase(guid);
	s_huntTravelStart.erase(guid);
	s_walkTargetTimer.erase(guid);
	s_roam.erase(guid);        // BOT_AMBIENT_ROAM: session + its reserve slot
	s_roamLedger.erase(guid);
	clearPlannerWalk(guid);
	clearFishingRun(guid);
	endHouseVisit(guid, "unregister");
	endShrineVisit(guid, "unregister");
	clearShrineBlacklist(guid);
	s_travelFcRecoveryCount.erase(guid);
	s_travelStartTime.erase(guid);
	s_travelDestPOI.erase(guid);
	s_travelArriveTarget.erase(guid);
	s_travelSrcPOI.erase(guid);
	s_lastRouteEndPos.erase(guid);
	walkPauseInfo_.erase(guid);
	// Dropping the mount roll here is what makes a true-offline deactivate→activate cycle a
	// genuine reconnect: activateBot then finds no entry and re-rolls unthrottled.
	botMountWants_.erase(guid);
	clearDepotBlacklist(guid);
	s_tpRewindBudget.erase(guid);

	bot.active = false;
	bot.state = BotAIState::INACTIVE;
	bot.activatedAt = 0;
	g_logger().info("[BotEngine] Deactivated bot '{}' — removed from world", bot.name);
	return true;
}

// ============================================================================
// Hibernation: full despawn while preserving AI state
// ============================================================================
//
// Differs from deactivateBot in two key ways:
//   1. BotState is NOT cleared — huntScriptId, huntPhase, partyHuntId, fcState, etc.
//      survive across hibernate/wake so the AI resumes seamlessly.
//   2. Static maps (s_lastHeartbeat, s_*RouteState, etc.) are NOT erased — they're
//      part of the AI's working memory.
//
// Wake re-creates the Player from DB via materializePlayerFromDb() WITHOUT calling
// activateBot (which would reset hunt state via lines ~1944-1998 in this file).

bool BotEngine::hibernateBot(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;

	auto &bot = bots_[it->second];
	if (bot.hibernated) return false;
	endIceFishSession(bot, "hibernated");
	clearLootState(bot.guid);
	// BOT_LURE_KITE: a hibernated bot has no Player and its virtual twin models neither
	// behaviour, so both must be dropped here — waking mid-lure with a stale pack timer
	// (or mid-kite with a cursor into a script the virtual sim has since rerolled) is
	// exactly the staleness the per-tick gate stamps guard against.
	clearLureKiteState(bot.guid);
	// BOT_AMBIENT_ROAM. The session erase is load-bearing, not tidiness: doActivityReroll also
	// runs for HIBERNATED bots via virtualAdvanceIdle, so a stale entry would keep suppressing
	// this bot's rerolls and freeze its virtual life permanently. The ledger goes too — a
	// hibernated bot occupies no density, so it owes the roam reserve nothing.
	s_roam.erase(guid);
	s_roamLedger.erase(guid);

	// JITTER DIAGNOSTIC: RAII timer covers all return paths. Threshold 5ms.
	struct HibTimer {
		int64_t start;
		uint32_t guid;
		std::string name;
		~HibTimer() {
			int64_t dt = botMonoMs() - start;
			if (dt > 5) {
				g_logger().warn("[HIB_SLOW] guid={} name={} duration={}ms",
					guid, name, dt);
			}
		}
	};
	HibTimer jitter_h{botMonoMs(), guid, bot.name};
	// Race guard: don't hibernate during death Phase 1 (dyingBots_ holds a strong ref;
	// removeCreature on a despawned creature would corrupt the death state machine).
	if (dyingBots_.count(guid) > 0) return false;

	auto player = bot.getPlayer();

	// Cast viewer guard: never hibernate while viewers are watching. Cascade with party
	// leader+viewer keeps the whole team awake (the cascade runs AFTER this guard so a
	// blocked leader hibernation never triggers member hibernation either).
	if (player && player->getCastViewerCount() > 0) {
		// Rate-limit log: once per minute per bot. Lua proximity loop fires at 300ms
		// cadence, so without throttling this spams ~200 lines/min for a single watched bot.
		static std::unordered_map<uint32_t, int64_t> s_lastViewerSkipLog;
		int64_t now = OTSYS_TIME();
		auto &lastLog = s_lastViewerSkipLog[guid];
		if (now - lastLog > 60000) {
			g_logger().info("[BotEngine] hibernateBot: skipping '{}' — has {} cast viewer(s)",
				bot.name, player->getCastViewerCount());
			lastLog = now;
		}
		return false;
	}

	// BOT_PARTY_INVITE_RENDEZVOUS trap #10 — a HUMAN-led party member must not hibernate.
	// This guard hole is PRE-EXISTING, not opened by this feature: bot_hibernation.lua's
	// awake-bot branch is purely distance-based (it never reads bot.state), and the suppress
	// below keys on partyHuntId, which human-led members never set. Today's /party members
	// escape only by accident — they stay glued within trailCfg_.followDist of the human, so
	// the Lua loop's nearPlayer test never starts their hysteresis. Walk-in ARMS the hole: an
	// assembling member is deliberately far from the leader for tens of seconds. Hibernating
	// one would strand its Canary membership, because removeCreature(isLogout=false) skips the
	// leaveParty branch — the human would keep a member that is no longer in the world.
	if (s_rvMember.count(guid) > 0
	    || (s_partyLeaderId.count(guid) > 0 && s_botToPartyHunt.count(guid) == 0)) {
		static std::unordered_map<uint32_t, int64_t> s_lastHumanPartySkipLog;
		int64_t nowMs = OTSYS_TIME();
		auto& lastLog = s_lastHumanPartySkipLog[guid];
		if (nowMs - lastLog > 60000) {
			lastLog = nowMs;
			g_logger().info("[BotEngine] hibernateBot: skipping '{}' — human-led party member (assembling={})",
				bot.name, s_rvMember.count(guid) > 0);
		}
		return false;
	}

	// BOT_PARTY_INVITE_RENDEZVOUS §3.7 — reclaim-to-inactive routing. This bot was NEVER logged
	// in before a party conscripted it (findBotsForParty ranks !active candidates, and a party
	// promotes them to active=true). Hibernating it would keep active=true forever and ratchet
	// the logged-in population upward with every party, so send it all the way back out instead.
	//
	// deactivateBot refuses anything that is not IDLE/DWELLING, and an ex-member that rerolled
	// its next task from where it stood is routinely HUNTING or TRAVELING by the time the
	// ordinary reclaim rules reach it — that is the COMMON path, not an edge. So force the
	// preconditions first. releasePartyMemberActivity is what makes this safe: it hands back
	// huntScriptId together with its activeHunts_/activeSpawnGroups_ entries, or we would have
	// traded a population leak for a hunt-reservation leak.
	if (s_reclaimToInactive.count(guid) > 0) {
		releasePartyMemberActivity(bot, "reclaim_to_inactive");
		bot.state = BotAIState::IDLE;
		if (deactivateBot(guid)) {
			s_reclaimToInactive.erase(guid);
			g_logger().info("[BotEngine] reclaim-to-inactive: '{}' deactivated (was never logged in before its party)",
				bot.name);
			return true;
		}
		// Never leave the bot in limbo — a refusal here degrades to ordinary hibernation and
		// keeps the set entry. A nonzero rate on this line means the forced preconditions
		// missed a field deactivateBot checks; it is an implementation bug, not noise.
		g_logger().warn("[BotEngine] reclaim-to-inactive refused for '{}' — falling back to hibernate", bot.name);
	}

	// PARTY_HUNT lifecycle coupling (2026-06-14): a support (non-leader) party member must
	// NEVER hibernate on its own — only via the leader's cascade below. Otherwise a support
	// that drifts >100 tiles from the EK hibernates independently (bot_hibernation.lua is not
	// party-aware), then the density wake-cap blocks its proximity re-wake → the ~30s
	// wake/hibernate oscillation that left cast-watched party supports inert. The cascade
	// (set via s_inPartyHibernateCascade RAII) bypasses this so the recursive member-hibernate
	// calls below still work. Dissolve paths clear partyHuntId first, so ex-members hibernate
	// normally. Mirror of the wake-side s_inPartyCascade exemption in shouldGateWake.
	if (!s_inPartyHibernateCascade && !bot.isPartyHuntLeader && bot.partyHuntId > 0) {
		// Closed-loop telemetry: this only fires when a live support drifted >100 tiles from
		// every anchor (bot_hibernation.lua wanted to hibernate it). After Fix C (no AdvStone
		// overlay starving doParty) a healthy support follows the EK and stays in range, so
		// this should be ~0. A steady stream = supports still NOT following (drift) → the EK's
		// distance below tells how far. Rate-limited 60s/guid so a stuck member can't spam.
		static std::unordered_map<uint32_t, int64_t> s_lastPartyHibSuppressLog;
		int64_t nowMs = OTSYS_TIME();
		auto& lastLog = s_lastPartyHibSuppressLog[guid];
		if (nowMs - lastLog > 60000) {
			lastLog = nowMs;
			int32_t leaderDist = -1;
			auto lgIt = s_partyHuntLeaderGuid.find(bot.partyHuntId);
			if (lgIt != s_partyHuntLeaderGuid.end()) {
				auto lbIt = guidToIndex_.find(lgIt->second);
				if (lbIt != guidToIndex_.end()) {
					const auto& lb = bots_[lbIt->second];
					leaderDist = std::max(
						std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(lb.currentPos.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(lb.currentPos.y)));
				}
			}
			g_logger().info("[BotEngine] PARTY_HUNT cohesion: suppressed independent hibernate of support '{}' (party #{}, distToLeader={}) — drift if recurring",
				bot.name, bot.partyHuntId, leaderDist);
		}
		return false;
	}

	// Cascade: party hunt leader → hibernate all members atomically. Snapshot members
	// first since recursive call may mutate maps. NOTE (hibernateAllEligibleBots edge case):
	// if a member is iterated before its leader there, the block above skips it; the leader's
	// cascade then catches it. A member whose leader is somehow NOT in the active set would be
	// left awake — pre-existing, orthogonal to this fix (party assembly keeps leader+members
	// in the same active state; dissolve clears partyHuntId on all members).
	if (player && bot.isPartyHuntLeader && bot.partyHuntId > 0) {
		// BOT_PARTY_INVITE_RENDEZVOUS trap #7: if this leader is mid-assembly, finalize as virtual —
		// drop the record and the members' s_rvMember entries BEFORE the cascade runs. Order is
		// load-bearing: the human-led/assembling guard earlier in this function does NOT check
		// s_inPartyHibernateCascade, so with the record still intact the cascade's recursive
		// member-hibernates would be REFUSED, stranding members walking toward a leader that has
		// left the world.
		for (auto aIt = s_partyAssembly.begin(); aIt != s_partyAssembly.end();) {
			if (aIt->second.kind == RvKind::BOT_LED_HUNT && aIt->second.partyHuntId == bot.partyHuntId) {
				for (const auto& m : aIt->second.members) s_rvMember.erase(m.guid);
				g_logger().info("[BotEngine] [PARTYRV] assembly #{} kind=bot finalized-as-virtual "
					"(leader '{}' hibernating mid-assembly)", aIt->first, bot.name);
				s_prv.asmFinalizedVirtual++;
				aIt = s_partyAssembly.erase(aIt);
			} else {
				++aIt;
			}
		}

		// BOT_PARTY_LEAK_FIX: tear the Canary party down HERE, while the leader Player is still
		// reachable and before the members hibernate. Wake rebuilds it via materializeCanaryParty,
		// so the invariant dissolveVirtualPartyHunt relies on (a virtual party owns no Canary
		// Party) holds again.
		dematerializeCanaryParty(bot.partyHuntId, "leader_hibernate");
		std::vector<uint32_t> members;
		auto memberIt = s_partyHuntMembers.find(bot.partyHuntId);
		if (memberIt != s_partyHuntMembers.end()) {
			members = memberIt->second;
		}
		// RAII: bypass the member-hibernate block for the duration of the cascade only, so the
		// recursive hibernateBot(memberGuid) calls actually hibernate the members. Unwinds the
		// flag on any return/throw (same pattern as the wake-side CascadeGuard).
		struct HibernateCascadeGuard {
			HibernateCascadeGuard() { s_inPartyHibernateCascade = true; }
			~HibernateCascadeGuard() { s_inPartyHibernateCascade = false; }
		} _hibCascadeGuard;
		for (uint32_t memberGuid : members) {
			if (memberGuid != guid) hibernateBot(memberGuid);
		}
	}

	if (player) {
		// Reset stuck floor-change state (resuming mid-step on wake would crash).
		if (bot.fcState == FloorChangeState::STEPPING_ON) {
			bot.fcState = FloorChangeState::SCANNING;
		}

		// Clear stale walk + follow + attack state before pool insert. The Player object
		// survives hibernation in the pool — without this cleanup, the woken bot would
		// resume mid-walk from a NEW position (virtualPos differs from pre-hibernate pos),
		// emitting inconsistent MapWalk packets that desync nearby spectator clients.
		// Same established pattern as d32629871 / f71539ad7 / 8ce6cd0fb teleport cleanups.
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
		player->setFollowCreature(nullptr);
		player->setAttackedCreature(nullptr);
		bot.hasWalkTarget = false;
		bot.pendingNavDest.clear();
		// A hibernating bot abandons whatever it was walking to, so it must also hand back any
		// NPC approach-tile claim — otherwise that counter spot stays blocked for the full
		// reservation TTL for a visit that will never happen.
		clearPlannerWalk(bot.guid);
		// Same for a fishing trip: the virtual simulator has no fishing phase, and its DWELLING
		// handler is a pure dwellUntil timer, so a hibernated bot carrying a session-length
		// dwellUntil would sit idle for minutes. clearFishingRun puts that back to normal.
		clearFishingRun(bot.guid);
		endHouseVisit(bot.guid, "hibernate");
		endShrineVisit(bot.guid, "hibernate");
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		bot.hasFleeTarget = false;
		bot.fleeDirectional = false;

		// Adv Stone exercise training: stop the Lua exerciseTrainingEvent loop and clear
		// the in-state flag so the live AI re-fires useItemEx on the next wake. The Lua
		// loop self-terminates anyway when the Player leaves g_game, but doing it
		// explicitly here ensures setTraining(false) reaches the loop while the Player
		// is still in-world. No-op fast path when training isn't active.
		stopAdvStoneTrainingIfActive(bot);

		// Cast viewers share the Player pointer via weak_ptr; disconnect them BEFORE
		// destroying the Player to avoid null-deref on next packet (see player.hpp:1683).
		player->disconnectAllCastViewers();
		player->setCastBroadcasting(false);
		// JITTER FIX: async DELETE — sync executeQuery on dispatcher caused multi-second
		// stalls when MySQL was slow. In-memory disconnect already complete; DB cleanup async.
		g_botDatabaseTasks().execute(fmt::format("DELETE FROM `cast_broadcasters` WHERE `player_id`={}", guid));

		// CRITICAL: set loginPosition before removeCreature(false). The isLogout=false
		// branch at player.cpp:11487 skips the loginPosition update, and
		// iologindata_save_player.cpp:218 saves loginPosition into the DB position
		// columns — so wake position would be stale otherwise (only matters if the
		// pool is lost across a server restart and we fall back to DB load).
		auto pos = player->getPosition();
		player->loginPosition = pos;

		// BOT_PARTY_INVITE_RENDEZVOUS §3.9a: drop any pending party invitation BEFORE leaving
		// the world. removeCreature(player, false) passes isLogout=false, which skips the
		// Player::onRemoveCreature branch that would otherwise run clearPartyInvitations() —
		// so the inviting party keeps an inviteList entry holding a shared_ptr to a bot that
		// is no longer in the world. Party::empty() counts invites, so such a party can also
		// never disband. clearPartyInvitations() is public and calls removeInvite on each
		// party, which disbands one left empty.
		// The CLEANUP is unconditional (clearPartyInvitations walks a normally-empty vector, so
		// this is free). Only the LOG is gated, on the one signal we own cheaply: invitePartyList
		// has no public size accessor and we deliberately do not add one for a log line, so an
		// invite that exists outside our tracking (e.g. one that landed while
		// botPartyInviteEnable was false) is still cleaned — just not counted.
		player->clearPartyInvitations();
		if (s_pendingInvites.erase(guid) > 0) {
			s_prv.staleCleared++;
			g_logger().info("[BotEngine] [PINVITE] stale-cleared: bot='{}' site=hibernateBot", bot.name);
		}

		// Capture STRONG ref into the hibernation pool BEFORE removeCreature/removePlayer
		// release the maps' refs. This keeps the Player object alive (with all inventory,
		// stats, conditions, etc.) so wakeBot can re-link without a DB load. Pre-v5 we
		// destroyed the Player and re-loaded from DB on wake — that synchronous I/O on
		// the game thread was the root cause of map-tile lag during fast traversal.
		hibernationPool_[guid] = player;

		// removeCreature with isLogout=false skips Lua onLogout events but still triggers
		// onRemoveCreature → g_saveManager().savePlayer() (player.cpp:11504). Single save.
		g_game().removeCreature(player, false);
		g_game().removePlayer(player);
		player->setOnline(false);
		bot.playerRef = std::weak_ptr<Player>();  // weak_ptr stays valid via pool's strong ref

		g_logger().info("[BotEngine] Hibernated bot '{}' at ({},{},{}) (pool size={})",
			bot.name, pos.x, pos.y, pos.z, hibernationPool_.size());
	} else {
		g_logger().info("[BotEngine] Hibernated bot '{}' (no player ref)", bot.name);
	}

	bot.hibernated = true;
	// VT_LAG false-positive fix (2026-06-08): reset the per-bot virtualTick timestamp on
	// awake→hibernated transition. Without this, the next virtualTick visit would
	// compute per_bot_elapsed = (now - last-hibernated-visit), which spans the entire
	// awake period — firing a spurious VT_LAG even though the bot was alive being played.
	lastVirtualAdvanceMs_[guid] = OTSYS_TIME();
	// Keep bot.active = true so the population scheduler treats hibernated bots as
	// "occupied" (already in the population) and doesn't pick them as activation
	// candidates. The tick loop has an explicit `if (bot.hibernated) continue;`
	// gate to skip them, and processBot's null-player check is bypassed by that gate.
	return true;
}

bool BotEngine::wakeBot(uint32_t guid) {
	// BOT_CSV: same refusal as activateBot. Wake is the hibernation path's own entry and
	// does not necessarily pass through activateBot, so both must be guarded or the
	// population/wake route would still materialize bots onto poisoned data.
	if (dataPoisoned_) {
		const int64_t now = OTSYS_TIME();
		if (lastPoisonLogMs_ == 0 || now - lastPoisonLogMs_ >= 60000) {
			lastPoisonLogMs_ = now;
			g_logger().error("[BOT_CSV] REFUSING bot {}: authored data failed to load ({})",
				"wake", dataPoisonReason_);
		}
		return false;
	}
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;

	auto &bot = bots_[it->second];
	if (!bot.hibernated) return false;

	// BOT_LIVENESS (2026-06-13): consume the wake-path flag (read + reset to the default
	// true) at the top so nested cascade wakes don't inherit a stale value by accident —
	// the cascade loop re-asserts `proximityWake` explicitly before each member wake.
	const bool proximityWake = s_proximityWake;
	s_proximityWake = true;

	// Liveness diagnostics (2026-06-09): count this as a "tried" wake. Pre-gate so
	// gated/granted/silently-failed all sum from this baseline. tried - gated - granted
	// in the [PROXIMITY] periodic log = silent failures (chooseWakePosition / placement).
	wakeTried60s_++;

	// PERF_INVESTIGATION_2026-05-24 Phase B (2026-06-01): density-capped wake gate.
	// shouldGateWake refreshes the anchor cluster cache if stale (≥100ms), updates
	// bot.lastWakeAttemptMs unconditionally for LRU fairness, and returns true when
	// a ring cap blocks this wake. Party cascade wakes bypass the cap via the
	// s_inPartyCascade thread_local (set by the cascade loop below). The cap reserves
	// the counter slot inline on grant, so back-to-back wakes in the same dispatcher
	// burst see the updated counts.
	refreshAnchorsIfStale(100);
	if (shouldGateWake(guid)) {
		wakeGated60s_++;
		return false;
	}

	// JITTER DIAGNOSTIC: RAII timer covers all return paths in the heavy body below.
	// Threshold-gated at 5ms — single pool-hit should be sub-ms typically.
	struct WakeTimer {
		int64_t start;
		uint32_t guid;
		std::string name;
		const char* cause;  // mutated to "db_load" inside slow path
		~WakeTimer() {
			int64_t dt = botMonoMs() - start;
			if (dt > 5) {
				g_logger().warn("[WAKE_SLOW] guid={} name={} duration={}ms cause={}",
					guid, name, dt, cause);
			}
			s_wakeBurstAccumMs += dt;
			s_wakeBurstCount++;
			if (dt > s_wakeBurstMaxSingleMs) s_wakeBurstMaxSingleMs = dt;
		}
	};
	WakeTimer jitter_t{botMonoMs(), guid, bot.name, "pool_hit"};

	// Capture virtual position BEFORE materialize — the v2 simulator may have advanced
	// bot.currentPos far beyond what's in the DB (5-min position-save throttle), e.g.
	// AdvStone trip dwelling in dungeon while DB still has the Edron boat position.
	Position virtualPos = bot.currentPos;

	std::shared_ptr<Player> player;

	// Fast path: pull Player out of the hibernation pool (zero DB I/O). The Player object
	// is the SAME one that was hibernated — all inventory/stats/conditions preserved.
	auto poolIt = hibernationPool_.find(guid);
	if (poolIt != hibernationPool_.end()) {
		player = poolIt->second;
		hibernationPool_.erase(poolIt);

		// CRITICAL: clear the isRemoved flag set by removeCreature during hibernate.
		// Creature::setRemoved() is one-way — without setNotRemoved() the woken bot
		// would be invisible to monster targeting (Spectators::find() and isTarget()
		// check isRemoved) AND filtered out of the cast viewer list (protocollogin.cpp:55
		// filters `!p->isRemoved()`). User-reported symptoms: monsters not aggroing
		// hunting bots; cast list "going on and off".
		player->setNotRemoved();

		// CRITICAL: clear the stale tile parent left over from before hibernate.
		// Game::removeCreature does NOT clear m_tile — it removes the creature from
		// the tile but the Player keeps the weak_ptr<Tile> reference. setParent with
		// an empty weak_ptr is a no-op (creature.cpp:1593 has no else-branch).
		// internalPlaceCreature line 1162 rejects creatures with non-null getParent(),
		// so without this, every pool-hit wake fell through to the slow DB-load path.
		player->clearTileParent();

		// Re-link to g_game() so monsters, spectator scans, getPlayerByName, etc. find
		// this Player again.
		g_game().addPlayer(player);
		player->setOnline(true);

		// TWO-STEP WAKE — same hook the boat NPCs and floor changes use.
		//
		// Step 1: silently place the Player at the staging tile (bot_manager.lua:22).
		// internalPlaceCreature emits NO spectator packets — that's fine here because
		// the staging tile has no real-player spectators. The Player just needs to
		// exist in the world so Step 2 can act on it.
		//
		// Step 2: internalTeleport from staging to the actual wake position. This is
		// the SAME path that Game::internalTeleport (game.cpp:2921) → Map::moveCreature
		// uses for boat NPC "yes" travel, floor-change tiles (ladders/holes/ropes),
		// and teleport tiles. It emits proper sendCreatureMove(teleport=true) to
		// spectators of the destination tile, which translates client-side to
		// sendRemoveTileThing(stagingPos) + sendAddCreature(virtualPos). The user's
		// client receives the bot through the same well-tested code path that's been
		// shipping for years for legitimate teleport events.
		//
		// Why this is safer than the previous spectator-loop approach: the loop tried
		// to mirror Game::placeCreature minus onPlacedCreature, but it iterated the
		// just-placed creature itself in the spectator set. spec->onCreatureAppear
		// with isLogin=true + spec==self routes to Player::onCreatureAppear:11433
		// which dereferences client->oldProtocol — bot Players have null client →
		// SIGSEGV. The teleport path avoids this branch entirely because it dispatches
		// to sendCreatureMove (which has a `if (client)` guard) instead of
		// onCreatureAppear.
		const Position stagingPos(31970, 32283, 7); // matches bot_manager.lua staging tile
		// chooseWakePosition validates virtualPos against unsafe-tile mask (walls, FC,
		// teleport, depot box, magic field, etc.). If unsafe, walks back through the
		// bot's route chain to find a safe prior waypoint. Falls back to town temple.
		// Prevents the "stuck inside wall" bug that occurred when virtualSim snapped
		// currentPos to a non-walkable POI tile and wake teleported the bot back to it.
		Position placeAt = chooseWakePosition(bot, virtualPos.x > 0 ? virtualPos : player->getLoginPosition(), proximityWake);

		// AdvStone bridge: virtual sim starts trips without setting the AdventurersGuild
		// storage (no Player object during virtualTryStartAdvStone). When the bot later
		// wakes and walks onto the aid:4253 forcefield, the MoveEvent reads storage=-1
		// and falls back to `player:getTown():getTemplePosition()` — for some bot towns
		// this returns an invalid position and the teleport silently fails, stranding
		// the bot ON the forcefield. Mirror the live startAdventurerStoneTrip path here:
		// stamp the start-town id onto the Player BEFORE the wake-teleport so the
		// MoveEvent always has a valid Town(townId).
		if (bot.advStoneActive && bot.advStoneStartTownId > 0) {
			// 52130 = Storage.Quest.U9_80.AdventurersGuild.Stone (matches the constexpr
			// declared later in this file; use literal here to avoid hoisting).
			player->addStorageValue(52130, static_cast<int32_t>(bot.advStoneStartTownId));
		}

		// Shrine bridge — same reasoning as the AdvStone bridge directly above, for the four
		// elemental shrine hubs. A bot that hibernated mid-trip and wakes at a hub never walked
		// an entrance tile, so Storage.ShrineEntrance is unset and shrine_exit.lua's fallback
		// silently fails for a bot (verified live: it stands on the flame indefinitely). Stamp
		// BEFORE the wake-teleport so the MoveEvent has valid state the moment the bot can step
		// on a flame.
		if (botNearShrineHub(placeAt)) {
			botStampShrineReturn(player, "wake");
		}

		bool placed = g_game().internalPlaceCreature(player, stagingPos, false, true);
		if (placed) {
			// Random N/S/E/W orientation so bots don't all face south by default. Set
			// BEFORE the internalTeleport below so the sendCreatureMove(teleport=true)
			// packet bakes in the new direction — spectators see the bot already facing
			// correctly on first render instead of a default-south sprite that pops to a
			// random direction one tick later. Cardinals only (no diagonals — diagonal
			// idle sprites look odd in Tibia).
			static const Direction kWakeDirs[4] = {
				DIRECTION_NORTH, DIRECTION_SOUTH, DIRECTION_EAST, DIRECTION_WEST
			};
			player->setDirection(kWakeDirs[uniform_random(0, 3)]);

			// Post-condition repair. chooseWakePosition already vets its result, but this
			// value also arrives from callers that computed it another way, and internalTeleport
			// skips queryDestination — so an FC tile here would leave the bot standing on
			// stairs. Cheap: isUnsafeWakeTile short-circuits on the common safe case.
			if (isUnsafeWakeTile(bot, placeAt)) {
				Position repaired = safePlacementTail(bot, placeAt, burstReservedTiles_,
					/*allowWideRings=*/true, /*allowTempleFallback=*/true, "wakeBot");
				if (repaired.x > 0) {
					s_tpSafeWakeRepairs++;
					placeAt = repaired;
				}
			}

			// Teleport to the destination via the boat/floor-change hook.
			ReturnValue tpRet = BOT_TELEPORT(player, placeAt, /*pushMove=*/false);
			if (tpRet != RETURNVALUE_NOERROR) {
				// Destination unwalkable — try loginPosition, then town temple.
				tpRet = BOT_TELEPORT(player, player->getLoginPosition(), false);
				if (tpRet != RETURNVALUE_NOERROR) {
					auto town = player->getTown();
					if (town) {
						tpRet = BOT_TELEPORT(player, town->getTemplePosition(), false);
					}
				}
				if (tpRet != RETURNVALUE_NOERROR) {
					// All teleport attempts failed — bot stranded at staging. Next hibernate
					// cycle will collect it; players never see the staging tile. Log so
					// repeated strands are visible in journalctl.
					g_logger().warn("[BotEngine] wakeBot: '{}' stranded at staging after teleport failures (destPos={},{},{} ret={})",
						bot.name, placeAt.x, placeAt.y, placeAt.z, static_cast<int>(tpRet));
				}
			}
		}
		if (!placed) {
			g_logger().warn("[BotEngine] wakeBot: pool-hit but could not place '{}' in world; falling back to materialize", bot.name);
			// Restore the pool entry so the materialize fallback below has a chance to clean up
			hibernationPool_[guid] = player;
			player.reset();
		} else {
			g_game().addCreatureCheck(player);
			player->registerCreatureEvent("BotDeath");
			bot.playerRef = player;

			// Defensive walk-state cleanup matching hibernateBot's pool-insert path.
			// hibernateBot already clears this state on the way IN, but doing it here too
			// is a safety net — guards against future code paths that bypass the hibernate
			// cleanup (admin direct-pool-insert, race, etc). Same pattern as f71539ad7.
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}
			player->setFollowCreature(nullptr);
			player->setAttackedCreature(nullptr);
			bot.hasWalkTarget = false;
			bot.pendingNavDest.clear();
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			bot.hasFleeTarget = false;
			bot.fleeDirectional = false;

			g_logger().info("[BotEngine] wakeBot: pool-hit '{}' at ({},{},{}) — no DB load (pool size={})",
				bot.name, placeAt.x, placeAt.y, placeAt.z, hibernationPool_.size());
		}
	}

	// Slow path: pool empty (e.g. server restart cleared the pool, or admin hibernate
	// before the pool existed). Materialize from DB as before.
	if (!player) {
		jitter_t.cause = "db_load";
		player = materializePlayerFromDb(bot);
		if (!player) {
			g_logger().warn("[BotEngine] wakeBot: failed to materialize '{}'; staying hibernated", bot.name);
			return false;
		}
		// AdvStone bridge (same as pool-hit path above): bridge the missing storage so
		// the forcefield MoveEvent teleports to a valid town.
		if (bot.advStoneActive && bot.advStoneStartTownId > 0) {
			// 52130 = Storage.Quest.U9_80.AdventurersGuild.Stone (matches the constexpr
			// declared later in this file; use literal here to avoid hoisting).
			player->addStorageValue(52130, static_cast<int32_t>(bot.advStoneStartTownId));
		}
		// Materialize places at loginPosition; resync to a safe wake position if it differs.
		// chooseWakePosition validates the destination and walks back through the route
		// chain if virtualPos lands on a non-walkable POI tile.
		Position safePos = chooseWakePosition(bot, virtualPos.x > 0 ? virtualPos : player->getPosition(), proximityWake);
		// Shrine bridge (same as the pool-hit path above). Placed AFTER safePos because that —
		// not virtualPos — is where the bot will physically stand.
		if (botNearShrineHub(safePos)) {
			botStampShrineReturn(player, "wake-db");
		}
		// Random N/S/E/W orientation BEFORE the resync teleport so the move packet bakes
		// the direction. Same rationale as the pool-hit path above.
		static const Direction kWakeDirs[4] = {
			DIRECTION_NORTH, DIRECTION_SOUTH, DIRECTION_EAST, DIRECTION_WEST
		};
		player->setDirection(kWakeDirs[uniform_random(0, 3)]);
		if (player->getPosition() != safePos && safePos.x > 0) {
			g_logger().info("[BotEngine] wakeBot: syncing '{}' from DB pos ({},{},{}) to safe pos ({},{},{})",
				bot.name,
				player->getPosition().x, player->getPosition().y, player->getPosition().z,
				safePos.x, safePos.y, safePos.z);
			BOT_TELEPORT(player, safePos, true);
		}

		// Future-proof: materializePlayerFromDb creates a fresh Player so listWalkDir is
		// naturally empty, but apply the same cleanup defensively in case that function
		// or internalTeleport ever leaves residual state. Same pattern as f71539ad7.
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
		player->setFollowCreature(nullptr);
		player->setAttackedCreature(nullptr);
		bot.hasWalkTarget = false;
		bot.pendingNavDest.clear();
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		bot.hasFleeTarget = false;
		bot.fleeDirectional = false;
	}

	// Resume floor-change from a clean state if it was stuck.
	if (bot.fcState == FloorChangeState::STEPPING_ON) {
		bot.fcState = FloorChangeState::SCANNING;
	}

	// Travel-state wake fix: the static maps s_travelStartTime / s_routeProgress are
	// process-scoped and survive hibernation. If a bot was hibernated mid-travel for
	// longer than TRAVEL_TIMEOUT_MS (5 min), the first doTraveling tick after wake
	// would see a pre-expired timer and fire the global timeout immediately —
	// teleporting the bot to temple + IDLE without giving it a chance to resume travel.
	// Reset both timers so the woken bot gets a fresh 5-min window from THIS tick.
	if (bot.state == BotAIState::TRAVELING) {
		s_travelStartTime[bot.guid] = OTSYS_TIME();
		s_routeProgress.erase(bot.guid);
	}

	// Parallel fix for HUNTING TRAVEL_TO phase: s_huntTravelStart is a process-scoped static
	// map that survives hibernation. When virtual sim runs for >HUNT_TRAVEL_MAX_MS (5 min) and
	// the bot then wakes, the live AI's TRAVEL_TO handler reads the stale timer, sees it as
	// pre-expired, and fires the global timeout immediately — teleporting bot to spawn-or-temple
	// and stalling mid-route until re-hibernation. Empirically reproduced 2026-05-16 with
	// Ophelia Frostborn on City Walk Venore→Thais (id=1308), 10 min hibernation → 28s stuck at
	// wake position (32512,32092,7) until re-hibernation at same tile. Same signature as Isolde
	// Stoneguard's 95s-stuck case. Reset the timer to give the woken bot a fresh 5-min window.
	if (bot.state == BotAIState::HUNTING && bot.huntPhase == HuntPhase::TRAVEL_TO) {
		s_huntTravelStart[bot.guid] = OTSYS_TIME();
	}

	bot.hibernated = false;
	bot.active = true;

	// BOT_LIVENESS_PACK: refresh the idle litter-drop budget so each awake session gets
	// one fresh drop opportunity (the once-per-wake cap resets on un-hibernate).
	bot.fidgetStationarySince = 0;
	bot.fidgetRolledThisStop  = false;
	bot.fidgetDroppedThisWake = false;

	// Mount roll for the woken bot. Placed after both the pool-hit and the DB-materialize
	// branches have converged, so `player` is live and placed either way. This is the
	// dominant reconnect path in steady state — activateBot alone left woken bots with
	// whatever mount state they happened to carry into hibernation. Self-throttled against
	// proximity-wake churn, so a bot oscillating near an observer keeps its current look.
	rollMountForReconnect(bot, player);

	// Player spawn-claim defense-in-depth (NB1): wakeBot resumes a HUNTING bot from its
	// preserved state without passing through any assignment gate. In a consistent state the
	// kick already aborted this bot (huntScriptId zeroed), so this never fires — but guard
	// against invariant drift so a woken bot can never silently resume a player-claimed hunt.
	if (bot.huntScriptId > 0) {
		std::string sg;
		for (const auto& s : huntScripts_) {
			if (s.id == bot.huntScriptId) { sg = s.spawnGroup; break; }
		}
		if (isScriptPlayerClaimed(bot.huntScriptId, sg)) {
			activeHunts_.erase(bot.huntScriptId);
			if (!sg.empty()) activeSpawnGroups_.erase(sg);
			bot.huntScriptId = 0;
			bot.state = BotAIState::IDLE;
			castLog(bot, "WAKE: hunt script player-claimed, resetting to IDLE");
		}
	}

	bot.currentPos = player->getPosition();
	bot.lastPos = bot.currentPos;
	bot.cachedLevel = static_cast<uint32_t>(player->getLevel());  // refresh on wake (level may have grown)
	// Don't reset tickCounter — preserves heal cadence alignment

	// BOT_LIVENESS (2026-06-13): login sparkle for proximity wakes that landed ON a
	// viewer's screen. chooseWakePosition already relocated qualifying route-following
	// bots OFF-screen (they walk in → no sparkle); the bots still on-screen here are
	// idle/dwelling/city bots with no hibernation-surviving route — emit a teleport
	// sparkle so they read as a login instead of a silent pop-in. wouldBeSeenByAnchor(.,0)
	// is false for a bot stranded at the remote staging tile (no anchor there), so a
	// failed-teleport bot never sparkles. Off-screen + teleport/explicit wakes: no sparkle.
	// addMagicEffect sends to spectators (incl. cast viewers) and is a no-op if unobserved.
	const bool wokeVisible = wouldBeSeenByAnchor(bot.currentPos, 0);
	if (proximityWake && wokeVisible) {
		g_game().addMagicEffect(bot.currentPos, CONST_ME_TELEPORT);
	}

	// Stagger AI burst: 2026-05-27 redesigned from counter+cap (which only spread the
	// first 10 bots before clamping) to GUID-based deterministic stagger, matching
	// Gesior's per-bot stagger pattern. With 200 bots waking in proximity, the old
	// scheme had bots 11-200 all clamping to the same 13-tick quiet period — they
	// all ran their first AI burst on the same dispatcher window ~2.6s later,
	// producing the 5s GAP_SLOW cascades observed at 12:00:47.
	//
	// New: each bot picks its own deterministic stagger from its guid, spreading
	// the first-AI burst across (WAKE_STAGGER_SPREAD_TICKS × engine tick interval).
	// At 200ms tick × 30 ticks = 6s spread, average ~3s. Multiplying guid by 7 (a
	// small prime) decorrelates adjacent guids so neighbouring bots in the array
	// don't share modulo positions.
	// BOT_LIVENESS (2026-06-13): proximity wakes (walk-by / cast-watch, ≤5/300ms) use the
	// short quiet window so bots near a viewer act in ~200-800ms. TELEPORT/EXPLICIT mass
	// wakes keep the full GUID stagger (anti-cascade). Both keep a guid-decorrelated spread.
	// Third case (2026-08-21): a TELEPORT-burst bot standing inside the arriving player's
	// viewport. It has already been placed in the world by the code above, so the player sees it
	// immediately -- the only thing the long stagger buys here is that they watch it stand
	// motionless for up to 3.2s. Give those the short window; everything else is unchanged.
	//
	// Gated on s_burstCenterValid, which is set only for the duration of wakeBotsInRadius, so the
	// 300ms Lua proximity monitor, /cavebot wake and cast-login wakes all keep their old paths.
	const bool visibleBurstWake = !proximityWake && s_burstCenterValid
		&& botVisibleFrom(s_burstCenter, bot.currentPos);
	if (visibleBurstWake) {
		bot.wakeQuietTicks = WAKE_QUIET_VISIBLE_BASE_TICKS
			+ static_cast<uint8_t>((guid * 7u) % WAKE_QUIET_VISIBLE_SPREAD_TICKS);
	} else if (proximityWake) {
		bot.wakeQuietTicks = WAKE_QUIET_PROX_BASE_TICKS + static_cast<uint8_t>((guid * 7u) % WAKE_QUIET_PROX_SPREAD_TICKS);
	} else {
		bot.wakeQuietTicks = WAKE_QUIET_BASE_TICKS + static_cast<uint8_t>((guid * 7u) % WAKE_STAGGER_SPREAD_TICKS);
	}
	if (visibleBurstWake) {
		// Confirms from journalctl which bots actually took the fast tier -- the acceptance check
		// is "did the statues stop", and this is how that is verified without guessing.
		g_logger().info("[WAKE_FAST] bot='{}' guid={} at=({},{},{}) eye=({},{},{}) quiet={}",
			bot.name, guid, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
			s_burstCenter.x, s_burstCenter.y, s_burstCenter.z, bot.wakeQuietTicks);
	}
	if (recentWakeStagger_ < WAKE_STAGGER_CAP) {
		recentWakeStagger_++; // kept for legacy diagnostic / non-burst single-wake paths
	}

	// Re-enable cast broadcasting (lost when Player was destroyed). DB INSERT goes async
	// via DatabaseTasks so the dispatcher doesn't block on N serial DB writes during a
	// wake-burst. In-game cast routing uses player->castViewers in-memory, set above.
	player->setCastBroadcasting(true);
	auto &db = Database::getInstance();
	g_botDatabaseTasks().execute(fmt::format(
		"INSERT INTO `cast_broadcasters` (`player_id`, `player_name`) VALUES ({}, {}) "
		"ON DUPLICATE KEY UPDATE `player_name` = {}",
		guid, db.escapeString(bot.name), db.escapeString(bot.name)));

	// [WAKE_VIS] closed-loop telemetry: path=prox (walk-by/cast, off-screen+sparkle
	// eligible) vs burst (teleport/explicit, unchanged); visible=did it land on a
	// viewer's screen (true ⇒ sparkled; false on a proximity wake ⇒ walked in off-screen
	// or genuinely off-screen); anchors=current anchor count. Lets journalctl confirm the
	// fix (proximity wakes near a viewer should be mostly visible=false or visible=true+spark).
	g_logger().info("[BotEngine] Woke bot '{}' at ({},{},{}) state={} huntPhase={} [WAKE_VIS path={} visible={} sparkle={} anchors={}]",
		bot.name, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
		static_cast<int>(bot.state), static_cast<int>(bot.huntPhase),
		proximityWake ? "prox" : "burst", wokeVisible ? 1 : 0,
		(proximityWake && wokeVisible) ? 1 : 0, currentAnchorPts_.size());

	// Cascade: any party member woken → wake the whole team and materialize the Canary
	// Party retroactively. The thread_local guard ensures Party::create runs once at the
	// outermost call (cascade initiator), AFTER all members are live regardless of who
	// triggered the wake (leader or support). File-scope (see s_inPartyCascade above)
	// so shouldGateWake can also read it and exempt cascade members from the density cap.
	if (!s_inPartyCascade) {
		auto phIt = s_botToPartyHunt.find(guid);
		if (phIt != s_botToPartyHunt.end()) {
			uint32_t partyHuntId = phIt->second;

			// Phase B.1 fix (2026-06-01): RAII guard on s_inPartyCascade. The prior bare
			// set-true/set-false pattern would leak `true` permanently for this dispatcher
			// thread if any of the recursive wakeBot calls or materializeCanaryParty threw
			// — and once leaked, every subsequent wake bypasses shouldGateWake's density
			// cap silently until server restart. RAII makes the unwind safe.
			struct CascadeGuard {
				CascadeGuard() { s_inPartyCascade = true; }
				~CascadeGuard() { s_inPartyCascade = false; }
			} _cascadeGuard;

			// Snapshot member guids (s_partyHuntMembers may be mutated by recursive wakes).
			std::vector<uint32_t> members;
			auto memberIt = s_partyHuntMembers.find(partyHuntId);
			if (memberIt != s_partyHuntMembers.end()) {
				members = memberIt->second;
			}

			// Wake every OTHER hibernated member (recursive — inPartyCascade prevents nested cascade).
			for (uint32_t memberGuid : members) {
				if (memberGuid == guid) continue;
				auto it = guidToIndex_.find(memberGuid);
				if (it == guidToIndex_.end()) continue;
				if (bots_[it->second].hibernated) {
					// Propagate the initiating wake's path to cascade members so a
					// teleport-burst party stays "burst" (no sparkle storm) and a
					// proximity party walks in / sparkles consistently.
					s_proximityWake = proximityWake;
					wakeBot(memberGuid);
				}
			}

			// All members live now — build the real Canary Party.
			materializeCanaryParty(partyHuntId);
			// CascadeGuard destructor resets s_inPartyCascade=false on scope exit.
		}
	}
	wakeGranted60s_++;
	return true;
}

std::vector<uint32_t> BotEngine::getHibernatedBotGuids() const {
	std::vector<uint32_t> out;
	out.reserve(bots_.size());
	for (const auto &bot : bots_) {
		if (bot.hibernated) out.push_back(bot.guid);
	}
	return out;
}

uint32_t BotEngine::getHibernatedBotGuidByName(const std::string &name) const {
	for (const auto &bot : bots_) {
		if (bot.hibernated && bot.name == name) {
			// CONTRACT: this getter has exactly one caller — castViewerLogin
			// (protocolgame.cpp), which immediately follows up with wakeBot(guid).
			// Arm the single-shot density-gate bypass here so that explicit
			// by-name cast wake succeeds even when the bot sits in the
			// outerLimitPct=0 no-wake band. The flag is consumed by the very
			// next shouldGateWake call (any guid), so it cannot leak. If this
			// getter ever gains a second caller that does NOT wake, revisit.
			s_forceWakeGuid = bot.guid;
			return bot.guid;
		}
	}
	return 0;
}

uint32_t BotEngine::hibernateAllEligibleBots() {
	// Snapshot guids first; cascade calls may mutate iteration order.
	std::vector<uint32_t> guids;
	guids.reserve(bots_.size());
	for (const auto &bot : bots_) {
		if (bot.active && !bot.hibernated) guids.push_back(bot.guid);
	}
	uint32_t count = 0;
	for (uint32_t guid : guids) {
		if (hibernateBot(guid)) ++count;
	}
	return count;
}

uint32_t BotEngine::wakeAllHibernatedBots() {
	// Mass-wake (admin/debug). Same burst semantics as wakeBotsInRadius so all 200 bots
	// don't pile on identical virtualPos tiles when they wake simultaneously.
	beginWakeBurst();
	std::vector<uint32_t> guids;
	guids.reserve(bots_.size());
	for (const auto &bot : bots_) {
		if (bot.hibernated) guids.push_back(bot.guid);
	}
	uint32_t count = 0;
	for (uint32_t guid : guids) {
		// Explicit admin/benchmark bulk wake: bypass the density gate per-guid.
		// Without this, outerLimitPct=0 band mode reduces wake_all to "wake only
		// bots within midRadius of an anchor" — silently invalidating before/after
		// CPU benchmark runs that rely on hibernate_all/wake_all symmetry.
		s_forceWakeGuid = guid;
		s_proximityWake = false;  // EXPLICIT admin/benchmark bulk wake — no off-screen/sparkle
		if (wakeBot(guid)) ++count;
	}
	return count;
}

// Phase B file-scope statics are declared higher up (~line 1175) so they precede
// wakeBot, which reads s_inPartyCascade. See "PERF_INVESTIGATION_2026-05-24 Phase B"
// block there.

void BotEngine::refreshAnchorsIfStale(int64_t maxAgeMs) {
	const int64_t now = OTSYS_TIME();
	if (anchorsRefreshedAt_ != 0 && (now - anchorsRefreshedAt_) < maxAgeMs) {
		// Cache is fresh enough — reuse current clusters + counters.
		return;
	}

	const int32_t clusterRadius = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_ANCHOR_CLUSTER_RADIUS));
	const int32_t innerRadius   = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_INNER_RADIUS));
	const int32_t midRadius     = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_MID_RADIUS));
	const int32_t outerRadius   = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_OUTER_RADIUS));

	// Snapshot previous-cluster centroids so we can log [DENSITY] cluster_dissolve for
	// any cluster that vanishes this refresh. Same packed-centroid key as the rate-limiter.
	std::vector<AnchorCluster> prevAnchors = std::move(currentAnchors_);
	currentAnchors_.clear();

	// Build anchor position list (real players + cast-watched bot players). Mirrors the
	// filter in bot_hibernation.lua lines 84-88. Z-agnostic Chebyshev for all rings.
	// Cleared HERE, not alongside the two lists further down, because unlike them it is filled
	// DURING the player loop rather than after it. Clearing it down there wiped it immediately
	// after it was populated — every refresh — so isScriptHuntRepelled hit its empty early-out at
	// all six gate sites and the entire repel did nothing. Caught in review; this codebase has now
	// produced a fix that compiled, deployed and was inert three separate times.
	huntRepelPts_.clear();

	struct AnchorPos { Position p; bool roamOk; bool keepout; };
	std::vector<AnchorPos> anchors;
	anchors.reserve(8);
	for (const auto& [id, player] : g_game().getPlayers()) {
		if (!player) continue;
		const bool isBot = player->isBotPlayer();
		if (isBot && player->getCastViewerCount() == 0) continue;  // bot without cast viewer → not an anchor

		// BOT_AMBIENT_ROAM suppression. An anchor can be a perfectly good camera and still be
		// somewhere we must not send wanderers: nobody mid-hunt wants ambient strangers arriving in
		// the spawn they are working. The anchor still counts for visibility and for the density
		// cap — only roam's own targeting skips it.
		bool roamOk = true;
		bool keepout = false;   // roamOk is about who an injection is RAISED FOR; this is about
		                        // where any roamer may stand. See roamSuppressedPts_.
		if (isBot) {
			// A cast-watched bot actually working its spawn: PATROLLING is the phase where the bot
			// is in among the monsters, and the only one that suppresses.
			//
			// Known and accepted gap, recorded because the comment that used to sit here got it
			// wrong: LEAVING is NOT a road phase at its start. It begins at the patrol terminus,
			// inside the spawn (bot_hunt.cpp, the "hunt ended" branch that logs pos=), and abortHunt
			// enters it mid-patrol; it may last LEAVING_PHASE_MAX_MS = 5 minutes. A bot on its way
			// out is deliberately left as ordinary company — the operator's call, 2026-08-24 — but
			// nobody should re-derive "LEAVING means the road" from this comment.
			//
			// The predicate is isBotSpawnEngaged, not an inline phase test, because a party-hunt
			// MEMBER has no hunt phase of its own — see that function for the whole story.
			if (auto bit = guidToIndex_.find(player->getGUID()); bit != guidToIndex_.end()) {
				const HuntScript* worked = nullptr;
				if (isBotSpawnEngaged(bots_[bit->second], &worked)) {
					roamOk = false;
					// Geometry, unlike targeting, is reserved for actual spawns: a quest patrols
					// with the same phase but walks an authored route through towns and roads, and
					// a 30-tile no-roam bubble dragged across a city is a bigger cost than the
					// company it saves the quest bot.
					keepout = !botScriptIsQuest(worked);
				}
			}
		} else if (isPlayerHuntEngaged(player->getGUID()) && !isInTownArea(player->getPosition())) {
			roamOk = false;
			keepout = true;   // a claimed spawn, and the claimant said so explicitly
			// Same test, second consumer: this player also repels HUNT ASSIGNMENT (see
			// isScriptHuntRepelled). Recorded here because the condition is already computed and
			// caching it per refresh is what keeps the selector's cost flat.
			huntRepelPts_.push_back(player->getPosition());
			huntRepelPtsPeak_ = std::max(huntRepelPtsPeak_, static_cast<uint32_t>(huntRepelPts_.size()));
		}
		anchors.push_back({player->getPosition(), roamOk, keepout});
	}

	// BOT_AMBIENT_ROAM headless testing: the synthetic anchor joins the REAL anchor list here
	// rather than being special-cased at each use site. That is the whole point — it then forms
	// clusters, accrues density counts and acts as a camera for wouldBeSeenByAnchor exactly like
	// a player would, so a headless run exercises the same code a live one does instead of a
	// parallel path that could drift from it.
	if (roamDebugAnchor_.x > 0) {
		anchors.push_back({roamDebugAnchor_, true, false});
	}

	// Keep the raw anchor list for shouldGateWake's outerLimitPct=0 band rule
	// (min-Chebyshev-to-ANY-anchor; see currentAnchorPts_ declaration for why
	// centroids are the wrong geometry for that check).
	currentAnchorPts_.clear();
	roamAnchorPts_.clear();
	roamSuppressedPts_.clear();
	for (const auto& a : anchors) {
		currentAnchorPts_.push_back(a.p);
		if (a.roamOk) {
			roamAnchorPts_.push_back(a.p);
		}
		if (a.keepout) {
			// The other half of the suppression, and the one that owns GEOMETRY rather than
			// targeting: roam may not stage into, walk into, or linger inside the neighbourhood of
			// a spawn somebody is working, whichever anchor the injection was raised for. Tested
			// independently of roamOk because it is a subset of it, not its complement — a quest
			// patrol is ineligible without being a spawn.
			roamSuppressedPts_.push_back(a.p);
		}
	}
	roamSuppressedNow_ = static_cast<uint32_t>(roamSuppressedPts_.size());

	// Union-find clustering: anchors within clusterRadius (Chebyshev) merge.
	// Trivial size (≤8 anchors typical); O(N²) is fine.
	std::vector<int32_t> parent(anchors.size());
	std::iota(parent.begin(), parent.end(), 0);
	auto find = [&](int32_t x) {
		while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
		return x;
	};
	auto unite = [&](int32_t a, int32_t b) {
		a = find(a); b = find(b);
		if (a != b) parent[b] = a;
	};
	for (size_t i = 0; i < anchors.size(); ++i) {
		for (size_t j = i + 1; j < anchors.size(); ++j) {
			const int32_t dx = std::abs(static_cast<int32_t>(anchors[i].p.x) - static_cast<int32_t>(anchors[j].p.x));
			const int32_t dy = std::abs(static_cast<int32_t>(anchors[i].p.y) - static_cast<int32_t>(anchors[j].p.y));
			if (dx <= clusterRadius && dy <= clusterRadius) unite(static_cast<int32_t>(i), static_cast<int32_t>(j));
		}
	}

	// Aggregate members per cluster, compute centroid (mean position).
	std::unordered_map<int32_t, std::vector<int32_t>> clusterMembers;
	for (size_t i = 0; i < anchors.size(); ++i) {
		clusterMembers[find(static_cast<int32_t>(i))].push_back(static_cast<int32_t>(i));
	}
	for (auto& [root, members] : clusterMembers) {
		int64_t sx = 0, sy = 0, sz = 0;
		for (int32_t idx : members) {
			sx += anchors[idx].p.x;
			sy += anchors[idx].p.y;
			sz += anchors[idx].p.z;
		}
		// BOT_AMBIENT_ROAM: record roam-eligibility as CLUSTER MEMBERSHIP while the member list is
		// still in hand. Deriving it later from a radius test against the centroid is a proxy that
		// errs both ways — outerRadius re-admits the cross-cluster targeting the gate exists to
		// stop, and midRadius can skip a legitimate anchor at the end of an elongated union-find
		// chain. Membership is exact and costs one field.
		Position roamAnchorOfCluster;
		bool clusterHasRoamOk = false;
		for (int32_t idx : members) {
			if (anchors[idx].roamOk) {
				roamAnchorOfCluster = anchors[idx].p;
				clusterHasRoamOk = true;
				break;
			}
		}
		const int32_t n = static_cast<int32_t>(members.size());
		AnchorCluster cluster;
		cluster.hasRoamAnchor = clusterHasRoamOk;
		cluster.roamAnchor = roamAnchorOfCluster;
		cluster.centroid = Position(static_cast<uint16_t>(sx / n), static_cast<uint16_t>(sy / n), static_cast<uint8_t>(sz / n));
		cluster.anchorCount = static_cast<uint32_t>(n);
		cluster.counts = {0, 0, 0};
		// Seed peakCounts from a surviving previous cluster at the same centroid (within
		// clusterRadius), so the 60s periodic summary tracks peaks across refreshes
		// instead of resetting every 100ms.
		cluster.peakCounts = {0, 0, 0};
		for (const auto& prev : prevAnchors) {
			const int32_t dx = std::abs(static_cast<int32_t>(prev.centroid.x) - static_cast<int32_t>(cluster.centroid.x));
			const int32_t dy = std::abs(static_cast<int32_t>(prev.centroid.y) - static_cast<int32_t>(cluster.centroid.y));
			if (dx <= clusterRadius && dy <= clusterRadius) {
				cluster.peakCounts = prev.peakCounts;
				break;
			}
		}
		currentAnchors_.push_back(cluster);
	}

	// Count currently-awake bots in each ring of each cluster. Iterate bots_ once,
	// classify against every cluster.
	// Fix #12: AdvStone chest (mode 1) and dummy (mode 2) bots are EXCLUDED from
	// the density count — they legitimately cluster on a few tiles and would
	// otherwise crowd out the cap for other island bots (idle/dwelling), making
	// the cluster appear over-capacity when it's not really.
	for (const auto& bot : bots_) {
		if (bot.hibernated || !bot.active) continue;  // only awake bots count against caps
		if (isOnAdvStoneIsland(bot.currentPos)
		    && (bot.advStoneDwellMode == 1 || bot.advStoneDwellMode == 2)) continue;
		for (auto& cluster : currentAnchors_) {
			const int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cluster.centroid.x));
			const int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cluster.centroid.y));
			const int32_t cheb = std::max(dx, dy);
			if (cheb > outerRadius) continue;
			// BOT_AMBIENT_ROAM: a ledgered bot is counted in BOTH arrays. The organic arm of the
			// cap subtracts roamCounts, so roamers occupy their own reserve instead of eating the
			// budget ordinary proximity wakes draw from. Membership comes from the LEDGER, not the
			// live session, because a bot whose session ended but which is still awake beside the
			// player is still one of the extra bodies the reserve paid for.
			const bool ledgered = s_roamLedger.count(bot.guid) != 0;
			cluster.counts[2]++;  // bot in outer ring
			if (ledgered) cluster.roamCounts[2]++;
			if (cheb <= midRadius)   { cluster.counts[1]++; if (ledgered) cluster.roamCounts[1]++; }
			if (cheb <= innerRadius) { cluster.counts[0]++; if (ledgered) cluster.roamCounts[0]++; }
		}
	}

	// Update peaks (after counting) so periodic [DENSITY] reflects observed peak.
	for (auto& cluster : currentAnchors_) {
		for (int i = 0; i < 3; ++i) {
			if (cluster.counts[i] > cluster.peakCounts[i]) cluster.peakCounts[i] = cluster.counts[i];
		}
	}

	// Log [DENSITY] cluster_dissolve for any previous cluster with no surviving match.
	for (const auto& prev : prevAnchors) {
		bool survived = false;
		for (const auto& cur : currentAnchors_) {
			const int32_t dx = std::abs(static_cast<int32_t>(prev.centroid.x) - static_cast<int32_t>(cur.centroid.x));
			const int32_t dy = std::abs(static_cast<int32_t>(prev.centroid.y) - static_cast<int32_t>(cur.centroid.y));
			if (dx <= clusterRadius && dy <= clusterRadius) { survived = true; break; }
		}
		if (!survived) {
			g_logger().info("[DENSITY] cluster_dissolve centroid=({},{},{}) prev_counts=[{},{},{}] prev_anchors={}",
				prev.centroid.x, prev.centroid.y, prev.centroid.z,
				prev.counts[0], prev.counts[1], prev.counts[2], prev.anchorCount);
		}
	}

	anchorsRefreshedAt_ = now;

	// Periodic [PROXBIAS] summary every 60s (player-proximity weighting, 2026-06-15):
	// selection-tier tallies, hunt near-player supply (eligible vs reserved — reservation-
	// throttle diagnostic), and a per-town hibernated histogram (mass-migration / depletion
	// watch). Counters reset after logging.
	if (now - s_lastProxPeriodicLogMs > 60000) {
		s_lastProxPeriodicLogMs = now;
		const uint64_t selTot = s_proxSelNear + s_proxSelMid + s_proxSelFar;
		if (selTot > 0 || s_proxHuntNearEligible > 0 || s_proxHuntNearReserved > 0) {
			g_logger().info("[PROXBIAS] summary sel_near={} sel_mid={} sel_far={} hunt_near_eligible={} hunt_near_reserved={} anchors={}",
				s_proxSelNear, s_proxSelMid, s_proxSelFar,
				s_proxHuntNearEligible, s_proxHuntNearReserved, currentAnchorPts_.size());
			std::unordered_map<uint32_t, uint32_t> townHib;
			for (const auto& b : bots_) {
				if (b.hibernated) townHib[b.townId]++;
			}
			std::vector<std::pair<uint32_t, uint32_t>> sorted(townHib.begin(), townHib.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string hist;
			for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
				if (!hist.empty()) hist += " ";
				hist += fmt::format("t{}={}", sorted[i].first, sorted[i].second);
			}
			g_logger().info("[PROXBIAS] hib_by_town(top8) {}", hist);
		}
		s_proxSelNear = s_proxSelMid = s_proxSelFar = 0;
		s_proxHuntNearEligible = s_proxHuntNearReserved = 0;
	}

	// Periodic [DENSITY] summary every 60s. Logs current snapshot + peak-since-last-log.
	if (now - s_lastDensityPeriodicLogMs > 60000) {
		s_lastDensityPeriodicLogMs = now;
		for (auto& cluster : currentAnchors_) {
			g_logger().info("[DENSITY] periodic centroid=({},{},{}) anchors={} counts_now=[{},{},{}] counts_peak=[{},{},{}]",
				cluster.centroid.x, cluster.centroid.y, cluster.centroid.z,
				cluster.anchorCount,
				cluster.counts[0], cluster.counts[1], cluster.counts[2],
				cluster.peakCounts[0], cluster.peakCounts[1], cluster.peakCounts[2]);
			// Reset peak after logging so next interval tracks its own peak.
			cluster.peakCounts = cluster.counts;
		}
	}
}

std::vector<std::string> BotEngine::getActiveBotNames() const {
	std::vector<std::string> out;
	out.reserve(bots_.size());
	for (const auto &bot : bots_) {
		// `bot.active` is set at registerBot, never cleared until unregisterBot —
		// so this returns a stable list regardless of whether the bot is currently
		// hibernated or awake or mid-transition. Cast list filters out bots in
		// transition states with the old broadcasting filter; this avoids that.
		if (bot.active && !bot.name.empty()) {
			out.push_back(bot.name);
		}
	}
	return out;
}

std::vector<uint32_t> BotEngine::getActiveBotGuids() const {
	std::vector<uint32_t> out;
	out.reserve(bots_.size());
	for (const auto &bot : bots_) {
		if (bot.active && !bot.name.empty()) {
			out.push_back(bot.guid);
		}
	}
	return out;
}

size_t BotEngine::getHibernatedBotCount() const {
	size_t n = 0;
	for (const auto &bot : bots_) {
		if (bot.active && bot.hibernated) {
			++n;
		}
	}
	return n;
}

// ============================================================================
// Player spawn-claim
// ============================================================================

// True if scriptId — or any non-empty spawnGroup sibling — is currently player-claimed
// and not expired. Lazily erases expired entries it walks past (defense-in-depth vs the
// periodic tick sweep). Consulted at every bot hunt-assignment gate.
// "In town" for roam-suppression purposes: close enough to a town temple that ambient traffic still
// belongs there. Someone who claims a spawn and then stands in the depot sorting their backpack has
// not asked for an empty city — the quiet they want starts when they head out. findNearestTown
// cannot answer this alone: it returns the closest town at ANY distance, so the radius must be an
// explicit test rather than a lookup.
bool BotEngine::isInTownArea(const Position& p) const {
	// Horizontal proximity alone is not enough: sewers and under-temple complexes sit directly
	// beneath the temple, and roam is cross-floor, so a purely 2D test would call a player hunting
	// in the sewers "in town" and keep sending wanderers down to them.
	//
	// But a flat surface cutoff is just as wrong in the other direction — six of this map's towns
	// have their STREETS underground (Kazordoon at z11, town 27 at z14; see getCityWalkZ). A flat
	// `z > 7 -> not in town` rule would classify a player standing in the Kazordoon depot as out of
	// town and suppress roam inside the city, which is the precise opposite of the intent.
	//
	// So compare the player's floor against that town's own walking level, falling back to the
	// temple's floor for towns absent from the table. ±1 keeps upstairs and cellar tiles inside the
	// city while still excluding anything deeper.
	// Cheapest first, and each rule answers "is this somewhere people gather" from a different
	// angle, because no single one covers the map.
	//
	// 1. A protection zone is the game's OWN marker for safe, town-like ground. It needs no table
	//    and is right by construction for temples, depots and most city cores.
	if (auto tile = g_game().map.getTile(p); tile && tile->hasFlag(TILESTATE_PROTECTIONZONE)) {
		return true;
	}

	// 2. Near a town temple, on a floor compatible with that town's street level.
	const auto& walkZ = getCityWalkZ();
	for (const auto& [id, town] : g_game().map.towns.getTowns()) {
		if (id == 0 || !town) continue;
		const auto tp = town->getTemplePosition();
		const int32_t d = std::max(std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(tp.x)),
		                           std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(tp.y)));
		if (d > ROAM_TOWN_RADIUS) continue;
		auto wit = walkZ.find(id);
		const int32_t cityZ = (wit != walkZ.end()) ? static_cast<int32_t>(wit->second)
		                                          : static_cast<int32_t>(tp.z);
		const int32_t dz = static_cast<int32_t>(p.z) - cityZ;
		// ASYMMETRIC on purpose. A symmetric ±1 reopens the case this z test exists to close:
		// surface-town sewers sit at exactly cityZ+1, and town sewers are claimable spawns (the
		// Thais and Venore rotworms are the canonical low-level claim), so ±1 would leave the most
		// common hunt-in-town-sewers case unsuppressed.
		//
		// Upper floors of town buildings are ABOVE street level, so cityZ-1 is accepted outright.
		// Below street level is accepted only when the tile is a protection zone — that is exactly
		// what separates a temple or depot cellar from a sewer, since sewers are never PZ.
		//
		// The two errors are not symmetric either, which decides the shape: under-suppression is
		// the user-visible complaint (wanderers arriving at the spawn you claimed), while
		// over-suppression is invisible (a flagged player upstairs gets slightly less ambient
		// traffic). Prefer the invisible error.
		if (dz == 0 || dz == -1) return true;
		if (dz == 1) {
			// (the PZ case is already handled by rule 1 above)
			continue;
		}
	}

	// 3. Standing among NPCs. This is the rule that makes the test work for places the towns
	//    table cannot describe. Marapur is the worked example: it is town 28 with a recorded street
	//    level of z7, it owns ZERO city POI rows, and its hub is spread across z2/z4/z5/z6 — so a
	//    player at (33777,32754,5) fails both rules above and would be treated as out in the wild,
	//    suppressing exactly the ambient traffic a city centre should have. It has three NPCs on
	//    that floor within 23 tiles, so this rule places it correctly with no table to maintain.
	//
	//    Same floor only, and a tight radius: "shopkeepers are around me" is the claim being made,
	//    and a lone quest NPC parked in a dungeon should not turn that dungeon into a town. Last
	//    because it is the most expensive, and it only ever runs for a hunt-flagged player.
	int32_t npcsNear = 0;
	for (const auto& [name, instances] : npcAnchors_) {
		for (const auto& a : instances) {
			if (a.npcPos.z != p.z) continue;
			const int32_t d = std::max(std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(a.npcPos.x)),
			                           std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(a.npcPos.y)));
			// TWO of them, not one. The anchor table is built from every NPC in the world, so a
			// single instance is satisfied by a dungeon hermit or a fortress trader — which would
			// make this rule do the exact thing its own comment says it must not. Djinn fortresses
			// and Nargor are the sharp cases: NPCs and monsters sharing a room. Marapur has three
			// on z5, so the worked example still passes.
			if (d <= ROAM_TOWN_NPC_RADIUS && ++npcsNear >= 2) return true;
		}
	}
	return false;
}

// True when a spawn sits close enough to a hunt-flagged, out-of-town player that offering it to a
// bot would send a stranger into their hunt.
//
// This exists because the selector does the OPPOSITE by default: candidate spawns are weighted by
// proximity to anchors, so claiming a spawn and standing on it actively attracts bots to every
// script around you. isScriptPlayerClaimed already blocks the ONE script a successful claim
// resolved to; this covers the two holes it leaves — neighbouring spawns whose patrol runs past
// you, and a claim that resolved to nothing at all, which writes no claim row and therefore blocks
// nothing. Here the flag alone is sufficient, which is precisely the case the user called out.
//
// Sampled the same way the proximity weighting samples, so cost is bounded by proxSampleCap rather
// than patrol length — and free entirely when nobody is flagged.
bool BotEngine::isScriptHuntRepelled(const HuntScript& s) const {
	if (huntRepelPts_.empty()) return false;
	huntRepelEvaluated_++;
	const auto& wps = s.patrolWaypoints;
	const size_t n = wps.size();
	if (n == 0) return false;
	const size_t cap = static_cast<size_t>(std::max(2, livenessCfg_.proxSampleCap));
	const size_t stride = (n <= cap) ? 1 : (n - 1) / (cap - 1);
	auto nearAny = [&](const Position& wp) {
		for (const auto& r : huntRepelPts_) {
			const int32_t d = std::max(std::abs(static_cast<int32_t>(wp.x) - static_cast<int32_t>(r.x)),
			                           std::abs(static_cast<int32_t>(wp.y) - static_cast<int32_t>(r.y)));
			if (d <= HUNT_REPEL_TILES) return true;
		}
		return false;
	};
	for (size_t i = 0; i < n; i += stride) {
		if (nearAny(wps[i].pos)) { huntRepelRejected_++; return true; }
	}
	if (nearAny(wps[n - 1].pos)) { huntRepelRejected_++; return true; }  // last, as the sampler does
	return false;
}

// BOT_AMBIENT_ROAM. "Is this bot in among the monsters right now?"
//
// Written as a derivation rather than a field read because the field does not exist for half the
// bots that need it. Only a hunt-phase MACHINE writes huntPhase (beginHuntPhase), and in a bot-led
// party hunt only the LEADER runs one: members sit in BotAIState::PARTY carrying whatever phase
// they were conscripted with, and the leader->member mirror that the previous version of this test
// relied on exists solely in virtualAdvancePartyHunt — the hibernated sim. Live, it never ran, so
// the suppression was a no-op for every cast-watched party member, which is exactly the population
// most likely to have a viewer attached. Asking the leader is the fix that cannot rot: it reads the
// authority instead of a copy.
//
// The reservation, not the state enum, carries the first clause. A hunt in progress is
// huntScriptId > 0, and PvP can move a patrolling bot through COMBAT / FLEEING / PK_ATTACK without
// touching huntPhase (doSelfDefense sets state; exitCombat restores HUNTING). Testing the
// reservation keeps the spawn protected across that window instead of opening it for the duration
// of a fight.
bool BotEngine::isBotSpawnEngaged(const BotState& b, const HuntScript** outScript) const {
	// Resolve the script id of whichever bot is actually running the hunt, then answer once at the
	// bottom. Written this way so the leader derivation below has exactly one exit that fills
	// outScript — an early `return true` per branch is how the two answers drift apart.
	uint32_t workedId = 0;
	if (b.huntScriptId > 0 && b.huntPhase == HuntPhase::PATROLLING) {
		workedId = b.huntScriptId;
	} else if (b.state == BotAIState::PARTY) {
		workedId = partyLeaderWorkedScriptId(b);
	}
	if (workedId == 0) return false;
	if (outScript) {
		for (const auto& s : huntScripts_) {
			if (s.id == workedId) { *outScript = &s; break; }
		}
	}
	return true;
}

// The leader half of isBotSpawnEngaged, split out only so that function has one exit.
uint32_t BotEngine::partyLeaderWorkedScriptId(const BotState& b) const {
	// partyLeaderGuid is the direct link; the session map is the fallback for a member whose field
	// was cleared by a teardown that has not finished.
	uint32_t leaderGuid = b.partyLeaderGuid;
	if (leaderGuid == 0) {
		if (auto ph = s_botToPartyHunt.find(b.guid); ph != s_botToPartyHunt.end()) {
			if (auto lg = s_partyHuntLeaderGuid.find(ph->second); lg != s_partyHuntLeaderGuid.end()) {
				leaderGuid = lg->second;
			}
		}
	}
	if (leaderGuid == 0 || leaderGuid == b.guid) return 0;  // human-led party, or self-reference
	auto it = guidToIndex_.find(leaderGuid);
	if (it == guidToIndex_.end()) return 0;
	const BotState& leader = bots_[it->second];
	if (leader.huntScriptId > 0 && leader.huntPhase == HuntPhase::PATROLLING) return leader.huntScriptId;
	return 0;
}

bool BotEngine::isPlayerHuntEngaged(uint32_t guid) const {
	auto it = playerHuntEngaged_.find(guid);
	return it != playerHuntEngaged_.end() && OTSYS_TIME() < it->second;
}

// Stamped by the claim command on EVERY invocation, successful or not. A player who typed the
// command has announced they are hunting; whether the engine could map their spawn to a known
// script is our problem, not theirs, and should not decide whether strangers wander past them.
void BotEngine::markPlayerHuntEngaged(uint32_t guid) {
	playerHuntEngaged_[guid] = OTSYS_TIME() + PLAYER_CLAIM_DURATION_MS;
}

bool BotEngine::isScriptPlayerClaimed(uint32_t scriptId, const std::string& spawnGroup) {
	int64_t now = OTSYS_TIME();
	auto it = playerClaims_.find(scriptId);
	if (it != playerClaims_.end()) {
		if (now >= it->second.expiresAt) {
			playerClaims_.erase(it);
		} else {
			return true;
		}
	}
	if (!spawnGroup.empty()) {
		for (auto j = playerClaims_.begin(); j != playerClaims_.end();) {
			if (now >= j->second.expiresAt) {
				j = playerClaims_.erase(j);
				continue;
			}
			if (j->second.spawnGroup == spawnGroup) {
				return true;
			}
			++j;
		}
	}
	return false;
}

// Find the hunt script whose spawn a position is standing in: min Chebyshev distance from
// pos to any patrol waypoint of an enabled "hunt" script, hard-gated to the same z. Fills
// the best + runner-up (for ambiguity detection). Returns nullptr if nothing is in range
// of the caller's threshold check (bestDist is set so the caller can apply PLAYER_CLAIM_MAX_DIST).
const HuntScript* BotEngine::detectClaimableScript(const Position& pos, int32_t& bestDist, const HuntScript*& runnerUp, int32_t& runnerUpDist) {
	const HuntScript* best = nullptr;
	bestDist = INT32_MAX;
	runnerUp = nullptr;
	runnerUpDist = INT32_MAX;
	for (const auto& s : huntScripts_) {
		if (!s.enabled) continue;
		if (s.patrolWaypoints.empty()) continue;
		if (s.scriptCategory != "hunt") continue;
		int32_t sd = INT32_MAX;
		for (const auto& wp : s.patrolWaypoints) {
			if (wp.pos.z != pos.z) continue; // hard same-floor gate
			int32_t d = std::max(
				std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(wp.pos.x)),
				std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(wp.pos.y)));
			if (d < sd) sd = d;
		}
		if (sd == INT32_MAX) continue; // no patrol waypoint on the player's floor
		if (sd < bestDist) {
			runnerUp = best;
			runnerUpDist = bestDist;
			best = &s;
			bestDist = sd;
		} else if (sd < runnerUpDist) {
			runnerUp = &s;
			runnerUpDist = sd;
		}
	}
	return best;
}

// Evict whichever bot currently reserves a script (and/or its spawnGroup), sending it to
// temple + IDLE. Mirrors the partyhunt force-clear template (1-bot-per-spawn reservation).
// Returns true if a reservation existed (kickedName set to the kicked bot, "" if the holder
// could not be resolved and the reservation was just released directly).
bool BotEngine::kickSpawnHolder(const HuntScript& script, const std::string& reason, std::string& kickedName) {
	kickedName.clear();

	auto evict = [&](uint32_t holderGuid) -> bool {
		auto holderIt = guidToIndex_.find(holderGuid);
		if (holderIt == guidToIndex_.end()) return false;
		auto& holderBot = bots_[holderIt->second];
		auto hp = holderBot.getPlayer();
		kickedName = hp ? hp->getName() : holderBot.name;
		abortHunt(holderBot, reason); // releases activeHunts_/activeSpawnGroups_, dissolves party if leader
		holderBot.state = BotAIState::IDLE;
		holderBot.hasWalkTarget = false;
		if (hp) { // hibernated holder has no Player — lock already freed, skip teleport
			auto templePos = hp->getTemplePosition();
			BOT_TELEPORT(hp, templePos, true);
			holderBot.currentPos = templePos;
			holderBot.lastPos = templePos;
		}
		return true;
	};

	bool kicked = false;
	uint32_t ahHolder = 0;
	auto ahIt = activeHunts_.find(script.id);
	if (ahIt != activeHunts_.end()) {
		ahHolder = ahIt->second; // capture before abortHunt invalidates the map entry
		if (!evict(ahHolder)) {  // holder bot not resolvable — release the reservation directly
			activeHunts_.erase(script.id);
			if (!script.spawnGroup.empty()) activeSpawnGroups_.erase(script.spawnGroup);
		}
		kicked = true;
	}
	// A different script sharing this spawnGroup may still hold the physical-spawn lock.
	if (!script.spawnGroup.empty()) {
		auto sgIt = activeSpawnGroups_.find(script.spawnGroup);
		if (sgIt != activeSpawnGroups_.end() && sgIt->second != ahHolder) {
			uint32_t sgHolder = sgIt->second;
			if (!evict(sgHolder)) {
				activeSpawnGroups_.erase(script.spawnGroup);
			}
			kicked = true;
		}
	}
	return kicked;
}

// ============================================================================
// Population scheduling
// ============================================================================

void BotEngine::doPopulationManagement() {
	int64_t now = OTSYS_TIME();

	if (engineStartTime_ == 0) engineStartTime_ = now;

	// Phase 4: kill the boot ramp. Scheduler runs every 1s while activeCount < totalBots
	// (boot/load ramp), drops to 10s once population is full (steady state). With
	// POPULATION_RAMP_ENABLED=false, each pass activates ALL eligible bots in one shot,
	// so the cast list fills within ~25s of boot instead of ~17 minutes.
	uint32_t totalBotsForRate = countTotalBots();
	int64_t rateLimit = (totalBotsForRate > 0 && countActiveBots() < totalBotsForRate) ? 1000 : 10000;
	if (now - lastPopulationTick_ < rateLimit) return;
	lastPopulationTick_ = now;

	if (!schedulerEnabled_) return;

	uint32_t totalBots = countTotalBots();
	if (totalBots == 0) return;

	// Compute target count
	int32_t targetCount;
	if constexpr (POPULATION_TIME_OF_DAY_ENABLED) {
		// Get current server time
		time_t rawTime = std::time(nullptr);
		struct tm* timeInfo = std::localtime(&rawTime);
		int currentHour = timeInfo->tm_hour;
		int currentMinute = timeInfo->tm_min;

		// Refresh jitter when hour changes
		if (currentHour != lastJitterHour_) {
			int32_t maxJitter = static_cast<int32_t>(totalBots * 5 / 100); // 5% of total
			populationJitter_ = (maxJitter > 0) ? (std::rand() % (2 * maxJitter + 1)) - maxJitter : 0;
			lastJitterHour_ = currentHour;

			int32_t pct = getSchedulePercent(currentHour, currentMinute);
			int32_t target = static_cast<int32_t>(totalBots) * pct / 100 + populationJitter_;
			target = std::max(0, std::min(target, static_cast<int32_t>(totalBots)));
			g_logger().info("[BotScheduler] Hour {} — target: {} bots ({}% + jitter {}), active: {}",
				currentHour, target, pct, populationJitter_, countActiveBots());
		}

		int32_t pct = getSchedulePercent(currentHour, currentMinute);
		targetCount = static_cast<int32_t>(totalBots) * pct / 100 + populationJitter_;
		targetCount = std::max(0, std::min(targetCount, static_cast<int32_t>(totalBots)));
	} else {
		// Time-of-day disabled: target is always 100%
		targetCount = static_cast<int32_t>(totalBots);
	}

	int32_t activeCount = static_cast<int32_t>(countActiveBots());
	int32_t delta = targetCount - activeCount;

	if (delta > 0) {
		// Need more bots — ramp limits activation to 2 per tick, otherwise activate all
		int32_t toActivate;
		if constexpr (POPULATION_RAMP_ENABLED) {
			toActivate = std::min(delta, 2);
		} else {
			toActivate = delta;
		}
		int32_t activated = 0;
		std::vector<size_t> inactiveIndices;
		for (size_t i = 0; i < bots_.size(); i++) {
			if (!bots_[i].active && bots_[i].guid > 0) {
				inactiveIndices.push_back(i);
			}
		}
		// Fisher-Yates shuffle for random selection
		for (size_t i = inactiveIndices.size(); i > 1; i--) {
			size_t j = std::rand() % i;
			std::swap(inactiveIndices[i - 1], inactiveIndices[j]);
		}
		for (size_t i = 0; i < inactiveIndices.size() && activated < toActivate; i++) {
			if (activateBot(bots_[inactiveIndices[i]].guid)) {
				activated++;
			}
		}
		if (activated > 0) {
			g_logger().info("[BotScheduler] Activated {} bots (active: {} -> {}, target: {})",
				activated, activeCount, activeCount + activated, targetCount);
		}
	} else if (delta < 0) {
		// Too many bots — ramp limits deactivation to 2 per tick, otherwise deactivate all needed
		int32_t toDeactivate;
		if constexpr (POPULATION_RAMP_ENABLED) {
			toDeactivate = std::min(-delta, 2);
		} else {
			toDeactivate = -delta;
		}
		int32_t deactivated = 0;
		std::vector<size_t> eligibleIndices;
		for (size_t i = 0; i < bots_.size(); i++) {
			if (bots_[i].active && (bots_[i].state == BotAIState::IDLE || bots_[i].state == BotAIState::DWELLING)) {
				eligibleIndices.push_back(i);
			}
		}
		for (size_t i = eligibleIndices.size(); i > 1; i--) {
			size_t j = std::rand() % i;
			std::swap(eligibleIndices[i - 1], eligibleIndices[j]);
		}
		for (size_t i = 0; i < eligibleIndices.size() && deactivated < toDeactivate; i++) {
			if (deactivateBot(bots_[eligibleIndices[i]].guid)) {
				deactivated++;
			}
		}
		if (deactivated > 0) {
			g_logger().info("[BotScheduler] Deactivated {} bots (active: {} -> {}, target: {})",
				deactivated, activeCount, activeCount - deactivated, targetCount);
		}
	}
}

// ============================================================================
// Info / query methods
// ============================================================================

uint32_t BotEngine::countActiveBots() const {
	uint32_t count = 0;
	for (const auto& bot : bots_) {
		if (bot.active) count++;
	}
	return count;
}

uint32_t BotEngine::countTotalBots() const {
	return static_cast<uint32_t>(bots_.size());
}

// ============================================================================
// Liveness diagnostics (2026-06-09) — proximity inventory + population report
// ============================================================================
// Diagnose why few bots appear awake near a player. Output classifies the
// possible causes: bots NOT virtually near (population), bots gated by density
// cap, or wake attempts silently failing (placement / chooseWakePosition).

std::string BotEngine::buildProximityReport() {
	refreshAnchorsIfStale(100);
	const int32_t innerR = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_INNER_RADIUS));
	const int32_t midR   = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_MID_RADIUS));
	const int32_t outerR = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_OUTER_RADIUS));

	std::string out;
	if (currentAnchors_.empty()) {
		out = "[PROXIMITY] no active anchors (no real players online + no cast viewers)";
		return out;
	}

	for (const auto& cluster : currentAnchors_) {
		uint32_t innerHib = 0, innerAwake = 0;
		uint32_t midHib = 0, midAwake = 0;
		uint32_t outerHib = 0, outerAwake = 0;
		uint32_t sIdle = 0, sDwell = 0, sHunt = 0, sTravel = 0, sParty = 0, sCombat = 0;
		for (const auto& bot : bots_) {
			if (!bot.active) continue;
			const int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cluster.centroid.x));
			const int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cluster.centroid.y));
			const int32_t cheb = std::max(dx, dy);
			if (cheb > outerR) continue;
			(bot.hibernated ? outerHib : outerAwake)++;
			if (cheb <= midR) (bot.hibernated ? midHib : midAwake)++;
			if (cheb <= innerR) (bot.hibernated ? innerHib : innerAwake)++;
			switch (bot.state) {
				case BotAIState::IDLE:      sIdle++;   break;
				case BotAIState::DWELLING:  sDwell++;  break;
				case BotAIState::HUNTING:   sHunt++;   break;
				case BotAIState::TRAVELING: sTravel++; break;
				case BotAIState::PARTY:     sParty++;  break;
				case BotAIState::COMBAT:    sCombat++; break;
				default: break;
			}
		}
		const uint32_t silentFail = (wakeTried60s_ >= wakeGated60s_ + wakeGranted60s_)
			? (wakeTried60s_ - wakeGated60s_ - wakeGranted60s_) : 0;
		out += fmt::format(
			"[PROXIMITY] anchor=({},{},{}) rings inner_hib={}/awake={} | mid_hib={}/awake={} | outer_hib={}/awake={}\n"
			"  wake_60s: tried={} granted={} gated={} silent_fail={}\n"
			"  outer_states: IDLE={} DWELL={} HUNT={} TRAVEL={} PARTY={} COMBAT={}\n",
			cluster.centroid.x, cluster.centroid.y, cluster.centroid.z,
			innerHib, innerAwake, midHib, midAwake, outerHib, outerAwake,
			wakeTried60s_, wakeGranted60s_, wakeGated60s_, silentFail,
			sIdle, sDwell, sHunt, sTravel, sParty, sCombat);
	}
	return out;
}

std::string BotEngine::buildTickPhaseHistogram() const {
	// Count active (non-hibernated) bots in each of the 30 tick-phase buckets. A roughly
	// uniform distribution confirms guid-phasing is spreading AI ticks across dispatcher windows.
	uint32_t buckets[BOT_TICK_PHASE_LCM] = {0};
	uint32_t total = 0;
	for (const auto& bot : bots_) {
		if (!bot.active || bot.hibernated) continue;
		buckets[bot.tickCounter % BOT_TICK_PHASE_LCM]++;
		total++;
	}
	uint32_t mn = total, mx = 0, nonEmpty = 0;
	for (uint32_t b : buckets) {
		if (b < mn) mn = b;
		if (b > mx) mx = b;
		if (b > 0) nonEmpty++;
	}
	double mean = total > 0 ? static_cast<double>(total) / BOT_TICK_PHASE_LCM : 0.0;
	std::string h;
	for (uint32_t i = 0; i < BOT_TICK_PHASE_LCM; i++) {
		h += fmt::format("{}{}", i == 0 ? "" : ",", buckets[i]);
	}
	return fmt::format("[TICK_PHASE] active={} buckets={} mean={:.1f} min={} max={} maxOverMean={:.2f} hist=[{}]",
		total, nonEmpty, mean, mn, mx, mean > 0 ? mx / mean : 0.0, h);
}

std::string BotEngine::buildPopulationReport() const {
	// Aggregate per-town counts by state. townId 0 means undetected/island.
	struct TownStats {
		std::string name;
		uint32_t total = 0, hib = 0, awake = 0;
		uint32_t sIdle = 0, sDwell = 0, sHunt = 0, sTravel = 0, sParty = 0, sCombat = 0;
	};
	std::unordered_map<uint32_t, TownStats> per;
	for (const auto& bot : bots_) {
		if (!bot.active) continue;
		auto& t = per[bot.townId];
		if (t.name.empty()) t.name = bot.townName.empty() ? fmt::format("town{}", bot.townId) : bot.townName;
		t.total++;
		(bot.hibernated ? t.hib : t.awake)++;
		switch (bot.state) {
			case BotAIState::IDLE:      t.sIdle++;   break;
			case BotAIState::DWELLING:  t.sDwell++;  break;
			case BotAIState::HUNTING:   t.sHunt++;   break;
			case BotAIState::TRAVELING: t.sTravel++; break;
			case BotAIState::PARTY:     t.sParty++;  break;
			case BotAIState::COMBAT:    t.sCombat++; break;
			default: break;
		}
	}
	std::string out = "[POPULATION] per-town (active bots only)\n";
	std::vector<std::pair<uint32_t, TownStats>> sorted(per.begin(), per.end());
	std::sort(sorted.begin(), sorted.end(),
		[](const auto& a, const auto& b) { return a.second.total > b.second.total; });
	for (const auto& [tid, t] : sorted) {
		out += fmt::format("  {} ({}): total={} hib={} awake={} | IDLE={} DWELL={} HUNT={} TRAVEL={} PARTY={} COMBAT={}\n",
			t.name, tid, t.total, t.hib, t.awake,
			t.sIdle, t.sDwell, t.sHunt, t.sTravel, t.sParty, t.sCombat);
	}
	return out;
}

void BotEngine::broadcastAdminLog(const std::string& multiline) {
	// Iterate players, send to anyone with admin access (GMs, GODs, etc.).
	// MESSAGE_GAMEMASTER_CONSOLE lands in the client's console / Server Log tab
	// only — no on-screen popup. Right choice for 60s periodic emissions that
	// would otherwise spam the bottom of the screen. Caller passes the full
	// multi-line string; sendTextMessage handles newlines.
	for (const auto& [id, player] : g_game().getPlayers()) {
		if (!player || !player->isAccessPlayer()) continue;
		player->sendTextMessage(MESSAGE_GAMEMASTER_CONSOLE, multiline);
	}
}

BotState* BotEngine::getBotState(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return nullptr;
	return &bots_[it->second];
}

std::string BotEngine::getStatusText(uint32_t guid) {
	auto* bot = getBotState(guid);
	if (!bot) return "";
	return buildStatusDetail(*bot);
}


// ============================================================================
// State persistence (save/restore on graceful shutdown/startup)
// ============================================================================

void BotEngine::clearPersistedStates() {
	auto& db = Database::getInstance();
	db.executeQuery("DELETE FROM `bot_state_persistence`");
	g_logger().info("[BotEngine] clearPersistedStates: table cleared");
}

void BotEngine::saveSingleBotState(const BotState& bot) {
	auto& db = Database::getInstance();

	// Resolve name: use live player if available, fall back to cached bot.name
	std::string escapedName;
	auto player = bot.getPlayer();
	if (player) {
		escapedName = db.escapeString(player->getName());
	} else if (!bot.name.empty()) {
		escapedName = db.escapeString(bot.name);
	} else {
		return; // can't save without a name
	}

	uint8_t aiState = static_cast<uint8_t>(bot.state);
	// Normalize transient combat states to IDLE
	if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::PK_ATTACK ||
	    bot.state == BotAIState::FLEEING || bot.state == BotAIState::PARTY) {
		aiState = static_cast<uint8_t>(BotAIState::IDLE);
	}
	// Inactive bot holding a hunt reservation — save as HUNTING so it can be restored
	if (!bot.active && bot.huntScriptId > 0) {
		aiState = static_cast<uint8_t>(BotAIState::HUNTING);
	}

	std::string huntScriptId = "NULL";
	std::string huntPhase = "NULL";
	std::string waypointIdx = "NULL";
	std::string killCount = "NULL";
	if (aiState == static_cast<uint8_t>(BotAIState::HUNTING)) {
		huntScriptId = std::to_string(bot.huntScriptId);
		huntPhase = std::to_string(static_cast<uint8_t>(bot.huntPhase));
		waypointIdx = std::to_string(bot.huntWaypointIdx);
		killCount = std::to_string(bot.huntKillCount);
	}

	std::string travelDest = "NULL";
	if (bot.state == BotAIState::TRAVELING) {
		travelDest = std::to_string(bot.travelDestTownId);
	}

	db.executeQuery(fmt::format(
		"INSERT INTO `bot_state_persistence` "
		"(`guid`, `name`, `ai_state`, `hunt_script_id`, `hunt_phase`, `waypoint_idx`, `kill_count`, `travel_dest_town_id`) "
		"VALUES ({},{},{},{},{},{},{},{}) "
		"ON DUPLICATE KEY UPDATE "
		"`ai_state`=VALUES(`ai_state`), `hunt_script_id`=VALUES(`hunt_script_id`), "
		"`hunt_phase`=VALUES(`hunt_phase`), `waypoint_idx`=VALUES(`waypoint_idx`), "
		"`kill_count`=VALUES(`kill_count`), `travel_dest_town_id`=VALUES(`travel_dest_town_id`)",
		bot.guid, escapedName, aiState,
		huntScriptId, huntPhase, waypointIdx, killCount,
		travelDest));
}

void BotEngine::restoreSingleBotState(BotState& bot, std::shared_ptr<DBResult> result) {
	int64_t now = OTSYS_TIME();
	uint8_t aiState = result->getNumber<uint8_t>("ai_state");

	if (aiState == static_cast<uint8_t>(BotAIState::HUNTING)) {
		uint32_t scriptId = result->getNumber<uint32_t>("hunt_script_id");

		// Find and validate script
		const HuntScript* script = nullptr;
		for (const auto& s : huntScripts_) {
			if (s.id == scriptId && s.enabled) { script = &s; break; }
		}
		if (!script) {
			castLog(bot, fmt::format("RESTORE: Script {} gone/disabled, staying IDLE", scriptId));
			return;
		}

		// Player spawn-claim (B2): a bot deactivated mid-hunt persists its hunt to
		// bot_state_persistence and is restored here on its next scheduler reactivation
		// (a LIVE path, not just reload). Do not let it re-grab a spawn a player has claimed.
		if (isScriptPlayerClaimed(scriptId, script->spawnGroup)) {
			castLog(bot, fmt::format("RESTORE: Script {} player-claimed, staying IDLE", scriptId));
			return;
		}

		// Allow if unclaimed OR already reserved by THIS bot (our own deactivation reservation)
		auto reservedBy = activeHunts_.find(scriptId);
		if (reservedBy != activeHunts_.end() && reservedBy->second != bot.guid) {
			castLog(bot, fmt::format("RESTORE: Script {} claimed by bot {}, staying IDLE", scriptId, reservedBy->second));
			return;
		}

		// Saved waypoint index vs the script as it exists NOW. A waypoint-data edit renumbers a
		// leg, and this index was written against the OLD numbering — so it can point past the
		// end (read downstream as "phase complete", silently skipping content) or, when a leg was
		// reassembled from different segments, at a physically unrelated tile.
		//
		// This runs BEFORE the reservation below on purpose. Bailing out after activeHunts_ /
		// activeSpawnGroups_ are taken would leave the bot IDLE while permanently squatting the
		// spawn — nothing releases a reservation for a bot that never entered HUNTING (compare
		// abortHunt, which always pairs the two). Returning here lands in exactly the same shape
		// as the existing no-saved-state path: activateBot has already cleared hunt state and
		// primed nextRerollTime, so the bot simply picks a fresh hunt in 5-15s.
		const HuntPhase savedPhase = static_cast<HuntPhase>(result->getNumber<uint8_t>("hunt_phase"));
		const uint32_t savedWpIdx = result->getNumber<uint32_t>("waypoint_idx");
		{
			const std::vector<Waypoint>* savedLeg = nullptr;
			switch (savedPhase) {
				case HuntPhase::TRAVEL_TO:  savedLeg = &script->travelToWaypoints; break;
				case HuntPhase::PATROLLING: savedLeg = &script->patrolWaypoints; break;
				// LEAVING may have been walking a recovery route, which has no column in
				// bot_state_persistence — that distinction was already unrecoverable across
				// hibernation. travelFrom is the only leg we can check it against.
				case HuntPhase::LEAVING:    savedLeg = &script->travelFromWaypoints; break;
				default: break; // PREPARING / RESUPPLYING carry no waypoint index
			}
			if (savedLeg && savedWpIdx > savedLeg->size()) {
				castLog(bot, fmt::format(
					"RESTORE: Script {} phase {} saved wp {} exceeds leg size {} (waypoints changed) — staying IDLE",
					scriptId, static_cast<int>(savedPhase), savedWpIdx, savedLeg->size()));
				return;
			}
		}

		// Reserve (or confirm existing reservation)
		activeHunts_[scriptId] = bot.guid;
		if (!script->spawnGroup.empty()) {
			activeSpawnGroups_[script->spawnGroup] = bot.guid;
		}

		// Restore hunt fields
		bot.huntScriptId = scriptId;
		bot.huntTownId = script->townId;
		bot.huntPhase = savedPhase;
		bot.huntWaypointIdx = savedWpIdx;
		bot.huntKillCount = result->getNumber<uint32_t>("kill_count");
		bot.huntStartTime = now;
		// Quests get the quest budget here too. This writer was missed when the quest clock first
		// shipped, so a quest bot that hibernated and woke reverted to the ordinary ~25-minute
		// hunt clock and was cut off mid-walkthrough — on the DOMINANT (hibernated) path.
		bot.huntEndTime = botScriptIsQuest(script)
			? botQuestHuntEndTime(bot.huntStartTime)
			: now + static_cast<int64_t>(uniform_random(g_configManager().getNumber(BOT_HUNT_TIME_MIN_SEC), g_configManager().getNumber(BOT_HUNT_TIME_MAX_SEC))) * 1000LL;
		bot.huntTargetId = 0;
		bot.huntChaseFailCount = 0;
		bot.huntIgnoredMonsters.clear();
		bot.huntWaypointSkipCount = 0;
		bot.huntPatrolCycles = 0;
		bot.huntCooldownUntil = 0;
		bot.state = BotAIState::HUNTING;

		// Sync townId to actual physical position rather than script.townId. The hunt's
		// authoritative town is bot.huntTownId (set above). bot.townId is used for
		// navigation route lookups (findCityRoute) and must match currentPos — otherwise
		// a bot whose virtual-sim position drifted away from the script's town gets stuck
		// loading the wrong city's route on the first walk_to_boat. See §54.5.
		syncTownIdToPos(bot);

		castLog(bot, fmt::format("RESTORE: Hunt script {} phase={} wp={} kills={}",
			scriptId, static_cast<uint8_t>(bot.huntPhase), bot.huntWaypointIdx, bot.huntKillCount));

	} else if (aiState == static_cast<uint8_t>(BotAIState::TRAVELING)) {
		uint32_t destTownId = result->getNumber<uint32_t>("travel_dest_town_id");
		if (destTownId > 0) {
			bot.travelDestTownId = destTownId;
			bot.travelPhase = "walk_to_boat";
			bot.pendingHuntAfterTravel = false;
			bot.travelDestVerified = false;
			bot.state = BotAIState::TRAVELING;
			// Same reasoning as the HUNTING branch — the activateBot caller set townId from
			// player->getTown() (DB home), which can mismatch a mid-travel position.
			syncTownIdToPos(bot);
			castLog(bot, fmt::format("RESTORE: Traveling to town {}", destTownId));
		}
	}
	// All other states: leave at IDLE (activateBot already set this)
}

void BotEngine::saveAllStates() {
	auto& db = Database::getInstance();

	// Build a multi-row upsert (one query, fast on shutdown).
	// Save: active bots AND inactive bots holding a hunt reservation (spawn stays locked for them).
	std::string values;
	uint32_t savedCount = 0;

	for (const auto& bot : bots_) {
		// Skip bots that are both inactive and have no hunt reservation
		if (!bot.active && bot.huntScriptId == 0) continue;

		// Resolve name: live player if available, fall back to cached bot.name
		std::string escapedName;
		auto player = bot.getPlayer();
		if (player) {
			escapedName = db.escapeString(player->getName());
		} else if (!bot.name.empty()) {
			escapedName = db.escapeString(bot.name);
		} else {
			continue; // can't save without a name
		}

		uint8_t aiState = static_cast<uint8_t>(bot.state);
		// Normalize transient combat states to IDLE
		if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::PK_ATTACK ||
		    bot.state == BotAIState::FLEEING || bot.state == BotAIState::PARTY) {
			aiState = static_cast<uint8_t>(BotAIState::IDLE);
		}
		// Inactive bot holding a hunt reservation — save as HUNTING for restore
		if (!bot.active && bot.huntScriptId > 0) {
			aiState = static_cast<uint8_t>(BotAIState::HUNTING);
		}

		// Hunt-specific fields
		std::string huntScriptId = "NULL";
		std::string huntPhase = "NULL";
		std::string waypointIdx = "NULL";
		std::string killCount = "NULL";
		if (aiState == static_cast<uint8_t>(BotAIState::HUNTING)) {
			huntScriptId = std::to_string(bot.huntScriptId);
			huntPhase = std::to_string(static_cast<uint8_t>(bot.huntPhase));
			waypointIdx = std::to_string(bot.huntWaypointIdx);
			killCount = std::to_string(bot.huntKillCount);
		}

		// Travel-specific fields
		std::string travelDest = "NULL";
		if (bot.state == BotAIState::TRAVELING) {
			travelDest = std::to_string(bot.travelDestTownId);
		}

		if (!values.empty()) values += ',';
		values += fmt::format("({},{},{},{},{},{},{},{})",
			bot.guid, escapedName, aiState,
			huntScriptId, huntPhase, waypointIdx, killCount,
			travelDest);
		savedCount++;
	}

	if (savedCount == 0) {
		g_logger().info("[BotEngine] saveAllStates: no bots to save");
		return;
	}

	bool ok = db.executeQuery(
		"INSERT INTO `bot_state_persistence` "
		"(`guid`, `name`, `ai_state`, `hunt_script_id`, `hunt_phase`, `waypoint_idx`, `kill_count`, "
		"`travel_dest_town_id`) "
		"VALUES " + values +
		" ON DUPLICATE KEY UPDATE "
		"`ai_state`=VALUES(`ai_state`), `hunt_script_id`=VALUES(`hunt_script_id`), "
		"`hunt_phase`=VALUES(`hunt_phase`), `waypoint_idx`=VALUES(`waypoint_idx`), "
		"`kill_count`=VALUES(`kill_count`), `travel_dest_town_id`=VALUES(`travel_dest_town_id`)"
	);

	if (ok) {
		g_logger().info("[BotEngine] saveAllStates: saved {} bot states", savedCount);
	} else {
		g_logger().error("[BotEngine] saveAllStates: DB query failed");
	}
}

void BotEngine::restoreAllStates() {
	auto& db = Database::getInstance();
	auto result = db.storeQuery(
		"SELECT `guid`, `ai_state`, `hunt_script_id`, `hunt_phase`, `waypoint_idx`, `kill_count`, "
		"`travel_dest_town_id` FROM `bot_state_persistence`");

	if (!result) {
		g_logger().info("[BotEngine] restoreAllStates: no persisted states found");
		return;
	}

	uint32_t restoredCount = 0;

	do {
		uint32_t guid = result->getNumber<uint32_t>("guid");
		auto it = guidToIndex_.find(guid);
		if (it == guidToIndex_.end()) continue; // bot not loaded

		auto& bot = bots_[it->second];

		// Activate bot (equip, cast broadcasting, spells)
		if (!bot.active) {
			activateBot(guid);
			// Re-fetch after activation (no resize risk — bot is already registered)
		}

		auto player = bot.getPlayer();
		if (!player) continue;

		// Position is already correct — bots load at their saved loginPosition from loadBotPlayer.
		// No teleport needed; just sync the in-memory position from the live player.
		bot.currentPos = player->getPosition();
		bot.lastPos = bot.currentPos;

		// Restore AI state using shared helper (hunt, travel, or stays IDLE)
		restoreSingleBotState(bot, result);

		// Bot has a restored task — no 1-min activation fallback needed
		bot.activatedAt = 0;

		restoredCount++;
	} while (result->next());

	// Clear persisted rows now that restore is done
	clearPersistedStates();

	g_logger().info("[BotEngine] restoreAllStates: restored {} bot states", restoredCount);
}

// ============================================================================
// Shared library factory functions — called by BotEngineLoader via dlsym
// ============================================================================

extern "C" {
	IBotEngine* createBotEngine() {
		// A fresh engine starts with an empty s_probeBots, but the probe flag itself lives on
		// the Player, which SURVIVES a reload. Clearing it is therefore registerBot's job, not
		// the factory's -- see the setSyntheticCastViewers(0) there.
		return new BotEngine();
	}

	void destroyBotEngine(IBotEngine* engine) {
		delete engine;
	}
}




