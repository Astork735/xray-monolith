#pragma once
#include <cmath>
#include <algorithm>

namespace vr
{
inline void FilterStick(float& x, float& y, float deadzone)
{
    if (!std::isfinite(x) || !std::isfinite(y)) { x = y = 0; return; }
    const float length = std::sqrt(x*x + y*y);
    if (length <= deadzone) { x = y = 0; return; }
    const float amount = (std::min(length, 1.f) - deadzone) / (1.f - deadzone);
    x *= amount / length; y *= amount / length;
}
struct TurnState
{
    bool armed = false;
    void Reset() { armed = false; }
    float Step(float axis, float dt, bool snap, bool smooth, float angle, float speed)
    {
        if (!std::isfinite(axis)) { Reset(); return 0; }
        if (std::abs(axis) < 0.3f) armed = true;
        if (smooth)
        {
            float y = 0; FilterStick(axis, y, 0.25f);
            return axis * speed * std::max(0.f, std::min(dt, 0.1f));
        }
        if (snap && armed && std::abs(axis) >= 0.7f)
        {
            armed = false;
            return axis > 0 ? angle : -angle;
        }
        return 0;
    }
};

// A view's extent as angles (radians) from its forward axis: left and down
// negative, right and up positive, as OpenXR gives each eye's field of view.
struct FovAngles
{
    float left = 0, right = 0, up = 0, down = 0;
    bool empty = true;
    // Widens the extent to take in a direction given in the same frame
    // (x right, y up, z forward). Directions behind the frame are ignored.
    void Include(float x, float y, float z)
    {
        if (!(z > 1e-4f) || !std::isfinite(x) || !std::isfinite(y)) return;
        const float h = std::atan2(x, z), v = std::atan2(y, z);
        if (empty) { left = right = h; up = down = v; empty = false; return; }
        left = std::min(left, h); right = std::max(right, h);
        down = std::min(down, v); up = std::max(up, v);
    }
};

// The square size, in pixels, the magnified scope's view is rendered at: the
// lens' footprint on the eye image (the tangent of its apparent half angle
// over that of the eye's vertical half field of view, as the eye's
// projection maps them) times `quality`, rounded up to a size bucket so a
// small head movement does not recreate render targets. `current` (0 when
// none) is kept while the need fits it and is not 15% below the bucket under
// it. Quality 0, or a need beyond the largest bucket, renders at eye size
// (the largest bucket bounds the smaller targets' memory).
inline unsigned ScopeResolution(float apparent_deg, float eye_vfov_deg, unsigned eye_px, float quality,
    unsigned current)
{
    if (!(quality > 0.f) || !(apparent_deg > 0.f) || !(eye_vfov_deg > 0.f) || !(eye_vfov_deg < 180.f) || !eye_px)
        return eye_px;
    const float half = 3.14159265f / 360.f;
    const float need = float(eye_px) * std::tan(std::min(apparent_deg, 179.f) * half) /
        std::tan(eye_vfov_deg * half) * quality;
    static const unsigned buckets[] = {256, 384, 512, 768};
    const int count = int(sizeof(buckets) / sizeof(buckets[0]));
    for (int i = 0; i < count; ++i)
        if (buckets[i] == current && current < eye_px && need <= float(current) &&
            (i == 0 || need > 0.85f * float(buckets[i - 1])))
            return current;
    for (int i = 0; i < count; ++i)
        if (need <= float(buckets[i])) return std::min(buckets[i], eye_px);
    return eye_px;
}

// The head's heading (radians about +Y, 0 along +Z) from its forward and up
// axes in any yaw-only frame. Looking down, the forward axis points at the
// floor and the up axis forward, so the up axis takes over as the head
// pitches; looking up, the up axis points back and is subtracted. False only
// for a degenerate frame.
inline bool HeadHeading(float fx, float fy, float fz, float ux, float uz, float& yaw)
{
    const float hx = fx - ux * fy, hz = fz - uz * fy;
    if (!(hx * hx + hz * hz > 1e-8f) || !std::isfinite(hx) || !std::isfinite(hz)) return false;
    yaw = std::atan2(hx, hz);
    return true;
}

// The torso's yaw follows the head's: it stays put while the head turns less
// than `deadband` (radians) from it, and is dragged along to stay within it
// beyond that. A glance to the side leaves belt and holsters where they are;
// turning round in the room brings them round. Result in (-pi, pi].
inline float TorsoYaw(float torso, float head, float deadband)
{
    const float pi = 3.14159265f;
    float diff = std::remainder(head - torso, 2.f * pi);
    if (diff > deadband) torso += diff - deadband;
    else if (diff < -deadband) torso += diff + deadband;
    torso = std::remainder(torso, 2.f * pi);
    return torso <= -pi ? torso + 2.f * pi : torso;
}

// Roomscale: how hard the body walks (0..1) toward a head that has walked
// `distance` metres away from it. Nothing inside `deadzone` (leaning and
// small steps stay leans); full walking speed 30 cm beyond it.
inline float RoomscaleRequest(float distance, float deadzone)
{
    if (!(distance > deadzone) || !std::isfinite(distance)) return 0.f;
    return std::min(1.f, 0.2f + (distance - deadzone) / 0.3f);
}

// How much of the body's move this frame followed the head: its part along
// the way to the head, never backwards and never past it. The tracking
// origin moves back by this so the head itself stays where it is.
inline float RoomscaleFollowed(float moved_along, float distance)
{
    if (!std::isfinite(moved_along) || !(distance > 0.f)) return 0.f;
    return std::max(0.f, std::min(moved_along, distance));
}

// Darkness (0..1) over the eyes when the head is held `blocked` metres short
// of where it is (inside a wall): none for a graze, black by 12 cm. Seeing
// the camera stop while the head moves on is worse than a fade.
inline float HeadBlockedFade(float blocked)
{
    if (!std::isfinite(blocked)) return 1.f;
    return std::max(0.f, std::min(1.f, (blocked - 0.02f) / 0.10f));
}

// The eye near plane (engine units): `minimum` while a tracked hand is
// within 45 cm (plus the plane) of the head, pushed out to 10 cm as the
// nearest hand moves away. `closest_hand` in metres (huge: none tracked).
inline float AdaptiveNearPlane(float closest_hand, float minimum, float world_scale)
{
    const float farthest = std::max(minimum, 0.10f * world_scale);
    if (!std::isfinite(closest_hand)) return farthest;
    return std::max(minimum, std::min(farthest, (closest_hand - 0.45f) * world_scale));
}

// Sweep a head volume at bounded intervals, then refine the first collision.
// The callback tests the entire head volume, not just its centre ray.
template<class Blocked> float HeadSweepFraction(float distance, Blocked blocked)
{
    if (blocked(0.f)) return 0.f;
    const int steps = std::max(1, int(std::ceil(distance / 0.025f)));
    float safe = 0;
    for (int step = 1; step <= steps; ++step)
    {
        float next = float(step) / steps;
        if (blocked(next))
        {
            for (int refine = 0; refine < 8; ++refine)
            {
                const float middle = (safe + next) * 0.5f;
                if (blocked(middle)) next = middle; else safe = middle;
            }
            return safe;
        }
        safe = next;
    }
    return 1.f;
}
}
