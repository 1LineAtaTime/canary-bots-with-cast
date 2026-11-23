/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_monk.cpp — #197 monk support: the harmony rotation, virtues, Serene telemetry, the party
// monk. Design: .work/research_2026-11-23/90_monk_support.md §3 and §7.
//
// WHY A RULE LADDER: every monk attack spell scores the same in the damage maximiser
// (SKILLVALUE -> getMaxWeaponDamage*0.5), and the maximiser has no notion of state. The monk's
// whole value is state: builders add one harmony point each (Spell::postCastSpell -> buildHarmony),
// a spender consumes all of them and multiplies its damage by 1 + base*2^(h-1)/100 — 1.08 at h=1,
// 2.28 at h=5 (3.56 with Virtue of Harmony while Serene). So: build to 5, then spend. A spender at
// h<5 throws most of its value away, except to finish a target (the execute case).
//
// The server does everything else: harmony gain/spend, the spender's builder-cooldown refund, the
// Spiritual Outburst recast at h=5, Serene (Player::onThink bot block, #197 core fix). This TU only
// decides WHICH spell to say, exactly as a player at the hotkeys would.
//
// Telemetry (closed loop):
//   [MONK] build|spend|focus|virtue|heal ...  per cast, cast-watched / verbose bots (botMonkLogAll: all)
//   [ULT_AOE] cast ... spell='exori gran mas nia'  every ultimate cast (same format as castAoeSpell)
//   [ULT_AOE] skip ...                             PvE ultimate refused (ultAoeAllowed, watched)
//   [BOT_SERENE] on|off ...                        Serene changes on a watched monk
//   [MONK_STATS] ...                               every 10 min when any monk cast anything
// Acceptance: [MONK_STATS] spendLow (a PvE spender below 5 harmony that was NOT an execute) == 0;
// ult casts with monsters < botUltAoeMinMonsters in PvE == 0; 0 POFF / SPELL_FAIL on monk words.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

namespace {

// Words, data/scripts/spells/** (all `monk;true, exalted monk;true`).
constexpr const char* MK_SWIFT_JAB = "exori infir pug";         // builder, lv0 (used below 14)
constexpr const char* MK_DOUBLE_JAB = "exori pug";              // builder, lv14, melee
constexpr const char* MK_MYSTIC_REPULSE = "exori amp pug";      // builder, lv30, range 7, 20 s
constexpr const char* MK_FLURRY = "exori mas pug";              // builder, lv35, directional area
constexpr const char* MK_CHAINED_PENANCE = "exori med pug";     // builder, lv70, chain 3 (hop 3)
constexpr const char* MK_GREATER_FLURRY = "exori gran mas pug"; // builder, lv90, directional area, 16 s
constexpr const char* MK_UPPERCUT = "exori gran pug";           // builder, lv110, melee, 60 s
constexpr const char* MK_TIGER_CLASH = "exori infir nia";       // spender, lv0 (used below 18)
constexpr const char* MK_GREATER_TIGER = "exori nia";           // spender, lv18, melee, 8 s
constexpr const char* MK_SWEEPING = "exori mas nia";            // spender, lv60, directional area, 8 s
constexpr const char* MK_KNOCKOUT = "exori gran nia";           // spender, lv125, melee, 24 s
constexpr const char* MK_OUTBURST = "exori gran mas nia";       // spender, ULTIMATE: chain 6 (hop 2), 24 s
constexpr const char* MK_FOCUS_SERENITY = "utamo tio";          // lv150, 10 min: Serene 7 s + cooldowns + harmony 5
constexpr const char* MK_FOCUS_HARMONY = "utevo nia";           // lv275, 2 min: harmony 5
constexpr const char* MK_RESTORE_BALANCE = "exura tio sio";     // lv18, heal another player, range 7
constexpr const char* MK_MASS_SPIRIT_MEND = "exura mas nia";    // lv150, spender: harmony-scaled party heal
constexpr const char* MK_ENLIGHTEN_PARTY = "utevo mas sio";     // lv32, 5 min party mana regeneration
constexpr const char* MK_MENTOR_OTHER = "uteta tio";            // lv150, 50 s per-vocation buff on another

// Spiritual Outburst is a WoD revelation spell (registered level 0, bots pass the stage check at 1).
// A real monk unlocks it with wheel points; a level-30 bot throwing it would look wrong.
constexpr int32_t MONK_ULT_MIN_LEVEL = 150;
constexpr int32_t MONK_PVE_EXECUTE_PCT = 15;     // a spender below 5 harmony may finish a target this low
constexpr int32_t MONK_PVP_KILL_WINDOW_PCT = 45; // PvP: spend at h>=3 / Focus Serenity from here
constexpr int64_t MONK_VIRTUE_PACE_MS = 60000;   // non-urgent virtue changes at most once a minute
constexpr int64_t MONK_STATS_EVERY_MS = 600000;

struct MonkStats {
	uint64_t builds = 0, spends = 0, spendLow = 0, executes = 0;
	uint64_t ultCasts = 0, ultSkips = 0, focus = 0, virtues = 0, heals = 0, fails = 0;
};
MonkStats s_monkStats;
int64_t s_monkStatsNextMs = 0;
std::unordered_map<uint32_t, int64_t> s_monkVirtueNextMs;
std::unordered_map<uint32_t, uint8_t> s_monkSereneSeen; // 0 = never seen, 1 = off, 2 = on
// Nothing was castable (every candidate cooling down): re-evaluate in 400 ms, not every tick.
std::unordered_map<uint32_t, int64_t> s_monkNextEvalMs;
constexpr int64_t MONK_IDLE_REEVAL_MS = 400;

int32_t monkCheb(const Position& a, const Position& b) {
	return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
}

int32_t monkHpPct(const std::shared_ptr<Creature>& c) {
	if (!c || c->getMaxHealth() <= 0) return 100;
	return static_cast<int32_t>(static_cast<int64_t>(c->getHealth()) * 100 / c->getMaxHealth());
}

const char* virtueName(Virtue_t v) {
	switch (v) {
		case Virtue_t::Harmony: return "harmony";
		case Virtue_t::Justice: return "justice";
		case Virtue_t::Sustain: return "sustain";
		default: return "none";
	}
}

const char* virtueWords(Virtue_t v) {
	switch (v) {
		case Virtue_t::Harmony: return "utori virtu";
		case Virtue_t::Justice: return "utito virtu";
		case Virtue_t::Sustain: return "utura tio";
		default: return nullptr;
	}
}

// Registry entry when the spell exists, the level allows it and no cooldown (own id, group,
// secondary group) blocks it. Mana is the caller's call: attack and support spells pay it
// differently (attackManaOk keeps the heal reserve).
std::shared_ptr<InstantSpell> monkReady(const std::shared_ptr<Player>& p, const char* words, int32_t minLevel) {
	if (!p || p->getLevel() < static_cast<uint32_t>(std::max(0, minLevel))) return nullptr;
	auto sp = g_spells().getInstantSpell(words);
	if (!sp) return nullptr;
	if (p->getLevel() < sp->getLevel()) return nullptr;
	if (p->hasCondition(CONDITION_SPELLCOOLDOWN, sp->getSpellId())) return nullptr;
	if (p->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, sp->getGroup())) return nullptr;
	const SpellGroup_t sec = sp->getSecondaryGroup();
	if (sec != SPELLGROUP_NONE && p->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, sec)) return nullptr;
	return sp;
}

