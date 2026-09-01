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

	-- refPrice[itemId] = reference gp value used ONLY to judge a REAL player's offer.
	-- Deliberately a second, separate table from `prices` above, and NOT merged into it:
	-- `prices` drives what bots may LIST (bins, uncovered[], isSaturated), and every entry
	-- there must be listable. computeListingPrice returns nil when market_max <= 0, and the
	-- seller pass silently no-ops on nil without ever calling noteOfferFailed — so a
	-- market_max=0 item merged into `prices` would sit in uncovered[] forever and bleed the
	-- coverage-first budget onto a hole that can never be filled. That is exactly the
	-- self-stall the NULL-category comment above _loadPrices documents.
	--
	-- Absent key means NO REFERENCE (the no-ref guards apply) — never 0. Encoding it as 0
	-- would make the `ref == nil` branch unreachable and turn every no-ref item into a
	-- gate FAILURE instead of a gate BYPASS.
	refPrice = {},

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

-- Bin pick weights are DERIVED FROM BIN SIZE at load (see _computeBinWeights), not hardcoded.
--
-- History: these used to be a fixed 19/19/19/19/19/5 table that had drifted badly out of step with
-- the actual bin contents. `runes` (34 items) and `potions` (41 items) each took 19% of all picks
-- while `other` — food + decoration + valuables + containers + tools + ammunition, ~1680 items —
-- took 5%. That made each rune ~190x likelier to be picked than each food item, which is why runes
-- and potions sat at 88-100 offers apiece while `meat`, `salmon`, `shrimp` and `dragon ham` had
-- none at all despite having prices.
--
-- weight = ceil(sqrt(#bin) * 10). sqrt rather than linear on purpose: linear would hand
-- creature_products + soulcores (1639 of 4647 items) a third of every pick and starve runes/potions
-- to ~0.7%, which just flips the same bug around. sqrt keeps small bins visible while ending the
-- 5%-for-1680-items starvation. Deriving them also means they can never drift from bin contents again.
BotMarket.BIN_WEIGHTS = {}
BotMarket.BIN_WEIGHT_TOTAL = 0

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

-- ─────────────────────────────────────────────────────────────────────────────
-- Per-(item, side) depth caps
-- ─────────────────────────────────────────────────────────────────────────────
-- The per-BOT cap (MAX_OFFERS_PER_BOT, 200) never binds — with ~25k offers across 500 bots each
-- bot holds ~50. Nothing ever capped a single ITEM, so the weighted picker was free to pile 100
-- offers / 100k qty onto one rune while thousands of priced items had zero. These cap depth per
-- (itemtype, sale) instead, which is what actually spreads orders across the catalogue.
--
-- Depth counts ALL offers on that item+side, bot and real player alike: it is "how deep is this
-- book", so bots correctly back off an item real players are already making a market in. It also
-- keeps the refresh a pure (sale, itemtype) index scan with no join to `players`.
BotMarket.MAX_OFFERS_PER_ITEM_SIDE = 6
BotMarket.MAX_QTY_PER_ITEM_SIDE = 10000

-- Don't bother listing a token stack just because a sliver of qty headroom remains; below this the
-- (item, side) counts as saturated instead. Stackables only — non-stackables list amount=1.
BotMarket.MIN_LISTABLE_STACK = 50

-- Share of seller picks aimed at an (item, side) with ZERO offers. This — not the cap — is what
-- fills the long tail; a cap alone only rejects, it never steers. The other 40% keeps running the
-- bin-weighted pick, which is what preserves depth on the staples.
BotMarket.COVERAGE_FIRST_PCT = 60

-- Bounded rerolls when the weighted pick lands on a saturated (item, side).
BotMarket.PICK_MAX_REROLLS = 8

-- Depth ledger refresh cadence (seconds).
BotMarket.DEPTH_REFRESH_SECS = 600

-- Consecutive Game.botCreateMarketOffer failures before an item is dropped from the pool.
-- Three, not one: botCreateMarketOffer also returns false on transient insufficient funds
-- (game_bot_market.cpp:152,161), and a funds blip must not evict a perfectly good item.
BotMarket.FAIL_STRIKES_MAX = 3

-- ── Depth ledger ────────────────────────────────────────────────────────────
-- Flat parallel integer tables rather than {n=,q=} records: a record per group would allocate
-- ~6200 GC-tracked tables every refresh that become garbage on the next one. Absent key = zero depth.
BotMarket.depthN = { [0] = {}, [1] = {} } -- depthN[sale][itemId] = offer count
BotMarket.depthQ = { [0] = {}, [1] = {} } -- depthQ[sale][itemId] = total quantity

-- Dense arrays of itemIds with no offers on that side, plus the reverse index that makes removal
-- O(1). Without uncoveredIdx, finding an itemId's position to swap-remove is a linear scan over up
-- to ~1700 entries on every single successful offer.
BotMarket.uncovered = { [0] = {}, [1] = {} }
BotMarket.uncoveredIdx = { [0] = {}, [1] = {} }

-- Optimistic updates made while a refresh query is in flight, replayed onto the fresh ledger when
-- it lands (see refreshDepth). Observed qlat on this worker has hit 4515ms, which is many seller
-- fires' worth of offers the query never saw.
BotMarket.pendingDelta = {}
BotMarket.refreshInFlight = false
BotMarket.refreshStartedAt = 0
BotMarket.depthLoadedAt = 0

-- Items that Game.botCreateMarketOffer keeps rejecting -> dropped from the pool entirely.
BotMarket.failStrikes = {}
BotMarket.unlistable = {}

-- ─────────────────────────────────────────────────────────────────────────────
-- Accept tunables — ALL of these come from config.lua
-- ─────────────────────────────────────────────────────────────────────────────
-- Declared in src/config/bot_config_keys.hpp (BOT_MARKET_CONFIG_KEYS); read once in
-- loadAll() below. The values here are only the fallbacks used if a config read fails,
-- and they mirror the defaults in that header — keep the two in step.
--
-- These REPLACE the old BUY_DEAL_THRESHOLD / BUY_DEAL_PROBABILITY / FULFILL_THRESHOLD /
-- FULFILL_PROBABILITY constants and the BUYER_INTERVAL_* cadence. The old buyer pass
-- rejected anything not priced below 90% of market_max, which meant a player had to
-- undercut the NPC vendor price on any item whose market_max was NPC-derived — for 557
-- items that is unsatisfiable by a rational seller.
BotMarket.ACCEPT_INTERVAL_MIN = 900
BotMarket.ACCEPT_INTERVAL_MAX = 2700
BotMarket.ACCEPT_CHANCE_PCT = 20
BotMarket.SELL_CEILING_PCT = 110
BotMarket.BUY_FLOOR_PCT = 95
BotMarket.ACCEPT_DRAIN_MS = 400
BotMarket.NOREF_MAX_UNIT_PRICE = 10
BotMarket.NOREF_MIN_UNIT_PRICE = 20
BotMarket.NOREF_FIRE_GOLD_BUDGET = 1000000

-- Hard ceiling on rows one sweep will consider. Real-offer volume is ~5 today; this only
-- exists so a pathological book can't hand the roll loop an unbounded result set. When it
-- binds, the sweep logs rowsCapped so the truncation is never silent.
BotMarket.ACCEPT_ROW_LIMIT = 5000

-- Seller-pass cadence (seconds, randomized per pass). Unrelated to the accept sweep.
BotMarket.SELLER_INTERVAL_MIN = 30
BotMarket.SELLER_INTERVAL_MAX = 120

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

	BotMarket._loadAcceptConfig()

	-- Depth ledger is rebuilt from scratch on every load (including /cavebot reload churn), so a
	-- stale ledger can never outlive the price table it indexes.
	BotMarket.depthN = { [0] = {}, [1] = {} }
	BotMarket.depthQ = { [0] = {}, [1] = {} }
	BotMarket.uncovered = { [0] = {}, [1] = {} }
	BotMarket.uncoveredIdx = { [0] = {}, [1] = {} }
	BotMarket.pendingDelta = {}
	BotMarket.refreshInFlight = false
	BotMarket.refreshStartedAt = 0
	BotMarket.depthLoadedAt = 0
	BotMarket.failStrikes = {}
	BotMarket.unlistable = {}

	BotMarket._loadPrices()
	BotMarket._loadRefPrices()
	BotMarket._computeBinWeights()
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

	-- Surface the un-listable exclusion explicitly. Without this line it is only inferable from a
	-- change in the loaded-item count, which is exactly the kind of silent data loss that hid the
	-- NULL-category problem for as long as it did.
	local excluded = BotMarket.getNumber(
		"SELECT COUNT(*) AS `cnt` FROM `bot_market_item_prices` " ..
		"WHERE `marketable` = 1 AND `market_max` IS NOT NULL AND `market_max` > 0 " ..
		"AND (`category` IS NULL OR `category` = '')"
	)
	logger.info(string.format(
		"[BotMarket] excluded %d un-listable (NULL/empty category, wareId=0 -> offers always rejected)",
		excluded
	))

	-- Weights are derived now, so log them: this is the table that was silently wrong before.
	local parts = {}
	for binName, w in pairs(BotMarket.BIN_WEIGHTS) do
		parts[#parts + 1] = string.format("%s=%.1f%%", binName, 100 * w / math.max(1, BotMarket.BIN_WEIGHT_TOTAL))
	end
	table.sort(parts)
	logger.info("[BotMarket] bin pick weights (sqrt-derived): " .. table.concat(parts, " "))

	-- Seed the depth ledger immediately rather than waiting for the first BotMarketDepth fire, so
	-- coverage-first picking is armed for the earliest seller passes.
	BotMarket.refreshDepth()
end

-- Small helper for one-off scalar COUNT queries at load time (startup only, never on a pass).
function BotMarket.getNumber(query)
	local resultId = db.storeQuery(query)
	if not resultId then return 0 end
	local n = Result.getNumber(resultId, "cnt") or 0
	Result.free(resultId)
	return n
end

-- Rows with no `category` are EXCLUDED, and that exclusion is load-bearing.
--
-- `category` comes from appearances.dat, so a NULL category means the item has no market
-- classification, which means wareId == 0 server-side, which means game_bot_market.cpp:121 rejects
-- every offer for it. Measured on the live DB: 351 such items, and **0 of them have ever received a
-- single offer**, against 3722/4647 for the rest. The three "not marketable (wareId=0)" warnings in
-- 6h of journal were items 39181 / 52783 / 52866 — all three NULL-category.
--
-- Left in the pool they would be fatal to coverage-first picking specifically: those 351 x 2 = 702
-- (item, side) slots can never be filled, so they never leave `uncovered`, and as the genuinely
-- fillable set drains the share of the coverage budget landing on permanently-failing items climbs
-- toward 100%. The feature would quietly stall itself.
--
-- `<> ''` is defensive only (live count is 0) — cheap insurance against a future re-import of the
-- price table writing empty strings where it now writes NULL.
function BotMarket._loadPrices()
	local resultId = db.storeQuery(
		"SELECT `item_id`, `name`, `npc_buy`, `npc_sell`, `market_max`, `market_low`, `market_high`, " ..
		"`marketable`, `weight`, `category`, `upgrade_class` FROM `bot_market_item_prices` " ..
		"WHERE `marketable` = 1 AND `market_max` IS NOT NULL AND `market_max` > 0 " ..
		"AND `category` IS NOT NULL AND `category` <> ''"
	)
	if not resultId then return end
	repeat
		local itemId = Result.getNumber(resultId, "item_id")
		local itemType = ItemType(itemId)
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
			-- Cached so isSaturated() stays allocation-free on the pick hot path
			stackable = (itemType and itemType:isStackable()) or false,
		}
		BotMarket.prices[itemId] = entry

		-- Bin assignment
		local bin = BotMarket.CATEGORY_TO_BIN[entry.category or ""] or "other"
		table.insert(BotMarket.itemsByBin[bin], itemId)
	until not Result.next(resultId)
	Result.free(resultId)
end

-- Read the accept tunables from config.lua. One place, called from loadAll().
--
-- These are load-once: config.lua is only re-read by `systemctl restart canary`, so
-- retuning any of them needs a restart (NOT `/cavebot reload`, which only swaps the .so).
function BotMarket._loadAcceptConfig()
	local function num(key, fallback)
		local v = configManager.getNumber(key)
		-- 0 is meaningful for the chance key (it disables the pass), so only fall back on
		-- a nil/failed read, never on a legitimate zero.
		if v == nil then return fallback end
		return v
	end
	BotMarket.ACCEPT_INTERVAL_MIN   = num(configKeys.BOT_MARKET_ACCEPT_INTERVAL_MIN_SEC, 900)
	BotMarket.ACCEPT_INTERVAL_MAX   = num(configKeys.BOT_MARKET_ACCEPT_INTERVAL_MAX_SEC, 2700)
	BotMarket.ACCEPT_CHANCE_PCT     = num(configKeys.BOT_MARKET_ACCEPT_CHANCE_PCT, 20)
	BotMarket.SELL_CEILING_PCT      = num(configKeys.BOT_MARKET_SELL_CEILING_PCT, 110)
	BotMarket.BUY_FLOOR_PCT         = num(configKeys.BOT_MARKET_BUY_FLOOR_PCT, 95)
	BotMarket.ACCEPT_DRAIN_MS       = num(configKeys.BOT_MARKET_ACCEPT_DRAIN_MS, 400)
	BotMarket.NOREF_MAX_UNIT_PRICE  = num(configKeys.BOT_MARKET_NOREF_MAX_UNIT_PRICE, 10)
	BotMarket.NOREF_MIN_UNIT_PRICE  = num(configKeys.BOT_MARKET_NOREF_MIN_UNIT_PRICE, 20)
	BotMarket.NOREF_FIRE_GOLD_BUDGET = num(configKeys.BOT_MARKET_NOREF_FIRE_GOLD_BUDGET, 1000000)

	-- An inverted interval would make math.random(min, max) throw on every sweep.
	if BotMarket.ACCEPT_INTERVAL_MAX < BotMarket.ACCEPT_INTERVAL_MIN then
		BotMarket.ACCEPT_INTERVAL_MAX = BotMarket.ACCEPT_INTERVAL_MIN
	end
	-- Drain of 0 would schedule every acceptance into the same tick, which is the whole
	-- thing the drain exists to prevent.
	if BotMarket.ACCEPT_DRAIN_MS < 50 then BotMarket.ACCEPT_DRAIN_MS = 50 end

	logger.info(string.format(
		"[BotMarket] accept cfg: every %d-%ds, %d%%/offer, sellCeil=%d%% buyFloor=%d%% drain=%dms, " ..
		"noref maxUnit=%d minUnit=%d fireBudget=%d",
		BotMarket.ACCEPT_INTERVAL_MIN, BotMarket.ACCEPT_INTERVAL_MAX, BotMarket.ACCEPT_CHANCE_PCT,
		BotMarket.SELL_CEILING_PCT, BotMarket.BUY_FLOOR_PCT, BotMarket.ACCEPT_DRAIN_MS,
		BotMarket.NOREF_MAX_UNIT_PRICE, BotMarket.NOREF_MIN_UNIT_PRICE, BotMarket.NOREF_FIRE_GOLD_BUDGET
	))
end

-- Build refPrice from the WHOLE price table.
--
-- Three deliberate differences from _loadPrices' filters, each load-bearing:
--   * no `market_max > 0` — resolving a reference when market_max is 0 is the entire point;
--     the boxes the whole rework started from are market_max=0 rows.
--   * no `category` filter — that filter is a proxy for wareId, and it only matters when
--     BOTS pick an item to list. Here the item is one a REAL player already listed, which
--     proves wareId != 0 (Game::playerCreateMarketOffer rejects wareId==0 before any row
--     can be inserted), so filtering on category can only lose references.
--   * no `marketable = 1` — measured on the live table, 347 rows have marketable <> 1 yet
--     DO carry a usable price. `marketable` is the importer's opinion, not the server's
--     wareId; filtering on it would push those 347 items into the unguarded no-ref path.
--
-- Fallback order market_max -> npc_sell -> npc_buy. npc_sell (what an NPC charges) is a
-- closer stand-in for market value than npc_buy (what an NPC pays), so it is preferred;
-- npc_buy is the last resort. Rows with no usable price at all are simply absent, which is
-- how the no-ref guards get triggered.
function BotMarket._loadRefPrices()
	BotMarket.refPrice = {}
	local resultId = db.storeQuery(
		"SELECT `item_id`, `market_max`, `npc_sell`, `npc_buy` FROM `bot_market_item_prices` " ..
		"WHERE COALESCE(`market_max`,0) > 0 OR COALESCE(`npc_sell`,0) > 0 OR COALESCE(`npc_buy`,0) > 0"
	)
	if not resultId then
		logger.warn("[BotMarket] refPrice load returned no rows — every real offer will take the no-ref path")
		return
	end
	local n = 0
	repeat
		local itemId = Result.getNumber(resultId, "item_id")
		local mm = Result.getNumber(resultId, "market_max") or 0
		local ns = Result.getNumber(resultId, "npc_sell") or 0
		local nb = Result.getNumber(resultId, "npc_buy") or 0
		local ref = (mm > 0 and mm) or (ns > 0 and ns) or (nb > 0 and nb) or nil
		if ref then
			BotMarket.refPrice[itemId] = ref
			n = n + 1
		end
	until not Result.next(resultId)
	Result.free(resultId)
	logger.info(string.format("[BotMarket] refPrice: %d items with a reference value (absent = no-ref guards apply)", n))
end

-- Derive pick weights from bin size (see the BIN_WEIGHTS comment for why sqrt).
function BotMarket._computeBinWeights()
	BotMarket.BIN_WEIGHTS = {}
	BotMarket.BIN_WEIGHT_TOTAL = 0
	for binName, list in pairs(BotMarket.itemsByBin) do
		local w = (#list > 0) and math.ceil(math.sqrt(#list) * 10) or 0
		BotMarket.BIN_WEIGHTS[binName] = w
		BotMarket.BIN_WEIGHT_TOTAL = BotMarket.BIN_WEIGHT_TOTAL + w
	end
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
-- Depth ledger
-- ─────────────────────────────────────────────────────────────────────────────

-- Drop an itemId out of uncovered[sale] in O(1): swap the tail into its slot, fix the tail
-- element's reverse index, then pop. Skipping the reverse-index fixup here would silently corrupt
-- every subsequent removal.
function BotMarket._removeUncovered(sale, itemId)
	local idx = BotMarket.uncoveredIdx[sale][itemId]
	if not idx then return end
	local arr = BotMarket.uncovered[sale]
	local last = #arr
	if idx ~= last then
		local moved = arr[last]
		arr[idx] = moved
		BotMarket.uncoveredIdx[sale][moved] = idx
	end
	arr[last] = nil
	BotMarket.uncoveredIdx[sale][itemId] = nil
end

-- Rebuild the uncovered sets by walking the price table against the fresh depth counts.
-- ~9300 integer lookups; cheap, and it is the only way an item that dropped back to zero offers
-- (purged or trimmed) can return to the coverage-first pool.
function BotMarket._rebuildUncovered()
	for _, sale in ipairs({ 0, 1 }) do
		local arr, idx, depth = {}, {}, BotMarket.depthN[sale]
		for itemId in pairs(BotMarket.prices) do
			if not BotMarket.unlistable[itemId] and (depth[itemId] or 0) == 0 then
				arr[#arr + 1] = itemId
				idx[itemId] = #arr
			end
		end
		BotMarket.uncovered[sale] = arr
		BotMarket.uncoveredIdx[sale] = idx
	end
end

-- Record an offer we just created, before the DB knows about it.
function BotMarket.noteOfferCreated(itemId, sale, amount)
	BotMarket.depthN[sale][itemId] = (BotMarket.depthN[sale][itemId] or 0) + 1
	BotMarket.depthQ[sale][itemId] = (BotMarket.depthQ[sale][itemId] or 0) + (amount or 0)
	BotMarket._removeUncovered(sale, itemId)
	BotMarket.failStrikes[itemId] = nil -- a success clears any accumulated strikes
	if BotMarket.refreshInFlight then
		BotMarket.pendingDelta[#BotMarket.pendingDelta + 1] = { itemId = itemId, sale = sale, amount = amount or 0 }
	end
end

-- Record a rejected create. Three consecutive strikes and the item leaves the pool for good —
-- this is the safety net for any item that is un-listable for a reason the category filter misses.
function BotMarket.noteOfferFailed(itemId)
	local n = (BotMarket.failStrikes[itemId] or 0) + 1
	BotMarket.failStrikes[itemId] = n
	if n < BotMarket.FAIL_STRIKES_MAX then return false end

	BotMarket.unlistable[itemId] = true
	BotMarket.failStrikes[itemId] = nil
	for _, sale in ipairs({ 0, 1 }) do
		BotMarket._removeUncovered(sale, itemId)
	end
	for binName, list in pairs(BotMarket.itemsByBin) do
		for i = #list, 1, -1 do
			if list[i] == itemId then table.remove(list, i) end
		end
	end
	BotMarket._computeBinWeights() -- bin shrank; keep weights consistent with contents
	logger.warn(string.format("[MARKET_UNLISTABLE] item=%d strikes=%d -> blacklisted", itemId, BotMarket.FAIL_STRIKES_MAX))
	return true
end

-- Refresh the ledger from the DB. One aggregate over the existing (sale, itemtype) index.
--
-- Deliberately NOT paged. Paging this was tried and reverted: the row-processing callback is
-- dispatched back onto the main dispatcher either way (botdatabasetasks.cpp:82), so paging never
-- reduced the only cost that can cause a stall — it just split one ~20ms block into five ~4ms ones
-- while tripling MySQL-side work (OFFSET re-aggregates from the start every page, since `amount`
-- is not in the index) and opening a window where a mid-cycle swap could push an item's count back
-- down and re-list it. At ~6200 groups this callback costs ~10-25ms once per 600s, an order of
-- magnitude under anything that has ever shown up as a stall here.
function BotMarket.refreshDepth()
	-- Never let two cycles race one pendingDelta: the first callback would replay-and-clear it and
	-- the second would replay an empty log, silently dropping every delta recorded between the two
	-- issue times. Only reachable if the worker stalls past a full refresh interval, which is
	-- exactly the case worth surviving.
	if BotMarket.refreshInFlight then
		-- ...but never latch forever. If a callback is lost (worker died mid-query) the flag would
		-- otherwise block every future refresh for the life of the process, and the ledger would
		-- quietly freeze at whatever it last held.
		if os.time() - (BotMarket.refreshStartedAt or 0) < BotMarket.DEPTH_REFRESH_SECS * 3 then
			return
		end
		logger.warn("[MARKET_DEPTH] previous refresh never completed — clearing in-flight latch and retrying")
	end
	BotMarket.refreshInFlight = true
	BotMarket.refreshStartedAt = os.time()
	BotMarket.pendingDelta = {}
	local t0 = Game.monotonicMs and Game.monotonicMs() or (os.time() * 1000)

	db.botAsyncStoreQuery(
		"SELECT `sale`, `itemtype`, COUNT(*) AS `n`, SUM(`amount`) AS `q` " ..
		"FROM `market_offers` GROUP BY `sale`, `itemtype`",
		function(resultId)
			local cbT0 = Game.monotonicMs and Game.monotonicMs() or (os.time() * 1000)
			-- A falsy result is a query ERROR or an empty table — storeQuery returns nullptr for
			-- both (database.cpp:360-368). Treating it as "market is empty" would wipe the ledger
			-- and re-list everything, so keep what we have and try again next cycle.
			if not resultId then
				BotMarket.refreshInFlight = false
				BotMarket.pendingDelta = {}
				logger.warn("[MARKET_DEPTH] refresh returned no result (error or empty) — keeping previous ledger")
				return
			end

			-- REASSIGN, never patch in place: GROUP BY emits no row for an empty group, so there is
			-- no zero to overwrite a stale count with. Patching would leave any item whose last
			-- offer was purged or trimmed stuck at its old count — permanently "saturated" and
			-- permanently absent from uncovered[].
			BotMarket.depthN = { [0] = {}, [1] = {} }
			BotMarket.depthQ = { [0] = {}, [1] = {} }

			local groups = 0
			repeat
				local sale = Result.getNumber(resultId, "sale")
				local itemId = Result.getNumber(resultId, "itemtype")
				if BotMarket.depthN[sale] then
					BotMarket.depthN[sale][itemId] = Result.getNumber(resultId, "n")
					BotMarket.depthQ[sale][itemId] = Result.getNumber(resultId, "q")
					groups = groups + 1
				end
			until not Result.next(resultId)
			Result.free(resultId)

			-- Replay anything created while the query was in flight (observed qlat up to 4515ms).
			-- Double-counting a delta the query already saw overstates depth, which is the safe
			-- direction — it makes bots more conservative, never less.
			for _, d in ipairs(BotMarket.pendingDelta) do
				BotMarket.depthN[d.sale][d.itemId] = (BotMarket.depthN[d.sale][d.itemId] or 0) + 1
				BotMarket.depthQ[d.sale][d.itemId] = (BotMarket.depthQ[d.sale][d.itemId] or 0) + d.amount
			end
			local replayed = #BotMarket.pendingDelta
			BotMarket.pendingDelta = {}

			BotMarket._rebuildUncovered()
			BotMarket.depthLoadedAt = os.time()
			BotMarket.refreshInFlight = false

			local now = Game.monotonicMs and Game.monotonicMs() or (os.time() * 1000)
			logger.info(string.format(
				"[MARKET_DEPTH] groups=%d replayed=%d qlat=%dms cb=%dms uncoveredS=%d uncoveredB=%d",
				groups, replayed, cbT0 - t0, now - cbT0,
				#BotMarket.uncovered[MARKETACTION_SELL], #BotMarket.uncovered[MARKETACTION_BUY]
			))
		end
	)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Helpers
-- ─────────────────────────────────────────────────────────────────────────────

-- Smallest amount worth listing for this item on a fresh offer.
function BotMarket.minListable(itemId)
	local p = BotMarket.prices[itemId]
	return (p and p.stackable) and BotMarket.MIN_LISTABLE_STACK or 1
end

function BotMarket.qtyHeadroom(itemId, sale)
	return BotMarket.MAX_QTY_PER_ITEM_SIDE - (BotMarket.depthQ[sale][itemId] or 0)
end

-- The single saturation predicate. Both the offer-count cap and the quantity cap live here on
-- purpose: an earlier split had the picker approve an item with 1 qty of headroom and then let
-- rollStackSize discover it was unusable, which needed a second reroll loop downstream of the
-- picker's already-spent budget, with no termination rule.
function BotMarket.isSaturated(itemId, sale)
	if BotMarket.unlistable[itemId] then return true end
	if (BotMarket.depthN[sale][itemId] or 0) >= BotMarket.MAX_OFFERS_PER_ITEM_SIDE then return true end
	return BotMarket.qtyHeadroom(itemId, sale) < BotMarket.minListable(itemId)
end

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

-- Roll a stack size, clamped to whatever quantity headroom the (item, side) has left.
-- Only clamps — never rerolls. isSaturated() already guaranteed headroom >= minListable for any
-- item the picker handed back, so the clamp can't produce a junk stack.
function BotMarket.rollStackSize(itemId, sale)
	local p = BotMarket.prices[itemId]
	if not (p and p.stackable) then return 1 end
	local rolled = math.random(BotMarket.STACK_MIN, BotMarket.STACK_MAX)
	if sale then
		local headroom = BotMarket.qtyHeadroom(itemId, sale)
		if rolled > headroom then rolled = headroom end
	end
	return math.max(1, rolled)
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

-- Pick a random item, weighted by bin distribution. Side-agnostic; callers go through
-- pickItemForSide, which layers coverage-first and saturation on top.
function BotMarket.pickItem()
	-- Choose a bin first (weighted), then a random item from that bin
	local r = math.random(1, math.max(1, BotMarket.BIN_WEIGHT_TOTAL))
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

-- Choose which side a coverage-first pick should fill, in proportion to how many holes each side
-- actually has.
--
-- The economic 60/40 SELL/BUY split still governs the weighted path — that is what sets market
-- character. But filling empty books is a repair operation, and it should go where the holes are:
-- under a fixed 60/40 the BUY side (1783 holes vs SELL's 1296) was the binding constraint and only
-- got 1.24x oversampling. Proportional selection gives both sides ~1.79x and self-balances as the
-- sets drain.
--
-- Caller must have already established that at least one side is non-empty, so this never divides
-- by zero.
function BotMarket.pickCoverageSide()
	local nSell = #BotMarket.uncovered[MARKETACTION_SELL]
	local nBuy = #BotMarket.uncovered[MARKETACTION_BUY]
	if nSell == 0 then return MARKETACTION_BUY end
	if nBuy == 0 then return MARKETACTION_SELL end
	return (math.random(1, nSell + nBuy) <= nSell) and MARKETACTION_SELL or MARKETACTION_BUY
end

-- Bin-weighted pick for a known side, rerolling past saturated items.
-- Returns itemId (or nil), a telemetry tag, and the number of rerolls burned.
function BotMarket.pickWeightedForSide(sale)
	local rerolls = 0
	for _ = 1, BotMarket.PICK_MAX_REROLLS do
		local itemId = BotMarket.pickItem()
		if not itemId then break end
		if not BotMarket.isSaturated(itemId, sale) then
			return itemId, "weighted", rerolls
		end
		rerolls = rerolls + 1
	end

	-- Every roll landed on a saturated item. Rather than waste the slot, spend it on a hole for
	-- this side if there is one.
	local arr = BotMarket.uncovered[sale]
	if #arr > 0 then
		return arr[math.random(1, #arr)], "coverage_fallback", rerolls
	end
	return nil, "saturated", rerolls
end

-- Single entry point for the seller pass: decide direction AND item together.
--
-- Direction has to be settled before the item is chosen, because saturation is a property of the
-- (item, side) pair — the same item can be full on SELL and empty on BUY.
--
-- Returns itemId, sale, tag, rerolls. itemId is nil when the slot should be skipped.
function BotMarket.pickOffer()
	local nSell = #BotMarket.uncovered[MARKETACTION_SELL]
	local nBuy = #BotMarket.uncovered[MARKETACTION_BUY]
	local coverageReady = BotMarket.depthLoadedAt > 0 and (nSell + nBuy) > 0

	-- Coverage-first: aim this pick at a book with nothing in it at all.
	-- Gated on depthLoadedAt so the window before the first refresh degrades to the plain weighted
	-- pick instead of silently doing nothing.
	if coverageReady and math.random(1, 100) <= BotMarket.COVERAGE_FIRST_PCT then
		local sale = BotMarket.pickCoverageSide()
		local arr = BotMarket.uncovered[sale]
		return arr[math.random(1, #arr)], sale, "coverage", 0
	end

	-- Otherwise the economic 60/40 SELL/BUY split, which is what keeps the market's character.
	local sale = BotMarket.rollDirection()
	local itemId, tag, rerolls = BotMarket.pickWeightedForSide(sale)
	return itemId, sale, tag, rerolls
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Accept decision
-- ─────────────────────────────────────────────────────────────────────────────

-- Judge ONE real-player offer. Pure function of its arguments plus the loaded config and
-- refPrice tables — no DB, no side effects — so the sweep stays a thin loop around it.
--
--   sale: 1 = the player is SELLING (the bot would BUY)
--         0 = the player is BUYING  (the bot would SELL, conjuring the goods)
--
-- Returns ok(boolean), reason(string), refUsed(number|nil).
-- `reason` is the telemetry code; every rejection path has a distinct one, because the old
-- passes collapsed price rejection, a lost coin flip and "no counterparty bot" into one
-- indistinguishable accepted=0 line, which is why this class of bug stayed invisible.
function BotMarket.judgeOffer(sale, itemId, price, amount)
	local ref = BotMarket.refPrice[itemId]

	if not ref then
		-- No reference value. Guard both directions; see bot_config_keys.hpp for why the
		-- no-ref set being worthless trash makes it MORE dangerous, not less.
		if sale == MARKETACTION_SELL then
			-- Bot buys: cap gp PER UNIT. Per-unit, not per-total, so a legitimate large
			-- stack of genuinely cheap junk still trades.
			if price > BotMarket.NOREF_MAX_UNIT_PRICE then
				return false, "noref_unit_too_high", nil
			end
		else
			-- Bot sells and CONJURES the goods (no depot debit), so a floor is what stops
			-- a player baiting free items at 1gp each.
			if price < BotMarket.NOREF_MIN_UNIT_PRICE then
				return false, "noref_unit_too_low", nil
			end
		end
		return true, "noref_accept", nil
	end

	-- Both thresholds are rounded OUTWARD, i.e. always in the player's favour, and never
	-- left as a fraction. Market prices are whole gp, so a fractional threshold is a
	-- threshold no integer price can sit exactly on: a reference of 10 at a 95% floor gave
	-- 9.5, which quietly meant "9 is rejected, 10 is required" — a 320-unit order priced at
	-- 9 was refused 24 times over 23 hours for being half a gold short. Rounding the floor
	-- DOWN and the ceiling UP makes the advertised percentage mean what it looks like.
	if sale == MARKETACTION_SELL then
		-- Bot buys at up to SELL_CEILING_PCT of reference, rounded UP.
		local ceiling = math.ceil(ref * BotMarket.SELL_CEILING_PCT / 100)
		if price <= ceiling then
			return true, "gate_pass", ref
		end
		return false, "gate_fail_overpriced", ref
	end

	-- Bot sells into the player's bid, which must be at least BUY_FLOOR_PCT of reference,
	-- truncated DOWN.
	local floorPrice = math.floor(ref * BotMarket.BUY_FLOOR_PCT / 100)
	if price >= floorPrice then
		return true, "gate_pass", ref
	end
	return false, "gate_fail_underbid", ref
end

-- Whether an accepted offer draws on the per-sweep no-ref gold budget, and by how much.
-- Only the bot-BUYS direction moves gold OUT to a real player, so only that direction is
-- charged; a no-ref bot-SELLS acceptance is bounded by the unit floor instead.
function BotMarket.norefGoldCost(sale, itemId, price, amount)
	if BotMarket.refPrice[itemId] then return 0 end
	if sale ~= MARKETACTION_SELL then return 0 end
	return price * amount
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
