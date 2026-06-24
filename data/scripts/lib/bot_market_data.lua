-- Bot Market Data Loader
-- Lazy-loads bot_market_item_prices into Lua tables for the bot market scheduler.
-- Loaded alphabetically after bot_hunting_data ("bot_h" < "bot_m") and before bot_system.

-- Mirror MarketAction_t enum from src/creatures/creatures_definitions.hpp:341
-- (not exposed via lua_enums, so we redefine locally)
if not MARKETACTION_BUY then MARKETACTION_BUY = 0 end
if not MARKETACTION_SELL then MARKETACTION_SELL = 1 end

BotMarket = {
	-- prices[itemId] = {market_max, market_low, market_high, npc_buy, npc_sell, marketable, weight, category, upgrade_class}
	prices = {},

	-- Curated bins for the seller pass: itemsByBin[binName] = {itemId, itemId, ...}
	-- Bins: equipment, reagents, potions, runes, creature_products, other
	itemsByBin = {
		equipment = {},
		reagents = {},
		potions = {},
		runes = {},
		creature_products = {},
		other = {},
	},

	-- Bot GUIDs cache (loaded once at startup)
	botGuids = {},

	-- Session state
	loaded = false,
	loadedAt = 0,
}

-- ─────────────────────────────────────────────────────────────────────────────
-- Constants (from approved plan)
-- ─────────────────────────────────────────────────────────────────────────────

BotMarket.BIN_WEIGHTS = {
	equipment = 19,
	reagents = 19,
	potions = 19,
	runes = 19,
	creature_products = 19,
	other = 5,
}
BotMarket.BIN_WEIGHT_TOTAL = 19 * 5 + 5 -- 100

-- Tier distribution (per upgrade_class > 0 item). Sum = 100.
BotMarket.TIER_DISTRIBUTION = { [0] = 80, [1] = 15, [2] = 4, [3] = 1 }

-- Direction split: 60% SELL, 40% BUY (sinks)
BotMarket.SELL_DIRECTION_PCT = 60

-- Anonymous probability (both directions)
BotMarket.ANONYMOUS_PCT = 25

-- Stack size range for stackable items
BotMarket.STACK_MIN = 250
BotMarket.STACK_MAX = 2000

-- Per-bot active offer cap — synced from config.lua (`maxMarketOffersAtATimePerPlayer`)
-- in BotMarket.loadAll(). The C++ wrapper `Game::botCreateMarketOffer` enforces the
-- same cap (canonical source of truth). This Lua-side check is just an optimization
-- to avoid wasted Game.botCreateMarketOffer calls when a bot is already saturated.
-- The constant below is a fallback if the config read fails.
BotMarket.MAX_OFFERS_PER_BOT = 200

-- Seller pass: pick this percentage of all bots per fire (200 bots × 5% = 10 bots, then jittered ±30%)
BotMarket.SELLER_BOTS_PCT = 5

-- Bot account id (verified in CLAUDE.md / MEMORY.md)
BotMarket.BOT_ACCOUNT_ID = 65000

-- Buyer pass: accept real SELL offer when listed price < market_max * BUY_DEAL_THRESHOLD
BotMarket.BUY_DEAL_THRESHOLD = 0.90
BotMarket.BUY_DEAL_PROBABILITY = 0.50

-- Fulfiller pass: fulfill real BUY offer when listed price >= market_max * FULFILL_THRESHOLD
BotMarket.FULFILL_THRESHOLD = 0.95
BotMarket.FULFILL_PROBABILITY = 0.50

-- Cadences (seconds, randomized per pass)
BotMarket.SELLER_INTERVAL_MIN = 30
BotMarket.SELLER_INTERVAL_MAX = 120
BotMarket.BUYER_INTERVAL_MIN = 600
BotMarket.BUYER_INTERVAL_MAX = 1800

-- Map appearances.dat ITEM_CATEGORY → seller bin
BotMarket.CATEGORY_TO_BIN = {
	-- Equipment
	armors = "equipment",
	helmets = "equipment",
	legs = "equipment",
	boots = "equipment",
	shields = "equipment",
	rings = "equipment",
	amulets = "equipment",
	axes = "equipment",
	clubs = "equipment",
	swords = "equipment",
	distance_weapons = "equipment",
	wands_rods = "equipment",
	fist_weapons = "equipment",
	quiver = "equipment",
	-- Reagents (imbuement materials & creature drops used for crafting)
	creature_products = "creature_products",
	-- Consumables
	potions = "potions",
	runes = "runes",
	food = "other",
	ammunition = "other",
	-- Misc
	containers = "other",
	decoration = "other",
	tools = "other",
	valuables = "other",
	soulcores = "reagents",
	premium_scrolls = "other",
	tibia_coins = "other",
	others = "other",
}

-- ─────────────────────────────────────────────────────────────────────────────
-- Loaders
-- ─────────────────────────────────────────────────────────────────────────────

