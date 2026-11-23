/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_spar.cpp — #87 sparring outside depots (design decided in #88).
//
// Two awake bots of a similar level, idling within SPAR_DEPOT_RADIUS of a depot POI, form a REAL
// Canary party and fight each other until either is below 30 % HP; then they stop and heal each
// other back to 90 %, and the party is disbanded.
//
// WHY A PARTY (verified in the 15.25 tree, not assumed): Player::onAttackedCreature
// (player.cpp ~6581) skips the whole skull / pz-lock block when `isPartner(targetPlayer)`, so a
// partner hit neither white-skulls the attacker nor pz-locks it; Player::onKilledPlayer
// (~6692) also exempts partners from frags. Combat::canDoCombat has no party rule, so partners
// CAN damage each other. The only party effect is getSkullClient's green "party" mark.
//
// THE HITS are the bot's own capped strikes, not weapon auto-attacks: a level-500 knight's melee
// can take ~60 % of a same-level sorcerer's health in one swing, which no 30 % stop line survives.
// Each strike is 4-8 % of the partner's max HP, never taking it below SPAR_FLOOR_HP_PCT, delivered
// through g_game().combatChangeHealth WITH the bot as the attacker (damage text, blood/element
// effect, partner attribution) after Player::onAttackedCreature, so the stock partner rule above is
// what keeps both skulls clear. A skull appearing anyway ends the spar at once ([SPAR] end
// reason=skull) — that is the tripwire for a server whose rules differ.
//
// NEVER A REAL PLAYER: both ends must be engine-owned bots. Pairing rules: level band
// (botSparLevelBandPct), per-bot cooldown, no rematch of the same pair for botSparRematchSec,
// at most botSparMaxPairs live pairs. Debug-pinned bots only spar by `/cavebot <bot> spar <other>`.
//
// VIRTUAL TWIN: a hibernated bot that settles at a depot POI may roll a simulated spar, which is a
// longer depot dwell bracketed by [SPAR] start/stop virtual=1. No damage, no party.
//
// Telemetry: [SPAR] start|stop|heal|end (+ virtual=1).
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

namespace {
	int32_t sparHpPct(const std::shared_ptr<Player>& p) {
		return (p && p->getMaxHealth() > 0) ? p->getHealth() * 100 / p->getMaxHealth() : 100;
	}

	int32_t sparCheb(const Position& a, const Position& b) {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		                std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	}

	bool sparPz(const Position& p) {
		auto t = g_game().map.getTile(p);
		return t && t->hasFlag(TILESTATE_PROTECTIONZONE);
	}

	// Simulated hibernated spars: guid -> dwell end. Read and written only in this TU.
	std::unordered_map<uint32_t, int64_t> s_sparVirtualUntil;

	const char* const kSparAsk[] = { "spar?", "wanna train a bit?", "duel? no kill", "sparring?", "fight me, to 30%" };
	const char* const kSparYes[] = { "sure", "ok", "lets go", "k", "bring it" };
	const char* const kSparEnd[] = { "gg", "nice", "good one", "gg wp", "thanks" };
}

bool BotEngine::sparNearDepot(const BotState& bot) const {
	auto it = cityPOIs_.find(bot.townId);
	if (it == cityPOIs_.end()) return false;
	const std::string towns = g_configManager().getString(BOT_SPAR_TOWNS);
	if (!towns.empty() && towns != "all") {
		bool listed = false;
		std::string cur;
		for (size_t i = 0; i <= towns.size(); i++) {
			if (i == towns.size() || towns[i] == ',') {
				while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
				while (!cur.empty() && cur.back() == ' ') cur.pop_back();
				if (!cur.empty() && strcasecmp(cur.c_str(), bot.townName.c_str()) == 0) listed = true;
				cur.clear();
			} else {
				cur += towns[i];
			}
		}
		if (!listed) return false;
	}
	for (const auto& poi : it->second) {
		if (poi.type != POIType::DEPOT && poi.type != POIType::DEPOT_OUTSIDE) continue;
		if (poi.pos.z == bot.currentPos.z && sparCheb(poi.pos, bot.currentPos) <= SPAR_DEPOT_RADIUS) return true;
	}
	return false;
}

