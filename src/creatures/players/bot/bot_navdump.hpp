/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// bot_navdump.hpp — shared binary map-dump format for the offline pathfinding
// simulator (BOT_NAV_REALISM Phase 3).
//
// Dependency-free (stdlib only), like bot_pathcore.hpp, so BOTH sides link it:
//   - server: the `/cavebot dumpnav` command fills a NavDump from live tiles and
//     writes it (writeFile).
//   - tool:   tools/botnavsim reads it (readFile) and feeds a NavDumpProvider to
//     botnav::findPath — running the EXACT same kernel against a static snapshot.
//
// The dump captures only what the kernel's Provider needs plus enough context
// to render/validate: per-tile bot-walkability + walk cost + FC/teleport/door/PZ
// flags, NPC positions, and town anchors. It deliberately does NOT capture live
// creatures or dynamic door state — see the fidelity caveat in the plan (§3):
// the sim measures static path SHAPE and FC-safety, not crowd behavior.
// ============================================================================

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace botnav {

inline constexpr uint32_t NAVDUMP_MAGIC = 0x424E4456; // 'BNDV'
// v1: tiles + npcs + anchors. v2 (MULTI-FLOOR): appends an exact portal section
// (floor-change transitions with kind/direction/landing, straight from the live
// tile flags + FC item ids + Teleport destinations). readFile accepts both; v1
// dumps simply load with an empty `portals` vector and the offline simulator
// falls back to walkability-inferred portals for them.
inline constexpr uint32_t NAVDUMP_VERSION = 2;
inline constexpr uint32_t NAVDUMP_VERSION_V1 = 1;
// v3 (#140): SPARSE whole-map dump. Same sections as v2, but the tiles are stored as 16x16 blocks
// (the map's own sector size) and only blocks with at least one non-zero cell are written, one byte
// of flags per cell (walkCost is always 0 for a player on a static map: AStarNodes::getTileWalkCost
// prices only creatures and, for monsters, fields). Written by `/cavebot dumpnav world` WITHOUT
// materialising a single tile (the live tile when one exists, else the BasicTile cache), so the
// whole map costs tens of MB on disk and nothing in the server's tile cache. Readers index it
// through a block directory; at()/index() hide the difference from every command.
inline constexpr uint32_t NAVDUMP_VERSION_SPARSE = 3;
inline constexpr int32_t NAVDUMP_BLOCK = 16;

// Per-tile flags (bitmask).
enum NavTileFlag : uint8_t {
	NAV_WALKABLE = 1 << 0, // bot can stand here (canWalkTo true, ignoring live creatures)
	NAV_FLOORCHANGE = 1 << 1, // stairs/ramp/hole/etc. — kernel must not path THROUGH (only to)
	NAV_TELEPORT = 1 << 2, // teleport tile — same rule as FC
	NAV_DOOR = 1 << 3, // has a door (closed doors are opened by the follower, not the kernel)
	NAV_PROTECTIONZONE = 1 << 4, // PZ
	NAV_HAS_GROUND = 1 << 5, // tile exists at all (has a ground item)
	NAV_HOUSE = 1 << 6 // v3: a house tile (route generation refuses them, like the live planner)
};

struct NavTile {
	uint8_t flags = 0; // NavTileFlag bitmask
	uint8_t walkCost = 0; // AStarNodes::getTileWalkCost result (0/40/180…), capped to 255
};

struct NavNpc {
	int32_t x = 0, y = 0, z = 0;
	std::string name;
};

struct NavAnchor {
	int32_t x = 0, y = 0, z = 0;
	std::string label; // e.g. "Thais:temple", "Thais:boat", "Thais:depot"
};

// v2: one DIRECTED floor-change transition captured from the live map.
// kind mirrors botnav::ZPortalKind (kept as raw uint8_t here so this header
// stays include-order independent of bot_zcore.hpp).
struct NavPortal {
	int32_t x = 0, y = 0, z = 0; // the transition tile (walk-on or use-target)
	int32_t lx = 0, ly = 0, lz = 0; // landing position after traversal
	uint8_t kind = 0; // ZPortalKind
	uint8_t goesDown = 0; // 1 = descending edge
};

// #227: an optional trailing section (after the portals) of the cells where the SERVER acts on a
// player by script -- the half of an interaction crossing the tile flags cannot show. A lift lever, a
// shrine, a quarter gate or an aid-keyed step-in teleport is an ordinary walkable tile to every
// other section; only its Lua registration makes it move the player. The destination stays in Lua
// and is not knowable here: an authored route's next waypoint is the claim, live runs verify it.
enum NavTriggerFlag : uint8_t {
	NAV_TRIG_STEP = 1 << 0, // a MoveEvent fires here: registered on the position, or on an aid/uid an item here carries
	NAV_TRIG_USE = 1 << 1, // an item here carries an action id or a unique id (the target of a scripted use)
};
inline constexpr uint32_t NAVDUMP_TRIGGER_MAGIC = 0x47495254; // 'TRIG'

struct NavTrigger {
	int32_t x = 0, y = 0, z = 0;
	uint8_t flags = 0; // NavTriggerFlag bitmask
	uint16_t aid = 0; // the first action id seen on the cell (0 = none / position-registered)
};

// A rectangular multi-floor region snapshot. Tiles are indexed
// [ (z - z1) * (w*h) + (y - y1) * w + (x - x1) ].
struct NavDump {
	int32_t x1 = 0, y1 = 0, z1 = 0;
	int32_t x2 = 0, y2 = 0, z2 = 0;
	std::vector<NavTile> tiles;
	std::vector<NavNpc> npcs;
	std::vector<NavAnchor> anchors;
	std::vector<NavPortal> portals; // v2 only; empty when reading a v1 dump
	std::vector<NavTrigger> triggers; // #227, optional tail section; empty for older dumps
	bool hasTriggers = false; // the dump carried the section (an empty one still says "none here")
	uint32_t version = NAVDUMP_VERSION; // version actually read/written
	// v3 sparse layout: tiles = 256 per present block + ONE trailing all-zero tile (emptyIndex) that
	// every cell of an absent block maps to, so dense-style code (label arrays sized tiles.size(),
	// at() returning a flags-0 tile inside the bounds) keeps working unchanged.
	bool sparse = false;
	int32_t bx0 = 0, by0 = 0, bw = 0, bh = 0; // block-directory origin and size (blocks)
	std::vector<int32_t> blockDir; // [(z - z1) * bh + by][bx] -> block slot, -1 = absent
	size_t emptyIndex = 0;

	int32_t width() const {
		return x2 - x1 + 1;
	}
	int32_t height() const {
		return y2 - y1 + 1;
	}
	int32_t floors() const {
		return z2 - z1 + 1;
	}
	bool inBounds(int32_t x, int32_t y, int32_t z) const {
		return x >= x1 && x <= x2 && y >= y1 && y <= y2 && z >= z1 && z <= z2;
	}
	size_t index(int32_t x, int32_t y, int32_t z) const {
		if (sparse) {
			const int32_t bx = (x >> 4) - bx0, by = (y >> 4) - by0;
			const int32_t slot = blockDir[static_cast<size_t>((z - z1) * bh + by) * static_cast<size_t>(bw) + static_cast<size_t>(bx)];
			return slot < 0 ? emptyIndex
			                : static_cast<size_t>(slot) * 256 + static_cast<size_t>(((y & 15) << 4) | (x & 15));
		}
		return static_cast<size_t>((z - z1)) * static_cast<size_t>(width() * height())
			+ static_cast<size_t>((y - y1)) * static_cast<size_t>(width())
			+ static_cast<size_t>(x - x1);
	}
	const NavTile* at(int32_t x, int32_t y, int32_t z) const {
		if (!inBounds(x, y, z)) {
			return nullptr;
		}
		return &tiles[index(x, y, z)];
	}

	void allocate() {
		tiles.assign(static_cast<size_t>(width()) * static_cast<size_t>(height()) * static_cast<size_t>(floors()), NavTile {});
	}

	// ---- serialization (little-endian host assumed; both ends are x64) ----
	static void wU32(std::ofstream& o, uint32_t v) {
		o.write(reinterpret_cast<const char*>(&v), 4);
	}
	static void wI32(std::ofstream& o, int32_t v) {
		o.write(reinterpret_cast<const char*>(&v), 4);
	}
	static uint32_t rU32(std::ifstream& i) {
		uint32_t v = 0;
		i.read(reinterpret_cast<char*>(&v), 4);
		return v;
	}
	static int32_t rI32(std::ifstream& i) {
		int32_t v = 0;
		i.read(reinterpret_cast<char*>(&v), 4);
		return v;
	}
	static void wStr(std::ofstream& o, const std::string& s) {
		wU32(o, static_cast<uint32_t>(s.size()));
		o.write(s.data(), static_cast<std::streamsize>(s.size()));
	}
	static std::string rStr(std::ifstream& i) {
		uint32_t n = rU32(i);
		std::string s(n, '\0');
		if (n) {
			i.read(&s[0], n);
		}
		return s;
	}

	bool writeFile(const std::string& path) const {
		std::ofstream o(path, std::ios::binary);
		if (!o) {
			return false;
		}
		wU32(o, NAVDUMP_MAGIC);
		wU32(o, NAVDUMP_VERSION);
		wI32(o, x1);
		wI32(o, y1);
		wI32(o, z1);
		wI32(o, x2);
		wI32(o, y2);
		wI32(o, z2);
		wU32(o, static_cast<uint32_t>(tiles.size()));
		for (const auto& t : tiles) {
			o.put(static_cast<char>(t.flags));
			o.put(static_cast<char>(t.walkCost));
		}
		wU32(o, static_cast<uint32_t>(npcs.size()));
		for (const auto& n : npcs) {
			wI32(o, n.x);
			wI32(o, n.y);
			wI32(o, n.z);
			wStr(o, n.name);
		}
		wU32(o, static_cast<uint32_t>(anchors.size()));
		for (const auto& a : anchors) {
			wI32(o, a.x);
			wI32(o, a.y);
			wI32(o, a.z);
			wStr(o, a.label);
		}
		// v2 portal section (always written; readers of v1 files never get here).
		wU32(o, static_cast<uint32_t>(portals.size()));
		for (const auto& p : portals) {
			wI32(o, p.x);
			wI32(o, p.y);
			wI32(o, p.z);
			wI32(o, p.lx);
			wI32(o, p.ly);
			wI32(o, p.lz);
			o.put(static_cast<char>(p.kind));
			o.put(static_cast<char>(p.goesDown));
		}
		return static_cast<bool>(o);
	}

	bool readFile(const std::string& path) {
		std::ifstream i(path, std::ios::binary);
		if (!i) {
			return false;
		}
		if (rU32(i) != NAVDUMP_MAGIC) {
			return false;
		}
		version = rU32(i);
		if (version == NAVDUMP_VERSION_SPARSE) {
			return readSparseBody(i);
		}
		sparse = false;
		if (version != NAVDUMP_VERSION_V1 && version != NAVDUMP_VERSION) {
			return false;
		}
		x1 = rI32(i);
		y1 = rI32(i);
		z1 = rI32(i);
		x2 = rI32(i);
		y2 = rI32(i);
		z2 = rI32(i);
		uint32_t nTiles = rU32(i);
		tiles.resize(nTiles);
		for (auto& t : tiles) {
			t.flags = static_cast<uint8_t>(i.get());
			t.walkCost = static_cast<uint8_t>(i.get());
		}
		uint32_t nNpc = rU32(i);
		npcs.resize(nNpc);
		for (auto& n : npcs) {
			n.x = rI32(i);
			n.y = rI32(i);
			n.z = rI32(i);
			n.name = rStr(i);
		}
		uint32_t nAnc = rU32(i);
		anchors.resize(nAnc);
		for (auto& a : anchors) {
			a.x = rI32(i);
			a.y = rI32(i);
			a.z = rI32(i);
			a.label = rStr(i);
		}
		portals.clear();
		if (version >= 2) {
			uint32_t nPort = rU32(i);
			portals.resize(nPort);
			for (auto& p : portals) {
				p.x = rI32(i);
				p.y = rI32(i);
				p.z = rI32(i);
				p.lx = rI32(i);
				p.ly = rI32(i);
				p.lz = rI32(i);
				p.kind = static_cast<uint8_t>(i.get());
				p.goesDown = static_cast<uint8_t>(i.get());
			}
		}
		return static_cast<bool>(i);
	}

	// npcs, anchors and portals: the same trailing sections in every version >= 2.
	void writeTail(std::ofstream& o) const {
		wU32(o, static_cast<uint32_t>(npcs.size()));
		for (const auto& n : npcs) {
			wI32(o, n.x);
			wI32(o, n.y);
			wI32(o, n.z);
			wStr(o, n.name);
		}
		wU32(o, static_cast<uint32_t>(anchors.size()));
		for (const auto& a : anchors) {
			wI32(o, a.x);
			wI32(o, a.y);
			wI32(o, a.z);
			wStr(o, a.label);
		}
		wU32(o, static_cast<uint32_t>(portals.size()));
		for (const auto& p : portals) {
			wI32(o, p.x);
			wI32(o, p.y);
			wI32(o, p.z);
			wI32(o, p.lx);
			wI32(o, p.ly);
			wI32(o, p.lz);
			o.put(static_cast<char>(p.kind));
			o.put(static_cast<char>(p.goesDown));
		}
		// #227 trigger section: magic + count + records. Readers that predate it stop after the
		// portals and never look further, so the section needs no version bump.
		wU32(o, NAVDUMP_TRIGGER_MAGIC);
		wU32(o, static_cast<uint32_t>(triggers.size()));
		for (const auto& t : triggers) {
			wI32(o, t.x);
			wI32(o, t.y);
			o.put(static_cast<char>(t.z));
			o.put(static_cast<char>(t.flags));
			o.put(static_cast<char>(t.aid & 0xFF));
			o.put(static_cast<char>(t.aid >> 8));
		}
	}

	// The optional #227 section after the portals; absent (EOF) in dumps written before it.
	bool readTriggers(std::ifstream& i) {
		triggers.clear();
		hasTriggers = false;
		if (i.peek() == std::char_traits<char>::eof()) {
			i.clear();
			return true;
		}
		if (rU32(i) != NAVDUMP_TRIGGER_MAGIC) {
			return false;
		}
		const uint32_t n = rU32(i);
		triggers.resize(n);
		for (auto& t : triggers) {
			t.x = rI32(i);
			t.y = rI32(i);
			t.z = static_cast<uint8_t>(i.get());
			t.flags = static_cast<uint8_t>(i.get());
			const auto lo = static_cast<uint8_t>(i.get());
			const auto hi = static_cast<uint8_t>(i.get());
			t.aid = static_cast<uint16_t>(lo | (hi << 8));
		}
		hasTriggers = static_cast<bool>(i);
		return hasTriggers;
	}

	// v3 body (after magic + version): bounds, block size + count, blocks, then the v2 tail.
	bool readSparseBody(std::ifstream& i) {
		sparse = true;
		x1 = rI32(i);
		y1 = rI32(i);
		z1 = rI32(i);
		x2 = rI32(i);
		y2 = rI32(i);
		z2 = rI32(i);
		const uint32_t blockSize = rU32(i);
		const uint32_t nBlocks = rU32(i);
		if (!i || blockSize != static_cast<uint32_t>(NAVDUMP_BLOCK) || x2 < x1 || y2 < y1 || z2 < z1) {
			return false;
		}
		bx0 = x1 >> 4;
		by0 = y1 >> 4;
		bw = (x2 >> 4) - bx0 + 1;
		bh = (y2 >> 4) - by0 + 1;
		blockDir.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh) * static_cast<size_t>(z2 - z1 + 1), -1);
		tiles.assign(static_cast<size_t>(nBlocks) * 256 + 1, NavTile {});
		emptyIndex = tiles.size() - 1;
		char cells[256];
		for (uint32_t b = 0; b < nBlocks; ++b) {
			const int32_t bx = rI32(i);
			const int32_t by = rI32(i);
			const int32_t z = static_cast<uint8_t>(i.get());
			i.read(cells, 256);
			if (!i || bx < bx0 || bx >= bx0 + bw || by < by0 || by >= by0 + bh || z < z1 || z > z2) {
				return false;
			}
			blockDir[static_cast<size_t>((z - z1) * bh + (by - by0)) * static_cast<size_t>(bw) + static_cast<size_t>(bx - bx0)]
				= static_cast<int32_t>(b);
			for (int c = 0; c < 256; ++c) {
				tiles[static_cast<size_t>(b) * 256 + static_cast<size_t>(c)].flags = static_cast<uint8_t>(cells[c]);
			}
		}
		uint32_t nNpc = rU32(i);
		npcs.resize(nNpc);
		for (auto& n : npcs) {
			n.x = rI32(i);
			n.y = rI32(i);
			n.z = rI32(i);
			n.name = rStr(i);
		}
		uint32_t nAnc = rU32(i);
		anchors.resize(nAnc);
		for (auto& a : anchors) {
			a.x = rI32(i);
			a.y = rI32(i);
			a.z = rI32(i);
			a.label = rStr(i);
		}
		uint32_t nPort = rU32(i);
		portals.resize(nPort);
		for (auto& p : portals) {
			p.x = rI32(i);
			p.y = rI32(i);
			p.z = rI32(i);
			p.lx = rI32(i);
			p.ly = rI32(i);
			p.lz = rI32(i);
			p.kind = static_cast<uint8_t>(i.get());
			p.goesDown = static_cast<uint8_t>(i.get());
		}
		if (!i) {
			return false;
		}
		return readTriggers(i);
	}
};

