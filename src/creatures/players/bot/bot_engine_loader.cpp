/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bot/bot_engine_loader.hpp"
#include "creatures/players/bot/bot_host.hpp"
#include "lib/logging/logger.hpp"
// Generated at build time (cmake/BotEngineAbiTag.cmake); the .so embeds the same tag.
#include "bot_engine_abi_tag.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <vector>

#ifdef _WIN32
// The engine is linked straight into canary.exe on Windows, so the factory the
// .so exports for dlsym() is just an ordinary external symbol here.
extern "C" IBotEngine* createBotEngine();
extern "C" void destroyBotEngine(IBotEngine* engine);
#else
	#include <dlfcn.h>
#endif

namespace {
#ifndef _WIN32
	// #114 item 3: the file-marker scan only exists for the dlopen()ed .so. On Windows the
	// engine is part of this image, so there is no library file to read (and MSVC rejects
	// [[gnu::used]]).
	// The executable carries the full marker too, so `grep -c BOTENGINE_ABI_TAG=<tag>` on both
	// shipped files shows at a glance whether they were built from one tree.
	[[gnu::used]] constexpr char kServerAbiMarker[] = BOT_ENGINE_ABI_MARKER;
	constexpr std::string_view kMarkerPrefix = BOT_ENGINE_ABI_MARKER_PREFIX;

	// #50 review: only libbot_engine.so calls the host services, so nothing in the executable
	// references bot_host.cpp. Where the core is linked as a static archive (test builds) that
	// object would be dropped and dlopen(RTLD_NOW) would fail on an undefined symbol. This file
	// is always linked (Game::start loads the engine through it), so referencing them here
	// keeps them in every build.
	[[gnu::used]] const auto kKeepBotHostScheduleEvent = &botHostScheduleEvent;
	[[gnu::used]] const auto kKeepBotHostMakePlayer = &botHostMakePlayer;

	bool isLowerHex(std::string_view s) {
		for (const char c : s) {
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				return false;
			}
		}
		return true;
	}
#endif
	constexpr std::string_view kServerAbiTag = BOT_ENGINE_ABI_TAG;
}

BotEngineLoader& BotEngineLoader::getInstance() {
	static BotEngineLoader instance;
	return instance;
}

BotEngineLoader::~BotEngineLoader() {
	unload();
}

#ifndef _WIN32
bool BotEngineLoader::verifyLibraryFile(const std::string& path, std::string& detail) const {
	// ABI gate, stage 1 (#58 review): read the tag out of the FILE BYTES. Nothing of the
	// library runs -- no dlopen, so no static initialisers of a mismatched build, and on a
	// reload the running engine has not been touched yet.
	//
	// #114: read in 1 MiB chunks (was: the whole ~100 MB .so through istreambuf_iterator, one
	// char at a time, on the game thread). Each window is the previous window's last
	// (markerLen - 1) bytes + the next chunk, so a marker straddling a chunk boundary is found
	// once, in the window that first holds all of it, and none can be counted twice (a whole
	// marker never fits in the carried tail).
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		detail = fmt::format("cannot open {}", path);
		return false;
	}
	const size_t markerLen = kMarkerPrefix.size() + kServerAbiTag.size();
	constexpr size_t kChunk = 1u << 20;
	std::string window;
	window.reserve(kChunk + markerLen);
	std::vector<char> buf(kChunk);
	std::string found;
	size_t markers = 0;
	while (in) {
		in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
		const auto got = static_cast<size_t>(in.gcount());
		if (got == 0) {
			break;
		}
		window.append(buf.data(), got);
		const std::string_view view(window);
		for (size_t pos = view.find(kMarkerPrefix); pos != std::string_view::npos;
		     pos = view.find(kMarkerPrefix, pos + 1)) {
			if (pos + markerLen > view.size()) {
				break; // incomplete at the window's end: the next window holds it whole
			}
			const auto tag = view.substr(pos + kMarkerPrefix.size(), kServerAbiTag.size());
			if (!isLowerHex(tag)) {
				continue; // the prefix alone, not a marker
			}
			++markers;
			if (tag != kServerAbiTag) {
				detail = fmt::format("ABI tag mismatch (library={}, server={})", tag, kServerAbiTag);
				return false;
			}
			found = std::string(tag);
		}
		const size_t keep = std::min(window.size(), markerLen - 1);
		window.erase(0, window.size() - keep);
	}
	if (in.bad()) {
		detail = fmt::format("read error on {}", path);
		return false;
	}
	if (markers == 0) {
		detail = fmt::format("no ABI marker in the library (built before the #58 file-marker gate; server={})", kServerAbiTag);
		return false;
	}
	detail = fmt::format("abi={}", found.substr(0, 12));
	return true;
}

