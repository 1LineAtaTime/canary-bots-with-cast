local castTalk = TalkAction("/cast")

function castTalk.onSay(player, words, param)
	if param == "on" then
		-- Also publishes the cast in cast_broadcasters (Player::setCastBroadcasting)
		player:setCastBroadcasting(true)
		player:sendTextMessage(MESSAGE_STATUS, "Cast broadcasting enabled. Viewers can now watch you.")
	elseif param == "off" then
		player:setCastBroadcasting(false)
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
