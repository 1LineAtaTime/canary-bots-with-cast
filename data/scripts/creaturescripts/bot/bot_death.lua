-- Bot death handler: pauses C++ BotEngine AI when bot dies, re-activates after 3s.
-- Registered per-bot during startup (registered once globally, fires for all bots).
-- Fires synchronously before Player::death() restores HP.

local botDeath = CreatureEvent("BotDeath")

function botDeath.onDeath(player, corpse, killer, mostDamageKiller, unjustified, mostDamageUnjustified)
	if not player:isBotPlayer() then return true end

	local guid = player:getGuid()
	local botName = player:getName()

	-- Pause bot AI (preserves hunt/travel state, resumes after 10-180s random delay)
	-- Player::death() will handle teleport to temple and HP/mana restore
	Game.botPauseForDeath(guid)

	logger.info("[Bot] " .. botName .. " died — AI paused, will resume in 10-180s at temple")

	return true
end

botDeath:register()
