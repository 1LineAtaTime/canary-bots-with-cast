/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_house.cpp — BOT_HOUSE_VISIT: awake bots visit bot-owned houses.
//
// An awake bot in a town rolls a chance to walk to a bot-owned house there, opens the door on the
// way, and idles at one interior tile for a few minutes — optionally greeting a hireling, standing
// at a locker, or training at an exercise dummy. When the idle window ends it rerolls its next
// activity through the ordinary reroll, so "stay a bit longer" is just the existing IDLE weight
// rather than a special case, and the walk OUT is handed to the scoped route planner because
// leaving needs the same door handling that got the bot in.
//
// SAME FLOOR ONLY. The idle tile, the locker and the dummy are all on the floor the bot walks in
// on. That is a product decision, not a limitation waiting to be lifted: it also keeps the feature
// clear of the cross-floor planner stall that currently holds botFishZBand at 0.
//
// Division of labour, following the rule fishing established:
//   * the INTERIOR INDEX (which tiles, which furniture) is harvested in bot_zgraph.cpp, riding the
//     whole-map sweep that already reads every cell — sweep code lives with the sweep;
//   * everything here is policy: which house, which tile, which activity, who claims what, and the
//     per-bot run itself.
//
// The run lives in a guid-keyed map rather than in BotState, mirroring FishingRun, because
// BotState is the ABI boundary and tuning this later should stay a .so-only rebuild.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#include "creatures/npcs/npc.hpp"     // hireling NPCs standing in a house
#include "lua/creature/actions.hpp"   // g_actions().useItemEx — the dummy training kickoff
#include "map/house/house.hpp"        // House::getOwner/getTownId/getEntryPosition/getDoors

namespace {

// How many houses to sample before giving up on this roll. The town's list is unordered, so a
// handful of random draws is enough to find an eligible one when any exist, and bounded when the
// town's houses are all full, all too far, or all unindexed.
constexpr int HOUSE_SAMPLES = 8;

// Chebyshev distance, the same metric every other reach cap in the engine uses.
int32_t houseCheb(const Position& a, const Position& b) {
	return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
	                std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
}

// Is this indexed tile actually stand-on-able RIGHT NOW?
//
// The interior index is a CANDIDATE set, not an authority — the same relationship the fishing
// index has with findNearbyFishingSpot. It is harvested from the BasicTile cache, which carries
// the OTBM's original furniture; a house's real contents are restored from the tile_store blob
// into LIVE tiles at world load, and an unmaterialized cell therefore reads as an empty room.
// Measured: tiles the index called idle turned out to hold restored furniture, and the planner
// correctly reported the target unreachable.
//
// Checking live here materializes only the handful of tiles of the ONE house being considered,
// which is bounded and cheap — unlike a whole-map getTile pass, which is why the harvest itself
// must stay on the cache.
// `allowAvoid` relaxes the BLOCKPATH test, and the reason is measured rather than assumed.
//
// TILESTATE_BLOCKPATH comes from the appearances protobuf `avoid` flag
// (items.cpp: `iType.blockPathFind = object.flags().avoid()`), which means "pathfinding should
// PREFER not to cross this", not "a creature cannot stand here". A comfy chair (28933) and a
// square side table (31207) declare no blocking attributes at all in items.xml -- they carry
// only `avoid`.
//
// Verified against our OWN pathfinder with `/cavebot <bot> route`: it routes ONTO a comfy chair
// (21 tiles, OK) and ONTO a side table (32 tiles, OK), crossing other furniture on the way. So
// treating BLOCKPATH as unreachable is stricter than the engine actually is, and it made a
// shrine ringed by chairs look impossible when a bot could have walked to it.
//
// Default stays false so the dummy, locker, hireling and idle modes keep their exact current
// behaviour -- this is not the change to alter those under. Shrines pass true. Relaxing it for
// dummies and lockers too is now a one-argument change at those call sites.
bool houseTileStandable(const Position& p, uint32_t houseId, bool allowAvoid = false) {
	const auto& tile = g_game().map.getTile(p);
	if (!tile || !tile->getGround()) {
		return false;
	}
	const auto& house = tile->getHouse();
	if (!house || house->getId() != houseId) {
		return false; // re-indexed, rebuilt, or simply not this house after all
	}
	if (tile->hasFlag(TILESTATE_BLOCKSOLID)
	    || (!allowAvoid && tile->hasFlag(TILESTATE_BLOCKPATH))
	    || tile->hasFlag(TILESTATE_FLOORCHANGE) || tile->hasFlag(TILESTATE_TELEPORT)) {
		return false;
	}
	return tile->getTopVisibleCreature(nullptr) == nullptr;
}

// Every tile a bot could stand on to USE this piece of furniture, checked live, all eight sides.
//
// Deliberately NOT filtered from the harvested idle list. A locker or a dummy is usable from
// whichever sides happen to be open — right, left, north, south — and that varies per house; the
// harvested list is a cache-derived approximation that drops any tile the OTBM snapshot thought was
// occupied, so intersecting with it threw away legitimate approach tiles and reported "nothing
// standable beside the locker" for furniture with a perfectly good free side. The live map is the
// authority for a question this specific, and the cost is eight getTile calls on one tile.
std::vector<Position> houseStandsAround(const Position& feature, uint32_t houseId,
                                       bool allowAvoid = false) {
	std::vector<Position> out;
	for (int32_t dx = -1; dx <= 1; ++dx) {
		for (int32_t dy = -1; dy <= 1; ++dy) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			const Position p(static_cast<uint16_t>(static_cast<int32_t>(feature.x) + dx),
			                 static_cast<uint16_t>(static_cast<int32_t>(feature.y) + dy),
			                 feature.z);
			if (houseTileStandable(p, houseId, allowAvoid)) {
				out.push_back(p);
			}
		}
	}
	return out;
}

// Why a specific tile cannot be stood on. Diagnostic only — houseTileStandable is the authority
// and this mirrors it exactly, which is why the two sit next to each other.
//
// Exists because "nothing standable beside the locker" is unfalsifiable from the outside: it could
// mean walls, furniture, a creature, or the tile belonging to a neighbouring flat, and those want
// completely different responses.
const char* houseStandFailReason(const Position& p, uint32_t houseId) {
	const auto& tile = g_game().map.getTile(p);
	if (!tile) return "no-tile";
	if (!tile->getGround()) return "no-ground";
	const auto& house = tile->getHouse();
	if (!house) return "not-a-house-tile";
	if (house->getId() != houseId) return "other-house";
	if (tile->hasFlag(TILESTATE_BLOCKSOLID)) return "blocksolid";
	if (tile->hasFlag(TILESTATE_BLOCKPATH)) return "blockpath";
	if (tile->hasFlag(TILESTATE_FLOORCHANGE)) return "floorchange";
	if (tile->hasFlag(TILESTATE_TELEPORT)) return "teleport";
	if (tile->getTopVisibleCreature(nullptr) != nullptr) return "creature";
	return "OK";
}

