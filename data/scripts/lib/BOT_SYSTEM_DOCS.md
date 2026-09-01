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

**Activity reroll** — when a bot finishes an activity it rolls the next one from
**TABLE A**, a percentage table that must sum to 100:

| Activity | Key | Default | What it does |
|---|---|---|---|
| Dwell | `botActivityDwell` | 12 | wander / stand around the current area |
| POI | `botActivityPoi` | 30 | walk to a point of interest (below) |
| Hunt | `botActivityHunt` | 10 | start an autonomous hunt |
| Party | `botActivityParty` | 2 | form or join a party hunt |
| Travel | `botActivityTravel` | 46 | travel to another city |

`botActivityRerollCooldownSec` (30s) throttles how often a bot re-rolls, and is
the single biggest lever on the whole system — it scales every activity's
absolute rate at once. A bin whose attempt fails (no eligible hunt, party cap
reached) takes a short `botActivityFallbackDwell{Min,Max}Sec` dwell instead of
donating its share to another activity.

**Points of interest (POI)** — depots, depot fronts, temples, boats, shops, NPCs,
water (fishing), houses, the Adventurer's Stone, and reward / imbuing shrines,
picked from **TABLE B** (`botPoi*`, also summing to 100) over whatever the bot's
current town actually has. `botPoiCrowdCapCount` / `botPoiCrowdCapRadius` stop a
single POI from getting overcrowded. A house visit then rolls **TABLE C**
(`botHouse*`) for what to do inside.

Full tables, defaults and the validation behaviour: **§13**. Live shares:
`/cavebot activity`. Dwell time at a POI/NPC after arrival is randomized
(`botDwellPoi*`, `botDwellNpc*`, `botDwellPostTravelSec`); the next reroll is
scheduled within `botDwellReroll{Min,Max}Sec`.

**Navigation realism** — bots do not walk like scripted NPCs:
- **Per-bot path jitter** (`botNavJitterMask`, 0-7, 0 = off) adds a tiny per-bot cost noise to
  pathfinding, so two bots walking the same route pick *different* tile-by-tile paths. Measured:
  routes go from **100% identical to ~0.1% identical** at only ~2.6% extra path length.
- **Walking lanes** (`botLaneEnable`) give each bot a persistent left / centre / right preference,
  so a shared street becomes a spread column instead of a conga line. A bot falls back to the
  centre automatically in narrow streets, on blocked or occupied tiles, and never stands on stairs
  or a teleport.
- **Route phase desync** (`botRoutePhaseDesync`) starts each bot at a *random point* in a patrol
  loop rather than the first waypoint, so bots sharing a hunt don't move as a lockstep cohort.
- **Multi-hop city routing** (`botNavGraphTowns`) chains authored routes (`A→X→B`) when no direct
  `A→B` route exists, so bots walk to places that previously required a teleport.
- **NPC approach tiles** — for every NPC the engine precomputes where a bot may legally stand to
  interact (walkable, in talk range, with line of sight). This is what lets bots trade with
  shopkeepers standing *behind counters*, from the same tile a real player would use.

