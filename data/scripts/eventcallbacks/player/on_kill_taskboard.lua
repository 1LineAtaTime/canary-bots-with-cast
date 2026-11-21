local callback = EventCallback("TaskboardPlayerOnKill")

-- Fork guard (P5, #160): monster types with no bestiary race (raceId 0) make
-- Player:isMonsterBestiaryUnlocked() report "Monster race id not exists" on every kill.
-- Logged once per monster type instead of once per kill.
local noRaceLogged = {}

function callback.playerOnKill(player, monster)
	local taskboard = rawget(_G, "Taskboard")
	if not taskboard or not taskboard.isEnabled() or not player or not monster then
		return
	end
	-- Bots never use the task board (fork).
	if type(player.isBotPlayer) == "function" and player:isBotPlayer() then
		return
	end

	local monsterType = monster:getType()
	if not monsterType then
		return
	end

	local raceId = monsterType:raceId()
	if not raceId or raceId == 0 then
		local typeName = monsterType:name()
		if not noRaceLogged[typeName] then
			noRaceLogged[typeName] = true
			logger.info("[Taskboard] kills of '{}' are not counted: the monster type has no bestiary race id (logged once per type)", typeName)
		end
		return
	end

	local isRewardBoss = false
	if type(monsterType.isRewardBoss) == "function" then
		local ok, result = pcall(monsterType.isRewardBoss, monsterType)
		isRewardBoss = ok and result == true
	end
	if isRewardBoss then
		return
	end

	taskboard.onMonsterKilled(player, raceId, monster)
end

callback:register()
