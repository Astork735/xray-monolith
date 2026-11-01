#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif

#ifdef VR_ENABLED

#include "vr_input.h"
#include "vr_config.h"

namespace
{
	const char* kHandPath[2] = { "/user/hand/left", "/user/hand/right" };
}

VrInput& VrInput::Get()
{
	static VrInput s_instance;
	return s_instance;
}

static XrAction MakeAction(XrActionSet set, XrActionType type, const char* name,
                           const XrPath* subactions, uint32_t subaction_count)
{
	XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
	ci.actionType = type;
	ci.countSubactionPaths = subaction_count;
	ci.subactionPaths = subactions;
	xr_strcpy(ci.actionName, name);
	xr_strcpy(ci.localizedActionName, name);

	XrAction action = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateAction(set, &ci, &action)))
	{
		Msg("! [VR] failed to create action '%s'", name);
		return XR_NULL_HANDLE;
	}
	return action;
}

bool VrInput::CreateActions(XrInstance instance)
{
	XrActionSetCreateInfo set_ci{ XR_TYPE_ACTION_SET_CREATE_INFO };
	xr_strcpy(set_ci.actionSetName, "gameplay");
	xr_strcpy(set_ci.localizedActionSetName, "Gameplay");
	set_ci.priority = 0;
	if (XR_FAILED(xrCreateActionSet(instance, &set_ci, &m_action_set)))
	{
		Msg("! [VR] xrCreateActionSet failed");
		return false;
	}

	// Resolve the two hand subaction paths once.
	XrPath hand[2]{ XR_NULL_PATH, XR_NULL_PATH };
	for (int i = 0; i < 2; ++i)
		if (XR_FAILED(xrStringToPath(instance, kHandPath[i], &hand[i])))
			{ Msg("! [VR] xrStringToPath failed for %s", kHandPath[i]); return false; }

	for (int i = 0; i < 2; ++i)
	{
		m_grip_pose[i] = MakeAction(m_action_set, XR_ACTION_TYPE_POSE_INPUT, i == 0 ? "grip_left" : "grip_right", &hand[i], 1);
		m_aim_pose[i] = MakeAction(m_action_set, XR_ACTION_TYPE_POSE_INPUT, i == 0 ? "aim_left" : "aim_right", &hand[i], 1);
		m_trigger_action[i] = MakeAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT, i == 0 ? "trigger_left" : "trigger_right", &hand[i], 1);
		m_grip_action[i] = MakeAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT, i == 0 ? "grip_value_left" : "grip_value_right", &hand[i], 1);
		m_thumbstick_action[i] = MakeAction(m_action_set, XR_ACTION_TYPE_VECTOR2F_INPUT, i == 0 ? "thumbstick_left" : "thumbstick_right", &hand[i], 1);
		m_select_action[i] = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, i == 0 ? "select_left" : "select_right", &hand[i], 1);
		m_button_action[i] = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, i == 0 ? "button_left" : "button_right", &hand[i], 1);
		m_haptic[i] = MakeAction(m_action_set, XR_ACTION_TYPE_VIBRATION_OUTPUT, i == 0 ? "haptic_left" : "haptic_right", &hand[i], 1);
	}
	m_menu_action = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_left", &hand[0], 1);
	m_sprint_action = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "sprint_left", &hand[0], 1);
	m_jump_action = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "jump_right", &hand[1], 1);
	if (!m_menu_action || !m_sprint_action || !m_jump_action) return false;

    for (int i = 0; i < 2; ++i)
    {
        m_secondary_action[i] = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, i == 0 ? "secondary_left" : "secondary_right", &hand[i], 1);
        m_pad_vector[i] = MakeAction(m_action_set, XR_ACTION_TYPE_VECTOR2F_INPUT, i == 0 ? "pad_left" : "pad_right", &hand[i], 1);
        m_pad_move_click[i] = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, i == 0 ? "pad_move_left" : "pad_move_right", &hand[i], 1);
        m_pad_button_click[i] = MakeAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, i == 0 ? "pad_button_left" : "pad_button_right", &hand[i], 1);
        if (!m_pad_vector[i] || !m_pad_move_click[i] || !m_pad_button_click[i]) return false;
        if (!m_secondary_action[i] || !m_grip_pose[i] || !m_aim_pose[i] || !m_trigger_action[i] || !m_grip_action[i] || !m_thumbstick_action[i] || !m_haptic[i] || !m_select_action[i] || !m_button_action[i])
            return false;
    }
    // Bind pose actions to their own spaces.
	for (int i = 0; i < 2; ++i)
	{
		XrActionSpaceCreateInfo gp{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		gp.action = m_grip_pose[i];
		gp.subactionPath = hand[i];
		gp.poseInActionSpace.orientation.w = 1.f;
		VR_XR_CHECK(xrCreateActionSpace(m_session, &gp, &m_grip_space[i]), "create grip space");

		XrActionSpaceCreateInfo ap{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		ap.action = m_aim_pose[i];
		ap.subactionPath = hand[i];
		ap.poseInActionSpace.orientation.w = 1.f;
		VR_XR_CHECK(xrCreateActionSpace(m_session, &ap, &m_aim_space[i]), "create aim space");
	}
    return true;
}

namespace
{
	struct Binding { XrAction action; const char* path; };

	void Suggest(XrInstance instance, const char* profile, const Binding* binds, size_t count)
	{
		std::vector<XrActionSuggestedBinding> suggested;
		suggested.reserve(count);
		for (size_t i = 0; i < count; ++i)
		{
			if (binds[i].action == XR_NULL_HANDLE)
				continue;
			XrPath path = XR_NULL_PATH;
			if (XR_FAILED(xrStringToPath(instance, binds[i].path, &path)))
				continue;
			suggested.push_back({ binds[i].action, path });
		}
		if (suggested.empty())
			return;

		XrInteractionProfileSuggestedBinding s{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		XrPath profile_path = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(instance, profile, &profile_path)))
			return;
		s.interactionProfile = profile_path;
		s.countSuggestedBindings = (uint32_t)suggested.size();
		s.suggestedBindings = suggested.data();

		// Failure here only means the runtime does not know this profile; not fatal.
		if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &s)))
			Msg("* [VR] profile not supported by runtime: %s", profile);
	}
}

