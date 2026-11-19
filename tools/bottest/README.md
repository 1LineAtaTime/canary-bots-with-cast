# tools/bottest — bot test & observability harness (Phases 1-2)

Design: `implementation_plans/BOT_TEST_HARNESS_PLAN.md` (REVISION 1 is authoritative).
A real OTClient (Redemption) plays as a normal player and as `@cast` operator cameras on bots,
driven by `*.steps` scenarios; the server journal is tailed and joined to what the client saw.

## Pieces
| file | role |
|---|---|
| `runner.py` | runs a `*.steps` scenario: clients, verbs, expects, exit codes, run dir, result.md |
| `watch.py` | tokenless anomaly watcher (started by the runner) -> `runs/<id>/.watch` |
| `hub.py` | loopback WebSocket hub (127.0.0.1:8765), clock tracker, `.cam` stripping; `hub.py phase0` = Phase 0 proof |
| `merge.py` | joins `cli-*.ndjson` with the journal and the server audit -> `server.ndjson`, `audit.pc.ndjson`, `latency.json` (incl. audit kinds + census + perception), `timeline.md` |
| `setup_client.py` | builds the harness OTClient copy from the installed client |
| `winproc.py` | `popen_kw()`: every harness subprocess (ssh.exe, mysql.exe, python, git, OTClient) is created with `CREATE_NO_WINDOW` + explicit stdin, so no console window ever pops up |
| `otc_overlay/edits.json` | the harness edits to the stock client (init.lua, mods.otmod), applied by setup_client.py; includes audio OFF from the first frame |
| `otc_mod/harness_ctl/` | OTClient mod: `ctl.lua` streams client events, executes hub verbs, hides the window; `perceive.lua` = perception parity (effects, missiles, texts, creatures, own UI state) |
| `scenarios/*.steps` | scenarios (`smoke_login_see`, `walk_observe_thais`, `hidden_parity`, `audit_probe_thais`, `audit_wake_walk`, `audit_func_check` (deploy smoke: both control paths + file sanity), `audit_wake_sparkle` (B2 wake -> login-sparkle sample), `perceive_self`, `perceive_combat`, `*_demo` contract checks) |
| `baseline_metrics.py` / `compare.py` | Phase 3 lean baseline: per-scenario metrics JSON from a run dir (`baselines/<date>/B#.json`), and a per-metric old/new/delta diff of two baseline dirs |
| `test_watch.py` | unit tests: CRASH/SLOW detectors, watcher end-to-end on a fake server, steps parser, audit parsing/filters/census |
| `harness.json` | non-secret config (server, ports, client dir, player chars, watch thresholds, `audit_path`) |
| `harness.local.json` | **secrets, gitignored** — copy `harness.local.json.example` |

