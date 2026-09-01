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
};

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
			}

			const int_fast32_t cost = getMapWalkCost(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(pos.x), static_cast<uint16_t>(pos.y));
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
				if (!pool.createOpenNode(n, static_cast<uint16_t>(pos.x), static_cast<uint16_t>(pos.y), newf, ((dX - sX) << 3) + ((dY - sY) << 3) + (std::max(dX, dY) << 3), extraCost)) {
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
