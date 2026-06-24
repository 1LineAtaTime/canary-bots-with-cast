-- ============================================================================
-- Bot Market Scheduler
-- ============================================================================
-- Three independent passes drive realistic bot market activity:
--
--   1. SELLER PASS (30-120s random):
--      Pick BotMarket.SELLER_BOTS_PCT% of all bots (5% of 200 = 10 bots, jittered
--      ±30% → ~7-13 bots) via BotMarket.pickActiveBots (distinct picks).
--      Each picked bot generates 1-3 offers via Game.botCreateMarketOffer.
--      Direction 60% SELL / 40% BUY, 25% anonymous, all tiers available.
--      Per-bot cap: BotMarket.MAX_OFFERS_PER_BOT (100, matches server cap).
--      Pricing: U(floor, market_max × U(0.95, 1.10)) for SELL,
--               U(market_max × 0.70, market_max × 0.95) for BUY.
--      Stack sizes: stackable U(250, 2000), non-stackable equipment 1.
--
--   2. BUYER PASS (600-1800s random):
--      Browse real-player SELL offers. Accept if listed price < market_max × 0.9 AND
--      coin flip. 1-3 acceptances per pass via Game.botAcceptMarketOffer.
--
--   3. FULFILLER PASS (600-1800s random):
--      Browse real-player BUY offers. Fulfill at exact listed price if listed price
--      ≥ market_max × 0.95 AND coin flip. 1-3 fulfillments per pass.
--
-- Ramp-up: for the first 4 hours after server start the per-pass batch size is
-- scaled by min(1, hoursElapsed / 4) so the market doesn't dump 1200 listings at
-- the start of every restart.
-- ============================================================================

local SERVER_START_TIME = os.time()
local RAMP_UP_HOURS = 4

-- TEMP (2026-05-21): empirical test for GAP_SLOW root cause.
-- Set to false to disable seller/buyer/fulfiller passes (monitor still runs).
-- Confirmed 2026-05-21: market disable did NOT reduce GAP_SLOW rate (~45/hr both
-- enabled and disabled). Market is exonerated; flag kept for future ad-hoc tests.
local PASSES_ENABLED = true

local function rampScale()
	local elapsedH = (os.time() - SERVER_START_TIME) / 3600
	if elapsedH >= RAMP_UP_HOURS then return 1.0 end
	return math.min(1.0, elapsedH / RAMP_UP_HOURS)
end

local function scaleCount(count)
	-- Always at least 1 if the unscaled count was at least 1
	local s = math.floor(count * rampScale() + 0.5)
	if s < 1 and count >= 1 then s = 1 end
	return s
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Lazy initialization
-- ─────────────────────────────────────────────────────────────────────────────

local function ensureLoaded()
	if not BotMarket then
		logger.warn("[BotMarket] BotMarket library not loaded; skipping pass")
		return false
	end
	if not BotMarket.loaded then
		BotMarket.loadAll()
	end
	-- Race fix: bot_active_players is populated by bot_engine.cpp::registerBot via
	-- g_databaseTasks().execute() (async queue), so the rows may not have committed
	-- by the time BotMarket.loadAll() runs at the first market pass (t≈30s, while
	-- BotStartup is still staggered-loading until t≈20s and DatabaseTasks drains
	-- shortly after). If _loadBots ran before the queue drained, botGuids stayed
	-- empty AND BotMarket.loaded=true, permanently disabling the market until the
	-- next server restart. Retry just the bot list here (cheap — single indexed
	-- SELECT) without invalidating the expensive prices cache.
	if #BotMarket.botGuids == 0 then
		BotMarket._loadBots()
		if #BotMarket.botGuids == 0 then return false end
	end
	-- Need at least one item with a price
	local total = 0
	for _, list in pairs(BotMarket.itemsByBin) do total = total + #list end
	return total > 0
end

