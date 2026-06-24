-- /botspawn command: teleport to a hunting spawn's first patrol waypoint
-- Usage:
--   /botspawn <id|name>        — teleport to spawn by script ID or name (substring match)
--   /botspawn list [filter]    — list available spawns, optionally filtered by name
--
-- Supports spawn names with special characters and spaces.
-- Examples:
--   /botspawn 42
--   /botspawn darashia mino
--   /botspawn oramond glooth
--   /botspawn list darashia

local spawnCmd = TalkAction("/botspawn")

function spawnCmd.onSay(player, words, param)
	if player:getGroup():getAccess() == false then
		player:sendTextMessage(MESSAGE_STATUS_WARNING, "You need God access to use /botspawn.")
		return true
	end

	if not param or param == "" then
		player:sendTextMessage(MESSAGE_STATUS, "Usage: /botspawn <id|name> — teleport to hunt spawn start")
		player:sendTextMessage(MESSAGE_STATUS, "       /botspawn list [filter] — list available spawns")
		return true
	end

	-- Check for "list" subcommand
	local listFilter = param:match("^list%s*(.*)")
	if listFilter ~= nil then
		local filter = listFilter ~= "" and listFilter:lower() or nil
		local results = {}
		for id, script in pairs(BotHuntData.scripts or {}) do
			if not filter or (script.name and script.name:lower():find(filter, 1, true)) then
				local wps = BotHuntData.getPhaseWaypoints(id, "hunt_patrol")
				local hasWps = wps and #wps > 0
				table.insert(results, {
					id = id,
					name = script.name or "?",
					town = script.town_name or "?",
					hasWps = hasWps,
				})
			end
		end
		table.sort(results, function(a, b) return a.id < b.id end)

		if #results == 0 then
			player:sendTextMessage(MESSAGE_STATUS, "No spawns found" .. (filter and (" matching '" .. filter .. "'") or "") .. ".")
			return true
		end

		-- Show in chunks of 10 to avoid message overflow
		local count = math.min(#results, 50)
		for i = 1, count do
			local r = results[i]
			local status = r.hasWps and "ok" or "no-wps"
			player:sendTextMessage(MESSAGE_STATUS, string.format(
				"  [%d] %s (%s) [%s]", r.id, r.name, r.town, status
			))
		end
		if #results > 50 then
			player:sendTextMessage(MESSAGE_STATUS, "  ... and " .. (#results - 50) .. " more. Use a filter to narrow results.")
		end
		player:sendTextMessage(MESSAGE_STATUS, "Found " .. #results .. " spawn(s)." .. (filter and (" Filter: '" .. filter .. "')") or ""))
		return true
	end

	-- Find spawn by ID or name substring
	local scriptId = tonumber(param)
	if not scriptId then
		-- Substring search — find best (shortest name) match for specificity
		local bestId = nil
		local bestLen = math.huge
		local paramLower = param:lower()
		for id, script in pairs(BotHuntData.scripts or {}) do
			if script.name and script.name:lower():find(paramLower, 1, true) then
				if #script.name < bestLen then
					bestLen = #script.name
					bestId = id
				end
			end
		end
		scriptId = bestId
	end

	if not scriptId then
		player:sendTextMessage(MESSAGE_STATUS, "Spawn not found: " .. param)
		player:sendTextMessage(MESSAGE_STATUS, "Use '/botspawn list' to see available spawns.")
		return true
	end

	local script = BotHuntData.scripts[scriptId]
	if not script then
		player:sendTextMessage(MESSAGE_STATUS, "Script ID " .. scriptId .. " not found in BotHuntData.")
		return true
	end

	local wps = BotHuntData.getPhaseWaypoints(scriptId, "hunt_patrol")
	if not wps or #wps == 0 then
		player:sendTextMessage(MESSAGE_STATUS, "Spawn '" .. (script.name or scriptId) .. "' has no patrol waypoints.")
		return true
	end

	local firstWp = wps[1]
	if not firstWp or not firstWp.pos_x or firstWp.pos_x == 0 then
		player:sendTextMessage(MESSAGE_STATUS, "Spawn '" .. (script.name or scriptId) .. "' first waypoint has invalid coordinates.")
		return true
	end

	local dest = Position(firstWp.pos_x, firstWp.pos_y, firstWp.pos_z)
	player:teleportTo(dest)
	player:sendTextMessage(MESSAGE_STATUS, string.format(
		"Teleported to spawn '%s' [%d] (%s) — pos %d,%d,%d (%d patrol waypoints)",
		script.name or "?", scriptId, script.town_name or "?",
		dest.x, dest.y, dest.z, #wps
	))
	return true
end

spawnCmd:separator(" ")
spawnCmd:groupType("god")
spawnCmd:register()
