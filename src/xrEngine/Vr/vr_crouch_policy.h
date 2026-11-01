#pragma once
#include <cmath>

namespace vr
{
// Physical crouch: the stance (0 stand, 1 crouch, 2 low) chosen by how far the
// tracked head is below the calibrated standing eye. Each threshold has a few
// centimetres of hysteresis so a head resting near it does not flicker the
// actor's collision box. Invalid tracking keeps the previous stance.
constexpr float crouch_hysteresis = 0.04f;

inline int PhysicalStance(bool valid, float delta, int previous, float crouch_depth, float low_depth)
{
    if (!valid || !std::isfinite(delta)) return previous;
    if (low_depth < crouch_depth + 2.f*crouch_hysteresis) low_depth = crouch_depth + 2.f*crouch_hysteresis;
    const float depth = -delta;
    const float low_enter = low_depth, low_leave = low_depth - crouch_hysteresis;
    const float crouch_enter = crouch_depth, crouch_leave = crouch_depth - crouch_hysteresis;
    if (depth >= low_enter || (previous == 2 && depth >= low_leave)) return 2;
    if (depth >= crouch_enter || (previous >= 1 && depth >= crouch_leave)) return 1;
    return 0;
}

// Stick and head both request a stance; the deeper one wins, so a seated
// player can still crouch with the stick.
inline int CombinedStance(int stick, int physical) { return stick > physical ? stick : physical; }
}
