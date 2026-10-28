#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif

#ifdef VR_ENABLED

#include "vr_tracking.h"
#include "vr_config.h"

namespace vr
{
	static inline Fvector AxisRemap(float x, float y, float z)
	{
		// Reflect Z: X-Ray is left-handed, +Y up and +Z forward.
		return Fvector().set(x, y, -z);
	}

	Fvector ToEnginePosition(const XrVector3f& p)
	{
		Fvector r = AxisRemap(p.x, p.y, p.z);
		r.mul(vr_world_scale);
		return r;
	}

	void ToEngineBasis(const XrQuaternionf& q,
	                   Fvector& out_right, Fvector& out_up, Fvector& out_forward)
	{
		const float x = q.x, y = q.y, z = q.z, w = q.w;

		// Rotation matrix from quaternion (columns are the local axes in world space).
		const float c00 = 1.f - 2.f * (y * y + z * z);
		const float c01 = 2.f * (x * y - w * z);
		const float c02 = 2.f * (x * z + w * y);

		const float c10 = 2.f * (x * y + w * z);
		const float c11 = 1.f - 2.f * (x * x + z * z);
		const float c12 = 2.f * (y * z - w * x);

		const float c20 = 2.f * (x * z - w * y);
		const float c21 = 2.f * (y * z + w * x);
		const float c22 = 1.f - 2.f * (x * x + y * y);

		// world_right   = R * (1,0,0)  = column 0
		// world_up      = R * (0,1,0)  = column 1
		// world_forward = R * (0,0,-1) = -column 2   (OpenXR forward is -Z)
		const Fvector right = AxisRemap(c00, c10, c20);
		const Fvector up = AxisRemap(c01, c11, c21);
		Fvector forward = AxisRemap(-c02, -c12, -c22);

		out_right = right;
		out_up = up;
		out_forward = forward;
	}

	Fmatrix ToEngineView(const XrPosef& pose,
	                     Fvector& out_position, Fvector& out_direction, Fvector& out_up)
	{
		Fvector right, up, forward;
		ToEngineBasis(pose.orientation, right, up, forward);

		const Fvector pos = ToEnginePosition(pose.position);
		out_position = pos;
		out_direction = forward;
		out_up = up;

		// Reuse the engine's view builder so handedness matches the renderer.
		Fmatrix view;
		view.build_camera_dir(pos, forward, up);
		return view;
	}

	Fmatrix MakeAsymmetricProjection(const XrFovf& fov, float near_plane, float far_plane)
	{
		// Signed tangents of the runtime-provided asymmetric frustum bounds.
		float l = tanf(fov.angleLeft);
		float r = tanf(fov.angleRight);
		float b = tanf(fov.angleDown);
		float t = tanf(fov.angleUp);
		if (l > r) { const float s = l; l = r; r = s; }
		if (b > t) { const float s = b; b = t; t = s; }

		const float w = r - l; // > 0
		const float h = t - b; // > 0

		// Off-centre perspective (D3D, row-vector convention; _ij == m[i-1][j-1]).
		// x' = x*(2/w) + z*(-(r+l)/w), y' = y*(2/h) + z*(-(t+b)/h).
		Fmatrix m;
		m._11 = (w > EPS_S) ? (2.f / w) : 1.f;
		m._12 = 0.f;
		m._13 = 0.f;
		m._14 = 0.f;

		m._21 = 0.f;
		m._22 = (h > EPS_S) ? (2.f / h) : 1.f;
		m._23 = 0.f;
		m._24 = 0.f;

		m._31 = (w > EPS_S) ? (-(r + l) / w) : 0.f;
		m._32 = (h > EPS_S) ? (-(t + b) / h) : 0.f;
		m._33 = far_plane / (far_plane - near_plane);
		m._34 = 1.f;

		m._41 = 0.f;
		m._42 = 0.f;
		m._43 = -m._33 * near_plane;
		m._44 = 0.f;

		return m;
	}

}
#endif
