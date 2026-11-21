-- ============================================================================
-- Bot storage auto-unlock (#83). Quest/access storages (wagons, shrines, levers, teleports)
-- come from the authored CSV data/bot/authored/bot_storages.csv instead of a hard-coded table,
-- and each row is level- and vocation-gated:
--   storage        Lua path under Storage (e.g. Storage.Quest.U8_4.InServiceOfYalahar.Questline,
--                  Storage.Quest.U8_54.TheNewFrontier.Mission02[1]) or a plain number
--   value          value to set
--   min_level      0 = any level; otherwise the bot needs level >= min_level
--   vocation_mask  0 = any; else bit (1 << (baseVocationId - 1)): 1 sorc, 2 druid, 4 paladin, 8 knight
--   note           free text (may contain commas: everything after the 4th comma)
-- Applied right after Game.loadBotPlayer (bot_manager.lua). The file is parsed once, on first use,
-- so the Storage constants are certainly loaded; a row whose key does not resolve is logged and
-- skipped (never a silent miss). Inspect a bot with `/cavebot <bot> storages`.
-- ============================================================================
BotStorages = BotStorages or {}

local CSV_PATH = "data/bot/authored/bot_storages.csv"

local function resolveKey(path)
	local n = tonumber(path)
	if n then
		return n
	end
	if path:sub(1, 8) ~= "Storage." and path ~= "Storage" then
		return nil
	end
	local node = Storage
	for seg in path:sub(9):gmatch("[^%.]+") do
		local name, idx = seg:match("^([%w_]+)%[(%d+)%]$")
		if name then
			node = type(node) == "table" and node[name] or nil
			node = type(node) == "table" and node[tonumber(idx)] or nil
		else
			node = type(node) == "table" and node[seg] or nil
		end
		if node == nil then
			return nil
		end
	end
	return type(node) == "number" and node or nil
end

function BotStorages.load()
	local f = io.open(CSV_PATH, "r")
	if not f then
		logger.error("[BOT_STORAGES] {} missing: bots get no quest-access storages", CSV_PATH)
		BotStorages.rows = {}
		return BotStorages.rows
	end
	local rows, bad, lineNo = {}, 0, 0
	for line in f:lines() do
		lineNo = lineNo + 1
		line = line:gsub("\r$", "")
		if lineNo > 1 and line ~= "" and line:sub(1, 1) ~= "#" then
			local path, value, minLevel, vocMask, note = line:match("^([^,]+),([^,]+),([^,]*),([^,]*),?(.*)$")
			local key = path and resolveKey(path)
			value = tonumber(value)
			if not key or not value then
				bad = bad + 1
				logger.error("[BOT_STORAGES] {}:{} unresolved row '{}'", CSV_PATH, lineNo, line)
			else
				rows[#rows + 1] = { path = path, key = key, value = value, minLevel = tonumber(minLevel) or 0, vocMask = tonumber(vocMask) or 0, note = note }
			end
		end
	end
	f:close()
	BotStorages.rows = rows
	BotStorages.badRows = bad
	logger.info("[BOT_STORAGES] loaded rows={} unresolved={} from {}", #rows, bad, CSV_PATH)
	return rows
end

local function rowApplies(row, level, vocBit)
	if row.minLevel > 0 and level < row.minLevel then
		return false
	end
	if row.vocMask ~= 0 and (vocBit == 0 or bit.band(row.vocMask, vocBit) == 0) then
		return false
	end
	return true
end

local function vocationBit(player)
	local voc = player:getVocation()
	local base = voc and voc:getBase()
	local id = base and base:getId() or 0
	return id > 0 and bit.lshift(1, id - 1) or 0
end

-- Returns (applied, gated) for this player. Before #83 every bot got every row, and storages
-- persist in player_storage, so a gated row still holding exactly the value the old table set
-- is cleared (-1 removes the key): otherwise a low-level bot would keep the unlock forever.
-- A gated key holding any other value (progress a script really made) is left alone.
function BotStorages.apply(player)
	local rows = BotStorages.rows or BotStorages.load()
	local level, vocBit = player:getLevel(), vocationBit(player)
	local applied, gated = 0, 0
	for _, row in ipairs(rows) do
		if rowApplies(row, level, vocBit) then
			player:setStorageValue(row.key, row.value)
			applied = applied + 1
		else
			if player:getStorageValue(row.key) == row.value then
				player:setStorageValue(row.key, -1)
			end
			gated = gated + 1
		end
	end
	return applied, gated
end

-- `/cavebot <bot> storages`: one summary line + every gated row the bot does / does not get.
function BotStorages.dump(name)
	local player = Player(name)
	if not player then
		return string.format("[BOT_STORAGES] '%s' is not online (a hibernated bot keeps its storages; wake it to inspect)", name)
	end
	local rows = BotStorages.rows or BotStorages.load()
	local level, vocBit = player:getLevel(), vocationBit(player)
	local out, set, gated, mismatch = {}, 0, 0, 0
	for _, row in ipairs(rows) do
		local cur = player:getStorageValue(row.key)
		if rowApplies(row, level, vocBit) then
			set = set + 1
			if cur ~= row.value then
				mismatch = mismatch + 1
				out[#out + 1] = string.format("  MISSING %s=%d (has %d)", row.path, row.value, cur)
			end
		else
			gated = gated + 1
			out[#out + 1] = string.format("  gated %s min_level=%d voc=%d (has %d)", row.path, row.minLevel, row.vocMask, cur)
		end
	end
	table.insert(out, 1, string.format("[BOT_STORAGES] %s level=%d rows=%d applies=%d gated=%d missing=%d", player:getName(), level, #rows, set, gated, mismatch))
	return table.concat(out, "\n")
end
