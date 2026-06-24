/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "lua/functions/core/game/game_functions.hpp"

#include "core.hpp"
#include "creatures/monsters/monster.hpp"
#include "creatures/monsters/monsters.hpp"
#include "creatures/npcs/npc.hpp"
#include "creatures/players/player.hpp"
#include "game/functions/game_reload.hpp"
#include "game/game.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "utils/tools.hpp"  // JITTER DIAGNOSTIC: OTSYS_TIME for luaGameMonotonicMs
#include "io/io_bosstiary.hpp"
#include "io/iobestiary.hpp"
#include "io/iologindata.hpp"
#include "items/item.hpp"
#include "lua/callbacks/events_callbacks.hpp"
#include "lua/creature/events.hpp"
#include "lua/creature/talkaction.hpp"
#include "lua/functions/creatures/npc/npc_type_functions.hpp"
#include "lua/functions/events/event_callback_functions.hpp"
#include "lua/scripts/lua_environment.hpp"
#include "map/spectators.hpp"
#include "lua/functions/lua_functions_loader.hpp"
#include "creatures/players/bot/bot_engine.hpp"
#include "creatures/players/bot/bot_engine_loader.hpp"

void GameFunctions::init(lua_State* L) {
	Lua::registerTable(L, "Game");

	Lua::registerMethod(L, "Game", "createNpcType", GameFunctions::luaGameCreateNpcType);
	Lua::registerMethod(L, "Game", "createMonsterType", GameFunctions::luaGameCreateMonsterType);
	Lua::registerMethod(L, "Game", "getMonsterTypeByName", GameFunctions::luaGameGetMonsterTypeByName);

	Lua::registerMethod(L, "Game", "getSpectators", GameFunctions::luaGameGetSpectators);

	Lua::registerMethod(L, "Game", "getBoostedCreature", GameFunctions::luaGameGetBoostedCreature);
	Lua::registerMethod(L, "Game", "getBestiaryList", GameFunctions::luaGameGetBestiaryList);

	Lua::registerMethod(L, "Game", "getPlayers", GameFunctions::luaGameGetPlayers);
	Lua::registerMethod(L, "Game", "loadMap", GameFunctions::luaGameLoadMap);
	Lua::registerMethod(L, "Game", "loadMapChunk", GameFunctions::luaGameloadMapChunk);

	Lua::registerMethod(L, "Game", "getExperienceForLevel", GameFunctions::luaGameGetExperienceForLevel);
	Lua::registerMethod(L, "Game", "getMonsterCount", GameFunctions::luaGameGetMonsterCount);
	Lua::registerMethod(L, "Game", "getPlayerCount", GameFunctions::luaGameGetPlayerCount);
	Lua::registerMethod(L, "Game", "getNpcCount", GameFunctions::luaGameGetNpcCount);
	Lua::registerMethod(L, "Game", "getMonsterTypes", GameFunctions::luaGameGetMonsterTypes);

	Lua::registerMethod(L, "Game", "getTowns", GameFunctions::luaGameGetTowns);
	Lua::registerMethod(L, "Game", "getHouses", GameFunctions::luaGameGetHouses);

	Lua::registerMethod(L, "Game", "getGameState", GameFunctions::luaGameGetGameState);
	Lua::registerMethod(L, "Game", "setGameState", GameFunctions::luaGameSetGameState);

	Lua::registerMethod(L, "Game", "getWorldType", GameFunctions::luaGameGetWorldType);
	Lua::registerMethod(L, "Game", "setWorldType", GameFunctions::luaGameSetWorldType);

	Lua::registerMethod(L, "Game", "getReturnMessage", GameFunctions::luaGameGetReturnMessage);

	Lua::registerMethod(L, "Game", "createItem", GameFunctions::luaGameCreateItem);
	Lua::registerMethod(L, "Game", "createContainer", GameFunctions::luaGameCreateContainer);
	Lua::registerMethod(L, "Game", "createMonster", GameFunctions::luaGameCreateMonster);
	Lua::registerMethod(L, "Game", "createSoulPitMonster", GameFunctions::luaGameCreateSoulPitMonster);
	Lua::registerMethod(L, "Game", "createNpc", GameFunctions::luaGameCreateNpc);
	Lua::registerMethod(L, "Game", "generateNpc", GameFunctions::luaGameGenerateNpc);
	Lua::registerMethod(L, "Game", "createTile", GameFunctions::luaGameCreateTile);
	Lua::registerMethod(L, "Game", "createBestiaryCharm", GameFunctions::luaGameCreateBestiaryCharm);

	Lua::registerMethod(L, "Game", "createItemClassification", GameFunctions::luaGameCreateItemClassification);

	Lua::registerMethod(L, "Game", "getBestiaryCharm", GameFunctions::luaGameGetBestiaryCharm);

	Lua::registerMethod(L, "Game", "startRaid", GameFunctions::luaGameStartRaid);

	Lua::registerMethod(L, "Game", "getClientVersion", GameFunctions::luaGameGetClientVersion);

	Lua::registerMethod(L, "Game", "reload", GameFunctions::luaGameReload);

	// JITTER DIAGNOSTIC: Lua-accessible monotonic ms timestamp for instrumentation.
	// Used by jitter_heartbeat.lua + bot_hibernation.lua loop-body timing.
	Lua::registerMethod(L, "Game", "monotonicMs", GameFunctions::luaGameMonotonicMs);

	Lua::registerMethod(L, "Game", "hasDistanceEffect", GameFunctions::luaGameHasDistanceEffect);
	Lua::registerMethod(L, "Game", "hasEffect", GameFunctions::luaGameHasEffect);
	Lua::registerMethod(L, "Game", "getOfflinePlayer", GameFunctions::luaGameGetOfflinePlayer);
	Lua::registerMethod(L, "Game", "loadBotPlayer", GameFunctions::luaGameLoadBotPlayer);
	Lua::registerMethod(L, "Game", "botActivate", GameFunctions::luaGameBotActivate);
	Lua::registerMethod(L, "Game", "botDeactivate", GameFunctions::luaGameBotDeactivate);
	Lua::registerMethod(L, "Game", "botForceDeactivate", GameFunctions::luaGameBotForceDeactivate);
	Lua::registerMethod(L, "Game", "botForceDeactivateForReload", GameFunctions::luaGameBotForceDeactivateForReload);
	Lua::registerMethod(L, "Game", "botPauseForDeath", GameFunctions::luaGameBotPauseForDeath);
	Lua::registerMethod(L, "Game", "botSetAIPaused", GameFunctions::luaGameBotSetAIPaused);
	Lua::registerMethod(L, "Game", "botSetAllAIPaused", GameFunctions::luaGameBotSetAllAIPaused);
	Lua::registerMethod(L, "Game", "botHibernate", GameFunctions::luaGameBotHibernate);
	Lua::registerMethod(L, "Game", "botWake", GameFunctions::luaGameBotWake);
	Lua::registerMethod(L, "Game", "botRecoverOrphanForReload", GameFunctions::luaGameBotRecoverOrphanForReload);
	Lua::registerMethod(L, "Game", "getBotHibernationStates", GameFunctions::luaGameGetBotHibernationStates);
	Lua::registerMethod(L, "Game", "botHibernateAllEligible", GameFunctions::luaGameBotHibernateAllEligible);
	Lua::registerMethod(L, "Game", "botWakeAllHibernated", GameFunctions::luaGameBotWakeAllHibernated);
	Lua::registerMethod(L, "Game", "botCreateMarketOffer", GameFunctions::luaGameBotCreateMarketOffer);
	Lua::registerMethod(L, "Game", "botAcceptMarketOffer", GameFunctions::luaGameBotAcceptMarketOffer);
	Lua::registerMethod(L, "Game", "botCancelMarketOffer", GameFunctions::luaGameBotCancelMarketOffer);
	Lua::registerMethod(L, "Game", "botReactivateForReload", GameFunctions::luaGameBotReactivateForReload);
	Lua::registerMethod(L, "Game", "botCountActive", GameFunctions::luaGameBotCountActive);
	Lua::registerMethod(L, "Game", "botCommand", GameFunctions::luaGameBotCommand);
	Lua::registerMethod(L, "Game", "botReload", GameFunctions::luaGameBotReload);
	Lua::registerMethod(L, "Game", "botReregisterAll", GameFunctions::luaGameBotReregisterAll);
	Lua::registerMethod(L, "Game", "botStartTickLoop", GameFunctions::luaGameBotStartTickLoop);
	Lua::registerMethod(L, "Game", "botGetState", GameFunctions::luaGameBotGetState);
	Lua::registerMethod(L, "Game", "botGetStatusText", GameFunctions::luaGameBotGetStatusText);
	Lua::registerMethod(L, "Game", "botInParty", GameFunctions::luaGameBotInParty);
	Lua::registerMethod(L, "Game", "botIsActive", GameFunctions::luaGameBotIsActive);
	Lua::registerMethod(L, "Game", "botSaveStates", GameFunctions::luaGameBotSaveStates);
	Lua::registerMethod(L, "Game", "botRestoreStates", GameFunctions::luaGameBotRestoreStates);
	Lua::registerMethod(L, "Game", "botClearPersistedStates", GameFunctions::luaGameBotClearPersistedStates);
	Lua::registerMethod(L, "Game", "getNormalizedPlayerName", GameFunctions::luaGameGetNormalizedPlayerName);
	Lua::registerMethod(L, "Game", "getNormalizedGuildName", GameFunctions::luaGameGetNormalizedGuildName);

	Lua::registerMethod(L, "Game", "addInfluencedMonster", GameFunctions::luaGameAddInfluencedMonster);
	Lua::registerMethod(L, "Game", "removeInfluencedMonster", GameFunctions::luaGameRemoveInfluencedMonster);
	Lua::registerMethod(L, "Game", "getInfluencedMonsters", GameFunctions::luaGameGetInfluencedMonsters);
	Lua::registerMethod(L, "Game", "makeFiendishMonster", GameFunctions::luaGameMakeFiendishMonster);
	Lua::registerMethod(L, "Game", "removeFiendishMonster", GameFunctions::luaGameRemoveFiendishMonster);
	Lua::registerMethod(L, "Game", "getFiendishMonsters", GameFunctions::luaGameGetFiendishMonsters);
	Lua::registerMethod(L, "Game", "getBoostedBoss", GameFunctions::luaGameGetBoostedBoss);

	Lua::registerMethod(L, "Game", "getLadderIds", GameFunctions::luaGameGetLadderIds);
	Lua::registerMethod(L, "Game", "isDebugBuild", GameFunctions::luaGameIsDebugBuild);
	Lua::registerMethod(L, "Game", "getDummies", GameFunctions::luaGameGetDummies);

	Lua::registerMethod(L, "Game", "getTalkActions", GameFunctions::luaGameGetTalkActions);
	Lua::registerMethod(L, "Game", "getEventCallbacks", GameFunctions::luaGameGetEventCallbacks);

	Lua::registerMethod(L, "Game", "registerAchievement", GameFunctions::luaGameRegisterAchievement);
	Lua::registerMethod(L, "Game", "getAchievementInfoById", GameFunctions::luaGameGetAchievementInfoById);
	Lua::registerMethod(L, "Game", "getAchievementInfoByName", GameFunctions::luaGameGetAchievementInfoByName);
	Lua::registerMethod(L, "Game", "getSecretAchievements", GameFunctions::luaGameGetSecretAchievements);
	Lua::registerMethod(L, "Game", "getPublicAchievements", GameFunctions::luaGameGetPublicAchievements);
	Lua::registerMethod(L, "Game", "getAchievements", GameFunctions::luaGameGetAchievements);

	Lua::registerMethod(L, "Game", "getSoulCoreItems", GameFunctions::luaGameGetSoulCoreItems);

	Lua::registerMethod(L, "Game", "getMonstersByRace", GameFunctions::luaGameGetMonstersByRace);
	Lua::registerMethod(L, "Game", "getMonstersByBestiaryStars", GameFunctions::luaGameGetMonstersByBestiaryStars);
}

