/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_movement.cpp — #198 movement realism for WATCHED bots.
//
// Research report 70 (2026-11-23) measured what gives our walking away on 70,349 harness steps:
// staircase paths (median 6 turns where an L needs 1), exact route replays (38 %), instant U-turns,
// goal-blind facing, taut inside corners (34 % of 90-degree turns). This module owns the fixes
// that are not in the dependency-free kernel (bot_pathcore.hpp holds the turn cost, the Manhattan
// heuristic, the diagonal / wall costs and the smooth noise):
//   - the per-walk shape: salt + diagonal and wall-offset rolls (botMoveShapeFor, from goTo);
//   - a hesitation and a turn before a U-turn re-plan (botMoveHoldWalk, from botStartAutoWalk);
//   - stops at doors and stairs, and moving random mid-street pauses there (botMoveDoorOpened,
//     botMoveStairHold, botMoveDeferPause);
//   - facing the NPC greeted, the player answered, and a player passing an idle bot.
//
// Everything acts only on a bot that is watched (botMoveObserved): unwatched, hibernated and virtual
// bots walk exactly as before, so the CPU cost lands only where someone can see it. Every chance is
// a config.lua key (BOT_MOVEMENT_CONFIG_KEYS), scaled per bot by a personality trait. Engine-side
// state is keyed by guid in inline maps (bot_engine_impl.hpp): no BotState / ABI change.
// Journal: "[MOVE] 60s ..." always; "[MOVE_WALK]" / "[MOVE_HOLD]" / "[MOVE_FACE]" per event for
// cast-watched bots (or every watched bot with botMoveLogAll).
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#pragma GCC diagnostic ignored "-Wunused-function"

namespace {
	constexpr int64_t MOVE_CFG_REFRESH_MS = 5000;
	constexpr int64_t MOVE_WALK_IDLE_MS = 60000;   // a walk unused this long is over
	constexpr int64_t MOVE_UTURN_WINDOW_MS = 1500; // the reversed step must be this recent
	constexpr int64_t MOVE_UTURN_COOLDOWN_MS = 4000;
	constexpr int64_t MOVE_HOLD_STALE_MS = 1500;   // a door / stair hold must be used this soon
	constexpr int64_t MOVE_DEFER_STALE_MS = 60000; // a moved pause expires if no door / stair comes
	constexpr int32_t MOVE_STAIR_HOLD_MAX_MS = 1500; // the FC machine is never held longer
	constexpr int64_t MOVE_GLANCE_RECHECK_MS = 1000;

	struct DeferredPause {
		int32_t ms = 0;
		int64_t atMs = 0;
	};
	std::unordered_map<uint32_t, DeferredPause>& deferredPauses() {
		static std::unordered_map<uint32_t, DeferredPause> m; // this TU only
		return m;
	}
	struct PlaceHold {
		int64_t untilMs = 0;
		int64_t setAtMs = 0;
		const char* kind = "";
	};
	std::unordered_map<uint32_t, PlaceHold>& placeHolds() {
		static std::unordered_map<uint32_t, PlaceHold> m; // this TU only
		return m;
	}
	// The stair tile a stop was last rolled for: one roll per stair per 30 s, so the FC machine's
	// per-tick STEPPING_ON re-entry (and goTo's per-tick call) does not re-roll it.
	struct StairRoll {
		Position pos {};
		int64_t atMs = 0;
	};
	std::unordered_map<uint32_t, StairRoll>& stairRolls() {
		static std::unordered_map<uint32_t, StairRoll> m; // this TU only
		return m;
	}
	constexpr int64_t MOVE_STAIR_REROLL_MS = 30000;
	std::unordered_map<uint32_t, int64_t>& glanceNextCheck() {
		static std::unordered_map<uint32_t, int64_t> m; // this TU only
		return m;
	}

	int32_t clampi(int64_t v, int32_t lo, int32_t hi) {
		return static_cast<int32_t>(std::min<int64_t>(hi, std::max<int64_t>(lo, v)));
	}

