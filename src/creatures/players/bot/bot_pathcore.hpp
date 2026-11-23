/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// bot_pathcore.hpp — dependency-free A* pathfinding kernel for bot navigation.
//
// BOT_NAV_REALISM Phase 2. This is deliberately isolated from the game engine:
// it includes ONLY position.hpp (a self-contained POD header) and the standard
// library — NO game.hpp / player.hpp / map.hpp / tile.hpp. All world access is
// abstracted behind a Provider template parameter, so the exact same algorithm
// runs (a) server-side against live tiles via BotTileQueryAdapter, and (b) in
// the offline `tools/botnavsim` simulator against a static map dump.
//
// Cost model is byte-for-byte the server's (AStarNodes):
//   straight step = 10 ; each extra diagonal axis = +25  →  getMapWalkCost().
//   tile extra cost (creature +40, harmful field +180) comes from the Provider.
//
// Provider concept (see BotTileQueryAdapter for the server implementation):
//   int32_t tileWalkCost(const Position& pos) const;
//       Extra walk cost for stepping onto `pos` (>= 0), or < 0 if the tile is
//       not walkable for this bot. Server side this wraps
//       Map::canWalkTo (null => -1, inheriting the botAllowFcPos FC whitelist)
//       + AStarNodes::getTileWalkCost. Offline it reads the dump grid.
//   int32_t jitter(const Position& pos) const;
//       Per-bot deterministic cost noise added to each neighbour (0 when the
//       botNavJitterMask config is 0 — Phase 4a). Kept tiny (<= mask, mask<=7).
//       #198: both providers compute it with botnav::shapeNoise (smooth, habit + per-walk).
//   int32_t blockedNeighbors(const Position& pos) const;   (optional, #198)
//       Count of the 8 neighbours that are statically blocked (wall, no ground). Only
//       consulted when PathParams::wallCost > 0; a Provider without it gets no wall cost.
//   bool blockedAt(const Position& pos) const;   (optional, #198)
//       The same static test for one tile; consulted for PathParams::cornerCost.
//   bool sightClear(const Position& from, const Position& to) const;
//       Only consulted when PathParams::clearSight is true. Server: isSightClear.
//       Offline: dump LOS or always-true. Bot goTo() uses clearSight=false, so
//       this is normally never called.
// ============================================================================

// std headers first: position.hpp is not self-sufficient (it uses uint8_t / std::max /
// std::string with no includes of its own, relying on the PCH in the server build). The
// offline navsim has no PCH, so we must satisfy those here before including it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/movement/position.hpp"

