/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_shrine.cpp — BOT_SHRINE_IDLE: awake bots stand in front of a shrine.
//
// An awake bot in a town rolls a chance to walk to the nearest daily reward shrine or imbuing
// shrine, stops on a reserved tile beside it, turns to face it, and idles there for a minute or
// four — glancing away and back the way somebody reading an inscription does. When the window
// ends it rerolls its next activity through the ordinary reroll.
//
// PURELY COSMETIC. The bot never uses either shrine, and that is not laziness: both actions are
// client-only. daily_reward_shrine.lua calls DailyReward.loadDailyReward, which sends a modal
// window, and imbuement_shrine.lua calls player:openImbuementWindow — a bot has no client, so
// the first would be a no-op and the second needs an item target it has no way to choose. There
// is nothing to gain from firing them and a live Lua path to break.
//
// DISCOVERY IS A RUNTIME SCAN, not an index. findNearbyShrines (bot_waypoint.cpp) is a near-clone
// of findReachableDepotLocker and inherits its z-band, its cross-z distance penalty, its
// free-non-FC-adjacency scoring and its demote-don't-exclude ranking. The alternative — harvesting
// shrines during the map sweep — would have cost a ZCACHE_VERSION bump and a ~21s synchronous
// dispatcher freeze on the next boot to derive facts this scan gets for free. The scan is memoized
// per town for the life of the engine, because a shrine is map furniture: it cannot move while the
// engine lives, and `/cavebot reload` builds a fresh engine and therefore a fresh memo.
//
// AWAKE BOTS ONLY, like fishing / house visits / NPC visits. The virtual simulator never runs the
// arrival handler, so a hibernated bot would win the roll, hold a stand tile, and never actually
// go. Letting hibernated bots hold a shrine needs a virtual dwell-end teardown that does not exist
// (virtualAdvanceDwelling is a pure timer and processBot never runs for them), so s_shrineRuns
// would leak permanently and poison every isShrineVisiting predicate. Deliberately deferred.
//
// ARRIVAL FOLLOWS THE HOUSE PATTERN, NOT THE FISHING ONE. A shrine visit reserves ONE exact tile
// so the bot can face the furniture. Fishing tolerates the generic 3-tile POI arrival because it
// re-scans live water and carries a 20s approach grace; neither exists here, and a visit that
// started three tiles out would have the bot facing the shrine through a wall. So the run is
// created at selection time in APPROACH, an early hook in doIdle (gated on the run, not on
// currentPOI, because a forced `/cavebot shrine` has no POI) does the exact-tile test, and a
// settle deadline ends the visit honestly rather than pretending it arrived.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

// ---------------------------------------------------------------------------
// Claims
// ---------------------------------------------------------------------------
//
// Keyed by stand tile, carrying (guid, kind) in the value. The kind is what makes the release
// per-kind: ONE reroll offers both a reward and an imbuing candidate and therefore takes two
// claims, so a guid-only release fired when one kind loses the weighted roll would hand back the
// winning kind's tile as well. Read-filtered on expiresAt, so an exit path that somehow misses
// endShrineVisit costs a TTL rather than a permanently consumed tile.

bool BotEngine::isShrineTileClaimed(const Position& tile, uint32_t byGuid) const {
	auto it = s_shrineTileClaims.find(botTileKey(tile));
	if (it == s_shrineTileClaims.end()) return false;
	if (it->second.guid == byGuid) return false;
	return it->second.expiresAt > OTSYS_TIME();
}

// How many bots currently hold a stand tile at THIS shrine. Counts live claims only, and excludes
// the asking bot so a re-pick by the same bot is not blocked by its own claim.
uint32_t BotEngine::shrineOccupantCount(const Position& shrine, uint32_t excludeGuid) const {
	const int64_t now = OTSYS_TIME();
	uint32_t n = 0;
	for (const auto& [guid, run] : s_shrineRuns) {
		if (guid == excludeGuid) continue;
		if (run.shrine != shrine) continue;
		auto it = s_shrineTileClaims.find(botTileKey(run.stand));
		if (it != s_shrineTileClaims.end() && it->second.expiresAt <= now) continue;
		n++;
	}
	return n;
}

