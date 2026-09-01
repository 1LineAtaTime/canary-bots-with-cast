/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_supply.cpp — BOT_SUPPLY_REALISM: potions, food, rune crafting, fishing.
//
// Four behaviours that make a bot read as a player managing supplies rather than
// a creature that never eats. All of them drive the REAL server code paths a
// manual click would drive — g_actions().useItem/useItemEx into the Lua action,
// g_spells().playerSaySpell into the spell registry — so what an observer sees
// (the "Aaaah...", the splash, the red rune effect, the spell words, the sound)
// is produced by the same code that produces it for a human.
//
// What is bot-specific is only the SUPPLY side: the consumed input is infinite
// for bots. That is done with narrow `isBotPlayer()` guards in the four scripts
// that own each consumable, mirroring the precedent already in spells.cpp:1481
// for rune charges. Bots do NOT get free output — the flask, the conjured rune
// and the fish are all created for real, and land in the backpack.
//
// Scheduling contract (differs per behaviour, deliberately):
//   * potions/food — ANY awake state. Hooked into the outer per-bot loop beside
//     doHealing, NOT processBot, which early-returns on death-pause, on every
//     floor change, and on AdvStone trips.
//   * rune crafting — same hook, plus a "not hunting" test inside. Covering
//     every other awake state structurally beats enumerating them; an earlier
//     per-state design had already silently missed TRAVELING.
//   * fishing — a town activity chosen at reroll like any other POI, then walked
//     by the scoped route planner (cross-floor included) and driven by a 3-phase
//     run keyed on guid.
//
// No concurrency caps anywhere: unlike hunting (activeHunts_ reserves a script to
// one bot) every bot may do any of these at any time, and nothing reserves a tile.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#include "creatures/combat/condition.hpp" // Condition::getTicks — the food fullness pre-check
#include "creatures/combat/spells.hpp"
#include "items/items.hpp"
#include "lua/creature/actions.hpp"
#include "utils/tools.hpp" // getDirectionTo

#include <algorithm>
#include <filesystem>
#include <limits>
#include <fstream>
#include <iterator>
#include <regex>

namespace {

// How long a bot must have been standing still, idle and errand-free, before it may conjure.
// "More than 10s idling" — long enough to exclude every SHORT stop a bot makes: the 3-8s
// post-dwell thinking pause (bot_tick.cpp), the 0.5-20s observed mid-walk pause, and the
// sub-second gap between path chunks. The 60-900s dwell roll and the 20-60s depot-locker wait
// both clear it comfortably. Deliberately a constant and not a config key: it is the definition
// of the behaviour, not a tuning knob.
constexpr int64_t RUNE_IDLE_MIN_MS = 10000;

// ---- Consumables the bot carries (seeded by equipBot) ----
constexpr uint16_t ITEM_FISHING_ROD = 3483;
constexpr uint16_t ITEM_WORM = 3492;
constexpr uint16_t ITEM_BLANK_RUNE = 3147;

// ---- Ice fishing (`fish:` waypoint marker) ----
// The Svargrond ice-field cycle, all three ids GROUND items:
//   7200 fragile ice   --pick 3456-->  7236 ice hole (fish visible)
//   7236 ice hole      --rod  3483-->  7237 emptied hole  (on a successful catch)
//   7237 emptied hole  --900s decay->  7200 fragile ice
// Deliberately NOT added to isFishableWaterId (bot_zgraph.cpp): that exclusion is what keeps idle
// town bots from wandering onto the ice fields. Ice is reached only by an explicit hunt waypoint.
constexpr uint16_t ITEM_PICK = 3456;
constexpr uint16_t ICE_FRAGILE = 7200;
constexpr uint16_t ICE_HOLE_FISH = 7236;
constexpr uint16_t ICE_HOLE_EMPTY = 7237;

// One visit to one hole. Long enough that the bot usually works the hole until it actually catches
// (the hole then transforms to 7237 and the session ends on its own), short enough that 25 stands
// still fit inside a 20-40 min hunt (botHuntTimeMinSec/MaxSec) with walking time to spare.
constexpr int32_t ICE_SESSION_MIN_MS = 20000;
constexpr int32_t ICE_SESSION_MAX_MS = 45000;

// A manual `/cavebot <bot> fishice` runs until the hole actually closes. This is only the safety
// cap so a session can never outlive an admin's interest in it — at 1-2s per cast, 5 minutes is
// ~150-300 casts, far past the point where a ~10% catch roll has effectively certainly landed.
constexpr int32_t ICE_MANUAL_CAP_MS = 300000;

uint16_t groundIdAt(const Position& pos) {
	const auto& tile = g_game().map.getTile(pos);
	if (!tile) return 0;
	const auto& ground = tile->getGround();
	return ground ? ground->getID() : 0;
}

// Portal-anchored cross-floor fishing search (selectFishingSpot P3): how many nearby portals to
// probe with a full findNearbyFishingSpot call, nearest-to-the-bot first. Bounds the cost in a
// portal-dense area (a dungeon with several close staircases) the way MAX_APPROACH_TILES bounds
// the NPC-approach search.
constexpr int32_t FISH_PORTAL_CANDIDATES_MAX = 4;

// Potion table, mirroring data/scripts/actions/items/potions.lua. Restricted to entries that
// actually carry health/mana — which is what guarantees the flask the script creates is one of
// 283/284/285. The buff potions (7439/7440/7443/49271), the mana-shield potion (35563) and the
// transform flask (6558) have no flask field and are deliberately absent.
struct PotionDef {
	uint16_t id;
	uint32_t level;
	bool heals;
	bool restoresMana;
	uint32_t vocMask; // bit per base vocation id; 0 = any
};
constexpr uint32_t VOC_SORC = 1u << 1, VOC_DRUID = 1u << 2, VOC_PAL = 1u << 3, VOC_KNIGHT = 1u << 4;
constexpr uint32_t VOC_ANY = 0;

// Ordered weakest -> strongest so the picker can walk it backwards.
constexpr PotionDef kPotions[] = {
	{ 266,   0,  true,  false, VOC_ANY },                                  // health potion
	{ 268,   0,  false, true,  VOC_ANY },                                  // mana potion
	{ 236,  50,  true,  false, VOC_PAL | VOC_KNIGHT },                     // strong health
	{ 237,  50,  false, true,  VOC_ANY },                                  // strong mana
	{ 238,  80,  false, true,  VOC_SORC | VOC_DRUID | VOC_PAL },           // great mana
	{ 239,  80,  true,  false, VOC_KNIGHT },                               // great health
	{ 7642, 80,  true,  true,  VOC_PAL },                                  // great spirit
	{ 7643, 130, true,  false, VOC_KNIGHT },                               // ultimate health
	{ 23373, 130, false, true, VOC_SORC | VOC_DRUID },                     // ultimate mana
	{ 23374, 130, true,  true, VOC_PAL },                                  // ultimate spirit
	{ 23375, 200, true,  false, VOC_KNIGHT },                              // supreme health
};


uint32_t vocBit(uint8_t baseVoc) {
	return baseVoc >= 1 && baseVoc <= 4 ? (1u << baseVoc) : 0u;
}

} // namespace