bool partyHasHealerNear(const std::shared_ptr<Player>& self, const std::shared_ptr<Party>& party, int32_t range) {
	if (!self || !party) return false;
	const Position me = self->getPosition();
	auto isNearDruid = [&](const std::shared_ptr<Player>& m) {
		if (!m || m.get() == self.get() || m->isRemoved() || m->getHealth() <= 0) return false;
		const auto voc = m->getVocation();
		if (!voc || voc->getBaseId() != 2) return false;
		const Position mp = m->getPosition();
		return mp.z == me.z && monkCheb(me, mp) <= range;
	};
	if (isNearDruid(party->getLeader())) return true;
	for (const auto& m : party->getMembers()) {
		if (isNearDruid(m)) return true;
	}
	return false;
}

} // namespace

// ============================================================================
// Chain simulation (Combat::pickChainTargets, combat.cpp ~1874)
// ============================================================================
int32_t BotEngine::monkChainCount(BotState& bot, int32_t hop, int32_t maxTargets, uint32_t targetId, bool pvp,
	bool& outUnsafe, int32_t* outPlayers) {
	outUnsafe = false;
	if (outPlayers) *outPlayers = 0;
	const auto self = bot.getPlayer();
	if (!self || hop <= 0 || maxTargets <= 0) return 0;
	const Position me = self->getPosition();
	const int32_t reach = std::min(hop * maxTargets, 10);

	struct Cand { Position pos; uint8_t kind; }; // 0 monster, 1 hostile player, 2 bystander
	std::vector<Cand> cands;
	for (const auto& spec : Spectators().find<Creature>(me, false, reach, reach, reach, reach)) {
		if (!spec || spec.get() == self.get() || spec->isRemoved() || spec->getHealth() <= 0) continue;
		if (spec->getNpc()) continue; // canChain: never an NPC
		const Position p = spec->getPosition();
		if (p.z != me.z) continue;
		if (const auto tile = spec->getTile(); tile && tile->hasFlag(TILESTATE_PROTECTIONZONE)) continue; // canChain
		if (const auto pl = spec->getPlayer()) {
			if (isAoeEnemyPlayer(bot, self, pl, targetId)) {
				cands.push_back({ p, 1 });
			} else if (aoeWouldDamage(self, pl)) {
				cands.push_back({ p, 2 }); // the server WOULD hit this player: never chain into it
			}
			continue;
		}
		if (Combat::canDoCombat(self, spec, true) != RETURNVALUE_NOERROR) continue; // isValidChainTarget
		cands.push_back({ p, 0 });
	}

	// The server hops to the Euclidean-nearest valid creature within `hop` (a square query) of the
	// previous link, with line of sight between the two, starting at the caster.
	std::vector<bool> used(cands.size(), false);
	Position cur = me;
	int32_t count = 0;
	while (count < maxTargets) {
		int32_t best = -1;
		int64_t bestD = std::numeric_limits<int64_t>::max();
		for (size_t i = 0; i < cands.size(); ++i) {
			if (used[i] || monkCheb(cur, cands[i].pos) > hop) continue;
			const int64_t dx = static_cast<int64_t>(cands[i].pos.x) - cur.x;
			const int64_t dy = static_cast<int64_t>(cands[i].pos.y) - cur.y;
			const int64_t d = dx * dx + dy * dy;
			if (d >= bestD) continue;
			if (!g_game().map.isSightClear(cur, cands[i].pos, true)) continue;
			bestD = d;
			best = static_cast<int32_t>(i);
		}
		if (best < 0) break;
		used[best] = true;
		const Cand& c = cands[best];
		if (c.kind == 2) {
			outUnsafe = true;
			return count;
		}
		if (c.kind == 1) {
			if (!pvp) {
				outUnsafe = true; // a hostile player outside a PvP context is still not a monster target
				return count;
			}
			if (outPlayers) ++*outPlayers;
		}
		++count;
		cur = c.pos;
	}
	return count;
}

