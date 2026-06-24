/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "game/scheduling/dispatcher.hpp"

#include "lib/thread/thread_pool.hpp"
#include "lib/di/container.hpp"
#include "utils/tools.hpp"

#include <atomic>
#include <cstdlib>
#include <thread>

thread_local DispatcherContext Dispatcher::dispacherContext;

// PERF_INVESTIGATION_2026-05-24 pre-flight telemetry. Captured once when the
// dispatcher's detached task starts (see init()), then read by storeQuery /
// executeQuery to log sync DB calls firing on the dispatcher. The atomic
// holds a default-constructed thread::id until init completes; that compares
// unequal to any real thread::id, so the early-startup window is a safe no-op.
static std::atomic<std::thread::id> s_dispatcherThreadId{};

// One-time env-var check, cached. Avoids getenv() on every storeQuery / addEvent.
static bool perfTelemetryEnabled() {
	static const bool enabled = []() {
		const char* v = std::getenv("BOT_PERF_TELEMETRY");
		return v != nullptr && v[0] == '1';
	}();
	return enabled;
}

// Per-dispatcher-API call counters. Flushed every 30s from the main loop.
static std::atomic<uint64_t> s_addEventCount{0};
static std::atomic<uint64_t> s_addWalkEventCount{0};
static std::atomic<uint64_t> s_scheduleEventCount{0};
static std::atomic<uint64_t> s_asyncEventCount{0};
static std::atomic<uint64_t> s_stopEventCount{0};

#include <chrono> // monoMs (JITTER FIX 2026-06-10) — explicit, don't rely on PCH
#include "database/database.hpp" // DbDispatcherStats (bundle 4, 2026-06-11)

// JITTER FIX 2026-06-10: OTSYS_TIME() returns a value cached at the top of each
// dispatcher cycle (UPDATE_OTSYS_TIME is only called there), so any duration
// measured with it INSIDE a cycle always reads 0 — CYCLE_SLOW and DISP_SLOW were
// structurally dead instruments, and CYCLE_GAP's "between" silently included the
// whole previous cycle's work. All jitter instrumentation below now uses this
// real monotonic clock; game-logic time reads stay on the cached OTSYS_TIME().
static inline int64_t monoMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Context of the most recent task executed on the dispatcher — included in the
// CYCLE_GAP warning so a long gap names what ran right before it. Owned copy
// (Task::context is a std::string that dies with the task). Dispatcher-thread only.
static std::string s_lastDispTaskCtx = "none";

// Bundle 4 (2026-06-11): per-cycle task accounting. A slow cycle made of many
// sub-20ms tasks (e.g. hundreds of small tasks each convoying briefly on the
// shared databaseLock) previously named nothing — these counters let CYCLE_SLOW
// report how many tasks ran, the single worst one, and how much of the cycle the
// dispatcher spent inside synchronous DB calls (drained from DbDispatcherStats).
// Dispatcher-thread only.
static uint32_t s_cycleTaskCount = 0;
static int64_t s_cycleMaxTaskMs = 0;
static std::string s_cycleMaxTaskCtx = "none";

// Bundle 5 (2026-06-11): per-cycle asyncWait barrier time. The 09:16-10:13
// lagmark class showed slow cycles whose serial tasks summed to ~0ms with
// dbsync=0 while the tracer proved the dispatcher was voluntarily sleeping —
// i.e. parked in asyncWait's retFuture.wait() while the 3 pool workers ground
// through the parallel monster-AI partition on 2 cores. This counter makes
// that barrier time first-class in CYCLE_SLOW (await=). Dispatcher-thread only.
static int64_t s_cycleAsyncWaitMs = 0;

bool Dispatcher::isOnDispatcherThread() {
	return std::this_thread::get_id() == s_dispatcherThreadId.load(std::memory_order_relaxed);
}

Dispatcher &Dispatcher::getInstance() {
	return inject<Dispatcher>();
}

