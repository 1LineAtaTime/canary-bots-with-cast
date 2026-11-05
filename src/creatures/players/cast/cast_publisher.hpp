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
// Failures: the desired state is never dropped while it is the newest one. A
// failed write is retried at once up to kMaxAttempts times in a row, then on a
// timer with backoff (kRetryBaseDelayMs doubling up to kRetryMaxDelayMs) until
// it succeeds, so a DELETE that failed while the database was down still
// withdraws the cast once the database is back. A newer request cancels the
// pending retry and is written at once; a retry only ever writes the newest state.
// Writes still queued at shutdown are lost on purpose: the startup cleanup in
// bot_manager.lua (which runs even with MASTER_DISABLE) empties the table before
// anyone can cast again.
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
	// Runs `task` on the dispatcher after `delayMs`.
	using Scheduler = std::function<void(uint32_t delayMs, std::function<void()> task)>;

	static constexpr uint8_t kMaxAttempts = 3;
	static constexpr uint32_t kRetryBaseDelayMs = 1000;
	static constexpr uint32_t kRetryMaxDelayMs = 60000;

	CastPublisher(Executor executor, Scheduler scheduler);

	CastPublisher(const CastPublisher &) = delete;
	CastPublisher &operator=(const CastPublisher &) = delete;

	static CastPublisher &getInstance();

	void publish(uint32_t guid, const std::string &name);
	void unpublish(uint32_t guid);

	// GUIDs with a write in flight or waiting for a retry.
	size_t pendingCount() const;

	// Delay before the next retry once immediate attempts are spent (0-based).
	static uint32_t retryDelayMs(uint32_t delayedRetries);

private:
	struct Entry {
		Operation desired;
		uint64_t generation = 0;
		uint64_t retryToken = 0;
		uint32_t failedAttempts = 0;
		bool inFlight = false;
		bool retryScheduled = false;
	};

	void request(Operation operation);
	void run(const Operation &operation, uint64_t generation);
	void onDone(uint32_t guid, uint64_t generation, bool success);
	void onRetry(uint32_t guid, uint64_t retryToken);

	Executor executor;
	Scheduler scheduler;
	mutable std::mutex mutex;
	uint64_t nextRetryToken = 0;
	std::unordered_map<uint32_t, Entry> entries;
};

constexpr auto g_castPublisher = CastPublisher::getInstance;