void BotEngine::claimShrineSpot(uint32_t guid, uint8_t kind, const ShrineSpot& spot) {
	s_shrineTileClaims[botTileKey(spot.stand)] =
		ShrineClaim { guid, kind, OTSYS_TIME() + SHRINE_CLAIM_MS };
}

void BotEngine::releaseShrineClaim(uint32_t guid, uint8_t kind) {
	std::erase_if(s_shrineTileClaims, [&](const auto& kv) {
		return kv.second.guid == guid && kv.second.kind == kind;
	});
}

void BotEngine::releaseShrineClaims(uint32_t guid) {
	std::erase_if(s_shrineTileClaims, [&](const auto& kv) { return kv.second.guid == guid; });
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// Memo for one town, filling it on first use. BOTH anchors are scanned and merged, and that is
// load-bearing rather than thorough: the measured "every town has a shrine within 1-14 tiles" is a
// distance to the NEARER of its depot and temple, and a shrine 14 tiles from the temple can be 25+
// from the depot. Scanning one anchor, at any radius, does not deliver the coverage.
//
// The negative result is cached too (`scanned` is set unconditionally). Without it, Bounac and
// Feyrist — which have no shrine within 140-206 tiles — and Carlin, whose only near reward shrine
// sits two floors below its anchors, would re-run a ~5.8k-tile scan on every reroll that passed
// the gate, forever.
const ShrineMemo& BotEngine::shrineMemoForTown(uint32_t townId) {
	auto it = s_shrineMemo.find(townId);
	if (it != s_shrineMemo.end() && it->second.scanned) {
		return it->second;
	}

	const int64_t t0 = botMonoMs();
	ShrineMemo memo;
	memo.scanned = true;

	const auto& allPOIs = getCityPOIs();
	auto poiIt = allPOIs.find(townId);
	if (poiIt != allPOIs.end()) {
		for (const auto& poi : poiIt->second) {
			if (poi.type != POIType::DEPOT && poi.type != POIType::TEMPLE) continue;
			ShrineScanResult res;
			if (!findNearbyShrines(poi.pos, SHRINE_SCAN_RADIUS, res)) continue;
			for (int32_t k = 0; k < 2; k++) {
				// First anchor to find a kind wins it. The anchors are a few tiles apart and both
				// scans rank by the same penalised distance, so there is nothing to gain from
				// comparing across them — and picking "closest to which anchor?" would be a
				// meaningless question when the bot is walking from somewhere else entirely.
				if (res.found[k] && !memo.found[k]) {
					memo.spot[k] = res.spot[k];
					memo.found[k] = true;
				}
			}
			if (memo.found[0] && memo.found[1]) break;
		}
	}

	g_logger().info("[SHRINE_SCAN] town={} reward={} imbuing={} in {} ms",
		townId,
		memo.found[0] ? fmt::format("({},{},{})", memo.spot[0].shrine.x, memo.spot[0].shrine.y,
			memo.spot[0].shrine.z) : "none",
		memo.found[1] ? fmt::format("({},{},{})", memo.spot[1].shrine.x, memo.spot[1].shrine.y,
			memo.spot[1].shrine.z) : "none",
		botMonoMs() - t0);

	return s_shrineMemo[townId] = memo;
}

// P1: one bot-centred live scan per reroll, both kinds at once.
//
// This tier exists because the town-anchor memo alone was measurably the wrong answer: a bot
// standing TWO tiles from a shrine was sent 21 tiles across Thais to the temple one, because the
// shrine it was next to sits 22 tiles from the town anchor and therefore outside the memo's
// radius. The requirement was always "the closest".
void BotEngine::findLocalShrines(const BotState& bot, ShrineScanResult& out) const {
	// Timed for the same reason as houseShrineStands: this is new per-gated-reroll work inside the
	// per-bot tick, and "is it the P1 scan?" should be answerable from the journal rather than
	// argued from dispatcher cycle lines.
	const int64_t t0 = botMonoMs();
	findNearbyShrines(bot.currentPos, SHRINE_LOCAL_RADIUS, out);
	if (const int64_t dt = botMonoMs() - t0; dt > 20) {
		g_logger().warn("[SHRINE_P1SCAN_SLOW] guid={} at=({},{},{}) radius={} {}ms",
		                bot.guid, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
		                SHRINE_LOCAL_RADIUS, dt);
	}
}

// Candidate for the POI roll. Commits NOTHING — claiming is selectNextPOI's job, so a candidate
// that loses the weighted roll leaves nothing behind. Same discipline as pickHouseVisit.
bool BotEngine::selectShrineSpot(const BotState& bot, uint8_t kind,
                                 const ShrineScanResult& local, ShrineSpot& out) {
	ShrineSpot spot;

	if (local.found[kind - 1]) {
		// P1 hit: take it and DISCARD P2 outright for this kind. Copied from the fishing ladder's
		// invariant — if any candidate is in local range the distant ones are thrown away, because
		// "a player uses the shrine in front of them" rather than walking across town past it.
		spot = local.spot[kind - 1];
	} else {
		// P2: the per-town memo. Never wrong, just incomplete — it only ever knew about shrines
		// near a town's depot or temple.
		//
		// findNearestTown, NOT bot.townId. The NPC visit switched for a documented reason: a
		// home-town lookup once sent a bot standing in Thais to an NPC 560 tiles away because its
		// home town said 15. There is deliberately NO home-town fallback either — a bot in Bounac
		// must be told "no shrine here", never handed another town's answer.
		const uint32_t townId = findNearestTown(bot.currentPos);
		if (townId == 0) return false;
		const ShrineMemo& memo = shrineMemoForTown(townId);
		if (!memo.found[kind - 1]) return false;
		spot = memo.spot[kind - 1];
	}

	// A shrine this bot already failed to reach. Required rather than a nicety: the giveup path
	// inserts the POI name into visitedPOIs and self-suppresses for a generation, but the 240s
	// stale-target teardown does NOT touch visitedPOIs, so without this a shrine that paths
	// plausibly and never completes would be re-picked by the same bot indefinitely.
	if (isShrineBlacklisted(bot.guid, spot.shrine)) return false;

	// The shrine is within ~15 of a town anchor, but the BOT can be arbitrarily far from that
	// anchor while findNearestTown still returns the town. Without this the only bound on the walk
	// is the 240s stale teardown — a self-limiting four-minute failure loop.
	const int32_t cheb = std::max(
		std::abs(static_cast<int32_t>(spot.stand.x) - static_cast<int32_t>(bot.currentPos.x)),
		std::abs(static_cast<int32_t>(spot.stand.y) - static_cast<int32_t>(bot.currentPos.y)));
	if (cheb > SHRINE_OFFER_MAX_CHEB) return false;

	if (shrineOccupantCount(spot.shrine, bot.guid)
	        >= static_cast<uint32_t>(std::max(1, livenessCfg_.shrineMaxOccupants))) {
		return false;
	}

	// The memo holds furniture position only; walkability and occupancy are point-in-time facts
	// and are re-checked live on every selection.
	auto standTile = g_game().map.getTile(spot.stand);
	if (!standTile || !standTile->getGround()) return false;
	if (standTile->hasFlag(TILESTATE_BLOCKSOLID) || standTile->hasFlag(TILESTATE_FLOORCHANGE)
	        || standTile->hasFlag(TILESTATE_TELEPORT)) {
		return false;
	}
	if (standTile->getTopCreature()) return false;

	out = spot;
	return true;
}

// ---------------------------------------------------------------------------
// Arrival + the visit
// ---------------------------------------------------------------------------

// Returns true when it consumed the tick. Mirrors tryHouseArrival: returns FALSE while the bot is
// still on its way, so the walk continues, and the settle deadline is armed on first contact with
// the stand tile's neighbourhood rather than at walk start (the walk itself is already bounded by
// the planner's own 240s stale-target budget).
bool BotEngine::tryShrineArrival(BotState& bot) {
	auto it = s_shrineRuns.find(bot.guid);
	if (it == s_shrineRuns.end() || it->second.phase != ShrinePhase::APPROACH) {
		return false;
	}
	auto& run = it->second;

	const bool onTile = bot.currentPos == run.stand;
	if (!onTile) {
		// Arm the deadline once the bot is close enough that the last steps are the only thing
		// left — same role as the house version's isInsideRunHouse gate. Before that, this simply
		// lets the walk continue.
		const int32_t cheb = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(run.stand.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(run.stand.y)));
		if (run.settleUntil == 0 && cheb <= 3 && bot.currentPos.z == run.stand.z) {
			run.settleUntil = OTSYS_TIME()
				+ std::max(1, livenessCfg_.shrineSettleSec) * 1000LL;
		}
		if (run.settleUntil != 0 && OTSYS_TIME() >= run.settleUntil) {
			// Could not close the last tiles. End honestly rather than starting a visit from
			// wherever the bot stalled — a bot "facing" a shrine it is three tiles and a wall away
			// from is worse than no visit at all. Blacklisted so the next reroll picks something
			// else instead of walking the same four minutes again.
			blacklistShrine(bot.guid, run.shrine);
			endShrineVisit(bot.guid, "settle_timeout");
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			clearPlannerWalk(bot.guid);
			return true;
		}
		return false;
	}

	startShrineVisit(bot);
	bot.hasWalkTarget = false;
	bot.pendingNavDest.clear();
	bot.currentPOI = nullptr;
	bot.pathFailCount = 0;
	bot.consecutivePOIFails = 0;
	clearPlannerWalk(bot.guid);
	return true;
}

