-- /house command (available to ALL players): take over a BOT-owned house,
-- add yourself as a native sub-owner, or release a house you own back to the
-- market. Backed by data/scripts/lib/bot_house_claim.lua (snapshot + auto-restore).

local function houseUsage(player)
	player:sendTextMessage(MESSAGE_STATUS, 'Usage: /house "<Exact Name>" owner | sub-owner | release')
end

local function findHouseByExactName(name)
	local lname = name:lower()
	for _, house in pairs(Game.getHouses()) do
		if house:getName():lower() == lname then
			return house
		end
	end
	return nil
end

local houseCmd = TalkAction("/house")

function houseCmd.onSay(player, words, param)
	local name, mode = (param or ""):match('^%s*"([^"]+)"%s+([%w%-]+)%s*$')
	if not name or not mode then
		houseUsage(player)
		return true
	end
	mode = mode:lower()
	if mode ~= "owner" and mode ~= "sub-owner" and mode ~= "release" then
		houseUsage(player)
		return true
	end

	local house = findHouseByExactName(name)
	if not house then
		player:sendCancelMessage(string.format('No house named exactly "%s".', name))
		return true
	end

	local ownerGuid = house:getOwnerGuid()

	-- release: hand a house YOU own back to the market (owner=0) so the
	-- BotHouseReclaim globalevent can return it to its original bot.
	if mode == "release" then
		if ownerGuid ~= player:getGuid() then
			player:sendCancelMessage("You can only release a house you currently own.")
			return true
		end
		house:setHouseOwner(0)
		player:sendTextMessage(MESSAGE_EVENT_ADVANCE,
			string.format('You have released "%s". It is now back on the market.', house:getName()))
		return true
	end

	-- owner / sub-owner: only BOT-owned houses are claimable here. This also
	-- blocks guildhalls and real-player houses (neither is bot-owned).
	if not BotHouseClaim.isBotGuid(ownerGuid) then
		player:sendCancelMessage("That house is not owned by a bot, so it cannot be claimed here.")
		return true
	end

	if mode == "sub-owner" then
		local list = house:getAccessList(SUBOWNER_LIST) or ""
		local me = player:getName()
		for line in (list .. "\n"):gmatch("([^\n]*)\n") do
			if line:match("^%s*(.-)%s*$"):lower() == me:lower() then
				player:sendTextMessage(MESSAGE_STATUS, string.format('You are already a sub-owner of "%s".', house:getName()))
				return true
			end
		end
		local newList = (list:match("^%s*$")) and me or (list .. "\n" .. me)
		house:setAccessList(SUBOWNER_LIST, newList)
		player:sendTextMessage(MESSAGE_STATUS, string.format('You are now a sub-owner of "%s".', house:getName()))
		return true
	end

	-- mode == "owner"
	if player:getHouse() then
		player:sendCancelMessage("You already own a house. Leave it before claiming another.")
		return true
	end
	local rent = house:getRent() or 0
	if player:getBankBalance() < rent then
		player:sendCancelMessage(string.format("You need at least %d gold in your bank to afford this house's rent.", rent))
		return true
	end

	-- Snapshot the bot's ORIGINAL layout once (permanent) before the transfer,
	-- so it can be restored verbatim if the house later falls vacant.
	BotHouseClaim.snapshotIfAbsent(house, ownerGuid)
	house:setHouseOwner(player:getGuid())
	player:sendTextMessage(MESSAGE_EVENT_ADVANCE,
		string.format('You are now the owner of "%s". The previous furnishings have been cleared.', house:getName()))
	return true
end

houseCmd:separator(" ")
houseCmd:groupType("normal")
houseCmd:register()
