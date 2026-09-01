/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// BOT_NAV_REALISM tunables — the ONE place a bot nav key is declared.
//
// Every bot config key used to be written twice: once as a ConfigKey_t enumerator
// in config_enums.hpp and once as a load*Config call in configmanager.cpp. Adding a
// tunable meant editing both, and forgetting the second half compiles cleanly and
// then silently reads 0/false at runtime — a failure that looks like a behavior bug,
// not a build mistake. This table generates both, so the two cannot drift.
//
// Adding a tunable is now: one line here + one line in config.lua. Note that it
// still needs a FULL rebuild (this header feeds the main binary's enum), which is
// the accepted trade for config.lua being the single source of truth — see the
// "Bot config lives ONLY in config.lua" rule in .claude/CLAUDE.md.
//
// Defaults are OFF/neutral by design: the Phase 1.5 registration rebuild changed no
// behavior, and later .so-only phases flip values in config.lua without a rebuild.
// ============================================================================

// INT(enum, luaKey, default) / BOOL(enum, luaKey, default) / STR(enum, luaKey, default)
#define BOT_NAV_REALISM_CONFIG_KEYS(INT, BOOL, STR)                                  \
	/* Phase 4a: per-bot A* cost jitter mask (0 = off, clamped to <= 7) */            \
	INT (BOT_NAV_JITTER_MASK,              "botNavJitterMask",              0)        \
	/* Phase 7: persistent per-route lane offset */                                   \
	BOOL(BOT_LANE_ENABLE,                  "botLaneEnable",                 false)    \
	/* Phase 7: waypoints to hold center after a lane-recovery trigger */             \
	INT (BOT_LANE_RECOVERY_WPS,            "botLaneRecoveryWps",            4)        \
	/* Phase 6: closed-loop awake-tick pathfind/scan/plan budget */                   \
	BOOL(BOT_AWAKE_BUDGET_ENABLE,          "botAwakeBudgetEnable",          false)    \
	/* Phase 6: served PATHFIND requests/tick at the >30ms load tier */               \
	INT (BOT_AWAKE_PATHFIND_PER_TICK_MID,  "botAwakePathfindPerTickMid",    8)        \
	/* Phase 6: served PATHFIND requests/tick at the >60ms load tier */               \
	INT (BOT_AWAKE_PATHFIND_PER_TICK_HIGH, "botAwakePathfindPerTickHigh",   3)        \
	/* Phase 6: % of unobserved bots running SCAN/PLAN per window (>60ms tier) */     \
	INT (BOT_AWAKE_ROTATE_PCT,             "botAwakeRotatePct",             50)       \
	/* Phase 6: rotation window length */                                             \
	INT (BOT_AWAKE_ROTATE_WINDOW_SEC,      "botAwakeRotateWindowSec",       30)       \
	/* Phase 8: csv of towns using the nav graph ("" = off, "all" = every town) */    \
	STR (BOT_NAV_GRAPH_TOWNS,              "botNavGraphTowns",              "")       \
	/* Phase 9: chance to re-sample among equal-cost routes at a junction */          \
	INT (BOT_JUNCTION_SWITCH_PCT,          "botJunctionSwitchPct",          20)       \
	/* Phase 10: chance to dwell + look around at a NODE arrival */                   \
	INT (BOT_JITTER_DWELL_PCT,             "botJitterDwellPct",             0)        \
	/* Phase 10: dwell duration min */                                                \
	INT (BOT_JITTER_DWELL_MIN_MS,          "botJitterDwellMinMs",           1000)     \
	/* Phase 10: dwell duration max */                                                \
	INT (BOT_JITTER_DWELL_MAX_MS,          "botJitterDwellMaxMs",           8000)     \
	/* Phase 10: chance to change destination mid-route */                            \
	INT (BOT_JITTER_REROLL_PCT,            "botJitterRerollPct",            0)        \
	/* Phase 10: chance to walk back then resume ("forgot something") */              \
	INT (BOT_JITTER_UTURN_PCT,             "botJitterUturnPct",             0)        \
	/* Phase 10: chance to pause at an NPC a leg passes ("shop window") */            \
	INT (BOT_JITTER_WINDOW_PCT,            "botJitterWindowPct",            0)        \
	/* Roll chance (0-100) that a bot walks to an NPC in its own town. One of two        \
	   automatic consumers of the scoped route planner (the other is botFishPct).        \
	   DEFAULT 0 = structurally unreachable (uniform_random(1,100) is never <= 0),        \
	   so the NPC block costs zero CPU, not merely zero selection odds. Raise only once   \
	   the planner is proven via `/cavebot <bot> goto x,y,z`, which drives the same code. */\
	INT (BOT_NPC_VISIT_PCT,                "botNpcVisitPct",                0)        \
	/* Phase 4b: scatter bots along a route's phase on entry (anti-lockstep) */       \
	BOOL(BOT_ROUTE_PHASE_DESYNC,           "botRoutePhaseDesync",           false)

