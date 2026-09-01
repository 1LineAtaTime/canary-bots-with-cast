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

// Per-tile flags (bitmask).
enum NavTileFlag : uint8_t {
	NAV_WALKABLE = 1 << 0, // bot can stand here (canWalkTo true, ignoring live creatures)
	NAV_FLOORCHANGE = 1 << 1, // stairs/ramp/hole/etc. — kernel must not path THROUGH (only to)
	NAV_TELEPORT = 1 << 2, // teleport tile — same rule as FC
	NAV_DOOR = 1 << 3, // has a door (closed doors are opened by the follower, not the kernel)
	NAV_PROTECTIONZONE = 1 << 4, // PZ
	NAV_HAS_GROUND = 1 << 5 // tile exists at all (has a ground item)
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

// A rectangular multi-floor region snapshot. Tiles are indexed
// [ (z - z1) * (w*h) + (y - y1) * w + (x - x1) ].
struct NavDump {
	int32_t x1 = 0, y1 = 0, z1 = 0;
	int32_t x2 = 0, y2 = 0, z2 = 0;
	std::vector<NavTile> tiles;
	std::vector<NavNpc> npcs;
	std::vector<NavAnchor> anchors;
	std::vector<NavPortal> portals; // v2 only; empty when reading a v1 dump
	uint32_t version = NAVDUMP_VERSION; // version actually read/written

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
};

} // namespace botnav
