-- ============================================================================
-- Bot Market Funding -- top up bot bank balances at server startup
-- ============================================================================
-- Each bot needs sufficient gold to make BUY offers and accept real player SELL
-- offers. Per the approved plan we top up to 100 kkk (100 billion gp) every
-- server startup.
--
-- Why 30s delay: bot_manager.lua loads 200 bots with 100ms staggered addEvent()
-- which takes ~20 seconds. We wait 30s to be safe so all bots are in BotPlayers.
-- For online bots we call Player:setBankBalance directly (otherwise raw SQL would
-- be overwritten on the next save). For any bot not yet loaded, raw SQL is the
-- defensive fallback.
-- ============================================================================

local FUND_AMOUNT = 100000000000 -- 100 kkk per bot
local FUNDING_DELAY_MS = 30000

local botMarketFunding = GlobalEvent("BotMarketFunding")

function botMarketFunding.onStartup()
	addEvent(function()
		local fundedInMemory = 0
		local botAccountId = (BOT_CONFIG and BOT_CONFIG.BOT_ACCOUNT_ID) or 65000

		for _, player in pairs(BotPlayers or {}) do
			if player and not player:isRemoved() then
				player:setBankBalance(FUND_AMOUNT)
				fundedInMemory = fundedInMemory + 1
			end
		end

		-- Defensive raw-SQL update for any bot that wasn't loaded into memory.
		-- For in-memory bots this gets overwritten by their next save (using the
		-- already-set bank balance from above), so it's harmless.
		db.asyncQuery(string.format(
			"UPDATE `players` SET `balance` = %d WHERE `account_id` = %d",
			FUND_AMOUNT, botAccountId
		))

		logger.info(string.format(
			"[BotMarketFunding] Funded %d in-memory bots with %d gp each (raw-SQL backstop applied to all account=%d players)",
			fundedInMemory, FUND_AMOUNT, botAccountId
		))
	end, FUNDING_DELAY_MS)

	return true
end

botMarketFunding:register()
