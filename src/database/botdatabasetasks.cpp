/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "database/botdatabasetasks.hpp"

#include "game/scheduling/dispatcher.hpp"

BotDatabaseTasks &g_botDatabaseTasks() {
	static BotDatabaseTasks instance;
	return instance;
}

BotDatabaseTasks::~BotDatabaseTasks() {
	if (worker_.joinable()) {
		worker_.request_stop();
		cv_.notify_all();
	}
	// std::jthread joins in its destructor.
}

void BotDatabaseTasks::ensureStartedLocked() {
	if (started_) {
		return;
	}
	started_ = true;
	worker_ = std::jthread([this](const std::stop_token &token) { run(token); });
}

void BotDatabaseTasks::execute(const std::string &query, const std::function<void(DBResult_ptr, bool)> &callback /* nullptr */) {
	{
		std::scoped_lock lock(mutex_);
		ensureStartedLocked();
		queue_.push_back(Job { query, callback, false });
	}
	cv_.notify_one();
}

void BotDatabaseTasks::store(const std::string &query, const std::function<void(DBResult_ptr, bool)> &callback /* nullptr */) {
	{
		std::scoped_lock lock(mutex_);
		ensureStartedLocked();
		queue_.push_back(Job { query, callback, true });
	}
	cv_.notify_one();
}

void BotDatabaseTasks::run(const std::stop_token &token) {
	// Connect on the worker thread with the same credentials as the main
	// connection. Config is loaded long before bot systems enqueue anything.
	connected_ = db_.connect();
	if (!connected_) {
		g_logger().error("[BotDatabaseTasks] dedicated DB connection FAILED — bot async queries will be dropped (check MySQL max_connections)");
	} else {
		g_logger().info("[BotDatabaseTasks] dedicated bot-DB worker online (own connection, off the shared pool)");
	}

	while (true) {
		Job job;
		{
			std::unique_lock lock(mutex_);
			cv_.wait(lock, [&] { return token.stop_requested() || !queue_.empty(); });
			if (token.stop_requested()) {
				return; // process teardown — drop any remainder, mirrors DatabaseTasks pool teardown
			}
			job = std::move(queue_.front());
			queue_.pop_front();
		}

		if (!connected_) {
			continue;
		}

		if (job.isStore) {
			DBResult_ptr result = db_.storeQuery(job.query);
			if (job.callback != nullptr) {
				g_dispatcher().addEvent([callback = std::move(job.callback), result]() { callback(result, true); }, "BotDatabaseTasks::store");
			}
		} else {
			bool success = db_.executeQuery(job.query);
			if (job.callback != nullptr) {
				g_dispatcher().addEvent([callback = std::move(job.callback), success]() { callback(nullptr, success); }, "BotDatabaseTasks::execute");
			}
		}
	}
}