bool BotEngineLoader::stageCopy(std::string& outPath, std::string& detail) const {
	// Hot-reload fix: copy the .so to a unique temp path so dlopen gets a fresh inode. Linux
	// caches dlopen by inode -- re-opening the same path after dlclose can return the old
	// (deleted) mapping instead of the new file content.
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	outPath = soPath_ + "." + std::to_string(ms) + ".tmp";
	std::error_code ec;
	std::filesystem::copy_file(soPath_, outPath, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		// #114: never leave a partial copy behind, and never fall back to opening the shared
		// original path -- that reintroduces the verify->dlopen race the private copy closes.
		std::error_code rmEc;
		std::filesystem::remove(outPath, rmEc);
		detail = fmt::format("cannot stage a private copy of {} ({}): reload refused, the running engine is kept",
		                     soPath_, ec.message());
		outPath.clear();
		return false;
	}
	return true;
}
#endif

void BotEngineLoader::dropPreflightCopy() {
	if (!preflightCopyPath_.empty()) {
		std::error_code ec;
		std::filesystem::remove(preflightCopyPath_, ec);
		preflightCopyPath_.clear();
	}
	preflightDetail_.clear();
	preflightAtMs_ = 0;
}

#ifndef _WIN32
void BotEngineLoader::removeStaleTempCopies() const {
	// #114: "<so>.<ms>.tmp" copies outlive a crash or a kill (unload() never ran). Nothing has
	// any of them open at startup, so every one of them is garbage.
	std::error_code ec;
	const std::filesystem::path so(soPath_);
	const auto dir = so.has_parent_path() ? so.parent_path() : std::filesystem::path(".");
	const std::string stem = so.filename().string() + ".";
	uint32_t removed = 0;
	for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
		const std::string name = e.path().filename().string();
		// Exactly our own pattern, "<so>.<digits>.tmp" (review): nothing else in the directory.
		if (name.size() <= stem.size() + 4 || !name.starts_with(stem) || !name.ends_with(".tmp")) {
			continue;
		}
		const std::string_view mid(name.data() + stem.size(), name.size() - stem.size() - 4);
		if (std::all_of(mid.begin(), mid.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
			std::error_code rmEc;
			if (std::filesystem::remove(e.path(), rmEc)) {
				removed++;
			}
		}
	}
	if (removed > 0) {
		g_logger().info("[BotEngineLoader] Removed {} stale temp copy(ies) of {}", removed, soPath_);
	}
}
#endif

bool BotEngineLoader::checkLibrary(std::string& detail) {
	if (!isLoaded()) {
		// #114: reload() can only REPLACE a running engine. After a startup refusal (or a failed
		// reload) there is none, and this pre-flight used to pass only for Game.botReload() to
		// answer "not loaded".
		detail = fmt::format("no bot engine is running ({}): restart the server with a canary and "
		                     "libbot_engine.so built from the same commit -- /cavebot reload cannot start one",
		                     lastError_.empty() ? std::string("not loaded at startup") : lastError_);
		return false;
	}
#ifdef _WIN32
	// #114 item 3: there is no library file -- the engine is statically linked into this
	// executable -- and nothing can be swapped in. REFUSE here, in the pre-flight, rather than
	// pass: the Lua reload flow runs before Game.botReload() and would otherwise force-deactivate
	// and re-activate every awake bot (hunts, travel and combat reset) while applying no code
	// and no data at all -- a "reload" that looks like it worked. Refusing here touches nothing.
	detail = "hot-reload is not supported on Windows (the bot engine is statically linked into canary.exe): "
	         "restart the server to apply code or data changes";
	return false;
#else
	dropPreflightCopy();
	// Review: remember what the source looked like, before AND after the copy. A changed size or
	// mtime across the copy means it may be torn; one changed by the time reload() runs means the
	// operator replaced the .so after this check -- either way the staged copy is not reused.
	std::error_code ec;
	const auto srcSize = std::filesystem::file_size(soPath_, ec);
	const auto srcMtime = ec ? std::filesystem::file_time_type {} : std::filesystem::last_write_time(soPath_, ec);
	if (ec) {
		detail = fmt::format("cannot stat {} ({})", soPath_, ec.message());
		return false;
	}
	std::string copy;
	if (!stageCopy(copy, detail)) {
		return false;
	}
	std::error_code ec2;
	if (std::filesystem::file_size(soPath_, ec2) != srcSize || ec2
	    || std::filesystem::last_write_time(soPath_, ec2) != srcMtime || ec2) {
		std::filesystem::remove(copy, ec2);
		detail = fmt::format("{} changed while it was being copied; run the reload again", soPath_);
		return false;
	}
	if (!verifyLibraryFile(copy, detail)) {
		std::filesystem::remove(copy, ec2);
		return false;
	}
	preflightSrcSize_ = srcSize;
	preflightSrcMtime_ = srcMtime;
	preflightCopyPath_ = copy;
	preflightDetail_ = detail;
	preflightAtMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return true;
#endif
}

