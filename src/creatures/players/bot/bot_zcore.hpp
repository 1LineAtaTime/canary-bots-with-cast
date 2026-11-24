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
#include <bit>
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

// #224: the reference kernel's Dijkstra state, kept between calls so an admin plan (the server's
// tick-sliced `/cavebot zparity` / `zbench` jobs) can run one search across many ticks. A resumed
// search is the SAME search: the state is only ever saved at the top of the settle loop (nothing
// half-done), and legCost is a pure function of (a, b) for a fixed (start, target) and map.
struct ZRefNodeState {
	int32_t dist = std::numeric_limits<int32_t>::max();
	int32_t hops = 0;
	uint32_t parent = UINT32_MAX; // portal idx of predecessor (UINT32_MAX = start)
	bool settled = false;
};
struct ZRefQEntry {
	int32_t dist;
	uint32_t idx;
	bool operator>(const ZRefQEntry& o) const {
		return dist > o.dist;
	}
};
struct ZPlanResume {
	bool active = false; // a saved search is waiting to be continued
	std::unordered_map<uint32_t, ZRefNodeState> states;
	std::priority_queue<ZRefQEntry, std::vector<ZRefQEntry>, std::greater<ZRefQEntry>> open;
	uint32_t goalIdx = UINT32_MAX;
	int32_t goalDist = std::numeric_limits<int32_t>::max();
	uint32_t expansions = 0; // cumulative over the calls (the maxExpansions budget)
	void reset() {
		active = false;
		states.clear();
		open = {};
		goalIdx = UINT32_MAX;
		goalDist = std::numeric_limits<int32_t>::max();
		expansions = 0;
	}
};

struct ZPlanParams {
	int32_t legMax = 80; // max same-floor leg length (Chebyshev) between hops
	int32_t hopCost = 150; // flat cost per traversal (≈ 15 straight steps) — favours fewer hops
	int32_t maxHops = 16; // plans needing more floor changes than this are rejected (live: BotEngine::Z_MAX_HOPS)
	uint32_t maxExpansions = 4096; // Dijkstra settle budget (portals, not tiles)
	// #139: stop at the FIRST settled portal that reaches the target (the pre-D-real behaviour).
	// false = exact: stop when the cheapest complete route is the cheapest thing in the queue.
	// Only botnavsim's `zreal` old arm sets it, to reproduce the deployed planner.
	bool firstGoalStops = false;

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

	// ---- Optional cost counters (#135; nullptr = not counted) ----
	// Settled portals and legCost calls of THIS search. The two numbers that separate the cost of
	// the graph search itself (fan-out: every settle prices every portal within legMax) from the
	// cost of the leg pricer's floods, which the caller times on its own side.
	uint32_t* outExpansions = nullptr;
	uint32_t* outLegCalls = nullptr;

	// #224 (server admin jobs only): when set, a deadline stop saves the search into *resume instead of
	// discarding it, and the next call with the same (start, target) continues it. The deadline is then
	// checked after every settle (a reference settle prices ~1-3k legs), not every 64. outAborted still
	// reports the stop. planZRouteAdj keeps its per-portal state in the caller's ZPlanScratch, so a
	// resumed adjacency search needs that scratch untouched in between (the server gives admin jobs
	// their own).
	ZPlanResume* resume = nullptr;
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
// Queue entry standing for "the best complete route found so far" (#139): never a portal index.
constexpr uint32_t Z_GOAL_SENTINEL = UINT32_MAX - 1;

inline int32_t zCheb(const Position& a, const Position& b) {
	return std::max(
		std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y))
	);
}