// ============================================================================
// The rotation
// ============================================================================
bool BotEngine::castMonkSpell(BotState& bot, const std::shared_ptr<Creature>& target, bool singleTargetOnly) {
	auto player = bot.getPlayer();
	if (!player || !target) return false;

	// Every attack spell shares the 2 s "attack" group: while it runs nothing below is castable,
	// so skip the spectator scans (castSpell is reached every combat tick).
	if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, SPELLGROUP_ATTACK)) return false;

	const int64_t now = OTSYS_TIME();
	if (auto ne = s_monkNextEvalMs.find(bot.guid); ne != s_monkNextEvalMs.end() && now < ne->second) return false;
	const int32_t level = static_cast<int32_t>(player->getLevel());
	const Position me = player->getPosition();
	const Position tpos = target->getPosition();
	const int32_t dist = monkCheb(me, tpos);
	const bool adjacent = dist <= 1;
	const bool targetIsPlayer = target->getPlayer() != nullptr;
	const bool pvp = targetIsPlayer || ultAoePvpContext(bot, target);
	const int32_t tHp = monkHpPct(target);
	const uint32_t targetId = target->getID();

	// needTarget spells read the attacked creature.
	if (player->getAttackedCreature() != target) {
		player->setAttackedCreature(target);
	}
	if (pvp) {
		monkEnsureVirtue(bot, Virtue_t::Justice, "pvp", true); // support group: no attack slot spent
	}

	const bool watched = livenessCfg_.monkLogAll || bot.verboseLog || getDebugCfg(bot.guid) != nullptr
		|| player->getCastViewerCount() > 0;

	// Mana for an ATTACK: the #187 heal reserve, then the legacy refill path (PvP-budgeted) when on.
	auto attackMana = [&](const std::shared_ptr<InstantSpell>& sp) -> bool {
		const uint32_t cost = sp->getManaCost(player);
		if (!attackManaOk(player, cost)) return false;
		if (player->getMana() >= cost) return true;
		if (!legacyManaRefill()) return false;
		if (targetIsPlayer) {
			const uint32_t maxMana = player->getMaxMana();
			if (bot.pvpManaSpent >= maxMana * 5 / 2) return false;
			bot.pvpManaSpent += maxMana;
		}
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
		return true;
	};

	auto logCast = [&](const char* kind, const char* words, uint8_t hBefore, int32_t n) {
		if (!watched) return;
		g_logger().info("[MONK] {} bot='{}' lvl={} h={}->{} spell='{}' n={} target='{}' hp={}% dist={} pvp={} serene={} virtue={} state={}",
			kind, bot.name, level, hBefore, player->getHarmony(), words, n, target->getName(), tHp, dist, pvp ? 1 : 0,
			player->hasCondition(CONDITION_SERENE) ? 1 : 0, virtueName(player->getVirtue()), botStateName(bot.state));
	};

	// Attack cast. `kind`: build / spend. Faces `dir` first for the directional areas.
	auto castAttack = [&](const char* words, const char* kind, int32_t n, const Direction* dir, bool execute) -> bool {
		if (dir) {
			g_game().internalCreatureTurn(player, *dir);
		} else if (dist >= 1) {
			g_game().internalCreatureTurn(player, getDirectionTo(me, tpos));
		}
		const uint8_t hBefore = player->getHarmony();
		std::string w = words;
		const auto result = g_spells().playerSaySpell(player, w);
		bot.lastAttackTime = now; // the generic path throttles failures the same way
		if (result != TALKACTION_BREAK) {
			s_monkStats.fails++;
			castLog(bot, fmt::format("MONK_FAIL: {} -> {} dist={} h={} result={}", words, target->getName(), dist,
				hBefore, static_cast<int>(result)));
			return false;
		}
		player->saySpell(TALKTYPE_SAY, words, false);
		bot.lastCombatProgress = now;
		if (targetIsPlayer) bot.lastPvpAttackTime = now;
		if (kind[0] == 's') {
			s_monkStats.spends++;
			if (hBefore < 5) {
				if (execute) s_monkStats.executes++;
				else if (!pvp) s_monkStats.spendLow++;
			}
		} else {
			s_monkStats.builds++;
		}
		castLog(bot, fmt::format("MONK: {} {} h={}->{} n={} -> {} ({}%)", kind, words, hBefore, player->getHarmony(), n,
			target->getName(), tHp));
		logCast(kind, words, hBefore, n);
		return true;
	};

	// Support-group cast that precedes a spend in the same slot (focus spells).
	auto castFocus = [&](const char* words) -> bool {
		auto sp = monkReady(player, words, 0);
		if (!sp) return false;
		const uint32_t cost = sp->getManaCost(player);
		if (!attackManaOk(player, cost) || player->getMana() < cost) return false;
		const uint8_t hBefore = player->getHarmony();
		std::string w = words;
		if (g_spells().playerSaySpell(player, w) != TALKACTION_BREAK) {
			s_monkStats.fails++;
			return false;
		}
		player->saySpell(TALKTYPE_SAY, words, false);
		s_monkStats.focus++;
		castLog(bot, fmt::format("MONK: focus {} h={}->{}", words, hBefore, player->getHarmony()));
		logCast("focus", words, hBefore, 0);
		return true;
	};

	// Nearby monsters for the directional areas (LOS is checked per direction).
	std::vector<std::shared_ptr<Creature>> nearby;
	int32_t adjacentMonsters = 0;
	for (const auto& spec : Spectators().find<Monster>(me, false, 5, 5, 5, 5)) {
		if (!spec || spec->isRemoved() || spec->getHealth() <= 0) continue;
		const Position p = spec->getPosition();
		if (p.z != me.z) continue;
		nearby.push_back(spec);
		if (monkCheb(me, p) <= 1) ++adjacentMonsters;
	}
	std::vector<std::shared_ptr<Player>> enemies;
	if (pvp) {
		enemies = collectAoeEnemyPlayers(bot, targetId);
		for (const auto& e : enemies) nearby.push_back(e);
	}

	const auto& aoeList = resolvedAoeSpells_[BOT_BASE_VOC_MONK];
	auto findAoe = [&](const char* words) -> const ResolvedSpell* {
		for (const auto& s : aoeList) {
			if (s.words == words) return &s;
		}
		return nullptr;
	};
	// Best cardinal direction and its hit count for a directional monk area; 0 when it would clip
	// a player the server would damage who is not a hostile (aoeClipsBystander, the AoE rule).
	auto waveCount = [&](const char* words, Direction& outDir) -> int32_t {
		const ResolvedSpell* rs = findAoe(words);
		if (!rs || nearby.empty()) return 0;
		int32_t cnt = 0;
		outDir = findBestWaveDirection(bot, rs, nearby, cnt);
		if (cnt > 0 && !aoeClipsBystander(bot, rs, outDir, nullptr, targetId, targetIsPlayer, "monk").empty()) return 0;
		return cnt;
	};

	// The ultimate, through the shared ultimate-AoE gate (isExplicitUltimateAoe marks it).
	const ResolvedSpell* ultRs = nullptr;
	for (const auto& s : resolvedSingleSpells_[BOT_BASE_VOC_MONK]) {
		if (s.words == MK_OUTBURST) {
			ultRs = &s;
			break;
		}
	}
	static const ResolvedSpell kUltFallback = [] {
		ResolvedSpell r;
		r.words = MK_OUTBURST;
		r.isUltimateAoe = true;
		return r;
	}();
	if (!ultRs) ultRs = &kUltFallback;
	auto tryOutburst = [&](int32_t minPlayers) -> bool {
		if (singleTargetOnly || level < MONK_ULT_MIN_LEVEL) return false;
		auto sp = monkReady(player, MK_OUTBURST, MONK_ULT_MIN_LEVEL);
		if (!sp) return false;
		bool unsafe = false;
		int32_t players = 0;
		const int32_t hits = monkChainCount(bot, 2, 6, targetId, pvp, unsafe, &players);
		if (unsafe || hits <= 0) return false;
		const int32_t monsters = hits - players;
		if (pvp) {
			if (players < minPlayers) return false;
		} else if (!ultAoeAllowed(bot, *ultRs, monsters, false, "monk", true)) {
			s_monkStats.ultSkips++;
			return false;
		}
		if (!attackMana(sp)) return false;
		const uint8_t hBefore = player->getHarmony();
		if (!castAttack(MK_OUTBURST, "spend", hits, nullptr, false)) return false;
		s_monkStats.ultCasts++;
		g_logger().info("[ULT_AOE] cast bot='{}' spell='{}' monsters={} need={} pvp={} players={} state={} h={}",
			bot.name, MK_OUTBURST, monsters, livenessCfg_.ultAoeMinMonsters, pvp ? 1 : 0, players,
			botStateName(bot.state), hBefore);
		return true;
	};
	// A melee single-target attack, when adjacent.
	auto tryMelee = [&](const char* words, int32_t minLevel, int32_t maxLevelExclusive, const char* kind, bool execute) -> bool {
		if (!adjacent) return false;
		if (maxLevelExclusive > 0 && level >= maxLevelExclusive) return false;
		auto sp = monkReady(player, words, minLevel);
		if (!sp || !attackMana(sp)) return false;
		return castAttack(words, kind, 1, nullptr, execute);
	};
	auto tryWave = [&](const char* words, int32_t minLevel, int32_t minHits, const char* kind) -> bool {
		if (singleTargetOnly) return false;
		auto sp = monkReady(player, words, minLevel);
		if (!sp) return false;
		Direction dir = DIRECTION_NORTH;
		const int32_t n = waveCount(words, dir);
		if (n < minHits || !attackMana(sp)) return false;
		return castAttack(words, kind, n, &dir, false);
	};
	auto tryRepulse = [&]() -> bool {
		if (dist < 2 || dist > 7 || tpos.z != me.z) return false;
		auto sp = monkReady(player, MK_MYSTIC_REPULSE, 30);
		if (!sp || !g_game().map.isSightClear(me, tpos, true) || !attackMana(sp)) return false;
		return castAttack(MK_MYSTIC_REPULSE, "build", 1, nullptr, false);
	};
	auto build = [&](bool areas, uint8_t h) -> bool {
		if (areas && !singleTargetOnly) {
			{
				auto sp = monkReady(player, MK_GREATER_FLURRY, 90);
				Direction dir = DIRECTION_NORTH;
				const int32_t n = sp ? waveCount(MK_GREATER_FLURRY, dir) : 0;
				if (sp && (n >= 3 || (n >= 2 && h <= 3)) && attackMana(sp)
				    && castAttack(MK_GREATER_FLURRY, "build", n, &dir, false)) {
					return true;
				}
			}
			if (auto sp = monkReady(player, MK_CHAINED_PENANCE, 70)) {
				bool unsafe = false;
				const int32_t n = monkChainCount(bot, 3, 3, targetId, pvp, unsafe);
				if (!unsafe && n >= 2 && attackMana(sp) && castAttack(MK_CHAINED_PENANCE, "build", n, nullptr, false)) {
					return true;
				}
			}
			if (tryWave(MK_FLURRY, 35, 2, "build")) return true;
		}
		if (tryMelee(MK_UPPERCUT, 110, 0, "build", false)) return true;
		if (tryMelee(MK_DOUBLE_JAB, 14, 0, "build", false)) return true;
		if (tryRepulse()) return true;
		return tryMelee(MK_SWIFT_JAB, 0, 14, "build", false);
	};
	auto spendSingle = [&](bool execute) -> bool {
		if (tryMelee(MK_KNOCKOUT, 125, 0, "spend", execute)) return true;
		if (tryMelee(MK_GREATER_TIGER, 18, 0, "spend", execute)) return true;
		return tryMelee(MK_TIGER_CLASH, 0, 18, "spend", execute);
	};

	uint8_t h = player->getHarmony();

	// ---------------- PvP (gang, vigilante, random PK, self-defence) ----------------
	if (pvp) {
		const bool killWindow = targetIsPlayer && tHp <= MONK_PVP_KILL_WINDOW_PCT;
		// Focus Serenity at the kill window: it clears cooldowns and fills harmony, so this same slot
		// becomes a 5-harmony Devastating Knockout. Focus Harmony when serenity is not available.
		if (killWindow && h < 5 && adjacent) {
			if ((level >= 150 && castFocus(MK_FOCUS_SERENITY)) || (h <= 1 && level >= 275 && castFocus(MK_FOCUS_HARMONY))) {
				h = player->getHarmony();
			}
		}
		const bool spend = h >= 5 || (h >= 3 && killWindow);
		if (spend) {
			if (tryOutburst(2)) return true; // only with 2+ hostile players inside the chain
			if (spendSingle(false)) return true;
		}
		if (build(false, h)) return true; // single-target builders only (the gang rule)
		s_monkNextEvalMs[bot.guid] = now + MONK_IDLE_REEVAL_MS;
		return false;
	}

	// ---------------- PvE ----------------
	const bool execute = h >= 3 && h < 5 && tHp <= MONK_PVE_EXECUTE_PCT;
	// Focus Harmony on a big pull: h <= 1 and the chain already reaches the ultimate's gate, so the
	// next cast is a 5-harmony Spiritual Outburst plus its own recast.
	if (h <= 1 && !singleTargetOnly && level >= 275 && monkReady(player, MK_OUTBURST, MONK_ULT_MIN_LEVEL)
	    && monkReady(player, MK_FOCUS_HARMONY, 275)) {
		bool unsafe = false;
		const int32_t n = monkChainCount(bot, 2, 6, targetId, false, unsafe);
		if (!unsafe && n >= std::max(1, livenessCfg_.ultAoeMinMonsters) && castFocus(MK_FOCUS_HARMONY)) {
			h = player->getHarmony();
		}
	}
	// Party pull: Serene is lost with 6+ creatures next to a party monk; Focus Serenity forces it
	// back for 7 s and fills harmony on a 4+ monster pull.
	if (h < 5 && adjacentMonsters >= 4 && level >= 150 && (bot.partyHuntId > 0 || player->getParty())
	    && castFocus(MK_FOCUS_SERENITY)) {
		h = player->getHarmony();
	}

	if (h >= 5) {
		if (tryOutburst(0)) return true;
		if (tryWave(MK_SWEEPING, 60, 2, "spend")) return true;
		if (spendSingle(false)) return true;
		// Every spender is cooling down: keep hitting with builders (harmony stays capped at 5).
	} else if (execute) {
		if (spendSingle(true)) return true;
	}
	if (build(true, h)) return true;
	s_monkNextEvalMs[bot.guid] = now + MONK_IDLE_REEVAL_MS;
	return false;
}

