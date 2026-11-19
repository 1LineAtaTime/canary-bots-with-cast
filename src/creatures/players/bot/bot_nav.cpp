/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_nav.cpp — A* pathfinder pool, pathcore adapter, navigation + door helpers
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
// Bot-specific A* pathfinder with 4096-node pool (8x server default of 512)
// ============================================================================

// Standalone A* node pool for bot pathfinding — heap-allocated, 4096 nodes (8x server's 512).
// Uses a min-heap (priority queue) for getBestNode instead of linear scan — O(log n) vs O(n).
// Also uses an unordered_map for O(1) position lookup instead of linear scan.
class BotAStarNodes {
public:
	static constexpr int32_t BOT_MAX_NODES = 4096;

	// `maxNodes` is runtime-settable so a caller can reproduce the STOCK server budget
	// (Map::getPathMatchingCond uses 512) instead of the bot pool's 4096. The `/cavebot route`
	// diagnostic depends on this: tracing a leg with a bigger budget than the live walker
	// actually gets would report routes the bot cannot walk, which is precisely the kind of
	// diagnostic-vs-live divergence this command exists to eliminate.
	explicit BotAStarNodes(uint32_t x, uint32_t y, int_fast32_t extraCost, int32_t maxNodes = BOT_MAX_NODES)
		: nodes_(maxNodes), maxNodes_(maxNodes) {
		curNode_ = 1;
		closedNodes_ = 0;

		auto& startNode = nodes_[0];
		startNode.parent = nullptr;
		startNode.x = x;
		startNode.y = y;
		startNode.f = 0;
		startNode.g = 0;
		startNode.c = extraCost;

		uint32_t key = (x << 16) | y;
		posMap_[key] = 0;
		openHeap_.push({0, 0}); // {cost, index}
		isOpen_.resize(maxNodes, false);
		isOpen_[0] = true;
	}

	bool createOpenNode(AStarNode* parent, uint32_t x, uint32_t y, int_fast32_t f, int_fast32_t heuristic, int_fast32_t extraCost) {
		if (curNode_ >= maxNodes_) return false;
		const int32_t retNode = curNode_++;
		auto& node = nodes_[retNode];
		node.parent = parent;
		node.x = x;
		node.y = y;
		node.f = f;
		node.g = heuristic;
		node.c = extraCost;

		uint32_t key = (x << 16) | y;
		posMap_[key] = retNode;
		isOpen_[retNode] = true;
		openHeap_.push({f + heuristic, retNode});
		return true;
	}

	AStarNode* getBestNode() {
		// Pop from min-heap, skip closed/stale entries
		while (!openHeap_.empty()) {
			auto [cost, idx] = openHeap_.top();
			openHeap_.pop();
			if (idx < curNode_ && isOpen_[idx]) {
				// Verify cost is current (node may have been re-opened with lower cost)
				int_fast32_t curCost = nodes_[idx].f + nodes_[idx].g;
				if (curCost <= cost) {
					return &nodes_[idx];
				}
				// Stale entry — re-push with current cost
				openHeap_.push({curCost, idx});
			}
		}
		return nullptr;
	}

	void closeNode(const AStarNode* node) {
		size_t idx = node - &nodes_[0];
		isOpen_[idx] = false;
		closedNodes_++;
	}

	void openNode(const AStarNode* node) {
		size_t idx = node - &nodes_[0];
		isOpen_[idx] = true;
		openHeap_.push({nodes_[idx].f + nodes_[idx].g, static_cast<int32_t>(idx)});
	}

	int32_t getClosedNodes() const { return closedNodes_; }

	AStarNode* getNodeByPosition(uint32_t x, uint32_t y) {
		uint32_t key = (x << 16) | y;
		auto it = posMap_.find(key);
		if (it != posMap_.end()) return &nodes_[it->second];
		return nullptr;
	}

private:
	struct HeapEntry {
		int_fast32_t cost;
		int32_t idx;
		bool operator>(const HeapEntry& o) const { return cost > o.cost; }
	};

	std::vector<AStarNode> nodes_;
	std::vector<bool> isOpen_;
	std::unordered_map<uint32_t, int32_t> posMap_;
	std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> openHeap_;
	int32_t closedNodes_ = 0;
	int32_t curNode_ = 0;
	int32_t maxNodes_ = BOT_MAX_NODES;
};

