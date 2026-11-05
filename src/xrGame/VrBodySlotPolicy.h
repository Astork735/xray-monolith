#pragma once
#include <cmath>
#include <cfloat>

namespace vr
{
// Places on the player's body a hand reaches to for an item. Offsets are in
// a yaw-only frame at the tracked head (x right, y up, z forward, metres).
// Items ride the belt 16 cm above the holster height, on the body rather than
// out in front of it, spread around the waist between the pistol holster
// (right hip) and the ammunition pouch (left hip); the sideways spread follows
// the configured hip width. The PDA rides flat on the chest on the off-hand
// side and the binoculars hang from their strap on the other. Four pockets
// hold the quick-use items (quick slots 1-4, left to right), around the front
// of the stomach above the belt. Every place is at least 14 cm from the next
// one, the holster and the pouch, so a hand is clearly at one of them.
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
constexpr float belt_raise = 0.16f;

// Laid out for a right-handed player; mirrored left to right for a
// left-handed one (holster and pouch swap hips too).
inline BodyVec BodySlotLocal(int kind, float hip_height, float hip_width = 0.22f, bool mirrored = false)
{
    // The chest places do not widen with the hips.
    const float hand = mirrored ? -1.f : 1.f, belt = -hip_height + belt_raise, side = hand*hip_width/0.22f;
    const float chest = -0.28f*hip_height/0.72f;
    switch (kind)
    {
    // Around the waist, hugging it: 12 cm to each side at the front (with
    // the hip width), 2 cm in front of the eyes; 21 cm at the sides, 16 cm
    // behind the eyes there (the knife and the device, well back on the
    // hips), so a hand reaching out in front of the stomach is at none of them.
    case BodyGrenade:    return {-0.12f*side, belt, 0.02f};       // front left
    case BodyBolt:       return {0.12f*side, belt, 0.02f};        // front right
    case BodyDetector:   return {-0.21f*side, belt, -0.16f};      // above and behind the pouch
    case BodyKnife:      return {0.21f*side, belt, -0.16f};       // above and behind the holster
    case BodyBinoculars: return {0.12f*hand, chest, -0.01f};      // on the chest, weapon-hand side
    case BodyInventory:  return {-0.18f*hand, -0.10f, -0.17f};    // over the off-hand shoulder
    case BodyPda:        return {-0.12f*hand, chest, -0.01f};     // on the chest, off-hand side
    case BodyPocket1: case BodyPocket2: case BodyPocket3: case BodyPocket4:
    {
        // On an arc around the stomach 13 cm above the belt, quick slot 1
        // on the left for either hand: two at the sides, two to the front.
        static const BodyVec arc[4] = {{-0.20f, 0.f, -0.05f}, {-0.07f, 0.f, 0.02f},
                                       {0.07f, 0.f, 0.02f}, {0.20f, 0.f, -0.05f}};
        const BodyVec& place = arc[kind - BodyPocket1];
        return {place.x, belt + 0.13f, place.z};
    }
    default:             return {0.f, 0.f, 0.f};
    }
}

// With the full body shown the places sit on it, where looking down past
// the belly sees them: each a direction round the pelvis (radians, 0
// forward, + toward the weapon hand) and a height above it, all within 75
// degrees of forward. The slot code puts each body_slot_margin outside
// the measured body. The belt carries the grenade and bolt at the front,
// the detector and knife at the sides; the PDA and binoculars ride the
// lower chest, the pockets the stomach between them, the ammunition pouch
// the off-hand hip. False for a place that keeps its head-relative spot
// (the inventory, over the shoulder).
struct BodyPlace { float angle, height; };
constexpr float body_slot_margin = 0.07f;
constexpr int BodyPouch = BodySlotCount;  // the pouch, for BodySlotOnBody only
inline bool BodySlotOnBody(int kind, bool mirrored, BodyPlace& place)
{
    constexpr float deg = 3.14159265f/180.f;
    const float hand = mirrored ? -1.f : 1.f;
    switch (kind)
    {
    case BodyGrenade:    place = {-30.f*deg*hand, 0.16f}; return true;
    case BodyBolt:       place = {30.f*deg*hand, 0.16f}; return true;
    case BodyDetector:   place = {-70.f*deg*hand, 0.16f}; return true;
    case BodyKnife:      place = {70.f*deg*hand, 0.16f}; return true;
    case BodyPda:        place = {-40.f*deg*hand, 0.40f}; return true;
    case BodyBinoculars: place = {40.f*deg*hand, 0.40f}; return true;
    // Quick slot 1 on the left for either hand.
    case BodyPocket1:    place = {-60.f*deg, 0.29f}; return true;
    case BodyPocket2:    place = {-20.f*deg, 0.29f}; return true;
    case BodyPocket3:    place = {20.f*deg, 0.29f}; return true;
    case BodyPocket4:    place = {60.f*deg, 0.29f}; return true;
    case BodyPouch:      place = {-60.f*deg*hand, 0.f}; return true;
    default:             return false;
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

// How far from a place a hand reaches it: the pockets sit close together
// above the belt, so they take a smaller reach than the belt.
inline float BodySlotReach(int kind, float radius) { return IsPocket(kind) ? radius*0.65f : radius; }

// The nearest available place within reach, or -1 (points indexed by kind).
inline int NearestBodySlot(const BodyVec* points, const bool* available, int count, const BodyVec& hand,
    float radius, float& distance)
{
    int best = -1;
    distance = FLT_MAX;
    for (int i = 0; i < count; ++i)
    {
        if (!available[i]) continue;
        const float d = BodyDistance(points[i], hand);
        if (d <= BodySlotReach(i, radius) && d < distance) { best = i; distance = d; }
    }
    return best;
}
}