// ============================================================================
// Virtues, Serene telemetry, squad heal, stats (heal slot, every awake bot)
// ============================================================================
bool BotEngine::monkEnsureVirtue(BotState& bot, Virtue_t want, const char* why, bool urgent) {
	auto player = bot.getPlayer();
	if (!player || player->getLevel() < 20) return false;
	if (player->getVirtue() == want) return false;
	const char* words = virtueWords(want);
	if (!words) return false;
	const int64_t now = OTSYS_TIME();
	auto& next = s_monkVirtueNextMs[bot.guid];
	if (now < next && !(urgent && want == Virtue_t::Justice)) return false;
	auto sp = monkReady(player, words, 20);
	if (!sp) return false;
	const uint32_t cost = sp->getManaCost(player);
	if (!attackManaOk(player, cost) || player->getMana() < cost) {
		next = now + 5000;
		return false;
	}
	const Virtue_t before = player->getVirtue();
	std::string w = words;
	if (g_spells().playerSaySpell(player, w) != TALKACTION_BREAK) {
		next = now + 5000;
		s_monkStats.fails++;
		return false;
	}
	player->saySpell(TALKTYPE_SAY, words, false);
	next = now + MONK_VIRTUE_PACE_MS;
	s_monkStats.virtues++;
	castLog(bot, fmt::format("MONK: virtue {} -> {} ({})", virtueName(before), virtueName(want), why));
	if (livenessCfg_.monkLogAll || bot.verboseLog || player->getCastViewerCount() > 0 || getDebugCfg(bot.guid)) {
		g_logger().info("[MONK] virtue bot='{}' lvl={} {}->{} why={} state={}", bot.name, player->getLevel(),
			virtueName(before), virtueName(want), why, botStateName(bot.state));
	}
	return true;
}

