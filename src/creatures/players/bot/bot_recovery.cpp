/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_recovery.cpp — #99 recovery rules: fruitless fights and death loops.
//
// FRUITLESS FIGHT. A fight is the run of ticks in which the bot has a target, closed by an 8 s
// gap with none. Progress is a target reaching a NEW HP low (a heal back up and the same hit again
// is not progress) or a kill. Sixty seconds without progress drops the fight:
//   - hunt:   the target type is avoided for 30 min, and the bot leaves along a patrol waypoint
//             the pathfinder accepts, at least 10 sqm from the target. If every target type of the
//             script is now avoided, the hunt is aborted.
//   - pk / combat (PvP): the caller's exitPK / exitCombat, which walk back to the pre-combat tile.
// That is the knight-vs-ghosts case (ghosts are 100 % physical immune): the per-target 10 s rule
// drops one ghost and the next one is picked up, forever. This rule spans the whole fight.
//
// DEATH LOOP. Every death records the creature type that was killing the bot and where. A second
// death to the same type inside 30 min makes the bot avoid that type for 30 min, marks both death
// spots, and aborts the hunt it would otherwise resume after the respawn. Hunt selection (live and
// the virtual twin) skips scripts that target an avoided type or patrol within 10 sqm of a marked
// spot.
//
// Everything is per bot and engine-lifetime (a reload forgets it, which is fine for 30-min rules).
// Telemetry: [FRUITLESS] drop|leave, [DEATHLOOP] record|avoid|abort|skip.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

