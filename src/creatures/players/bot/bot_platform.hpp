/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

// ============================================================================
// bot_platform.hpp -- the ONE place the bot engine talks to the OS.
//
// The engine was written against Linux and is deployed there; Windows is a
// build target for local development, not the production host. Every POSIX
// primitive the engine needs is wrapped here exactly once so that:
//
//   1. the 21 engine TUs contain no #ifdef _WIN32 at all, and
//   2. the Linux code path stays TEXTUALLY identical to what has been running
//      in production -- the #else branch below is a verbatim move, so a Linux
//      binary built before and after this header is unchanged.
//
// Anything added here must keep that property: Windows code goes in the
// #ifdef branch, and the Linux branch is only ever moved, never rewritten.
// ============================================================================

// definitions.hpp gives us NOMINMAX, _CRT_SECURE_NO_WARNINGS, the MSVC warning
// suppressions, and -- load-bearing for the engine -- the
//     #define strcasecmp _stricmp
// that makes bot_data.cpp:711 and bot_hunt.cpp:1249/2212 compile unmodified.
//
// The engine reaches definitions.hpp today only because pch.hpp is force-included
// by target_precompile_headers. That is a silent dependency on
// USE_PRECOMPILED_HEADER=ON: flip it OFF and the engine loses strcasecmp with a
// confusing error. Including it explicitly here makes the guarantee unconditional.
#include "utils/definitions.hpp"

#include <algorithm> // std::min in the chunked _write loop
#include <cstdint>
#include <cstddef>
#include <cstring> // strerror
#include <ctime>
#include <string>

#ifdef _WIN32
	// WIN32_LEAN_AND_MEAN is NOT defined anywhere in the tree. Without it,
	// <windows.h> drags in winsock.h, which then collides with the winsock2.h
	// that asio includes. NOMINMAX comes from definitions.hpp above and is what
	// stops the min/max macros from breaking std::min/std::max and
	// numeric_limits<>::max() in all 21 engine TUs.
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>

	#include <io.h>
	#include <fcntl.h>
	#include <sys/stat.h>

	// The engine uses ssize_t as a SIGNED loop index that must go negative
	// (bot_waypoint.cpp:1103/1194 walk a waypoint list backwards and rely on
	// `i >= 0` terminating). MSVC has no ssize_t. Typedef it rather than editing
	// those loops, so the Linux TUs stay untouched.
using ssize_t = std::ptrdiff_t;
#else
	#include <fcntl.h>
	#include <unistd.h>
#endif

// ----------------------------------------------------------------------------
// Thread CPU time
// ----------------------------------------------------------------------------
//
// The caller (botThreadCpuUs in bot_engine_impl.hpp) documents WHY this is a
// per-thread clock rather than a per-process one. What matters here is the
// accuracy difference between the two implementations:
//
//   Linux   clock_gettime(CLOCK_THREAD_CPUTIME_ID) -- nanosecond resolution.
//   Windows GetThreadTimes()                       -- 100ns units, but only
//           actually updated on the scheduler tick, so it quantises to ~15.6ms.
//
// That quantisation is fine for the aggregate body_cpu-vs-body_wall
// discrimination the tick profiler does over many ticks, and useless for
// single-tick attribution. Windows is a dev target, so this is an accepted
// trade; do not port a Windows profiling number back to a Linux conclusion.
inline uint64_t botPlatformThreadCpuUs() {
#ifdef _WIN32
	FILETIME creationTime {};
	FILETIME exitTime {};
	FILETIME kernelTime {};
	FILETIME userTime {};
	if (GetThreadTimes(GetCurrentThread(), &creationTime, &exitTime, &kernelTime, &userTime) == 0) {
		return 0;
	}
	const auto toUs = [](const FILETIME &ft) -> uint64_t {
		ULARGE_INTEGER v {};
		v.LowPart = ft.dwLowDateTime;
		v.HighPart = ft.dwHighDateTime;
		return v.QuadPart / 10ULL; // 100ns units -> microseconds
	};
	return toUs(kernelTime) + toUs(userTime);
#else
	timespec ts {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL
		+ static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
#endif
}

// ----------------------------------------------------------------------------
// Local time
// ----------------------------------------------------------------------------
//
// MSVC's localtime_s takes (tm*, const time_t*) -- the REVERSE of both C11's
// localtime_s and POSIX's localtime_r. Getting it backwards compiles on some
// configurations and produces garbage timestamps, so it is wrapped once here.
inline void botPlatformLocalTime(time_t secs, struct tm &out) {
#ifdef _WIN32
	localtime_s(&out, &secs);
#else
	localtime_r(&secs, &out);
#endif
}

// ----------------------------------------------------------------------------
// Durable file write
// ----------------------------------------------------------------------------
//
// Writes `content` to `path` and does not return true until the bytes are on
// the platter -- the authored-CSV writer relies on this so a crash mid-save
// cannot leave a half-written hunt table.
//
// The Windows side maps 1:1 onto the CRT's low-level IO (_open/_write/_commit),
// with _O_BINARY added: without it the CRT translates '\n' to "\r\n" on write,
// which would silently corrupt every CSV the engine round-trips.
inline bool botPlatformWriteFileDurable(const std::string &path, const std::string &content, std::string &errOut) {
#ifdef _WIN32
	int fd = -1;
	if (_sopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fd < 0) {
		char msg[256] {};
		strerror_s(msg, sizeof(msg), errno);
		errOut = "open('" + path + "') failed: " + msg;
		return false;
	}
	size_t off = 0;
	while (off < content.size()) {
		const unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(content.size() - off, 1u << 30));
		const int w = _write(fd, content.data() + off, chunk);
		if (w <= 0) {
			char msg[256] {};
			strerror_s(msg, sizeof(msg), errno);
			errOut = "write('" + path + "') failed: " + msg;
			_close(fd);
			return false;
		}
		off += static_cast<size_t>(w);
	}
	if (_commit(fd) != 0) {
		char msg[256] {};
		strerror_s(msg, sizeof(msg), errno);
		errOut = "commit('" + path + "') failed: " + msg;
		_close(fd);
		return false;
	}
	_close(fd);
	return true;
#else
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		errOut = std::string("open('") + path + "') failed: " + strerror(errno);
		return false;
	}
	size_t off = 0;
	while (off < content.size()) {
		const ssize_t w = ::write(fd, content.data() + off, content.size() - off);
		if (w <= 0) {
			errOut = std::string("write('") + path + "') failed: " + strerror(errno);
			::close(fd);
			return false;
		}
		off += static_cast<size_t>(w);
	}
	if (::fsync(fd) != 0) {
		errOut = std::string("fsync('") + path + "') failed: " + strerror(errno);
		::close(fd);
		return false;
	}
	::close(fd);
	return true;
#endif
}

// ----------------------------------------------------------------------------
// Directory sync
// ----------------------------------------------------------------------------
//
// On POSIX a rename is not durable until the containing DIRECTORY is fsynced --
// the file contents survive a power cut but the name may not. Windows has no
// equivalent operation and needs none: NTFS journals its own metadata, and
// MoveFileEx (which std::filesystem::rename uses) is already durable. So this
// is a genuine no-op there, not an unimplemented stub.
//
// Best-effort on both platforms: a failure here must never fail the write.
inline void botPlatformSyncDirectory(const std::string &dir) {
#ifdef _WIN32
	(void)dir;
#else
	const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
	if (dfd >= 0) {
		::fsync(dfd);
		::close(dfd);
	}
#endif
}