-- ─────────────────────────────────────────────────────────────────────────────
-- SELLER PASS: bots create offers
-- ─────────────────────────────────────────────────────────────────────────────

-- JITTER telemetry 2026-06-10: [MARKET_PASS] fire/duration logging. The buyer and
-- fulfiller passes fire on random 600-1800s timers with the heaviest MySQL footprint
-- in steady state (filesort over the 75k-row market_offers table) and previously
-- left ZERO journal trace — lagmark correlation was untestable. qlat (queue ->
-- callback latency) doubles as a free mysqld-busyness probe at each fire.
-- Game.monotonicMs() now returns a FRESH clock read (see luaGameMonotonicMs), so
-- in-task wall timings are real.
local function marketNowMs()
	return Game.monotonicMs and Game.monotonicMs() or (os.time() * 1000)
end

local sellerNextFireAt = 0
local sellerPass = GlobalEvent("BotMarketSeller")

local function runSellerPass()
	if not ensureLoaded() then return end
	local passT0 = marketNowMs()
	local offersAttempted = 0

	-- Target N bots = SELLER_BOTS_PCT% of fleet (e.g. 5% of 200 = 10), then jitter ±30%
	-- to preserve some variance, then ramp-scale during the first 4 hours.
	local targetBots = math.max(1, math.floor(#BotMarket.botGuids * BotMarket.SELLER_BOTS_PCT / 100))
	local jitterLo = math.max(1, math.floor(targetBots * 0.7))
	local jitterHi = math.ceil(targetBots * 1.3)
	local jittered = math.random(jitterLo, jitterHi)
	local botCount = scaleCount(jittered)

	local picks = BotMarket.pickActiveBots(botCount)

	-- JITTER FIX 2026-06-11 (bundle 3): the per-pick sync BotMarket.getOfferCount
	-- round-trip is gone — the per-player cap is now enforced atomically inside
	-- Game.botCreateMarketOffer's cap-guarded async INSERT, so this loop does no
	-- DB work on the dispatcher at all (measured 50-243ms COUNTs in convoy before).
	-- BUNDLE 5b (13:34 lagmark): cap INSERTs per fire — 16 enqueued at once occupied
	-- the shared pool workers that asyncWait needs for the parallel monster-AI
	-- partition (await=3260ms). 4 per fire keeps the burst sub-perceptible.
	local FIRE_INSERT_CAP = 4
	for _, botGuid in ipairs(picks) do
		if offersAttempted >= FIRE_INSERT_CAP then break end
		local offers = math.random(1, 3)
		for _ = 1, offers do
			if offersAttempted >= FIRE_INSERT_CAP then break end
			local itemId = BotMarket.pickItem()
			if itemId then
				local action = BotMarket.rollDirection()
				local actionStr = (action == MARKETACTION_SELL) and "SELL" or "BUY"
				local price = BotMarket.computeListingPrice(itemId, actionStr)
				if price and price > 0 then
					local entry = BotMarket.prices[itemId] or {}
					local tier = BotMarket.rollTier(entry.upgrade_class)
					local amount = BotMarket.rollStackSize(itemId)
					local anon = BotMarket.rollAnonymous()

					-- Wrapper logs warnings on failure; we don't surface every reject
					Game.botCreateMarketOffer(botGuid, action, itemId, amount, price, tier, anon)
					offersAttempted = offersAttempted + 1
				end
			end
		end
	end

	logger.info(string.format("[MARKET_PASS] pass=seller picks=%d offersAttempted=%d wall=%dms",
		#picks, offersAttempted, marketNowMs() - passT0))
end

function sellerPass.onThink(interval)
	if not PASSES_ENABLED then return true end
	if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end
	local now = os.time()
	if now < sellerNextFireAt then return true end
	sellerNextFireAt = now + math.random(BotMarket.SELLER_INTERVAL_MIN, BotMarket.SELLER_INTERVAL_MAX)

	-- Wrap in pcall so a single error doesn't kill the GlobalEvent
	local ok, err = pcall(runSellerPass)
	if not ok then
		logger.warn("[BotMarketSeller] pass error: " .. tostring(err))
	end
	return true
end

sellerPass:interval(30000) -- 30s polling tick; actual firing controlled by sellerNextFireAt
sellerPass:register()

-- ─────────────────────────────────────────────────────────────────────────────
-- BUYER PASS: bot accepts cheap real-player SELL offers
-- ─────────────────────────────────────────────────────────────────────────────

local buyerNextFireAt = 0
local buyerPass = GlobalEvent("BotMarketBuyer")

local function runBuyerPass()
	if not ensureLoaded() then return end
	local qT0 = marketNowMs()

	-- PERF_INVESTIGATION_2026-05-24 Tier 1-E-b: async DB SELECT — the prior
	-- sync db.storeQuery blocked the dispatcher for the round-trip latency
	-- (counted in [DB_SYNC_DISPATCHER] telemetry). asyncStoreQuery hands the
	-- query to g_databaseTasks() worker and invokes the callback when results
	-- are ready, leaving the dispatcher free to service other tasks meanwhile.
	local q = string.format(
		"SELECT `id`, `player_id`, `itemtype`, `amount`, `price`, `tier` " ..
		"FROM `market_offers` WHERE `sale` = 1 AND `player_id` NOT IN " ..
		"(SELECT `id` FROM `players` WHERE `account_id` = %d) " ..
		"ORDER BY `price` ASC LIMIT 500",
		BotMarket.BOT_ACCOUNT_ID
	)
	db.botAsyncStoreQuery(q, function(resultId)
		local cbT0 = marketNowMs()
		if not resultId then
			logger.info(string.format("[MARKET_PASS] pass=buyer qlat=%dms rows=0 accepted=0 cb=0ms", cbT0 - qT0))
			return
		end

		local maxAcceptances = scaleCount(math.random(1, 3))
		local accepted = 0
		local rows = 0

		repeat
			rows = rows + 1
			if accepted >= maxAcceptances then break end

			local offerId = Result.getNumber(resultId, "id")
			local itemId = Result.getNumber(resultId, "itemtype")
			local amount = Result.getNumber(resultId, "amount")
			local price = Result.getNumber(resultId, "price")

			local fair = BotMarket.prices[itemId]
			if fair and fair.market_max and fair.market_max > 0 then
				if price < fair.market_max * BotMarket.BUY_DEAL_THRESHOLD
					and math.random() < BotMarket.BUY_DEAL_PROBABILITY then
					-- Pick a buyer bot with sufficient balance
					local total = price * amount
					local buyerGuid = nil
					for _, candidate in ipairs(BotMarket.botGuids) do
						local p = Player(candidate)
						if p and p:getBankBalance() >= total then
							buyerGuid = candidate
							break
						end
					end
					if buyerGuid and Game.botAcceptMarketOffer(buyerGuid, offerId, amount) then
						accepted = accepted + 1
					end
				end
			end
		until not Result.next(resultId)
		Result.free(resultId)
		logger.info(string.format("[MARKET_PASS] pass=buyer qlat=%dms rows=%d accepted=%d cb=%dms",
			cbT0 - qT0, rows, accepted, marketNowMs() - cbT0))
	end)
end

function buyerPass.onThink(interval)
	if not PASSES_ENABLED then return true end
	local now = os.time()
	if now < buyerNextFireAt then return true end
	buyerNextFireAt = now + math.random(BotMarket.BUYER_INTERVAL_MIN, BotMarket.BUYER_INTERVAL_MAX)

	local ok, err = pcall(runBuyerPass)
	if not ok then
		logger.warn("[BotMarketBuyer] pass error: " .. tostring(err))
	end
	return true
end

buyerPass:interval(60000) -- 60s polling
buyerPass:register()

-- ─────────────────────────────────────────────────────────────────────────────
-- FULFILLER PASS: bot fulfills real-player BUY offers (sells to them at listed price)
-- ─────────────────────────────────────────────────────────────────────────────

local fulfillerNextFireAt = 0
local fulfillerPass = GlobalEvent("BotMarketFulfiller")

local function runFulfillerPass()
	if not ensureLoaded() then return end
	local qT0 = marketNowMs()

	-- PERF Tier 1-E-b: async query (see runBuyerPass comment)
	local q = string.format(
		"SELECT `id`, `player_id`, `itemtype`, `amount`, `price`, `tier` " ..
		"FROM `market_offers` WHERE `sale` = 0 AND `player_id` NOT IN " ..
		"(SELECT `id` FROM `players` WHERE `account_id` = %d) " ..
		"ORDER BY `price` DESC LIMIT 500",
		BotMarket.BOT_ACCOUNT_ID
	)
	db.botAsyncStoreQuery(q, function(resultId)
		local cbT0 = marketNowMs()
		if not resultId then
			logger.info(string.format("[MARKET_PASS] pass=fulfiller qlat=%dms rows=0 fulfilled=0 cb=0ms", cbT0 - qT0))
			return
		end

		local maxFulfillments = scaleCount(math.random(1, 3))
		local fulfilled = 0
		local rows = 0

		repeat
			rows = rows + 1
			if fulfilled >= maxFulfillments then break end

			local offerId = Result.getNumber(resultId, "id")
			local itemId = Result.getNumber(resultId, "itemtype")
			local amount = Result.getNumber(resultId, "amount")
			local price = Result.getNumber(resultId, "price")

			local fair = BotMarket.prices[itemId]
			if fair and fair.market_max and fair.market_max > 0 then
				if price >= fair.market_max * BotMarket.FULFILL_THRESHOLD
					and math.random() < BotMarket.FULFILL_PROBABILITY then
					local sellerGuid = BotMarket.pickActiveBot()
					if sellerGuid and Game.botAcceptMarketOffer(sellerGuid, offerId, amount) then
						fulfilled = fulfilled + 1
					end
				end
			end
		until not Result.next(resultId)
		Result.free(resultId)
		logger.info(string.format("[MARKET_PASS] pass=fulfiller qlat=%dms rows=%d fulfilled=%d cb=%dms",
			cbT0 - qT0, rows, fulfilled, marketNowMs() - cbT0))
	end)
end

function fulfillerPass.onThink(interval)
	if not PASSES_ENABLED then return true end
	local now = os.time()
	if now < fulfillerNextFireAt then return true end
	fulfillerNextFireAt = now + math.random(BotMarket.BUYER_INTERVAL_MIN, BotMarket.BUYER_INTERVAL_MAX)

	local ok, err = pcall(runFulfillerPass)
	if not ok then
		logger.warn("[BotMarketFulfiller] pass error: " .. tostring(err))
	end
	return true
end

fulfillerPass:interval(60000)
fulfillerPass:register()

-- ─────────────────────────────────────────────────────────────────────────────
-- Periodic monitoring (every 5 min): log market activity stats
-- ─────────────────────────────────────────────────────────────────────────────

local monitor = GlobalEvent("BotMarketMonitor")

-- JITTER FIX 2026-06-11 (bundle 3): rolling age purge of bot offers.
-- market_offers grew unbounded (75.6k rows, +~1k/day) which made every scan over
-- the table slower forever — including the STOCK 30-min Game::loadItemsPrice
-- refresh (sync on the dispatcher, 280-440ms and growing = a lagmark class).
-- Purging bot offers older than PURGE_MAX_AGE_SECS also guarantees none ever
-- reaches the 30-day expiry, pre-empting the 2026-07-02 avalanche through the
-- stock IOMarket::checkExpiredOffers sweep (per-offer sync work on dispatcher)
-- WITHOUT touching any stock code. Batch-limited DELETE runs on the DB worker;
-- steady state ≈ purge-age × creation rate (~7d x ~1k/day ≈ 7k rows).
local PURGE_MAX_AGE_SECS = 7 * 86400
-- BUNDLE 5b (13:34 lagmark): purge moved OUT of monitor.onThink to its own timer,
-- phase-OFFSET ~2.5min from the monitor's 5-min stats scan — the DELETE batch,
-- the 50k-row stats aggregation and a 16-INSERT seller burst all landing in the
-- same second occupied the shared pool workers asyncWait needs (await=3260ms ->
-- lagmark). Batch also shrunk 500->300 to bound per-burst mysqld hold time.
local PURGE_BATCH = 300
local purgeNextFireAt = os.time() + 150 -- first fire offset to sit between monitor fires
local purgePass = GlobalEvent("BotMarketPurge")

function purgePass.onThink(interval)
	if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end
	if not BotMarket or not BotMarket.BOT_ACCOUNT_ID then return true end
	local now = os.time()
	if now < purgeNextFireAt then return true end
	purgeNextFireAt = now + 300
	db.botAsyncQuery(string.format(
		"DELETE FROM `market_offers` WHERE `player_id` IN (SELECT `id` FROM `players` WHERE `account_id` = %d) AND `created` < %d LIMIT %d",
		BotMarket.BOT_ACCOUNT_ID, now - PURGE_MAX_AGE_SECS, PURGE_BATCH
	))
	return true
end

purgePass:interval(60000) -- 60s poll; actual firing gated by purgeNextFireAt
purgePass:register()

function monitor.onThink(interval)
	if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end
	if not BotMarket or not BotMarket.BOT_ACCOUNT_ID then return true end -- review WARN: defensive nil guard
	local qT0 = marketNowMs()
	-- PERF Tier 1-E-b: async query (see runBuyerPass comment)
	db.botAsyncStoreQuery(string.format(
		"SELECT " ..
		"  SUM(CASE WHEN `sale`=1 AND `player_id` IN (SELECT id FROM players WHERE account_id=%d) THEN 1 ELSE 0 END) AS `bot_sells`, " ..
		"  SUM(CASE WHEN `sale`=0 AND `player_id` IN (SELECT id FROM players WHERE account_id=%d) THEN 1 ELSE 0 END) AS `bot_buys`, " ..
		"  SUM(CASE WHEN `sale`=1 AND `player_id` NOT IN (SELECT id FROM players WHERE account_id=%d) THEN 1 ELSE 0 END) AS `real_sells`, " ..
		"  SUM(CASE WHEN `sale`=0 AND `player_id` NOT IN (SELECT id FROM players WHERE account_id=%d) THEN 1 ELSE 0 END) AS `real_buys` " ..
		"FROM `market_offers`",
		BotMarket.BOT_ACCOUNT_ID, BotMarket.BOT_ACCOUNT_ID, BotMarket.BOT_ACCOUNT_ID, BotMarket.BOT_ACCOUNT_ID
	), function(resultId)
		if not resultId then return end
		local bs = Result.getNumber(resultId, "bot_sells") or 0
		local bb = Result.getNumber(resultId, "bot_buys") or 0
		local rs = Result.getNumber(resultId, "real_sells") or 0
		local rb = Result.getNumber(resultId, "real_buys") or 0
		-- qlat = queue->callback latency of the 75k-row aggregation: a free mysqld
		-- busyness probe every 5 min (JITTER telemetry 2026-06-10).
		logger.info(string.format(
			"[BotMarketMonitor] active offers: bot=%d (S=%d B=%d), real=%d (S=%d B=%d), ramp=%.0f%%, qlat=%dms",
			bs + bb, bs, bb, rs + rb, rs, rb, rampScale() * 100, marketNowMs() - qT0
		))
		Result.free(resultId)
	end)
	return true
end

monitor:interval(300000) -- 5 min
monitor:register()