namespace {
	std::string lowerName(const std::string& n) {
		std::string s = n;
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	int32_t cheb2d(const Position& a, const Position& b) {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		                std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	}
}

void BotEngine::fightProgressReset(uint32_t guid) {
	s_fightProgress.erase(guid);
}

void BotEngine::fightProgressKill(uint32_t guid) {
	auto it = s_fightProgress.find(guid);
	if (it != s_fightProgress.end()) it->second.lastProgressMs = OTSYS_TIME();
}

// Called every tick the bot is fighting `target`. Returns true exactly once, when the fight has
// gone FRUITLESS_MS without progress; the caller drops it.
bool BotEngine::fightProgressTick(BotState& bot, const std::shared_ptr<Creature>& target, const char* kind) {
	if (!target) return false;
	const int64_t now = OTSYS_TIME();
	auto& fp = s_fightProgress[bot.guid];
	if (fp.engagedSinceMs == 0 || now - fp.lastEngagedMs > FIGHT_OVER_GAP_MS) {
		fp = FightProgress {};
		fp.engagedSinceMs = now;
		fp.lastProgressMs = now;
	}
	fp.lastEngagedMs = now;

	const uint32_t id = target->getID();
	const int32_t hp = target->getHealth();
	auto it = fp.lowHp.find(id);
	if (it == fp.lowHp.end()) {
		fp.lowHp.emplace(id, hp); // a fresh target is not progress by itself
	} else if (hp < it->second) {
		it->second = hp;
		fp.lastProgressMs = now;
	}
	if (now - fp.lastProgressMs < FRUITLESS_MS) return false;

	const int32_t hpPct = target->getMaxHealth() > 0 ? hp * 100 / target->getMaxHealth() : 100;
	g_logger().info("[FRUITLESS] drop guid={} name='{}' kind={} target='{}' hp={}% fightMs={} noProgressMs={} targets={}",
		bot.guid, bot.name, kind, target->getName(), hpPct, now - fp.engagedSinceMs, now - fp.lastProgressMs,
		fp.lowHp.size());
	castLogError(bot, fmt::format("FRUITLESS: no progress on {} for {}s, dropping the fight",
		target->getName(), (now - fp.lastProgressMs) / 1000));
	if (target->getMonster()) {
		avoidCreatureType(bot.guid, target->getName(), now + DEATH_AVOID_MS, "fruitless");
	}
	s_fightProgress.erase(bot.guid);
	return true;
}

void BotEngine::avoidCreatureType(uint32_t guid, const std::string& name, int64_t untilMs, const char* why) {
	auto& m = s_avoidType[guid];
	auto& until = m[lowerName(name)];
	until = std::max(until, untilMs);
	g_logger().info("[DEATHLOOP] avoid guid={} type='{}' why={} forSec={}", guid, lowerName(name), why,
		(untilMs - OTSYS_TIME()) / 1000);
}

bool BotEngine::isCreatureTypeAvoided(uint32_t guid, const std::string& name) const {
	auto it = s_avoidType.find(guid);
	if (it == s_avoidType.end() || it->second.empty()) return false;
	auto jt = it->second.find(lowerName(name));
	return jt != it->second.end() && OTSYS_TIME() < jt->second;
}

bool BotEngine::isScriptDeathAvoided(uint32_t guid, const HuntScript& s) const {
	const int64_t now = OTSYS_TIME();
	if (auto it = s_avoidType.find(guid); it != s_avoidType.end() && !it->second.empty()) {
		for (const auto& t : s.targetNames) {
			auto jt = it->second.find(t); // targetNames are pre-lowered at load
			if (jt != it->second.end() && now < jt->second) return true;
		}
	}
	if (auto it = s_avoidSpots.find(guid); it != s_avoidSpots.end() && !it->second.empty()) {
		for (const auto& spot : it->second) {
			if (now >= spot.untilMs) continue;
			for (const auto& wp : s.patrolWaypoints) {
				if (wp.pos.z == spot.pos.z && cheb2d(wp.pos, spot.pos) <= DEATH_SPOT_RADIUS) return true;
			}
		}
	}
	return false;
}

// From pauseBotForDeath, BEFORE it clears the combat fields. The killer is the nearest monster
// that has the bot as its target, else the hunt target; a PvP death is not a creature-type loop.
void BotEngine::recordBotDeath(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!player) return;
	// #187 acceptance: did the bot die with an empty mana pool (potions not keeping up)?
	if (livenessCfg_.manaPotionEnable) {
		const auto lm = s_lastManaPct.find(bot.guid);
		g_logger().info("[BOT_MANA_DEATH] bot='{}' voc={} lvl={} lastManaPct={} state={}", bot.name,
			getBaseVocation(bot.vocationId), player->getLevel(), lm != s_lastManaPct.end() ? lm->second : -1,
			static_cast<int>(bot.state));
	}
	const int64_t now = OTSYS_TIME();
	const Position pos = player->getPosition();
	std::string type;
	int32_t best = 999;
	auto spectators = Spectators().find<Monster>(pos, false, 7, 7, 5, 5);
	for (const auto& spec : spectators) {
		auto m = spec->getMonster();
		if (!m || m->getHealth() <= 0 || m->getPosition().z != pos.z) continue;
		auto tgt = m->getAttackedCreature();
		if (!tgt || tgt->getID() != player->getID()) continue;
		const int32_t d = cheb2d(m->getPosition(), pos);
		if (d < best) {
			best = d;
			type = m->getName();
		}
	}
	if (type.empty() && bot.huntTargetId != 0) {
		auto t = g_game().getCreatureByID(bot.huntTargetId);
		if (t && t->getMonster()) type = t->getName();
	}
	if (type.empty()) return;
	type = lowerName(type);

	auto& deaths = s_botDeaths[bot.guid];
	deaths.erase(std::remove_if(deaths.begin(), deaths.end(),
		[&](const BotDeathRecord& d) { return now - d.ms > DEATH_LOOP_WINDOW_MS; }), deaths.end());
	deaths.push_back({ type, pos, now });
	int32_t same = 0;
	for (const auto& d : deaths) if (d.type == type) same++;
	g_logger().info("[DEATHLOOP] record guid={} name='{}' type='{}' pos=({},{},{}) sameType30m={} hunt={}",
		bot.guid, bot.name, type, pos.x, pos.y, pos.z, same, bot.huntScriptId);
	if (same < 2) return;

	avoidCreatureType(bot.guid, type, now + DEATH_AVOID_MS, "death_loop");
	auto& spots = s_avoidSpots[bot.guid];
	spots.erase(std::remove_if(spots.begin(), spots.end(),
		[&](const AvoidSpot& s) { return now >= s.untilMs; }), spots.end());
	for (const auto& d : deaths) {
		if (d.type == type) spots.push_back({ d.pos, now + DEATH_AVOID_MS });
	}
	if (bot.huntScriptId != 0 && bot.partyHuntId == 0) s_deathLoopAbortPending.insert(bot.guid);
}
