/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// BOT_TEST_HARNESS Phase 2 — synchronous NDJSON audit sink. See bot_audit_sink.hpp for why
// this is in the main binary and how libbot_engine.so reaches it.

#include "creatures/players/bot/bot_audit_sink.hpp"

#include "config/configmanager.hpp"
#include "lib/logging/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <vector>

namespace {
	std::string audLowerTrim(std::string_view in) {
		size_t b = 0;
		size_t e = in.size();
		while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) {
			++b;
		}
		while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) {
			--e;
		}
		std::string out(in.substr(b, e - b));
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	std::string audTrim(std::string_view in) {
		size_t b = 0;
		size_t e = in.size();
		while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) {
			++b;
		}
		while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) {
			--e;
		}
		return std::string(in.substr(b, e - b));
	}

	std::vector<std::string> audSplitCsv(std::string_view csv) {
		std::vector<std::string> out;
		size_t start = 0;
		while (start <= csv.size()) {
			size_t comma = csv.find(',', start);
			if (comma == std::string_view::npos) {
				comma = csv.size();
			}
			std::string tok = audTrim(csv.substr(start, comma - start));
			if (!tok.empty()) {
				out.push_back(std::move(tok));
			}
			start = comma + 1;
		}
		return out;
	}
}

struct BotAuditSink::Impl {
	mutable std::mutex mu;

	// settings (guarded by mu; `on` and `gen` are also readable lock-free)
	std::atomic<bool> on { false };
	std::atomic<uint32_t> gen { 0 };
	std::atomic<bool> configured { false };
	bool allKinds = true;
	std::set<std::string> kindSet;   // lowercase
	std::vector<std::string> watch; // display case, unique case-insensitively
	std::string path = "logs/bot_audit.ndjson";
	uint64_t maxBytes = 32ull * 1024 * 1024;
	int32_t probeMs = 500;
	int32_t censusMs = 10000;

	// file
	std::FILE* fh = nullptr;
	std::string openPath;
	uint64_t bytes = 0;

	// counters
	std::atomic<uint64_t> seq { 0 };
	uint64_t linesWritten = 0;
	uint64_t bytesWritten = 0;
	uint64_t rotations = 0;
	uint64_t writeErrors = 0;
	int64_t lastErrorLogMs = 0;
	std::string lastError;
	int64_t nextOpenAttemptMs = 0;   // failed-open back-off: no syscalls per flush while broken
	uint32_t slowStreak = 0;         // consecutive writes slower than SLOW_WRITE_MS
	std::string autoDisabled;        // non-empty = the kill switch fired (reason)
	static constexpr int64_t OPEN_RETRY_MS = 5000;
	static constexpr int64_t SLOW_WRITE_MS = 50;
	static constexpr uint32_t SLOW_WRITE_STREAK = 5;

	void bump() {
		gen.fetch_add(1, std::memory_order_acq_rel);
	}

	void closeLocked() {
		if (fh) {
			std::fflush(fh);
			std::fclose(fh);
			fh = nullptr;
		}
		openPath.clear();
		bytes = 0;
	}

	void noteErrorLocked(const std::string &err) {
		lastError = err;
		writeErrors++;
		const int64_t now = BotAuditSink::epochMs();
		if (now - lastErrorLogMs > 60000) {
			lastErrorLogMs = now;
			g_logger().error("[BOT_AUDIT] {} (path='{}'); records are being dropped", err, path);
		}
	}

	bool openLocked() {
		if (fh && openPath == path) {
			return true;
		}
		closeLocked();
		const int64_t now = BotAuditSink::epochMs();
		if (now < nextOpenAttemptMs) {
			return false; // back-off after a failed open: no mkdir/fopen syscalls every flush
		}
		std::error_code ec;
		const std::filesystem::path p(path);
		if (p.has_parent_path()) {
			std::filesystem::create_directories(p.parent_path(), ec);
		}
		fh = std::fopen(path.c_str(), "ab");
		if (!fh) {
			nextOpenAttemptMs = now + OPEN_RETRY_MS;
			noteErrorLocked("cannot open audit file");
			return false;
		}
		nextOpenAttemptMs = 0;
		const auto sz = std::filesystem::file_size(p, ec);
		bytes = ec ? 0 : static_cast<uint64_t>(sz);
		openPath = path;
		return true;
	}

