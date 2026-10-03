// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

// spacesync-monado (EXPERIMENTAL, untested on hardware): SpaceSync's no-head-tracker calibration and Stay Aligned
// for Monado-based runtimes (WiVRn). Lighthouse devices are moved by setting
// their tracking origin offset through libmonado; poses are read through a
// headless OpenXR session. Replaces motoc for this setup.

#include "CalibrationSolver.h"
#include "MonadoConfig.h"
#include "MonadoLink.h"
#include "Sound.h"
#include "StayRunner.h"
#include "XrLink.h"

#include <picojson.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using namespace monado;
using rigid::Pose;

static std::atomic<bool> g_stop{ false };

static void Log(const char* fmt, ...)
{
	char msg[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof msg, fmt, args);
	va_end(args);

	time_t now = time(nullptr);
	tm local;
	localtime_r(&now, &local);
	char line[600];
	snprintf(line, sizeof line, "[%02d:%02d:%02d] %s\n", local.tm_hour, local.tm_min, local.tm_sec, msg);

	fputs(line, stderr);
	static std::string path = paths::StateDir() + "/monado.log";
	if (FILE* f = fopen(path.c_str(), "a"))
	{
		fputs(line, f);
		fclose(f);
	}
}

static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// calibrate holds this lock while it works on the raw lighthouse offset;
// run pauses while it is held and loads the new calibration afterwards.
static std::string CalibrationLockPath() { return paths::StateDir() + "/calibrating.lock"; }

static bool CalibrationInProgress(int fd)
{
	if (fd < 0)
		return false;
	if (flock(fd, LOCK_SH | LOCK_NB) != 0)
		return true;
	flock(fd, LOCK_UN);
	return false;
}

// ---------------------------------------------------------------------------
// Connection

struct Link
{
	MonadoLink mnd;
	XrLink xr;
	DeviceInfo head;
};

// Waits for the Monado service and a running OpenXR session.
static bool Connect(Link& link, int waitSeconds)
{
	std::string error;
	if (!link.mnd.Load(error))
	{
		Log("%s", error.c_str());
		return false;
	}
	Log("spacesync-monado is experimental; please report problems with this log");
	Log("using runtime %s (libmonado %s)", link.mnd.RuntimeJson().c_str(), link.mnd.Version().c_str());

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
	bool told = false;
	while (!link.mnd.Connect(error, told))
	{
		if (g_stop || std::chrono::steady_clock::now() > deadline)
		{
			Log("%s", error.c_str());
			return false;
		}
		if (!told)
			Log("waiting for the Monado service (WiVRn with a headset connected) ...");
		told = true;
		SleepMs(2000);
	}

	if (!link.xr.Init(error))
	{
		Log("%s", error.c_str());
		return false;
	}
	for (int i = 0; i < 250 && !g_stop && !link.xr.Running(); i++)
	{
		if (!link.xr.Poll())
			return false;
		SleepMs(40);
	}
	if (!link.xr.Running())
	{
		Log("the OpenXR session did not start");
		return false;
	}
	link.xr.RefreshDevices();

	if (!link.mnd.HeadDevice(link.head))
	{
		Log("no headset in Monado");
		return false;
	}
	return true;
}

// Root-space pose: stage-space pose moved by the stage offset.
static std::optional<Pose> LocateRoot(Link& link, const std::string& serial, XrTime t, const Pose& stage)
{
	auto p = link.xr.Locate(serial, t);
	if (!p)
		return std::nullopt;
	return stage * *p;
}

// The lighthouse tracking origin: the saved one, else the origin of the
// lighthouse ("LHR-") devices, else any origin other than the headset's.
static bool FindLighthouseOrigin(Link& link, const std::string& savedName, uint32_t& origin)
{
	auto names = link.mnd.OriginNames();
	if (!savedName.empty())
		for (uint32_t i = 0; i < names.size(); i++)
			if (names[i] == savedName && i != link.head.origin)
			{
				origin = i;
				return true;
			}

	std::map<uint32_t, int> votes;
	for (const auto& d : link.mnd.Devices())
		if (d.origin != link.head.origin)
			votes[d.origin] += d.serial.rfind("LHR-", 0) == 0 ? 10 : 1;
	int best = 0;
	for (auto& kv : votes)
		if (kv.second > best)
		{
			best = kv.second;
			origin = kv.first;
		}
	return best > 0;
}

static std::string OriginName(Link& link, uint32_t origin)
{
	auto names = link.mnd.OriginNames();
	return origin < names.size() ? names[origin] : std::to_string(origin);
}

