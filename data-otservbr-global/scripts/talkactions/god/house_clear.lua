-- /houseclear — undo /houseingest.
--
-- Removes every movable/bed/carpet/trashHolder item from every tile of the
-- target house (i.e., everything that *would* be saved to tile_store, leaving
-- the OTBM-placed walls/doors/ground intact). Also despawns + deletes any
-- hireling whose position is inside the target house. Then clears ownership.
--
-- Usage:
--   /houseclear                — clear EVERY currently-owned house on the server
--   /houseclear here           — clear the house under your feet
--   /houseclear <house_id>     — clear by ID
--   /houseclear <house name>   — clear by exact name

local houseClear = TalkAction("/houseclear")

-- Mirrors Canary's isSavedToHouses() at item.cpp:2313 using available Lua
-- bindings on ItemType. We use this to only remove items that COULD have been
-- player-placed decoration — never OTBM-placed structure, which would
-- otherwise reappear on the next server restart anyway.
local function passesSaveFilter(itemId)
    local iType = ItemType(itemId)
    if not iType then return false end
    if iType:isMovable() then return true end
    if iType:getWrapableTo() > 0 then return true end
    if iType:isDoor() then return true end
    if iType:isWritable() then return true end
    local t = iType:getType()
    if t == ITEM_TYPE_BED or t == ITEM_TYPE_CARPET or t == ITEM_TYPE_TRASHHOLDER then
        return true
    end
    return false
end

local function clearOne(house)
    local removed = 0
    local hirelings_removed = 0

    local houseTilePos = {}
    for _, tile in ipairs(house:getTiles()) do
        local p = tile:getPosition()
        houseTilePos[p.x .. "," .. p.y .. "," .. p.z] = true
        local items = tile:getItems() or {}
        for i = #items, 1, -1 do
            local item = items[i]
            if passesSaveFilter(item:getId()) then
                item:remove()
                removed = removed + 1
            end
        end
    end

    if HIRELINGS then
        for i = #HIRELINGS, 1, -1 do
            local h = HIRELINGS[i]
            local k = h.posx .. "," .. h.posy .. "," .. h.posz
            if houseTilePos[k] then
                if h.cid and h.cid > 0 then
                    local npc = Npc(h.cid)
                    if npc then npc:remove() end
                end
                db.query(string.format("DELETE FROM `player_hirelings` WHERE `id`=%d", h.id))
                table.remove(HIRELINGS, i)
                hirelings_removed = hirelings_removed + 1
            end
        end
    end

    local prevOwner = house:getOwnerGuid()
    house:setHouseOwner(0)
    return removed, hirelings_removed, prevOwner
end

local function resolveOne(player, param)
    if param == "here" then
        local tile = Tile(player:getPosition())
        return tile and tile:getHouse() or nil
    end
    local asNum = tonumber(param)
    if asNum then return House(asNum) end
    for _, h in pairs(Game.getHouses()) do
        if h:getName() == param then return h end
    end
    return nil
end

CLEAR_CHUNK_SIZE = 3
CLEAR_CHUNK_DELAY_MS = 500

-- Sweep every currently-owned house in chunks. Done as an async chain so
-- 600+ setHouseOwner(0) + transferToDepot + door-description-updates don't
-- pile up in one synchronous Lua tick — the monolithic version SEGV'd
-- canary after ~95 houses, same root cause we hit on /housebackfill.
function processClearChunk(state)
    local processed = 0
    for _ = 1, CLEAR_CHUNK_SIZE do
        local house = table.remove(state.queue)
        if not house then break end
        local ok, r, hr = pcall(clearOne, house)
        if ok then
            state.total_houses = state.total_houses + 1
            state.total_items = state.total_items + (r or 0)
            state.total_hirelings = state.total_hirelings + (hr or 0)
        else
            state.errors = state.errors + 1
            print("[houseclear] clearOne errored: " .. tostring(r))
        end
        processed = processed + 1
    end

    local remaining = #state.queue
    if remaining > 0 then
        if state.total_houses % 30 == 0 then
            print(string.format("[houseclear] progress: %d cleared, %d remaining",
                state.total_houses, remaining))
        end
        addEvent(processClearChunk, CLEAR_CHUNK_DELAY_MS, state)
    else
        local msg = string.format(
            "[houseclear] sweep done: %d houses, %d items, %d hirelings, %d errors",
            state.total_houses, state.total_items, state.total_hirelings, state.errors
        )
        print(msg)
        local p = Player(state.playerGuid)
        if p then p:sendTextMessage(MESSAGE_LOGIN, msg) end
    end
end

function houseClear.onSay(player, words, param)
    logCommand(player, words, param)

    -- No arg: chunked sweep across all owned houses.
    if param == "" or param == nil then
        local queue = {}
        for _, h in pairs(Game.getHouses()) do
            if h:getOwnerGuid() > 0 then queue[#queue + 1] = h end
        end
        if #queue == 0 then
            player:sendTextMessage(MESSAGE_LOGIN, "[houseclear] no owned houses to clear")
            return true
        end
        player:sendTextMessage(MESSAGE_LOGIN,
            string.format("[houseclear] sweeping %d houses (chunked, ~%ds)...",
                #queue, math.ceil(#queue / CLEAR_CHUNK_SIZE * CLEAR_CHUNK_DELAY_MS / 1000)))
        local state = {
            queue = queue,
            total_houses = 0,
            total_items = 0,
            total_hirelings = 0,
            errors = 0,
            playerGuid = player:getGuid(),
        }
        processClearChunk(state)
        return true
    end

    local house = resolveOne(player, param)
    if not house then
        player:sendCancelMessage("House not found.")
        return true
    end

    local r, hr, prevOwner = clearOne(house)
    local msg = string.format(
        "[houseclear] house=%d (%s): removed %d items, %d hirelings, owner %d -> 0",
        house:getId(), house:getName(), r, hr, prevOwner
    )
    player:sendTextMessage(MESSAGE_LOGIN, msg)
    print(msg)
    return true
end

houseClear:separator(" ")
houseClear:groupType("god")
houseClear:register()