	// Rotate <path> -> <path>.1 (overwriting the previous .1). Keeps disk use <= ~2x maxBytes.
	void rotateLocked() {
		closeLocked();
		std::error_code ec;
		std::filesystem::rename(path, path + ".1", ec);
		if (ec) {
			// Could not rotate (e.g. permissions): truncate instead of growing without bound.
			std::FILE* t = std::fopen(path.c_str(), "wb");
			if (t) {
				std::fclose(t);
			}
		}
		rotations++;
		openLocked();
	}
};

BotAuditSink::BotAuditSink() :
	impl_(std::make_unique<Impl>()) { }

BotAuditSink::~BotAuditSink() = default;

BotAuditSink &BotAuditSink::get() {
	// Intentionally immortal (never destroyed). The engine's destructor flushes into the sink
	// during BotEngineLoader's own static teardown; a function-local static could already have
	// been destroyed by then. Every write() fflush()es, so nothing is lost at process exit.
	static BotAuditSink* instance = new BotAuditSink();
	return *instance;
}

int64_t BotAuditSink::epochMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch()
	)
		.count();
}

void BotAuditSink::appendJsonString(std::string &out, std::string_view s) {
	static constexpr char hex[] = "0123456789abcdef";
	out.push_back('"');
	for (const char ch : s) {
		const auto c = static_cast<unsigned char>(ch);
		switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (c < 0x20) {
					out += "\\u00";
					out.push_back(hex[c >> 4]);
					out.push_back(hex[c & 0xF]);
				} else {
					out.push_back(ch);
				}
		}
	}
	out.push_back('"');
}

std::string_view BotAuditSink::utf8Prefix(std::string_view s, size_t maxBytes) {
	if (s.size() <= maxBytes) {
		return s;
	}
	size_t n = maxBytes;
	// s[n] is the first byte NOT kept: while it is a continuation byte (10xxxxxx) the cut would land
	// inside a character, so back up to that character's lead byte and drop it whole.
	while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
		--n;
	}
	return s.substr(0, n);
}

void BotAuditSink::ensureConfigured() {
	if (!impl_->configured.load(std::memory_order_acquire)) {
		applyConfig();
	}
}

void BotAuditSink::applyConfig() {
	const bool on = g_configManager().getBoolean(BOT_AUDIT_ENABLED);
	std::string path = g_configManager().getString(BOT_AUDIT_PATH);
	const std::string kinds = g_configManager().getString(BOT_AUDIT_KINDS);
	const std::string watch = g_configManager().getString(BOT_AUDIT_WATCH);
	const int32_t maxMb = std::clamp(g_configManager().getNumber(BOT_AUDIT_MAX_MB), 1, 4096);
	const int32_t probeMs = std::clamp(g_configManager().getNumber(BOT_AUDIT_PROBE_MS), 100, 60000);
	const int32_t censusSec = std::clamp(g_configManager().getNumber(BOT_AUDIT_CENSUS_SEC), 1, 3600);
	if (path.empty()) {
		path = "logs/bot_audit.ndjson";
	}
	{
		std::scoped_lock lock(impl_->mu);
		if (path != impl_->path) {
			impl_->closeLocked();
			impl_->path = path;
		}
		impl_->maxBytes = static_cast<uint64_t>(maxMb) * 1024 * 1024;
		impl_->probeMs = probeMs;
		impl_->censusMs = censusSec * 1000;
		impl_->on.store(on, std::memory_order_release);
		impl_->closeLocked(); // reopen lazily (path may have changed); also clears any back-off
		impl_->nextOpenAttemptMs = 0;
		impl_->slowStreak = 0;
		impl_->autoDisabled.clear();
		impl_->configured.store(true, std::memory_order_release);
	}
	setKinds(kinds);
	setWatch(watch);
	impl_->bump();
	g_logger().info("[BOT_AUDIT] config applied: enabled={} path='{}' kinds='{}' watch='{}' maxMb={} probeMs={} censusSec={}",
	                on, path, this->kinds(), this->watch(), maxMb, probeMs, censusSec);
}

