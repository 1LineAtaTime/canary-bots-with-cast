/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_zgraph.cpp — TRUE MULTI-FLOOR pathfinding: whole-map portal graph +
// z-route planner (engine side of bot_zcore.hpp).
//
// New Phase-12 module in the SAME libbot_engine.so (hot-reload unchanged).
//
// The graph is built once per engine load (loadHuntData) by sweeping the map's
// SECTOR grid and classifying every column WITHOUT materializing tiles:
//   - already-materialized tiles are read via Floor::getTile (exact TILESTATE
//     flags + live items),
//   - everything else is read via Floor::getTileCache (BasicTile) and
//     classified through ItemType (floorChange flags, ladder/rope/sewer/shovel
//     ids, teleport destinations stored in BasicItem.destX/Y/Z).
// This matters: Map::getTile() materializes tiles from the BasicTile cache
// (mapcache.cpp getOrCreateTileFromCache) — a naive whole-map getTile sweep
// would inflate RAM by hundreds of MB. The sector walk touches no caches.
//
// Landing positions replicate the REAL mechanics:
//   - flag transitions: Tile::queryDestination (tile.cpp:932) replica,
//     including the SOUTH_ALT/EAST_ALT offset-tile checks that the old (dead)
//     computeFloorChangeDest got wrong,
//   - ladders/rope spots: Position:moveUpstairs() (z-1, prefer SOUTH, else
//     scan neighbours),
//   - sewers/shovel holes: z+1 with the same snap,
//   - teleports: the item's explicit destination.
// Every landing is validated walkable (live-or-cache) before the portal is
// admitted; planning is optimistic about LEGS (Chebyshev) and the runtime FC
// state machine validates them — failures blacklist the portal for
// Z_BLACKLIST_MS and the next goTo() call replans around it.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

#include "game/movement/teleport.hpp" // Teleport::getDestPos (live-tile teleport portals)
#include "items/items.hpp"

#include "map/house/house.hpp" // House::getId — route planning refuses other people's houses

#include <algorithm>
#include <random> // std::mt19937 / std::random_device — the pre-cap shuffle in buildFishingSpotIndex
#include <unordered_set>

