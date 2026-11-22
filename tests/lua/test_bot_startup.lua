-- Regression tests for the BotStartup loader (data/scripts/globalevents/bot_manager.lua).
-- Run from the repository root with: luajit tests/lua/test_bot_startup.lua  (or lua5.4)
-- Exercises the real startup handler with a deterministic fake dispatcher, clock and DB:
-- no C++ build, running server or database is needed.
-- Adapted from dandpb's PR #3 (tests/lua/bot_startup_test.lua, tools/bot_brain/test_startup_batches.lua).
local scriptPath = arg and arg[1] or "data/scripts/globalevents/bot_manager.lua"
local passed = 0

local function check(condition, message)
	if not condition then
		error(message, 2)
	end
end

-- options: target, pool, loadMs (per load), loadMsFor[name], failures[name] = n | "throw",
-- dispatcherLag[n] = extra ms before the n-th executed timer, showOnline, hibernateAfterMs
local function startup(options)
	options = options or {}
	local state = { now = 0, events = {}, calls = {}, attempts = {}, queries = {}, logs = {}, finalizers = {}, maxPending = 0, storagesApplied = {} }
	local names = {}
	for i = 1, options.pool or options.target or 1 do
		names[i] = "Bot " .. i
	end
	local cursor = 1
	local startupEvent
	BOT_CONFIG = { TARGET_ONLINE = options.target or #names, BOT_ACCOUNT_ID = 65000, MANAGER_INTERVAL = 10000 }
	BotPlayers = {}
	BotStorages = {
		apply = function(player)
			state.storagesApplied[player:getGuid()] = true
			return 3, 1
		end,
	}
	configKeys = { BOT_PERSONALITY_REROLL_ON_RESTART = "reroll", BOT_PLAYERS_SHOW_AS_ONLINE = "online" }
	configManager = { getBoolean = function(key)
		return key == "online" and options.showOnline ~= false
	end }
	GlobalEvent = function(name)
		local event = { register = function() end, interval = function() end }
		if name == "BotStartup" then
			startupEvent = event
		end
		return event
	end
	Position = function(...) return { ... } end
	db = {
		query = function() return true end,
		storeQuery = function(query)
			if query:find("FROM `players`", 1, true) then
				return #names > 0 and 1 or nil
			end
			return nil -- bot_startup_config
		end,
		botAsyncQuery = function(query) state.queries[#state.queries + 1] = query end,
		botAsyncStoreQuery = function() end,
		escapeString = function(s) return "'" .. s .. "'" end,
	}
	Result = {
		getString = function() return names[cursor] end,
		next = function() cursor = cursor + 1; return cursor <= #names end,
		free = function() end,
	}
	logger = {}
	for _, level in ipairs({ "info", "warn", "error" }) do
		logger[level] = function(message, ...)
			state.logs[#state.logs + 1] = { level = level, message = message, args = { ... } }
		end
	end
	local function pending()
		local n = 0
		for _, e in ipairs(state.events) do
			if e.kind == "load" then n = n + 1 end
		end
		return n
	end
	local inFinalizer = false
	addEvent = function(callback, delay)
		check(delay >= 100, "startup must respect the server's 100 ms addEvent floor")
		local kind = inFinalizer and "final" or (delay == 2000 and "final" or "load")
		state.events[#state.events + 1] = { callback = callback, delay = delay, due = state.now + delay, kind = kind, seq = #state.events }
		state.maxPending = math.max(state.maxPending, pending())
	end
	Game = {
		createTile = function() end,
		monotonicMs = function() return state.now end,
		botRestoreStates = function() end,
		loadBotPlayer = function(name)
			state.attempts[name] = (state.attempts[name] or 0) + 1
			local call = { name = name, start = state.now }
			state.calls[#state.calls + 1] = call
			state.now = state.now + ((options.loadMsFor and options.loadMsFor[name]) or options.loadMs or 0)
			call.finish = state.now
			local failure = options.failures and options.failures[name]
			if failure == "throw" then
				error("load failed {with braces}")
			elseif failure and state.attempts[name] <= failure then
				return nil
			end
			local guid = tonumber(name:match("%d+"))
			local player = {
				getGuid = function() return guid end,
				registerEvent = function(_, event)
					check(event == "BotDeath", "loaded players must retain their death handler")
				end,
			}
			if options.hibernateAfterMs then
				-- Same Lua-cache removal as a no-anchor hibernation, while startup is still running.
				state.events[#state.events + 1] = { kind = "hib", due = state.now + options.hibernateAfterMs, seq = #state.events, callback = function()
					check(state.storagesApplied[guid], "a bot hibernated before it received quest access")
					BotPlayers[guid] = nil
				end }
			end
			return player
		end,
	}
	dofile(scriptPath)
	check(startupEvent.onStartup() == true, "startup hook must succeed")
	state.initialPending = pending()
	local executed = 0
	while #state.events > 0 do
		table.sort(state.events, function(a, b)
			if a.due ~= b.due then return a.due < b.due end
			return a.seq < b.seq
		end)
		local event = table.remove(state.events, 1)
		if event.kind == "final" and event.delay == 2000 then
			state.finalizers[#state.finalizers + 1] = { after = #state.calls, due = event.due }
		end
		if event.kind == "load" then
			executed = executed + 1
			local lag = options.dispatcherLag and options.dispatcherLag[executed] or 0
			state.now = math.max(state.now, event.due) + lag
		else
			state.now = math.max(state.now, event.due)
		end
		inFinalizer = event.kind == "final"
		event.callback()
		inFinalizer = false
		check(executed < 100000, "startup loop did not terminate")
	end
	state.loaded = 0
	for _ in pairs(BotPlayers) do state.loaded = state.loaded + 1 end
	return state
end

local function findLog(s, needle)
	for _, log in ipairs(s.logs) do
		if log.message:find(needle, 1, true) then return log end
	end
end

local function gap(s, i)
	return s.calls[i].start - s.calls[i - 1].finish
end

local function test(name, run)
	run()
	passed = passed + 1
	print("ok " .. passed .. " - " .. name)
end

test("only one load event is ever pending; nothing is queued up front", function()
	local s = startup({ target = 200, loadMs = 30 })
	check(s.initialPending == 1, "all loads were queued before the first one completed")
	check(s.maxPending == 1, "startup accumulated a load backlog")
	check(s.loaded == 200, "not all bots loaded")
end)

test("slow loads (one per slice) get a bounded backoff", function()
	local s = startup({ target = 49, loadMs = 750 })
	for i = 2, #s.calls do
		local g = gap(s, i)
		check(g > 100 and g <= 1000, "slow loads must yield between 100 ms and 1 s, got " .. g)
	end
	check(s.loaded == 49 and s.maxPending == 1, "slow startup lost bots or overlapped loads")
end)

test("healthy loads are batched into slices, so 500 x 100 ms costs ~60 s, not ~100 s", function()
	local s = startup({ target = 500, loadMs = 100 })
	check(s.loaded == 500 and #s.calls == 500, "population was lost or loaded twice")
	local elapsed = s.calls[#s.calls].finish
	check(elapsed <= 62000, string.format("healthy startup took %.1fs", elapsed / 1000))
	-- Every slice ends with a yield: the dispatcher is never held past one slice + one load.
	local sliceStart = s.calls[1].start
	for i = 2, #s.calls do
		if gap(s, i) > 0 then
			check(gap(s, i) >= 100, "a slice yielded for less than the timer floor")
			sliceStart = s.calls[i].start
		end
		check(s.calls[i].finish - sliceStart <= 600, "a slice held the dispatcher too long")
	end
end)

test("997 instant loads finish well under 100 s of scheduler time", function()
	local s = startup({ target = 997 })
	check(s.loaded == 997 and #s.calls == 997, "population was lost or loaded twice")
	check(#s.finalizers == 1 and s.finalizers[1].after == 997, "finalization ran early or twice")
	local batches = 0
	for _, log in ipairs(s.logs) do
		if log.message:find("[BOT_BATCH]", 1, true) then batches = batches + 1 end
	end
	check(batches == 20, "progress every 50 bots + the final partial batch, got " .. batches)
end)

local function sliceGaps(s)
	local gaps = {}
	for i = 2, #s.calls do
		if gap(s, i) > 0 then gaps[#gaps + 1] = gap(s, i) end
	end
	return gaps
end

test("dispatcher lag raises the yield and healthy slices lower it again", function()
	-- 100 ms loads: five per 500 ms slice, no registration pressure, so the yield is the floor.
	local s = startup({ target = 50, loadMs = 100, dispatcherLag = { [3] = 200 } })
	local g = sliceGaps(s)
	check(g[1] == 100 and g[2] == 300, "test did not simulate a late callback")
	check(g[3] == 800, "a 200 ms late timer must yield 4 x 200 ms, got " .. tostring(g[3]))
	check(g[4] == 400 and g[5] == 200 and g[6] == 100, "the yield did not halve back to the floor")
	check(s.loaded == 50 and s.maxPending == 1, "startup lost bots or queued parallel loads under lag")
end)

test("a single slow load raises the yield, capped at 1 s", function()
	local s = startup({ target = 20, loadMs = 100, loadMsFor = { ["Bot 8"] = 250 } })
	local g = sliceGaps(s)
	check(g[2] == 600, "a 250 ms load must yield 4 x 150 ms, got " .. tostring(g[2]))
	local slow = startup({ target = 5, loadMs = 900 })
	for _, v in ipairs(sliceGaps(slow)) do
		check(v == 1000, "the yield must cap at 1 s")
	end
end)

test("the first event's lateness (server startup task) is not counted as pressure", function()
	local s = startup({ target = 15, loadMs = 100, dispatcherLag = { [1] = 9000 } })
	check(sliceGaps(s)[1] == 100, "boot-time lateness inflated the first yield")
end)

test("transient nil load retries on the same slot before advancing", function()
	local s = startup({ target = 2, failures = { ["Bot 1"] = 2 } })
	check(s.loaded == 2 and s.attempts["Bot 1"] == 3, "transient failure was not retried")
	check(s.calls[3].name == "Bot 1" and s.calls[4].name == "Bot 2", "loads overlapped retries")
	check(gap(s, 2) == 1000, "retry did not back off 1 s")
end)

test("permanent nil failure does not block the next bot and is reported", function()
	local s = startup({ target = 2, failures = { ["Bot 1"] = 100 } })
	check(s.attempts["Bot 1"] == 3 and s.attempts["Bot 2"] == 1, "retry bound was not respected")
	check(s.loaded == 1 and #s.finalizers == 1, "failed bot prevented completion")
	local final = findLog(s, "bots loaded as INACTIVE")
	check(final and final.args[1] == 1 and final.args[2] == 2 and final.args[3] == 1, "final log must report loaded/planned/failed")
end)

test("thrown load error on the last slot is bounded and still finalizes", function()
	local s = startup({ target = 2, failures = { ["Bot 2"] = "throw" } })
	check(s.loaded == 1 and s.attempts["Bot 2"] == 3, "thrown error escaped startup")
	check(#s.finalizers == 1 and s.finalizers[1].after == 4, "last failure lost finalization")
	local err = findLog(s, "Error loading bot")
	check(err and tostring(err.args[2]):find("{with braces}", 1, true), "error text must be an argument, not the format string")
end)

test("final log counts loads, not BotPlayers (bots may hibernate during startup)", function()
	local s = startup({ target = 120, loadMs = 50, hibernateAfterMs = 3000 })
	local final = findLog(s, "bots loaded as INACTIVE")
	check(final and final.args[1] == 120 and final.args[3] == 0, "final count was taken from the Lua cache")
	check(s.loaded < 120, "the test must hibernate some bots while startup is still loading")
end)

test("every bot receives quest access before it can hibernate", function()
	local s = startup({ target = 60, loadMs = 50, hibernateAfterMs = 100 })
	local n = 0
	for _ in pairs(s.storagesApplied) do n = n + 1 end
	check(n == 60, "quest access was not applied to every loaded bot")
end)

test("hidden bots never touch players_online", function()
	local s = startup({ target = 51, showOnline = false })
	for _, query in ipairs(s.queries) do
		check(not query:find("players_online", 1, true), "online visibility flag was ignored")
	end
end)

test("visible bots are synced to players_online once, after loading", function()
	local s = startup({ target = 51 })
	local inserts = 0
	for _, query in ipairs(s.queries) do
		if query:find("INSERT IGNORE INTO `players_online`", 1, true) then inserts = inserts + 1 end
	end
	check(inserts == 1, "players_online sync must run exactly once")
end)

test("zero target and empty pool schedule no loads", function()
	for _, options in ipairs({ { target = 0, pool = 10 }, { pool = 0 } }) do
		local s = startup(options)
		check(#s.calls == 0 and #s.finalizers == 0, "empty startup scheduled work")
	end
end)

test("stratified selection remains unchanged", function()
	local s = startup({ target = 3, pool = 100 })
	check(s.calls[1].name == "Bot 1" and s.calls[2].name == "Bot 34" and s.calls[3].name == "Bot 67", "selection changed")
end)

print(passed .. " startup tests passed")