// ============================================================================
// BOT_SUPPLY_REALISM tunables — potions, food, rune crafting, fishing.
//
// Same X-macro contract as the table above: one line here generates the
// ConfigKey_t enumerator, the configmanager.cpp load call, and the
// `/cavebot botcfg` dump row, so the three can never drift.
//
// Split into its own table purely for readability — these are supply/idle
// behaviours, not navigation. Both tables are expanded side by side at the
// same three sites.
// ============================================================================
#define BOT_ACTIVITY_CONFIG_KEYS(INT, BOOL, STR)                                     \
	/* --- Potions + food: fire in ANY state while the bot is awake --- */            \
	/* chance per eligible check (checks run on the 500ms heal cadence) */            \
	INT (BOT_POTION_CHANCE_PCT,            "botPotionChancePct",            8)        \
	/* per-bot floor between sips, so a chance hit can't machine-gun */               \
	INT (BOT_POTION_MIN_INTERVAL_MS,       "botPotionMinIntervalMs",        20000)    \
	INT (BOT_FOOD_CHANCE_PCT,              "botFoodChancePct",              6)        \
	INT (BOT_FOOD_MIN_INTERVAL_MS,         "botFoodMinIntervalMs",          60000)    \
	/* --- Rune crafting: idle in place >10s, OR fishing. Never while hunting. --- */ \
	INT (BOT_RUNECRAFT_CHANCE_PCT,         "botRuneCraftChancePct",         10)       \
	INT (BOT_RUNECRAFT_INTERVAL_MIN_MS,    "botRuneCraftIntervalMinMs",     8000)     \
	INT (BOT_RUNECRAFT_INTERVAL_MAX_MS,    "botRuneCraftIntervalMaxMs",     25000)    \
	/* top up mana for the conjure — an idle bot's mana is otherwise unused */        \
	BOOL(BOT_RUNECRAFT_REFILL_MANA,        "botRuneCraftRefillMana",        true)     \
	/* --- Fishing: awake, in a town, chosen at reroll like any other POI --- */      \
	/* DEFAULT 0 = structurally unreachable, same rollout as botNpcVisitPct. Prove    \
	   with `/cavebot fishspots` + a manual goto to a stand tile BEFORE raising. */    \
	INT (BOT_FISH_PCT,                     "botFishPct",                    0)        \
	/* relative weight of the water candidate against depot/temple/boat/shop/npc.     \
	   Mirrors botPoiWeightNpc: botFishPct is eligibility, this is the share. */       \
	/* eligibility radius from the bot, matching the planner's own reach */           \
	INT (BOT_FISH_MAX_DIST,                "botFishMaxDist",                150)      \
	INT (BOT_FISH_MAX_DZ,                  "botFishMaxDz",                  7)        \
	/* cast cadence once standing at the shore */                                     \
	INT (BOT_FISH_CAST_INTERVAL_MIN_MS,    "botFishCastIntervalMinMs",      1000)     \
	INT (BOT_FISH_CAST_INTERVAL_MAX_MS,    "botFishCastIntervalMaxMs",      2000)     \
	/* how long one fishing session lasts before the walk home */                     \
	INT (BOT_FISH_DURATION_MIN_SEC,        "botFishDurationMinSec",         120)      \
	INT (BOT_FISH_DURATION_MAX_SEC,        "botFishDurationMaxSec",         420)      \
	/* grid-thinning cap so spots spread along a shore instead of clustering */       \
	INT (BOT_FISH_MAX_SPOTS_PER_TOWN,      "botFishMaxSpotsPerTown",        64)       \
	/* Cross-floor ceiling, in both places a fishing decision can cross a floor:      \
	   the prebuilt index's eligibility filter, and selectFishingSpot's live retry   \
	   on neighbouring floors when the bot's own floor has no water at all (both     \
	   bot_supply.cpp). Effective cap is min(this, botFishMaxDz). Keep this small —  \
	   z8+ shoreline is mostly cave water, and every extra floor also multiplies     \
	   the worst-case cost of that live retry (one more FISH_LOS_PROBE_BUDGET-sized  \
	   scan per floor, both directions). */                                         \
	INT (BOT_FISH_Z_BAND,                  "botFishZBand",                  1)        \
	/* --- Support spells: any awake state EXCEPT hunting (the slot rune crafting     \
	   vacated). Self-target, non-aggressive buffs/heals/cures, filtered per bot by   \
	   vocation, level, cooldown and mana. Fires while WALKING too, which is the      \
	   point — a bot keeping utani hur up on the road is what reads as a player.      \
	                                                                                  \
	   APPEND NEW KEYS AT THE END OF THIS TABLE, never in the middle. The X-macro     \
	   generates the ConfigKey_t enumerators in order, so an insertion shifts the     \
	   VALUE of every key below it. Binary and .so are rebuilt together here, so that \
	   is harmless today — but the documented fast path is a .so-ONLY rebuild, and    \
	   after a mid-table insert a .so-only build would leave the two disagreeing and  \
	   every shifted key would silently read its neighbour's value. Appended keys     \
	   degrade to "the new key reads 0" instead, which is loud and obvious. */        \
	INT (BOT_SUPPORT_SPELL_CHANCE_PCT,      "botSupportSpellChancePct",      35)      \
	INT (BOT_SUPPORT_SPELL_INTERVAL_MIN_MS, "botSupportSpellIntervalMinMs",  15000)   \
	INT (BOT_SUPPORT_SPELL_INTERVAL_MAX_MS, "botSupportSpellIntervalMaxMs",  45000)   \
	/* --- Quests. Not hunts: a quest is a linear one-shot walkthrough that runs      \
	   travel_to -> hunt_patrol -> travel_from once, and it is SHARED between bots    \
	   rather than 1-bot-reserved like a hunt spawn. The cooldown gates how soon the  \
	   next bot may START the same quest; it does not prevent overlap, since these    \
	   walkthroughs run 5-15+ minutes. Neither existing table above is a thematic fit \
	   for quest scheduling, so these live here as a deliberate exception — appended  \
	   at the end, per the rule directly above. */                                    \
	INT (BOT_QUEST_SCRIPT_COOLDOWN_SEC,     "botQuestScriptCooldownSec",     180)     \
	/* Share of bots eligible to run quests at all, as a percentage, applied as       \
	   (guid % 100) < pct at registration. Was hardcoded (guid % 20 == 0) = 5%. */    \
	INT (BOT_QUEST_BOT_PCT,                 "botQuestBotPct",                5)       \
	/* --- House visits. An awake bot walks to a bot-owned house in the town it is    \
	   standing in, opens the door, and idles at one interior tile on the ENTRY FLOOR \
	   (same-floor only, deliberately — see BOT_HOUSE_VISIT_PLAN.md §2.0), optionally \
	   greeting a hireling, standing at a locker, or training at a dummy. Appended at \
	   the end per the rule above. */                                                 \
	/* Master switch for the ACCESS grant in House::getHouseAccessLevel — bots become \
	   sub-owners of every bot-owned house. Independent of the visit roll below: with \
	   this on and the roll at 0, bots still reach house interiors through the depot  \
	   dwell's PZ roam, which is accepted. OFF = byte-identical to pre-feature. */    \
	BOOL(BOT_HOUSE_ACCESS_ENABLE,           "botHouseAccessEnable",          false)   \
	/* The account whose houses count as bot-owned (bots share account 65000). */     \
	INT (BOT_HOUSE_ACCOUNT_ID,              "botHouseAccountId",             65000)   \
	/* Roll (0-100) that the house candidate is OFFERED when an awake bot rolls a     \
	   POI. Gates the ENTIRE block, so 0 is structurally unreachable — uniform_random \
	   (1,100) is never <= 0 — not merely improbable. Mirrors botNpcVisitPct. */      \
	INT (BOT_HOUSE_VISIT_PCT,               "botHouseVisitPct",              0)       \
	/* Relative weight of the house candidate against depot/temple/boat/npc/water. */ \
	/* How long the bot stays inside, in seconds. */                                  \
	INT (BOT_HOUSE_IDLE_MIN_SEC,            "botHouseIdleMinSec",            120)     \
	INT (BOT_HOUSE_IDLE_MAX_SEC,            "botHouseIdleMaxSec",            600)     \
	/* Reach cap on the house entry, in tiles (same as the NPC visit's). */           \
	INT (BOT_HOUSE_MAX_DIST,                "botHouseMaxDist",               150)     \
	/* Concurrent visitors per house. */                                              \
	INT (BOT_HOUSE_MAX_OCCUPANTS,           "botHouseMaxOccupants",          2)       \
	/* Chance to perform a sub-activity when the house HAS one (else plain idle). */  \
	/* Seconds to reach the exact interior tile after first setting foot in the       \
	   house; on expiry the bot settles where it stands and downgrades to plain idle  \
	   (or abandons the visit if it never got inside). */                             \
	INT (BOT_HOUSE_SETTLE_SEC,              "botHouseSettleSec",             45)

