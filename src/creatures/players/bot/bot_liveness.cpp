/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_liveness.cpp — "makes bots feel alive" behaviors (Phase 11 module split).
//
// Second module carved out of bot_engine.cpp. Contents: the botStartAutoWalk
// wrapper (observed mid-walk pauses) and tickLivenessBehaviors (turn-in-place,
// fidget item drops, PZ roaming, mounts, and the other ambient motions).
//
// This became a clean carve-out only after the shared state it touched
// (botCacheStats_, walkPauseInfo_) was promoted to BotEngine members — a
// file-scope static moved into a shared header would have given each TU its own
// copy and silently forked the state.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#pragma GCC diagnostic ignored "-Wunused-function"


// ============================================================================
// BOT_LIVENESS_PACK Phase C.5: botStartAutoWalk wrapper
// ============================================================================
//
// Replaces every direct botStartAutoWalk(bot, player,dirs) call site in bot_engine.cpp.
// Decides whether to inject a pause before issuing the walk. Two tiers:
//   - Unobserved (nobody watching): the original short/modest pause — low chance,
//     400-5000ms, cap botWalkPauseMaxPerRoute. No personality gate.
//   - Observed (a real player or cast-watched bot is on screen): longer (up to
//     botWalkPauseObservedMaxMs, default 20s), more frequent, gated by each bot's
//     walkBias personality trait so ~bottom-quartile bots never do it and the rest
//     scale up — some bots reliably pause near players, others ignore it.
//
// State allowlist: pauses fire ONLY in IDLE / DWELLING / TRAVELING. Never during
// HUNTING (covers hunt patrolling), PARTY support, combat / PK / flee / mid-FC, or
// with a live attacker / hunt target. Falls through to immediate startAutoWalk when
// no pause is scheduled.
//
// Reload safety: the in-flight pause is a dispatcher callback whose code lives in
// libbot_engine.so. We remember its event id (pendingWalkPauseEventId) so
// deactivateAll() can stopEvent() it before destroyBotEngine() + dlclose() on
// /cavebot reload — otherwise the callback would fire into unloaded code. Only one
// pause is in flight per bot at a time (bounds dispatcher load + avoids racing a
// stale replay against a fresh walk).
void BotEngine::botStartAutoWalk(BotState& bot, const std::shared_ptr<Player>& player,
                                  const std::vector<Direction>& dirs) {
	if (!player || dirs.empty()) {
		// !!! DO NOT REPLACE this player->startAutoWalk(dirs) with botStartAutoWalk(...).
		// !!! It IS the unwrapping target; recursing back into the wrapper causes an
		// !!! infinite loop (commit cd7c3e94e). If a future edit needs to convert
		// !!! `player->startAutoWalk(` -> `botStartAutoWalk(`, skip THIS line and the
		// !!! one near the bottom of this function.
		if (player) player->startAutoWalk(dirs);
		return;
	}

	// State allowlist + hard safety gates. Pausing is town/travel flavor only.
	const bool stateAllowsPause = (bot.state == BotAIState::IDLE
	                               || bot.state == BotAIState::DWELLING
	                               || bot.state == BotAIState::TRAVELING);
	const bool unsafe = !stateAllowsPause
	                    || bot.attackerId != 0 || bot.huntTargetId != 0
	                    || bot.fcState != FloorChangeState::NONE;

	if (unsafe) {
		// Something urgent took over (combat/chase/hunt/FC) — cancel any leisurely pause
		// still pending and issue the walk immediately so responsiveness isn't blocked.
		if (bot.pendingWalkPauseEventId != 0) {
			g_dispatcher().stopEvent(bot.pendingWalkPauseEventId);
			bot.pendingWalkPauseEventId = 0;
			walkPauseInfo_.erase(bot.guid);
		}
		// !!! DO NOT REPLACE with botStartAutoWalk(...) — see top-of-function comment.
		player->startAutoWalk(dirs);
		return;
	}

	// A pause is already in flight: let it resolve (it will replay or drop + re-issue
	// next tick). Don't walk now (would race the replay) and don't stack a 2nd event.
	if (bot.pendingWalkPauseEventId != 0) return;

	const auto& cfg = livenessCfg_;

	// Decide a pause duration (-1 = no pause). Observed tier is tried first; its cheap
	// gates (personality, cap, chance roll) short-circuit BEFORE the spectator scan so
	// we don't scan on every walk step at scale.
	int32_t pauseMs = -1;
	const char* pauseTier = "obs";  // which tier produced pauseMs (for cast-chat debug line)

	const uint8_t wb = bot.walkBias();  // 0-15 personality "pausiness"
	const int32_t obsChanceBase = cfg.walkPauseObservedChancePct;
	if (obsChanceBase > 0 && wb > 3
	    && static_cast<int32_t>(bot.pausesThisRouteObserved) < cfg.walkPauseObservedMaxPerRoute) {
		const int32_t obsChance = obsChanceBase * static_cast<int32_t>(wb) / 15;
		if (obsChance > 0 && uniform_random(1, 100) <= obsChance
		    && botWalkObserved(bot, player)) {
			const int32_t minMs = cfg.walkPauseObservedMinMs;
			const int32_t maxMs = cfg.walkPauseObservedMaxMs;
			pauseMs = uniform_random(std::max(1, minMs), std::max(minMs, maxMs));
			bot.pausesThisRouteObserved++;
		}
	}

	// Unobserved tier (also the fallback when an observed pause didn't roll). Unchanged
	// from the original behavior: short, modest, no personality gate.
	if (pauseMs < 0) {
		const int32_t pauseChance = cfg.walkPauseChancePct;
		if (pauseChance > 0
		    && static_cast<int32_t>(bot.pausesThisRoute) < cfg.walkPauseMaxPerRoute
		    && uniform_random(1, 100) <= pauseChance) {
			const int32_t minMs = cfg.walkPauseMinMs;
			const int32_t maxMs = cfg.walkPauseMaxMs;
			pauseMs = uniform_random(std::max(1, minMs), std::max(minMs, maxMs));
			bot.pausesThisRoute++;
			pauseTier = "unobs";
		}
	}

	if (pauseMs > 0) {
		const uint32_t guid = bot.guid;
		// Record start+duration for the admin-look status line (buildStatusDetail) and
		// emit a one-shot debug line into Cast Chat (verboseLog-gated). Erased everywhere
		// pendingWalkPauseEventId returns to 0.
		walkPauseInfo_[guid] = { OTSYS_TIME(), pauseMs };
		castLog(bot, fmt::format("PAUSE {} {:.1f}s mid-route (wb={})",
			pauseTier, pauseMs / 1000.0, wb));
		const Position capturedPos = bot.currentPos;
		const BotAIState capturedState = bot.state;
		auto dirsCopy = dirs;
		const uint64_t evId = g_dispatcher().scheduleEvent(pauseMs,
			[this, guid, capturedPos, capturedState, dirsCopy = std::move(dirsCopy)]() {
				auto it = guidToIndex_.find(guid);
				if (it == guidToIndex_.end()) return;
				BotState& b = bots_[it->second];
				b.pendingWalkPauseEventId = 0;  // fired — no longer cancellable
				walkPauseInfo_.erase(guid);    // pause window over (walk-issued or stale-dropped)
				if (b.hibernated) return;
				auto p = b.getPlayer();
				if (!p) return;
				// Drop the (now possibly stale) replay if combat began, the bot left a
				// pause-eligible state, or it moved during the pause. The think loop will
				// issue a fresh path next tick.
				if (b.attackerId != 0 || b.huntTargetId != 0
				    || b.state != capturedState
				    || b.currentPos != capturedPos) return;
				p->startAutoWalk(dirsCopy);
			}, "botStartAutoWalk_pause");
		bot.pendingWalkPauseEventId = evId;
		return;
	}
	// !!! DO NOT REPLACE this player->startAutoWalk(dirs) with botStartAutoWalk(...).
	// !!! See the matching comment at the top of this function. Recursing here pegged
	// !!! the dispatcher at 100% CPU (commit cd7c3e94e).
	player->startAutoWalk(dirs);
}

