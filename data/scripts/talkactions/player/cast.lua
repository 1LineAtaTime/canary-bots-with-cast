local castTalk = TalkAction("/cast")

-- allowPlayerCast (config.lua, default false, #163): this fork's human /cast is optional.
-- With it off, players stream with upstream's built-in livestream (!livestream on).
local function playerCastAllowed()
	return configKeys.ALLOW_PLAYER_CAST ~= nil and configManager.getBoolean(configKeys.ALLOW_PLAYER_CAST) == true
end

function castTalk.onSay(player, words, param)
	if not playerCastAllowed() then
		-- Changes no state. A cast begun before the toggle flipped is stopped by Player::onThink.
		player:sendTextMessage(MESSAGE_STATUS, "Player cast is disabled on this server. Use !livestream on to stream instead.")
		return true
	end

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