struct HouseShrineStand {
	Position stand;
	Position shrine;
	uint8_t  kind = 0;   // SHRINE_KIND_REWARD / _IMBUING
};

// Per-(house, floor) memo for the scan below.
//
// The scan runs for every sampled house before the TABLE C roll -- the roll has to know whether the
// house HAS a shrine to offer SHRINE as a candidate -- so without a memo it was paid HOUSE_SAMPLES
// times per house pick. Measured cost of that: the processBot p95 doubled from 67ms to 134ms the day
// it shipped, and a bisect disabling house visits pulled it back to 101ms and halved the >200ms tick
// spikes. So this is not speculative hygiene.
//
// TTL rather than engine-lifetime, unlike the TOWN shrine memo, because the exposures differ in
// kind: map furniture cannot move while the engine lives, but house furniture demonstrably does --
// an owner rearranges (which is exactly how the idleTiles bug was found), BotHouseReclaim re-places
// items on restore, and tile_store is rewritten on world save. Both staleness modes are benign and
// self-correcting, so a 15-minute backstop is proportionate. The empty result is cached too, and
// that is where most of the win is: the large majority of bot houses have no shrine at all.
struct HouseShrineMemo {
	std::vector<HouseShrineStand> stands;
	int64_t builtAt = 0;
};
std::unordered_map<uint64_t, HouseShrineMemo> s_houseShrineMemo;
constexpr int64_t HOUSE_SHRINE_MEMO_TTL_MS = 900000; // 15 min

// Live scan of ONE house for shrines, returning {stand, shrine, kind}.
//
// Anchored on House::getTiles() -- the house's OWN live tile list -- and on
// houseStandsAround/houseTileStandable, which are the exact predicates the dummy and locker modes
// use. That is the whole point of this rewrite.
//
// The first version walked floor.idleTiles and probed THEIR cardinal neighbours, and it was wrong
// in a way that only showed up in the field: idleTiles comes from the map sweep, which reads the
// BasicTile cache holding the OTBM's ORIGINAL furniture. A shrine bought from the store and a
// chair the owner has since dragged aside are both invisible to it. Reported live: an operator
// cleared the furniture around a reward shrine, the tiles became genuinely standable, and this
// still refused -- because those tiles were never in idleTiles to be probed in the first place.
// bot_house.cpp says as much at the top of the file: the interior index is a candidate set, not an
// authority. Discovery of anything the owner can MOVE has to go to the live map.
//
// Cardinal stand tiles are preferred because internalCreatureTurn is 4-directional and only a
// cardinal neighbour can actually be faced. A diagonal is accepted as a fallback rather than
// refusing the shrine outright -- standing beside it looking slightly past it is much closer to
// what was asked for than walking 30 tiles to a different one.
std::vector<HouseShrineStand> houseShrineScan(uint32_t houseId, uint8_t z) {
	const int64_t t0 = botMonoMs();
	std::vector<HouseShrineStand> out;

	const auto& house = g_game().map.houses.getHouse(houseId);
	if (!house) {
		return out;
	}

	for (const auto& htile : house->getTiles()) {
		if (!htile) {
			continue;
		}
		const Position pos = htile->getPosition();
		if (pos.z != z) {
			continue;
		}
		const auto* items = htile->getItemList();
		if (!items) {
			continue;
		}
		uint8_t kind = 0;
		for (const auto& item : *items) {
			const uint16_t id = item->getID();
			for (const uint16_t r : kRewardShrineIds) if (id == r) { kind = SHRINE_KIND_REWARD; break; }
			if (!kind) for (const uint16_t i : kImbuingShrineIds) if (id == i) { kind = SHRINE_KIND_IMBUING; break; }
			if (kind) break;
		}
		if (!kind) {
			continue;
		}

		// All eight sides, live, via the same helper the dummy and locker modes use. Cardinals win;
		// a diagonal is kept only if nothing cardinal is free.
		Position best;
		bool haveBest = false, bestCardinal = false;
		for (const auto& cand : houseStandsAround(pos, houseId, /*allowAvoid=*/true)) {
			const bool cardinal = (cand.x == pos.x) != (cand.y == pos.y);
			if (!haveBest || (cardinal && !bestCardinal)) {
				best = cand;
				haveBest = true;
				bestCardinal = cardinal;
			}
			if (bestCardinal) {
				break;
			}
		}
		if (haveBest) {
			out.push_back(HouseShrineStand { best, pos, kind });
		}
	}

	if (const int64_t dt = botMonoMs() - t0; dt > 20) {
		g_logger().warn("[SHRINE_HOUSESCAN_SLOW] house={} z={} hits={} {}ms",
		                houseId, z, out.size(), dt);
	}
	return out;
}

// Memo-backed entry point. Every caller goes through this; nothing calls houseShrineScan directly.
const std::vector<HouseShrineStand>& houseShrineStands(uint32_t houseId, uint8_t z) {
	const uint64_t key = (static_cast<uint64_t>(houseId) << 8) | z;
	const int64_t now = botMonoMs();
	auto it = s_houseShrineMemo.find(key);
	if (it != s_houseShrineMemo.end() && now - it->second.builtAt < HOUSE_SHRINE_MEMO_TTL_MS) {
		return it->second.stands;
	}
	auto& slot = s_houseShrineMemo[key];
	slot.stands = houseShrineScan(houseId, z);
	slot.builtAt = now;
	return slot.stands;
}

} // namespace

// ============================================================================
// Indexes
// ============================================================================

// Which houses belong to bots, bucketed by town. Deliberately NOT part of the map sweep: this is
// an iteration over a few hundred House objects, not 18M cells, so it belongs with the feature —
// the same placement buildConjureTables uses.
//
// Rebuilt on every loadHuntData (so /cavebot reload picks up ownership changes). Ownership is
// re-checked at pick time anyway, because a house can be sold or rent-evicted between rebuilds.
void BotEngine::buildBotHouseIndex() {
	botHousesByTown_.clear();
	const auto botAcct = static_cast<uint32_t>(g_configManager().getNumber(BOT_HOUSE_ACCOUNT_ID));
	uint32_t owned = 0;
	for (const auto& [houseId, house] : g_game().map.houses.getHouses()) {
		if (!house || house->getOwner() == 0 || house->getOwnerAccountId() != botAcct) {
			continue;
		}
		botHousesByTown_[house->getTownId()].push_back(houseId);
		++owned;
	}
	g_logger().info("[HOUSE_INDEX] {} bot-owned houses across {} towns", owned, botHousesByTown_.size());

	// Loud, once, if the stock account-ownership shortcut is on: it would already hand every bot
	// HOUSE_OWNER on every bot-owned house (they share one account), ahead of this feature's own
	// check and bypassing botHouseAccessEnable entirely.
	if (g_configManager().getBoolean(HOUSE_OWNED_BY_ACCOUNT)) {
		g_logger().warn("[HOUSE_INDEX] houseOwnedByAccount is TRUE — every bot already has OWNER "
		                "rights on every bot-owned house, regardless of botHouseAccessEnable");
	}
}

