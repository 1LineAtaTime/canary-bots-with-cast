/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bot/bot_engine_impl.hpp"

// ============================================================================
// BOT_AMBIENT_ROAM
//
// Bots materialise on a vetted tile just outside a real player's viewport, walk to a random
// reachable tile near them (floors above and below included), stand there a while, then pick
// another. When the player leaves they are hibernated and put back where they came from.
//
// The existing liveness systems are DEMAND-side: a hibernated bot wakes when a player happens to
// walk near wherever it virtually is, and player-proximity weighting nudges its next virtual
// destination toward players over minutes. Both do nothing when no bot happens to be nearby. This
// is the SUPPLY-side half — it puts a cast on stage where the player actually is, now.
//
// Two structural decisions carry most of the safety:
//
//  * No new BotAIState. A roamer is an ordinary IDLE/DWELLING bot to every other subsystem, so it
//    keeps normal combat, chat, supply and hibernation behaviour for free, and every existing
//    teardown that moves a bot out of those states implicitly ends the session. The session lives
//    in a per-guid side table, exactly like the fishing run and the house visit.
//
//  * Roamers do not compete with organic wakes. They draw from botRoamReserveSlots EXTRA slots per
//    density ring, and the organic arm of the cap subtracts them, so ordinary proximity wakes keep
//    their full budget. Without that subtraction the feature would silently tax the very liveness
//    it is supposed to add to.
// ============================================================================

namespace {

// 60s [ROAM] summary counters. File-scope in ONE TU, so `static` is correct here — the shared
// header is where `static` would silently fork per-TU copies.
struct RoamStats {
	uint32_t injected = 0;
	uint32_t gatedDensity = 0;   // the cap refused (reserve full, or ring over limit+reserve)
	uint32_t gatedNoStaging = 0; // nowhere off-screen, safe and near enough to put the bot
	uint32_t gatedNoRegion = 0;  // nothing reachable near the anchor (or build deferred)
	uint32_t gatedNoRecruit = 0; // no eligible hibernated bot anywhere
	uint32_t gatedUnaccountable = 0; // staging tile sits where the ring counters cannot see it
	uint32_t gatedKeepout = 0;   // injections refused: nowhere to stage outside somebody's spawn
	uint32_t legKeepoutFails = 0; // leg PICKS that failed with at least one keep-out rejection
	                              // (not a count of rejected tiles — a pick that succeeds anyway
	                              // is not a problem and is deliberately not reported)
	uint32_t evictedKeepout = 0; // sessions retired because a hunt started on top of them
	uint32_t wakeFailed = 0;
	uint32_t legs = 0;
	uint32_t arrivals = 0;
	uint32_t legFails = 0;
	uint32_t defendFights = 0;
	uint32_t defendKills = 0;
	uint32_t suspended = 0;
	uint32_t resumed = 0;
	uint32_t regionDeferred = 0;
	uint32_t relNoAnchor = 0, relTtl = 0, relFailstreak = 0, relRegionGone = 0, relInvariant = 0;
	uint32_t relSpawnClaimed = 0;
	uint32_t hibernateRefused = 0;
};
RoamStats s_roamStats;
int64_t s_lastRoamSummaryMs = 0;
// Per-cluster injection pacing, keyed by quantized centroid so a drifting anchor keeps its slot.
std::unordered_map<uint64_t, int64_t> s_roamNextInjectMs;

int32_t roamCheb(const Position& a, const Position& b) {
	return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
	                std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
}

} // namespace

// Inside the neighbourhood of a spawn somebody is working.
//
// Banded to one floor rather than z-agnostic like the density rings. Those are about eyes and
// bodies, which a floor does not separate; this is about MONSTERS, which it does — a wanderer on
// the surface is no part of a dungeon hunt eight floors below, and towns sit on top of spawns often
// enough that a z-agnostic radius here would quietly stop roam serving half of Thais. One floor,
// not zero, because botRoamMaxDz lets a leg cross a floor: a roamer staged one z away can walk in.
bool BotEngine::roamInKeepout(const Position& p) const {
	for (const auto& a : roamSuppressedPts_) {
		if (std::abs(static_cast<int32_t>(a.z) - static_cast<int32_t>(p.z)) > 1) continue;
		if (roamCheb(a, p) <= ROAM_SUPPRESS_KEEPOUT) return true;
	}
	return false;
}