template <class LegCost, class Exclude>
bool planZRoute(const ZPortalGraph& g, const Position& start, const Position& target, std::vector<ZRouteHop>& outHops, const ZPlanParams& pp, LegCost&& legCostRaw, Exclude&& exclude) {
	outHops.clear();
	uint32_t expansions = 0;
	uint32_t legCalls = 0;
	// Writes the #135 counters on every exit path (a struct dtor, so no return can skip it).
	struct CounterSink {
		const ZPlanParams& pp;
		const uint32_t& expansions;
		const uint32_t& legCalls;
		~CounterSink() {
			if (pp.outExpansions != nullptr) {
				*pp.outExpansions = expansions;
			}
			if (pp.outLegCalls != nullptr) {
				*pp.outLegCalls = legCalls;
			}
		}
	} counterSink { pp, expansions, legCalls };
	auto legCost = [&](const Position& a, const Position& b) -> int32_t {
		++legCalls;
		return legCostRaw(a, b);
	};

	// #224: a resumable search keeps its state in *pp.resume; otherwise in this call's own.
	ZPlanResume localState;
	ZPlanResume& R = pp.resume != nullptr ? *pp.resume : localState;
	const bool resumed = R.active;
	R.active = false; // set again only by a deadline stop below
	auto& states = R.states;
	auto& open = R.open;
	uint32_t& goalIdx = R.goalIdx;
	int32_t& goalDist = R.goalDist;
	uint32_t settledThisCall = 0;

	if (!resumed) {
	R.reset();
	if (start.z == target.z && zCheb(start, target) <= pp.legMax) {
		const int32_t direct = legCost(start, target);
		if (direct >= 0) {
			return true; // no hop needed
		}
		// else: same floor but not leg-reachable — maybe reachable via another floor
		// (bridges/underpasses). Fall through to graph search.
	}

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
	} // !resumed

	while (!open.empty() && R.expansions < pp.maxExpansions) {
		// Deadline check every 64 settles: frequent enough to bound the overrun to roughly one
		// batch of leg evaluations, rare enough that the clock read is noise. Aborting leaves any
		// floods it computed in the caller's reach cache, so the retry next tick resumes cheaply
		// rather than starting over. #224: a resumable search checks after every settle of this call
		// (at least one, so each call makes progress) and keeps its state.
		if (pp.deadlineMonoMs != 0
		    && (pp.resume != nullptr ? settledThisCall != 0 : ((expansions & 63u) == 0 && expansions != 0))
		    && zSteadyMs() > pp.deadlineMonoMs) {
			if (pp.outAborted != nullptr) {
				*pp.outAborted = true;
			}
			outHops.clear();
			if (pp.resume != nullptr) {
				R.active = true;
			}
			return false;
		}
		auto [d, idx] = open.top();
		open.pop();
		if (idx == Z_GOAL_SENTINEL) {
			break; // #139: the cheapest complete route is settled (see the goal test below)
		}
		auto& st = states[idx];
		if (st.settled || d > st.dist) {
			continue;
		}
		st.settled = true;
		++expansions;
		++R.expansions;
		++settledThisCall;

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
					open.push({ total, Z_GOAL_SENTINEL });
				}
				if (pp.firstGoalStops) {
					break;
				}
				// #139 D-real: do NOT stop at the first settled goal. With Chebyshev final legs that
				// was nearly optimal; with walk-step legs a portal settled early can carry a long
				// final leg while a later one walks 10 tiles. The complete route is queued as a
				// sentinel and the search stops when it is the cheapest thing left (exact Dijkstra).
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

// ============================================================================
// D0 (#139 part 1): precomputed same-floor portal adjacency.
//
// planZRoute above prices, on EVERY settle, every portal inside the legMax box around the landing:
// a radius scan over up to 16 spatial-hash chunks plus one legCost call each, and the server's
// legCost does a clock read, two forced-landing set probes and two component hash lookups per call.
// For #213's pair that is the whole budget even with every flood cached (`progress=0`).
//
// Everything an interior leg depends on is static per graph load: the box membership and the
// component labels of both ends. So the box scan is done ONCE: for each portal i, the portals j inside
// the box around landing(i), in EXACTLY the order forEachOnFloorNear visits them, keeping
//   - same-component pairs (both ends labelled, equal) as plain entries, priced by the caller's cheap
//     interiorCost(i, j) (10 * Chebyshev plus the dynamic forced-landing check);
//   - pairs with an unlabelled end as ADHOC entries, priced by the full legCost, as today;
// and dropping labelled pairs in different components (legCost answers -1 for them, always).
//
// The search is then planZRoute verbatim, with the box scan replaced by the list walk and the state
// map replaced by a dense, generation-stamped array. Pushes happen in the same order with the same
// costs, so the priority queue sees the same sequence and the route is BIT-IDENTICAL -- checked by
// navsim `zparity` and the server's `/cavebot zparity`, not argued. Two inputs could still make an
// interior leg take a different legCost branch, and both fall back to the exact old behaviour:
//   - a settled landing that IS the start position (legCost's `a == from` startReach branch): that
//     one node is expanded by the old box scan;
//   - a target that IS a portal tile (the `b == target` branch on interior legs): the whole search
//     runs planZRoute.
// ============================================================================
struct ZPortalAdjacency {
	static constexpr uint32_t ADHOC = 0x80000000u; // entry must be priced by the full legCost
	// compOf() answer for an unlabelled endpoint that can never be leg-reachable (D0b, below).
	static constexpr int64_t DEAD = -2;
	std::vector<uint32_t> start;                   // size N+1: entries of portal i = [start[i], start[i+1])
	std::vector<uint32_t> to;                      // portal index | ADHOC
	uint32_t adhocEntries = 0;
	uint32_t deadDropped = 0; // entries dropped because one end is DEAD

	bool built() const {
		return !start.empty();
	}
	size_t bytes() const {
		return (start.capacity() + to.capacity()) * sizeof(uint32_t);
	}
	void clear() {
		start.clear();
		to.clear();
		start.shrink_to_fit();
		to.shrink_to_fit();
		adhocEntries = 0;
		deadDropped = 0;
	}
	// compOf(const Position&) -> int64_t component id, -1 when unlabelled, DEAD when unlabelled AND
	// provably unreachable. D0b: an endpoint with no flood-passable cell in its own 3x3 gets an EMPTY
	// flood as an origin (no passable seed), and as a destination can never be in anyone's flood
	// (reachable() accepts the tile or a neighbour, and none of them is passable). So every leg
	// touching it prices -1, and dropping the entry is exact -- while those tiles stay impassable,
	// which compOf checks on the live map at every build. Live: 467 of the 470 unlabelled endpoints,
	// each of which used to cost the OTHER end an ad-hoc flood and a reach-cache slot per plan.
	template <class CompOf>
	void build(const ZPortalGraph& g, int32_t legMax, CompOf&& compOf) {
		const auto& ps = g.portals();
		start.assign(ps.size() + 1, 0);
		to.clear();
		adhocEntries = 0;
		deadDropped = 0;
		std::vector<int64_t> posComp(ps.size());
		for (uint32_t j = 0; j < ps.size(); ++j) {
			posComp[j] = compOf(ps[j].pos);
		}
		for (uint32_t i = 0; i < ps.size(); ++i) {
			start[i] = static_cast<uint32_t>(to.size());
			const int64_t ca = compOf(ps[i].landing);
			g.forEachOnFloorNear(ps[i].landing.z, ps[i].landing, legMax, [&](uint32_t j, const ZPortal&) {
				if (j == i) {
					return;
				}
				const int64_t cb = posComp[j];
				if (ca == DEAD || cb == DEAD) {
					++deadDropped;
				} else if (ca < 0 || cb < 0) {
					to.push_back(j | ADHOC);
					++adhocEntries;
				} else if (ca == cb) {
					to.push_back(j);
				}
			});
		}
		start[ps.size()] = static_cast<uint32_t>(to.size());
		to.shrink_to_fit();
	}
};

// Dense per-search state, reused across searches (generation stamp: no per-search clear).
struct ZPlanScratch {
	std::vector<int32_t> dist;
	std::vector<int32_t> hops;
	std::vector<uint32_t> parent;
	std::vector<uint32_t> stamp;
	std::vector<uint8_t> settled;
	uint32_t gen = 0;

	void begin(size_t n) {
		if (stamp.size() != n) {
			dist.assign(n, 0);
			hops.assign(n, 0);
			parent.assign(n, 0);
			stamp.assign(n, 0);
			settled.assign(n, 0);
			gen = 0;
		}
		if (++gen == 0) {
			std::fill(stamp.begin(), stamp.end(), 0u);
			gen = 1;
		}
	}
	// planZRoute's `states[idx]` default construction, made explicit.
	void touch(uint32_t i) {
		if (stamp[i] != gen) {
			stamp[i] = gen;
			dist[i] = std::numeric_limits<int32_t>::max();
			hops[i] = 0;
			parent[i] = UINT32_MAX;
			settled[i] = 0;
		}
	}
};

// interiorCost(uint32_t fromPortal, uint32_t toPortal, uint32_t entry) -> int32_t: the cost of the leg
// landing(fromPortal) -> pos(toPortal) for a plain (same-component) adjacency entry (`entry` indexes
// adj.to, for per-entry side tables such as the walk steps); < 0 = refuse.
template <class LegCost, class InteriorCost, class Exclude>
bool planZRouteAdj(const ZPortalGraph& g, const ZPortalAdjacency& adj, ZPlanScratch& sc, const Position& start,
                   const Position& target, std::vector<ZRouteHop>& outHops, const ZPlanParams& pp,
                   LegCost&& legCostRaw, InteriorCost&& interiorCostRaw, Exclude&& exclude) {
	// The target sits ON a portal tile: interior legs to it take legCost's targetReach branch, which
	// the adjacency cannot express. Rare (a goto onto a stair) -- run the reference search.
	bool targetIsPortal = false;
	g.forEachOnFloorNear(target.z, target, 0, [&](uint32_t, const ZPortal&) { targetIsPortal = true; });
	if (targetIsPortal || !adj.built() || adj.start.size() != g.size() + 1) {
		return planZRoute(g, start, target, outHops, pp, legCostRaw, exclude);
	}
	outHops.clear();
	uint32_t expansions = 0;
	uint32_t legCalls = 0;
	struct CounterSink {
		const ZPlanParams& pp;
		const uint32_t& expansions;
		const uint32_t& legCalls;
		~CounterSink() {
			if (pp.outExpansions != nullptr) {
				*pp.outExpansions = expansions;
			}
			if (pp.outLegCalls != nullptr) {
				*pp.outLegCalls = legCalls;
			}
		}
	} counterSink { pp, expansions, legCalls };
	auto legCost = [&](const Position& a, const Position& b) -> int32_t {
		++legCalls;
		return legCostRaw(a, b);
	};

	// #224: resumable exactly like planZRoute; the heap and goal live in *pp.resume, the per-portal
	// state in `sc` (which must not be reused by another search in between).
	ZPlanResume localState;
	ZPlanResume& R = pp.resume != nullptr ? *pp.resume : localState;
	const bool resumed = R.active;
	R.active = false;
	auto& open = R.open;
	uint32_t& goalIdx = R.goalIdx;
	int32_t& goalDist = R.goalDist;
	uint32_t settledThisCall = 0;

	if (!resumed) {
	R.reset();
	if (start.z == target.z && zCheb(start, target) <= pp.legMax) {
		const int32_t direct = legCost(start, target);
		if (direct >= 0) {
			return true;
		}
	}

	sc.begin(g.size());

	g.forEachOnFloorNear(start.z, start, pp.legMax, [&](uint32_t idx, const ZPortal& p) {
		if (exclude(p)) {
			return;
		}
		const int32_t c = legCost(start, p.pos);
		if (c < 0) {
			return;
		}
		sc.touch(idx);
		const int32_t nd = c + pp.hopCost;
		if (nd < sc.dist[idx]) {
			sc.dist[idx] = nd;
			sc.hops[idx] = 1;
			sc.parent[idx] = UINT32_MAX;
			open.push({ nd, idx });
		}
	});
	} // !resumed

	while (!open.empty() && R.expansions < pp.maxExpansions) {
		if (pp.deadlineMonoMs != 0
		    && (pp.resume != nullptr ? settledThisCall != 0 : ((expansions & 63u) == 0 && expansions != 0))
		    && zSteadyMs() > pp.deadlineMonoMs) {
			if (pp.outAborted != nullptr) {
				*pp.outAborted = true;
			}
			outHops.clear();
			if (pp.resume != nullptr) {
				R.active = true;
			}
			return false;
		}
		auto [d, idx] = open.top();
		open.pop();
		if (idx == Z_GOAL_SENTINEL) {
			break; // #139: see planZRoute
		}
		sc.touch(idx);
		if (sc.settled[idx] || d > sc.dist[idx]) {
			continue;
		}
		sc.settled[idx] = 1;
		++expansions;
		++R.expansions;
		++settledThisCall;

		const ZPortal& p = g.at(idx);
		const Position& at = p.landing;

		if (at.z == target.z && zCheb(at, target) <= pp.legMax) {
			const int32_t fc = legCost(at, target);
			if (fc >= 0) {
				const int32_t total = sc.dist[idx] + fc;
				if (total < goalDist) {
					goalDist = total;
					goalIdx = idx;
					open.push({ total, Z_GOAL_SENTINEL });
				}
				if (pp.firstGoalStops) {
					break;
				}
			}
		}

		if (sc.hops[idx] >= pp.maxHops) {
			continue;
		}
		const int32_t baseDist = sc.dist[idx];
		const int32_t baseHops = sc.hops[idx];
		auto relax = [&](uint32_t nIdx, int32_t c) {
			sc.touch(nIdx);
			const int32_t nd = baseDist + c + pp.hopCost;
			if (nd < sc.dist[nIdx]) {
				sc.dist[nIdx] = nd;
				sc.hops[nIdx] = baseHops + 1;
				sc.parent[nIdx] = idx;
				open.push({ nd, nIdx });
			}
		};
		if (at == start) {
			// legCost's `a == from` branch prices this node's legs from the start flood: old box scan.
			g.forEachOnFloorNear(at.z, at, pp.legMax, [&](uint32_t nIdx, const ZPortal& np) {
				if (nIdx == idx || exclude(np)) {
					return;
				}
				const int32_t c = legCost(at, np.pos);
				if (c >= 0) {
					relax(nIdx, c);
				}
			});
			continue;
		}
		for (uint32_t e = adj.start[idx], eEnd = adj.start[idx + 1]; e < eEnd; ++e) {
			const uint32_t entry = adj.to[e];
			const uint32_t nIdx = entry & ~ZPortalAdjacency::ADHOC;
			const ZPortal& np = g.at(nIdx);
			if (exclude(np)) {
				continue;
			}
			int32_t c;
			if (entry & ZPortalAdjacency::ADHOC) {
				c = legCost(at, np.pos);
			} else {
				++legCalls;
				c = interiorCostRaw(idx, nIdx, e);
			}
			if (c >= 0) {
				relax(nIdx, c);
			}
		}
	}

	if (goalIdx == UINT32_MAX) {
		return false;
	}
	std::vector<uint32_t> chain;
	for (uint32_t cur = goalIdx; cur != UINT32_MAX; cur = sc.parent[cur]) {
		chain.push_back(cur);
	}
	std::reverse(chain.begin(), chain.end());
	outHops.reserve(chain.size());
	for (uint32_t i : chain) {
		outHops.push_back({ i, g.at(i) });
	}
	return true;
}

// ============================================================================
// B (#137): static flood-passability bitmap.
//
// One bit per map cell with exactly the engine's zFloodPassableAt answer, captured in the zgraph
// sector sweep (which already visits every cell) and persisted in the zcache. A flood then tests a
// bit instead of looking up the sector, taking the floor's shared lock, copying a tile shared_ptr
// and scanning the item list (~1 us/cell measured, #135). Read only by the planner's floods and the
// build-time component labelling; goTo / A* and the FC machine keep validating against live tiles,
// so a transient blocker the bitmap cannot see costs a replan, exactly as the 10-min reach cache
// already does.
//
// Layout: 16x16-cell blocks (Canary's SECTOR_SIZE), 4 uint64 each. Absent and all-impassable
// sector-floors store no block. Each floor has a dense uint32 index over its own sector bounding
// box (0 = no block, else 1-based block id); blocks are stored floor-major, so one floor's blocks are
// a contiguous range (the dense per-floor label arrays of the component pass rely on that).
// Dependency-free so botnavsim can use it as well.
// ============================================================================
struct ZPassBitmap {
	static constexpr int32_t BLOCK = 16;
	static constexpr uint32_t FLOORS = 16;
	struct FloorIndex {
		int32_t sx0 = 0, sy0 = 0, sw = 0, sh = 0; // sector-grid box
		std::vector<uint32_t> idx;                 // sw*sh, 0 = no block, else block id + 1
		uint32_t first = 0, count = 0;             // this floor's block range
	};
	FloorIndex floors[FLOORS];
	std::vector<uint64_t> bits; // 4 words per block

	// Build input, consumed by finalize(): one entry per present, non-empty sector-floor.
	struct Raw {
		int32_t sx = 0, sy = 0; // sector coordinates (cell >> 4)
		uint8_t z = 0;
		uint64_t w[4] = { 0, 0, 0, 0 };
	};
	std::vector<Raw> raw;

	bool built() const {
		return !bits.empty();
	}
	uint32_t blockCount() const {
		return static_cast<uint32_t>(bits.size() / 4);
	}
	size_t bytes() const {
		size_t n = bits.capacity() * sizeof(uint64_t);
		for (const auto& f : floors) {
			n += f.idx.capacity() * sizeof(uint32_t);
		}
		return n;
	}
	void clear() {
		for (auto& f : floors) {
			f = FloorIndex {};
		}
		bits.clear();
		bits.shrink_to_fit();
		raw.clear();
		raw.shrink_to_fit();
	}
	static uint32_t cellOf(int32_t x, int32_t y) {
		return (static_cast<uint32_t>(y & (BLOCK - 1)) << 4) | static_cast<uint32_t>(x & (BLOCK - 1));
	}
	// Global block id holding (x,y,z), or -1.
	int64_t blockOf(int32_t x, int32_t y, int32_t z) const {
		if (static_cast<uint32_t>(z) >= FLOORS || x < 0 || y < 0) {
			return -1;
		}
		const FloorIndex& f = floors[z];
		const int32_t sx = (x >> 4) - f.sx0, sy = (y >> 4) - f.sy0;
		if (static_cast<uint32_t>(sx) >= static_cast<uint32_t>(f.sw) || static_cast<uint32_t>(sy) >= static_cast<uint32_t>(f.sh)) {
			return -1;
		}
		const uint32_t b = f.idx[static_cast<size_t>(sy) * f.sw + sx];
		return b == 0 ? -1 : static_cast<int64_t>(b - 1);
	}
	bool bitAt(int64_t block, uint32_t cell) const {
		return (bits[static_cast<size_t>(block) * 4 + (cell >> 6)] >> (cell & 63)) & 1u;
	}
	bool passable(int32_t x, int32_t y, int32_t z) const {
		const int64_t b = blockOf(x, y, z);
		return b >= 0 && bitAt(b, cellOf(x, y));
	}
	void addRaw(int32_t sx, int32_t sy, uint8_t z, const uint64_t w[4]) {
		if ((w[0] | w[1] | w[2] | w[3]) == 0 || z >= FLOORS) {
			return;
		}
		Raw r;
		r.sx = sx;
		r.sy = sy;
		r.z = z;
		for (int k = 0; k < 4; ++k) {
			r.w[k] = w[k];
		}
		raw.push_back(r);
	}
	// Turns `raw` into the indexed form (block ids in canonical order, see below).
	void finalize() {
		for (auto& f : floors) {
			f = FloorIndex {};
		}
		bits.clear();
		// Canonical order (z, sy, sx) = forEachBlock's order, so a block's id is the same whether the
		// bitmap was swept or read back from the cache -- per-block side tables (ZCompLabels) are
		// persisted by id.
		std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
			return a.z != b.z ? a.z < b.z : (a.sy != b.sy ? a.sy < b.sy : a.sx < b.sx);
		});
		bits.reserve(raw.size() * 4);
		size_t r0 = 0;
		for (uint32_t z = 0; z < FLOORS; ++z) {
			size_t r1 = r0;
			int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
			while (r1 < raw.size() && raw[r1].z == z) {
				x0 = std::min(x0, raw[r1].sx);
				y0 = std::min(y0, raw[r1].sy);
				x1 = std::max(x1, raw[r1].sx);
				y1 = std::max(y1, raw[r1].sy);
				++r1;
			}
			FloorIndex& f = floors[z];
			f.first = static_cast<uint32_t>(bits.size() / 4);
			if (r1 > r0) {
				f.sx0 = x0;
				f.sy0 = y0;
				f.sw = x1 - x0 + 1;
				f.sh = y1 - y0 + 1;
				f.idx.assign(static_cast<size_t>(f.sw) * f.sh, 0);
				for (size_t r = r0; r < r1; ++r) {
					const uint32_t id = static_cast<uint32_t>(bits.size() / 4);
					f.idx[static_cast<size_t>(raw[r].sy - y0) * f.sw + (raw[r].sx - x0)] = id + 1;
					for (int k = 0; k < 4; ++k) {
						bits.push_back(raw[r].w[k]);
					}
				}
			}
			f.count = static_cast<uint32_t>(bits.size() / 4) - f.first;
			r0 = r1;
		}
		raw.clear();
		raw.shrink_to_fit();
	}
	uint64_t passableCells() const {
		uint64_t n = 0;
		for (uint64_t w : bits) {
			n += static_cast<uint64_t>(std::popcount(w));
		}
		return n;
	}
	// Calls fn(sx, sy, z, block id) for every stored block, in block-id order.
	template <class Fn>
	void forEachBlock(Fn&& fn) const {
		for (uint32_t z = 0; z < FLOORS; ++z) {
			const FloorIndex& f = floors[z];
			for (int32_t sy = 0; sy < f.sh; ++sy) {
				for (int32_t sx = 0; sx < f.sw; ++sx) {
					const uint32_t b = f.idx[static_cast<size_t>(sy) * f.sw + sx];
					if (b != 0) {
						fn(f.sx0 + sx, f.sy0 + sy, static_cast<uint8_t>(z), b - 1);
					}
				}
			}
		}
	}
};