// Bot-specific pathfinding using the larger 4096-node pool.
// Adapted from Map::getPathMatchingCond — identical logic, just uses BotAStarNodes.
// `startOverride` lets a caller trace a leg the bot is not currently standing on (the
// `/cavebot route` diagnostic plans every leg of a multi-hop route in one shot). `creature`
// stays the mover for walkability, so results still reflect that bot's own tile permissions.
static bool botGetPathMatchingCond(
	const std::shared_ptr<Creature>& creature,
	const Position& targetPos,
	std::vector<Direction>& dirList,
	const FrozenPathingConditionCall& pathCondition,
	const FindPathParams& fpp,
	int32_t maxNodes = BotAStarNodes::BOT_MAX_NODES,
	const Position* startOverride = nullptr)
{
	Position pos = startOverride ? *startOverride : creature->getPosition();
	Position endPos;

	BotAStarNodes nodes(pos.x, pos.y, AStarNodes::getTileWalkCost(creature, g_game().map.getTile(pos.x, pos.y, pos.z)), maxNodes);

	int32_t bestMatch = 0;

	static int_fast32_t dirNeighbors[8][5][2] = {
		{ { -1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { -1, 1 } },
		{ { -1, 0 }, { 0, 1 }, { 0, -1 }, { -1, -1 }, { -1, 1 } },
		{ { -1, 0 }, { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 } },
		{ { 0, 1 }, { 1, 0 }, { 0, -1 }, { 1, -1 }, { 1, 1 } },
		{ { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { 1, 1 } },
		{ { -1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } },
		{ { 0, 1 }, { 1, 0 }, { 1, -1 }, { 1, 1 }, { -1, 1 } },
		{ { -1, 0 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } }
	};

	static int_fast32_t allNeighbors[8][2] = {
		{ -1, 0 }, { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 }
	};

	const Position startPos = pos;
	const int_fast32_t sX = std::abs(targetPos.getX() - pos.getX());
	const int_fast32_t sY = std::abs(targetPos.getY() - pos.getY());

	uint_fast16_t cntDirs = 0;
	const AStarNode* found = nullptr;

	do {
		AStarNode* n = nodes.getBestNode();
		if (!n) {
			if (found) break;
			return false;
		}

		const int_fast32_t x = n->x;
		const int_fast32_t y = n->y;
		pos.x = x;
		pos.y = y;
		if (pathCondition(startPos, pos, fpp, bestMatch)) {
			found = n;
			endPos = pos;
			if (bestMatch == 0) break;
		}

		++cntDirs;

		uint_fast32_t dirCount;
		int_fast32_t* neighbors;
		if (n->parent) {
			const int_fast32_t offset_x = n->parent->x - x;
			const int_fast32_t offset_y = n->parent->y - y;
			if (offset_y == 0) {
				neighbors = offset_x == -1 ? *dirNeighbors[DIRECTION_WEST] : *dirNeighbors[DIRECTION_EAST];
			} else if (offset_x == 0) {
				neighbors = offset_y == -1 ? *dirNeighbors[DIRECTION_NORTH] : *dirNeighbors[DIRECTION_SOUTH];
			} else if (offset_y == -1) {
				neighbors = offset_x == -1 ? *dirNeighbors[DIRECTION_NORTHWEST] : *dirNeighbors[DIRECTION_NORTHEAST];
			} else {
				neighbors = offset_x == -1 ? *dirNeighbors[DIRECTION_SOUTHWEST] : *dirNeighbors[DIRECTION_SOUTHEAST];
			}
			dirCount = 5;
		} else {
			dirCount = 8;
			neighbors = *allNeighbors;
		}

		const int_fast32_t f = n->f;
		for (uint_fast32_t i = 0; i < dirCount; ++i) {
			pos.x = x + *neighbors++;
			pos.y = y + *neighbors++;
			if (fpp.maxSearchDist != 0 && (Position::getDistanceX(startPos, pos) > fpp.maxSearchDist || Position::getDistanceY(startPos, pos) > fpp.maxSearchDist)) {
				continue;
			}
			if (fpp.keepDistance && !pathCondition.isInRange(startPos, pos, fpp)) {
				continue;
			}

			int_fast32_t extraCost;
			AStarNode* neighborNode = nodes.getNodeByPosition(pos.x, pos.y);
			if (neighborNode) {
				extraCost = neighborNode->c;
			} else {
				const auto& tile = g_game().map.canWalkTo(creature, pos);
				if (!tile) continue;
				extraCost = AStarNodes::getTileWalkCost(creature, tile);
			}

			const int_fast32_t cost = AStarNodes::getMapWalkCost(n, pos);
			const int_fast32_t newf = f + cost + extraCost;
			if (neighborNode) {
				if (neighborNode->f <= newf) continue;
				neighborNode->f = newf;
				neighborNode->parent = n;
				nodes.openNode(neighborNode);
			} else {
				const int_fast32_t dX = std::abs(targetPos.getX() - pos.getX());
				const int_fast32_t dY = std::abs(targetPos.getY() - pos.getY());
				if (!nodes.createOpenNode(n, pos.x, pos.y, newf, ((dX - sX) << 3) + ((dY - sY) << 3) + (std::max(dX, dY) << 3), extraCost)) {
					if (found) break;
					return false;
				}
			}
		}
		nodes.closeNode(n);
	} while (fpp.maxSearchDist != 0 || nodes.getClosedNodes() < 100);

	if (!found) return false;

	int_fast32_t prevx = endPos.x;
	int_fast32_t prevy = endPos.y;
	dirList.reserve(cntDirs);

	found = found->parent;
	while (found) {
		pos.x = found->x;
		pos.y = found->y;
		const int_fast32_t dx = pos.getX() - prevx;
		const int_fast32_t dy = pos.getY() - prevy;
		prevx = pos.x;
		prevy = pos.y;
		if (dx == 1) {
			dirList.emplace_back(dy == 1 ? DIRECTION_NORTHWEST : dy == -1 ? DIRECTION_SOUTHWEST : DIRECTION_WEST);
		} else if (dx == -1) {
			dirList.emplace_back(dy == 1 ? DIRECTION_NORTHEAST : dy == -1 ? DIRECTION_SOUTHEAST : DIRECTION_EAST);
		} else if (dy == 1) {
			dirList.emplace_back(DIRECTION_NORTH);
		} else if (dy == -1) {
			dirList.emplace_back(DIRECTION_SOUTH);
		}
		found = found->parent;
	}
	return true;
}

// ---- /cavebot route: trace the exact tiles the live walker would take on ONE leg ----
// Mirrors goTo()'s FindPathParams and, critically, the STOCK 512-node budget that
// Map::getPathMatchingCond gives the real walker — not the 4096-node bot pool. A leg that
// fails here is therefore a leg the bot genuinely cannot walk, which is the whole point of
// the command: reproduce live reachability instead of an optimistic approximation of it.
//
// `outTiles` receives every tile of the walk, start-exclusive and end-inclusive.
bool BotEngine::botTraceLegPath(const std::shared_ptr<Player>& mover, const Position& from, const Position& to,
	int32_t arrivalDist, std::vector<Position>& outTiles, bool wide) {
	outTiles.clear();
	if (!mover) return false;

	FindPathParams fpp;
	fpp.fullPathSearch = true;
	fpp.clearSight = false;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = wide ? PATH_WIDE_DIST : PATH_MAX_DIST;
	fpp.minTargetDist = 0;
	fpp.maxTargetDist = arrivalDist;

	// Let A* land on the destination tile itself when it is a walk-on floor-change tile — the
	// same whitelist goTo() applies. Without it every stair/ladder portal reads as unreachable,
	// which is exactly the false negative that made the offline dump disagree with the server.
	const bool allowFc = (arrivalDist == 0 && isWalkOnFcTile(to));
	if (allowFc) mover->setBotAllowFcPos(to);

	std::vector<Direction> dirList;
	const bool ok = botGetPathMatchingCond(mover, to, dirList, FrozenPathingConditionCall(to), fpp,
		wide ? BotAStarNodes::BOT_MAX_NODES : STOCK_PATH_MAX_NODES, &from);
	if (allowFc) mover->clearBotAllowFcPos();
	if (!ok) return false;

	// Directions come out end-first (the walker pops from the back); replay them from `from`.
	Position at = from;
	for (auto it = dirList.rbegin(); it != dirList.rend(); ++it) {
		at = getNextPosition(*it, at);
		outTiles.push_back(at);
	}
	return true;
}

// Replay goTo()'s ACTUAL walk over a whole leg: chunk toward the target, advance, re-path,
// repeat. Tracing a leg as one full A* call would under-report reachability, because the live
// walker never solves a long leg in a single search — it solves a 12-tile chunk, walks it, and
// searches again from there. A 40-tile leg that no single 512-node search can crack is still
// perfectly walkable this way, so a one-shot trace would report a false failure.
//
// Mirrors goTo()'s chunk arithmetic and its true-target retry so the diagnostic and the walker
// stay behaviorally identical. `outTiles` accumulates the full tile-by-tile walk.
bool BotEngine::botTraceLegWalk(const std::shared_ptr<Player>& mover, const Position& from, const Position& to,
	int32_t arrivalDist, std::vector<Position>& outTiles, std::string& outNote) {
	outTiles.clear();
	outNote.clear();
	Position at = from;
	int chunks = 0;
	// A leg needs ceil(dist/CHUNK_DIST) searches; 64 is far past any legitimate leg and just
	// stops a pathological loop from hanging the dispatcher.
	constexpr int MAX_CHUNKS = 64;

	while (botnav::zCheb(at, to) > arrivalDist) {
		if (++chunks > MAX_CHUNKS) {
			outNote = fmt::format("gave up after {} chunks (still {} tiles out)", MAX_CHUNKS, botnav::zCheb(at, to));
			return false;
		}
		const int32_t sdx = static_cast<int32_t>(to.x) - static_cast<int32_t>(at.x);
		const int32_t sdy = static_cast<int32_t>(to.y) - static_cast<int32_t>(at.y);
		const int32_t dist = std::max(std::abs(sdx), std::abs(sdy));

		std::vector<Position> seg;
		// #119: goTo()'s direct-first attempt, same band (botnav::directFirstBand), as the plain walk
		// callers that opt in (GoToDirectFirstScope) get it. No limiter here: a trace runs each leg
		// once. Traced with the stock pathfinder; the live attempt uses the jitter kernel instead when
		// botNavJitterMask > 0.
		if (botnav::directFirstBand(dist) && botTraceLegPath(mover, at, to, arrivalDist, seg) && !seg.empty()) {
			outTiles.insert(outTiles.end(), seg.begin(), seg.end());
			at = seg.back();
			continue;
		}
		seg.clear();

		const Position pathTarget = botnav::chunkTarget(at, to, CHUNK_DIST);
		const int32_t pathArrival = pathTarget == to ? arrivalDist : 3;

		// NO wide-search auto-escalation. It used to sit here to mirror goTo()'s escalation; goTo
		// no longer has one, so escalating would make this diagnostic OVER-report reachability —
		// the exact diagnostic-vs-live divergence the command exists to eliminate, just inverted.
		// A chunk that fails here is a chunk the live walker also fails.
		//
		// The wide profile is still reachable deliberately, via `/cavebot route ... wide`, which
		// calls botTraceLegPath(wide=true) directly. That is now the way to PREVIEW what
		// planScopedWalk()'s tier 2 would find, without pretending the generic walker has it.
		const bool ok = botTraceLegPath(mover, at, pathTarget, pathArrival, seg);
		if (!ok || seg.empty()) {
			outNote = fmt::format("stuck at ({},{},{}) aiming for ({},{},{}) [cheb={}]",
				at.x, at.y, at.z, pathTarget.x, pathTarget.y, pathTarget.z, dist);
			return false;
		}
		outTiles.insert(outTiles.end(), seg.begin(), seg.end());
		at = seg.back();
	}
	return true;
}

// ============================================================================
// BOT_NAV_REALISM Phase 2: bot_pathcore server adapter + jitter seam
// ============================================================================
// The dependency-free A* kernel (bot_pathcore.hpp) reaches live tiles through
// this adapter. It preserves the exact server cost model + the botAllowFcPos FC
// whitelist (both inherited via Map::canWalkTo). Used only when botNavJitterMask
// > 0 (Phase 4a); at mask==0 goTo() keeps calling the stock server pathfinder,
// so this path is dormant (bit-identical behavior) until jitter is switched on.
//
// NOTE: the orphaned BotAStarNodes + botGetPathMatchingCond above (0 call sites)
// are superseded by the kernel and slated for removal in Phase 11 (engine split);
// left in place now to keep this deploy a minimal diff.

// Per-bot, per-session A* cost seed. Computed inline (no BotState field / ABI
// change) from guid + the server-start epoch, mirroring personalitySeed.






// ============================================================================
// Navigation helpers (Phase 1+2)
// ============================================================================

// #119 direct-first limiter: guid -> time before which this bot makes NO direct-first attempt,
// whatever the target. Per bot, not per target (review): a per-target memo is defeated by any
// caller whose target changes between calls. Read and written only by goTo() in this TU.
namespace {
	constexpr int64_t DIRECT_FIRST_MISS_MS = 3000;
	std::unordered_map<uint32_t, int64_t> s_directFirstSuppressUntil;
}

bool BotEngine::goTo(BotState& bot, const Position& target, int32_t maxDist) {
	auto player = bot.getPlayer();
	if (!player) return false;

	int32_t arrivalDist = maxDist >= 0 ? maxDist : 3; // -1 = use default 3, 0 = exact tile

	if (isAtPosition(bot.currentPos, target, arrivalDist) &&
		bot.currentPos.z == target.z) {
		return true;
	}

	// Different z — only start FC for non-waypoint navigation (PK/combat z-pursuit, idle return)
	// Waypoint following (city routes, travel, patrol, leaving) handles z-transitions via
	// the waypoint system — server handles stairs/ramps/teleports when bot steps on the tile
	bool isFollowingWaypoints = bot.followingCityRoute ||
		bot.state == BotAIState::TRAVELING ||
		(bot.state == BotAIState::HUNTING && bot.huntScriptId > 0);
	// Deliberately the SIMPLE greedy hand-off, identical to main's. goTo() runs on the per-tick
	// movement path for every bot, so anything that plans a JOURNEY here is paid ~500x per tick.
	// The portal-graph hop planner that used to live in this branch cost a measured 5733ms
	// single-bot dispatcher stall (server-wide freeze). Journey planning now lives in
	// planScopedWalk(), which has exactly two rare callers.
	//
	// Second, load-bearing consequence: with no followWaypoints call in this function, goTo has no
	// path back into itself. The SIGSEGV recursion class (nested goTo re-entering the hop block,
	// 3 crashes in 6 min at 500 bots) is now STRUCTURALLY impossible rather than merely guarded.
	// Keep it that way — do not reintroduce a followWaypoints/leg-walk call here.
	if (bot.currentPos.z != target.z && bot.fcState == FloorChangeState::NONE && !isFollowingWaypoints) {
		startFloorChange(bot, target.z > bot.currentPos.z, target);
		return bot.fcState != FloorChangeState::NONE;
	}

	// BOT_TEST_HARNESS: `path` record for a WATCHED bot, emitted on every return below (RAII).
	// Created AFTER the already-there / floor-change early-outs: those run no search.
	BotAudPathScope audPath(&bot, "astar", bot.currentPos, target, arrivalDist);

	// Allow A* to path onto the destination FC tile only when navigating to a walk-on FC target.
	// We set the exact destination position — tile.cpp only allows the specific tile matching
	// botAllowFcPos, blocking all other FC tiles as intermediate steps (prevents accidental
	// z-transitions through nearby reverse-staircases). The flag is scoped to this pathfinding call only.
	bool allowFc = (arrivalDist == 0 && isWalkOnFcTile(target));
	if (allowFc) player->setBotAllowFcPos(target);

	int32_t sdx = static_cast<int32_t>(target.x) - static_cast<int32_t>(bot.currentPos.x);
	int32_t sdy = static_cast<int32_t>(target.y) - static_cast<int32_t>(bot.currentPos.y);
	int32_t dist = std::max(std::abs(sdx), std::abs(sdy));

	FindPathParams fpp;
	fpp.fullPathSearch = true;
	fpp.clearSight = false;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = PATH_MAX_DIST;
	fpp.minTargetDist = 0;

	std::vector<Direction> dirList;
	const int32_t navJitterMask = botNavJitterMaskClamped();

	// #119: direct-first. Inside botnav::directFirstBand (CHUNK_DIST < cheb <= 2*CHUNK_DIST) ask
	// ONE ordinary 512-node search for the whole route to the true target before chunking. The
	// chunk target is a straight-line interpolation; on the near side of a wall it lands in a
	// dead-end pocket, the chunk walk "succeeds" into it, and the next search has to walk back out:
	// baseline B5 temple -> ammo-shop stairs = 88 tiles with 10 revisited for a 65-tile route.
	// botnavsim walkcheck (Thais z7, 600 pairs in the band): 0 walks lost, revisits 724 -> 42,
	// tiles -10 %, searches per walk 2.16 -> 1.26 (a direct hit replaces several chunk searches).
	// The cost is one extra failed 512-node search when the target is NOT reachable in budget.
	// Bounded three ways (review): only plain walk callers opt in (GoToDirectFirstScope -- never
	// chase/flee/combat or the NODE candidate probe); the attempt is ONE search (the jitter
	// kernel when botNavJitterMask > 0, else the stock pathfinder -- not both); and after any
	// miss the bot makes no direct attempt for DIRECT_FIRST_MISS_MS (3 s), for ANY target.
	// Worst case: one extra 512-node search per bot per 3 s. Live cost: "[DIRECT_FIRST] 60s".
	if (s_goToDirectFirstOptIn && botnav::directFirstBand(dist)) {
		const int64_t nowMs = OTSYS_TIME();
		auto& suppressUntil = s_directFirstSuppressUntil[bot.guid];
		if (nowMs < suppressUntil) {
			s_directFirstSuppressed60s++;
		} else {
			fpp.maxTargetDist = arrivalDist;
			const bool found = navJitterMask > 0
				? botJitterPath(player, bot.currentPos, target, fpp, botNavSeed(bot.guid), navJitterMask, dirList)
				: g_game().map.getPathMatchingCond(player, target, dirList, FrozenPathingConditionCall(target), fpp);
			if (found) {
				s_directFirstHit60s++;
				if (allowFc) player->clearBotAllowFcPos();
				audPath.ok("astar-direct", dirList.size());
				botStartAutoWalk(bot, player, dirList);
				return true;
			}
			s_directFirstMiss60s++;
			suppressUntil = nowMs + DIRECT_FIRST_MISS_MS;
			dirList.clear();
		}
	}

	// Chunk long paths into sub-paths for reliability through obstacles.
	// 12 tiles keeps each A* sub-problem within the server's 512-node budget.
	// (CHUNK_DIST now lives on BotEngine so `/cavebot route` reports the same chunk target;
	// the arithmetic lives in botnav::chunkTarget, shared with botTraceLegWalk and botnavsim.)
	const Position pathTarget = botnav::chunkTarget(bot.currentPos, target, CHUNK_DIST);
	const int32_t pathArrivalDist = pathTarget == target ? arrivalDist : 3; // a chunk needs no exact arrival
	fpp.maxTargetDist = pathArrivalDist;

	// BOT_NAV_REALISM Phase 4a: when botNavJitterMask > 0, route through the pathcore
	// kernel with per-bot cost jitter so two bots don't take the identical shortest path.
	// Falls through to the stock server pathfinder if the kernel returns no path, so jitter
	// never reduces robustness. At mask == 0 (default) this branch is skipped entirely and
	// behavior is bit-identical to before.
	if (navJitterMask > 0
		&& botJitterPath(player, bot.currentPos, pathTarget, fpp, botNavSeed(bot.guid), navJitterMask, dirList)) {
		if (allowFc) player->clearBotAllowFcPos();
		audPath.ok("jitter", dirList.size());
		botStartAutoWalk(bot, player, dirList);
		return true;
	}
	dirList.clear();
	// Use server's SSE-optimized getPathMatchingCond (512 nodes, bounded by maxSearchDist).
	// With CHUNK_DIST=12, each sub-problem stays within the 512-node budget.
	if (g_game().map.getPathMatchingCond(player, pathTarget, dirList, FrozenPathingConditionCall(pathTarget), fpp)) {
		if (allowFc) player->clearBotAllowFcPos();
		audPath.ok(dist > CHUNK_DIST ? "astar-chunk" : "astar", dirList.size());
		botStartAutoWalk(bot, player,dirList);
		return true;
	}

	// NOTHING expensive belongs between here and the offset retry. A wide-search escalation
	// (4096-node pool + PATH_WIDE_DIST box) used to sit here, and it is the reason this whole
	// revert exists: "only runs after a search has already failed" is NOT self-limiting, because
	// a bot whose target is genuinely unreachable fails EVERY tick. Measured at 500 bots over
	// 11 min, PROCBOT_SLOW went 1630 events / 28ms avg / 107ms max / 45s total blocked ->
	// 2188 / 63ms / 5733ms max / 137s total; one bot held the dispatcher for 5.7 seconds, a
	// server-wide freeze felt in game. A 15s per-bot cooldown brought it back to ~18s blocked
	// but the cost was still on the hot path for all ~500 bots.
	//
	// The wide search does not come back here. It lives in goToWide(), reached only from planner
	// legs via followWaypoints' cfg.wideSearchOnFail -- PLEG, planScopedWalk's ZLEG and (#130) the FC
	// machine's planned-hop ZLEG, which any cross-floor goTo with a landed z-plan walks -- plus
	// planScopedWalk's door-budget tier. Because that last ZLEG is not a rare caller, goToWide
	// bounds itself: a per-bot cooldown and a global per-tick cap ("[WIDE] 60s").

	// Retry with random offsets — skip when exact-tile arrival is required (maxDist=0)
	// to avoid sending the bot to wrong positions and defeating pathfail counters
	if (arrivalDist > 0) {
		int32_t offsetRange = (dist > CHUNK_DIST) ? 5 : 2;
		for (int retry = 0; retry < 2; retry++) {
			Position offsetTarget = pathTarget;
			offsetTarget.x += static_cast<uint16_t>(uniform_random(-offsetRange, offsetRange));
			offsetTarget.y += static_cast<uint16_t>(uniform_random(-offsetRange, offsetRange));
			dirList.clear();
			if (g_game().map.getPathMatchingCond(player, offsetTarget, dirList, FrozenPathingConditionCall(offsetTarget), fpp)) {
				if (allowFc) player->clearBotAllowFcPos();
				audPath.ok("astar-offset", dirList.size());
				botStartAutoWalk(bot, player,dirList);
				return true;
			}
		}
	}

	if (allowFc) player->clearBotAllowFcPos();
	audPath.result("fail");

	// Give up. A same-floor target that is only reachable by leaving this floor and coming back
	// (bridges, underpasses, castle interiors, Thais' stacked cellars) is NOT solved here — the
	// portal-graph fallback that used to arm from this point is gone with the rest of the journey
	// planning. This is a deliberate, user-approved trade: the generic caller fails cheaply
	// (one bounded 512-node search) and only planScopedWalk() pays for the graph.
	return false;
}

// Walk as CLOSE to `target` as the map allows, preferring 0 tiles and degrading to maxDist.
//
// A fixed arrival distance of 3 makes every approach look identical and unnaturally distant — a
// real player walks up to an NPC, and only stops short when something is in the way. Tries the
// tightest distance first and accepts the first that paths, so the bot ends up adjacent when the
// tile beside the NPC is free, and still succeeds at 3 when it is crowded, behind a counter, or
// otherwise unreachable.
//
// Only tightens when the bot is ALREADY near the target: while still walking in from across town
// the loose distance is used, so a long approach does not pay up to maxDist+1 A* calls per tick.
bool BotEngine::goToClosest(BotState& bot, const Position& target, int32_t maxDist, WaypointType wpType) {
	const int32_t cheb = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(target.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(target.y)));
	if (bot.currentPos.z != target.z || cheb > maxDist + 8) {
		return goToWithDoors(bot, target, maxDist, wpType);
	}
	for (int32_t want = 0; want <= maxDist; ++want) {
		if (goToWithDoors(bot, target, want, wpType)) {
			return true;
		}
	}
	return false;
}

// ============================================================================
// Scoped route planner — the ONLY place journey planning is allowed to live
// ============================================================================
//
// Reachable from exactly two callers, both rare by construction: `/cavebot <bot> goto x,y,z`
// and the NPC visit (one weighted choice per reroll interval; ships disabled at
// botNpcVisitPct = 0). That rarity IS the rate limit — there is deliberately no cooldown here,
// because the thing being avoided is not "this search is expensive" but "every bot pays for it
// every tick", and neither caller can produce that.
//
// Escalation is STRICT: each tier runs only after the previous one failed, so an ordinary walk
// (the overwhelming majority) ends at tier 1 having cost exactly what it costs today.
//
//   step 0  Is the walk already issued still progressing? Nothing re-plans unless it is not.
//   step 1  Continue an in-flight same-floor leg (its DOOR waypoints get opened in stride).
//   step 3  ONE bounded door-permissive BFS answers two questions at once: is the target
//           reachable on this floor treating every openable door as open, and which doors does
//           the route cross? Doors found -> segment the route at them, because a closed door IS
//           a wall to A* and no single search can cross it. NOT reachable -> this target needs
//           floor hops even at same z, so skip step 4 entirely. Route clear AND already near ->
//           goToClosest for the final-approach distance tightening.
//           Nothing may run before this BFS: an earlier near-target shortcut preempted door
//           handling and left bots oscillating in front of closed doors.
//   step 4  ONE direct, unchunked A* to the true target, 4096 nodes / PATH_WIDE_DIST box, whole
//           route queued in a single call. The OTClient Redemption model (Map::findPath): path
//           directly to the destination, no interpolation, no re-evaluation until it fails.
//           What is NOT adopted is PathFindAllowNonPathable/AllowNonWalkable — a client paths
//           optimistically because it has fog of war and a server to reject illegal steps; we
//           ARE that server, so every tile keeps its real walkability check.
//   step 5  goToClosest chunked, as the fallback when the direct search finds nothing.
//   step 6  Portal graph: decompose into floor hops, walk each same-floor approach leg, hand the
//           transition to the FC machine. The graph does the floor decomposition; OTClient's
//           algorithm applies to the same-z legs.
//
// The door handling is what makes this fluid rather than reactive. Planning treats openable doors
// as open (zFloodPassableAt already does, and already refuses key-locked doors and other players'
// houses), so a door on the route becomes a DOOR waypoint the bot walks up to and opens — instead
// of the bot discovering it by failing against it and stalling until a timeout. As the server we
// can see which doors are openable; a real client cannot, so this is strictly better than a map
// click, not merely equal to one.
//
// Chunking is absent on purpose. A chunk target is a straight-line interpolation toward the
// destination, so when the true route doglegs around a large obstacle it lands INSIDE that
// obstacle and the walk fails on a route that exists (measured: (32345,32244,7)->(32350,32226,7)
// chunks to (32348,32232,7), sealed inside the Thais depot block; the real route is a ~50-tile
// dogleg). Tier 2 asking for the whole route in one search is precisely the fix.
// How long ONE waypoint of a planner leg may take before followWaypoints calls it stuck.
//
// A flat 15s is right for a door the bot is standing next to and catastrophically wrong for one
// across town: the leg's FIRST waypoint is the door, and reaching a door 129 tiles away takes ~40s
// at ~300ms/tile, so the waypoint was always skipped as "stuck" before the bot could arrive, then
// the stand waypoint after it, and the whole leg aborted. Measured exactly that on a Svargrond
// house visit: PLEG skipped both waypoints at 15s each while the bot was walking normally.
//
// Scale with the distance actually being covered, keeping 15s as the floor for near work and
// capping well inside the leg's own 120s global timeout.
static int64_t plannerWpStuckMs(const Position& from, const Position& to) {
	const int64_t d = std::max(std::abs(static_cast<int32_t>(from.x) - static_cast<int32_t>(to.x)),
	                           std::abs(static_cast<int32_t>(from.y) - static_cast<int32_t>(to.y)));
	return std::clamp<int64_t>(15000 + d * 400, 15000, 90000);
}

// One unchunked wide search (4096 nodes, PATH_WIDE_DIST box) to `target`, rate-limited per bot AND
// per tick.
//
// This is the capability the chunked walker lacks: goTo() interpolates a straight-line point every
// CHUNK_DIST tiles, and when the true route doglegs around a building that point lands INSIDE the
// building. A planner leg's first waypoint is normally a DOOR, so that failure is not an edge case
// for house visits — it is the common case, and it is the same failure the direct-search tier of
// planScopedWalk was written to fix.
//
// Callers (followWaypoints' cfg.wideSearchOnFail, plus planScopedWalk's door-budget tier): PLEG and
// planScopedWalk's ZLEG -- the two rare planner callers -- and, since #130, the FC machine's
// planned-hop ZLEG. That last one is NOT rare: any cross-floor goTo whose z-plan landed
// (s_plannedFc) walks it. So "only planner walks reach this" no longer bounds WHICH bots pay; the
// two limits below bound the cost on their own.
//
// THE LIMITS ARE LOAD-BEARING, not defensive tidiness. A wide search that FAILS is not
// self-limiting: the bot retries it every tick for the whole per-waypoint stuck window (15-90s,
// i.e. 150-900 ticks). That is the exact shape that produced the measured incident behind commit
// 0f97ea5b9 — at 500 bots, PROCBOT_SLOW went 1630 events / 28ms avg / 107ms max to 2188 / 63ms /
// 5733ms max, one bot holding the dispatcher 5.7 seconds.
//   - per bot: one search per WIDE_SEARCH_COOLDOWN_MS after a miss, and at least
//     WIDE_SEARCH_SUCCESS_GAP_MS after a hit (#130 review). A hit used to clear the cooldown, so a
//     walk that keeps getting cancelled (hit -> walk dropped -> goTo fails -> hit ...) could run a
//     wide search every tick;
//   - per tick: at most WIDE_SEARCHES_PER_TICK across ALL bots. The rest are deferred -- no search,
//     no cooldown charged, retried by the caller on a later tick -- so N bots dead-ending on the same
//     leg in the same tick cost two searches, not N.
// Always-on cost line: "[WIDE] 60s ..." from the tick loop's 60 s block.
bool BotEngine::goToWide(BotState& bot, const Position& target, int32_t maxDist, const char* site) {
	lastWideDeferred_ = false;
	auto player = bot.getPlayer();
	if (!player || bot.currentPos.z != target.z) {
		return false;
	}
	static std::unordered_map<uint32_t, int64_t> s_wideSearchNextAllowed;
	static constexpr int64_t WIDE_SEARCH_COOLDOWN_MS = 15000;
	static constexpr int64_t WIDE_SEARCH_SUCCESS_GAP_MS = 2000;
	static constexpr uint32_t WIDE_SEARCHES_PER_TICK = 2;
	const int64_t now = OTSYS_TIME();
	auto& nextAllowed = s_wideSearchNextAllowed[bot.guid];
	if (now < nextAllowed) {
		s_wide60sCooldownSkips++;
		return false; // cooldown: no search ran, so no `path` record either
	}
	if (s_wideTickStamp != s_tickBodyStartMs) {
		s_wideTickStamp = s_tickBodyStartMs;
		s_wideThisTick = 0;
	}
	if (s_wideThisTick >= WIDE_SEARCHES_PER_TICK) {
		s_wide60sDeferred++;
		lastWideDeferred_ = true; // R2-1: callers must not read this as a failed search
		return false; // deferred: nothing ran, nothing charged -- the caller retries on a later tick
	}
	s_wideThisTick++;
	s_wide60sMaxPerTick = std::max(s_wide60sMaxPerTick, s_wideThisTick);
	nextAllowed = now + WIDE_SEARCH_COOLDOWN_MS;
	// Which leg asked. The FC machine's planned-hop ZLEG and planScopedWalk's ZLEG share the "ZLEG"
	// prefix; only the former runs with the FC machine engaged.
	const bool fcLeg = bot.fcState != FloorChangeState::NONE;
	const char* label = fcLeg ? "FC-ZLEG" : (site ? site : "?");
	BotAudPathScope audPath(&bot, "wide", bot.currentPos, target, maxDist);

	FindPathParams fpp;
	fpp.fullPathSearch = true;
	fpp.clearSight = false;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = PATH_WIDE_DIST;
	fpp.minTargetDist = 0;
	fpp.maxTargetDist = maxDist;

	const bool allowFc = (maxDist == 0 && isWalkOnFcTile(target));
	if (allowFc) player->setBotAllowFcPos(target);
	std::vector<Direction> dirList;
	const auto t0 = std::chrono::steady_clock::now();
	const bool ok = botGetPathMatchingCond(player, target, dirList,
		FrozenPathingConditionCall(target), fpp, BotAStarNodes::BOT_MAX_NODES);
	const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
	if (allowFc) player->clearBotAllowFcPos();
	s_wide60sRuns++;
	s_wide60sUsMax = std::max(s_wide60sUsMax, us);
	if (s_wide60sUs.size() < 4096) {
		s_wide60sUs.push_back(static_cast<uint32_t>(std::min<int64_t>(us, 0xFFFFFFFF)));
	}
	if (fcLeg) {
		s_wide60sFcLeg++;
	}
	if (!ok) {
		s_legWideFail++; // #130: [ZROUTE] legWide=ok/run
		audPath.result("fail");
		return false;
	}
	s_legWideOk++;
	s_wide60sOk++;
	audPath.ok("wide", dirList.size());
	nextAllowed = now + WIDE_SEARCH_SUCCESS_GAP_MS;
	botStartAutoWalk(bot, player, dirList);
	castLog(bot, fmt::format("{}: wide search rescued the leg to ({},{},{}) — {} steps",
		label, target.x, target.y, target.z, dirList.size()));
	return true;
}

// The bot's nav reported success but queued nothing and has not moved. One warning once the state
// has persisted long enough to be real (not a single tick between chunks), then rate-limited, so a
// genuinely wedged bot is loud exactly once every 10s instead of 10x/second.
//
// O(1) per bot per tick and no allocation in the common path — cheap enough for 500 awake bots.
void BotEngine::noteNavStartedButIdle(BotState& bot) {
	static constexpr int64_t WARN_AFTER_MS = 3000;
	static constexpr int64_t WARN_REPEAT_MS = 10000;
	const int64_t now = OTSYS_TIME();
	auto& rec = s_navStartedButIdle[bot.guid];
	if (rec.since == 0 || rec.at != bot.currentPos) {
		rec.since = now;          // first observation, or the bot actually moved — restart the clock
		rec.at = bot.currentPos;
		rec.lastWarn = 0;
		return;
	}
	if (now - rec.since < WARN_AFTER_MS) {
		return;
	}
	if (rec.lastWarn != 0 && now - rec.lastWarn < WARN_REPEAT_MS) {
		return;
	}
	rec.lastWarn = now;
	g_logger().warn("[NAV_WEDGED] guid={} name='{}' at=({},{},{}) target=({},{},{}) "
	                "nav said started but queued nothing and has not moved for {}s",
	                bot.guid, bot.name, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
	                bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z,
	                (now - rec.since) / 1000);
	trackNavEvent("nav_wedged", bot, 0, "", bot.townId, "",
		fmt::format("target ({},{},{})", bot.walkTarget.x, bot.walkTarget.y, bot.walkTarget.z));
}

// "This leg is finished." Says WHICH of the two ways it finished, because the result struct
// cannot: reaching the last waypoint and skipping the last waypoint both exit through the same
// "Completed all waypoints" path. Distinguishing them in the log is the single line that turns an
// hour of cross-referencing PLAN/PLEG/IDLE-NAV timestamps into a one-glance diagnosis.
void BotEngine::logPlannerLegSpent(BotState& bot, size_t idxBefore, size_t size, const char* prefix) {
	// idx advanced past the end while the bot was ON the last waypoint == genuine arrival; any
	// other shape means the loop ran out because waypoints were skipped.
	const bool arrived = (idxBefore + 1 >= size);
	if (arrived) {
		castLog(bot, fmt::format("{}: leg spent — arrived {}/{}", prefix, size, size));
	} else {
		castLogError(bot, fmt::format("{}: leg spent — EXHAUSTED via skip at wp {}/{} (bot at ({},{},{}))",
			prefix, idxBefore + 1, size, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
		trackNavEvent("planner_leg_exhausted", bot, 0, "", bot.townId, "",
			fmt::format("wp {}/{}", idxBefore + 1, size));
	}
}

// True once this bot has rebuilt a leg for the SAME target too many times. See the declaration:
// the BFS is deterministic, so a rebuild finds the identical door and fails identically; without a
// bound the only thing that ended the cycle was the unrelated 240s stale-target guard.
bool BotEngine::plannerLegRebuildExhausted(BotState& bot, const Position& target) {
	auto& rec = s_plannerLegRebuilds[bot.guid];
	if (rec.forTarget != target) {
		rec.forTarget = target;
		rec.count = 0;
	}
	if (++rec.count > PLANNER_LEG_MAX_REBUILDS) {
		castLogError(bot, fmt::format("PLAN: giving up on ({},{},{}) — leg rebuilt {} times without arriving",
			target.x, target.y, target.z, rec.count - 1));
		return true;
	}
	return false;
}

bool BotEngine::planScopedWalk(BotState& bot, const Position& target, int32_t maxDist) {
	auto player = bot.getPlayer();
	if (!player) return false;

	// An active hop plan means tiers 1 and 2 have ALREADY been proven not to work for this
	// target, so re-running them every tick of a 40-tile approach walk would buy nothing and
	// pay for a 4096-node search each time. Jump straight back to tier 3.
	auto hopIt = s_plannedHop.find(bot.guid);
	if (hopIt != s_plannedHop.end()
		&& (hopIt->second.forTarget != target || hopIt->second.hop.portal.pos.z != bot.currentPos.z
		    || hopIt->second.blacklistStamp != zPortalBlacklistStamp(hopIt->second.hop.portal.pos))) {
		// A new target, floor or quarantine state needs a fresh decision. A fallback
		// selected under the same observed quarantine remains a valid cached decision.
		s_plannedHop.erase(hopIt);
		s_zLegPlan.erase(bot.guid);
		hopIt = s_plannedHop.end();
	}
	const bool haveHop = hopIt != s_plannedHop.end();
	const bool sameFloor = bot.currentPos.z == target.z;

	// (Step 0, the stall check, cannot live here: doIdle returns early while listWalkDir is
	// non-empty, so this function is never even called mid-walk. It runs from doIdle instead —
	// see plannerWalkBlocked / plannerReplan.)

	// ---- Step 1: continue an in-flight same-floor leg ----
	// The leg carries its DOOR waypoints, so followWaypoints opens each door as the bot reaches
	// it rather than the bot discovering the obstruction by failing against it.
	auto legIt = s_plannerLeg.find(bot.guid);
	if (legIt != s_plannerLeg.end()) {
		if (legIt->second.forTarget != target || legIt->second.z != bot.currentPos.z) {
			s_plannerLeg.erase(legIt); // retargeted, or we changed floor — rebuild
		} else {
			auto& leg = legIt->second;
			WaypointFollowConfig cfg;
			cfg.logPrefix = "PLEG";
			cfg.globalTimeoutMs = 120000;
			cfg.perWpStuckMs = plannerWpStuckMs(bot.currentPos, target);
			cfg.trackMovementProgress = true;
			cfg.zChangeGraceMs = 500;
			cfg.wideSearchOnFail = true; // planner leg: escalate a failed waypoint walk once
			const size_t legSize = leg.wps.size();
			const size_t legIdxBefore = leg.idx;
			auto res = followWaypoints(bot, leg.wps, leg.idx, leg.skipCount, cfg);
			// `inProgress`, NOT `!aborted`. followWaypoints sets `aborted` for only three loud
			// failures (global timeout, the 200-tile sanity check, a jitter reroll); a leg whose
			// waypoints were all SKIPPED on the per-waypoint stuck timer instead runs the loop out
			// and exits via "Completed all waypoints" with inProgress=false and aborted=FALSE.
			// Testing !aborted therefore read a spent leg as a healthy one, returned "started",
			// and left s_plannerLeg in place — so every later tick re-entered with idx == size,
			// the while loop never executed, and the planner reported success forever while
			// queuing nothing. That silent freeze also disarmed every escape: plannerWalkBlocked
			// needs a non-empty walk queue, and pathFailCount only climbs when navStarted is
			// false. Measured in production: 22 house visits started, 1 arrived, and the failures
			// were 14 hibernate + 7 stale + ZERO giveup — the fingerprint of exactly this.
			//
			// Every other followWaypoints caller already checks inProgress first (bot_hunt.cpp's
			// three, followCityRoute); the planner was the odd one out.
			if (res.inProgress) {
				return true;
			}
			// Spent. Two causes share this exit and the struct cannot tell them apart: the bot
			// genuinely reached the final waypoint, or every waypoint was skipped. So do NOT
			// return false here — that would report failure for a leg that just arrived and could
			// abort a house visit at the moment it succeeded. Erase and fall through: on genuine
			// arrival the tiers below no-op at distance 0 and return true harmlessly; on
			// exhaustion they re-derive for real.
			logPlannerLegSpent(bot, legIdxBefore, legSize, "PLEG");
			s_plannerLeg.erase(bot.guid);
			if (plannerLegRebuildExhausted(bot, target)) {
				return false;
			}
		}
	}

	// Tiers 1 and 2 are SAME-FLOOR ONLY, and that is not an optimisation — it is what keeps the
	// ladder a ladder. On a different floor goToClosest cannot fail: it degrades to goToWithDoors
	// -> goTo, whose different-floor branch arms the greedy FC machine and returns true. Running
	// tier 1 first for a cross-floor target would therefore always "succeed" and tiers 2-3 would
	// never run — collapsing the planner into exactly the greedy behaviour it exists to replace,
	// for the majority case NPC visits care about (visits are eligible up to 6 floors away).
	//
	// Worse, greedy startFloorChange sets fcState immediately, and the dispatcher hands the WHOLE
	// tick to handleFloorChange before self-defense or combat run. Tier 3's approach leg walks
	// with fcState left at NONE precisely to avoid that, and only engages the FC machine in the
	// last ~10 tiles.
	//
	// So: cross-floor goes straight to the graph, and the greedy hand-off stays as the LAST
	// resort, below, for when the graph has no route at all.

	// How close before the final approach takes over. This is goToClosest's OWN tightening
	// threshold (bot_nav.cpp): beyond it goToClosest just forwards to goToWithDoors anyway, so
	// splitting here costs nothing and changes no behaviour for the near case.
	const int32_t chebToTarget = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(target.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(target.y)));
	const bool nearTarget = chebToTarget <= maxDist + 8;

	// ---- Step 3: same floor — do we need hops, and what is in the way? ----
	// A closed door-permissive frontier can require floor hops at the same z. Reaching the
	// BFS node budget is unknown instead: try the existing bounded, cooldown-gated direct
	// search before selecting a portal detour.
	bool needHops = false;
	if (sameFloor && !haveHop) {
		std::vector<Position> doors;
		// Consumed once: a re-plan after a stall must not reuse the answer that just failed.
		const bool forceFresh = s_plannerForceFresh.erase(bot.guid) > 0;
		bool doorBudgetExhausted = false;
		if (plannerLegDoors(bot.currentPos, target, doors, forceFresh, &doorBudgetExhausted)) {
			if (doors.empty()) {
				// Clear run. Near the target, hand over to goToClosest for the distance
				// tightening that makes the final approach look human (walk up to the NPC rather
				// than stopping at 3).
				//
				// This check MUST sit behind the door BFS, not in front of it. When it ran first
				// it preempted door handling for anything within maxDist+8, so a target behind a
				// closed door never got a DOOR waypoint once the bot came close — goToClosest
				// cannot open doors, so it returned true while queuing nothing and the bot
				// oscillated in place forever (observed live at (32359,32252,7)).
				//
				// The queue check closes the other half of that bug: goToClosest reporting
				// success is not proof a walk started. Without it, a no-op "success" every tick
				// keeps pathFailCount at 0, so the give-up never triggers and the stall detector
				// (which needs a non-empty queue) never arms either. pendingWalkPauseEventId
				// counts as started — the observed-walk-pause defers the walk deliberately.
				if (nearTarget && goToClosest(bot, target, maxDist)
					&& (!player->listWalkDir.empty() || bot.pendingWalkPauseEventId != 0)) {
					return true;
				}
				// Otherwise fall through to the single direct search below — the pure OTClient
				// case, one path, whole route queued, nothing re-evaluated until it fails.
			} else {
				// Segment at the doors. Each door is a leg boundary because a closed door is a
				// wall to A*; between doors the route is still solved in one search. The bot
				// walks up and opens each one in stride instead of bumping into it and stalling.
				PlannerLeg leg;
				leg.forTarget = target;
				leg.z = bot.currentPos.z;
				for (const auto& d : doors) {
					leg.wps.emplace_back(d, WaypointType::DOOR);
				}
				leg.wps.emplace_back(target, WaypointType::STAND);
				castLog(bot, fmt::format("PLAN: same-floor route to ({},{},{}) — {} door(s) on the way",
					target.x, target.y, target.z, doors.size()));
				auto& stored = (s_plannerLeg[bot.guid] = std::move(leg));
				// A NEW leg needs a NEW clock. followWaypoints' global no-progress timer resets only
				// when waypointIdx CHANGES, and every rebuilt leg starts at index 0 — so a leg built
				// after an earlier one also stalled at 0 inherited the old timestamp and aborted on
				// its first tick, before the bot could take a single step toward the door. Observed
				// live: "same-floor route — 1 door(s)" immediately followed by "No progress for 2min
				// — aborting".
				s_routeProgress.erase(bot.guid);
				WaypointFollowConfig cfg;
				cfg.logPrefix = "PLEG";
				cfg.globalTimeoutMs = 120000;
				cfg.perWpStuckMs = plannerWpStuckMs(bot.currentPos, target);
				cfg.trackMovementProgress = true;
				cfg.zChangeGraceMs = 500;
				cfg.wideSearchOnFail = true; // planner leg: escalate a failed waypoint walk once
				const size_t legSize = stored.wps.size();
				const size_t legIdxBefore = stored.idx;
				auto res = followWaypoints(bot, stored.wps, stored.idx, stored.skipCount, cfg);
				// Same correction as step 1 — see the long note there. A leg built THIS tick can
				// only report !inProgress by genuinely arriving (the per-waypoint stuck timer needs
				// real elapsed time to fire), so the fall-through below is the cheap, harmless case
				// here; it is written the same way anyway so the two sites cannot drift.
				if (res.inProgress) {
					return true;
				}
				logPlannerLegSpent(bot, legIdxBefore, legSize, "PLEG");
				s_plannerLeg.erase(bot.guid);
				if (plannerLegRebuildExhausted(bot, target)) {
					return false;
				}
			}
		} else {
			if (doorBudgetExhausted) {
				castLog(bot, fmt::format("PLAN: door search budget exhausted for ({},{},{}) — trying bounded direct path",
					target.x, target.y, target.z));
				if (goToWide(bot, target, maxDist, "PLAN")) return true;
			} else {
				castLog(bot, fmt::format("PLAN: ({},{},{}) not reachable within door-search bounds on z={} — planning floor hops",
					target.x, target.y, target.z, bot.currentPos.z));
			}
			// Preserve the previous graph fallback on failure or cooldown. Do not run
			// another ungated direct search in step 4 after the bounded attempt.
			needHops = true;
		}
	}

	// ---- Step 4: one direct, unchunked search — the OTClient model ----
	// Reached when the route is clear of doors, so the whole thing is solvable in one search.
	//
	// goTo() chunks anything beyond CHUNK_DIST=12 to an interpolated point, walks it, then
	// re-paths from there. That is continuous re-evaluation: a 60-tile walk re-decides its route
	// five times, each from a straight-line guess that ignores where the real route goes. It is
	// also NOT what OTClient Redemption does on a map click — Map::findPath solves the whole
	// route to the true destination once and only re-solves when the walk actually fails.
	//
	// (On a map click OTClient computes the path CLIENT-side and sends the resulting direction
	// list; the server just executes it. botStartAutoWalk -> player->startAutoWalk is that same
	// mechanism, so this is not an imitation of a map click — it is one.)
	//
	// One search, the ENTIRE route queued in a single call, and no further pathfinding until the
	// queue drains or step 0 detects a stall. Chunking survives only as the fallback below.
	//
	// Affordable precisely because this function has two rare callers; the generic per-tick
	// goTo() every other bot uses is untouched and still chunks.
	//
	// Same floor by construction — A* here is strictly 2D (mirroring OTClient's
	// PathFindResultImpossible for startPos.z != goalPos.z).
	if (sameFloor && !haveHop && !needHops) {
		FindPathParams fpp;
		fpp.fullPathSearch = true;
		fpp.clearSight = false;
		fpp.allowDiagonal = true;
		fpp.keepDistance = false;
		fpp.maxSearchDist = PATH_WIDE_DIST;
		fpp.minTargetDist = 0;
		fpp.maxTargetDist = maxDist;

		const bool allowFc = (maxDist == 0 && isWalkOnFcTile(target));
		if (allowFc) player->setBotAllowFcPos(target);
		std::vector<Direction> dirList;
		const bool ok = botGetPathMatchingCond(player, target, dirList,
			FrozenPathingConditionCall(target), fpp, BotAStarNodes::BOT_MAX_NODES);
		if (allowFc) player->clearBotAllowFcPos();
		if (ok) {
			botStartAutoWalk(bot, player, dirList);
			castLog(bot, fmt::format("PLAN: direct route to ({},{},{}) — {} steps (wide search)",
				target.x, target.y, target.z, dirList.size()));
			return true;
		}
	}

	// ---- Step 5: chunked fallback for a long walk the direct search could not solve ----
	// Reached only when one 4096-node search over a 200-tile box found nothing, which for a
	// same-floor target usually means the route is genuinely long rather than blocked. Chunking
	// re-evaluates, but a walk that re-evaluates beats a walk that never starts.
	if (sameFloor && !haveHop && !needHops && !nearTarget && goToClosest(bot, target, maxDist)) {
		return true;
	}

	// ---- Step 6: portal graph ----
	// The FC machine owns the whole tick once engaged; never plan over it. Reported as success
	// because a floor change genuinely IS in progress — returning false would inflate the
	// caller's pathFailCount and abandon a walk that is proceeding normally.
	if (bot.fcState != FloorChangeState::NONE) return true;
	if (!zGraphReady_) return sameFloor ? false : goToClosest(bot, target, maxDist);

	if (!haveHop) {
		std::vector<botnav::ZRouteHop> hops;
		// forceGraph: for a same-floor target step 3's door-permissive BFS has PROVEN the direct
		// leg unwalkable, so the planner must search the graph rather than answering "same floor,
		// no hop needed".
		if (!zPlanFullRoute(bot.currentPos, target, hops, /*forceGraph=*/true, bot.guid) || hops.empty()) {
			// #116 review: a SHORT cold-cache deferral or a free-retry abort means "not yet", not
			// "unreachable": returning false would charge doIdle's pathFailCount (3 strikes -> give
			// up) for a plan that succeeds on the next, cheap attempt. So report "started" and wait
			// -- but bounded (review round 2): a pair whose backoff got ARMED has stopped
			// converging, and any wait for one target past Z_PLAN_WAIT_MAX_MS is no longer "cold
			// cache"; both fall through to the normal give-up, like before this fix. Without the
			// bound a non-converging pair kept the bot motionless for up to the 240 s stale-target
			// guard.
			// #130: cross-floor waits the same bounded way. Its greedy hand-off below picks the
			// NEAREST transition in the right direction, blind to the target, so taking it on a
			// "not yet" answer is a guess the planner had not made -- and after a planned portal is
			// quarantined the replan (which has to prove the detour) aborts, so the guess fires
			// exactly then: B5 G3 climbed the ladder at (32383,32231,7) into an 11-tile z6 pocket
			// whose only exit was that ladder, and stranded there. Waiting re-plans toward the
			// ORIGINAL goal every tick, so a converging plan (or its blacklist-free [ZPLAN_RECOVER]
			// pass, which brings back the only stair) is taken instead. Bounds (review F3): the wait
			// is Z_PLAN_WAIT_MAX_MS PER HOP, not per walk -- a landed plan erases s_zPlanWait, so a
			// later hop of the same journey can wait again; the walk as a whole stays bounded by the
			// planner-walk stale-target guard (240 s) and by the abort backoff, which ends any wait
			// as soon as it arms. An armed backoff, an expired wait and its tombstone still end in
			// the greedy hand-off, as before. Making the proof cheap is the nav-precompute epic
			// (#134, C').
			const bool shortWait = (lastZPlanResult_ == ZPlanResult::DEFERRED
				|| (lastZPlanResult_ == ZPlanResult::ABORTED && !lastZPlanBackoffArmed_));
			if (shortWait) {
				const int64_t nowMs = OTSYS_TIME();
				auto& w = s_zPlanWait[bot.guid];
				if (w.startMs == 0 || w.target != target) {
					w = ZPlanWait { target, nowMs, 0, false };
				}
				if (!w.gaveUp && nowMs - w.startMs <= Z_PLAN_WAIT_MAX_MS) {
					w.untilMs = nowMs + 1500;
					return true;
				}
				if (!w.gaveUp) {
					castLog(bot, fmt::format("PLAN: waited {}ms on a deferred plan to ({},{},{}) -- giving up",
						nowMs - w.startMs, target.x, target.y, target.z));
					w.gaveUp = true; // tombstone: no second wait for this target
					w.untilMs = 0;
				}
			} else if (auto wit = s_zPlanWait.find(bot.guid); wit != s_zPlanWait.end()
			           && !(wit->second.gaveUp && wit->second.target == target)) {
				s_zPlanWait.erase(wit); // keep only a same-target tombstone
			}
			if (!lastZPlanWasWait()) {
				s_zPlanFail++;
			}
			// No graph route. For a cross-floor target the legacy greedy hand-off is still worth a
			// try — it is what every other caller gets, and the graph can be incomplete. For a
			// same-floor target there is nothing left: give up cleanly, NO retry loop. Either way
			// the caller's pathFailCount give-up bounds how many ticks this is re-attempted.
			return sameFloor ? false : goToClosest(bot, target, maxDist);
		}
		s_zPlanOk++;
		s_zPlanWait.erase(bot.guid); // the plan landed: any wait for it is over
		PlannerHop ph;
		ph.hop.portal = hops.front().portal;
		ph.hop.plannedAt = OTSYS_TIME();
		ph.forTarget = target;
		// A fresh fallback may deliberately accept a quarantined portal. Reuse
		// that decision until its observed quarantine changes, rather than
		// re-running the graph on every chunk of the accepted approach.
		ph.blacklistStamp = zPortalBlacklistStamp(ph.hop.portal.pos);
		hopIt = s_plannedHop.emplace(bot.guid, ph).first;
		castLog(bot, fmt::format("PLAN: {} hop(s) to ({},{},{}) — next: {} {} at ({},{},{}) lands ({},{},{})",
			hops.size(), target.x, target.y, target.z,
			botnav::zPortalKindName(ph.hop.portal.kind), ph.hop.portal.goesDown ? "DOWN" : "UP",
			ph.hop.portal.pos.x, ph.hop.portal.pos.y, ph.hop.portal.pos.z,
			ph.hop.portal.landing.x, ph.hop.portal.landing.y, ph.hop.portal.landing.z));
	}

	const ZPlannedHop hop = hopIt->second.hop; // by value: followWaypoints below can erase the map
	const int32_t hopDist = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(hop.portal.pos.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(hop.portal.pos.y)));

	if (hopDist > Z_FC_ENGAGE_DIST) {
		// Portal still far: walk the approach as ordinary WAYPOINTS — the same synthetic leg the
		// FC machine picks up later, so doors discovered now carry over instead of being
		// rediscovered. followWaypoints opens DOOR waypoints on arrival, which is what makes the
		// "plan as if openable doors are open, then open them when you get there" model work:
		// zFloodPassableAt already treats a closed-but-openable door as passable when planning.
		//
		// fcState is deliberately NOT set here. `if (bot.fcState != NONE) { handleFloorChange;
		// return; }` hands the WHOLE tick to the FC machine, so entering it for a 35-tile approach
		// would silence self-defense, hunting and combat for the entire walk.
		s_zLegWalks++;
		auto& leg = ensureZLegPlan(bot, bot.currentPos, hop.portal);
		WaypointFollowConfig cfg;
		cfg.logPrefix = "ZLEG";
		cfg.globalTimeoutMs = 60000;
		cfg.perWpStuckMs = plannerWpStuckMs(bot.currentPos, hop.portal.pos);
		cfg.trackMovementProgress = true;
		cfg.zChangeGraceMs = 500;
		cfg.wideSearchOnFail = true; // planner leg: escalate a failed waypoint walk once
		const size_t zLegSize = leg.wps.size();
		const size_t zLegIdxBefore = leg.idx;
		auto res = followWaypoints(bot, leg.wps, leg.idx, leg.skipCount, cfg);
		// `inProgress`, not `!aborted` — see step 1. This site needs no fall-through: the leg's
		// target is an intermediate PORTAL rather than the caller's destination, and this is the
		// last tier, so there is nothing below to fall through to. Widening the condition simply
		// lets the cleanup that already existed for the aborted case handle the skip-exhausted case
		// identically, which is what it should always have done.
		if (res.inProgress) {
			return true;
		}
		logPlannerLegSpent(bot, zLegIdxBefore, zLegSize, "ZLEG");
		// The leg is unwalkable. Give up on the whole walk rather than blacklisting the portal and
		// replanning: this planner has no business quarantining a staircase for every other bot on
		// the strength of one NPC visit failing, and a retry ladder here is exactly the
		// "only runs after a failure" trap that made the old escalation unbounded.
		s_zLegPlan.erase(bot.guid);
		s_plannedHop.erase(bot.guid);
		s_zHopFail++;
		return sameFloor ? false : goToClosest(bot, target, maxDist);
	}

	// Close enough — hand the transition to the FC machine.
	//
	// s_plannedFc MUST be written here, and it is NOT redundant with what startFloorChange does
	// for itself. startFloorChange only consults the z-planner when `targetPos.z !=
	// bot.currentPos.z` (bot_tick.cpp), and tier 3's whole reason to exist is the SAME-z target
	// reachable only by leaving this floor and coming back — for which that condition is false.
	// Without this line the FC machine would fall back to its greedy nearest-transition scan and
	// take the wrong staircase. On a cross-floor target it also saves startFloorChange a redundant
	// re-plan: it finds this entry already on the current floor and keeps it.
	s_plannedFc[bot.guid] = hop;
	startFloorChange(bot, hop.portal.goesDown, target);
	if (bot.fcState == FloorChangeState::NONE) {
		// The failure guard can refuse the handoff. Do not acknowledge a walk
		// that has neither a queue nor a running floor-change executor.
		s_plannedFc.erase(bot.guid);
		s_plannedHop.erase(bot.guid);
		s_zLegPlan.erase(bot.guid);
		return false;
	}
	return true;
}

// "The walk it is executing is not progressing." Returns true at most once per stall, then
// re-arms — the caller reacts by throwing away its cached plan and re-deriving.
//
// Why not wait for "There is no way": that message is generated CLIENT-side by OTClient's own
// findPath, and the server-side RETURNVALUE_THEREISNOWAY goes out via sendCancelMessage, which a
// bot has no client to receive. Worse, creature.cpp does NOT drop the queue when a step fails — it
// re-issues the same blocked step forever. So the only observable signal is the queue itself.
//
// Requires BOTH "did not move" and "queue did not shrink". A bot pushed aside by another creature
// keeps draining its queue and is not stuck; a bot walking into a closed door drains nothing.
bool BotEngine::plannerWalkBlocked(BotState& bot, const std::shared_ptr<Player>& player) {
	auto& st = s_plannerStuck[bot.guid];
	const size_t q = player->listWalkDir.size();
	const int64_t now = OTSYS_TIME();

	if (q == 0) { // nothing queued — no walk to be blocked
		st = PlannerStuck {};
		return false;
	}
	if (bot.currentPos != st.lastPos || q < st.lastQueue || st.sinceMs == 0) {
		st.lastPos = bot.currentPos;
		st.lastQueue = q;
		st.sinceMs = now; // progressing (or first observation) — restart the clock
		return false;
	}
	if (now - st.sinceMs >= PLANNER_STUCK_MS) {
		st = PlannerStuck {};
		return true;
	}
	return false;
}

// Throw away every cached routing decision for this bot while KEEPING the planner claim, and
// cancel the walk in flight. Used when a walk stalls: the door that was open may have shut, a
// creature may have parked on the route, or the leg may turn out to need floor hops after all —
// so the next planScopedWalk re-derives from scratch against current state.
//
// Deliberately not clearPlannerWalk: that also drops s_plannerWalk, which would hand the bot back
// to the generic chunked walker mid-journey.
void BotEngine::plannerReplan(BotState& bot, const std::shared_ptr<Player>& player) {
	s_plannerLeg.erase(bot.guid);
	s_zLegPlan.erase(bot.guid);
	s_plannedHop.erase(bot.guid);
	// The cached door list is what produced the route that just failed, so re-deriving from it
	// would hand back the identical bad answer and the bot would loop against it for the whole
	// 60s TTL. Force one fresh BFS.
	s_plannerForceFresh.insert(bot.guid);
	if (player) {
		player->listWalkDir.clear();
		player->stopEventWalk();
	}
}

// True only while the planner still owns the bot's CURRENT walk target. Comparing the position
// rather than trusting a bare guid entry is what makes the claim self-invalidating — see the
// declaration in bot_engine_impl.hpp.
bool BotEngine::isPlannerWalk(const BotState& bot) const {
	if (!bot.hasWalkTarget) return false;
	auto it = s_plannerWalk.find(bot.guid);
	return it != s_plannerWalk.end() && it->second == bot.walkTarget;
}

void BotEngine::clearPlannerWalk(uint32_t guid) {
	s_plannerWalk.erase(guid);
	// Close the route clocks with the walk that owned them (#118). The planner's legs (PLEG/ZLEG)
	// are the only followWaypoints routes a planner walk drives, and a walk that ends on arrival
	// via the FC machine never passes followWaypoints' "Completed all waypoints" cleanup — so the
	// clocks outlived it and the NEXT goto read the time since that arrival as a stall. Gated on a
	// leg actually existing, so a bot following some other route keeps its clocks.
	if (s_plannerLeg.count(guid) > 0 || s_zLegPlan.count(guid) > 0 || s_plannedHop.count(guid) > 0) {
		s_routeWpTimer.erase(guid);
		s_routeProgress.erase(guid);
	}
	s_plannerLeg.erase(guid);
	s_plannerLegRebuilds.erase(guid); // new walk == fresh rebuild budget
	s_navStartedButIdle.erase(guid);
	s_zPlanWait.erase(guid); // #116
	s_plannerStuck.erase(guid);
	s_plannerForceFresh.erase(guid);
	if (s_plannedHop.erase(guid) > 0) {
		s_zLegPlan.erase(guid);
	}
	// Hand back the NPC approach-tile claim and drop the pending greeting. Previously NOTHING
	// released these except a successful arrival — every other exit (give-up, stale-target
	// timeout, hibernate, death, /cavebot stop, a retargeting goto) relied purely on the
	// reservation TTL expiring. That was survivable at a 45s TTL; at the 300s TTL the long
	// cross-floor visits now need, a leaked claim would block a counter spot for five minutes.
	//
	// Safe to call on the success path too: the arrival handler greets and releases BEFORE this
	// runs, and both operations are idempotent.
	releaseNpcApproach(guid);
	s_pendingNpcVisit.erase(guid);
}

bool BotEngine::goToWithDoors(BotState& bot, const Position& target, int32_t maxDist, WaypointType wpType) {
	// For DOOR waypoints: proactively open the door BEFORE pathfinding.
	// Without this, goTo() returns true when the bot is within maxDist (typically 1)
	// of the door — the bot "arrives" without ever opening it, then can't path to the
	// next waypoint through the closed door.
	if (wpType == WaypointType::DOOR) {
		auto player = bot.getPlayer();
		if (player) {
			tryOpenDoorAt(bot, player, target);
		}
	}
	// Try pathfinding — if door was just opened, the path should now be clear
	if (goTo(bot, target, maxDist)) return true;
	// goTo's internal retries all failed — try opening a door toward target
	auto player = bot.getPlayer();
	if (!player) return false;
	// For DOOR waypoints: try again (door might not have opened due to distance)
	if (wpType == WaypointType::DOOR && tryOpenDoorAt(bot, player, target)) {
		castLog(bot, fmt::format("DOORS: Opened door at waypoint, retrying path to ({},{},{})",
			target.x, target.y, target.z));
		return goTo(bot, target, maxDist);
	}
	// Fallback: scan tiles adjacent to bot toward target
	if (tryOpenDoors(bot, player, target)) {
		castLog(bot, fmt::format("DOORS: Opened door, retrying path to ({},{},{})",
			target.x, target.y, target.z));
		return goTo(bot, target, maxDist);
	}
	// LAST TIER: bridge to a door that is too far to see.
	//
	// tryOpenDoors above only inspects 1-3 tiles in the direction of travel, and a bot must be
	// ADJACENT to open a door — but A* will not path it toward the door, because a closed door is
	// a wall to canWalkTo. So a door 7-15 tiles out is an absolute stop: measured on the Thais
	// castle approach, six closed-but-unlocked doors at (32327,32207)…(32338,32208) killed every
	// route to Black Bert and Henricus even though a bot can open all of them.
	//
	// Walk to the first door in WALKING order that makes progress, open it, and let the next tick
	// re-path through it. Placed last so nothing above changes behaviour: authored hunt/city
	// routes only reach here when they were going to fail anyway.
	if (tryBridgeDoorLeg(bot, player, target)) {
		return true;
	}
	return false;
}

// Build (or reuse) the current hop's waypoint list: every blocking door in walking order,
// then the portal itself as the terminal action waypoint.
//
// Cached per bot and keyed on the hop position, so the doors already found and the walk progress
// already made carry over between ticks — and, importantly, between goTo's far-leg branch and the
// FC machine's WALKING_TO state, which are two entry points into the SAME leg.
BotEngine::ZLegPlan& BotEngine::ensureZLegPlan(BotState& bot, const Position& from, const botnav::ZPortal& hop) {
	auto& plan = s_zLegPlan[bot.guid];
	if (plan.forPortalPos == hop.pos && !plan.wps.empty()) {
		return plan;
	}
	plan = ZLegPlan {};
	plan.forPortalPos = hop.pos;
	// Fresh leg, fresh clocks — both of them (#118). s_routeProgress (the global no-progress
	// timer) was already reset here; s_routeWpTimer (the per-waypoint skip timer) was not, so a
	// leg built for a portal at the same index and position as an earlier, never-closed leg
	// inherited that leg's age and skipped its terminal waypoint on the first call.
	s_routeProgress.erase(bot.guid);
	s_routeWpTimer.erase(bot.guid);

	// Doors ON the route, in walking order — one path reconstruction, not a chain of
	// nearest-door searches.
	//
	// The first version called zFindBlockingDoor repeatedly, restarting from each door it found.
	// Because that search returned any openable door merely CLOSER to the target, the bot chased
	// doors off to the side of its route and tried to open them (observed: it walked to a door 9
	// tiles north of its path, then had to time out and skip it). zFindDoorsOnPath BFSes to the
	// portal and keeps only the doors the path actually crosses — and refuses to route through
	// another player's house on the way.
	std::vector<Position> doors;
	zFindDoorsOnPath(from, hop.pos, doors);
	for (const auto& d : doors) {
		plan.wps.emplace_back(d, WaypointType::DOOR);
	}

	// Terminal waypoint: the portal, typed so handleActionWaypoint performs the right mechanic.
	uint16_t itemId = 0;
	std::string extra;
	WaypointType wt = WaypointType::STAND;
	switch (hop.kind) {
		case botnav::ZPortalKind::LADDER:      wt = WaypointType::LADDER; break;
		case botnav::ZPortalKind::ROPE_SPOT:   wt = WaypointType::ROPE; break;
		case botnav::ZPortalKind::HOLE:        wt = WaypointType::HOLE; break;
		case botnav::ZPortalKind::STAIRS:      wt = WaypointType::STAIRS_UP; break;
		case botnav::ZPortalKind::TELEPORT:    wt = WaypointType::TELEPORT; break;
		case botnav::ZPortalKind::SEWER:
			wt = WaypointType::USE_WITH;
			extra = fmt::format("tile_item:{}", SEWER_ITEM_ID);
			break;
		case botnav::ZPortalKind::SHOVEL_HOLE:
			wt = WaypointType::USE_WITH;
			itemId = SHOVEL_ITEM_ID;
			break;
		default: wt = WaypointType::STAND; break;
	}
	Waypoint terminal(hop.pos, wt, itemId, extra);
	terminal.isWalkOnFc = isWalkOnFcTile(hop.pos);
	plan.wps.push_back(terminal);

	castLog(bot, fmt::format("ZLEG: built leg to {} ({},{},{}) — {} door(s)",
		botnav::zPortalKindName(hop.kind), hop.pos.x, hop.pos.y, hop.pos.z, plan.wps.size() - 1));
	return plan;
}

bool BotEngine::tryBridgeDoorLeg(BotState& bot, const std::shared_ptr<Player>& player, const Position& target) {
	if (bot.currentPos.z != target.z) {
		return false;
	}
	Position door;
	if (!zFindBlockingDoor(bot.currentPos, target, door)) {
		return false;
	}
	const uint64_t doorKey = botTileKey(door);
	const int64_t now = OTSYS_TIME();

	// One door at a time, with a hard timeout. s_failedDoors covers "opened and it did not work";
	// it does NOT cover "never reached the door", which is this exact failure, so without a
	// separate timer the bot would keep walking at the same unreachable door indefinitely.
	auto it = s_doorBridgeAttempt.find(bot.guid);
	if (it != s_doorBridgeAttempt.end() && it->second.first == doorKey) {
		if (now - it->second.second > Z_DOOR_BRIDGE_TIMEOUT_MS) {
			castLog(bot, fmt::format("DOOR_BRIDGE: giving up on ({},{},{}) — cooling down",
				door.x, door.y, door.z));
			s_failedDoors[door] = now; // reuse the existing 60s door cooldown
			s_doorBridgeAttempt.erase(it);
			s_zDoorBridgeGiveup++;
			return false;
		}
	} else {
		s_doorBridgeAttempt[bot.guid] = { doorKey, now };
	}

	const int32_t dist = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(door.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(door.y)));
	if (dist <= 1) {
		if (tryOpenDoorAt(bot, player, door)) {
			castLog(bot, fmt::format("DOOR_BRIDGE: opened blocking door ({},{},{})",
				door.x, door.y, door.z));
			s_doorBridgeAttempt.erase(bot.guid);
			s_zDoorBridgeOk++;
			return true; // next tick re-paths through the now-open door
		}
		// Adjacent but the open failed — let the existing cooldown handle it.
		s_failedDoors[door] = now;
		s_doorBridgeAttempt.erase(bot.guid);
		s_zDoorBridgeGiveup++;
		return false;
	}
	// Walk to a tile beside the door. The approach itself is reachable — the door is what blocks,
	// not the way to it — so plain goTo with arrivalDist 1 is enough and avoids recursing back
	// into goToWithDoors.
	if (goTo(bot, door, 1)) {
		castLog(bot, fmt::format("DOOR_BRIDGE: walking to blocking door at ({},{},{}) dist={}",
			door.x, door.y, door.z, dist));
		return true;
	}
	return false;
}