bool BotEngineLoader::load(const std::string& soPath) {
	// Only set soPath_ on first load — reload() preserves the canonical path
	// and passes a temp copy path here instead
	if (soPath_.empty()) {
		soPath_ = soPath;
#ifndef _WIN32
		removeStaleTempCopies();
#endif
	}

#ifdef _WIN32
	// Statically linked: no image to open, no symbols to look up. Bind the factory
	// directly so every path below (including unload) is otherwise unchanged.
	lastError_.clear();
	createFunc_ = &createBotEngine;
	destroyFunc_ = &destroyBotEngine;
	engine_ = createFunc_();
	if (!engine_) {
		lastError_ = "createBotEngine() returned nullptr";
		g_logger().error("[BotEngineLoader] createBotEngine() returned nullptr");
		createFunc_ = nullptr;
		destroyFunc_ = nullptr;
		return false;
	}
	g_logger().info("[BotEngineLoader] Bot engine ready (statically linked, abi={}, hot-reload unavailable)", kServerAbiTag.substr(0, 12));
	return true;
#else
	std::string detail;
	if (!verifyLibraryFile(soPath, detail)) {
		lastError_ = detail;
		g_logger().error("[BotEngineLoader] REFUSING {}: {}. "
		                 "Rebuild canary and libbot_engine.so from the same commit and ship both.",
		                 soPath, detail);
		return false;
	}
	return openVerified(soPath);
#endif
}

#ifndef _WIN32
bool BotEngineLoader::openVerified(const std::string& soPath) {
	lastError_.clear();
	soHandle_ = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!soHandle_) {
		const char* err = dlerror();
		lastError_ = fmt::format("dlopen failed: {}", err ? err : "?");
		g_logger().error("[BotEngineLoader] Failed to load {}: {}", soPath, lastError_);
		return false;
	}

	// ABI gate, stage 2 (#58): the file marker was already checked before dlopen; this is the
	// second line of defence (the exported tag must agree too). botEngineAbiTag is a plain C
	// function returning a literal, so calling it is safe.
	using AbiTagFunc = const char* (*)();
	auto abiTagFunc = reinterpret_cast<AbiTagFunc>(dlsym(soHandle_, "botEngineAbiTag"));
	const char* soTag = abiTagFunc ? abiTagFunc() : nullptr;
	if (!soTag || std::strcmp(soTag, BOT_ENGINE_ABI_TAG) != 0) {
		g_logger().error("[BotEngineLoader] REFUSING {}: ABI tag mismatch (library={}, server={}). "
		                 "Rebuild canary and libbot_engine.so from the same commit and ship both.",
		                 soPath, soTag ? soTag : "<none: built before the ABI tag>", BOT_ENGINE_ABI_TAG);
		lastError_ = "ABI tag mismatch after dlopen";
		dlclose(soHandle_);
		soHandle_ = nullptr;
		return false;
	}

	createFunc_ = reinterpret_cast<CreateFunc>(dlsym(soHandle_, "createBotEngine"));
	destroyFunc_ = reinterpret_cast<DestroyFunc>(dlsym(soHandle_, "destroyBotEngine"));

	if (!createFunc_ || !destroyFunc_) {
		lastError_ = "missing factory symbols";
		g_logger().error("[BotEngineLoader] Missing factory symbols in {}: {}", soPath, dlerror());
		createFunc_ = nullptr;
		destroyFunc_ = nullptr;
		dlclose(soHandle_);
		soHandle_ = nullptr;
		return false;
	}

	engine_ = createFunc_();
	if (!engine_) {
		lastError_ = "createBotEngine() returned nullptr";
		g_logger().error("[BotEngineLoader] createBotEngine() returned nullptr");
		createFunc_ = nullptr;
		destroyFunc_ = nullptr;
		dlclose(soHandle_);
		soHandle_ = nullptr;
		return false;
	}

	g_logger().info("[BotEngineLoader] Loaded {} successfully (abi={})", soPath, kServerAbiTag.substr(0, 12));
	return true;
}
#endif

