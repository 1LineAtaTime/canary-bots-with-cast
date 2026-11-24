/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

// #102 realism (batch 7): two of the epic's slices that are about what a watching player sees.
//
//  * Bed sleep (#27). A house visitor may end its visit in bed, the way a player logs out: the stock
//    BedItem::sleep moves the body onto the bed and marks the sleeper (its bot branch skips the
//    client logout a bot cannot do), and the engine then takes the body off the map by hibernating
//    it. The bed shows the bot as its sleeper for botBedSleepMinSec..MaxSec; the bot is frozen
//    (no virtual sim, no proximity wake) and then wakes ON the bed through the stock wakeUp, which
//    also applies the sleep regeneration.
//
//  * Disguised rescues (#104). A rescue teleport (teleport home, to a patrol start, party catch-up)
//    is the most visible thing a bot does that a player cannot. rescueTeleport() is the single gate
//    the rescue sites go through: unwatched, it is the old instant teleport; watched at the far end
//    only, the bot lands off-screen near the destination instead; watched where it stands, the bot
//    walks out of view first (or waits for the watcher to leave) and only then jumps. Past
//    botRescueMaxDeferSec it jumps anyway, dressed with the teleport effect of a scroll.
//    The hibernated (virtual) twin needs nothing: a virtual rescue is a position assignment that by
//    definition nobody can see.

#include "creatures/players/bot/bot_engine_impl.hpp"

#include "items/bed.hpp"
#include "map/house/house.hpp"

// ============================================================================
// #27 bed sleep
// ============================================================================

bool BotEngine::startBedSleep(BotState& bot, const Position& bedPos, uint32_t houseId, const char* why) {
	auto player = bot.getPlayer();
	auto fail = [&](const char* reason) {
		g_logger().info("[BED_SLEEP] fail bot='{}' house={} bed=({},{},{}) reason={} why={}",
			bot.name, houseId, bedPos.x, bedPos.y, bedPos.z, reason, why);
		return false;
	};
	if (!player || player->isRemoved() || bot.hibernated) return fail("no_body");
	// A watched bot cannot hibernate (hibernateBot refuses cast-watched bots), so it would lie on
	// the bed forever. Party members follow their leader.
	if (player->getCastViewerCount() > 0) return fail("cast_watched");
	if (bot.partyHuntId > 0 || bot.attackerId != 0 || dyingBots_.count(bot.guid) > 0) return fail("busy");
	if (bedSleepers_.count(bot.guid) > 0) return fail("already_sleeping");
	const auto& tile = g_game().map.getTile(bedPos);
	const auto bed = tile ? tile->getBedItem() : nullptr;
	if (!bed) return fail("no_bed");
	// The stock gate a player passes through on use: premium, a house bed, the pillow half of a
	// complete bed, and free (or ours to reclaim).
	if (!bed->canUse(player)) return fail("cannot_use");
	if (bed->getSleeper() != 0) return fail("occupied");

	if (!player->listWalkDir.empty()) {
		player->listWalkDir.clear();
		player->stopEventWalk();
	}
	if (!bed->sleep(player)) return fail("sleep_refused");

	const int64_t now = OTSYS_TIME();
	const int32_t secs = uniform_random(livenessCfg_.bedSleepMinSec,
		std::max(livenessCfg_.bedSleepMinSec, livenessCfg_.bedSleepMaxSec));
	bedSleepers_[bot.guid] = BedSleep { bedPos, houseId, now, now + secs * 1000LL, 0 };
	bot.hasWalkTarget = false;
	bot.pendingNavDest.clear();
	bot.currentPOI = nullptr;
	endHouseVisit(bot.guid, "bed_sleep");
	g_logger().info("[BED_SLEEP] start bot='{}' house={} bed=({},{},{}) sleepSec={} why={}",
		bot.name, houseId, bedPos.x, bedPos.y, bedPos.z, secs, why);
	return true;
}

bool BotEngine::bedSleepRefusesWake(const BotState& bot) const {
	if (bedWakeForce_) return false;
	const auto it = bedSleepers_.find(bot.guid);
	return it != bedSleepers_.end() && OTSYS_TIME() < it->second.untilMs;
}

bool BotEngine::bedWakePlacement(const BotState& bot, Position& placeAt) const {
	const auto it = bedSleepers_.find(bot.guid);
	if (it == bedSleepers_.end()) return false;
	placeAt = it->second.bedPos;  // a player who slept in a bed logs back in on it
	return true;
}