// Hip tracker: configured serial, else a Waist/Chest role in SteamVR's
// settings (what SpaceSync uses on SteamVR). "none" turns it off.
static std::string ResolveHip(const Config& cfg)
{
	if (cfg.hip == "none")
		return "";
	if (!cfg.hip.empty())
		return cfg.hip;

	const char* home = getenv("HOME");
	if (!home)
		return "";
	std::string text = paths::ReadFile(std::string(home) + "/.local/share/Steam/config/steamvr.vrsettings");
	picojson::value root;
	if (text.empty() || !picojson::parse(root, text).empty() || !root.is<picojson::object>())
		return "";
	auto tr = root.get<picojson::object>().find("trackers");
	if (tr == root.get<picojson::object>().end() || !tr->second.is<picojson::object>())
		return "";
	std::string chest;
	for (const auto& kv : tr->second.get<picojson::object>())
	{
		if (!kv.second.is<std::string>())
			continue;
		// "/devices/htc/vive_trackerLHR-XXXXXXXX": "TrackerRole_Waist"
		std::string key = kv.first.substr(kv.first.find_last_of('/') + 1);
		if (key.rfind("vive_tracker", 0) == 0)
			key = key.substr(12);
		if (kv.second.get<std::string>() == "TrackerRole_Waist")
			return key;
		if (kv.second.get<std::string>() == "TrackerRole_Chest" && chest.empty())
			chest = key;
	}
	return chest;
}

// ---------------------------------------------------------------------------
// run: apply the calibration and keep Stay Aligned going.

static bool Near(const Pose& a, const Pose& b, double mm, double deg)
{
	return (a.p - b.p).norm() * 1000.0 <= mm && rigid::AngleDeg(a.q, b.q) <= deg;
}

