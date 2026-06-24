-- ============================================================================
-- Bot Player System -- Gesior-style auto-walk for Canary 13.x
-- 200 bots across all cities, inter-city travel, fight/flight, random PK
-- Uses creature:startAutoWalk() + creature:isWalking() (server-side walk scheduler)
-- ============================================================================

-- Configuration
BOT_CONFIG = {
	-- TEMP TEST 1 (2026-05-21): master disable for GAP_SLOW infrastructure-cause test.
	-- When true, all bot-driven globalevents short-circuit and TARGET_ONLINE forced to 0.
	-- Test 1 RESULT: 1 GAP_SLOW in 10 min with bots off vs ~139/hr with bots on (~23× drop).
	-- Confirms: bots are the load source. Now testing taskset CPU pinning.
	MASTER_DISABLE = false,
	BOT_ACCOUNT_ID = 65000,
	-- Active bot count comes from config.lua botPlayersOnline (default 200 if unset).
	-- Capped by BotStartup against the actual DB pool size (currently 997 bots in MySQL).
	TARGET_ONLINE = (configManager and configManager.getNumber(configKeys.BOT_PLAYERS_ONLINE)) or 200,
	MANAGER_INTERVAL = 10000,         -- ms between population manager ticks
	THINK_INTERVAL = 100,             -- ms between AI ticks (100ms for smooth walking)
	THINK_INTERVAL_WALK = 500,        -- ms for bots walking/traveling (engine handles steps)
	THINK_INTERVAL_IDLE = 2000,       -- ms between AI ticks for IDLE/DWELLING bots
	PATH_MAX_DIST = 50,              -- max A* search distance
	WAYPOINT_DIST = 20,              -- intermediate waypoint distance for long walks
	CHAT_CHANCE = 300,               -- 1/N chance per tick to say something
	HEAL_THRESHOLD_PERCENT = 60,
	ATTACK_COOLDOWN = 2,             -- seconds between attacks
	HEAL_COOLDOWN = 1,               -- seconds between heals
	DEAD_POSITION = Position(31970, 32283, 7),  -- staging area for inactive bots
	LOG_INTERVAL = 30,               -- log position every N ticks (~30 seconds)
	POI_DWELL_MIN = 300,             -- 5 minutes min dwell at POI
	POI_DWELL_MAX = 1800,            -- 30 minutes max dwell at POI
	POI_ARRIVAL_DIST = 3,            -- tiles from POI center to count as "arrived"
	STUCK_THRESHOLD = 5,             -- consecutive POI failures before emergency teleport
	-- Loitering removed: real players walk to destinations, not stop mid-road
	TRAVEL_CHANCE_PER_TICK = 1000,   -- 1/N per tick (~0.1%, travel every ~15-20 min)
	TRAVEL_PAUSE_MIN = 10,           -- seconds to "wait at dock"
	TRAVEL_PAUSE_MAX = 30,
	PK_CHANCE_PER_TICK = 400,        -- 1/N per tick (~0.25%) to PK a real player
	PK_BOT_CHANCE_PER_TICK = 4000,   -- 1/N per tick (~0.025%) to PK another bot
	PK_TIMEOUT = 15,                 -- seconds before PK bot gives up
	COMBAT_TIMEOUT = 30,             -- seconds before combat/flee state resets
	COMBAT_LEASH_DIST = 15,         -- max distance to chase attacker before giving up
	FLEE_DISTANCE = 15,              -- tiles to run when fleeing
	-- Hunting config
	HUNT_CHANCE_PER_TICK = 200,      -- 1/N per tick (~0.5%) to start a hunt
	HUNT_DURATION_MIN = 1800,        -- 30 min minimum hunt duration
	HUNT_DURATION_MAX = 10800,       -- 3 hr maximum hunt duration
	HUNT_COOLDOWN_MIN = 600,         -- 10 min between hunts
	HUNT_COOLDOWN_MAX = 1800,        -- 30 min between hunts
	HUNT_RESUPPLY_BANK_MIN = 20,     -- seconds at bank NPC
	HUNT_RESUPPLY_BANK_MAX = 60,
	HUNT_RESUPPLY_DEPOT_MIN = 60,    -- seconds at depot
	HUNT_RESUPPLY_DEPOT_MAX = 180,   -- 3 min max at depot
	HUNT_RESUPPLY_SHOP_MIN = 20,     -- seconds at each shop NPC
	HUNT_RESUPPLY_SHOP_MAX = 60,
	HUNT_MONSTER_SCAN_RADIUS = 7,
	HUNT_MONSTER_ATTACK_CD = 2,      -- seconds between monster attacks
	HUNT_SAFETY_TIMEOUT = 14400,     -- 4h max per hunt session
	HUNT_WAYPOINT_SKIP_DELAY = 200,  -- ms delay when trying next waypoint on failure
	HUNT_STUCK_THRESHOLD = 30,       -- consecutive skips without progress before abort
	HUNT_NODE_ARRIVAL_DIST = 2,      -- tiles from node to count as arrived
	HUNT_CHASE_FAIL_LIMIT = 8,       -- ticks of failed chase before giving up on target
	HUNT_IGNORE_DURATION = 30,       -- seconds to ignore an unreachable monster
	HUNT_BLOCKER_FAIL_LIMIT = 10,    -- ticks before giving up on a path blocker
	DEBUG_COMBAT = true,             -- enable combat debug logging
	DEBUG_COMBAT_INTERVAL = 5,       -- seconds between debug log entries per bot
	-- Debug mode: set to true to load only 1 bot for testing z-transitions
	DEBUG_MODE = false,
	DEBUG_BOT_NAME = "Ophelia Flamecrest",  -- nil = first bot in DB; set "BotName" to pick specific bot
	-- 2026-05-28: when TARGET_ONLINE (i.e. config.lua botPlayersOnline) is set to 1, BotManager
	-- will load the named bot specifically instead of the lowest-id bot from stratification.
	-- Set to "" to use default stratified pick. Useful for isolated cast-watch testing.
	SINGLE_BOT_NAME = "Jorth Elsoutchawnc",
	-- Cached at load time: true when binary compiled with -DDEBUG_LOG (cmake Debug build)
	IS_DEBUG_BUILD = Game.isDebugBuild and Game.isDebugBuild() or false,
}

-- State enum
BOT_STATE = {
	INACTIVE  = 0,
	IDLE      = 1,   -- walking between POIs
	DWELLING  = 2,   -- standing at a POI (simulating interaction)
	COMBAT    = 3,   -- fighting back against attacker
	FLEEING   = 4,   -- running away from attacker
	TRAVELING = 5,   -- paused at dock, about to teleport to another city
	PK_ATTACK = 6,   -- randomly attacking a nearby player
	HUNTING   = 7,   -- hunting monsters (travel/patrol/leave/resupply)
	PARTY     = 8,   -- following a player in a party (/party command)
}

HUNT_PHASE = {
	PREPARING    = 0,  -- depot box + shops BEFORE heading to spawn
	TRAVEL_TO    = 1,  -- city -> spawn entrance
	PATROLLING   = 2,  -- hunt loop: walk waypoints + fight monsters
	LEAVING      = 3,  -- spawn exit -> city
	RESUPPLYING  = 4,  -- bank -> depot -> shops -> re-hunt or end
}

-- Floor change sub-states (multi-tick state machine, BBot-inspired)
FLOOR_CHANGE_STATE = {
	NONE        = 0,  -- no floor change in progress
	SCANNING    = 1,  -- finding transitions nearby
	WALKING_TO  = 2,  -- pathfinding to adjacent tile
	STEPPING_ON = 3,  -- stepping onto the floor-change tile
	VERIFYING   = 4,  -- waiting to confirm z changed (~2 ticks)
	USING_ITEM  = 5,  -- using rope/shovel/ladder on tile
	COMPLETED   = 6,  -- z-change confirmed, resume navigation
	FAILED      = 7,  -- all attempts exhausted
}

-- ============================================================================
-- Spell tables: per vocation, level-gated, with cooldowns
-- ============================================================================

BOT_SPELLS = {
	[1] = { -- Sorcerer (base voc 1)
		{level = 1,   name = "exori vis",          combatType = COMBAT_ENERGYDAMAGE, range = 3, effect = CONST_ME_ENERGYHIT,    cd = 2, baseDmg = 15},
		{level = 23,  name = "exori mort",         combatType = COMBAT_DEATHDAMAGE,  range = 3, effect = CONST_ME_MORTAREA,     cd = 2, baseDmg = 25},
		{level = 28,  name = "exevo vis lux",      combatType = COMBAT_ENERGYDAMAGE, range = 5, effect = CONST_ME_ENERGYAREA,   cd = 4, baseDmg = 30},
		{level = 45,  name = "adori vita vis",     combatType = COMBAT_DEATHDAMAGE,  range = 5, effect = CONST_ME_MORTAREA,     cd = 4, baseDmg = 50, isRune = true, pvpOnly = true},
		{level = 60,  name = "exori gran vis",     combatType = COMBAT_ENERGYDAMAGE, range = 3, effect = CONST_ME_ENERGYHIT,    cd = 8, baseDmg = 45},
	},
	[2] = { -- Druid (base voc 2)
		{level = 1,   name = "exori flam",         combatType = COMBAT_FIREDAMAGE,   range = 3, effect = CONST_ME_FIREATTACK,   cd = 2, baseDmg = 15},
		{level = 21,  name = "exori frigo",        combatType = COMBAT_ICEDAMAGE,    range = 3, effect = CONST_ME_ICEATTACK,    cd = 2, baseDmg = 20},
		{level = 28,  name = "exevo frigo hur",    combatType = COMBAT_ICEDAMAGE,    range = 5, effect = CONST_ME_ICEAREA,      cd = 4, baseDmg = 30},
		{level = 45,  name = "adori vita vis",     combatType = COMBAT_DEATHDAMAGE,  range = 5, effect = CONST_ME_MORTAREA,     cd = 4, baseDmg = 50, isRune = true, pvpOnly = true},
		{level = 60,  name = "exori gran frigo",   combatType = COMBAT_ICEDAMAGE,    range = 3, effect = CONST_ME_ICEAREA,      cd = 8, baseDmg = 45},
	},
	[3] = { -- Paladin (base voc 3)
		{level = 1,   name = "exori san",          combatType = COMBAT_HOLYDAMAGE,   range = 5, effect = CONST_ME_HOLYDAMAGE,   cd = 2, baseDmg = 20, projectile = CONST_ANI_ROYALSPEAR},
		{level = 24,  name = "exori con",          combatType = COMBAT_PHYSICALDAMAGE,range = 5, cd = 2, baseDmg = 25, projectile = CONST_ANI_ROYALSPEAR},
		{level = 60,  name = "exori gran con",     combatType = COMBAT_PHYSICALDAMAGE,range = 5, cd = 6, baseDmg = 40, projectile = CONST_ANI_ROYALSPEAR},
	},
	[4] = { -- Knight (base voc 4)
		{level = 1,   name = "exori min",          combatType = COMBAT_PHYSICALDAMAGE, range = 1, effect = CONST_ME_HITAREA,    cd = 2, baseDmg = 15},
		{level = 20,  name = "exori",              combatType = COMBAT_PHYSICALDAMAGE, range = 1, effect = CONST_ME_HITAREA,    cd = 2, baseDmg = 25},
		{level = 35,  name = "exori hur",          combatType = COMBAT_PHYSICALDAMAGE, range = 1, effect = CONST_ME_HITAREA,    cd = 4, baseDmg = 35},
		{level = 45,  name = "exori gran",         combatType = COMBAT_PHYSICALDAMAGE, range = 1, effect = CONST_ME_HITAREA,    cd = 6, baseDmg = 50},
		{level = 90,  name = "exori gran ico",     combatType = COMBAT_ICEDAMAGE,      range = 1, effect = CONST_ME_ICEATTACK,  cd = 8, baseDmg = 60},
	},
}

BOT_HEAL_SPELLS = {
	[1] = { -- Sorcerer
		{level = 1,  name = "exura",       heal = 80,   cd = 1},
		{level = 30, name = "exura vita",  heal = 200,  cd = 2},
	},
	[2] = { -- Druid
		{level = 1,  name = "exura",       heal = 80,   cd = 1},
		{level = 30, name = "exura vita",  heal = 200,  cd = 2},
	},
	[3] = { -- Paladin
		{level = 1,  name = "exura",       heal = 80,   cd = 1},
		{level = 30, name = "exura san",   heal = 250,  cd = 2},
	},
	[4] = { -- Knight
		{level = 1,  name = "exura",       heal = 80,   cd = 1},
		{level = 30, name = "exura ico",   heal = 300,  cd = 2},
	},
}

-- ============================================================================
-- City Points of Interest (POIs) — keyed by town_id
-- Coordinates from NPC spawn data in otservbr-npc.xml
-- Positions are street-side (not inside buildings)
-- ============================================================================

-- Walking z-level per city (may differ from temple z-level)
-- e.g. Ankrahmun temple is underground z=8 but city streets are z=7
CITY_WALK_Z = {
	[5]  = 7,   -- Ab'Dendriel (surface)
	[6]  = 7,   -- Carlin (surface)
	[7]  = 11,  -- Kazordoon (underground dwarf city at z=11)
	[8]  = 7,   -- Thais (surface)
	[9]  = 7,   -- Venore (surface)
	[10] = 7,   -- Ankrahmun (temple underground z=8, city surface z=7)
	[11] = 8,   -- Edron (mountain city z=8)
	[12] = 10,  -- Farmine (temple z=11, city surface z=10)
	[13] = 7,   -- Darashia (temple at z=1 tower, city surface z=7)
	[14] = 7,   -- Liberty Bay (surface)
	[15] = 7,   -- Port Hope (surface)
	[16] = 7,   -- Svargrond (surface)
	[17] = 7,   -- Yalahar (surface)
	[18] = 9,   -- Gray Beach (surface z=9)
	[19] = 8,   -- Krailos (surface z=8)
	[20] = 6,   -- Rathleton (main level z=6)
	[21] = 6,   -- Roshamuul (main level z=6)
	[22] = 5,   -- Issavi (main level z=5)
	[24] = 7,   -- Cobra Bastion (surface)
	[25] = 7,   -- Bounac (surface)
	[26] = 7,   -- Feyrist (surface)
	[27] = 14,  -- Gnomprona (deep underground z=14)
	[28] = 7,   -- Marapur (surface)
	[29] = 7,   -- Candia (surface)
	[30] = 7,   -- Silvertides (surface)
	[31] = 5,   -- Moonfall (z=5)
}

CITY_POIS = {
	[8] = { -- Thais (temple: 32369,32241,7)
		{ name = "Thais Depot",          pos = Position(32365, 32212, 7), type = "depot" },
		{ name = "Thais Temple",         pos = Position(32369, 32241, 7), type = "temple" },
		{ name = "Sam's General Store",  pos = Position(32362, 32196, 7), type = "shop" },
		{ name = "Frodo's Tavern",       pos = Position(32360, 32211, 7), type = "shop" },
		{ name = "Gorn's Armour Shop",   pos = Position(32376, 32202, 7), type = "shop" },
		{ name = "Xodet's Magic Shop",   pos = Position(32395, 32224, 7), type = "shop" },
		{ name = "Oswald's Shop",        pos = Position(32380, 32222, 7), type = "shop" },
		{ name = "Benjamin (Post)",      pos = Position(32348, 32221, 7), type = "npc" },
		{ name = "Elane (Paladin GM)",   pos = Position(32344, 32233, 7), type = "npc" },
		{ name = "Quentin (Monk)",       pos = Position(32369, 32238, 7), type = "npc" },
		{ name = "Aruda (Equipment)",    pos = Position(32369, 32216, 7), type = "shop" },
	},
	[6] = { -- Carlin (temple: 32360,31782,7)
		{ name = "Carlin Depot",         pos = Position(32360, 31785, 7), type = "depot" },
		{ name = "Carlin Temple",        pos = Position(32360, 31782, 7), type = "temple" },
		{ name = "Carlin Bank",          pos = Position(32343, 31828, 7), type = "npc" },
		{ name = "Carlin Armour Shop",   pos = Position(32370, 31795, 7), type = "shop" },
	},
	[5] = { -- Ab'Dendriel (temple: 32732,31634,7)
		{ name = "Ab'Dendriel Depot",    pos = Position(32734, 31634, 7), type = "depot" },
		{ name = "Ab'Dendriel Temple",   pos = Position(32732, 31634, 7), type = "temple" },
		{ name = "Amarie (Equipment)",   pos = Position(32693, 31658, 7), type = "shop" },
		{ name = "Shiriel (Spells)",     pos = Position(32669, 31657, 7), type = "npc" },
	},
	[9] = { -- Venore (temple: 32957,32076,7)
		{ name = "Venore Depot",         pos = Position(32957, 32076, 7), type = "depot" },
		{ name = "Venore Temple",        pos = Position(32957, 32076, 7), type = "temple" },
		{ name = "Venore Market",        pos = Position(32960, 32090, 7), type = "shop" },
		{ name = "Venore Magic Shop",    pos = Position(32970, 32075, 7), type = "shop" },
	},
	[11] = { -- Edron (temple: 33217,31814,8)
		{ name = "Edron Depot",          pos = Position(33217, 31814, 8), type = "depot" },
		{ name = "Edron Temple",         pos = Position(33217, 31814, 8), type = "temple" },
		{ name = "Edron Academy",        pos = Position(33230, 31830, 8), type = "npc" },
		{ name = "Edron Weapon Shop",    pos = Position(33210, 31800, 8), type = "shop" },
	},
	[7] = { -- Kazordoon (temple: 32649,31925,11) — underground dwarf city
		{ name = "Kazordoon Depot",      pos = Position(32649, 31911, 11), type = "depot" },
		{ name = "Kazordoon Temple",     pos = Position(32649, 31925, 11), type = "temple" },
		{ name = "Kazordoon Forge",      pos = Position(32653, 31926, 11), type = "shop" },
	},
	[13] = { -- Darashia (temple: 33213,32454,1 — tower/roof; city streets z=7)
		{ name = "Darashia Depot",       pos = Position(33217, 32423, 7), type = "depot" },
		{ name = "Darashia Temple",      pos = Position(33213, 32453, 7), type = "temple" },
		{ name = "Darashia Bazaar",      pos = Position(33232, 32430, 7), type = "shop" },
		{ name = "Darashia Magic Shop",  pos = Position(33225, 32434, 7), type = "shop" },
	},
	[10] = { -- Ankrahmun (temple: 33194,32853,8 — underground; city surface z=7)
		{ name = "Ankrahmun Depot",      pos = Position(33195, 32855, 7), type = "depot" },
		{ name = "Ankrahmun Temple",     pos = Position(33197, 32851, 7), type = "temple" },
		{ name = "Ankrahmun Trade St",   pos = Position(33181, 32883, 7), type = "shop" },
		{ name = "Ankrahmun Shop",       pos = Position(33158, 32849, 7), type = "shop" },
	},
	[14] = { -- Liberty Bay (temple: 32317,32826,7)
		{ name = "Liberty Bay Depot",    pos = Position(32317, 32826, 7), type = "depot" },
		{ name = "Liberty Bay Temple",   pos = Position(32317, 32826, 7), type = "temple" },
		{ name = "Liberty Bay Market",   pos = Position(32310, 32840, 7), type = "shop" },
	},
	[15] = { -- Port Hope (temple: 32594,32745,7)
		{ name = "Port Hope Depot",      pos = Position(32595, 32745, 7), type = "depot" },
		{ name = "Port Hope Temple",     pos = Position(32594, 32745, 7), type = "temple" },
		{ name = "Port Hope Trader",     pos = Position(32590, 32735, 7), type = "shop" },
	},
	[16] = { -- Svargrond (temple: 32212,31132,7)
		{ name = "Svargrond Depot",      pos = Position(32212, 31132, 7), type = "depot" },
		{ name = "Svargrond Temple",     pos = Position(32212, 31132, 7), type = "temple" },
		{ name = "Svargrond Tavern",     pos = Position(32230, 31140, 7), type = "shop" },
	},
	[17] = { -- Yalahar (temple: 32787,31276,7)
		{ name = "Yalahar Depot",        pos = Position(32787, 31277, 7), type = "depot" },
		{ name = "Yalahar Temple",       pos = Position(32787, 31276, 7), type = "temple" },
		{ name = "Yalahar Trade Quarter",pos = Position(32810, 31251, 7), type = "shop" },
	},
	[20] = { -- Rathleton (temple: 33594,31899,6)
		{ name = "Rathleton Depot",      pos = Position(33593, 31899, 6), type = "depot" },
		{ name = "Rathleton Temple",     pos = Position(33594, 31899, 6), type = "temple" },
		{ name = "Rathleton Workshop",   pos = Position(33622, 31883, 6), type = "shop" },
		{ name = "Rathleton Shop",       pos = Position(33611, 31886, 6), type = "shop" },
	},
	[22] = { -- Issavi (temple: 33921,31477,5)
		{ name = "Issavi Depot",         pos = Position(33924, 31476, 5), type = "depot" },
		{ name = "Issavi Temple",        pos = Position(33921, 31477, 5), type = "temple" },
		{ name = "Issavi Market",        pos = Position(33935, 31484, 5), type = "shop" },
		{ name = "Issavi Shop",          pos = Position(33909, 31495, 5), type = "shop" },
	},
	[12] = { -- Farmine (temple: 33023,31521,11; city z=10)
		{ name = "Farmine Depot",        pos = Position(33023, 31542, 10), type = "depot" },
		{ name = "Farmine Temple",       pos = Position(33023, 31521, 11), type = "temple" },
		{ name = "Farmine Esrik",        pos = Position(33036, 31530, 10), type = "npc" },
		{ name = "Farmine Cael",         pos = Position(33028, 31513, 10), type = "npc" },
	},
	[19] = { -- Krailos (temple: 33657,31665,8)
		{ name = "Krailos Depot",        pos = Position(33657, 31665, 8), type = "depot" },
		{ name = "Krailos Temple",       pos = Position(33657, 31665, 8), type = "temple" },
	},
	[21] = { -- Roshamuul (temple: 33513,32363,6)
		{ name = "Roshamuul Depot",      pos = Position(33513, 32363, 6), type = "depot" },
		{ name = "Roshamuul Temple",     pos = Position(33513, 32363, 6), type = "temple" },
	},
	[25] = { -- Bounac (temple: 32424,32445,7)
		{ name = "Bounac Depot",         pos = Position(32424, 32445, 7), type = "depot" },
		{ name = "Bounac Temple",        pos = Position(32424, 32445, 7), type = "temple" },
	},
	[26] = { -- Feyrist (temple: 33490,32221,7)
		{ name = "Feyrist Depot",        pos = Position(33490, 32221, 7), type = "depot" },
		{ name = "Feyrist Temple",       pos = Position(33490, 32221, 7), type = "temple" },
		{ name = "Feyrist Taegen",       pos = Position(33492, 32227, 7), type = "npc" },
		{ name = "Feyrist Maelyrra",     pos = Position(33485, 32236, 7), type = "npc" },
	},
}

-- POI type weights for IDLE priority selection
-- Shops/NPCs removed from IDLE rotation (visited during hunt resupply only)
POI_TYPE_WEIGHTS = {
	depot         = 35,   -- inside depot area (50% chance walk to locker)
	depot_outside = 12,   -- ±3-5 tiles from depot entrance
	temple        = 25,   -- temple area
	boat          = 15,   -- boat NPC position (z may differ from city walk z)
	shop          = 0,    -- excluded from IDLE selection
	npc           = 0,    -- excluded from IDLE selection
}

-- Hardcoded boat captain NPC positions (fallback when route graph has no boat POI)
BOAT_POSITIONS = {
	[5]  = Position(33175, 31763, 6),   -- Ab'Dendriel (Captain Seahorse)
	[6]  = Position(32310, 32210, 6),   -- Carlin (Captain Bluebear)
	[7]  = Position(33791, 31383, 6),   -- Kazordoon (Captain Jack Rat) [actually carts/steamship]
	[8]  = Position(32387, 31822, 6),   -- Thais (Captain Greyhound)
	[9]  = Position(32954, 32022, 6),   -- Venore (Captain Fearless)
	[10] = Position(33092, 32883, 6),   -- Ankrahmun (Captain Sinbeard)
	[11] = Position(33173, 31764, 6),   -- Edron (Captain Gulliver)
	[12] = Position(32097, 31964, 6),   -- Farmine (Captain Kurt) [steamship]
	[13] = Position(33289, 32480, 6),   -- Darashia (Captain Fearless)
	[14] = Position(32285, 32892, 6),   -- Liberty Bay (Captain Bluebear)
	[15] = Position(32527, 32784, 6),   -- Port Hope (Captain Bravesoul)
	[16] = Position(32341, 31108, 6),   -- Svargrond (Captain Haba)
	[17] = Position(32805, 31272, 6),   -- Yalahar (near Captain Cookie, walkable floor)
	[19] = Position(33492, 31712, 6),   -- Krailos (NPC)
	[20] = Position(32298, 32895, 6),   -- Rathleton (Captain Max)
	[21] = Position(33286, 31955, 6),   -- Roshamuul (Pemaret)
	[22] = Position(32056, 32368, 6),   -- Issavi (Captain Tiberius)
	[25] = Position(32346, 32859, 7),   -- Bounac (Captain Waverider)
	[26] = Position(33561, 32196, 6),   -- Feyrist (NPC)
}

-- Travel routes (simulating boat connections)
TRAVEL_DESTINATIONS = {
	[8]  = { 6, 9, 5, 14, 15, 25 },   -- Thais -> Carlin, Venore, Ab'D, LB, PH, Bounac
	[6]  = { 8, 5, 16 },              -- Carlin -> Thais, Ab'D, Svargrond
	[5]  = { 8, 6, 9, 11, 26 },       -- Ab'D -> Thais, Carlin, Venore, Edron, Feyrist
	[9]  = { 8, 5, 13, 10, 14, 15 },  -- Venore -> many destinations
	[11] = { 5, 9, 17, 12 },          -- Edron -> Ab'D, Venore, Yalahar, Farmine
	[7]  = { 8, 9 },                  -- Kazordoon -> Thais, Venore
	[12] = { 11 },                    -- Farmine -> Edron (steamship)
	[13] = { 9, 10, 14 },             -- Darashia -> Venore, Ankrahmun, LB
	[10] = { 9, 13, 14, 15 },         -- Ankrahmun -> Venore, Darashia, LB, PH
	[14] = { 8, 9, 10, 15 },          -- Liberty Bay -> Thais, Venore, Ankr, PH
	[15] = { 9, 10, 14 },             -- Port Hope -> Venore, Ankrahmun, LB
	[16] = { 6, 17 },                 -- Svargrond -> Carlin, Yalahar
	[17] = { 11, 16, 20, 19 },        -- Yalahar -> Edron, Svargrond, Rathleton, Krailos
	[19] = { 17 },                    -- Krailos -> Yalahar
	[20] = { 17, 22, 21 },            -- Rathleton -> Yalahar, Issavi, Roshamuul
	[21] = { 20 },                    -- Roshamuul -> Rathleton (boat)
	[22] = { 20 },                    -- Issavi -> Rathleton
	[25] = { 8 },                     -- Bounac -> Thais (boat)
	[26] = { 5 },                     -- Feyrist -> Ab'Dendriel (portal)
}

-- Chat phrases
BOT_CHAT = {
	idle = {
		"afk 5 min", "brb", "anyone want to hunt?", "nice server", "hi all",
		"looking for team", "selling loot", "checking depot", "nice outfit",
		"gg", "lol", "sup everyone", "back", "hey", "just chilling",
		"anyone online?", "love this server", "bored",
		"gonna go hunt soon", "need a healer", "ty",
		"gl hunting", "be right back", "morning", "any raids?",
	},
	depot = {
		"checking depot", "need to store some loot", "brb depot",
		"organizing my stuff", "where did i put that...",
	},
	shop = {
		"buying supplies", "need some pots", "selling loot",
		"how much for this?", "ty for the trade",
	},
	temple = {
		"need a heal", "checking blessings", "just died lol",
		"afk temple", "waiting for team",
	},
	npc = {
		"hi", "trade", "one moment", "done", "ty",
	},
	travel = {
		"heading out", "time to travel", "taking the boat",
		"gonna explore another city", "see you guys later",
		"brb switching cities", "off to trade elsewhere",
	},
	combat = {
		"bring it on!", "you want to fight?", "stop!",
		"leave me alone!", "help!",
	},
	flee = {
		"gotta run!", "nope nope nope", "help!!",
		"someone help me!", "running away!",
	},
	greeting = {
		"hi", "hey", "hello", "sup", "yo", "hail", "greetings",
		"good morning", "good evening", "howdy", "whats up",
	},
	hunting = {
		"nice loot", "exp is good here", "almost done",
		"need to refill soon", "this spawn is great",
		"one more round", "careful with that monster",
	},
}

-- Build closed->open door lookup from door tables (loaded from data/libs/tables/doors.lua)
-- Only regular and key doors — quest/level doors should NOT be opened by bots
CLOSED_TO_OPEN_DOOR = {}
if KeyDoorTable then
	for _, entry in ipairs(KeyDoorTable) do
		if entry.closedDoor and entry.openDoor then
			CLOSED_TO_OPEN_DOOR[entry.closedDoor] = entry.openDoor
		end
	end
end
if CustomDoorTable then
	for _, entry in ipairs(CustomDoorTable) do
		if entry.closedDoor and entry.openDoor then
			CLOSED_TO_OPEN_DOOR[entry.closedDoor] = entry.openDoor
		end
	end
end

-- Runtime state tables
BotPlayers = {}    -- {[guid] = playerRef}
BotState = {}      -- {[guid] = state_table}
BotActive = {}     -- {[guid] = true/false}

BotSystem = {}

-- ============================================================================
-- Core Navigation (Gesior-style: getPathTo -> startAutoWalk)
-- ============================================================================

function BotSystem.goTo(player, targetPos, state)
	if player:isWalking() then
		return true
	end

	-- Throttle pathfinding retries: if we failed recently, wait ~1s before trying again
	if state then
		local now = os.time()
		if state.goToFailTime and (now - state.goToFailTime) < 1 then
			return false
		end
	end

	local pos = player:getPosition()
	if pos.z ~= targetPos.z then
		return false
	end

	local dx = targetPos.x - pos.x
	local dy = targetPos.y - pos.y
	local dist = math.max(math.abs(dx), math.abs(dy))

	local walkTo = targetPos
	if dist > BOT_CONFIG.PATH_MAX_DIST then
		local ratio = BOT_CONFIG.WAYPOINT_DIST / dist
		walkTo = Position(
			math.floor(pos.x + dx * ratio),
			math.floor(pos.y + dy * ratio),
			pos.z
		)
	end

	local dirs = player:getPathTo(walkTo, 0, 3, true, false, BOT_CONFIG.PATH_MAX_DIST)
	if not dirs or type(dirs) ~= "table" or #dirs == 0 then
		-- Try small random offsets
		for _ = 1, 3 do
			local offsetPos = Position(
				walkTo.x + math.random(-2, 2),
				walkTo.y + math.random(-2, 2),
				walkTo.z
			)
			dirs = player:getPathTo(offsetPos, 0, 3, true, false, BOT_CONFIG.PATH_MAX_DIST)
			if dirs and type(dirs) == "table" and #dirs > 0 then
				break
			end
		end
		-- For long distances, try axis-aligned and larger offsets
		if (not dirs or type(dirs) ~= "table" or #dirs == 0) and dist > BOT_CONFIG.PATH_MAX_DIST then
			local stepDist = math.min(BOT_CONFIG.WAYPOINT_DIST, dist)
			local alternatives = {
				Position(pos.x + (dx > 0 and stepDist or -stepDist), pos.y, pos.z), -- x-only
				Position(pos.x, pos.y + (dy > 0 and stepDist or -stepDist), pos.z), -- y-only
				Position(pos.x + (dx > 0 and stepDist or -stepDist), pos.y + math.random(-10, 10), pos.z),
				Position(pos.x + math.random(-10, 10), pos.y + (dy > 0 and stepDist or -stepDist), pos.z),
			}
			for _, altPos in ipairs(alternatives) do
				dirs = player:getPathTo(altPos, 0, 3, true, false, BOT_CONFIG.PATH_MAX_DIST)
				if dirs and type(dirs) == "table" and #dirs > 0 then
					break
				end
			end
		end
		if not dirs or type(dirs) ~= "table" or #dirs == 0 then
			-- Try opening doors that may be blocking the path
			BotSystem.tryOpenDoors(player, walkTo)
			if state then state.goToFailTime = os.time() end
			return false
		end
	end

	if state then state.goToFailTime = nil end
	player:startAutoWalk(dirs)
	return true
end

-- ============================================================================
-- Z-Level Recovery (uses CITY_WALK_Z, not temple z, since some temples are underground)
-- ============================================================================

function BotSystem.getExpectedZ(player)
	local town = player:getTown()
	if not town then return 7 end
	local townId = town:getId()
	return CITY_WALK_Z[townId] or town:getTemplePosition().z
end

function BotSystem.getSpawnPosition(player)
	-- Get a walkable spawn position at the correct z-level for this city
	local town = player:getTown()
	if not town then return player:getPosition() end
	local townId = town:getId()
	local walkZ = CITY_WALK_Z[townId] or town:getTemplePosition().z
	local templePos = town:getTemplePosition()

	-- If temple z matches walk z, use temple position directly
	if templePos.z == walkZ then
		return templePos
	end

	-- Temple is on a different z (underground/tower) — use first POI position instead
	local pois = CITY_POIS[townId]
	if pois and #pois > 0 then
		return pois[1].pos
	end

	-- Fallback: temple position with z overridden
	return Position(templePos.x, templePos.y, walkZ)
end

function BotSystem.checkZLevel(player, state)
	-- Don't force z-recovery during combat pursuit, return navigation, party, or cavebot commands
	if state.state == BOT_STATE.COMBAT or state.state == BOT_STATE.FLEEING
		or state.state == BOT_STATE.PK_ATTACK or state.state == BOT_STATE.PARTY
		or state.returningHome or state.cavebotCommand
		or state.floorChangeState ~= FLOOR_CHANGE_STATE.NONE then
		return false
	end

	local pos = player:getPosition()
	local expectedZ = BotSystem.getExpectedZ(player)
	if pos.z == expectedZ then
		return false
	end

	local spawnPos = BotSystem.getSpawnPosition(player)
	logger.info("[Bot] " .. player:getName() ..
		" z-level recovery: was at z=" .. pos.z ..
		", expected z=" .. expectedZ .. ", teleporting to city surface")
	player:teleportTo(spawnPos)
	state.walkTarget = nil
	state.currentPOI = nil
	state.pathFailCount = 0
	return true
end

-- ============================================================================
-- Debug Logging & Walk History (cavebot debug framework)
-- ============================================================================

-- Rate-limit floor-change debug spam: suppress repeated FC messages per bot (30s cooldown)
local fcDebugLastLog = {} -- [botName] = os.time()
local FC_DEBUG_COOLDOWN = 30

function BotSystem.debugLog(state, msg)
	if state and state.verboseLog then
		-- Rate-limit floor-change state machine messages (SCANNING, WALKING_TO, FLOOR_CHANGE, etc.)
		local c1 = msg:sub(1, 1)
		local isFC = c1 == "S" and (msg:sub(1, 8) == "SCANNING" or msg:sub(1, 11) == "STEPPING_ON"
				or msg:sub(1, 8) == "Starting")
			or c1 == "W" and msg:sub(1, 10) == "WALKING_TO"
			or c1 == "V" and msg:sub(1, 9) == "VERIFYING"
			or c1 == "F" and msg:sub(1, 12) == "FLOOR_CHANGE"
			or c1 == "U" and msg:sub(1, 10) == "USING_ITEM"
			or c1 == " "
		if isFC then
			local name = state.botName or "?"
			local now = os.time()
			if fcDebugLastLog[name] and (now - fcDebugLastLog[name]) < FC_DEBUG_COOLDOWN then
				return
			end
			fcDebugLastLog[name] = now
		end
		logger.info("[CavebotDebug:" .. (state.botName or "?") .. "] " .. msg)
	end
end

-- Cast Chat channel ID (0x50 = 80, defined in const.hpp as CHANNEL_CAST)
local CHANNEL_CAST = 0x50
-- Talk type for white text in Cast Chat (CHANNEL_MANAGER = 6 renders white in OTClient)
local CAST_TALK_TYPE = TALKTYPE_CHANNEL_Y

-- Send debug message to Cast Chat channel (visible to cast viewers).
-- Only active when verbose logging is enabled for this bot.
function BotSystem.castDebug(player, state, msg)
	if not player then return end
	if state and state.verboseLog then
		player:sendChannelMessage("", "[Bot] " .. msg, CAST_TALK_TYPE, CHANNEL_CAST)
		logger.info("[CavebotDebug:" .. (state.botName or "?") .. "] " .. msg)
	end
end

function BotSystem.recordPosition(state, pos)
	state.walkHistoryIdx = (state.walkHistoryIdx % 8) + 1
	state.walkHistory[state.walkHistoryIdx] = { x = pos.x, y = pos.y, z = pos.z }
end

function BotSystem.isStuck(state)
	if not state.walkHistory or #state.walkHistory < 5 then return false end
	local ref = state.walkHistory[state.walkHistoryIdx]
	if not ref then return false end
	local sameCount = 0
	for _, p in ipairs(state.walkHistory) do
		if p.x == ref.x and p.y == ref.y and p.z == ref.z then
			sameCount = sameCount + 1
		end
	end
	return sameCount >= 5
end

-- ============================================================================
-- Floor Change State Machine (multi-tick, BBot-inspired)
-- ============================================================================

-- Compute the correct destination position for a floor-change tile based on its flags
function BotSystem.computeFloorChangeDestination(fcPos, goDown)
	local destX, destY, destZ = fcPos.x, fcPos.y, fcPos.z
	if goDown then
		destZ = fcPos.z + 1
		-- For going DOWN: the tile at the current z has FLOORCHANGE_DOWN flag;
		-- the tile below determines directional offset
		local belowTile = Tile(Position(fcPos.x, fcPos.y, fcPos.z + 1))
		if belowTile then
			if belowTile:hasFlag(TILESTATE_FLOORCHANGE_NORTH) then destY = destY + 1 end
			if belowTile:hasFlag(TILESTATE_FLOORCHANGE_SOUTH) then destY = destY - 1 end
			if belowTile:hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT) then destY = destY - 2 end
			if belowTile:hasFlag(TILESTATE_FLOORCHANGE_EAST) then destX = destX - 1 end
			if belowTile:hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT) then destX = destX - 2 end
			if belowTile:hasFlag(TILESTATE_FLOORCHANGE_WEST) then destX = destX + 1 end
		end
	else
		destZ = fcPos.z - 1
		-- For going UP: the tile at the current z has FLOORCHANGE flags with directions
		local tile = Tile(fcPos)
		if tile then
			if tile:hasFlag(TILESTATE_FLOORCHANGE_NORTH) then destY = destY - 1 end
			if tile:hasFlag(TILESTATE_FLOORCHANGE_SOUTH) then destY = destY + 1 end
			if tile:hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT) then destY = destY + 2 end
			if tile:hasFlag(TILESTATE_FLOORCHANGE_EAST) then destX = destX + 1 end
			if tile:hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT) then destX = destX + 2 end
			if tile:hasFlag(TILESTATE_FLOORCHANGE_WEST) then destX = destX - 1 end
		end
	end
	return Position(destX, destY, destZ)
