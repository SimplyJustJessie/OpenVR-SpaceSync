// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Linux port). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// Linux stand-ins for the places Windows keeps SpaceSync's data (registry,
// %LOCALAPPDATA%). Directories follow the XDG base directory spec and are
// created on first use.

#ifndef _WIN32

#include <cstdlib>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace paths
{
	inline std::string XdgDir(const char *envVar, const char *homeFallback)
	{
		std::string base;
		if (const char *v = getenv(envVar); v && *v)
			base = v;
		else if (const char *home = getenv("HOME"); home && *home)
			base = std::string(home) + "/" + homeFallback;
		else
			base = "/tmp";

		mkdir(base.c_str(), 0755);
		std::string dir = base + "/spacesync";
		mkdir(dir.c_str(), 0755);
		return dir;
	}

	// Profile and settings: $XDG_CONFIG_HOME/spacesync (~/.config/spacesync).
	inline std::string ConfigDir() { return XdgDir("XDG_CONFIG_HOME", ".config"); }

	// Logs and runtime markers: $XDG_STATE_HOME/spacesync (~/.local/state/spacesync),
	// the same folder the driver logs to.
	inline std::string StateDir() { return XdgDir("XDG_STATE_HOME", ".local/state"); }

	inline std::string ReadFile(const std::string &path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return "";
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	// Write to a temp file and rename over the target, so a crash mid-write
	// never leaves a truncated profile behind.
	inline bool WriteFileAtomic(const std::string &path, const std::string &data)
	{
		std::string tmp = path + ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;
			out << data;
			if (!out.flush())
				return false;
		}
		return rename(tmp.c_str(), path.c_str()) == 0;
	}
}

#endif