bool BotEngine::sparEligible(const BotState& bot, bool forced) const {
	if (!bot.active || bot.hibernated || bot.deathPauseUntil > 0 || bot.advStoneActive) return false;
	if (bot.state != BotAIState::IDLE && bot.state != BotAIState::DWELLING) return false;
	if (bot.huntScriptId != 0 || bot.partyHuntId != 0) return false;
	if (isSparring(bot.guid) || s_gangByGuid.count(bot.guid) > 0 || s_rvMember.count(bot.guid) > 0) return false;
	auto player = bot.getPlayer();
	if (!player || player->isRemoved() || !player->isBotPlayer()) return false;
	if (player->getParty() || player->isPzLocked() || player->getSkull() != SKULL_NONE) return false;
	if (player->getLevel() < static_cast<uint32_t>(g_configManager().getNumber(PROTECTION_LEVEL))) return false;
	if (sparHpPct(player) < (forced ? 50 : 90)) return false;
	if (sparPz(bot.currentPos)) return false;
	if (!forced) {
		auto cd = s_sparCooldown.find(bot.guid);
		if (cd != s_sparCooldown.end() && OTSYS_TIME() < cd->second) return false;
	}
	return true;
}

std::string BotEngine::startSpar(BotState& a, BotState& b, bool forced) {
	if (a.guid == b.guid) return "Cannot spar with itself.";
	if (!sparEligible(a, forced)) return fmt::format("'{}' cannot spar now (state/party/skull/hp/pz/cooldown).", a.name);
	if (!sparEligible(b, forced)) return fmt::format("'{}' cannot spar now (state/party/skull/hp/pz/cooldown).", b.name);
	auto pa = a.getPlayer();
	auto pb = b.getPlayer();
	if (pa->getPosition().z != pb->getPosition().z || sparCheb(pa->getPosition(), pb->getPosition()) > 10) {
		return fmt::format("'{}' and '{}' are not close enough (same floor, <= 10 sqm).", a.name, b.name);
	}

	auto party = Party::create(pa);
	if (!party) return "Party::create failed.";
	if (!party->invitePlayer(pb) || !party->joinParty(pb) || !pa->isPartner(pb)) {
		party->disband();
		return fmt::format("'{}' could not join '{}''s party.", b.name, a.name);
	}

	const int64_t now = OTSYS_TIME();
	SparPair pr;
	pr.id = s_sparSeq++;
	pr.a = a.guid;
	pr.b = b.guid;
	pr.phase = SparPhase::APPROACH;
	pr.startMs = now;
	pr.phaseMs = now;
	pr.partyOurs = true;
	pr.forced = forced;
	s_sparPairs[pr.id] = pr;
	s_sparByGuid[a.guid] = pr.id;
	s_sparByGuid[b.guid] = pr.id;
	for (BotState* bs : { &a, &b }) {
		bs->hasWalkTarget = false;
		bs->currentPOI = nullptr;
		bs->followingCityRoute = false;
		auto p = bs->getPlayer();
		if (p && !p->listWalkDir.empty()) {
			p->listWalkDir.clear();
			p->stopEventWalk();
		}
		s_sparNextHit[bs->guid] = now + uniform_random(1500, 3000);
	}
	g_game().internalCreatureSay(pa, TALKTYPE_SAY, kSparAsk[uniform_random(0, 4)], false);
	g_game().internalCreatureSay(pb, TALKTYPE_SAY, kSparYes[uniform_random(0, 4)], false);
	g_logger().info("[SPAR] start pair={} a='{}' lv{} b='{}' lv{} town='{}' pos=({},{},{}) forced={} live={}",
		pr.id, a.name, pa->getLevel(), b.name, pb->getLevel(), a.townName,
		a.currentPos.x, a.currentPos.y, a.currentPos.z, forced ? 1 : 0, s_sparPairs.size());
	return fmt::format("Spar {} started: '{}' vs '{}' (party formed). Watch [SPAR].", pr.id, a.name, b.name);
}