void Dispatcher::init() {
	UPDATE_OTSYS_TIME();

	auto dispatcherStarted = std::make_shared<std::promise<void>>();
	auto futureStarted = dispatcherStarted->get_future();

	threadPool.detach_task([this, dispatcherStarted]() mutable {
		std::unique_lock asyncLock(dummyMutex);

		// PERF_INVESTIGATION_2026-05-24 telemetry: capture our thread id so
		// Database::storeQuery / executeQuery can detect when they fire on the
		// dispatcher thread. Free regardless of telemetry env flag.
		s_dispatcherThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);

		dispatcherStarted->set_value();

		int64_t prev_cycle_end = monoMs(); // JITTER FIX: real clock (see monoMs above)
		int64_t last_intended_wait_ms = 0;
		int64_t last_perf_flush_ms = monoMs();
		while (!threadPool.isStopped()) {
			UPDATE_OTSYS_TIME();

			// PERF_INVESTIGATION_2026-05-24 telemetry flush — once per 30s, log the
			// per-API call counts. Gated by env BOT_PERF_TELEMETRY=1. Validates
			// whether the phmap btree<Task>::rebalance_or_split 26% spike is
			// driven by addEvent volume from our bot code (hypothesis was NO,
			// it's from bot_state_persistence INSERT bursts — this counter
			// closes the loop).
			if (perfTelemetryEnabled()) {
				int64_t now_ms = monoMs();
				if (now_ms - last_perf_flush_ms >= 30000) {
					double secs = static_cast<double>(now_ms - last_perf_flush_ms) / 1000.0;
					uint64_t adds = s_addEventCount.exchange(0, std::memory_order_relaxed);
					uint64_t walks = s_addWalkEventCount.exchange(0, std::memory_order_relaxed);
					uint64_t scheds = s_scheduleEventCount.exchange(0, std::memory_order_relaxed);
					uint64_t asyncs = s_asyncEventCount.exchange(0, std::memory_order_relaxed);
					uint64_t stops = s_stopEventCount.exchange(0, std::memory_order_relaxed);
					if (adds + walks + scheds + asyncs + stops > 0) {
						g_logger().info(
							"[DISP_RATES] window={:.1f}s add={}({:.1f}/s) walk={}({:.1f}/s) sched={}({:.1f}/s) async={}({:.1f}/s) stop={}({:.1f}/s)",
							secs,
							adds, adds / secs,
							walks, walks / secs,
							scheds, scheds / secs,
							asyncs, asyncs / secs,
							stops, stops / secs);
					}
					last_perf_flush_ms = now_ms;
				}
			}

			// JITTER DIAGNOSTIC: catch the GAP between two consecutive dispatcher
			// cycles. If the wait_for at the bottom of the previous iteration
			// returned late (clock drift, condvar spurious wait, OS preemption,
			// timeUntilNextScheduledTask returning a too-large value), the gap
			// shows up here even though no single iteration's work is slow.
			// JITTER FIX 2026-06-10: monoMs() (real clock) instead of the cached
			// OTSYS_TIME — these four reads used to return the SAME value, freezing
			// CYCLE_SLOW at 0 forever and folding cycle work into CYCLE_GAP.
			int64_t cyc_t0 = monoMs();
			int64_t between_cycles = cyc_t0 - prev_cycle_end;

			// Bundle 4: reset per-cycle task accounting (filled by executeSerialEvents
			// / executeScheduledEvents below).
			s_cycleTaskCount = 0;
			s_cycleMaxTaskMs = 0;
			s_cycleMaxTaskCtx.assign("none");
			s_cycleAsyncWaitMs = 0;

			executeEvents();
			int64_t cyc_t1 = monoMs();
			executeScheduledEvents();
			int64_t cyc_t2 = monoMs();
			mergeEvents();
			int64_t cyc_t3 = monoMs();
			prev_cycle_end = cyc_t3;

			int64_t cyc_total = cyc_t3 - cyc_t0;
			// Bundle 4: drain the dispatcher's sync-DB time for THIS cycle (resets the
			// counter for the next one) — must drain every cycle, logged or not.
			int64_t cyc_dbsync_ms = DbDispatcherStats::fetchResetSyncDbUs() / 1000;
			if (cyc_total > 10) {
				g_logger().warn("[CYCLE_SLOW] total={}ms exec={}ms sched={}ms merge={}ms gap={}ms tasks={} max={}ms:{} dbsync={}ms await={}ms",
					cyc_total, (cyc_t1 - cyc_t0), (cyc_t2 - cyc_t1), (cyc_t3 - cyc_t2),
					between_cycles, s_cycleTaskCount, s_cycleMaxTaskMs, s_cycleMaxTaskCtx,
					cyc_dbsync_ms, s_cycleAsyncWaitMs);
			}
			// Independently log the inter-cycle gap (typically ~0-100ms idle wait):
			// >400ms means the dispatcher thread was effectively asleep too long.
			// Compare `between` (actual elapsed) to `intended` (what we asked wait_for
			// to wait): if actual >> intended the condvar overshot; if intended is large
			// the head of scheduledTasks was wrong.
			// 2026-05-26: raised threshold 150ms → 400ms after PERF_INVESTIGATION
			// confirmed 150-400ms range is pure LXC scheduler noise floor that doesn't
			// correlate with user-visible lag (matches GAP_SLOW raise 200ms → 500ms).
			if (between_cycles > 400) {
				g_logger().warn("[CYCLE_GAP] between={}ms intended={}ms last_task={}",
					between_cycles, last_intended_wait_ms, s_lastDispTaskCtx);
			}

			// JITTER DIAGNOSTIC: record what we INTENDED to wait. The next iteration
			// compares this to the actual elapsed time (CYCLE_GAP) — if actual > intended
			// the wait_for is overshooting (condvar issue / OS preemption), if
			// actual ≈ intended but intended is too large, timeUntilNextScheduledTask
			// is returning a wrong value (scheduledTasks head not being kept fresh).
			if (!hasPendingTasks) {
				auto intended = timeUntilNextScheduledTask();
				int64_t intended_ms = intended.count() > 60000 ? 60000 : intended.count();
				last_intended_wait_ms = intended_ms;
				signalSchedule.wait_for(asyncLock, intended);
			} else {
				last_intended_wait_ms = 0;
			}
		}
	});

	if (futureStarted.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
		throw std::logic_error("Failed to initialize dispatcher: timeout waiting for thread start");
	}
}

