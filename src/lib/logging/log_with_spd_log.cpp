/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "lib/di/container.hpp"

LogWithSpdLog::LogWithSpdLog() {
	// Async thread pool: 8192 queue slots, 1 background writer thread
	spdlog::init_thread_pool(8192, 1);

	auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	auto async_logger = std::make_shared<spdlog::async_logger>(
		"async_default",
		stdout_sink,
		spdlog::thread_pool(),
		spdlog::async_overflow_policy::overrun_oldest
	);
	spdlog::set_default_logger(async_logger);

	setLevel("info");
	spdlog::set_pattern("[%Y-%d-%m %H:%M:%S.%e] [%^%l%$] %v ");
	spdlog::flush_on(spdlog::level::warn);
	spdlog::flush_every(std::chrono::seconds(3));

#ifdef DEBUG_LOG
	spdlog::set_pattern("[%Y-%d-%m %H:%M:%S.%e] [thread %t] [%^%l%$] %v ");
#endif
}

Logger &LogWithSpdLog::getInstance() {
	return inject<Logger>();
}

void LogWithSpdLog::setLevel(const std::string &name) const {
	debug("Setting log level to: {}.", name);
	const auto level = spdlog::level::from_str(name);
	spdlog::set_level(level);
}

std::string LogWithSpdLog::getLevel() const {
	const auto level = spdlog::level::to_string_view(spdlog::get_level());
	return std::string { level.begin(), level.end() };
}

void LogWithSpdLog::info(const std::string &msg) const {
	SPDLOG_INFO(msg);
}

void LogWithSpdLog::warn(const std::string &msg) const {
	SPDLOG_WARN(msg);
}

void LogWithSpdLog::error(const std::string &msg) const {
	SPDLOG_ERROR(msg);
}

void LogWithSpdLog::critical(const std::string &msg) const {
	SPDLOG_CRITICAL(msg);
}

#if defined(DEBUG_LOG)
void LogWithSpdLog::debug(const std::string &msg) const {
	SPDLOG_DEBUG(msg);
}

void LogWithSpdLog::trace(const std::string &msg) const {
	SPDLOG_TRACE(msg);
}
#endif