	const MoveCfg& moveCfg() {
		const int64_t now = OTSYS_TIME();
		if (s_moveCfgAtMs != 0 && now - s_moveCfgAtMs < MOVE_CFG_REFRESH_MS) {
			return s_moveCfg;
		}
		auto& cm = g_configManager();
		MoveCfg c;
		c.shapeEnable = cm.getBoolean(BOT_MOVE_SHAPE_ENABLE);
		c.turn90 = clampi(cm.getNumber(BOT_MOVE_TURN_COST_90), 0, 30);
		c.turn45 = clampi(cm.getNumber(BOT_MOVE_TURN_COST_45), 0, 30);
		c.heuristicPct = clampi(cm.getNumber(BOT_MOVE_HEURISTIC_PCT), 100, 200);
		c.noiseAmp = clampi(cm.getNumber(BOT_MOVE_NOISE_AMP), 0, 10);
		c.noiseScale = clampi(cm.getNumber(BOT_MOVE_NOISE_SCALE), 2, 16);
		c.noiseHabitPct = clampi(cm.getNumber(BOT_MOVE_NOISE_HABIT_PCT), 0, 100);
		c.diagPct = clampi(cm.getNumber(BOT_MOVE_DIAGONAL_PCT), 0, 100);
		c.diagCost = clampi(cm.getNumber(BOT_MOVE_DIAGONAL_COST), 10, 25);
		c.wallPct = clampi(cm.getNumber(BOT_MOVE_WALL_OFFSET_PCT), 0, 100);
		c.wallCost = clampi(cm.getNumber(BOT_MOVE_WALL_COST), 0, 10);
		c.wallCap = clampi(cm.getNumber(BOT_MOVE_WALL_COST_CAP), 0, 30);
		c.wallCostLow = clampi(cm.getNumber(BOT_MOVE_WALL_COST_LOW), 0, c.wallCost);
		c.cornerCost = clampi(cm.getNumber(BOT_MOVE_CORNER_COST), 0, 60);
		c.uturnPct = clampi(cm.getNumber(BOT_MOVE_UTURN_HESITATE_PCT), 0, 100);
		c.uturnMinMs = clampi(cm.getNumber(BOT_MOVE_UTURN_MIN_MS), 50, 5000);
		c.uturnMaxMs = clampi(cm.getNumber(BOT_MOVE_UTURN_MAX_MS), c.uturnMinMs, 5000);
		c.faceNpcPct = clampi(cm.getNumber(BOT_MOVE_FACE_NPC_PCT), 0, 100);
		c.faceSpeakerPct = clampi(cm.getNumber(BOT_MOVE_FACE_SPEAKER_PCT), 0, 100);
		c.facePlayerPct = clampi(cm.getNumber(BOT_MOVE_FACE_PLAYER_PCT), 0, 100);
		c.doorPausePct = clampi(cm.getNumber(BOT_MOVE_DOOR_PAUSE_PCT), 0, 100);
		c.stairPausePct = clampi(cm.getNumber(BOT_MOVE_STAIR_PAUSE_PCT), 0, 100);
		c.placeMinMs = clampi(cm.getNumber(BOT_MOVE_PLACE_PAUSE_MIN_MS), 50, 5000);
		c.placeMaxMs = clampi(cm.getNumber(BOT_MOVE_PLACE_PAUSE_MAX_MS), c.placeMinMs, 5000);
		c.pauseToPlacePct = clampi(cm.getNumber(BOT_MOVE_PAUSE_TO_PLACE_PCT), 0, 100);
		c.placeDeferMaxMs = clampi(cm.getNumber(BOT_MOVE_PLACE_PAUSE_DEFER_MAX_MS), 0, 20000);
		c.logAll = cm.getBoolean(BOT_MOVE_LOG_ALL);
		s_moveCfg = c;
		s_moveCfgAtMs = now;
		return s_moveCfg;
	}

