-- Run from the repository root: lua tests/lua/bot_startup_test.lua
-- Exercise the real startup handler with a deterministic dispatcher and fake DB.
local scriptPath = arg[1] or "data/scripts/globalevents/bot_manager.lua"
local passed = 0

local function check(condition, message)
	assert(condition, message)
end

local function startup(options)
	options = options or {}
	local state = { now = 0, events = {}, calls = {}, attempts = {}, queries = {}, logs = {}, finalizers = {}, maxPending = 0 }
	local names = {}
	for i = 1, options.pool or options.target or 1 do
		names[i] = "Bot " .. i
	end
	local current = 1
	local startupEvent
	BOT_CONFIG = { TARGET_ONLINE = options.target or #names, BOT_ACCOUNT_ID = 65000 }
	BotPlayers = {}
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
		storeQuery = function() return #names > 0 and 1 or nil end,
		botAsyncQuery = function(query) state.queries[#state.queries + 1] = query end,
	}
	Result = {
		getString = function() return names[current] end,
		next = function() current = current + 1; return current <= #names end,
		free = function() end,
	}
	logger = {}
	for _, level in ipairs({ "info", "warn", "error" }) do
		logger[level] = function(message, ...)
			state.logs[#state.logs + 1] = { level = level, message = message, args = { ... } }
		end
	end
	addEvent = function(callback, delay)
		check(delay > 0, "startup must yield to the dispatcher")
		state.events[#state.events + 1] = { callback = callback, delay = delay, due = state.now + delay }
		state.maxPending = math.max(state.maxPending, #state.events)
	end
	Game = {
		createTile = function() end,
		loadBotPlayer = function(name)
			state.attempts[name] = (state.attempts[name] or 0) + 1
			local call = { name = name, start = state.now }
			state.calls[#state.calls + 1] = call
			state.now = state.now + (options.loadMs or 0)
			call.finish = state.now
			local failure = options.failures and options.failures[name]
			if failure == "throw" then
				error("load failed {with braces}")
			elseif failure and state.attempts[name] <= failure then
				return nil
			end
			local guid = tonumber(name:match("%d+"))
			return {
				getGuid = function() return guid end,
				registerEvent = function(_, event)
					check(event == "BotDeath", "loaded players must retain their death handler")
				end,
			}
		end,
	}
	dofile(scriptPath)
	check(startupEvent.onStartup() == true, "startup hook must succeed")
	state.initialPending = #state.events
	local executed = 0
	while #state.events > 0 do
		table.sort(state.events, function(a, b) return a.due < b.due end)
		local event = table.remove(state.events, 1)
		if event.delay == 2000 then
			-- Quest/access setup and persisted-state restoration are unchanged. Verify
			-- their entry point is scheduled once, only after every load is attempted.
			state.finalizers[#state.finalizers + 1] = { after = #state.calls, due = event.due }
		else
			state.now = math.max(state.now, event.due)
			event.callback()
		end
		executed = executed + 1
		check(executed < 10000, "startup retry loop did not terminate")
	end
	state.loaded = 0
	for _ in pairs(BotPlayers) do state.loaded = state.loaded + 1 end
	return state
end

local function test(name, run)
	run()
	passed = passed + 1
	print("ok " .. passed .. " - " .. name)
end

test("slow loads have one pending load and yield after completion", function()
	local s = startup({ target = 49, loadMs = 750 })
	check(s.initialPending == 1, "all loads were queued before the first one completed")
	check(s.maxPending <= 1, "startup accumulated a load backlog")
	for i = 2, #s.calls do
		check(s.calls[i].start - s.calls[i - 1].finish >= 100, "slow load starved dispatcher work")
	end
	check(s.loaded == 49, "not all bots loaded")
end)

test("997 bots load in twenty batches with hibernation pauses", function()
	local s = startup({ target = 997 })
	check(s.loaded == 997 and #s.calls == 997, "population was lost or loaded twice")
	local batches = 0
	for _, log in ipairs(s.logs) do
		if log.message:find("[BOT_BATCH]", 1, true) then batches = batches + 1 end
	end
	check(batches == 20, "last partial batch was not reported")
	for i = 2, #s.calls do
		local expected = (i - 1) % 50 == 0 and 35000 or 100
		check(s.calls[i].start - s.calls[i - 1].finish == expected, "incorrect batch pacing")
	end
	check(#s.finalizers == 1 and s.finalizers[1].after == 997, "finalization ran early or twice")
end)

test("transient nil load retries before advancing", function()
	local s = startup({ target = 2, failures = { ["Bot 1"] = 2 } })
	check(s.loaded == 2 and s.attempts["Bot 1"] == 3, "transient failure was not retried")
	check(s.calls[3].name == "Bot 1" and s.calls[4].name == "Bot 2", "loads overlapped retries")
	check(s.calls[2].start - s.calls[1].finish == 1000, "retry did not back off")
end)

test("permanent nil failure does not block the next bot", function()
	local s = startup({ target = 2, failures = { ["Bot 1"] = 100 } })
	check(s.attempts["Bot 1"] == 3 and s.attempts["Bot 2"] == 1, "retry bound was not respected")
	check(s.loaded == 1 and #s.finalizers == 1, "failed bot prevented completion")
end)

test("thrown load error is bounded and final failure still finalizes", function()
	local s = startup({ target = 2, failures = { ["Bot 2"] = "throw" } })
	check(s.loaded == 1 and s.attempts["Bot 2"] == 3, "thrown error escaped startup")
	check(#s.finalizers == 1 and s.finalizers[1].after == 4, "last failure lost finalization")
end)

test("hidden bots do not appear in players_online during batches", function()
	local s = startup({ target = 51, showOnline = false })
	for _, query in ipairs(s.queries) do
		check(not query:find("players_online", 1, true), "online visibility flag was ignored")
	end
end)

test("visible bots are published after the first batch", function()
	local s = startup({ target = 51 })
	local batchSyncs = 0
	for _, query in ipairs(s.queries) do
		if query:find("SELECT `player_id` FROM `bot_active_players`", 1, true) then batchSyncs = batchSyncs + 1 end
	end
	check(batchSyncs == 2, "batch population did not become visible")
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