bool BotEngine::isBotOwnedHouse(uint32_t houseId) const {
	const auto& house = g_game().map.houses.getHouse(houseId);
	if (!house || house->getOwner() == 0) {
		return false;
	}
	const auto botAcct = static_cast<uint32_t>(g_configManager().getNumber(BOT_HOUSE_ACCOUNT_ID));
	return house->getOwnerAccountId() == botAcct;
}

// ============================================================================
// Claims
// ============================================================================
//
// Three tables, all read-filtered on expiresAt so that an exit path which somehow misses
// endHouseVisit costs a TTL rather than a permanently consumed slot. They are bookkeeping, not
// enforcement: Map::canWalkTo already gives bots FLAG_IGNOREBLOCKCREATURE on any PZ tile, and
// house tiles are PZ, so a bot can still physically arrive on a tile another creature occupies.
// What the claims buy is that two bots never CHOOSE the same spot.

bool BotEngine::isHouseTileClaimed(const Position& tile, uint32_t byGuid) const {
	const auto it = s_houseTileClaims.find(botTileKey(tile));
	return it != s_houseTileClaims.end() && it->second.expiresAt > OTSYS_TIME()
		&& it->second.guid != byGuid;
}

uint32_t BotEngine::houseOccupantCount(uint32_t houseId, uint32_t excludeGuid) const {
	const auto it = s_houseOccupants.find(houseId);
	if (it == s_houseOccupants.end()) {
		return 0;
	}
	const int64_t now = OTSYS_TIME();
	uint32_t n = 0;
	for (const auto& c : it->second) {
		if (c.expiresAt > now && c.guid != excludeGuid) {
			++n;
		}
	}
	return n;
}

void BotEngine::releaseHouseClaims(uint32_t guid) {
	std::erase_if(s_houseTileClaims, [guid](const auto& kv) { return kv.second.guid == guid; });
	for (auto it = s_houseOccupants.begin(); it != s_houseOccupants.end();) {
		std::erase_if(it->second, [guid](const HouseClaim& c) { return c.guid == guid; });
		it = it->second.empty() ? s_houseOccupants.erase(it) : std::next(it);
	}
}

// ============================================================================
// Picking a visit
// ============================================================================