// ============================================================================
// Conjure spell discovery
// ============================================================================
//
// Mirrors parseLuaRuneFiles' approach (regex over the shipped Lua) because the server registry
// knows a spell's words/level/mana/vocations but NOT that it conjures, nor what it produces.
//
// Filter, in order:
//   * reagentId must be 0 or 3147 (blank rune). This is what excludes enchant_spear /
//     enchant_staff, which consume a real spear/staff — verified conjureItem(3277, ...).
//   * the conjured item must be a rune or ammunition, which excludes conjure_wand_of_darkness.
//   * the spell must exist in the live registry, which supplies level + vocation gating.
void BotEngine::buildConjureTables() {
	conjureSpells_.clear();
	namespace fs = std::filesystem;
	const std::string dir = "data/scripts/spells/conjuring";
	if (!fs::exists(dir)) {
		g_logger().warn("[BotSupply] conjuring dir '{}' not found — rune crafting disabled", dir);
		return;
	}

	// Custom R"RX(...)RX" delimiters: the default R"(...)" cannot hold these patterns, because
	// the sequence )" occurs INSIDE them (…([^"]+)") and would terminate the literal early.
	const std::regex conjureRe(R"RX(conjureItem\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+))RX");
	const std::regex wordsRe(R"RX(spell:words\s*\(\s*"([^"]+)")RX");

	for (const auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
			continue;
		}
		std::ifstream file(entry.path());
		if (!file.is_open()) {
			continue;
		}
		const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		std::smatch m;
		if (!std::regex_search(content, m, conjureRe)) {
			continue;
		}
		const uint16_t reagentId = static_cast<uint16_t>(std::stoi(m[1].str()));
		const uint16_t conjureId = static_cast<uint16_t>(std::stoi(m[2].str()));
		// Anything other than "nothing" or a blank rune means the spell eats a real item the bot
		// would have to keep buying — enchant_spear/enchant_staff.
		if (reagentId != 0 && reagentId != ITEM_BLANK_RUNE) {
			continue;
		}
		const ItemType& it = Item::items[conjureId];
		if (!it.isRune() && it.weaponType != WEAPON_AMMO) {
			continue; // wand of darkness et al
		}
		if (!std::regex_search(content, m, wordsRe)) {
			continue;
		}
		const std::string words = m[1].str();

		const auto& spell = g_spells().getInstantSpell(words);
		if (!spell) {
			continue;
		}
		ConjureSpell cs;
		cs.words = words;
		cs.conjureId = conjureId;
		cs.reagentId = reagentId;
		cs.level = spell->getLevel();
		// Promoted vocation ids are base+4; the registry keys on the promoted id (see
		// pvpResolveHasteSpells, which reads the same map).
		const auto& vocMap = spell->getVocMap();
		for (uint8_t baseVoc = 1; baseVoc <= 4; ++baseVoc) {
			if (vocMap.find(static_cast<uint16_t>(baseVoc + 4)) != vocMap.end()) {
				cs.vocMask |= vocBit(baseVoc);
			}
		}
		if (cs.vocMask == 0) {
			continue;
		}
		conjureSpells_.push_back(std::move(cs));
	}

	// Stable order — signatureConjure indexes into this by personality seed, and a directory
	// iteration order change must not silently repoint every bot at a different spell.
	std::sort(conjureSpells_.begin(), conjureSpells_.end(),
		[](const ConjureSpell& a, const ConjureSpell& b) { return a.words < b.words; });

	g_logger().info("[BotSupply] {} conjure spells discovered", conjureSpells_.size());
}

// The bot's one spell for the session. Deterministic from personalitySeed so it is stable while
// the server runs and re-rolls on restart, exactly like the other personality-derived traits.
const BotEngine::ConjureSpell* BotEngine::signatureConjure(const BotState& bot) const {
	if (conjureSpells_.empty()) {
		return nullptr;
	}
	const uint32_t bit = vocBit(getBaseVocation(bot.vocationId));
	if (bit == 0) {
		return nullptr; // knights conjure nothing — correct
	}
	const int32_t level = bot.getPlayer() ? bot.getPlayer()->getLevel()
	                                      : static_cast<int32_t>(bot.cachedLevel);

	std::vector<const ConjureSpell*> eligible;
	for (const auto& cs : conjureSpells_) {
		if ((cs.vocMask & bit) == 0) {
			continue;
		}
		if (level < static_cast<int32_t>(cs.level)) {
			continue;
		}
		eligible.push_back(&cs);
	}
	if (eligible.empty()) {
		return nullptr;
	}
	return eligible[bot.personalitySeed % eligible.size()];
}

// ============================================================================
// Potions + food
// ============================================================================

bool BotEngine::tryDrinkPotion(BotState& bot, bool force, bool preferMana, std::string* outMsg) {
	auto player = bot.getPlayer();
	if (!player) {
		if (outMsg) *outMsg = "no player object (hibernated?)";
		return false;
	}
	const auto& cfg = livenessCfg_;
	const int64_t now = OTSYS_TIME();

	if (!force) {
		auto it = s_nextPotionMs.find(bot.guid);
		if (it != s_nextPotionMs.end() && now < it->second) {
			return false;
		}
		// Re-arm even on a miss so the roll is paced rather than retried every 500ms.
		s_nextPotionMs[bot.guid] = now + std::max(1000, cfg.potionMinIntervalMs);
		if (uniform_random(1, 100) > cfg.potionChancePct) {
			return false;
		}
	}

	// Only drink when it would actually do something — a bot sipping at full HP and full mana is
	// the single most recognisable bot tell. A forced drink skips this so an admin always sees
	// the effect, even on a topped-up bot.
	bool wantHp = player->getHealth() < player->getMaxHealth();
	bool wantMana = player->getMana() < player->getMaxMana();
	if (!force && !wantHp && !wantMana) {
		return false;
	}
	if (force) {
		// Honour the caller's preference rather than whatever happens to be missing.
		wantMana = preferMana || wantMana;
		wantHp = !preferMana || wantHp;
	}

	const uint32_t bit = vocBit(getBaseVocation(bot.vocationId));
	const uint32_t level = player->getLevel();
	auto pickPotion = [&](bool needHeal, bool needMana) -> const PotionDef* {
		for (int i = static_cast<int>(std::size(kPotions)) - 1; i >= 0; --i) {
			const PotionDef& p = kPotions[i];
			if (level < p.level) continue;
			if (p.vocMask != VOC_ANY && (p.vocMask & bit) == 0) continue;
			if (!((p.heals && needHeal) || (p.restoresMana && needMana))) continue;
			return &p;
		}
		return nullptr;
	};
	// Forced + mana-preferred tries mana first, then falls back so a knight still gets something.
	const PotionDef* pick = force && preferMana ? pickPotion(false, true) : pickPotion(wantHp, wantMana);
	if (!pick && force) {
		pick = pickPotion(true, true);
	}
	if (!pick) {
		if (outMsg) *outMsg = "no potion this bot's level/vocation can drink";
		return false;
	}
	auto potion = g_game().findItemOfType(player, pick->id, true, -1);
	if (!potion) {
		if (outMsg) *outMsg = fmt::format("potion {} not in inventory", pick->id);
		return false;
	}
	// fromPos MUST be the 0xFFFF container sentinel, not the item's map position. potions.lua
	// branches on `fromPosition.x == CONTAINER_POSITION` to decide whether the empty flask goes
	// into the backpack or is created on the ground; an item held by a player resolves through
	// getTopParent() to the player's TILE, which would drop every flask on the floor.
	// internalGetPosition is the engine's own helper for exactly this and sets x = 0xFFFF.
	Position fromPos;
	uint8_t fromStack = 0;
	g_game().internalGetPosition(potion, fromPos, fromStack);
	// Target self by passing the player as the trailing creature argument — Action::getTarget
	// returns it directly, satisfying potions.lua's `target:isPlayer()` with no stackpos work.
	if (!g_actions().useItemEx(player, fromPos, player->getPosition(), 0, potion, false, player)) {
		if (outMsg) *outMsg = fmt::format("useItemEx refused potion {} (exhausted?)", pick->id);
		return false;
	}
	s_nextPotionMs[bot.guid] = now + std::max(1000, cfg.potionMinIntervalMs);
	const std::string msg = fmt::format("drank potion {} (hp={}/{} mana={}/{})", pick->id,
		player->getHealth(), player->getMaxHealth(), player->getMana(), player->getMaxMana());
	castLog(bot, "SUPPLY: " + msg);
	if (outMsg) *outMsg = msg;
	return true;
}

bool BotEngine::tryEatFood(BotState& bot, bool force, std::string* outMsg) {
	auto player = bot.getPlayer();
	if (!player) {
		if (outMsg) *outMsg = "no player object (hibernated?)";
		return false;
	}
	const auto& cfg = livenessCfg_;
	const int64_t now = OTSYS_TIME();

	if (!force) {
		auto it = s_nextFoodMs.find(bot.guid);
		if (it != s_nextFoodMs.end() && now < it->second) {
			return false;
		}
		s_nextFoodMs[bot.guid] = now + std::max(1000, cfg.foodMinIntervalMs);
		if (uniform_random(1, 100) > cfg.foodChancePct) {
			return false;
		}
		// Pre-check fullness the way foods.lua does, so the automatic path never triggers its
		// "You are full." branch — that would be a visible non-action. A forced eat skips the
		// check and lets the script answer, which is what an admin testing it wants to see.
		if (const auto& cond = player->getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)) {
			if ((cond->getTicks() / 1000 + 60 * 12) >= 1200) {
				return false;
			}
		}
	}

	const uint16_t foodId = kBotFoods[uniform_random(0, static_cast<int32_t>(std::size(kBotFoods)) - 1)];
	auto food = g_game().findItemOfType(player, foodId, true, -1);
	if (!food) {
		if (outMsg) *outMsg = fmt::format("food {} not in inventory", foodId);
		return false;
	}
	if (!g_actions().useItem(player, player->getPosition(), 0, food, false)) {
		if (outMsg) *outMsg = fmt::format("useItem refused food {}", foodId);
		return false;
	}
	s_nextFoodMs[bot.guid] = now + std::max(1000, cfg.foodMinIntervalMs);
	const std::string msg = fmt::format("ate food {}", foodId);
	castLog(bot, "SUPPLY: " + msg);
	if (outMsg) *outMsg = msg;
	return true;
}

void BotEngine::maybeUseSupplies(BotState& bot) {
	// The outer loop already skipped inactive and hibernated bots before calling us, but it does
	// not null-check the Player (the adjacent mana-restore block does its own check too).
	if (!bot.getPlayer()) {
		return;
	}
	tryDrinkPotion(bot, /*force=*/false, /*preferMana=*/false);
	tryEatFood(bot, /*force=*/false);
}

// ============================================================================
// Rune crafting
// ============================================================================

bool BotEngine::tryCraftRune(BotState& bot, bool force, std::string* outMsg) {
	auto player = bot.getPlayer();
	if (!player) {
		if (outMsg) *outMsg = "no player object (hibernated?)";
		return false;
	}
	// "Any awake state EXCEPT hunting." PARTY is a party hunt, so it counts as hunting. This
	// holds even when forced — it is the feature's defining scope, not a pacing gate.
	if (bot.state == BotAIState::HUNTING || bot.huntScriptId > 0
	    || bot.state == BotAIState::PARTY || bot.partyHuntId > 0
	    // BOT_PARTY_INVITE_RENDEZVOUS: a human-led member is state-IDLE while it walks in,
	    // so the PARTY/partyHuntId pair above misses it entirely. Being SELECTED here is the
	    // problem, not merely being walked: the errand claims resources and fights the walk.
	    || s_rvMember.count(bot.guid) > 0 || s_partyLeaderId.count(bot.guid) > 0) {
		if (outMsg) *outMsg = "bot is hunting — rune crafting is excluded while hunting";
		return false;
	}
	if (!force) {
		// Anything that needs the bot's full attention. Same shape as maybeFidgetDrop's gates.
		if (bot.attackerId != 0 || bot.huntTargetId != 0) {
			return false;
		}
		if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::PK_ATTACK
		    || bot.state == BotAIState::FLEEING) {
			return false;
		}
		if (bot.fcState != FloorChangeState::NONE) {
			return false;
		}
		if (!player->listWalkDir.empty()) {
			// Redundant under the idle gate below (an idle bot has no walk queue), kept as the
			// cheap early-out. NB the original reason given here — "conjuring mid-step would
			// cancel the walk" — was wrong: playerSaySpell/playerCastInstant/Combat::execute
			// never touch listWalkDir. The support-spell path relies on that and casts mid-walk.
			return false;
		}

		// ---- THE SCOPE RULE (2026-08-03) ----
		// Rune crafting used to fire in any awake state except hunting. It now needs the bot to
		// be either genuinely idle in place, or standing at the water fishing. Checked BEFORE the
		// interval re-arm below so a walking bot does not burn its next slot on a gate it cannot
		// pass — otherwise the first idle moment would usually land inside a cooling interval.
		if (!isAmbientFishing(bot.guid) && !isIdleInPlaceFor(bot, RUNE_IDLE_MIN_MS)) {
			return false;
		}

		// ...but never inside somebody's house. Player:conjureItem ends in
		// self:addItem(conjureId, count) (register_spells.lua), and luaPlayerAddItem defaults
		// canDropOnMap = TRUE — so once the backpack is full the conjured rune lands on the FLOOR.
		// The scope rule above makes that reachable: "idle in place for 10s" is exactly what a bot
		// dwelling in a house is, for minutes at a time. Same reasoning as the fidget-drop bail in
		// bot_liveness.cpp; a bot leaves nothing behind in a house it is only visiting.
		//
		// Ambient path only — `force` is the admin's own `/cavebot <bot> rune`, which stays usable
		// for testing wherever the bot happens to be standing.
		if (auto here = g_game().map.getTile(bot.currentPos); here && here->getHouse() != nullptr) {
			return false;
		}
	}

	const auto& cfg = livenessCfg_;
	const int64_t now = OTSYS_TIME();
	if (!force) {
		auto it = s_nextRuneCraftMs.find(bot.guid);
		if (it != s_nextRuneCraftMs.end() && now < it->second) {
			return false;
		}
		const int32_t lo = std::max(1000, cfg.runeCraftIntervalMinMs);
		const int32_t hi = std::max(lo, cfg.runeCraftIntervalMaxMs);
		s_nextRuneCraftMs[bot.guid] = now + uniform_random(lo, hi);
		if (uniform_random(1, 100) > cfg.runeCraftChancePct) {
			return false;
		}
	}

	const ConjureSpell* cs = signatureConjure(bot);
	if (!cs) {
		if (outMsg) {
			*outMsg = "no conjure spell for this vocation/level (knights have none by design)";
		}
		return false;
	}
	const auto& spell = g_spells().getInstantSpell(cs->words);
	if (!spell) {
		if (outMsg) *outMsg = fmt::format("spell '{}' not in registry", cs->words);
		return false;
	}
	if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell->getSpellId())
	    || player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell->getGroup())) {
		if (outMsg) *outMsg = fmt::format("'{}' still on cooldown", cs->words);
		return false;
	}
	// Mana: an idle bot's mana pool is otherwise unused, and running dry would make crafting
	// invisible for minutes at a time. Soul is handled by the bot bypass in spells.cpp.
	const uint32_t manaCost = spell->getManaCost(player);
	if (player->getMana() < manaCost) {
		if (!cfg.runeCraftRefillMana && !force) {
			if (outMsg) *outMsg = "not enough mana and botRuneCraftRefillMana is off";
			return false;
		}
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
	}

	std::string words = cs->words;
	if (g_spells().playerSaySpell(player, words) != TALKACTION_BREAK) {
		if (outMsg) *outMsg = fmt::format("playerSaySpell refused '{}'", words);
		return false;
	}
	player->saySpell(TALKTYPE_SAY, words, false);
	const std::string msg = fmt::format("{} -> item {}", words, cs->conjureId);
	castLog(bot, "RUNECRAFT: " + msg);
	// state + idleMs + fishing are logged so the scope rule is provable from journalctl alone:
	// EVERY [BotRune] line must carry state=IDLE|DWELLING with idleMs>=10000, or fishing=1.
	{
		const auto it = s_idleStationary.find(bot.guid);
		const int64_t idleMs = (it != s_idleStationary.end() && it->second.since != 0)
			? OTSYS_TIME() - it->second.since : 0;
		g_logger().info("[BotRune] guid={} name='{}' cast '{}' (item={}) state={} idleMs={} fishing={} forced={}",
		                bot.guid, bot.name, words, cs->conjureId,
		                static_cast<int>(bot.state), idleMs,
		                isAmbientFishing(bot.guid) ? 1 : 0, force ? 1 : 0);
	}
	if (outMsg) *outMsg = msg;
	return true;
}

