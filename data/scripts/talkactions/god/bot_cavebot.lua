-- /cavebot command: debug/control bot player AI via C++ BotEngine
-- Usage: /cavebot <botname> <command> [args...]
-- Commands: status, pos, goto, teleport, navigate, travel, hunt, stop, resume, etc.

-- Look up town by name (case-insensitive, supports partial match)
local function findTown(name)
	local town = Town(name)
	if town then return town end
	-- Try capitalized
	town = Town(name:sub(1,1):upper() .. name:sub(2):lower())
	if town then return town end
	return nil
end

-- Simulation state for waypoint walk-through
local SimulationState = {} -- [playerId] = { waypoints, currentIdx, paused, type, label, phase, eventId }

local function simulationTick(playerId)
	local sim = SimulationState[playerId]
	if not sim or sim.paused then return end
	local player = Player(playerId)
	if not player then SimulationState[playerId] = nil; return end

	sim.currentIdx = sim.currentIdx + 1
	if sim.currentIdx > #sim.waypoints then
		player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] Completed %s (%d waypoints)", sim.label, #sim.waypoints))
		SimulationState[playerId] = nil
		return
	end

	local wp = sim.waypoints[sim.currentIdx]
	player:teleportTo(Position(wp.x, wp.y, wp.z))

	local msg
	if sim.type == "route" then
		msg = string.format("[sim] ROUTE: Walking to %s wp %d/%d at (%d,%d,%d)",
			wp.wpType or "stand", sim.currentIdx, #sim.waypoints, wp.x, wp.y, wp.z)
	elseif sim.type == "hunt" then
		msg = string.format("[sim] %s: wp %d/%d (%d,%d,%d) type=%s",
			(sim.phase or "patrol"):upper(), sim.currentIdx, #sim.waypoints, wp.x, wp.y, wp.z, wp.wpType or "stand")
	elseif sim.type == "poi" then
		msg = string.format("[sim] POI: %d/%d '%s' [%s] (%d,%d,%d)",
			sim.currentIdx, #sim.waypoints, wp.name or "?", wp.poiType or "?", wp.x, wp.y, wp.z)
	end
	player:sendTextMessage(MESSAGE_STATUS, msg)

	sim.eventId = addEvent(simulationTick, 1000, playerId)
end

local cavebotCmd = TalkAction("/cavebot")

function cavebotCmd.onSay(player, words, param)
	-- Party subcommand: available to ALL players (not just gods)
	if param and param ~= "" then
		local trimmedParam = param:lower():match("^%s*(.-)%s*$")
		if trimmedParam == "party leave" or trimmedParam == "party dismiss" then
			local result = Game.botCommand("*", "party leave " .. player:getId())
			player:sendTextMessage(MESSAGE_STATUS, result)
			return true
		elseif trimmedParam:sub(1,6) == "party " then
			local vocList = trimmedParam:sub(7)
			local result = Game.botCommand("*", "party create " .. player:getId() .. " " .. vocList)
			player:sendTextMessage(MESSAGE_STATUS, result)
			return true
		elseif trimmedParam == "party" then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot party ek,ed,ms  |  /cavebot party leave")
			return true
		elseif trimmedParam == "claim" or trimmedParam:sub(1, 6) == "claim " then
			-- Claim the spawn you're standing in (kicks the bot hunting it, reserves it 1h)
			local nameArg = trimmedParam == "claim" and "" or trimmedParam:sub(7)
			local cmd = "claimspawn " .. player:getId()
			if nameArg ~= "" then cmd = cmd .. " " .. nameArg end
			local result = Game.botCommand("_global", cmd)
			player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
			return true
		elseif trimmedParam == "release" or trimmedParam == "unclaim" then
			local result = Game.botCommand("_global", "releasespawn " .. player:getId())
			player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
			return true
		end
		-- House claim/sub-owner/release moved to the standalone /house talkaction
		-- (data/scripts/talkactions/player/bot_house.lua).
	end

	-- Require God access (group >= 3)
	if player:getGroup():getAccess() == false then
		player:sendTextMessage(MESSAGE_STATUS_WARNING, "You need God access to use /cavebot.")
		return true
	end

	if not param or param == "" then
		player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot <botname> <command> [args...]")
		player:sendTextMessage(MESSAGE_STATUS, "Commands: status, goto x,y,z, stop, verbose on|off, reload, partyhunt, partyinfo, partystop")
		player:sendTextMessage(MESSAGE_STATUS, "Spawn-claim: claim [name], release, claims, clearclaim <name>")
		return true
	end

	-- Global command: reload [debug,N | debug off] (no bot name needed)
	-- /cavebot reload              → plain hot-reload, preserve current active set
	-- /cavebot reload debug,1      → hot-reload + activate only 1 bot, enable debug stream on it
	-- /cavebot reload debug,2      → hot-reload + activate only 2 bots with debug stream
	-- /cavebot reload debug off    → hot-reload + activate ALL registered bots, clear debug streams
	local lowParam = param:lower()
	if lowParam == "reload" or lowParam:match("^reload%s") then
		local opts = { source = "talkaction", player = player }
		-- Parse from `param` (original case), NOT `lowParam` — bot names must survive.
		local dbgSpec, dbgNames = BotSystem.parseDebugSpec(param)
		if dbgSpec then
			opts.debugCount = dbgSpec
			opts.debugBotNames = dbgNames
		end
		BotSystem.executeReload(opts)
		return true
	end

	-- ================================================================
	-- AUTHORED-DATA COMMANDS (BOT_CSV Milestone 2)
	--
	-- These used to run 46 direct db.query/db.storeQuery calls against
	-- bot_hunt_scripts / bot_hunt_waypoints / bot_hunt_targets /
	-- bot_city_routes / bot_city_route_waypoints / bot_city_pois. Authored
	-- data now lives in data/bot/authored/*.csv, so every data operation is
	-- an engine command in bot_csvedit.cpp; this file keeps only argument
	-- parsing, town resolution and message formatting.
	--
	-- Two things worth knowing before editing anything here:
	--
	--  * SEQ IS LINE ORDER. There is no seq column any more, so "insert at
	--    seq N" is "insert a line at index N of that phase/route block".
	--    The UPDATE ... SET seq = seq +/- 1 renumbering pairs this file used
	--    to run are gone, not moved.
	--
	--  * EDITS DO NOT AUTO-APPLY. The engine writes the file and stops; the
	--    running bots keep the data they loaded. Run /cavebot reload to
	--    apply — which is exactly what these commands have always said.
	-- ================================================================

	-- Send one engine command and print whatever it returns, line by line.
	local function csvCall(cmd)
		local result = tostring(Game.botCommand("_global", cmd))
		for line in result:gmatch("[^\n]+") do
			player:sendTextMessage(MESSAGE_STATUS, line)
		end
		return true
	end

	-- Parse a leading '"quoted name"' or a bare first token.
	-- Returns name, rest-of-string.
	local function takeName(s)
		local quoted, after = s:match('^%s*"([^"]+)"%s*(.*)$')
		if quoted then
			return quoted, after
		end
		local bare, rest = s:match("^%s*(%S+)%s*(.*)$")
		return bare, rest or ""
	end

	-- Split '<hunt name> <phase> <rest...>' where the name may be UNQUOTED and contain
	-- spaces. Quoted wins; otherwise scan for the first phase keyword and treat
	-- everything before it as the name. The pre-CSV command supported this and the
	-- documented examples rely on it (/cavebot huntadd Pirates Yalahar patrol 5), so
	-- requiring quotes would be a silent regression.
	local PHASE_WORDS = { travel_to = true, hunt_patrol = true, travel_from = true, patrol = true }
	local function takeNameBeforePhase(s)
		local quoted, after = s:match('^%s*"([^"]+)"%s*(.*)$')
		if quoted then
			return quoted, after
		end
		local tokens = {}
		for t in s:gmatch("%S+") do tokens[#tokens + 1] = t end
		for i = 2, #tokens do            -- start at 2: the name is never empty
			if PHASE_WORDS[tokens[i]:lower()] then
				return table.concat(tokens, " ", 1, i - 1), table.concat(tokens, " ", i)
			end
		end
		-- No phase keyword found: fall back to first-token-is-the-name so the usage
		-- message below fires with something sensible rather than nil.
		return tokens[1], table.concat(tokens, " ", 2)
	end

	-- Waypoint types the engine's strict enum accepts for hand-entry.
	local VALID_WP_TYPES = {
		stand = true, node = true, ladder = true, rope = true, hole = true,
		shovel = true, stairs_up = true, stairs_down = true, door = true,
		action = true, machete = true, use_with = true, npc_interact = true,
		teleport = true, levitate_up = true, levitate_down = true,
	}

	-- Trailing "[type] [x,y,z]" shared by routeadd/huntadd. Defaults to
	-- 'stand' at the sender's own position, as before.
	local function takeTypeAndPos(tokens, startIdx)
		local wpType, pos = "stand", player:getPosition()
		for i = startIdx, #tokens do
			local tok = tokens[i]
			local cx, cy, cz = tok:match("^(%d+),(%d+),(%d+)$")
			if cx then
				pos = { x = tonumber(cx), y = tonumber(cy), z = tonumber(cz) }
			elseif VALID_WP_TYPES[tok:lower()] then
				wpType = tok:lower()
			else
				return nil, nil, tok
			end
		end
		return wpType, pos, nil
	end

	local function posArg(pos)
		return string.format("%d,%d,%d", pos.x, pos.y, pos.z)
	end

	-- ---------------------------------------------------------------
	-- routewp <town> [src~dst]
	-- ---------------------------------------------------------------
	if param:lower():match("^routewp%s+") then
		local args = param:sub(#"routewp " + 1)
		local townArg, rest = takeName(args)
		local town = findTown(townArg)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tostring(townArg)))
			return true
		end
		local pair = rest:match("^%s*(%S+)")
		if pair then
			return csvCall(string.format("csvroutewp %d|%s", town:getId(), pair))
		end
		return csvCall(string.format("csvroutewp %d", town:getId()))
	end

	-- ---------------------------------------------------------------
	-- routeadd <town> <src~dst> <seq> [type] [x,y,z]
	-- routedel <town> <src~dst> <seq>
	-- ---------------------------------------------------------------
	if param:lower():match("^routeadd%s+") or param:lower():match("^routedel%s+") then
		local isAdd = param:lower():match("^routeadd%s+") ~= nil
		local args = param:sub((isAdd and #"routeadd " or #"routedel ") + 1)
		local tokens = {}
		for t in args:gmatch("%S+") do tokens[#tokens + 1] = t end
		if #tokens < 3 then
			player:sendTextMessage(MESSAGE_STATUS, isAdd
				and "Usage: /cavebot routeadd <town> <src~dst> <seq> [type] [x,y,z]"
				or  "Usage: /cavebot routedel <town> <src~dst> <seq>")
			player:sendTextMessage(MESSAGE_STATUS, "Example: /cavebot routeadd darashia depot~boat 3 stand 32310,32210,6")
			player:sendTextMessage(MESSAGE_STATUS, "seq is the 0-based position in the route; omit x,y,z to use where you stand.")
			return true
		end
		local town = findTown(tokens[1])
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tokens[1]))
			return true
		end
		local src, dst = tokens[2]:match("^([^~]+)~(.+)$")
		if not src then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid route format. Use: src~dst (e.g. depot~boat)")
			return true
		end
		local seq = tonumber(tokens[3])
		if not seq then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid seq number: " .. tokens[3])
			return true
		end
		if not isAdd then
			return csvCall(string.format("csvroutewpdel %d|%s|%s|%d", town:getId(), src, dst, seq))
		end
		local wpType, pos, bad = takeTypeAndPos(tokens, 4)
		if bad then
			player:sendTextMessage(MESSAGE_STATUS, string.format("Invalid type or coordinates '%s'.", bad))
			return true
		end
		return csvCall(string.format("csvroutewpadd %d|%s|%s|%d|%s|%s",
			town:getId(), src, dst, seq, wpType, posArg(pos)))
	end

	-- ================================================================
	-- POI Commands: poi, poiadd, poidel, poiupdate
	-- ================================================================

	-- poiadd <town> "<name>" <type> [x,y,z]
	if param:lower():match("^poiadd%s+") then
		local args = param:sub(#"poiadd " + 1)
		local townArg, rest = takeName(args)
		local town = findTown(townArg)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tostring(townArg)))
			return true
		end
		local poiName, rest2 = takeName(rest)
		if not poiName or poiName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poiadd <town> "<name>" <type> [x,y,z]')
			player:sendTextMessage(MESSAGE_STATUS, "Types: depot, depot_outside, temple, boat, shop, npc, adventurer_stone")
			return true
		end
		local poiType, pos = "depot", player:getPosition()
		for tok in (rest2 or ""):gmatch("%S+") do
			local cx, cy, cz = tok:match("^(%d+),(%d+),(%d+)$")
			if cx then
				pos = { x = tonumber(cx), y = tonumber(cy), z = tonumber(cz) }
			else
				poiType = tok:lower()
			end
		end
		return csvCall(string.format("csvpoiadd %d|%s|%s|%s", town:getId(), poiName, posArg(pos), poiType))
	end

	-- poidel <town> "<name>"
	if param:lower():match("^poidel%s+") then
		local townArg, rest = takeName(param:sub(#"poidel " + 1))
		local town = findTown(townArg)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tostring(townArg)))
			return true
		end
		local poiName = takeName(rest)
		if not poiName or poiName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poidel <town> "<name>"')
			return true
		end
		return csvCall(string.format("csvpoidel %d|%s", town:getId(), poiName))
	end

	-- poiupdate <town> "<name>" [x,y,z]
	if param:lower():match("^poiupdate%s+") then
		local townArg, rest = takeName(param:sub(#"poiupdate " + 1))
		local town = findTown(townArg)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tostring(townArg)))
			return true
		end
		local poiName, rest2 = takeName(rest)
		if not poiName or poiName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poiupdate <town> "<name>" [x,y,z]')
			return true
		end
		local pos = player:getPosition()
		local cx, cy, cz = (rest2 or ""):match("(%d+),(%d+),(%d+)")
		if cx then
			pos = { x = tonumber(cx), y = tonumber(cy), z = tonumber(cz) }
		end
		return csvCall(string.format("csvpoiupdate %d|%s|%s", town:getId(), poiName, posArg(pos)))
	end

	-- poi <town>
	if param:lower():match("^poi%s+") then
		local townArg = takeName(param:sub(#"poi " + 1))
		local town = findTown(townArg)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tostring(townArg)))
			return true
		end
		return csvCall(string.format("csvpoi %d", town:getId()))
	end

	-- ================================================================
	-- Simulate Commands: teleport admin through waypoints
	--
	-- The position list now comes from the engine's csvpositions command
	-- (which reads the CSVs); the pause/continue/stop state machine and
	-- simulationTick are unchanged.
	-- ================================================================

	if param:lower():match("^simulate%s+") then
		local simArgs = param:sub(#"simulate " + 1)
		local subCmd = simArgs:match("^(%S+)")
		if not subCmd then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate route|hunt|poi|pause|continue|stop")
			return true
		end
		subCmd = subCmd:lower()

		if not SimulationState then
			SimulationState = {}
		end

		if subCmd == "pause" then
			local sim = SimulationState[player:getId()]
			if sim then
				sim.paused = true
				if sim.eventId then stopEvent(sim.eventId); sim.eventId = nil end
				player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] Paused at wp %d/%d", sim.currentIdx, #sim.waypoints))
			else
				player:sendTextMessage(MESSAGE_STATUS, "[sim] No active simulation.")
			end
			return true
		end

		if subCmd == "continue" or subCmd == "resume" then
			local sim = SimulationState[player:getId()]
			if sim and sim.paused then
				sim.paused = false
				sim.eventId = addEvent(simulationTick, 1000, player:getId())
				player:sendTextMessage(MESSAGE_STATUS, "[sim] Resumed.")
			else
				player:sendTextMessage(MESSAGE_STATUS, "[sim] No paused simulation to resume.")
			end
			return true
		end

		if subCmd == "stop" then
			local sim = SimulationState[player:getId()]
			if sim then
				if sim.eventId then stopEvent(sim.eventId) end
				SimulationState[player:getId()] = nil
				player:sendTextMessage(MESSAGE_STATUS, "[sim] Stopped.")
			else
				player:sendTextMessage(MESSAGE_STATUS, "[sim] No active simulation.")
			end
			return true
		end

		-- Ask the engine for the position list, then drive the same walker.
		local posCmd, label, simPhase
		if subCmd == "route" then
			local tokens = {}
			for t in simArgs:sub(#"route " + 1):gmatch("%S+") do tokens[#tokens + 1] = t end
			if #tokens < 2 then
				player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate route <town> <src~dst>")
				return true
			end
			local town = findTown(tokens[1])
			if not town then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tokens[1]))
				return true
			end
			local src, dst = tokens[2]:match("^([^~]+)~(.+)$")
			if not src then
				player:sendTextMessage(MESSAGE_STATUS, "Invalid route format. Use: src~dst")
				return true
			end
			posCmd = string.format("csvpositions route|%d|%s|%s", town:getId(), src, dst)
			label = string.format("route %s~%s in %s", src, dst, town:getName())
		elseif subCmd == "hunt" then
			local rest = simArgs:sub(#"hunt " + 1)
			local scriptName, after = takeNameBeforePhase(rest)
			if not scriptName then
				player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot simulate hunt "<hunt name>" [phase]')
				return true
			end
			local phase = (after or ""):match("^%s*(%S+)")
			if phase == "patrol" then phase = "hunt_patrol" end
			simPhase = phase
			posCmd = string.format("csvpositions hunt|%s%s", scriptName, phase and ("|" .. phase) or "")
			label = string.format("hunt '%s'%s", scriptName, phase and (" " .. phase) or "")
		elseif subCmd == "poi" then
			local townArg = simArgs:sub(#"poi " + 1):match("^%s*(%S+)")
			local town = townArg and findTown(townArg)
			if not town then
				player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate poi <town>")
				return true
			end
			posCmd = string.format("csvpositions poi|%d", town:getId())
			label = string.format("POIs of %s", town:getName())
		else
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate route|hunt|poi|pause|continue|stop")
			return true
		end

		local raw = tostring(Game.botCommand("_global", posCmd))
		if raw:sub(1, 3) == "ERR" then
			player:sendTextMessage(MESSAGE_STATUS, "[sim] " .. raw)
			return true
		end
		local waypoints = {}
		for line in raw:gmatch("[^\n]+") do
			local x, y, z = line:match("^(%d+),(%d+),(%d+)$")
			if x then
				waypoints[#waypoints + 1] = { x = tonumber(x), y = tonumber(y), z = tonumber(z) }
			end
		end
		if #waypoints == 0 then
			player:sendTextMessage(MESSAGE_STATUS, "[sim] No waypoints found for " .. label)
			return true
		end

		local old = SimulationState[player:getId()]
		if old and old.eventId then stopEvent(old.eventId) end
		SimulationState[player:getId()] = {
			waypoints = waypoints,
			currentIdx = 1,
			paused = false,
			type = subCmd,
			label = label,
			phase = simPhase,
			eventId = nil,
		}
		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[sim] Walking %d waypoints of %s. /cavebot simulate pause|continue|stop",
			#waypoints, label))
		SimulationState[player:getId()].eventId = addEvent(simulationTick, 1000, player:getId())
		return true
	end

	-- ================================================================
	-- Hunt-script commands: huntwp, huntadd, huntdel, hunttarget,
	--                       targetadd, targetdel
	-- ================================================================

	-- huntwp "<hunt name>" [phase]
	if param:lower():match("^huntwp%s+") then
		local scriptName, rest = takeNameBeforePhase(param:sub(#"huntwp " + 1))
		if not scriptName then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntwp "<hunt name>" [phase]')
			return true
		end
		local phase = (rest or ""):match("^%s*(%S+)")
		if phase == "patrol" then phase = "hunt_patrol" end
		return csvCall(string.format("csvhuntwp %s%s", scriptName, phase and ("|" .. phase) or ""))
	end

	-- huntadd "<hunt name>" <phase> <seq> [type] [x,y,z]
	-- huntdel "<hunt name>" <phase> <seq>
	if param:lower():match("^huntadd%s+") or param:lower():match("^huntdel%s+") then
		local isAdd = param:lower():match("^huntadd%s+") ~= nil
		local args = param:sub((isAdd and #"huntadd " or #"huntdel ") + 1)
		local scriptName, rest = takeNameBeforePhase(args)
		local tokens = {}
		for t in (rest or ""):gmatch("%S+") do tokens[#tokens + 1] = t end
		if not scriptName or #tokens < 2 then
			player:sendTextMessage(MESSAGE_STATUS, isAdd
				and 'Usage: /cavebot huntadd "<hunt name>" <phase> <seq> [type] [x,y,z]'
				or  'Usage: /cavebot huntdel "<hunt name>" <phase> <seq>')
			player:sendTextMessage(MESSAGE_STATUS, "  Phases: travel_to, patrol (hunt_patrol), travel_from")
			player:sendTextMessage(MESSAGE_STATUS, "  seq is the 0-based position within that phase.")
			return true
		end
		local phase = tokens[1]:lower()
		if phase == "patrol" then phase = "hunt_patrol" end
		if phase ~= "travel_to" and phase ~= "hunt_patrol" and phase ~= "travel_from" then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid phase '" .. tokens[1] .. "'. Valid: travel_to, patrol, travel_from")
			return true
		end
		local seq = tonumber(tokens[2])
		if not seq then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid seq number: " .. tokens[2])
			return true
		end
		if not isAdd then
			return csvCall(string.format("csvhuntwpdel %s|%s|%d", scriptName, phase, seq))
		end
		local wpType, pos, bad = takeTypeAndPos(tokens, 3)
		if bad then
			player:sendTextMessage(MESSAGE_STATUS, string.format("Invalid type or coordinates '%s'.", bad))
			return true
		end
		return csvCall(string.format("csvhuntwpadd %s|%s|%d|%s|%s", scriptName, phase, seq, wpType, posArg(pos)))
	end

	-- hunttarget "<hunt name>"
	if param:lower():match("^hunttarget%s+") then
		local scriptName = takeName(param:sub(#"hunttarget " + 1))
		if not scriptName then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot hunttarget "<hunt name>"')
			return true
		end
		return csvCall(string.format("csvhunttarget %s", scriptName))
	end

	-- targetadd "<hunt name>" <monster name>
	-- targetdel "<hunt name>" <monster name>
	if param:lower():match("^targetadd%s+") or param:lower():match("^targetdel%s+") then
		local isAdd = param:lower():match("^targetadd%s+") ~= nil
		local args = param:sub((isAdd and #"targetadd " or #"targetdel ") + 1)
		local scriptName, rest = takeName(args)
		local monsterName = (rest or ""):match("^%s*(.-)%s*$")
		if not scriptName or monsterName == "" then
			player:sendTextMessage(MESSAGE_STATUS, isAdd
				and 'Usage: /cavebot targetadd "<hunt name>" <monster name>'
				or  'Usage: /cavebot targetdel "<hunt name>" <monster name>')
			return true
		end
		-- The old command accepted trailing [priority] [count]. Those columns existed
		-- in bot_hunt_targets but the engine never read them (only monster_name was
		-- ever SELECTed), so they are not carried into the CSV. Strip and say so
		-- rather than silently treating them as part of the monster name.
		local stripped = monsterName:match("^(.-)%s+%d+%s+%d+$") or monsterName:match("^(.-)%s+%d+$")
		if stripped and stripped ~= "" then
			player:sendTextMessage(MESSAGE_STATUS,
				"[cavebot] Note: priority/count are not stored — the engine never read them. Using name: " .. stripped)
			monsterName = stripped
		end
		return csvCall(string.format("csv%s %s|%s", isAdd and "targetadd" or "targetdel", scriptName, monsterName))
	end

	-- Global NAV DIAGNOSTICS passthrough. These are implemented in the engine
	-- (bot_command.cpp) but had no talkaction route, so they were only reachable through the
	-- bot_commands MySQL queue — which is drained on a 10s MANAGER_INTERVAL and therefore
	-- useless for interactive debugging. Passed through with ORIGINAL CASE: npcapproach takes a
	-- display-cased NPC name, and lowercasing it would break the lookup.
	--   /cavebot zplan 32369,32241,7 32310,32210,6   plan a cross-floor route (no movement)
	--   /cavebot zgraph                              portal/component/flood-cache stats
	--   /cavebot npcapproach Frodo                   computed approach tiles for an NPC
	--   /cavebot botcfg                              every nav tunable as actually loaded
	--   /cavebot pathbench [N]                       us/call: server vs kernel vs jitter
	--   /cavebot pathtest [N]                        kernel-vs-server path parity
	--   /cavebot cache                               per-cache hit rates
	--   /cavebot perfstat                            tick histograms + counters (JSON)
	--   /cavebot perfphases                          per-phase cost + worst ticks (JSON)
	--   /cavebot perfreset                           start a new measurement window
	--   /cavebot probe list|clear                    perf-harness probe bots
	--   /cavebot hibernateall                        deterministic floor before a run
	do
		local navHead = param:lower():match("^(%a+)")
		-- `route` was added to the engine well after this allowlist was written and never
		-- registered here, so /cavebot route fell through to the bot-name parser below and
		-- answered "Bot 'route' not found" — indistinguishable from the command not existing.
		-- It only ever worked via the bot_commands MySQL queue, which bypasses this table.
		local navGlobals = {
			zplan = true, zgraph = true, npcapproach = true, botcfg = true,
			pathbench = true, pathtest = true, cache = true, dumpnav = true,
			route = true,
			-- fishspots fell into the exact same hole as route: implemented in the engine,
			-- never registered here, so /cavebot fishspots answered "Bot 'fishspots' not found"
			-- while the MySQL queue path worked fine.
			fishspots = true,
			-- BOT_CORPSE_LOOT. Registered here on the SAME day it was written, because
			-- route and fishspots both shipped without it and answered "Bot 'x' not found"
			-- until someone noticed months later.
			lootstats = true,
			-- BOT_ACTIVITY_PCT. Registered in the SAME commit as the command, for the reason
			-- the two comments above record: route and fishspots both shipped unregistered and
			-- answered "Bot 'x' not found", which is indistinguishable from not existing.
			activity = true,
			-- PERF STRESS HARNESS. Registered in the same commit as the commands, for the
			-- reason the three comments above record. `probe` covers `probe list` and
			-- `probe clear`; the per-bot `probe on|off|teleport` forms parse through the
			-- bot-name path below and never reach this table.
			perfstat = true, perfphases = true, perfreset = true,
			probe = true, hibernateall = true,
		}
		if navHead and navGlobals[navHead] then
			local result = Game.botCommand("_global", param)
			for line in tostring(result):gmatch("[^\n]+") do
				player:sendTextMessage(MESSAGE_STATUS, line)
			end
			return true
		end
	end

	-- Global command: whohunts [search]
	-- /cavebot whohunts         → show all active hunt reservations
	-- /cavebot whohunts wasp    → show reservations matching "wasp"
	if param:lower():match("^whohunts") then
		local result = Game.botCommand("_global", param:lower())
		player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
		return true
	end

	-- Global command: claims — list active player spawn-claims (owner + minutes left)
	if param:lower() == "claims" then
		local result = Game.botCommand("_global", "listclaims")
		player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
		return true
	end

	-- Global command: clearclaim <name> — admin force-release a player spawn-claim
	if param:lower():match("^clearclaim%s") then
		local nameArg = param:sub(#"clearclaim " + 1)
		local result = Game.botCommand("_global", "clearclaim " .. nameArg)
		player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
		return true
	end

	-- Global command: partyinfo — show all active party hunts
	if param:lower() == "partyinfo" then
		local result = Game.botCommand("_global", "partyinfo")
		player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
		return true
	end

	-- Global command: population — per-town bot counts by state + per-anchor
	-- proximity snapshot (active bots only). Merged surface: prints both the
	-- [POPULATION] and [PROXIMITY] reports on demand. These are no longer
	-- broadcast to admin chat on a timer (see bot_engine.cpp) — pull them here.
	if param:lower() == "population" then
		player:sendTextMessage(MESSAGE_STATUS, tostring(Game.botCommand("_global", "population")))
		player:sendTextMessage(MESSAGE_STATUS, tostring(Game.botCommand("_global", "proximity")))
		return true
	end

	-- Deprecated: /cavebot proximity is merged into /cavebot population.
	if param:lower() == "proximity" then
		player:sendTextMessage(MESSAGE_STATUS, "[cavebot] /cavebot proximity is merged into /cavebot population (shows both reports).")
		return true
	end

	-- Global command: partystop <botname> — dissolve a specific bot's party hunt
	if param:lower():match("^partystop%s") then
		local targetName = param:sub(11) -- after "partystop "
		local result = Game.botCommand("_global", "partystop " .. targetName)
		player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))
		return true
	end

	-- Parse bot name and command.
	-- Supports: /cavebot "Name With Spaces" command...
	--           /cavebot Name With Spaces command  (auto-detect by matching against loaded bots)
	--           /cavebot SingleName command
	local botName, cmdStr

	-- Try quoted name first: "Bot Name" command...
	local quoted, afterQuote = param:match('^"([^"]+)"%s+(.+)$')
	if quoted then
		botName = quoted
		cmdStr = afterQuote
	end

	-- If no quoted match, try to find the longest bot name prefix
	if not botName then
		local paramLower = param:lower()
		local bestLen = 0
		for _, p in pairs(BotPlayers or {}) do
			if p and not p:isRemoved() then
				local name = p:getName()
				local nameLower = name:lower()
				if #name > bestLen and paramLower:sub(1, #nameLower + 1) == nameLower .. " " then
					bestLen = #name
					botName = name
				end
			end
		end
		if botName then
			cmdStr = param:sub(#botName + 2)
		end
	end

	-- Fallback: single-word name
	if not botName then
		botName, cmdStr = param:match("^(%S+)%s+(.+)$")
	end

	if not botName or not cmdStr then
		player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot <botname> <command> [args...]")
		return true
	end

	-- Special handling: "active" without coords → use admin's position
	if cmdStr == "active" then
		local pos = player:getPosition()
		cmdStr = string.format("active %d,%d,%d", pos.x, pos.y, pos.z)
	end

	-- Route command to C++ BotEngine
	local result = Game.botCommand(botName, cmdStr)
	player:sendTextMessage(MESSAGE_STATUS, "[cavebot] " .. tostring(result))

	return true
end

cavebotCmd:separator(" ")
cavebotCmd:groupType("god")
cavebotCmd:register()