// #140: streaming writer for a v3 sparse dump. The server adds blocks over many ticks (a tick-sliced
// admin job), so nothing but the open stream is held; the bounds and the block count are patched into
// the header by finish().
struct NavDumpSparseWriter {
	std::ofstream o;
	uint32_t blocks = 0;
	int32_t minX = INT32_MAX, minY = INT32_MAX, minZ = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN, maxZ = INT32_MIN;

	bool begin(const std::string& path) {
		o.open(path, std::ios::binary | std::ios::trunc);
		if (!o) {
			return false;
		}
		NavDump::wU32(o, NAVDUMP_MAGIC);
		NavDump::wU32(o, NAVDUMP_VERSION_SPARSE);
		for (int k = 0; k < 6; ++k) {
			NavDump::wI32(o, 0); // bounds, patched by finish()
		}
		NavDump::wU32(o, static_cast<uint32_t>(NAVDUMP_BLOCK));
		NavDump::wU32(o, 0); // block count, patched by finish()
		return static_cast<bool>(o);
	}
	// cells[(y & 15) * 16 + (x & 15)] = NavTileFlag bitmask; bx/by in block units (x >> 4).
	void addBlock(int32_t bx, int32_t by, int32_t z, const uint8_t* cells) {
		NavDump::wI32(o, bx);
		NavDump::wI32(o, by);
		o.put(static_cast<char>(z));
		o.write(reinterpret_cast<const char*>(cells), 256);
		blocks++;
		minX = std::min(minX, bx * 16);
		minY = std::min(minY, by * 16);
		minZ = std::min(minZ, z);
		maxX = std::max(maxX, bx * 16 + 15);
		maxY = std::max(maxY, by * 16 + 15);
		maxZ = std::max(maxZ, z);
	}
	// Writes the tail sections of `meta` (npcs, anchors, portals) and patches the header.
	bool finish(const NavDump& meta) {
		meta.writeTail(o);
		if (blocks == 0) {
			minX = minY = minZ = maxX = maxY = maxZ = 0;
		}
		o.seekp(8);
		NavDump::wI32(o, minX);
		NavDump::wI32(o, minY);
		NavDump::wI32(o, minZ);
		NavDump::wI32(o, maxX);
		NavDump::wI32(o, maxY);
		NavDump::wI32(o, maxZ);
		NavDump::wU32(o, static_cast<uint32_t>(NAVDUMP_BLOCK));
		NavDump::wU32(o, blocks);
		o.close();
		return !o.fail();
	}
};

} // namespace botnav
