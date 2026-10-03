// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// libmonado, loaded at runtime from the path the OpenXR runtime manifest
// names (MND_libmonado_path), the same way motoc finds it. Only the calls
// SpaceSync needs: devices, tracking origins and their offsets, and the
// stage reference space offset.

#include "Rigid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace monado
{
	struct DeviceInfo
	{
		uint32_t index = 0;
		std::string name;
		std::string serial;
		uint32_t origin = 0;
	};

	class MonadoLink
	{
	public:
		~MonadoLink();

		// Finds the runtime manifest and libmonado. When the active runtime is
		// not Monado-based (e.g. SteamVR), falls back to an installed WiVRn
		// and points XR_RUNTIME_JSON at it so OpenXR uses the same runtime.
		bool Load(std::string& error);

		// Connects to the running service. False while it is not running.
		// quiet hides libmonado's own banner on failure (for retries).
		bool Connect(std::string& error, bool quiet = false);

		std::string RuntimeJson() const { return runtimeJson; }
		std::string Version() const;

		std::vector<DeviceInfo> Devices();
		std::vector<std::string> OriginNames();
		bool HeadDevice(DeviceInfo& out);

		bool GetOriginOffset(uint32_t origin, rigid::Pose& out);
		bool SetOriginOffset(uint32_t origin, const rigid::Pose& pose);
		bool GetStageOffset(rigid::Pose& out);

	private:
		void* lib = nullptr;
		void* root = nullptr;
		std::string runtimeJson;
		struct Api;
		Api* api = nullptr;
	};
}