bool BotEngine::maybeCraftRunes(BotState& bot) {
	return tryCraftRune(bot, /*force=*/false);
}

// ============================================================================
// Idle-in-place clock
// ============================================================================
//
// Driven from the outer per-bot loop, which is the ONLY place that runs for every awake bot in
// every state. A clock that only ticked in IDLE/DWELLING would go stale exactly when a bot
// leaves them, which is why BotState::fidgetStationarySince (maintained inside maybeFidgetDrop,
// reached only from doIdle/doDwelling) could not be reused for this.
void BotEngine::updateIdleStationaryClock(BotState& bot) {
	auto& rec = s_idleStationary[bot.guid];

	auto player = bot.getPlayer();
	const bool idleState = player
		&& !bot.hibernated
		&& (bot.state == BotAIState::IDLE || bot.state == BotAIState::DWELLING)
		&& bot.attackerId == 0 && bot.huntTargetId == 0
		&& bot.huntScriptId == 0 && bot.partyHuntId == 0
		// Same reasoning as the three gates above (BOT_PARTY_INVITE_RENDEZVOUS).
		&& s_rvMember.count(bot.guid) == 0 && s_partyLeaderId.count(bot.guid) == 0
		&& bot.fcState == FloorChangeState::NONE
		&& player->listWalkDir.empty()
		// Any pending errand. A bot walking to a depot locker or following a city route is not
		// idling even in the tick where its walk queue happens to be empty.
		&& !bot.hasWalkTarget && !bot.followingCityRoute && !bot.hasDepotTarget;

	// THE load-bearing check. Between two path chunks a walking bot has an empty listWalkDir for
	// a tick or two, and a teleport (boat, carpet, USE_WITH shrine) leaves every flag above
	// looking calm. Position movement is what actually distinguishes "standing" from "moving".
	if (!idleState || (rec.since != 0 && rec.at != bot.currentPos)) {
		rec.since = idleState ? OTSYS_TIME() : 0;
		rec.at = bot.currentPos;
		return;
	}
	if (rec.since == 0) {
		rec.since = OTSYS_TIME();
	}
	rec.at = bot.currentPos;
}

bool BotEngine::isIdleInPlaceFor(const BotState& bot, int64_t ms) const {
	const auto it = s_idleStationary.find(bot.guid);
	if (it == s_idleStationary.end() || it->second.since == 0) {
		return false;
	}
	return OTSYS_TIME() - it->second.since >= ms;
}

// ============================================================================
// Ambient support spells
// ============================================================================

namespace {

// Spells that survive every structural filter below but must still never be cast.
//
// ONE entry, and it earns its place: exani tera moves the bot a full floor. Every other hazard
// in the datapack is caught structurally (levitate and the summons by hasParam, challenge and
// mass healing by selfTarget, the wheel avatars by needLearn, the focus combat buffs by
// secondaryGroup). A floor-changing teleport is also exactly the class of unexpected jump the
// route-cursor logic in bot_tick.cpp has to repair, so blocking it keeps this feature out of
// that machinery entirely.
//
// exana vita (cancel magic shield) is here by explicit product decision, not for safety: it
// would sit in the same pool as utamo vita and undo it. utana vid (invisible) and exana ina
// (cancel invisibility) are deliberately NOT blocked — an invisible bot breaks nothing.
bool isSupportSpellBlocked(const std::string& words) {
	return words == "exani tera"    // magic rope — teleports the bot up a floor
	    || words == "exana vita";   // cancel magic shield — would undo utamo vita
}

// Every `spell:words(...)` declared by a script under `dir`, recursively.
//
// DIRECTORY IS THE SIGNAL, and it has to be: the whole of data/scripts/spells/conjuring/ and
// data/scripts/spells/party/ declares group("support") + isSelfTarget(true), so nothing in the
// registry distinguishes them from a haste. Found live on first deploy — a level-1200 paladin's
// pool came back with exevo con / exevo con flam / exevo con grav / exevo con hur / exevo con
// mort / exevo con pox / exevo con vis / exevo gran con grav / exevo infir con in it, i.e. the
// bot would have conjured ammunition while walking across town, which is precisely the thing
// this whole change exists to stop. enchant_spear (exeta con) is worse than cosmetic: it
// CONSUMES a real spear per cast (reagent 3277, which is why buildConjureTables rejects it too).
//
// Sweeping the directory rather than listing words means a datapack addition is excluded
// automatically instead of silently joining the pool.
std::unordered_set<std::string> collectSpellWordsInDir(const std::string& dir) {
	std::unordered_set<std::string> out;
	namespace fs = std::filesystem;
	if (!fs::exists(dir)) {
		g_logger().warn("[BotSupply] spell dir '{}' not found — support pool may be over-inclusive", dir);
		return out;
	}
	const std::regex wordsRe(R"RX(spell:words\s*\(\s*"([^"]+)")RX");
	for (const auto& entry : fs::recursive_directory_iterator(dir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
			continue;
		}
		std::ifstream file(entry.path());
		if (!file.is_open()) {
			continue;
		}
		const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::smatch m;
		if (std::regex_search(content, m, wordsRe)) {
			out.insert(m[1].str());
		}
	}
	return out;
}

} // namespace

// Walks the LIVE registry rather than parsing Lua (unlike buildConjureTables, which has to parse
// because the registry does not record that a spell conjures). Everything we need here — group,
// self-target, aggressive, param/target/learn/weapon requirements, level, vocations, cooldown —
// is already on the registry object.
void BotEngine::buildSupportSpellTables() {
	for (auto& v : resolvedSupport_) {
		v.clear();
	}

	// Cross-reference set: words the engine's OWN heal ladder depends on that carry a long
	// cooldown. CONDITION_SPELLCOOLDOWN is keyed by spell id regardless of who cast it, so an
	// ambient cast at full HP burns the very cooldown doHealing is saving. exura gran ico is the
	// knight emergency heal at 600s — casting it while idling in a temple would leave that bot
	// with no emergency heal for ten minutes. Stated as "never spend a cooldown doHealing is
	// saving" rather than as a blanket cooldown ceiling: utura/utura gran are also >30s and also
	// group("healing"), but appear nowhere in the ladder, so a blanket rule would wrongly drop
	// two spells nothing is waiting on.
	// Whole-directory exclusions. conjuring/ is the rune-crafting feature's territory — and that
	// feature is now idle/fishing-gated, so letting its spells in through the support pool would
	// hand back exactly what the gate takes away. party/ spells (the `* mas sio` family) belong
	// to a party; bot_party.cpp already owns party support during party hunts, and support spells
	// are excluded while hunting anyway, so a solo bot casting Enchant Party is pure noise.
	std::unordered_set<std::string> excludedByDir = collectSpellWordsInDir("data/scripts/spells/conjuring");
	{
		const auto partyWords = collectSpellWordsInDir("data/scripts/spells/party");
		excludedByDir.insert(partyWords.begin(), partyWords.end());
	}

	std::unordered_set<std::string> ladderLongCd;
	for (uint8_t baseVoc = 1; baseVoc <= 4; ++baseVoc) {
		for (const auto& hs : getHealSpells(baseVoc)) {
			// Read the cooldown from the LIVE registry, not from hs.cd: the hardcoded table can
			// drift from the Lua, and the live value is the one the condition is built from.
			const auto& spell = g_spells().getInstantSpell(hs.name);
			if (spell && spell->getCooldown() > static_cast<uint32_t>(HEAL_LONG_CD_THRESHOLD_S) * 1000) {
				ladderLongCd.insert(hs.name);
			}
		}
	}

	for (const auto& [words, spell] : g_spells().getInstantSpells()) {
		if (!spell || words.empty() || words[0] == '#') {
			continue; // monster/internal spells register as "#####462"
		}
		const SpellGroup_t group = spell->getGroup();
		if (group != SPELLGROUP_SUPPORT && group != SPELLGROUP_HEALING) {
			continue;
		}
		// The focus/crippling combat buffs (blood rage, protector, sharpshooter, swift foot)
		// declare a secondary group. They are 10-second in-combat buffs; a player does not cast
		// them walking through town, and they would burn the focus group cooldown for nothing.
		if (spell->getSecondaryGroup() != SPELLGROUP_NONE) {
			continue;
		}
		// Self-target alone removes exura sio, mass healing, challenge, exevo pan, exiva and the
		// summons. The rest of these keep the cast to something a bot can complete unattended.
		if (!spell->getSelfTarget() || spell->getAggressive() || spell->getNeedTarget()
		    || spell->getNeedWeapon() || !spell->isEnabled() || spell->getHasParam()) {
			continue;
		}
		// needLearn matters more than it looks: playerSpellCheck BYPASSES the learn check for bot
		// players (spells.cpp) and, because that branch is an else-if, skips the vocation check
		// with it. The wheel avatars would therefore look castable and then fail inside their own
		// Lua with a visible POFF.
		if (spell->getNeedLearn()) {
			continue;
		}
		if (isSupportSpellBlocked(words) || excludedByDir.count(words) != 0) {
			continue;
		}
		if (ladderLongCd.count(words) != 0) {
			continue;
		}

		SupportSpell ss;
		ss.words = words;
		ss.level = spell->getLevel();
		// Promoted vocation ids are base+4; the registry keys on the promoted id. This also
		// excludes the monk-only spells (monk 9 / exalted monk 10) for free.
		const auto& vocMap = spell->getVocMap();
		for (uint8_t baseVoc = 1; baseVoc <= 4; ++baseVoc) {
			if (vocMap.find(static_cast<uint16_t>(baseVoc + 4)) != vocMap.end()) {
				ss.vocMask |= vocBit(baseVoc);
			}
		}
		if (ss.vocMask == 0) {
			// Includes exura dis, which declares vocation("none") and so has an EMPTY vocSpellMap.
			// playerSpellCheck treats an empty map as "anyone may cast", but requiring an explicit
			// vocation entry is the conservative reading, matches buildConjureTables exactly, and
			// costs us only a 1-point practice heal no leveled player casts. Deliberate.
			continue;
		}

		for (uint8_t baseVoc = 1; baseVoc <= 4; ++baseVoc) {
			if ((ss.vocMask & vocBit(baseVoc)) != 0) {
				resolvedSupport_[baseVoc].push_back(ss);
			}
		}
	}

	// Stable order so a registry iteration change cannot silently reshuffle what each bot rolls.
	for (auto& v : resolvedSupport_) {
		std::sort(v.begin(), v.end(),
			[](const SupportSpell& a, const SupportSpell& b) { return a.words < b.words; });
	}

	g_logger().info("[BotSupply] support spells discovered: sorc={} druid={} pal={} knight={}",
	                resolvedSupport_[1].size(), resolvedSupport_[2].size(),
	                resolvedSupport_[3].size(), resolvedSupport_[4].size());
}

bool BotEngine::trySupportSpell(BotState& bot, bool force, std::string* outMsg) {
	auto player = bot.getPlayer();
	if (!player) {
		if (outMsg) *outMsg = "no player object (hibernated?)";
		return false;
	}
	// "Any awake state EXCEPT hunting", inherited verbatim from what rune crafting used to be.
	// PARTY is a party hunt, so it counts as hunting. Holds even when forced — it is the
	// feature's defining scope, not a pacing gate.
	if (bot.state == BotAIState::HUNTING || bot.huntScriptId > 0
	    || bot.state == BotAIState::PARTY || bot.partyHuntId > 0
	    // BOT_PARTY_INVITE_RENDEZVOUS: a human-led member is state-IDLE while it walks in,
	    // so the PARTY/partyHuntId pair above misses it entirely. Being SELECTED here is the
	    // problem, not merely being walked: the errand claims resources and fights the walk.
	    || s_rvMember.count(bot.guid) > 0 || s_partyLeaderId.count(bot.guid) > 0) {
		if (outMsg) *outMsg = "bot is hunting — support spells are excluded while hunting";
		return false;
	}
	if (!force) {
		if (bot.attackerId != 0 || bot.huntTargetId != 0) {
			return false;
		}
		if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::PK_ATTACK
		    || bot.state == BotAIState::FLEEING) {
			return false; // PvP has its own haste (pvpCastBestHaste) and its own mana budget
		}
		if (bot.fcState != FloorChangeState::NONE) {
			return false; // the one place a stray cast could confuse the FC verification step
		}
		// listWalkDir is deliberately NOT checked — casting while walking is the point.
	}

	const auto& cfg = livenessCfg_;
	const int64_t now = OTSYS_TIME();
	if (!force) {
		auto it = s_nextSupportSpellMs.find(bot.guid);
		if (it != s_nextSupportSpellMs.end() && now < it->second) {
			return false;
		}
		// Re-arm on every check, hit or miss, so a failed roll is paced rather than retried on
		// the next slot 200ms later.
		const int32_t lo = std::max(1000, cfg.supportSpellIntervalMinMs);
		const int32_t hi = std::max(lo, cfg.supportSpellIntervalMaxMs);
		s_nextSupportSpellMs[bot.guid] = now + uniform_random(lo, hi);
		if (uniform_random(1, 100) > cfg.supportSpellChancePct) {
			return false;
		}
	}

	const uint8_t baseVoc = getBaseVocation(bot.vocationId);
	if (baseVoc < 1 || baseVoc > 4) {
		if (outMsg) *outMsg = fmt::format("no support pool for vocation {}", bot.vocationId);
		return false;
	}
	const auto& pool = resolvedSupport_[baseVoc];
	if (pool.empty()) {
		if (outMsg) *outMsg = "support pool is empty (see [BotSupply] at startup)";
		return false;
	}

	// Eligibility is level + cooldowns + mana, nothing else. No condition gating by design: a bot
	// may re-cast a buff it already has (addCondition refreshes in place, it does not stack) or
	// cure a condition it does not have (a dispel with nothing to dispel is a no-op).
	const int32_t level = player->getLevel();
	std::vector<const SupportSpell*> eligible;
	eligible.reserve(pool.size());
	for (const auto& ss : pool) {
		if (level < static_cast<int32_t>(ss.level)) {
			continue;
		}
		const auto& spell = g_spells().getInstantSpell(ss.words);
		if (!spell) {
			continue;
		}
		if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell->getSpellId())
		    || player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell->getGroup())) {
			continue;
		}
		if (player->getMana() < spell->getManaCost(player)) {
			continue; // no refill — bot_tick.cpp already tops mana up below 50%
		}
		eligible.push_back(&ss);
	}
	if (eligible.empty()) {
		if (outMsg) {
			*outMsg = fmt::format("no castable support spell right now ({} in pool, level {})",
				pool.size(), level);
		}
		return false;
	}

	const SupportSpell* pick = eligible[uniform_random(0, static_cast<int32_t>(eligible.size()) - 1)];
	std::string words = pick->words;
	if (g_spells().playerSaySpell(player, words) != TALKACTION_BREAK) {
		if (outMsg) *outMsg = fmt::format("playerSaySpell refused '{}'", words);
		return false;
	}
	player->saySpell(TALKTYPE_SAY, words, false);
	castLog(bot, "SUPPORT: " + words);
	// state is logged on purpose: acceptance is "lines appear for TRAVELING/IDLE/DWELLING and
	// NEVER for HUNTING/PARTY", which has to be checkable from journalctl alone.
	g_logger().info("[BotSupport] guid={} name='{}' cast '{}' state={} eligible={}/{} forced={}",
	                bot.guid, bot.name, words, static_cast<int>(bot.state),
	                eligible.size(), pool.size(), force ? 1 : 0);
	if (outMsg) *outMsg = words;
	return true;
}