	// Personality scaling: trait 0..15 -> 0.5x .. 1.5x of the configured chance, capped at 100.
	int32_t traitPct(int32_t pct, uint8_t trait) {
		if (pct <= 0) {
			return 0;
		}
		return std::min<int32_t>(100, pct * (15 + 2 * static_cast<int32_t>(trait)) / 30);
	}
	bool roll(int32_t pct) {
		return pct > 0 && uniform_random(1, 100) <= pct;
	}

	void dirVec(Direction d, int32_t& dx, int32_t& dy) {
		dx = dy = 0;
		switch (d) {
			case DIRECTION_NORTH: dy = -1; break;
			case DIRECTION_SOUTH: dy = 1; break;
			case DIRECTION_EAST: dx = 1; break;
			case DIRECTION_WEST: dx = -1; break;
			case DIRECTION_NORTHEAST: dx = 1; dy = -1; break;
			case DIRECTION_NORTHWEST: dx = -1; dy = -1; break;
			case DIRECTION_SOUTHEAST: dx = 1; dy = 1; break;
			case DIRECTION_SOUTHWEST: dx = -1; dy = 1; break;
			default: break;
		}
	}
	// internalCreatureTurn is 4-directional: face along the dominant axis.
	Direction cardinalToward(const Position& from, const Position& to) {
		const int32_t dx = static_cast<int32_t>(to.x) - static_cast<int32_t>(from.x);
		const int32_t dy = static_cast<int32_t>(to.y) - static_cast<int32_t>(from.y);
		if (dx == 0 && dy == 0) {
			return DIRECTION_NONE;
		}
		if (std::abs(dx) >= std::abs(dy)) {
			return dx > 0 ? DIRECTION_EAST : DIRECTION_WEST;
		}
		return dy > 0 ? DIRECTION_SOUTH : DIRECTION_NORTH;
	}
	Direction cardinalOf(Direction d) {
		int32_t dx, dy;
		dirVec(d, dx, dy);
		Position o(1000, 1000, 7);
		return cardinalToward(o, Position(static_cast<uint16_t>(1000 + dx), static_cast<uint16_t>(1000 + dy), 7));
	}
	int32_t cheb(const Position& a, const Position& b) {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
			std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	}
	bool leisureState(const BotState& bot) {
		return (bot.state == BotAIState::IDLE || bot.state == BotAIState::DWELLING || bot.state == BotAIState::TRAVELING)
			&& bot.attackerId == 0 && bot.huntTargetId == 0;
	}
} // namespace

bool BotEngine::botMoveObserved(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!player || bot.hibernated) {
		return false;
	}
	return player->getCastViewerCount() > 0 || botWalkObserved(bot, player);
}

// Per-event journal lines only for a cast-watched bot (the harness camera) unless botMoveLogAll.
bool BotEngine::botMoveLogFor(const std::shared_ptr<Player>& player) {
	return player && (player->getCastViewerCount() > 0 || s_moveCfg.logAll);
}

