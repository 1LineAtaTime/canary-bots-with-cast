-- /party command: summon bot players to form a party with shared exp
-- Usage:
--   /party ek,ed      — summon a knight and druid bot
--   /party ms,rp,ed   — summon a sorcerer, paladin, druid bot
--   /party leave       — dismiss all party bots
--   /party             — show help
--
-- Delegates to C++ BotEngine::executeCommand for state-accurate bot selection.

local botParty = TalkAction("/party")

function botParty.onSay(player, words, param)
	if not param or param == "" then
		player:sendTextMessage(MESSAGE_STATUS,
			"Usage: /party ek,ed,ms  |  /party leave")
		return true
	end

	local trimmed = param:lower():match("^%s*(.-)%s*$")

	-- Dismiss command
	if trimmed == "leave" or trimmed == "dismiss" or trimmed == "disband" then
		local result = Game.botCommand("", "party leave " .. player:getId())
		player:sendTextMessage(MESSAGE_STATUS, result)
		return true
	end

	-- Create party — delegate to C++ engine
	-- Command format: "party create <leaderCreatureId> <vocList>"
	local result = Game.botCommand("", "party create " .. player:getId() .. " " .. trimmed)
	player:sendTextMessage(MESSAGE_STATUS, result)
	return true
end

botParty:separator(" ")
botParty:groupType("normal")
botParty:register()