void BotEngine::startShrineVisit(BotState& bot) {
	auto it = s_shrineRuns.find(bot.guid);
	if (it == s_shrineRuns.end()) return;
	auto& run = it->second;
	auto player = bot.getPlayer();

	const int32_t lo = std::max(5, livenessCfg_.shrineIdleMinSec);
	const int32_t hi = std::max(lo, livenessCfg_.shrineIdleMaxSec);
	const int32_t secs = uniform_random(lo, hi);

	run.phase = ShrinePhase::IDLE;
	run.until = OTSYS_TIME() + secs * 1000LL;
	run.nextGlanceMs = OTSYS_TIME() + uniform_random(8, 25) * 1000LL;

	// Hold DWELLING open past the idle window. doDwelling's tail is the sole authority for leaving
	// DWELLING and knows nothing about this run, so the margin has to come from here. +5000 is
	// right where fishing needs +30000: fishing's margin covers a 20s approach grace and a return
	// leg, and a shrine visit has neither — its settle deadline lives in APPROACH, before `until`
	// is ever rolled. endShrineVisit normalizes this back down; nothing else would, because
	// virtualAdvanceDwelling is a pure timer.
	bot.dwellUntil = run.until + 5000;
	bot.state = BotAIState::DWELLING;

	if (player) {
		g_game().internalCreatureTurn(player, shrineFacingDir(run.stand, run.shrine));
	}
	castLog(bot, fmt::format("SHRINE: standing at the {} shrine for {}s",
		run.kind == SHRINE_KIND_REWARD ? "reward" : "imbuing", secs));
	g_logger().info("[SHRINE_VISIT_ARRIVE] guid={} kind={} shrine=({},{},{}) idle={}s",
		bot.guid, run.kind == SHRINE_KIND_REWARD ? "reward" : "imbuing",
		run.shrine.x, run.shrine.y, run.shrine.z, secs);
}