void VrInput::SuggestBindings(XrInstance instance, bool hp)
{
    // Touch, Index, Mixed Reality, simple, Vive wand, and the HP Reverb G2
    // (only with its extension enabled; it is laid out like Touch).
    enum { Touch, Index, MixedReality, Simple, Vive, Hp };
    const char* profiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/microsoft/mixed_reality_controller",
        "/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/htc/vive_controller",
        "/interaction_profiles/hp/mixed_reality_controller"
    };
    const int profile_count = hp ? 6 : 5;
    for (int profile = 0; profile < profile_count; ++profile)
    {
        const bool touch_like = profile == Touch || profile == Hp;
        const bool trackpad = profile == MixedReality || profile == Vive;
        char paths[2][12][96];
        std::vector<Binding> bindings;
        for (int hand = 0; hand < 2; ++hand)
        {
            const char* side = hand == 0 ? "left" : "right";
            int used = 0;
            const auto path = [&](const char* input) -> const char*
            {
                char* out = paths[hand][used++];
                sprintf_s(out, 96, "/user/hand/%s/%s", side, input);
                return out;
            };
            // Khronos simple_controller exposes a Boolean select click, not
            // a float trigger axis. Suggesting the float action on that path
            // invalidates the profile's binding set in conforming runtimes.
            const char* trigger = path(profile == Simple ? "input/select/click" : "input/trigger/value");
            if (profile != Simple) bindings.push_back({m_trigger_action[hand], trigger});
            bindings.push_back({m_select_action[hand], trigger});
            if (profile != Simple)
            {
                if (touch_like || profile == Index)
                {
                    bindings.push_back({m_button_action[hand],
                        path(touch_like && hand == 0 ? "input/x/click" : "input/a/click")});
                    bindings.push_back({m_secondary_action[hand],
                        path(touch_like && hand == 0 ? "input/y/click" : "input/b/click")});
                }
                else if (profile == MixedReality)
                    bindings.push_back({m_button_action[hand], path("input/thumbstick/click")});
                bindings.push_back({m_grip_action[hand], path(trackpad ? "input/squeeze/click" : "input/squeeze/value")});
                bindings.push_back({m_thumbstick_action[hand], path(profile == Vive ? "input/trackpad" : "input/thumbstick")});
                if (trackpad)
                {
                    bindings.push_back({m_pad_vector[hand], path("input/trackpad")});
                    bindings.push_back({profile == Vive ? m_pad_button_click[hand] : m_pad_move_click[hand],
                        path("input/trackpad/click")});
                }
            }
            bindings.push_back({m_grip_pose[hand], path("input/grip/pose")});
            bindings.push_back({m_aim_pose[hand], path("input/aim/pose")});
            bindings.push_back({m_haptic[hand], path("output/haptic")});
        }
        if (touch_like)
        {
            bindings.push_back({m_menu_action, "/user/hand/left/input/menu/click"});
            bindings.push_back({m_sprint_action, "/user/hand/left/input/thumbstick/click"});
            bindings.push_back({m_jump_action, "/user/hand/right/input/thumbstick/click"});
        }
        else if (profile == Index)
        {
            // Index: the system button is the runtime's, so a firm press on
            // the left trackpad opens the menu. The stick clicks are free.
            bindings.push_back({m_menu_action, "/user/hand/left/input/trackpad/force"});
            bindings.push_back({m_sprint_action, "/user/hand/left/input/thumbstick/click"});
            bindings.push_back({m_jump_action, "/user/hand/right/input/thumbstick/click"});
        }
        else if (profile == MixedReality)
        {
            // The stick clicks are the face buttons here; sprint and jump are
            // the lower half of the trackpads (pad_move).
            bindings.push_back({m_menu_action, "/user/hand/left/input/menu/click"});
        }
        else if (profile == Vive)
        {
            // The trackpad is the stick; the right menu button jumps.
            bindings.push_back({m_menu_action, "/user/hand/left/input/menu/click"});
            bindings.push_back({m_jump_action, "/user/hand/right/input/menu/click"});
        }
        // Suggestions replace a profile's complete list, so both hands must
        // be submitted together rather than overwriting the left bindings.
        Suggest(instance, profiles[profile], bindings.data(), bindings.size());
    }
}