// Called by wakeBot (every path converges there) and by activation: the bot is in the world again,
// so if a bed still names it as the sleeper, wake it up through the stock path -- regeneration for
// the time slept, sleeper cleared, both bed halves back to the empty sprite.
void BotEngine::finishBedWake(BotState& bot, const std::shared_ptr<Player>& player, const char* path) {
	if (!player) return;
	const auto it = bedSleepers_.find(bot.guid);
	const bool tracked = it != bedSleepers_.end();
	const auto bed = g_game().getBedBySleeper(bot.guid);
	if (!tracked && !bed) return;
	int64_t sleptSec = 0;
	if (tracked) {
		sleptSec = (OTSYS_TIME() - it->second.sinceMs) / 1000;
		const Position bedPos = it->second.bedPos;
		bedSleepers_.erase(it);
		if (player->getPosition() != bedPos && !player->isRemoved()) {
			botCountTeleport("finishBedWake");
			g_game().internalTeleport(player, bedPos, false);
			bot.currentPos = bedPos;
			bot.lastPos = bedPos;
		}
	}
	if (bed) {
		bed->wakeUp(player);
	}
	// A player who logs in shows the login effect; a woken sleeper is exactly that.
	if (!player->isRemoved() && wouldBeSeenByAnchor(player->getPosition(), 0)) {
		g_game().addMagicEffect(player->getPosition(), CONST_ME_TELEPORT);
	}
	g_logger().info("[BED_SLEEP] wake bot='{}' path={} slept={}s bedCleared={} at=({},{},{})",
		bot.name, path, sleptSec, bed ? 1 : 0, player->getPosition().x, player->getPosition().y,
		player->getPosition().z);
}

std::string BotEngine::bedSleepStatus(const BotState& bot) const {
	const auto it = bedSleepers_.find(bot.guid);
	if (it == bedSleepers_.end()) {
		const auto bed = g_game().getBedBySleeper(bot.guid);
		return bed ? fmt::format("not tracked, but a bed at ({},{},{}) names this bot (stale)",
			bed->getPosition().x, bed->getPosition().y, bed->getPosition().z)
		           : "not sleeping";
	}
	const auto& s = it->second;
	const int64_t now = OTSYS_TIME();
	// bedItem = the sprite every client on screen draws (the stock transformToOnUse "sleeper" id
	// while occupied); bedSleeper = the guid the bed itself records.
	const auto& tile = g_game().map.getTile(s.bedPos);
	const auto bed = tile ? tile->getBedItem() : nullptr;
	return fmt::format("sleeping in house {} bed=({},{},{}) for {}s, wakes in {}s, hibernated={} bedItem={} bedSleeper={}",
		s.houseId, s.bedPos.x, s.bedPos.y, s.bedPos.z, (now - s.sinceMs) / 1000,
		std::max<int64_t>(0, (s.untilMs - now) / 1000), bot.hibernated ? 1 : 0,
		bed ? bed->getID() : 0, bed ? bed->getSleeper() : 0);
}

// Ends a sleep now (admin `bed wake`, or `bed wake` in a test): wakes a hibernated sleeper.
std::string BotEngine::forceBedWake(BotState& bot) {
	auto it = bedSleepers_.find(bot.guid);
	if (it == bedSleepers_.end()) return "not sleeping";
	it->second.untilMs = OTSYS_TIME();
	if (!bot.hibernated) {
		finishBedWake(bot, bot.getPlayer(), "forced_awake");
		return "woke (was still lying down)";
	}
	bedWakeForce_ = true;
	s_forceWakeGuid = bot.guid;
	s_proximityWake = false;
	const bool ok = wakeBot(bot.guid);
	bedWakeForce_ = false;
	return ok ? "woke on the bed" : "wake refused (see journal); retried by the sleep timer";
}

// ============================================================================
// #104 disguised rescues
// ============================================================================

namespace {
	int64_t s_lastRescueLogMs = 0;
	// A cast-watched bot carries its own camera: walking away or waiting cannot hide anything from
	// that viewer. It gets the #104 "teleport scroll" look instead -- a short pause, the teleport
	// effect where it stood and where it lands.
	constexpr int64_t RESCUE_CAMERA_DELAY_MS = 2500;
}

// Is `p` on the screen of any watcher OTHER than the bot's own camera? currentAnchorPts_ holds every
// real player and every cast-watched bot; a cast-watched bot is its own anchor and must not count,
// or it could never be "unwatched" anywhere.
bool BotEngine::rescueWatched(const Position& p, const Position& self, bool selfIsCamera) const {
	for (const auto& a : currentAnchorPts_) {
		if (selfIsCamera && a == self) continue;
		if (botVisibleFrom(a, p, 1)) return true;
	}
	return false;
}