void Dispatcher::executeSerialEvents(const uint8_t groupId) {
	auto &tasks = m_tasks[groupId];
	if (tasks.empty()) {
		return;
	}

	dispacherContext.group = static_cast<TaskGroup>(groupId);
	dispacherContext.type = DispatcherType::Event;

	for (const auto &task : tasks) {
		dispacherContext.taskName = task.getContext();
		s_lastDispTaskCtx.assign(task.getContext());
		// JITTER DIAGNOSTIC: time each task (real clock — see monoMs). >20ms is logged.
		int64_t disp_start = monoMs();
		bool disp_ok = task.execute();
		int64_t disp_dur = monoMs() - disp_start;
		s_cycleTaskCount++;
		if (disp_dur > s_cycleMaxTaskMs) {
			s_cycleMaxTaskMs = disp_dur;
			s_cycleMaxTaskCtx.assign(task.getContext());
		}
		if (disp_dur > 20) {
			g_logger().warn("[DISP_SLOW] kind=serial name={} duration={}ms",
				task.getContext(), disp_dur);
		}
		if (disp_ok) {
			++dispatcherCycle;
		}
	}
	tasks.clear();

	dispacherContext.reset();
}

void Dispatcher::executeParallelEvents(const uint8_t groupId) {
	auto &tasks = m_tasks[groupId];
	if (tasks.empty()) {
		return;
	}

	asyncWait(tasks.size(), [groupId, &tasks](size_t i) {
		dispacherContext.type = DispatcherType::AsyncEvent;
		dispacherContext.group = static_cast<TaskGroup>(groupId);
		tasks[i].execute();

		dispacherContext.reset();
	});

	tasks.clear();
}

void Dispatcher::asyncWait(size_t requestSize, std::function<void(size_t i)> &&f) {
	if (requestSize == 0) {
		return;
	}

	// This prevents an async call from running inside another async call.
	if (asyncWaitDisabled) {
		for (uint_fast64_t i = 0; i < requestSize; ++i) {
			f(i);
		}
		return;
	}

	// Bundle 5 (2026-06-11): time the fork-join (local partition + retFuture.wait
	// on the pool workers). Accumulated into CYCLE_SLOW await=; big barriers also
	// log standalone so they are visible even without a slow cycle.
	const bool aw_timed = isOnDispatcherThread();
	const int64_t aw_start = aw_timed ? monoMs() : 0;

	const auto &partitions = generatePartition(requestSize);
	const auto pSize = partitions.size();

	BS::multi_future<void> retFuture;

	if (pSize > 1) {
		asyncWaitDisabled = true;
		const auto min = partitions[1].first;
		const auto max = partitions[partitions.size() - 1].second;
		retFuture = threadPool.submit_loop(min, max, [&f](const unsigned int i) { f(i); });
	}

	const auto &[min, max] = partitions[0];
	for (uint_fast64_t i = min; i < max; ++i) {
		f(i);
	}

	if (pSize > 1) {
		retFuture.wait();
		asyncWaitDisabled = false;
	}

	if (aw_timed) {
		const int64_t aw_wall = monoMs() - aw_start;
		s_cycleAsyncWaitMs += aw_wall;
		if (aw_wall > 100) {
			g_logger().warn("[ASYNC_WAIT] n={} wall={}ms", requestSize, aw_wall);
		}
	}
}