// Telemetry tag for a session ending. A member rather than a file-scope helper because RoamEnd is
// private to BotEngine, and widening a type's visibility just so a log line can name it would be
// the tail wagging the dog.
const char* BotEngine::roamEndName(RoamEnd why) {
	switch (why) {
		case RoamEnd::NO_ANCHOR:   return "no_anchor";
		case RoamEnd::SESSION_TTL: return "session_ttl";
		case RoamEnd::FAILSTREAK:  return "failstreak";
		case RoamEnd::REGION_GONE: return "region_gone";
		case RoamEnd::INVARIANT:   return "invariant";
		case RoamEnd::SPAWN_CLAIMED: return "spawn_claimed";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

// The ledger is the ACCOUNTING view and deliberately outlives the behavioural session: a bot whose
// session ended but which is still awake beside the player is still one of the extra bodies the
// reserve paid for, and releasing its slot early would let the next injection push the ring past
// its true ceiling.
//
// So the sweep is a LEAK guard, not a retirement policy: it only drops entries whose bot is no
// longer awake-and-active (or has left the population entirely). An earlier revision expired live
// entries on a timer, which handed the bot back to the ORGANIC arm of the cap and suppressed
// ordinary wakes instead — the same starvation one arm over.
void BotEngine::sweepRoamLedger(int64_t nowMs) {
	if (s_roamLedger.empty()) return;
	const int64_t ttl = std::max<int64_t>(30000, g_configManager().getNumber(BOT_ROAM_LEDGER_TTL_MS));
	for (auto it = s_roamLedger.begin(); it != s_roamLedger.end();) {
		bool drop = false;
		auto idx = guidToIndex_.find(it->first);
		if (idx == guidToIndex_.end()) {
			drop = true;  // no longer registered at all
		} else {
			const BotState& b = bots_[idx->second];
			// Not awake -> it occupies no density, so it owes the reserve nothing.
			if (b.hibernated || !b.active) drop = true;
		}
		// Backstop only: a live awake entry this old means a teardown site was missed. Bounded
		// rather than permanent, and loud enough to find in the summary if it ever fires.
		if (!drop && nowMs - it->second.grantedMs > ttl * 4) drop = true;
		it = drop ? s_roamLedger.erase(it) : std::next(it);
	}
}

// ---------------------------------------------------------------------------
// Session invariants
// ---------------------------------------------------------------------------

// SESSION-level only. The two walk-level conditions (the planner claim is still ours, and the bot
// still has a walk target) are checked inside the WALKING driver instead, because both go false at
// the moment of a perfectly healthy ARRIVAL — the walk system consumes them — and a supervisor
// that ran first would tear down every session on the tick it succeeded. The driver can tell the
// two apart by distance; the supervisor cannot.
bool BotEngine::roamSessionInvariantsHold(BotState& bot, const RoamRun& run) const {
	if (bot.hibernated || !bot.active) return false;
	if (bot.aiPaused) return false;
	if (s_debugPinned.count(bot.guid)) return false;
	if (bot.stopCooldownUntil > OTSYS_TIME()) return false;
	if (bot.isQuestBot) return false;
	// Anything below means another subsystem owns the bot. These do not fail the session outright
	// — the caller SUSPENDS on them (see tickAmbientRoam), because a bot that fights and wins
	// should go back to wandering rather than being thrown away.
	if (bot.partyHuntId > 0) return false;
	if (s_botToPartyHunt.count(bot.guid) || s_rvMember.count(bot.guid)) return false;
	if (s_gangByGuid.count(bot.guid)) return false;
	if (bot.advStoneActive) return false;
	if (isFishing(bot.guid) || isHouseVisiting(bot.guid) || isIceFishing(bot.guid)) return false;
	// A shrine visitor is an ordinary DWELLING bot to everything else, so without this it is
	// recruitable — and roam would walk it off the tile it reserved, mid-visit, with the run and
	// the claim still standing.
	if (isShrineVisiting(bot.guid)) return false;
	if (bot.state != BotAIState::IDLE && bot.state != BotAIState::DWELLING) return false;
	(void)run;
	return true;
}

// ---------------------------------------------------------------------------
// Ending a session
// ---------------------------------------------------------------------------

void BotEngine::endRoamSession(BotState& bot, RoamEnd why) {
	auto it = s_roam.find(bot.guid);
	if (it == s_roam.end()) return;
	RoamRun run = it->second;   // copy: hibernateBot below may invalidate the map entry
	s_roam.erase(it);

	clearRoamDefense(bot, run);
	clearPlannerWalk(bot.guid);
	bot.hasWalkTarget = false;
	bot.pendingNavDest.clear();
	bot.pathFailCount = 0;
	bot.consecutivePOIFails = 0;
	bot.huntTargetId = 0;
	s_walkTargetTimer.erase(bot.guid);
	// A roam dwell must not outlive the session. Left stretched, the bot hibernates pinned in a
	// long virtual dwell and does nothing for minutes — the same trap endHouseVisit normalises.
	if (bot.dwellUntil > OTSYS_TIME()) bot.dwellUntil = 0;

	switch (why) {
		case RoamEnd::NO_ANCHOR:   s_roamStats.relNoAnchor++;  break;
		case RoamEnd::SESSION_TTL: s_roamStats.relTtl++;       break;
		case RoamEnd::FAILSTREAK:  s_roamStats.relFailstreak++; break;
		case RoamEnd::REGION_GONE: s_roamStats.relRegionGone++; break;
		case RoamEnd::INVARIANT:   s_roamStats.relInvariant++;  break;
		case RoamEnd::SPAWN_CLAIMED: s_roamStats.relSpawnClaimed++; break;
	}

	// INVARIANT endings mean something else legitimately took the bot; it stays awake and busy,
	// and its ledger entry stays with it until it is no longer awake. Every other ending is the
	// feature letting go, and there the bot goes home and to sleep — which is what makes the
	// reserve self-clearing and stops the population draining toward player hotspots over hours.
	if (why == RoamEnd::INVARIANT) {
		g_logger().info("[BotEngine] [ROAM] released bot='{}' reason={} legs={}",
			bot.name, roamEndName(why), run.legs);
		return;
	}

	// NEVER vanish in front of someone. Hibernation deletes the body from the world, and the
	// restore-teleport removes it from where it stands — both are a pop-OUT, which is exactly the
	// sin off-screen staging exists to avoid at the other end. User-reported: "I do see some bots
	// randomly disappear while I am still there within visible range."
	//
	// If the bot is on any anchor's screen we simply stop owning it: the session ends, the LEDGER
	// ENTRY STAYS (it is still an extra awake body near the player, so the reserve must keep
	// accounting for it), and the bot carries on under normal AI. It walks off by itself and the
	// ordinary hibernation loop retires it once the player leaves. We lose the home-restore for
	// that bot, which is a far smaller cost than a body blinking out of existence on screen.
	auto player = bot.getPlayer();
	if (wouldBeSeenByAnchor(bot.currentPos, 0)) {
		g_logger().info("[BotEngine] [ROAM] released bot='{}' reason={} legs={} — observed, so left "
			"awake in place (no teleport, no hibernate)", bot.name, roamEndName(why), run.legs);
		return;
	}

	// hibernateBot also refuses cast-watched bots, human-led party members and bots in the death
	// pipeline. Same rule for the same reason.
	const bool watched = player && player->getCastViewerCount() > 0;
	if (!watched && run.homePos.x > 0 && player) {
		if (!isUnsafeWakeTile(bot, run.homePos)) {
			BOT_TELEPORT(player, run.homePos, true);
			bot.currentPos = run.homePos;
			bot.lastPos = run.homePos;
		}
	}
	const bool slept = hibernateBot(bot.guid);
	if (!slept) s_roamStats.hibernateRefused++;

	g_logger().info("[BotEngine] [ROAM] released bot='{}' reason={} legs={} home=({},{},{}) hibernated={}",
		bot.name, roamEndName(why), run.legs,
		run.homePos.x, run.homePos.y, run.homePos.z, slept ? "yes" : "refused");
}

// ---------------------------------------------------------------------------
// Monster self-defense
// ---------------------------------------------------------------------------

void BotEngine::clearRoamDefense(BotState& bot, RoamRun& run) {
	if (run.defendTargetId == 0) return;
	run.defendTargetId = 0;
	run.defendSinceMs = 0;
	bot.huntTargetId = 0;
	if (auto player = bot.getPlayer()) {
		player->setAttackedCreature(nullptr);
		player->setFollowCreature(nullptr);
	}
}

// One tick of fighting back. Ported from tickFishingDefense, and the property that makes the port
// work is the same one that made the original work: it NEVER writes bot.state. Entering COMBAT
// would trip the session's own (IDLE||DWELLING) invariant and throw away the run.
//
// This matters more for roaming than for fishing. doSelfDefense only reacts to PLAYER attackers,
// so without this a roamer sent to a tile near a spawn is simply eaten, and pauseBotForDeath then
// teleports it to its temple in full view of the player — the exact pop-out the whole feature
// exists to remove.
bool BotEngine::tickRoamDefense(BotState& bot, RoamRun& run) {
	auto player = bot.getPlayer();
	if (!player) return false;
	const int64_t now = OTSYS_TIME();

	// Never fight out of a protection zone: castSpell refuses to fire from one anyway, but
	// chaseTarget has no such guard and would walk the bot out of safety to reach something it
	// structurally cannot hit.
	if (auto selfTile = g_game().map.getTile(bot.currentPos);
	    selfTile && selfTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
		clearRoamDefense(bot, run);
		return false;
	}

	const int64_t maxMs = std::max<int64_t>(5000, g_configManager().getNumber(BOT_ROAM_DEFEND_MAX_MS));
	std::shared_ptr<Creature> target;
	if (run.defendTargetId != 0) {
		target = g_game().getCreatureByID(run.defendTargetId);
		const bool dead = !target || target->isRemoved() || target->getHealth() <= 0;
		if (dead) {
			run.defendKills++;
			s_roamStats.defendKills++;
			clearRoamDefense(bot, run);
			target = nullptr;
		} else if (target->getPosition().z != bot.currentPos.z) {
			clearRoamDefense(bot, run);
			target = nullptr;
		} else if (now - run.defendSinceMs > maxMs) {
			// Cannot finish it — disengage and let the leg carry on. Better a bot that walks away
			// from a fight it is losing than one welded to an unkillable target forever.
			castLog(bot, "ROAM: cannot kill attacker in time — disengaging");
			clearRoamDefense(bot, run);
			target = nullptr;
		} else if (roamCheb(bot.currentPos, run.defendOrigin) > ROAM_DEFEND_LEASH) {
			// Leashed to where the bot STOOD when it engaged, not to the leg destination: a leg
			// dest can be twenty tiles off, and a chase measured against it could drag the fight
			// across the whole region, possibly straight at the player.
			castLog(bot, "ROAM: attacker pulled us off the leash — disengaging");
			clearRoamDefense(bot, run);
			target = nullptr;
		}
	}

	if (!target) {
		// A roamer leaving somebody's SPAWN does not start new fights on the way out: that is the
		// difference between a wanderer that walks off when a hunt starts around it and one that
		// spends the next ninety seconds killing that hunt's monsters — measured live at 16 a
		// minute in a party's spawn. An engagement already in progress is left alone; its own cap,
		// leash and z-test end it, and dropping a fight mid-swing only leaves the bot being hit
		// while it walks.
		//
		// Narrowed to the keep-out rather than to `retiring` at large. A run retiring for the
		// ordinary reasons — session TTL, the player walked away — is not in anybody's way, and
		// refusing to let it defend itself would make it a punching bag for no gain.
		if (run.retiring && roamInKeepout(bot.currentPos)) return false;
		auto threat = pickFishingThreat(bot);   // nearest monster whose target is THIS bot
		if (!threat) return false;
		run.defendTargetId = threat->getID();
		run.defendSinceMs = now;
		run.defendOrigin = bot.currentPos;
		s_roamStats.defendFights++;
		target = threat;
		castLog(bot, fmt::format("ROAM: defending against {}", target->getName()));
	}

	// Hold the outer dwell ahead of the fight. Returning true is what actually keeps the bot in
	// DWELLING, but a stale dwellUntil would strand it if this hook ever stopped running.
	if (bot.state == BotAIState::DWELLING) {
		bot.dwellUntil = std::max(bot.dwellUntil, now + 30000);
	}
	// The "current monster target" contract every busy-gate in the engine already reads (rune
	// crafting, support spells, liveness micro-actions, fidget drop, the idle clock).
	bot.huntTargetId = run.defendTargetId;

	// Re-assert the engine target every tick, minus the z-change grace window: castSpell only sets
	// it when a single-target instant wins its scoring, so with a rune winner or everything on
	// cooldown a knight would otherwise stand in melee range dealing exactly zero damage.
	{
		auto zIt = s_lastZChangeTime.find(bot.guid);
		const bool inZGrace = (zIt != s_lastZChangeTime.end() && now - zIt->second < Z_CHANGE_GRACE_MS);
		if (!inZGrace && player->getAttackedCreature() != target) {
			player->setAttackedCreature(target);
		}
	}

	castSpell(bot, target);

	// Chase only while the fight stays near where it started; past that hold position and keep
	// attacking if still in reach, and let the abort timer resolve a monster that simply flees.
	if (roamCheb(run.defendOrigin, target->getPosition()) <= ROAM_DEFEND_LEASH) {
		chaseTarget(bot, target);
	} else {
		// setAttackedCreature re-arms engine follow under chaseMode, and Creature::
		// setAttackedCreature silently drops the engagement on a transient canSee/z failure, so
		// the guarded re-assert above legitimately re-fires mid-hold. Must run every holding tick.
		player->setFollowCreature(nullptr);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Leg selection
// ---------------------------------------------------------------------------

bool BotEngine::roamPickNextLeg(BotState& bot, RoamRun& run) {
	// A RETIRING run must still be able to pick legs, and by then its anchor is very often the one
	// that just became ineligible — so eligibility is dropped here and the full camera list is
	// used. Without this the picker starves at exactly the moment the walk-off it exists to perform
	// is needed, and the bot is abandoned awake on the player's screen: the earlier defect wearing
	// a new label. Pass 0 of the destination loop below is what then aims it out of view.
	const std::vector<Position>& pickAnchors = run.retiring ? currentAnchorPts_ : roamAnchorPts_;
	if (pickAnchors.empty() && roamDebugAnchor_.x == 0) return false;

	// Re-target the NEAREST anchor each leg, so a slowly-moving player keeps company without the
	// bot ever pathing AT them. The session's own release distance is what stops this becoming a
	// follow: drift past it and the roamer goes home instead of tagging along.
	Position anchor = roamDebugAnchor_;
	int32_t best = anchor.x > 0 ? roamCheb(bot.currentPos, anchor) : INT32_MAX;
	for (const auto& a : pickAnchors) {
		const int32_t d = roamCheb(bot.currentPos, a);
		if (d < best) { best = d; anchor = a; }
	}
	if (anchor.x == 0) return false;

	const auto* region = getRoamRegion(anchor);
	if (!region || region->dests.empty()) {
		s_roamStats.regionDeferred++;
		g_logger().info("[BotEngine] [ROAM] pick_fail bot='{}' cause=no_region anchor=({},{},{})",
			bot.name, anchor.x, anchor.y, anchor.z);
		return false;
	}

	const int32_t minLeg = std::max(4, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_MIN_LEG_DIST)));

	// Reservoir-sample a destination that is far enough from the BOT to be a visible walk. Measured
	// from the bot and not from the anchor: the anchor distance says nothing about how far this
	// particular bot has to travel, and a floor under the arrival tolerance (3) would let a leg
	// complete in a single step — a shuffle in place rather than a walk.
	// A RETIRING run walks toward the exit: pass 0 considers only destinations that are off every
	// anchor's screen, so the bot leaves view under its own feet and the supervisor can retire it
	// there without anyone watching it go.
	//
	// Two passes rather than one filter, because "off-screen" must never make a leg unpickable. In
	// a tight indoor region there may be no such tile at all — the genuinely cornered case — and
	// there the unfiltered pass takes over and the retirement cap owns the outcome. Without pass 0
	// a retiring bot picks uniformly at random from a region centred on the anchor, so roughly half
	// its "walk-off" legs head straight across the claimant's screen.
	Position chosen;
	uint32_t seen = 0, tooNear = 0, unsafe = 0, keepout = 0;
	for (int pass = (run.retiring ? 0 : 1); pass <= 1 && chosen.x == 0; ++pass) {
		seen = 0; tooNear = 0; unsafe = 0; keepout = 0;
		for (const auto& p : region->dests) {
			if (roamCheb(bot.currentPos, p) < minLeg) { tooNear++; continue; }
			if (p == run.dest) continue;                 // no immediate repeat
			if (pass == 0 && wouldBeSeenByAnchor(p, ASSEMBLY_OFFSCREEN_MARGIN)) continue;
			// NOT applied to a retiring run, and that exemption is load-bearing rather than
			// permissive. botRoamRadius is 20 and getRoamRegion floods exactly that far, so every
			// tile a region can offer is within 20 of the anchor it was built around — while the
			// keep-out is 30. A retiring bot picks its anchor from the FULL camera list (a43a635ef:
			// its own anchor is usually the suppressed one), so filtering here would reject the
			// entire region, every pass, and the walk-off would starve: 45s of pick_fail standing
			// still inside the spawn, then the retirement cap firing into endRoamSession's
			// observed-check and abandoning the bot awake exactly where it must not be. That is the
			// defect a43a635ef, 216c29704 and 0acc8bcae were each written to remove.
			//
			// Leaving is already steered away from the claimant by pass 0's off-screen filter,
			// which is the right instrument: it asks "can they see it", not "is it far enough".
			if (!run.retiring && roamInKeepout(p)) { keepout++; continue; }
			if (isUnsafeWakeTile(bot, p)) { unsafe++; continue; }  // per-pick: needs the bot
			if (++seen == 1 || uniform_random(1, static_cast<int32_t>(seen)) == 1) {
				chosen = p;
			}
		}
	}
	if (chosen.x == 0) {
		if (keepout > 0) s_roamStats.legKeepoutFails++;
		g_logger().info("[BotEngine] [ROAM] pick_fail bot='{}' cause=no_candidate dests={} "
			"tooNear={} unsafe={} keepout={} retiring={} botPos=({},{},{})",
			bot.name, region->dests.size(), tooNear, unsafe, keepout, run.retiring ? 1 : 0,
			bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
		return false;
	}

	run.dest = chosen;
	run.failStreak = 0;   // a successful pick clears a transient streak
	run.phase = RoamPhase::WALKING;
	run.phaseSinceMs = OTSYS_TIME();
	bot.walkTarget = chosen;
	bot.hasWalkTarget = true;
	bot.currentPOI = nullptr;
	bot.pendingNavDest.clear();
	bot.pathFailCount = 0;
	bot.state = BotAIState::IDLE;
	bot.dwellUntil = 0;
	// Claim the scoped planner, which is what gives roam cross-floor routing, door opening, the
	// stall detector and the 240s budget without re-implementing any of it.
	s_plannerWalk[bot.guid] = chosen;
	run.legs++;
	s_roamStats.legs++;

	castLog(bot, fmt::format("ROAM: leg {} to ({},{},{}) dist={}",
		run.legs, chosen.x, chosen.y, chosen.z, roamCheb(bot.currentPos, chosen)));
	g_logger().info("[BotEngine] [ROAM] leg_start bot='{}' seq={} dest=({},{},{}) cheb_from_bot={}",
		bot.name, run.legs, chosen.x, chosen.y, chosen.z, roamCheb(bot.currentPos, chosen));
	return true;
}

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

bool BotEngine::injectRoamBot(size_t clusterIdx, int64_t nowMs) {
	if (clusterIdx >= currentAnchors_.size()) return false;
	const Position centroid = currentAnchors_[clusterIdx].centroid;

	// Stage against a RAW anchor, not the centroid: with several players merged into one cluster
	// the centroid can sit in open ground nobody is standing on, and "just off screen" is a
	// statement about a real viewport.
	// This cluster's OWN roam-eligible member, recorded at build time. Picking the global-nearest
	// eligible anchor instead lets an injection raised for a suppressed cluster reach across the
	// map, stage against a foreign anchor, and be charged to the wrong cluster's occupancy count.
	Position anchor = currentAnchors_[clusterIdx].hasRoamAnchor
		? currentAnchors_[clusterIdx].roamAnchor : Position();
	if (anchor.x == 0 && roamDebugAnchor_.x > 0) anchor = roamDebugAnchor_;
	if (anchor.x == 0) return false;

	const auto* region = getRoamRegion(anchor);
	if (!region || region->dests.empty()) {
		s_roamStats.gatedNoRegion++;
		return false;
	}

	const int32_t midRadius = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_MID_RADIUS));

	// --- staging tile: off-screen, safe, reachable, and ACCOUNTABLE ---
	// Same-floor first. A cellar tile one floor down is off-screen at any horizontal distance for
	// a surface anchor, which is tempting, but a roamer staged a staircase away may never be seen
	// at all — the walk-in is the point. Cross-floor is the rescue for tight indoor spaces.
	std::vector<const Position*> sameFloor, otherFloor;
	bool keepoutSkipped = false;
	for (const auto& p : region->dests) {
		const int32_t d = roamCheb(anchor, p);
		if (d < ROAM_STAGE_MIN_DIST || d > ROAM_STAGE_MAX_DIST) continue;
		// Refuse to place a roamer where the ring counters cannot see it. Ring membership is
		// measured from the cluster CENTROID, so a tile inside midRadius of an edge anchor but
		// outside it from the centroid would be invisible to the cap — an unaccounted body.
		if (roamCheb(centroid, p) > midRadius) continue;
		// Never stage into a spawn somebody is working, even when this cluster's own anchor is
		// perfectly eligible. Staging is bounded by midRadius (50) from the CENTROID, so without
		// this a roamer raised for an idle player could be placed on top of a hunt 40 tiles away —
		// and the two need not even share a cluster, which is why anchor eligibility alone (a
		// targeting rule) could never have covered it.
		if (wouldBeSeenByAnchor(p, ASSEMBLY_OFFSCREEN_MARGIN + 3)) continue;
		// AFTER the on-screen test, not before: a tile that was unusable anyway must not be
		// attributed to the keep-out, or gatedKeepout absorbs gatedNoStaging's cases and the
		// summary loses the very distinction it was added to draw.
		if (roamInKeepout(p)) { keepoutSkipped = true; continue; }
		(p.z == anchor.z ? sameFloor : otherFloor).push_back(&p);
	}
	if (sameFloor.empty() && otherFloor.empty()) {
		// Distinguish "nowhere off-screen" from "nowhere outside a spawn". Both end the injection,
		// but only one of them means the suppression did its job, and a summary that cannot tell
		// them apart is how the previous version of this gate stayed inert for a week.
		if (keepoutSkipped) s_roamStats.gatedKeepout++;
		else s_roamStats.gatedNoStaging++;
		return false;
	}
	auto& pool = sameFloor.empty() ? otherFloor : sameFloor;

	// --- recruit: farthest-away eligible hibernated bot ---
	// Farthest on purpose. Bots near the player will wake organically and provide their own
	// traffic; conscripting them would just move liveness around. The far ones are doing nothing
	// anybody can see, so borrowing them costs the world nothing visible.
	uint32_t pickGuid = 0;
	int32_t pickDist = -1;
	int64_t pickLru = 0;
	for (const auto& b : bots_) {
		if (!b.active || !b.hibernated) continue;
		if (b.state != BotAIState::IDLE && b.state != BotAIState::DWELLING) continue;
		if (b.huntScriptId > 0 || b.partyHuntId > 0) continue;
		if (b.advStoneActive || b.isQuestBot) continue;
		if (b.aiPaused || b.deathPauseUntil > 0) continue;
		if (s_debugPinned.count(b.guid)) continue;
		if (s_roam.count(b.guid) || s_roamLedger.count(b.guid)) continue;
		if (s_rvMember.count(b.guid) || s_botToPartyHunt.count(b.guid)) continue;
		if (s_gangByGuid.count(b.guid) || s_reclaimToInactive.count(b.guid)) continue;
		const int32_t d = roamCheb(b.currentPos, anchor);
		if (d > pickDist || (d == pickDist && b.lastWakeAttemptMs < pickLru)) {
			pickDist = d;
			pickGuid = b.guid;
			pickLru = b.lastWakeAttemptMs;
		}
	}
	if (pickGuid == 0) {
		s_roamStats.gatedNoRecruit++;
		return false;
	}

	auto idx = guidToIndex_.find(pickGuid);
	if (idx == guidToIndex_.end()) return false;
	BotState& bot = bots_[idx->second];

	const Position staging = *pool[uniform_random(0, static_cast<int32_t>(pool.size()) - 1)];

	// Hand back whatever the bot was holding BEFORE anything else, or a hunt reservation leaks.
	// Eligibility above already refused stop-cooldowned bots, which matters because this call
	// clears that cooldown as a side effect.
	releasePartyMemberActivity(bot, "ambient_roam");
	// Virtual-sim leftovers that survive hibernation and would otherwise freeze the bot on its
	// staging tile: a locker reroll timer returns from doIdle unconditionally for up to a minute,
	// and a carried-over failure count plus roam's own bad legs reaches the give-up ladder that
	// ends in a teleport to temple — in full view of the player.
	s_depotLockerRerollTime.erase(bot.guid);
	s_depotDwellWalkTarget.erase(bot.guid);
	s_depotDwellWalkFails.erase(bot.guid);
	s_depotWalkRetries.erase(bot.guid);
	clearDepotBlacklist(bot.guid);
	s_staleWalkStart.erase(bot.guid);
	s_walkTargetTimer.erase(bot.guid);
	bot.consecutivePOIFails = 0;
	bot.pathFailCount = 0;
	bot.hasDepotTarget = false;

	const Position home = bot.currentPos;
	const Position homeLast = bot.lastPos;

	beginWakeBurst();  // clear stale tile reservations so placement cannot drift into the viewport
	bot.currentPos = staging;
	bot.lastPos = staging;
	s_proximityWake = false;   // no route rewind, no login sparkle: the tile is already off-screen
	s_roamWakeGuid = pickGuid; // raises the cap by the reserve; NOT s_forceWakeGuid, which bypasses it
	const bool woke = wakeBot(pickGuid);
	s_roamWakeGuid = 0;        // wakeBot has refusals that return before the gate ever consumes it
	s_proximityWake = true;

	if (!woke) {
		// Put the virtual position back. Without this a refused injection strands the bot
		// hibernated ON the staging tile: its position is persisted by the 5-minute virtual save,
		// the Lua proximity loop then hammers ordinary wakes at it, and its whole virtual life has
		// silently relocated next to a player it was never assigned to.
		bot.currentPos = home;
		bot.lastPos = homeLast;
		s_roamLedger.erase(pickGuid);
		s_roamStats.gatedDensity++;
		return false;
	}

	// Post-condition: the bot must actually BE near the anchor. chooseWakePosition vets the tile it
	// is handed and, when it does not like it, walks back through the bot's route chain and can
	// ultimately fall back to the town temple — so a wake that "succeeded" may have placed the bot
	// on the other side of the world. Observed live: a roamer materialised at its home temple,
	// picked a destination 1120 tiles away, and was released as out-of-range 100ms later.
	//
	// The party staging code re-tests its result for exactly this reason; this is the same
	// discipline applied to the placement rather than to the candidate.
	if (roamCheb(bot.currentPos, anchor) > ROAM_STAGE_MAX_DIST + 6) {
		g_logger().info("[BotEngine] [ROAM] placement rejected for '{}': wanted ({},{},{}), got "
			"({},{},{}) — {} tiles from anchor; sending it home",
			bot.name, staging.x, staging.y, staging.z,
			bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, roamCheb(bot.currentPos, anchor));
		if (auto pl = bot.getPlayer(); pl && !isUnsafeWakeTile(bot, home)) {
			BOT_TELEPORT(pl, home, true);
			bot.currentPos = home;
			bot.lastPos = homeLast;
		}
		hibernateBot(pickGuid);
		s_roamStats.gatedNoStaging++;
		return false;
	}

	s_roamLedger[pickGuid] = RoamLedgerEntry{ nowMs };

	RoamRun run;
	run.phase = RoamPhase::WALKING;
	run.homePos = home;
	run.lastSeenPos = bot.currentPos;
	run.anchorClusterIdx = static_cast<uint32_t>(clusterIdx);
	run.startedMs = nowMs;
	run.phaseSinceMs = nowMs;
	s_roam[pickGuid] = run;

	// A recruit can wake straight back into DWELLING with a virtual dwell timer minutes out and
	// simply stand there; force it live before the first leg is assigned.
	bot.state = BotAIState::IDLE;
	bot.dwellUntil = 0;
	bot.activatedAt = 0;

	// Assign leg 1 HERE, atomically with the state change. A bare-IDLE bot with no walk target
	// falls through to the activity reroll within ten seconds and walks off to a hunt in another
	// town — the recruit would be gone before it ever roamed.
	if (!roamPickNextLeg(bot, s_roam[pickGuid])) {
		s_roam.erase(pickGuid);
		s_roamLedger.erase(pickGuid);
		bot.currentPos = home;
		bot.lastPos = homeLast;
		if (auto pl = bot.getPlayer(); pl && !isUnsafeWakeTile(bot, home)) {
			BOT_TELEPORT(pl, home, true);
		}
		hibernateBot(pickGuid);
		s_roamStats.gatedNoRegion++;
		return false;
	}

	s_roamStats.injected++;
	// Log where the bot ACTUALLY is, not where we asked for it. Reporting the requested tile hid a
	// placement that had relocated the bot across the map and made the log actively misleading.
	g_logger().info("[BotEngine] [ROAM] injected bot='{}' at ({},{},{}) (wanted ({},{},{})) "
		"anchor=({},{},{}) recruited_from={} tiles home=({},{},{})",
		bot.name, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
		staging.x, staging.y, staging.z, anchor.x, anchor.y, anchor.z,
		pickDist, home.x, home.y, home.z);
	return true;
}

// ---------------------------------------------------------------------------
// Supervisor
// ---------------------------------------------------------------------------

void BotEngine::tickAmbientRoam(int64_t nowMs) {
	const bool enabled = g_configManager().getBoolean(BOT_ROAM_ENABLE);

	// Validation runs even with no anchors and even when disabled — that is precisely when
	// "the player is gone" has to be noticed, and when a freshly-disabled feature has to let go
	// of the bots it is holding. Only INJECTION is gated on anchors.
	if (!s_roam.empty()) {
		std::vector<std::pair<uint32_t, RoamEnd>> ending;
		const int64_t sessionMax = std::max<int64_t>(30000, g_configManager().getNumber(BOT_ROAM_SESSION_MAX_MS));
		const int32_t releaseTiles = std::max(8, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_RELEASE_TILES)));

		for (auto& [guid, run] : s_roam) {
			auto idx = guidToIndex_.find(guid);
			if (idx == guidToIndex_.end()) continue;
			BotState& bot = bots_[idx->second];

			if (!enabled) { ending.push_back({guid, RoamEnd::SESSION_TTL}); continue; }

			// A fight or a conscription SUSPENDS rather than ends. Tearing the session down while
			// the bot stayed awake is what stranded reserve slots in an earlier design: the bot
			// kept consuming density but stopped being counted as a roamer.
			if (!roamSessionInvariantsHold(bot, run)) {
				if (!run.suspended) {
					run.suspended = true;
					s_roamStats.suspended++;
				}
				// Only a bot that has genuinely left (hibernated, deactivated, gone) is dropped.
				if (bot.hibernated || !bot.active) ending.push_back({guid, RoamEnd::INVARIANT});
				continue;
			}
			if (run.suspended) {
				run.suspended = false;
				s_roamStats.resumed++;
				// Re-arm: whatever took the bot cleared its walk state on the way out.
				run.phase = RoamPhase::DWELLING;
				run.dwellUntilMs = nowMs;
			}

			// A retiring session keeps walking until the bot is off somebody's screen, then goes
			// quietly. Only then can it be hibernated and put back home without a visible pop.
			if (run.retiring) {
				if (!wouldBeSeenByAnchor(bot.currentPos, ASSEMBLY_OFFSCREEN_MARGIN)) {
					ending.push_back({guid, run.retireWhy});
				} else if (nowMs - run.retireSinceMs > ROAM_RETIRE_MAX_MS) {
					// Cornered somewhere permanently in view. Give up on the tidy exit rather than
					// roam forever; endRoamSession's observed-check turns this into "leave it awake
					// in place", which is still never a pop.
					ending.push_back({guid, run.retireWhy});
				}
				continue;
			}

			if (nowMs - run.startedMs > sessionMax) {
				run.retiring = true;
				run.retireWhy = RoamEnd::SESSION_TTL;
				run.retireSinceMs = nowMs;
				continue;
			}

			// A hunt started on top of this roamer — it did not walk into the spawn, the spawn
			// arrived around it. Anchor eligibility cannot express that: the roamer may still be
			// well inside release distance of some OTHER perfectly eligible anchor, so the
			// no_anchor rule below would never fire and it would simply keep wandering the spawn
			// under a legal session. Retire it the same way, walking it off screen first.
			if (!run.retiring && roamInKeepout(bot.currentPos)) {
				run.retiring = true;
				run.retireWhy = RoamEnd::SPAWN_CLAIMED;
				run.retireSinceMs = nowMs;
				s_roamStats.evictedKeepout++;
				g_logger().info("[BotEngine] [ROAM] evicting bot='{}' at ({},{},{}) — a hunt is "
					"being worked within {} tiles", bot.name, bot.currentPos.x, bot.currentPos.y,
					bot.currentPos.z, ROAM_SUPPRESS_KEEPOUT);
				continue;
			}

			// Anchor distance, checked EVERY tick and not once per leg: a player who walks off
			// mid-leg would otherwise leave the roamer trailing a stale destination for the full
			// planner budget, which is the following behaviour this is meant to avoid.
			int32_t nearest = INT32_MAX;
			if (roamDebugAnchor_.x > 0) nearest = roamCheb(bot.currentPos, roamDebugAnchor_);
			for (const auto& a : roamAnchorPts_) {
				nearest = std::min(nearest, roamCheb(bot.currentPos, a));
			}
			if (nearest > releaseTiles) {
				// Suppression created a case NO_ANCHOR never had before. It used to mean "the player
				// walked away", so the roamer was off-screen by definition and could be retired on
				// the spot. Now an anchor can become ineligible while standing right there — the
				// moment a player claims a hunt, or a watched bot starts patrolling — and ending
				// immediately would dump every roamer on their screen as a free-agent awake bot,
				// which then rerolls normal AI and may wander into the very spawn we are protecting.
				//
				// So if anyone can still see it, walk it off first exactly like the TTL path does.
				if (wouldBeSeenByAnchor(bot.currentPos, ASSEMBLY_OFFSCREEN_MARGIN)) {
					if (!run.retiring) {
						run.retiring = true;
						run.retireWhy = RoamEnd::NO_ANCHOR;
						run.retireSinceMs = nowMs;
					}
				} else {
					ending.push_back({guid, RoamEnd::NO_ANCHOR});
				}
				continue;
			}
		}
		for (auto& [guid, why] : ending) {
			auto idx = guidToIndex_.find(guid);
			if (idx != guidToIndex_.end()) endRoamSession(bots_[idx->second], why);
			else { s_roam.erase(guid); s_roamLedger.erase(guid); }
		}
	}

	sweepRoamLedger(nowMs);
	tickRoamCastDigest(nowMs);

	// ---- 60s summary. Emitted regardless of activity so a silent feature is still legible. ----
	if (nowMs - s_lastRoamSummaryMs > 60000) {
		s_lastRoamSummaryMs = nowMs;
		if (s_roamStats.injected || !s_roam.empty() || s_roamStats.gatedDensity
			|| s_roamStats.gatedNoRecruit || s_roamStats.gatedNoStaging || s_roamStats.gatedNoRegion
			|| roamSuppressedNow_ || s_roamStats.gatedKeepout || s_roamStats.evictedKeepout) {
			g_logger().info("[BotEngine] [ROAM] active={} ledger={} injected={} legs={} arrived={} "
				"legfail={} defend={}/{} susp={}/{} | suppressed_now={} keepout: staging={} legfail={} "
				"evicted={} | gated: density={} staging={} region={} "
				"recruit={} unaccountable={} | released: no_anchor={} ttl={} failstreak={} "
				"region_gone={} invariant={} spawn_claimed={} hib_refused={} | huntRepel: flagged={} ptsPeak={} eval={} rejected={}",
				s_roam.size(), s_roamLedger.size(), s_roamStats.injected, s_roamStats.legs,
				s_roamStats.arrivals, s_roamStats.legFails, s_roamStats.defendFights,
				s_roamStats.defendKills, s_roamStats.suspended, s_roamStats.resumed,
				roamSuppressedNow_, s_roamStats.gatedKeepout, s_roamStats.legKeepoutFails,
				s_roamStats.evictedKeepout,
				s_roamStats.gatedDensity, s_roamStats.gatedNoStaging, s_roamStats.gatedNoRegion,
				s_roamStats.gatedNoRecruit, s_roamStats.gatedUnaccountable,
				s_roamStats.relNoAnchor, s_roamStats.relTtl, s_roamStats.relFailstreak,
				s_roamStats.relRegionGone, s_roamStats.relInvariant, s_roamStats.relSpawnClaimed,
				s_roamStats.hibernateRefused,
				huntRepelPts_.size(), huntRepelPtsPeak_, huntRepelEvaluated_, huntRepelRejected_);
			// Zeroed with the rest: eval=0 while pts>0 would mean the gate is never consulted,
			// and rejected=0 while eval>0 means it is consulted but never bites. Both are states
			// this feature has actually been in, and neither is visible without these two numbers.
			huntRepelEvaluated_ = 0;
			huntRepelRejected_ = 0;
			huntRepelPtsPeak_ = 0;
		}
		s_roamStats = RoamStats{};
	}

	if (!enabled) return;
	if (currentAnchors_.empty() && roamDebugAnchor_.x == 0) return;

	const int32_t reserve = std::max(0, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_RESERVE_SLOTS)));
	const int32_t perCluster = std::min(
		std::max(0, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_TARGET_PER_CLUSTER))),
		reserve > 0 ? reserve : 0);
	if (perCluster <= 0) return;
	const int32_t maxTotal = std::max(0, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_MAX_TOTAL)));
	if (maxTotal > 0 && static_cast<int32_t>(s_roam.size()) >= maxTotal) return;

	const int64_t interval = std::max<int64_t>(500, g_configManager().getNumber(BOT_ROAM_INJECT_INTERVAL_MS));
	const int32_t outerR = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_OUTER_RADIUS));

	// ONE injection per tick, server-wide. Wake bursts are what [WAKE_BURST] exists to catch.
	for (size_t ci = 0; ci < currentAnchors_.size(); ++ci) {
		const uint64_t key = botTileKey(currentAnchors_[ci].centroid);
		// Clusters are built from the UNFILTERED anchor list, so one made entirely of suppressed
		// anchors is still in this vector. Skip it before stamping a cooldown or calling the
		// injector: otherwise injectRoamBot would reach past the cluster for the nearest eligible
		// anchor anywhere on the map, charge the resulting roamer to the wrong cluster's count, and
		// pay for a cold region build far from here.
		// Membership recorded at cluster build, not a radius test: outerRadius would re-admit the
		// cross-cluster targeting this gate exists to stop, and midRadius could skip a legitimate
		// anchor at the end of an elongated union-find chain.
		if (!currentAnchors_[ci].hasRoamAnchor) continue;
		if (auto nit = s_roamNextInjectMs.find(key); nit != s_roamNextInjectMs.end() && nowMs < nit->second) {
			continue;
		}
		// Count live roamers already serving this cluster.
		int32_t here = 0;
		for (const auto& [guid, run] : s_roam) {
			auto idx = guidToIndex_.find(guid);
			if (idx == guidToIndex_.end()) continue;
			if (roamCheb(bots_[idx->second].currentPos, currentAnchors_[ci].centroid) <= outerR) here++;
		}
		if (here >= perCluster) continue;
		s_roamNextInjectMs[key] = nowMs + interval;
		if (injectRoamBot(ci, nowMs)) return;
	}
}