// Observed-tier gate for the mid-walk pause. Mirrors the chat observer gate: cheap
// server-global early-out when there are no anchors (no real players online + no
// cast-watched bots), then a viewport Spectators::find<Player> scan for a non-bot
// player or a cast-watched bot on screen. Result cached ~500ms per bot so we don't
// scan on every walk step across ~200 bots.
bool BotEngine::botWalkObserved(BotState& bot, const std::shared_ptr<Player>& player) {
	const int64_t now = OTSYS_TIME();
	if (now - bot.walkObservedCacheMs < 500) { botCacheHit(BotCacheId::WalkObserved); return bot.walkObservedCache; }
	const uint64_t obsStartUs = botMonoUs(); // Phase 5 instrumentation
	bot.walkObservedCacheMs = now;

	bool observed = false;
	if (!currentAnchors_.empty()) {
		auto spectators = Spectators().find<Player>(player->getPosition(), true);
		for (const auto& s : spectators) {
			auto other = s ? s->getPlayer() : nullptr;
			if (!other || other->getID() == player->getID()) continue;
			if (!other->isBotPlayer()) { observed = true; break; }       // real player on screen
			if (other->getCastViewerCount() > 0) { observed = true; break; } // cast-watched bot
		}
	}
	bot.walkObservedCache = observed;
	botCacheMiss(BotCacheId::WalkObserved, botMonoUs() - obsStartUs);
	return observed;
}

// ============================================================================
// BOT_LIVENESS_PACK Phase C: tickLivenessBehaviors
// ============================================================================
//
// Per-tick liveness behaviors for awake bots in IDLE/DWELLING. Single entry
// point keeps the call sites in doIdle/doDwelling to one line each. Each
// sub-behavior is independently gated so adding/removing one doesn't ripple.
//
// Hard gates (top of the function): combat, mid-walk, FC machine, dummy
// training, hibernated. Anything that requires the bot's full attention.
// Per-behavior gates check personalitySeed traits + per-bot timers.
void BotEngine::tickLivenessBehaviors(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;
	if (bot.hibernated) return;

	// Hard gates — never fire these during real activity.
	if (bot.attackerId != 0 || bot.huntTargetId != 0) return;
	if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::PK_ATTACK
	    || bot.state == BotAIState::FLEEING) return;
	if (bot.fcState != FloorChangeState::NONE) return;
	if (!player->listWalkDir.empty()) return;
	// Dummy training: turning cancels the Lua training loop. Skip.
	if (bot.advStoneDwellMode == 2 && bot.advStoneTrainingActive) return;

	const int64_t now = OTSYS_TIME();
	const auto& cfg = livenessCfg_;  // cached config (refreshed per-tick by tick()) — see refreshLivenessCfgIfStale

	// --- Turn-in-place (Phase C.4) ---
	// Uses Game::internalCreatureTurn (same path as spell-cast facing). Broadcasts
	// via Spectators().find<Player>() so cast viewers see it.
	const int32_t turnIntervalTicks = cfg.turnInPlaceIntervalTicks;
	if (turnIntervalTicks > 0 && bot.tickCounter > 0
	    && static_cast<int32_t>(bot.tickCounter % turnIntervalTicks) == 0
	    && now >= bot.nextTurnInPlaceTime) {
		const int32_t turnChance = cfg.turnInPlaceChancePct;
		if (uniform_random(1, 100) <= turnChance) {
			static const Direction kDirs[] = {
				DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
			};
			Direction newDir = kDirs[uniform_random(0, 3)];
			if (newDir != player->getDirection()) {
				g_game().internalCreatureTurn(player, newDir);
			}
			// Schedule next eligible turn (lightweight cooldown to avoid back-to-back).
			bot.nextTurnInPlaceTime = now + uniform_random(3000, 10000);
		}
	}

	// --- World Chat post (Phase C.8 / v2 Phase D) ---
	// worldChatMode "channel" (legacy) keeps the old channel-3 posting. In
	// "local" (default) and "off" modes bots never post to World Chat; the
	// banter corpus surfaces through the local slot below instead. The timer is
	// deliberately left untouched in local/off mode so flipping the mode back
	// to "channel" via /cavebot reload resumes cleanly.
	if (chatCfg_.worldChatMode == 2 && now >= bot.nextWorldChatTime) {
		tryEmitChat(bot, player, "banter", /*channelId=*/3);
		const int32_t minMs = cfg.worldChatIntervalMinMs;
		const int32_t maxMs = cfg.worldChatIntervalMaxMs;
		bot.nextWorldChatTime = now + uniform_random(std::max(1, minMs), std::max(minMs, maxMs));
	}

	// --- Advertising post (Phase C.8) ---
	if (now >= bot.nextAdvertisingTime) {
		tryEmitChat(bot, player, "advertising", /*channelId=*/5);
		const int32_t minMs = cfg.advertisingIntervalMinMs;
		const int32_t maxMs = cfg.advertisingIntervalMaxMs;
		bot.nextAdvertisingTime = now + uniform_random(std::max(1, minMs), std::max(minMs, maxMs));
	}

	// --- Background local chatter (Phase C.2 / v2 Phase D) ---
	// The local slot now splits between banter (ex-world-chat content, spoken
	// near a real player thanks to the observer gate) and plain idle filler.
	// banterSharePct picks which bucket this attempt tries; the bucket's own
	// chance + cooldown + observer gate + per-player throttle still apply
	// inside tryEmitChat.
	if (now >= bot.lastChatTimeMs + cfg.chatCooldownMinMs) {
		const bool banterEnabled = chatCfg_.worldChatMode == 1;  // "local" mode only;
		// "channel" keeps banter on ch3, "off" silences banter content entirely
		const char* localCategory = (banterEnabled
			&& uniform_random(1, 100) <= chatCfg_.banterSharePct) ? "banter" : "idle";
		tryEmitChat(bot, player, localCategory, /*channelId=*/0);
	}

	// NOTE: the idle litter drop lives in maybeFidgetDrop(), called directly from
	// doIdle + doDwelling (not here) so its stop-tracking sees the walking/errand
	// transitions that this function's early-return gates would otherwise hide.
}