namespace {

// ---- raw cell access: NEVER materializes (no Map::getTile) ----
struct ZCell {
	std::shared_ptr<Tile> tile; // materialized tile (preferred — exact)
	std::shared_ptr<BasicTile> cached; // basic-tile cache entry
	bool exists() const {
		return tile != nullptr || cached != nullptr;
	}
};

ZCell zCellAt(int32_t x, int32_t y, int32_t z) {
	ZCell c;
	if (x < 0 || y < 0 || x > 0xFFFF || y > 0xFFFF || z < 0 || z >= static_cast<int32_t>(MAP_MAX_LAYERS)) {
		return c;
	}
	MapSector* sector = g_game().map.getMapSector(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	if (!sector) {
		return c;
	}
	const auto& floor = sector->getFloor(static_cast<uint8_t>(z));
	if (!floor) {
		return c;
	}
	c.tile = floor->getTile(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
	c.cached = floor->getTileCache(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
	return c;
}

// OR of ItemType floorChange flags for a cached (unmaterialized) tile — this is
// exactly what Tile would carry after internalAddThing set its flags.
uint32_t cachedFcFlags(const std::shared_ptr<BasicTile>& bt) {
	uint32_t flags = 0;
	if (bt->ground) {
		flags |= Item::items[bt->ground->id].floorChange;
	}
	for (const auto& it : bt->items) {
		flags |= Item::items[it->id].floorChange;
	}
	return flags;
}

// TILESTATE floorchange flags of a cell (live tile preferred).
uint32_t zFcFlagsAt(int32_t x, int32_t y, int32_t z) {
	ZCell c = zCellAt(x, y, z);
	if (c.tile) {
		uint32_t flags = 0;
		static const TileFlags_t fcBits[] = {
			TILESTATE_FLOORCHANGE_DOWN, TILESTATE_FLOORCHANGE_NORTH, TILESTATE_FLOORCHANGE_SOUTH,
			TILESTATE_FLOORCHANGE_EAST, TILESTATE_FLOORCHANGE_WEST, TILESTATE_FLOORCHANGE_SOUTH_ALT,
			TILESTATE_FLOORCHANGE_EAST_ALT
		};
		for (auto b : fcBits) {
			if (c.tile->hasFlag(b)) {
				flags |= b;
			}
		}
		return flags;
	}
	if (c.cached) {
		return cachedFcFlags(c.cached);
	}
	return 0;
}

// ---- BOT_SUPPLY_REALISM: fishable water ----
//
// The ids `data-otservbr-global/scripts/actions/other/fishing.lua` accepts, restricted to the
// plain sea/shallow set. The specials it also accepts (7236 ice hole, 9582 corpse, 13988
// sandfish, 12560/12561/12563, 21414 Dawnport) are deliberately EXCLUDED: they transform,
// decay, or belong to a quest, and on the plain ids the script's only reward is fish 3578,
// which keeps a bot's catch to a single stackable item type.
//
// Verified against the live map by parsing all 17.8M tiles of otservbr.otbm: 4597-4602 are
// 1.49M tiles, 4609-4614 0.50M, 629-634 19.6k, and every one of the 30 towns has shoreline
// within 50 tiles of its temple.
bool isFishableWaterId(uint16_t id) {
	return (id >= 4597 && id <= 4602)
		|| (id >= 4609 && id <= 4614)
		|| (id >= 629 && id <= 634);
}

// Ground id of a cell without materializing it (live tile preferred, else the BasicTile cache).
uint16_t zGroundIdAt(const ZCell& c) {
	if (c.tile) {
		const auto& g = c.tile->getGround();
		return g ? g->getID() : 0;
	}
	if (c.cached && c.cached->ground) {
		return c.cached->ground->id;
	}
	return 0;
}

// ---- BOT_SUPPLY_REALISM: "clear" water — the ONE predicate for castable water ----
//
// Ground IS the water for every id in isFishableWaterId, so "nothing on top or on the bottom"
// (the fishing feature's own requirement) reduces to one check: the tile's item vector is empty.
// A ship (items.xml fromid="4881" toid="4903") sits ON TOP of ordinary water ground exactly like
// any other item, so this rejects it the same way it would reject a crate or a railing — no
// special-casing of ship ids needed. This is the SINGLE source of truth for both the live scan
// (findNearbyFishingSpot, below) and the prebuilt index's harvest filter (buildZPortalGraph):
// before this fix only the live scan enforced it, so the disk-cached index — built from the raw
// isFishableWaterId(ground) test — kept handing out ship tiles verbatim on every boot. See
// ZCACHE_VERSION's v9 note.
bool zCellIsClearWater(const ZCell& c) {
	if (!c.exists() || !isFishableWaterId(zGroundIdAt(c))) {
		return false;
	}
	if (c.tile) {
		const TileItemVector* items = c.tile->getItemList();
		return !items || items->empty();
	}
	return c.cached && c.cached->items.empty();
}

bool zCellIsClearWaterAt(int32_t x, int32_t y, int32_t z) {
	return zCellIsClearWater(zCellAt(x, y, z));
}

// Actions::canUseFar's own box (actions.cpp:211 — Position::areInRange<7,5>, with checkFloor and
// checkLineOfSight both true by Action's defaults, which fishing.lua never overrides). Matches
// tickFishingRun's own re-validation (bot_supply.cpp) exactly, on purpose: the water tile this
// file hands out and the tile the live cast re-checks before every use must agree on the box, or
// one of them is wrong.
constexpr int32_t FISH_CAST_RANGE_X = 7;
constexpr int32_t FISH_CAST_RANGE_Y = 5;

// Hard ceiling on materializing isSightClear probes across one findNearbyFishingSpot call. Every
// clear-water hit in a stand candidate's throw box costs one Map::getTile-backed sight check
// (checkSightLine, map.cpp) — the only call in this whole feature that actually materializes
// tiles; everything else here reads BasicTile/live-tile without creating either. A shoreline with
// plenty of clear water that is ALSO persistently sight-blocked (a sea wall, say) would otherwise
// let one fishing decision re-probe a full <=15x11 box at every candidate all the way out to
// maxRadius. Same budgeting idiom as Z_DOOR_BRIDGE_BUDGET / PLANNER_LEG_BUDGET.
// (Value lives on BotEngine in bot_engine_impl.hpp — bot_supply.cpp's portal-anchored phase
//  needs the same ceiling, and an anonymous-namespace constant here is invisible to it.)

// Every clear water tile actually castable from `stand` — within FISH_CAST_RANGE_X/Y, same floor,
// clear LOS — not just its 8 immediate neighbours. This is what lets a bot fish from wherever it
// is already standing rather than only the one tile touching the water, exactly like a player
// casting a line from a dock. Cheap ground-id/item checks run over the whole box first (zCellAt —
// never materializes); isSightClear only runs on cells that already passed that filter, and stops
// altogether once losBudget is spent.
void collectCastableWater(const Position& stand, std::vector<Position>& out, int32_t& losBudget) {
	out.clear();
	for (int32_t dy = -FISH_CAST_RANGE_Y; dy <= FISH_CAST_RANGE_Y; ++dy) {
		for (int32_t dx = -FISH_CAST_RANGE_X; dx <= FISH_CAST_RANGE_X; ++dx) {
			const int32_t wx = static_cast<int32_t>(stand.x) + dx;
			const int32_t wy = static_cast<int32_t>(stand.y) + dy;
			if (!zCellIsClearWaterAt(wx, wy, stand.z)) {
				continue;
			}
			if (losBudget <= 0) {
				return; // budget spent — keep whatever this candidate already found
			}
			--losBudget;
			const Position water(static_cast<uint16_t>(wx), static_cast<uint16_t>(wy), stand.z);
			if (!g_game().map.isSightClear(stand, water, true)) {
				continue;
			}
			out.push_back(water);
		}
	}
}

// Conservative "a creature could stand here" check without materializing.
// Runtime A* is the real authority; this only gates portal landings.
bool zWalkableAt(int32_t x, int32_t y, int32_t z) {
	ZCell c = zCellAt(x, y, z);
	if (c.tile) {
		if (!c.tile->getGround()) {
			return false;
		}
		if (c.tile->hasFlag(TILESTATE_BLOCKSOLID) || c.tile->hasFlag(TILESTATE_FLOORCHANGE) || c.tile->hasFlag(TILESTATE_TELEPORT)) {
			return false;
		}
		return true;
	}
	if (c.cached) {
		if (!c.cached->ground) {
			return false;
		}
		if (Item::items[c.cached->ground->id].blockSolid) {
			return false;
		}
		if (cachedFcFlags(c.cached) != 0) {
			return false;
		}
		for (const auto& it : c.cached->items) {
			const ItemType& t = Item::items[it->id];
			if (t.blockSolid || t.blockPathFind) {
				return false;
			}
			if (it->destX != 0 || it->destY != 0) {
				return false; // teleport tile
			}
		}
		return true;
	}
	return false;
}

// Given water already proven castable from `fallback` (collectCastableWater found it in
// `fallback`'s own throw box), search for the walkable tile genuinely CLOSEST to the water itself —
// a bounded ring scan centred on the WATER, radius capped at max(FISH_CAST_RANGE_X,
// FISH_CAST_RANGE_Y): nothing further out than that could ever legally cast here, so this costs
// nothing extra when `fallback` already IS the closest tile, and only a handful of extra
// zWalkableAt/isSightClear probes in the common case where it is not.
//
// This is what makes findNearbyFishingSpot hug the shore instead of settling for whichever stand
// its own outward, bot-centred ring search happened to reach first: that search stops at the FIRST
// ring with ANY castable water, and the water can sit anywhere in a stand's <=7,5 box — including
// the far edge, 7 tiles out, with a walkable tile 1 tile from the water going unconsidered because
// the bot's own position already "worked". Re-centring the search on the water and expanding
// outward from THERE finds that tile instead.
//
// `fallback` is always a safe floor: it is itself a proven legal stand for `water` (that is how the
// caller derived `water` to begin with), so a LOS-budget cut-off or a shore with nothing closer than
// `fallback` returns `fallback` unchanged — never worse than the pre-refinement result, only
// potentially better. Ring order guarantees the first walkable+LOS hit is already the nearest, so
// this returns on first success rather than scanning the whole box.
Position zClosestStandForWater(const Position& water, const Position& fallback, int32_t& losBudget) {
	const int32_t maxR = std::max(FISH_CAST_RANGE_X, FISH_CAST_RANGE_Y);
	for (int32_t r = 1; r <= maxR; ++r) {
		for (int32_t dx = -r; dx <= r; ++dx) {
			if (dx < -FISH_CAST_RANGE_X || dx > FISH_CAST_RANGE_X) {
				continue;
			}
			for (int32_t dy = -r; dy <= r; ++dy) {
				if (std::max(std::abs(dx), std::abs(dy)) != r || dy < -FISH_CAST_RANGE_Y || dy > FISH_CAST_RANGE_Y) {
					continue; // interior of the ring, or outside the rectangular <7,5> throw box
				}
				const int32_t cx = static_cast<int32_t>(water.x) + dx;
				const int32_t cy = static_cast<int32_t>(water.y) + dy;
				if (!zWalkableAt(cx, cy, water.z)) {
					continue;
				}
				if (losBudget <= 0) {
					return fallback; // budget spent — the proven candidate beats an unverified one
				}
				--losBudget;
				const Position cand(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), water.z);
				if (!g_game().map.isSightClear(cand, water, true)) {
					continue;
				}
				return cand; // nearest-first ring order: the first hit IS the closest
			}
		}
	}
	return fallback; // nothing closer than what the caller already found
}

// Snap a landing to the best walkable tile around (x,y) on floor lz.
// preferSouth = the moveUpstairs() order (ladders/ropes); otherwise the
// stairs/hole order (same column first).
bool zSnapLanding(int32_t x, int32_t y, int32_t lz, bool preferSouth, Position& out) {
	static const int32_t downOrder[][2] = {
		{ 0, 0 }, { 0, 1 }, { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, -2 }, { -2, 0 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
	};
	static const int32_t upOrder[][2] = {
		{ 0, 1 }, { 0, 0 }, { 0, -1 }, { 1, 0 }, { -1, 0 }, { 0, 2 }, { 2, 0 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 }
	};
	const auto* order = preferSouth ? upOrder : downOrder;
	for (size_t i = 0; i < 11; ++i) {
		const int32_t cx = x + order[i][0];
		const int32_t cy = y + order[i][1];
		if (zWalkableAt(cx, cy, lz)) {
			out = Position(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), static_cast<uint8_t>(lz));
			return true;
		}
	}
	return false;
}

// House id owning this tile, or 0 for public ground.
//
// Bots must not treat somebody's house or garden as a shortcut: those are private, usually behind
// a door they have no key for, and walking a stranger's bot through them is exactly the kind of
// thing a real player would not do. Route planning therefore refuses house tiles UNLESS the
// destination is inside that same house — a bot delivering itself to a house it owns, or to an NPC
// standing indoors, still works.
uint32_t zHouseIdAt(int32_t x, int32_t y, int32_t z) {
	ZCell c = zCellAt(x, y, z);
	if (c.tile) {
		const auto& house = c.tile->getHouse();
		return house ? house->getId() : 0;
	}
	// Fall back to the static map cache. This used to return 0 for every unmaterialized cell, on
	// the reasoning that such a tile "carries no house data" — but BasicTile::houseId is populated
	// at map load for every cell, materialized or not (mapcache.cpp getOrCreateTileFromCache reads
	// it to decide whether to build a HouseTile at all). The old answer therefore made this guard
	// silently INERT for any house nobody had walked into: routes were free to cut straight through
	// a stranger's living room as long as the map cache had never been materialized there, which
	// for most of the map is the normal state.
	//
	// Consequence of fixing it: a guard that was doing nothing starts doing its job, so a route
	// that used to cross an untouched house is now refused and falls through to the next planner
	// tier. That reaches every goToWithDoors caller (hunts, city routes, dwell roams) via
	// zFindBlockingDoor -> tryBridgeDoorLeg, not just the scoped planner — watch DOOR_BRIDGE
	// give-up counters and route path-fail rates after deploy, not only [ZPLAN].
	if (c.cached) {
		return c.cached->houseId;
	}
	return 0;
}

// BOT_HOUSE_VISIT: classify one house cell for the interior index.
//
// Reads the SAME dual {live tile, BasicTile} pair everything else in this sweep reads, and — this
// is the load-bearing part — classifies furniture through the id table (`Item::items[id]`) rather
// than through TILESTATE flags. A cached tile's flags are not backfilled the way a live tile's
// are, so a TILESTATE_DEPOT test would silently miss every locker in a house nobody has walked
// into, which is most of them. The live depot scans elsewhere in the engine legitimately use the
// flag; a cache-sourced harvest cannot.
//
// `walkable` means "a bot could stand here and idle": real ground, not a floor change or teleport,
// nothing blocking. A tile holding a dummy or a locker is reported as such and NOT as an idle
// candidate — those are destinations to stand NEXT to, not on.
void zClassifyHouseCell(const ZCell& c, bool& outWalkable, bool& outDummy, bool& outLocker) {
	outWalkable = outDummy = outLocker = false;
	if (c.tile) {
		for (const auto* items = c.tile->getItemList(); items != nullptr;) {
			for (const auto& item : *items) {
				const ItemType& t = Item::items[item->getID()];
				if (t.isDummy()) outDummy = true;
				if (t.isDepot()) outLocker = true;
			}
			break;
		}
		outWalkable = c.tile->getGround() != nullptr
			&& !c.tile->hasFlag(TILESTATE_FLOORCHANGE)
			&& !c.tile->hasFlag(TILESTATE_TELEPORT)
			&& !c.tile->hasFlag(TILESTATE_BLOCKSOLID)
			&& !c.tile->hasFlag(TILESTATE_BLOCKPATH);
	} else if (c.cached) {
		if (!c.cached->ground || cachedFcFlags(c.cached) != 0) {
			return;
		}
		bool blocked = Item::items[c.cached->ground->id].blockSolid;
		for (const auto& it : c.cached->items) {
			const ItemType& t = Item::items[it->id];
			if (t.isDummy()) outDummy = true;
			if (t.isDepot()) outLocker = true;
			if (t.blockSolid || t.blockPathFind) blocked = true;
			if (it->destX != 0 || it->destY != 0) return; // teleport tile
		}
		outWalkable = !blocked;
	}
	if (outDummy || outLocker) {
		outWalkable = false; // stand beside these, not on them
	}
}

// Shared 8-neighbour offsets. Both the component build and LocalReach need "this tile itself
// isn't floodable (it's a stair/FC tile you stand NEXT to) — check its neighbours".
constexpr int32_t kNbrOffsets[8][2] = {
	{ 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
};

// Like zWalkableAt, but a DOOR tile counts as passable regardless of its current open/closed
// state, because goToWithDoors opens doors on demand at walk time. A component build that
// respected the LIVE door state would mark a portal one shut door away as "disconnected" —
// the same class of false verdict this whole fix exists to remove, just inverted.
//
// zWalkableAt itself is deliberately UNCHANGED: it gates portal LANDING validation, where a
// bare door tile really is a bad place to land.
bool zFloodPassableAt(int32_t x, int32_t y, int32_t z) {
	ZCell c = zCellAt(x, y, z);
	if (c.tile) {
		if (!c.tile->getGround() || c.tile->hasFlag(TILESTATE_FLOORCHANGE) || c.tile->hasFlag(TILESTATE_TELEPORT)) {
			return false;
		}
		if (!c.tile->hasFlag(TILESTATE_BLOCKSOLID)) {
			return true;
		}
		if (const auto* items = c.tile->getItemList()) {
			for (const auto& item : *items) {
				const uint16_t id = item->getID();
				// OPENABLE means "in the door table", not merely ItemType::isDoor(). Tibia types a
				// great many solid, permanently-shut things as doors — a stone window (6445) is
				// literally `type="door"` in items.xml — so isDoor() let the flood walk THROUGH
				// WINDOWS. That produced phantom routes: the flood reported a target reachable, the
				// door collector below reported zero doors (it has always keyed on the door table,
				// so it never recognised the window), no DOOR waypoint was ever created, and A*
				// then refused the route because you cannot walk through a window. Measured at
				// Carlin 'Park Lane 3a': flood said reachable, `/cavebot route` said NO PATH over
				// 6 tiles, and the bot either halted 3 tiles short or bounced up and down stairs
				// hunting for a floor route that was never needed.
				//
				// getDoorTable() is already the definition of "openable" used by the door
				// collector and by tryOpenDoorAt, so this makes the flood agree with the two
				// places that act on its answer rather than inventing a third opinion.
				//
				// Key-locked doors stay impassable: a bot carries no keys, so treating them as
				// open would recreate the optimism this whole predicate exists to remove.
				if (BotEngine::getDoorTable().count(id) && !BotEngine::isKeyLockedDoorId(id)) {
					return true;
				}
			}
		}
		return false;
	}
	if (c.cached) {
		if (!c.cached->ground || cachedFcFlags(c.cached) != 0) {
			return false;
		}
		bool blocked = Item::items[c.cached->ground->id].blockSolid;
		bool openableDoor = false;
		for (const auto& it : c.cached->items) {
			const ItemType& t = Item::items[it->id];
			// Same correction as the live branch above: openable means "in the door table". An
			// open door is not blockSolid and so never needs this escape at all; what this branch
			// decides is whether a SOLID door-typed item may be treated as a way through, and for
			// a window the answer is no.
			if (BotEngine::getDoorTable().count(it->id) && !BotEngine::isKeyLockedDoorId(it->id)) {
				openableDoor = true;
			}
			if (t.blockSolid || t.blockPathFind) {
				blocked = true;
			}
			if (it->destX != 0 || it->destY != 0) {
				return false; // teleport tile
			}
		}
		return !blocked || openableDoor;
	}
	return false;
}

// Component id of (x,y) in a freshly-built per-floor label map, falling back to a labelled
// neighbour when (x,y) is itself an FC/teleport tile (stairs never get flooded through, so they
// carry no direct label). -1 when nothing is adjacent either.
int32_t zLabelOf(const std::unordered_map<uint32_t, uint32_t>& label, int32_t x, int32_t y) {
	auto key = [](int32_t kx, int32_t ky) {
		return (static_cast<uint32_t>(kx) << 16) | static_cast<uint32_t>(ky);
	};
	auto direct = label.find(key(x, y));
	if (direct != label.end()) {
		return static_cast<int32_t>(direct->second);
	}
	for (const auto& d : kNbrOffsets) {
		auto it = label.find(key(x + d[0], y + d[1]));
		if (it != label.end()) {
			return static_cast<int32_t>(it->second);
		}
	}
	return -1;
}

// Bounded local-BFS reachability oracle for the two END legs of a route (start->candidate
// portal, last landing->target). Those positions are arbitrary and were never labelled at build
// time, so they get ONE flood each — computed lazily and reused for every candidate anchored at
// the same origin, never re-run per candidate. Capped by Chebyshev radius AND a hard cell budget
// so a wide-open floor cannot blow the planning tick.
// Cache handle type mirrors BotEngine::zReachCache_ (declared in the impl header); passed in by
// pointer so this anonymous-namespace class can share the engine's map without owning it.
using ZReachCacheMap = std::unordered_map<uint64_t, std::pair<BotEngine::ZReachSet, int64_t>>;

class LocalReach {
public:
	// cache/hits/misses are optional: pass nullptr to force a private flood. Nothing does that
	// any more -- the ad-hoc unlabelled-anchor path used to, on the false premise that its
	// origins were one-off. They are portal positions, which recur across every plan.
	LocalReach(Position origin, int32_t radius, uint32_t budget,
	           ZReachCacheMap* cache = nullptr, uint64_t* hits = nullptr, uint64_t* misses = nullptr,
	           int64_t ttlMs = 0, size_t cacheMax = 0) :
		origin_(origin), radius_(radius), budget_(budget), cache_(cache), hits_(hits),
		misses_(misses), ttlMs_(ttlMs), cacheMax_(cacheMax) { }

	bool reachable(const Position& p) {
		if (p.z != origin_.z || botnav::zCheb(origin_, p) > radius_) {
			return false;
		}
		ensure();
		if (!set_) {
			return false;
		}
		if (set_->count(botTileKey(p))) {
			return true;
		}
		// p may be a stair/FC tile the flood never enters — accept an adjacent reachable tile,
		// the same rule the build-time anchor projection uses.
		for (const auto& d : kNbrOffsets) {
			Position np(static_cast<uint16_t>(static_cast<int32_t>(p.x) + d[0]),
			            static_cast<uint16_t>(static_cast<int32_t>(p.y) + d[1]), p.z);
			if (set_->count(botTileKey(np))) {
				return true;
			}
		}
		return false;
	}

	uint32_t cellsVisited() const {
		return set_ ? static_cast<uint32_t>(set_->size()) : 0;
	}

	bool wasCacheHit() const {
		return cacheHit_;
	}

	// BOT_AMBIENT_ROAM: the flood set itself. Every other consumer only ever asks "is THIS tile
	// reachable", so `set_` stayed private; roam has to ENUMERATE the reachable tiles to pick a
	// random one, which is the one question a membership test cannot answer. Returns nullptr if
	// the flood produced nothing.
	const std::unordered_set<uint64_t>* cells() {
		ensure();
		return set_ ? set_.get() : nullptr;
	}

	uint8_t originZ() const {
		return origin_.z;
	}

private:
	void ensure() {
		if (computed_) {
			return;
		}
		computed_ = true;
		const uint64_t originKey = botTileKey(origin_);
		const int64_t now = OTSYS_TIME();

		if (cache_) {
			auto it = cache_->find(originKey);
			if (it != cache_->end()) {
				if (now < it->second.second) {
					set_ = it->second.first;
					cacheHit_ = true;
					if (hits_) {
						++*hits_;
					}
					return;
				}
				cache_->erase(it); // expired
			}
			if (misses_) {
				++*misses_;
			}
		}

		auto owned = std::make_shared<std::unordered_set<uint64_t>>();
		if (zFloodPassableAt(origin_.x, origin_.y, origin_.z)) {
			flood(*owned, origin_.x, origin_.y);
		} else {
			// Origin itself unwalkable (mid-transition, or snapped onto a portal tile) — seed from
			// the first passable neighbour instead.
			for (const auto& d : kNbrOffsets) {
				const int32_t nx = static_cast<int32_t>(origin_.x) + d[0];
				const int32_t ny = static_cast<int32_t>(origin_.y) + d[1];
				if (zFloodPassableAt(nx, ny, origin_.z)) {
					flood(*owned, nx, ny);
					break;
				}
			}
		}
		set_ = owned;

		if (cache_ && cacheMax_ > 0) {
			// Crude size bound: once full, drop the oldest-expiring entry. Floods are all
			// equally valid, so eviction order barely matters — only the memory ceiling does.
			if (cache_->size() >= cacheMax_) {
				auto oldest = cache_->begin();
				for (auto i = cache_->begin(); i != cache_->end(); ++i) {
					if (i->second.second < oldest->second.second) {
						oldest = i;
					}
				}
				cache_->erase(oldest);
			}
			(*cache_)[originKey] = { set_, now + ttlMs_ };
		}
	}

	void flood(std::unordered_set<uint64_t>& visited, int32_t sx, int32_t sy) {
		auto keyOf = [this](int32_t x, int32_t y) {
			return botTileKey(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), origin_.z));
		};
		std::vector<std::pair<int32_t, int32_t>> queue;
		visited.insert(keyOf(sx, sy));
		queue.push_back({ sx, sy });
		while (!queue.empty() && visited.size() < budget_) {
			auto [cx, cy] = queue.back();
			queue.pop_back();
			if (std::max(std::abs(cx - static_cast<int32_t>(origin_.x)),
			             std::abs(cy - static_cast<int32_t>(origin_.y))) >= radius_) {
				continue; // hit the leg-max boundary
			}
			for (const auto& d : kNbrOffsets) {
				const int32_t nx = cx + d[0], ny = cy + d[1];
				const uint64_t nk = keyOf(nx, ny);
				if (visited.size() >= budget_ || visited.count(nk) || !zFloodPassableAt(nx, ny, origin_.z)) {
					continue;
				}
				visited.insert(nk);
				queue.push_back({ nx, ny });
			}
		}
	}

	Position origin_;
	int32_t radius_;
	uint32_t budget_;
	ZReachCacheMap* cache_ = nullptr;
	uint64_t* hits_ = nullptr;
	uint64_t* misses_ = nullptr;
	int64_t ttlMs_ = 0;
	size_t cacheMax_ = 0;
	bool computed_ = false;
	bool cacheHit_ = false;
	BotEngine::ZReachSet set_; // shared with the cache when cached
};

// ============================================================================
// BOT_ROUTE_SPLICE — the walkability probe behind city-route detour splicing.
//
// An authored city route often walks INTO the temple and straight back out, because
// `farmine|carpet~depot` is literally `carpet~temple` + `temple~depot` concatenated. The splice
// pass elides such a z-excursion and jumps the bot from the waypoint before it to the waypoint
// after it. That jump is unwaypointed, so before blessing one we must prove the bot can actually
// walk it. These are the three gates that prove it.
//
// EVERYTHING HERE IS PLAYER-FREE, and that is a hard requirement, not a convenience.
// loadCityRouteCore runs for HIBERNATED bots, which have no Player at all. A probe needing one
// would splice for awake bots and not for hibernated ones — the live/virtual divergence that made
// a24c5b909 largely inert in production. It also rules out the real A* kernel: Map::canWalkTo
// returns nullptr for a null creature (map.cpp:658-661), so botnav::findPath with no creature
// reports every tile unwalkable and would silently decline every splice.
//
// All three gates are conservative by construction: a failure DECLINES the splice and the bot
// walks today's authored route unchanged. The only error this probe can make cheaply is refusing
// a gap that was in fact fine.
// ============================================================================

// Strict cousin of zFloodPassableAt. It is deliberately NOT that function, and the difference is
// the whole point:
//
//   DOORS. zFloodPassableAt treats an openable door tile as passable, on the stated premise that
//   "goToWithDoors opens doors on demand at walk time" (see its comment above). That premise is
//   false for THIS consumer. A spliced gap is walked by followWaypoints' plain-goTo branch, and
//   the door fallback there is excluded for NODE waypoints outright:
//       if (waypoint.type != WaypointType::NODE) { if (tryOpenDoors(...)) ... }   [bot_waypoint.cpp]
//   So nothing would ever open a door on a spliced gap. A splice across a door would A*-fail into
//   it for perWpStuckMs (30 s) and then skip the waypoint — for every bot, on every trip, forever,
//   because the splice is cached. Hence: any door-table item at all disqualifies the tile,
//   INCLUDING an open one, since an open door can be shut later and no waypoint exists to reopen it.
//
//   HOUSES. The "never route through somebody's home" rule lives in the planner tiers
//   (zFindDoorsOnPathImpl's houseGuard), not in the flood. Without this check a corner-cut through
//   a stranger's living room would be blessed here.
//
// It inherits zFloodPassableAt's rejection of FC and teleport tiles, which is what makes an anchor
// sitting on a stair tile decline automatically rather than producing a gap A* can never complete.
bool botSpliceWalkableAt(int32_t x, int32_t y, int32_t z) {
	if (!zFloodPassableAt(x, y, z)) {
		return false; // not walkable even under the permissive rules
	}
	if (zHouseIdAt(x, y, z) != 0) {
		return false; // somebody's house
	}
	// zFloodPassableAt may have accepted this tile *because* of its door escape. Re-inspect and
	// reject anything carrying a door-table item, open or shut.
	const ZCell c = zCellAt(x, y, z);
	if (c.tile) {
		if (const auto* items = c.tile->getItemList()) {
			for (const auto& item : *items) {
				if (BotEngine::getDoorTable().count(item->getID())) {
					return false;
				}
			}
		}
		return true;
	}
	if (c.cached) {
		for (const auto& it : c.cached->items) {
			if (BotEngine::getDoorTable().count(it->id)) {
				return false;
			}
		}
		return true;
	}
	return false;
}

// Gate 6b. Same-floor 8-connected BFS from `from` to `to` over botSpliceWalkableAt, returning the
// STEP COUNT, or -1 if unreachable.
//
// Step count, not mere reachability: gate 6c compares the walk against its own straight line, and
// a membership test (which is all LocalReach offers) cannot answer that. A tile 6 away that needs
// a 40-step walk around a wall is reachable and is still a bad splice.
//
// The Chebyshev radius bound is load-bearing, not just a budget: a path that leaves the box and
// comes back is deliberately NOT found. If the bot would have to walk well outside the corridor
// the authored route establishes, we do not want that splice at all.
//
// Note the deliberate DIVERGENCE from LocalReach::reachable, which accepts a target via an
// 8-neighbour fallback when the target tile itself is one the flood will not enter. We do the
// opposite and require BOTH endpoints to be strictly walkable. LocalReach is asking "is this
// region connected", where projecting onto a neighbour is the right answer for a stair tile; we
// are asking "will the walker arrive ON this exact tile", and for a FC tile the answer is no,
// because A* refuses to path onto one.
int32_t botSpliceStepDist(const Position& from, const Position& to, int32_t radius, uint32_t budget) {
	if (from.z != to.z) {
		return -1;
	}
	if (botnav::zCheb(from, to) > radius) {
		return -1;
	}
	if (from.x == to.x && from.y == to.y) {
		return 0;
	}
	// Both endpoints must themselves be walkable under the strict predicate. Without this an
	// anchor sitting on a FLOORCHANGE tile (typed `stand` in imported data, invisible to any
	// offline check) would yield a gap the live A* can never complete, since A* refuses FC tiles.
	if (!botSpliceWalkableAt(from.x, from.y, from.z) || !botSpliceWalkableAt(to.x, to.y, to.z)) {
		return -1;
	}

	const uint64_t goal = botTileKey(to);
	std::unordered_set<uint64_t> visited;
	std::vector<std::pair<Position, int32_t>> queue; // tile + steps from `from`
	size_t head = 0;
	visited.insert(botTileKey(from));
	queue.emplace_back(from, 0);

	while (head < queue.size()) {
		const auto [cur, steps] = queue[head++];
		for (const auto& d : kNbrOffsets) {
			const int32_t nx = static_cast<int32_t>(cur.x) + d[0];
			const int32_t ny = static_cast<int32_t>(cur.y) + d[1];
			const Position np(static_cast<uint16_t>(nx), static_cast<uint16_t>(ny), from.z);
			if (botnav::zCheb(from, np) > radius) {
				continue; // outside the corridor we are willing to bless
			}
			const uint64_t nk = botTileKey(np);
			if (visited.count(nk)) {
				continue;
			}
			if (nk == goal) {
				return steps + 1;
			}
			if (visited.size() >= budget) {
				return -1; // budget exhausted before reaching the goal
			}
			if (!botSpliceWalkableAt(nx, ny, from.z)) {
				continue;
			}
			visited.insert(nk);
			queue.emplace_back(np, steps + 1);
		}
	}
	return -1;
}

// Find the CLOSED, OPENABLE door that stands between the bot and `target`.
//
// Why a BFS and not "nearest door by distance": a cluster of six doors sits around the Thais
// castle approach, and picking the raw nearest flips between doors on opposite sides of the bot as
// it shuffles — a ping-pong. Expanding outward from the bot through door-PERMISSIVE terrain visits
// tiles in true walking order, so the first door found is genuinely the first one in the way.
//
// The progress filter matters as much: a door is only a candidate if standing at it gets the bot
// CLOSER to the target than it is now. Without it the BFS happily returns a door behind the bot.
//
// Deliberately reuses zFloodPassableAt, the same predicate the portal-graph components are built
// from, so "the planner thought this was connected" and "the bot can bridge to it" agree by
// construction. Bounded by radius and cell budget like every other flood here.
// Every closed-but-openable door that lies ON the actual route from `from` to `target`,
// in walking order.
//
// The previous version returned any openable door found in a 24-tile flood that happened to be
// closer to the target than the start. That is not "in the way" — it is "vaguely over there", so
// the bot detoured to doors off to the side of its route and tried to open them. Worse, the caller
// then re-ran the search FROM that door, wandering further off course each time.
//
// This reconstructs the real path instead: BFS over door-permissive terrain with parent links,
// then walk the chain back from the target and keep only the door tiles the path actually crosses.
// A door the route never touches is never returned, so the bot only opens what genuinely blocks it.
//
// `houseGuard` rejects tiles belonging to a house other than the ones this route legitimately
// touches, so a route is never planned through somebody's home or garden — see zHouseIdAt.
//
// TWO houses are admitted, not one. The destination's house is the obvious case: a bot walking to
// an NPC indoors, or to a house it is visiting, must be allowed inside. The house the bot is
// STANDING IN is the non-obvious one, and omitting it is fatal the moment a bot can be indoors at
// all: leaving a house for a public destination gives allowedHouse == 0, every neighbour of the
// bot's own tile is house ground with hid != 0, and the flood dies on its first step — unable to
// reach the very door it needs to open. That went unnoticed for as long as it did because nothing
// could put a bot inside a house in the first place (HouseTile::queryAdd refuses an uninvited
// player), so this guard had only ever been exercised walking IN.
bool zFindDoorsOnPathImpl(const Position& from, const Position& target, int32_t radius,
                          uint32_t budget, uint32_t allowedHouseId, uint32_t fromHouseId,
                          std::vector<Position>& outDoors) {
	outDoors.clear();
	if (from.z != target.z) {
		return false;
	}
	std::unordered_map<uint64_t, uint64_t> parent; // tile key -> predecessor key
	std::unordered_map<uint64_t, Position> posOf;
	std::vector<Position> queue;
	const uint64_t fromKey = botTileKey(from);
	parent[fromKey] = fromKey;
	posOf[fromKey] = from;
	queue.push_back(from);

	const uint64_t targetKey = botTileKey(target);
	bool reached = false;
	size_t head = 0;
	while (head < queue.size() && parent.size() < budget) {
		const Position cur = queue[head++];
		if (botTileKey(cur) == targetKey) {
			reached = true;
			break;
		}
		for (const auto& d : kNbrOffsets) {
			const int32_t nx = static_cast<int32_t>(cur.x) + d[0];
			const int32_t ny = static_cast<int32_t>(cur.y) + d[1];
			const Position np(static_cast<uint16_t>(nx), static_cast<uint16_t>(ny), from.z);
			const uint64_t nk = botTileKey(np);
			if (parent.count(nk)) {
				continue;
			}
			if (std::max(std::abs(nx - static_cast<int32_t>(from.x)),
			             std::abs(ny - static_cast<int32_t>(from.y))) > radius) {
				continue;
			}
			// The target itself is admitted even when it is an FC/portal tile: that is the tile
			// we are trying to REACH, and portals are never flood-passable by construction.
			const bool isTarget = (nk == targetKey);
			if (!isTarget) {
				if (!zFloodPassableAt(nx, ny, from.z)) {
					continue;
				}
				const uint32_t hid = zHouseIdAt(nx, ny, from.z);
				if (hid != 0 && hid != allowedHouseId && hid != fromHouseId) {
					continue; // someone else's house — not a shortcut
				}
			}
			parent[nk] = botTileKey(cur);
			posOf[nk] = np;
			queue.push_back(np);
		}
	}
	if (!reached) {
		return false;
	}
	// Walk back from the target collecting doors, then reverse into walking order.
	uint64_t k = targetKey;
	while (true) {
		const uint64_t pk = parent[k];
		const Position p = posOf[k];
		if (k != targetKey && k != fromKey) {
			// A closed openable door: not plain-walkable (A* refuses it) but flood-passable.
			if (!zWalkableAt(p.x, p.y, p.z) && zFloodPassableAt(p.x, p.y, p.z)) {
				const auto& tile = g_game().map.getTile(p);
				if (tile) {
					if (const auto* items = tile->getItemList()) {
						for (const auto& item : *items) {
							const uint16_t id = item->getID();
							if (BotEngine::getDoorTable().count(id) && !BotEngine::isKeyLockedDoorId(id)) {
								outDoors.push_back(p);
								break;
							}
						}
					}
				}
			}
		}
		if (pk == k) {
			break;
		}
		k = pk;
	}
	std::reverse(outDoors.begin(), outDoors.end());
	return true;
}

// Replica of Tile::queryDestination (tile.cpp:932) for the DOWN direction —
// including the offset-tile SOUTH_ALT/EAST_ALT checks the dead
// computeFloorChangeDest missed. Falls back to snapping when the computed
// tile is not walkable.
bool zStairsDownLanding(int32_t x, int32_t y, int32_t z, Position& out) {
	const int32_t dz = z + 1;
	// queryDestination checks (x, y-1, dz) for SOUTH_ALT and (x-1, y, dz) for EAST_ALT first.
	if (zFcFlagsAt(x, y - 1, dz) & TILESTATE_FLOORCHANGE_SOUTH_ALT) {
		if (zWalkableAt(x, y - 2, dz)) {
			out = Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y - 2), static_cast<uint8_t>(dz));
			return true;
		}
	} else if (zFcFlagsAt(x - 1, y, dz) & TILESTATE_FLOORCHANGE_EAST_ALT) {
		if (zWalkableAt(x - 2, y, dz)) {
			out = Position(static_cast<uint16_t>(x - 2), static_cast<uint16_t>(y), static_cast<uint8_t>(dz));
			return true;
		}
	} else {
		int32_t dx = x, dy = y;
		const uint32_t below = zFcFlagsAt(x, y, dz);
		if (below & TILESTATE_FLOORCHANGE_NORTH) {
			++dy;
		}
		if (below & TILESTATE_FLOORCHANGE_SOUTH) {
			--dy;
		}
		if (below & TILESTATE_FLOORCHANGE_SOUTH_ALT) {
			dy -= 2;
		}
		if (below & TILESTATE_FLOORCHANGE_EAST) {
			--dx;
		}
		if (below & TILESTATE_FLOORCHANGE_EAST_ALT) {
			dx -= 2;
		}
		if (below & TILESTATE_FLOORCHANGE_WEST) {
			++dx;
		}
		if (zWalkableAt(dx, dy, dz)) {
			out = Position(static_cast<uint16_t>(dx), static_cast<uint16_t>(dy), static_cast<uint8_t>(dz));
			return true;
		}
	}
	return zSnapLanding(x, y, dz, false, out);
}