bool BotEngine::monkEmergencyHeal(BotState& bot, int32_t hpPct, bool pvp) {
	auto player = bot.getPlayer();
	if (!player || pvp) return false; // never in PvP: the harmony belongs to the damage cycle
	const uint8_t h = player->getHarmony();
	if (h < 3) return false;
	auto sp = monkReady(player, MK_MASS_SPIRIT_MEND, 150);
	if (!sp || player->getMana() < sp->getManaCost(player)) return false;
	std::string w = MK_MASS_SPIRIT_MEND;
	if (g_spells().playerSaySpell(player, w) != TALKACTION_BREAK) return false;
	player->saySpell(TALKTYPE_SAY, MK_MASS_SPIRIT_MEND, false);
	s_monkStats.heals++;
	castLog(bot, fmt::format("HEAL: {} h={} (emergency, {}%)", MK_MASS_SPIRIT_MEND, h, hpPct));
	if (livenessCfg_.monkLogAll || bot.verboseLog || player->getCastViewerCount() > 0 || getDebugCfg(bot.guid)) {
		g_logger().info("[MONK] heal bot='{}' spell='{}' h={}->{} hp={}% why=solo_emergency state={}", bot.name,
			MK_MASS_SPIRIT_MEND, h, player->getHarmony(), hpPct, botStateName(bot.state));
	}
	return true;
}

bool BotEngine::monkHealOther(BotState& bot, const std::shared_ptr<Player>& who, const char* why) {
	auto player = bot.getPlayer();
	if (!player || !who || who.get() == player.get() || who->isRemoved() || who->getHealth() <= 0) return false;
	const Position me = player->getPosition();
	const Position wp = who->getPosition();
	if (wp.z != me.z || monkCheb(me, wp) > 7 || !g_game().map.isSightClear(me, wp, true)) return false;
	auto sp = monkReady(player, MK_RESTORE_BALANCE, 18);
	if (!sp || player->getMana() < sp->getManaCost(player)) return false;
	std::string words = fmt::format("{} \"{}\"", MK_RESTORE_BALANCE, who->getName());
	if (g_spells().playerSaySpell(player, words) != TALKACTION_BREAK) return false;
	player->saySpell(TALKTYPE_SAY, fmt::format("{} \"{}\"", MK_RESTORE_BALANCE, who->getName()), false);
	s_monkStats.heals++;
	castLog(bot, fmt::format("HEAL: {} \"{}\" ({}%, {})", MK_RESTORE_BALANCE, who->getName(), monkHpPct(who), why));
	if (livenessCfg_.monkLogAll || bot.verboseLog || player->getCastViewerCount() > 0 || getDebugCfg(bot.guid)) {
		g_logger().info("[MONK] heal bot='{}' spell='{}' target='{}' hp={}% why={} state={}", bot.name,
			MK_RESTORE_BALANCE, who->getName(), monkHpPct(who), why, botStateName(bot.state));
	}
	return true;
}