// Offered to the POI roll in bot_poi.cpp. Const because it runs inside selectNextPOI's candidate
// build, which must not commit anything: the claim is taken by the caller only if the candidate
// actually wins the weighted roll.
bool BotEngine::pickHouseVisit(const BotState& bot, HouseRun& out) const {
	// The town the bot is physically standing in, not its home town — bots roam, and visiting a
	// house where you actually are is both achievable and more realistic. Same idiom as the NPC
	// visit's town resolution.
	uint32_t townId = findNearestTown(bot.currentPos);
	auto tit = botHousesByTown_.find(townId);
	if (tit == botHousesByTown_.end()) {
		tit = botHousesByTown_.find(bot.townId);
	}
	if (tit == botHousesByTown_.end() || tit->second.empty()) {
		return false;
	}

	const auto& ids = tit->second;
	const int32_t maxDist = livenessCfg_.houseMaxDist;
	const auto maxOccupants = static_cast<uint32_t>(std::max(1, livenessCfg_.houseMaxOccupants));

	for (int attempt = 0; attempt < HOUSE_SAMPLES; ++attempt) {
		const uint32_t houseId = ids[uniform_random(0, static_cast<int32_t>(ids.size()) - 1)];

		// Ownership can change between index rebuilds (sale, rent eviction, a player claiming a
		// bot house), and the whole grant hangs off it.
		if (!isBotOwnedHouse(houseId)) {
			continue;
		}
		if (houseOccupantCount(houseId, bot.guid) >= maxOccupants) {
			continue;
		}
		const auto hit = houseInteriors_.find(houseId);
		if (hit == houseInteriors_.end()) {
			continue;
		}
		const auto& interior = hit->second;

		// SAME FLOOR: the entry must be on the bot's own floor, and only that floor's contents are
		// ever considered. A house whose door is on another level is simply not offered.
		if (interior.entry.z != bot.currentPos.z) {
			continue;
		}
		if (houseCheb(interior.entry, bot.currentPos) > maxDist) {
			continue;
		}
		const auto fit = interior.floors.find(bot.currentPos.z);
		if (fit == interior.floors.end() || fit->second.idleTiles.empty()) {
			continue;
		}
		const auto& floor = fit->second;

		// Pick the activity first, then the tile it implies — a dummy visit wants to stand beside
		// the dummy, not at a random spot with a dummy elsewhere in the room.
		HouseRun run;
		run.houseId = houseId;
		run.mode = HouseMode::IDLE;

		// BOT_ACTIVITY_PCT TABLE C. Hirelings are resolved LIVE, never cached: they are NPCs
		// the owner can dismiss or move, so a cached position goes stale in a way tiles do not.
		std::vector<std::pair<Position, std::string>> hirelings;
		for (const auto& t : floor.idleTiles) {
			const auto& tile = g_game().map.getTile(t);
			if (!tile) {
				continue;
			}
			if (const auto& npc = tile->getTopVisibleCreature(nullptr); npc && npc->getNpc()) {
				hirelings.emplace_back(t, npc->getName());
			}
		}

		// Shrines are resolved LIVE for a harder reason than hirelings: they CANNOT be cached.
		// floor.*Tiles come from the map sweep, which reads the BasicTile cache holding the OTBM's
		// ORIGINAL furniture -- and every shrine in a bot house was bought from the store and
		// restored at runtime from tile_store, so it has never existed in the OTBM. Extending
		// buildHouseInteriorIndex to harvest shrines would cost a ZCACHE_VERSION bump and return
		// zero rows. (This is the same blind spot that made an OTBM-derived shrine id list miss
		// ~90% of the world's shrines; see kRewardShrineIds in bot_engine_impl.hpp.)
		//
		// So: walk this floor's idle tiles -- which ARE reliable, being ground -- and look at each
		// one's four cardinal neighbours for a shrine item. The idle tile that finds one IS the
		// stand tile, already known standable, and cardinal because internalCreatureTurn is
		// 4-directional and a bot cannot face a shrine diagonally.
		const auto& shrineStands = houseShrineStands(houseId, bot.currentPos.z);

		// Weighted over what this house ACTUALLY HAS, exactly as selectNextPOI rolls over the
		// candidates a town actually has. Most houses have no dummy and no hireling, so the
		// realised split differs from the configured one -- `/cavebot activity` is where that
		// gap is visible. IDLE is always a candidate, so the roll can never come up empty.
		//
		// Replaces a single "do something?" chance followed by a hardcoded UNIFORM pick between
		// the three features, which gave no way to prefer dummy training over locker use.
		std::vector<HouseMode> modes;
		std::vector<int32_t> weights;
		modes.push_back(HouseMode::IDLE);
		weights.push_back(std::max(0, livenessCfg_.houseIdlePct));
		if (!hirelings.empty()) {
			modes.push_back(HouseMode::HIRELING);
			weights.push_back(std::max(0, livenessCfg_.houseHirelingPct));
		}
		if (!floor.dummyTiles.empty()) {
			modes.push_back(HouseMode::DUMMY);
			weights.push_back(std::max(0, livenessCfg_.houseDummyPct));
		}
		if (!floor.lockerTiles.empty()) {
			modes.push_back(HouseMode::LOCKER);
			weights.push_back(std::max(0, livenessCfg_.houseLockerPct));
		}
		if (!shrineStands.empty()) {
			modes.push_back(HouseMode::SHRINE);
			weights.push_back(std::max(0, livenessCfg_.houseShrinePct));
		}

		int32_t wTotal = 0;
		for (int32_t w : weights) wTotal += w;
		HouseMode want = HouseMode::IDLE;
		if (wTotal > 0) {
			int32_t r = uniform_random(1, wTotal);
			for (size_t mi = 0; mi < modes.size(); ++mi) {
				r -= weights[mi];
				if (r <= 0) { want = modes[mi]; break; }
			}
		}

		if (want == HouseMode::SHRINE) {
			// Discovery already paired every hit with its stand tile, so unlike DUMMY/LOCKER there
			// is no adjacency search to redo -- only the claim check, from a random start so two
			// bots in a house with several shrines do not converge on the same one.
			const int32_t n = static_cast<int32_t>(shrineStands.size());
			const int32_t start = uniform_random(0, n - 1);
			for (int32_t i = 0; i < n; ++i) {
				const auto& cand = shrineStands[(start + i) % n];
				if (isHouseTileClaimed(cand.stand, bot.guid)) continue;
				run.mode = HouseMode::SHRINE;
				run.feature = cand.shrine;
				run.tile = cand.stand;
				break;
			}
		} else if (want == HouseMode::HIRELING) {
			const auto& h = hirelings[uniform_random(0, static_cast<int32_t>(hirelings.size()) - 1)];
			run.mode = HouseMode::HIRELING;
			run.feature = h.first;
			run.hirelingName = h.second;
		} else if (want == HouseMode::DUMMY || want == HouseMode::LOCKER) {
			// Try EVERY piece of that type, starting from a random one, and keep the first with a
			// free side. Picking one at random and giving up if it happened to be boxed in
			// discarded the whole house -- and with it any other locker or dummy standing free two
			// tiles away. A failure here simply leaves run.mode == IDLE, which is a legitimate
			// outcome rather than a lost visit.
			const auto& src = (want == HouseMode::DUMMY) ? floor.dummyTiles : floor.lockerTiles;
			const int32_t n = static_cast<int32_t>(src.size());
			const int32_t start = uniform_random(0, n - 1);
			for (int32_t i = 0; i < n; ++i) {
				const Position& cand = src[(start + i) % n];
				if (want == HouseMode::DUMMY && isHouseTileClaimed(cand, bot.guid)) {
					continue;
				}
				if (houseStandsAround(cand, houseId).empty()) {
					continue;
				}
				run.mode = want;
				run.feature = cand;
				break;
			}
		}

		// Where the bot actually stands. For a feature, the nearest free idle tile beside it — a
		// dummy or a locker is used from an adjacent square, and a hireling is greeted from talk
		// range. For plain idle, any free tile in the room.
		if (run.mode == HouseMode::IDLE) {
			std::vector<Position> free;
			for (const auto& t : floor.idleTiles) {
				if (!isHouseTileClaimed(t, bot.guid) && houseTileStandable(t, houseId)) {
					free.push_back(t);
				}
			}
			if (free.empty()) {
				continue;
			}
			run.tile = free[uniform_random(0, static_cast<int32_t>(free.size()) - 1)];
		} else if (run.mode == HouseMode::SHRINE) {
			// run.tile was already fixed by discovery and must NOT go through the branch below:
			// houseStandsAround returns all EIGHT sides, and a diagonal tile cannot face a shrine
			// at all (internalCreatureTurn is 4-directional). Only re-check standability, which is
			// a live fact the harvested idle list can be stale about.
			if (!houseTileStandable(run.tile, houseId, /*allowAvoid=*/true)) {
				continue;
			}
		} else {
			// The dummy itself is claimed too, so two visitors cannot both roll DUMMY against one
			// dummy and have the second silently fail Game's hasActor() check.
			if (run.mode == HouseMode::DUMMY && isHouseTileClaimed(run.feature, bot.guid)) {
				continue;
			}
			// All eight sides, live. Which side of a locker or dummy is open varies per house, and
			// the harvested idle list — a cached approximation — was dropping perfectly good ones.
			std::vector<Position> adjacent;
			for (const auto& t : houseStandsAround(run.feature, houseId)) {
				if (!isHouseTileClaimed(t, bot.guid)) {
					adjacent.push_back(t);
				}
			}
			if (adjacent.empty()) {
				continue; // genuinely boxed in, or every side taken — try another house
			}
			run.tile = adjacent[uniform_random(0, static_cast<int32_t>(adjacent.size()) - 1)];
		}

		out = run;
		return true;
	}
	return false;
}

// Commit the claims the pick chose. Split from pickHouseVisit so the candidate can be OFFERED to
// the weighted roll without reserving anything — the NPC visit's offer/commit/release discipline,
// which exists because a candidate that loses the roll must not leave a reservation behind.
void BotEngine::claimHouseVisit(uint32_t guid, const HouseRun& run) {
	const int64_t expiry = OTSYS_TIME() + HOUSE_CLAIM_MS;
	s_houseTileClaims[botTileKey(run.tile)] = HouseClaim { guid, expiry };
	if (run.mode == HouseMode::DUMMY) {
		s_houseTileClaims[botTileKey(run.feature)] = HouseClaim { guid, expiry };
	}
	s_houseOccupants[run.houseId].push_back(HouseClaim { guid, expiry });
	s_houseRuns[guid] = run;
}