bool BotEngine::maybeSupportSpell(BotState& bot) {
	return trySupportSpell(bot, /*force=*/false);
}

// `/cavebot <bot> supportlist`. Shows the whole pool with a per-entry verdict so a wrong
// vocation or level filter is visible without waiting for a cast that never comes.
std::string BotEngine::describeSupportPool(const BotState& bot) const {
	auto player = bot.getPlayer();
	if (!player) {
		return "bot has no player object — wake it first";
	}
	const uint8_t baseVoc = getBaseVocation(bot.vocationId);
	if (baseVoc < 1 || baseVoc > 4) {
		return fmt::format("no support pool for vocation {}", bot.vocationId);
	}
	const auto& pool = resolvedSupport_[baseVoc];
	if (pool.empty()) {
		return "support pool is empty (see [BotSupply] at startup)";
	}

	const int32_t level = player->getLevel();
	std::string out = fmt::format("[SUPPORT] '{}' voc={} level={} pool={}\n",
		bot.name, bot.vocationId, level, pool.size());
	uint32_t ready = 0;
	for (const auto& ss : pool) {
		const char* why = "READY";
		const auto& spell = g_spells().getInstantSpell(ss.words);
		const uint32_t mana = spell ? spell->getManaCost(player) : 0;
		if (level < static_cast<int32_t>(ss.level)) {
			why = "level";
		} else if (!spell) {
			why = "not in registry";
		} else if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell->getSpellId())) {
			why = "spell cooldown";
		} else if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell->getGroup())) {
			why = "group cooldown";
		} else if (player->getMana() < mana) {
			why = "mana";
		} else {
			ready++;
		}
		out += fmt::format("  {:<18} lvl {:<4} mana {:<5} {}\n", ss.words, ss.level, mana, why);
	}
	out += fmt::format("  -> {}/{} castable now\n", ready, pool.size());
	return out;
}

// Send the bot fishing right now, exactly as a winning POI reroll would. Used by
// `/cavebot <bot> fish` so the behaviour can be exercised without waiting out the weighted
// reroll lottery.
bool BotEngine::forceFishingTrip(BotState& bot, std::string& outMsg) {
	auto player = bot.getPlayer();
	if (!player) {
		outMsg = "bot has no player object — wake it first";
		return false;
	}
	if (bot.state == BotAIState::HUNTING || bot.huntScriptId > 0
	    || bot.state == BotAIState::PARTY || bot.partyHuntId > 0
	    // BOT_PARTY_INVITE_RENDEZVOUS: a human-led member is state-IDLE while it walks in,
	    // so the PARTY/partyHuntId pair above misses it entirely. Being SELECTED here is the
	    // problem, not merely being walked: the errand claims resources and fights the walk.
	    || s_rvMember.count(bot.guid) > 0 || s_partyLeaderId.count(bot.guid) > 0) {
		outMsg = "bot is hunting — use 'endhunt' first";
		return false;
	}
	if (fishingSpots_.empty()) {
		outMsg = "fishing index is empty (see [BotFish] at startup)";
		return false;
	}
	FishingSpot spot;
	if (!selectFishingSpot(bot, spot)) {
		outMsg = fmt::format("no fishing spot within {} tiles of ({},{},{}) for town {}",
			livenessCfg_.fishMaxDist, bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
			findNearestTown(bot.currentPos));
		return false;
	}

	// Respect the same claim the natural reroll honours — forcing a trip should not let an admin
	// walk two bots onto the same water either.
	if (isFishSpotClaimed(spot, bot.guid)) {
		outMsg = fmt::format("water ({},{},{}) is claimed by another bot — try again shortly",
			spot.water.x, spot.water.y, spot.water.z);
		return false;
	}

	// Drop whatever it was doing, then set up the same state the reroll's POI branch sets.
	clearFishingRun(bot.guid);        // also releases any claim THIS bot still held
	claimFishingSpot(bot.guid, spot); // AFTER clearFishingRun, which would drop it right back
	bot.stopCooldownUntil = 0;   // a prior `stop` would otherwise block the arrival reroll
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.hasDepotTarget = false;
	bot.pendingNavDest.clear();
	bot.pathFailCount = 0;

	s_pendingFishSpot[bot.guid] = spot;   // arrival uses THIS pair, not a re-derived one
	auto& poi = s_forcedFishPoi[bot.guid];
	poi = BotPOI { "_fishing", spot.stand, POIType::WATER };
	bot.walkTarget = spot.stand;
	bot.hasWalkTarget = true;
	bot.currentPOI = &poi;
	bot.state = BotAIState::IDLE;   // POI arrival is handled in doIdle, not doDwelling
	s_plannerWalk[bot.guid] = bot.walkTarget;

	outMsg = fmt::format("walking to fish at stand ({},{},{}) water ({},{},{})",
		spot.stand.x, spot.stand.y, spot.stand.z, spot.water.x, spot.water.y, spot.water.z);
	castLog(bot, "FISH: " + outMsg);
	g_logger().info("[BotFish] guid={} name='{}' FORCED trip -> stand ({},{},{})",
	                bot.guid, bot.name, spot.stand.x, spot.stand.y, spot.stand.z);
	return true;
}

// ============================================================================
// Fishing
// ============================================================================

