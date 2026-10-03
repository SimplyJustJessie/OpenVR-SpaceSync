// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// Runs SpaceSync's Stay Aligned (StayAligned.h, unchanged) against Monado.
//
// In the SteamVR driver a lighthouse pose goes through two steps: the
// calibration ("drift", lighthouse -> headset space) and then the Stay
// Aligned correction C. Both act on every lighthouse device alike, so on
// Monado they collapse into the lighthouse tracking origin's offset:
//
//     origin offset = C * M
//
// where M is the calibration as an origin offset (lighthouse native ->
// Monado root). The driver's RebaseDrift(J) (follow a headset recenter)
// becomes M = J * M, since J maps old headset-space coordinates to new ones.
// This mirrors ServerTrackedDeviceProvider::StayAlignedStep step for step.

#include "Rigid.h"

#include <optional>
#include <string>
#include <vector>

namespace monado
{
	class StayRunner
	{
	public:
		// Starts over from a calibration. Called at startup and whenever the
		// calibration changes from outside.
		void reset(const rigid::Pose& calibration)
		{
			M = calibration;
			aligner.reset();
			aligner.resetBody();
		}

		// The hip tracker changed (or went away); relearn the body model.
		void resetBody() { aligner.resetBody(); }

		// One headset sample. head is the headset pose in Monado's root space,
		// hipNative the hip tracker in lighthouse native coordinates (before
		// any origin offset), if there is a tracking hip. Returns log lines.
		std::vector<std::string> step(double t, const rigid::Pose& head, const std::optional<rigid::Pose>& hipNative)
		{
			std::vector<std::string> log;

			// The driver stores the hip after the calibration, before C.
			stay::Hip hip;
			if (hipNative)
			{
				rigid::Pose hipCal = M * *hipNative;
				hip.valid = true;
				hip.position = rigid::ToVr(hipCal.p);
				hip.rotation = rigid::ToVr(hipCal.q);
			}

			stay::Event ev = aligner.step(t, rigid::ToVr(head.q), rigid::ToVr(head.p), hip);

			if (ev.jump)
			{
				if (ev.follow)
					rebase(ev.J);
				log.push_back(Format("headset jump %.2f deg / %.1f cm (score %.0f, yaw after %.1f deg, recenter evidence %+.1f) -> %s",
					stay::Deg(ev.J.yaw), vecNorm(ev.J.t) * 100.0, ev.T, ev.yawAfterDeg, ev.lambda,
					ev.follow ? "followed (recenter)" : (ev.reverted ? "held, earlier hiccup reverted" : "held (hiccup)")));
			}
			if (ev.rescueChecked)
				log.push_back(Format("alignment check after headset pause: hip mismatch %.1f cm, yaw mismatch %.1f deg, pose looked re-centred %s -> %s",
					ev.rescueTransMisM * 100.0, ev.rescueYawMisDeg, ev.rescueCanonical ? "yes" : "no",
					ev.rebase && ev.rebaseKind == 1 ? "re-aligned from the hip tracker" : "alignment kept"));
			if (ev.rebase)
			{
				rebase(ev.K);
				if (ev.rebaseKind == 2)
					log.push_back(Format("re-alignment settled (%.2f deg / %.1f cm folded in)", stay::Deg(ev.K.yaw), stay::NormH(ev.K.t) * 100.0));
			}
			return log;
		}

		// The headset's own frame moved by J without the headset moving
		// (its tracking origin offset changed). Lighthouse follows, and the
		// aligner shifts its history, like the driver's DetectHmdFrameJump path.
		void externalShift(double t, const stay::Frame& J)
		{
			rebase(J);
			aligner.externalShift(t, J);
		}

		// What the lighthouse tracking origin offset should be right now.
		rigid::Pose originOffset() const { return rigid::FromFrame(aligner.correction()) * M; }

		const rigid::Pose& calibration() const { return M; }
		const stay::Aligner& state() const { return aligner; }

	private:
		rigid::Pose M;
		stay::Aligner aligner;

		void rebase(const stay::Frame& J) { M = rigid::FromFrame(J) * M; }

		template <typename... Args>
		static std::string Format(const char* fmt, Args... args)
		{
			char buf[320];
			std::snprintf(buf, sizeof buf, fmt, args...);
			return buf;
		}
	};
}