void BotEngine::maybeMonkUpkeep(BotState& bot) {
	const int64_t now = OTSYS_TIME();
	// Global stats line (any bot's slot may flush it). Silent while no monk cast anything.
	if (now >= s_monkStatsNextMs) {
		const auto& st = s_monkStats;
		if (s_monkStatsNextMs != 0 && (st.builds + st.spends + st.focus + st.virtues + st.heals + st.fails) > 0) {
			g_logger().info("[MONK_STATS] builds={} spends={} spendLow={} executes={} ult={} ultSkip={} focus={} virtues={} heals={} fails={} window={}s",
				st.builds, st.spends, st.spendLow, st.executes, st.ultCasts, st.ultSkips, st.focus, st.virtues, st.heals,
				st.fails, MONK_STATS_EVERY_MS / 1000);
			s_monkStats = MonkStats {};
		}
		s_monkStatsNextMs = now + MONK_STATS_EVERY_MS;
	}

	if (getBaseVocation(bot.vocationId) != BOT_BASE_VOC_MONK) return;
	auto player = bot.getPlayer();
	if (!player || player->isRemoved() || player->getHealth() <= 0) return;
	tickMonkDrill(bot); // test verb only (no-op without a drill)
	const bool watched = livenessCfg_.monkLogAll || bot.verboseLog || player->getCastViewerCount() > 0
		|| getDebugCfg(bot.guid) != nullptr;

	// [BOT_SERENE]: the core fix (Player::onThink bot block) drives the condition; this only reports it.
	{
		const uint8_t v = player->hasCondition(CONDITION_SERENE) ? 2 : 1;
		auto& seen = s_monkSereneSeen[bot.guid];
		if (seen != v) {
			if (watched) {
				int32_t adj = 0;
				for (const auto& c : Spectators().find<Creature>(player->getPosition(), false, 1, 1, 1, 1)) {
					if (c && c.get() != player.get() && !c->getNpc()) ++adj;
				}
				g_logger().info("[BOT_SERENE] {} bot='{}' lvl={} party={} adjacent={} state={}", v == 2 ? "on" : "off",
					bot.name, player->getLevel(), player->getParty() ? 1 : 0, adj, botStateName(bot.state));
			}
			seen = v;
		}
	}

	// Virtue. PvP (fight with a player, gang) -> Justice; a party without a druid -> Sustain; else
	// Harmony. castMonkSpell asks for Justice at once when a fight with a player starts.
	{
		const bool pvp = s_gangByGuid.count(bot.guid) > 0 || bot.state == BotAIState::PK_ATTACK
			|| ultAoePvpContext(bot, player->getAttackedCreature());
		Virtue_t want = Virtue_t::Harmony;
		const char* why = "solo";
		if (pvp) {
			want = Virtue_t::Justice;
			why = "pvp";
		} else if (const auto party = player->getParty()) {
			if (!partyHasHealerNear(player, party, 30)) {
				want = Virtue_t::Sustain;
				why = "party_no_healer";
			} else {
				why = "party";
			}
		}
		monkEnsureVirtue(bot, want, why, pvp);
	}

	// Gang squad heal: a mate at or below 40 % while this monk is at or above 60 %.
	if (const auto gi = s_gangByGuid.find(bot.guid); gi != s_gangByGuid.end() && monkHpPct(player) >= 60) {
		for (const auto& spec : Spectators().find<Player>(player->getPosition(), false, 7, 7, 5, 5)) {
			const auto mate = spec ? spec->getPlayer() : nullptr;
			if (!mate || mate.get() == player.get() || monkHpPct(mate) > 40) continue;
			const auto mi = s_gangByGuid.find(mate->getGUID());
			if (mi == s_gangByGuid.end() || mi->second != gi->second) continue;
			if (monkHealOther(bot, mate, "gang_mate")) break;
		}
	}
}