void BotEngine::endSpar(uint32_t guid, const char* reason) {
	auto bit = s_sparByGuid.find(guid);
	if (bit == s_sparByGuid.end()) return;
	auto pit = s_sparPairs.find(bit->second);
	if (pit == s_sparPairs.end()) {
		s_sparByGuid.erase(bit);
		return;
	}
	const SparPair pr = pit->second;
	s_sparPairs.erase(pit);
	s_sparByGuid.erase(pr.a);
	s_sparByGuid.erase(pr.b);
	s_sparNextHit.erase(pr.a);
	s_sparNextHit.erase(pr.b);

	const int64_t now = OTSYS_TIME();
	auto pa = resolveBotPlayer(pr.a);
	auto pb = resolveBotPlayer(pr.b);
	const int skullA = pa ? static_cast<int>(pa->getSkull()) : -1;
	const int skullB = pb ? static_cast<int>(pb->getSkull()) : -1;
	// The party was ours (an all-bot party nobody else can be in); the shared reclaim helper is the
	// one that handles every leader/member/destroyed-leader case and refuses any party a real player
	// has joined.
	if (pr.partyOurs) {
		reclaimStaleCanaryParty(pr.a, "spar_end");
		reclaimStaleCanaryParty(pr.b, "spar_end");
	}
	s_sparCooldown[pr.a] = now + g_configManager().getNumber(BOT_SPAR_COOLDOWN_SEC) * 1000LL;
	s_sparCooldown[pr.b] = now + g_configManager().getNumber(BOT_SPAR_COOLDOWN_SEC) * 1000LL;
	s_sparRematch[sparPairKey(pr.a, pr.b)] = now + g_configManager().getNumber(BOT_SPAR_REMATCH_SEC) * 1000LL;

	const bool friendly = std::string_view(reason) == "done" || std::string_view(reason) == "heal_timeout";
	for (uint32_t g : { pr.a, pr.b }) {
		auto idx = guidToIndex_.find(g);
		if (idx == guidToIndex_.end()) continue;
		BotState& bs = bots_[idx->second];
		auto p = bs.getPlayer();
		if (p && p->getAttackedCreature()) p->setAttackedCreature(nullptr);
		if (bs.state == BotAIState::IDLE || bs.state == BotAIState::DWELLING) {
			bs.state = BotAIState::IDLE;
			bs.hasWalkTarget = false;
			bs.nextRerollTime = now + uniform_random(5, 20) * 1000LL;
		}
		if (friendly && p && uniform_random(1, 2) == 1) {
			g_game().internalCreatureSay(p, TALKTYPE_SAY, kSparEnd[uniform_random(0, 4)], false);
		}
	}
	g_logger().info("[SPAR] end pair={} reason={} durMs={} hits={} heals={} minHpA={}% minHpB={}% hpA={}% hpB={}% skullA={} skullB={} live={}",
		pr.id, reason, now - pr.startMs, pr.hits, pr.heals, pr.minHpPctA, pr.minHpPctB,
		pa ? sparHpPct(pa) : -1, pb ? sparHpPct(pb) : -1, skullA, skullB, s_sparPairs.size());
}

bool BotEngine::sparSuppressesHealing(const BotState& bot) const {
	auto bit = s_sparByGuid.find(bot.guid);
	if (bit == s_sparByGuid.end()) return false;
	auto pit = s_sparPairs.find(bit->second);
	if (pit == s_sparPairs.end()) return false;
	// HEAL phase: keep self-healing off until the partner's first heal landed. Otherwise doHealing
	// tops both up within a second of the stop and the pair "ends done" without ever healing each
	// other (heals=0), which is the whole point of the post-fight phase. The floor below still applies.
	if (pit->second.phase == SparPhase::HEAL && pit->second.heals > 0) return false;
	auto p = bot.getPlayer();
	return p && sparHpPct(p) >= SPAR_FLOOR_HP_PCT;
}

void BotEngine::checkSparStart(BotState& bot) {
	if (s_sparPairs.empty() && !g_configManager().getBoolean(BOT_SPAR_ENABLE)) return;
	if (!g_configManager().getBoolean(BOT_SPAR_ENABLE)) return;
	if (isSparring(bot.guid) || s_debugPinned.count(bot.guid) > 0) return;
	const int64_t now = OTSYS_TIME();
	auto& next = s_sparNextScan[bot.guid];
	if (now < next) return;
	next = now + SPAR_SCAN_MS + static_cast<int64_t>(bot.guid % 7) * 1000;
	if (static_cast<int64_t>(s_sparPairs.size()) >= g_configManager().getNumber(BOT_SPAR_MAX_PAIRS)) return;
	if (uniform_random(1, 100) > g_configManager().getNumber(BOT_SPAR_CHANCE_PCT)) return;
	if (!sparEligible(bot, false) || !sparNearDepot(bot)) return;
	auto self = bot.getPlayer();
	const int32_t myLevel = static_cast<int32_t>(self->getLevel());
	const int32_t band = static_cast<int32_t>(g_configManager().getNumber(BOT_SPAR_LEVEL_BAND_PCT));

	refreshSpectatorCacheIfStale(bot);
	BotState* best = nullptr;
	int32_t bestDist = 999;
	for (uint32_t pid : bot.cachedPlayerIds) {
		auto c = g_game().getCreatureByID(pid);
		auto p = c ? c->getPlayer() : nullptr;
		if (!p || p == self || !p->isBotPlayer()) continue; // real players are never paired
		auto idx = guidToIndex_.find(p->getGUID());
		if (idx == guidToIndex_.end()) continue;
		BotState& o = bots_[idx->second];
		if (s_debugPinned.count(o.guid) > 0) continue;
		if (o.currentPos.z != bot.currentPos.z) continue;
		const int32_t d = sparCheb(o.currentPos, bot.currentPos);
		if (d > SPAR_PARTNER_RADIUS || d >= bestDist) continue;
		const int32_t lv = static_cast<int32_t>(p->getLevel());
		if (std::abs(lv - myLevel) * 100 > band * std::max(lv, myLevel)) continue;
		auto rm = s_sparRematch.find(sparPairKey(bot.guid, o.guid));
		if (rm != s_sparRematch.end() && now < rm->second) continue;
		if (!sparEligible(o, false)) continue;
		best = &o;
		bestDist = d;
	}
	if (best) startSpar(bot, *best, false);
}

