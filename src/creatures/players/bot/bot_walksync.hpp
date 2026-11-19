/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// bot_walksync.hpp -- #144 walk-desync classifier. Dependency-free (position.hpp + std only), so the
// engine (BotEngine::checkWalkDesync, bot_liveness.cpp) and the offline simulator
// (tools/botnavsim `desync` / `desynccheck`) run the SAME decision.
//
// Stock Creature::onCreatureWalk pops a direction from listWalkDir BEFORE the move is tried. When the
// move fails it only sends sendCancelWalk() to a client; a bot has none, so the rest of its queue runs
// one tile off the planned line. A successful step moves the creature synchronously inside the same
// dispatcher task as the pop, so between tasks a walk that has only succeeded satisfies
//     position == base + (the directions consumed since base)
// and any failed step (or a push) breaks that for the rest of the queue.
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "game/movement/position.hpp"

namespace botnav {

	// Mirrors getNextPosition (utils/tools.cpp) for the 8 walk directions, without its dependencies.
	inline Position walkStep(Direction d, Position p) {
		switch (d) {
			case DIRECTION_NORTH: p.y--; break;
			case DIRECTION_SOUTH: p.y++; break;
			case DIRECTION_WEST: p.x--; break;
			case DIRECTION_EAST: p.x++; break;
			case DIRECTION_SOUTHWEST: p.x--; p.y++; break;
			case DIRECTION_NORTHWEST: p.x--; p.y--; break;
			case DIRECTION_NORTHEAST: p.x++; p.y--; break;
			case DIRECTION_SOUTHEAST: p.x++; p.y++; break;
			default: break;
		}
		return p;
	}

	enum class WalkSync : uint8_t {
		Unchanged, // nothing consumed, not moved: no information
		InSync,    // the bot stands where the consumed directions lead
		Foreign,   // the queue is not (a prefix of) the one we handed over: not ours to judge
		Jump,      // floor change or teleport: other code owns that
		Desync,    // off the planned line (failed step / push): drop the rest of the queue
	};

	struct WalkSyncResult {
		WalkSync kind = WalkSync::Unchanged;
		Position expected {};
		size_t consumed = 0;
		// Desync only: the bot still stands ON the issued line, after `onLineAt` of the consumed
		// directions (the step after that one failed). -1 = it is off the line. When >= 0 the walk can
		// be retried as issued: re-queue issued[0 .. issued.size() - onLineAt) (see retryQueue).
		int32_t onLineAt = -1;
	};

	// `issued` = the queue at the last in-sync check, `base` = where the bot stood then; `queue` and
	// `cur` = now. Both queues are consumed from the BACK (Creature::getNextStep pops listWalkDir.back()).
	inline WalkSyncResult classifyWalkSync(const std::vector<Direction>& issued, const Position& base,
	                                       const std::vector<Direction>& queue, const Position& cur) {
		WalkSyncResult r;
		const size_t tn = issued.size();
		const size_t qn = queue.size();
		if (qn > tn || !std::equal(queue.begin(), queue.end(), issued.begin())) {
			r.kind = WalkSync::Foreign;
			return r;
		}
		if (qn == tn && cur == base) {
			return r; // Unchanged (content confirmed above: a same-length foreign queue is Foreign)
		}
		Position expect = base;
		for (size_t i = tn; i > qn; --i) {
			expect = walkStep(issued[i - 1], expect);
		}
		r.expected = expect;
		r.consumed = tn - qn;
		if (cur == expect) {
			r.kind = WalkSync::InSync;
			return r;
		}
		// A failed step moves the bot LESS than it consumed, a push moves it one tile: either way it
		// is within consumed+1 tiles of the base on the same floor.
		const int32_t moved = std::max(
			std::abs(static_cast<int32_t>(cur.x) - static_cast<int32_t>(base.x)),
			std::abs(static_cast<int32_t>(cur.y) - static_cast<int32_t>(base.y)));
		if (cur.z != base.z || static_cast<size_t>(moved) > r.consumed + 1) {
			r.kind = WalkSync::Jump;
			return r;
		}
		r.kind = WalkSync::Desync;
		// Still on the line? Walk the consumed directions again from the base and keep the LAST tile
		// that matches (the common case: the bot stands one step short of the tile it could not enter).
		Position p = base;
		if (p == cur) {
			r.onLineAt = 0;
		}
		for (size_t m = 1; m < r.consumed; ++m) {
			p = walkStep(issued[tn - m], p);
			if (p == cur) {
				r.onLineAt = static_cast<int32_t>(m);
			}
		}
		return r;
	}

	// The queue that retries the issued walk from tile `onLineAt` of it: every direction after it,
	// the failed one included (back = next step, as startAutoWalk expects).
	inline std::vector<Direction> retryQueue(const std::vector<Direction>& issued, int32_t onLineAt) {
		return std::vector<Direction>(issued.begin(), issued.end() - onLineAt);
	}

} // namespace botnav
