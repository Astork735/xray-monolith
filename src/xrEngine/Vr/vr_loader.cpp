#include "../stdafx.h"

#ifdef VR_ENABLED

#include "vr_common.h"

// Definitions for the dynamically resolved entry points (declared in vr_loader.h).
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_p = nullptr;
#define VR_XR_DEFINE(NAME) PFN_##NAME NAME##_p = nullptr;
VR_XR_GLOBAL_FUNCTIONS(VR_XR_DEFINE)
VR_XR_INSTANCE_FUNCTIONS(VR_XR_DEFINE)
VR_XR_BODY_FUNCTIONS(VR_XR_DEFINE)
VR_XR_FIDELITY_FUNCTIONS(VR_XR_DEFINE)
VR_XR_VISIBILITY_MASK_FUNCTIONS(VR_XR_DEFINE)
#undef VR_XR_DEFINE

namespace
{
	HMODULE s_module = nullptr;

	bool Fetch(XrInstance instance, const char* name, PFN_xrVoidFunction* out)
	{
		XrResult result = xrGetInstanceProcAddr_p(instance, name, out);
		if (XR_FAILED(result) || !*out)
		{
			Msg("! [VR] entry point not available: %s (%s)", name, vr::ResultToString(result));
			return false;
		}
		return true;
	}
}

namespace vr
{
	bool OpenXRLoaded()
	{
		return s_module != nullptr && xrGetInstanceProcAddr_p != nullptr;
	}

	bool LoadOpenXR()
	{
		if (s_module)
			return true;

		// Only reached when VR was actually requested, so a missing/openable DLL
		// here never affects normal desktop startup.
		s_module = LoadLibraryA("openxr_loader.dll");
		if (!s_module)
		{
			Msg("! [VR] openxr_loader.dll not present - native VR unavailable");
			return false;
		}

		xrGetInstanceProcAddr_p =
			reinterpret_cast<PFN_xrGetInstanceProcAddr>(
				reinterpret_cast<void*>(GetProcAddress(s_module, "xrGetInstanceProcAddr")));
		if (!xrGetInstanceProcAddr_p)
		{
			Msg("! [VR] openxr_loader.dll has no xrGetInstanceProcAddr");
			UnloadOpenXR();
			return false;
		}

		bool ok = true;
#define VR_FETCH(NAME) ok &= Fetch(XR_NULL_HANDLE, #NAME, reinterpret_cast<PFN_xrVoidFunction*>(&NAME##_p));
		VR_XR_GLOBAL_FUNCTIONS(VR_FETCH)
#undef VR_FETCH

		if (!ok)
		{
			UnloadOpenXR();
			return false;
		}

		Msg("* [VR] openxr_loader.dll loaded");
		return true;
	}

	bool LoadOpenXRInstance(XrInstance instance)
	{
		if (!OpenXRLoaded())
			return false;

		bool ok = true;
#define VR_FETCH(NAME) ok &= Fetch(instance, #NAME, reinterpret_cast<PFN_xrVoidFunction*>(&NAME##_p));
		VR_XR_INSTANCE_FUNCTIONS(VR_FETCH)
#undef VR_FETCH

		if (!ok)
			Msg("! [VR] one or more OpenXR entry points are missing");

		return ok;
	}

	bool LoadOpenXRBodyTracking(XrInstance instance, bool fidelity)
	{
		if (!OpenXRLoaded())
			return false;

		bool ok = true;
#define VR_FETCH(NAME) ok &= Fetch(instance, #NAME, reinterpret_cast<PFN_xrVoidFunction*>(&NAME##_p));
		VR_XR_BODY_FUNCTIONS(VR_FETCH)
#undef VR_FETCH
		// High fidelity is only asked for: without it the tracker still runs.
		if (ok && fidelity)
		{
#define VR_FETCH(NAME) Fetch(instance, #NAME, reinterpret_cast<PFN_xrVoidFunction*>(&NAME##_p));
			VR_XR_FIDELITY_FUNCTIONS(VR_FETCH)
#undef VR_FETCH
		}
		if (!ok)
		{
#define VR_CLEAR(NAME) NAME##_p = nullptr;
			VR_XR_BODY_FUNCTIONS(VR_CLEAR)
			VR_XR_FIDELITY_FUNCTIONS(VR_CLEAR)
#undef VR_CLEAR
		}
		return ok;
	}

	bool LoadOpenXRVisibilityMask(XrInstance instance)
	{
		if (!OpenXRLoaded())
			return false;
		bool ok = true;
#define VR_FETCH(NAME) ok &= Fetch(instance, #NAME, reinterpret_cast<PFN_xrVoidFunction*>(&NAME##_p));
		VR_XR_VISIBILITY_MASK_FUNCTIONS(VR_FETCH)
#undef VR_FETCH
		if (!ok)
		{
#define VR_CLEAR(NAME) NAME##_p = nullptr;
			VR_XR_VISIBILITY_MASK_FUNCTIONS(VR_CLEAR)
#undef VR_CLEAR
		}
		return ok;
	}

	void UnloadOpenXR()
	{
		xrGetInstanceProcAddr_p = nullptr;
#define VR_CLEAR(NAME) NAME##_p = nullptr;
		VR_XR_GLOBAL_FUNCTIONS(VR_CLEAR)
		VR_XR_INSTANCE_FUNCTIONS(VR_CLEAR)
		VR_XR_BODY_FUNCTIONS(VR_CLEAR)
		VR_XR_FIDELITY_FUNCTIONS(VR_CLEAR)
		VR_XR_VISIBILITY_MASK_FUNCTIONS(VR_CLEAR)
#undef VR_CLEAR

		if (s_module)
		{
			FreeLibrary(s_module);
			s_module = nullptr;
		}
	}
}

#endif // VR_ENABLED