void BotEngine::sparHit(BotState& bot, const std::shared_ptr<Player>& self, const std::shared_ptr<Player>& other, SparPair& pr) {
	const Position from = self->getPosition();
	const Position to = other->getPosition();
	const int32_t dist = sparCheb(from, to);
	const uint8_t baseVoc = getBaseVocation(bot.vocationId);
	const bool melee = baseVoc == 4 || baseVoc == BOT_BASE_VOC_MONK; // knight, monk (#197: base 9, not 5)
	if (melee ? dist > 1 : dist > 4) return;
	if (!g_game().map.isSightClear(from, to, true)) return;

	const int32_t maxHp = other->getMaxHealth();
	const int32_t hp = other->getHealth();
	const int32_t floorHp = maxHp * SPAR_FLOOR_HP_PCT / 100 + 1;
	int32_t dmg = maxHp * uniform_random(4, 8) / 100;
	dmg = std::min(dmg, hp - floorHp);
	if (dmg <= 0) return;

	CombatType_t type = COMBAT_PHYSICALDAMAGE;
	if (baseVoc == 3) {
		g_game().addDistanceEffect(from, to, CONST_ANI_ARROW);
	} else if (baseVoc == 1) {
		g_game().addDistanceEffect(from, to, CONST_ANI_FIRE);
		type = COMBAT_FIREDAMAGE;
	} else if (baseVoc == 2) {
		g_game().addDistanceEffect(from, to, CONST_ANI_ICE);
		type = COMBAT_ICEDAMAGE;
	} else if (baseVoc == BOT_BASE_VOC_MONK) {
		g_game().addMagicEffect(to, CONST_ME_HITAREA); // #197: a punch, not a projectile
	}
	g_game().internalCreatureTurn(self, getDirectionTo(from, to));
	// The stock rule under test: for a partner this adds in-fight ticks only (no skull, no pz-lock).
	self->onAttackedCreature(other);

	CombatDamage damage;
	damage.primary.type = type;
	damage.primary.value = -dmg;
	g_game().combatChangeHealth(self, other, damage);
	pr.hits++;
	const int32_t pct = sparHpPct(other);
	if (pr.a == other->getGUID()) pr.minHpPctA = std::min(pr.minHpPctA, pct);
	else pr.minHpPctB = std::min(pr.minHpPctB, pct);
	bot.lastAttackTime = OTSYS_TIME();
}

