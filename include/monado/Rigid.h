// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// Rigid transforms for the Monado companion, plus conversions to the
// OpenVR math types the shared Stay Aligned code (StayAligned.h) uses.
// Conventions match OpenVR and OpenXR: Y up, -Z forward, right-handed,
// positive yaw rotates about +Y.

#include "PoseMath.h"
#include "StayAligned.h"

#include <Dense>

#include <cmath>

namespace rigid
{
	struct Pose
	{
		Eigen::Quaterniond q = Eigen::Quaterniond::Identity();
		Eigen::Vector3d p = Eigen::Vector3d::Zero();

		static Pose Identity() { return {}; }

		Eigen::Vector3d apply(const Eigen::Vector3d& v) const { return q * v + p; }

		Pose inverse() const
		{
			Pose r;
			r.q = q.conjugate();
			r.p = -(r.q * p);
			return r;
		}
	};

	// (a * b) applies b first, then a.
	inline Pose operator*(const Pose& a, const Pose& b)
	{
		Pose r;
		r.q = (a.q * b.q).normalized();
		r.p = a.q * b.p + a.p;
		return r;
	}

	inline double Yaw(const Eigen::Quaterniond& q)
	{
		Eigen::Vector3d f = q * Eigen::Vector3d(0, 0, -1);
		return std::atan2(-f.x(), -f.z());
	}

	inline Eigen::Quaterniond FromYaw(double yaw)
	{
		return Eigen::Quaterniond(Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitY()));
	}

	inline double AngleDeg(const Eigen::Quaterniond& a, const Eigen::Quaterniond& b)
	{
		return Eigen::AngleAxisd((a.conjugate() * b).normalized()).angle() * 180.0 / POSE_PI;
	}

	// Stay Aligned works in yaw + translation frames.
	inline Pose FromFrame(const stay::Frame& f)
	{
		Pose r;
		r.q = FromYaw(f.yaw);
		r.p = { f.t.v[0], f.t.v[1], f.t.v[2] };
		return r;
	}

	// Keeps only the yaw of the rotation; pitch/roll are dropped.
	inline stay::Frame ToFrame(const Pose& p)
	{
		return { Yaw(p.q), { p.p.x(), p.p.y(), p.p.z() } };
	}

	inline vr::HmdQuaternion_t ToVr(const Eigen::Quaterniond& q) { return { q.w(), q.x(), q.y(), q.z() }; }
	inline vr::HmdVector3d_t ToVr(const Eigen::Vector3d& v) { return { v.x(), v.y(), v.z() }; }
}
