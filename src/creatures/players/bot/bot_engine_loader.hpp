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

	// Load the shared library for the first time
	bool load(const std::string& soPath = "./libbot_engine.so");

	// Hot-reload: destroy engine, dlclose, dlopen new .so, create engine
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
