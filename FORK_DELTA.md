# FORK_DELTA.md: every way this repository deviates from upstream canary

Rebuilt for the 15.25 port (#128, #157) from `implementation_plans/upstream_1525/FORK_DELTA_INVENTORY.md`.

- **Upstream base:** `666a60f6f` (upstream/main 2026-11-05, CLIENT_VERSION 1525, SERVER_RELEASE_VERSION 3.6.1),
  merged into `port/upstream-1525` in three real merges (M1 `017ca62e1`, M2 `f01b4f906`, M3 `666a60f6f`).
- **The rule:** after every `git merge upstream/...`, a conflicted or semantically touched file MUST have a row here.
  `tools/fork_delta_check.sh` fails when an upstream file the fork modifies has no row (rule 2), and when a file
  under `data-otservbr-global/migrations/` does not exist upstream (rule 1).
- **New upstream file:** take theirs. **File listed here:** resolve per its note, keeping the fork hook.
  **Everything else** should not conflict.
- **Runbook for the next sync:** PORT_PLAN.md section 11.

## Rules that are not per-file

1. **No fork-owned numbered migrations.** The bot schema lives in `database/bots/*.sql` (idempotent), so
   `db_version` tracks upstream only (#150). A database last used by the 15.00 bot server is reconciled once with
   `database/bots/91_upstream_1525_schema.sql`.
2. **Binary and `.so` are always rebuilt together.** Upstream inserts config keys mid-enum on every sync, which
   shifts every BOT_* ordinal. The #58 ABI tag hashes every `src/*.hpp` + the toolchain, so the loader refuses a
   `.so` built against another header set. `.so` reload is not a deploy tool: deploy is stop + copy + start.
3. **Lua sees bot config keys by name** (`config_functions.cpp`, #154), not through magic_enum, whose range stops
   at ordinal 500. A new BOT_* table must be added to `BOT_ALL_CONFIG_KEY_TABLES` (bot_config_keys.hpp), and every
   `configKeys.BOT_*` read by Lua must be in the self-check list at the top of `data/scripts/lib/bot_system.lua`.
4. **Bots never enter upstream's LivestreamManager.** Cameras on bots use our cast path ("bot camera"); human
   casters use upstream's livestream. Our human `/cast` is optional (#163, cast C2): `allowPlayerCast`
   (config.lua, default false) gates it in ONE place, `Player::canCastBroadcast()` (bots always pass). With it off
   `setCastBroadcasting(true)` refuses a human, so no human `cast_broadcasters` row and no human fanout ever exist;
   `Player::onThink` stops a human cast still running after a reload; the `@cast` list and routing skip humans.
5. **Cast fanout survives every `Player::send*` rewrite.** Put `forwardToCastViewers` before any new `!client`
   return; keep the `castViewers.empty()` early-outs; a new `send*` that a camera would see gets a fanout.
6. **Upstream workflows that act on a repository** are guarded to `opentibiabr/canary` (F-CI), never deleted.

## Files new in the fork (no upstream counterpart, never conflict)

`src/creatures/players/bot/*` (engine TUs, impl/interface/loader/platform/pathcore/navdump/walksync/host headers),
`src/database/botdatabasetasks.*`, `src/game/game_bot_market.cpp`, `src/config/bot_config_keys.hpp`,
`cmake/BotEngineAbiTag.cmake`, `src/creatures/players/cast/*`, all bot Lua/data (`data/scripts/**/bot_*`,
`data/bot/**`, `data/bot_chat/**`), `database/bots/**`, `deployment/**`, `tools/**`, `implementation_plans/**`.

## Modified upstream files (the re-apply set)

`+/-` = lines vs `666a60f6f`.

| File | Family | +/- | Hook / re-apply note |
|---|---|---|---|
| `CMakeLists.txt` | O1 build | +12/-1 | ccache default + bot source lists. Take upstream's toolchain/vcpkg changes, keep our two hooks. |
| `cmake/modules/CanaryLib.cmake` | O1 build | +11/-1 | `-rdynamic` on the core so the `.so` resolves Player/Game/... from the executable. |
| `src/creatures/CMakeLists.txt` | O1 build | +276/-0 | `bot_engine` target: SHARED `.so` on Linux (own PCH, `-fPIC -fno-lto -fno-gnu-unique`, ABI tag #58, #50 POST_BUILD symbol gate), STATIC on Windows (`/MT /bigobj /utf-8`, `setup_target`). Keep upstream's target changes; re-apply ours as one block. |
| `src/database/CMakeLists.txt` | O1 build | +2/-1 | Bot DB task sources (`botdatabasetasks.*`). |
| `src/game/CMakeLists.txt` | O1 build | +1/-0 | `game_bot_market.cpp` source. |
| `tests/unit/players/CMakeLists.txt` | O1 build | +1/-0 | `cast_publisher_test.cpp` (kept next to upstream's tests). |
| `config.lua.dist` | O3c bot-config | +890/-0 | The bot block (all `bot*` keys, cast/camera keys, server tweaks) on top of upstream's file. Keep upstream's new keys; production `config.lua` gets the same additions by hand. |
| `src/config/config_enums.hpp` | O3c bot-config | +125/-1 | BOT_* X-macro tables expanded AFTER every core key (and the non-table fork keys at the very end). Upstream inserts core keys mid-enum on every sync: that is fine for the paired binary+.so rebuild (the #58 ABI tag hashes every `src/*.hpp`, so a skewed pair is refused) but pushes bot ordinals past MAGIC_ENUM_RANGE_MAX (500) -> see `config_functions.cpp` (#154). New upstream keys go BEFORE the bot tables. |
| `src/config/configmanager.cpp` | O3c bot-config | +184/-0 | X-macro loader for the BOT_* tables + cast keys + the cast-operator startup warning (before upstream's deferred callbacks). |
| `src/lua/functions/core/game/config_functions.cpp` | O3c bot-config | +17/-0 | #154: registers every bot config key in Lua `configKeys` BY NAME (magic_enum stops at 500). |
| `src/main.cpp` | O3 lifecycle | +7/-0 | Touch `g_dispatcher()` before `inject<CanaryServer>()` so the Dispatcher singleton outlives Game at exit (Game is first built by ServiceManager -> `Signals::asyncWait`; `~SpawnNpc` calls `Dispatcher::stopEvent`: ASAN heap-use-after-free on every SIGTERM, P5 #160). **Candidate upstream report** (same code at the pin). |
| `src/creatures/creature.cpp` | O3 lifecycle + O7 cast | +16/-1 | `onAttacking` bot guard (woken-bot login protection). `getStepDuration`: a bot with a real cast viewer steps `BOT_CAST_STEP_EXTRA_MS` (10) slower, matching OTClient's local-player step overhead so the camera never runs behind (per-step hitch on fast bots). Keep the hook after `walk.duration`, before the diagonal factor. |
| `src/creatures/creature.hpp` | O3 lifecycle | +20/-0 | Hibernation helpers (`setNotRemoved`, `clearTileParent`) + `friend class BotEngine`. `BOT_CAST_STEP_EXTRA_MS` next to `WALK_DIAGONAL_EXTRA_COST`. |
| `src/creatures/monsters/monster.cpp` | O5 perf | +12/-2 | #160: `countsAsSchedulingViewer` - a bot is not a "visible for scheduling" spectator unless it has a cast viewer (real or synthetic), so monsters seen only by bots use the background walk/AI lanes. Targeting and compute relevance (`isPlayerControlledCreature`) still count bots. Re-apply on any rewrite of `observeVisiblePlayerForScheduling` / `addVisiblePlayerSpectator`. |
| `src/creatures/monsters/monster.hpp` | O3 lifecycle | +7/-20 | Public push utilities the engine uses. |
| `src/creatures/npcs/npcs.hpp` | O3 lifecycle | +16/-0 | `getNpcTypes` / `getShopItems` accessors (fidget-drop pool from gold NPC shops). |
| `src/game/game.hpp` | O3 lifecycle | +13/-0 | Bot method declarations (`restartBotTickLoop`, market helpers). |
| `src/io/functions/iologindata_load_player.cpp` | O3 lifecycle | +22/-3 | Bot load (onlinetime fix #44). |
| `src/io/functions/iologindata_save_player.cpp` | O3 lifecycle | +3/-1 | Bot save tweak. |
| `src/lua/functions/core/game/game_functions.cpp` | O3 lifecycle | +661/-0 | `loadBotPlayer` + `Game.bot*` (engine control, reload, census). Uses the legacy Player userdata helpers like upstream (#3987 left Player/Creature for a later audit). |
| `src/lua/functions/core/game/game_functions.hpp` | O3 lifecycle | +39/-0 | `loadBotPlayer` + `Game.bot*` (engine control, reload, census). Uses the legacy Player userdata helpers like upstream (#3987 left Player/Creature for a later audit). |
| `src/map/house/house.cpp` | O3 lifecycle | +19/-0 | House claim/reclaim hooks. |
| `src/server/network/protocol/protocolstatus.cpp` | O3 lifecycle | +17/-4 | Bot-inclusive online count (XML + binary) plus upstream's `unique` attribute. |
| `src/game/game.cpp` | O3 lifecycle + O5t | +154/-37 | Engine load + `restartBotTickLoop` (single owner of the 100ms BotEngine::tick cycleEvent, explicit `DispatcherLane::WorldCommit`), pre-wake on teleport, `updatePlayersOnline` bot exclusion (on top of #3969's hardened sync; never upstream's DELETE-all shortcut), `checkCreatures` jitter diagnostic, `Game::shutdown` calling `g_dispatcher().shutdown()` first. |
| `src/creatures/players/player.cpp` | O3 lifecycle + O7 cast | +464/-90 | HARD. Bot death lifecycle (early-exit before despawn), `canSee` fallback for clientless bots, PZ NPC walk-through, hibernate save-skip, no-levelup, bot backpack for cameras; `canCastBroadcast` + the `allowPlayerCast` gate in `setCastBroadcasting`/`onThink` (#163); **cast fanout**: 68+ `forwardToCastViewers` sites and the `castViewers.empty()` early-outs. On every upstream rewrite of a `send*`: put the fanout BEFORE any new `!client` return (#3911 lesson) and run the fanout audit (`.work/port1525/bin/fanout_audit.py` style: every M0 fanout site must survive). #160: `countsAsSchedulingViewer()` (read by `monster.cpp`). |
| `src/creatures/players/player.hpp` | O3 lifecycle + O7 cast | +83/-0 | `isBotPlayer`, cast viewer members, bot accessors. Append-only. #160: `countsAsSchedulingViewer()` (read by `monster.cpp`). `hasRealCastViewers()` (read by `Creature::getStepDuration`). |
| `src/lua/functions/creatures/player/player_functions.cpp` | O3 lifecycle + O7 cast | +205/-0 | Bot Lua methods (`setSlotItem`, `castSpell`, `setFightMode`, `isBotPlayer`, cast methods) next to upstream's. |
| `src/lua/functions/creatures/player/player_functions.hpp` | O3 lifecycle + O7 cast | +11/-0 | Bot Lua methods (`setSlotItem`, `castSpell`, `setFightMode`, `isBotPlayer`, cast methods) next to upstream's. |
| `data/events/scripts/creature.lua` | O4 gameplay | +5/-1 | Skull rule: allow attacking a skulled player. |
| `data/libs/systems/blessing.lua` | O4 gameplay | +2/-1 | `playerLoseItemsOnDeath` config switch. |
| `src/creatures/combat/combat.cpp` | O4 gameplay | +1/-1 | No-consume for bots. |
| `src/creatures/combat/spells.cpp` | O4 gameplay | +14/-6 | Bot learn-spell bypass on both `hasLearnedInstantSpell` checks of upstream's LEARN_SPELLS structure (#3971); infinite soul / rune charges. |
| `src/creatures/players/grouping/party.cpp` | O4 gameplay | +5/-0 | Fight-tick seed for bots. |
| `src/io/iomarket.cpp` | O4 gameplay | +29/-0 | Bot market hooks. |
| `src/io/iomarket.hpp` | O4 gameplay | +1/-0 | Bot market hooks. |
| `src/items/decay/decay.cpp` | O4 gameplay | +10/-0 | Dwarven-ring decay freeze for bots. |
| `src/items/tile.cpp` | O4 gameplay | +41/-9 | Bot pathing flags (FLAG_IGNOREBLOCKITEM / IGNOREFIELDDAMAGE branches); upstream's Expert PvP field step applies on the plain BLOCKPATH branch. `queryAdd`'s PZ-to-non-PZ `maxPlayersOutsidePZPerAccount` check skips bot players (they share account 65000), mirroring `spells.cpp`; humans on one account are still capped. |
| `src/items/weapons/weapons.cpp` | O4 gameplay | +6/-1 | No weapon break for bots. |
| `src/lua/functions/creatures/creature_functions.cpp` | O4 gameplay | +62/-3 | `addHealth` attacker argument (kill attribution) + bot helpers. |
| `src/lua/functions/creatures/creature_functions.hpp` | O4 gameplay | +2/-0 | `addHealth` attacker argument (kill attribution) + bot helpers. |
| `src/map/map.cpp` | O4 gameplay | +13/-1 | Bot A* flags (push items; walk through creatures in PZ) in upstream's per-node `getPathfindingTile` (used by `canWalkTo` and `getPathMatching`), via `getPlayerRaw()`. |
| `data-otservbr-global/scripts/quests/soul_war/eventcallback_on_combat_taint.lua` | O5 perf | +15/-3 | Skip the per-hit kv lookup for bots. |
| `data/scripts/globalevents/update_guild_war_status.lua` | O5 perf | +5/-1 | Async guild-war status UPDATE. |
| `src/creatures/players/imbuements/imbuements.cpp` | O5 perf | +15/-0 | Bot imbuement decay-scan skip. |
| `src/game/scheduling/save_manager.cpp` | O5 perf | +26/-2 | Bots always async-save; hibernate save-skip; bots always refresh `loginPosition` in `saveAll` (#3903 made a temple position sticky until logout, and bots never log out). |
| `src/lib/logging/log_with_spd_log.cpp` | O5 perf | +16/-0 | Async spdlog (flush_on warn + flush_every 3s) with upstream's timestamp format. |
| `src/lua/functions/core/libs/db_functions.cpp` | O5 perf | +70/-0 | `db.asyncQuery` Lua binding. |
| `src/lua/functions/core/libs/db_functions.hpp` | O5 perf | +2/-0 | `db.asyncQuery` Lua binding. |
| `src/database/database.cpp` | O5 perf + O5t | +93/-0 | `db.asyncQuery` support + sync-DB timer (DB_SYNC_SLOW, DbDispatcherStats) next to upstream's query helpers. |
| `src/database/database.hpp` | O5 perf + O5t | +10/-0 | `db.asyncQuery` support + sync-DB timer (DB_SYNC_SLOW, DbDispatcherStats) next to upstream's query helpers. |
| `src/game/scheduling/dispatcher.cpp` | O5t telemetry + O5 perf | +207/-4 | RETAINED (user directive): CYCLE_SLOW / CYCLE_GAP / DISP_SLOW / ASYNC_WAIT / DISP_RATES / DB_SYNC drain, re-instrumented on upstream's lane loop (#4023/#4030). Also the +4 thread-task reserve (BotDatabaseTasks), `isOnDispatcherThread`, `friend Game`, and the shutdown guard in `executeScheduledTask`. **Per-task globals are written on the dispatcher thread only** (`executeTask` checks `isOnDispatcherThread()`): upstream's parallel lanes run `executeTask` on pool workers concurrently, and unguarded `std::string` assigns corrupted the heap (P5 #160 SEGVs). Never patch hunks: take upstream's loop and re-instrument. **#160 scheduling:** `executeBarrierLane` runs the background `MonsterAI` lane inline on the dispatcher thread (single-partition path, still BarrierParallel context); visible lanes keep upstream's fork-join. Pairs with `monster.cpp` `countsAsSchedulingViewer`. |
| `src/game/scheduling/dispatcher.hpp` | O5t telemetry | +22/-1 | RETAINED (user directive): CYCLE_SLOW / CYCLE_GAP / DISP_SLOW / ASYNC_WAIT / DISP_RATES / DB_SYNC drain, re-instrumented on upstream's lane loop (#4023/#4030). Also the +4 thread-task reserve (BotDatabaseTasks), `isOnDispatcherThread`, `friend Game`, and the shutdown guard in `executeScheduledTask`. Never patch hunks: take upstream's loop and re-instrument. |
| `src/lua/global/globalevent.cpp` | O5t telemetry | +12/-1 | Globalevent timing. |
| `src/server/network/protocol/protocolgame.cpp` | O7 cast (bot camera) | +573/-6 | HARD. Bot camera (#153, cast C1): `routeViewerLogin` (a bot, or `@cast` with `allowPlayerCast` (#163) -> `botCameraLogin`, a human `@livestream` -> upstream's `castViewerLogin`), placed AFTER upstream's login-layout parse; 15.25 `sendCastViewerInit` + the `botCameraUpstreamBootstrap` A/B arm; viewer input gate, operator `/cavebot`, `/lagmark`, Cast Chat 0x50; `release()` / `writeToOutputBuffer` / `parseSay` hooks next to upstream's livestream. Upstream's livestream code stays untouched. **Upstream livestream fix (P5 #160 V8):** `sendLivestreamViewerAppear` copies the viewer's known-creature set into the caster's protocol after the viewer bootstrap, so the byte-mirrored stream never sends a creature as "known" that this viewer never received (OTClient dropped the viewer). Same bug at the pin. **#215:** a bot camera gets the monk Virtue entry of 0xC1 alone in its own network message (`writeIsolatedToOutputBuffer`): the June-2026 Redemption client misreads its official 15.25 layout and drops the rest of the message, which desynced the camera's creatures. |
| `src/server/network/protocol/protocolgame.hpp` | O7 cast (bot camera) | +23/-0 | HARD. Bot camera (#153, cast C1): `routeViewerLogin` (a bot, or `@cast` with `allowPlayerCast` (#163) -> `botCameraLogin`, a human `@livestream` -> upstream's `castViewerLogin`), placed AFTER upstream's login-layout parse; 15.25 `sendCastViewerInit` + the `botCameraUpstreamBootstrap` A/B arm; viewer input gate, operator `/cavebot`, `/lagmark`, Cast Chat 0x50; `release()` / `writeToOutputBuffer` / `parseSay` hooks next to upstream's livestream. Upstream's livestream code stays untouched. |
| `src/server/network/protocol/protocollogin.cpp` | O7 cast (bot camera) | +103/-31 | `@cast` -> the bot camera list (humans on our `/cast` only with `allowPlayerCast`, #163); `@livestream` -> upstream's list plus the active bots. One list format (upstream's livestream one). |
| `src/server/network/protocol/protocollogin.hpp` | O7 cast (bot camera) | +3/-0 | `@cast` -> the bot camera list; `@livestream` -> upstream's list plus the active bots. One list format (upstream's livestream one). |
| `src/utils/const.hpp` | O7 cast (bot camera) | +4/-0 | `CHANNEL_CAST = 0x50` + `static_assert` that it differs from `CHANNEL_LIVESTREAM`. |
| `data/scripts/creaturescripts/player/death.lua` | O7 cast | +5/-0 | Human cast cleanup on death / logout (stays: stopping is allowed whatever `allowPlayerCast` says, #163). |
| `data/scripts/creaturescripts/player/logout.lua` | O7 cast | +6/-0 | Human cast cleanup on death / logout (stays: stopping is allowed whatever `allowPlayerCast` says, #163). |
| `data-otservbr-global/migrations/50.lua` | O8 db | +2/-2 | Charm columns INT UNSIGNED (ours). |
| `schema.sql` | O8 db | +19/-19 | Ours: onlinetime BIGINT, charm counters INT UNSIGNED, `cast_broadcasters`, no sample accounts (they come from database/bots). Take upstream's `db_version` and new tables. |
| `data-otservbr-global/scripts/actions/other/fishing.lua` | O6 bot-data | +11/-2 | Bot infinite worms. |
| `data-otservbr-global/scripts/globalevents/others/check_mount.lua` | O6 bot-data | +4/-0 | Skip bots in the mount check. |
| `data-otservbr-global/scripts/movements/teleport/shrine_entrance.lua` | O6 bot-data | +1/-1 | Bots ignore the shrine level gate. |
| `data/XML/groups.xml` | O6 bot-data | +44/-0 | Group 7 `bot`. |
| `data/global.lua` | O6 bot-data + O4 gameplay | +12/-2 | `BotPlayers` global. `PARTY_PROTECTION` / `ADVANCED_SECURE_MODE` off only for an explicit `retro-pvp` world: upstream #4033 made `"pvp"` a retro alias, which dropped party protection + secure-mode area protection on our `"pvp"` world (2026-11-23; the bot AoE bystander guard mirrors these rules via `Combat::canDoCombat`). Re-apply after any upstream edit of the two lines. |
| `data/scripts/actions/doors/key_door.lua` | O6 bot-data | +26/-2 | Bots open doors without keys / level / quest checks. |
| `data/scripts/actions/doors/level_door.lua` | O6 bot-data | +5/-2 | Bots open doors without keys / level / quest checks. |
| `data/scripts/actions/doors/quest_door.lua` | O6 bot-data | +5/-2 | Bots open doors without keys / level / quest checks. |
| `data/scripts/actions/items/foods.lua` | O6 bot-data | +5/-0 | Bots eat / drink from an infinite supply. |
| `data/scripts/actions/items/potions.lua` | O6 bot-data | +8/-0 | Bots eat / drink from an infinite supply. |
| `data/scripts/eventcallbacks/player/on_kill_taskboard.lua` | O6 bot-data | +20/-1 | Bots skip the task-board kill hook; a monster type with raceId 0 returns early (logged once per type) instead of making `isMonsterBestiaryUnlocked(0)` report "Monster race id not exists" on every kill. **Candidate upstream report**: the raceId-0 guard also affects humans on stock upstream (P5 #160). |
| `data/scripts/eventcallbacks/player/on_look.lua` | O6 bot-data | +20/-0 | Bot status in look text. |
| `data/scripts/globalevents/server_initialization.lua` | O6 bot-data | +4/-0 | Truncate `bot_active_players` at boot. |
| `data/scripts/lib/register_spells.lua` | O6 bot-data | +4/-1 | Bots conjure without blank runes. |
| `.github/workflows/autofix-ci.yml` | F-CI | +2/-1 | Root job guarded with `github.repository == 'opentibiabr/canary' && (...)` (#157). On conflict: take upstream's job, re-wrap its `if:`. |
| `.github/workflows/copilot-issue-fix.yml` | F-CI | +2/-1 | Root job guarded with `github.repository == 'opentibiabr/canary' && (...)` (#157). On conflict: take upstream's job, re-wrap its `if:`. |
| `.github/workflows/cron-stale.yml` | F-CI | +4/-1 | Root job guarded with `github.repository == 'opentibiabr/canary' && (...)` (#157). On conflict: take upstream's job, re-wrap its `if:`. |
| `.github/workflows/release.yml` | F-CI | +2/-1 | Root job guarded with `github.repository == 'opentibiabr/canary' && (...)` (#157). On conflict: take upstream's job, re-wrap its `if:`. |
| `.github/workflows/sync-release-tag.yml` | F-CI | +2/-1 | Root job guarded with `github.repository == 'opentibiabr/canary' && (...)` (#157). On conflict: take upstream's job, re-wrap its `if:`. |
| `.github/workflows/update_vcpkg_baseline.yml` | F-CI | +2/-1 | Root job guarded with `github.repository == 'opentibiabr/canary' && (...)` (#157). On conflict: take upstream's job, re-wrap its `if:`. |
| `.github/workflows/reusable-build-windows.yml` | O9 windows | +8/-1 | CMake-only matrix (the vcxproj `Solution` arm has no bot files and is dead). Take upstream's runner/cache changes. |
| `LICENSE` | docs | +57/-0 | Fork notice above the GPL text. |
| `README.md` | docs | +381/-95 | Ours (the fork's README). On conflict: `git checkout --ours`. |
| `.gitattributes` | repo | +26/-0 | Narrow eol rules for authored CSV / cfg / botperf scripts, after upstream's rules. Keep both. |
| `.gitignore` | repo | +81/-1 | Our ignore block (tools/, runs/, secrets). Keep both. |

