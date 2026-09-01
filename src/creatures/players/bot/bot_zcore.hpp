/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// bot_zcore.hpp — dependency-free MULTI-FLOOR (cross-z) route planner.
//
// BOT_NAV_REALISM: true multi-floor pathfinding. Like bot_pathcore.hpp this is
// deliberately isolated from the game engine: it includes ONLY position.hpp and
// the standard library. All world knowledge arrives through:
//   - a ZPortalGraph the caller fills with portals (floor-change transitions
//     with their LANDING positions), and
//   - a LegCost callable estimating/validating same-floor traversal.
//
// Both sides link it:
//   - server: bot_zgraph.cpp builds the graph from the map's BasicTile cache
//     (no tile materialization) and plans with an optimistic Chebyshev LegCost
//     (runtime execution validates each leg; failures blacklist + replan).
//   - tools/botnavsim: builds the graph from a NavDump (v2 exact portals, or
//     v1 walkability-inferred) and plans with the REAL A* kernel as LegCost —
//     making every leg of the returned route a proven, walkable path. That is
//     the offline acceptance gate for multi-floor routing.
//
// Model: a route from S to T with S.z != T.z is a sequence
//   S --(same-floor walk)--> P1.pos --(traverse P1)--> P1.landing
//     --(same-floor walk)--> P2.pos --(traverse)--> ... --> T
// where each Pi is a portal (stairs/ramp/hole/ladder/rope spot/sewer grate/
// shovel pile/teleport). Planning is Dijkstra over portals; traversal cost is a
// flat hopCost (favours fewer floor changes), leg cost comes from LegCost.
//
// Why a portal graph instead of 3D A* neighbour expansion in bot_pathcore:
//   1. A floor change is not a "step" — ladders/ropes/shovels/sewers need item
//      USE actions with positional preconditions. A 3D tile path through them
//      is not executable by startAutoWalk; the runtime FC state machine must
//      run per transition regardless, so the natural plan unit is the portal.
//   2. The 2D kernel's node pool/key packing (x<<16|y) and the 512-node server
//      budget are load-bearing; cross-town multi-floor expansions would blow
//      both on every replan tick.
//   3. Portal counts are tiny (tens of thousands world-wide, a handful per
//      plan region), so Dijkstra here is microseconds while legs stay small
//      A* problems that reuse ALL existing door/blocker/retry machinery.
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/movement/position.hpp"