bool BotEngine::botMoveShapeFor(BotState& bot, const std::shared_ptr<Player>& player, const Position& target, BotPathShape& shape) {
	const MoveCfg& c = moveCfg();
	// Town and travel walking only: chase, flee, hunt patrols and combat keep the stock search
	// (their shape is set by the fight, and nothing here may slow a chase down).
	if (!c.shapeEnable || !leisureState(bot) || !botMoveObserved(bot, player)) {
		return false;
	}
	const int64_t now = OTSYS_TIME();
	MoveWalk& w = s_moveWalk[bot.guid];
	const bool fresh = w.atMs == 0 || now - w.atMs > MOVE_WALK_IDLE_MS || w.target.z != target.z || cheb(w.target, target) > 2;
	if (fresh) {
		w.target = target;
		w.salt = static_cast<uint32_t>(uniform_random(1, 0x7FFFFFFF)) ^ (bot.guid * 0x9E3779B1u);
		w.diag = roll(traitPct(c.diagPct, bot.walkBias()));
		w.wall = roll(traitPct(c.wallPct, bot.walkBias()));
		s_move60sWalks++;
		s_move60sDiag += w.diag ? 1 : 0;
		s_move60sWall += w.wall ? 1 : 0;
		if (botMoveLogFor(player)) {
			g_logger().info("[MOVE_WALK] bot='{}' from=({},{},{}) to=({},{},{}) salt={} diag={} wall={} wb={}",
				bot.name, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, target.x, target.y, target.z,
				w.salt, w.diag ? 1 : 0, w.wall ? 1 : 0, bot.walkBias());
		}
	}
	w.atMs = now;
	shape.cost.turnCost90 = c.turn90;
	shape.cost.turnCost45 = c.turn45;
	shape.cost.heuristicPct = c.heuristicPct;
	shape.cost.diagonalCost = w.diag ? c.diagCost : botnav::PC_DIAGONAL_COST;
	// #210: a walk that did not roll the offset still pays the low wall cost. With none at all the
	// turn cost holds a straight run along whatever wall the walk starts against (botnavsim: 48.5 %
	// avoidable wall contact vs 44.3 % for the stock search).
	shape.cost.wallCost = w.wall ? c.wallCost : c.wallCostLow;
	shape.cost.wallCostCap = c.wallCap;
	shape.cost.cornerCost = c.cornerCost;
	shape.noise.amp = c.noiseAmp;
	shape.noise.scale = c.noiseScale;
	shape.noise.habitPct = c.noiseHabitPct;
	shape.noise.walkSeed = w.salt;
	return true;
}

void BotEngine::botMoveNoteStep(BotState& bot) {
	const Position& a = bot.lastPos;
	const Position& b = bot.currentPos;
	if (a.z != b.z || a.x == 0 || cheb(a, b) > 2) {
		s_moveLastStep.erase(bot.guid);
		return;
	}
	MoveStep& s = s_moveLastStep[bot.guid];
	const int32_t dx = static_cast<int32_t>(b.x) - static_cast<int32_t>(a.x);
	const int32_t dy = static_cast<int32_t>(b.y) - static_cast<int32_t>(a.y);
	s.dx = static_cast<int8_t>((dx > 0) - (dx < 0));
	s.dy = static_cast<int8_t>((dy > 0) - (dy < 0));
	s.atMs = OTSYS_TIME();
}

bool BotEngine::botMoveHoldWalk(BotState& bot, const std::shared_ptr<Player>& player, const std::vector<Direction>& dirs) {
	if (dirs.empty() || !leisureState(bot) || bot.fcState != FloorChangeState::NONE) {
		return false;
	}
	const MoveCfg& c = moveCfg();
	const int64_t now = OTSYS_TIME();
	const uint8_t fid = bot.fidgetiness();

	// 1) A door / stair stop set just before this walk was issued.
	auto& holds = placeHolds();
	if (!holds.empty()) {
		if (auto h = holds.find(bot.guid); h != holds.end()) {
			const PlaceHold hold = h->second;
			holds.erase(h);
			const int64_t left = hold.untilMs - now;
			if (now - hold.setAtMs <= MOVE_HOLD_STALE_MS && left > 50) {
				player->listWalkDir.clear();
				player->stopEventWalk();
				scheduleWalkPause(bot, dirs, static_cast<int32_t>(left), true, bot.walkBias(), hold.kind);
				return true;
			}
		}
	}

	// 2) A U-turn: the first step of the new queue reverses (135 degrees or more) a step taken in
	// the last 1.5 s. A human stops, turns, then walks back; the bot used to snap round mid-stride.
	if (c.uturnPct <= 0) {
		return false;
	}
	auto st = s_moveLastStep.find(bot.guid);
	if (st == s_moveLastStep.end() || now - st->second.atMs > MOVE_UTURN_WINDOW_MS) {
		return false;
	}
	int32_t fx, fy;
	dirVec(dirs.back(), fx, fy); // startAutoWalk order: the first step is the BACK of the vector
	const int32_t inDir = botnav::pcDirIndex(st->second.dx, st->second.dy);
	const int32_t outDir = botnav::pcDirIndex(fx, fy);
	if (inDir < 0 || outDir < 0) {
		return false;
	}
	int32_t turn = std::abs(inDir - outDir);
	turn = turn > 4 ? 8 - turn : turn;
	if (turn < 3) {
		return false;
	}
	auto& lastUturn = s_moveUturnAtMs[bot.guid];
	if (now - lastUturn < MOVE_UTURN_COOLDOWN_MS || !botMoveObserved(bot, player)) {
		return false;
	}
	s_move60sUturnSeen++; // a watched U-turn that could hesitate: the denominator of the chance
	if (!roll(traitPct(c.uturnPct, fid))) {
		return false;
	}
	lastUturn = now;
	const int32_t ms = uniform_random(c.uturnMinMs, c.uturnMaxMs);
	player->listWalkDir.clear();
	player->stopEventWalk();
	const Direction face = cardinalOf(dirs.back());
	if (face != DIRECTION_NONE && face != player->getDirection()) {
		g_game().internalCreatureTurn(player, face);
	}
	s_move60sUturn++;
	if (botMoveLogFor(player)) {
		g_logger().info("[MOVE_HOLD] bot='{}' kind=uturn ms={} at=({},{},{}) turn={}", bot.name, ms,
			bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, turn * 45);
	}
	scheduleWalkPause(bot, dirs, ms, true, bot.walkBias(), "uturn");
	return true;
}