// true = consumed the tick. Called from doDwelling's early slot beside tickFishingRun and
// tickHouseVisit, ahead of the unconditional early-returns below them.
bool BotEngine::tickShrineVisit(BotState& bot) {
	auto it = s_shrineRuns.find(bot.guid);
	if (it == s_shrineRuns.end()) return false;
	auto& run = it->second;
	if (run.phase != ShrinePhase::IDLE) return false; // APPROACH is a planner walk, not a dwell

	const int64_t now = OTSYS_TIME();

	// Displacement guard. A bot moved off its tile by anything else — a GM teleport, a party
	// conscription that raced the teardown, a push — is no longer looking at anything.
	if (bot.currentPos != run.stand) {
		endShrineVisit(bot.guid, "displaced");
		return false;
	}

	// Recomputed, never incremented: an incremented dwellUntil would outlive a stopped run and
	// strand the bot, whereas recomputing lets the timer expire naturally the moment this stops
	// being called.
	bot.dwellUntil = run.until + 5000;

	if (now >= run.until) {
		endShrineVisit(bot.guid, "dwell_end");
		return false; // let doDwelling's tail reroll normally
	}

	if (now >= run.nextGlanceMs) {
		run.nextGlanceMs = now + uniform_random(8, 25) * 1000LL;
		if (auto player = bot.getPlayer()) {
			// Mostly look back at the shrine; occasionally glance elsewhere. A bot frozen on one
			// facing for four minutes reads as furniture itself.
			static const Direction kDirs[4] = { DIRECTION_NORTH, DIRECTION_EAST,
				DIRECTION_SOUTH, DIRECTION_WEST };
			const Direction dir = (uniform_random(1, 100) <= 70)
				? shrineFacingDir(run.stand, run.shrine)
				: kDirs[uniform_random(0, 3)];
			g_game().internalCreatureTurn(player, dir);
		}
	}
	return true;
}

