#pragma once
#include "vr_common.h"
#ifdef VR_ENABLED

// Meta body tracking (XR_FB_body_tracking), as Virtual Desktop's OpenXR
// runtime (VDXR) forwards it with "Forward tracking data to PC" on. A Quest
// 3/3S reads the upper body from its cameras (asked for with
// XR_META_body_tracking_fidelity); a Quest 2 or Pro estimates it from the
// headset and controllers. Only joint positions are read, never their
// orientations, so nothing depends on the skeleton's axes.
class CVrBodyTracker
{
public:
    // Between attempts (ns) while the runtime does not support body tracking
    // yet: Virtual Desktop may only report it once the headset streams the
    // body, and its settings can change while the game runs.
    static constexpr XrTime retry_interval = 3000000000;
    // Creates the tracker when the system supports it; otherwise tries again
    // after retry_interval (`now` is the frame's display time). Each reason
    // it is not made is logged once, with what to check.
    bool Create(XrInstance instance, XrSystemId system, XrSession session, bool fidelity, XrTime now);
    void Destroy();
    bool Created() const { return m_tracker != XR_NULL_HANDLE; }
    // Try again at once (vr_body_tracking turned on, a new session).
    void ResetAvailability() { m_next_attempt = 0; m_reason = 0; }
    // This frame's joints in `base` at `time`. Tracked when the runtime says
    // the body is active, with at least `min_confidence`.
    void Locate(XrSpace base, XrTime time, float min_confidence);
    void Invalidate() { m_tracked = false; }
    bool Tracked() const { return m_tracked; }
    float Confidence() const { return m_confidence; }
    // The fidelity the runtime reports it is tracking at this frame
    // (XR_BODY_TRACKING_FIDELITY_HIGH_META: the Quest 3's cameras), 0 when
    // it does not say.
    int Fidelity() const { return m_fidelity; }
    // A joint's pose (XrBodyJointFB), when tracked and its position is valid.
    bool Joint(int joint, XrPosef& pose) const;
private:
    XrBodyTrackerFB m_tracker = XR_NULL_HANDLE;
    XrBodyJointLocationFB m_joints[XR_BODY_JOINT_COUNT_FB]{};
    bool m_tracked = false, m_fidelity_status = false;
    float m_confidence = 0.f;
    int m_fidelity = 0, m_logged_fidelity = 0;
    XrTime m_next_attempt = 0;
    int m_reason = 0; // why the last attempt failed (logged once per reason)
    int m_log_count = 0, m_logged_state = -1;
};

#endif