// ---------------------------------------------------------------------------
// Per-bot drivers
// ---------------------------------------------------------------------------

// Pre-consumption arrival hook, dispatched from doIdle at the same site tryHouseArrival uses —
// deliberately ahead of the generic planner-arrival block and while hasWalkTarget is still true.
// Running after that block would mean the walk had already been consumed, and the phase flip would
// come a tick too late to distinguish "arrived" from "lost the walk".
bool BotEngine::tryRoamArrival(BotState& bot) {
	auto it = s_roam.find(bot.guid);
	if (it == s_roam.end()) return false;
	RoamRun& run = it->second;
	if (run.phase != RoamPhase::WALKING || run.suspended) return false;
	if (bot.currentPos.z != run.dest.z) return false;
	if (roamCheb(bot.currentPos, run.dest) > POI_ARRIVAL_DIST) return false;

	const int32_t dwellMin = static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_DWELL_MIN_MS));
	const int32_t dwellMax = std::max(dwellMin, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_DWELL_MAX_MS)));

	run.phase = RoamPhase::DWELLING;
	run.phaseSinceMs = OTSYS_TIME();
	run.failStreak = 0;
	run.dwellUntilMs = OTSYS_TIME() + uniform_random(dwellMin, dwellMax);

	bot.hasWalkTarget = false;
	bot.pendingNavDest.clear();
	bot.pathFailCount = 0;
	bot.consecutivePOIFails = 0;
	bot.currentPOI = nullptr;
	s_walkTargetTimer.erase(bot.guid);
	clearPlannerWalk(bot.guid);
	bot.state = BotAIState::DWELLING;
	bot.dwellUntil = run.dwellUntilMs;

	s_roamStats.arrivals++;
	castLog(bot, fmt::format("ROAM: arrived at ({},{},{}), dwelling {}ms",
		run.dest.x, run.dest.y, run.dest.z, run.dwellUntilMs - OTSYS_TIME()));
	g_logger().info("[BotEngine] [ROAM] arrived bot='{}' seq={} at ({},{},{}) dwell={}ms",
		bot.name, run.legs, run.dest.x, run.dest.y, run.dest.z, run.dwellUntilMs - OTSYS_TIME());
	return true;
}

