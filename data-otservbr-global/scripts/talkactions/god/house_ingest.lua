-- /houseingest — load house_dump_data.lua, assign bot owners, spawn items.
--
-- Expects house_dump_data.lua at the canary working directory root, generated
-- by tools/house_ingestion/build_ingest_data.py from a house_dump.json
-- captured by the OTClient game_housedump mod.
--
-- Phase A: for each dumped house (including guildhalls — Canary's setOwner
-- accepts any player guid regardless of the guildHall flag), pick a bot from
-- the same town that doesn't already own a house, call house:setHouseOwner.
--
-- Phase B: for each tile, spawn every item from the dump that ISN'T already
-- present on the tile. The OTBM placed walls/doors/ground at server boot, so
-- duplicates are skipped to avoid visual stacking. Anything else in the dump
-- gets created via Game.createItem and lives in memory.
--
-- Persistence is delegated to Canary's normal save cycle (saveTile +
-- isSavedToHouses), which is the safest path because we use the exact same
-- code path every player uses when decorating a house. The trade-off:
--   * Movable items, wrappable furniture, beds, carpets, trophies, tapestries,
--     writable signs, containers with contents → saved to tile_store → persist
--     across restarts.
--   * Structural items (framework walls, chimneys, custom ground tiles used
--     as decoration) → spawn for the session but vanish on next server save.
--     Re-run /houseingest after restart to re-place them.
--
-- Idempotent: re-running after a restart re-spawns missing structural items
-- and skips persistent ones (which are already on the tile from tile_store).
-- Use /houseclear to fully wipe a house.

local DUMP_FILE = "house_dump_data.lua"
local BOT_ACCOUNT_ID = 65000

local houseIngest = TalkAction("/houseingest")

