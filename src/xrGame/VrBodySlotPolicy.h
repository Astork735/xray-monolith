#pragma once
#include <cmath>
#include <cfloat>

namespace vr
{
// Places on the player's body a hand reaches to for an item. Offsets are in
// a yaw-only frame at the tracked head (x right, y up, z forward, metres).
// Items ride the belt 10 cm above the holster height, spread around the waist
// between the pistol holster (right hip) and the ammunition pouch (left hip);
// the sideways spread follows the configured hip width. The PDA rides flat on
// the chest on the off-hand side; four chest pockets below it hold the
// quick-use items (quick slots 1-4, left to right).
enum BodySlotKind
{
    BodyKnife, BodyGrenade, BodyBolt, BodyBinoculars, BodyDetector, BodyInventory, BodyPda,
    BodyPocket1, BodyPocket2, BodyPocket3, BodyPocket4,
    BodySlotCount
};
inline bool IsPocket(int kind) { return kind >= BodyPocket1 && kind <= BodyPocket4; }
struct BodyVec { float x, y, z; };

// Head position plus a local offset in the yaw frame whose forward is the
// horizontal direction (fx, fz). X-Ray is left-handed: right = up x forward.
inline BodyVec BodyToWorld(const BodyVec& head, float fx, float fz, const BodyVec& local)
{
    float length = std::sqrt(fx*fx + fz*fz);
    if (!(length > 1e-4f)) { fx = 0.f; fz = 1.f; length = 1.f; }
    fx /= length; fz /= length;
    const float rx = fz, rz = -fx;
    return {head.x + rx*local.x + fx*local.z, head.y + local.y, head.z + rz*local.x + fz*local.z};
}

// How far the belt sits above the holster height.
constexpr float belt_raise = 0.10f;

// Laid out for a right-handed player; mirrored left to right for a
// left-handed one (holster and pouch swap hips too).
inline BodyVec BodySlotLocal(int kind, float hip_height, float hip_width = 0.22f, bool mirrored = false)
{
    const float belt = -hip_height + belt_raise, side = (mirrored ? -1.f : 1.f)*hip_width/0.22f;
    switch (kind)
    {
    case BodyBinoculars: return {0.f, belt, 0.17f};          // belt buckle
    case BodyGrenade:    return {-0.13f*side, belt, 0.13f};  // belt, front left
    case BodyBolt:       return {0.13f*side, belt, 0.13f};   // belt, front right
    case BodyDetector:   return {-0.17f*side, belt, -0.12f}; // belt, behind the pouch
    case BodyKnife:      return {0.17f*side, belt, -0.12f};  // belt, behind the holster
    case BodyInventory:  return {mirrored ? 0.18f : -0.18f, -0.10f, -0.12f}; // over the off-hand shoulder
    case BodyPda:        return {mirrored ? 0.12f : -0.12f, -0.32f*hip_height/0.72f, 0.03f}; // on the chest
    case BodyPocket1: case BodyPocket2: case BodyPocket3: case BodyPocket4:
        // A row across the lower chest, quick slot 1 on the left for either
        // hand; the chest's height follows the hip height.
        return {-0.195f + 0.13f*float(kind - BodyPocket1), -0.42f*hip_height/0.72f, 0.12f};
    default:             return {0.f, 0.f, 0.f};
    }
}

// Where a held item meets the mouth: the hand's grip centre a little in
// front of and below the eyes, in the head's yaw frame. An item held there
// for mouth_dwell_ms is eaten, drunk or applied.
constexpr float mouth_reach = 0.12f;
constexpr unsigned mouth_dwell_ms = 300;
inline BodyVec MouthPoint(const BodyVec& head, float fx, float fz)
{
    return BodyToWorld(head, fx, fz, {0.f, -0.08f, 0.08f});
}

inline float BodyDistance(const BodyVec& a, const BodyVec& b)
{
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return std::sqrt(x*x + y*y + z*z);
}

// The nearest available place within reach, or -1.
inline int NearestBodySlot(const BodyVec* points, const bool* available, int count, const BodyVec& hand,
    float radius, float& distance)
{
    int best = -1;
    distance = FLT_MAX;
    for (int i = 0; i < count; ++i)
    {
        if (!available[i]) continue;
        const float d = BodyDistance(points[i], hand);
        if (d <= radius && d < distance) { best = i; distance = d; }
    }
    return best;
}
}
