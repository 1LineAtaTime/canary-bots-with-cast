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
		local debugMatch = lowParam:match("^reload%s+debug%s*,%s*(%d+)")
		if debugMatch then
			opts.debugCount = tonumber(debugMatch)
		elseif lowParam:match("^reload%s+debug%s+off") or lowParam:match("^reload%s+debug%-off") then
			opts.debugCount = "off"
		end
		BotSystem.executeReload(opts)
		return true
	end

	-- Global command: routewp <town> [src~dst]
	-- /cavebot routewp darashia            → list all routes for Darashia
	-- /cavebot routewp darashia depot~boat → list waypoints for that route
	local routewpMatch = param:lower():match("^routewp%s+")
	if routewpMatch then
		local routeArgs = param:sub(#"routewp " + 1)
		local tokens = {}
		for token in routeArgs:gmatch("%S+") do tokens[#tokens + 1] = token end

		if #tokens < 1 then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot routewp <town> [src~dst]")
			return true
		end

		local townName = tokens[1]
		local town = findTown(townName)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townName))
			return true
		end
		local townId = town:getId()
		townName = town:getName()

		-- No route specified: list all routes for this town
		if #tokens == 1 then
				local query = string.format(
				"SELECT `id`, `source_name`, (SELECT COUNT(*) FROM `bot_city_route_waypoints` w WHERE w.`route_id` = r.`id`) as wp_count FROM `bot_city_routes` r WHERE `town_id` = %d AND `enabled` = 1 ORDER BY `source_name`",
				townId)
			local result = db.storeQuery(query)
			if not result then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No enabled routes found for %s (town %d).", townName, townId))
				return true
			end

			local routes = {}
			repeat
				local sourceName = Result.getString(result, "source_name")
				local wpCount = Result.getNumber(result, "wp_count")
				local routeLabel = sourceName:match("|([^:]+):")
				if routeLabel then
					routes[#routes + 1] = string.format("  %s (%d wps)", routeLabel, wpCount)
				else
					routes[#routes + 1] = string.format("  %s (%d wps)", sourceName, wpCount)
				end
			until not Result.next(result)
			Result.free(result)

			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Routes for %s (town %d): %d found", townName, townId, #routes))
			local batch = {}
			for i, r in ipairs(routes) do
				batch[#batch + 1] = r
				if #batch >= 10 or i == #routes then
					player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
					batch = {}
				end
			end
			return true
		end

		-- Route specified: list waypoints
		local routeSpec = tokens[2]
		local src, dst = routeSpec:match("^([^~]+)~(.+)$")
		if not src or not dst then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid route format. Use: src~dst (e.g. depot~boat)")
			return true
		end
		src = src:lower()
		dst = dst:lower()

		local sourceLike = "%|" .. src .. "~" .. dst .. ":"
		local query = string.format(
			"SELECT `id`, `source_name` FROM `bot_city_routes` WHERE `town_id` = %d AND `source_name` LIKE %s AND `enabled` = 1",
			townId, db.escapeString(sourceLike))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No enabled route '%s~%s' found for %s (town %d).", src, dst, townName, townId))
			return true
		end

		local routeId = Result.getNumber(result, "id")
		local sourceName = Result.getString(result, "source_name")
		Result.free(result)

		local wpQuery = string.format(
			"SELECT `seq`, `pos_x`, `pos_y`, `pos_z`, `waypoint_type` FROM `bot_city_route_waypoints` WHERE `route_id` = %d ORDER BY `seq` ASC",
			routeId)
		local wpResult = db.storeQuery(wpQuery)
		if not wpResult then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Route %s has no waypoints.", sourceName))
			return true
		end

		player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Route %s:", sourceName))
		local batch = {}
		repeat
			local seq = Result.getNumber(wpResult, "seq")
			local wx = Result.getNumber(wpResult, "pos_x")
			local wy = Result.getNumber(wpResult, "pos_y")
			local wz = Result.getNumber(wpResult, "pos_z")
			local wt = Result.getString(wpResult, "waypoint_type")
			batch[#batch + 1] = string.format("  %d: (%d,%d,%d) %s", seq, wx, wy, wz, wt)
			if #batch >= 10 then
				player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
				batch = {}
			end
		until not Result.next(wpResult)
		Result.free(wpResult)
		if #batch > 0 then
			player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
		end
		return true
	end

	-- Global command: routeadd <town> <src~dst> <seq> [type] [x,y,z]
	-- Uses sender's position if x,y,z not provided.
	local routeaddMatch = param:lower():match("^routeadd%s+")
	if routeaddMatch then
		local routeArgs = param:sub(#"routeadd " + 1)
		local tokens = {}
		for token in routeArgs:gmatch("%S+") do tokens[#tokens + 1] = token end

		if #tokens < 3 then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot routeadd <town> <src~dst> <seq> [type] [x,y,z]")
			player:sendTextMessage(MESSAGE_STATUS, "Examples: /cavebot routeadd darashia depot~boat 0")
			player:sendTextMessage(MESSAGE_STATUS, "          /cavebot routeadd darashia depot~boat 3 ladder")
			player:sendTextMessage(MESSAGE_STATUS, "          /cavebot routeadd darashia depot~boat 3 stand 32310,32210,6")
			player:sendTextMessage(MESSAGE_STATUS, "Types: stand (default), node, ladder, rope, hole, stairs_up, stairs_down, door")
			return true
		end

		local townName = tokens[1]
		local town = findTown(townName)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townName))
			return true
		end
		local townId = town:getId()
		townName = town:getName()

		local routeSpec = tokens[2]
		local src, dst = routeSpec:match("^([^~]+)~(.+)$")
		if not src or not dst then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid route format. Use: src~dst (e.g. depot~boat)")
			return true
		end

		local seq = tonumber(tokens[3])
		if not seq then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid seq number: " .. tokens[3])
			return true
		end

		-- Parse optional [type] [x,y,z] from remaining tokens
		local wpType = "stand"
		local wpX, wpY, wpZ = nil, nil, nil
		local validTypes = {stand=true, node=true, ladder=true, rope=true, hole=true, stairs_up=true, stairs_down=true, door=true, action=true}

		if #tokens == 4 then
			local cx, cy, cz = tokens[4]:match("^(%d+),(%d+),(%d+)$")
			if cx then
				wpX, wpY, wpZ = tonumber(cx), tonumber(cy), tonumber(cz)
			elseif validTypes[tokens[4]:lower()] then
				wpType = tokens[4]:lower()
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid type '" .. tokens[4] .. "'. Valid: stand, node, ladder, rope, hole, stairs_up, stairs_down, door")
				return true
			end
		elseif #tokens == 5 then
			if validTypes[tokens[4]:lower()] then
				wpType = tokens[4]:lower()
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid type '" .. tokens[4] .. "'.")
				return true
			end
			local cx, cy, cz = tokens[5]:match("^(%d+),(%d+),(%d+)$")
			if cx then
				wpX, wpY, wpZ = tonumber(cx), tonumber(cy), tonumber(cz)
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid coordinates '" .. tokens[5] .. "'. Use: x,y,z")
				return true
			end
		end

		local pos = player:getPosition()
		if wpX then pos = {x = wpX, y = wpY, z = wpZ} end

		local sourceLike = "%|" .. src .. "~" .. dst .. ":"
		local query = string.format(
			"SELECT `id`, `source_name` FROM `bot_city_routes` WHERE `town_id` = %d AND `source_name` LIKE %s AND `enabled` = 1",
			townId, db.escapeString(sourceLike))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No enabled route '%s~%s' found for %s (town %d).", src, dst, townName, townId))
			return true
		end

		local routeId = Result.getNumber(result, "id")
		local sourceName = Result.getString(result, "source_name")
		Result.free(result)

		local countQuery = string.format("SELECT COUNT(*) as cnt FROM `bot_city_route_waypoints` WHERE `route_id` = %d", routeId)
		local countResult = db.storeQuery(countQuery)
		local wpCount = 0
		if countResult then
			wpCount = Result.getNumber(countResult, "cnt")
			Result.free(countResult)
		end

		db.query(string.format("UPDATE `bot_city_route_waypoints` SET `seq` = `seq` + 1 WHERE `route_id` = %d AND `seq` >= %d", routeId, seq))
		db.query(string.format(
			"INSERT INTO `bot_city_route_waypoints` (`route_id`, `seq`, `pos_x`, `pos_y`, `pos_z`, `waypoint_type`) VALUES (%d, %d, %d, %d, %d, %s)",
			routeId, seq, pos.x, pos.y, pos.z, db.escapeString(wpType)))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Added wp [%s] (%d,%d,%d) at seq %d for %s (was %d, now %d). /cavebot reload to apply.",
			wpType, pos.x, pos.y, pos.z, seq, sourceName, wpCount, wpCount + 1))
		return true
	end

	-- Global command: routedel <town> <src~dst> <seq>
	local routedelMatch = param:lower():match("^routedel%s+")
	if routedelMatch then
		local routeArgs = param:sub(#"routedel " + 1)
		local tokens = {}
		for token in routeArgs:gmatch("%S+") do tokens[#tokens + 1] = token end

		if #tokens < 3 then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot routedel <town> <src~dst> <seq>")
			player:sendTextMessage(MESSAGE_STATUS, "Example: /cavebot routedel darashia depot~boat 3")
			return true
		end

		local townName = tokens[1]
		local town = findTown(townName)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townName))
			return true
		end
		local townId = town:getId()
		townName = town:getName()

		local routeSpec = tokens[2]
		local src, dst = routeSpec:match("^([^~]+)~(.+)$")
		if not src or not dst then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid route format. Use: src~dst (e.g. depot~boat)")
			return true
		end

		local seq = tonumber(tokens[3])
		if not seq then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid seq number: " .. tokens[3])
			return true
		end

		local sourceLike = "%|" .. src .. "~" .. dst .. ":"
		local query = string.format(
			"SELECT `id`, `source_name` FROM `bot_city_routes` WHERE `town_id` = %d AND `source_name` LIKE %s AND `enabled` = 1",
			townId, db.escapeString(sourceLike))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No enabled route '%s~%s' found for %s (town %d).", src, dst, townName, townId))
			return true
		end

		local routeId = Result.getNumber(result, "id")
		local sourceName = Result.getString(result, "source_name")
		Result.free(result)

		local wpQuery = string.format(
			"SELECT `pos_x`, `pos_y`, `pos_z`, `waypoint_type` FROM `bot_city_route_waypoints` WHERE `route_id` = %d AND `seq` = %d",
			routeId, seq)
		local wpResult = db.storeQuery(wpQuery)
		if not wpResult then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No waypoint at seq %d in route %s.", seq, sourceName))
			return true
		end
		local delInfo = string.format("[%s] (%d,%d,%d)",
			Result.getString(wpResult, "waypoint_type"),
			Result.getNumber(wpResult, "pos_x"),
			Result.getNumber(wpResult, "pos_y"),
			Result.getNumber(wpResult, "pos_z"))
		Result.free(wpResult)

		local countQuery = string.format("SELECT COUNT(*) as cnt FROM `bot_city_route_waypoints` WHERE `route_id` = %d", routeId)
		local countResult = db.storeQuery(countQuery)
		local wpCount = 0
		if countResult then
			wpCount = Result.getNumber(countResult, "cnt")
			Result.free(countResult)
		end

		db.query(string.format("DELETE FROM `bot_city_route_waypoints` WHERE `route_id` = %d AND `seq` = %d", routeId, seq))
		db.query(string.format("UPDATE `bot_city_route_waypoints` SET `seq` = `seq` - 1 WHERE `route_id` = %d AND `seq` > %d", routeId, seq))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Deleted wp %s at seq %d from %s (was %d, now %d). /cavebot reload to apply.",
			delInfo, seq, sourceName, wpCount, wpCount - 1))
		return true
	end

	-- ================================================================
	-- POI Commands: poi, poiadd, poidel, poiupdate
	-- ================================================================

	-- Global command: poiadd <town> "<name>" <type> [x,y,z]
	local poiaddMatch = param:lower():match("^poiadd%s+")
	if poiaddMatch then
		local poiArgs = param:sub(#"poiadd " + 1)
		-- Parse: <town> "<name>" <type> [x,y,z]  OR  <town> <name> <type> [x,y,z]
		local tokens = {}
		-- First token is town name
		local rest = poiArgs
		local townToken = rest:match("^(%S+)")
		if not townToken then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poiadd <town> "<name>" <type> [x,y,z]')
			return true
		end
		rest = rest:sub(#townToken + 1):match("^%s*(.-)%s*$")

		local town = findTown(townToken)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townToken))
			return true
		end
		local townId = town:getId()

		-- Parse quoted name or single-word name
		local poiName, afterName
		if rest:sub(1,1) == '"' then
			poiName, afterName = rest:match('^"([^"]+)"%s*(.*)')
		end
		if not poiName then
			poiName = rest:match("^(%S+)")
			afterName = rest:sub(#poiName + 1):match("^%s*(.-)%s*$")
		end
		if not poiName or poiName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poiadd <town> "<name>" <type> [x,y,z]')
			return true
		end

		-- Parse type
		local validTypes = {depot=true, temple=true, boat=true, shop=true, npc=true}
		local remainTokens = {}
		for t in (afterName or ""):gmatch("%S+") do remainTokens[#remainTokens + 1] = t end

		if #remainTokens < 1 or not validTypes[remainTokens[1]:lower()] then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid or missing type. Valid: depot, temple, boat, shop, npc")
			return true
		end
		local poiType = remainTokens[1]:lower()

		-- Parse optional x,y,z or use admin position
		local pos = player:getPosition()
		if #remainTokens >= 2 then
			local cx, cy, cz = remainTokens[2]:match("^(%d+),(%d+),(%d+)$")
			if cx then
				pos = {x = tonumber(cx), y = tonumber(cy), z = tonumber(cz)}
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid coordinates '" .. remainTokens[2] .. "'. Use: x,y,z")
				return true
			end
		end

		db.query(string.format(
			"INSERT INTO `bot_city_pois` (`town_id`, `name`, `pos_x`, `pos_y`, `pos_z`, `poi_type`) VALUES (%d, %s, %d, %d, %d, %s)",
			townId, db.escapeString(poiName), pos.x, pos.y, pos.z, db.escapeString(poiType)))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Added POI '%s' [%s] at (%d,%d,%d) for %s. /cavebot reload to apply.",
			poiName, poiType, pos.x, pos.y, pos.z, town:getName()))
		return true
	end

	-- Global command: poidel <town> "<name>"
	local poidelMatch = param:lower():match("^poidel%s+")
	if poidelMatch then
		local poiArgs = param:sub(#"poidel " + 1)
		local townToken = poiArgs:match("^(%S+)")
		if not townToken then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poidel <town> "<name>"')
			return true
		end
		local rest = poiArgs:sub(#townToken + 1):match("^%s*(.-)%s*$")

		local town = findTown(townToken)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townToken))
			return true
		end
		local townId = town:getId()

		-- Parse quoted or unquoted name
		local poiName
		if rest:sub(1,1) == '"' then
			poiName = rest:match('^"([^"]+)"')
		end
		if not poiName then
			poiName = rest:match("^(.+)$")
		end
		if not poiName or poiName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poidel <town> "<name>"')
			return true
		end
		poiName = poiName:match("^%s*(.-)%s*$")

		local checkQuery = string.format(
			"SELECT `id` FROM `bot_city_pois` WHERE `town_id` = %d AND `name` = %s",
			townId, db.escapeString(poiName))
		local result = db.storeQuery(checkQuery)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] POI '%s' not found in %s.", poiName, town:getName()))
			return true
		end
		Result.free(result)

		db.query(string.format(
			"DELETE FROM `bot_city_pois` WHERE `town_id` = %d AND `name` = %s",
			townId, db.escapeString(poiName)))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Deleted POI '%s' from %s. /cavebot reload to apply.", poiName, town:getName()))
		return true
	end

	-- Global command: poiupdate <town> "<name>" [x,y,z]
	local poiupdateMatch = param:lower():match("^poiupdate%s+")
	if poiupdateMatch then
		local poiArgs = param:sub(#"poiupdate " + 1)
		local townToken = poiArgs:match("^(%S+)")
		if not townToken then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poiupdate <town> "<name>" [x,y,z]')
			return true
		end
		local rest = poiArgs:sub(#townToken + 1):match("^%s*(.-)%s*$")

		local town = findTown(townToken)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townToken))
			return true
		end
		local townId = town:getId()

		-- Parse quoted or unquoted name
		local poiName, afterName
		if rest:sub(1,1) == '"' then
			poiName, afterName = rest:match('^"([^"]+)"%s*(.*)')
		end
		if not poiName then
			poiName = rest:match("^(%S+)")
			afterName = rest:sub(#(poiName or "") + 1):match("^%s*(.-)%s*$")
		end
		if not poiName or poiName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot poiupdate <town> "<name>" [x,y,z]')
			return true
		end

		-- Parse optional x,y,z or use admin position
		local pos = player:getPosition()
		if afterName and afterName ~= "" then
			local cx, cy, cz = afterName:match("^(%d+),(%d+),(%d+)$")
			if cx then
				pos = {x = tonumber(cx), y = tonumber(cy), z = tonumber(cz)}
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid coordinates '" .. afterName .. "'. Use: x,y,z")
				return true
			end
		end

		local checkQuery = string.format(
			"SELECT `id` FROM `bot_city_pois` WHERE `town_id` = %d AND `name` = %s",
			townId, db.escapeString(poiName))
		local result = db.storeQuery(checkQuery)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] POI '%s' not found in %s.", poiName, town:getName()))
			return true
		end
		Result.free(result)

		db.query(string.format(
			"UPDATE `bot_city_pois` SET `pos_x` = %d, `pos_y` = %d, `pos_z` = %d WHERE `town_id` = %d AND `name` = %s",
			pos.x, pos.y, pos.z, townId, db.escapeString(poiName)))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Updated POI '%s' to (%d,%d,%d) in %s. /cavebot reload to apply.",
			poiName, pos.x, pos.y, pos.z, town:getName()))
		return true
	end

	-- Global command: poi <town>
	local poiMatch = param:lower():match("^poi%s+")
	if poiMatch then
		local poiArgs = param:sub(#"poi " + 1)
		local townToken = poiArgs:match("^(%S+)")
		if not townToken then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot poi <town>")
			return true
		end

		local town = findTown(townToken)
		if not town then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townToken))
			return true
		end
		local townId = town:getId()

		local query = string.format(
			"SELECT `name`, `pos_x`, `pos_y`, `pos_z`, `poi_type`, `weight`, `enabled` "
			.. "FROM `bot_city_pois` WHERE `town_id` = %d ORDER BY `poi_type`, `name`", townId)
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No POIs for %s (town %d).", town:getName(), townId))
			return true
		end

		local lines = {}
		repeat
			local name = Result.getString(result, "name")
			local poiType = Result.getString(result, "poi_type")
			local enabled = Result.getNumber(result, "enabled")
			local flag = enabled == 1 and "" or " [DISABLED]"
			lines[#lines + 1] = string.format("  %s [%s] (%d,%d,%d)%s",
				name, poiType,
				Result.getNumber(result, "pos_x"),
				Result.getNumber(result, "pos_y"),
				Result.getNumber(result, "pos_z"),
				flag)
		until not Result.next(result)
		Result.free(result)

		player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] POIs for %s (town %d): %d found", town:getName(), townId, #lines))
		local batch = {}
		for i, r in ipairs(lines) do
			batch[#batch + 1] = r
			if #batch >= 10 or i == #lines then
				player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
				batch = {}
			end
		end
		return true
	end

	-- ================================================================
	-- Simulate Commands: teleport admin through waypoints
	-- ================================================================

	local simMatch = param:lower():match("^simulate%s+")
	if simMatch then
		local simArgs = param:sub(#"simulate " + 1)
		local subCmd = simArgs:match("^(%S+)")
		if not subCmd then
			player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate route|hunt|poi|pause|continue|stop")
			return true
		end
		subCmd = subCmd:lower()

		-- Simulation state table (persists across calls via upvalue)
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

		if subCmd == "route" then
			local rest = simArgs:sub(#"route " + 1):match("^%s*(.-)%s*$")
			local tokens = {}
			for t in rest:gmatch("%S+") do tokens[#tokens + 1] = t end

			if #tokens < 2 then
				player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate route <town> <src~dst>")
				return true
			end

			local town = findTown(tokens[1])
			if not town then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", tokens[1]))
				return true
			end
			local townId = town:getId()

			local src, dst = tokens[2]:match("^([^~]+)~(.+)$")
			if not src or not dst then
				player:sendTextMessage(MESSAGE_STATUS, "Invalid route format. Use: src~dst (e.g. depot~boat)")
				return true
			end

			local sourceLike = "%|" .. src:lower() .. "~" .. dst:lower() .. ":"
			local query = string.format(
				"SELECT `id`, `source_name` FROM `bot_city_routes` WHERE `town_id` = %d AND `source_name` LIKE %s AND `enabled` = 1",
				townId, db.escapeString(sourceLike))
			local result = db.storeQuery(query)
			if not result then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] No route '%s~%s' for %s.", src, dst, town:getName()))
				return true
			end
			local routeId = Result.getNumber(result, "id")
			local sourceName = Result.getString(result, "source_name")
			Result.free(result)

			local wpQuery = string.format(
				"SELECT `pos_x`, `pos_y`, `pos_z`, `waypoint_type` FROM `bot_city_route_waypoints` WHERE `route_id` = %d ORDER BY `seq` ASC",
				routeId)
			local wpResult = db.storeQuery(wpQuery)
			if not wpResult then
				player:sendTextMessage(MESSAGE_STATUS, "[sim] Route has no waypoints.")
				return true
			end

			local wps = {}
			repeat
				wps[#wps + 1] = {
					x = Result.getNumber(wpResult, "pos_x"),
					y = Result.getNumber(wpResult, "pos_y"),
					z = Result.getNumber(wpResult, "pos_z"),
					wpType = Result.getString(wpResult, "waypoint_type"),
				}
			until not Result.next(wpResult)
			Result.free(wpResult)

			-- Stop any existing simulation
			local oldSim = SimulationState[player:getId()]
			if oldSim and oldSim.eventId then stopEvent(oldSim.eventId) end

			SimulationState[player:getId()] = {
				waypoints = wps,
				currentIdx = 0,
				paused = false,
				type = "route",
				label = string.format("route %s (%s)", sourceName, town:getName()),
			}
			player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] Starting route simulation: %s (%d waypoints)", sourceName, #wps))
			SimulationState[player:getId()].eventId = addEvent(simulationTick, 1000, player:getId())
			return true
		end

		if subCmd == "hunt" then
			local rest = simArgs:sub(#"hunt " + 1):match("^%s*(.-)%s*$")

			-- Parse quoted hunt name or single-word
			local huntName, phase
			if rest:sub(1,1) == '"' then
				huntName, phase = rest:match('^"([^"]+)"%s*(%S*)')
			end
			if not huntName then
				local tokens = {}
				for t in rest:gmatch("%S+") do tokens[#tokens + 1] = t end
				if #tokens < 1 then
					player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot simulate hunt "<name>" [phase]')
					return true
				end
				huntName = tokens[1]
				phase = tokens[2]
			end

			if not phase or phase == "" then phase = "hunt_patrol" end
			local phaseMap = {patrol = "hunt_patrol", travel_to = "travel_to", travel = "travel_to", travel_from = "travel_from", leave = "travel_from"}
			phase = phaseMap[phase:lower()] or phase:lower()

			-- Find hunt script
			local scriptQuery = string.format(
				"SELECT `id`, `name` FROM `bot_hunt_scripts` WHERE `name` LIKE %s LIMIT 1",
				db.escapeString("%" .. huntName .. "%"))
			local scriptResult = db.storeQuery(scriptQuery)
			if not scriptResult then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] No hunt script matching '%s'.", huntName))
				return true
			end
			local scriptId = Result.getNumber(scriptResult, "id")
			local scriptName = Result.getString(scriptResult, "name")
			Result.free(scriptResult)

			local wpQuery = string.format(
				"SELECT `pos_x`, `pos_y`, `pos_z`, `waypoint_type` FROM `bot_hunt_waypoints` WHERE `script_id` = %d AND `phase` = %s ORDER BY `seq` ASC",
				scriptId, db.escapeString(phase))
			local wpResult = db.storeQuery(wpQuery)
			if not wpResult then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] No %s waypoints for '%s'.", phase, scriptName))
				return true
			end

			local wps = {}
			repeat
				wps[#wps + 1] = {
					x = Result.getNumber(wpResult, "pos_x"),
					y = Result.getNumber(wpResult, "pos_y"),
					z = Result.getNumber(wpResult, "pos_z"),
					wpType = Result.getString(wpResult, "waypoint_type"),
				}
			until not Result.next(wpResult)
			Result.free(wpResult)

			local oldSim = SimulationState[player:getId()]
			if oldSim and oldSim.eventId then stopEvent(oldSim.eventId) end

			SimulationState[player:getId()] = {
				waypoints = wps,
				currentIdx = 0,
				paused = false,
				type = "hunt",
				phase = phase,
				label = string.format("hunt '%s' %s", scriptName, phase),
			}
			player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] Starting hunt simulation: '%s' %s (%d waypoints)", scriptName, phase, #wps))
			SimulationState[player:getId()].eventId = addEvent(simulationTick, 1000, player:getId())
			return true
		end

		if subCmd == "poi" then
			local rest = simArgs:sub(#"poi " + 1):match("^%s*(.-)%s*$")
			local townToken = rest:match("^(%S+)")
			if not townToken then
				player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate poi <town>")
				return true
			end

			local town = findTown(townToken)
			if not town then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Unknown town '%s'.", townToken))
				return true
			end
			local townId = town:getId()

			local query = string.format(
				"SELECT `name`, `pos_x`, `pos_y`, `pos_z`, `poi_type` FROM `bot_city_pois` WHERE `town_id` = %d AND `enabled` = 1 ORDER BY `poi_type`, `name`",
				townId)
			local result = db.storeQuery(query)
			if not result then
				player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] No POIs for %s.", town:getName()))
				return true
			end

			local wps = {}
			repeat
				wps[#wps + 1] = {
					x = Result.getNumber(result, "pos_x"),
					y = Result.getNumber(result, "pos_y"),
					z = Result.getNumber(result, "pos_z"),
					name = Result.getString(result, "name"),
					poiType = Result.getString(result, "poi_type"),
				}
			until not Result.next(result)
			Result.free(result)

			local oldSim = SimulationState[player:getId()]
			if oldSim and oldSim.eventId then stopEvent(oldSim.eventId) end

			SimulationState[player:getId()] = {
				waypoints = wps,
				currentIdx = 0,
				paused = false,
				type = "poi",
				label = string.format("POIs in %s", town:getName()),
			}
			player:sendTextMessage(MESSAGE_STATUS, string.format("[sim] Starting POI simulation: %s (%d POIs)", town:getName(), #wps))
			SimulationState[player:getId()].eventId = addEvent(simulationTick, 1000, player:getId())
			return true
		end

		player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot simulate route|hunt|poi|pause|continue|stop")
		return true
	end

	-- Global command: huntwp <search> [phase]
	-- /cavebot huntwp pirates                     → list all hunt scripts matching "pirates"
	-- /cavebot huntwp "Pirates Yalahar"           → list phases + wp counts for that script
	-- /cavebot huntwp "Pirates Yalahar" patrol    → list patrol waypoints
	local huntwpMatch = param:lower():match("^huntwp%s+")
	if huntwpMatch then
		local huntwpArgs = param:sub(#"huntwp " + 1)
		-- Parse: quoted "Hunt Name" phase  OR  unquoted search [phase]
		local huntName, phase
		local quotedName, afterQuote = huntwpArgs:match('^"([^"]+)"%s*(.*)')
		if quotedName then
			huntName = quotedName
			phase = afterQuote ~= "" and afterQuote:match("^(%S+)") or nil
		else
			local tokens = {}
			for token in huntwpArgs:gmatch("%S+") do tokens[#tokens + 1] = token end
			if #tokens == 0 then
				player:sendTextMessage(MESSAGE_STATUS, "Usage: /cavebot huntwp <search> [phase]\n  Phases: travel_to, patrol (hunt_patrol), travel_from")
				return true
			end
			-- Last token might be a phase name
			local lastToken = tokens[#tokens]:lower()
			local phaseAliases = {travel_to=true, hunt_patrol=true, travel_from=true, patrol=true}
			if #tokens >= 2 and phaseAliases[lastToken] then
				phase = lastToken
				table.remove(tokens)
			end
			huntName = table.concat(tokens, " ")
		end

		-- Normalize phase alias
		if phase then
			phase = phase:lower()
			if phase == "patrol" then phase = "hunt_patrol" end
		end

		-- Search for matching scripts
		local searchLike = "%" .. huntName:gsub("%%", "%%%%") .. "%"
		local query = string.format(
			"SELECT `id`, `name`, `town_id`, `enabled` FROM `bot_hunt_scripts` WHERE `name` LIKE %s ORDER BY `name` LIMIT 50",
			db.escapeString(searchLike))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No hunt scripts matching '%s'.", huntName))
			return true
		end

		-- Collect matches
		local scripts = {}
		repeat
			scripts[#scripts + 1] = {
				id = Result.getNumber(result, "id"),
				name = Result.getString(result, "name"),
				townId = Result.getNumber(result, "town_id"),
				enabled = Result.getNumber(result, "enabled"),
			}
		until not Result.next(result)
		Result.free(result)

		-- Multiple matches and no phase: list matching scripts
		if #scripts > 1 or (not phase and #scripts == 1 and huntName:lower() ~= scripts[1].name:lower()) then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Hunt scripts matching '%s': %d found", huntName, #scripts))
			local batch = {}
			for i, s in ipairs(scripts) do
				-- Get wp counts per phase
				local wpQuery = string.format(
					"SELECT `phase`, COUNT(*) as cnt FROM `bot_hunt_waypoints` WHERE `script_id` = %d GROUP BY `phase`",
					s.id)
				local wpResult = db.storeQuery(wpQuery)
				local phaseCounts = {}
				if wpResult then
					repeat
						phaseCounts[Result.getString(wpResult, "phase")] = Result.getNumber(wpResult, "cnt")
					until not Result.next(wpResult)
					Result.free(wpResult)
				end
				local parts = {}
				for _, ph in ipairs({"travel_to", "hunt_patrol", "travel_from"}) do
					if phaseCounts[ph] then
						parts[#parts + 1] = string.format("%s:%d", ph, phaseCounts[ph])
					end
				end
				local enabled = s.enabled == 1 and "" or " [DISABLED]"
				batch[#batch + 1] = string.format("  [%d] %s (town %d) — %s%s", s.id, s.name, s.townId, table.concat(parts, ", "), enabled)
				if #batch >= 8 or i == #scripts then
					player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
					batch = {}
				end
			end
			return true
		end

		-- Single exact match (or close enough): show waypoints
		local script = scripts[1]

		if not phase then
			-- List all phases with counts
			local wpQuery = string.format(
				"SELECT `phase`, COUNT(*) as cnt FROM `bot_hunt_waypoints` WHERE `script_id` = %d GROUP BY `phase`",
				script.id)
			local wpResult = db.storeQuery(wpQuery)
			local phaseCounts = {}
			if wpResult then
				repeat
					phaseCounts[Result.getString(wpResult, "phase")] = Result.getNumber(wpResult, "cnt")
				until not Result.next(wpResult)
				Result.free(wpResult)
			end
			local enabled = script.enabled == 1 and "enabled" or "DISABLED"
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Hunt script [%d] '%s' (town %d, %s):",
				script.id, script.name, script.townId, enabled))
			for _, ph in ipairs({"travel_to", "hunt_patrol", "travel_from"}) do
				local cnt = phaseCounts[ph] or 0
				player:sendTextMessage(MESSAGE_STATUS, string.format("  %s: %d waypoints", ph, cnt))
			end
			player:sendTextMessage(MESSAGE_STATUS, "Use: /cavebot huntwp \"" .. script.name .. "\" patrol")
			return true
		end

		-- List waypoints for specific phase
		local wpQuery = string.format(
			"SELECT `seq`, `pos_x`, `pos_y`, `pos_z`, `waypoint_type`, `label` FROM `bot_hunt_waypoints` WHERE `script_id` = %d AND `phase` = %s ORDER BY `seq` ASC",
			script.id, db.escapeString(phase))
		local wpResult = db.storeQuery(wpQuery)
		if not wpResult then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No %s waypoints for '%s'.", phase, script.name))
			return true
		end

		player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] %s waypoints for [%d] '%s':", phase, script.id, script.name))
		local batch = {}
		repeat
			local seq = Result.getNumber(wpResult, "seq")
			local wx = Result.getNumber(wpResult, "pos_x")
			local wy = Result.getNumber(wpResult, "pos_y")
			local wz = Result.getNumber(wpResult, "pos_z")
			local wt = Result.getString(wpResult, "waypoint_type")
			local lbl = Result.getString(wpResult, "label")
			local extra = (lbl and lbl ~= "") and (" '" .. lbl .. "'") or ""
			batch[#batch + 1] = string.format("  %d: (%d,%d,%d) %s%s", seq, wx, wy, wz, wt, extra)
			if #batch >= 10 then
				player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
				batch = {}
			end
		until not Result.next(wpResult)
		Result.free(wpResult)
		if #batch > 0 then
			player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
		end
		return true
	end

	-- Global command: huntadd <hunt_name> <phase> <seq> [type] [x,y,z]
	-- /cavebot huntadd "Pirates Yalahar" patrol 5
	-- /cavebot huntadd "Pirates Yalahar" patrol 5 door
	-- /cavebot huntadd "Pirates Yalahar" patrol 5 stand 32640,31301,6
	local huntaddMatch = param:lower():match("^huntadd%s+")
	if huntaddMatch then
		local huntaddArgs = param:sub(#"huntadd " + 1)
		-- Parse: "Hunt Name" phase seq [type] [x,y,z]  OR  HuntName phase seq [type] [x,y,z]
		local huntName, restArgs
		local quotedName, afterQuote = huntaddArgs:match('^"([^"]+)"%s+(.*)')
		if quotedName then
			huntName = quotedName
			restArgs = afterQuote
		else
			-- Everything before the first phase keyword is the hunt name
			-- We need at least: name phase seq
			local tokens = {}
			for token in huntaddArgs:gmatch("%S+") do tokens[#tokens + 1] = token end
			if #tokens < 3 then
				player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntadd "<hunt name>" <phase> <seq> [type] [x,y,z]')
				player:sendTextMessage(MESSAGE_STATUS, "  Phases: travel_to, patrol (hunt_patrol), travel_from")
				player:sendTextMessage(MESSAGE_STATUS, "  Types: stand (default), node, ladder, rope, hole, stairs_up, stairs_down, door")
				return true
			end
			-- Find the phase token (scan from end backward to find first phase keyword)
			local phaseAliases = {travel_to=true, hunt_patrol=true, travel_from=true, patrol=true}
			local phaseIdx = nil
			for i = 1, #tokens do
				if phaseAliases[tokens[i]:lower()] and i < #tokens then
					phaseIdx = i
					break
				end
			end
			if not phaseIdx or phaseIdx >= #tokens then
				player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntadd "<hunt name>" <phase> <seq> [type] [x,y,z]')
				return true
			end
			huntName = table.concat(tokens, " ", 1, phaseIdx - 1)
			restArgs = table.concat(tokens, " ", phaseIdx)
		end

		local restTokens = {}
		for token in restArgs:gmatch("%S+") do restTokens[#restTokens + 1] = token end

		if #restTokens < 2 then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntadd "<hunt name>" <phase> <seq> [type] [x,y,z]')
			return true
		end

		local phase = restTokens[1]:lower()
		if phase == "patrol" then phase = "hunt_patrol" end
		local validPhases = {travel_to=true, hunt_patrol=true, travel_from=true}
		if not validPhases[phase] then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid phase '" .. restTokens[1] .. "'. Valid: travel_to, patrol, travel_from")
			return true
		end

		local seq = tonumber(restTokens[2])
		if not seq then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid seq number: " .. restTokens[2])
			return true
		end

		-- Parse optional [type] [x,y,z]
		local wpType = "stand"
		local wpX, wpY, wpZ = nil, nil, nil
		local validTypes = {stand=true, node=true, ladder=true, rope=true, hole=true, shovel=true, stairs_up=true, stairs_down=true, door=true, lever=true, levitate_up=true, levitate_down=true, label=true, conditional=true}

		if #restTokens == 3 then
			local cx, cy, cz = restTokens[3]:match("^(%d+),(%d+),(%d+)$")
			if cx then
				wpX, wpY, wpZ = tonumber(cx), tonumber(cy), tonumber(cz)
			elseif validTypes[restTokens[3]:lower()] then
				wpType = restTokens[3]:lower()
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid type '" .. restTokens[3] .. "'.")
				return true
			end
		elseif #restTokens == 4 then
			if validTypes[restTokens[3]:lower()] then
				wpType = restTokens[3]:lower()
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid type '" .. restTokens[3] .. "'.")
				return true
			end
			local cx, cy, cz = restTokens[4]:match("^(%d+),(%d+),(%d+)$")
			if cx then
				wpX, wpY, wpZ = tonumber(cx), tonumber(cy), tonumber(cz)
			else
				player:sendTextMessage(MESSAGE_STATUS, "Invalid coordinates '" .. restTokens[4] .. "'. Use: x,y,z")
				return true
			end
		end

		if wpType == "shovel" then wpType = "hole" end  -- normalize alias

		local pos = player:getPosition()
		if wpX then pos = {x = wpX, y = wpY, z = wpZ} end

		-- Find the hunt script
		local query = string.format(
			"SELECT `id`, `name` FROM `bot_hunt_scripts` WHERE `name` LIKE %s ORDER BY `name` LIMIT 2",
			db.escapeString("%" .. huntName .. "%"))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No hunt script matching '%s'.", huntName))
			return true
		end

		local scriptId = Result.getNumber(result, "id")
		local scriptName = Result.getString(result, "name")
		local hasMore = Result.next(result)
		Result.free(result)

		if hasMore and huntName:lower() ~= scriptName:lower() then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Multiple scripts match '%s'. Use exact name in quotes.", huntName))
			return true
		end

		-- Get current count
		local countQuery = string.format(
			"SELECT COUNT(*) as cnt FROM `bot_hunt_waypoints` WHERE `script_id` = %d AND `phase` = %s",
			scriptId, db.escapeString(phase))
		local countResult = db.storeQuery(countQuery)
		local wpCount = 0
		if countResult then
			wpCount = Result.getNumber(countResult, "cnt")
			Result.free(countResult)
		end

		-- Shift existing waypoints and insert
		db.query(string.format(
			"UPDATE `bot_hunt_waypoints` SET `seq` = `seq` + 1 WHERE `script_id` = %d AND `phase` = %s AND `seq` >= %d",
			scriptId, db.escapeString(phase), seq))
		db.query(string.format(
			"INSERT INTO `bot_hunt_waypoints` (`script_id`, `phase`, `seq`, `waypoint_type`, `pos_x`, `pos_y`, `pos_z`) VALUES (%d, %s, %d, %s, %d, %d, %d)",
			scriptId, db.escapeString(phase), seq, db.escapeString(wpType), pos.x, pos.y, pos.z))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Added [%s] (%d,%d,%d) at %s seq %d for '%s' (was %d, now %d). /cavebot reload to apply.",
			wpType, pos.x, pos.y, pos.z, phase, seq, scriptName, wpCount, wpCount + 1))
		return true
	end

	-- Global command: huntdel <hunt_name> <phase> <seq>
	-- /cavebot huntdel "Pirates Yalahar" patrol 19
	local huntdelMatch = param:lower():match("^huntdel%s+")
	if huntdelMatch then
		local huntdelArgs = param:sub(#"huntdel " + 1)
		local huntName, restArgs
		local quotedName, afterQuote = huntdelArgs:match('^"([^"]+)"%s+(.*)')
		if quotedName then
			huntName = quotedName
			restArgs = afterQuote
		else
			local tokens = {}
			for token in huntdelArgs:gmatch("%S+") do tokens[#tokens + 1] = token end
			if #tokens < 3 then
				player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntdel "<hunt name>" <phase> <seq>')
				player:sendTextMessage(MESSAGE_STATUS, "  Phases: travel_to, patrol (hunt_patrol), travel_from")
				return true
			end
			local phaseAliases = {travel_to=true, hunt_patrol=true, travel_from=true, patrol=true}
			local phaseIdx = nil
			for i = 1, #tokens do
				if phaseAliases[tokens[i]:lower()] and i < #tokens then
					phaseIdx = i
					break
				end
			end
			if not phaseIdx or phaseIdx >= #tokens then
				player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntdel "<hunt name>" <phase> <seq>')
				return true
			end
			huntName = table.concat(tokens, " ", 1, phaseIdx - 1)
			restArgs = table.concat(tokens, " ", phaseIdx)
		end

		local restTokens = {}
		for token in restArgs:gmatch("%S+") do restTokens[#restTokens + 1] = token end

		if #restTokens < 2 then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot huntdel "<hunt name>" <phase> <seq>')
			return true
		end

		local phase = restTokens[1]:lower()
		if phase == "patrol" then phase = "hunt_patrol" end
		local validPhases = {travel_to=true, hunt_patrol=true, travel_from=true}
		if not validPhases[phase] then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid phase '" .. restTokens[1] .. "'. Valid: travel_to, patrol, travel_from")
			return true
		end

		local seq = tonumber(restTokens[2])
		if not seq then
			player:sendTextMessage(MESSAGE_STATUS, "Invalid seq number: " .. restTokens[2])
			return true
		end

		-- Find the hunt script
		local query = string.format(
			"SELECT `id`, `name` FROM `bot_hunt_scripts` WHERE `name` LIKE %s ORDER BY `name` LIMIT 2",
			db.escapeString("%" .. huntName .. "%"))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No hunt script matching '%s'.", huntName))
			return true
		end

		local scriptId = Result.getNumber(result, "id")
		local scriptName = Result.getString(result, "name")
		local hasMore = Result.next(result)
		Result.free(result)

		if hasMore and huntName:lower() ~= scriptName:lower() then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Multiple scripts match '%s'. Use exact name in quotes.", huntName))
			return true
		end

		-- Verify waypoint exists
		local wpQuery = string.format(
			"SELECT `pos_x`, `pos_y`, `pos_z`, `waypoint_type`, `label` FROM `bot_hunt_waypoints` WHERE `script_id` = %d AND `phase` = %s AND `seq` = %d",
			scriptId, db.escapeString(phase), seq)
		local wpResult = db.storeQuery(wpQuery)
		if not wpResult then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No waypoint at %s seq %d for '%s'.", phase, seq, scriptName))
			return true
		end
		local delInfo = string.format("[%s] (%d,%d,%d)",
			Result.getString(wpResult, "waypoint_type"),
			Result.getNumber(wpResult, "pos_x"),
			Result.getNumber(wpResult, "pos_y"),
			Result.getNumber(wpResult, "pos_z"))
		Result.free(wpResult)

		-- Get current count
		local countQuery = string.format(
			"SELECT COUNT(*) as cnt FROM `bot_hunt_waypoints` WHERE `script_id` = %d AND `phase` = %s",
			scriptId, db.escapeString(phase))
		local countResult = db.storeQuery(countQuery)
		local wpCount = 0
		if countResult then
			wpCount = Result.getNumber(countResult, "cnt")
			Result.free(countResult)
		end

		-- Delete and shift
		db.query(string.format(
			"DELETE FROM `bot_hunt_waypoints` WHERE `script_id` = %d AND `phase` = %s AND `seq` = %d",
			scriptId, db.escapeString(phase), seq))
		db.query(string.format(
			"UPDATE `bot_hunt_waypoints` SET `seq` = `seq` - 1 WHERE `script_id` = %d AND `phase` = %s AND `seq` > %d",
			scriptId, db.escapeString(phase), seq))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Deleted %s at %s seq %d from '%s' (was %d, now %d). /cavebot reload to apply.",
			delInfo, phase, seq, scriptName, wpCount, wpCount - 1))
		return true
	end

	-- Global command: hunttarget <search>
	-- /cavebot hunttarget wyrm               → list all scripts matching "wyrm" with target counts
	-- /cavebot hunttarget "Wyrm Liberty Bay"  → list targets for that specific script
	local hunttargetMatch = param:lower():match("^hunttarget%s+")
	if hunttargetMatch then
		local hunttargetArgs = param:sub(#"hunttarget " + 1)
		local huntName
		local quotedName = hunttargetArgs:match('^"([^"]+)"')
		if quotedName then
			huntName = quotedName
		else
			huntName = hunttargetArgs:match("^(.+)$")
		end
		if not huntName or huntName:match("^%s*$") then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot hunttarget <search>\n  /cavebot hunttarget "Wyrm Liberty Bay"')
			return true
		end
		huntName = huntName:match("^%s*(.-)%s*$") -- trim

		-- Search for matching scripts
		local searchLike = "%" .. huntName:gsub("%%", "%%%%") .. "%"
		local query = string.format(
			"SELECT `id`, `name`, `enabled` FROM `bot_hunt_scripts` WHERE `name` LIKE %s ORDER BY `name` LIMIT 50",
			db.escapeString(searchLike))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No hunt scripts matching '%s'.", huntName))
			return true
		end

		local scripts = {}
		repeat
			scripts[#scripts + 1] = {
				id = Result.getNumber(result, "id"),
				name = Result.getString(result, "name"),
				enabled = Result.getNumber(result, "enabled"),
			}
		until not Result.next(result)
		Result.free(result)

		-- Multiple matches: list scripts with target counts
		if #scripts > 1 or (#scripts == 1 and huntName:lower() ~= scripts[1].name:lower()) then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Hunt scripts matching '%s': %d found", huntName, #scripts))
			local batch = {}
			for i, s in ipairs(scripts) do
				local tQuery = string.format(
					"SELECT COUNT(*) as cnt FROM `bot_hunt_targets` WHERE `script_id` = %d", s.id)
				local tResult = db.storeQuery(tQuery)
				local tCount = 0
				if tResult then
					tCount = Result.getNumber(tResult, "cnt")
					Result.free(tResult)
				end
				local enabled = s.enabled == 1 and "" or " [DISABLED]"
				batch[#batch + 1] = string.format("  [%d] %s — %d targets%s", s.id, s.name, tCount, enabled)
				if #batch >= 8 or i == #scripts then
					player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
					batch = {}
				end
			end
			return true
		end

		-- Single exact match: show targets
		local script = scripts[1]
		local tQuery = string.format(
			"SELECT `monster_name`, `priority`, `count` FROM `bot_hunt_targets` WHERE `script_id` = %d ORDER BY `priority` DESC, `monster_name` ASC",
			script.id)
		local tResult = db.storeQuery(tQuery)
		if not tResult then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No targets for [%d] '%s'.", script.id, script.name))
			return true
		end

		local enabled = script.enabled == 1 and "enabled" or "DISABLED"
		player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Targets for [%d] '%s' (%s):", script.id, script.name, enabled))
		local batch = {}
		repeat
			local name = Result.getString(tResult, "monster_name")
			local priority = Result.getNumber(tResult, "priority")
			local count = Result.getNumber(tResult, "count")
			batch[#batch + 1] = string.format("  %s (priority: %d, count: %d)", name, priority, count)
			if #batch >= 10 then
				player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
				batch = {}
			end
		until not Result.next(tResult)
		Result.free(tResult)
		if #batch > 0 then
			player:sendTextMessage(MESSAGE_STATUS, table.concat(batch, "\n"))
		end
		return true
	end

	-- Global command: targetadd "<hunt name>" <monster_name> [priority] [count]
	-- /cavebot targetadd "Wyrm Liberty Bay" Elder Wyrm
	-- /cavebot targetadd "Wyrm Liberty Bay" Elder Wyrm 5 2
	local targetaddMatch = param:lower():match("^targetadd%s+")
	if targetaddMatch then
		local targetaddArgs = param:sub(#"targetadd " + 1)
		local huntName, monsterName
		local quotedName, afterQuote = targetaddArgs:match('^"([^"]+)"%s+(.*)')
		if quotedName then
			huntName = quotedName
			monsterName = afterQuote
		else
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot targetadd "<hunt name>" <monster name> [priority] [count]')
			return true
		end

		if not monsterName or monsterName:match("^%s*$") then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot targetadd "<hunt name>" <monster name> [priority] [count]')
			return true
		end
		monsterName = monsterName:match("^%s*(.-)%s*$") -- trim

		-- Parse optional priority and count from end of monsterName
		local priority = 1
		local count = 1
		-- Check if last two tokens are numbers (priority count)
		local tokens = {}
		for token in monsterName:gmatch("%S+") do tokens[#tokens + 1] = token end
		if #tokens >= 3 and tonumber(tokens[#tokens]) and tonumber(tokens[#tokens - 1]) then
			count = tonumber(tokens[#tokens])
			priority = tonumber(tokens[#tokens - 1])
			table.remove(tokens)
			table.remove(tokens)
			monsterName = table.concat(tokens, " ")
		elseif #tokens >= 2 and tonumber(tokens[#tokens]) then
			priority = tonumber(tokens[#tokens])
			table.remove(tokens)
			monsterName = table.concat(tokens, " ")
		end

		if monsterName == "" then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot targetadd "<hunt name>" <monster name> [priority] [count]')
			return true
		end

		-- Find the hunt script
		local query = string.format(
			"SELECT `id`, `name` FROM `bot_hunt_scripts` WHERE `name` LIKE %s ORDER BY `name` LIMIT 2",
			db.escapeString("%" .. huntName .. "%"))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No hunt script matching '%s'.", huntName))
			return true
		end

		local scriptId = Result.getNumber(result, "id")
		local scriptName = Result.getString(result, "name")
		local hasMore = Result.next(result)
		Result.free(result)

		if hasMore and huntName:lower() ~= scriptName:lower() then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Multiple scripts match '%s'. Use exact name in quotes.", huntName))
			return true
		end

		-- Check duplicate
		local dupQuery = string.format(
			"SELECT 1 FROM `bot_hunt_targets` WHERE `script_id` = %d AND `monster_name` = %s",
			scriptId, db.escapeString(monsterName))
		local dupResult = db.storeQuery(dupQuery)
		if dupResult then
			Result.free(dupResult)
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Target '%s' already exists for '%s'.", monsterName, scriptName))
			return true
		end

		-- Insert
		db.query(string.format(
			"INSERT INTO `bot_hunt_targets` (`script_id`, `monster_name`, `priority`, `count`) VALUES (%d, %s, %d, %d)",
			scriptId, db.escapeString(monsterName), priority, count))

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Added target '%s' (priority: %d, count: %d) to '%s'. /cavebot reload to apply.",
			monsterName, priority, count, scriptName))
		return true
	end

	-- Global command: targetdel "<hunt name>" <monster_name>
	-- /cavebot targetdel "Wyrm Liberty Bay" Elder Wyrm
	local targetdelMatch = param:lower():match("^targetdel%s+")
	if targetdelMatch then
		local targetdelArgs = param:sub(#"targetdel " + 1)
		local huntName, monsterName
		local quotedName, afterQuote = targetdelArgs:match('^"([^"]+)"%s+(.*)')
		if quotedName then
			huntName = quotedName
			monsterName = afterQuote
		else
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot targetdel "<hunt name>" <monster name>')
			return true
		end

		if not monsterName or monsterName:match("^%s*$") then
			player:sendTextMessage(MESSAGE_STATUS, 'Usage: /cavebot targetdel "<hunt name>" <monster name>')
			return true
		end
		monsterName = monsterName:match("^%s*(.-)%s*$") -- trim

		-- Find the hunt script
		local query = string.format(
			"SELECT `id`, `name` FROM `bot_hunt_scripts` WHERE `name` LIKE %s ORDER BY `name` LIMIT 2",
			db.escapeString("%" .. huntName .. "%"))
		local result = db.storeQuery(query)
		if not result then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] No hunt script matching '%s'.", huntName))
			return true
		end

		local scriptId = Result.getNumber(result, "id")
		local scriptName = Result.getString(result, "name")
		local hasMore = Result.next(result)
		Result.free(result)

		if hasMore and huntName:lower() ~= scriptName:lower() then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Multiple scripts match '%s'. Use exact name in quotes.", huntName))
			return true
		end

		-- Verify target exists
		local existQuery = string.format(
			"SELECT `monster_name` FROM `bot_hunt_targets` WHERE `script_id` = %d AND `monster_name` = %s",
			scriptId, db.escapeString(monsterName))
		local existResult = db.storeQuery(existQuery)
		if not existResult then
			player:sendTextMessage(MESSAGE_STATUS, string.format("[cavebot] Target '%s' not found in '%s'.", monsterName, scriptName))
			return true
		end
		Result.free(existResult)

		-- Delete
		db.query(string.format(
			"DELETE FROM `bot_hunt_targets` WHERE `script_id` = %d AND `monster_name` = %s",
			scriptId, db.escapeString(monsterName)))

		-- Get remaining count
		local countQuery = string.format(
			"SELECT COUNT(*) as cnt FROM `bot_hunt_targets` WHERE `script_id` = %d", scriptId)
		local countResult = db.storeQuery(countQuery)
		local remaining = 0
		if countResult then
			remaining = Result.getNumber(countResult, "cnt")
			Result.free(countResult)
		end

		player:sendTextMessage(MESSAGE_STATUS, string.format(
			"[cavebot] Deleted target '%s' from '%s' (%d targets remaining). /cavebot reload to apply.",
			monsterName, scriptName, remaining))
		return true
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