bool BotEngine::tryOpenDoors(BotState& bot, const std::shared_ptr<Player>& player, const Position& targetPos) {
	auto pos = player->getPosition();
	int32_t dx = static_cast<int32_t>(targetPos.x) - static_cast<int32_t>(pos.x);
	int32_t dy = static_cast<int32_t>(targetPos.y) - static_cast<int32_t>(pos.y);
	int32_t ndx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
	int32_t ndy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);

	// Only check tiles in the direction toward target (1-3 tiles max)
	// Matches Lua bot_system.lua:3804-3843 pattern — never scan all 8 adjacent
	std::vector<Position> tilesToCheck;
	if (ndx != 0 && ndy != 0) tilesToCheck.emplace_back(pos.x + ndx, pos.y + ndy, pos.z);
	if (ndx != 0) tilesToCheck.emplace_back(pos.x + ndx, pos.y, pos.z);
	if (ndy != 0) tilesToCheck.emplace_back(pos.x, pos.y + ndy, pos.z);

	auto& doorTable = getDoorTable();

	for (const auto& tilePos : tilesToCheck) {
		auto failIt = s_failedDoors.find(tilePos);
		if (failIt != s_failedDoors.end()) {
			if (OTSYS_TIME() - failIt->second <= DOOR_RETRY_COOLDOWN_MS) continue;
			s_failedDoors.erase(failIt);
		}
		auto tile = g_game().map.getTile(tilePos);
		if (!tile) continue;
		auto items = tile->getItemList();
		if (!items) continue;
		for (auto& item : *items) {
			auto it = doorTable.find(item->getID());
			if (it != doorTable.end()) {
				uint16_t closedId = item->getID();
				g_actions().useItem(player, tilePos, 0, item, false);
				if (item->getID() == closedId) {
					s_failedDoors[tilePos] = OTSYS_TIME();
					castLog(bot, fmt::format("DOOR: Failed to open door {} at ({},{},{}) — cooldown 60s",
						closedId, tilePos.x, tilePos.y, tilePos.z));
					continue;
				}
				castLog(bot, fmt::format("DOOR: Opened door {} at ({},{},{})",
					closedId, tilePos.x, tilePos.y, tilePos.z));
				return true;
			}
		}
	}
	return false;
}

