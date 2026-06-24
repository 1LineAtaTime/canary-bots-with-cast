-- ============================================================================
-- Bot Manager -- Load bots on startup
-- Bots load at their last saved position (players.posx/y/z from normal server save).
-- Inactive bots are truly offline — removed from the world when deactivated.
-- Use /cavebot <name> active|inactive to control bots.
-- ============================================================================

-- Staging position — kept as fallback for brand-new bots with no prior loginPosition.
-- INACTIVE_STAGING_POS is no longer used as the "home" for inactive bots.
-- local INACTIVE_STAGING_POS = Position(31970, 32283, 7)

-- Startup: load all bot players from database
local botStartup = GlobalEvent("BotStartup")

function botStartup.onStartup()
	if BOT_CONFIG.MASTER_DISABLE then
		logger.warn("[BotManager] MASTER_DISABLE=true — bot startup skipped (lag-investigation Test 1)")
		return true
	end
	logger.info("[BotManager] Starting bot system...")

	-- Clear stale cast_broadcasters from previous run/crash
	db.query("DELETE FROM `cast_broadcasters`")

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

	-- Load bots with staggered timing to avoid startup spike (100ms per bot)
	for i, name in ipairs(botNames) do
		addEvent(function()
			local player = Game.loadBotPlayer(name)
			if player then
				local guid = player:getGuid()
				BotPlayers[guid] = player

				-- Register death handler
				player:registerEvent("BotDeath")

				-- Log every 10th bot
				if i % 10 == 0 or i == #botNames then
					logger.info("[BotManager] Loaded bot " .. i .. "/" .. #botNames .. ": " .. name)
				end
			else
				logger.warn("[BotManager] Failed to load bot: " .. name)
			end

			-- After all bots loaded, set required quest/access storages
			if i == #botNames then
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
					-- Set quest storages so bots can use wagons, shrines, levers, teleports
					local botStorages = {
						-- The New Frontier (Farmine access — full quest complete)
						[Storage.Quest.U8_54.TheNewFrontier.Questline] = 29,  -- final state per freequests
						[Storage.Quest.U8_54.TheNewFrontier.Mission01] = 3,
						[Storage.Quest.U8_54.TheNewFrontier.Mission02[1]] = 4,
						[Storage.Quest.U8_54.TheNewFrontier.Mission02.Beaver1] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission02.Beaver2] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission02.Beaver3] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission03] = 3,
						[Storage.Quest.U8_54.TheNewFrontier.Mission04] = 2,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05[1]] = 2,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05.KingTibianus] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05.Leeland] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05.Angus] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05.Wyrdin] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05.Telas] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission05.Humgolf] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.Mission06] = 5,
						[Storage.Quest.U8_54.TheNewFrontier.Mission07[1]] = 2,
						[Storage.Quest.U8_54.TheNewFrontier.Mission08] = 2,
						[Storage.Quest.U8_54.TheNewFrontier.Mission09[1]] = 3,
						[Storage.Quest.U8_54.TheNewFrontier.Mission10[1]] = 2,
						[Storage.Quest.U8_54.TheNewFrontier.Mission10.MagicCarpetDoor] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.TomeofKnowledge] = 12,  -- all 10 tome rewards from Cael (Snake Teleport, Corruption Hole, Zao Palace, etc.)
						[Storage.Quest.U8_54.TheNewFrontier.ZaoPalaceDoors] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.SnakeHeadTeleport] = 1,
						[Storage.Quest.U8_54.TheNewFrontier.CorruptionHole] = 1,
						-- Children of the Revolution (Dragonblaze Peaks / Muggy Plains teleport access — quest shown complete)
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Questline] = 21,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Mission00] = 2,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Mission01] = 3,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Mission02] = 5,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Mission03] = 3,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Mission04] = 6,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.Mission05] = 3,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.SpyBuilding01] = 1,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.SpyBuilding02] = 1,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.SpyBuilding03] = 1,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.StrangeSymbols] = 1,
						[Storage.Quest.U8_54.ChildrenOfTheRevolution.teleportAccess] = 1,
						-- Wrath of the Emperor (Zao teleport network — mystic flames at (33136,31248/9,6) need TeleportAccess.Rebel + BossRoom)
						[Storage.Quest.U8_6.WrathOfTheEmperor.Questline] = 29,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission01] = 3,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission02] = 3,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission03] = 3,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission04] = 3,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission05] = 3,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission06] = 5,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission07] = 6,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission08] = 2,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission09] = 2,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission10] = 6,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission11] = 2,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Mission12] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.Rebel] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.Zlak] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.Zizzle] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.SleepingDragon] = 2,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.InnerSanctum] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.AwarnessEmperor] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.Wote10] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.TeleportAccess.BossRoom] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Bosses.Fury] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Bosses.Wrath] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Bosses.Scorn] = 1,
						[Storage.Quest.U8_6.WrathOfTheEmperor.Bosses.Spite] = 1,
						-- Threatened Dreams (Feyrist shrine access)
						[Storage.Quest.U11_40.ThreatenedDreams.Mission01[1]] = 16,
						-- In Service of Yalahar (all 6 city gates: Alchemist/Trade/Arena/Cemetery/Sunken/Factory — quest shown complete)
						[Storage.Quest.U8_4.InServiceOfYalahar.TheWayToYalahar] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.Questline] = 51,  -- >= 45 unlocks all gate mechanisms
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission01] = 6,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission02] = 8,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission03] = 6,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission04] = 6,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission05] = 8,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission06] = 5,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission07] = 5,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission08] = 4,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission09] = 2,
						[Storage.Quest.U8_4.InServiceOfYalahar.Mission10] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SewerPipe01] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SewerPipe02] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SewerPipe03] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SewerPipe04] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DiseasedDan] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DiseasedBill] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DiseasedFred] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.AlchemistFormula] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.BadSide] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.GoodSide] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.MrWestDoor] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.MrWestStatus] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.TamerinStatus] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.MorikSummon] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.QuaraState] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.QuaraSplasher] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.QuaraSharptooth] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.QuaraInky] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.MatrixState] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.NotesPalimuth] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.NotesAzerus] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DoorToAzerus] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DoorToBog] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DoorToLastFight] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DoorToMatrix] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.DoorToQuara] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.TownsCounter] = 5,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.AbDendriel] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.Darashia] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.Venore] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.Ankrahmun] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.PortHope] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.Thais] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.LibertyBay] = 1,
						[Storage.Quest.U8_4.InServiceOfYalahar.SearoutesAroundYalahar.Carlin] = 1,
						-- Forgotten Knowledge (Thais fire portal hub + all element teleports — quest shown complete)
						[Storage.Quest.U11_02.ForgottenKnowledge.Tomes] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessDeath] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessViolet] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessEarth] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessFire] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessIce] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessGolden] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessLast] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessPortals] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessMachine] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.AccessLavaTeleport] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.LadyTenebrisKilled] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.LloydKilled] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.ThornKnightKilled] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.DragonkingKilled] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.HorrorKilled] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.TimeGuardianKilled] = 1,
						[Storage.Quest.U11_02.ForgottenKnowledge.LastLoreKilled] = 1,
						-- The Secret Library (full questline complete — Asura Palace mirror, Lotus/Eye Key chests, library teleport)
						[Storage.Quest.U11_80.TheSecretLibrary.Questlog] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.LibraryPermission] = 7,  -- 7 = quest done, unlocks library teleport at (32177,31925,7)
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.Questline] = 7,   -- max value (mission complete)
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.FlammingOrchid] = 1,  -- required for Asura Palace mirror (aid 4910)
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.SkeletonNotes] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.StrandHair] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.LotusKey] = 1,     -- chest collected flag (key item: 28476)
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.EyeKey] = 1,       -- chest collected flag (key item: 28477)
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.ScribbledNotes] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.EbonyPiece] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.PeacockBallad] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.BlackSkull] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.SilverChimes] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.Asuras.Fragrance] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.FalconBastion.Questline] = 3,
						[Storage.Quest.U11_80.TheSecretLibrary.FalconBastion.FalconBastionAccess] = 1,
						-- KillingBosses gates the Falcon Bastion doors (>=4), boats (>=3) and boss
						-- entrance (>=5) via actions_doors.lua/movements_bossEntrance.lua. 5 = all open.
						-- Required by the "Falcons" hunt route (use: waypoints land on these doors).
						[Storage.Quest.U11_80.TheSecretLibrary.FalconBastion.KillingBosses] = 5,
						[Storage.Quest.U11_80.TheSecretLibrary.Darashia.Questline] = 9,
						[Storage.Quest.U11_80.TheSecretLibrary.LiquidDeath.Questline] = 8,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.Questline] = 8,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.LeverPermission] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.FinalBasin] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.SkullSample] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.YellowGem] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.GreenGem] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.MoTA.RedGem] = 1,
						[Storage.Quest.U11_80.TheSecretLibrary.SmallIslands.Questline] = 4,
						-- Kilmaresh (Issavi sphynx caves / Cobra Bastion region access — mirrors freequests).
						[Storage.Quest.U12_20.KilmareshQuest.AccessDoor] = 1,
						[Storage.Quest.U12_20.KilmareshQuest.Second.Investigating] = 1,
						[Storage.Quest.U12_20.KilmareshQuest.Sixth.GryphonMask] = 1,
						[Storage.Quest.U12_20.KilmareshQuest.Sixth.MirrorMask] = 1,
						[Storage.Quest.U12_20.KilmareshQuest.Sixth.IvoryMask] = 1,
						[Storage.Quest.U12_20.KilmareshQuest.Sixth.SilverMask] = 1,
						-- Ferumbras Ascension (seal-room hunt access — mirrors freequests + main entrance Access).
						[Storage.Quest.U10_90.FerumbrasAscension.Access] = 1,
						[Storage.Quest.U10_90.FerumbrasAscension.FirstDoor] = 1,
						[Storage.Quest.U10_90.FerumbrasAscension.MonsterDoor] = 1,
						[Storage.Quest.U10_90.FerumbrasAscension.TarbazDoor] = 1,
						[Storage.Quest.U10_90.FerumbrasAscension.HabitatsAccess] = 1,
						[Storage.Quest.U10_90.FerumbrasAscension.TheLordOfTheLiceAccess] = 1,
						[Storage.Quest.U10_90.FerumbrasAscension.Statue] = 1,
						-- The Lost Brother (single-storage quest)
						[Storage.Quest.U10_80.TheLostBrotherQuest] = 3,
						-- Lion's Rock (Darashia inner sanctum — entrance teleport at (33128,32308,8)
						-- gates on Questline >= 4; 11 is the final post-reward value)
						[Storage.Quest.U10_70.LionsRock.Questline] = 11,
						[Storage.Quest.U10_70.LionsRock.OuterSanctum.Skeleton] = 1,
						[Storage.Quest.U10_70.LionsRock.OuterSanctum.LionsStrength] = 1,
						[Storage.Quest.U10_70.LionsRock.OuterSanctum.LionsBeauty] = 1,
						[Storage.Quest.U10_70.LionsRock.OuterSanctum.LionsTears] = 1,
						[Storage.Quest.U10_70.LionsRock.InnerSanctum.SnakeSign] = 1,
						[Storage.Quest.U10_70.LionsRock.InnerSanctum.LizardSign] = 1,
						[Storage.Quest.U10_70.LionsRock.InnerSanctum.ScorpionSign] = 1,
						[Storage.Quest.U10_70.LionsRock.InnerSanctum.HyenaSign] = 1,
						[Storage.Quest.U10_70.LionsRock.InnerSanctum.Message] = 1,
						-- Wagon Ticket (Kazordoon ore wagons — year 2099)
						[Storage.WagonTicket] = 2147483647,  -- max INT32 (~year 2038)
					}
					local storageCount = 0
					for guid, player in pairs(BotPlayers) do
						for key, value in pairs(botStorages) do
							player:setStorageValue(key, value)
						end
						storageCount = storageCount + 1
					end
					logger.info("[BotManager] Set " .. storageCount .. " bot quest/access storages (wagons, shrines, quests)")
					logger.info("[BotManager] All " .. #botNames .. " bots loaded as INACTIVE.")
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
								-- restoreAllStates() already deletes bot_state_persistence rows internally.
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
		end, i * 100)
	end

	return true
end

botStartup:register()

-- Population manager: DISABLED — only polls bot_commands table for admin control
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
				local debugMatch = low:match("^reload%s+debug%s*,%s*(%d+)")
				if debugMatch then
					opts.debugCount = tonumber(debugMatch)
				elseif low:match("^reload%s+debug%s+off") or low:match("^reload%s+debug%-off") then
					opts.debugCount = "off"
				end
				local rv = BotSystem.executeReload(opts)
				resultText = (rv and rv.message) or "reload returned nil"
			else
				resultText = Game.botCommand(botName, cmd) or ""
				logger.info("[BotManager] Command '" .. cmd .. "' for '" .. botName .. "': " .. resultText)
			end

			-- Persist result + timestamp (mark processed in same statement)
			db.botAsyncQuery(string.format(
				"UPDATE `bot_commands` SET `processed` = 1, `result` = %s, `executed_at` = %d WHERE `id` = %d",
				db.escapeString(resultText:sub(1, 4096)), os.time(), cmdId))
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
