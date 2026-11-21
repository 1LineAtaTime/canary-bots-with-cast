/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "canary_server.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "lib/di/container.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <span>
	#include <string_view>
#endif

namespace {
	constexpr std::string_view GenerateLuaApiDocsOnlyArgument = "--generate-lua-api-docs-only";

	bool hasArgument(const std::span<char*> arguments, const std::string_view expectedArgument) {
		for (std::size_t index = 1; index < arguments.size(); ++index) {
			if (std::string_view(arguments[index]) == expectedArgument) {
				return true;
			}
		}
		return false;
	}
}

int main(int argc, char* argv[]) {
	// FORK(P5 #160): build the Dispatcher singleton FIRST. Function-local statics are destroyed in reverse
	// order of construction, and Game is first built inside CanaryServer's dependencies (ServiceManager ->
	// Signals::asyncWait -> g_game()), before anything touches the dispatcher. Game then outlived it: at exit
	// ~Game -> ~Map -> ~SpawnNpc -> Dispatcher::stopEvent() read the freed dispatcher (ASAN
	// heap-use-after-free on every SIGTERM). Candidate upstream report.
	[[maybe_unused]] const auto &dispatcher = g_dispatcher();
	auto &server = inject<CanaryServer>();
	const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
	if (hasArgument(arguments, GenerateLuaApiDocsOnlyArgument)) {
		return server.generateLuaApiDocsOnly();
	}

	return server.run();
}