// ============================================================================
// D-real (#139 part 2): walk steps on a leg.
//
// A breadth-first flood with the exact membership rule of the engine's LocalReach (a cell joins when
// it is passable and next to an expanded cell; a cell is expanded only while strictly inside the
// Chebyshev radius of the origin), so the set of reached cells is the same as LocalReach's whenever
// the budget covers the radius disc (the planner's does: 30000 >= 161^2). On top of membership it
// records the 8-connected step count from the origin, which is what a leg really costs to walk --
// equal to Chebyshev on open ground, larger around walls (#222: a 19-Chebyshev leg that is 66 steps).
// Window arrays only: no hashing.
// ============================================================================
struct ZStepField {
	static constexpr uint16_t UNREACHED = 0xFFFF;
	int32_t ox = 0, oy = 0, radius = 0, side = 0;
	uint8_t z = 0;
	uint32_t cells = 0;
	std::vector<uint16_t> d;

	size_t bytes() const {
		return d.capacity() * sizeof(uint16_t);
	}
	// pass(x, y) -> bool (floor fz). (sx, sy) = the flood seed: the origin itself, or the first
	// passable neighbour when the origin is not passable (then seedSteps = 1: the step onto it).
	template <class Pass>
	void build(Pass&& pass, int32_t originX, int32_t originY, uint8_t fz, int32_t r, int32_t sx, int32_t sy, uint16_t seedSteps,
	           uint32_t budget) {
		ox = originX;
		oy = originY;
		z = fz;
		radius = r;
		side = 2 * r + 3;
		cells = 0;
		d.assign(static_cast<size_t>(side) * side, UNREACHED);
		const int32_t wx0 = sx - ox + r + 1, wy0 = sy - oy + r + 1;
		if (static_cast<uint32_t>(wx0) >= static_cast<uint32_t>(side) || static_cast<uint32_t>(wy0) >= static_cast<uint32_t>(side)
		    || budget == 0) {
			return;
		}
		static constexpr int32_t kD[8][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
		std::vector<uint32_t> queue;
		queue.reserve(4096);
		d[static_cast<size_t>(wy0) * side + wx0] = seedSteps;
		queue.push_back(static_cast<uint32_t>(wy0) * side + wx0);
		cells = 1;
		for (size_t qi = 0; qi < queue.size() && cells < budget; ++qi) {
			const uint32_t cur = queue[qi];
			const int32_t cwx = static_cast<int32_t>(cur % side), cwy = static_cast<int32_t>(cur / side);
			const int32_t cx = cwx - r - 1 + ox, cy = cwy - r - 1 + oy;
			if (std::max(std::abs(cx - ox), std::abs(cy - oy)) >= r) {
				continue;
			}
			const uint16_t nd = static_cast<uint16_t>(d[cur] + 1);
			for (const auto& k : kD) {
				const int32_t nwx = cwx + k[0], nwy = cwy + k[1];
				const size_t ni = static_cast<size_t>(nwy) * side + nwx; // in the window: |n - origin| <= r
				if (d[ni] != UNREACHED || cells >= budget || !pass(cx + k[0], cy + k[1])) {
					continue;
				}
				d[ni] = nd;
				queue.push_back(static_cast<uint32_t>(ni));
				++cells;
			}
		}
	}
	int32_t at(int32_t x, int32_t y) const {
		const int32_t wx = x - ox + radius + 1, wy = y - oy + radius + 1;
		if (d.empty() || static_cast<uint32_t>(wx) >= static_cast<uint32_t>(side) || static_cast<uint32_t>(wy) >= static_cast<uint32_t>(side)) {
			return -1;
		}
		const uint16_t v = d[static_cast<size_t>(wy) * side + wx];
		return v == UNREACHED ? -1 : v;
	}
	// LocalReach::reachable with a price: p within the radius, and p itself reached (its steps) or a
	// reached neighbour (the cheapest one + the step onto p). -1 = not reachable.
	int32_t stepsTo(const Position& p) const {
		if (p.z != z || std::max(std::abs(static_cast<int32_t>(p.x) - ox), std::abs(static_cast<int32_t>(p.y) - oy)) > radius) {
			return -1;
		}
		const int32_t direct = at(p.x, p.y);
		if (direct >= 0) {
			return direct;
		}
		int32_t best = -1;
		for (int32_t dx = -1; dx <= 1; ++dx) {
			for (int32_t dy = -1; dy <= 1; ++dy) {
				const int32_t v = (dx || dy) ? at(p.x + dx, p.y + dy) : -1;
				if (v >= 0 && (best < 0 || v + 1 < best)) {
					best = v + 1;
				}
			}
		}
		return best;
	}
};

// ============================================================================
// C' (#138): whole-map component labels + a negative-only connectivity oracle.
//
// ZCompLabels keeps the component pass's per-cell labels (global ids: per-floor id + the floor's base)
// instead of discarding them, compressed per bitmap block: a block whose passable cells all share one
// component stores that one id; only "mixed" blocks (walls between rooms) keep 256 ids.
//
// ZCompOracle unions, for every portal, every component in the 3x3 of its tile with every component
// in the 3x3 of its landing -- the cells the planner's legs can attach to that portal through (the
// start/target floods accept a tile OR a neighbour). Directions are ignored and leg lengths are
// ignored, so the oracle's "connected" is a SUPERSET of what the planner can route: its answer is
// trusted only when it says "unreachable". "Reachable" verdicts still come from the real search.
// ============================================================================
struct ZCompLabels {
	static constexpr uint32_t NONE = UINT32_MAX;
	static constexpr uint32_t MIXED = 0x80000000u;
	std::vector<uint32_t> blockLab; // per bitmap block id: uniform label, MIXED | pool slot, or NONE
	std::vector<uint32_t> pool;     // 256 labels per mixed block (NONE for impassable cells)
	uint32_t components = 0;
	uint32_t mixedBlocks = 0;

