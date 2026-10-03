// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

// Synthetic tests for the Monado companion's math: the calibration solver and
// the Stay Aligned runner's frame handling. No VR runtime needed.

#include "CalibrationSolver.h"
#include "StayRunner.h"

#include <cstdio>
#include <random>

using monado::CalibrationSample;
using rigid::Pose;

static int failures = 0;
#define CHECK(cond, ...) do { std::printf("%s  ", (cond) ? "PASS" : "FAIL"); std::printf(__VA_ARGS__); std::printf("\n"); if (!(cond)) failures++; } while (0)

static std::mt19937 rng(1234);

static double Gauss(double sigma) { return std::normal_distribution<double>(0.0, sigma)(rng); }

static Pose RandomPose(double maxYawDeg, double tilt, double pos)
{
	std::uniform_real_distribution<double> u(-1.0, 1.0);
	Pose p;
	p.q = rigid::FromYaw(u(rng) * maxYawDeg * POSE_PI / 180.0)
		* Eigen::AngleAxisd(u(rng) * tilt, Eigen::Vector3d::UnitX())
		* Eigen::AngleAxisd(u(rng) * tilt, Eigen::Vector3d::UnitZ());
	p.p = { u(rng) * pos, u(rng) * pos, u(rng) * pos };
	return p;
}

static Pose Noisy(const Pose& p, double posSigma, double rotSigmaDeg)
{
	Pose r = p;
	double s = rotSigmaDeg * POSE_PI / 180.0;
	r.q = (p.q * Eigen::AngleAxisd(Gauss(s), Eigen::Vector3d::UnitX())
		* Eigen::AngleAxisd(Gauss(s), Eigen::Vector3d::UnitY())
		* Eigen::AngleAxisd(Gauss(s), Eigen::Vector3d::UnitZ())).normalized();
	r.p += Eigen::Vector3d(Gauss(posSigma), Gauss(posSigma), Gauss(posSigma));
	return r;
}

// Head motion like the calibration wizard: look around, nod, tilt.
static std::vector<Pose> WizardHeadPath(bool allowTilt)
{
	std::vector<Pose> path;
	Eigen::Vector3d standing(0.2, 1.65, -0.4);
	for (int i = 0; i < 300; i++)
	{
		double ph = i / 300.0 * 2.0 * POSE_PI;
		double yaw = 0.7 * std::sin(ph * 2.0);
		double pitch = allowTilt ? 0.5 * std::sin(ph * 3.0 + 1.0) : 0.0;
		double roll = allowTilt ? 0.3 * std::sin(ph * 5.0) : 0.0;
		Pose h;
		h.q = rigid::FromYaw(yaw) * Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitX()) * Eigen::AngleAxisd(roll, Eigen::Vector3d::UnitZ());
		h.p = standing + Eigen::Vector3d(0.05 * std::sin(ph), 0.02 * std::sin(2 * ph), 0.05 * std::cos(ph));
		path.push_back(h);
	}
	return path;
}

static void TestCalibration()
{
	for (int trial = 0; trial < 5; trial++)
	{
		Pose M = RandomPose(180.0, 0.0, 3.0);  // levelled: yaw + translation
		Pose X = RandomPose(180.0, 1.0, 0.12); // device somewhere on the head
		std::vector<CalibrationSample> samples;
		for (const Pose& head : WizardHeadPath(true))
		{
			Pose device = M.inverse() * head * X.inverse();
			samples.push_back({ Noisy(head, 0.0007, 0.08), Noisy(device, 0.0005, 0.05) });
		}
		auto r = monado::SolveCalibration(samples);
		double yawErr = rigid::AngleDeg(r.M.q, M.q);
		double posErrMm = (r.M.p - M.p).norm() * 1000.0;
		CHECK(r.ok && yawErr < 0.3 && posErrMm < 5.0,
			"calibration trial %d: yaw error %.3f deg, position error %.2f mm, residual %.2f mm %s",
			trial, yawErr, posErrMm, r.rmsMm, r.message.c_str());
	}

	// Only turning left/right: the yaw can't be determined.
	{
		Pose M = RandomPose(180.0, 0.0, 3.0), X = RandomPose(180.0, 1.0, 0.12);
		std::vector<CalibrationSample> samples;
		for (const Pose& head : WizardHeadPath(false))
			samples.push_back({ head, M.inverse() * head * X.inverse() });
		auto r = monado::SolveCalibration(samples);
		CHECK(!r.ok, "turning only is rejected (%s)", r.message.c_str());
	}

	// The device was not held against the head (moves independently).
	{
		Pose M = RandomPose(180.0, 0.0, 3.0);
		std::vector<CalibrationSample> samples;
		for (const Pose& head : WizardHeadPath(true))
			samples.push_back({ head, RandomPose(180.0, 1.0, 1.0) });
		auto r = monado::SolveCalibration(samples);
		CHECK(!r.ok, "device not on the head is rejected (%s)", r.message.c_str());
	}
}

