/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "creatures/players/cast/cast_publisher.hpp"

#include "database/database.hpp"
#include "database/databasetasks.hpp"
#include "game/scheduling/dispatcher.hpp"

CastPublisher::CastPublisher(Executor executor, Scheduler scheduler) :
	executor(std::move(executor)), scheduler(std::move(scheduler)) { }

CastPublisher &CastPublisher::getInstance() {
	static CastPublisher instance(
		[](const Operation &operation, std::function<void(bool)> done) {
			std::string query;
			if (operation.publish) {
				const std::string name = g_database().escapeString(operation.name);
				query = fmt::format(
					"INSERT INTO `cast_broadcasters` (`player_id`, `player_name`) VALUES ({}, {}) "
					"ON DUPLICATE KEY UPDATE `player_name` = {}",
					operation.guid, name, name
				);
			} else {
				query = fmt::format("DELETE FROM `cast_broadcasters` WHERE `player_id` = {}", operation.guid);
			}
			// DatabaseTasks delivers the callback on the dispatcher thread.
			g_databaseTasks().execute(query, [done = std::move(done)](const DBResult_ptr &, bool success) {
				done(success);
			});
		},
		[](uint32_t delayMs, std::function<void()> task) {
			g_dispatcher().scheduleEvent(delayMs, std::move(task), "CastPublisher::retry");
		}
	);
	return instance;
}

void CastPublisher::publish(uint32_t guid, const std::string &name) {
	request({ guid, true, name });
}

void CastPublisher::unpublish(uint32_t guid) {
	request({ guid, false, {} });
}

size_t CastPublisher::pendingCount() const {
	std::scoped_lock lock(mutex);
	return entries.size();
}

uint32_t CastPublisher::retryDelayMs(uint32_t delayedRetries) {
	uint32_t delay = kRetryBaseDelayMs;
	for (uint32_t i = 0; i < delayedRetries && delay < kRetryMaxDelayMs; ++i) {
		delay *= 2;
	}
	return std::min(delay, kRetryMaxDelayMs);
}

void CastPublisher::request(Operation operation) {
	uint64_t generation = 0;
	{
		std::scoped_lock lock(mutex);
		auto &entry = entries[operation.guid];
		entry.desired = std::move(operation);
		entry.generation++;
		entry.failedAttempts = 0;
		if (entry.inFlight) {
			// onDone writes this newest state once the current write finishes.
			return;
		}
		// A retry waiting on its timer is superseded: write the new state now.
		entry.retryScheduled = false;
		entry.inFlight = true;
		operation = entry.desired;
		generation = entry.generation;
	}
	run(operation, generation);
}

void CastPublisher::run(const Operation &operation, uint64_t generation) {
	executor(operation, [this, guid = operation.guid, generation](bool success) {
		onDone(guid, generation, success);
	});
}

void CastPublisher::onDone(uint32_t guid, uint64_t generation, bool success) {
	Operation next;
	uint64_t nextGeneration = 0;
	uint32_t retryDelay = 0;
	uint64_t retryToken = 0;
	{
		std::scoped_lock lock(mutex);
		auto it = entries.find(guid);
		if (it == entries.end()) {
			return;
		}
		auto &entry = it->second;
		entry.inFlight = false;
		if (entry.generation == generation) {
			if (success) {
				entries.erase(it);
				return;
			}
			const char* kind = entry.desired.publish ? "UPSERT" : "DELETE";
			if (++entry.failedAttempts >= kMaxAttempts) {
				// Keep the intent and try again later: the database may be down for a while.
				retryDelay = retryDelayMs(entry.failedAttempts - kMaxAttempts);
				retryToken = ++nextRetryToken;
				entry.retryToken = retryToken;
				entry.retryScheduled = true;
				g_logger().error("[Cast] {} of cast_broadcasters row for player {} failed {} times, retrying in {} ms", kind, guid, entry.failedAttempts, retryDelay);
			} else {
				g_logger().warn("[Cast] {} of cast_broadcasters row for player {} failed, retrying", kind, guid);
			}
		}
		if (!entry.retryScheduled) {
			// Either an immediate retry of the same state or a newer state requested
			// meanwhile: in both cases write the newest desired state, never the stale one.
			entry.inFlight = true;
			next = entry.desired;
			nextGeneration = entry.generation;
		}
	}
	if (retryToken != 0) {
		// Outside the lock, so a scheduler that runs the task at once is safe too.
		scheduler(retryDelay, [this, guid, retryToken]() { onRetry(guid, retryToken); });
		return;
	}
	run(next, nextGeneration);
}

void CastPublisher::onRetry(uint32_t guid, uint64_t retryToken) {
	Operation next;
	uint64_t nextGeneration = 0;
	{
		std::scoped_lock lock(mutex);
		auto it = entries.find(guid);
		if (it == entries.end()) {
			return;
		}
		auto &entry = it->second;
		if (!entry.retryScheduled || entry.retryToken != retryToken || entry.inFlight) {
			// Superseded by a newer request, which was written already.
			return;
		}
		entry.retryScheduled = false;
		entry.inFlight = true;
		next = entry.desired;
		nextGeneration = entry.generation;
	}
	run(next, nextGeneration);
}
