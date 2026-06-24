/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "game/movement/position.hpp"

class Player;
class Creature;
class Monster;
class Item;

// Bot AI states (mirrors Lua BOT_STATE)
enum class BotAIState : uint8_t {
	INACTIVE = 0,
	IDLE = 1,
	DWELLING = 2,
	COMBAT = 3,
	FLEEING = 4,
	TRAVELING = 5,
	PK_ATTACK = 6,
	HUNTING = 7,
	PARTY = 8,
};

// POI type
enum class POIType : uint8_t {
	DEPOT,
	DEPOT_OUTSIDE,
	TEMPLE,
	BOAT,
	SHOP,
	NPC,
	ADVENTURER_STONE,  // arrives at temple → uses item 16277 → tours shared dungeon → returns to same temple
};

// Hunt phase
enum class HuntPhase : uint8_t {
	PREPARING = 0,
	TRAVEL_TO = 1,
	PATROLLING = 2,
	LEAVING = 3,
	RESUPPLYING = 4,
};

// Floor change state machine
enum class FloorChangeState : uint8_t {
	NONE = 0,
	SCANNING = 1,
	WALKING_TO = 2,
	STEPPING_ON = 3,
	VERIFYING = 4,
	COMPLETED = 5,
	FAILED = 6,
};

// Z-transition type
struct ZTransition {
	Position pos;
	std::string type; // "stairs", "ladder", "sewer"
	int32_t dist = 0;
};

// Point of Interest
struct BotPOI {
	std::string name;
	Position pos;
	POIType type;
};

// Spell definition
struct BotSpell {
	int32_t level = 0;
	std::string name;
	uint8_t combatType = 0;
	int32_t range = 0;
	int32_t cd = 2;
	int32_t baseDmg = 15;
	bool isRune = false;
	bool pvpOnly = false;
	double avgMlevelCoef = 0; // estimated avg damage = (lv/5) + (ml * coef) + constant
	double avgConstant = 0;
};

// AoE spell area type
enum class AoeAreaType : uint8_t {
	CIRCLE,       // self-centered circle (Divine Caldera, Groundshaker, Rage of the Skies, Eternal Winter)
	WAVE4,        // directional wave AREA_WAVE4 (Fire Wave, Ice Wave)
	SQUAREWAVE5,  // directional wave AREA_SQUAREWAVE5 (Terra Wave)
	MELEE_CIRCLE, // small radius around self AREA_SQUARE1X1 (Berserk, Fierce Berserk)
	BEAM5,        // directional beam AREA_BEAM5 (Energy Beam) — 4 tiles forward, 1 wide
	RING,         // self-centered ring with empty inner hole (Ice Burst, Terra Burst — AREA_RING<N>_BURST<M>)
};

// AoE spell definition
struct BotAoeSpell {
	int32_t level = 0;
	std::string name;          // spell words
	uint8_t combatType = 0;    // CombatType_t
	AoeAreaType areaType = AoeAreaType::CIRCLE;
	int32_t areaSize = 3;      // radius for CIRCLE, depth for WAVE, 1 for MELEE
	int32_t cd = 4;            // cooldown in seconds
	int32_t baseDmg = 20;      // per-target base damage (legacy, used as fallback)
	int32_t minTargets = 3;    // minimum valid targets to use this spell
	uint16_t magicEffect = 0;  // CONST_ME_* visual effect
	double avgMlevelCoef = 0;  // estimated avg damage = (lv/5) + (ml * coef) + constant
	double avgConstant = 0;
};

// Heal spell definition
struct BotHealSpell {
	int32_t level = 0;
	std::string name;
	int32_t heal = 80;
	int32_t cd = 1;
};