**Random "alive" motions** (so bots don't look robotic):
- **Mid-walk dwell** (`botJitterDwellPct`) — stop, look around, then carry on.
- **U-turns** (`botJitterUturnPct`) — occasionally double back a few waypoints, as if the bot forgot
  something (once per bot per 10 min).
- **Changing their mind** (`botJitterRerollPct`) — abandon a city route part-way and pick a new
  destination.
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

**Force-logout on connection loss** (`forceLogoutOnConnectionLoss`) — a
**server-wide** policy (applies to real players too, not just bots): when a
client's connection dies — window closed, process killed, cable pulled — the
character is removed from the world **immediately** instead of lingering until
the `pzLocked` / in-fight timer runs out. Removal is identical to an admin
`/kick` (`removePlayer(true, true)`), so it bypasses the in-fight, pz-lock, and
NOLOGOUT-tile logout blocks. A *voluntary* logout (the client's logout button)
is untouched and still obeys the normal "may not logout during a fight" rule —
only a genuinely lost connection forces.
- Trigger is the connection being gone (`isDisconnected()` → `getIP() == 0`),
  checked once/second in `Player::sendPing()`. Bots never reach it — they have
  no client and `onThink` returns early before `sendPing`.
- Timing: **≤1s** for a clean socket close or crash; **~31s** for a silent link
  death (no FIN), bounded by the 30s TCP read timeout — that ceiling is global,
  not tunable from this feature.
- White-skull note: a **white skull is not persisted across _any_ logout** in
  stock canary (only red/black skulls are saved). So combat-logging while
  white-skulled drops the white skull — this is pre-existing behavior, **not**
  introduced here; the feature only removes the ~60s in-fight wait that
  previously blocked the logout.
- This is a **C++ key, not a `bot*` key**: default **off**, and it needs a full
  rebuild + restart to change — NOT `/cavebot reload` (see §13).

---

## 6. Hunting

Bots run real hunt scripts (waypoint routes + target monsters) loaded from
`data/bot/authored/hunt_scripts.csv` and `hunt_waypoints/<id>.csv`. A hunt
progresses through phases: **prepare** (depot + shops) → **travel to spawn** →
**patrol/kill** → **leave** → **resupply**, then may re-roll into another hunt.
One bot per physical spawn is enforced via a reservation system. Routes between
city POIs come from `city_routes.csv` + `city_route_waypoints/town_<id>.csv`.

> Since the BOT_CSV migration (2026-08-14) the authored data lives in git, not
> MySQL — the `bot_hunt_*` tables still exist but nothing reads them. See §6c for
> how to add a script, and §15 for what MySQL is still used for.

### 6a. Lure mode — gather a pack, then kill it

By default a bot stops and kills the first monster that comes into range, one at
a time. With lure mode armed it instead walks its patrol **holding fire** while
monsters aggro and trail it, and only turns and fights once enough have gathered.
Then the counter resets and it goes back to luring.

Armed when the hunt script sets `min_monsters` in
`data/bot/authored/hunt_scripts.csv` **and** the bot's level is at least
`min_level × botLureLevelFactorPct/100` (130% by default) — or unconditionally
when the bot is leading a **party hunt**, which uses `botLurePartyDefaultMin` if
the script itself leaves the column at 0. Never for quests or `traveling`
scripts. A party's supports need no special handling: they mirror the leader's
attacked-creature, so while the leader holds fire, so do they.

**Holding fire also switches off keep-distance.** Retreat lives inside
`chaseTarget`, which is only reachable when the bot has a target, so a luring
mage does not kite — it walks. That is why there are nine ways out of a lure:

| # | Trigger | Meaning |
|---|---|---|
| 1 | `pack_full` | reached `min_monsters` — the normal one |
| 2 | `hp_floor` | below `botLureHpFloorPct` — the main survival valve |
| 3 | `timeout` | `botLureMaxMs` elapsed; fight whatever gathered |
| 4 | `lap_wrap` | patrol wrapped past where the lure started |
| 5 | `blocked` | no movement for `botLureBlockedMs` — something is in the way |
| 6 | `support_*` / `member_hp` | aggro or damage landed on a party support |
| 7 | `shedding` | pack shrinking below its peak — we are losing them |
| 8 | `contact_stalled` | cornered while luring; fighting is how retreat comes back |
| 9 | `hunt_end_hold` | the hunt clock expired mid-lure (see below) |

The bot also **paces**: if the tail of the pack drifts past `botLurePaceDist` it
stands still to let them close — but never while something is already near, since
walking is the only spacing it has.

**Hunt-end hold.** A lure-armed bot is luring most of the time, so the hunt clock
usually expires mid-lure. Ending the hunt there would send the bot down its
authored return leg dragging a pack it will never target (the attack-all rule is
patrol-only). So the clock is held until the pack is down, bounded by
`botLureMaxMs`, skipped near the absolute safety ceiling.

Inspect with `/cavebot <bot> lure`; force it on one bot with
`/cavebot <bot> lure <n>` (`off` to clear). The heartbeat line shows
`[lure 3/5]` or `[engage 5 trig=1]`, and every engagement logs a
`LURE ENGAGE` line to the journal.

### 6b. Kite-backtrack — running the corridor instead of standing still

A keep-distance bot that runs out of room used to stand still and tank: both the
"cornered" case (no retreat path, no single step) and the 15-tile waypoint drift
cap ended in the same place. It now **retraces the patrol waypoints it already
walked** — ground it demonstrably crossed, so walkable and on the right floor —
ping-ponging over that stretch until the pack dies, then resuming the patrol at
the waypoint it is standing on.

Bounded by `botKiteDepthWps` waypoints, `botKiteMaxSpanTiles`, `botKiteMaxLegs`
reversals and `botKiteMaxMs`. The window stops at any floor change (kiting
through a ladder sheds the pack and can strand the bot), a direction is only
taken if it increases distance to the threat, and a burst that would pass through
the threat's keep-distance band is rejected.

Two behaviours are deliberately unavailable: kiting cannot start at patrol
waypoint 0 or at the lap boundary, because the already-walked stretch there
belongs to the previous lap. And while kiting, both target-abandonment checks in
`scanAndAttackMonster` are suppressed (damage is legitimately slow, and the
reachability probe runs from the fleeing position) — `botKiteCooldownMs`, armed
only on give-up exits, is what stops that suppression from being re-armed forever.

### 6c. Adding a hunt script

Authored bot data lives in **`data/bot/authored/`** as CSV, in git. It is not in
MySQL and not in `config.lua`, so **adding a hunt needs no rebuild and no server
restart** — commit, push, pull on the server, `/cavebot reload`.

**Tool:** `tools/cfg_importer/convert_cfg.py`, which reads OTC cavebot 1.3 hunt
`.cfg` files and writes the authored tree directly.

```bash
python tools/cfg_importer/convert_cfg.py                       # re-run the manifest
python tools/cfg_importer/convert_cfg.py --dry-run             # report + validate, write nothing
python tools/cfg_importer/convert_cfg.py path/to/NewHunt.cfg   # import one loose cfg
python tools/cfg_importer/convert_cfg.py --only Falcons.cfg    # one manifest entry
python tools/cfg_importer/convert_cfg.py --regenerate X.cfg    # lift a skip_reimport pin
```

You can also **drag a `.cfg` onto the script in Explorer**. It classifies the file,
infers name/level/town, writes the script, validates, adds a manifest row with
`enabled=1`, and copies the cfg into `tools/cfg_importer/cfg/` so the import is
reproducible from a clean clone. The console is held open so you can read the report.

**That `.cfg` directory holds three kinds of file** and the extension does not tell
them apart. Only the first is in scope here:

| kind | how to recognize it | destination |
|---|---|---|
| hunt script | has `label:travel_to` / `hunt_patrol` / `travel_from` | `hunt_scripts.csv` + `hunt_waypoints/<id>.csv` |
| city route | labels are `src-dst` pairs (`temple-depot`) | `city_routes.csv` + `city_route_waypoints/` — **no tool writes these yet** |
| POI route | no labels at all | as above |

Importing a route cfg as a hunt would *look* fine — every waypoint lands in
`hunt_patrol` — so the importer refuses kinds 2 and 3 by name.

**The manifest** (`tools/cfg_importer/cfg_manifest.csv`) is the per-script control
file: `town_id`, `min_level`/`max_level`, `vocation_mask`, `keep_distance_*`,
`min_monsters`, `enabled`, `script_category`. **Scalar edits belong here, not in
`hunt_scripts.csv`** — a re-import rewrites that row from the manifest. The cfg's own
`label:level:` line is only a default for a *new* import; it never overrides a value
already in the manifest.

**Waypoint edits do not survive a re-import.** If a script's waypoints were hand-fixed
after import, set `skip_reimport=1` — that freezes its waypoints and targets while
still syncing its scalars. Fourteen scripts (ids 2068–2081) carry this pin today: they
were imported before `b04c1f180` added the `travel_to[0]→stand` promotion and the
teleport-gap pre-pass, so re-importing would silently change routes that work.

**Five things the importer refuses**, because `/cavebot reload` destroys the engine
*before* parsing the CSVs — a malformed tree means **no bot activates at all**, with no
previous data to fall back on:

1. a cfg that is not a hunt script (above);
2. a waypoint type outside the engine's 16-value enum;
3. an empty `hunt_patrol` (or empty `travel_to` on a `traveling` script) — such a
   script loads clean and is then *never picked*, and `validate.py` does not catch it;
4. a duplicate `lower(name)` — the engine derives the 1-bot-per-spawn reservation key
   from the name, so two scripts sharing one would silently share a single reservation;
5. `is_quest` disagreeing with `script_category`.

For a **new** cfg two more apply, because it ships `enabled=1` and is live on the next
reload: the town must be placeable within 150 tiles of a temple/depot/boat POI
(`followWaypoints` aborts a route above 200 tiles, so a wrong town gives a script that
loads clean and fails every single run), and the cfg must state a level — without one
`min_level` would default to 1 against an uncapped ceiling, making it eligible for every
bot at every level and instantly the most-picked script on the server.

**Workflow.** The review moment is *before* push, since the script goes live on reload:

```
drop the cfg  →  read the report + git diff  →  commit + push
              →  pull on the server  →  /cavebot reload
```

Undo a local import with
`git checkout -- data/bot/authored && git clean -f data/bot/authored/hunt_waypoints`
— the checkout alone leaves a new script's untracked waypoint file behind as an orphan,
which the validator rejects.

Run `python tools/bot_csv/validate.py` after **any** hand-edit to the tree. The importer
runs it against a staged copy and publishes only on PASS, but hand-edits have no such gate.
Note that `/cavebot csv*` edits the *server's* copy in place, so a `git pull` can clobber
in-game edits.

**Levels — how the window actually works.** `min_level`/`max_level` gate which bots may
take a script, with **0 meaning no bound**. There is no fallback: a bot with no eligible
script simply does not hunt.

For a **party**, `effectiveLevel = leaderLevel × tolerance` is compared against
`min_level` **only** — `max_level` is compared against the raw leader level. So a party
lowers the floor and never lifts the ceiling.

| leader's base vocation | tolerance | rationale |
|---|---|---|
| EK | ×3 | tank + full support, the calibrated value |
| RP | ×2 | holds a front line, but no exeta and no EK HP pool |
| MS / ED | ×1.5 | no tank in the party at all |

Two fields look like they gate a hunt and **do not**: `vocation_mask` and `script_type`
are loaded and then consulted by nothing (`bot_hunt.cpp`: "level-only filter — vocation
and town are not filtered").

Be deliberate about `max_level`. Most imported scripts never had one authored — it was
derived as `min_level × 3` by the repo importers, and hardcoded to 9999 by the cfg
converter. The practical effect today is that a level-900+ bot has only **13** eligible
hunts out of 223, all of them uncapped `.cfg` imports; with one bot per spawn, at most 13
of the ~148 bots at that level can hunt at once. New scripts default to `max_level=0`
(uncapped), which is usually what you want for high-level content.

**Deleting a script** is a manual two-step: remove its row from `hunt_scripts.csv` **and**
its `hunt_waypoints/<id>.csv`. Doing one without the other is a hard validator error. The
id is retired, never reused.

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

## 11b. Ambient roam

Bots materialise on a vetted tile just outside a player's viewport, walk between random reachable
tiles near them — across floors, through the normal planner and floor-change machine — dwell at each
stop, and are hibernated back to where they came from when the player leaves.

This is the **supply** side of liveness. Hibernation-wake and proximity weighting are both *demand*
side: they react to where bots already happen to be, and do nothing when none are nearby. Roam puts
a cast on stage where the player actually is.

**It does not compete with organic wakes.** Roamers draw on `botRoamReserveSlots` (3) EXTRA density
slots per ring, and the organic arm of the cap subtracts them, so ordinary proximity wakes keep
their full budget. At the live config that means inner 3+3 and mid 10+3, of which at most 3 are
roamers.

There is **no new `BotAIState`** — a roamer is an ordinary IDLE/DWELLING bot to every other
subsystem, which is what gives it combat, chat, supply and hibernation for free. The session lives
in a per-guid side table like the fishing run and the house visit.

### Staying away from hunts

Two suppressions, both driven by the **claim command and nothing else**. A player hunting without
claiming is invisible to all of it, and `/cavebot release` clears it.

* A cast-watched bot in `HUNTING`/`PATROLLING` (or `PARTY`/`PATROLLING`) stops attracting roamers.
* A player who claimed a hunt and is **outside a town** stops attracting roamers, *and* stops the
  hunt selector steering bots toward them — which by default it actively does, since candidate
  spawns are weighted by proximity to anchors.

The flag is stamped on **every** claim invocation, before the resolution and cooldown checks, so a
spawn the engine does not know about still counts. Claims and the flag both last an hour, and both
are cleared by `release`, by logout, and by `/cavebot reload` (engine state — worth knowing when
testing).

### What counts as "in town"

Three rules, cheapest first; any one is enough. Check any tile with `/cavebot intown x,y,z`.

1. **Protection zone** — the game's own marker for safe ground.
2. **Temple plaza** — within `ROAM_TOWN_RADIUS` (25) of a town temple, on a floor compatible with
   that town's street level from `getCityWalkZ()` (above street outright, below only when PZ).
3. **NPC cluster** — two or more NPCs within 30 tiles on the same floor.

Rule 3 exists because the towns table cannot describe everywhere. Marapur is town 28 with a recorded
street level of z7, owns zero city-POI rows, and spreads across z2/z4/z5/z6 — rules 1-2 alone put a
player standing in its centre out in the wild. Rule 2's radius is deliberately small: at 60 it
called a player hunting 46 tiles from a temple, with no PZ and no NPCs, "in town".

### Telemetry

The 60s `[ROAM]` line carries sessions, legs, arrivals, gate reasons, release reasons and
`huntRepel: pts= eval= rejected=`. Read the last three together: `eval=0` while `pts>0` means the
repel gate is never consulted; `rejected=0` while `eval>0` means it is consulted but never bites.
Both are states this feature has genuinely been in.

Bot status text reads `AMBIENT ROAM - <task>` (walking to a tile, idle with the remaining dwell,
fighting a named monster, leaving, suspended) plus the leg number.

Full design and defect history: `implementation_plans/BOT_AMBIENT_ROAM.md`.

## 11c. City-route detour splicing

Authored city routes are frequently the naive concatenation of two legs joined at a POI.
`farmine|carpet~depot` *is* `carpet~temple` + `temple~depot`, tile for tile, so a bot walking from
the magic carpet to the depot went **down into the temple at z11 and straight back out** on every
trip. `roshamuul|boat~depot` has the same shape, and so does every runtime multi-hop chain, which
concatenates its legs at the shared POI.

Splicing removes that. At load, every authored route is scanned for a span that **leaves the floor
and returns to it**, and if the bot can simply walk from the waypoint before the excursion to the
waypoint after it, the interior is elided. Live: **78 of 1810 routes spliced, 248 waypoints
removed, in 0 ms.** Thais and Ankrahmun come out untouched.

### What may be elided

A window `W[i] … W[j]` is a candidate only if:

1. `W[i]` and `W[j]` are on the same floor and within 10 tiles (Chebyshev);
2. `W[i]`, `W[j]` **and** every interior waypoint is *plain* — `NODE` or `STAND`, `itemId == 0`,
   `extraData` empty. That protects `USE_WITH`/`LADDER`/`DOOR`/`NPC_INTERACT`/`TELEPORT` and the
   shrine-return and ice-fishing markers that ride `extraData`. The **anchors** are constrained too,
   not just the interior: otherwise a window ending at a `USE_WITH` could elide the positioning
   `STAND` right before it, which is a real shape in the Farmine carpet route.
3. at least one interior waypoint is on a **different floor** — the z-excursion rule. Without it the
   pass also "shortcuts" same-floor stretches, and measured over the authored data that meant 385
   splices instead of 199, straight-lining through Venore swamp and cutting corners through
   Ankrahmun blocks;
4. the elided span costs at least 8 more than the jump, counting a floor change as 8.

`W[0]` and `W[n-1]` can only ever be anchors, never interior, so a route always still starts at its
source and ends at its destination.

### The two map-backed gates

Both are **Player-free**, which is a requirement rather than a convenience: `loadCityRouteCore` runs
for hibernated bots too, so a gate needing a Player would splice for awake bots and not for
hibernated ones. It also rules out the real A\* kernel — `Map::canWalkTo` returns `nullptr` for a
null creature, so `botnav::findPath` with no creature calls every tile unwalkable.

* **reachability + savings** — an 8-connected BFS (`botSpliceStepDist`) returns the walk's *step
  count*, and the jump must be reachable and cheaper than what it replaces. The predicate is
  stricter than the ordinary flood: it rejects **any** door-table tile (open ones too) and **any**
  house tile. A spliced gap is walked by the plain-`goTo` branch, which never opens doors for NODE
  waypoints, so a splice across a door would stall the bot for 30 s per trip forever.
* **directness** — the walk may cost at most twice its straight-line distance, rejecting gaps that
  are reachable only by winding around.

Every rejection is conservative: the route is left exactly as authored.

**Line of sight was tried as a third gate and removed on measurement.** Audited across all 19 towns,
`isSightClear` rejected 16 windows that nothing else rejected — and all 16 looked walkable, several
with a walk cost *exactly* equal to their straight-line distance, one confirmed by `/cavebot route`
at 13 tiles. Meanwhile it *passed* 60 gaps that were not walkable at all. `BLOCKPROJECTILE` is the
protobuf `unsight` flag, so a parapet beside the line blocks sight while the walk steps around it:
sight and passage are different questions. It is still computed and printed by the audit command so
the evidence stays visible.

### Scope

Splicing happens inside `loadCityRouteCore`, the single choke point for city-route walking, so awake
and hibernated bots get an identical list. That covers **city routes, travel legs, and navigate/POI
walks**, including the hunt PREPARING phase's depot and shop walks, which are city routes that
happen to run during a hunt.

Hunt and quest **script** waypoints — `travel_to`, `patrol`, `travel_from` — are walked by
`followWaypoints` directly, never pass through `loadCityRouteCore`, and are untouched by
construction. Adventurer-stone routes are excluded too, and must stay excluded: that path resumes by
absolute index into its own list.

Authored pairs are precomputed at load; multi-hop chains are per-bot randomised, cannot be
enumerated, and splice once on first use. Nothing ever mutates the route graph itself — `/cavebot
csv` dumps it directly and its parity gate is byte-identical output.

### Tuning and rollback

The constants live at file scope in `bot_zgraph.cpp`, **not** in `config.lua`: there is no
deployment in which walking into the temple and back out is wanted. `BOT_SPLICE_ENABLED = false`
restores the previous behaviour exactly, at the cost of a rebuild and restart rather than a config
edit.

### Auditing

`/cavebot splice <townId>` splices a **copy** of every route in a town and reports what would change
and what was declined, naming the gate that declined each window. `/cavebot splice <townId> <src>
<dst>` runs the full resolution including the multi-hop fallback, so a chain splice spanning a join
POI can be checked. Both are read-only. In audit mode every gate is evaluated rather than
short-circuiting — a short-circuiting audit is what hid the line-of-sight problem on the first run.

`tools/bot_route_splice/report.py` gives the offline geometric candidate set (rules 1-4 only, no
map, so a strict superset), with `--route` for a before/after waypoint dump.

Telemetry: one `[SPLICE] precomputed …` inventory per boot, plus a periodic `[SPLICE] N route(s)
spliced` line — both unconditional, no verbose mode needed.

## 12. Command reference

### Global — `/cavebot <subcommand>` (no bot name)

**Population & lifecycle**

| Command | Purpose |
|---|---|
| `/cavebot active` | list currently active bots |
| `/cavebot population` | per-town bot count broken down by state |
| `/cavebot proximity` | proximity-weighting telemetry |
| `/cavebot schedule on\|off\|status` | enable/disable the time-of-day population scheduler, or show its current target |
| `/cavebot reload` | hot-reload the engine (`.so` + data), keeping the current active set |
| `/cavebot reload debug,<Bot Name>` | isolate ONE named bot — everything else stays hibernated with no tasks |
| `/cavebot reload debug,<Name A>;<Name B>` | isolate several, **semicolon**-separated |
| `/cavebot reload debug,N` | isolate N **arbitrary** bots (it picks — prefer the name form) |
| `/cavebot reload debug off` | leave debug mode — see the warning below |

> **`reload debug off` does not repopulate.** It clears the flag and re-activates whatever is still
> registered, but `BotStartup` — the globalevent that loads bots from the DB — only runs at server
> **start**. Leaving debug mode therefore leaves you with just the handful of bots that survived it
> (measured: 500 → 1). Plan a restart whenever you exit debug mode.

**Navigation diagnostics**

| Command | Purpose |
|---|---|
| `/cavebot zplan x,y,z x,y,z` | plan a route between two tiles without moving a bot — **z-hops only** |
| `/cavebot zplan x,y,z x,y,z -v` | same + per-leg `[ZLEGCOST]` trace in the journal |
| `/cavebot route x,y,z x,y,z` | the **complete tile-by-tile** route: every hop *and* every tile between them |
| `/cavebot route x,y,z` | same, from the first awake bot's position |
| `/cavebot route … wide` | trace with the **planner's** walker instead of the generic one (see below) |
| `/cavebot zgraph x,y,z` | portals near a tile, plus graph/blacklist totals |
| `/cavebot npcapproach <name>` | show the approach tiles derived for an NPC |
| `/cavebot tpscan x,y,z [r]` | read-only inspector for teleport / floor-change tiles |
| `/cavebot fishspots` | what the fishing-spot index actually holds |
| `/cavebot botcfg` | dump every navigation tunable **as actually loaded** from config.lua |
| `/cavebot cache` | per-cache hit rate and recompute cost |
| `/cavebot pathtest [N]` | compare the bot pathfinder against the engine's, N sample routes |
| `/cavebot pathbench [N]` | time server vs bot kernel vs kernel+jitter, µs per pathfinding call |
| `/cavebot dumpnav x1,y1,x2,y2,z1,z2` | dump a map region for the offline path simulator |
| `/cavebot roam` | ambient-roam inventory: live sessions, ledger, per-cluster counts, suppressed anchors |
| `/cavebot intown x,y,z` | why a tile counts as in-town (or not) — prints each rule's own verdict |
| `/cavebot roamanchor x,y,z` / `roamanchor off` | synthetic anchor, so roam is drivable with nobody logged in |

**Hunts, parties, claims**

| Command | Purpose |
|---|---|
| `/cavebot whohunts [search]` | which bot holds which hunt-script reservation |
| `/cavebot partyinfo` | active bot-party info |
| `/cavebot partystop <name>` | dissolve one bot's party hunt |
| `/cavebot claims` / `listclaims` | active player spawn-claims |
| `/cavebot clearclaim <name>` | admin force-release a player spawn-claim |
| `/cavebot claim [name]` | claim a spawn. **Always** flags you as hunting, even when no spawn matches |
| `/cavebot release` / `unclaim` | release your claim **and** clear the hunting flag |
| `/cavebot simulate route\|hunt\|poi …` | dry-run a route/hunt/POI without a live bot |

**Data editing** (DB-backed waypoint/POI tables)

| Command | Purpose |
|---|---|
| `/cavebot routewp\|routeadd\|routedel …` | inspect / add / delete city-route waypoints |
| `/cavebot huntwp\|huntadd\|huntdel …` | inspect / add / delete hunt waypoints |
| `/cavebot hunttarget\|targetadd\|targetdel …` | inspect / add / delete hunt target monsters |
| `/cavebot poi\|poiadd\|poidel\|poiupdate …` | inspect / add / delete / update POIs |

> House claiming is **not** a `/cavebot` subcommand — it moved to the standalone `/house`
> talkaction (`data/scripts/talkactions/player/bot_house.lua`).

### `zplan` vs `route`

`zplan` answers *"which floor transitions does the planner pick?"* — it prints hops and the
Chebyshev distance between them, nothing else. A route can look perfect in `zplan` and still be
unwalkable, because Chebyshev distance says nothing about whether a route exists.

`route` answers *"can the bot actually walk this, and by what path?"* It replays the real walker
leg by leg through the same pathfinder and the same node budget the live bot gets, and prints
every tile. It sees live doors, creatures and house flags, which the offline simulator cannot.

**Two walkers now exist, so the header line names which one was modelled:**

- default — `walker=generic goTo (chunked, 512 nodes, dist<=50)`. What ordinary bots use: a
  12-tile chunked search, re-pathed per chunk.
- `wide` — `walker=planner tier 2 (direct, 4096 nodes, dist<=200)`. What the scoped route planner
  issues: ONE direct search to the true destination, no chunking.

The default deliberately does **not** escalate to a wide search on a failed chunk. It used to,
mirroring an escalation `goTo()` no longer has — which made the diagnostic *over-report*
reachability, the exact diagnostic-vs-live divergence this command exists to eliminate. Use
`wide` explicitly to preview what the planner would find.

Read the per-leg annotations:

- `= N tiles OK` — the search carried the leg, the normal case.
- `*** NO PATH *** (stuck at (x,y,z) aiming for (x,y,z) [cheb=N])` plus `reached N tiles before
  stalling` — unreachable for this walker. A leg that walks 30 tiles then stops is a different bug
  from one that cannot take a single step; the annotation distinguishes them.
- `cheb=N > CHUNK_DIST=12 -> goTo() would aim at (x,y,z) which is REACHABLE/ALSO UNREACHABLE` —
  on a failed leg, prices the straight-line chunk target. This is what separates "the target is
  truly unreachable" from "chunking aimed into an obstacle and gave up on a route that exists".
  Suppressed in `wide` mode, which has no chunk target.

A large `route` sweep is not free — it briefly raises tick times. That is the diagnostic's own
cost, not steady-state load.

### Per-bot — `/cavebot <name> <subcommand>`

Quote names containing spaces: `/cavebot "Aldric Abunce" status`.

**State & lifecycle**

| Command | Purpose |
|---|---|
| `/cavebot <name> status` | full state report (state, pos, hp/mana, hunt, fc, travel) |
| `/cavebot <name> pos` | short position report |
| `/cavebot <name> info` | detailed state info |
| `/cavebot <name> routeinfo` | read-only dump of the bot's live navigation state |
| `/cavebot <name> stop` | halt everything — also grants a 5-min no-reroll cooldown |
| `/cavebot <name> resume` | resume normal AI from IDLE (cancels `stop`) |
| `/cavebot <name> wake` | wake a hibernated bot — **required before any other per-bot command** |
| `/cavebot <name> hibernate` | force hibernation |
| `/cavebot <name> active [x,y,z]` | force-activate an inactive bot (at a tile, or at its temple) |
| `/cavebot <name> inactive` | force-deactivate (true offline — removed from the world) |
| `/cavebot <name> pin on\|off\|status` | stop the bot self-assigning hunts/POIs (survives rerolls; no expiry) |
| `/cavebot <name> roam status` | this bot's roam session — phase, leg, destination, home, fail streak |
| `/cavebot <name> roam end` | end its roam session (releases it back to normal AI) |
| `/cavebot <name> roamregion` | the reachable region roam would use from this bot's nearest anchor |

**Movement**

| Command | Purpose |
|---|---|
| `/cavebot <name> goto x,y,z` | walk the bot to a tile (uses the full nav stack) |
| `/cavebot <name> teleport x,y,z` | place the bot on a tile instantly |
| `/cavebot <name> navigate <poi>` | walk to a POI in the current city |
| `/cavebot <name> sequence <poi1,poi2,…>` | multi-leg navigation |
| `/cavebot <name> travel <city> [wp#]` | inter-city travel, or teleport to a route waypoint |
| `/cavebot <name> stairs up\|down` | force a floor change |
| `/cavebot <name> step <dir>` | force a single step |
| `/cavebot <name> poi` | detect the nearest POI |
| `/cavebot <name> routes` | list available travel destinations |

> **Before a manual `goto`, run `stop` first.** The planner branch is gated on
> `!followingCityRoute`, so a bot mid-city-route silently never reaches it, and the autonomous
> reroll will otherwise steal the bot mid-walk. `stop` clears both.

**Hunting & parties**

| Command | Purpose |
|---|---|
| `/cavebot <name> hunt [name\|id]` | start a hunt (named/id, or auto-pick) |
| `/cavebot <name> endhunt` | force hunt-time expiry so PATROLLING moves to LEAVING |
| `/cavebot <name> debug_kills <N>` | per-bot hunt kill limit (0 = time-based only) |
| `/cavebot <name> partyhunt [scriptId]` | force an EK bot to start a party hunt |
| `/cavebot <name> advstone [chest\|dummy [wepId]\|wp]` | manually start an Adventurer's Stone trip |

**Forced behaviours** (skip the random roll + pacing gates, so a behaviour can be exercised on
demand — everything that would make the action *illegal* still applies, and the reason comes back
in the reply rather than failing silently)

| Command | Purpose |
|---|---|
| `/cavebot <name> eat` | force one food-eating attempt |
| `/cavebot <name> potion [health]` | force a potion drink (defaults to mana) |
| `/cavebot <name> rune` | force one rune conjure |
| `/cavebot <name> support` | force one ambient support cast (still refuses while hunting — by design) |
| `/cavebot <name> supportlist` | the whole support-spell pool and why each entry is/isn't castable now |
| `/cavebot <name> fish` | force a fishing trip |
| `/cavebot <name> fishice [waypoint]` | ice-fish beside the bot, or at the nearest `fish:` waypoint (alias: `icefish`) |
| `/cavebot <name> idleclock` | report the idle clock the **unforced** rune gate reads (needs 10s idle or fishing) |

**Inspection & world editing**

| Command | Purpose |
|---|---|
| `/cavebot <name> scan [radius]` | scan for floor-change tiles |
| `/cavebot <name> scandoors [radius]` | scan nearby tiles for closed doors |
| `/cavebot <name> tileinfo x,y,z` | dump a tile's items and flags |
| `/cavebot <name> use x,y,z` | use the item at a position (sewer/ladder interaction tests) |
| `/cavebot <name> placeitem <id> <x,y,z>` | place an item for testing |
| `/cavebot <name> removeitem <id> <x,y,z>` | remove an item for testing |
| `/cavebot <name> pk <target>` | force a PK attack |

**Logging & debug stream**

| Command | Purpose |
|---|---|
| `/cavebot <name> verbose on\|off` | per-bot castLog stream to the journal (alias: `log on\|off`) |
| `/cavebot <name> debug on\|off\|status` | per-bot debug stream |
| `/cavebot <name> debug grid on\|off` | ASCII surroundings grid |
| `/cavebot <name> debug events on\|off` | `[BOT:EVT]` event log |
| `/cavebot <name> debug snapshot <ms>` | snapshot interval |

> `verbose on` is usually the right tool, **not** `reload debug,<name>`: it puts one bot's
> `PLAN:`/`PLEG:`/`ZLEG:`/`IDLE-NAV:`/`FC_VERIFY:` lines into the journal without despawning the
> other 499 or requiring a restart to undo.

#### Isolating one bot to observe it

```
/cavebot reload debug,Aldric Abunce     -- only this bot runs; the rest stay hibernated, no tasks
/cavebot Aldric Abunce pin on           -- stop it self-assigning work over your test
/cavebot Aldric Abunce verbose on
/cavebot Aldric Abunce teleport 32345,32265,5
/cavebot Aldric Abunce goto 32350,32225,5
```

#### Running commands headless (no game client)

Every command above can be queued through the `bot_commands` MySQL table. The manager drains it on
a 10s interval and writes the reply back into the row's `result` column (and to the journal):

```sql
INSERT INTO bot_commands (bot_name, command) VALUES ('Aldric Abunce', 'status');
INSERT INTO bot_commands (bot_name, command) VALUES ('_global', 'botcfg');
INSERT INTO bot_commands (bot_name, command) VALUES ('_global', 'reload');
```

Use `_global` as the bot name for the global subcommands. Poll `processed = 1` to know a row is
done — `result` is an empty string, **not** NULL, while pending, so `COALESCE(result,'')` is not a
usable completion test.

> **This path bypasses the talkaction's Lua allowlist**, which the in-game `/cavebot` goes through.
> A global subcommand can therefore work perfectly here while being unreachable in game — exactly
> how `/cavebot route` shipped broken. When adding a global subcommand to the engine, also add it
> to `navGlobals` in `data/scripts/talkactions/god/bot_cavebot.lua`, and verify it **in game**, not
> only through the queue.

#### Driving a bot from Cast Chat (cast operator)

`/cavebot` also works typed into the **Cast Chat** channel while you spectate a bot, but it needs a
password — `@cast` login is otherwise unauthenticated, so anyone watching would inherit bot control.

1. Set `castOperatorPassword` in `config.lua` (requires `authType = "password"`, which is the
   default; the secret rides the login password field). Restart — it is a core-binary config key.
   **The web login must forward it.** Protocol 13+/OTClient logs in through MyAAC's `login.php`,
   not the game server's port 7171, and that endpoint builds the session key itself. It has to
   emit `"@cast\n" . ($request->password ?? "")` — an empty tail silently disables operator mode
   for every viewer with no error anywhere. Source of truth is `deployment/web/login.php`; copy it
   to `/var/www/html/login.php` (the live web root is not a git repo, so this edit is otherwise
   unversioned). The legacy 7171 path already does this at `protocollogin.cpp:43`.
2. Log in with account **`@cast`** (or `@livestream`), that password, and the **bot's name** as the
   character.
3. You get *"Operator session: type /cavebot &lt;command&gt; here…"* in Cast Chat. Now type normally:

```
/cavebot "Keira Elsoutchawnc" wake
/cavebot "Keira Elsoutchawnc" travel thais
```

What to expect:
- **Only `/cavebot` is dispatched**, and only when the character you are watching is a bot.
  Any other `/command` is forwarded as ordinary chat, exactly as before.
- **The watched bot is the acting player.** `/cavebot routeadd|poiadd` with no coordinates records
  the tile *the bot you are watching* is standing on — usually what you want. By the same token
  `/cavebot simulate …` walks that bot through the waypoint list.
- **Output is visible to every viewer** of that bot, not just you (`Player::sendTextMessage` fans
  out to all cast viewers).
- A wrong password silently leaves you an ordinary read-only viewer. Five failures from one IP
  within 5 minutes locks operator elevation from that IP for 15 minutes.
- Every command is audited to the journal as `[Cast][OP]`.

A *human* broadcasting their own character still cannot use Cast Chat commands — `parseSay`
short-circuits caster `CHANNEL_CAST` text before it reaches the talkaction dispatcher. That is a
separate gap and is not covered here.

Two things that bite:
- **A hibernated bot rejects every command** with *"Use '/cavebot &lt;name&gt; wake' first."* After a plain
  `/cavebot reload` with no players online, most bots hibernate within seconds.
- **`/cavebot reload` can crash the server** (known, unfixed): walk tasks scheduled from inside the
  `.so` can fire after `dlclose` has unmapped their code — `botStartAutoWalk`'s lambda in
  `Dispatcher::executeScheduledEvents`, SIGSEGV. It is a race, so it succeeds most of the time and
  is likeliest when bots are actively walking. `systemctl restart canary` has no such race; prefer
  it when the timing matters.

### Player-facing
| Command | Purpose |
|---|---|
| `/cavebot claim [name]` | reserve the hunt spawn you're standing in (kicks the bot hunting it, ~1h). **Also flags you as hunting even if no spawn matches** — while flagged and outside a town, ambient bots stay away and the hunt selector stops steering bots toward you |
| `/cavebot release` | release the spawn **and** clear the hunting flag |
| `/party <vocs> [min,max] [teleport]` | summon bots into your party. Uncapped size; `[100,1500]` sets an explicit level range; `teleport` forces the old instant assembly instead of walking them in |
| `/party leave` | dismiss your party bots (each rolls its next activity from where it stands) |
| *(click-invite)* | inviting a bot from the game client works too — it accepts after 1.5-4.5s unless it is in a bot party hunt or mid-fight |
| `/cast on` / `/cast off` | start / stop broadcasting your character |
| `/house "<name>" owner` | claim a free bot house (no house + can pay rent) |
| `/house "<name>" sub-owner` | become sub-owner of a bot house |
| `/house "<name>" release` | release a house you claimed (bot reclaims it) |

---

## 13. Configuration (`config.lua`)

All `bot*` keys live in `config.lua` and are read at script load. **Changing a
value** is hot: `/cavebot _global reloadconfig` re-reads the file with no rebuild
and no restart. **Adding or renaming** a key needs a rebuild.

### The decision weights — what a bot does next

Everything deciding **what a bot does next** is three percentage tables in the
`BOT ACTIVITY PERCENTAGES` block of `config.lua`. **Tables A and B must each sum
to 100.** If one does not, the server still boots and uses the numbers exactly as
written, but logs an error and stamps `TABLE A/B INVALID` on `/cavebot botcfg`
and on the 5-minute reroll summary. Nothing is silently rescaled.

**TABLE A — which activity to start.** Sampled every `botActivityRerollCooldownSec`
(30s) while a bot is idle. This cooldown is the single biggest lever in the block:
it scales every activity's absolute rate at once.

| Key | Default | Activity |
|---|---|---|
| `botActivityDwell` | 12 | stand around / micro-idle |
| `botActivityPoi` | 30 | walk to a point of interest (→ TABLE B) |
| `botActivityHunt` | 10 | take a hunt script |
| `botActivityParty` | 2 | form or join a party hunt |
| `botActivityTravel` | 46 | travel to another city |

`botActivityParty` is deliberately tiny: parties run 2-3 hours and occupy
`botPartyMaxPct` of the population, so a bigger number here creates *refused
attempts*, not more parties. Raise `botPartyMaxPct` instead. A bin whose attempt
fails takes a short `botActivityFallbackDwell{Min,Max}Sec` dwell rather than
donating its share to a neighbour.

**TABLE B — which POI, once TABLE A picks POI.** Rolled over what the bot's
**current town actually has**, so these are shares of the available candidates,
not guaranteed rates. A *hibernated* bot cannot reach `npc`/`water`/`house`/
`shrine` (awake-only), so its roll covers the other seven rows renormalized.

| Key | Default | Key | Default |
|---|---|---|---|
| `botPoiDepot` | 19 | `botPoiWater` (fishing) | 14 |
| `botPoiDepotOutside` | 9 | `botPoiHouse` (→ TABLE C) | 13 |
| `botPoiTemple` | 5 | `botPoiAdvStone` | 6 |
| `botPoiBoat` | 8 | `botPoiRewardShrine` | 4 |
| `botPoiShop` | 5 | `botPoiImbuingShrine` | 4 |
| `botPoiNpc` | 13 | | |

**TABLE C — what to do once inside a house.** `botPoiHouse` only gets the bot
through the door. Rolled over what that *house* actually has — most have no
dummy and no hireling — so idle's realised share runs well above its number.

| Key | Default | Sub-activity |
|---|---|---|
| `botHouseIdle` | 26 | sit/stand around |
| `botHouseHireling` | 20 | interact with a hireling |
| `botHouseDummy` | 20 | train on a dummy |
| `botHouseLocker` | 19 | use the depot locker |
| `botHouseShrine` | 15 | use a house shrine (~19% of bot houses have one) |

Run **`/cavebot activity`** to see nominal, eligible-now and realised shares side
by side, split by awake vs hibernated — that is the authoritative view, and it is
why no derived "effective share" arithmetic is written into these tables.

### Other groups

- **Population:** `botPlayersOnline`, `botPlayersShowAsOnline`
- **Liveness / density:** `botDensityCapEnabled`, `botDensityAnchorClusterRadius`,
  `botDensityCap{Inner,Mid,Outer}Radius`, `botDensityCap{Inner,Mid,Outer}LimitPct`
- **Ambient roam:** `botRoamEnable`, `botRoamReserveSlots`, `botRoamTargetPerCluster`,
  `botRoamMaxTotal`, `botRoamRadius`, `botRoamDwell{Min,Max}Ms`,
  `botRoamSessionMaxMs`, `botRoamReleaseTiles` — see §11b
- **POI crowding:** `botPoiCrowdCapCount`, `botPoiCrowdCapRadius`
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
- **Navigation realism:** `botNavJitterMask` (0-7), `botLaneEnable`, `botLaneRecoveryWps`,
  `botRoutePhaseDesync`, `botNavGraphTowns` (`""` = off / `"all"` / csv of town ids),
  `botJunctionSwitchPct`
- **Human jitter:** `botJitterDwellPct`, `botJitterDwellMin/MaxMs`, `botJitterUturnPct`,
  `botJitterRerollPct`, `botJitterWindowPct`
- **Load protection:** `botAwakeBudgetEnable`, `botAwakePathfindPerTick{Mid,High}`,
  `botAwakeRotatePct`, `botAwakeRotateWindowSec` — under dispatcher load, caps how many
  *unobserved* awake bots run their AI per tick. Bots a player can actually see are **never**
  throttled.
- **Telemetry:** `botTelemetryEnabled` (default **off** — see §14)

> All bot configuration lives in `config.lua`. There is deliberately no second config file or
> override layer — changing a tunable needs a server restart, and that is the accepted trade for a
> single source of truth.

> **Non-bot server key:** `forceLogoutOnConnectionLoss` (default `false`) also lives in `config.lua`
> but is **not** a `bot*` key — it is read in C++, applies to **all** players, and needs a full
> rebuild + restart (it is **not** hot-reloadable via `/cavebot reload`). It force-kicks a character
> the instant its connection dies, bypassing the anti-combat-log block. See §5.

See `config.lua.dist` for inline comments and defaults on every key.

---

## 14. Debug & telemetry

- **Debug mode:** `/cavebot reload debug,<Bot Name>` isolates that ONE bot (everything else stays
  hibernated with no tasks) and streams verbose `[BOT:DBG]`/`[BOT:EVT]` telemetry plus a live ASCII
  heartbeat grid to the server log (`journalctl -u canary` on Linux). `debug,N` takes N arbitrary
  bots instead; `debug off` returns to the full population. See §12 for the full sequence and the
  hibernation/reload gotchas.
- **Chat console logging:** `botChatVerboseLog = true` logs every chat line.
- **DB telemetry:** `botTelemetryEnabled` (default **false**) gates best-effort
  writes to `bot_chat_emissions` (offline dup-rate measurement only — never read
  by runtime logic). Leave off in production.
- **Perf telemetry:** set the environment variable `BOT_PERF_TELEMETRY=1` to log
  dispatcher/jitter timing. Off unless explicitly set.

---

## 15. Data sources — files vs. database

**Authored bot data is in git, not MySQL.** Since the BOT_CSV migration
(2026-08-14) the engine loads `data/bot/authored/*.csv` — hunt scripts and their
waypoints, city routes, POIs, equipment, town mapping, travel positions. See §6c
for how to add to it and `tools/bot_csv/validate.py` to check it.

This distribution ships **no** MySQL copy of that data and no legacy tables for
it — the CSVs are the only source of truth. A query against `bot_hunt_scripts`
will fail with `ERROR 1146 … doesn't exist`; that is expected.

> Want SQL for ad-hoc surveying? Load the CSVs into a *scratch* database with
> `tools/bot_csv/import_to_mysql.py --db canary_scratch --create` and query that.
> Keep it scratch: MySQL is downstream of the files, never upstream.

**Live MySQL tables**, all still written at runtime:

| Table | What |
|---|---|
| `bot_hunt_script_stats` | `successful_hunts` / `total_kills` per script — generated telemetry, split out of the authored table by BOT_CSV. See EXTENDED §"Hunt Success Tracking" |
| `bot_nav_events` | navigation-failure counters (stuck, teleport fallbacks) |
| `bot_market_item_prices` | market price reference |
| `bot_active_players`, `bot_state_persistence`, `bot_startup_config` | runtime population + persisted state |
| `bot_commands`, `bot_test_commands` | the admin command queue |
| `bot_house_origin`, `bot_house_origin_items` | house-reclaim layout snapshots |
| `bot_chat_emissions`, `bot_hub_presence_60s` | telemetry |

Plus `cast_broadcasters` for the cast system. Bot characters live in the stock
`accounts` (id `65000`) and `players` tables.

To re-seed the bot population at a different size/name-set, see
`tools/bot_population_generator/`.
