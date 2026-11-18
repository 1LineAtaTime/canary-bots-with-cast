-- Public startup lifecycle with a virtual event loop: no game/database required.
local events, pending, attempts, loaded, warnings, onlineQueries = {}, {}, {}, {}, {}, {}
local clock = 0
local count = 351
local players, missedInitialization = {}, false
local hibernatedDuringStartup = false
dofile("data-otservbr-global/lib/core/storages.lua")
BOT_CONFIG = { MASTER_DISABLE = false, BOT_ACCOUNT_ID = 65000, TARGET_ONLINE = count, MANAGER_INTERVAL = 10000 }
BotPlayers = {}
Position = function(x, y, z) return { x = x, y = y, z = z } end
configKeys = { BOT_PLAYERS_SHOW_AS_ONLINE = "show_online", BOT_POPULATION_REFERENCE_BOTS = "references" }
local showOnline = arg[2] == "online"
configManager = {
	getBoolean = function(key) return key == "show_online" and showOnline end,
	getString = function() return "" end,
}
logger = { info = function() end, error = function() end,
	warn = function(message) warnings[#warnings + 1] = message end }
GlobalEvent = function(name)
	local event = { register = function() end, interval = function() end }
	events[name] = event
	return event
end
addEvent = function(callback, delay)
	pending[#pending + 1] = { at = clock + delay, callback = callback }
end
db = { query = function() return true end, botAsyncQuery = function(query) onlineQueries[#onlineQueries + 1] = query; return true end,
	storeQuery = function(query) return query:find("FROM `players`", 1, true) and { index = 1 } or false end }
Result = {
	getString = function(cursor) return "Bot " .. cursor.index end,
	next = function(cursor) cursor.index = cursor.index + 1; return cursor.index <= count end,
	free = function() end,
}
Game = {
	createTile = function() end,
	monotonicMs = function() return clock end,
	loadBotPlayer = function(name)
		local guid = tonumber(name:match("%d+"))
		attempts[guid] = (attempts[guid] or 0) + 1
		if guid == 2 and attempts[guid] == 1 then error("transient load exception") end
		if guid == 52 and attempts[guid] == 1 then return nil end
		if guid == count then return nil end
		loaded[guid] = clock
		local player = { storages = {}, getGuid = function() return guid end }
		player.registerEvent = function() player.registered = true end
		player.setStorageValue = function(_, key, value) player.storages[key] = value end
		players[guid] = player
		addEvent(function()
			if not attempts[count] then hibernatedDuringStartup = true end
			if player.storages[Storage.WagonTicket] ~= 2147483647 then missedInitialization = true end
			BotPlayers[guid] = nil -- Same cache removal as no-anchor hibernation.
		end, 30000)
		return player
	end,
}
dofile(arg[1] or "data/scripts/globalevents/bot_manager.lua")
assert(events.BotStartup.onStartup())
local processed = 0
while #pending > 0 do
	processed = processed + 1; assert(processed < 1000, "startup event loop did not finish")
	table.sort(pending, function(a, b) return a.at < b.at end)
	local nextEvent = table.remove(pending, 1)
	clock = nextEvent.at
	nextEvent.callback()
end
assert(loaded[51] and loaded[50] and loaded[51] - loaded[50] == 100,
	"a healthy dispatcher should load the next bot immediately after the usual timer delay")
assert(loaded[count - 1] and attempts[count] == 3, "final load failure must have bounded retries")
assert(attempts[2] == 2 and attempts[52] == 2, "transient exceptions/nil loads must retry")
local registered = 0
for _, player in pairs(players) do
	if player.registered then registered = registered + 1 end
end
assert(registered == count - 1, "one failed bot must not lose later registrations")
local loadWarnings = 0
for _, message in ipairs(warnings) do if message:find("Failed to load bot", 1, true) then loadWarnings = loadWarnings + 1 end end
assert(loadWarnings == 1, "failed final bot must warn")
assert(hibernatedDuringStartup, "the test must hibernate a bot while startup is still loading")
assert(not missedInitialization, "earlier bots must receive quest access before hibernation")
if showOnline then
	assert(#onlineQueries >= 3, "visible bots must synchronize online rows during loading")
else
	assert(#onlineQueries == 0, "hidden bots must not publish online rows during loading")
end
print("PASS: bots receive quest access before hibernation during startup, including with load retries")
