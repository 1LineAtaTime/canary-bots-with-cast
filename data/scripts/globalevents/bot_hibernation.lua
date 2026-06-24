-- ============================================================================
-- Bot Hibernation -- spatial despawn with AI-state preservation
-- ============================================================================
-- Proximity monitor: every TICK_INTERVAL_MS, check each bot's distance to the
-- nearest real player. Bots far from any real player for HYSTERESIS_MS get
-- hibernated (Player object destroyed, BotState preserved). Bots that come
-- within DISTANCE_TILES of a real player wake immediately.
--
-- Hibernation reclaims both the bot's AI tick cost AND the world-reaction cost
-- (monster aggro toward the in-world Player). See CPU_BENCHMARK_2026-05-06.md.
-- ============================================================================

local HIBERNATION_CONFIG = {
    DISTANCE_TILES   = 100,    -- max |dx|,|dy| from a real player to stay awake (any z)
    HYSTERESIS_MS    = 30000,  -- ms with no real player nearby before hibernating
    TICK_INTERVAL_MS = 300,    -- proximity loop cadence (3 game ticks; was 1000)

    -- Rate limits prevent the game thread from blocking during traversal bursts.
    -- Each wake op does a synchronous DB load (~5-20ms); each hibernate op does
    -- a synchronous DB save via onRemoveCreature.
    MAX_HIBERNATES_PER_TICK = 5,
    MAX_WAKES_PER_TICK      = 5,

    -- Master switch: set false to disable the entire system without removing the file.
    ENABLED = true,

    -- PERF_INVESTIGATION_2026-05-24 pre-flight telemetry: emit a [BOT:TRANSITION]
    -- line on every wake / hibernate, with reason + hysteresis age. Logs ~one
    -- line per real wake / hibernate, so cost is bounded by transition rate,
    -- NOT by tick rate. Safe at low transition volume.
    -- Enabled 2026-06-09 for liveness diagnostics — pairs with the C++ [PROXIMITY]
    -- and [POPULATION] periodic logs to triangulate why few bots appear near players.
    TRANSITION_LOG_ENABLED = true,
}

local Hibernation = GlobalEvent("BotHibernationMonitor")

-- Per-guid timer: ms timestamp when the bot first became eligible to hibernate
-- (no real player within DISTANCE_TILES). Reset to 0 when a player approaches.
local noPlayerSince = {}

-- Squared Chebyshev distance check -- returns true if any real player is within
-- DISTANCE_TILES of (bx, by). Z is ignored entirely (any floor).
local function anyRealPlayerNear(bx, by, realPlayerPositions)
    local dist = HIBERNATION_CONFIG.DISTANCE_TILES
    for i = 1, #realPlayerPositions do
        local pos = realPlayerPositions[i]
        local dx = pos.x - bx
        local dy = pos.y - by
        if dx < 0 then dx = -dx end
        if dy < 0 then dy = -dy end
        if dx <= dist and dy <= dist then
            return true
        end
    end
    return false
end

local diagTickCount = 0