// ============================================================================
// The run
// ============================================================================

bool BotEngine::isInsideRunHouse(const BotState& bot) const {
	const auto it = s_houseRuns.find(bot.guid);
	if (it == s_houseRuns.end()) {
		return false;
	}
	const auto& tile = g_game().map.getTile(bot.currentPos);
	if (!tile) {
		return false;
	}
	const auto& house = tile->getHouse();
	return house && house->getId() == it->second.houseId;
}

// Arrival test, shared by the POI-driven visit and the forced `/cavebot house`.
//
// EXACT tile, not POI_ARRIVAL_DIST. The generic arrival tolerates three tiles, which for a house
// means "arrived" can mean standing in the street outside the front door, or three tiles from the
// dummy the bot came to use — and whichever block consumes the walk clears hasWalkTarget, so the
// bot then stands wherever it stopped for the whole idle window. Observed exactly that live before
// this existed: the bot halted two tiles out, the goto-arrival branch declared success, and the
// visit evaporated with its claims still held.
//
// Bounded by botHouseSettleSec, armed the first tick the bot is actually INSIDE the house — not at
// walk start, because the walk in is already bounded by the planner's own stale-target budget.
bool BotEngine::tryHouseArrival(BotState& bot) {
	auto it = s_houseRuns.find(bot.guid);
	if (it == s_houseRuns.end() || it->second.phase != HousePhase::APPROACH) {
		return false;
	}
	auto& run = it->second;
	const bool inside = isInsideRunHouse(bot);

	if (!inside) {
		// Still on the way. Only meaningful once the deadline has been armed, which only happens
		// inside — so before the bot ever gets in, this simply lets the walk continue.
		if (run.settleUntil != 0 && OTSYS_TIME() >= run.settleUntil) {
			endHouseVisit(bot.guid, "settle_timeout");
			bot.hasWalkTarget = false;
			bot.currentPOI = nullptr;
			clearPlannerWalk(bot.guid);
			return true;
		}
		return false;
	}

	if (run.settleUntil == 0) {
		run.settleUntil = OTSYS_TIME() + std::max(1, livenessCfg_.houseSettleSec) * 1000LL;
	}
	const bool onTile = bot.currentPos == run.tile;
	if (!onTile && OTSYS_TIME() < run.settleUntil) {
		return false; // keep closing the last few tiles; hasWalkTarget stays set
	}
	if (!onTile) {
		// Could not close the gap in time. Settle here and drop the sub-activity, which was chosen
		// for a tile the bot never reached — greeting a hireling from across the room, or swinging
		// at a dummy that is not adjacent, would both be wrong.
		castLog(bot, fmt::format("HOUSE: settle deadline at ({},{},{}) — idling here",
			bot.currentPos.x, bot.currentPos.y, bot.currentPos.z));
		run.mode = HouseMode::IDLE;
		run.tile = bot.currentPos;
	}

	startHouseVisit(bot);
	bot.hasWalkTarget = false;
	bot.pendingNavDest.clear();
	bot.currentPOI = nullptr;
	bot.pathFailCount = 0;
	bot.consecutivePOIFails = 0;
	clearPlannerWalk(bot.guid);
	return true;
}

// Begins the idle window, once the bot is standing where it meant to stand.
void BotEngine::startHouseVisit(BotState& bot) {
	auto it = s_houseRuns.find(bot.guid);
	if (it == s_houseRuns.end()) {
		return;
	}
	auto& run = it->second;
	auto player = bot.getPlayer();
	if (!player) {
		endHouseVisit(bot.guid, "no_player");
		return;
	}

	run.phase = HousePhase::IDLE;
	const int32_t secs = uniform_random(std::max(1, livenessCfg_.houseIdleMinSec),
	                                    std::max(livenessCfg_.houseIdleMinSec, livenessCfg_.houseIdleMaxSec));
	run.until = OTSYS_TIME() + secs * 1000LL;

	// Hold dwellUntil past the session for the same reason startFishingRun does: doDwelling's tail
	// is the sole authority for leaving DWELLING and knows nothing about this run, so an ordinary
	// POI dwell would expire mid-visit and reroll the bot away from its own trip. endHouseVisit
	// puts it back.
	bot.dwellUntil = run.until + 5000;

	const char* modeName = "idle";
	switch (run.mode) {
		case HouseMode::HIRELING: {
			// Greet like a player who just walked in. The NPC_INTERACT waypoint type does the same
			// thing for routes; here the bot is already standing in talk range, so the greeting is
			// all that is left of it.
			static const char* kGreetings[] = { "hi", "hello", "hey", "heya" };
			const char* greet = kGreetings[uniform_random(0, 3)];
			g_game().internalCreatureSay(player, TALKTYPE_SAY, greet, false);
			modeName = "hireling";
			break;
		}
		case HouseMode::DUMMY: {
			modeName = "dummy";
			// Same kickoff as the Adventurer's-Stone dummy, against this house's own dummy. The
			// weapon must be a real item in the bag (the Lua action checks inventory), and the
			// dummy's stackpos must be resolved or STACKPOS_USETARGET picks the ground and the
			// action silently no-ops — bot stands there, no swing, no training.
			run.trainWeaponId = kLastingExerciseIds[uniform_random(0,
				static_cast<int32_t>(std::size(kLastingExerciseIds)) - 1)];
			auto weapon = g_game().findItemOfType(player, run.trainWeaponId, true, -1);
			std::shared_ptr<Item> dummyItem;
			uint8_t stackPos = 0;
			if (const auto& dtile = g_game().map.getTile(run.feature)) {
				if (auto items = dtile->getItemList()) {
					for (const auto& i : *items) {
						if (i && i->isDummy()) { dummyItem = i; break; }
					}
				}
				if (dummyItem) {
					const int32_t sp = dtile->getThingIndex(dummyItem);
					if (sp >= 0 && sp <= 255) stackPos = static_cast<uint8_t>(sp);
				}
			}
			if (weapon && dummyItem) {
				run.trainingActive = g_actions().useItemEx(player, weapon->getPosition(),
				                                           run.feature, stackPos, weapon, false);
			}
			g_logger().info("[HOUSE_VISIT_ARRIVE] guid={} house={} mode=dummy idle={}s train={}",
			                bot.guid, run.houseId, secs, run.trainingActive ? "OK" : "FAILED");
			bot.state = BotAIState::DWELLING;
			return;
		}
		case HouseMode::LOCKER: modeName = "locker"; break;
		case HouseMode::SHRINE: {
			// The whole point of the mode: face the shrine. run.tile is a CARDINAL neighbour of
			// run.feature by construction (pickHouseVisit's discovery pairs them that way and the
			// stand-tile branch is skipped for SHRINE), so a 4-directional turn can always look
			// at it. Nothing is USED — both shrine actions are client-only.
			modeName = "shrine";
			if (auto p = bot.getPlayer()) {
				g_game().internalCreatureTurn(p, shrineFacingDir(run.tile, run.feature));
			}
			break;
		}
		case HouseMode::IDLE:   modeName = "idle"; break;
	}

	g_logger().info("[HOUSE_VISIT_ARRIVE] guid={} house={} mode={} idle={}s",
	                bot.guid, run.houseId, modeName, secs);
	bot.state = BotAIState::DWELLING;
}

