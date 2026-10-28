#ifndef xrEngine_vr_config_h
#define xrEngine_vr_config_h
#pragma once

#include "vr_common.h"

#ifdef VR_ENABLED

// How VR was requested for this run. Command line wins over the saved
// preference in user.ltx (see vr_resolve_launch_mode).
enum EVrLaunchMode
{
	VR_Launch_Auto = 0,  // no explicit request - use the saved preference
	VR_Launch_Force,     // "-vr"    - request VR, desktop fallback on failure
	VR_Launch_Off,       // "-novr"  - force desktop, never touch OpenXR
};

// Console variables (registered from xr_ioc_cmd.cpp, persisted to user.ltx).
extern int   vr_enabled;            // 0 = desktop, 1 = automatic, 2 = request VR
extern int   vr_tracking_origin;    // 0 = local/seated, 1 = stage/standing
extern float vr_world_scale;        // metres -> engine units
extern float vr_near_plane;         // eye near clip (m); hands and sights come close
extern int   vr_debug;              // log poses / action state each frame
extern int   vr_snap_turn;          // 1 = snap turning on
extern float vr_snap_turn_angle;    // degrees per snap
extern int   vr_smooth_turn;        // 1 = continuous (smooth) turning
extern float vr_render_scale;       // per-eye render target scale

extern int vr_controller_movement, vr_hands, vr_hand_rays;
extern int vr_animated_grip;        // 1 = grips from first-person animations
extern int vr_physical_hands;       // vr_hand_physics: 0 off, 1 loose objects, 2 objects and world
extern int vr_physical_weapon;      // vr_weapon_physics: 1 = the held weapon pushes loose objects
extern float vr_scope_zoom;         // magnified scopes: the game's zoom times this
extern float vr_scope_size;         // scope lens image size over the eyepiece glass
extern float vr_move_deadzone, vr_turn_speed, vr_max_head_offset;
extern int vr_physical_crouch;      // 1 = ducking the real head crouches the actor
extern float vr_crouch_depth, vr_low_crouch_depth; // metres below the calibrated eye
extern int vr_body_slots, vr_slot_rings; // grab items from the body / show slot markers
extern float vr_slot_radius;        // body-slot reach (m)
extern float vr_hip_height, vr_hip_width; // head-to-hip drop and hip half-width (m)
extern float vr_vignette;           // comfort vignette strength while moving/turning (0 off)
extern int vr_wrist_display;        // status display on the off-hand wrist
extern int vr_hide_hud;             // hide the flat HUD indicators in VR
extern int vr_left_handed;          // mirror sticks and body places for a left-handed player
extern float vr_throw_scale;        // thrown grenade/bolt speed per unit of hand speed
extern float vr_recoil;             // weapon kick in the hands per shot (0 none, 1 default)
extern float vr_controller_smoothing; // hand pose smoothing, 0 off .. 10 (a 100 ms time constant)
extern int vr_shared_shadows;       // 1 = the eyes share one set of sun shadow cascades
extern float vr_scope_quality;      // scope view pixels per lens pixel on the eye (0 = eye size)
extern int vr_roomscale;            // 1 = the actor walks after a head that walked away from it
extern float vr_torso_deadband;     // degrees the head turns before belt and holsters turn with it

// Result of parsing "-vr" / "-novr" plus the saved preference.
extern EVrLaunchMode g_vr_launch_mode;

// Parse the command line (raw, case-insensitive) and the saved preference into
// g_vr_launch_mode. Precedence: "-novr" beats "-vr" beats the saved value.
void vr_resolve_launch_mode(const char* lpCmdLine);

#endif // VR_ENABLED
#endif // xrEngine_vr_config_h
