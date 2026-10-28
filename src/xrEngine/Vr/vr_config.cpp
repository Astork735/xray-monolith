#include "../stdafx.h"

#ifdef VR_ENABLED

#include "vr_config.h"
#include "vr_policy.h"

#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Console variables. Registered from xr_ioc_cmd.cpp so they round-trip through
// user.ltx exactly like every other engine cvar.
// ---------------------------------------------------------------------------
int   vr_enabled           = 1;      // default: prefer VR, auto-fall back to desktop
int   vr_tracking_origin   = 0;
float vr_world_scale       = 1.0f;
float vr_near_plane        = 0.03f;
int   vr_animated_grip     = 1;
int   vr_physical_hands    = 0;
int   vr_physical_weapon   = 0;
float vr_scope_zoom        = 0.6f;
float vr_scope_size        = 1.3f;
int   vr_debug             = 0;
int   vr_snap_turn         = 1;
float vr_snap_turn_angle   = 30.0f;
int   vr_smooth_turn       = 0;
float vr_render_scale      = 1.0f;

int vr_controller_movement = 1, vr_hands = 1, vr_hand_rays = 1;
float vr_move_deadzone = 0.2f, vr_turn_speed = 90.f, vr_max_head_offset = 1.f;
int vr_physical_crouch = 1;
float vr_crouch_depth = 0.30f, vr_low_crouch_depth = 0.60f;
int vr_body_slots = 1, vr_slot_rings = 1;
float vr_slot_radius = 0.10f;
float vr_hip_height = 0.72f, vr_hip_width = 0.22f;
float vr_vignette = 0.f;
int vr_wrist_display = 1, vr_hide_hud = 0, vr_left_handed = 0;
float vr_throw_scale = 1.75f;
float vr_recoil = 1.f;
float vr_controller_smoothing = 0.f;
int vr_shared_shadows = 1;
float vr_scope_quality = 1.f;
int vr_roomscale = 1;
float vr_torso_deadband = 50.f;

EVrLaunchMode g_vr_launch_mode = VR_Launch_Auto;

namespace vr
{
	const char* ResultToString(XrResult result)
	{
		switch (result)
		{
		case XR_SUCCESS: return "XR_SUCCESS";
		case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
		case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
		case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
		case XR_SESSION_NOT_FOCUSED: return "XR_SESSION_NOT_FOCUSED";
		case XR_FRAME_DISCARDED: return "XR_FRAME_DISCARDED";
		case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
		case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
		case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
		case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
		case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
		case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
		case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
		case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
		case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
		case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
		case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
		case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
		case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
		case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
		case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
		case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
		case XR_ERROR_PATH_INVALID: return "XR_ERROR_PATH_INVALID";
		case XR_ERROR_PATH_COUNT_EXCEEDED: return "XR_ERROR_PATH_COUNT_EXCEEDED";
		case XR_ERROR_PATH_FORMAT_INVALID: return "XR_ERROR_PATH_FORMAT_INVALID";
		case XR_ERROR_PATH_UNSUPPORTED: return "XR_ERROR_PATH_UNSUPPORTED";
		case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
		case XR_ERROR_ACTION_TYPE_MISMATCH: return "XR_ERROR_ACTION_TYPE_MISMATCH";
		case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
		case XR_ERROR_SESSION_NOT_STOPPING: return "XR_ERROR_SESSION_NOT_STOPPING";
		case XR_ERROR_TIME_INVALID: return "XR_ERROR_TIME_INVALID";
		case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
		case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
		case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
		case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
		case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
		case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
		case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
		default: return "XR_<unrecognised>";
		}
	}

	bool Succeeded(XrResult result, const char* what)
	{
		if (XR_SUCCEEDED(result))
			return true;

		Msg("! [VR] %s failed: %s (%d)", what, ResultToString(result), int(result));
		return false;
	}
}

// ---------------------------------------------------------------------------
// Launch mode resolution.
//   "-novr" anywhere on the command line -> desktop.
//   otherwise "-vr"                      -> force VR.
//   otherwise                            -> the saved vr_enabled preference.
// ---------------------------------------------------------------------------
void vr_resolve_launch_mode(const char* lpCmdLine)
{
	const bool no_vr = vr::LaunchOverride(lpCmdLine) == 0;
	const bool want_vr = vr::LaunchOverride(lpCmdLine) == 2;

	if (no_vr)
	{
		g_vr_launch_mode = VR_Launch_Off;
		Msg("* [VR] launch mode: desktop (-novr)");
	}
	else if (want_vr)
	{
		g_vr_launch_mode = VR_Launch_Force;
		Msg("* [VR] launch mode: VR requested (-vr)");
	}
	else
	{
		g_vr_launch_mode = VR_Launch_Auto;
		Msg("* [VR] launch mode: auto (using saved preference)");
	}
}

#endif // VR_ENABLED