// Drives an active visit from doDwelling. Returns true while it owns the tick.
//
// Placed in the SAME early slot tickFishingRun occupies, ahead of the dwell-walk-target,
// locker-reroll and hasDepotTarget blocks — any of which would otherwise starve it for as long as
// that state takes to resolve, and since dwellUntil is held open a starved run would never reach
// the tail that ends it either.
bool BotEngine::tickHouseVisit(BotState& bot) {
	auto it = s_houseRuns.find(bot.guid);
	if (it == s_houseRuns.end()) {
		return false;
	}
	auto& run = it->second;

	// Displacement. Covers House::tryTransferOwnership's kickPlayer (rent eviction, auction, the
	// /house admin command), a GM teleport, and /cavebot teleport — none of which know anything
	// about this run. Only meaningful once the bot actually got inside; before that the walk is
	// still in progress and being outside is the normal state.
	if (run.phase == HousePhase::IDLE && !isInsideRunHouse(bot)) {
		endHouseVisit(bot.guid, "displaced");
		return false;
	}

	if (run.phase == HousePhase::APPROACH) {
		return false; // the walk is driven by the POI machinery until arrival
	}

	if (OTSYS_TIME() >= run.until) {
		endHouseVisit(bot.guid, "dwell_end");
		return false; // let doDwelling's tail reroll normally
	}
	return true; // idling: this run owns the tick
}

void BotEngine::stopHouseTrainingIfActive(BotState& bot) {
	const auto it = s_houseRuns.find(bot.guid);
	if (it == s_houseRuns.end() || !it->second.trainingActive) {
		return;
	}
	// The Lua exerciseTrainingEvent loop lives in the MAIN BINARY's Lua state and survives
	// dlclose/dlopen, so it has to be told to stop while the Player is still in-world — the same
	// reason stopAdvStoneTrainingIfActive exists and is called from the reload teardown.
	if (auto player = bot.getPlayer()) {
		player->setTraining(false);
	}
	it->second.trainingActive = false;
}

void BotEngine::endHouseVisit(uint32_t guid, const char* reason) {
	const auto it = s_houseRuns.find(guid);
	if (it == s_houseRuns.end()) {
		return; // idempotent — every exit path may call this
	}
	const uint32_t houseId = it->second.houseId;

	if (it->second.trainingActive) {
		if (const auto idx = guidToIndex_.find(guid); idx != guidToIndex_.end()) {
			stopHouseTrainingIfActive(bots_[idx->second]);
		}
	}
	releaseHouseClaims(guid);
	s_houseRuns.erase(guid);

	// Hand the walk OUT to the scoped route planner, exactly once. Leaving needs the same door
	// handling that got the bot in, and the generic walker has none. Consumed by the next
	// doActivityReroll whatever it rolls, so it cannot survive unrelated dwell/hunt cycles.
	s_houseExitPlanner.insert(guid);

	// Put dwellUntil back to something ordinary — startHouseVisit stretched it well past a normal
	// dwell, and virtualAdvanceDwelling is a pure timer that knows nothing about this run.
	if (const auto idx = guidToIndex_.find(guid); idx != guidToIndex_.end()) {
		auto& bot = bots_[idx->second];
		if (bot.dwellUntil > OTSYS_TIME() + 60000) {
			bot.dwellUntil = OTSYS_TIME() + uniform_random(3, 8) * 1000LL;
		}
	}

	g_logger().info("[HOUSE_VISIT_END] guid={} house={} reason={}", guid, houseId, reason);
}

// ============================================================================
// Debug commands
// ============================================================================

