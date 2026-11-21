-- #107 Bot death drop: the loot table for a bot killed by a REAL player.
--
-- Bots persist no inventory, so the bag is generated at death by
-- data/scripts/creaturescripts/bot/bot_death.lua (onDeath fires before Player::death() sends the
-- bot back to its temple). It goes into the corpse, or onto the death tile when there is no
-- corpse (a PvP-zone death drops none).
--
-- Real-player killers only: a bot killed by a monster, a field or another bot drops nothing.
-- No cooldown and no cap; the skull system already prices repeated kills.
--
-- Tunable without a rebuild: edit this file, then restart (or reload scripts).
--   chance   : out of 100000, like creature loot (100000 = always)
--   min/max  : base count range
--   perLevel : extra count per victim level (floor(level * perLevel)), added to the roll
--   maxCount : hard cap for the stack (never above 100)
--   minLevel / maxLevel : victim level gate (0 = no gate), keeps the supplies level-coherent
BotDeathDrop = {
	enabled = true,
	bagId = 2853, -- bag
	maxStacks = 6, -- keep it a SMALL bag: at most this many stacks besides the coins
	coins = { id = 3035, chance = 100000, min = 1, max = 5, perLevel = 0.02, maxCount = 40 }, -- platinum coin
	items = {
		{ id = 266, chance = 30000, min = 1, max = 5, maxLevel = 60 }, -- health potion
		{ id = 268, chance = 30000, min = 1, max = 5, maxLevel = 80 }, -- mana potion
		{ id = 236, chance = 20000, min = 1, max = 4, minLevel = 50, maxLevel = 200 }, -- strong health potion
		{ id = 237, chance = 25000, min = 1, max = 5, minLevel = 50, maxLevel = 200 }, -- strong mana potion
		{ id = 239, chance = 15000, min = 1, max = 3, minLevel = 80 }, -- great health potion
		{ id = 238, chance = 20000, min = 1, max = 5, minLevel = 80 }, -- great mana potion
		{ id = 7642, chance = 15000, min = 1, max = 3, minLevel = 80 }, -- great spirit potion
		{ id = 7643, chance = 10000, min = 1, max = 3, minLevel = 130 }, -- ultimate health potion
		{ id = 23373, chance = 12000, min = 1, max = 4, minLevel = 130 }, -- ultimate mana potion
		{ id = 23375, chance = 6000, min = 1, max = 2, minLevel = 200 }, -- supreme health potion
		{ id = 3160, chance = 10000, min = 1, max = 3, minLevel = 24 }, -- ultimate healing rune
		{ id = 3155, chance = 10000, min = 1, max = 5, minLevel = 45 }, -- sudden death rune
		{ id = 3191, chance = 8000, min = 1, max = 5, minLevel = 30 }, -- great fireball rune
		{ id = 3161, chance = 8000, min = 1, max = 5, minLevel = 30 }, -- avalanche rune
		{ id = 3725, chance = 20000, min = 1, max = 5 }, -- brown mushroom
		{ id = 3582, chance = 10000, min = 1, max = 3 }, -- ham
		{ id = 3003, chance = 5000, min = 1, max = 1 }, -- rope
		{ id = 3457, chance = 5000, min = 1, max = 1 }, -- shovel
	},
}

local function rollCount(entry, level)
	local count = math.random(entry.min or 1, math.max(entry.min or 1, entry.max or 1))
	if entry.perLevel then
		count = count + math.floor(level * entry.perLevel)
	end
	return math.max(1, math.min(count, entry.maxCount or 100, 100))
end

local function levelOk(entry, level)
	if entry.minLevel and entry.minLevel > 0 and level < entry.minLevel then
		return false
	end
	if entry.maxLevel and entry.maxLevel > 0 and level > entry.maxLevel then
		return false
	end
	return true
end

-- Rolls the table for a victim of `level`. Returns { {id, count}, ... } (coins first).
function BotDeathDrop.roll(level)
	local out = {}
	local c = BotDeathDrop.coins
	if c and math.random(1, 100000) <= c.chance then
		out[#out + 1] = { c.id, rollCount(c, level) }
	end
	local stacks = 0
	for _, entry in ipairs(BotDeathDrop.items) do
		if stacks >= BotDeathDrop.maxStacks then
			break
		end
		if levelOk(entry, level) and math.random(1, 100000) <= entry.chance then
			out[#out + 1] = { entry.id, rollCount(entry, level) }
			stacks = stacks + 1
		end
	end
	return out
end