// ============================================================================
// BOT_MARKET_ACCEPT tunables — how bots decide to accept a REAL player's market
// offer. Third table, expanded at the same three sites as the two above.
//
// Replaces the old hardcoded BUY_DEAL_THRESHOLD/BUY_DEAL_PROBABILITY/
// FULFILL_THRESHOLD/FULFILL_PROBABILITY constants in bot_market_data.lua. The old
// model gave each FIRE a budget of 1-3 acceptances taken in price order, which let
// whichever player held the cheapest offers absorb the whole budget. The model here
// is one independent roll PER OFFER, so every order of every player gets the same
// chance on every fire and there is no cross-player budget to monopolise.
// ============================================================================
#define BOT_MARKET_CONFIG_KEYS(INT, BOOL, STR)                                       \
	/* Random seconds between accept sweeps (one roll per real offer per sweep). */    \
	INT (BOT_MARKET_ACCEPT_INTERVAL_MIN_SEC, "botMarketAcceptIntervalMinSec", 900)     \
	INT (BOT_MARKET_ACCEPT_INTERVAL_MAX_SEC, "botMarketAcceptIntervalMaxSec", 2700)    \
	/* Per-OFFER chance (0-100) that a sweep considers that offer at all. 0 disables   \
	   the whole pass — uniform_random(1,100) is never <= 0. */                        \
	INT (BOT_MARKET_ACCEPT_CHANCE_PCT,       "botMarketAcceptChancePct",      20)      \
	/* Bot BUYS a player's SELL offer when price <= ref * thisPct/100. Above 100 on    \
	   purpose: bots are willing to overpay slightly, which is what makes a player's   \
	   fairly-priced listing actually sell. */                                         \
	INT (BOT_MARKET_SELL_CEILING_PCT,        "botMarketSellCeilingPct",       110)     \
	/* Bot SELLS into a player's BUY offer when price >= ref * thisPct/100. */         \
	INT (BOT_MARKET_BUY_FLOOR_PCT,           "botMarketBuyFloorPct",          95)      \
	/* Milliseconds between the individual acceptances a sweep decided on. Each        \
	   Game.botAcceptMarketOffer does synchronous DB work on the dispatcher (two       \
	   offline player loads, two history appends, a delete and possibly a full         \
	   savePlayer), so a sweep NEVER settles them in one tick — it paces them. */      \
	INT (BOT_MARKET_ACCEPT_DRAIN_MS,         "botMarketAcceptDrainMs",        400)     \
	/* ── No-reference guards ──────────────────────────────────────────────────       \
	   An item with no market_max and no NPC price has no value the server knows, so   \
	   the price gate above has nothing to compare against and the offer would be      \
	   accepted at whatever the player typed. Measured on the live table that set is   \
	   56 items and is almost entirely FREE-TO-FARM TRASH (wooden trash, stone         \
	   rubbish, rotten meat, broken bottle, box, chest), which is what makes it        \
	   dangerous rather than harmless: a player can obtain unlimited quantities and    \
	   list them at up to 999,999,999,999 gp/unit (the engine's own cap, game.cpp).    \
	   The bot's funds check is NOT a bound — a hibernated bot's debit is never saved  \
	   (see the skip-save note in game_bot_market.cpp), so its balance is re-read at   \
	   full value on every accept and never depletes.                                  \
	                                                                                   \
	   Set MaxUnitPrice to 999999999999 to get literal "accept whatever is listed". */ \
	/* Bot BUYS a no-ref item only at or below this many gp PER UNIT. Per-unit, not    \
	   per-total: a total cap would reject a legitimate 64000-stack of 1gp junk. */    \
	INT (BOT_MARKET_NOREF_MAX_UNIT_PRICE,    "botMarketNorefMaxUnitPrice",    10)      \
	/* Bot SELLS a no-ref item only at or above this many gp per unit. The BUY branch  \
	   CONJURES the goods with no depot debit, so without a floor a player could bait  \
	   free items at 1gp each. */                                                      \
	INT (BOT_MARKET_NOREF_MIN_UNIT_PRICE,    "botMarketNorefMinUnitPrice",    20)      \
	/* Total gold a single sweep may move through no-ref acceptances. The per-unit cap \
	   bounds ONE order; this bounds the other axis — a player may hold up to          \
	   maxMarketOffersAtATimePerPlayer offers and every one gets its own roll, so      \
	   per-order caps alone scale linearly with order count. Ref'd offers are never    \
	   charged to this budget and keep draining after it is exhausted. */               \
	INT (BOT_MARKET_NOREF_FIRE_GOLD_BUDGET,  "botMarketNorefFireGoldBudget",  1000000)