namespace botnav {

enum class ZPortalKind : uint8_t {
	STAIRS = 0, // walk-on tile-flag transition (stairs/ramps/holes with FLOORCHANGE flags)
	HOLE = 1, // walk-on FLOORCHANGE_DOWN (open hole / trapdoor)
	LADDER = 2, // USE item to go up (ladder ids / ITEM_TYPE_LADDER)
	ROPE_SPOT = 3, // USE rope on spot to go up
	SEWER = 4, // USE sewer grate to go down
	SHOVEL_HOLE = 5, // USE shovel on stone pile, then fall down
	TELEPORT = 6, // walk-on teleport item with explicit destination
	INFERRED = 7, // navdump-v1 inference (direction/kind unknown; landing snapped)
};

inline const char* zPortalKindName(ZPortalKind k) {
	switch (k) {
		case ZPortalKind::STAIRS:
			return "stairs";
		case ZPortalKind::HOLE:
			return "hole";
		case ZPortalKind::LADDER:
			return "ladder";
		case ZPortalKind::ROPE_SPOT:
			return "rope";
		case ZPortalKind::SEWER:
			return "sewer";
		case ZPortalKind::SHOVEL_HOLE:
			return "shovel";
		case ZPortalKind::TELEPORT:
			return "teleport";
		default:
			return "inferred";
	}
}

// One DIRECTED floor-change edge: standing near/on `pos` and traversing the
// mechanism puts you at `landing`. A physical staircase pair is two portals
// (one per direction), each anchored on its own floor's tile.
struct ZPortal {
	Position pos {};
	Position landing {};
	ZPortalKind kind = ZPortalKind::INFERRED;
	bool goesDown = false; // landing.z > pos.z (teleports may be same-z/cross-map)
	// True when `landing` was FORCE-PLACED rather than confirmed walkable — the
	// DOWN_FORCED mode below, or UP_SCAN's unconditional SOUTH fallback. The real
	// server forces the player there regardless, so the portal stays admitted, but a
	// planner must then check that exact tile and never accept a NEIGHBOUR's verdict:
	// both zLabelOf and LocalReach fall back to an 8-neighbour lookup when a tile has
	// no direct label, which would happily report "reachable" for a landing that is a
	// wall with walkable neighbours. See the guard in zPlanFullRoute's legCost.
	bool landingForced = false;
};

// How a landing position is derived. Replaces an earlier hand-rolled snap that did not
// match the game and was duplicated verbatim in tools/botnavsim.
enum class ZSnapMode {
	UP_SCAN,     // ladders and rope spots
	DOWN_FORCED, // sewer grates and shovel holes
};

// Landing for USE-item mechanics, mirroring the real scripts exactly.
//
// UP_SCAN reproduces Position:moveUpstairs() (data/libs/functions/position.lua:22):
// z-1, then SOUTH first; if that is not walkable, loop NORTH, EAST, WEST, SW, SE, NW, NE
// (the Lua loop runs DIRECTION_NORTH..NORTHEAST substituting SOUTH->WEST); and if none of
// them is walkable, `swap(self, defaultPosition)` places the player at SOUTH ANYWAY.
// The self-tile is NEVER a candidate and no radius-2 tile is ever tried — the previous
// implementation did both, and rejected the portal outright when nothing was walkable
// instead of force-placing, which is why some ladder/rope portals were silently missing
// or mis-landed.
//
// DOWN_FORCED reproduces the sewer-grate and shovel-hole actions: same (x,y), z+1,
// unconditional teleport, no scan and no walkability check at all.
//
// Always succeeds, exactly like the game. `outForced` reports whether the result was
// force-placed so callers can apply the stricter check described on ZPortal::landingForced.
template <class Walkable>
inline void zSnapLandingT(int32_t x, int32_t y, int32_t lz, ZSnapMode mode,
                          Walkable&& walkable, Position& out, bool& outForced) {
	if (mode == ZSnapMode::DOWN_FORCED) {
		out = Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(lz));
		outForced = true; // never validated, by design — the script forces it
		return;
	}
	// SOUTH, NORTH, EAST, WEST, SW, SE, NW, NE
	static constexpr int32_t order[8][2] = {
		{ 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 }, { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }
	};
	for (const auto& d : order) {
		const int32_t cx = x + d[0], cy = y + d[1];
		if (walkable(cx, cy, lz)) {
			out = Position(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), static_cast<uint8_t>(lz));
			outForced = false;
			return;
		}
	}
	out = Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y + 1), static_cast<uint8_t>(lz));
	outForced = true; // forced SOUTH, matching moveUpstairs' final swap
}

// Portal container + per-floor spatial hash (64x64 chunks) for radius queries.
class ZPortalGraph {
public:
	static constexpr int32_t CHUNK = 64;

	void clear() {
		portals_.clear();
		index_.clear();
		finalized_ = false;
	}

	void add(const ZPortal& p) {
		portals_.push_back(p);
		finalized_ = false;
	}

	void finalize() {
		index_.clear();
		for (uint32_t i = 0; i < portals_.size(); ++i) {
			const auto& p = portals_[i];
			index_[chunkKey(p.pos.z, p.pos.x / CHUNK, p.pos.y / CHUNK)].push_back(i);
		}
		finalized_ = true;
	}

	size_t size() const {
		return portals_.size();
	}
	bool empty() const {
		return portals_.empty();
	}
	const std::vector<ZPortal>& portals() const {
		return portals_;
	}
	const ZPortal& at(uint32_t idx) const {
		return portals_[idx];
	}

