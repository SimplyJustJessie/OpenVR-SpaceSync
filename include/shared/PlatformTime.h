// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Linux port). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// The driver times everything with the Win32 performance counter. On other
// platforms this provides the same API on top of CLOCK_MONOTONIC (nanosecond
// ticks), so the timing code stays identical across platforms.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#else

#include <cstdint>
#include <ctime>

union LARGE_INTEGER
{
	int64_t QuadPart;
};

inline int QueryPerformanceCounter(LARGE_INTEGER *counter)
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	counter->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	return 1;
}

inline int QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 1000000000LL;
	return 1;
}

#endif