bool BotEngine::tryOpenDoorAt(BotState& bot, const std::shared_ptr<Player>& player, const Position& doorPos) {
	auto failIt = s_failedDoors.find(doorPos);
	if (failIt != s_failedDoors.end()) {
		if (OTSYS_TIME() - failIt->second <= DOOR_RETRY_COOLDOWN_MS) return false;
		s_failedDoors.erase(failIt);
	}
	auto tile = g_game().map.getTile(doorPos);
	if (!tile) return false;
	auto items = tile->getItemList();
	if (!items) return false;
	auto& doorTable = getDoorTable();
	// First pass: check door table for known closed door IDs
	for (auto& item : *items) {
		if (doorTable.find(item->getID()) != doorTable.end()) {
			uint16_t closedId = item->getID();
			g_actions().useItem(player, doorPos, 0, item, false);
			if (item->getID() == closedId) {
				s_failedDoors[doorPos] = OTSYS_TIME();
				castLog(bot, fmt::format("DOOR: Failed to open door {} at ({},{},{}) — cooldown 60s",
					closedId, doorPos.x, doorPos.y, doorPos.z));
				return false;
			}
			castLog(bot, fmt::format("DOOR: Opened door {} at ({},{},{})",
				closedId, doorPos.x, doorPos.y, doorPos.z));
			return true;
		}
	}
	// Fallback: if tile blocks movement and has items, try using them anyway.
	// Some doors (newer areas) may not be in our table but the server action
	// system knows how to handle them.
	if (tile->hasFlag(TILESTATE_BLOCKPATH) || tile->hasFlag(TILESTATE_BLOCKSOLID)) {
		for (auto& item : *items) {
			// Skip ground tiles (stackpos 0) and non-moveable base items
			if (item->getID() < 100) continue;
			uint16_t itemId = item->getID();
			g_actions().useItem(player, doorPos, 0, item, false);
			// Check if the tile is no longer blocking after use
			if (!tile->hasFlag(TILESTATE_BLOCKPATH) && !tile->hasFlag(TILESTATE_BLOCKSOLID)) {
				castLog(bot, fmt::format("DOOR: Opened unknown door {} at ({},{},{})",
					itemId, doorPos.x, doorPos.y, doorPos.z));
				return true;
			}
		}
		// Tile still blocks — mark as failed
		s_failedDoors[doorPos] = OTSYS_TIME();
	}
	return false;
}