std::string BotEngine::forceHouseVisit(BotState& bot, const std::string& arg) {
	if (bot.hibernated) {
		return "House visits are awake-only; this bot is hibernated.";
	}
	// Same reason as forceShrineVisit's guard: a stopped bot skips the AI state machine entirely,
	// so it would accept this command, print a destination and then never move.
	if (bot.aiPaused) {
		return "bot is STOPPED (aiPaused) — it cannot walk anywhere until `resume`.";
	}
	endHouseVisit(bot.guid, "forced_restart");

	// "<houseId> [idle|dummy|locker|hireling]" — the mode word is what makes a sub-activity
	// testable at all. Without it the dummy path is only reachable by chance, which is not a test;
	// `advstone dummy` exists for the same reason.
	std::string idArg = arg;
	std::string modeArg;
	if (const size_t sp = arg.find(' '); sp != std::string::npos) {
		idArg = arg.substr(0, sp);
		modeArg = arg.substr(sp + 1);
	}
	// `house dummy` with no id: the mode word alone. Nobody wants to look a house id up first just
	// to watch a bot train, and `/cavebot fish` sets the expectation that one word is enough. A
	// non-numeric first token is therefore a MODE, and the house is chosen by searching the bot's
	// town for one that actually has that feature on the bot's floor.
	if (!idArg.empty() && std::isdigit(static_cast<unsigned char>(idArg[0])) == 0) {
		// Re-join, do not overwrite: `house shrine reward` splits to idArg="shrine",
		// modeArg="reward", and assigning modeArg = idArg would throw the kind away and silently
		// send the bot to whichever shrine the house listed first.
		modeArg = modeArg.empty() ? idArg : idArg + " " + modeArg;
		idArg.clear();
	}

	// "shrine", "shrine reward", "shrine imbuing". 0 = either kind will do.
	uint8_t wantKind = 0;
	if (modeArg.rfind("shrine", 0) == 0) {
		const size_t ksp = modeArg.find(' ');
		if (ksp != std::string::npos) {
			const std::string k = modeArg.substr(ksp + 1);
			if (k == "reward") wantKind = SHRINE_KIND_REWARD;
			else if (k == "imbuing" || k == "imbue") wantKind = SHRINE_KIND_IMBUING;
		}
	}

	// Mode-only: find a house in this town that HAS the thing, rather than rolling until one turns
	// up. Same eligibility rules the real pick applies (bot-owned, in reach, entry on this floor,
	// not full), so what it exercises is the production path with the randomness removed.
	if (idArg.empty() && !modeArg.empty() && modeArg != "idle") {
		uint32_t townId = findNearestTown(bot.currentPos);
		auto tit = botHousesByTown_.find(townId);
		if (tit == botHousesByTown_.end()) {
			tit = botHousesByTown_.find(bot.townId);
		}
		if (tit == botHousesByTown_.end()) {
			return "No bot-owned houses in this town.";
		}
		const int32_t maxDist = livenessCfg_.houseMaxDist;
		uint32_t best = 0;
		int32_t bestDist = INT32_MAX;
		for (const uint32_t hid : tit->second) {
			const auto hit2 = houseInteriors_.find(hid);
			if (hit2 == houseInteriors_.end() || hit2->second.entry.z != bot.currentPos.z) {
				continue;
			}
			const int32_t d = houseCheb(hit2->second.entry, bot.currentPos);
			if (d > maxDist || d >= bestDist || !isBotOwnedHouse(hid)) {
				continue;
			}
			const auto fit2 = hit2->second.floors.find(bot.currentPos.z);
			if (fit2 == hit2->second.floors.end()) {
				continue;
			}
			bool has = false;
			if (modeArg == "dummy") {
				has = !fit2->second.dummyTiles.empty();
			} else if (modeArg == "locker") {
				has = !fit2->second.lockerTiles.empty();
			} else if (modeArg.rfind("shrine", 0) == 0) {
				for (const auto& c : houseShrineStands(hid, bot.currentPos.z)) {
					if (!wantKind || c.kind == wantKind) { has = true; break; }
				}
			} else if (modeArg == "hireling") {
				for (const auto& t : fit2->second.idleTiles) {
					const auto& tile = g_game().map.getTile(t);
					if (!tile) {
						continue;
					}
					if (const auto& c = tile->getTopVisibleCreature(nullptr); c && c->getNpc()) {
						has = true;
						break;
					}
				}
			} else {
				return fmt::format("Unknown mode '{}'. Valid: idle, dummy, locker, hireling.", modeArg);
			}
			if (has) {
				best = hid;
				bestDist = d;
			}
		}
		if (best == 0) {
			return fmt::format("No bot-owned house within {} tiles on z={} has a {}.",
			                   maxDist, bot.currentPos.z, modeArg);
		}
		idArg = std::to_string(best);
	}

	HouseRun run;
	if (!idArg.empty()) {
		// Explicit house id: skip the sampler entirely so a specific interior can be exercised.
		const uint32_t houseId = static_cast<uint32_t>(std::atoi(idArg.c_str()));
		const auto hit = houseInteriors_.find(houseId);
		if (hit == houseInteriors_.end()) {
			return fmt::format("House {} is not in the interior index.", houseId);
		}
		const auto fit = hit->second.floors.find(bot.currentPos.z);
		if (fit == hit->second.floors.end() || fit->second.idleTiles.empty()) {
			return fmt::format("House {} has no indexed tiles on z={} (bot's floor).",
			                   houseId, bot.currentPos.z);
		}
		run.houseId = houseId;
		run.mode = HouseMode::IDLE;
		std::vector<Position> free;
		for (const auto& t : fit->second.idleTiles) {
			if (houseTileStandable(t, houseId)) {
				free.push_back(t);
			}
		}
		if (free.empty()) {
			return fmt::format("House {} has {} indexed tiles on z={} but none is standable now "
			                   "(furniture restored from tile_store is invisible to the cached harvest).",
			                   houseId, fit->second.idleTiles.size(), bot.currentPos.z);
		}
		run.tile = free[uniform_random(0, static_cast<int32_t>(free.size()) - 1)];

		// Optional mode word. Resolve the feature from THIS floor's contents and re-aim the stand
		// tile at something adjacent to it, mirroring what pickHouseVisit does for a rolled mode.
		if (!modeArg.empty() && modeArg != "idle") {
			const auto& fl = fit->second;
			Position feature;
			bool found = false;
			// Walk EVERY piece of the requested type and keep the first with a free side.
			// Taking [0] meant one boxed-in locker failed the whole command even when the house
			// had another standing clear.
			int32_t tried = 0;
			if (modeArg == "dummy" || modeArg == "locker") {
				const auto& src = (modeArg == "dummy") ? fl.dummyTiles : fl.lockerTiles;
				tried = static_cast<int32_t>(src.size());
				for (const auto& cand : src) {
					if (houseStandsAround(cand, houseId).empty()) {
						continue;
					}
					feature = cand;
					run.mode = (modeArg == "dummy") ? HouseMode::DUMMY : HouseMode::LOCKER;
					found = true;
					break;
				}
				if (!found && tried > 0) {
					return fmt::format("House {}: all {} {}(s) on z={} are boxed in — no free tile on "
					                   "any of the eight sides.", houseId, tried, modeArg, bot.currentPos.z);
				}
			} else if (modeArg.rfind("shrine", 0) == 0) {
				// Discovery already paired each shrine with its cardinal stand tile, so unlike
				// dummy/locker there is no eight-side search to redo.
				//
				// wantKind matters: `shrine reward` delegated from the shrine command must reach
				// the REWARD shrine. Without it this took whichever shrine the scan listed first,
				// so asking for reward in a house holding both parked the bot at the imbuing one —
				// caught by physically running the command rather than by reading the code.
				const auto& stands = houseShrineStands(houseId, bot.currentPos.z);
				tried = static_cast<int32_t>(stands.size());
				for (const auto& c : stands) {
					if (wantKind && c.kind != wantKind) continue;
					if (!houseTileStandable(c.stand, houseId, /*allowAvoid=*/true)) continue;
					feature = c.shrine;
					run.tile = c.stand;
					run.mode = HouseMode::SHRINE;
					found = true;
					break;
				}
				if (!found && tried > 0) {
					return fmt::format("House {}: no reachable {}shrine on z={} ({} found, all "
					                   "blocked or of the other kind).", houseId,
					                   wantKind == SHRINE_KIND_REWARD ? "reward " :
					                   wantKind == SHRINE_KIND_IMBUING ? "imbuing " : "",
					                   bot.currentPos.z, tried);
				}
			} else if (modeArg == "hireling") {
				for (const auto& t : fl.idleTiles) {
					const auto& tile = g_game().map.getTile(t);
					if (!tile) {
						continue;
					}
					if (const auto& c = tile->getTopVisibleCreature(nullptr); c && c->getNpc()) {
						feature = t;
						run.hirelingName = c->getName();
						run.mode = HouseMode::HIRELING;
						found = true;
						break;
					}
				}
			}
			if (!found) {
				return fmt::format("House {} has no '{}' on z={}.", houseId, modeArg, bot.currentPos.z);
			}
			run.feature = feature;
			// SHRINE already fixed run.tile to a CARDINAL neighbour during discovery and must skip
			// the eight-side pick below: houseStandsAround includes diagonals, and a bot on a
			// diagonal tile cannot face the shrine at all (internalCreatureTurn is 4-directional).
			// Same carve-out as the one in pickHouseVisit, for the same reason.
			if (run.mode != HouseMode::SHRINE) {
				// All eight sides, live — see houseStandsAround.
				std::vector<Position> adj = houseStandsAround(feature, houseId);
				if (adj.empty()) {
					return fmt::format("House {}: nothing standable on any side of the {} at ({},{},{}).",
					                   houseId, modeArg, feature.x, feature.y, feature.z);
				}
				run.tile = adj[uniform_random(0, static_cast<int32_t>(adj.size()) - 1)];
			}
		}
	} else if (!pickHouseVisit(bot, run)) {
		return "No eligible bot-owned house on this floor within reach.";
	}

	claimHouseVisit(bot.guid, run);
	bot.walkTarget = run.tile;
	bot.hasWalkTarget = true;
	bot.currentPOI = nullptr;
	bot.pathFailCount = 0;
	bot.followingCityRoute = false;
	s_plannerWalk[bot.guid] = bot.walkTarget;
	bot.state = BotAIState::IDLE;

	const char* modeName = run.mode == HouseMode::HIRELING ? "hireling"
		: run.mode == HouseMode::DUMMY ? "dummy"
		: run.mode == HouseMode::LOCKER ? "locker"
		: run.mode == HouseMode::SHRINE ? "shrine" : "idle";
	g_logger().info("[HOUSE_VISIT_START] guid={} house={} mode={} tile=({},{},{}) from=({},{},{}) forced",
	                bot.guid, run.houseId, modeName, run.tile.x, run.tile.y, run.tile.z,
	                bot.currentPos.x, bot.currentPos.y, bot.currentPos.z);
	return fmt::format("House visit: house={} mode={} -> ({},{},{}), dist={}",
	                   run.houseId, modeName, run.tile.x, run.tile.y, run.tile.z,
	                   houseCheb(run.tile, bot.currentPos));
}