// WALKING-phase driver, from doIdle. Owns the tick only when it has something to say; the actual
// walking is done by the shared planner path, which is the whole point of claiming s_plannerWalk.
bool BotEngine::roamDriveWalk(BotState& bot) {
	auto it = s_roam.find(bot.guid);
	if (it == s_roam.end()) return false;
	RoamRun& run = it->second;
	if (run.suspended) return false;

	// Defense first: it must run before any check that could end the session, or a bot would be
	// released for "standing still" while it is in fact fighting for its life.
	if (tickRoamDefense(bot, run)) return true;

	if (run.phase != RoamPhase::WALKING) return false;

	// A leg dest sits within botRoamRadius of the anchor, so "twice the radius away from my own
	// destination" is comfortably outside anything walking can produce and only a teleport reaches.
	const int32_t radiusForDisplace =
		2 * std::max(4, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_RADIUS)));

	// Displacement (GM teleport, /cavebot teleport): the engine's jump detector clears the walk
	// queue but KEEPS walkTarget, so without this a displaced bot would resume walking to a
	// destination near where the anchor used to be, from anywhere on the map.
	//
	// A floor change is not displacement, and must be excluded explicitly. Stepping onto a ladder
	// or hole moves the bot to a landing that is routinely several tiles away in x/y as well as a
	// floor up or down — indistinguishable from a teleport by position alone. The first version
	// treated that as displacement and killed the session on the very first cross-floor leg, which
	// is precisely the behaviour this feature exists to produce (observed live: region_gone=6 per
	// minute, every one of them a bot that had just used a staircase).
	// Measured against bot.lastPos — the PREVIOUS ENGINE TICK — and at the same >10 threshold
	// processBot's own teleport detector uses, rather than against a roam-local snapshot.
	//
	// The first version kept its own `lastSeenPos`, which is only refreshed on ticks where doIdle
	// is dispatched and never at all during a 5-40s dwell. A level-600 bot on a mount covers far
	// more than a few tiles in that gap, so ordinary walking read as teleportation: 109 sessions in
	// 30 minutes were killed this way, each one hibernating a bot in front of the player. Zero
	// pick_fail lines in the same window is what proved it — no leg pick had failed at all, so
	// every one of those "region_gone" endings came from here.
	const int64_t nowMs = OTSYS_TIME();
	auto zIt = s_lastZChangeTime.find(bot.guid);
	const bool recentZChange = (zIt != s_lastZChangeTime.end() && nowMs - zIt->second < Z_CHANGE_GRACE_MS);
	const bool fcActive = bot.fcState != FloorChangeState::NONE
		|| recentZChange || bot.currentPos.z != bot.lastPos.z;
	if (!fcActive && roamCheb(bot.currentPos, bot.lastPos) > ROAM_DISPLACE_TILES
		&& roamCheb(bot.currentPos, run.dest) > radiusForDisplace) {
		// Genuinely teleported AND nowhere near the leg it was walking. Retire rather than end:
		// endRoamSession refuses to hibernate an observed bot, so this can never be a pop.
		castLog(bot, "ROAM: displaced — retiring session");
		run.retiring = true;
		run.retireWhy = RoamEnd::REGION_GONE;
		run.retireSinceMs = nowMs;
		return false;
	}

	// Walk-level invariants live HERE rather than in the supervisor, because both go false at a
	// healthy arrival too. Distance is what separates the two cases, and only the driver has it.
	const bool claimOurs = isPlannerWalk(bot) && bot.walkTarget == run.dest;
	if (!bot.hasWalkTarget || !claimOurs) {
		if (roamCheb(bot.currentPos, run.dest) <= POI_ARRIVAL_DIST && bot.currentPos.z == run.dest.z) {
			return false;  // arrival hook will take it
		}
		// The walk was consumed by the give-up ladder or the stale-target guard, or an admin
		// retargeted the bot. Either way this leg is over.
		s_roamStats.legFails++;
		if (!run.retiring
			&& ++run.failStreak >= std::max(1, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_MAX_FAIL_STREAK)))) {
			// Retire rather than end outright: ending hibernates the bot where it stands, and a bot
			// whose walks keep failing is usually standing in plain view when it happens. Skipped
			// while ALREADY retiring, so an in-flight walk-off keeps both its reason and its full
			// deadline instead of being relabelled by a failure it is expected to have.
			run.retiring = true;
			run.retireWhy = RoamEnd::FAILSTREAK;
			run.retireSinceMs = OTSYS_TIME();
			run.failStreak = 0;
		}
		if (!roamPickNextLeg(bot, run)) {
			// Same transient case as the dwell-phase picker: fall back to a short dwell and try
			// again rather than tearing down a session because one tick had no region budget.
			run.phase = RoamPhase::DWELLING;
			run.dwellUntilMs = OTSYS_TIME() + ROAM_LEG_RETRY_MS;
			bot.state = BotAIState::DWELLING;
			bot.dwellUntil = run.dwellUntilMs;
		}
		return true;
	}
	return false;  // walking normally — let the shared driver move it
}