// Idempotent — every teardown site calls it unconditionally.
void BotEngine::endShrineVisit(uint32_t guid, const char* reason) {
	auto it = s_shrineRuns.find(guid);
	if (it == s_shrineRuns.end()) return;
	const uint8_t kind = it->second.kind;
	const Position shrine = it->second.shrine;
	s_shrineRuns.erase(it);
	releaseShrineClaim(guid, kind);
	clearPlannerWalk(guid);

	// Normalize the stretched dwellUntil back down. Nothing else would: virtualAdvanceDwelling is
	// a pure timer, and bot_party.cpp's recruitment snapshot copies dwellUntil verbatim, so a
	// value left at run.until + 5s would strand the bot for the rest of the window doing nothing.
	for (auto& bot : bots_) {
		if (bot.guid != guid) continue;
		bot.dwellUntil = OTSYS_TIME() + uniform_random(3, 8) * 1000LL;
		break;
	}

	g_logger().info("[SHRINE_VISIT_END] guid={} kind={} shrine=({},{},{}) reason={}",
		guid, kind == SHRINE_KIND_REWARD ? "reward" : "imbuing",
		shrine.x, shrine.y, shrine.z, reason);
}

// ---------------------------------------------------------------------------
// Debug commands
// ---------------------------------------------------------------------------