// ============================================================================
// BOT_PARTY_TRAIL_FOLLOW tunables — party support bots walk the route their
// leader actually took (a per-party breadcrumb trail of the leader's footsteps)
// instead of teleporting to the leader; teleport survives only as a watchdog.
// See implementation_plans/BOT_PARTY_TRAIL_FOLLOW.md.
//
// Fourth table, expanded at the same three sites as the tables above. A NEW
// table appended after the last one, so no existing enumerator value shifts
// (see the append rule documented in BOT_ACTIVITY_CONFIG_KEYS).
// ============================================================================
#define BOT_PARTY_TRAIL_CONFIG_KEYS(INT, BOOL, STR)                                  \
	/* Master kill switch. false = today's teleport behavior, bit-identical. Flip    \
	   in config.lua only after ~24h of [PTRAIL] baseline has been collected. */      \
	BOOL(BOT_PARTY_TRAIL_ENABLE,           "botPartyTrailEnable",           false)    \
	/* Also walk the trail for HUMAN-led parties (doPartyFollow). */                  \
	BOOL(BOT_PARTY_TRAIL_HUMAN_LEAD,       "botPartyTrailHumanLead",        true)     \
	/* No-progress watchdog: cursor makes no progress this long -> teleport. */       \
	INT (BOT_PARTY_TRAIL_STUCK_MS,         "botPartyTrailStuckMs",          30000)    \
	/* Hard lag cap: follower this many tiles behind the trail -> teleport. */        \
	INT (BOT_PARTY_TRAIL_MAX_LAG_TILES,    "botPartyTrailMaxLagTiles",      40)       \
	/* Breadcrumb ring size per leader. */                                            \
	INT (BOT_PARTY_TRAIL_MAX_NODES,        "botPartyTrailMaxNodes",         256)      \
	/* Tiles per A* leg while replaying the trail. */                                 \
	INT (BOT_PARTY_TRAIL_HORIZON,          "botPartyTrailHorizon",          10)       \
	/* Trail staleness: nodes older than this are never anchor candidates, and an     \
	   idle trail is re-recorded rather than resumed (hibernate/re-wake safety). */   \
	INT (BOT_PARTY_TRAIL_MAX_AGE_MS,       "botPartyTrailMaxAgeMs",         15000)    \
	/* Straggler distance that holds a BOT leader's waypoint advancement. */          \
	INT (BOT_PARTY_LEADER_WAIT_DIST,       "botPartyLeaderWaitDist",        9)       \
	/* Max hold before the leader advances anyway. */                                 \
	INT (BOT_PARTY_LEADER_WAIT_MAX_MS,     "botPartyLeaderWaitMaxMs",       20000)    \
	/* ROUND2 B: reroll weight for STARTING a party hunt. Applies to ALL vocations —  \
	   the leader is elected EK > RP > initiator, so this is no longer an EK-only     \
	   knob and is funded from botRerollWeightHunt (25 -> 13) rather than from the    \
	   travel tail. Ladder: idle 15 / POI 35 / hunt 13 / party 22 / travel tail 15. */\
	/* Max % of LOGGED-IN bots (hibernated + awake) that may be party-bound at once.  \
	   Counted in BOTS, not parties. 0 = uncapped. Gates FORMATION only — a running   \
	   party is never dissolved, so the count decays as hunts end. */                 \
	INT (BOT_PARTY_MAX_PCT,                "botPartyMaxPct",                20)       \
	/* BOT_PARTY_INVITE_RENDEZVOUS ----------------------------------------------      \
	   Bots accept real party invites, and ONE assembly machine serves both bot-led    \
	   party hunts and human-led parties (members walk in instead of popping in).      \
	   Appended at the END of this table so no existing enumerator value shifts.       \
	   See implementation_plans/BOT_PARTY_INVITE_RENDEZVOUS.md.                        \
	                                                                                  \
	   Master switch for invite detection + acceptance. false = today's behavior       \
	   (a real player's invite to a bot is simply never answered). */                  \
	BOOL(BOT_PARTY_INVITE_ENABLE,          "botPartyInviteEnable",          false)    \
	/* Leader-side invite poll cadence. The poll is gated on a real player (or cast    \
	   viewer) being online, so it costs nothing on an empty server. */                \
	INT (BOT_PARTY_INVITE_POLL_MS,         "botPartyInvitePollMs",          1000)     \
	/* Human-like pause before the bot accepts, uniform in [min,max]. */              \
	INT (BOT_PARTY_INVITE_ACCEPT_MIN_MS,   "botPartyInviteAcceptMinMs",     1500)     \
	INT (BOT_PARTY_INVITE_ACCEPT_MAX_MS,   "botPartyInviteAcceptMaxMs",     4500)     \
	/* Max hold while the bot finishes a fight it was already in; expiry -> decline.   \
	   A bot in an autonomous party hunt declines immediately instead of holding. */   \
	INT (BOT_PARTY_INVITE_HOLD_MAX_MS,     "botPartyInviteHoldMaxMs",       30000)    \
	/* Human-led start-walking threshold (was the PARTY_FOLLOW_DIST constant).         \
	   Deliberately NOT the same quantity as PARTY_HUNT_SUPPORT_FOLLOW_DIST (3), which \
	   is the trail's arrival/retire ring — aliasing them would make direct follow     \
	   stop exactly where the trail retires. Clamped to [1, PARTY_LEASH_DIST-1]. */    \
	INT (BOT_PARTY_FOLLOW_DIST,            "botPartyFollowDist",            2)        \
	/* Assembly master switch (BOTH kinds). false = today's instant teleport           \
	   assembly, bit-identical. Independent of botPartyInviteEnable, so                \
	   invite=true + rv=false is a valid intermediate deploy stage. */                 \
	BOOL(BOT_PARTY_RV_ENABLE,              "botPartyRvEnable",              false)    \
	/* Per-member TRAVEL+WALK budget measured from TRAVELLING entry; expiry ->         \
	   teleport fallback (counted, never silent). */                                   \
	INT (BOT_PARTY_RV_MAX_MS,              "botPartyRvMaxMs",               300000)   \
	/* Per-member graceful wind-down cap. MUST stay >= LEAVING_PHASE_MAX_MS (300s) or  \
	   a graceful hunt exit gets force-torn mid-walk, defeating the point. */          \
	INT (BOT_PARTY_RV_FINISH_MAX_MS,       "botPartyRvFinishMaxMs",         300000)