// Waypoint type (from DB waypoint_type column)
enum class WaypointType : uint8_t {
	NODE = 0,        // normal navigation waypoint
	STAND = 1,       // stop and wait at this position
	LADDER = 2,      // step on ladder to change floor
	ROPE = 3,        // use rope spot to go up
	HOLE = 4,        // step on hole to go down
	STAIRS_UP = 5,
	STAIRS_DOWN = 6,
	DOOR = 7,        // open door
	ACTION = 8,      // custom action (action_label)
	LEVITATE_UP = 9,
	LEVITATE_DOWN = 10,
	MACHETE = 11,       // use machete on target tile (jungle grass etc.)
	USE_WITH = 12,      // generic use-item-on-tile (pickaxe, gems, etc.)
	NPC_INTERACT = 13,  // walk to NPC and say "hi" (quest scripts)
	TELEPORT = 14,      // fire internalTeleport immediately (cross-continent NPC TPs the bot can't walk)
};

// Waypoint with position and type
struct Waypoint {
	Position pos;
	WaypointType type = WaypointType::NODE;
	uint16_t itemId = 0;         // item to use for MACHETE/USE_WITH (e.g. 3308=machete, 3456=pickaxe)
	std::string extraData;       // direction for levitate ("face_north"), NPC name, item ID string, etc.
	bool isWalkOnFc = false;     // cached: tile has FLOORCHANGE or TELEPORT flag (set at load time)

	Waypoint() = default;
	Waypoint(const Position& p, WaypointType t = WaypointType::NODE) : pos(p), type(t) {}
	Waypoint(const Position& p, WaypointType t, uint16_t item, const std::string& extra = "")
		: pos(p), type(t), itemId(item), extraData(extra) {}
};

// City route graph for intra-city navigation
struct CityRouteGraph {
	// pairs[src][dst] = waypoints from src POI to dst POI
	std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Waypoint>>> pairs;
	// pois[name] = position of the POI
	std::unordered_map<std::string, Position> pois;
};

// Hunt script (loaded from MySQL)
struct HuntScript {
	uint32_t id = 0;
	std::string name;
	uint32_t townId = 0;
	uint32_t levelMin = 0;
	uint32_t levelMax = 0;
	uint32_t vocationMask = 0;
	bool enabled = true;
	bool isQuest = false;
	std::string scriptCategory;  // "hunt", "quest", "traveling"
	std::string spawnGroup;
	uint8_t keepDistanceEK = 0; // min sqm from monsters per vocation (0 = disabled)
	uint8_t keepDistanceMS = 0;
	uint8_t keepDistanceED = 0;
	uint8_t keepDistanceRP = 0;
	std::vector<Waypoint> travelToWaypoints;
	std::vector<Waypoint> patrolWaypoints;
	std::vector<Waypoint> travelFromWaypoints;
	std::vector<std::string> targetNames; // lowercase monster names
};

// Per-bot state (contiguous in memory for cache efficiency)
struct BotState {
	// Identity
	uint32_t guid = 0;
	std::string name;              // cached player name for re-loading after true-offline removal
	std::weak_ptr<Player> playerRef;
	uint32_t townId = 0;
	std::string townName;  // source_name from travel_positions (e.g. "Edron", "Thais")
	uint8_t vocationId = 0;

	// AI state
	BotAIState state = BotAIState::INACTIVE;
	bool active = false;
	bool aiPaused = false;       // when true, tick loop skips this bot (used for CPU benchmarking)
	bool isQuestBot = false;     // ~5% of bots run quest scripts (guid % 20 == 0)
	uint32_t tickCounter = 0;

	// Hibernation: when true, Player object is destroyed but BotState persists so the AI
	// state machine resumes seamlessly when the bot wakes up. Triggered by proximity loop
	// in bot_hibernation.lua when no real player is within HIBERNATION_CONFIG.DISTANCE_TILES
	// for HIBERNATION_CONFIG.HYSTERESIS_MS. Wake is immediate when any real player approaches.
	bool hibernated = false;
	int64_t hibernateEligibleSince = 0;  // Lua-side timer; 0 = currently has player nearby

	// v2 virtual simulator: hibernated bots run a thin state-machine simulator every 30s
	// that advances position + state without touching the game engine. See BOT_HIBERNATION_V2.md.
	uint32_t cachedLevel = 0;        // populated at registerBot/wakeBot for level-gated hunt reroll
	int64_t lastVirtualPosSave = 0;  // last OTSYS_TIME() the virtual position was UPDATEd to DB

