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
--   2. ACCEPT SWEEP (botMarketAcceptIntervalMin..MaxSec, default 900-2700s):
--      Looks at EVERY real-player offer, both directions, and gives each one its own
--      independent roll at botMarketAcceptChancePct. Winners are price-gated against a
--      reference value (market_max, else NPC price, else the botMarketNoref* guards),
--      then a PARTIAL quantity of 1..offerAmount is taken, then the acceptances are
--      paced one per botMarketAcceptDrainMs. See the section header further down.
--
--      This replaced the old BUYER and FULFILLER passes, which shared a per-fire budget
--      of 1-3 acceptances taken in strict price order — so whoever held the cheapest
--      offers absorbed it and everyone else's orders were never reached.
--
-- Ramp-up: for the first 4 hours after server start the SELLER pass's batch size is
-- scaled by min(1, hoursElapsed / 4) so the market doesn't dump 1200 listings at
-- the start of every restart. It deliberately does NOT apply to the accept sweep:
-- there it silently capped acceptances to one per fire for the first two hours.
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
	local covFirst, rerolls, capBlocked, failed = 0, 0, 0, 0
	for _, botGuid in ipairs(picks) do
		if offersAttempted >= FIRE_INSERT_CAP then break end
		local offers = math.random(1, 3)
		for _ = 1, offers do
			if offersAttempted >= FIRE_INSERT_CAP then break end
			-- Direction and item are chosen together: saturation is a property of the (item, side)
			-- pair, so the side has to be settled first.
			local itemId, action, tag, rr = BotMarket.pickOffer()
			rerolls = rerolls + (rr or 0)
			if not itemId then
				-- Every candidate for this side was already at its depth cap.
				capBlocked = capBlocked + 1
			else
				if tag == "coverage" or tag == "coverage_fallback" then covFirst = covFirst + 1 end
				local actionStr = (action == MARKETACTION_SELL) and "SELL" or "BUY"
				local price = BotMarket.computeListingPrice(itemId, actionStr)
				if price and price > 0 then
					local entry = BotMarket.prices[itemId] or {}
					local tier = BotMarket.rollTier(entry.upgrade_class)
					local amount = BotMarket.rollStackSize(itemId, action)
					local anon = BotMarket.rollAnonymous()

					-- Wrapper logs warnings on failure; we don't surface every reject
					if Game.botCreateMarketOffer(botGuid, action, itemId, amount, price, tier, anon) then
						BotMarket.noteOfferCreated(itemId, action, amount)
					else
						-- Repeated rejects mean the item is un-listable (wareId=0) rather than the
						-- bot being briefly short of funds; noteOfferFailed drops it after 3.
						BotMarket.noteOfferFailed(itemId)
						failed = failed + 1
					end
					offersAttempted = offersAttempted + 1
				end
			end
		end
	end

	logger.info(string.format(
		"[MARKET_PASS] pass=seller picks=%d offersAttempted=%d covFirst=%d rerolls=%d capBlocked=%d failed=%d wall=%dms",
		#picks, offersAttempted, covFirst, rerolls, capBlocked, failed, marketNowMs() - passT0))
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
-- ─────────────────────────────────────────────────────────────────────────────
-- ACCEPT SWEEP: one independent roll per REAL-PLAYER offer
-- ─────────────────────────────────────────────────────────────────────────────
-- Replaces the old BUYER and FULFILLER passes, which were two near-identical loops that
-- each took a global budget of 1-3 acceptances per fire in strict price order. Two
-- consequences of that design, both fixed here:
--
--   * whoever held the cheapest SELL (or priciest BUY) offers absorbed the entire budget,
--     so with several players listing, most players' orders were never even reached;
--   * the budget was additionally scaled by the 4-hour restart ramp, which on a box that
--     restarts often meant a hard cap of ONE acceptance per fire, indefinitely.
--
-- The model here is: every real offer gets its OWN independent roll on every sweep. There
-- is no cross-player budget to monopolise, so N players with M orders each get N*M rolls.
-- The only remaining aggregate limit is the no-ref gold budget, which exists purely as an
-- exploit bound and never touches an offer that has a reference price.
--
-- Acceptances are then PACED, one per botMarketAcceptDrainMs, rather than settled inside
-- the query callback. That is not cosmetic: each Game.botAcceptMarketOffer performs
-- synchronous DB work on the dispatcher (an offline load of the bot, an offline load of the
-- real counterparty, getOfferById, inbox insertion, two appendHistory writes, a delete, and
-- a full savePlayer when the counterparty is offline). Settling a sweep's worth of those in
-- one tick is the same shape as the seller-pass INSERT burst that produced the 885ms
-- HB_STALL documented above.

-- Bumped every time this file is executed. `/reload scripts` re-runs it in the SAME Lua
-- state, so any drain closure still pending from the previous execution would otherwise
-- keep firing against the new generation's state. Each closure captures the generation it
-- was created in and abandons itself when it no longer matches.
BOT_MARKET_ACCEPT_GEN = (BOT_MARKET_ACCEPT_GEN or 0) + 1

local acceptNextFireAt = 0
local acceptInFlight = false
local acceptStartedAt = 0
-- Touched by every drain step. The stale-latch check below tests PROGRESS, not total
-- elapsed time: a large queue legitimately takes queueLen * drainMs to settle, which can
-- exceed a whole fire interval, and treating "still working" as "stuck" would clear the
-- latch and start the overlapping sweep the latch exists to prevent.
local acceptLastProgressAt = 0
-- No drain step has run for this many seconds => the chain really is dead (lost callback,
-- reload, addEvent dropped) rather than merely slow. Comfortably above drainMs.
local ACCEPT_STALL_SECS = 120
local acceptPass = GlobalEvent("BotMarketAccept")

-- Round-robin the winners across their owners so that when several players win on the same
-- sweep their acceptances land interleaved (A, B, A, B) instead of all of one player's
-- first. This is PACING and budget fairness, not acceptance fairness — whether an offer is
-- accepted was already decided by its own roll before this runs.
local function interleaveByPlayer(winners)
	local byPlayer, order = {}, {}
	for _, w in ipairs(winners) do
		local bucket = byPlayer[w.playerId]
		if not bucket then
			bucket = {}
			byPlayer[w.playerId] = bucket
			order[#order + 1] = w.playerId
		end
		bucket[#bucket + 1] = w
	end
	local out, round = {}, 1
	while #out < #winners do
		for _, pid in ipairs(order) do
			local w = byPlayer[pid][round]
			if w then out[#out + 1] = w end
		end
		round = round + 1
	end
	return out
end

-- Settle one queued acceptance per tick, then reschedule.
local scheduleDrain
scheduleDrain = function(queue, idx, gen, stats)
	addEvent(function()
		if gen ~= BOT_MARKET_ACCEPT_GEN then
			-- Script was reloaded under us; the new generation owns the state now.
			return
		end

		acceptLastProgressAt = os.time()

		local entry = queue[idx]
		if not entry then
			acceptInFlight = false
			logger.info(string.format(
				"[MARKET_ACCEPT] drained queue=%d accepted=%d units=%d rejected=%d noBot=%d errors=%d",
				#queue, stats.accepted, stats.qty, stats.failed, stats.noBot, stats.errors))
			return
		end

		-- Per-step pcall. The old passes wrapped one whole pass in a single pcall; splitting
		-- the work across addEvent callbacks creates a new failure boundary per step, and a
		-- throw here must not silently drop the rest of the queue.
		local ok, err = pcall(function()
			local botGuid = BotMarket.pickActiveBot()
			if not botGuid then
				-- Transient: botGuids can be empty between startup and the DatabaseTasks
				-- drain (see ensureLoaded's race note).
				stats.noBot = stats.noBot + 1
				return
			end
			-- pickActiveBot (not the old awake-only Player() scan): Game.botAcceptMarketOffer
			-- resolves the guid with getPlayerByGUID(guid, true), which offline-loads a
			-- hibernated bot and does its own funds check. The old loop could only ever
			-- select one of the 1-32 AWAKE bots and silently no-opped when none were.
			-- acceptAmount is the PARTIAL quantity rolled at selection time (1..offer amount),
			-- not the whole offer. botAcceptMarketOffer handles the partial fill: it leaves the
			-- offer open with the remainder when amount < offer.amount, and only deletes the row
			-- when the remainder hits zero.
			if Game.botAcceptMarketOffer(botGuid, entry.offerId, entry.acceptAmount) then
				stats.accepted = stats.accepted + 1
				stats.qty = stats.qty + entry.acceptAmount
			else
				-- Expected and harmless when the offer was cancelled or accepted by someone
				-- else between the roll and now: botAcceptMarketOffer returns false on
				-- offer.id == 0.
				stats.failed = stats.failed + 1
			end
		end)
		if not ok then
			stats.errors = stats.errors + 1
			logger.warn("[MARKET_ACCEPT] drain step error: " .. tostring(err))
		end

		-- Unconditional reschedule, success or failure.
		scheduleDrain(queue, idx + 1, gen, stats)
	end, BotMarket.ACCEPT_DRAIN_MS)
end

local function runAcceptPass()
	if not ensureLoaded() then return end
	-- 0 disables the sweep entirely: math.random(1,100) <= 0 is never true, so rolling
	-- would just burn a query every interval.
	if BotMarket.ACCEPT_CHANCE_PCT <= 0 then return end

	-- Never let two sweeps overlap: the second would re-roll offers the first has already
	-- queued but not yet settled. Same latch shape as BotMarket.refreshDepth, including the
	-- never-latch-forever escape.
	if acceptInFlight then
		if os.time() - acceptLastProgressAt <= ACCEPT_STALL_SECS then
			logger.info(string.format(
				"[MARKET_ACCEPT] previous sweep still draining (%ds in) — skipping this fire",
				os.time() - acceptStartedAt))
			return
		end
		logger.warn(string.format(
			"[MARKET_ACCEPT] previous sweep stalled (no drain step for %ds) — clearing latch and re-running",
			os.time() - acceptLastProgressAt))
	end
	acceptInFlight = true
	acceptStartedAt = os.time()
	acceptLastProgressAt = acceptStartedAt

	local gen = BOT_MARKET_ACCEPT_GEN
	local qT0 = marketNowMs()

	-- One query for BOTH directions. No ORDER BY on purpose — the old passes' price
	-- ordering is exactly what biased the budget toward one player, and with an independent
	-- per-row roll there is nothing left for an ordering to do.
	local q = string.format(
		"SELECT `id`, `player_id`, `sale`, `itemtype`, `amount`, `price` FROM `market_offers` " ..
		"WHERE `player_id` NOT IN (SELECT `id` FROM `players` WHERE `account_id` = %d) LIMIT %d",
		BotMarket.BOT_ACCOUNT_ID, BotMarket.ACCEPT_ROW_LIMIT
	)
	db.botAsyncStoreQuery(q, function(resultId)
		if gen ~= BOT_MARKET_ACCEPT_GEN then return end
		local cbT0 = marketNowMs()

		-- Falsy resultId is a query ERROR *or* an empty table — storeQuery returns nullptr
		-- for both. Either way there is nothing to roll.
		if not resultId then
			acceptInFlight = false
			logger.info(string.format("[MARKET_ACCEPT] qlat=%dms rows=0 (no real offers)", cbT0 - qT0))
			return
		end

		local rows, rolled = 0, 0
		-- Units offered vs units actually taken, so the partial-fill behaviour is visible in
		-- the journal rather than only inferable from shrinking offer amounts.
		local qtyOffered, qtyRolled = 0, 0
		local reasons = {}
		local winners = {}
		local function note(reason)
			reasons[reason] = (reasons[reason] or 0) + 1
		end

		repeat
			rows = rows + 1
			local offerId = Result.getNumber(resultId, "id")
			local playerId = Result.getNumber(resultId, "player_id")
			local sale = Result.getNumber(resultId, "sale")
			local itemId = Result.getNumber(resultId, "itemtype")
			local amount = Result.getNumber(resultId, "amount")
			local price = Result.getNumber(resultId, "price")

			-- THE roll. One per offer, independent of every other offer and every other
			-- player. This is the whole fairness property.
			if math.random(1, 100) <= BotMarket.ACCEPT_CHANCE_PCT then
				rolled = rolled + 1
				local ok, reason = BotMarket.judgeOffer(sale, itemId, price, amount)
				note(reason)
				if ok then
					-- PARTIAL FILL: take a random slice of the order rather than all of it, so a
					-- 10-box order might go 3 now, 5 later, 2 later still — the way a real book
					-- gets eaten by several buyers instead of one instant clean sweep. amount=1
					-- degenerates to 1, so single-unit offers are unaffected.
					--
					-- Rolled HERE, at selection, not at drain time: the no-ref gold budget below
					-- has to be charged the quantity actually being bought, and rolling later
					-- would make it charge for units the sweep never takes.
					local acceptAmount = (amount > 1) and math.random(1, amount) or 1
					qtyOffered = qtyOffered + amount
					qtyRolled = qtyRolled + acceptAmount
					winners[#winners + 1] = {
						offerId = offerId,
						playerId = playerId,
						sale = sale,
						itemId = itemId,
						amount = amount,
						acceptAmount = acceptAmount,
						price = price,
					}
				end
			else
				note("roll_lost")
			end
		until not Result.next(resultId)
		Result.free(resultId)

		-- Apply the no-ref gold budget in INTERLEAVED order, not row order, so that when the
		-- budget is contested it is spread across players rather than consumed by whichever
		-- player's rows the query happened to return first.
		local ordered = interleaveByPlayer(winners)
		local queue, norefSpent = {}, 0
		for _, w in ipairs(ordered) do
			local cost = BotMarket.norefGoldCost(w.sale, w.itemId, w.price, w.acceptAmount)
			if cost > 0 and (norefSpent + cost) > BotMarket.NOREF_FIRE_GOLD_BUDGET then
				note("noref_budget_exhausted")
			else
				norefSpent = norefSpent + cost
				queue[#queue + 1] = w
			end
		end

		-- Reason breakdown, sorted for stable reading. Every rejection path has its own
		-- code, so "why did nothing happen" is answerable from one journal line.
		local parts = {}
		for reason, n in pairs(reasons) do
			parts[#parts + 1] = string.format("%s=%d", reason, n)
		end
		table.sort(parts)
		logger.info(string.format(
			"[MARKET_ACCEPT] qlat=%dms rows=%d%s rolled=%d queued=%d units=%d/%d norefGold=%d cb=%dms | %s",
			cbT0 - qT0, rows,
			(rows >= BotMarket.ACCEPT_ROW_LIMIT) and " ROWS_CAPPED" or "",
			rolled, #queue, qtyRolled, qtyOffered, norefSpent, marketNowMs() - cbT0,
			table.concat(parts, " ")
		))

		if #queue == 0 then
			acceptInFlight = false
			return
		end
		scheduleDrain(queue, 1, gen, { accepted = 0, failed = 0, noBot = 0, errors = 0, qty = 0 })
	end)
end

function acceptPass.onThink(interval)
	if not PASSES_ENABLED then return true end
	-- MASTER_DISABLE gates this pass, unlike the old buyer/fulfiller passes which checked
	-- only PASSES_ENABLED and kept trading with the bot system otherwise switched off.
	if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end
	local now = os.time()
	if now < acceptNextFireAt then return true end
	acceptNextFireAt = now + math.random(BotMarket.ACCEPT_INTERVAL_MIN, BotMarket.ACCEPT_INTERVAL_MAX)

	local ok, err = pcall(runAcceptPass)
	if not ok then
		-- Never leave the latch stuck: it would block every future sweep.
		acceptInFlight = false
		logger.warn("[BotMarketAccept] sweep error: " .. tostring(err))
	end
	return true
end

acceptPass:interval(60000) -- 60s poll; actual firing gated by acceptNextFireAt
acceptPass:register()

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

-- ─────────────────────────────────────────────────────────────────────────────
-- DEPTH REFRESH: reconcile the per-(item, side) ledger with the DB
-- ─────────────────────────────────────────────────────────────────────────────
-- The seller pass keeps the ledger current optimistically as it creates offers, but only the DB
-- knows about offers that real players accepted or that the purge/trim passes removed. This
-- reconciles the two, and is the only path by which an item that drained back to zero offers
-- returns to the coverage-first pool.
--
-- Phase offset t+75s: the market timers all share one DB worker, and bundle 5b showed what happens
-- when several land in the same second. Monitor fires at t+0/300s, purge at t+150s, trim at t+225s,
-- this at t+75s — four distinct residues mod 300.
local depthNextFireAt = os.time() + 75
local depthPass = GlobalEvent("BotMarketDepth")

function depthPass.onThink(interval)
	if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end
	if not BotMarket or not BotMarket.loaded then return true end
	local now = os.time()
	if now < depthNextFireAt then return true end
	depthNextFireAt = now + BotMarket.DEPTH_REFRESH_SECS

	local ok, err = pcall(BotMarket.refreshDepth)
	if not ok then
		-- Never leave the in-flight latch stuck: it would block every future refresh.
		BotMarket.refreshInFlight = false
		logger.warn("[BotMarketDepth] refresh error: " .. tostring(err))
	end
	return true
end

depthPass:interval(60000) -- 60s poll; actual firing gated by depthNextFireAt
depthPass:register()

-- ─────────────────────────────────────────────────────────────────────────────
-- TRIM: drain bot offers that exceed the per-(item, side) depth cap
-- ─────────────────────────────────────────────────────────────────────────────
-- The caps only gate NEW offers, so without this the ~8.8k rows already over cap would take the
-- full 7-day purge window to age out and the rebalance would be invisible for a week. This drains
-- them in ~2.5h and then idles at ~0 rows/fire, so it is safe to leave registered permanently.
--
-- ORDER BY created DESC is load-bearing: it makes rn=1 the NEWEST offer, so `rn > cap` deletes the
-- older excess and keeps the freshest book. With ASC it would keep the six stalest rows and delete
-- the newly-created ones — permanently churning against the very offers the picker is creating.
--
-- Only bot rows are ever deletable: the account_id join sits inside the innermost ranked subquery,
-- so a real player's offer id can never enter the deletion set. That matters — a real BUY offer
-- holds escrowed gold, and raw-deleting one would destroy player property. (Skipping the escrow
-- refund for BOT offers is fine and is what the existing purge above already does: bot_market_funding
-- restores every bot to 100kkk each restart.)
--
-- The double-derived-table wrapper is required — MySQL rejects LIMIT directly inside an IN subquery.
local TRIM_BATCH = 300
local trimNextFireAt = os.time() + 225
local trimPass = GlobalEvent("BotMarketTrim")

function trimPass.onThink(interval)
	if BOT_CONFIG and BOT_CONFIG.MASTER_DISABLE then return true end
	if not BotMarket or not BotMarket.BOT_ACCOUNT_ID then return true end
	local now = os.time()
	if now < trimNextFireAt then return true end
	trimNextFireAt = now + 300

	-- Skip the DELETE entirely when the ledger says nothing is over cap. EXPLAIN shows this
	-- statement is a full 24.7k-row scan plus a temporary+filesort for the window function; once
	-- the backlog has drained there is no reason to pay that every 5 minutes forever.
	--
	-- Safe direction: the ledger counts bot AND real offers, so ledger depth >= bot depth. If the
	-- ledger sees nothing above the cap then no bot row can be above it either. The converse (real
	-- players holding depth on an item no bot is over-listing) just costs one wasted query.
	local maxDepth = 0
	for _, side in ipairs({ MARKETACTION_BUY, MARKETACTION_SELL }) do
		for _, n in pairs(BotMarket.depthN[side] or {}) do
			if n > maxDepth then maxDepth = n end
		end
	end
	if BotMarket.depthLoadedAt > 0 and maxDepth <= BotMarket.MAX_OFFERS_PER_ITEM_SIDE then
		return true
	end

	db.botAsyncQuery(string.format(
		"DELETE FROM `market_offers` WHERE `id` IN (" ..
		"  SELECT `id` FROM (" ..
		"    SELECT `r`.`id` FROM (" ..
		"      SELECT `mo`.`id`, ROW_NUMBER() OVER (" ..
		"        PARTITION BY `mo`.`itemtype`, `mo`.`sale` ORDER BY `mo`.`created` DESC) AS `rn`" ..
		"      FROM `market_offers` `mo`" ..
		"      JOIN `players` `p` ON `p`.`id` = `mo`.`player_id`" ..
		"      WHERE `p`.`account_id` = %d" ..
		"    ) `r` WHERE `r`.`rn` > %d ORDER BY `r`.`rn` DESC LIMIT %d" ..
		"  ) `d`" ..
		")",
		BotMarket.BOT_ACCOUNT_ID, BotMarket.MAX_OFFERS_PER_ITEM_SIDE, TRIM_BATCH
	))
	logger.info(string.format("[MARKET_TRIM] batch<=%d cap=%d", TRIM_BATCH, BotMarket.MAX_OFFERS_PER_ITEM_SIDE))
	return true
end

trimPass:interval(60000) -- 60s poll; actual firing gated by trimNextFireAt
trimPass:register()

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
		--
		-- distinct/maxDepth come from the in-memory ledger rather than a second query: they are the
		-- acceptance signal for the depth-cap work (spread should climb, maxDepth should settle at
		-- MAX_OFFERS_PER_ITEM_SIDE), and reading them costs nothing.
		local distinctS, distinctB, maxDepth = 0, 0, 0
		for _, n in pairs(BotMarket.depthN[MARKETACTION_SELL] or {}) do
			distinctS = distinctS + 1
			if n > maxDepth then maxDepth = n end
		end
		for _, n in pairs(BotMarket.depthN[MARKETACTION_BUY] or {}) do
			distinctB = distinctB + 1
			if n > maxDepth then maxDepth = n end
		end
		logger.info(string.format(
			"[BotMarketMonitor] active offers: bot=%d (S=%d B=%d), real=%d (S=%d B=%d), ramp=%.0f%%, qlat=%dms, distinctS=%d distinctB=%d maxDepth=%d uncoveredS=%d uncoveredB=%d",
			bs + bb, bs, bb, rs + rb, rs, rb, rampScale() * 100, marketNowMs() - qT0,
			distinctS, distinctB, maxDepth,
			#(BotMarket.uncovered[MARKETACTION_SELL] or {}), #(BotMarket.uncovered[MARKETACTION_BUY] or {})
		))
		Result.free(resultId)
	end)
	return true
end

monitor:interval(300000) -- 5 min
monitor:register()