// Four tiers, tried in this strict priority order — each one a closer or cheaper option than the
// one after it, so nothing below tier N ever gets a chance while tier N still has an answer:
//
//   P1  same floor,  live scan,        <=FISH_LOCAL_RADIUS (20) tiles
//   P2  same floor,  prebuilt index,   <=botFishMaxDist (150) tiles
//   P3  other floor, live scan,        portal-anchored (dz=1) / z-shift (dz>=2), <=zBand floors
//   P4  other floor, prebuilt index,   <=botFishMaxDist (150) tiles, <=zBand floors
//
// P1 is tried first because it is the cheapest AND the most realistic when it succeeds: bounded by
// FISH_LOS_PROBE_BUDGET, no index lookup at all, and it hugs the actual shoreline (see
// findNearbyFishingSpot's own header). The prebuilt index thins at 12x12 and fills each town's
// spot cap in raw sweep order with no backfill, so a whole side of a town can be unrepresented — a
// bot at the Thais depot was once being routed to distant water while open sea sat a dozen tiles
// away. Asking the map directly, at the bot's actual position, is the only thing that answers "the
// closest water to me". Everything below only runs when P1 finds nothing local.
//
// P2 before P3 is the important ordering, not an accident. A same-floor spot 80 tiles away is
// still "walk there and fish", exactly what a player does; a cross-floor spot 8 tiles away is "go
// down a ladder to fish", which a player only does when the water in front of them really is the
// only water there is. So the index's own same-floor candidates (haveSameFloor, below) are checked
// BEFORE P3 gets a chance to hand back a geometrically closer cross-floor pick. Trying local
// cross-floor before the far same-floor index was considered and rejected: it would make bots
// prefer a floor change over a plain walk whenever the floor below happened to have nearer water,
// which is backwards from how a real player decides where to fish.
//
// P3 exists because the index is coarse in exactly the way P1 already fixes for the same-floor
// case: 12x12 grid-thinned, capped at botFishMaxSpotsPerTown per town in raw sweep order, "stand"
// resolved as a tile merely ADJACENT to water. A town whose only nearby water sits one floor down
// can easily have nothing in the index within zBand for that floor at all, or an index entry far
// closer to a DIFFERENT part of the shore than the one actually nearest the bot right now. P3 asks
// findNearbyFishingSpot the same question again, one floor at a time, by shifting `from.z` — that
// function itself stays same-floor-only per call (see its own header), so this loop is the only
// place a cross-floor LIVE result can come from.
//
// Cross-floor risk, and how P3/P4 both bound it: a forced trip once picked an index z6->z7 spot in
// Rathleton and the bot never moved — planScopedWalk reported "started" every tick with an empty
// walk queue and no PLAN line, while `/cavebot zplan` planned that exact same route fine. That is a
// plan/execution gap, not a missing route, so nothing here can fully rule it out by construction —
// but three things bound EXPOSURE to it, same discipline in both P3 and the index filter below:
//   - haveSameFloor discards cross-floor OUTRIGHT whenever any same-floor water exists at all, so
//     cross-floor is only even attempted for a town with genuinely nothing on the bot's own floor —
//     structurally rare.
//   - every cross-floor candidate, live or indexed, is gated on zGraphReady_: no portal graph, no
//     cross-floor pick, full stop.
//   - dz is capped to zBand (the tighter of botFishZBand / botFishMaxDz), not the looser
//     botFishMaxDz alone, so a wide-open botFishMaxDz cannot widen cross-floor exposure on its own.
// A pre-flight zPlanFullRoute() call was considered as a fourth layer and rejected: the Rathleton
// route WAS plannable (zplan proved it), so a plan-validity check would not have caught that exact
// failure, and calling it here would force findNearbyFishingSpot/selectFishingSpot non-const for a
// check that cannot deliver the guarantee it looks like it delivers. Hardening the general planner
// stall detector (plannerWalkBlocked currently only fires when listWalkDir is non-empty, which
// misses this exact "started with an empty queue" signature) would close the gap for real, but that
// is a planScopedWalk-level fix shared with NPC visits, not a fishing-selection one.
bool BotEngine::selectFishingSpot(const BotState& bot, FishingSpot& out) const {
	// P1 — same floor, live scan.
	if (findNearbyFishingSpot(bot.currentPos, FISH_LOCAL_RADIUS, out)) {
		return true;
	}

	// The town the bot is physically STANDING in, not its home townId — bots roam and the two
	// drift, which is the same reason the NPC visit switched to findNearestTown.
	uint32_t town = findNearestTown(bot.currentPos);
	auto it = fishingSpots_.find(town);
	if (it == fishingSpots_.end() || it->second.empty()) {
		it = fishingSpots_.find(bot.townId);
	}
	if (it == fishingSpots_.end() || it->second.empty()) {
		return false;
	}
	const auto& spots = it->second;
	const auto& cfg = livenessCfg_;
	// The effective cross-floor ceiling: the TIGHTER of the two configured caps. Hoisted to a name
	// because P3's floor loop below needs to walk exactly this same range, not just gate on it.
	const int32_t zBand = std::max(0, std::min(cfg.fishZBand, cfg.fishMaxDz));

	// NEAREST first. An earlier version drew randomly from the town's list on the theory that it
	// would spread bots along the shore; in practice it sent a bot standing five tiles from the
	// Thais canal off to water 140 tiles away, which is the opposite of how a player behaves.
	// Spread now comes from the tie-break below instead: gather the closest few and pick among
	// them, so bots at the same spot don't all converge on one identical tile while still going
	// to water that is actually near them.
	//
	// This gathers BOTH same-floor and cross-floor candidates (P2 and P4's raw material) in one
	// pass; which of the two `eligible` ends up serving is decided by haveSameFloor below.
	struct Ranked { const FishingSpot* spot; int32_t dist; };
	std::vector<Ranked> eligible;
	eligible.reserve(spots.size());
	for (const auto& s : spots) {
		const int32_t dz = std::abs(static_cast<int32_t>(s.stand.z) - static_cast<int32_t>(bot.currentPos.z));
		if (dz != 0) {
			// Cross-floor needs the portal graph; without it the planner cannot decompose the
			// route and the walk would degrade to greedy floor-change scanning.
			if (!zGraphReady_ || dz > zBand) {
				continue;
			}
		}
		const int32_t d = std::max(
			std::abs(static_cast<int32_t>(s.stand.x) - static_cast<int32_t>(bot.currentPos.x)),
			std::abs(static_cast<int32_t>(s.stand.y) - static_cast<int32_t>(bot.currentPos.y)));
		if (d > cfg.fishMaxDist) {
			continue;
		}
		// A floor change costs far more than a few tiles of walking, so weight it heavily —
		// otherwise a spot one floor down "ties" with one a few tiles along the same shore.
		eligible.push_back({ &s, d + dz * 40 });
	}

	// STRONGLY prefer the bot's own floor. Not just a cost preference — if any same-floor water
	// is in range (however far, up to botFishMaxDist), cross-floor candidates are discarded
	// outright and P3 below never runs. A player fishes at the water in front of them rather than
	// descending to another level; see this function's own header for the full cross-floor
	// discipline (P2 before P3, and why).
	const bool haveSameFloor = std::any_of(eligible.begin(), eligible.end(),
		[&](const Ranked& r) { return r.spot->stand.z == bot.currentPos.z; });
	if (haveSameFloor) {
		// P2 — same floor, prebuilt index. `eligible` narrows to same-floor survivors; picked by
		// the shared sort+pick at the bottom.
		std::erase_if(eligible, [&](const Ranked& r) { return r.spot->stand.z != bot.currentPos.z; });
	} else if (zGraphReady_ && zBand > 0) {
		// P3 — other floor, PORTAL-ANCHORED live scan (dz = 1).
		//
		// The naive version of this (re-running findNearbyFishingSpot from (bot.x, bot.y, bot.z±dz))
		// is blind to reachability: that point only COINCIDENTALLY shares x/y with the bot and can
		// sit in a completely disconnected part of the target floor. Observed at Rathleton — a bot
		// at (33585,31900,6) was sent to stand (33571,31886,7), 14 tiles west, while the planner's
		// own route there went through a portal at (33635,31944,6), 50 tiles EAST, because that
		// stretch of z7 was only reachable through a portal nowhere near the bot.
		//
		// So: enumerate the REAL portals one hop off the bot's floor, anchor the water search on
		// each one's LANDING — where the bot will actually arrive — and rank by total walking
		// distance, (bot -> portal) + (landing -> stand). The winner is by construction close to a
		// tile the bot can genuinely reach with one floor change. That also tends to pull the route
		// planner toward the same nearby portal when it plans the walk afterwards: not because
		// anything here forces its hand (zPlanFullRoute always re-derives its own Dijkstra), but
		// because the target it is handed now sits near a validated-reachable landing instead of in
		// an unreachable pocket. It does NOT fix planScopedWalk/ZLEG execution stalls.
		//
		// Efficiency: losBudget is created ONCE and threaded through every candidate, so the whole
		// phase costs at most FISH_LOS_PROBE_BUDGET materializing probes — the same ceiling as one
		// P1 call, regardless of how many portals are nearby — and candidates are capped nearest-
		// first at FISH_PORTAL_CANDIDATES_MAX so a portal-dense area cannot blow it up either.
		struct PortalCand { Position pos, landing; int32_t distToPortal; };
		std::vector<PortalCand> portalCands;
		zGraph_.forEachOnFloorNear(bot.currentPos.z, bot.currentPos, Z_LEG_MAX,
			[&](uint32_t /*idx*/, const botnav::ZPortal& p) {
				// Single-hop, genuine floor change only. p.pos.z == bot.currentPos.z is already
				// guaranteed by forEachOnFloorNear's per-floor index; a landing two or more floors
				// away needs an intermediate hop this enumeration cannot see, so those fall to the
				// legacy z-shift below.
				const int32_t hopDz = static_cast<int32_t>(p.landing.z) - static_cast<int32_t>(bot.currentPos.z);
				if (hopDz != 1 && hopDz != -1) {
					return;
				}
				portalCands.push_back({ p.pos, p.landing, botnav::zCheb(bot.currentPos, p.pos) });
			});
		std::sort(portalCands.begin(), portalCands.end(),
			[](const PortalCand& a, const PortalCand& b) { return a.distToPortal < b.distToPortal; });
		if (static_cast<int32_t>(portalCands.size()) > FISH_PORTAL_CANDIDATES_MAX) {
			portalCands.resize(FISH_PORTAL_CANDIDATES_MAX);
		}

		FishingSpot best;
		int32_t bestCost = std::numeric_limits<int32_t>::max();
		bool foundAnchored = false;
		int32_t losBudget = FISH_LOS_PROBE_BUDGET; // shared across EVERY candidate below
		for (const auto& c : portalCands) {
			FishingSpot cand;
			if (findNearbyFishingSpot(c.landing, FISH_LOCAL_RADIUS, cand, losBudget)) {
				// Flat sum of two Chebyshev distances, both already this codebase's distance unit.
				// No per-hop constant: it would be added to every candidate here and so cannot
				// change the ranking — this phase is only ever compared against its own members.
				const int32_t cost = c.distToPortal + botnav::zCheb(c.landing, cand.stand);
				if (cost < bestCost) {
					bestCost = cost;
					best = cand;
					foundAnchored = true;
				}
			}
			if (losBudget <= 0) {
				break; // probe ceiling hit — keep whatever was already found
			}
		}
		if (foundAnchored) {
			out = best;
			return true;
		}

		// dz >= 2 — only reachable when an admin raises botFishZBand past its default of 1. No
		// single portal spans two floors, so anchoring these the way dz=1 just did would need a
		// real multi-hop PATH rather than a portal lookup — materially bigger than this change.
		// Kept as the original blind z-shift, and only after the anchored search found nothing.
		for (int32_t dz = 2; dz <= zBand; ++dz) {
			if (bot.currentPos.z >= dz) {
				const Position shallower(bot.currentPos.x, bot.currentPos.y,
					static_cast<uint8_t>(bot.currentPos.z - dz));
				if (findNearbyFishingSpot(shallower, FISH_LOCAL_RADIUS, out)) {
					return true;
				}
			}
			if (bot.currentPos.z + dz < MAP_MAX_LAYERS) {
				const Position deeper(bot.currentPos.x, bot.currentPos.y,
					static_cast<uint8_t>(bot.currentPos.z + dz));
				if (findNearbyFishingSpot(deeper, FISH_LOCAL_RADIUS, out)) {
					return true;
				}
			}
		}
	}

	// P2 (same-floor survivors) or P4 (cross-floor, untouched since haveSameFloor was false and
	// P3 above found nothing) — whichever `eligible` holds at this point.
	if (eligible.empty()) {
		return false;
	}
	std::sort(eligible.begin(), eligible.end(),
		[](const Ranked& a, const Ranked& b) { return a.dist < b.dist; });
	// Pick among the closest few (all of which are genuinely near), not the whole town.
	const size_t pool = std::min<size_t>(eligible.size(), 3);
	out = *eligible[uniform_random(0, static_cast<int32_t>(pool) - 1)].spot;
	return true;
}