static int CmdRun(bool stay)
{
	Config cfg;
	cfg.Load();
	if (!cfg.calibrated)
	{
		// Started from the session script before the first calibration:
		// wait for one instead of exiting.
		Log("not calibrated yet; waiting for 'spacesync-monado calibrate'");
		while (!g_stop && !(cfg.Load() && cfg.calibrated))
			SleepMs(2000);
		if (g_stop)
			return 1;
		SleepMs(500);
	}
	stay = stay && cfg.stayAligned;

	Link link;
	if (!Connect(link, 24 * 3600))
		return 1;

	uint32_t lh = 0;
	if (!FindLighthouseOrigin(link, cfg.origin, lh))
	{
		Log("no lighthouse tracking origin found (are the lighthouse devices on?)");
		return 1;
	}
	if (!cfg.origin.empty() && OriginName(link, lh) != cfg.origin)
		Log("calibrated origin \"%s\" not found, using \"%s\"", cfg.origin.c_str(), OriginName(link, lh).c_str());

	StayRunner runner;
	runner.reset(cfg.M);
	Pose applied = cfg.M;
	if (!link.mnd.SetOriginOffset(lh, applied))
	{
		Log("could not set the lighthouse origin offset");
		return 1;
	}
	Log("calibration from %s applied to \"%s\" (headset: %s)", cfg.date.c_str(), OriginName(link, lh).c_str(), link.head.serial.c_str());
	if (!stay)
	{
		Log("Stay Aligned is off; calibration applied, exiting");
		return 0;
	}
	Log("Stay Aligned: started");

	std::string hip = ResolveHip(cfg);
	std::string hipUsed = "<unset>";
	Pose hmdOrigin;
	bool hmdOriginKnown = link.mnd.GetOriginOffset(link.head.origin, hmdOrigin);
	double lastRefresh = 0.0, lastStatus = 0.0, lastForeign = -1e9;
	int okSteps = 0;
	int tick = 0;
	bool paused = false;
	int lockFd = open(CalibrationLockPath().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);

	const auto period = std::chrono::microseconds(11111); // ~90 Hz, the headset's pose rate
	auto next = std::chrono::steady_clock::now();
	while (!g_stop)
	{
		if (!link.xr.Poll())
		{
			Log("OpenXR session ended");
			break;
		}
		next += period;
		if (!link.xr.Running())
		{
			std::this_thread::sleep_until(next);
			continue;
		}

		XrTime now = link.xr.Now();
		double t = XrLink::Seconds(now);
		bool slowTick = (tick++ % 9) == 0; // ~10 Hz for the checks below

		if (slowTick)
		{
			bool calibrating = CalibrationInProgress(lockFd);
			if (calibrating && !paused)
				Log("Stay Aligned: paused while calibrating");
			if (!calibrating && paused)
			{
				Config fresh;
				if (fresh.Load() && fresh.calibrated)
				{
					cfg = fresh;
					runner.reset(cfg.M);
					applied = cfg.M;
					link.mnd.SetOriginOffset(lh, applied);
					hipUsed = "<unset>";
					Log("Stay Aligned: new calibration from %s loaded, resumed", cfg.date.c_str());
				}
			}
			paused = calibrating;
		}
		if (paused)
		{
			std::this_thread::sleep_until(next);
			continue;
		}

		if (t - lastRefresh > 2.0)
		{
			lastRefresh = t;
			if (link.xr.RefreshDevices())
				link.mnd.HeadDevice(link.head);
			std::string want = link.xr.HasDevice(hip) ? hip : "";
			if (want != hipUsed)
			{
				if (!want.empty())
					Log("Stay Aligned: hip tracker is %s, learning its offset below the head (stand normally for about a minute in total)", want.c_str());
				else if (!hip.empty())
					Log("Stay Aligned: hip tracker %s not connected, only headset recenters and hiccups are handled", hip.c_str());
				else
					Log("Stay Aligned: no hip tracker set (spacesync-monado set-hip SERIAL), only headset recenters and hiccups are handled");
				hipUsed = want;
				runner.resetBody();
			}
		}

		// Someone else moved the lighthouse origin (motoc, a recalibration):
		// take it as the new calibration rather than fighting over it.
		Pose current;
		if (slowTick && link.mnd.GetOriginOffset(lh, current) && !Near(current, applied, 1.0, 0.05))
		{
			if (t - lastForeign > 60.0)
				Log("the lighthouse offset was changed by another program; adopting it (is motoc still running?)");
			lastForeign = t;
			runner.reset(current);
			applied = current;
		}

		// The headset's origin offset changed: its whole frame moved.
		Pose hmdNow;
		if (slowTick && hmdOriginKnown && link.mnd.GetOriginOffset(link.head.origin, hmdNow) && !Near(hmdNow, hmdOrigin, 0.5, 0.02))
		{
			stay::Frame J = rigid::ToFrame(hmdNow * hmdOrigin.inverse());
			runner.externalShift(t, J);
			Log("Stay Aligned: headset origin moved %.2f deg / %.1f cm, lighthouse follows", stay::Deg(J.yaw), vecNorm(J.t) * 100.0);
			hmdOrigin = hmdNow;
		}

		Pose stage;
		link.mnd.GetStageOffset(stage);
		auto head = LocateRoot(link, link.head.serial, now, stage);
		if (head)
		{
			std::optional<Pose> hipNative;
			if (!hipUsed.empty())
				if (auto hipRoot = LocateRoot(link, hipUsed, now, stage))
					hipNative = applied.inverse() * *hipRoot;

			for (const auto& line : runner.step(t, *head, hipNative))
				Log("Stay Aligned: %s", line.c_str());
			okSteps++;

			Pose want = runner.originOffset();
			if (!Near(want, applied, 0.05, 0.001) && link.mnd.SetOriginOffset(lh, want))
				applied = want;
		}

		if (t - lastStatus > 60.0)
		{
			lastStatus = t;
			const auto& a = runner.state();
			stay::Frame c = a.correction();
			Log("Stay Aligned: hip %s, body model %s, correction %.2f deg / %.1f cm (sigma %.2f deg), updates %d ok / %d rejected, recenters %u, hiccups held %u, re-alignments %u, headset samples %d",
				hipUsed.empty() ? "none" : hipUsed.c_str(), a.bodyReady() ? "ready" : "learning",
				stay::Deg(c.yaw), stay::NormH(c.t) * 100.0, a.yawSigmaDeg(), a.accepted(), a.rejected(),
				a.recentersFollowed, a.jumpsHeld, a.rescues, okSteps);
			okSteps = 0;
		}

		std::this_thread::sleep_until(next);
		if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(200))
			next = std::chrono::steady_clock::now();
	}

	// Leave the calibration in place without the momentary correction.
	if (!paused)
		link.mnd.SetOriginOffset(lh, runner.calibration());
	if (lockFd >= 0)
		close(lockFd);
	Log("Stay Aligned: stopped");
	return 0;
}

// ---------------------------------------------------------------------------
// calibrate: hold a lighthouse device against the headset, follow the voice.

