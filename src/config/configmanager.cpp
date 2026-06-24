/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "config/configmanager.hpp"

#include "lib/di/container.hpp"
#include "game/game.hpp"
#include "server/network/webhook/webhook.hpp"
#include "utils/tools.hpp"

#if LUA_VERSION_NUM >= 502
	#undef lua_strlen
	#define lua_strlen lua_rawlen
#endif

ConfigManager &ConfigManager::getInstance() {
	return inject<ConfigManager>();
}

bool ConfigManager::load() {
	lua_State* L = luaL_newstate();
	if (!L) {
		throw std::ios_base::failure("Failed to allocate memory");
	}

	luaL_openlibs(L);

	if (luaL_dofile(L, configFileLua.c_str())) {
		g_logger().error("[ConfigManager::load] - {}", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	// Parse config
	// Info that must be loaded one time (unless we reset the modules involved)
	if (!loaded) {
		loadBoolConfig(L, BIND_ONLY_GLOBAL_ADDRESS, "bindOnlyGlobalAddress", false);
		loadBoolConfig(L, BOT_DENSITY_CAP_ENABLED, "botDensityCapEnabled", true);
		loadBoolConfig(L, DISABLE_LEGACY_RAIDS, "disableLegacyRaids", false);
		loadBoolConfig(L, OLD_PROTOCOL, "allowOldProtocol", true);
		loadBoolConfig(L, OPTIMIZE_DATABASE, "startupDatabaseOptimization", true);
		loadBoolConfig(L, RANDOM_MONSTER_SPAWN, "randomMonsterSpawn", false);
		loadBoolConfig(L, RESET_SESSIONS_ON_STARTUP, "resetSessionsOnStartup", false);
		loadBoolConfig(L, TOGGLE_MAINTAIN_MODE, "toggleMaintainMode", false);
		loadBoolConfig(L, TOGGLE_MAP_CUSTOM, "toggleMapCustom", true);

		loadFloatConfig(L, HOUSE_PRICE_RENT_MULTIPLIER, "housePriceRentMultiplier", 1.0);
		loadFloatConfig(L, HOUSE_RENT_RATE, "houseRentRate", 1.0);

		loadIntConfig(L, BOT_DENSITY_ANCHOR_CLUSTER_RADIUS, "botDensityAnchorClusterRadius", 50);
		// Ring limits are percentages of botPlayersOnline, truncated to whole bots
		// in BotEngine (pctOfBotTotal). At 500 bots: 0.6/2.0/3.0 -> 3/10/15.
		loadFloatConfig(L, BOT_DENSITY_CAP_INNER_LIMIT_PCT, "botDensityCapInnerLimitPct", 0.6);
		loadIntConfig(L, BOT_DENSITY_CAP_INNER_RADIUS, "botDensityCapInnerRadius", 7);
		loadFloatConfig(L, BOT_DENSITY_CAP_MID_LIMIT_PCT, "botDensityCapMidLimitPct", 2.0);
		loadIntConfig(L, BOT_DENSITY_CAP_MID_RADIUS, "botDensityCapMidRadius", 50);
		loadFloatConfig(L, BOT_DENSITY_CAP_OUTER_LIMIT_PCT, "botDensityCapOuterLimitPct", 3.0);
		loadIntConfig(L, BOT_DENSITY_CAP_OUTER_RADIUS, "botDensityCapOuterRadius", 100);
		loadIntConfig(L, BOT_PLAYERS_ONLINE, "botPlayersOnline", 200);

		// ---- Bot Liveness Pack (see BOT_SYSTEM_DOCS.md) ----
		// POI weights — relative weight when bots roll which POI to walk to.
		// NPC + SHOP rebalanced UP (3->8, 5->10) to make hubs visibly populated.
		loadIntConfig(L, BOT_POI_WEIGHT_DEPOT,            "botPoiWeightDepot",           40);
		loadIntConfig(L, BOT_POI_WEIGHT_DEPOT_OUTSIDE,    "botPoiWeightDepotOutside",    20);
		loadIntConfig(L, BOT_POI_WEIGHT_TEMPLE,           "botPoiWeightTemple",          10);
		loadIntConfig(L, BOT_POI_WEIGHT_BOAT,             "botPoiWeightBoat",            20);
		loadIntConfig(L, BOT_POI_WEIGHT_SHOP,             "botPoiWeightShop",            10);
		loadIntConfig(L, BOT_POI_WEIGHT_NPC,              "botPoiWeightNpc",              8);
		loadIntConfig(L, BOT_POI_WEIGHT_ADVENTURER_STONE, "botPoiWeightAdventurerStone", 10);
		// Top-level activity reroll weights (must sum to 100). HUNT trimmed -10 to feed POI +10.
		loadIntConfig(L, BOT_REROLL_WEIGHT_IDLE,   "botRerollWeightIdle",   15);
		loadIntConfig(L, BOT_REROLL_WEIGHT_POI,    "botRerollWeightPoi",    35);
		loadIntConfig(L, BOT_REROLL_WEIGHT_HUNT,   "botRerollWeightHunt",   25);
		loadIntConfig(L, BOT_REROLL_WEIGHT_TRAVEL, "botRerollWeightTravel", 25);
		// Player-proximity weighting (2026-06-15): bias HIBERNATED bots' next task/location
		// toward routes/towns near real players (or cast-watched bots) so the virtual sim
		// funnels ambient traffic toward players. Balanced defaults; set botProxTravelCatBonus=0
		// to disable just the travel-propensity nudge while keeping destination/POI/hunt bias.
		loadBoolConfig(L, BOT_PROX_WEIGHT_ENABLED,  "botProxWeightEnabled",  true);
		loadBoolConfig(L, BOT_PROX_WEIGHT_AWAKE,    "botProxWeightAwake",    false);
		loadIntConfig(L, BOT_PROX_BASELINE_WEIGHT,  "botProxBaselineWeight",   1);
		loadIntConfig(L, BOT_PROX_NEAR_TILES,       "botProxNearTiles",      100);
		loadIntConfig(L, BOT_PROX_MID_TILES,        "botProxMidTiles",       350);
		loadIntConfig(L, BOT_PROX_BONUS_NEAR,       "botProxBonusNear",       18);
		loadIntConfig(L, BOT_PROX_BONUS_MID,        "botProxBonusMid",         6);
		loadIntConfig(L, BOT_PROX_SAMPLE_CAP,       "botProxSampleCap",        8);
		loadIntConfig(L, BOT_PROX_TRAVEL_CAT_BONUS, "botProxTravelCatBonus",  25);
		// Dwell durations (seconds). POI dwell widened from [120,600] -> [180,900] so
		// bots linger long enough to be noticed at hubs.
		loadIntConfig(L, BOT_REROLL_COOLDOWN_SEC,   "botRerollCooldownSec",    30);
		loadIntConfig(L, BOT_DWELL_REROLL_MIN_SEC,  "botDwellRerollMinSec",    60);
		loadIntConfig(L, BOT_DWELL_REROLL_MAX_SEC,  "botDwellRerollMaxSec",   300);
		loadIntConfig(L, BOT_DWELL_POI_MIN_SEC,     "botDwellPoiMinSec",      180);
		loadIntConfig(L, BOT_DWELL_POI_MAX_SEC,     "botDwellPoiMaxSec",      900);
		loadIntConfig(L, BOT_DWELL_NPC_MIN_SEC,     "botDwellNpcMinSec",       15);
		loadIntConfig(L, BOT_DWELL_NPC_MAX_SEC,     "botDwellNpcMaxSec",       60);
		loadIntConfig(L, BOT_DWELL_POST_TRAVEL_SEC, "botDwellPostTravelSec",   60);
		// Hunt session duration (2026-06-09): moved from constexpr HUNT_TIME_MIN/MAX in
		// bot_engine.cpp to config so it can be tuned without a rebuild. Defaults 1200/2400
		// (20-40 min) replace the previous 1800/10800 (30 min - 3h) which kept bots stuck
		// underground hunting for hours and starved city visibility.
		loadIntConfig(L, BOT_HUNT_TIME_MIN_SEC,    "botHuntTimeMinSec",    1200);
		loadIntConfig(L, BOT_HUNT_TIME_MAX_SEC,    "botHuntTimeMaxSec",    2400);
		// AdvStone dwells. Chest extended (mode==1) so bots visibly linger at reward chest.
		loadIntConfig(L, BOT_ADV_STONE_DWELL_IDLE_MIN_SEC,  "botAdvStoneDwellIdleMinSec",   60);
		loadIntConfig(L, BOT_ADV_STONE_DWELL_IDLE_MAX_SEC,  "botAdvStoneDwellIdleMaxSec",  300);
		loadIntConfig(L, BOT_ADV_STONE_DWELL_CHEST_MIN_SEC, "botAdvStoneDwellChestMinSec", 300);
		loadIntConfig(L, BOT_ADV_STONE_DWELL_CHEST_MAX_SEC, "botAdvStoneDwellChestMaxSec", 1200);
		loadIntConfig(L, BOT_ADV_STONE_DWELL_DUMMY_MIN_SEC, "botAdvStoneDwellDummyMinSec", 180);
		loadIntConfig(L, BOT_ADV_STONE_DWELL_DUMMY_MAX_SEC, "botAdvStoneDwellDummyMaxSec", 1800);
		// Max % of botPlayersOnline concurrently in chest/dummy sub-activities
		// (truncated to whole bots; 2.5% of 500 -> 12). Enforced at the mode roll
		// in selectAdvStoneSubActivity — capped trips demote to waypoint idling.
		loadFloatConfig(L, BOT_ADV_STONE_CHEST_DUMMY_CAP_PCT, "botAdvStoneChestDummyCapPct", 2.5);
		// Mount activation chance (% per activation). Dead-code at 30 pre-Phase-A;
		// becomes effective once player_storage grant lands.
		loadIntConfig(L, BOT_MOUNT_CHANCE_PCT, "botMountChancePct", 60);
		// Crowd cap: skip POI selection if >= count bots already within radius tiles.
		// Exempt at AdvStone island (chest/dummy/idle legitimately cluster).
		loadIntConfig(L, BOT_POI_CROWD_CAP_COUNT,  "botPoiCrowdCapCount",  3);
		loadIntConfig(L, BOT_POI_CROWD_CAP_RADIUS, "botPoiCrowdCapRadius", 2);
		// Idle turn-in-place via Game::internalCreatureTurn (same as spell-cast).
		loadIntConfig(L, BOT_TURN_IN_PLACE_CHANCE_PCT,    "botTurnInPlaceChancePct",    20);
		loadIntConfig(L, BOT_TURN_IN_PLACE_INTERVAL_TICKS, "botTurnInPlaceIntervalTicks", 25);
		// Mid-walk pause (also applies to city routes; gated against combat/FC/hunt-target).
		// Hard cap per route limits clustering; commit ead32465d removed an earlier flat
		// LOITER_CHANCE=40 because mid-walk pauses looked unnatural — these values are
		// tuned to be qualitatively different (lower probability, shorter cap).
		loadIntConfig(L, BOT_WALK_PAUSE_CHANCE_PCT,    "botWalkPauseChancePct",     2);
		loadIntConfig(L, BOT_WALK_PAUSE_MIN_MS,        "botWalkPauseMinMs",       400);
		loadIntConfig(L, BOT_WALK_PAUSE_MAX_MS,        "botWalkPauseMaxMs",      5000);
		loadIntConfig(L, BOT_WALK_PAUSE_MAX_PER_ROUTE, "botWalkPauseMaxPerRoute",   3);
		// Observed-tier mid-walk pause: longer, more frequent, personality-gated pauses
		// that fire ONLY when a real player or cast-watched bot is on screen. Applies in
		// IDLE/DWELLING/TRAVELING only (never hunting/patrolling/party/combat). See botStartAutoWalk.
		loadIntConfig(L, BOT_WALK_PAUSE_OBSERVED_CHANCE_PCT,    "botWalkPauseObservedChancePct",      12);
		loadIntConfig(L, BOT_WALK_PAUSE_OBSERVED_MIN_MS,        "botWalkPauseObservedMinMs",         500);
		loadIntConfig(L, BOT_WALK_PAUSE_OBSERVED_MAX_MS,        "botWalkPauseObservedMaxMs",       20000);
		loadIntConfig(L, BOT_WALK_PAUSE_OBSERVED_MAX_PER_ROUTE, "botWalkPauseObservedMaxPerRoute",     8);
		// Idle litter drop (drop-only). Chance rolled once per genuine stop, scaled by
		// per-bot fidgetiness and clamped to 95%, capped at one drop per awake session.
		// Item pool = any NPC-buyable item (ItemType.buyPrice) below botFidgetMaxItemValueGp.
		// (BOT_FIDGET_INTERVAL_MIN/MAX_SEC are now unused — kept to not break existing config.)
		loadIntConfig(L, BOT_FIDGET_CHANCE_PCT,        "botFidgetChancePct",        38);
		loadIntConfig(L, BOT_FIDGET_INTERVAL_MIN_SEC,  "botFidgetIntervalMinSec",   60);
		loadIntConfig(L, BOT_FIDGET_INTERVAL_MAX_SEC,  "botFidgetIntervalMaxSec",  300);
		loadIntConfig(L, BOT_FIDGET_MAX_ITEM_VALUE_GP, "botFidgetMaxItemValueGp", 1000);
		// ---- Gang-PK alpha-strike (see .claude/tmp_gang_pk_pzblock_plan.md) ----
		// A few idle bots standing in a PZ jump an exposed victim just outside it: they stage
		// at the PZ edge, step out together, then surround + magic-wall-box the victim and burst
		// it with single-target nukes (never AoE -> no collateral). Very rare, observer-gated,
		// bounded by the existing 5% skull cap. Odds are 1-in-N per eligible scan.
		loadBoolConfig(L, BOT_GANG_ENABLE,           "botGangEnable",           true);
		loadBoolConfig(L, BOT_GANG_REQUIRE_OBSERVER, "botGangRequireObserver",  true);
		loadBoolConfig(L, BOT_GANG_TARGET_PLAYERS,   "botGangTargetPlayers",    true);
		loadIntConfig(L, BOT_GANG_MIN_SIZE,          "botGangMinSize",             2);
		loadIntConfig(L, BOT_GANG_MAX_SIZE,          "botGangMaxSize",             4);
		loadIntConfig(L, BOT_GANG_RECRUIT_RADIUS,    "botGangRecruitRadius",       5);
		loadIntConfig(L, BOT_GANG_VICTIM_BAND,       "botGangVictimBand",          3);
		loadIntConfig(L, BOT_GANG_STAGE_WINDOW_MS,   "botGangStageWindowMs",    2500);
		loadIntConfig(L, BOT_GANG_SCAN_COOLDOWN_MS,  "botGangScanCooldownMs",   4000);
		loadIntConfig(L, BOT_GANG_ODDS_VS_PLAYER,    "botGangOddsVsPlayer",       33);
		loadIntConfig(L, BOT_GANG_ODDS_VS_BOT,       "botGangOddsVsBot",         200);
		loadIntConfig(L, BOT_GANG_WALL_CHANCE_PCT,   "botGangWallChancePct",      80);
		loadIntConfig(L, BOT_GANG_PARALYZE_CHANCE_PCT, "botGangParalyzeChancePct", 60);
		loadIntConfig(L, BOT_GANG_VICTIM_COOLDOWN_SEC, "botGangVictimCooldownSec", 86400);
		// ---- PZ-blocked roaming ----
		// While genuinely pz-locked (real 60s), a bot can't enter depot/temple/boat PZ. It stops
		// rolling HUNT/TRAVEL and instead mills around non-PZ tiles (the depot-roam behavior,
		// anchored anywhere) until the lock clears.
		loadBoolConfig(L, BOT_PZROAM_ENABLE,            "botPzRoamEnable",        true);
		loadIntConfig(L, BOT_PZROAM_INTERVAL_MIN_SEC,   "botPzRoamIntervalMinSec",  20);
		loadIntConfig(L, BOT_PZROAM_INTERVAL_MAX_SEC,   "botPzRoamIntervalMaxSec",  60);
		loadIntConfig(L, BOT_PZROAM_STAY_PCT,           "botPzRoamStayPct",         40);
		// Local chat cadence scaled per-bot by personalitySeed.chattyness() (4-bit field).
		loadIntConfig(L, BOT_CHAT_COOLDOWN_MIN_MS, "botChatCooldownMinMs",  30000);
		loadIntConfig(L, BOT_CHAT_COOLDOWN_MAX_MS, "botChatCooldownMaxMs", 300000);
		// World Chat (channel 3) — global chitchat. Per-bot 20-40 min => ~1 post/min server-wide at 30 awake.
		loadIntConfig(L, BOT_WORLD_CHAT_INTERVAL_MIN_MS, "botWorldChatIntervalMinMs", 1200000);
		loadIntConfig(L, BOT_WORLD_CHAT_INTERVAL_MAX_MS, "botWorldChatIntervalMaxMs", 2400000);
		// Advertising (channel 5) — trade offers. Server-side script enforces 2-min hard cap.
		loadIntConfig(L, BOT_ADVERTISING_INTERVAL_MIN_MS, "botAdvertisingIntervalMinMs", 300000);
		loadIntConfig(L, BOT_ADVERTISING_INTERVAL_MAX_MS, "botAdvertisingIntervalMaxMs", 600000);
		// Anti-repeat ring size (Playerbots-style; Playerbots doesn't have one — their bots visibly repeat).
		loadIntConfig(L, BOT_CHAT_ANTI_REPEAT_RING_SIZE, "botChatAntiRepeatRingSize", 8);
		// Master chat-rate knob — scales overall chat rate without touching per-category percentages.
		// 100 = no-op, 50 = halve, 200 = double. Per-category chance still applies on top.
		loadIntConfig(L, BOT_CHAT_MASTER_CHANCE_PCT, "botChatMasterChancePct", 100);
		loadIntConfig(L, DEPOT_BOXES, "depotBoxes", 20);
		loadIntConfig(L, FREE_DEPOT_LIMIT, "freeDepotLimit", 2000);
		loadIntConfig(L, GAME_PORT, "gameProtocolPort", 7172);
		loadIntConfig(L, LOGIN_PORT, "loginProtocolPort", 7171);
		loadIntConfig(L, MARKET_OFFER_DURATION, "marketOfferDuration", 30 * 24 * 60 * 60);
		loadIntConfig(L, MARKET_REFRESH_PRICES, "marketRefreshPricesInterval", 30);
		loadIntConfig(L, PREMIUM_DEPOT_LIMIT, "premiumDepotLimit", 8000);
		loadIntConfig(L, SQL_PORT, "mysqlPort", 3306);
		loadIntConfig(L, STATUS_PORT, "statusProtocolPort", 7171);

		loadStringConfig(L, AUTH_TYPE, "authType", "password");
		loadStringConfig(L, HOUSE_RENT_PERIOD, "houseRentPeriod", "never");
		loadStringConfig(L, IP, "ip", "127.0.0.1");
		loadStringConfig(L, MAINTAIN_MODE_MESSAGE, "maintainModeMessage", "");
		loadStringConfig(L, MAP_AUTHOR, "mapAuthor", "Eduardo Dantas");
		loadStringConfig(L, MAP_DOWNLOAD_URL, "mapDownloadUrl", "");
		loadStringConfig(L, MAP_NAME, "mapName", "canary");
		loadStringConfig(L, MYSQL_DB, "mysqlDatabase", "canary");
		loadBoolConfig(L, MYSQL_DB_BACKUP, "mysqlDatabaseBackup", false);
		loadStringConfig(L, MYSQL_HOST, "mysqlHost", "127.0.0.1");
		loadStringConfig(L, MYSQL_PASS, "mysqlPass", "");
		loadStringConfig(L, MYSQL_SOCK, "mysqlSock", "");
		loadStringConfig(L, MYSQL_USER, "mysqlUser", "root");
	}

	loadBoolConfig(L, AIMBOT_HOTKEY_ENABLED, "hotkeyAimbotEnabled", true);
	loadBoolConfig(L, ALLOW_CHANGEOUTFIT, "allowChangeOutfit", true);
	loadBoolConfig(L, ALLOW_RELOAD, "allowReload", false);
	loadBoolConfig(L, AUTOBANK, "autoBank", false);
	loadBoolConfig(L, AUTOLOOT, "autoLoot", false);
	loadBoolConfig(L, BOT_PLAYERS_SHOW_AS_ONLINE, "botPlayersShowAsOnline", true);
	// Bot Liveness Pack booleans (see BOT_SYSTEM_DOCS.md)
	loadBoolConfig(L, BOT_PERSONALITY_REROLL_ON_RESTART, "botPersonalityRerollOnRestart", true);
	loadBoolConfig(L, BOT_CHAT_VERBOSE_LOG,             "botChatVerboseLog",              false);
	// Whether hibernated bots can post to channels 3 (World Chat) + 5 (Advertising).
	// They never post Local Chat since they're not in the world. Set false to silence all
	// hibernated chat traffic (e.g. for soak testing).
	loadBoolConfig(L, BOT_HIBERNATED_CHAT_ENABLED,      "botHibernatedChatEnabled",       true);
	// Best-effort telemetry writes to bot_chat_emissions / bot_hub_presence_60s (used
	// only to measure chat dup rates offline — never read by runtime logic). Off by
	// default; the chat anti-repeat/throttle is fully in-memory and unaffected.
	loadBoolConfig(L, BOT_TELEMETRY_ENABLED,            "botTelemetryEnabled",            false);
	loadBoolConfig(L, BOOSTED_BOSS_SLOT, "boostedBossSlot", true);
	loadBoolConfig(L, CAST_ENABLED, "castEnabled", false);
	loadBoolConfig(L, CLASSIC_ATTACK_SPEED, "classicAttackSpeed", false);
	loadBoolConfig(L, CLEAN_PROTECTION_ZONES, "cleanProtectionZones", false);
	loadBoolConfig(L, CONVERT_UNSAFE_SCRIPTS, "convertUnsafeScripts", true);
	loadBoolConfig(L, DISABLE_MONSTER_ARMOR, "disableMonsterArmor", false);
	loadBoolConfig(L, DISCORD_SEND_FOOTER, "discordSendFooter", true);
	loadBoolConfig(L, EMOTE_SPELLS, "emoteSpells", false);
	loadBoolConfig(L, ENABLE_PLAYER_PUT_ITEM_IN_AMMO_SLOT, "enablePlayerPutItemInAmmoSlot", false);
	loadBoolConfig(L, ENABLE_SUPPORT_OUTFIT, "enableSupportOutfit", true);
	loadBoolConfig(L, EXPERIENCE_FROM_PLAYERS, "experienceByKillingPlayers", false);
	loadBoolConfig(L, FREE_PREMIUM, "freePremium", false);
	loadBoolConfig(L, GLOBAL_SERVER_SAVE_CLEAN_MAP, "globalServerSaveCleanMap", false);
	loadBoolConfig(L, GLOBAL_SERVER_SAVE_CLOSE, "globalServerSaveClose", false);
	loadBoolConfig(L, GLOBAL_SERVER_SAVE_NOTIFY_MESSAGE, "globalServerSaveNotifyMessage", true);
	loadBoolConfig(L, GLOBAL_SERVER_SAVE_SHUTDOWN, "globalServerSaveShutdown", true);
	loadBoolConfig(L, HOUSE_OWNED_BY_ACCOUNT, "houseOwnedByAccount", false);
	loadBoolConfig(L, HOUSE_PURSHASED_SHOW_PRICE, "housePurchasedShowPrice", false);
	loadBoolConfig(L, INVENTORY_GLOW, "inventoryGlowOnFiveBless", false);
	loadBoolConfig(L, LOYALTY_ENABLED, "loyaltyEnabled", true);
	loadBoolConfig(L, MARKET_PREMIUM, "premiumToCreateMarketOffer", true);
	loadBoolConfig(L, METRICS_ENABLE_OSTREAM, "metricsEnableOstream", false);
	loadBoolConfig(L, METRICS_ENABLE_PROMETHEUS, "metricsEnablePrometheus", false);
	loadBoolConfig(L, ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, "onlyInvitedCanMoveHouseItems", true);
	loadBoolConfig(L, ONLY_PREMIUM_ACCOUNT, "onlyPremiumAccount", false);
	loadBoolConfig(L, PARTY_AUTO_SHARE_EXPERIENCE, "partyAutoShareExperience", true);
	loadBoolConfig(L, PARTY_SHARE_LOOT_BOOSTS, "partyShareLootBoosts", true);
	loadBoolConfig(L, PLAYER_LOSE_ITEMS_ON_DEATH, "playerLoseItemsOnDeath", true);
	loadBoolConfig(L, PREY_ENABLED, "preySystemEnabled", true);
	loadBoolConfig(L, PREY_FREE_THIRD_SLOT, "preyFreeThirdSlot", false);
	loadBoolConfig(L, PUSH_WHEN_ATTACKING, "pushWhenAttacking", false);
	loadBoolConfig(L, RATE_USE_STAGES, "rateUseStages", false);
	loadBoolConfig(L, REFUND_BEGINNING_WEAPON_MANA, "refundBeginningWeaponMana", false);
	loadBoolConfig(L, REMOVE_BEGINNING_WEAPON_AMMO, "removeBeginningWeaponAmmunition", true);
	loadBoolConfig(L, REMOVE_POTION_CHARGES, "removeChargesFromPotions", true);
	loadBoolConfig(L, REMOVE_RUNE_CHARGES, "removeChargesFromRunes", true);
	loadBoolConfig(L, REMOVE_WEAPON_AMMO, "removeWeaponAmmunition", true);
	loadBoolConfig(L, REMOVE_WEAPON_CHARGES, "removeWeaponCharges", true);
	loadBoolConfig(L, REPLACE_KICK_ON_LOGIN, "replaceKickOnLogin", true);
	loadBoolConfig(L, REWARD_CHEST_COLLECT_ENABLED, "rewardChestCollectEnabled", true);
	loadBoolConfig(L, SCRIPTS_CONSOLE_LOGS, "showScriptsLogInConsole", true);
	loadBoolConfig(L, SHOW_LOOTS_IN_BESTIARY, "showLootsInBestiary", false);
	loadBoolConfig(L, SKULLED_DEATH_LOSE_STORE_ITEM, "skulledDeathLoseStoreItem", false);
	loadBoolConfig(L, SORT_LOOT_BY_CHANCE, "sortLootByChance", false);
	loadBoolConfig(L, STAMINA_PZ, "staminaPz", false);
	loadBoolConfig(L, STAMINA_SYSTEM, "staminaSystem", true);
	loadBoolConfig(L, STAMINA_TRAINER, "staminaTrainer", false);
	loadBoolConfig(L, STASH_MOVING, "stashMoving", false);
	loadIntConfig(L, STASH_MANAGE_AMOUNT, "stashCountByEachTime", 100000);
	loadBoolConfig(L, TASK_HUNTING_ENABLED, "taskHuntingSystemEnabled", true);
	loadBoolConfig(L, TASK_HUNTING_FREE_THIRD_SLOT, "taskHuntingFreeThirdSlot", false);
	loadBoolConfig(L, TELEPORT_PLAYER_TO_VOCATION_ROOM, "teleportPlayerToVocationRoom", true);
	loadBoolConfig(L, TELEPORT_SUMMONS, "teleportSummons", false);
	loadBoolConfig(L, TOGGLE_CHAIN_SYSTEM, "toggleChainSystem", true);
	loadBoolConfig(L, TOGGLE_DOWNLOAD_MAP, "toggleDownloadMap", false);
	loadBoolConfig(L, TOGGLE_FREE_QUEST, "toggleFreeQuest", true);
	loadBoolConfig(L, TOGGLE_GOLD_POUCH_ALLOW_ANYTHING, "toggleGoldPouchAllowAnything", false);
	loadBoolConfig(L, TOGGLE_GOLD_POUCH_QUICKLOOT_ONLY, "toggleGoldPouchQuickLootOnly", false);
	loadBoolConfig(L, TOGGLE_HAZARDSYSTEM, "toogleHazardSystem", true);
	loadBoolConfig(L, TOGGLE_HOUSE_TRANSFER_ON_SERVER_RESTART, "togglehouseTransferOnRestart", false);
	loadBoolConfig(L, TOGGLE_IMBUEMENT_NON_AGGRESSIVE_FIGHT_ONLY, "toggleImbuementNonAggressiveFightOnly", false);
	loadBoolConfig(L, TOGGLE_IMBUEMENT_SHRINE_STORAGE, "toggleImbuementShrineStorage", true);
	loadBoolConfig(L, TOGGLE_MOUNT_IN_PZ, "toggleMountInProtectionZone", false);
	loadBoolConfig(L, TOGGLE_RECEIVE_REWARD, "toggleReceiveReward", false);
	loadBoolConfig(L, TOGGLE_SAVE_ASYNC, "toggleSaveAsync", false);
	loadBoolConfig(L, TOGGLE_SAVE_INTERVAL_CLEAN_MAP, "toggleSaveIntervalCleanMap", false);
	loadBoolConfig(L, TOGGLE_SAVE_INTERVAL, "toggleSaveInterval", false);
	loadBoolConfig(L, TOGGLE_SERVER_IS_RETRO, "toggleServerIsRetroPVP", false);
	loadBoolConfig(L, TOGGLE_TRAVELS_FREE, "toggleTravelsFree", false);
	loadBoolConfig(L, TOGGLE_WHEELSYSTEM, "wheelSystemEnabled", true);
	loadBoolConfig(L, USE_ANY_DATAPACK_FOLDER, "useAnyDatapackFolder", false);
	loadBoolConfig(L, VIP_AUTOLOOT_VIP_ONLY, "vipAutoLootVipOnly", false);
	loadBoolConfig(L, VIP_KEEP_HOUSE, "vipKeepHouse", false);
	loadBoolConfig(L, VIP_STAY_ONLINE, "vipStayOnline", false);
	loadBoolConfig(L, VIP_SYSTEM_ENABLED, "vipSystemEnabled", false);
	loadBoolConfig(L, WARN_UNSAFE_SCRIPTS, "warnUnsafeScripts", true);
	loadBoolConfig(L, XP_DISPLAY_MODE, "experienceDisplayRates", true);
	loadBoolConfig(L, CYCLOPEDIA_HOUSE_AUCTION, "toggleCyclopediaHouseAuction", true);
	loadBoolConfig(L, LEAVE_PARTY_ON_DEATH, "leavePartyOnDeath", false);

	loadFloatConfig(L, BESTIARY_RATE_CHARM_SHOP_PRICE, "bestiaryRateCharmShopPrice", 1.0);
	loadFloatConfig(L, COMBAT_CHAIN_SKILL_FORMULA_AXE, "combatChainSkillFormulaAxe", 0.9);
	loadFloatConfig(L, COMBAT_CHAIN_SKILL_FORMULA_CLUB, "combatChainSkillFormulaClub", 0.7);
	loadFloatConfig(L, COMBAT_CHAIN_SKILL_FORMULA_SWORD, "combatChainSkillFormulaSword", 1.1);
	loadFloatConfig(L, COMBAT_CHAIN_SKILL_FORMULA_FIST, "combatChainSkillFormulaFist", 1.1);
	loadFloatConfig(L, FORGE_AMOUNT_MULTIPLIER, "forgeAmountMultiplier", 3.0);
	loadFloatConfig(L, HAZARD_EXP_BONUS_MULTIPLIER, "hazardExpBonusMultiplier", 2.0);
	loadFloatConfig(L, LOYALTY_BONUS_PERCENTAGE_MULTIPLIER, "loyaltyBonusPercentageMultiplier", 1.0);
	loadFloatConfig(L, MOMENTUM_CHANCE_FORMULA_A, "momentumChanceFormulaA", 0.05);
	loadFloatConfig(L, MOMENTUM_CHANCE_FORMULA_B, "momentumChanceFormulaB", 1.9);
	loadFloatConfig(L, MOMENTUM_CHANCE_FORMULA_C, "momentumChanceFormulaC", 0.05);
	loadFloatConfig(L, ONSLAUGHT_CHANCE_FORMULA_A, "onslaughtChanceFormulaA", 0.05);
	loadFloatConfig(L, ONSLAUGHT_CHANCE_FORMULA_B, "onslaughtChanceFormulaB", 0.4);
	loadFloatConfig(L, ONSLAUGHT_CHANCE_FORMULA_C, "onslaughtChanceFormulaC", 0.05);
	loadFloatConfig(L, PARTY_SHARE_LOOT_BOOSTS_DIMINISHING_FACTOR, "partyShareLootBoostsDimishingFactor", 0.7f);
	loadFloatConfig(L, PVP_RATE_DAMAGE_REDUCTION_PER_LEVEL, "pvpRateDamageReductionPerLevel", 0.0);
	loadFloatConfig(L, PVP_RATE_DAMAGE_TAKEN_PER_LEVEL, "pvpRateDamageTakenPerLevel", 0.0);
	loadFloatConfig(L, RATE_ATTACK_SPEED, "rateAttackSpeed", 1.0);
	loadFloatConfig(L, RATE_BOSS_ATTACK, "rateBossAttack", 1.0);
	loadFloatConfig(L, RATE_BOSS_DEFENSE, "rateBossDefense", 1.0);
	loadFloatConfig(L, RATE_BOSS_HEALTH, "rateBossHealth", 1.0);
	loadFloatConfig(L, RATE_EXERCISE_TRAINING_SPEED, "rateExerciseTrainingSpeed", 1.0);
	loadFloatConfig(L, RATE_HEALTH_REGEN_SPEED, "rateHealthRegenSpeed", 1.0);
	loadFloatConfig(L, RATE_HEALTH_REGEN, "rateHealthRegen", 1.0);
	loadFloatConfig(L, RATE_MANA_REGEN_SPEED, "rateManaRegenSpeed", 1.0);
	loadFloatConfig(L, RATE_MANA_REGEN, "rateManaRegen", 1.0);
	loadFloatConfig(L, RATE_MONSTER_ATTACK, "rateMonsterAttack", 1.0);
	loadFloatConfig(L, RATE_MONSTER_DEFENSE, "rateMonsterDefense", 1.0);
	loadFloatConfig(L, RATE_MONSTER_HEALTH, "rateMonsterHealth", 1.0);
	loadFloatConfig(L, RATE_NPC_HEALTH, "rateNpcHealth", 1.0);
	loadFloatConfig(L, RATE_OFFLINE_TRAINING_SPEED, "rateOfflineTrainingSpeed", 1.0);
	loadFloatConfig(L, RATE_SOUL_REGEN_SPEED, "rateSoulRegenSpeed", 1.0);
	loadFloatConfig(L, RATE_SOUL_REGEN, "rateSoulRegen", 1.0);
	loadFloatConfig(L, RATE_SPELL_COOLDOWN, "rateSpellCooldown", 1.0);
	loadFloatConfig(L, RUSE_CHANCE_FORMULA_A, "ruseChanceFormulaA", 0.0307576);
	loadFloatConfig(L, RUSE_CHANCE_FORMULA_B, "ruseChanceFormulaB", 0.440697);
	loadFloatConfig(L, RUSE_CHANCE_FORMULA_C, "ruseChanceFormulaC", 0.026);
	loadFloatConfig(L, TRANSCENDENCE_CHANCE_FORMULA_A, "transcendanceChanceFormulaA", 0.0127);
	loadFloatConfig(L, TRANSCENDENCE_CHANCE_FORMULA_B, "transcendanceChanceFormulaB", 0.1070);
	loadFloatConfig(L, TRANSCENDENCE_CHANCE_FORMULA_C, "transcendanceChanceFormulaC", 0.0073);
	loadFloatConfig(L, AMPLIFICATION_CHANCE_FORMULA_A, "amplificationChanceFormulaA", 0.4);
	loadFloatConfig(L, AMPLIFICATION_CHANCE_FORMULA_B, "amplificationChanceFormulaB", 1.7);
	loadFloatConfig(L, AMPLIFICATION_CHANCE_FORMULA_C, "amplificationChanceFormulaC", 0.4);

	loadFloatConfig(L, ANIMUS_MASTERY_MAX_MONSTER_XP_MULTIPLIER, "animusMasteryMaxMonsterXpMultiplier", 4.0);
	loadFloatConfig(L, ANIMUS_MASTERY_MONSTER_XP_MULTIPLIER, "animusMasteryMonsterXpMultiplier", 2.0);
	loadFloatConfig(L, ANIMUS_MASTERY_MONSTERS_XP_MULTIPLIER, "animusMasteryMonstersXpMultiplier", 0.1);

	loadIntConfig(L, ACTIONS_DELAY_INTERVAL, "timeBetweenActions", 200);
	loadIntConfig(L, ADVENTURERSBLESSING_LEVEL, "adventurersBlessingLevel", 21);
	loadIntConfig(L, BESTIARY_KILL_MULTIPLIER, "bestiaryKillMultiplier", 1);
	loadIntConfig(L, BLACK_SKULL_DURATION, "blackSkullDuration", 45);
	loadIntConfig(L, BOOSTED_BOSS_KILL_BONUS, "boostedBossKillBonus", 3);
	loadIntConfig(L, BOOSTED_BOSS_LOOT_BONUS, "boostedBossLootBonus", 250);
	loadIntConfig(L, BOSS_DEFAULT_TIME_TO_DEFEAT, "bossDefaultTimeToDefeat", 20 * 60);
	loadIntConfig(L, BOSS_DEFAULT_TIME_TO_FIGHT_AGAIN, "bossDefaultTimeToFightAgain", 20 * 60 * 60);
	loadIntConfig(L, BOSSTIARY_KILL_MULTIPLIER, "bosstiaryKillMultiplier", 1);
	loadIntConfig(L, BUY_AOL_COMMAND_FEE, "buyAolCommandFee", 0);
	loadIntConfig(L, BUY_BLESS_COMMAND_FEE, "buyBlessCommandFee", 0);
	loadIntConfig(L, CAST_MAX_VIEWERS, "castMaxViewers", 50);
	loadIntConfig(L, CAST_MAX_VIEWERS_PER_IP, "castMaxViewersPerIp", 3);
	loadIntConfig(L, CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES, "checkExpiredMarketOffersEachMinutes", 60);
	loadIntConfig(L, COMBAT_CHAIN_DELAY, "combatChainDelay", 50);
	loadIntConfig(L, COMBAT_CHAIN_TARGETS, "combatChainTargets", 5);
	loadIntConfig(L, COMPRESSION_LEVEL, "packetCompressionLevel", 6);
	loadIntConfig(L, CRITICALCHANCE, "criticalChance", 10);
	loadIntConfig(L, DAY_KILLS_TO_RED, "dayKillsToRedSkull", 3);
	loadIntConfig(L, DEATH_LOSE_PERCENT, "deathLosePercent", -1);
	loadIntConfig(L, DEFAULT_RESPAWN_TIME, "defaultRespawnTime", 60);
	loadIntConfig(L, DEFAULT_DESPAWNRADIUS, "deSpawnRadius", 50);
	loadIntConfig(L, DEFAULT_DESPAWNRANGE, "deSpawnRange", 2);
	loadIntConfig(L, DEPOTCHEST, "depotChest", 4);
	loadIntConfig(L, DISCORD_WEBHOOK_DELAY_MS, "discordWebhookDelayMs", Webhook::DEFAULT_DELAY_MS);
	loadIntConfig(L, EX_ACTIONS_DELAY_INTERVAL, "timeBetweenExActions", 1000);
	loadIntConfig(L, EXP_FROM_PLAYERS_LEVEL_RANGE, "expFromPlayersLevelRange", 75);
	loadIntConfig(L, FAMILIAR_TIME, "familiarTime", 30);
	loadIntConfig(L, FORGE_BASE_SUCCESS_RATE, "forgeBaseSuccessRate", 50);
	loadIntConfig(L, FORGE_BONUS_SUCCESS_RATE, "forgeBonusSuccessRate", 15);
	loadIntConfig(L, FORGE_CONVERGENCE_FUSION_DUST_COST, "forgeConvergenceFusionDustCost", 130);
	loadIntConfig(L, FORGE_CONVERGENCE_TRANSFER_DUST_COST, "forgeConvergenceTransferCost", 160);
	loadIntConfig(L, FORGE_CORE_COST, "forgeCoreCost", 50);
	loadIntConfig(L, FORGE_COST_ONE_SLIVER, "forgeCostOneSliver", 20);
	loadIntConfig(L, FORGE_FIENDISH_CREATURES_LIMIT, "forgeFiendishLimit", 3);
	loadIntConfig(L, FORGE_FUSION_DUST_COST, "forgeFusionDustCost", 100);
	loadIntConfig(L, FORGE_INFLUENCED_CREATURES_LIMIT, "forgeInfluencedLimit", 300);
	loadIntConfig(L, FORGE_MAX_DUST, "forgeMaxDust", 225);
	loadIntConfig(L, FORGE_MAX_ITEM_TIER, "forgeMaxItemTier", 10);
	loadIntConfig(L, FORGE_MAX_SLIVERS, "forgeMaxSlivers", 7);
	loadIntConfig(L, FORGE_MIN_SLIVERS, "forgeMinSlivers", 3);
	loadIntConfig(L, FORGE_SLIVER_AMOUNT, "forgeSliverAmount", 3);
	loadIntConfig(L, FORGE_TIER_LOSS_REDUCTION, "forgeTierLossReduction", 50);
	loadIntConfig(L, FORGE_TRANSFER_DUST_COST, "forgeTransferDustCost", 100);
	loadIntConfig(L, FRAG_TIME, "timeToDecreaseFrags", 24 * 60 * 60 * 1000);
	loadIntConfig(L, FREE_QUEST_STAGE, "freeQuestStage", 1);
	loadIntConfig(L, GLOBAL_SERVER_SAVE_NOTIFY_DURATION, "globalServerSaveNotifyDuration", 5);
	loadIntConfig(L, HAZARD_CRITICAL_CHANCE, "hazardCriticalChance", 750);
	loadIntConfig(L, HAZARD_CRITICAL_INTERVAL, "hazardCriticalInterval", 2000);
	loadIntConfig(L, HAZARD_CRITICAL_MULTIPLIER, "hazardCriticalMultiplier", 25);
	loadIntConfig(L, HAZARD_DAMAGE_MULTIPLIER, "hazardDamageMultiplier", 200);
	loadIntConfig(L, HAZARD_DEFENSE_MULTIPLIER, "hazardDefenseMultiplier", 0);
	loadIntConfig(L, HAZARD_DODGE_MULTIPLIER, "hazardDodgeMultiplier", 85);
	loadIntConfig(L, HAZARD_LOOT_BONUS_MULTIPLIER, "hazardLootBonusMultiplier", 2);
	loadIntConfig(L, HAZARD_PODS_DAMAGE, "hazardPodsDamage", 5);
	loadIntConfig(L, HAZARD_PODS_DROP_MULTIPLIER, "hazardPodsDropMultiplier", 87);
	loadIntConfig(L, HAZARD_PODS_TIME_TO_DAMAGE, "hazardPodsTimeToDamage", 2000);
	loadIntConfig(L, HAZARD_PODS_TIME_TO_SPAWN, "hazardPodsTimeToSpawn", 4000);
	loadIntConfig(L, HAZARD_SPAWN_PLUNDER_MULTIPLIER, "hazardSpawnPlunderMultiplier", 25);
	loadIntConfig(L, DAYS_TO_CLOSE_BID, "daysToCloseBid", 7);
	loadIntConfig(L, HOUSE_BUY_LEVEL, "houseBuyLevel", 0);
	loadIntConfig(L, HOUSE_LOSE_AFTER_INACTIVITY, "houseLoseAfterInactivity", 0);
	loadIntConfig(L, HOUSE_PRICE_PER_SQM, "housePriceEachSQM", 1000);
	loadIntConfig(L, KICK_AFTER_MINUTES, "kickIdlePlayerAfterMinutes", 15);
	loadIntConfig(L, LOOTPOUCH_MAXLIMIT, "lootPouchMaxLimit", 2000);
	loadIntConfig(L, LOW_LEVEL_BONUS_EXP, "lowLevelBonusExp", 50);
	loadIntConfig(L, LOYALTY_POINTS_PER_CREATION_DAY, "loyaltyPointsPerCreationDay", 1);
	loadIntConfig(L, LOYALTY_POINTS_PER_PREMIUM_DAY_PURCHASED, "loyaltyPointsPerPremiumDayPurchased", 0);
	loadIntConfig(L, LOYALTY_POINTS_PER_PREMIUM_DAY_SPENT, "loyaltyPointsPerPremiumDaySpent", 0);
	loadIntConfig(L, MAX_ALLOWED_ON_A_DUMMY, "maxAllowedOnADummy", 1);
	loadIntConfig(L, MAX_CONTAINER_ITEM, "maxItem", 5000);
	loadIntConfig(L, MAX_CONTAINER, "maxContainer", 500);
	loadIntConfig(L, MAX_CONTAINER_DEPTH, "maxContainerDepth", 200);
	loadIntConfig(L, MAX_INBOX_ITEMS, "maxInboxItems", 0);
	loadIntConfig(L, MAX_DAMAGE_REFLECTION, "maxDamageReflection", 200);
	loadIntConfig(L, MAX_ELEMENTAL_RESISTANCE, "maxElementalResistance", 200);
	loadIntConfig(L, MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER, "maxMarketOffersAtATimePerPlayer", 100);
	loadIntConfig(L, MAX_MESSAGEBUFFER, "maxMessageBuffer", 4);
	loadIntConfig(L, MAX_PACKETS_PER_SECOND, "maxPacketsPerSecond", 25);
	loadIntConfig(L, MAX_PLAYERS_OUTSIDE_PZ_PER_ACCOUNT, "maxPlayersOutsidePZPerAccount", 1);
	loadIntConfig(L, MAX_PLAYERS_PER_ACCOUNT, "maxPlayersOnlinePerAccount", 1);
	loadIntConfig(L, MAX_PLAYERS, "maxPlayers", 0);
	loadIntConfig(L, METRICS_OSTREAM_INTERVAL, "metricsOstreamInterval", 1000);
	loadIntConfig(L, MIN_DELAY_BETWEEN_CONDITIONS, "minDelayBetweenConditions", 0);
	loadIntConfig(L, MIN_ELEMENTAL_RESISTANCE, "minElementalResistance", -200);
	loadIntConfig(L, MIN_TOWN_ID_TO_BANK_TRANSFER_FROM_MAIN, "minTownIdToBankTransferFromMain", 4);
	loadIntConfig(L, MONTH_KILLS_TO_RED, "monthKillsToRedSkull", 10);
	loadIntConfig(L, ORANGE_SKULL_DURATION, "orangeSkullDuration", 7);
	loadIntConfig(L, LOGIN_PROTECTION_TIME, "loginProtectionTime", 10000);
	loadIntConfig(L, PARALLELISM, "parallelism", 2);
	loadIntConfig(L, PARTY_LIST_MAX_DISTANCE, "partyListMaxDistance", 0);
	loadIntConfig(L, PREY_BONUS_REROLL_PRICE, "preyBonusRerollPrice", 1);
	loadIntConfig(L, PREY_BONUS_TIME, "preyBonusTime", 7200);
	loadIntConfig(L, PREY_FREE_REROLL_TIME, "preyFreeRerollTime", 72000);
	loadIntConfig(L, PREY_REROLL_PRICE_LEVEL, "preyRerollPricePerLevel", 200);
	loadIntConfig(L, PREY_SELECTION_LIST_PRICE, "preySelectListPrice", 5);
	loadIntConfig(L, PROTECTION_LEVEL, "protectionLevel", 1);
	loadIntConfig(L, PUSH_DELAY, "pushDelay", 1000);
	loadIntConfig(L, PUSH_DISTANCE_DELAY, "pushDistanceDelay", 1500);
	loadIntConfig(L, PVP_MAX_LEVEL_DIFFERENCE, "pvpMaxLevelDifference", 0);
	loadIntConfig(L, PZ_LOCKED, "pzLocked", 60000);
	loadIntConfig(L, RATE_EXPERIENCE, "rateExp", 1);
	loadIntConfig(L, RATE_KILLING_IN_THE_NAME_OF_POINTS, "rateKillingInTheNameOfPoints", 1);
	loadIntConfig(L, RATE_LOOT, "rateLoot", 1);
	loadIntConfig(L, RATE_MAGIC, "rateMagic", 1);
	loadIntConfig(L, RATE_SKILL, "rateSkill", 1);
	loadIntConfig(L, RATE_SPAWN, "rateSpawn", 1);
	loadIntConfig(L, RED_SKULL_DURATION, "redSkullDuration", 30);
	loadIntConfig(L, REWARD_CHEST_MAX_COLLECT_ITEMS, "rewardChestMaxCollectItems", 200);
	loadIntConfig(L, SAVE_INTERVAL_TIME, "saveIntervalTime", 1);
	loadIntConfig(L, STAIRHOP_DELAY, "stairJumpExhaustion", 2000);
	loadIntConfig(L, STAMINA_GREEN_DELAY, "staminaGreenDelay", 5);
	loadIntConfig(L, STAMINA_ORANGE_DELAY, "staminaOrangeDelay", 1);
	loadIntConfig(L, STAMINA_PZ_GAIN, "staminaPzGain", 1);
	loadIntConfig(L, STAMINA_TRAINER_DELAY, "staminaTrainerDelay", 5);
	loadIntConfig(L, STAMINA_TRAINER_GAIN, "staminaTrainerGain", 1);
	loadFloatConfig(L, PARTY_SHARE_RANGE_MULTIPLIER, "partyShareRangeMultiplier", 1.5f);
	loadIntConfig(L, START_STREAK_LEVEL, "startStreakLevel", 0);
	loadIntConfig(L, STATUSQUERY_TIMEOUT, "statusTimeout", 5000);
	loadIntConfig(L, STORE_COIN_PACKET, "coinPacketSize", 25);
	loadIntConfig(L, STOREINBOX_MAXLIMIT, "storeInboxMaxLimit", 2000);
	loadIntConfig(L, T_CONST, "temporaryConst", 2);
	loadIntConfig(L, TASK_HUNTING_BONUS_REROLL_PRICE, "taskHuntingBonusRerollPrice", 1);
	loadIntConfig(L, TASK_HUNTING_FREE_REROLL_TIME, "taskHuntingFreeRerollTime", 72000);
	loadIntConfig(L, TASK_HUNTING_LIMIT_EXHAUST, "taskHuntingLimitedTasksExhaust", 72000);
	loadIntConfig(L, TASK_HUNTING_REROLL_PRICE_LEVEL, "taskHuntingRerollPricePerLevel", 200);
	loadIntConfig(L, TASK_HUNTING_SELECTION_LIST_PRICE, "taskHuntingSelectListPrice", 5);
	loadIntConfig(L, TIBIADROME_CONCOCTION_COOLDOWN, "tibiadromeConcoctionCooldown", 24 * 60 * 60);
	loadIntConfig(L, TIBIADROME_CONCOCTION_DURATION, "tibiadromeConcoctionDuration", 1 * 60 * 60);
	loadIntConfig(L, TRANSCENDENCE_AVATAR_DURATION, "transcendenceAvatarDuration", 7000);
	loadIntConfig(L, VIP_BONUS_EXP, "vipBonusExp", 0);
	loadIntConfig(L, VIP_BONUS_LOOT, "vipBonusLoot", 0);
	loadIntConfig(L, VIP_BONUS_SKILL, "vipBonusSkill", 0);
	loadIntConfig(L, VIP_FAMILIAR_TIME_COOLDOWN_REDUCTION, "vipFamiliarTimeCooldownReduction", 0);
	loadIntConfig(L, WEEK_KILLS_TO_RED, "weekKillsToRedSkull", 5);
	loadIntConfig(L, WHEEL_ATELIER_REVEAL_GREATER_COST, "wheelAtelierRevealGreaterCost", 6000000);
	loadIntConfig(L, WHEEL_ATELIER_REVEAL_LESSER_COST, "wheelAtelierRevealLesserCost", 125000);
	loadIntConfig(L, WHEEL_ATELIER_REVEAL_REGULAR_COST, "wheelAtelierRevealRegularCost", 1000000);
	loadIntConfig(L, WHEEL_ATELIER_ROTATE_GREATER_COST, "wheelAtelierRotateGreaterCost", 500000);
	loadIntConfig(L, WHEEL_ATELIER_ROTATE_LESSER_COST, "wheelAtelierRotateLesserCost", 125000);
	loadIntConfig(L, WHEEL_ATELIER_ROTATE_REGULAR_COST, "wheelAtelierRotateRegularCost", 250000);
	loadIntConfig(L, WHEEL_POINTS_PER_LEVEL, "wheelPointsPerLevel", 1);
	loadIntConfig(L, WHITE_SKULL_TIME, "whiteSkullTime", 15 * 60 * 1000);
	loadIntConfig(L, AUGMENT_INCREASED_DAMAGE_PERCENT, "augmentIncreasedDamagePercent", 5);
	loadIntConfig(L, AUGMENT_POWERFUL_IMPACT_PERCENT, "augmentPowerfulImpactPercent", 10);
	loadIntConfig(L, AUGMENT_STRONG_IMPACT_PERCENT, "augmentStrongImpactPercent", 7);
	loadIntConfig(L, ANIMUS_MASTERY_MONSTERS_TO_INCREASE_XP_MULTIPLIER, "animusMasteryMonstersToIncreaseXpMultiplier", 10);

	loadStringConfig(L, CORE_DIRECTORY, "coreDirectory", "data");
	loadStringConfig(L, DATA_DIRECTORY, "dataPackDirectory", "data-otservbr-global");
	loadStringConfig(L, DEFAULT_PRIORITY, "defaultPriority", "high");
	loadStringConfig(L, DISCORD_WEBHOOK_URL, "discordWebhookURL", "");
	loadStringConfig(L, FORGE_FIENDISH_INTERVAL_TIME, "forgeFiendishIntervalTime", "1");
	loadStringConfig(L, FORGE_FIENDISH_INTERVAL_TYPE, "forgeFiendishIntervalType", "hour");
	loadStringConfig(L, GLOBAL_SERVER_SAVE_TIME, "globalServerSaveTime", "06:00");
	loadStringConfig(L, LOCATION, "location", "");
	loadStringConfig(L, M_CONST, "memoryConst", "1<<16");
	loadStringConfig(L, METRICS_PROMETHEUS_ADDRESS, "metricsPrometheusAddress", "localhost:9464");
	loadStringConfig(L, OWNER_EMAIL, "ownerEmail", "");
	loadStringConfig(L, OWNER_NAME, "ownerName", "");
	loadStringConfig(L, SAVE_INTERVAL_TYPE, "saveIntervalType", "");
	loadStringConfig(L, SERVER_MOTD, "serverMotd", "");
	loadStringConfig(L, SERVER_NAME, "serverName", "");
	loadStringConfig(L, STORE_IMAGES_URL, "coinImagesURL", "");
	loadStringConfig(L, TIBIADROME_CONCOCTION_TICK_TYPE, "tibiadromeConcoctionTickType", "online");
	loadStringConfig(L, URL, "url", "");
	loadStringConfig(L, WORLD_TYPE, "worldType", "pvp");
	loadStringConfig(L, LOGLEVEL, "logLevel", "info");

	loadLuaOTCFeatures(L);

	loaded = true;
	lua_close(L);
	return true;
}

bool ConfigManager::reload() {
	m_configString.clear();
	m_configInteger.clear();
	m_configBoolean.clear();
	m_configFloat.clear();
	const bool result = load();
	if (transformToSHA1(getString(SERVER_MOTD)) != g_game().getMotdHash()) {
		g_game().incrementMotdNum();
	}
	return result;
}

void ConfigManager::missingConfigWarning(const char* identifier) {
	g_logger().debug("[{}]: Missing configuration for identifier: {}", __FUNCTION__, identifier);
}

std::string ConfigManager::loadStringConfig(lua_State* L, const ConfigKey_t &key, const char* identifier, const std::string &defaultValue) {
	std::string value = defaultValue;
	lua_getglobal(L, identifier);
	if (lua_isstring(L, -1)) {
		value = lua_tostring(L, -1);
	} else {
		missingConfigWarning(identifier);
	}
	configs[key] = value;
	lua_pop(L, 1);
	return value;
}

int32_t ConfigManager::loadIntConfig(lua_State* L, const ConfigKey_t &key, const char* identifier, const int32_t &defaultValue) {
	int32_t value = defaultValue;
	lua_getglobal(L, identifier);
	if (lua_isnumber(L, -1)) {
		value = static_cast<int32_t>(lua_tointeger(L, -1));
	} else {
		missingConfigWarning(identifier);
	}
	configs[key] = value;
	lua_pop(L, 1);
	return value;
}

bool ConfigManager::loadBoolConfig(lua_State* L, const ConfigKey_t &key, const char* identifier, const bool &defaultValue) {
	bool value = defaultValue;
	lua_getglobal(L, identifier);
	if (lua_isboolean(L, -1)) {
		value = static_cast<bool>(lua_toboolean(L, -1));
	} else {
		missingConfigWarning(identifier);
	}
	configs[key] = value;
	lua_pop(L, 1);
	return value;
}

float ConfigManager::loadFloatConfig(lua_State* L, const ConfigKey_t &key, const char* identifier, const float &defaultValue) {
	float value = defaultValue;
	lua_getglobal(L, identifier);
	if (lua_isnumber(L, -1)) {
		value = static_cast<float>(lua_tonumber(L, -1));
	} else {
		missingConfigWarning(identifier);
	}
	configs[key] = value;
	lua_pop(L, 1);
	return value;
}

const std::string &ConfigManager::getString(const ConfigKey_t &key, const std::source_location &location /*= std::source_location::current()*/) const {
	auto itCache = m_configString.find(key);
	if (itCache != m_configString.end()) {
		return itCache->second;
	}

	auto it = configs.find(key);
	if (it != configs.end()) {
		if (const auto* value = std::get_if<std::string>(&it->second)) {
			m_configString[key] = *value;
			return *value;
		}
	}

	static const std::string staticEmptyString;
	g_logger().warn("[{}] accessing invalid or wrong type index: {}[{}]. Called line: {}:{}, in {}", __FUNCTION__, magic_enum::enum_name(key), fmt::underlying(key), location.line(), location.column(), location.function_name());
	return staticEmptyString;
}

int32_t ConfigManager::getNumber(const ConfigKey_t &key, const std::source_location &location /*= std::source_location::current()*/) const {
	auto itCache = m_configInteger.find(key);
	if (itCache != m_configInteger.end()) {
		return itCache->second;
	}

	auto it = configs.find(key);
	if (it != configs.end()) {
		if (std::holds_alternative<int32_t>(it->second)) {
			const auto value = std::get<int32_t>(it->second);
			m_configInteger[key] = value;
			return value;
		}
	}

	g_logger().warn("[{}] accessing invalid or wrong type index: {}[{}]. Called line: {}:{}, in {}", __FUNCTION__, magic_enum::enum_name(key), fmt::underlying(key), location.line(), location.column(), location.function_name());
	return 0;
}

bool ConfigManager::getBoolean(const ConfigKey_t &key, const std::source_location &location /*= std::source_location::current()*/) const {
	auto itCache = m_configBoolean.find(key);
	if (itCache != m_configBoolean.end()) {
		return itCache->second;
	}

	auto it = configs.find(key);
	if (it != configs.end()) {
		if (std::holds_alternative<bool>(it->second)) {
			const auto value = std::get<bool>(it->second);
			m_configBoolean[key] = value;
			return value;
		}
	}

	g_logger().warn("[{}] accessing invalid or wrong type index: {}[{}]. Called line: {}:{}, in {}", __FUNCTION__, magic_enum::enum_name(key), fmt::underlying(key), location.line(), location.column(), location.function_name());
	return false;
}

float ConfigManager::getFloat(const ConfigKey_t &key, const std::source_location &location /*= std::source_location::current()*/) const {
	auto itCache = m_configFloat.find(key);
	if (itCache != m_configFloat.end()) {
		return itCache->second;
	}

	auto it = configs.find(key);
	if (it != configs.end()) {
		if (std::holds_alternative<float>(it->second)) {
			const auto value = std::get<float>(it->second);
			m_configFloat[key] = value;
			return value;
		}
	}

	g_logger().warn("[{}] accessing invalid or wrong type index: {}[{}]. Called line: {}:{}, in {}", __FUNCTION__, magic_enum::enum_name(key), fmt::underlying(key), location.line(), location.column(), location.function_name());
	return 0.0f;
}

void ConfigManager::loadLuaOTCFeatures(lua_State* L) {
	lua_getglobal(L, "OTCRFeatures");
	if (!lua_istable(L, -1)) {
		// Temp to avoid a bug in OTC if the "OTCRFeatures" array is not declared in config.lua.
		enabledFeaturesOTC.push_back(101);
		enabledFeaturesOTC.push_back(102);
		enabledFeaturesOTC.push_back(103);
		enabledFeaturesOTC.push_back(118);
		lua_pop(L, 1);
		return;
	}

	lua_pushstring(L, "enableFeature");
	lua_gettable(L, -2);
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			const auto feature = static_cast<uint8_t>(lua_tointeger(L, -1));
			enabledFeaturesOTC.push_back(feature);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	lua_pushstring(L, "disableFeature");
	lua_gettable(L, -2);
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			const auto feature = static_cast<uint8_t>(lua_tointeger(L, -1));
			disabledFeaturesOTC.push_back(feature);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	lua_pop(L, 1);
}
OTCFeatures ConfigManager::getEnabledFeaturesOTC() const {
	return enabledFeaturesOTC;
}

OTCFeatures ConfigManager::getDisabledFeaturesOTC() const {
	return disabledFeaturesOTC;
}
