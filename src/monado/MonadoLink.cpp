// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#include "MonadoLink.h"

#include <picojson.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <type_traits>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace monado
{
	// Mirrors monado.h (libmonado API 1.x).
	struct MndPose
	{
		float qx, qy, qz, qw;
		float px, py, pz;
	};

	enum MndProperty : int32_t
	{
		MND_PROPERTY_NAME_STRING = 0,
		MND_PROPERTY_SERIAL_STRING = 1,
		MND_PROPERTY_TRACKING_ORIGIN_U32 = 2,
	};

	enum MndReferenceSpace : int32_t
	{
		MND_SPACE_REFERENCE_TYPE_STAGE = 3,
	};

	using MndResult = int32_t;

	struct MonadoLink::Api
	{
		void (*get_version)(uint32_t*, uint32_t*, uint32_t*);
		MndResult (*root_create)(void**);
		void (*root_destroy)(void**);
		MndResult (*get_device_count)(void*, uint32_t*);
		MndResult (*get_device_info_u32)(void*, uint32_t, MndProperty, uint32_t*);
		MndResult (*get_device_info_string)(void*, uint32_t, MndProperty, const char**);
		MndResult (*get_device_from_role)(void*, const char*, int32_t*);
		MndResult (*get_tracking_origin_count)(void*, uint32_t*);
		MndResult (*get_tracking_origin_name)(void*, uint32_t, const char**);
		MndResult (*get_tracking_origin_offset)(void*, uint32_t, MndPose*);
		MndResult (*set_tracking_origin_offset)(void*, uint32_t, const MndPose*);
		MndResult (*get_reference_space_offset)(void*, MndReferenceSpace, MndPose*);
	};

	static rigid::Pose FromMnd(const MndPose& m)
	{
		rigid::Pose p;
		p.q = Eigen::Quaterniond(m.qw, m.qx, m.qy, m.qz).normalized();
		p.p = { m.px, m.py, m.pz };
		return p;
	}

	static MndPose ToMnd(const rigid::Pose& p)
	{
		Eigen::Quaterniond q = p.q.normalized();
		return { (float)q.x(), (float)q.y(), (float)q.z(), (float)q.w(), (float)p.p.x(), (float)p.p.y(), (float)p.p.z() };
	}

	static bool Exists(const std::string& path)
	{
		struct stat st;
		return stat(path.c_str(), &st) == 0;
	}

	static std::string Dir(const std::string& path)
	{
		size_t slash = path.find_last_of('/');
		return slash == std::string::npos ? "." : path.substr(0, slash);
	}

	// MND_libmonado_path from a runtime manifest, resolved against the
	// manifest's directory (the symlink's first, then its target's).
	static std::string LibmonadoFromManifest(const std::string& manifest)
	{
		std::ifstream in(manifest);
		if (!in)
			return "";
		std::stringstream buf;
		buf << in.rdbuf();
		picojson::value root;
		if (!picojson::parse(root, buf.str()).empty() || !root.is<picojson::object>())
			return "";
		const auto& obj = root.get<picojson::object>();
		auto rt = obj.find("runtime");
		if (rt == obj.end() || !rt->second.is<picojson::object>())
			return "";
		const auto& runtime = rt->second.get<picojson::object>();
		auto lp = runtime.find("MND_libmonado_path");
		if (lp == runtime.end() || !lp->second.is<std::string>())
			return "";
		std::string lib = lp->second.get<std::string>();
		if (!lib.empty() && lib[0] == '/')
			return lib;

		std::string candidate = Dir(manifest) + "/" + lib;
		if (Exists(candidate))
			return candidate;
		char real[PATH_MAX];
		if (realpath(manifest.c_str(), real))
		{
			candidate = Dir(real) + "/" + lib;
			if (Exists(candidate))
				return candidate;
		}
		return "";
	}

	static std::vector<std::string> ActiveRuntimeCandidates()
	{
		std::vector<std::string> out;
		if (const char* env = getenv("XR_RUNTIME_JSON"); env && *env)
			out.push_back(env);
		if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
			out.push_back(std::string(xdg) + "/openxr/1/active_runtime.json");
		else if (const char* home = getenv("HOME"); home && *home)
			out.push_back(std::string(home) + "/.config/openxr/1/active_runtime.json");
		out.push_back("/etc/xdg/openxr/1/active_runtime.json");
		return out;
	}

	static std::vector<std::string> WivrnManifests()
	{
		std::vector<std::string> out;
		if (const char* home = getenv("HOME"); home && *home)
			out.push_back(std::string(home) + "/.local/share/openxr/1/openxr_wivrn.json");
		out.push_back("/usr/local/share/openxr/1/openxr_wivrn.json");
		out.push_back("/usr/share/openxr/1/openxr_wivrn.json");
		return out;
	}

	MonadoLink::~MonadoLink()
	{
		if (api && root)
			api->root_destroy(&root);
		delete api;
		if (lib)
			dlclose(lib);
	}

	bool MonadoLink::Load(std::string& error)
	{
		std::string libPath;
		bool fromActive = false;
		for (const auto& manifest : ActiveRuntimeCandidates())
		{
			if (!Exists(manifest))
				continue;
			// The first manifest that exists is the one the OpenXR loader uses.
			libPath = LibmonadoFromManifest(manifest);
			if (!libPath.empty())
			{
				runtimeJson = manifest;
				fromActive = true;
			}
			break;
		}
		if (libPath.empty())
		{
			for (const auto& manifest : WivrnManifests())
			{
				libPath = LibmonadoFromManifest(manifest);
				if (!libPath.empty())
				{
					runtimeJson = manifest;
					break;
				}
			}
		}
		if (libPath.empty())
		{
			error = "no Monado-based OpenXR runtime found (is WiVRn installed?)";
			return false;
		}
		if (!fromActive)
			setenv("XR_RUNTIME_JSON", runtimeJson.c_str(), 1);

		lib = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!lib)
		{
			error = std::string("could not load ") + libPath + ": " + dlerror();
			return false;
		}

		api = new Api{};
		bool ok = true;
		auto sym = [&](auto& fn, const char* name)
		{
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
			if (!fn)
			{
				ok = false;
				error = std::string("libmonado is missing ") + name;
			}
		};
		sym(api->get_version, "mnd_api_get_version");
		sym(api->root_create, "mnd_root_create");
		sym(api->root_destroy, "mnd_root_destroy");
		sym(api->get_device_count, "mnd_root_get_device_count");
		sym(api->get_device_info_u32, "mnd_root_get_device_info_u32");
		sym(api->get_device_info_string, "mnd_root_get_device_info_string");
		sym(api->get_device_from_role, "mnd_root_get_device_from_role");
		sym(api->get_tracking_origin_count, "mnd_root_get_tracking_origin_count");
		sym(api->get_tracking_origin_name, "mnd_root_get_tracking_origin_name");
		sym(api->get_tracking_origin_offset, "mnd_root_get_tracking_origin_offset");
		sym(api->set_tracking_origin_offset, "mnd_root_set_tracking_origin_offset");
		sym(api->get_reference_space_offset, "mnd_root_get_reference_space_offset");
		if (!ok)
			return false;

		uint32_t major = 0, minor = 0, patch = 0;
		api->get_version(&major, &minor, &patch);
		if (major != 1)
		{
			error = "unsupported libmonado API version " + Version();
			return false;
		}
		return true;
	}

	std::string MonadoLink::Version() const
	{
		uint32_t major = 0, minor = 0, patch = 0;
		if (api)
			api->get_version(&major, &minor, &patch);
		return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	}

	bool MonadoLink::Connect(std::string& error, bool quiet)
	{
		if (root)
			return true;
		int savedStderr = -1;
		if (quiet)
		{
			fflush(stderr);
			savedStderr = dup(STDERR_FILENO);
			int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (devnull >= 0)
			{
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
		}
		MndResult r = api->root_create(&root);
		if (savedStderr >= 0)
		{
			fflush(stderr);
			dup2(savedStderr, STDERR_FILENO);
			close(savedStderr);
		}
		if (r != 0)
		{
			root = nullptr;
			error = "could not connect to the Monado service (is WiVRn running with a headset connected?), error " + std::to_string(r);
			return false;
		}
		return true;
	}

	std::vector<DeviceInfo> MonadoLink::Devices()
	{
		std::vector<DeviceInfo> out;
		uint32_t count = 0;
		if (api->get_device_count(root, &count) != 0)
			return out;
		for (uint32_t i = 0; i < count; i++)
		{
			DeviceInfo d;
			d.index = i;
			const char* s = nullptr;
			if (api->get_device_info_string(root, i, MND_PROPERTY_NAME_STRING, &s) == 0 && s)
				d.name = s;
			s = nullptr;
			if (api->get_device_info_string(root, i, MND_PROPERTY_SERIAL_STRING, &s) == 0 && s)
				d.serial = s;
			if (api->get_device_info_u32(root, i, MND_PROPERTY_TRACKING_ORIGIN_U32, &d.origin) != 0)
				continue;
			out.push_back(d);
		}
		return out;
	}

	std::vector<std::string> MonadoLink::OriginNames()
	{
		std::vector<std::string> out;
		uint32_t count = 0;
		if (api->get_tracking_origin_count(root, &count) != 0)
			return out;
		for (uint32_t i = 0; i < count; i++)
		{
			const char* s = nullptr;
			api->get_tracking_origin_name(root, i, &s);
			out.push_back(s ? s : "");
		}
		return out;
	}

	bool MonadoLink::HeadDevice(DeviceInfo& out)
	{
		int32_t index = -1;
		if (api->get_device_from_role(root, "head", &index) != 0 || index < 0)
			return false;
		for (const auto& d : Devices())
			if (d.index == (uint32_t)index)
			{
				out = d;
				return true;
			}
		return false;
	}

	bool MonadoLink::GetOriginOffset(uint32_t origin, rigid::Pose& out)
	{
		MndPose m;
		if (api->get_tracking_origin_offset(root, origin, &m) != 0)
			return false;
		out = FromMnd(m);
		return true;
	}

	bool MonadoLink::SetOriginOffset(uint32_t origin, const rigid::Pose& pose)
	{
		MndPose m = ToMnd(pose);
		return api->set_tracking_origin_offset(root, origin, &m) == 0;
	}

	bool MonadoLink::GetStageOffset(rigid::Pose& out)
	{
		MndPose m;
		if (api->get_reference_space_offset(root, MND_SPACE_REFERENCE_TYPE_STAGE, &m) != 0)
			return false;
		out = FromMnd(m);
		return true;
	}
}
