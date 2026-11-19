/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bot/bot_engine_interface.hpp"

#include <filesystem>

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

	// Hot-reload: verify the new .so's ABI marker, then destroy engine, dlclose, dlopen
	// the new .so, create engine. A refused library leaves the running engine untouched
	// (isLoaded() stays true) and returns false; lastError() says why.
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

	// ABI pre-flight (#58): read the ABI marker out of the library file (no dlopen) and
	// compare it with this executable's tag. Used by `/cavebot reload` before it touches
	// any bot. On success detail holds a short tag; on failure the reason.
	// #114: it stages the private copy reload() will open and verifies THAT, so one
	// `/cavebot reload` scans the library once, and the bytes verified are the bytes loaded.
	// Fails ("restart required") when no engine is loaded: reload() cannot start one.
	// Windows (#114 item 3): there is no library file and nothing to swap in, so it always
	// fails with a "restart the server" detail and scans/stages nothing. Failing HERE is what
	// keeps `/cavebot reload` zero-residue: the Lua flow would otherwise force-deactivate and
	// re-activate every awake bot before Game.botReload() refused.
	bool checkLibrary(std::string& detail);

	// #114 (review): drop the copy checkLibrary() staged when the reload that was meant to use it
	// is aborted (the Lua flow's CSV pre-flight or prefix failure). Idempotent.
	void discardLibraryCheck() {
		dropPreflightCopy();
	}

	// Why the last load()/reload() failed or was refused (empty after a success).
	const std::string& lastError() const {
		return lastError_;
	}

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

	bool verifyLibraryFile(const std::string& path, std::string& detail) const;
	bool openVerified(const std::string& soPath); // dlopen + factories; marker already checked
	// #114: copy soPath_ to a fresh unique temp path (fresh inode for dlopen). On failure no
	// partial file is left behind and detail says why.
	bool stageCopy(std::string& outPath, std::string& detail) const;
	void dropPreflightCopy();
	void removeStaleTempCopies() const; // leftovers of a crash, at startup

	void* soHandle_ = nullptr;
	IBotEngine* engine_ = nullptr;
	std::string soPath_;
	std::string loadedCopyPath_; // temp copy path used during hot-reload (unique inode)
	std::string lastError_;
	// #114: a copy staged + verified by checkLibrary(), consumed by the reload() right after it.
	std::string preflightCopyPath_;
	std::string preflightDetail_;
	int64_t preflightAtMs_ = 0;
	// Identity of soPath_ when the copy was staged: reload() reuses the copy only if it still matches.
	uintmax_t preflightSrcSize_ = 0;
	std::filesystem::file_time_type preflightSrcMtime_ {};

	// Factory function types exported by the .so
	using CreateFunc = IBotEngine* (*)();
	using DestroyFunc = void (*)(IBotEngine*);

	CreateFunc createFunc_ = nullptr;
	DestroyFunc destroyFunc_ = nullptr;
};