bool VrInput::Initialize(XrInstance instance, XrSession session, bool hp)
{
	m_session = session; // stored for action-space creation
	if (!CreateActions(instance)) { Shutdown(); return false; }
	SuggestBindings(instance, hp);

	XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &m_action_set;
	if (XR_FAILED(xrAttachSessionActionSets(session, &attach)))
	{
		Msg("! [VR] xrAttachSessionActionSets failed");
		Shutdown();
		return false;
	}

	m_ready = (m_action_set != XR_NULL_HANDLE);
	if (m_ready)
		Msg("* [VR] input actions ready");
	return m_ready;
}

void VrInput::Sync(XrSession session, XrTime time, XrSpace space)
{
	ClearState();
	if (!m_ready || m_action_set == XR_NULL_HANDLE)
		return;

	XrActiveActionSet active{ m_action_set, XR_NULL_PATH };
	XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (xrSyncActions(session, &sync) != XR_SUCCESS)
		return;

    m_last_sync = GetTickCount64();
	XrActionStateGetInfo left_info{XR_TYPE_ACTION_STATE_GET_INFO};
	XrActionStateBoolean left_state{XR_TYPE_ACTION_STATE_BOOLEAN};
	left_info.action=m_menu_action;
	if (XR_SUCCEEDED(xrGetActionStateBoolean(session,&left_info,&left_state)) && left_state.isActive)
		m_menu_button=left_state.currentState==XR_TRUE;
	left_state={XR_TYPE_ACTION_STATE_BOOLEAN};
	left_info.action=m_sprint_action;
	if (XR_SUCCEEDED(xrGetActionStateBoolean(session,&left_info,&left_state)) && left_state.isActive)
		m_sprint_button=left_state.currentState==XR_TRUE;
	left_state={XR_TYPE_ACTION_STATE_BOOLEAN};
	left_info.action=m_jump_action;
	if (XR_SUCCEEDED(xrGetActionStateBoolean(session,&left_info,&left_state)) && left_state.isActive)
		m_jump_button=left_state.currentState==XR_TRUE;
	for (int i = 0; i < 2; ++i)
	{
		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };

        gi.action = m_secondary_action[i];
        XrActionStateBoolean secondary{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session,&gi,&secondary)) && secondary.isActive)
            m_secondary_button[i]=secondary.currentState==XR_TRUE;
        gi.action = m_button_action[i];
        XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &button)) && button.isActive)
            m_button[i] = button.currentState == XR_TRUE;
        gi.action = m_select_action[i];
        XrActionStateBoolean select{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &select)) && select.isActive)
            m_select[i] = select.currentState == XR_TRUE;
		float f = 0.f;
		gi.action = m_trigger_action[i];
		XrActionStateFloat trigger{ XR_TYPE_ACTION_STATE_FLOAT };
		if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &trigger)) && trigger.isActive)
		{
			f = trigger.currentState;
			m_active[i] = true;
		}
		m_trigger[i] = f;

		gi.action = m_grip_action[i];
		XrActionStateFloat grip{ XR_TYPE_ACTION_STATE_FLOAT };
		if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &grip)) && grip.isActive)
			m_grip[i] = grip.currentState;

		gi.action = m_thumbstick_action[i];
		XrActionStateVector2f stick{ XR_TYPE_ACTION_STATE_VECTOR2F };
		if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &stick)) && stick.isActive)
			m_thumbstick[i] = stick.currentState;

		// Poses.
		auto locate = [&](XrAction action, XrSpace action_space, bool& have, XrPosef& out)
		{
			if (action_space == XR_NULL_HANDLE)
			{
				have = false;
				return;
			}
			XrActionStatePose pose_state{ XR_TYPE_ACTION_STATE_POSE };
			gi.action = action;
			if (XR_FAILED(xrGetActionStatePose(session, &gi, &pose_state)) || !pose_state.isActive)
			{
				have = false;
				return;
			}

			XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(xrLocateSpace(action_space, space, time, &loc)) &&
				(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
				(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				out = loc.pose;
				have = true;
			}
			else
			{
				have = false;
			}
		};

		// A trackpad click, split by where it is pressed (see m_pad_vector).
		gi.action = m_pad_vector[i];
		XrActionStateVector2f pad{XR_TYPE_ACTION_STATE_VECTOR2F};
		if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &pad)) && pad.isActive)
		{
			const bool upper = pad.currentState.y >= 0.f;
			XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
			gi.action = m_pad_move_click[i];
			if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &click)) && click.isActive && click.currentState)
			{
				if (upper) m_secondary_button[i] = true;
				else (i == 0 ? m_sprint_button : m_jump_button) = true;
			}
			click = {XR_TYPE_ACTION_STATE_BOOLEAN};
			gi.action = m_pad_button_click[i];
			if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &click)) && click.isActive && click.currentState)
			{
				if (upper) m_secondary_button[i] = true;
				else m_button[i] = true;
			}
		}

		locate(m_grip_pose[i], m_grip_space[i], m_have_grip_pose[i], m_grip_pose_v[i]);
		locate(m_aim_pose[i], m_aim_space[i], m_have_aim_pose[i], m_aim_pose_v[i]);
        m_active[i] = m_have_grip_pose[i] || m_have_aim_pose[i];
        if (vr_debug && Device.dwFrame % 120 == 0)
            Msg("[VR] hand=%d active=%d grip_valid=%d aim_valid=%d trigger=%.2f squeeze=%.2f button=%d secondary=%d grip=(%.3f %.3f %.3f) aim=(%.3f %.3f %.3f)",
                i, m_active[i], m_have_grip_pose[i], m_have_aim_pose[i], m_trigger[i], m_grip[i], m_button[i], m_secondary_button[i],
                m_grip_pose_v[i].position.x, m_grip_pose_v[i].position.y, m_grip_pose_v[i].position.z,
                m_aim_pose_v[i].position.x, m_aim_pose_v[i].position.y, m_aim_pose_v[i].position.z);
	}
}

