// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#include "XrLink.h"

#include <ctime>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>

#include <cstring>
#include <vector>

// XR_MNDX_xdev_space is not in the Khronos headers; values from Monado.
#define XR_MNDX_XDEV_SPACE_EXTENSION_NAME "XR_MNDX_xdev_space"
#define XR_MND_HEADLESS_EXTENSION_NAME_STR "XR_MND_headless"

namespace
{
	XR_DEFINE_HANDLE(XrXDevListMNDX)
	typedef uint64_t XrXDevIdMNDX;

	const XrStructureType XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX = (XrStructureType)1000444002;
	const XrStructureType XR_TYPE_GET_XDEV_INFO_MNDX = (XrStructureType)1000444003;
	const XrStructureType XR_TYPE_XDEV_PROPERTIES_MNDX = (XrStructureType)1000444004;
	const XrStructureType XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX = (XrStructureType)1000444005;

	struct XrCreateXDevListInfoMNDX { XrStructureType type; const void* next; };
	struct XrGetXDevInfoMNDX { XrStructureType type; const void* next; XrXDevIdMNDX id; };
	struct XrXDevPropertiesMNDX { XrStructureType type; void* next; char name[256]; char serial[256]; XrBool32 canCreateSpace; };
	struct XrCreateXDevSpaceInfoMNDX { XrStructureType type; const void* next; XrXDevListMNDX xdevList; XrXDevIdMNDX id; XrPosef offset; };

	typedef XrResult (XRAPI_PTR* PFN_xrCreateXDevListMNDX)(XrSession, const XrCreateXDevListInfoMNDX*, XrXDevListMNDX*);
	typedef XrResult (XRAPI_PTR* PFN_xrGetXDevListGenerationNumberMNDX)(XrXDevListMNDX, uint64_t*);
	typedef XrResult (XRAPI_PTR* PFN_xrEnumerateXDevsMNDX)(XrXDevListMNDX, uint32_t, uint32_t*, XrXDevIdMNDX*);
	typedef XrResult (XRAPI_PTR* PFN_xrGetXDevPropertiesMNDX)(XrXDevListMNDX, const XrGetXDevInfoMNDX*, XrXDevPropertiesMNDX*);
	typedef XrResult (XRAPI_PTR* PFN_xrDestroyXDevListMNDX)(XrXDevListMNDX);
	typedef XrResult (XRAPI_PTR* PFN_xrCreateXDevSpaceMNDX)(XrSession, const XrCreateXDevSpaceInfoMNDX*, XrSpace*);

	PFN_xrConvertTimespecTimeToTimeKHR convertTime = nullptr;
}

namespace monado
{
	struct XrLink::XDevList
	{
		XrXDevListMNDX list = XR_NULL_HANDLE;
		PFN_xrCreateXDevListMNDX create = nullptr;
		PFN_xrGetXDevListGenerationNumberMNDX generation = nullptr;
		PFN_xrEnumerateXDevsMNDX enumerate = nullptr;
		PFN_xrGetXDevPropertiesMNDX properties = nullptr;
		PFN_xrDestroyXDevListMNDX destroy = nullptr;
		PFN_xrCreateXDevSpaceMNDX createSpace = nullptr;
	};

	static std::string XrError(XrInstance instance, XrResult r)
	{
		char buf[XR_MAX_RESULT_STRING_SIZE] = {};
		if (instance != XR_NULL_HANDLE && xrResultToString(instance, r, buf) == XR_SUCCESS)
			return buf;
		return std::to_string((int)r);
	}

	XrLink::~XrLink()
	{
		DestroySpaces();
		if (xdev && xdev->list)
			xdev->destroy(xdev->list);
		delete xdev;
		if (stage)
			xrDestroySpace(stage);
		if (session)
		{
			if (running)
				xrEndSession(session);
			xrDestroySession(session);
		}
		if (actionSet)
			xrDestroyActionSet(actionSet);
		if (instance)
			xrDestroyInstance(instance);
	}

	bool XrLink::Init(std::string& error)
	{
		uint32_t count = 0;
		xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
		std::vector<XrExtensionProperties> props(count, { XR_TYPE_EXTENSION_PROPERTIES });
		xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
		auto has = [&](const char* name)
		{
			for (const auto& p : props)
				if (std::strcmp(p.extensionName, name) == 0)
					return true;
			return false;
		};
		for (const char* need : { XR_MND_HEADLESS_EXTENSION_NAME_STR, XR_MNDX_XDEV_SPACE_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME })
			if (!has(need))
			{
				error = std::string("the OpenXR runtime does not offer ") + need + " (is WiVRn the runtime in use?)";
				return false;
			}

		const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME_STR, XR_MNDX_XDEV_SPACE_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME };
		XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
		std::strcpy(ci.applicationInfo.applicationName, "SpaceSync");
		std::strcpy(ci.applicationInfo.engineName, "SpaceSync");
		ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
		ci.enabledExtensionCount = 3;
		ci.enabledExtensionNames = exts;
		XrResult r = xrCreateInstance(&ci, &instance);
		if (XR_FAILED(r))
		{
			error = "xrCreateInstance failed: " + XrError(XR_NULL_HANDLE, r);
			return false;
		}

		XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		r = xrGetSystem(instance, &sgi, &system);
		if (XR_FAILED(r))
		{
			error = "no headset available: " + XrError(instance, r);
			return false;
		}

		xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction*)&convertTime);
		xdev = new XDevList;
		xrGetInstanceProcAddr(instance, "xrCreateXDevListMNDX", (PFN_xrVoidFunction*)&xdev->create);
		xrGetInstanceProcAddr(instance, "xrGetXDevListGenerationNumberMNDX", (PFN_xrVoidFunction*)&xdev->generation);
		xrGetInstanceProcAddr(instance, "xrEnumerateXDevsMNDX", (PFN_xrVoidFunction*)&xdev->enumerate);
		xrGetInstanceProcAddr(instance, "xrGetXDevPropertiesMNDX", (PFN_xrVoidFunction*)&xdev->properties);
		xrGetInstanceProcAddr(instance, "xrDestroyXDevListMNDX", (PFN_xrVoidFunction*)&xdev->destroy);
		xrGetInstanceProcAddr(instance, "xrCreateXDevSpaceMNDX", (PFN_xrVoidFunction*)&xdev->createSpace);
		if (!convertTime || !xdev->create || !xdev->generation || !xdev->enumerate || !xdev->properties || !xdev->destroy || !xdev->createSpace)
		{
			error = "OpenXR runtime is missing extension functions";
			return false;
		}

		// Headless: no graphics binding.
		XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
		sci.systemId = system;
		r = xrCreateSession(instance, &sci, &session);
		if (XR_FAILED(r))
		{
			error = "xrCreateSession failed: " + XrError(instance, r);
			return false;
		}

		// motoc attaches an (empty) action set and syncs it every loop; do the same.
		XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
		std::strcpy(asci.actionSetName, "spacesync");
		std::strcpy(asci.localizedActionSetName, "SpaceSync");
		xrCreateActionSet(instance, &asci, &actionSet);
		XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attach.countActionSets = 1;
		attach.actionSets = &actionSet;
		xrAttachSessionActionSets(session, &attach);

		XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		rci.poseInReferenceSpace.orientation.w = 1.0f;
		r = xrCreateReferenceSpace(session, &rci, &stage);
		if (XR_FAILED(r))
		{
			error = "could not create the stage space: " + XrError(instance, r);
			return false;
		}
		return true;
	}

	bool XrLink::Poll()
	{
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(instance, &ev) == XR_SUCCESS)
		{
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
				switch (sc->state)
				{
				case XR_SESSION_STATE_READY:
				{
					XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					running = XR_SUCCEEDED(xrBeginSession(session, &bi));
					break;
				}
				case XR_SESSION_STATE_STOPPING:
					xrEndSession(session);
					running = false;
					break;
				case XR_SESSION_STATE_EXITING:
				case XR_SESSION_STATE_LOSS_PENDING:
					running = false;
					return false;
				default:
					break;
				}
			}
			else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
				return false;
			ev = { XR_TYPE_EVENT_DATA_BUFFER };
		}

		if (running)
		{
			XrActiveActionSet active = { actionSet, XR_NULL_PATH };
			XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
			sync.countActiveActionSets = 1;
			sync.activeActionSets = &active;
			xrSyncActions(session, &sync);
		}
		return true;
	}

	void XrLink::DestroySpaces()
	{
		for (auto& kv : spaces)
			xrDestroySpace(kv.second);
		spaces.clear();
	}

	bool XrLink::RefreshDevices()
	{
		// A list is a snapshot of Monado's devices. Make a fresh one and keep
		// it only when its generation differs from the one in use.
		XrXDevListMNDX fresh = XR_NULL_HANDLE;
		XrCreateXDevListInfoMNDX info = { XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX, nullptr };
		if (XR_FAILED(xdev->create(session, &info, &fresh)))
			return false;
		uint64_t generation = 0;
		if (XR_FAILED(xdev->generation(fresh, &generation)) || (xdev->list && generation == xdevGeneration))
		{
			xdev->destroy(fresh);
			return false;
		}

		// Spaces were made from the old list; replace both.
		DestroySpaces();
		if (xdev->list)
			xdev->destroy(xdev->list);
		xdev->list = fresh;
		xdevGeneration = generation;

		uint32_t count = 0;
		xdev->enumerate(xdev->list, 0, &count, nullptr);
		std::vector<XrXDevIdMNDX> ids(count);
		xdev->enumerate(xdev->list, count, &count, ids.data());
		for (XrXDevIdMNDX id : ids)
		{
			XrGetXDevInfoMNDX gi = { XR_TYPE_GET_XDEV_INFO_MNDX, nullptr, id };
			XrXDevPropertiesMNDX p = {};
			p.type = XR_TYPE_XDEV_PROPERTIES_MNDX;
			if (XR_FAILED(xdev->properties(xdev->list, &gi, &p)) || !p.canCreateSpace || spaces.count(p.serial))
				continue;
			XrCreateXDevSpaceInfoMNDX si = { XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX, nullptr, xdev->list, id, {} };
			si.offset.orientation.w = 1.0f;
			XrSpace space = XR_NULL_HANDLE;
			if (XR_SUCCEEDED(xdev->createSpace(session, &si, &space)))
				spaces[p.serial] = space;
		}
		return true;
	}

	XrTime XrLink::Now() const
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		XrTime t = 0;
		convertTime(instance, &ts, &t);
		return t;
	}

	std::optional<rigid::Pose> XrLink::Locate(const std::string& serial, XrTime time) const
	{
		auto it = spaces.find(serial);
		if (it == spaces.end())
			return std::nullopt;
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
		if (XR_FAILED(xrLocateSpace(it->second, stage, time, &loc)))
			return std::nullopt;
		const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
			| XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
		if ((loc.locationFlags & need) != need)
			return std::nullopt;
		rigid::Pose p;
		p.q = Eigen::Quaterniond(loc.pose.orientation.w, loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z).normalized();
		p.p = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
		return p;
	}
}