static int CmdCalibrate(const std::string& deviceArg, int delaySeconds)
{
	Config cfg;
	cfg.Load();

	Link link;
	if (!Connect(link, 30))
		return 1;
	sound::Init();
	// The sound worker must be joined before exit on every path.
	struct SoundGuard { ~SoundGuard() { sound::Shutdown(); } } soundGuard;

	uint32_t lh = 0;
	if (!FindLighthouseOrigin(link, cfg.origin, lh))
	{
		Log("no lighthouse tracking origin found (are the lighthouse devices on?)");
		return 1;
	}

	// Pauses a running 'spacesync-monado run'; released when we exit.
	int lockFd = open(CalibrationLockPath().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) != 0)
	{
		Log("another calibration is already running");
		return 1;
	}
	SleepMs(300); // give a running Stay Aligned a moment to pause

	// The calibration is solved against raw lighthouse coordinates.
	Pose previous;
	link.mnd.GetOriginOffset(lh, previous);
	link.mnd.SetOriginOffset(lh, Pose::Identity());
	auto restore = [&] { link.mnd.SetOriginOffset(lh, previous); };

	Log("Hold a tracker or controller firmly against your headset. Starting in %d s ...", delaySeconds);
	for (int i = 0; i < delaySeconds * 10 && !g_stop; i++)
	{
		link.xr.Poll();
		SleepMs(100);
	}

	std::vector<std::string> candidates;
	if (!deviceArg.empty())
		candidates.push_back(deviceArg);
	else
		for (const auto& d : link.mnd.Devices())
			if (d.origin == lh && link.xr.HasDevice(d.serial))
				candidates.push_back(d.serial);
	if (candidates.empty())
	{
		Log("no lighthouse tracker or controller found");
		restore();
		return 1;
	}

	// Several candidates: the one held to the head keeps a fixed rotation
	// relative to the headset while the head turns.
	std::string device = candidates[0];
	if (candidates.size() > 1)
	{
		Log("Move your head around to identify the device you are holding ...");
		sound::Play("look_left");
		sound::Play("look_right");
		std::map<std::string, std::vector<Eigen::Quaterniond>> rel;
		std::vector<Eigen::Quaterniond> headRot;
		for (int i = 0; i < 60 && !g_stop; i++)
		{
			link.xr.Poll();
			XrTime now = link.xr.Now();
			Pose stage;
			link.mnd.GetStageOffset(stage);
			if (auto h = LocateRoot(link, link.head.serial, now, stage))
			{
				headRot.push_back(h->q);
				for (const auto& c : candidates)
					if (auto d = link.xr.Locate(c, now))
						rel[c].push_back(h->q.conjugate() * d->q);
			}
			SleepMs(100);
		}
		double headTurn = 0.0;
		for (const auto& q : headRot)
			headTurn = (std::max)(headTurn, rigid::AngleDeg(headRot.front(), q));
		double bestSpread = 1e9;
		for (const auto& [serial, qs] : rel)
		{
			if (qs.size() < 20)
				continue;
			double spread = 0.0;
			for (const auto& q : qs)
				spread = (std::max)(spread, rigid::AngleDeg(qs.front(), q));
			if (spread < bestSpread)
			{
				bestSpread = spread;
				device = serial;
			}
		}
		if (headTurn < 20.0 || bestSpread > 10.0)
		{
			Log("could not tell which device is on your head (head turned %.0f deg); pass --device SERIAL", headTurn);
			restore();
			return 1;
		}
	}
	Log("calibrating with %s", device.c_str());

	// Same prompts as SpaceSync's wizard. Turning moves the device around;
	// looking up and down is what pins down the direction.
	const char* steps[] = { "look_center", "look_left", "look_center", "look_right", "look_center", "look_up", "look_center", "look_down", "look_center" };
	std::vector<CalibrationSample> samples;
	Pose lastKept, lastHead;
	bool haveKept = false, haveLast = false;
	double lastHeadT = 0.0;
	for (const char* step : steps)
	{
		if (g_stop)
			break;
		sound::Play(step);
		Log("  %s", step);
		for (int i = 0; i < 120 && !g_stop; i++) // ~4 s per step at 30 Hz
		{
			link.xr.Poll();
			XrTime now = link.xr.Now();
			Pose stage;
			link.mnd.GetStageOffset(stage);
			auto h = LocateRoot(link, link.head.serial, now, stage);
			auto d = LocateRoot(link, device, now, stage); // offset is identity: native coordinates

			// Headset and lighthouse timing never match exactly; fast turns
			// turn that into error, so only sample calm-ish movement.
			double t = XrLink::Seconds(now);
			bool calm = false;
			if (h && haveLast && t > lastHeadT)
				calm = rigid::AngleDeg(lastHead.q, h->q) / (t - lastHeadT) < 45.0;
			if (h)
			{
				lastHead = *h;
				lastHeadT = t;
				haveLast = true;
			}

			if (h && d && calm && (!haveKept || rigid::AngleDeg(lastKept.q, h->q) > 2.0 || (lastKept.p - h->p).norm() > 0.01))
			{
				samples.push_back({ *h, *d });
				lastKept = *h;
				haveKept = true;
			}
			SleepMs(33);
		}
	}
	if (g_stop)
	{
		restore();
		return 1;
	}

	CalibrationResult r = SolveCalibration(samples);
	if (!r.ok)
	{
		Log("calibration failed: %s. Previous calibration restored.", r.message.c_str());
		restore();
		return 1;
	}

	cfg.calibrated = true;
	cfg.M = r.M;
	cfg.origin = OriginName(link, lh);
	cfg.device = device;
	cfg.rmsMm = r.rmsMm;
	cfg.date = Config::Today();
	if (!cfg.Save())
	{
		Log("could not save %s", Config::Path().c_str());
		restore();
		return 1;
	}
	link.mnd.SetOriginOffset(lh, r.M);
	sound::Play("next");
	Log("calibrated: %zu samples, error %.1f mm / %.2f deg, %d tilt pairs. Saved to %s",
		samples.size(), r.rmsMm, r.rotRmsDeg, r.tiltPairs, Config::Path().c_str());
	SleepMs(1500); // let the confirmation sound finish
	return 0;
}