function Hibernation.onThink(interval)
    if not HIBERNATION_CONFIG.ENABLED then return true end
    if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end

    diagTickCount = diagTickCount + 1
    if diagTickCount == 1 or (diagTickCount % 60) == 0 then
        logger.info(string.format("[BotHibernation] tick #%d firing", diagTickCount))
    end

    -- JITTER DIAGNOSTIC (JITTER_ROOT_CAUSE_DIAGNOSTIC.md §3.7):
    -- wall-clock timing of the proximity-loop body. Uses Game.monotonicMs (OTSYS_TIME)
    -- to catch mutex blocks inside getBotHibernationStates that os.clock() would miss.
    local jitter_bodyStart = Game.monotonicMs and Game.monotonicMs() or 0

    -- Build proximity-anchor position list once per tick. Anchors are:
    --   1. Real (non-bot) players.
    --   2. Bot players currently being watched via cast (viewer count > 0).
    -- A cast-watched bot acts as a proximity anchor so neighboring bots stay
    -- awake while the viewer watches, mirroring the wake-radius behavior real
    -- players get via Game::internalTeleport → wakeBotsInRadius.
    -- The watched bot itself is already prevented from hibernating by the
    -- bot_engine.cpp cast-viewer guard, so this only affects its neighbors.
    -- 200 bots + 1-3 real players (+ a handful of cast-watched bots): ~5 µs.
    local realPlayerPositions = {}
    for _, p in ipairs(Game.getPlayers()) do
        if (not p:isBotPlayer()) or (p:getCastViewerCount() > 0) then
            realPlayerPositions[#realPlayerPositions + 1] = p:getPosition()
        end
    end

    -- TIER 2-G (PERF_INVESTIGATION_2026-05-24): when no anchors exist (no real
    -- players online + no cast viewers), no hibernated bot can possibly wake.
    -- The 500-entry C++ snapshot via Game.getBotHibernationStates() costs
    -- significant shared_ptr<Tile> alloc per bot (perf samples up to 42.99% of
    -- post-stall CPU on __shared_ptr<Tile>::__shared_ptr). Skip the full snapshot
    -- and build a minimal awake-only iteration set from the Lua BotPlayers cache
    -- (typically 0-30 entries vs the full 500). This:
    --   • Still progresses awake bots through hibernation hysteresis (so they
    --     get hibernated within HYSTERESIS_MS of the last anchor disappearing)
    --   • Skips per-tile lookups for hibernated bots (no work needed for them
    --     when no anchor can come close)
    --   • Auto-resyncs on the next tick that has anchors (full snapshot runs)
    local nAnchors = #realPlayerPositions
    local botStates
    local jitter_getStatesMs = 0

    if nAnchors == 0 then
        botStates = {}
        if BotPlayers ~= nil then
            for guid, p in pairs(BotPlayers) do
                if p and not p:isRemoved() then
                    local pos = p:getPosition()
                    botStates[#botStates + 1] = {
                        guid = guid, name = p:getName(),
                        hibernated = false, active = true,
                        x = pos.x, y = pos.y, z = pos.z,
                    }
                end
            end
        end
    else
        -- JITTER DIAGNOSTIC: time the C++→Lua state snapshot separately
        local jitter_getStatesStart = Game.monotonicMs and Game.monotonicMs() or 0
        -- Snapshot bot state into Lua table once. C++ side iterates bots_ vector + g_game().getPlayers().
        botStates = Game.getBotHibernationStates()
        jitter_getStatesMs = (Game.monotonicMs and Game.monotonicMs() or 0) - jitter_getStatesStart
        if not botStates then
            if diagTickCount % 30 == 0 then logger.info("[BotHibernation] getBotHibernationStates returned nil") end
            return true
        end
    end
    -- Periodic visibility into proximity-loop state (every 60s)
    if (diagTickCount % 60) == 0 then
        local hibCount, awakeCount = 0, 0
        for i = 1, #botStates do
            if botStates[i].hibernated then hibCount = hibCount + 1
            elseif botStates[i].active then awakeCount = awakeCount + 1
            end
        end
        logger.info(string.format("[BotHibernation] %d bots: hibernated=%d awake=%d, %d real players",
            #botStates, hibCount, awakeCount, #realPlayerPositions))
    end

    local now = os.time() * 1000  -- ms; matches OTSYS_TIME() granularity loosely
    local hibernatesFired = 0
    local wakesFired = 0
    local seenGuids = {}

    -- PERF_INVESTIGATION_2026-05-24 Phase B (2026-06-01): LRU sort by lastWakeAttemptMs
    -- ascending (oldest first). When the density cap blocks some wakes, the skipped
    -- bots have their lastWakeAttemptMs updated by shouldGateWake on the C++ side, so
    -- they rotate to the back of the queue next iteration — no permanent dead zones
    -- at high-density chokepoints (temple, boat). Cost: ~100µs for 500 entries.
    table.sort(botStates, function(a, b)
        return (a.lastWakeAttemptMs or 0) < (b.lastWakeAttemptMs or 0)
    end)

    for i = 1, #botStates do
        local b = botStates[i]
        seenGuids[b.guid] = true

        -- Defensive sync: keep BotPlayers + BotActive in sync with whether the bot
        -- has a live Player object. Critical for cast list correctness:
        -- BotSystem.thinkEvent (bot_system.lua:2563-2568) periodically checks
        -- BotActive[guid] and CLEARS setCastBroadcasting(false) if not active.
        -- The C++ wakeBot sets broadcasting=true but doesn't touch BotActive, so
        -- without this sync the Lua watchdog re-clears broadcasting ~500ms later
        -- and the woken bot drops out of the cast list.
        if BotPlayers ~= nil then
            if b.hibernated then
                if BotPlayers[b.guid] ~= nil then BotPlayers[b.guid] = nil end
                if BotActive ~= nil then BotActive[b.guid] = false end
            else
                if BotPlayers[b.guid] == nil then
                    local p = Player(b.name)
                    if p then BotPlayers[b.guid] = p end
                end
                -- Bot is awake (active in C++) — Lua side must mirror so the
                -- thinkEvent watchdog doesn't clear setCastBroadcasting.
                if BotActive ~= nil and BotActive[b.guid] ~= true then
                    BotActive[b.guid] = true
                end
            end
        end

        local nearPlayer = anyRealPlayerNear(b.x, b.y, realPlayerPositions)

        if b.hibernated then
            -- Wake immediately on any real-player approach. Cascades to teammates
            -- via C++ wakeBot (party hunt leader cascade).
            if nearPlayer and wakesFired < HIBERNATION_CONFIG.MAX_WAKES_PER_TICK then
                if Game.botWake(b.guid) then
                    wakesFired = wakesFired + 1
                    noPlayerSince[b.guid] = nil
                    -- Refresh the Lua BotPlayers reference to the newly materialized Player.
                    -- The old shared_ptr (if any) was to the destroyed Player.
                    if BotPlayers ~= nil then
                        local newPlayer = Player(b.name)
                        if newPlayer then BotPlayers[b.guid] = newPlayer end
                    end
                    if HIBERNATION_CONFIG.TRANSITION_LOG_ENABLED then
                        logger.info(string.format(
                            "[BOT:TRANSITION] dir=wake guid=%d name=%s at=(%d,%d,%d) reason=proximity",
                            b.guid, tostring(b.name), b.x or 0, b.y or 0, b.z or 0))
                    end
                end
            end
        elseif b.active then
            -- Awake bot: check if it qualifies for hibernation
            if nearPlayer then
                noPlayerSince[b.guid] = nil  -- reset timer
            else
                if not noPlayerSince[b.guid] then
                    -- JITTER FIX: subtract random 0-10s from start time to stagger hibernation
                    -- across a 10s window instead of all bots hitting HYSTERESIS_MS together.
                    -- When user walks away from 20+ bots, they all START their hysteresis at
                    -- nearly the same moment. Without jitter, all 20 hibernate at exactly the
                    -- 30s mark, causing ~20 sync DB writes + removeCreature calls in one tick.
                    -- With 0-10s subtraction, each bot is "10s into hysteresis" already, so
                    -- effective hysteresis is 20-30s spread evenly over 10s = ~2 bots/sec.
                    noPlayerSince[b.guid] = now - math.random(0, 10000)
                elseif (now - noPlayerSince[b.guid]) >= HIBERNATION_CONFIG.HYSTERESIS_MS then
                    if hibernatesFired < HIBERNATION_CONFIG.MAX_HIBERNATES_PER_TICK then
                        local hysteresisAge = now - noPlayerSince[b.guid]
                        if Game.botHibernate(b.guid) then
                            hibernatesFired = hibernatesFired + 1
                            noPlayerSince[b.guid] = nil
                            -- Drop the Lua-side strong ref so the destroyed Player can
                            -- actually be released from memory. Restored on wake above.
                            if BotPlayers ~= nil then
                                BotPlayers[b.guid] = nil
                            end
                            if HIBERNATION_CONFIG.TRANSITION_LOG_ENABLED then
                                logger.info(string.format(
                                    "[BOT:TRANSITION] dir=hibernate guid=%d name=%s at=(%d,%d,%d) reason=hysteresis_expired age_ms=%d",
                                    b.guid, tostring(b.name), b.x or 0, b.y or 0, b.z or 0, hysteresisAge))
                            end
                        end
                    end
                end
            end
        end
    end

    -- Garbage-collect timers for bots that no longer exist (e.g., unregistered)
    for guid, _ in pairs(noPlayerSince) do
        if not seenGuids[guid] then
            noPlayerSince[guid] = nil
        end
    end

    -- JITTER DIAGNOSTIC: log if loop body or getStates exceeded threshold
    if Game.monotonicMs then
        local jitter_bodyMs = Game.monotonicMs() - jitter_bodyStart
        if jitter_bodyMs > 20 or jitter_getStatesMs > 10 then
            logger.warn(string.format(
                "[HIB_LOOP_SLOW] body=%dms getStates=%dms iter=%d wakes=%d hibs=%d",
                jitter_bodyMs, jitter_getStatesMs, #botStates, wakesFired, hibernatesFired))
        end
    end

    return true
end

Hibernation:interval(HIBERNATION_CONFIG.TICK_INTERVAL_MS)
Hibernation:register()
