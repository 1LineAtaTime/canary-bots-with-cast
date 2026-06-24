-- ============================================================================
-- JITTER DIAGNOSTIC: /lagmark talkaction
-- ============================================================================
-- Part of the internal jitter root-cause analysis.
-- User spams /lagmark during felt jitter. Each invocation logs a timestamp and
-- echoes a confirmation back to the client. Once the dispatcher resumes after
-- a stall, queued /lagmark messages process in burst and confirmations appear
-- — the user knows to stop spamming when the first echo arrives.
-- ============================================================================

local lagmark = TalkAction("/lagmark")

function lagmark.onSay(player, words, param)
    local ms = Game.monotonicMs and Game.monotonicMs() or (os.time() * 1000)
    logger.info(string.format("[LAGMARK] guid=%d name=%s t=%d",
        player:getGuid(), player:getName(), ms))
    player:sendTextMessage(MESSAGE_STATUS, "lagmark @ " .. ms)
    return false
end

lagmark:groupType("god")
lagmark:separator(" ")
lagmark:register()