// Replica of the UP branch of queryDestination (own-tile flags decide the offset).
bool zStairsUpLanding(int32_t x, int32_t y, int32_t z, uint32_t ownFlags, Position& out) {
	const int32_t dz = z - 1;
	int32_t dx = x, dy = y;
	if (ownFlags & TILESTATE_FLOORCHANGE_NORTH) {
		--dy;
	}
	if (ownFlags & TILESTATE_FLOORCHANGE_SOUTH) {
		++dy;
	}
	if (ownFlags & TILESTATE_FLOORCHANGE_EAST) {
		++dx;
	}
	if (ownFlags & TILESTATE_FLOORCHANGE_WEST) {
		--dx;
	}
	if (ownFlags & TILESTATE_FLOORCHANGE_SOUTH_ALT) {
		dy += 2;
	}
	if (ownFlags & TILESTATE_FLOORCHANGE_EAST_ALT) {
		dx += 2;
	}
	if (zWalkableAt(dx, dy, dz)) {
		out = Position(static_cast<uint16_t>(dx), static_cast<uint16_t>(dy), static_cast<uint8_t>(dz));
		return true;
	}
	return zSnapLanding(x, y, dz, false, out);
}

bool idInVector(const std::vector<uint16_t>& v, uint16_t id) {
	return std::find(v.begin(), v.end(), id) != v.end();
}

// Classify one column cell into 0..n directed portals. Shared by the graph
// build and the dumpnav-v2 region collector.
void zClassifyCell(int32_t x, int32_t y, int32_t z, const ZCell& c, std::vector<botnav::ZPortal>& out) {
	uint32_t fcFlags = 0;
	bool ladder = false, sewer = false, shovel = false, rope = false;
	Position teleportDest {};
	bool teleport = false;

	if (c.tile) {
		fcFlags = zFcFlagsAt(x, y, z); // live flags
		if (auto ground = c.tile->getGround()) {
			const uint16_t gid = ground->getID();
			ladder |= ground->isLadder() || isLadderItemId(gid);
			sewer |= gid == SEWER_ITEM_ID;
			shovel |= idInVector(SHOVEL_HOLE_IDS, gid);
			rope |= idInVector(ROPE_SPOT_IDS, gid);
		}
		if (const auto* items = c.tile->getItemList()) {
			for (const auto& item : *items) {
				const uint16_t id = item->getID();
				ladder |= item->isLadder() || isLadderItemId(id);
				sewer |= id == SEWER_ITEM_ID;
				rope |= id == 12935;
			}
		}
		if (const auto& tp = c.tile->getTeleportItem()) {
			teleportDest = tp->getDestPos();
			teleport = teleportDest.x != 0 || teleportDest.y != 0;
		}
	} else if (c.cached) {
		fcFlags = cachedFcFlags(c.cached);
		auto classifyId = [&](uint16_t id) {
			const ItemType& it = Item::items[id];
			ladder |= it.isLadder() || isLadderItemId(id);
			sewer |= id == SEWER_ITEM_ID;
			rope |= id == 12935;
		};
		if (c.cached->ground) {
			const uint16_t gid = c.cached->ground->id;
			classifyId(gid);
			shovel |= idInVector(SHOVEL_HOLE_IDS, gid);
			rope |= idInVector(ROPE_SPOT_IDS, gid);
		}
		for (const auto& bi : c.cached->items) {
			classifyId(bi->id);
			if (bi->destX != 0 || bi->destY != 0) {
				teleportDest = Position(bi->destX, bi->destY, bi->destZ);
				teleport = true;
			}
		}
	} else {
		return;
	}

	const Position pos(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z));
	Position land;

	// A portal can only go up if there IS a floor above, and down if there is one below.
	// Without these, z==0 computes a landing at z-1 == -1, which `static_cast<uint8_t>`
	// wraps to 255 — and every consumer then indexes a MAP_MAX_LAYERS-sized array with it.
	// That is a real 239-byte stack smash in buildZPortalGraph (found by ASan); it corrupted
	// a neighbouring local and surfaced later as a SIGSEGV in unrelated code.
	const bool canGoUp = z > 0;
	const bool canGoDown = z + 1 < MAP_MAX_LAYERS;

	if (teleport) {
		if (zWalkableAt(teleportDest.x, teleportDest.y, teleportDest.z) || zSnapLanding(teleportDest.x, teleportDest.y, teleportDest.z, false, teleportDest)) {
			out.push_back({ pos, teleportDest, botnav::ZPortalKind::TELEPORT, teleportDest.z > z });
		}
		return; // a teleport tile is nothing else
	}

	if (fcFlags & TILESTATE_FLOORCHANGE_DOWN) {
		if (canGoDown && zStairsDownLanding(x, y, z, land)) {
			out.push_back({ pos, land, botnav::ZPortalKind::HOLE, true });
		}
	} else if (fcFlags != 0) {
		// UP-flag transition (stairs/ramps with N/S/E/W or *_ALT flags)
		if (canGoUp && zStairsUpLanding(x, y, z, fcFlags, land)) {
			out.push_back({ pos, land, botnav::ZPortalKind::STAIRS, false });
		}
	}

	// USE-item mechanisms (these carry no FLOORCHANGE tile flags — the exact
	// gap that makes them invisible to A* and to navdump v1).
	//
	// zSnapLandingT replaces the old zSnapLanding for these four kinds. The old one tried
	// candidates the real mechanics never try (the self tile, radius-2 neighbours) and REJECTED
	// the portal when nothing was walkable — where the actual scripts force-place the player.
	// That silently dropped or mis-landed ladder/rope/sewer/shovel portals. It always succeeds
	// now, and reports whether the landing was forced so the planner can check that exact tile
	// instead of trusting a neighbour's reachability.
	auto walkable = [](int32_t wx, int32_t wy, int32_t wz) {
		return zWalkableAt(wx, wy, wz);
	};
	bool forced = false;
	if (ladder && canGoUp) {
		botnav::zSnapLandingT(x, y, z - 1, botnav::ZSnapMode::UP_SCAN, walkable, land, forced);
		out.push_back({ pos, land, botnav::ZPortalKind::LADDER, false, forced });
	}
	if (rope && canGoUp) {
		botnav::zSnapLandingT(x, y, z - 1, botnav::ZSnapMode::UP_SCAN, walkable, land, forced);
		out.push_back({ pos, land, botnav::ZPortalKind::ROPE_SPOT, false, forced });
	}
	if (sewer && canGoDown && !(fcFlags & TILESTATE_FLOORCHANGE_DOWN)) {
		botnav::zSnapLandingT(x, y, z + 1, botnav::ZSnapMode::DOWN_FORCED, walkable, land, forced);
		out.push_back({ pos, land, botnav::ZPortalKind::SEWER, true, forced });
	}
	if (shovel && canGoDown && !(fcFlags & TILESTATE_FLOORCHANGE_DOWN)) {
		botnav::zSnapLandingT(x, y, z + 1, botnav::ZSnapMode::DOWN_FORCED, walkable, land, forced);
		out.push_back({ pos, land, botnav::ZPortalKind::SHOVEL_HOLE, true, forced });
	}
}

} // namespace

// ============================================================================
// BOT_ROUTE_SPLICE — the pass itself. Lives in this TU because botSpliceStepDist and
// botSpliceWalkableAt are file-scope here, exactly as getRoamRegion lives here for LocalReach.
// ============================================================================

// Tuning. In the source rather than config.lua on purpose: this corrects authored routes that walk
// into a temple and straight back out, and there is no deployment in which we want that walk back.
// `inline`, not bare `static` — per CLAUDE.md a file-scope `static` in a shared header forks per
// TU. These are constexpr and immutable so a fork would be harmless, but the codebase has one rule
// here and this follows it.
inline constexpr bool     BOT_SPLICE_ENABLED    = true;
inline constexpr int32_t  BOT_SPLICE_MAX_GAP    = 10;  // furthest same-floor jump we will bless
inline constexpr int32_t  BOT_SPLICE_MIN_SAVE   = 8;   // authored cost the jump must actually save
inline constexpr int32_t  BOT_SPLICE_LOOKAHEAD  = 8;   // waypoints scanned ahead for a return
inline constexpr int32_t  BOT_SPLICE_Z_PENALTY  = 8;   // modelled cost of one floor change
inline constexpr int32_t  BOT_SPLICE_DIRECTNESS = 2;   // gap walk may cost at most 2x its straight line
inline constexpr uint32_t BOT_SPLICE_BUDGET     = 600; // BFS cell ceiling; (2*10+1)^2 = 441

namespace {

// A waypoint is "plain" if removing it, or anchoring a jump on it, loses nothing. Every other type
// does work — USE_WITH, LADDER, ROPE, HOLE, DOOR, ACTION, LEVITATE_*, MACHETE, NPC_INTERACT,
// TELEPORT — and a non-empty extraData carries a side effect even on a NODE/STAND (the shrine
// return marker followWaypoints stamps, and the ice-fishing markers).
//
// The ANCHORS must be plain too, not merely the interior. Otherwise a window ending at a USE_WITH
// could elide the positioning STAND placed immediately before it — Farmine has exactly that shape
// (stand 32991,31539,1 then use_with 32992,31539,1).
bool botSpliceWpPlain(const Waypoint& w) {
	return (w.type == WaypointType::NODE || w.type == WaypointType::STAND)
		&& w.itemId == 0 && w.extraData.empty();
}

// What the authored route pays to walk W[k] -> W[k+1]. A floor change is not free: the FC state
// machine has to find a stair, step on it, and sit out zChangeGraceMs.
int32_t botSpliceLegCost(const Waypoint& a, const Waypoint& b) {
	return botnav::zCheb(a.pos, b.pos) + (a.pos.z != b.pos.z ? BOT_SPLICE_Z_PENALTY : 0);
}

} // namespace

