-- /housebackfill — bring map house occupancy to a configurable target
-- (default 80%) by randomly decorating empty houses with templates drawn
-- from the existing /houseingest dump.
--
-- Algorithm (intentionally simple):
--   1. Load house_dump_data.lua (already on the canary working dir from the
--      /houseingest pipeline). For every tile in the dump that has ≥2 items,
--      strip the first item (ground/carpet base — would duplicate OTBM
--      ground) and keep the rest as a decoration template. The pool ends up
--      with thousands of real-player-placed decoration stacks.
--   2. Read 3 config values from config.lua via a sandboxed loadfile so we
--      don't pollute _G:
--        houseOccupancyTargetPct           (default 80)
--        houseBackfillHirelingChancePct    (default 5)
--        houseBackfillTileDecorationPct    (default 45)
--   3. Build the candidate pool = houses where owner=0 AND houseId not in
--      the dump (so /houseingest's 582 houses stay untouched).
--   4. Shuffle candidates three times.
--   5. needed = floor(total_houses * target_pct/100) - currently_owned,
--      clamped to #candidates.
--   6. For the first `needed` candidates: assign a bot owner (same-town
--      preferred, cross-town fallback), then for each of the house's tiles
--      roll the per-tile decoration chance — if it hits, pick a random
--      template from the pool and place its items in reverse order
--      (preserves look-cycle order, matches /houseingest behavior),
--      skipping ids already on the tile.
--   7. Deterministically pre-select floor(needed * hireling_pct/100) houses
--      to receive a hireling. For each: pick a random fantasy seller name
--      not already used, a random hireling looktype, a random non-door
--      tile, then INSERT into player_hirelings + spawn.
--   8. Final step: unlock ALL hireling skills + outfits in the KV store of
--      every bot (account_id=BOT_ACCOUNT_ID), so any hireling owned by any
--      bot has the complete seller catalog available. This also retrofits
--      the 33 hirelings created by /houseingest.

local CONFIG_FILE      = "config.lua"
local DUMP_FILE        = "house_dump_data.lua"
local BOT_ACCOUNT_ID   = 65000

local DEFAULT_TARGET_PCT     = 80
local DEFAULT_HIRELING_PCT   = 5
local DEFAULT_DECORATION_PCT = 45

-- ~100 baked-in fantasy seller names. Used as the random hireling name pool
-- for backfilled houses. Skipped if a name is already in player_hirelings.
local SELLER_NAMES = {
    "Alfonso Goldhand", "Beatrice Silvercoin", "Cedric Coinpurse", "Dagmar Wares",
    "Edwin Pricegoode", "Freya Marketwise", "Gunnar Stockpile", "Helga Coinwise",
    "Ivar Brassbalance", "Jorund Fairtrade", "Kira Whitewares", "Lothar Stonemark",
    "Magnus Goldfist", "Nora Brightcoin", "Olaf Goodbargain", "Petra Sharptongue",
    "Quentin Honestworks", "Ragnar Goodweight", "Sigrid Fairmeasure", "Thorvald Truepay",
    "Ulf Sellstone", "Vera Trinketmaker", "Wulfric Boltsmith", "Xandra Coinstack",
    "Ymir Greentrade", "Zara Bluebargain", "Bjorn Redgold", "Astrid Stonepenny",
    "Erik Quickdeal", "Greta Wool-Trader", "Halvar Stoutpurse", "Ingrid Heavycoin",
    "Kjeld Lightpocket", "Leif Fastsale", "Mira Bigprofit", "Niall Smallchange",
    "Oda Tradelock", "Per Marketstall", "Runa Coppertrade", "Sven Ironbargain",
    "Tilda the Trader", "Ulrik the Merchant", "Vala Coppercoin", "Wolfgang Highprice",
    "Yrsa Lowprice", "Bertha Stocktrade", "Cnut Goldweave", "Drogo Silverhand",
    "Egil Tinplate", "Fenris Blackmark", "Aldric Penny-Wise", "Brunhilde Coinhouse",
    "Conrad Tradewell", "Dietrich Gildlock", "Elsa Marketrune", "Frederick Penny-Cup",
    "Gerda Pricestick", "Hagen Trinketcoin", "Ilse Goldweave", "Joachim Sellstock",
    "Klara Wares-And-All", "Ludwig Honesttrade", "Mathilda Marketkin", "Norbert Goldcry",
    "Otto Heavyplate", "Pernille Goodthrift", "Quirin Lownote", "Rolf Highbalance",
    "Sieglinde Truecost", "Tycho Coinfast", "Una Pennygood", "Volker Heavystock",
    "Wenzel Trade-Hand", "Yorick Pennymark", "Adelheid Truework", "Borghild Stoneprice",
    "Caspar Goodturn", "Detlef Coinkeen", "Eckhart Pricecut", "Fridolin Goldnose",
    "Gisela Quickcoin", "Harald Stockwise", "Inga Marketclaim", "Jonas Trinkettrade",
    "Kunigunde Lowwares", "Lars Goldsell", "Mette Pricemark", "Nikolai Brassgold",
    "Ortrud Goodcoin", "Paula Pricewell", "Reinhard Fastcoin", "Sibylle Marketgilt",
    "Torsten Heavygold", "Ulrika Honestcoin", "Volkmar Truelock", "Wiebke Goldgrasp",
    "Xerxes Pennychain", "Yngve Pricekin", "Zelda Heavynote", "Aksel Goodgilt",
    "Britta Coinshare", "Christof Goldturn", "Dorthe Marketprice"
}

-- HIRELING_OUTFITS_TABLE values from data/libs/systems/hireling.lua:
-- citizen default + 9 outfit dresses × 2 sexes. Female is the odd id.
local HIRELING_LOOKTYPES = {
    1107, 1108,  -- citizen
    1109, 1110,  -- banker
    1111, 1112,  -- trader
    1113, 1114,  -- cooker
    1115, 1116,  -- steward
    1117, 1118,  -- servant
    1123, 1124,  -- bonelord
    1125, 1126,  -- dragon
    1129, 1130,  -- hydra
    1131, 1132,  -- ferumbras
}

local HIRELING_SKILL_NAMES  = { "banker", "cooker", "steward", "trader" }
local HIRELING_OUTFIT_NAMES = { "banker", "cooker", "steward", "trader", "servant", "hydra", "ferumbras", "bonelord", "dragon" }

-- ---------- helpers ----------

local function loadConfig()
    local cfg = setmetatable({}, { __index = _G })
    local fn, err = loadfile(CONFIG_FILE)
    if not fn then
        print("[housebackfill] loadfile config.lua failed: " .. tostring(err))
        return {}
    end
    if setfenv then setfenv(fn, cfg) end
    local ok, runErr = pcall(fn)
    if not ok then
        print("[housebackfill] config.lua exec error: " .. tostring(runErr))
    end
    return cfg
end

local function buildBotPool()
    local botsByTown = {}
    local q = db.storeQuery(string.format(
        "SELECT `id`, `town_id` FROM `players` WHERE `account_id` = %d", BOT_ACCOUNT_ID
    ))
    if q then
        repeat
            local guid = result.getNumber(q, "id")
            local townId = result.getNumber(q, "town_id")
            botsByTown[townId] = botsByTown[townId] or {}
            table.insert(botsByTown[townId], guid)
        until not result.next(q)
        result.free(q)
    end

    local owned = {}
    local q2 = db.storeQuery("SELECT `owner` FROM `houses` WHERE `owner` > 0")
    if q2 then
        repeat
            owned[result.getNumber(q2, "owner")] = true
        until not result.next(q2)
        result.free(q2)
    end

    for tid, pool in pairs(botsByTown) do
        local filtered = {}
        for _, guid in ipairs(pool) do
            if not owned[guid] then filtered[#filtered + 1] = guid end
        end
        botsByTown[tid] = filtered
    end

    return botsByTown
end

local function pickBot(botsByTown, townId)
    local pool = botsByTown[townId]
    if pool and #pool > 0 then
        return table.remove(pool, 1), false  -- false = same-town
    end
    for _, p in pairs(botsByTown) do
        if #p > 0 then
            return table.remove(p, 1), true  -- true = cross-town
        end
    end
    return nil, false
end

-- Beds in Tibia are split items (headboard+footboard, pillow+blanket). When
-- the random tile selector drops a single bed half on a tile, it ends up
-- visually broken. Strip every bed-typed item from templates at pool-build
-- time; if a template ends up empty after the strip, skip it entirely.
local function isBedItemId(itemId)
    local iType = ItemType(itemId)
    return iType and iType:getType() == ITEM_TYPE_BED
end

-- Non-movable items from a dump tile's stack (walls, doors, fences, depots,
-- mailboxes, framework walls, etc.) must be filtered from templates. The
-- structural-TILE skip stops us from decorating ON walls, but a template
-- whose source tile contained a wall still carries that wall id — and the
-- random tile selector would happily plant it in the middle of an empty
-- floor in some unrelated house. Filter at item level so walls never enter
-- the template pool at all.
local function isStructuralItemId(itemId)
    local iType = ItemType(itemId)
    return iType and not iType:isMovable()
end

-- A "structural" tile is one whose existing contents include any
-- non-movable item besides the ground (walls, doors, windows, mailboxes,
-- depots, fences, ladders, etc.). Backfill should never decorate these —
-- the items end up stuck through walls or floating mid-air. /houseingest is
-- not affected because it only places items at exact dump positions, which
-- by construction came from real interior tiles.
local function isStructuralTile(tile)
    local groundId
    local g = tile:getGround()
    if g then groundId = g:getId() end

    for _, item in ipairs(tile:getItems() or {}) do
        local id = item:getId()
        if id ~= groundId then
            local iType = ItemType(id)
            if iType and not iType:isMovable() then return true end
        end
    end
    return false
end

-- ---- tile materialization ----
--
-- house:getTiles() returns the C++ House::houseTiles vector, populated
-- lazily by MapCache::getOrCreateTileFromCache (which only fires when a
-- tile is accessed). On a freshly-nuked tile_store DB,
-- IOMapSerialize::loadHouseItems has zero rows to drive that population,
-- so the vector ends up nearly empty for every house — only the few tiles
-- that incidental traffic touched make it in. The decoration loops then
-- iterate over 3-10 tiles per house instead of hundreds, silently produce
-- ~zero items, and bare_houses_filled lies in the summary.
--
-- /houseingest accidentally works around this because each Game.createItem
-- call internally hits Map::getTile() which materializes the tile (and
-- House::addTile fires for HouseTiles). /housebackfill iterates first and
-- never triggers materialization.
--
-- Fix: force-materialize via Tile(pos) BFS from the entry. Each Tile()
-- call goes through getOrCreateTileFromCache → House::addTile when the
-- tile is a HouseTile. After this BFS returns, house:getTiles() reflects
-- the full reachable tile set.
--
-- Notes:
-- - 4-connected on the same floor, plus z±1 for stair tiles in multi-
--   floor houses. Stair tiles inside a house are themselves HouseTiles,
--   so the flood crosses floors naturally.
-- - Bounded at 4096 tiles; largest guildhall is ~600.
-- - Hash uses string-format keys, NOT integer-packed coords. Tibia
--   y-coords are in [31099, 32877] — any low-multiplier integer packing
--   collides catastrophically and produces partial floods.
-- - Probing outdoor neighbor tiles via Tile() is safe: House::addTile is
--   only called for cachedTile->isHouse() == true tiles.
local FLOOD_MAX_TILES = 4096
local FLOOD_NEIGHBOR_OFFSETS = {
    { 1,  0, 0}, {-1,  0, 0}, { 0,  1, 0}, { 0, -1, 0},
    { 0,  0, 1}, { 0,  0,-1},
}

-- 27 positions: entry + 26 3D Moore neighbors. getExitPosition() is the
-- doormat tile OUTSIDE the front door, so the entry itself is virtually
-- never a HouseTile. For some houses (Marble Guildhall, Rathleton Hills
-- Estate) the nearest house tile is even on a different z-level than the
-- entry. We probe all 26 neighbors to find any HouseTile of this house
-- before starting the BFS.
local SEED_PROBE_OFFSETS = {}
for dz = -1, 1 do
    for dy = -1, 1 do
        for dx = -1, 1 do
            SEED_PROBE_OFFSETS[#SEED_PROBE_OFFSETS + 1] = { dx, dy, dz }
        end
    end
end

local function materializeHouseTiles(house)
    local entry = house:getExitPosition()
    if not entry then return 0 end
    local houseId = house:getId()

    -- Find a valid start: any HouseTile belonging to this house within the
    -- entry's 3D Moore neighborhood. Without this the BFS dies on the first
    -- iteration because the entry tile (doormat, outside) isn't a HouseTile.
    local startPos
    for _, d in ipairs(SEED_PROBE_OFFSETS) do
        local px, py, pz = entry.x + d[1], entry.y + d[2], entry.z + d[3]
        if pz >= 0 and pz <= 15 then
            local tile = Tile(Position(px, py, pz))
            if tile then
                local th = tile:getHouse()
                if th and th:getId() == houseId then
                    startPos = { px, py, pz }
                    break
                end
            end
        end
    end
    if not startPos then return 0 end

    local visited = {}
    local stack = { startPos }
    visited[string.format("%d:%d:%d", startPos[1], startPos[2], startPos[3])] = true
    local materialized = 0

    while #stack > 0 and materialized < FLOOD_MAX_TILES do
        local p = table.remove(stack)
        local pos = Position(p[1], p[2], p[3])
        local tile = Tile(pos)
        if tile then
            local th = tile:getHouse()
            if th and th:getId() == houseId then
                materialized = materialized + 1
                for _, d in ipairs(FLOOD_NEIGHBOR_OFFSETS) do
                    local nx, ny, nz = p[1] + d[1], p[2] + d[2], p[3] + d[3]
                    if nz >= 0 and nz <= 15 then
                        local k = string.format("%d:%d:%d", nx, ny, nz)
                        if not visited[k] then
                            visited[k] = true
                            stack[#stack + 1] = { nx, ny, nz }
                        end
                    end
                end
            end
        end
    end
    return materialized
end

-- Count tiles in a house whose stack contains at least one movable item
-- (besides the ground). Used to detect "bare" houses where /houseingest
-- placed many structural items (walls/windows/archways) but few movable
-- decorations, which after the next server save leaves the house visibly
-- empty since only movable items pass isSavedToHouses().
local function countMovableTilesInHouse(house)
    local count = 0
    for _, tile in ipairs(house:getTiles()) do
        local groundId
        local g = tile:getGround()
        if g then groundId = g:getId() end
        for _, item in ipairs(tile:getItems() or {}) do
            local id = item:getId()
            if id ~= groundId then
                local iType = ItemType(id)
                if iType and iType:isMovable() then
                    count = count + 1
                    break  -- one movable item per tile is enough
                end
            end
        end
    end
    return count
end

-- A house is "bare" if fewer than 20% of its declared-interior-tile count
-- have at least one movable item. House:getSize() is NOT exposed to Lua —
-- only getTileCount() which includes walls. The XML-declared size is
-- read from MySQL houses.size at bare-pass setup time and passed in.
-- 20% floor with a min of 1 catches both small flats (7 tiles → need ≥1)
-- and large houses (35 tiles → need ≥7).
local function isHouseBare(house, size)
    if not size or size <= 0 then return false end
    local threshold = math.max(1, math.floor(size * 0.20))
    return countMovableTilesInHouse(house) < threshold
end

local function loadTemplatesAndDumpedSet()
    local ok, data = pcall(dofile, DUMP_FILE)
    if not ok or type(data) ~= "table" or not data.houses then
        return nil, nil, "dofile " .. DUMP_FILE .. " failed: " .. tostring(data)
    end
    local pool, dumpedHouses = {}, {}
    local templatesDroppedEmpty = 0
    local itemsFilteredBeds = 0
    local itemsFilteredStructural = 0
    for hid, tiles in pairs(data.houses) do
        dumpedHouses[hid] = true
        for _, tile in ipairs(tiles) do
            if tile.i and #tile.i >= 2 then
                local stack = {}
                for k = 2, #tile.i do
                    local entry = tile.i[k]
                    local id = entry[1]
                    if isBedItemId(id) then
                        itemsFilteredBeds = itemsFilteredBeds + 1
                    elseif isStructuralItemId(id) then
                        itemsFilteredStructural = itemsFilteredStructural + 1
                    else
                        stack[#stack + 1] = entry
                    end
                end
                if #stack >= 1 then
                    pool[#pool + 1] = stack
                else
                    templatesDroppedEmpty = templatesDroppedEmpty + 1
                end
            end
        end
    end
    print(string.format("[housebackfill] template pool: %d templates, dropped %d empty after filter | filtered items: %d beds, %d structural",
        #pool, templatesDroppedEmpty, itemsFilteredBeds, itemsFilteredStructural))
    return pool, dumpedHouses, nil
end

local function shuffleN(arr, times)
    for _ = 1, times do
        for i = #arr, 2, -1 do
            local j = math.random(i)
            arr[i], arr[j] = arr[j], arr[i]
        end
    end
end

local function pickRandomInteriorTile(house)
    local tiles = house:getTiles()
    if not tiles or #tiles == 0 then return nil end
    for _ = 1, 30 do
        local tile = tiles[math.random(#tiles)]
        local pos = tile:getPosition()
        if not house:getDoorIdByPosition(pos) and not isStructuralTile(tile) then
            return pos
        end
    end
    -- Fallback: any non-door tile, even structural — keeps the hireling
    -- placed somewhere rather than failing the spawn entirely.
    for _, tile in ipairs(tiles) do
        local pos = tile:getPosition()
        if not house:getDoorIdByPosition(pos) then return pos end
    end
    return tiles[1]:getPosition()
end

local function placeHireling(house, botGuid, usedNames)
    local name
    for _ = 1, 200 do
        local cand = SELLER_NAMES[math.random(#SELLER_NAMES)]
        if not usedNames[cand] then name = cand; break end
    end
    if not name then return false end

    local pos = pickRandomInteriorTile(house)
    if not pos then return false end

    local looktype = HIRELING_LOOKTYPES[math.random(#HIRELING_LOOKTYPES)]
    local sex = (looktype % 2 == 1) and HIRELING_SEX.FEMALE or HIRELING_SEX.MALE

    -- Generic citizen colors; the dress looktype carries the visual variation.
    local lookbody, lookfeet, lookhead, looklegs = 87, 95, 78, 87

    local sql = string.format(
        "INSERT INTO `player_hirelings` (`player_id`,`name`,`active`,`sex`,`posx`,`posy`,`posz`,`lookbody`,`lookfeet`,`lookhead`,`looklegs`,`looktype`) VALUES (%d,%s,1,%d,%d,%d,%d,%d,%d,%d,%d,%d)",
        botGuid, db.escapeString(name), sex,
        pos.x, pos.y, pos.z,
        lookbody, lookfeet, lookhead, looklegs, looktype
    )

    if not db.query(sql) then return false end
    local newId = db.lastInsertId()
    if not newId or newId <= 0 then return false end

    local h = Hireling:new()
    h.id = newId
    h.player_id = botGuid
    h.name = name
    h.active = 1
    h.sex = sex
    h.posx, h.posy, h.posz = pos.x, pos.y, pos.z
    h.lookbody, h.lookfeet = lookbody, lookfeet
    h.lookhead, h.looklegs = lookhead, looklegs
    h.looktype = looktype
    table.insert(HIRELINGS, h)
    h:spawn()
    usedNames[name] = true
    return true
end

local function unlockAllBotKVs()
    local unlocked = 0
    local q = db.storeQuery(string.format(
        "SELECT `id` FROM `players` WHERE `account_id` = %d", BOT_ACCOUNT_ID
    ))
    if not q then return 0 end
    repeat
        local guid = result.getNumber(q, "id")
        local p = Player(guid) or Game.getOfflinePlayer(guid)
        if p then
            local skillKv  = p:kv():scoped("hireling-skills")
            local outfitKv = p:kv():scoped("hireling-outfits")
            for _, s in ipairs(HIRELING_SKILL_NAMES)  do skillKv:set(s, true) end
            for _, o in ipairs(HIRELING_OUTFIT_NAMES) do outfitKv:set(o, true) end
            p:save()
            unlocked = unlocked + 1
        end
    until not result.next(q)
    result.free(q)
    return unlocked
end

-- ---------- talkaction ----------

local houseBackfill = TalkAction("/housebackfill")

function houseBackfill.onSay(player, words, param)
    logCommand(player, words, param)
    print("[housebackfill] firing - by " .. player:getName())
    player:sendTextMessage(MESSAGE_LOGIN, "[housebackfill] starting...")

    local cfg = loadConfig()
    local target_pct     = cfg.houseOccupancyTargetPct        or DEFAULT_TARGET_PCT
    local hireling_pct   = cfg.houseBackfillHirelingChancePct or DEFAULT_HIRELING_PCT
    local decoration_pct = cfg.houseBackfillTileDecorationPct or DEFAULT_DECORATION_PCT
    print(string.format(
        "[housebackfill] config: target=%d%% hireling=%d%% decor=%d%%",
        target_pct, hireling_pct, decoration_pct
    ))

    local templates, dumpedHouses, loadErr = loadTemplatesAndDumpedSet()
    if not templates then
        player:sendCancelMessage("[housebackfill] " .. (loadErr or "template load failed"))
        return true
    end
    local dumpedCount = 0
    for _ in pairs(dumpedHouses) do dumpedCount = dumpedCount + 1 end
    print(string.format(
        "[housebackfill] loaded %d templates from %d dumped houses",
        #templates, dumpedCount
    ))

    if #templates == 0 then
        player:sendCancelMessage("[housebackfill] template pool is empty")
        return true
    end

    local botsByTown = buildBotPool()

    -- Walk all houses: count totals, count owned, collect backfill candidates
    -- (unowned AND not in the dump).
    local totalHouses, currentlyOwned = 0, 0
    local candidates = {}
    for _, house in pairs(Game.getHouses()) do
        totalHouses = totalHouses + 1
        if house:getOwnerGuid() > 0 then
            currentlyOwned = currentlyOwned + 1
        elseif not dumpedHouses[house:getId()] then
            candidates[#candidates + 1] = house
        end
    end

    local targetOwned = math.floor(totalHouses * target_pct / 100)
    local needed = math.max(0, targetOwned - currentlyOwned)
    needed = math.min(needed, #candidates)

    print(string.format(
        "[housebackfill] totalHouses=%d currentlyOwned=%d targetOwned=%d candidates=%d needed=%d",
        totalHouses, currentlyOwned, targetOwned, #candidates, needed
    ))

    -- Even if the ownership target is already met (needed=0), we still want
    -- to run the bare-pass over existing bot-owned houses to fill any
    -- sparse-decorated dumped houses (e.g., Paupers Palace flats).
    if needed == 0 then
        print(string.format(
            "[housebackfill] ownership target=%d%% already met (%d/%d). Skipping to bare-pass.",
            target_pct, currentlyOwned, totalHouses
        ))
    end

    math.randomseed(os.time())
    shuffleN(candidates, 3)

    -- Deterministic hireling pre-selection: first N of the shuffled list.
    local hirelingTargetCount = math.floor(needed * hireling_pct / 100)
    local hirelingSet = {}
    for i = 1, hirelingTargetCount do
        if candidates[i] then hirelingSet[candidates[i]:getId()] = true end
    end

    -- Pre-flight hireling names already in DB.
    local usedNames = {}
    local nq = db.storeQuery("SELECT `name` FROM `player_hirelings`")
    if nq then
        repeat
            usedNames[result.getString(nq, "name")] = true
        until not result.next(nq)
        result.free(nq)
    end

    local stats = {
        assigned            = 0,
        same_town           = 0,
        cross_town          = 0,
        no_bot              = 0,
        items_spawned       = 0,
        items_skipped       = 0,
        structural_skipped  = 0,
        hirelings_spawned   = 0,
        hirelings_failed    = 0,
        decorated_tiles     = 0,
        bare_houses_filled  = 0,
    }

    -- Run the backfill loop in chunks across multiple ticks. Doing it all in
    -- one synchronous Lua tick crashed canary at ~175/212 houses on the
    -- first attempt — SEGV with no Lua error, almost certainly some
    -- downstream callback being overwhelmed by 200+ setHouseOwner +
    -- Game.createItem + npc:place() invocations stacked back-to-back.
    -- Chunked execution gives the dispatcher + map subsystems room to
    -- breathe between batches.
    local state = {
        candidates    = candidates,
        needed        = needed,
        templates     = templates,
        botsByTown    = botsByTown,
        hirelingSet   = hirelingSet,
        usedNames     = usedNames,
        stats         = stats,
        target_pct    = target_pct,
        decoration_pct = decoration_pct,
        totalHouses   = totalHouses,
        targetOwned   = targetOwned,
        playerGuid    = player:getGuid(),
    }

    processBackfillChunk(state, 1)
    return true
end

CHUNK_SIZE = 3  -- houses per tick; very small to localize any deterministic crash
CHUNK_DELAY_MS = 500  -- generous gap between chunks

-- Process one house with full error isolation. Returns true on success,
-- false if anything in this house's work failed.
function processOneHouse(state, house, houseIdx)
    local townId = house:getTown():getId()
    local botGuid, isCross = pickBot(state.botsByTown, townId)

    if not botGuid then
        state.stats.no_bot = state.stats.no_bot + 1
        return true
    end

    -- Pcall around all map mutations so a Lua-level error doesn't take down
    -- the whole talkaction. SEGVs from C++ still kill the process, but a
    -- failed pcall here at least helps localize which house triggered it
    -- (the next chunk starts and we see one less house processed).
    local ok, err = pcall(function()
        house:setHouseOwner(botGuid)
        state.stats.assigned = state.stats.assigned + 1
        if isCross then state.stats.cross_town = state.stats.cross_town + 1
        else state.stats.same_town = state.stats.same_town + 1 end

        -- Force House::houseTiles to be fully populated before iterating.
        -- Without this the loop sees only the few tiles that incidental
        -- traffic already materialized — usually 3-10 instead of hundreds.
        materializeHouseTiles(house)

        for _, tile in ipairs(house:getTiles()) do
            -- Skip walls, doors, windows, depots, mailboxes, any tile with a
            -- non-movable item besides the ground. These would otherwise get
            -- decoration items "stuck" to walls or floating through doors.
            if isStructuralTile(tile) then
                state.stats.structural_skipped = (state.stats.structural_skipped or 0) + 1
            elseif math.random(100) <= state.decoration_pct then
                local template = state.templates[math.random(#state.templates)]
                local pos = tile:getPosition()
                local existing = {}
                for _, item in ipairs(tile:getItems() or {}) do
                    existing[item:getId()] = true
                end
                local g = tile:getGround()
                if g then existing[g:getId()] = true end

                local placedAny = false
                for k = #template, 1, -1 do
                    local it = template[k]
                    local itemId = it[1]
                    local count = it[2] or 1
                    if existing[itemId] then
                        state.stats.items_skipped = state.stats.items_skipped + 1
                    else
                        Game.createItem(itemId, count, pos)
                        existing[itemId] = true
                        state.stats.items_spawned = state.stats.items_spawned + 1
                        placedAny = true
                    end
                end
                if placedAny then state.stats.decorated_tiles = state.stats.decorated_tiles + 1 end
            end
        end

        if state.hirelingSet[house:getId()] then
            if placeHireling(house, botGuid, state.usedNames) then
                state.stats.hirelings_spawned = state.stats.hirelings_spawned + 1
            else
                state.stats.hirelings_failed = state.stats.hirelings_failed + 1
            end
        end
    end)

    if not ok then
        print(string.format("[housebackfill] house #%d (id=%d) errored: %s",
            houseIdx, house:getId(), tostring(err)))
        return false
    end
    return true
end

function processBackfillChunk(state, startIdx)
    local endIdx = math.min(startIdx + CHUNK_SIZE - 1, state.needed)

    print(string.format("[housebackfill] chunk %d-%d/%d starting",
        startIdx, endIdx, state.needed))

    for i = startIdx, endIdx do
        local house = state.candidates[i]
        if house then processOneHouse(state, house, i) end
    end

    if endIdx < state.needed then
        addEvent(processBackfillChunk, CHUNK_DELAY_MS, state, endIdx + 1)
    else
        -- Primary backfill done. Build a queue of all bot-owned houses for
        -- the bare-pass (second phase): any house owned by a bot whose
        -- movable-tile count is below the 20% threshold gets randomized
        -- decoration. Targets dumped-but-sparse houses like the Paupers
        -- Palace flats where /houseingest placed mostly structural items
        -- and only 1-2 movable decorations survive the save filter.
        state.bareQueue = {}
        state.houseSizes = {}  -- houseId -> declared interior size from MySQL
        local bq = db.storeQuery(string.format(
            "SELECT h.`id`, h.`size` FROM `houses` h INNER JOIN `players` p ON h.`owner` = p.`id` WHERE p.`account_id` = %d",
            BOT_ACCOUNT_ID
        ))
        if bq then
            repeat
                local hid = result.getNumber(bq, "id")
                local hsize = result.getNumber(bq, "size")
                local house = House(hid)
                if house then
                    table.insert(state.bareQueue, house)
                    state.houseSizes[hid] = hsize
                end
            until not result.next(bq)
            result.free(bq)
        end
        print(string.format("[housebackfill] primary done. bare-pass: %d bot-owned houses queued",
            #state.bareQueue))

        if #state.bareQueue > 0 then
            processBarePassChunk(state, 1)
        else
            local unlocked = unlockAllBotKVs()
            local msg = string.format(
                "[housebackfill] target=%d%% (%d/%d) | assigned=%d (same_town=%d cross_town=%d) no_bot=%d | tiles_decorated=%d items=%d skipped=%d structural=%d | hirelings=%d failed=%d | bare_filled=0 | bots_unlocked=%d",
                state.target_pct, state.targetOwned, state.totalHouses,
                state.stats.assigned, state.stats.same_town, state.stats.cross_town, state.stats.no_bot,
                state.stats.decorated_tiles, state.stats.items_spawned, state.stats.items_skipped, state.stats.structural_skipped,
                state.stats.hirelings_spawned, state.stats.hirelings_failed,
                unlocked
            )
            print(msg)
            local p = Player(state.playerGuid)
            if p then p:sendTextMessage(MESSAGE_LOGIN, msg) end
        end
    end
end

-- Second-pass chunk. Same chunk-size/delay pacing as the primary pass so we
-- don't reintroduce the SEGV pattern. For each bot-owned house in the queue:
-- check isHouseBare(), and if bare, run the same per-tile decoration loop
-- (structural skip + reverse template iteration + dedup) at the configured
-- decoration_pct. /houseingest's items already on the tile are skipped by
-- the existing[itemId] dedup check.
function processBarePassChunk(state, startIdx)
    local endIdx = math.min(startIdx + CHUNK_SIZE - 1, #state.bareQueue)
    print(string.format("[housebackfill] bare-pass chunk %d-%d/%d",
        startIdx, endIdx, #state.bareQueue))

    for i = startIdx, endIdx do
        local house = state.bareQueue[i]
        if not house then goto continue end
        local size = state.houseSizes[house:getId()]
        -- MUST run before isHouseBare and the decoration loop. Without
        -- this, countMovableTilesInHouse iterates a near-empty vector and
        -- both bareness detection AND decoration see only a handful of
        -- tiles per house.
        materializeHouseTiles(house)
        local bareOk, bareResult = pcall(isHouseBare, house, size)
        if bareOk and bareResult then
            local ok, err = pcall(function()
                -- Only count houses that actually had ≥1 item placed; the
                -- previous unconditional bump made the summary lie when the
                -- inner loop produced zero items (e.g., flood-fill found
                -- only structural tiles).
                local placedAny_anywhere = false
                for _, tile in ipairs(house:getTiles()) do
                    if isStructuralTile(tile) then
                        state.stats.structural_skipped = state.stats.structural_skipped + 1
                    elseif math.random(100) <= state.decoration_pct then
                        local template = state.templates[math.random(#state.templates)]
                        local pos = tile:getPosition()
                        local existing = {}
                        for _, item in ipairs(tile:getItems() or {}) do
                            existing[item:getId()] = true
                        end
                        local g = tile:getGround()
                        if g then existing[g:getId()] = true end

                        local placedAny = false
                        for k = #template, 1, -1 do
                            local it = template[k]
                            local itemId = it[1]
                            local count = it[2] or 1
                            if existing[itemId] then
                                state.stats.items_skipped = state.stats.items_skipped + 1
                            else
                                Game.createItem(itemId, count, pos)
                                existing[itemId] = true
                                state.stats.items_spawned = state.stats.items_spawned + 1
                                placedAny = true
                            end
                        end
                        if placedAny then
                            state.stats.decorated_tiles = state.stats.decorated_tiles + 1
                            placedAny_anywhere = true
                        end
                    end
                end
                if placedAny_anywhere then
                    state.stats.bare_houses_filled = state.stats.bare_houses_filled + 1
                end
            end)
            if not ok then
                print(string.format("[housebackfill] bare-pass house id=%d errored: %s",
                    house:getId(), tostring(err)))
            end
        elseif not bareOk then
            print(string.format("[housebackfill] bare-pass isHouseBare(id=%d) errored: %s",
                house:getId(), tostring(bareResult)))
        end
        ::continue::
    end

    if endIdx < #state.bareQueue then
        addEvent(processBarePassChunk, CHUNK_DELAY_MS, state, endIdx + 1)
    else
        local unlocked = unlockAllBotKVs()
        local msg = string.format(
            "[housebackfill] target=%d%% (%d/%d) | assigned=%d (same_town=%d cross_town=%d) no_bot=%d | tiles_decorated=%d items=%d skipped=%d structural=%d | hirelings=%d failed=%d | bare_filled=%d | bots_unlocked=%d",
            state.target_pct, state.targetOwned, state.totalHouses,
            state.stats.assigned, state.stats.same_town, state.stats.cross_town, state.stats.no_bot,
            state.stats.decorated_tiles, state.stats.items_spawned, state.stats.items_skipped, state.stats.structural_skipped,
            state.stats.hirelings_spawned, state.stats.hirelings_failed,
            state.stats.bare_houses_filled,
            unlocked
        )
        print(msg)
        local p = Player(state.playerGuid)
        if p then p:sendTextMessage(MESSAGE_LOGIN, msg) end
    end
end

houseBackfill:separator(" ")
houseBackfill:groupType("god")
houseBackfill:register()
