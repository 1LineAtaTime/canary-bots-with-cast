/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bot/bot_engine_interface.hpp"

// Provided by bot_engine_loader.cpp — returns the active IBotEngine from the loaded .so
IBotEngine& getBotEngineInstance();

// All existing call sites use g_botEngine().method() — this preserves that pattern
constexpr auto g_botEngine = getBotEngineInstance;
