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

CastPublisher::CastPublisher(Executor executor) :
	executor(std::move(executor)) { }

CastPublisher &CastPublisher::getInstance() {
	static CastPublisher instance([](const Operation &operation, std::function<void(bool)> done) {
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
	});
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
	{
		std::scoped_lock lock(mutex);
		auto it = entries.find(guid);
		if (it == entries.end()) {
			return;
		}
		auto &entry = it->second;
		if (entry.generation == generation) {
			if (success) {
				entries.erase(it);
				return;
			}
			if (++entry.failedAttempts >= kMaxAttempts) {
				g_logger().error("[Cast] Giving up {} of cast_broadcasters row for player {} after {} failed attempts",
					entry.desired.publish ? "UPSERT" : "DELETE", guid, entry.failedAttempts);
				entries.erase(it);
				return;
			}
			g_logger().warn("[Cast] {} of cast_broadcasters row for player {} failed, retrying",
				entry.desired.publish ? "UPSERT" : "DELETE", guid);
		}
		// Either a retry of the same state or a newer state requested meanwhile:
		// in both cases write the newest desired state, never the stale one.
		next = entry.desired;
		nextGeneration = entry.generation;
	}
	run(next, nextGeneration);
}
