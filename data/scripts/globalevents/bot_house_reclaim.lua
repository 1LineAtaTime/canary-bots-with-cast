-- ============================================================================
-- bot_house_reclaim.lua — when a bot-claimed house falls vacant ("back on the
-- market"), hand it back to its original bot and restore the snapshotted layout
-- (see data/scripts/lib/bot_house_claim.lua).
--
-- Vacancy is read straight from the `houses` row with a CONSERVATIVE filter
-- (owner=0, state=0, no bids, no pending transfer) so this never fights or
-- cancels a Cyclopedia auction. Restore work is chunked across ticks to avoid
-- the bulk-op SEGV pattern seen in /housebackfill.
-- ============================================================================

local RECLAIM_INTERVAL_MS   = 300000  -- 5 minutes
local RECLAIM_CHUNK         = 5       -- houses restored per tick
local RECLAIM_CHUNK_DELAY_MS = 500

local function reclaimChunk(todo, startIdx)
	local endIdx = math.min(startIdx + RECLAIM_CHUNK - 1, #todo)
	for i = startIdx, endIdx do
		local e = todo[i]
		local house = House(e.hid)
		-- Re-check ownership at apply time (the async snapshot may be stale).
		if house and house:getOwnerGuid() == 0 and BotHouseClaim.isBotGuid(e.bot) then
			local ok, err = pcall(function()
				local n = BotHouseClaim.restore(house, e.bot)
				logger.info(string.format("[BotHouseReclaim] house %d returned to bot %d (%d items restored)", e.hid, e.bot, n))
			end)
			if not ok then
				logger.error("[BotHouseReclaim] house " .. tostring(e.hid) .. " restore failed: " .. tostring(err))
			end
		end
	end

	if endIdx < #todo then
		addEvent(reclaimChunk, RECLAIM_CHUNK_DELAY_MS, todo, endIdx + 1)
	end
end

local botHouseReclaim = GlobalEvent("BotHouseReclaim")

function botHouseReclaim.onThink(interval)
	BotHouseClaim.ensureSchema()

	-- Async so the dispatcher isn't blocked on the DB round-trip (the callback
	-- runs back on the main thread, where map mutation is safe).
	db.botAsyncStoreQuery(
		"SELECT o.`house_id` AS hid, o.`bot_guid` AS bot "
		.. "FROM `bot_house_origin` o JOIN `houses` h ON h.`id` = o.`house_id` "
		.. "WHERE h.`owner` = 0 AND h.`state` = 0 AND h.`bidder` = 0 "
		.. "AND h.`highest_bid` = 0 AND h.`new_owner` <= 0",
		function(res)
			if not res then return end
			local todo = {}
			repeat
				todo[#todo + 1] = { hid = Result.getNumber(res, "hid"), bot = Result.getNumber(res, "bot") }
			until not Result.next(res)
			Result.free(res)
			if #todo > 0 then
				reclaimChunk(todo, 1)
			end
		end)

	return true
end

botHouseReclaim:interval(RECLAIM_INTERVAL_MS)
botHouseReclaim:register()
