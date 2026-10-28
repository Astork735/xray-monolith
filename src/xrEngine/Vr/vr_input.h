#ifndef xrEngine_vr_input_h
#define xrEngine_vr_input_h
#pragma once

#include "vr_common.h"

#ifdef VR_ENABLED

// ---------------------------------------------------------------------------
// VrInput - OpenXR action based input for the two tracked hands.
//
// Grip pose = where/how the hand holds an object (used to attach a weapon).
// Aim pose  = where the weapon points (used for aiming/firing).
// These are deliberately kept separate; they are never interchangeable.
//
// hand index: 0 = left, 1 = right.
// ---------------------------------------------------------------------------
class VrInput
{
public:
	static VrInput& Get();

	// hp: the runtime enabled XR_EXT_hp_mixed_reality_controller.
	bool Initialize(XrInstance instance, XrSession session, bool hp = false);
	void Shutdown();

	// Sync actions and update poses for the given predicted display time.
	void Sync(XrSession session, XrTime time, XrSpace space);
	void Invalidate() { ClearState(); }

	bool HasRecentState() const { return m_last_sync && GetTickCount64() - m_last_sync < 250; }
	bool  IsReady() const { return m_ready; }

	bool  HandActive(int hand) const { return hand >= 0 && hand < 2 && m_active[hand]; }
	bool  GripPose(int hand, XrPosef& out) const;
	bool  AimPose(int hand, XrPosef& out) const;
	float Trigger(int hand) const { return hand >= 0 && hand < 2 ? m_trigger[hand] : 0.f; }
	float Grip(int hand) const { return hand >= 0 && hand < 2 ? m_grip[hand] : 0.f; }
	bool SecondaryButton(int hand) const { return hand >= 0 && hand < 2 && m_secondary_button[hand]; }
	bool Button(int hand) const { return hand >= 0 && hand < 2 && m_button[hand]; }
	bool Select(int hand) const { return hand >= 0 && hand < 2 && m_select[hand]; }
	bool MenuButton() const { return m_menu_button; }
	bool SprintButton() const { return m_sprint_button; }
	bool JumpButton() const { return m_jump_button; }
	void  Thumbstick(int hand, float& x, float& y) const;

	// Fire-and-forget haptic pulse on one hand (no-op if unsupported).
	void  ApplyHaptic(int hand, float amplitude, XrDuration duration_ns);

private:
	VrInput() {}
	VrInput(const VrInput&);
	VrInput& operator=(const VrInput&);

	bool CreateActions(XrInstance instance);
	void SuggestBindings(XrInstance instance, bool hp);
	void DestroyActions();
	void ClearState();

	XrActionSet m_action_set = XR_NULL_HANDLE;
	XrSession   m_session = XR_NULL_HANDLE;
	XrAction    m_grip_pose[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction    m_aim_pose[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction    m_trigger_action[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction    m_grip_action[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction    m_thumbstick_action[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction    m_select_action[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction    m_button_action[2]{};
    XrAction m_secondary_action[2]{};
	XrAction m_menu_action = XR_NULL_HANDLE;
	XrAction m_sprint_action = XR_NULL_HANDLE;
	XrAction m_jump_action = XR_NULL_HANDLE;
	// Trackpad controllers (Mixed Reality, Vive) have too few buttons: a
	// trackpad click is split by where it is pressed. Upper half: the
	// secondary button on both. Lower half: sprint/jump on Mixed Reality
	// (pad_move), the face button on Vive (pad_button).
	XrAction m_pad_vector[2]{};
	XrAction m_pad_move_click[2]{};
	XrAction m_pad_button_click[2]{};
	XrAction    m_haptic[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };

	XrSpace     m_grip_space[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrSpace     m_aim_space[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };

	bool        m_button[2]{};
    bool m_secondary_button[2]{};
	bool m_menu_button = false, m_sprint_button = false, m_jump_button = false;
	bool        m_select[2]{ false, false };
	bool        m_active[2]{ false, false };
	float       m_trigger[2]{ 0.f, 0.f };
	float       m_grip[2]{ 0.f, 0.f };
	XrVector2f  m_thumbstick[2]{ { 0.f, 0.f }, { 0.f, 0.f } };

	bool        m_have_grip_pose[2]{ false, false };
	bool        m_have_aim_pose[2]{ false, false };
	XrPosef     m_grip_pose_v[2]{};
	XrPosef     m_aim_pose_v[2]{};

	ULONGLONG m_last_sync = 0;
	bool m_ready = false;
};

#endif // VR_ENABLED
#endif // xrEngine_vr_input_h
