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
ENGINE_API float HandSqueeze(int hand);
ENGINE_API float HandTrigger(int hand);
ENGINE_API bool HandSecondaryButton(int hand);
ENGINE_API bool HandButton(int hand); // lower face button / stick click
ENGINE_API bool SprintButton(); // left stick click
ENGINE_API bool JumpButton(); // right stick click
ENGINE_API void HandHaptic(int hand, float amplitude);
ENGINE_API bool HandsVisible();
// Weapons held as their first-person animation holds them (vr_animated_grip).
ENGINE_API bool AnimatedGrip();
// Physical hands: 0 off, 1 push loose objects, 2 also stop on the world
// (vr_physical_hands); the held weapon pushes loose objects (vr_physical_weapon).
ENGINE_API int PhysicalHands();
// Magnified scopes: the game asks each update for the view from the scope's
// objective (camera frame, vertical field of view in degrees); the stereo
// renderer draws it every other frame, and DrawScopeLens shows the latest
// one on the eyepiece in the eye being rendered (centre, half-extent axes,
// 0..1 brightness; the eye's offset from the exit pupil across the lens,
// in lens units, shades that side).
// lens: the lens' apparent angle (degrees) from the eye, which sets the
// size the view is rendered at (vr_scope_quality); 0 renders it at eye size.
ENGINE_API void SetScopeView(const Fmatrix& camera, float fov, float lens = 0.f);
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
// Right thumbstick up/down while a UI window is open (+ up).
ENGINE_API float UiScrollAxis();
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
ENGINE_API bool HideHud();
// A thrown grenade or bolt leaves at the hand's speed times this
// (vr_throw_scale): an arm without a weight to swing throws short.
ENGINE_API float ThrowScale();
// How hard a shot kicks the weapon in the hands (vr_recoil): 0 none, 1
// the default, up to 3.
ENGINE_API float Recoil();
// UI-layer coordinates (0..1) of a world point, for world-anchored labels.
ENGINE_API bool WorldToUi(const Fvector& world, float& u, float& v);
}
