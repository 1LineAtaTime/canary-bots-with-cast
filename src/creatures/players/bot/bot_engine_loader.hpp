/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bot/bot_engine_interface.hpp"

class BotEngineLoader {
public:
	static BotEngineLoader& getInstance();

	// Load the engine for the first time.
	//
	// Linux: dlopen()s the shared library at soPath.
	// Windows: the engine is STATICALLY linked into canary.exe (see the long note in
	//          src/creatures/CMakeLists.txt), so soPath is ignored and the extern "C"
	//          factory is called directly. Never fails for a missing file.
	bool load(const std::string& soPath = "./libbot_engine.so");

	// True only where the engine can be swapped without restarting the server, i.e.
	// where it is a real shared library. Callers must consult this BEFORE treating a
	// false from reload() as an error -- on Windows reload is unsupported, not broken.
	static constexpr bool supportsHotReload() {
#ifdef _WIN32
		return false;
#else
		return true;
#endif
	}

	// Hot-reload: destroy engine, dlclose, dlopen new .so, create engine.
	//
	// Windows returns false WITHOUT touching the engine. It deliberately does not
	// destroy-and-recreate in place: the engine's ~200 file-scope `inline` globals
	// (party sessions, gang sessions, loot runs, per-guid cooldowns) are wiped on
	// Linux only because dlclose unloads the whole image. Statically linked they
	// would survive, and a fresh BotEngine reading stale guid-keyed state is exactly
	// the failure class the "fresh engine every load" design was bought to avoid
	// (see bot_engine_impl.hpp:6362). Restarting the process reproduces the Linux
	// guarantee exactly -- and on a local dev box a restart costs seconds, which is
	// the whole reason hot-reload exists on the production host and not here.
	bool reload();

	// Get the active engine instance (asserts loaded)
	IBotEngine& getEngine();

	// Check if .so is loaded and engine is valid
	bool isLoaded() const;

	// Clean shutdown
	void unload();

	BotEngineLoader(const BotEngineLoader&) = delete;
	BotEngineLoader& operator=(const BotEngineLoader&) = delete;

private:
	BotEngineLoader() = default;
	~BotEngineLoader();

	void* soHandle_ = nullptr;
	IBotEngine* engine_ = nullptr;
	std::string soPath_;
	std::string loadedCopyPath_; // temp copy path used during hot-reload (unique inode)

	// Factory function types exported by the .so
	using CreateFunc = IBotEngine* (*)();
	using DestroyFunc = void (*)(IBotEngine*);

	CreateFunc createFunc_ = nullptr;
	DestroyFunc destroyFunc_ = nullptr;
};
