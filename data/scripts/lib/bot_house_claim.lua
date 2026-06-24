-- ============================================================================
-- bot_house_claim.lua — shared helpers for the player-facing house takeover
-- (`/cavebot house "<Name>" owner | sub-owner`) and the BotHouseReclaim
-- globalevent.
--
-- A normal player can take a BOT-owned house ("owner") or add themselves as a
-- native sub-owner ("sub-owner"). The bot's ORIGINAL interior layout is
-- snapshotted ONCE into bot_house_origin / bot_house_origin_items (permanent —
-- survives owner changes and anything the new owner adds). The BotHouseReclaim
-- globalevent restores that layout verbatim and hands the house back to its bot
-- whenever it later falls vacant ("back on the market").
--
-- House interior items live in tile_store as ONE serialized blob per house,
-- rewritten wholesale from live memory on every world save — so a snapshot can
-- only be restored by re-placing items into the LIVE map (Game.createItem),
-- exactly as /houseingest and /housebackfill already do.
-- ============================================================================

BotHouseClaim = BotHouseClaim or {}

-- Bot guids occupy 65001..66004, skipping the reserved 65201..65207 (see CLAUDE.md).
BotHouseClaim.BOT_GUID_MIN = 65001
BotHouseClaim.BOT_GUID_MAX = 66004
BotHouseClaim.BOT_GUID_RESERVED_MIN = 65201
BotHouseClaim.BOT_GUID_RESERVED_MAX = 65207

function BotHouseClaim.isBotGuid(guid)
	if not guid or guid <= 0 then return false end
	if guid < BotHouseClaim.BOT_GUID_MIN or guid > BotHouseClaim.BOT_GUID_MAX then return false end
	if guid >= BotHouseClaim.BOT_GUID_RESERVED_MIN and guid <= BotHouseClaim.BOT_GUID_RESERVED_MAX then return false end
	return true
end

-- ---- schema (idempotent; migrations here are applied manually elsewhere) ----
local schemaReady = false
function BotHouseClaim.ensureSchema()
	if schemaReady then return end
	db.query([[CREATE TABLE IF NOT EXISTS `bot_house_origin` (
		`house_id` INT NOT NULL,
		`bot_guid` INT NOT NULL,
		`created_at` INT NOT NULL DEFAULT 0,
		PRIMARY KEY (`house_id`)
	) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3]])
	db.query([[CREATE TABLE IF NOT EXISTS `bot_house_origin_items` (
		`id` INT NOT NULL AUTO_INCREMENT,
		`house_id` INT NOT NULL,
		`pos_x` INT NOT NULL,
		`pos_y` INT NOT NULL,
		`pos_z` INT NOT NULL,
		`item_id` INT NOT NULL,
		`count` INT NOT NULL DEFAULT 1,
		`seq` INT NOT NULL DEFAULT 0,
		PRIMARY KEY (`id`),
		KEY `house_id_index` (`house_id`)
	) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3]])
	schemaReady = true
end