// DWELLING-phase driver, from doDwelling, in the same early slot as tickFishingRun/tickHouseVisit.
// Below the depot early-returns it would starve; below the dwell tail the tail's own IDLE
// transition would fire first and hand the bot to the activity reroll.
bool BotEngine::tickRoamSession(BotState& bot) {
	auto it = s_roam.find(bot.guid);
	if (it == s_roam.end()) return false;
	RoamRun& run = it->second;
	if (run.suspended) return false;

	if (tickRoamDefense(bot, run)) return true;

	if (run.phase != RoamPhase::DWELLING) return false;
	if (OTSYS_TIME() < run.dwellUntilMs) {
		bot.dwellUntil = run.dwellUntilMs;  // hold the tail off; it is the sole authority for leaving
		return true;
	}

	if (!roamPickNextLeg(bot, run)) {
		// A failed pick is usually TRANSIENT, not terminal: the region cache expires every few
		// seconds and only one cold rebuild is allowed per tick server-wide, so with several
		// roamers a pick routinely lands on a tick where the rebuild was deferred. Ending the
		// session there churned bots through inject/release every few seconds. Retry shortly and
		// only give up once the streak says the region is genuinely unusable.
		if (!run.retiring
			&& ++run.failStreak >= std::max(1, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_MAX_FAIL_STREAK)))) {
			// Only a NON-retiring run may be ended here. Ending a retiring one on a ~4.5s failstreak
			// rather than the supervisor's 45s cap is what turned the walk-off into an abandon-in-
			// place mislabelled as region_gone: the picker legitimately fails while a bot is
			// walking itself out of view, and that failure must not be fatal.
			endRoamSession(bot, RoamEnd::REGION_GONE);
			return true;
		}
		run.dwellUntilMs = OTSYS_TIME() + ROAM_LEG_RETRY_MS;
		bot.dwellUntil = run.dwellUntilMs;
		return true;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