// Mount lifecycle.
//
// The pre-existing design rolled botMountChancePct once, inline in activateBot, and called
// toggleMount(true) on the spot. Two things made that roll mostly inert:
//
//   1. Player::toggleMount(true) returns false inside a protection zone (player.cpp, guarded by
//      toggleMountInProtectionZone which is false on this server; bots are group_id 1 so the
//      `group->access` escape does not apply). Bots reconnect at their DB login position, and
//      ~40% of them sit exactly on their town temple tile with most of the rest elsewhere in
//      town — all PZ. So the roll silently no-opped for the majority.
//   2. There was no second attempt. Stock canary only re-mounts on PZ exit when `wasMounted` is
//      set, and that is set only when an ALREADY-mounted player walks INTO a PZ. A bot whose
//      roll failed in town therefore stayed unmounted for its entire session.
//
// So the roll now records an *intent* that survives the failure, and tryOpportunisticMount
// re-asserts it once the bot is somewhere it can actually mount. `wasMounted` is private with no
// accessor, so this path deliberately reads only isMounted(); the two are complementary rather
// than competing — stock's PZ-exit re-mount is event-driven and instant, this is the periodic
// safety net, and whichever fires first turns the other into a no-op.
void BotEngine::rollMountForReconnect(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!player) {
		return;
	}
	const int64_t now = OTSYS_TIME();

	auto it = botMountWants_.find(bot.guid);
	if (it != botMountWants_.end() && (now - it->second.lastRollMs) < MOUNT_REROLL_MIN_INTERVAL_MS) {
		// Churn wake, not a new session. Re-rolling here would dismount and re-model a bot a
		// player may be looking at right now — the proximity-wake path is exactly the one that
		// fires near observers. Keep the existing intent and just re-assert it, which still
		// rescues a bot that wanted a mount but was PZ-blocked on its last attempt.
		tryOpportunisticMount(bot, player);
		return;
	}

	// Genuinely new session — full re-roll, so a bot is neither always mounted nor always on
	// the same model across reconnects.
	if (player->isMounted()) {
		// hibernateBot pools the Player as-is without dismounting, so a wake can hand back a
		// still-mounted bot; without this the re-roll would be invisible for the dominant path.
		// Note this is dismount() and not toggleMount(false): the latter stamps lastToggleMount,
		// and the toggleMount(true) below would then trip the 3s exhaustion guard and fail.
		player->dismount();
		// dismount() only clears defaultOutfit.lookMount (and the mount speed bonus). The
		// render-facing copy is currentOutfit, written by exactly one function in the codebase —
		// internalCreatureChangeOutfit — and ProtocolGame::AddCreature puts currentOutfit on the
		// wire for EVERY render, known creature or not. So an unpaired dismount leaves the bot
		// unmounted to the rules and mounted to every client, permanently: isMounted() is now
		// false, which is exactly the gate onChangeZone's PZ-entry dismount tests, so walking
		// into a temple no longer corrects the sprite. Every other dismount() site in the tree
		// (onChangeZone, death, toggleMount(false), untameMount, ConditionOutfit) pairs the two;
		// this was the only one that did not. Pushing the outfit rather than switching to
		// toggleMount(false) keeps the original reason for calling dismount() directly —
		// internalCreatureChangeOutfit does not stamp lastToggleMount, so the toggleMount(true)
		// below still clears the 3s exhaustion guard.
		g_game().internalCreatureChangeOutfit(player, player->getDefaultOutfit());
	}

	// livenessCfg_ is the per-tick cache — a live g_configManager().getNumber() here is what
	// commit 30a503cc5 removed from the per-bot paths to fix dispatcher saturation.
	const bool wants = uniform_random(1, 100) <= livenessCfg_.mountChancePct;
	const uint8_t mountId = wants ? player->getRandomMountId() : 0;
	const bool finalWants = wants && mountId > 0;
	botMountWants_[bot.guid] = { finalWants, mountId, now };

	if (finalWants) {
		player->setCurrentMount(mountId);
		// May well fail if we are standing in a temple — that is expected, and
		// tryOpportunisticMount picks it up once the bot walks out.
		player->toggleMount(true);
	}
}

void BotEngine::tryOpportunisticMount(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!player || player->isMounted()) {
		return;
	}
	auto it = botMountWants_.find(bot.guid);
	if (it == botMountWants_.end() || !it->second.wants) {
		return;
	}
	// toggleMount reads getLastMount(), which prefers PSTRG_MOUNTS_CURRENTMOUNT — the value
	// setCurrentMount stored at roll time — so this re-asserts the same model, not a new one.
	player->toggleMount(true);
}