// ============================================================================
// BOT_AMBIENT_ROAM tunables — bots materialise just outside a player's view and
// wander between random reachable tiles near them, across floors, dwelling at
// each stop, until the player leaves and they hibernate.
//
// FIFTH table, appended after the last one so no existing enumerator value
// shifts (see the append rule in BOT_ACTIVITY_CONFIG_KEYS). Expanded at FOUR
// sites, not three: config_enums.hpp, configmanager.cpp, AND the hand-maintained
// dump in `/cavebot botcfg` (bot_command.cpp) — a table missing from that last
// one is invisible to the only command that can verify what actually loaded,
// which is exactly how the 2026-07-31 silent config loss went unnoticed.
//
// Unlike every other bot feature here this one ships ENABLED by user decision.
// It is bounded by botRoamReserveSlots (below) rather than by a 0 default.
// ============================================================================
#define BOT_AMBIENT_ROAM_CONFIG_KEYS(INT, BOOL, STR)                                 \
	/* Master switch. false = no injection, no sessions, no cost. */                  \
	BOOL(BOT_ROAM_ENABLE,                  "botRoamEnable",                 true)     \
	/* EXTRA awake-bot slots per density ring, usable ONLY by roamers. The organic    \
	   arm of shouldGateWake subtracts roamCounts, so ordinary wakes keep their full  \
	   base limit and this is a true addition, not a redistribution: at the live      \
	   config that means inner 3+3 and mid 10+3, of which at most 3 are roamers. */   \
	INT (BOT_ROAM_RESERVE_SLOTS,           "botRoamReserveSlots",           3)        \
	/* Roamers wanted per anchor CLUSTER (clusters merge at                           \
	   botDensityAnchorClusterRadius), effective value min(this, reserveSlots).       \
	   Per-cluster and not per-anchor on purpose: three grouped players form ONE      \
	   cluster, so a per-anchor target would ask for 9 against a reserve of 3 and     \
	   guarantee 6 permanently-gated attempts. */                                     \
	INT (BOT_ROAM_TARGET_PER_CLUSTER,      "botRoamTargetPerCluster",       3)        \
	/* Global ceiling on concurrent roamers, across every cluster. */                 \
	INT (BOT_ROAM_MAX_TOTAL,               "botRoamMaxTotal",               24)       \
	/* Leg destinations are drawn from within this many tiles of the anchor. */       \
	INT (BOT_ROAM_RADIUS,                  "botRoamRadius",                 20)       \
	/* Floors above/below the anchor that destinations may sit on. */                 \
	INT (BOT_ROAM_MAX_DZ,                  "botRoamMaxDz",                  1)        \
	/* Minimum leg length, measured from the BOT (not the anchor) — the quantity      \
	   that decides whether a leg is a visible walk. MUST stay above the arrival      \
	   tolerance: the shared walk driver hard-codes maxDist 3 and POI_ARRIVAL_DIST    \
	   is 3, so anything <= 4 completes after a single step. */                       \
	INT (BOT_ROAM_MIN_LEG_DIST,            "botRoamMinLegDist",             8)        \
	/* Dwell at each stop. */                                                         \
	INT (BOT_ROAM_DWELL_MIN_MS,            "botRoamDwellMinMs",             5000)     \
	INT (BOT_ROAM_DWELL_MAX_MS,            "botRoamDwellMaxMs",             40000)    \
	/* Per-cluster cadence between injections. One injection per tick server-wide     \
	   regardless, so a burst can never stall the dispatcher. */                      \
	INT (BOT_ROAM_INJECT_INTERVAL_MS,      "botRoamInjectIntervalMs",       4000)     \
	/* Anchor distance beyond which a session ends. On release the bot is             \
	   explicitly hibernated AND teleported back to its pre-injection position.       \
	   Deliberately tighter than the Lua loop's 100-tile hibernation radius: a        \
	   roamer released at 100 would stay awake (that loop never starts hysteresis     \
	   while a player is within 100), holding a density slot it no longer uses and    \
	   making the reserve unusable. */                                                \
	INT (BOT_ROAM_RELEASE_TILES,           "botRoamReleaseTiles",           40)       \
	/* Max session length. Without it, a stationary player keeps the same few bots    \
	   orbiting indefinitely — the 100-tile rule never fires for someone standing     \
	   still — which reads as artificial. On expiry the bot hibernates and a          \
	   DIFFERENT one is recruited, so the cast rotates. */                            \
	INT (BOT_ROAM_SESSION_MAX_MS,          "botRoamSessionMaxMs",           240000)   \
	/* Accounting ledger TTL. The ledger outlives the behavioural session so a bot    \
	   that stopped roaming but is still awake keeps the slot it was granted; this    \
	   bounds that for the case where it neither hibernates nor is torn down. */      \
	INT (BOT_ROAM_LEDGER_TTL_MS,           "botRoamLedgerTtlMs",            300000)   \
	/* Cap on one monster fight before the roamer disengages. */                      \
	INT (BOT_ROAM_DEFEND_MAX_MS,           "botRoamDefendMaxMs",            90000)    \
	/* Reachable-region cache: rebuild after this long, or once the anchor has        \
	   moved this far. Move-invalidation must stay coarse (>= radius/2) or a walking  \
	   player invalidates the region faster than the TTL can amortise the flood. */   \
	INT (BOT_ROAM_REGION_TTL_MS,           "botRoamRegionTtlMs",            5000)     \
	INT (BOT_ROAM_REGION_MOVE_TILES,       "botRoamRegionMoveTiles",        10)       \
	/* Flood budget. Must exceed (2*botRoamRadius+1)^2 or LocalReach truncates the    \
	   disc silently and corners of the region vanish nondeterministically. */        \
	INT (BOT_ROAM_REGION_BUDGET,           "botRoamRegionBudget",           2048)     \
	/* Consecutive failed legs before the session gives up. MUST stay below           \
	   STUCK_THRESHOLD (5) — that ladder ends in teleportToTemple, i.e. an ambient    \
	   bot vanishing in front of the player, which is the pop-out this whole          \
	   feature exists to remove. */                                                   \
	INT (BOT_ROAM_MAX_FAIL_STREAK,         "botRoamMaxFailStreak",          3)

// ============================================================================
// BOT_CORPSE_LOOT tunables — a hunting bot opens the corpses it killed so the
// client's "this corpse can be looted" highlight goes away.
//
// SIXTH table, appended after the last one so no existing enumerator value
// shifts (see the append rule in BOT_ACTIVITY_CONFIG_KEYS). Expanded at FOUR
// sites: this header, config_enums.hpp, configmanager.cpp, and the
// hand-maintained dump in `/cavebot botcfg` (bot_command.cpp).
//
// WHY THIS EXISTS, and why the numbers below are what they are:
// Item::setID (item.cpp:788-790) strips CORPSEOWNER on every decay stage while
// Container::m_lootHighlightActive survives it, so a corpse whose next decay
// stage is still a container becomes highlighted for EVERY player, not just its
// owner. Measured over data/items/appearances.dat joined to items.xml: of 802
// first-stage corpse containers with a decayTo, 746 go publicly lit after just
// 10 SECONDS and then stay lit for the remaining 300-600s of their decay chain.
// That same 10s is the only window in which a bot can still recognise its own
// kill by CORPSEOWNER, which is why the census runs during combat and not only
// between fights — see the BOT_CORPSE_LOOT block in bot_hunt.cpp.
// ============================================================================
#define BOT_CORPSE_LOOT_CONFIG_KEYS(INT, BOOL, STR)                                  \
	/* Master switch. false = the feature costs nothing, not merely opens nothing:    \
	   every pass early-outs on this before touching a tile. */                       \
	BOOL(BOT_LOOT_OPEN_ENABLE,             "botLootOpenEnable",             false)    \
	/* Census half-width. 7 = the client viewport, NOT a smaller "near the bot"       \
	   number: the corpse drops on the MONSTER's tile (creature.cpp:705) and mages    \
	   and paladins kill at range, so a radius of 4 would silently skip most ranged   \
	   kills — which then go publicly lit per the note above. */                      \
	INT (BOT_LOOT_RADIUS,                  "botLootRadius",                 7)        \
	/* How long after a kill the WALK pass stays armed. Refreshed on every successful \
	   open, never on scan — refreshing on scan would let one unreachable corpse hold \
	   the window open forever via pick-drop churn. */                                \
	INT (BOT_LOOT_WINDOW_MS,               "botLootWindowMs",               20000)    \
	/* Census cadence. Zeroed on the kill tick so the tick after a kill always        \
	   censuses; that is what bounds adjacent-open latency to ~one tick. */           \
	INT (BOT_LOOT_SCAN_MS,                 "botLootScanMs",                 750)      \
	/* false disables the WALK pass outright (adjacent opens continue). It does NOT   \
	   mean "run the walk pass but drop every run", which would burn a pick+drop      \
	   cycle every census with nothing to show for it. */                             \
	BOOL(BOT_LOOT_WALK,                    "botLootWalk",                   true)     \
	/* Per-corpse walk budget. Deliberately short: goTo costs up to 3 A* searches on  \
	   failure and an unreachable target fails EVERY tick, which is the exact shape   \
	   of the 5733ms dispatcher stall documented at bot_nav.cpp:488-505. */           \
	INT (BOT_LOOT_MAX_WALK_MS,             "botLootMaxWalkMs",              4000)     \
	/* Human pause before the corpse opens. Without it an EK's corpses unsparkle the  \
	   instant they hit the ground, which reads as a script on cast. */               \
	INT (BOT_LOOT_DELAY_MIN_MS,            "botLootDelayMinMs",             300)      \
	INT (BOT_LOOT_DELAY_MAX_MS,            "botLootDelayMaxMs",             800)      \
	/* Also open OWNERLESS highlighted corpses (owner already stripped by decay), but \
	   ONLY when no real player is on screen. This is the only setting that actually  \
	   keeps a hunting ground clean — own-kill claims alone still leak every corpse    \
	   the bot could not reach inside the 10s window. The on-screen guard is what     \
	   stops a bot unsparkling the corpses of a human sharing the spawn. Set false    \
	   for strictly-own-kills behaviour. */                                           \
	BOOL(BOT_LOOT_PUBLIC_CLEANUP,          "botLootPublicCleanup",          true)