// Called from the WATER branch of the POI-arrival handler. The bot is standing on (or beside)
// its chosen shore tile; from here the run owns the bot until it walks home.
void BotEngine::startFishingRun(BotState& bot) {
	FishingRun run;
	run.phase = FishPhase::FISHING;
	run.casts = 0;

	// Use the spot chosen when the trip started. Re-deriving it here from bot.currentPos was a
	// real bug: POI_ARRIVAL_DIST is 3, so the bot can be three tiles off the stand tile, and
	// re-deriving discards the index's walkability + LOS vetting in favour of whatever water
	// happens to sit next to wherever it halted. tickFishingRun closes the remaining gap.
	// Re-confirm against the LIVE map now that we are actually here. The frozen pick was made
	// from wherever the bot stood when it decided to fish, and POI_ARRIVAL_DIST lets it halt up
	// to 3 tiles off; a fresh local scan from the real arrival position is strictly more accurate
	// than the snapshot. (Freezing was the right call only while the alternative was an unvetted
	// probe of adjacent tiles — with a properly vetted local scan available, it no longer is.)
	// Falls back to the frozen pick when the live scan finds nothing, e.g. water only reachable
	// past the arrival slop.
	FishingSpot live;
	if (findNearbyFishingSpot(bot.currentPos, FISH_LOCAL_RADIUS, live)) {
		run.water = live.water;
		run.stand = live.stand;
		s_pendingFishSpot.erase(bot.guid);
	} else {
		auto pend = s_pendingFishSpot.find(bot.guid);
		if (pend == s_pendingFishSpot.end()) {
			castLog(bot, "FISH: arrived with no water in range and no pending spot — abandoning");
			return;
		}
		run.water = pend->second.water;
		run.stand = pend->second.stand;
		s_pendingFishSpot.erase(pend);
	}

	const uint32_t town = findNearestTown(bot.currentPos);

	// Pick the way home NOW, while we still know which town this is. Deciding at the end would
	// risk the bot's nearest town having drifted mid-session, sending it to the wrong continent.
	// Nearest carpet/boat/temple/depot POI of this town — the same anchors the reroll would send
	// it to next anyway, so the walk home doubles as the start of its next errand.
	run.home = bot.currentPos;
	{
		int32_t bestD = INT32_MAX;
		auto considerTown = [&](uint32_t tid) {
			auto pit = cityPOIs_.find(tid);
			if (pit == cityPOIs_.end()) {
				return;
			}
			for (const auto& poi : pit->second) {
				if (poi.type != POIType::TEMPLE && poi.type != POIType::DEPOT
				    && poi.type != POIType::DEPOT_OUTSIDE && poi.type != POIType::BOAT) {
					continue;
				}
				const int32_t d = std::abs(static_cast<int32_t>(poi.pos.x) - static_cast<int32_t>(bot.currentPos.x))
					+ std::abs(static_cast<int32_t>(poi.pos.y) - static_cast<int32_t>(bot.currentPos.y))
					+ std::abs(static_cast<int32_t>(poi.pos.z) - static_cast<int32_t>(bot.currentPos.z)) * 10;
				if (d < bestD) {
					bestD = d;
					run.home = poi.pos;
				}
			}
		};
		considerTown(town);
		if (bestD == INT32_MAX) {
			considerTown(bot.townId);
		}
	}

	const auto& cfg = livenessCfg_;
	const int32_t lo = std::max(5, cfg.fishDurationMinSec);
	const int32_t hi = std::max(lo, cfg.fishDurationMaxSec);
	const int32_t dur = uniform_random(lo, hi);
	const int64_t now = OTSYS_TIME();
	run.until = now + dur * 1000LL;
	run.nextCastMs = now;
	// Grace period to walk the last POI_ARRIVAL_DIST tiles onto the chosen stand tile before the
	// first cast — see the approach block in tickFishingRun for why arrival alone is not enough.
	run.approachUntil = now + 20000;

	// Hold DWELLING open past the session. doDwelling's tail is the SOLE authority for leaving
	// DWELLING and knows nothing about this run, so an ordinary 60-300s POI dwell would expire
	// mid-session and reroll the bot away from its own fishing trip.
	bot.dwellUntil = run.until + 30000;

	s_fishing[bot.guid] = run;
	castLog(bot, fmt::format("FISH: fishing at ({},{},{}) for {}s, home=({},{},{})",
		run.water.x, run.water.y, run.water.z, dur, run.home.x, run.home.y, run.home.z));
	g_logger().info("[BotFish] guid={} name='{}' arrived water=({},{},{}) dur={}s",
	                bot.guid, bot.name, run.water.x, run.water.y, run.water.z, dur);
}

// The ONE way a fishing run leaves the shore for home, shared by the normal session end and by
// both defense exits (hurt / gave up on a monster).
//
// This is a whole block of state, not a phase flip, and that distinction bit once already:
// tickFishingRun returns false for any phase but FISHING, and TRAVEL/RETURNING are driven by the
// planner off bot.walkTarget. Setting `phase = RETURNING` alone would leave the bot standing in
// DWELLING with nothing ticking it until dwellUntil expired, at which point the processBot guard
// would log "interrupted" and destroy the run — the bot would never walk home at all.
//
// Also clears the defend sub-state: the walk home must not be spent holding a monster target,
// which would keep every huntTargetId-gated behaviour (rune crafting, support spells, liveness,
// mounting) shut off and the 600ms combat cadence armed for the whole trip back.
void BotEngine::beginFishingReturn(BotState& bot, FishingRun& run, const char* reason) {
	clearFishingDefense(bot, run);
	// The planner claim is re-taken here; isPlannerWalk compares against bot.walkTarget on every
	// call, so the old claim self-invalidated the moment we changed the target.
	run.phase = FishPhase::RETURNING;
	bot.walkTarget = run.home;
	bot.hasWalkTarget = true;
	bot.currentPOI = nullptr;
	bot.pendingNavDest.clear();
	bot.pathFailCount = 0;
	bot.state = BotAIState::IDLE;
	s_plannerWalk[bot.guid] = bot.walkTarget;
	castLog(bot, fmt::format("FISH: {} ({} casts, {} kills) — returning to ({},{},{})",
		reason, run.casts, run.defendKills, run.home.x, run.home.y, run.home.z));
	g_logger().info("[BotFish] guid={} name='{}' returning reason={} casts={} defendKills={}",
	                bot.guid, bot.name, reason, run.casts, run.defendKills);
}

// Drop the engagement without touching the rest of the run. Safe to call when not engaged.
void BotEngine::clearFishingDefense(BotState& bot, FishingRun& run) {
	if (run.defendTargetId == 0) {
		return;
	}
	if (auto player = bot.getPlayer()) {
		// Only detach OUR monster. An unconditional null would cancel a target something else
		// set — the PvP path in particular runs in the same tick as some teardowns.
		auto attacked = player->getAttackedCreature();
		if (attacked && attacked->getID() == run.defendTargetId) {
			player->setAttackedCreature(nullptr);
		}
		player->setFollowCreature(nullptr);
	}
	if (bot.huntTargetId == run.defendTargetId) {
		bot.huntTargetId = 0;
	}
	s_retreatUntil.erase(bot.guid);
	s_approachCooldown.erase(bot.guid);
	run.defendTargetId = 0;
	run.defendSinceMs = 0;
}

// Nearest monster currently targeting THIS bot, on our floor and in client view range.
std::shared_ptr<Creature> BotEngine::pickFishingThreat(const BotState& bot) const {
	auto player = bot.getPlayer();
	if (!player) return nullptr;

	std::shared_ptr<Creature> best;
	int32_t bestDist = INT32_MAX;
	auto spectators = Spectators().find<Monster>(bot.currentPos, false,
		MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_Y, MONSTER_SCAN_RADIUS_Y);
	for (const auto& spec : spectators) {
		auto monster = spec->getMonster();
		if (!monster || spec->isRemoved() || spec->getHealth() <= 0) continue;
		const auto mpos = spec->getPosition();
		if (mpos.z != bot.currentPos.z) continue;
		// "Actively attacking us" is the whole trigger. Anything looser turns a defensive
		// behaviour into bots wandering off to aggro the scenery.
		auto mtarget = monster->getAttackedCreature();
		if (!mtarget || mtarget->getID() != player->getID()) continue;
		const int32_t d = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(mpos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(mpos.y)));
		if (d < bestDist) {
			bestDist = d;
			best = spec;
		}
	}
	return best;
}

// One tick of shore self-defense. Returns true when it owned the tick.
//
// Runs INSIDE the FISHING phase and never changes bot.state, because the processBot guard
// destroys the run on any transition out of DWELLING. Returning true is also what protects a long
// fight: it starves doDwelling's tail, which is the sole authority for leaving DWELLING.
bool BotEngine::tickFishingDefense(BotState& bot, FishingRun& run) {
	auto player = bot.getPlayer();
	if (!player) return false;

	const int64_t now = OTSYS_TIME();

	// Never fight from inside a protection zone. castSpell refuses to fire from one, but
	// chaseTarget has no such check — without this gate a stale aggro reading would walk the bot
	// out of safety while it is structurally unable to hit anything.
	if (auto selfTile = g_game().map.getTile(bot.currentPos);
	    selfTile && selfTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
		clearFishingDefense(bot, run);
		return false;
	}

	std::shared_ptr<Creature> target;
	if (run.defendTargetId != 0) {
		target = g_game().getCreatureByID(run.defendTargetId);
		const bool dead = !target || target->isRemoved() || target->getHealth() <= 0;
		if (dead) {
			run.defendKills++;
			// Pay the fight back into the session so combat does not silently eat the trip.
			run.defendOwedMs += now - run.defendSinceMs;
			castLog(bot, fmt::format("FISH: killed attacker #{} — {} owed back",
				run.defendKills, run.defendOwedMs));
			g_logger().info("[BotFish] guid={} name='{}' defend kill #{} owedMs={}",
			                bot.guid, bot.name, run.defendKills, run.defendOwedMs);
			clearFishingDefense(bot, run);
			target = nullptr;
		} else if (target->getPosition().z != bot.currentPos.z) {
			castLog(bot, "FISH: attacker left the floor — back to fishing");
			clearFishingDefense(bot, run);
			target = nullptr;
		} else if (now - run.defendSinceMs > FISH_DEFEND_MAX_MS) {
			castLogError(bot, fmt::format("FISH: cannot kill {} in {}s — leaving",
				target->getName(), FISH_DEFEND_MAX_MS / 1000));
			beginFishingReturn(bot, run, "fight timed out");
			return true;
		}
	}

	// Not engaged (or just disengaged): look for someone attacking us. Packs re-engage here with
	// a fresh per-engagement clock.
	if (!target) {
		target = pickFishingThreat(bot);
		if (!target) {
			if (run.defendOwedMs > 0) {
				// Nothing left attacking — resume. Give back the combat time, re-stamp the spot
				// claim (FISH_CLAIM_MS carries only 60s of slack over the planned round trip, so
				// a long fight would otherwise let the claim lapse and a second bot walk in), and
				// re-open the approach window so the bot walks back onto its stand tile.
				run.until += run.defendOwedMs;
				run.defendOwedMs = 0;
				claimFishingSpot(bot.guid, FishingSpot { run.water, run.stand });
				run.approachUntil = now + 20000;
				bot.dwellUntil = run.until + 30000;
				castLog(bot, "FISH: shore clear — resuming the session");
				g_logger().info("[BotFish] guid={} name='{}' defend resume until=+{}s",
				                bot.guid, bot.name, (run.until - now) / 1000);
			}
			return false; // let the normal casting flow have the tick
		}
		run.defendTargetId = target->getID();
		run.defendSinceMs = now;
		castLog(bot, fmt::format("FISH: {} is attacking — fighting back", target->getName()));
		g_logger().info("[BotFish] guid={} name='{}' defend engage target='{}' id={}",
		                bot.guid, bot.name, target->getName(), run.defendTargetId);
	}

	// Retreat before the fight rather than after dying. doHealing runs at 200ms for every awake
	// bot regardless of state, so walking home is a real option.
	const int32_t maxHp = player->getMaxHealth();
	if (maxHp > 0 && player->getHealth() * 100 / maxHp <= FISH_DEFEND_RETREAT_HP_PCT) {
		beginFishingReturn(bot, run, "too hurt to keep fishing");
		return true;
	}

	// Keep the outer dwell timer ahead of the fight — the return below is what actually holds
	// DWELLING, but a stale dwellUntil would strand the bot if this hook ever stopped running.
	bot.dwellUntil = std::max(bot.dwellUntil, now + 30000);
	// Mirrors the hunt path's "current monster target" contract, which every busy-gate in the
	// engine already reads (rune crafting, support spells, liveness micro-actions, fidget drop,
	// the idle clock, mount retry, waypoint jitter) plus the debug overlay. Cleared by
	// clearFishingDefense, and by clearFishingRun for the teardowns that bypass this loop.
	bot.huntTargetId = run.defendTargetId;

	// Re-assert the engine target EVERY tick. castSpell only sets it when a single-target instant
	// wins its scoring — with a rune winner, or nothing off cooldown, it never fires and a knight
	// would stand in melee range dealing exactly zero damage until the abort timer.
	{
		auto zIt = s_lastZChangeTime.find(bot.guid);
		const bool inZGrace = (zIt != s_lastZChangeTime.end() && now - zIt->second < Z_CHANGE_GRACE_MS);
		if (!inZGrace && player->getAttackedCreature() != target) {
			player->setAttackedCreature(target);
		}
	}

	castSpell(bot, target);

	// Leash: chase only while the fight stays near the spot. Past that, hold the shore and keep
	// attacking if it is still in reach; the abort timer resolves a monster that simply runs away.
	const auto tpos = target->getPosition();
	const int32_t distFromStand = std::max(
		std::abs(static_cast<int32_t>(run.stand.x) - static_cast<int32_t>(tpos.x)),
		std::abs(static_cast<int32_t>(run.stand.y) - static_cast<int32_t>(tpos.y)));
	if (distFromStand <= FISH_DEFEND_LEASH) {
		chaseTarget(bot, target);
	} else {
		// setAttackedCreature re-arms engine follow under chaseMode whenever followCreature is not
		// already the target, and Creature::setAttackedCreature silently resets the engagement on a
		// transient canSee/z failure — so the guarded re-assert above legitimately re-fires mid-hold.
		// This has to run on EVERY holding tick, not once.
		player->setFollowCreature(nullptr);
	}
	return true;
}

