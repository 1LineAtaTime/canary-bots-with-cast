local updateGuildWarStatus = GlobalEvent("UpdateGuildWarStatus")

function updateGuildWarStatus.onThink(interval)
	local currentTime = os.time()
	-- JITTER FIX 2026-06-11 (user-approved stock deviation): async — the sync
	-- db.query blocked the dispatcher for the full DB round-trip every 60s
	-- (measured 3.8s at 15:09 behind a DB convoy; implicated in 2 of 3 lagmarks
	-- that window). Fire-and-forget UPDATE; behavior identical.
	db.asyncQuery(string.format("UPDATE `guild_wars` SET `status` = 4, `ended` = %d WHERE `status` = 1 AND `ended` != 0 AND `ended` < %d", currentTime, currentTime))
	return true
end

updateGuildWarStatus:interval(60000)
updateGuildWarStatus:register()