// Game
int GameFunctions::luaGameCreateMonsterType(lua_State* L) {
	// Game.createMonsterType(name[, variant = ""[, alternateName = ""]])
	if (Lua::isString(L, 1)) {
		const auto name = Lua::getString(L, 1);
		std::string uniqueName = name;
		auto variant = Lua::getString(L, 2, "");
		const auto alternateName = Lua::getString(L, 3, "");
		std::set<std::string> names;
		const auto monsterType = std::make_shared<MonsterType>(name);
		if (!monsterType) {
			lua_pushstring(L, "MonsterType is nullptr");
			lua_error(L);
			return 1;
		}

		// if variant starts with !, then it's the only variant for this monster, so we register it with both names
		if (variant.starts_with("!")) {
			names.insert(name);
			variant = variant.substr(1);
		}
		if (!variant.empty()) {
			uniqueName = variant + "|" + name;
		}
		names.insert(uniqueName);

		monsterType->name = name;
		if (!alternateName.empty()) {
			names.insert(alternateName);
			monsterType->name = alternateName;
		}

		monsterType->variantName = variant;
		monsterType->nameDescription = "a " + name;

		for (const auto &alternateName : names) {
			if (!g_monsters().tryAddMonsterType(alternateName, monsterType)) {
				lua_pushstring(L, fmt::format("The monster with name {} already registered", alternateName).c_str());
				lua_error(L);
				return 1;
			}
		}

		Lua::pushUserdata<MonsterType>(L, monsterType);
		Lua::setMetatable(L, -1, "MonsterType");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameCreateNpcType(lua_State* L) {
	return NpcTypeFunctions::luaNpcTypeCreate(L);
}

int GameFunctions::luaGameGetMonsterTypeByName(lua_State* L) {
	if (!Lua::isString(L, 1)) {
		Lua::reportErrorFunc("First argument must be a string");
		Lua::pushBoolean(L, false);
		return 1;
	}

	const auto name = Lua::getString(L, 1);
	const auto &mType = g_monsters().getMonsterType(name);
	if (!mType) {
		Lua::reportErrorFunc(fmt::format("MonsterType with name {} not found", name));
		Lua::pushBoolean(L, false);
		return 1;
	}

	Lua::pushUserdata<MonsterType>(L, mType);
	Lua::setMetatable(L, -1, "MonsterType");
	return 1;
}

int GameFunctions::luaGameGetSpectators(lua_State* L) {
	// Game.getSpectators(position[, multifloor = false[, onlyPlayer = false[, minRangeX = 0[, maxRangeX = 0[, minRangeY = 0[, maxRangeY = 0]]]]]])
	const Position &position = Lua::getPosition(L, 1);
	const bool multifloor = Lua::getBoolean(L, 2, false);
	const bool onlyPlayers = Lua::getBoolean(L, 3, false);
	const auto minRangeX = Lua::getNumber<int32_t>(L, 4, 0);
	const auto maxRangeX = Lua::getNumber<int32_t>(L, 5, 0);
	const auto minRangeY = Lua::getNumber<int32_t>(L, 6, 0);
	const auto maxRangeY = Lua::getNumber<int32_t>(L, 7, 0);

	Spectators spectators;

	if (onlyPlayers) {
		spectators.find<Player>(position, multifloor, minRangeX, maxRangeX, minRangeY, maxRangeY);
	} else {
		spectators.find<Creature>(position, multifloor, minRangeX, maxRangeX, minRangeY, maxRangeY);
	}

	lua_createtable(L, spectators.size(), 0);

	int index = 0;
	for (const auto &creature : spectators) {
		Lua::pushUserdata<Creature>(L, creature);
		Lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetBoostedCreature(lua_State* L) {
	// Game.getBoostedCreature()
	Lua::pushString(L, g_game().getBoostedMonsterName());
	return 1;
}

int GameFunctions::luaGameGetBestiaryList(lua_State* L) {
	// Game.getBestiaryList([bool[string or BestiaryType_t]])
	lua_newtable(L);
	int index = 0;
	const bool name = Lua::getBoolean(L, 2, false);

	if (lua_gettop(L) <= 2) {
		const std::map<uint16_t, std::string> &mtype_list = g_game().getBestiaryList();
		for (const auto &ita : mtype_list) {
			if (name) {
				Lua::pushString(L, ita.second);
			} else {
				lua_pushnumber(L, ita.first);
			}
			lua_rawseti(L, -2, ++index);
		}
	} else {
		if (Lua::isNumber(L, 2)) {
			const std::map<uint16_t, std::string> tmplist = g_iobestiary().findRaceByName("CANARY", false, Lua::getNumber<BestiaryType_t>(L, 2));
			for (const auto &itb : tmplist) {
				if (name) {
					Lua::pushString(L, itb.second);
				} else {
					lua_pushnumber(L, itb.first);
				}
				lua_rawseti(L, -2, ++index);
			}
		} else {
			const std::map<uint16_t, std::string> tmplist = g_iobestiary().findRaceByName(Lua::getString(L, 2));
			for (const auto &itc : tmplist) {
				if (name) {
					Lua::pushString(L, itc.second);
				} else {
					lua_pushnumber(L, itc.first);
				}
				lua_rawseti(L, -2, ++index);
			}
		}
	}
	return 1;
}

int GameFunctions::luaGameGetPlayers(lua_State* L) {
	// Game.getPlayers()
	lua_createtable(L, g_game().getPlayersOnline(), 0);

	int index = 0;
	for (const auto &playerEntry : g_game().getPlayers()) {
		Lua::pushUserdata<Player>(L, playerEntry.second);
		Lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameLoadMap(lua_State* L) {
	// Game.loadMap(path)
	const std::string &path = Lua::getString(L, 1);
	g_dispatcher().addEvent([path]() { g_game().loadMap(path); }, __FUNCTION__);
	return 0;
}

int GameFunctions::luaGameloadMapChunk(lua_State* L) {
	// Game.loadMapChunk(path, position, remove)
	const std::string &path = Lua::getString(L, 1);
	const Position &position = Lua::getPosition(L, 2);
	g_dispatcher().addEvent([path, position]() { g_game().loadMap(path, position); }, __FUNCTION__);
	return 0;
}

int GameFunctions::luaGameGetExperienceForLevel(lua_State* L) {
	// Game.getExperienceForLevel(level)
	const uint32_t level = Lua::getNumber<uint32_t>(L, 1);
	if (level == 0) {
		Lua::reportErrorFunc("Level must be greater than 0.");
	} else {
		lua_pushnumber(L, Player::getExpForLevel(level));
	}
	return 1;
}

int GameFunctions::luaGameGetMonsterCount(lua_State* L) {
	// Game.getMonsterCount()
	lua_pushnumber(L, g_game().getMonstersOnline());
	return 1;
}

int GameFunctions::luaGameGetPlayerCount(lua_State* L) {
	// Game.getPlayerCount()
	lua_pushnumber(L, g_game().getPlayersOnline());
	return 1;
}

int GameFunctions::luaGameGetNpcCount(lua_State* L) {
	// Game.getNpcCount()
	lua_pushnumber(L, g_game().getNpcsOnline());
	return 1;
}

int GameFunctions::luaGameGetMonsterTypes(lua_State* L) {
	// Game.getMonsterTypes()
	const auto type = g_monsters().monsters;
	lua_createtable(L, type.size(), 0);

	for (const auto &[typeName, mType] : type) {
		Lua::pushUserdata<MonsterType>(L, mType);
		Lua::setMetatable(L, -1, "MonsterType");
		lua_setfield(L, -2, typeName.c_str());
	}
	return 1;
}

int GameFunctions::luaGameGetTowns(lua_State* L) {
	// Game.getTowns()
	const auto towns = g_game().map.towns.getTowns();
	lua_createtable(L, towns.size(), 0);

	int index = 0;
	for (const auto &townEntry : towns) {
		Lua::pushUserdata<Town>(L, townEntry.second);
		Lua::setMetatable(L, -1, "Town");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetHouses(lua_State* L) {
	// Game.getHouses()
	const auto houses = g_game().map.houses.getHouses();
	lua_createtable(L, houses.size(), 0);

	int index = 0;
	for (const auto &houseEntry : houses) {
		Lua::pushUserdata<House>(L, houseEntry.second);
		Lua::setMetatable(L, -1, "House");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetGameState(lua_State* L) {
	// Game.getGameState()
	lua_pushnumber(L, g_game().getGameState());
	return 1;
}

int GameFunctions::luaGameSetGameState(lua_State* L) {
	// Game.setGameState(state)
	const GameState_t state = Lua::getNumber<GameState_t>(L, 1);
	g_game().setGameState(state);
	Lua::pushBoolean(L, true);
	return 1;
}

int GameFunctions::luaGameGetWorldType(lua_State* L) {
	// Game.getWorldType()
	lua_pushnumber(L, g_game().getWorldType());
	return 1;
}

int GameFunctions::luaGameSetWorldType(lua_State* L) {
	// Game.setWorldType(type)
	const WorldType_t type = Lua::getNumber<WorldType_t>(L, 1);
	g_game().setWorldType(type);
	Lua::pushBoolean(L, true);
	return 1;
}

int GameFunctions::luaGameGetReturnMessage(lua_State* L) {
	// Game.getReturnMessage(value)
	const ReturnValue value = Lua::getNumber<ReturnValue>(L, 1);
	Lua::pushString(L, getReturnMessage(value));
	return 1;
}

int GameFunctions::luaGameCreateItem(lua_State* L) {
	// Game.createItem(itemId or name[, count[, position]])
	uint16_t itemId;
	if (Lua::isNumber(L, 1)) {
		itemId = Lua::getNumber<uint16_t>(L, 1);
	} else {
		itemId = Item::items.getItemIdByName(Lua::getString(L, 1));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const auto count = Lua::getNumber<int32_t>(L, 2, 1);
	int32_t itemCount = 1;
	int32_t subType = 1;

	const ItemType &it = Item::items[itemId];
	if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = std::ceil(count / static_cast<float_t>(it.stackSize));
		}

		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	Position position;
	if (lua_gettop(L) >= 3) {
		position = Lua::getPosition(L, 3);
	}

	const bool hasTable = itemCount > 1;
	if (hasTable) {
		lua_newtable(L);
	} else if (itemCount == 0) {
		lua_pushnil(L);
		return 1;
	}

	for (int32_t i = 1; i <= itemCount; ++i) {
		int32_t stackCount = subType;
		if (it.stackable) {
			stackCount = std::min<int32_t>(stackCount, it.stackSize);
			subType -= stackCount;
		}

		const auto &item = Item::CreateItem(itemId, stackCount);
		if (!item) {
			if (!hasTable) {
				lua_pushnil(L);
			}
			return 1;
		}

		if (position.x != 0) {
			const auto &tile = g_game().map.getTile(position);
			if (!tile) {
				if (!hasTable) {
					lua_pushnil(L);
				}
				return 1;
			}

			ReturnValue ret = g_game().internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
			if (ret != RETURNVALUE_NOERROR) {
				if (!hasTable) {
					lua_pushnil(L);
				}
				return 1;
			}
		} else {
			Lua::getScriptEnv()->addTempItem(item);
			item->setParent(VirtualCylinder::virtualCylinder);
		}

		if (hasTable) {
			lua_pushnumber(L, i);
			Lua::pushUserdata<Item>(L, item);
			Lua::setItemMetatable(L, -1, item);
			lua_settable(L, -3);
		} else {
			Lua::pushUserdata<Item>(L, item);
			Lua::setItemMetatable(L, -1, item);
		}
	}

	return 1;
}

int GameFunctions::luaGameCreateContainer(lua_State* L) {
	// Game.createContainer(itemId, size[, position])
	const uint16_t size = Lua::getNumber<uint16_t>(L, 2);
	uint16_t id;
	if (Lua::isNumber(L, 1)) {
		id = Lua::getNumber<uint16_t>(L, 1);
	} else {
		id = Item::items.getItemIdByName(Lua::getString(L, 1));
		if (id == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const auto &container = Item::CreateItemAsContainer(id, size);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position &position = Lua::getPosition(L, 3);
		const auto &tile = g_game().map.getTile(position);
		if (!tile) {
			lua_pushnil(L);
			return 1;
		}

		g_game().internalAddItem(tile, container, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		Lua::getScriptEnv()->addTempItem(container);
		container->setParent(VirtualCylinder::virtualCylinder);
	}

	Lua::pushUserdata<Container>(L, container);
	Lua::setMetatable(L, -1, "Container");
	return 1;
}

int GameFunctions::luaGameCreateMonster(lua_State* L) {
	// Game.createMonster(monsterName, position[, extended = false[, force = false[, master = nil]]])
	const auto &monster = Monster::createMonster(Lua::getString(L, 1));
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	bool isSummon = false;
	if (lua_gettop(L) >= 5) {
		if (const auto &master = Lua::getCreature(L, 5)) {
			monster->setMaster(master, true);
			isSummon = true;
		}
	}

	const Position &position = Lua::getPosition(L, 2);
	const bool extended = Lua::getBoolean(L, 3, false);
	const bool force = Lua::getBoolean(L, 4, false);
	if (g_game().placeCreature(monster, position, extended, force)) {
		monster->onSpawn(position);
		const auto &mtype = monster->getMonsterType();
		if (mtype && mtype->info.raceid > 0 && mtype->info.bosstiaryRace == BosstiaryRarity_t::RARITY_ARCHFOE) {
			for (const auto &spectator : Spectators().find<Player>(monster->getPosition(), true)) {
				if (const auto &tmpPlayer = spectator->getPlayer()) {
					tmpPlayer->sendBosstiaryCooldownTimer();
				}
			}
		}

		Lua::pushUserdata<Monster>(L, monster);
		Lua::setMetatable(L, -1, "Monster");
	} else {
		if (isSummon) {
			monster->setMaster(nullptr);
		} else {
		}
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameCreateSoulPitMonster(lua_State* L) {
	// Game.createSoulPitMonster(monsterName, position, [stack = 1, [, extended = false[, force = false[, master = nil]]]])
	const auto &monster = Monster::createMonster(Lua::getString(L, 1));
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	bool isSummon = false;
	if (lua_gettop(L) >= 6) {
		if (const auto &master = Lua::getCreature(L, 6)) {
			monster->setMaster(master, true);
			isSummon = true;
		}
	}

	const Position &position = Lua::getPosition(L, 2);
	const uint8_t stack = Lua::getNumber<uint8_t>(L, 3, 1);
	const bool extended = Lua::getBoolean(L, 4, false);
	const bool force = Lua::getBoolean(L, 5, false);
	if (g_game().placeCreature(monster, position, extended, force)) {
		monster->setSoulPitStack(stack);
		monster->onSpawn(position);

		Lua::pushUserdata<Monster>(L, monster);
		Lua::setMetatable(L, -1, "Monster");
	} else {
		if (isSummon) {
			monster->setMaster(nullptr);
		}
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameGenerateNpc(lua_State* L) {
	// Game.generateNpc(npcName)
	const auto &npc = Npc::createNpc(Lua::getString(L, 1));
	if (!npc) {
		lua_pushnil(L);
		return 1;
	} else {
		Lua::pushUserdata<Npc>(L, npc);
		Lua::setMetatable(L, -1, "Npc");
	}
	return 1;
}

int GameFunctions::luaGameCreateNpc(lua_State* L) {
	// Game.createNpc(npcName, position[, extended = false[, force = false]])
	const auto &npc = Npc::createNpc(Lua::getString(L, 1));
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	const Position &position = Lua::getPosition(L, 2);
	const bool extended = Lua::getBoolean(L, 3, false);
	const bool force = Lua::getBoolean(L, 4, false);
	if (g_game().placeCreature(npc, position, extended, force)) {
		Lua::pushUserdata<Npc>(L, npc);
		Lua::setMetatable(L, -1, "Npc");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameCreateTile(lua_State* L) {
	// Game.createTile(x, y, z[, isDynamic = false])
	// Game.createTile(position[, isDynamic = false])
	Position position;
	bool isDynamic;
	if (Lua::isTable(L, 1)) {
		position = Lua::getPosition(L, 1);
		isDynamic = Lua::getBoolean(L, 2, false);
	} else {
		position.x = Lua::getNumber<uint16_t>(L, 1);
		position.y = Lua::getNumber<uint16_t>(L, 2);
		position.z = Lua::getNumber<uint16_t>(L, 3);
		isDynamic = Lua::getBoolean(L, 4, false);
	}

	Lua::pushUserdata(L, g_game().map.getOrCreateTile(position, isDynamic));
	Lua::setMetatable(L, -1, "Tile");
	return 1;
}

int GameFunctions::luaGameGetBestiaryCharm(lua_State* L) {
	// Game.getBestiaryCharm()
	const auto c_list = g_game().getCharmList();
	lua_createtable(L, c_list.size(), 0);

	int index = 0;
	for (const auto &charmPtr : c_list) {
		Lua::pushUserdata<Charm>(L, charmPtr);
		Lua::setMetatable(L, -1, "Charm");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameCreateBestiaryCharm(lua_State* L) {
	// Game.createBestiaryCharm(id)
	if (const std::shared_ptr<Charm> &charm = g_iobestiary().getBestiaryCharm(static_cast<charmRune_t>(Lua::getNumber<int8_t>(L, 1, 0)), true)) {
		Lua::pushUserdata<Charm>(L, charm);
		Lua::setMetatable(L, -1, "Charm");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameCreateItemClassification(lua_State* L) {
	// Game.createItemClassification(id)
	const ItemClassification* itemClassification = g_game().getItemsClassification(Lua::getNumber<uint8_t>(L, 1), true);
	if (itemClassification) {
		Lua::pushUserdata<const ItemClassification>(L, itemClassification);
		Lua::setMetatable(L, -1, "ItemClassification");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameStartRaid(lua_State* L) {
	// Game.startRaid(raidName)
	const std::string &raidName = Lua::getString(L, 1);

	const auto &raid = g_game().raids.getRaidByName(raidName);
	if (!raid || !raid->isLoaded()) {
		lua_pushnumber(L, RETURNVALUE_NOSUCHRAIDEXISTS);
		return 1;
	}

	if (g_game().raids.getRunning()) {
		lua_pushnumber(L, RETURNVALUE_ANOTHERRAIDISALREADYEXECUTING);
		return 1;
	}

	g_game().raids.setRunning(raid);
	raid->startRaid();
	lua_pushnumber(L, RETURNVALUE_NOERROR);
	return 1;
}

int GameFunctions::luaGameGetClientVersion(lua_State* L) {
	// Game.getClientVersion()
	lua_createtable(L, 0, 3);
	Lua::setField(L, "min", CLIENT_VERSION);
	Lua::setField(L, "max", CLIENT_VERSION);
	const std::string version = fmt::format("{}.{}", CLIENT_VERSION_UPPER, CLIENT_VERSION_LOWER);
	Lua::setField(L, "string", version);
	return 1;
}

int GameFunctions::luaGameMonotonicMs(lua_State* L) {
	// JITTER DIAGNOSTIC: epoch ms for Lua-side wall-clock instrumentation
	// (jitter_heartbeat.lua, bot_hibernation.lua, bot_market.lua pass timing).
	// JITTER FIX 2026-06-10: pass useTime=true — the default OTSYS_TIME() returns a
	// value CACHED at the top of the dispatcher cycle, so durations measured inside
	// a single task always read 0. Cost ≈ one clock_gettime syscall (~30ns).
	lua_pushnumber(L, static_cast<lua_Number>(OTSYS_TIME(true)));
	return 1;
}

int GameFunctions::luaGameReload(lua_State* L) {
	// Game.reload(reloadType)
	const Reload_t reloadType = Lua::getNumber<Reload_t>(L, 1);
	if (GameReload::getReloadNumber(reloadType) == GameReload::getReloadNumber(Reload_t::RELOAD_TYPE_NONE)) {
		Lua::reportErrorFunc("Reload type is none");
		Lua::pushBoolean(L, false);
		return 0;
	}

	if (GameReload::getReloadNumber(reloadType) >= GameReload::getReloadNumber(Reload_t::RELOAD_TYPE_LAST)) {
		Lua::reportErrorFunc("Reload type not exist");
		Lua::pushBoolean(L, false);
		return 0;
	}

	Lua::pushBoolean(L, GameReload::init(reloadType));
	lua_gc(g_luaEnvironment().getLuaState(), LUA_GCCOLLECT, 0);
	return 1;
}

int GameFunctions::luaGameHasEffect(lua_State* L) {
	// Game.hasEffect(effectId)
	const uint16_t effectId = Lua::getNumber<uint16_t>(L, 1);
	Lua::pushBoolean(L, g_game().hasEffect(effectId));
	return 1;
}

int GameFunctions::luaGameHasDistanceEffect(lua_State* L) {
	// Game.hasDistanceEffect(effectId)
	const uint16_t effectId = Lua::getNumber<uint16_t>(L, 1);
	Lua::pushBoolean(L, g_game().hasDistanceEffect(effectId));
	return 1;
}

int GameFunctions::luaGameGetOfflinePlayer(lua_State* L) {
	// Game.getOfflinePlayer(name or id)
	std::shared_ptr<Player> player = nullptr;
	if (Lua::isNumber(L, 1)) {
		const uint32_t id = Lua::getNumber<uint32_t>(L, 1);
		if (id >= Player::getFirstID() && id <= Player::getLastID()) {
			player = g_game().getPlayerByID(id, true);
		} else {
			player = g_game().getPlayerByGUID(id, true);
		}
	} else if (Lua::isString(L, 1)) {
		const auto name = Lua::getString(L, 1);
		player = g_game().getPlayerByName(name, true);
	}
	if (!player) {
		lua_pushnil(L);
	} else {
		Lua::pushUserdata<Player>(L, player);
		Lua::setMetatable(L, -1, "Player");
	}

	return 1;
}

int GameFunctions::luaGameLoadBotPlayer(lua_State* L) {
	// Game.loadBotPlayer(name) — loads a player from DB, places in world with no client
	const std::string &name = Lua::getString(L, 1);
	g_logger().info("[Game.loadBotPlayer] Loading bot player '{}'...", name);

	// If already online, return existing
	auto existing = g_game().getPlayerByName(name);
	if (existing) {
		g_logger().info("[Game.loadBotPlayer] Bot '{}' already online, reusing", name);
		existing->setBotPlayer(true);
		Lua::pushUserdata<Player>(L, existing);
		Lua::setMetatable(L, -1, "Player");
		return 1;
	}

	// Create player with no client connection
	auto player = std::make_shared<Player>(nullptr);
	if (!IOLoginData::loadPlayerByName(player, name, false)) {
		g_logger().warn("[Game.loadBotPlayer] Failed to load bot '{}' from database", name);
		lua_pushnil(L);
		return 1;
	}

	g_logger().info("[Game.loadBotPlayer] Loaded '{}' from DB (level {}, vocation {})", name, player->getLevel(), player->getVocationId());

	player->setBotPlayer(true);
	player->setOnline(true);

	// Initialize base speed — loadPlayerByName doesn't call this, so bots would have speed=0
	player->initBotBaseSpeed();

	// Place at prior login position (normal re-login behavior).
	// Staging is only used as fallback for brand-new bots with no real prior position.
	static constexpr Position INACTIVE_POS { 31970, 32283, 7 };

	auto loginPos = player->getLoginPosition();
	bool hasRealLoginPos = loginPos.x > 0 && loginPos.y > 0 &&
	    !(loginPos.x == INACTIVE_POS.x && loginPos.y == INACTIVE_POS.y);

	bool placed = false;
	if (hasRealLoginPos) {
		placed = g_game().internalPlaceCreature(player, loginPos, false, true);
	}

	if (!placed) {
		// Fallback: staging tile (brand-new bot, or loginPos tile was blocked)
		if (!g_game().map.getTile(INACTIVE_POS)) {
			g_game().map.getOrCreateTile(INACTIVE_POS, true);
		}
		g_logger().info("[Game.loadBotPlayer] Using staging fallback for '{}'", name);
		placed = g_game().internalPlaceCreature(player, INACTIVE_POS, false, true);
	}

	if (!placed) {
		// Final fallback: town temple
		const auto &town = player->getTown();
		if (town) {
			auto templePos = town->getTemplePosition();
			placed = g_game().internalPlaceCreature(player, templePos, false, true);
		}
	}
	if (!placed) {
		g_logger().warn("[Game.loadBotPlayer] Failed to place bot '{}' in world — no valid position", name);
		player->setOnline(false);
		lua_pushnil(L);
		return 1;
	}

	// Enable chase mode so setAttackedCreature triggers setFollowCreature
	// (engine handles pathfinding with clearSight=true, routes around walls)
	player->setChaseMode(true);
	// Secure mode ON by default — prevents bots from attacking SKULL_NONE targets
	// Only disabled temporarily during Random PK and Vigilante combat in bot_engine.cpp
	player->setSecureMode(true);

	// Manually add to creature check list so onThink fires
	g_game().addCreatureCheck(player);

	// Register with C++ BotEngine for batch AI processing
	g_botEngine().registerBot(player);

	auto finalPos = player->getPosition();
	g_logger().info("[Game.loadBotPlayer] Bot '{}' placed successfully at ({}, {}, {})", name, finalPos.x, finalPos.y, finalPos.z);

	Lua::pushUserdata<Player>(L, player);
	Lua::setMetatable(L, -1, "Player");
	return 1;
}

int GameFunctions::luaGameBotActivate(lua_State* L) {
	// Game.botActivate(guid) — returns true if bot was actually activated (was inactive)
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	bool result = g_botEngine().activateBot(guid);
	lua_pushboolean(L, result);
	return 1;
}

int GameFunctions::luaGameBotDeactivate(lua_State* L) {
	// Game.botDeactivate(guid) — returns true if bot was actually deactivated
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	bool result = g_botEngine().deactivateBot(guid);
	lua_pushboolean(L, result);
	return 1;
}

int GameFunctions::luaGameBotForceDeactivate(lua_State* L) {
	// Game.botForceDeactivate(guid) — unconditional deactivation (for death handling)
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	g_botEngine().forceDeactivateBot(guid);
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotForceDeactivateForReload(lua_State* L) {
	// Game.botForceDeactivateForReload(guid) — deactivate without touching cast or position
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	g_botEngine().forceDeactivateBotForReload(guid);
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotPauseForDeath(lua_State* L) {
	// Game.botPauseForDeath(guid) — pause bot AI after death (10-180s), then resume
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	g_botEngine().pauseBotForDeath(guid);
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotSetAIPaused(lua_State* L) {
	// Game.botSetAIPaused(nameOrGuid, paused) — gate the AI tick for one bot (CPU benchmarking)
	uint32_t guid = 0;
	if (lua_isnumber(L, 1)) {
		guid = Lua::getNumber<uint32_t>(L, 1);
	} else {
		const std::string &name = Lua::getString(L, 1);
		auto player = g_game().getPlayerByName(name);
		if (!player) {
			lua_pushboolean(L, false);
			return 1;
		}
		guid = player->getGUID();
	}
	bool paused = Lua::getBoolean(L, 2);
	g_botEngine().setBotAIPaused(guid, paused);
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotSetAllAIPaused(lua_State* L) {
	// Game.botSetAllAIPaused(paused) — gate the AI tick for every active bot (CPU benchmarking)
	bool paused = Lua::getBoolean(L, 1);
	g_botEngine().setAllBotsAIPaused(paused);
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotHibernate(lua_State* L) {
	// Game.botHibernate(guid) — despawn bot but preserve AI state. Returns true if hibernated.
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	bool result = g_botEngine().hibernateBot(guid);
	lua_pushboolean(L, result);
	return 1;
}

int GameFunctions::luaGameBotWake(lua_State* L) {
	// Game.botWake(guid) — re-materialize Player from DB and resume AI from preserved state.
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	bool result = g_botEngine().wakeBot(guid);
	lua_pushboolean(L, result);
	return 1;
}

int GameFunctions::luaGameBotRecoverOrphanForReload(lua_State* L) {
	// Game.botRecoverOrphanForReload(guid, player) — re-attach a hibernated bot's
	// orphaned Player object (Lua-held shared_ptr) to the new engine after botReload
	// destroyed the old hibernationPool_. Used by executeReload to ensure all 200
	// bots survive /cavebot reload.
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	auto player = Lua::getUserdataShared<Player>(L, 2, "Player");
	if (!player) {
		Lua::reportErrorFunc("Player is nullptr");
		lua_pushboolean(L, false);
		return 1;
	}
	bool result = g_botEngine().recoverOrphanForReload(guid, player);
	lua_pushboolean(L, result);
	return 1;
}

int GameFunctions::luaGameGetBotHibernationStates(lua_State* L) {
	// Game.getBotHibernationStates() — returns array of {guid, name, hibernated, active, x, y, z}
	// for every registered bot. Used by bot_hibernation.lua proximity loop.
	lua_newtable(L);
	int idx = 1;
	uint32_t total = g_botEngine().countTotalBots();
	// Iterate via getBotState (which returns by guid). Simpler path: expose via a single bulk call.
	// We have no enumerate-all API — so use getHibernatedBotGuids() + g_game().getPlayers() for awake.
	// However that mixes two iteration paths. Cleaner: walk all known guids by combining
	// hibernated list + awake list (active bots have a Player in g_game()).
	// Helper: push a bool as a Lua boolean (Lua::setField only has number/string overloads,
	// which would convert bool to 0/1 — and 0 is TRUTHY in Lua, breaking conditional checks).
	auto setBoolField = [L](const char* key, bool value) {
		lua_pushboolean(L, value);
		lua_setfield(L, -2, key);
	};

	auto hibernated = g_botEngine().getHibernatedBotGuids();
	for (uint32_t guid : hibernated) {
		auto state = g_botEngine().getBotState(guid);
		if (!state) continue;
		lua_newtable(L);
		Lua::setField(L, "guid", state->guid);
		Lua::setField(L, "name", state->name);
		setBoolField("hibernated", true);
		setBoolField("active", state->active);
		Lua::setField(L, "x", state->currentPos.x);
		Lua::setField(L, "y", state->currentPos.y);
		Lua::setField(L, "z", state->currentPos.z);
		// PERF_INVESTIGATION_2026-05-24 Phase B: LRU sort key for the Lua proximity
		// loop. Updated unconditionally by shouldGateWake on every wake attempt.
		Lua::setField(L, "lastWakeAttemptMs", state->lastWakeAttemptMs);
		lua_rawseti(L, -2, idx++);
	}
	// Awake bots: iterate g_game().getPlayers() and filter by isBotPlayer()
	for (const auto &[id, p] : g_game().getPlayers()) {
		if (!p || !p->isBotPlayer()) continue;
		auto state = g_botEngine().getBotState(p->getGUID());
		if (!state) continue;
		if (state->hibernated) continue; // already in the hibernated pass above
		lua_newtable(L);
		Lua::setField(L, "guid", state->guid);
		Lua::setField(L, "name", state->name);
		setBoolField("hibernated", false);
		setBoolField("active", state->active);
		auto pos = p->getPosition();
		Lua::setField(L, "x", pos.x);
		Lua::setField(L, "y", pos.y);
		Lua::setField(L, "z", pos.z);
		Lua::setField(L, "lastWakeAttemptMs", state->lastWakeAttemptMs);
		lua_rawseti(L, -2, idx++);
	}
	(void) total;
	return 1;
}

int GameFunctions::luaGameBotHibernateAllEligible(lua_State* L) {
	// Game.botHibernateAllEligible() — bulk hibernate every active bot. Returns count.
	uint32_t count = g_botEngine().hibernateAllEligibleBots();
	lua_pushnumber(L, count);
	return 1;
}

int GameFunctions::luaGameBotWakeAllHibernated(lua_State* L) {
	// Game.botWakeAllHibernated() — bulk wake every hibernated bot. Returns count.
	uint32_t count = g_botEngine().wakeAllHibernatedBots();
	lua_pushnumber(L, count);
	return 1;
}

int GameFunctions::luaGameBotReactivateForReload(lua_State* L) {
	// Game.botReactivateForReload(guid) — reactivate without teleport or cast toggle
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	bool result = g_botEngine().reactivateBotForReload(guid);
	lua_pushboolean(L, result);
	return 1;
}

int GameFunctions::luaGameBotCreateMarketOffer(lua_State* L) {
	// Game.botCreateMarketOffer(botGuid, action, itemId, amount, price, tier, anonymous)
	// Returns true on success, false on failure (logged in C++).
	uint32_t botGuid = Lua::getNumber<uint32_t>(L, 1);
	uint8_t action = Lua::getNumber<uint8_t>(L, 2);
	uint16_t itemId = Lua::getNumber<uint16_t>(L, 3);
	uint16_t amount = Lua::getNumber<uint16_t>(L, 4);
	uint64_t price = Lua::getNumber<uint64_t>(L, 5);
	uint8_t tier = Lua::getNumber<uint8_t>(L, 6, 0);
	bool anonymous = Lua::getBoolean(L, 7, false);
	bool ok = g_game().botCreateMarketOffer(botGuid, action, itemId, amount, price, tier, anonymous);
	lua_pushboolean(L, ok);
	return 1;
}

int GameFunctions::luaGameBotAcceptMarketOffer(lua_State* L) {
	// Game.botAcceptMarketOffer(botGuid, offerId, amount)
	uint32_t botGuid = Lua::getNumber<uint32_t>(L, 1);
	uint32_t offerId = Lua::getNumber<uint32_t>(L, 2);
	uint16_t amount = Lua::getNumber<uint16_t>(L, 3);
	bool ok = g_game().botAcceptMarketOffer(botGuid, offerId, amount);
	lua_pushboolean(L, ok);
	return 1;
}

int GameFunctions::luaGameBotCancelMarketOffer(lua_State* L) {
	// Game.botCancelMarketOffer(botGuid, offerId)
	uint32_t botGuid = Lua::getNumber<uint32_t>(L, 1);
	uint32_t offerId = Lua::getNumber<uint32_t>(L, 2);
	bool ok = g_game().botCancelMarketOffer(botGuid, offerId);
	lua_pushboolean(L, ok);
	return 1;
}

int GameFunctions::luaGameBotCountActive(lua_State* L) {
	// Game.botCountActive()
	lua_pushnumber(L, g_botEngine().countActiveBots());
	return 1;
}

int GameFunctions::luaGameBotCommand(lua_State* L) {
	// Game.botCommand(botName, command)
	const std::string &botName = Lua::getString(L, 1);
	const std::string &command = Lua::getString(L, 2);
	std::string result = g_botEngine().executeCommand(botName, command);
	Lua::pushString(L, result);
	return 1;
}

int GameFunctions::luaGameBotReload(lua_State* L) {
	// Game.botReload() — hot-reload the bot engine shared library
	auto& loader = BotEngineLoader::getInstance();
	if (!loader.isLoaded()) {
		Lua::pushString(L, "Bot engine not loaded.");
		return 1;
	}

	bool success = loader.reload();
	if (!success) {
		Lua::pushString(L, "FAILED to reload bot engine .so! Bots are disabled.");
		return 1;
	}

	// Re-initialize hunt data from MySQL
	loader.getEngine().loadHuntData();

	Lua::pushString(L, "Bot engine reloaded successfully.");
	return 1;
}

int GameFunctions::luaGameBotReregisterAll(lua_State* L) {
	// Game.botReregisterAll() — re-register all online bot players with the new engine instance
	auto& loader = BotEngineLoader::getInstance();
	if (!loader.isLoaded()) {
		lua_pushnumber(L, 0);
		return 1;
	}

	auto& engine = loader.getEngine();
	uint32_t count = 0;
	for (const auto& [id, player] : g_game().getPlayers()) {
		if (player && player->isBotPlayer()) {
			engine.registerBot(player);
			count++;
		}
	}
	lua_pushnumber(L, count);
	return 1;
}

int GameFunctions::luaGameBotStartTickLoop(lua_State* L) {
	// Game.botStartTickLoop() — create a new 100ms cycleEvent for g_botEngine().tick()
	// Used after hot-reload to ensure the tick loop is running
	if (!BotEngineLoader::getInstance().isLoaded()) {
		lua_pushboolean(L, false);
		return 1;
	}

	// JITTER FIX 2026-06-10: route through Game::restartBotTickLoop(), which stops
	// the previous cycleEvent first. This call used to LEAK one 100ms tick loop per
	// /cavebot reload (3 concurrent loops measured live on 2026-06-10 — all bot AI
	// cadences ran 3x fast and VT throughput tripled).
	g_game().restartBotTickLoop();
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotGetState(lua_State* L) {
	// Game.botGetState(guid) — returns bot AI state integer, or -1 if not found
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	auto* botState = g_botEngine().getBotState(guid);
	if (!botState) {
		lua_pushnumber(L, -1);
	} else {
		lua_pushnumber(L, static_cast<int>(botState->state));
	}
	return 1;
}

int GameFunctions::luaGameBotGetStatusText(lua_State* L) {
	// Game.botGetStatusText(guid) — returns bot status detail string, or "" if not found
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	std::string status = g_botEngine().getStatusText(guid);
	Lua::pushString(L, status);
	return 1;
}

int GameFunctions::luaGameBotInParty(lua_State* L) {
	// Game.botInParty(guid) — returns true if bot is currently in a player party
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	auto* botState = g_botEngine().getBotState(guid);
	if (!botState) {
		lua_pushboolean(L, false);
	} else {
		lua_pushboolean(L, botState->partyLeaderGuid != 0);
	}
	return 1;
}

int GameFunctions::luaGameBotIsActive(lua_State* L) {
	// Game.botIsActive(guid) — returns true if the bot is currently active (in the world, AI ticking)
	uint32_t guid = Lua::getNumber<uint32_t>(L, 1);
	auto* botState = g_botEngine().getBotState(guid);
	lua_pushboolean(L, botState && botState->active);
	return 1;
}

int GameFunctions::luaGameBotSaveStates(lua_State* L) {
	// Game.botSaveStates() — save all active bot states to DB (call from shutdown hook)
	g_botEngine().saveAllStates();
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotRestoreStates(lua_State* L) {
	// Game.botRestoreStates() — restore bot states from DB (call after all bots loaded)
	g_botEngine().restoreAllStates();
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameBotClearPersistedStates(lua_State* L) {
	// Game.botClearPersistedStates() — delete all rows from bot_state_persistence
	g_botEngine().clearPersistedStates();
	lua_pushboolean(L, true);
	return 1;
}

int GameFunctions::luaGameGetNormalizedPlayerName(lua_State* L) {
	// Game.getNormalizedPlayerName(name[, isNewName = false])
	const auto name = Lua::getString(L, 1);
	const auto isNewName = Lua::getBoolean(L, 2, false);
	const auto &player = g_game().getPlayerByName(name, true, isNewName);
	if (player) {
		Lua::pushString(L, player->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameGetNormalizedGuildName(lua_State* L) {
	// Game.getNormalizedGuildName(name)
	const auto name = Lua::getString(L, 1);
	const auto &guild = g_game().getGuildByName(name, true);
	if (guild) {
		Lua::pushString(L, guild->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int GameFunctions::luaGameAddInfluencedMonster(lua_State* L) {
	// Game.addInfluencedMonster(monster)
	const auto &monster = Lua::getUserdataShared<Monster>(L, 1, "Monster");
	if (!monster) {
		Lua::reportErrorFunc(Lua::getErrorDesc(LUA_ERROR_MONSTER_NOT_FOUND));
		Lua::pushBoolean(L, false);
		return 0;
	}

	lua_pushboolean(L, g_game().addInfluencedMonster(monster));
	return 1;
}

int GameFunctions::luaGameRemoveInfluencedMonster(lua_State* L) {
	// Game.removeInfluencedMonster(monsterId)
	const uint32_t monsterId = Lua::getNumber<uint32_t>(L, 1);
	const auto create = Lua::getBoolean(L, 2, false);
	lua_pushnumber(L, g_game().removeInfluencedMonster(monsterId, create));
	return 1;
}

int GameFunctions::luaGameGetInfluencedMonsters(lua_State* L) {
	// Game.getInfluencedMonsters()
	const auto &monsters = g_game().getInfluencedMonsters();
	lua_createtable(L, static_cast<int>(monsters.size()), 0);
	int index = 0;
	for (const auto monsterId : monsters) {
		++index;
		lua_pushnumber(L, monsterId);
		lua_rawseti(L, -2, index);
	}

	return 1;
}

int GameFunctions::luaGameGetLadderIds(lua_State* L) {
	// Game.getLadderIds()
	const auto &ladders = Item::items.getLadders();
	lua_createtable(L, static_cast<int>(ladders.size()), 0);
	int index = 0;
	for (const auto &ladderId : ladders) {
		++index;
		lua_pushnumber(L, static_cast<lua_Number>(ladderId));
		lua_rawseti(L, -2, index);
	}

	return 1;
}

int GameFunctions::luaGameGetDummies(lua_State* L) {
	/**
	 * @brief Retrieve dummy IDs categorized by type.
	 * @details This function provides a table containing two sub-tables: one for free dummies and one for house (or premium) dummies.

	* @note usage on lua:
	    local dummies = Game.getDummies()
	    local rate = dummies[1] -- Retrieve dummy rate
	*/

	const auto &dummies = Item::items.getDummys();
	lua_createtable(L, dummies.size(), 0);
	for (const auto &[dummyId, rate] : dummies) {
		lua_pushnumber(L, static_cast<lua_Number>(rate));
		lua_rawseti(L, -2, dummyId);
	}
	return 1;
}

int GameFunctions::luaGameMakeFiendishMonster(lua_State* L) {
	// Game.makeFiendishMonster(monsterId[default= 0])
	const auto monsterId = Lua::getNumber<uint32_t>(L, 1, 0);
	const auto createForgeableMonsters = Lua::getBoolean(L, 2, false);
	lua_pushnumber(L, g_game().makeFiendishMonster(monsterId, createForgeableMonsters));
	return 1;
}

int GameFunctions::luaGameRemoveFiendishMonster(lua_State* L) {
	// Game.removeFiendishMonster(monsterId)
	const uint32_t monsterId = Lua::getNumber<uint32_t>(L, 1);
	const auto create = Lua::getBoolean(L, 2, false);
	lua_pushnumber(L, g_game().removeFiendishMonster(monsterId, create));
	return 1;
}

int GameFunctions::luaGameGetFiendishMonsters(lua_State* L) {
	// Game.getFiendishMonsters()
	const auto &monsters = g_game().getFiendishMonsters();

	lua_createtable(L, static_cast<int>(monsters.size()), 0);
	int index = 0;
	for (const auto monsterId : monsters) {
		++index;
		lua_pushnumber(L, monsterId);
		lua_rawseti(L, -2, index);
	}

	return 1;
}

int GameFunctions::luaGameGetBoostedBoss(lua_State* L) {
	// Game.getBoostedBoss()
	Lua::pushString(L, g_ioBosstiary().getBoostedBossName());
	return 1;
}

int GameFunctions::luaGameGetTalkActions(lua_State* L) {
	// Game.getTalkActions()
	const auto talkactionsMap = g_talkActions().getTalkActionsMap();
	lua_createtable(L, static_cast<int>(talkactionsMap.size()), 0);

	for (const auto &[talkName, talkactionSharedPtr] : talkactionsMap) {
		Lua::pushUserdata<TalkAction>(L, talkactionSharedPtr);
		Lua::setMetatable(L, -1, "TalkAction");
		lua_setfield(L, -2, talkName.c_str());
	}
	return 1;
}

int GameFunctions::luaGameGetEventCallbacks(lua_State* L) {
	lua_createtable(L, 0, 0);
	lua_pushcfunction(L, EventCallbackFunctions::luaEventCallbackLoad);
	for (const auto &[value, name] : magic_enum::enum_entries<EventCallback_t>()) {
		if (value != EventCallback_t::none) {
			std::string methodName = magic_enum::enum_name(value).data();
			lua_pushstring(L, methodName.c_str());
			// Copy the function reference to the top of the stack
			lua_pushvalue(L, -2);
			lua_settable(L, -4);
		}
	}
	// Pop the function
	lua_pop(L, 1);
	return 1;
}

int GameFunctions::luaGameRegisterAchievement(lua_State* L) {
	// Game.registerAchievement(id, name, description, secret, grade, points)
	if (lua_gettop(L) < 6) {
		Lua::reportErrorFunc("Achievement can only be registered with all params.");
		return 1;
	}

	const uint16_t id = Lua::getNumber<uint16_t>(L, 1);
	const std::string name = Lua::getString(L, 2);
	const std::string description = Lua::getString(L, 3);
	const bool secret = Lua::getBoolean(L, 4);
	const uint8_t grade = Lua::getNumber<uint8_t>(L, 5);
	const uint8_t points = Lua::getNumber<uint8_t>(L, 6);
	g_game().registerAchievement(id, name, description, secret, grade, points);
	Lua::pushBoolean(L, true);
	return 1;
}

int GameFunctions::luaGameGetAchievementInfoById(lua_State* L) {
	// Game.getAchievementInfoById(id)
	const uint16_t id = Lua::getNumber<uint16_t>(L, 1);
	const Achievement achievement = g_game().getAchievementById(id);
	if (achievement.id == 0) {
		Lua::reportErrorFunc("Achievement id is wrong");
		return 1;
	}

	lua_createtable(L, 0, 6);
	Lua::setField(L, "id", achievement.id);
	Lua::setField(L, "name", achievement.name);
	Lua::setField(L, "description", achievement.description);
	Lua::setField(L, "points", achievement.points);
	Lua::setField(L, "grade", achievement.grade);
	Lua::setField(L, "secret", achievement.secret);
	return 1;
}

int GameFunctions::luaGameGetAchievementInfoByName(lua_State* L) {
	// Game.getAchievementInfoByName(name)
	const std::string name = Lua::getString(L, 1);
	const Achievement achievement = g_game().getAchievementByName(name);
	if (achievement.id == 0) {
		Lua::reportErrorFunc("Achievement name is wrong");
		return 1;
	}

	lua_createtable(L, 0, 6);
	Lua::setField(L, "id", achievement.id);
	Lua::setField(L, "name", achievement.name);
	Lua::setField(L, "description", achievement.description);
	Lua::setField(L, "points", achievement.points);
	Lua::setField(L, "grade", achievement.grade);
	Lua::setField(L, "secret", achievement.secret);
	return 1;
}

int GameFunctions::luaGameGetSecretAchievements(lua_State* L) {
	// Game.getSecretAchievements()
	const std::vector<Achievement> &achievements = g_game().getSecretAchievements();
	int index = 0;
	lua_createtable(L, achievements.size(), 0);
	for (const auto &achievement : achievements) {
		lua_createtable(L, 0, 6);
		Lua::setField(L, "id", achievement.id);
		Lua::setField(L, "name", achievement.name);
		Lua::setField(L, "description", achievement.description);
		Lua::setField(L, "points", achievement.points);
		Lua::setField(L, "grade", achievement.grade);
		Lua::setField(L, "secret", achievement.secret);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetPublicAchievements(lua_State* L) {
	// Game.getPublicAchievements()
	const std::vector<Achievement> &achievements = g_game().getPublicAchievements();
	int index = 0;
	lua_createtable(L, achievements.size(), 0);
	for (const auto &achievement : achievements) {
		lua_createtable(L, 0, 6);
		Lua::setField(L, "id", achievement.id);
		Lua::setField(L, "name", achievement.name);
		Lua::setField(L, "description", achievement.description);
		Lua::setField(L, "points", achievement.points);
		Lua::setField(L, "grade", achievement.grade);
		Lua::setField(L, "secret", achievement.secret);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetAchievements(lua_State* L) {
	// Game.getAchievements()
	const std::map<uint16_t, Achievement> &achievements = g_game().getAchievements();
	int index = 0;
	lua_createtable(L, achievements.size(), 0);
	for (const auto &achievement_it : achievements) {
		lua_createtable(L, 0, 6);
		Lua::setField(L, "id", achievement_it.first);
		Lua::setField(L, "name", achievement_it.second.name);
		Lua::setField(L, "description", achievement_it.second.description);
		Lua::setField(L, "points", achievement_it.second.points);
		Lua::setField(L, "grade", achievement_it.second.grade);
		Lua::setField(L, "secret", achievement_it.second.secret);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetSoulCoreItems(lua_State* L) {
	// Game.getSoulCoreItems()
	std::vector<const ItemType*> soulCoreItems;

	for (const auto &itemType : Item::items.getItems()) {
		if (itemType.m_primaryType == "SoulCores" || itemType.type == ITEM_TYPE_SOULCORES) {
			soulCoreItems.emplace_back(&itemType);
		}
	}

	lua_createtable(L, soulCoreItems.size(), 0);

	int index = 0;
	for (const auto* itemType : soulCoreItems) {
		Lua::pushUserdata<const ItemType>(L, itemType);
		Lua::setMetatable(L, -1, "ItemType");
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int GameFunctions::luaGameGetMonstersByRace(lua_State* L) {
	// Game.getMonstersByRace(race)
	const BestiaryType_t race = Lua::getNumber<BestiaryType_t>(L, 1);
	const auto monstersByRace = g_monsters().getMonstersByRace(race);

	lua_createtable(L, monstersByRace.size(), 0);
	int index = 0;
	for (const auto &monsterType : monstersByRace) {
		Lua::pushUserdata<MonsterType>(L, monsterType);
		Lua::setMetatable(L, -1, "MonsterType");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameGetMonstersByBestiaryStars(lua_State* L) {
	// Game.getMonstersByBestiaryStars(stars)
	const uint8_t stars = Lua::getNumber<uint8_t>(L, 1);
	const auto monstersByStars = g_monsters().getMonstersByBestiaryStars(stars);

	lua_createtable(L, monstersByStars.size(), 0);
	int index = 0;
	for (const auto &monsterType : monstersByStars) {
		Lua::pushUserdata<MonsterType>(L, monsterType);
		Lua::setMetatable(L, -1, "MonsterType");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int GameFunctions::luaGameIsDebugBuild(lua_State* L) {
	// Game.isDebugBuild()
#ifdef DEBUG_LOG
	Lua::pushBoolean(L, true);
#else
	Lua::pushBoolean(L, false);
#endif
	return 1;
}