ReturnValue BotEngine::rescueTeleport(BotState& bot, const std::shared_ptr<Player>& player, const Position& dest,
	bool push, const char* site) {
	if (!player) return RETURNVALUE_NOTPOSSIBLE;
	auto jump = [&](const Position& to) {
		botCountTeleport(site);
		return g_game().internalTeleport(player, to, push);
	};
	if (!livenessCfg_.rescueDisguise || bot.hibernated || player->isRemoved()) return jump(dest);
	refreshAnchorsIfStale(100);
	if (currentAnchorPts_.empty()) {
		rescueStats_.direct++;
		return jump(dest);
	}
	const Position from = player->getPosition();
	const bool selfCam = player->getCastViewerCount() > 0;
	const bool fromSeen = rescueWatched(from, from, selfCam);
	const bool destSeen = rescueWatched(dest, from, selfCam);
	if (!selfCam && !fromSeen && !destSeen) {
		rescueStats_.direct++;
		return jump(dest);
	}
	if (!selfCam && !fromSeen) {
		// Only the far end is watched: arrive off-screen near it and let the AI walk the rest, the
		// same staged walk-in a party recruit uses.
		const Position alt = chooseAssemblyStagingPos(bot, dest);
		if (alt.x != 0) {
			rescueStats_.offscreen++;
			g_logger().info("[RESCUE_TP] offscreen bot='{}' site={} dest=({},{},{}) landing=({},{},{})",
				bot.name, site, dest.x, dest.y, dest.z, alt.x, alt.y, alt.z);
			return jump(alt);
		}
	}
	// Watched where it stands (or nowhere unwatched to land): defer. The tick loop owns the bot
	// until the jump happens (tickPendingRescue), so the caller's state -- which assumes the bot
	// is already at `dest` -- is never acted on from the wrong place.
	const int64_t now = OTSYS_TIME();
	auto& pr = pendingRescues_[bot.guid];
	const bool fresh = pr.startMs == 0;
	pr.dest = dest;
	pr.push = push;
	pr.site = site;
	if (fresh) {
		pr.startMs = now;
		pr.deadlineMs = now + (selfCam ? RESCUE_CAMERA_DELAY_MS : livenessCfg_.rescueMaxDeferSec * 1000LL);
		pr.nextWalkMs = now;
		pr.walks = 0;
		rescueStats_.deferred++;
	}
	if (!player->listWalkDir.empty()) {
		player->listWalkDir.clear();
		player->stopEventWalk();
	}
	player->setFollowCreature(nullptr);
	g_logger().info("[RESCUE_TP] deferred bot='{}' site={} from=({},{},{}) dest=({},{},{}) fromSeen={} destSeen={} cast={}",
		bot.name, site, from.x, from.y, from.z, dest.x, dest.y, dest.z, fromSeen ? 1 : 0, destSeen ? 1 : 0,
		player->getCastViewerCount());
	return RETURNVALUE_NOERROR;
}

// `landing` is taken BY VALUE: two callers pass pendingRescues_[guid].dest, which the erase below
// frees -- the teleport then read a dangling reference (#217: the camera fallback logged
// landing=(51216,40192,227) and only landed right while the freed node was still intact).
void BotEngine::executePendingRescue(BotState& bot, const std::shared_ptr<Player>& player, Position landing,
	const char* how) {
	auto it = pendingRescues_.find(bot.guid);
	if (it == pendingRescues_.end()) return;
	const PendingRescue pr = it->second;
	pendingRescues_.erase(it);
	if (!player->listWalkDir.empty()) {
		player->listWalkDir.clear();
		player->stopEventWalk();
	}
	const Position from = player->getPosition();
	const bool fallback = std::string_view(how) == "fallback";
	if (fallback) {
		g_game().addMagicEffect(from, CONST_ME_TELEPORT);
	}
	botCountTeleport(pr.site);
	g_game().internalTeleport(player, landing, pr.push);
	if (fallback) {
		g_game().addMagicEffect(landing, CONST_ME_TELEPORT);
	}
	bot.currentPos = player->getPosition();
	bot.lastPos = bot.currentPos;
	if (fallback) rescueStats_.fallback++; else rescueStats_.hidden++;
	g_logger().info("[RESCUE_TP] done bot='{}' site={} how={} waitedMs={} walks={} from=({},{},{}) landing=({},{},{}) seenNow={}",
		bot.name, pr.site, how, OTSYS_TIME() - pr.startMs, pr.walks, from.x, from.y, from.z,
		landing.x, landing.y, landing.z, wouldBeSeenByAnchor(from, 0) ? 1 : 0);
}