// ---------------------------------------------------------------------------

static int CmdList()
{
	Link link;
	if (!Connect(link, 10))
		return 1;
	auto names = link.mnd.OriginNames();
	Config cfg;
	cfg.Load();
	std::string hip = ResolveHip(cfg);
	for (uint32_t o = 0; o < names.size(); o++)
	{
		Pose off;
		link.mnd.GetOriginOffset(o, off);
		printf("origin %u \"%s\"  offset %.3f %.3f %.3f\n", o, names[o].c_str(), off.p.x(), off.p.y(), off.p.z());
		for (const auto& d : link.mnd.Devices())
			if (d.origin == o)
				printf("    [%u] %-14s %s%s%s\n", d.index, d.serial.c_str(), d.name.c_str(),
					d.serial == link.head.serial ? "  (headset)" : "", !hip.empty() && d.serial == hip ? "  (hip)" : "");
	}
	return 0;
}

static int CmdSetHip(const std::string& serial)
{
	Config cfg;
	cfg.Load();
	cfg.hip = serial;
	if (!cfg.Save())
		return 1;
	printf("hip tracker: %s\n", serial == "none" ? "off" : serial.empty() ? "SteamVR role" : serial.c_str());
	return 0;
}

static void Usage()
{
	printf(
		"spacesync-monado: SpaceSync calibration and Stay Aligned for WiVRn/Monado\n"
		"EXPERIMENTAL: not yet tested against a live WiVRn session.\n\n"
		"  calibrate [--device SERIAL] [--delay S]  hold a tracker or controller against the\n"
		"                                           headset and follow the voice prompts\n"
		"  run [--no-stay]                          apply the calibration and keep Stay Aligned\n"
		"                                           running (for wivrn-session.sh)\n"
		"  set-hip SERIAL|none|auto                 hip tracker for Stay Aligned (optional)\n"
		"  list                                     show devices and tracking origins\n\n"
		"Config: %s\nLog:    %s/monado.log\n",
		Config::Path().c_str(), paths::StateDir().c_str());
}

int main(int argc, char** argv)
{
	signal(SIGINT, [](int) { g_stop = true; });
	signal(SIGTERM, [](int) { g_stop = true; });

	std::string cmd = argc > 1 ? argv[1] : "help";
	std::string device;
	int delay = 5;
	bool stay = true;
	for (int i = 2; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--device" && i + 1 < argc)
			device = argv[++i];
		else if (a == "--delay" && i + 1 < argc)
			delay = atoi(argv[++i]);
		else if (a == "--no-stay")
			stay = false;
	}

	if (cmd == "run")
		return CmdRun(stay);
	if (cmd == "calibrate")
		return CmdCalibrate(device, delay);
	if (cmd == "list")
		return CmdList();
	if (cmd == "set-hip" && argc > 2)
		return CmdSetHip(std::string(argv[2]) == "auto" ? "" : argv[2]);
	Usage();
	return cmd == "help" || cmd == "--help" || cmd == "-h" ? 0 : 2;
}
