// SPDX-License-Identifier: AGPL-3.0-only
// Modified by Shinyflvres, 2026-08-23. Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#define _CRT_SECURE_NO_DEPRECATE
#include "Logging.h"
#include <chrono>

#ifndef _WIN32
#include <cstdlib>
#include <sys/stat.h>
#endif

FILE *LogFile;

std::string DriverLogPath(const char *fileName)
{
#ifdef _WIN32
	return fileName;
#else
	// vrserver's working directory is inside the SteamVR install, so on Linux
	// logs go to $XDG_STATE_HOME/spacesync (default ~/.local/state/spacesync).
	std::string dir;
	if (const char *state = getenv("XDG_STATE_HOME"); state && *state)
		dir = state;
	else if (const char *home = getenv("HOME"); home && *home)
		dir = std::string(home) + "/.local/state";
	else
		return fileName;

	mkdir(dir.c_str(), 0755);
	dir += "/spacesync";
	mkdir(dir.c_str(), 0755);
	return dir + "/" + fileName;
#endif
}

void OpenLogFile()
{
	LogFile = fopen(DriverLogPath("spacesync_driver.log").c_str(), "a");
	if (LogFile == nullptr)
	{
		LogFile = stderr;
	}
}

void CloseLogFile()
{
	int result = fclose(LogFile);
	if (result != 0)
		std::exit(EXIT_FAILURE);
}

tm TimeForLog()
{
	auto now = std::chrono::system_clock::now();
	auto nowTime = std::chrono::system_clock::to_time_t(now);
	tm value;
#ifdef _WIN32
	auto tm = localtime_s(&value, &nowTime);
#else
	localtime_r(&nowTime, &value);
#endif
	return value;
}

void LogFlush()
{
	fflush(LogFile);
}