	// PERF_INVESTIGATION_2026-05-24 Phase B (2026-06-01): LRU fairness for density-capped
	// wake. Updated by shouldGateWake on every wake attempt (grant or cap-skip) so a bot
	// that loses a cap slot rotates to the back of the LRU queue next iteration. Sorted
	// ascending (oldest-first) in wakeBotsInRadius and in the Lua proximity loop.
	int64_t lastWakeAttemptMs = 0;

	// Activation fallback: if still IDLE 60s after activation without transitioning, teleport to temple
	int64_t activatedAt = 0;     // OTSYS_TIME() when activated; 0 = no pending fallback

	// Position tracking
	Position currentPos;
	Position lastPos;

	// Spectator cache (Gesior b_possible_targets pattern, 2026-05-27).
	// Cached snapshot of Players and Monsters within MONSTER_SCAN_RADIUS of the bot's
	// position. Refreshed every SPECTATOR_CACHE_TTL_MS (600ms = 3 ticks at 200ms tick,
	// matching Gesior's checkInterval 3 cadence for updatePossibleTargets).
	// Stores creature IDs (not shared_ptrs) so it doesn't extend creature lifetime;
	// call sites resolve via g_game().getCreatureByID + recheck isRemoved/getHealth.
	// Used for TARGET SELECTION (which creatures do I consider?). NOT used for AOE
	// damage evaluation or rune impact preview — those keep fresh per-call scans
	// because monsters may have moved within the 600ms TTL window.
	std::vector<uint32_t> cachedPlayerIds;
	std::vector<uint32_t> cachedMonsterIds;
	int64_t cachedSpectatorsExpiry = 0;  // OTSYS_TIME() ms when cache becomes stale

	// Walking
	Position walkTarget;
	bool hasWalkTarget = false;
	const BotPOI* currentPOI = nullptr;
	uint8_t pathFailCount = 0;
	uint8_t consecutivePOIFails = 0;
	std::unordered_set<std::string> visitedPOIs;

	// Dwelling
	int64_t dwellUntil = 0;
	Position idleDepotTarget;
	bool hasDepotTarget = false;

	// Stop cooldown (prevent hunt/travel rolls after admin stop command)
	int64_t stopCooldownUntil = 0;

	// Returning home
	bool returningHome = false;

	// Chat
	uint32_t lastChatTick = 0;          // legacy: zeroed at register/activate, never read for rate-limit. Kept for ABI continuity.

	// ---- Bot Liveness Pack (Phase B) ----
	// All fields below are scheduling state for liveness behaviors. See
	// BOT_SYSTEM_DOCS.md for the full design. ~80 bytes
	// per bot × 1000 bots = ~80KB extra resident; negligible.

	// 16-bit personality. Set at registerBot from hash(guid * golden_ratio_64
	// ^ s_serverStartEpoch), so identity is stable across the session but rerolls
	// every server restart. 4-bit fields × 4 traits.
	uint16_t personalitySeed = 0;
	inline uint8_t chattyness()    const { return static_cast<uint8_t>((personalitySeed >>  0) & 0xF); }
	inline uint8_t fidgetiness()   const { return static_cast<uint8_t>((personalitySeed >>  4) & 0xF); }
	inline uint8_t walkBias()      const { return static_cast<uint8_t>((personalitySeed >>  8) & 0xF); }
	inline uint8_t solitudeBias()  const { return static_cast<uint8_t>((personalitySeed >> 12) & 0xF); }

	// Idle litter-drop state (BOT_LIVENESS_PACK). Drop-only — bots never pick the
	// item back up; the item is left on the ground. fidgetStationarySince: when the
	// current continuous stop began (0 while walking / on an errand). fidgetRolledThisStop:
	// one chance-roll per continuous stop. fidgetDroppedThisWake: at most one drop per
	// awake session (reset in wakeBot / registerBot).
	int64_t fidgetStationarySince = 0;
	bool fidgetRolledThisStop = false;
	bool fidgetDroppedThisWake = false;

