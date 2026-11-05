#include <gtest/gtest.h>

#include "creatures/players/cast/cast_publisher.hpp"

#include <deque>
#include <map>

namespace {
	// Controllable persistence: writes stay pending until the test completes them,
	// in any order and with any result, standing in for a slow or failing database.
	class FakeCastTable {
	public:
		struct Pending {
			CastPublisher::Operation operation;
			std::function<void(bool)> done;
		};

		CastPublisher::Executor executor() {
			return [this](const CastPublisher::Operation &operation, std::function<void(bool)> done) {
				++writes;
				pending.push_back({ operation, std::move(done) });
			};
		}

		// Finishes the oldest pending write. On success it is applied to the table.
		void complete(bool success = true) {
			ASSERT_FALSE(pending.empty());
			auto write = std::move(pending.front());
			pending.pop_front();
			if (success) {
				if (write.operation.publish) {
					rows[write.operation.guid] = write.operation.name;
				} else {
					rows.erase(write.operation.guid);
				}
			}
			write.done(success);
		}

		CastPublisher::Scheduler scheduler() {
			return [this](uint32_t delayMs, std::function<void()> task) {
				timers.push_back({ delayMs, std::move(task) });
			};
		}

		// Fires the oldest pending retry timer.
		void fireTimer() {
			ASSERT_FALSE(timers.empty());
			auto timer = std::move(timers.front());
			timers.pop_front();
			timer.second();
		}

		void drain() {
			while (!pending.empty()) {
				complete();
			}
		}

		bool listed(uint32_t guid) const {
			return rows.contains(guid);
		}

		std::deque<Pending> pending;
		std::map<uint32_t, std::string> rows;
		std::deque<std::pair<uint32_t, std::function<void()>>> timers;
		size_t writes = 0;
	};
} // namespace

TEST(CastPublisher, StartThenStopListsAndWithdrawsTheCast) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());

	publisher.publish(7, "Caster");
	table.drain();
	EXPECT_TRUE(table.listed(7));
	EXPECT_EQ(table.rows[7], "Caster");

	publisher.unpublish(7);
	table.drain();
	EXPECT_FALSE(table.listed(7));
	EXPECT_EQ(publisher.pendingCount(), 0u);
}

TEST(CastPublisher, DelayedDeleteOfOldSessionKeepsTheNewCast) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());
	publisher.publish(7, "Caster");
	table.drain();

	// Abrupt disconnect: the DELETE is slow. The player reconnects and casts again.
	publisher.unpublish(7);
	publisher.publish(7, "Caster");

	// Only one write per character is in flight: the new UPSERT waits for the DELETE.
	ASSERT_EQ(table.pending.size(), 1u);
	EXPECT_FALSE(table.pending.front().operation.publish);
	table.complete();
	EXPECT_FALSE(table.listed(7));

	ASSERT_EQ(table.pending.size(), 1u);
	EXPECT_TRUE(table.pending.front().operation.publish);
	table.drain();
	EXPECT_TRUE(table.listed(7));
	EXPECT_EQ(publisher.pendingCount(), 0u);
}

TEST(CastPublisher, LatestRequestWinsAndIntermediateStatesAreSkipped) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());

	publisher.publish(7, "Caster");
	publisher.unpublish(7);
	publisher.publish(7, "Caster");
	publisher.unpublish(7);

	table.drain();
	EXPECT_FALSE(table.listed(7));
	// First UPSERT, then only the newest state (DELETE): the middle requests coalesce.
	EXPECT_EQ(table.writes, 2u);
	EXPECT_EQ(publisher.pendingCount(), 0u);
}

TEST(CastPublisher, RepeatedStopsNeverRemoveALaterCast) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());

	// release() and a later logout both stop, then the character casts again.
	publisher.unpublish(7);
	publisher.unpublish(7);
	publisher.publish(7, "Caster");
	table.drain();
	EXPECT_TRUE(table.listed(7));
}

TEST(CastPublisher, FailedDeleteIsRetriedWithTheNewestStateOnly) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());
	publisher.publish(7, "Caster");
	table.drain();

	publisher.unpublish(7);
	publisher.publish(7, "Caster");
	table.complete(false); // the old DELETE fails; it must not be retried after the new start

	ASSERT_EQ(table.pending.size(), 1u);
	EXPECT_TRUE(table.pending.front().operation.publish);
	table.drain();
	EXPECT_TRUE(table.listed(7));
}

