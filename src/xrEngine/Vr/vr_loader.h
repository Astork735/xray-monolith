#ifndef xrEngine_vr_loader_h
#define xrEngine_vr_loader_h
#pragma once

// ---------------------------------------------------------------------------
// Dynamic loading of the OpenXR loader.
//
// The game must start and run in flat-screen mode with no OpenXR runtime and no
// openxr_loader.dll present, so we never link against openxr_loader.lib and
// never rely on the linker's import table. Instead the loader DLL is opened on
// demand (only when VR is actually requested) and every entry point is resolved
// through xrGetInstanceProcAddr.
//
// Each xr* function used by the VR code is remapped (via the macros below) to a
// function-pointer with an _p suffix, so the call sites read normally.
//
// This header is included by vr_common.h *after* the OpenXR headers.
// ---------------------------------------------------------------------------

#ifdef VR_ENABLED

// --- entry points we resolve ------------------------------------------------
// One list for the declarations, definitions, resolution and clearing
// (vr_loader.cpp). Every use pastes or quotes the name (## or #), so the
// aliases below never expand inside it.
// Global (resolved with XR_NULL_HANDLE):
#define VR_XR_GLOBAL_FUNCTIONS(X) \
	X(xrCreateInstance) \
	X(xrEnumerateInstanceExtensionProperties)

// Instance level (resolved once an XrInstance exists):
#define VR_XR_INSTANCE_FUNCTIONS(X) \
	X(xrGetInstanceProperties) \
	X(xrGetSystem) \
	X(xrCreateSession) \
	X(xrDestroySession) \
	X(xrDestroyInstance) \
	X(xrCreateReferenceSpace) \
	X(xrDestroySpace) \
	X(xrBeginSession) \
	X(xrEndSession) \
	X(xrPollEvent) \
	X(xrWaitFrame) \
	X(xrBeginFrame) \
	X(xrLocateViews) \
	X(xrEndFrame) \
	X(xrEnumerateSwapchainFormats) \
	X(xrEnumerateViewConfigurationViews) \
	X(xrCreateSwapchain) \
	X(xrEnumerateSwapchainImages) \
	X(xrDestroySwapchain) \
	X(xrAcquireSwapchainImage) \
	X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) \
	X(xrCreateActionSet) \
	X(xrDestroyActionSet) \
	X(xrStringToPath) \
	X(xrCreateAction) \
	X(xrCreateActionSpace) \
	X(xrSuggestInteractionProfileBindings) \
	X(xrSyncActions) \
	X(xrAttachSessionActionSets) \
	X(xrGetActionStateBoolean) \
	X(xrGetActionStateFloat) \
	X(xrGetActionStateVector2f) \
	X(xrGetActionStatePose) \
	X(xrLocateSpace) \
	X(xrApplyHapticFeedback)

// Optional, resolved apart (vr::LoadOpenXRBodyTracking): a runtime without
// body tracking (XR_FB_body_tracking) or its fidelity request
// (XR_META_body_tracking_fidelity) still starts VR.
#define VR_XR_BODY_FUNCTIONS(X) 	X(xrGetSystemProperties) 	X(xrCreateBodyTrackerFB) 	X(xrDestroyBodyTrackerFB) 	X(xrLocateBodyJointsFB)
#define VR_XR_FIDELITY_FUNCTIONS(X) 	X(xrRequestBodyTrackingFidelityMETA)
// Optional (vr::LoadOpenXRVisibilityMask): the area of each eye the lenses
// never show (XR_KHR_visibility_mask).
#define VR_XR_VISIBILITY_MASK_FUNCTIONS(X) 	X(xrGetVisibilityMaskKHR)

#define VR_XR_DECLARE(NAME) extern PFN_##NAME NAME##_p;
extern PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_p;
VR_XR_GLOBAL_FUNCTIONS(VR_XR_DECLARE)
VR_XR_INSTANCE_FUNCTIONS(VR_XR_DECLARE)
VR_XR_BODY_FUNCTIONS(VR_XR_DECLARE)
VR_XR_FIDELITY_FUNCTIONS(VR_XR_DECLARE)
VR_XR_VISIBILITY_MASK_FUNCTIONS(VR_XR_DECLARE)
#undef VR_XR_DECLARE