// BOT_LIVENESS_PACK: idle litter drop (drop-only, once per awake session).
//
// Fires when a bot has been standing still for >=1s in IDLE/DWELLING with no errand
// and no combat/PvP/hunt/FC/dummy-training. Rolls a chance (botFidgetChancePct, scaled
// by per-bot fidgetiness and clamped to 95%) to drop ONE cheap NPC-buyable item on the
// current tile. The bot never picks it back up — the item is left on the ground.
//
// Stop-tracking: any "busy" condition (walking, walk target, city route, depot run,
// combat, FC, ...) resets the stationary clock so the NEXT genuine stop rolls fresh.
// fidgetRolledThisStop latches one roll per continuous stop; fidgetDroppedThisWake
// caps it at one actual drop per awake session (reset in wakeBot/registerBot).
void BotEngine::maybeFidgetDrop(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) {
		return;
	}

	// Never litter from INSIDE a house. The candidate loop below already refuses a drop TILE that
	// belongs to a house, which covers the bot's own sqm — but the throw box is 5x4, so a bot
	// standing indoors can still pitch an item through the doorway onto public ground. Bots reach
	// house interiors from two directions now: the depot dwell's PZ roam, whose tile search has no
	// house filter, and the house-visit activity itself.
	//
	// Deliberately ahead of the stationary gate rather than folded into it: this is "where the bot
	// is", not "is the bot busy", and resetting the fidget clock for it would be wrong — a bot that
	// idles indoors and then steps out should still be eligible on the far side of the door.
	if (auto here = g_game().map.getTile(bot.currentPos); here && here->getHouse() != nullptr) {
		return;
	}

	// Not an idle stop — reset the clock and bail. Covers hibernation, combat/PvP/flee,
	// in-progress floor change, dummy training, active walking, and any pending errand.
	if (bot.hibernated
	    || bot.attackerId != 0 || bot.huntTargetId != 0
	    || bot.state == BotAIState::COMBAT || bot.state == BotAIState::PK_ATTACK
	    || bot.state == BotAIState::FLEEING
	    || bot.fcState != FloorChangeState::NONE
	    || (bot.advStoneDwellMode == 2 && bot.advStoneTrainingActive)
	    || !player->listWalkDir.empty()
	    || bot.hasWalkTarget || bot.followingCityRoute || bot.hasDepotTarget) {
		bot.fidgetStationarySince = 0;
		bot.fidgetRolledThisStop = false;
		return;
	}

	if (safeFidgetItemIds_.empty()) {
		return;
	}

	const int64_t now = OTSYS_TIME();
	if (bot.fidgetStationarySince == 0) {
		bot.fidgetStationarySince = now;
	}
	// Already dropped this wake, or already rolled for this continuous stop.
	if (bot.fidgetDroppedThisWake || bot.fidgetRolledThisStop) {
		return;
	}
	// Require >=1s of continuous standing-still before the roll.
	if (now - bot.fidgetStationarySince < 1000) {
		return;
	}

	// One roll per stop, regardless of outcome.
	bot.fidgetRolledThisStop = true;

	const int32_t baseChance = livenessCfg_.fidgetChancePct;
	int32_t scaledChance = (baseChance * (4 + bot.fidgetiness())) / 8;
	if (scaledChance > 95) scaledChance = 95;
	if (scaledChance < 0) scaledChance = 0;
	if (uniform_random(1, 100) > scaledChance) {
		return;
	}

	// Pick a random nearby tile to drop on — any reachable + throwable sqm within range
	// (the bot's OWN sqm included), so litter spreads naturally instead of always landing
	// underfoot. Sample up to kMaxTries random offsets; accept the first valid candidate.
	const Position botPos = player->getPosition();
	static constexpr int32_t kRangeX = 5;   // inside the 8x6 throw viewport (canThrowObjectTo)
	static constexpr int32_t kRangeY = 4;
	static constexpr int32_t kMaxTries = 12;
	std::shared_ptr<Tile> dropTile;
	for (int32_t attempt = 0; attempt < kMaxTries; ++attempt) {
		int32_t dx = uniform_random(-kRangeX, kRangeX);
		int32_t dy = uniform_random(-kRangeY, kRangeY);
		Position cand(static_cast<uint16_t>(botPos.x + dx),
		              static_cast<uint16_t>(botPos.y + dy), botPos.z);
		// canWalkTo: reachable floor tile (also rejects void/no-ground, floor-change and
		// teleport tiles via the bot's creature queryAdd). Out-of-map offsets wrap to a huge
		// uint16 -> getTile null -> skip. Returns the bot's own tile directly when cand==botPos.
		auto tile = g_game().map.canWalkTo(player, cand);
		if (!tile) {
			continue;
		}
		// Never litter in houses, nor onto tiles that would eat/relocate the item while
		// internalAddItem still reports success: trashholder destroys it, mailbox consumes it,
		// teleport/floor-change move it elsewhere. (FC/teleport are also covered by canWalkTo;
		// kept here as defense-in-depth in case that filter ever changes.)
		if (tile->getHouse() != nullptr) {
			continue;
		}
		if (tile->hasFlag(TILESTATE_TRASHHOLDER | TILESTATE_MAILBOX
		                  | TILESTATE_TELEPORT | TILESTATE_FLOORCHANGE)) {
			continue;
		}
		// One drop per sqm (skip tiles that already hold a loose ground item), and respect the
		// client stack-depth cap (FLAG_NOLIMIT below bypasses the engine's own queryAdd check).
		if (tile->getDownItemCount() > 0 || tile->getItemCount() > 9) {
			continue;
		}
		// Throwable/shootable: within throw range + clear line of sight from the bot.
		if (!g_game().map.canThrowObjectTo(botPos, cand)) {
			continue;
		}
		dropTile = tile;
		break;
	}
	if (!dropTile) {
		return;  // no valid spot this stop — keep the once-per-wake budget for a later stop
	}

	uint16_t itemId = safeFidgetItemIds_[uniform_random(
		0, static_cast<int32_t>(safeFidgetItemIds_.size()) - 1)];
	auto item = Item::CreateItem(itemId, 1);
	if (!item) {
		return;
	}
	if (g_game().internalAddItem(dropTile, item, INDEX_WHEREEVER, FLAG_NOLIMIT) == RETURNVALUE_NOERROR) {
		bot.fidgetDroppedThisWake = true;
	}
}

