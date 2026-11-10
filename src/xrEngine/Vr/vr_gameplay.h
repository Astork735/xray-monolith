#pragma once

// Engine/game bridge: no OpenXR or renderer dependency in desktop game builds.
namespace vr
{
ENGINE_API bool GameplayActive();
// The head's forward in the body's yaw-only frame, NOT world space: for
// steering in the actor's own movement basis. World code uses HeadTransform.
ENGINE_API bool HeadDirectionInBody(Fvector& direction);
// The head in world space: i right, j up, k forward (where the player
// looks), c the head's position (as HeadPosition).
ENGINE_API bool HeadTransform(Fmatrix& world);
ENGINE_API bool HeadPosition(Fvector& position);
// The player's torso heading in world space (horizontal, unit): follows the
// head's turning beyond vr_torso_deadband degrees, so belt, holsters and
// pockets stay at the real body when the player turns round in the room.
ENGINE_API bool TorsoForward(Fvector& forward);
// Meta body tracking (vr_body_tracking: XR_FB_body_tracking, which Virtual
// Desktop's own runtime forwards). The controllers still place the hands:
// the tracked body bends the elbows, moves the shoulders and turns the
// torso (TorsoForward), and measures the player.
enum BodyJoint { BodyJointChest, BodyJointShoulder, BodyJointElbow, BodyJointWrist };
// A tracked joint's world position (side 0 left, 1 right; the chest takes
// either), smoothed; kept from the last tracked frame while
// BodyTrackingWeight eases out. False when there is none.
ENGINE_API bool BodyJointPosition(BodyJoint joint, int side, Fvector& world);
// How far the tracked body is used this frame: 0..1, eased over 0.2 s.
ENGINE_API float BodyTrackingWeight();
// Reads this frame's tracked body (the engine calls it every frame; the
// readers above call it too).
ENGINE_API void UpdateBodyTracking();
// The player's shoulder width (vr_shoulder_width, else measured) over the
// average's: 0.85..1.20, 1 when neither is known. Spreads the places on
// the shoulders and chest.
ENGINE_API float ShoulderWidthScale();
// The player's arm, shoulder joint to wrist (vr_arm_length, else measured),
// in engine units; 0 when neither is known.
ENGINE_API float ArmLength();
// The game's floor under the actor (world y), as the game places the body:
// seated reach (vr_seated_reach) takes hands reaching for the real floor
// down to it.
ENGINE_API void SetGameFloor(float world_y);
// Places the tracked body at the actor's camera for this frame. The actor
// calls it after updating its camera, before anything uses hand poses.
ENGINE_API void UpdateGameplayBody();
// Roomscale (vr_roomscale): the actor's body is about to be placed
// world_delta further on while the player's head has not moved; the head
// stays where it is in the world. Call before UpdateGameplayBody.
ENGINE_API void ShiftTrackingOrigin(const Fvector& world_delta);
ENGINE_API bool Roomscale();
// Tracked head height relative to the calibrated standing eye, in metres
// (negative when ducking). False without valid gameplay tracking.
ENGINE_API bool HeadHeightDelta(float& metres);
ENGINE_API void HideHandForFrame(int hand);
ENGINE_API bool HandHiddenForFrame(int hand);
ENGINE_API unsigned GameplayInputEpoch();
// Changes each time the tracked body is placed in the world for a frame;
// world-space hand and head poses are stable between changes.
ENGINE_API unsigned BodyEpoch();
ENGINE_API bool MovementInput(float dt, float& side, float& forward, float& turn_radians);
ENGINE_API void SuspendGameplayInput();
// Right stick up/down edge: +1 to stand up a step, -1 to go down a step.
ENGINE_API int StanceStep();
ENGINE_API bool HandTransform(int hand, bool aim, Fmatrix& world);
// The same hand in the body's frame, and the body's placement in the world:
// HandTransform = BodyTransform * HandInBody. Hand motion measured in the
// body's frame is the player's real hand motion; the body's turning (snap
// or smooth) and walking are not in it.
ENGINE_API bool HandInBody(int hand, bool aim, Fmatrix& local);
ENGINE_API bool BodyTransform(Fmatrix& world);
// The actor that owned the tracked body is being destroyed (level change,
// load, death and respawn): its successor is calibrated again.
ENGINE_API void ForgetGameplayBody(const void* owner);
ENGINE_API float HandSqueeze(int hand);
ENGINE_API float HandTrigger(int hand);
ENGINE_API bool HandSecondaryButton(int hand);
ENGINE_API bool HandButton(int hand); // lower face button / stick click
ENGINE_API bool SprintButton(); // left stick click
ENGINE_API bool JumpButton(); // right stick click
ENGINE_API void HandHaptic(int hand, float amplitude);
// Quick save, as the quick save key, from the next PollVrEvents.
ENGINE_API void RequestQuickSave();
ENGINE_API bool HandsVisible();
// Weapons held as their first-person animation holds them (vr_animated_grip).
ENGINE_API bool AnimatedGrip();
// Single shots and hand-worked actions fire as fast as the trigger and the
// action are worked, not at the weapon's rpm (vr_mechanical_fire_rate).
ENGINE_API bool MechanicalFireRate();
// Physical hands: 0 off, 1 push loose objects, 2 also stop on the world
// (vr_physical_hands); the held weapon pushes loose objects (vr_physical_weapon).
ENGINE_API int PhysicalHands();
// Geometry-aware contact diagnostics (vr_contact_debug): 1 logs magazine and
// finger solve timings, 2 also draws the magazine's path and contact.
ENGINE_API int ContactDebug();
// The player's torso and legs shown in VR (vr_body).
ENGINE_API bool BodyVisible();
// Magnified scopes: the game asks each update for the view from the scope's
// objective (camera frame, vertical field of view in degrees); the stereo
// renderer draws it every other frame, and DrawScopeLens shows the latest
// one on the eyepiece in the eye being rendered (centre, half-extent axes,
// 0..1 brightness; the eye's offset from the exit pupil across the lens,
// in lens units, shades that side).
// lens: the lens' apparent angle (degrees) from the eye, which sets the
// size the view is rendered at (vr_scope_quality); 0 renders it at eye size.
// reticle: the scope's own reticle texture (its flat-screen scope texture),
// drawn over the view on the lens; none draws a plain duplex crosshair.
// reticle_rect: the part of that texture it is (texels x1, y1, x2, y2, as
// the UI texture list gives it); null for the whole texture.
ENGINE_API void SetScopeView(const Fmatrix& camera, float fov, float lens = 0.f, LPCSTR reticle = nullptr,
    const float* reticle_rect = nullptr);
ENGINE_API bool DrawScopeLens(const Fvector& centre, const Fvector& right, const Fvector& up,
    float brightness, float offset_x, float offset_y);
// A red-dot sight's dot on a quad (centre, half-extent axes); depth: hidden
// behind nearer scene depth (the sight's frame), else drawn over the scene.
ENGINE_API void DrawReflexDot(const Fvector& centre, const Fvector& right, const Fvector& up, float alpha,
    bool depth);
// vr_scope_zoom: magnified scopes show the game's zoom times this.
ENGINE_API float ScopeZoomScale();
// vr_scope_size: the scope's lens image is drawn this much larger than the
// eyepiece glass.
ENGINE_API float ScopeLensScale();
ENGINE_API bool PhysicalWeapon();
// Thumbstick up/down while a UI window is open (+ up): the turn stick, and
// with move_stick_too the movement stick as well (the stronger push wins;
// for a window that stops walking, so the hand that points can scroll).
ENGINE_API float UiScrollAxis(bool move_stick_too=false);
ENGINE_API bool HandRaysVisible();
ENGINE_API float MaxHeadOffset();
// Physical crouch (vr_physical_crouch): ducking depths in metres.
ENGINE_API bool PhysicalCrouch();
ENGINE_API float CrouchDepth();
ENGINE_API float LowCrouchDepth();
// Body slots (vr_body_slots): reach radius, marker rings, hip placement (m).
ENGINE_API bool BodySlots();
ENGINE_API bool BodySlotRings();
ENGINE_API float BodySlotRadius();
// vr_hip_height's default: the shown body's belt is moved by the setting's
// difference from it (VrBodyHipShift).
constexpr float default_hip_height = 0.65f;
ENGINE_API float HipHeight();
ENGINE_API float HipWidth();
// vr_left_handed: the left hand is the weapon/pointer hand; sticks and body
// places are mirrored.
ENGINE_API bool LeftHanded();
// The hand that carries the weapon by default (1 right, 0 left).
ENGINE_API int DominantHand();
// Status display on the other wrist (vr_wrist_display); flat HUD hidden
// (vr_hide_hud).
ENGINE_API bool WristDisplay();
// PDA message text on the HUD, times the usual size (vr_message_text_size).
ENGINE_API float MessageTextSize();
ENGINE_API bool HideHud();
// How much larger the in-game HUD panel looks than at its default size
// (vr_hud_scale over vr_hud_distance): HUD elements are drawn this much
// closer to the centre so they grow inward (vr::HudInsetOffset).
ENGINE_API float HudMagnification();
// A world point on the head-locked in-game HUD panel: where the line from
// the eyes to it crosses the panel, as u, v across it (0..1, v down;
// unclamped, mirrored through the centre behind the head, as a flat-screen
// projection is), and its distance ahead of the eyes (negative behind).
// A marker drawn there on the HUD lies over the point (game.world2ui).
// `inside`: ahead of the eyes and on the panel. False without gameplay
// tracking or while a menu's panel is anchored in the room.
ENGINE_API bool WorldToHud(const Fvector& world, float& u, float& v, float& depth, bool& inside);
// A thrown grenade or bolt leaves at the hand's speed times this
// (vr_throw_scale): an arm without a weight to swing throws short.
ENGINE_API float ThrowScale();
// How hard a shot kicks the weapon in the hands (vr_recoil): 0 none, 1
// the default, up to 3.
ENGINE_API float Recoil();
// Item names show within this distance across the floor (vr_label_range);
// 0 shows none.
ENGINE_API float LabelRange();
// A looked-at character's name and faction show within this distance
// (vr_npc_label_range); 0 never.
ENGINE_API float NpcLabelRange();
}