	// Visit portals whose pos is on floor z within Chebyshev `radius` of `c`.
	// f(uint32_t portalIdx, const ZPortal&).
	template <class F>
	void forEachOnFloorNear(uint8_t z, const Position& c, int32_t radius, F&& f) const {
		const int32_t cx1 = (std::max(0, static_cast<int32_t>(c.x) - radius)) / CHUNK;
		const int32_t cx2 = (static_cast<int32_t>(c.x) + radius) / CHUNK;
		const int32_t cy1 = (std::max(0, static_cast<int32_t>(c.y) - radius)) / CHUNK;
		const int32_t cy2 = (static_cast<int32_t>(c.y) + radius) / CHUNK;
		for (int32_t cx = cx1; cx <= cx2; ++cx) {
			for (int32_t cy = cy1; cy <= cy2; ++cy) {
				auto it = index_.find(chunkKey(z, static_cast<uint16_t>(cx), static_cast<uint16_t>(cy)));
				if (it == index_.end()) {
					continue;
				}
				for (uint32_t idx : it->second) {
					const ZPortal& p = portals_[idx];
					const int32_t d = std::max(
						std::abs(static_cast<int32_t>(p.pos.x) - static_cast<int32_t>(c.x)),
						std::abs(static_cast<int32_t>(p.pos.y) - static_cast<int32_t>(c.y))
					);
					if (d <= radius) {
						f(idx, p);
					}
				}
			}
		}
	}

private:
	static uint64_t chunkKey(uint8_t z, uint16_t cx, uint16_t cy) {
		return (static_cast<uint64_t>(z) << 40) | (static_cast<uint64_t>(cx) << 20) | cy;
	}

	std::vector<ZPortal> portals_;
	std::unordered_map<uint64_t, std::vector<uint32_t>> index_;
	bool finalized_ = false;
};

// Monotonic milliseconds, standard library only so this header stays dependency-free.
inline int64_t zSteadyMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct ZPlanParams {
	int32_t legMax = 80; // max same-floor leg length (Chebyshev) between hops
	int32_t hopCost = 150; // flat cost per traversal (≈ 15 straight steps) — favours fewer hops
	int32_t maxHops = 8; // plans needing more floor changes than this are rejected
	uint32_t maxExpansions = 4096; // Dijkstra settle budget (portals, not tiles)

	// ---- Optional wall-clock abort (server only; the offline simulator leaves both at zero) ----
	//
	// maxExpansions caps HOW MANY portals are settled, never how long each costs, so a search can
	// stay inside its expansion budget and still run for seconds. On the live server that blocks
	// the dispatcher, and the tick priority order says no bot work may do that.
	//
	// Carried in the params rather than the signature so tools/botnavsim/navsim.cpp keeps
	// compiling and behaving identically -- it simply never sets these.
	//
	// An abort means "I ran out of time", NOT "unreachable". The caller must distinguish them:
	// planZRoute returns false either way, and outAborted is what tells them apart. Treating an
	// abort as unreachable would blacklist portals for being slow.
	int64_t deadlineMonoMs = 0;   // 0 = no deadline
	bool* outAborted = nullptr;   // set true if the deadline stopped the search
};

struct ZRouteHop {
	uint32_t portalIdx = 0;
	ZPortal portal {};
};

// Dijkstra over the portal graph.
//   legCost(from, to)  -> int32_t : same-floor traversal cost estimate; < 0 = unreachable.
//                         from.z == to.z always holds when called.
//   exclude(portal)    -> bool    : true = portal unusable (runtime blacklist etc.).
// Returns true with outHops = ordered portal sequence (empty if start.z == target.z
// and directly leg-reachable). False if no route within the budgets.
inline int32_t zCheb(const Position& a, const Position& b) {
	return std::max(
		std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y))
	);
}