end

-- Main floor-change handler — called when floorChangeState ~= NONE
function BotSystem.handleFloorChange(player, state)
	local pos = player:getPosition()

	-- Timeout: if >15 seconds in any non-NONE state, force FAILED
	if state.floorChangeStartTime and (os.time() - state.floorChangeStartTime) > 15 then
		BotSystem.debugLog(state, "FLOOR_CHANGE TIMEOUT after 15 seconds")
		state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
	end

	if state.floorChangeState == FLOOR_CHANGE_STATE.SCANNING then
		-- Find transitions nearby
		local goDown = state.floorChangeTarget and state.floorChangeTarget.goDown or false
		local targetPos = state.floorChangeTarget and state.floorChangeTarget.targetPos or pos

		local allTransitions = {}
		local seen = {}
		local function addFrom(center)
			local transitions = BotSystem.findZTransitions(center, 12, goDown)
			for _, entry in ipairs(transitions) do
				local key = entry.pos.x .. "," .. entry.pos.y .. "," .. entry.type
				if not seen[key] then
					seen[key] = true
					table.insert(allTransitions, entry)
				end
			end
		end

		addFrom(Position(pos.x, pos.y, pos.z))
		if targetPos then
			addFrom(Position(targetPos.x, targetPos.y, pos.z))
		end

		-- Use extra search centers if provided (e.g., midpoint between hunt waypoints)
		local extras = state.floorChangeTarget and state.floorChangeTarget.extraSearchCenters
		if extras then
			for _, center in ipairs(extras) do
				addFrom(Position(center.x, center.y, pos.z))
			end
		end

		-- Sort by distance from bot
		table.sort(allTransitions, function(a, b)
			local da = math.abs(pos.x - a.pos.x) + math.abs(pos.y - a.pos.y)
			local db = math.abs(pos.x - b.pos.x) + math.abs(pos.y - b.pos.y)
			return da < db
		end)

		BotSystem.debugLog(state, "SCANNING: found " .. #allTransitions .. " transitions ("
			.. (goDown and "DOWN" or "UP") .. ")")
		for i, t in ipairs(allTransitions) do
			BotSystem.debugLog(state, "  #" .. i .. ": " .. t.type .. " at ("
				.. t.pos.x .. "," .. t.pos.y .. "," .. t.pos.z .. ") dist=" .. t.dist)
		end

		if #allTransitions == 0 then
			BotSystem.debugLog(state, "SCANNING: no transitions found, FAILED")
			state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
			return
		end

		state.floorChangeTransitions = allTransitions
		state.floorChangeTransIdx = 1
		state.floorChangeState = FLOOR_CHANGE_STATE.WALKING_TO
		BotSystem.debugLog(state, "SCANNING -> WALKING_TO (target: " .. allTransitions[1].type
			.. " at " .. allTransitions[1].pos.x .. "," .. allTransitions[1].pos.y .. ")")

	elseif state.floorChangeState == FLOOR_CHANGE_STATE.WALKING_TO then
		if player:isWalking() then return end

		local trans = state.floorChangeTransitions[state.floorChangeTransIdx]
		if not trans then
			state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
			return
		end

		local fc = trans.pos
		local dist = math.max(math.abs(pos.x - fc.x), math.abs(pos.y - fc.y))

		BotSystem.debugLog(state, "WALKING_TO: " .. trans.type .. " at ("
			.. fc.x .. "," .. fc.y .. "," .. fc.z .. ") dist=" .. dist)

		if trans.type == "ladder" or trans.type == "sewer" or trans.type == "rope" then
			if dist == 0 then
				-- On the tile: transition to USING_ITEM
				state.floorChangeState = FLOOR_CHANGE_STATE.USING_ITEM
				BotSystem.debugLog(state, "WALKING_TO -> USING_ITEM (on " .. trans.type .. " tile)")
				return
			end
			-- Path directly to the tile (ladders/sewers aren't FLOORCHANGE, A* accepts them)
			local dirs = player:getPathTo(fc, 0, 0, true, false, BOT_CONFIG.PATH_MAX_DIST)
			if dirs and type(dirs) == "table" and #dirs > 0 then
				player:startAutoWalk(dirs)
				BotSystem.debugLog(state, "WALKING_TO: pathing to " .. trans.type .. " tile, " .. #dirs .. " steps")
			else
				BotSystem.debugLog(state, "WALKING_TO: path failed, trying next transition")
				state.floorChangeTransIdx = state.floorChangeTransIdx + 1
				if state.floorChangeTransIdx > #state.floorChangeTransitions then
					state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
				end
			end
		else
			-- Stairs (FLOORCHANGE tiles) — A* rejects them, path to adjacent
			if dist == 0 then
				-- On stairs but z didn't change — go to STEPPING_ON which will do teleport fallback
				state.floorChangeState = FLOOR_CHANGE_STATE.STEPPING_ON
				BotSystem.debugLog(state, "WALKING_TO -> STEPPING_ON (already on stairs)")
				return
			end
			if dist == 1 then
				-- Adjacent — check if diagonal
				local isDiagonal = (pos.x ~= fc.x and pos.y ~= fc.y)
				if isDiagonal then
					-- Route through a cardinal-adjacent tile first
					local cardinalAdj = {
						Position(fc.x, pos.y, pos.z),
						Position(pos.x, fc.y, pos.z),
					}
					local stepped = false
					for _, adj in ipairs(cardinalAdj) do
						local tile = Tile(adj)
						if tile and tile:getGround() and not tile:hasFlag(TILESTATE_BLOCKSOLID) then
							local d = Position.getDirectionTo(pos, adj)
							if d then
								player:startAutoWalk({ d })
								BotSystem.debugLog(state, "WALKING_TO: diagonal fix -> stepping cardinal to ("
									.. adj.x .. "," .. adj.y .. ")")
								stepped = true
								break
							end
						end
					end
					if not stepped then
						-- Can't find cardinal route, try diagonal step directly
						local stepDir = Position.getDirectionTo(pos, fc)
						if stepDir then
							player:startAutoWalk({ stepDir })
							BotSystem.debugLog(state, "WALKING_TO: diagonal direct step toward stairs")
						end
					end
				else
					-- Cardinal adjacent — go to STEPPING_ON
					state.floorChangeState = FLOOR_CHANGE_STATE.STEPPING_ON
					BotSystem.debugLog(state, "WALKING_TO -> STEPPING_ON (adjacent, dist=1)")
				end
				return
			end
			-- Far from stairs — path to within 1 tile
			local dirs = player:getPathTo(fc, 1, 1, true, false, BOT_CONFIG.PATH_MAX_DIST)
			if dirs and type(dirs) == "table" and #dirs > 0 then
				player:startAutoWalk(dirs)
				BotSystem.debugLog(state, "WALKING_TO: pathing near stairs, " .. #dirs .. " steps")
			else
				-- Try opening doors between us and the stairs
				BotSystem.tryOpenDoors(player, fc)
				BotSystem.debugLog(state, "WALKING_TO: path near stairs failed, trying next transition")
				state.floorChangeTransIdx = state.floorChangeTransIdx + 1
				if state.floorChangeTransIdx > #state.floorChangeTransitions then
					state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
				end
			end
		end

	elseif state.floorChangeState == FLOOR_CHANGE_STATE.STEPPING_ON then
		if player:isWalking() then return end

		local trans = state.floorChangeTransitions[state.floorChangeTransIdx]
		if not trans then
			state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
			return
		end

		local fc = trans.pos
		local dist = math.max(math.abs(pos.x - fc.x), math.abs(pos.y - fc.y))

		state.floorChangePreZ = pos.z
		state.floorChangeAttempts = state.floorChangeAttempts + 1

		if dist == 0 then
			-- Already on stairs tile but z hasn't changed — use teleport fallback
			local goDown = state.floorChangeTarget and state.floorChangeTarget.goDown or false
			local destPos = BotSystem.computeFloorChangeDestination(fc, goDown)
			BotSystem.debugLog(state, "STEPPING_ON: on stairs, teleport fallback to ("
				.. destPos.x .. "," .. destPos.y .. "," .. destPos.z .. ")")
			player:teleportTo(destPos, true)
			state.floorChangeState = FLOOR_CHANGE_STATE.VERIFYING
			return
		end

		if dist == 1 then
			-- Step onto the stair tile
			local stepDir = Position.getDirectionTo(pos, fc)
			if stepDir then
				-- Use creature:move() (FLAG_NOLIMIT) to guarantee stepping onto floor-change tile
				player:move(stepDir)
				BotSystem.debugLog(state, "STEPPING_ON: move(" .. stepDir .. ") toward stairs at ("
					.. fc.x .. "," .. fc.y .. "," .. fc.z .. ")")
				state.floorChangeState = FLOOR_CHANGE_STATE.VERIFYING
			else
				BotSystem.debugLog(state, "STEPPING_ON: no direction to stairs? FAILED")
				state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
			end
		else
			-- Still too far — go back to WALKING_TO
			BotSystem.debugLog(state, "STEPPING_ON: dist=" .. dist .. ", back to WALKING_TO")
			state.floorChangeState = FLOOR_CHANGE_STATE.WALKING_TO
		end

	elseif state.floorChangeState == FLOOR_CHANGE_STATE.VERIFYING then
		-- Check if z changed (give 1-2 ticks for the move to process)
		local preZ = state.floorChangePreZ
		if not preZ then
			state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
			return
		end

		if pos.z ~= preZ then
			-- Z changed — success!
			BotSystem.debugLog(state, "VERIFYING: SUCCESS z=" .. preZ .. " -> " .. pos.z)
			state.floorChangeState = FLOOR_CHANGE_STATE.COMPLETED
		else
			-- Z didn't change — retry with creature:move() or try next transition
			if state.floorChangeAttempts < 3 then
				BotSystem.debugLog(state, "VERIFYING: z still " .. pos.z .. " (attempt "
					.. state.floorChangeAttempts .. "/3), retrying with move()")
				local trans = state.floorChangeTransitions[state.floorChangeTransIdx]
				if trans then
					local fc = trans.pos
					local dist = math.max(math.abs(pos.x - fc.x), math.abs(pos.y - fc.y))
					if dist <= 1 then
						local stepDir = Position.getDirectionTo(pos, fc)
						if stepDir then
							player:move(stepDir) -- FLAG_NOLIMIT
						end
					end
				end
				state.floorChangeState = FLOOR_CHANGE_STATE.STEPPING_ON
			else
				-- 3 attempts failed — try next transition in list
				BotSystem.debugLog(state, "VERIFYING: 3 attempts failed, trying next transition")
				state.floorChangeTransIdx = state.floorChangeTransIdx + 1
				state.floorChangeAttempts = 0
				if state.floorChangeTransIdx > #state.floorChangeTransitions then
					state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
				else
					state.floorChangeState = FLOOR_CHANGE_STATE.WALKING_TO
				end
			end
		end

	elseif state.floorChangeState == FLOOR_CHANGE_STATE.USING_ITEM then
		-- For ladders/sewers: simulate "use" by teleporting to destination
		local trans = state.floorChangeTransitions[state.floorChangeTransIdx]
		if not trans then
			state.floorChangeState = FLOOR_CHANGE_STATE.FAILED
			return
		end

		local fc = trans.pos
		state.floorChangePreZ = pos.z

		if trans.type == "ladder" then
			-- Ladder goes UP: z-1, and offset 1 tile south (+y) to land next to the hole
			local destPos = Position(fc.x, fc.y - 1, fc.z - 1)
			player:teleportTo(destPos, true)
			BotSystem.debugLog(state, "USING_ITEM: ladder at (" .. fc.x .. "," .. fc.y .. "," .. fc.z
				.. ") -> teleport to (" .. destPos.x .. "," .. destPos.y .. "," .. destPos.z .. ")")
		elseif trans.type == "sewer" then
			-- Sewer/trapdoor goes DOWN: z+1 (no offset — you land on the same x,y below)
			local destPos = Position(fc.x, fc.y, fc.z + 1)
			player:teleportTo(destPos, true)
			BotSystem.debugLog(state, "USING_ITEM: sewer at (" .. fc.x .. "," .. fc.y .. "," .. fc.z
				.. ") -> teleport to (" .. destPos.x .. "," .. destPos.y .. "," .. destPos.z .. ")")
		elseif trans.type == "rope" then
			-- Rope goes UP — use moveUpstairs() same as magic_rope.lua
			local ropePos = Position(fc.x, fc.y, fc.z)
			local destPos = ropePos:moveUpstairs()
			player:teleportTo(destPos, true)
			BotSystem.debugLog(state, "USING_ITEM: rope at (" .. fc.x .. "," .. fc.y .. "," .. fc.z
				.. ") -> teleport to (" .. destPos.x .. "," .. destPos.y .. "," .. destPos.z .. ")")
		end

		state.floorChangeState = FLOOR_CHANGE_STATE.VERIFYING

	elseif state.floorChangeState == FLOOR_CHANGE_STATE.COMPLETED then
		BotSystem.debugLog(state, "FLOOR_CHANGE COMPLETED at (" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")")
		local source = state.floorChangeTarget and state.floorChangeTarget.source or nil
		BotSystem.resetFloorChangeState(state)
		state.floorChangeResult = "completed"
		state.floorChangeSource = source

	elseif state.floorChangeState == FLOOR_CHANGE_STATE.FAILED then
		BotSystem.debugLog(state, "FLOOR_CHANGE FAILED at (" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")")
		local source = state.floorChangeTarget and state.floorChangeTarget.source or nil
		BotSystem.resetFloorChangeState(state)
		state.floorChangeResult = "failed"
		state.floorChangeSource = source
	end
end

function BotSystem.resetFloorChangeState(state)
	state.floorChangeState = FLOOR_CHANGE_STATE.NONE
	state.floorChangeTarget = nil
	state.floorChangePreZ = nil
	state.floorChangeAttempts = 0
	state.floorChangeStartTime = nil
	state.floorChangeTransitions = nil
	state.floorChangeTransIdx = 0
	-- Note: floorChangeResult and floorChangeSource are NOT cleared here
	-- They persist for one tick so callers can check the outcome
end

-- Initiate a floor change toward a target z-level
-- extraSearchCenters: optional table of Position objects to also scan for transitions
-- source: optional string identifying what triggered this ("hunt", "cavebot", etc.)
function BotSystem.startFloorChange(state, goDown, targetPos, extraSearchCenters, source)
	state.floorChangeState = FLOOR_CHANGE_STATE.SCANNING
	state.floorChangeTarget = {
		goDown = goDown,
		targetPos = targetPos,
		extraSearchCenters = extraSearchCenters,
		source = source or "unknown",
	}
	state.floorChangePreZ = nil
	state.floorChangeAttempts = 0
	state.floorChangeStartTime = os.time()
	state.floorChangeTransitions = nil
	state.floorChangeTransIdx = 0
	BotSystem.debugLog(state, "Starting floor change: " .. (goDown and "DOWN" or "UP")
		.. " toward (" .. (targetPos and (targetPos.x .. "," .. targetPos.y .. "," .. targetPos.z) or "nil") .. ")"
		.. " source=" .. (source or "unknown")
		.. (extraSearchCenters and (" +" .. #extraSearchCenters .. " extra centers") or ""))
end

-- ============================================================================
-- Shared reload — invoked by /cavebot reload talkaction AND by bot_commands queue.
-- opts = {
--   source       = "talkaction" | "queue" | nil  -- caller tag for logs
--   player       = <player obj> | nil           -- for sendTextMessage feedback (talkaction only)
--   debugCount   = nil | <N> | "off"            -- nil = no debug-mode change (preserve active set)
--                                                -- N = re-activate only the first N online bots, disable debug for others
--                                                -- "off" = re-activate ALL bots in BotPlayers, clear debug streams
--   debugBotName = nil | <name>                 -- explicit bot to debug (overrides "first online")
-- }
-- Returns { ok = bool, message = string, lines = { ... } }
-- Re-entrancy guard: reload is async (step 4b stagger), so a second invocation
-- arriving mid-materialization would corrupt counter state. Module-level flag
-- blocks entry until finalizeReload clears it.
BotSystem._reloadInProgress = BotSystem._reloadInProgress or false

function BotSystem.executeReload(opts)
	opts = opts or {}
	local source = opts.source or "?"
	local lines = {}
	local function emit(msg)
		lines[#lines + 1] = msg
		-- Guard isRemoved — async window is wider than before; talkaction player
		-- could log out between when the reload starts and when finalizeReload fires.
		if opts.player and not opts.player:isRemoved() then
			opts.player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. msg)
		end
		logger.info("[reload:{}] {}", source, msg)
	end

	if BotSystem._reloadInProgress then
		emit("reload already in progress — ignoring concurrent request")
		return { ok = false, message = "reload already in progress", lines = lines }
	end
	BotSystem._reloadInProgress = true

	-- Steps 1-4 are synchronous; wrap in pcall so a Lua error in the prefix
	-- (e.g., unexpected return type from a C++ binding) clears the re-entrancy
	-- flag and surfaces the error instead of leaving _reloadInProgress=true
	-- forever (which would block every future reload until canary restarts).
	local onlineGuids, hibernatedNames
	local prefixOk, prefixErr = pcall(function()
		-- 1. Detect currently-active bots (via C++ engine active flag)
		onlineGuids = {}
		for guid, p in pairs(BotPlayers or {}) do
			if p and not p:isRemoved() and Game.botIsActive(guid) then
				onlineGuids[#onlineGuids + 1] = guid
			end
		end
		emit("Found " .. #onlineGuids .. " online bots to preserve")

		-- 1b. Snapshot hibernated bot names from OLD engine BEFORE force-deactivate / dlclose.
		-- The hibernationPool_ (in the .so) holds the only strong refs to hibernated Player
		-- objects; those refs die with the .so unload. We re-materialize them from DB after
		-- the reload via Game.loadBotPlayer(name) — same path as startup.
		hibernatedNames = {}
		local hibStates = Game.getBotHibernationStates()
		if hibStates then
			for i = 1, #hibStates do
				local b = hibStates[i]
				if b.hibernated then
					hibernatedNames[#hibernatedNames + 1] = b.name
				end
			end
		end
		emit("Found " .. #hibernatedNames .. " hibernated bots to re-load from DB")

		-- 2. Force-deactivate all online bots in OLD engine (clears hunt reservations, preserves cast streams)
		for _, guid in ipairs(onlineGuids) do
			Game.botForceDeactivateForReload(guid)
		end

		-- 3. Reload the .so + hunt data — this destroys old hibernationPool_
		local result = Game.botReload()
		emit(tostring(result))

		-- 4. Restart the 100ms tick loop (old cycleEvent may be stale after dlclose/dlopen)
		Game.botStartTickLoop()
	end)
	if not prefixOk then
		BotSystem._reloadInProgress = false
		logger.error("[reload:{}] prefix error: {}", source, tostring(prefixErr))
		return { ok = false, message = "reload prefix failed: " .. tostring(prefixErr), lines = lines }
	end

	-- 4b. Re-materialize hibernated bots from DB asynchronously.
	-- luaAddEvent clamps delay to min 100ms (global_functions.cpp:702), so passing no
	-- delay puts all N callbacks at the same T+100ms deadline → all fire in one
	-- dispatcher sweep → ~150ms × N freeze → real-player ping-timeout kicks at
	-- N>=500. We index-stagger so each bot lands on its own dispatcher iteration.
	-- After the last materialization completes, finalizeReload runs steps 5-9.
	local BOT_MATERIALIZE_INTERVAL_MS = 100
	local materializedCount = 0
	local pendingMaterializations = #hibernatedNames

	-- All post-materialization steps live inside finalizeReload so they wait for
	-- step 4b to complete. Captured locals: opts, lines, emit, onlineGuids, source.
	-- Defensive double-call guard + pcall wrapper: if finalizeReload errors out
	-- partway through, we MUST still clear _reloadInProgress, otherwise the
	-- re-entrancy guard locks out all future reloads. The `finalized` flag
	-- prevents accidental re-entry if some future change calls it twice.
	local finalized = false
	local function finalizeReload()
		if finalized then
			logger.warn("[reload:{}] finalizeReload called twice — skipping", source)
			return
		end
		finalized = true
		local finalizeOk, finalizeErr = pcall(function()
		if materializedCount > 0 then
			emit("Re-materialized " .. materializedCount .. " hibernated bots from DB")
		end

		-- 5. Re-register all bot players with the new engine (awake survivors + freshly materialized)
		local count = Game.botReregisterAll()
		emit("Re-registered " .. count .. " bots.")

		-- 5b. Defensive orphan recovery: any bot in BotPlayers that's still not active in
		-- the new engine (e.g., botReregisterAll missed it for some edge case reason).
		-- With the materialize-from-DB step above, this loop normally finds 0; kept as a
		-- safety net.
		local orphanCount = 0
		for guid, p in pairs(BotPlayers or {}) do
			if p and not Game.botIsActive(guid) then
				if Game.botRecoverOrphanForReload(guid, p) then
					orphanCount = orphanCount + 1
				end
			end
		end
		if orphanCount > 0 then
			emit("Recovered " .. orphanCount .. " orphan bot(s)")
		end

		-- 6. Determine target set for re-activation based on debugCount
		local debugCount = opts.debugCount
		local activateGuids = {}
		local debugTargets = {}

		if debugCount == "off" then
			-- Clear persisted debug mode, re-activate everything
			db.query("INSERT INTO `bot_startup_config` (`config_key`, `config_value`) "
				.. "VALUES ('debug_active_count', '0') "
				.. "ON DUPLICATE KEY UPDATE `config_value` = '0'")
			-- Re-enable C++ population scheduler so it can manage normal active counts
			Game.botCommand("_global", "schedule on")
			emit("scheduler ON")
			for guid, _ in pairs(BotPlayers or {}) do
				activateGuids[#activateGuids + 1] = guid
			end
			emit("debug mode OFF — activating ALL " .. #activateGuids .. " registered bots")
		elseif type(debugCount) == "number" and debugCount > 0 then
			-- Persist new debug count
			db.query("INSERT INTO `bot_startup_config` (`config_key`, `config_value`) "
				.. "VALUES ('debug_active_count', '" .. debugCount .. "') "
				.. "ON DUPLICATE KEY UPDATE `config_value` = '" .. debugCount .. "'")
			-- Disable C++ population scheduler — otherwise it re-activates the 199 we just turned off
			Game.botCommand("_global", "schedule off")
			emit("scheduler OFF (debug mode controls active set)")

			-- Pick which bots to debug
			-- Priority: explicit opts.debugBotName > BOT_CONFIG.DEBUG_BOT_NAME > first N from onlineGuids > first N from BotPlayers
			local preferredName = opts.debugBotName
			if not preferredName and BOT_CONFIG.DEBUG_BOT_NAME and BOT_CONFIG.DEBUG_BOT_NAME ~= "" then
				preferredName = BOT_CONFIG.DEBUG_BOT_NAME
			end

			local picked = {}
			if preferredName then
				local prefP = Player(preferredName)
				if prefP then
					local prefGuid = prefP:getGuid()
					if BotPlayers[prefGuid] then
						picked[prefGuid] = true
						activateGuids[#activateGuids + 1] = prefGuid
					end
				end
			end

			-- Fill remaining slots from onlineGuids (preserves "previously online" preference)
			for _, guid in ipairs(onlineGuids) do
				if #activateGuids >= debugCount then break end
				if not picked[guid] then
					picked[guid] = true
					activateGuids[#activateGuids + 1] = guid
				end
			end

			-- Still not enough? Fall back to any registered bot
			if #activateGuids < debugCount then
				for guid, _ in pairs(BotPlayers or {}) do
					if #activateGuids >= debugCount then break end
					if not picked[guid] then
						picked[guid] = true
						activateGuids[#activateGuids + 1] = guid
					end
				end
			end

			-- These are the debug targets — debug stream will be enabled on them
			for _, guid in ipairs(activateGuids) do
				debugTargets[guid] = true
			end
			emit("debug mode ON — activating " .. #activateGuids .. " bot(s); rest stay offline")
		else
			-- No debug change: preserve previously-online set
			activateGuids = onlineGuids
		end

		-- 7. Re-activate selected bots
		-- Primary path: botReactivateForReload preserves state for bots that were active before the reload.
		-- Fallback: botActivate is the canonical fresh-activation path (teleport to temple, state=IDLE) for
		-- bots that weren't in a preserved state — e.g., during "debug off" we want to bring back bots that
		-- may have been INACTIVE before the debug session started.
		local reactivated = 0
		local fallbackActivated = 0
		for _, guid in ipairs(activateGuids) do
			if Game.botReactivateForReload(guid) then
				reactivated = reactivated + 1
			elseif Game.botActivate(guid) then
				reactivated = reactivated + 1
				fallbackActivated = fallbackActivated + 1
			end
		end
		if fallbackActivated > 0 then
			emit("Re-activated " .. reactivated .. "/" .. #activateGuids .. " bots ("
				.. fallbackActivated .. " via fresh-activate fallback).")
		else
			emit("Re-activated " .. reactivated .. "/" .. #activateGuids .. " bots.")
		end

		-- 8. Toggle debug stream on the chosen targets (one per active bot in debug mode)
		if debugCount == "off" then
			-- Best-effort: turn off debug for all bots that might have had it on
			for guid, _ in pairs(BotPlayers or {}) do
				local p = BotPlayers[guid]
				if p and not p:isRemoved() then
					Game.botCommand(p:getName(), "debug off")
				end
			end
		elseif type(debugCount) == "number" and debugCount > 0 then
			for guid, _ in pairs(debugTargets) do
				local p = BotPlayers[guid]
				if p and not p:isRemoved() then
					local r = Game.botCommand(p:getName(), "debug on")
					emit("debug stream enabled on '" .. p:getName() .. "' — " .. tostring(r))
				end
			end
		end

		-- 9. Show git commit info so admin knows what version is running (talkaction-only — slow popen)
		if opts.source == "talkaction" then
			local handle = io.popen("git log -1 --pretty=format:'%h %s' 2>/dev/null")
			if handle then
				local gitInfo = handle:read("*a")
				handle:close()
				if gitInfo and gitInfo ~= "" then
					emit("Rev: " .. gitInfo)
				end
			end
		end

		emit("reload finalized; active=" .. reactivated .. " debug=" .. (debugCount and tostring(debugCount) or "preserve"))
		end) -- pcall wrap of finalize body
		if not finalizeOk then
			logger.error("[reload:{}] finalize error: {}", source, tostring(finalizeErr))
			-- Surface to admin if possible (player ref may be stale by now — emit guards it)
			emit("reload finalize FAILED: " .. tostring(finalizeErr))
		end
		BotSystem._reloadInProgress = false
	end -- finalizeReload

	-- Dispatch step 4b: stagger each loadBotPlayer onto its own dispatcher iteration.
	-- pcall guards per-bot failures so the counter still decrements even if one bot
	-- errors out — otherwise pendingMaterializations would never hit 0 and the reload
	-- would stay "in progress" forever, blocking future reloads (re-entrancy guard).
	if pendingMaterializations == 0 then
		finalizeReload()
	else
		for i, name in ipairs(hibernatedNames) do
			addEvent(function()
				local ok, err = pcall(function()
					if not Player(name) then
						if Game.loadBotPlayer(name) then
							materializedCount = materializedCount + 1
						end
					end
				end)
				if not ok then
					logger.error("[reload:materialize] error loading bot '" .. name .. "': " .. tostring(err))
				end
				pendingMaterializations = pendingMaterializations - 1
				if pendingMaterializations == 0 then
					finalizeReload()
				end
			end, i * BOT_MATERIALIZE_INTERVAL_MS)
		end
	end

	-- Sync return: caller (bot_commands queue) gets an "in progress" message.
	-- Final state lands in the journal via emit() inside finalizeReload.
	return {
		ok = true,
		message = (#hibernatedNames > 0)
			and ("reload started; materializing " .. #hibernatedNames .. " hibernated bots async (~"
				 .. math.ceil(#hibernatedNames * BOT_MATERIALIZE_INTERVAL_MS / 1000) .. "s)")
			or "reload started; no hibernated bots to materialize",
		lines = lines,
	}
end

-- ============================================================================
-- Command Queue Polling (MySQL-based, for automated testing)
-- ============================================================================

-- Global command queue poll: ONE query for all bots (called from bot_manager every 10s)
function BotSystem.pollGlobalCommandQueue()
	local resultId = db.storeQuery(
		"SELECT `id`, `bot_name`, `command` FROM `bot_commands` WHERE `processed` = 0 ORDER BY `id` ASC LIMIT 10")
	if not resultId then return end
	repeat
		local cmdId = Result.getNumber(resultId, "id")
		local botName = Result.getString(resultId, "bot_name")
		local cmd = Result.getString(resultId, "command")
		-- Mark processed immediately
		db.botAsyncQuery("UPDATE `bot_commands` SET `processed` = 1 WHERE `id` = " .. cmdId)
		-- Find the bot and execute
		local player = Player(botName)
		if player then
			local guid = player:getGuid()
			local state = BotState[guid]
			if state then
				logger.info("[CavebotDebug:" .. botName .. "] Command from queue: " .. cmd)
				local result = BotSystem.executeCavebotCommand(player, state, cmd)
				if result then
					logger.info("[CavebotDebug:" .. botName .. "] " .. result)
					BotSystem.castDebug(player, state, "CMD result: " .. result)
				end
			end
		else
			logger.warn("[CavebotDebug:" .. botName .. "] Bot not found for command: " .. cmd)
		end
	until not Result.next(resultId)
	Result.free(resultId)
end

-- Clear all active tasks/state so a new command starts clean
function BotSystem.clearAllTasks(player, state)
	-- Release hunt reservation
	if state.huntScriptId then
		BotHuntData.releaseHunt(state.huntScriptId, player:getGuid())
	end
	-- Hunt state
	state.huntScriptId = nil
	state.huntPhase = nil
	state.huntWaypoints = nil
	state.huntWaypointIdx = 0
	state.huntTargets = nil
	state.huntStartTime = nil
	state.huntEndTime = nil
	state.huntKillCount = nil
	state.huntTownId = nil
	state.huntMonsterTarget = nil
	state.huntWaypointSkipCount = 0
	state.huntLastPos = nil
	state.huntCycles = nil
	state.huntScanCooldown = nil
	-- Cavebot/navigation state
	state.cavebotCommand = nil
	state.cavebotTarget = nil
	state.cavebotNavRoute = nil
	state.cavebotNavSrc = nil
	state.cavebotNavDest = nil
	state.cavebotSequence = nil
	state.cavebotSequenceIdx = nil
	state.routeIdx = nil
	state.routeStuckCount = nil
	state.routeStuckSince = nil
	state.routeStuckLogged = nil
	state.navDepotTarget = nil
	state.navDepotWaitUntil = nil
	state.seqDepotTarget = nil
	state.seqWaitUntil = nil
	state.seqWaitPOI = nil
	-- Travel state
	state.travelDestTownId = nil
	state.travelDestTownName = nil
	state.travelPhase = nil
	state.travelBoatNpcPos = nil
	state.travelWaitUntil = nil
	state.walkToPOITarget = nil
	state.walkToPOIFailCount = nil
	state.pendingHuntAfterTravel = nil
	-- Resupply state
	state.resupplyStep = nil
	state.resupplyUntil = 0
	state.resupplyWalkTarget = nil
	state.resupplyRoute = nil
	state.resupplyStartTime = nil
	state.resupplyDepotTarget = nil
	state.resupplyLastPOI = nil
	-- General
	state.walkTarget = nil
	state.currentPOI = nil
	BotSystem.resetFloorChangeState(state)
	-- Clear target
	if player:getTarget() then player:setTarget(nil) end
end

-- Parse and execute a cavebot command string
function BotSystem.executeCavebotCommand(player, state, cmdStr)
	local cmd, args = cmdStr:match("^(%S+)%s*(.*)")
	if not cmd then return end
	cmd = cmd:lower()
	local pos = player:getPosition()

	if cmd == "status" then
		local town = player:getTown()
		local townName = town and town:getName() or "?"
		local msg = "Status: " .. player:getName()
			.. " pos=(" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")"
			.. " state=" .. state.state
			.. " city=" .. townName
			.. " hp=" .. player:getHealth() .. "/" .. player:getMaxHealth()
		if state.huntScriptId then
			local scriptName = BotHuntData.scripts[state.huntScriptId]
				and BotHuntData.scripts[state.huntScriptId].name or "?"
			msg = msg .. " hunt=" .. scriptName
				.. " phase=" .. (state.huntPhase or "?")
				.. " wp=" .. (state.huntWaypointIdx or 0)
		end
		if state.cavebotCommand then
			msg = msg .. " cmd=" .. state.cavebotCommand
		end
		msg = msg .. " fc_state=" .. state.floorChangeState
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. msg)
		return msg

	elseif cmd == "pos" then
		local msg = player:getName() .. " at (" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")"
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. msg)
		return msg

	elseif cmd == "goto" then
		BotSystem.clearAllTasks(player, state)
		local x, y, z = args:match("(%d+),(%d+),(%d+)")
		if not x then
			x, y, z = args:match("(%d+)%s+(%d+)%s+(%d+)")
		end
		if not x then return "Usage: goto x,y,z" end
		state.cavebotCommand = "goto"
		state.cavebotTarget = Position(tonumber(x), tonumber(y), tonumber(z))
		state.state = BOT_STATE.HUNTING -- prevent z-recovery
		state.verboseLog = true
		logger.info("[CavebotDebug:" .. player:getName() .. "] goto " .. x .. "," .. y .. "," .. z)
		return "Walking to " .. x .. "," .. y .. "," .. z

	elseif cmd == "stairs" then
		BotSystem.clearAllTasks(player, state)
		local goDown = (args:lower() == "down")
		local transitions = BotSystem.findZTransitions(pos, 15, goDown)
		local msg = (goDown and "DOWN" or "UP") .. " transitions: " .. #transitions
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. msg)
		for i, t in ipairs(transitions) do
			logger.info("[CavebotDebug:" .. player:getName() .. "]   #" .. i .. ": "
				.. t.type .. " at (" .. t.pos.x .. "," .. t.pos.y .. "," .. t.pos.z .. ") dist=" .. t.dist)
		end
		if #transitions > 0 then
			state.cavebotCommand = "stairs"
			state.state = BOT_STATE.HUNTING
			state.verboseLog = true
			BotSystem.startFloorChange(state, goDown, nil, nil, "cavebot")
		end
		return msg

	elseif cmd == "scan" then
		local radius = tonumber(args) or 10
		local upT = BotSystem.findZTransitions(pos, radius, false)
		local downT = BotSystem.findZTransitions(pos, radius, true)
		logger.info("[CavebotDebug:" .. player:getName() .. "] SCAN radius=" .. radius)
		logger.info("[CavebotDebug:" .. player:getName() .. "] UP transitions: " .. #upT)
		for _, t in ipairs(upT) do
			logger.info("[CavebotDebug:" .. player:getName() .. "]   " .. t.type
				.. " at (" .. t.pos.x .. "," .. t.pos.y .. "," .. t.pos.z .. ") dist=" .. t.dist)
		end
		logger.info("[CavebotDebug:" .. player:getName() .. "] DOWN transitions: " .. #downT)
		for _, t in ipairs(downT) do
			logger.info("[CavebotDebug:" .. player:getName() .. "]   " .. t.type
				.. " at (" .. t.pos.x .. "," .. t.pos.y .. "," .. t.pos.z .. ") dist=" .. t.dist)
		end
		return "UP=" .. #upT .. " DOWN=" .. #downT

	elseif cmd == "step" then
		local dirMap = {
			north = DIRECTION_NORTH, south = DIRECTION_SOUTH,
			east = DIRECTION_EAST, west = DIRECTION_WEST,
			ne = DIRECTION_NORTHEAST, nw = DIRECTION_NORTHWEST,
			se = DIRECTION_SOUTHEAST, sw = DIRECTION_SOUTHWEST,
		}
		local dir = dirMap[args:lower()]
		if not dir then return "Usage: step north|south|east|west|ne|nw|se|sw" end
		player:move(dir)
		local newPos = player:getPosition()
		logger.info("[CavebotDebug:" .. player:getName() .. "] step " .. args
			.. " -> (" .. newPos.x .. "," .. newPos.y .. "," .. newPos.z .. ")")
		return "Moved " .. args

	elseif cmd == "teleport" or cmd == "tp" then
		local x, y, z = args:match("(%d+),(%d+),(%d+)")
		if not x then
			x, y, z = args:match("(%d+)%s+(%d+)%s+(%d+)")
		end
		if not x then return "Usage: teleport x,y,z" end
		player:teleportTo(Position(tonumber(x), tonumber(y), tonumber(z)))
		local newPos = player:getPosition()
		logger.info("[CavebotDebug:" .. player:getName() .. "] teleported to ("
			.. newPos.x .. "," .. newPos.y .. "," .. newPos.z .. ")")
		return "Teleported"

	elseif cmd == "stop" then
		BotSystem.clearAllTasks(player, state)
		state.state = BOT_STATE.IDLE
		logger.info("[CavebotDebug:" .. player:getName() .. "] stopped")
		return "Stopped"

	elseif cmd == "resume" then
		BotSystem.clearAllTasks(player, state)
		state.state = BOT_STATE.IDLE
		state.verboseLog = false
		logger.info("[CavebotDebug:" .. player:getName() .. "] resumed normal AI")
		return "Resumed"

	elseif cmd == "advstone" then
		-- Adventurer's Stone trip — bot must be standing in a temple PZ.
		-- All trip state lives in C++; we just forward to executeCommand which
		-- runs startAdventurerStoneTrip(). Useful as a SQL test trigger:
		--   INSERT INTO bot_commands (bot_name, command) VALUES ('<Bot>', 'advstone');
		local result = Game.botCommand(player:getName(), "advstone")
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. tostring(result))
		return result

	elseif cmd == "log" then
		if args:lower() == "on" then
			state.verboseLog = true
			logger.info("[CavebotDebug:" .. player:getName() .. "] verbose logging ON")
			return "Logging ON"
		elseif args:lower() == "off" then
			state.verboseLog = false
			logger.info("[CavebotDebug:" .. player:getName() .. "] verbose logging OFF")
			return "Logging OFF"
		end
		return "Usage: log on|off"

	elseif cmd == "hunt" then
		local scriptId = tonumber(args)
		if not scriptId then
			-- Try finding by name
			for id, script in pairs(BotHuntData.scripts or {}) do
				if script.name and script.name:lower():find(args:lower(), 1, true) then
					scriptId = id
					break
				end
			end
		end
		if not scriptId then return "Hunt script not found: " .. args end
		BotSystem.clearAllTasks(player, state)
		state.verboseLog = true
		-- Force start this hunt
		local ok = BotHuntData.reserveHunt(scriptId, player:getGuid())
		if ok then
			local script = BotHuntData.scripts[scriptId]
			local huntTownId = script and script.town_id or nil
			local botTownId = player:getTown() and player:getTown():getId() or nil

			state.huntScriptId = scriptId
			state.huntStartTime = os.time()
			state.huntEndTime = os.time() + 3600
			state.huntKillCount = 0
			state.huntTargets = BotHuntData.getTargets(scriptId)
			state.huntTownId = huntTownId

			-- Check if hunt is in a different city — travel there first
			if huntTownId and botTownId and huntTownId ~= botTownId then
				local townName = script.town_name or ("town " .. huntTownId)
				logger.info("[CavebotDebug:" .. player:getName() .. "] hunt " .. scriptId
					.. " is in " .. townName .. " (bot in town " .. botTownId .. "), traveling first")
				BotSystem.castDebug(player, state, "Hunt: need to travel to " .. townName .. " first")
				-- Set up travel to the hunt's city
				state.pendingHuntAfterTravel = true
				-- Use the cavebot travel system
				local destTownName = townName:lower()
				BotSystem.startCavebotTravel(player, state, huntTownId, destTownName)
				return "Hunt " .. scriptId .. ": traveling to " .. townName .. " first"
			end

			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PREPARING)
			logger.info("[CavebotDebug:" .. player:getName() .. "] starting hunt script " .. scriptId)
			return "Hunt started: " .. scriptId .. " (preparing: depot + shops)"
		else
			return "Hunt script " .. scriptId .. " already reserved"
		end

	elseif cmd == "info" then
		local upT = BotSystem.findZTransitions(pos, 15, false)
		local downT = BotSystem.findZTransitions(pos, 15, true)
		local msg = "Bot at (" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")"
			.. " UP=" .. #upT .. " DOWN=" .. #downT
			.. " state=" .. state.state
			.. " fc_state=" .. state.floorChangeState
			.. " stuck=" .. tostring(BotSystem.isStuck(state))
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. msg)
		return msg

	elseif cmd == "poi" then
		local townId = player:getTown() and player:getTown():getId() or 0
		local poiName, poiDist = BotHuntData.detectNearestPOI(townId, player:getPosition())
		local msg = "Nearest POI: " .. (poiName or "none") .. " (dist=" .. (poiDist or 999) .. ")"
			.. " townId=" .. townId
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. msg)
		return msg

	elseif cmd == "routes" then
		local townId = player:getTown() and player:getTown():getId() or 0
		local pois = BotHuntData.listPOIs(townId)
		local routes = BotHuntData.listRoutes(townId)
		local msg = "Town " .. townId .. ": " .. #pois .. " POIs, " .. #routes .. " route pairs"
		logger.info("[CavebotDebug:" .. player:getName() .. "] " .. msg)
		logger.info("[CavebotDebug:" .. player:getName() .. "]   POIs: " .. table.concat(pois, ", "))
		-- Log first 20 routes
		for i = 1, math.min(20, #routes) do
			local r = routes[i]
			logger.info("[CavebotDebug:" .. player:getName() .. "]   " .. r.src .. " -> " .. r.dst)
		end
		return msg .. "\nPOIs: " .. table.concat(pois, ", ")

	elseif cmd == "navigate" or cmd == "nav" then
		local destination = args:lower():match("^(%S+)")
		if not destination then return "Usage: navigate <poi>" end
		BotSystem.clearAllTasks(player, state)

		local townId = player:getTown() and player:getTown():getId() or 0
		-- Detect current POI
		local currentPOI, poiDist = BotHuntData.detectNearestPOI(townId, player:getPosition())
		if not currentPOI or poiDist > 50 then
			return "Can't detect current location (nearest=" .. (currentPOI or "none") .. " dist=" .. (poiDist or 999) .. ")"
		end

		-- Find route
		local waypoints, reversed = BotHuntData.findRoute(townId, currentPOI, destination)
		if not waypoints then
			return "No route from " .. currentPOI .. " to " .. destination .. " in town " .. townId
		end

		-- Set up navigation
		state.cavebotCommand = "navigate"
		state.cavebotNavRoute = waypoints
		state.cavebotNavDest = destination
		state.cavebotNavSrc = currentPOI
		state.routeIdx = 1
		state.routeStuckCount = 0
		state.state = BOT_STATE.HUNTING -- prevent z-recovery
		state.verboseLog = true
		BotSystem.castDebug(player, state, "Navigate: " .. currentPOI .. " -> " .. destination
			.. " (" .. #waypoints .. " wps" .. (reversed and ", reversed" or "") .. ")")
		return "Navigating from " .. currentPOI .. " to " .. destination
			.. " (" .. #waypoints .. " waypoints" .. (reversed and ", reversed" or "") .. ")"

	elseif cmd == "sequence" or cmd == "seq" then
		-- Parse comma-separated POI list: "temple,depot,bank,boat"
		local pois = {}
		for poi in args:gmatch("([^,]+)") do
			local trimmed = poi:match("^%s*(.-)%s*$"):lower()
			if trimmed ~= "" then
				pois[#pois + 1] = trimmed
			end
		end
		if #pois < 2 then return "Need at least 2 POIs (comma-separated): sequence temple,depot,bank" end
		BotSystem.clearAllTasks(player, state)

		state.cavebotCommand = "sequence"
		state.cavebotSequence = pois
		state.cavebotSequenceIdx = 1
		state.state = BOT_STATE.HUNTING
		state.verboseLog = true
		BotSystem.castDebug(player, state, "Sequence: " .. table.concat(pois, " -> "))
		return "Starting sequence: " .. table.concat(pois, " -> ")

	elseif cmd == "travel" then
		local destCity = args:match("^%s*(.-)%s*$") -- trim whitespace, keep full name
		if not destCity or destCity == "" then return "Usage: travel <city>" end
		destCity = destCity:lower()
		BotSystem.clearAllTasks(player, state)

		local townId = player:getTown() and player:getTown():getId() or 0
		-- Find destination town ID (exact match first, then partial)
		local destTownId = nil
		local destTownName = nil
		-- Pass 1: exact match
		for _, town in ipairs(Game.getTowns() or {}) do
			if town:getName():lower() == destCity then
				destTownId = town:getId()
				destTownName = town:getName()
				break
			end
		end
		-- Pass 2: partial match (e.g. "liberty" matches "Liberty Bay")
		if not destTownId then
			for _, town in ipairs(Game.getTowns() or {}) do
				if town:getName():lower():find(destCity, 1, true) then
					destTownId = town:getId()
					destTownName = town:getName()
					break
				end
			end
		end
		if not destTownId then return "Unknown city: " .. destCity end

		-- Get boat NPC position in destination town (BOAT_POSITIONS preferred — actual NPC coords)
		local destBoatPos = BOAT_POSITIONS[destTownId]
		if not destBoatPos then
			-- Fallback to route graph POI
			local destGraph = BotHuntData.routeGraph[destTownId]
			if destGraph and destGraph.pois and destGraph.pois["boat"] then
				local p = destGraph.pois["boat"]
				destBoatPos = Position(p.x, p.y, p.z)
			end
		end
		if not destBoatPos then
			return "No boat position for destination city " .. (destTownName or destCity)
		end

		-- Get current city's boat NPC position (BOAT_POSITIONS preferred)
		local srcBoatPos = BOAT_POSITIONS[townId]

		-- Try to detect current POI and find a walking route to boat
		local currentPOI, poiDist = BotHuntData.detectNearestPOI(townId, player:getPosition())
		local waypoints = nil
		if currentPOI and poiDist <= 50 and currentPOI ~= "boat" then
			waypoints = BotHuntData.findRoute(townId, currentPOI, "boat")
		end

		state.cavebotCommand = "travel"
		state.travelDestTownId = destTownId
		state.travelDestTownName = destTownName or destCity
		state.travelDestBoatPos = destBoatPos
		state.travelPhase = "going_to_boat"
		state.state = BOT_STATE.HUNTING
		state.verboseLog = true

		if currentPOI == "boat" and poiDist and poiDist < 5 then
			-- Already near boat — walk to the exact boat spot
			if srcBoatPos then
				state.travelPhase = "walking_to_npc"
				state.travelBoatNpcPos = srcBoatPos
				BotSystem.castDebug(player, state, "Travel: near boat, walking to boat spot")
			else
				state.travelPhase = "at_boat"
				state.travelWaitUntil = os.time() + math.random(3, 10)
			end
		elseif waypoints then
			-- Have a walking route to boat
			state.cavebotNavRoute = waypoints
			state.cavebotNavDest = "boat"
			state.cavebotNavSrc = currentPOI
			state.routeIdx = 1
			state.routeStuckCount = 0
			BotSystem.castDebug(player, state, "Travel: " .. currentPOI .. " -> boat -> "
				.. (destTownName or destCity) .. " (" .. #waypoints .. " wps)")
		elseif srcBoatPos then
			-- No walking route — teleport directly to current city's boat NPC
			player:teleportTo(srcBoatPos)
			BotSystem.castDebug(player, state, "Travel: no route to boat, teleported to boat NPC at ("
				.. srcBoatPos.x .. "," .. srcBoatPos.y .. "," .. srcBoatPos.z .. ")")
			state.travelPhase = "at_boat"
			state.travelWaitUntil = os.time() + math.random(3, 10)
		else
			return "No boat position for current city (townId=" .. townId .. ")"
		end
		return "Traveling to " .. (destTownName or destCity)

	elseif cmd == "pk" then
		-- Force bot to PK a named target: pk TargetName
		local targetName = args
		if not targetName or targetName == "" then
			return "Usage: pk <target name>"
		end
		local target = Player(targetName)
		if not target then
			return "Target not found or offline: " .. targetName
		end
		if target:getId() == player:getId() then
			return "Cannot PK self"
		end
		BotSystem.clearAllTasks(player, state)
		state.state = BOT_STATE.PK_ATTACK
		state.pkTarget = target:getId()
		state.pkStartTime = os.time()
		state.lastAttackTime = 0
		state.lastCombatProgress = os.time()
		player:setTarget(target)
		BotSystem.castDebug(player, state, "PK FORCED: attacking " .. target:getName()
			.. " (id=" .. target:getId() .. ")")
		return "PK initiated on " .. target:getName()

	else
		return "Unknown command: " .. cmd
	end
end

-- ============================================================================
-- Cavebot "goto" Command Navigation (handles same-z pathfinding + z-transitions)
-- ============================================================================

function BotSystem.processCavebotGoto(player, state)
	if player:isWalking() then return end

	local pos = player:getPosition()
	local target = state.cavebotTarget
	if not target then
		state.cavebotCommand = nil
		return
	end

	local dx = math.abs(pos.x - target.x)
	local dy = math.abs(pos.y - target.y)
	local dist = math.max(dx, dy)

	-- Check arrival
	if dx <= 1 and dy <= 1 and pos.z == target.z then
		BotSystem.debugLog(state, "GOTO: arrived at (" .. target.x .. "," .. target.y .. "," .. target.z .. ")")
		state.cavebotCommand = nil
		state.cavebotTarget = nil
		return
	end

	-- Z-transition needed?
	if pos.z ~= target.z then
		local goDown = target.z > pos.z
		BotSystem.debugLog(state, "GOTO: z-mismatch pos.z=" .. pos.z .. " target.z=" .. target.z
			.. " -> starting floor change " .. (goDown and "DOWN" or "UP"))
		BotSystem.startFloorChange(state, goDown, target, nil, "cavebot")
		return
	end

	-- Same z: pathfind to target
	BotSystem.debugLog(state, "GOTO: same z, walking to (" .. target.x .. "," .. target.y .. ") dist=" .. dist)
	local ok = BotSystem.goTo(player, target)
	if not ok then
		BotSystem.tryOpenDoors(player, target)
		ok = BotSystem.goTo(player, target)
		if not ok then
			-- Check if stuck
			if BotSystem.isStuck(state) then
				BotSystem.debugLog(state, "GOTO: STUCK - aborting")
				state.cavebotCommand = nil
				state.cavebotTarget = nil
			else
				BotSystem.debugLog(state, "GOTO: path failed, retrying next tick")
			end
		end
	end
end

-- ============================================================================
-- startCavebotTravel: programmatic travel setup (used by hunt command)
-- ============================================================================

function BotSystem.startCavebotTravel(player, state, destTownId, destTownName)
	local townId = player:getTown() and player:getTown():getId() or 0

	-- Get boat position for destination
	local destBoatPos = BOAT_POSITIONS[destTownId]
	if not destBoatPos then
		local destGraph = BotHuntData.routeGraph[destTownId]
		if destGraph and destGraph.pois and destGraph.pois["boat"] then
			local p = destGraph.pois["boat"]
			destBoatPos = Position(p.x, p.y, p.z)
		end
	end

	-- Get current city's boat NPC position
	local srcBoatPos = BOAT_POSITIONS[townId]

	-- Try to detect current POI and find a walking route to boat
	local currentPOI, poiDist = BotHuntData.detectNearestPOI(townId, player:getPosition())
	local waypoints = nil
	if currentPOI and poiDist <= 50 and currentPOI ~= "boat" then
		waypoints = BotHuntData.findRoute(townId, currentPOI, "boat")
	end

	state.cavebotCommand = "travel"
	state.travelDestTownId = destTownId
	state.travelDestTownName = destTownName or ("town " .. destTownId)
	state.travelDestBoatPos = destBoatPos
	state.travelPhase = "going_to_boat"
	state.state = BOT_STATE.HUNTING
	state.verboseLog = true
	BotSystem.resetFloorChangeState(state)

	if not destBoatPos then
		-- No destination boat — teleport directly to temple
		local destTown = Town(destTownId)
		if destTown then
			player:teleportTo(destTown:getTemplePosition())
			BotSystem.castDebug(player, state, "Travel: no boat pos for dest, teleported to temple")
		end
		state.cavebotCommand = nil
		state.travelPhase = nil
		return false
	end

	if currentPOI == "boat" and poiDist and poiDist < 5 then
		if srcBoatPos then
			state.travelPhase = "walking_to_npc"
			state.travelBoatNpcPos = srcBoatPos
			BotSystem.castDebug(player, state, "Travel: near boat, walking to boat spot")
		else
			state.travelPhase = "at_boat"
			state.travelWaitUntil = os.time() + math.random(3, 10)
		end
	elseif waypoints then
		state.cavebotNavRoute = waypoints
		state.cavebotNavDest = "boat"
		state.cavebotNavSrc = currentPOI
		state.routeIdx = 1
		state.routeStuckCount = 0
		BotSystem.castDebug(player, state, "Travel: " .. currentPOI .. " -> boat -> "
			.. (destTownName or "?") .. " (" .. #waypoints .. " wps)")
	elseif srcBoatPos then
		-- No DB route — use runtime pathfinding to walk to boat
		state.travelPhase = "walking_to_poi"
		state.walkToPOITarget = srcBoatPos
		state.walkToPOIFailCount = 0
		BotSystem.castDebug(player, state, "Travel: no route to boat, pathfinding to ("
			.. srcBoatPos.x .. "," .. srcBoatPos.y .. "," .. srcBoatPos.z .. ")")
	else
		-- No source boat either — teleport to dest temple
		local destTown = Town(destTownId)
		if destTown then
			player:teleportTo(destTown:getTemplePosition())
			BotSystem.castDebug(player, state, "Travel: no boat info, teleported to dest temple")
		end
		state.cavebotCommand = nil
		state.travelPhase = nil
		return false
	end
	return true
end

-- ============================================================================
-- Cavebot "navigate" Command: route-graph based POI-to-POI navigation
-- ============================================================================

function BotSystem.processCavebotNavigate(player, state)
	-- Sub-state: walking to depot locker (after route arrival at depot)
	if state.navDepotTarget then
		local pos = player:getPosition()
		local dp = state.navDepotTarget

		local dist = math.max(math.abs(pos.x - dp.x), math.abs(pos.y - dp.y))
		if dist <= 1 and pos.z == dp.z then
			-- Adjacent to depot locker — start the delay
			BotSystem.castDebug(player, state, "Navigate: reached depot locker at ("
				.. dp.x .. "," .. dp.y .. "," .. dp.z .. "), starting delay")
			if math.random(1, 2) == 1 then
				BotSystem.sayRandom(player, state, "depot")
			end
			state.navDepotTarget = nil
			local delay = { min = 60, max = 180 }
			local waitSeconds = math.random(delay.min, delay.max)
			state.navDepotWaitUntil = os.time() + waitSeconds
			BotSystem.castDebug(player, state, "Navigate: waiting " .. waitSeconds .. "s at depot locker")
			return
		end

		-- Not adjacent yet — walk toward it
		if not player:isWalking() then
			local foundPath = false
			local adjacentOffsets = {{0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1}}
			for _, off in ipairs(adjacentOffsets) do
				local adjPos = Position(dp.x + off[1], dp.y + off[2], dp.z)
				local adjTile = Tile(adjPos)
				if adjTile and adjTile:isWalkable() then
					local dirs = player:getPathTo(adjPos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs > 0 then
						player:startAutoWalk(dirs)
						foundPath = true
						break
					end
				end
			end

			if not foundPath then
				BotSystem.castDebug(player, state, "Navigate: depot locker unreachable, finding another")
				local newLocker = BotSystem.findReachableDepotLocker(player, state)
				if newLocker and (newLocker.x ~= dp.x or newLocker.y ~= dp.y) then
					state.navDepotTarget = newLocker
				else
					-- No reachable locker — just wait here
					BotSystem.castDebug(player, state, "Navigate: no reachable locker, waiting here")
					state.navDepotTarget = nil
					local waitSeconds = math.random(60, 180)
					state.navDepotWaitUntil = os.time() + waitSeconds
				end
			end
		end
		return
	end

	-- Sub-state: waiting at depot locker
	if state.navDepotWaitUntil then
		if os.time() >= state.navDepotWaitUntil then
			BotSystem.castDebug(player, state, "Navigate: depot wait complete, done")
			logger.info("[CavebotDebug:" .. player:getName() .. "] Navigate depot visit complete")
			state.navDepotWaitUntil = nil
			state.cavebotCommand = nil
			state.cavebotNavRoute = nil
			state.cavebotNavDest = nil
			state.cavebotNavSrc = nil
			state.routeIdx = nil
			state.routeStuckCount = nil
		end
		return
	end

	local result = BotSystem.followCityRoute(player, state, state.cavebotNavRoute)
	if result == "arrived" then
		local dest = state.cavebotNavDest or "?"
		BotSystem.castDebug(player, state, "Navigate: ARRIVED at " .. dest)
		logger.info("[CavebotDebug:" .. player:getName() .. "] Navigate arrived at " .. dest)

		-- If destination is depot, find and walk to a depot locker
		if dest == "depot" then
			local lockerPos = BotSystem.findReachableDepotLocker(player, state)
			if lockerPos then
				local pos = player:getPosition()
				local dist = math.max(math.abs(pos.x - lockerPos.x), math.abs(pos.y - lockerPos.y))
				if dist <= 1 and pos.z == lockerPos.z then
					-- Already adjacent — start delay immediately
					BotSystem.castDebug(player, state, "Navigate: already adjacent to depot locker")
					if math.random(1, 2) == 1 then
						BotSystem.sayRandom(player, state, "depot")
					end
					local waitSeconds = math.random(60, 180)
					state.navDepotWaitUntil = os.time() + waitSeconds
					BotSystem.castDebug(player, state, "Navigate: waiting " .. waitSeconds .. "s at depot")
				else
					-- Walk to the locker
					BotSystem.castDebug(player, state, "Navigate: walking to depot locker at ("
						.. lockerPos.x .. "," .. lockerPos.y .. "," .. lockerPos.z
						.. ") dist=" .. dist)
					state.navDepotTarget = lockerPos
				end
			else
				-- No locker found — just wait at current position
				BotSystem.castDebug(player, state, "Navigate: no depot locker found, waiting here")
				local waitSeconds = math.random(60, 180)
				state.navDepotWaitUntil = os.time() + waitSeconds
			end
			-- Keep cavebotCommand active — depot sub-state will clear it when done
			state.cavebotNavRoute = nil
			state.routeIdx = nil
			state.routeStuckCount = nil
			return
		end

		-- Non-depot destination — check for pending hunt, then clear
		state.cavebotNavRoute = nil
		state.cavebotNavDest = nil
		state.cavebotNavSrc = nil
		state.routeIdx = nil
		state.routeStuckCount = nil
		if state.pendingHuntAfterTravel and state.huntScriptId then
			state.pendingHuntAfterTravel = nil
			state.cavebotCommand = nil
			BotSystem.castDebug(player, state, "Navigate complete, preparing for hunt " .. state.huntScriptId)
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PREPARING)
		else
			state.cavebotCommand = nil
		end
	elseif result == "failed" then
		BotSystem.castDebug(player, state, "Navigate: FAILED to reach " .. (state.cavebotNavDest or "?"))
		logger.warn("[CavebotDebug:" .. player:getName() .. "] Navigate failed to reach " .. (state.cavebotNavDest or "?"))
		state.cavebotNavRoute = nil
		state.cavebotNavDest = nil
		state.cavebotNavSrc = nil
		state.routeIdx = nil
		state.routeStuckCount = nil
		if state.pendingHuntAfterTravel and state.huntScriptId then
			state.pendingHuntAfterTravel = nil
			state.cavebotCommand = nil
			BotSystem.castDebug(player, state, "Navigate failed, preparing for hunt anyway " .. state.huntScriptId)
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PREPARING)
		else
			state.cavebotCommand = nil
		end
	end
	-- "walking" = still in progress
end

-- ============================================================================
-- Cavebot "sequence" Command: multi-leg navigation (poi1->poi2->poi3->...)
-- ============================================================================

-- POI delay ranges (in seconds): how long bots wait at each POI type
local POI_DELAYS = {
	depot    = { min = 60,  max = 180 }, -- 1-3 minutes (using depot box)
	bank     = { min = 10,  max = 90  }, -- talking to NPC
	potions  = { min = 10,  max = 90  },
	runes    = { min = 10,  max = 90  },
	ammo     = { min = 10,  max = 90  },
	food     = { min = 10,  max = 90  },
	boat     = { min = 5,   max = 10  }, -- quick travel interaction
	carpet   = { min = 5,   max = 10  },
}

-- Scan for all depot lockers in a wide area around the bot.
-- Returns a sorted list: { {pos, dist, occupied, id}, ... }
-- Sorted by: unoccupied first, then closest.
function BotSystem.findAllDepotLockers(player, state)
	local pos = player:getPosition()
	local lockers = {}
	-- Scan 15x15 area (radius 7) — depots can be spread across a large room
	for dx = -7, 7 do
		for dy = -7, 7 do
			local checkPos = Position(pos.x + dx, pos.y + dy, pos.z)
			local tile = Tile(checkPos)
			if tile and tile:hasFlag(TILESTATE_DEPOT) then
				local items = tile:getItems()
				if items then
					for _, item in ipairs(items) do
						local iid = item:getId()
						if iid >= 3497 and iid <= 3500 then
							local dist = math.abs(dx) + math.abs(dy)
							local occupied = false
							local topCreature = tile:getTopCreature()
							if topCreature and topCreature:getId() ~= player:getId() then
								occupied = true
							end
							lockers[#lockers + 1] = { pos = checkPos, dist = dist, occupied = occupied, id = iid }
						end
					end
				end
			end
		end
	end

	-- Sort: prefer unoccupied, then closest
	table.sort(lockers, function(a, b)
		if a.occupied ~= b.occupied then return not a.occupied end
		return a.dist < b.dist
	end)

	return lockers
end

-- Find the first reachable, unoccupied depot locker. Tries pathing to each one.
-- Returns lockerPos or nil.
function BotSystem.findReachableDepotLocker(player, state)
	local lockers = BotSystem.findAllDepotLockers(player, state)
	if #lockers == 0 then
		BotSystem.castDebug(player, state, "Depot: no lockers found in 15x15 area")
		return nil
	end

	BotSystem.castDebug(player, state, "Depot: found " .. #lockers .. " lockers, testing reachability...")

	local adjacentOffsets = {{0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1}}
	for i, locker in ipairs(lockers) do
		if not locker.occupied then
			-- Try to find a walkable adjacent tile we can path to
			for _, off in ipairs(adjacentOffsets) do
				local adjPos = Position(locker.pos.x + off[1], locker.pos.y + off[2], locker.pos.z)
				local adjTile = Tile(adjPos)
				if adjTile and adjTile:isWalkable() then
					local dirs = player:getPathTo(adjPos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs >= 0 then
						BotSystem.castDebug(player, state, "Depot: locker #" .. i .. " (id=" .. locker.id
							.. ") at (" .. locker.pos.x .. "," .. locker.pos.y .. "," .. locker.pos.z
							.. ") is reachable via (" .. adjPos.x .. "," .. adjPos.y .. ")")
						return locker.pos
					end
				end
			end
		end
	end

	-- All unoccupied lockers unreachable — try occupied ones as fallback
	for i, locker in ipairs(lockers) do
		if locker.occupied then
			for _, off in ipairs(adjacentOffsets) do
				local adjPos = Position(locker.pos.x + off[1], locker.pos.y + off[2], locker.pos.z)
				local adjTile = Tile(adjPos)
				if adjTile and adjTile:isWalkable() then
					local dirs = player:getPathTo(adjPos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs >= 0 then
						BotSystem.castDebug(player, state, "Depot: locker #" .. i .. " (OCCUPIED, id=" .. locker.id
							.. ") at (" .. locker.pos.x .. "," .. locker.pos.y .. ") is reachable (fallback)")
						return locker.pos
					end
				end
			end
		end
	end

	BotSystem.castDebug(player, state, "Depot: no reachable locker found")
	return nil
end

function BotSystem.processCavebotSequence(player, state)
	local seq = state.cavebotSequence
	local idx = state.cavebotSequenceIdx or 1

	-- Sub-state: walking to depot locker (must reach it before delay starts)
	if state.seqDepotTarget then
		local pos = player:getPosition()
		local dp = state.seqDepotTarget
		local dist = math.max(math.abs(pos.x - dp.x), math.abs(pos.y - dp.y))

		if dist <= 1 and pos.z == dp.z then
			-- Adjacent to depot locker — start the delay now
			BotSystem.castDebug(player, state, "Depot: reached locker at ("
				.. dp.x .. "," .. dp.y .. "," .. dp.z .. "), starting delay")
			if math.random(1, 2) == 1 then
				BotSystem.sayRandom(player, state, "depot")
			end
			state.seqDepotTarget = nil
			local delay = POI_DELAYS["depot"]
			local waitSeconds = math.random(delay.min, delay.max)
			state.seqWaitUntil = os.time() + waitSeconds
			state.seqWaitPOI = "depot"
			BotSystem.castDebug(player, state, "Sequence: waiting " .. waitSeconds .. "s at depot")
			return
		end

		-- Not adjacent yet — walk toward it
		if not player:isWalking() then
			-- Find a walkable adjacent tile and path to it
			local foundPath = false
			local adjacentOffsets = {{0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1}}
			for _, off in ipairs(adjacentOffsets) do
				local adjPos = Position(dp.x + off[1], dp.y + off[2], dp.z)
				local adjTile = Tile(adjPos)
				if adjTile and adjTile:isWalkable() then
					local dirs = player:getPathTo(adjPos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs > 0 then
						player:startAutoWalk(dirs)
						foundPath = true
						break
					end
				end
			end

			if not foundPath then
				-- This locker became unreachable — find a new one
				BotSystem.castDebug(player, state, "Depot: locker unreachable, finding another")
				local newLocker = BotSystem.findReachableDepotLocker(player, state)
				if newLocker and (newLocker.x ~= dp.x or newLocker.y ~= dp.y) then
					state.seqDepotTarget = newLocker
				else
					-- No reachable locker at all — just wait here
					BotSystem.castDebug(player, state, "Depot: no reachable locker, waiting here")
					state.seqDepotTarget = nil
					local delay = POI_DELAYS["depot"]
					local waitSeconds = math.random(delay.min, delay.max)
					state.seqWaitUntil = os.time() + waitSeconds
					state.seqWaitPOI = "depot"
				end
			end
		end
		return
	end

	-- Check if we're waiting at a POI (delay timer)
	if state.seqWaitUntil then
		if os.time() < state.seqWaitUntil then
			return -- still waiting at POI
		end
		-- Delay done
		local poiName = state.seqWaitPOI or "?"
		BotSystem.castDebug(player, state, "Sequence: done waiting at " .. poiName)
		state.seqWaitUntil = nil
		state.seqWaitPOI = nil
	end

	-- If no active leg, start the next one
	if not state.cavebotNavRoute then
		if idx >= #seq then
			-- All legs done
			BotSystem.castDebug(player, state, "Sequence: ALL LEGS COMPLETE")
			logger.info("[CavebotDebug:" .. player:getName() .. "] Sequence complete")
			state.cavebotCommand = nil
			state.cavebotSequence = nil
			state.cavebotSequenceIdx = nil
			state.seqWaitUntil = nil
			state.seqWaitPOI = nil
			state.seqDepotTarget = nil
			return
		end

		local src = seq[idx]
		local dst = seq[idx + 1]
		local townId = player:getTown() and player:getTown():getId() or 0

		-- Auto-detect source POI if first leg
		if idx == 1 then
			local detectedPOI, poiDist = BotHuntData.detectNearestPOI(townId, player:getPosition())
			if detectedPOI and poiDist < 30 then
				src = detectedPOI
				BotSystem.castDebug(player, state, "Sequence: detected current POI as '" .. src .. "' (dist=" .. poiDist .. ")")
			end
		end

		local waypoints, reversed = BotHuntData.findRoute(townId, src, dst)
		if not waypoints then
			BotSystem.castDebug(player, state, "Sequence: no route from " .. src .. " to " .. dst .. ", aborting")
			logger.warn("[CavebotDebug:" .. player:getName() .. "] Sequence: no route " .. src .. " -> " .. dst)
			state.cavebotCommand = nil
			state.cavebotSequence = nil
			return
		end

		BotSystem.castDebug(player, state, "Sequence leg " .. idx .. "/" .. (#seq - 1)
			.. ": " .. src .. " -> " .. dst .. " (" .. #waypoints .. " wps"
			.. (reversed and ", reversed" or "") .. ")")

		state.cavebotNavRoute = waypoints
		state.cavebotNavDest = dst
		state.cavebotNavSrc = src
		state.routeIdx = 1
		state.routeStuckCount = 0
	end

	-- Process the current leg
	local result = BotSystem.followCityRoute(player, state, state.cavebotNavRoute)
	if result == "arrived" then
		local dest = state.cavebotNavDest or "?"
		BotSystem.castDebug(player, state, "Sequence leg " .. idx .. ": ARRIVED at " .. dest)

		-- Clear current leg, advance to next
		state.cavebotNavRoute = nil
		state.cavebotNavDest = nil
		state.cavebotNavSrc = nil
		state.routeIdx = nil
		state.routeStuckCount = nil
		state.cavebotSequenceIdx = idx + 1

		-- POI-specific actions and delays
		local delay = POI_DELAYS[dest]
		if dest == "depot" then
			-- Find nearest available depot locker and walk to it
			local lockerPos = BotSystem.findReachableDepotLocker(player, state)
			if lockerPos then
				local pos = player:getPosition()
				local dist = math.max(math.abs(pos.x - lockerPos.x), math.abs(pos.y - lockerPos.y))
				if dist <= 1 and pos.z == lockerPos.z then
					-- Already adjacent — start delay immediately
					BotSystem.castDebug(player, state, "Depot: already adjacent to locker")
					if math.random(1, 2) == 1 then
						BotSystem.sayRandom(player, state, "depot")
					end
					local waitSeconds = math.random(delay.min, delay.max)
					state.seqWaitUntil = os.time() + waitSeconds
					state.seqWaitPOI = "depot"
					BotSystem.castDebug(player, state, "Sequence: waiting " .. waitSeconds .. "s at depot")
				else
					-- Need to walk to locker first
					BotSystem.castDebug(player, state, "Depot: walking to locker at ("
						.. lockerPos.x .. "," .. lockerPos.y .. "," .. lockerPos.z
						.. ") dist=" .. dist)
					state.seqDepotTarget = lockerPos
				end
			else
				-- No locker found, just wait at current position
				local waitSeconds = math.random(delay.min, delay.max)
				state.seqWaitUntil = os.time() + waitSeconds
				state.seqWaitPOI = "depot"
				BotSystem.castDebug(player, state, "Sequence: no locker found, waiting " .. waitSeconds .. "s at depot")
			end
		elseif dest == "boat" or dest == "carpet" then
			player:say("hi", TALKTYPE_SAY)
			BotSystem.castDebug(player, state, "Saying 'hi' at " .. dest)
			if delay and (idx < #seq) then
				local waitSeconds = math.random(delay.min, delay.max)
				state.seqWaitUntil = os.time() + waitSeconds
				state.seqWaitPOI = dest
				BotSystem.castDebug(player, state, "Sequence: waiting " .. waitSeconds .. "s at " .. dest)
			end
		elseif dest == "bank" or dest == "potions" or dest == "runes"
			or dest == "ammo" or dest == "food" then
			player:say("hi", TALKTYPE_SAY)
			BotSystem.castDebug(player, state, "Saying 'hi' at " .. dest)
			if delay and (idx < #seq) then
				local waitSeconds = math.random(delay.min, delay.max)
				state.seqWaitUntil = os.time() + waitSeconds
				state.seqWaitPOI = dest
				BotSystem.castDebug(player, state, "Sequence: waiting " .. waitSeconds .. "s at " .. dest)
			end
		else
			-- Non-standard POI, no delay
			if delay and (idx < #seq) then
				local waitSeconds = math.random(delay.min, delay.max)
				state.seqWaitUntil = os.time() + waitSeconds
				state.seqWaitPOI = dest
				BotSystem.castDebug(player, state, "Sequence: waiting " .. waitSeconds .. "s at " .. dest)
			end
		end
	elseif result == "failed" then
		BotSystem.castDebug(player, state, "Sequence leg " .. idx .. ": FAILED")
		state.cavebotCommand = nil
		state.cavebotSequence = nil
	end
end

-- ============================================================================
-- Cavebot "travel" Command: inter-city boat travel
-- ============================================================================

function BotSystem.processCavebotTravel(player, state)
	local phase = state.travelPhase or "going_to_boat"

	if phase == "going_to_boat" then
		-- Navigate to boat in current city
		if not state.cavebotNavRoute then
			BotSystem.castDebug(player, state, "Travel: no route to boat, aborting")
			state.cavebotCommand = nil
			return
		end
		local result = BotSystem.followCityRoute(player, state, state.cavebotNavRoute)
		if result == "arrived" then
			state.cavebotNavRoute = nil
			state.routeIdx = nil
			state.routeStuckCount = nil
			-- Walk to the actual boat NPC position
			local townId = player:getTown() and player:getTown():getId() or 0
			local boatNpcPos = BOAT_POSITIONS[townId]
			if boatNpcPos then
				state.travelPhase = "walking_to_npc"
				state.travelBoatNpcPos = boatNpcPos
				BotSystem.castDebug(player, state, "Travel: route done, walking to boat NPC at ("
					.. boatNpcPos.x .. "," .. boatNpcPos.y .. "," .. boatNpcPos.z .. ")")
			else
				-- No NPC position known — just say hi and proceed
				BotSystem.castDebug(player, state, "Travel: arrived at boat, saying hi...")
				player:say("hi", TALKTYPE_SAY)
				state.travelPhase = "at_boat"
				state.travelWaitUntil = os.time() + math.random(3, 10)
			end
		elseif result == "failed" then
			BotSystem.castDebug(player, state, "Travel: FAILED to reach boat")
			state.cavebotCommand = nil
		end

	elseif phase == "walking_to_npc" then
		-- Walk the last few tiles to the actual boat NPC
		local npcPos = state.travelBoatNpcPos
		if not npcPos then
			state.travelPhase = "at_boat"
			state.travelWaitUntil = os.time() + math.random(3, 10)
			return
		end
		local pos = player:getPosition()
		local dist = math.max(math.abs(pos.x - npcPos.x), math.abs(pos.y - npcPos.y))
		if dist <= 1 and pos.z == npcPos.z then
			-- Adjacent or on top of NPC — say hi and proceed
			BotSystem.castDebug(player, state, "Travel: at boat NPC, saying hi...")
			player:say("hi", TALKTYPE_SAY)
			state.travelPhase = "at_boat"
			state.travelWaitUntil = os.time() + math.random(3, 10)
			state.travelBoatNpcPos = nil
			return
		end
		-- Z mismatch — need floor change
		if pos.z ~= npcPos.z then
			if state.floorChangeState == FLOOR_CHANGE_STATE.NONE then
				local goDown = npcPos.z > pos.z
				BotSystem.startFloorChange(state, goDown, npcPos, nil, "route")
			end
			return
		end
		-- Walk to NPC
		if not player:isWalking() then
			local dirs = player:getPathTo(npcPos, 0, 1, true, true, BOT_CONFIG.PATH_MAX_DIST)
			if dirs and #dirs > 0 then
				player:startAutoWalk(dirs)
			else
				-- Can't path — just teleport as last resort
				player:teleportTo(npcPos)
				BotSystem.castDebug(player, state, "Travel: can't path to boat NPC, teleported")
			end
		end

	elseif phase == "at_boat" then
		-- Wait a bit then teleport to destination
		if os.time() < (state.travelWaitUntil or 0) then return end

		local destBoatPos = state.travelDestBoatPos
		if not destBoatPos then
			BotSystem.castDebug(player, state, "Travel: no destination boat position, aborting")
			state.cavebotCommand = nil
			return
		end

		-- Teleport to destination boat
		local destPos = Position(destBoatPos.x, destBoatPos.y, destBoatPos.z)
		player:teleportTo(destPos)
		BotSystem.castDebug(player, state, "Travel: TELEPORTED to " .. (state.travelDestTownName or "?")
			.. " boat at (" .. destPos.x .. "," .. destPos.y .. "," .. destPos.z .. ")")

		-- Change bot's town to destination
		local destTownId = state.travelDestTownId
		if destTownId then
			local town = Town(destTownId)
			if town then
				player:setTown(town)
				BotSystem.castDebug(player, state, "Travel: set town to " .. town:getName())
			end
		end

		-- Now navigate from boat to temple in destination city
		state.travelPhase = "arrived_dest"
		state.travelWaitUntil = os.time() + 2 -- short pause after teleport

	elseif phase == "walking_to_poi" then
		-- Runtime pathfinding to a POI (boat or temple) when no DB route exists
		local target = state.walkToPOITarget
		if not target then
			state.travelPhase = "at_boat"
			state.travelWaitUntil = os.time() + math.random(3, 10)
			return
		end

		local pos = player:getPosition()
		local dist = math.max(math.abs(pos.x - target.x), math.abs(pos.y - target.y))

		-- Arrival check (within 3 tiles, allow ±1 z for boat docks)
		if dist <= 3 and math.abs(pos.z - target.z) <= 1 then
			BotSystem.castDebug(player, state, "Travel: pathfinding arrived near target")
			state.walkToPOITarget = nil
			state.walkToPOIFailCount = nil
			-- If walking to boat, proceed to walking_to_npc
			if state.travelPhase == "walking_to_poi" then
				local townId = player:getTown() and player:getTown():getId() or 0
				local boatNpcPos = BOAT_POSITIONS[townId]
				if boatNpcPos and not state.walkToPOIIsTemple then
					state.travelPhase = "walking_to_npc"
					state.travelBoatNpcPos = boatNpcPos
				elseif state.walkToPOIIsTemple then
					-- Arrived at temple after travel
					state.walkToPOIIsTemple = nil
					BotSystem.castDebug(player, state, "Travel: arrived at temple via pathfinding")
					if state.pendingHuntAfterTravel and state.huntScriptId then
						state.pendingHuntAfterTravel = nil
						state.cavebotCommand = nil
						state.travelPhase = nil
						state.travelDestTownId = nil
						state.travelDestTownName = nil
						state.travelDestBoatPos = nil
						BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PREPARING)
					else
						state.cavebotCommand = nil
						state.travelPhase = nil
						state.travelDestTownId = nil
						state.travelDestTownName = nil
						state.travelDestBoatPos = nil
						state.state = BOT_STATE.IDLE
					end
				else
					state.travelPhase = "at_boat"
					state.travelWaitUntil = os.time() + math.random(3, 10)
				end
			end
			return
		end

		-- Z mismatch — need floor change
		if pos.z ~= target.z then
			if state.floorChangeState == FLOOR_CHANGE_STATE.NONE then
				local goDown = target.z > pos.z
				BotSystem.startFloorChange(state, goDown, target, nil, "route")
			end
			return
		end

		-- Walk toward target using goTo()
		if not player:isWalking() then
			local ok = BotSystem.goTo(player, target)
			if not ok then
				state.walkToPOIFailCount = (state.walkToPOIFailCount or 0) + 1
				if state.walkToPOIFailCount >= 30 then
					-- Pathfinding failed too many times — teleport as last resort
					player:teleportTo(target)
					BotSystem.castDebug(player, state, "Travel: pathfinding failed 30x, teleported to target")
					state.walkToPOITarget = nil
					state.walkToPOIFailCount = nil
				end
			else
				state.walkToPOIFailCount = 0
			end
		end

	elseif phase == "arrived_dest" then
		if os.time() < (state.travelWaitUntil or 0) then return end

		local destTownId = state.travelDestTownId
		-- Try to navigate boat -> temple in destination city
		local waypoints, reversed = BotHuntData.findRoute(destTownId, "boat", "temple")
		if waypoints then
			BotSystem.castDebug(player, state, "Travel: navigating boat -> temple in "
				.. (state.travelDestTownName or "?") .. " (" .. #waypoints .. " wps)")
			state.cavebotCommand = "navigate"
			state.cavebotNavRoute = waypoints
			state.cavebotNavDest = "temple"
			state.cavebotNavSrc = "boat"
			state.routeIdx = 1
			state.routeStuckCount = 0
		else
			-- No DB route — use runtime pathfinding to walk to temple
			local destTown = Town(destTownId)
			if destTown then
				local templePos = destTown:getTemplePosition()
				local walkZ = CITY_WALK_Z[destTownId] or templePos.z
				local walkTarget = Position(templePos.x, templePos.y, walkZ)
				state.travelPhase = "walking_to_poi"
				state.walkToPOITarget = walkTarget
				state.walkToPOIFailCount = 0
				state.walkToPOIIsTemple = true
				BotSystem.castDebug(player, state, "Travel: no boat->temple route, pathfinding to ("
					.. walkTarget.x .. "," .. walkTarget.y .. "," .. walkTarget.z .. ")")
			else
				BotSystem.castDebug(player, state, "Travel: no route and no town data, done")
				if state.pendingHuntAfterTravel and state.huntScriptId then
					state.pendingHuntAfterTravel = nil
					state.cavebotCommand = nil
					BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PREPARING)
				else
					state.cavebotCommand = nil
				end
			end
		end

		-- Clear travel sub-state (keep travelPhase if walking_to_poi was set)
		if state.travelPhase ~= "walking_to_poi" then
			state.travelPhase = nil
			state.travelDestTownId = nil
			state.travelDestTownName = nil
			state.travelDestBoatPos = nil
		end
		state.travelWaitUntil = nil
	end
end

-- ============================================================================
-- Bot Lifecycle
-- ============================================================================

function BotSystem.initBot(player)
	local guid = player:getGuid()
	BotPlayers[guid] = player
	BotActive[guid] = false
	BotState[guid] = {
		state = BOT_STATE.INACTIVE,
		walkTarget = nil,
		currentPOI = nil,
		dwellUntil = 0,
		lastHealTime = 0,
		lastAttackTime = 0,
		lastChatTime = 0,
		lastKnownHp = player:getMaxHealth(),
		tickCounter = 0,
		pathFailCount = 0,
		consecutivePOIFails = 0,
		visitedPOIs = {},
		-- Combat state
		combatDecision = nil,     -- "fight", "flee", or "ignore"
		combatStartTime = nil,
		lastCombatProgress = nil, -- resets on attacks, z-transitions, mob clears, pathfinding
		attackerId = nil,         -- creature ID of who attacked us
		targetLastSameZPos = nil, -- target's last position on same z (for stair pursuit)
		ignoredAttackerId = nil,  -- attacker we decided to ignore (too weak)
		ignoredHitBack = false,   -- did we already retaliate once while ignoring?
		preCombatPos = nil,       -- position before combat started (for return navigation)
		zBreadcrumbs = {},        -- z-transitions taken during combat: {{fromPos, toZ, type}}
		returningHome = false,    -- true when navigating back after combat
		returnBreadcrumbs = {},   -- reversed z-breadcrumbs for return trip
		-- Vigilante memory: PKers we already decided NOT to attack
		seenPKers = {},           -- {[creatureId] = os.time()} — cleared 60s after PKer leaves screen
		-- PK state
		pkTarget = nil,
		pkStartTime = nil,
		-- Travel state
		travelDestTownId = nil,
		travelUntil = nil,
		-- Hunting state
		huntScriptId = nil,        -- active hunt script DB id
		huntPhase = nil,           -- HUNT_PHASE enum
		huntWaypoints = nil,       -- current phase waypoint list
		huntWaypointIdx = 0,       -- current waypoint index
		huntTargets = nil,         -- monster targets for this hunt
		huntStartTime = nil,       -- when the hunt started (os.time)
		huntEndTime = nil,         -- when the hunt should end (os.time)
		huntCooldownUntil = 0,     -- next hunt allowed after this time
		huntKillCount = 0,         -- monsters killed this hunt
		huntMonsterTarget = nil,   -- creature ID of monster being attacked
		huntWaypointSkipCount = 0, -- consecutive skips without movement
		huntLastPos = nil,         -- position at last successful waypoint
		huntTownId = nil,          -- town the hunt is based in
		-- Resupply sub-state
		resupplyStep = nil,        -- "bank", "depot", "shop1", "shop2", "done"
		resupplyUntil = 0,         -- when current resupply step ends
		resupplyWalkTarget = nil,  -- where we're walking to for resupply
		-- Cavebot debug/command state
		verboseLog = false,        -- per-bot verbose z-transition logging
		botName = player:getName(),
		cavebotCommand = nil,      -- active cavebot command ("goto", "stairs", "hunt", etc.)
		cavebotTarget = nil,       -- target position for goto/waypoint commands
		cavebotStairsTarget = nil, -- selected transition for stairs command
		cavebotStairsGoDown = nil, -- direction for stairs command
		-- Floor change state machine
		floorChangeState = FLOOR_CHANGE_STATE.NONE,
		floorChangeTarget = nil,   -- {pos, type, goDown}
		floorChangePreZ = nil,     -- z before attempting step
		floorChangeAttempts = 0,   -- retry counter (max 3)
		floorChangeStartTime = nil,  -- for timeout detection (os.time())
		floorChangeTransitions = nil, -- list of found transitions (try next on failure)
		floorChangeTransIdx = 0,   -- current index in transitions list
		-- Walk history for stuck detection (BBot-inspired, circular buffer of 8)
		walkHistory = {},
		walkHistoryIdx = 0,
	}
end

function BotSystem.activateBot(player)
	local guid = player:getGuid()
	if not BotState[guid] then
		BotSystem.initBot(player)
	end

	local state = BotState[guid]
	BotActive[guid] = true

	-- Teleport to city temple at correct walk z-level (safe, walkable, PZ)
	local town = player:getTown()
	if town then
		local townId = town:getId()
		local templePos = town:getTemplePosition()
		local walkZ = CITY_WALK_Z[townId] or templePos.z
		player:teleportTo(Position(templePos.x, templePos.y, walkZ))
	end

	state.state = BOT_STATE.IDLE
	state.walkTarget = nil
	state.currentPOI = nil
	state.dwellUntil = 0
	state.lastKnownHp = player:getMaxHealth()
	state.tickCounter = 0
	state.pathFailCount = 0
	state.consecutivePOIFails = 0
	state.visitedPOIs = {}
	state.combatDecision = nil
	state.combatStartTime = nil
	state.attackerId = nil
	state.targetLastSameZPos = nil
	state.ignoredAttackerId = nil
	state.ignoredHitBack = false
	state.seenPKers = {}
	state.pkTarget = nil
	state.pkStartTime = nil
	state.travelDestTownId = nil
	state.travelUntil = nil
	-- Hunt state reset
	if state.huntScriptId then
		BotHuntData.releaseHunt(state.huntScriptId, player:getGuid())
	end
	state.huntScriptId = nil
	state.huntPhase = nil
	state.huntWaypoints = nil
	state.huntWaypointIdx = 0
	state.huntTargets = nil
	state.huntStartTime = nil
	state.huntEndTime = nil
	state.huntCooldownUntil = 0
	state.huntKillCount = 0
	state.huntMonsterTarget = nil
	state.huntWaypointSkipCount = 0
	state.huntLastPos = nil
	state.huntTownId = nil
	state.resupplyStep = nil
	state.resupplyUntil = 0
	state.resupplyWalkTarget = nil
	-- Cavebot debug state reset
	state.cavebotCommand = nil
	state.cavebotTarget = nil
	state.cavebotStairsTarget = nil
	state.cavebotStairsGoDown = nil
	BotSystem.resetFloorChangeState(state)
	state.walkHistory = {}
	state.walkHistoryIdx = 0
	-- Keep verboseLog and botName across resets

	-- Set all skills and magic level to 100 (must be done in Lua, DB values get overwritten on save)
	player:setMagicLevel(100)
	player:setSkillLevel(SKILL_FIST, 100)
	player:setSkillLevel(SKILL_CLUB, 100)
	player:setSkillLevel(SKILL_SWORD, 100)
	player:setSkillLevel(SKILL_AXE, 100)
	player:setSkillLevel(SKILL_DISTANCE, 100)
	player:setSkillLevel(SKILL_SHIELD, 100)
	player:setSkillLevel(SKILL_FISHING, 100)

	player:addHealth(player:getMaxHealth())
	player:addMana(player:getMaxMana())

	-- Set fight mode to full attack (1) — server auto-attacks with weapon
	player:setFightMode(1)

	-- Learn all spells for this vocation (attack + healing)
	BotSystem.learnAllSpells(player)

	-- Equip best gear for level and vocation
	BotSystem.equipBot(player)

	-- Enable cast broadcasting so viewers can watch this bot
	player:setCastBroadcasting(true)
	db.query("INSERT INTO `cast_broadcasters` (`player_id`, `player_name`) VALUES ("
		.. guid .. ", " .. db.escapeString(player:getName())
		.. ") ON DUPLICATE KEY UPDATE `player_name` = " .. db.escapeString(player:getName()))

	local pos = player:getPosition()
	local townName = town and town:getName() or "unknown"
	logger.info("[Bot] Activated " .. player:getName() ..
		" in " .. townName ..
		" at (" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")" ..
		" speed=" .. player:getSpeed() ..
		" level=" .. player:getLevel())

	player:registerEvent("BotDeath")
	-- Stagger think start so 200 bots don't all fire on the same tick
	addEvent(BotSystem.thinkEvent, BOT_CONFIG.THINK_INTERVAL + math.random(0, 500), guid)
end

function BotSystem.deactivateBot(player)
	local guid = player:getGuid()
	BotActive[guid] = false
	if BotState[guid] then
		-- Release any active hunt reservation
		if BotState[guid].huntScriptId then
			BotHuntData.releaseHunt(BotState[guid].huntScriptId, guid)
		end
		BotState[guid].state = BOT_STATE.INACTIVE
	end
	player:setCastBroadcasting(false)
	db.query("DELETE FROM `cast_broadcasters` WHERE `player_id` = " .. guid)
	if not player:teleportTo(BOT_CONFIG.DEAD_POSITION) then
		-- Fallback position if storage tile fails
		player:teleportTo(Position(31970, 32283, 7))
	end
end

-- ============================================================================
-- Think Loop
-- ============================================================================

function BotSystem.thinkEvent(guid)
	-- JITTER DIAGNOSTIC: per-bot thinkEvent timing. Threshold 10ms.
	local jitter_thinkStart = Game.monotonicMs and Game.monotonicMs() or 0
	local jitter_botName = nil

	local player = BotPlayers[guid]
	if not player or player:isRemoved() then
		BotActive[guid] = false
		return
	end
	jitter_botName = player:getName()
	-- Phase 5c: only clear cast-broadcasting if bot is genuinely INACTIVE (state==INACTIVE).
	-- The original `if not BotActive[guid] then ... clear broadcasting` was firing during
	-- the brief window between C++ wakeBot and the proximity loop's BotActive sync,
	-- causing woken bots to drop out of the cast list and producing the "list count
	-- varies wildly" symptom user reported. The state==INACTIVE check is the correct
	-- "is this bot really offline" signal — wakeBot puts the bot in IDLE state.
	if not BotActive[guid] then
		local s = BotState[guid]
		if s and s.state == BOT_STATE.INACTIVE then
			if player:isCastBroadcasting() then
				player:setCastBroadcasting(false)
				db.query("DELETE FROM `cast_broadcasters` WHERE `player_id` = " .. guid)
			end
		end
		return
	end

	local state = BotState[guid]
	if not state then return end

	-- Lua think loop is now a lightweight watchdog only.
	-- All bot AI (healing, combat, hunting, traveling, self-defense, PK, etc.)
	-- is driven by the C++ BotEngine::tick() at 100-300ms.
	-- Lua only handles: floor change state machine + cavebot admin commands + z-recovery.

	local ok, err = pcall(function()
		-- Floor change state machine: if active, handle it before anything else
		if state.floorChangeState ~= FLOOR_CHANGE_STATE.NONE then
			-- During hunt patrol: check for nearby targets BEFORE running floor change.
			-- If a monster is right here on our z-level, cancel the z-transition and fight it.
			if state.floorChangeSource == "hunt"
				and state.huntPhase == HUNT_PHASE.PATROLLING
				and not state.huntMonsterTarget then
				if BotSystem.scanAndAttackHuntTarget(player, state) then
					-- Found a target! Cancel the floor change and fight
					BotSystem.resetFloorChangeState(state)
					state.floorChangeResult = nil
					state.floorChangeSource = nil
					return
				end
			end

			BotSystem.handleFloorChange(player, state)
			-- If completed or failed, process result for the source that triggered it
			if state.floorChangeState == FLOOR_CHANGE_STATE.NONE then
				local result = state.floorChangeResult
				local source = state.floorChangeSource

				if source == "route" then
					-- Route floor-change: leave result for followCityRoute() to consume
					-- Don't clear floorChangeResult/floorChangeSource here
				else
					state.floorChangeResult = nil
					state.floorChangeSource = nil

					if source == "cavebot" or state.cavebotCommand == "stairs" then
						-- Cavebot command finished
						if state.cavebotCommand == "stairs" then
							state.cavebotCommand = nil
						end
					elseif source == "hunt" and result == "failed" then
						-- Hunt z-transition failed — skip the current waypoint
						logger.warn("[Bot:" .. player:getName() .. "] HUNTING floor change FAILED, skipping waypoint "
							.. (state.huntWaypointIdx or "?"))
						BotSystem.skipHuntWaypoint(player, state, "z-transition failed")
					end
					-- For "completed" during hunt: normal AI will run and navigateToHuntWaypoint
					-- will see the z matches and proceed with same-z pathfinding
				end
			end
			if state.floorChangeState ~= FLOOR_CHANGE_STATE.NONE then
				return -- don't run normal AI while floor change in progress
			end
		end

		-- Cavebot command processing: goto navigation
		if state.cavebotCommand == "goto" and state.cavebotTarget then
			BotSystem.processCavebotGoto(player, state)
			return -- skip normal AI
		end

		-- Cavebot command processing: navigate (route graph or depot sub-state)
		if state.cavebotCommand == "navigate" and (state.cavebotNavRoute or state.navDepotTarget or state.navDepotWaitUntil) then
			BotSystem.processCavebotNavigate(player, state)
			return -- skip normal AI
		end

		-- Cavebot command processing: sequence (multi-leg navigate)
		if state.cavebotCommand == "sequence" and state.cavebotSequence then
			BotSystem.processCavebotSequence(player, state)
			return -- skip normal AI
		end

		-- Cavebot command processing: travel (inter-city via boat)
		if state.cavebotCommand == "travel" then
			BotSystem.processCavebotTravel(player, state)
			return -- skip normal AI
		end

		-- Z-level recovery (slow: once per 60s, skip during HUNTING — hunts go underground)
		local now = os.time()
		if state.state ~= BOT_STATE.HUNTING then
			if now - (state.lastZCheck or 0) >= 60 then
				state.lastZCheck = now
				if BotSystem.checkZLevel(player, state) then
					return
				end
			end
		end
	end)

	if not ok then
		-- Log the error but keep the think loop alive
		state.thinkErrors = (state.thinkErrors or 0) + 1
		-- Log every error for the first 5, then every 60th to avoid spam
		if state.thinkErrors <= 5 or state.thinkErrors % 60 == 0 then
			logger.error("[Bot] " .. player:getName() .. " thinkEvent error #"
				.. state.thinkErrors .. ": " .. tostring(err))
		end
	end

	-- ALWAYS reschedule — errors must never kill the think loop
	-- Lua is now a watchdog: 2s normally, 500ms during active floor change
	if BotActive[guid] then
		local interval = 2000
		if state and state.floorChangeState ~= FLOOR_CHANGE_STATE.NONE then
			interval = 500
		end
		addEvent(BotSystem.thinkEvent, interval, guid)
	end

	-- JITTER DIAGNOSTIC: log if this bot's thinkEvent body exceeded 10ms
	if Game.monotonicMs then
		local jitter_thinkMs = Game.monotonicMs() - jitter_thinkStart
		if jitter_thinkMs > 10 then
			logger.warn(string.format("[THINK_SLOW] guid=%d name=%s duration=%dms",
				guid, jitter_botName or "?", jitter_thinkMs))
		end
	end
end

-- ============================================================================
-- Return Home: retrace z-transitions after combat ends
-- ============================================================================

function BotSystem.doReturnHome(player, state)
	if player:isWalking() then return end

	local pos = player:getPosition()

	-- Process breadcrumbs: each entry has a z-transition to reverse
	if state.returnBreadcrumbs and #state.returnBreadcrumbs > 0 then
		local nextBC = state.returnBreadcrumbs[1]

		-- Are we on the correct z-level to use this transition?
		-- The transitionPos is where the original transition was — we need to find
		-- the reverse transition on our CURRENT z-level
		if pos.z ~= nextBC.targetZ then
			-- Need to transition to nextBC.targetZ
			local goDown = nextBC.targetZ > pos.z
			local transitions = BotSystem.findZTransitions(pos, 10, goDown)

			-- Also search around the original transition position (projected to our z)
			local origProjected = Position(nextBC.transitionPos.x, nextBC.transitionPos.y, pos.z)
			local moreTransitions = BotSystem.findZTransitions(origProjected, 10, goDown)
			-- Merge and dedupe
			local seen = {}
			local allT = {}
			for _, t in ipairs(transitions) do
				local key = t.pos.x .. "," .. t.pos.y .. "," .. t.type
				if not seen[key] then seen[key] = true; table.insert(allT, t) end
			end
			for _, t in ipairs(moreTransitions) do
				local key = t.pos.x .. "," .. t.pos.y .. "," .. t.type
				if not seen[key] then seen[key] = true; table.insert(allT, t) end
			end

			-- Sort by distance from bot
			table.sort(allT, function(a, b)
				local da = math.abs(pos.x - a.pos.x) + math.abs(pos.y - a.pos.y)
				local db = math.abs(pos.x - b.pos.x) + math.abs(pos.y - b.pos.y)
				return da < db
			end)

			if #allT > 0 then
				local walked = false
				for _, entry in ipairs(allT) do
					local fc = entry.pos
					local dist = math.max(math.abs(pos.x - fc.x), math.abs(pos.y - fc.y))

					if entry.type == "ladder" or entry.type == "sewer" then
						if dist == 0 then
							local destPos = Position(fc.x, fc.y, fc.z)
							if entry.type == "ladder" then
								destPos.z = destPos.z - 1 -- ladder goes UP (z decreases)
								destPos.y = destPos.y - 1 -- offset north to land next to hole
							else
								destPos.z = destPos.z + 1
							end
							player:teleportTo(destPos, true)
							logger.info("[Bot] " .. player:getName() .. " return: used " .. entry.type
								.. " at (" .. fc.x .. "," .. fc.y .. "," .. fc.z
								.. ") -> (" .. destPos.x .. "," .. destPos.y .. "," .. destPos.z .. ")")
							table.remove(state.returnBreadcrumbs, 1)
							walked = true
							break
						end
						local dirs = player:getPathTo(fc, 0, 0, true, false, BOT_CONFIG.PATH_MAX_DIST)
						if dirs and type(dirs) == "table" and #dirs > 0 then
							player:startAutoWalk(dirs)
							walked = true
							break
						end
					else
						-- Stairs
						if dist == 0 then
							walked = true
							break
						end
						if dist == 1 then
							local stepDir = Position.getDirectionTo(pos, fc)
							if stepDir then
								player:startAutoWalk({stepDir})
								table.remove(state.returnBreadcrumbs, 1)
								walked = true
								break
							end
						end
						local dirs = player:getPathTo(fc, 1, 1, true, false, BOT_CONFIG.PATH_MAX_DIST)
						if dirs and type(dirs) == "table" and #dirs > 0 then
							player:startAutoWalk(dirs)
							walked = true
							break
						end
					end
				end

				if not walked then
					-- Can't reach any transition — try clearing blocking creatures
					BotSystem.tryAttackBlockingCreature(player, state, allT[1].pos)
				end
			else
				-- No transitions found — give up returning, just z-recover normally
				logger.info("[Bot] " .. player:getName() .. " return: no transitions found, giving up")
				state.returningHome = false
				state.returnBreadcrumbs = {}
				state.returnDestination = nil
			end
		else
			-- We're on the right z for this breadcrumb — pop it and continue
			table.remove(state.returnBreadcrumbs, 1)
		end
		return
	end

	-- All z-transitions done — walk back to the pre-combat XY position
	if state.returnDestination then
		local dest = state.returnDestination
		if pos.z == dest.z then
			local dist = math.max(math.abs(pos.x - dest.x), math.abs(pos.y - dest.y))
			if dist <= 3 then
				-- Close enough — done returning
				logger.info("[Bot] " .. player:getName() .. " returned home successfully")
				state.returningHome = false
				state.returnBreadcrumbs = {}
				state.returnDestination = nil
				return
			end
			-- Walk toward pre-combat position
			local destPos = Position(dest.x, dest.y, dest.z)
			local dirs = player:getPathTo(destPos, 0, 3, true, true, BOT_CONFIG.PATH_MAX_DIST)
			if dirs and type(dirs) == "table" and #dirs > 0 then
				player:startAutoWalk(dirs)
			else
				-- Can't path — close enough or blocked, give up
				logger.info("[Bot] " .. player:getName() .. " return: can't path to origin, stopping")
				state.returningHome = false
				state.returnBreadcrumbs = {}
				state.returnDestination = nil
			end
		else
			-- Still on wrong z but no breadcrumbs left — give up
			logger.info("[Bot] " .. player:getName() .. " return: wrong z but no breadcrumbs, giving up")
			state.returningHome = false
			state.returnBreadcrumbs = {}
			state.returnDestination = nil
		end
		return
	end

	-- Nothing to return to — clear flag
	state.returningHome = false
end

-- ============================================================================
-- IDLE State: walk between POIs, loiter, travel
-- ============================================================================

function BotSystem.doIdle(player, state)
	-- Random chat
	if math.random(1, BOT_CONFIG.CHAT_CHANCE) == 1 then
		BotSystem.sayRandom(player, state, "idle")
	end

	-- If auto-walk in progress, let it continue
	if player:isWalking() then
		return
	end

	-- Hunt roll (only when idle and cooldown expired)
	if not state.walkTarget and os.time() >= (state.huntCooldownUntil or 0) then
		if math.random(1, BOT_CONFIG.HUNT_CHANCE_PER_TICK) == 1 then
			if BotSystem.tryStartHunt(player, state) then
				return
			end
		end
	end

	-- Travel roll (only when idle and not walking to a target)
	if not state.walkTarget then
		if math.random(1, BOT_CONFIG.TRAVEL_CHANCE_PER_TICK) == 1 then
			BotSystem.startTravel(player, state)
			return
		end
	end

	-- Check arrival at current POI
	if state.currentPOI and state.walkTarget then
		local pos = player:getPosition()
		local dx = math.abs(pos.x - state.walkTarget.x)
		local dy = math.abs(pos.y - state.walkTarget.y)
		local dz = math.abs(pos.z - state.walkTarget.z)
		-- Arrival distance: 3 tiles on same z, or 1 tile z-diff (for boat at z=6 when city is z=7)
		if dx <= BOT_CONFIG.POI_ARRIVAL_DIST and dy <= BOT_CONFIG.POI_ARRIVAL_DIST and dz <= 1 then
			state.consecutivePOIFails = 0
			local dwellTime = math.random(BOT_CONFIG.POI_DWELL_MIN, BOT_CONFIG.POI_DWELL_MAX)
			state.state = BOT_STATE.DWELLING
			state.dwellUntil = os.time() + dwellTime

			BotSystem.castDebug(player, state, "IDLE: arrived at " .. state.currentPOI.name
				.. ", dwelling for " .. math.floor(dwellTime / 60) .. "m" .. (dwellTime % 60) .. "s")
			logger.info("[Bot] " .. player:getName() ..
				" arrived at " .. state.currentPOI.name ..
				", dwelling for " .. math.floor(dwellTime / 60) .. "m" .. (dwellTime % 60) .. "s")

			-- Depot arrival: 50% chance to walk to a depot locker
			local poiType = state.currentPOI.type
			if poiType == "depot" and math.random(1, 2) == 1 then
				local lockerPos = BotSystem.findReachableDepotLocker(player, state)
				if lockerPos then
					state.idleDepotTarget = lockerPos
					BotSystem.castDebug(player, state, "IDLE: walking to depot locker at ("
						.. lockerPos.x .. "," .. lockerPos.y .. "," .. lockerPos.z .. ")")
				end
			end

			if BOT_CHAT[poiType] and math.random(1, 3) == 1 then
				BotSystem.sayRandom(player, state, poiType)
			end
			return
		end
	end

	-- Pick a new POI if needed
	if not state.walkTarget then
		local poi = BotSystem.pickNextPOI(player, state)
		if poi then
			state.currentPOI = poi
			state.walkTarget = poi.pos
			state.pathFailCount = 0

			local pos = player:getPosition()
			BotSystem.castDebug(player, state, "IDLE: heading to " .. poi.name
				.. " (" .. poi.pos.x .. "," .. poi.pos.y .. "," .. poi.pos.z .. ")")
			logger.info("[Bot] " .. player:getName() ..
				" heading to " .. poi.name ..
				" from (" .. pos.x .. "," .. pos.y .. ")")
		end
	end

	-- Navigate to current target
	if state.walkTarget then
		local ok = BotSystem.goTo(player, state.walkTarget, state)
		if not ok then
			state.pathFailCount = (state.pathFailCount or 0) + 1
			if state.pathFailCount >= 3 then
				state.consecutivePOIFails = (state.consecutivePOIFails or 0) + 1
				state.walkTarget = nil
				state.currentPOI = nil
				state.pathFailCount = 0

				if state.consecutivePOIFails >= BOT_CONFIG.STUCK_THRESHOLD then
					logger.info("[Bot] " .. player:getName() ..
						" stuck! Emergency teleport to temple")
					local town = player:getTown()
					if town then
						player:teleportTo(town:getTemplePosition())
					end
					state.consecutivePOIFails = 0
					state.visitedPOIs = {}
				end
			end
		else
			state.pathFailCount = 0
		end
	end
end

-- ============================================================================
-- DWELLING State: standing at a POI
-- ============================================================================

function BotSystem.doDwelling(player, state)
	-- Depot locker walk sub-state: walk to locker while dwelling
	if state.idleDepotTarget then
		if not player:isWalking() then
			local pos = player:getPosition()
			local dp = state.idleDepotTarget
			local dist = math.max(math.abs(pos.x - dp.x), math.abs(pos.y - dp.y))
			if dist <= 1 and pos.z == dp.z then
				-- Arrived at depot locker
				state.idleDepotTarget = nil
				if math.random(1, 2) == 1 then
					BotSystem.sayRandom(player, state, "depot")
				end
			else
				-- Walk toward locker
				local adjacentOffsets = {{0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1}}
				for _, off in ipairs(adjacentOffsets) do
					local adjPos = Position(dp.x + off[1], dp.y + off[2], dp.z)
					local adjTile = Tile(adjPos)
					if adjTile and adjTile:isWalkable() then
						local dirs = player:getPathTo(adjPos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
						if dirs and type(dirs) == "table" and #dirs > 0 then
							player:startAutoWalk(dirs)
							break
						end
					end
				end
			end
		end
		-- Don't interrupt depot walking with hunt/travel rolls — let it finish first
		if state.idleDepotTarget then return end
	end

	if math.random(1, BOT_CONFIG.CHAT_CHANCE) == 1 then
		local poiType = state.currentPOI and state.currentPOI.type or "idle"
		BotSystem.sayRandom(player, state, poiType)
	end

	-- Hunt roll while dwelling (bots may decide to go hunting)
	if os.time() >= (state.huntCooldownUntil or 0) then
		if math.random(1, BOT_CONFIG.HUNT_CHANCE_PER_TICK) == 1 then
			if BotSystem.tryStartHunt(player, state) then
				return
			end
		end
	end

	-- Travel roll while dwelling (bots might decide to leave while idling)
	if math.random(1, BOT_CONFIG.TRAVEL_CHANCE_PER_TICK) == 1 then
		BotSystem.startTravel(player, state)
		return
	end

	if os.time() >= state.dwellUntil then
		BotSystem.castDebug(player, state, "DWELLING: time up at " .. (state.currentPOI and state.currentPOI.name or "?")
			.. ", going IDLE")
		state.state = BOT_STATE.IDLE
		state.walkTarget = nil
		state.currentPOI = nil
		state.pathFailCount = 0
		state.idleDepotTarget = nil
	end
end

-- ============================================================================
-- TRAVELING State: inter-city travel
-- ============================================================================

function BotSystem.startTravel(player, state)
	local town = player:getTown()
	if not town then return end
	local townId = town:getId()
	local destinations = TRAVEL_DESTINATIONS[townId]
	if not destinations or #destinations == 0 then return end

	local destTownId = destinations[math.random(1, #destinations)]
	local destTown = Town(destTownId)
	if not destTown then return end

	if math.random(1, 3) == 1 then
		BotSystem.sayRandom(player, state, "travel")
	end

	state.walkTarget = nil
	state.currentPOI = nil

	BotSystem.castDebug(player, state, "TRAVEL: " .. town:getName() .. " -> " .. destTown:getName())
	logger.info("[Bot] " .. player:getName() .. " traveling from " ..
		town:getName() .. " to " .. destTown:getName())

	-- Use proper boat travel: walk to boat -> say hi -> wait -> teleport -> walk from boat
	BotSystem.startCavebotTravel(player, state, destTownId, destTown:getName())
end

function BotSystem.doTraveling(player, state)
	-- Legacy fallback: startTravel() now uses boat-based travel via startCavebotTravel(),
	-- so this state should no longer be reached. If it is, recover gracefully.
	logger.warn("[Bot] " .. player:getName() .. " in legacy TRAVELING state, recovering to IDLE")
	state.state = BOT_STATE.IDLE
	state.travelDestTownId = nil
	state.travelUntil = nil
end

-- ============================================================================
-- Self-Defense: Fight or Flight
-- ============================================================================

function BotSystem.doSelfDefense(player, state)
	-- Don't interrupt PK mode or party mode
	if state.state == BOT_STATE.PK_ATTACK then return end
	if state.state == BOT_STATE.PARTY then return end

	local pos = player:getPosition()

	-- Handle "ignore" mode: we decided this attacker is too weak to bother with.
	-- Optionally hit them once, then continue normal behavior.
	if state.ignoredAttackerId then
		local ignored = Creature(state.ignoredAttackerId)
		if not ignored or ignored:isRemoved() or ignored:getHealth() <= 0 then
			-- Ignored attacker is gone — clear
			state.ignoredAttackerId = nil
			state.ignoredHitBack = false
			return
		end
		-- Check if they're still targeting us
		local target = ignored:getTarget()
		if not target or target:getId() ~= player:getId() then
			-- They stopped attacking us — clear
			state.ignoredAttackerId = nil
			state.ignoredHitBack = false
			return
		end
		-- Hit back once (50% chance) then go about our business
		if not state.ignoredHitBack then
			state.ignoredHitBack = true
			if math.random(1, 2) == 1 then
				player:setTarget(ignored)
				BotSystem.doCastSpell(player, state, ignored)
			end
		end
		return  -- Continue normal IDLE/DWELLING behavior
	end

	-- Find our target: remembered attacker (any z) or new attacker from spectators
	local attacker = nil
	local attackerOnSameZ = false

	-- Check remembered attacker (may be on different z for z-pursuit through stairs)
	if state.attackerId then
		local remembered = Creature(state.attackerId)
		if remembered and not remembered:isRemoved() and remembered:getHealth() > 0 then
			local rpos = remembered:getPosition()
			if rpos.z == pos.z then
				local dist = math.max(math.abs(pos.x - rpos.x), math.abs(pos.y - rpos.y))
				if dist <= BOT_CONFIG.COMBAT_LEASH_DIST then
					attacker = remembered
					attackerOnSameZ = true
				end
			else
				-- Different z — pursue through stairs if attacker is still a threat
				local skull = remembered:getSkull()
				local stillTargeting = remembered:getTarget() and remembered:getTarget():getId() == player:getId()
				if stillTargeting or (skull and skull >= 3) then
					attacker = remembered
					attackerOnSameZ = false
				end
			end
		end
	end

	-- Scan spectators for new PLAYER attackers targeting us (same z only, takes priority)
	-- Only consider Players — monsters/NPCs targeting the bot should never trigger combat
	-- Throttle: scan every 20 ticks if remembered attacker, 10 ticks if not
	-- At 500ms walk interval: 10s/5s; at 2s idle interval: 20s/10s
	local scanInterval = attacker and 20 or 10
	state.defenseScanCooldown = (state.defenseScanCooldown or 0) - 1
	if state.defenseScanCooldown <= 0 then
		state.defenseScanCooldown = scanInterval
		local spectators = Game.getSpectators(pos, false, false, 7, 7, 5, 5)
		if spectators and #spectators > 0 then
			local bestDist = 999
			for _, creature in ipairs(spectators) do
				if creature ~= player and not creature:isRemoved() and creature:getHealth() > 0
					and Player(creature) then
					local target = creature:getTarget()
					if target and target:getId() == player:getId() then
						local tpos = creature:getPosition()
						if tpos.z == pos.z then
							local dist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))
							if dist < bestDist then
								bestDist = dist
								attacker = creature
								attackerOnSameZ = true
							end
						end
					end
				end
			end
		end
	end

	-- If no attacker found, check skull-based memory for off-screen attacker
	if not attacker and state.attackerId then
		local remembered = Creature(state.attackerId)
		if remembered and not remembered:isRemoved() and remembered:getHealth() > 0 then
			local skull = remembered:getSkull()
			if skull and skull >= 3 then
				state.lastAttackerSeen = state.lastAttackerSeen or os.time()
				if os.time() - state.lastAttackerSeen < 60 then
					-- Stay in combat — chase through stairs if on different z
					if state.state == BOT_STATE.COMBAT then
						BotSystem.chaseTarget(player, state, remembered)
					end
					return
				end
			end
		end
	end

	if not attacker then
		-- No attacker found and memory expired — exit combat
		if state.state == BOT_STATE.COMBAT or state.state == BOT_STATE.FLEEING then
			BotSystem.exitCombat(player, state)
		end
		return
	end

	-- Attacker found — reset the memory timer
	state.lastAttackerSeen = os.time()

	-- Enter combat if not already in a combat state
	if state.state ~= BOT_STATE.COMBAT and state.state ~= BOT_STATE.FLEEING then
		state.combatStartTime = os.time()
		state.lastCombatProgress = os.time()
		state.attackerId = attacker:getId()
		state.walkTarget = nil
		state.currentPOI = nil
		state.returningHome = false
		state.returnBreadcrumbs = {}
		-- Remember where we were before combat so we can return
		local combatPos = player:getPosition()
		state.preCombatPos = { x = combatPos.x, y = combatPos.y, z = combatPos.z }
		state.zBreadcrumbs = {}

		-- Level-based combat decision:
		-- If bot significantly outlevels attacker (6x+ level),
		-- fight chance drops from 50% to ~17%, and "ignore" replaces "flee".
		local botLevel = player:getLevel()
		local attackerLevel = 0
		local ap = Player(attacker:getId())
		if ap then attackerLevel = ap:getLevel() end

		local outleveling = (attackerLevel > 0 and botLevel >= attackerLevel * 6)

		if outleveling then
			-- High-level bot vs weak attacker: 17% fight, 83% ignore
			local roll = math.random(1, 6)
			if roll == 1 then
				state.state = BOT_STATE.COMBAT
				state.combatDecision = "fight"
				if math.random(1, 2) == 1 then
					BotSystem.sayRandom(player, state, "combat")
				end
				logger.info("[Bot] " .. player:getName() .. " (lv" .. botLevel ..
					") FIGHTING weaker attacker " .. attacker:getName() .. " (lv" .. attackerLevel .. ")")
			else
				-- Ignore: hit once maybe, then go back to normal
				state.ignoredAttackerId = attacker:getId()
				state.ignoredHitBack = false
				state.combatDecision = nil
				state.combatStartTime = nil
				state.attackerId = nil
				logger.info("[Bot] " .. player:getName() .. " (lv" .. botLevel ..
					") IGNORING weak attacker " .. attacker:getName() .. " (lv" .. attackerLevel .. ")")
				return
			end
		else
			-- Normal case: 50% fight, 50% flee
			if math.random(1, 2) == 1 then
				state.state = BOT_STATE.COMBAT
				state.combatDecision = "fight"
				if math.random(1, 2) == 1 then
					BotSystem.sayRandom(player, state, "combat")
				end
				logger.info("[Bot] " .. player:getName() .. " FIGHTING " .. attacker:getName())
			else
				state.state = BOT_STATE.FLEEING
				state.combatDecision = "flee"
				if math.random(1, 2) == 1 then
					BotSystem.sayRandom(player, state, "flee")
				end
				logger.info("[Bot] " .. player:getName() .. " FLEEING from " .. attacker:getName())
			end
		end
	end

	-- Update remembered attacker (in case a new closer one appeared)
	state.attackerId = attacker:getId()

	-- Re-check: if attacker stopped targeting us AND has no PK skull, disengage
	-- (They gave up or skull expired — no reason to keep fighting)
	if state.state == BOT_STATE.COMBAT or state.state == BOT_STATE.FLEEING then
		local target = attacker:getTarget()
		local stillTargeting = target and target:getId() == player:getId()
		local skull = attacker:getSkull()
		local hasPKSkull = skull and skull >= 3
		if not stillTargeting and not hasPKSkull then
			logger.info("[Bot] " .. player:getName() .. " disengaging: attacker "
				.. attacker:getName() .. " no longer targeting us and no skull")
			BotSystem.exitCombat(player, state)
			return
		end
	end

	-- Execute decision
	if state.state == BOT_STATE.COMBAT then
		-- Set target (server auto-attacks with weapon) + cast spell
		if player:getTarget() ~= attacker then
			player:setTarget(attacker)
		end
		BotSystem.doCastSpell(player, state, attacker)
		BotSystem.chaseTarget(player, state, attacker)  -- chaseTarget handles z-pursuit
	elseif state.state == BOT_STATE.FLEEING then
		if attackerOnSameZ then
			BotSystem.doFlee(player, state, attacker)
		end
	end
end

-- Cache ladder IDs at load time for z-transition item detection
BOT_LADDER_IDS = {}
if Game and Game.getLadderIds then
	local ids = Game.getLadderIds()
	if ids then
		for _, id in ipairs(ids) do
			BOT_LADDER_IDS[id] = true
		end
	end
end
-- Item 435 is a sewer grate / trapdoor (goes DOWN)
BOT_SEWER_ID = 435

function BotSystem.findZTransitions(centerPos, radius, goDown)
	-- Find all ways to change z-level within radius of centerPos.
	-- goDown=true: find transitions going DOWN (z increases)
	-- goDown=false: find transitions going UP (z decreases)
	-- Returns list of {pos=Position, type="stairs"|"ladder"|"sewer", dist=N}
	local results = {}
	for dx = -radius, radius do
		for dy = -radius, radius do
			local checkPos = Position(centerPos.x + dx, centerPos.y + dy, centerPos.z)
			local tile = Tile(checkPos)
			if tile then
				local d = math.abs(dx) + math.abs(dy)
				-- Check floor-change tiles (stairs/ramps)
				if goDown and tile:hasFlag(TILESTATE_FLOORCHANGE_DOWN) then
					table.insert(results, { pos = checkPos, type = "stairs", dist = d })
				elseif not goDown and tile:hasFlag(TILESTATE_FLOORCHANGE)
					and not tile:hasFlag(TILESTATE_FLOORCHANGE_DOWN) then
					table.insert(results, { pos = checkPos, type = "stairs", dist = d })
				end
				-- Check useable items (ladders go UP, sewers go DOWN)
				local items = tile:getItems()
				if items then
					for _, item in ipairs(items) do
						local itemId = item:getId()
						if not goDown and BOT_LADDER_IDS[itemId] then
							table.insert(results, { pos = checkPos, type = "ladder", dist = d })
							break
						elseif goDown and itemId == BOT_SEWER_ID then
							table.insert(results, { pos = checkPos, type = "sewer", dist = d })
							break
						end
					end
				end
				-- Check rope spots (going UP only) — uses Tile:isRopeSpot() from tile.lua
				if not goDown and tile:isRopeSpot() then
					table.insert(results, { pos = checkPos, type = "rope", dist = d })
				end
			end
		end
	end
	table.sort(results, function(a, b) return a.dist < b.dist end)
	return results
end

function BotSystem.chaseTarget(player, state, target)
	local pos = player:getPosition()
	local tpos = target:getPosition()

	-- PZ check: stop chasing if target is in protection zone
	local tTile = Tile(tpos)
	if tTile and tTile:hasFlag(TILESTATE_PROTECTIONZONE) then
		return
	end

	-- Z-level pursuit: if target went to different floor, find and walk to
	-- the floor-change tile near their last same-z position.
	-- The last known position might be 1-2 tiles from the actual stair/portal,
	-- so we scan a radius around it for TILESTATE_FLOORCHANGE tiles.
	if pos.z ~= tpos.z then
		-- Target on different z-level — find directionally correct transitions
		local goDown = tpos.z > pos.z  -- target is below us (higher z number = lower in world)

		-- Collect transitions from multiple search centers, deduped
		local allTransitions = {}
		local seen = {}

		local function addFrom(center)
			local transitions = BotSystem.findZTransitions(center, 10, goDown)
			for _, entry in ipairs(transitions) do
				local key = entry.pos.x .. "," .. entry.pos.y .. "," .. entry.type
				if not seen[key] then
					seen[key] = true
					table.insert(allTransitions, entry)
				end
			end
		end

		if state.targetLastSameZPos then
			addFrom(Position(state.targetLastSameZPos.x, state.targetLastSameZPos.y, pos.z))
		end
		addFrom(Position(pos.x, pos.y, pos.z))
		addFrom(Position(tpos.x, tpos.y, pos.z))

		-- Sort by distance from bot
		table.sort(allTransitions, function(a, b)
			local da = math.abs(pos.x - a.pos.x) + math.abs(pos.y - a.pos.y)
			local db = math.abs(pos.x - b.pos.x) + math.abs(pos.y - b.pos.y)
			return da < db
		end)

		-- NOTE: Canary's A* pathfinder rejects floor-change tiles (tile.cpp:610).
		-- For stairs: path to 1 tile away, then manually step onto them.
		-- For ladders/sewers: path to the tile, then teleport (simulating "use").
		if #allTransitions > 0 and not player:isWalking() then
			local walked = false
			for _, entry in ipairs(allTransitions) do
				local fc = entry.pos
				local dist = math.max(math.abs(pos.x - fc.x), math.abs(pos.y - fc.y))

				if entry.type == "ladder" or entry.type == "sewer" then
					-- Ladder/sewer: need to be ON the tile, then teleport
					if dist == 0 then
						-- We're on the item — simulate "use" via teleport
						local destPos = Position(fc.x, fc.y, fc.z)
						if entry.type == "ladder" then
							destPos.z = destPos.z - 1 -- ladder goes UP (z decreases)
							destPos.y = destPos.y - 1 -- offset north to land next to hole
						else
							destPos.z = destPos.z + 1
						end
						player:teleportTo(destPos, true)
						state.lastCombatProgress = os.time()
						-- Record breadcrumb for return navigation
						table.insert(state.zBreadcrumbs, {
							fromPos = { x = fc.x, y = fc.y, z = fc.z },
							toZ = destPos.z,
							type = entry.type
						})
						logger.info("[Bot] " .. player:getName() .. " z-pursuit: used " .. entry.type
							.. " at (" .. fc.x .. "," .. fc.y .. "," .. fc.z
							.. ") -> z=" .. destPos.z)
						walked = true
						break
					end
					-- Path directly to the ladder/sewer tile (these aren't FLOORCHANGE tiles,
					-- so the pathfinder doesn't reject them)
					local dirs = player:getPathTo(fc, 0, 0, true, false, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs > 0 then
						player:startAutoWalk(dirs)
						logger.info("[Bot] " .. player:getName() .. " z-pursuit: walking to "
							.. entry.type .. " at (" .. fc.x .. "," .. fc.y .. "," .. fc.z
							.. ") dist=" .. dist)
						walked = true
						break
					end
				else
					-- Stairs: A* rejects these tiles, so path to adjacent then step on
					if dist == 0 then
						logger.info("[Bot] " .. player:getName() .. " ON stairs at ("
							.. fc.x .. "," .. fc.y .. "," .. fc.z .. ") waiting for z-transition")
						walked = true
						break
					end
					if dist == 1 then
						-- Adjacent — manually step onto the stair
						local stepDir = Position.getDirectionTo(pos, fc)
						if stepDir then
							player:startAutoWalk({stepDir})
							state.lastCombatProgress = os.time()
							-- Record breadcrumb for return navigation
							local destZ = goDown and (fc.z + 1) or (fc.z - 1)
							table.insert(state.zBreadcrumbs, {
								fromPos = { x = fc.x, y = fc.y, z = fc.z },
								toZ = destZ,
								type = "stairs"
							})
							logger.info("[Bot] " .. player:getName() .. " z-pursuit: stepping onto stairs at ("
								.. fc.x .. "," .. fc.y .. "," .. fc.z .. ")")
							walked = true
							break
						end
					end
					-- Path to within 1 tile of the stairs
					local dirs = player:getPathTo(fc, 1, 1, true, false, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs > 0 then
						player:startAutoWalk(dirs)
						logger.info("[Bot] " .. player:getName() .. " z-pursuit: walking near stairs at ("
							.. fc.x .. "," .. fc.y .. "," .. fc.z .. ") dist=" .. dist)
						walked = true
						break
					end
				end
			end
			if not walked then
				-- Try doors first, then clear blocking monsters toward nearest transition
				if not BotSystem.tryOpenDoors(player, allTransitions[1].pos) then
					BotSystem.tryAttackBlockingCreature(player, state, allTransitions[1].pos)
				end
				logger.info("[Bot] " .. player:getName() .. " z-pursuit: can't reach any of "
					.. #allTransitions .. " transitions (" .. (goDown and "DOWN" or "UP") .. ")")
			end
		elseif #allTransitions == 0 then
			logger.info("[Bot] " .. player:getName() .. " z-pursuit: no "
				.. (goDown and "DOWN" or "UP") .. " transitions found")
		end
		return
	end

	-- Same floor — track target's position for z-level pursuit
	state.targetLastSameZPos = { x = tpos.x, y = tpos.y, z = tpos.z }

	-- Get attack range based on vocation
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4
	local attackRange = 1  -- Knight: melee
	if baseVoc == 1 then attackRange = 3      -- Sorcerer
	elseif baseVoc == 2 then attackRange = 3  -- Druid
	elseif baseVoc == 3 then attackRange = 5  -- Paladin
	end

	local dist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))

	-- Only stop if in range AND have clear line of sight (no wall between us)
	if dist <= attackRange and pos:isSightClear(tpos, true) then return end

	-- If walking, check if target moved enough to justify recalculating
	if player:isWalking() then
		if state.chaseLastTargetPos then
			local old = state.chaseLastTargetPos
			local moved = math.max(math.abs(tpos.x - old.x), math.abs(tpos.y - old.y))
			if moved < 3 then return end -- target hasn't moved much
		else
			return
		end
	end

	-- Pathfind to target with clearSight=true so path goes AROUND walls
	state.chaseLastTargetPos = { x = tpos.x, y = tpos.y, z = tpos.z }
	local dirs = player:getPathTo(tpos, 0, attackRange, true, true, BOT_CONFIG.PATH_MAX_DIST)
	if not dirs or type(dirs) ~= "table" or #dirs == 0 then
		-- Fallback: try without clearSight (get as close as possible)
		dirs = player:getPathTo(tpos, 0, attackRange, true, false, BOT_CONFIG.PATH_MAX_DIST)
	end
	if not dirs or type(dirs) ~= "table" or #dirs == 0 then
		-- Try slight offsets as last resort
		for _ = 1, 3 do
			local offsetPos = Position(
				tpos.x + math.random(-1, 1),
				tpos.y + math.random(-1, 1),
				tpos.z
			)
			dirs = player:getPathTo(offsetPos, 0, attackRange, true, true, BOT_CONFIG.PATH_MAX_DIST)
			if dirs and type(dirs) == "table" and #dirs > 0 then break end
		end
	end

	-- If pathfinding still fails, try clearing obstacles
	if not dirs or type(dirs) ~= "table" or #dirs == 0 then
		-- Try doors first
		if BotSystem.tryOpenDoors(player, tpos) then return end
		-- Try attacking blocking monsters/NPCs between us and the target
		BotSystem.tryAttackBlockingCreature(player, state, tpos)
		return
	end

	state.lastCombatProgress = os.time()
	player:startAutoWalk(dirs)
end

-- Attack the nearest monster or NPC blocking the path toward a target position.
-- This lets bots clear their way through sewers/caves when pursuing a player.
function BotSystem.tryAttackBlockingCreature(player, state, targetPos)
	local pos = player:getPosition()
	local dx = targetPos.x - pos.x
	local dy = targetPos.y - pos.y
	local dist = math.max(math.abs(dx), math.abs(dy), 1)
	local ndx = dx > 0 and 1 or (dx < 0 and -1 or 0)
	local ndy = dy > 0 and 1 or (dy < 0 and -1 or 0)

	-- Check tiles in the direction of the target (primary diagonal, then cardinal)
	local tilesToCheck = {}
	if ndx ~= 0 or ndy ~= 0 then
		table.insert(tilesToCheck, Position(pos.x + ndx, pos.y + ndy, pos.z))
	end
	if ndx ~= 0 then
		table.insert(tilesToCheck, Position(pos.x + ndx, pos.y, pos.z))
	end
	if ndy ~= 0 then
		table.insert(tilesToCheck, Position(pos.x, pos.y + ndy, pos.z))
	end

	-- Also check tiles 2 away for ranged vocations
	if ndx ~= 0 or ndy ~= 0 then
		table.insert(tilesToCheck, Position(pos.x + ndx * 2, pos.y + ndy * 2, pos.z))
	end

	for _, tilePos in ipairs(tilesToCheck) do
		local tile = Tile(tilePos)
		if tile then
			local creatures = tile:getCreatures()
			if creatures then
				for _, creature in ipairs(creatures) do
					if creature ~= player and not creature:isRemoved() and creature:getHealth() > 0
						and not Player(creature) then
						-- Found a monster/NPC blocking the path — attack it
						BotSystem.doAttackCreature(player, state, creature)
						return
					end
				end
			end
		end
	end
end

-- Attack any creature (monster/NPC) to clear path.
-- Uses native targeting: setAttackedCreature (server auto-attacks) + cast a spell.
function BotSystem.doAttackCreature(player, state, target)
	local now = os.time()
	if now - state.lastAttackTime < BOT_CONFIG.ATTACK_COOLDOWN then return end

	local pos = player:getPosition()
	local tpos = target:getPosition()
	if pos.z ~= tpos.z then return end

	local dist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))
	if dist > 5 then return end -- too far

	local hasLOS = pos:isSightClear(tpos, true)

	-- Set target (server handles weapon auto-attacks if LOS is clear)
	if player:getTarget() ~= target then
		player:setTarget(target)
	end

	-- Ensure mana
	if player:getMana() < player:getMaxMana() * 0.3 then
		player:addMana(player:getMaxMana())
	end

	-- Also cast a spell for extra damage (requires LOS)
	if hasLOS then
		local spell = BotSystem.selectAttackSpell(player, state, target)
		if spell then
			local ok = player:castSpell(spell.name, target)
			if ok then
				state.lastAttackTime = now
				state.lastCombatProgress = now
				state["spellCd_" .. spell.name] = now + spell.cd
			end
		else
			state.lastAttackTime = now
		end
	else
		-- No LOS — weapon auto-attack won't work either
		-- Just set cooldown so we don't spam
		state.lastAttackTime = now
		BotSystem.combatLog(player, state, "HUNT-ATTACK-NOLOS: " .. target:getName()
			.. " dist=" .. dist
			.. " at (" .. tpos.x .. "," .. tpos.y .. "," .. tpos.z .. ")")
	end
end

function BotSystem.tryOpenDoors(player, targetPos)
	local pos = player:getPosition()
	-- Direction toward target
	local dx = targetPos.x - pos.x
	local dy = targetPos.y - pos.y
	local ndx = dx > 0 and 1 or (dx < 0 and -1 or 0)
	local ndy = dy > 0 and 1 or (dy < 0 and -1 or 0)

	-- Check tiles in the direction of the target (primary + orthogonal)
	local tilesToCheck = {}
	if ndx ~= 0 or ndy ~= 0 then
		table.insert(tilesToCheck, Position(pos.x + ndx, pos.y + ndy, pos.z))
	end
	if ndx ~= 0 then
		table.insert(tilesToCheck, Position(pos.x + ndx, pos.y, pos.z))
	end
	if ndy ~= 0 then
		table.insert(tilesToCheck, Position(pos.x, pos.y + ndy, pos.z))
	end

	for _, tilePos in ipairs(tilesToCheck) do
		local tile = Tile(tilePos)
		if tile then
			local items = tile:getItems()
			if items then
				for _, item in ipairs(items) do
					local itemId = item:getId()
					local openId = CLOSED_TO_OPEN_DOOR[itemId]
					if openId then
						item:transform(openId)
						logger.info("[Bot] " .. player:getName() ..
							" opened door at (" .. tilePos.x .. "," .. tilePos.y .. "," .. tilePos.z .. ")")
						return true
					end
				end
			end
		end
	end
	return false
end

function BotSystem.doFlee(player, state, attacker)
	if player:isWalking() then return end

	local pos = player:getPosition()
	local apos = attacker:getPosition()
	local dx = pos.x - apos.x
	local dy = pos.y - apos.y
	local dist = math.max(math.abs(dx), math.abs(dy), 1)
	local fleeX = pos.x + math.floor(dx / dist * BOT_CONFIG.FLEE_DISTANCE)
	local fleeY = pos.y + math.floor(dy / dist * BOT_CONFIG.FLEE_DISTANCE)

	BotSystem.goTo(player, Position(fleeX, fleeY, pos.z))
end

function BotSystem.exitCombat(player, state)
	player:setTarget(nil)
	player:setFollowCreature(nil)

	state.combatDecision = nil
	state.combatStartTime = nil
	state.lastCombatProgress = nil
	state.attackerId = nil
	state.lastAttackerSeen = nil
	state.chaseLastTargetPos = nil
	state.targetLastSameZPos = nil
	state.ignoredAttackerId = nil
	state.ignoredHitBack = false
	state.walkTarget = nil
	state.currentPOI = nil
	state.pvpManaSpent = nil
	state.combatHpCheckTime = nil
	state.combatHpBaseline = nil
	state.combatStalemateCount = nil

	-- Resume hunting if a hunt was in progress (hunt fields preserved during combat)
	if state.huntScriptId then
		state.returningHome = false
		state.returnBreadcrumbs = {}
		state.returnDestination = nil
		state.preCombatPos = nil
		state.zBreadcrumbs = {}
		state.state = BOT_STATE.HUNTING
		BotSystem.castDebug(player, state, "COMBAT EXIT: resuming hunt phase=" .. (state.huntPhase or "?"))
		logger.info("[Bot] " .. player:getName() .. " exiting combat, resuming hunt "
			.. (state.huntScriptId or "?") .. " phase=" .. (state.huntPhase or "?"))
		return
	end

	-- If we took z-transitions during combat, navigate back the way we came
	if state.preCombatPos and state.zBreadcrumbs and #state.zBreadcrumbs > 0 then
		-- Reverse the breadcrumbs so we retrace in opposite order
		local reversed = {}
		for i = #state.zBreadcrumbs, 1, -1 do
			local bc = state.zBreadcrumbs[i]
			table.insert(reversed, {
				-- To return: go to where we arrived (bc.toZ) and use the transition
				-- to get back to where we came from (bc.fromPos.z)
				targetZ = bc.fromPos.z,
				transitionPos = bc.fromPos,  -- the original transition tile
				type = bc.type
			})
		end
		state.returningHome = true
		state.returnBreadcrumbs = reversed
		state.returnDestination = state.preCombatPos
		state.state = BOT_STATE.IDLE
		logger.info("[Bot] " .. player:getName() .. " exiting combat, returning home via "
			.. #reversed .. " z-transition(s)")
	else
		state.returningHome = false
		state.returnBreadcrumbs = {}
		state.returnDestination = nil
		state.state = BOT_STATE.IDLE
		logger.info("[Bot] " .. player:getName() .. " exiting combat (attacker left or dead)")
	end

	state.preCombatPos = nil
	state.zBreadcrumbs = {}
end

-- ============================================================================
-- Vigilante: attack visible PKers (white skull or higher)
-- ============================================================================

function BotSystem.checkAttackPKer(player, state)
	-- Already in combat/PK — don't interrupt
	if state.state == BOT_STATE.COMBAT or state.state == BOT_STATE.FLEEING
		or state.state == BOT_STATE.PK_ATTACK then return end

	-- Throttle: only scan every 2 seconds
	local now = os.time()
	if state.lastPKerScanTime and (now - state.lastPKerScanTime) < 2 then return end
	state.lastPKerScanTime = now

	-- Don't attack within 60 seconds of respawning from death
	if state.lastDeathTime and now - state.lastDeathTime < 60 then return end

	-- Check own PZ status — don't attack from inside PZ
	local myTile = Tile(player:getPosition())
	if myTile and myTile:hasFlag(TILESTATE_PROTECTIONZONE) then return end

	local pos = player:getPosition()
	local spectators = Game.getSpectators(pos, false, true, 7, 7, 5, 5)
	if not spectators or #spectators == 0 then return end

	local botLevel = player:getLevel()

	-- Build set of currently visible PKers (skull >= 3, same z, not in PZ)
	local visiblePKerIds = {}
	local newPKers = {}  -- PKers we haven't rolled for yet
	for _, creature in ipairs(spectators) do
		if creature ~= player and not creature:isRemoved() and creature:getHealth() > 0 then
			-- Skip invisible/ghost mode (GOD/GM)
			if creature:isInGhostMode() then goto vigilante_continue end

			local skull = creature:getSkull()
			if skull and skull >= 3 then
				-- Skip admin/GM accounts
				local pp = Player(creature:getId())
				if pp then
					local group = pp:getGroup()
					if group and group:getAccess() then goto vigilante_continue end
				end

				local tTile = Tile(creature:getPosition())
				if tTile and not tTile:hasFlag(TILESTATE_PROTECTIONZONE) then
					local cpos = creature:getPosition()
					if cpos.z == pos.z then
						local cid = creature:getId()
						visiblePKerIds[cid] = true
						if not state.seenPKers[cid] then
							table.insert(newPKers, creature)
						else
							-- Already seen — update last visible timestamp
							state.seenPKers[cid] = os.time()
						end
					end
				end
			end
		end
		::vigilante_continue::
	end

	-- Clean up memory: only remove PKers who left screen AND 60 seconds have passed
	for cid, lastSeen in pairs(state.seenPKers) do
		if not visiblePKerIds[cid] then
			if os.time() - lastSeen >= 60 then
				state.seenPKers[cid] = nil
			end
		end
	end

	-- Roll 5% for each NEW PKer (ones we haven't seen before in this window)
	for _, pker in ipairs(newPKers) do
		local cid = pker:getId()
		local shouldRoll = true

		-- Level-based ignore: if bot outlevels PKer by 6x, don't bother attacking
		local pkerLevel = 0
		local pp = Player(cid)
		if pp then pkerLevel = pp:getLevel() end
		if pkerLevel > 0 and botLevel >= pkerLevel * 6 then
			-- Too weak to care about — remember and skip
			state.seenPKers[cid] = os.time()
			shouldRoll = false
		end

		if shouldRoll then
			if math.random(1, 20) == 1 then
				-- Attack this PKer
				state.state = BOT_STATE.COMBAT
				state.combatDecision = "fight"
				state.combatStartTime = os.time()
				state.attackerId = cid
				state.lastAttackerSeen = os.time()
				state.walkTarget = nil
				state.currentPOI = nil

				if math.random(1, 2) == 1 then
					BotSystem.sayRandom(player, state, "combat")
				end

				local targetName = pker:getName() or "unknown"
				logger.info("[Bot] " .. player:getName() .. " (lv" .. botLevel
					.. ") vigilante attacking PKer " .. targetName .. " (lv" .. pkerLevel .. ")")
				return
			else
				-- Remember: we decided NOT to attack this PKer (with timestamp)
				state.seenPKers[cid] = os.time()
			end
		end
	end
end

-- (Random PK handled by C++ BotEngine::checkRandomPK in bot_engine.cpp)

function BotSystem.doPKAttack(player, state)
	-- Use lastCombatProgress for timeout (updated each spell cast), not static pkStartTime
	local lastProgress = state.lastCombatProgress or state.pkStartTime
	if os.time() - lastProgress > BOT_CONFIG.PK_TIMEOUT then
		BotSystem.exitPK(player, state)
		return
	end

	local target = Creature(state.pkTarget)
	if not target or target:isRemoved() or target:getHealth() <= 0 then
		BotSystem.exitPK(player, state)
		return
	end

	local pos = player:getPosition()
	local tpos = target:getPosition()

	-- PZ safety: stop attacking if target reached protection zone
	local tTile = Tile(tpos)
	if tTile and tTile:hasFlag(TILESTATE_PROTECTIONZONE) then
		BotSystem.exitPK(player, state)
		return
	end

	-- PZ safety: stop if bot itself is in PZ (shouldn't happen with proper skull, but safety net)
	local myTile = Tile(pos)
	if myTile and myTile:hasFlag(TILESTATE_PROTECTIONZONE) then
		BotSystem.exitPK(player, state)
		return
	end

	-- Leash distance check (same z only — allow z-pursuit through stairs)
	if pos.z == tpos.z then
		local dist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))
		if dist > 10 then
			BotSystem.exitPK(player, state)
			return
		end
	end

	-- Set target (server auto-attacks with weapon) + cast spell
	if player:getTarget() ~= target then
		player:setTarget(target)
	end
	BotSystem.doCastSpell(player, state, target)
	BotSystem.chaseTarget(player, state, target)  -- chaseTarget handles z-pursuit
end

function BotSystem.exitPK(player, state)
	local oldTarget = state.pkTarget
	state.pkTarget = nil
	state.pkStartTime = nil
	state.chaseLastTargetPos = nil
	state.targetLastSameZPos = nil
	state.walkTarget = nil
	state.currentPOI = nil
	state.pvpManaSpent = nil
	state.combatHpCheckTime = nil
	state.combatHpBaseline = nil
	state.combatStalemateCount = nil
	player:setTarget(nil)
	player:setFollowCreature(nil)

	-- Check if anyone is still attacking us — if so, enter COMBAT instead of IDLE
	-- This prevents the "ignore weak attacker" roll from immediately disengaging
	local pos = player:getPosition()
	local spectators = Game.getSpectators(pos, false, false, 7, 7, 5, 5)
	local nearbyAttacker = nil
	if spectators then
		for _, creature in ipairs(spectators) do
			if creature ~= player and not creature:isRemoved() and creature:getHealth() > 0
				and Player(creature:getId()) then
				local target = creature:getTarget()
				if target and target:getId() == player:getId() then
					nearbyAttacker = creature
					break
				end
			end
		end
	end

	if nearbyAttacker then
		-- Someone is still attacking us — enter COMBAT to fight back
		state.state = BOT_STATE.COMBAT
		state.combatDecision = "fight"
		state.combatStartTime = os.time()
		state.lastCombatProgress = os.time()
		state.attackerId = nearbyAttacker:getId()
		local cpos = player:getPosition()
		state.preCombatPos = { x = cpos.x, y = cpos.y, z = cpos.z }
		state.zBreadcrumbs = {}
		logger.info("[Bot] " .. player:getName() .. " PK ended, still under attack by "
			.. nearbyAttacker:getName() .. " — entering COMBAT")
	else
		state.state = BOT_STATE.IDLE
	end
end

-- ============================================================================
-- POI Selection: weighted, multi-city, with fallback
-- ============================================================================

function BotSystem.pickNextPOI(player, state)
	local town = player:getTown()
	if not town then return nil end
	local townId = town:getId()
	local walkZ = CITY_WALK_Z[townId] or town:getTemplePosition().z

	-- Build IDLE POI list dynamically: depot, depot_outside, boat, temple
	-- (Shops and NPCs excluded from IDLE rotation — only visited during hunts)
	local idlePois = {}

	-- Find depot and temple from CITY_POIS
	local pois = CITY_POIS[townId]
	local depotPoi = nil
	local templePoi = nil
	if pois then
		for _, poi in ipairs(pois) do
			if poi.type == "depot" and not depotPoi then
				depotPoi = poi
			elseif poi.type == "temple" and not templePoi then
				templePoi = poi
			end
		end
	end

	-- Temple POI (always available via town data)
	if templePoi then
		table.insert(idlePois, {
			name = templePoi.name,
			pos = Position(templePoi.pos.x, templePoi.pos.y, walkZ),
			type = "temple",
		})
	else
		local tp = town:getTemplePosition()
		table.insert(idlePois, {
			name = town:getName() .. " Temple",
			pos = Position(tp.x, tp.y, walkZ),
			type = "temple",
		})
	end

	-- Depot POI (inside)
	if depotPoi then
		table.insert(idlePois, {
			name = depotPoi.name,
			pos = Position(depotPoi.pos.x, depotPoi.pos.y, walkZ),
			type = "depot",
		})
		-- Depot outside variant (±3-5 tiles from depot)
		local ox = math.random(-5, 5)
		local oy = math.random(-5, 5)
		if math.abs(ox) < 3 then ox = ox > 0 and 3 or -3 end
		if math.abs(oy) < 3 then oy = oy > 0 and 3 or -3 end
		table.insert(idlePois, {
			name = depotPoi.name .. " (outside)",
			pos = Position(depotPoi.pos.x + ox, depotPoi.pos.y + oy, walkZ),
			type = "depot_outside",
		})
	end

	-- Boat POI (use walk z for accessibility — bot hangs out near the dock area)
	local boatPos = BOAT_POSITIONS[townId]
	if boatPos then
		table.insert(idlePois, {
			name = town:getName() .. " Boat",
			pos = Position(boatPos.x, boatPos.y, walkZ),
			type = "boat",
		})
	end

	if #idlePois == 0 then
		return BotSystem.generateFallbackPOI(player)
	end

	-- Build candidate list excluding recently visited (by POI name)
	local visitedNames = state.visitedPOIs or {}
	local isVisited = {}
	for _, name in ipairs(visitedNames) do
		isVisited[name] = true
	end

	local candidates = {}
	local totalWeight = 0
	for _, poi in ipairs(idlePois) do
		if not isVisited[poi.name] then
			local w = POI_TYPE_WEIGHTS[poi.type] or 10
			if w > 0 then
				table.insert(candidates, { poi = poi, weight = w })
				totalWeight = totalWeight + w
			end
		end
	end

	-- If all visited recently, reset
	if #candidates == 0 or totalWeight == 0 then
		state.visitedPOIs = {}
		candidates = {}
		totalWeight = 0
		for _, poi in ipairs(idlePois) do
			local w = POI_TYPE_WEIGHTS[poi.type] or 10
			if w > 0 then
				table.insert(candidates, { poi = poi, weight = w })
				totalWeight = totalWeight + w
			end
		end
	end

	if #candidates == 0 or totalWeight == 0 then
		return BotSystem.generateFallbackPOI(player)
	end

	-- Weighted random selection
	local roll = math.random(1, totalWeight)
	local cumulative = 0
	local pick = candidates[1]
	for _, c in ipairs(candidates) do
		cumulative = cumulative + c.weight
		if roll <= cumulative then
			pick = c
			break
		end
	end

	-- Track visited by name (keep last 3 — smaller pool now)
	table.insert(visitedNames, pick.poi.name)
	if #visitedNames > 3 then
		table.remove(visitedNames, 1)
	end
	state.visitedPOIs = visitedNames

	return pick.poi
end

function BotSystem.generateFallbackPOI(player)
	local town = player:getTown()
	if not town then return nil end
	local tp = town:getTemplePosition()
	local walkZ = CITY_WALK_Z[town:getId()] or tp.z
	return {
		name = town:getName() .. " Temple",
		pos = Position(tp.x, tp.y, walkZ),
		type = "temple",
	}
end

-- ============================================================================
-- Spell Learning & Selection
-- ============================================================================

-- Learn all attack + heal spells appropriate for bot's vocation
function BotSystem.learnAllSpells(player)
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4
	local count = 0

	local atkSpells = BOT_SPELLS[baseVoc]
	if atkSpells then
		for _, spell in ipairs(atkSpells) do
			player:learnSpell(spell.name)
			count = count + 1
		end
	end

	local healSpells = BOT_HEAL_SPELLS[baseVoc]
	if healSpells then
		for _, spell in ipairs(healSpells) do
			player:learnSpell(spell.name)
			count = count + 1
		end
	end

	-- Learn universal spells
	player:learnSpell("exura")
	player:learnSpell("exura vita")

	logger.info("[Bot] " .. player:getName() .. " learned " .. count .. " spells (voc=" .. baseVoc .. ")")
end

-- Throwing weapon item IDs that need stacking (paladin ammo)
local THROWING_WEAPON_IDS = {
	[3287] = true,   -- throwing star
	[3298] = true,   -- throwing knife
	[3302] = true,   -- snowball
	[7367] = true,   -- enchanted spear
	[7368] = true,   -- assassin star
	[7378] = true,   -- royal spear
	[11230] = true,  -- viper star
	[25757] = true,  -- prismatic bolt
	[25759] = true,  -- royal star
	[14251] = true,  -- hunting spear
}
local THROWING_WEAPON_COUNT = 100  -- Stack count (ammo never consumed for bots via C++ skip)

-- Equip bot with best gear for their level and vocation.
-- Efficient: skips slots where the correct item is already equipped.
function BotSystem.equipBot(player)
	local level = math.min(player:getLevel(), 500)
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4

	local loadout = BotEquipmentData.getLoadout(level, baseVoc)
	if not loadout then
		logger.warn("[Bot] " .. player:getName() .. " no equipment loadout for lv=" .. level .. " voc=" .. baseVoc)
		return
	end

	local slotMap = {
		{field = "slot_head",  slot = CONST_SLOT_HEAD,  name = "head"},
		{field = "slot_armor", slot = CONST_SLOT_ARMOR, name = "armor"},
		{field = "slot_legs",  slot = CONST_SLOT_LEGS,  name = "legs"},
		{field = "slot_feet",  slot = CONST_SLOT_FEET,  name = "feet"},
		{field = "slot_right", slot = CONST_SLOT_LEFT,  name = "weapon"},  -- DB slot_right = weapon -> left hand
		{field = "slot_left",  slot = CONST_SLOT_RIGHT, name = "shield"},  -- DB slot_left = shield -> right hand
	}

	local equipped = 0
	local updated = 0
	local failed = {}
	for _, s in ipairs(slotMap) do
		local itemId = loadout[s.field]
		if itemId and itemId > 0 then
			-- Check if already equipped correctly
			local current = player:getSlotItem(s.slot)
			local currentId = current and current:getId() or 0
			if currentId == itemId then
				equipped = equipped + 1
			else
				-- Need to equip or replace
				local count = THROWING_WEAPON_IDS[itemId] and THROWING_WEAPON_COUNT or 1
				local ok = player:setSlotItem(s.slot, itemId, count)
				if ok then
					equipped = equipped + 1
					updated = updated + 1
				else
					table.insert(failed, s.name .. "=" .. itemId)
				end
			end
		end
	end

	if #failed > 0 then
		logger.warn("[Bot] " .. player:getName() .. " equip FAILED for: " .. table.concat(failed, ", ")
			.. " (lv=" .. level .. " voc=" .. baseVoc .. ")")
	end
	if updated > 0 then
		logger.info("[Bot] " .. player:getName() .. " equipped " .. equipped .. "/6 items, " .. updated .. " updated"
			.. " (lv=" .. level .. " voc=" .. baseVoc .. ")")
	end
end

-- Select best available attack spell for the given target
-- Returns the spell table entry or nil
function BotSystem.selectAttackSpell(player, state, target)
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4
	local level = player:getLevel()
	local spells = BOT_SPELLS[baseVoc]
	if not spells then return nil end

	local pos = player:getPosition()
	local tpos = target:getPosition()
	local dist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))
	local now = os.time()

	-- Determine if target is a player (for PvP-only spells like SD rune)
	local targetIsPlayer = Player(target:getId()) ~= nil
	-- For monsters: SD rune usable only if monster HP >= 5x bot's max HP AND not immune to death
	local targetIsStrongMonster = false
	if not targetIsPlayer then
		local mon = Monster(target:getId())
		if mon then
			local monHp = mon:getMaxHealth()
			local botHp = Player(player:getId()) and player:getMaxHealth() or 1
			if monHp >= botHp * 5 then
				-- Check death immunity via monster type element map
				-- getElementList() returns table[combatType] = absorbPercent
				local mtype = mon:getType()
				local elementMap = mtype and mtype:getElementList() or nil
				local deathAbsorb = elementMap and elementMap[COMBAT_DEATHDAMAGE] or 0
				if deathAbsorb < 100 then
					targetIsStrongMonster = true
				end
			end
		end
	end

	-- Collect all available spells (off cooldown, in range, level met)
	local available = {}
	for i = 1, #spells do
		local spell = spells[i]
		if level >= spell.level then
			-- pvpOnly spells (SD rune): only vs players or strong non-immune monsters
			if spell.pvpOnly and not targetIsPlayer and not targetIsStrongMonster then
				-- skip this spell
			else
				local cdKey = "spellCd_" .. spell.name
				if not state[cdKey] or now >= state[cdKey] then
					if spell.range == 0 or dist <= spell.range then
						available[#available + 1] = spell
					end
				end
			end
		end
	end

	if #available == 0 then return nil end

	-- Pick randomly from available spells
	return available[math.random(#available)]
end

-- ============================================================================
-- Combat: Attack and Healing
-- ============================================================================

-- Cast a spell on the current target.
-- Weapon auto-attacks are handled by the server via setAttackedCreature.
-- This function only handles spell casting: select a random available spell and cast it.
-- The spell system uses getAttackedCreature() for targeting automatically.
function BotSystem.doCastSpell(player, state, target)
	local now = os.time()
	if now - state.lastAttackTime < BOT_CONFIG.ATTACK_COOLDOWN then return end

	local pos = player:getPosition()
	local tpos = target:getPosition()
	local dist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))

	-- Must be on same floor with line of sight for spells
	if pos.z ~= tpos.z then return end
	if not pos:isSightClear(tpos, true) then
		state.spellLOSFailCount = (state.spellLOSFailCount or 0) + 1
		BotSystem.combatLog(player, state, "HUNT-SPELL-BLOCKED: noLOS to " .. target:getName()
			.. " dist=" .. dist
			.. " at (" .. tpos.x .. "," .. tpos.y .. "," .. tpos.z .. ")"
			.. " from (" .. pos.x .. "," .. pos.y .. "," .. pos.z .. ")"
			.. " fails=" .. state.spellLOSFailCount)
		-- Still count as combat progress since weapon auto-attack may still work
		-- (server checks LOS independently for weapon attacks)
		state.lastCombatProgress = now
		return
	end

	state.spellLOSFailCount = 0

	-- Select a random available spell (level-gated, cooldown-aware, range-checked)
	local spell = BotSystem.selectAttackSpell(player, state, target)
	if not spell then
		-- No spell available but weapon auto-attack is running via setTarget
		state.lastCombatProgress = now
		return
	end

	-- Ensure mana for casting (PvP: mana budget is limited so bots eventually run dry and die)
	if player:getMana() < player:getMaxMana() * 0.3 then
		local targetIsPlayer = Player(target:getId()) ~= nil
		if targetIsPlayer then
			local maxMana = player:getMaxMana()
			local budget = maxMana * 2.5  -- 5 refills of ~50% each
			state.pvpManaSpent = state.pvpManaSpent or 0
			if state.pvpManaSpent < budget then
				player:addMana(maxMana)
				state.pvpManaSpent = state.pvpManaSpent + maxMana
			end
		else
			player:addMana(player:getMaxMana())
		end
	end

	-- For isRune spells (SD): skip castSpell (no actual spell to cast), apply damage directly
	local ok = false
	if spell.isRune then
		ok = true  -- rune always "succeeds"
		-- Visual: show projectile + effect
		if spell.effect then
			tpos:sendMagicEffect(spell.effect)
		end
	else
		-- Cast through real spell system — spell uses attackedCreature for targeting
		ok = player:castSpell(spell.name, target)
	end

	if ok then
		state.lastAttackTime = now
		state.lastCombatProgress = now
		state["spellCd_" .. spell.name] = now + spell.cd

		-- Standard spell combat handles monster damage via the spell system.
		-- For PvP (bot vs player/bot), spell damage is blocked by Tibia PvP rules,
		-- so apply damage directly using the addHealth PvP bypass (nullptr attacker).
		-- Also needed for isRune spells which have no real spell to cast.
		local targetPlayer = Player(target:getId())
		if targetPlayer or spell.isRune then
			local baseDmg = spell.baseDmg or 20
			local level = player:getLevel()
			local dmg = math.floor(baseDmg * (1 + level / 100) * (0.8 + math.random() * 0.4))
			target:addHealth(-dmg, spell.combatType or COMBAT_PHYSICALDAMAGE, player)
			BotSystem.combatLogForce(player, state, "SPELL-DMG: " .. spell.name
				.. " -" .. dmg .. "hp to " .. target:getName()
				.. " (base=" .. baseDmg .. " lv=" .. level .. ")"
				.. (spell.isRune and " [rune]" or " [pvp-bypass]"))
		end

		BotSystem.combatLog(player, state, "HUNT-SPELL: " .. spell.name
			.. " ok=true target=" .. target:getName() .. " dist=" .. dist)
	else
		-- Spell failed but weapon auto-attack still running
		state.lastCombatProgress = now
		state.lastAttackTime = now
		BotSystem.combatLog(player, state, "HUNT-SPELL: " .. spell.name
			.. " ok=false target=" .. target:getName() .. " dist=" .. dist)
	end
end

function BotSystem.doHealing(player, state)
	-- PvP mana budget exhausted — stop healing entirely (simulates running out of mana for heals)
	local inPvP = false
	if state.state == BOT_STATE.PK_ATTACK then
		inPvP = true
	elseif (state.state == BOT_STATE.COMBAT or state.state == BOT_STATE.FLEEING) and state.attackerId then
		inPvP = Player(state.attackerId) ~= nil
	end
	if inPvP and (state.pvpManaSpent or 0) >= (player:getMaxMana() * 2.5) then
		return  -- No mana left to heal — bot will die from damage
	end

	local hp = player:getHealth()
	local maxHp = player:getMaxHealth()
	if (hp / math.max(maxHp, 1)) * 100 > BOT_CONFIG.HEAL_THRESHOLD_PERCENT then
		return
	end

	local now = os.time()
	if now - state.lastHealTime < BOT_CONFIG.HEAL_COOLDOWN then return end

	-- Select best heal spell (level-gated, cooldown-aware)
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4
	local level = player:getLevel()
	local healSpells = BOT_HEAL_SPELLS[baseVoc]
	local bestHeal = nil

	if healSpells then
		for i = #healSpells, 1, -1 do
			local spell = healSpells[i]
			if level >= spell.level then
				local cdKey = "healCd_" .. spell.name
				if not state[cdKey] or now >= state[cdKey] then
					bestHeal = spell
					break
				end
			end
		end
	end

	if not bestHeal then
		bestHeal = {name = "exura", heal = 80, cd = 1}
	end

	state.lastHealTime = now
	state["healCd_" .. bestHeal.name] = now + bestHeal.cd

	-- Scale heal amount by level and magic level
	local magicLevel = player:getMagicLevel()
	local healAmount = bestHeal.heal + math.floor(level / 5 + magicLevel * 2)
	local pos = player:getPosition()

	player:say(bestHeal.name, TALKTYPE_MONSTER_SAY)
	pos:sendMagicEffect(CONST_ME_MAGIC_BLUE)
	player:addHealth(healAmount)
	-- PvP mana budget: track total mana added, stop when budget exhausted
	local inPvP = false
	if state.state == BOT_STATE.PK_ATTACK then
		inPvP = true
	elseif (state.state == BOT_STATE.COMBAT or state.state == BOT_STATE.FLEEING) and state.attackerId then
		inPvP = Player(state.attackerId) ~= nil
	end
	local manaRegen = math.floor(level * 2 + 50)
	if inPvP then
		local maxMana = player:getMaxMana()
		local budget = maxMana * 2.5  -- 5 refills of ~50% each
		state.pvpManaSpent = state.pvpManaSpent or 0
		if state.pvpManaSpent < budget then
			player:addMana(manaRegen)
			state.pvpManaSpent = state.pvpManaSpent + manaRegen
		end
		-- Once budget exhausted, no more mana regen — bot drains and dies
	else
		player:addMana(manaRegen)
	end
end

-- ============================================================================
-- Chat
-- ============================================================================

function BotSystem.sayRandom(player, state, category)
	local now = os.time()
	if now - state.lastChatTime < 60 then return end
	state.lastChatTime = now

	local phrases = BOT_CHAT[category] or BOT_CHAT.idle
	if #phrases > 0 then
		player:say(phrases[math.random(1, #phrases)], TALKTYPE_SAY)
	end
end

-- ============================================================================
-- Population Helpers
-- ============================================================================

function BotSystem.countActiveBots()
	local count = 0
	for _, active in pairs(BotActive) do
		if active then count = count + 1 end
	end
	return count
end

function BotSystem.getInactiveBots()
	local inactive = {}
	for guid, active in pairs(BotActive) do
		if not active and BotPlayers[guid] and not BotPlayers[guid]:isRemoved() then
			table.insert(inactive, guid)
		end
	end
	return inactive
end

-- ============================================================================
-- Hunting System
-- ============================================================================

-- Try to start a hunt: find eligible scripts, reserve one, begin travel
function BotSystem.tryStartHunt(player, state)
	local botData = {
		townId = player:getTown() and player:getTown():getId() or 8,
	}
	local eligible = BotHuntData.findHuntsForBot(player, botData)
	if #eligible == 0 then
		return false
	end

	-- Pick from top candidates (prefer same-town hunts)
	local pick = nil
	-- Try same-town first
	for _, entry in ipairs(eligible) do
		if entry.script.town_id == botData.townId then
			pick = entry
			break
		end
	end
	-- Fallback to any eligible
	if not pick then
		pick = eligible[math.random(1, math.min(5, #eligible))]
	end

	-- Reserve the hunt (1-bot-per-spawn)
	local scriptId = pick.scriptId
	if not BotHuntData.reserveHunt(scriptId, player:getGuid()) then
		return false
	end

	-- Roll hunt duration
	local duration = math.random(BOT_CONFIG.HUNT_DURATION_MIN, BOT_CONFIG.HUNT_DURATION_MAX)

	-- Set up hunt state
	state.huntScriptId = scriptId
	state.huntTownId = pick.script.town_id
	state.huntStartTime = os.time()
	state.huntEndTime = os.time() + duration
	state.huntKillCount = 0
	state.huntMonsterTarget = nil
	state.huntWaypointSkipCount = 0
	state.huntLastPos = nil
	state.huntTargets = BotHuntData.getTargets(scriptId)

	-- If hunt is in a different town, travel there first
	if pick.script.town_id ~= botData.townId then
		-- Check if reachable
		local destinations = TRAVEL_DESTINATIONS[botData.townId]
		if destinations then
			local canReach = false
			for _, destId in ipairs(destinations) do
				if destId == pick.script.town_id then
					canReach = true
					break
				end
			end
			if not canReach then
				-- Can't reach this town directly — release and skip
				BotHuntData.releaseHunt(scriptId, player:getGuid())
				state.huntScriptId = nil
				return false
			end
		end
		-- Travel to hunt town first, then begin hunt
		state.travelDestTownId = pick.script.town_id
		state.travelUntil = os.time() + math.random(BOT_CONFIG.TRAVEL_PAUSE_MIN, BOT_CONFIG.TRAVEL_PAUSE_MAX)
		state.state = BOT_STATE.TRAVELING
		state.walkTarget = nil
		state.currentPOI = nil
		BotSystem.castDebug(player, state, "HUNT START: traveling to " .. pick.script.town_name
			.. " for " .. pick.script.name
			.. " (duration=" .. math.floor(duration / 60) .. "m)")
		logger.info("[Bot] " .. player:getName() .. " traveling to " .. pick.script.town_name
			.. " for hunt: " .. pick.script.name
			.. " (duration=" .. math.floor(duration / 60) .. "m)")
		return true
	end

	-- Same town: prepare (depot + shops) then travel to spawn
	BotSystem.castDebug(player, state, "HUNT START: " .. pick.script.name
		.. " in " .. pick.script.town_name
		.. " (duration=" .. math.floor(duration / 60) .. "m"
		.. " targets=" .. #(state.huntTargets or {}) .. ")")
	BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PREPARING)
	logger.info("[Bot] " .. player:getName() .. " starting hunt: " .. pick.script.name
		.. " in " .. pick.script.town_name
		.. " (duration=" .. math.floor(duration / 60) .. "m)")
	return true
end

-- Begin a hunt phase (travel_to, patrolling, leaving, resupplying)
-- Phase name lookup (hoisted out of tick loop)
local HUNT_PHASE_NAMES = {
	[HUNT_PHASE.PREPARING] = "PREPARING",
	[HUNT_PHASE.TRAVEL_TO] = "TRAVEL_TO",
	[HUNT_PHASE.PATROLLING] = "PATROLLING",
	[HUNT_PHASE.LEAVING] = "LEAVING",
	[HUNT_PHASE.RESUPPLYING] = "RESUPPLYING",
}

function BotSystem.beginHuntPhase(player, state, phase)
	BotSystem.castDebug(player, state, "Hunt phase: " .. (HUNT_PHASE_NAMES[phase] or tostring(phase)))

	state.huntPhase = phase
	state.huntWaypointIdx = 0
	state.huntWaypointSkipCount = 0
	state.walkTarget = nil
	state.currentPOI = nil
	state.state = BOT_STATE.HUNTING

	if phase == HUNT_PHASE.PREPARING then
		-- Pre-hunt preparation: visit depot box + shops before heading to spawn
		state.huntWaypoints = nil
		state.resupplyStep = "depot"
		state.resupplyUntil = 0
		state.resupplyWalkTarget = nil
		state.resupplyRoute = nil
		state.routeIdx = nil
		state.routeStuckCount = nil
		state.resupplyStartTime = nil
		state.resupplyDepotTarget = nil
		state.resupplyDepotWaitUntil = nil
		state.resupplyLastPOI = nil
		logger.info("[Bot] " .. player:getName() .. " preparing for hunt: depot + shops")
	elseif phase == HUNT_PHASE.TRAVEL_TO then
		state.huntWaypoints = BotHuntData.getPhaseWaypoints(state.huntScriptId, "travel_to")
		if not state.huntWaypoints or #state.huntWaypoints == 0 then
			-- No travel_to phase, go straight to patrol
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PATROLLING)
			return
		end
	elseif phase == HUNT_PHASE.PATROLLING then
		state.huntWaypoints = BotHuntData.getPhaseWaypoints(state.huntScriptId, "hunt_patrol")
		if not state.huntWaypoints or #state.huntWaypoints == 0 then
			-- No patrol waypoints, abort
			BotSystem.abortHunt(player, state, "no patrol waypoints")
			return
		end
		-- Check if enough patrol waypoints are on the bot's current z (>=15%)
		local botZ = player:getPosition().z
		local surfaceCount = 0
		local totalCount = #state.huntWaypoints
		for _, wp in ipairs(state.huntWaypoints) do
			if wp.pos_z and wp.pos_z == botZ then
				surfaceCount = surfaceCount + 1
			end
		end
		if totalCount == 0 or (surfaceCount / totalCount) < 0.15 then
			BotSystem.abortHunt(player, state, "patrol floor unreachable (bot z=" .. botZ
				.. ", surface=" .. surfaceCount .. "/" .. totalCount .. ")")
			return
		end
		-- Teleport to first patrol waypoint if too far away
		-- (travel_to phase often doesn't reach the spawn entrance)
		local firstWp = state.huntWaypoints[1]
		if firstWp and firstWp.pos_x and firstWp.pos_x > 0 then
			local pos = player:getPosition()
			local dist = math.max(math.abs(pos.x - firstWp.pos_x), math.abs(pos.y - firstWp.pos_y))
			if dist > 30 or pos.z ~= firstWp.pos_z then
				local tpPos = Position(firstWp.pos_x, firstWp.pos_y, firstWp.pos_z)
				player:teleportTo(tpPos)
				logger.info("[Bot] " .. player:getName() .. " teleported to patrol start ("
					.. tpPos.x .. "," .. tpPos.y .. "," .. tpPos.z .. ") dist=" .. dist)
			end
		end
	elseif phase == HUNT_PHASE.LEAVING then
		state.huntWaypoints = BotHuntData.getPhaseWaypoints(state.huntScriptId, "travel_from")
		if not state.huntWaypoints or #state.huntWaypoints == 0 then
			-- No travel_from, reverse travel_to
			local travelTo = BotHuntData.getPhaseWaypoints(state.huntScriptId, "travel_to")
			if travelTo and #travelTo > 0 then
				state.huntWaypoints = {}
				for i = #travelTo, 1, -1 do
					state.huntWaypoints[#state.huntWaypoints + 1] = travelTo[i]
				end
			else
				-- No travel waypoints at all, just end the hunt
				BotSystem.beginHuntPhase(player, state, HUNT_PHASE.RESUPPLYING)
				return
			end
		end
	elseif phase == HUNT_PHASE.RESUPPLYING then
		state.huntWaypoints = nil
		state.resupplyStep = "bank"
		state.resupplyUntil = 0
		state.resupplyWalkTarget = nil
		state.resupplyRoute = nil
		state.routeIdx = nil
		state.routeStuckCount = nil
		state.resupplyStartTime = nil
		state.resupplyDepotTarget = nil
		state.resupplyLastPOI = nil
		-- Teleport back to town if far from city
		local town = player:getTown()
		if town then
			local templePos = town:getTemplePosition()
			local pos = player:getPosition()
			local dist = math.max(math.abs(pos.x - templePos.x), math.abs(pos.y - templePos.y))
			local expectedZ = BotSystem.getExpectedZ(player)
			if dist > 30 or pos.z ~= expectedZ then
				local tpPos = Position(templePos.x, templePos.y, expectedZ)
				player:teleportTo(tpPos)
				logger.info("[Bot] " .. player:getName() .. " teleported to town for resupply ("
					.. tpPos.x .. "," .. tpPos.y .. "," .. tpPos.z .. ")")
			end
		end
	end
end

-- Main hunting dispatch
function BotSystem.doHunting(player, state)
	-- Guard: if we're in HUNTING state but have no hunt phase AND no cavebot command,
	-- fall back to IDLE. This catches bots left in HUNTING after cavebot commands complete.
	if not state.huntPhase and not state.cavebotCommand then
		state.state = BOT_STATE.IDLE
		state.verboseLog = state.verboseLogAutoEnabled or false
		return
	end

	-- Safety timeout
	if state.huntStartTime and os.time() - state.huntStartTime > BOT_CONFIG.HUNT_SAFETY_TIMEOUT then
		BotSystem.castDebug(player, state, "HUNT safety timeout after " .. BOT_CONFIG.HUNT_SAFETY_TIMEOUT .. "s")
		BotSystem.abortHunt(player, state, "safety timeout (" .. BOT_CONFIG.HUNT_SAFETY_TIMEOUT .. "s)")
		return
	end

	-- Heal during hunts (throttled: only check every 5 ticks ~500ms)
	state.healCheckCooldown = (state.healCheckCooldown or 0) - 1
	if state.healCheckCooldown <= 0 then
		state.healCheckCooldown = 5
		BotSystem.doHealing(player, state)
	end
	-- Restore mana (bots have infinite mana) — only check every 10 ticks
	state.manaCheckCooldown = (state.manaCheckCooldown or 0) - 1
	if state.manaCheckCooldown <= 0 then
		state.manaCheckCooldown = 10
		if player:getMana() < player:getMaxMana() * 0.5 then
			player:addMana(player:getMaxMana())
		end
	end

	if state.huntPhase == HUNT_PHASE.PREPARING then
		BotSystem.doHuntResupply(player, state)
	elseif state.huntPhase == HUNT_PHASE.TRAVEL_TO then
		BotSystem.doHuntTravel(player, state, HUNT_PHASE.PATROLLING)
	elseif state.huntPhase == HUNT_PHASE.PATROLLING then
		BotSystem.doHuntPatrol(player, state)
	elseif state.huntPhase == HUNT_PHASE.LEAVING then
		BotSystem.doHuntTravel(player, state, nil) -- nil = go to resupply after
	elseif state.huntPhase == HUNT_PHASE.RESUPPLYING then
		BotSystem.doHuntResupply(player, state)
	end
end

-- Follow waypoints for travel_to or leaving phase
function BotSystem.doHuntTravel(player, state, nextPhase)
	if player:isWalking() then return end

	local wps = state.huntWaypoints
	if not wps or #wps == 0 then
		BotSystem.castDebug(player, state, "TRAVEL: no waypoints, advancing phase")
		if nextPhase then
			BotSystem.beginHuntPhase(player, state, nextPhase)
		else
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.RESUPPLYING)
		end
		return
	end

	-- Advance waypoint index
	local advanced = BotSystem.advanceHuntWaypoint(player, state)
	if advanced == "done" then
		-- Reached end of waypoints
		BotSystem.castDebug(player, state, "TRAVEL: all " .. #wps .. " waypoints done, advancing phase")
		if nextPhase then
			BotSystem.beginHuntPhase(player, state, nextPhase)
		else
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.RESUPPLYING)
		end
		return
	end

	-- Navigate to current waypoint
	local wp = wps[state.huntWaypointIdx]
	if wp and wp.pos_x and wp.pos_x > 0 then
		local pos = player:getPosition()
		local dist = math.max(math.abs(pos.x - wp.pos_x), math.abs(pos.y - wp.pos_y))
		BotSystem.castDebug(player, state, "TRAVEL: wp " .. state.huntWaypointIdx .. "/" .. #wps
			.. " (" .. wp.pos_x .. "," .. wp.pos_y .. "," .. wp.pos_z .. ")"
			.. " dist=" .. dist .. " type=" .. (wp.waypoint_type or "walk"))
	end
	BotSystem.navigateToHuntWaypoint(player, state)
end

-- Hunt patrol: walk waypoints + scan for monsters + attack
function BotSystem.doHuntPatrol(player, state)
	-- Occasional chat during hunting
	if math.random(1, BOT_CONFIG.CHAT_CHANCE * 2) == 1 then
		BotSystem.sayRandom(player, state, "hunting")
	end

	-- Check if hunt time expired
	if state.huntEndTime and os.time() >= state.huntEndTime then
		local remaining = state.huntEndTime - os.time()
		BotSystem.castDebug(player, state, "PATROL: hunt time expired, kills=" .. (state.huntKillCount or 0)
			.. " cycles=" .. (state.huntCycles or 0) .. ", leaving spawn")
		BotSystem.beginHuntPhase(player, state, HUNT_PHASE.LEAVING)
		return
	end

	-- ALWAYS scan for monsters — even while walking to a waypoint.
	-- This lets bots interrupt movement to chase a target that appears mid-walk.
	if BotSystem.scanAndAttackHuntTarget(player, state) then
		return -- busy fighting or chasing
	end

	-- Check waypoint arrival BEFORE navigating (handles proximity-based advancement)
	local advanced = BotSystem.advanceHuntWaypoint(player, state)
	if advanced == "done" then
		-- Loop back to start of patrol
		state.huntWaypointIdx = 0
		state.huntWaypointSkipCount = 0
		state.huntCycles = (state.huntCycles or 0) + 1
		BotSystem.castDebug(player, state, "PATROL: cycle " .. state.huntCycles .. " complete, looping back"
			.. " kills=" .. (state.huntKillCount or 0))
	end

	if player:isWalking() then return end

	-- Navigate to next waypoint
	local wps = state.huntWaypoints
	local idx = state.huntWaypointIdx
	if wps and idx and idx >= 1 and idx <= #wps then
		local wp = wps[idx]
		if wp and wp.pos_x and wp.pos_x > 0 then
			local pos = player:getPosition()
			local dist = math.max(math.abs(pos.x - wp.pos_x), math.abs(pos.y - wp.pos_y))
			BotSystem.castDebug(player, state, "PATROL: navigating to wp " .. idx .. "/" .. #wps
				.. " (" .. wp.pos_x .. "," .. wp.pos_y .. "," .. wp.pos_z .. ")"
				.. " dist=" .. dist .. " type=" .. (wp.waypoint_type or "walk"))
		end
	end
	BotSystem.navigateToHuntWaypoint(player, state)
end

-- Check arrival and advance to next waypoint. Returns "done" if past last, true if advanced, false if not.
function BotSystem.advanceHuntWaypoint(player, state)
	local wps = state.huntWaypoints
	if not wps or #wps == 0 then return "done" end

	local pos = player:getPosition()
	local idx = state.huntWaypointIdx

	-- If we haven't started yet, go to first waypoint
	if idx == 0 then
		state.huntWaypointIdx = 1
		BotSystem.castDebug(player, state, "ADVANCE: starting at wp 1/" .. #wps)
		return true
	end

	-- Check if we've arrived at current waypoint
	if idx <= #wps then
		local wp = wps[idx]
		if wp and wp.pos_x and wp.pos_x > 0 then
			local dx = math.abs(pos.x - wp.pos_x)
			local dy = math.abs(pos.y - wp.pos_y)
			local arrivalDist = BOT_CONFIG.HUNT_NODE_ARRIVAL_DIST

			-- Stand waypoints need near-exact position in patrol, close enough during travel
			if wp.waypoint_type == "stand" then
				if state.huntPhase == HUNT_PHASE.PATROLLING then
					arrivalDist = 0
				else
					arrivalDist = 1  -- close enough during travel phases
				end
			end

			-- Floor-change tiles can't be stood on (A* rejects them), so relax arrival to dist=1
			if arrivalDist == 0 then
				local wpTile = Tile(Position(wp.pos_x, wp.pos_y, wp.pos_z))
				if wpTile and wpTile:hasFlag(TILESTATE_FLOORCHANGE) then
					arrivalDist = 1
				end
			end

			if dx <= arrivalDist and dy <= arrivalDist and pos.z == wp.pos_z then
				-- Arrived! Handle special waypoint actions
				BotSystem.handleHuntWaypointAction(player, state, wp)

				-- Reset skip counter on successful arrival
				state.huntWaypointSkipCount = 0
				state.huntLastPos = { x = pos.x, y = pos.y, z = pos.z }

				-- Advance to next
				state.huntWaypointIdx = idx + 1
				BotSystem.castDebug(player, state, "ADVANCE: arrived wp " .. idx .. "/" .. #wps
					.. " (" .. wp.pos_x .. "," .. wp.pos_y .. "," .. wp.pos_z .. ")"
					.. " type=" .. (wp.waypoint_type or "walk"))
				if state.huntWaypointIdx > #wps then
					BotSystem.castDebug(player, state, "ADVANCE: all waypoints done")
					return "done"
				end
				return true
			end
		else
			-- Label/conditional waypoint — skip to next
			BotSystem.castDebug(player, state, "ADVANCE: skipping label/cond wp " .. idx)
			state.huntWaypointIdx = idx + 1
			if state.huntWaypointIdx > #wps then
				return "done"
			end
			return true
		end
	else
		return "done"
	end

	return false
end

-- Handle special waypoint actions (door, rope, ladder, etc.)
function BotSystem.handleHuntWaypointAction(player, state, wp)
	local wtype = wp.waypoint_type

	if wtype == "door" then
		-- Try to open door at waypoint position
		local doorPos = Position(wp.pos_x, wp.pos_y, wp.pos_z)
		BotSystem.tryOpenDoors(player, doorPos)
	end

	-- Pretend delay at supply points
	if wp.extra_data == "pretend_delay" then
		-- Say something at this supply point
		if math.random(1, 3) == 1 then
			BotSystem.sayRandom(player, state, "shop")
		end
	end
end

-- Navigate to the current hunt waypoint
-- Z-transitions are delegated to the floor-change state machine (handleFloorChange)
-- which runs over multiple ticks with retry logic, diagonal fix, and verification.
function BotSystem.navigateToHuntWaypoint(player, state)
	local wps = state.huntWaypoints
	if not wps then return end

	local idx = state.huntWaypointIdx
	if idx < 1 or idx > #wps then return end

	local wp = wps[idx]
	if not wp or not wp.pos_x or wp.pos_x == 0 then
		-- Skip nil/labels/conditionals
		state.huntWaypointIdx = idx + 1
		return
	end

	if player:isWalking() then return end

	local pos = player:getPosition()
	local targetPos = Position(wp.pos_x, wp.pos_y, wp.pos_z)

	-- Z-transition needed?
	if pos.z ~= targetPos.z then
		-- If floor change state machine is already running, let it finish
		if state.floorChangeState ~= FLOOR_CHANGE_STATE.NONE then
			return
		end

		local goDown = targetPos.z > pos.z
		BotSystem.castDebug(player, state, "NAV: z-transition needed at wp " .. idx
			.. " (z=" .. pos.z .. " -> z=" .. targetPos.z .. ", " .. (goDown and "DOWN" or "UP") .. ")")

		-- Build extra search centers (hunt-specific strategies)
		local extraCenters = {}
		-- Strategy 1: if previous waypoint was a rope/ladder/hole type, use its position
		-- (the imported script tells us exactly where the z-transition is)
		if idx > 1 then
			local prevWp = wps[idx - 1]
			if prevWp and prevWp.pos_x and prevWp.pos_x > 0 and prevWp.pos_z == pos.z then
				if prevWp.waypoint_type == "rope" or prevWp.waypoint_type == "ladder"
					or prevWp.waypoint_type == "hole" then
					table.insert(extraCenters, Position(prevWp.pos_x, prevWp.pos_y, pos.z))
					BotSystem.castDebug(player, state, "NAV: using " .. prevWp.waypoint_type
						.. " wp hint at (" .. prevWp.pos_x .. "," .. prevWp.pos_y .. "," .. pos.z .. ")")
				end
				-- Strategy 2: midpoint between previous waypoint and target
				local midX = math.floor((prevWp.pos_x + targetPos.x) / 2)
				local midY = math.floor((prevWp.pos_y + targetPos.y) / 2)
				table.insert(extraCenters, Position(midX, midY, pos.z))
			end
		end

		BotSystem.startFloorChange(state, goDown, targetPos,
			#extraCenters > 0 and extraCenters or nil, "hunt")
		return
	end

	-- If the waypoint is on a floor-change tile, A* can't path TO it (rejects FLOORCHANGE).
	-- goTo() would succeed to within 3 tiles but the bot would oscillate, never arriving.
	-- Instead, path to within 1 tile and let advanceHuntWaypoint (arrivalDist=1 for FC tiles) advance.
	local targetTile = Tile(targetPos)
	if targetTile and targetTile:hasFlag(TILESTATE_FLOORCHANGE) then
		local dist = math.max(math.abs(pos.x - targetPos.x), math.abs(pos.y - targetPos.y))
		if dist <= 1 then
			-- Already adjacent — arrival check will handle it
			BotSystem.castDebug(player, state, "NAV: adjacent to floor-change tile wp " .. idx .. ", waiting for arrival check")
			state.huntWaypointSkipCount = 0
			return
		end
		BotSystem.castDebug(player, state, "NAV: pathing near floor-change tile wp " .. idx .. " dist=" .. dist)
		local dirs = player:getPathTo(targetPos, 1, 1, true, false, BOT_CONFIG.PATH_MAX_DIST)
		if dirs and type(dirs) == "table" and #dirs > 0 then
			player:startAutoWalk(dirs)
			state.huntWaypointSkipCount = 0
			return
		end
		-- Can't path near the stairs — try opening doors then skip
		BotSystem.castDebug(player, state, "NAV: floor-change tile unreachable at wp " .. idx .. ", trying doors then skipping")
		BotSystem.tryOpenDoors(player, targetPos)
		BotSystem.skipHuntWaypoint(player, state, "floor-change tile unreachable")
		return
	end

	-- Same z, normal tile: pathfind to within arrival distance of the waypoint.
	-- Use maxDist matching arrival check (not goTo's maxDist=3, which causes
	-- oscillation when bot reaches dist=3 but arrival needs dist<=2).
	local arrivalDist = BOT_CONFIG.HUNT_NODE_ARRIVAL_DIST
	local maxDist = math.max(arrivalDist - 1, 0) -- path to 1 tile inside arrival zone
	local dist = math.max(math.abs(pos.x - targetPos.x), math.abs(pos.y - targetPos.y))

	-- Only use tight pathfinding when close; for long distances use goTo()'s chunked approach
	local ok = false
	if dist <= BOT_CONFIG.PATH_MAX_DIST then
		local dirs = player:getPathTo(targetPos, 0, maxDist, true, false, BOT_CONFIG.PATH_MAX_DIST)
		if not dirs or type(dirs) ~= "table" or #dirs == 0 then
			-- Retry without clearSight restriction
			dirs = player:getPathTo(targetPos, 0, maxDist, true, true, BOT_CONFIG.PATH_MAX_DIST)
		end
		if dirs and type(dirs) == "table" and #dirs > 0 then
			player:startAutoWalk(dirs)
			ok = true
		end
	else
		-- Far away — use chunked goTo for long-range navigation
		ok = BotSystem.goTo(player, targetPos)
	end

	if not ok then
		-- Try to open doors first
		BotSystem.tryOpenDoors(player, targetPos)
		local dirs = player:getPathTo(targetPos, 0, maxDist, true, false, BOT_CONFIG.PATH_MAX_DIST)
		if dirs and type(dirs) == "table" and #dirs > 0 then
			player:startAutoWalk(dirs)
			ok = true
		end
		if not ok then
			-- Try to clear blocking monsters before skipping
			BotSystem.castDebug(player, state, "NAV: path failed to wp " .. idx
				.. " (" .. targetPos.x .. "," .. targetPos.y .. "," .. targetPos.z .. ")"
				.. ", trying to clear blocker")
			if not BotSystem.clearPathBlocker(player, state, targetPos) then
				-- Skip to next waypoint (with delay)
				BotSystem.castDebug(player, state, "NAV: no blocker found, skipping wp " .. idx)
				BotSystem.skipHuntWaypoint(player, state, "path failed")
			end
		end
	else
		-- Successful pathfind — reset skip counter
		state.huntWaypointSkipCount = 0
	end
end

-- Clear any monster blocking the path toward a target position
-- Returns true if a blocker was found and attacked
function BotSystem.clearPathBlocker(player, state, targetPos)
	local pos = player:getPosition()
	local dx = targetPos.x - pos.x
	local dy = targetPos.y - pos.y
	local ndx = dx > 0 and 1 or (dx < 0 and -1 or 0)
	local ndy = dy > 0 and 1 or (dy < 0 and -1 or 0)

	-- Build ordered list of positions to check:
	-- 1) Tiles in direction of travel (up to 3 deep)
	-- 2) All 8 adjacent tiles (for when surrounded by monsters)
	local checked = {}
	local checkList = {}

	-- Direction tiles first (highest priority)
	if ndx ~= 0 or ndy ~= 0 then
		for step = 1, 3 do
			local cx, cy = pos.x + ndx * step, pos.y + ndy * step
			local key = cx .. "," .. cy
			if not checked[key] then
				checked[key] = true
				checkList[#checkList + 1] = Position(cx, cy, pos.z)
			end
		end
	end

	-- All 8 adjacent tiles (catches blockers from any direction)
	for ox = -1, 1 do
		for oy = -1, 1 do
			if ox ~= 0 or oy ~= 0 then
				local cx, cy = pos.x + ox, pos.y + oy
				local key = cx .. "," .. cy
				if not checked[key] then
					checked[key] = true
					checkList[#checkList + 1] = Position(cx, cy, pos.z)
				end
			end
		end
	end

	for _, checkPos in ipairs(checkList) do
		local tile = Tile(checkPos)
		if tile then
			local creature = tile:getTopCreature()
			if creature and creature:isMonster() and creature:getHealth() > 0 then
				local cid = creature:getId()

				-- Check if we've been trying to clear this same blocker too long
				if state.blockerTarget == cid then
					state.blockerFailCount = (state.blockerFailCount or 0) + 1
					if state.blockerFailCount >= BOT_CONFIG.HUNT_BLOCKER_FAIL_LIMIT then
						BotSystem.combatLogForce(player, state, "HUNT-BLOCKER-GIVEUP: "
							.. creature:getName() .. " after " .. state.blockerFailCount .. " ticks")
						state.blockerTarget = nil
						state.blockerFailCount = 0
						return false -- give up, let caller skip waypoint
					end
				else
					state.blockerTarget = cid
					state.blockerFailCount = 1
				end

				-- Only attack if we have LOS
				if pos:isSightClear(checkPos, true) then
					BotSystem.doAttackCreature(player, state, creature)
					BotSystem.castDebug(player, state, "BLOCKER: attacking " .. creature:getName()
						.. " at (" .. checkPos.x .. "," .. checkPos.y .. "," .. checkPos.z .. ")"
						.. " attempt=" .. state.blockerFailCount)
					logger.info("[Bot:" .. player:getName() .. "] HUNTING clearing blocker "
						.. creature:getName() .. " at (" .. checkPos.x .. "," .. checkPos.y .. "," .. checkPos.z .. ")"
						.. " attempt=" .. state.blockerFailCount)
				else
					BotSystem.combatLog(player, state, "HUNT-BLOCKER-NOLOS: "
						.. creature:getName() .. " at (" .. checkPos.x .. "," .. checkPos.y .. "," .. checkPos.z .. ")")
				end
				return true
			end
		end
	end
	-- No blocker found — reset tracker
	state.blockerTarget = nil
	state.blockerFailCount = 0
	return false
end

-- Skip current waypoint and try the next one
function BotSystem.skipHuntWaypoint(player, state, reason)
	state.huntWaypointSkipCount = (state.huntWaypointSkipCount or 0) + 1
	BotSystem.castDebug(player, state, "SKIP: wp " .. (state.huntWaypointIdx or 0)
		.. " reason=" .. reason .. " skipCount=" .. state.huntWaypointSkipCount)

	-- Check if we've made real progress toward the target (not just oscillating)
	local pos = player:getPosition()
	local madeProgress = false
	if state.huntLastPos then
		-- Calculate distance to current waypoint target
		local wps = state.huntWaypoints
		local idx = state.huntWaypointIdx
		if wps and idx and idx >= 1 and idx <= #wps then
			local wp = wps[idx]
			if wp and wp.pos_x and wp.pos_x > 0 then
				local oldDist = math.max(math.abs(state.huntLastPos.x - wp.pos_x),
					math.abs(state.huntLastPos.y - wp.pos_y))
				local newDist = math.max(math.abs(pos.x - wp.pos_x),
					math.abs(pos.y - wp.pos_y))
				-- Only count as progress if we got at least 3 tiles closer
				madeProgress = (newDist < oldDist - 2)
				if madeProgress then
					BotSystem.castDebug(player, state, "SKIP: progress detected (oldDist=" .. oldDist .. " newDist=" .. newDist .. "), resetting skip count")
				end
			end
		end
	else
		madeProgress = true -- first time, give benefit of doubt
	end

	if madeProgress then
		state.huntWaypointSkipCount = 0
	end
	state.huntLastPos = { x = pos.x, y = pos.y, z = pos.z }

	if state.huntWaypointSkipCount >= BOT_CONFIG.HUNT_STUCK_THRESHOLD then
		BotSystem.castDebug(player, state, "SKIP: stuck threshold reached (" .. BOT_CONFIG.HUNT_STUCK_THRESHOLD .. "), aborting hunt")
		BotSystem.abortHunt(player, state, "stuck at waypoint " .. (state.huntWaypointIdx or 0)
			.. " (" .. reason .. ")")
		return
	end

	-- Skip to next waypoint
	local wps = state.huntWaypoints
	if wps and state.huntWaypointIdx < #wps then
		if reason == "z-transition failed" or reason == "z-mismatch" then
			-- Z-transition failed: skip immediately to next waypoint on our z-level.
			local startIdx = state.huntWaypointIdx
			while state.huntWaypointIdx < #wps do
				state.huntWaypointIdx = state.huntWaypointIdx + 1
				local nextWp = wps[state.huntWaypointIdx]
				if nextWp and nextWp.pos_z and nextWp.pos_z == pos.z then
					break
				end
			end
			local skipped = state.huntWaypointIdx - startIdx
			BotSystem.castDebug(player, state, "SKIP: z-skip " .. skipped .. " wps, now at wp "
				.. state.huntWaypointIdx .. " (z=" .. pos.z .. ")")
		else
			state.huntWaypointIdx = state.huntWaypointIdx + 1
			BotSystem.castDebug(player, state, "SKIP: advancing to wp " .. state.huntWaypointIdx .. "/" .. #wps)
		end
	else
		-- Past end of waypoints
		if state.huntPhase == HUNT_PHASE.PATROLLING then
			-- Loop back, reset skip counter per cycle
			state.huntWaypointIdx = 1
			state.huntCycles = (state.huntCycles or 0) + 1
			state.huntWaypointSkipCount = 0
			BotSystem.castDebug(player, state, "SKIP: end of patrol, looping (cycle " .. state.huntCycles
				.. " kills=" .. (state.huntKillCount or 0) .. ")")
			-- Abort if 3+ cycles with zero kills (patrol area unreachable or empty)
			if state.huntCycles >= 3 and (state.huntKillCount or 0) == 0 then
				BotSystem.castDebug(player, state, "SKIP: no kills after " .. state.huntCycles .. " cycles, aborting")
				BotSystem.abortHunt(player, state, "no kills after " .. state.huntCycles .. " patrol cycles")
				return
			end
		else
			-- For travel phases, consider it done
			BotSystem.castDebug(player, state, "SKIP: end of travel waypoints, advancing phase")
			if state.huntPhase == HUNT_PHASE.TRAVEL_TO then
				BotSystem.beginHuntPhase(player, state, HUNT_PHASE.PATROLLING)
			else
				BotSystem.beginHuntPhase(player, state, HUNT_PHASE.RESUPPLYING)
			end
		end
	end
end

-- Debug log helper: throttled to once per DEBUG_COMBAT_INTERVAL seconds per bot
function BotSystem.combatLog(player, state, msg)
	if not BOT_CONFIG.DEBUG_COMBAT then return end
	local now = os.time()
	if now - (state.lastCombatLogTime or 0) < BOT_CONFIG.DEBUG_COMBAT_INTERVAL then return end
	state.lastCombatLogTime = now
	if state and state.verboseLog and player then
		player:sendChannelMessage("", "[Bot] " .. msg, CAST_TALK_TYPE, CHANNEL_CAST)
	end
	logger.info("[Bot:" .. player:getName() .. "] " .. msg)
end

-- Force a combat log (bypasses throttle, for important one-time events)
function BotSystem.combatLogForce(player, state, msg)
	if not BOT_CONFIG.DEBUG_COMBAT then return end
	state.lastCombatLogTime = os.time()
	if state and state.verboseLog and player then
		player:sendChannelMessage("", "[Bot] " .. msg, CAST_TALK_TYPE, CHANNEL_CAST)
	end
	logger.info("[Bot:" .. player:getName() .. "] " .. msg)
end

-- Check if a monster is in the temporary ignore list
function BotSystem.isMonsterIgnored(state, creatureId)
	if not state.huntIgnoredMonsters then return false end
	local until_time = state.huntIgnoredMonsters[creatureId]
	if not until_time then return false end
	if os.time() >= until_time then
		state.huntIgnoredMonsters[creatureId] = nil
		return false
	end
	return true
end

-- Find a cardinal-aligned attack position for ranged vocations (mages/paladins).
-- Prefers tiles on the same axis (N/S/E/W) as the monster for clear LOS.
function BotSystem.findCardinalAttackPosition(player, monsterPos, range)
	local pos = player:getPosition()
	-- Try cardinal offsets at increasing distances (1..range), sorted by distance from player
	local candidates = {}
	for d = 1, range do
		local offsets = {
			Position(monsterPos.x, monsterPos.y - d, monsterPos.z), -- north
			Position(monsterPos.x, monsterPos.y + d, monsterPos.z), -- south
			Position(monsterPos.x - d, monsterPos.y, monsterPos.z), -- west
			Position(monsterPos.x + d, monsterPos.y, monsterPos.z), -- east
		}
		for _, tilePos in ipairs(offsets) do
			local tile = Tile(tilePos)
			-- Check tile exists, has ground, and no solid blocking
			if tile and tile:getGround() and not tile:hasFlag(TILESTATE_BLOCKSOLID) then
				local pdist = math.abs(pos.x - tilePos.x) + math.abs(pos.y - tilePos.y)
				candidates[#candidates + 1] = {pos = tilePos, dist = pdist}
			end
		end
	end
	-- Sort by distance from player (closest first)
	table.sort(candidates, function(a, b) return a.dist < b.dist end)
	-- Try pathfinding to each candidate
	for _, c in ipairs(candidates) do
		local dirs = player:getPathTo(c.pos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
		if dirs and type(dirs) == "table" and #dirs > 0 then
			return dirs
		end
	end
	return nil
end

-- Scan for monsters and attack highest-priority target
-- Uses native targeting: setAttackedCreature (red box) -> server auto-attacks with weapon.
-- Chase mode is enabled in C++, so server handles follow/pathfinding for melee (knights).
-- Mages use manual cardinal positioning to maintain range and LOS.
function BotSystem.scanAndAttackHuntTarget(player, state)
	local pos = player:getPosition()
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4
	local name = player:getName()
	-- Vocation-aware attack range: mages keep distance, knights go melee
	local attackRange = 1
	if baseVoc == 1 or baseVoc == 2 then attackRange = 3
	elseif baseVoc == 3 then attackRange = 5 end

	-- Check if already fighting a monster
	if state.huntMonsterTarget then
		local monster = Creature(state.huntMonsterTarget)
		if monster and not monster:isRemoved() and monster:getHealth() > 0 then
			local mpos = monster:getPosition()
			local dist = math.max(math.abs(pos.x - mpos.x), math.abs(pos.y - mpos.y))
			if dist <= BOT_CONFIG.HUNT_MONSTER_SCAN_RADIUS + 5 then
				-- Ensure target is set (server handles auto-attack with weapon)
				if player:getTarget() ~= monster then
					player:setTarget(monster)
				end
				local hasLOS = pos.z == mpos.z and pos:isSightClear(mpos, true)

				-- Periodically cast spells (spell system uses attackedCreature for targeting)
				BotSystem.doCastSpell(player, state, monster)

				-- Persistent LOS failure = give up on this target too
				-- (bot may be in range but separated by wall)
				if not hasLOS then
					state.huntLOSFailCount = (state.huntLOSFailCount or 0) + 1
					if state.huntLOSFailCount >= BOT_CONFIG.HUNT_CHASE_FAIL_LIMIT then
						state.huntChaseFailCount = BOT_CONFIG.HUNT_CHASE_FAIL_LIMIT
					end
				else
					state.huntLOSFailCount = 0
				end

				-- Chase logic depends on vocation
				if pos.z ~= mpos.z then
					BotSystem.huntChaseMonster(player, state, monster)
					state.huntChaseFailCount = 0
				elseif dist <= attackRange and hasLOS then
					-- In range with LOS — weapon auto-attacks + spells handle damage
					state.huntChaseFailCount = 0
					-- Mages: disable chase mode to hold position at range
					if (baseVoc == 1 or baseVoc == 2) and dist >= 2 then
						player:setChaseMode(false)
					end
				elseif not player:isWalking() then
					-- Need to close distance or fix LOS — manual pathfinding for ALL vocations
					-- (setChaseMode/setFollowCreature is unreliable for bot players)
					local dirs = nil
					if (baseVoc == 1 or baseVoc == 2) then
						-- Mages: try cardinal positioning first for ranged LOS
						player:setChaseMode(false)
						dirs = BotSystem.findCardinalAttackPosition(player, mpos, attackRange)
					end
					if not dirs then
						-- All vocations: pathfind toward monster
						dirs = player:getPathTo(mpos, 0, attackRange, true, true, BOT_CONFIG.PATH_MAX_DIST)
						if not dirs or type(dirs) ~= "table" or #dirs == 0 then
							dirs = player:getPathTo(mpos, 0, attackRange, true, false, BOT_CONFIG.PATH_MAX_DIST)
						end
					end
					if dirs and type(dirs) == "table" and #dirs > 0 then
						player:startAutoWalk(dirs)
						state.huntChaseFailCount = 0
					else
						state.huntChaseFailCount = (state.huntChaseFailCount or 0) + 1
					end

					-- Unreachable target timeout
					if (state.huntChaseFailCount or 0) >= BOT_CONFIG.HUNT_CHASE_FAIL_LIMIT then
						local mid = state.huntMonsterTarget
						BotSystem.combatLogForce(player, state, "HUNT-TARGET-TIMEOUT: giving up on "
							.. monster:getName() .. " after " .. state.huntChaseFailCount .. " fails"
							.. " dist=" .. dist .. " LOS=" .. tostring(hasLOS))
						player:setTarget(nil)
						state.huntMonsterTarget = nil
						state.huntChaseFailCount = 0
						-- Add to temporary ignore list
						if not state.huntIgnoredMonsters then state.huntIgnoredMonsters = {} end
						state.huntIgnoredMonsters[mid] = os.time() + BOT_CONFIG.HUNT_IGNORE_DURATION
						return true -- stay in combat mode, try rescanning
					end
				end

				BotSystem.combatLog(player, state, "HUNT-COMBAT: " .. monster:getName()
					.. " dist=" .. dist .. " LOS=" .. tostring(hasLOS)
					.. " walking=" .. tostring(player:isWalking())
					.. " chaseFails=" .. (state.huntChaseFailCount or 0))
				return true
			end
		end
		-- Monster dead or gone — clear target, count kill, resume patrol
		BotSystem.combatLogForce(player, state, "HUNT-TARGET-CLEARED: monster dead/gone"
			.. " kills=" .. ((state.huntKillCount or 0) + 1))
		player:setTarget(nil)
		state.huntMonsterTarget = nil
		state.huntChaseFailCount = 0
		state.huntKillCount = (state.huntKillCount or 0) + 1
		-- Re-enable chase mode for next target
		player:setChaseMode(true)
		BotSystem.findClosestForwardWaypoint(player, state)
		state.huntScanCooldown = 0 -- immediately re-scan after kill
	end

	-- Scan for monsters (throttled: only every 5 ticks (~500ms) when no target locked)
	-- Returns false during cooldown so navigation can proceed; cooldown resets to 0
	-- after a kill (see above) so we always re-scan immediately after killing a monster
	state.huntScanCooldown = (state.huntScanCooldown or 0) - 1
	if state.huntScanCooldown > 0 then return false end
	state.huntScanCooldown = 5

	local targets = state.huntTargets
	if not targets or #targets == 0 then return false end

	local radius = BOT_CONFIG.HUNT_MONSTER_SCAN_RADIUS
	local spectators = Game.getSpectators(pos, false, false, radius, radius, radius, radius)
	if not spectators or #spectators == 0 then return false end

	-- Build target name lookup (handle comma-separated names like "Rotworm, Carrion Worm")
	local targetNames = {}
	for _, t in ipairs(targets) do
		local tname = t.monster_name
		if tname:find(",") then
			for part in tname:gmatch("[^,]+") do
				local trimmed = part:match("^%s*(.-)%s*$")
				if trimmed and #trimmed > 0 then
					targetNames[trimmed:lower()] = t
				end
			end
		else
			targetNames[tname:lower()] = t
		end
	end

	-- Collect ALL matching monsters (sorted by priority then distance)
	local candidates = {}

	-- Debug: periodic scan logging (every 60 ticks = ~60s)
	state.huntScanTick = (state.huntScanTick or 0) + 1
	local debugScan = (state.huntScanTick % 60 == 0)
	local debugMonsters = {}
	local debugMatchFails = {}

	for _, creature in ipairs(spectators) do
		if creature ~= player and not creature:isRemoved() and creature:getHealth() > 0 then
			local cid = creature:getId()
			if not Player(cid) and not BotSystem.isMonsterIgnored(state, cid) then
				local cname = creature:getName():lower()
				local targetInfo = targetNames[cname]
				if targetInfo then
					local cpos = creature:getPosition()
					if cpos.z == pos.z then
						local dist = math.max(math.abs(pos.x - cpos.x), math.abs(pos.y - cpos.y))
						candidates[#candidates + 1] = {
							creature = creature,
							priority = targetInfo.priority,
							dist = dist,
							pos = cpos,
						}
					elseif debugScan then
						debugMatchFails[#debugMatchFails + 1] = cname .. "(z=" .. cpos.z .. ")"
					end
				elseif debugScan then
					debugMonsters[cname] = (debugMonsters[cname] or 0) + 1
				end
			end
		end
	end

	-- Sort candidates: highest priority first, then closest
	table.sort(candidates, function(a, b)
		if a.priority ~= b.priority then return a.priority > b.priority end
		return a.dist < b.dist
	end)

	-- Debug log: what creatures are nearby that DON'T match targets
	if debugScan and #candidates == 0 then
		local nearbyStr = ""
		for cname, count in pairs(debugMonsters) do
			if nearbyStr ~= "" then nearbyStr = nearbyStr .. ", " end
			nearbyStr = nearbyStr .. cname .. "x" .. count
		end
		local failStr = ""
		for _, f in ipairs(debugMatchFails) do
			if failStr ~= "" then failStr = failStr .. ", " end
			failStr = failStr .. f
		end
		local targetStr = ""
		local tCount = 0
		for tname, _ in pairs(targetNames) do
			tCount = tCount + 1
			if tCount <= 3 then
				if targetStr ~= "" then targetStr = targetStr .. "," end
				targetStr = targetStr .. tname
			end
		end
		if tCount > 3 then targetStr = targetStr .. ",+" .. (tCount - 3) end
		logger.info("[Bot] " .. name .. " hunt-scan at ("
			.. pos.x .. "," .. pos.y .. "," .. pos.z .. ") spectators=" .. #spectators
			.. " nearby=[" .. nearbyStr .. "]"
			.. (failStr ~= "" and (" matchfail=[" .. failStr .. "]") or "")
			.. " seeking=[" .. targetStr .. "]"
			.. " kills=" .. (state.huntKillCount or 0)
			.. " cycle=" .. (state.huntCycles or 0)
			.. " wp=" .. (state.huntWaypointIdx or 0) .. "/" .. #(state.huntWaypoints or {}))
	end

	-- Try each candidate: validate reachability before committing
	for _, cand in ipairs(candidates) do
		local monster = cand.creature
		local mpos = cand.pos
		local dist = cand.dist
		local hasLOS = pos:isSightClear(mpos, true)

		-- Accept if: already in range with LOS, or pathfinding succeeds
		local reachable = false
		if dist <= attackRange and hasLOS then
			reachable = true
		elseif dist <= attackRange and not hasLOS then
			-- In range but no LOS — check if we can reposition
			if baseVoc == 1 or baseVoc == 2 then
				reachable = (BotSystem.findCardinalAttackPosition(player, mpos, attackRange) ~= nil)
			else
				-- Melee: chase mode will handle it
				reachable = true
			end
		else
			-- Out of range — for melee, chase mode handles it; for ranged, check path
			if baseVoc == 3 or baseVoc == 4 then
				reachable = true -- chase mode will get us there
			else
				local dirs = player:getPathTo(mpos, 0, attackRange, true, true, BOT_CONFIG.PATH_MAX_DIST)
				reachable = (dirs and type(dirs) == "table" and #dirs > 0)
			end
		end

		if reachable then
			-- Commit to this target
			state.huntMonsterTarget = monster:getId()
			state.huntChaseFailCount = 0
			player:setTarget(monster)
			BotSystem.doCastSpell(player, state, monster)

			BotSystem.combatLogForce(player, state, "HUNT-TARGET: selected " .. monster:getName()
				.. " id=" .. monster:getId() .. " dist=" .. dist
				.. " LOS=" .. tostring(hasLOS) .. " voc=" .. baseVoc
				.. " at (" .. mpos.x .. "," .. mpos.y .. "," .. mpos.z .. ")")

			-- Start chase if needed (engine handles melee via setFollowCreature)
			if dist > attackRange and not player:isWalking() then
				if baseVoc == 1 or baseVoc == 2 then
					player:setChaseMode(false)
					local dirs = BotSystem.findCardinalAttackPosition(player, mpos, attackRange)
					if dirs then
						player:startAutoWalk(dirs)
					end
				else
					player:setChaseMode(true)
				end
			end
			return true
		end
	end

	return false
end

-- Chase a monster across z-levels during hunting (mirrors chaseTarget z-pursuit)
function BotSystem.huntChaseMonster(player, state, monster)
	local pos = player:getPosition()
	local tpos = monster:getPosition()
	if pos.z == tpos.z then return end

	local goDown = tpos.z > pos.z

	-- Collect transitions from multiple search centers
	local allTransitions = {}
	local seen = {}
	local function addFrom(center)
		local transitions = BotSystem.findZTransitions(center, 10, goDown)
		for _, entry in ipairs(transitions) do
			local key = entry.pos.x .. "," .. entry.pos.y .. "," .. entry.type
			if not seen[key] then
				seen[key] = true
				table.insert(allTransitions, entry)
			end
		end
	end
	addFrom(Position(pos.x, pos.y, pos.z))
	addFrom(Position(tpos.x, tpos.y, pos.z))

	-- Sort by distance from bot
	table.sort(allTransitions, function(a, b)
		local da = math.abs(pos.x - a.pos.x) + math.abs(pos.y - a.pos.y)
		local db = math.abs(pos.x - b.pos.x) + math.abs(pos.y - b.pos.y)
		return da < db
	end)

	if #allTransitions > 0 and not player:isWalking() then
		for _, entry in ipairs(allTransitions) do
			local fc = entry.pos
			local dist = math.max(math.abs(pos.x - fc.x), math.abs(pos.y - fc.y))

			if entry.type == "ladder" or entry.type == "sewer" then
				if dist == 0 then
					local destPos = Position(fc.x, fc.y, fc.z)
					if entry.type == "ladder" then
						destPos.z = destPos.z - 1 -- ladder goes UP (z decreases)
						destPos.y = destPos.y - 1 -- offset north to land next to hole
					else
						destPos.z = destPos.z + 1
					end
					player:teleportTo(destPos, true)
					return
				end
				local dirs = player:getPathTo(fc, 0, 0, true, false, BOT_CONFIG.PATH_MAX_DIST)
				if dirs and type(dirs) == "table" and #dirs > 0 then
					player:startAutoWalk(dirs)
					return
				end
			else
				if dist == 0 then return end -- on stairs, waiting for transition
				if dist == 1 then
					local stepDir = Position.getDirectionTo(pos, fc)
					if stepDir then
						player:startAutoWalk({stepDir})
						return
					end
				end
				local dirs = player:getPathTo(fc, 1, 1, true, false, BOT_CONFIG.PATH_MAX_DIST)
				if dirs and type(dirs) == "table" and #dirs > 0 then
					player:startAutoWalk(dirs)
					return
				end
			end
		end
	end
end

-- After killing a hunt target, find the closest forward waypoint to resume patrol.
-- Never goes backward — if we passed waypoints during chase, skip them.
function BotSystem.findClosestForwardWaypoint(player, state)
	local wps = state.huntWaypoints
	if not wps or #wps == 0 then return end

	local pos = player:getPosition()
	local currentIdx = state.huntWaypointIdx or 1
	local bestIdx = currentIdx
	local bestDist = 9999

	-- Search from current index forward through all remaining waypoints
	for i = currentIdx, #wps do
		local wp = wps[i]
		if wp and wp.pos_x and wp.pos_x > 0 and wp.pos_z == pos.z then
			local d = math.max(math.abs(pos.x - wp.pos_x), math.abs(pos.y - wp.pos_y))
			if d < bestDist then
				bestDist = d
				bestIdx = i
			end
		end
	end

	-- If the closest waypoint is very near (we're already past it), advance to next
	if bestIdx >= currentIdx then
		local wp = wps[bestIdx]
		if wp and wp.pos_x and wp.pos_x > 0 then
			local d = math.max(math.abs(pos.x - wp.pos_x), math.abs(pos.y - wp.pos_y))
			if d <= 2 and bestIdx < #wps then
				bestIdx = bestIdx + 1
			end
		end
	end

	if bestIdx ~= currentIdx then
		BotSystem.castDebug(player, state, "RESUME: after kill, wp " .. currentIdx .. " -> " .. bestIdx
			.. " (nearest forward at dist=" .. bestDist .. ")")
	else
		BotSystem.castDebug(player, state, "RESUME: staying at wp " .. currentIdx .. " (dist=" .. bestDist .. ")")
	end
	state.huntWaypointIdx = math.min(bestIdx, #wps)
end

-- Follow a city route waypoint by waypoint using state.routeIdx
-- Returns: "arrived" when at final waypoint, "walking" while in transit, "failed" if stuck
-- Uses a loop to advance through multiple arrived waypoints per tick (no 1s pauses)
function BotSystem.followCityRoute(player, state, routeWps)
	if not routeWps or #routeWps == 0 then return "failed" end

	if not state.routeIdx then state.routeIdx = 1 end
	if state.routeIdx > #routeWps then return "arrived" end

	-- If floor-change state machine is running, let it finish
	if state.floorChangeState ~= FLOOR_CHANGE_STATE.NONE then
		return "walking"
	end

	-- Check if a floor change just completed/failed — then continue processing below
	if state.floorChangeResult and state.floorChangeSource == "route" then
		local fcResult = state.floorChangeResult
		state.floorChangeResult = nil
		state.floorChangeSource = nil
		if fcResult == "completed" then
			BotSystem.castDebug(player, state, "Route wp " .. state.routeIdx .. "/" .. #routeWps
				.. ": z-transition OK, now at z=" .. player:getPosition().z)
			-- Fall through to waypoint loop below (no return "walking")
		else
			BotSystem.castDebug(player, state, "Route wp " .. state.routeIdx .. "/" .. #routeWps
				.. ": z-transition FAILED, skipping waypoint")
			state.routeIdx = state.routeIdx + 1
			state.routeStuckCount = 0
			if state.routeIdx > #routeWps then return "arrived" end
			-- Fall through to process next waypoint immediately
		end
	end

	-- Loop: advance through all consecutive waypoints we've already reached
	-- This eliminates the 1-second-per-waypoint delay from single recursion
	local advancedCount = 0
	while state.routeIdx <= #routeWps and advancedCount < 20 do
		local wp = routeWps[state.routeIdx]
		local targetPos = Position(wp.pos_x, wp.pos_y, wp.pos_z)
		local pos = player:getPosition()
		local dist = math.max(math.abs(pos.x - targetPos.x), math.abs(pos.y - targetPos.y))

		-- Check if we've arrived at this waypoint
		if dist <= 2 and pos.z == targetPos.z then
			advancedCount = advancedCount + 1
			BotSystem.castDebug(player, state, "Route wp " .. state.routeIdx .. "/" .. #routeWps
				.. ": arrived at (" .. targetPos.x .. "," .. targetPos.y .. "," .. targetPos.z .. ")")

			-- Handle special waypoint types
			if wp.waypoint_type == "door" then
				BotSystem.tryOpenDoors(player, targetPos)
			end

			state.routeIdx = state.routeIdx + 1
			state.routeStuckCount = 0
			if state.routeIdx > #routeWps then
				BotSystem.castDebug(player, state, "Route: ARRIVED at destination")
				return "arrived"
			end
			-- Continue loop to check next waypoint immediately
		else
			-- Not arrived — break out and handle walk/z-transition below
			break
		end
	end

	-- If we advanced but there's more, process current target
	if state.routeIdx > #routeWps then return "arrived" end

	local wp = routeWps[state.routeIdx]
	local targetPos = Position(wp.pos_x, wp.pos_y, wp.pos_z)
	local pos = player:getPosition()

	-- Z-mismatch: use floor-change state machine (NOT teleport!)
	if pos.z ~= targetPos.z then
		local goDown = targetPos.z > pos.z
		BotSystem.castDebug(player, state, "Route wp " .. state.routeIdx .. "/" .. #routeWps
			.. ": z-transition " .. (goDown and "DOWN" or "UP") .. " (z=" .. pos.z .. "->" .. targetPos.z .. ")")
		BotSystem.startFloorChange(state, goDown, targetPos, nil, "route")
		return "walking"
	end

	-- Same z: walk to waypoint (only issue new walk if not already walking)
	if not player:isWalking() then
		-- For floor-change tile waypoints, path to 1 tile away (A* rejects FLOORCHANGE tiles)
		local targetTile = Tile(targetPos)
		local isFcTile = targetTile and targetTile:hasFlag(TILESTATE_FLOORCHANGE)
		local minDist, maxDist = 0, 2
		if isFcTile then minDist, maxDist = 1, 1 end

		local dirs = player:getPathTo(targetPos, minDist, maxDist, true, true, BOT_CONFIG.PATH_MAX_DIST)
		if dirs and type(dirs) == "table" and #dirs > 0 then
			player:startAutoWalk(dirs)
			state.routeStuckSince = nil
			state.routeStuckLogged = nil
		else
			-- Time-based stuck detection (works correctly with both fast and normal ticks)
			local now = os.time()
			if not state.routeStuckSince then
				state.routeStuckSince = now
			end
			local stuckSeconds = now - state.routeStuckSince
			-- Try to open doors in the direction of the target
			BotSystem.tryOpenDoors(player, targetPos)
			-- Try to clear path blockers (monsters)
			BotSystem.clearPathBlocker(player, state, targetPos)

			if stuckSeconds >= 5 and not state.routeStuckLogged then
				state.routeStuckLogged = true
				BotSystem.castDebug(player, state, "Route wp " .. state.routeIdx .. "/" .. #routeWps
					.. ": stuck at (" .. pos.x .. "," .. pos.y .. "," .. pos.z
					.. ") trying to reach (" .. targetPos.x .. "," .. targetPos.y .. "," .. targetPos.z .. ")")
			end

			-- After 15 seconds stuck, skip to next waypoint
			if stuckSeconds > 15 then
				BotSystem.castDebug(player, state, "Route wp " .. state.routeIdx .. "/" .. #routeWps
					.. ": stuck too long, skipping")
				state.routeIdx = state.routeIdx + 1
				state.routeStuckSince = nil
				state.routeStuckLogged = nil
				if state.routeIdx > #routeWps then return "arrived" end
			end
		end
	end

	return "walking"
end

function BotSystem.doHuntResupply(player, state)
	-- Resupply timeout: 5 minutes max total for entire resupply/prepare phase
	if not state.resupplyStartTime then
		state.resupplyStartTime = os.time()
	end
	if os.time() - state.resupplyStartTime > 300 then
		BotSystem.castDebug(player, state, "RESUPPLY: timeout (300s), phase=" .. (state.huntPhase or "?"))
		if state.huntPhase == HUNT_PHASE.PREPARING then
			-- Timeout during prep — skip to travel_to anyway
			logger.info("[Bot] " .. player:getName() .. " prepare timeout, heading to spawn")
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.TRAVEL_TO)
		else
			BotSystem.endHunt(player, state)
		end
		return
	end

	local step = state.resupplyStep

	-- Sub-state: walking to depot locker (after route arrival at depot)
	if state.resupplyDepotTarget then
		local pos = player:getPosition()
		local dp = state.resupplyDepotTarget
		local dist = math.max(math.abs(pos.x - dp.x), math.abs(pos.y - dp.y))
		if dist <= 1 and pos.z == dp.z then
			-- Adjacent to depot locker — start the delay
			if math.random(1, 2) == 1 then
				BotSystem.sayRandom(player, state, "depot")
			end
			state.resupplyDepotTarget = nil
			state.resupplyUntil = os.time() + math.random(
				BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MIN, BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MAX)
			local waitSec = state.resupplyUntil - os.time()
			BotSystem.castDebug(player, state, "RESUPPLY: at depot locker (" .. dp.x .. "," .. dp.y .. "), waiting " .. waitSec .. "s")
			return
		end
		-- Not adjacent yet — walk toward it
		if not player:isWalking() then
			local foundPath = false
			local adjacentOffsets = {{0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1}}
			for _, off in ipairs(adjacentOffsets) do
				local adjPos = Position(dp.x + off[1], dp.y + off[2], dp.z)
				local adjTile = Tile(adjPos)
				if adjTile and adjTile:isWalkable() then
					local dirs = player:getPathTo(adjPos, 0, 0, true, true, BOT_CONFIG.PATH_MAX_DIST)
					if dirs and type(dirs) == "table" and #dirs > 0 then
						player:startAutoWalk(dirs)
						foundPath = true
						break
					end
				end
			end
			if not foundPath then
				local newLocker = BotSystem.findReachableDepotLocker(player, state)
				if newLocker and (newLocker.x ~= dp.x or newLocker.y ~= dp.y) then
					state.resupplyDepotTarget = newLocker
				else
					-- No reachable locker — just wait here
					state.resupplyDepotTarget = nil
					state.resupplyUntil = os.time() + math.random(
						BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MIN, BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MAX)
				end
			end
		end
		return
	end

	-- Check if current step delay is still running
	if state.resupplyUntil > 0 and os.time() < state.resupplyUntil then
		-- Show wait timer every ~5 seconds
		local remaining = state.resupplyUntil - os.time()
		if not state.lastResupplyWaitLog or os.time() - state.lastResupplyWaitLog >= 5 then
			state.lastResupplyWaitLog = os.time()
			BotSystem.castDebug(player, state, "RESUPPLY: waiting at " .. (step or "?")
				.. " (" .. remaining .. "s remaining)")
		end
		return -- still waiting
	end

	local townId = state.huntTownId or (player:getTown() and player:getTown():getId() or 8)

	-- Helper: find route from bot's current position to a destination POI
	local function findRouteToDestination(destPOI)
		local pos = player:getPosition()
		local srcPOI = state.resupplyLastPOI
		if not srcPOI then
			srcPOI = BotHuntData.detectNearestPOI(townId, pos)
		end
		if not srcPOI then return nil end
		local route = BotHuntData.findRoute(townId, srcPOI, destPOI)
		return route
	end

	if step == "bank" then
		-- Bank is only visited during post-hunt RESUPPLYING, not during PREPARING
		if state.huntPhase == HUNT_PHASE.PREPARING then
			BotSystem.castDebug(player, state, "RESUPPLY: skipping bank (PREPARING phase), -> depot")
			state.resupplyStep = "depot"
			state.resupplyUntil = 0
			return
		end
		if state.resupplyUntil == 0 then
			if not state.resupplyRoute then
				BotSystem.castDebug(player, state, "RESUPPLY: finding route to bank")
				state.resupplyRoute = findRouteToDestination("bank")
				state.routeIdx = nil
			end
			if state.resupplyRoute then
				local result = BotSystem.followCityRoute(player, state, state.resupplyRoute)
				if result == "arrived" then
					state.resupplyLastPOI = "bank"
					if math.random(1, 2) == 1 then
						player:say("hi", TALKTYPE_SAY)
					end
					state.resupplyUntil = os.time() + math.random(
						BOT_CONFIG.HUNT_RESUPPLY_BANK_MIN, BOT_CONFIG.HUNT_RESUPPLY_BANK_MAX)
					BotSystem.castDebug(player, state, "RESUPPLY: arrived at bank, waiting "
						.. (state.resupplyUntil - os.time()) .. "s")
				elseif result == "failed" then
					state.resupplyStep = "depot"
					state.resupplyUntil = 0
					state.resupplyRoute = nil
					state.routeIdx = nil
				end
				return
			end
			-- No bank route, skip to depot
			state.resupplyStep = "depot"
			state.resupplyUntil = 0
			return
		end
		-- Bank delay done, move to depot
		BotSystem.castDebug(player, state, "RESUPPLY: bank done, -> depot")
		state.resupplyStep = "depot"
		state.resupplyUntil = 0
		state.resupplyRoute = nil
		state.routeIdx = nil

	elseif step == "depot" then
		if state.resupplyUntil == 0 then
			if not state.resupplyRoute then
				BotSystem.castDebug(player, state, "RESUPPLY: finding route to depot")
				state.resupplyRoute = findRouteToDestination("depot")
				state.routeIdx = nil
			end
			if state.resupplyRoute then
				local result = BotSystem.followCityRoute(player, state, state.resupplyRoute)
				if result == "arrived" then
					state.resupplyLastPOI = "depot"
					-- Find and walk to a depot locker (like navigate depot does)
					local lockerPos = BotSystem.findReachableDepotLocker(player, state)
					if lockerPos then
						local pos = player:getPosition()
						local dist = math.max(math.abs(pos.x - lockerPos.x), math.abs(pos.y - lockerPos.y))
						if dist <= 1 and pos.z == lockerPos.z then
							-- Already adjacent — start delay
							if math.random(1, 2) == 1 then
								BotSystem.sayRandom(player, state, "depot")
							end
							state.resupplyUntil = os.time() + math.random(
								BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MIN, BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MAX)
							BotSystem.castDebug(player, state, "RESUPPLY: at depot locker, waiting "
								.. (state.resupplyUntil - os.time()) .. "s")
						else
							-- Walk to the locker
							BotSystem.castDebug(player, state, "RESUPPLY: walking to depot locker at ("
								.. lockerPos.x .. "," .. lockerPos.y .. "," .. lockerPos.z .. ") dist=" .. dist)
							state.resupplyDepotTarget = lockerPos
						end
					else
						-- No locker found — just wait at current position
						state.resupplyUntil = os.time() + math.random(
							BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MIN, BOT_CONFIG.HUNT_RESUPPLY_DEPOT_MAX)
					end
					state.resupplyRoute = nil
					state.routeIdx = nil
				elseif result == "failed" then
					state.resupplyStep = "shop"
					state.resupplyUntil = 0
					state.resupplyRoute = nil
					state.routeIdx = nil
				end
				return
			end
			state.resupplyStep = "shop"
			state.resupplyUntil = 0
			return
		end
		-- Depot delay done, move to shop
		BotSystem.castDebug(player, state, "RESUPPLY: depot done, -> shop")
		state.resupplyStep = "shop"
		state.resupplyUntil = 0
		state.resupplyRoute = nil
		state.routeIdx = nil

	elseif step == "shop" then
		if state.resupplyUntil == 0 then
			if not state.resupplyRoute then
				BotSystem.castDebug(player, state, "RESUPPLY: finding route to shop (potions/runes/ammo)")
				state.resupplyRoute = findRouteToDestination("potions")
					or findRouteToDestination("runes")
					or findRouteToDestination("ammo")
				state.routeIdx = nil
			end
			if state.resupplyRoute then
				local result = BotSystem.followCityRoute(player, state, state.resupplyRoute)
				if result == "arrived" then
					state.resupplyLastPOI = "potions"
					if math.random(1, 2) == 1 then
						player:say("hi", TALKTYPE_SAY)
					end
					state.resupplyUntil = os.time() + math.random(
						BOT_CONFIG.HUNT_RESUPPLY_SHOP_MIN, BOT_CONFIG.HUNT_RESUPPLY_SHOP_MAX)
					BotSystem.castDebug(player, state, "RESUPPLY: arrived at shop, waiting "
						.. (state.resupplyUntil - os.time()) .. "s")
				elseif result == "failed" then
					state.resupplyStep = "done"
					state.resupplyUntil = 0
					state.resupplyRoute = nil
					state.routeIdx = nil
				end
				return
			end
			state.resupplyStep = "done"
			state.resupplyUntil = 0
			return
		end
		BotSystem.castDebug(player, state, "RESUPPLY: shop done, -> done")
		state.resupplyStep = "done"
		state.resupplyUntil = 0
		state.resupplyRoute = nil
		state.routeIdx = nil

	elseif step == "done" then
		if state.huntPhase == HUNT_PHASE.PREPARING then
			-- Preparation complete — now head to the spawn
			BotSystem.castDebug(player, state, "RESUPPLY: preparation complete, heading to spawn")
			logger.info("[Bot] " .. player:getName() .. " preparation done, heading to spawn")
			BotSystem.beginHuntPhase(player, state, HUNT_PHASE.TRAVEL_TO)
		elseif state.huntPhase == HUNT_PHASE.RESUPPLYING then
			-- Post-hunt resupply done — re-roll for another hunt or end
			BotSystem.castDebug(player, state, "RESUPPLY: post-hunt resupply done, re-rolling")
			BotSystem.finishResupplyAndReroll(player, state)
		else
			BotSystem.castDebug(player, state, "RESUPPLY: done, ending hunt")
			BotSystem.endHunt(player, state)
		end
	end
end

-- After post-hunt resupply, decide whether to hunt again or go idle
function BotSystem.finishResupplyAndReroll(player, state)
	local scriptId = state.huntScriptId
	local kills = state.huntKillCount or 0
	local scriptName = "?"
	if scriptId and BotHuntData.scripts[scriptId] then
		scriptName = BotHuntData.scripts[scriptId].name
	end

	-- Release the current hunt reservation
	if scriptId then
		BotHuntData.releaseHunt(scriptId, player:getGuid())
	end

	BotSystem.castDebug(player, state, "REROLL: resupply done (hunt: " .. scriptName
		.. " kills=" .. kills .. "), rolling for next hunt")
	logger.info("[Bot] " .. player:getName() .. " resupply done (hunt: " .. scriptName
		.. " kills=" .. kills .. "), rolling for next hunt")

	-- Roll for another hunt (same chance as normal idle roll)
	-- If successful, the bot stays in the hunt city and hunts again
	if math.random(1, BOT_CONFIG.HUNT_CHANCE_PER_TICK) <= 3 then
		-- Clear old hunt state but keep position
		local oldTownId = state.huntTownId
		player:setTarget(nil)
		state.huntScriptId = nil
		state.huntPhase = nil
		state.huntWaypoints = nil
		state.huntWaypointIdx = 0
		state.huntTargets = nil
		state.huntStartTime = nil
		state.huntEndTime = nil
		state.huntKillCount = 0
		state.huntMonsterTarget = nil
		state.huntWaypointSkipCount = 0
		state.huntLastPos = nil
		state.resupplyStep = nil
		state.resupplyUntil = 0
		state.resupplyWalkTarget = nil
		state.resupplyRoute = nil
		state.routeIdx = nil
		state.routeStuckCount = nil
		state.resupplyDepotTarget = nil
		state.resupplyDepotWaitUntil = nil
		state.resupplyLastPOI = nil
		state.walkTarget = nil
		state.currentPOI = nil
		state.state = BOT_STATE.IDLE

		-- Try to start a new hunt (prefer same town since we're already here)
		if BotSystem.tryStartHunt(player, state) then
			logger.info("[Bot] " .. player:getName() .. " re-hunting after resupply!")
			return
		end
	end

	-- No re-hunt — end normally
	BotSystem.endHunt(player, state)
end

-- Cleanly end a hunt (success)
function BotSystem.endHunt(player, state)
	-- Clear native target
	player:setTarget(nil)
	local scriptId = state.huntScriptId
	local kills = state.huntKillCount or 0
	local duration = state.huntStartTime and (os.time() - state.huntStartTime) or 0
	local scriptName = "?"
	if scriptId and BotHuntData.scripts[scriptId] then
		scriptName = BotHuntData.scripts[scriptId].name
	end

	BotSystem.castDebug(player, state, "HUNT END: " .. scriptName
		.. " kills=" .. kills
		.. " duration=" .. math.floor(duration / 60) .. "m")
	logger.info("[Bot] " .. player:getName() .. " finished hunt: " .. scriptName
		.. " kills=" .. kills
		.. " duration=" .. math.floor(duration / 60) .. "m")

	-- Release reservation
	if scriptId then
		BotHuntData.releaseHunt(scriptId, player:getGuid())
	end

	-- Set cooldown
	local cooldown = math.random(BOT_CONFIG.HUNT_COOLDOWN_MIN, BOT_CONFIG.HUNT_COOLDOWN_MAX)
	state.huntCooldownUntil = os.time() + cooldown

	-- Clear all hunt state
	state.huntScriptId = nil
	state.huntPhase = nil
	state.huntWaypoints = nil
	state.huntWaypointIdx = 0
	state.huntTargets = nil
	state.huntStartTime = nil
	state.huntEndTime = nil
	state.huntKillCount = 0
	state.huntMonsterTarget = nil
	state.huntWaypointSkipCount = 0
	state.huntLastPos = nil
	state.huntTownId = nil
	state.resupplyStep = nil
	state.resupplyUntil = 0
	state.resupplyWalkTarget = nil
	state.resupplyRoute = nil
	state.routeIdx = nil
	state.routeStuckCount = nil
	state.resupplyDepotTarget = nil
	state.resupplyDepotWaitUntil = nil
	state.resupplyLastPOI = nil
	state.walkTarget = nil
	state.currentPOI = nil

	-- Teleport home if far from town
	local town = player:getTown()
	if town then
		local pos = player:getPosition()
		local templePos = town:getTemplePosition()
		local expectedZ = BotSystem.getExpectedZ(player)
		local dist = math.max(math.abs(pos.x - templePos.x), math.abs(pos.y - templePos.y))
		if pos.z ~= expectedZ or dist > 50 then
			local spawnPos = BotSystem.getSpawnPosition(player)
			player:teleportTo(spawnPos)
		end
	end

	-- Return to idle
	state.state = BOT_STATE.IDLE
end

-- Abort a hunt (stuck, timeout, error)
function BotSystem.abortHunt(player, state, reason)
	-- Clear native target
	player:setTarget(nil)
	local scriptId = state.huntScriptId
	local scriptName = "?"
	if scriptId and BotHuntData.scripts[scriptId] then
		scriptName = BotHuntData.scripts[scriptId].name
	end

	BotSystem.castDebug(player, state, "ABORT: " .. scriptName
		.. " reason=" .. (reason or "unknown")
		.. " kills=" .. (state.huntKillCount or 0)
		.. " phase=" .. (state.huntPhase or "?"))
	logger.warn("[Bot] " .. player:getName() .. " ABORTING hunt: " .. scriptName
		.. " reason=" .. (reason or "unknown")
		.. " kills=" .. (state.huntKillCount or 0)
		.. " phase=" .. (state.huntPhase or "?"))

	-- Release reservation
	if scriptId then
		BotHuntData.releaseHunt(scriptId, player:getGuid())
	end

	-- Teleport home if far from town or on wrong z-level
	local pos = player:getPosition()
	local town = player:getTown()
	local expectedZ = BotSystem.getExpectedZ(player)
	if town then
		local templePos = town:getTemplePosition()
		local dist = math.max(math.abs(pos.x - templePos.x), math.abs(pos.y - templePos.y))
		if pos.z ~= expectedZ or dist > 50 then
			local spawnPos = BotSystem.getSpawnPosition(player)
			player:teleportTo(spawnPos)
			logger.info("[Bot] " .. player:getName() .. " teleported home from ("
				.. pos.x .. "," .. pos.y .. "," .. pos.z .. ") dist=" .. dist)
		end
	end

	-- Set cooldown (shorter than normal — allow retrying sooner)
	state.huntCooldownUntil = os.time() + math.random(300, 600)

	-- Clear all hunt state
	state.huntScriptId = nil
	state.huntPhase = nil
	state.huntWaypoints = nil
	state.huntWaypointIdx = 0
	state.huntTargets = nil
	state.huntStartTime = nil
	state.huntEndTime = nil
	state.huntKillCount = 0
	state.huntMonsterTarget = nil
	state.huntWaypointSkipCount = 0
	state.huntLastPos = nil
	state.huntTownId = nil
	state.resupplyStep = nil
	state.resupplyUntil = 0
	state.resupplyWalkTarget = nil
	state.resupplyRoute = nil
	state.routeIdx = nil
	state.routeStuckCount = nil
	state.resupplyDepotTarget = nil
	state.resupplyDepotWaitUntil = nil
	state.resupplyLastPOI = nil
	state.walkTarget = nil
	state.currentPOI = nil

	state.state = BOT_STATE.IDLE
end

-- ============================================================================
-- Party System: /party command support
-- ============================================================================

-- Find leader's current combat target (monster being fought nearby)
function BotSystem.findLeaderCombatTarget(leader)
	local leaderPos = leader:getPosition()
	local spectators = Game.getSpectators(leaderPos, false, false, 7, 7, 7, 7)
	for _, creature in ipairs(spectators) do
		if creature:isMonster() and creature:getHealth() > 0 then
			-- Check if this monster is attacking the leader or being attacked
			local target = creature:getTarget()
			if target and target:getId() == leader:getId() then
				return creature
			end
		end
	end
	return nil
end

-- Party follow AI: follow leader, attack their target, heal party members
function BotSystem.doPartyFollow(player, state)
	local leaderGuid = state.partyLeader
	if not leaderGuid then
		BotSystem.exitParty(player, state)
		return
	end

	local leader = Player(leaderGuid)
	if not leader then
		BotSystem.exitParty(player, state)
		return
	end

	-- Check party still exists
	local party = player:getParty()
	if not party then
		BotSystem.exitParty(player, state)
		return
	end

	local pos = player:getPosition()
	local leaderPos = leader:getPosition()

	-- Self heal (always)
	BotSystem.doHealing(player, state)

	-- Mana restore
	if player:getMana() < player:getMaxMana() * 0.5 then
		player:addMana(player:getMaxMana())
	end

	-- Party healing (druid only)
	BotSystem.doPartyHealing(player, state, party)

	-- Attack leader's target
	local leaderTarget = leader:getTarget()
	if not leaderTarget or leaderTarget:getHealth() <= 0 then
		leaderTarget = BotSystem.findLeaderCombatTarget(leader)
	end

	local chasing = false
	if leaderTarget and leaderTarget:getHealth() > 0 then
		-- Set target (server auto-attacks with weapon) + cast spell
		if player:getTarget() ~= leaderTarget then
			player:setTarget(leaderTarget)
		end
		BotSystem.doCastSpell(player, state, leaderTarget)
		-- Actively chase the target if not in range
		local tpos = leaderTarget:getPosition()
		if pos.z == tpos.z then
			local voc = player:getVocation()
			local baseVoc = voc and voc:getBaseId() or 4
			local attackRange = 1
			if baseVoc == 1 or baseVoc == 2 then attackRange = 3
			elseif baseVoc == 3 then attackRange = 5 end
			local tdist = math.max(math.abs(pos.x - tpos.x), math.abs(pos.y - tpos.y))
			if tdist > attackRange and not player:isWalking() then
				local dirs = player:getPathTo(tpos, 0, attackRange, true, true, BOT_CONFIG.PATH_MAX_DIST)
				if dirs and type(dirs) == "table" and #dirs > 0 then
					player:startAutoWalk(dirs)
					chasing = true
				end
			elseif tdist <= attackRange then
				chasing = true -- in range, no need to follow leader
			end
		end
	end

	-- Follow leader (only if not chasing a target)
	if not chasing then
		if pos.z ~= leaderPos.z then
			BotSystem.chaseTarget(player, state, leader)
		else
			local dist = math.max(math.abs(pos.x - leaderPos.x), math.abs(pos.y - leaderPos.y))
			if dist > 2 and not player:isWalking() then
				local dirs = player:getPathTo(leaderPos, 0, 1, true, true, BOT_CONFIG.PATH_MAX_DIST)
				if dirs and type(dirs) == "table" and #dirs > 0 then
					player:startAutoWalk(dirs)
				elseif dist > 15 then
					-- Too far, teleport near leader
					player:teleportTo(leaderPos, true)
				end
			end
		end
	end
end

-- Druid party healing: heal party members with exura sio
function BotSystem.doPartyHealing(player, state, party)
	local voc = player:getVocation()
	local baseVoc = voc and voc:getBaseId() or 4
	if baseVoc ~= 2 then return end -- Only druids heal others

	local now = os.time()
	if state.lastPartyHealTime and now - state.lastPartyHealTime < 1 then return end

	local members = party:getMembers()
	local leader = party:getLeader()
	if leader then table.insert(members, leader) end

	for _, member in ipairs(members) do
		if member:getId() ~= player:getId() then
			local hp = member:getHealth()
			local maxHp = member:getMaxHealth()
			if (hp / math.max(maxHp, 1)) * 100 < 70 then
				local mpos = member:getPosition()
				local pos = player:getPosition()
				local dist = math.max(math.abs(pos.x - mpos.x), math.abs(pos.y - mpos.y))
				if dist <= 7 and pos.z == mpos.z then
					state.lastPartyHealTime = now
					player:say("exura sio \"" .. member:getName(), TALKTYPE_MONSTER_SAY)
					local magicLevel = player:getMagicLevel()
					local healAmount = math.floor(player:getLevel() / 5 + magicLevel * 4) + 100
					member:addHealth(healAmount)
					mpos:sendMagicEffect(CONST_ME_MAGIC_BLUE)
					return -- heal 1 per tick
				end
			end
		end
	end
end

-- Exit party mode: return bot to normal behavior
function BotSystem.exitParty(player, state)
	local party = player:getParty()
	if party then
		party:removeMember(player)
	end

	state.partyLeader = nil
	state.lastPartyHealTime = nil
	state.state = BOT_STATE.IDLE

	-- Teleport home
	local town = player:getTown()
	if town then
		local spawnPos = BotSystem.getSpawnPosition(player)
		player:teleportTo(spawnPos)
	end

	logger.info("[Bot] " .. player:getName() .. " left party, returning to IDLE")
end

logger.info("[BotSystem] Bot system library loaded (200 bots, all cities, travel, fight/flight, PK, hunting, party)")