TEST(CastPublisher, FailedStopIsRetriedUntilItSucceeds) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());
	publisher.publish(7, "Caster");
	table.drain();

	publisher.unpublish(7);
	table.complete(false);
	ASSERT_EQ(table.pending.size(), 1u);
	EXPECT_FALSE(table.pending.front().operation.publish);
	table.complete();
	EXPECT_FALSE(table.listed(7));
	EXPECT_EQ(publisher.pendingCount(), 0u);
}

TEST(CastPublisher, FailedDeleteIsReconciledOnceTheDatabaseIsBack) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());
	publisher.publish(7, "Caster");
	table.drain();

	// Abrupt disconnect while the database is down: every immediate attempt fails.
	publisher.unpublish(7);
	for (uint8_t attempt = 0; attempt < CastPublisher::kMaxAttempts; ++attempt) {
		ASSERT_EQ(table.pending.size(), 1u);
		table.complete(false);
	}
	// The intent is kept and retried later instead of being dropped.
	EXPECT_TRUE(table.pending.empty());
	ASSERT_EQ(table.timers.size(), 1u);
	EXPECT_EQ(table.timers.front().first, CastPublisher::kRetryBaseDelayMs);
	EXPECT_EQ(publisher.pendingCount(), 1u);
	EXPECT_TRUE(table.listed(7));

	// Still down at the first delayed retry: back off further.
	table.fireTimer();
	ASSERT_EQ(table.pending.size(), 1u);
	EXPECT_FALSE(table.pending.front().operation.publish);
	table.complete(false);
	ASSERT_EQ(table.timers.size(), 1u);
	EXPECT_EQ(table.timers.front().first, CastPublisher::kRetryBaseDelayMs * 2);

	// The database is back: the stale row goes away and nothing is left pending.
	table.fireTimer();
	table.complete();
	EXPECT_FALSE(table.listed(7));
	EXPECT_TRUE(table.timers.empty());
	EXPECT_EQ(publisher.pendingCount(), 0u);
}

TEST(CastPublisher, NewCastDuringBackoffIsWrittenAtOnceAndTheOldRetryIsDropped) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());
	publisher.publish(7, "Caster");
	table.drain();

	publisher.unpublish(7);
	for (uint8_t attempt = 0; attempt < CastPublisher::kMaxAttempts; ++attempt) {
		table.complete(false);
	}
	ASSERT_EQ(table.timers.size(), 1u);

	// Reconnect + /cast on while the old DELETE waits for its retry.
	publisher.publish(7, "Caster");
	ASSERT_EQ(table.pending.size(), 1u);
	EXPECT_TRUE(table.pending.front().operation.publish);

	// The old retry fires while the new UPSERT is still running: no second write.
	const size_t writesBefore = table.writes;
	table.fireTimer();
	EXPECT_EQ(table.writes, writesBefore);
	ASSERT_EQ(table.pending.size(), 1u);

	table.complete();
	EXPECT_TRUE(table.listed(7));
	EXPECT_TRUE(table.pending.empty());
	EXPECT_TRUE(table.timers.empty());
	EXPECT_EQ(publisher.pendingCount(), 0u);
}

TEST(CastPublisher, RetryDelayDoublesUpToTheCap) {
	EXPECT_EQ(CastPublisher::retryDelayMs(0), CastPublisher::kRetryBaseDelayMs);
	EXPECT_EQ(CastPublisher::retryDelayMs(1), CastPublisher::kRetryBaseDelayMs * 2);
	EXPECT_EQ(CastPublisher::retryDelayMs(3), CastPublisher::kRetryBaseDelayMs * 8);
	EXPECT_EQ(CastPublisher::retryDelayMs(100), CastPublisher::kRetryMaxDelayMs);
}

TEST(CastPublisher, DifferentCharactersDoNotWaitOnEachOther) {
	FakeCastTable table;
	CastPublisher publisher(table.executor(), table.scheduler());

	publisher.publish(7, "First");
	publisher.publish(8, "Second");
	EXPECT_EQ(table.pending.size(), 2u);
	table.drain();
	EXPECT_TRUE(table.listed(7));
	EXPECT_TRUE(table.listed(8));
}
