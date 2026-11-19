/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// Host services for libbot_engine.so (#50). Defined in the EXECUTABLE (bot_host.cpp) and
// reached from the engine through -rdynamic, exactly like bot_audit_sink.
//
// Why these exist: since #50 the engine library really unloads on /cavebot reload. Any object
// the executable keeps past that point must not carry a pointer into the unloaded image.
// `std::make_shared<T>` writes the vptr of its control block (`_Sp_counted_ptr_inplace<T>`)
// in the TU that calls it, and the executable does NOT export those vtables (LTO keeps them
// local), so a make_shared executed inside the engine points its control block at the
// engine's own copy of the vtable. When that object is released after the reload, the
// dispatcher calls `_M_dispose` through a vtable that is no longer mapped -- or, because the
// next copy is mapped at the same base, through whatever the new build has at that offset.
// That is the "12-minute" reload SEGV (`_Sp_counted_base::_M_release` <- `~shared_ptr<Task>`
// <- Dispatcher::executeScheduledEvents), reproduced on a debug box for #50.
//
// So every object that can outlive the engine is constructed HERE:
//   - dispatcher Tasks (the inline Dispatcher::scheduleEvent overload does make_shared<Task>);
//   - Players (materializePlayerFromDb; awake bots survive a reload in the game world).
// The POST_BUILD gate (cmake/check_bot_engine_symbols.cmake) fails the build when the engine
// defines a shared_ptr control block for any type outside its private allowlist, and when it
// calls a Dispatcher scheduling entry point directly instead of botHostScheduleEvent.
//
// The lambda inside a scheduled Task is still engine code. The engine MUST cancel every event
// it scheduled before it is destroyed (BotEngine::cancelPendingDispatcherEvents); a cancelled
// Task drops its std::function at once, so what stays queued is plain executable data.

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

class Player;

// g_dispatcher().scheduleEvent(delayMs, f, context), with the Task allocated by the executable.
// Returns the event id for Dispatcher::stopEvent (0 while the dispatcher is shutting down).
uint64_t botHostScheduleEvent(uint32_t delayMs, std::function<void(void)> &&f, std::string_view context);

// std::make_shared<Player>(nullptr), allocated by the executable.
std::shared_ptr<Player> botHostMakePlayer();