	// Turn-in-place scheduling — uses Game::internalCreatureTurn (same as spell-cast).
	int64_t nextTurnInPlaceTime = 0;

	// World Chat (channel 3) + Advertising (channel 5) per-bot post timers.
	// Advertising server-side enforces a 2-min mute condition regardless of this.
	int64_t nextWorldChatTime = 0;
	int64_t nextAdvertisingTime = 0;

	// Local chat anti-repeat LRU ring (Playerbots-style; Playerbots itself doesn't
	// have one and bots visibly repeat — this is our addition). Stores hashes of
	// recently-emitted phrase IDs; oldest evicted on insert. Ring size is bounded
	// by botChatAntiRepeatRingSize config (default 8 entries).
	static constexpr size_t kChatAntiRepeatRingSize = 8;
	uint32_t recentChatRing[kChatAntiRepeatRingSize] = {0};
	uint8_t  recentChatRingNext = 0;

	// Local chat rate-limit timestamp (ms). Replaces the unused lastChatTick.
	int64_t lastChatTimeMs = 0;

	// BOT_CHAT_LIVENESS_V2 Phase F: last time this bot answered a player
	// (keyword reply or PM reply). Separate from lastChatTimeMs so a bot that
	// just idled can still answer a direct "hi", but can't be farmed for
	// replies in a loop.
	int64_t lastReplyTimeMs = 0;

	// Mid-walk pause hard cap. Reset at the start of every new route to prevent
	// the 2%/step probability from clustering into N>3 pauses per route. The
	// pause itself is scheduled via g_dispatcher().scheduleEvent and gated against
	// combat/FC/hunt-target states.
	uint8_t pausesThisRoute = 0;
	// Observed-tier mid-walk pause counter (separate budget from pausesThisRoute so a
	// player arriving mid-route doesn't find a bot that already spent its unobserved
	// budget walking rigidly). Reset at the start of every new route alongside it.
	uint8_t pausesThisRouteObserved = 0;
	// Per-bot "is a real player / cast-watched bot on screen" cache, refreshed at most
	// every ~500ms inside botStartAutoWalk to avoid a Spectators::find<Player> scan on
	// every walk step at scale. walkObservedCacheMs is the last refresh time (ms).
	bool walkObservedCache = false;
	int64_t walkObservedCacheMs = 0;
	// Dispatcher event id of an in-flight scheduled mid-walk pause (0 = none). Stored so
	// deactivateAll() can stopEvent() it before the engine is destroyed + the .so is
	// dlclose()d on /cavebot reload — otherwise the callback would fire into unloaded
	// code. Cleared when the pause fires or is cancelled.
	uint64_t pendingWalkPauseEventId = 0;

	// walk_to_boat spread reservation. Set when the bot picks an alternate tile
	// near the boat NPC via spiralFindFree to avoid stacking with other arrivals.
	// Cleared when travelPhase advances past at_boat.
	Position travelSpreadTarget {};

	// Logging
	bool verboseLog = false;
	bool verboseLogManual = false; // true = user explicitly ran "log on", viewer check won't override
	uint32_t lastLogTick = 0;

	// Floor change state machine (Phase 2)
	FloorChangeState fcState = FloorChangeState::NONE;
	bool fcGoDown = false;
	Position fcTargetPos;
	std::vector<ZTransition> fcTransitions;
	size_t fcTransIdx = 0;
	uint8_t fcPreZ = 0;
	uint8_t fcAttempts = 0;
	int64_t fcStartTime = 0;

	// Door opening (Phase 2)
	// Uses static lookup table in BotEngine