// ============================================================================
// BOT_ACTIVITY_PCT — the two percentage tables that decide what a bot does next.
//
// THE RULE: each table sums to 100, and each number is a real percentage. Nothing
// here is a "relative weight" any more, and nothing is a residual tail.
//
// This replaces five keys that lied. botRerollWeightTravel advertised 25, was read
// into a variable that was never used, and delivered a 15% residual while the bots
// actually travelled 46.5% of the time -- three different numbers for one behaviour.
// The four botRerollWeight* keys and the seven botPoiWeight* keys are DELETED, not
// aliased: a silently-ignored legacy key is exactly how that one survived for months.
//
// TABLE A (activity) is the top-level roll. TABLE B (POI destination) runs only when
// A picks POI, so a row's real share is A.poi% x B.row%. Do NOT write those products
// into comments here -- they go stale the moment either table is edited, which is the
// failure mode the old pool-arithmetic comments demonstrated. Run `/cavebot activity`.
//
// FAILURE SEMANTICS (this is what makes the numbers true): a bin whose attempt fails,
// or which is ineligible for this bot right now, becomes a SHORT DWELL and the bot
// re-rolls fresh. Failures never silently donate their share to another bin. HUNT and
// PARTY are both stock-limited -- 221 hunt scripts held 2-3h for 500 bots, and a party
// cap of botPartyMaxPct x population -- so their attempts genuinely do fail, routinely.
// That is visible as dwell time and in `/cavebot activity`, not as a hidden bias.
//
// Retuning any VALUE here is free: edit config.lua then `/cavebot _global reloadconfig`.
// Only adding or renaming a KEY needs a full rebuild.
// ============================================================================
#define BOT_ACTIVITY_PCT_CONFIG_KEYS(INT, BOOL, STR)                                 \
	/* ---- TABLE A: which ACTIVITY to start. MUST SUM TO 100. ---- */                              \
	/* Stand around where it is. Also the fallback every failed attempt lands in. */ \
	INT (BOT_ACTIVITY_DWELL,                    "botActivityDwell",                   12)       \
	/* Walk to a point of interest in town -- then TABLE B picks which one. */       \
	INT (BOT_ACTIVITY_POI,                      "botActivityPoi",                     30)       \
	/* Start a solo hunt. Capped by the 221-script pool, so some attempts fail. */   \
	INT (BOT_ACTIVITY_HUNT,                     "botActivityHunt",                    10)       \
	/* Try to FORM a party hunt. Deliberately small: parties run 2-3h and occupy     \
	   botPartyMaxPct of the population, so the cap only absorbs ~10 new formations  \
	   an hour. A bigger number here does not make more parties -- it just makes     \
	   more refused attempts, which become dwell. Raise botPartyMaxPct instead if    \
	   you want more parties, but note that uncapping once put 448 of 500 bots into  \
	   parties and starved solo hunting. */                                          \
	INT (BOT_ACTIVITY_PARTY,                    "botActivityParty",                   2)        \
	/* Travel to another town (boat/carpet), or a city walk -- see                   \
	   botTravelCityWalkPct for the split between them. */                           \
	INT (BOT_ACTIVITY_TRAVEL,                   "botActivityTravel",                  46)       \
	/* ---- TABLE B: POI destination. MUST SUM TO 100. ---- */                       \
	/* Rolled over whatever the bot's CURRENT TOWN actually has, so these are shares  \
	   of the available candidates, not of all nine rows. Coverage is uneven and no   \
	   number can change that: 19 towns have a depot and a temple, 14 have an         \
	   adventurer's stone, 9 have a boat, and exactly ONE has a shop. A hibernated    \
	   bot additionally cannot reach npc/water/house/rewardShrine/imbuingShrine (all  \
	   awake-only), so its table is the other six rows. NOTE: the two shrine rows are \
	   declared in BOT_SHRINE_CONFIG_KEYS at the BOTTOM of this file, not here, for   \
	   the ordinal-safety reason explained there -- but they ARE TABLE B rows and are \
	   part of its sum-to-100. `/cavebot activity` reports both populations. */       \
	INT (BOT_POI_DEPOT,                    "botPoiDepot",                   22)       \
	INT (BOT_POI_DEPOT_OUTSIDE,            "botPoiDepotOutside",            11)       \
	INT (BOT_POI_TEMPLE,                   "botPoiTemple",                  5)        \
	INT (BOT_POI_BOAT,                     "botPoiBoat",                    11)       \
	INT (BOT_POI_SHOP,                     "botPoiShop",                    5)        \
	INT (BOT_POI_NPC,                      "botPoiNpc",                     13)       \
	INT (BOT_POI_WATER,                    "botPoiWater",                   14)       \
	INT (BOT_POI_HOUSE,                    "botPoiHouse",                   13)       \
	INT (BOT_POI_ADV_STONE,                "botPoiAdvStone",                6)        \
	/* ---- Cadence + the rolls that used to be hardcoded ---- */                    \
	/* How often an idle bot samples TABLE A. The single biggest lever in this        \
	   section: it scales every activity's absolute rate at once. */                  \
	INT (BOT_ACTIVITY_REROLL_COOLDOWN_SEC,      "botActivityRerollCooldownSec",       30)       \
	/* Length of the fallback dwell after a failed or ineligible attempt. Kept        \
	   SHORT and separate from botDwellRerollMin/MaxSec (which is a CHOSEN dwell):    \
	   a saturated party cap would otherwise convert its share into minutes of        \
	   standing still instead of seconds. */                                          \
	INT (BOT_ACTIVITY_FALLBACK_DWELL_MIN_SEC,   "botActivityFallbackDwellMinSec",     30)       \
	INT (BOT_ACTIVITY_FALLBACK_DWELL_MAX_SEC,   "botActivityFallbackDwellMaxSec",     90)       \
	/* Of TRAVEL rolls, how many are a walking city route rather than a boat. NOTE:   \
	   structurally inert for HIBERNATED bots -- tryStartCityWalk needs a live Player \
	   and returns false without one -- so the realised rate across the whole         \
	   population is far below this number. Measured 0.4% at 30. */                   \
	INT (BOT_TRAVEL_CITY_WALK_PCT,         "botTravelCityWalkPct",          30)       \
	/* Where a bot walks when it LANDS in a destination town. A WEIGHT WALK, not four \
	   independent gates: the four are summed and one bucket is drawn, so they behave \
	   as percentages when they SUM TO 100 and degrade proportionally when they do    \
	   not. Depot=100 with the rest 0 reproduces the pre-feature behaviour exactly.    \
	   All four 0 falls back to depot rather than dividing by zero.                   \
	   THE REALISED SPLIT IS DEPOT-HEAVIER THAN CONFIGURED, and that is data, not a    \
	   bug: shops are authored in only 9 of the 18 towns (and non-core POIs in the     \
	   same 9), so a shop/other roll in Farmine, Roshamuul, Rathleton, Krailos,        \
	   Issavi, Feyrist, Ab'Dendriel, Carlin or Kazordoon has nothing to resolve to     \
	   and falls through to depot. Hunt-bound and party-assembly bots are forced to    \
	   depot regardless -- see pickTravelArrivalTarget. */                            \
	INT (BOT_TRAVEL_ARRIVE_DEPOT_PCT,      "botTravelArriveDepotPct",       55)       \
	INT (BOT_TRAVEL_ARRIVE_TEMPLE_PCT,     "botTravelArriveTemplePct",      20)       \
	INT (BOT_TRAVEL_ARRIVE_SHOP_PCT,       "botTravelArriveShopPct",        20)       \
	INT (BOT_TRAVEL_ARRIVE_OTHER_PCT,      "botTravelArriveOtherPct",        5)       \
	/* After resupplying, chance to go straight back out on another hunt instead of   \
	   returning to TABLE A. Applies at BOTH live resupply sites. The virtual         \
	   (hibernated) path has no such roll -- it always returns to TABLE A. */         \
	INT (BOT_RESUPPLY_REHUNT_PCT,          "botResupplyRehuntPct",          50)       \
	/* On arriving at a depot, chance to walk to the locker rather than mill about    \
	   outside. Applies at both depot-arrival sites. */                               \
	/* ---- TABLE C: what to do once INSIDE a house. MUST SUM TO 100. ---- */              \
	/* TABLE B's botPoiHouse only gets the bot through the door; this decides what it does \
	   in there. Rolled over what the house ACTUALLY HAS -- most houses have no training   \
	   dummy and no hireling -- so a row's realised share differs from its number in the   \
	   same way TABLE B's does. Plain idle is always available, so the roll never comes up \
	   empty. This replaces botHouseSubActivityPct, which was a single "do something?"     \
	   dial followed by a hardcoded UNIFORM pick between hireling/dummy/locker -- there was\
	   no way to prefer one over another. Idle is simply its own row now. */               \
	INT (BOT_HOUSE_IDLE,                   "botHouseIdle",                  30)       \
	INT (BOT_HOUSE_HIRELING,               "botHouseHireling",              23)       \
	INT (BOT_HOUSE_DUMMY,                  "botHouseDummy",                 23)       \
	INT (BOT_HOUSE_LOCKER,                 "botHouseLocker",                24)       \
	INT (BOT_DEPOT_LOCKER_PCT,             "botDepotLockerPct",             40)