namespace botnav {

// Mirrors AStarNodes cost constants exactly (do not diverge — parity depends on it).
inline constexpr int_fast32_t PC_NORMAL_COST = 10;
inline constexpr int_fast32_t PC_DIAGONAL_COST = 25;
inline constexpr int_fast32_t PC_DEFAULT_MAX_NODES = 512; // server AStarNodes budget
// goTo()'s chunk length (BotEngine::CHUNK_DIST is this value). A walk farther than this searches
// to an intermediate chunk target instead of the true one, so each search fits the 512-node budget.
inline constexpr int32_t PC_CHUNK_DIST = 12;

// The chunk target goTo() aims at when the true target is more than `chunkDist` away: the point
// `chunkDist` tiles along the straight line, same floor. Shared by goTo, `/cavebot route`
// (botTraceLegWalk) and botnavsim `walk`, so the diagnostic and the simulator cannot drift from
// the walker. Signed arithmetic: the engine's old inline copy cast a negative double offset to
// uint16_t, which is undefined behaviour that only worked by wraparound.
inline Position chunkTarget(const Position& from, const Position& to, int32_t chunkDist) {
	const int32_t sdx = static_cast<int32_t>(to.x) - static_cast<int32_t>(from.x);
	const int32_t sdy = static_cast<int32_t>(to.y) - static_cast<int32_t>(from.y);
	const int32_t dist = std::max(std::abs(sdx), std::abs(sdy));
	if (dist <= chunkDist) {
		return to;
	}
	const double ratio = static_cast<double>(chunkDist) / dist;
	Position p = from;
	p.x = static_cast<uint16_t>(static_cast<int32_t>(from.x) + static_cast<int32_t>(sdx * ratio));
	p.y = static_cast<uint16_t>(static_cast<int32_t>(from.y) + static_cast<int32_t>(sdy * ratio));
	return p;
}

// #119: the distances at which goTo() first asks for the WHOLE route to the true target, before it
// falls back to the straight-line chunk. Below the band there is no chunk to replace. Inside it, one
// 512-node search usually solves the walk outright -- and a straight-line chunk target is exactly
// what walks a bot into a dead-end pocket on the near side of a wall (temple -> ammo shop stairs:
// chunked 88 tiles with 10 revisits, the direct route is ~65). Above it a single 512-node search
// rarely completes, so the extra attempt would mostly be a wasted search: the chunk alone runs there.
inline constexpr int32_t PC_DIRECT_FIRST_MAX = 2 * PC_CHUNK_DIST;
inline bool directFirstBand(int32_t chebDist) {
	return chebDist > PC_CHUNK_DIST && chebDist <= PC_DIRECT_FIRST_MAX;
}

// Subset of FindPathParams the kernel needs. Field names/semantics match
// creatures_definitions.hpp FindPathParams so the adapter is a trivial copy.
struct PathParams {
	bool fullPathSearch = true;
	bool allowDiagonal = true; // NOTE: findPath is always 8-connected (matches the stock engine
	                           // pathfinder); this flag is currently not honored. All callers pass true.
	bool keepDistance = false;
	bool clearSight = false;
	int32_t maxSearchDist = 0; // 0 => unbounded (until 100 closed nodes)
	// Default to exact-tile matching so a caller that forgets to set these still finds a path
	// (the stock FindPathParams uses -1, which would match nothing — a footgun; Opus review).
	int32_t minTargetDist = 0;
	int32_t maxTargetDist = 0;
	// #198 path shape (all 0 / stock = the server's exact cost model, so every existing caller and
	// gate is unchanged). turnCost45/turnCost90 are charged when a step changes direction from the
	// step that reached its parent: a staircase E S E S pays a turn per tile, an L pays one. The
	// search stays keyed by tile, not (tile, direction), so it is approximate in the turn dimension
	// -- that is fine, the goal is shape, not optimality. diagonalCost is the extra cost of the
	// second axis of a diagonal step (stock 25: a diagonal = 35 against two straights = 20).
	// wallCost is charged per blocked 8-neighbour of a tile, capped at wallCostCap, so a path keeps
	// a tile off walls and inside corners where the map leaves room (a 1-wide corridor pays it on
	// every tile alike, so it never changes there). Needs the Provider's blockedNeighbors().
	int32_t turnCost45 = 0;
	int32_t turnCost90 = 0;
	int32_t diagonalCost = 25;
	int32_t wallCost = 0;
	int32_t wallCostCap = 0;
	// #198: heuristic in percent of the Manhattan lower bound 10 * (dx + dy); 0 = the stock
	// heuristic 8 * (dx + dy) + 8 * max(dx, dy). The stock max() term is the real source of the
	// staircase: it rewards stepping along whichever axis has more distance left, so once the two
	// are equal the greedy search alternates axes every tile (botnavsim: median 6 turns on one-turn
	// walks with ZERO noise). A plain Manhattan term has no axis preference, so the turn cost picks
	// an L and the noise picks which L. Above 100 the search is greedier (fewer expansions).
	int32_t heuristicPct = 0;
	// #198: extra cost of a 90-degree turn whose INSIDE corner tile is blocked: the taut path that
	// shaves a building corner (34 % of the bots' 90-degree turns, research report 70). Charged only
	// on such turns, so a single tile query per turning expansion. Needs the Provider's blockedAt().
	int32_t cornerCost = 0;
	bool shaped() const {
		return turnCost45 != 0 || turnCost90 != 0 || diagonalCost != 25 || wallCost != 0 || heuristicPct != 0 || cornerCost != 0;
	}
};

// #198: direction of a unit step as an index 0..7 around the compass (E, SE, S, SW, W, NW, N, NE),
// so the turn between two steps is the circular index distance (1 = 45 degrees, 2 = 90 ...).
inline int32_t pcDirIndex(int32_t dx, int32_t dy) {
	static const int8_t lut[3][3] = {
		// dy = -1      0       +1
		{ 5, 4, 3 }, // dx = -1: NW W SW
		{ 6, -1, 2 }, // dx = 0: N . S
		{ 7, 0, 1 }, // dx = +1: NE E SE
	};
	return lut[dx + 1][dy + 1];
}
inline int32_t pcTurnCost(int32_t inDir, int32_t outDir, const PathParams& fpp) {
	if (inDir < 0 || outDir < 0 || inDir == outDir) {
		return 0;
	}
	int32_t d = std::abs(inDir - outDir);
	if (d > 4) {
		d = 8 - d;
	}
	// 45 -> t45, 90 -> t90, 135 -> t90 + t45, 180 -> 2 t90
	return (d / 2) * fpp.turnCost90 + (d % 2) * fpp.turnCost45;
}

// #198: smooth (value) noise for the path cost, replacing per-tile white noise. White noise on a
// tie-rich cost surface (every monotone path across a plaza costs the same) picks a random
// interleaving of E and S steps -- a staircase. Value noise varies over `scale` tiles, so two
// equal-length routes differ by WHICH SIDE of the plaza they take, not by tile-level zigzag.
// Two fields are blended: a per-bot "habit" field (fixed for the session) and a per-walk field,
// so one bot walks about the same way each time but not tile for tile.
struct NoiseParams {
	uint32_t habitSeed = 0;
	uint32_t walkSeed = 0;
	int32_t amp = 0; // result range [0, amp]; 0 = no noise
	int32_t scale = 0; // lattice spacing in tiles; <= 1 = legacy white noise (h & amp, amp = mask)
	int32_t habitPct = 100; // weight of the habit field, 0..100
};
inline uint32_t pcHash(uint32_t seed, uint32_t x, uint32_t y) {
	uint32_t h = seed ^ (x * 0x9E3779B1u) ^ (y * 0x85EBCA77u);
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return h;
}
// Bilinear value noise in [0, 255] at lattice spacing s (s >= 2).
inline int32_t pcValueNoise255(uint32_t seed, uint32_t x, uint32_t y, int32_t s) {
	const uint32_t gx = x / static_cast<uint32_t>(s), gy = y / static_cast<uint32_t>(s);
	const int32_t fx = static_cast<int32_t>(x - gx * static_cast<uint32_t>(s));
	const int32_t fy = static_cast<int32_t>(y - gy * static_cast<uint32_t>(s));
	const int32_t v00 = static_cast<int32_t>(pcHash(seed, gx, gy) & 255u);
	const int32_t v10 = static_cast<int32_t>(pcHash(seed, gx + 1, gy) & 255u);
	const int32_t v01 = static_cast<int32_t>(pcHash(seed, gx, gy + 1) & 255u);
	const int32_t v11 = static_cast<int32_t>(pcHash(seed, gx + 1, gy + 1) & 255u);
	const int32_t top = v00 * (s - fx) + v10 * fx;
	const int32_t bot = v01 * (s - fx) + v11 * fx;
	return (top * (s - fy) + bot * fy) / (s * s);
}
inline int32_t shapeNoise(const NoiseParams& np, uint32_t x, uint32_t y) {
	if (np.amp <= 0) {
		return 0;
	}
	if (np.scale <= 1) {
		// Legacy per-tile white noise, bit-identical to the pre-#198 jitter(): h & mask.
		return static_cast<int32_t>(pcHash(np.habitSeed, x, y) & static_cast<uint32_t>(np.amp));
	}
	int32_t v = pcValueNoise255(np.habitSeed, x, y, np.scale);
	if (np.habitPct < 100) {
		const int32_t w = pcValueNoise255(np.walkSeed ^ 0xA5A5F00Du, x, y, np.scale);
		v = (v * np.habitPct + w * (100 - np.habitPct)) / 100;
	}
	return (v * np.amp + 127) / 255;
}

struct PathNode {
	PathNode* parent;
	int_fast32_t f; // g-cost so far
	int_fast32_t g; // heuristic
	int_fast32_t c; // this tile's extra cost (cached for neighbour reuse)
	uint16_t x, y;
};

// Reusable open/closed node pool. One instance is owned by the engine (and by
// the navsim) and reset() per search, so there is no per-call heap allocation
// of the node array — addressing the Opus-review concern about the old
// per-call 4096-node BotAStarNodes.
class PathNodePool {
public:
	explicit PathNodePool(int32_t maxNodes = PC_DEFAULT_MAX_NODES) :
		maxNodes_(maxNodes), nodes_(static_cast<size_t>(maxNodes)), isOpen_(static_cast<size_t>(maxNodes), false) { }

