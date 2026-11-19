/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// Host services for libbot_engine.so (#50). See bot_host.hpp: these run in the executable so
// that the shared_ptr control blocks they create point at the executable's vtables, which stay
// mapped when the engine library is unloaded by /cavebot reload.

#include "creatures/players/bot/bot_host.hpp"

#include "creatures/players/player.hpp"
#include "game/scheduling/dispatcher.hpp"

// [[gnu::used]] / [[gnu::noinline]]: nothing in the executable calls these, only the engine
// (through the dynamic symbol table), so keep LTO from discarding or folding them.
[[gnu::used, gnu::noinline]] uint64_t botHostScheduleEvent(uint32_t delayMs, std::function<void(void)> &&f, std::string_view context) {
	return g_dispatcher().scheduleEvent(delayMs, std::move(f), context);
}

[[gnu::used, gnu::noinline]] std::shared_ptr<Player> botHostMakePlayer() {
	return std::make_shared<Player>(nullptr);
}
