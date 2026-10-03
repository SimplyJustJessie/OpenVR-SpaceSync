// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Monado companion). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

#pragma once

// One-time calibration without a head tracker: a lighthouse device is held
// rigidly against the headset while the head moves. Every sample satisfies
//
//     head_i = M * device_i * X
//
// with M the lighthouse -> headset-space transform (what becomes the
// lighthouse tracking origin offset) and X the unknown device -> head offset.
// Both spaces are gravity aligned, so M is levelled to yaw + translation,
// like SpaceSync's no-tracker calibration (LevelledRotation).
//
//  1. Yaw: relative rotations satisfy  Rh_i Rh_j^-1 = Rm (Rd_i Rd_j^-1) Rm^-1,
//     so their axes differ by Rm alone. Fit the yaw that best maps the device
//     axes onto the headset axes. Only tilting/nodding (horizontal axes)
//     carries yaw information; turning left/right does not.
//  2. X rotation: average of Rd_i^-1 Rm^-1 Rh_i.
//  3. Translations: ph_i = Rm (pd_i + Rd_i tx) + tm is linear in (tm, tx);
//     solve the least squares problem.

#include "Rigid.h"

#include <string>
#include <vector>

namespace monado
{
	struct CalibrationSample
	{
		rigid::Pose head;   // headset in Monado root space
		rigid::Pose device; // held device in lighthouse native space
	};

	struct CalibrationResult
	{
		bool ok = false;
		std::string message;
		rigid::Pose M;        // lighthouse native -> root (yaw + translation)
		rigid::Pose X;        // device -> head offset while it was held
		double rmsMm = 0.0;   // position residual
		double rotRmsDeg = 0.0;
		int tiltPairs = 0;    // sample pairs with a nod/tilt, which is what fixes the yaw
	};

	inline CalibrationResult SolveCalibration(const std::vector<CalibrationSample>& s)
	{
		CalibrationResult r;
		const size_t n = s.size();
		if (n < 20)
		{
			r.message = "not enough samples (" + std::to_string(n) + "), move your head more";
			return r;
		}

		// 1. Yaw from rotation axes of sample pairs.
		double sumCos = 0.0, sumSin = 0.0;
		int tiltPairs = 0;
		for (size_t i = 0; i < n; i++)
		{
			for (size_t j = i + 1; j < n; j += 3)
			{
				Eigen::AngleAxisd dh((s[i].head.q * s[j].head.q.conjugate()).normalized());
				Eigen::AngleAxisd dd((s[i].device.q * s[j].device.q.conjugate()).normalized());
				double angle = dh.angle();
				if (angle < 10.0 * POSE_PI / 180.0 || angle > 150.0 * POSE_PI / 180.0)
					continue;
				// Same rotation seen from both systems; skip pairs where they disagree.
				if (std::fabs(angle - dd.angle()) > 5.0 * POSE_PI / 180.0)
					continue;
				Eigen::Vector3d h = dh.axis(), d = dd.axis();
				double w = std::sin(angle * 0.5);
				// maximise sum h . RotY(yaw) d over the horizontal plane
				sumCos += w * (h.x() * d.x() + h.z() * d.z());
				sumSin += w * (h.x() * d.z() - h.z() * d.x());
				if (std::sqrt(d.x() * d.x() + d.z() * d.z()) > 0.5)
					tiltPairs++;
			}
		}
		if (tiltPairs < 30)
		{
			r.message = "not enough up/down head movement to find the direction; nod and tilt your head more";
			return r;
		}
		double yaw = std::atan2(sumSin, sumCos);
		r.tiltPairs = tiltPairs;
		Eigen::Quaterniond Rm = rigid::FromYaw(yaw);

		// 2. Device -> head rotation.
		Eigen::Vector4d acc = Eigen::Vector4d::Zero();
		Eigen::Quaterniond first = (s[0].device.q.conjugate() * Rm.conjugate() * s[0].head.q).normalized();
		for (const auto& smp : s)
		{
			Eigen::Quaterniond x = (smp.device.q.conjugate() * Rm.conjugate() * smp.head.q).normalized();
			if (x.dot(first) < 0.0)
				x.coeffs() = -x.coeffs();
			acc += x.coeffs();
		}
		Eigen::Quaterniond Rx;
		Rx.coeffs() = acc.normalized();

		// 3. Translations, least squares in (tm, tx).
		Eigen::Matrix<double, 6, 6> AtA = Eigen::Matrix<double, 6, 6>::Zero();
		Eigen::Matrix<double, 6, 1> Atb = Eigen::Matrix<double, 6, 1>::Zero();
		for (const auto& smp : s)
		{
			Eigen::Matrix<double, 3, 6> A;
			A.leftCols<3>() = Eigen::Matrix3d::Identity();
			A.rightCols<3>() = (Rm * smp.device.q).toRotationMatrix();
			Eigen::Vector3d b = smp.head.p - Rm * smp.device.p;
			AtA += A.transpose() * A;
			Atb += A.transpose() * b;
		}
		Eigen::JacobiSVD<Eigen::Matrix<double, 6, 6>> svd(AtA, Eigen::ComputeFullU | Eigen::ComputeFullV);
		double cond = svd.singularValues()(0) / (std::max)(svd.singularValues()(5), 1e-12);
		if (cond > 1e6)
		{
			r.message = "head movement too uniform to place the device; turn and tilt your head in all directions";
			return r;
		}
		Eigen::Matrix<double, 6, 1> sol = svd.solve(Atb);

		r.M.q = Rm;
		r.M.p = sol.head<3>();
		r.X.q = Rx;
		r.X.p = sol.tail<3>();

		double sq = 0.0, rq = 0.0;
		for (const auto& smp : s)
		{
			rigid::Pose predicted = r.M * smp.device * r.X;
			sq += (predicted.p - smp.head.p).squaredNorm();
			double a = rigid::AngleDeg(predicted.q, smp.head.q);
			rq += a * a;
		}
		r.rmsMm = std::sqrt(sq / n) * 1000.0;
		r.rotRmsDeg = std::sqrt(rq / n);

		if (r.rmsMm > 50.0 || r.rotRmsDeg > 5.0)
		{
			char buf[200];
			std::snprintf(buf, sizeof buf, "calibration error too high (%.0f mm / %.1f deg); hold the device firmly against the headset and move slowly", r.rmsMm, r.rotRmsDeg);
			r.message = buf;
			return r;
		}
		r.ok = true;
		return r;
	}
}