	void reset(uint16_t x, uint16_t y, int_fast32_t extraCost) {
		curNode_ = 1;
		closedNodes_ = 0;
		posMap_.clear();
		std::fill(isOpen_.begin(), isOpen_.end(), false);
		while (!openHeap_.empty()) {
			openHeap_.pop();
		}
		auto& startNode = nodes_[0];
		startNode.parent = nullptr;
		startNode.x = x;
		startNode.y = y;
		startNode.f = 0;
		startNode.g = 0;
		startNode.c = extraCost;
		posMap_[key(x, y)] = 0;
		isOpen_[0] = true;
		openHeap_.push({ 0, 0 });
	}

	bool createOpenNode(PathNode* parent, uint16_t x, uint16_t y, int_fast32_t f, int_fast32_t heuristic, int_fast32_t extraCost) {
		if (curNode_ >= maxNodes_) {
			return false;
		}
		const int32_t idx = curNode_++;
		auto& node = nodes_[idx];
		node.parent = parent;
		node.x = x;
		node.y = y;
		node.f = f;
		node.g = heuristic;
		node.c = extraCost;
		posMap_[key(x, y)] = idx;
		isOpen_[idx] = true;
		openHeap_.push({ f + heuristic, idx });
		return true;
	}

	PathNode* getBestNode() {
		while (!openHeap_.empty()) {
			auto [cost, idx] = openHeap_.top();
			openHeap_.pop();
			if (idx < curNode_ && isOpen_[idx]) {
				const int_fast32_t curCost = nodes_[idx].f + nodes_[idx].g;
				if (curCost <= cost) {
					return &nodes_[idx];
				}
				openHeap_.push({ curCost, idx });
			}
		}
		return nullptr;
	}