bool BotEngine::tickFishingRun(BotState& bot) {
	auto runIt = s_fishing.find(bot.guid);
	if (runIt == s_fishing.end()) {
		return false;
	}
	auto player = bot.getPlayer();
	if (!player) {
		clearFishingRun(bot.guid);
		return false;
	}
	FishingRun& run = runIt->second;
	const int64_t now = OTSYS_TIME();

	if (run.phase != FishPhase::FISHING) {
		return false; // TRAVEL and RETURNING are driven by the planner via hasWalkTarget
	}

	// Keep the outer dwell timer ahead of the session. Recomputed from run.until every tick
	// rather than incremented, so if this hook ever stops running the timer simply expires on
	// its own instead of pinning the bot in DWELLING forever.
	bot.dwellUntil = run.until + 30000;

	// Self-defense first, and deliberately AHEAD of the session-end check: a bot that walks off
	// mid-fight because its timer happened to expire is the same "ignores the monster" bug wearing
	// a different hat. Finish the fight, then the extended session runs down normally.
	if (tickFishingDefense(bot, run)) {
		return true;
	}

	if (now >= run.until) {
		beginFishingReturn(bot, run, "session done");
		return true;
	}

	// Close the last few tiles if we halted short. POI arrival tolerates 3 tiles, and the fishing
	// action needs same floor, Actions::canUseFar's <7,5> box and clear line of sight — usually
	// satisfied from wherever the bot stopped, but not always. Walk to the vetted stand tile
	// until the cast is actually legal rather than firing a useItemEx the server will refuse.
	// Close the last few tiles onto the CHOSEN stand tile before casting.
	//
	// POI arrival fires anywhere within POI_ARRIVAL_DIST (3) of the walk target, and from three
	// tiles short the cast is usually still legal — observed live: stand (32937,32075,7), bot
	// stopped at (32934,32075,7), water (32939,32077,7) still inside <7,5>, so the range gate
	// below was satisfied and the bot fished from there. That silently discards the work of
	// picking the walkable tile nearest the water, which is the whole point of that search.
	//
	// Bounded by approachUntil so a stand tile that turns out occupied or newly blocked costs a
	// few seconds and then falls through to casting from wherever the bot actually is, rather
	// than burning the session walking into something it cannot reach.
	if (bot.currentPos != run.stand && now < run.approachUntil) {
		if (player->listWalkDir.empty() && !goToClosest(bot, run.stand, 0)) {
			run.approachUntil = 0; // unreachable — stop trying, cast from here if legal
		}
		return true; // ours while we close the gap
	}

	const bool inRange = bot.currentPos.z == run.water.z
		&& Position::areInRange<7, 5>(run.water, bot.currentPos)
		&& g_game().map.isSightClear(bot.currentPos, run.water, true);
	if (!inRange) {
		if (player->listWalkDir.empty()) {
			if (!goToClosest(bot, run.stand, 0)) {
				castLog(bot, fmt::format("FISH: cannot reach stand tile ({},{},{}) — ending session",
					run.stand.x, run.stand.y, run.stand.z));
				clearFishingRun(bot.guid);
				return false;
			}
		}
		return true; // still ours while we close the gap
	}

	if (now < run.nextCastMs) {
		return true; // holding between casts — still ours
	}
	const auto& cfg = livenessCfg_;
	const int32_t lo = std::max(200, cfg.fishCastIntervalMinMs);
	const int32_t hi = std::max(lo, cfg.fishCastIntervalMaxMs);
	run.nextCastMs = now + uniform_random(lo, hi);

	if (castToolAt(bot, ITEM_FISHING_ROD, run.water)) {
		run.casts++;
	}
	return true;
}

// ---- The ONE cast ----
// Shared by ambient fishing, ice fishing and `tool:` waypoint markers. See the header for why the
// tool is a TEMP item rather than one pulled out of the bag.
//
// Deliberately has NO range gate and NO teardown of its own. Range is the caller's business
// (ambient checks <7,5>+LOS against its vetted stand tile; ice requires true adjacency because the
// pick, unlike the rod, is not allowFarUse). Teardown likewise: pulling clearFishingRun in here
// would let an ice session invoke ambient-fishing cleanup.
bool BotEngine::castToolAt(BotState& bot, uint16_t toolId, const Position& target) {
	auto player = bot.getPlayer();
	if (!player) return false;

	auto tool = Item::CreateItem(toolId, 1);
	if (!tool) {
		castLogError(bot, fmt::format("CAST: CreateItem({}) failed", toolId));
		return false;
	}

	// Face the target, like a player does before using a tool on it.
	const Direction dir = getDirectionTo(bot.currentPos, target);
	if (player->getDirection() != dir) {
		g_game().internalCreatureTurn(player, dir);
	}

	// Resolve the target's stackpos so STACKPOS_USETARGET picks the ground item and not something
	// lying on top of it — the same resolution the AdvStone dummy kickoff does. Verified via
	// data/items/appearances.dat that every id we aim at (water 4597-4602/4609-4614/629-634,
	// fragile ice 7200, ice hole 7236/7237) carries the protobuf `bank` flag, i.e. Canary loads
	// them into tile->ground and not tile->items (src/items/items.cpp:168, io/iomap.cpp:175).
	uint8_t stackPos = 0;
	if (const auto& tile = g_game().map.getTile(target)) {
		if (const auto& ground = tile->getGround()) {
			const int32_t sp = tile->getThingIndex(ground);
			if (sp >= 0 && sp <= 255) {
				stackPos = static_cast<uint8_t>(sp);
			}
		}
	}
	// Temp tool has no parent, so there is no real fromPos to resolve — the bot's own tile is what
	// the machete/rope/shovel paths have always passed, and every Lua action we drive
	// (fishing.onUse, onUsePick, onUseMachete, onUseRope) ignores fromPosition.
	return g_actions().useItemEx(player, bot.currentPos, target, stackPos, tool, false);
}

// ============================================================================
// Ice fishing — the `fish:<dx>,<dy>` waypoint marker
// ============================================================================
//
// Driven from handleActionWaypoint (bot_tick.cpp) on arrival at a hunt waypoint carrying the
// marker, and held in place by the gate in followWaypoints (bot_waypoint.cpp). The bot is already
// standing where it should be, so unlike the ambient FishingRun this never walks anywhere: it
// stands, works the hole, and lets the patrol resume.

void BotEngine::beginIceFishSession(BotState& bot, const Position& target, bool untilClosed) {
	// Never run both fishing systems on one bot. Three independent things already make this
	// unreachable (patrol runs state==HUNTING; bot_tick.cpp tears down s_fishing on any tick the
	// bot is not DWELLING/IDLE-with-walk; bot_party.cpp skips fishing bots for recruitment), so
	// this is belt-and-braces rather than the guarantee.
	if (isFishing(bot.guid)) {
		castLogError(bot, "ICEFISH: ambient fishing run active — skipping marker");
		return;
	}
	const uint16_t ground = groundIdAt(target);
	if (ground != ICE_FRAGILE && ground != ICE_HOLE_FISH) {
		castLog(bot, fmt::format("ICEFISH: ({},{},{}) is ground {} — nothing to fish",
			target.x, target.y, target.z, ground));
		return;
	}

	const int64_t now = OTSYS_TIME();
	IceFishSession s;
	s.target = target;
	s.untilClosed = untilClosed;
	s.endsAtMs = untilClosed ? now + ICE_MANUAL_CAP_MS
	                         : now + uniform_random(ICE_SESSION_MIN_MS, ICE_SESSION_MAX_MS);
	// Floor is above the 500ms action-waypoint pause the arrival site sets, so the first use is
	// not swallowed by that gate before this session ever gets a tick.
	s.nextUseMs = now + uniform_random(600, 1200);
	iceFishing_[bot.guid] = s;

	const Direction dir = getDirectionTo(bot.currentPos, target);
	if (auto player = bot.getPlayer(); player && player->getDirection() != dir) {
		g_game().internalCreatureTurn(player, dir);
	}
	castLog(bot, fmt::format("ICEFISH: begin ({},{},{}) ground={} for {}s",
		target.x, target.y, target.z, ground, (s.endsAtMs - now) / 1000));
}

