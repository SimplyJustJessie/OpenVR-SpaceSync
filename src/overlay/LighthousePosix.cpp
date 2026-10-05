// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Linux port). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

// Linux build of the lighthouse module. Basestation power control is left to
// a separate lighthouse manager, so the power API reports "unavailable" and
// does nothing. Note() stays real: the overlay uses it as its event log
// (startup, SteamVR registration, errors), written to lighthouse.log like on
// Windows, but under $XDG_STATE_HOME/spacesync.

#include "Lighthouse.h"
#include "PlatformPaths.h"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace lighthouse
{
	namespace
	{
		std::mutex logMutex;

		void Log(const char* message)
		{
			std::lock_guard<std::mutex> lock(logMutex);
			static std::string path = paths::StateDir() + "/lighthouse.log";
			FILE* f = fopen(path.c_str(), "a");
			if (!f)
				return;
			timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			tm local;
			localtime_r(&ts.tv_sec, &local);
			fprintf(f, "[%02d:%02d:%02d.%03d] app: %s\n", local.tm_hour, local.tm_min, local.tm_sec, (int)(ts.tv_nsec / 1000000), message);
			fclose(f);
		}
	}

	void Init() {}
	void Shutdown() {}
	void EnsureScanning() {}
	bool Scanning() { return false; }
	bool Available() { return false; }
	std::string AvailabilityError() { return "Basestation power control is not part of the Linux build."; }
	std::vector<Station> Stations() { return {}; }
	void RequestPower(uint64_t, Power) {}
	void RequestPowerAll(Power) {}
	void RequestRefresh(uint64_t) {}
	void SetAutoWake(bool) {}
	void SetManaged(bool) {}
	bool Managed() { return false; }
	void PowerDownAllAndWait(Power, int) {}
	void BeginPowerDownAll(Power) {}
	bool Idle() { return true; }

	void Note(const char* message)
	{
		Log(message);
	}
}