void Dispatcher::executeEvents(const TaskGroup startGroup) {
	for (uint_fast8_t groupId = static_cast<uint8_t>(startGroup); groupId < static_cast<uint8_t>(TaskGroup::Last); ++groupId) {
		const auto isWalk = groupId == static_cast<uint8_t>(TaskGroup::Walk);

		if (groupId == static_cast<uint8_t>(TaskGroup::Serial) || isWalk) {
			mergeEvents();
			executeSerialEvents(groupId);
			mergeAsyncEvents();
		} else {
			executeParallelEvents(groupId);
		}
	}
}

void Dispatcher::executeScheduledEvents() {
	// Stop firing scheduled / cycle events once shutdown begins.
	//
	// PR #3527 (upstream, May 2025) added the `shuttingDown` flag and gated
	// all enqueue paths on it (addEvent/addWalkEvent/scheduleEvent/asyncEvent
	// at lines 223/234/245/261). That fixes NEW enqueues but leaves the
	// queued-task race: lambdas that captured `[this]` raw (most notably
	// SpawnMonster::checkSpawnMonster at spawn_monster.cpp:166-170 and
	// SpawnNpc::checkSpawnNpc at spawn_npc.cpp:249-260) keep firing after
	// Game::shutdown() runs map.spawnsMonster.clear() — which destroys the
	// SpawnMonster/SpawnNpc instances. The lambda's `this` becomes dangling
	// and the next access (spawnedMonsterMap.contains(...) at
	// spawn_monster.cpp:212) hits glibc's "corrupted double-linked list"
	// detection in the freed Rb_tree → SIGABRT. This crash hit ~13 of 14
	// recent shutdowns including the daily 06:00 globalServerSave restart.
	//
	// Combined with Game::shutdown() now calling g_dispatcher().shutdown()
	// at its top (game.cpp), this guard closes the window: by the time
	// map.spawnsMonster.clear() executes, no further scheduled lambdas can
	// fire on the soon-to-be-destroyed objects.
	if (shuttingDown) {
		return;
	}

	auto &threadScheduledTasks = getThreadTask()->scheduledTasks;

	auto it = scheduledTasks.begin();
	while (it != scheduledTasks.end()) {
		const auto &task = *it;
		if (task->getTime() > OTSYS_TIME()) {
			break;
		}

		dispacherContext.type = task->isCycle() ? DispatcherType::CycleEvent : DispatcherType::ScheduledEvent;
		dispacherContext.group = TaskGroup::Serial;
		dispacherContext.taskName = task->getContext();

		s_lastDispTaskCtx.assign(task->getContext());
		// JITTER DIAGNOSTIC: time each scheduled/cycle task (real clock). >20ms is logged.
		int64_t disp_start = monoMs();
		bool disp_ok = task->execute();
		int64_t disp_dur = monoMs() - disp_start;
		s_cycleTaskCount++;
		if (disp_dur > s_cycleMaxTaskMs) {
			s_cycleMaxTaskMs = disp_dur;
			s_cycleMaxTaskCtx.assign(task->getContext());
		}
		if (disp_dur > 20) {
			const char* kind = task->isCycle() ? "cycle" : "scheduled";
			g_logger().warn("[DISP_SLOW] kind={} name={} duration={}ms",
				kind, task->getContext(), disp_dur);
		}
		if (disp_ok && task->isCycle()) {
			task->updateTime();
			threadScheduledTasks.emplace_back(task);
		} else {
			scheduledTasksRef.erase(task->getId());
		}

		++it;
	}

	if (it != scheduledTasks.begin()) {
		scheduledTasks.erase(scheduledTasks.begin(), it);
	}

	dispacherContext.reset();

	mergeAsyncEvents(); // merge async events requested by scheduled events
	executeEvents(TaskGroup::GenericParallel); // execute async events requested by scheduled events
}