function BotMarket.loadAll()
	local startTime = os.clock()
	BotMarket.prices = {}
	for binName, _ in pairs(BotMarket.itemsByBin) do
		BotMarket.itemsByBin[binName] = {}
	end
	BotMarket.botGuids = {}

	-- Sync per-bot cap from config.lua (single source of truth shared with C++ wrapper)
	local cfgCap = configManager.getNumber(configKeys.MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER)
	if cfgCap and cfgCap > 0 then
		BotMarket.MAX_OFFERS_PER_BOT = cfgCap
	end

	BotMarket._loadPrices()
	BotMarket._loadBots()

	BotMarket.loaded = true
	BotMarket.loadedAt = os.time()

	local ms = (os.clock() - startTime) * 1000
	local total = 0
	for _, list in pairs(BotMarket.itemsByBin) do total = total + #list end
	logger.info(string.format(
		"[BotMarket] Loaded %d items in %.0fms (equipment=%d reagents=%d potions=%d runes=%d creature_products=%d other=%d), %d bots, cap=%d/bot",
		total, ms,
		#BotMarket.itemsByBin.equipment, #BotMarket.itemsByBin.reagents,
		#BotMarket.itemsByBin.potions, #BotMarket.itemsByBin.runes,
		#BotMarket.itemsByBin.creature_products, #BotMarket.itemsByBin.other,
		#BotMarket.botGuids, BotMarket.MAX_OFFERS_PER_BOT
	))
end

function BotMarket._loadPrices()
	local resultId = db.storeQuery(
		"SELECT `item_id`, `name`, `npc_buy`, `npc_sell`, `market_max`, `market_low`, `market_high`, " ..
		"`marketable`, `weight`, `category`, `upgrade_class` FROM `bot_market_item_prices` " ..
		"WHERE `marketable` = 1 AND `market_max` IS NOT NULL AND `market_max` > 0"
	)
	if not resultId then return end
	repeat
		local itemId = Result.getNumber(resultId, "item_id")
		local entry = {
			name = Result.getString(resultId, "name") or "",
			npc_buy = Result.getNumber(resultId, "npc_buy"),
			npc_sell = Result.getNumber(resultId, "npc_sell"),
			market_max = Result.getNumber(resultId, "market_max"),
			market_low = Result.getNumber(resultId, "market_low"),
			market_high = Result.getNumber(resultId, "market_high"),
			weight = Result.getNumber(resultId, "weight"),
			category = Result.getString(resultId, "category"),
			upgrade_class = Result.getNumber(resultId, "upgrade_class") or 0,
		}
		BotMarket.prices[itemId] = entry

		-- Bin assignment
		local bin = BotMarket.CATEGORY_TO_BIN[entry.category or ""] or "other"
		table.insert(BotMarket.itemsByBin[bin], itemId)
	until not Result.next(resultId)
	Result.free(resultId)
end

-- Load the active-bot set maintained by the C++ BotEngine (bot_active_players
-- table, migration 60.lua). This is the same source the @cast list uses, so
-- market participants match exactly what users see in the cast viewer.
--
-- Filtering here (not by account_id alone) keeps market activity scaled to
-- botPlayersOnline: with 997 in the DB pool and config=200, only 200 bots
-- participate as market makers. Without the join, all 997 would create offers
-- via Game::botCreateMarketOffer's offline-load fallback path — 5× the intended
-- market liquidity, with sync DB Player loads on every offer from an unloaded
-- bot.
--
-- No refresh needed at runtime: botPlayersOnline is read at script-load time
-- and only changes via `systemctl restart canary`, which re-runs BotStartup
-- (re-populating bot_active_players) and BotMarket.loadAll() (re-querying here)
-- in lockstep. /cavebot reload churns the same set of GUIDs, so the cache
-- stays semantically valid.
function BotMarket._loadBots()
	-- Idempotent — callable from loadAll() at startup AND from ensureLoaded()'s
	-- retry path. Reset here so a partial-then-full sequence doesn't duplicate.
	BotMarket.botGuids = {}
	local resultId = db.storeQuery(string.format(
		"SELECT p.`id` FROM `players` p " ..
		"JOIN `bot_active_players` bap ON bap.`player_id` = p.`id` " ..
		"WHERE p.`account_id` = %d",
		BotMarket.BOT_ACCOUNT_ID
	))
	if not resultId then return end
	repeat
		table.insert(BotMarket.botGuids, Result.getNumber(resultId, "id"))
	until not Result.next(resultId)
	Result.free(resultId)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Helpers
-- ─────────────────────────────────────────────────────────────────────────────

