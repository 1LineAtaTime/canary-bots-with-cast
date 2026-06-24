# Bot Player System — Extended Technical Documentation

> Complete, in-depth developer reference for the bot system — architecture,
> behaviors, performance analysis, and internals — intended as a starting point
> for anyone continuing development. For a concise operator/user overview (setup,
> commands, key config), see [BOT_SYSTEM_DOCS.md](BOT_SYSTEM_DOCS.md).

<details>
<summary><strong>Overview</strong></summary>


200 bot players running across 14 Tibia cities with autonomous AI behaviors.
All bots belong to `account_id=65000` (player IDs 65001-65200).

The system uses a **hybrid C++ + Lua architecture**:
- **C++ Bot Engine** (`libbot_engine.so`): Core AI tick loop, state machine, combat,
  hunting, navigation, travel. Compiled as a shared library for hot-reload without
  server restart. Loaded via `dlopen` at runtime.
- **Lua**: Bot manager (startup/population), admin commands (`/cavebot`), hunt data
  loader, and `Game.loadBotPlayer()`/`player:isBotPlayer()` C++ APIs exposed to Lua.

Built by taking inspiration from https://otland.net/threads/8-60-thais-war-with-pvp-bots.204193/ (Gesior.pl)

### Time-of-Day Activity Scheduling

Bots follow a time-of-day activity curve implemented in `doPopulationManagement()` inside
`bot_engine.cpp`. On startup, all bots load as INACTIVE at their last saved position (from
`players.posx/y/z`, written by normal server shutdown). After a 30s grace period, the
scheduler activates/deactivates ±2 bots per 10s tick toward the target for the current time.

| Hour (server local) | % of total bots | ~Bots (of 200) |
|----------------------|-----------------|-----------------|
| 0-5 (night)          | 60%             | 120             |
| 6-8 (early morning)  | 30%             | 60              |
| 9-11 (morning)       | 40%             | 80              |
| 12-15 (afternoon)    | 60%             | 120             |
| 16-19 (evening)      | 75%             | 150             |
| 20-23 (prime time)   | 100%            | 200             |

- **30-minute ramps** between brackets: 30 minutes before the next bracket starts,
  the target percentage linearly transitions from the current to the next value.
  E.g., 23:30→00:00 ramps from 100% to 60% (no hard jumps at bracket boundaries).
- ±5% random jitter on target (recalculated each hour)
- Only IDLE or DWELLING bots are deactivated (never mid-hunt/combat/travel)
- Deactivated bots are **truly offline** — removed from the game world via `removeCreature + removePlayer` (like a disconnect). They do not sit at a staging area.
- Bots start INACTIVE on load, activated gradually after 30s startup grace
- Admin commands: `/cavebot anybot schedule on|off|status`

### Bot State Persistence (Graceful Restart)

On graceful server shutdown, all active bot states are saved to `bot_state_persistence` DB table
by the Lua `BotShutdown` GlobalEvent, which calls `Game.botSaveStates()` and sets
`should_restore_states=1` in `bot_startup_config`.

On next startup, if the restore flag is set, `Game.botRestoreStates()` is called after all bots
are registered. Each `activateBot()` call checks `bot_state_persistence` for a saved row and
calls `restoreSingleBotState()` to restore:
- `ai_state` (HUNTING, TRAVELING, etc.)
- `hunt_script_id`, `hunt_phase`, `waypoint_idx`, `kill_count`
- `travel_dest_town_id`

**Persisted state fields** (in `bot_state_persistence`):

| Column | Description |
|--------|-------------|
| `guid` | Bot player GUID |
| `name` | Bot player name (for re-loading after true-offline removal) |
| `ai_state` | Numeric BotAIState at save time (transient states normalized to IDLE) |
| `hunt_script_id` | Hunt script being run (0 if not hunting) |
| `hunt_phase` | Hunt phase enum value |
| `waypoint_idx` | Patrol waypoint index at save time |
| `kill_count` | Kills accumulated in current hunt |
| `travel_dest_town_id` | Destination town ID if TRAVELING |

Bot positions are **not** saved here — `players.posx/y/z` (written by normal server save) is
the authoritative position. `loadBotPlayer` uses `loginPosition` directly.

**Deactivated mid-hunt bots**: `forceDeactivateBot()` calls `saveSingleBotState()` before
removing the bot from the world, and does NOT erase the `activeHunts_` reservation. On
re-activation, the bot resumes from exact waypoint/kill count with spawn still locked.

**1-minute activation fallback**: After activation, if the bot stays IDLE for 60s without
picking any activity (e.g. loaded at an unreachable position), it teleports to its town temple.
Cleared as soon as `doActivityReroll()` fires.

### Autonomous Activity Reroll

When a bot is IDLE with nothing to do, `doActivityReroll()` automatically picks the next activity.
Implemented in `bot_engine.cpp`, called from `doIdle()` when the bot has no walk target.

**Reroll weights** (configurable constants, must sum to 100):

| Activity | Weight | Description |
|----------|--------|-------------|
| IDLE/Dwell | 15% (5% post-activation) | Stand still for 180-900s |
| POI Walk | 40% | Navigate to a POI in current town via city routes |
| Hunt | 35% | Start a hunt (level-only eligibility — no vocation filter; may travel cross-town if needed) |
| Travel | 10% | Travel to another city via boat |

**Post-activation reduction**: On first reroll after server start or `/cavebot reload`, dwell weight drops to 5% (`REROLL_IDLE_WEIGHT_POST_ACTIVATION`). The `postActivationReroll` flag in BotState is set during `activateBot()`/`reactivateBotForReload()` and cleared after first reroll. This prevents mass temple dwelling on restart — ~95% of bots immediately start activities.