bool BotEngine::tryOpenAdjacentDoor(BotState& bot, const std::shared_ptr<Player>& player) {
	auto pos = player->getPosition();
	auto& doorTable = getDoorTable();
	for (int dx = -1; dx <= 1; dx++) {
		for (int dy = -1; dy <= 1; dy++) {
			if (dx == 0 && dy == 0) continue;
			Position tilePos(pos.x + dx, pos.y + dy, pos.z);
			auto failIt = s_failedDoors.find(tilePos);
			if (failIt != s_failedDoors.end()) {
				if (OTSYS_TIME() - failIt->second <= DOOR_RETRY_COOLDOWN_MS) continue;
				s_failedDoors.erase(failIt);
			}
			auto tile = g_game().map.getTile(tilePos);
			if (!tile) continue;
			auto items = tile->getItemList();
			if (!items) continue;
			for (auto& item : *items) {
				if (doorTable.find(item->getID()) != doorTable.end()) {
					uint16_t closedId = item->getID();
					g_actions().useItem(player, tilePos, 0, item, false);
					if (item->getID() == closedId) {
						s_failedDoors[tilePos] = OTSYS_TIME();
						castLog(bot, fmt::format("DOOR: Failed to open adjacent door {} at ({},{},{}) — cooldown 60s",
							closedId, tilePos.x, tilePos.y, tilePos.z));
						continue;
					}
					castLog(bot, fmt::format("DOOR: Opened adjacent door {} at ({},{},{})",
						closedId, tilePos.x, tilePos.y, tilePos.z));
					return true;
				}
			}
		}
	}
	return false;
}