// Dumps what the harvest actually found, including every door's id and whether the engine
// classifies it key-locked. That last column is the point: the planner treats a key-locked door as
// impassable because a bot carries no keys, so if a house's real entrance uses one of those ids
// this is where it shows up — before a bot is sent to stand in front of it.
std::string BotEngine::describeHouseInterior(const BotState& bot, const std::string& arg) const {
	uint32_t houseId = 0;
	if (!arg.empty() && arg != "near") {
		houseId = static_cast<uint32_t>(std::atoi(arg.c_str()));
	} else {
		// Nearest indexed bot house to the bot, on any floor.
		int32_t best = INT32_MAX;
		for (const auto& [id, interior] : houseInteriors_) {
			const int32_t d = houseCheb(interior.entry, bot.currentPos);
			if (d < best) {
				best = d;
				houseId = id;
			}
		}
		if (houseId == 0) {
			return "House interior index is empty.";
		}
	}

	const auto hit = houseInteriors_.find(houseId);
	if (hit == houseInteriors_.end()) {
		return fmt::format("House {} is not in the interior index.", houseId);
	}
	const auto& interior = hit->second;
	const auto& house = g_game().map.houses.getHouse(houseId);

	std::string out = fmt::format(
		"House {} '{}' town={} owner={} botOwned={} entry=({},{},{}) dist={}\n",
		houseId, house ? house->getName() : "?", interior.townId,
		house ? house->getOwner() : 0, isBotOwnedHouse(houseId) ? "yes" : "NO",
		interior.entry.x, interior.entry.y, interior.entry.z,
		houseCheb(interior.entry, bot.currentPos));

	for (const auto& [z, floor] : interior.floors) {
		out += fmt::format("  z={}{}: idle={} dummies={} lockers={}\n",
		                   z, z == bot.currentPos.z ? " (bot's floor)" : "",
		                   floor.idleTiles.size(), floor.dummyTiles.size(), floor.lockerTiles.size());
		auto describeSides = [&](const char* what, const Position& f) {
			out += fmt::format("    {} ({},{},{}) stands={}\n", what, f.x, f.y, f.z,
			                   houseStandsAround(f, houseId).size());
			for (int32_t ddx = -1; ddx <= 1; ++ddx) {
				for (int32_t ddy = -1; ddy <= 1; ++ddy) {
					if (ddx == 0 && ddy == 0) {
						continue;
					}
					const Position p(static_cast<uint16_t>(static_cast<int32_t>(f.x) + ddx),
					                 static_cast<uint16_t>(static_cast<int32_t>(f.y) + ddy), f.z);
					out += fmt::format("      ({},{}) {}\n", p.x, p.y, houseStandFailReason(p, houseId));
				}
			}
		};
		for (const auto& d : floor.dummyTiles) {
			describeSides("dummy", d);
		}
		for (const auto& l : floor.lockerTiles) {
			describeSides("locker", l);
		}
	}

	// Doors come from the live House object, not the index — House::getDoors is already
	// maintained by the map, and the id + locked classification is the whole reason to look.
	if (house) {
		for (const auto& door : house->getDoors()) {
			if (!door) {
				continue;
			}
			const uint16_t id = door->getID();
			const auto pos = door->getPosition();
			out += fmt::format("    door id={} at ({},{},{}) keyLocked={}\n",
			                   id, pos.x, pos.y, pos.z,
			                   isKeyLockedDoorId(id) ? "YES (planner treats as impassable)" : "no");
		}
	}

	// Live hirelings on the bot's floor — never cached, since the owner can dismiss or move them.
	const auto fit = interior.floors.find(bot.currentPos.z);
	if (fit != interior.floors.end()) {
		for (const auto& t : fit->second.idleTiles) {
			const auto& tile = g_game().map.getTile(t);
			if (!tile) {
				continue;
			}
			if (const auto& c = tile->getTopVisibleCreature(nullptr); c && c->getNpc()) {
				out += fmt::format("    hireling '{}' at ({},{},{})\n", c->getName(), t.x, t.y, t.z);
			}
		}
	}

	out += fmt::format("  occupants={}/{}\n", houseOccupantCount(houseId, 0),
	                   livenessCfg_.houseMaxOccupants);
	return out;
}