uint32_t BotAuditSink::generation() {
	ensureConfigured();
	return impl_->gen.load(std::memory_order_acquire);
}

bool BotAuditSink::enabled() const {
	return impl_->on.load(std::memory_order_acquire);
}

void BotAuditSink::setEnabled(bool on) {
	ensureConfigured();
	{
		std::scoped_lock lock(impl_->mu);
		impl_->on.store(on, std::memory_order_release);
		if (!on) {
			impl_->closeLocked();
		} else {
			// an explicit `on` re-arms the kill switch and retries a failed open immediately
			impl_->slowStreak = 0;
			impl_->autoDisabled.clear();
			impl_->nextOpenAttemptMs = 0;
		}
	}
	impl_->bump();
}

std::string BotAuditSink::kinds() const {
	std::scoped_lock lock(impl_->mu);
	if (impl_->allKinds) {
		return "all";
	}
	std::string out;
	for (const auto &k : impl_->kindSet) {
		if (!out.empty()) {
			out += ',';
		}
		out += k;
	}
	return out;
}

void BotAuditSink::setKinds(const std::string &csv) {
	ensureConfigured();
	{
		std::scoped_lock lock(impl_->mu);
		impl_->kindSet.clear();
		impl_->allKinds = false;
		for (const auto &tok : audSplitCsv(csv)) {
			const std::string k = audLowerTrim(tok);
			if (k == "all" || k == "*") {
				impl_->allKinds = true;
			} else if (!k.empty()) {
				impl_->kindSet.insert(k);
			}
		}
		// An empty list means "nothing selected", which is legal (audit on, no kinds = silent).
	}
	impl_->bump();
}

bool BotAuditSink::wants(std::string_view kind) const {
	if (!impl_->on.load(std::memory_order_acquire)) {
		return false;
	}
	std::scoped_lock lock(impl_->mu);
	return impl_->allKinds || impl_->kindSet.contains(std::string(kind));
}

std::string BotAuditSink::watch() const {
	std::scoped_lock lock(impl_->mu);
	std::string out;
	for (const auto &w : impl_->watch) {
		if (!out.empty()) {
			out += ',';
		}
		out += w;
	}
	return out;
}

void BotAuditSink::setWatch(const std::string &csv) {
	ensureConfigured();
	{
		std::scoped_lock lock(impl_->mu);
		impl_->watch.clear();
	}
	for (const auto &tok : audSplitCsv(csv)) {
		addWatch(tok);
	}
	impl_->bump();
}

void BotAuditSink::addWatch(const std::string &name) {
	ensureConfigured();
	const std::string n = audTrim(name);
	if (n.empty()) {
		return;
	}
	{
		std::scoped_lock lock(impl_->mu);
		const std::string key = audLowerTrim(n);
		for (const auto &w : impl_->watch) {
			if (audLowerTrim(w) == key) {
				return;
			}
		}
		impl_->watch.push_back(n);
	}
	impl_->bump();
}

bool BotAuditSink::removeWatch(const std::string &name) {
	ensureConfigured();
	bool removed = false;
	{
		std::scoped_lock lock(impl_->mu);
		const std::string key = audLowerTrim(name);
		const auto before = impl_->watch.size();
		std::erase_if(impl_->watch, [&key](const std::string &w) { return audLowerTrim(w) == key; });
		removed = impl_->watch.size() != before;
	}
	if (removed) {
		impl_->bump();
	}
	return removed;
}

int32_t BotAuditSink::probeMs() const {
	std::scoped_lock lock(impl_->mu);
	return impl_->probeMs;
}

int32_t BotAuditSink::censusMs() const {
	std::scoped_lock lock(impl_->mu);
	return impl_->censusMs;
}