// `/cavebot <bot> shrine [reward|imbuing]` — force a visit now, bypassing botShrineVisitPct and
// the TABLE B roll. Mirrors `/cavebot <bot> fish` and `/cavebot <bot> house`: the walk has no
// currentPOI, which is exactly why tryShrineArrival is gated on the run instead.
std::string BotEngine::forceShrineVisit(BotState& bot, const std::string& arg) {
	std::string which = arg;
	std::transform(which.begin(), which.end(), which.begin(), ::tolower);

	uint8_t kind = 0;
	if (which.empty() || which == "reward") kind = SHRINE_KIND_REWARD;
	else if (which == "imbuing" || which == "imbue") kind = SHRINE_KIND_IMBUING;
	else return "usage: shrine [reward|imbuing]";

	if (bot.hibernated) return "bot is hibernated — wake it first";
	// A stopped bot skips the whole AI state machine (`if (bot.aiPaused) continue;`), so it can
	// never execute a walk. Reporting "walking to X" at it is a lie that costs an operator a real
	// debugging session: it looks exactly like a targeting bug, because the target IS printed and
	// the bot then stands still. Refuse instead.
	if (bot.aiPaused) {
		return "bot is STOPPED (aiPaused) — it cannot walk anywhere until `resume`. "
		       "Run `/cavebot <bot> resume` first.";
	}

	// A forced visit clears this bot's blacklist first: the operator asking for a specific shrine
	// wants it retried, not silently skipped because an earlier attempt timed out.
	clearShrineBlacklist(bot.guid);
	endShrineVisit(bot.guid, "forced_restart");

	// The forced command runs the same two-tier lookup the autonomous path does, so what it
	// demonstrates is the production behaviour with the roll removed.
	ShrineScanResult localShrines;
	findLocalShrines(bot, localShrines);

	// "Go to the closest shrine" has to mean the closest one, including a shrine standing in a
	// house. The shrine POI still refuses house tiles — that walk owns no door handling, no tile
	// claim, no occupancy cap and no exit planner — so rather than target it directly, DELEGATE
	// to a house visit, which owns all four.
	//
	// This exists because the location split, correct as it is internally, was wrong from the
	// outside: an operator standing beside a house shrine ran `shrine imbuing` and watched the bot
	// walk 22 tiles to the temple. The machinery split stays; what changes is that the command no
	// longer pretends the near shrine is not there.
	// Filled by either the boxed-in detection or a house refusal; appended to whatever this
	// command ends up answering.
	std::string blockedNote;
	auto chebFromBot = [&](const Position& p) {
		return std::max(
			std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(bot.currentPos.x)),
			std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(bot.currentPos.y)));
	};
	if (localShrines.houseFound[kind - 1]) {
		const int32_t houseDist = chebFromBot(localShrines.houseSpot[kind - 1].stand);
		// Beaten only by a non-house shrine that is genuinely nearer. P2's memo target is always
		// further than P1's radius, so a P1 house hit wins against it by construction.
		const bool plainCloser = localShrines.found[kind - 1]
			&& chebFromBot(localShrines.spot[kind - 1].stand) <= houseDist;
		if (!plainCloser) {
			const uint32_t hid = localShrines.houseId[kind - 1];
			const Position& sh = localShrines.houseSpot[kind - 1].shrine;
			// The EXPLICIT house id, never the mode-only form: `house shrine` searches the town
			// for any qualifying house, which on another day is a different house than the one
			// two tiles away. Delegating hands over claims and occupancy wholesale, so nothing is
			// claimed here.
			const std::string houseMsg = forceHouseVisit(bot, fmt::format("{} shrine {}", hid,
				kind == SHRINE_KIND_REWARD ? "reward" : "imbuing"));
			// forceHouseVisit answers with "House visit:" only on success; anything else is a
			// refusal (not bot-owned, at botHouseMaxOccupants, boxed in). Fall back to the nearest
			// non-house shrine and SAY WHY, rather than erroring out on a command that has a
			// perfectly good second answer.
			if (houseMsg.rfind("House visit:", 0) == 0) {
				return fmt::format("that {} shrine at ({},{},{}) is inside house {} — starting a "
					"house visit instead, which owns the door and the claims. {}",
					kind == SHRINE_KIND_REWARD ? "reward" : "imbuing",
					sh.x, sh.y, sh.z, hid, houseMsg);
			}
			// Surface it in the RETURN value as well as castLog: castLog reaches cast viewers and
			// verboseLog bots only, so an operator running this over the command queue saw a
			// silent 30-tile walk to the temple with no hint that a nearer shrine had been tried
			// and rejected.
			blockedNote = fmt::format(" NOTE: there IS a {} shrine at ({},{},{}) in house {}, {} "
				"tiles away, but it could not be used: {}",
				kind == SHRINE_KIND_REWARD ? "reward" : "imbuing",
				sh.x, sh.y, sh.z, hid, chebFromBot(localShrines.houseSpot[kind - 1].stand),
				houseMsg);
			castLog(bot, fmt::format("SHRINE: house {} refused ({}), falling back", hid, houseMsg));
		}
	}

	// A nearby shrine that exists but cannot be stood beside is the single most confusing outcome
	// this command has: the bot walks 30 tiles to the temple while the operator is standing next
	// to one. Name it.
	// ADDITIVE, not either/or: the two notes answer different questions. The house-refusal note
	// says why the nearest usable-looking candidate was rejected; the boxed-in note says why a
	// shrine the operator can SEE was never a candidate at all. Reporting only the first one names
	// a shrine across the street while the one they are standing beside goes unexplained.
	if (localShrines.blockedFound[kind - 1]) {
		const Position& b = localShrines.blockedPos[kind - 1];
		blockedNote += fmt::format(" NOTE: a {} shrine at ({},{},{}), {} tiles away, is BOXED IN — "
			"all four of its cardinal tiles are blocked by wall or furniture, so no bot can stand "
			"facing it.",
			kind == SHRINE_KIND_REWARD ? "reward" : "imbuing", b.x, b.y, b.z, chebFromBot(b));
	}

	ShrineSpot spot;
	if (!selectShrineSpot(bot, kind, localShrines, spot)) {
		const uint32_t townId = findNearestTown(bot.currentPos);
		const ShrineMemo& memo = shrineMemoForTown(townId);
		return fmt::format("no {} shrine available (town={} scanned={} found={} — "
			"try `/cavebot shrines` for the full picture).{}",
			kind == SHRINE_KIND_REWARD ? "reward" : "imbuing", townId,
			memo.scanned ? "yes" : "no", memo.found[kind - 1] ? "yes" : "no", blockedNote);
	}

	claimShrineSpot(bot.guid, kind, spot);
	s_shrineRuns[bot.guid] = ShrineRun { spot.shrine, spot.stand, kind,
		ShrinePhase::APPROACH, 0, 0, 0 };

	bot.walkTarget = spot.stand;
	bot.hasWalkTarget = true;
	bot.currentPOI = nullptr;
	bot.pendingNavDest.clear();
	bot.pathFailCount = 0;
	bot.followingCityRoute = false;
	bot.state = BotAIState::IDLE;
	// The shrine sits inside a depot or temple building, behind a door the generic walker cannot
	// open — the same reason the NPC/water/house targets take the scoped planner.
	s_plannerWalk[bot.guid] = bot.walkTarget;

	g_logger().info("[SHRINE_VISIT_START] guid={} kind={} shrine=({},{},{}) stand=({},{},{}) forced",
		bot.guid, kind == SHRINE_KIND_REWARD ? "reward" : "imbuing",
		spot.shrine.x, spot.shrine.y, spot.shrine.z, spot.stand.x, spot.stand.y, spot.stand.z);

	return fmt::format("walking to the {} shrine at ({},{},{}), standing on ({},{},{}) — dist {}{}",
		kind == SHRINE_KIND_REWARD ? "reward" : "imbuing",
		spot.shrine.x, spot.shrine.y, spot.shrine.z,
		spot.stand.x, spot.stand.y, spot.stand.z,
		std::max(std::abs(static_cast<int32_t>(spot.stand.x) - static_cast<int32_t>(bot.currentPos.x)),
		         std::abs(static_cast<int32_t>(spot.stand.y) - static_cast<int32_t>(bot.currentPos.y))),
		blockedNote);
}