bool BotEngine::tryPathToHuntDoor(BotState& bot, const std::shared_ptr<Player>& player) {
	// When FC WALKING_TO can't pathfind to stairs (door blocks path) and bot isn't
	// adjacent to the door, check the hunt script for DOOR waypoints on the same z.
	// If a closed door is found, pathfind to within 1 tile so we can open it next tick.
	if (bot.huntScriptId == 0) return false;

	const HuntScript* script = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == bot.huntScriptId) { script = &s; break; }
	}
	if (!script) return false;

	auto& doorTable = getDoorTable();
	auto pos = player->getPosition();

	// Collect DOOR waypoints from patrol + travel_from phases on the same z
	struct DoorCandidate {
		Position pos;
		int32_t dist;
	};
	std::vector<DoorCandidate> candidates;

	auto checkWaypoints = [&](const std::vector<Waypoint>& wps) {
		for (const auto& wp : wps) {
			if (wp.type != WaypointType::DOOR || wp.pos.z != pos.z) continue;
			int32_t d = std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(wp.pos.x)) +
						std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(wp.pos.y));
			if (d > 0 && d <= PATH_MAX_DIST) {
				// Skip doors that failed to open recently
				auto failIt = s_failedDoors.find(wp.pos);
				if (failIt != s_failedDoors.end() && OTSYS_TIME() - failIt->second <= DOOR_RETRY_COOLDOWN_MS) continue;
				// Verify the door is actually closed
				auto tile = g_game().map.getTile(wp.pos);
				if (!tile) continue;
				auto items = tile->getItemList();
				if (!items) continue;
				bool hasClosed = false;
				for (auto& item : *items) {
					if (doorTable.find(item->getID()) != doorTable.end()) {
						hasClosed = true;
						break;
					}
				}
				if (hasClosed) {
					candidates.push_back({wp.pos, d});
				}
			}
		}
	};

	checkWaypoints(script->patrolWaypoints);
	checkWaypoints(script->travelToWaypoints);
	checkWaypoints(script->travelFromWaypoints);

	if (candidates.empty()) return false;

	// Sort by distance — try closest first
	std::sort(candidates.begin(), candidates.end(), [](const DoorCandidate& a, const DoorCandidate& b) {
		return a.dist < b.dist;
	});

	// Try to open (if adjacent) or pathfind to within 1 tile of the closest closed door
	for (const auto& c : candidates) {
		if (c.dist <= 1) {
			// Already adjacent — try to open instead of re-pathing
			if (tryOpenDoorAt(bot, player, c.pos)) {
				castLog(bot, fmt::format("FC_WALK: Opened hunt DOOR at ({},{},{})",
					c.pos.x, c.pos.y, c.pos.z));
				return true;
			}
			// tryOpenDoorAt marks failed doors in s_failedDoors — skip this candidate
			continue;
		}

		FindPathParams fpp;
		fpp.fullPathSearch = true;
		fpp.clearSight = false;
		fpp.allowDiagonal = true;
		fpp.keepDistance = false;
		fpp.maxSearchDist = PATH_MAX_DIST;
		fpp.minTargetDist = 1;
		fpp.maxTargetDist = 1;

		std::vector<Direction> dirList;
		if (g_game().map.getPathMatching(player, c.pos, dirList, FrozenPathingConditionCall(c.pos), fpp)) {
			botStartAutoWalk(bot, player,dirList);
			castLog(bot, fmt::format("FC_WALK: Pathing to hunt DOOR at ({},{},{}) dist={}",
				c.pos.x, c.pos.y, c.pos.z, c.dist));
			return true;
		}
	}
	return false;
}

