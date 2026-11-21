-- #94: ExtendedOpcode channel between the OTClient bot-control panel (otclient/modules/game_botcontrol)
-- and the bot command verbs (src/creatures/players/bot/bot_verb.cpp).
--
-- Client -> server (opcode 120, plain text):
--   list                 -> the speaker's party bots + bots on screen, as JSON {t="list", bots=[...]}
--   cmd <verb text>      -> run a verb exactly as if typed in the party channel, e.g. "cmd @ek follow",
--                           "cmd @Some_Bot invite", "cmd summon ek,ed". Reply JSON {t="res", replies=[...]}
-- Server -> client (opcode 120): the JSON string the engine returned.
--
-- The security ladder, addressing, rate limit and [BOT_VERB] telemetry all live in the engine, so
-- this file only relays. OTClient enables extended opcodes when the server sends opcode 0 at login
-- (ProtocolGame::sendClientLoginPreamble does, for every OTClient OS), which is why a Tibia client
-- can never reach this handler.

local BOT_CONTROL_OPCODE = 120
local MAX_REQUEST = 512

local botControlOpcode = CreatureEvent("BotControlOpcode")

function botControlOpcode.onExtendedOpcode(player, opcode, buffer)
	if opcode ~= BOT_CONTROL_OPCODE then
		return
	end
	if player:isBotPlayer() or not Game.botCommand then
		return
	end
	if type(buffer) ~= "string" or buffer == "" or #buffer > MAX_REQUEST then
		return
	end
	-- One line only: the engine command grammar is line-based.
	buffer = buffer:gsub("[\r\n]", " ")
	local reply = Game.botCommand("_global", "verb opcode " .. player:getId() .. " " .. buffer, "opcode")
	if reply and reply ~= "" then
		player:sendExtendedOpcode(BOT_CONTROL_OPCODE, reply)
	end
end

botControlOpcode:register()

local botControlOpcodeLogin = CreatureEvent("BotControlOpcodeLogin")

function botControlOpcodeLogin.onLogin(player)
	if not player:isBotPlayer() then
		player:registerEvent("BotControlOpcode")
	end
	return true
end

botControlOpcodeLogin:register()
