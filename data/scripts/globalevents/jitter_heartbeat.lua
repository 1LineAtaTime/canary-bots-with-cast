-- ============================================================================
-- JITTER DIAGNOSTIC: dispatcher heartbeat
-- ============================================================================
-- Part of the internal jitter root-cause analysis.
-- Primary spike-detection instrument: fires every INTERVAL_MS, logs if the
-- delay between expected and actual fire time exceeds THRESHOLD_MS.
-- A delay = dispatcher was stalled BETWEEN this fire and the previous one.
-- ============================================================================

local INTERVAL_MS  = 200
local THRESHOLD_MS = 100

local lastFire = nil

local hb = GlobalEvent("JitterHeartbeat")
function hb.onThink()
    local now = Game.monotonicMs and Game.monotonicMs() or (os.time() * 1000)
    if lastFire then
        local expected = lastFire + INTERVAL_MS
        local delta = now - expected
        if delta > THRESHOLD_MS then
            logger.warn(string.format(
                "[HB_STALL] expected=%d actual=%d delta=%dms",
                expected, now, delta))
        end
    end
    lastFire = now
    return true
end

hb:interval(INTERVAL_MS)
hb:register()