// Pre-selects the dwell sub-activity at trip start. Equal-chance roll across 3 modes;
// modes 1/2 can demote to 0 (route waypoint) when no free tile is available.
//
// Gating: chest/dummy are at z=7, but the route's idle-waypoint pool includes z=6 tiles.
// If the chosen idle waypoint is NOT on z=7, the walk-to-target would have to cross
// floors mid-phase-1 (which goTo refuses while followingCityRoute is set), so we force
// mode 0 in that case. Without this gate the bot would dwell at the wrong floor and
// the 30s deadline would fire, demoting awkwardly to mode-0 at the idle-wp position.
void BotEngine::selectAdvStoneSubActivity(BotState& bot) {
	bot.advStoneDwellMode = 0;
	bot.advStoneDwellWeaponId = 0;
	bot.advStoneTrainingActive = false;
	bot.advStoneDwellTarget = Position();

	// Z-gate: only allow sub-activity when idle waypoint is on z=7 (same floor as chest/dummy).
	bool idleZIsTargetZ = bot.advStoneIdleAt < adventurerStoneRoute_.size()
		&& adventurerStoneRoute_[bot.advStoneIdleAt].pos.z == 7;

	// Exclude tiles already chosen as advStoneDwellTarget by ANY other active Adv Stone
	// trip. Without this, two bots whose trip-starts fire in close succession can each
	// see the same tile as "free" via isFreeWalkableTile (point-in-time check) and pick
	// it, then both walk there and stack (Adv Stone island is PZ → FLAG_IGNOREBLOCKCREATURE
	// allows stacking). The reservation is bot.advStoneDwellTarget which is set right
	// here at trip start, cleared on trip end / dwell expire / demote-on-deadline.
	auto filterClaimedByOtherBots = [&](std::vector<Position>& cands) {
		std::vector<Position> filtered;
		filtered.reserve(cands.size());
		for (const auto& p : cands) {
			bool claimed = false;
			for (const auto& other : bots_) {
				if (other.guid == bot.guid) continue;
				if (!other.advStoneActive) continue;
				if (other.advStoneDwellTarget == p) { claimed = true; break; }
			}
			if (!claimed) filtered.push_back(p);
		}
		cands = std::move(filtered);
	};

	auto trySetChest = [&]() -> bool {
		auto cands = collectAdjacentFree(kAdvStoneRewardChest);
		filterClaimedByOtherBots(cands);
		if (cands.empty()) return false;
		bot.advStoneDwellTarget = cands[uniform_random(0, static_cast<int32_t>(cands.size()) - 1)];
		bot.advStoneDwellMode = 1;
		return true;
	};
	auto trySetDummy = [&]() -> bool {
		uint16_t weaponId;
		if (s_forceAdvStoneNextWeapon != 0) {
			weaponId = s_forceAdvStoneNextWeapon;
			s_forceAdvStoneNextWeapon = 0; // consume
		} else {
			weaponId = kLastingExerciseIds[uniform_random(0,
				static_cast<int32_t>(std::size(kLastingExerciseIds)) - 1)];
		}
		std::vector<Position> cands;
		if (isMeleeExerciseWeapon(weaponId)) {
			cands = collectAdjacentFree(kAdvStoneExerciseDummy);
		} else {
			// Ranged: radius-5 same-z scan with LOS pruning. Mirrors the bot's combat
			// targeting check (g_game().map.isSightClear) so the distance shot can land.
			cands = collectFreeWithLOS(kAdvStoneExerciseDummy, 5);
			// Bias toward true-distance candidates (Chebyshev >= kRangedDummyMinDist).
			// Without this, adjacent LOS tiles are uniformly selected with the same
			// probability as far tiles — a bot with a bow standing next to the dummy
			// looks cosmetically wrong. Fall back to ALL candidates if no distance tile
			// is available (cramped dungeon layout) so the trip still completes.
			std::vector<Position> rangedCands;
			rangedCands.reserve(cands.size());
			for (const auto& p : cands) {
				int32_t dx = std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(kAdvStoneExerciseDummy.x));
				int32_t dy = std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(kAdvStoneExerciseDummy.y));
				if (std::max(dx, dy) >= kRangedDummyMinDist) {
					rangedCands.push_back(p);
				}
			}
			if (!rangedCands.empty()) {
				cands = std::move(rangedCands);
			}
		}
		// Inter-bot dedup: skip tiles already targeted by other active Adv Stone trips
		// to prevent visual stacking on the dummy adj tile.
		filterClaimedByOtherBots(cands);
		if (cands.empty()) return false;
		bot.advStoneDwellTarget = cands[uniform_random(0, static_cast<int32_t>(cands.size()) - 1)];
		bot.advStoneDwellWeaponId = weaponId;
		bot.advStoneDwellMode = 2;
		return true;
	};

	// One-shot force override (set via `advstone <chest|dummy|waypoint>` bot command).
	// Forces the next trip to a specific mode, bypassing the z-gate, the random roll
	// AND the chest/dummy concurrency cap (deterministic debug tool).
	if (s_forceAdvStoneNextMode != 0) {
		uint8_t forced = s_forceAdvStoneNextMode;
		s_forceAdvStoneNextMode = 0; // consume
		if (forced == 1 && trySetChest()) return;
		if (forced == 2 && trySetDummy()) return;
		// forced==3 (waypoint) or candidate scan empty → fall through to mode 0
		bot.advStoneDwellMode = 0;
		return;
	}

	// Chest/dummy concurrency cap: at most botAdvStoneChestDummyCapPct% of
	// botPlayersOnline (truncated; 2.5% of 500 -> 12) may hold a chest (mode 1)
	// or dummy (mode 2) sub-activity at once. Live AND hibernated trips both
	// roll here, so counting advStoneActive trips with mode 1/2 covers both;
	// the slot is held from this roll until dwell end / demote / trip end.
	// Capped trips stay mode 0 — the bot still tours the island and idles at a
	// route waypoint, so the island remains lively.
	const uint32_t chestDummyCap = pctOfBotTotal(BOT_ADV_STONE_CHEST_DUMMY_CAP_PCT);
	uint32_t chestDummyBusy = 0;
	for (const auto& other : bots_) {
		if (other.guid == bot.guid) continue;
		if (other.advStoneActive && (other.advStoneDwellMode == 1 || other.advStoneDwellMode == 2)) {
			chestDummyBusy++;
		}
	}
	if (chestDummyBusy >= chestDummyCap) {
		g_logger().info("[ADVSTONE] chest/dummy cap reached ({}/{}) guid={} -> waypoint mode",
			chestDummyBusy, chestDummyCap, bot.guid);
		return; // advStoneDwellMode already reset to 0 at function entry
	}

	// BOT_LIVENESS_PACK Phase A.5: if the rolled idle waypoint isn't on z=7, re-pick
	// from the z=7-only subset BEFORE deciding mode. Previously this would force mode 0,
	// which (combined with ~⅔ of the route being on z=6) is why bots almost never train
	// at the dummy or visit the chest. Re-pick preserves the original commit 24d2eef85
	// invariant (no goTo across floors mid-phase-1) without nuking mode 1/2 chances.
	if (!idleZIsTargetZ) {
		uint16_t z7Idle = pickAdventurerStoneIdleIdx(/*requireZ=*/7);
		if (z7Idle != 0) {
			bot.advStoneIdleAt = z7Idle;
			idleZIsTargetZ = true;
		} else {
			// No z=7 NODE candidates exist (route reshape?) — fall back to mode 0.
			bot.advStoneDwellMode = 0;
			return;
		}
	}

	int32_t initial = uniform_random(0, 2);
	for (int step = 0; step < 3; ++step) {
		uint8_t mode = static_cast<uint8_t>((initial + step) % 3);
		if (mode == 0) {
			bot.advStoneDwellMode = 0;
			return;
		}
		if (mode == 1 && trySetChest()) return;
		if (mode == 2 && trySetDummy()) return;
	}
}