// ============================================================================
// Party monk (PARTY_ROLE_MONK)
// ============================================================================
void BotEngine::doPartyHuntMonk(BotState& bot, BotState* leaderBot) {
	auto player = bot.getPlayer();
	if (!player || !leaderBot) return;
	auto leaderPlayer = leaderBot->getPlayer();
	if (!leaderPlayer) return;
	const auto party = player->getParty();
	const Position me = player->getPosition();

	// === HEALING (healing group: never blocks the attack below) ===
	if (party) {
		const bool healerNear = partyHasHealerNear(player, party, 7);
		std::vector<std::shared_ptr<Player>> group;
		group.push_back(party->getLeader());
		for (const auto& m : party->getMembers()) group.push_back(m);
		int32_t lowNear = monkHpPct(player) <= 60 ? 1 : 0; // mass spirit mend heals the caster too
		std::shared_ptr<Player> healTarget;
		int32_t healPct = 101;
		for (const auto& m : group) {
			if (!m || m.get() == player.get() || m->isRemoved() || m->getHealth() <= 0) continue;
			const Position mp = m->getPosition();
			if (mp.z != me.z || monkCheb(me, mp) > 7) continue;
			const int32_t pct = monkHpPct(m);
			if (pct <= 60 && monkCheb(me, mp) <= 3) ++lowNear;
			const int32_t line = m.get() == leaderPlayer.get() ? 85 : 75;
			if (pct <= line && pct < healPct) {
				healPct = pct;
				healTarget = m;
			}
		}
		bool healed = false;
		if (lowNear >= 2 && !ultAoePvpContext(bot, player->getAttackedCreature())) {
			if (auto sp = monkReady(player, MK_MASS_SPIRIT_MEND, 150); sp && player->getMana() >= sp->getManaCost(player)) {
				const uint8_t hBefore = player->getHarmony();
				std::string w = MK_MASS_SPIRIT_MEND;
				if (g_spells().playerSaySpell(player, w) == TALKACTION_BREAK) {
					player->saySpell(TALKTYPE_SAY, MK_MASS_SPIRIT_MEND, false);
					s_monkStats.heals++;
					healed = true;
					castLog(bot, fmt::format("PARTY_HUNT: {} ({} low, h={})", MK_MASS_SPIRIT_MEND, lowNear, hBefore));
				}
			}
		}
		// Restore Balance only when no druid is there to do it (the ED owns party healing).
		if (!healed && healTarget && !healerNear) {
			monkHealOther(bot, healTarget, healTarget.get() == leaderPlayer.get() ? "party_leader" : "party_member");
		}

		// === SUPPORT (support group, one per tick) ===
		bool supported = false;
		if (leaderPlayer.get() != player.get() && player->getLevel() >= 150) {
			const Position lp = leaderPlayer->getPosition();
			if (lp.z == me.z && monkCheb(me, lp) <= 7 && g_game().map.isSightClear(me, lp, true)) {
				if (auto sp = monkReady(player, MK_MENTOR_OTHER, 150);
				    sp && attackManaOk(player, sp->getManaCost(player)) && player->getMana() >= sp->getManaCost(player)) {
					std::string w = fmt::format("{} \"{}\"", MK_MENTOR_OTHER, leaderPlayer->getName());
					if (g_spells().playerSaySpell(player, w) == TALKACTION_BREAK) {
						player->saySpell(TALKTYPE_SAY, w, false);
						supported = true;
						castLog(bot, fmt::format("PARTY_HUNT: {} \"{}\"", MK_MENTOR_OTHER, leaderPlayer->getName()));
					}
				}
			}
		}
		if (!supported) {
			int32_t inRange = 0;
			for (const auto& m : group) {
				if (!m || m.get() == player.get() || m->isRemoved()) continue;
				const Position mp = m->getPosition();
				if (mp.z == me.z && monkCheb(me, mp) <= 7) ++inRange;
			}
			if (inRange >= 2) {
				if (auto sp = monkReady(player, MK_ENLIGHTEN_PARTY, 32);
				    sp && attackManaOk(player, sp->getManaCost(player)) && player->getMana() >= sp->getManaCost(player)) {
					std::string w = MK_ENLIGHTEN_PARTY;
					if (g_spells().playerSaySpell(player, w) == TALKACTION_BREAK) {
						player->saySpell(TALKTYPE_SAY, MK_ENLIGHTEN_PARTY, false);
						castLog(bot, fmt::format("PARTY_HUNT: {} ({} members in range)", MK_ENLIGHTEN_PARTY, inRange));
						if (livenessCfg_.monkLogAll || bot.verboseLog || player->getCastViewerCount() > 0) {
							g_logger().info("[MONK] support bot='{}' spell='{}' members={} party=#{}", bot.name,
								MK_ENLIGHTEN_PARTY, inRange, bot.partyHuntId);
						}
					}
				}
			}
		}
	}

	// === ATTACK: the leader's target, in melee ===
	auto leaderTarget = leaderPlayer->getAttackedCreature();
	if (!leaderTarget || leaderTarget->getHealth() <= 0 || leaderTarget->isRemoved()) return;
	const Position tpos = leaderTarget->getPosition();
	if (tpos.z != me.z) return;
	const Position lpos = leaderPlayer->getPosition();
	// Same engagement gate as the other supports: wait until the leader has closed on a MONSTER
	// (a kiting player is never reached by a melee leader, so the gate skips player targets).
	if (monkCheb(lpos, tpos) > 2 && !leaderTarget->getPlayer()) return;
	// Never chase out of the leader's leash: followPartyHuntLeader brings the monk back instead.
	if (monkCheb(lpos, tpos) > PARTY_LEASH_DIST) return;

	if (player->getAttackedCreature() != leaderTarget) {
		player->setAttackedCreature(leaderTarget);
	}
	castSpell(bot, leaderTarget);
	if (monkCheb(me, tpos) > 1) {
		chaseTarget(bot, leaderTarget); // melee approach (keep distance 0)
	}
	// Engaged in melee: the role owns positioning this tick, so followPartyHuntLeader must not walk
	// the monk back to the leader's side (its keepDist-0 path converges on the LEADER, not the
	// target). The teleport / stair safety nets still run (see followPartyHuntLeader).
	s_roleWalkedThisTick.insert(bot.guid);
}

// ============================================================================
// Test hooks: `/cavebot <bot> gear` and `/cavebot <bot> monk`
// ============================================================================
std::string BotEngine::describeBotGear(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) {
		return fmt::format("GEAR bot='{}' hibernated (wake it first)", bot.name);
	}
	struct SlotName { Slots_t slot; const char* name; };
	static constexpr SlotName kSlots[] = {
		{ CONST_SLOT_HEAD, "head" }, { CONST_SLOT_NECKLACE, "neck" }, { CONST_SLOT_ARMOR, "armor" },
		{ CONST_SLOT_LEFT, "weapon" }, { CONST_SLOT_RIGHT, "shield" }, { CONST_SLOT_LEGS, "legs" },
		{ CONST_SLOT_FEET, "feet" }, { CONST_SLOT_RING, "ring" },
	};
	std::string out = fmt::format("GEAR bot='{}' lvl={} voc={} base={}", bot.name, player->getLevel(),
		player->getVocationId(), getBaseVocation(bot.vocationId));
	std::string imbues;
	for (const auto& s : kSlots) {
		const auto item = player->getInventoryItem(s.slot);
		if (!item) {
			out += fmt::format(" {}=0", s.name);
			continue;
		}
		out += fmt::format(" {}={}:t{}", s.name, item->getID(), static_cast<int>(item->getTier()));
		for (uint8_t i = 0; i < item->getImbuementSlot(); ++i) {
			ImbuementInfo info;
			if (item->getImbuementInfo(i, &info) && info.imbuement) {
				imbues += fmt::format("{}{}:{}", imbues.empty() ? "" : ",", s.name, info.imbuement->getName());
			}
		}
	}
	std::string potions;
	static constexpr uint16_t kPotionIds[] = { 266, 268, 236, 237, 238, 239, 7642, 7643, 23373, 23374, 23375 };
	for (const uint16_t id : kPotionIds) {
		if (g_game().findItemOfType(player, id, true, -1)) {
			potions += fmt::format("{}{}", potions.empty() ? "" : ",", id);
		}
	}
	out += fmt::format(" potions={} imbues={}", potions.empty() ? "none" : potions, imbues.empty() ? "none" : imbues);
	return out;
}

