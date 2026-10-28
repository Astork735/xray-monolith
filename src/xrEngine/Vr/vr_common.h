#ifndef xrEngine_vr_common_h
#define xrEngine_vr_common_h
#pragma once

// ---------------------------------------------------------------------------
// Native OpenXR VR - shared glue.
//
// Everything in src/xrEngine/Vr/ is compiled only when VR_ENABLED is defined
// (the DX11 / R4 build configurations). On every other target this header and
// its users reduce to nothing, so desktop-only and dedicated-server builds are
// byte-for-byte unaffected.
// ---------------------------------------------------------------------------

#include "../../build_config_defines.h"

#ifdef VR_ENABLED

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>

// Suppress the OpenXR prototypes: we resolve every entry point dynamically from
// openxr_loader.dll at runtime (see vr_loader.h) so the game has no load-time
// dependency on the loader - desktop startup works with OpenXR absent. The
// Xr*/PFN_xr* typedefs and constants are still provided.
#define XR_NO_PROTOTYPES

// These must be defined before <openxr_platform.h> so it declares the
// Win32 + D3D11 extension structures (XrGraphicsBindingD3D11KHR, ...).
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vr_loader.h"

namespace vr
{
	// Human readable XrResult, including the runtime's own message where possible.
	const char* ResultToString(XrResult result);

	// Logs and returns true on XR_SUCCESS.
	bool Succeeded(XrResult result, const char* what);

	// The single supported graphics binding for now (DX11).
	inline XrGraphicsBindingD3D11KHR MakeD3D11Binding(ID3D11Device* device)
	{
		XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
		binding.device = device;
		return binding;
	}
}

// Hard failure of an OpenXR call: logs the call + result and returns false
// from the enclosing bool function.
#define VR_XR_CHECK(expr, what)                       \
	do {                                              \
		XrResult _vr_res = (expr);                    \
		if (!vr::Succeeded(_vr_res, (what)))          \
			return false;                             \
	} while (0)

#endif // VR_ENABLED
#endif // xrEngine_vr_common_h
