/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bot/bot_engine_loader.hpp"
#include "lib/logging/logger.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <chrono>

BotEngineLoader& BotEngineLoader::getInstance() {
	static BotEngineLoader instance;
	return instance;
}

BotEngineLoader::~BotEngineLoader() {
	unload();
}

bool BotEngineLoader::load(const std::string& soPath) {
	// Only set soPath_ on first load — reload() preserves the canonical path
	// and passes a temp copy path here instead
	if (soPath_.empty()) {
		soPath_ = soPath;
	}

	soHandle_ = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!soHandle_) {
		g_logger().error("[BotEngineLoader] Failed to load {}: {}", soPath, dlerror());
		return false;
	}

	createFunc_ = reinterpret_cast<CreateFunc>(dlsym(soHandle_, "createBotEngine"));
	destroyFunc_ = reinterpret_cast<DestroyFunc>(dlsym(soHandle_, "destroyBotEngine"));

	if (!createFunc_ || !destroyFunc_) {
		g_logger().error("[BotEngineLoader] Missing factory symbols in {}: {}", soPath, dlerror());
		dlclose(soHandle_);
		soHandle_ = nullptr;
		return false;
	}

	engine_ = createFunc_();
	if (!engine_) {
		g_logger().error("[BotEngineLoader] createBotEngine() returned nullptr");
		dlclose(soHandle_);
		soHandle_ = nullptr;
		return false;
	}

	g_logger().info("[BotEngineLoader] Loaded {} successfully", soPath);
	return true;
}

bool BotEngineLoader::reload() {
	g_logger().info("[BotEngineLoader] Starting hot-reload of {}...", soPath_);

	// Destroy old engine instance
	if (engine_ && destroyFunc_) {
		destroyFunc_(engine_);
		engine_ = nullptr;
	}

	// Close old shared library
	if (soHandle_) {
		int rc = dlclose(soHandle_);
		g_logger().info("[BotEngineLoader] dlclose returned {}", rc);
		soHandle_ = nullptr;
	}

	createFunc_ = nullptr;
	destroyFunc_ = nullptr;

	// Clean up any previous temp copy
	if (!loadedCopyPath_.empty()) {
		std::error_code ec;
		std::filesystem::remove(loadedCopyPath_, ec);
		if (!ec) {
			g_logger().info("[BotEngineLoader] Removed old temp copy: {}", loadedCopyPath_);
		}
		loadedCopyPath_.clear();
	}

	// Hot-reload fix: copy the .so to a unique temp path so dlopen gets a fresh
	// inode. Linux caches dlopen by inode — re-opening the same path after dlclose
	// can return the old (deleted) mapping instead of the new file content.
	auto now = std::chrono::steady_clock::now().time_since_epoch();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	std::string tempPath = soPath_ + "." + std::to_string(ms) + ".tmp";

	std::error_code ec;
	std::filesystem::copy_file(soPath_, tempPath, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		g_logger().error("[BotEngineLoader] Failed to copy {} -> {}: {}", soPath_, tempPath, ec.message());
		// Fallback: try loading from original path anyway
		bool ok = load(soPath_);
		if (ok) {
			g_logger().info("[BotEngineLoader] Hot-reload complete (fallback to original path)");
		} else {
			g_logger().error("[BotEngineLoader] Hot-reload FAILED — bot engine is unavailable");
		}
		return ok;
	}

	g_logger().info("[BotEngineLoader] Copied .so to temp path: {}", tempPath);
	loadedCopyPath_ = tempPath;

	// Load from the unique temp path — guaranteed fresh inode
	bool ok = load(tempPath);
	if (ok) {
		g_logger().info("[BotEngineLoader] Hot-reload complete (fresh inode via {})", tempPath);
	} else {
		g_logger().error("[BotEngineLoader] Hot-reload FAILED — bot engine is unavailable");
		// Clean up failed temp copy
		std::filesystem::remove(tempPath, ec);
		loadedCopyPath_.clear();
	}
	return ok;
}

IBotEngine& BotEngineLoader::getEngine() {
	return *engine_;
}

bool BotEngineLoader::isLoaded() const {
	return engine_ != nullptr;
}

void BotEngineLoader::unload() {
	if (engine_ && destroyFunc_) {
		destroyFunc_(engine_);
		engine_ = nullptr;
	}
	if (soHandle_) {
		dlclose(soHandle_);
		soHandle_ = nullptr;
	}
	createFunc_ = nullptr;
	destroyFunc_ = nullptr;

	// Clean up temp copy
	if (!loadedCopyPath_.empty()) {
		std::error_code ec;
		std::filesystem::remove(loadedCopyPath_, ec);
		loadedCopyPath_.clear();
	}
}

// Global accessor — this is what g_botEngine() resolves to
IBotEngine& getBotEngineInstance() {
	return BotEngineLoader::getInstance().getEngine();
}