// Terminates an active Lua exercise training loop if one is running.
// The Lua `exerciseTrainingEvent` self-terminates on next iteration once `player:isTraining()` returns 0.
void BotEngine::stopAdvStoneTrainingIfActive(BotState& bot) {
	if (!bot.advStoneTrainingActive) return;
	if (auto player = bot.getPlayer()) {
		player->setTraining(false);
	}
	bot.advStoneTrainingActive = false;
}

bool BotEngine::startAdventurerStoneTrip(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return false;
	if (adventurerStoneRoute_.empty()) {
		castLog(bot, "ADVSTONE: trip aborted (route not loaded)");
		return false;
	}

	auto pos = player->getPosition();
	auto tile = g_game().map.getTile(pos);
	// Mirror adventurers_stone.lua onUse() guards exactly: PZ + not house + not pz-locked.
	// (TILESTATE_HOUSE is a Lua-only constant; C++ exposes house-ness via getHouse() != nullptr.)
	if (!tile || !tile->hasFlag(TILESTATE_PROTECTIONZONE) || tile->getHouse() != nullptr
			|| player->isPzLocked()) {
		castLog(bot, "ADVSTONE: trip aborted (not in temple PZ or pz-locked)");
		return false;
	}

	uint32_t townIdAtTemple = findAdventurerStoneTownAt(pos);
	if (townIdAtTemple == 0) {
		castLog(bot, fmt::format("ADVSTONE: trip aborted (pos ({},{},{}) outside any known temple range)",
			pos.x, pos.y, pos.z));
		return false;
	}

	// Mimic adventurers_stone.lua: storage value tells the aid:4253 MoveEvent which town
	// to send the bot back to. Then teleport to the dungeon entry.
	player->addStorageValue(STORAGE_ADVENTURERS_GUILD_STONE, static_cast<int32_t>(townIdAtTemple));
	g_game().addMagicEffect(pos, CONST_ME_TELEPORT);
	BOT_TELEPORT(player, kAdventurerStoneDest, true);
	g_game().addMagicEffect(kAdventurerStoneDest, CONST_ME_TELEPORT);

	// Trip state setup
	bot.advStoneActive = true;
	bot.advStoneStartTownId = townIdAtTemple;
	bot.advStonePhase = 0;
	bot.advStoneRouteIdx = 0;
	bot.advStoneIdleAt = pickAdventurerStoneIdleIdx();
	bot.advStoneDwellUntil = 0;
	bot.advStoneDeadline = 0;
	// 3-way dwell sub-activity (chest / dummy / waypoint) — pre-selected at trip start so the
	// scan happens once, with the dungeon still empty of crowd-state churn from this trip.
	selectAdvStoneSubActivity(bot);
	// Clear normal POI/dwell state so doIdle won't fight us
	bot.currentPOI = nullptr;
	bot.hasWalkTarget = false;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.currentPos = kAdventurerStoneDest;

	const char* modeStr = (bot.advStoneDwellMode == 1) ? "chest"
	                    : (bot.advStoneDwellMode == 2) ? "dummy"
	                    : "waypoint";
	castLog(bot, fmt::format("ADVSTONE: trip started from town {}, idle wp={}, mode={}{}",
		townIdAtTemple, bot.advStoneIdleAt + 1, modeStr,
		bot.advStoneDwellMode == 2 ? fmt::format(" (weapon={})", bot.advStoneDwellWeaponId) : ""));
	return true;
}

