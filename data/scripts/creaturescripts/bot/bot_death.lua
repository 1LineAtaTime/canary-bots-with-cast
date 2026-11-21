-- Bot death handler: pauses C++ BotEngine AI when bot dies, re-activates after 3s.
-- Registered per-bot during startup (registered once globally, fires for all bots).
-- Fires synchronously before Player::death() restores HP.

local botDeath = CreatureEvent("BotDeath")

-- The real (non-bot) player behind a killer: the creature itself, or the master of a summon.
local function realPlayerOf(creature)
	if not creature then
		return nil
	end
	local master = creature:getMaster()
	if master then
		creature = master
	end
	if not creature:isPlayer() then
		return nil
	end
	local p = Player(creature:getId())
	if p and not p:isBotPlayer() then
		return p
	end
	return nil
end

local function describeKiller(creature)
	if not creature then
		return "none"
	end
	if creature:isPlayer() then
		local p = Player(creature:getId())
		return (p and p:isBotPlayer() and "bot:" or "player:") .. creature:getName()
	end
	return "creature:" .. creature:getName()
end

-- #107: a bot killed by a REAL player drops a small level-scaled bag. The loot table lives in
-- data/scripts/lib/bot_death_drop.lua. Bots keep no inventory, so the bag is generated here, in
-- the corpse, or on the death tile when there is no corpse (a PvP-zone death drops none).
local function dropDeathBag(player, corpse, killer, mostDamageKiller)
	local drop = BotDeathDrop
	if not drop or not drop.enabled then
		return
	end
	local victimName = player:getName()
	local killerPlayer = realPlayerOf(killer) or realPlayerOf(mostDamageKiller)
	if not killerPlayer then
		logger.info(string.format("[BOT_DEATH_DROP] skip victim=%s killer=%s reason=not_real_player", victimName, describeKiller(killer)))
		return
	end
	local level = player:getLevel()
	local rolled = drop.roll(level)
	if #rolled == 0 then
		logger.info(string.format("[BOT_DEATH_DROP] killer=%s victim=%s level=%d items=none", killerPlayer:getName(), victimName, level))
		return
	end
	local bag
	local where = "corpse"
	if corpse and corpse:isContainer() then
		bag = corpse:addItem(drop.bagId, 1)
	end
	if not bag then
		where = "tile"
		bag = Game.createItem(drop.bagId, 1, player:getPosition())
	end
	if not bag then
		logger.warn(string.format("[BOT_DEATH_DROP] killer=%s victim=%s failed=no_bag", killerPlayer:getName(), victimName))
		return
	end
	local parts = {}
	for _, it in ipairs(rolled) do
		if bag:addItem(it[1], it[2]) then
			parts[#parts + 1] = string.format("%dx%d", it[1], it[2])
		end
	end
	logger.info(string.format("[BOT_DEATH_DROP] killer=%s victim=%s level=%d where=%s items=%s", killerPlayer:getName(), victimName, level, where, table.concat(parts, ",")))
end

function botDeath.onDeath(player, corpse, killer, mostDamageKiller, unjustified, mostDamageUnjustified)
	if not player:isBotPlayer() then return true end

	local guid = player:getGuid()
	local botName = player:getName()

	local ok, err = pcall(dropDeathBag, player, corpse, killer, mostDamageKiller)
	if not ok then
		logger.error("[BOT_DEATH_DROP] error: " .. tostring(err))
	end

	-- Pause bot AI (preserves hunt/travel state, resumes after 10-180s random delay)
	-- Player::death() will handle teleport to temple and HP/mana restore
	Game.botPauseForDeath(guid)

	logger.info("[Bot] " .. botName .. " died — AI paused, will resume in 10-180s at temple")

	return true
end

botDeath:register()