// `/cavebot shrines [<townId>]` — FORCE-FILL every town's memo and dump it, so the whole picture
// is one command rather than something that accretes as bots happen to visit towns. This is what
// gets diffed against the offline OTBM scan.
std::string BotEngine::describeShrines(const std::string& arg) {
	std::string out = "Shrines (runtime scan, radius " + std::to_string(SHRINE_SCAN_RADIUS)
		+ ", z+-1, both town anchors merged)\n";

	uint32_t onlyTown = 0;
	if (!arg.empty()) {
		try { onlyTown = static_cast<uint32_t>(std::stoul(arg)); } catch (...) { onlyTown = 0; }
	}

	std::vector<uint32_t> towns;
	for (const auto& [townId, pois] : getCityPOIs()) {
		if (onlyTown != 0 && townId != onlyTown) continue;
		(void)pois;
		towns.push_back(townId);
	}
	std::sort(towns.begin(), towns.end());

	uint32_t haveReward = 0, haveImbuing = 0;
	for (uint32_t townId : towns) {
		const ShrineMemo& memo = shrineMemoForTown(townId);
		out += fmt::format("  town {:>3}: ", townId);
		for (int32_t k = 0; k < 2; k++) {
			const char* label = (k == 0) ? "reward" : "imbuing";
			if (!memo.found[k]) {
				out += fmt::format("{}=none  ", label);
				continue;
			}
			(k == 0 ? haveReward : haveImbuing)++;
			const auto& s = memo.spot[k];
			const uint32_t occ = shrineOccupantCount(s.shrine, 0);
			out += fmt::format("{}=({},{},{})/stand({},{},{})/occ{}  ",
				label, s.shrine.x, s.shrine.y, s.shrine.z,
				s.stand.x, s.stand.y, s.stand.z, occ);
		}
		out += "\n";
	}
	out += fmt::format("  -- {} towns: {} with a reward shrine, {} with an imbuing shrine.\n",
		towns.size(), haveReward, haveImbuing);
	out += fmt::format("  -- active visits: {}\n", s_shrineRuns.size());
	return out;
}