// One tick of a deferred rescue. Returns true while it owns the bot's tick (its AI is skipped).
bool BotEngine::tickPendingRescue(BotState& bot) {
	auto it = pendingRescues_.find(bot.guid);
	if (it == pendingRescues_.end()) return false;
	auto player = bot.getPlayer();
	if (!player || player->isRemoved() || dyingBots_.count(bot.guid) > 0) {
		dropPendingRescue(bot.guid, "no_body");
		return false;
	}
	// Attacked while waiting: fighting back matters more than the rescue; the AI re-rescues later
	// if it is still stuck.
	if (bot.attackerId != 0) {
		dropPendingRescue(bot.guid, "attacked");
		return false;
	}
	auto& pr = it->second;
	const int64_t now = OTSYS_TIME();
	bot.currentPos = player->getPosition();
	refreshAnchorsIfStale(250);
	const bool selfCam = player->getCastViewerCount() > 0;

	if (!selfCam && !rescueWatched(bot.currentPos, bot.currentPos, false)) {
		Position landing = pr.dest;
		if (rescueWatched(landing, bot.currentPos, false)) {
			const Position alt = chooseAssemblyStagingPos(bot, pr.dest);
			if (alt.x != 0) {
				landing = alt;
			} else if (now < pr.deadlineMs) {
				return true;  // nowhere unwatched to land yet: keep waiting
			}
		}
		executePendingRescue(bot, player, landing, pr.walks > 0 ? "walked" : "waited");
		return true;
	}
	if (now >= pr.deadlineMs) {
		executePendingRescue(bot, player, pr.dest, "fallback");
		return true;
	}

	// Walk out of view, away from the nearest watcher. A cast-watched bot carries its camera with it,
	// so walking cannot help: it just waits for the deadline.
	if (!selfCam && player->listWalkDir.empty() && now >= pr.nextWalkMs) {
		pr.nextWalkMs = now + 1500;
		const Position& me = bot.currentPos;
		const Position* nearest = nullptr;
		int32_t best = INT32_MAX;
		for (const auto& a : currentAnchorPts_) {
			const int32_t d = std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(me.x)),
				std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(me.y)));
			if (d < best) { best = d; nearest = &a; }
		}
		int32_t sx = 0, sy = 0;
		if (nearest) {
			sx = (me.x > nearest->x) ? 1 : (me.x < nearest->x ? -1 : 0);
			sy = (me.y > nearest->y) ? 1 : (me.y < nearest->y ? -1 : 0);
		}
		if (sx == 0 && sy == 0) { sx = uniform_random(0, 1) ? 1 : -1; }
		// Straight away first, then the two 45-degree turns, then the two sideways ones.
		const int32_t cand[5][2] = { { sx, sy }, { sx + sy, sy - sx }, { sx - sy, sy + sx }, { sy, -sx }, { -sy, sx } };
		constexpr int32_t kAwayTiles = 11;
		for (const auto& c : cand) {
			const int32_t dx = std::clamp(c[0], -1, 1), dy = std::clamp(c[1], -1, 1);
			if (dx == 0 && dy == 0) continue;
			Position target = me;
			target.x = static_cast<uint16_t>(std::clamp<int32_t>(me.x + dx * kAwayTiles, 1, 65000));
			target.y = static_cast<uint16_t>(std::clamp<int32_t>(me.y + dy * kAwayTiles, 1, 65000));
			std::vector<Direction> dirs;
			if (player->getPathTo(target, dirs, 0, 3, true, true, 14) && !dirs.empty()) {
				player->startAutoWalk(dirs, false, Creature::WalkStartPolicy::ImmediateWhenReady);
				pr.walks++;
				break;
			}
		}
	}
	if (now - s_lastRescueLogMs >= 5000) {
		s_lastRescueLogMs = now;
		g_logger().info("[RESCUE_TP] waiting bot='{}' site={} waitedMs={} walks={} at=({},{},{})",
			bot.name, pr.site, now - pr.startMs, pr.walks, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
	}
	return true;
}

// Hibernation is the moment nobody is looking: finish a pending rescue with the plain jump, so the
// body that goes into the pool is where the caller meant it to be.
void BotEngine::flushPendingRescue(BotState& bot, const char* why) {
	auto it = pendingRescues_.find(bot.guid);
	if (it == pendingRescues_.end()) return;
	auto player = bot.getPlayer();
	if (!player || player->isRemoved()) {
		dropPendingRescue(bot.guid, why);
		return;
	}
	executePendingRescue(bot, player, it->second.dest, why);
}

