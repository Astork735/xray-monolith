#pragma once
#include <cmath>
#include <cfloat>
#include "VrVec.h"

// Engine-free decisions for picking up, wielding and letting go of weapons
// (VrWeaponInteraction.cpp), so they can be unit tested (tests/vr).
namespace vr
{

// A receiving hand at the handle can wield the weapon; one on the fore-end
// carries it. Use the existing 12 cm grip reach, but at most 4 cm forward so
// a nearby magazine well or compact fore-end is not treated as the handle.
inline bool HandAtWeaponGrip(const V3& hand, const V3& grip, const V3& forward)
{
    const V3 offset=hand-grip;
    const float length=Length(forward);
    return length>1e-4f && Dot(offset,offset)<=0.12f*0.12f &&
        Dot(offset,forward)<=0.04f*length;
}

// --- Letting go of the weapon in the hand -----------------------------------

enum class Release { None, Holster, Drop };
// Per weapon while it is in the hand; reset when it leaves the hand.
struct HolsterState
{
    bool armed = false;   // letting go at the holster now puts it back
    bool gripped = false; // the grip has held it (a key-drawn weapon has not)
};
constexpr float holster_clear_margin = 0.04f; // clear of the holster: past its reach by this
constexpr unsigned holster_arm_ms = 250;      // or held this long

// Where an open weapon hand sends the weapon this frame. distance: from the
// weapon hand to its holster (FLT_MAX without one); open: the hand is open;
// opened: it opened this frame; held_ms: since the weapon came into the hand.
// - Just drawn, it stays until the hand has been clear of the holster or has
//   held it holster_arm_ms: relaxing the grip on the way out keeps it.
// - Then an open hand at the holster puts it back, whether it opened there or
//   arrived there open, once a grip has held it.
// - Opened anywhere else, it leaves the hand.
inline Release ReleaseDecision(HolsterState& state, float distance, float range, unsigned held_ms,
    bool open, bool opened)
{
    if (distance > range + holster_clear_margin || held_ms > holster_arm_ms) state.armed = true;
    if (!open) state.gripped = true;
    const bool at = state.armed && distance <= range;
    if (open && at && (opened || state.gripped)) return Release::Holster;
    if (opened && distance > range) return Release::Drop;
    return Release::None;
}

// A same-state firing restart can be queued after an earlier shot finished.
// Finish native firing, including its animation, before hand/ownership changes.
inline bool NativeFirePending(bool firing, bool next_firing)
{ return firing || next_firing; }

// Reload, launcher switching and native firing finish before any ownership
// change. Other animations can end through the weapon's own hide/drop.
inline bool ReleaseNow(bool at_rest, bool native_busy, bool fire_pending=false)
{ return !fire_pending && (at_rest || !native_busy); }

// --- Wielding a weapon taken by hand ----------------------------------------

// OtherComing: no item is out yet, but another weapon is on its way to a
// hand (its slot's activation is queued, or a wield is pending for it). It
// counts as held: one weapon is on its way at a time, so the other hand
// cannot draw or wield a second one over it.
enum class ActiveItem { None, ThisWeapon, OtherHeld, OtherLeaving, OtherComing };
enum class WieldStep
{
    Wait,       // not in the inventory yet
    Stow,       // into its holster on that hand's side, not drawn
    Activate,   // into its slot and drawn in that hand
    Adopt,      // the game's pickup drew it already: that hand takes it, done
    HoldHand,   // the previous item is still being put away: show it in the hand, wait
    Cancel,     // the other hand holds a weapon: no wield
};
// What the wield of a weapon taken by hand does this frame. Adopt matters:
// left waiting while the game had already drawn it, the wield drew it again
// the moment it was holstered.
inline WieldStep WieldDecision(bool stow, bool in_inventory, ActiveItem active)
{
    if (stow) return in_inventory ? WieldStep::Stow : WieldStep::Wait;
    if (active == ActiveItem::OtherHeld || active == ActiveItem::OtherComing) return WieldStep::Cancel;
    if (!in_inventory) return WieldStep::Wait;
    if (active == ActiveItem::ThisWeapon) return WieldStep::Adopt;
    if (active == ActiveItem::OtherLeaving) return WieldStep::HoldHand;
    return WieldStep::Activate;
}

// Whether a slot weapon taken by a hand goes straight into that hand (to be
// wielded, WieldDecision) rather than being carried. With nothing in the
// hands it does. While the weapon just let go is still being put away, only
// when the new weapon's slot is free: drawing it otherwise makes room in that
// slot by pushing the weapon being holstered there into the backpack.
inline bool TakeIntoHand(ActiveItem active, bool slot_free)
{
    if (active == ActiveItem::None) return true;
    if (active == ActiveItem::OtherLeaving) return slot_free;
    return false;
}

// A hand's claim on a weapon (it took the weapon, which is drawn into that
// hand) holds only while the weapon is on its way there: its own slot's
// activation is queued (in_queued_slot: in weapon slot 2 or 3, that slot
// queued and not yet active), or a wield (not a stow) is pending for it.
// Once it is active the claim is consumed by the weapon's own update. A
// weapon in the backpack, whose slot id the inventory leaves stale, or one
// stowed in its holster, has none: a stale claim drew it in the hand that
// once took it while it hung on the body.
inline bool ClaimHolds(bool active, bool in_queued_slot, bool wield_pending)
{
    return active || in_queued_slot || wield_pending;
}

// --- Choosing what a hand takes ---------------------------------------------

constexpr float grab_touch = 0.12f;        // on an item: within this of its box
constexpr float pointer_range = 2.f;
constexpr float pointer_min = 0.05f;
constexpr float pointer_tolerance = 0.07f; // aside per metre along (about 4 degrees)

// An item centre within the pointer's cone: along the ray within range, and
// aside of it by no more than its radius plus the tolerance. angle (aside per
// along) ranks candidates, smallest first. dir must be unit length.
inline bool PointerCone(const V3& origin, const V3& dir, const V3& centre, float radius,
    float& along, float& angle)
{
    const V3 to = (centre - origin);
    along = Dot(to, dir);
    if (along < pointer_min || along > pointer_range) return false;
    const V3 on_ray = {origin.x + dir.x*along, origin.y + dir.y*along, origin.z + dir.z*along};
    const float aside = Length((centre - on_ray));
    if (aside > radius + along*pointer_tolerance) return false;
    angle = aside/along;
    return true;
}

// Where a ray enters a box (slab test), within max_range; start and dir in
// the box's frame. A ray starting inside enters at 0.
inline bool RayBoxEnter(const V3& start, const V3& dir, const V3& lo, const V3& hi,
    float max_range, float& enter)
{
    const float s[3] = {start.x, start.y, start.z}, d[3] = {dir.x, dir.y, dir.z};
    const float l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    enter = 0.f;
    float leave = max_range;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::fabs(d[axis]) < 1e-6f)
        {
            if (s[axis] < l[axis] || s[axis] > h[axis]) return false;
            continue;
        }
        float t0 = (l[axis] - s[axis])/d[axis], t1 = (h[axis] - s[axis])/d[axis];
        if (t0 > t1) { const float t = t0; t0 = t1; t1 = t; }
        if (t0 > enter) enter = t0;
        if (t1 < leave) leave = t1;
        if (enter > leave) return false;
    }
    return true;
}

// A free hand on the held weapon (its fore-end, magazine, slide): within
// on_bore of the bore line from the weapon hand forward bore_length. A
// squeeze there is for the weapon, not a grab; beside the weapon hand is not.
constexpr float on_bore = 0.16f;
constexpr float bore_length = 0.75f;
inline bool AtWeapon(const V3& hand, const V3& weapon_hand, const V3& bore_dir)
{
    const V3 to = (hand - weapon_hand);
    float along = Dot(to, bore_dir);
    along = along < 0.f ? 0.f : along > bore_length ? bore_length : along;
    const V3 on = {weapon_hand.x + bore_dir.x*along, weapon_hand.y + bore_dir.y*along,
        weapon_hand.z + bore_dir.z*along};
    return Length((hand - on)) < on_bore;
}
}
