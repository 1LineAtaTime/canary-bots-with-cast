/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#include "database/database.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Bundle 6 (2026-06-11): dedicated DB worker for BOT-originated async queries.
//
// DatabaseTasks runs its query bodies on the shared BS::thread_pool — the same
// 3 free workers Dispatcher::asyncWait borrows for the parallel monster-AI
// partition — so every bot DB burst (market INSERTs, offer purges, monitor
// scans, virtual position saves, cast_broadcasters writes) occupied AI workers
// and parked the dispatcher at the fork-join barrier (the "fuel-1 worker-theft"
// lagmark class, measured end-to-end at the 2026-06-11 13:34 lagmark:
// [ASYNC_WAIT] n=89 wall=3260ms while qlat=3336ms). All queries also serialized
// on the single shared Database connection's databaseLock, so worker-side
// transactions head-of-line-blocked sync dispatcher queries (BEGIN waited 3.0s).
//
// This class owns ONE dedicated std::jthread and ONE dedicated MySQL
// connection: bot DB I/O can no longer steal asyncWait workers, and it never
// touches the shared databaseLock. Callbacks are dispatched back onto the
// dispatcher thread, mirroring DatabaseTasks semantics exactly.
//
// Stock-code note: this is a NEW bot-infrastructure class; DatabaseTasks and
// all stock callers are untouched.
class BotDatabaseTasks {
public:
	BotDatabaseTasks() = default;
	~BotDatabaseTasks();

	BotDatabaseTasks(const BotDatabaseTasks &) = delete;
	BotDatabaseTasks &operator=(const BotDatabaseTasks &) = delete;

	void execute(const std::string &query, const std::function<void(DBResult_ptr, bool)> &callback = nullptr);
	void store(const std::string &query, const std::function<void(DBResult_ptr, bool)> &callback = nullptr);

private:
	struct Job {
		std::string query;
		std::function<void(DBResult_ptr, bool)> callback;
		bool isStore = false;
	};

	// Caller must hold mutex_. Spawns the worker (and its DB connection) on
	// first use — bot systems only enqueue long after config/DB are ready.
	void ensureStartedLocked();
	void run(const std::stop_token &token);

	Database db_; // dedicated connection — touched ONLY by worker_
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Job> queue_;
	bool started_ = false;
	bool connected_ = false;
	std::jthread worker_;
};

// Exported accessor (main binary, resolved by libbot_engine.so via -rdynamic).
BotDatabaseTasks &g_botDatabaseTasks();