std::string BotEngine::buildRoamReport() {
	std::string out = fmt::format("[ROAM] enable={} reserve={} target/cluster={} maxTotal={} "
		"radius={} active={} ledger={} regions={} reachCache={}/{} hit/miss\n",
		g_configManager().getBoolean(BOT_ROAM_ENABLE) ? "true" : "false",
		g_configManager().getNumber(BOT_ROAM_RESERVE_SLOTS),
		g_configManager().getNumber(BOT_ROAM_TARGET_PER_CLUSTER),
		g_configManager().getNumber(BOT_ROAM_MAX_TOTAL),
		g_configManager().getNumber(BOT_ROAM_RADIUS),
		s_roam.size(), s_roamLedger.size(), roamRegions_.size(),
		roamReachHits_, roamReachMisses_);

	if (roamDebugAnchor_.x > 0) {
		out += fmt::format("  debug anchor: ({},{},{})\n",
			roamDebugAnchor_.x, roamDebugAnchor_.y, roamDebugAnchor_.z);
	}
	if (currentAnchors_.empty() && roamDebugAnchor_.x == 0) {
		out += "  no anchors (no real players online, no cast viewers, no debug anchor)\n";
	} else if (roamAnchorPts_.size() < currentAnchorPts_.size()) {
		// The gap between the two lists IS the suppression: anchors that still count as eyes
		// (staging avoids their screen, they hold density slots) but that roam must not target.
		// Surfaced because "roam does nothing here" and "roam is deliberately staying away" look
		// identical from the outside otherwise.
		out += fmt::format("  {} of {} anchor(s) suppressed - hunting bot mid-patrol, or a "
			"hunt-flagged player outside a town\n",
			currentAnchorPts_.size() - roamAnchorPts_.size(), currentAnchorPts_.size());
	}
	for (size_t i = 0; i < currentAnchors_.size(); ++i) {
		const auto& c = currentAnchors_[i];
		out += fmt::format("  cluster#{} centroid=({},{},{}) anchors={} counts=[{},{},{}] roam=[{},{},{}]\n",
			i, c.centroid.x, c.centroid.y, c.centroid.z, c.anchorCount,
			c.counts[0], c.counts[1], c.counts[2],
			c.roamCounts[0], c.roamCounts[1], c.roamCounts[2]);
	}
	for (const auto& [guid, run] : s_roam) {
		auto idx = guidToIndex_.find(guid);
		if (idx == guidToIndex_.end()) continue;
		const BotState& b = bots_[idx->second];
		out += fmt::format("  '{}' {} legs={} at ({},{},{}) dest=({},{},{}){}{}\n",
			b.name, run.phase == RoamPhase::WALKING ? "WALKING" : "DWELLING", run.legs,
			b.currentPos.x, b.currentPos.y, b.currentPos.z,
			run.dest.x, run.dest.y, run.dest.z,
			run.suspended ? " SUSPENDED" : "",
			run.defendTargetId ? " FIGHTING" : "");
	}
	return out;
}