std::string BotEngine::describeMonk(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) {
		return fmt::format("MONK bot='{}' hibernated (wake it first)", bot.name);
	}
	if (getBaseVocation(bot.vocationId) != BOT_BASE_VOC_MONK) {
		return fmt::format("MONK bot='{}' is not a monk (voc={})", bot.name, player->getVocationId());
	}
	return fmt::format("MONK bot='{}' lvl={} voc={} h={} serene={} virtue={} party={} role={} state={}",
		bot.name, player->getLevel(), player->getVocationId(), player->getHarmony(),
		player->hasCondition(CONDITION_SERENE) ? 1 : 0, virtueName(player->getVirtue()),
		player->getParty() ? 1 : 0, static_cast<int>(bot.partyRole), botStateName(bot.state));
}

// ============================================================================
// `/cavebot <monk> monkdrill <n> [secs]` | `monkdrill stop` — deterministic rotation test.
// Spawns n Training Machines (1,000,000 HP, speed 0, 2-7 melee) next to the bot, holds the AI
// (aiPaused: no walking; the heal slot — potions, heals, this upkeep — still runs) and attacks the
// nearest one through castSpell every heal slot, so the 2 s pacing and every gate are the real
// ones. n=1: build to 5 then a single spend, [ULT_AOE] skip monsters=1; n=2: area spells and
// skip monsters=2; n>=3: the ultimate. Ends after secs (default 60, max 600) and removes them.
// ============================================================================
namespace {
struct MonkDrill {
	std::vector<uint32_t> monsterIds;
	int64_t untilMs = 0;
	bool wasPaused = false;
};
std::unordered_map<uint32_t, MonkDrill> s_monkDrills;
} // namespace

static void monkDrillDespawn(MonkDrill& d) {
	for (const uint32_t id : d.monsterIds) {
		if (auto c = g_game().getCreatureByID(id); c && !c->isRemoved()) {
			g_game().removeCreature(c);
		}
	}
	d.monsterIds.clear();
}

std::string BotEngine::monkDrillCommand(BotState& bot, const std::string& args) {
	auto player = bot.getPlayer();
	if (!player || player->isRemoved()) {
		return fmt::format("Bot '{}' is not awake.", bot.name);
	}
	if (args == "stop") {
		auto it = s_monkDrills.find(bot.guid);
		if (it == s_monkDrills.end()) return fmt::format("Bot '{}' has no drill.", bot.name);
		monkDrillDespawn(it->second);
		bot.aiPaused = it->second.wasPaused;
		player->setAttackedCreature(nullptr);
		s_monkDrills.erase(it);
		return fmt::format("Bot '{}' drill stopped.", bot.name);
	}
	if (getBaseVocation(bot.vocationId) != BOT_BASE_VOC_MONK) {
		return fmt::format("Bot '{}' is not a monk (voc={}).", bot.name, bot.vocationId);
	}
	int32_t n = 1, secs = 60;
	if (sscanf(args.c_str(), "%d %d", &n, &secs) < 1) {
		return "Usage: monkdrill <1-6> [seconds] | monkdrill stop";
	}
	n = std::clamp(n, 1, 6);
	secs = std::clamp(secs, 10, 600);
	auto& d = s_monkDrills[bot.guid];
	monkDrillDespawn(d);
	d.wasPaused = d.untilMs == 0 ? bot.aiPaused : d.wasPaused;
	d.untilMs = OTSYS_TIME() + secs * 1000LL;
	for (int32_t i = 0; i < n; ++i) {
		auto m = Monster::createMonster("Training Machine");
		if (!m || !g_game().placeCreature(m, player->getPosition(), true, false)) break;
		d.monsterIds.push_back(m->getID());
	}
	player->stopWalk();
	bot.aiPaused = true;
	return fmt::format("Bot '{}' drill: {} training machine(s) for {}s (lvl={} h={}).", bot.name,
		d.monsterIds.size(), secs, player->getLevel(), player->getHarmony());
}

void BotEngine::tickMonkDrill(BotState& bot) {
	auto it = s_monkDrills.find(bot.guid);
	if (it == s_monkDrills.end()) return;
	auto player = bot.getPlayer();
	MonkDrill& d = it->second;
	if (!player || OTSYS_TIME() >= d.untilMs) {
		monkDrillDespawn(d);
		bot.aiPaused = d.wasPaused;
		if (player) {
			player->setAttackedCreature(nullptr);
			g_logger().info("[MONK] drill_end bot='{}' h={}", bot.name, player->getHarmony());
		}
		s_monkDrills.erase(it);
		return;
	}
	std::shared_ptr<Creature> best;
	int32_t bestD = 99;
	for (const uint32_t id : d.monsterIds) {
		auto c = g_game().getCreatureByID(id);
		if (!c || c->isRemoved() || c->getHealth() <= 0) continue;
		const int32_t dd = monkCheb(player->getPosition(), c->getPosition());
		if (dd < bestD) {
			bestD = dd;
			best = c;
		}
	}
	if (best) {
		bot.currentPos = player->getPosition();
		castSpell(bot, best);
		if (bestD > 1 && player->listWalkDir.empty()) {
			chaseTarget(bot, best); // melee: step next to it (aiPaused stops only the state machine)
		}
	}
}
