#include "../stdafx.h"

#ifdef VR_ENABLED

#include "vr_common.h"

// Definitions for the dynamically resolved entry points (declared in vr_loader.h).
PFN_xrGetInstanceProcAddr                   xrGetInstanceProcAddr_p = nullptr;
PFN_xrCreateInstance                        xrCreateInstance_p = nullptr;
PFN_xrEnumerateInstanceExtensionProperties  xrEnumerateInstanceExtensionProperties_p = nullptr;
PFN_xrGetInstanceProperties                 xrGetInstanceProperties_p = nullptr;
PFN_xrGetSystem                             xrGetSystem_p = nullptr;
PFN_xrCreateSession                         xrCreateSession_p = nullptr;
PFN_xrDestroySession                        xrDestroySession_p = nullptr;
PFN_xrDestroyInstance                       xrDestroyInstance_p = nullptr;
PFN_xrCreateReferenceSpace                  xrCreateReferenceSpace_p = nullptr;
PFN_xrDestroySpace                          xrDestroySpace_p = nullptr;
PFN_xrBeginSession                          xrBeginSession_p = nullptr;
PFN_xrEndSession                            xrEndSession_p = nullptr;
PFN_xrPollEvent                             xrPollEvent_p = nullptr;
PFN_xrWaitFrame                             xrWaitFrame_p = nullptr;
PFN_xrBeginFrame                            xrBeginFrame_p = nullptr;
PFN_xrLocateViews                           xrLocateViews_p = nullptr;
PFN_xrEndFrame                              xrEndFrame_p = nullptr;
PFN_xrEnumerateSwapchainFormats             xrEnumerateSwapchainFormats_p = nullptr;
PFN_xrEnumerateViewConfigurationViews       xrEnumerateViewConfigurationViews_p = nullptr;
PFN_xrCreateSwapchain                       xrCreateSwapchain_p = nullptr;
PFN_xrEnumerateSwapchainImages              xrEnumerateSwapchainImages_p = nullptr;
PFN_xrDestroySwapchain                      xrDestroySwapchain_p = nullptr;
PFN_xrAcquireSwapchainImage                 xrAcquireSwapchainImage_p = nullptr;
PFN_xrWaitSwapchainImage                    xrWaitSwapchainImage_p = nullptr;
PFN_xrReleaseSwapchainImage                 xrReleaseSwapchainImage_p = nullptr;
PFN_xrCreateActionSet                       xrCreateActionSet_p = nullptr;
PFN_xrDestroyActionSet                      xrDestroyActionSet_p = nullptr;
PFN_xrStringToPath                          xrStringToPath_p = nullptr;
PFN_xrCreateAction                          xrCreateAction_p = nullptr;
PFN_xrCreateActionSpace                     xrCreateActionSpace_p = nullptr;
PFN_xrSuggestInteractionProfileBindings     xrSuggestInteractionProfileBindings_p = nullptr;
PFN_xrSyncActions                           xrSyncActions_p = nullptr;
PFN_xrAttachSessionActionSets               xrAttachSessionActionSets_p = nullptr;
PFN_xrGetActionStateBoolean xrGetActionStateBoolean_p = nullptr;
PFN_xrGetActionStateFloat                   xrGetActionStateFloat_p = nullptr;
PFN_xrGetActionStateVector2f                xrGetActionStateVector2f_p = nullptr;
PFN_xrGetActionStatePose                    xrGetActionStatePose_p = nullptr;
PFN_xrLocateSpace                           xrLocateSpace_p = nullptr;
PFN_xrApplyHapticFeedback                   xrApplyHapticFeedback_p = nullptr;

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
		ok &= Fetch(XR_NULL_HANDLE, "xrCreateInstance",
			reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance_p));
		ok &= Fetch(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
			reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties_p));

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
#define VR_FETCH(NAME) ok &= Fetch(instance, #NAME, reinterpret_cast<PFN_xrVoidFunction*>(&NAME##_p))
		VR_FETCH(xrGetInstanceProperties);
		VR_FETCH(xrGetSystem);
		VR_FETCH(xrCreateSession);
		VR_FETCH(xrDestroySession);
		VR_FETCH(xrDestroyInstance);
		VR_FETCH(xrCreateReferenceSpace);
		VR_FETCH(xrDestroySpace);
		VR_FETCH(xrBeginSession);
		VR_FETCH(xrEndSession);
		VR_FETCH(xrPollEvent);
		VR_FETCH(xrWaitFrame);
		VR_FETCH(xrBeginFrame);
		VR_FETCH(xrLocateViews);
		VR_FETCH(xrEndFrame);
		VR_FETCH(xrEnumerateSwapchainFormats);
		VR_FETCH(xrEnumerateViewConfigurationViews);
		VR_FETCH(xrCreateSwapchain);
		VR_FETCH(xrEnumerateSwapchainImages);
		VR_FETCH(xrDestroySwapchain);
		VR_FETCH(xrAcquireSwapchainImage);
		VR_FETCH(xrWaitSwapchainImage);
		VR_FETCH(xrReleaseSwapchainImage);
		VR_FETCH(xrCreateActionSet);
		VR_FETCH(xrDestroyActionSet);
		VR_FETCH(xrStringToPath);
		VR_FETCH(xrCreateAction);
		VR_FETCH(xrCreateActionSpace);
		VR_FETCH(xrSuggestInteractionProfileBindings);
		VR_FETCH(xrSyncActions);
		VR_FETCH(xrAttachSessionActionSets);
		VR_FETCH(xrGetActionStateBoolean);
		VR_FETCH(xrGetActionStateFloat);
		VR_FETCH(xrGetActionStateVector2f);
		VR_FETCH(xrGetActionStatePose);
		VR_FETCH(xrLocateSpace);
		VR_FETCH(xrApplyHapticFeedback);