// ============================================================================
// BOT_LURE_KITE tunables — two hunt-combat behaviours that share one table.
//
// LURE: a bot whose hunt script sets `min_monsters` walks its patrol HOLDING FIRE
// while monsters aggro and trail it, then engages the whole pack at once. This is
// what a real player does at a spawn; the previous behaviour was to stop and kill
// the first thing that came into range, forever.
//
// KITE: a keep-distance bot that has backed as far as it can (cornered, or stopped
// by the 15-tile waypoint drift cap) used to STAND STILL and tank. It now retraces
// the patrol waypoints it already walked — known-walkable ground — ping-ponging over
// that stretch until the pack dies.
//
// Both default OFF, per the table-wide rule above. Data ALSO gates lure: with no
// `min_monsters` column in hunt_scripts.csv, and no party hunt, nothing arms even
// with botLureEnable=true.
//
// APPEND-ONLY, and this table is expanded LAST among the BOT_* tables: the
// hot-reloaded libbot_engine.so reads every bot key by ORDINAL, so inserting a key
// above an existing one silently misreads all of them on any binary/.so skew.
// ============================================================================
#define BOT_LURE_KITE_CONFIG_KEYS(INT, BOOL, STR)                                    \
	/* --- Lure --- */                                                                \
	BOOL(BOT_LURE_ENABLE,                  "botLureEnable",                 false)    \
	/* Level gate: arm only when level >= script min_level * this / 100. A script      \
	   with min_level 0 can NEVER arm on the level path (only as a party hunt) —       \
	   otherwise a level-8 bot would lure a dragon spawn. */                           \
	INT (BOT_LURE_LEVEL_FACTOR_PCT,        "botLureLevelFactorPct",         130)      \
	/* A party hunt lures regardless of level or of the script's min_monsters. */     \
	BOOL(BOT_LURE_PARTY_ALWAYS,            "botLurePartyAlways",            true)     \
	/* Pack size used for a party hunt whose script has min_monsters = 0. Without      \
	   this the party rule is dead code at ship defaults (every script is 0). */       \
	INT (BOT_LURE_PARTY_DEFAULT_MIN,       "botLurePartyDefaultMin",        3)        \
	/* Census radius. CLAMPED to MONSTER_SCAN_RADIUS (10) at load: the census reads    \
	   the spectator cache, which cannot see farther, so a larger value would          \
	   silently undercount rather than widen the net. */                               \
	INT (BOT_LURE_RADIUS,                  "botLureRadius",                 7)        \
	/* Hard ceiling on pack size, so a data typo cannot ask for 50 monsters. */       \
	INT (BOT_LURE_MAX_PACK,                "botLureMaxPack",                12)       \
	/* Pace: stand still to let the TAIL of the pack catch up. Never applied while     \
	   anything is close (see botLureContactMs) — walking IS the spacing. */          \
	INT (BOT_LURE_PACE_DIST,               "botLurePaceDist",               6)        \
	INT (BOT_LURE_PACE_MAX_MS,             "botLurePaceMaxMs",              4000)     \
	/* Absolute lure duration before engaging with whatever has gathered. Also the     \
	   bound on the hunt-end lure hold. */                                            \
	INT (BOT_LURE_MAX_MS,                  "botLureMaxMs",                  60000)    \
	/* Engage immediately below this HP%. Keep-distance retreat is structurally OFF    \
	   while luring (no target => chaseTarget never runs), so this is the main         \
	   survival valve. */                                                             \
	INT (BOT_LURE_HP_FLOOR_PCT,            "botLureHpFloorPct",             70)       \
	/* No net movement for this long while luring = something is body-blocking a       \
	   corridor; engage so the normal blocking-monster path can clear it. */          \
	INT (BOT_LURE_BLOCKED_MS,              "botLureBlockedMs",              3000)     \
	/* Sustained melee contact (or a support in contact) before engaging. */          \
	INT (BOT_LURE_CONTACT_MS,              "botLureContactMs",              1500)     \
	/* Pack shrinking below its peak for this long = we are shedding aggro; take       \
	   what we have rather than lure an emptying screen forever. */                   \
	INT (BOT_LURE_DECAY_MS,                "botLureDecayMs",                6000)     \
	/* --- Kite-backtrack --- */                                                      \
	BOOL(BOT_KITE_BACKTRACK_ENABLE,        "botKiteBacktrackEnable",        false)    \
	/* How many already-walked patrol waypoints the ping-pong window may span. */     \
	INT (BOT_KITE_DEPTH_WPS,               "botKiteDepthWps",               6)        \
	INT (BOT_KITE_MAX_SPAN_TILES,          "botKiteMaxSpanTiles",           30)       \
	/* Direction reversals before giving up and standing to fight. */                 \
	INT (BOT_KITE_MAX_LEGS,                "botKiteMaxLegs",                6)        \
	INT (BOT_KITE_MAX_MS,                  "botKiteMaxMs",                  45000)    \
	/* Set ONLY on a give-up exit, never on a threat-gone exit. While kiting, both     \
	   target-abandonment checks in scanAndAttackMonster are suppressed; without a     \
	   cooldown a give-up could re-arm on the very next tick and starve them forever,  \
	   leaving only the 5400s/12600s safety ceiling as a bound. */                     \
	INT (BOT_KITE_COOLDOWN_MS,             "botKiteCooldownMs",             12000)

