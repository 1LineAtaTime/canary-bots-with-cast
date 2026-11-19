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

// True when an engine is constructed and safe to call.
//
// getBotEngineInstance() dereferences a null pointer when it is not, so any caller
// that can run while the engine is absent MUST check first. That window is real on
// Linux: the engine is missing before Game::start() loads it, and between the
// destroy and the dlopen of every /cavebot reload. The protocol layer is the sharp
// edge — a login or a status poll arriving in that window would take the server
// down rather than merely miss the bots.
bool isBotEngineReady();

// All existing call sites use g_botEngine().method() — this preserves that pattern
constexpr auto g_botEngine = getBotEngineInstance;
