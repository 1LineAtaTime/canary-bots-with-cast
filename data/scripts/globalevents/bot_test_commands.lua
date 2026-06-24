-- ============================================================================
-- Bot Test Commands -- MySQL-driven sentinel for CPU benchmarking
-- Polls bot_test_commands table once per second and applies aiPaused gating.
-- Used by external benchmark tooling. No game-state side effects beyond toggling
-- the aiPaused flag on the bot AI tick loop.
-- ============================================================================

-- Disabled by default — this poller is only needed when running the external
-- CPU benchmark tooling (insert rows into bot_test_commands MySQL table to drive
-- pause_all / resume_all / hibernate_all / wake_all). Flipped to false 2026-05-21
-- since we don't run automated benchmarks; saves the 1s sync MySQL query baseline.
-- Set true to re-enable if the benchmark tooling is run again.
local POLLER_ENABLED = false

local CPUTestPoller = GlobalEvent("BotTestCommandPoller")

function CPUTestPoller.onThink(interval)
	if not POLLER_ENABLED then return true end
	local rows = db.storeQuery(
		"SELECT `id`, `command` FROM `bot_test_commands` WHERE `executed` = 0 ORDER BY `id` ASC LIMIT 10")
	if not rows then return true end
	repeat
		local id = Result.getNumber(rows, "id")
		local cmd = Result.getString(rows, "command")
		if cmd == "pause_all" then
			Game.botSetAllAIPaused(true)
			logger.info("[BotTestCommands] pause_all applied")
		elseif cmd == "resume_all" then
			Game.botSetAllAIPaused(false)
			logger.info("[BotTestCommands] resume_all applied")
		elseif cmd == "hibernate_all" then
			local count = Game.botHibernateAllEligible()
			logger.info(string.format("[BotTestCommands] hibernate_all applied: %d bots hibernated", count))
		elseif cmd == "wake_all" then
			local count = Game.botWakeAllHibernated()
			logger.info(string.format("[BotTestCommands] wake_all applied: %d bots woken", count))
		end
		db.asyncQuery(string.format(
			"UPDATE `bot_test_commands` SET `executed` = 1, `executed_at` = %d WHERE `id` = %d",
			os.time(), id))
	until not Result.next(rows)
	Result.free(rows)
	return true
end

CPUTestPoller:interval(1000)
CPUTestPoller:register()
