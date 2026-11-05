/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

// Keeps the `cast_broadcasters` row of a HUMAN caster in step with
// Player::setCastBroadcasting, which is the only caller (issue #13). Before this,
// the row was written by cast.lua / logout.lua / death.lua, so an abrupt
// disconnect (ProtocolGame::release clears the flag) left a dead cast listed by
// deployment/web/login.php until the next startup.
//
// Ordering: DatabaseTasks runs queries on the thread pool with no per-character
// order, so UPSERT and DELETE of the same GUID both go through this queue. At most
// one write per GUID is in flight; requests made meanwhile only replace the
// desired state, and the newest one is written when the current write finishes.
// A late DELETE of an old session therefore can never be the last write after a
// newer `/cast on`: the newer UPSERT always runs after it. Different GUIDs never
// wait on each other and the dispatcher never waits on SQL.
//
// Failures: a write that fails is retried with the newest desired state, up to
// kMaxAttempts in a row, then logged and dropped (a newer request starts over).
// Writes still queued at shutdown are lost on purpose: the startup cleanup in
// bot_manager.lua empties the table before anyone can cast again.
//
// Bots keep their own cast_broadcasters path in the bot engine and are never
// sent here.
class CastPublisher {
public:
	struct Operation {
		uint32_t guid = 0;
		bool publish = false; // true: UPSERT the row, false: DELETE it
		std::string name;
	};

	// Applies `operation` off the dispatcher and calls `done(success)` exactly once.
	using Executor = std::function<void(const Operation &operation, std::function<void(bool)> done)>;

	static constexpr uint8_t kMaxAttempts = 3;

	explicit CastPublisher(Executor executor);

	CastPublisher(const CastPublisher &) = delete;
	CastPublisher &operator=(const CastPublisher &) = delete;

	static CastPublisher &getInstance();

	void publish(uint32_t guid, const std::string &name);
	void unpublish(uint32_t guid);

	// GUIDs with a write queued or in flight.
	size_t pendingCount() const;

private:
	struct Entry {
		Operation desired;
		uint64_t generation = 0;
		uint8_t failedAttempts = 0;
		bool inFlight = false;
	};

	void request(Operation operation);
	void run(const Operation &operation, uint64_t generation);
	void onDone(uint32_t guid, uint64_t generation, bool success);

	Executor executor;
	mutable std::mutex mutex;
	std::unordered_map<uint32_t, Entry> entries;
};

constexpr auto g_castPublisher = CastPublisher::getInstance;