void BotEngine::doAdventurerStone(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player || player->isRemoved()) {
		// Death handler clears advStoneActive — defensive; if we somehow get here with
		// a despawned player, just end the trip rather than try to drive a corpse.
		endAdventurerStoneTrip(bot);
		return;
	}

	const auto& route = adventurerStoneRoute_;

	// Phase 0: following_route
	// ORDERING NOTE: advStoneRouteIdx is synced from cityRouteIdx AFTER followCityRoute()
	// mutates it, so the idle-waypoint check below sees the PREVIOUS tick's index.
	// This causes a one-tick lag in the dwell trigger (benign at 100ms ticks). Do NOT
	// refactor to sync before the call — the dwell check expects to compare the index
	// BEFORE this tick's advancement, otherwise the dwell triggers one waypoint late.
	if (bot.advStonePhase == 0) {
		// Early home-arrival detection: the server-side aid:4253 MoveEvent can fire
		// during followCityRoute's grace-FC handling for the forcefield wp (the bot's
		// position jumps mid-handler from the dungeon to a temple). When this happens
		// we must end the trip BEFORE attempting goTo(forcefield), otherwise goTo
		// detects the z mismatch (bot at temple z, forcefield at z=6) and starts the
		// FC state machine, which drives the bot down the temple's stairs.
		// Skip this check at trip start (advStoneRouteIdx == 0): the trip-start
		// teleport leaves the bot at the dungeon entry, NOT in a temple range, so
		// findAdventurerStoneTownAt returns 0 — but the guard is cheap insurance.
		if (bot.advStoneRouteIdx > 0) {
			auto pos = player->getPosition();
			uint32_t homeTown = findAdventurerStoneTownAt(pos);
			if (homeTown != 0) {
				castLog(bot, fmt::format("ADVSTONE: trip complete, returned to town {}", homeTown));
				endAdventurerStoneTrip(bot);
				return;
			}
		}

		if (route.empty() || bot.advStoneRouteIdx >= route.size()) {
			// Route done but bot not yet detected at home — start stepping-on-forcefield
			// phase. Phase 2 polls findAdventurerStoneTownAt until it succeeds OR the
			// 30s deadline fires (fail-safe). We do NOT call goTo here: if the route
			// finished naturally (bot at forcefield tile, MoveEvent already fired),
			// phase 2's first tick will detect home arrival. If the bot is somehow
			// stuck near the forcefield, phase 2's listWalkDir-empty branch retries goTo.
			bot.advStonePhase = 2;
			bot.advStoneDeadline = OTSYS_TIME() + 30000;
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			return;
		}

		// Idle-waypoint detection — runs against the PREVIOUS tick's index (see comment above).
		// On reaching the chosen idle waypoint, we transition to phase 1. For mode==0 (route
		// waypoint dwell), the timer starts immediately. For mode==1/2 (chest/dummy), the
		// timer is deferred until the bot reaches advStoneDwellTarget — phase 1 handles the
		// walk-to-target + arrival detection + (mode 2) useItemEx kickoff.
		if (bot.advStoneRouteIdx == bot.advStoneIdleAt && bot.advStoneDwellUntil == 0) {
			const auto& wp = route[bot.advStoneRouteIdx];
			auto pos = player->getPosition();
			int32_t dx = std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(wp.pos.x));
			int32_t dy = std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(wp.pos.y));
			if (std::max(dx, dy) <= 2 && pos.z == wp.pos.z) {
				bot.advStonePhase = 1;
				if (bot.advStoneDwellMode == 0) {
					int32_t dwellSec = advStoneDwellSecs(0);
					bot.advStoneDwellUntil = OTSYS_TIME() + dwellSec * 1000LL;
					castLog(bot, fmt::format("ADVSTONE: dwelling at wp {}/{} for {}s",
						bot.advStoneRouteIdx + 1, route.size(), dwellSec));
				} else {
					// 30s deadline to reach the pre-picked sub-activity tile. If A* can't
					// path there (tile became occupied since trip start, route obstruction,
					// etc.), we demote to mode 0 and dwell in place.
					bot.advStoneDeadline = OTSYS_TIME() + 30000;
					castLog(bot, fmt::format("ADVSTONE: reached idle wp {}/{}, walking to {} target ({},{},{})",
						bot.advStoneRouteIdx + 1, route.size(),
						bot.advStoneDwellMode == 1 ? "chest" : "dummy",
						bot.advStoneDwellTarget.x, bot.advStoneDwellTarget.y, bot.advStoneDwellTarget.z));
				}
				return;
			}
		}

		// Drive route via existing followCityRoute mechanism — populate cityRouteWps and
		// let the well-tested route-following code handle navigation, FC transitions, etc.
		// Re-populate on every phase-0 (re-)entry: trip start (after stone use) and
		// dwell→follow resume both pass through here.
		if (!bot.followingCityRoute) {
			bot.cityRouteWps.assign(route.begin(), route.end());
			bot.cityRouteIdx = bot.advStoneRouteIdx;  // resume from where we left off
			bot.followingCityRoute = true;
		}

		bool stillRouting = followCityRoute(bot);
		// Sync our trip-local idx so we know when we hit advStoneIdleAt
		bot.advStoneRouteIdx = static_cast<uint16_t>(bot.cityRouteIdx);

		if (!stillRouting) {
			// Route complete — phase 2 takes over on the next tick. No goTo here:
			// the early home-arrival check at the top of phase 0 already runs each
			// tick, and if the bot was teleported home during grace-FC handling, that
			// check ends the trip cleanly. If the bot is somehow stuck near the
			// forcefield, phase 2 retries goTo when listWalkDir is empty.
			bot.advStonePhase = 2;
			bot.advStoneDeadline = OTSYS_TIME() + 30000;
			return;
		}
		return;
	}

	// Phase 1: dwelling
	// Mode 0: dwellUntil is set on phase entry; just wait for expiry.
	// Mode 1/2: walk to advStoneDwellTarget first. Once arrived, start the dwell timer
	// (mode 2 also fires useItemEx on the live backpack weapon to kick off the Lua
	// exercise training loop — that loop self-runs in Lua land until setTraining(false)).
	if (bot.advStonePhase == 1) {
		// Mode 1/2: walk to advStoneDwellTarget if not arrived, then start dwell on arrival.
		// IMPORTANT: this block fires on ANY mode-1/2 entry, not just when dwellUntil==0.
		// When a hibernated bot's virtual sim has already entered phase 1 (advStoneDwellUntil
		// pre-set with remaining time) and a player wakes it on the island, the live AI here
		// still needs to walk the bot to the chest/dummy target and kick off training — the
		// pre-set timer just counts down naturally with the leftover time.
		if (bot.advStoneDwellMode != 0) {
			auto pos = player->getPosition();
			bool arrived = (pos == bot.advStoneDwellTarget);
			if (!arrived) {
				// Wake-from-virtual path: virtual sim never sets advStoneDeadline, so on the
				// first tick post-wake it's still 0 and OTSYS_TIME() > 0 would trip the
				// expiry check immediately. Initialize to now+30s here mirroring the
				// trip-start initialization done elsewhere (line ~9929).
				if (bot.advStoneDeadline == 0) {
					bot.advStoneDeadline = OTSYS_TIME() + 30000;
				}
				// Deadline check — if we can't reach the pre-picked tile within 30s
				// (most likely cause: tile became occupied between trip-start scan and
				// arrival), demote to mode-0 dwell at current position so the trip
				// completes cleanly instead of stalling here forever.
				if (OTSYS_TIME() > bot.advStoneDeadline) {
					castLog(bot, fmt::format("ADVSTONE: walk-to-target deadline elapsed at ({},{},{}), demoting to mode-0 dwell",
						pos.x, pos.y, pos.z));
					bot.advStoneDwellMode = 0;
					bot.advStoneDwellWeaponId = 0;
					bot.advStoneDwellTarget = Position();
					bot.advStoneDeadline = 0;
					// Only restart dwellUntil if there's no remaining time (live-fresh demote).
					// On a wake-from-virtual mid-dwell demote, preserve the leftover time.
					if (bot.advStoneDwellUntil == 0 || OTSYS_TIME() >= bot.advStoneDwellUntil) {
						int32_t dwellSec = advStoneDwellSecs(0);
						bot.advStoneDwellUntil = OTSYS_TIME() + dwellSec * 1000LL;
					}
					return;
				}
				// listWalkDir-empty means goTo isn't currently driving — (re)issue it.
				if (player->listWalkDir.empty()) {
					goTo(bot, bot.advStoneDwellTarget, 0);
				}
				return;
			}
			bot.advStoneDeadline = 0; // arrived — clear deadline
			// Start the dwell timer ONLY if it wasn't already set by virtual sim. In the
			// wake-from-virtual path advStoneDwellUntil already counts down with the
			// leftover time, so don't reset it. In the live-fresh path it's 0 here.
			// Log only on first set (avoid per-tick spam on subsequent arrived ticks).
			if (bot.advStoneDwellUntil == 0) {
				int32_t dwellSec = advStoneDwellSecs(bot.advStoneDwellMode);
				bot.advStoneDwellUntil = OTSYS_TIME() + dwellSec * 1000LL;
				castLog(bot, fmt::format("ADVSTONE: arrived at {} target ({},{},{}), dwelling for {}s",
					bot.advStoneDwellMode == 1 ? "chest" : "dummy",
					pos.x, pos.y, pos.z, dwellSec));
			}

			if (bot.advStoneDwellMode == 2 && !bot.advStoneTrainingActive) {
				// Fetch the live backpack weapon — the Lua action requires the weapon to be in
				// the player's inventory (getItemCount > 0); a transient CreateItem would fail.
				auto weapon = g_game().findItemOfType(player, bot.advStoneDwellWeaponId, true, -1);
				if (!weapon) {
					castLog(bot, fmt::format("ADVSTONE: training weapon {} not in backpack — skipping kickoff",
						bot.advStoneDwellWeaponId));
				} else {
					// Resolve the dummy's actual stackpos so STACKPOS_USETARGET picks it
					// (not the ground tile at stackpos 0). Lua action checks isDummy(targetId);
					// without the right stackpos, target resolves to ground and action silently
					// no-ops — bot stands still, no swing animation, no training started.
					auto dummyTile = g_game().map.getTile(kAdvStoneExerciseDummy);
					std::shared_ptr<Item> dummyItem;
					uint8_t dummyStackPos = 0;
					if (dummyTile) {
						if (auto items = dummyTile->getItemList()) {
							for (const auto& it : *items) {
								if (it && it->isDummy()) { dummyItem = it; break; }
							}
						}
						if (dummyItem) {
							int32_t sp = dummyTile->getThingIndex(dummyItem);
							if (sp >= 0 && sp <= 255) dummyStackPos = static_cast<uint8_t>(sp);
						}
					}
					if (!dummyItem) {
						castLog(bot, fmt::format("ADVSTONE: training kickoff aborted — no dummy item at ({},{},{})",
							kAdvStoneExerciseDummy.x, kAdvStoneExerciseDummy.y, kAdvStoneExerciseDummy.z));
					} else {
						castLog(bot, fmt::format("ADVSTONE: dummy resolved id={} stackpos={}",
							dummyItem->getID(), dummyStackPos));
						bool ok = g_actions().useItemEx(player, weapon->getPosition(),
							kAdvStoneExerciseDummy, dummyStackPos, weapon, false);
						bot.advStoneTrainingActive = ok;
						castLog(bot, fmt::format("ADVSTONE: training kickoff weapon={} ({}), useItemEx={}",
							bot.advStoneDwellWeaponId,
							isMeleeExerciseWeapon(bot.advStoneDwellWeaponId) ? "melee" : "ranged",
							ok ? "OK" : "FAILED"));
					}
				}
			}
			// Fall through to the timer-expiry check below — handles dwell expiry uniformly
			// for both mode-0 and mode-1/2 paths instead of duplicating the cleanup logic.
		}

		if (OTSYS_TIME() >= bot.advStoneDwellUntil) {
			// Stop training if it was active (Lua loop self-terminates on next iteration).
			stopAdvStoneTrainingIfActive(bot);
			// Reset sub-activity state — next idle waypoint will use mode 0 by default.
			bot.advStoneDwellMode = 0;
			bot.advStoneDwellWeaponId = 0;
			bot.advStoneDwellTarget = Position();
			bot.advStoneDwellUntil = 0;
			bot.advStonePhase = 0;
			bot.advStoneRouteIdx++;
			// Force followCityRoute to re-init when phase 0 resumes — clears the
			// function-static s_routeSkipCount entry (followCityRoute clears it on a
			// false return, which we trigger by setting followingCityRoute = false +
			// cityRouteWps.clear() here, then re-populating in the next phase-0 entry).
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
		}
		return;
	}

	// Phase 2: stepping_on_forcefield — wait for the aid:4253 MoveEvent to teleport home
	if (bot.advStonePhase == 2) {
		auto pos = player->getPosition();
		// Success signal: bot's position is inside one of the known temple ranges
		uint32_t arrivedTown = findAdventurerStoneTownAt(pos);
		if (arrivedTown != 0) {
			castLog(bot, fmt::format("ADVSTONE: trip complete, returned to town {}", arrivedTown));
			endAdventurerStoneTrip(bot);
			return;
		}
		// Still in the dungeon — keep walking to the forcefield until deadline
		if (OTSYS_TIME() > bot.advStoneDeadline) {
			castLog(bot, fmt::format("ADVSTONE: forcefield deadline elapsed (pos ({},{},{})), ending trip",
				pos.x, pos.y, pos.z));
			endAdventurerStoneTrip(bot);
			return;
		}
		// Re-issue the goTo periodically in case the bot got knocked off-path
		if (player->listWalkDir.empty()) {
			goTo(bot, kAdventurerStoneForcefield, 0);
		}
		return;
	}
}

