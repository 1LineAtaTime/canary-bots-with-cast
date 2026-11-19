/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// BOT_TEST_HARNESS Phase 2 — the bot audit sink (MAIN BINARY side).
//
// A synchronous NDJSON file writer owned by the main binary, deliberately NOT a spdlog
// logger: g_logger is async with an 8192-slot overrun_oldest queue and a 3 s flush, so it
// drops lines under load and stamps them with a cycle-cached local time (review finding R4).
// Every record carries its own epoch-ms `ts` from std::chrono::system_clock instead.
//
// WHY IT LIVES IN THE MAIN BINARY, AND HOW THE ENGINE REACHES IT (the ABI choice):
//   * No change to bot_engine_interface.hpp / IBotEngine. The engine (libbot_engine.so on
//     Linux, a static lib on Windows) calls the free-standing singleton below directly. On
//     Linux the .so resolves these symbols from the executable at dlopen time, exactly as it
//     already resolves Game, Player and g_configManager (the binary links with -rdynamic,
//     cmake/modules/CanaryLib.cmake). On Windows the engine is linked statically into the
//     same image, so it is an ordinary call.
//   * pImpl: every member is behind Impl, and every method is out of line, so the class
//     LAYOUT is invisible to the engine. The sink can grow fields without the engine ever
//     being rebuilt against a stale layout. The only contract is the method list here.
//   * Settings (on/off, kinds, watch list) live HERE, not in the engine, so the runtime
//     `/cavebot audit ...` toggles survive a `/cavebot reload` (which destroys the engine and,
//     on Linux, dlclose()s every engine global). They reset on restart; config.lua is the
//     single source of the defaults (botAudit* keys, src/config/bot_config_keys.hpp).
//   * No thread. write() is synchronous: fwrite + fflush of whole lines under a mutex. The
//     engine batches its records per tick and calls write() once from the dispatcher, so
//     the steady-state cost is one small fwrite per 100 ms tick — and nothing at all when off.
//
// A new libbot_engine.so references these symbols, so it CANNOT be loaded by a canary binary
// built before this file existed (dlopen fails with an undefined symbol, the engine stays
// unloaded). Ship the first deploy of this as a full rebuild + restart.
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class BotAuditSink {
public:
	static BotAuditSink &get();

	BotAuditSink(const BotAuditSink &) = delete;
	BotAuditSink &operator=(const BotAuditSink &) = delete;

	// Bumps on every settings change (enable, kinds, watch, config re-apply). The engine polls
	// this once per tick and rebuilds its cached bitmask only when it moved. The first call
	// applies config.lua lazily, so the sink never reads config before it is loaded.
	[[nodiscard]] uint32_t generation();

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool on);

	// Comma list (case-insensitive). "all" = every kind. Stored normalised (lowercase, no
	// spaces, deduplicated); the engine maps the names it knows onto its own bitmask.
	[[nodiscard]] std::string kinds() const;
	void setKinds(const std::string &csv);
	// True if the sink is enabled and `kind` is selected. For main-binary producers (the
	// Game.botCommand `cmd` record); engine hooks use their own cached bitmask instead.
	[[nodiscard]] bool wants(std::string_view kind) const;

	// Watched bots, by name (comma list; matching is case-insensitive). "*" = every bot, but only
	// for the engine's low-rate kinds (AUD_WATCHALL_KINDS: wake, hib, state, decide, pathout
	// osc/stuck/abandoned); probe, path, walkpause and pathout-arrived need explicit names.
	[[nodiscard]] std::string watch() const;
	void setWatch(const std::string &csv);
	void addWatch(const std::string &name);
	bool removeWatch(const std::string &name);

	[[nodiscard]] int32_t probeMs() const;
	[[nodiscard]] int32_t censusMs() const;

	// Re-read the botAudit* keys from config.lua (after `/cavebot reloadconfig`), discarding
	// any runtime toggles.
	void applyConfig();

	// Monotonic record sequence, shared by every producer (engine and main binary). File
	// order is NOT seq order — the engine flushes per tick — so consumers sort by seq/ts.
	[[nodiscard]] uint64_t nextSeq();

	// Append one or more COMPLETE '\n'-terminated lines and flush. No-op while disabled.
	// Size-capped: past botAuditMaxMb the file is rotated to <path>.1 (one generation kept),
	// so the audit can never use more than ~2x the cap on disk. A failed open is retried at most
	// every 5 s; 5 consecutive writes slower than 50 ms auto-disable the audit (disk stall
	// kill switch: this runs on the dispatcher and must not keep blocking the tick).
	void write(std::string_view lines);

	// Main-binary `cmd` record: one line per Game.botCommand call when the cmd kind is on.
	void writeCmd(std::string_view source, std::string_view botName, std::string_view command, bool ok, std::string_view replyHead);

	[[nodiscard]] std::string status() const;

	// Epoch milliseconds from the wall clock — NEVER OTSYS_TIME (cycle-cached; lagmark root cause).
	[[nodiscard]] static int64_t epochMs();
	// Appends `s` as a quoted JSON string (escapes ", \, control chars; passes UTF-8 through).
	static void appendJsonString(std::string &out, std::string_view s);
	// At most `maxBytes` of `s`, cut back to a UTF-8 code-point boundary (never half a character).
	[[nodiscard]] static std::string_view utf8Prefix(std::string_view s, size_t maxBytes);

private:
	// The lazy first applyConfig(), run by generation() AND by every setter, so a `/cavebot audit`
	// issued before the engine's first tick is not overwritten by the config.lua defaults later.
	void ensureConfigured();
	BotAuditSink();
	~BotAuditSink();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