// Elide z-excursion detours from a city-route waypoint list, in place. Returns the number of
// waypoints removed; appends one line per accepted splice to outLog and one per declined window to
// rejectLog (both optional — the audit command passes them, the runtime path passes outLog only).
//
// W[0] and W[n-1] are structurally safe: they can only ever be anchors, never interior, so a
// caller's "this route starts at src and ends at dst" contract survives untouched.
//
// Scanning is shortest-jump-first (smallest qualifying j, not largest). That keeps each
// unwaypointed gap as short as it can be, which minimises the surface on which this probe and the
// real walker can disagree.
size_t BotEngine::spliceRouteDetours(std::vector<Waypoint>& wps, const std::string& tag,
                                     std::vector<std::string>* outLog,
                                     std::vector<std::string>* rejectLog) {
	if (!BOT_SPLICE_ENABLED || wps.size() < 3) {
		return 0;
	}
	size_t removed = 0;
	size_t i = 0;
	while (i + 2 < wps.size()) {
		const size_t limit = std::min(i + static_cast<size_t>(BOT_SPLICE_LOOKAHEAD), wps.size() - 1);
		for (size_t j = i + 2; j <= limit; j++) {
			const Waypoint& a = wps[i];
			const Waypoint& b = wps[j];
			// 1. same floor
			if (a.pos.z != b.pos.z) {
				continue;
			}
			// 3. anchors and interior must all be plain
			if (!botSpliceWpPlain(a) || !botSpliceWpPlain(b)) {
				continue;
			}
			bool interiorPlain = true;
			bool zExcursion = false;
			for (size_t k = i + 1; k < j; k++) {
				if (!botSpliceWpPlain(wps[k])) {
					interiorPlain = false;
					break;
				}
				if (wps[k].pos.z != a.pos.z) {
					zExcursion = true;
				}
			}
			if (!interiorPlain) {
				continue;
			}
			// 4. the span must actually LEAVE the floor and come back. This is the single biggest
			// scope reduction in the design: without it the pass also "shortcuts" same-floor
			// stretches, which is how it ends up straight-lining through Venore swamp and cutting
			// corners through Ankrahmun blocks. Measured over the authored data: 385 splices
			// without this line, 199 with it, and Thais drops from 19 to 0.
			if (!zExcursion) {
				continue;
			}
			// 2. close to each other
			const int32_t gap = botnav::zCheb(a.pos, b.pos);
			if (gap > BOT_SPLICE_MAX_GAP) {
				continue;
			}
			// 5. the jump must save real walking
			int32_t elided = 0;
			for (size_t k = i; k < j; k++) {
				elided += botSpliceLegCost(wps[k], wps[k + 1]);
			}
			if (elided - gap < BOT_SPLICE_MIN_SAVE) {
				continue;
			}

			// ---- 6. map-backed gates. Only windows that already passed 1-5 get here. ----
			//
			// LINE OF SIGHT WAS TRIED HERE AND REMOVED, on measurement. isSightClear looked like
			// the natural gate — it is Player-free, cheap, and it encodes "within visible
			// distance". Audited across all 19 towns it turned out to be pure false-negative:
			//
			//   320 windows  los=FAIL reach=FAIL   reachability rejects them anyway
			//    60 windows  los=ok   reach=FAIL   LOS PASSED an unwalkable gap
			//    16 windows  los=FAIL reach=ok     LOS the sole rejector  <-- all of them wrong
			//     2 windows  los=FAIL direct=FAIL  directness rejects them anyway
			//
			// Of the 16 it uniquely rejected, several had gapCost EXACTLY equal to their Chebyshev
			// distance — a dead-straight walk with no detour at all — and `/cavebot route` walked
			// one of them (Roshamuul depot~boat 33521,32364,7 -> 33515,32357,7) in 13 tiles. The
			// cause is that CONST_PROP_BLOCKPROJECTILE is the protobuf `unsight` flag
			// (items.cpp:227), so a parapet or window beside the line blocks SIGHT while the
			// 8-connected walk steps around it. Sight and passage are different questions, and it
			// is passage we needed. The 60 `los=ok reach=FAIL` rows are the same lesson inverted:
			// LOS would have been actively dangerous on its own.
			//
			// It is still COMPUTED in audit mode and reported, so the evidence stays visible and
			// nobody re-adds it as a gate on intuition.
			//
			// AUDIT MODE (rejectLog set) evaluates every gate rather than short-circuiting. The
			// runtime path short-circuits, which is right for speed and useless for diagnosis —
			// a short-circuiting audit is what hid this finding on the first run.
			const bool audit = (rejectLog != nullptr);
			// 6a: line of sight. CONST_PROP_BLOCKPROJECTILE is the protobuf `unsight` flag
			// (items.cpp:227), so this tests WALLS — not furniture, and not fences, railings or
			// water. It is emphatically NOT a walkability test on its own; 6b is what proves a walk
			// exists. What 6a uniquely catches is the jump straight across a building, which is
			// exactly the shape a route deliberately goes over or around.
			// 6a: a real same-floor walk exists, and it is cheaper than the detour it replaces.
			const int32_t gapCost = botSpliceStepDist(a.pos, b.pos, BOT_SPLICE_MAX_GAP, BOT_SPLICE_BUDGET);
			const bool reachOk = (gapCost >= 0);
			const bool saveOk = reachOk && (gapCost + BOT_SPLICE_MIN_SAVE <= elided);
			// 6b: directness. An 8-connected walk down a clear straight line costs exactly its
			// Chebyshev distance, so gapCost/gap is a true detour factor. This rejects "reachable,
			// but only by winding around inside the box".
			const bool directOk = reachOk && (gapCost <= gap * BOT_SPLICE_DIRECTNESS);
			if (audit && !(reachOk && saveOk && directOk)) {
				rejectLog->push_back(fmt::format(
					"{} [{}]->[{}] gap={} elided={} REJECT reach={} save={} direct={} (los={}, not a gate)",
					tag, i, j, gap, elided,
					reachOk ? fmt::format("{}", gapCost) : "FAIL",
					saveOk ? "ok" : "FAIL", directOk ? "ok" : "FAIL",
					g_game().map.isSightClear(a.pos, b.pos, true) ? "ok" : "FAIL"));
				continue;
			}
			if (!reachOk || !saveOk) {
				continue;
			}
			if (!directOk) {
				continue;
			}

			if (outLog) {
				std::string interior;
				for (size_t k = i + 1; k < j; k++) {
					if (!interior.empty()) {
						interior += ",";
					}
					interior += waypointTypeName(wps[k].type);
				}
				outLog->push_back(fmt::format(
					"{} [{}]({},{},{}) -> [{}]({},{},{}) gap={} cost={} elided={} cut={} interior={}",
					tag, i, a.pos.x, a.pos.y, a.pos.z, j, b.pos.x, b.pos.y, b.pos.z,
					gap, gapCost, elided, j - i - 1, interior));
			}
			wps.erase(wps.begin() + static_cast<std::ptrdiff_t>(i) + 1,
			          wps.begin() + static_cast<std::ptrdiff_t>(j));
			removed += j - i - 1;
			break;
		}
		// After an erase the kept anchor W[j] has slid down to index i+1, so advancing by one
		// resumes AT it either way — which is what lets a chained double-dip excursion get its own
		// window instead of being swallowed by the first one.
		i++;
	}
	return removed;
}

// Member wrappers so bot_nav.cpp can reach the anonymous-namespace implementations above.

// Doors ON the route, in walking order. `target` is usually a portal tile; if it sits inside a
// house, that house is whitelisted so the bot may enter to reach it — and so is the house the bot
// is currently standing in, so it can find its way back OUT. See zFindDoorsOnPathImpl.
bool BotEngine::zFindDoorsOnPath(const Position& from, const Position& target, std::vector<Position>& outDoors) {
	const uint32_t allowedHouse = zHouseIdAt(target.x, target.y, target.z);
	const uint32_t fromHouse = zHouseIdAt(from.x, from.y, from.z);
	return zFindDoorsOnPathImpl(from, target, Z_DOOR_BRIDGE_RADIUS, Z_DOOR_BRIDGE_BUDGET,
	                            allowedHouse, fromHouse, outDoors);
}

// Scoped route planner's same-floor leg query: is `target` reachable on this floor when every
// openable door is treated as open, and which doors does the route cross?
//
// Two answers from one bounded BFS. `false` is the planner's "this needs floor hops" signal — and
// it is meaningful even when from.z == target.z, which is the case that motivated all of this
// (two pockets of one floor joined only via another floor). Running this is cheaper than asking
// zPlanFullRoute first, because that pays for two reachability floods before it can say the same
// thing, and this hands back the door list as a by-product.
//
// Shared across bots and cached on the (from,to) pair. The BFS is a pure function of those two
// tiles plus the map and current door states; it carries none of the per-bot coupling that keeps
// finished ROUTES uncacheable (portal blacklist, just-used-portal guard).
bool BotEngine::plannerLegDoors(const Position& from, const Position& target, std::vector<Position>& outDoors,
                                bool bypassCache) {
	outDoors.clear();
	if (from.z != target.z) {
		return false;
	}
	const uint64_t fk = botTileKey(from);
	const uint64_t tk = botTileKey(target);
	// Hash-combine, then verify from/to on hit — so a collision can never serve another leg's
	// doors, which would send a bot to open a door nowhere near its route.
	const uint64_t key = fk * 1000003ULL ^ (tk + 0x9e3779b97f4a7c15ULL + (fk << 6) + (fk >> 2));
	const int64_t now = OTSYS_TIME();

	auto it = s_doorPathCache.find(key);
	if (it != s_doorPathCache.end()) {
		if (bypassCache && it->second.from == from && it->second.to == target) {
			s_doorPathCache.erase(it); // stale by assumption — the walk it produced just failed
			it = s_doorPathCache.end();
		}
	}
	if (it != s_doorPathCache.end()) {
		if (it->second.expiry > now && it->second.from == from && it->second.to == target) {
			s_doorPathHits++;
			outDoors = it->second.doors;
			return it->second.reachable;
		}
		s_doorPathCache.erase(it); // expired, or a collision on another pair
	}
	s_doorPathMisses++;

	// Both house ids are pure functions of `from`/`target`, and the cache above re-verifies that
	// exact pair on every hit — so admitting a second house cannot make a cached answer apply to a
	// route it was not computed for.
	const uint32_t allowedHouse = zHouseIdAt(target.x, target.y, target.z);
	const uint32_t fromHouse = zHouseIdAt(from.x, from.y, from.z);
	const bool reachable = zFindDoorsOnPathImpl(from, target, PLANNER_LEG_RADIUS, PLANNER_LEG_BUDGET,
	                                            allowedHouse, fromHouse, outDoors);

	// Cheap bound: drop the whole table rather than tracking LRU. Entries are 60s-lived and this
	// runs for two rare callers, so a wipe costs one extra BFS for whoever asks next.
	if (s_doorPathCache.size() >= DOOR_PATH_CACHE_MAX) {
		s_doorPathCache.clear();
	}
	DoorPathEntry e;
	e.from = from;
	e.to = target;
	e.doors = outDoors;
	e.reachable = reachable;
	e.expiry = now + DOOR_PATH_TTL_MS;
	s_doorPathCache.emplace(key, std::move(e));
	return reachable;
}

// Kept for goToWithDoors' same-floor callers: the FIRST door on the route, or false if the route
// is clear. Now backed by the path reconstruction, so it can no longer point at a door off to the
// side that the bot was never going to walk past.
bool BotEngine::zFindBlockingDoor(const Position& from, const Position& target, Position& outDoor) {
	std::vector<Position> doors;
	if (!zFindDoorsOnPath(from, target, doors) || doors.empty()) {
		return false;
	}
	outDoor = doors.front();
	return true;
}

// ============================================================================
// Graph build
// ============================================================================