	// Combat state (Phase 3)
	uint32_t attackerId = 0;       // creature ID of whoever is attacking us
	uint32_t pkTarget = 0;         // creature ID for PK attack
	std::string combatDecision;    // "fight" or "flee"
	int64_t combatStartTime = 0;
	int64_t lastCombatProgress = 0;
	int64_t lastAttackTime = 0;
	int64_t lastHealTime = 0;
	uint32_t pvpManaSpent = 0;
	uint32_t ignoredAttackerId = 0;
	bool ignoredHitBack = false;
	int32_t defenseScanCooldown = 0;
	Position preCombatPos;
	bool hasPCPos = false;
	int64_t combatHpCheckTime = 0;
	int32_t combatHpBaseline = 0;
	uint8_t combatStalemateCount = 0;
	// Vigilante: seen PKers (creatureId -> timestamp)
	std::unordered_map<uint32_t, int64_t> seenPKers;
	int64_t lastPKerScanTime = 0;
	int64_t lastDeathTime = 0;
	int64_t lastPvpAttackTime = 0; // last time bot attacked a player (for PZ-lock)

	// Flee-to-PZ navigation
	bool hasFleeTarget = false;     // whether we have a valid PZ flee destination
	bool fleeDirectional = false;   // true = fallback to directional flee (no route found)

	// Death pause (bot stays active but paused at temple after dying, like a real player)
	int64_t deathPauseUntil = 0;  // 0 = not paused, >0 = OTSYS_TIME when pause ends
	BotAIState preDeathState = BotAIState::IDLE;  // state to resume after pause

	// Party hunt state (autonomous bot-to-bot party hunts)
	uint32_t partyHuntId = 0;         // unique session ID (0 = not in party hunt)
	uint8_t partyRole = 0;            // 0=none, 1=TANK, 2=HEALER, 3=DPS_MAGE, 4=DPS_RANGED
	uint32_t partyLeaderGuid = 0;     // guid of EK leading the party hunt
	bool isPartyHuntLeader = false;   // true if THIS bot is the EK leader

	// Hunting state (Phase 4)
	uint32_t huntScriptId = 0;
	HuntPhase huntPhase = HuntPhase::PREPARING;
	uint32_t huntTownId = 0;
	int64_t huntStartTime = 0;
	int64_t huntEndTime = 0;
	uint32_t huntKillCount = 0;
	size_t huntWaypointIdx = 0;
	uint8_t huntPatrolCycles = 0;
	uint32_t huntTargetId = 0;       // current monster target
	uint8_t huntChaseFailCount = 0;
	std::unordered_set<uint32_t> huntIgnoredMonsters;
	int64_t huntCooldownUntil = 0;
	uint32_t huntWaypointSkipCount = 0;
	int64_t lastKillTime = 0;        // timestamp of last monster kill (for linger check)
	int64_t huntResupplyStart = 0;
	uint8_t huntResupplyPhase = 0;   // 0=bank, 1=depot, 2=shop, 3=done
	bool resupplyRerolled = false;   // true after reroll decision at depot during resupply
	uint32_t huntDebugKillLimit = 0; // 0=disabled (time-based only), >0=debug kill limit

	// Preparing state (depot + shop before hunt)
	int64_t prepareStartTime = 0;
	int64_t prepareWaitUntil = 0;
	uint8_t prepareStep = 0;     // 0=walk_depot, 1=at_depot, 2=walk_shop, 3=at_shop, 4=exit_shop, 5=done
	Position prepareTarget;
	bool prepareHasTarget = false;

	// City route following (for intra-city navigation)
	std::vector<Waypoint> cityRouteWps;
	size_t cityRouteIdx = 0;
	bool followingCityRoute = false;

	// Pending navigation destination (set by navigate command, consumed by doIdle)
	std::string pendingNavDest;

	// Travel state (Phase 5)
	uint32_t travelDestTownId = 0;
	std::string travelPhase;       // "walk_to_boat", "at_boat", "teleported", "walk_from_boat", "arrived"
	Position travelBoatPos;        // destination boat position
	Position travelSrcBoatPos;     // source boat position (walk to this first)
	int64_t travelWaitUntil = 0;
	bool pendingHuntAfterTravel = false;
	bool travelDestVerified = false;  // true after one-time destination verification in walk_from_boat

	// Smart route selection — track attempted source POIs for current destination
	std::unordered_set<std::string> triedRouteSources;
	std::string lastRouteDestination;

