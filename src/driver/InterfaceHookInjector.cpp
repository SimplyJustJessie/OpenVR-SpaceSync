// SPDX-License-Identifier: AGPL-3.0-only
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#include "Logging.h"
#include "Hooking.h"
#include "InterfaceHookInjector.h"
#include "ServerTrackedDeviceProvider.h"
#include "Main.h"

static Hook<void*(*)(void*, const char *, vr::EVRInitError *)>
	GetGenericInterfaceHook("IVRDriverContext::GetGenericInterface");

static Hook<void(*)(void*, uint32_t, const vr::DriverPose_t &, uint32_t)>
	TrackedDevicePoseUpdatedHook005("IVRServerDriverHost005::TrackedDevicePoseUpdated");

static Hook<void(*)(void*, uint32_t, const vr::DriverPose_t &, uint32_t)>
	TrackedDevicePoseUpdatedHook006("IVRServerDriverHost006::TrackedDevicePoseUpdated");

static void DetourTrackedDevicePoseUpdated005(void* _this, uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize)
{
	if (sizeof(vr::DriverPose_t) != unPoseStructSize)
	{
		TrackedDevicePoseUpdatedHook005.originalFunc(_this, unWhichDevice, newPose, unPoseStructSize);
		return;
	}
	auto pose = newPose;
	if (g_server.HandleDevicePoseUpdated(unWhichDevice, pose))
	{
		TrackedDevicePoseUpdatedHook005.originalFunc(_this, unWhichDevice, pose, unPoseStructSize);
	}
}

static void DetourTrackedDevicePoseUpdated006(void* _this, uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize)
{
	if (sizeof(vr::DriverPose_t) != unPoseStructSize)
	{
		TrackedDevicePoseUpdatedHook006.originalFunc(_this, unWhichDevice, newPose, unPoseStructSize);
		return;
	}
	auto pose = newPose;
	if (g_server.HandleDevicePoseUpdated(unWhichDevice, pose))
	{
		TrackedDevicePoseUpdatedHook006.originalFunc(_this, unWhichDevice, pose, unPoseStructSize);
	}
}

static void *DetourGetGenericInterface(void* _this, const char *pchInterfaceVersion, vr::EVRInitError *peError)
{
	TRACE("ServerTrackedDeviceProvider::DetourGetGenericInterface(%s)", pchInterfaceVersion);
	auto originalInterface = GetGenericInterfaceHook.originalFunc(_this, pchInterfaceVersion, peError);

	std::string iface(pchInterfaceVersion);
	if (!originalInterface)
	{
		// Nothing to hook; the caller gets the error from peError.
	}
	else if (iface == "IVRServerDriverHost_005")
	{
		if (!IHook::Exists(TrackedDevicePoseUpdatedHook005.name))
		{
			TrackedDevicePoseUpdatedHook005.CreateHookInObjectVTable(originalInterface, 1, (void *)&DetourTrackedDevicePoseUpdated005);
			IHook::Register(&TrackedDevicePoseUpdatedHook005);
		}
	}
	else if (iface == "IVRServerDriverHost_006")
	{
		if (!IHook::Exists(TrackedDevicePoseUpdatedHook006.name))
		{
			TrackedDevicePoseUpdatedHook006.CreateHookInObjectVTable(originalInterface, 1, (void *)&DetourTrackedDevicePoseUpdated006);
			IHook::Register(&TrackedDevicePoseUpdatedHook006); 
		}
	}

	return originalInterface;
}

void InjectHooks(vr::IVRDriverContext *pDriverContext)
{
#ifdef _WIN32
	auto err = MH_Initialize();
	if (err == MH_OK)
	{
		GetGenericInterfaceHook.CreateHookInObjectVTable(pDriverContext, 0, &DetourGetGenericInterface);
		IHook::Register(&GetGenericInterfaceHook);
	}
	else
	{
		LOG("MH_Initialize error: %s", MH_StatusToString(err));
	}
#else
	if (GetGenericInterfaceHook.CreateHookInObjectVTable(pDriverContext, 0, (void *)&DetourGetGenericInterface))
	{
		IHook::Register(&GetGenericInterfaceHook);

		// Drivers loaded before us (lighthouse) already fetched their driver
		// host, so nothing may ask for it again. Ask once ourselves so the
		// detour patches the host vtable now, which those drivers share.
		vr::EVRInitError hostErr = vr::VRInitError_None;
		pDriverContext->GetGenericInterface(vr::IVRServerDriverHost_Version, &hostErr);
		if (!IHook::Exists(TrackedDevicePoseUpdatedHook006.name))
			LOG("Pose hook not installed after host request (error %d)", (int)hostErr);
	}
#endif
}

void DisableHooks()
{
	IHook::DestroyAll();
#ifdef _WIN32
	MH_Uninitialize();
#endif
}