template <class LegCost, class Exclude>
bool planZRoute(const ZPortalGraph& g, const Position& start, const Position& target, std::vector<ZRouteHop>& outHops, const ZPlanParams& pp, LegCost&& legCost, Exclude&& exclude) {
	outHops.clear();

	if (start.z == target.z && zCheb(start, target) <= pp.legMax) {
		const int32_t direct = legCost(start, target);
		if (direct >= 0) {
			return true; // no hop needed
		}
		// else: same floor but not leg-reachable — maybe reachable via another floor
		// (bridges/underpasses). Fall through to graph search.
	}

	struct NodeState {
		int32_t dist = std::numeric_limits<int32_t>::max();
		int32_t hops = 0;
		uint32_t parent = UINT32_MAX; // portal idx of predecessor (UINT32_MAX = start)
		bool settled = false;
	};
	std::unordered_map<uint32_t, NodeState> states;

	struct QEntry {
		int32_t dist;
		uint32_t idx;
		bool operator>(const QEntry& o) const {
			return dist > o.dist;
		}
	};
	std::priority_queue<QEntry, std::vector<QEntry>, std::greater<QEntry>> open;

	// Seed: portals reachable on the start floor.
	g.forEachOnFloorNear(start.z, start, pp.legMax, [&](uint32_t idx, const ZPortal& p) {
		if (exclude(p)) {
			return;
		}
		const int32_t c = legCost(start, p.pos);
		if (c < 0) {
			return;
		}
		auto& st = states[idx];
		const int32_t nd = c + pp.hopCost;
		if (nd < st.dist) {
			st.dist = nd;
			st.hops = 1;
			st.parent = UINT32_MAX;
			open.push({ nd, idx });
		}
	});

	uint32_t expansions = 0;
	uint32_t goalIdx = UINT32_MAX;
	int32_t goalDist = std::numeric_limits<int32_t>::max();

	while (!open.empty() && expansions < pp.maxExpansions) {
		// Deadline check every 64 settles: frequent enough to bound the overrun to roughly one
		// batch of leg evaluations, rare enough that the clock read is noise. Aborting leaves any
		// floods it computed in the caller's reach cache, so the retry next tick resumes cheaply
		// rather than starting over.
		if (pp.deadlineMonoMs != 0 && (expansions & 63u) == 0 && expansions != 0
		    && zSteadyMs() > pp.deadlineMonoMs) {
			if (pp.outAborted != nullptr) {
				*pp.outAborted = true;
			}
			outHops.clear();
			return false;
		}
		auto [d, idx] = open.top();
		open.pop();
		auto& st = states[idx];
		if (st.settled || d > st.dist) {
			continue;
		}
		st.settled = true;
		++expansions;

		const ZPortal& p = g.at(idx);
		const Position& at = p.landing; // we are here after traversing portal idx

		// Goal test: on the target floor and leg-reachable to the target
		// (distance-capped like every other leg — runtime chunks walks, but a
		// leg beyond legMax was never validated by the seeding radius either).
		if (at.z == target.z && zCheb(at, target) <= pp.legMax) {
			const int32_t fc = legCost(at, target);
			if (fc >= 0) {
				const int32_t total = st.dist + fc;
				if (total < goalDist) {
					goalDist = total;
					goalIdx = idx;
				}
				// First settled goal is optimal enough (Dijkstra order + final leg
				// admissible-ish); keep it simple and stop.
				break;
			}
		}

		if (st.hops >= pp.maxHops) {
			continue;
		}

		// Expand: portals on the landing floor within legMax.
		g.forEachOnFloorNear(at.z, at, pp.legMax, [&](uint32_t nIdx, const ZPortal& np) {
			if (nIdx == idx || exclude(np)) {
				return;
			}
			const int32_t c = legCost(at, np.pos);
			if (c < 0) {
				return;
			}
			auto& nst = states[nIdx];
			const int32_t nd = st.dist + c + pp.hopCost;
			if (nd < nst.dist) {
				nst.dist = nd;
				nst.hops = st.hops + 1;
				nst.parent = idx;
				open.push({ nd, nIdx });
			}
		});
	}

	if (goalIdx == UINT32_MAX) {
		return false;
	}

	// Reconstruct portal chain.
	std::vector<uint32_t> chain;
	for (uint32_t cur = goalIdx; cur != UINT32_MAX; cur = states[cur].parent) {
		chain.push_back(cur);
	}
	std::reverse(chain.begin(), chain.end());
	outHops.reserve(chain.size());
	for (uint32_t idx : chain) {
		outHops.push_back({ idx, g.at(idx) });
	}
	return true;
}

} // namespace botnav