// ---------------------------------------------------------------------------
// Cast-chat digest
// ---------------------------------------------------------------------------

// One roamer, compressed to something readable mid-stream.
std::string BotEngine::roamTaskBrief(const RoamRun& run) const {
	if (run.defendTargetId != 0) {
		if (auto t = g_game().getCreatureByID(run.defendTargetId)) {
			return fmt::format("fighting {}", t->getName());
		}
		return "fighting";
	}
	if (run.suspended) return "busy";
	if (run.retiring) return "leaving";
	if (run.phase == RoamPhase::WALKING) {
		return fmt::format("-> ({},{},{})", run.dest.x, run.dest.y, run.dest.z);
	}
	const int64_t left = run.dwellUntilMs - OTSYS_TIME();
	return fmt::format("idle {}s", left > 0 ? left / 1000 : 0);
}

// Tells a cast viewer who is wandering around the bot they are watching.
//
// Gated on getCastViewerCount(), NOT on verboseLog: verboseLog is the debug firehose, and this is
// meant for an ordinary viewer who never touched a command. Nothing is emitted when nobody is
// watching, so this costs a map lookup per awake bot and nothing else.
//
// Emission is change-driven with a slow heartbeat — see the note on ROAM_CAST_HEARTBEAT_MS for why
// that shape and not a fixed cadence.
void BotEngine::tickRoamCastDigest(int64_t nowMs) {
	if (s_roam.empty() && roamCastLast_.empty()) return;

	for (auto& bot : bots_) {
		if (!bot.active || bot.hibernated) continue;
		auto player = bot.getPlayer();
		if (!player || player->getRealCastViewerCount() == 0) continue; // narration needs a real viewer
		if (roamCastMuted_.count(bot.guid)) continue;

		// Roamers close enough that the viewer might actually see them.
		std::vector<std::pair<uint32_t, const RoamRun*>> nearby;
		for (const auto& [guid, run] : s_roam) {
			if (guid == bot.guid) continue;          // don't report the watched bot to itself
			auto idx = guidToIndex_.find(guid);
			if (idx == guidToIndex_.end()) continue;
			const BotState& r = bots_[idx->second];
			if (r.hibernated || !r.active) continue;
			if (roamCheb(r.currentPos, bot.currentPos) > ROAM_CAST_RADIUS) continue;
			nearby.push_back({guid, &run});
		}
		// Stable order so the hash is about membership, not map iteration order.
		std::sort(nearby.begin(), nearby.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });

		uint64_t hash = 1469598103934665603ULL;   // FNV-1a over the guid set
		for (const auto& [guid, run] : nearby) {
			hash = (hash ^ guid) * 1099511628211ULL;
		}

		auto& last = roamCastLast_[bot.guid];
		const bool rosterChanged = last.first != hash;
		const bool heartbeatDue = nowMs - last.second >= ROAM_CAST_HEARTBEAT_MS;
		if (!rosterChanged && !heartbeatDue) continue;
		// Nothing nearby and nothing was being reported: stay silent rather than announcing zero
		// every minute. The hash is still recorded so the next arrival reads as a change.
		if (nearby.empty() && !rosterChanged) { last.second = nowMs; continue; }
		last.first = hash;
		last.second = nowMs;

		if (nearby.empty()) {
			player->sendChannelMessage("Roam", "no wanderers nearby", TALKTYPE_CHANNEL_O, CHANNEL_CAST);
			continue;
		}

		std::string line = fmt::format("{} nearby: ", nearby.size());
		size_t listed = 0;
		for (const auto& [guid, run] : nearby) {
			if (listed >= ROAM_CAST_MAX_LISTED) {
				line += fmt::format("+{} more", nearby.size() - listed);
				break;
			}
			auto idx = guidToIndex_.find(guid);
			const BotState& r = bots_[idx->second];
			if (listed > 0) line += " | ";
			line += fmt::format("{} {}", r.name, roamTaskBrief(*run));
			listed++;
		}
		player->sendChannelMessage("Roam", line, TALKTYPE_CHANNEL_O, CHANNEL_CAST);
	}

	// Drop bookkeeping for bots nobody is watching any more.
	for (auto it = roamCastLast_.begin(); it != roamCastLast_.end();) {
		auto idx = guidToIndex_.find(it->first);
		bool drop = idx == guidToIndex_.end();
		if (!drop) {
			auto p = bots_[idx->second].getPlayer();
			drop = !p || p->getRealCastViewerCount() == 0; // narration needs a real viewer
		}
		it = drop ? roamCastLast_.erase(it) : std::next(it);
	}
}
