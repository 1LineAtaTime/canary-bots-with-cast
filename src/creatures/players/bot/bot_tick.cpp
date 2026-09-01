/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_tick.cpp — main tick, per-bot dispatch, IDLE/DWELLING, v2 virtual sim, floor changes
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
// PZ-blocked roaming (Feature 2) — per-bot mill-around timer.
// ============================================================================





// ============================================================================
// Waypoint type parser
// ============================================================================



// Walk-on FC/teleport: tile has FLOORCHANGE or TELEPORT flag — server handles z-transition.
// FLOORCHANGE: stairs, open holes/trapdoors (queryDestination handles)
// TELEPORT: ventilation grilles, slime slides, magic forcefields (action scripts handle)






// ============================================================================
// Player-proximity weighting helpers (2026-06-15)
// Bias hibernated bots' next task/location toward real-player / cast-watched-bot
// anchors (currentAnchorPts_) so the virtual simulator funnels ambient traffic
// toward players. Distances are z-agnostic Chebyshev (matches hibernation/density).
// ============================================================================

// Min Chebyshev distance from p to the nearest anchor. -1 when there are no anchors
// (callers take the uniform fast path before calling, but -1 is handled defensively).
int32_t BotEngine::minChebToAnchor(const Position& p) const {
	int32_t best = -1;
	for (const auto& a : currentAnchorPts_) {
		const int32_t d = std::max(std::abs(static_cast<int32_t>(p.x) - static_cast<int32_t>(a.x)),
		                           std::abs(static_cast<int32_t>(p.y) - static_cast<int32_t>(a.y)));
		if (best < 0 || d < best) best = d;
	}
	return best;
}

// Tiered additive weight bonus for a candidate whose nearest anchor is minCheb tiles away.
int32_t BotEngine::proximityBonus(int32_t minCheb) const {
	if (minCheb < 0) return 0;                                  // no anchors
	if (minCheb <= livenessCfg_.proxNearTiles) return livenessCfg_.proxBonusNear;
	if (minCheb <= livenessCfg_.proxMidTiles)  return livenessCfg_.proxBonusMid;
	return 0;
}

// Min anchor distance over a capped, strided sample of a hunt's patrol waypoints
// (always includes first + last). Returns -1 if no patrol waypoints or no anchors.
int32_t BotEngine::sampledMinChebForScript(const HuntScript& s) const {
	const auto& wps = s.patrolWaypoints;
	const size_t n = wps.size();
	if (n == 0) return -1;
	const size_t cap = static_cast<size_t>(std::max(2, livenessCfg_.proxSampleCap));  // >=2 → stride safe
	const size_t stride = (n <= cap) ? 1 : (n - 1) / (cap - 1);
	int32_t best = -1;
	for (size_t i = 0; i < n; i += stride) {
		const int32_t d = minChebToAnchor(wps[i].pos);
		if (d >= 0 && (best < 0 || d < best)) best = d;
	}
	const int32_t dLast = minChebToAnchor(wps[n - 1].pos);     // always include last
	if (dLast >= 0 && (best < 0 || dLast < best)) best = dLast;
	return best;
}

// Min anchor distance to a town's POI footprint (depot/temple/boat/NPCs); falls back
// to the town temple. Returns -1 when there are no anchors.
int32_t BotEngine::minChebToTown(uint32_t townId) {
	if (currentAnchorPts_.empty()) return -1;
	int32_t best = -1;
	auto& allPOIs = getCityPOIs();
	auto it = allPOIs.find(townId);
	if (it != allPOIs.end()) {
		for (const auto& poi : it->second) {
			const int32_t d = minChebToAnchor(poi.pos);
			if (d >= 0 && (best < 0 || d < best)) best = d;
		}
	}
	if (best < 0) {
		if (auto town = g_game().map.towns.getTown(townId)) {
			best = minChebToAnchor(town->getTemplePosition());
		}
	}
	return best;
}

// Weighted index pick in [0,n). Valid index for n>=1; all-zero weights degrade to
// uniform; total capped at INT32_MAX for uniform_random(int,int).
size_t BotEngine::weightedPick(const std::vector<int32_t>& w) {
	const size_t n = w.size();
	if (n == 0) return 0;                                       // defensive; callers guard n>=1
	int64_t total = 0;
	for (int32_t x : w) total += (x > 0 ? x : 0);
	if (total <= 0) return static_cast<size_t>(uniform_random(0, static_cast<int32_t>(n) - 1));
	// overflow guard; cap<total is dead in practice (max total « INT32_MAX)
	const int32_t cap = static_cast<int32_t>(std::min<int64_t>(total, INT32_MAX));
	const int32_t roll = uniform_random(1, cap);
	int64_t cum = 0;
	for (size_t i = 0; i < n; ++i) {
		cum += (w[i] > 0 ? w[i] : 0);
		if (roll <= cum) return i;
	}
	return n - 1;                                               // rounding tail
}

// Tally a weighted selection by the chosen candidate's anchor distance tier, and emit a
// rate-limited per-selection [PROXBIAS] line (1/sec) so we can confirm the bias is firing.
void BotEngine::recordProxSelection(int32_t chosenDist, const char* kind, uint32_t guid, const std::string& pick) {
	if (chosenDist < 0 || chosenDist > livenessCfg_.proxMidTiles) s_proxSelFar++;
	else if (chosenDist <= livenessCfg_.proxNearTiles) s_proxSelNear++;
	else s_proxSelMid++;
	const int64_t now = OTSYS_TIME();
	if (now - s_lastProxBiasLogMs >= 1000) {
		s_lastProxBiasLogMs = now;
		g_logger().info("[PROXBIAS] kind={} guid={} pick='{}' minCheb={} anchors={}",
			kind, guid, pick, chosenDist, currentAnchorPts_.size());
	}
}

// Resolves a percent-of-botPlayersOnline config key to a whole-bot limit
// (truncated, per design: 2.5% of 500 = 12.5 -> 12). The +1e-3 epsilon guards
// float32 key storage rounding the pct DOWN (e.g. 0.7f = 0.69999998..., so
// 0.7% of 1000 would floor to 6 instead of 7); it is far too small to promote
// a genuinely fractional product.

bool BotEngine::shouldGateWake(uint32_t guid) {
	// Single-shot force exemption for explicit wakes (cast-viewer login, partyhunt
	// support assembly, /cavebot wake). Consumed unconditionally — even on guid
	// mismatch — so a set flag can never outlive one gate decision and leak the
	// bypass to an unrelated proximity wake. See s_forceWakeGuid declaration.
	const bool forceWake = (s_forceWakeGuid != 0 && s_forceWakeGuid == guid);
	// BOT_AMBIENT_ROAM: read BOTH single-shot flags and reset BOTH before either early return.
	// Consuming the roam flag after the forceWake return would let a forced wake carry a stranded
	// roam flag past this decision and into an unrelated one — and the Lua proximity loop fires up
	// to five wakes every 300ms, so "unrelated" arrives almost immediately.
	const bool roamWake = (s_roamWakeGuid != 0 && s_roamWakeGuid == guid);
	s_forceWakeGuid = 0;
	s_roamWakeGuid = 0;
	if (forceWake) return false;

	// Cascade exemption: party-leader cascade wakes never count against the cap. The
	// cap shapes WHICH party wakes (via the leader's gate decision); never which
	// members. Otherwise a party of 5 hitting an inner cap of 3 would split — leader
	// + 2 members wake, members 3-4 stay hibernated, and Party::create runs on a
	// partial roster. See Sonnet review in PERF_INVESTIGATION_2026-05-24.md.
	if (s_inPartyCascade) return false;  // false = "do not gate" (mirror of the boolean inversion below)

	// PARTY_HUNT exemption (2026-06-14): never density-gate a party member's wake. With the
	// hibernateBot lifecycle coupling, members normally only wake via the leader cascade
	// (already exempt above), so this is defense-in-depth for the residual independent paths:
	// admin mass-wake (wakeAllHibernatedBots), a member left hibernated at reload, or any
	// future per-member wake. Members must wake WITH their party, not be split by a dense
	// inner ring around the EK (which is exactly what blocked re-wakes in the oscillation bug).
	{
		auto pit = guidToIndex_.find(guid);
		if (pit != guidToIndex_.end() && bots_[pit->second].partyHuntId > 0) return false;
	}

	// Master flag: if disabled, all wakes go through. Telemetry still fires elsewhere
	// (refreshAnchorsIfStale runs unconditionally so [DENSITY] periodic logs continue).
	if (!g_configManager().getBoolean(BOT_DENSITY_CAP_ENABLED)) return false;

	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;  // invalid guid, let wakeBot handle
	BotState& bot = bots_[it->second];

	// Set LRU timestamp unconditionally — every wake attempt (grant or skip) rotates
	// this bot to the back of the LRU queue so cap-locked dead zones can't form.
	const int64_t now = OTSYS_TIME();
	bot.lastWakeAttemptMs = now;

	if (currentAnchors_.empty()) return false;  // no anchors → no cap to enforce

	// BOT_LIVENESS_PACK Phase A.4 (refined Fix #12): AdvStone island density cap.
	// Only chest (mode 1) and training-dummy (mode 2) bots are EXEMPT — they
	// legitimately cluster on the same tiles and shouldn't get capped. Other
	// island bots (idling at waypoints, dwelling) go through the cap normally.
	// Density count in refreshAnchorsIfStale also EXCLUDES chest/dummy bots so
	// they don't crowd out the legitimate cluster.
	if (isOnAdvStoneIsland(bot.currentPos)
	    && (bot.advStoneDwellMode == 1 || bot.advStoneDwellMode == 2)) {
		return false;
	}

	const int32_t innerRadius = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_INNER_RADIUS));
	const int32_t midRadius   = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_MID_RADIUS));
	const int32_t outerRadius = static_cast<int32_t>(g_configManager().getNumber(BOT_DENSITY_CAP_OUTER_RADIUS));
	const uint32_t innerLimit = pctOfBotTotal(BOT_DENSITY_CAP_INNER_LIMIT_PCT);
	const uint32_t midLimit   = pctOfBotTotal(BOT_DENSITY_CAP_MID_LIMIT_PCT);
	const uint32_t outerLimit = pctOfBotTotal(BOT_DENSITY_CAP_OUTER_LIMIT_PCT);

	// outerLimitPct=0 → hard no-wake zone beyond midRadius of EVERY anchor: liveness
	// concentrates within midRadius of where players actually are. Measured against
	// RAW anchor positions, not cluster centroids — per-cluster band checks dead-zone
	// bots adjacent to a second player 51-100 tiles away, and merged-cluster centroid
	// displacement both over- and under-gates (2026-06-12 review blocker). This check
	// is also independent of outerRadius containment, closing the "bot >outerRadius
	// from every centroid wakes ungated" hole. Inner/mid caps below stay centroid-
	// based and unchanged; their cumulative outer check is disabled at outerLimit==0
	// (counts[2] >= 0 is always true and would gate every wake — liveness blackout).
	// Known approximation (accepted): in an elongated merged cluster a bot can be
	// within midRadius of an EDGE anchor but outside midRadius of the centroid — the
	// band rule passes it and the centroid-based mid cap can't see it, so the
	// effective per-area awake ceiling is soft in multi-anchor chains. Dominant
	// single-player case is exact (centroid == player).
	// Band mode keys on the RAW configured pct, not the truncated outerLimit —
	// a nonzero pct that floors to 0 bots at small botPlayersOnline (3.0% below
	// ~34 total) must NOT silently enable the hard band; it just disables the
	// cumulative outer check below (the outerLimit > 0 guard).
	const bool hardBand = g_configManager().getFloat(BOT_DENSITY_CAP_OUTER_LIMIT_PCT) <= 0.0f;
	if (hardBand) {
		int32_t minAnchorCheb = INT32_MAX;
		for (const auto& ap : currentAnchorPts_) {
			const int32_t adx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(ap.x));
			const int32_t ady = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(ap.y));
			minAnchorCheb = std::min(minAnchorCheb, std::max(adx, ady));
		}
		if (minAnchorCheb > midRadius) {
			// Rate-limited 60s per 50-tile area of the BOT (steady-state condition,
			// not an anomaly — the proximity loop retries band bots forever). High
			// bit distinguishes band keys from centroid cap_hit keys in the shared map.
			constexpr int32_t LOG_KEY_QUANT = 50;
			const uint64_t key = (1ULL << 63)
				| (static_cast<uint64_t>(bot.currentPos.x / LOG_KEY_QUANT) << 32)
				| (static_cast<uint64_t>(bot.currentPos.y / LOG_KEY_QUANT) << 8)
				| static_cast<uint64_t>(bot.currentPos.z);
			int64_t& lastLog = s_lastDensityCapHitLogMs[key];
			if (now - lastLog > 60000) {
				lastLog = now;
				g_logger().info("[DENSITY] band_gate guid={} bot_pos=({},{},{}) minAnchorCheb={} midRadius={} (outerLimitPct=0 no-wake band)",
					guid, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					minAnchorCheb, midRadius);
			}
			return true;  // gate this wake
		}
	}

	// Pre-flight pass: check whether ANY cluster the bot falls into is at-or-above cap
	// in ANY ring it's in. If so, gate. Innermost-first so the [DENSITY] log shows the
	// tightest binding constraint.
	for (auto& cluster : currentAnchors_) {
		const int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cluster.centroid.x));
		const int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cluster.centroid.y));
		const int32_t cheb = std::max(dx, dy);
		if (cheb > outerRadius) continue;  // bot not in this cluster's outer ring

		// BOT_AMBIENT_ROAM: roamers get botRoamReserveSlots EXTRA slots per ring that only they
		// may use, so an ambient bot never competes with an ordinary proximity wake.
		//
		//   organic: (counts - roamCounts) >= limit          -> gate
		//   roam:    roamCounts >= reserve                   -> gate  (reserve is full)
		//            OR counts >= limit + reserve            -> gate  (ring is at its true ceiling)
		//
		// The SUBTRACTION on the organic arm is what makes the reserve an addition rather than a
		// redistribution: without it three roamers standing inside the inner ring would push
		// counts to the base limit and start gating the very organic wakes the reserve was
		// supposed to leave untouched. Attribution is by cluster centroid for both arrays and both
		// arms — mixing that with nearest-anchor geometry would index one array under two
		// different partitions.
		const uint32_t reserve = roamWake
			? static_cast<uint32_t>(std::max<int64_t>(0, g_configManager().getNumber(BOT_ROAM_RESERVE_SLOTS)))
			: 0u;
		auto organic = [&](int ring) -> uint32_t {
			// Clamped: an underflow here would wrap to ~4 billion and silently disable the cap.
			return cluster.counts[ring] > cluster.roamCounts[ring]
				? cluster.counts[ring] - cluster.roamCounts[ring] : 0u;
		};
		auto gateRing = [&](int ring, uint32_t limit) -> bool {
			if (!roamWake) return organic(ring) >= limit;
			return cluster.roamCounts[ring] >= reserve || cluster.counts[ring] >= limit + reserve;
		};

		const char* gatedRing = nullptr;
		uint32_t gatedCount = 0, gatedLimit = 0;
		if (cheb <= innerRadius && gateRing(0, innerLimit)) {
			gatedRing = "inner"; gatedCount = cluster.counts[0]; gatedLimit = innerLimit + reserve;
		} else if (cheb <= midRadius && gateRing(1, midLimit)) {
			gatedRing = "mid"; gatedCount = cluster.counts[1]; gatedLimit = midLimit + reserve;
		} else if (outerLimit > 0 && gateRing(2, outerLimit)) {  // bot is in outer by construction
			gatedRing = "outer"; gatedCount = cluster.counts[2]; gatedLimit = outerLimit + reserve;
		}

		if (gatedRing) {
			// Per-cluster 5s rate-limit on cap_hit log. Phase B.1 fix (2026-06-01):
			// quantize centroid to 50-tile buckets (= botDensityAnchorClusterRadius)
			// so anchor drift doesn't reset the rate-limit slot. Pre-fix observed
			// 482 distinct exact centroids over 6h (one anchor walking through a
			// city drifted across 62 distinct centroids in one logical cluster),
			// effectively defeating the 5s limit and producing 342 sub-5s log
			// violations. Quantization collapses that to ~37 logical slots — one
			// per visited city area — preserving meaningful per-area diagnostics.
			constexpr int32_t LOG_KEY_QUANT = 50;
			const uint64_t key = (static_cast<uint64_t>(cluster.centroid.x / LOG_KEY_QUANT) << 32)
				| (static_cast<uint64_t>(cluster.centroid.y / LOG_KEY_QUANT) << 8)
				| static_cast<uint64_t>(cluster.centroid.z);
			int64_t& lastLog = s_lastDensityCapHitLogMs[key];
			if (now - lastLog > 5000) {
				lastLog = now;
				g_logger().info("[DENSITY] cap_hit guid={} roam={} bot_pos=({},{},{}) cluster=({},{},{}) ring={} count={} cap={} counts=[{},{},{}] roamCounts=[{},{},{}]",
					guid, roamWake ? "yes" : "no",
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					cluster.centroid.x, cluster.centroid.y, cluster.centroid.z,
					gatedRing, gatedCount, gatedLimit,
					cluster.counts[0], cluster.counts[1], cluster.counts[2],
					cluster.roamCounts[0], cluster.roamCounts[1], cluster.roamCounts[2]);
			}
			return true;  // gate this wake
		}
	}

	// All cluster checks passed. Reserve the slot: increment counters for every cluster
	// ring this bot falls into. Done BEFORE wakeBot returns so back-to-back wakes in the
	// same dispatcher event see the updated counters.
	for (auto& cluster : currentAnchors_) {
		const int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(cluster.centroid.x));
		const int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(cluster.centroid.y));
		const int32_t cheb = std::max(dx, dy);
		if (cheb > outerRadius) continue;
		cluster.counts[2]++;
		// A roam grant reserves in BOTH arrays, mirroring how refreshAnchorsIfStale counts a
		// ledgered bot. Reserving only in counts would leave the reserve looking free until the
		// next anchor refresh and let a burst overshoot the ceiling.
		if (roamWake) cluster.roamCounts[2]++;
		if (cluster.counts[2] > cluster.peakCounts[2]) cluster.peakCounts[2] = cluster.counts[2];
		if (cheb <= midRadius) {
			cluster.counts[1]++;
			if (roamWake) cluster.roamCounts[1]++;
			if (cluster.counts[1] > cluster.peakCounts[1]) cluster.peakCounts[1] = cluster.counts[1];
		}
		if (cheb <= innerRadius) {
			cluster.counts[0]++;
			if (roamWake) cluster.roamCounts[0]++;
			if (cluster.counts[0] > cluster.peakCounts[0]) cluster.peakCounts[0] = cluster.counts[0];
		}
	}
	return false;  // do not gate
}

uint32_t BotEngine::wakeBotsInRadius(const Position& pos, int radius) {
	// Pre-wake hook from Game::internalTeleport. Snapshot guids first to avoid iterator
	// invalidation if wakeBot's party-leader cascade mutates bots_/hibernationPool_.
	// beginWakeBurst clears the per-burst tile reservations so chooseWakePosition can
	// spread bots that share the same virtualPos to adjacent free tiles.
	beginWakeBurst();

	// Phase B.1 fix (2026-06-01): FORCE-INVALIDATE the anchor cache before refreshing.
	// This call always represents a positional discontinuity (player just teleported via
	// boat NPC, scroll, ladder, death-to-temple, /goto, etc.). A 50ms stale-cache check
	// is wrong here: if BotEngine::tick refreshed <50ms before the teleport, the cluster
	// is cached at the OLD player position. The new candidates at the destination fall
	// outside the old cluster's outer ring → shouldGateWake's "no matching cluster" branch
	// returns "do not gate" for every candidate → cap fully bypassed for the burst.
	// Verified at 14:45:29: 37 wakes at Ab'Dendriel with zero cap_hit logs, while
	// cluster_dissolve fired 0.2s later for the Thais centroid 350 tiles away — proving
	// the cache was holding the pre-teleport cluster during the burst. The explicit
	// `anchorsRefreshedAt_ = 0` is belt-and-braces (refreshAnchorsIfStale(0) alone is
	// sufficient since `(now - 0) < 0` is always false), but makes the cache-poisoning
	// intent obvious at the call site.
	anchorsRefreshedAt_ = 0;
	refreshAnchorsIfStale(0);

	std::vector<uint32_t> guids;
	guids.reserve(16);
	for (const auto &bot : bots_) {
		if (!bot.hibernated) continue;
		// Z-agnostic Chebyshev (matches bot_hibernation.lua anyRealPlayerNear semantics).
		const int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(pos.x));
		const int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(pos.y));
		if (dx <= radius && dy <= radius) {
			guids.push_back(bot.guid);
		}
	}

	// Phase B: LRU sort — wake the bot with the OLDEST lastWakeAttemptMs first so a
	// repeated wake burst at the same anchor rotates through all candidates instead of
	// always picking the same ones (cap-locked dead zones).
	std::sort(guids.begin(), guids.end(), [this](uint32_t a, uint32_t b) {
		const auto itA = guidToIndex_.find(a);
		const auto itB = guidToIndex_.find(b);
		if (itA == guidToIndex_.end() || itB == guidToIndex_.end()) return false;
		return bots_[itA->second].lastWakeAttemptMs < bots_[itB->second].lastWakeAttemptMs;
	});
	// JITTER DIAGNOSTIC: reset burst accumulators, set callsite, log totals at exit.
	int64_t jitter_burstStart = botMonoMs(); // JITTER FIX: real clock (cached read froze wall= at 0)
	s_wakeBurstAccumMs = 0;
	s_wakeBurstMaxSingleMs = 0;
	s_wakeBurstCount = 0;
	s_wakeBurstCallSite = "teleport";  // wakeBotsInRadius is only called from internalTeleport
	// Publish the arrival point so wakeBot can give the bots the player can actually SEE a short
	// quiet window instead of the full anti-cascade stagger. Scoped to this loop and cleared at exit,
	// mirroring s_wakeBurstCallSite: single-wake paths (the 300ms Lua monitor, /cavebot wake, cast
	// login) must keep their existing behaviour rather than read a stale centre.
	s_burstCenter = pos;
	s_burstCenterValid = true;
	uint32_t count = 0;
	for (uint32_t guid : guids) {
		// TELEPORT burst: a real player just teleported in and gets a full map redraw,
		// so "pop-in" is expected — skip off-screen relocation + login sparkle, and keep
		// the full GUID quiet stagger (anti-cascade for up-to-100-tile-radius mass wakes).
		s_proximityWake = false;
		if (wakeBot(guid)) ++count;
	}
	int64_t jitter_burstWall = botMonoMs() - jitter_burstStart;
	if (s_wakeBurstAccumMs > 20 || jitter_burstWall > 20) {
		g_logger().warn("[WAKE_BURST] count={} total={}ms wall={}ms maxSingle={}ms callSite={}",
			s_wakeBurstCount, s_wakeBurstAccumMs, jitter_burstWall,
			s_wakeBurstMaxSingleMs, s_wakeBurstCallSite);
	}
	s_wakeBurstCallSite = "proximity";  // reset for future single-wake paths
	s_burstCenterValid = false;         // stale centre must never leak into single-wake paths
	botPerf_.wakesTeleport += count; // PERF HARNESS: burst wakes, separated from proximity wakes
	return count;
}

void BotEngine::logHuntAssign(const BotState& bot, uint32_t scriptId) const {
	// Find the script. ~200 entries, linear scan is fine — fires at ~0.1/sec server-wide.
	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == scriptId) { script = &s; break; }
	}
	if (!script) return;
	// Use first patrol waypoint as the "spawn coords" — user can /teleport there to watch.
	Position spawnPos = script->patrolWaypoints.empty() ? Position() : script->patrolWaypoints[0].pos;
	std::string townName;
	if (auto town = g_game().map.towns.getTown(script->townId); town) {
		townName = town->getName();
	} else {
		townName = std::to_string(script->townId);
	}
	g_logger().info("[HuntAssign] {} (lv{} voc{}) -> '{}' (id={} town={}) spawn=({},{},{})",
		bot.name, bot.cachedLevel, static_cast<int>(bot.vocationId),
		script->name, scriptId, townName,
		spawnPos.x, spawnPos.y, spawnPos.z);
}

bool BotEngine::recoverOrphanForReload(uint32_t guid, const std::shared_ptr<Player>& player) {
	// Reload-recovery for hibernated bots whose Player object survived the dlclose
	// (because Lua's BotPlayers table holds a strong shared_ptr) but whose BotState
	// + hibernationPool entry were destroyed with the old engine.
	//
	// Bring the Player back to a clean "in-world, not active, not hibernated" state
	// so that the standard reactivateBotForReload teleport-to-POI flow can run on it
	// uniformly. After this returns true, the caller (Lua executeReload) treats the
	// bot like any other newly-registered bot.
	//
	// Skip if a BotState for this guid already exists — botReregisterAll may have
	// covered this bot if it was awake (not hibernated) at reload time.
	if (guidToIndex_.find(guid) != guidToIndex_.end()) {
		return true; // already registered, nothing to do
	}
	if (!player) {
		g_logger().warn("[BotEngine] recoverOrphanForReload: null player for guid={}", guid);
		return false;
	}

	// Restore the Player to "live creature" state — these flags were set during
	// hibernate's removeCreature path and need clearing before re-attach to g_game.
	player->setNotRemoved();
	player->clearTileParent();

	// Re-add to g_game()'s player map so getPlayerByName/getPlayers find it again.
	g_game().addPlayer(player);
	player->setOnline(true);

	// Silently place at the bot_manager.lua staging tile. No spectators there →
	// no packet emission, no onCreatureAppear hook fires. The Player is now in
	// the world but invisible to clients.
	const Position stagingPos(31970, 32283, 7);
	if (!g_game().internalPlaceCreature(player, stagingPos, false, true)) {
		g_logger().warn("[BotEngine] recoverOrphanForReload: failed to place '{}' at staging tile",
			player->getName());
		// Roll back addPlayer to keep g_game().players_ consistent
		g_game().removePlayer(player);
		player->setOnline(false);
		return false;
	}

	// Create a fresh BotState in the new engine. registerBot initializes
	// active=false, hibernated=false (line 1953) — the standard "newly registered,
	// awaiting activation" state. Lua's reactivateBotForReload loop will then
	// teleport the bot to depot/boat/temple/PZ via the random distribution.
	registerBot(player);

	g_logger().info("[BotEngine] recoverOrphanForReload: '{}' (guid={}) re-attached at staging, awaiting reactivation",
		player->getName(), guid);
	return true;
}


// ============================================================================
// Adventurer's Stone trip (POIType::ADVENTURER_STONE) — shared constants
//
// MOVED to the top of bot_engine.cpp (Phase 4) so the v2 virtual simulator's
// `virtualAdvanceIdle` can reach `findAdventurerStoneTownAt` and `kAdventurerStoneDest`.
// Anonymous-namespace symbols are only visible after their definition in the
// translation unit, so this block must precede any caller.
//
// Mimics data-otservbr-global/scripts/actions/adventurers_guild/adventurers_stone.lua:
//   1. Bot at any temple PZ uses item 16277 → teleported to (32210, 32300, 6)
//   2. Bot tours the dungeon following the route loaded in adventurerStoneRoute_
//   3. Dwells once at a random non-stairs node waypoint for 5-30 min
//   4. Steps onto magic forcefield at (32210, 32292, 6) → server-side aid:4253 MoveEvent
//      (data-otservbr-global/scripts/movements/teleport/adventurers_guild.lua) reads
//      the storage value set in step 1 and teleports the bot back to that town's temple.
//
// We mimic the Lua action handler directly in C++ (set storage + teleport) rather than
// invoke the engine's USE_WITH path, because the stone is a "use self" Action(:id) — not
// a "use with target" — and useItemEx() does not dispatch to onUse for self-target items.
// ============================================================================


// ============================================================================
// v2 Virtual Simulator (BOT_HIBERNATION_V2.md)
// ----------------------------------------------------------------------------
// Runs every ~30s for hibernated bots. Advances position + state-machine
// fields purely in C++ memory — no spells, mana, healing, combat, damage,
// death, loot, items, party formation, NPCs, tile interactions. Live AI
// resumes seamlessly from advanced state when bot is woken.
// ============================================================================


// Phase 6: index-based virtual progression. Advance the waypoint index by
// `elapsed_ms / VIRTUAL_MS_PER_WAYPOINT` and snap the bot's position to the
// resulting waypoint. Returns the new index, clamped to size (caller checks
// `newIdx >= size` for phase transition / route exhaustion).
//
// Replaces the prior distance-interpolation model. The simpler index-based
// model has two key advantages: (1) wake position is always exactly a script
// waypoint (always reachable, semantically meaningful) rather than a possibly-
// invalid interpolated mid-segment tile; (2) trip duration is predictable
// (waypoint_count × 5s) regardless of segment geometry — ideal for travel
// routes with mixed short/long segments (e.g., city walks + boat teleports).
size_t BotEngine::advanceWaypointIdx(uint32_t guid, size_t currentIdx, size_t totalSize, int64_t elapsed_ms) {
	// 2026-06-10 fix: was `steps = elapsed_ms / 5000` which integer-floored to 0 whenever
	// per-bot elapsed_ms < 5000ms (the observed steady state at ~100ms per visit), freezing
	// the index after the single first-visit step (5000ms fallback at virtualTick lastMs==0).
	// Now accumulates fractional progress in waypointLeftoverMs_[guid]; 50 consecutive 100ms
	// visits = 5000ms total = 1 wp advance, matching the designed 1 wp / 5s wall-clock speed.
	// Leftover persists across virtualTick visits AND across hunt-phase transitions; the
	// inter-phase bleed (up to 4999ms head-start) is cleared in virtualAdvanceHunting at the
	// HUNT:VT transition site so first-wp accuracy on a new phase is restored.
	if (totalSize == 0 || elapsed_ms <= 0) return currentIdx;
	int64_t &leftover = waypointLeftoverMs_[guid];
	int64_t total = leftover + elapsed_ms;
	size_t steps = static_cast<size_t>(total / VIRTUAL_MS_PER_WAYPOINT);
	leftover = total % VIRTUAL_MS_PER_WAYPOINT;
	if (steps == 0) return currentIdx;
	return std::min(currentIdx + steps, totalSize);
}

// Advance bot through bot.cityRouteWps by one virtual tick. Returns the status
// the caller needs to act on:
//   NotActive     — no route currently loaded; caller proceeds with its own logic
//   StillWalking  — index advanced, currentPos snapped to new waypoint, caller MUST return
//   JustCompleted — route exhausted this tick; cityRouteWps/Idx/followingCityRoute cleared.
//                   Caller MUST perform its own phase transition before returning.
BotEngine::VirtualRouteStatus BotEngine::virtualTickCityRoute(BotState &bot, int64_t elapsed_ms) {
	if (!bot.followingCityRoute || bot.cityRouteWps.empty()) {
		return VirtualRouteStatus::NotActive;
	}
	size_t newIdx = advanceWaypointIdx(bot.guid, bot.cityRouteIdx, bot.cityRouteWps.size(), elapsed_ms);
	bot.cityRouteIdx = newIdx;
	if (newIdx < bot.cityRouteWps.size()) {
		bot.currentPos = bot.cityRouteWps[newIdx].pos;
		return VirtualRouteStatus::StillWalking;
	}
	// Route exhausted — clear state and let caller transition phase
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	return VirtualRouteStatus::JustCompleted;
}

void BotEngine::virtualTick(int64_t budget_ms) {
	// PERF_INVESTIGATION_2026-05-24 Phase A (2026-05-31): wall-clock-budgeted virtual
	// sim with rolling index across bots_. Replaces the prior 5s-gate + 4-bucket
	// round-robin (which burst-processed ~25% of hibernated bots every 5s, causing
	// 5s GAP_SLOW spikes confirmed in Phase B baseline). The caller computes budget_ms
	// from the tickBody EWMA: 3ms at low load, 1ms at moderate load, 0 (defer) at high
	// load. Self-throttling: virtual-sim work NEVER blocks the dispatcher.
	//
	// Rolling index s_virtualTickIndex walks bots_ forward across ticks. Bots are not
	// touched on a deterministic schedule (no bucket modulo), but per-bot
	// s_lastBotAdvanceMs ensures each bot's advance functions see the correct elapsed
	// time regardless of when they're processed.
	//
	// Worst-case staleness: bounded by sustained-load duration. Under normal load
	// (loadEwma < 30ms), full 3ms budget processes ~120 bots/tick = 750 hibernated
	// bots covered every 6 ticks (600ms) — far faster than the prior 20s cadence.
	// Under sustained high load, virtual sim defers indefinitely; bots' currentPos
	// goes stale but they're hibernated so nobody sees them. On wake,
	// chooseWakePosition validates the (possibly stale) virtualPos and falls back to
	// safe tiles. Wake latency itself is unaffected by this function.
	if (budget_ms <= 0 || bots_.empty()) return;

	// Per-bot last-advance tracking so each bot sees the correct elapsed time
	// regardless of which tick advances it. First-time fallback is 5000ms to match
	// the prior 5s-gate warm-up cadence (so cooldown counters aren't suddenly slammed
	// by a 100ms elapsed on freshly-warmed bots).
	// Moved from static local to instance member (lastVirtualAdvanceMs_) so hibernateBot
	// can reset on awake→hibernated transitions and avoid spurious VT_LAGs.
	static size_t s_virtualTickIndex = 0;
	static int64_t s_lastVtLagWarn = 0;
	static int64_t s_lastVtHeartbeat = 0;
	static uint64_t s_vtAdvancedSinceHeartbeat = 0;
	constexpr int64_t VT_LAG_WARN_MS = 60000;

	const int64_t now = OTSYS_TIME(); // cached epoch ms — per-bot elapsed bookkeeping (cycle granularity is fine)
	// JITTER FIX 2026-06-10: the wall-clock budget MUST use a real clock — with the
	// cached OTSYS_TIME the break below never fired and every call processed ALL
	// hibernated bots (the Phase-A budget was inert; confirmed live by VT_HEARTBEAT
	// arithmetic: 14,649 advances/s = 3 leaked loops x ALL 487 bots x 10Hz).
	const int64_t vt_start_mono = botMonoMs();

	size_t checked = 0;
	uint32_t advanced = 0;
	const size_t totalBots = bots_.size();

	while (checked < totalBots) {
		if (s_virtualTickIndex >= bots_.size()) {
			s_virtualTickIndex = 0;
		}
		BotState &bot = bots_[s_virtualTickIndex];
		s_virtualTickIndex++;
		checked++;

		if (!bot.hibernated) continue;

		int64_t &lastMs = lastVirtualAdvanceMs_[bot.guid];
		int64_t per_bot_elapsed = (lastMs == 0) ? 5000 : (now - lastMs);

		// VT_LAG: warn when a bot's advance is stale beyond threshold. Rate-limited
		// globally so a sustained defer storm doesn't spam the log.
		// Position/town/state + route fields included to test the chronic-bot route/location
		// hypothesis. Cluster signals:
		//   - position clusters → location-specific (broken waypoint, unreachable next-step)
		//   - hunt id:phase:wp repeats → hunt-route bug at a specific step
		//   - travel='walk_to_boat'/'at_boat'/etc clusters → city-travel state machine stall
		//   - advStone phase repeats → AdvStone state machine stall
		//   - cityWp clusters → city-route waypoint walk stall
		// Empty/zero route fields signal "not applicable" for the bot's current state.
		if (lastMs != 0 && per_bot_elapsed > VT_LAG_WARN_MS && (now - s_lastVtLagWarn) > 60000) {
			s_lastVtLagWarn = now;
			g_logger().warn("[VT_LAG] bot guid={} name='{}' pos=({},{},{}) town='{}' state={} hunt={}:{}:{} travel='{}' advStone={}:{}:{} cityWp={} elapsed={}ms (>{}ms threshold) — sustained load deferral",
				bot.guid, bot.name,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				bot.townName, static_cast<int>(bot.state),
				bot.huntScriptId, static_cast<int>(bot.huntPhase), bot.huntWaypointIdx,
				bot.travelPhase,
				bot.advStoneActive ? 1 : 0, bot.advStonePhase, bot.advStoneRouteIdx,
				bot.cityRouteIdx,
				per_bot_elapsed, VT_LAG_WARN_MS);
		}

		lastMs = now;

		switch (bot.state) {
			case BotAIState::IDLE:
				virtualAdvanceIdle(bot, per_bot_elapsed);
				break;
			case BotAIState::DWELLING:
				virtualAdvanceDwelling(bot, per_bot_elapsed);
				break;
			case BotAIState::TRAVELING:
				virtualAdvanceTraveling(bot, per_bot_elapsed);
				break;
			case BotAIState::HUNTING:
				virtualAdvanceHunting(bot, per_bot_elapsed);
				break;
			case BotAIState::PARTY:
				virtualAdvancePartyHunt(bot, per_bot_elapsed);
				break;
			default:
				// COMBAT/PK_ATTACK/FLEEING shouldn't persist into hibernation; normalize to IDLE.
				bot.state = BotAIState::IDLE;
				bot.nextRerollTime = OTSYS_TIME() + 5000;
				break;
		}

		// Adventurer's Stone trip runs as an overlay on top of the base state machine
		if (bot.advStoneActive) {
			virtualAdvanceAdvStone(bot, per_bot_elapsed);
		}

		// Fix #11: hibernated bot channel chat (World Chat + Advertising only).
		// Bots not in the world (no spectators, no Local Chat) but still subscribed
		// to global channels. Gated by livenessCfg_.hibernatedChatEnabled. Cheap
		// per-bot: 2 timer checks + at most 2 talkToChannel attempts when timers fire.
		tickHibernatedChat(bot);

		maybeQueueVirtualPositionSave(bot);
		++advanced;

		// Budget check every 4 advances: cheap inner loop with bounded overshoot.
		// One bot's advance is typically <100µs, so worst-case overshoot is <0.4ms.
		if ((advanced & 3) == 0) {
			if (botMonoMs() - vt_start_mono >= budget_ms) break;
		}
	}

	s_vtAdvancedSinceHeartbeat += advanced;

	// VT_HEARTBEAT: periodic summary, every 5 min. Confirms the rolling index is
	// progressing and reports per-period throughput for soak analysis.
	if (now - s_lastVtHeartbeat > 300000) {
		s_lastVtHeartbeat = now;
		g_logger().info("[VT_HEARTBEAT] advanced {} bots over 5min, index={}/{}, budget={}ms this call",
			s_vtAdvancedSinceHeartbeat, s_virtualTickIndex, bots_.size(), budget_ms);
		s_vtAdvancedSinceHeartbeat = 0;
	}
}

void BotEngine::virtualAdvanceIdle(BotState &bot, int64_t elapsed_ms) {
	// Adventurer's Stone overlay is the sole owner of the bot during a trip. Base IDLE
	// state machine (route walk, POI dwell, doActivityReroll) must not run — any of
	// those can mutate bot.currentPos and strand the bot away from its dwell target
	// on wake. Without this guard, a hibernated bot mid-Adv-Stone-trip can get
	// virtually relocated to e.g. Venore boat during phase-1 dwell; on cast reconnect,
	// the live AI's walk-to-target hits the 30s deadline and demotes to mode 0.
	// virtualAdvanceAdvStone (called by virtualTick after this) handles the full trip
	// lifecycle (route → dwell → forcefield → return) on its own.
	if (bot.advStoneActive) return;

	// Per-tick city-route walking. Captured so POI/depot/nav handlers below can
	// distinguish "route just completed" (do arrival action) from "no route loaded
	// yet" (try to load one).
	auto routeStatus = virtualTickCityRoute(bot, elapsed_ms);
	if (routeStatus == VirtualRouteStatus::StillWalking) return;

	// POI walk: Phase D — walk city route to POI waypoint-by-waypoint when possible,
	// fall back to today's one-tick snap when no route exists. pendingNavDest holds
	// the destination route key ("depot"/"temple"/etc.) set alongside hasWalkTarget
	// at IDLE reroll (bot_engine.cpp:4160).
	if (bot.hasWalkTarget) {
		if (routeStatus == VirtualRouteStatus::NotActive) {
			// First entry: try loading the city route to the POI. If found, the next
			// virtualTick will walk it via virtualTickCityRoute.
			if (!bot.pendingNavDest.empty() && loadCityRouteCore(bot, "", bot.pendingNavDest)) {
				return;  // route loaded; subsequent ticks walk it
			}
			// No route loadable — fall through to arrival path (today's snap behavior).
		}
		// Either routeStatus == JustCompleted (route walked to end) or NotActive with
		// no loadable route. Snap exactly to walkTarget tile (route's last waypoint
		// may be close-but-not-exact for STAND-arrival semantics) and fire arrival logic.
		bot.currentPos = bot.walkTarget;
		bot.hasWalkTarget = false;
		bot.pendingNavDest.clear();

		// AdvStone intercept: mirror startAdventurerStoneTrip without engine ops.
		// The live AI (doIdle:4592) calls startAdventurerStoneTrip on POI arrival;
		// without this virtual mirror, hibernated bots dwell forever at the temple
		// instead of starting the dungeon trip. findAdventurerStoneTownAt is a pure
		// table lookup (no Player needed).
		if (bot.currentPOI && bot.currentPOI->type == POIType::ADVENTURER_STONE
				&& !adventurerStoneRoute_.empty()) {
			uint32_t townId = findAdventurerStoneTownAt(bot.currentPos);
			if (townId != 0) {
				bot.advStoneActive = true;
				bot.advStoneStartTownId = townId;
				bot.advStonePhase = 0;
				bot.advStoneRouteIdx = 0;
				bot.advStoneIdleAt = pickAdventurerStoneIdleIdx();
				bot.advStoneDwellUntil = 0;
				bot.advStoneDeadline = 0;
				// Pre-roll the 3-way sub-activity (waypoint / chest / dummy) here so a
				// player who later wakes this hibernated bot inherits a pre-picked mode
				// + target. selectAdvStoneSubActivity is Player-free (pure map queries:
				// collectAdjacentFree, collectFreeWithLOS). Live phase-1 walk-to-target
				// uses the rolled values on wake; virtual sim itself just dwells the
				// rolled-mode duration via advStoneDwellSecs(mode) when reaching idleAt.
				selectAdvStoneSubActivity(bot);
				bot.currentPOI = nullptr;
				bot.followingCityRoute = false;
				bot.cityRouteWps.clear();
				bot.cityRouteIdx = 0;

				// PRE-ADVANCE FIX (Sonnet-reviewed): skip the virtual route walk entirely.
				// Place the bot at its dwell endpoint (chest/dummy adjacent tile for mode
				// 1/2, idle waypoint for mode 0) from trip start onwards.
				//
				// Previously: bot.currentPos = kAdventurerStoneDest (WP 1 = entry tile),
				// virtualAdvanceAdvStone walks 1 wp/5s — taking ~85s to reach a typical
				// idleAt. Under sustained dispatcher load (observed VT_LAG of 60-992s,
				// virtualTick budget squeezed by per-tick body work), the bot never
				// advances past WP 1. A player teleporting to the island sees ALL bots
				// piled at WP 1 (entry tile), then they walk to their dwell positions
				// AFTER the player arrives — defeating the "alive island" UX.
				//
				// Pre-advance directly sets currentPos to the dwell endpoint, jumps
				// advStoneRouteIdx to idleAt, and starts phase-1 dwell. Wake-from-virtual
				// then lands at the right tile regardless of virtualSim catch-up.
				bot.advStoneRouteIdx = bot.advStoneIdleAt;
				bot.advStonePhase = 1;
				bot.advStoneDwellUntil = OTSYS_TIME() + advStoneDwellSecs(bot.advStoneDwellMode) * 1000LL;
				if (bot.advStoneDwellMode != 0 && bot.advStoneDwellTarget.x > 0) {
					// Mode 1 (chest) or mode 2 (dummy training) — adjacent walkable tile.
					bot.currentPos = bot.advStoneDwellTarget;
				} else if (bot.advStoneIdleAt > 0
						&& bot.advStoneIdleAt < adventurerStoneRoute_.size()) {
					// Mode 0 (random idle waypoint) — the picked NODE waypoint position.
					bot.currentPos = adventurerStoneRoute_[bot.advStoneIdleAt].pos;
				} else {
					// pickAdventurerStoneIdleIdx is documented to return a valid index;
					// hitting this means the route is malformed. Don't silently revert
					// to WP 1 — that re-introduces the exact bug this fix addresses.
					g_logger().error("[BotEngine] virtual AdvStone trip-start: bot guid={} mode={} "
						"idleAt={} out of bounds (route size={}) — trip cancelled",
						bot.guid, static_cast<int>(bot.advStoneDwellMode),
						bot.advStoneIdleAt, adventurerStoneRoute_.size());
					bot.advStoneActive = false;
					return;
				}

				g_logger().info("[BotEngine] virtual AdvStone trip start (pre-advanced): "
					"bot guid={} town={} mode={} idleAt={} pos=({},{},{})",
					bot.guid, townId, static_cast<int>(bot.advStoneDwellMode),
					bot.advStoneIdleAt, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
				return; // state stays IDLE; virtualAdvanceAdvStone phase-1 dwells until expiry
			}
			// townId == 0 means the bot's current pos isn't in any known temple range.
			// Fall through to normal DWELLING — same as live behavior on PZ-check fail.
		}

		// Mirror live's POI-type branched dwell from doIdle (REROLL_POI_DWELL vs REROLL_NPC_DWELL).
		// Pre-fix this used uniform_random(15, 90) for all POIs, which meant hibernated bots
		// at depots/temples/boats spent only 15-90s per visit instead of the live 120-600s.
		bool isShortDwell = bot.currentPOI != nullptr
		    && (bot.currentPOI->type == POIType::NPC || bot.currentPOI->type == POIType::SHOP);
		int32_t dwellTime = isShortDwell
		    ? uniform_random(g_configManager().getNumber(BOT_DWELL_NPC_MIN_SEC), g_configManager().getNumber(BOT_DWELL_NPC_MAX_SEC))
		    : uniform_random(g_configManager().getNumber(BOT_DWELL_POI_MIN_SEC), g_configManager().getNumber(BOT_DWELL_POI_MAX_SEC));
		bot.dwellUntil = OTSYS_TIME() + dwellTime * 1000LL;
		bot.state = BotAIState::DWELLING;
		return;
	}

	// Phase E: depot locker walk. Set by travel arrival, POI arrival, or hunt prep
	// (hasDepotTarget + idleDepotTarget). Live AI walks via direct A* (no city route)
	// in doIdle (~line 5230). Virtual sim: load city route to "depot" POI first, then
	// snap exactly to the specific locker tile on arrival (route ends at depot POI,
	// which is ≈ but not exactly the locker). Mirrors live arrival timer to suppress
	// reroll for 20-60s like live (s_depotLockerRerollTime + bot.nextRerollTime).
	if (bot.hasDepotTarget && bot.idleDepotTarget.x > 0) {
		if (routeStatus == VirtualRouteStatus::NotActive) {
			if (loadCityRouteCore(bot, "", "depot")) {
				return;  // route loaded; subsequent ticks walk it
			}
			// No route — fall through to snap (matches today's behavior of stalling at
			// current position; live AI on wake will use direct A* to reach the locker).
		}
		bot.currentPos = bot.idleDepotTarget;
		bot.hasDepotTarget = false;
		int32_t waitSecs = uniform_random(20, 60);
		int64_t until = OTSYS_TIME() + waitSecs * 1000LL;
		s_depotLockerRerollTime[bot.guid] = until;
		bot.nextRerollTime = until;
		return;
	}

	// Reroll when idle and timer is due — doActivityReroll is null-Player-safe
	// (HUNT/CITYWALK branches early-return false on null player; TRAVEL branch is fully safe;
	// POI branch uses read-only g_game().map.getTile calls which are safe without a Player).
	if (OTSYS_TIME() >= bot.nextRerollTime) {
		doActivityReroll(bot);
	}
}

void BotEngine::virtualAdvanceDwelling(BotState &bot, int64_t /*elapsed_ms*/) {
	// Pure timer wait — no position change. checkVigilante / random PZ-tile selection skipped.
	if (OTSYS_TIME() >= bot.dwellUntil) {
		bot.state = BotAIState::IDLE;
		bot.nextRerollTime = OTSYS_TIME(); // fire reroll on next virtual tick
	}
}

void BotEngine::virtualAdvanceTraveling(BotState &bot, int64_t elapsed_ms) {
	// Per-tick city-route walking. Capture status so per-phase handlers can
	// distinguish "route just completed" (transition phase) from "no route loaded yet"
	// (try to load one).
	auto routeStatus = virtualTickCityRoute(bot, elapsed_ms);
	if (routeStatus == VirtualRouteStatus::StillWalking) return;

	// snapToward: bot "arrives" at a single-target position in one tick (5s).
	// Used as the no-route fallback for phases that don't yet support waypoint walking.
	auto snapToward = [&](const Position &target) {
		bot.currentPos = target;
		return true; // always "arrived" in one tick
	};

	if (bot.travelPhase == "walk_to_boat") {
		// No source boat = nothing to walk to (matches today's behavior).
		if (bot.travelSrcBoatPos.x == 0) {
			bot.travelPhase = "at_boat";
			bot.travelWaitUntil = OTSYS_TIME() + uniform_random(3, 10) * 1000LL;
			return;
		}
		// Phase C: walk source city route waypoint-by-waypoint. Pattern mirrors live
		// doTraveling walk_to_boat (bot_engine.cpp ~line 12779-12783): destination key
		// is s_travelSrcPOI ("boat"/"carpet", default "boat"), fallback "temple".
		if (routeStatus == VirtualRouteStatus::JustCompleted) {
			// Mirror live AI bot_engine.cpp:12710-12720: mark this route's source as tried
			// so the next NotActive tick picks a different POI via findBestRouteSource.
			auto srcIt = s_lastRouteSource.find(bot.guid);
			if (srcIt != s_lastRouteSource.end() && !srcIt->second.empty()) {
				bot.triedRouteSources.insert(srcIt->second);
				s_lastRouteSource.erase(srcIt);
			}
			// Live handler (bot_engine.cpp:12738) only transitions to at_boat when
			// dist <= 3 to the actual boat NPC. If route ends farther away (e.g.
			// "temple" fallback route ends at temple, not boat), DON'T transition;
			// next virtualTick will retry with a different route source.
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.travelSrcBoatPos.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.travelSrcBoatPos.y)));
			if (dist <= 3 && bot.currentPos.z == bot.travelSrcBoatPos.z) {
				bot.travelPhase = "at_boat";
				bot.travelWaitUntil = OTSYS_TIME() + uniform_random(3, 10) * 1000LL;
			}
			return;
		}
		// No route loaded yet (NotActive). Try src-POI then temple fallback.
		std::string srcPOI = s_travelSrcPOI.count(bot.guid) ? s_travelSrcPOI[bot.guid] : "boat";
		if (loadCityRouteCore(bot, "", srcPOI) ||
			loadCityRouteCore(bot, "", "temple")) {
			return;  // route loaded; next virtualTick walks it
		}
		// No route — fall back to today's snap-to-boat behavior.
		snapToward(bot.travelSrcBoatPos);
		bot.travelPhase = "at_boat";
		bot.travelWaitUntil = OTSYS_TIME() + uniform_random(3, 10) * 1000LL;
	} else if (bot.travelPhase == "at_boat") {
		if (OTSYS_TIME() >= bot.travelWaitUntil) {
			// Virtual boat ride: instant teleport to destination boat position
			if (bot.travelBoatPos.x > 0) {
				bot.currentPos = bot.travelBoatPos;
			}
			// Mirror the live at_boat handler (bot_engine.cpp ~line 12565): update townId
			// and townName the moment the bot virtually arrives in the destination city.
			// Without this, woken bots show status "IDLE in <source-city>" while physically
			// sitting at the destination's boat dropoff — bot.townName is the stale source.
			if (bot.travelDestTownId > 0) {
				auto town = g_game().map.towns.getTown(bot.travelDestTownId);
				if (town) {
					bot.townId = bot.travelDestTownId;
					auto nameIt = travelTownNames_.find(bot.travelDestTownId);
					bot.townName = nameIt != travelTownNames_.end() ? nameIt->second : town->getName();
				}
			}
			bot.travelPhase = "teleported";
			bot.travelWaitUntil = OTSYS_TIME() + uniform_random(2, 5) * 1000LL;
		}
	} else if (bot.travelPhase == "teleported") {
		if (OTSYS_TIME() >= bot.travelWaitUntil) {
			bot.travelPhase = "walk_from_boat";
		}
	} else if (bot.travelPhase == "walk_from_boat") {
		// Phase B: walk the destination city route waypoint-by-waypoint so a real
		// player walking near any intermediate waypoint can wake this bot mid-journey.
		// Pattern mirrors live doTraveling walk_from_boat (bot_engine.cpp ~line 12866-12872).
		if (routeStatus == VirtualRouteStatus::JustCompleted) {
			// Destination route walked to completion — bot virtually arrived.
			bot.travelPhase = "arrived";
		} else {
			// No route loaded yet (NotActive). Try arrival-POI → depot/temple chain.
			// s_travelDestPOI was set to "boat"/"carpet" at startTravel; default "boat".
			std::string arrivalPOI = s_travelDestPOI.count(bot.guid) ? s_travelDestPOI[bot.guid] : "boat";
			// BOT_TRAVEL_ARRIVE_MIX — the twin of the live prepend in bot_travel.cpp. It reads the
			// SAME stored roll rather than making its own, so a bot woken mid-journey does not
			// change where it was heading (feedback_live_twin_must_match).
			const std::string arriveTarget = s_travelArriveTarget.count(bot.guid)
				? s_travelArriveTarget[bot.guid] : std::string("depot");
			if (arriveTarget != "depot") {
				if (loadCityRouteCore(bot, arrivalPOI, arriveTarget) ||
					loadCityRouteCore(bot, "", arriveTarget)) {
					noteTravelArrivalClass(arriveTarget);
					bot.travelDestVerified = true;
					return;
				}
				s_arriveFallbackCount++;
			}
			if (loadCityRouteCore(bot, arrivalPOI, "depot") ||
				loadCityRouteCore(bot, arrivalPOI, "temple") ||
				loadCityRouteCore(bot, "", "depot") ||
				loadCityRouteCore(bot, "", "temple")) {
				if (arriveTarget == "depot") {
					noteTravelArrivalClass(arriveTarget);
				}
				// Pre-set travelDestVerified so live AI's walk_from_boat handler
				// (bot_engine.cpp ~line 12808-12830) doesn't reset to walk_to_boat
				// when the woken bot is far from travelBoatPos. The virtual sim
				// skipped the at_boat→walk_from_boat position verification entirely.
				bot.travelDestVerified = true;
				return;  // route loaded; next virtualTick walks it
			}
			// No city route exists for this destination — fall back to today's behavior
			// (snap to temple POI, mark arrived). Keeps inter-city travel working even
			// for towns whose route tables are missing or malformed.
			Position destPos = bot.currentPos;
			auto poiIt = cityPOIs_.find(bot.travelDestTownId);
			if (poiIt != cityPOIs_.end()) {
				for (const auto &poi : poiIt->second) {
					if (poi.type == POIType::TEMPLE) { destPos = poi.pos; break; }
				}
			}
			snapToward(destPos);
			bot.travelPhase = "arrived";
		}
	} else if (bot.travelPhase == "arrived" || bot.travelPhase.empty()) {
		// Update townId/townName, clear travel state, transition to IDLE
		if (bot.travelDestTownId > 0) {
			bot.townId = bot.travelDestTownId;
			auto nameIt = travelTownNames_.find(bot.travelDestTownId);
			if (nameIt != travelTownNames_.end()) {
				bot.townName = nameIt->second;
			}
		}
		bot.travelDestTownId = 0;
		bot.travelPhase.clear();
		bot.pendingHuntAfterTravel = false; // can't kick off hunt here; live AI or virtual reroll handles
		bot.state = BotAIState::IDLE;
		bot.nextRerollTime = OTSYS_TIME() + uniform_random(5, 15) * 1000LL;
	}
}

void BotEngine::maybeQueueVirtualPositionSave(BotState &bot) {
	int64_t now = OTSYS_TIME();
	if (now - bot.lastVirtualPosSave < VIRTUAL_POS_SAVE_THROTTLE_MS) return;

	// Skip if virtually-advanced position hasn't drifted enough to be worth a DB write.
	int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.lastPos.x));
	int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.lastPos.y));
	if (dx < VIRTUAL_POS_SAVE_MIN_DRIFT && dy < VIRTUAL_POS_SAVE_MIN_DRIFT
	    && bot.currentPos.z == bot.lastPos.z) {
		bot.lastVirtualPosSave = now;
		return;
	}

	bot.lastVirtualPosSave = now;
	bot.lastPos = bot.currentPos;
	g_botDatabaseTasks().execute(fmt::format(
		"UPDATE `players` SET `posx`={}, `posy`={}, `posz`={} WHERE `id`={}",
		bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, bot.guid));
}

// Phase 2: virtualAdvanceHunting — full HUNTING phase machine in pure memory.
// Mirrors live doHunting → doHuntPrepare/Travel/Patrol/Leaving/Resupply but skips depot/shop
// item simulation, monster scans, spell casts, etc. Time-based hunt completion.
void BotEngine::virtualAdvanceHunting(BotState &bot, int64_t elapsed_ms) {
	// HUNT_VT diagnostic (2026-06-10): capture phase at entry so end-of-fn can detect
	// transitions, and emit a [HUNT:STUCK] warn if this bot has been frozen in the
	// same phase > 30min (warn rate-limited per-bot to 1 per 5min).
	const int64_t huntVt_now = OTSYS_TIME();
	const HuntPhase huntVt_prevPhase = bot.huntPhase;
	int64_t &huntVt_enteredMs = huntPhaseEnteredMs_[bot.guid];
	if (huntVt_enteredMs == 0) huntVt_enteredMs = huntVt_now;
	const int64_t huntVt_inPhaseMs = huntVt_now - huntVt_enteredMs;
	if (huntVt_inPhaseMs > 1800 * 1000LL) {
		int64_t &huntVt_lastWarn = lastHuntStuckWarnMs_[bot.guid];
		if (huntVt_now - huntVt_lastWarn > 300000) {
			huntVt_lastWarn = huntVt_now;
			g_logger().warn("[HUNT:STUCK] bot guid={} name='{}' huntPhase={} huntScriptId={} wpIdx={} prepStep={} in_phase_ms={}",
				bot.guid, bot.name, static_cast<int>(bot.huntPhase),
				bot.huntScriptId, bot.huntWaypointIdx, bot.prepareStep, huntVt_inPhaseMs);
		}
	}

	// Look up the active hunt script. If gone (reload, etc.), abort to IDLE.
	const HuntScript *script = nullptr;
	for (const auto &s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script) {
		bot.huntScriptId = 0;
		bot.state = BotAIState::IDLE;
		bot.nextRerollTime = OTSYS_TIME() + 5000;
		return;
	}

	// ---- Absolute ceiling, mirroring doHunting's ---------------------------------------------
	// The virtual sim enforces NONE of the live per-phase budgets — it just walks the waypoint
	// index at VIRTUAL_MS_PER_WAYPOINT — so a hibernated bot's hunt could run past the ceiling
	// that doHunting applies unconditionally on every phase. huntStartTime is real wall-clock and
	// wakeBot never resets it, so such a bot was aborted on its FIRST live tick after waking,
	// whatever it happened to be doing: the virtual clock and the live clock had diverged, and the
	// live one ambushed it. Joint worst case over the enabled non-quest scripts is 4030s (2081
	// "Asura Palace Cave -2": 131 travel_to + 135 travel_from waypoints = 1330s of legs, plus a
	// 2400s patrol clock and 300s of resupply); 8 of 220 scripts exceeded the old 3600s ceiling.
	//
	// Raising HUNT_SAFETY_TIMEOUT to 5400 buys margin, but only this check makes the two clocks
	// agree. Same ceiling selection as bot_hunt.cpp's live site, so they cannot drift apart.
	{
		const int32_t vtSafetyTimeout = bot.isPartyHuntLeader
			? PARTY_SAFETY_TIMEOUT
			: (botScriptIsQuest(script) ? QUEST_SAFETY_TIMEOUT : HUNT_SAFETY_TIMEOUT);
		if (bot.huntStartTime > 0
		    && OTSYS_TIME() - bot.huntStartTime > vtSafetyTimeout * 1000LL) {
			g_logger().info("[HUNT:VT] guid={} name='{}' virtual safety timeout ({}s) in phase={} wpIdx={} — releasing hunt",
				bot.guid, bot.name, vtSafetyTimeout, static_cast<int>(bot.huntPhase), bot.huntWaypointIdx);
			if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
				dissolveVirtualPartyHunt(bot.partyHuntId, "virtual_safety_timeout");
			}
			// Terminal outcome, same teardown the virtual resupply-complete path performs.
			bot.followingCityRoute = false;
			bot.cityRouteWps.clear();
			bot.cityRouteIdx = 0;
			activeHunts_.erase(bot.huntScriptId);
			if (!script->spawnGroup.empty()) activeSpawnGroups_.erase(script->spawnGroup);
			// Snap the synthetic position home. Leaving it deep in a spawn would hand
			// chooseWakePosition a virtualPos it has to repair, and the bot is notionally done.
			if (auto town = g_game().map.towns.getTown(bot.townId)) {
				bot.currentPos = town->getTemplePosition();
				bot.lastPos = bot.currentPos;
			}
			bot.isRecoveryRoute = false;
			bot.recoveryWaypoints.clear();
			bot.huntScriptId = 0;
			bot.huntKillCount = 0;
			bot.huntTargetId = 0;
			bot.huntIgnoredMonsters.clear();
			bot.huntPatrolCycles = 0;
			bot.huntWaypointIdx = 0;
			bot.huntPhase = HuntPhase::PREPARING;  // reset for next hunt
			bot.huntResupplyPhase = 0;
			bot.huntResupplyStart = 0;
			bot.prepareStep = 0;
			bot.prepareWaitUntil = 0;
			bot.state = BotAIState::IDLE;
			bot.nextRerollTime = OTSYS_TIME();
			return;
		}
	}

	switch (bot.huntPhase) {
		case HuntPhase::PREPARING: {
			// Phase G: simulate the intra-city prep walks (temple→depot, depot→potions)
			// via city routes so a real player walking through the city can wake bots
			// during resupply. Mirrors live doHuntPrepare (bot_engine.cpp:11103-11465).
			// Hard timeouts (prepareWaitUntil + RESUPPLY_TIMEOUT) preserved as safety net.
			auto prepRouteStatus = virtualTickCityRoute(bot, elapsed_ms);
			if (prepRouteStatus == VirtualRouteStatus::StillWalking) return;

			// Step transitions on JustCompleted (walk done) or wait expiry (dwell done).
			if (prepRouteStatus == VirtualRouteStatus::JustCompleted) {
				if (bot.prepareStep == 0) {
					bot.prepareStep = 1;
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
				} else if (bot.prepareStep == 2) {
					bot.prepareStep = 3;
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(10, 60) * 1000LL;
				}
			} else if (bot.prepareWaitUntil > 0 && OTSYS_TIME() >= bot.prepareWaitUntil) {
				if (bot.prepareStep == 1) {
					bot.prepareStep = 2;
					bot.prepareWaitUntil = 0;
				} else if (bot.prepareStep == 3) {
					// Live AI step 4 (exit walk) is conditional on can-reach-spawn check
					// that requires a Player object — skip to done in virtual sim. Live
					// AI on wake will do the right thing.
					bot.prepareStep = 5;
					bot.prepareWaitUntil = 0;
				}
			}

			bool prepDone = (bot.prepareStep >= 5);
			// Virtual sim walks 1 wp/5s — much slower than real-player movement (~5 tiles/s).
			// A 30-waypoint route takes 150s virtually vs <30s live. Double the live timeout
			// for the virtual path so typical resupply chains don't get cut short.
			if (bot.prepareStartTime > 0 &&
			    OTSYS_TIME() - bot.prepareStartTime > RESUPPLY_TIMEOUT * 1000LL * 2) {
				prepDone = true; // hard timeout safety net (doubled for virtual sim)
			}
			if (prepDone) {
				// Clear any leftover city route so TRAVEL_TO doesn't see stale state
				bot.followingCityRoute = false;
				bot.cityRouteWps.clear();
				bot.cityRouteIdx = 0;
				bot.prepareStep = 5;
				bot.prepareHasTarget = false;
				bot.huntWaypointIdx = 0;
				bot.huntPhase = HuntPhase::TRAVEL_TO;
				break;
			}

			// Load route for current walk step (only when not already routing and not dwelling).
			// Sets prepareHasTarget=true to match live AI behavior (bot_engine.cpp:11347) so
			// wake handoff doesn't enter live AI's POI-fallback re-load branch.
			if (prepRouteStatus == VirtualRouteStatus::NotActive && bot.prepareWaitUntil == 0) {
				if (bot.prepareStep == 0) {
					if (loadCityRouteCore(bot, "", "depot")) {
						bot.prepareHasTarget = true;
					} else {
						bot.prepareStep = 2;  // no depot route → skip to shop step
					}
				} else if (bot.prepareStep == 2) {
					if (loadCityRouteCore(bot, "depot", "potions") ||
						loadCityRouteCore(bot, "", "potions")) {
						bot.prepareHasTarget = true;
					} else {
						bot.prepareStep = 5;  // no shop route → done
					}
				}
			}
			break;
		}

		case HuntPhase::TRAVEL_TO: {
			if (script->travelToWaypoints.empty()) {
				// Snap directly to the patrol entry waypoint. Phase 4b: that entry is
				// phase-scattered (not always index 0) so hibernated bots entering the same
				// loop don't resume it in lockstep when they wake.
				bot.huntWaypointIdx = botPatrolEntryIdx(script->patrolWaypoints, *script);
				if (!script->patrolWaypoints.empty()) {
					bot.currentPos = script->patrolWaypoints[bot.huntWaypointIdx].pos;
				}
				bot.huntPatrolCycles = 0;
				bot.huntPhase = HuntPhase::PATROLLING;
				break;
			}
			// Phase 6: index-based, 1 waypoint per 5s.
			size_t newIdx = advanceWaypointIdx(bot.guid, bot.huntWaypointIdx, script->travelToWaypoints.size(), elapsed_ms);
			bot.huntWaypointIdx = newIdx;
			if (newIdx < script->travelToWaypoints.size()) {
				bot.currentPos = script->travelToWaypoints[newIdx].pos;
			}
			if (bot.huntWaypointIdx >= script->travelToWaypoints.size()) {
				// Phase 4b: phase-scattered patrol entry (see botPatrolEntryIdx).
				// Snap currentPos to the SAME waypoint, exactly like the empty-travel branch
				// above. Without this the index jumps to the scattered entry while currentPos
				// is still the last travel_to waypoint — the virtual twin of the live
				// landing/index mismatch. It self-corrects on the next virtual tick, but a
				// bot woken in that window is placed from a stale position.
				bot.huntWaypointIdx = botPatrolEntryIdx(script->patrolWaypoints, *script);
				if (!script->patrolWaypoints.empty()) {
					bot.currentPos = script->patrolWaypoints[bot.huntWaypointIdx].pos;
				}
				bot.huntPatrolCycles = 0;
				bot.huntPhase = HuntPhase::PATROLLING;
			}
			break;
		}

		case HuntPhase::PATROLLING: {
			// Time-based exit (per design: no virtual kill-counter advancement).
			//
			// QUESTS ONLY here. A quest's clock is an absolute deadline from the quest budget
			// table, not a lap-shaped one. For an ordinary hunt the decision is deferred to the
			// loop boundary below, mirroring live doHuntPatrol, so travel_from is entered from the
			// patrol's TERMINAL waypoint — the position the script is authored to continue from.
			//
			// The quest carve-out is EXPLICIT, not an accident of arithmetic. Deferring a quest
			// here would today be inert only because the largest patrol (291 wps) takes
			// 291 x VIRTUAL_MS_PER_WAYPOINT = 24 min virtually and so always wraps before the quest
			// clock could bite. That is a numeric coincidence, not a guarantee, and relying on one
			// is exactly what left a24c5b909 inert in production twice.
			if (bot.huntEndTime > 0 && OTSYS_TIME() >= bot.huntEndTime
			    && botScriptIsQuest(script)) {
				// Virtual party leader transitioning to LEAVING — dissolve party FIRST so
				// supports go IDLE before the leader's reroll could pick them up again.
				if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
					dissolveVirtualPartyHunt(bot.partyHuntId, "virtual_quest_clock");
				}
				bot.huntWaypointIdx = 0;
				bot.huntPhase = HuntPhase::LEAVING;
				break;
			}
			if (script->patrolWaypoints.empty()) {
				// Nothing to walk; immediately go to LEAVING
				if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
					dissolveVirtualPartyHunt(bot.partyHuntId, "virtual_leaving_empty_wps");
				}
				bot.huntWaypointIdx = 0;
				bot.huntPhase = HuntPhase::LEAVING;
				break;
			}
			// Phase 6: index-based, 1 waypoint per 5s.
			size_t newIdx = advanceWaypointIdx(bot.guid, bot.huntWaypointIdx, script->patrolWaypoints.size(), elapsed_ms);
			bot.huntWaypointIdx = newIdx;
			if (newIdx < script->patrolWaypoints.size()) {
				bot.currentPos = script->patrolWaypoints[newIdx].pos;
			}
			if (bot.huntWaypointIdx >= script->patrolWaypoints.size()) {
				if (script->isQuest || script->scriptCategory == "quest") {
					// Quest patrol done — walk the authored return leg, mirroring the live
					// path. This used to drop straight to IDLE, skipping travel_from entirely.
					if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
						dissolveVirtualPartyHunt(bot.partyHuntId, "virtual_quest_patrol_complete");
					}
					bot.huntWaypointIdx = 0;
					bot.huntPhase = HuntPhase::LEAVING;
					break;
				}
				// Lap closed. THIS is where an ordinary hunt honours its clock, so the return leg
				// starts from the patrol's terminal waypoint rather than wherever the timer happened
				// to fall. Mirrors live doHuntPatrol's lapComplete branch.
				if (bot.huntEndTime > 0 && OTSYS_TIME() >= bot.huntEndTime) {
					if (bot.isPartyHuntLeader && bot.partyHuntId > 0) {
						dissolveVirtualPartyHunt(bot.partyHuntId, "virtual_leaving_timer");
					}
					// advanceWaypointIdx can step from size-2 straight to size, in which case the
					// snap above was skipped and currentPos still holds an earlier waypoint. Land
					// the synthetic position ON the terminus, since that is the whole point of
					// deferring to the lap boundary and it is what a woken bot should resume from.
					bot.currentPos = script->patrolWaypoints.back().pos;
					bot.lastPos = bot.currentPos;
					bot.huntWaypointIdx = 0;
					bot.huntPhase = HuntPhase::LEAVING;
					break;
				}
				bot.huntPatrolCycles++;
				bot.huntWaypointIdx = 0;
			}
			break;
		}

		case HuntPhase::LEAVING: {
			if (script->travelFromWaypoints.empty()) {
				// No return route — skip straight to resupply at current position
				bot.huntWaypointIdx = 0;
				bot.huntResupplyStart = OTSYS_TIME();
				bot.huntResupplyPhase = 0;
				bot.prepareStep = 0;
				bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
				bot.huntPhase = HuntPhase::RESUPPLYING;
				break;
			}
			// Phase 6: index-based, 1 waypoint per 5s.
			size_t newIdx = advanceWaypointIdx(bot.guid, bot.huntWaypointIdx, script->travelFromWaypoints.size(), elapsed_ms);
			bot.huntWaypointIdx = newIdx;
			if (newIdx < script->travelFromWaypoints.size()) {
				bot.currentPos = script->travelFromWaypoints[newIdx].pos;
			}
			if (bot.huntWaypointIdx >= script->travelFromWaypoints.size()) {
				// BOT_TELEPORT_TILE_SAFETY Phase 2a-virtual. advanceWaypointIdx clamps to
				// totalSize (see its `std::min`), and on the tick it clamps, the guard above
				// (`newIdx < size()`) is false — so bot.currentPos was NEVER updated to the
				// route's end. Snap it to the last waypoint before handing off to RESUPPLYING,
				// then resync the town: without this a hibernated bot finishing a Feyrist
				// return enters RESUPPLYING with BOTH a stale position and townId=26, whose
				// depot/temple are back inside Feyrist. The live path gets the same repair in
				// the teleport-detection block; this is the hibernated half, which is the
				// dominant population.
				if (!script->travelFromWaypoints.empty()) {
					bot.currentPos = script->travelFromWaypoints.back().pos;
				}
				syncTownIdToPos(bot);
				bot.huntWaypointIdx = 0;
				bot.huntResupplyStart = OTSYS_TIME();
				bot.huntResupplyPhase = 0;
				bot.prepareStep = 0;
				bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
				bot.huntPhase = HuntPhase::RESUPPLYING;
			}
			break;
		}

		case HuntPhase::RESUPPLYING: {
			// Phase G: same shop chain as PREPARING, different transition on done.
			// LEAVING set prepareStep=0 + prepareWaitUntil before entering this phase
			// (line ~3393), giving us the initial dwell at LEAVING terminus before walks.
			auto resupplyRouteStatus = virtualTickCityRoute(bot, elapsed_ms);
			if (resupplyRouteStatus == VirtualRouteStatus::StillWalking) return;

			if (resupplyRouteStatus == VirtualRouteStatus::JustCompleted) {
				if (bot.prepareStep == 0) {
					bot.prepareStep = 1;
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
				} else if (bot.prepareStep == 2) {
					bot.prepareStep = 3;
					bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(10, 60) * 1000LL;
				}
			} else if (bot.prepareWaitUntil > 0 && OTSYS_TIME() >= bot.prepareWaitUntil) {
				if (bot.prepareStep == 0) {
					// LEAVING's initial post-spawn dwell expired; clear so route loads.
					bot.prepareWaitUntil = 0;
				} else if (bot.prepareStep == 1) {
					bot.prepareStep = 2;
					bot.prepareWaitUntil = 0;
				} else if (bot.prepareStep == 3) {
					// Skip live's conditional exit walk (step 4) — virtual has no A* check
					bot.prepareStep = 5;
					bot.prepareWaitUntil = 0;
				}
			}

			bool done = (bot.prepareStep >= 5);
			// Doubled timeout for virtual sim — same rationale as PREPARING (1 wp/5s
			// is slower than real-player movement, so the live 5-min cap aborts
			// typical 30-waypoint resupply chains prematurely).
			if (bot.huntResupplyStart > 0 &&
			    OTSYS_TIME() - bot.huntResupplyStart > RESUPPLY_TIMEOUT * 1000LL * 2) done = true;
			if (done) {
				// Clear any leftover city route + release reservation
				bot.followingCityRoute = false;
				bot.cityRouteWps.clear();
				bot.cityRouteIdx = 0;
				activeHunts_.erase(bot.huntScriptId);
				if (!script->spawnGroup.empty()) activeSpawnGroups_.erase(script->spawnGroup);
				bot.huntScriptId = 0;
				bot.huntKillCount = 0;
				bot.huntTargetId = 0;
				bot.huntIgnoredMonsters.clear();
				bot.huntPatrolCycles = 0;
				bot.huntWaypointIdx = 0;
				bot.huntPhase = HuntPhase::PREPARING;  // reset for next hunt
				bot.huntResupplyPhase = 0;
				bot.huntResupplyStart = 0;
				bot.prepareStep = 0;
				bot.prepareWaitUntil = 0;
				bot.state = BotAIState::IDLE;
				bot.nextRerollTime = OTSYS_TIME();  // fire reroll immediately on next virtualTick
				break;
			}

			// Load route for current walk step. prepareHasTarget=true matches live AI
			// (bot_engine.cpp:11347) so wake handoff doesn't re-load via POI fallback.
			if (resupplyRouteStatus == VirtualRouteStatus::NotActive && bot.prepareWaitUntil == 0) {
				if (bot.prepareStep == 0) {
					if (loadCityRouteCore(bot, "", "depot")) {
						bot.prepareHasTarget = true;
					} else {
						bot.prepareStep = 2;
					}
				} else if (bot.prepareStep == 2) {
					if (loadCityRouteCore(bot, "depot", "potions") ||
						loadCityRouteCore(bot, "", "potions")) {
						bot.prepareHasTarget = true;
					} else {
						bot.prepareStep = 5;
					}
				}
			}
			break;
		}
	}

	// HUNT_VT diagnostic (2026-06-10): detect phase transition this call. Captures
	// the "code says yes, runtime says no" discrepancy: lets us see whether
	// virtualAdvanceHunting actually transitions phases for hibernated bots in
	// steady state. Only the switch-driven transitions reach this point; early
	// returns (StillWalking, script-vanished) don't change huntPhase so they don't
	// need logging. Note: RESUPPLYING done resets huntPhase=PREPARING + state=IDLE,
	// so the transition log will show 4→0 there.
	if (bot.huntPhase != huntVt_prevPhase) {
		huntPhaseEnteredMs_[bot.guid] = huntVt_now;
		// 2026-06-10 fix: clear leftover-ms on phase transition. Without this, a remainder
		// from the previous phase (up to 4999ms) carries over and gives the new phase's first
		// wp a head-start of up to ~half a waypoint. Sonnet-flagged inaccuracy; one-line fix.
		waypointLeftoverMs_.erase(bot.guid);
		g_logger().info("[HUNT:VT] bot guid={} name='{}' phase {}->{}  huntScriptId={} wpIdx={} state={} prev_in_phase_ms={}",
			bot.guid, bot.name,
			static_cast<int>(huntVt_prevPhase), static_cast<int>(bot.huntPhase),
			bot.huntScriptId, bot.huntWaypointIdx, static_cast<int>(bot.state),
			huntVt_inPhaseMs);
	}
}

// Phase 3: virtualAdvancePartyHunt — EK leader runs virtualAdvanceHunting;
// follower bots shadow leader's currentPos with deterministic per-bot offset.
void BotEngine::virtualAdvancePartyHunt(BotState &bot, int64_t elapsed_ms) {
	if (bot.isPartyHuntLeader) {
		// Leader runs the hunting state machine; team-cascade in proximity loop
		// keeps members synchronized at wake/hibernate boundaries.
		virtualAdvanceHunting(bot, elapsed_ms);
		return;
	}

	// Follower: mirror leader's position with deterministic offset (avoid stacking on same tile).
	if (bot.partyHuntId == 0 || bot.partyLeaderGuid == 0) {
		// Party state corrupted — exit gracefully
		s_botToPartyHunt.erase(bot.guid);
		bot.state = BotAIState::IDLE;
		bot.partyHuntId = 0;
		bot.nextRerollTime = OTSYS_TIME() + 5000;
		return;
	}

	auto leaderIt = guidToIndex_.find(bot.partyLeaderGuid);
	if (leaderIt == guidToIndex_.end()) {
		// Leader not registered — dissolve from this side
		s_botToPartyHunt.erase(bot.guid);
		bot.state = BotAIState::IDLE;
		bot.partyHuntId = 0;
		bot.partyLeaderGuid = 0;
		bot.partyRole = 0;
		bot.nextRerollTime = OTSYS_TIME() + 5000;
		return;
	}

	const auto &leader = bots_[leaderIt->second];
	int32_t offsetX = static_cast<int32_t>(bot.guid % 5) - 2;        // -2..+2
	int32_t offsetY = static_cast<int32_t>((bot.guid >> 4) % 5) - 2; // -2..+2
	bot.currentPos.x = static_cast<uint16_t>(static_cast<int32_t>(leader.currentPos.x) + offsetX);
	bot.currentPos.y = static_cast<uint16_t>(static_cast<int32_t>(leader.currentPos.y) + offsetY);
	bot.currentPos.z = leader.currentPos.z;
	// Keep follower's own state synced to leader's high-level state for visual coherence
	if (leader.state == BotAIState::HUNTING || leader.state == BotAIState::PARTY) {
		bot.huntPhase = leader.huntPhase;
	}
}

// Phase 3: virtualAdvanceAdvStone — overlay phase machine for the dungeon trip.
// Uses kAdventurerStoneForcefield/kAdventurerStoneDest from the top-of-file anon namespace.
void BotEngine::virtualAdvanceAdvStone(BotState &bot, int64_t elapsed_ms) {
	if (bot.advStonePhase == 0) {
		// Phase 6: index-based, 1 waypoint per 5s along the dungeon route.
		size_t newIdx = advanceWaypointIdx(bot.guid,
			static_cast<size_t>(bot.advStoneRouteIdx),
			adventurerStoneRoute_.size(), elapsed_ms);
		bot.advStoneRouteIdx = static_cast<uint16_t>(newIdx);
		if (newIdx < adventurerStoneRoute_.size()) {
			bot.currentPos = adventurerStoneRoute_[newIdx].pos;
		}

		// Phase 5d: check past-end FIRST (before idle-trigger). Without this, idleAt
		// near route end could fire dwell with idx already >= size(), producing the
		// "wp 17/16" cosmetic mismatch. pickAdventurerStoneIdleIdx guarantees idleAt
		// < size()-1 so this is safe.
		if (bot.advStoneRouteIdx >= adventurerStoneRoute_.size()) {
			bot.advStonePhase = 2;
			return;
		}
		// Reached chosen idle waypoint? transition to dwelling.
		// Phase 5b: dwell shortened from 5-30 min to 1-3 min so bots visibly cycle
		// through the dungeon rather than appearing "stuck" at one waypoint.
		if (bot.advStoneIdleAt > 0 && bot.advStoneRouteIdx >= bot.advStoneIdleAt
		    && bot.advStoneDwellUntil == 0) {
			bot.advStonePhase = 1;
			// Mirror live durations exactly (advStoneDwellSecs handles test-mode clamp +
			// per-mode chest/dummy/idle ranges via config). Sim doesn't model walk-to-target
			// for mode 1/2; just uses the appropriate dwell range for timing parity.
			bot.advStoneDwellUntil = OTSYS_TIME() + advStoneDwellSecs(bot.advStoneDwellMode) * 1000LL;
			// BOT_LIVENESS_PACK Phase A.5 fix: when sub-activity is chest (mode 1) or
			// dummy (mode 2), snap currentPos to the chosen target tile so wake places
			// the bot at the chest/dummy, not at the route node. Previously the bot
			// virtually "dwelt" at the idle waypoint while supposedly at the chest —
			// player teleporting to the island would see the bot at WP 17 instead of
			// next to the reward chest. chooseWakePosition's kUnsafeWakeMask validates
			// and walks back to a safe nearby tile if the target is unsafe.
			if (bot.advStoneDwellMode != 0 && bot.advStoneDwellTarget.x > 0) {
				bot.currentPos = bot.advStoneDwellTarget;
			}
			return;
		}
	} else if (bot.advStonePhase == 1) {
		// Dwelling at chosen route waypoint (or chest/dummy target per Phase A.5 sync above)
		if (OTSYS_TIME() >= bot.advStoneDwellUntil) {
			bot.advStonePhase = 0; // resume route to forcefield
			bot.advStoneDwellUntil = 0;
			// Resume from the idle route waypoint position (so the route walker picks
			// up cleanly from idleAt rather than from the chest/dummy tile, which is
			// off-route). chooseWakePosition + the route walker handle the geometry.
			if (bot.advStoneDwellMode != 0
			    && bot.advStoneIdleAt < adventurerStoneRoute_.size()) {
				bot.currentPos = adventurerStoneRoute_[bot.advStoneIdleAt].pos;
			}
			// Mirror the live dwell-expiry cleanup (doAdventurerStone phase 1):
			// release the chest/dummy sub-activity at dwell end. Without this the
			// stale mode survives the walk-out — the >=idleAt dwell trigger above
			// re-snaps to the stale chest/dummy target for ANOTHER full chest/dummy
			// duration, the bot keeps occupying a chest/dummy cap slot, and
			// filterClaimedByOtherBots keeps the tile claimed. Walk-out re-dwells
			// now run as short mode-0 waypoint pauses instead. A bot woken during
			// walk-out also no longer walks BACK to the chest/dummy in live.
			bot.advStoneDwellMode = 0;
			bot.advStoneDwellWeaponId = 0;
			bot.advStoneDwellTarget = Position();
		}
	} else if (bot.advStonePhase == 2) {
		// Snap to forcefield then back to start-town temple (mimics MoveEvent teleport)
		bot.currentPos = kAdventurerStoneForcefield;
		Position homeTemple = bot.currentPos; // fallback
		auto poiIt = cityPOIs_.find(bot.advStoneStartTownId);
		if (poiIt != cityPOIs_.end()) {
			for (const auto &poi : poiIt->second) {
				if (poi.type == POIType::TEMPLE) { homeTemple = poi.pos; break; }
			}
		}
		bot.currentPos = homeTemple;
		bot.advStoneActive = false;
		bot.advStonePhase = 0;
		bot.advStoneRouteIdx = 0;
		bot.advStoneIdleAt = 0;
		bot.advStoneDwellUntil = 0;
		bot.advStoneDeadline = 0;
		bot.advStoneDwellMode = 0;
		bot.advStoneDwellWeaponId = 0;
		bot.advStoneDwellTarget = Position();
		// 2026-06-10 fix: mirror endAdventurerStoneTrip — allow this bot to re-pick
		// AdvStone on its next POI roll without waiting for full visitedPOIs cycle.
		// Keeps v2 simulator and live engine paths in sync.
		bot.visitedPOIs.erase("adventurer_stone");
		bot.state = BotAIState::IDLE;
		bot.nextRerollTime = OTSYS_TIME() + uniform_random(30, 120) * 1000LL;
	}
}

// Phase 3: virtualTryStartHunt — null-Player-safe mirror of tryStartHunt's selection
// path. Picks an eligible script by level + vocation + reservation status, reserves
// it, and sets up hunt state. Skips the live path's teleport/waypoint setup —
// the virtual ticker walks waypoints from currentPos at 4 tiles/sec.
bool BotEngine::virtualTryStartHunt(BotState &bot) {
	if (bot.cachedLevel == 0) return false; // no cached level → can't filter

	int32_t level = static_cast<int32_t>(bot.cachedLevel);
	std::string wantedCategory = bot.isQuestBot ? "quest" : "hunt";

	std::vector<const HuntScript *> eligible;
	auto match = [&](const HuntScript &s, const std::string &cat) {
		if (!s.enabled) return false;
		if (s.patrolWaypoints.empty()) return false;
		if (s.scriptCategory != cat) return false;
		if (s.levelMin > 0 && level < static_cast<int32_t>(s.levelMin)) return false;
		if (s.levelMax > 0 && level > static_cast<int32_t>(s.levelMax)) return false;
		// Quests are shared under a cooldown, not reserved — mirrors live tryStartHunt.
		if (s.isQuest || s.scriptCategory == "quest") {
			if (botQuestOnCooldown(s.id)) return false;
		} else {
			if (activeHunts_.count(s.id)) return false;
			if (!s.spawnGroup.empty() && activeSpawnGroups_.count(s.spawnGroup)) return false;
		}
		if (isScriptPlayerClaimed(s.id, s.spawnGroup)) return false; // player spawn-claim
		if (isScriptHuntRepelled(s)) return false; // hunt-flagged player nearby
		// Empty targetNames is allowed for non-quest hunts — bot attacks all monsters during PATROLLING.
		return true;
	};
	for (const auto &script : huntScripts_) {
		if (match(script, wantedCategory)) eligible.push_back(&script);
	}
	// Quest bots fall back to regular hunts if no quests are available
	if (eligible.empty() && bot.isQuestBot) {
		for (const auto &script : huntScripts_) {
			if (match(script, "hunt")) eligible.push_back(&script);
		}
	}
	if (eligible.empty()) return false;

	// Player-proximity weighting (2026-06-15): bias toward hunts whose patrol waypoints are
	// near a real-player / cast-watched-bot anchor. Empty-anchor / disabled → uniform (unchanged).
	const HuntScript *selected;
	if (!livenessCfg_.proxEnabled || currentAnchorPts_.empty()) {
		selected = eligible[uniform_random(0, static_cast<int>(eligible.size()) - 1)];
	} else {
		std::vector<int32_t> w(eligible.size());
		std::vector<int32_t> dist(eligible.size());
		for (size_t i = 0; i < eligible.size(); ++i) {
			dist[i] = sampledMinChebForScript(*eligible[i]);
			w[i] = livenessCfg_.proxBaselineWeight + proximityBonus(dist[i]);
			if (dist[i] >= 0 && dist[i] <= livenessCfg_.proxNearTiles) s_proxHuntNearEligible++;
		}
		const size_t pick = weightedPick(w);
		selected = eligible[pick];
		recordProxSelection(dist[pick], "hunt", bot.guid, selected->name);
		// Reservation-throttle diagnostic: count near-player scripts already reserved.
		for (const auto &s : huntScripts_) {
			if (!s.enabled || s.patrolWaypoints.empty() || s.scriptCategory != wantedCategory) continue;
			if (s.levelMin > 0 && level < static_cast<int32_t>(s.levelMin)) continue;
			if (s.levelMax > 0 && level > static_cast<int32_t>(s.levelMax)) continue;
			if (!(activeHunts_.count(s.id) || (!s.spawnGroup.empty() && activeSpawnGroups_.count(s.spawnGroup)) || isScriptPlayerClaimed(s.id, s.spawnGroup))) continue;
			const int32_t d = sampledMinChebForScript(s);
			if (d >= 0 && d <= livenessCfg_.proxNearTiles) s_proxHuntNearReserved++;
		}
	}

	// Reserve (hunts) or stamp the shared-quest cooldown
	const bool selectedIsQuest = selected->isQuest || selected->scriptCategory == "quest";
	if (selectedIsQuest) {
		botStampQuestStart(selected->id);
	} else {
		activeHunts_[selected->id] = bot.guid;
		if (!selected->spawnGroup.empty()) activeSpawnGroups_[selected->spawnGroup] = bot.guid;
	}

	// Set up hunt state — mirrors live tryStartHunt but skips engine ops
	bot.huntScriptId = selected->id;
	logHuntAssign(bot, selected->id);
	bot.huntTownId = selected->townId;
	bot.huntStartTime = OTSYS_TIME();
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

	// Start in PREPARING (live path uses beginHuntPhase; we replicate the timer setup here).
	// If hunt is in different town, we'd live-call startTravel — but virtual mode just
	// snaps the bot's townId at the end of TRAVEL_TO via the patrol-wp jump. Acceptable.
	bot.state = BotAIState::HUNTING;
	bot.huntPhase = HuntPhase::PREPARING;
	bot.prepareStartTime = OTSYS_TIME();
	bot.prepareStep = 0;
	bot.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
	bot.prepareHasTarget = false;
	g_logger().info("[BotEngine] virtual: bot guid={} starting hunt '{}' (town {}, level {})",
		bot.guid, selected->name, selected->townId, level);
	return true;
}

// Phase 3: virtualTryStartPartyHunt — null-Player-safe mirror of tryStartPartyHunt.
// Hibernated EK rolls into party-hunt window → form a "virtual party" entirely in static
// maps + BotState fields. No wakeBot, no Party::create, no internalTeleport. Supports
// stay hibernated; their virtualAdvancePartyHunt pins their currentPos to the leader.
// On observe (cast viewer / proximity / admin cmd), wakeBot's cascade materializes the
// whole team together via materializeCanaryParty.
bool BotEngine::virtualTryStartPartyHunt(BotState &bot, int32_t forceScriptId) {
	// ROUND2 E: ANY vocation may initiate, and the leader is ELECTED (EK > RP > initiator) using
	// the SAME shared helpers as the live path — recruitment shape and election order live in one
	// place so the two formation twins cannot drift. This is the dominant path: ~95% of parties
	// form here, so a change that only touched the live side would be largely inert in production.
	if (bot.partyHuntId > 0 || s_botToPartyHunt.count(bot.guid)) return false;
	if (bot.cachedLevel == 0) return false;
	const int32_t initLevel = static_cast<int32_t>(bot.cachedLevel);
	if (initLevel < PARTY_HUNT_MIN_LEVEL) return false;

	g_logger().info("[BotEngine] PARTYHUNT-VTRY: '{}' lv{} voc={} (forced={})",
		bot.name, initLevel, getBaseVocation(bot.vocationId), forceScriptId);

	size_t rosterBestEd = 0, rosterBestMs = 0, rosterBestRp = 0;
	const PartyRoster roster = recruitPartyRoster(bot, initLevel, &rosterBestEd, &rosterBestMs, &rosterBestRp);
	const uint32_t leaderGuid = electPartyLeader(roster, bot.guid);
	if (roster.supportCount(leaderGuid) == 0) {
		g_logger().info("[BotEngine] PARTYHUNT-VFAIL: '{}' no supports (best ed={}, ms={}, rp={})",
			bot.name, rosterBestEd, rosterBestMs, rosterBestRp);
		return false;
	}

	// BOT_PARTY_CAP: same gate, same position (post-election, pre-mutation). The virtual path is
	// ~95%% of formations, so a live-only cap would be nearly inert in production.
	if (forceScriptId == 0 && !partyCapAllows(1 + static_cast<uint32_t>(roster.supportCount(leaderGuid)), "virtual")) {
		return false;
	}

	// No wake obligations here — everyone stays hibernated. The elected leader is simply the
	// BotState that receives the hunt fields and isPartyHuntLeader.
	BotState* leaderPtr = &bot;
	if (leaderGuid != bot.guid) {
		auto it = guidToIndex_.find(leaderGuid);
		if (it == guidToIndex_.end()) {
			return false;
		}
		leaderPtr = &bots_[it->second];
		if (leaderPtr->cachedLevel == 0) {
			leaderPtr = &bot; // demote: no usable level for the elected leader
		}
	}
	BotState& leader = *leaderPtr;
	const uint32_t initiatorGuid = bot.guid;
	const int32_t leaderLevel = static_cast<int32_t>(leader.cachedLevel);

	// The elected leader may itself be running a virtualSim hunt — release it before it takes the
	// party script, exactly as setupVirtualSupport does for supports.
	if (leader.guid != initiatorGuid && leader.huntScriptId > 0) {
		activeHunts_.erase(leader.huntScriptId);
		for (const auto& hs : huntScripts_) {
			if (hs.id == leader.huntScriptId && !hs.spawnGroup.empty()) {
				activeSpawnGroups_.erase(hs.spawnGroup);
				break;
			}
		}
		leader.huntScriptId = 0;
	}

	// Find eligible scripts — same filters and per-leader tolerance as the live path.
	std::vector<const HuntScript*> eligible;
	for (const auto& script : huntScripts_) {
		if (forceScriptId > 0) {
			if (static_cast<int32_t>(script.id) != forceScriptId) continue;
		} else {
			if (!script.enabled) continue;
			if (script.patrolWaypoints.empty()) continue;
			// Hunts only — the same filter tryStartPartyHunt has (bot_party.cpp). This loop is
			// otherwise a verbatim copy of the live one, and it was the ONLY copy that never got
			// the filter: a party EK could still be handed a quest walkthrough, run it on the
			// uncapped PARTY_HUNT_TIME clock below, and "finish" it after a single truncated pass.
			// Since the virtual sim is the dominant path for hibernated bots, the live-side fix
			// was largely inert in production without this.
			if (script.scriptCategory != "hunt") continue;
			// ROUND2 E: same per-leader-vocation tolerance as the live path (shared helper).
			const PartyLevelTolerance tol = partyLevelToleranceFor(getBaseVocation(leader.vocationId));
			int32_t effectiveLevel = leaderLevel * tol.num / tol.den;
			if (script.levelMin > 0 && effectiveLevel < static_cast<int32_t>(script.levelMin)) continue;
			if (script.levelMax > 0 && leaderLevel > static_cast<int32_t>(script.levelMax)) continue;
			if (activeHunts_.count(script.id)) continue;
			if (!script.spawnGroup.empty() && activeSpawnGroups_.count(script.spawnGroup)) continue;
			if (isScriptPlayerClaimed(script.id, script.spawnGroup)) continue; // player spawn-claim
			if (isScriptHuntRepelled(script)) continue; // hunt-flagged player nearby
		}
		eligible.push_back(&script);
	}

	if (eligible.empty()) {
		g_logger().info("[BotEngine] PARTYHUNT-VFAIL: '{}' no eligible scripts", leader.name);
		return false;
	}

	auto rng = std::mt19937(std::random_device{}());
	std::shuffle(eligible.begin(), eligible.end(), rng);

	size_t bestEdCount = rosterBestEd, bestMsCount = rosterBestMs, bestRpCount = rosterBestRp;

	// ROUND2 E: roster recruited once before the election (shared helper) — the per-script retry
	// was vestigial. Supports are every roster member except the elected leader, which includes
	// the initiator when it lost the election.
	const uint32_t edGuid = (roster.ed != leader.guid) ? roster.ed : 0;
	const uint32_t msGuid = (roster.ms != leader.guid) ? roster.ms : 0;
	const uint32_t rpGuid = (roster.rp != leader.guid) ? roster.rp : 0;
	for (const auto* script : eligible) {

		// Form party — purely in static maps + BotState, no engine ops.
		uint32_t partyHuntId = s_nextPartyHuntId++;

		// Reserve hunt script
		activeHunts_[script->id] = leader.guid;
		if (!script->spawnGroup.empty()) {
			activeSpawnGroups_[script->spawnGroup] = leader.guid;
		}

		// Setup EK (virtual leader) — virtualAdvanceHunting drives him through phases
		leader.huntScriptId = script->id;
		leader.huntTownId = script->townId;
		leader.huntStartTime = OTSYS_TIME();
		leader.huntEndTime = leader.huntStartTime + uniform_random(PARTY_HUNT_TIME_MIN, PARTY_HUNT_TIME_MAX) * 1000LL;
		leader.huntKillCount = 0;
		leader.huntWaypointIdx = 0;
		leader.huntPatrolCycles = 0;
		leader.huntTargetId = 0;
		leader.huntChaseFailCount = 0;
		leader.huntIgnoredMonsters.clear();
		leader.huntWaypointSkipCount = 0;
		leader.huntPhase = HuntPhase::PREPARING;
		leader.state = BotAIState::HUNTING;
		leader.prepareStartTime = OTSYS_TIME();
		leader.prepareStep = 0;
		leader.prepareWaitUntil = OTSYS_TIME() + uniform_random(30, 90) * 1000LL;
		leader.prepareHasTarget = false;
		leader.partyHuntId = partyHuntId;
		leader.partyRole = PARTY_ROLE_TANK;
		leader.partyLeaderGuid = leader.guid;
		leader.isPartyHuntLeader = true;

		// Register in static maps
		s_partyHuntLeaderGuid[partyHuntId] = leader.guid;
		s_partyHuntScript[partyHuntId] = script->id;
		s_partyHuntDeathCount[partyHuntId] = 0;
		s_partyHuntKillCount[partyHuntId] = 0;
		s_botToPartyHunt[leader.guid] = partyHuntId;

		std::vector<uint32_t> members;
		members.push_back(leader.guid);

		// Setup virtual supports — pinned to leader by virtualAdvancePartyHunt.
		auto setupVirtualSupport = [&](uint32_t guid, uint8_t role) {
			auto it = guidToIndex_.find(guid);
			if (it == guidToIndex_.end()) return;
			auto& supportBot = bots_[it->second];

			s_partyWasInactive[guid] = !supportBot.active;

			// Release any virtualSim hunt the support was running
			if (supportBot.huntScriptId > 0) {
				activeHunts_.erase(supportBot.huntScriptId);
				for (const auto& hs : huntScripts_) {
					if (hs.id == supportBot.huntScriptId && !hs.spawnGroup.empty()) {
						activeSpawnGroups_.erase(hs.spawnGroup);
						break;
					}
				}
				supportBot.huntScriptId = 0;
			}
			// The virtual twin of the normalisation in formPartyWithLeader's setupSupport. This
			// path is the DOMINANT one — most parties form while every member is hibernated — so a
			// fix applied only to the live handler would have been largely inert in production.
			supportBot.huntPhase = HuntPhase::PREPARING;
			supportBot.huntWaypointIdx = 0;
			supportBot.huntWaypointSkipCount = 0;
			supportBot.huntKillCount = 0;
			supportBot.huntIgnoredMonsters.clear();

			// Defensive: a candidate that began an AdvStone trip in the selection→setup gap
			// would keep running doAdventurerStone (preempts the state switch) and never
			// follow. findBotsForParty already excludes advStoneActive; this closes the race.
			clearAdvStoneState(supportBot);
			clearFishingRun(supportBot.guid); // closes the same selection->setup race
			endHouseVisit(supportBot.guid, "party_conscripted"); // same race, same reason

			supportBot.state = BotAIState::PARTY;
			supportBot.partyHuntId = partyHuntId;
			supportBot.partyRole = role;
			supportBot.partyLeaderGuid = leader.guid;
			supportBot.isPartyHuntLeader = false;
			supportBot.huntTargetId = 0;
			supportBot.hasWalkTarget = false;
			supportBot.followingCityRoute = false;
			supportBot.cityRouteWps.clear();
			supportBot.cityRouteIdx = 0;
			s_botToPartyHunt[guid] = partyHuntId;
			members.push_back(guid);
		};

		if (edGuid) setupVirtualSupport(edGuid, PARTY_ROLE_HEALER);
		if (msGuid) setupVirtualSupport(msGuid, PARTY_ROLE_DPS_MAGE);
		if (rpGuid) setupVirtualSupport(rpGuid, PARTY_ROLE_DPS_RANGED);

		s_partyHuntMembers[partyHuntId] = members;

		std::string memberNames;
		for (uint32_t guid : members) {
			auto mIt = guidToIndex_.find(guid);
			if (mIt == guidToIndex_.end()) continue;
			const auto& mBot = bots_[mIt->second];
			if (!memberNames.empty()) memberNames += ", ";
			memberNames += mBot.name;
			memberNames += "(";
			memberNames += mBot.partyRole == PARTY_ROLE_TANK ? "EK" :
				mBot.partyRole == PARTY_ROLE_HEALER ? "ED" :
				mBot.partyRole == PARTY_ROLE_DPS_MAGE ? "MS" : "RP";
			memberNames += ")";
		}

		g_logger().info("[BotEngine] PARTY_HUNT #{} virtual-formed: '{}' [{}] — {} virtual members: {} (leader='{}' voc={} elected={}, initiator guid={})",
			partyHuntId, script->name, script->id, members.size(), memberNames,
			leader.name, getBaseVocation(leader.vocationId),
			leader.guid == initiatorGuid ? "INITIATOR" : (getBaseVocation(leader.vocationId) == 4 ? "EK" : "RP"),
			initiatorGuid);

		return true;
	}

	g_logger().info("[BotEngine] PARTYHUNT-VFAIL: '{}' lv{} no supports across {} scripts (best ed={}, ms={}, rp={})",
		leader.name, leaderLevel, eligible.size(), bestEdCount, bestMsCount, bestRpCount);
	return false;
}

// Dissolve a virtual party — clears all static maps + BotState party fields without
// touching Canary engine ops (no exitPartyMode, no Party::disband). Used when the
// virtual leader transitions to LEAVING/RESUPPLYING.
void BotEngine::dissolveVirtualPartyHunt(uint32_t partyHuntId, const std::string& reason) {
	auto membersIt = s_partyHuntMembers.find(partyHuntId);
	if (membersIt == s_partyHuntMembers.end()) return;

	auto members = membersIt->second; // copy — we'll modify the map
	g_logger().info("[BotEngine] PARTY_HUNT #{} virtual-dissolved: {}", partyHuntId, reason);

	auto leaderIt = s_partyHuntLeaderGuid.find(partyHuntId);
	uint32_t leaderGuid = (leaderIt != s_partyHuntLeaderGuid.end()) ? leaderIt->second : 0;

	for (uint32_t guid : members) {
		// BOT_PARTY_CAP (ratchet insurance): unconditional erase BEFORE the guidToIndex_ guard —
		// s_botToPartyHunt.size() is the cap numerator, so a stranded entry would permanently
		// consume cap headroom. No-op when already absent; the erase below stays harmlessly.
		s_botToPartyHunt.erase(guid);
		auto it = guidToIndex_.find(guid);
		if (it == guidToIndex_.end()) continue;
		auto& mBot = bots_[it->second];

		mBot.partyHuntId = 0;
		mBot.partyRole = 0;
		mBot.partyLeaderGuid = 0;
		mBot.isPartyHuntLeader = false;
		s_botToPartyHunt.erase(guid);
		s_partyLeaderId.erase(guid); // BOT_PARTY_LEAK_FIX: was never erased here — a stale entry
		                             // hid genuine leaks from any s_partyLeaderId-based check
		s_followerLastLeaderZ.erase(guid);
		s_followerZChangeDetected.erase(guid);
		s_followerSeparatedSince.erase(guid);
		s_partyFollowTeleportCooldown.erase(guid);
		s_followerLastTeleLeaderZ.erase(guid);
		s_followerLeaderZStamp.erase(guid);
		s_partyFormationOffset.erase(guid); // P8 inc2
		s_lastSlotRollMs.erase(guid);

		// Supports go back to IDLE (re-roll naturally). Leader keeps huntScriptId so
		// virtualAdvanceHunting can continue him through LEAVING/RESUPPLYING solo.
		if (guid != leaderGuid) {
			mBot.state = BotAIState::IDLE;
			mBot.nextRerollTime = OTSYS_TIME() + uniform_random(5, 30) * 1000;
		}
	}

	s_partyHuntMembers.erase(partyHuntId);
	s_partyHuntScript.erase(partyHuntId);
	s_partyHuntLeaderGuid.erase(partyHuntId);
	s_partyHuntDeathCount.erase(partyHuntId);
	s_partyHuntKillCount.erase(partyHuntId);
}

// Called from wakeBot's cascade after all party members are live. Builds the Canary
// Party retroactively so the team is visible/functional in the game world.
void BotEngine::materializeCanaryParty(uint32_t partyHuntId) {
	auto leaderIt = s_partyHuntLeaderGuid.find(partyHuntId);
	if (leaderIt == s_partyHuntLeaderGuid.end()) return;
	uint32_t leaderGuid = leaderIt->second;

	auto lIt = guidToIndex_.find(leaderGuid);
	if (lIt == guidToIndex_.end()) return;
	auto& leaderBot = bots_[lIt->second];
	auto leaderPlayer = leaderBot.getPlayer();
	if (!leaderPlayer) {
		g_logger().warn("[BotEngine] materializeCanaryParty #{}: leader has no Player", partyHuntId);
		return;
	}

	// If a Party already exists AND we lead it (live formation path already built one), skip.
	// BOT_PARTY_LEAK_FIX: a woken leader carrying a STALE party used to make this return silently,
	// so the hunt ran with a phantom party and the supports never joined.
	if (auto existing = leaderPlayer->getParty()) {
		if (existing->getLeader() == leaderPlayer) return;
		reclaimStaleCanaryParty(leaderGuid, "materialize_stale");
		if (leaderPlayer->getParty()) return; // could not reclaim — do not build a second party
	}

	auto party = Party::create(leaderPlayer);
	if (!party) {
		g_logger().warn("[BotEngine] materializeCanaryParty #{}: Party::create returned null", partyHuntId);
		return;
	}

	auto membersIt = s_partyHuntMembers.find(partyHuntId);
	if (membersIt == s_partyHuntMembers.end()) return;

	// FC-safe placement: route support teleports through chooseWakePosition so none land on
	// the leader's ladder/hole/teleport tile or stack on the leader's exact tile (the Cormaya
	// bug). materializeCanaryParty runs inside the wakeBot cascade, whose per-member wakes
	// already populated burstReservedTiles_ via chooseWakePosition — reuse it; self-heal only
	// for a cold caller where every member was already awake (no per-member wake fired).
	if (burstReservedTiles_.empty()) beginWakeBurst();
	// Reserve the leader's own tile so a support never displaces onto it / its descent tile.
	burstReservedTiles_.insert(packPosU64(leaderBot.currentPos));

	uint32_t joined = 0;
	for (uint32_t guid : membersIt->second) {
		if (guid == leaderGuid) continue;
		auto mIt = guidToIndex_.find(guid);
		if (mIt == guidToIndex_.end()) continue;
		auto& supportBot = bots_[mIt->second];
		auto supportPlayer = supportBot.getPlayer();
		if (!supportPlayer) continue;

		// FC-safe placement near the leader (immediate, not an off-screen walk-in → false)
		Position placeAt = chooseWakePosition(supportBot, leaderBot.currentPos, /*proximityWake=*/false);
		BOT_TELEPORT(supportPlayer, placeAt, true);
		supportBot.currentPos = placeAt;
		supportBot.lastPos = placeAt;
		supportPlayer->setSecureMode(true);

		party->invitePlayer(supportPlayer);
		party->joinParty(supportPlayer);
		joined++;
	}

	party->setSharedExperience(leaderPlayer, true);
	g_logger().info("[BotEngine] PARTY_HUNT #{} materialized: leader='{}' joined={} supports",
		partyHuntId, leaderPlayer->getName(), joined);
}

void BotEngine::forceDeactivateBot(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return;

	auto& bot = bots_[it->second];

	// Adv Stone trip / training cleanup before we tear down the player. setTraining(false)
	// stops the Lua exercise-training addEvent loop from outliving the player reference.
	if (bot.advStoneActive) {
		endAdventurerStoneTrip(bot);
	} else {
		// Defensive: training flag without active trip (shouldn't happen, but cheap insurance).
		stopAdvStoneTrainingIfActive(bot);
	}
	// Same for house dummy training, which starts the SAME Lua loop against a house's dummy.
	// clearFishingRun is deliberately NOT wired into this teardown and does not need to be —
	// fishing has no Lua-side loop to outlive it — which is exactly why "wire it wherever
	// clearFishingRun is wired" would have missed the one path that matters here.
	stopHouseTrainingIfActive(bot);
	endHouseVisit(guid, "force_deactivate");
	endShrineVisit(guid, "force_deactivate");

	auto& db = Database::getInstance();

	// If bot has a hunt reservation, save its state to DB so it can resume on reactivation.
	// Do NOT erase the reservation from activeHunts_ — keep the spawn locked for this bot.
	if (bot.huntScriptId > 0) {
		saveSingleBotState(bot);
		// Keep activeHunts_[huntScriptId] = guid — spawn stays reserved
		// huntScriptId kept in BotState for reservation tracking
	} else {
		// No hunt reservation — nothing to save or preserve
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
	s_travelFcRecoveryCount.erase(guid);
	s_travelStartTime.erase(guid);
	s_travelDestPOI.erase(guid);
	s_travelArriveTarget.erase(guid);
	s_travelSrcPOI.erase(guid);
	s_lastRouteEndPos.erase(guid);
	walkPauseInfo_.erase(guid);
	clearDepotBlacklist(guid);

	// Cleanup party hunt state if bot was in autonomous party hunt
	if (bot.partyHuntId > 0) {
		if (bot.isPartyHuntLeader) {
			dissolvePartyHunt(bot.partyHuntId, "force_deactivate");
		} else {
			exitPartyHuntMode(bot);
		}
	}

	// Cleanup party state if bot was in party (human-led)
	if (bot.state == BotAIState::PARTY) {
		auto player = bot.getPlayer();
		if (player) {
			auto party = player->getParty();
			if (party) {
				party->leaveParty(player, true);
			}
		}
		s_partyLeaderId.erase(guid);
		s_lastLeaderPos.erase(guid);
		s_lastPartyHealTime.erase(guid);
		s_partyWasInactive.erase(guid);
		s_partyPrevSecureMode.erase(guid);
	}

	// BOT_PARTY_LEAK_FIX: the block above is keyed on state==PARTY (and the one before it on
	// partyHuntId), so a bot holding a STALE Canary party — engine state already zero — matched
	// neither and had its Player destroyed with the party still attached. If it was the Canary
	// leader that party becomes unreclaimable by any stock call. Last exit, covers both.
	reclaimStaleCanaryParty(guid, "forceDeactivateBot");

	// True offline: remove bot from game world
	auto player = bot.getPlayer();
	if (player) {
		// Save current position to players table before removing
		auto pos = player->getPosition();
		g_botDatabaseTasks().execute(fmt::format(
			"UPDATE `players` SET `posx`={}, `posy`={}, `posz`={} WHERE `id`={}",
			pos.x, pos.y, pos.z, guid));

		player->setCastBroadcasting(false);
		// JITTER FIX: async DELETE (was sync executeQuery — see hibernateBot fix).
		g_botDatabaseTasks().execute(fmt::format("DELETE FROM `cast_broadcasters` WHERE `player_id` = {}", guid));
		g_game().removeCreature(player, false);
		g_game().removePlayer(player);
		player->setOnline(false);
		bot.playerRef = std::weak_ptr<Player>();
	}

	// Drop any hibernation-pool ref so the Player destructor fires on shutdown
	// (deactivateAll calls this for every active bot, including hibernated ones since
	// they have active=true). Without this, pool keeps Players alive past shutdown.
	auto poolIt = hibernationPool_.find(guid);
	if (poolIt != hibernationPool_.end()) {
		auto poolPlayer = poolIt->second;
		hibernationPool_.erase(poolIt);
		if (poolPlayer) {
			poolPlayer->setOnline(false);
			// Save position for next-boot DB load (pool is in-memory only, lost on restart)
			auto pos = poolPlayer->getPosition();
			g_botDatabaseTasks().execute(fmt::format(
				"UPDATE `players` SET `posx`={}, `posy`={}, `posz`={} WHERE `id`={}",
				pos.x, pos.y, pos.z, guid));
		}
	}
	bot.hibernated = false;

	bot.active = false;
	bot.state = BotAIState::INACTIVE;
	bot.townId = 0;
	bot.townName.clear();
	// Note: huntScriptId is intentionally NOT cleared if a hunt reservation was saved
	// It stays set so the reservation in activeHunts_ remains associated with this bot
	bot.lastDeathTime = OTSYS_TIME();
	bot.deathPauseUntil = 0;
	bot.recoveryWaypoints.clear();
	bot.isRecoveryRoute = false;
	s_targetHpTracker.erase(guid);
	s_lastTrackedTargetId.erase(guid);
	s_lastZChangeTime.erase(guid);
	s_lastMoveTime.erase(guid);
	s_lastZPursuitTime.erase(guid);
	s_targetLastSameZPos.erase(guid);
	s_lastAttackerSeenTime.erase(guid);
	s_combatNoTargetSince.erase(guid);
	s_targetTrail.erase(guid);
	s_inRangeSince.erase(guid);
	s_inRangeAttackSnapshot.erase(guid);
	s_retreatUntil.erase(guid);
	s_lastFoughtCreature.erase(guid);
	s_lastCombatExitTime.erase(guid);
	s_returnPos.erase(guid);
	s_returnStartTime.erase(guid);
	s_reengageTarget.erase(guid);
	s_reengageUntil.erase(guid);
	s_pvpDanceCd.erase(guid);
	s_pvpHasteCd.erase(guid);
	s_pvpWallCd.erase(guid);
	s_pvpPendingWall.erase(guid);
	leaveGang(guid); // Feature 1: release gang membership + skull reservation
}

void BotEngine::forceDeactivateBotForReload(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return;

	auto& bot = bots_[it->second];

	// Adv Stone trip cleanup before hot-reload tears down the engine. Critical: the Lua
	// `exerciseTrainingEvent` addEvent loop lives in main-binary Lua state and survives
	// dlclose/dlopen — without setTraining(false) here, the loop would run forever after
	// reload, hitting the dummy from wherever the bot ended up.
	if (bot.advStoneActive) {
		endAdventurerStoneTrip(bot);
	} else {
		stopAdvStoneTrainingIfActive(bot);
	}
	// House dummy training starts that same main-binary Lua loop, so it needs the same treatment
	// on the reload path. The visit itself is ended too: the run lives in a .so-local map that
	// dlclose destroys, so leaving it would strand the tile/dummy/occupancy claims for a run
	// nothing can finish.
	stopHouseTrainingIfActive(bot);
	endHouseVisit(guid, "reload");
	endShrineVisit(guid, "reload");

	// Release hunt reservation if hunting
	if (bot.huntScriptId > 0) {
		activeHunts_.erase(bot.huntScriptId);
		for (auto& hs : huntScripts_) {
			if (hs.id == bot.huntScriptId && !hs.spawnGroup.empty()) {
				activeSpawnGroups_.erase(hs.spawnGroup);
				break;
			}
		}
	}

	// BOT_PARTY_INVITE_RENDEZVOUS trap #11 — a HUMAN-led party member must leave its Canary party
	// HERE, while its Player is still in the world and leaveParty can do its job. All the engine
	// bookkeeping (s_partyLeaderId, s_rvMember, the assembly record) lives in .so-local statics
	// that dlclose destroys, so after reload nothing would remember the membership — but Party
	// itself lives in the main binary and would keep the bot in the human's memberList forever.
	// reclaimStaleCanaryParty cannot help: it refuses any party containing a real player, which
	// is exactly this case. Pre-existing for today's /party bots; walk-in makes it routine.
	if (s_partyLeaderId.count(guid) > 0 && s_botToPartyHunt.count(guid) == 0) {
		if (auto partyPlayer = bot.getPlayer()) {
			if (auto party = partyPlayer->getParty()) {
				party->leaveParty(partyPlayer, true);
				g_logger().info("[BotEngine] reload drain: '{}' left its human-led party", bot.name);
			}
			// Pending invitation too — the same isLogout=false hole as hibernate/deactivate.
			partyPlayer->clearPartyInvitations();
		}
	}
	s_partyLeaderId.erase(guid);
	s_pendingInvites.erase(guid);
	s_inviteDebugKeepAlive.erase(guid);
	dropAssemblyMember(guid, "reload");

	// NOTE: Do NOT touch cast broadcasting or teleport — bot stays in place
	// Cast viewers remain connected through the .so reload

	bot.active = false;
	bot.state = BotAIState::INACTIVE;
	bot.townId = 0;
	bot.townName.clear();
	bot.huntScriptId = 0;
	bot.recoveryWaypoints.clear();
	bot.isRecoveryRoute = false;
	leaveGang(guid); // Feature 1: drop gang membership before reload (statics reset anyway)
}

void BotEngine::pauseBotForDeath(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return;

	auto& bot = bots_[it->second];

	// Save current state so we can resume after the pause
	bot.preDeathState = bot.state;
	// 1 second offline pause (server processes death: despawn, condition cleanup, skull clearing)
	bot.deathPauseUntil = OTSYS_TIME() + 1000;
	bot.lastDeathTime = OTSYS_TIME();

	// Clear combat/target state but preserve hunt/travel state
	auto player = bot.getPlayer();
	if (player) {
		player->setAttackedCreature(nullptr);
		// Keep a strong reference to prevent Player destruction after despawn()
		dyingBots_[guid] = player;
	}
	bot.attackerId = 0;
	bot.pkTarget = 0;
	bot.huntTargetId = 0;
	bot.huntChaseFailCount = 0;

	// Adventurer's Stone trip cleanup: bot dying mid-trip would otherwise keep
	// advStoneActive=true after respawn, and doAdventurerStone() would try to drive
	// a freshly-respawned bot back through the dungeon. Abort cleanly instead.
	if (bot.advStoneActive) {
		castLog(bot, "DEATH: aborting Adventurer's Stone trip");
		endAdventurerStoneTrip(bot);
	}

	// Clear stale return walk state — bot will respawn at temple, not pre-combat position
	// A bot that dies mid-visit respawns at the temple and would otherwise resume walking to the
	// same NPC across town, still holding a claim on that approach tile. Drop the walk and the
	// claim together.
	bot.hasWalkTarget = false;
	bot.currentPOI = nullptr;
	clearPlannerWalk(guid);
	clearFishingRun(guid);
	endHouseVisit(guid, "deactivate");
	endShrineVisit(guid, "deactivate");
	s_returnPos.erase(guid);
	s_returnStartTime.erase(guid);
	s_retreatUntil.erase(guid);
	s_pvpDanceCd.erase(guid);
	s_pvpHasteCd.erase(guid);
	s_pvpWallCd.erase(guid);
	s_pvpPendingWall.erase(guid);
	s_gangVictimLastDist.erase(guid);
	s_gangFleeStreak.erase(guid);
	s_gangBoxRollNext.erase(guid);
	s_gangVictimLastPos.erase(guid);
	// TRAIL: a dead follower's cursor/session must not survive into the respawn (per-follower
	// state; the shared leader-keyed trail lives on for the surviving siblings).
	s_followerCursor.erase(guid);
	s_followerZHopSession.erase(guid);
	leaveGang(guid); // Feature 1: release gang membership + skull reservation on death

	// Party hunt death handling
	if (bot.partyHuntId > 0) {
		auto dcIt = s_partyHuntDeathCount.find(bot.partyHuntId);
		if (dcIt != s_partyHuntDeathCount.end()) {
			dcIt->second++;
		}

		if (bot.isPartyHuntLeader) {
			// EK leader died → dissolve the entire party hunt
			castLog(bot, "DEATH: Party hunt EK leader died, dissolving party");
			dissolvePartyHunt(bot.partyHuntId, "leader_death");
		} else {
			// Support bot died → keep partyHuntId so we can re-join after respawn
			castLog(bot, fmt::format("DEATH: Party hunt support bot died (partyHuntId={}), will re-join after respawn",
				bot.partyHuntId));
		}
	}

	castLog(bot, fmt::format("DEATH: Server handling death, will re-login in 1s then chill at temple, resume state {}",
		botStateName(bot.preDeathState)));
}

void BotEngine::setBotAIPaused(uint32_t guid, bool paused) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return;
	bots_[it->second].aiPaused = paused;
}

void BotEngine::setAllBotsAIPaused(bool paused) {
	uint32_t affected = 0;
	for (auto& bot : bots_) {
		if (bot.active) {
			bot.aiPaused = paused;
			affected++;
		}
	}
	g_logger().info("[BotEngine] setAllBotsAIPaused({}) — applied to {} active bots", paused ? "true" : "false", affected);
}

bool BotEngine::reactivateBotForReload(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;

	auto& bot = bots_[it->second];
	if (bot.active) return false;

	auto player = bot.getPlayer();
	if (!player) return false;

	bot.active = true;
	bot.state = BotAIState::IDLE;
	bot.tickCounter = botInitialTickPhase(bot.guid);  // guid-phased (Phase 1) — not 0
	s_depotLockerRerollTime.erase(bot.guid);
	s_depotDwellWalkTarget.erase(bot.guid);
	s_depotDwellWalkFails.erase(bot.guid);
	s_routeProgress.erase(bot.guid);
	s_lastFcPositions.erase(bot.guid);
	s_walkTargetTimer.erase(bot.guid);
	s_travelFcRecoveryCount.erase(bot.guid);
	s_travelStartTime.erase(bot.guid); s_travelDestPOI.erase(bot.guid); s_travelSrcPOI.erase(bot.guid); s_lastRouteEndPos.erase(bot.guid); s_travelArriveTarget.erase(bot.guid);
	s_huntTravelStart.erase(bot.guid);

	// Set town from player's DB assignment BEFORE teleporting
	auto town = player->getTown();
	if (town) {
		bot.townId = town->getID();
		auto nameIt = travelTownNames_.find(town->getID());
		bot.townName = (nameIt != travelTownNames_.end()) ? nameIt->second : town->getName();
	}

	// Teleport to random starting location (same logic as activateBot)
	{
		int spawnRoll = uniform_random(1, 100);
		bool spawned = false;

		auto& allPOIs = getCityPOIs();
		auto poiIt = allPOIs.find(bot.townId);
		Position depotPos;
		if (poiIt != allPOIs.end()) {
			for (const auto& poi : poiIt->second) {
				if (poi.type == POIType::DEPOT) { depotPos = poi.pos; break; }
			}
		}

		if (spawnRoll <= 40 && depotPos.x > 0) {
			// Try radius=1 first for accurate POI placement, fall back to wider radius
			// if no walkable adj tile is free (e.g. cluster of bots already at depot).
			Position tile = findRandomTileNear(depotPos, 1, 1);
			if (tile.x == 0) tile = findRandomTileNear(depotPos, 5, 1);
			if (tile.x > 0) {
				BOT_TELEPORT(player, tile, true);
				bot.currentPos = tile;
				spawned = true;
			}
		} else if (spawnRoll <= 65) {
			auto boatPos = getTravelPosition(bot.townId).first;
			if (boatPos.x > 0) {
				// Same tighten-then-fallback pattern for boat tile.
				Position tile = findRandomTileNear(boatPos, 1);
				if (tile.x == 0) tile = findRandomTileNear(boatPos, 4);
				if (tile.x > 0) {
					BOT_TELEPORT(player, tile, true);
					bot.currentPos = tile;
					spawned = true;
				}
			}
		} else if (spawnRoll <= 85) {
			teleportToTemple(bot);
			spawned = true;
		} else if (depotPos.x > 0) {
			// findPZBoundaryTile keeps radius=8 — its job is finding the PZ boundary
			// edge, which is typically deep into PZ. Tightening to 1 would miss it.
			Position tile = findPZBoundaryTile(depotPos, 8);
			if (tile.x > 0) {
				BOT_TELEPORT(player, tile, true);
				bot.currentPos = tile;
				spawned = true;
			}
		}

		if (!spawned) {
			teleportToTemple(bot);
		}
	}
	// NOTE: Do NOT touch cast — broadcast is still active with viewers connected
	// NOTE: Do NOT reset HP/mana — bot was never killed

	// Clear all AI state so bot starts fresh in the new engine
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

	// Reset logging — auto-enabled when cast viewers join (preserve manual if viewer still watching)
	bot.verboseLog = false;
	bot.verboseLogManual = false;

	player->setFightMode(FIGHTMODE_ATTACK);

	// Re-equip gear (weapon may have decayed or been lost)
	equipBot(bot);

	// Mount roll, mirroring activateBot. Note this always re-rolls rather than hitting the
	// throttle: /cavebot reload destroys the BotEngine (destroyBotEngine before dlclose) and
	// reactivation runs against a fresh instance, so botMountWants_ is empty here. Restyling
	// on an explicit admin reload is fine, and it is also the only way to re-roll on demand
	// without waiting out MOUNT_REROLL_MIN_INTERVAL_MS when testing this.
	rollMountForReconnect(bot, player);

	bot.postActivationReroll = true;

	g_logger().info("[BotEngine] Reactivated bot '{}' for reload (guid={}, pos=({},{},{}))",
		player->getName(), guid, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
	return true;
}

void BotEngine::teleportToTemple(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;
	auto town = player->getTown();
	if (!town) return;

	// Use temple position directly from towns.xml — same as /town command
	auto templePos = town->getTemplePosition();
	BOT_TELEPORT(player, templePos, true);
	bot.currentPos = templePos;

	// Update navigation townId to match actual position
	uint32_t detectedTown = findNearestTown(templePos);
	if (detectedTown > 0) {
		bot.townId = detectedTown;
		auto nameIt = travelTownNames_.find(detectedTown);
		auto detectedTownObj = g_game().map.towns.getTown(detectedTown);
		bot.townName = nameIt != travelTownNames_.end() ? nameIt->second :
			(detectedTownObj ? detectedTownObj->getName() : "");
	}
}

uint32_t BotEngine::findNearestTown(const Position& pos) const {
	// Skip detection while bot is on the Adventurer's Stone island/dungeon — none of the
	// towns geographically nearby (Rookgaard at 112 tiles) are actually reachable from
	// the island via normal travel. Returning 0 means callers (syncTownIdToPos, the
	// auto-detect block at ~line 5168) leave bot.townId unchanged → bot retains its
	// origin town until the Adv Stone return MoveEvent teleports it back to the actual
	// origin temple, at which point this function returns the correct town normally.
	if (isOnAdvStoneIsland(pos)) return 0;

	uint32_t bestId = 0;
	int32_t bestDist = INT32_MAX;
	for (const auto& [id, town] : g_game().map.towns.getTowns()) {
		if (id == 0) continue;
		auto tp = town->getTemplePosition();
		int32_t d = std::max(
			std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(tp.x)),
			std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(tp.y)));
		if (d < bestDist) {
			bestDist = d;
			bestId = id;
		}
	}
	return bestId;
}

void BotEngine::syncTownIdToPos(BotState& bot) {
	// Fixes the townId/currentPos desync that produces "stuck at boat route" bots:
	// activateBot sets townId from player->getTown() (DB home) and `active x,y,z`
	// short-circuit updates currentPos without townId — both leave the field stale
	// relative to the physical position. Stuck bots can't move so the movement-gated
	// auto-detect at line 5144 never re-fires (chicken-and-egg). This helper is the
	// proactive sync called at every site where currentPos changes.
	uint32_t detected = findNearestTown(bot.currentPos);
	if (detected == 0 || detected == bot.townId) return;  // no-op on detection failure or already-correct
	uint32_t oldTownId = bot.townId;
	bot.townId = detected;
	auto nameIt = travelTownNames_.find(detected);
	auto detectedTownObj = g_game().map.towns.getTown(detected);
	bot.townName = nameIt != travelTownNames_.end() ? nameIt->second :
		(detectedTownObj ? detectedTownObj->getName() : "");
	castLog(bot, fmt::format("TOWN_SYNC: pos=({},{},{}) {}->{}",
		bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, oldTownId, detected));
}

// ============================================================================
// Recovery route — navigate back instead of teleporting
// ============================================================================

bool BotEngine::findNearestRecoveryRoute(BotState& bot) {
	const HuntScript* bestScript = nullptr;
	int32_t bestDist = INT32_MAX;
	size_t bestPatrolIdx = 0;

	auto searchScript = [&](const HuntScript& script, bool isTier1) {
		if (script.travelFromWaypoints.empty()) return;
		const bool scriptIsQuest = script.isQuest || script.scriptCategory == "quest";
		// Quests are shared under a cooldown, not reserved (see tryStartHunt), so the
		// reservation map says nothing about whether another bot is running one.
		if (!scriptIsQuest) {
			// Skip if another bot already has this script reserved
			auto ahIt = activeHunts_.find(script.id);
			if (ahIt != activeHunts_.end() && ahIt->second != bot.guid) return;
		}
		// Never recovery-route into (and re-reserve) a player-claimed spawn (B1)
		if (isScriptPlayerClaimed(script.id, script.spawnGroup)) return;
		// 1-bot-per-spawnGroup: a sibling script may hold the physical-spawn lock (B3 — was missing here)
		if (!script.spawnGroup.empty()) {
			auto sgIt = activeSpawnGroups_.find(script.spawnGroup);
			if (sgIt != activeSpawnGroups_.end() && sgIt->second != bot.guid) return;
		}
		// Never regress the bot's own route. On Tier 1 the bot is standing somewhere along a
		// route it is already running, so a nearest-tile match BEHIND its cursor rewinds it.
		// That is the reported failure: the bot stood on patrolWaypoints[0], nearest-tile
		// picked index 0, and the "route home" replayed the entire 122-waypoint quest
		// including its teleport. Recovery means finish forward and walk home, not restart.
		// Tier 2 indexes a different script's array, so the clamp must not apply there.
		const size_t idxFloor = (isTier1 && script.id == bot.huntScriptId)
			? std::min(bot.huntWaypointIdx, script.patrolWaypoints.size())
			: 0;

		// Search patrol waypoints at bot's z-level
		for (size_t i = idxFloor; i < script.patrolWaypoints.size(); i++) {
			auto& wp = script.patrolWaypoints[i].pos;
			if (wp.z != bot.currentPos.z) continue;
			int32_t d = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wp.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wp.y)));
			if (d < bestDist) {
				bestDist = d;
				bestScript = &script;
				bestPatrolIdx = i;
			}
		}
		// Also check travel_from and travel_to waypoints at bot's z
		for (const auto* wps : { &script.travelFromWaypoints, &script.travelToWaypoints }) {
			for (size_t i = 0; i < wps->size(); i++) {
				auto& wp = (*wps)[i].pos;
				if (wp.z != bot.currentPos.z) continue;
				int32_t d = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(wp.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(wp.y)));
				if (d < bestDist) {
					bestDist = d;
					bestScript = &script;
					bestPatrolIdx = SIZE_MAX; // flag: match is not from patrol
				}
			}
		}
	};

	// Tier 1: Check active hunt script first
	if (bot.huntScriptId > 0) {
		for (const auto& script : huntScripts_) {
			if (script.id == bot.huntScriptId) {
				searchScript(script, /*isTier1=*/true);
				break;
			}
		}
	}

	// Tier 2: If no good match from active hunt, search ALL scripts.
	// Quests are excluded here: a bot looking for a way home should not be handed someone
	// else's linear walkthrough, whose teleports and levers it would fire from the middle.
	// Its OWN quest is still available via Tier 1, forward-clamped above.
	if (!bestScript || bestDist > PATH_MAX_DIST) {
		bestDist = INT32_MAX;
		bestScript = nullptr;
		for (const auto& script : huntScripts_) {
			if (script.isQuest || script.scriptCategory == "quest") continue;
			searchScript(script, /*isTier1=*/false);
		}
	}

	if (!bestScript || bestDist > PATH_MAX_DIST) {
		return false; // No nearby route — caller falls back to teleport
	}

	// Build recovery waypoint sequence: patrol[bestPatrolIdx..first discontinuity] +
	// travel_from[0..end].
	//
	// The truncation matters: handleActionWaypoint has no already-visited memory, so a
	// teleport, lever or levitate inside the copied span fires AGAIN on the way "home".
	// A gap between the patrol tail and travel_from[0] already exists structurally in this
	// function, so stopping early is no worse in shape — but it IS worse when the
	// discontinuity was the only way out of a cave, so this relies on doHuntLeaving now
	// handling result.aborted (temple + resupply + a nav-event row) instead of misreading it
	// as an arrival.
	bot.recoveryWaypoints.clear();
	if (bestPatrolIdx != SIZE_MAX) {
		for (size_t i = bestPatrolIdx; i < bestScript->patrolWaypoints.size(); i++) {
			const auto& wpt = bestScript->patrolWaypoints[i];
			if (i > bestPatrolIdx
				&& (botIsRouteDiscontinuity(wpt)
					|| wpt.type == WaypointType::LEVITATE_UP
					|| wpt.type == WaypointType::LEVITATE_DOWN)) {
				break;
			}
			bot.recoveryWaypoints.push_back(wpt);
		}
	}
	for (const auto& wp : bestScript->travelFromWaypoints) {
		bot.recoveryWaypoints.push_back(wp);
	}

	// Set bot state to follow recovery waypoints via doHuntLeaving()
	const bool bestIsQuest = bestScript->isQuest || bestScript->scriptCategory == "quest";
	bot.huntScriptId = bestScript->id;
	logHuntAssign(bot, bestScript->id);
	bot.huntStartTime = OTSYS_TIME(); // Reset safety timeout so doHunting() doesn't re-abort immediately
	// Reserve the script for recovery — prevents another bot from hunting it while we navigate
	// through. Quests are shared under a cooldown rather than reserved, so taking a
	// reservation here would block every other bot's recovery search on that quest for the
	// whole walk home.
	if (!bestIsQuest) {
		activeHunts_[bestScript->id] = bot.guid;
		if (!bestScript->spawnGroup.empty()) {
			activeSpawnGroups_[bestScript->spawnGroup] = bot.guid;
		}
	}
	bot.huntWaypointIdx = 0;
	bot.huntWaypointSkipCount = 0;
	bot.isRecoveryRoute = true;
	bot.state = BotAIState::HUNTING;
	beginHuntPhase(bot, HuntPhase::LEAVING);

	// Set bot.townId for navigation — do NOT call player->setTown() (preserve DB town for death respawn)
	bot.townId = bestScript->townId;
	auto town = g_game().map.towns.getTown(bestScript->townId);
	if (town) {
		auto nameIt = travelTownNames_.find(bestScript->townId);
		bot.townName = nameIt != travelTownNames_.end() ? nameIt->second : town->getName();
	}

	castLog(bot, fmt::format("RECOVERY: Using script {} ('{}') from patrol wp {}, dist={}, {} recovery wps",
		bestScript->id, bestScript->name,
		bestPatrolIdx != SIZE_MAX ? static_cast<int>(bestPatrolIdx) : -1,
		bestDist, bot.recoveryWaypoints.size()));
	return true;
}


// ============================================================================
// Tick frequency gating
// ============================================================================

bool BotEngine::isTickDue(const BotState& bot) const {
	switch (bot.state) {
		case BotAIState::COMBAT:
		case BotAIState::PK_ATTACK:
		case BotAIState::FLEEING:
			return (bot.tickCounter % TICK_FREQ_COMBAT) == 0;

		case BotAIState::HUNTING:
			if (bot.huntTargetId > 0) return (bot.tickCounter % TICK_FREQ_COMBAT) == 0;
			// Preparing/resupply at depot/shop — slower tick rate (no urgency)
			// At 200ms tick: %3 = 600ms (was %5 = 500ms at 100ms tick — close enough)
			if (bot.huntPhase == HuntPhase::PREPARING || bot.huntPhase == HuntPhase::RESUPPLYING)
				return (bot.tickCounter % 3) == 0;
			return (bot.tickCounter % TICK_FREQ_WALKING) == 0;

		case BotAIState::TRAVELING:
			return (bot.tickCounter % TICK_FREQ_WALKING) == 0;

		case BotAIState::PARTY: {
			auto player = bot.getPlayer();
			if (player && player->getAttackedCreature())
				return (bot.tickCounter % TICK_FREQ_COMBAT) == 0;
			return (bot.tickCounter % TICK_FREQ_WALKING) == 0;
		}

		case BotAIState::IDLE:
		case BotAIState::DWELLING: {
			// Holding a monster target in IDLE/DWELLING means the shore self-defense of a fishing
			// run — it fights inside DWELLING because leaving it destroys the run. Give that the
			// same 600ms cadence a hunt fight gets; 1s makes a bot look like it is reacting late.
			if (bot.huntTargetId > 0) {
				return (bot.tickCounter % TICK_FREQ_COMBAT) == 0;
			}
			auto player = bot.getPlayer();
			if (player && (!player->listWalkDir.empty() || bot.followingCityRoute)) {
				return (bot.tickCounter % TICK_FREQ_WALKING) == 0;
			}
			// ~1s for idle bots (fast enough to detect attacks promptly)
			return (bot.tickCounter % TICK_FREQ_IDLE_SCAN) == 0;
		}

		default:
			return (bot.tickCounter % TICK_FREQ_IDLE) == 0;
	}
}

bool BotEngine::isBotPzLocked(const BotState& bot) const {
	return bot.lastPvpAttackTime > 0 && (OTSYS_TIME() - bot.lastPvpAttackTime < PZ_LOCK_DURATION * 1000);
}

// ============================================================================
// Autonomous Activity Reroll
// ============================================================================

// ============================================================================
// BOT_ACTIVITY_PCT helpers
// ============================================================================

// The short dwell a bot takes when the bin it rolled is ineligible or its attempt failed.
//
// This is THE mechanism that makes the percentages true. Previously a failed HUNT attempt fell
// through into PARTY and then into the untested TRAVEL tail, so travel silently collected every
// other bin's failures: config said 25, the ladder implemented 15, and the bots travelled 46.5%.
// Now a failure costs a few seconds of standing still and a fresh roll, which shows up honestly
// as dwell time in the telemetry instead of as a hidden bias toward one activity.
//
// Deliberately uses its OWN short range rather than botDwellRerollMin/MaxSec: HUNT and PARTY are
// both finite reservations (221 hunt scripts held 2-3h; a party cap of botPartyMaxPct of the
// population) so their attempts fail routinely, and borrowing the long CHOSEN-dwell range would
// convert that routine failure into minutes of idling per bot.
void BotEngine::rerollFallbackDwell(BotState& bot, const char* reason) {
	auto& cm = g_configManager();
	int32_t lo = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_FALLBACK_DWELL_MIN_SEC));
	int32_t hi = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_FALLBACK_DWELL_MAX_SEC));
	if (lo < 1) lo = 1;
	if (hi < lo) hi = lo;
	const int32_t secs = uniform_random(lo, hi);
	bot.dwellUntil = OTSYS_TIME() + static_cast<int64_t>(secs) * 1000;
	bot.state = BotAIState::DWELLING;
	castLog(bot, fmt::format("REROLL: {} -- fallback dwell {}s", reason, secs));
}

// TABLE A validation. Logs and carries on; it neither rescales nor refuses to boot.
//
// Rescaling was rejected deliberately: it would run a distribution the operator never wrote,
// which is the exact class of "the number does not mean what it says" defect this whole feature
// exists to remove. Refusing to boot was rejected too -- a server that will not start because a
// percentage is 99 is worse than one that starts and complains. So: keep the numbers as written,
// make the roll honest about its own total, and make the breakage impossible to miss.
void BotEngine::validateActivityTable(int32_t d, int32_t p, int32_t h, int32_t pa, int32_t t) {
	const int32_t sum = d + p + h + pa + t;
	if (sum == s_actTableSum && (sum == 100) == s_actTableValid) return;  // unchanged since last check
	s_actTableSum = sum;
	s_actTableValid = (sum == 100);
	if (!s_actTableValid) {
		g_logger().error("[BotEngine] BOT_ACTIVITY_PCT TABLE A INVALID: botActivityDwell={} + botActivityPoi={} "
			"+ botActivityHunt={} + botActivityParty={} + botActivityTravel={} = {} (must be 100, off by {:+d}). "
			"Values are used AS WRITTEN -- shares are out of {}, not 100. Fix config.lua and run "
			"'/cavebot _global reloadconfig'.",
			d, p, h, pa, t, sum, sum - 100, sum);
	} else {
		g_logger().info("[BotEngine] BOT_ACTIVITY_PCT TABLE A ok: {}/{}/{}/{}/{} = 100 "
			"(dwell/poi/hunt/party/travel)", d, p, h, pa, t);
	}
}

// TABLE B validation. Same contract as TABLE A. Note the sum is checked against the CONFIGURED
// nine rows, not against any one town's candidate pool: selectNextPOI rolls over whatever the
// town actually has, and coverage is uneven by design (19 depots, 14 stones, 9 boats, 1 shop),
// so a per-town sum of 100 is not achievable and is not the invariant being asserted here.
void BotEngine::validatePoiTable() {
	auto& c = livenessCfg_;
	const int32_t sum = c.poiWeightDepot + c.poiWeightDepotOutside + c.poiWeightTemple
		+ c.poiWeightBoat + c.poiWeightShop + c.poiWeightNpc + c.poiWeightWater
		+ c.poiWeightHouse + c.poiWeightAdvStone
		+ c.poiWeightRewardShrine + c.poiWeightImbuingShrine;
	if (sum == s_poiTableSum && (sum == 100) == s_poiTableValid) return;
	s_poiTableSum = sum;
	s_poiTableValid = (sum == 100);
	if (!s_poiTableValid) {
		g_logger().error("[BotEngine] BOT_ACTIVITY_PCT TABLE B INVALID: depot={} depotOutside={} "
			"temple={} boat={} shop={} npc={} water={} house={} advStone={} rewardShrine={} "
			"imbuingShrine={} = {} (must be 100, off by {:+d}). Fix config.lua and run "
			"'/cavebot _global reloadconfig'.",
			c.poiWeightDepot, c.poiWeightDepotOutside, c.poiWeightTemple, c.poiWeightBoat,
			c.poiWeightShop, c.poiWeightNpc, c.poiWeightWater, c.poiWeightHouse,
			c.poiWeightAdvStone, c.poiWeightRewardShrine, c.poiWeightImbuingShrine,
			sum, sum - 100);
	} else {
		g_logger().info("[BotEngine] BOT_ACTIVITY_PCT TABLE B ok: sum = 100");
	}
}

// `/cavebot activity` -- the two tables as CONFIGURED and as REALISED.
//
// This exists because a percentage is what a bot ATTEMPTS, and HUNT and PARTY are finite pools
// (221 hunt scripts held 2-3h each; a party cap of botPartyMaxPct of the population) whose
// attempts routinely fail. Nominal and realised therefore differ legitimately. Showing both,
// with the failures named, is what keeps the numbers honest -- the old design hid the same gap
// by quietly routing every failure into TRAVEL.
//
// Composed POI shares are computed HERE and never written into config.lua comments: they change
// whenever either table is edited, and stale derived arithmetic in comments is precisely the
// failure mode this feature replaced.
std::string BotEngine::activityReport() const {
	auto& cm = g_configManager();
	const int32_t a[5] = {
		static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_DWELL)),
		static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_POI)),
		static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_HUNT)),
		static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_PARTY)),
		static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_TRAVEL)),
	};
	static const char* an[5] = { "dwell", "poi", "hunt", "party", "travel" };
	const int32_t aSum = a[0] + a[1] + a[2] + a[3] + a[4];

	const auto& c = livenessCfg_;
	const int32_t b[11] = { c.poiWeightDepot, c.poiWeightDepotOutside, c.poiWeightTemple,
		c.poiWeightBoat, c.poiWeightShop, c.poiWeightNpc, c.poiWeightWater,
		c.poiWeightHouse, c.poiWeightAdvStone, c.poiWeightRewardShrine,
		c.poiWeightImbuingShrine };
	static const char* bn[11] = { "depot", "depotOutside", "temple", "boat", "shop", "npc",
		"water", "house", "advStone", "rewardShrine", "imbuingShrine" };
	int32_t bSum = 0;
	for (int32_t v : b) bSum += v;

	// Awake vs hibernated is not cosmetic: tryStartCityWalk needs a live Player, and the
	// npc/water/house/shrine POI candidates are awake-only, so the two populations genuinely run
	// different effective tables.
	size_t awake = 0, hib = 0;
	for (const auto& bot : bots_) {
		if (!bot.active) continue;
		if (bot.hibernated) hib++; else awake++;
	}

	std::string out = "[ACTIVITY] BOT_ACTIVITY_PCT\n";
	out += fmt::format("  population: {} awake, {} hibernated\n", awake, hib);
	out += fmt::format("  TABLE A (activity) sum={}{}\n", aSum,
		aSum == 100 ? "" : "   *** INVALID -- must be 100; values are used AS WRITTEN ***");
	for (int i = 0; i < 5; i++) {
		out += fmt::format("    {:<14} {:>3}\n", an[i], a[i]);
	}
	out += fmt::format("  TABLE B (POI destination) sum={}{}\n", bSum,
		bSum == 100 ? "" : "   *** INVALID -- must be 100; values are used AS WRITTEN ***");
	// std::size(b), not a literal: the literal 9 survived the two shrine rows being added to the
	// array and silently printed a nine-row table under an eleven-row sum, which reads as a table
	// that does not add up. Deriving the bound from the array means the next row added cannot
	// reintroduce that.
	for (size_t i = 0; i < std::size(b); i++) {
		const double composed = (aSum > 0 && bSum > 0)
			? (static_cast<double>(a[1]) / aSum) * (static_cast<double>(b[i]) / bSum) * 100.0
			: 0.0;
		out += fmt::format("    {:<14} {:>3}   (= {:.1f}% of all activity)\n", bn[i], b[i], composed);
	}
	out += "  NOTE: TABLE B is rolled over whatever the bot's CURRENT TOWN has. All 19 towns have\n"
	       "  a depot and temple, 14 an adventurer's stone, 9 a boat, 1 a shop -- so a row's real\n"
	       "  per-town share differs from its number, and hibernated bots cannot reach npc/water/\n"
	       "  house/rewardShrine/imbuingShrine at all.\n";

	const int32_t h[5] = { c.houseIdlePct, c.houseHirelingPct, c.houseDummyPct, c.houseLockerPct,
		c.houseShrinePct };
	static const char* hn[5] = { "idle", "hireling", "dummy", "locker", "shrine" };
	int32_t hSum = 0;
	for (int32_t v : h) hSum += v;
	out += fmt::format("  TABLE C (inside a house) sum={}{}\n", hSum,
		hSum == 100 ? "" : "   *** INVALID -- must be 100; values are used AS WRITTEN ***");
	for (size_t i = 0; i < std::size(h); i++) {
		out += fmt::format("    {:<14} {:>3}\n", hn[i], h[i]);
	}
	out += "  NOTE: TABLE C is rolled over what each HOUSE has. Most houses have no training\n"
	       "  dummy and no hireling, so idle's realised share runs well above its number.\n";

	uint64_t tot = 0;
	for (uint64_t v : s_actCum) tot += v;
	if (tot == 0) {
		out += "  REALISED since load: no rerolls yet\n";
		return out;
	}
	out += fmt::format("  REALISED since load ({} rerolls):\n", tot);
	static const char* on[11] = { "dwell", "poi", "poiFail", "hunt", "huntFail", "party",
		"partyFail", "travel", "cityWalk", "travelFail", "roundTail" };
	for (int i = 0; i < 11; i++) {
		if (s_actCum[i] == 0) continue;
		out += fmt::format("    {:<14} {:>7}  {:>5.1f}%\n", on[i], s_actCum[i],
			100.0 * static_cast<double>(s_actCum[i]) / static_cast<double>(tot));
	}
	out += "  NOTE: the *Fail rows are attempts that found no free hunt script, were refused by\n"
	       "  the party cap, or had nowhere to travel. Each became a SHORT DWELL and a fresh roll,\n"
	       "  never another activity -- which is why nominal and realised differ with no hidden\n"
	       "  bias toward any one bin.\n";
	return out;
}

// TABLE C validation. Same contract as A and B.
void BotEngine::validateHouseTable() {
	auto& c = livenessCfg_;
	const int32_t sum = c.houseIdlePct + c.houseHirelingPct + c.houseDummyPct + c.houseLockerPct
		+ c.houseShrinePct;
	if (sum == s_houseTableSum && (sum == 100) == s_houseTableValid) return;
	s_houseTableSum = sum;
	s_houseTableValid = (sum == 100);
	if (!s_houseTableValid) {
		g_logger().error("[BotEngine] BOT_ACTIVITY_PCT TABLE C INVALID: botHouseIdle={} "
			"botHouseHireling={} botHouseDummy={} botHouseLocker={} botHouseShrine={} = {} (must be "
			"100, off by {:+d}). Fix config.lua and run '/cavebot _global reloadconfig'.",
			c.houseIdlePct, c.houseHirelingPct, c.houseDummyPct, c.houseLockerPct, c.houseShrinePct,
			sum, sum - 100);
	} else {
		g_logger().info("[BotEngine] BOT_ACTIVITY_PCT TABLE C ok: sum = 100");
	}
}

std::string BotEngine::activityTableStatusSuffix() const {
	if (s_actTableValid && s_poiTableValid && s_houseTableValid) return "";
	std::string out;
	if (!s_actTableValid) out += fmt::format(" -- TABLE A INVALID (sum={}, must be 100)", s_actTableSum);
	if (!s_poiTableValid) out += fmt::format(" -- TABLE B INVALID (sum={}, must be 100)", s_poiTableSum);
	if (!s_houseTableValid) out += fmt::format(" -- TABLE C INVALID (sum={}, must be 100)", s_houseTableSum);
	return out;
}

void BotEngine::doActivityReroll(BotState& bot) {
	// BOT_PARTY_INVITE_RENDEZVOUS trap #3 — a bot that is assembling for a party must never
	// reroll. This function is not passive: for an IDLE/DWELLING bot holding a huntScriptId it
	// ERASES the activeHunts_/activeSpawnGroups_ reservation and clears the field (the
	// "stale state" branch below), and it is also the only IDLE path into startTravel. Either
	// would sabotage an assembly in progress — a member winding down would have its reservation
	// stripped mid-teardown, or be sent off to another town instead of to its leader.
	if (s_rvMember.count(bot.guid) > 0) return;
	// BOT_AMBIENT_ROAM. A roamer is driven by tickRoamWalk/tickRoamSession and must never be
	// handed an unrelated activity mid-session. Until now it was kept out only ACCIDENTALLY, by
	// the busy guard below happening to see hasWalkTarget/dwellUntil set -- and that accident does
	// not cover a SUSPENDED session, which is neither walking nor dwelling. Guard it explicitly.
	// Deliberately `!suspended`: a permanently-suspended session (roamSessionInvariantsHold fails
	// for e.g. a quest bot) would otherwise freeze this bot's rerolls with nothing to break the
	// tie except eventual hibernation.
	if (isRoamingActive(bot.guid)) return;
	// ...and neither must a BOT_LED leader that is HOLDING at the anchor while its members
	// converge. It sits IDLE with a reserved huntScriptId, which is exactly what the stale-
	// state branch below erases from activeHunts_ before sending it somewhere else.
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0
	    && assemblyActiveForPartyHunt(bot.partyHuntId)) {
		return;
	}

	// Guard: respect reroll cooldown
	if (OTSYS_TIME() < bot.nextRerollTime) return;

	// Guard: don't reroll if busy
	if (bot.hasWalkTarget || bot.followingCityRoute || !bot.pendingNavDest.empty()) return;
	if (bot.huntScriptId > 0 || bot.travelDestTownId > 0) {
		// Safety: if bot is IDLE/DWELLING with stale hunt/travel state, clear it
		if (bot.state == BotAIState::IDLE || bot.state == BotAIState::DWELLING) {
			castLogError(bot, fmt::format("REROLL: Clearing stale state (hunt={}, travel={})",
				bot.huntScriptId, bot.travelDestTownId));
			if (bot.huntScriptId > 0) {
				activeHunts_.erase(bot.huntScriptId);
				for (auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
						activeSpawnGroups_.erase(s.spawnGroup);
						break;
					}
				}
			}
			bot.huntScriptId = 0;
			bot.huntKillCount = 0;
			bot.huntPhase = HuntPhase::PREPARING;
			bot.huntWaypointIdx = 0;
			bot.huntWaypointSkipCount = 0;
			bot.huntIgnoredMonsters.clear();
			bot.travelDestTownId = 0;
			bot.travelPhase.clear();
		} else {
			return; // legitimately busy
		}
	}
	if (bot.stopCooldownUntil > OTSYS_TIME()) return;
	// Debug-pinned bots never self-assign work. Debug mode exists to observe ONE bot doing
	// exactly what the operator asked; an activity reroll mid-observation silently replaces
	// the thing under test (a manual `goto` would be overwritten by a hunt/POI pick).
	// Unlike `stop`, this does not expire.
	if (s_debugPinned.count(bot.guid)) return;

	// Reset stuck-loop counters on reroll
	s_fcConsecutiveFailures.erase(bot.guid);
	clearDepotBlacklist(bot.guid);

	// Bot is picking an activity — activation fallback no longer needed
	bot.activatedAt = 0;

	// Set next reroll cooldown (safety net — should not fire again unless activity fails)
	bot.nextRerollTime = OTSYS_TIME() + g_configManager().getNumber(BOT_ACTIVITY_REROLL_COOLDOWN_SEC) * 1000;
	s_rerollTotal++;

	// BOT_LIVENESS_PACK Phase C.5: new activity == new route, so reset BOTH
	// per-route mid-walk pause counters (unobserved + observed). Hard caps are
	// enforced again from scratch for the upcoming POI/hunt/travel walk.
	bot.pausesThisRoute = 0;
	bot.pausesThisRouteObserved = 0;

	// ==== BOT_ACTIVITY_PCT: TABLE A ====
	// Five real percentages summing to 100. Every bin is TESTED -- there is no residual
	// tail. The old ladder left TRAVEL untested, so it silently absorbed both the unspent
	// remainder AND every failed HUNT/PARTY attempt: config said 25, the ladder implemented
	// 15, and the bots travelled 46.5%. A bin that is ineligible or whose attempt fails now
	// takes the SHORT fallback dwell (see rerollFallbackDwell) and re-rolls fresh, so a
	// failure can never inflate a neighbour.
	auto& cm = g_configManager();
	int32_t wDwell  = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_DWELL));
	int32_t wPoi    = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_POI));
	int32_t wHunt   = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_HUNT));
	int32_t wParty  = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_PARTY));
	int32_t wTravel = static_cast<int32_t>(cm.getNumber(BOT_ACTIVITY_TRAVEL));
	validateActivityTable(wDwell, wPoi, wHunt, wParty, wTravel);

	// First reroll after activation/reload: bias AWAY from standing still, so a freshly
	// woken population does something visible instead of 500 bots idling in place. Takes
	// dwell's share off the table and gives it to POI -- the total stays 100, unlike the
	// old version which shrank dwell without shrinking the ceiling and thereby handed the
	// difference to the untested travel tail.
	if (bot.postActivationReroll) {
		const int32_t shed = std::max(0, wDwell - REROLL_IDLE_WEIGHT_POST_ACTIVATION);
		wDwell -= shed;
		wPoi   += shed;
		bot.postActivationReroll = false;
	}

	// Hunt cooldown is ELIGIBILITY, not a redistribution: zero the bin and let the roll run
	// over the smaller total. The old code moved half of hunt's weight into POI and the
	// other half into a variable that was never read.
	const bool huntOnCD = bot.huntCooldownUntil > OTSYS_TIME();
	if (huntOnCD) {
		wHunt = 0; wParty = 0;   // PARTY is gated on the SAME cooldown (see its bin below)
		s_rerollIneligHunt++; s_rerollIneligParty++;
	}

	// Player-proximity travel nudge: hibernated bots far from any anchor drift toward the
	// towns players are in. Expressed as "add to travel and grow the total" -- arithmetically
	// identical to the old ceiling-widening trick, but now every bin's share is computable
	// because the total is explicit rather than an implied 100+bonus denominator.
	if (livenessCfg_.proxEnabled && bot.hibernated && !currentAnchorPts_.empty()
		&& minChebToAnchor(bot.currentPos) > livenessCfg_.proxNearTiles) {
		wTravel += std::max(0, livenessCfg_.proxTravelCatBonus);
	}

	// PZ-blocked roaming: a genuinely pz-locked bot cannot enter the depot/temple/boat
	// protection zones that HUNT (depot prep) and TRAVEL (boat) walk into, so those bins are
	// ineligible and the roll runs over DWELL+POI only. Uses the REAL player->isPzLocked();
	// note selectNextPOI deliberately uses the ENGINE's 900s isBotPzLocked() window instead --
	// the two predicates are different on purpose and must not be unified.
	bool pzLocked = false;
	{ auto pl = bot.getPlayer(); pzLocked = pl && pl->isPzLocked(); }
	if (pzLocked) {
		if (wHunt > 0) s_rerollIneligHunt++;
		if (wParty > 0) s_rerollIneligParty++;
		s_rerollIneligPz++;
		wHunt = 0; wParty = 0; wTravel = 0;
		castLog(bot, "PZROAM: pz-locked -- reroll restricted to DWELL/POI (no hunt/travel into PZ)");
	}

	const int32_t total = std::max(1, wDwell + wPoi + wHunt + wParty + wTravel);
	const int roll = uniform_random(1, total);
	int cumulative = 0;

	// --- IDLE/DWELL ---
	cumulative += wDwell;
	if (roll <= cumulative) {
		s_rerollDwell++; recordActOutcome(ActOutcome::Dwell);
		// A CHOSEN dwell uses the long configured range. Deliberately NOT the same range as the
		// fallback dwell after a failed attempt -- aliasing them would turn a saturated party cap
		// into minutes of standing still instead of seconds.
		int dwellTime = uniform_random(g_configManager().getNumber(BOT_DWELL_REROLL_MIN_SEC), g_configManager().getNumber(BOT_DWELL_REROLL_MAX_SEC));
		bot.dwellUntil = OTSYS_TIME() + dwellTime * 1000;
		bot.state = BotAIState::DWELLING;
		castLog(bot, fmt::format("REROLL: Dwelling for {}s (roll={}, bin=1-{} of {})",
			dwellTime, roll, wDwell, total));
		return;
	}

	// --- POI WALK ---
	cumulative += wPoi;
	if (roll <= cumulative) {
		auto poi = selectNextPOI(bot);
		if (poi) {
			s_rerollPoi++; recordActOutcome(ActOutcome::Poi);
			const char* routeName = poiTypeToRouteName(poi->type);
			bot.walkTarget = poi->pos;
			bot.hasWalkTarget = true;
			// Seam 2 of 2 into the scoped route planner. Claimed HERE rather than in
			// selectNextPOI, where the NPC visit is decided: this is the only place walkTarget is
			// actually assigned, so the claim and the target can never disagree. One weighted
			// choice per reroll interval, and the whole NPC block is gated on botNpcVisitPct,
			// which ships at 0 — that rarity is what stands in for a rate limit.
			// A HOUSE target is inside a building, behind a door the generic walker cannot open,
			// so it needs the planner for the same reason the other two do.
			if (poi->type == POIType::NPC || poi->type == POIType::WATER
				|| poi->type == POIType::HOUSE
				|| poi->type == POIType::REWARD_SHRINE
				|| poi->type == POIType::IMBUING_SHRINE) {
				s_plannerWalk[bot.guid] = bot.walkTarget;
			}
			// The walk OUT of a house, claimed exactly once. endHouseVisit set this flag; consume
			// it here whatever the roll produced, so it can never survive into an unrelated
			// activity several rerolls later and hand the planner a target that has nothing to do
			// with a house. Leaving needs the same door handling that got the bot in.
			if (s_houseExitPlanner.erase(bot.guid) > 0) {
				s_plannerWalk[bot.guid] = bot.walkTarget;
			}
			bot.currentPOI = poi;
			bot.pathFailCount = 0;
			bot.followingCityRoute = false;
			// Clear FIRST: the assignment below only fires for a non-empty route name, so a POI
			// type with no city route (WATER — a shoreline has none) would otherwise inherit the
			// previous walk's destination and attempt a bogus city-route lookup for it.
			bot.pendingNavDest.clear();
			if (routeName[0] != '\0') {
				bot.pendingNavDest = routeName;
			}
			castLog(bot, fmt::format("REROLL: Walking to {} ({},{},{}) (roll={})",
				poi->name, poi->pos.x, poi->pos.y, poi->pos.z, roll));
			return;
		}
		// No POI available -- fallback dwell, then a fresh roll.
		s_rerollPoiFail++; recordActOutcome(ActOutcome::PoiFail);
		rerollFallbackDwell(bot, "poi_none");
		return;
	}

	// --- HUNT ---
	cumulative += wHunt;
	if (roll <= cumulative) {
		auto player = bot.getPlayer();
		// Hibernated bots have no Player but cache level in BotState — needed for level filtering.
		int32_t level = player ? player->getLevel() : static_cast<int32_t>(bot.cachedLevel);
		if (bot.huntCooldownUntil > OTSYS_TIME()) {
			// Unreachable in practice: wHunt is zeroed above while the cooldown is live, so this
			// bin cannot be rolled. Kept as a belt-and-braces guard.
			castLog(bot, fmt::format("REROLL: Hunt on cooldown ({}s left)",
				(bot.huntCooldownUntil - OTSYS_TIME()) / 1000));
		} else if (tryStartHunt(bot)) {
			s_rerollHunt++; recordActOutcome(ActOutcome::Hunt);
			castLog(bot, fmt::format("REROLL: Starting hunt (roll={})", roll));
			return;
		} else if (bot.hibernated && virtualTryStartHunt(bot)) {
			// Live tryStartHunt requires a Player; for hibernated bots, fall through to the
			// virtual variant so they keep starting fresh hunts after completing prior ones.
			s_rerollHunt++; recordActOutcome(ActOutcome::Hunt);
			return;
		} else {
			// Count eligible vs reserved for diagnostic cast log
			s_rerollHuntFail++; recordActOutcome(ActOutcome::HuntFail);
			int eligible = 0, reserved = 0;
			std::string wantedCat = bot.isQuestBot ? "quest" : "hunt";
			for (const auto& s : huntScripts_) {
				if (!s.enabled || s.patrolWaypoints.empty()) continue;
				if (s.scriptCategory != wantedCat) continue;
				if (s.levelMin > 0 && level < static_cast<int32_t>(s.levelMin)) continue;
				if (s.levelMax > 0 && level > static_cast<int32_t>(s.levelMax)) continue;
				// Empty targetNames is allowed — bot attacks all monsters during PATROLLING
				eligible++;
				if (activeHunts_.count(s.id) || (!s.spawnGroup.empty() && activeSpawnGroups_.count(s.spawnGroup)) || isScriptPlayerClaimed(s.id, s.spawnGroup))
					reserved++; // include player-claimed scripts so "free=N" telemetry isn't overstated (NB6)
			}
			castLog(bot, fmt::format("REROLL: No hunt available (eligible={}, reserved={}, free={})",
				eligible, reserved, eligible - reserved));
		}
		// The hunt-script pool is a FINITE RESERVATION (221 scripts, one bot each, held 2-3h), so
		// this failure is routine rather than exceptional. Take the fallback dwell and return.
		// It used to fall through into PARTY and then TRAVEL, which is exactly where travel's
		// undeclared 31-point over-share came from.
		rerollFallbackDwell(bot, "hunt_unavailable");
		return;
	}

	// --- PARTY HUNT (ROUND2 B: ALL vocations — the leader is elected EK > RP > initiator) ---
	// The EK-only gate is gone: any vocation may now START a party hunt, and formation elects the
	// leader from the assembled roster. Weight is config-driven and funded from the hunt weight
	// (25 -> 13) so the unallocated travel tail keeps its share.
	{
		cumulative += wParty;
		if (roll <= cumulative) {
			bool onCooldown = bot.huntCooldownUntil > OTSYS_TIME();
			if (!onCooldown && tryStartPartyHunt(bot)) {
				s_rerollParty++; recordActOutcome(ActOutcome::Party);
				castLog(bot, fmt::format("REROLL: Starting PARTY HUNT (roll={}, range={}-{})",
					roll, cumulative - wParty + 1, cumulative));
				return;
			}
			// Party hunt failed — fall through to travel. Log unconditionally so we can measure the
			// gap between bots hitting the window and parties forming. Vocation is logged so the
			// post-change miss-rate can be attributed per vocation.
			auto p = bot.getPlayer();
			g_logger().info("[BotEngine] PARTYHUNT-WINDOW: '{}' (voc={}) rolled into party window (roll={} range={}-{}) but did not form (onCooldown={}, hibernated={})",
				p ? p->getName() : bot.name, getBaseVocation(bot.vocationId), roll,
				cumulative - wParty + 1, cumulative,
				onCooldown, !p);
			// Same reasoning as HUNT: the party cap is a finite pool of bot-slots, so refusal is
			// the common case, not an error. Dwell rather than donating the share to TRAVEL.
			s_rerollPartyFail++; recordActOutcome(ActOutcome::PartyFail);
			rerollFallbackDwell(bot, "party_refused");
			return;
		}
	}

	// --- TRAVEL ---
	// Now a TESTED bin like every other. It used to be the untested tail, which is what let it
	// absorb the unspent remainder plus every HUNT and PARTY failure.
	cumulative += wTravel;
	if (roll <= cumulative) {
		// Split between a walking city route and a boat. NOTE: tryStartCityWalk needs a live
		// Player and returns false without one, so this split is structurally inert for
		// hibernated bots and the realised city-walk rate across the whole population is far
		// below the configured percentage. Documented on the key itself.
		if (uniform_random(1, 100) <= static_cast<int32_t>(cm.getNumber(BOT_TRAVEL_CITY_WALK_PCT))
			&& tryStartCityWalk(bot)) {
			s_rerollCityWalk++; recordActOutcome(ActOutcome::CityWalk);
			castLog(bot, fmt::format("REROLL: City walk (roll={}, range={}-100)", roll, cumulative + 1));
			return;
		}

		auto& dests = getTravelDestinations();
		auto destIt = dests.find(bot.townId);
		if (destIt != dests.end() && !destIt->second.empty()) {
			auto& destinations = destIt->second;
			uint32_t destTownId;
			int32_t destDist = -1;
			// Player-proximity travel-destination weighting (2026-06-15): for hibernated bots,
			// weight towns by anchor proximity to their POI footprint. Awake / no-anchor → uniform.
			const bool proxTravel = livenessCfg_.proxEnabled && bot.hibernated && !currentAnchorPts_.empty();
			if (proxTravel) {
				std::vector<int32_t> w(destinations.size());
				std::vector<int32_t> dist(destinations.size());
				for (size_t i = 0; i < destinations.size(); ++i) {
					dist[i] = minChebToTown(destinations[i]);
					w[i] = livenessCfg_.proxBaselineWeight + proximityBonus(dist[i]);
				}
				const size_t pick = weightedPick(w);
				destTownId = destinations[pick];
				destDist = dist[pick];
			} else {
				// Re-pick excluding the bot's own town rather than treating a same-town draw as a
				// failed activity: "travel" that lands where you already are is a wasted draw, not
				// an unavailable activity. Only genuinely having nowhere to go is a failure.
				std::vector<uint32_t> away;
				away.reserve(destinations.size());
				for (uint32_t d : destinations) { if (d != bot.townId) away.push_back(d); }
				destTownId = away.empty()
					? bot.townId
					: away[uniform_random(0, static_cast<int32_t>(away.size()) - 1)];
			}
			if (destTownId != bot.townId) {
				s_rerollTravel++; recordActOutcome(ActOutcome::Travel);
				if (proxTravel) recordProxSelection(destDist, "travel", bot.guid, fmt::format("town {}", destTownId));
				auto nameIt = travelTownNames_.find(destTownId);
				std::string destName = (nameIt != travelTownNames_.end()) ? nameIt->second
					: fmt::format("town {}", destTownId);
				startTravel(bot, destTownId);
				castLog(bot, fmt::format("REROLL: Traveling to {} (roll={}, range={}-100)",
					destName, roll, cumulative + 1));
				return;
			} else {
				s_rerollTravelSame++;
			}
		}
		// Nowhere to travel from this town -- fallback dwell, fresh roll next time.
		s_rerollTravelFail++; recordActOutcome(ActOutcome::TravelFail);
		rerollFallbackDwell(bot, "travel_none");
		return;
	}

	// Rounding guard. Integer percentages can leave `roll` one past the last cumulative bound
	// when a table has been normalised. Never fall out of this function silently: that would
	// leave the bot with no activity AND no dwell, frozen until the cooldown expires.
	s_rerollRoundingTail++; recordActOutcome(ActOutcome::RoundingTail);
	rerollFallbackDwell(bot, "rounding_tail");
}

// ============================================================================
// PZ-blocked roaming (Feature 2)
// ============================================================================
// While a bot is genuinely pz-locked (real 60s server lock after a PvP hit) it cannot enter the
// depot/temple/boat protection zones, so the normal depot-anchored "mill around" behavior is
// unreachable. This keeps it moving in non-PZ space (turn-in-place / short walks to a nearby
// non-PZ tile) until the lock clears. Returns true if it consumed the tick (caller skips the rest
// of doIdle/doDwelling so we don't fight the walk).
bool BotEngine::handlePzRoam(BotState& bot) {
	if (!g_configManager().getBoolean(BOT_PZROAM_ENABLE)) return false;
	auto player = bot.getPlayer();
	if (!player || !player->isPzLocked()) return false;

	// Don't interfere with an active errand/walk/floor-change — but DO own the tick while
	// pz-locked so depot-locker / activity-reroll idle logic doesn't try to route into a PZ.
	if (bot.hasWalkTarget || bot.followingCityRoute || bot.fcState != FloorChangeState::NONE) return false;
	if (!player->listWalkDir.empty()) return true; // let the current mill-around walk finish

	int64_t now = OTSYS_TIME();
	auto& next = s_pzRoamNextTime[bot.guid];
	if (next == 0 || now < next) {
		if (next == 0) {
			next = now + uniform_random(g_configManager().getNumber(BOT_PZROAM_INTERVAL_MIN_SEC),
									   g_configManager().getNumber(BOT_PZROAM_INTERVAL_MAX_SEC)) * 1000LL;
		}
		return true; // waiting between actions — hold the tick
	}
	next = now + uniform_random(g_configManager().getNumber(BOT_PZROAM_INTERVAL_MIN_SEC),
							   g_configManager().getNumber(BOT_PZROAM_INTERVAL_MAX_SEC)) * 1000LL;

	int32_t stayPct = static_cast<int32_t>(g_configManager().getNumber(BOT_PZROAM_STAY_PCT));
	if (uniform_random(1, 100) <= stayPct) {
		static const Direction kDirs[4] = { DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST };
		g_game().internalCreatureTurn(player, kDirs[uniform_random(0, 3)]);
		castLog(bot, "PZROAM: staying (turn in place)");
	} else {
		Position dest = findRandomTileNear(bot.currentPos, 4, /*pzFilter=*/0); // non-PZ only
		if (dest.x > 0 && dest.z == bot.currentPos.z && !(dest == bot.currentPos)) {
			goToWithDoors(bot, dest, 0);
			castLog(bot, fmt::format("PZROAM: roaming to non-PZ ({},{},{})", dest.x, dest.y, dest.z));
		} else {
			castLog(bot, "PZROAM: no non-PZ tile nearby, staying");
		}
	}
	return true;
}


// ============================================================================
// Spectator cache (Gesior b_possible_targets pattern, 2026-05-27)
// ============================================================================
// At 30 active bots × 5Hz × multiple scans/tick, redundant Spectators::find calls
// at the same bot position dominate the per-bot CPU profile. Gesior's analogous
// pattern: each bot caches its 8x6 single-floor scan result for 3 ticks (600ms),
// matching the cadence of updatePossibleTargets in his think.lua.
//
// We mirror this: cache is refreshed at most every SPECTATOR_CACHE_TTL_MS,
// storing only Creature IDs (no shared_ptr → no lifetime extension). Call sites
// resolve via g_game().getCreatureByID and recheck isRemoved/health.
//
// Cache is intentionally NOT invalidated on bot movement — a 600ms TTL allows
// the bot to move 1-2 tiles between refreshes, which is acceptable for target
// SELECTION (the candidate set is mostly stable). AOE damage evaluation paths
// must NOT use this cache: monsters may have moved within the window and the
// damage calculation needs exact positions at cast time.

void BotEngine::refreshSpectatorCacheIfStale(BotState& bot) {
	int64_t now = OTSYS_TIME();
	if (now < bot.cachedSpectatorsExpiry) { botCacheHit(BotCacheId::Spectators); return; }
	const uint64_t specStartUs = botMonoUs(); // Phase 5 instrumentation

	bot.cachedPlayerIds.clear();
	bot.cachedMonsterIds.clear();

	// Single scan at the largest range used by selection sites (MONSTER_SCAN_RADIUS).
	// Sites needing tighter ranges re-check distance against bot.currentPos at use.
	auto specs = Spectators().find<Creature>(bot.currentPos, false,
		MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_X,
		MONSTER_SCAN_RADIUS_Y, MONSTER_SCAN_RADIUS_Y);
	for (const auto& c : specs) {
		if (!c || c->isRemoved() || c->getHealth() <= 0) continue;
		if (c->getPlayer()) {
			bot.cachedPlayerIds.push_back(c->getID());
		} else if (c->getMonster()) {
			bot.cachedMonsterIds.push_back(c->getID());
		}
	}
	bot.cachedSpectatorsExpiry = now + SPECTATOR_CACHE_TTL_MS;
	botCacheMiss(BotCacheId::Spectators, botMonoUs() - specStartUs);
}

// ============================================================================
// Main tick
// ============================================================================

void BotEngine::tick() {
	// JITTER DIAGNOSTIC (the jitter root-cause analysis §3.3):
	// threshold-gated tick body + gap timing. Zero overhead at steady state.
	static int64_t s_lastTickEnd = 0;
	// PERF_INVESTIGATION_2026-05-24 Phase A: rolling EWMA of tickBody for load-aware
	// virtual-sim budget. Single-pole filter α=0.3 over ~10 ticks. tickBody only
	// (not tickGap) because tickGap is contaminated by OS preemption unrelated to
	// our work. Initialized to 0 on .so load → first ticks get full catch-up budget,
	// which is intended (re-load should run all deferred work promptly).
	static double s_loadEwmaMs = 0.0;
	// JITTER FIX 2026-06-10: real clock — with the cached OTSYS_TIME, body/vt/fne
	// always read 0 and the EWMA never moved (Phase-A budget tiers were inert).
	int64_t jitter_tickStart = botMonoMs();
	int64_t jitter_tickGap = (s_lastTickEnd > 0) ? (jitter_tickStart - s_lastTickEnd) : 0;
	// PERF HARNESS: thread CPU at tick entry. Paired with the wall clock at tick exit this is
	// what separates "computing" from "blocked" from "preempted" -- see BotPerfStats.
	const uint64_t perf_cpuStartUs = botThreadCpuUs();
	// Tier 3-5 deadline reference. Everything the bot loop does is deferrable; tiers 1-2 are not.
	s_tickBodyStartMs = jitter_tickStart;
	// Per-phase wall durations for this tick, filled by the BotPhaseTimer scopes below. Kept so
	// the worst-tick register can name the phase that dominated a spike.
	int64_t perf_phaseUs[static_cast<size_t>(BotTickPhase::COUNT)] = {};

	// BOT_LIVENESS_PACK perf hotfix: refresh liveness config cache once per tick
	// (no-op if last refresh was <5s ago). All hot-path config reads in this
	// engine (getPOIWeight, tickLivenessBehaviors, botStartAutoWalk, crowd cap)
	// now consult livenessCfg_ instead of g_configManager().getNumber(BOT_*) on
	// every read. Saves ~22% CPU at 500 bots — the dispatcher was caught in
	// ConfigManager::getInstance under saturation (gdb stack trace).
	{
		BotPhaseTimer _pt(botPerf_.phase[0], perf_phaseUs[0]);
		refreshLivenessCfgIfStale(5000);
	}
	// Party-follow teleport de-collision set is per-tick — reset at tick top so the
	// in-tick reservations made by chooseSafePartyFollowPos never carry across ticks.
	s_partyFollowReservedThisTick.clear();
	// BOT_PARTY_LEAK_FIX: reconciliation sweep (self-throttled to PARTY_LEAK_SWEEP_MS).
	{
		BotPhaseTimer _pt(botPerf_.phase[1], perf_phaseUs[1]);
		sweepStaleCanaryParties();
	}
	s_laneReservedThisTick.clear(); // Phase 7: per-tick lane-tile claims
	s_partyFormationClaims.clear(); // P8 inc2: per-tick formation-slot claims

	// BOT_PARTY_TRAIL_FOLLOW: sample wanted leaders' footsteps at the full engine cadence
	// (200ms, independent of each leader's own isTickDue phase — sampling inside processBot
	// would drop to 400ms and miss steps) + the unconditional 60s [PTRAIL] summary. Free at
	// steady state: empty-map check inside the recorder, 60s clock inside the emitter.
	{
		const int64_t trailNowMs = OTSYS_TIME();
		{
			BotPhaseTimer _pt(botPerf_.phase[2], perf_phaseUs[2]);
			recordLeaderTrails(trailNowMs);
			emitPtrailSummaryIfDue(trailNowMs);
		}

		// BOT_PARTY_INVITE_RENDEZVOUS. Both are free at steady state: the invite poll returns on
		// an O(1) presence gate plus its own cadence check, and the assembly supervisor returns
		// on an empty-map check (no records exist unless a party is mid-assembly). Deliberately
		// driven here at tick top rather than from doParty/doPartyHunt — during assembly no
		// member is in PARTY state yet, so a per-bot dispatch would never run.
		{
			BotPhaseTimer _pt(botPerf_.phase[3], perf_phaseUs[3]);
			tickPartyInvites(trailNowMs);
		}
		{
			BotPhaseTimer _pt(botPerf_.phase[4], perf_phaseUs[4]);
			tickPartyAssembly(trailNowMs);
		}

		// BOT_AMBIENT_ROAM supervisor. Driven here rather than per-bot for the same reason the
		// assembly supervisor is: injection has to pick a bot that is currently HIBERNATED, and
		// hibernated bots are never dispatched. Session VALIDATION deliberately runs even with no
		// anchors and even when the feature is switched off — that is exactly when "the player is
		// gone" has to be noticed and the held bots let go.
		{
			BotPhaseTimer _pt(botPerf_.phase[5], perf_phaseUs[5]);
			tickAmbientRoam(trailNowMs);
		}
	}

	// BOT_CHAT_LIVENESS_V2 Phase F: fire due keyword/PM replies. Empty-vector
	// check inside makes this free at steady state.
	{
		BotPhaseTimer _pt(botPerf_.phase[6], perf_phaseUs[6]);
		processPendingReplies(OTSYS_TIME());
	}

	{
		BotPhaseTimer _pt(botPerf_.phase[7], perf_phaseUs[7]);
		doPopulationManagement();
	}

	// PERF_INVESTIGATION_2026-05-24 Phase B (2026-06-01): keep the density-cap anchor
	// cluster cache warm and emit the periodic [DENSITY] summary at 60s cadence even
	// when no wakes happen. Cheap (≤1µs) at steady state when cache is fresh.
	{
		BotPhaseTimer _pt(botPerf_.phase[8], perf_phaseUs[8]);
		refreshAnchorsIfStale(100);
	}

	// PERF_INVESTIGATION_2026-05-24 Phase A: load-aware virtual-sim budget. Called
	// every tick with a wall-clock budget derived from s_loadEwmaMs. Replaces the
	// prior 5s gate + 4-bucket round-robin model — virtualTick now self-throttles
	// via its budget and defers on demand when the dispatcher is under pressure.
	// Budget tiers chosen so steady-state load (< 30ms tickBody) gets generous catch-up
	// (3ms = ~120 bots/tick), moderate load (30-60ms) gets gentle progress (1ms),
	// and high load (≥ 60ms) defers entirely. See progress log entry 2026-05-31 in
	// the perf investigation notes.
	int64_t jitter_virtualTickMs = 0;
	{
		constexpr int64_t VT_THRESH_GENTLE_MS = 30;
		constexpr int64_t VT_THRESH_SKIP_MS = 60;
		constexpr int64_t VT_BUDGET_LOW_MS = 3;
		constexpr int64_t VT_BUDGET_GENTLE_MS = 1;

		int64_t budget_ms;
		if (s_loadEwmaMs < static_cast<double>(VT_THRESH_GENTLE_MS)) {
			budget_ms = VT_BUDGET_LOW_MS;
		} else if (s_loadEwmaMs < static_cast<double>(VT_THRESH_SKIP_MS)) {
			budget_ms = VT_BUDGET_GENTLE_MS;
		} else {
			budget_ms = 0;
			static int64_t s_lastVtDeferWarn = 0;
			int64_t now_ms = OTSYS_TIME();
			if (now_ms - s_lastVtDeferWarn > 60000) {
				s_lastVtDeferWarn = now_ms;
				g_logger().warn("[VT_DEFER] loadEwma={:.1f}ms (>={}ms threshold) — virtual sim skipped this tick",
					s_loadEwmaMs, VT_THRESH_SKIP_MS);
			}
		}

		if (budget_ms > 0) {
			int64_t vt_start = botMonoMs();
			virtualTick(budget_ms);
			jitter_virtualTickMs = botMonoMs() - vt_start;
		}
	}

	// Liveness diagnostics (2026-06-09): [PROXIMITY] log every 60s.
	// Surfaces per-anchor bot ring inventory + wake outcomes so we can tell whether
	// "few awake bots near depot" is caused by no-bots-virtually-there, density cap
	// gating, or silent wake failures. Sent to system log + all admins (GMs/GODs)
	// via in-game server log channel. Counters reset after emission.
	static int64_t s_lastProximityLog = 0;
	if (OTSYS_TIME() - s_lastProximityLog > 60000) {
		s_lastProximityLog = OTSYS_TIME();
		std::string report = buildProximityReport();
		g_logger().info("{}", report);
		// In-game chat broadcast intentionally disabled: was spamming the admin
		// console every 60s. Pull on demand via "/cavebot population" (merged
		// surface — also prints this proximity report). Journal log above retained.
		wakeTried60s_ = 0;
		wakeGated60s_ = 0;
		wakeGranted60s_ = 0;
		wakeStatsWindowStartMs_ = OTSYS_TIME();
	}

	// Player spawn-claim sweep (~15s): expire 1h claims + auto-release on owner logout.
	// getPlayerByGUID(guid, false) only checks the in-memory online-players map (no DB hit).
	static int64_t s_lastClaimSweep = 0;
	// BOT_AMBIENT_ROAM: the hunt-engagement flag rides the same sweep, so it retires on exactly the
	// same terms as a claim — expiry or the owner going offline. Note the emptiness guard covers
	// BOTH maps: a player whose claim never resolved has a flag but no claim, and gating the sweep
	// on playerClaims_ alone would strand that flag until a real claim happened to exist.
	if ((!playerClaims_.empty() || !playerHuntEngaged_.empty()) && OTSYS_TIME() - s_lastClaimSweep > 15000) {
		s_lastClaimSweep = OTSYS_TIME();
		int64_t nowMs = OTSYS_TIME();
		for (auto it = playerHuntEngaged_.begin(); it != playerHuntEngaged_.end();) {
			const bool expired = nowMs >= it->second;
			const bool ownerOffline = (g_game().getPlayerByGUID(it->first, false) == nullptr);
			if (expired || ownerOffline) {
				g_logger().info("[BotEngine] [ROAM] hunt-flag cleared for guid={} reason={}",
					it->first, expired ? "expired" : "owner-offline");
				it = playerHuntEngaged_.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = playerClaims_.begin(); it != playerClaims_.end();) {
			bool expired = nowMs >= it->second.expiresAt;
			bool ownerOffline = (g_game().getPlayerByGUID(it->second.guid, false) == nullptr);
			if (expired || ownerOffline) {
				g_logger().info("[BotEngine] CLAIM-RELEASE: script {} owner='{}' reason={}",
					it->second.scriptId, it->second.ownerName, expired ? "expired" : "owner-offline");
				it = playerClaims_.erase(it);
			} else {
				++it;
			}
		}
	}

	// Liveness diagnostics (2026-06-09): [POPULATION] log every 5 min.
	// Answers "where are all the bots?" Per-town breakdown by state. Tells us if
	// in-town bot density is high (so anchors near town should see them) or if
	// bots are mostly HUNTING (underground, invisible) or scattered.
	static int64_t s_lastPopulationLog = 0;
	if (OTSYS_TIME() - s_lastPopulationLog > 300000) {
		s_lastPopulationLog = OTSYS_TIME();
		std::string report = buildPopulationReport();
		g_logger().info("{}", report);
		// In-game chat broadcast intentionally disabled: was spamming the admin
		// console every 5 min. Pull on demand via "/cavebot population". Journal
		// log above retained for server-side triage.
	}

	// Periodic state summary (every 5 min) — always logged regardless of verboseLog
	static int64_t s_lastStateSummary = 0;
	if (OTSYS_TIME() - s_lastStateSummary > 300000) {
		s_lastStateSummary = OTSYS_TIME();
		int idle = 0, dwell = 0, hunt = 0, travel = 0, combat = 0, party = 0, other = 0;
		int wrongZ = 0;
		auto& walkZ = getCityWalkZ();
		for (auto& b : bots_) {
			if (!b.active) continue;
			switch (b.state) {
				case BotAIState::IDLE: idle++; break;
				case BotAIState::DWELLING: dwell++; break;
				case BotAIState::HUNTING: hunt++; break;
				case BotAIState::TRAVELING: travel++; break;
				case BotAIState::COMBAT: case BotAIState::PK_ATTACK: case BotAIState::FLEEING: combat++; break;
				case BotAIState::PARTY: party++; break;
				default: other++; break;
			}
			auto wzIt = walkZ.find(b.townId);
			if (wzIt != walkZ.end() && b.currentPos.z != wzIt->second) wrongZ++;
		}
		g_logger().info("[BotEngine] States: idle={} dwell={} hunt={} travel={} combat={} party={} | wrongZ={} | active={}",
			idle, dwell, hunt, travel, combat, party, wrongZ, countActiveBots());

		// BOT_NAV_REALISM Phase 1: tick-phase distribution (acceptance signal). Steady-state home
		// for the histogram — guaranteed to have active bots here, unlike loadHuntData.
		g_logger().info("{}", buildTickPhaseHistogram());

		// BOT_NAV_REALISM Phase 10: human-jitter rates (aggregate — per-event logging would
		// spam at 500 bots). Counters reset each window so this reads as events/5min.
		if (s_jitterDwellCount > 0 || s_jitterUturnCount > 0 || s_jitterRerollCount > 0) {
			g_logger().info("[JITTER] dwell={} uturn={} midRouteReroll={} (per 5min)",
				s_jitterDwellCount, s_jitterUturnCount, s_jitterRerollCount);
			s_jitterDwellCount = 0;
			s_jitterUturnCount = 0;
			s_jitterRerollCount = 0;
		}
		// BOT_NAV_REALISM Phase 8 increment 3: which teleport fallbacks actually fire, and how
		// often. This is the KPI increment 4 (routing recovery sites through the graph) moves.
		if (!teleportSites_.empty()) {
			std::vector<std::pair<std::string, uint32_t>> sites(teleportSites_.begin(), teleportSites_.end());
			std::sort(sites.begin(), sites.end(),
				[](const auto& a, const auto& b) { return a.second > b.second; });
			std::string line;
			uint32_t total = 0;
			for (const auto& [site, n] : sites) {
				total += n;
				if (line.size() < 400) {
					line += fmt::format(" {}={}", site, n);
				}
			}
			g_logger().info("[NAV_TELEPORT_FALLBACK] total={} sites={} |{} (per 5min)",
				total, sites.size(), line);
			teleportSites_.clear();
		}
		// BOT_NAV_REALISM Phase 8: multi-hop city routes served (each one is a teleport avoided).
		if (s_arriveDepotCount + s_arriveTempleCount + s_arriveShopCount + s_arriveOtherCount > 0) {
			g_logger().info("[ARRIVE] depot={} temple={} shop={} other={} fellBackToDepot={}",
				s_arriveDepotCount, s_arriveTempleCount, s_arriveShopCount, s_arriveOtherCount,
				s_arriveFallbackCount);
			s_arriveDepotCount = s_arriveTempleCount = s_arriveShopCount = 0;
			s_arriveOtherCount = s_arriveFallbackCount = 0;
		}
		if (s_routeSpliceRoutes > 0) {
			g_logger().info("[SPLICE] {} route(s) spliced, {} waypoint(s) removed",
				s_routeSpliceRoutes, s_routeSpliceWpsSaved);
		}
		if (s_graphMultiHopCount > 0) {
			g_logger().info("[GRAPH] multiHopRoutes={} avgLegs={} (per 5min)",
				s_graphMultiHopCount, s_graphHopSum / s_graphMultiHopCount);
			s_graphMultiHopCount = 0;
			s_graphHopSum = 0;
		}
		// TRUE MULTI-FLOOR: z-route planner engagement + hop outcomes. planFail
		// counts fallbacks to the legacy greedy scan (graph had no route);
		// hopFail counts blacklist events (bad edge quarantined + replanned).
		if (s_zPlanOk > 0 || s_zPlanFail > 0 || s_zHopOk > 0 || s_zHopFail > 0) {
			g_logger().info("[ZROUTE] planOk={} planFail={} hopOk={} hopFail={} legWalks={} portals={} blacklisted={} (per 5min)",
				s_zPlanOk, s_zPlanFail, s_zHopOk, s_zHopFail, s_zLegWalks, zGraph_.size(), s_zPortalBlacklist.size());
			s_zPlanOk = 0;
			s_zPlanFail = 0;
			s_zHopOk = 0;
			s_zHopFail = 0;
			s_zLegWalks = 0;
		}
		// BOT_HUNT_ENTRY_AND_TELEPORT_SAFETY Phase 3/4 engagement. Aggregated here because the
		// per-event detail goes through castLog, which is cast-viewer gated and so invisible
		// for the population at large. lastResort should stay 0 — anything else means the
		// spiral/temple/login tail ran out of options and a bot was placed on a bad tile.
		if (s_tpSafeRepairs > 0 || s_tpSafeRewinds > 0 || s_tpSafeRefused > 0
			|| s_tpSafeLastResort > 0 || s_tpSafeWakeRepairs > 0 || s_tpSafeFormationRepairs > 0) {
			g_logger().info("[TPSAFE] repairs={} rewinds={} refused={} wake={} formation={} lastResort={} (per 5min)",
				s_tpSafeRepairs, s_tpSafeRewinds, s_tpSafeRefused,
				s_tpSafeWakeRepairs, s_tpSafeFormationRepairs, s_tpSafeLastResort);
			s_tpSafeRepairs = 0;
			s_tpSafeRewinds = 0;
			s_tpSafeRefused = 0;
			s_tpSafeWakeRepairs = 0;
			s_tpSafeFormationRepairs = 0;
			s_tpSafeLastResort = 0;
		}
		// BOT_NAV_REALISM Phase 4b: route phase desync engagement. `suppressed` counts entries
		// the quest / discontinuity guards refused to scatter — without it a working guard is
		// indistinguishable from desync being switched off.
		if (s_desyncScattered > 0 || s_desyncSuppressed > 0
		    || s_desyncNearMatched > 0 || s_desyncNearNone > 0) {
			g_logger().info("[DESYNC] scattered={} avgPhasePct={} suppressed={} nearMatched={} nearNone={} (per 5min)",
				s_desyncScattered,
				s_desyncScattered > 0 ? s_desyncPhaseSumPct / s_desyncScattered : 0,
				s_desyncSuppressed, s_desyncNearMatched, s_desyncNearNone);
			s_desyncScattered = 0;
			s_desyncPhaseSumPct = 0;
			s_desyncSuppressed = 0;
			s_desyncNearMatched = 0;
			s_desyncNearNone = 0;
		}
		// BOT_NAV_REALISM Phase 7: lane-offset engagement (headless verification signal).
		if (s_laneOffsetUsed > 0 || s_laneCenterFallback > 0) {
			const uint32_t laneTotal = s_laneOffsetUsed + s_laneCenterFallback;
			g_logger().info("[LANE] offsetUsed={} centerFallback={} ({}% lane) reserveClash={} (per 5min)",
				s_laneOffsetUsed, s_laneCenterFallback,
				laneTotal > 0 ? (s_laneOffsetUsed * 100 / laneTotal) : 0, s_laneReserveClash);
			s_laneOffsetUsed = 0;
			s_laneCenterFallback = 0;
			s_laneReserveClash = 0;
		}

		// Reroll outcome summary
		if (s_rerollTotal > 0) {
			// Outcome counters SUM TO total by construction -- every terminal path in
			// doActivityReroll increments exactly one. If accounted != total, a path was added
			// without a counter; that is a bug, and saying so here is cheaper than rediscovering
			// it from a distribution that quietly stopped adding up.
			const uint32_t accounted = s_rerollDwell + s_rerollPoi + s_rerollPoiFail
				+ s_rerollHunt + s_rerollHuntFail + s_rerollParty + s_rerollPartyFail
				+ s_rerollTravel + s_rerollCityWalk + s_rerollTravelFail + s_rerollRoundingTail;
			g_logger().info("[BotEngine] REROLL:{} total={} accounted={} | dwell={} poi={} poiFail={} "
				"hunt={} huntFail={} party={} partyFail={} travel={} cityWalk={} travelFail={} "
				"roundTail={} | ineligible: hunt={} party={} pz={}",
				activityTableStatusSuffix(),
				s_rerollTotal, accounted,
				s_rerollDwell, s_rerollPoi, s_rerollPoiFail,
				s_rerollHunt, s_rerollHuntFail, s_rerollParty, s_rerollPartyFail,
				s_rerollTravel, s_rerollCityWalk, s_rerollTravelFail, s_rerollRoundingTail,
				s_rerollIneligHunt, s_rerollIneligParty, s_rerollIneligPz);
			if (accounted != s_rerollTotal) {
				g_logger().warn("[BotEngine] REROLL: {} of {} rerolls unaccounted -- a terminal "
					"path is missing its counter", s_rerollTotal - accounted, s_rerollTotal);
			}
			s_rerollTotal = s_rerollDwell = s_rerollPoi = s_rerollPoiFail = 0;
			s_rerollHunt = s_rerollHuntFail = s_rerollTravel = s_rerollTravelSame = s_rerollCityWalk = 0;
			s_rerollParty = s_rerollPartyFail = s_rerollTravelFail = s_rerollRoundingTail = 0;
			s_rerollIneligHunt = s_rerollIneligParty = s_rerollIneligPz = 0;
		}

		// Detailed IDLE diagnostic — log first 10 IDLE bots with blocking reason
		int idleDiag = 0;
		int idleHuntCD = 0, idleHasWalk = 0, idleFollowRoute = 0, idleHuntSet = 0, idleTravelSet = 0, idleStopCD = 0, idleRerollCD = 0;
		int idleWalking = 0, idleDepot = 0, idleDepotReroll = 0, idlePending = 0, idleFc = 0;
		for (auto& b : bots_) {
			if (!b.active || b.state != BotAIState::IDLE) continue;
			bool huntCD = b.huntCooldownUntil > OTSYS_TIME();
			bool rerollCD = b.nextRerollTime > OTSYS_TIME();
			if (huntCD) idleHuntCD++;
			if (b.hasWalkTarget) idleHasWalk++;
			if (b.followingCityRoute) idleFollowRoute++;
			if (b.huntScriptId > 0) idleHuntSet++;
			if (b.travelDestTownId > 0) idleTravelSet++;
			if (b.stopCooldownUntil > OTSYS_TIME()) idleStopCD++;
			if (rerollCD) idleRerollCD++;
			auto p = b.getPlayer();
			if (p && !p->listWalkDir.empty()) idleWalking++;
			if (b.hasDepotTarget) idleDepot++;
			if (s_depotLockerRerollTime.count(b.guid)) idleDepotReroll++;
			if (!b.pendingNavDest.empty()) idlePending++;
			if (b.fcState != FloorChangeState::NONE) idleFc++;
			if (idleDiag < 5) {
				std::string name = p ? p->getName() : "?";
				int walkQ = p ? static_cast<int>(p->listWalkDir.size()) : 0;
				g_logger().info("[BotEngine] IDLE_DIAG: {} town={} z={} walkZ={} huntCD={}s rerollCD={}s hasWalk={} route={} walkQ={} depot={} fc={}",
					name, b.townId, b.currentPos.z,
					walkZ.count(b.townId) ? walkZ.at(b.townId) : 0,
					huntCD ? (b.huntCooldownUntil - OTSYS_TIME()) / 1000 : 0,
					rerollCD ? (b.nextRerollTime - OTSYS_TIME()) / 1000 : 0,
					b.hasWalkTarget ? 1 : 0, b.followingCityRoute ? 1 : 0,
					walkQ, b.hasDepotTarget ? 1 : 0, static_cast<int>(b.fcState));
			}
			idleDiag++;
		}
		if (idle > 0) {
			g_logger().info("[BotEngine] IDLE_SUMMARY: huntCD={} hasWalk={} route={} walking={} depot={} depotReroll={} pending={} fc={} rerollCD={} | unblocked={}",
				idleHuntCD, idleHasWalk, idleFollowRoute, idleWalking, idleDepot, idleDepotReroll, idlePending, idleFc, idleRerollCD,
				idle - idleHuntCD - idleHasWalk - idleFollowRoute - idleWalking - idleDepot - idleDepotReroll - idlePending - idleFc - idleRerollCD);
		}

		// 2026-05-27: temple-accumulation diagnostic. User reported bots clustering
		// in Ab'Dendriel temple (and likely other temples) after hibernation cycles.
		// Iterates all active bots (incl hibernated, who keep b.active=true), counts
		// per-town those within 15 tiles XY + 1 z of their town's temple position.
		// Reports towns with >=3 clustered bots so we can correlate clusters with the
		// "many bots in same area" lag observed when teleporting in.
		std::unordered_map<uint32_t, std::pair<int, int>> templeBots; // townId -> (awakeCount, hibCount)
		for (auto& b : bots_) {
			if (!b.active) continue;
			auto town = g_game().map.towns.getTown(b.townId);
			if (!town) continue;
			auto tpos = town->getTemplePosition();
			int32_t dx = std::abs(static_cast<int32_t>(b.currentPos.x) - static_cast<int32_t>(tpos.x));
			int32_t dy = std::abs(static_cast<int32_t>(b.currentPos.y) - static_cast<int32_t>(tpos.y));
			int32_t dz = std::abs(static_cast<int32_t>(b.currentPos.z) - static_cast<int32_t>(tpos.z));
			if (dx <= 15 && dy <= 15 && dz <= 1) {
				auto& counts = templeBots[b.townId];
				if (b.hibernated) counts.second++;
				else counts.first++;
			}
		}
		for (auto& [townId, counts] : templeBots) {
			int total = counts.first + counts.second;
			if (total >= 3) {
				auto town = g_game().map.towns.getTown(townId);
				auto tpos = town ? town->getTemplePosition() : Position(0,0,0);
				g_logger().info("[TEMPLE_ACCUM] town={} ({}) temple=({},{},{}) within15: awake={} hib={} total={}",
					townId, town ? town->getName() : "?", tpos.x, tpos.y, tpos.z,
					counts.first, counts.second, total);
			}
		}
	}

	// Auto-toggle verboseLog based on cast viewers (every ~5s = 50 ticks)
	static uint32_t s_viewerCheckCounter = 0;
	if (++s_viewerCheckCounter >= 50) {
		s_viewerCheckCounter = 0;
		for (auto& bot : bots_) {
			if (!bot.active) continue;
			if (bot.verboseLogManual) continue; // don't override manual setting

			auto player = bot.getPlayer();
			if (!player) continue;

			// REAL viewers only: a perf-harness probe bot must never switch the debug
			// firehose on for itself -- the journal I/O would dominate the very tick it
			// exists to measure. See Player::getRealCastViewerCount.
			bot.verboseLog = (player->getRealCastViewerCount() > 0);
		}
	}

	// Periodic cleanup of failed door cooldowns (every ~60s)
	static int64_t s_lastDoorCleanup = 0;
	if (OTSYS_TIME() - s_lastDoorCleanup > 60000) {
		s_lastDoorCleanup = OTSYS_TIME();
		std::erase_if(s_failedDoors, [](const auto& pair) {
			return OTSYS_TIME() - pair.second > DOOR_RETRY_COOLDOWN_MS;
		});
	}

	// Wake stagger decay: one decrement per tick. Combined with per-wake increment in
	// wakeBot, this caps the spread of post-wake quiet periods at WAKE_STAGGER_CAP ticks.
	if (recentWakeStagger_ > 0) {
		recentWakeStagger_--;
	}

	// ---- BOT_NAV_REALISM Phase 6: awake-tick budget ----
	// Closed-loop throttle driven by the SAME tickBody EWMA the virtual sim already uses. Under
	// dispatcher load, cap how many UNOBSERVED awake bots run their AI state machine per tick.
	// Adapted from playerbots' DETAILED_MOVE_ACTIVITY gate (pathfinding is disabled outright for
	// bots no player is near) and the living-universe budgeted scheduler (observed flights always
	// processed, everything else credit-limited).
	//
	// Two properties make this safe:
	//  * OBSERVED bots are exempt at EVERY tier — a real player or cast viewer on screen means the
	//    bot behaves at full fidelity, so nothing a human can see is ever degraded.
	//  * A deferred bot is NOT frozen: it keeps walking its committed listWalkDir (the server's
	//    walk scheduler drives that, not this loop), so deferral only postpones its next DECISION.
	// Fairness: Phase 1 guid-phased tickCounter already decorrelates WHICH bots come due on a
	// given tick, so the budget is handed to a rotating subset rather than always the same
	// low-index bots.
	int32_t aiBudgetRemaining = INT32_MAX;
	bool budgetTierActive = false;
	if (g_configManager().getBoolean(BOT_AWAKE_BUDGET_ENABLE)) {
		if (s_loadEwmaMs >= 60.0) {
			aiBudgetRemaining = g_configManager().getNumber(BOT_AWAKE_PATHFIND_PER_TICK_HIGH);
			budgetTierActive = true;
		} else if (s_loadEwmaMs >= 30.0) {
			aiBudgetRemaining = g_configManager().getNumber(BOT_AWAKE_PATHFIND_PER_TICK_MID);
			budgetTierActive = true;
		}
	}
	uint32_t budgetDeferred = 0, budgetServed = 0, budgetObservedExempt = 0;

	uint32_t deactivatedThisTick = 0;
	int jitter_awakeCount = 0;
	int jitter_hibCount = 0;
	const uint64_t perf_botLoopStartUs = botMonoUs();
	for (auto& bot : bots_) {
		if (!bot.active) continue;
		// Hibernated bots have active=true (so the scheduler treats them as in-population
		// and doesn't try to re-activate them) but are skipped by tick + heal entirely
		// because their Player object is destroyed.
		if (bot.hibernated) { jitter_hibCount++; continue; }
		jitter_awakeCount++;
		bot.tickCounter++;

		// Debug stream: heartbeat snapshot + pending-cast verification (gated on per-bot flag).
		// Hash lookup is O(1); when debug is off the early-return is ~30ns.
		if (auto* dcfg = getDebugCfg(bot.guid)) {
			// Always process pending post-cast first so spell-impact frames are timely
			if (dcfg->pending.active) {
				dbgEmitPostCastIfDue(bot);
			}
			int64_t now = OTSYS_TIME();
			if (now - dcfg->lastSnapshot >= dcfg->snapshotMs) {
				dbgEmitHeartbeat(bot, *dcfg);
				dcfg->lastSnapshot = now;
			}
			dcfg->lastBotTickTime = now;
		}

		// Healing runs at a fixed 200ms regardless of state tick rate (tick() is a
		// cycleEvent(100) — game.cpp restartBotTickLoop — and TICK_FREQ_HEAL is 2).
		// Heal stays active even when aiPaused — bots in spawns would die otherwise.
		if ((bot.tickCounter % TICK_FREQ_HEAL) == 0) {
			doHealing(bot);

			// BOT_SUPPLY_REALISM. Deliberately HERE and not in processBot: processBot
			// early-returns on the death-pause window, on every floor change, and on the whole
			// AdvStone trip, so hooking it there would silently skip potions/food during
			// exactly the situations they are supposed to cover ("any time the bot is awake").
			// This loop has already skipped !active and hibernated bots above, so the
			// awake-only gate comes for free. Rune crafting and support spells share the slot
			// and do their own state tests internally — covering every awake state structurally
			// beats enumerating them, which is how an earlier per-state design missed TRAVELING.
			maybeUseSupplies(bot);
			// Must precede both consumers: the rune gate reads this clock, and this is the only
			// slot that runs for every awake bot in EVERY state, which is what a stationary
			// clock needs (see updateIdleStationaryClock's header).
			updateIdleStationaryClock(bot);
			// Rune crafting: idle >10s or fishing. Support spells: everywhere but hunting,
			// walking included. Chained so at most ONE spell is cast per bot per slot — both can
			// legitimately be eligible at once (an idle bot, or one standing at a fishing spot),
			// and a bot conjuring and hasting in the same 200ms reads as a script, not a player.
			if (!maybeCraftRunes(bot)) {
				maybeSupportSpell(bot);
			}
			// Ice fishing is DRIVEN here and only HELD in followWaypoints. Driving it from the
			// follower instead looked natural — that is where the bot must be stopped from
			// advancing — but the follower is not called in every state that can own a session:
			// an IDLE bot started via `/cavebot icefish` never calls it at all, and doHuntPatrol
			// early-returns before it whenever a monster is targeted. The session then froze
			// instead of progressing (observed live: session open, casts=0, hole untouched).
			// This slot runs for every awake bot regardless of state, which is the same reason
			// the supply hooks above live here.
			tickIceFishSession(bot);

			// Periodic mana restoration — restore to max when below 50% (PvE only)
			auto manaPlayer = bot.getPlayer();
			if (manaPlayer) {
				uint32_t mana = manaPlayer->getMana();
				uint32_t maxMana = manaPlayer->getMaxMana();
				if (maxMana > 0 && mana * 100 / maxMana < 50) {
					bool inPvP = bot.attackerId > 0 && g_game().getPlayerByID(bot.attackerId) != nullptr;
					if (!inPvP) {
						manaPlayer->mana = maxMana;
						g_game().addPlayerMana(manaPlayer);
					}
				}
			}
		}

		if (bot.aiPaused) continue; // skip AI state machine; heal above still runs

		// Wake-stagger gate: freshly-woken bots skip AI for N ticks (heal already ran).
		// Spreads the post-wake AI burst across multiple dispatcher windows so 13
		// simultaneous A* + Spectators::find calls don't stall the dispatcher.
		// BOT_LIVENESS (2026-06-13): when the quiet window EXPIRES this tick, run the
		// first AI tick IMMEDIATELY (skip the isTickDue phase wait that otherwise adds
		// up to TICK_FREQ*100ms of extra dead time -- the engine ticks at 100ms, see
		// Game::botStartTickLoop's cycleEvent(100); the old "200ms" here was stale) and reset so
		// the NEXT AI tick lands a full normal interval later (no off-cadence double-
		// fire). The quiet SPREAD above (still 3..32 for mass wakes) remains the
		// anti-cascade mechanism; this only removes the post-quiet jitter.
		if (bot.wakeQuietTicks > 0) {
			if (--bot.wakeQuietTicks > 0) {
				continue;  // still quiet
			}
			// guid-phased (Phase 1): the fall-through still forces the immediate first AI tick;
			// this value only sets the SUBSEQUENT cadence phase so woken bots don't collide.
			// (Without this, every wake — the dominant runtime lifecycle event — re-zeroed the
			// phase and silently undid the activate-site phasing within minutes of churn.)
			bot.tickCounter = botInitialTickPhase(bot.guid);
			// fall through to processBot (skip the isTickDue check this once)
		} else if (!isTickDue(bot)) {
			continue;
		}

		// Phase 6 budget gate (see the block above tick's bot loop). Only bites when the load
		// tier is active AND the bot is unobserved.
		if (budgetTierActive) {
			auto budgetPlayer = bot.getPlayer();
			if (budgetPlayer && botWalkObserved(bot, budgetPlayer)) {
				budgetObservedExempt++; // never throttled — a human can see this bot
			} else if (aiBudgetRemaining <= 0) {
				budgetDeferred++;
				continue; // keeps walking its committed path; only the next decision is delayed
			} else {
				aiBudgetRemaining--;
				budgetServed++;
			}
		}

		bool wasBotActive = bot.active;
		processBot(bot);
		if (wasBotActive && !bot.active) {
			deactivatedThisTick++;
		}
	}
	// PERF HARNESS: wall cost of the whole per-bot loop. Stamped here rather than via an RAII
	// scope because the loop body has many `continue` paths and the for-statement itself is the
	// thing being measured.
	const int64_t perf_botLoopUs = static_cast<int64_t>(botMonoUs() - perf_botLoopStartUs);

	// Phase 6 telemetry — only when the throttle actually engaged.
	if (budgetDeferred > 0) {
		static int64_t s_lastBudgetLog = 0;
		if (OTSYS_TIME() - s_lastBudgetLog > 30000) {
			s_lastBudgetLog = OTSYS_TIME();
			g_logger().info("[BUDGET] tier={} ewma={:.1f}ms served={} deferred={} observedExempt={}",
				s_loadEwmaMs >= 60.0 ? "high" : "mid", s_loadEwmaMs,
				budgetServed, budgetDeferred, budgetObservedExempt);
		}
	}

	if (deactivatedThisTick > 0) {
		g_logger().warn("[BotEngine] {} bots auto-deactivated this tick", deactivatedThisTick);
	}

	// Flush buffered navigation events to database
	int64_t jitter_fneStart = botMonoMs();
	flushNavEvents();
	int64_t jitter_fneMs = botMonoMs() - jitter_fneStart;

	// JITTER DIAGNOSTIC: log if tick body or gap-since-last-tick exceeds threshold.
	// Body breakdown: vt=virtualTick, fne=flushNavEvents, awake/hib = bot population.
	// If body=0 and the sum of these phases is also ~0, the cost is OUTSIDE the bot tick.
	int64_t jitter_tickEnd = botMonoMs();
	int64_t jitter_tickBody = jitter_tickEnd - jitter_tickStart;
	s_lastTickEnd = jitter_tickEnd;

	// ---- PERF HARNESS: fold this tick into the measurement window ----
	// Everything here is arithmetic on values the tick already computed, plus one CPU-clock
	// read, so the instrument costs far less than the noise it measures.
	{
		auto& perf = botPerf_;
		perf.ticks++;
		const int64_t bodyCpuUs = static_cast<int64_t>(botThreadCpuUs() - perf_cpuStartUs);
		perf.bodyWall.addUs(jitter_tickBody * 1000);
		perf.bodyCpu.addUs(bodyCpuUs);
		perf.gapWall.addUs(jitter_tickGap * 1000);
		perf.virtualTickWall.addUs(jitter_virtualTickMs * 1000);
		perf.flushNavWall.addUs(jitter_fneMs * 1000);
		perf.phase[static_cast<size_t>(BotTickPhase::VirtualTick)].addUs(jitter_virtualTickMs * 1000);
		perf.phase[static_cast<size_t>(BotTickPhase::FlushNavEvents)].addUs(jitter_fneMs * 1000);
		perf_phaseUs[static_cast<size_t>(BotTickPhase::VirtualTick)] = jitter_virtualTickMs * 1000;
		perf_phaseUs[static_cast<size_t>(BotTickPhase::FlushNavEvents)] = jitter_fneMs * 1000;
		perf.phase[static_cast<size_t>(BotTickPhase::BotLoop)].addUs(perf_botLoopUs);
		perf_phaseUs[static_cast<size_t>(BotTickPhase::BotLoop)] = perf_botLoopUs;

		perf.budgetServed += budgetServed;
		perf.budgetDeferred += budgetDeferred;
		perf.budgetObservedExempt += budgetObservedExempt;
		if (jitter_tickBody > 150) { perf.tickSlowCrossings++; }
		if (jitter_tickGap > 500) { perf.gapSlowCrossings++; }

		// Worst-tick register: which phase dominated this tick's wall time.
		size_t domIdx = 0;
		for (size_t i = 1; i < static_cast<size_t>(BotTickPhase::COUNT); i++) {
			if (perf_phaseUs[i] > perf_phaseUs[domIdx]) { domIdx = i; }
		}
		perf.noteWorst(jitter_tickBody, bodyCpuUs / 1000, static_cast<uint8_t>(domIdx),
			perf_phaseUs[domIdx] / 1000, static_cast<uint32_t>(jitter_awakeCount));

		// Gauges at ~1Hz. Cheap, and the harness needs the population shape a window ran under
		// to know whether two runs are even comparable.
		if (jitter_tickEnd - perf.lastGaugeMs >= 1000) {
			perf.lastGaugeMs = jitter_tickEnd;
			perf.gaugeAwake = static_cast<uint32_t>(jitter_awakeCount);
			perf.gaugeHibernated = static_cast<uint32_t>(jitter_hibCount);
			perf.gaugeAnchors = static_cast<uint32_t>(currentAnchors_.size());
			perf.gaugeProbes = static_cast<uint32_t>(s_probeBots.size());
			uint32_t realPlayers = 0;
			for (const auto& [pid, pl] : g_game().getPlayers()) {
				if (pl && !pl->isBotPlayer()) { realPlayers++; }
			}
			perf.gaugeRealPlayers = realPlayers;
		}
	}

	// PERF_INVESTIGATION_2026-05-24 Phase A: update tickBody EWMA for next tick's
	// virtual-sim budget computation. α=0.3 single-pole filter. A single 150ms tick
	// raises EWMA by ~45ms above baseline; recovery to baseline takes ~3-4 ticks.
	{
		constexpr double LOAD_EWMA_ALPHA = 0.3;
		s_loadEwmaMs = LOAD_EWMA_ALPHA * static_cast<double>(jitter_tickBody)
			+ (1.0 - LOAD_EWMA_ALPHA) * s_loadEwmaMs;
	}

	// Starvation telemetry. Deferral is what protects tiers 1-2; a climbing STREAK means a bot is
	// being starved rather than paced -- the "bots look frozen" case, visible as a number here.
	if (s_zPlanDeferrals > 0 && OTSYS_TIME() - s_zPlanDeferLogMs > 30000) {
		s_zPlanDeferLogMs = OTSYS_TIME();
		// Prune first, then report: an entry is only cleared when its bot's plan passes the gate,
		// so hibernated bots would otherwise be counted as "currently deferring" forever.
		const int64_t staleBefore = OTSYS_TIME() - Z_DEFER_STALE_MS;
		std::erase_if(s_zPlanDeferStreak, [staleBefore](const auto& kv) {
			return kv.second.lastMs < staleBefore;
		});
		// Worst streak is reported over the LIVE set, so it decays instead of pinning forever on
		// a bot that hibernated hours ago.
		uint32_t liveWorst = 0, liveWorstGuid = 0;
		for (const auto& [g, st] : s_zPlanDeferStreak) {
			if (st.count > liveWorst) {
				liveWorst = st.count;
				liveWorstGuid = g;
			}
		}
		g_logger().info("[ZPLAN_DEFER] {} deferred (budget {}ms), worst streak {} guid={} (peak {}), "
			"{} deferring now",
			s_zPlanDeferrals, Z_PLAN_TICK_BUDGET_MS, liveWorst, liveWorstGuid,
			s_zPlanDeferWorstStreak, s_zPlanDeferStreak.size());
	}

	if (jitter_tickBody > 150) {
		g_logger().warn("[TICK_SLOW] body={}ms gap={}ms vt={}ms fne={}ms awake={} hib={} ewma={:.1f}ms",
			jitter_tickBody, jitter_tickGap, jitter_virtualTickMs, jitter_fneMs,
			jitter_awakeCount, jitter_hibCount, s_loadEwmaMs);
	}
	// Threshold 200ms → 500ms (2026-05-26 PERF_INVESTIGATION findings):
	// Empirical observation at bots=0 + Legolas active combat + bots=2 23-hour
	// soak: the LXC scheduler floor produces 200-400ms blips that DO NOT
	// correlate with user-perceived lag (zero lagmarks correspond to them).
	// 500ms catches "real" stalls — anything user would notice as a hitch —
	// while filtering the constant LXC noise. Prior data at 200ms gave 92/hr
	// at bots=500 cast-watching active hunter, of which only ~5-10/hr were
	// actually user-visible (the ≥1s events). 500ms threshold is the new
	// signal-to-noise floor.
	if (jitter_tickGap > 500) {
		g_logger().warn("[GAP_SLOW] gap={}ms body={}ms vt={}ms fne={}ms awake={} hib={} ewma={:.1f}ms",
			jitter_tickGap, jitter_tickBody, jitter_virtualTickMs, jitter_fneMs,
			jitter_awakeCount, jitter_hibCount, s_loadEwmaMs);
	}
}

// ============================================================================
// Per-bot processing
// ============================================================================

void BotEngine::processBot(BotState& bot) {
	// JITTER DIAGNOSTIC: per-bot timing. Threshold 10ms. RAII covers early returns below.
	struct ProcTimer {
		int64_t start;
		uint32_t guid;
		std::string name;
		~ProcTimer() {
			int64_t dt = botMonoMs() - start;
			if (dt > 10) {
				g_logger().warn("[PROCBOT_SLOW] guid={} name={} duration={}ms",
					guid, name, dt);
			}
		}
	};
	ProcTimer jitter_p{botMonoMs(), bot.guid, bot.name};

	// MANUAL CONTROL MODE — only command-driven actions are processed.
	// No autonomous decisions (no random hunts, PK, vigilante, chat, etc.)
	auto player = bot.getPlayer();
	if (!player) {
		// DIAG (2026-05-22 Leander/Keira loop investigation): rate-limited per-guid log
		// to identify why playerRef.lock() returns null between activation and the next
		// tick. Cross-check against g_game().getPlayerByName(bot.name) — if it finds a
		// valid Player, our weak_ptr is stale (someone replaced the shared_ptr without
		// updating bot.playerRef). If g_game lookup also fails, the Player object got
		// destroyed and BotScheduler keeps trying to revive it.
		static std::unordered_map<uint32_t, int64_t> s_lastDeactLog;
		int64_t now = OTSYS_TIME();
		auto &lastLog = s_lastDeactLog[bot.guid];
		if (now - lastLog > 10000) {  // once per 10s per bot, max
			lastLog = now;
			auto gamePlayer = g_game().getPlayerByName(bot.name);
			g_logger().warn("[DEACT_DIAG] {} (guid={}): player=null path. g_game lookup={} hibernated={} state={} pos=({},{},{}) town={}",
				bot.name, bot.guid,
				gamePlayer ? (gamePlayer->isRemoved() ? "found_removed" : "found_alive") : "not_found",
				bot.hibernated, static_cast<int>(bot.state),
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				bot.townId);
		}
		bot.active = false;
		bot.state = BotAIState::INACTIVE;
		return;
	}
	if (player->isRemoved()) {
		// DIAG: same rate-limit, isRemoved path
		static std::unordered_map<uint32_t, int64_t> s_lastDeactLog2;
		int64_t now = OTSYS_TIME();
		auto &lastLog = s_lastDeactLog2[bot.guid];
		if (now - lastLog > 10000) {
			lastLog = now;
			g_logger().warn("[DEACT_DIAG] {} (guid={}): isRemoved path. pos=({},{},{}) town={} state={} hibernated={}",
				bot.name, bot.guid,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				bot.townId, static_cast<int>(bot.state), bot.hibernated);
		}
		bot.active = false;
		bot.state = BotAIState::INACTIVE;
		return;
	}

	// Sync position
	bot.lastPos = bot.currentPos;
	bot.currentPos = player->getPosition();

	// Detect teleportation — clear stale walk state and navigation
	if (bot.lastPos.x != 0) {
		int32_t posDiff = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.lastPos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.lastPos.y)));
		int32_t tickDx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.lastPos.x));
		int32_t tickDy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.lastPos.y));
		int32_t tickDz = std::abs(static_cast<int32_t>(bot.currentPos.z) - static_cast<int32_t>(bot.lastPos.z));
		// Normal FC: dx<=2, dy<=2, dz=1-2. Teleport: dx>2 OR dy>2 OR dz>2
		bool zJump = (tickDz > 0 && bot.fcState == FloorChangeState::NONE && (tickDx > 2 || tickDy > 2 || tickDz > 2));

		// Check if this is an EXPECTED teleport from a city route action (wagon, shrine, carpet, teleport tile)
		// Don't clear route state for these — the route waypoints account for the position jump
		if (posDiff > 10 || zJump) {
			// Check if this is an EXPECTED teleport from a route action or travel phase
			bool expectedRouteTP = false;

			// City route USE_WITH waypoints (wagons, shrines, carpets)
			if (bot.followingCityRoute && !bot.cityRouteWps.empty()) {
				size_t startCheck = bot.cityRouteIdx > 0 ? bot.cityRouteIdx - 1 : 0;
				size_t endCheck = std::min(bot.cityRouteIdx + 2, bot.cityRouteWps.size());
				for (size_t i = startCheck; i < endCheck; i++) {
					auto& wp = bot.cityRouteWps[i];
					if (wp.type == WaypointType::USE_WITH) {
						// `continue`, NOT `break`. Marking the jump expected keeps the route
						// (7a30eef3e's purpose) but on its own leaves cityRouteIdx pointing at
						// the PRE-teleport waypoint. Measured on the Feyrist->Thais trip: bot
						// used the exit shrine at route 19675 wp10, jumped 567 tiles to the
						// earth hub, and the cursor stayed on wp11 (33539,32209,7) — still in
						// Feyrist. followWaypoints' 200-tile sanity check then aborted the whole
						// route one tick later and stranded the bot at the hub, one step from
						// the wp12 flame it was supposed to walk onto.
						// Breaking here skipped the landing-match below, which is exactly the
						// auto-advance ab6155a38 describes for the hunt-phase mirror. Keep
						// scanning so it can run.
						expectedRouteTP = true;
						continue;
					}
					if (i >= bot.cityRouteIdx && isAtPosition(bot.currentPos, wp.pos, 3)) {
						expectedRouteTP = true;
						bot.cityRouteIdx = i;
						break;
					}
				}
			}
			// Travel phases that involve boat teleports
			if (bot.state == BotAIState::TRAVELING &&
				(bot.travelPhase == "teleported" || bot.travelPhase == "walk_from_boat")) {
				expectedRouteTP = true;
			}
			// Hunt-phase waypoints (PATROL/LEAVING/TRAVEL_TO) — same pattern as city route.
			// Recognizes USE_WITH (ladder/carpet/shrine) and "bot landed at upcoming wp"
			// (e.g. slime tile, server MoveEvent destination matching a known waypoint)
			// as expected teleports; auto-advances huntWaypointIdx when applicable.
			if (!expectedRouteTP && bot.state == BotAIState::HUNTING && bot.huntScriptId > 0) {
				const HuntScript* huntScript = nullptr;
				for (const auto& s : huntScripts_) {
					if (s.id == bot.huntScriptId) { huntScript = &s; break; }
				}
				if (huntScript) {
					const std::vector<Waypoint>* huntWps = nullptr;
					if (bot.huntPhase == HuntPhase::PATROLLING) {
						huntWps = &huntScript->patrolWaypoints;
					} else if (bot.huntPhase == HuntPhase::LEAVING) {
						huntWps = (bot.isRecoveryRoute && !bot.recoveryWaypoints.empty())
							? &bot.recoveryWaypoints : &huntScript->travelFromWaypoints;
					} else if (bot.huntPhase == HuntPhase::TRAVEL_TO) {
						huntWps = &huntScript->travelToWaypoints;
					}
					if (huntWps && !huntWps->empty()) {
						size_t startCheck = bot.huntWaypointIdx > 0 ? bot.huntWaypointIdx - 1 : 0;
						size_t endCheck = std::min(bot.huntWaypointIdx + 2, huntWps->size());
						for (size_t i = startCheck; i < endCheck; i++) {
							auto& wp = (*huntWps)[i];
							if (wp.type == WaypointType::USE_WITH) {
								// Same `break` -> `continue` correction as the city-route scan above,
								// for the same reason: ab6155a38 documents this block as "mark as
								// expected, preserve state, auto-advance huntWaypointIdx if
								// applicable", but breaking here skips that advance whenever the
								// USE_WITH is what caused the jump -- the normal case. Now live for
								// Feyrist hunt scripts 15/16/17/18, whose travel_from uses a USE_WITH
								// on the Feyrist exit shrine immediately before the hub flame.
								expectedRouteTP = true;
								continue;
							}
							if (i >= bot.huntWaypointIdx && isAtPosition(bot.currentPos, wp.pos, 3)) {
								expectedRouteTP = true;
								bot.huntWaypointIdx = i;
								break;
							}
						}
					}
				}
			}

			// BOT_TELEPORT_TILE_SAFETY Phase 2a + shrine bridge. This is the one place per bot
			// per tick where a real position jump is already detected and classified, so both
			// repairs ride it for free:
			//
			//  (1) townId resync. A teleport-tile relocation (mystic flame, forcefield, Lua
			//      MoveEvent, boat, wagon) moves the bot across the world without touching
			//      bot.townId, leaving city routes pointed at the old town — e.g. a bot back
			//      from Feyrist keeps townId=26 whose depot/temple are inside Feyrist.
			//      syncTownIdToPos self-guards: it no-ops on detected==0 (which is how the
			//      Adventurer's-Stone island carve-out works) and on detected==bot.townId.
			//  (2) shrine storage. If the jump landed the bot at an elemental shrine hub, stamp
			//      Storage.ShrineEntrance now so the hub's flames work when it steps on one.
			//
			// Both run on the EXPECTED and UNEXPECTED branches: `expectedRouteTP` covers city
			// route USE_WITH (wagons/carpets/shrines) and boat travel, which are precisely the
			// canonical cross-town moves, so it needs the resync at least as much.
			syncTownIdToPos(bot);
			if (botNearShrineHub(bot.currentPos)) {
				botStampShrineReturn(player, "teleport-jump");
			}

			// BOT_TELEPORT_TILE_SAFETY diagnostic. The expected/unexpected decision below
			// decides whether the bot KEEPS its city route or has it wiped, and it is the
			// prime suspect for "bots walk the Feyrist route correctly, arrive at the earth
			// hub, and lose it". Both outcomes are castLog-only, which never reaches
			// journalctl, so mirror the decision here. Gated on actually having a route, so
			// this fires only on the rare jump-while-routed case, not on every teleport.
			if (bot.followingCityRoute && !bot.cityRouteWps.empty()) {
				g_logger().info("[ROUTETP] {} jump={} tiles ({},{},{})->({},{},{}) expected={} "
					"routeIdx={}/{} dest='{}' -> route will be {}",
					player->getName(), posDiff,
					bot.lastPos.x, bot.lastPos.y, bot.lastPos.z,
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					expectedRouteTP ? 1 : 0, bot.cityRouteIdx, bot.cityRouteWps.size(),
					bot.lastRouteDestination, expectedRouteTP ? "KEPT" : "*** WIPED ***");
			}

			if (expectedRouteTP) {
				// Expected route teleport — just clear walk queue, keep route intact
				if (!player->listWalkDir.empty()) {
					player->listWalkDir.clear();
					player->stopEventWalk();
				}
				castLog(bot, fmt::format("ROUTE_TP: Expected {} tile jump (z: {}→{}) from route action",
					posDiff, bot.lastPos.z, bot.currentPos.z));
			} else {
				if (!player->listWalkDir.empty()) {
					player->listWalkDir.clear();
					player->stopEventWalk();
				}
				if (bot.fcState != FloorChangeState::NONE) {
					resetFloorChange(bot);
				}
				if (bot.followingCityRoute) {
					bot.followingCityRoute = false;
					bot.cityRouteWps.clear();
					bot.cityRouteIdx = 0;
				}
				bot.hasFleeTarget = false;
				bot.fleeDirectional = false;
				castLog(bot, fmt::format("TELEPORT: Detected {} tile jump (z: {}→{}), cleared walk state",
					posDiff, bot.lastPos.z, bot.currentPos.z));

				// Auto-advance past teleport waypoints during PATROLLING/LEAVING/TRAVEL_TO.
				// Catches "bot was at/near current wp, then server-teleported to a destination
				// unrelated to any known wp" — e.g. Nightmare Isles dimensional portal which
				// teleports to one of 3 random mainland coords. The EXPECTED check above
				// handles cases where the destination matches an upcoming wp; this handles
				// cases where it doesn't.
				//
				// Tolerance 3 (matches EXPECTED check): bot.lastPos lags by one engine tick.
				// If the bot walked from wps[idx-1] toward wps[idx] and stepped on a Lua
				// MoveEvent tile mid-walk, lastPos is the prior tick's position (typically
				// wps[idx-1] or in transit), NOT the just-stepped-on wps[idx]. Chebyshev=3
				// covers both "still walking from prev wp" and "just arrived at current wp".
				//
				// deathPauseUntil guard: on the first tick after death respawn, posDiff is
				// huge (death tile → temple) and triggers teleport-detection. The wps are
				// preserved (per comment at line 4795) so auto-advance could falsely fire
				// if bot died near a waypoint. Skip the advance during the offline/chill
				// window — line 6153 zeroes deathPauseUntil once chill expires.
				if (bot.state == BotAIState::HUNTING && bot.huntScriptId > 0 &&
					bot.deathPauseUntil <= OTSYS_TIME()) {
					const HuntScript* script = nullptr;
					for (const auto& s : huntScripts_) {
						if (s.id == bot.huntScriptId) { script = &s; break; }
					}
					if (script) {
						const std::vector<Waypoint>* wps = nullptr;
						const char* phaseName = "HUNT";
						if (bot.huntPhase == HuntPhase::PATROLLING) {
							wps = &script->patrolWaypoints;
							phaseName = "PATROL";
						} else if (bot.huntPhase == HuntPhase::LEAVING) {
							wps = (bot.isRecoveryRoute && !bot.recoveryWaypoints.empty())
								? &bot.recoveryWaypoints : &script->travelFromWaypoints;
							phaseName = "LEAVING";
						} else if (bot.huntPhase == HuntPhase::TRAVEL_TO) {
							wps = &script->travelToWaypoints;
							phaseName = "TRAVEL_TO";
						}
						if (wps && bot.huntWaypointIdx < wps->size()) {
							auto& wp = (*wps)[bot.huntWaypointIdx].pos;
							if (isAtPosition(bot.lastPos, wp, 3) && bot.lastPos.z == wp.z) {
								bot.huntWaypointIdx++;
								bot.huntWaypointSkipCount = 0;
								castLog(bot, fmt::format("{}: Teleported from wp {}/{} — auto-advanced",
									phaseName, bot.huntWaypointIdx, wps->size()));
							}
						}
					}
				}
			}
		}
	}

	// Periodic heartbeat — 60s status summary to Cast Chat
	{
		auto now = OTSYS_TIME();
		auto& lastHB = s_lastHeartbeat[bot.guid];
		if (now - lastHB >= HEARTBEAT_INTERVAL_MS) {
			lastHB = now;
			logHeartbeat(bot);
		}
	}

	// Death pause — two-phase re-login system
	// Phase 1: offline (1s) — guid is in dyingBots_ (strong ref keeps Player alive)
	// Phase 2: temple chill (10-60s) — bot is online, activated, casting, but AI idle
	if (bot.deathPauseUntil > 0) {
		if (OTSYS_TIME() < bot.deathPauseUntil) {
			return; // Still paused, skip all AI
		}

		auto dyingIt = dyingBots_.find(bot.guid);
		if (dyingIt != dyingBots_.end()) {
			// === Phase 1 expired: re-login bot at temple ===
			auto dyingPlayer = dyingIt->second;
			if (dyingPlayer) {
				// loginPosition already set to temple by Player::death() (line 3798)
				dyingPlayer->health = dyingPlayer->healthMax;
				dyingPlayer->mana = dyingPlayer->getMaxMana();
				dyingPlayer->spawn(); // Re-place on map, re-add to game maps
				g_game().addCreatureHealth(dyingPlayer);
				g_game().addPlayerMana(dyingPlayer);
			}
			dyingBots_.erase(dyingIt);

			// Phase 2: chill at temple 10-60s (bot is online, casting, but idle)
			int32_t chillSecs = uniform_random(10, 60);
			bot.deathPauseUntil = OTSYS_TIME() + chillSecs * 1000LL;
			bot.state = BotAIState::IDLE;
			castLog(bot, fmt::format("DEATH: Re-logged at temple, chilling for {}s", chillSecs));
			return;
		}

		// === Phase 2 expired: resume normal activity ===
		bot.deathPauseUntil = 0;

		// Party hunt support bot re-join: if partyHuntId still valid, teleport to EK and resume
		if (bot.partyHuntId > 0 && !bot.isPartyHuntLeader) {
			auto phIt = s_partyHuntMembers.find(bot.partyHuntId);
			if (phIt != s_partyHuntMembers.end()) {
				// Party still active — teleport to EK and resume PARTY state
				auto leaderIt = s_partyHuntLeaderGuid.find(bot.partyHuntId);
				if (leaderIt != s_partyHuntLeaderGuid.end()) {
					auto leaderIdx = guidToIndex_.find(leaderIt->second);
					if (leaderIdx != guidToIndex_.end()) {
						auto& leaderBot = bots_[leaderIdx->second];
						auto player = bot.getPlayer();
						auto leaderPlayer = leaderBot.getPlayer();
						if (player && leaderPlayer) {
							// Same FC-safety as the formation path: never drop a re-joining
							// support onto the leader's exact tile unvetted.
							std::unordered_set<uint64_t> rejoinReserved;
							Position rejoinPos = chooseSafePartyFollowPos(bot,
								leaderPlayer->getPosition(), rejoinReserved);
							s_ptrail.respawnTele++; // [PTRAIL]: ACCEPTED teleport (death-respawn rejoin)
							// TRAIL: rejoin is instant by design (temple can be cross-town) — make
							// sure no pre-death cursor survives into the fresh episode.
							s_followerCursor.erase(bot.guid);
							s_followerZHopSession.erase(bot.guid);
							// BOT_PARTY_INVITE_RENDEZVOUS trap #5: if this member is still enrolled
							// in an assembly, walk back from the temple instead of teleporting onto
							// the leader — that teleport is the exact pop-in the feature removes.
							if (auto rvIt = s_rvMember.find(bot.guid); rvIt != s_rvMember.end()) {
								if (auto aIt = s_partyAssembly.find(rvIt->second); aIt != s_partyAssembly.end()) {
									for (auto& m : aIt->second.members) {
										if (m.guid != bot.guid) continue;
										// Same approach decision as enrolment: from the temple this will
										// normally stage off-screen near the leader and walk in,
										// rather than boat all the way back.
										beginAssemblyApproach(bot, aIt->second, m);
										break;
									}
									bot.state = BotAIState::IDLE; // the supervisor drives it again
									bot.activatedAt = 0;
									castLog(bot, "DEATH: re-entering party assembly from temple (no teleport)");
									return;
								}
							}
							BOT_TELEPORT(player, rejoinPos);
							bot.currentPos = rejoinPos;
							bot.state = BotAIState::PARTY;
							castLog(bot, fmt::format("DEATH: Party hunt still active, re-joining EK at ({},{},{})",
								bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
							return;
						}
					}
				}
				// Failed to find leader — clear party state and go idle
				bot.partyHuntId = 0;
				bot.partyRole = 0;
				bot.partyLeaderGuid = 0;
			} else {
				// Party dissolved while we were dead — clear state
				bot.partyHuntId = 0;
				bot.partyRole = 0;
				bot.partyLeaderGuid = 0;
				s_botToPartyHunt.erase(bot.guid);
			}
		}

		if (bot.preDeathState == BotAIState::PK_ATTACK) {
			bot.state = BotAIState::IDLE; // Don't resume PK after death
		} else {
			bot.state = bot.preDeathState; // Resume hunt/travel/idle
		}
		castLog(bot, fmt::format("DEATH: Temple chill over, resuming state={}",
			botStateName(bot.state)));
		// Hunt/travel state fields are still intact, bot will continue where it left off
	}

	// Detect z-change for attack grace period
	if (bot.currentPos.z != bot.lastPos.z && bot.lastPos.z != 0) {
		s_lastZChangeTime[bot.guid] = OTSYS_TIME();

		// Clear stale walk queue on unexpected z-change during waypoint following.
		// The remaining steps in listWalkDir are for the OLD floor — executing them
		// can cause further accidental z-changes (cascading ramp transitions).
		bool isFollowingWps = bot.followingCityRoute ||
			bot.state == BotAIState::TRAVELING ||
			(bot.state == BotAIState::HUNTING && bot.huntScriptId > 0);
		if (isFollowingWps && bot.fcState == FloorChangeState::NONE) {
			auto zPlayer = bot.getPlayer();
			if (zPlayer && !zPlayer->listWalkDir.empty()) {
				zPlayer->listWalkDir.clear();
				zPlayer->stopEventWalk();
			}
		}
	}

	// Stuck detection: attack adjacent blockers after 3s of no movement during active navigation
	static constexpr int64_t STUCK_ATTACK_INTERVAL_MS = 3000;
	if (bot.currentPos != bot.lastPos) {
		s_lastMoveTime[bot.guid] = OTSYS_TIME();
	} else if (bot.active && (bot.state == BotAIState::HUNTING || bot.state == BotAIState::TRAVELING)) {
		auto moveIt = s_lastMoveTime.find(bot.guid);
		int64_t lastMove = (moveIt != s_lastMoveTime.end()) ? moveIt->second : OTSYS_TIME();
		if (OTSYS_TIME() - lastMove > STUCK_ATTACK_INTERVAL_MS) {
			tryAttackBlockingMonster(bot);
			s_lastMoveTime[bot.guid] = OTSYS_TIME();
		}
	}

	// Auto-detect town from position. Movement-gated for the common case; periodic
	// safety-net every ~10s (100 ticks @ 100ms) catches the chicken-and-egg deadlock
	// where a bot stuck due to a stale townId would never move → never re-detect.
	// Cost at 200 bots: ~600 ops/sec. Do NOT call player->setTown() — preserve DB town for death respawn.
	if (bot.currentPos != bot.lastPos || bot.townId == 0 || (bot.tickCounter % 100) == 0) {
		uint32_t detectedTown = findNearestTown(bot.currentPos);
		if (detectedTown > 0 && detectedTown != bot.townId) {
			auto town = g_game().map.towns.getTown(detectedTown);
			if (town) {
				bot.townId = detectedTown;
				auto nameIt = travelTownNames_.find(detectedTown);
				bot.townName = nameIt != travelTownNames_.end() ? nameIt->second : town->getName();
			}
		}
	}

	// Push blocking items on the next walk tile (same as monsters with canPushItems)
	if (!player->listWalkDir.empty()) {
		Direction nextDir = player->listWalkDir.back();
		Position nextPos = getNextPosition(nextDir, bot.currentPos);
		auto nextTile = g_game().map.getTile(nextPos);
		if (nextTile && nextTile->hasFlag(TILESTATE_BLOCKPATH)) {
			Monster::pushItems(nextTile, nextDir);
		}
	}

	// Handle floor change state machine (needed for z-transitions during navigation)
	if (bot.fcState != FloorChangeState::NONE) {
		handleFloorChange(bot);
		return;
	}

	// Adventurer's Stone trip — fully takes over the AI tick. Placed AFTER handleFloorChange
	// so stairs_up/stairs_down mid-trip still work, but BEFORE doSelfDefense and state
	// dispatch — bot doesn't fight back during a trip; if killed, pauseBotForDeath cleanup
	// aborts the trip cleanly. The trip is short and entirely in safe (non-PvP) territory
	// for most populations, so this trade-off is acceptable.
	if (bot.advStoneActive) {
		doAdventurerStone(bot);
		return;
	}

	// Self-defense: detect player attackers before state dispatch
	// PK_ATTACK bots also need self-defense (vigilante attacks them → fight/flee/ignore)
	// PARTY bots skip self-defense — they follow the leader, not fight PKers independently
	if (bot.state != BotAIState::INACTIVE && bot.state != BotAIState::PARTY) {
		doSelfDefense(bot);
	}

	// BOT_SUPPLY_REALISM: a fishing run only ticks from doDwelling, so any transition OUT of
	// IDLE/DWELLING abandons it. Catching that here — one guarded call covering combat, PK,
	// flee, hunt, travel and party in a single place — is what stops a stretched dwellUntil
	// outliving the run (virtualAdvanceDwelling is a pure timer, and bot_party.cpp's
	// recruitment snapshot copies dwellUntil verbatim).
	// IDLE stays exempt only while a walk is actually claimed — that covers both legitimate
	// legs of a trip (the outbound one runs in IDLE by design, and so does RETURNING). The
	// moment an admin command forces IDLE and clears hasWalkTarget (`stop`, `resume`, ...)
	// the exemption lapses and the run is torn down, instead of being orphaned with nothing
	// ticking it — doDwelling is the only thing that drives a session.
	if (isFishing(bot.guid) && bot.state != BotAIState::DWELLING
	    && !(bot.state == BotAIState::IDLE && bot.hasWalkTarget)) {
		castLog(bot, "FISH: interrupted — leaving the shore");
		clearFishingRun(bot.guid);
	}

	// BOT_SHRINE_IDLE: same guard, and the IDLE+hasWalkTarget exemption is MANDATORY here rather
	// than copied for symmetry. The run is created at selection time in APPROACH and the outbound
	// walk runs in IDLE by design, so without the exemption this fires on the very first tick
	// after the reroll and kills every visit before the bot takes a step. The exemption lapses the
	// moment an admin command forces IDLE and clears hasWalkTarget (`stop`, `resume`), which is
	// exactly when the run should be torn down instead of orphaned.
	if (isShrineVisiting(bot.guid) && bot.state != BotAIState::DWELLING
	    && !(bot.state == BotAIState::IDLE && bot.hasWalkTarget)) {
		castLog(bot, "SHRINE: interrupted — leaving the shrine");
		endShrineVisit(bot.guid, "interrupted");
	}

	// Re-assert the mount intent for a bot that rolled one but could not act on it yet —
	// almost always because it reconnected inside a town PZ, where toggleMount(true) refuses.
	// Deliberately here rather than in tickLivenessBehaviors: that function early-returns on
	// !listWalkDir.empty(), so a bot walking continuously out of town would never be checked,
	// which is exactly the moment we are waiting for. bot.tickCounter is guid-phased by
	// botInitialTickPhase, so the population spreads across the window instead of all firing
	// on the same tick; the cost per bot per tick is a handful of integer compares, and the
	// toggleMount call itself only runs for a bot that is both unmounted and wants a mount.
	static constexpr uint32_t MOUNT_RETRY_INTERVAL_TICKS = 300;  // ~30s at 100ms/tick
	if (bot.attackerId == 0 && bot.huntTargetId == 0
	    && bot.state != BotAIState::COMBAT && bot.state != BotAIState::FLEEING
	    && bot.state != BotAIState::PK_ATTACK
	    && (bot.tickCounter % MOUNT_RETRY_INTERVAL_TICKS) == 0) {
		tryOpportunisticMount(bot, player);
	}

	// Dispatch by state
	switch (bot.state) {
		case BotAIState::IDLE:
			doIdle(bot);
			break;
		case BotAIState::DWELLING:
			doDwelling(bot);
			break;
		case BotAIState::TRAVELING:
			doTraveling(bot);
			break;
		case BotAIState::HUNTING:
			doHunting(bot);
			break;
		case BotAIState::COMBAT:
			doCombat(bot);
			break;
		case BotAIState::FLEEING:
			doFleeing(bot);
			break;
		case BotAIState::PK_ATTACK:
			doPKAttack(bot);
			break;
		case BotAIState::PARTY:
			doParty(bot);
			break;
		default:
			break;
	}
}

// ============================================================================
// IDLE state
// ============================================================================

void BotEngine::doIdle(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Gang-PK staging (Feature 1): if this bot is a committed gang member, drive its walk to the
	// PZ-edge stage tile and the synchronized burst. Consumes the tick while staging.
	if (s_gangByGuid.count(bot.guid)) {
		if (handleGangStaging(bot)) return;
	}

	// BOT_PARTY_INVITE_RENDEZVOUS: a member walking in to join a party. MUST come before the
	// activation fallback below, which would teleport it to its temple, and before every walk
	// this function can otherwise start. Consumes the tick, exactly like gang staging.
	if (!s_rvMember.empty() && s_rvMember.count(bot.guid) > 0) {
		if (handleAssemblyStaging(bot)) return;
	}

	// Idle litter drop (drop-only, once per wake). Self-gates: it resets its stop
	// clock whenever the bot is walking or on an errand, so calling it up front (before
	// doIdle's navigation/return/route early-returns) correctly tracks genuine stops.
	maybeFidgetDrop(bot);

	// 1-min post-activation fallback: if bot is stuck IDLE with no active task after activation,
	// teleport to its town temple. Guards: hasWalkTarget/followingCityRoute mean bot IS making progress.
	if (bot.activatedAt > 0 && OTSYS_TIME() - bot.activatedAt > 60000
	    && !bot.hasWalkTarget && !bot.followingCityRoute) {
		bot.activatedAt = 0;
		teleportToTemple(bot);
		castLog(bot, "ACTIVATION_FALLBACK: 60s idle — teleporting to temple");
		return;
	}

	bool pzLocked = isBotPzLocked(bot);

	// PK re-engage: if recently-fought target reappears within 7 tiles, resume fight
	// Window: 60s OR while bot still has PZ-lock (whichever is longer)
	auto reIt = s_reengageTarget.find(bot.guid);
	if (reIt != s_reengageTarget.end()) {
		bool timerExpired = OTSYS_TIME() > s_reengageUntil[bot.guid];
		bool stillPzLocked = player->isPzLocked();
		if (timerExpired && !stillPzLocked) {
			s_reengageTarget.erase(bot.guid);
			s_reengageUntil.erase(bot.guid);
		} else {
			auto reTarget = g_game().getCreatureByID(reIt->second);
			if (reTarget && !reTarget->isRemoved() && reTarget->getHealth() > 0) {
				auto tpos = reTarget->getPosition();
				int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(tpos.x));
				int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(tpos.y));
				int32_t dist = std::max(dx, dy);
				if (dist <= 7 && bot.currentPos.z == tpos.z) {
					castLog(bot, fmt::format("PK RE-ENGAGE: Target {} reappeared at dist={}",
						reTarget->getName(), dist));
					player->setSecureMode(false);  // Allow attacking unmarked players
					bot.state = BotAIState::PK_ATTACK;
					bot.pkTarget = reIt->second;
					bot.combatStartTime = OTSYS_TIME();
					bot.lastCombatProgress = OTSYS_TIME();
					bot.combatHpCheckTime = OTSYS_TIME();
					bot.combatHpBaseline = 0;
					bot.combatStalemateCount = 0;
					bot.hasWalkTarget = false;
					bot.pvpManaSpent = 0;
					s_reengageTarget.erase(bot.guid);
					s_reengageUntil.erase(bot.guid);
					return;
				}
			}
		}
	}

	// Post-combat return walk: walk back to pre-combat position
	auto returnIt = s_returnPos.find(bot.guid);
	if (returnIt != s_returnPos.end()) {
		auto timeIt = s_returnStartTime.find(bot.guid);
		int64_t returnStart = timeIt != s_returnStartTime.end() ? timeIt->second : 0;

		// 60s timeout — give up
		if (OTSYS_TIME() - returnStart > 60000) {
			castLog(bot, "RETURN: Timeout, giving up");
			s_returnPos.erase(bot.guid); s_returnStartTime.erase(bot.guid);
		}
		// Arrived (within 3 tiles, same z)
		else if (bot.currentPos.z == returnIt->second.z &&
				 isAtPosition(bot.currentPos, returnIt->second, 3)) {
			castLog(bot, "RETURN: Arrived at pre-combat position");
			s_returnPos.erase(bot.guid); s_returnStartTime.erase(bot.guid);
		}
		// Different z — floor change needed
		else if (bot.currentPos.z != returnIt->second.z && bot.fcState == FloorChangeState::NONE) {
			bool goDown = returnIt->second.z > bot.currentPos.z;
			castLog(bot, fmt::format("RETURN: Floor change {} to z={}", goDown ? "DOWN" : "UP", returnIt->second.z));
			startFloorChange(bot, goDown, returnIt->second);
			return;
		}
		// Same z — walk to position
		else if (bot.currentPos.z == returnIt->second.z) {
			if (!player->listWalkDir.empty()) return; // Walking in progress
			if (!goToWithDoors(bot, returnIt->second)) {
				castLog(bot, "RETURN: Can't pathfind, giving up");
				s_returnPos.erase(bot.guid); s_returnStartTime.erase(bot.guid);
			}
			return;
		}
		// FC in progress — wait
		else { return; }
	}

	// Vigilante: scan for PKers (IDLE state)
	checkVigilante(bot);
	if (bot.state != BotAIState::IDLE) return;

	// Random PK roll
	checkRandomPK(bot);
	if (bot.state != BotAIState::IDLE) return;

	// Gang-PK initiation (Feature 1): a bot standing in a PZ may recruit nearby idle bots to
	// jump an exposed victim just outside. Sets up a session; members are driven by the staging
	// handler at the top of doIdle next tick.
	checkGangJump(bot);
	if (bot.state != BotAIState::IDLE || s_gangByGuid.count(bot.guid)) return;

	// PZ-blocked roaming (Feature 2): while genuinely pz-locked, mill around non-PZ space instead
	// of routing into a depot/temple/boat PZ.
	if (handlePzRoam(bot)) return;

	// Follow active city route FIRST — takes priority over depot walk and everything else.
	// followCityRoute() has its own walk-queue management, so we must NOT block with listWalkDir.
	if (bot.followingCityRoute) {
		if (followCityRoute(bot)) return; // route still in progress
		// Route complete
		castLog(bot, "IDLE: City route complete");
		bot.followingCityRoute = false;
		bot.pendingNavDest.clear();

		// Z-mismatch recovery: if route completed but bot is on wrong z for destination,
		// walk back through the last FC to get back to the correct z, then retry routes
		if (bot.hasWalkTarget && bot.currentPos.z != bot.walkTarget.z) {
			// Clear walk target — route failed to reach correct z, don't deadlock
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			auto fcIt = s_lastFcPositions.find(bot.guid);
			if (fcIt != s_lastFcPositions.end()) {
				auto& [preFcPos, postFcPos] = fcIt->second;
				castLog(bot, fmt::format("IDLE: Route z-mismatch (bot z={}, target z={}), "
					"FC recovery to ({},{},{})",
					bot.currentPos.z, bot.walkTarget.z,
					preFcPos.x, preFcPos.y, preFcPos.z));
				trackNavEvent("idle_z_mismatch", bot, 0, "", bot.townId, "",
					fmt::format("bot z={} target z={} fc_recovery", bot.currentPos.z, bot.walkTarget.z));
				startFloorChange(bot, preFcPos.z > bot.currentPos.z, preFcPos);
				bot.triedRouteSources.clear();
				s_lastFcPositions.erase(fcIt);
				return;
			}
			// No FC history — just clear and let reroll pick up
			castLog(bot, fmt::format("IDLE: Route z-mismatch (bot z={}, target z={}), no FC history, clearing target",
				bot.currentPos.z, bot.walkTarget.z));
			trackNavEvent("idle_z_mismatch", bot, 0, "", bot.townId, "",
				fmt::format("bot z={} target z={} no_fc_history", bot.currentPos.z, bot.walkTarget.z));
		}

		// Dynamic POIs (depot_outside, boat_nearby): keep hasWalkTarget so bot walks
		// the final leg from city route endpoint to the actual dynamic tile position
		if (bot.currentPOI && (bot.currentPOI->type == POIType::DEPOT_OUTSIDE ||
			(!bot.currentPOI->name.empty() && bot.currentPOI->name[0] == '_'))) {
			castLog(bot, fmt::format("IDLE: Route complete, walking to dynamic POI {} at ({},{},{})",
				bot.currentPOI->name, bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
			// hasWalkTarget stays true — fallback A* below will walk the last few tiles
			return;
		}

		bot.hasWalkTarget = false;

		// Walk to depot locker if one is nearby (same as travel arrival / hunt resupply)
		auto lockerPos = findReachableDepotLocker(bot);
		if (lockerPos.x != 0) {
			bot.idleDepotTarget = lockerPos;
			bot.hasDepotTarget = true;
			s_depotWalkRetries[bot.guid] = 0;
			castLog(bot, fmt::format("IDLE: Walking to depot locker at ({},{},{})",
				lockerPos.x, lockerPos.y, lockerPos.z));
		}
		return;
	}

	// Depot dwell walk target: handle walking to PZ roam / step outside destination
	{
		auto walkIt = s_depotDwellWalkTarget.find(bot.guid);
		if (walkIt != s_depotDwellWalkTarget.end()) {
			if (!player->listWalkDir.empty()) {
				// Stale-walk guard: if walk queue has not drained in 10s, bot is blocked.
				// Clear queue and fall through to re-pathfind or cancel.
				auto& staleStart = s_staleWalkStart[bot.guid];
				if (staleStart == 0) staleStart = OTSYS_TIME();
				if (OTSYS_TIME() - staleStart < 10000) return; // Still within grace period
				castLog(bot, fmt::format("IDLE: Depot dwell walk stalled ({} steps), clearing",
					player->listWalkDir.size()));
				player->listWalkDir.clear();
				player->stopEventWalk();
				s_staleWalkStart.erase(bot.guid);
				// Fall through to re-check distance and re-pathfind or cancel
			} else {
				s_staleWalkStart.erase(bot.guid); // Walk drained naturally
			}
			Position target = walkIt->second;

			// Cancel dwell walk if target is on a different z — no FC during idle PZ roaming
			if (bot.currentPos.z != target.z) {
				castLog(bot, fmt::format("IDLE: Cancelling dwell walk to ({},{},{}) — different z, no FC in PZ idle",
					target.x, target.y, target.z));
				s_depotDwellWalkTarget.erase(walkIt);
				s_depotDwellWalkFails.erase(bot.guid);
				return;
			}

			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(target.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(target.y)));
			if (dist <= 1) {
				// Arrived at depot dwell destination
				castLog(bot, fmt::format("IDLE: Arrived at dwell target ({},{},{})", target.x, target.y, target.z));
				s_depotDwellWalkTarget.erase(walkIt);
				s_depotDwellWalkFails.erase(bot.guid);
			} else {
				if (!goToWithDoors(bot, target, 1)) {
					s_depotDwellWalkFails[bot.guid]++;
					if (s_depotDwellWalkFails[bot.guid] >= 3) {
						castLog(bot, fmt::format("IDLE: Can't reach dwell target ({},{},{}) after 3 fails, cancelling",
							target.x, target.y, target.z));
						s_depotDwellWalkTarget.erase(walkIt);
						s_depotDwellWalkFails.erase(bot.guid);
					}
				}
			}
			return;
		}
	}

	// Depot locker reroll: after waiting 20-60s at locker, decide where to dwell
	{
		auto rerollIt = s_depotLockerRerollTime.find(bot.guid);
		if (rerollIt != s_depotLockerRerollTime.end()) {
			if (OTSYS_TIME() < rerollIt->second) return; // Still waiting at locker
			s_depotLockerRerollTime.erase(rerollIt);

			// Verify bot is actually near a depot — stale timer from previous town
			bool nearDepot = false;
			for (int dx = -2; dx <= 2 && !nearDepot; dx++) {
				for (int dy = -2; dy <= 2 && !nearDepot; dy++) {
					auto t = g_game().map.getTile(Position(bot.currentPos.x + dx, bot.currentPos.y + dy, bot.currentPos.z));
					if (t && t->hasFlag(TILESTATE_DEPOT)) nearDepot = true;
				}
			}
			if (!nearDepot) {
				castLog(bot, "DEPOT: Stale locker timer, not near depot — skipping reroll");
				return;
			}

			// botDepotLockerPct: stay at the locker vs mill about outside. Duplicated at two
			// arrival sites (travel arrival and POI arrival); both read the same key so they
			// cannot drift apart the way the hardcoded 40s could.
			const int32_t lockerPct = static_cast<int32_t>(g_configManager().getNumber(BOT_DEPOT_LOCKER_PCT));
			int depotRoll = uniform_random(1, 100);
			// Remainder is split evenly between "roam to a PZ tile" and "step outside", so the
			// three branches stay coherent at ANY lockerPct. A hardcoded second bound (it was 70)
			// would silently make this branch unreachable the moment lockerPct was raised past it.
			const int32_t roamBound = lockerPct + (100 - lockerPct) / 2;
			if (depotRoll <= lockerPct) {
				castLog(bot, fmt::format("DEPOT: Staying at locker ({}%)", lockerPct));
			} else if (depotRoll <= roamBound) {
				Position pzTile = findRandomReachablePZTile(bot.currentPos);
				if (pzTile.x > 0 && pzTile.z == bot.currentPos.z) {
					s_depotDwellWalkTarget[bot.guid] = pzTile;
					s_depotDwellWalkFails[bot.guid] = 0;
					castLog(bot, fmt::format("DEPOT: Roaming to PZ tile ({},{},{})",
						pzTile.x, pzTile.y, pzTile.z));
				} else {
					castLog(bot, "DEPOT: No reachable same-z PZ tile, staying at locker");
				}
			} else {
				Position nonPzTile = findClosestNonPZTile(bot.currentPos);
				if (nonPzTile.x > 0 && nonPzTile.z == bot.currentPos.z) {
					s_depotDwellWalkTarget[bot.guid] = nonPzTile;
					s_depotDwellWalkFails[bot.guid] = 0;
					castLog(bot, fmt::format("DEPOT: Stepping outside to ({},{},{})",
						nonPzTile.x, nonPzTile.y, nonPzTile.z));
				} else {
					castLog(bot, "DEPOT: No same-z non-PZ tile found, staying at locker");
				}
			}
			return;
		}
	}

	// Handle depot locker walk (set by travel arrival or POI arrival)
	if (bot.hasDepotTarget) {
		if (!player->listWalkDir.empty()) {
			// Stale-walk guard: if walk queue stuck for >10s, clear and retry.
			auto& staleStart = s_staleWalkStart[bot.guid];
			if (staleStart == 0) staleStart = OTSYS_TIME();
			if (OTSYS_TIME() - staleStart < 10000) return;
			castLog(bot, fmt::format("DEPOT: Locker walk stalled ({} steps), clearing",
				player->listWalkDir.size()));
			player->listWalkDir.clear();
			player->stopEventWalk();
			s_staleWalkStart.erase(bot.guid);
		} else {
			s_staleWalkStart.erase(bot.guid);
		}

		// Track walk retries — after 5 idle ticks, blacklist locker and try a new one
		s_depotWalkRetries[bot.guid]++;
		if (s_depotWalkRetries[bot.guid] > 5) {
			blacklistDepotLocker(bot.guid, bot.idleDepotTarget);
			if (s_depotBlacklist[bot.guid].size() >= 5) {
				castLog(bot, "DEPOT: Tried 5 lockers, giving up until reroll");
				bot.hasDepotTarget = false;
				clearDepotBlacklist(bot.guid);
				return;
			}
			Position newLocker = findReachableDepotLocker(bot);
			if (newLocker.x != 0) {
				castLog(bot, fmt::format("DEPOT: Retrying with locker {}/5 at ({},{},{})",
					s_depotBlacklist[bot.guid].size() + 1,
					newLocker.x, newLocker.y, newLocker.z));
				bot.idleDepotTarget = newLocker;
				s_depotWalkRetries[bot.guid] = 0;
			} else {
				castLog(bot, "DEPOT: No more lockers available, giving up until reroll");
				bot.hasDepotTarget = false;
				clearDepotBlacklist(bot.guid);
				return;
			}
		}

		// If locker is on a different z-level, trigger floor change first
		if (bot.currentPos.z != bot.idleDepotTarget.z && bot.fcState == FloorChangeState::NONE) {
			bool goDown = bot.idleDepotTarget.z > bot.currentPos.z;
			castLog(bot, fmt::format("DEPOT: Locker at z={}, bot at z={}, triggering floor change {}",
				bot.idleDepotTarget.z, bot.currentPos.z, goDown ? "DOWN" : "UP"));
			startFloorChange(bot, goDown, bot.idleDepotTarget);
			return;
		}
		if (bot.fcState != FloorChangeState::NONE) return;

		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.idleDepotTarget.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.idleDepotTarget.y)));
		if (dist <= 1 && bot.currentPos.z == bot.idleDepotTarget.z) {
			// Adjacent to locker — wait 20-60s then reroll where to dwell
			bot.hasDepotTarget = false;
			clearDepotBlacklist(bot.guid);
			int32_t waitSecs = uniform_random(20, 60);
			s_depotLockerRerollTime[bot.guid] = OTSYS_TIME() + waitSecs * 1000LL;
			castLog(bot, fmt::format("DEPOT: Arrived at locker, waiting {}s before reroll", waitSecs));
		} else {
			if (!goTo(bot, bot.idleDepotTarget, 1)) {
				bot.hasDepotTarget = false; // Can't reach — give up
			}
		}
		return;
	}

	// For all other idle actions, wait if still walking
	if (!player->listWalkDir.empty()) {
		// Planner walk that has stopped making progress. This check MUST be here rather than
		// inside planScopedWalk: the branch below returns while a walk is queued, so the
		// planner is never called mid-walk and could never observe its own stall.
		//
		// A failed autowalk step is not dropped by the server — creature.cpp re-issues the same
		// blocked step indefinitely and only sends the player a cancel message, which a bot has
		// no client to receive. So without this a bot that walks into a closed door retries it
		// forever and doIdle returns forever; nothing else notices until the 2-minute stale-target
		// guard. Re-deriving picks the door up as a DOOR waypoint and opens it.
		if (isPlannerWalk(bot) && plannerWalkBlocked(bot, player)) {
			castLog(bot, fmt::format("PLAN: walk stalled at ({},{},{}) — re-deriving route to ({},{},{})",
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
			plannerReplan(bot, player);
			// Fall through — the walk queue is now empty, so this tick re-plans immediately.
		} else
		// Safety: if no active walk target and walk queue has been stale for >5s, clear it
		if (!bot.hasWalkTarget && !bot.followingCityRoute) {
			auto& staleStart = s_staleWalkStart[bot.guid];
			if (staleStart == 0) staleStart = OTSYS_TIME();
			if (OTSYS_TIME() - staleStart > 5000) {
				castLog(bot, fmt::format("IDLE: Clearing stale walk queue ({} steps, no target)",
					player->listWalkDir.size()));
				player->listWalkDir.clear();
				player->stopEventWalk();
				s_staleWalkStart.erase(bot.guid);
				// Fall through to reroll check below
			} else {
				return; // Still within grace period — let walk drain naturally
			}
		} else {
			s_staleWalkStart.erase(bot.guid); // Active target — reset timer
			return;
		}
	} else {
		s_staleWalkStart.erase(bot.guid); // Walk queue empty — reset timer
	}

	// Arrival for a planner walk that has NO POI behind it — i.e. `/cavebot goto`.
	//
	// The POI arrival check below is gated on `bot.currentPOI`, which the goto command explicitly
	// sets to nullptr. Nothing else consumes the walk target, so the bot reaches the tile and then
	// keeps re-deciding to walk there every tick; the only thing that ever ends it is the 2-minute
	// stale-target guard. Two visible symptoms, both reported: the walk never "finishes", and
	// moving or teleporting the bot makes it immediately set off for the old target again.
	//
	// Scoped to isPlannerWalk deliberately. `hasWalkTarget && !currentPOI` is NOT specific enough:
	// bot_hunt.cpp's depot-recovery walk clears currentPOI and then sets walkTarget (bot_hunt.cpp
	// ~921-932), so a generic no-POI arrival rule would silently consume that walk too.
	// BOT_HOUSE_VISIT arrival. Deliberately ahead of BOTH arrival blocks below and gated only on
	// the run, not on currentPOI: a POI-driven visit has one and a forced `/cavebot house` does
	// not, and both need the same exact-tile test. Consumes the tick when it acts.
	if (bot.hasWalkTarget && isHouseVisiting(bot.guid) && tryHouseArrival(bot)) {
		return;
	}

	// BOT_SHRINE_IDLE arrival. Same placement and the same reasoning as the house hook directly
	// above: ahead of BOTH arrival blocks below, and gated on the run rather than on currentPOI,
	// because a POI-driven visit has one and a forced `/cavebot <bot> shrine` does not, and both
	// need the same EXACT-tile test. A shrine visit reserves one specific tile so the bot can face
	// the furniture, so the 3-tile tolerance below would start the visit looking at a wall.
	if (bot.hasWalkTarget && isShrineVisiting(bot.guid) && tryShrineArrival(bot)) {
		return;
	}

	// BOT_AMBIENT_ROAM. Both calls sit HERE, ahead of the generic planner-arrival block below,
	// and that placement is load-bearing rather than stylistic:
	//
	//  * tryRoamArrival must see hasWalkTarget still TRUE. The block below consumes the walk and
	//    the planner claim on arrival, so an arrival hook placed after it would find both already
	//    gone and could no longer tell "the leg succeeded" from "the leg was torn away" — which
	//    would end every session on the tick it arrived.
	//  * roamDriveWalk carries the monster self-defense, and it must run before anything that can
	//    end a session, or a bot gets released for standing still while it is in fact fighting.
	if (isRoaming(bot.guid)) {
		if (bot.hasWalkTarget && tryRoamArrival(bot)) return;
		if (roamDriveWalk(bot)) return;
	}

	// A house visit driven by `/cavebot <bot> house` also has no POI behind it, and this block's
	// 3-tile tolerance would consume its walk while the bot is still OUTSIDE the house — the run
	// would then sit in s_houseRuns holding a tile, a dummy and an occupancy slot for a visit that
	// never starts. Observed live: the bot stopped two tiles from its target, this branch declared
	// "arrived at goto target", and the visit silently evaporated. The house path does its own
	// exact-tile arrival further down; keep this one off it.
	if (bot.hasWalkTarget && !bot.currentPOI && isPlannerWalk(bot) && !isHouseVisiting(bot.guid)
		&& !isShrineVisiting(bot.guid)
		&& isAtPosition(bot.currentPos, bot.walkTarget, POI_ARRIVAL_DIST)
		&& bot.currentPos.z == bot.walkTarget.z) {
		// BOT_SUPPLY_REALISM: a fishing trip's RETURN leg lands here (no POI, planner-claimed).
		// Matched on the phase AND the exact target, never on "this bot has a fishing run":
		// this block is shared with `/cavebot goto`, which is not an interruption hook, so an
		// admin retargeting a bot mid-session would otherwise be read as "trip complete" and
		// log the wrong coordinates.
		if (auto fr = s_fishing.find(bot.guid);
			fr != s_fishing.end() && fr->second.phase == FishPhase::RETURNING
			&& bot.walkTarget == fr->second.home) {
			castLog(bot, fmt::format("FISH: home at ({},{},{}) — trip complete",
				bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
			g_logger().info("[BotFish] guid={} name='{}' home — trip complete", bot.guid, bot.name);
			clearFishingRun(bot.guid); // also drops the planner claim and normalizes dwellUntil
			bot.hasWalkTarget = false;
			bot.pendingNavDest.clear();
			bot.pathFailCount = 0;
			bot.consecutivePOIFails = 0;
			s_walkTargetTimer.erase(bot.guid);
			return;
		}
		castLog(bot, fmt::format("IDLE: Arrived at goto target ({},{},{})",
			bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
		bot.hasWalkTarget = false;
		bot.pendingNavDest.clear();
		bot.pathFailCount = 0;
		bot.consecutivePOIFails = 0;
		s_walkTargetTimer.erase(bot.guid);
		clearPlannerWalk(bot.guid);
		return;
	}

	// Check POI arrival
	//
	// The shrine types are excluded here, TYPE-QUALIFIED rather than on isShrineVisiting alone.
	// This block declares "arrived" at POI_ARRIVAL_DIST (3), which for a shrine would clear
	// currentPOI and hasWalkTarget and start a GENERIC dwell up to three tiles off the reserved
	// tile — leaving the run stranded in APPROACH holding its claim, since tryShrineArrival above
	// is gated on hasWalkTarget and would never get another chance. Qualifying on the POI type as
	// well as the run means a stale run cannot suppress this block for an unrelated arrival (a
	// WATER walk, say) and starve the fishing intercept further down.
	const bool shrinePoiArrival = bot.currentPOI
		&& (bot.currentPOI->type == POIType::REWARD_SHRINE
		    || bot.currentPOI->type == POIType::IMBUING_SHRINE)
		&& isShrineVisiting(bot.guid);
	if (bot.hasWalkTarget && bot.currentPOI && !shrinePoiArrival) {
		bool arrived = isAtPosition(bot.currentPos, bot.walkTarget, POI_ARRIVAL_DIST) &&
			bot.currentPos.z == bot.walkTarget.z;
		if (arrived) {
			// Adventurer's Stone POI: instead of dwelling, kick off the dungeon trip.
			// startAdventurerStoneTrip() validates PZ + temple range + not pz-locked;
			// on failure, fall through to normal dwell so the bot idles at the temple
			// like a regular Temple POI (next POI roll will retry the trip naturally).
			if (bot.currentPOI->type == POIType::ADVENTURER_STONE) {
				if (startAdventurerStoneTrip(bot)) {
					bot.hasWalkTarget = false;
					bot.pendingNavDest.clear();
					bot.currentPOI = nullptr;
					bot.pathFailCount = 0;
					bot.consecutivePOIFails = 0;
					return;
				}
				// fall through to normal dwell
			}
			// Social NPC visit: greet on arrival, exactly like the boat-captain path does, then
			// let the dwell below hold the bot at the counter for a while. The tile claim is
			// released here rather than at dwell end — once the bot is physically standing on it,
			// resolveNpcApproach's occupancy check already keeps other bots off it.
			if (bot.currentPOI->type == POIType::NPC) {
				auto nvIt = s_pendingNpcVisit.find(bot.guid);
				if (nvIt != s_pendingNpcVisit.end()) {
					if (auto p = bot.getPlayer()) {
						// Canary's FOCUS_GREETWORDS is { "hi", "hello" } (data/npclib/npc_system/
						// modules.lua) — "hey"/"heya" are said but draw no reply, which is what
						// happens when a real player uses them too.
						static const char* kGreetings[] = { "hi", "hello", "hey", "heya" };
						const char* greet = kGreetings[uniform_random(0, 3)];
						g_game().internalCreatureSay(p, TALKTYPE_SAY, greet, false);
						castLog(bot, fmt::format("IDLE: greeting NPC '{}' with '{}'", nvIt->second, greet));
						// Journal signal so NPC visits are verifiable without an in-game observer
						// (castLog only reaches cast viewers / verboseLog bots). Bounded by the
						// 15-60s dwell, so this cannot spam even with the whole pool awake.
						g_logger().info("[POI_SELECT_NPC] bot guid={} name='{}' town={} greeted '{}' with '{}' at ({},{},{})",
							bot.guid, bot.name, bot.townId, nvIt->second, greet,
							bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
					}
					s_pendingNpcVisit.erase(nvIt);
				}
				releaseNpcApproach(bot.guid);
			}
			// BOT_SUPPLY_REALISM: a WATER POI is the shoreline stand tile. Start the fishing run
			// instead of dwelling generically — without this branch the arrival falls through to
			// the tail below, the bot dwells like any other POI, and no fishing ever happens.
			// Returns early like ADVENTURER_STONE does, so it replicates the tail's cleanup.
			// startFishingRun also holds dwellUntil open past the session (doDwelling's tail is
			// the sole authority for leaving DWELLING and knows nothing about the run).
			if (bot.currentPOI->type == POIType::WATER) {
				startFishingRun(bot);
				bot.hasWalkTarget = false;
				bot.pendingNavDest.clear();
				bot.currentPOI = nullptr;
				bot.pathFailCount = 0;
				bot.consecutivePOIFails = 0;
				clearPlannerWalk(bot.guid);
				bot.state = BotAIState::DWELLING;
				// startFishingRun bails (leaving no run) when it can't find water adjacent; in
				// that case dwellUntil was never stretched, so fall back to an ordinary dwell so
				// the bot doesn't sit on a zero timer.
				if (!isFishing(bot.guid)) {
					bot.dwellUntil = OTSYS_TIME() + uniform_random(
						g_configManager().getNumber(BOT_DWELL_NPC_MIN_SEC),
						g_configManager().getNumber(BOT_DWELL_NPC_MAX_SEC)) * 1000;
				}
				return;
			}
			// NPC and SHOP POIs get shorter dwell times
			bool isShortDwell = (bot.currentPOI->type == POIType::NPC || bot.currentPOI->type == POIType::SHOP);
			int dwellTime = isShortDwell
				? uniform_random(g_configManager().getNumber(BOT_DWELL_NPC_MIN_SEC), g_configManager().getNumber(BOT_DWELL_NPC_MAX_SEC))
				: uniform_random(g_configManager().getNumber(BOT_DWELL_POI_MIN_SEC), g_configManager().getNumber(BOT_DWELL_POI_MAX_SEC));
			castLog(bot, fmt::format("IDLE: Arrived at {}, dwelling for {}s{}",
				bot.currentPOI->name, dwellTime, isShortDwell ? " (short)" : ""));
			bot.hasWalkTarget = false;
			bot.pendingNavDest.clear();   // route was for getting here; destination consumed
			bot.currentPOI = nullptr;
			bot.pathFailCount = 0;
			bot.consecutivePOIFails = 0;
			clearPlannerWalk(bot.guid);   // arrived — release the planner claim and its hop plan
			bot.dwellUntil = OTSYS_TIME() + dwellTime * 1000;
			bot.state = BotAIState::DWELLING;
			return;
		}
	}

	// Autonomous activity reroll — picks next activity when idle
	if (!bot.hasWalkTarget) {
		if ((bot.tickCounter % 100) == 0) {  // check every ~10s (cooldown is 60s anyway)
			doActivityReroll(bot);
		}
		return;
	}

	// Try city route for navigate command (pendingNavDest set by reroll or /cavebot navigate)
	if (!bot.followingCityRoute && !bot.pendingNavDest.empty()) {
		if (startCityRoute(bot, "", bot.pendingNavDest)) {
			// Check if already near the last waypoint — skip the route
			if (!bot.cityRouteWps.empty()) {
				auto& lastWp = bot.cityRouteWps.back().pos;
				int32_t distToEnd = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(lastWp.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(lastWp.y)));
				if (distToEnd <= 5 && bot.currentPos.z == lastWp.z) {
					castLog(bot, fmt::format("IDLE: Already near route end ({},{},{}), skipping",
						lastWp.x, lastWp.y, lastWp.z));
					std::string dest = bot.lastRouteDestination;
					bot.followingCityRoute = false;
					bot.cityRouteWps.clear();
					bot.cityRouteIdx = 0;
					bot.pendingNavDest.clear();
					// Trigger depot locker walk if dest was "depot"
					if (dest == "depot") {
						auto lockerPos = findReachableDepotLocker(bot);
						if (lockerPos.x != 0) {
							bot.idleDepotTarget = lockerPos;
							bot.hasDepotTarget = true;
							s_depotWalkRetries[bot.guid] = 0;
						}
					}
					bot.hasWalkTarget = false;
					return;
				}
			}
			castLog(bot, fmt::format("IDLE: Using city route to '{}'", bot.pendingNavDest));
			bot.pendingNavDest.clear();
		} else {
			castLog(bot, fmt::format("IDLE: No city route to '{}', using direct pathfinding", bot.pendingNavDest));
			bot.pendingNavDest.clear();
		}
	}

	// Safety: clear stale walkTarget after no progress for the budget below.
	if (bot.hasWalkTarget && !bot.followingCityRoute) {
		uint64_t targetHash = static_cast<uint64_t>(bot.walkTarget.x)
			+ static_cast<uint64_t>(bot.walkTarget.y) * 65536
			+ static_cast<uint64_t>(bot.walkTarget.z) * 65536 * 65536;
		auto& walkTimer = s_walkTargetTimer[bot.guid];
		// Planner walks get double. NPC visits are now eligible town-wide (up to 150 tiles and 7
		// floors), and at roughly 300ms per tile such a walk plus its floor changes does not fit
		// in 2 minutes — the guard would abandon journeys that are progressing perfectly well.
		const int64_t walkBudgetMs = isPlannerWalk(bot) ? 240000 : 120000;
		if (walkTimer.first != targetHash) {
			walkTimer = {targetHash, OTSYS_TIME()};
		} else if (OTSYS_TIME() - walkTimer.second > walkBudgetMs) {
			castLogError(bot, fmt::format("IDLE: Stale walkTarget timeout ({}s) at ({},{},{}) — clearing",
				walkBudgetMs / 1000, bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
			trackNavEvent("idle_stale_target", bot, 0, "", bot.townId, "",
				fmt::format("target ({},{},{})", bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
			bot.hasWalkTarget = false;
			bot.pendingNavDest.clear();
			bot.currentPOI = nullptr;
			bot.pathFailCount = 0;
			s_walkTargetTimer.erase(bot.guid);
			// The planner claim self-invalidates once walkTarget changes, but the hop plan and
			// synthetic leg it owns would survive; drop them with the walk that created them.
			// clearFishingRun is a superset of clearPlannerWalk: a fishing walk that goes stale
			// here never reached startFishingRun, so the stand+water claim taken at selection
			// time would otherwise sit for the full FISH_CLAIM_MS with nothing behind it.
			clearFishingRun(bot.guid);
			endHouseVisit(bot.guid, "stale");
			// Blacklist BEFORE ending: the stale path is the one that does NOT touch visitedPOIs,
			// so without recording the shrine here the same bot re-picks the same unreachable
			// shrine on the very next reroll and burns another four-minute walk, forever.
			if (auto sit = s_shrineRuns.find(bot.guid); sit != s_shrineRuns.end()) {
				blacklistShrine(bot.guid, sit->second.shrine);
			}
			endShrineVisit(bot.guid, "stale");
			return;
		}
	}

	// Fallback: raw A* pathfinding to walk target (NOT when following a city route — route handles its own nav)
	if (bot.hasWalkTarget && !bot.followingCityRoute) {
		// goToClosest, not goToWithDoors: walk right up to the destination when the map allows
		// it instead of always stopping at the default 3 tiles. Matters most for NPC visits —
		// standing 3 tiles off every shopkeeper reads as robotic; a real player steps up to the
		// counter. Degrades to 3 automatically when the near tiles are taken or unreachable, and
		// only tightens once the bot is already close, so the long walk in costs nothing extra.
		// The scoped route planner takes over ONLY for the two walks that claimed it (an admin
		// `goto`, or an NPC visit) and only while the claim still matches this exact target.
		// Everything else — depot recovery, temple returns, shops, boats — keeps the generic
		// path, which is the whole point of the split.
		// A house visit asks for the EXACT tile (maxDist 0), not the usual 3. Its arrival test is
		// exact, so a planner that stops three tiles short would leave the bot waiting out the
		// settle deadline and then idling somewhere it did not choose — with the sub-activity it
		// walked there for dropped.
		const bool navStarted = isPlannerWalk(bot)
			? planScopedWalk(bot, bot.walkTarget,
				(isHouseVisiting(bot.guid) || isShrineVisiting(bot.guid)) ? 0 : 3)
			: goToClosest(bot, bot.walkTarget);
		if (bot.verboseLog) {
			// goToClosest returning true only means a walk/plan was STARTED. A bot that reports
			// success every tick while its position never changes is the signature of a nav that
			// hands off (e.g. to the FC machine) without anything actually executing, which is
			// otherwise completely silent.
			castLog(bot, fmt::format("IDLE-NAV: {}({},{},{}) -> {} | pos=({},{},{}) fc={} fails={} queued={}",
				isPlannerWalk(bot) ? "planScopedWalk" : "goToClosest",
				bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z, navStarted ? "started" : "FAILED",
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
				static_cast<int>(bot.fcState), bot.pathFailCount,
				player->listWalkDir.size()));
		}
		// "started" with an EMPTY walk queue is the lie that hides a dead planner leg: the
		// planner reports success, nothing is queued, the bot never moves, and because
		// navStarted is true neither pathFailCount nor the stall detector (which needs a
		// non-empty queue) ever engages. Deliberately NOT gated behind verboseLog and
		// deliberately at error level — the whole value is catching bots nobody is watching.
		// pendingWalkPauseEventId is excluded because a deferred walk is a real, intended pause.
		if (navStarted && player->listWalkDir.empty() && bot.pendingWalkPauseEventId == 0) {
			noteNavStartedButIdle(bot);
		} else {
			s_navStartedButIdle.erase(bot.guid);
		}
		if (!navStarted) {
			tryAttackBlockingMonster(bot);
			bot.pathFailCount++;
			// NPC squeeze: same guarded mechanism as followWaypoints. Direct-nav to a POI
			// (temple/NPC/shop) gives up fast (>=3), so attempt the squeeze before that —
			// the helper only acts when an NPC is the chokepoint with open ground beyond,
			// and resets pathFailCount on success so the bot keeps progressing.
			if (bot.pathFailCount >= 2 && tryStepPastBlockingNpc(bot, bot.walkTarget)) {
				bot.pathFailCount = 0;
				return;
			}
			if (bot.pathFailCount >= 3) {
				castLogError(bot, fmt::format("IDLE: Can't reach {} after {} fails, giving up",
					bot.currentPOI ? bot.currentPOI->name : "target", bot.pathFailCount));
				if (bot.currentPOI) bot.visitedPOIs.insert(bot.currentPOI->name);
				bot.hasWalkTarget = false;
				bot.pendingNavDest.clear();
				bot.currentPOI = nullptr;
				// Superset of clearPlannerWalk — also drops the stand+water claim a fishing
				// walk took at selection time but never got to use.
				clearFishingRun(bot.guid);
				endHouseVisit(bot.guid, "giveup");
				if (auto sit = s_shrineRuns.find(bot.guid); sit != s_shrineRuns.end()) {
					blacklistShrine(bot.guid, sit->second.shrine);
				}
				endShrineVisit(bot.guid, "giveup");
				bot.consecutivePOIFails++;
				if (bot.consecutivePOIFails >= STUCK_THRESHOLD) {
					if (!pzLocked) {
						if (!findNearestRecoveryRoute(bot)) {
							castLogError(bot, "IDLE: Too many fails, teleporting to temple");
							teleportToTemple(bot);
						}
					} else {
						castLogError(bot, "IDLE: Too many fails but PZ-locked, standing still");
					}
					bot.consecutivePOIFails = 0;
					bot.visitedPOIs.clear();
				}
			}
		} else {
			bot.pathFailCount = 0;
		}
	}
}

// ============================================================================
// DWELLING state
// ============================================================================

void BotEngine::doDwelling(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// BOT_PARTY_INVITE_RENDEZVOUS: defensive twin of the doIdle preempt. Enrolment forces IDLE,
	// but a wake can restore DWELLING (observed: a woken member came back state=2), and every
	// dwell walk below would pull it away from its leader.
	if (!s_rvMember.empty() && s_rvMember.count(bot.guid) > 0) {
		if (handleAssemblyStaging(bot)) return;
	}

	// Vigilante: scan for PKers (DWELLING state)
	checkVigilante(bot);
	if (bot.state != BotAIState::DWELLING) return;

	// PZ-blocked roaming (Feature 2): mill around non-PZ space while pz-locked instead of the
	// depot-anchored dwell walks below (which would route into a PZ).
	if (handlePzRoam(bot)) return;

	// BOT_LIVENESS_PACK Phase C: turn-in-place / chat / micro-actions.
	// All sub-behaviors are no-ops if their gates fail (combat, walking, FC,
	// dummy training). Safe to call unconditionally from every dwelling tick.
	tickLivenessBehaviors(bot);
	// Idle litter drop (drop-only, once per wake) — self-gates on stop/errand state.
	maybeFidgetDrop(bot);

	// BOT_SUPPLY_REALISM: drive an active fishing session. Placed HERE, ahead of the four
	// unconditional early-returns below (dwell walk target, locker reroll, hasDepotTarget, and
	// handlePzRoam above) — any of which would otherwise starve it for as long as that state
	// takes to resolve. Returns true while it owns the tick.
	if (tickFishingRun(bot)) {
		return;
	}
	// BOT_AMBIENT_ROAM: same early slot, for the same reason as the two neighbours — the four
	// unconditional early-returns below (dwell walk target, locker reroll, depot target) would
	// starve the next-leg picker, and the dwell tail further down would fire its own IDLE
	// transition first and hand the bot to the activity reroll, ending the session by accident.
	if (tickRoamSession(bot)) {
		return;
	}
	// BOT_HOUSE_VISIT: same early slot, for the same reason — the four unconditional early-returns
	// below would starve an active house visit, and since startHouseVisit holds dwellUntil open a
	// starved run would never reach the tail that ends it either. Also carries the displacement
	// guard (kicked by an ownership transfer, teleported by a GM), which must run even on the ticks
	// where the run itself has nothing to do.
	if (tickHouseVisit(bot)) {
		return;
	}
	// BOT_SHRINE_IDLE: same early slot, same reason as its three neighbours — startShrineVisit
	// holds dwellUntil open past the idle window, so a run starved by the unconditional
	// early-returns below would never reach the tail that ends it. Also carries the displacement
	// guard, which must run even on ticks where the visit itself has nothing to do.
	if (tickShrineVisit(bot)) {
		return;
	}

	// Depot dwell walk target: handle walking to PZ roam / step outside destination
	{
		auto walkIt = s_depotDwellWalkTarget.find(bot.guid);
		if (walkIt != s_depotDwellWalkTarget.end()) {
			if (!player->listWalkDir.empty()) return; // Still walking
			Position target = walkIt->second;

			// Cancel dwell walk if target is on a different z — no FC during dwelling PZ roaming
			if (bot.currentPos.z != target.z) {
				castLog(bot, fmt::format("DWELLING: Cancelling dwell walk to ({},{},{}) — different z, no FC in PZ dwell",
					target.x, target.y, target.z));
				s_depotDwellWalkTarget.erase(walkIt);
				s_depotDwellWalkFails.erase(bot.guid);
				return;
			}

			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(target.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(target.y)));
			if (dist <= 1) {
				castLog(bot, fmt::format("DWELLING: Arrived at dwell target ({},{},{})", target.x, target.y, target.z));
				s_depotDwellWalkTarget.erase(walkIt);
				s_depotDwellWalkFails.erase(bot.guid);
			} else {
				if (!goToWithDoors(bot, target, 1)) {
					s_depotDwellWalkFails[bot.guid]++;
					if (s_depotDwellWalkFails[bot.guid] >= 3) {
						castLog(bot, fmt::format("DWELLING: Can't reach dwell target ({},{},{}) after 3 fails, cancelling",
							target.x, target.y, target.z));
						s_depotDwellWalkTarget.erase(walkIt);
						s_depotDwellWalkFails.erase(bot.guid);
					}
				}
			}
			return;
		}
	}

	// Depot locker reroll: after waiting 20-60s at locker, decide where to dwell
	{
		auto rerollIt = s_depotLockerRerollTime.find(bot.guid);
		if (rerollIt != s_depotLockerRerollTime.end()) {
			if (OTSYS_TIME() < rerollIt->second) return; // Still waiting at locker
			s_depotLockerRerollTime.erase(rerollIt);

			// Verify bot is actually near a depot — stale timer from previous town
			bool nearDepot = false;
			for (int dx = -2; dx <= 2 && !nearDepot; dx++) {
				for (int dy = -2; dy <= 2 && !nearDepot; dy++) {
					auto t = g_game().map.getTile(Position(bot.currentPos.x + dx, bot.currentPos.y + dy, bot.currentPos.z));
					if (t && t->hasFlag(TILESTATE_DEPOT)) nearDepot = true;
				}
			}
			if (!nearDepot) {
				castLog(bot, "DEPOT: Stale locker timer, not near depot — skipping reroll");
				return;
			}

			// botDepotLockerPct: stay at the locker vs mill about outside. Duplicated at two
			// arrival sites (travel arrival and POI arrival); both read the same key so they
			// cannot drift apart the way the hardcoded 40s could.
			const int32_t lockerPct = static_cast<int32_t>(g_configManager().getNumber(BOT_DEPOT_LOCKER_PCT));
			int depotRoll = uniform_random(1, 100);
			// Remainder is split evenly between "roam to a PZ tile" and "step outside", so the
			// three branches stay coherent at ANY lockerPct. A hardcoded second bound (it was 70)
			// would silently make this branch unreachable the moment lockerPct was raised past it.
			const int32_t roamBound = lockerPct + (100 - lockerPct) / 2;
			if (depotRoll <= lockerPct) {
				castLog(bot, fmt::format("DEPOT: Staying at locker ({}%)", lockerPct));
			} else if (depotRoll <= roamBound) {
				Position pzTile = findRandomReachablePZTile(bot.currentPos);
				if (pzTile.x > 0 && pzTile.z == bot.currentPos.z) {
					s_depotDwellWalkTarget[bot.guid] = pzTile;
					s_depotDwellWalkFails[bot.guid] = 0;
					castLog(bot, fmt::format("DEPOT: Roaming to PZ tile ({},{},{})",
						pzTile.x, pzTile.y, pzTile.z));
				} else {
					castLog(bot, "DEPOT: No reachable same-z PZ tile, staying at locker");
				}
			} else {
				Position nonPzTile = findClosestNonPZTile(bot.currentPos);
				if (nonPzTile.x > 0 && nonPzTile.z == bot.currentPos.z) {
					s_depotDwellWalkTarget[bot.guid] = nonPzTile;
					s_depotDwellWalkFails[bot.guid] = 0;
					castLog(bot, fmt::format("DEPOT: Stepping outside to ({},{},{})",
						nonPzTile.x, nonPzTile.y, nonPzTile.z));
				} else {
					castLog(bot, "DEPOT: No same-z non-PZ tile found, staying at locker");
				}
			}
			return;
		}
	}

	if (bot.hasDepotTarget) {
		if (!player->listWalkDir.empty()) return;

		// Track walk retries — after 5 idle ticks, blacklist locker and try a new one
		s_depotWalkRetries[bot.guid]++;
		if (s_depotWalkRetries[bot.guid] > 5) {
			blacklistDepotLocker(bot.guid, bot.idleDepotTarget);
			if (s_depotBlacklist[bot.guid].size() >= 5) {
				castLog(bot, "DEPOT: Tried 5 lockers, giving up until reroll");
				bot.hasDepotTarget = false;
				clearDepotBlacklist(bot.guid);
				return;
			}
			Position newLocker = findReachableDepotLocker(bot);
			if (newLocker.x != 0) {
				castLog(bot, fmt::format("DEPOT: Retrying with locker {}/5 at ({},{},{})",
					s_depotBlacklist[bot.guid].size() + 1,
					newLocker.x, newLocker.y, newLocker.z));
				bot.idleDepotTarget = newLocker;
				s_depotWalkRetries[bot.guid] = 0;
			} else {
				castLog(bot, "DEPOT: No more lockers available, giving up until reroll");
				bot.hasDepotTarget = false;
				clearDepotBlacklist(bot.guid);
				return;
			}
		}

		// If locker is on a different z-level, trigger floor change first
		if (bot.currentPos.z != bot.idleDepotTarget.z && bot.fcState == FloorChangeState::NONE) {
			bool goDown = bot.idleDepotTarget.z > bot.currentPos.z;
			castLog(bot, fmt::format("DEPOT: Locker at z={}, bot at z={}, triggering floor change {}",
				bot.idleDepotTarget.z, bot.currentPos.z, goDown ? "DOWN" : "UP"));
			startFloorChange(bot, goDown, bot.idleDepotTarget);
			return;
		}
		// If floor change is in progress, let it complete
		if (bot.fcState != FloorChangeState::NONE) return;

		int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(bot.idleDepotTarget.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(bot.idleDepotTarget.y)));
		if (dist <= 1 && bot.currentPos.z == bot.idleDepotTarget.z) {
			// Adjacent to locker — wait 20-60s then reroll where to dwell
			bot.hasDepotTarget = false;
			clearDepotBlacklist(bot.guid);
			tryEmitChat(bot, player, "depot", /*channelId=*/0);
			int32_t waitSecs = uniform_random(20, 60);
			s_depotLockerRerollTime[bot.guid] = OTSYS_TIME() + waitSecs * 1000LL;
			castLog(bot, fmt::format("DEPOT: Arrived at locker, waiting {}s before reroll", waitSecs));
		} else {
			// Walk to tile adjacent to locker (maxDist=1 means stop adjacent)
			if (!goTo(bot, bot.idleDepotTarget, 1)) {
				bot.hasDepotTarget = false; // Can't reach — give up
			}
		}
		return;
	}

	if (OTSYS_TIME() >= bot.dwellUntil) {
		bot.state = BotAIState::IDLE;
		bot.hasWalkTarget = false;
		bot.currentPOI = nullptr;
		bot.hasDepotTarget = false;
		bot.nextRerollTime = OTSYS_TIME() + uniform_random(3, 8) * 1000;  // short thinking pause
	}
}


// ============================================================================
// Floor change state machine (Phase 2)
// ============================================================================

void BotEngine::startFloorChange(BotState& bot, bool goDown, const Position& targetPos) {
	// Refuse to start after 5 consecutive failures — wait for reroll
	if (s_fcConsecutiveFailures[bot.guid] >= 5) {
		return;
	}
	// TRUE MULTI-FLOOR: consult the z-route planner so EVERY caller (goTo,
	// depot walks, hunt prep, PK pursuit) gets a globally planned first hop
	// instead of relying on the greedy 12-tile scan. goTo may have planned
	// already (far-portal pre-leg walk); a stale plan for another floor is
	// dropped and re-derived. No plan → legacy greedy behavior, unchanged.
	if (zGraphReady_ && targetPos.z != bot.currentPos.z) {
		auto plannedIt = s_plannedFc.find(bot.guid);
		if (plannedIt != s_plannedFc.end() && plannedIt->second.portal.pos.z != bot.currentPos.z) {
			s_plannedFc.erase(plannedIt);
			plannedIt = s_plannedFc.end();
		}
		if (plannedIt == s_plannedFc.end()) {
			ZPlannedHop hop;
			if (zPlanNextHop(bot, targetPos, hop)) {
				plannedIt = s_plannedFc.emplace(bot.guid, hop).first;
			}
		}
		if (plannedIt != s_plannedFc.end()) {
			// The planned hop's direction wins — a multi-hop route may go UP
			// first even when the target floor is below (or vice versa).
			goDown = plannedIt->second.portal.goesDown;
		}
	}
	bot.fcState = FloorChangeState::SCANNING;
	bot.fcGoDown = goDown;
	bot.fcTargetPos = targetPos;
	bot.fcTransitions.clear();
	bot.fcTransIdx = 0;
	bot.fcPreZ = 0;
	bot.fcAttempts = 0;
	bot.fcStartTime = OTSYS_TIME();
}

// Called wherever the FC machine is about to abandon bot.fcTransitions[fcTransIdx] and move on to
// the next candidate. Returns true when it has already handled the give-up (caller must NOT
// advance fcTransIdx); false means "carry on with the legacy behaviour".
//
// Why this exists: the SCANNING state injects the PLANNED portal at index 0 of a
// distance-sorted list, but four separate `fcTransIdx++` sites used to walk past it on any
// difficulty and silently take a nearer, unplanned transition. The bot then changed floor in the
// wrong place and the target became unreachable — measured on a Thais NPC tour: Black Bert planned
// stairs at (32334,32206,7) but changed floor at (32350,32225,6) and gave up 15 tiles short;
// Gamel and Henricus failed the same way. The planner's entire job is choosing WHICH portal, so
// the executor must not quietly overrule it.
//
// For a bot with NO plan this is a pure no-op passthrough, so authored-waypoint navigation
// (hunts, city routes, travel) keeps the greedy list behaviour it has always had.
//
// Blacklisting on give-up is mandatory, not optional: the planner is deterministic, so dropping
// the plan without quarantining the portal would replan the SAME portal next tick and livelock.
// Once blacklisted, zPlanFullRoute's exclude lambda routes around it, and if every portal out of
// the component is blacklisted the existing s_fcConsecutiveFailures >= 5 backstop pauses FC.
bool BotEngine::fcGiveUpOnPlannedTrans(BotState& bot, const ZTransition& trans, uint8_t maxRetries,
                                       const char* site) {
	auto plannedIt = s_plannedFc.find(bot.guid);
	if (plannedIt == s_plannedFc.end() || plannedIt->second.portal.pos != trans.pos) {
		return false; // no plan, or this isn't the planned entry — legacy path
	}
	if (maxRetries > 0) {
		uint8_t& fails = s_fcPlannedApproachFails[bot.guid];
		if (++fails < maxRetries) {
			return true; // retry the SAME planned portal next tick; do not advance
		}
		s_fcPlannedApproachFails.erase(bot.guid);
	}
	// BOT_PARTY_TRAIL_FOLLOW: a party follower replaying a RESOLVED hop (curated waypoint type
	// or portal-graph match) is jammed at a portal its leader traversed seconds ago — that is
	// local congestion, not evidence of a bad portal, and s_zPortalBlacklist is engine-wide for
	// 10 minutes (poisoning it would reroute every bot on the server). Skip the quarantine for
	// those sessions ONLY. Synthetic INFERRED sessions (portalResolved == false) matched
	// nothing and have no confirmed mechanism behind them, so they keep the unconditional
	// blacklist — repeated independent failures there are exactly the signal it exists to
	// record. Anti-livelock is preserved either way: the mandatory-blacklist rule above exists
	// because the deterministic PLANNER would re-pick the same portal, but a party replay does
	// not source its portal from the planner and is capped at ZHOP_MAX_SESSION_ATTEMPTS before
	// handing off to the teleport watchdog.
	auto zhopIt = s_followerZHopSession.find(bot.guid);
	const bool partyResolvedHop = zhopIt != s_followerZHopSession.end() && zhopIt->second.portalResolved;
	if (partyResolvedHop) {
		castLog(bot, fmt::format("FC: planned portal unreachable — party replay, NOT blacklisting ({},{},{})",
			trans.pos.x, trans.pos.y, trans.pos.z));
	} else {
		castLog(bot, fmt::format("FC: planned portal unreachable — blacklisting ({},{},{})",
			trans.pos.x, trans.pos.y, trans.pos.z));
		zBlacklistPortal(trans.pos, site);
	}
	s_plannedFc.erase(bot.guid);
	s_zHopFail++;
	s_fcConsecutiveFailures[bot.guid]++;
	resetFloorChange(bot);
	return true;
}

void BotEngine::resetFloorChange(BotState& bot) {
	bot.fcState = FloorChangeState::NONE;
	bot.fcTransitions.clear();
	bot.fcTransIdx = 0;
	bot.fcAttempts = 0;
	s_fcBlockerRetries.erase(bot.guid);
	s_fcPlannedApproachFails.erase(bot.guid);
	s_zLegPlan.erase(bot.guid); // synthetic leg belongs to the hop being reset
	// TRUE MULTI-FLOOR: never carry a planned hop across FC sessions — the next
	// goTo/startFloorChange replans from the bot's current position (failure
	// sites blacklist the portal BEFORE resetting, so replans route around it).
	s_plannedFc.erase(bot.guid);
}

// LADDER_ITEM_IDS / isLadderItemId hoisted to bot_engine_impl.hpp
// (TRUE MULTI-FLOOR: shared with the portal-graph builder in bot_zgraph.cpp).

std::shared_ptr<Item> BotEngine::findLadderItem(const Position& pos) {
	auto tile = g_game().map.getTile(pos);
	if (!tile) return nullptr;
	// Check ground item first
	if (auto ground = tile->getGround()) {
		if (ground->isLadder() || isLadderItemId(ground->getID())) return ground;
	}
	// Iterate all stacked items on tile (same pattern as Tile::getDoorItem)
	const auto* items = tile->getItemList();
	if (items) {
		for (const auto& item : *items) {
			if (item->isLadder() || isLadderItemId(item->getID())) return item;
		}
	}
	return nullptr;
}

std::shared_ptr<Item> BotEngine::findSewerItem(const Position& pos) {
	auto tile = g_game().map.getTile(pos);
	if (!tile) return nullptr;
	if (auto ground = tile->getGround()) {
		if (ground->getID() == SEWER_ITEM_ID) return ground;
	}
	const auto* items = tile->getItemList();
	if (items) {
		for (const auto& item : *items) {
			if (item->getID() == SEWER_ITEM_ID) return item;
		}
	}
	return nullptr;
}

static bool isShovelHole(const Position& pos) {
	auto tile = g_game().map.getTile(pos);
	if (!tile) return false;
	auto ground = tile->getGround();
	if (!ground) return false;
	uint16_t gid = ground->getID();
	for (auto id : SHOVEL_HOLE_IDS) {
		if (gid == id) return true;
	}
	return false;
}

static bool isRopeSpot(const Position& pos) {
	auto tile = g_game().map.getTile(pos);
	if (!tile) return false;
	auto ground = tile->getGround();
	if (ground) {
		uint16_t gid = ground->getID();
		for (auto id : ROPE_SPOT_IDS) {
			if (gid == id) return true;
		}
	}
	// Stacked rope spot items (ID 12935)
	const auto* items = tile->getItemList();
	if (items) {
		for (const auto& item : *items) {
			if (item->getID() == 12935) return true;
		}
	}
	return false;
}

bool BotEngine::tryUseLadder(BotState& bot, const Position& ladderPos, bool goDown) {
	auto player = bot.getPlayer();
	if (!player) return false;

	auto useItem = findLadderItem(ladderPos);
	if (useItem) {
		g_actions().useItem(player, ladderPos, 0, useItem, false);
		castLog(bot, fmt::format("FC: Used ladder {} at ({},{},{})",
			useItem->getID(), ladderPos.x, ladderPos.y, ladderPos.z));
		return true;
	}
	// No teleport fallback — just fail so the caller can handle it
	castLogError(bot, fmt::format("FC: No ladder item at ({},{},{}) — failed",
		ladderPos.x, ladderPos.y, ladderPos.z));
	return false;
}

void BotEngine::castLevitateSpell(BotState& bot, const Waypoint& waypoint) {
	auto player = bot.getPlayer();
	if (!player) return;

	// Set facing direction from extraData (e.g. "face_north", "face_south", etc.)
	// This is the direction the player must face before casting levitate
	if (!waypoint.extraData.empty()) {
		Direction faceDir = DIRECTION_NORTH; // default
		if (waypoint.extraData == "face_north") faceDir = DIRECTION_NORTH;
		else if (waypoint.extraData == "face_south") faceDir = DIRECTION_SOUTH;
		else if (waypoint.extraData == "face_east") faceDir = DIRECTION_EAST;
		else if (waypoint.extraData == "face_west") faceDir = DIRECTION_WEST;
		g_game().internalCreatureTurn(player, faceDir);
	}

	// Cast the actual levitate spell — "exani hur up" or "exani hur down"
	bool goUp = (waypoint.type == WaypointType::LEVITATE_UP);
	std::string words = goUp ? "exani hur up" : "exani hur down";

	auto result = g_spells().playerSaySpell(player, words);
	if (result == TALKACTION_BREAK) {
		// Spell accepted by server — show the visual speech bubble
		player->saySpell(TALKTYPE_SAY, words, false);
		castLog(bot, fmt::format("LEVITATE: Cast '{}' facing {} at ({},{},{})",
			words, waypoint.extraData.empty() ? "default" : waypoint.extraData,
			waypoint.pos.x, waypoint.pos.y, waypoint.pos.z));
	} else {
		// Spell failed (cooldown, not enough mana, etc.) — fall back to FC state machine
		castLogError(bot, fmt::format("LEVITATE: Spell '{}' failed (result={}), falling back to FC",
			words, static_cast<int>(result)));
		bool goDown = (waypoint.type == WaypointType::LEVITATE_DOWN);
		startFloorChange(bot, goDown, waypoint.pos);
	}
}

// Parses the extra_data offset markers. `tool:<id>` / `tool:<id>@<dx>,<dy>` / `fish:<dx>,<dy>`.
bool BotEngine::parseOffsetMarker(const std::string& extra, const std::string& prefix,
                                  int32_t& dx, int32_t& dy, uint16_t& itemId) {
	dx = 0; dy = 0; itemId = 0;
	if (extra.rfind(prefix, 0) != 0) return false;
	std::string body = extra.substr(prefix.size());
	if (body.empty()) return false;

	const bool wantsItem = (prefix == "tool:");
	if (wantsItem) {
		const size_t at = body.find('@');
		const std::string idPart = (at == std::string::npos) ? body : body.substr(0, at);
		try { itemId = static_cast<uint16_t>(std::stoi(idPart)); } catch (...) { return false; }
		if (itemId == 0) return false;
		if (at == std::string::npos) return true;   // no offset: act on the waypoint's own tile
		body = body.substr(at + 1);
	}

	const size_t comma = body.find(',');
	if (comma == std::string::npos) return false;
	try {
		dx = std::stoi(body.substr(0, comma));
		dy = std::stoi(body.substr(comma + 1));
	} catch (...) { return false; }
	// Only the 8 neighbours plus the tile itself are reachable for a tool use.
	return std::abs(dx) <= 1 && std::abs(dy) <= 1;
}

bool BotEngine::handleActionWaypoint(BotState& bot, const Waypoint& waypoint) {
	auto player = bot.getPlayer();
	if (!player) return false;

	auto& wp = waypoint.pos;

	// ---- extra_data markers, readable on ANY waypoint type ----
	// The PREFIX ALONE decides the return value, never the parse result: a malformed marker must
	// still make the caller take the action pause. Returning false there would drop the waypoint
	// into the NODE/STAND `continue` path in followWaypoints, which navigates to the next
	// waypoint on the same tick — the exact bug the pause exists to prevent.
	{
		const bool isFish = waypoint.extraData.rfind("fish:", 0) == 0;
		const bool isTool = waypoint.extraData.rfind("tool:", 0) == 0;
		if (isFish || isTool) {
			int32_t dx = 0, dy = 0;
			uint16_t toolId = 0;
			if (!parseOffsetMarker(waypoint.extraData, isFish ? "fish:" : "tool:", dx, dy, toolId)) {
				castLogError(bot, fmt::format("ACTION: malformed marker '{}' at ({},{},{})",
					waypoint.extraData, wp.x, wp.y, wp.z));
				return true; // fail safe, not fail open
			}
			const Position target(static_cast<uint16_t>(wp.x + dx),
			                      static_cast<uint16_t>(wp.y + dy), wp.z);
			if (isFish) {
				beginIceFishSession(bot, target);
			} else if (castToolAt(bot, toolId, target)) {
				castLog(bot, fmt::format("ACTION: Used tool {} at ({},{},{})",
					toolId, target.x, target.y, target.z));
			}
			return true;
		}
	}

	if (waypoint.type == WaypointType::MACHETE) {
		auto tempMachete = Item::CreateItem(MACHETE_ITEM_ID, 1);
		if (tempMachete) {
			g_actions().useItemEx(player, bot.currentPos, wp, 0, tempMachete, false);
			castLog(bot, fmt::format("ACTION: Used machete at ({},{},{})", wp.x, wp.y, wp.z));
		}
	} else if (waypoint.type == WaypointType::USE_WITH && waypoint.itemId > 0) {
		auto tempItem = Item::CreateItem(waypoint.itemId, 1);
		if (tempItem) {
			g_actions().useItemEx(player, bot.currentPos, wp, 0, tempItem, false);
			castLog(bot, fmt::format("ACTION: Used item {} at ({},{},{})", waypoint.itemId, wp.x, wp.y, wp.z));
		}
	} else if (waypoint.type == WaypointType::USE_WITH && waypoint.itemId == 0) {
		// "use on tile" — use an item at the target position (levers, switches, etc.)
		auto tile = g_game().map.getTile(wp);
		if (tile) {
			// Explicit-target form: extra_data "tile_item:ID[,ID...]" — use the tile's
			// ground/stacked item whose id is in the set. Needed for harvest nodes like
			// Oramond juicy roots (21104,21105) whose Action is registered by item id, so
			// they carry no actionId/forceUse to detect generically. Surgical: only
			// waypoints carrying this prefix take this path; the generic path below is
			// untouched.
			static const std::string kTileItemPrefix = "tile_item:";
			if (waypoint.extraData.rfind(kTileItemPrefix, 0) == 0) {
				std::unordered_set<uint16_t> targetIds;
				const std::string ids = waypoint.extraData.substr(kTileItemPrefix.size());
				size_t start = 0;
				while (start <= ids.size()) {
					size_t comma = ids.find(',', start);
					std::string tok = ids.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
					try {
						if (!tok.empty()) targetIds.insert(static_cast<uint16_t>(std::stoi(tok)));
					} catch (...) { }
					if (comma == std::string::npos) break;
					start = comma + 1;
				}
				std::shared_ptr<Item> useItem = nullptr;
				// Ground first (harvest nodes are typically ground-layer), then stacked.
				if (auto ground = tile->getGround(); ground && targetIds.count(ground->getID()) > 0) {
					useItem = ground;
				}
				if (!useItem) {
					if (auto items = tile->getItemList()) {
						for (const auto& item : *items) {
							if (targetIds.count(item->getID()) > 0) { useItem = item; break; }
						}
					}
				}
				if (useItem) {
					g_actions().useItem(player, wp, 0, useItem, false);
					castLog(bot, fmt::format("ACTION: Used tile item {} at ({},{},{})", useItem->getID(), wp.x, wp.y, wp.z));
				} else {
					castLog(bot, fmt::format("ACTION: No target tile item found at ({},{},{})", wp.x, wp.y, wp.z));
				}
			} else {
				// Generic "use on tile" — prefer items with actionId (levers, quest items)
				// over furniture/decoration.
				std::shared_ptr<Item> useItem = nullptr;
				std::shared_ptr<Item> actionIdItem = nullptr;
				if (auto items = tile->getItemList()) {
					for (const auto& item : *items) {
						if (item->getID() < 100) continue;
						if (!useItem) useItem = item;
						if (item->getAttribute<uint16_t>(ItemAttribute_t::ACTIONID) > 0 && !actionIdItem) actionIdItem = item;
					}
				}
				if (actionIdItem) useItem = actionIdItem;
				if (useItem) {
					g_actions().useItem(player, wp, 0, useItem, false);
					castLog(bot, fmt::format("ACTION: Used tile item {} at ({},{},{})", useItem->getID(), wp.x, wp.y, wp.z));
				} else {
					castLog(bot, fmt::format("ACTION: No useable item found at ({},{},{})", wp.x, wp.y, wp.z));
				}
			}
		}
	} else if (waypoint.type == WaypointType::LADDER) {
		// Use findLadderItem to find the actual ladder by known item IDs
		auto useItem = findLadderItem(wp);
		if (useItem) {
			g_actions().useItem(player, wp, 0, useItem, false);
			castLog(bot, fmt::format("ACTION: Used ladder item {} at ({},{},{})", useItem->getID(), wp.x, wp.y, wp.z));
		} else {
			castLog(bot, fmt::format("ACTION: No ladder found at ({},{},{})", wp.x, wp.y, wp.z));
		}
	} else if (waypoint.type == WaypointType::ROPE) {
		// Use rope item (like use_with, but with the rope item id) — pulls bot UP through hole
		auto tempRope = Item::CreateItem(ROPE_ITEM_ID, 1);
		if (tempRope) {
			g_actions().useItemEx(player, bot.currentPos, wp, 0, tempRope, false);
			castLog(bot, fmt::format("ACTION: Used rope at ({},{},{})", wp.x, wp.y, wp.z));
		}
	} else if (waypoint.type == WaypointType::DOOR) {
		tryOpenDoorAt(bot, player, wp);
		castLog(bot, fmt::format("ACTION: Opened door at ({},{},{})", wp.x, wp.y, wp.z));
	} else if (waypoint.type == WaypointType::NPC_INTERACT) {
		// Greet, then stand there like a player reading the NPC's reply. Same 3-10s dwell the
		// boat/carpet captains use (bot_travel.cpp), so an NPC stop reads the same way whether the
		// route is a quest walkthrough or a ferry ride.
		//
		// The hold is what makes this waypoint type a stop rather than a drive-by; followWaypoints
		// merges it with its own 500ms action pause via std::max, so ordering between the two is
		// not load-bearing here.
		const char* greeting = (uniform_random(0, 1) == 0) ? "hi" : "hello";
		g_game().internalCreatureSay(player, TALKTYPE_SAY, greeting, false);
		const int32_t waitMs = uniform_random(3, 10) * 1000;
		auto& pauseUntil = s_actionWpPauseUntil[bot.guid];
		pauseUntil = std::max(pauseUntil, OTSYS_TIME() + waitMs);
		castLog(bot, fmt::format("ACTION: Said '{}' at NPC waypoint ({},{},{}), waiting {}s",
			greeting, wp.x, wp.y, wp.z, waitMs / 1000));
		return true;
	}
	// No marker fired. Every branch above is a non-NODE/STAND waypoint type, so the caller's
	// `didAction || type != NODE && type != STAND` gate is already true for them via the right
	// operand — this return only matters for the marker case handled at the top.
	return false;
}

std::vector<ZTransition> BotEngine::findZTransitions(const Position& center, int32_t radius, bool goDown) {
	std::vector<ZTransition> results;

	for (int32_t dx = -radius; dx <= radius; dx++) {
		for (int32_t dy = -radius; dy <= radius; dy++) {
			Position checkPos(center.x + dx, center.y + dy, center.z);
			auto tile = g_game().map.getTile(checkPos);
			if (!tile) continue;

			int32_t d = std::abs(dx) + std::abs(dy);

			if (goDown && tile->hasFlag(TILESTATE_FLOORCHANGE_DOWN)) {
				results.push_back({checkPos, "stairs", d});
			} else if (!goDown) {
				if (tile->hasFlag(TILESTATE_FLOORCHANGE) && !tile->hasFlag(TILESTATE_FLOORCHANGE_DOWN)) {
					// Standard UP transitions (stairs with NORTH/SOUTH/EAST/WEST floorchange)
					results.push_back({checkPos, "stairs", d});
				} else if (findLadderItem(checkPos)) {
					// Ladder item found — can USE to go UP (works with or without FLOORCHANGE_DOWN)
					results.push_back({checkPos, "ladder", d});
				}
			}

			// Item scan: sewer grates (going DOWN) are USE items without tile flags
			if (goDown && !tile->hasFlag(TILESTATE_FLOORCHANGE_DOWN)) {
				if (findSewerItem(checkPos)) {
					results.push_back({checkPos, "sewer", d});
				}
			}

			// Shovel holes (going DOWN) — stone piles without FLOORCHANGE flags
			if (goDown && !tile->hasFlag(TILESTATE_FLOORCHANGE_DOWN)) {
				if (isShovelHole(checkPos)) {
					results.push_back({checkPos, "shovel", d});
				}
			}

			// Rope spots (going UP) — underground holes without FLOORCHANGE flags
			if (!goDown) {
				if (isRopeSpot(checkPos)) {
					results.push_back({checkPos, "rope", d});
				}
			}
		}
	}

	// Sort by distance
	std::sort(results.begin(), results.end(), [](const ZTransition& a, const ZTransition& b) {
		return a.dist < b.dist;
	});

	return results;
}

Position BotEngine::computeFloorChangeDest(const Position& fcPos, bool goDown) {
	int32_t destX = fcPos.x, destY = fcPos.y;
	uint8_t destZ = goDown ? fcPos.z + 1 : fcPos.z - 1;

	if (goDown) {
		auto belowTile = g_game().map.getTile(Position(fcPos.x, fcPos.y, fcPos.z + 1));
		if (belowTile) {
			if (belowTile->hasFlag(TILESTATE_FLOORCHANGE_NORTH)) destY++;
			if (belowTile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH)) destY--;
			if (belowTile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) destY -= 2;
			if (belowTile->hasFlag(TILESTATE_FLOORCHANGE_EAST)) destX--;
			if (belowTile->hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) destX -= 2;
			if (belowTile->hasFlag(TILESTATE_FLOORCHANGE_WEST)) destX++;
		}
	} else {
		auto tile = g_game().map.getTile(fcPos);
		if (tile) {
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_NORTH)) destY--;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH)) destY++;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) destY += 2;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_EAST)) destX++;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) destX += 2;
			if (tile->hasFlag(TILESTATE_FLOORCHANGE_WEST)) destX--;
		}
	}

	return Position(destX, destY, destZ);
}

void BotEngine::handleFloorChange(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) { resetFloorChange(bot); return; }

	// Timeout. Planned hops (TRUE MULTI-FLOOR) get a larger budget: their
	// WALKING_TO leg may legitimately span up to Z_LEG_MAX tiles, which does
	// not fit in the legacy 15s. On expiry the planned portal is quarantined
	// so the next plan routes around it.
	{
		auto plannedIt = s_plannedFc.find(bot.guid);
		const int64_t budgetMs = (plannedIt != s_plannedFc.end()) ? 60 * 1000 : FC_TIMEOUT * 1000;
		if (OTSYS_TIME() - bot.fcStartTime > budgetMs) {
			if (plannedIt != s_plannedFc.end()) {
				// A 60s no-progress budget expiring is usually TRANSIENT (a creature parked on the
				// leg, a slow door, momentary tile contention) — not proof the portal is bad. Give
				// it the same 3-attempt allowance the approach failure path uses before quarantining,
				// otherwise one unlucky tick deletes the only route between two areas for 10 minutes.
				const ZTransition trans { plannedIt->second.portal.pos, "timeout", 0 };
				if (fcGiveUpOnPlannedTrans(bot, trans, 3, "timeout")) {
					bot.fcStartTime = OTSYS_TIME(); // retry branch: restart the budget, keep the plan
					return;
				}
				s_zHopFail++;
			}
			s_fcConsecutiveFailures[bot.guid]++;
			resetFloorChange(bot);
			return;
		}
	}

	switch (bot.fcState) {
		case FloorChangeState::SCANNING: {
			auto transitions = findZTransitions(bot.currentPos, FC_SCAN_RADIUS, bot.fcGoDown);

			// Search around where target was last seen on our z-level (most accurate for finding
			// the transition they actually used, since target has already changed floors)
			auto lastSameZIt = s_targetLastSameZPos.find(bot.guid);
			if (lastSameZIt != s_targetLastSameZPos.end()) {
				auto lastZTrans = findZTransitions(
					Position(lastSameZIt->second.x, lastSameZIt->second.y, bot.currentPos.z),
					FC_SCAN_RADIUS, bot.fcGoDown);
				for (auto& t : lastZTrans) {
					bool found = false;
					for (const auto& existing : transitions) {
						if (existing.pos == t.pos) { found = true; break; }
					}
					if (!found) transitions.push_back(std::move(t));
				}
			}

			// Also search around target's current projected position
			if (bot.fcTargetPos.x > 0) {
				auto targetTrans = findZTransitions(
					Position(bot.fcTargetPos.x, bot.fcTargetPos.y, bot.currentPos.z),
					FC_SCAN_RADIUS, bot.fcGoDown);
				for (auto& t : targetTrans) {
					// Dedup and add
					bool found = false;
					for (const auto& existing : transitions) {
						if (existing.pos == t.pos) { found = true; break; }
					}
					if (!found) transitions.push_back(std::move(t));
				}
			}

			// Sort by distance from bot
			std::sort(transitions.begin(), transitions.end(), [&](const ZTransition& a, const ZTransition& b) {
				int32_t da = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(a.pos.x)) +
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(a.pos.y));
				int32_t db = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(b.pos.x)) +
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(b.pos.y));
				return da < db;
			});

			// TRUE MULTI-FLOOR: the globally planned hop goes FIRST — the greedy
			// nearest-transition order stays behind it as fallback. This also
			// rescues the "no transitions within 12 tiles" case: the planned
			// portal is injected even when the local scan found nothing.
			{
				auto plannedIt = s_plannedFc.find(bot.guid);
				if (plannedIt != s_plannedFc.end() && plannedIt->second.portal.pos.z == bot.currentPos.z) {
					const auto& hop = plannedIt->second.portal;
					const char* machineType = "stairs"; // walk-on default (stairs/holes/teleports)
					switch (hop.kind) {
						case botnav::ZPortalKind::LADDER:
							machineType = "ladder";
							break;
						case botnav::ZPortalKind::ROPE_SPOT:
							machineType = "rope";
							break;
						case botnav::ZPortalKind::SEWER:
							machineType = "sewer";
							break;
						case botnav::ZPortalKind::SHOVEL_HOLE:
							machineType = "shovel";
							break;
						default:
							break;
					}
					std::erase_if(transitions, [&](const ZTransition& t) { return t.pos == hop.pos; });
					transitions.insert(transitions.begin(), ZTransition { hop.pos, machineType, 0 });
					castLog(bot, fmt::format("FC_SCAN: planned hop injected first — {} at ({},{},{})",
						machineType, hop.pos.x, hop.pos.y, hop.pos.z));
				}
			}

			// Debug: log all found transitions
			{
				std::string searchCenters = fmt::format("bot=({},{},{})", bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
				if (lastSameZIt != s_targetLastSameZPos.end()) {
					searchCenters += fmt::format(" lastSameZ=({},{},{})", lastSameZIt->second.x, lastSameZIt->second.y, lastSameZIt->second.z);
				}
				if (bot.fcTargetPos.x > 0) {
					searchCenters += fmt::format(" target=({},{},{})", bot.fcTargetPos.x, bot.fcTargetPos.y, bot.fcTargetPos.z);
				}
				castLog(bot, fmt::format("FC_SCAN: {} {} — found {} transitions, centers: {}",
					bot.fcGoDown ? "DOWN" : "UP", fmt::format("radius={}", FC_SCAN_RADIUS),
					transitions.size(), searchCenters));
				for (size_t i = 0; i < transitions.size() && i < 5; i++) {
					auto& t = transitions[i];
					int32_t d = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(t.pos.x)) +
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(t.pos.y));
					castLog(bot, fmt::format("  FC_TRANS[{}]: type={} pos=({},{},{}) botDist={}",
						i, t.type, t.pos.x, t.pos.y, t.pos.z, d));
				}
			}

			if (transitions.empty()) {
				s_fcConsecutiveFailures[bot.guid]++;
				if (s_fcConsecutiveFailures[bot.guid] >= 5) {
					castLog(bot, "FC_SCAN: 5 consecutive failures, pausing FC until reroll");
				} else {
					castLog(bot, "FC_SCAN: No transitions found — resetting");
				}
				resetFloorChange(bot);
				return;
			}

			bot.fcTransitions = std::move(transitions);
			bot.fcTransIdx = 0;
			bot.fcState = FloorChangeState::WALKING_TO;
			castLog(bot, fmt::format("FC_SCAN: Selected {} transitions, moving to WALKING_TO",
				bot.fcTransitions.size()));
			break;
		}

		case FloorChangeState::WALKING_TO: {
			if (!player->listWalkDir.empty()) return;

			// PLANNED HOP: walk it as ordinary waypoints (doors + the portal action) through the
			// same followWaypoints machinery that walks authored routes, instead of the greedy
			// transition scan below. This is what gives multi-floor hops door handling — the
			// legacy path has none beyond a 1-3 tile peek, which is why bots stalled on the Thais
			// castle doors 7-15 tiles out.
			{
				auto plannedIt = s_plannedFc.find(bot.guid);
				if (plannedIt != s_plannedFc.end()) {
					const auto& hop = plannedIt->second.portal;

					// Did the transition already happen? Either a USE action fired last tick and
					// this tick's position sync just picked it up, or the bot auto-walked onto a
					// walk-on FC tile (the server changes z on step-in; we only see it after).
					// This MUST be checked before re-entering followWaypoints: its own walk-on-FC
					// arrival test requires currentPos.z to already differ, so calling it again
					// here would never match and we would fall through to the legacy scan on the
					// wrong floor. VERIFYING is the only place success is judged.
					if (bot.currentPos.z != hop.pos.z) {
						s_zLegPlan.erase(bot.guid);
						bot.fcState = FloorChangeState::VERIFYING;
						return;
					}

					bot.fcPreZ = bot.currentPos.z; // stamped while still on the source floor
					auto& leg = ensureZLegPlan(bot, bot.currentPos, hop);
					WaypointFollowConfig cfg;
					cfg.logPrefix = "ZLEG";
					cfg.globalTimeoutMs = 60000;
					cfg.perWpStuckMs = 15000;
					cfg.zChangeGraceMs = 500;
					auto res = followWaypoints(bot, leg.wps, leg.idx, leg.skipCount, cfg);
					if (res.aborted) {
						// An aborted leg is usually transient — the 60s budget is keyed on waypoint
						// PROGRESS, so a compound obstruction (e.g. a second door the one-shot
						// zFindDoorsOnPath never saw) can burn it while the bot is still doing
						// legitimate work. Allow 3 attempts before quarantining; on the retry branch
						// drop the leg plan only, keeping s_plannedFc so the next tick rebuilds the
						// leg fresh via ensureZLegPlan and retries the SAME planned hop.
						const ZTransition trans { hop.pos, "zleg", 0 };
						s_zLegPlan.erase(bot.guid);
						if (fcGiveUpOnPlannedTrans(bot, trans, 3, "zleg")) {
							// `trans`, NOT `hop`: hop is a reference INTO s_plannedFc
							// (const auto& hop = plannedIt->second.portal above), and the give-up
							// branch of fcGiveUpOnPlannedTrans erases that entry — reading hop here
							// is a use-after-free. It crashed the server with SIGSEGV once.
							castLog(bot, fmt::format("ZLEG: leg aborted at ({},{},{})",
								trans.pos.x, trans.pos.y, trans.pos.z));
							resetFloorChange(bot);
							return;
						}
						zBlacklistPortal(hop.pos, "zleg");
						s_plannedFc.erase(plannedIt);
						s_zHopFail++;
						resetFloorChange(bot);
						return;
					}
					// Terminal waypoint consumed this tick (the USE action fired). Hand off
					// immediately rather than waiting for a later call to report !inProgress —
					// followWaypoints returns early on the action tick with inProgress still true.
					if (leg.idx >= leg.wps.size()) {
						s_zLegPlan.erase(bot.guid);
						bot.fcState = FloorChangeState::VERIFYING;
					}
					return;
				}
			}

			if (bot.fcTransIdx >= bot.fcTransitions.size()) {
				s_fcConsecutiveFailures[bot.guid]++;
				if (s_fcConsecutiveFailures[bot.guid] >= 5) {
					castLog(bot, "FC_WALK: 5 consecutive failures (all transitions exhausted), pausing FC until reroll");
				}
				resetFloorChange(bot);
				return;
			}

			auto& trans = bot.fcTransitions[bot.fcTransIdx];
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(trans.pos.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(trans.pos.y)));

			// Skip transitions that are too far for A* to reach — EXCEPT the
			// planned hop, which is walked with the chunked goTo (TRUE
			// MULTI-FLOOR: legs up to Z_LEG_MAX are legitimate; goTo's
			// CHUNK_DIST sub-pathing keeps each A* inside its node budget).
			if (dist > PATH_MAX_DIST) {
				auto plannedIt = s_plannedFc.find(bot.guid);
				if (plannedIt != s_plannedFc.end() && plannedIt->second.portal.pos == trans.pos) {
					Position legTarget(trans.pos.x, trans.pos.y, bot.currentPos.z);
					if (goTo(bot, legTarget, 1)) {
						s_zLegWalks++;
						return; // walking the leg; re-evaluate next tick
					}
					// Leg unwalkable — drop the plan, quarantine the portal.
					zBlacklistPortal(trans.pos, "legacy_farleg");
					s_plannedFc.erase(bot.guid);
					s_zHopFail++;
				}
				bot.fcTransIdx++;
				if (bot.fcTransIdx >= bot.fcTransitions.size()) {
					s_fcConsecutiveFailures[bot.guid]++;
					resetFloorChange(bot);
				}
				return;
			}

			if (dist <= 1) {
				// Adjacent or on tile — try stepping on / using
				s_fcBlockerRetries[bot.guid] = 0;
				bot.fcState = FloorChangeState::STEPPING_ON;
				return;
			}

			// Path to within 1 tile
			FindPathParams fpp;
			fpp.fullPathSearch = true;
			fpp.clearSight = false;
			fpp.allowDiagonal = true;
			fpp.keepDistance = false;
			fpp.maxSearchDist = PATH_MAX_DIST;
			fpp.minTargetDist = 1;
			fpp.maxTargetDist = 1;

			std::vector<Direction> dirList;
			if (g_game().map.getPathMatching(player, trans.pos, dirList, FrozenPathingConditionCall(trans.pos), fpp)) {
				s_fcBlockerRetries[bot.guid] = 0;
				botStartAutoWalk(bot, player,dirList);
			} else {
				// Pathfinding failed — try door first, then blocker with retry limit
				if (tryOpenDoors(bot, player, trans.pos)) {
					// Door opened toward stairs — retry pathfinding next tick
				} else if (tryPathToHuntDoor(bot, player)) {
					// Walking toward a hunt DOOR waypoint — will open when adjacent
				} else if (tryAttackBlockingMonster(bot)) {
					// Blocker found — retry with limit
					s_fcBlockerRetries[bot.guid]++;
					if (s_fcBlockerRetries[bot.guid] >= 15) {
						castLog(bot, "FC_WALK: Blocker retry limit reached, trying next transition");
						s_fcBlockerRetries[bot.guid] = 0;
						// 15 attempts is already a generous budget, so a planned portal gives up
						// immediately here rather than getting a second allowance.
						if (!fcGiveUpOnPlannedTrans(bot, trans, /*maxRetries=*/0)) {
							bot.fcTransIdx++;
							if (bot.fcTransIdx >= bot.fcTransitions.size()) {
								s_fcConsecutiveFailures[bot.guid]++;
								resetFloorChange(bot);
							}
						}
					}
				} else {
					// No door, no blocker — try next transition.
					//
					// THIS is the branch that broke multi-floor navigation: it was the only
					// fcTransIdx++ site that logged nothing, so a planned portal was silently
					// swapped for a nearer one and the bot changed floor in the wrong place.
					// Unlike the blocker path it had no retry budget at all and gave up on the
					// first tick, so a single transient obstruction (another bot standing on the
					// path) was enough. Planned portals now get 3 attempts before quarantine.
					if (!fcGiveUpOnPlannedTrans(bot, trans, /*maxRetries=*/3)) {
						bot.fcTransIdx++;
						if (bot.fcTransIdx >= bot.fcTransitions.size()) {
							s_fcConsecutiveFailures[bot.guid]++;
							resetFloorChange(bot);
						}
					}
				}
			}
			break;
		}

		case FloorChangeState::STEPPING_ON: {
			if (!player->listWalkDir.empty()) return;

			if (bot.fcTransIdx >= bot.fcTransitions.size()) {
				resetFloorChange(bot);
				return;
			}

			auto& trans = bot.fcTransitions[bot.fcTransIdx];
			int32_t dist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(trans.pos.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(trans.pos.y)));

			castLog(bot, fmt::format("FC_STEP: type={} pos=({},{},{}) dist={} attempt={} botPos=({},{},{})",
				trans.type, trans.pos.x, trans.pos.y, trans.pos.z, dist, bot.fcAttempts + 1,
				bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));

			bot.fcPreZ = bot.currentPos.z;
			bot.fcAttempts++;

			if (dist == 0) {
				// Already on tile — check if USE item (sewer/ladder) that needs action
				if (bot.fcGoDown) {
					auto sewerItem = findSewerItem(trans.pos);
					if (sewerItem) {
						g_actions().useItem(player, trans.pos, 0, sewerItem, false);
						castLog(bot, fmt::format("FC: Used sewer {} at ({},{},{})",
							sewerItem->getID(), trans.pos.x, trans.pos.y, trans.pos.z));
						bot.fcState = FloorChangeState::VERIFYING;
						return;
					}
					// Shovel at dist=0: can't use while standing on it — move away first
					if (trans.type == "shovel" || isShovelHole(trans.pos)) {
						// Try cardinal directions to find a walkable adjacent tile
						static const Direction dirs[] = { DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST };
						bool moved = false;
						for (auto dir : dirs) {
							Position adjPos = bot.currentPos;
							switch (dir) {
								case DIRECTION_NORTH: adjPos.y--; break;
								case DIRECTION_EAST:  adjPos.x++; break;
								case DIRECTION_SOUTH: adjPos.y++; break;
								case DIRECTION_WEST:  adjPos.x--; break;
								default: break;
							}
							auto adjTile = g_game().map.getTile(adjPos);
							if (adjTile && !adjTile->hasFlag(TILESTATE_FLOORCHANGE) && adjTile->queryAdd(0, player, 1, FLAG_PATHFINDING) == RETURNVALUE_NOERROR) {
								g_game().internalMoveCreature(player, dir, FLAG_NOLIMIT);
								castLog(bot, fmt::format("FC: On shovel hole, moving away {} to use from dist=1", dir));
								moved = true;
								break;
							}
						}
						if (!moved) {
							castLog(bot, "FC: On shovel hole, can't move away — skipping transition");
							if (!fcGiveUpOnPlannedTrans(bot, trans, /*maxRetries=*/3)) {
								bot.fcTransIdx++;
								if (bot.fcTransIdx >= bot.fcTransitions.size()) {
									resetFloorChange(bot);
								}
							}
						}
						// Stay in STEPPING_ON — next tick at dist=1, shovel handler fires
						return;
					}
				} else {
					auto ladderItem = findLadderItem(trans.pos);
					if (ladderItem) {
						g_actions().useItem(player, trans.pos, 0, ladderItem, false);
						castLog(bot, fmt::format("FC: Used ladder {} at ({},{},{})",
							ladderItem->getID(), trans.pos.x, trans.pos.y, trans.pos.z));
						bot.fcState = FloorChangeState::VERIFYING;
						return;
					}
					// Rope at dist=0 — use rope action from current position
					if (trans.type == "rope" || isRopeSpot(trans.pos)) {
						auto tempRope = Item::CreateItem(ROPE_ITEM_ID, 1);
						if (tempRope) {
							g_actions().useItemEx(player, bot.currentPos, trans.pos, 0, tempRope, false);
							castLog(bot, fmt::format("FC: Used rope at ({},{},{})",
								trans.pos.x, trans.pos.y, trans.pos.z));
						}
						bot.fcState = FloorChangeState::VERIFYING;
						return;
					}
				}
				// Step-on tile but z didn't change — go to VERIFYING to retry or try next
				bot.fcState = FloorChangeState::VERIFYING;
				return;
			}

			if (dist == 1) {
				// Check for USE items: ladders (UP) and sewers (DOWN)
				auto tileUseItem = findLadderItem(trans.pos);
				bool isLadder = tileUseItem != nullptr;
				bool isSewer = false;
				if (!isLadder && bot.fcGoDown) {
					tileUseItem = findSewerItem(trans.pos);
					isSewer = tileUseItem != nullptr;
				}

				// Check for shovel holes and rope spots
				bool isShovelHoleTile = (!isLadder && !isSewer && bot.fcGoDown &&
					(trans.type == "shovel" || isShovelHole(trans.pos)));
				bool isRopeSpotTile = (!isLadder && !bot.fcGoDown &&
					(trans.type == "rope" || isRopeSpot(trans.pos)));

				if ((!bot.fcGoDown && isLadder) || (bot.fcGoDown && isSewer)) {
					// Ladder UP or sewer DOWN: use item action
					g_actions().useItem(player, trans.pos, 0, tileUseItem, false);
					castLog(bot, fmt::format("FC: Used {} {} at ({},{},{})",
						isLadder ? "ladder" : "sewer",
						tileUseItem->getID(), trans.pos.x, trans.pos.y, trans.pos.z));
					bot.fcState = FloorChangeState::VERIFYING;
				} else if (isShovelHoleTile) {
					// Shovel: use FROM adjacent position ON the stone pile (don't step on it)
					auto tempShovel = Item::CreateItem(SHOVEL_ITEM_ID, 1);
					if (tempShovel) {
						g_actions().useItemEx(player, bot.currentPos, trans.pos, 0, tempShovel, false);
						castLog(bot, fmt::format("FC: Used shovel at ({},{},{}) from ({},{},{})",
							trans.pos.x, trans.pos.y, trans.pos.z,
							bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
					}
					bot.fcState = FloorChangeState::VERIFYING;
				} else if (isRopeSpotTile) {
					// Rope: step onto the rope spot first, next tick at dist=0 will use rope
					int32_t dx = static_cast<int32_t>(trans.pos.x) - static_cast<int32_t>(bot.currentPos.x);
					int32_t dy = static_cast<int32_t>(trans.pos.y) - static_cast<int32_t>(bot.currentPos.y);
					Direction dir;
					if (dx == 0 && dy == -1) dir = DIRECTION_NORTH;
					else if (dx == 1 && dy == 0) dir = DIRECTION_EAST;
					else if (dx == 0 && dy == 1) dir = DIRECTION_SOUTH;
					else if (dx == -1 && dy == 0) dir = DIRECTION_WEST;
					else if (dx == 1 && dy == -1) dir = DIRECTION_NORTHEAST;
					else if (dx == 1 && dy == 1) dir = DIRECTION_SOUTHEAST;
					else if (dx == -1 && dy == 1) dir = DIRECTION_SOUTHWEST;
					else dir = DIRECTION_NORTHWEST;

					g_game().internalMoveCreature(player, dir, FLAG_NOLIMIT);
					castLog(bot, fmt::format("FC: Stepping onto rope spot at ({},{},{})",
						trans.pos.x, trans.pos.y, trans.pos.z));
					// Stay in STEPPING_ON — next tick at dist=0, rope handler fires
				} else {
					// Stairs up/down, holes, trapdoors: step onto the tile
					int32_t dx = static_cast<int32_t>(trans.pos.x) - static_cast<int32_t>(bot.currentPos.x);
					int32_t dy = static_cast<int32_t>(trans.pos.y) - static_cast<int32_t>(bot.currentPos.y);
					Direction dir;
					if (dx == 0 && dy == -1) dir = DIRECTION_NORTH;
					else if (dx == 1 && dy == 0) dir = DIRECTION_EAST;
					else if (dx == 0 && dy == 1) dir = DIRECTION_SOUTH;
					else if (dx == -1 && dy == 0) dir = DIRECTION_WEST;
					else if (dx == 1 && dy == -1) dir = DIRECTION_NORTHEAST;
					else if (dx == 1 && dy == 1) dir = DIRECTION_SOUTHEAST;
					else if (dx == -1 && dy == 1) dir = DIRECTION_SOUTHWEST;
					else dir = DIRECTION_NORTHWEST;

					g_game().internalMoveCreature(player, dir, FLAG_NOLIMIT);
					bot.fcState = FloorChangeState::VERIFYING;
				}
			} else {
				bot.fcState = FloorChangeState::WALKING_TO;
			}
			break;
		}

		case FloorChangeState::VERIFYING: {
			// Update position
			bot.currentPos = player->getPosition();

			if (bot.currentPos.z != bot.fcPreZ) {
				// TRUE MULTI-FLOOR: a planned hop verifies against its PLANNED
				// landing floor, not the toward-target heuristic below — a
				// multi-hop route may legitimately step AWAY from the target
				// floor (up-first-then-down through another wing).
				auto plannedIt = s_plannedFc.find(bot.guid);
				const bool traversedPlanned = plannedIt != s_plannedFc.end()
					&& bot.fcTransIdx < bot.fcTransitions.size()
					&& bot.fcTransitions[bot.fcTransIdx].pos == plannedIt->second.portal.pos
					&& bot.fcPreZ == plannedIt->second.portal.pos.z;
				if (traversedPlanned) {
					const uint8_t expectedZ = plannedIt->second.portal.landing.z;
					if (bot.currentPos.z == expectedZ) {
						castLog(bot, fmt::format("FC_VERIFY: PLANNED HOP OK z={}->{} at ({},{},{})",
							bot.fcPreZ, bot.currentPos.z, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
						s_zHopOk++;
						// Remember it so the next plan cannot immediately send us back through
						// the same portal (the z7<->z6 ping-pong on one staircase).
						s_zLastPortalUsed[bot.guid] = {
							botTileKey(plannedIt->second.portal.pos),
							OTSYS_TIME() + Z_LAST_PORTAL_GUARD_MS
						};
						bot.huntIgnoredMonsters.clear();
						s_fcConsecutiveFailures.erase(bot.guid);
						s_lastFcPositions[bot.guid] = {
							Position(bot.fcTargetPos.x, bot.fcTargetPos.y, bot.fcPreZ),
							bot.currentPos
						};
						s_plannedFc.erase(plannedIt);
						resetFloorChange(bot);
						break;
					}
					// Landed on an unexpected floor — the portal is not what the
					// graph thought it was. Quarantine it and replan next tick.
					castLog(bot, fmt::format("FC_VERIFY: PLANNED HOP WRONG-Z z={}->{} expected {} — blacklisting portal",
						bot.fcPreZ, bot.currentPos.z, expectedZ));
					zBlacklistPortal(plannedIt->second.portal.pos, "verify_wrongz");
					s_plannedFc.erase(plannedIt);
					s_zHopFail++;
					s_fcConsecutiveFailures[bot.guid]++;
					resetFloorChange(bot);
					break;
				}
				// A z-change happened. Before accepting it as success, verify it moved us
				// TOWARD the target floor. A wrong-direction hop (e.g. stepping a reverse
				// staircase) must NOT count as progress, else the depot cross-z walk
				// ping-pongs up/down the same stairwell — the user-reported "up and down
				// the stairs" at multi-floor depots. The gate applies only when fcPreZ is
				// valid (set in STEPPING_ON, before any internalMoveCreature) AND the target
				// is on a different floor than the one we stepped from; otherwise fall back
				// to the legacy any-z-change=success semantics (e.g. PK/PvP z-pursuit of a
				// mobile target whose precise floor we don't track here).
				const bool wentDown = (bot.currentPos.z > bot.fcPreZ);
				const bool targetFloorKnown = (bot.fcPreZ != 0 && bot.fcTargetPos.z != bot.fcPreZ);
				if (targetFloorKnown && wentDown != (bot.fcTargetPos.z > bot.fcPreZ)) {
					// Wrong direction — do NOT erase the failure counter; accrue it so the
					// >=5 startFloorChange pause + the depot retry/blacklist guards take over
					// instead of looping. Reset so the handler re-derives a fresh,
					// correct-direction FC from the bot's new floor next tick (fcTransitions
					// here were scanned for the old floor and are now stale).
					castLog(bot, fmt::format("FC_VERIFY: WRONG-DIR z={}->{} target.z={} — not progress, resetting",
						bot.fcPreZ, bot.currentPos.z, bot.fcTargetPos.z));
					s_fcConsecutiveFailures[bot.guid]++;
					resetFloorChange(bot);
					break;
				}
				// Success — clear ignored monsters since reachability
				// changes completely on a different z-level
				castLog(bot, fmt::format("FC_VERIFY: SUCCESS z={}->{} at ({},{},{})",
					bot.fcPreZ, bot.currentPos.z, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
				bot.huntIgnoredMonsters.clear();
				s_fcConsecutiveFailures.erase(bot.guid);
				// Save FC positions for z-mismatch recovery (walk back if route fails)
				s_lastFcPositions[bot.guid] = {
					Position(bot.fcTargetPos.x, bot.fcTargetPos.y, bot.fcPreZ),  // pre-FC position (old z)
					bot.currentPos  // post-FC position (new z)
				};
				resetFloorChange(bot);
			} else {
				castLog(bot, fmt::format("FC_VERIFY: FAIL z still {} after attempt {}/3 at ({},{},{})",
					bot.currentPos.z, bot.fcAttempts,
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
				if (bot.fcAttempts < 3) {
					bot.fcState = FloorChangeState::STEPPING_ON;
				} else {
					// Three STEPPING_ON attempts already used the item and the z did not change —
					// that is a real "this portal does not work" signal, so a planned portal is
					// quarantined immediately rather than given a further allowance.
					// `trans` is not in scope in this case block; index defensively.
					const bool idxValid = bot.fcTransIdx < bot.fcTransitions.size();
					if (!idxValid || !fcGiveUpOnPlannedTrans(bot, bot.fcTransitions[bot.fcTransIdx], /*maxRetries=*/0)) {
						// Try next transition
						bot.fcTransIdx++;
						bot.fcAttempts = 0;
						if (bot.fcTransIdx >= bot.fcTransitions.size()) {
							s_fcConsecutiveFailures[bot.guid]++;
							castLog(bot, "FC_VERIFY: All transitions exhausted — resetting");
							resetFloorChange(bot);
						} else {
							castLog(bot, fmt::format("FC_VERIFY: Trying next transition idx={}",
								bot.fcTransIdx));
							bot.fcState = FloorChangeState::WALKING_TO;
						}
					}
				}
			}
			break;
		}

		default:
			resetFloorChange(bot);
			break;
	}
}