	void closeNode(const PathNode* node) {
		isOpen_[static_cast<size_t>(node - &nodes_[0])] = false;
		closedNodes_++;
	}

	void openNode(const PathNode* node) {
		const size_t idx = static_cast<size_t>(node - &nodes_[0]);
		isOpen_[idx] = true;
		openHeap_.push({ nodes_[idx].f + nodes_[idx].g, static_cast<int32_t>(idx) });
	}

	int32_t getClosedNodes() const {
		return closedNodes_;
	}

	PathNode* getNodeByPosition(uint16_t x, uint16_t y) {
		auto it = posMap_.find(key(x, y));
		return it != posMap_.end() ? &nodes_[it->second] : nullptr;
	}

private:
	static uint32_t key(uint16_t x, uint16_t y) {
		return (static_cast<uint32_t>(x) << 16) | y;
	}
	struct HeapEntry {
		int_fast32_t cost;
		int32_t idx;
		bool operator>(const HeapEntry& o) const {
			return cost > o.cost;
		}
	};

	int32_t maxNodes_;
	std::vector<PathNode> nodes_;
	std::vector<bool> isOpen_;
	std::unordered_map<uint32_t, int32_t> posMap_;
	std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> openHeap_;
	int32_t closedNodes_ = 0;
	int32_t curNode_ = 0;
};

// Exact replica of AStarNodes::getMapWalkCost (straight 10, diagonal +25/axis).
inline int_fast32_t getMapWalkCost(uint16_t nodeX, uint16_t nodeY, uint16_t nx, uint16_t ny) {
	return (((std::abs(static_cast<int>(nodeX) - static_cast<int>(nx)) + std::abs(static_cast<int>(nodeY) - static_cast<int>(ny))) - 1) * PC_DIAGONAL_COST) + PC_NORMAL_COST;
}

// Replica of FrozenPathingConditionCall::isInRange (position-only; no game deps).
inline bool pcInRange(const Position& startPos, const Position& testPos, const Position& targetPos, const PathParams& fpp) {
	if (fpp.fullPathSearch) {
		if (testPos.x > targetPos.x + fpp.maxTargetDist || testPos.x < targetPos.x - fpp.maxTargetDist
			|| testPos.y > targetPos.y + fpp.maxTargetDist || testPos.y < targetPos.y - fpp.maxTargetDist) {
			return false;
		}
	} else {
		const int_fast32_t dx = Position::getOffsetX(startPos, targetPos);
		const int32_t dxMax = (dx >= 0 ? fpp.maxTargetDist : 0);
		if (testPos.x > targetPos.x + dxMax) {
			return false;
		}
		const int32_t dxMin = (dx <= 0 ? fpp.maxTargetDist : 0);
		if (testPos.x < targetPos.x - dxMin) {
			return false;
		}
		const int_fast32_t dy = Position::getOffsetY(startPos, targetPos);
		const int32_t dyMax = (dy >= 0 ? fpp.maxTargetDist : 0);
		if (testPos.y > targetPos.y + dyMax) {
			return false;
		}
		const int32_t dyMin = (dy <= 0 ? fpp.maxTargetDist : 0);
		if (testPos.y < targetPos.y - dyMin) {
			return false;
		}
	}
	return true;
}

// Replica of FrozenPathingConditionCall::operator() with the LOS check routed
// through the Provider (only when clearSight is set).
template <class Provider>
inline bool pcTargetReached(const Position& startPos, const Position& testPos, const Position& targetPos, const PathParams& fpp, int32_t& bestMatchDist, const Provider& prov) {
	if (!pcInRange(startPos, testPos, targetPos, fpp)) {
		return false;
	}
	if (fpp.clearSight && !prov.sightClear(testPos, targetPos)) {
		return false;
	}
	const int32_t testDist = std::max<int32_t>(Position::getDistanceX(targetPos, testPos), Position::getDistanceY(targetPos, testPos));
	if (fpp.maxTargetDist == 1) {
		return !(testDist < fpp.minTargetDist || testDist > fpp.maxTargetDist);
	}
	if (testDist <= fpp.maxTargetDist) {
		if (testDist < fpp.minTargetDist) {
			return false;
		}
		if (testDist == fpp.maxTargetDist) {
			bestMatchDist = 0;
			return true;
		}
		if (testDist > bestMatchDist) {
			bestMatchDist = testDist;
			return true;
		}
	}
	return false;
}

// Core A*. Fills outPath with the tile sequence from start (exclusive) to the
// matched end tile (inclusive), forward order. Returns false if no path.
// Algorithm mirrors bot_engine's botGetPathMatchingCond / Map::getPathMatchingCond.
template <class Provider>
bool findPath(const Position& startPos, const Position& targetPos, std::vector<Position>& outPath, const PathParams& fpp, const Provider& prov, PathNodePool& pool) {
	outPath.clear();
	// Neighbour offset tables: index 0 = the 5 forward-biased dirs given a parent
	// direction; the fallback all-8 used at the start node. Order matches the
	// server's DIRECTION_* enum (W,N,E,S,NW... = the layout in bot_engine).
	static const int_fast32_t dirNeighbors[8][5][2] = {
		{ { -1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { -1, 1 } },
		{ { -1, 0 }, { 0, 1 }, { 0, -1 }, { -1, -1 }, { -1, 1 } },
		{ { -1, 0 }, { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 } },
		{ { 0, 1 }, { 1, 0 }, { 0, -1 }, { 1, -1 }, { 1, 1 } },
		{ { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { 1, 1 } },
		{ { -1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } },
		{ { 0, 1 }, { 1, 0 }, { 1, -1 }, { 1, 1 }, { -1, 1 } },
		{ { -1, 0 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } }
	};
	static const int_fast32_t allNeighbors[8][2] = {
		{ -1, 0 }, { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 }
	};
	// dirNeighbors rows are indexed by the real Direction enum (position.hpp):
	// NORTH=0, EAST=1, SOUTH=2, WEST=3, SOUTHWEST=4, SOUTHEAST=5, NORTHWEST=6, NORTHEAST=7.

	Position pos = startPos;
	const int32_t startCost = prov.tileWalkCost(startPos);
	pool.reset(static_cast<uint16_t>(startPos.x), static_cast<uint16_t>(startPos.y), startCost < 0 ? 0 : startCost);

	int32_t bestMatch = 0;
	const Position start = startPos;
	const int_fast32_t sX = std::abs(targetPos.getX() - startPos.getX());
	const int_fast32_t sY = std::abs(targetPos.getY() - startPos.getY());

	const PathNode* found = nullptr;
	const bool shaped = fpp.shaped();
	do {
		PathNode* n = pool.getBestNode();
		if (!n) {
			if (found) {
				break;
			}
			return false;
		}

		const int_fast32_t x = n->x;
		const int_fast32_t y = n->y;
		pos.x = static_cast<uint16_t>(x);
		pos.y = static_cast<uint16_t>(y);
		if (pcTargetReached(start, pos, targetPos, fpp, bestMatch, prov)) {
			found = n;
			if (bestMatch == 0) {
				break;
			}
		}

		uint_fast32_t dirCount;
		const int_fast32_t* neighbors;
		if (n->parent) {
			const int_fast32_t offsetX = n->parent->x - x;
			const int_fast32_t offsetY = n->parent->y - y;
			if (offsetY == 0) {
				neighbors = *dirNeighbors[offsetX == -1 ? DIRECTION_WEST : DIRECTION_EAST];
			} else if (offsetX == 0) {
				neighbors = *dirNeighbors[offsetY == -1 ? DIRECTION_NORTH : DIRECTION_SOUTH];
			} else if (offsetY == -1) {
				neighbors = *dirNeighbors[offsetX == -1 ? DIRECTION_NORTHWEST : DIRECTION_NORTHEAST];
			} else {
				neighbors = *dirNeighbors[offsetX == -1 ? DIRECTION_SOUTHWEST : DIRECTION_SOUTHEAST];
			}
			dirCount = 5;
		} else {
			dirCount = 8;
			neighbors = *allNeighbors;
		}

		const int_fast32_t f = n->f;
		// #198: the direction of the step that reached n (-1 at the start node: no turn is charged
		// for the first step).
		const int32_t inDir = n->parent ? pcDirIndex(static_cast<int32_t>(x - n->parent->x), static_cast<int32_t>(y - n->parent->y)) : -1;
		for (uint_fast32_t i = 0; i < dirCount; ++i) {
			pos.x = static_cast<uint16_t>(x + *neighbors++);
			pos.y = static_cast<uint16_t>(y + *neighbors++);
			if (fpp.maxSearchDist != 0 && (Position::getDistanceX(start, pos) > fpp.maxSearchDist || Position::getDistanceY(start, pos) > fpp.maxSearchDist)) {
				continue;
			}
			if (fpp.keepDistance && !pcInRange(start, pos, targetPos, fpp)) {
				continue;
			}

			int_fast32_t extraCost;
			PathNode* neighborNode = pool.getNodeByPosition(static_cast<uint16_t>(pos.x), static_cast<uint16_t>(pos.y));
			if (neighborNode) {
				extraCost = neighborNode->c;
			} else {
				const int32_t wc = prov.tileWalkCost(pos);
				if (wc < 0) {
					continue; // unwalkable
				}
				extraCost = wc + prov.jitter(pos);
				if constexpr (requires { prov.blockedNeighbors(pos); }) {
					if (fpp.wallCost > 0) {
						extraCost += std::min<int_fast32_t>(fpp.wallCostCap, static_cast<int_fast32_t>(fpp.wallCost) * prov.blockedNeighbors(pos));
					}
				}
			}

			int_fast32_t cost;
			if (shaped) {
				const int32_t sdx = static_cast<int32_t>(pos.x) - static_cast<int32_t>(x);
				const int32_t sdy = static_cast<int32_t>(pos.y) - static_cast<int32_t>(y);
				cost = (sdx != 0 && sdy != 0) ? PC_NORMAL_COST + fpp.diagonalCost : PC_NORMAL_COST;
				const int32_t outDir = pcDirIndex(sdx, sdy);
				cost += pcTurnCost(inDir, outDir, fpp);
				if constexpr (requires { prov.blockedAt(pos); }) {
					if (fpp.cornerCost > 0) {
						if (sdx != 0 && sdy != 0) {
							// A diagonal step shaves BOTH orthogonal neighbours it passes between.
							if (prov.blockedAt(Position(static_cast<uint16_t>(x + sdx), static_cast<uint16_t>(y), pos.z))
							    || prov.blockedAt(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y + sdy), pos.z))) {
								cost += fpp.cornerCost;
							}
						} else if (inDir >= 0 && (inDir % 2) == 0 && ((inDir - outDir + 8) % 4) == 2) {
							// A 90-degree turn between two straight steps: the inside corner is pos - (incoming step).
							const Position corner(static_cast<uint16_t>(pos.x - (x - n->parent->x)), static_cast<uint16_t>(pos.y - (y - n->parent->y)), pos.z);
							if (prov.blockedAt(corner)) {
								cost += fpp.cornerCost;
							}
						}
					}
				}
			} else {
				cost = getMapWalkCost(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(pos.x), static_cast<uint16_t>(pos.y));
			}
			const int_fast32_t newf = f + cost + extraCost;
			if (neighborNode) {
				if (neighborNode->f <= newf) {
					continue;
				}
				neighborNode->f = newf;
				neighborNode->parent = n;
				pool.openNode(neighborNode);
			} else {
				const int_fast32_t dX = std::abs(targetPos.getX() - pos.getX());
				const int_fast32_t dY = std::abs(targetPos.getY() - pos.getY());
				const int_fast32_t h = fpp.heuristicPct > 0
					? ((dX + dY) * PC_NORMAL_COST * fpp.heuristicPct) / 100
					: ((dX - sX) << 3) + ((dY - sY) << 3) + (std::max(dX, dY) << 3);
				if (!pool.createOpenNode(n, static_cast<uint16_t>(pos.x), static_cast<uint16_t>(pos.y), newf, h, extraCost)) {
					if (found) {
						break;
					}
					return false;
				}
			}
		}
		pool.closeNode(n);
	} while (fpp.maxSearchDist != 0 || pool.getClosedNodes() < 100);

	if (!found) {
		return false;
	}

	// Walk parents back to start, then reverse into forward order.
	for (const PathNode* it = found; it && it->parent; it = it->parent) {
		outPath.emplace_back(static_cast<uint16_t>(it->x), static_cast<uint16_t>(it->y), startPos.z);
	}
	std::reverse(outPath.begin(), outPath.end());
	return true;
}

} // namespace botnav