bool BotEngine::sparHeal(BotState& bot, const std::shared_ptr<Player>& self, const std::shared_ptr<Player>& other, SparPair& pr) {
	if (sparHpPct(other) >= SPAR_HEALED_HP_PCT) return false; // each heals the OTHER; self via doHealing
	const int32_t before = sparHpPct(other);
	const uint8_t baseVoc = getBaseVocation(bot.vocationId);
	const char* how = nullptr;

	// 1. A druid's real "exura sio".
	if (baseVoc == 2 && self->getLevel() >= 18) {
		std::string words = fmt::format("exura sio \"{}\"", other->getName());
		if (g_spells().playerSaySpell(self, words) == TALKACTION_BREAK) {
			self->saySpell(TALKTYPE_SAY, fmt::format("exura sio \"{}\"", other->getName()), false);
			how = "sio";
		}
	}
	// 1b. #197: a monk's real "exura tio sio" (Restore Balance, level 18).
	if (!how && baseVoc == BOT_BASE_VOC_MONK && self->getLevel() >= 18 && monkHealOther(bot, other, "spar")) {
		how = "tio_sio";
	}
	// 2. A real ultimate healing rune from the backpack.
	if (!how && sparCheb(self->getPosition(), other->getPosition()) <= RUNE_RANGE) {
		if (auto rune = findRuneInBackpack(self, 3160)) {
			if (executeRune(bot, rune, other->getPosition(), other)) how = "uh";
		}
	}
	// 3. Most bots carry no UH: the same rune's look and size, applied directly.
	if (!how) {
		const int32_t amount = other->getMaxHealth() * uniform_random(15, 25) / 100;
		g_game().addDistanceEffect(self->getPosition(), other->getPosition(), CONST_ANI_SMALLHOLY);
		g_game().addMagicEffect(other->getPosition(), CONST_ME_MAGIC_BLUE);
		CombatDamage heal;
		heal.primary.type = COMBAT_HEALING;
		heal.primary.value = amount;
		g_game().combatChangeHealth(self, other, heal);
		how = "uh_sim";
	}
	pr.heals++;
	g_logger().info("[SPAR] heal pair={} by='{}' to='{}' how={} hp={}%->{}%", pr.id, self->getName(),
		other->getName(), how, before, sparHpPct(other));
	return true;
}

// Called at the top of doIdle / doDwelling. true = the bot is sparring and the tick is consumed.
bool BotEngine::handleSpar(BotState& bot) {
	auto bit = s_sparByGuid.find(bot.guid);
	if (bit == s_sparByGuid.end()) return false;
	auto pit = s_sparPairs.find(bit->second);
	if (pit == s_sparPairs.end()) {
		s_sparByGuid.erase(bit);
		return false;
	}
	SparPair& pr = pit->second;
	const uint32_t otherGuid = pr.a == bot.guid ? pr.b : pr.a;
	auto oIdx = guidToIndex_.find(otherGuid);
	BotState* ob = oIdx != guidToIndex_.end() ? &bots_[oIdx->second] : nullptr;
	auto self = bot.getPlayer();
	auto other = ob ? ob->getPlayer() : nullptr;

	const char* abort = nullptr;
	if (!self) abort = "self_gone";
	else if (!ob || !other || other->isRemoved() || !ob->active || ob->hibernated) abort = "partner_gone";
	else if (ob->state != BotAIState::IDLE && ob->state != BotAIState::DWELLING) abort = "partner_busy";
	else if (!self->isPartner(other)) abort = "party_lost";
	else if (self->getSkull() != SKULL_NONE || other->getSkull() != SKULL_NONE) abort = "skull";
	else if (other->getPosition().z != self->getPosition().z
	         || sparCheb(other->getPosition(), self->getPosition()) > 12) abort = "separated";
	if (abort) {
		if (std::string_view(abort) == "skull") {
			g_logger().warn("[SPAR] SKULL during spar pair={} '{}'={} '{}'={} -- partner rule did not hold",
				pr.id, bot.name, self ? static_cast<int>(self->getSkull()) : -1,
				ob ? ob->name : "?", other ? static_cast<int>(other->getSkull()) : -1);
		}
		endSpar(bot.guid, abort);
		return false;
	}

	const int64_t now = OTSYS_TIME();
	const Position me = self->getPosition();
	const Position op = other->getPosition();
	const int32_t dist = sparCheb(me, op);
	const uint8_t baseVoc = getBaseVocation(bot.vocationId);
	const bool melee = baseVoc == 4 || baseVoc == BOT_BASE_VOC_MONK;
	const int32_t want = melee ? 1 : 3;
	const int32_t hpSelf = sparHpPct(self);
	const int32_t hpOther = sparHpPct(other);
	auto& nextAct = s_sparNextHit[bot.guid];

	auto closeIn = [&]() {
		if (dist > want && self->listWalkDir.empty() && bot.fcState == FloorChangeState::NONE) {
			goTo(bot, op, want);
		}
	};

	switch (pr.phase) {
		case SparPhase::APPROACH: {
			if (now - pr.phaseMs > SPAR_APPROACH_MAX_MS) {
				endSpar(bot.guid, "approach_timeout");
				return false;
			}
			closeIn();
			// FIGHT once both ends are in range: a melee end needs adjacency, two ranged ends 3 sqm.
			const uint8_t oVoc = getBaseVocation(ob->vocationId);
			const int32_t oWant = (oVoc == 4 || oVoc == BOT_BASE_VOC_MONK) ? 1 : 3;
			if (dist <= std::min(want, oWant)) {
				pr.phase = SparPhase::FIGHT;
				pr.phaseMs = now;
			}
			return true;
		}
		case SparPhase::FIGHT: {
			if (hpSelf < SPAR_STOP_HP_PCT || hpOther < SPAR_STOP_HP_PCT || now - pr.phaseMs > SPAR_FIGHT_MAX_MS) {
				const bool timedOut = hpSelf >= SPAR_STOP_HP_PCT && hpOther >= SPAR_STOP_HP_PCT;
				g_logger().info("[SPAR] stop pair={} reason={} '{}'={}% '{}'={}% hits={} fightMs={}", pr.id,
					timedOut ? "time" : "hp", bot.name, hpSelf, ob->name, hpOther, pr.hits, now - pr.phaseMs);
				pr.phase = SparPhase::HEAL;
				pr.phaseMs = now;
				if (self->getAttackedCreature()) self->setAttackedCreature(nullptr);
				if (other->getAttackedCreature()) other->setAttackedCreature(nullptr);
				nextAct = now + uniform_random(600, 1500);
				s_sparNextHit[otherGuid] = now + uniform_random(600, 1500);
				return true;
			}
			closeIn();
			if (now >= nextAct && !sparPz(me) && !sparPz(op)) {
				sparHit(bot, self, other, pr);
				nextAct = now + uniform_random(1800, 2600);
			}
			return true;
		}
		case SparPhase::HEAL: {
			if (hpSelf >= SPAR_HEALED_HP_PCT && hpOther >= SPAR_HEALED_HP_PCT) {
				endSpar(bot.guid, "done");
				return false;
			}
			if (now - pr.phaseMs > SPAR_HEAL_MAX_MS) {
				endSpar(bot.guid, "heal_timeout");
				return false;
			}
			if (now >= nextAct) {
				if (sparHeal(bot, self, other, pr)) nextAct = now + uniform_random(1500, 2500);
				else nextAct = now + 500;
			}
			return true;
		}
	}
	return true;
}

