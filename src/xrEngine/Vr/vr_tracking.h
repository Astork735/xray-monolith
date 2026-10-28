#ifndef xrEngine_vr_tracking_h
#define xrEngine_vr_tracking_h
#pragma once

#include "vr_common.h"

#ifdef VR_ENABLED

// ---------------------------------------------------------------------------
// Coordinate conversion between OpenXR and X-Ray.
//
// OpenXR : right-handed, +X right, +Y up, -Z forward, metres, quaternion (x,y,z,w)
// X-Ray  : left-handed, +X right, +Y up, +Z forward, metres.
// Positions reflect Z: (xr.x, xr.y, -xr.z). Quaternion basis vectors are
// transformed by the same reflection. Fmatrix stores row vectors; translation
// occupies _41.._43 and off-centre projection terms occupy _31 and _32.
// ---------------------------------------------------------------------------
namespace vr
{
	// Z reflection + world scale for a translation.
	Fvector ToEnginePosition(const XrVector3f& p);

	// Remap the orientation to engine basis vectors (unit length).
	void ToEngineBasis(const XrQuaternionf& q,
	                   Fvector& out_right, Fvector& out_up, Fvector& out_forward);

	// Build an engine view (world->view) matrix exactly like CCameraFirstEye /
	// CCameraManager do, from a head pose. Also reports position/dir/up.
	Fmatrix ToEngineView(const XrPosef& pose,
	                     Fvector& out_position, Fvector& out_direction, Fvector& out_up);

	// Asymmetric perspective projection from an OpenXR FOV, matching the layout
	// of Fmatrix::build_projection_HAT (row vectors, _ij == m[i-1][j-1]).
	Fmatrix MakeAsymmetricProjection(const XrFovf& fov, float near_plane, float far_plane);
}

#endif // VR_ENABLED
#endif // xrEngine_vr_tracking_h