	// Recovery route (for navigating back when lost/far from town instead of teleporting)
	std::vector<Waypoint> recoveryWaypoints;  // Combined patrol[idx..end] + travel_from
	bool isRecoveryRoute = false;

	// Adventurer's Stone trip state (POIType::ADVENTURER_STONE)
	// Self-contained sub-state machine: bot arrives at temple POI → uses stone (mimics
	// adventurers_stone.lua action) → walks dungeon waypoints → dwells at one random non-stairs
	// wp → steps on forcefield (32210,32292,6) → teleported home by aid:4253 MoveEvent
	bool advStoneActive = false;
	uint8_t advStonePhase = 0;          // 0=following_route, 1=dwelling, 2=stepping_on_forcefield
	uint16_t advStoneRouteIdx = 0;      // current waypoint index into adventurerStoneRoute_
	uint16_t advStoneIdleAt = 0;        // randomly picked dwell waypoint index (set at trip start)
	uint32_t advStoneStartTownId = 0;   // town the bot used the stone from (matches kAdventurerStoneTempleRanges)
	int64_t advStoneDwellUntil = 0;
	int64_t advStoneDeadline = 0;       // forcefield-step fail-safe (~30s after route end)

	// 3-way dwell sub-activity (rolled at trip start, executed during phase 1)
	uint8_t advStoneDwellMode = 0;      // 0=route waypoint (existing), 1=reward chest, 2=exercise dummy
	uint16_t advStoneDwellWeaponId = 0; // Lasting Exercise weapon id (mode==2 only)
	bool advStoneTrainingActive = false;// true once useItemEx has kicked off Lua training loop; gates setTraining(false) on cleanup
	Position advStoneDwellTarget;       // walkable tile near chest/dummy (mode!=0 only)

	// Autonomous activity reroll
	int64_t nextRerollTime = 0;  // earliest OTSYS_TIME for next activity decision
	bool postActivationReroll = false;  // First reroll after activation uses reduced dwell weight

	// Wake stagger: ticks to skip AI work after wake-from-hibernation. Set per-bot in wakeBot
	// to (3 + recentWakeStagger_), where the engine-wide stagger counter increments per wake
	// and decays 1/tick. Spreads the post-wake AI burst (Spectators::find + A* per bot) over
	// multiple dispatcher windows instead of stacking 13 simultaneous AI ticks. Heal still runs.
	uint8_t wakeQuietTicks = 0;

	// Helpers
	std::shared_ptr<Player> getPlayer() const {
		return playerRef.lock();
	}
};

// Abstract interface for the bot engine — implemented in libbot_engine.so
class IBotEngine {
public:
	virtual ~IBotEngine() = default;

	// Lifecycle
	virtual void registerBot(const std::shared_ptr<Player>& player) = 0;
	virtual void unregisterBot(uint32_t guid) = 0;
	virtual bool activateBot(uint32_t guid) = 0;
	virtual bool deactivateBot(uint32_t guid) = 0;
	virtual void forceDeactivateBot(uint32_t guid) = 0;
	virtual void forceDeactivateBotForReload(uint32_t guid) = 0;
	virtual bool reactivateBotForReload(uint32_t guid) = 0;
	virtual void deactivateAll() = 0;
	virtual void pauseBotForDeath(uint32_t guid) = 0;

	// Main tick — called from game loop
	virtual void tick() = 0;

	// Info
	virtual uint32_t countActiveBots() const = 0;
	virtual uint32_t countTotalBots() const = 0;
	virtual BotState* getBotState(uint32_t guid) = 0;
	virtual std::string getStatusText(uint32_t guid) = 0;

	// Hunt data loading (called at startup)
	virtual void loadHuntData() = 0;

	// Command interface
	virtual std::string executeCommand(const std::string& botName, const std::string& command) = 0;

	// State persistence (save/restore on graceful shutdown/startup)
	virtual void saveAllStates() = 0;
	virtual void restoreAllStates() = 0;
	virtual void clearPersistedStates() = 0;

	// AI pause flag (CPU benchmarking: bot stays in world, no tick processing)
	virtual void setBotAIPaused(uint32_t guid, bool paused) = 0;
	virtual void setAllBotsAIPaused(bool paused) = 0;

