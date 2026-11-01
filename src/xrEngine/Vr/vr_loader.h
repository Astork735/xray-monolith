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
// Global (resolved with XR_NULL_HANDLE):
extern PFN_xrGetInstanceProcAddr                   xrGetInstanceProcAddr_p;
extern PFN_xrCreateInstance                        xrCreateInstance_p;
extern PFN_xrEnumerateInstanceExtensionProperties  xrEnumerateInstanceExtensionProperties_p;

// Instance level (resolved once an XrInstance exists):
extern PFN_xrGetInstanceProperties               xrGetInstanceProperties_p;
extern PFN_xrGetSystem                           xrGetSystem_p;
extern PFN_xrCreateSession                       xrCreateSession_p;
extern PFN_xrDestroySession                      xrDestroySession_p;
extern PFN_xrDestroyInstance                     xrDestroyInstance_p;
extern PFN_xrCreateReferenceSpace                xrCreateReferenceSpace_p;
extern PFN_xrDestroySpace                        xrDestroySpace_p;
extern PFN_xrBeginSession                        xrBeginSession_p;
extern PFN_xrEndSession                          xrEndSession_p;
extern PFN_xrPollEvent                           xrPollEvent_p;
extern PFN_xrWaitFrame                           xrWaitFrame_p;
extern PFN_xrBeginFrame                          xrBeginFrame_p;
extern PFN_xrLocateViews                         xrLocateViews_p;
extern PFN_xrEndFrame                            xrEndFrame_p;
extern PFN_xrEnumerateSwapchainFormats           xrEnumerateSwapchainFormats_p;
extern PFN_xrEnumerateViewConfigurationViews     xrEnumerateViewConfigurationViews_p;
extern PFN_xrCreateSwapchain                     xrCreateSwapchain_p;
extern PFN_xrEnumerateSwapchainImages            xrEnumerateSwapchainImages_p;
extern PFN_xrDestroySwapchain                    xrDestroySwapchain_p;
extern PFN_xrAcquireSwapchainImage               xrAcquireSwapchainImage_p;
extern PFN_xrWaitSwapchainImage                  xrWaitSwapchainImage_p;
extern PFN_xrReleaseSwapchainImage               xrReleaseSwapchainImage_p;
extern PFN_xrCreateActionSet                     xrCreateActionSet_p;
extern PFN_xrDestroyActionSet                    xrDestroyActionSet_p;
extern PFN_xrStringToPath                        xrStringToPath_p;
extern PFN_xrCreateAction                        xrCreateAction_p;
extern PFN_xrCreateActionSpace                   xrCreateActionSpace_p;
extern PFN_xrSuggestInteractionProfileBindings   xrSuggestInteractionProfileBindings_p;
extern PFN_xrSyncActions                         xrSyncActions_p;
extern PFN_xrAttachSessionActionSets             xrAttachSessionActionSets_p;
extern PFN_xrGetActionStateBoolean xrGetActionStateBoolean_p;
extern PFN_xrGetActionStateFloat                 xrGetActionStateFloat_p;
extern PFN_xrGetActionStateVector2f              xrGetActionStateVector2f_p;
extern PFN_xrGetActionStatePose                  xrGetActionStatePose_p;
extern PFN_xrLocateSpace                         xrLocateSpace_p;
extern PFN_xrApplyHapticFeedback                 xrApplyHapticFeedback_p;

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
#define xrGetActionStateBoolean xrGetActionStateBoolean_p
#define xrGetActionStateFloat                  xrGetActionStateFloat_p
#define xrGetActionStateVector2f               xrGetActionStateVector2f_p
#define xrGetActionStatePose                   xrGetActionStatePose_p
#define xrLocateSpace                          xrLocateSpace_p
#define xrApplyHapticFeedback                  xrApplyHapticFeedback_p

namespace vr
{
	// Opens openxr_loader.dll and resolves the global entry points. Returns false
	// (without failing the game) when the DLL is absent.
	bool LoadOpenXR();

	// Resolves the instance-level entry points. Call after xrCreateInstance.
	bool LoadOpenXRInstance(XrInstance instance);

	// Releases the loader DLL and clears all pointers.
	void UnloadOpenXR();

	// True once LoadOpenXR() succeeded.
	bool OpenXRLoaded();
}

#endif // VR_ENABLED
#endif // xrEngine_vr_loader_h
