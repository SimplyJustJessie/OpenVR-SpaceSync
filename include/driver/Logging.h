// SPDX-License-Identifier: AGPL-3.0-only
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#pragma once

#include <cstdio>
#include <ctime>
#include <string>

extern FILE *LogFile;

// Where the driver's log files go: the working directory on Windows,
// $XDG_STATE_HOME/spacesync on Linux.
std::string DriverLogPath(const char *fileName);

void OpenLogFile();
void CloseLogFile();

tm TimeForLog();
void LogFlush();

#ifndef LOG
#ifdef _MSC_VER
#define LOG(fmt, ...) do { \
	tm logNow = TimeForLog(); \
	fprintf(LogFile, "[%02d:%02d:%02d] " fmt "\n", logNow.tm_hour, logNow.tm_min, logNow.tm_sec, __VA_ARGS__); \
	LogFlush(); \
} while (0)
#else
// GCC/Clang only drop the trailing comma of an argument-less LOG("...")
// with the ##__VA_ARGS__ extension; MSVC does it on its own.
#define LOG(fmt, ...) do { \
	tm logNow = TimeForLog(); \
	fprintf(LogFile, "[%02d:%02d:%02d] " fmt "\n", logNow.tm_hour, logNow.tm_min, logNow.tm_sec, ##__VA_ARGS__); \
	LogFlush(); \
} while (0)
#endif
#endif

#define TRACE(...) {}

#ifndef TRACE
#define TRACE LOG
#endif
