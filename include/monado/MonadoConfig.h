// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// Settings and calibration of the Monado companion, in
// $XDG_CONFIG_HOME/spacesync/monado.json.

#include "PlatformPaths.h"
#include "Rigid.h"

#include <picojson.h>

#include <ctime>
#include <string>

namespace monado
{
	struct Config
	{
		bool calibrated = false;
		std::string origin;  // lighthouse tracking origin name
		rigid::Pose M;       // calibration: lighthouse native -> root
		std::string device;  // device held during calibration
		double rmsMm = 0.0;
		std::string date;

		std::string hip;     // hip tracker serial, "" = SteamVR role, "none" = off
		bool stayAligned = true;

		static std::string Path() { return paths::ConfigDir() + "/monado.json"; }

		bool Load()
		{
			std::string text = paths::ReadFile(Path());
			picojson::value root;
			if (text.empty() || !picojson::parse(root, text).empty() || !root.is<picojson::object>())
				return false;
			auto& o = root.get<picojson::object>();
			auto str = [&](const picojson::object& obj, const char* k) { auto it = obj.find(k); return it != obj.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string(); };
			auto num = [&](const picojson::object& obj, const char* k) { auto it = obj.find(k); return it != obj.end() && it->second.is<double>() ? it->second.get<double>() : 0.0; };

			hip = str(o, "hip");
			if (auto it = o.find("stay_aligned"); it != o.end() && it->second.is<bool>())
				stayAligned = it->second.get<bool>();

			auto cal = o.find("calibration");
			if (cal != o.end() && cal->second.is<picojson::object>())
			{
				const auto& c = cal->second.get<picojson::object>();
				auto q = c.find("rotation"), p = c.find("translation");
				if (q != c.end() && p != c.end() && q->second.is<picojson::array>() && p->second.is<picojson::array>()
					&& q->second.get<picojson::array>().size() == 4 && p->second.get<picojson::array>().size() == 3)
				{
					const auto& qa = q->second.get<picojson::array>();
					const auto& pa = p->second.get<picojson::array>();
					M.q = Eigen::Quaterniond(qa[3].get<double>(), qa[0].get<double>(), qa[1].get<double>(), qa[2].get<double>()).normalized();
					M.p = { pa[0].get<double>(), pa[1].get<double>(), pa[2].get<double>() };
					origin = str(c, "origin");
					device = str(c, "device");
					date = str(c, "date");
					rmsMm = num(c, "rms_mm");
					calibrated = true;
				}
			}
			return true;
		}

		bool Save() const
		{
			picojson::object o;
			o["hip"] = picojson::value(hip);
			o["stay_aligned"] = picojson::value(stayAligned);
			if (calibrated)
			{
				picojson::object c;
				c["origin"] = picojson::value(origin);
				c["device"] = picojson::value(device);
				c["date"] = picojson::value(date);
				c["rms_mm"] = picojson::value(rmsMm);
				c["rotation"] = picojson::value(picojson::array{ picojson::value(M.q.x()), picojson::value(M.q.y()), picojson::value(M.q.z()), picojson::value(M.q.w()) });
				c["translation"] = picojson::value(picojson::array{ picojson::value(M.p.x()), picojson::value(M.p.y()), picojson::value(M.p.z()) });
				o["calibration"] = picojson::value(c);
			}
			return paths::WriteFileAtomic(Path(), picojson::value(o).serialize(true));
		}

		static std::string Today()
		{
			time_t now = time(nullptr);
			tm local;
			localtime_r(&now, &local);
			char buf[32];
			strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &local);
			return buf;
		}
	};
}