// Simulates standing with a hip tracker, then a headset recenter: the headset's
// tracking frame jumps by J while the person does not move. Stay Aligned should
// follow it so the hip tracker stays under the head in the new frame.
static void TestRecenterFollowed()
{
	Pose M = RandomPose(180.0, 0.0, 2.0);
	monado::StayRunner runner;
	runner.reset(M);

	const double rate = 90.0;
	Pose J;
	J.q = rigid::FromYaw(25.0 * POSE_PI / 180.0);
	J.p = { 0.30, 0.0, -0.20 };

	double t = 0.0;
	int followed = 0;
	auto simulate = [&](double seconds, const Pose& slamFromWorld)
	{
		for (int i = 0; i < (int)(seconds * rate); i++, t += 1.0 / rate)
		{
			// Standing, swaying a little, head looking around slowly.
			Pose headWorld;
			headWorld.q = rigid::FromYaw(0.3 * std::sin(t * 0.4)) * Eigen::AngleAxisd(0.1 * std::sin(t * 0.7), Eigen::Vector3d::UnitX());
			headWorld.p = Eigen::Vector3d(0.02 * std::sin(t * 0.5), 1.65 + 0.005 * std::sin(t * 1.3), 0.02 * std::cos(t * 0.3));
			Pose hipWorld;
			hipWorld.q = rigid::FromYaw(0.1 * std::sin(t * 0.4));
			hipWorld.p = headWorld.p + Eigen::Vector3d(0.0, -0.65, 0.05);

			// The headset reports in its SLAM frame; the hip tracker in lighthouse space,
			// which the calibration M maps onto the original SLAM frame.
			Pose head = Noisy(slamFromWorld * headWorld, 0.0003, 0.02);
			Pose hipNative = Noisy(M.inverse() * hipWorld, 0.0003, 0.02);
			for (const auto& line : runner.step(t, head, hipNative))
			{
				std::printf("      t=%.1fs %s\n", t, line.c_str());
				if (line.find("followed") != std::string::npos)
					followed++;
			}
		}
	};

	simulate(120.0, Pose::Identity()); // learn the body model
	CHECK(runner.state().bodyReady(), "body model learned while standing");
	simulate(20.0, J);                  // recenter: SLAM frame jumped by J

	// Where does the hip tracker end up relative to the head now?
	Pose headWorld; headWorld.p = { 0.0, 1.65, 0.0 };
	Pose hipWorld; hipWorld.p = { 0.0, 1.0, 0.05 };
	Pose headSlam = J * headWorld;
	Pose hipRoot = runner.originOffset() * (M.inverse() * hipWorld);
	Eigen::Vector3d rel = headSlam.inverse().apply(hipRoot.p);
	Eigen::Vector3d expected = headWorld.inverse().apply(hipWorld.p);
	double errCm = (rel - expected).norm() * 100.0;
	CHECK(followed >= 1, "recenter detected and followed (%d)", followed);
	CHECK(errCm < 3.0, "hip stays under the head after the recenter (off by %.1f cm)", errCm);
}

// A brief hiccup while facing away from the play area's forward direction:
// the SLAM frame jumps and comes back. Lighthouse must not move. (Stay Aligned
// judges recenters by whether the jump turns the heading back to straight
// ahead, so a hiccup while facing forward can be taken for a recenter.)
static void TestHiccupHeld()
{
	Pose M = RandomPose(180.0, 0.0, 2.0);
	monado::StayRunner runner;
	runner.reset(M);
	Pose J;
	J.q = rigid::FromYaw(8.0 * POSE_PI / 180.0);
	J.p = { 0.10, 0.0, 0.05 };
	double t = 0.0;
	auto simulate = [&](double seconds, const Pose& slam)
	{
		for (int i = 0; i < (int)(seconds * 90.0); i++, t += 1.0 / 90.0)
		{
			Pose headWorld;
			headWorld.q = rigid::FromYaw(1.0 + 0.3 * std::sin(t * 0.4));
			headWorld.p = Eigen::Vector3d(0.02 * std::sin(t * 0.5), 1.65, 0.0);
			Pose hipWorld; hipWorld.p = headWorld.p + Eigen::Vector3d(0, -0.65, 0.05);
			for (const auto& line : runner.step(t, Noisy(slam * headWorld, 0.0003, 0.02), Noisy(M.inverse() * hipWorld, 0.0003, 0.02)))
				std::printf("      t=%.1fs %s\n", t, line.c_str());
		}
	};
	simulate(120.0, Pose::Identity());
	simulate(0.4, J);
	simulate(20.0, Pose::Identity());
	Pose drift = runner.originOffset() * M.inverse();
	double moved = drift.p.norm() * 100.0, turned = rigid::AngleDeg(drift.q, Eigen::Quaterniond::Identity());
	CHECK(moved < 2.0 && turned < 1.0, "hiccup leaves lighthouse in place (moved %.1f cm, %.2f deg)", moved, turned);
}

int main()
{
	TestCalibration();
	TestRecenterFollowed();
	TestHiccupHeld();
	std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