void BotEngine::clearAdvStoneState(BotState& bot) {
	// Terminate any active Lua exercise training loop before clearing state.
	// Called from every trip-end path (natural completion, death, combat/PK interruption,
	// forceDeactivate, hot-reload teardown) so the Lua addEvent doesn't outlive the trip,
	// AND from party recruitment so an AdvStone-active bot stops preempting doParty.
	stopAdvStoneTrainingIfActive(bot);
	bot.advStoneActive = false;
	bot.advStonePhase = 0;
	bot.advStoneRouteIdx = 0;
	bot.advStoneIdleAt = 0;
	bot.advStoneStartTownId = 0;
	bot.advStoneDwellUntil = 0;
	bot.advStoneDeadline = 0;
	bot.advStoneDwellMode = 0;
	bot.advStoneDwellWeaponId = 0;
	bot.advStoneDwellTarget = Position();
}

void BotEngine::endAdventurerStoneTrip(BotState& bot) {
	clearAdvStoneState(bot);
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	// 2026-06-10 fix: allow this bot to re-pick AdvStone on its next POI roll without
	// waiting for the full visitedPOIs cycle to exhaust all other POIs. Without this,
	// AdvStone was effectively rationed to once per ~6-POI cycle per bot, which is the
	// dominant cause of "after the initial cohort leaves, no new bots arrive at the island."
	// See workflow analysis in the design notes.
	bot.visitedPOIs.erase("adventurer_stone");
	if (bot.state == BotAIState::INACTIVE) return;
	bot.state = BotAIState::IDLE;
}