bool VrInput::GripPose(int hand, XrPosef& out) const
{
	if (hand < 0 || hand > 1 || !m_have_grip_pose[hand])
		return false;
	out = m_grip_pose_v[hand];
	return true;
}

bool VrInput::AimPose(int hand, XrPosef& out) const
{
	if (hand < 0 || hand > 1 || !m_have_aim_pose[hand])
		return false;
	out = m_aim_pose_v[hand];
	return true;
}

void VrInput::Thumbstick(int hand, float& x, float& y) const
{
	if (hand < 0 || hand > 1) { x = y = 0.f; return; }
	x = m_thumbstick[hand].x;
	y = m_thumbstick[hand].y;
}

void VrInput::ApplyHaptic(int hand, float amplitude, XrDuration duration_ns)
{
	if (hand < 0 || hand > 1 || m_haptic[hand] == XR_NULL_HANDLE || !m_ready)
		return;

	XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
	vibration.amplitude = amplitude;
	vibration.duration = duration_ns;
	clamp(vibration.amplitude, 0.f, 1.f);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

	XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = m_haptic[hand];
	xrApplyHapticFeedback(m_session, &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
}

void VrInput::DestroyActions()
{
	m_menu_action=m_sprint_action=m_jump_action=XR_NULL_HANDLE;
	for (int i = 0; i < 2; ++i)
	{
		if (m_grip_space[i] != XR_NULL_HANDLE) { xrDestroySpace(m_grip_space[i]); m_grip_space[i] = XR_NULL_HANDLE; }
		if (m_aim_space[i] != XR_NULL_HANDLE) { xrDestroySpace(m_aim_space[i]); m_aim_space[i] = XR_NULL_HANDLE; }
        m_secondary_action[i]=m_pad_vector[i]=m_pad_move_click[i]=m_pad_button_click[i]=XR_NULL_HANDLE;
        m_grip_pose[i] = m_aim_pose[i] = m_trigger_action[i] = m_grip_action[i] = m_thumbstick_action[i] = m_haptic[i] = m_select_action[i] = m_button_action[i] = XR_NULL_HANDLE;
	}
	if (m_action_set != XR_NULL_HANDLE)
	{
		xrDestroyActionSet(m_action_set);
		m_action_set = XR_NULL_HANDLE;
	}
}

void VrInput::ClearState()
{
    m_last_sync = 0;
	m_menu_button=m_sprint_button=m_jump_button=false;
    for (int i = 0; i < 2; ++i)
    {
        m_active[i] = m_have_grip_pose[i] = m_have_aim_pose[i] = false;
        m_trigger[i] = m_grip[i] = 0;
        m_secondary_button[i]=false;
        m_select[i] = m_button[i] = false;
        m_thumbstick[i] = {0, 0};
        m_grip_pose_v[i] = m_aim_pose_v[i] = {};
    }
}

void VrInput::Shutdown()
{
	DestroyActions();
	m_ready = false;
	m_session = XR_NULL_HANDLE;
	ClearState();
}

#endif // VR_ENABLED