local function buildBotPool()
    -- bots by town, then filter out bots already owning houses
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

    for townId, pool in pairs(botsByTown) do
        local filtered = {}
        for _, guid in ipairs(pool) do
            if not owned[guid] then filtered[#filtered + 1] = guid end
        end
        botsByTown[townId] = filtered
    end

    return botsByTown
end

function houseIngest.onSay(player, words, param)
    logCommand(player, words, param)
    print("[houseingest] firing — by " .. player:getName())
    player:sendTextMessage(MESSAGE_LOGIN, "[houseingest] starting...")

    local t0 = os.clock()
    local okLoad, data = pcall(dofile, DUMP_FILE)
    local tLoad = os.clock() - t0
    print(string.format("[houseingest] dofile done in %.2fs (ok=%s, type=%s)", tLoad, tostring(okLoad), type(data)))
    if not okLoad or type(data) ~= "table" then
        local err = "[houseingest] dofile FAILED: " .. tostring(data)
        print(err)
        player:sendCancelMessage(err)
        return true
    end
    if not data.houses or not data.guildhalls then
        print("[houseingest] data missing houses/guildhalls fields")
        player:sendCancelMessage("dump data missing 'houses' or 'guildhalls'")
        return true
    end
    local houseCount = 0
    for _ in pairs(data.houses) do houseCount = houseCount + 1 end
    print(string.format("[houseingest] data loaded: %d houses, %d npcs", houseCount, data.npcs and #data.npcs or 0))

    local botsByTown = buildBotPool()

    -- Phase A: ownership (guildhalls included — bots can own them too).
    -- Prefer a same-town bot. If that pool is empty (some towns have more
    -- houses than the stratified bot population gives them — Thais, Edron,
    -- Carlin, Yalahar etc.), fall back to any town that still has surplus
    -- bots. Tracked separately as cross_town_assigned.
    local assigned = {}
    local stats = {
        already_owned = 0,
        guildhall_assigned = 0,
        assigned_new = 0,
        cross_town_assigned = 0,
        no_bot_available = 0,
    }

    local function takeAnyBot()
        for tid, pool in pairs(botsByTown) do
            if #pool > 0 then
                return table.remove(pool, 1)
            end
        end
        return nil
    end

    for houseId, _ in pairs(data.houses) do
        local house = House(houseId)
        if house then
            local existing = house:getOwnerGuid()
            if existing > 0 then
                assigned[houseId] = existing
                stats.already_owned = stats.already_owned + 1
            else
                local townId = house:getTown():getId()
                local botGuid = nil
                local pool = botsByTown[townId]
                if pool and #pool > 0 then
                    botGuid = table.remove(pool, 1)
                else
                    botGuid = takeAnyBot()
                    if botGuid then
                        stats.cross_town_assigned = stats.cross_town_assigned + 1
                    end
                end
                if botGuid then
                    house:setHouseOwner(botGuid)
                    assigned[houseId] = botGuid
                    stats.assigned_new = stats.assigned_new + 1
                    if data.guildhalls[houseId] then
                        stats.guildhall_assigned = stats.guildhall_assigned + 1
                    end
                else
                    stats.no_bot_available = stats.no_bot_available + 1
                end
            end
        end
    end

    -- Phase B: items (no per-item filter; skip duplicates of what's already
    -- on the tile so we don't double the OTBM-placed walls/doors/ground)
    local spawned = 0
    local skipped_already_present = 0
    local skipped_no_owner = 0

    for houseId, tiles in pairs(data.houses) do
        if assigned[houseId] then
            for _, tileEntry in ipairs(tiles) do
                local pos = Position(tileEntry.p[1], tileEntry.p[2], tileEntry.p[3])
                local tile = Tile(pos)
                local existing = {}
                if tile then
                    local tileItems = tile:getItems() or {}
                    for _, item in ipairs(tileItems) do
                        existing[item:getId()] = true
                    end
                    local g = tile:getGround()
                    if g then existing[g:getId()] = true end
                end
                -- Iterate in reverse so the final tile vector ends up with
                -- the dump's bottom-most item at index 0 and topmost at the
                -- end. Tile::addThing inserts new common items at the front
                -- of the down-items section (tile.cpp:1145 — insert at
                -- getBeginDownItem()), so processing top→bottom leaves
                -- bottom→top order in the vector, which makes the in-game
                -- look cycle traverse items in placement order. Visual
                -- stacking is unaffected: carpets/onBottom items still
                -- render at floor level via item flags.
                for k = #tileEntry.i, 1, -1 do
                    local it = tileEntry.i[k]
                    local itemId = it[1]
                    local count = it[2] or 1
                    if existing[itemId] then
                        skipped_already_present = skipped_already_present + 1
                    else
                        Game.createItem(itemId, count, pos)
                        spawned = spawned + 1
                        existing[itemId] = true  -- guard against multi-occurrences in same tile
                    end
                end
            end
        else
            for _, tileEntry in ipairs(tiles) do
                skipped_no_owner = skipped_no_owner + #tileEntry.i
            end
        end
    end

    -- Phase C: hirelings.
    -- Each captured NPC becomes a Hireling row in player_hirelings, owned by
    -- the bot that owns the house. We mirror Player:addNewHireling but
    -- skip the lamp-creation side-effect (the bot doesn't need an inventory
    -- lamp; the hireling is already active in-house) and skip the looktype
    -- filter so any captured NPC outfit comes through verbatim. After insert
    -- + push into HIRELINGS, Hireling:spawn() creates the in-world NPC and
    -- the global checkHouseAccess() loop keeps it alive across restarts.
    local hireling_spawned = 0
    local hireling_dup_name = 0
    local hireling_no_owner = 0

    if data.npcs then
        -- Existing names (across all bots) to avoid INSERT duplicate-name conflicts.
        local existingNames = {}
        local q3 = db.storeQuery("SELECT `name` FROM `player_hirelings`")
        if q3 then
            repeat
                existingNames[result.getString(q3, "name")] = true
            until not result.next(q3)
            result.free(q3)
        end

        for _, npc in ipairs(data.npcs) do
            local houseId = npc.h
            local botGuid = assigned[houseId]
            if not botGuid then
                hireling_no_owner = hireling_no_owner + 1
            elseif existingNames[npc.n] then
                hireling_dup_name = hireling_dup_name + 1
            else
                -- Default sex from looktype parity (matches HIRELING_OUTFITS_TABLE
                -- where female is the lower (odd) of the dress pair, male the
                -- next even). Citizen default (128/136) we just call male.
                local sex = (npc.o.t % 2 == 1) and HIRELING_SEX.FEMALE or HIRELING_SEX.MALE

                local insertSql = string.format(
                    "INSERT INTO `player_hirelings` (`player_id`,`name`,`active`,`sex`,`posx`,`posy`,`posz`,`lookbody`,`lookfeet`,`lookhead`,`looklegs`,`looktype`) VALUES (%d,%s,1,%d,%d,%d,%d,%d,%d,%d,%d,%d)",
                    botGuid, db.escapeString(npc.n), sex,
                    npc.p[1], npc.p[2], npc.p[3],
                    npc.o.b, npc.o.f, npc.o.h, npc.o.l, npc.o.t
                )
                local ok = db.query(insertSql)
                local newId = ok and db.lastInsertId() or 0
                if newId and newId > 0 then
                    local h = Hireling:new()
                    h.id = newId
                    h.player_id = botGuid
                    h.name = npc.n
                    h.active = 1
                    h.sex = sex
                    h.posx = npc.p[1]; h.posy = npc.p[2]; h.posz = npc.p[3]
                    h.lookbody = npc.o.b; h.lookfeet = npc.o.f
                    h.lookhead = npc.o.h; h.looklegs = npc.o.l
                    h.looktype = npc.o.t
                    table.insert(HIRELINGS, h)
                    h:spawn()
                    existingNames[npc.n] = true
                    hireling_spawned = hireling_spawned + 1
                end
            end
        end
    end

    local msg = string.format(
        "[houseingest] owners: assigned=%d (incl. %d guildhalls, %d cross-town) already=%d no_bot=%d | items: spawned=%d already_present=%d no_owner_skip=%d | hirelings: spawned=%d dup_name=%d no_owner=%d",
        stats.assigned_new, stats.guildhall_assigned, stats.cross_town_assigned,
        stats.already_owned, stats.no_bot_available,
        spawned, skipped_already_present, skipped_no_owner,
        hireling_spawned, hireling_dup_name, hireling_no_owner
    )
    player:sendTextMessage(MESSAGE_LOGIN, msg)
    print(msg)
    return true
end

houseIngest:separator(" ")
houseIngest:groupType("god")
houseIngest:register()