// `/cavebot <bot> fishice` — work the ice next to the bot.
// `/cavebot <bot> fishice waypoint` — jump to the nearest `fish:` hunt waypoint and work that,
// which exercises the real data path (marker -> offset -> session) rather than an ad-hoc target.
// Both run untilClosed: the bot picks the hole open and keeps casting until it transforms.
std::string BotEngine::forceIceFish(BotState& bot, bool useWaypoint) {
	auto player = bot.getPlayer();
	if (!player) return "bot has no player object — wake it first";

	if (useWaypoint) {
		// Nearest WORKABLE marked waypoint across every loaded script (so this keeps working if
		// other hunts gain `fish:` markers later). Workable matters: a hole another bot just
		// fished is 7237 for the next 900s, and picking the merely-nearest marker would strand an
		// admin on a spent hole with a confusing "not ice" reply. Holes are re-checked live, so
		// this naturally walks the field as bots empty it.
		const Waypoint* best = nullptr;
		const HuntScript* bestScript = nullptr;
		Position bestTarget;
		int64_t bestDist = std::numeric_limits<int64_t>::max();
		int32_t markersSeen = 0, markersSpent = 0;
		for (const auto& script : huntScripts_) {
			for (const auto& wp : script.patrolWaypoints) {
				if (wp.extraData.rfind("fish:", 0) != 0) continue;
				markersSeen++;
				int32_t mx = 0, my = 0;
				uint16_t unused = 0;
				if (!parseOffsetMarker(wp.extraData, "fish:", mx, my, unused)) continue;
				const Position t(static_cast<uint16_t>(wp.pos.x + mx),
				                 static_cast<uint16_t>(wp.pos.y + my), wp.pos.z);
				const uint16_t gid = groundIdAt(t);
				if (gid != ICE_FRAGILE && gid != ICE_HOLE_FISH) { markersSpent++; continue; }
				const int64_t dx = static_cast<int64_t>(wp.pos.x) - bot.currentPos.x;
				const int64_t dy = static_cast<int64_t>(wp.pos.y) - bot.currentPos.y;
				const int64_t d = dx * dx + dy * dy;
				if (d < bestDist) { bestDist = d; best = &wp; bestScript = &script; bestTarget = t; }
			}
		}
		if (!best) {
			if (markersSeen == 0) {
				return "no waypoint anywhere carries a `fish:` marker "
				       "(did database/bots/16_ice_fishing_waypoints.sql run?)";
			}
			return fmt::format("all {} `fish:` markers are on spent holes right now "
				"(emptied holes refreeze after 15 min) — try again shortly", markersSpent);
		}
		const Position stand = best->pos;
		const Position target = bestTarget;
		BOT_TELEPORT(player, stand, true);
		bot.currentPos = stand;
		bot.lastPos = stand;
		// Stop whatever else was driving the bot, or it walks off the stand mid-session.
		bot.hasWalkTarget = false;
		bot.pendingNavDest.clear();
		bot.followingCityRoute = false;
		bot.cityRouteWps.clear();
		bot.cityRouteIdx = 0;
		beginIceFishSession(bot, target, /*untilClosed=*/true);
		if (!isIceFishing(bot.guid)) {
			return fmt::format("teleported to stand ({},{},{}) but the target ({},{},{}) is not ice",
				stand.x, stand.y, stand.z, target.x, target.y, target.z);
		}
		return fmt::format("'{}' wp ({},{},{}) -> working ice ({},{},{}) until it closes",
			bestScript ? bestScript->name : "?", stand.x, stand.y, stand.z,
			target.x, target.y, target.z);
	}

	// Adjacent form: own tile last, so a bot standing beside a hole prefers the hole.
	static const std::array<std::pair<int32_t, int32_t>, 9> kRing = { {
		{0, 1}, {0, -1}, {1, 0}, {-1, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}, {0, 0}
	} };
	for (const auto& [ox, oy] : kRing) {
		const Position p(static_cast<uint16_t>(bot.currentPos.x + ox),
		                 static_cast<uint16_t>(bot.currentPos.y + oy), bot.currentPos.z);
		const uint16_t gid = groundIdAt(p);
		if (gid == ICE_FRAGILE || gid == ICE_HOLE_FISH) {
			bot.hasWalkTarget = false;
			bot.pendingNavDest.clear();
			beginIceFishSession(bot, p, /*untilClosed=*/true);
			return isIceFishing(bot.guid)
				? fmt::format("working ice ({},{},{}) ground={} until it closes", p.x, p.y, p.z, gid)
				: "refused to start — see the log";
		}
	}
	return fmt::format("no fragile ice (7200) or ice hole (7236) within 1 sqm of ({},{},{}) "
		"— try `fishice waypoint`", bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
}

void BotEngine::endIceFishSession(BotState& bot, const char* reason) {
	auto it = iceFishing_.find(bot.guid);
	if (it == iceFishing_.end()) return;
	castLog(bot, fmt::format("ICEFISH: end ({}) casts={} picks={}",
		reason, it->second.casts, it->second.picks));
	if (it->second.casts > 0 || it->second.picks > 0) {
		g_logger().info("[BotIceFish] guid={} name='{}' {} casts={} picks={}",
			bot.guid, bot.name, reason, it->second.casts, it->second.picks);
	}
	iceFishing_.erase(it);
}

// Returns true while the bot must hold position. Every end condition is OR'd and re-checked every
// tick, which is what makes a stale session harmless: whatever happens to the bot in between, the
// first followWaypoints call after it resumes tears the session down before it can hold for more
// than one tick. That self-heal — not the opportunistic erases scattered through the lifecycle
// paths — is the correctness guarantee here.
bool BotEngine::tickIceFishSession(BotState& bot) {
	auto it = iceFishing_.find(bot.guid);
	if (it == iceFishing_.end()) return false;

	auto player = bot.getPlayer();
	if (!player) { iceFishing_.erase(it); return false; }

	IceFishSession& s = it->second;
	const int64_t now = OTSYS_TIME();

	if (now >= s.endsAtMs) { endIceFishSession(bot, "time up"); return false; }
	// Combat always wins — a bot standing at a hole while a badger chews on it reads as a bug.
	if (bot.huntTargetId > 0 || bot.attackerId > 0) {
		endIceFishSession(bot, "combat"); return false;
	}
	// Adjacency IS the range gate. castToolAt has none of its own, and the pick is not
	// allowFarUse (data/scripts/actions/tools/pick.lua registers id 3456 with no far-use) while
	// the rod is (fishing.lua). Loosening this check would silently break the pick and leave the
	// rod working — do not relax it without giving the pick its own range test.
	const int32_t dx = std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(s.target.x));
	const int32_t dy = std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(s.target.y));
	if (bot.currentPos.z != s.target.z || dx > 1 || dy > 1) {
		endIceFishSession(bot, "moved away"); return false;
	}

	const uint16_t ground = groundIdAt(s.target);
	if (ground == ICE_HOLE_EMPTY) {
		// Caught it (or someone else did) — the hole is spent and refreezes on its own in 900s.
		endIceFishSession(bot, "hole emptied"); return false;
	}
	if (ground != ICE_FRAGILE && ground != ICE_HOLE_FISH) {
		endIceFishSession(bot, "no longer ice"); return false;
	}

	if (now < s.nextUseMs) return true; // holding between uses — still ours

	const auto& cfg = livenessCfg_;
	const int32_t lo = std::max(200, cfg.fishCastIntervalMinMs);
	const int32_t hi = std::max(lo, cfg.fishCastIntervalMaxMs);
	s.nextUseMs = now + uniform_random(lo, hi);

	if (ground == ICE_FRAGILE) {
		// Closed. onUsePick (register_actions.lua) transforms it to 7236 and shows HITAREA.
		if (castToolAt(bot, ITEM_PICK, s.target)) {
			s.picks++;
			castLog(bot, fmt::format("ICEFISH: pick @({},{},{})", s.target.x, s.target.y, s.target.z));
		}
	} else {
		// Open with fish visible. fishing.lua rolls the catch; on success it transforms the hole
		// to 7237, which ends this session on the next tick via the check above.
		if (castToolAt(bot, ITEM_FISHING_ROD, s.target)) {
			s.casts++;
			castLog(bot, fmt::format("ICEFISH: cast @({},{},{}) n={}",
				s.target.x, s.target.y, s.target.z, s.casts));
		}
	}
	return true;
}

// ---- Stand + water claims ----
// Two bots casting into the same water from two different stands was invisible to A*, which only
// ever keeps them off the same STAND tile. These three functions are the whole mechanism; every
// release in the engine funnels through clearFishingRun, which calls releaseFishingSpot first.

bool BotEngine::isFishSpotClaimed(const FishingSpot& spot, uint32_t byGuid) const {
	const int64_t now = OTSYS_TIME();
	auto taken = [&](const Position& p) {
		auto it = s_fishClaims.find(botTileKey(p));
		return it != s_fishClaims.end() && it->second.expiresAt > now && it->second.guid != byGuid;
	};
	return taken(spot.stand) || taken(spot.water);
}

void BotEngine::claimFishingSpot(uint32_t guid, const FishingSpot& spot) {
	const int64_t expiresAt = OTSYS_TIME() + FISH_CLAIM_MS;
	s_fishClaims[botTileKey(spot.stand)] = FishClaim { guid, expiresAt };
	s_fishClaims[botTileKey(spot.water)] = FishClaim { guid, expiresAt };
}

void BotEngine::releaseFishingSpot(uint32_t guid) {
	std::erase_if(s_fishClaims, [guid](const auto& kv) { return kv.second.guid == guid; });
}

void BotEngine::clearFishingRun(uint32_t guid) {
	// Drop the stand+water claim FIRST and unconditionally — every path below can return
	// early (no pending spot, no active run) and the claim must go regardless of which.
	releaseFishingSpot(guid);
	// Always drop a pending spot, even when no run started — an abandoned walk would otherwise
	// leave it to be consumed by an unrelated later arrival.
	s_pendingFishSpot.erase(guid);
	auto it = s_fishing.find(guid);
	if (it == s_fishing.end()) {
		return;
	}
	// THE defense teardown funnel. Two paths destroy a run mid-fight and neither one goes through
	// tickFishingDefense: the PvP interrupt (processBot's leaving-DWELLING guard fires the same
	// tick doSelfDefense sets COMBAT, and exitCombat clears attackerId but NEVER huntTargetId) and
	// hibernation (hibernateBot has no combat gate, and huntTargetId survives the pool round-trip
	// because wakeBot does not clear it). Leaving the field set there strands the bot behind every
	// huntTargetId-gated behaviour — walking and rerolling forever but never crafting, chatting,
	// fidgeting or mounting again.
	//
	// Placed AFTER the run-exists early-out on purpose: callers that reach this function while a
	// bot holds a GENUINE hunt target (party conscription, deactivateBot) hold no run and return
	// above, so they cannot stomp it.
	const uint32_t defendId = it->second.defendTargetId; // capture BEFORE the erase below
	if (defendId != 0) {
		auto idxIt = guidToIndex_.find(guid);
		if (idxIt != guidToIndex_.end()) {
			auto& defBot = bots_[idxIt->second];
			// getPlayer() can be null here — tickFishingRun itself calls this function precisely
			// when the Player handle has gone.
			if (auto player = defBot.getPlayer()) {
				auto attacked = player->getAttackedCreature();
				if (attacked && attacked->getID() == defendId) {
					player->setAttackedCreature(nullptr);
				}
				player->setFollowCreature(nullptr);
			}
			if (defBot.huntTargetId == defendId) {
				defBot.huntTargetId = 0;
			}
		}
		s_retreatUntil.erase(guid);
		s_approachCooldown.erase(guid);
	}
	s_fishing.erase(it);
	// Drop the planner claim and its hop plan with the run that created them. Safe to call on a
	// bot that never had one — releaseNpcApproach inside is a no-op for a bot holding no
	// approach reservation, which fishing deliberately never does.
	clearPlannerWalk(guid);
	// Put dwellUntil back to something ordinary. startFishingRun stretched it well past a normal
	// POI dwell; leaving that behind would strand the bot — virtualAdvanceDwelling is a pure
	// timer, and bot_party.cpp's recruitment snapshot copies dwellUntil verbatim.
	auto idx = guidToIndex_.find(guid);
	if (idx != guidToIndex_.end()) {
		auto& bot = bots_[idx->second];
		const int64_t normal = OTSYS_TIME()
			+ uniform_random(std::max(1, livenessCfg_.dwellRerollMinSec),
			                 std::max(1, livenessCfg_.dwellRerollMaxSec)) * 1000LL;
		if (bot.dwellUntil > normal) {
			bot.dwellUntil = normal;
		}
	}
}