bool BotEngine::tryOpenDoorsOnTrail(BotState& bot, const std::shared_ptr<Player>& player) {
	auto trailIt = s_targetTrail.find(bot.guid);
	if (trailIt == s_targetTrail.end() || trailIt->second.count == 0) return false;

	auto& trail = trailIt->second;
	auto& doorTable = getDoorTable();
	auto pos = player->getPosition();

	// Check recent trail positions for closed doors near the bot
	// Iterate from most recent to oldest — most recent is most relevant
	for (size_t i = 0; i < trail.count; i++) {
		size_t idx = (trail.head + TARGET_TRAIL_SIZE - 1 - i) % TARGET_TRAIL_SIZE;
		const auto& trailPos = trail.positions[idx];
		if (trailPos.z != pos.z) continue;

		// Only consider trail positions within 5 tiles of bot
		int32_t tdist = std::max(
			std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(trailPos.x)),
			std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(trailPos.y)));
		if (tdist > 5) continue;

		// Check this trail tile for a closed door
		auto tile = g_game().map.getTile(trailPos);
		if (!tile) continue;
		auto items = tile->getItemList();
		if (!items) continue;

		for (auto& item : *items) {
			auto it = doorTable.find(item->getID());
			if (it == doorTable.end()) continue;

			// Found closed door on target's trail
			int32_t doorDist = std::max(
				std::abs(static_cast<int32_t>(pos.x) - static_cast<int32_t>(trailPos.x)),
				std::abs(static_cast<int32_t>(pos.y) - static_cast<int32_t>(trailPos.y)));

			if (doorDist <= 1) {
				// Adjacent — open it directly
				auto failIt = s_failedDoors.find(trailPos);
				if (failIt != s_failedDoors.end() && OTSYS_TIME() - failIt->second <= DOOR_RETRY_COOLDOWN_MS) continue;
				uint16_t closedId = item->getID();
				g_actions().useItem(player, trailPos, 0, item, false);
				if (item->getID() == closedId) {
					s_failedDoors[trailPos] = OTSYS_TIME();
					castLog(bot, fmt::format("TRAIL DOOR: Failed to open door {} at ({},{},{}) — cooldown 60s",
						closedId, trailPos.x, trailPos.y, trailPos.z));
					continue;
				}
				castLog(bot, fmt::format("TRAIL DOOR: Opened door {} at ({},{},{}) from target's path",
					closedId, trailPos.x, trailPos.y, trailPos.z));
				return true;
			} else {
				// Door found on trail but not adjacent — walk toward it
				castLog(bot, fmt::format("TRAIL DOOR: Walking to door at ({},{},{}) dist={} from target's path",
					trailPos.x, trailPos.y, trailPos.z, doorDist));
				goTo(bot, trailPos, 1);
				return true;
			}
		}
	}
	return false;
}

bool BotEngine::isAtPosition(const Position& a, const Position& b, int32_t range) const {
	return std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)) <= range &&
		   std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)) <= range;
}

