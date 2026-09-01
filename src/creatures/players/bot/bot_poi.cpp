/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_poi.cpp — point-of-interest selection + dynamic POI generation
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
// POI selection
// ============================================================================

// Per-bot storage for dynamically generated POIs.
// Slots: [0] depot_outside, [1] boat_nearby, [2] npc_visit, [3] fishing, [4] house_visit,
//        [5] reward_shrine, [6] imbuing_shrine.
// SIZE MUST MATCH THE HIGHEST INDEX USED BELOW. This is a raw std::array, so a slot index
// past the end is an out-of-bounds heap write, not a bounds error — silent memory corruption
// that shows up as the candidate simply never winning a roll. Shipped exactly that once.
static std::unordered_map<uint32_t, std::array<BotPOI, 7>> s_dynamicPOIs;

const BotPOI* BotEngine::selectNextPOI(BotState& bot) {
	auto& allPOIs = getCityPOIs();
	auto it = allPOIs.find(bot.townId);
	if (it == allPOIs.end() || it->second.empty()) return nullptr;

	// When PZ-locked, skip POIs in protection zones (depot, temple, boat)
	bool pzLocked = isBotPzLocked(bot);

	// BOT_LIVENESS_PACK Phase A.4: per-POI crowd cap. Skip a candidate POI if there are
	// already N awake bots within R tiles of its position. Cached config (per-tick refresh).
	const int32_t crowdCount = livenessCfg_.poiCrowdCapCount;
	const int32_t crowdRadius = livenessCfg_.poiCrowdCapRadius;
	auto isCrowded = [&](const Position& target) -> bool {
		if (crowdCount <= 0 || crowdRadius <= 0) return false;
		if (isOnAdvStoneIsland(target)) return false;  // exempt
		int32_t hits = 0;
		for (const auto& other : bots_) {
			if (other.guid == bot.guid) continue;
			if (other.hibernated || !other.active) continue;
			if (other.currentPos.z != target.z) continue;
			const int32_t dx = std::abs(static_cast<int32_t>(other.currentPos.x) - static_cast<int32_t>(target.x));
			const int32_t dy = std::abs(static_cast<int32_t>(other.currentPos.y) - static_cast<int32_t>(target.y));
			if (std::max(dx, dy) <= crowdRadius) {
				if (++hits >= crowdCount) return true;
			}
		}
		return false;
	};

	// Player-proximity weighting (2026-06-15): bias toward POIs near a player when the bot is
	// hibernated (or proxAwake). No-op when disabled / no anchors → unchanged per-type balance.
	auto proxAdjust = [&](const Position& pos, int base) -> int {
		if (livenessCfg_.proxEnabled && (bot.hibernated || livenessCfg_.proxAwake) && !currentAnchorPts_.empty())
			return base + proximityBonus(minChebToAnchor(pos));
		return base;
	};

	// Build candidate list from static POIs
	std::vector<const BotPOI*> candidates;
	std::vector<int> weights;

	for (const auto& poi : it->second) {
		int weight = getPOIWeight(poi.type);
		if (weight <= 0) continue;
		if (bot.visitedPOIs.count(poi.name)) continue;
		if (pzLocked && (poi.type == POIType::DEPOT || poi.type == POIType::TEMPLE || poi.type == POIType::BOAT)) continue;
		// AdvStone POIs are trip triggers (not walk-to-tile) — exempt from crowd cap.
		if (poi.type != POIType::ADVENTURER_STONE && isCrowded(poi.pos)) continue;
		candidates.push_back(&poi);
		weights.push_back(proxAdjust(poi.pos, weight));
	}

	auto& dynSlots = s_dynamicPOIs[bot.guid];

	// Add dynamic "depot outside" POI — find non-PZ tile at PZ boundary near depot
	if (!bot.visitedPOIs.count("_depot_outside") && !pzLocked) {
		for (const auto& poi : it->second) {
			if (poi.type != POIType::DEPOT) continue;
			Position boundary = findPZBoundaryTile(poi.pos, 8);
			if (boundary.x > 0) {
				dynSlots[0] = BotPOI { "_depot_outside", boundary, POIType::DEPOT_OUTSIDE };
				candidates.push_back(&dynSlots[0]);
				weights.push_back(proxAdjust(boundary, getPOIWeight(POIType::DEPOT_OUTSIDE)));
			}
			break;
		}
	}

	// Add dynamic "boat nearby" POI — find random tile near boat position
	auto boatPos = getTravelPosition(bot.townId).first;
	if (boatPos.x > 0 && !bot.visitedPOIs.count("_boat_nearby")) {
		Position nearBoat = findRandomTileNear(boatPos, 4);
		if (nearBoat.x > 0) {
			dynSlots[1] = BotPOI { "_boat_nearby", nearBoat, POIType::BOAT };
			candidates.push_back(&dynSlots[1]);
			weights.push_back(proxAdjust(nearBoat, getPOIWeight(POIType::BOAT)));
		}
	}

	std::string npcCandidateName; // set if an NPC slot is offered; committed only if it wins
	bool fishCandidateClaimed = false; // WATER slot offered (and thus claimed) below

	// Add dynamic "visit a random NPC" POI (Phase 8 increment 2, wired up 2026-07-28).
	//
	// Deliberately ONE candidate slot rather than a POI row per NPC. A town holds ~55 anchored
	// NPCs; giving each its own candidate at the NPC weight would make NPC visits ~75% of every
	// pick and swamp depot/temple/boat. As a single slot, botPoiWeightNpc means what an operator
	// expects — a type-level weight comparable to the others, independent of how many NPCs the
	// town happens to have.
	//
	// The destination is the precomputed APPROACH TILE, not the NPC's own tile: that is what lets
	// a bot stand across a counter exactly where a real player would, and it claims the tile so
	// two bots never walk to the same spot. Until this call site existed, npcAnchors_ was built
	// every startup and only ever read by the /cavebot npcapproach debug command.
	// AWAKE bots only. A hibernated bot is advanced by the index-based virtual simulator, which
	// never runs the arrival handler — so it would claim a REAL approach tile for a visit that
	// never physically happens, and with most of the pool hibernated those claims would keep awake
	// bots off counter spots. Measured before this guard: 25 visits started, 0 arrivals, all from
	// hibernated bots. Nothing is lost by skipping them — an unobserved bot's NPC visit is
	// invisible by definition, and it starts doing them the moment it wakes.
	// npcVisitPct gates the ENTIRE block, not just the final weight, so at 0 none of the
	// sampling, anchor lookup or approach-tile reservation below runs at all. The roll uses
	// uniform_random(1,100), whose range starts at 1, so `<= 0` can never be true — disabled
	// means structurally unreachable, not merely improbable. This is the master switch for the
	// scoped route planner: NPC visits are its only automatic consumer.
	const bool npcVisitAllowed = livenessCfg_.npcVisitPct > 0
		&& uniform_random(1, 100) <= livenessCfg_.npcVisitPct;
	if (npcVisitAllowed && !bot.hibernated && !bot.visitedPOIs.count("_npc_visit") && !pzLocked) {
		// Use the town the bot is physically STANDING in, not its home townId. Bots roam, and the
		// two drift: one picked 'Andrew Lyze' 560 tiles away because its home town said 15 while
		// it stood in Thais. Visiting an NPC where you actually are is both achievable and more
		// realistic. Falls back to the home town when the position maps to no anchored town.
		uint32_t npcTown = findNearestTown(bot.currentPos);
		auto nit = npcNamesByTown_.find(npcTown);
		if (nit == npcNamesByTown_.end()) {
			nit = npcNamesByTown_.find(bot.townId);
		}
		if (nit != npcNamesByTown_.end() && !nit->second.empty()) {
			const auto& names = nit->second;
			// Prefer an NPC the bot can plausibly reach on foot: same floor, reasonably close.
			// A uniform pick over the whole town selected NPCs 20-130 tiles away and 1-4 floors
			// up or down; across 11 minutes that produced 5 started visits and 0 arrivals,
			// because those walks need floor changes and mostly time out. Sample a handful of
			// names and take the first that is same-z and near, else fall back to the last tried
			// (which preserves the old behavior rather than skipping the visit entirely).
			// Widened 40/50 -> 150 once the scoped route planner shipped.
			//
			// The old caps existed because a uniform pick over the whole town chose NPCs 20-130
			// tiles away and 1-4 floors up or down, and those walks "mostly time out" — measured
			// 5 started visits, 0 arrivals over 11 minutes. That was the CHUNKED walker failing:
			// it re-aimed at a straight-line interpolated point every 12 tiles and lost any route
			// that doglegged, and it had no door handling. The planner solves the whole route in
			// one search, segments at doors, and decomposes floors through the portal graph — the
			// long cross-floor visit is precisely what it was built for.
			//
			// 150 covers a whole town and stays inside the planner's own reach (PLANNER_LEG_RADIUS
			// and PATH_WIDE_DIST are both 200). The walkTarget budget is raised to 4 min for
			// planner walks to match (bot_tick.cpp) — at ~300ms/tile a 150-tile walk plus floor
			// changes does not fit in the default 2 min.
			constexpr int32_t NPC_VISIT_MAX_DIST = 150;
			// TRUE MULTI-FLOOR: with the portal graph ready, NPCs on OTHER floors are eligible
			// (595/995 NPC placements are not on z=7).
			constexpr int32_t NPC_VISIT_MAX_DIST_XZ = 150;
			constexpr int32_t NPC_VISIT_MAX_DZ = 7;
			// More samples: the eligible set is much larger now, but so is the chance that any
			// individual draw has no resolvable approach anchor.
			constexpr int   NPC_VISIT_SAMPLES  = 12;
			std::string npcName;
			for (int attempt = 0; attempt < NPC_VISIT_SAMPLES; attempt++) {
				const std::string& cand = names[uniform_random(0, static_cast<int32_t>(names.size()) - 1)];
				npcName = cand;
				auto ait = npcAnchors_.find(cand);
				if (ait == npcAnchors_.end() || ait->second.empty()) continue;
				bool nearOk = false;
				for (const auto& a : ait->second) {
					const int32_t d = std::max(
						std::abs(static_cast<int32_t>(a.npcPos.x) - static_cast<int32_t>(bot.currentPos.x)),
						std::abs(static_cast<int32_t>(a.npcPos.y) - static_cast<int32_t>(bot.currentPos.y)));
					if (a.npcPos.z == bot.currentPos.z) {
						if (d <= NPC_VISIT_MAX_DIST) { nearOk = true; break; }
					} else if (zGraphReady_) {
						const int32_t dz = std::abs(static_cast<int32_t>(a.npcPos.z) - static_cast<int32_t>(bot.currentPos.z));
						if (d <= NPC_VISIT_MAX_DIST_XZ && dz <= NPC_VISIT_MAX_DZ) { nearOk = true; break; }
					}
				}
				if (nearOk) break;
			}
			Position approachTile;
			bool isFallback = false;
			if (resolveNpcApproach(bot, npcName, approachTile, isFallback) && !isCrowded(approachTile)) {
				dynSlots[2] = BotPOI { "_npc_visit", approachTile, POIType::NPC };
				candidates.push_back(&dynSlots[2]);
				weights.push_back(proxAdjust(approachTile, getPOIWeight(POIType::NPC)));
				npcCandidateName = npcName; // committed only if the roll below actually picks it
			} else {
				// Either the NPC has no anchors or every approach tile is taken/crowded. Drop the
				// claim so a 45s TTL reservation isn't left behind for a visit that never starts.
				releaseNpcApproach(bot.guid);
			}
		}
	}

	// ---- BOT_SUPPLY_REALISM: fishing trip ----
	// Same shape as the NPC visit above, gated the same way: fishPct gates the ENTIRE block so at
	// 0 nothing is sampled (uniform_random(1,100) starts at 1, so `<= 0` is unreachable), and
	// AWAKE bots only — a hibernated bot is advanced by the index-based virtual simulator, which
	// never runs the arrival handler, so it would win the roll and then never actually fish.
	// Unlike hunting there is NO cap on concurrent fishers and no tile reservation: two bots that
	// pick the same shore are separated by A* treating an occupied tile as blocked, exactly as
	// anywhere else bots walk.
	const bool fishAllowed = livenessCfg_.fishPct > 0
		&& uniform_random(1, 100) <= livenessCfg_.fishPct;
	if (fishAllowed && !bot.hibernated && !bot.visitedPOIs.count("_fishing") && !pzLocked) {
		FishingSpot spot;
		if (selectFishingSpot(bot, spot) && !isCrowded(spot.stand)
		    && !isFishSpotClaimed(spot, bot.guid)) {
			// Claim BOTH tiles now, speculatively — same discipline as resolveNpcApproach's
			// approach-tile claim: offering a candidate reserves it, and the commit/discard
			// below hands it back if a different candidate wins the roll. When everything
			// nearby is already claimed the guard above simply stops the WATER candidate from
			// being offered at all — the bot skips fishing this reroll and tries again on the
			// next one, the same graceful no-op as an NPC whose approach tiles are all taken.
			claimFishingSpot(bot.guid, spot);
			fishCandidateClaimed = true;
			// Remember the exact water/stand pair. Arrival must use THIS, not re-derive from
			// wherever the bot halts — POI_ARRIVAL_DIST is 3, and re-deriving throws away the
			// walkability and line-of-sight vetting the index did. Written here (not only when
			// the roll wins) is harmless: startFishingRun consumes and erases it, and a losing
			// candidate's entry is simply overwritten by the next roll.
			s_pendingFishSpot[bot.guid] = spot;
			dynSlots[3] = BotPOI { "_fishing", spot.stand, POIType::WATER };
			candidates.push_back(&dynSlots[3]);
			weights.push_back(proxAdjust(spot.stand, getPOIWeight(POIType::WATER)));
		}
	}

	// ---- BOT_HOUSE_VISIT: go stand around in a bot-owned house ----
	// Gated exactly like the two above: houseVisitPct gates the ENTIRE block so at 0 nothing is
	// sampled, and AWAKE bots only — the virtual simulator never runs the arrival handler, so a
	// hibernated bot would win the roll, hold a house slot and never actually go.
	//
	// Unlike fishing this one DOES reserve, because a house has a small number of usable spots and
	// a hard occupancy cap: two bots choosing the same chair, or six choosing the same house, is
	// exactly what the claims exist to prevent. The reservation is speculative here and handed back
	// below if a different candidate wins the roll.
	//
	// The name starts with '_' deliberately: doIdle's dynamic-POI branch keys on that to keep
	// hasWalkTarget alive after a city route ends, so the bot walks the last tiles to the actual
	// interior tile instead of stopping at the route's endpoint.
	bool houseCandidateClaimed = false;
	const bool houseAllowed = livenessCfg_.houseVisitPct > 0
		&& uniform_random(1, 100) <= livenessCfg_.houseVisitPct;
	if (houseAllowed && !bot.hibernated && !bot.visitedPOIs.count("_house_visit") && !pzLocked) {
		HouseRun run;
		if (pickHouseVisit(bot, run) && !isCrowded(run.tile)) {
			claimHouseVisit(bot.guid, run);
			houseCandidateClaimed = true;
			dynSlots[4] = BotPOI { "_house_visit", run.tile, POIType::HOUSE };
			candidates.push_back(&dynSlots[4]);
			weights.push_back(proxAdjust(run.tile, getPOIWeight(POIType::HOUSE)));
		}
	}

	// ---- BOT_SHRINE_IDLE: go stand in front of a reward / imbuing shrine ----
	// Gated exactly like the three above: shrineVisitPct gates the ENTIRE block so at 0 nothing is
	// sampled, and AWAKE bots only — the virtual simulator never runs the arrival handler, so a
	// hibernated bot would win the roll, hold a stand tile and never actually go.
	//
	// ONE roll covers BOTH kinds. That is a deliberate distribution choice, not an oversight: at
	// botShrineVisitPct=25 the two candidates appear together in the same 25% of rerolls rather
	// than independently, and TABLE B's two weights decide the split between them. Two separate
	// rolls would have made the combined shrine share depend on the pct twice over.
	//
	// Discovery is the runtime scan (findNearbyShrines) behind a per-town memo — there is no
	// index and no cache file, so nothing here can be stale relative to the map.
	bool rewardCandidateClaimed = false;
	bool imbuingCandidateClaimed = false;
	// Offered spots are held HERE, not written into s_shrineRuns, because both kinds can be
	// offered in one roll and s_shrineRuns is keyed by guid alone — writing at offer time would
	// have the second kind silently overwrite the first, and the winner could then walk to one
	// shrine while its run pointed at the other. The run is created in the commit block below,
	// for the winning kind only. Same split as pickHouseVisit / claimHouseVisit.
	ShrineSpot offeredSpot[2];
	const bool shrineAllowed = livenessCfg_.shrineVisitPct > 0
		&& uniform_random(1, 100) <= livenessCfg_.shrineVisitPct;
	// isShrineVisiting guard: a bot already on a shrine errand must not be offered another. In
	// practice a reroll cannot reach here mid-visit (APPROACH holds hasWalkTarget, IDLE is
	// consumed by tickShrineVisit), but the commit below assigns s_shrineRuns[guid] outright, so
	// if it ever did, the old run's claim would be orphaned until SHRINE_CLAIM_MS expired.
	if (shrineAllowed && !bot.hibernated && !pzLocked && !isShrineVisiting(bot.guid)) {
		// P1 runs ONCE here, not once per kind: findNearbyShrines fills both kinds in a single
		// pass, so scanning per kind would double the cost for nothing. Both kind-blocks below
		// read this same result.
		ShrineScanResult localShrines;
		findLocalShrines(bot, localShrines);
		struct KindSlot { uint8_t kind; POIType type; const char* name; size_t slot; bool* claimed; };
		const KindSlot kinds[2] = {
			{ SHRINE_KIND_REWARD,  POIType::REWARD_SHRINE,  "_reward_shrine",  5, &rewardCandidateClaimed  },
			{ SHRINE_KIND_IMBUING, POIType::IMBUING_SHRINE, "_imbuing_shrine", 6, &imbuingCandidateClaimed },
		};
		for (const auto& k : kinds) {
			if (bot.visitedPOIs.count(k.name)) continue;
			// Skip a zero-weight kind before doing any work. The static-POI loop already applies
			// this rule (`if (weight <= 0) continue;`), and it matters more here: a zero-weight
			// candidate can never win the roll, but offering it would still run the scan on a
			// memo miss and take a claim that only the loser-cleanup gives back. This is the
			// shipped default state — both weights are 0 until config.lua says otherwise.
			const int kindWeight = getPOIWeight(k.type);
			if (kindWeight <= 0) continue;
			ShrineSpot spot;
			if (!selectShrineSpot(bot, k.kind, localShrines, spot)) continue;
			if (isCrowded(spot.stand)) continue;
			if (isShrineTileClaimed(spot.stand, bot.guid)) continue;
			// Speculative, exactly like the fishing and house claims: offering the candidate
			// reserves the tile, and the commit/discard below hands it back if a different
			// candidate wins the roll. Claimed per (guid, kind) — a guid-only release fired when
			// ONE kind loses would otherwise free the other kind's winning claim too.
			claimShrineSpot(bot.guid, k.kind, spot);
			*k.claimed = true;
			offeredSpot[k.kind - 1] = spot;
			dynSlots[k.slot] = BotPOI { k.name, spot.stand, k.type };
			candidates.push_back(&dynSlots[k.slot]);
			weights.push_back(proxAdjust(spot.stand, kindWeight));
		}
	}

	if (candidates.empty()) {
		bot.visitedPOIs.clear();
		// Retry with cleared visited set (skip dynamic generation on retry to keep it simple).
		// Proximity bonus intentionally omitted here — safe degradation to base per-type weights
		// on the rare exhausted-visit fallback (BOT_PLAYER_PROXIMITY_WEIGHTING §5.8).
		for (const auto& poi : it->second) {
			int weight = getPOIWeight(poi.type);
			if (weight <= 0) continue;
			if (pzLocked && (poi.type == POIType::DEPOT || poi.type == POIType::TEMPLE || poi.type == POIType::BOAT)) continue;
			candidates.push_back(&poi);
			weights.push_back(weight);
		}
		if (candidates.empty()) return nullptr;
	}

	int totalWeight = 0;
	for (int w : weights) totalWeight += w;
	int roll = uniform_random(1, totalWeight);
	int cumulative = 0;
	for (size_t i = 0; i < candidates.size(); i++) {
		cumulative += weights[i];
		if (roll <= cumulative) {
			bot.visitedPOIs.insert(candidates[i]->name);
			// Commit or discard the NPC claim based on what actually won. Offering the slot
			// already reserved an approach tile for 45s; if the roll went elsewhere that tile
			// must be handed back immediately, or a bot that never visits an NPC would keep
			// blocking a counter spot for every other bot.
			if (!npcCandidateName.empty()) {
				if (candidates[i]->type == POIType::NPC) {
					s_pendingNpcVisit[bot.guid] = npcCandidateName;
					g_logger().info("[NPC_VISIT_START] bot guid={} town={} -> '{}' tile=({},{},{}) from=({},{},{})",
						bot.guid, bot.townId, npcCandidateName,
						candidates[i]->pos.x, candidates[i]->pos.y, candidates[i]->pos.z,
						bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
				} else {
					releaseNpcApproach(bot.guid);
				}
			}
			// Same commit/discard rule for the fishing claim: only the WATER candidate keeps it.
			if (fishCandidateClaimed && candidates[i]->type != POIType::WATER) {
				releaseFishingSpot(bot.guid);
			}
			// ...and for the house claim, which holds a tile, possibly a dummy, and an occupancy
			// slot. A losing candidate that kept them would block a house for 15 minutes.
			if (houseCandidateClaimed) {
				if (candidates[i]->type == POIType::HOUSE) {
					const auto& run = s_houseRuns[bot.guid];
					g_logger().info("[HOUSE_VISIT_START] guid={} house={} town={} mode={} tile=({},{},{}) from=({},{},{}) dist={}",
						bot.guid, run.houseId, bot.townId, static_cast<int>(run.mode),
						run.tile.x, run.tile.y, run.tile.z,
						bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
						std::max(std::abs(static_cast<int32_t>(run.tile.x) - static_cast<int32_t>(bot.currentPos.x)),
						         std::abs(static_cast<int32_t>(run.tile.y) - static_cast<int32_t>(bot.currentPos.y))));
				} else {
					endHouseVisit(bot.guid, "lost_roll");
				}
			}
			// Shrine claims, per KIND. Both kinds can have been offered and each holds its own
			// stand tile, so this cannot be a single guid-keyed release: the losing kind hands
			// its tile back while the winning kind keeps its own. The winner's run is created
			// HERE rather than at offer time, for the same reason.
			if (rewardCandidateClaimed || imbuingCandidateClaimed) {
				const bool rewardWon  = candidates[i]->type == POIType::REWARD_SHRINE;
				const bool imbuingWon = candidates[i]->type == POIType::IMBUING_SHRINE;
				if (rewardCandidateClaimed && !rewardWon) releaseShrineClaim(bot.guid, SHRINE_KIND_REWARD);
				if (imbuingCandidateClaimed && !imbuingWon) releaseShrineClaim(bot.guid, SHRINE_KIND_IMBUING);
				if (rewardWon || imbuingWon) {
					const uint8_t kind = rewardWon ? SHRINE_KIND_REWARD : SHRINE_KIND_IMBUING;
					const ShrineSpot& s = offeredSpot[kind - 1];
					s_shrineRuns[bot.guid] = ShrineRun { s.shrine, s.stand, kind,
						ShrinePhase::APPROACH, 0, 0, 0 };
					g_logger().info("[SHRINE_VISIT_START] guid={} town={} kind={} shrine=({},{},{}) "
						"stand=({},{},{}) from=({},{},{}) dist={}",
						bot.guid, bot.townId, rewardWon ? "reward" : "imbuing",
						s.shrine.x, s.shrine.y, s.shrine.z, s.stand.x, s.stand.y, s.stand.z,
						bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
						std::max(std::abs(static_cast<int32_t>(s.stand.x) - static_cast<int32_t>(bot.currentPos.x)),
						         std::abs(static_cast<int32_t>(s.stand.y) - static_cast<int32_t>(bot.currentPos.y))));
				}
			}
			// 2026-06-10 diag: log AdvStone selections so we can verify visitedPOIs.erase
			// fix is actually unblocking runtime re-picks. The existing "virtual AdvStone
			// trip start (pre-advanced)" log only fires for startup pre-advancement, not
			// for runtime selectNextPOI hits — without this line, runtime AdvStone selection
			// is invisible in the journal.
			if (candidates[i]->type == POIType::ADVENTURER_STONE) {
				g_logger().info("[BotEngine] POI_SELECT_ADVSTONE: bot guid={} name='{}' town={} pos=({},{},{}) candidate_pool={}",
					bot.guid, bot.name, bot.townId,
					bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					candidates.size());
			}
			return candidates[i];
		}
	}
	// Rounding fallback — same commit/discard rule as above.
	if (!npcCandidateName.empty()) {
		if (candidates.back()->type == POIType::NPC) {
			s_pendingNpcVisit[bot.guid] = npcCandidateName;
		} else {
			releaseNpcApproach(bot.guid);
		}
	}
	if (fishCandidateClaimed && candidates.back()->type != POIType::WATER) {
		releaseFishingSpot(bot.guid); // rounding fallback — same rule as the loop above
	}
	if (houseCandidateClaimed && candidates.back()->type != POIType::HOUSE) {
		endHouseVisit(bot.guid, "lost_roll"); // rounding fallback — same rule as the loop above
	}
	// Rounding fallback for the shrine claims — same per-kind rule as the loop above. Omitting
	// this half is the exact shape of a silent claim leak: the loop's `return` is skipped when
	// integer rounding leaves `roll` past the last cumulative bound, so this path really is
	// reachable, and a claim released nowhere would hold a stand tile for SHRINE_CLAIM_MS.
	if (rewardCandidateClaimed || imbuingCandidateClaimed) {
		const bool rewardWon  = candidates.back()->type == POIType::REWARD_SHRINE;
		const bool imbuingWon = candidates.back()->type == POIType::IMBUING_SHRINE;
		if (rewardCandidateClaimed && !rewardWon) releaseShrineClaim(bot.guid, SHRINE_KIND_REWARD);
		if (imbuingCandidateClaimed && !imbuingWon) releaseShrineClaim(bot.guid, SHRINE_KIND_IMBUING);
		if (rewardWon || imbuingWon) {
			const uint8_t kind = rewardWon ? SHRINE_KIND_REWARD : SHRINE_KIND_IMBUING;
			const ShrineSpot& s = offeredSpot[kind - 1];
			s_shrineRuns[bot.guid] = ShrineRun { s.shrine, s.stand, kind,
				ShrinePhase::APPROACH, 0, 0, 0 };
			g_logger().info("[SHRINE_VISIT_START] guid={} town={} kind={} shrine=({},{},{}) "
				"stand=({},{},{}) (rounding fallback)",
				bot.guid, bot.townId, rewardWon ? "reward" : "imbuing",
				s.shrine.x, s.shrine.y, s.shrine.z, s.stand.x, s.stand.y, s.stand.z);
		}
	}
	return candidates.back();
}

// AdvStone constants/helpers moved to top of file (see ~line 2310) so the
// v2 virtual simulator can reach them. `pickAdventurerStoneIdleIdx` stays here
// because it depends on `adventurerStoneRoute_` which is loaded later.

uint16_t BotEngine::pickAdventurerStoneIdleIdx(uint8_t requireZ) const {
	std::vector<uint16_t> candidates;
	if (adventurerStoneRoute_.size() < 2) return 0;
	// Exclude:
	//   - the final waypoint (forcefield — always the trip end)
	//   - non-NODE waypoints (stairs_up/stairs_down don't make sense as dwell points)
	//   - the entry teleport tile (32210, 32300, 6) — bot lands here at trip start
	//   - the tile right before the forcefield (32210, 32293, 6) — too close to the exit
	//   - waypoints not on requireZ (when non-zero), so the chosen idle is on the same
	//     floor as a planned chest/dummy sub-activity (per BOT_LIVENESS_PACK Phase A.5
	//     — replaces the previous mode-1/2 demotion when idle was on z=6).
	constexpr Position kSkipEntry = Position(32210, 32300, 6);
	constexpr Position kSkipPreForcefield = Position(32210, 32293, 6);
	for (size_t i = 0; i + 1 < adventurerStoneRoute_.size(); ++i) {
		if (adventurerStoneRoute_[i].type != WaypointType::NODE) continue;
		const auto& p = adventurerStoneRoute_[i].pos;
		if (p == kSkipEntry || p == kSkipPreForcefield) continue;
		if (requireZ != 0 && p.z != requireZ) continue;
		candidates.push_back(static_cast<uint16_t>(i));
	}
	if (candidates.empty()) return 0;
	int32_t pick = uniform_random(0, static_cast<int32_t>(candidates.size()) - 1);
	return candidates[static_cast<size_t>(pick)];
}

// Tile-pick helpers for Adv Stone sub-activity selection.
// Walkability check: same primitives as findReachableDepotLocker (BLOCKSOLID + creature scan).

// BOT_LIVENESS_PACK perf hotfix: refresh the cached liveness config struct.
// All hot-path reads go through this cache; admins changing config.lua need
// to wait up to 5s OR /reload for changes to take effect — acceptable trade
// vs the ~22% CPU saved by avoiding per-tick mutex acquires in ConfigManager.
void BotEngine::refreshLivenessCfgIfStale(int64_t maxAgeMs) {
	const int64_t now = OTSYS_TIME();
	if (livenessCfg_.lastRefreshMs != 0 && (now - livenessCfg_.lastRefreshMs) < maxAgeMs) return;
	auto& cm = g_configManager();
	auto& c = livenessCfg_;
	c.poiWeightDepot           = static_cast<int32_t>(cm.getNumber(BOT_POI_DEPOT));
	c.poiWeightDepotOutside    = static_cast<int32_t>(cm.getNumber(BOT_POI_DEPOT_OUTSIDE));
	c.poiWeightTemple          = static_cast<int32_t>(cm.getNumber(BOT_POI_TEMPLE));
	c.poiWeightBoat            = static_cast<int32_t>(cm.getNumber(BOT_POI_BOAT));
	c.poiWeightShop            = static_cast<int32_t>(cm.getNumber(BOT_POI_SHOP));
	c.poiWeightNpc             = static_cast<int32_t>(cm.getNumber(BOT_POI_NPC));
	c.npcVisitPct              = static_cast<int32_t>(cm.getNumber(BOT_NPC_VISIT_PCT));
	c.poiWeightWater           = static_cast<int32_t>(cm.getNumber(BOT_POI_WATER));
	c.poiWeightHouse           = static_cast<int32_t>(cm.getNumber(BOT_POI_HOUSE));
	c.houseVisitPct            = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_VISIT_PCT));
	c.houseIdleMinSec          = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_IDLE_MIN_SEC));
	c.houseIdleMaxSec          = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_IDLE_MAX_SEC));
	c.houseMaxDist             = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_MAX_DIST));
	c.houseMaxOccupants        = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_MAX_OCCUPANTS));
	c.houseIdlePct             = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_IDLE));
	c.houseHirelingPct         = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_HIRELING));
	c.houseDummyPct            = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_DUMMY));
	c.houseLockerPct           = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_LOCKER));
	c.houseShrinePct           = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_SHRINE));
	c.houseSettleSec           = static_cast<int32_t>(cm.getNumber(BOT_HOUSE_SETTLE_SEC));
	c.poiWeightRewardShrine    = static_cast<int32_t>(cm.getNumber(BOT_POI_REWARD_SHRINE));
	c.poiWeightImbuingShrine   = static_cast<int32_t>(cm.getNumber(BOT_POI_IMBUING_SHRINE));
	c.shrineVisitPct           = static_cast<int32_t>(cm.getNumber(BOT_SHRINE_VISIT_PCT));
	c.shrineMaxOccupants       = static_cast<int32_t>(cm.getNumber(BOT_SHRINE_MAX_OCCUPANTS));
	c.shrineIdleMinSec         = static_cast<int32_t>(cm.getNumber(BOT_SHRINE_IDLE_MIN_SEC));
	c.shrineIdleMaxSec         = static_cast<int32_t>(cm.getNumber(BOT_SHRINE_IDLE_MAX_SEC));
	c.shrineSettleSec          = static_cast<int32_t>(cm.getNumber(BOT_SHRINE_SETTLE_SEC));
	c.fishPct                  = static_cast<int32_t>(cm.getNumber(BOT_FISH_PCT));
	c.fishMaxDist              = static_cast<int32_t>(cm.getNumber(BOT_FISH_MAX_DIST));
	c.fishMaxDz                = static_cast<int32_t>(cm.getNumber(BOT_FISH_MAX_DZ));
	c.fishCastIntervalMinMs    = static_cast<int32_t>(cm.getNumber(BOT_FISH_CAST_INTERVAL_MIN_MS));
	c.fishCastIntervalMaxMs    = static_cast<int32_t>(cm.getNumber(BOT_FISH_CAST_INTERVAL_MAX_MS));
	c.fishDurationMinSec       = static_cast<int32_t>(cm.getNumber(BOT_FISH_DURATION_MIN_SEC));
	c.fishDurationMaxSec       = static_cast<int32_t>(cm.getNumber(BOT_FISH_DURATION_MAX_SEC));
	c.fishMaxSpotsPerTown      = static_cast<int32_t>(cm.getNumber(BOT_FISH_MAX_SPOTS_PER_TOWN));
	c.fishZBand                = static_cast<int32_t>(cm.getNumber(BOT_FISH_Z_BAND));
	c.potionChancePct          = static_cast<int32_t>(cm.getNumber(BOT_POTION_CHANCE_PCT));
	c.potionMinIntervalMs      = static_cast<int32_t>(cm.getNumber(BOT_POTION_MIN_INTERVAL_MS));
	c.foodChancePct            = static_cast<int32_t>(cm.getNumber(BOT_FOOD_CHANCE_PCT));
	c.foodMinIntervalMs        = static_cast<int32_t>(cm.getNumber(BOT_FOOD_MIN_INTERVAL_MS));
	c.runeCraftChancePct       = static_cast<int32_t>(cm.getNumber(BOT_RUNECRAFT_CHANCE_PCT));
	c.runeCraftIntervalMinMs   = static_cast<int32_t>(cm.getNumber(BOT_RUNECRAFT_INTERVAL_MIN_MS));
	c.runeCraftIntervalMaxMs   = static_cast<int32_t>(cm.getNumber(BOT_RUNECRAFT_INTERVAL_MAX_MS));
	c.runeCraftRefillMana      = cm.getBoolean(BOT_RUNECRAFT_REFILL_MANA);
	c.supportSpellChancePct    = static_cast<int32_t>(cm.getNumber(BOT_SUPPORT_SPELL_CHANCE_PCT));
	c.supportSpellIntervalMinMs = static_cast<int32_t>(cm.getNumber(BOT_SUPPORT_SPELL_INTERVAL_MIN_MS));
	c.supportSpellIntervalMaxMs = static_cast<int32_t>(cm.getNumber(BOT_SUPPORT_SPELL_INTERVAL_MAX_MS));
	c.poiWeightAdvStone        = static_cast<int32_t>(cm.getNumber(BOT_POI_ADV_STONE));
	c.dwellRerollMinSec        = static_cast<int32_t>(cm.getNumber(BOT_DWELL_REROLL_MIN_SEC));
	c.dwellRerollMaxSec        = static_cast<int32_t>(cm.getNumber(BOT_DWELL_REROLL_MAX_SEC));
	c.dwellPoiMinSec           = static_cast<int32_t>(cm.getNumber(BOT_DWELL_POI_MIN_SEC));
	c.dwellPoiMaxSec           = static_cast<int32_t>(cm.getNumber(BOT_DWELL_POI_MAX_SEC));
	c.dwellNpcMinSec           = static_cast<int32_t>(cm.getNumber(BOT_DWELL_NPC_MIN_SEC));
	c.dwellNpcMaxSec           = static_cast<int32_t>(cm.getNumber(BOT_DWELL_NPC_MAX_SEC));
	c.dwellPostTravelSec       = static_cast<int32_t>(cm.getNumber(BOT_DWELL_POST_TRAVEL_SEC));
	c.advStoneDwellIdleMinSec  = static_cast<int32_t>(cm.getNumber(BOT_ADV_STONE_DWELL_IDLE_MIN_SEC));
	c.advStoneDwellIdleMaxSec  = static_cast<int32_t>(cm.getNumber(BOT_ADV_STONE_DWELL_IDLE_MAX_SEC));
	c.advStoneDwellChestMinSec = static_cast<int32_t>(cm.getNumber(BOT_ADV_STONE_DWELL_CHEST_MIN_SEC));
	c.advStoneDwellChestMaxSec = static_cast<int32_t>(cm.getNumber(BOT_ADV_STONE_DWELL_CHEST_MAX_SEC));
	c.advStoneDwellDummyMinSec = static_cast<int32_t>(cm.getNumber(BOT_ADV_STONE_DWELL_DUMMY_MIN_SEC));
	c.advStoneDwellDummyMaxSec = static_cast<int32_t>(cm.getNumber(BOT_ADV_STONE_DWELL_DUMMY_MAX_SEC));
	c.mountChancePct           = static_cast<int32_t>(cm.getNumber(BOT_MOUNT_CHANCE_PCT));
	c.poiCrowdCapCount         = static_cast<int32_t>(cm.getNumber(BOT_POI_CROWD_CAP_COUNT));
	c.poiCrowdCapRadius        = static_cast<int32_t>(cm.getNumber(BOT_POI_CROWD_CAP_RADIUS));
	c.proxEnabled              = cm.getBoolean(BOT_PROX_WEIGHT_ENABLED);
	c.proxAwake                = cm.getBoolean(BOT_PROX_WEIGHT_AWAKE);
	c.proxBaselineWeight       = static_cast<int32_t>(cm.getNumber(BOT_PROX_BASELINE_WEIGHT));
	c.proxNearTiles            = static_cast<int32_t>(cm.getNumber(BOT_PROX_NEAR_TILES));
	c.proxMidTiles             = static_cast<int32_t>(cm.getNumber(BOT_PROX_MID_TILES));
	c.proxBonusNear            = static_cast<int32_t>(cm.getNumber(BOT_PROX_BONUS_NEAR));
	c.proxBonusMid             = static_cast<int32_t>(cm.getNumber(BOT_PROX_BONUS_MID));
	c.proxSampleCap            = static_cast<int32_t>(cm.getNumber(BOT_PROX_SAMPLE_CAP));
	c.proxTravelCatBonus       = static_cast<int32_t>(cm.getNumber(BOT_PROX_TRAVEL_CAT_BONUS));
	c.turnInPlaceChancePct     = static_cast<int32_t>(cm.getNumber(BOT_TURN_IN_PLACE_CHANCE_PCT));
	c.turnInPlaceIntervalTicks = static_cast<int32_t>(cm.getNumber(BOT_TURN_IN_PLACE_INTERVAL_TICKS));
	c.walkPauseChancePct       = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_CHANCE_PCT));
	c.walkPauseMinMs           = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_MIN_MS));
	c.walkPauseMaxMs           = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_MAX_MS));
	c.walkPauseMaxPerRoute     = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_MAX_PER_ROUTE));
	// BOT_ACTIVITY_PCT: TABLE B must sum to 100. Checked HERE rather than at startup only,
	// because this is the one function every path (boot, 5s refresh, and the forced refresh
	// inside '/cavebot _global reloadconfig') funnels through -- so a table broken by a live
	// retune is reported within seconds instead of surviving until the next restart.
	validatePoiTable();
	validateHouseTable();
	c.walkPauseObservedChancePct   = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_OBSERVED_CHANCE_PCT));
	c.walkPauseObservedMinMs       = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_OBSERVED_MIN_MS));
	c.walkPauseObservedMaxMs       = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_OBSERVED_MAX_MS));
	c.walkPauseObservedMaxPerRoute = static_cast<int32_t>(cm.getNumber(BOT_WALK_PAUSE_OBSERVED_MAX_PER_ROUTE));
	c.fidgetChancePct          = static_cast<int32_t>(cm.getNumber(BOT_FIDGET_CHANCE_PCT));
	c.fidgetIntervalMinSec     = static_cast<int32_t>(cm.getNumber(BOT_FIDGET_INTERVAL_MIN_SEC));
	c.fidgetIntervalMaxSec     = static_cast<int32_t>(cm.getNumber(BOT_FIDGET_INTERVAL_MAX_SEC));
	c.fidgetMaxItemValueGp     = static_cast<int32_t>(cm.getNumber(BOT_FIDGET_MAX_ITEM_VALUE_GP));
	c.chatCooldownMinMs        = static_cast<int32_t>(cm.getNumber(BOT_CHAT_COOLDOWN_MIN_MS));
	c.chatCooldownMaxMs        = static_cast<int32_t>(cm.getNumber(BOT_CHAT_COOLDOWN_MAX_MS));
	c.worldChatIntervalMinMs   = static_cast<int32_t>(cm.getNumber(BOT_WORLD_CHAT_INTERVAL_MIN_MS));
	c.worldChatIntervalMaxMs   = static_cast<int32_t>(cm.getNumber(BOT_WORLD_CHAT_INTERVAL_MAX_MS));
	c.advertisingIntervalMinMs = static_cast<int32_t>(cm.getNumber(BOT_ADVERTISING_INTERVAL_MIN_MS));
	c.advertisingIntervalMaxMs = static_cast<int32_t>(cm.getNumber(BOT_ADVERTISING_INTERVAL_MAX_MS));
	c.chatAntiRepeatRingSize   = static_cast<int32_t>(cm.getNumber(BOT_CHAT_ANTI_REPEAT_RING_SIZE));
	c.chatMasterChancePct      = static_cast<int32_t>(cm.getNumber(BOT_CHAT_MASTER_CHANCE_PCT));
	c.chatVerboseLog           = cm.getBoolean(BOT_CHAT_VERBOSE_LOG);
	c.telemetryEnabled         = cm.getBoolean(BOT_TELEMETRY_ENABLED);
	c.personalityRerollOnRestart = cm.getBoolean(BOT_PERSONALITY_REROLL_ON_RESTART);
	c.hibernatedChatEnabled    = cm.getBoolean(BOT_HIBERNATED_CHAT_ENABLED);

	// Refresh real-player count alongside config. Single O(players_online) pass —
	// players_online is typically small (a handful of real players at most, plus
	// bots filtered out). Refreshed every 5s. Used to gate channel chat (silence
	// when no real reader is online).
	const int32_t prevReal = c.realPlayerCount;
	int32_t real = 0;
	for (const auto& [id, p] : g_game().getPlayers()) {
		if (p && !p->isBotPlayer() && !p->isRemoved()) ++real;
	}
	c.realPlayerCount = real;

	// Legacy login-flood fix (2026-06-04): when realPlayerCount transitions
	// from 0 to non-zero, re-jitter every bot's channel-chat timers to a
	// future point in the full configured interval. Reason: while channels
	// were silenced (realPlayerCount==0), tryEmitChat returned false without
	// advancing the timer, so the timer kept being "due" (now >= next). Once
	// the gate opens, every bot fires immediately on the next tick — flooding
	// World Chat + Advertising with dozens of simultaneous messages.
	//
	// Fix: same stagger registerBot uses at startup — set next = now + uniform(0, max).
	// This means after a 0→1 transition, channel chat ramps up gradually over the
	// next 90-180s (world) / 30-90s (adv) instead of bursting in one tick.
	if (prevReal == 0 && real > 0) {
		const int32_t worldMaxMs = c.worldChatIntervalMaxMs;
		const int32_t advMaxMs   = c.advertisingIntervalMaxMs;
		int32_t restaggered = 0;
		for (auto& b : bots_) {
			b.nextWorldChatTime   = now + uniform_random(0, std::max(1, worldMaxMs));
			b.nextAdvertisingTime = now + uniform_random(0, std::max(1, advMaxMs));
			++restaggered;
		}
		g_logger().info(
			"[BotChat] realPlayerCount 0->{} — re-jittered channel-chat timers on {} bots "
			"(staggered over next {}ms world / {}ms adv)",
			real, restaggered, worldMaxMs, advMaxMs);
	}

	// BOT_PARTY_TRAIL_FOLLOW: refresh the trail config cache on the same 5s cadence.
	trailCfg_.enable      = cm.getBoolean(BOT_PARTY_TRAIL_ENABLE);
	// BOT_CORPSE_LOOT — same 5s cadence, so `/cavebot _global reloadconfig` retunes live.
	lootCfg_.enable        = cm.getBoolean(BOT_LOOT_OPEN_ENABLE);
	lootCfg_.walk          = cm.getBoolean(BOT_LOOT_WALK);
	lootCfg_.publicCleanup = cm.getBoolean(BOT_LOOT_PUBLIC_CLEANUP);
	lootCfg_.radius        = static_cast<int32_t>(cm.getNumber(BOT_LOOT_RADIUS));
	lootCfg_.windowMs      = static_cast<int32_t>(cm.getNumber(BOT_LOOT_WINDOW_MS));
	lootCfg_.scanMs        = static_cast<int32_t>(cm.getNumber(BOT_LOOT_SCAN_MS));
	lootCfg_.maxWalkMs     = static_cast<int32_t>(cm.getNumber(BOT_LOOT_MAX_WALK_MS));
	lootCfg_.delayMinMs    = static_cast<int32_t>(cm.getNumber(BOT_LOOT_DELAY_MIN_MS));
	lootCfg_.delayMaxMs    = static_cast<int32_t>(cm.getNumber(BOT_LOOT_DELAY_MAX_MS));
	// BOT_LURE_KITE — same 5s cadence, so both behaviours retune live via
	// `/cavebot _global reloadconfig` without a restart.
	lureCfg_.enable          = cm.getBoolean(BOT_LURE_ENABLE);
	lureCfg_.levelFactorPct  = static_cast<int32_t>(cm.getNumber(BOT_LURE_LEVEL_FACTOR_PCT));
	lureCfg_.partyAlways     = cm.getBoolean(BOT_LURE_PARTY_ALWAYS);
	lureCfg_.partyDefaultMin = static_cast<int32_t>(cm.getNumber(BOT_LURE_PARTY_DEFAULT_MIN));
	// The census reads the spectator cache, which is filled to MONSTER_SCAN_RADIUS.
	// A larger radius would silently undercount instead of widening the net, so it is
	// clamped HERE rather than trusted at the use site.
	lureCfg_.radius          = std::clamp(
		static_cast<int32_t>(cm.getNumber(BOT_LURE_RADIUS)), 2, MONSTER_SCAN_RADIUS);
	lureCfg_.maxPack         = static_cast<int32_t>(cm.getNumber(BOT_LURE_MAX_PACK));
	lureCfg_.paceDist        = static_cast<int32_t>(cm.getNumber(BOT_LURE_PACE_DIST));
	lureCfg_.paceMaxMs       = static_cast<int32_t>(cm.getNumber(BOT_LURE_PACE_MAX_MS));
	lureCfg_.maxMs           = static_cast<int32_t>(cm.getNumber(BOT_LURE_MAX_MS));
	lureCfg_.hpFloorPct      = static_cast<int32_t>(cm.getNumber(BOT_LURE_HP_FLOOR_PCT));
	lureCfg_.blockedMs       = static_cast<int32_t>(cm.getNumber(BOT_LURE_BLOCKED_MS));
	lureCfg_.contactMs       = static_cast<int32_t>(cm.getNumber(BOT_LURE_CONTACT_MS));
	lureCfg_.decayMs         = static_cast<int32_t>(cm.getNumber(BOT_LURE_DECAY_MS));
	lureCfg_.kiteEnable      = cm.getBoolean(BOT_KITE_BACKTRACK_ENABLE);
	lureCfg_.kiteDepthWps    = static_cast<int32_t>(cm.getNumber(BOT_KITE_DEPTH_WPS));
	lureCfg_.kiteMaxSpanTiles= static_cast<int32_t>(cm.getNumber(BOT_KITE_MAX_SPAN_TILES));
	lureCfg_.kiteMaxLegs     = static_cast<int32_t>(cm.getNumber(BOT_KITE_MAX_LEGS));
	lureCfg_.kiteMaxMs       = static_cast<int32_t>(cm.getNumber(BOT_KITE_MAX_MS));
	lureCfg_.kiteCooldownMs  = static_cast<int32_t>(cm.getNumber(BOT_KITE_COOLDOWN_MS));
	trailCfg_.humanLead   = cm.getBoolean(BOT_PARTY_TRAIL_HUMAN_LEAD);
	trailCfg_.stuckMs     = static_cast<int32_t>(cm.getNumber(BOT_PARTY_TRAIL_STUCK_MS));
	trailCfg_.maxLagTiles = static_cast<int32_t>(cm.getNumber(BOT_PARTY_TRAIL_MAX_LAG_TILES));
	trailCfg_.maxNodes    = static_cast<int32_t>(cm.getNumber(BOT_PARTY_TRAIL_MAX_NODES));
	trailCfg_.horizon     = static_cast<int32_t>(cm.getNumber(BOT_PARTY_TRAIL_HORIZON));
	trailCfg_.maxAgeMs    = static_cast<int32_t>(cm.getNumber(BOT_PARTY_TRAIL_MAX_AGE_MS));
	trailCfg_.waitDist    = static_cast<int32_t>(cm.getNumber(BOT_PARTY_LEADER_WAIT_DIST));
	trailCfg_.waitMaxMs   = static_cast<int32_t>(cm.getNumber(BOT_PARTY_LEADER_WAIT_MAX_MS));
	trailCfg_.maxPartyPct = static_cast<int32_t>(cm.getNumber(BOT_PARTY_MAX_PCT));

	// BOT_PARTY_INVITE_RENDEZVOUS: same 5s cadence. followDist is clamped here rather than at
	// the use site — an operator typo of 0 would freeze followers on top of the leader, and
	// anything >= PARTY_LEASH_DIST would let them drift past the chase leash.
	trailCfg_.followDist = std::clamp(
		static_cast<int32_t>(cm.getNumber(BOT_PARTY_FOLLOW_DIST)), 1, PARTY_LEASH_DIST - 1);

	inviteCfg_.enable      = cm.getBoolean(BOT_PARTY_INVITE_ENABLE);
	inviteCfg_.pollMs      = static_cast<int32_t>(cm.getNumber(BOT_PARTY_INVITE_POLL_MS));
	inviteCfg_.acceptMinMs = static_cast<int32_t>(cm.getNumber(BOT_PARTY_INVITE_ACCEPT_MIN_MS));
	inviteCfg_.acceptMaxMs = static_cast<int32_t>(cm.getNumber(BOT_PARTY_INVITE_ACCEPT_MAX_MS));
	inviteCfg_.holdMaxMs   = static_cast<int32_t>(cm.getNumber(BOT_PARTY_INVITE_HOLD_MAX_MS));
	// uniform_random(min, max) needs min <= max; a swapped pair in config.lua must not UB.
	if (inviteCfg_.acceptMaxMs < inviteCfg_.acceptMinMs) {
		inviteCfg_.acceptMaxMs = inviteCfg_.acceptMinMs;
	}

	asmCfg_.enable      = cm.getBoolean(BOT_PARTY_RV_ENABLE);
	asmCfg_.maxMs       = static_cast<int32_t>(cm.getNumber(BOT_PARTY_RV_MAX_MS));
	asmCfg_.finishMaxMs = static_cast<int32_t>(cm.getNumber(BOT_PARTY_RV_FINISH_MAX_MS));
	// A wind-down cap below the LEAVING phase ceiling would force-tear graceful hunt exits
	// mid-walk, which is the entire point of the FINISHING phase.
	if (asmCfg_.finishMaxMs < static_cast<int32_t>(LEAVING_PHASE_MAX_MS)) {
		asmCfg_.finishMaxMs = static_cast<int32_t>(LEAVING_PHASE_MAX_MS);
	}

	c.lastRefreshMs = now;
}

