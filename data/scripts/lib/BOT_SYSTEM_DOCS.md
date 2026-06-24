# Bot Player System — Reference

Autonomous "bot players" that populate the world: they hunt, travel between
cities, chat, trade on the market, own houses, form parties, and defend
themselves — so a low-population server feels alive. A companion **Cast** system
lets anyone spectate a live character (including bots) without an account.

This is a concise operator/user reference. For day-to-day setup see the repo
`README.md`; this file covers behavior, commands, and configuration in full.

---

## 1. Architecture (why it's built this way)

- **C++ engine in a hot-reloadable shared library.** The bot AI lives in
  `libbot_engine.so` (built from `src/creatures/players/bot/bot_engine.cpp`) and
  is loaded via `dlopen` at runtime, separate from the main `canary` binary.
  - **Why C++, not Lua:** the engine ticks up to hundreds of bots every 100 ms
    (pathfinding, spectator scans, combat decisions, chat). The original Lua
    prototype became a CPU/GC bottleneck at that scale; native C++ removed the
    lag spikes. A thin Lua layer (`data/scripts/lib/bot_system.lua` and friends)
    still handles orchestration and data loading.
  - **Why a separate `.so`:** it rebuilds in ~30 s and can be swapped live, so
    iteration doesn't require a full server rebuild or kicking players.
- **`/cavebot reload`** (god command) hot-swaps the engine with no restart:
  deactivate bots → unload old `.so` → load new `.so` → reload hunt + chat data →
  re-register bot players → re-activate the ones that were active. It also
  re-reads the chat corpus and the `bot*` tuning keys.
- Bots are real `Player` rows (account id `65000`), loaded by the server at
  startup — not fake creatures. They appear on the map, in battle lists, on the
  website online list, and can be spectated.

---

## 2. Population & how many are online

- The database seeds ~**997** bot characters across the game's cities, spread by
  level and vocation. You do **not** run all of them.
- **`botPlayersOnline`** (config.lua, default **500**) sets how many load at
  startup, drawn evenly across the level/vocation/town spread. Range **0 → 997**.
  Set `0` to disable bots entirely.
- **`botPlayersShowAsOnline`** — whether bots (awake or hibernated) count toward
  the displayed online list / player count.
- Loading is **stratified** (evenly sampled), not "first N", so any
  `botPlayersOnline` value yields a representative level/vocation/town mix.

---

## 3. Personalities & autonomous behavior

Each bot has a lightweight **personality** that biases how often it idles,
lingers, pauses mid-walk, and re-rolls its next activity. (`botPersonalityReroll
OnRestart` re-rolls personalities on restart when true.)

**Activity reroll** — when a bot finishes an activity it rolls the next one with
weighted odds:

| Activity | Key | What it does |
|---|---|---|
| Idle | `botRerollWeightIdle` | wander / stand around the current area |
| POI | `botRerollWeightPoi` | walk to a point of interest (below) |
| Hunt | `botRerollWeightHunt` | start an autonomous hunt |
| Travel | `botRerollWeightTravel` | take a boat to another city |

`botRerollCooldownSec` throttles how often a bot re-rolls.

**Points of interest (POI)** — depots, depot fronts, temples, boats, shops,
NPCs, and the Adventurer's Stone, each with a relative weight
(`botPoiWeight*`). `botPoiCrowdCapCount` / `botPoiCrowdCapRadius` stop a single
POI from getting overcrowded. Dwell time at a POI/NPC after arrival is randomized
(`botDwellPoi*`, `botDwellNpc*`, `botDwellPostTravelSec`); the next reroll is
scheduled within `botDwellReroll{Min,Max}Sec`.