// Virtual twin: called when a hibernated bot settles into a dwell at a depot POI. A simulated
// spar is a longer dwell, bracketed by [SPAR] start/stop virtual=1, under the same pair cap.
void BotEngine::virtualMaybeSpar(BotState& bot) {
	const int64_t now = OTSYS_TIME();
	// Expire finished simulated spars first (their dwell ran out or the bot moved on).
	for (auto it = s_sparVirtualUntil.begin(); it != s_sparVirtualUntil.end();) {
		if (now >= it->second) {
			g_logger().info("[SPAR] stop virtual=1 guid={}", it->first);
			it = s_sparVirtualUntil.erase(it);
		} else {
			++it;
		}
	}
	if (!g_configManager().getBoolean(BOT_SPAR_ENABLE)) return;
	if (!bot.currentPOI || (bot.currentPOI->type != POIType::DEPOT && bot.currentPOI->type != POIType::DEPOT_OUTSIDE)) return;
	if (s_sparVirtualUntil.count(bot.guid) > 0 || bot.huntScriptId != 0 || bot.partyHuntId != 0) return;
	auto cd = s_sparCooldown.find(bot.guid);
	if (cd != s_sparCooldown.end() && now < cd->second) return;
	if (static_cast<int64_t>(s_sparVirtualUntil.size()) >= g_configManager().getNumber(BOT_SPAR_MAX_PAIRS) * 2) return;
	if (uniform_random(1, 100) > g_configManager().getNumber(BOT_SPAR_CHANCE_PCT)) return;
	const std::string& savedTown = bot.townName;
	if (!sparNearDepot(bot)) return;
	const int32_t secs = uniform_random(60, 180);
	bot.dwellUntil = std::max<int64_t>(bot.dwellUntil, now + static_cast<int64_t>(secs) * 1000);
	s_sparVirtualUntil[bot.guid] = bot.dwellUntil;
	s_sparCooldown[bot.guid] = bot.dwellUntil + g_configManager().getNumber(BOT_SPAR_COOLDOWN_SEC) * 1000LL;
	g_logger().info("[SPAR] start virtual=1 guid={} name='{}' town='{}' dwellSec={}", bot.guid, bot.name, savedTown, secs);
}