bool BotEngine::botMoveDeferPause(BotState& bot, int32_t pauseMs) {
	const MoveCfg& c = moveCfg();
	if (c.pauseToPlacePct <= 0 || c.placeDeferMaxMs <= 0 || !roll(traitPct(c.pauseToPlacePct, bot.fidgetiness()))) {
		return false;
	}
	deferredPauses()[bot.guid] = { std::min(pauseMs, c.placeDeferMaxMs), OTSYS_TIME() };
	s_move60sDeferred++;
	return true;
}

// Set the stop the next walk issue honours. A moved (deferred) pause is spent here first; otherwise
// the place's own chance rolls. Returns the hold in ms (0 = none).
static int32_t movePlaceHoldMs(BotState& bot, int32_t pct, int32_t capMs) {
	const MoveCfg& c = s_moveCfg;
	const int64_t now = OTSYS_TIME();
	auto& def = deferredPauses();
	if (auto it = def.find(bot.guid); it != def.end()) {
		const DeferredPause d = it->second;
		def.erase(it);
		if (now - d.atMs <= MOVE_DEFER_STALE_MS && d.ms > 0) {
			s_move60sDeferredSpent++;
			return std::min(d.ms, capMs);
		}
	}
	if (!roll(traitPct(pct, bot.fidgetiness()))) {
		return 0;
	}
	return std::min(uniform_random(c.placeMinMs, c.placeMaxMs), capMs);
}

void BotEngine::botMoveDoorOpened(BotState& bot, const std::shared_ptr<Player>& player, const Position& doorPos) {
	const MoveCfg& c = moveCfg();
	if (!leisureState(bot) || bot.fcState != FloorChangeState::NONE || !botMoveObserved(bot, player)) {
		return;
	}
	s_move60sDoorSeen++;
	const int32_t ms = movePlaceHoldMs(bot, c.doorPausePct, 20000);
	if (ms <= 0) {
		return;
	}
	const int64_t now = OTSYS_TIME();
	placeHolds()[bot.guid] = { now + ms, now, "door" };
	s_move60sDoor++;
	if (botMoveLogFor(player)) {
		g_logger().info("[MOVE_HOLD] bot='{}' kind=door ms={} door=({},{},{})", bot.name, ms, doorPos.x, doorPos.y, doorPos.z);
	}
}