	bool built() const {
		return !blockLab.empty();
	}
	size_t bytes() const {
		return (blockLab.capacity() + pool.capacity()) * sizeof(uint32_t);
	}
	void clear() {
		blockLab.clear();
		blockLab.shrink_to_fit();
		pool.clear();
		pool.shrink_to_fit();
		components = 0;
		mixedBlocks = 0;
	}
	// Global component of (x,y,z), or -1 (impassable, absent, or a floor that was never labelled).
	int64_t at(const ZPassBitmap& bm, int32_t x, int32_t y, int32_t z) const {
		const int64_t b = bm.blockOf(x, y, z);
		if (b < 0 || static_cast<size_t>(b) >= blockLab.size()) {
			return -1;
		}
		const uint32_t cell = ZPassBitmap::cellOf(x, y);
		if (!bm.bitAt(b, cell)) {
			return -1;
		}
		const uint32_t v = blockLab[static_cast<size_t>(b)];
		if (v == NONE) {
			return -1;
		}
		if (v & MIXED) {
			const uint32_t l = pool[static_cast<size_t>(v & ~MIXED) * 256 + cell];
			return l == NONE ? -1 : static_cast<int64_t>(l);
		}
		return static_cast<int64_t>(v);
	}
	// Store one block's 256 labels (NONE where impassable).
	void setBlock(uint32_t block, const uint32_t* cells) {
		if (blockLab.size() <= block) {
			blockLab.resize(static_cast<size_t>(block) + 1, NONE);
		}
		uint32_t uniform = NONE;
		bool mixed = false;
		for (int c = 0; c < 256; ++c) {
			if (cells[c] == NONE) {
				continue;
			}
			if (uniform == NONE) {
				uniform = cells[c];
			} else if (cells[c] != uniform) {
				mixed = true;
				break;
			}
		}
		if (!mixed) {
			blockLab[block] = uniform;
			return;
		}
		blockLab[block] = MIXED | static_cast<uint32_t>(pool.size() / 256);
		pool.insert(pool.end(), cells, cells + 256);
		++mixedBlocks;
	}
};

struct ZCompOracle {
	std::vector<uint32_t> root; // global component -> representative of its union

	bool built() const {
		return !root.empty();
	}
	void init(uint32_t n) {
		root.resize(n);
		for (uint32_t i = 0; i < n; ++i) {
			root[i] = i;
		}
	}
	uint32_t find(uint32_t a) {
		while (root[a] != a) {
			root[a] = root[root[a]];
			a = root[a];
		}
		return a;
	}
	void unite(uint32_t a, uint32_t b) {
		a = find(a);
		b = find(b);
		if (a != b) {
			root[std::max(a, b)] = std::min(a, b);
		}
	}
	// After all unite() calls: every entry points at its final representative.
	uint32_t flatten() {
		uint32_t groups = 0;
		for (uint32_t i = 0; i < root.size(); ++i) {
			root[i] = find(i);
			groups += root[i] == i ? 1 : 0;
		}
		return groups;
	}
	uint32_t rep(uint32_t a) const {
		return root[a];
	}
};

} // namespace botnav