void BotEngine::dropPendingRescue(uint32_t guid, const char* why) {
	auto it = pendingRescues_.find(guid);
	if (it == pendingRescues_.end()) return;
	g_logger().info("[RESCUE_TP] dropped guid={} site={} reason={} waitedMs={}", guid, it->second.site, why,
		OTSYS_TIME() - it->second.startMs);
	rescueStats_.dropped++;
	pendingRescues_.erase(it);
}

// ============================================================================
// Shared once-a-tick driver (called from tick() before the per-bot loop)
// ============================================================================

void BotEngine::tickRealismTimers() {
	if (bedSleepers_.empty()) return;
	const int64_t now = OTSYS_TIME();
	if (now < nextBedTickMs_) return;
	nextBedTickMs_ = now + 500;

	std::vector<uint32_t> toHibernate, toWake, toDrop;
	for (const auto& [guid, s] : bedSleepers_) {
		auto idx = guidToIndex_.find(guid);
		if (idx == guidToIndex_.end() || !bots_[idx->second].active) { toDrop.push_back(guid); continue; }
		const BotState& bot = bots_[idx->second];
		if (!bot.hibernated) {
			// Lying down: the walk event of BedItem::sleep has put the body on the bed (or is about
			// to). Give it a moment, then take the body off the map like a logout.
			const auto p = bot.getPlayer();
			if (p && (p->getPosition() == s.bedPos || now - s.sinceMs >= 1500)) toHibernate.push_back(guid);
		} else if (now >= s.untilMs && now >= s.retryMs) {
			toWake.push_back(guid);
		}
	}
	for (uint32_t guid : toDrop) {
		bedSleepers_.erase(guid);
	}
	for (uint32_t guid : toHibernate) {
		auto idx = guidToIndex_.find(guid);
		if (idx == guidToIndex_.end()) continue;
		BotState& bot = bots_[idx->second];
		const auto player = bot.getPlayer();
		const Position at = player ? player->getPosition() : Position();
		if (!hibernateBot(guid)) {
			// Refused (cast viewer joined, party, dying): get up again rather than lie there awake.
			g_logger().info("[BED_SLEEP] abort bot='{}' reason=hibernate_refused", bot.name);
			if (auto bit = bedSleepers_.find(guid); bit != bedSleepers_.end()) bit->second.untilMs = now;
			finishBedWake(bot, player, "hib_refused");
			continue;
		}
		g_logger().info("[BED_SLEEP] asleep bot='{}' at=({},{},{}) onBed={}", bot.name, at.x, at.y, at.z,
			(bedSleepers_.count(guid) && at == bedSleepers_[guid].bedPos) ? 1 : 0);
	}
	for (uint32_t guid : toWake) {
		auto sit = bedSleepers_.find(guid);
		if (sit == bedSleepers_.end()) continue;
		sit->second.retryMs = now + 5000;
		bedWakeForce_ = true;
		s_proximityWake = false;  // a timed wake is not a proximity wake: no off-screen relocation
		const bool ok = wakeBot(guid);
		bedWakeForce_ = false;
		if (!ok) {
			g_logger().info("[BED_SLEEP] wake-deferred guid={} (wake refused, retry in 5s)", guid);
		}
	}
}

std::string BotEngine::realismStatus() const {
	std::string out = fmt::format("[REALISM] bedSleepers={} rescuePending={} rescue: direct={} offscreen={} deferred={} hidden={} fallback={} dropped={}\n",
		bedSleepers_.size(), pendingRescues_.size(), rescueStats_.direct, rescueStats_.offscreen,
		rescueStats_.deferred, rescueStats_.hidden, rescueStats_.fallback, rescueStats_.dropped);
	const int64_t now = OTSYS_TIME();
	for (const auto& [guid, s] : bedSleepers_) {
		out += fmt::format("  sleeper guid={} house={} bed=({},{},{}) sleptS={} wakeInS={}\n", guid, s.houseId,
			s.bedPos.x, s.bedPos.y, s.bedPos.z, (now - s.sinceMs) / 1000, std::max<int64_t>(0, (s.untilMs - now) / 1000));
	}
	for (const auto& [guid, pr] : pendingRescues_) {
		out += fmt::format("  rescue guid={} site={} dest=({},{},{}) waitedMs={} walks={}\n", guid, pr.site,
			pr.dest.x, pr.dest.y, pr.dest.z, now - pr.startMs, pr.walks);
	}
	return out;
}