#undef VR_FETCH

		if (!ok)
			Msg("! [VR] one or more OpenXR entry points are missing");

		return ok;
	}

	void UnloadOpenXR()
	{
		xrGetInstanceProcAddr_p = nullptr;
		xrCreateInstance_p = nullptr;
		xrEnumerateInstanceExtensionProperties_p = nullptr;
		xrGetInstanceProperties_p = nullptr;
		xrGetSystem_p = nullptr;
		xrCreateSession_p = nullptr;
		xrDestroySession_p = nullptr;
		xrDestroyInstance_p = nullptr;
		xrCreateReferenceSpace_p = nullptr;
		xrDestroySpace_p = nullptr;
		xrBeginSession_p = nullptr;
		xrEndSession_p = nullptr;
		xrPollEvent_p = nullptr;
		xrWaitFrame_p = nullptr;
		xrBeginFrame_p = nullptr;
		xrLocateViews_p = nullptr;
		xrEndFrame_p = nullptr;
		xrEnumerateSwapchainFormats_p = nullptr;
		xrEnumerateViewConfigurationViews_p = nullptr;
		xrCreateSwapchain_p = nullptr;
		xrEnumerateSwapchainImages_p = nullptr;
		xrDestroySwapchain_p = nullptr;
		xrAcquireSwapchainImage_p = nullptr;
		xrWaitSwapchainImage_p = nullptr;
		xrReleaseSwapchainImage_p = nullptr;
		xrCreateActionSet_p = nullptr;
		xrDestroyActionSet_p = nullptr;
		xrStringToPath_p = nullptr;
		xrCreateAction_p = nullptr;
		xrCreateActionSpace_p = nullptr;
		xrSuggestInteractionProfileBindings_p = nullptr;
		xrSyncActions_p = nullptr;
		xrAttachSessionActionSets_p = nullptr;
		xrGetActionStateBoolean_p = nullptr;
		xrGetActionStateFloat_p = nullptr;
		xrGetActionStateVector2f_p = nullptr;
		xrGetActionStatePose_p = nullptr;
		xrLocateSpace_p = nullptr;
		xrApplyHapticFeedback_p = nullptr;

		if (s_module)
		{
			FreeLibrary(s_module);
			s_module = nullptr;
		}
	}
}

#endif // VR_ENABLED