-- ---- tile materialization (House::houseTiles is lazily populated) ----------
-- Lifted from /housebackfill: flood-fill from the entry's 3D Moore neighborhood
-- so house:getTiles() reflects the full reachable interior before we read it.
local FLOOD_MAX_TILES = 4096
local FLOOD_NEIGHBOR_OFFSETS = {
	{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
}
local SEED_PROBE_OFFSETS = {}
for dz = -1, 1 do
	for dy = -1, 1 do
		for dx = -1, 1 do
			SEED_PROBE_OFFSETS[#SEED_PROBE_OFFSETS + 1] = { dx, dy, dz }
		end
	end
end

function BotHouseClaim.materialize(house)
	local entry = house:getExitPosition()
	if not entry then return 0 end
	local houseId = house:getId()

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
	local n = 0

	while #stack > 0 and n < FLOOD_MAX_TILES do
		local p = table.remove(stack)
		local tile = Tile(Position(p[1], p[2], p[3]))
		if tile then
			local th = tile:getHouse()
			if th and th:getId() == houseId then
				n = n + 1
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
	return n
end

-- ---- snapshot (capture the bot's original movable-item layout, ONCE) -------
-- No-op if an origin record already exists (keeps the canonical first layout,
-- exactly as the user asked — it must persist across owner changes). Returns
-- created(bool), itemCount(number).
function BotHouseClaim.snapshotIfAbsent(house, botGuid)
	BotHouseClaim.ensureSchema()
	local hid = house:getId()

	local existing = db.storeQuery(string.format("SELECT `house_id` FROM `bot_house_origin` WHERE `house_id` = %d", hid))
	if existing then
		Result.free(existing)
		return false, 0
	end

	BotHouseClaim.materialize(house)
	db.query(string.format(
		"INSERT INTO `bot_house_origin` (`house_id`,`bot_guid`,`created_at`) VALUES (%d,%d,%d)",
		hid, botGuid, os.time()))

	-- Capture only movable items (what the engine actually persists to the
	-- house — structural walls/doors/windows come from the OTBM map, not
	-- tile_store). Stack order is recorded as `seq` per tile.
	local values = {}
	for _, tile in ipairs(house:getTiles()) do
		local pos = tile:getPosition()
		local groundId
		local g = tile:getGround()
		if g then groundId = g:getId() end
		local seq = 0
		for _, item in ipairs(tile:getItems() or {}) do
			local id = item:getId()
			if id ~= groundId then
				local iType = ItemType(id)
				if iType and iType:isMovable() then
					seq = seq + 1
					local count = item:getCount() or 1
					values[#values + 1] = string.format("(%d,%d,%d,%d,%d,%d,%d)",
						hid, pos.x, pos.y, pos.z, id, count, seq)
				end
			end
		end
	end

	-- Chunked insert (a furnished house can hold hundreds of items).
	local CHUNK = 150
	local i = 1
	while i <= #values do
		local j = math.min(i + CHUNK - 1, #values)
		db.query("INSERT INTO `bot_house_origin_items` (`house_id`,`pos_x`,`pos_y`,`pos_z`,`item_id`,`count`,`seq`) VALUES "
			.. table.concat(values, ",", i, j))
		i = j + 1
	end

	return true, #values
end

-- ---- restore (re-place the snapshot into the live house, hand back to bot) --
-- Idempotent: clears current movable items on the snapshot tiles before
-- recreating, so a retried run (e.g. after a crash mid-restore) doesn't stack
-- duplicates. Returns the number of items recreated.
function BotHouseClaim.restore(house, botGuid)
	BotHouseClaim.ensureSchema()
	local hid = house:getId()

	local rows = {}
	local posSeen = {}
	local posList = {}
	local q = db.storeQuery(string.format(
		"SELECT `pos_x`,`pos_y`,`pos_z`,`item_id`,`count` FROM `bot_house_origin_items` WHERE `house_id` = %d ORDER BY `pos_x`,`pos_y`,`pos_z`,`seq` ASC",
		hid))
	if q then
		repeat
			local x = Result.getNumber(q, "pos_x")
			local y = Result.getNumber(q, "pos_y")
			local z = Result.getNumber(q, "pos_z")
			rows[#rows + 1] = { x = x, y = y, z = z, id = Result.getNumber(q, "item_id"), count = Result.getNumber(q, "count") }
			local key = string.format("%d:%d:%d", x, y, z)
			if not posSeen[key] then
				posSeen[key] = true
				posList[#posList + 1] = { x, y, z }
			end
		until not Result.next(q)
		Result.free(q)
	end

	-- Clear current movable items on each snapshot tile (idempotency).
	for _, p in ipairs(posList) do
		local tile = Tile(Position(p[1], p[2], p[3]))
		if tile then
			local groundId
			local g = tile:getGround()
			if g then groundId = g:getId() end
			local toRemove = {}
			for _, item in ipairs(tile:getItems() or {}) do
				local id = item:getId()
				if id ~= groundId then
					local iType = ItemType(id)
					if iType and iType:isMovable() then
						toRemove[#toRemove + 1] = item
					end
				end
			end
			for _, item in ipairs(toRemove) do
				item:remove()
			end
		end
	end

	-- Recreate the snapshot layout, then hand ownership back to the bot.
	for _, r in ipairs(rows) do
		Game.createItem(r.id, r.count, Position(r.x, r.y, r.z))
	end
	house:setHouseOwner(botGuid)

	return #rows
end
