#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif
#ifdef VR_ENABLED
#include "vr_body_tracking.h"

bool CVrBodyTracker::Create(XrInstance instance, XrSystemId system, XrSession session, bool fidelity, XrTime now)
{
    if (m_tracker != XR_NULL_HANDLE) return true;
    if (!xrCreateBodyTrackerFB || !xrLocateBodyJointsFB || !xrGetSystemProperties) return false;
    if (m_next_attempt && now < m_next_attempt) return false;
    m_next_attempt = now + retry_interval;
    const auto failed = [&](int reason, const char* message, XrResult result)
    {
        // Once per reason, not every retry.
        if (m_reason == reason) return false;
        m_reason = reason;
        Msg("* [VR] body tracking: %s (%s); trying again every %d s", message, vr::ResultToString(result),
            int(retry_interval / 1000000000));
        return false;
    };
    XrSystemBodyTrackingPropertiesFB body{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB};
    XrSystemPropertiesBodyTrackingFidelityMETA high{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    properties.next = &body;
    if (fidelity) body.next = &high;
    XrResult result = xrGetSystemProperties(instance, system, &properties);
    if (XR_FAILED(result)) return failed(1, "the runtime's system properties were not read", result);
    // Virtual Desktop reports support only while the headset sends the body.
    if (!body.supportsBodyTracking)
        return failed(2, "the runtime reports no body tracking. In Virtual Desktop turn on Forward tracking data "
            "to PC; on the Quest turn on Movement tracking -> Body tracking and allow Virtual Desktop to use it",
            result);
    XrBodyTrackerCreateInfoFB info{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB};
    info.bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB;
    result = xrCreateBodyTrackerFB(session, &info, &m_tracker);
    if (XR_FAILED(result))
    {
        m_tracker = XR_NULL_HANDLE;
        return failed(3, "the tracker was not created", result);
    }
    m_reason = 0;
    // Quest 3/3S: the cameras' upper body. A Quest 2 or Pro does not support
    // it and estimates the body from the headset and controllers.
    const bool supports_high = fidelity && high.supportsBodyTrackingFidelity && xrRequestBodyTrackingFidelityMETA;
    const bool requested = supports_high &&
        XR_SUCCEEDED(xrRequestBodyTrackingFidelityMETA(m_tracker, XR_BODY_TRACKING_FIDELITY_HIGH_META));
    m_fidelity_status = fidelity;
    Msg("* [VR] body tracking: tracker created; camera (high) fidelity %s", requested ? "requested" :
        fidelity ? "not supported by this headset (estimated from headset and controllers)" : "not offered");
    m_logged_state = -1;
    m_logged_fidelity = 0;
    return true;
}
void CVrBodyTracker::Destroy()
{
    if (m_tracker != XR_NULL_HANDLE && xrDestroyBodyTrackerFB) xrDestroyBodyTrackerFB(m_tracker);
    m_tracker = XR_NULL_HANDLE;
    m_tracked = false;
    m_confidence = 0.f;
    m_fidelity = 0;
}
void CVrBodyTracker::Locate(XrSpace base, XrTime time, float min_confidence)
{
    m_tracked = false;
    if (m_tracker == XR_NULL_HANDLE || base == XR_NULL_HANDLE) return;
    XrBodyJointsLocateInfoFB info{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB};
    info.baseSpace = base;
    info.time = time;
    XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB};
    // The fidelity it is actually tracking at (camera or estimated).
    XrBodyTrackingFidelityStatusMETA status{XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META};
    if (m_fidelity_status) locations.next = &status;
    locations.jointCount = XR_BODY_JOINT_COUNT_FB;
    locations.jointLocations = m_joints;
    if (XR_FAILED(xrLocateBodyJointsFB(m_tracker, &info, &locations))) return;
    m_confidence = locations.confidence;
    m_tracked = locations.isActive && locations.confidence >= min_confidence;
    m_fidelity = m_fidelity_status && locations.isActive ? int(status.fidelity) : 0;
    // Changes are logged a few times a session, not on every flicker.
    if (m_log_count >= 12) return;
    if (m_fidelity && m_fidelity != m_logged_fidelity)
    {
        ++m_log_count;
        m_logged_fidelity = m_fidelity;
        Msg("* [VR] body tracking: fidelity %s", m_fidelity == XR_BODY_TRACKING_FIDELITY_HIGH_META ?
            "high (the cameras see the upper body)" : "low (estimated from headset and controllers)");
    }
    const int state = m_tracked ? 1 : 0;
    if (state == m_logged_state) return;
    ++m_log_count;
    m_logged_state = state;
    if (m_tracked) Msg("* [VR] body tracking: tracking (confidence %.2f)", locations.confidence);
    else
        // The usual cause is a setting on the headset or in Virtual Desktop.
        Msg("* [VR] body tracking: no body (active=%d confidence=%.2f); on the Quest, Movement tracking -> "
            "Body tracking, and in Virtual Desktop, Forward tracking data to PC", int(locations.isActive),
            locations.confidence);
}
bool CVrBodyTracker::Joint(int joint, XrPosef& pose) const
{
    if (!m_tracked || joint < 0 || joint >= XR_BODY_JOINT_COUNT_FB) return false;
    const auto& location = m_joints[joint];
    if (!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
    pose = location.pose;
    return true;
}
#endif
