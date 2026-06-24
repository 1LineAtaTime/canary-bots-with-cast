-- Bot Equipment Data Loader
-- Loads pre-computed equipment loadouts from MySQL (bot_equipment table)
-- for fast lookup by level and vocation.
-- Must be loaded before bot_manager.lua (alphabetically "bot_e" < "bot_m")

BotEquipmentData = {
	loadouts = {}, -- {[level] = {[vocation] = {slot_head=id, slot_armor=id, ...}}}
}

function BotEquipmentData.loadAll()
	local startTime = os.clock()
	BotEquipmentData.loadouts = {}

	local resultId = db.storeQuery(
		"SELECT `level`, `vocation`, `slot_head`, `slot_armor`, `slot_legs`, " ..
		"`slot_feet`, `slot_right`, `slot_left` FROM `bot_equipment`"
	)
	if not resultId then
		logger.warn("[BotEquipmentData] No equipment data found in bot_equipment table")
		return
	end

	local count = 0
	repeat
		local level = Result.getNumber(resultId, "level")
		local voc = Result.getNumber(resultId, "vocation")

		if not BotEquipmentData.loadouts[level] then
			BotEquipmentData.loadouts[level] = {}
		end

		BotEquipmentData.loadouts[level][voc] = {
			slot_head  = Result.getNumber(resultId, "slot_head"),
			slot_armor = Result.getNumber(resultId, "slot_armor"),
			slot_legs  = Result.getNumber(resultId, "slot_legs"),
			slot_feet  = Result.getNumber(resultId, "slot_feet"),
			slot_right = Result.getNumber(resultId, "slot_right"),
			slot_left  = Result.getNumber(resultId, "slot_left"),
		}
		count = count + 1
	until not Result.next(resultId)
	Result.free(resultId)

	local elapsed = math.floor((os.clock() - startTime) * 1000)
	logger.info("[BotEquipmentData] Loaded {} equipment loadouts ({}ms)", count, elapsed)
end

function BotEquipmentData.getLoadout(level, baseVoc)
	level = math.min(level, 500)
	local byVoc = BotEquipmentData.loadouts[level]
	if byVoc then return byVoc[baseVoc] end
	return nil
end