void Dispatcher::__mergeEvents(const std::array<uint8_t, 2> &groups, const bool mergeScheduledEvents) {
	for (const auto &thread : threads) {
		std::scoped_lock lock(thread->mutex);
		for (const auto group : groups) {
			auto &threadTasks = thread->tasks[group];
			auto &tasks = m_tasks[group];

			if (threadTasks.size() > tasks.size()) {
				tasks.swap(threadTasks);
			}

			if (!threadTasks.empty()) {
				tasks.insert(tasks.end(), make_move_iterator(threadTasks.begin()), make_move_iterator(threadTasks.end()));
				threadTasks.clear();
			}
		}

		if (mergeScheduledEvents && !thread->scheduledTasks.empty()) {
			scheduledTasks.insert(make_move_iterator(thread->scheduledTasks.begin()), make_move_iterator(thread->scheduledTasks.end()));
			thread->scheduledTasks.clear();
		}
	}
}

// Merge only async thread events with main dispatch events
void Dispatcher::mergeAsyncEvents() {
	static constexpr auto groups = std::to_array({ static_cast<uint8_t>(TaskGroup::WalkParallel), static_cast<uint8_t>(TaskGroup::GenericParallel) });
	__mergeEvents(groups, false);
}

// Merge thread events with main dispatch events
void Dispatcher::mergeEvents() {
	static constexpr auto groups = std::to_array({ static_cast<uint8_t>(TaskGroup::Walk), static_cast<uint8_t>(TaskGroup::Serial) });
	__mergeEvents(groups, true);
	checkPendingTasks();
}

std::chrono::milliseconds Dispatcher::timeUntilNextScheduledTask() const {
	constexpr auto CHRONO_0 = std::chrono::milliseconds(0);
	constexpr auto CHRONO_MILI_MAX = std::chrono::milliseconds::max();

	if (scheduledTasks.empty()) {
		return CHRONO_MILI_MAX;
	}

	const auto &task = *scheduledTasks.begin();
	const auto timeRemaining = std::chrono::milliseconds(task->getTime() - OTSYS_TIME());
	return std::max<std::chrono::milliseconds>(timeRemaining, CHRONO_0);
}

void Dispatcher::addEvent(std::function<void(void)> &&f, std::string_view context, uint32_t expiresAfterMs) {
	if (shuttingDown) {
		return;
	}

	s_addEventCount.fetch_add(1, std::memory_order_relaxed); // PERF telemetry

	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);
	thread->tasks[static_cast<uint8_t>(TaskGroup::Serial)].emplace_back(expiresAfterMs, std::move(f), context);
	notify();
}

void Dispatcher::addWalkEvent(std::function<void(void)> &&f, uint32_t expiresAfterMs) {
	if (shuttingDown) {
		return;
	}

	s_addWalkEventCount.fetch_add(1, std::memory_order_relaxed); // PERF telemetry

	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);
	thread->tasks[static_cast<uint8_t>(TaskGroup::Walk)].emplace_back(expiresAfterMs, std::move(f), this->context().taskName);
	notify();
}

uint64_t Dispatcher::scheduleEvent(const std::shared_ptr<Task> &task) {
	if (shuttingDown) {
		return 0;
	}

	s_scheduleEventCount.fetch_add(1, std::memory_order_relaxed); // PERF telemetry

	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);

	auto eventId = scheduledTasksRef
					   .emplace(task->getId(), thread->scheduledTasks.emplace_back(task))
					   .first->first;

	notify();
	return eventId;
}

void Dispatcher::asyncEvent(std::function<void(void)> &&f, TaskGroup group) {
	if (shuttingDown) {
		return;
	}

	s_asyncEventCount.fetch_add(1, std::memory_order_relaxed); // PERF telemetry

	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);
	thread->tasks[static_cast<uint8_t>(group)].emplace_back(0, std::move(f), dispacherContext.taskName);
	notify();
}

void Dispatcher::stopEvent(uint64_t eventId) {
	s_stopEventCount.fetch_add(1, std::memory_order_relaxed); // PERF telemetry

	auto it = scheduledTasksRef.find(eventId);
	if (it != scheduledTasksRef.end()) {
		it->second->cancel();
		scheduledTasksRef.erase(it);
	}
}

void Dispatcher::safeCall(std::function<void(void)> &&f) {
	if (dispacherContext.isAsync()) {
		addEvent(std::move(f), dispacherContext.taskName);
	} else {
		f();
	}
}

bool DispatcherContext::isOn() {
	return OTSYS_TIME() != 0;
}
