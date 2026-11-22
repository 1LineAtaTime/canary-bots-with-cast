-- ============================================================================
-- Bot Manager -- Load bots on startup
-- Bots load at their last saved position (players.posx/y/z from normal server save).
-- Inactive bots are truly offline — removed from the world when deactivated.
-- Use /cavebot <name> active|inactive to control bots.
-- ============================================================================

-- Staging position — kept as fallback for brand-new bots with no prior loginPosition.
-- INACTIVE_STAGING_POS is no longer used as the "home" for inactive bots.
-- local INACTIVE_STAGING_POS = Position(31970, 32283, 7)

-- Quest/access storages (wagons, shrines, levers, teleports). Applied to each bot right after
-- Game.loadBotPlayer succeeds (#57). They used to be set in one pass over BotPlayers two seconds
-- after the LAST bot loaded (~50 s at 500 bots), and a bot that hibernated in between had already
-- left BotPlayers: it kept the same Player object in the hibernation pool, without the storages.
-- #83: the table now lives in data/bot/authored/bot_storages.csv, level- and vocation-gated
-- (data/scripts/lib/bot_storages.lua). Idea and shape from dandpb/canary-bots-with-cast
-- 9e37f3032e (initializeBotQuestAccess).
local questAccessCount = 0
local questAccessFailures = 0
local questAccessApplied, questAccessGated = 0, 0
local function initializeBotQuestAccess(player)
	local applied, gated = BotStorages.apply(player)
	questAccessApplied = questAccessApplied + applied
	questAccessGated = questAccessGated + gated
	questAccessCount = questAccessCount + 1
end

-- Startup: load all bot players from database
local botStartup = GlobalEvent("BotStartup")

function botStartup.onStartup()
	-- Clear stale cast_broadcasters from previous run/crash. Runs even with bots disabled:
	-- human casts lost at shutdown or in a crash rely on it (CastPublisher, issue #13).
	db.query("DELETE FROM `cast_broadcasters`")

	if BOT_CONFIG.MASTER_DISABLE then
		logger.warn("[BotManager] MASTER_DISABLE=true — bot startup skipped (lag-investigation Test 1)")
		return true
	end
	logger.info("[BotManager] Starting bot system...")

	-- BOT_LIVENESS_PACK Phase C.1: visual-identity reroll. Every server restart
	-- shuffles each bot's looktype + addons + colors + mount + mount colors.
	-- Bots ALREADY own the full catalog (mounts + outfits via player_storage seeded
	-- by tools/bot_population_generator/generate.py) — these UPDATEs just pick a
	-- different row from the owned set. Runs BEFORE Game.loadBotPlayer below so
	-- the in-memory Player object reads the fresh values on load. ~3 sync queries,
	-- ~150ms total blocking time (negligible vs the rest of startup).
	-- Only triggers at GLOBALEVENT_STARTUP — /cavebot reload does NOT hit this
	-- path (it goes through createBotEngine + bot re-registration without touching
	-- this onStartup), which matches the user's "reroll every daily restart" intent.
	if configManager.getBoolean(configKeys.BOT_PERSONALITY_REROLL_ON_RESTART) then
		logger.info("[BotManager] Rerolling visual identity (looktype + addons + colors + mount) for all bots...")
		-- 1. Pick a random OWNED outfit (lookType<<16|addons) per bot from player_storage.
		--    MySQL 8.0 window function — runs in <100ms for ~123k storage rows.
		db.query([[
			UPDATE players p
			INNER JOIN (
				SELECT player_id, value,
				       ROW_NUMBER() OVER (PARTITION BY player_id ORDER BY RAND()) AS rn
				  FROM player_storage
				 WHERE `key` BETWEEN 10001001 AND 10001500
			) s ON p.id = s.player_id AND s.rn = 1
			   SET p.looktype = FLOOR(s.value / 65536),
			       p.lookaddons = FLOOR(RAND() * 4)
			 WHERE p.account_id = ]] .. BOT_CONFIG.BOT_ACCOUNT_ID)
		-- 2. Randomize the four outfit-color bytes + four mount-color bytes (palette 0..132).
		db.query([[
			UPDATE players SET
			  lookbody      = FLOOR(RAND() * 133),
			  lookfeet      = FLOOR(RAND() * 133),
			  lookhead      = FLOOR(RAND() * 133),
			  looklegs      = FLOOR(RAND() * 133),
			  lookmountbody = FLOOR(RAND() * 133),
			  lookmountfeet = FLOOR(RAND() * 133),
			  lookmounthead = FLOOR(RAND() * 133),
			  lookmountlegs = FLOOR(RAND() * 133)
			WHERE account_id = ]] .. BOT_CONFIG.BOT_ACCOUNT_ID)
		-- 3. Pick a random mount id 1..235 as the current mount. mountChancePct (in
		--    bot_engine.cpp activateBot) decides whether the bot is actually mounted.
		db.query([[
			UPDATE player_storage ps
			INNER JOIN players p ON p.id = ps.player_id
			   SET ps.value = FLOOR(1 + RAND() * 235)
			 WHERE p.account_id = ]] .. BOT_CONFIG.BOT_ACCOUNT_ID .. [[
			   AND ps.`key` = 10002011]])
		logger.info("[BotManager] Visual identity reroll complete.")
	end

	-- Ensure staging tile exists for brand-new bots that have no prior loginPosition
	Game.createTile(Position(31970, 32283, 7), true)

	-- Query all bot characters ordered by id ASC (= level ASC, since bots are seeded sorted by level).
	-- We fetch all then stratify in Lua so any TARGET_ONLINE < total still covers the full
	-- level/vocation range (otherwise LIMIT N would clip to the lowest-level slice).
	local query = "SELECT `id`, `name` FROM `players` WHERE `account_id` = " .. BOT_CONFIG.BOT_ACCOUNT_ID
		.. " AND `deletion` = 0 ORDER BY `id` ASC"

	local resultId = db.storeQuery(query)

	if not resultId then
		logger.warn("[BotManager] No bot players found in database (account_id=" ..
			BOT_CONFIG.BOT_ACCOUNT_ID .. ")")
		return true
	end

	local allBotNames = {}
	repeat
		table.insert(allBotNames, Result.getString(resultId, "name"))
	until not Result.next(resultId)
	Result.free(resultId)

	-- Stratified selection: evenly-spaced indices across [1, total] so we get diverse levels
	-- + vocations regardless of TARGET_ONLINE. Deterministic across restarts so
	-- bot_state_persistence rows stay coherent.
	local total = #allBotNames
	local target = BOT_CONFIG.TARGET_ONLINE
	local botNames = {}

	-- 2026-05-28 test override: when target == 1 AND BOT_CONFIG.SINGLE_BOT_NAME is set,
	-- load that specific bot (useful for isolated cast-watch perf testing on a chosen
	-- high-level character rather than the lowest-id bot stratification picker would return).
	if target == 1 and BOT_CONFIG.SINGLE_BOT_NAME and BOT_CONFIG.SINGLE_BOT_NAME ~= "" then
		local found = false
		for _, n in ipairs(allBotNames) do
			if n == BOT_CONFIG.SINGLE_BOT_NAME then
				table.insert(botNames, n)
				found = true
				break
			end
		end
		if not found then
			logger.warn("[BotManager] SINGLE_BOT_NAME '" .. BOT_CONFIG.SINGLE_BOT_NAME ..
				"' not in DB; falling back to stratified pick")
		end
	end

	if #botNames == 0 then
		if target >= total then
			botNames = allBotNames
		else
			for i = 0, target - 1 do
				local idx = math.floor(i * total / target) + 1
				table.insert(botNames, allBotNames[idx])
			end
		end

		-- 2026-05-28 extension: when target > 1 AND SINGLE_BOT_NAME is set, ensure the
		-- named bot is in the picked set. If missing, replace the last entry with it.
		-- Keeps the stratified diversity while guaranteeing the cast-watched bot is loaded.
		if target > 1 and BOT_CONFIG.SINGLE_BOT_NAME and BOT_CONFIG.SINGLE_BOT_NAME ~= "" then
			local already = false
			for _, n in ipairs(botNames) do
				if n == BOT_CONFIG.SINGLE_BOT_NAME then already = true; break end
			end
			if not already then
				-- Verify the named bot exists in DB
				local existsInDB = false
				for _, n in ipairs(allBotNames) do
					if n == BOT_CONFIG.SINGLE_BOT_NAME then existsInDB = true; break end
				end
				if existsInDB then
					botNames[#botNames] = BOT_CONFIG.SINGLE_BOT_NAME
					logger.info("[BotManager] Forced inclusion of '" .. BOT_CONFIG.SINGLE_BOT_NAME ..
						"' replacing last stratified pick")
				else
					logger.warn("[BotManager] SINGLE_BOT_NAME '" .. BOT_CONFIG.SINGLE_BOT_NAME ..
						"' not in DB; using stratified pick as-is")
				end
			end
		end
	end

	logger.info("[BotManager] Found " .. total .. " bots in database, loading "
		.. #botNames .. " (stratified across level/vocation) as INACTIVE...")

	-- Adaptive startup pacing (adapted from dandpb's PR #3). Exactly one load event is pending
	-- at a time, scheduled only after the previous one finished, so a slow load (or a busy
	-- dispatcher) can never pile up a backlog of timers that all fire in the same cycle.
	-- Each event loads bots for up to STARTUP_SLICE_MS, then yields to the dispatcher. The yield
	-- starts at 100 ms (the addEvent floor) and rises to at most 1 s at 4x the measured
	-- pressure: how late the timer fired, or how far a single load ran past 100 ms. It then
	-- halves back toward the floor as the server catches up.
	-- A failed or erroring load is retried up to twice, 1 s apart, on the same slot; the
	-- end-of-load work below still runs exactly once, after the last slot's final attempt.
	local STARTUP_SLICE_MS, STARTUP_MIN_DELAY_MS, STARTUP_MAX_DELAY_MS = 500, 100, 1000
	local STARTUP_PRESSURE_MULT, STARTUP_BATCH_LOG = 4, 50
	local planned = #botNames
	local loadedCount, failedCount = 0, 0
	local nextDelayMs = STARTUP_MIN_DELAY_MS
	local loadStartedAt = Game.monotonicMs()
	local maxLatenessMs, maxLoadMs = 0, 0

	local function finishLoading()
		-- PERF_INVESTIGATION_2026-05-28: sync bot rows into players_online
		-- exactly once after loading completes. Previously Game::updatePlayersOnline
		-- enumerated every active bot guid into a sync INSERT/DELETE pair on the
		-- dispatcher every 10 minutes; at bots=500 that was ~500ms of dispatcher
		-- blockage. The C++ sweep no longer touches bot rows (account_id=65000
		-- filter in DELETE); we replace the whole bot row set here via async
		-- queries so the dispatcher is never blocked, and re-sync on /cavebot
		-- reload too (this same loading path runs on reload).
		if configManager.getBoolean(configKeys.BOT_PLAYERS_SHOW_AS_ONLINE) then
			local botGuids = {}
			for guid, _ in pairs(BotPlayers) do
				table.insert(botGuids, tostring(guid))
			end
			if #botGuids > 0 then
				local guidList = table.concat(botGuids, ",")
				db.botAsyncQuery(string.format(
					"DELETE FROM `players_online` WHERE `player_id` IN (SELECT `id` FROM `players` WHERE `account_id` = 65000) AND `player_id` NOT IN (%s)",
					guidList))
				local valueRows = {}
				for _, g in ipairs(botGuids) do
					table.insert(valueRows, "(" .. g .. ")")
				end
				db.botAsyncQuery("INSERT IGNORE INTO `players_online` (`player_id`) VALUES " .. table.concat(valueRows, ","))
				logger.info("[BotManager] Synced " .. #botGuids .. " bot rows to players_online (async)")
			end
		end

		addEvent(function()
			-- Quest/access storages are set per bot right after its load (#57).
			logger.info("[BOT_STORAGES] applied to bots={} rowsSet={} rowsGated={} (wagons, shrines, quests; level-gated)", questAccessCount, questAccessApplied, questAccessGated)
			if questAccessFailures > 0 then
				logger.error("[BotManager] initializeBotQuestAccess FAILED on {} bot(s) (first 3 errors logged above)", questAccessFailures)
			end
			-- Report what actually loaded, not what was planned.
			logger.info("[BotManager] All {} of {} bots loaded as INACTIVE ({} failed) in {}s.",
				loadedCount, planned, failedCount, string.format("%.1f", (Game.monotonicMs() - loadStartedAt) / 1000))
			logger.info("[BotManager] Use '/cavebot <name> active' to activate a bot at your position.")

			-- Check for persisted state restore (500ms after last bot is registered)
			addEvent(function()
				local cfgResult = db.storeQuery(
					"SELECT `config_value` FROM `bot_startup_config` WHERE `config_key` = 'should_restore_states'"
				)
				if cfgResult then
					local val = Result.getString(cfgResult, "config_value")
					Result.free(cfgResult)
					if val == "1" then
						logger.info("[BotManager] Restore flag set — restoring persisted bot states...")
						Game.botRestoreStates()
						-- Restored rows are consumed; tasks for unloaded bots remain persisted.
						-- Clear the flag so a crash before next graceful shutdown doesn't re-restore stale data.
						db.botAsyncQuery("UPDATE `bot_startup_config` SET `config_value`='0' WHERE `config_key`='should_restore_states'")
						logger.info("[BotManager] Bot state restore complete. Restore flag cleared.")
					else
						logger.info("[BotManager] No restore flag — bots activating fresh.")
					end
				else
					logger.warn("[BotManager] bot_startup_config not found — bots activate fresh.")
				end
			end, 500)
		end, 2000)
	end

	-- Bookkeeping for one slot's final outcome (success, or failure after the last attempt).
	local function settleSlot(i, name, player)
		if player then
			loadedCount = loadedCount + 1
			local guid = player:getGuid()
			BotPlayers[guid] = player

			-- Register death handler
			player:registerEvent("BotDeath")
			-- Before anything can hibernate it (#57). Protected: an error here (e.g. a renamed
			-- Storage constant) must not abort the slice -- and would repeat for every bot.
			local qaOk, qaErr = pcall(initializeBotQuestAccess, player)
			if not qaOk then
				questAccessFailures = questAccessFailures + 1
				if questAccessFailures <= 3 then
					logger.error("[BotManager] initializeBotQuestAccess failed for '{}': {}", name, tostring(qaErr))
				end
			end

			-- Log every 10th bot
			if i % 10 == 0 or i == planned then
				logger.info("[BotManager] Loaded bot " .. i .. "/" .. planned .. ": " .. name)
			end
		else
			failedCount = failedCount + 1
			logger.warn("[BotManager] Failed to load bot: " .. name)
		end
		if i % STARTUP_BATCH_LOG == 0 or i == planned then
			logger.info("[BOT_BATCH] completed {}/{}; loaded={} failed={} elapsed={}ms delay={}ms maxLate={}ms maxLoad={}ms",
				i, planned, loadedCount, failedCount, Game.monotonicMs() - loadStartedAt, nextDelayMs, maxLatenessMs, maxLoadMs)
			maxLatenessMs, maxLoadMs = 0, 0
		end
	end

	local function scheduleSlice(i, attempt, delay)
		if i > planned then return end
		local due = Game.monotonicMs() + delay
		addEvent(function()
			local started = Game.monotonicMs()
			-- The first event always fires late: onStartup runs inside the server's own startup
			-- task, which keeps the dispatcher busy until the world is up. That is not pressure.
			local lateMs = i > 1 and math.max(0, started - due) or 0
			if lateMs > maxLatenessMs then maxLatenessMs = lateMs end
			local slowestMs = 0
			while true do
				local name = botNames[i]
				local t0 = Game.monotonicMs()
				local ok, player = pcall(Game.loadBotPlayer, name)
				local costMs = Game.monotonicMs() - t0
				if costMs > slowestMs then slowestMs = costMs end
				if costMs > maxLoadMs then maxLoadMs = costMs end
				if not ok then
					logger.error("[BotManager] Error loading bot '{}': {}", name, tostring(player))
					player = nil
				end
				if not player and attempt < 3 then
					scheduleSlice(i, attempt + 1, 1000)
					return
				end
				settleSlot(i, name, player)
				if i == planned then
					finishLoading()
					return
				end
				i, attempt = i + 1, 1
				if Game.monotonicMs() - started >= STARTUP_SLICE_MS then break end
			end
			-- Loads up to the 100 ms floor are healthy; only the excess counts as pressure.
			local pressureMs = math.max(lateMs, slowestMs - STARTUP_MIN_DELAY_MS)
			nextDelayMs = math.max(STARTUP_MIN_DELAY_MS, math.floor(nextDelayMs / 2),
				math.min(STARTUP_MAX_DELAY_MS, pressureMs * STARTUP_PRESSURE_MULT))
			scheduleSlice(i, 1, nextDelayMs)
		end, delay)
	end
	scheduleSlice(1, 1, STARTUP_MIN_DELAY_MS)

	return true
end

botStartup:register()

-- Population manager: DISABLED — only polls bot_commands table for admin control
-- Max bytes of a command result persisted to bot_commands.result.
-- The column is TEXT (65535 BYTES), and db.escapeString can at worst double the length, so
-- 32000 is the largest provably safe cap. The previous value was 4096, which silently truncated
-- longer replies with no error -- the perf harness reads structured output back through this
-- channel, and a half-JSON reply parses as garbage rather than failing loudly.
local BOT_CMD_RESULT_MAX = 32000

local botManager = GlobalEvent("BotManager")

function botManager.onThink(interval)
	-- Global command queue poll (so MySQL-based commands still work).
	-- JITTER FIX 2026-06-10: async — the sync db.storeQuery blocked the dispatcher
	-- for the full round-trip incl. databaseLock convoy behind worker-thread DB
	-- bursts (measured 376ms and 643ms tonight via DB_SYNC_SLOW). Command
	-- processing becomes eventually-consistent within the same 10s window.
	db.botAsyncStoreQuery(
		"SELECT `id`, `bot_name`, `command` FROM `bot_commands` WHERE `processed` = 0 ORDER BY `id` ASC LIMIT 10",
		function(cmdResult)
	if cmdResult then
		repeat
			local cmdId = Result.getNumber(cmdResult, "id")
			local botName = Result.getString(cmdResult, "bot_name")
			local cmd = Result.getString(cmdResult, "command")

			local resultText
			-- Global reload: parse `reload [debug,N | debug off]`
			-- _global / reload              → plain hot-reload, preserve current active set
			-- _global / reload debug,1      → hot-reload + activate 1 bot with debug stream
			-- _global / reload debug off    → hot-reload + activate all bots, clear debug
			local low = cmd:lower()
			if botName == "_global" and (low == "reload" or low:match("^reload%s")) then
				local opts = { source = "queue" }
				-- Parse from `cmd` (original case), NOT `low` — bot names must survive.
				local dbgSpec, dbgNames = BotSystem.parseDebugSpec(cmd)
				if dbgSpec then
					opts.debugCount = dbgSpec
					opts.debugBotNames = dbgNames
				end
				local rv = BotSystem.executeReload(opts)
				resultText = (rv and rv.message) or "reload returned nil"
			elseif low == "storages" then
				resultText = BotStorages.dump(botName)
			else
				-- 3rd arg labels the BOT_TEST_HARNESS `cmd` audit record only.
				resultText = Game.botCommand(botName, cmd, "queue") or ""
				-- Reply text is data, not a format string: JSON replies carry braces.
				logger.info("[BotManager] Command '{}' for '{}': {}", cmd, botName, resultText)
			end

			-- Persist result + timestamp (mark processed in same statement)
			db.botAsyncQuery(string.format(
				"UPDATE `bot_commands` SET `processed` = 1, `result` = %s, `executed_at` = %d WHERE `id` = %d",
				db.escapeString(resultText:sub(1, BOT_CMD_RESULT_MAX)), os.time(), cmdId))
		until not Result.next(cmdResult)
		Result.free(cmdResult)
	end
	end)

	return true
end

botManager:interval(BOT_CONFIG.MANAGER_INTERVAL)
botManager:register()

-- Shutdown handler: save all active bot states to DB before server stops.
-- Sets should_restore_states=1 so next boot restores where bots left off.
local botShutdown = GlobalEvent("BotShutdown")

function botShutdown.onShutdown()
	logger.info("[BotManager] Server shutting down — saving bot states...")
	Game.botSaveStates()
	db.query(
		"INSERT INTO `bot_startup_config` (`config_key`, `config_value`) "
		.. "VALUES ('should_restore_states', '1') "
		.. "ON DUPLICATE KEY UPDATE `config_value` = '1'"
	)
	logger.info("[BotManager] Bot states saved. Restore flag set for next boot.")
	return true
end

botShutdown:register()