bool BotEngineLoader::reload() {
#ifdef _WIN32
	// Refuse rather than fake it. See the rationale on supportsHotReload() in the
	// header; callers use that predicate to report "restart required" instead of
	// the misleading "bots are disabled" that a plain false would produce.
	lastError_ = "hot-reload is not supported on Windows (engine is statically linked): restart the server";
	g_logger().warn("[BotEngineLoader] Hot-reload is not supported on Windows (engine is statically linked) -- restart the server to apply changes");
	return false;
#else
	g_logger().info("[BotEngineLoader] Starting hot-reload of {}...", soPath_);

	// The private copy is what gets verified and opened, so the bytes that pass the ABI gate are
	// exactly the bytes that get loaded. #114: the `/cavebot reload` pre-flight (checkLibrary)
	// staged and verified one moments ago -- use it instead of copying and scanning ~100 MB
	// again. A stale one (> 60 s: an aborted reload, a manual Game.botCheckLibrary) or one whose
	// source .so has changed size or mtime since it was staged is dropped and redone.
	std::string loadPath;
	std::string detail;
	const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	std::error_code ec;
	std::error_code srcEc;
	const bool srcUnchanged = std::filesystem::file_size(soPath_, srcEc) == preflightSrcSize_ && !srcEc
		&& std::filesystem::last_write_time(soPath_, srcEc) == preflightSrcMtime_ && !srcEc;
	if (!preflightCopyPath_.empty() && nowMs - preflightAtMs_ <= 60000 && srcUnchanged
	    && std::filesystem::exists(preflightCopyPath_, ec)) {
		loadPath = preflightCopyPath_;
		detail = preflightDetail_;
		preflightCopyPath_.clear(); // ownership moves to this reload
		preflightDetail_.clear();
		preflightAtMs_ = 0;
		g_logger().info("[BotEngineLoader] Using the pre-flight copy {} ({}), no second scan", loadPath, detail);
	} else {
		dropPreflightCopy();
		if (!stageCopy(loadPath, detail)) {
			lastError_ = detail;
			g_logger().error("[BotEngineLoader] Hot-reload REFUSED: {}", detail);
			return false;
		}
		g_logger().info("[BotEngineLoader] Copied .so to temp path: {}", loadPath);
		// ABI gate BEFORE anything is torn down (#58 review): a mismatched library is refused
		// while the running engine keeps running. The old order destroyed and dlclose'd it
		// first, so a bad .so left the server without bots until a restart.
		if (!verifyLibraryFile(loadPath, detail)) {
			lastError_ = detail;
			g_logger().error("[BotEngineLoader] Hot-reload REFUSED for {}: {}. The running bot engine was kept. "
			                 "Rebuild canary and libbot_engine.so from the same commit and ship both.",
			                 soPath_, detail);
			std::filesystem::remove(loadPath, ec);
			return false;
		}
	}

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

	// Clean up the previous temp copy (its handle was closed above)
	if (!loadedCopyPath_.empty()) {
		std::filesystem::remove(loadedCopyPath_, ec);
		if (!ec) {
			g_logger().info("[BotEngineLoader] Removed old temp copy: {}", loadedCopyPath_);
		}
		loadedCopyPath_.clear();
	}

	const bool ok = openVerified(loadPath);
	if (ok) {
		loadedCopyPath_ = loadPath;
		g_logger().info("[BotEngineLoader] Hot-reload complete (fresh inode via {})", loadPath);
	} else {
		g_logger().error("[BotEngineLoader] Hot-reload FAILED — bot engine is unavailable");
		std::filesystem::remove(loadPath, ec);
	}
	return ok;
#endif
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
#ifndef _WIN32
	if (soHandle_) {
		dlclose(soHandle_);
		soHandle_ = nullptr;
	}
#endif
	createFunc_ = nullptr;
	destroyFunc_ = nullptr;

	// Clean up temp copy
	if (!loadedCopyPath_.empty()) {
		std::error_code ec;
		std::filesystem::remove(loadedCopyPath_, ec);
		loadedCopyPath_.clear();
	}
	dropPreflightCopy();
}

// Global accessor — this is what g_botEngine() resolves to
IBotEngine& getBotEngineInstance() {
	return BotEngineLoader::getInstance().getEngine();
}

bool isBotEngineReady() {
	return BotEngineLoader::getInstance().isLoaded();
}