bool BotEngine::botMoveStairHold(BotState& bot, const std::shared_ptr<Player>& player, const Position& stairPos) {
	const int64_t now = OTSYS_TIME();
	// Already rolled for this stair tile: keep waiting out the hold (FC machine use).
	auto& rolls = stairRolls();
	if (auto it = rolls.find(bot.guid); it != rolls.end() && it->second.pos == stairPos && now - it->second.atMs < MOVE_STAIR_REROLL_MS) {
		auto& holds = placeHolds();
		auto h = holds.find(bot.guid);
		return bot.fcState != FloorChangeState::NONE && h != holds.end() && now < h->second.untilMs;
	}
	const MoveCfg& c = moveCfg();
	if (!leisureState(bot) || !botMoveObserved(bot, player)) {
		return false;
	}
	rolls[bot.guid] = { stairPos, now }; // one roll per stair tile
	s_move60sStairSeen++;
	const int32_t ms = movePlaceHoldMs(bot, c.stairPausePct,
		bot.fcState != FloorChangeState::NONE ? MOVE_STAIR_HOLD_MAX_MS : 20000);
	if (ms <= 0) {
		return false;
	}
	placeHolds()[bot.guid] = { now + ms, now, "stairs" };
	const Direction face = cardinalToward(bot.currentPos, stairPos);
	if (face != DIRECTION_NONE && face != player->getDirection() && player->listWalkDir.empty()) {
		g_game().internalCreatureTurn(player, face);
	}
	s_move60sStair++;
	if (botMoveLogFor(player)) {
		g_logger().info("[MOVE_HOLD] bot='{}' kind=stairs ms={} stairs=({},{},{}) fc={}", bot.name, ms,
			stairPos.x, stairPos.y, stairPos.z, bot.fcState != FloorChangeState::NONE ? 1 : 0);
	}
	return bot.fcState != FloorChangeState::NONE;
}

void BotEngine::botMoveFaceCreature(BotState& bot, const std::shared_ptr<Player>& player, const Position& at, const char* why) {
	if (!player || at.z != bot.currentPos.z || !player->listWalkDir.empty() || cheb(bot.currentPos, at) > 8) {
		return;
	}
	const Direction face = cardinalToward(bot.currentPos, at);
	if (face == DIRECTION_NONE) {
		return;
	}
	if (face != player->getDirection()) {
		g_game().internalCreatureTurn(player, face);
	}
	if (botMoveLogFor(player)) {
		g_logger().info("[MOVE_FACE] bot='{}' why={} at=({},{},{}) dir={}", bot.name, why, at.x, at.y, at.z,
			static_cast<int>(face));
	}
}

void BotEngine::botMoveFaceNpc(BotState& bot, const std::shared_ptr<Player>& player) {
	const MoveCfg& c = moveCfg();
	if (!player || c.faceNpcPct <= 0 || !botMoveObserved(bot, player)) {
		return;
	}
	s_move60sFaceNpcSeen++;
	if (!roll(traitPct(c.faceNpcPct, bot.fidgetiness()))) {
		return;
	}
	std::shared_ptr<Creature> best;
	int32_t bestD = 99;
	for (const auto& cr : Spectators().find<Npc>(bot.currentPos, false, 4, 4, 4, 4)) {
		if (!cr || cr->getPosition().z != bot.currentPos.z) {
			continue;
		}
		const int32_t d = cheb(bot.currentPos, cr->getPosition());
		if (d < bestD) {
			bestD = d;
			best = cr;
		}
	}
	if (best) {
		s_move60sFaceNpc++;
		botMoveFaceCreature(bot, player, best->getPosition(), "npc");
	}
}

