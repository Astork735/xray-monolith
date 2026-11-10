#pragma once
// Shared by the VR weapon and hands translation units split from
// VrWeaponInteraction.cpp (VrPickup, VrHandBodies, VrFloorMagazines,
// VrBodyAnchors, VrWeaponUpdate): their small helpers (namespace vrwi) and the
// per-frame steps UpdateVrWeapons runs. Not for use outside them.
#include "VrWeaponInteraction.h"
#include "VrPickupPolicy.h"
#include "VrWeaponModel.h"

class CPhysicsShellHolder;
class CPhysicsShell;
class IRenderVisual;
namespace collide { struct rq_result; }

namespace vrwi
{
// The contact surface of a weapon's world model (held without a measured
// HUD model), named `name`: built on a worker thread once per model for the
// session, null until it is ready. Built on the game thread, a first grab of
// each GAMMA weapon froze the game for a moment.
std::shared_ptr<const VrWeaponContact> WorldModelContact(IRenderVisual* visual, const shared_str& name);
inline Fvector Point(const Fmatrix& transform, const Fvector& local)
{
    Fvector result=local;
    transform.transform_tiny(result);
    return result;
}
// Point's inverse: a world point in the transform's own space.
inline Fvector Local(const Fmatrix& transform, const Fvector& world)
{
    Fvector result;
    Fmatrix().invert(transform).transform_tiny(result,world);
    return result;
}
inline bool Near(const Fvector& a, const Fvector& b, float radius)
{
    return a.distance_to_sqr(b)<=radius*radius;
}

// --- Magazines left in the world (registry in VrWeaponInteraction.cpp) ------
// A magazine left in the world (dropped, released, let go away from the
// weapon). It belongs to no weapon, so it outlives the one it came from, and
// to no save: it holds its rounds until a hand puts them back (into a
// weapon that takes it, or the backpack) and is gone with them when it
// expires or the level is unloaded.
struct VrFloorMagazine
{
    CPhysicsShell* shell=nullptr;
    // Posed once, as it left the weapon (the bones read `poses`).
    IRenderVisual* visual=nullptr;
    xr_vector<Fmatrix> poses;
    xr_vector<CCartridge> cartridges;
    shared_str kind; // CVrWeaponInteraction::MagazineKind
    Fvector box_center{}, box_half{};
    u32 expires=0;
    // The hand carrying it (-1 none) and its pose in that hand's grip frame.
    int hand=-1;
    Fmatrix in_hand=Fidentity;
    // How the weapon it left holds a magazine from the pouch, in that
    // (physical) hand's grip frame: that hand taking it holds it so.
    int hold_hand=-1;
    Fmatrix hold=Fidentity;
};
constexpr u32 floor_magazine_life=5*60*1000;
constexpr size_t floor_magazine_limit=16;
extern xr_list<VrFloorMagazine> g_floor_magazines;
// Its rounds are gone with it; its body and model are released.
void RetireFloorMagazine(VrFloorMagazine& magazine);
// The one the hand carries, or null.
VrFloorMagazine* FloorMagazineIn(int hand);
// Rounds back into the actor's boxes, part-filled first, the rest as new boxes.
void ReturnRoundsToActor(CActor& actor, const xr_vector<CCartridge>& rounds);
}

// Every weapon interaction in existence: claims that are no longer on their
// way lapse (CVrWeaponInteraction::UpdateHandClaim).
void VrUpdateHandClaims(CActor& actor);

// --- VrPickup.cpp -----------------------------------------------------------
// The active item is being put away (its hide runs, or another is queued).
bool VrActiveItemLeaving(CActor& actor);
// No item is out yet, but a weapon other than `except` is on its way to a
// hand: its slot's draw is queued, or a wield (not a stow) is pending.
bool VrWeaponComing(CActor& actor, const CObject* except=nullptr);
// What the hands have out, seen by a weapon taken by hand (`self`).
vr::ActiveItem VrActiveItemState(CActor& actor, const CObject* self);
inline vr::V3 Pick(const Fvector& v) { return {v.x,v.y,v.z}; }
// The eyes see the hand: within 2 m, nothing of the static world between.
bool VrHandReachable(CActor& actor, const Fvector& hand);
// An item a free hand can take: loose in the world, active physics, not the
// other hand's.
bool VrTakeable(CActor& actor, int side, CPhysicsShellHolder* object);
// How far a point (in the box's frame) is outside the box; 0 inside.
float BoxDistance(const Fvector& local, const Fbox& box);
// What a free hand takes on a squeeze, and what its pointer names (see the
// definition for the order they are tried in).
// hand_pose/aim_pose: the hand's grip and aim this frame when the caller
// already has them (otherwise they are read here).
CPhysicsShellHolder* VrGrabTarget(CActor& actor, int side, bool near_only, bool& touching, const char*& why,
    bool pointer_missed=false, const Fmatrix* hand_pose=nullptr, const Fmatrix* aim_pose=nullptr);
void UpdateVrPickup(CActor& actor, int side, bool available, bool grip, bool trigger, bool may_grab,
    bool at_weapon);

// --- VrHandBodies.cpp -------------------------------------------------------
// The desktop pick along a hand's pointer, through foliage and grates.
bool VrPointerPick(CActor& actor, int side, collide::rq_result& hit);
void UpdateVrHandBodies(CActor& actor, const bool tracked[2], const Fmatrix hands[2]);
void UpdateVrClimb(CActor& actor, const bool available[2], const bool grip[2], const bool grip_edge[2]);

// --- VrFloorMagazines.cpp ---------------------------------------------------
void UpdateVrFloorMagazines(CActor& actor, bool controls_valid, const bool tracked[2],
    const Fmatrix hands[2], const bool down[2], const bool may_grab[2], CWeapon* held_weapon);