	// Hibernation: full despawn + AI-state preservation. Player is destroyed, BotState
	// stays in bots_ map, no monster aggro/world reaction cost. Wake re-creates Player
	// from DB without resetting BotState (unlike activateBot which wipes it).
	virtual bool hibernateBot(uint32_t guid) = 0;
	virtual bool wakeBot(uint32_t guid) = 0;
	virtual std::vector<uint32_t> getHibernatedBotGuids() const = 0;
	virtual uint32_t getHibernatedBotGuidByName(const std::string &name) const = 0;
	// PM-to-hibernated-bot bypass (ported from feat/bot-llm-chat Phase 6). Returns
	// the hibernated Player object from the engine's hibernation pool by guid, or
	// nullptr if guid isn't in the pool. The Player object is still valid (inventory,
	// name, account, etc. preserved across hibernation) — it's just unlinked from
	// g_game().getPlayers().
	virtual std::shared_ptr<Player> getHibernatedBotPlayer(uint32_t guid) const = 0;

	// BOT_CHAT_LIVENESS_V2 Phase F: keyword replies. Called from Game on the
	// dispatcher thread. onPlayerSayNearBots fires for every real-player local
	// say (TALKTYPE_SAY) — the engine keyword-matches the text and may schedule
	// ONE nearby idle bot to answer after a human-feeling typing delay.
	// onPlayerPmToBot fires when a real player PMs a bot (awake or hibernated —
	// Game::playerSpeakTo resolves hibernated receivers via the pool first).
	virtual void onPlayerSayNearBots(uint32_t playerId, const Position& pos, const std::string& text) = 0;
	virtual void onPlayerPmToBot(uint32_t botGuid, uint32_t playerId, const std::string& text) = 0;
	// Bulk operations (debugging/benchmarking; mirrors setAllBotsAIPaused)
	virtual uint32_t hibernateAllEligibleBots() = 0;
	virtual uint32_t wakeAllHibernatedBots() = 0;

	// Pre-wake hook: called from Game::internalTeleport when a real player teleports.
	// Wakes any hibernated bot whose virtualPos is within Chebyshev radius of pos. Bots
	// materialize in the same dispatcher window as the player's arrival packet so the
	// destination looks populated immediately. Returns count woken.
	virtual uint32_t wakeBotsInRadius(const Position& pos, int radius) = 0;

	// Reload-recovery: re-attaches an orphaned hibernated Player (Lua-held shared_ptr)
	// to the new engine after botReload() destroyed the old hibernationPool_. The Player
	// object survived the dlclose because Lua's BotPlayers table held a strong ref;
	// this method puts it back in g_game(), creates a fresh BotState, and silently
	// places at the staging tile. The caller (Lua executeReload) then runs the standard
	// reactivateBotForReload teleport-to-POI distribution on it.
	// Returns true if the orphan was successfully recovered, false on any failure.
	virtual bool recoverOrphanForReload(uint32_t guid, const std::shared_ptr<Player>& player) = 0;

	// Phase 6: list of all registered active bots (regardless of hibernation state).
	// Used by the cast viewer character list — gives a stable count that doesn't jitter
	// with transient broadcasting/removed flags during hibernate/wake transitions.
	virtual std::vector<std::string> getActiveBotNames() const = 0;

	// Guid sibling of getActiveBotNames(). Used by Game::updatePlayersOnline to
	// include hibernated bots in the players_online DB table when the
	// botPlayersShowAsOnline config flag is true (awake bots are already covered
	// via g_game().getPlayers()).
	virtual std::vector<uint32_t> getActiveBotGuids() const = 0;

	// Count of bots that are active && hibernated. Used by ProtocolStatus to
	// inflate the reported player count when botPlayersShowAsOnline is true so
	// MyAAC's sidebar widget and the binary 0x20/XML status query agree with the
	// players_online DB table (awake bots already count via g_game().getPlayers()).
	virtual size_t getHibernatedBotCount() const = 0;
};