**Random "alive" motions** (so bots don't look robotic):
- **Mid-walk pauses** — `botWalkPause*` (a small chance per route to stop briefly).
  When a real player / cast viewer is watching, longer, more human pauses kick in
  (`botWalkPauseObserved*`, up to ~20 s).
- **Turn in place** — `botTurnInPlace*`.
- **Fidget item drops** — see §9.
- **Mounts** — `botMountChancePct` chance a bot is mounted.
- **PZ roam** — `botPzRoam*` lets idle bots wander protection zones.

**Travel** is boat-based: bots walk to the boat NPC, sail to another city, and
resume activities there (no teleport in normal play).

---

## 4. Liveness — awake vs. hibernate

To keep CPU flat with many bots, bots that no one can see **hibernate** (frozen,
out of the tick loop) and **wake** on demand.

- **Wake triggers:** a real player or cast viewer comes near, a viewer clicks the
  bot in the cast list, or someone PMs the bot. Waking can include an off-screen
  walk-in / login "sparkle" so it looks natural.
- **Density cap** (`botDensityCapEnabled`, default on) limits how many bots wake
  near a player so crowds don't balloon. Real players and cast-watched bots form
  an anchor cluster (`botDensityAnchorClusterRadius`); per cluster the inner /
  mid / outer rings (`botDensityCap{Inner,Mid,Outer}Radius`) cap wakes at a
  **percentage of `botPlayersOnline`** (`...LimitPct`). Party cascades are exempt.
  Set `botDensityCapEnabled = false` to wake freely.
- **Proximity weighting** biases hibernated bots' next task/location toward where
  real players are, so the world fills in around players rather than emptily.

---

## 5. Combat, PvP & gang raids

**Self-defense** when attacked:
- Normal: ~50% fight back, ~50% flee.
- Outleveling the attacker (bot ≥ 2× attacker level, or damage < 5% max HP):
  mostly ignore (~17% fight), often after a token hit, then resume.

**Vigilante** — on first sight of a player-killer, a per-PKer 5% chance the bot
decides to attack them.

**Random PK** — a small chance a bot turns aggressor against an eligible target,
going through the proper PvP pipeline (skulls, PZ, level limits).

**Gang raids** (`botGangEnable`) — bots can band into a roaming gang that
ambushes targets:
- `botGangTargetPlayers` (target real players), `botGangMin/MaxSize`,
  `botGangRecruitRadius`, `botGangVictimBand` (level band of valid victims),
  `botGangStageWindowMs` / `botGangScanCooldownMs` (timing),
  `botGangVictimCooldownSec` (per-victim cooldown so the same person isn't farmed).
- Odds are tunable separately vs. players (`botGangOddsVsPlayer`) and vs. other
  bots (`botGangOddsVsBot`); higher value = lower chance.
- `botGangWallChancePct`, `botGangParalyzeChancePct` add wall/paralyze tactics.
- `botGangRequireObserver` only stages raids when someone can witness them.

---

## 6. Hunting

Bots run real hunt scripts (waypoint routes + target monsters) loaded from the
`bot_hunt_*` tables. A hunt progresses through phases: **prepare** (depot +
shops) → **travel to spawn** → **patrol/kill** → **leave** → **resupply**, then
may re-roll into another hunt. One bot per physical spawn is enforced via a
reservation system. Routes between city POIs come from `bot_city_routes`.

---

## 7. Chat

Bots talk using a large template corpus (`data/bot_chat/phrases.json`).

- **Banter** (local say) is **observer-gated**: it only fires when a real player
  or cast viewer is within view, and is throttled per nearby player so bots don't
  spam. `botChatCooldown*`, `botChatMasterChancePct`.
- **Advertising** runs on the trade channel (`botAdvertisingInterval*`).
- **Keyword replies** — a nearby/idle bot answers when a player says or PMs a
  recognized phrase (price/trade/greeting), after a short "typing" delay; PMs work
  even for hibernated bots. `botHibernatedChatEnabled` gates channel chat for
  hibernated bots.
- **Anti-repeat / throttle** is entirely in-memory (`botChatAntiRepeatRingSize` +
  time-windowed cross-bot dedup) — no two bots repeat the same line in a window.
- `botChatVerboseLog` logs each emission to the journal (debugging).

---

## 8. Market

Bots keep the player market lively from the `bot_market_*` data (priced from
gold-NPC shop items, currency-aware):

- They **create auctions and bids**, **cancel** stale ones, and **accept**
  matching offers, at realistic prices.
- Funded from a bot bank pool so offers are real and fillable by players.

---

## 9. Equipment, forge & item drops

- **Equipment** is assigned per **level + vocation** (`bot_equipment`): bots
  spawn wearing level-appropriate gear for their class.
- **Forge tiers** are applied up to a cap (tier 6), and gear carries
  **imbuements**, so bots look and fight like geared players.
- Bots keep an active **dwarven ring** equipped (decay frozen for bots).
- **Item drops (fidget)** — occasionally a bot drops a low-value item near itself
  to leave "litter" that makes areas feel used. Controlled by `botFidgetChancePct`,
  `botFidgetInterval*`, and `botFidgetMaxItemValueGp` (only cheap items; scattered
  to a nearby reachable tile).

---

## 10. Houses

- Bots own houses and furnish them; layouts are snapshotted so they can be
  restored exactly.
- Real players can take over a bot house with the player-facing command (§12):
  claim ownership or sub-ownership. When a claimed/sub-owned house is later
  released or vacated, a globalevent (`BotHouseReclaim`) returns it to its bot and
  restores the original furniture verbatim.

---

## 11. Cast (spectator) system

- A character broadcasts with **`/cast on`** (and `/cast off`). Bots broadcast
  automatically.
- Spectators connect with the special account name **`@cast`** (no password) and
  pick a live character from the list — including hibernated bots, which **wake on
  click**. Works through the MyAAC `login.php` web login (protocol 13+/OTClient).
- Viewers are numbered and read-only; the caster sees viewer chat in a dedicated
  color.

---

## 12. Command reference

### God / admin — `/cavebot <subcommand>`
| Command | Purpose |
|---|---|
| `/cavebot active` | list currently active bots |
| `/cavebot population` | population / online breakdown |
| `/cavebot proximity` | proximity-weighting telemetry |
| `/cavebot claims` | list bot house claims |
| `/cavebot partyinfo` | active bot-party info |
| `/cavebot reload` | hot-reload the engine (`.so` + data) |
| `/cavebot reload debug,N` | reload into debug mode with N bots + telemetry |
| `/cavebot <name> pause\|continue\|stop` | control a single bot |
| `/cavebot <name> route\|hunt\|poi …` | inspect/record routes & waypoints |

### Player-facing
| Command | Purpose |
|---|---|
| `/cavebot claim` | reserve the hunt spawn you're standing in (kicks the bot hunting it, reserves ~1h) |
| `/cavebot release` | release a hunt spawn you claimed |
| `/party` | summon nearby bots to form/join your party |
| `/cast on` / `/cast off` | start / stop broadcasting your character |
| `/house "<name>" owner` | claim a free bot house (no house + can pay rent) |
| `/house "<name>" sub-owner` | become sub-owner of a bot house |
| `/house "<name>" release` | release a house you claimed (bot reclaims it) |

---

## 13. Configuration (`config.lua`)

All `bot*` keys are read at script load and are **hot-reloadable** via
`/cavebot reload` (no rebuild). Grouped:

- **Population:** `botPlayersOnline`, `botPlayersShowAsOnline`
- **Liveness / density:** `botDensityCapEnabled`, `botDensityAnchorClusterRadius`,
  `botDensityCap{Inner,Mid,Outer}Radius`, `botDensityCap{Inner,Mid,Outer}LimitPct`
- **Activity reroll:** `botRerollWeight{Idle,Poi,Hunt,Travel}`, `botRerollCooldownSec`
- **POI:** `botPoiWeight{Depot,DepotOutside,Temple,Boat,Shop,Npc,AdventurerStone}`,
  `botPoiCrowdCapCount`, `botPoiCrowdCapRadius`
- **Dwell:** `botDwellReroll{Min,Max}Sec`, `botDwellPoi{Min,Max}Sec`,
  `botDwellNpc{Min,Max}Sec`, `botDwellPostTravelSec`
- **Adventurer's Stone:** `botAdvStoneDwell{Idle,Chest,Dummy}{Min,Max}Sec`,
  `botAdvStoneChestDummyCapPct`
- **Motion / "alive":** `botWalkPause{ChancePct,MinMs,MaxMs,MaxPerRoute}`,
  `botWalkPauseObserved{ChancePct,MinMs,MaxMs,MaxPerRoute}`,
  `botTurnInPlace{ChancePct,IntervalTicks}`, `botMountChancePct`,
  `botPzRoam{Enable,IntervalMinSec,IntervalMaxSec,StayPct}`
- **Fidget drops:** `botFidgetChancePct`, `botFidgetInterval{Min,Max}Sec`,
  `botFidgetMaxItemValueGp`
- **Chat:** `botChatCooldown{Min,Max}Ms`, `botChatMasterChancePct`,
  `botChatAntiRepeatRingSize`, `botAdvertisingInterval{Min,Max}Ms`,
  `botWorldChatInterval{Min,Max}Ms`, `botHibernatedChatEnabled`, `botChatVerboseLog`
- **Gang raids:** `botGangEnable`, `botGangRequireObserver`, `botGangTargetPlayers`,
  `botGangMinSize`, `botGangMaxSize`, `botGangRecruitRadius`, `botGangVictimBand`,
  `botGangStageWindowMs`, `botGangScanCooldownMs`, `botGangVictimCooldownSec`,
  `botGangOddsVsPlayer`, `botGangOddsVsBot`, `botGangWallChancePct`,
  `botGangParalyzeChancePct`
- **Personality:** `botPersonalityRerollOnRestart`
- **Telemetry:** `botTelemetryEnabled` (default **off** — see §14)

See `config.lua.dist` for inline comments and defaults on every key.

---

## 14. Debug & telemetry

- **Debug mode:** `/cavebot reload debug,N` loads N bots with verbose
  `[BOT:DBG]`/`[BOT:EVT]` telemetry and a live ASCII heartbeat grid in the server
  log (`journalctl -u canary` on Linux).
- **Chat console logging:** `botChatVerboseLog = true` logs every chat line.
- **DB telemetry:** `botTelemetryEnabled` (default **false**) gates best-effort
  writes to `bot_chat_emissions` (offline dup-rate measurement only — never read
  by runtime logic). Leave off in production.
- **Perf telemetry:** set the environment variable `BOT_PERF_TELEMETRY=1` to log
  dispatcher/jitter timing. Off unless explicitly set.

---

## 15. Database tables

Created by migrations on first boot (and by `database/bots/00_bot_schema.sql` for
data import): `bot_hunt_scripts`, `bot_hunt_waypoints`, `bot_hunt_targets`,
`bot_hunt_fields`, `bot_hunt_exclusion_zones`, `bot_city_routes`,
`bot_city_route_waypoints`, `bot_market_item_prices`, `bot_equipment`,
`bot_town_mapping`, `bot_active_players`, `bot_chat_emissions` (telemetry),
`bot_hub_presence_60s` (telemetry), plus `cast_broadcasters` for the cast system.
Bot characters live in the stock `accounts` (id `65000`) and `players` tables.

To re-seed the bot population at a different size/name-set, see
`tools/bot_population_generator/`.
