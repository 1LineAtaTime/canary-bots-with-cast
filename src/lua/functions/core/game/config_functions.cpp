/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "lua/functions/core/game/config_functions.hpp"

#include "config/configmanager.hpp"
#include "lua/functions/lua_functions_loader.hpp"

void ConfigFunctions::init(lua_State* L) {
	Lua::registerTable(L, "configManager");
	Lua::registerMethod(L, "configManager", "getString", luaConfigManagerGetString);
	Lua::registerMethod(L, "configManager", "getNumber", luaConfigManagerGetNumber);
	Lua::registerMethod(L, "configManager", "getBoolean", luaConfigManagerGetBoolean);
	Lua::registerMethod(L, "configManager", "getFloat", luaConfigManagerGetFloat);

#define registerMagicEnumIn(L, tableName, enumValue)         \
	do {                                                     \
		auto name = magic_enum::enum_name(enumValue).data(); \
		Lua::registerVariable(L, tableName, name, value);    \
	} while (0)
	Lua::registerTable(L, "configKeys");
	for (auto value : magic_enum::enum_values<ConfigKey_t>()) {
		auto enumName = magic_enum::enum_name(value).data();
		if (enumName) {
			registerMagicEnumIn(L, "configKeys", value);
			g_logger().debug("Registering ConfigManager enum name '{}' value '{}' to lua", enumName, fmt::underlying(value));
		}
	}
#undef registerMagicEnumIn

	// FORK(bot) #154: register the bot-fork keys by NAME, independent of magic_enum. magic_enum
	// only enumerates ordinals up to MAGIC_ENUM_RANGE_MAX (500, src/pch.hpp); the BOT_* tables
	// are expanded after the core keys, and every upstream sync inserts more core keys mid-enum,
	// so BOT_* ordinals cross 500 and the loop above silently skips them. Lua then reads
	// configKeys.BOT_X as nil and every configManager.get*(nil) quietly returns a fallback.
	// The values are the real ordinals, so re-registering a key the loop already saw is a no-op.
#define BOT_CFG_LUA_KEY(enumKey, luaKey, defaultValue) \
	Lua::registerVariable(L, "configKeys", #enumKey, static_cast<lua_Number>(enumKey));
	BOT_ALL_CONFIG_KEY_TABLES(BOT_CFG_LUA_KEY, BOT_CFG_LUA_KEY, BOT_CFG_LUA_KEY)
#undef BOT_CFG_LUA_KEY
	// Fork keys outside the tables: three in the alphabetical block (low ordinals today, but the
	// block grows with every sync) and the ones appended at the very end of ConfigKey_t.
	Lua::registerVariable(L, "configKeys", "BOT_PLAYERS_ONLINE", static_cast<lua_Number>(BOT_PLAYERS_ONLINE));
	Lua::registerVariable(L, "configKeys", "BOT_PLAYERS_SHOW_AS_ONLINE", static_cast<lua_Number>(BOT_PLAYERS_SHOW_AS_ONLINE));
	Lua::registerVariable(L, "configKeys", "BOT_PERSONALITY_REROLL_ON_RESTART", static_cast<lua_Number>(BOT_PERSONALITY_REROLL_ON_RESTART));
	Lua::registerVariable(L, "configKeys", "FORCE_LOGOUT_ON_CONNECTION_LOSS", static_cast<lua_Number>(FORCE_LOGOUT_ON_CONNECTION_LOSS));
}

int ConfigFunctions::luaConfigManagerGetString(lua_State* L) {
	const auto key = Lua::getNumber<ConfigKey_t>(L, -1);
	if (!key) {
		Lua::reportErrorFunc("Wrong enum");
		return 1;
	}

	Lua::pushString(L, g_configManager().getString(Lua::getNumber<ConfigKey_t>(L, -1)));
	return 1;
}

int ConfigFunctions::luaConfigManagerGetNumber(lua_State* L) {
	const auto key = Lua::getNumber<ConfigKey_t>(L, -1);
	if (!key) {
		Lua::reportErrorFunc("Wrong enum");
		return 1;
	}

	lua_pushnumber(L, g_configManager().getNumber(Lua::getNumber<ConfigKey_t>(L, -1)));
	return 1;
}

int ConfigFunctions::luaConfigManagerGetBoolean(lua_State* L) {
	const auto key = Lua::getNumber<ConfigKey_t>(L, -1);
	if (!key) {
		Lua::reportErrorFunc("Wrong enum");
		return 1;
	}

	Lua::pushBoolean(L, g_configManager().getBoolean(Lua::getNumber<ConfigKey_t>(L, -1)));
	return 1;
}

int ConfigFunctions::luaConfigManagerGetFloat(lua_State* L) {
	// configManager.getFloat(key, shouldRound = true)

	// Ensure the first argument (key) is provided and is a valid enum
	const auto key = Lua::getNumber<ConfigKey_t>(L, 1);
	if (!key) {
		Lua::reportErrorFunc("Wrong enum");
		return 1;
	}

	// Check if the second argument (shouldRound) is provided and is a boolean; default to true if not provided
	bool shouldRound = Lua::getBoolean(L, 2, true);
	float value = g_configManager().getFloat(key);
	double finalValue = shouldRound ? static_cast<double>(std::round(value * 100.0) / 100.0) : value;

	g_logger().debug("[{}] key: {}, finalValue: {}, shouldRound: {}", __FUNCTION__, magic_enum::enum_name(key), finalValue, shouldRound);
	lua_pushnumber(L, finalValue);
	return 1;
}