// Map the API names onto the resolved pointers.
#define xrGetInstanceProcAddr                  xrGetInstanceProcAddr_p
#define xrCreateInstance                       xrCreateInstance_p
#define xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties_p
#define xrGetInstanceProperties                xrGetInstanceProperties_p
#define xrGetSystem                            xrGetSystem_p
#define xrCreateSession                        xrCreateSession_p
#define xrDestroySession                       xrDestroySession_p
#define xrDestroyInstance                      xrDestroyInstance_p
#define xrCreateReferenceSpace                 xrCreateReferenceSpace_p
#define xrDestroySpace                         xrDestroySpace_p
#define xrBeginSession                         xrBeginSession_p
#define xrEndSession                           xrEndSession_p
#define xrPollEvent                            xrPollEvent_p
#define xrWaitFrame                            xrWaitFrame_p
#define xrBeginFrame                           xrBeginFrame_p
#define xrLocateViews                          xrLocateViews_p
#define xrEndFrame                             xrEndFrame_p
#define xrEnumerateSwapchainFormats            xrEnumerateSwapchainFormats_p
#define xrEnumerateViewConfigurationViews      xrEnumerateViewConfigurationViews_p
#define xrCreateSwapchain                      xrCreateSwapchain_p
#define xrEnumerateSwapchainImages             xrEnumerateSwapchainImages_p
#define xrDestroySwapchain                     xrDestroySwapchain_p
#define xrAcquireSwapchainImage                xrAcquireSwapchainImage_p
#define xrWaitSwapchainImage                   xrWaitSwapchainImage_p
#define xrReleaseSwapchainImage                xrReleaseSwapchainImage_p
#define xrCreateActionSet                      xrCreateActionSet_p
#define xrDestroyActionSet                     xrDestroyActionSet_p
#define xrStringToPath                         xrStringToPath_p
#define xrCreateAction                         xrCreateAction_p
#define xrCreateActionSpace                    xrCreateActionSpace_p
#define xrSuggestInteractionProfileBindings    xrSuggestInteractionProfileBindings_p
#define xrSyncActions                          xrSyncActions_p
#define xrAttachSessionActionSets              xrAttachSessionActionSets_p
#define xrGetActionStateBoolean                xrGetActionStateBoolean_p
#define xrGetActionStateFloat                  xrGetActionStateFloat_p
#define xrGetActionStateVector2f               xrGetActionStateVector2f_p
#define xrGetActionStatePose                   xrGetActionStatePose_p
#define xrLocateSpace                          xrLocateSpace_p
#define xrApplyHapticFeedback                  xrApplyHapticFeedback_p
#define xrGetSystemProperties                  xrGetSystemProperties_p
#define xrCreateBodyTrackerFB                  xrCreateBodyTrackerFB_p
#define xrDestroyBodyTrackerFB                 xrDestroyBodyTrackerFB_p
#define xrLocateBodyJointsFB                   xrLocateBodyJointsFB_p
#define xrRequestBodyTrackingFidelityMETA      xrRequestBodyTrackingFidelityMETA_p
#define xrGetVisibilityMaskKHR                 xrGetVisibilityMaskKHR_p

namespace vr
{
	// Opens openxr_loader.dll and resolves the global entry points. Returns false
	// (without failing the game) when the DLL is absent.
	bool LoadOpenXR();

	// Resolves the instance-level entry points. Call after xrCreateInstance.
	bool LoadOpenXRInstance(XrInstance instance);

	// Resolves the optional body tracking entry points (only once the
	// extensions were enabled). Never fails VR: false leaves body tracking
	// off. The fidelity request is resolved only when `fidelity` is set and
	// may stay null.
	bool LoadOpenXRBodyTracking(XrInstance instance, bool fidelity);

	// Resolves xrGetVisibilityMaskKHR (only once XR_KHR_visibility_mask was
	// enabled). Never fails VR: false leaves the mask unknown.
	bool LoadOpenXRVisibilityMask(XrInstance instance);

	// Releases the loader DLL and clears all pointers.
	void UnloadOpenXR();

	// True once LoadOpenXR() succeeded.
	bool OpenXRLoaded();
}

#endif // VR_ENABLED
#endif // xrEngine_vr_loader_h
