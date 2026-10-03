// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// A headless OpenXR session (XR_MND_headless) that reads raw device poses
// through XR_MNDX_xdev_space, like motoc. Poses come back in the stage
// reference space; callers move them into Monado's root space with the stage
// offset from libmonado.

#include "Rigid.h"

#include <openxr/openxr.h>

#include <map>
#include <optional>
#include <string>

namespace monado
{
	class XrLink
	{
	public:
		~XrLink();

		bool Init(std::string& error);

		// Handles session state changes. False once the session is over.
		bool Poll();
		bool Running() const { return running; }

		// Rebuilds the device list when Monado's changes (trackers switched on
		// later). Returns true when it changed.
		bool RefreshDevices();
		bool HasDevice(const std::string& serial) const { return spaces.count(serial) != 0; }

		XrTime Now() const;
		static double Seconds(XrTime t) { return t * 1e-9; }

		// Pose in the stage space, only when position and orientation are tracked.
		std::optional<rigid::Pose> Locate(const std::string& serial, XrTime time) const;

	private:
		XrInstance instance = XR_NULL_HANDLE;
		XrSystemId system = XR_NULL_SYSTEM_ID;
		XrSession session = XR_NULL_HANDLE;
		XrActionSet actionSet = XR_NULL_HANDLE;
		XrSpace stage = XR_NULL_HANDLE;
		bool running = false;

		struct XDevList;
		XDevList* xdev = nullptr;
		uint64_t xdevGeneration = ~0ull;
		std::map<std::string, XrSpace> spaces;

		void DestroySpaces();
	};
}