## One-time setup
1. `python tools/bottest/setup_client.py` builds `~/Documents/otc-harness/` from
   `C:\Program Files (x86)\TibiaOTClient-Redemption` (read only; never modified): exe + runtime
   files, `data/` with only `things/1500`, `modules/`, `mods/` without `game_bot` (it dials an
   external BotServer), the `otc_overlay` edits (no network asset installer; per-instance write dir
   `%APPDATA%\otc-harness\otch-<name>\`), and `mods/harness_ctl`. `--force` rebuilds,
   `--dest DIR` builds elsewhere, `--mod-only` just re-installs `harness_ctl` after editing ctl.lua.
   No exe/DLL/asset is ever stored in this directory (CipSoft assets must not be committed).
2. `pip install websockets`.
3. `harness.local.json` from the example (from a git worktree, `harness.local.json` and
   `.claude/secrets.local` fall back to the MAIN checkout's copies; never copy them into a worktree). Harness character: account 70001 `harnessacct`, char
   `Harness One` (guid 70001, group 1 = non-GM). Cast operator password = server `castOperatorPassword`
   (5 wrong tries lock the IP for 15 min: copy it, never guess).

## Run a scenario
```
python tools/bottest/runner.py tools/bottest/scenarios/smoke_login_see.steps [--run-id ID] [--label L] [--visible]
```
Exit codes: **0** all verbs OK and expects pass, **3** a verb returned `STUCK verb=<v> reason=<r> pos=x,y,z`
(the run stops there), **4** an expect failed (`FAIL check=... got=... want...`), **2** infrastructure.
Clients run **hidden** (no window; `--visible` or `# VISIBLE: true` to debug). The runner always logs
everything out and terminates only the OTClient PIDs it launched.

### From Claude Code: background run + Monitor on `.watch`
1. Start the run in the background with a known id:
   `python tools/bottest/runner.py tools/bottest/scenarios/walk_observe_thais.steps --run-id wo_1`
   (Bash tool, `run_in_background: true`).
2. Follow `runs/wo_1/.watch` with the Monitor tool (e.g. until a line contains `DONE`). It only gets a
   line on an anomaly or an end marker, so it costs nothing while the run is healthy:
   `WATCH-START`, `CRASH`, `ABORT?`, `LOGSTALL`, `SLOW`, `CLIENTDOWN`, `RUNFAIL`, `RECOVERED`, then
   `RESULT-READY` -> `RUN-ENDED` -> `DONE`. `WATCH-STOPPED` if you create `runs/wo_1/.stop`,
   `WATCH-EXPIRED` at max age (default 120 min).
3. On DONE read `runs/wo_1/result.md` (first row `| Result | PASS/STUCK/FAIL/INFRA | exit=N |`), then
   `run.log` / `timeline.md` for detail. React to `CRASH` / `CLIENTDOWN` immediately; do not wait out
   timeouts.

All server access by watch.py is read-only (`systemctl show`, `journalctl --after-cursor` over one
persistent ssh session); it never starts/stops/signals canary.

## Run dir (`runs/<runId>/`, gitignored)
`run.log` (step + verdict lines, ends `DONE`), `.watch`, `result.md`, `run.json` (git sha, botcfg sha256,
clock samples, clients, vars, final DB position), the steps file copy, `hub.ndjson`, `cli-<name>.ndjson`,
`server.live.log` (live tail used by expects), `server.log` + `server.ndjson` + `timeline.md` +
`latency.json` (merge.py), `audit.live.ndjson` (live audit tail) + `audit.ndjson` (pulled window, server
clock) + `audit.pc.ndjson` (PC clock), `records/*.cam` (packet records, login packet stripped), `shots/*.png`,
`view-*.txt` (dump-view ASCII), `view-*.json` (perception `view`), `self-*.json` (`self`), `cmd-*.txt`
(`/cavebot` replies), `botcfg.txt`, `clients.json`.

## Steps format
```
# SCENARIO: name   SEED: 0   CAMERAS: auto   BOTCFG: auto|off   VISIBLE: false
@p1 login                                   player actor -> harness.json "players"
@p1 goto 32367,32240,7 range=2 stuck=10 timeout=120
@p1 wait-see any-bot moving=1 not=$B1 timeout=60 as=B2      sets $B2 and $B2_cid
@cam:$B2 login                              @cast operator camera on that bot (client c1, c2, ...)
@srv cmd "$B2" status expect="state="       in-band via an operator camera, else bot_commands queue
@srv expect src=cli:p1 k=see_move bot=$B2 min=1 within=120
@srv expect join=move cli=p1 bot=any min=1 within=120 window=100
@srv expect-none src=srv msg~="Lua Script Error" within=300
@p1 logout
```
`;` separates verbs on one line, `# ...` is a comment, `$VAR`/`${VAR}` substitute captured values,
`optional=1` turns a STUCK into SKIP.

**Client verbs** (`@p<n>`, most also on `@cam:<Bot>`): `login [char]`, `logout`,
`goto x,y,z [range=] [stuck=10] [timeout=120]`, `walk <dir> [n]`, `face <dir>`, `stop`, `follow <name>`,
`say <text>` (camera: cast chat), `pm <name> <text>`,
`wait-see <name>|any-bot|any-player|any-monster|any-npc [timeout=] [moving=1] [not=a,b] [as=VAR]`,
`wait-gone <name> [timeout=]`, `hold <secs> [watch=a,b]`, `dump-view`, `shot [label] [mode=window|map] [reveal=1]`,
`set-fps <n>`, `window-state [expect=hidden|visible]`, `lagmark [n]` (camera only), `sleep <s>`, `note <text>`,
`client-state [expect=re]` (audio, hidden, fps), perception: `view [recent=ms] [expect=re]`, `self [as=S] [expect=re]`, `perceive [poll=] [self_poll=] [recent=]
[tap=on|off] [creatures=on|off] [self=on|off] [reset=1] [expect=re]`, `open [slot]` (default back),
`use <itemId> [with=self|<name>|none]`.
`walk-check [within=S] [end=x,y,z] [end-range=N] [max-revisits=N] [max-osc=N] [min-steps=N] [max-steps=N] [z-seq=7,6]`
(runner-side, on the client's own `self_pos` stream; on a camera that is the bot's own walk): steps, tiles revisited,
A->B->A reversals and the floor sequence of the LAST walk (a jump > 1 tile = teleport starts a new walk) -> `walk-N.json`.
PvP / social (Phase 3): `attack <name>|stop` (safe-fight off, chase on), `party invite <name>|leave`,
`trade <name> [slot=1]|cancel` (client events `trade_own` / `trade_counter` / `trade_close`). `login` on a dead
character leaves the death window and logs in again (`relogin_after_death`).
**Server verbs** (`@srv`): `cmd "<bot>|_global" <subcommand> [expect=regex] [via=cam|queue] [as=VAR]`,
`audit <sub...>` (= `cmd _global audit <sub...>`, see below), `botcfg`, `find-bot [since=900] [not=a,b] [as=VAR]`
(read-only journal scan: most recent bot whose last hunt-phase transition entered PATROLLING and has not
logged `[HUNT:STUCK]` since), `expect ...`, `expect-none ...`. `runner.py --var NAME=VALUE` pre-sets `$NAME`.

goto plans client-side paths over the known map and re-plans whenever the walk stalls: it waits out an
occupied exact destination (`range=0`), uses any free tile within `range` otherwise, walks to the known
tile nearest a far destination and re-plans, and approaches an unwalkable destination then reports it.
STUCK reasons: `dest-unwalkable`, `blocked-by-creature`, `no-path`, `no-progress`, `timeout`,
`other-floor` (same-floor only in Phase 1), `offline`.

**Expect filters**: `src=` (glob: `cli:p1`, `cli:*`, `srv`), `k=` (client kinds `see_appear see_move
see_disappear see_talk self_pos stat login_ok`, perception `see_effect see_missile see_text see_msg
see_creature see_self see_cooldown see_sound see_effect_end tap_error`; server kinds `wake roam_arrived roam_leg route
login logout cast_* lagmark` or the lowercased `[TAG]`), `bot=<name>|any` (name, or creature id: client
cid, server `guid=N` -> cid = 0x10000000 + guid, or a bot name resolved through what the client saw),
any `field=value`, `field~=regex` (`text~=` also matches server `msg`). `within=S` looks back S seconds
(default: whole run); `wait=S` retries (default 5 s, journal lines arrive up to ~3 s late); `min=`/`max=`.
`join=move`: client `see_move` events matched to a journal line about the same creature within
`window=` ms (default 100; `pos=1` also requires the same tile).

## Server audit (Phase 2)
Server truth comes from the engine audit stream, not from journal greps: one synchronous NDJSON file
written by the main binary (`botAuditPath`, default `logs/bot_audit.ndjson`, rotated to `.1` past
`botAuditMaxMb`), every record stamped with the server's epoch ms. Off by default
(`botAuditEnabled = false`); toggled at runtime in memory (survives `/cavebot reload`, resets on
restart):
```
/cavebot audit on|off|status|config|flush        config = re-apply config.lua botAudit*
/cavebot audit watch <bot>|watch *|watch clear   * = every bot, LOW-RATE kinds only (below)
/cavebot audit unwatch <bot>
/cavebot audit kinds all|probe,state,wake,hib,walkpause,path,pathout,decide,cmd,census
/cavebot audit mark <text>                       free-text marker record
```
`watch *` covers every bot but only for the low-rate kinds wake, hib, state, decide and pathout
`osc`/`stuck`/`abandoned`. `probe`, `path`, `walkpause` and pathout `arrived` are per-tick or
per-route and need an explicitly NAMED bot (a `*` with them would be hundreds of KB/s of synchronous
writes on the dispatcher); `*` state records carry `reason=unlogged` (the decision-line capture is
named-only too). Five consecutive writes slower than 50 ms auto-disable the audit (one warning in
the journal, `AUTO-DISABLED` in `status`; `/cavebot audit on` re-arms); a failed open is retried at
most every 5 s.

Envelope: `{"v":1,"ts":<epoch ms>,"seq":n,"src":"srv","k":"<kind>","guid":..,"cid":<0x10000000+guid>,
"bot":"Name","pos":[x,y,z],"st":"IDLE","hib":0|1, ...}` (census/cmd/audit/mark have no bot fields).

| kind | when | extra fields |
|---|---|---|
| `audit` | settings changed | kinds, watch, watchResolved, gen (also `dropped` if the buffer overflowed) |
| `probe` | every `botAuditProbeMs` per NAMED watched bot | ph, fc, tgt, atk, walk, pathLen, awake, obs, cast, hpPct, live, tickUs, hunt/wp/kills, routeIdx/routeLen, travel, quiet, rerollInMs, dwellMs, pausing, lastAct(+AgeMs) |
| `state` | state or hunt phase changed (sampled each 100 ms tick, live AND hibernated) | from, to, pfrom, pto, reason (last decision log line within 2 s, else `virtual-sim`/`unlogged`), detect=tick-diff |
| `wake` | wake granted / density-gated / first AI tick | result ok\|gated, path prox\|burst\|explicit, visible, sparkle, fast, landing, moved, quietTicks, anchors, cascade; `event=first_act firstActMs` |
| `hib` | hibernated | result ok\|deactivated, pool, cascade, anchors |
| `walkpause` | mid-walk pause scheduled | tier observed\|unobserved, ms, walkBias, nThisRoute, steps |
| `path` | a search ran (goTo, goToWide, zPlanFullRoute) | planner astar\|astar-chunk\|astar-offset\|jitter\|wide\|zplan, src, dst, arrival, result ok\|fail\|fail-cached\|deferred\|abort\|backoff (zplan: pair still inside its abort backoff, #116), len, expanded (zplan), ms |
| `pathout` | route end / waypoint skip / oscillation | outcome arrived\|stuck\|abandoned\|osc, route (log prefix), wp, wps, reason, val; osc: period 2\|3, returns, a, b |
| `decide` | reroll / POI / hunt pick (live AND virtual twin) | what, chosen, n (candidates), detail; reroll: roll, total, w{dwell,poi,hunt,party,travel}, rej, walk/poi/hunt/travelTo/dwellMs |
| `cmd` | any Game.botCommand | source talkaction\|cast-op\|queue\|lua, bot, command, ok (best effort), reply (first line) |
| `census` | every `botAuditCensusSec`, ALL bots | reg, active, awake, hibn, cast, paused, state{}, phase{[awake,hib]}, town{[awake,hib]}, vhunt/lhunt{n,patrolling,kills,wpPct}, anchors, realPlayers, wake*60s |

The runner tails the file live (`src=srv:audit`, PC clock) and pulls the run window at the end
(read-only `cat | awk` over ssh). Use `src=srv:audit` with kinds that also exist as journal kinds
(`wake`): `src=srv` alone is the journal. Examples:
```
@srv audit watch "$B1" ; audit on expect="enabled=on"
@srv expect src=srv:audit k=walkpause bot=$B1 tier=observed min=1 within=300
@srv expect-none src=srv:audit k=walkpause st=HUNTING within=300
@srv expect src=srv:audit k=wake result=ok path=prox min=1 within=600
```
result.md gets a "Server audit" section: per-kind counts, wake -> p1 see_appear latency, and the
census summary (awake/hib/cast p50/min/max, state means, hunt phases, virtual-hunt progress, towns).
Cost: off = one generation poll per tick + one mask test per hook; on = only watched bots + the
census. Run `/perf-gate` after deploying a build that contains it (audit off vs the previous build).

## Perception (perceive.lua, measured 2026-11-18)
Goal: the hidden client perceives what a human player sees, without rebuilding OTClient.

**Sources.** Lua has no getter for tile effects (`Tile::m_effects`), floor missiles
(`Map::getFloorMissiles`) or animated/static texts (`Map::getAnimatedTexts/getStaticTexts`) - none is
bound in `src/client/luafunctions.cpp` - so a tile poller is impossible. Instead `perceive.lua` wraps
gamelib's `ProtocolGame.onOpcode`, which `ProtocolGame::parseMessage` calls for EVERY server opcode
before the C++ parser and whose read position it restores when Lua returns false
(`protocolgameparse.cpp:65-71`). The tap parses 0x83 (effects + distance missiles + sounds, mirroring
`parseMagicEffect` byte for byte), 0x84 (>= 1320: effect removal), 0x85 (old-protocol missile) and
0xB4 (damage/heal/mana/exp numbers = the animated texts) and always returns false, so the C++ parse
and drawing are untouched. Every packet is seen at parse time (no sampling, no lifetime race).
Creature/self state comes from the client's own Lua signals (exact timing), plus a 100 ms poller for
direction and walking (no turn signal exists) and a 500 ms self poll (regeneration, cooldown expiry).

| event | fields (besides cms/ems) | source |
|---|---|---|
| `see_effect` | id, x,y,z, at, dx,dy,dz (vs own pos), cid/name of the creature on the tile, source, phases, life_est_ms (phases x 75 ms), rep_ms (same id+tile seen < 2 s ago: the client may skip drawing it, `Tile::addThing` waitFor) | tap 0x83 |
| `see_missile` | id, from, to, x/y/z + tx/ty/tz, dist, life_ms (= 150 ms x sqrt(len), `Missile::setPath`), cid/name (shooter tile), to_cid/to_name | tap 0x83 types 4/5, 0x85 |
| `see_sound` / `see_effect_end` | sound, which, source / id, at | tap 0x83 types 6/7, 0x84 |
| `see_text kind=anim` | text, value, color, mode (DamageDealed/DamageReceived/DamageOthers/Heal/HealOthers/Mana/Exp/ExpOthers), part (physical/magic), at, cid/name | tap 0xB4 |
| `see_text kind=static` | text, name (speaker), mode, level, at | `g_game.onTalk` with a map position (the speech bubbles) |
| `see_msg` | mode, mode_name, text (status, loot, look, combat log, `/cavebot` replies) | `g_game.onTextMessage` (was `see_text` before) |
| `see_creature` | cid, name, ctype (player/monster/npc/summon), bot, why, changed, new values + d_<k> / was_<k> for hp, skull, shield, emblem, ctype_id, master, speed, outfit, dead, invisible; why=dir / walk from the poller | Creature signals + poller |
| `see_appear` | now also skull, shield, emblem, speed, outfit, dir, master, walking, ctype_id | |
| `see_self` | why (signals coalesced per packet batch, or `poll`), changed, new values + d_<k> / was_<k>: hp/hpmax, mp/mpmax, cap (free; total flaps per packet in >= 1281), soul, stamina, level/levelpct/exp, mlvl, speed, manashield, states + cond (pz, swords, pzblock, haste, poison, ...), sk_*/skp_*, inv_<slot> (id[xcount][:c charges][:t tier]; duration only in `self`), cont_<id> (name\|items), attack, follow, fight/chase/pvp/safe, cd/cdg/cdm (active cooldowns) | LocalPlayer / g_game / Container signals + poll |
| `see_cooldown` | kind spell/group/multiuse, id, ms | `g_game.onSpellCooldown` etc. |

`view` returns every creature on all known floors with the full creature state, the own state, open
containers and the tapped events of the last `recent` ms (`view-N.json`); `self` returns the own UI
state (`self-N.json`, reply line `hp= mp= cap= ... cond= cd= bags=[...]`, `as=S` sets `$S_<field>`);
`perceive` returns counters (taps per opcode, tap errors, events per kind). `open` opens the backpack
(a closed container's contents are unknown to a human too). merge.py adds `perception` to
latency.json and result.md: per-client counts, id histograms, `say` -> own see_effect latency,
cross-client agreement (effects one client saw inside both clients' views, matched by id+tile
+-500 ms in the other) and audit `wake` -> see_effect on the landing tile (the B2 sparkle).
A camera's LocalPlayer is the watched bot, so a camera's `see_self` is that bot's HP/mana/cooldowns.

## Clock and joins
PC<->LXC offset re-sampled every 5 min over one persistent ssh session (min-RTT, ~1.3 ms uncertainty),
interpolated per event. Windows: +-100 ms journal, +-50 ms lagmark. The offset moves (Phase 0: -969 ms,
1 h later: -1006 ms), so never reuse an old offset. Note: `[ROAM] arrived` is logged on the tick AFTER
the step, so the client usually sees the step 20-50 ms before that line (negative dt).

## Hidden mode (measured 2026-11-18)
OTClient's `run()` shows the window after init.lua/mods, so the mod moves it off-screen during load
and hides it in `g_app.onRun`; Win32 confirms 0 visible windows (`window-state`). `SW_HIDE` does not
stop rendering (only minimise does), so behaviour is identical: lagmark RTT p50 7-12 ms hidden vs 8 ms
visible, `/cavebot` reply 5-15 ms vs 7-13 ms, every verb OK in both (`hidden_parity.steps`).
Screenshots while hidden: `mode=window` (back buffer) and `mode=map` (map FBO) both gave live,
non-black frames on this machine; reading a hidden window's back buffer is not guaranteed by GL
(pixel ownership), so if a driver returns black use `mode=map` or `reveal=1` (shows the window
~1 s on-screen, captures, hides). `shot` reports `nonzero_rgb` and flags `BLACK`.

## No console windows, no audio
- Windows: all spawns go through `winproc.popen_kw()` (`CREATE_NO_WINDOW`, explicit stdin). The
  watcher used to be started `DETACHED_PROCESS`, i.e. with NO console, so every `ssh.exe` it ran
  allocated a new visible console; it now gets a hidden console its children inherit. Measured
  2026-11-18 with a 100 ms EnumWindows sampler over perceive_cross x2 + smoke_login_see: 0 new
  console windows (ConsoleWindowClass / CASCADIA_HOSTING_WINDOW_CLASS). OTClient is a GUI-subsystem
  exe; its own window may be "visible" for ~100 ms during load, parked at (-32000,-32000).
- Audio: the overlay sets `enableAudio`/`enableMusicSound` false and `musicSoundVolume` 0 in the
  per-instance settings right after corelib loads and calls `g_sounds.setAudioEnabled(false)` before
  `modules/client` can start the login music; harness_ctl re-asserts it (options, all channels
  disabled + gain 0, stopAll) at load, 1 s later and on every game start. Every `login` records
  `audio_enabled=` in run.log / clients.json; `client-state` shows the full state.

## Known limits
- Perception gaps that need an OTClient C++ change (issue #111): whether an effect was actually DRAWN
  (`Tile::addThing` may drop a same-id effect, tile.cpp:254-264; `m_effects` unbound), drawn lifetime
  (Animator total duration unbound; `life_est_ms` is phases x 75 ms), light levels, attached effects /
  particles / shaders (onAttach only), and a true null-renderer `--headless` build.
- goto is same-floor only; stairs/ladders/authored waypoint replay come later.
- Cameras are observers: they wake the bot they watch (castViewerLogin).
- Only one harness account/character exists (`@p1`).
- `[BOT:EVT]` lines now end with ` bot='Name' guid=N ems=<epoch ms>` (Phase 2 build onward).