**Dwell times** after arriving at POI:
- Depot, Temple, Boat: 300-900s
- NPC, Shop: 30-120s (shorter since bots don't linger there)
- Post-travel arrival: 180s before next reroll
- Post-hunt: 10-30s before next reroll
- Between rerolls (cooldown): 60s minimum

**Depot locker walk**: When a city route completes (navigate command or reroll POI walk to depot/temple),
the bot checks for nearby depot lockers and walks to them — same behavior as travel arrival.
Dynamic POIs (`_depot_outside`, `_boat_nearby`) skip this and walk to their actual target tile instead.

**Activation spawn**: Bots spawn at randomized locations (not always temple):
40% depot PZ area, 25% boat area, 20% temple, 15% depot outside (PZ boundary).
Same logic applies during `/cavebot reload` re-activation.

**Heartbeat logging**: Every 60s, each bot logs `STATUS: <state> [pos=(x,y,z) PZ|noPZ]` for diagnostics.

**Cast Chat logging**: All reroll decisions logged with roll value, range, chosen activity,
and timing details for easy debugging.

### Files

| File | Purpose |
|------|---------|
| **C++ Bot Engine (shared library)** | |
| `src/creatures/players/bot/bot_engine.cpp` | All bot AI: state machine, combat, hunting, navigation, travel → compiles to `libbot_engine.so` |
| `src/creatures/players/bot/bot_engine_interface.hpp` | ABI boundary: all shared types (enums, structs, `IBotEngine` pure virtual interface) |
| `src/creatures/players/bot/bot_engine_loader.hpp/cpp` | `dlopen`/`dlclose` management, `BotEngineLoader` singleton, factory function loading |
| `src/creatures/players/bot/bot_engine.hpp` | Thin wrapper: `constexpr auto g_botEngine = getBotEngineInstance;` — all call sites use this |
| **Lua** | |
| `data/scripts/lib/bot_hunting_data.lua` | Hunt data + route graph loader (MySQL → Lua at startup) |
| `data/scripts/globalevents/bot_manager.lua` | Bot startup loader — loads all bots as INACTIVE |
| `data/scripts/talkactions/god/bot_cavebot.lua` | `/cavebot` admin command for debug/control + `/cavebot reload` for hot-reload |
| `data/scripts/talkactions/god/bot_spawn.lua` | `/botspawn` admin command — teleport to hunt spawn start |
| `data/libs/tables/doors.lua` | Door ID tables (KeyDoorTable, CustomDoorTable) used by door-opening behavior |

---


</details>

<details>
<summary><strong>State Machine</strong></summary>


```
IDLE ──reroll──> DWELLING (15%) ──timer 180-900s──> IDLE
 |               POI walk (40%) ──city route──> arrive ──dwell 300-900s / 30-120s──> IDLE
 |               HUNT (35%) ──prepare+travel+patrol──> endHunt ──> IDLE
 |               TRAVEL (10%) ──boat──> new city ──180s pause──> IDLE
 |
 |──attacked──> COMBAT (fight) / FLEEING (flee) / IGNORE
 |              COMBAT: chase + attack attacker
 |              FLEEING: run opposite direction
 |              IGNORE: maybe hit once, resume normal (outleveling)
 |              All: timeout after 120s or 5-min HP stalemate → IDLE
 |
 |──PK roll (30% skull cap)──> PK_ATTACK ──timeout 15s (resets on damage dealt)──> COMBAT / IDLE
 |
 |──vigilante (5% per new PKer)──> COMBAT
```

### States

| State | ID | Description |
|-------|----|-------------|
| INACTIVE | 0 | Bot loaded but not active (deactivated by scheduler) |
| IDLE | 1 | Walking between POIs |
| DWELLING | 2 | Standing at a POI (5-30 minutes) |
| COMBAT | 3 | Fighting back against an attacker (chase + attack) |
| FLEEING | 4 | Running away from an attacker |
| TRAVELING | 5 | Traveling to another city via boat |
| PK_ATTACK | 6 | Randomly attacking a nearby player |
| HUNTING | 7 | Following hunt script waypoints, killing targets |
| PARTY | 8 | Following a human player in a party |

---


</details>

<details>
<summary><strong>Behaviors</strong></summary>


### 1. POI Walking (IDLE → DWELLING)

- POIs loaded from MySQL table `bot_city_pois` (was hardcoded C++ prior to 2026-03-14) + dynamically generated at runtime:
  - `_depot_outside`: non-PZ tile at PZ boundary near depot (via `findPZBoundaryTile()`)
  - `_boat_nearby`: random walkable tile within 4 sqm of boat NPC position (via `findRandomTileNear()`)
- Weighted random selection: depot(40), depot_outside(20), boat(20), temple(10), adventurer_stone(10), shop(5), npc(3)
- Bot walks to POI using city routes, then A* for the final leg to dynamic tile positions
- Dynamic POIs bypass the auto-locker-walk on city route completion (bot walks to actual target)
- On arrival (within 3 tiles, same z), enters DWELLING for 300-900s (or 30-120s for shop/npc)
- Tracks visited POIs by name to avoid repetition
- **Depot arrival**: walks to nearest depot locker, waits 20-60s, then rerolls: 40% stay, 30% PZ roam, 30% step outside
- **No floor changes during idle/dwelling PZ roaming**: if a dwell walk target is on a different z-level, the walk is cancelled. `findClosestNonPZTile()` only scans same-z. This prevents bots from going up/down stairs while idling and getting stuck.
- If pathfinding fails 5x consecutively: emergency teleport to temple
- **Door opening**: `goTo()` attempts `tryOpenDoors()` when all pathfinding fails, clearing blocked paths

### 1b. Adventurer's Stone Trip (POIType::ADVENTURER_STONE)

A special POI type that turns into a guided dungeon trip on arrival:

- **POI position** = city's temple PZ (same as `temple` POI), so the bot walks to the temple via the standard temple-POI navigation. Never teleported.
- **On arrival**, `BotEngine::doIdle` calls `startAdventurerStoneTrip()` instead of going to DWELLING:
  - Guards: `tile->hasFlag(TILESTATE_PROTECTIONZONE)`, `!tile->hasFlag(TILESTATE_HOUSE)`, `!player->isPzLocked()` — exact mirror of `data-otservbr-global/scripts/actions/adventurers_guild/adventurers_stone.lua` `onUse()`.
  - Range check: bot's position must be inside one of `kAdventurerStoneTempleRanges` (15 entries — copied from `config.Temples` in the action file, minus Ankrahmun and Darashia which have walkZ-vs-PZ-z mismatch).
  - On match: `setStorageValue(STORAGE_ADVENTURERS_GUILD_STONE, townId)` + `internalTeleport((32210,32300,6))` + magic effects. The storage tells the exit-forcefield's `aid:4253` MoveEvent which town to send the bot back to.
- **Trip phases** (`bot.advStonePhase`): 0=following_route, 1=dwelling, 2=stepping_on_forcefield. The trip handler runs from `tick()` AFTER the floor-change state machine but BEFORE state dispatch — fully takes over the AI for the duration.
- **Route**: 16 waypoints loaded from `bot_city_routes` with `town_id=0` sentinel (one global shared route stored in `adventurerStoneRoute_`). Waypoints traverse the dungeon: 6 nodes → `stairs_down` (clientID 413, z=6→7) → 6 nodes → `stairs_up` (clientID 1958, z=7→6) → 4 nodes → `stand` on the magic forcefield at `(32210, 32292, 6)`. The `stand` arrivalDist=0 guarantees the bot steps onto the exact forcefield tile.
- **3-way idle sub-activity** (pre-selected at trip start by `selectAdvStoneSubActivity()`, equal-chance roll):
  - **Mode 0 — route waypoint**: dwell at the random NODE waypoint picked by `pickAdventurerStoneIdleIdx()` for 60-300s. Original behavior.
  - **Mode 1 — reward chest**: walk to a free walkable tile adjacent to `(32192, 32292, 7)`, stand there for 60-300s as if having just opened the daily chest. Tile picked via `collectAdjacentFree(chestPos)` — `!TILESTATE_BLOCKSOLID && getCreatures().empty()`.
  - **Mode 2 — exercise dummy training**: random Lasting Exercise weapon picked from `{35285 sword, 35286 axe, 35287 club, 44067 shield, 50295 wraps, 35288 bow, 35289 rod, 35290 wand}`. Melee weapons → adjacent tile to dummy at `(32196, 32296, 7)` via `collectAdjacentFree`. Ranged → radius-5 same-z tile with LOS via `collectFreeWithLOS` (`g_game().map.isSightClear`, same API the bot's combat targeting uses). On arrival, `g_actions().useItemEx(player, weapon, dummyPos, dummyStackPos, weapon, false)` triggers the real `exercise_training_weapons.lua` action — bot trains for **180-1800s (3-30 min)** with all visual effects (hit-area + distance animation), gaining real skill/mana via the action's `addSkillTries`/`addManaSpent`. **Dummy stackpos resolved at runtime** via `Tile::getThingIndex(dummyItem)` after locating the dummy via `Item::isDummy()` — passing the default stackpos=0 would resolve to the ground tile and the action would silently no-op.
- **z-gate on sub-activity**: chest/dummy are on z=7; sub-activity rolls only fire when the chosen idle waypoint is also on z=7. If `pickAdventurerStoneIdleIdx()` returns a z=6 waypoint (wps 12-15), force mode 0. Prevents cross-floor walk-to-target deadlock (`goTo` refuses FC while `followingCityRoute=true`).
- **30s walk deadline (phase 1)**: from idle-wp arrival to dwell-target arrival. Hit → demote to mode 0 (clear weapon/target, start 60-300s dwell at current pos). Covers tile-occupied races between trip-start scan and arrival.
- **Lasting Exercise weapons in inventory**: each bot's backpack receives all 8 weapons with 14400 charges at first `equipBot()` call. Per-id idempotent via `g_game().findItemOfType(player, id, true, -1)` — silent no-op on subsequent activations. Persisted in `player_items` so daily restarts don't add duplicates. 14400 charges = ~8h continuous training; daily server restart implicit reset is plenty.
- **Trip end**: bot steps on forcefield → server-side `aid:4253` MoveEvent (`data-otservbr-global/scripts/movements/teleport/adventurers_guild.lua`) reads the storage and teleports the bot back to the matched town's temple. The trip handler detects the position-jump via temple-range membership (not raw distance), with a 30s deadline as fail-safe.
- **No reservation**: multiple bots can be in the dungeon at once. Implicit cap on Mode 2 melee concurrency = 8 (dummy adjacent slots); ranged Mode 2 spreads across radius-5 minus occupied; overflow rolls demote to mode 0.
- **Excluded towns**: Ankrahmun (10), Darashia (13), Gray Beach (18 — has range but no temple POI). Bots from these towns never roll the POI; if manually teleported into a known temple range they can still trigger the trip.
- **Cleanup hooks**: `pauseBotForDeath`, `exitCombat`, `exitPK`, and the `stop` admin command all call `endAdventurerStoneTrip()`, which calls `stopAdvStoneTrainingIfActive(bot)` first → `player->setTraining(false)` if a training session is active, then clears all sub-activity state. Also called from `forceDeactivateBot` and `forceDeactivateBotForReload` so hot-reload teardown doesn't leak orphan Lua `addEvent` training loops (the Lua loop lives in main-binary state and survives `dlclose`/`dlopen`).
- **Test triggers**:
  - SQL `INSERT INTO bot_commands (bot_name, command) VALUES ('<Bot>', 'advstone');` — random mode roll (production)
  - `advstone chest` — force chest mode (debug)
  - `advstone dummy` — force dummy mode, random weapon (debug)
  - `advstone dummy 35290` — force dummy with wand 35290 (debug, e.g. to validate ranged-weapon path)
  - `advstone waypoint` — force route waypoint dwell (debug)
  - `/cavebot <bot> advstone [<mode> [<weaponId>]]` — same args via talkaction
  - One-shot: the forced mode/weapon is consumed by the next trip and resets to random.
- **Intentional behavior**: bot does NOT fight back during a trip — `doAdventurerStone()` short-circuits before `doSelfDefense` and the state-dispatch switch. If killed mid-trip, the death hook aborts cleanly. Acceptable trade-off because the dungeon is non-PvP territory for typical populations.

### 2. Inter-City Travel (Boat-Based)

- 0.1% chance per tick while IDLE/DWELLING to start traveling
- Routes defined in `TRAVEL_DESTINATIONS` (simulating boat connections)
- Uses `startCavebotTravel()` for proper multi-step boat journey:
  1. Walk to source city boat NPC (via route graph or runtime pathfinding)
  2. Say "hi" to NPC, wait 3-10 seconds (randomized delay)
  3. Teleport to destination city boat position (with effect)
  4. Walk from destination boat to temple (via route graph or runtime pathfinding)
- **Runtime pathfinding fallback**: If no DB route exists for boat→temple or temple→boat,
  uses `goTo()` iteratively ("walking_to_poi" phase) with 30-tick failure teleport safety net
- Updates `player:setTown()` on arrival
- Legacy `TRAVELING` state (instant teleport) is removed — recovered to IDLE if ever reached

### 3. Self-Defense: Fight / Flee / Ignore

When attacked, the bot makes a one-time decision:

**Normal case** (similar levels): 50% fight, 50% flee

**Outleveling case** (bot level >= 6x attacker level):
- 17% fight (1/6), 83% ignore (5/6)
- "Ignore" = optionally hit back once (50% chance), then continue IDLE/DWELLING
- No fleeing — high-level bots don't run from weaklings

**Fight behavior (COMBAT)**:
- Chase attacker using manual pathfinding (`chaseTarget`)
- Attack with vocation-appropriate spell when in range + line of sight
- Track attacker by ID with 60-second skull-based memory
- Follows through stairs/z-changes and opens doors

**Flee behavior (FLEEING)**:
- Run in opposite direction (15 tiles)
- Only flee while attacker is on same z-level

### 4. Combat Mechanics

**Equipment**: Loaded from MySQL `bot_equipment` table (key = level×10 + baseVoc). `equipBot()`
runs on activation and hunt reroll. Paladin level 150+: jungle bow (35518) + jungle quiver
(35524) + 100× non-decaying diamond arrows (35901, auto-loaded into quiver). Ammo never
consumed (`isBotPlayer()` bypass in weapons.cpp).

**Bot Equipment Buffs** (imbuements + forge tiers — applied automatically by `equipBot`):

*Imbuements (lv 50+)*. `applyBotImbuements` walks a per-`(baseVoc, slot)` priority list and
applies the first accepted tier-3 ("Powerful") imbuement to each empty imbuement subslot.
Priority always leads with **Critical Hit** then **Life Leech**, then:
- Weapon (LEFT) and head: voc skillboost (sword/axe/club for EK, distance for RP, magic for
  sorc/druid) → mana leech
- Armor + legs: all 6 elemental protections (death/earth/fire/ice/energy/holy)
- Feet: speed
- Shield (RIGHT when not a quiver): shielding → protections

The tier-3 ID lookup is a static cache built once per `.so` load by scanning `g_imbuements()`
ids 1..69 for `getBaseID()==3`. The system honors `items.xml` category/tier acceptance via
`hasImbuementType(cat, 3)` — items that don't accept a category are skipped. Idempotent via
the in-subslot `getImbuementInfo` check, so re-running `equipBot` (hot reload, hunt reroll)
doesn't double-apply. Duration is `0xFFFFFF` (~194 days in-combat decay) with
`startImbuementDecay` skipped — keeps the 1Hz decay scheduler idle for effectively-permanent
items. Stat add/remove on item swap is handled by the existing post-notification chain
(`onPlayerDeEquip`→`removeItemImbuementStats`, `onPlayerEquip`→`addItemImbuementStats`); the
manual `addItemImbuementStats` call here only covers the case where the item is already
sitting in the slot when we apply the imbuement.

*Forge tiers (lv 150+)*. `applyBotForgeTiers` sets `item->setTier(N)` on:
- LEFT → **Fatal/Onslaught** (combat.cpp:2502)
- HEAD → **Momentum** (player.cpp:10378)
- ARMOR → **Ruse/Dodge** (player.cpp:12057)
- LEGS → **Transcendence** (player.cpp:10461)
- FEET → **Amplification** — multiplies the other four chances (combat.cpp:2504, etc.)

Scale (level→tier): <150→0, 150-199→1, 200-249→2, 250-299→3, 300-349→4, 350-399→5,
400-449→6, 450-499→7, 500-599→8, 600-699→9, 700+→10. `setTier` is a silent no-op on items
with `upgradeClassification==0`; an explicit guard keeps the log line accurate. Skipped
when current tier already equals target.

*Visible effects*. Critical hits ("CRITICAL!" floating text) come from the Critical Hit
imbuement's stat bonus (player skill total) consumed in `Combat::applyExtensions`. Fatal
hits ("FATAL!" floating text) come from weapon tier consumed in the same function. Momentum
gates on `CONDITION_INFIGHT`, which is added by `Player::onAttackedCreature` →
`addInFightTicks` on every attack — so it fires for bots in combat. Transcendence uses
`checkLastAggressiveActionWithin(2000)` and triggers naturally.

*PvP scope*. Bot→player damage uses `target:addHealth(-dmg, type, attacker)` Lua bypass that
intentionally skips `Combat::applyExtensions`. Fatal/Critical do NOT fire on bot→player
attacks — by design (the bypass exists to skirt PvP damage filters, not to apply forge
benefits).

**Dynamic Spell System**: Spell tables are built at init by joining the server's spell registry
(vocation, level, range, cooldown, spell group) with parsed Lua files (`data/scripts/spells/attack/`
and `data/scripts/runes/`) for combat type, area pattern, and damage formula coefficients.

- `buildSpellTables()` runs at constructor + `loadHuntData` (covers hot-reload)
- Per-vocation arrays: `resolvedSingleSpells_[baseVoc]` and `resolvedAoeSpells_[baseVoc]`
- Runes: `resolvedAoeRunes_` (Ava, GFB, Thunderstorm, Stone Shower, Explosion) + `resolvedSdRune_`
- Knight spells use `Weapons::getMaxWeaponDamage()` for damage estimation (skill-based formula)
- WoD spells (needLearn=true) work for bots — `playerSpellCheck()`, `upgradeSpellsWOD()`,
  and `revelationStageWOD()` bypass the learn/wheel checks for `isBotPlayer()`

**Unified scoring** in `castSpell()`: All attack options scored by estimated damage × targets:
1. AoE spells (`selectAoeSpell` → validates targets in area via `isInAoeArea()`)
2. AoE runes (Ava/GFB/Thunderstorm/etc — min 2 targets, best position via `findBestRunePosition`)
3. Single-target spells (`selectAttackSpell` → best damage vs resistance)
4. SD rune (single-target, range 5, vs death resistance)
Highest total score wins. Each option checks level, cooldown, mana, and target resistance.

**Rune system**: No artificial level gate — each rune checks its own level/maglevel requirements
via `g_spells().getRuneSpell(runeId)`. Knights excluded from runes entirely.

**PvP damage bypass**: Standard Tibia PvP rules block `castSpell()` damage between players.
After casting for visuals, bots apply direct damage via `target:addHealth(-dmg, combatType, player)`
which bypasses PvP checks (passes nullptr to `combatChangeHealth`).

**PvP mana budget**: During PvP combat, bots have a total mana budget of 2.5x maxMana
(equivalent to 5 refills from 50% to 100%). Both big refills (in `doCastSpell` when mana
< 30%) and passive regen (in `doHealing`) count toward this budget via `state.pvpManaSpent`.
Once exhausted, all mana regen AND HP healing stop — the bot drains and dies from damage.
Budget resets when exiting PK/combat.

**Healing**: When HP < 60%, cast "exura vita" (heals level×3+100 HP)

**HP Stalemate Detection**: During combat, HP percentages of both combatants are sampled
every 60 seconds. If 5 consecutive checks (5 minutes total) show neither HP changed by >10%,
combat is exited as a stalemate. Any significant HP change resets the counter.

**SecureMode & diamond arrows**: Bots keep `secureMode=true` at all times (even during PvP).
The Lua `onTargetCombat` callback in `creature.lua` was fixed to add a skull check (matching
C++ `canTargetCreature()` logic): damage goes through against skulled targets (yellow/red/black),
blocked against unmarked players. This prevents AoE diamond arrow splash from skulling innocents.
`Player:getSkullClient(target)` Lua binding was added for this check.

**Combat loop fix**: `doSelfDefense()` spectator scan now requires `isPzLocked()` on potential
attackers. Prevents stale `getAttackedCreature()` (not auto-cleared when `CONDITION_INFIGHT`
expires) from causing rapid combat enter/exit cycling.

**PZ pathfinding**: Bots use `FLAG_IGNOREBLOCKCREATURE` when pathfinding through protection
zone tiles, allowing them to path through creatures in depot areas (like real Tibia push behavior).

**PZ-lock stuck timers**: When PZ-locked, bots reset the 5-minute stuck timer in city routes,
walk-to-boat, and boat-to-depot paths — PZ-lock wait is legitimate, not a stuck condition.

**Post-death FC recovery**: `pauseBotForDeath()` clears `s_returnPos` and `s_returnStartTime`
to prevent stale pre-combat positions from triggering floor changes after temple respawn.

### 5. Chase Target (Following)

Uses manual pathfinding instead of `setFollowCreature` (unreliable for bot players).

- `getPathTo(target, 0, attackRange, fullSearch=true, clearSight=true)`
- Recalculates when target moves 3+ tiles
- Uses `clearSight=true` to route around walls
- Falls back to `clearSight=false`, then offset positions
- **Door opening**: If all pathfinding fails, scans adjacent tiles for closed doors
  and opens them via `g_actions().useItem()` using all four door tables (Key, Custom, Quest, Level).
  Quest doors pass because `Player::getStorageValue()` returns 999999 for bots (unset keys).
  Level doors pass because `level_door.lua` has `isBotPlayer()` bypass.
  Failed doors get 60s cooldown (`s_failedDoors`)

### 6. Z-Level Pursuit (Stairs/Ramps/Ropes)

When target changes z-level during combat:

1. `targetLastSameZPos` tracks target's last position on the same z
2. `findZTransitions()` scans radius around that position for:
   - `TILESTATE_FLOORCHANGE` tiles (stairs, ramps)
   - `BOT_LADDER_IDS` items (ladders → UP)
   - `BOT_SEWER_ID` item 435 (sewer grates → DOWN)
   - Shovel holes (ground IDs 593, 606, 608, 867, 21341 → DOWN)
   - Rope spots (ground IDs 386, 421, 12935, etc. + stacked items → UP)
3. Bot walks to the transition tile and uses it:
   - **Stairs/ramps**: step-on (internalMoveCreature)
   - **Ladders**: use item via `g_actions().useItemEx()`
   - **Sewers**: use item via `g_actions().useItemEx()`
   - **Shovel**: must be adjacent (dist=1), uses temp `Item::CreateItem(3457)` via `g_actions().useItemEx()` on ground tile. Cannot use at dist=0 (skips to next transition)
   - **Rope**: two-step process — at dist=1, step onto rope spot; at dist=0, use temp `Item::CreateItem(3003)` via `g_actions().useItemEx()`. Teleports up via `onUseRope()` Lua handler
4. Z-recovery is disabled during COMBAT/FLEEING/PK_ATTACK states
5. After z-change, normal chase resumes on the new floor
6. In active COMBAT state, z-pursuit works regardless of attacker skull.
   Outside combat, z-pursuit only engages for PK-skulled attackers (≥ white).
7. Hunt waypoint hints: when a previous waypoint has type "rope"/"ladder"/"hole",
   its position is used as an extra search center for the floor-change scanner.

### 6b. Teleport Portal Auto-Detection

Hunt scripts may place patrol waypoints on teleport tiles (e.g., magic forcefield item 1949)
where consecutive waypoints have different z-levels but are far apart — not a staircase.

**Distance heuristic**: When two consecutive waypoints differ in z AND are >30 tiles apart in xy,
the engine treats them as a teleport (not stairs). This avoids the FC state machine entirely:

1. `implicitFc = false` → arrivalDist stays 0 for STAND waypoints
2. `isTeleportWp = true` → uses walk-adjacent + `internalMoveCreature(FLAG_NOLIMIT)` pattern
   (same as FC STEPPING_ON) because A* rejects magic forcefield tiles as destinations
3. After teleport: position-jump detection (`posDiff > 10` or `zJump`) fires, clears walk state,
   and auto-advances `huntWaypointIdx` if bot was near the current waypoint before the jump
4. Bot lands at/near the destination waypoint and continues patrol normally

**No DB changes required** — works automatically with any hunt script that has teleport waypoints.

### 7. Vigilante PKer Attack

- IDLE/DWELLING bots scan spectators for players with skull ≥ 3 (white/red/black)
- **One-shot decision per PKer**: When a new PKer first appears in visible range,
  roll 5%. If no → remember this PKer in `seenPKers` (won't re-roll until they leave).
  If yes → enter COMBAT (always fight, never flee from PKers)
- **Level-based ignore**: If bot outlevels PKer by 6x+, skip the 5% roll entirely
  (bot doesn't care about weak PKers)
- `seenPKers` stores timestamps: PKers who left visible range are only cleared
  after 60 seconds have passed since they were last visible. This prevents
  rapid re-rolling when a PKer moves in/out of screen range.

### 8. Random PK

- IDLE bots have two separate rolls:
  - Real players: 0.25% per tick (1/400)
  - Other bots: 0.025% per tick (1/4000)
- **30% Skull Cap**: PK is skipped if ≥30% of active bots already have red or black skulls
  (`countSkulledBots()` checks all active bots for `skull >= SKULL_RED`)
- Checks PZ for both bot and target
- Enters PK_ATTACK state for 15 seconds max
- Supports z-pursuit (follows through stairs)
- Can be forced via `pk <target name>` cavebot command
- **Secure mode**: `setSecureMode(false)` on PvP entry (allows unjustified attacks).
  Restored to `true` on `exitCombat()`/`exitPK()`.

### PvP Combat Attack System

During PvP (COMBAT and PK_ATTACK states), bots use the same unified `castSpell()` scoring
as PvE, with these PvP-specific behaviors:

- **All attack types available**: AOE spells (with directional wave optimization), AOE runes
  (GFB, avalanche), SD runes (MS/ED only), and single-target spells all score against the target
- **Player target scoring**: `selectAoeSpell()` receives the PvP target as `pvpTarget` parameter,
  so wave spells are aimed at the player. AOE rune scoring falls back to the player's position
  when no monsters are nearby
- **SD runes**: Restricted to MS (baseVoc 1) and ED (baseVoc 2). Works against player targets
- **Bystander safety check**: Before firing AOE spells or runes in PvP, scans the AOE area for
  non-target players/bots. If any bystander would be hit, downgrades to single-target spell or SD
- **Mana budget**: 2.5x max mana for PvP (prevents infinite spell spam against players)

### 9. Chat

- 1/300 chance per tick to say a contextual phrase
- Categories: idle, depot, shop, temple, npc, travel, combat, flee, greeting
- 60-second cooldown between messages

### 10. Party System (PARTY state)

Any player (not just gods) can summon bot players into a party using `/party ek,ed,ms` or
`/cavebot party ek,ed,ms`. Bots follow the player, mirror their attack target, and perform
vocation-specific roles. Entirely implemented in C++ (`bot_engine.cpp`), no Lua party AI.

**Commands** (two entry points, same C++ backend):
- `/party ek,ed,ms` — Player-facing command (delegates to C++ via `Game.botCommand`)
- `/party leave` — Dismiss all party bots, restore them to previous state
- `/cavebot party ek,ed,ms` — Admin command variant (same behavior)
- `/cavebot party leave` — Admin dismiss variant

**Bot Selection (5-Tier Priority)**:

| Tier | Criteria |
|------|----------|
| 1 | Inactive bots (`!bot.active`) |
| 2 | Active bots in IDLE state |
| 3 | Active bots in DWELLING state |
| 4 | Active bots in TRAVELING, COMBAT, or FLEEING state |
| 5 | Active bots in any state (HUNTING, PK_ATTACK — last resort) |

- Level range: `[playerLevel * 2/3, playerLevel * 3/2]`
- Shuffle within each tier for randomness
- Max 4 bots per party
- Skip bots already in a party

**Party AI (doParty)**:
1. **Validate**: Check leader exists, party exists. If leader gone, find another real player in party. If none, disband.
2. **Self-heal**: Calls existing `doHealing()` for HP restore, paralysis cure
3. **Mana restore**: Top up to max if < 50%
4. **Knight tank**: EK bots cast "exeta res" (Challenge, spell ID 93) every cooldown (2s) when monsters nearby
5. **Mirror target**: Attack whatever the leader attacks. Leash check: abort chase if > 7 tiles from leader or LOS broken
6. **Druid healing**: Priority order:
   - Self critical (< 50% HP): strongest self-heal
   - Mass heal ("exura gran mas res", spell ID 82): if 2+ members < 70% HP, within range, off cooldown
   - Targeted heal ("exura sio \"Name\"", spell ID 84): Leader → Knight bots → Others, if < 70% HP
7. **Follow leader**: Pathfind if 3-15 tiles away (same z), teleport if > 15 tiles or different z

**State Preservation**:
- Full `BotState` copy saved before entering PARTY mode
- On exit: active bots teleport back to exact pre-party position, all state fields restored
- Inactive bots: deactivated, teleported to INACTIVE_POS, cast broadcasting disabled
- Hunt reservations preserved (tier 5 bots resume hunting at exact waypoint)
- PvP toggle (`secureMode`): set to true on join, restored to previous value on exit

**Static State Maps** (in `bot_engine.cpp`, no ABI change):
- `s_partyLeaderId[guid]` — leader creature ID
- `s_prePartyState[guid]` — full BotState snapshot
- `s_partyWasInactive[guid]` — whether bot was inactive before party
- `s_partyPrevSecureMode[guid]` — previous secureMode value
- `s_lastLeaderPos[guid]` — last known leader position
- `s_lastPartyHealTime[guid]` — druid heal cooldown tracking

### 11. Autonomous Party Hunts (bot-only)

Bots can form 4-member party hunts autonomously — one EK leader + 3 support bots (ED, MS, RP).
Triggered via `partyhunt` command (DB `bot_commands` table) or automatic reroll (10% chance).
Entirely in C++ (`bot_engine.cpp`).

**Formation**:
- EK bot selects a hunt script, then recruits 3 support bots from same city
- Support selection: same 5-tier priority as player parties, level range `[EK * 2/3, EK * 3/2]`
- Roles assigned by base vocation: ED=healer, MS=DPS mage, RP=DPS ranged
- Duration: 2-3 hours randomized

**EK Leader Behavior**:
- Runs normal hunt system (travel, patrol, combat) with existing cavebot logic
- Casts `exeta res` (challenge) every ~2s when monsters within 1 tile
- Support bots follow EK through all hunt phases

**Support Bot AI (doPartyHunt)**:
1. **Self-heal** + **mana restore** (infinite mana for party bots)
2. **Role dispatch**: healer/DPS mage/DPS ranged
3. **Follow EK** via `followPartyHuntLeader()`

**Follower Z-Change Delay**:
- When the EK changes z-level (stairs/teleport), followers wait 2 seconds before teleporting
- Per-follower detection: each follower tracks the leader's last known z independently (immune to tick processing order)
- After delay, followers teleport to the EK's **current position** (not stale landing pos)
- Walk queue cleared during delay to prevent walking onto stairs
- `s_lastZChangeTime` explicitly set during teleport (bot.lastPos=currentPos prevents auto-detection)
- `tryPartyAoeReposition()` checks z-change grace period to prevent spell spam after teleport

**EK Engagement Gate**:
- Support bots do NOT attack until EK is within melee range (Chebyshev dist ≤ 2) of its target
- Prevents supports from luring monsters before EK has aggro
- Exception: ED healing (exura sio) always runs regardless of EK distance

**Approach Logic**:
- Support bots pathfind toward **EK's position** (not the target monster)
- Keeps bots behind the front line — EK leads, supports follow
- keepDistance: ED/MS=3 (script default), RP=0 (melee, can tank in party hunts)
- Approach cooldown prevents per-tick A* overhead

**Party Member Walk-Through** (`player.cpp`):
- Bot players in the same party can always walk through each other (`canWalkthrough`)
- Prevents EK from getting blocked by teammates in non-PZ areas

**Yield Logic**:
- Support bots detect when they're in the EK's walking path (next step position)
- Step perpendicular to let EK pass
- Checks 2 perpendicular directions, picks walkable + unoccupied tile

**Party Member Spread**:
- Prevents support bots from stacking on the same tile
- `tryPartyMemberSpread()`: detects overlap with another party member, steps to best adjacent tile
- Scored: LOS to EK (+10), within keepDistance (+5), maintains min distance (+3), no party overlap
- Only the higher-guid bot spreads (prevents simultaneous opposite-direction oscillation)
- 2s cooldown + 3s approach suppression after spreading

**Keep Distance + Retreat**:
- ED/MS maintain vocation-appropriate distance from monsters (3 tiles default)
- RP keepDistance = 0 in party hunts (can tank, needs close for AoE)
- Multi-monster retreat: scan spectators within keepDist+2, compute threat centroid, A* retreat
- Retreat cooldown prevents oscillation

**AoE Repositioning** (`tryPartyAoeReposition`):
- Evaluates nearby tiles (radius 5) for optimal AoE spell placement
- Candidate tiles: current pos + 8 adjacent + 8 arc positions at keepDistance from EK + 4 centroid-approach tiles
- Centroid-approach: samples 4 tiles along the line from bot to monster cluster center
- Scores tiles by: monster hits × spell damage, requires 30% improvement to reposition
- keepDistance relaxed by -1 tile for AoE evaluation (bots can get 1 tile closer than normal)
- Z-level check: candidate tiles must be on leader's z, monsters must match tile z
- Z-change grace period: no AoE evaluation for 2s after z-change teleport
- `castAoeSpell()`: re-syncs position + skips cast if 0 monsters at actual position
- All AoE scans use `find<Monster>` (not `find<Creature>`) to exclude players/bots/NPCs
- Secondary spell group cooldown check prevents puff from dual-group spells (e.g. ultimatestrikes)
- Spells below level 10 filtered for bots over level 10 (removes infir-tier spam)

**Partyhunt Force-Clear**:
- `partyhunt <script_id>` command force-clears the spawn reservation from the current holder
- Aborts the holding bot's hunt, sends it to temple/idle
- Then proceeds with party formation on the specified script

**Death / Dissolution**:
- 3 party deaths with 0 kills → dissolve + disable hunt script
- EK leaving/resupplying → dissolve party
- Support bots exit to IDLE state

**Static State Maps**:
- `s_partyHuntMembers[partyHuntId]` — member GUIDs
- `s_partyHuntLeaderGuid[partyHuntId]` — EK leader GUID
- `s_botToPartyHunt[guid]` — reverse: bot → party hunt ID
- `s_followerLastLeaderZ[guid]` — per-follower last known leader z-level
- `s_followerZChangeDetected[guid]` — per-follower z-change detection timestamp
- `s_retreatUntil[guid]` — retreat cooldown
- `s_approachCooldown[guid]` — approach pathfind cooldown
- `s_spreadCooldown[guid]` — party member spread cooldown

### 12. Bot Market System (Phase A)

Populates the in-game market with realistic bidirectional supply/demand. The active loaded bot set (~200, scoped by `botPlayersOnline`) creates offers, accepts real-player SELL offers at bargain prices, and fulfills real-player BUY offers — all transactions go through the canonical server flow so real players experience the market exactly as if they were trading with real counterparties.

**Architecture:**
- **Price reference table** (`bot_market_item_prices`, schema migration 57): populated by one-off importer (`tools/bot_price_importer/`) from three sources in priority order — Cipsoft `appearances.dat` protobuf NPC sale/buy prices, NPC Lua shopBlock scrape (supplemental), TibiaWiki API (player-market `value` field), heuristic fallback (`max(npc_sell × 1.5, weight × class_mult, 100)`). Total 39,969 items, 5,322 with market_max prices.
- **C++ wrappers** (`src/game/game_bot_market.cpp`): `Game::botCreateMarketOffer / botAcceptMarketOffer / botCancelMarketOffer` mirror the inner work of `playerCreateMarketOffer / playerAcceptMarketOffer / playerCancelMarketOffer` minus the UI gates (`isInMarket`, `isUIExhausted`, depot prerequisites). Standard 2% server fee still applies. Real players on the other side receive money via `setBankBalance + savePlayer` and items via `processItemInsertion(playerInbox, ...)` — same primitives as the user-facing flow. Plus `IOMarket::getOfferById` helper for direct lookup by id.
- **Lua bindings** (`game_functions.cpp`): `Game.botCreateMarketOffer / botAcceptMarketOffer / botCancelMarketOffer`.
- **Price cache + helpers** (`data/scripts/lib/bot_market_data.lua`): lazy-loaded `BotMarket.prices[itemId]` from MySQL; helpers `computeFloor`, `computeListingPrice`, `rollTier`, `rollStackSize`, `pickActiveBot`. Constants: tier distribution T0=80% T1=15% T2=4% T3=1%, stack sizes for stackables 250-2000, bins equally weighted (equipment/reagents/potions/runes/creature_products at 19% each, "other" 5%).
- **Bot scoping** (`BotMarket._loadBots`): the seller/buyer/fulfiller passes operate on the loaded set only via `JOIN bot_active_players` (migration 60) — same source the @cast list uses, so market participants match the cast viewer exactly and seller-pass volume scales with `botPlayersOnline`. With 997 in the DB pool and config=200, only the 200 loaded bots create new offers. **Race fix** in `ensureLoaded`: `bot_active_players` is populated via async `g_databaseTasks().execute()` from `bot_engine.cpp::registerBot`, so the table may be empty when `BotMarket.loadAll()` lazy-fires at the first pass (~30s, while BotStartup is still staggered-loading and DatabaseTasks is draining). If `botGuids` comes back empty, retry `_loadBots()` on each subsequent pass until the queue drains — prevents permanent market disable from a startup race that previously would set `loaded=true` with `botGuids={}`.
- **Funding** (`bot_market_funding.lua`): 30s post-startup, top up all 997 bots in the DB pool to 100kkk via in-memory `setBankBalance` + raw SQL backstop. Intentionally NOT filtered to the loaded set — funding runs only once at startup, so seeding the entire pool means bots that get loaded later (e.g. if `botPlayersOnline` is raised in a future restart) already have balance ready without a re-funding pass.
- **Four-pass scheduler** (`bot_market.lua`):

  | Pass | Polling tick | Random fire interval | Per-fire batch | Behavior |
  |---|---|---|---|---|
  | **Seller** | 30s | 30–120s | `SELLER_BOTS_PCT`% of LOADED set (5% of `botPlayersOnline`=200 → 10 bots, jittered ±30% → ~7–13, ramp-scaled) × 1–3 offers each | 60% SELL / 40% BUY, 25% anonymous, per-bot cap = config.lua `maxMarketOffersAtATimePerPlayer` (currently 200, synced into `BotMarket.MAX_OFFERS_PER_BOT` at startup; C++ wrapper `Game::botCreateMarketOffer` enforces the same cap canonically), vendor-arbitrage floor `max(npc_sell × 1.05, npc_buy × 0.6)`. Insert via `Game.botCreateMarketOffer`. |
  | **Buyer** | 60s | 600–1800s | 1–3 acceptances | Scan real-player SELL offers (`sale=1`), accept if listed price < market_max × 0.9 AND 50% coin flip. Bot pays bank, items vanish into bot inbox, real seller's bank credited. |
  | **Fulfiller** | 60s | 600–1800s | 1–3 fulfillments | Scan real-player BUY offers (`sale=0`), fulfill at listed price if ≥ market_max × 0.95 AND 50% coin flip. Items conjured by bot, delivered to real buyer's inbox via `processItemInsertion`. Escrow money flows from buyer's pre-debited balance to bot. |
  | **Monitor** | 5 min | every tick | — | Logs active offer counts by side (`bot=X (S=… B=…), real=Y (S=… B=…)`) and ramp%. |

- **Ramp-up scaler**: first 4 hours after server start, multiply per-pass batch size by `min(1, hoursElapsed/4)` so the market doesn't dump 1200 listings at the start.
- **One-off prepopulation** (`tools/bot_price_importer/prepopulate_market.py`): seeded 20,053 bot offers across all 4,998 marketable items with tight per-item variance (~±0.2% of market_max). Run once with canary stopped.

**Validated end-to-end on production**: bot 65001 bought The Hulk's daramian mace at 10gp (market_max 750gp = clear bargain). Bot 65007 fulfilled Legolas's BUY for 3 christmas present bags at 223,004gp each. Real players received money/items via the canonical code path.

**Files:**
- C++: `src/game/game_bot_market.cpp`, `src/game/game.hpp`, `src/io/iomarket.cpp/.hpp`, `src/lua/functions/core/game/game_functions.cpp/.hpp`
- Lua: `data/scripts/lib/bot_market_data.lua`, `data/scripts/globalevents/bot_market.lua`, `data/scripts/globalevents/bot_market_funding.lua`
- Schema: `data-otservbr-global/migrations/57.lua`
- Importer: `tools/bot_price_importer/` (gitignored)

---


</details>

<details>
<summary><strong>Key Configuration (`BOT_CONFIG`)</strong></summary>


`BOT_CONFIG.TARGET_ONLINE` reads from `configManager.getNumber(configKeys.BOT_PLAYERS_ONLINE)`
at script-load time, with a fallback of 200. Configure in `config.lua` via
`botPlayersOnline = 200`. The setting only takes effect on `systemctl restart canary` —
a hot `/cavebot reload` does not re-stratify the active set.

The MySQL bot pool currently holds 997 bot characters (account_id=65000, IDs 65001-66004
skipping reserved IDs 65201-65207). `BotStartup` picks `TARGET_ONLINE` of them at
stratified indices `floor(i * 997 / target)` so any value in `[1, 997]` samples
roughly evenly across all 4 vocations, 14 towns, and the full level range (8-1000).
Voc/town assignment in the SQL is Fisher-Yates shuffled (not periodic) to keep the
sample balanced even when the picker's stride aligns with the original period-4/14
structure. Re-seed via `python tools/bot_population_generator/generate.py`.

| Key | Default | Description |
|-----|---------|-------------|
| THINK_INTERVAL | 1000ms | AI tick rate (normal behaviors) |
| FAST_THINK_INTERVAL | 100ms | AI tick rate during active navigation |
| PATH_MAX_DIST | 50 | Max A* search distance (nodes) |
| POI_DWELL_MIN/MAX | 300-1800s | Time spent at each POI (5-30 min) |
| TRAVEL_CHANCE_PER_TICK | 1/1000 | ~0.1% per tick to travel |
| PK_CHANCE_PER_TICK | 1/400 | ~0.25% per tick to PK real player |
| PK_BOT_CHANCE_PER_TICK | 1/4000 | ~0.025% per tick to PK another bot |
| COMBAT_LEASH_DIST | 15 | Max chase distance before giving up |
| FLEE_DISTANCE | 15 | Tiles to run when fleeing |
| ATTACK_COOLDOWN | 2s | Between attacks |
| HEAL_THRESHOLD_PERCENT | 60 | HP% below which bot heals |
| MANAGER_INTERVAL | 10000ms | Population manager tick (activations/deactivations) |
| SKULL_CAP_PERCENT | 30% | Max % of active bots with red/black skulls |
| STALEMATE_CHECKS | 5 | 60s checks before HP stalemate exits combat (5 min) |
| BOAT_NPC_DELAY | 3-10s | Random delay at boat NPC before teleport |

---


</details>

<details>
<summary><strong>CPU Performance Analysis</strong></summary>


### Known issue — shutdown crash "corrupted double-linked list"

Reproduced twice on 2026-05-07. Crash signature in journalctl:
```
[info] Done!
[warning] [BotEngine] N bots auto-deactivated this tick
[warning] [BotEngine] M bots auto-deactivated this tick
corrupted double-linked list
canary.service: Main process exited, code=dumped, status=6/ABRT
```

**Runtime is not affected** — only the shutdown path crashes. systemd's `Restart=always` respawns canary cleanly. Player state is saved BEFORE the crash (`Server saved in 1.11 seconds` is logged earlier in shutdown), so no data loss.

**Root cause analysis (no coredumps available, code review only):**

- `Game::shutdown()` ([src/game/game.cpp:8479](src/game/game.cpp#L8479)) does `clear()`s and `serviceManager->stop()` then logs `Done!`.
- The `BotEngine::tick()` cycleEvent ([game_functions.cpp:1079](src/lua/functions/core/game/game_functions.cpp#L1079)) keeps firing on the dispatcher thread — note the auto-deactivation warnings are logged AFTER `Done!`, indicating the dispatcher hasn't fully drained.
- During those ticks, `processBot()` finds `bot.getPlayer()` returning nullptr (Players are being torn down on main thread) and sets `bot.active = false`. That part is safe.
- The crash is glibc's heap-corruption detection in a doubly-linked list (malloc bin metadata). It indicates either a double-free, a use-after-free, or a buffer overflow earlier that corrupted heap metadata.
- Most likely culprit: race between dispatcher thread (mid-tick) and main thread (destroying Player objects), OR `flushNavEvents()` calling `g_databaseTasks().execute()` while the database task system is being torn down.

**Why the obvious fix doesn't work:** calling `BotEngineLoader::getInstance().unload()` at the top of `Game::shutdown()` introduces a worse race — the dispatcher thread could be mid-call to `g_botEngine().tick()` when main thread `dlclose`s the .so, immediately crashing on virtual-call dispatch into unmapped memory. A correct fix needs either (a) atomic engine pointer + busy-wait drain in unload, or (b) explicit dispatcher-stop-and-drain BEFORE unload.

**Workarounds in place:** none. Issue is non-blocking; documented for future investigation. To debug properly: enable coredumps (`ulimit -c unlimited`), reproduce, run gdb with `bt full` on the core.

### Failed CPU optimization experiments (do NOT re-attempt without new data)

Three optimization attempts were investigated and rejected. Documented here so future work doesn't repeat them.

**1. JPS pathfinding (smoke-tested 2026-05-07, REVERTED 2026-05-08)** — Implemented `JPSPathfinder` as a drop-in for `Map::getPathMatching`/`getPathMatchingCond`, gated on a `pathfindingUseJps` config flag. Live test with 200 active bots: **JPS was ~2× slower** (0.74 cores ON vs 0.31 cores OFF). Root cause: forced-neighbor identification adds 4 extra `Map::canWalkTo` probes per cardinal jump step; diagonal jumps recursively probe both cardinals. Tibia's short paths + dense obstacles + tile-grid don't trigger JPS's open-list savings the literature claims (which assume large open continuous-coordinate worlds). All JPS code (jps.cpp/hpp, dispatch wrappers, config flag, CMakeLists entry) was removed to keep the codebase clean.

**2. Hunt-script-id linear-scan replacement (investigated 2026-05-08, NOT IMPLEMENTED)** — `bot_engine.cpp` has ~55 sites where `for (auto& s : huntScripts_) { if (s.id == bot.huntScriptId) ... }` runs. The vector size is N=203. Initial estimate suggested a hashmap conversion would be a significant CPU win. Sonnet+Opus cross-review concluded otherwise: at N=203 with `id` at struct offset 0, scanning hits 16 ids per cache line (~13 cache fetches total). Total cost of all per-tick lookups is ~2 µs/tick = ~20 µs/sec across the whole server — below measurement noise at the 0.35-core baseline. A hashmap would likely be SLOWER for this N due to cache effects (bucket array + linked list dereferences vs. contiguous vector). The actual hot paths are `Map::getPathMatching` (A* per monster per spectator scan) and `Spectators().find<Monster>()`, not integer comparisons.

**3. Hunt-waypoint compression (investigated 2026-05-07, NO-GO)** — Hypothesis: collapsing runs of consecutive `node`/`stand` waypoints on the same z would reduce A* calls. Investigation revealed:
- 95.1% of waypoints are `node` (72.3%) + `stand` (22.8%); 4.9% are anchors (ladder/rope/use_with/etc).
- The C++ engine **already has runtime waypoint compression** via `PATROL_LOOKAHEAD` (3-tile, scan 8 ahead) at [bot_engine.cpp:688-689,9812-9854](src/creatures/players/bot/bot_engine.cpp#L688) — when a bot is within 3 tiles of any of the next 8 waypoints, it skips ahead. So removing dense waypoints provides ~0% CPU savings.
- NODE waypoints sample a random 3×3 tile (intentional wandering pattern). Compression would change hunt behavior.
- Tight node clusters often interleave with `use_with`/`npc_interact` (quest puzzle positioning). Compression breaks quests.
- Hazard avoidance (skirting fields/magic walls) may be encoded by tight node runs that A* can't reproduce.

**Low-risk follow-up** (allowed and intended): dedup of strictly-consecutive waypoints with identical (`waypoint_type` IN ('node','stand'), `pos_x`, `pos_y`, `pos_z`). ~800 rows, zero behavioral change. Aggressive compression NOT permitted.

### Consolidated Summary — 2026-05-06 Three-Test Suite (200 active bots, 2-core LXC)

| Test | Setup | Cores | What it isolates |
|---|---|---:|---|
| 1-C (baseline) | active normal (group=1, full AI, full heal) | **0.375** | total active-state cost |
| 1-B | `aiPaused`, heal kept active, world reacting | 0.143 | heal + world reaction (no AI) |
| 1-B' (invalid) | `aiPaused`, heal also gated, bots dying | 0.037 | underestimate — bots died |
| 2-pre | group=1 normal | 0.404 | new C baseline (slightly higher state-mix) |
| **2-post** | **group=7 admin (`IgnoredByMonsters`/`IgnoredByNpcs`/`CannotBeAttacked`), AI on** | **0.244** | **bot AI without world reaction** |
| **3-A** | group=1, 1B HP buffer, AI on | **0.387** | active baseline with heal-noise removed |
| **3-B** | group=1, 1B HP buffer, AI+heal both paused | **0.199** | world reacting to inert attackable bots |

**Cross-test deductions (one row per finding):**

| Finding | Cores | Source delta | Implication |
|---|---:|---|---|
| Bot engine cost (AI + heal-block iter) under realistic load | **~0.19** | 3-A − 3-B | Cleanest engine-cost number we have. Optimizing AI tick logic, pathfinding, monster scans targets this slice. |
| World reaction to attackable bots in spawns (monster aggro/path/attack toward bots, NPC spectator scans) | **~0.16–0.20** | 2-pre − 2-post = 0.16; or 3-B = 0.20 | Mostly outside our control unless we redesign spawns. Disabling reactions via group flags recovers all of it. |
| Pure world sim (200 in-world Players, no AI, no aggro toward them) | **~0.04** | 1-B' lower-bound | Adding more in-world Players scales linearly here — cheap. |
| Heal *cast* cost when bots actually drop below 75% HP | **~0.10** | 1-B − 1-B' | Only pays when bots take damage. Negligible if HP never drops (Tests 2 and 3-A both show 0). |
| Heal *block iteration* cost (HP%/paralysis/mana check, no cast) | **<~0.01** | derived | Too small to optimize standalone. |
| Combat-triggered AI work (entering COMBAT, flee, retaliation) | **~0.03** | 1-AI(0.23) + 1-WS(0.04) − 2-post(0.244) | Subset of "AI" that fires only when world attacks bot. Removed in admin mode. |

**Key lessons learned from the test campaign:**

1. **`Restart=always` autorestart + bot save-on-shutdown can silently overwrite SQL UPDATEs.** The first Test 2 attempt failed because `systemctl restart canary` triggered `BotManager` graceful shutdown, which ran `IOLoginData::savePlayerGuard` per bot, writing in-memory `group_id=1` back to the DB and reverting our UPDATE. Fix: `systemctl stop canary` → confirm offline → `UPDATE` while no Player objects exist → `systemctl start canary` (within 20s `RestartSec` window). Verify after restart with `SELECT group_id, COUNT(*) FROM players ... GROUP BY group_id` — partial reverts (some 1s, some 7s) are the giveaway.
2. **Group `id=7 'bot'`** (existing in [data/XML/groups.xml](data/XML/groups.xml)) was *almost* right out of the box for benchmarking — only `cannotattackmonster` had to flip from 1 to 0 so bots can still hunt while monsters/NPCs ignore them. No new group needed.
3. **HP buffer trick** (`UPDATE healthmax/health=1000000000` with backup table) is a reusable pattern for any test where bots must survive without heal — at 200 bots × 100-200 dmg/sec, 1B HP buys ~30 hours of continuous damage. Restore via `UPDATE players p JOIN backup b ON p.id=b.id SET p.health=b.health, ...` then `DROP TABLE backup`.
4. **Heal cost was overstated by Test 1 alone.** Test 1 attributed 0.10 cores to heal, but Test 3-A (group=1, 1B HP buffer, heal block iterates but never fires casts) ran at 0.387 cores — almost identical to Test 1 C (0.375). So the 0.10 cores was almost entirely cast cost when bots were actually damaged. Lever for optimization: prevent damage in the first place, not throttle the heal block.

   **Confirmed via direct instrumentation 2026-05-07**: atomic ns counters around `doHealing()` and the mana-restore block, 8-min steady-state run with 200 bots in hunt mix. Per-call cost: heal ≈ **3500–5000 ns**, mana ≈ **70 ns**. Total: 400 calls/sec × ~4070 ns = **~0.0016 cores** at the call site. The 0.10-core delta from Test 1 is *not* in the heal block itself — it's downstream work (network packets to spectators when health changes, or measurement noise). Even removing the entire heal block saves only ~0.0016 cores. **Heal-block optimization is not a viable lever.** Real CPU hotspots remain: `Map::getPathMatching` (A* per monster scan) and `Spectators().find<Monster>()` (per-tick spatial query).
5. **`HEAL:` log lines are gated by `bot.verboseLog`** ([bot_engine.cpp:10888-10895](src/creatures/players/bot/bot_engine.cpp#L10888)) which is only true when someone is watching a bot via cast viewer. So `journalctl | grep -c 'HEAL:'` is *not* a reliable "is heal running" signal in steady state — only "is anyone watching a bot that healed". Use kill-attribution / bot HP queries for damage verification instead.
6. **`top` per-process %CPU is in Irix mode** (100% per core). On the 2-core LXC, the steady-state 0.38 cores = 19% of total capacity — would be 9.5% on a 4-core LXC. **Always report cores (absolute), never %CPU**, since % depends on core count.
7. **State mix matters for fair comparison.** Re-runs with significantly different idle/dwell/hunt/travel ratios can shift total cost by 0.05+ cores. Capture the state-mix line from `journalctl ... grep 'States: idle'` alongside every CPU sample so future you can validate the comparison.

Full per-test details and reproduction in the internal design notes

---

### May 2026 Re-measurement (with `aiPaused` flag, healing-active fix)

After implementing the `aiPaused` flag (BotState gating in `BotEngine::tick()`, exposed via `Game.botSetAIPaused`/`Game.botSetAllAIPaused` and the `bot_test_commands` MySQL queue), we ran multiple A/B/C benchmarks on **2026-05-06**.

Hardware: **2-core Intel i5-6260U @ 1.80GHz, 8 GB RAM, 1 GB swap**. (Note: prior March-2026 measurements below used "8 vCPU" — LXC has been resized at some point.)

**Critical fix during testing**: the original `aiPaused` early-continue gated heal too, which killed bots in spawns. Heal block was moved above the gate so heal still runs while AI is paused. Validated: 52 bot deaths in 2 min on broken version vs only 2 deaths in 6 min on corrected version (matches normal background death rate).

Methodology:
- `aiPaused=true` mass-applied via `INSERT INTO bot_test_commands (command) VALUES ('pause_all')`. Bots stay in-world (no teleport, no inactive marker, no hunt-reservation churn). With the heal-active fix, bots also survive monster damage indefinitely.
- 60s top samples + `/proc/<pid>/stat` utime+stime deltas at start/end of each window for absolute CPU-seconds.

| Phase | Cores | top %CPU | top %MEM | RES MB | State mix |
|---|---|---|---|---|---|
| C corrected — steady-state hunt-heavy | **0.375** | 37.4% | 19.8% | ~1660 | idle=37 dwell=16 hunt=100 travel=24 party=21 |
| B corrected — paused (heal still runs) | **0.143** | 14.3% | 19.2% | ~1610 | (frozen, heal active, monsters still hit them) |
| B' diagnostic — paused (heal also gated, broken pause) | 0.037 | 3.7% | 17.8% | ~1495 | (frozen, but bots died — invalid for production) |

**Final cost breakdown:**

| Component | Cores | % of total | Source |
|---|---|---|---|
| Bot AI state machine (hunt phases, combat targeting, pathfinding, dwell logic, etc.) | **~0.23** | **~62%** | C − B |
| Healing across 200 bots @ 500ms cadence | **~0.10** | **~28%** | B − B' |
| World sim baseline (200 in-world Player objects, monsters/spawns/light, no AI, no heal) | **~0.04** | **~10%** | B' |

**Operational implications:**
- **Heal cost is a non-trivial slice** (~0.1 cores for 200 bots running heal-check at 500ms). Optimization opportunity: skip heal-check when HP > 95%, or stale-check at >1s intervals when bot is in PZ.
- **Bot AI dominates** but is concentrated in HUNTING bots (state machine + monster scans). Idle/dwelling/traveling bots are cheap.
- **RAM ~1.6 GB resident** at 200 bots, stable. Pause did not reduce RAM (Player objects remain in memory).
- On the 2-core LXC, C = 18.7% of total 2-core capacity. Same workload would be 9.4% of a 4-core LXC. Always report cores, not %.

**Reconciliation with March 2026 figures below:** the prior "8 vCPU" measurement reported 12-15% steady-state with 200 bots ≈ 1.0 cores absolute. The new 2-vCPU measurement shows ~0.38 cores absolute. The ~3× difference is roughly explained by (a) different state-mix profile (March measurement had ~100 hunting; current ~100), (b) the LXC may have actually been 8 vCPU with the per-process %CPU display in non-Irix mode at the time, or (c) AI improvements/regressions between periods. **Use the new C = 0.38 cores as the operating baseline going forward.**

**Caveat on first capture (A)**: 3 minutes of stabilization wasn't enough — A still had 136 bots in TRAVEL because hunt rolls hadn't yet materialized into HUNT-state transitions. A measured 0.136 cores. Future re-runs should wait ≥10 min for hunt-heavy mix to develop before sampling.

### May 2026 Scaling-by-Load Benchmark (feat/debug-mode @ `875ced8e8`, 2026-05-13)

After investigating a kite/A* regression on `feat/cavebot`, we reverted to `feat/debug-mode` and measured CPU at every load point so future changes have a clean reference table. Same 2-core LXC hardware. Bot count controlled via `BOT_CONFIG.TARGET_ONLINE` in [data/scripts/lib/bot_system.lua](data/scripts/lib/bot_system.lua) (no bots load on boot when `TARGET_ONLINE=0`).

| Scenario | Avg %CPU | Max %CPU | Notes |
|---|---:|---:|---|
| Pure server, 0 bots loaded | **0.1%** | 1.0% | `TARGET_ONLINE=0`, no bot Player objects in world |
| 1 real player online, 0 bots | **1.3%** | 3.0% | Legolas alone, no bot subsystem cost |
| 200 bots in memory, all INACTIVE | 13.7% | 20% | Bots stored but `BotEngine::tick()` early-exits inactive |
| 200 bots in world, AI paused (`aiPaused=true`) | 30.2% | 40% | Heal still runs; bots stay in spawns surviving damage |
| 1 bot debug-mode (heartbeat + ASCII grid) | 3.2% | 6.7% | Cost of grid render + mob list per heartbeat |
| 100 bots active (normal) | 20.4% | 24% | Roughly linear with bot count |
| 200 bots active (feat/debug-mode baseline) | **45%** | 77% | **The clean operating baseline** |
| 200 bots active (feat/cavebot — REVERTED) | 56.9% | 82% | +12pp regression on kite/A* — reverted |

**Per-bot incremental cost (derived from this table):**
- Per **active** bot ≈ **0.15-0.2% CPU avg** on this hardware (linear: 0→100→200 active = 1.3% → 20.4% → 45%)
- Per **in-memory inactive** bot ≈ **0.06% CPU avg** (200 inactive cost 13.7% over the 1.3% solo-player baseline)
- The dominant cost runs **on the main dispatcher thread** — bot pathfinding/scan spikes therefore stall the same thread that processes real-player commands.

**Operational baseline going forward**: feat/debug-mode `875ced8e8` at 200 active = 45% avg / 77% max. Any future PR must measure against this and stay within ±15%. The 4-row scaling profile (0 / 1-player / 100 / 200) is the regression set — capture all four when changing anything in `bot_engine.cpp` or the bot Lua layer.

**Why this matters for threading work**: at 200 bots, ~30-45pp of the dispatcher's 100%-per-core budget is bot work. Even short pathfinding spikes inside the bot path block real-player input on that same thread. Isolating bot CPU to a worker thread is the next big lever — see the internal design notes

### March 2026 Analysis (historical, on 8 vCPU LXC)

Comprehensive investigation of server CPU usage with 200 bot players (March 2026).
Target CPU range: **2.0-4.0%** with 100+ active bots.

### Experiment Results

All measurements on the LXC server (8 vCPU, 8GB RAM) using `top -p $(pgrep canary)`.

| Scenario | Bots Active | Bots Hunting | CPU (avg) |
|----------|:-----------:|:------------:|:---------:|
| All inactive (staging area) | 0 | 0 | **3.0-3.5%** |
| 50 active (IDLE at temples) | 50 | 0 | **3.0-4.0%** |
| 200 active (IDLE at temples) | 200 | 0-2 | **3.5-4.0%** |
| 200 active, ~7 hunting | 200 | 7 | **5.0-6.5%** |
| 200 active, ~10 hunting | 200 | 10 | **5.0-8.0%** |
| 200 stopped at spawn positions | 200 | 0 (stopped) | **11-13%** |
| 200 active, ~100+ hunting | 200 | ~100 | **12-15%** |

### Root Cause Breakdown

The CPU is **NOT** consumed by bot AI code. The dominant cost is the Canary game engine
running monster AI when bots are positioned near monster spawns.

| Source | CPU Cost | Explanation |
|--------|:--------:|-------------|
| **Server baseline** (game engine, dispatcher, map) | ~3% | Fixed cost regardless of bots |
| **Monster AI activation** | ~8-10% | **PRIMARY DRIVER** — monsters near bots transition from IDLE (0 cost) to ACTIVE (0.1-1ms/s per monster). Each hunting bot activates 5-30 monsters in 11x11 viewport |
| **Bot AI processing** (tick loop, FC scans, pathfinding) | ~1-2% | Cheap — `isTickDue` gates most processing to 1-2s intervals |
| **FC scan loops** (stuck bots) | ~1% | `findZTransitions` scans 25x25 grid; bots in failed FC loops scan every 300-400ms |

### How Monster Idle/Active Works

From `src/creatures/monsters/monster.cpp`:

- **IDLE monsters** (no player in viewport): Removed from `checkCreatureLists[]` via
  `Game::removeCreatureCheck()`. `onThink()` is never called. **Cost: 0 CPU**.
- **ACTIVE monsters** (player within 11x11 tiles): Added to `checkCreatureLists[]`.
  `onThink()` called every 1s: runs `updateTargetList()` (spectator scan), `onThink_async()`
  (target selection, pathfinding, combat spells, defense), `executeConditions()`.
  **Cost: 0.1-1ms per monster per second.**

When a bot walks into a monster spawn, all monsters within the 11x11 tile viewport activate.
With 100 bots across ~50 spawns activating 5-30 monsters each, that's hundreds to thousands
of monster think cycles per second.

### Key Insight

"Stopped" bots at spawn locations (11-13% CPU) cost almost the same as actively hunting bots
(12-15%) because **the dominant cost is monster AI activation from bot proximity, not bot AI
computation**. Bots at temples/staging (3-4% CPU) trigger zero monster AI because temples are
in protection zones with no nearby monsters.

### Improvement Strategies (sorted by impact)

1. **Cap simultaneous hunting bots** (~3-5% savings)
   - Currently all 200 can hunt simultaneously
   - Cap at 50-80 hunting bots max; the rest dwell/idle (0 CPU cost)
   - Implement: counter in `tryStartHunt()`, reject if `countHuntingBots() >= MAX_HUNTING`

2. **Increase hunt cooldowns** (~2-3% savings)
   - Current: `HUNT_COOLDOWN_MIN=600, MAX=1800` (10-30 min)
   - Increase to 900-2700s (15-45 min) — longer dwell time = fewer bots hunting concurrently

3. **Fix FC scan loops** (~1% savings)
   - Add exponential backoff: 1s -> 2s -> 4s -> 8s -> 16s between FC retries
   - Current: retries every 300-400ms indefinitely until 5 failures
   - After 5 failures, don't erase the failure counter when skipping waypoints

4. **Reduce IDLE tick frequency** (~0.5% savings)
   - `TICK_FREQ_IDLE` from 20 (2s) to 50 (5s)
   - `TICK_FREQ_HEAL` from 5 (500ms) to 10 (1s) for non-combat bots

5. **Skip `doSelfDefense()` for bots in PZ** (~0.3% savings)
   - Currently scans spectators (15x11 area) every tick for every active bot
   - Skip if bot is in protection zone (temples)

---

### Stealth Mode POC Results (2026-03-07)

POC: All 200 bots set to "bot" group id=7 (IgnoredByMonsters, CannotBeAttacked, CannotAttackMonster,
IgnoredByNpcs, isAccessPlayer) + stealthMode=true in C++ (skips scanAndAttackMonster,
tryAttackBlockingMonster, doSelfDefense entirely). Bots still walk, navigate, patrol, dwell — just
invisible to monster/NPC AI. 0 kills recorded (stealth correctly prevents all combat).

**Server**: LXC container, PID 244247, 200 active bots (~33 hunting, ~34 traveling, ~37 dwelling, ~97 idle)

| Sample | Time    | CPU% (avg of 2nd+3rd reading) | Notes |
|:------:|---------|:-----------------------------:|-------|
| 1      | 16:17   | 6.3%   | Ramping up (bots activating) |
| 2      | 16:19   | 8.0%   | Ramping up |
| 3      | 16:21   | 13.5%  | Ramping up |
| 4      | 16:23   | 18.5%  | |
| 5      | 16:25   | 15.5%  | |
| 6      | 16:27   | 16.3%  | |
| 7      | 16:29   | 16.3%  | |
| 8      | 16:31   | 15.5%  | |
| 9      | 16:33   | 19.7%  | Periodic spike |
| 10     | 16:35   | 15.5%  | |
| 11     | 16:37   | 16.7%  | |
| 12     | 16:39   | 17.5%  | |
| 13     | 16:41   | 17.0%  | |
| 14     | 16:43   | 18.3%  | |
| 15     | 16:46   | 16.8%  | |

**Stabilized average (samples 5-15)**: 16.8%
**CPU-time based avg (sample 5→15)**: 17.0% (211s CPU / 1241s wall)
**Expected**: 3.5-5.0% — **Hypothesis REJECTED**: Monster AI activation was NOT the dominant CPU cost.

Additional measurement with group_id=1 + stealthMode=true (PID 243893, same code, different group):
stabilized at ~16.5% — nearly identical, confirming group flags (IgnoredByMonsters etc.) made
no measurable difference.

**Key finding**: The dominant CPU cost is bot navigation processing (A* pathfinding, waypoint
following, floor-change scanning, creature movement events), NOT monster AI activation.

---

### Baseline Re-Measurement (2026-03-07)

After reverting stealth mode (stealthMode removed, group_id=1), same measurement protocol.

**Server**: LXC container, PID 245535, 200 active bots (~39 hunting, ~32 traveling, ~31 dwelling, ~99 idle)

| Sample | Time    | CPU% (avg of 2nd+3rd reading) | Notes |
|:------:|---------|:-----------------------------:|-------|
| 1      | 17:01   | 5.0%   | Ramping up |
| 2      | 17:03   | 10.0%  | Ramping up |
| 3      | 17:05   | 14.3%  | Ramping up |
| 4      | 17:07   | 12.5%  | |
| 5      | 17:09   | 12.5%  | |
| 6      | 17:11   | 11.5%  | |
| 7      | 17:13   | 10.2%  | |
| 8      | 17:15   | 12.5%  | |
| 9      | 17:17   | 14.8%  | Periodic spike |
| 10     | 17:19   | 12.5%  | |
| 11     | 17:21   | 11.2%  | |
| 12     | 17:23   | 13.5%  | |
| 13     | 17:26   | 12.3%  | |
| 14     | 17:28   | 13.5%  | |
| 15     | 17:30   | 13.3%  | |

**Stabilized average (samples 5-15)**: 12.5%
**CPU-time based avg (sample 5→15)**: 12.9% (160s CPU / 1241s wall)

### POC Conclusion

| Scenario | Stabilized CPU | CPU-time Avg |
|----------|:-----------:|:------------:|
| Stealth mode (IgnoredByMonsters + stealthMode skip) | 16.8% | 17.0% |
| Baseline (normal hunting with combat) | 12.5% | 12.9% |
| **Delta** | **+4.3%** | **+4.1%** |

**Stealth mode was 4% WORSE than baseline.** The hypothesis that monster AI activation was the
dominant CPU cost is **rejected**. Reasons stealth mode was worse:

1. **No combat pauses**: Baseline bots stop walking during monster combat. Stealth bots never
   stop — they cycle through patrol waypoints at full speed, generating more pathfinding calls
   and creature movement events per unit time.
2. **No kill-based linger**: Baseline bots linger after kills. Stealth bots keep moving constantly.
3. **More floor-change scanning**: Faster waypoint cycling = more FC_SCAN operations (592/min
   stealth vs 392/min baseline).

**Actual CPU bottleneck**: Bot navigation processing — A* pathfinding, waypoint following,
floor-change tile scanning (radius-12 search), creature movement events (onCreatureMove
dispatched to all spectators per bot step). These costs scale with number of actively MOVING
bots, not with monster AI.

**Implication for proximity-based activation**: Making bots invisible to monsters provides zero
benefit. Instead, CPU reduction should focus on:
- Reducing unnecessary pathfinding/movement
- Throttling floor-change scanning frequency
- Reducing tick frequency for bots far from real players (graduated tick rate)
- Fully pausing (no movement) bots that are far from real players

---


</details>

<details>
<summary><strong>Hibernation & Wake System</strong></summary>


The hibernation system reclaims CPU + RAM by despawning bots that are far from any real player.
Bots stay in C++ memory as `BotState` only; their `Player` object is destroyed and replaced
either from a per-bot **hibernation pool** (warm cache, zero DB I/O) or freshly materialized
from the database.

### Loop & thresholds

Driven by [bot_hibernation.lua](../../globalevents/bot_hibernation.lua):

| Setting | Value | Purpose |
|---|---|---|
| `DISTANCE_TILES` | 100 | Max Chebyshev distance (any z) from a real player to stay awake |
| `HYSTERESIS_MS` | 30000 | Time with no real player nearby before hibernating |
| `TICK_INTERVAL_MS` | 300 | Proximity loop cadence (3 game ticks) |
| `MAX_HIBERNATES_PER_TICK` | 5 | Cap to prevent dispatcher saturation |
| `MAX_WAKES_PER_TICK` | 5 | Cap for the proximity-poll path (NOT the pre-wake hook) |

### Wake triggers

Two paths can wake a hibernated bot:

1. **Polling (300ms cadence)** — the proximity loop in `bot_hibernation.lua`. Hibernates bots
   with no real player within `DISTANCE_TILES` for `HYSTERESIS_MS`. Wakes bots that come back
   within range. Subject to `MAX_WAKES_PER_TICK` AND the Phase B density cap (see below).
2. **Pre-wake hook (event-driven)** — `Game::internalTeleport` (`src/game/game.cpp`) calls
   `g_botEngine().wakeBotsInRadius(newPos, 100)` synchronously when a real player teleports
   (boat NPC, scroll, ladder/hole, death-to-temple, `/goto`, etc.). Hibernated bots within
   100 tiles of the destination materialize in the SAME dispatcher event as the player's
   arrival packet — destinations look populated immediately, no 300ms trickle-in.
   `wakeBotsInRadius` sorts candidates by `lastWakeAttemptMs` ascending (LRU oldest-first)
   before invoking `wakeBot` per candidate, and each `wakeBot` call goes through the density
   cap.

### Density cap (Phase B, 2026-06-01)

To prevent bot clusters at temple/boat chokepoints and to bound pathfinding pressure during
wake bursts, every `wakeBot` call is gated by `shouldGateWake(guid)` (in `bot_engine.cpp`).
The cap is per-cluster, not global: real players + cast-watched bots within
`botDensityAnchorClusterRadius` (default 50) tiles of each other merge via union-find into a
single "anchor cluster" at the centroid. Per cluster, three concentric ring caps are
enforced:

| Ring | Config keys | Defaults |
|---|---|---|
| inner (screen-visible) | `botDensityCapInnerRadius` / `botDensityCapInnerLimitPct` | 7 tiles / 0.6% |
| mid | `botDensityCapMidRadius` / `botDensityCapMidLimitPct` | 50 tiles / 2.0% |
| outer | `botDensityCapOuterRadius` / `botDensityCapOuterLimitPct` | 100 tiles / 3.0% |

Ring **limits are percentages of `botPlayersOnline`**, truncated to whole bots by
`pctOfBotTotal()` in `bot_engine.cpp` (at 500 bots: 0.6/2.0/3.0% → 3/10/15). Beware the
truncation at small totals — the inner pct floors to 0 below ~167 total bots, which gates
ALL wakes near players. Radii stay absolute tile distances. The related
`botAdvStoneChestDummyCapPct` (default 2.5% → 12 at 500) bounds how many bots may
concurrently hold an Adventurer's Stone reward-chest/training-dummy sub-activity; it is
enforced at the mode roll in `selectAdvStoneSubActivity` (capped trips demote to waypoint
idling), because chest/dummy bots are exempt from the ring caps below ("Fix #12").
Set `botDensityCapEnabled=false` to disable gating (telemetry still fires). Restart required
for all of these keys (load-once; `/reload config` does not re-read them).

**Special case `botDensityCapOuterLimitPct = 0`** (the shipped `config.lua` value as of
2026-06-12): everything beyond `midRadius` of **every raw anchor** (player/cast positions,
not cluster centroids) becomes a hard no-wake zone, logged as `[DENSITY] band_gate` (60s
rate-limit per 50-tile area). Inner/mid caps apply unchanged; the cumulative outer check is
disabled. Explicit wakes — cast-viewer login by bot name, partyhunt support assembly,
`/cavebot <name> wake` — bypass the band via a single-shot `s_forceWakeGuid` exemption
consumed by the next `shouldGateWake` call. This replaced the reverted "crowd-skip" hunt
assignment (bundle 6) as the answer to multi-aggro-center monster-AI load: cap how many
places bots keep monsters active, not which spawns they pick.

**LRU fairness via `BotState::lastWakeAttemptMs`**: `shouldGateWake` updates this timestamp
unconditionally on every wake attempt (grant or cap-skip). Both `wakeBotsInRadius` (C++) and
the Lua proximity loop sort candidates ascending → bots that lost a cap slot rotate to the
back of the queue → no permanent dead zones at high-density chokepoints.

**Party-leader cascade is EXEMPT from the cap**. The file-scope `s_inPartyCascade`
thread_local is set by the cascade loop before recursive `wakeBot` calls;
`shouldGateWake` short-circuits when set. Result: cap shapes WHICH party wakes (via the
leader's gate decision); never WHICH members. A 5-bot party hitting an inner cap of 3 still
wakes together.

**Counter accounting**: when `shouldGateWake` grants a wake, it increments the matching ring
counters in-place before returning, so back-to-back wakes in one dispatcher event see updated
counts (no over-grant within a burst). If a later step in `wakeBot` fails (DB error etc.),
the counter is over by 1 until the next anchor refresh (≤100ms) — accepted slack.

**Phase B.1 fix (2026-06-01)**: `wakeBotsInRadius` force-invalidates the anchor cache at
entry (`anchorsRefreshedAt_ = 0; refreshAnchorsIfStale(0);`). Without this, a player
teleport firing <50 ms after the previous `BotEngine::tick` refresh would re-use the cluster
cached at the PRE-teleport position; all new wake candidates at the destination fall outside
the old cluster's outer ring → `shouldGateWake` walks all clusters with `cheb > outerRadius
→ continue` → no gate fires → entire burst bypasses the cap. Verified in production at
14:45:29 (37 bots woke at Ab'Dendriel with zero `cap_hit` logs). Side effect: a spurious
`[DENSITY] cluster_dissolve` log line per teleport when the player has been moving fast.

**Phase B.1 fix (2026-06-01)**: party-cascade flag (`s_inPartyCascade`) is now wrapped in
an RAII `CascadeGuard` struct. Pre-fix, an exception during the cascade loop (e.g., inside
`materializeCanaryParty` or a recursive `wakeBot`) would leave the flag `true` forever for
the dispatcher thread, silently bypassing the density cap for every subsequent wake until
server restart. RAII ensures the flag is cleared on every scope exit.

**`[DENSITY]` telemetry**:

| Tag | Cadence | What it shows |
|---|---|---|
| `cap_hit` | per-cluster-centroid 5s rate limit | A wake was gated. Logs guid, bot_pos, cluster centroid, gated ring, count vs cap, all-ring counts |
| `cluster_dissolve` | per-event | A previously-active cluster has no surviving anchor in next refresh (cast-watcher disconnect, player logout). Instruments the cast-watcher oscillation risk |
| `periodic` | 60s per cluster | Cluster centroid, anchor count, current ring counts, peak ring counts since last log |

### Wake position safety (`chooseWakePosition`)

When a bot wakes, its `virtualPos` (from the virtual simulator) is validated before placement.
Unsafe positions trigger walk-back through the bot's route chain:

| Rejected by | Why |
|---|---|
| `kUnsafeWakeMask` | wall, blocking item, floor-change tile, native teleport, depot box, magic field |
| `tile->queryAdd(FLAG_PATHFINDING) != NOERROR` | door/lever/use_with-target items blocking entry |
| `g_moveEvents().hasPosition(p)` | position-registered Lua MoveEvent (e.g. dragolisk teleports) |
| `g_moveEvents().hasActionId(item->aid)` on any tile item OR ground | action-id-registered MoveEvent (adv stone forcefield aid:4253, sorcerer guild aid:5555, schrodinger aid:15998, magician quarter aid:7813, citizen svargrond aid:30032, roshamuul carpet aid:4256, etc.) |

When rejected, `walkBack` searches the route chain (HUNTING patrol waypoints, TRAVELING
cityRouteWps, advStone dungeon waypoints, party-leader pos) for the prior safe waypoint.
Final fallback: town temple.

### Post-wake AI burst mitigation

Wake bursts (13+ bots in 2 seconds) used to lag the dispatcher because every freshly-woken
bot ran a full AI tick — `Spectators::find<Monster>` scan, `getPathTo` (A*), spell selection —
on the same window. Three combined mitigations:

1. **`BotState::wakeQuietTicks`** — per-bot counter set to `3 + recentWakeStagger_` on wake.
   While > 0, the bot's AI work is skipped (heal still runs). Decrements each tick.
2. **`BotEngine::recentWakeStagger_`** — engine-wide counter, increments per `wakeBot` call,
   decays 1/tick, capped at +10. Each consecutive wake in a burst gets a progressively larger
   quiet period (3, 4, 5, ..., 13 ticks). Spreads the AI burst across 12 dispatcher windows.
3. **Async cast_broadcasters DB write** — `g_databaseTasks().execute()` instead of
   synchronous `db.executeQuery()`. Wake-time INSERTs no longer block the dispatcher.

### Travel-state wake fixes

The static `s_travelStartTime[guid]` map is process-scoped and survives hibernation.
Without the fix, a bot hibernated > 5 min mid-travel would wake with a pre-expired global
timer and immediately teleport-to-temple via the 5-min travel timeout. Fixed by resetting
`s_travelStartTime[guid]` and erasing `s_routeProgress[guid]` in `wakeBot` when the bot
state is TRAVELING.

`virtualAdvanceTraveling`'s at_boat → teleported transition also mirrors the live at_boat
handler's `townId`/`townName` update — previously this gap caused woken bots to show
"IDLE in <source-city>" status text while physically sitting at the destination boat dropoff.

### HUNT TRAVEL_TO wake-stuck fix (parallel to TRAVELING)

`s_huntTravelStart[guid]` is the HUNTING-state equivalent of `s_travelStartTime` — it
gates the 5-min HUNT_TRAVEL_MAX_MS timeout for the TRAVEL_TO phase. It also survives
hibernation. Empirically reproduced via debug,1: Ophelia Frostborn on `City Walk: Venore →
Thais` (id=1308) hibernated 10 min mid-route, then wake fires the stale timeout
immediately, bot stays motionless at wake position for 28 s before re-hibernation on the
same tile. Same signature as Isolde Stoneguard's earlier 95s-stuck case.

Fix: in `wakeBot`, when `state == HUNTING && huntPhase == TRAVEL_TO`, reset
`s_huntTravelStart[bot.guid] = OTSYS_TIME()`. After-fix verification: same scenario
completed the City Walk in 78 s post-wake (vs 28 s stuck → re-hibernate pre-fix).

Other HUNTING phases (PREPARING / PATROLLING / LEAVING / RESUPPLYING) were empirically
tested with hibernate-then-wake handoffs and **all work correctly** — no parallel resets
applied for those. The LEAVING phase has structurally identical static maps
(`s_leavingPhaseStart`, `s_leavingWpTimer`) that could theoretically exhibit the same bug
with very long hibernation; defer until empirically observed (per the project's
"empirically-confirmed bugs only" rule).

### Universal virtual waypoint walking

Per the internal design notes
The virtual simulator originally walked hunt waypoints (TRAVEL_TO / PATROLLING / LEAVING) at
1 wp / 5s but **snapped instantly** for inter-city travel, IDLE POI walks, depot walks, and
hunt resupply chains. Hibernated bots were therefore invisible during those activities.

Extension (7 phases, all deployed) makes every per-bot path use the same waypoint-walking
primitive (`advanceWaypointIdx` + per-tick snap to `waypoints[idx].pos`):

| Activity | Previously | Now |
|---|---|---|
| HUNTING TRAVEL_TO / PATROLLING / LEAVING | walks waypoints 1/5s | (unchanged) |
| HUNTING PREPARING / RESUPPLYING (depot, shop chain) | timer only — frozen | walks `loadCityRouteCore("", "depot")` then `("depot", "potions")` 1/5s, dwells in-between |
| TRAVELING `walk_to_boat` | snap to boat NPC in 1 tick | walks source city route to boat 1/5s |
| TRAVELING `at_boat` (boat ride) | virtual teleport | (unchanged — boats ARE teleports in game) |
| TRAVELING `walk_from_boat` | snap to destination temple POI in 1 tick | walks destination city route from boat 1/5s |
| IDLE POI walk (`hasWalkTarget`) | snap to POI in 1 tick | walks city route via `bot.pendingNavDest` key 1/5s, snap on completion |
| IDLE depot locker walk (`hasDepotTarget`) | not handled | walks `""→depot` route 1/5s, snap to locker tile on completion |

Shared infrastructure:
- **`loadCityRouteCore(bot, srcPOI, dstPOI)`** — Player-free route lookup + state population
  (`bot.cityRouteWps`/`Idx`/`followingCityRoute`). Live `startCityRoute` is now a thin
  wrapper that adds `castLog`.
- **`virtualTickCityRoute(bot, elapsed_ms)`** returns `VirtualRouteStatus` enum
  (`NotActive` / `StillWalking` / `JustCompleted`). Per-phase handlers branch on it.
- **`chooseWakePosition`** gained an IDLE+route branch (mirrors TRAVELING branch) so IDLE
  bots hibernated mid-route on unsafe tiles wake at the nearest upstream safe waypoint
  instead of the town temple.
- **`walkBack` rewinds `cityRouteIdx` / `huntWaypointIdx` / `advStoneRouteIdx` atomically**
  with the chosen wake position, so live AI's `followWaypoints` 200-tile sanity check
  never trips after wake.

Wake handoff fixes for the new paths:
- TRAVELING walk_from_boat sets `bot.travelDestVerified = true` after route load (otherwise
  live AI sees `distToDestBoat > 10` on wake and resets to walk_to_boat — see
  [bot_engine.cpp:12808-12830](../../../src/creatures/players/bot/bot_engine.cpp#L12808-L12830)).
- HUNTING PREPARING/RESUPPLYING sets `bot.prepareHasTarget = true` after route load (otherwise
  live AI enters POI-fallback re-load branch and overwrites `prepareTarget`).
- Virtual-path `RESUPPLY_TIMEOUT` is doubled to 10 min — virtual walks at 1 wp/5s, much
  slower than real-player movement, so 30-waypoint shop chains exceed the live 5-min cap.

End result: walking through any city now shows bots walking depot→shop→spawn-approach
cycles (Phase G), walking to depot lockers after travel arrival (Phase E), walking from
boat to depot/temple on arrival (Phase B), walking from temple/depot to boat on departure
(Phase C), and walking to POIs on idle reroll (Phase D). Cross-city overland walks are
covered separately by the 20 `script_category='traveling'` City Walk hunt scripts that the
existing hunt-virtual-sim already walked since BOT_HIBERNATION_V2 Phase 6.

Per-bot cost on hibernation pool tick: ~50µs to load a route, ~µs to advance an index per
subsequent tick. Memory: ~1.5KB per loaded `bot.cityRouteWps` × 200 bots = 300KB worst case.

### `endhunt` debug command

Admin/debug shortcut: `INSERT INTO bot_commands (bot_name, command) VALUES ('<Bot>',
'endhunt')` forces `bot.huntEndTime = OTSYS_TIME() - 1`, causing the next `doHuntPatrol`
tick to transition PATROLLING → LEAVING. Used for empirically testing hibernate/wake in
the LEAVING and RESUPPLYING phases without waiting 30-180 min for natural hunt expiration.
Only affects the targeted bot — safe to leave in tree.

### Dispatcher jitter — root causes + fixes

Reference: the internal design notes +
CPP_BOT_ENGINE_PROGRESS.md §54. Empirical 5-run diagnostic identified two recurring stall
causes and shipped fixes. ~66% reduction in user-felt jitter, ~87% smaller max stall.

**Symptom**: with 200 bots online, ~30s-cadence freezes during walking — character movement
stops 100ms-2s, black tiles at screen edges, chat input drops, recovery is "snap". Control
test with `TARGET_ONLINE=0` proved bot work (not LXC/kernel) is the cause.

**Fix #1 — periodic 5-min DB write storm**. All 200 bots register at startup with
`lastVirtualPosSave=0`. After `VIRTUAL_POS_SAVE_THROTTLE_MS=5min`, the next `virtualTick` finds
every bot eligible to save and enqueues ~190 async `UPDATE players` tasks in one tick →
600-900ms dispatcher stall every 5 minutes precisely. **Fix**: `registerBot` now sets
`lastVirtualPosSave = OTSYS_TIME() - uniform_random(0, 300000)` so each bot's first save lands
at a different time within the 5-min window. Subsequent saves stay staggered naturally.
Commit `0aa75a2ad`.

**Fix #2 — hibernate burst on walk-away**. When user walks away from a populated area, 10-20
bots start their `noPlayerSince` hysteresis timer within ms of each other. At the exact 30s
mark they all hit `hibernateBot` in the same proximity-loop tick — even with the existing
`MAX_HIBERNATES_PER_TICK=5` cap, the burst saturates the dispatcher across 4 sequential ticks.
Cumulative DB work (cast_broadcasters DELETE, onRemove auto-save) is the dominant cost. CPU
correlation proved this: during these stalls, dispatcher main thread is IDLE in
`do_epoll_wait` while WORKER threads spike — classic "dispatcher waits on DB worker" pattern.
**Fix**: `bot_hibernation.lua` subtracts `math.random(0, 10000)` from `noPlayerSince[guid]`
when first set. Each bot hibernates 20-30s after eligibility (spread across 10s = ~2/sec)
instead of all at 30s. Commit `7f2935ffd`.

### Jitter diagnostic instrumentation (kept in production)

All threshold-gated — sub-0.05% steady-state CPU overhead, no log spam in normal operation.
Future jitter events get auto-captured for analysis without needing to redeploy.

| Tag | Threshold | What it catches |
|---|---|---|
| `[HB_STALL]` | heartbeat delay > 100ms | Server-side stall detected by 200ms heartbeat |
| `[TICK_SLOW]` | BotEngine::tick body > 150ms | Engine tick itself too slow |
| `[GAP_SLOW]` | tick-to-tick gap > 200ms | Dispatcher busy with non-tick work |
| `[WAKE_SLOW]` / `[WAKE_BURST]` | single wake > 5ms / wakeBotsInRadius total > 20ms | Slow wake or wake-burst from teleport |
| `[HIB_SLOW]` | single hibernate > 5ms | Slow hibernate (DB stall) |
| `[HIB_LOOP_SLOW]` | Lua proximity loop body > 20ms | Hibernation Lua loop slow |
| `[CHECKCREATURES_SLOW]` | per-bucket > 5ms | Game::checkCreatures bucket slow |
| `[PROCBOT_SLOW]` | per-bot processBot > 10ms | Specific bot's C++ AI tick slow |
| `[THINK_SLOW]` | per-bot Lua thinkEvent > 10ms | Specific bot's Lua watchdog slow |
| `[LAGMARK]` | n/a (user-triggered) | User-tagged felt jitter event |

User-facing command: `/lagmark` (god-only talkaction) — spam during felt jitter, echoes
`lagmark @ <timestamp>` back to client when dispatcher resumes processing.

Cross-correlation analyzer: `tools/lagmark-correlate.py` (deployed at
`/usr/local/bin/lagmark-correlate.py`). Pairs engine-log + cpu-log per run, classifies each
lagmark cluster by dominant correlate (big_stall / small_stall / wake_burst / hibernate_burst /
gap_only / uncorrelated), reports CPU spike pattern (dispatcher idle vs worker busy).

CPU+wchan sampler: `/usr/local/bin/canary-cpu-monitor.sh` — 50ms cadence, logs per-thread CPU%
and kernel wait-channel. Run manually during a diagnostic session, output goes to
`/var/log/jitter/cpu-<RUN>.log`.

JitterHeartbeat globalEvent: fires every 200ms, logs `[HB_STALL]` automatically when
actual-vs-expected fire time exceeds 100ms. No setup needed — runs continuously.

### `/cavebot reload` hibernation-aware path

Plain reload would lose all hibernated bots (their `Player` shared_ptrs live in the engine's
`hibernationPool_` which dies with the .so dlclose). The reload sequence:

1. Snapshot **all** bot guids: awake from `BotPlayers` + hibernated names from
   `Game.getBotHibernationStates()` **BEFORE** force-deactivate / dlclose.
2. Force-deactivate awake bots in old engine; preserves cast streams.
3. `Game.botReload()` → dlclose + dlopen + `loadHuntData`.
4. `Game.botStartTickLoop()` — restart the 100ms cycleEvent.
5. For each snapshotted guid NOT already in `g_game().getPlayers()`: call
   `Game.loadBotPlayer(name)` (same path startup uses) — re-materialize from DB.
6. `Game.botReregisterAll()` — registers all 200 in one pass with fresh `BotState`.
7. Optional: `Game.botRecoverOrphanForReload(guid, p)` for any bot in `BotPlayers` not yet
   active (defensive fallback; normally 0 entries after step 5).
8. Re-activate the originally-awake bots so their cast viewers don't drop.

End result: all 200 bots survive `/cavebot reload`. They re-hibernate naturally over 30s
hysteresis once they're far from any real player.

### Hunt assignment logging

`BotEngine::logHuntAssign(bot, scriptId)` emits a `[HuntAssign]` journal line whenever a bot
reserves a new hunt script. Called at 6 of the 8 `bot.huntScriptId = X` assignment sites in
`bot_engine.cpp` (skipped at party-state restore and DB rehydration — neither is a "new"
reservation). Format:
```
[HuntAssign] <bot> (lv<N> voc<V>) -> '<script>' (id=<N> town=<T>) spawn=(x,y,z)
```
Spawn coords = first patrol waypoint; useful for `/goto x,y,z` to teleport-watch the hunt.

Live dashboard:
```
journalctl -u canary -f | grep HuntAssign
```

---


</details>

<details>
<summary><strong>Cast Viewer System</strong></summary>


Two independent login paths can reach a bot as a cast viewer; both end in
`ProtocolGame::castViewerLogin` (`src/server/network/protocol/protocolgame.cpp:735`)
which is where the wake-on-click logic lives.

### Path A — Binary login server (port 7171, native Tibia / OTC without httpLogin)

`protocollogin.cpp::getCharacterList` lines 34-102 handles `@cast` / `@livestream`. The
character list is built from `IBotEngine::getActiveBotNames()` — returns every bot with
`bot.active && !name.empty()` (stable across hibernate/wake — `bot.active` is set at
`activateBot` and only cleared at `deactivateBot`/`unregisterBot`) — plus real-player
broadcasters. Toggle: `static constexpr bool kShowBotsInCastList = true` at line 58.

**Protocol limit — 255 names (Path A only)**: the Tibia char-list packet uses a
`uint8_t` count ([protocollogin.cpp:86](../../src/server/network/protocol/protocollogin.cpp#L86)),
so when `botPlayersOnline > 255` the @cast dropdown silently truncates to the first 255
sorted names. Cast-watching individual bots beyond #255 still works (e.g. via direct
character-name login), only the dropdown is capped. Lifting this would require a custom
client-side protocol extension. **Path B (httpLogin/PHP, below) is NOT affected** — it
sends the full list as JSON.

Look for `[Cast] Character list: N broadcasting players found` in journalctl.

### Path B — MyAAC httpLogin (PHP, port 80, OTClient `httpLogin = true`)

OTClient with `Servers_init["http://.../login.php"].httpLogin = true` POSTs to
`/var/www/html/login.php`. The `@cast` block at lines 101-170 builds the character list
from a Laravel/Eloquent query. **As of the 2026-05-23 fix (commit 5ca6f775a)**, the query
joins against `bot_active_players` instead of filtering by `account_id`:

```php
$broadcasters = Player::where(function ($q) {
    $q->whereIn("id", function ($sub) {
        $sub->select("player_id")->from("cast_broadcasters");
    })->orWhereIn("id", function ($sub) {
        $sub->select("player_id")->from("bot_active_players");
    });
})->notDeleted()->...->get();
```

**Why the bot_active_players table?** When the bot pool grew from 200 → 997 in MySQL
(only 200 loaded into the engine at any time via `botPlayersOnline`), the prior
`account_id=65000` filter started returning all 997 — the 797 unloaded characters
appeared as ghost entries; clicking one returned "This player is not broadcasting".

The new source-of-truth table is maintained synchronously by the C++ BotEngine — see
[migration 60.lua](../../data-otservbr-global/migrations/60.lua) for the contract and
[bot_engine.cpp:2303,2342](../../src/creatures/players/bot/bot_engine.cpp#L2303) for the
write sites. The table:

- is populated within ~1ms of `BotEngine::registerBot` via `g_databaseTasks().execute()`
  (async pattern matching the rest of `bot_engine.cpp`'s DB writes)
- holds rows for bots that are loaded into the engine, awake **or** hibernated — so
  wake-on-click still works for hibernated entries
- is independent of `botPlayersShowAsOnline` (that flag governs the website's online
  player count, not the cast list)
- is independent of the 10-minute `updatePlayersOnline` cycle
- is `TRUNCATE`d at server boot by `server_initialization.lua` for crash recovery
- FK CASCADEs on `players.id` so a deleted bot character self-cleans

Prior history of this section: 2026-05-19 added the `orWhere("account_id", 65000)`
clause to surface hibernated bots (commit 982857599);
2026-05-23 superseded that with the `bot_active_players` join.

The PHP file lives outside the canary build (it's part of the MyAAC installation at
`/var/www/html/`), but a git-tracked mirror is committed to `deployment/web/login.php`
and synced bidirectionally per `deployment/README.md`. Always diff repo vs LXC before
and after any edit, and verify MD5 after pushing.

### Wake-on-click flow

When a viewer selects a character, the OTClient connects to game port 7172 with
`@cast` as account + chosen character name. `ProtocolGame::onRecvFirstMessage` line 1009
dispatches to `castViewerLogin(characterName)` which:

1. `g_game().getPlayerByName(characterName)` — returns the live `Player` if awake.
2. If null, call `g_botEngine().getHibernatedBotGuidByName(characterName)`. Non-zero guid
   means it's a hibernated bot.
3. `g_botEngine().wakeBot(hibernatedGuid)` — synchronous wake. Pool-hit (~1-5 ms) restores
   the same Player from the hibernation pool; pool-miss falls back to DB load (~5-20 ms).
   On success, `setCastBroadcasting(true)` is re-set and the `cast_broadcasters` row is
   re-INSERTed.
4. Re-query `getPlayerByName` → now returns the woken Player.
5. If still null AND wake was attempted, disconnect with `Failed to wake bot for streaming.
   Please try again in a moment.` (logged at `warn` level with the bot name). Otherwise
   the original `This player is not broadcasting.` message.
6. Attach viewer via `addCastViewer`, send `sendCastViewerInit`, open Cast Chat.

### Hibernation while watched

`bot_engine.cpp::hibernateBot` lines 2374-2386 guard:

```cpp
if (player && player->getCastViewerCount() > 0) {
    // ratelimited info log (1 line per bot per 60s)
    return false;
}
```

Bots with one or more viewers never hibernate. After the last viewer disconnects (via
`ProtocolGame::release` lines 510-525 → `removeCastViewer`), `getCastViewerCount()` drops
to 0; if no real player is within `DISTANCE_TILES` for `HYSTERESIS_MS` (30 s), the bot
hibernates as normal. The bot's `cast_broadcasters` row is then DELETEd — but it stays
visible in the @cast list under Path B because the PHP query also matches
`account_id = BOT_ACCOUNT_ID`.

### Lua watchdog safety (Phase 5c)

`bot_system.lua:2803-2811` only clears `setCastBroadcasting(false)` when
`state == INACTIVE`. During the transient ~0-300 ms window after C++ `wakeBot` but
before the Lua proximity loop syncs `BotActive[guid] = true`, the watchdog would
otherwise clear broadcasting and drop the just-attached viewer. After wake,
`bot.state == IDLE`, so the guard skips the clear.

### Verification queries

```sql
-- Currently-streaming (in-memory broadcasting + DB row present)
SELECT player_id, player_name FROM cast_broadcasters;

-- What the @cast list shows after the Path B fix (should match `account_id` row count
-- for the bot account, plus any real-player broadcasters)
SELECT id, name FROM players
WHERE (id IN (SELECT player_id FROM cast_broadcasters) OR account_id = 65000)
  AND deletion = 0
ORDER BY name;
```

---


</details>

<details>
<summary><strong>Website Player Count / Online List</strong></summary>


Bot hibernation removes the `Player` object from `g_game().getPlayers()` (via
`hibernateBot` → `g_game().removePlayer(player)` at
[bot_engine.cpp:2318](../../../src/creatures/players/bot/bot_engine.cpp#L2318)).
Without compensation, every "who is online" surface in the MyAAC website undercounts —
hibernated bots vanish even though they're conceptually still in the world.

The `botPlayersShowAsOnline` config flag (default `true`) inflates all three displays
to include active hibernated bots. Set it `false` to revert to pre-hibernation behavior
(real players + awake bots only).

### Config flag

```lua
-- config.lua / config.lua.dist
botPlayersShowAsOnline = true
```

Wired through `BOT_PLAYERS_SHOW_AS_ONLINE` in
[src/config/config_enums.hpp](../../../src/config/config_enums.hpp) and loaded by
`loadBoolConfig(..., "botPlayersShowAsOnline", true)` in
[src/config/configmanager.cpp](../../../src/config/configmanager.cpp). Lives in the
reloadable section.

### Three pipelines, all flag-aware

| Pipeline | Updated by | Read by | Cadence |
|---|---|---|---|
| `players_online` DB table | `Game::updatePlayersOnline` ([game.cpp:11295](../../../src/game/game.cpp#L11295)) | MyAAC `/online` page, `login.php` "is player online" | every 10 min (`UPDATE_PLAYERS_ONLINE_DB`) |
| TCP status protocol — binary 0x20 | `ProtocolStatus::sendInfo` ([protocolstatus.cpp:189](../../../src/server/network/protocol/protocolstatus.cpp#L189)) | MyAAC `OTS_ServerInfo::info()`, third-party status pollers | on probe (cached ~6s by MyAAC) |
| TCP status protocol — XML | `ProtocolStatus::sendStatusString` ([protocolstatus.cpp:109](../../../src/server/network/protocol/protocolstatus.cpp#L109)) | MyAAC `OTS_ServerInfo::status()` → page header sidebar widget (every page on the site) | on probe (cached ~6s by MyAAC) |

All three use the same effective formula when `botPlayersShowAsOnline = true`:
```
count = g_game().getPlayersOnline()                  // real players + awake bots
      + g_botEngine().getHibernatedBotCount()         // active && hibernated
```

The DB pipeline builds the actual ID list via
`g_botEngine().getActiveBotGuids()` (which returns awake + hibernated together);
`INSERT IGNORE` dedupes against the awake bots that are also in `getPlayers()`.

### New `IBotEngine` API methods (ABI surface)

| Method | Returns | Used by |
|---|---|---|
| `getActiveBotGuids()` | `vector<uint32_t>` of every `bot.active && !name.empty()` (awake + hibernated) | `Game::updatePlayersOnline` to populate the DB table |
| `getHibernatedBotCount()` | `size_t` of every `bot.active && bot.hibernated` | `ProtocolStatus::sendInfo` / `sendStatusString` to inflate the count |

Both are guid/count siblings of the existing `getActiveBotNames()` used by the cast
viewer list (see Cast Viewer System above). All three were added to keep the
hibernation-aware bookkeeping in one place — the .so layer — and only export
ABI-stable types across the interface boundary.

### Cadence caveat — `/online` page lags the sidebar by up to 10 minutes

The sidebar widget pulls from the live TCP status probe (responds within ~6s of any
visit). The `/online` page reads `players_online` which is only refreshed every 10
minutes by the `UPDATE_PLAYERS_ONLINE_DB` cycle. After a restart the page can show
0 for up to 10 minutes while the widget already shows the inflated count. If instant
`/online` is needed: shorten the dispatcher constant or trigger the cycle explicitly
on login.

### What was intentionally left untouched

- `Game::checkPlayersRecord` ([game.cpp:8657](../../../src/game/game.cpp#L8657))
  still calls bare `getPlayersOnline()`. The "max concurrent players online" record
  remains a real-activity metric, not a bot-roster size. Change `getPlayersOnline()`
  itself if the record should reflect the inflated count.
- `ProtocolStatus REQUEST_EXT_PLAYERS_INFO` (player name + level list) is not
  inflated. Only the count field that MyAAC and trackers actually display is. Bot
  names already appear in the cast viewer list via `getActiveBotNames()` /
  `kShowBotsInCastList` (Cast Viewer System above).
- The pre-existing IP-dedup-cap-5 anti-multiclient filter in the XML path was
  dropped during unification (commit `21a6072e7`). It was a relic that excluded
  awake bots (IP=0) and isn't worth keeping when it diverges the XML count from
  the binary and DB pipelines.

### Verification

```sql
-- DB-table pipeline (should be ~200 after each 10-min cycle when flag enabled)
SELECT COUNT(*) FROM players_online;

-- Inspect MyAAC's cached widget value (refreshed every ~6s)
SELECT name, value FROM myaac_config
WHERE name IN ('status_players', 'status_lastCheck', 'status_online');

-- Force MyAAC to re-probe on next page load (e.g. to verify after restart)
UPDATE myaac_config SET value = 0 WHERE name = 'status_lastCheck';
```

```bash
# What the sidebar widget will render on the next page load
curl -s http://127.0.0.1/ | grep -oE 'id="players"[^>]*>[0-9]+'

# What the /online page will show
curl -s http://127.0.0.1/index.php/online | grep -oE 'Currently [0-9]+ players'
```

### Pre-existing upstream bug fixed in passing

`Game::updatePlayersOnline` previously logged `[error] Failed to update players online`
every 10 minutes when all bots were hibernated and no real players were connected. The
transaction callback returned `false` ("no rows changed") and the wrapper interpreted
that as transaction failure. The rewrite always returns `true` from the callback —
real transaction failures are still surfaced by `DBTransaction::executeWithinTransaction`'s
own catch/commit-failure logs. Full background: §60 of
the internal design notes

---


</details>

<details>
<summary><strong>Z-Level System</strong></summary>


Some cities have temples on different z-levels than their streets:

| City | Temple Z | Walk Z | Notes |
|------|----------|--------|-------|
| Ankrahmun | 8 | 7 | Temple underground, city surface |
| Darashia | 1 | 7 | Temple on tower, city surface |
| Kazordoon | 11 | 11 | Underground dwarf city |
| Edron | 8 | 8 | Mountain city |
| Rathleton | 6 | 6 | Main level |
| Issavi | 5 | 5 | Main level |
| Others | 7 | 7 | Standard surface |

`CITY_WALK_Z[townId]` maps each city to its correct walking z-level.
`getSpawnPosition()` returns a position at the walk z-level (uses first POI if temple z differs).

**Z-filter removed** (2026-03-14): The hunt selection filter that required >=15% of patrol waypoints at `CITY_WALK_Z` was removed. Floor-change navigation is now reliable, so underground hunts are no longer blocked.

---


</details>

<details>
<summary><strong>State Table (`BotState[guid]`)</strong></summary>


Each bot maintains a state table with these fields:

| Field | Type | Description |
|-------|------|-------------|
| state | number | Current BOT_STATE |
| walkTarget | Position | Current walk destination |
| currentPOI | table | Current POI {name, pos, type} |
| dwellUntil | number | os.time() when dwelling ends |
| lastHealTime | number | Cooldown tracker |
| lastAttackTime | number | Cooldown tracker |
| lastChatTime | number | Cooldown tracker |
| lastKnownHp | number | HP last tick (for damage detection) |
| tickCounter | number | Total ticks since activation |
| pathFailCount | number | Consecutive path failures to current target |
| consecutivePOIFails | number | POIs failed in a row (triggers emergency teleport at 5) |
| visitedPOIs | table | Last 5 POI indices visited |
| combatDecision | string | "fight", "flee", or nil |
| combatStartTime | number | When combat started |
| attackerId | number | Creature ID of attacker |
| targetLastSameZPos | table | {x,y,z} of target's last same-z position (stair pursuit) |
| chaseLastTargetPos | table | {x,y,z} last pathfind target (avoid recalc) |
| lastAttackerSeen | number | os.time() when attacker was last visible (60s memory) |
| ignoredAttackerId | number | Attacker we're ignoring (too weak) |
| ignoredHitBack | boolean | Did we retaliate once while ignoring? |
| seenPKers | table | {[creatureId]=os.time()} PKers we decided not to attack, cleared 60s after leaving screen |
| pkTarget | number | Creature ID of PK target |
| pkStartTime | number | When PK started |
| pvpManaSpent | number | Total mana regenerated during PvP (budget: 2.5x maxMana, resets on exit) |
| verboseLog | boolean | Per-bot verbose logging flag (Cast Chat + journal) |
| verboseLogAutoEnabled | boolean | True if verbose was auto-enabled by cast viewer |
| travelDestTownId | number | Destination town ID |
| travelUntil | number | When travel pause ends |
| combatHpCheckTime | number | Last HP stalemate sample time |
| combatHpBaseline | table | {botHpPct, targetHpPct} baseline for stalemate |
| combatStalemateCount | number | Consecutive unchanged HP checks (exit at 5) |
| idleDepotTarget | Position | Depot locker to walk to after arriving at depot POI |
| walkToPOITarget | Position | Runtime pathfinding target (travel fallback) |
| walkToPOIFailCount | number | Consecutive pathfinding failures (teleport at 30) |

---


</details>

<details>
<summary><strong>Cavebot Navigation System</strong></summary>


Route-graph-based intra-city navigation and inter-city travel. Bots follow pre-imported
waypoint routes between POIs (temple, depot, bank, shops, boat) with proper z-transitions
via the floor-change state machine (no teleporting).

### Route Graph

Loaded at startup from MySQL tables `bot_city_routes` + `bot_city_route_waypoints`.
Routes are parsed from source names like `venore|temple~bank:` into `source="temple"`, `dest="bank"`.

```
BotHuntData.routeGraph[townId] = {
    pairs = { [src] = { [dst] = {waypoints} } },
    pois  = { [name] = {x, y, z} }
}
```

- **Venore**: 151 routes, **Thais**: 98, **Port Hope**: 160
- Lookup: `BotHuntData.findRoute(townId, src, dst)` — tries direct, then reversed
- POI detection: `BotHuntData.detectNearestPOI(townId, pos)` — finds nearest POI by position

### Unified Waypoint Following (`followWaypoints()`)

All waypoint-following is handled by a single `followWaypoints()` function, called by
city routes, travel_to, travel_from (leaving), and patrol. Configured per-phase via
`WaypointFollowConfig`:

```cpp
struct WaypointFollowConfig {
    int32_t globalTimeoutMs = 300000;  // 5 min global stuck timeout
    int32_t perWpStuckMs = 30000;      // 30s per waypoint before skip
    bool enableLookaheadSkip = false;   // patrol only: drift recovery during combat
    bool enableTeleportStand = false;   // patrol only: step onto teleport tiles
    int32_t zChangeGraceMs = 500;       // pause after z-change (prevents cascading FC skips)
    std::string logPrefix = "WP";       // log prefix for this phase
};
```

**Key behaviors:**
- **Type-based arrival distances**: STAND/HOLE/STAIRS=0, NODE=1, LADDER/ROPE/USE_WITH=1, NPC_INTERACT=3, Walk-on FC=3 (inverted z-check)
- **Walk-on FC tiles**: Arrival = `bot.z != wp.z` (z changed after stepping on stair). A* patched via `botAllowFcPath` flag
- **Z-change grace period (500ms)**: After any z-change, ALL waypoint processing pauses for 500ms. Prevents cascading false positives where the bot is on a different z and burns through stair waypoints in one tick
- **NODE random-tile navigation**: All NODE waypoints use 9-candidate (center + 8 adjacent) Fisher-Yates shuffle for natural movement
- **Teleport STAND**: Patrol-only — steps onto teleport tiles with `FLAG_NOLIMIT` when adjacent
- **Look-ahead skip**: Patrol-only — scans 8 waypoints ahead to skip past current if bot drifted during combat
- **Z-mismatch**: 10 failures → skip waypoint (non-FC tiles only)
- **Stuck detection**: Time-based 30s per waypoint, 5-min global timeout
- **Action waypoints**: `handleActionWaypoint()` on arrival (machete, use_with, ladder, levitate)
- **PZ fallback**: In protection zones, uses `FrozenPathingConditionCall` to walk onto creature-occupied tiles

**Callers:**
- `followCityRoute()` — thin wrapper, handles route completion cleanup
- `doHuntTravel()` — handles TRAVEL_TO timeout (5 min → teleport to patrol start)
- `doHuntLeaving()` — handles combat scanning/pausing before calling followWaypoints
- `doHuntPatrol()` — handles hunt timer, monster scanning, combat pausing, patrol cycle management

### Shop Exit Step (Preparation → TRAVEL_TO)

After the PREPARING phase (depot → shop → wait), a shop exit step checks if the bot
can reach the first 5 travel_to waypoints via A* pathfinding. If unreachable (e.g.,
Edron potion shop on different z-level behind stairs), follows a `potions→exit-potions`
city route to get back to street level before starting TRAVEL_TO.

- prepareStep flow: 0=walk_depot → 1=at_depot → 2=walk_shop → 3=at_shop → 4=exit_shop → 5=done
- Tries `potions→exit-potions` first, falls back to `potions→depot`
- Edron has a dedicated 5-waypoint exit route via stairs

### Fast Tick System

During active navigation (`state.cavebotCommand` is set), the think interval drops from
1000ms to 100ms. This makes walking seamless with <100ms response at waypoints and
z-transitions. All tick-dependent timers use `os.time()` to work correctly at both speeds.

### POI Delays

When a sequence arrives at a POI, the bot waits before continuing:

| POI Type | Delay | Action on Arrival |
|----------|-------|-------------------|
| depot | 60-180s | Walk to nearest free locker, then wait |
| bank, potions, runes, ammo, food | 10-90s | Say "hi" to NPC |
| boat, carpet | 5-10s | Say "hi" to NPC |

### Depot Locker Interaction

1. Scan 23x23 viewport for depot lockers (`TILESTATE_DEPOT` flag), check z±1 with distance penalty
2. Tier priority: unoccupied + free adjacent > unoccupied + creature adjacent > occupied > closest
3. Test reachability: find walkable adjacent tile, verify `getPathTo()` succeeds
4. Walk to adjacent tile of chosen locker (5-retry limit with blacklist, max 5 lockers before giving up)
5. Once adjacent: wait 20-60s at locker, then reroll:
   - **70%**: Stay at locker (dwell in place)
   - **20%**: Roam to random PZ tile in depot (BFS-constrained — path never leaves PZ zone)
   - **10%**: Step to closest non-PZ tile (just outside depot entrance, scans z±2 for underground depots)
6. Walking to roam/outside targets uses persistent `s_depotDwellWalkTarget` with floor-change support
7. **Stale-walk guard**: Both `s_depotDwellWalkTarget` and `hasDepotTarget` (locker walk) blocks have a 10s stale-walk timeout via `s_staleWalkStart`. If `listWalkDir` stays non-empty for ≥10s (stuck path), the queue is cleared and the stall is logged. Prevents bots from being permanently stuck in depot dwell loops (symptom: `walking=19+` in `IDLE_SUMMARY`)

### Stuck Bot Safety Valves

Multiple timeout and recovery mechanisms prevent bots from getting permanently stuck:

#### Route Stuck (5 min)
- **Route following** (`followCityRoute`): tracks last waypoint index advance via `s_routeProgress`. If no progress for 5 minutes, **aborts the route** (sets `cityRouteIdx = size`, returns false) so the caller's route-complete handler fires and can recover.
- **Travel routes (walk_from_boat/walk_to_boat)**: same mechanism. If `startCityRoute()` keeps failing for 5 minutes (no route exists), logs: `"STUCK: No route boat->depot in Town for Xmin — bot suspended"`
- Progress timer resets when: waypoint advances, route completes, new route starts, or bot reactivates

#### IDLE hasWalkTarget Deadlock (2 min)
- `s_walkTargetTimer` tracks how long `hasWalkTarget` has pointed at the same target. After 2 minutes with no change, clears `hasWalkTarget` and `currentPOI` so reroll can fire.
- Prevents permanent IDLE deadlock from any code path that leaves `hasWalkTarget` stuck.

#### IDLE Z-Mismatch FC Recovery
- When a city route completes but bot is on wrong z-level, `hasWalkTarget` is cleared BEFORE attempting FC recovery. This ensures reroll isn't blocked even if FC recovery fails or loops.
- If no FC history exists (`s_lastFcPositions`), target is cleared immediately.

#### TRAVELING Z-Mismatch (max 2 FC attempts)
- `s_travelFcRecoveryCount` limits FC recovery to 2 attempts per travel leg. After exhaustion, bot teleports directly to the boat position via `internalTeleport`.
- Prevents infinite z-level oscillation when FC recovery creates new entries that defeat the erase guard.

#### Global TRAVELING Timeout (15 min)
- `s_travelStartTime` tracks when bot entered TRAVELING. After 15 minutes, teleports to temple and goes IDLE.
- Catches all stuck TRAVELING scenarios (missing routes, broken waypoints, etc.).

#### LEAVING Phase Timeout (5 min)
- `s_leavingPhaseStart` set by `beginHuntPhase(LEAVING)`. After 5 minutes, teleports to temple and starts RESUPPLYING.
- `findNearestRecoveryRoute()` now calls `beginHuntPhase(LEAVING)` instead of setting phase directly, ensuring this timeout always fires.

### Navigation Event Tracking (Diagnostics)

Bot navigation issues are tracked in the `bot_nav_events` MySQL table for diagnostics. Events are buffered in memory and flushed to the database every 30 seconds. Each unique `(event_type, hunt_script_id, route_town_id, route_type)` combination gets one row with an incrementing `event_count`.

**Tracked event types:**
| Event Type | Description |
|---|---|
| `route_stuck` | City route stuck at waypoint for 5+ minutes |
| `idle_stale_target` | IDLE walkTarget stuck for 2+ minutes |
| `idle_z_mismatch` | Z-level mismatch after route completion (FC recovery or no FC history) |
| `travel_z_teleport` | Travel FC recovery exhausted, teleported to boat |
| `travel_timeout` | Global 15-min travel timeout, teleported to temple |
| `travel_no_route_boat` | Can't reach boat NPC for 5+ minutes |
| `hunt_abort` | Hunt aborted (with reason: stuck, pk_threat, safety timeout, etc.) |
| `leaving_timeout` | LEAVING phase 5-min timeout, teleported to temple |
| `leaving_teleport` | No travelFrom/recovery waypoints, teleported to temple |
| `hunt_teleport_patrol` | Teleported to patrol start (>30 tiles or wrong z) |
| `patrol_wp_stuck` | Patrol waypoint skipped after stuck threshold |

**Query examples:**
```sql
-- Top offenders
SELECT event_type, hunt_script_name, town_name, event_count FROM bot_nav_events ORDER BY event_count DESC LIMIT 20;
-- Hunt scripts that never work
SELECT id, name, town_name, successful_hunts FROM bot_hunt_scripts WHERE successful_hunts = 0 AND enabled = 1;
```

### Hunt Success Tracking

The `bot_hunt_scripts` table has `successful_hunts` and `total_kills` columns that are incremented when a hunt ends normally (timer or kill limit reached) with at least 1 kill. This allows filtering out scripts that are enabled but never produce successful hunts.

### community scripts Script Import + Extended Waypoint Types

The bot engine supports waypoints from several community script formats:

**New waypoint types** (in `bot_engine_interface.hpp` WaypointType enum):
| Type | Value | Behavior |
|------|-------|----------|
| `MACHETE` | 11 | Walk adjacent, create temp machete (ID 3308), `useItemEx()` on target tile |
| `USE_WITH` | 12 | Walk adjacent, create temp item (waypoint.itemId), `useItemEx()` on target. If itemId=0 (use_on_tile), finds top item on tile and calls `useItem()` directly |
| `NPC_INTERACT` | 13 | Walk within 3 tiles, say "hi" via `internalCreatureSay()` |
| `TELEPORT` | 14 | Fire `internalTeleport(player, wp, true)` immediately on encounter — no walking, no arrival check. Used for cross-continent NPC teleports the bot can't replicate on foot (e.g. Bibby Bloodbath → Lower Roshamuul, Nightmare Isles entrance portal). Handled in both `followWaypoints` (travel routes) and `advanceHuntWaypoint` (patrol) by branching BEFORE the arrival/walk logic: clear `listWalkDir`, internalTeleport, sync `currentPos`/`lastPos`, advance index, set 500ms pause to let the server settle. Source-format: cavebot cfg `label:TELEPORT` followed by `goto:X,Y,Z` (the destination); `cfg_importer` converter promotes the next coord-step to `teleport` type. |

**Walk-on floor changes** (stairs, open holes/trapdoors — tiles with `TILESTATE_FLOORCHANGE`):
- A* patched in `tile.cpp:610` to allow bot players onto FC tiles, gated by `player->isBotAllowFcPath()`
- Flag set only in `goTo()` when `arrivalDist=0 && isWalkOnFcTile(target)` — never during combat
- All 4 waypoint paths (city routes, travel, leaving, patrol) use inverted z-check: `arrived = (bot.z != wp.z)`, distance-independent (combat drift doesn't prevent advancement)
- `doHuntPatrol()` detects walk-on FC z-change BEFORE `if (huntTargetId > 0) return` — immediate advancement even if bot already targeted a monster on the new floor
- Item-use FCs (rope/shovel/ladder) still use the FC state machine
- DB: 1007 implicit FC NODE waypoints updated to STAND for exact positioning

**Z-REDIRECT** (z-mismatch on walkOnFc waypoints):
- When `bot.z != wp.z` for a walkOnFc waypoint, Z-REDIRECT computes `sameZTarget = (wp.x, wp.y, bot.z)` (same XY, bot's current z) and calls `goTo()` to walk the bot onto the FC tile on the current floor, letting the server's stair mechanic handle the z-change
- **Log spam guard**: `castLog` fires only once per transition — the `if (hasFc && !listWalkDir.empty()) return` early-return comes first, so the log only appears when walk queue is empty (first initiation)
- **Reverse staircase guard**: Before calling `goTo()`, checks if `waypoints[nextIdx].z == bot.currentPos.z`. If true, the bot already completed the transition (e.g. via combat drift through a nearby FC tile) — `waypointIdx` advances directly instead of navigating to the reverse staircase. This matches the universal Tibia bot standard: `currentZ == targetZ` → waypoint already reached

**Teleport detection during routes:**
- `processBot()` teleport detection (posDiff > 10 or z-jump) skips route clearing when near USE_WITH waypoints (wagons, shrines, carpet teleports produce expected large position jumps)
- Also exempts TRAVELING state during teleport/walk_from_boat phases
- Only fires when `posDiff > 10 || zJump` — normal 1-tile walking never triggers it
- Legitimate route teleports log "ROUTE_TP: Expected N tile jump" instead of clearing state

**Teleport tiles (ventilation grilles, slime slides, magic forcefields):**
- `isWalkOnFcTile()` detects both `TILESTATE_FLOORCHANGE` and `TILESTATE_TELEPORT`
- `tile.cpp` allows both tile types when `botAllowFcPath` is set (waypoint navigation only)
- Bot walks directly onto teleport tiles, server's action script handles the teleport
- Waypoints on teleport tiles use `stand` type — no special USE_WITH needed

**Multi-entry town travel:**
- `travelPositions_` stores `vector<Position>` per town — supports multiple entry points (boat + carpet)
- `getTravelPosition()` picks randomly from available positions (50/50 boat vs carpet)
- `walk_from_boat` tries multiple route sources: boat→depot, carpet→depot, boat→temple, carpet→temple, then auto-detect
- Towns with dual entry: Farmine (steamboat + carpet), Kazordoon (steamboat + carpet), Edron (boat + carpet)

**Quest/access storages (set on every startup in bot_manager.lua):**
- The New Frontier quest (Farmine): Questline=20 (Stage 3 elevator), all Mission01-Mission10=2, 12 Tomes of Knowledge, ZaoPalaceDoors, SnakeHeadTeleport, CorruptionHole
- Threatened Dreams (Feyrist earth shrine entrance): Mission01[1] = 16
- Wagon Ticket (Kazordoon ore wagons): Storage.WagonTicket = max INT32

**Z-change delay in waypoint paths:**
- After USE_WITH, LADDER, or DOOR waypoints: pause 1 tick (~100ms) to let server process the action
- After any waypoint where the NEXT waypoint has a different z-level: pause 1 tick
- Applies to: followCityRoute, doHuntTravel, doHuntLeaving (advanceHuntWaypoint already pauses every wp)
- PvP/PK chase/retreat: NOT affected (uses direct pathfinding, not waypoints)

**LADDER waypoint handling:**
- Uses `findLadderItem()` which searches by known ladder item IDs (433, 482, 483, 1948, 1968, 5542, etc.)
- NOT the generic USE_WITH item selection (which can pick ground tiles instead of the ladder)

**Boat proximity check after route completion:**
- Uses last waypoint of completed route (`s_lastRouteEndPos`) for position/z check
- Route waypoints cleared by `followCityRoute()` before proximity check runs — must save beforehand
- No automatic teleport-to-boat fallback on z-mismatch (removed to surface bugs)

**Travel POI tracking (boat vs carpet):**
- `travelPositions_` stores `{Position, routePOI}` pairs ("boat" or "carpet")
- `getTravelPosition()` picks randomly and returns both position + POI name
- `s_travelDestPOI`/`s_travelSrcPOI` track which was chosen
- `walk_from_boat` uses `s_travelDestPOI` for route source (not hardcoded "boat")
- `walk_to_boat` tries `depot→srcPOI` first (bot usually at depot)

**Levitate spell casting** (`LEVITATE_UP`/`LEVITATE_DOWN`):
- Sets facing direction via `g_game().internalCreatureTurn(player, faceDir)` from waypoint.extraData ("face_north", "face_south", etc.)
- Casts actual spell "exani hur up/down" via `g_spells().playerSaySpell()` + visual via `player->saySpell()`
- Falls back to floor-change state machine on spell failure

**Quest system**:
- 5% of bots (`guid % 20 == 0`) are quest bots, preferring `scriptCategory=="quest"` scripts
- Quest patrol is one-shot (no loop) — reaching end of waypoints triggers `abortHunt("quest completed")`
- Quest bots fall back to regular hunts when no quests are available

**City walking (traveling scripts)**:
- 20 city-to-city routes parsed from community scripts Auto City Walker (5 cities × 4 destinations)
- Hub-and-spoke design through central crossroads, stored as `scriptCategory="traveling"` in DB
- `tryStartCityWalk()`: 30% reroll chance, 1-bot-per-route reservation, starts directly in TRAVEL_TO
- On completion: release reservation, transition to IDLE, set nextRerollTime 30-120s

**Door access for bots** (Lua-side):
- `key_door.lua`: Bots open any locked/closed door without keys
- `quest_door.lua`: Bots bypass storage requirement (`player:isBotPlayer()` check)
- `level_door.lua`: Bots already bypass level check
- All door scripts: nil-safe `getPosition()` for bot temp items (prevents Lua errors)

### Inter-City Travel (`travel` command)

Full multi-step journey:
1. Navigate to boat POI in current city (route graph or runtime pathfinding fallback)
2. Walk to boat NPC within 3 tiles, say "hi", wait 3-10 seconds (randomized)
3. Teleport to destination city's boat position (with effect)
4. Navigate from destination boat to temple (route graph or runtime pathfinding fallback)

**Runtime pathfinding ("walking_to_poi" phase):** When no DB route exists for a leg of the
journey, the bot uses `goTo()` each tick to walk directly. Handles z-transitions via the
floor-change state machine. Falls back to teleport after 30 consecutive pathfinding failures.

---


</details>

<details>
<summary><strong>`/cavebot` Admin Command</strong></summary>


God-access talkaction for debugging and controlling bot navigation via C++ BotEngine.
Supports multi-word bot names via quotes or auto-detection.

### Usage

```
/cavebot <botname> <command> [args]
/cavebot "Bot Name With Spaces" <command> [args]
```

### Commands

| Command | Args | Description |
|---------|------|-------------|
| `status` | — | Full report: position, state, hunt phase, health, travel |
| `pos` | — | Short position report |
| `goto` | `x,y,z` | Walk to specific coordinates |
| `teleport` | `x,y,z` | Teleport bot to exact position |
| `navigate` | `<poi>` | Walk to POI in current city (temple, depot, boat, shop) |
| `sequence` | `a,b,c` | Multi-leg navigation (comma-separated POI names) |
| `travel` | `<city> [wp#]` | Inter-city boat travel, or teleport to route waypoint # for debugging |
| `poi` | — | Detect nearest POI to current position |
| `routes` | — | List available travel routes from current town |
| `hunt` | `[id\|name]` | Start hunt — no args picks random eligible (level-only filter, no vocation restriction), or filter by name/id |
| `endhunt` | — | Force-expire the current hunt's time so the next PATROLLING tick transitions to LEAVING. Use to test LEAVING/RESUPPLYING phases without waiting 30-180 min (or landing a `debug_kills 1` kill). Bot must be in a hunt; effect is observable only once the bot reaches PATROLLING. |
| `debug_kills` | `<N>` | Set per-bot hunt kill limit (0 = time-based only, default). Once `huntKillCount >= N`, hunt transitions to LEAVING — same path as natural time expiration. Useful pre-set before `hunt` to short-circuit testing. |
| `partyhunt` | `[scriptId]` | EK only. Start a party hunt — auto-recruits an ED partner (and additional members up to script's preferred party size) and begins the hunt as the leader. With no arg, picks a random eligible party-tagged script; with `scriptId`, forces a specific script. Works on hibernated EKs (wakes the team after virtual party formation). |
| `advstone` | `[chest\|dummy [wepId]\|wp]` | Manually start an Adventurer's Stone trip. Bot must be standing in a temple PZ. Optional mode forces dwell behavior (chest/dummy/waypoint); `dummy <wepId>` forces a specific Lasting Exercise weapon (35290 wand, 35288 bow, etc.). No args = random mode + random weapon. One-shot override (resets after trip). |
| `stairs` | `up\|down` | Find and use nearest stairs |
| `scan` | `[radius]` | Scan for floor-change tiles |
| `step` | `<dir>` | Force a single `creature:move()` step (n/s/e/w/ne/nw/se/sw) |
| `use` | `x,y,z` | Use the top stacked item (or ladder/sewer/ground) at the given tile. Diagnostic — test sewer/ladder/door interaction without waypoint navigation. |
| `tileinfo` | `x,y,z` | Dump tile flags (FC directions, PZ) + ground + stacked items (id/name/isLadder/type/floorChange). Diagnostic for waypoint debugging. |
| `scandoors` | `[radius]` | Scan a `radius`-tile box (default 15, max 30) around the bot for items in the door table; reports first 20 hits. |
| `placeitem` | `<id> <x,y,z>` | Spawn an item at the given tile (testing — e.g. drop a key/lever for door logic). |
| `removeitem` | `<id> <x,y,z>` | Remove the first item with `id` from the given tile. |
| `active` | `[x,y,z]` | Activate bot — no args teleports to temple, with coords teleports to position |
| `inactive` | — | Deactivate bot (true offline — removes from world; re-activating reloads from DB at last saved position) |
| `hibernate` | — | Move bot to hibernated state — preserves `state`, `huntScriptId`, `huntPhase`, `huntKillCount`, `partyHuntId`, etc. without keeping a live `Player` in the world. Use `wake` to restore. Fails on in-flight death. |
| `wake` | — | Wake a hibernated bot back into the world at its preserved position. Restores active hunt/party state. |
| `stop` | — | Halt all activity, release hunt reservation, clear state. Applies a 5-min cooldown blocking auto-hunt/travel (use `resume` to cancel). |
| `resume` | — | Resume normal AI (set to IDLE), clear stop cooldown |
| `log` | `on\|off` | Toggle verbose logging (alias: `verbose on\|off`) |
| `debug` | `on\|off\|status\|grid on\|off\|events on\|off\|snapshot <ms>` | Per-bot debug stream control. `debug on` enables the journalctl-tailable `[BOT:DBG]`/`[BOT:EVT]` stream (ASCII grid + event log). `snapshot <ms>` sets the grid interval (100-60000 ms). Toggled in bulk via `/cavebot reload debug,N`. |
| `info` | — | Show z-transitions and detailed state summary |
| `pk` | `<target name>` | Force bot to PK a named target (supports spaces in names) |
| `reload` | — | **Global** (no bot name): hot-reload `libbot_engine.so` without server restart |
| `routewp` | `<town> [src~dst]` | **Global**: list all routes for a town, or list waypoints of a specific route |
| `routeadd` | `<town> <src~dst> <seq> [type] [x,y,z]` | **Global**: insert waypoint into MySQL route at admin's position (or explicit x,y,z). `/cavebot reload` to apply. |
| `routedel` | `<town> <src~dst> <seq>` | **Global**: delete waypoint from MySQL route and shift remaining seqs down. `/cavebot reload` to apply. |
| `huntwp` | `<search> [phase]` | **Global**: list hunt scripts matching search, or list waypoints of a specific script+phase |
| `huntadd` | `"<hunt name>" <phase> <seq> [type] [x,y,z]` | **Global**: insert waypoint into hunt script phase. `/cavebot reload` to apply. |
| `huntdel` | `"<hunt name>" <phase> <seq>` | **Global**: delete waypoint from hunt script phase. `/cavebot reload` to apply. |
| `hunttarget` | `<search>` | **Global**: list hunt scripts matching search with target counts, or list targets of a specific script |
| `targetadd` | `"<hunt name>" <monster> [pri] [cnt]` | **Global**: add a monster target to a hunt script. `/cavebot reload` to apply. |
| `targetdel` | `"<hunt name>" <monster>` | **Global**: delete a monster target from a hunt script. `/cavebot reload` to apply. |
| `whohunts` | `[search]` | **Global**: list all active hunt reservations (script ID, name, bot name, guid). Optional search term filters by script name |
| `population` | — | **Global**: on-demand liveness dump — prints the per-town bot-count-by-state report (`[POPULATION]`) **and** the per-anchor proximity/wake snapshot (`[PROXIMITY]`) as two messages. These are no longer broadcast to admin chat on a timer — `journalctl` still logs both periodically. `/cavebot proximity` is deprecated → merged here. |
| `poi` | `<town>` | **Global**: list all POIs for a town from MySQL `bot_city_pois` table |
| `poiadd` | `<town> "<name>" <type> [x,y,z]` | **Global**: add POI to MySQL. Types: depot/temple/boat/shop/npc. Uses admin pos if no coords. `/cavebot reload` to apply. |
| `poidel` | `<town> "<name>"` | **Global**: delete POI from MySQL. `/cavebot reload` to apply. |
| `poiupdate` | `<town> "<name>" [x,y,z]` | **Global**: update POI position. Uses admin pos if no coords. `/cavebot reload` to apply. |
| `simulate route` | `<town> <src~dst>` | **Global**: teleport admin through route waypoints at 1/sec |
| `simulate hunt` | `"<name>" [phase]` | **Global**: teleport admin through hunt waypoints at 1/sec. Phase: patrol/travel_to/travel_from |
| `simulate poi` | `<town>` | **Global**: teleport admin through town POIs at 1/sec |
| `simulate pause\|continue\|stop` | — | **Global**: control active waypoint simulation |
| `party` | `<voc1,voc2,...>` | **Any player**: summon bot players into party (ek/ed/ms/rp, case-insensitive). Max 4 bots. |
| `party leave` | — | **Any player**: dismiss all party bots, restore their previous state |
| `claim` | `[name]` | **Any player**: claim the hunt spawn you're standing in. Kicks the bot reserving it to temple and blocks all bot assignment of that script (+ its spawnGroup) for 1h. `name` picks/disambiguates by hunt-script name. In-memory only (lost on `/cavebot reload` + restart). See "Player Spawn-Claim" below. |
| `release` | — | **Any player** (alias `unclaim`): release your own active spawn claim early |
| `claims` | — | **Global**: list active player spawn-claims (owner + minutes left) |
| `clearclaim` | `<name>` | **Global**: admin force-release a player's spawn claim by hunt-script name |
| `house` | `"<name>" owner\|sub-owner\|release` | **Any player**: take a BOT-owned house (`owner` — free, gated on not already owning a house + bank ≥ the house's rent), add yourself to its native sub-owner list (`sub-owner`), or hand a house you own back to the market (`release`). Exact quoted name. See "Bot House Claim" below. |

### Bot House Claim (`/cavebot house`)

Lets a normal player take over one of the ~794 bot-owned houses (bots fill houses
for ambiance; this is how a real player acquires one). Pure Lua + an additive,
self-provisioned schema — no C++/.so rebuild.

```
/cavebot house "Paupers Palace, Flat 05" owner       -- become the owner (free)
/cavebot house "Paupers Palace, Flat 05" sub-owner   -- add self to SUBOWNER_LIST
/cavebot house "Paupers Palace, Flat 05" release     -- give YOUR house back to the market
```

- **Name match**: quoted, exact (case-insensitive) against `Game.getHouses()`; no match → reject.
- **`owner`**: rejects if the house isn't bot-owned (also blocks guildhalls/real-player houses), if you already own a house (`player:getHouse()`), or if `getBankBalance() < house:getRent()` (affordability gate — no gold is deducted). On the **first-ever** claim of a house, the bot's movable-item layout is snapshotted **once, permanently** into `bot_house_origin` / `bot_house_origin_items`.
- **`sub-owner`**: appends you to the house's native `SUBOWNER_LIST` (`house:setAccessList`). No cap. Bot keeps ownership.
- **`release`**: only a house you currently own; sets `owner=0` (back on the market).
- **Auto-reclaim**: the `BotHouseReclaim` globalevent (`data/scripts/globalevents/bot_house_reclaim.lua`, 5-min, chunked 5/tick) returns any **tracked** house to its original bot and restores the snapshot verbatim once it's genuinely vacant — detected by a conservative direct read `owner=0 AND state=0 AND bidder=0 AND highest_bid=0 AND new_owner<=0` (so it never fights a Cyclopedia auction).
- **Storage note**: house interiors live as one serialized `tile_store` blob per house, rewritten from live memory on every world save — so restore re-places items into the live map (`Game.createItem`), it can't just rewrite the blob. Helpers + schema: `data/scripts/lib/bot_house_claim.lua`, `data/scripts/lib/sql/bot_house_claim.sql`.

### Route Editing (`/cavebot routewp` / `routeadd` / `routedel`)

Edit and inspect city route waypoints directly from in-game. Town is specified by name (case-insensitive).

**routewp** — list routes or inspect waypoints:
```
/cavebot routewp darashia                             -- list all enabled routes for Darashia
/cavebot routewp darashia depot~boat                  -- list all waypoints for that route
```

**routeadd** — insert a waypoint, shifting existing waypoints at seq >= N up by 1:
```
/cavebot routeadd darashia depot~boat 0                       -- [stand] at admin's current position
/cavebot routeadd darashia boat~depot 5 ladder                 -- [ladder] at admin's current position
/cavebot routeadd darashia depot~boat 3 stand 32310,32210,6    -- [stand] at explicit coordinates
/cavebot routeadd darashia depot~boat 3 32310,32210,6          -- [stand] at explicit coords (type defaults to stand)
```

**routedel** — remove a waypoint and shift remaining seqs down by 1:
```
/cavebot routedel darashia depot~boat 3                        -- deletes wp 3, renumbers 4→3, 5→4, etc.
```

Types: `stand` (default), `node`, `ladder`, `rope`, `hole`, `stairs_up`, `stairs_down`, `door`

**Important**: Changes are persisted to MySQL immediately but only loaded into the bot engine after `/cavebot reload`.

### Hunt Waypoint Editing (`/cavebot huntwp` / `huntadd` / `huntdel`)

Edit and inspect hunt script waypoints directly from in-game. Hunt scripts are identified by name (fuzzy search with LIKE, or exact match in quotes).

**huntwp** — search scripts or inspect waypoints:
```
/cavebot huntwp pirates                              -- list all scripts matching "pirates"
/cavebot huntwp "Pirates Yalahar"                    -- list phases + wp counts for that script
/cavebot huntwp "Pirates Yalahar" patrol             -- list patrol (hunt_patrol) waypoints
/cavebot huntwp "Pirates Yalahar" travel_to          -- list travel_to waypoints
```

**huntadd** — insert a waypoint into a hunt script phase, shifting seq >= N up by 1:
```
/cavebot huntadd "Pirates Yalahar" patrol 5                       -- [stand] at admin's position
/cavebot huntadd "Pirates Yalahar" patrol 5 door                  -- [door] at admin's position
/cavebot huntadd "Pirates Yalahar" patrol 5 stand 32640,31301,6   -- [stand] at explicit coords
/cavebot huntadd "Pirates Yalahar" travel_to 0 stairs_up          -- insert at start of travel_to
```

**huntdel** — remove a waypoint and shift remaining seqs down by 1:
```
/cavebot huntdel "Pirates Yalahar" patrol 19                      -- deletes seq 19, renumbers 20→19, etc.
```

Phases: `travel_to`, `patrol` (alias for `hunt_patrol`), `travel_from`
Types: `stand` (default), `node`, `ladder`, `rope`, `hole`, `shovel`, `stairs_up`, `stairs_down`, `door`, `lever`, `levitate_up`, `levitate_down`, `label`, `conditional`

**Important**: Changes are persisted to MySQL immediately but only loaded into the bot engine after `/cavebot reload`.

### Hunt Target Editing (`/cavebot hunttarget` / `targetadd` / `targetdel`)

View and edit the monster targets assigned to hunt scripts directly from in-game.

**hunttarget** — search scripts or list targets:
```
/cavebot hunttarget wyrm                               -- list all scripts matching "wyrm" with target counts
/cavebot hunttarget "Wyrm Liberty Bay"                 -- list targets (name, priority, count) for that script
```

**targetadd** — add a monster target to a hunt script:
```
/cavebot targetadd "Wyrm Liberty Bay" Elder Wyrm                -- add with defaults (priority=1, count=1)
/cavebot targetadd "Wyrm Liberty Bay" Elder Wyrm 5              -- add with priority=5, count=1
/cavebot targetadd "Wyrm Liberty Bay" Elder Wyrm 5 2            -- add with priority=5, count=2
```

**targetdel** — remove a monster target from a hunt script:
```
/cavebot targetdel "Wyrm Liberty Bay" Elder Wyrm                -- deletes Elder Wyrm target
```

**Important**: Changes are persisted to MySQL immediately but only loaded into the bot engine after `/cavebot reload`.

### Empty `bot_hunt_targets` = attack all monsters during PATROLLING (non-quest)

When a hunt script has zero rows in `bot_hunt_targets`, the engine treats it as "attack any monster the spectator scan finds" — **but only while `huntPhase == PATROLLING` AND `is_quest = 0`**. This is the model used by all 10 hand-authored manual scripts (Roshamuul cluster, Nightmare Isles, Asura Palace, Draken Walls, Farmine Lizard City) where enumerating spawn populations was undesired.

Behavior matrix:

| Script state | `huntPhase` | Target match logic |
|---|---|---|
| Empty targets, hunt | PATROLLING | match = any monster |
| Empty targets, hunt | TRAVEL_TO / LEAVING / RESUPPLYING / PREPARING | match = none (no engagement) |
| Empty targets, quest | any | match = none (quests self-defend via `attackerId` path only — they don't seek targets) |
| Populated targets | any | match = exact lowercase name comparison against `script->targetNames` |

Implementation: three target-scan call-sites (`findThreatCentroid`, `chooseTarget`, `hasNearbyReachableTargets`) gate the all-match fallback on `(bot.huntPhase == HuntPhase::PATROLLING && !script->isQuest)`. The eligibility filters that previously skipped empty-target hunts in four places (`reroll`, `pickHunt`, `pickPartyHunt`, `forceAssign`) were removed.

### Manual Hunt Scripts (`source='manual'`)

Hand-authored hunt scripts converted from OTC cavebot 1.3 `.cfg` exports. As of 2026-05-19 there are 12 in the DB (IDs 2068-2079):

| Name | Town | Min Lvl | Special features |
|---|---|---|---|
| Chosen | Farmine | 200 | 3 levitate waypoints (`exani hur down/up`) — Lizard City portal entry |
| Corruption Hole Chosen | Farmine | 250 | 2 STAND-forced waypoints at (33341, 31167, 7) + (32334, 32087, 7) — portals (client IDs 25053/25054) require exact-tile arrival |
| Nightmare Isles | Roshamuul | 400 | 2 TELEPORT waypoints (entrance + exit) |
| Roshamuul Upper | Roshamuul | 300 | — |
| Roshamuul Lower | Liberty Bay | 170 | 2 TELEPORT waypoints — Bibby Bloodbath NPC in/out |
| Roshamuul Prison -1/-2/-3 | Roshamuul | 350 | three z-level variants of same physical area, distinct spawn_groups |
| Asura Palace Top | Port Hope | 350 | — |
| Asura Palace Cave | Port Hope | 550 | — |
| Draken Walls Farmine | Farmine | 300 | 2 levitate waypoints |
| Farmine Lizard City | Farmine | 300 | 2 levitate waypoints |

Only Chosen + Corruption Hole Chosen carry explicit `bot_hunt_targets` (Lizard Chosen, Ghastly Dragon — taken from the cfg's `label:monsters:` line). The other 10 rely on the §"Empty `bot_hunt_targets`" attack-all-in-patrol path above.

**Source format → DB translation** (local-only `tools/cfg_importer/convert_cfg.py`):

| Cfg line | DB waypoint type | Notes |
|---|---|---|
| `goto:X,Y,Z` | `node` (default) or `stand` | Promoted to `stand` if next wp differs in z OR next wp is a `teleport` (must arrive on exact trigger tile) |
| `use:X,Y,Z` | `use_with` with `extra_data=NULL` | Engine's "use whatever's on this tile" path — prefers items with `actionId` (levers, doors, quest items) |
| `usewith:ITEM,X,Y,Z` | `use_with` with `extra_data=ITEM_ID` | Item-on-tile, e.g. `3457` (small pick) or `3003` (rope) |
| `label:TELEPORT` then next coord-step | `teleport` | Bot warps to the next coord via `internalTeleport` — no walking |
| `function:action levitate_<dir>_<up\|down>` | `levitate_up` / `levitate_down` at prev wp pos | `extra_data=face_<dir>` (face_north/south/east/west) — bot turns then casts `exani hur up/down` |
| `label:level: N+` | `min_level=N` | — |
| `label:monsters: a, b, c` | inserts into `bot_hunt_targets` | Optional |
| `label:<any text> ... (X, Y, Z) ... STAND` | force STAND on that exact wp | User-authored "this coord MUST be stand" override for portal tiles |
| Consecutive duplicate `(type, x, y, z)` | deduped | Cavebot exports often double-record the current tile |
| `label:travel_to` AFTER `hunt_patrol` block | remapped to `travel_from` | Cfg typo correction |

### Player Spawn-Claim (`/cavebot claim` / `release` / `claims` / `clearclaim`)

Lets a **real player** standing inside a monster spawn reserve it for themselves: kicks whatever
bot is hunting/reserving the spawn (to its temple) and blocks **all** bot assignment of that hunt
script — and its `spawnGroup` — for **1 hour**. `claim`/`release` are handled *before* the god gate
(like `/cavebot party`), so any real player can use them; `claims`/`clearclaim` are god-only.

```
/cavebot claim                  -- claim the spawn you're standing in
/cavebot claim Darashia Minotaurs  -- pick/disambiguate by hunt-script name
/cavebot release                -- (alias: unclaim) release your own claim early
/cavebot claims                 -- (god) list active claims + minutes left
/cavebot clearclaim <name>      -- (god) force-release a player's claim by script name
```

**In-memory only — by design.** The claim lives in the engine's `playerClaims_` map; it is
intentionally **lost on `/cavebot reload` and on server restart** (no DB table, no persistence).
The player simply re-issues `claim`. This keeps the feature a `.so`-only change.

**How it works:**
- *Detection* — `detectClaimableScript()` finds the hunt script whose **patrol** waypoints are
  closest to your tile (min Chebyshev distance, hard-gated to the same z, enabled `"hunt"` scripts
  only). Must be within 8 tiles. If two spawns are within 4 tiles of each other you're told to
  disambiguate with `/cavebot claim <name>`.
- *Kick* — `kickSpawnHolder()` reuses the partyhunt force-clear template: aborts the holder's hunt
  (releasing its `activeHunts_`/`activeSpawnGroups_` reservation; dissolving its party if it was a
  leader) and teleports it to temple + IDLE. It re-rolls a *different* hunt within 5–15 s. Hibernated
  holders are handled (reservation freed, no physical teleport).
- *Block* — `isScriptPlayerClaimed()` is consulted at **every** bot hunt-assignment path so no bot
  (awake or hibernated) can re-grab the spawn: `tryStartHunt`, `tryStartCityWalk`,
  `virtualTryStartHunt`/`virtualTryStartPartyHunt` (hibernated), `tryStartPartyHunt`, the recovery-route
  scan (`findNearestRecoveryRoute`), the post-deactivation restore (`restoreSingleBotState`), and a
  defensive guard in `wakeBot`.
- *Release* — a claim ends on **1 h expiry**, an explicit `release`, or **owner logout** (detected by
  a ~15 s `tick()` sweep). One claim per player (a new claim moves the old). A 30 s anti-grief cooldown
  applies to claiming (not releasing).
- *God override* — `partyhunt`/`hunt` force commands bypass claims by design (a god can always force a
  bot onto a spawn); the claim still blocks other bots until it expires or is cleared.

God force commands and admins can clear a stuck/unwanted claim with `/cavebot clearclaim <name>`.
Telemetry: `journalctl` shows `[BotEngine] CLAIM: …` on claim, `… player-claimed` on restore/wake
skips, and `[BotEngine] CLAIM-RELEASE: … reason=expired|owner-offline` on sweep release.

### Hot-Reload (`/cavebot reload`)

Reloads the bot engine shared library (`libbot_engine.so`) without restarting the server.
This is the primary development workflow for `bot_engine.cpp` changes (~90% of iterations).

**Reload sequence:**
1. Collects all currently active bot GUIDs (via `Game.botIsActive(guid)` on each registered bot)
2. Force-deactivates all active bots in the old engine
3. Calls `Game.botReload()` which:
   a. Destroys the old engine instance (`destroyBotEngine()`)
   b. `dlclose` the old `.so`
   c. Copies `./libbot_engine.so` to a unique temp path (e.g., `./libbot_engine.so.12345.tmp`) — **this is critical**: Linux caches `dlopen` by inode, so re-opening the same path after `dlclose` returns the old (deleted) mapping instead of the new file. The temp copy forces a fresh inode.
   d. `dlopen` the temp copy
   e. Creates a new engine instance (`createBotEngine()`)
   f. Calls `loadHuntData()` on the new engine
   g. Cleans up the previous temp copy (if any)
4. Calls `Game.botStartTickLoop()` — creates a fresh 100ms cycleEvent for the tick loop
5. Calls `Game.botReregisterAll()` — re-registers all online bot players with the new engine
6. Re-activates previously active bots, teleporting each to their town temple (clean start)

**Development workflow:**
```bash
# 1. Edit bot_engine.cpp locally, push
git push origin feat/cavebot

# 2. Server: pull + compile ONLY the .so (~31 seconds)
ssh root@<your-server> "cd /path/to/canary && git pull origin feat/cavebot && \
  cmake --build --preset linux-release --target bot_engine -j3"

# 3. Copy the new .so into place (rm old first to unlink inode)
ssh root@<your-server> "cd /path/to/canary && rm -f libbot_engine.so && \
  cp build/linux-release/bin/libbot_engine.so ."

# 4. In-game: /cavebot reload  (no server restart!)
```

**Manual hot-reload without `/cavebot reload`** (if needed):
```bash
# On the server, after copying the new .so:
# The reload can also be triggered via MySQL bot_commands if the bot_manager
# has reload support, or by restarting the server as a last resort:
systemctl stop canary && cp build/linux-release/bin/canary canary && \
  cp build/linux-release/bin/libbot_engine.so . && systemctl start canary
```

**When to use hot-reload vs full restart:**
- `bot_engine.cpp` only → `.so` build + `/cavebot reload` (no restart)
- `bot_engine_loader.cpp/.hpp` changes → full build + server restart (loader is in main binary)
- `bot_engine_interface.hpp` changes → full build + server restart (ABI boundary)
- Other C++ files → full build + server restart
- Lua files → `systemctl restart canary`

### Cast Chat Debug

Bot actions are automatically visible in Cast Chat when a viewer joins a bot's cast stream.
Messages include: state transitions, combat decisions, spell casts, healing, hunt start/end,
monster targeting/kills, travel events, and **periodic 60s heartbeat** (`STATUS:` messages showing
current state, e.g., "HUNTING 'Darashia Minotaurs' PATROLLING wp 12/49 — 5 kills"). No manual enable needed.

### Command Delivery

Commands can also be sent via MySQL (polled every 10 seconds):

```sql
INSERT INTO bot_commands (bot_name, command, created_at)
VALUES ('Nikolai Dawnbringer', 'hunt darashia mino', NOW());
```

### Examples

```
/cavebot ophelia stoneguard hunt darashia mino
/cavebot ophelia stoneguard travel thais
/cavebot ophelia stoneguard navigate depot
/cavebot ophelia stoneguard status
/cavebot ophelia stoneguard stop
/cavebot "Ophelia Stoneguard" teleport 32957,32076,7
```

### Monitoring

```bash
# Watch all bot engine output
journalctl -u canary --since '1 min ago' | grep BotEngine

# Watch specific bot
journalctl -u canary --since '1 min ago' | grep 'Ophelia'
```

---


</details>

<details>
<summary><strong>`/botspawn` Admin Command</strong></summary>


God-access talkaction to teleport directly to a hunting spawn's first patrol waypoint.
Useful for inspecting spawn locations and debugging hunt scripts.

**Note**: Uses `/botspawn` instead of `/spawn` to avoid conflict with the existing
`/spawn` creature spawning command.

### Usage

```
/botspawn <id|name>        — teleport to spawn by script ID or name substring
/botspawn list [filter]    — list available spawns, optionally filtered
```

Supports spawn names with special characters and spaces — uses plain substring matching.

### Examples

```
/botspawn 42                       — teleport to script ID 42
/botspawn darashia mino            — teleport to first match containing "darashia mino"
/botspawn oramond glooth           — teleport to Oramond Glooth Tower
/botspawn list                     — list all spawns (max 50 shown)
/botspawn list darashia            — list spawns matching "darashia"
```

The command reports the spawn name, script ID, town, coordinates, and patrol waypoint count
on teleport. The `list` subcommand shows `[ok]` or `[no-wps]` status for each spawn.

---


</details>

<details>
<summary><strong>Adding New Behaviors</strong></summary>


**In C++ (`bot_engine.cpp`):**
1. **New state**: Add to `BotAIState` enum in `bot_engine_interface.hpp` (ABI change → full rebuild)
2. **New per-tick behavior**: Add method to `BotEngine` class in `bot_engine.cpp`, call from `tick()`
3. **New state field**: Add to `BotState` struct in `bot_engine_interface.hpp` (ABI change → full rebuild)
4. **New IBotEngine method**: Add to `IBotEngine` in `bot_engine_interface.hpp` + implement in `bot_engine.cpp` (ABI change)
5. **Internal-only changes** (new private methods, logic changes): Edit `bot_engine.cpp` only → `.so` rebuild + hot-reload

### Think Loop Order (1s normal / 100ms during navigation)

```
1. Poll MySQL command queue (every 2 seconds, time-based)
2. Floor-change state machine (if active)
3. Cavebot command processing (navigate/sequence/travel — skips normal AI)
4. Position logging (every 30 seconds, time-based)
5. Z-level recovery (skip if in combat or hunting)
6. Self-defense (detect attackers, fight/flee/ignore)
7. Healing (if HP < 60%)
8. Vigilante PKer scan (IDLE/DWELLING only)
9. State dispatch:
   - IDLE: random PK roll, POI walking
   - DWELLING: chat, travel roll, dwell timer
   - TRAVELING: legacy recovery → IDLE
   - COMBAT/FLEEING: safety timeout (120s) + HP stalemate (5 min)
   - PK_ATTACK: chase and attack target
   - HUNTING: hunt waypoint navigation, monster scanning, combat
```

### Verified Lua APIs

| API | Purpose |
|-----|---------|
| `creature:setTarget(target/nil)` | Set attack target |
| `creature:setFollowCreature(target/nil)` | Set follow target (unreliable for bots) |
| `player:getPathTo(pos, min, max, full, clearSight, maxDist)` | A* pathfinding |
| `player:startAutoWalk(dirTable)` | Execute walk path |
| `player:isWalking()` | Check if auto-walking |
| `Tile(pos):hasFlag(TILESTATE_PROTECTIONZONE)` | PZ check |
| `Tile(pos):hasFlag(TILESTATE_FLOORCHANGE)` | Stair/ramp detection |
| `tile:getItems()` | Get items on tile |
| `item:transform(newId)` | Change item (open door) |
| `item:getId()` | Get item ID |
| `creature:getSkull()` | Get PK skull (0=none, 3=white, 4=red, 5=black) |
| `pos:isSightClear(tpos, true)` | Line of sight check |
| `player:setTown(Town(id))` | Change home town |
| `player:isBotPlayer()` | Check if bot player |
| `Game.getSpectators(pos, multifloor, onlyPlayers, rangeX, rangeX, rangeY, rangeY)` | Find nearby creatures |
| `player:getCastViewerCount()` | Count active cast viewers |
| `player:castSpell(spellName, target)` | Cast spell through real spell system |
| `target:addHealth(-dmg, combatType, attacker)` | Apply direct damage (PvP bypass) |
| `Tile:isRopeSpot()` | Check if tile is a rope hole |
| `Position:moveUpstairs()` | Calculate position one floor up (rope destination) |
| `MonsterType:getElementList()` | Get monster absorption map (table[combatType]=percent) |
| `Game.botGetState(guid)` | Get C++ bot AI state integer (-1 if not found, 0=INACTIVE..8=PARTY) |
| `Game.botInParty(guid)` | Check if bot is currently in a player party (bool) |
| `Game.botIsActive(guid)` | Check if bot is active in C++ engine — true if activated and in-world (used by `/cavebot reload` to detect active bots without staging position check) |
| `Game.botCommand(botName, command)` | Execute C++ bot engine command, returns result string |

### Cast Chat Debug Auto-Enable

When the canary binary is a debug build (`BOT_CONFIG.IS_DEBUG_BUILD = true`), Cast Chat debug
messages are automatically enabled when a viewer joins a bot's cast. Checked every 5 seconds
via `player:getCastViewerCount()`. Auto-disables when no viewers remain (unless manually enabled
via cavebot `log on` command).

---


</details>

<details>
<summary><strong>Server Admin Commands Reference</strong></summary>


Standard Canary/OTServer slash commands by access level. Useful for testing and debugging bots.
Source: [OTLand thread #193772](https://otland.net/threads/basic-commands-and-their-effects-from-tutor-to-god.193772/)

### Tutor / Senior Tutor (Access 1-2)

| Command | Syntax | Description |
|---------|--------|-------------|
| `info` | `/info <player>` | Show basic information about a player |
| `getonline` | `/getonline` | List all online players with levels and IPs |

### Gamemaster / Community Manager (Access 3-5)

| Command | Syntax | Description |
|---------|--------|-------------|
| `invisible` | `/invisible` or `/ghost` | Turn invisible to all lower-group characters |
| `goto` | `/goto <player>` | Teleport near target player (with portal effect) |
| `c` | `/c <player>` | Teleport target player to your position |
| `send` | `/send <player>,x,y,z` | Move a player to given coordinates |
| `a` | `/a <number>` | Move N squares straight ahead (facing direction) |
| `town` | `/town <townname>` | Teleport to a city's temple |
| `t` | `/t` | Return to your hometown temple |
| `t` | `/t <player>` | Move a player to their hometown temple |
| `up` | `/up` | Hop one floor up |
| `down` | `/down` | Hop one floor down |
| `B` | `/B <message>` | Broadcast red message to all online players |
| `bc` | `/bc <colour>,<message>` | Broadcast colored text to all players |
| `#b` | `#b <message>` | Turn text red on all public channels |
| `summon` | `/summon <creature>` | Summon a creature that follows you |
| `m` | `/m <creature>` | Spawn a wild (aggressive) creature next to you |
| `ban` | `/ban <player>` | Ban target player's character or account |
| `b` | `/b <player>` | Ban the IP address of target player |
| `kick` | `/kick <player>` | Kick player from the game |
| `notations` | `/notations <player>` | Show player's notation count |
| `i` | `/i <itemID>,<amount>` | Create an item by ID |
| `n` | `/n <itemname>,<amount>` | Create an item by name |
| `owner` | `/owner <player>` | Switch house/guildhall owner |
| `mc` | `/mc` | Show all characters connected from same IP |
| `gmoutfit` | `/gmoutfit` or `/look <looktype>` | Change to GM outfit appearance |
| `cliport` | `/cliport` | Click-teleport mode (teleport by clicking on screen) |
| `clean` | `/clean` | Clean items from the map floor |
| `pos` | `/pos` | Show your current map position |
| `pos` | `/pos x,y,z` | Teleport to specific map coordinates |

### God / Administrator (Access 6+)

| Command | Syntax | Description |
|---------|--------|-------------|
| `closeserver` | `/closeserver` | Close server to players (GMs+ stay online) |
| `openserver` | `/openserver` | Re-open server to players |
| `promote` | `/promote <player>` | Promote player to next group upward |
| `demote` | `/demote <player>` | Demote player to next group downward |
| `shutdown` | `/shutdown` | Shut down the entire server |
| `s` | `/s <NPCname>` | Summon a specific NPC next to you |
| `remove` | `/remove` | Destroy whatever is in the next square ahead |
| `giveexp` | `/giveexp <amount>,<player>` | Add experience points to a player |
| `addskill` | `/addskill <player>,<amount>,<skillID>` | Add skill points to a player |
| `attr` | `/attr` | Add statistics/attributes to weapons |
| `max` | `/max <number>` | Set maximum online player limit |
| `reload` | `/reload <target>` | Reload a specific server file/module |
| `z` | `/z <effectID>` | Show a magic effect on your character |


</details>

<details>
<summary><strong>Dispatcher Lag — CPUAffinity=1 fix (2026-05-21)</strong></summary>


Full investigation: the internal design notes

### Symptom
Recurring `[GAP_SLOW]` warnings at ~139/hour, with `body=0 vt=0 fne=0` — bot tick body itself fast (well under 150ms), but the gap between consecutive ticks reached 200-1700ms. Occasionally felt by players as game hitches.

### Root cause
On the 2-CPU LXC container, the bot system's baseline work (200 bot states + virtualTick every 5s + hibernation Lua loop every 300ms + network packet handling) pushes load avg to ~4.5 (2.3× capacity). The OS scheduler preempts canary's dispatcher thread when CPUs are saturated. The dispatcher's `pthread_cond_timedwait(40ms)` does NOT overshoot the timer itself — but after it fires, the thread can't get scheduled back onto a CPU for 100-1700ms because contention is high and canary is at `Nice=7`.

Confirmed via instrumentation: `[CYCLE_GAP] between=433ms intended=37ms` — dispatcher asked to wait 37ms but actually didn't run again for 433ms. With `intended=0ms` (pending tasks ready), gaps still hit 150-300ms — pure preemption.

### The fix
`systemd CPUAffinity=1` drop-in pins all canary threads to CPU 1 at fork time:

```ini
# /etc/systemd/system/canary.service.d/affinity.conf
[Service]
CPUAffinity=1
```

`systemctl daemon-reload && systemctl restart canary`. Verify: `taskset -p $(pgrep canary)` should return mask `2` (binary `10` = CPU index 1). All canary threads inherit this.

**Result**: GAP_SLOW rate dropped from ~139/hr to ~30-45/hr (~67-78% reduction). Persisted through daily restarts.

### Why pinning to one CPU helps even with 2 CPUs
- Pinning canary to CPU 1 leaves CPU 3 free for system services / LXC tenants / kworkers / IRQ handlers — those no longer compete with canary's scheduler queue.
- The dispatcher still competes with canary's *own* sibling threads on CPU 1 (BS::thread_pool workers, network handlers, DB tasks), which is why pinning gives a partial (not full) fix.

### Telemetry tags (kept in tree for future diagnosis)
All threshold-gated, zero steady-state cost. See the internal design notes for full semantics.

| Tag | Source file | Threshold | Meaning |
|---|---|---|---|
| `[GAP_SLOW]` | bot_engine.cpp | gap > 200ms | Bot tick to bot tick gap. Reports body/vt/fne/awake/hib phase breakdown. |
| `[TICK_SLOW]` | bot_engine.cpp | body > 150ms | Bot tick body slow. Same breakdown. |
| `[GE_SLOW]` | globalevent.cpp | 20ms | Single Lua globalevent's `onThink` slow. |
| `[DISP_SLOW]` | dispatcher.cpp | 20ms | Single dispatcher task slow (kind=serial/scheduled/cycle, name=context). |
| `[CYCLE_SLOW]` | dispatcher.cpp | 10ms | One dispatcher main-loop iteration's exec+sched+merge sum slow. |
| `[CYCLE_GAP]` | dispatcher.cpp | 150ms | Inter-iteration wall-clock gap (= wait_for + scheduler delay). Reports `between` (actual) and `intended` (what timeUntilNextScheduledTask returned). |
| `[HB_STALL]` | jitter_heartbeat.lua | 100ms | Lua dispatcher heartbeat delta from expected fire time. |

### Test toggles (default enabled in production)
Three Lua flags for future empirical isolation. Flip to `false` + restart canary to re-run an isolation test.

| File | Flag | Effect when false |
|---|---|---|
| `data/scripts/lib/bot_system.lua` | `BOT_CONFIG.MASTER_DISABLE` | Skips BotStartup / BotHibernation / BotMarket monitors. No bots load. |
| `data/scripts/globalevents/bot_market.lua` | `PASSES_ENABLED` | Skips SellerPass / BuyerPass / FulfillerPass. Monitor still fires. |
| `data/scripts/globalevents/bot_test_commands.lua` | `POLLER_ENABLED` | Skips 1s sync MySQL poll. Default `false` (we don't run external benchmarks). |

### Constraints we can't change from inside the LXC
- `CAP_SYS_NICE` is denied → can't `renice` to negative values, `Nice=-5` in systemd is silently ignored.
- `/proc/sys/kernel/sched_schedstats=0` and read-only → can't measure per-thread runqueue wait.
- `/sys/fs/cgroup/cpu.weight` read-only → can't boost cgroup CPU shares.

If the LXC host operator can grant `CAP_SYS_NICE`, raising priority to `Nice=-5` would close the remaining 47-77% of GAP_SLOW (eliminate dispatcher preemption from sibling threads). Without host cooperation, the next optimization avenues are code-side bot work reduction (e.g., throttle hibernation Lua loop cadence when no real players online).


</details>