namespace {

// ---- Portal-graph disk cache --------------------------------------------------------------
//
// Rebuilding the graph costs ~21s (ZGRAPH ~3s + ZCOMP ~18s flood-filling 4M cells) and runs
// SYNCHRONOUSLY on the single dispatcher thread, so every /cavebot reload froze the whole server
// for ~22s — long enough for clients to time out and report "connection failed", which is
// indistinguishable from a crash. The graph is a pure function of the MAP, which does not change
// on reload, so persist it and reload it instead.
//
// Keyed on the map file's size+mtime: replacing the .otbm invalidates the cache automatically.
// If the map cannot be stat'd we neither read nor write a cache and simply rebuild — a stale
// graph would mis-route every bot, so "cannot verify" must mean "rebuild", never "assume valid".
constexpr uint32_t ZCACHE_MAGIC = 0x5A474331; // 'ZGC1'
// v2 appends the fishing-spot index (BOT_SUPPLY_REALISM). v3 dropped isolated islets; v4 adds
// the per-floor component reachability filter. v9 harvests fishing water via zCellIsClearWater
// instead of the raw ground-id test, so a ship — an item stacked ON water ground — is excluded at
// harvest instead of only in the live scan; every cache through v8 was built before that
// predicate existed and still hands out ship tiles on load. Bumping the version is what makes an
// existing cache rebuild rather than be read back with a stale/absent section.
//
// v10 appends the house-interior index (BOT_HOUSE_VISIT) and excludes water inside houses from the
// fishing harvest. Note the constant lives in this .so TU, so the one-off rebuild it forces fires
// on the first `/cavebot reload` after the deploy as well as on the restart — and that rebuild is
// SYNCHRONOUS on the dispatcher thread, i.e. a deliberate ~21s freeze of the whole server, not a
// deadlock. Expect fishing-spot counts to drop slightly on that rebuild: that is the house-water
// exclusion, not a regression.
// v11 stops the flood walking through WINDOWS. zFloodPassableAt's door escape keyed on
// ItemType::isDoor(), and Tibia types many solid, permanently-shut things as doors — a stone
// window (6445) is `type="door"` in items.xml. Because the SAME predicate builds the per-floor
// component labels below, windows have been silently merging components map-wide, which is why
// the planner could believe in same-floor routes that A* refuses and in floor hops that lead
// nowhere. Every cached graph through v10 was built with that flaw baked into its components, so
// the version must move or the fix would be invisible on any machine with a warm cache.
constexpr uint32_t ZCACHE_VERSION = 11;

std::string zCachePath() {
	return "libbot_engine.zgraph.cache";
}

// size+mtime of the loaded map, or false when it cannot be determined.
bool zMapFingerprint(int64_t& outSize, int64_t& outMtime) {
	const std::string mapName = g_configManager().getString(MAP_NAME);
	if (mapName.empty()) {
		return false;
	}
	const std::string dataDir = g_configManager().getString(DATA_DIRECTORY);
	// Canary composes the map path the same way in Map::load.
	const std::vector<std::string> candidates = {
		dataDir + "/world/" + mapName + ".otbm",
		"data-otservbr-global/world/" + mapName + ".otbm",
		mapName + ".otbm",
	};
	for (const auto& p : candidates) {
		std::error_code ec;
		const auto sz = std::filesystem::file_size(p, ec);
		if (ec) {
			continue;
		}
		const auto mt = std::filesystem::last_write_time(p, ec);
		if (ec) {
			continue;
		}
		outSize = static_cast<int64_t>(sz);
		outMtime = static_cast<int64_t>(mt.time_since_epoch().count());
		return true;
	}
	return false;
}

template <class T>
void zcWrite(std::ofstream& o, const T& v) {
	o.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T>
bool zcRead(std::ifstream& i, T& v) {
	return static_cast<bool>(i.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

} // namespace

// BOT_SUPPLY_REALISM: turn raw water cells into per-town fishing spots.
//
// A "spot" is a water tile plus a walkable, non-floor-change tile to stand on with clear line
// of sight to it — everything the fishing action needs (`fishing:allowFarUse(true)` permits
// <7,5> with LOS on the same floor, and we stand 1 tile away).
//
// Order matters for cost: GRID-THIN FIRST, then resolve stand tiles and assign towns. Town
// assignment is an O(towns) temple-distance scan per candidate and there are ~2M water cells,
// so doing it before thinning would be ~60M distance computations for an index we cap at
// botFishMaxSpotsPerTown entries per town.
void BotEngine::buildFishingSpotIndex(const std::vector<Position>& waterCells) {
	const int64_t t0 = botMonoMs();
	fishingSpots_.clear();
	zFishDropped_ = 0;
	if (waterCells.empty()) {
		return;
	}

	const int32_t perTownCap = std::max(1, static_cast<int32_t>(livenessCfg_.fishMaxSpotsPerTown));
	uint32_t noStandTile = 0, noTown = 0, islet = 0;

	// Reject stand tiles on isolated islets and ledges.
	//
	// zWalkableAt proves a tile is STANDABLE, not REACHABLE, and shoreline is exactly where the
	// two diverge — a sandbar or a ledge across the water is perfectly walkable and completely
	// cut off. Found in live testing: a bot sent to (33156,31903,7) near Edron walked to within
	// 39 tiles, then reported "started" with an empty walk queue every tick until the 4-minute
	// walk budget expired. `/cavebot route` confirmed *** NO PATH ***.
	//
	// A bounded flood is the cheap test: anything connected to real terrain reaches the cap
	// almost immediately, while an islet exhausts its own tiles first. This is not full
	// connectivity (a large disconnected island would still pass), but it removes the class of
	// failure that actually occurs, and the per-spot cost is trivial because the flood aborts
	// the moment it hits the cap.
	constexpr int32_t ISLET_MIN_TILES = 80;
	std::unordered_set<uint64_t> seen;
	std::vector<std::pair<int32_t, int32_t>> stack;
	auto standsOnRealTerrain = [&](const Position& p) {
		seen.clear();
		stack.clear();
		stack.push_back({ static_cast<int32_t>(p.x), static_cast<int32_t>(p.y) });
		seen.insert((static_cast<uint64_t>(p.x) << 20) | static_cast<uint64_t>(p.y));
		int32_t count = 0;
		while (!stack.empty() && count < ISLET_MIN_TILES) {
			auto [cx, cy] = stack.back();
			stack.pop_back();
			++count;
			for (int32_t dx = -1; dx <= 1; ++dx) {
				for (int32_t dy = -1; dy <= 1; ++dy) {
					if (dx == 0 && dy == 0) {
						continue;
					}
					const int32_t nx = cx + dx, ny = cy + dy;
					const uint64_t k = (static_cast<uint64_t>(nx) << 20) | static_cast<uint64_t>(ny);
					if (seen.count(k) || !zFloodPassableAt(nx, ny, p.z)) {
						continue;
					}
					seen.insert(k);
					stack.push_back({ nx, ny });
				}
			}
		}
		return count >= ISLET_MIN_TILES;
	};

	// VALIDATE EVERY CANDIDATE, THEN THIN. Doing it the other way round is what reduced Venore --
	// a canal city with water beside the depot -- to a single spot: one arbitrary representative
	// per 12x12 cell was chosen first, and in a canal that representative is usually the tile
	// whose only non-water neighbour is a building wall, so the whole cell died on the walkability
	// probe even though a street-side tile existed a few squares away. Validating first and
	// thinning the survivors keeps one GOOD spot per cell instead of gambling on one arbitrary
	// one. The sweep's shoreline pre-filter already cut the input from ~2M to ~135k, so the extra
	// probes are affordable.
	struct Cand { Position water, stand; };
	std::vector<Cand> valid;
	valid.reserve(waterCells.size() / 8 + 16);
	for (const auto& w : waterCells) {
		// A walkable neighbour to stand on. Cardinals first (a straight cast reads better than a
		// diagonal one), then diagonals.
		static const int32_t kDx[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
		static const int32_t kDy[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
		Position stand;
		bool found = false;
		for (int n = 0; n < 8 && !found; ++n) {
			const int32_t nx = static_cast<int32_t>(w.x) + kDx[n];
			const int32_t ny = static_cast<int32_t>(w.y) + kDy[n];
			if (!zWalkableAt(nx, ny, w.z)) {
				continue;
			}
			const Position cand(static_cast<uint16_t>(nx), static_cast<uint16_t>(ny), w.z);
			// zWalkableAt already rejects floor-change and teleport tiles, which is what keeps a
			// bot from parking on stairs to fish.
			if (!g_game().map.isSightClear(cand, w, true)) {
				continue;
			}
			stand = cand;
			found = true;
		}
		if (!found) {
			noStandTile++;
			continue;
		}
		valid.push_back({ w, stand });
	}

	// Thin the VALIDATED set so spots spread along a shore instead of clustering.
	constexpr int32_t GRID = 12;
	std::unordered_set<uint64_t> gridSeen;
	std::vector<Cand> thinned;
	thinned.reserve(valid.size() / 4 + 16);
	for (const auto& c : valid) {
		const uint64_t gk = (static_cast<uint64_t>(c.water.x / GRID) << 40)
			| (static_cast<uint64_t>(c.water.y / GRID) << 8) | static_cast<uint64_t>(c.water.z);
		if (!gridSeen.insert(gk).second) {
			continue;
		}
		thinned.push_back(c);
	}

	// Randomize before the per-town cap is applied. `thinned` is still in SECTOR SWEEP ORDER
	// (raster over sx, then sy) — a town with water on several sides (Thais: canal plus open sea)
	// would otherwise have its cap filled entirely by whichever side the sweep reaches first,
	// leaving the rest of that town's shoreline unindexed. One-time reload-time cost on an
	// already-thinned set, so a full shuffle is cheap; same idiom as bot_combat.cpp's candidate
	// shuffle.
	{
		auto rng = std::mt19937(std::random_device{}());
		std::shuffle(thinned.begin(), thinned.end(), rng);
	}

	for (const auto& cand : thinned) {
		const Position& w = cand.water;
		const Position& stand = cand.stand;
		if (!standsOnRealTerrain(stand)) {
			islet++;
			continue;
		}

		// Nearest temple wins the spot, same idiom buildNpcApproachAnchors uses. The z term is
		// weighted so a shore directly below a town does not get stolen by a closer-in-xy town
		// on another floor.
		uint32_t bestTown = 0;
		int32_t bestDist = INT32_MAX;
		for (const auto& [townId, town] : g_game().map.towns.getTowns()) {
			if (!town) {
				continue;
			}
			const auto tpos = town->getTemplePosition();
			const int32_t d = std::abs(static_cast<int32_t>(w.x) - static_cast<int32_t>(tpos.x))
				+ std::abs(static_cast<int32_t>(w.y) - static_cast<int32_t>(tpos.y))
				+ std::abs(static_cast<int32_t>(w.z) - static_cast<int32_t>(tpos.z)) * 10;
			if (d < bestDist) {
				bestDist = d;
				bestTown = townId;
			}
		}
		if (bestTown == 0) {
			noTown++;
			continue;
		}
		// Nearest-temple assignment alone lets a shore 300+ tiles out still "belong" to a town
		// (Carlin was collecting spots up near 31446, ~340 tiles from its temple). Those can
		// never be selected — botFishMaxDist is 150 — but they would consume the per-town cap
		// and crowd out the usable ones. Bound the index generously above that limit.
		constexpr int32_t MAX_SPOT_DIST_FROM_TOWN = 250;
		if (auto town = g_game().map.towns.getTown(bestTown)) {
			const auto tpos = town->getTemplePosition();
			const int32_t cheb = std::max(
				std::abs(static_cast<int32_t>(w.x) - static_cast<int32_t>(tpos.x)),
				std::abs(static_cast<int32_t>(w.y) - static_cast<int32_t>(tpos.y)));
			if (cheb > MAX_SPOT_DIST_FROM_TOWN) {
				noTown++;
				continue;
			}
		}
		auto& list = fishingSpots_[bestTown];
		if (static_cast<int32_t>(list.size()) >= perTownCap) {
			continue;
		}
		list.push_back(FishingSpot { w, stand, bestTown });
	}

	uint32_t total = 0;
	for (const auto& [t, v] : fishingSpots_) {
		total += static_cast<uint32_t>(v.size());
	}
	g_logger().info("[BotFish] indexed {} spots across {} towns (water cells={} "
	                "validated={} thinned={} noStandTile={} islet={} noTown={}) in {} ms",
	                total, fishingSpots_.size(), waterCells.size(), valid.size(), thinned.size(),
	                noStandTile, islet, noTown, botMonoMs() - t0);
}

// BOT_HOUSE_VISIT: bucket the sweep's raw house cells into houseInteriors_, keyed house -> floor.
//
// Kept deliberately dumb: no thinning, no reachability filtering, no town assignment. Unlike
// fishing spots — where a 2M-cell water set has to be reduced before the expensive per-candidate
// probes — a house interior is already tiny (~100-200 tiles), the whole point is to keep ALL of
// its idle tiles so the pick can be random, and every tile is by construction inside one small
// enclosed space. Everything selective happens later, per visit, in bot_house.cpp.
//
// townId and entry are filled in here from the live House object rather than the sweep: they are
// per-house facts, not per-tile ones, and Houses::getHouses() is a few hundred entries.
void BotEngine::buildHouseInteriorIndex(const std::vector<HouseCell>& cells) {
	const int64_t t0 = botMonoMs();
	houseInteriors_.clear();
	if (cells.empty()) {
		return;
	}

	for (const auto& hc : cells) {
		auto& interior = houseInteriors_[hc.houseId];
		auto& floor = interior.floors[hc.pos.z];
		if (hc.dummy) {
			floor.dummyTiles.push_back(hc.pos);
		} else if (hc.locker) {
			floor.lockerTiles.push_back(hc.pos);
		} else if (hc.walkable) {
			floor.idleTiles.push_back(hc.pos);
		}
	}

	uint32_t idle = 0, dummies = 0, lockers = 0;
	for (auto& [houseId, interior] : houseInteriors_) {
		if (const auto& house = g_game().map.houses.getHouse(houseId)) {
			interior.townId = house->getTownId();
			interior.entry = house->getEntryPosition();
		}
		for (const auto& [z, floor] : interior.floors) {
			idle += static_cast<uint32_t>(floor.idleTiles.size());
			dummies += static_cast<uint32_t>(floor.dummyTiles.size());
			lockers += static_cast<uint32_t>(floor.lockerTiles.size());
		}
	}

	g_logger().info("[HOUSE_INDEX] {} houses (cells={} idle={} dummies={} lockers={}) in {} ms",
	                houseInteriors_.size(), cells.size(), idle, dummies, lockers, botMonoMs() - t0);
}

// BOT_SUPPLY_REALISM: find the genuinely nearest CASTABLE water for a bot standing at `from`, and
// get it as close to the shoreline as walkability allows.
//
// Stand and cast tile are independent. fishing.lua's allowFarUse(true) lets Actions::canUseFar
// cast up to FISH_CAST_RANGE_X/Y tiles away with clear LOS (actions.cpp:211) — a bot does not have
// to touch the water to fish it, any more than a player standing on a dock does. So this ring-scans
// WALKABLE STAND CANDIDATES outward from the bot, starting at radius 0 (the bot's own tile), and
// for each ring of candidates — nearest first — collects every clear water tile inside ITS throw
// box via collectCastableWater. That EXISTENCE search stops at the first candidate that has any
// water — the ring radius r at which it stops is a hard cap on how far the bot ever walks to reach
// water AT ALL, which is what keeps this from wandering past close water toward something further
// out. That half of the design is UNCHANGED from the previous round.
//
// What changed: it used to also SETTLE for that first candidate, cast and all. r=0 is the bot's own
// tile, and if water happened to sit at the far edge of its throw box (7 tiles out) the bot fished
// from exactly where it was already standing — even with a tile 1 step from the water sitting right
// there. Now, once existence is proven at ring r0, zClosestStandForWater re-centres the search on
// the actual water tile found and ring-expands OUTWARD FROM THE WATER (bounded to its own <=7,5
// throw box — nothing further out could ever cast here, so this second pass is bounded regardless
// of maxRadius) for the walkable tile genuinely closest to it. That candidate is always at least as
// good as r0's own stand: r0's stand is itself a legal answer to the second search too (it is how
// `water` was found to begin with), so a LOS-budget cut-off or a shore with nothing closer degrades
// to exactly the old single-phase result, never worse.
//
// The invariant from the previous round — "a bot that already has castable water in range never
// walks PAST it toward water further away" — still holds exactly as before: the outward, bot-
// centred search still stops at the first ring with any hit, so a bot 140 tiles from anything still
// searches at most maxRadius rings and returns false, same as always. What is gone is the WEAKER
// invariant that a bot with water already in range never moves AT ALL — that one only ever held
// because the two searches used to be the same search. They are cleanly separable: "how far do I
// walk to reach water" (still bot-centred, still stops at the first hit — unbounded distances stay
// impossible) and "how close do I get to it once I'm there" (now water-centred, always <=7 tiles,
// run once per call, so the far case pays nothing extra for it).
//
// Ordering, spelled out: WATER FIRST, then MINIMISE THE STAND. Phase one's job is only to prove
// water is reachable at all and hand back ONE concrete tile to aim at — the water closest to the
// winning stand, by Chebyshev distance, out of everything in that stand's throw box. Phase two then
// finds the best stand for THAT tile. Enumerating every (stand, water) pair in the search area and
// ranking the lot was considered and rejected: it would pay for a full <=7,5 LOS scan from every
// ring candidate instead of stopping at the first, which is exactly the unbounded cost
// FISH_LOS_PROBE_BUDGET exists to prevent.
//
// The cast target is `water` itself now, not a uniform-random pick among the final stand's whole
// throw box: once the stand is chosen specifically to be close to that tile, casting somewhere else
// in its box would undo the point of choosing it. Diversity across bots still comes from the map —
// different bots standing at different positions find different nearest water — so this needs no
// randomisation of its own.
//
// SAME FLOOR ONLY, by construction: `from.z` is used throughout and never varies within one call.
// selectFishingSpot (bot_supply.cpp) is what tries neighbouring floors, by calling this function
// again with a z-shifted `from` — see that function's own header for why the cross-floor retry
// belongs in the caller, in strict priority order below the same-floor index, and not in here.
//
// The prebuilt index cannot answer either half of this: it grid-thins at 12x12, caps each town's
// spots in raw sweep order, and resolves "stand" as a tile ADJACENT to the water only, so a shore
// with no walkable tile touching the water at all (a raised bank, a rocky edge) has nothing
// indexed. The index stays useful as a coarse "this town has water, walk roughly there" hint for
// bots this function finds nothing for at all (selectFishingSpot's fallback).
bool BotEngine::findNearbyFishingSpot(const Position& from, int32_t maxRadius, FishingSpot& out) const {
	int32_t losBudget = FISH_LOS_PROBE_BUDGET;
	return findNearbyFishingSpot(from, maxRadius, out, losBudget);
}

// Shares `losBudget` with the caller instead of owning a fresh one — see the declaration in
// bot_engine_impl.hpp for why the portal-anchored cross-floor phase needs that. Identical
// algorithm to the 3-arg overload; only the budget's origin differs, so the commentary on the
// search itself lives on the header above rather than being duplicated here.
bool BotEngine::findNearbyFishingSpot(const Position& from, int32_t maxRadius, FishingSpot& out,
                                      int32_t& losBudget) const {
	std::vector<Position> castable;
	castable.reserve(16);

	std::vector<Position> ringStands;
	for (int32_t r = 0; r <= maxRadius; ++r) {
		// Perimeter of the Chebyshev ring at radius r around the BOT — r=0 is `from` itself.
		ringStands.clear();
		for (int32_t dx = -r; dx <= r; ++dx) {
			for (int32_t dy = -r; dy <= r; ++dy) {
				if (std::max(std::abs(dx), std::abs(dy)) != r) {
					continue; // interior already covered by a previous ring
				}
				const int32_t sx = static_cast<int32_t>(from.x) + dx;
				const int32_t sy = static_cast<int32_t>(from.y) + dy;
				if (zWalkableAt(sx, sy, from.z)) {
					ringStands.emplace_back(static_cast<uint16_t>(sx), static_cast<uint16_t>(sy), from.z);
				}
			}
		}
		if (ringStands.empty()) {
			continue;
		}
		// Nearest-first within the ring (Chebyshev ties the whole ring; this breaks the tie) —
		// same discipline the old water-first version used on its ring survivors.
		std::sort(ringStands.begin(), ringStands.end(), [&](const Position& a, const Position& b) {
			const int32_t da = std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(from.x))
				+ std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(from.y));
			const int32_t db = std::abs(static_cast<int32_t>(b.x) - static_cast<int32_t>(from.x))
				+ std::abs(static_cast<int32_t>(b.y) - static_cast<int32_t>(from.y));
			return da < db;
		});
		for (const auto& stand : ringStands) {
			collectCastableWater(stand, castable, losBudget);
			if (!castable.empty()) {
				// Existence proven at this ring. Anchor phase two on the water tile CLOSEST TO
				// THIS STAND (not necessarily closest to `from` — the two coincide almost always
				// in practice, since `stand` is itself the ring-nearest candidate to `from`, but
				// anchoring on the stand's own distance is what keeps phase two's improvement
				// monotonic; see zClosestStandForWater's contract).
				Position water = castable.front();
				int32_t bestWaterDist = botnav::zCheb(stand, water);
				for (const auto& w : castable) {
					const int32_t d = botnav::zCheb(stand, w);
					if (d < bestWaterDist) {
						bestWaterDist = d;
						water = w;
					}
				}
				out.stand = zClosestStandForWater(water, stand, losBudget);
				out.water = water;
				out.townId = 0; // local find — the town is whatever the bot is standing in
				return true;
			}
			if (losBudget <= 0) {
				// Materializing-probe ceiling hit with nothing found yet. Stop rather than keep
				// ringing outward for free — the index fallback (selectFishingSpot) picks this up.
				return false;
			}
		}
	}
	return false;
}

bool BotEngine::loadZGraphCache() {
	int64_t mapSize = 0, mapMtime = 0;
	if (!zMapFingerprint(mapSize, mapMtime)) {
		return false;
	}
	std::ifstream in(zCachePath(), std::ios::binary);
	if (!in) {
		return false;
	}
	uint32_t magic = 0, version = 0;
	int64_t cachedSize = 0, cachedMtime = 0;
	if (!zcRead(in, magic) || !zcRead(in, version) || !zcRead(in, cachedSize) || !zcRead(in, cachedMtime)) {
		return false;
	}
	if (magic != ZCACHE_MAGIC || version != ZCACHE_VERSION
		|| cachedSize != mapSize || cachedMtime != mapMtime) {
		return false; // different map, or a format change — rebuild
	}
	uint32_t nPortals = 0, nForced = 0, nComp = 0;
	if (!zcRead(in, nPortals)) {
		return false;
	}
	zGraph_.clear();
	for (uint32_t i = 0; i < nPortals; ++i) {
		botnav::ZPortal p;
		int32_t px = 0, py = 0, pz = 0, lx = 0, ly = 0, lz = 0;
		uint8_t kind = 0, down = 0, forced = 0;
		if (!zcRead(in, px) || !zcRead(in, py) || !zcRead(in, pz)
			|| !zcRead(in, lx) || !zcRead(in, ly) || !zcRead(in, lz)
			|| !zcRead(in, kind) || !zcRead(in, down) || !zcRead(in, forced)) {
			zGraph_.clear();
			return false;
		}
		p.pos = Position(static_cast<uint16_t>(px), static_cast<uint16_t>(py), static_cast<uint8_t>(pz));
		p.landing = Position(static_cast<uint16_t>(lx), static_cast<uint16_t>(ly), static_cast<uint8_t>(lz));
		p.kind = static_cast<botnav::ZPortalKind>(kind);
		p.goesDown = down != 0;
		p.landingForced = forced != 0;
		zGraph_.add(p);
	}
	zForcedLandings_.clear();
	if (!zcRead(in, nForced)) {
		zGraph_.clear();
		return false;
	}
	for (uint32_t i = 0; i < nForced; ++i) {
		uint64_t key = 0;
		if (!zcRead(in, key)) {
			zGraph_.clear();
			zForcedLandings_.clear();
			return false;
		}
		zForcedLandings_.insert(key);
	}
	zPortalComponent_.clear();
	if (!zcRead(in, nComp)) {
		zGraph_.clear();
		zForcedLandings_.clear();
		return false;
	}
	for (uint32_t i = 0; i < nComp; ++i) {
		uint64_t key = 0;
		uint32_t comp = 0;
		if (!zcRead(in, key) || !zcRead(in, comp)) {
			zGraph_.clear();
			zForcedLandings_.clear();
			zPortalComponent_.clear();
			return false;
		}
		zPortalComponent_[key] = comp;
	}
	// ---- v2: fishing-spot index ----
	// Same discipline as the sections above: any short read clears EVERY populated map and
	// returns false, so a truncated cache rebuilds from scratch rather than leaving the engine
	// half-loaded (a partial portal graph would mis-route every bot).
	fishingSpots_.clear();
	uint32_t nFishTowns = 0;
	auto bailFish = [&]() {
		zGraph_.clear();
		zForcedLandings_.clear();
		zPortalComponent_.clear();
		fishingSpots_.clear();
		return false;
	};
	if (!zcRead(in, nFishTowns)) {
		return bailFish();
	}
	for (uint32_t t = 0; t < nFishTowns; ++t) {
		uint32_t townId = 0, nSpots = 0;
		if (!zcRead(in, townId) || !zcRead(in, nSpots)) {
			return bailFish();
		}
		auto& list = fishingSpots_[townId];
		list.reserve(nSpots);
		for (uint32_t i = 0; i < nSpots; ++i) {
			int32_t wx = 0, wy = 0, wz = 0, sx = 0, sy = 0, sz = 0;
			if (!zcRead(in, wx) || !zcRead(in, wy) || !zcRead(in, wz)
				|| !zcRead(in, sx) || !zcRead(in, sy) || !zcRead(in, sz)) {
				return bailFish();
			}
			list.push_back(FishingSpot {
				Position(static_cast<uint16_t>(wx), static_cast<uint16_t>(wy), static_cast<uint8_t>(wz)),
				Position(static_cast<uint16_t>(sx), static_cast<uint16_t>(sy), static_cast<uint8_t>(sz)),
				townId });
		}
	}
	// ---- v10: house-interior index ----
	// Same all-or-nothing discipline; bailHouse clears the fishing section too.
	houseInteriors_.clear();
	auto bailHouse = [&]() {
		houseInteriors_.clear();
		return bailFish();
	};
	uint32_t nHouses = 0;
	if (!zcRead(in, nHouses)) {
		return bailHouse();
	}
	for (uint32_t h = 0; h < nHouses; ++h) {
		uint32_t houseId = 0, townId = 0, nFloors = 0;
		int32_t ex = 0, ey = 0, ez = 0;
		if (!zcRead(in, houseId) || !zcRead(in, townId)
			|| !zcRead(in, ex) || !zcRead(in, ey) || !zcRead(in, ez) || !zcRead(in, nFloors)) {
			return bailHouse();
		}
		auto& interior = houseInteriors_[houseId];
		interior.townId = townId;
		interior.entry = Position(static_cast<uint16_t>(ex), static_cast<uint16_t>(ey),
		                          static_cast<uint8_t>(ez));
		for (uint32_t f = 0; f < nFloors; ++f) {
			uint8_t z = 0;
			if (!zcRead(in, z)) {
				return bailHouse();
			}
			auto& floor = interior.floors[z];
			// idle, dummy, locker — same order as the writer.
			std::vector<Position>* lists[3] = { &floor.idleTiles, &floor.dummyTiles, &floor.lockerTiles };
			for (auto* list : lists) {
				uint32_t n = 0;
				if (!zcRead(in, n)) {
					return bailHouse();
				}
				list->reserve(n);
				for (uint32_t i = 0; i < n; ++i) {
					int32_t px = 0, py = 0;
					if (!zcRead(in, px) || !zcRead(in, py)) {
						return bailHouse();
					}
					list->push_back(Position(static_cast<uint16_t>(px), static_cast<uint16_t>(py), z));
				}
			}
		}
	}

	zGraph_.finalize();
	zGraphReady_ = !zGraph_.empty();
	if (zGraphReady_) {
		uint32_t total = 0;
		for (const auto& [t, v] : fishingSpots_) {
			total += static_cast<uint32_t>(v.size());
		}
		g_logger().info("[BotFish] loaded {} spots across {} towns from cache",
		                total, fishingSpots_.size());
		g_logger().info("[HOUSE_INDEX] loaded {} houses from cache", houseInteriors_.size());
	}
	return zGraphReady_;
}

void BotEngine::saveZGraphCache() {
	int64_t mapSize = 0, mapMtime = 0;
	if (!zMapFingerprint(mapSize, mapMtime) || zGraph_.empty()) {
		return;
	}
	// Write to a temp file and rename, so a crash mid-write cannot leave a truncated cache that
	// would then be read back as a valid-but-wrong graph.
	const std::string tmp = zCachePath() + ".tmp";
	{
		std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
		if (!o) {
			return;
		}
		zcWrite(o, ZCACHE_MAGIC);
		zcWrite(o, ZCACHE_VERSION);
		zcWrite(o, mapSize);
		zcWrite(o, mapMtime);
		const auto& portals = zGraph_.portals();
		zcWrite(o, static_cast<uint32_t>(portals.size()));
		for (const auto& p : portals) {
			zcWrite(o, static_cast<int32_t>(p.pos.x));
			zcWrite(o, static_cast<int32_t>(p.pos.y));
			zcWrite(o, static_cast<int32_t>(p.pos.z));
			zcWrite(o, static_cast<int32_t>(p.landing.x));
			zcWrite(o, static_cast<int32_t>(p.landing.y));
			zcWrite(o, static_cast<int32_t>(p.landing.z));
			zcWrite(o, static_cast<uint8_t>(p.kind));
			zcWrite(o, static_cast<uint8_t>(p.goesDown ? 1 : 0));
			zcWrite(o, static_cast<uint8_t>(p.landingForced ? 1 : 0));
		}
		zcWrite(o, static_cast<uint32_t>(zForcedLandings_.size()));
		for (const auto key : zForcedLandings_) {
			zcWrite(o, key);
		}
		zcWrite(o, static_cast<uint32_t>(zPortalComponent_.size()));
		for (const auto& [key, comp] : zPortalComponent_) {
			zcWrite(o, key);
			zcWrite(o, comp);
		}
		// ---- v2: fishing-spot index ----
		zcWrite(o, static_cast<uint32_t>(fishingSpots_.size()));
		for (const auto& [townId, spots] : fishingSpots_) {
			zcWrite(o, static_cast<uint32_t>(townId));
			zcWrite(o, static_cast<uint32_t>(spots.size()));
			for (const auto& s : spots) {
				zcWrite(o, static_cast<int32_t>(s.water.x));
				zcWrite(o, static_cast<int32_t>(s.water.y));
				zcWrite(o, static_cast<int32_t>(s.water.z));
				zcWrite(o, static_cast<int32_t>(s.stand.x));
				zcWrite(o, static_cast<int32_t>(s.stand.y));
				zcWrite(o, static_cast<int32_t>(s.stand.z));
			}
		}
		// ---- v10: house-interior index ----
		// z is written once per floor and the tiles carry only x/y, since a floor bucket is by
		// definition single-z. The three lists are written in a fixed order the reader mirrors.
		zcWrite(o, static_cast<uint32_t>(houseInteriors_.size()));
		for (const auto& [houseId, interior] : houseInteriors_) {
			zcWrite(o, static_cast<uint32_t>(houseId));
			zcWrite(o, static_cast<uint32_t>(interior.townId));
			zcWrite(o, static_cast<int32_t>(interior.entry.x));
			zcWrite(o, static_cast<int32_t>(interior.entry.y));
			zcWrite(o, static_cast<int32_t>(interior.entry.z));
			zcWrite(o, static_cast<uint32_t>(interior.floors.size()));
			for (const auto& [z, floor] : interior.floors) {
				zcWrite(o, static_cast<uint8_t>(z));
				const std::vector<Position>* lists[3] = { &floor.idleTiles, &floor.dummyTiles, &floor.lockerTiles };
				for (const auto* list : lists) {
					zcWrite(o, static_cast<uint32_t>(list->size()));
					for (const auto& p : *list) {
						zcWrite(o, static_cast<int32_t>(p.x));
						zcWrite(o, static_cast<int32_t>(p.y));
					}
				}
			}
		}
		if (!o) {
			return;
		}
	}
	std::error_code ec;
	std::filesystem::rename(tmp, zCachePath(), ec);
	if (ec) {
		std::filesystem::remove(tmp, ec);
	}
}

void BotEngine::buildZPortalGraph() {
	const int64_t t0 = botMonoMs();
	s_zPortalBlacklist.clear();
	s_plannedFc.clear();
	// Reuse the cached graph when the map has not changed. This is what keeps /cavebot reload
	// from freezing the server for ~22s.
	if (loadZGraphCache()) {
		g_logger().info("[ZGRAPH] loaded from cache: portals={} anchors={} in {} ms (map unchanged)",
		                zGraph_.size(), zPortalComponent_.size(), botMonoMs() - t0);
		return;
	}
	zGraph_.clear();
	zGraphReady_ = false;

	// Sector sweep over the plausible world extent. getMapSector is an
	// unordered_map find — absent sectors cost ~nothing. otservbr-global lives
	// well inside [30000,35000)²; the margin future-proofs custom areas.
	constexpr int32_t SWEEP_MIN = 24576, SWEEP_MAX = 40960;
	std::vector<botnav::ZPortal> cellPortals;
	uint32_t sectorsSeen = 0, cellsSeen = 0;
	// Raw water cells harvested during the sweep; turned into stand-tile spots afterwards so
	// the expensive per-candidate work (neighbour probes, town assignment) runs on a thinned
	// set rather than on ~2M tiles.
	std::vector<Position> waterCells;
	// Same idea for house interiors: collect raw classified cells here, bucket them into
	// houseInteriors_ afterwards. ~800 bot-owned houses of ~100-200 tiles each, so this stays
	// small next to waterCells.
	std::vector<HouseCell> houseCells;

	for (int32_t sx = SWEEP_MIN; sx < SWEEP_MAX; sx += SECTOR_SIZE) {
		for (int32_t sy = SWEEP_MIN; sy < SWEEP_MAX; sy += SECTOR_SIZE) {
			MapSector* sector = g_game().map.getMapSector(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
			if (!sector) {
				continue;
			}
			sectorsSeen++;
			for (uint8_t z = 0; z < MAP_MAX_LAYERS; ++z) {
				const auto& floor = sector->getFloor(z);
				if (!floor) {
					continue;
				}
				const auto& tiles = floor->getTiles();
				for (int32_t i = 0; i < SECTOR_SIZE; ++i) {
					for (int32_t j = 0; j < SECTOR_SIZE; ++j) {
						const auto& pairTC = tiles[i][j];
						if (!pairTC.first && !pairTC.second) {
							continue;
						}
						cellsSeen++;
						ZCell c { pairTC.first, pairTC.second };
						cellPortals.clear();
						zClassifyCell(sx + i, sy + j, z, c, cellPortals);
						for (const auto& p : cellPortals) {
							zGraph_.add(p);
						}
						// BOT_HOUSE_VISIT: harvest house interiors from this same sweep, for the
						// reason spelled out for fishing below — the sector walk is already paid
						// for, and a standalone Map::getTile pass would materialize tiles.
						// House membership is one field read (BasicTile::houseId) or one virtual
						// call (Tile::getHouse), and only cells that ARE house cells — a tiny
						// fraction of 18M — pay for the item-list classification.
						const uint32_t houseId = c.tile
							? (c.tile->getHouse() ? c.tile->getHouse()->getId() : 0)
							: (c.cached ? c.cached->houseId : 0);
						if (houseId != 0) {
							HouseCell hc;
							hc.houseId = houseId;
							hc.pos = Position(static_cast<uint16_t>(sx + i),
							                  static_cast<uint16_t>(sy + j), z);
							zClassifyHouseCell(c, hc.walkable, hc.dummy, hc.locker);
							if (hc.walkable || hc.dummy || hc.locker) {
								houseCells.push_back(hc);
							}
						}
						// BOT_SUPPLY_REALISM: harvest fishing spots from the SAME sweep. The
						// sector walk and the ground-id read are already paid for here; doing
						// this as a separate pass would mean either a second 18M-cell walk or
						// a Map::getTile sweep, and getTile MATERIALIZES tiles from the
						// BasicTile cache (see this file's header) — hundreds of MB of RAM for
						// an index we can collect for the cost of an id compare. zCellIsClearWater
						// (not the raw ground-id test) is what keeps a ship — an item stacked ON
						// water ground — out of the index; see that function's own comment.
						// Water INSIDE a house is not a fishing spot. It was harmless while bots
						// could not enter houses — the tile simply never became reachable — but
						// the moment house access lands, a private canal or dock would become a
						// live _fishing POI for any bot with botFishPct, independent of the house
						// feature's own roll, and hold it there for a whole casting session. The
						// PZ-roam transit into a house is accepted; a bot fishing in somebody's
						// living room for five minutes is not.
						if (houseId == 0 && zCellIsClearWater(c)) {
							// Keep only SHORELINE water — a tile with at least one non-water
							// cardinal neighbour. Without this pre-filter the grid-thinning below
							// picks an arbitrary representative per 12x12 cell, which for open
							// water and for wide canals is almost always a mid-water tile with no
							// walkable neighbour: measured 91% of thinned candidates discarded as
							// noStandTile, which reduced Venore (a canal city) to ONE usable spot
							// and left Ankrahmun with nothing above z15. Filtering to shore first
							// makes every thinned representative a viable candidate.
							bool isShore = false;
							static const int32_t kCx[4] = { 0, 0, -1, 1 };
							static const int32_t kCy[4] = { -1, 1, 0, 0 };
							for (int n = 0; n < 4 && !isShore; ++n) {
								const ZCell nc = zCellAt(sx + i + kCx[n], sy + j + kCy[n], z);
								if (!nc.exists()) {
									continue;
								}
								const uint16_t ngid = zGroundIdAt(nc);
								if (ngid != 0 && !isFishableWaterId(ngid)) {
									isShore = true;
								}
							}
							if (isShore) {
								waterCells.push_back(Position(static_cast<uint16_t>(sx + i),
								                             static_cast<uint16_t>(sy + j), z));
							}
						}
					}
				}
			}
		}
	}

	zGraph_.finalize();
	zGraphReady_ = !zGraph_.empty();
	g_logger().info("[ZGRAPH] portals={} sectors={} cells={} built in {} ms",
	                zGraph_.size(), sectorsSeen, cellsSeen, botMonoMs() - t0);

	buildFishingSpotIndex(waterCells);
	buildHouseInteriorIndex(houseCells);

	// Index the force-placed landings so legCost can gate them cheaply (set lookup first, the
	// tile probe only on a hit).
	zForcedLandings_.clear();
	for (const auto& p : zGraph_.portals()) {
		if (p.landingForced) {
			zForcedLandings_.insert(botTileKey(p.landing));
		}
	}

	// ---- Per-floor connected components, projected onto portal anchors only ----
	// This is what makes portal SELECTION correct: Dijkstra's interior edges become a true
	// connectivity test instead of a straight-line guess. Only floors that actually anchor a
	// portal get flooded, and the per-floor label map is transient, so peak memory is one
	// floor's walkable cells rather than the whole ~18M-cell map.
	const int64_t t1 = botMonoMs();
	zPortalComponent_.clear();
	bool hasPortalOnFloor[MAP_MAX_LAYERS] = {};
	// Position::z is uint8_t, so a bad landing z (an underflowed z-1 == 255, say) indexes
	// 239 bytes past this 16-byte array and silently corrupts the neighbouring local —
	// which is exactly the startup SIGSEGV ASan caught here. zClassifyCell no longer emits
	// such portals; this stays as the backstop so a future producer bug degrades a route
	// instead of smashing the stack.
	uint32_t droppedOutOfRange = 0;
	auto markFloor = [&](uint8_t fz) {
		if (fz < MAP_MAX_LAYERS) {
			hasPortalOnFloor[fz] = true;
		} else {
			++droppedOutOfRange;
		}
	};
	for (const auto& p : zGraph_.portals()) {
		markFloor(p.pos.z);
		markFloor(p.landing.z);
	}
	if (droppedOutOfRange > 0) {
		g_logger().error("[ZGRAPH] {} portal endpoint(s) had an out-of-range z (>= {}) — dropped from "
			"the per-floor component pass; this indicates a portal-producer bug",
			droppedOutOfRange, static_cast<int>(MAP_MAX_LAYERS));
	}
	auto key2d = [](int32_t kx, int32_t ky) {
		return (static_cast<uint32_t>(kx) << 16) | static_cast<uint32_t>(ky);
	};
	uint32_t labeledCells = 0, floorsLabeled = 0, componentsTotal = 0;
	for (uint8_t z = 0; z < MAP_MAX_LAYERS; ++z) {
		if (!hasPortalOnFloor[z]) {
			continue;
		}
		++floorsLabeled;
		std::unordered_map<uint32_t, uint32_t> label;
		uint32_t nextComponent = 0;
		std::vector<std::pair<int32_t, int32_t>> queue;
		for (int32_t sx = SWEEP_MIN; sx < SWEEP_MAX; sx += SECTOR_SIZE) {
			for (int32_t sy = SWEEP_MIN; sy < SWEEP_MAX; sy += SECTOR_SIZE) {
				MapSector* sector = g_game().map.getMapSector(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
				if (!sector || !sector->getFloor(z)) {
					continue;
				}
				for (int32_t i = 0; i < SECTOR_SIZE; ++i) {
					for (int32_t j = 0; j < SECTOR_SIZE; ++j) {
						const int32_t x = sx + i, y = sy + j;
						if (label.count(key2d(x, y)) || !zFloodPassableAt(x, y, z)) {
							continue;
						}
						const uint32_t compId = nextComponent++;
						label[key2d(x, y)] = compId;
						queue.clear();
						queue.push_back({ x, y });
						while (!queue.empty()) {
							auto [cx, cy] = queue.back();
							queue.pop_back();
							for (const auto& d : kNbrOffsets) {
								const int32_t nx = cx + d[0], ny = cy + d[1];
								if (label.count(key2d(nx, ny)) || !zFloodPassableAt(nx, ny, z)) {
									continue;
								}
								label[key2d(nx, ny)] = compId;
								queue.push_back({ nx, ny });
							}
						}
					}
				}
			}
		}
		for (const auto& p : zGraph_.portals()) {
			if (p.pos.z == z) {
				const int32_t cid = zLabelOf(label, p.pos.x, p.pos.y);
				if (cid >= 0) {
					zPortalComponent_[botTileKey(p.pos)] = static_cast<uint32_t>(cid);
				}
			}
			if (p.landing.z == z) {
				const int32_t cid = zLabelOf(label, p.landing.x, p.landing.y);
				if (cid >= 0) {
					zPortalComponent_[botTileKey(p.landing)] = static_cast<uint32_t>(cid);
				}
			}
		}
		// BOT_SUPPLY_REALISM: reachability filter for fishing spots, run HERE because this is the
		// only place the per-floor component labels exist (they are transient by design — see the
		// peak-memory note above). The bounded islet flood in buildFishingSpotIndex catches sand-
		// bars; this catches the bigger case it cannot: a stand tile on the far side of a bay,
		// walkable and part of real terrain but in a different connected component from the town.
		//
		// Kept when the spot's component either holds the owning town's temple (same floor: an
		// exact same-component test) or holds a portal anchor (other floors: it is at least
		// connected to the rest of the world, and the planner validates the actual route at run
		// time and gives up cleanly if there is none).
		{
			// Reference components per town = where that town's POIs actually are on THIS floor.
			// The temple position alone is not a usable reference: Edron's temple is on z8 while
			// its shoreline is on z7, so a temple-only test left every Edron spot falling through
			// to the weak anchor branch — which is how (33156,31903,7), the tile that started
			// this, survived two earlier filters. cityPOIs_ holds the depot/temple/boat/shop
			// positions bots genuinely walk to, so "same component as a POI of this town on this
			// floor" is exactly the property we need.
			std::unordered_map<uint32_t, std::unordered_set<int32_t>> refCompByTown;
			for (const auto& [tid, pois] : cityPOIs_) {
				for (const auto& poi : pois) {
					if (poi.pos.z != z) {
						continue;
					}
					const int32_t cid = zLabelOf(label, poi.pos.x, poi.pos.y);
					if (cid >= 0) {
						refCompByTown[tid].insert(cid);
					}
				}
			}
			for (const auto& [tid, town] : g_game().map.towns.getTowns()) {
				if (!town) {
					continue;
				}
				const auto tp = town->getTemplePosition();
				if (tp.z != z) {
					continue;
				}
				const int32_t cid = zLabelOf(label, tp.x, tp.y);
				if (cid >= 0) {
					refCompByTown[tid].insert(cid);
				}
			}
			std::unordered_set<int32_t> anchoredComp;
			for (const auto& p : zGraph_.portals()) {
				if (p.pos.z == z) {
					const int32_t c = zLabelOf(label, p.pos.x, p.pos.y);
					if (c >= 0) {
						anchoredComp.insert(c);
					}
				}
				if (p.landing.z == z) {
					const int32_t c = zLabelOf(label, p.landing.x, p.landing.y);
					if (c >= 0) {
						anchoredComp.insert(c);
					}
				}
			}
			for (auto& [townId, spots] : fishingSpots_) {
				auto tIt = refCompByTown.find(townId);
				const size_t before = spots.size();
				std::erase_if(spots, [&](const FishingSpot& s) {
					if (s.stand.z != z) {
						return false; // not this floor's problem
					}
					const int32_t c = zLabelOf(label, s.stand.x, s.stand.y);
					if (c < 0) {
						return true; // not floodable at all
					}
					// Keep when EITHER the component holds one of this town's POIs (plain
					// same-floor walk) OR it holds a portal anchor (enterable by a floor change).
					// The second arm is not a fallback — it is the Venore case: that city's canal
					// shore is on z7 but in a component you can only enter by going down stairs or
					// a trapdoor, so a POI-only test dropped all of it and left Venore with two
					// spots. Reaching it is exactly what the portal graph and the planner's tier 3
					// exist for, and a route that turns out not to exist still degrades safely
					// through the normal pathFailCount give-up.
					if (anchoredComp.find(c) != anchoredComp.end()) {
						return false;
					}
					if (tIt != refCompByTown.end() && !tIt->second.empty()) {
						return tIt->second.find(c) == tIt->second.end();
					}
					return true; // no POI on this floor and no way in — genuinely stranded
				});
				zFishDropped_ += static_cast<uint32_t>(before - spots.size());
			}
		}

		labeledCells += static_cast<uint32_t>(label.size());
		componentsTotal += nextComponent;
	}
	g_logger().info("[ZCOMP] floors={} components={} anchors={} labeledCells={} in {} ms",
	                floorsLabeled, componentsTotal, zPortalComponent_.size(), labeledCells, botMonoMs() - t1);
	{
		uint32_t kept = 0;
		for (const auto& [t, v] : fishingSpots_) {
			kept += static_cast<uint32_t>(v.size());
		}
		g_logger().info("[BotFish] reachability filter: dropped {} unreachable spot(s), {} remain "
		                "across {} town(s)", zFishDropped_, kept, fishingSpots_.size());
	}
	// Persist so the next reload (and the next restart) skips all of the above.
	saveZGraphCache();
}

// ============================================================================
// Planner + blacklist
// ============================================================================

bool BotEngine::zPortalBlacklisted(const Position& portalPos) {
	auto it = s_zPortalBlacklist.find(botTileKey(portalPos));
	if (it == s_zPortalBlacklist.end()) {
		return false;
	}
	if (OTSYS_TIME() >= it->second) {
		s_zPortalBlacklist.erase(it);
		return false;
	}
	return true;
}

void BotEngine::zBlacklistPortal(const Position& portalPos, const char* site) {
	s_zPortalBlacklist[botTileKey(portalPos)] = OTSYS_TIME() + Z_BLACKLIST_MS;
	// Quarantining a portal can delete the ONLY route between two areas, so every entry is worth
	// seeing. `site` names the call site so a spurious-blacklist source is identifiable from the
	// journal alone.
	g_logger().warn("[ZBLACKLIST] portal=({},{},{}) site={} total={}",
	                portalPos.x, portalPos.y, portalPos.z, site, s_zPortalBlacklist.size());
}

// ---- Failed-z-plan negative cache (2026-08-21) ----
//
// A FAILING z-plan is the expensive one, and it repeats. Live capture during an operator
// walkaround into Venore:
//
//   [ZPLAN_SLOW] 2313ms (32958,32053,6)->(32911,32081,10) hops=0 targetFlood=18
//   [ZPLAN_SLOW] 3096ms (32958,32053,6)->(32911,32081,10) hops=0 targetFlood=18
//   [ZPLAN_SLOW] 3545ms (32960,32060,7)->(32911,32081,10) hops=0 targetFlood=18
//   [TICK_SLOW]  body=6001ms
//
// Same unreachable destination, re-planned every tick by two bots, producing 5-6 SECOND tick
// bodies -- a hard freeze for anyone online. targetFlood=18 says the destination is an 18-cell
// enclosed pocket; there is no route and there never will be, but nothing remembered that.
//
// Why failures cost seconds while successes do not: interior legs are O(1) component compares,
// but an UNLABELLED portal pays a fresh, deliberately uncached LocalReach flood per leg
// evaluation -- and a failing Dijkstra exhausts the whole graph, so it prices the maximum number
// of legs. Capping Z_END_LEG_BUDGET is NOT the fix: both floods above finished far under it
// (4001 and 18 against a 30000 budget), so they stopped on geometry, and lowering the budget
// re-introduces the documented "48-tile Thais leg wrongly unreachable" bug.
//
// Two inputs make a naive cache actively harmful, so both are handled explicitly:
//
//   justUsed  -- the per-BOT anti-ping-pong guard (Z_LAST_PORTAL_GUARD_MS, 60s) is applied in
//                BOTH passes. A bot whose only route runs back through the staircase it just
//                used fails for a reason personal and temporary to it. Caching that globally
//                would strand every other bot heading to the same place. So: never insert while
//                justUsed is set.
//   forceGraph -- poisons the direct from->target leg (see legCost). A forceGraph=true failure
//                says nothing about forceGraph=false, so it is part of the key, not ignored.
//
// Insertion also waits until the blacklist-free second pass has failed (or been proven
// redundant), which makes a cached verdict independent of blacklist state -- so blacklist expiry
// never has to invalidate anything.
//
// File-scope static, not inline: this is used only by zPlanFullRoute in this one TU, so a
// per-TU copy is not merely harmless, it is the whole scope. It also means the cache dies with
// the engine on every reload, which is exactly the invalidation the portal graph needs (the
// graph is built once per engine load and there is no generation counter to key against).
namespace {

// Source is quantized because the failing bot drifts between attempts -- (32958,32053) and
// (32960,32060) above are the same bot two seconds apart. Without quantization each step mints a
// fresh cache miss and the spin continues. z is kept RAW: reachability changes completely
// between floors, and the evidence spans z=6 and z=7 for one destination.
constexpr int32_t Z_NEG_SRC_QUANTUM = 24;
constexpr int64_t Z_NEG_TTL_MS = 90 * 1000;
constexpr size_t Z_NEG_CACHE_MAX = 256;

struct ZNegEntry {
	uint64_t srcKey = 0;    // stored so a hash collision is detected rather than acted on
	uint64_t tgtKey = 0;
	bool forceGraph = false;
	int64_t expiresAt = 0;
	// 0 = global verdict (unreachable for everyone). Non-zero = this bot only, because the
	// failure was caused by its own justUsed guard. Verified on lookup: a per-bot entry must
	// never suppress another bot's plan, which would be the stranding bug this design avoids.
	uint32_t guid = 0;
};

std::unordered_map<uint64_t, ZNegEntry> s_zNegCache;
uint64_t s_zNegHits = 0;
int64_t s_zNegLastLogMs = 0;

inline uint64_t zNegSrcKey(const Position& from) {
	const int32_t qx = (static_cast<int32_t>(from.x) / Z_NEG_SRC_QUANTUM) * Z_NEG_SRC_QUANTUM;
	const int32_t qy = (static_cast<int32_t>(from.y) / Z_NEG_SRC_QUANTUM) * Z_NEG_SRC_QUANTUM;
	return (static_cast<uint64_t>(qx) << 24) ^ (static_cast<uint64_t>(qy) << 8)
		^ static_cast<uint64_t>(from.z);
}

inline uint64_t zNegHash(uint64_t srcKey, uint64_t tgtKey, bool forceGraph) {
	return srcKey * 1000003ull ^ (tgtKey + 0x9e3779b97f4a7c15ull)
		^ (forceGraph ? 0x5bf03635ull : 0ull);
}

} // namespace

bool BotEngine::zPlanFullRoute(const Position& from, const Position& target, std::vector<botnav::ZRouteHop>& hops, bool forceGraph, uint32_t forGuid) {
	if (!zGraphReady_ || zGraph_.empty()) {
		return false;
	}
	botnav::ZPlanParams pp;
	pp.legMax = Z_LEG_MAX;
	pp.maxHops = Z_MAX_HOPS;
	// Optimistic leg model: Chebyshev distance. The runtime FC machine's A*
	// validates each leg; failures feed back via the portal blacklist.
	//
	// forceGraph: the caller has ALREADY proven the direct from->target leg unwalkable (its
	// same-floor A* failed), so report exactly that pair as unreachable. planZRoute then falls
	// through to the portal graph instead of answering "no hop needed", which is what lets a
	// bot route z6 -> z7 -> z6 around an obstruction it cannot walk through on one floor.
	// Only the direct pair is poisoned; portal-to-portal legs keep the optimistic estimate.
	// Reachability-aware leg model. The old version returned 10*Chebyshev unconditionally, so
	// every leg looked walkable and the planner picked whichever portal was physically nearest —
	// proven wrong live: from the Thais temple it chose stairs 7 tiles away whose landing sits 65
	// tiles from the boat in a DISCONNECTED part of z=6, over the ladder 48 tiles away that the
	// authored city route uses and that actually connects.
	//
	// Cost VALUES are unchanged (10*Chebyshev) for reachable legs — only the reachability GATE
	// is new, so Dijkstra's cost scale and route preferences are otherwise identical.
	//
	//   interior portal->portal : O(1) precomputed component equality (build-time flood fill)
	//   the two END legs       : one bounded local BFS each, lazily computed and memoized
	//   unlabelled anchor      : ad-hoc bounded BFS — never a silent "assume reachable"
	// Both end-leg floods go through the SHARED cache: a flood depends only on (origin, map), not
	// on which bot asked, so the first bot to leave a temple pays the ~30ms and every bot after it
	// reuses the result. The ad-hoc unlabelled-anchor flood below now shares the SAME cache: its
	// origins are portal positions, i.e. fixed graph nodes, not the one-off origins once claimed.
	// ---- Tier 3-5 tick deadline (see Z_PLAN_TICK_BUDGET_MS) ----
	// Checked BEFORE the negative-cache probe so a deferral never touches the cache, and before
	// the floods so it costs nothing. A DEFER is emphatically NOT a failure: it must not record a
	// negative (the pair may be perfectly reachable), must not count as a plan failure, and must
	// not blacklist anything. Callers fall back to the same path they already take when no graph
	// route exists -- a well-exercised route that keeps the bot moving -- and the plan is
	// attempted again next tick.
	//
	// forGuid == 0 is an admin/debug call (/cavebot zplan, route): never deferred, because an
	// operator waiting on a command is not tier 3-5 work.
	if (forGuid != 0 && s_tickBodyStartMs > 0
	    && botMonoMs() - s_tickBodyStartMs > Z_PLAN_TICK_BUDGET_MS) {
		s_zPlanDeferrals++;
		auto& st = s_zPlanDeferStreak[forGuid];
		st.count++;
		st.lastMs = OTSYS_TIME();
		if (st.count > s_zPlanDeferWorstStreak) {
			s_zPlanDeferWorstStreak = st.count;
			s_zPlanDeferWorstGuid = forGuid;
		}
		return false;
	}
	if (forGuid != 0) {
		// Got through the gate: this bot is not being starved.
		s_zPlanDeferStreak.erase(forGuid);
	}

	// Negative-cache probe. Must sit BEFORE the LocalReach constructions below -- those are the
	// floods whose cost we are avoiding.
	const uint64_t negSrc = zNegSrcKey(from);
	const uint64_t negTgt = botTileKey(target);
	const uint64_t negHash = zNegHash(negSrc, negTgt, forceGraph);
	// Two entries can answer this plan: a global verdict, and a per-bot one recorded while this
	// bot's justUsed guard was active. Probe both -- checking only the global hash would make
	// every per-bot entry write-only.
	const uint64_t negHashBot = forGuid ? (negHash ^ (forGuid * 0x9e3779b1ull)) : negHash;
	const int64_t negNow = OTSYS_TIME();
	for (const uint64_t probe : { negHash, negHashBot }) {
		auto it = s_zNegCache.find(probe);
		if (it == s_zNegCache.end()) {
			continue;
		}
		{
		const ZNegEntry& e = it->second;
		if (e.expiresAt <= negNow) {
			s_zNegCache.erase(it);
		} else if (e.srcKey == negSrc && e.tgtKey == negTgt && e.forceGraph == forceGraph
		           && (e.guid == 0 || e.guid == forGuid)) {
			// Verified match, not just a hash hit -- a collision must never strand a bot.
			s_zNegHits++;
			if (negNow - s_zNegLastLogMs > 30000) {
				s_zNegLastLogMs = negNow;
				g_logger().info("[ZPLAN_NEGCACHE] {} suppressed re-plans, {} entries "
					"(latest ({},{},{})->({},{},{}))",
					s_zNegHits, s_zNegCache.size(), from.x, from.y, from.z,
					target.x, target.y, target.z);
			}
			return false;
			}
		}
	}

	LocalReach startReach(from, Z_LEG_MAX, Z_END_LEG_BUDGET, &zReachCache_, &zReachHits_,
	                      &zReachMisses_, Z_REACH_TTL_MS, Z_REACH_CACHE_MAX);
	LocalReach targetReach(target, Z_LEG_MAX, Z_END_LEG_BUDGET, &zReachCache_, &zReachHits_,
	                       &zReachMisses_, Z_REACH_TTL_MS, Z_REACH_CACHE_MAX);
	auto legCost = [&, forceGraph](const Position& a, const Position& b) -> int32_t {
		if (forceGraph && a == from && b == target) {
			return -1;
		}
		// Force-placed landings must be judged on their OWN tile. Everything below
		// (component labels, LocalReach) falls back to an 8-neighbour lookup when a tile has no
		// direct label — fine for a stair you stand beside, wrong for an unvalidated landing,
		// which would be called reachable purely because something next to it is.
		if (!zForcedLandings_.empty()) {
			if (zForcedLandings_.count(botTileKey(a)) && !zFloodPassableAt(a.x, a.y, a.z)) {
				return -1;
			}
			if (zForcedLandings_.count(botTileKey(b)) && !zFloodPassableAt(b.x, b.y, b.z)) {
				return -1;
			}
		}
		// Single exit point so the trace can name the branch that priced this leg.
		const char* why = "?";
		int32_t cost = -1;
		if (a == from) {
			why = "startReach";
			cost = startReach.reachable(b) ? 10 * botnav::zCheb(a, b) : -1;
		} else if (b == target) {
			why = "targetReach";
			cost = targetReach.reachable(a) ? 10 * botnav::zCheb(a, b) : -1;
		} else {
			auto ca = zPortalComponent_.find(botTileKey(a));
			auto cb = zPortalComponent_.find(botTileKey(b));
			if (ca != zPortalComponent_.end() && cb != zPortalComponent_.end()) {
				why = ca->second == cb->second ? "component=" : "component!=";
				cost = ca->second == cb->second ? 10 * botnav::zCheb(a, b) : -1;
			} else {
				why = ca == zPortalComponent_.end() && cb == zPortalComponent_.end()
					? "adhoc(a,b unlabelled)"
					: (ca == zPortalComponent_.end() ? "adhoc(a unlabelled)" : "adhoc(b unlabelled)");
				// Shared cache, NOT a private flood. The old comment called these origins
				// "one-off" and that is simply wrong: `a` is a PORTAL POSITION, a fixed node in
				// the graph, which recurs in every plan that touches this region. Flooding it
				// fresh each time was the dominant cost of the whole planner.
				//
				// Measured over 20h: slow plans were 69% SUCCESSES, not failures, and hops=2 had
				// the worst mean (937ms) despite only 19 events. Both follow from this line --
				// planZRoute is plain Dijkstra with a 4096-portal settle budget, so a deeper goal
				// settles more portals, and every unlabelled one paid its own up-to-30k-cell
				// flood. The budget caps expansions, never the cost per expansion.
				LocalReach adhoc(a, Z_LEG_MAX, Z_END_LEG_BUDGET, &zReachCache_, &zReachHits_,
				                 &zReachMisses_, Z_REACH_TTL_MS, Z_REACH_CACHE_MAX);
				cost = adhoc.reachable(b) ? 10 * botnav::zCheb(a, b) : -1;
			}
		}
		if (zPlanTrace_) {
			// Enough context to tell a component MISMATCH from a MISSING label: a portal tile that
			// is not itself flood-passable (every STAIRS/HOLE tile) got its component id from
			// zLabelOf's 8-neighbour fallback, which picks the first labelled neighbour in a fixed
			// order and can therefore borrow a DISCONNECTED pocket's id.
			auto ca = zPortalComponent_.find(botTileKey(a));
			auto cb = zPortalComponent_.find(botTileKey(b));
			g_logger().info(
				"[ZLEGCOST] ({},{},{})->({},{},{}) cheb={} cost={} via {} | a:comp={} pass={} forced={} bl={}"
				" | b:comp={} pass={} forced={} bl={}",
				a.x, a.y, a.z, b.x, b.y, b.z, botnav::zCheb(a, b), cost, why,
				ca == zPortalComponent_.end() ? -1 : static_cast<int64_t>(ca->second),
				zFloodPassableAt(a.x, a.y, a.z) ? "Y" : "N",
				zForcedLandings_.count(botTileKey(a)) ? "Y" : "N",
				zPortalBlacklisted(a) ? "Y" : "N",
				cb == zPortalComponent_.end() ? -1 : static_cast<int64_t>(cb->second),
				zFloodPassableAt(b.x, b.y, b.z) ? "Y" : "N",
				zForcedLandings_.count(botTileKey(b)) ? "Y" : "N",
				zPortalBlacklisted(b) ? "Y" : "N");
		}
		return cost;
	};
	// Loop guard: never re-take the portal this bot just came through. Without it the
	// optimistic leg model happily plans "go back down the stairs you just climbed", and the
	// bot ping-pongs across one staircase forever.
	uint64_t justUsed = 0;
	int64_t justUsedExpiry = 0;   // when the per-bot guard lapses; caps the per-bot negative below
	if (forGuid != 0) {
		auto lu = s_zLastPortalUsed.find(forGuid);
		if (lu != s_zLastPortalUsed.end()) {
			if (OTSYS_TIME() < lu->second.second) {
				justUsed = lu->second.first;
				justUsedExpiry = lu->second.second;
			} else {
				s_zLastPortalUsed.erase(lu);
			}
		}
	}
	// Count how many portals pass 1 actually rejected FOR BEING BLACKLISTED. If that is zero, the
	// blacklist-free retry below is bit-identical to this pass and is guaranteed to fail too --
	// and on a failing plan that retry re-prices every unlabelled portal, each of which pays a
	// fresh uncached flood. Exact test, not a heuristic: skipping only when nothing was excluded
	// cannot change any outcome.
	uint32_t blacklistExclusions = 0;
	auto exclude = [this, justUsed, &blacklistExclusions](const botnav::ZPortal& p) {
		if (justUsed != 0 && botTileKey(p.pos) == justUsed) {
			return true;
		}
		if (zPortalBlacklisted(p.pos)) {
			blacklistExclusions++;
			return true;
		}
		return false;
	};
	const int64_t planStart = botMonoMs();
	// Mid-search deadline. The entry gate above stops a plan from STARTING late; this stops one
	// already running from holding the tick, which the entry gate provably cannot do -- measured
	// live: a plan that began under the entry budget then ran 3284ms.
	//
	// An abort is not a failure. It leaves outAborted set, and everything below that records a
	// verdict (both negative caches) or counts a failure is skipped, because the pair may be
	// perfectly routable and we simply ran out of time. The floods it did compute stay in
	// zReachCache_, so next tick's retry resumes cheaply instead of starting over.
	bool planAborted = false;
	if (forGuid != 0) {
		pp.deadlineMonoMs = botnav::zSteadyMs() + Z_PLAN_SEARCH_BUDGET_MS;
		pp.outAborted = &planAborted;
	}
	bool ok = botnav::planZRoute(zGraph_, from, target, hops, pp, legCost, exclude);
	// A blacklisted portal must never make an otherwise-viable route UNPLANNABLE. Many routes are
	// a unique chain of portals (proven live: (32345,32265,5)->(32350,32225,5) is the only way
	// between two pockets of z=5, via z6->z7->z6), so excluding one link does not steer the bot
	// around it — it deletes the only route and the bot gives up entirely for Z_BLACKLIST_MS.
	//
	// So: on failure, retry once WITHOUT the blacklist. Trying a portal that failed before beats
	// doing nothing, and a blacklist entry is often spurious anyway (see the retry gating at the
	// blacklist call sites). The justUsed anti-ping-pong guard is deliberately KEPT on both passes
	// — it is a different, tick-scoped signal ("this bot just came through here"), and it is what
	// prevents the z6<->z7 same-staircase oscillation that caused the earlier revert. Lifting only
	// the blacklist half therefore cannot resurrect that bug.
	if (!ok && !planAborted && !s_zPortalBlacklist.empty() && blacklistExclusions > 0) {
		auto excludeJustUsedOnly = [justUsed](const botnav::ZPortal& p) {
			return justUsed != 0 && botTileKey(p.pos) == justUsed;
		};
		std::vector<botnav::ZRouteHop> hops2;
		if (botnav::planZRoute(zGraph_, from, target, hops2, pp, legCost, excludeJustUsedOnly)) {
			hops = std::move(hops2);
			ok = true;
			g_logger().warn("[ZPLAN_RECOVER] ({},{},{})->({},{},{}) routable only after ignoring {} "
			                "blacklisted portal(s)",
			                from.x, from.y, from.z, target.x, target.y, target.z,
			                s_zPortalBlacklist.size());
		}
	}
	// Record a confirmed-unreachable pair so the next tick does not pay for the same discovery.
	//
	// Gated on justUsed == 0: that guard is per-BOT and expires (Z_LAST_PORTAL_GUARD_MS), so a
	// failure caused by it is personal and temporary to this bot. Caching it globally would
	// strand every other bot heading to the same destination -- the one way this optimisation
	// could turn into a stuck-bot bug.
	//
	// By this point the blacklist-free pass has either run and failed, or been proven redundant,
	// so the verdict does not depend on blacklist state and needs no invalidation when it expires.
	// justUsed != 0 blocks the GLOBAL insert above for a sound reason, but on its own it left the
	// commonest spin uncached: s_zLastPortalUsed is re-stamped for 60s on every SUCCESSFUL planned
	// hop, so a bot making intermittent progress -- hop succeeds, next plan fails, repeat -- is
	// permanently inside a fresh guard window and never qualifies. Live evidence: one pair,
	// (32227,31150,7)->(32214,31134,8), replanned 43 times in an hour at ~550ms each, never cached.
	//
	// So record it PER BOT instead, and cap the entry at the guard's own expiry. The negative then
	// cannot outlive the transient condition that produced it: the moment the guard lapses the
	// plan is allowed again, and if it still fails with justUsed == 0 it becomes a global entry.
	// This is also the cross-walk throttle the goTo path never had -- bot_nav.cpp's caller never
	// increments s_fcConsecutiveFailures, so nothing bounded its re-planning before.
	if (!ok && !planAborted && justUsed != 0 && forGuid != 0 && justUsedExpiry > 0) {
		const int64_t nowMs = OTSYS_TIME();
		const int64_t expiry = std::min(nowMs + Z_NEG_TTL_MS, justUsedExpiry);
		if (expiry > nowMs) {
			if (s_zNegCache.size() >= Z_NEG_CACHE_MAX) {
				std::erase_if(s_zNegCache, [nowMs](const auto& kv) { return kv.second.expiresAt <= nowMs; });
				if (s_zNegCache.size() >= Z_NEG_CACHE_MAX) {
					s_zNegCache.clear();
				}
			}
			const uint64_t perBotHash = zNegHash(negSrc, negTgt, forceGraph) ^ (forGuid * 0x9e3779b1ull);
			s_zNegCache[perBotHash] = ZNegEntry { negSrc, negTgt, forceGraph, expiry, forGuid };
		}
	}
	if (!ok && !planAborted && justUsed == 0) {
		if (s_zNegCache.size() >= Z_NEG_CACHE_MAX) {
			// Cheap reclaim: drop expired entries first, and if none are expired, clear. This is
			// a miss-cost optimisation, not a correctness structure -- losing it costs one slow
			// plan, so an LRU is not worth the bookkeeping.
			const int64_t nowMs = OTSYS_TIME();
			std::erase_if(s_zNegCache, [nowMs](const auto& kv) { return kv.second.expiresAt <= nowMs; });
			if (s_zNegCache.size() >= Z_NEG_CACHE_MAX) {
				s_zNegCache.clear();
			}
		}
		s_zNegCache[negHash] = ZNegEntry { negSrc, negTgt, forceGraph, OTSYS_TIME() + Z_NEG_TTL_MS, 0 };
	}

	if (planAborted) {
		s_zPlanDeferrals++;
		auto& st = s_zPlanDeferStreak[forGuid];
		st.count++;
		st.lastMs = OTSYS_TIME();
		if (st.count > s_zPlanDeferWorstStreak) {
			s_zPlanDeferWorstStreak = st.count;
			s_zPlanDeferWorstGuid = forGuid;
		}
	}
	const int64_t planMs = botMonoMs() - planStart;
	if (planMs >= Z_PLAN_SLOW_MS) {
		// The end-leg floods are the only unbounded-ish cost here; report their actual size so
		// Z_END_LEG_BUDGET can be tuned from evidence rather than guessed at again.
		g_logger().warn("[ZPLAN_SLOW]{} {}ms ({},{},{})->({},{},{}) hops={} startFlood={} targetFlood={}",
			planAborted ? " ABORTED" : "",
			planMs, from.x, from.y, from.z, target.x, target.y, target.z,
			hops.size(), startReach.cellsVisited(), targetReach.cellsVisited());
	}
	return ok;
}

bool BotEngine::zPlanNextHop(BotState& bot, const Position& target, ZPlannedHop& out, bool forceGraph) {
	std::vector<botnav::ZRouteHop> hops;
	if (!zPlanFullRoute(bot.currentPos, target, hops, forceGraph, bot.guid) || hops.empty()) {
		s_zPlanFail++;
		return false;
	}
	s_zPlanOk++;
	out.portal = hops.front().portal;
	out.plannedAt = OTSYS_TIME();
	castLog(bot, fmt::format("ZROUTE: {} hop(s) to ({},{},{}) — next: {} {} at ({},{},{}) lands ({},{},{})",
		hops.size(), target.x, target.y, target.z,
		botnav::zPortalKindName(out.portal.kind), out.portal.goesDown ? "DOWN" : "UP",
		out.portal.pos.x, out.portal.pos.y, out.portal.pos.z,
		out.portal.landing.x, out.portal.landing.y, out.portal.landing.z));
	return true;
}

// ============================================================================
// dumpnav v2 portal collector (exact — same classifier as the graph build)
// ============================================================================

void BotEngine::collectNavPortalsInRegion(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2, std::vector<botnav::NavPortal>& out) {
	std::vector<botnav::ZPortal> cellPortals;
	for (int32_t z = z1; z <= z2; ++z) {
		for (int32_t y = y1; y <= y2; ++y) {
			for (int32_t x = x1; x <= x2; ++x) {
				ZCell c = zCellAt(x, y, z);
				if (!c.exists()) {
					continue;
				}
				cellPortals.clear();
				zClassifyCell(x, y, z, c, cellPortals);
				for (const auto& p : cellPortals) {
					botnav::NavPortal np;
					np.x = p.pos.x;
					np.y = p.pos.y;
					np.z = p.pos.z;
					np.lx = p.landing.x;
					np.ly = p.landing.y;
					np.lz = p.landing.z;
					np.kind = static_cast<uint8_t>(p.kind);
					np.goesDown = p.goesDown ? 1 : 0;
					out.push_back(np);
				}
			}
		}
	}
}

// ============================================================================
// BOT_AMBIENT_ROAM — the reachable-region oracle
//
// Lives in this TU because LocalReach is a file-scope class here and hoisting it into the impl
// header would rebuild all twenty engine TUs for no benefit.
//
// Answers "which tiles near this player may a roamer walk to", with an emphasis on REACHABLE
// rather than merely walkable: the flood is a connected set seeded at the anchor, so a tile three
// tiles away that needs a sixty-tile detour is correctly excluded. Conservative, never optimistic
// — a false negative costs one skipped destination, a false positive costs a stuck bot.
//
// Cross-floor comes from the portal graph rather than from the flood: zFloodPassableAt refuses
// FLOORCHANGE and TELEPORT tiles (rightly — nobody should dwell on a staircase), so the flood is
// strictly same-floor by construction and stairs have to be re-introduced deliberately.
// ============================================================================
const BotEngine::RoamRegion* BotEngine::getRoamRegion(const Position& anchor) {
	const int64_t now = OTSYS_TIME();
	const int32_t radius = std::max(4, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_RADIUS)));
	const int32_t maxDz = std::clamp(static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_MAX_DZ)), 0, 3);
	const uint32_t budget = static_cast<uint32_t>(std::max<int64_t>(256, g_configManager().getNumber(BOT_ROAM_REGION_BUDGET)));
	const int64_t ttlMs = std::max<int64_t>(1000, g_configManager().getNumber(BOT_ROAM_REGION_TTL_MS));
	const int32_t moveTiles = std::max(1, static_cast<int32_t>(g_configManager().getNumber(BOT_ROAM_REGION_MOVE_TILES)));

	// Quantize the cache key so a player taking a single step does not orphan the region. The
	// TTL alone cannot do this: at moveTiles=10 a walking anchor would otherwise mint a new key
	// every step and never reuse a flood, which is exactly the case the cache exists for.
	const Position centre(static_cast<uint16_t>(anchor.x / moveTiles * moveTiles),
	                      static_cast<uint16_t>(anchor.y / moveTiles * moveTiles), anchor.z);
	const uint64_t cacheKey = botTileKey(centre);

	auto cached = roamRegions_.find(cacheKey);
	if (cached != roamRegions_.end() && now < cached->second.expiryMs && !cached->second.dests.empty()) {
		return &cached->second;
	}

	// One COLD build per engine tick, server-wide. A region build is 1+P floods and several
	// players arriving together would otherwise stack them into a single dispatcher window --
	// the shape of stall that [VT_DEFER] exists to report.
	//
	// When the budget is spent, serve the STALE region rather than reporting failure. Terrain does
	// not move, so a few seconds of staleness costs nothing for choosing somewhere to walk —
	// whereas returning nullptr made every caller treat a routine budget collision as "the region
	// is gone" and tear the session down. Observed live: bots cycling inject -> release every few
	// seconds, each one logging region_gone.
	if (lastRoamRegionBuildTick_ == now) {
		if (cached != roamRegions_.end() && !cached->second.dests.empty()) {
			return &cached->second;
		}
		return nullptr;
	}
	lastRoamRegionBuildTick_ = now;
	if (cached != roamRegions_.end()) {
		roamRegions_.erase(cached);
	}

	const int64_t t0 = botMonoMs();
	RoamRegion region;
	region.anchor = centre;
	region.builtMs = now;
	region.expiryMs = now + ttlMs;

	// A tile is a legal roam destination if it is in the flood AND is somewhere a bot can
	// plausibly stand around. zFloodPassableAt is a strict SUBSET of the wake-safety mask, not an
	// equivalent, so the extra checks here are load-bearing rather than belt-and-braces:
	//   - house interiors flood in through openable doors, and HouseTile::queryAdd then refuses
	//     the step, so the planner routes to the door and the bot batters it;
	//   - the flood counts an openable door tile as passable, and a bot dwelling half a minute
	//     inside a doorframe is not the look this feature is for.
	auto admitTile = [&](const Position& p) -> bool {
		auto tile = g_game().map.getTile(p);
		if (!tile) return false;
		if (tile->getHouse() != nullptr) return false;
		if (const auto* items = tile->getItemList()) {
			for (const auto& item : *items) {
				if (getDoorTable().count(item->getID())) return false;
			}
		}
		return true;
	};

	auto harvest = [&](LocalReach& reach, uint8_t z) {
		const auto* cells = reach.cells();
		if (!cells) return;
		region.cells += static_cast<uint32_t>(cells->size());
		for (uint64_t k : *cells) {
			// botTileKey packs x<<24 | y<<8 | z (see the impl header); unpack rather than
			// re-deriving, so the two can never disagree about the layout.
			const uint16_t x = static_cast<uint16_t>((k >> 24) & 0xFFFF);
			const uint16_t y = static_cast<uint16_t>((k >> 8) & 0xFFFF);
			const Position p(x, y, z);
			if (botnav::zCheb(centre, p) > radius) continue;
			if (!admitTile(p)) continue;
			region.dests.push_back(p);
		}
	};

	// Flood from the QUANTIZED centre, never the raw anchor. The region cache key was quantized but
	// the flood origin was not, so a player taking a single step minted a brand-new flood origin —
	// and with portal floods riding along, that is a great deal of tile-probing per step. Measured
	// live with a moving player in Marapur: 69 cache hits against 1442 misses (4.6%), with
	// BotEngine::tick spiking to 3.9 s. Quantizing the origin lets consecutive steps share one
	// flood. LocalReach seeds from the first passable neighbour, so a centre inside a wall is
	// handled rather than fatal.
	// ...but the quantized centre is an arbitrary point, and on an island or in a cave it lands in
	// water or solid rock as often as not. LocalReach probes only the 8 immediate neighbours before
	// giving up, which is not enough: quantizing naively broke Marapur outright — a region that had
	// been yielding 876 destinations started returning nothing at all.
	//
	// So walk outward from the centre for the first passable tile. The search depends ONLY on the
	// centre, so every anchor inside the same quantization cell still derives the SAME origin and
	// the cache win survives. Falling back to the raw anchor would reintroduce the per-step misses
	// this quantization exists to remove, so that is the last resort rather than the first.
	Position floodFrom = centre;
	if (!zFloodPassableAt(centre.x, centre.y, centre.z)) {
		bool found = false;
		for (int32_t r = 1; r <= 6 && !found; ++r) {
			for (int32_t dy = -r; dy <= r && !found; ++dy) {
				for (int32_t dx = -r; dx <= r && !found; ++dx) {
					if (std::max(std::abs(dx), std::abs(dy)) != r) continue;  // ring r only
					const int32_t nx = static_cast<int32_t>(centre.x) + dx;
					const int32_t ny = static_cast<int32_t>(centre.y) + dy;
					if (nx < 1 || ny < 1 || nx > 65000 || ny > 65000) continue;
					if (!zFloodPassableAt(nx, ny, centre.z)) continue;
					floodFrom = Position(static_cast<uint16_t>(nx), static_cast<uint16_t>(ny), centre.z);
					found = true;
				}
			}
		}
		if (!found) {
			floodFrom = anchor;  // last resort: correctness over cache locality
		}
	}

	LocalReach base(floodFrom, radius, budget, &roamReachCache_, &roamReachHits_, &roamReachMisses_,
	                ROAM_FLOOD_TTL_MS, ROAM_FLOOD_CACHE_MAX);
	harvest(base, floodFrom.z);

	// Cross-floor. Only portals the planner would actually accept are admitted, because a
	// destination the planner then refuses to route to is pure fail-streak churn:
	//   - blacklisted portals are quarantined for Z_BLACKLIST_MS after a failed traversal;
	//   - a FORCED landing was never confirmed walkable, only assumed.
	if (maxDz > 0 && !zGraph_.empty()) {
		zGraph_.forEachOnFloorNear(centre.z, centre, radius, [&](uint32_t, const botnav::ZPortal& p) {
			// Tight on purpose: every admitted portal costs a whole extra flood, and the cold
			// build is the dominant per-step cost while the anchor is moving.
			if (region.portals >= ROAM_MAX_PORTAL_FLOODS) return;
			if (p.landingForced) return;
			if (std::abs(static_cast<int32_t>(p.landing.z) - static_cast<int32_t>(centre.z)) > maxDz) return;
			if (p.landing.z == centre.z) return;   // same-floor portal adds no new territory
			if (zPortalBlacklisted(p.pos)) return;
			if (!base.reachable(p.pos)) return;    // the portal must be reachable on foot from here
			region.portals++;
			LocalReach landing(p.landing, radius, budget, &roamReachCache_, &roamReachHits_,
			                   &roamReachMisses_, ROAM_FLOOD_TTL_MS, ROAM_FLOOD_CACHE_MAX);
			harvest(landing, p.landing.z);
		});
	}

	region.buildMs = botMonoMs() - t0;

	// De-duplicate: two portals landing on the same floor harvest overlapping discs.
	std::sort(region.dests.begin(), region.dests.end(), [](const Position& a, const Position& b) {
		return botTileKey(a) < botTileKey(b);
	});
	region.dests.erase(std::unique(region.dests.begin(), region.dests.end()), region.dests.end());

	if (region.dests.empty()) {
		// Cache the miss briefly anyway. Without it, an anchor standing somewhere genuinely
		// barren (a boat deck, a sealed vault) re-floods on every supervisor pass forever.
		region.expiryMs = now + ttlMs;
		roamRegions_[cacheKey] = std::move(region);
		return nullptr;
	}

	// Keyed by position, and a travelling player mints new keys forever — evict expired entries
	// rather than letting the map grow for the life of the process.
	if (roamRegions_.size() > ROAM_REGION_CACHE_MAX) {
		for (auto rit = roamRegions_.begin(); rit != roamRegions_.end();) {
			rit = (now >= rit->second.expiryMs) ? roamRegions_.erase(rit) : std::next(rit);
		}
	}
	auto [it, ok] = roamRegions_.insert_or_assign(cacheKey, std::move(region));
	return &it->second;
}