bool BotEngine::botMoveIdleFacing(BotState& bot, const std::shared_ptr<Player>& player) {
	const MoveCfg& c = moveCfg();
	if (!player || c.facePlayerPct <= 0 || !player->listWalkDir.empty() || !botMoveObserved(bot, player)) {
		return false;
	}
	const int64_t now = OTSYS_TIME();
	auto& next = glanceNextCheck()[bot.guid];
	if (now < next) {
		return false;
	}
	next = now + MOVE_GLANCE_RECHECK_MS;
	std::shared_ptr<Player> best;
	int32_t bestD = 99;
	for (const auto& s : Spectators().find<Player>(bot.currentPos, false, 4, 4, 4, 4)) {
		auto other = s ? s->getPlayer() : nullptr;
		if (!other || other->getID() == player->getID() || other->getPosition().z != bot.currentPos.z) {
			continue;
		}
		const int32_t d = cheb(bot.currentPos, other->getPosition());
		if (d > 0 && d < bestD) {
			bestD = d;
			best = other;
		}
	}
	if (!best) {
		return false;
	}
	// A player already glanced at is not stared at again on every check: only a NEW one in range
	// rolls, so the bot turns when someone walks up, not continuously.
	auto& glanced = s_moveGlanced[bot.guid];
	if (glanced == best->getID()) {
		return false;
	}
	glanced = best->getID();
	s_move60sFacePlayerSeen++;
	if (!roll(traitPct(c.facePlayerPct, bot.fidgetiness()))) {
		return false;
	}
	s_move60sFacePlayer++;
	botMoveFaceCreature(bot, player, best->getPosition(), "player");
	return true;
}

int32_t BotEngine::botMoveFaceSpeakerPct(const BotState& bot) {
	return traitPct(moveCfg().faceSpeakerPct, bot.fidgetiness());
}

void BotEngine::botMoveForget(uint32_t guid) {
	s_moveWalk.erase(guid);
	s_moveLastStep.erase(guid);
	s_moveUturnAtMs.erase(guid);
	stairRolls().erase(guid);
	s_moveGlanced.erase(guid);
	deferredPauses().erase(guid);
	placeHolds().erase(guid);
	glanceNextCheck().erase(guid);
}

std::string BotEngine::botMove60sSummary() {
	// #209: the bot with the most kernel misses this minute (acceptance: <= 60 on one target).
	uint32_t worstMisses = 0, worstGuid = 0;
	for (const auto& [guid, n] : s_move60sShapeFailByBot) {
		if (n > worstMisses) {
			worstMisses = n;
			worstGuid = guid;
		}
	}
	std::string worstName;
	if (worstGuid != 0) {
		if (auto it = guidToIndex_.find(worstGuid); it != guidToIndex_.end()) {
			worstName = fmt::format("@'{}'", bots_[it->second].name);
		}
	}
	const std::string out = fmt::format(
		"[MOVE] 60s walks={} shaped={} shapeFail={} diag={} wall={} uturn={}/{} faceNpc={}/{} faceSpeaker={}/{} facePlayer={}/{} door={}/{} stairs={}/{} deferred={} deferredSpent={} shapeSkip={} shapeFailMax={}{} laneWall={} plannerShaped={}",
		s_move60sWalks, s_move60sShaped, s_move60sShapeFail, s_move60sDiag, s_move60sWall, s_move60sUturn, s_move60sUturnSeen,
		s_move60sFaceNpc, s_move60sFaceNpcSeen, s_move60sFaceSpeaker, s_move60sFaceSpeakerSeen, s_move60sFacePlayer,
		s_move60sFacePlayerSeen, s_move60sDoor, s_move60sDoorSeen, s_move60sStair, s_move60sStairSeen, s_move60sDeferred,
		s_move60sDeferredSpent, s_move60sShapeSkip, worstMisses, worstName, s_move60sLaneWall, s_move60sPlannerShaped);
	s_move60sWalks = s_move60sShaped = s_move60sShapeFail = s_move60sDiag = s_move60sWall = 0;
	s_move60sShapeSkip = s_move60sLaneWall = s_move60sPlannerShaped = 0;
	s_move60sShapeFailByBot.clear();
	s_move60sUturn = s_move60sUturnSeen = 0;
	s_move60sFaceNpc = s_move60sFaceSpeaker = s_move60sFacePlayer = 0;
	s_move60sFaceNpcSeen = s_move60sFaceSpeakerSeen = s_move60sFacePlayerSeen = 0;
	s_move60sDoor = s_move60sStair = s_move60sDeferred = s_move60sDeferredSpent = 0;
	s_move60sDoorSeen = s_move60sStairSeen = 0;
	return out;
}