uint64_t BotAuditSink::nextSeq() {
	return impl_->seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

void BotAuditSink::write(std::string_view lines) {
	if (lines.empty() || !impl_->on.load(std::memory_order_acquire)) {
		return;
	}
	std::scoped_lock lock(impl_->mu);
	if (!impl_->openLocked()) {
		return;
	}
	if (impl_->bytes > 0 && impl_->bytes + lines.size() > impl_->maxBytes) {
		impl_->rotateLocked();
		if (!impl_->fh) {
			return;
		}
	}
	const auto t0 = std::chrono::steady_clock::now();
	const size_t n = std::fwrite(lines.data(), 1, lines.size(), impl_->fh);
	std::fflush(impl_->fh);
	const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	if (n != lines.size()) {
		impl_->noteErrorLocked("short write");
	}
	impl_->bytes += n;
	impl_->bytesWritten += n;
	impl_->linesWritten += static_cast<uint64_t>(std::count(lines.begin(), lines.end(), '\n'));

	// Kill switch: this write runs on the dispatcher. A stalling disk must cost at most a few slow
	// ticks, never a sustained freeze — after SLOW_WRITE_STREAK consecutive slow writes, turn the
	// audit off (one warning; `/cavebot audit on` re-arms it).
	impl_->slowStreak = tookMs > Impl::SLOW_WRITE_MS ? impl_->slowStreak + 1 : 0;
	if (impl_->slowStreak >= Impl::SLOW_WRITE_STREAK) {
		impl_->autoDisabled = std::to_string(impl_->slowStreak) + " consecutive writes > "
			+ std::to_string(Impl::SLOW_WRITE_MS) + " ms (last " + std::to_string(tookMs) + " ms)";
		impl_->on.store(false, std::memory_order_release);
		impl_->closeLocked();
		impl_->slowStreak = 0;
		impl_->bump();
		g_logger().warn("[BOT_AUDIT] auto-disabled: {} (path='{}'). Re-enable with /cavebot audit on",
		                impl_->autoDisabled, impl_->path);
	}
}

void BotAuditSink::writeCmd(std::string_view source, std::string_view botName, std::string_view command, bool ok, std::string_view replyHead) {
	if (!wants("cmd")) {
		return;
	}
	std::string line;
	line.reserve(160 + command.size() + replyHead.size());
	line += R"({"v":1,"ts":)";
	line += std::to_string(epochMs());
	line += R"(,"seq":)";
	line += std::to_string(nextSeq());
	line += R"(,"src":"srv","k":"cmd","source":)";
	appendJsonString(line, source);
	line += R"(,"bot":)";
	appendJsonString(line, botName);
	line += R"(,"command":)";
	appendJsonString(line, utf8Prefix(command, 400));
	line += R"(,"ok":)";
	line += ok ? "true" : "false";
	line += R"(,"reply":)";
	appendJsonString(line, utf8Prefix(replyHead, 200));
	line += "}\n";
	write(line);
}

std::string BotAuditSink::status() const {
	std::scoped_lock lock(impl_->mu);
	std::string kinds;
	if (impl_->allKinds) {
		kinds = "all";
	} else {
		for (const auto &k : impl_->kindSet) {
			kinds += (kinds.empty() ? "" : ",") + k;
		}
	}
	std::string watch;
	for (const auto &w : impl_->watch) {
		watch += (watch.empty() ? "" : ",") + w;
	}
	return "[BOT_AUDIT] enabled=" + std::string(impl_->on.load() ? "on" : "off")
		+ " path='" + impl_->path + "' open=" + (impl_->fh ? "yes" : "no")
		+ " kinds=" + (kinds.empty() ? "(none)" : kinds)
		+ " watch=" + (watch.empty() ? "(none)" : watch)
		+ " probeMs=" + std::to_string(impl_->probeMs)
		+ " censusSec=" + std::to_string(impl_->censusMs / 1000)
		+ " maxMb=" + std::to_string(impl_->maxBytes / (1024 * 1024))
		+ " fileBytes=" + std::to_string(impl_->bytes)
		+ " lines=" + std::to_string(impl_->linesWritten)
		+ " rotations=" + std::to_string(impl_->rotations)
		+ " errors=" + std::to_string(impl_->writeErrors)
		+ (impl_->lastError.empty() ? "" : " lastError='" + impl_->lastError + "'")
		+ (impl_->autoDisabled.empty() ? "" : " AUTO-DISABLED='" + impl_->autoDisabled + "'")
		+ " seq=" + std::to_string(impl_->seq.load())
		+ " gen=" + std::to_string(impl_->gen.load());
}