// ============================================================================
// BOT_SHRINE_IDLE — bots walk to a reward / imbuing shrine and idle facing it.
//
// This table is its OWN table, expanded LAST (after BOT_LURE_KITE_CONFIG_KEYS) in both
// config_enums.hpp and configmanager.cpp, and that placement is the whole point rather than
// tidiness. The eight BOT_* tables expand IN SEQUENCE into one enum, and the hot-reloaded
// libbot_engine.so reads bot keys BY ORDINAL — so appending these to, say, the fishing table
// would silently shift every key in the six tables after it, and push the tail of the enum past
// MAGIC_ENUM_RANGE_MAX (500, src/pch.hpp), beyond which a key drops out of Lua's `configKeys`
// registration with no diagnostic at all. Appending here shifts nothing that a .so reads.
//
// Discovery is a RUNTIME SCAN (findNearbyShrines, bot_waypoint.cpp), memoized per town for the
// life of the engine — there is no index, no cache file and no ZCACHE_VERSION involvement, so
// none of these keys can be made stale by a warm cache.
//
// botShrineVisitPct ships at 0 and that is structural, not shy: `pct > 0` short-circuits the
// entire sampling block, and uniform_random(1,100) starts at 1 so `<= 0` is unreachable rather
// than merely improbable. At 0 the feature costs nothing at all. Same discipline as botFishPct.
// The SHARE, once eligible, is botPoiRewardShrine / botPoiImbuingShrine in TABLE B.
// ============================================================================
#define BOT_SHRINE_CONFIG_KEYS(INT, BOOL, STR)                                       \
	/* Eligibility only (0 = off, non-zero = on). ONE roll gates BOTH kinds, so at    \
	   25 both candidates are offered in the same 25% of rerolls rather than          \
	   independently; TABLE B still decides the split between them. */                \
	INT (BOT_SHRINE_VISIT_PCT,             "botShrineVisitPct",             0)        \
	/* How many bots may hold a stand tile at ONE shrine at once. A town has about    \
	   one shrine of each kind, so without a cap every eligible bot converges on the  \
	   same tile-ring and a player walking in finds a halo around the depot. */       \
	INT (BOT_SHRINE_MAX_OCCUPANTS,         "botShrineMaxOccupants",         2)        \
	/* How long the bot stands there looking at it. */                                \
	INT (BOT_SHRINE_IDLE_MIN_SEC,          "botShrineIdleMinSec",           60)       \
	INT (BOT_SHRINE_IDLE_MAX_SEC,          "botShrineIdleMaxSec",           240)      \
	/* Deadline for closing the last tiles onto the reserved tile once the bot is     \
	   adjacent to it. Mirrors botHouseSettleSec; on expiry the visit ends rather     \
	   than pretending the bot arrived somewhere it did not. */                       \
	INT (BOT_SHRINE_SETTLE_SEC,            "botShrineSettleSec",            45)       \
	/* ---- These two are TABLE B rows (POI destination share, see                     \
	   BOT_ACTIVITY_PCT_CONFIG_KEYS above) and are validated as part of TABLE B's      \
	   sum-to-100. They live HERE rather than beside the other nine for one reason:    \
	   BOT_ACTIVITY_PCT_CONFIG_KEYS is expanded BEFORE BOT_LURE_KITE_CONFIG_KEYS, so   \
	   appending to it would shift every lure/kite ordinal, and the .so reads bot keys \
	   by ordinal. Grouping by ordinal-safety beats grouping by topic here; the        \
	   comment is the compensation. Both default to 0, which keeps the nine original   \
	   rows summing to 100 and the feature inert until config.lua says otherwise. */   \
	INT (BOT_POI_REWARD_SHRINE,            "botPoiRewardShrine",            0)        \
	INT (BOT_POI_IMBUING_SHRINE,           "botPoiImbuingShrine",           0)       \
	/* TABLE C row (what to do INSIDE a house), here for the same ordinal-safety     \
	   reason as the two above. House shrines are the ONLY way a bot reaches a       \
	   store-bought shrine: the shrine POI deliberately skips house tiles,           \
	   because a shrine walk owns none of the door handling, tile claims,            \
	   occupancy cap or exit planner that a house visit does. */                     \
	INT (BOT_HOUSE_SHRINE,                 "botHouseShrine",                15)
