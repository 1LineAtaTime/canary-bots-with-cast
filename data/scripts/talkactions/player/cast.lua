local castTalk = TalkAction("/cast")

function castTalk.onSay(player, words, param)
	if param == "on" then
		player:setCastBroadcasting(true)
		db.query("INSERT INTO `cast_broadcasters` (`player_id`, `player_name`) VALUES (" .. player:getGuid() .. ", " .. db.escapeString(player:getName()) .. ") ON DUPLICATE KEY UPDATE `player_name` = " .. db.escapeString(player:getName()))
		player:sendTextMessage(MESSAGE_STATUS, "Cast broadcasting enabled. Viewers can now watch you.")
	elseif param == "off" then
		player:setCastBroadcasting(false)
		db.query("DELETE FROM `cast_broadcasters` WHERE `player_id` = " .. player:getGuid())
		player:sendTextMessage(MESSAGE_STATUS, "Cast broadcasting disabled. All viewers disconnected.")
	else
		local count = player:getCastViewerCount()
		local status = player:isCastBroadcasting() and "ON" or "OFF"
		player:sendTextMessage(MESSAGE_STATUS, "Cast is " .. status .. ". Viewers: " .. count .. ". Usage: /cast on/off")
	end
	return true
end

castTalk:separator(" ")
castTalk:groupType("normal")
castTalk:register()