-- Vendor arbitrage floor: bot SELL price ≥ max(npc_sell × 1.05, npc_buy × 0.6).
-- Returns nil for items with no NPC reference (no arbitrage risk).
function BotMarket.computeFloor(itemId)
	local p = BotMarket.prices[itemId]
	if not p then return nil end
	local floor = nil
	if p.npc_sell and p.npc_sell > 0 then
		floor = math.max(floor or 0, math.floor(p.npc_sell * 1.05))
	end
	if p.npc_buy and p.npc_buy > 0 then
		floor = math.max(floor or 0, math.floor(p.npc_buy * 0.6))
	end
	return floor
end

-- Compute a listing price for action ∈ {"SELL", "BUY"}.
function BotMarket.computeListingPrice(itemId, action)
	local p = BotMarket.prices[itemId]
	if not p or not p.market_max or p.market_max <= 0 then return nil end

	if action == "SELL" then
		local floor = BotMarket.computeFloor(itemId) or 1
		-- Ceiling: market_max × U(0.95, 1.10)
		local ceilingMul = 0.95 + math.random() * 0.15
		local ceiling = math.floor(p.market_max * ceilingMul)
		if ceiling < floor then ceiling = floor end
		-- Listing: U(floor, ceiling)
		return math.random(floor, ceiling)
	else -- BUY
		-- Bots want bargains: U(market_max × 0.70, market_max × 0.95)
		local lo = math.floor(p.market_max * 0.70)
		local hi = math.floor(p.market_max * 0.95)
		if lo < 1 then lo = 1 end
		if hi < lo then hi = lo end
		return math.random(lo, hi)
	end
end

-- Roll a tier for an upgradeable item. Non-upgradeable returns 0.
function BotMarket.rollTier(upgradeClass)
	if not upgradeClass or upgradeClass == 0 then return 0 end
	local r = math.random(1, 100)
	local cumulative = 0
	for tier, pct in pairs(BotMarket.TIER_DISTRIBUTION) do
		cumulative = cumulative + pct
		if r <= cumulative then return tier end
	end
	return 0
end

-- Roll a stack size. Stackable items get U(STACK_MIN, STACK_MAX). Non-stackable: 1.
function BotMarket.rollStackSize(itemId)
	local itemType = ItemType(itemId)
	if itemType and itemType:isStackable() then
		return math.random(BotMarket.STACK_MIN, BotMarket.STACK_MAX)
	end
	return 1
end

-- 25% probability for anonymous offers
function BotMarket.rollAnonymous()
	return math.random(1, 100) <= BotMarket.ANONYMOUS_PCT
end

-- Direction roll: 60% SELL, 40% BUY
function BotMarket.rollDirection()
	if math.random(1, 100) <= BotMarket.SELL_DIRECTION_PCT then
		return MARKETACTION_SELL
	else
		return MARKETACTION_BUY
	end
end

-- Pick a random active bot guid (no level filtering — just any bot)
function BotMarket.pickActiveBot()
	if #BotMarket.botGuids == 0 then return nil end
	return BotMarket.botGuids[math.random(1, #BotMarket.botGuids)]
end

-- Pick up to n DISTINCT bot guids via Fisher-Yates partial shuffle on a copy.
-- Returns at most #botGuids if n exceeds the pool. Order is randomized.
function BotMarket.pickActiveBots(n)
	local total = #BotMarket.botGuids
	if total == 0 or n <= 0 then return {} end
	if n > total then n = total end
	-- Copy then partial-shuffle the first n positions
	local pool = {}
	for i = 1, total do pool[i] = BotMarket.botGuids[i] end
	local picks = {}
	for i = 1, n do
		local j = math.random(i, total)
		pool[i], pool[j] = pool[j], pool[i]
		picks[i] = pool[i]
	end
	return picks
end

-- Pick a random item, weighted by bin distribution.
function BotMarket.pickItem()
	-- Choose a bin first (weighted), then a random item from that bin
	local r = math.random(1, BotMarket.BIN_WEIGHT_TOTAL)
	local cumulative = 0
	for binName, weight in pairs(BotMarket.BIN_WEIGHTS) do
		cumulative = cumulative + weight
		if r <= cumulative then
			local bin = BotMarket.itemsByBin[binName]
			if bin and #bin > 0 then
				return bin[math.random(1, #bin)]
			end
			-- Fall through if bin is empty
			break
		end
	end
	-- Fallback: random item from any non-empty bin
	for _, bin in pairs(BotMarket.itemsByBin) do
		if #bin > 0 then return bin[math.random(1, #bin)] end
	end
	return nil
end

-- Count active offers for a given player_id
function BotMarket.getOfferCount(playerId)
	local resultId = db.storeQuery(
		"SELECT COUNT(*) AS `cnt` FROM `market_offers` WHERE `player_id` = " .. playerId
	)
	if not resultId then return 0 end
	local cnt = Result.getNumber(resultId, "cnt") or 0
	Result.free(resultId)
	return cnt
end

-- Auto-load on first require — skipped in tests
-- (caller in bot_market.lua triggers BotMarket.loadAll() at server-startup)
