#pragma once

#include "VrPistolState.h"
#include "VrReloadRules.h"
#include "VrFingerContact.h"
#include "VrWeaponMechanics.h"
#include "VrActionPath.h"
#include "VrPickupPolicy.h"
#include "VrSupportAction.h"
#include "VrWeaponPose.h"
class IKinematicsAnimated;
class VrClipSampler;
#include <memory>
#include <optional>
#include "weaponammo.h"

class CWeapon;
class CActor;

// Where the arm puts the wrist of a free hand on a controller: this far
// behind the grip centre, toward the forearm (player_hud_legs.cpp). A hold
// taken from the animation lines its wrist up with it.
constexpr float vr_wrist_behind_grip=0.065f;

// A hand pose in world space: anatomical frame (wrist to knuckles, little
// finger to index, and their cross product), the midpoint of the index and
// middle knuckles, and the direction of each joint's segment for the thumb,
// index and remaining fingers (three joints each).
struct VrHandBones
{
    Fmatrix hand;
    const xr_vector<Fmatrix>* locals=nullptr;
    shared_str visual;
    // Moved along the bore from where the animation holds the weapon
    // (SupportSlide): drawing presses it onto the surface there.
    bool slid=false;
};

struct VrHandPose
{
    Fvector basis[3];
    Fvector knuckles;
    Fvector finger[3][3];
    bool slid=false; // as VrHandBones::slid
    // The hands model the animation was sampled on.
    shared_str visual;
};
// Geometry for physical contact (VrContactField.h), shared by every weapon
// using one HUD visual and profile: the receiver with its magazine well
// opened, the cycling part (slide, bolt or charging handle) on its own, the
// magazine as a body and as a surface, and its way into the well.
struct VrWeaponContact
{
    vr::ContactField receiver, cycle, magazine_surface;
    // A charging handle that moves apart from the cycling part (its own
    // surface, not part of the receiver's). Both are placed where they are
    // drawn: their bones' poses against these bind poses (MagazineScene).
    vr::ContactField handle;
    u16 cycle_bone=u16(-1), handle_bone=u16(-1);
    Fmatrix cycle_bind=Fidentity, handle_bind=Fidentity;
    vr::MagazineBody magazine;
    // The seated magazine's exposed surface (below where it enters the
    // receiver), in model space: what a hand takes it out by.
    std::vector<vr::V3> grab_samples;
    vr::InsertPath path;
    float build_ms=0.f;
};
class CPhysicsShell;
class IKinematics;
class IRenderVisual;
struct firedeps;

// A physical interaction belongs to its weapon, not to a weapon class or a
// renderer singleton. No state in this object is serialized.
class CVrWeaponInteraction
{
public:
    explicit CVrWeaponInteraction(CWeapon* weapon);
    ~CVrWeaponInteraction();
    // Measures every weapon section with a HUD whose name contains `filter`
    // (all when empty) as its first draw does, without spawning it, and logs
    // what derivation found between "[VR dump]" markers. Two dumps compared
    // by tools/vr_derive_diff.py show what a code change did to any weapon.
    // Words: parts of section names (any matches); "derived" derives the
    // weapons with an audited profile too, as if they had none.
    static void DeriveDump(LPCSTR args);

    bool Supported() const { return m_supported; }
    // The shoulder this long gun was put away on by hand (1 right, 0 left),
    // or -1: VrSlotShoulderRight decides. Not saved.
    int HolsterShoulder() const { return m_holster_shoulder; }
    void SetHolsterShoulder(int side) { m_holster_shoulder=side; }
    bool Mode() const;
    bool Transform(Fmatrix& world) const;
    bool HeldPose(Fmatrix& world) const;
private:
    // HeldPose is asked 15-30 times a frame (the arms, each eye, sights,
    // fire points): worked out once per frame and state. The controllers are
    // read once a frame; what else moves the pose mid-frame is in the key.
    struct HeldPoseKey
    {
        u32 frame=u32(-1);
        bool carried=false, supporting=false, cup=false, break_hand=false, open=false, pumping=false, hold=false;
        int hand=0;
        float pump=0.f, recoil[3]{};
        Fvector hold_at{}, grip{};
        bool operator==(const HeldPoseKey& o) const
        {
            return frame==o.frame && carried==o.carried && supporting==o.supporting && cup==o.cup &&
                break_hand==o.break_hand && open==o.open && pumping==o.pumping && hold==o.hold && hand==o.hand &&
                pump==o.pump && recoil[0]==o.recoil[0] && recoil[1]==o.recoil[1] && recoil[2]==o.recoil[2] &&
                hold_at.x==o.hold_at.x && hold_at.y==o.hold_at.y && hold_at.z==o.hold_at.z &&
                grip.x==o.grip.x && grip.y==o.grip.y && grip.z==o.grip.z;
        }
    };
    mutable HeldPoseKey m_held_key;
    mutable Fmatrix m_held_pose=Fidentity;
    mutable bool m_held_valid=false;
    bool ComputeHeldPose(Fmatrix& world) const;
public:
    // A weapon passed to the other hand, where that hand took it.
    bool CarriedPose(Fmatrix& world) const;
    // A shot kicks the weapon up, a little aside and back in the hands; it
    // springs back over about a quarter of a second (vr_recoil).
    void OnShot();
    // Tracked placement of an unprofiled weapon's world model in the hand.
    bool GenericTransform(Fmatrix& world) const;
    bool AimTransform(Fmatrix& world) const { return Transform(world) || GenericTransform(world); }
    // The muzzle in the weapon's model space: the profile's, or the game's
    // fire point on an unprofiled weapon.
    const Fvector& MuzzleLocal() const;
    // The head is behind the weapon on its bore line, looking along it:
    // aiming down the sights, which earns the aimed-fire accuracy the
    // desktop gives the zoom key.
    bool Aiming() const;
    // Right-hand frame on a profiled weapon's grip: k wrist-to-knuckles, j up
    // the grip toward the index finger, c the grip centre held by the palm.
    // Unprofiled weapons instead follow the hand (see GenericTransform).
    bool HandFrame(Fmatrix& frame) const;
    // Grip radius and world trigger point for the right hand's fingers.
    float HandGripRadius() const { return m_profile.hand_grip_radius; }
    bool TriggerPoint(Fvector& world) const;
    // How the weapon's first-person idle animation holds it, placed on the
    // tracked weapon for a physical hand (0 left, 1 right): the weapon hand
    // takes the animation's right hand, a supporting hand its left. A hand of
    // the other side is mirrored across the weapon.
    bool AuthoredHand(int side, VrHandPose& pose) const;
    // The first-person hand exactly as the weapon's animations pose it: the
    // hand bone in world space and every bone's local pose, for the arms
    // model when it is the same first-person hands visual.
    bool AuthoredBones(int side, VrHandBones& bones) const;
    // The red dot or the scope lens, drawn into the current eye.
    void RenderSight();
    // Each update: the zoomed view the scope needs, if the head is at it.
    void RequestScopeView() const;
    bool BreakAction() const { return m_profile.break_action; }
    // Shells are loaded by hand: break-action barrels or a tube loading port.
    bool ShellLoading() const { return m_profile.break_action || m_profile.tube || m_profile.internal_rounds; }
    // The surfaces a hand closes on (0 left, 1 right), in weapon model space:
    // the weapon for the weapon hand or a supporting hand, the held magazine
    // for a hand holding one. With the weapon's world transform, the bore
    // height the thumb stays below (model space), what is held (for the
    // grip cache) and what kind of hold it is.
    bool GripField(int side, vr::ContactScene& scene, Fmatrix& world, float& ceiling,
        const void*& object, vr::GripKind& kind);
    bool HolsterPointFor(CActor& actor, Fvector& point, float& range) const;
    // Grip frame the support hand should follow instead of its controller.
    bool SupportHandOverride(Fmatrix& grip) const;
    // Hands are symmetric: the weapon is held by whichever hand drew it and
    // the other hand supports. Role 1 is the weapon hand, role 0 support.
    int WeaponHand() const { return m_hand; }
    // claim: the weapon is on its way into that hand (a draw or a wield). A
    // weapon stowed by that hand keeps the side but is not drawn into it.
    void SetWeaponHand(int hand, bool claim=true) { m_hand=hand ? 1 : 0; m_carried=false; m_hand_claimed=claim; }
    // In its own slot, whose draw is queued and not yet done.
    bool InQueuedSlot(CActor& actor) const;
    // Each frame, for every weapon the actor carries (backpack included): a
    // claim lasts only while the weapon is on its way (vr::ClaimHolds).
    void UpdateHandClaim(CActor& actor);
    // Anything left in the pouch for this weapon.
    bool PouchHasAmmo() const;
    // The rounds the pouch gives (an index into the weapon's ammunition
    // types): the next magazine drawn is filled with them, and shells or a
    // native reload take them next. SwapPouchAmmo moves on to the next type
    // the player carries (false when there is no other); PouchAmmoShown is
    // true for 1.5 s after a change, to label the pouch.
    u8 PouchAmmo() const;
    bool SwapPouchAmmo();
    bool PouchAmmoShown(shared_str& section) const;
    // Samples the first-person grips ahead of the draw; true if it did work.
    bool Prepare();

    // The weapon's collision boxes for physical pushing, in model space.
    bool CollisionBoxes(Fmatrix& world, const xr_vector<Fobb>*& boxes);
    xr_vector<Fobb> m_collision_boxes, m_generic_box;
    int HandOf(int role) const { return role ? m_hand : 1-m_hand; }
    // The left hand steadies the weapon (two-handed hold).
    bool Supporting() const { return m_supporting; }
    void RenderPouch(CActor& actor);
    bool FireDependencies(firedeps& deps) const;
    bool Render();
    void RenderHolster();
    void RenderLooseMagazines();
    void RenderBasicMagazines();
    void UpdateProps();
    void Interact(CActor& actor);
    // Interact for a weapon without physical handling (no profile, or no
    // mechanism): held, supported, handed over and fired through the game.
    void InteractGeneric(CActor& actor);
    // This frame's controls, kept for the next frame's edges (m_trigger,
    // m_grip, ...): written once by CommitEdges when the update leaves by
    // any path. A path that passes the weapon between hands changes `next`
    // before it leaves.
    struct Edges { bool trigger, grip[2], button, secondary, mag_trigger; };
    struct CommitEdges
    {
        CVrWeaponInteraction& self;
        const Edges& next;
        ~CommitEdges();
    };
    // The support hand on the weapon (SupportNear): sets m_supporting,
    // m_support_cup and m_support_along; returns SupportNear's kind.
    int DetectSupport(const Fvector& weapon_hand, const Fvector& muzzle, const Fvector& axis,
        const Fvector& support);
    // A native first-shot transaction must finish in the original hand.
    // Wait keeps the outgoing release edge until transfer or cancellation.
    enum class HandOverResult { None, Wait, Done };
    HandOverResult HandOverOnRelease(CActor& actor, Edges& next);
    // ReleaseWanted then UpdateRelease for this frame's weapon grip; true
    // once the weapon has been let go.
    bool StepRelease(CActor& actor, bool grip);
    // One frame's handling of a profiled weapon in the hand (Interact),
    // shared by its steps below: the weapon and both hands in the world,
    // the support hand in the weapon's frame (local), this frame's
    // controls, and the places the hands work.
    struct Handling
    {
        Fmatrix world, left, right;
        Fmatrix held_root;  // a held magazine's root in the support hand (MagazineHandPose)
        Fvector local, cycle_point, mag_base;
        bool left_valid=false, held_valid=false;
        bool on_part=false;  // the support hand works a part or the belt (UpdateParts)
        bool has_release=false;  // a bolt catch, slide stop or release
        bool grip[2]{}, mag_trigger=false, mag_hold=false, button=false, secondary=false;
        vr::SupportAction support_action=vr::SupportAction::None;
    };
    // What a pull of the support trigger works this frame (vr::ChooseSupportAction).
    vr::SupportAction ChooseSupport(CActor& actor, const Handling& h);
    // The support trigger on the slide or charging handle takes it.
    void StartRack(const Handling& h);
    // The support grip on the fore-end or round the weapon hand steadies it.
    void UpdateSupport(CActor& actor, const Handling& h);
    // A taken slide or handle follows the hand; let go, it returns or stays.
    void MoveRack(const Handling& h);
    // The magazine release (B) and the support hand taking a magazine out.
    void WorkMagazine(Handling& h);
    // Bolt catch, handle slap, dust cover, launcher, and the A button.
    void WorkActionControls(CActor& actor, const Handling& h);
    // A held magazine into the well, or let go of it.
    void InsertMagazine(CActor& actor, Handling& h);
    bool HolsterGrabDistance(CActor& actor, const Fvector& hand, float& distance) const;
    // An open weapon hand at the holster now puts the weapon back (its ring).
    bool HolsterArmed() const { return m_holster.armed; }
    void ReleaseInactiveHeld();
    bool LeftHandBusy() const;
    float DetachedRoundWeight() const;
    // keep_measured: the weapon leaves the hand but stays the same weapon (a
    // drop): its model and measurements stay, so the next pickup and the
    // frames before the drop lands do not measure it again.
    void PrepareSave(bool keep_measured=false);
    void PrepareSnapshot();
    bool CanFire() const;
    // The action is cycling from the last shot (VR chamber): an automatic
    // weapon waits for it between shots instead of stopping.
    bool Cycling() const;
    // The action is worked by hand after each shot (a bolt-action, a lever).
    bool WorkedByHand() const { return m_profile.manual_cycle; }
    // It has an automatic fire mode (installed upgrades included), as the
    // mechanics classification read it.
    bool AutomaticMode() const { return m_profile.evidence.automatic_mode; }
    // A pump gun set to a self-loading fire mode (a SPAS-12's semi-auto):
    // it cycles itself and its fore-end is locked.
    bool PumpSelfLoading() const;
    // A pump gun: a fore-end that is worked, no bolt of its own; or a
    // dual-mode gun (DualPump) set to pump.
    bool PumpGun() const { return m_profile.pump_bone.size() && (!m_profile.cycle_bone.size() || m_pump_mode); }
    // A self-loading tube shotgun with a fore-end of its own (GAMMA's
    // SPAS-12): the fire-mode button (A) switches it between self-loading
    // and pump action. Session state, set to self-loading when drawn.
    bool DualPump() const { return m_profile.tube && m_profile.pump_bone.size() && m_profile.cycle_bone.size(); }
    bool m_pump_mode=false;
    bool BlockReload() const;
    // Reloaded by hand only (magazine or shells); native reload is disabled.
    bool PhysicalReload() const { return Mode() && (m_profile.detachable || ShellLoading()); }
    bool UsesMagazineButton() const
    { return Mode() && (m_profile.detachable || m_basic_magazine || m_profile.break_action); }
    bool FillingMagazine() const { return m_filling; }
    bool DeferShellDrop() const
    { return Mode() && ((m_profile.chamber && m_profile.cycle_travel>0) ||
        m_profile.break_action || (m_profile.tube && m_profile.pump_bone.size())); }
    void CartridgeFired();
    void Destroy();
    // Which magazine this weapon takes: its dedicated magazine meshes, else
    // its HUD model's magazine bone. Empty for none a hand can hold.
    shared_str MagazineKind() const;
    // A magazine from the world (VrFloorMagazine), in the support hand: it
    // becomes the held magazine, held as one from the pouch, if it is this
    // weapon's kind and every round is one the weapon fires, and the hand is
    // free for it.
    bool TakeFloorMagazine(const shared_str& kind, xr_vector<CCartridge>& rounds);

private:
    struct Profile
    {
        shared_str section, visual, mag_bone, cycle_bone, rounds_bone;
        // Full-sized alternate magazines used by native reload animations.
        // VR renders only its canonical magazine; IDs are cached solely for
        // derivation on this skeleton, names for posing other private copies.
        xr_vector<shared_str> reload_magazines;
        xr_vector<u8> reload_magazine_mask;
        // A bone of an alternate reload magazine on a skeleton of `count` bones.
        bool ReloadMagazineBone(u16 id, size_t count) const
        { return reload_magazine_mask.size()==count && reload_magazine_mask[id]; }
        // Parent-relative bone poses in the idle animation: where parts
        // bound away from the gun (belt links) rest on it. rest_contents
        // lists the magazine's bones that rest elsewhere than bound, parents
        // first; VR poses them so while it moves the magazine.
        xr_vector<Fmatrix> rest_local;
        xr_vector<u16> rest_contents;
        // rest_local when the magazine has contents posed at rest, else null.
        const xr_vector<Fmatrix>* MagazineRest() const { return rest_contents.empty() ? nullptr : &rest_local; }
        // Parts the shot animation throws clear of the gun (a pump or bolt
        // gun's spent case): kept still while the hand works the action, and
        // hidden while it is open. The first with a mesh (case_bone) is
        // thrown as a loose case when the weapon has no shell particles.
        xr_vector<u8> thrown_mask;
        shared_str case_bone;
        Fvector case_center{}, case_half{};
        // A charging handle apart from the bolt (it does not move on shots),
        // found in the reload animation; the hand racks it. The bolt then
        // keeps its own shot travel (bolt_travel) apart from the handle's
        // stroke (cycle_travel).
        shared_str handle_bone;
        float bolt_travel=0.f;
        // The cycling part was found only in the reload (a charging handle):
        // it moves when worked by hand, never with the shots.
        bool cycle_from_reload=false;
        // A manually worked action (a bolt-action, a lever): a shot leaves the
        // fired case in the chamber until the hand works it, and it stays
        // where the hand lets go of it.
        bool manual_cycle=false;
        // Fires from an open bolt (a PPSh, a Thompson): its idle holds the
        // bolt back, cocked. Drawn as the game animates it (back at rest,
        // forward on each shot); no chamber to charge, nothing to rack.
        bool open_bolt=false;
        // An open bolt no animation moves (GAMMA's Thompsons): VR snaps it
        // forward and back on each shot from where the model rests it.
        bool open_bolt_static=false;
        // The shot animation moves the magazine's contents (GAMMA's RPD's
        // belt): VR adds no feed step of its own (PoseBeltFeed).
        bool shot_moves_magazine=false;
        // Loaded a round at a time through the open action (a bolt-action's
        // internal magazine), at loading_port, only with the action open.
        bool internal_rounds=false;
        // The manual action's path from its own shot animation (a bolt's
        // lift and pull, a lever's swing), and every moving bone's
        // model-space pose per path key (key-major).
        vr::ActionPath action_path;
        xr_vector<u16> path_bones;
        xr_vector<Fmatrix> path_poses;
        // A dust cover: hinged at the ejection port, opened by the bolt's
        // first movement, closed by hand (its open model-space pose).
        shared_str dust_cover_bone;
        Fmatrix dust_cover_open=Fidentity;
        Fvector dust_cover_grab{};
        // Where the bolt catch or slide stop is pressed (model space).
        Fvector release_point{};
        // What derivation measured, for the mechanics classification.
        vr::MechanicsEvidence evidence;
        shared_str mag_extra_bones[2];
        Fvector grip_offset{}, support_offset{}, mag_grip{}, mag_base{}, cycle_grab{};
        // The handle's knob on the other side, when its part has one on each
        // (CycleGrabMirror).
        std::optional<Fvector> cycle_grab_mirror;
        // Round the fore-end, from the mesh (derived long guns).
        std::optional<Fvector> forend_point;
        Fvector muzzle{}, shell_point{};
        // The HUD section's silencer and second (launcher) fire points,
        // placed as the muzzle is; unset, those shots flash at the muzzle.
        Fvector muzzle_silencer{}, muzzle2{};
        bool has_muzzle_silencer=false, has_muzzle2=false;
    Fvector launcher_grab{};
        Fvector mag_box_half{}, mag_box_center{};
        float cycle_travel=0, extraction=0, grab_radius=0.08f, guide_radius=0.015f;
        // The mechanism's stroke and hold-open, as profiled and classified
        // (ClassifyMechanics): for an internal-magazine self-loader or a
        // launcher they differ from cycle_travel and slide_stop. Copied into
        // m_mechanism by ApplyMechanismLimits, so a reset never loses them.
        float travel_limit=vr::MagazineMechanism<xr_vector<CCartridge>>::travel;
        bool locks_on_empty=true;
        float hand_grip_angle=18.f, hand_grip_radius=0.015f;
        std::optional<Fvector> hand_grip_center, trigger_point;
        // Profile generated from the HUD model's own bones and meshes:
        // automatic until measured, derived once AutoProfile has run.
        bool automatic=false, derived=false;
        // Break-action (hinged barrels) loaded one shell at a time.
        bool break_action=false;
        shared_str hinge_bone, shell_bones[2];
        shared_str loose_round_visual; // complete cartridge when the HUD has only a clip or spent case
        float hinge_angle=0.f;
        Fvector hinge_point{};
        Fvector shell_center[2]{}, shell_box_half{};
        // Tube shotguns: shells enter a loading port; an optional pump bone
        // (fore-end) must be cycled by hand after each shot.
        bool tube=false;
        Fvector loading_port{}, pump_grab{};
        shared_str pump_bone;
        float pump_travel=0.f;
        // Loose display parts (a shell posed at the ejection port or in a
        // reload position) that must not show on a static weapon.
        shared_str hidden_parts[3];
        // Measured magazine geometry: the well axis (into the well, model
        // space) and the magazine point that sits in the fist.
        Fvector well_axis{0.f,1.f,0.f};
        bool explicit_well_axis=false;
        // Moving parts a hand works (a top cover on its hinge), the order the
        // reload must follow (VrReloadRules.h) and a belt laid by hand. Only
        // weapons whose profile names them have any.
        int part_count=0;
        vr::PartDef parts[vr::max_parts];
        shared_str part_names[vr::max_parts], part_bones[vr::max_parts];
        bool part_from_animation[vr::max_parts]{};
        vr::ReloadRules rules;
        vr::BeltDef belt;
        shared_str belt_free_bone;
        // The belt's links from the box to the feed tray (DeriveReload), and
        // for each bone the link it moves with (-1: none), by bone id.
        xr_vector<u16> belt_chain_ids;
        xr_vector<s16> belt_link_of;
        // Bore height in model space; the thumb stays below it.
        float bore_height=0.f;
        // Diagnostics for [VR align] (LogAlignment), changing nothing: the
        // grip found on the mesh (else estimated from the trigger) and how
        // far a stock wrist's correction raised it.
        bool grip_from_mesh=false;
        float grip_wrist_lift=0.f;
        bool two_handed=false, detachable=false, chamber=false, slide_stop=false;
    } m_profile;
    // A fired shell or case, or an unprofiled pistol's empty magazine (proxy,
    // drawn as a box). Real magazines left in the world are VrFloorMagazines.
    using PoseCache=vr::WeaponPoseCache<xr_vector<Fmatrix>>;
    struct DropProp
    {
        CPhysicsShell* shell=nullptr;
        IRenderVisual* visual=nullptr;
        xr_vector<Fmatrix> poses;
        PoseCache pose_cache;
        u32 expires=0;
        bool proxy=false;
        shared_str part; // only this bone (and children) is shown
        // That part's bounds in model space, when it is not a break-action's shell.
        bool own_box=false;
        Fvector box_center{}, box_half{};
    };
    CWeapon& m_weapon;
    int m_hand=1;
    // A hand took this weapon (holster grab, pickup) and its draw is still
    // pending; a weapon drawn any other way goes to the dominant hand. Not a
    // timeout: a first pickup measures the model, a hitch longer than any.
    bool m_hand_claimed=false;
    // The active item at the last update: a change is a draw (Interact).
    bool m_in_hand=false;
    // Where it was last drawn (vr_contact_debug logs a change).
    const char* m_drawn_as=nullptr;
    void NoteDrawn(const char* where);
    bool m_support_cup=false;
    float m_support_along=0.f; // fore-end hold point along the barrel line
    // The animation's support hand moved along the weapon to where the hand
    // closed on it (SupportSlide), for the hold point it was worked out for.
    mutable float m_slide_along=-1.f;
    // Where the support hand closed on the weapon (weapon space), taken in
    // the first frame it holds it: the gun is steered by that point.
    mutable std::optional<Fvector> m_support_hold;
    // Handed over to the support hand from a fore-end hold drawn from the
    // animation: that hand stays drawn so, where it held (HandOver).
    bool m_carry_authored=false;
    bool m_carry_pump=false; // the carrying hand keeps a pump at its current stroke
    Fvector m_carry_slide{};
    mutable Fvector m_slide{};
    bool m_supported=false, m_holstered=false, m_filling=false;
    int m_holster_shoulder=-1;
    // Recoil still to return (radians up and aside, metres back), as of
    // m_recoil_time; it decays as it is read.
    mutable float m_recoil_pitch=0.f, m_recoil_yaw=0.f, m_recoil_back=0.f;
    mutable u32 m_recoil_time=0;
    // How fast it springs back (per second): slower held in one hand.
    float m_recoil_settle=14.f;
    void DecayRecoil() const;
    bool m_reported_generic=false;
    bool m_basic_magazine=false;
    unsigned m_basic_reload_rounds=0;
    bool m_release_pending=false;
    // Where a pending release goes, fixed when the hand let go (the holster,
    // or out of the hand), and since when it waits on the weapon.
    bool m_release_holster=false;
    u32 m_release_since=0;
    // Whether opening the weapon hand now lets go of the weapon (see
    // ReleaseWanted); updated every frame the weapon is in the hand. Sets
    // m_release_holster.
    bool ReleaseWanted(CActor& actor, bool opened, bool open);
    // Starts or continues a release; true once the weapon has been let go.
    bool UpdateRelease(CActor& actor, bool wanted, bool gripped);
    // Native firing/animation or a queued firing transition must finish first.
    bool NativeFirePending() const;
    // The weapon is out of the hand: forget how it was held.
    void ResetHold();
    // Both hands let go of everything they work on the weapon (slide,
    // magazine on its way in, support) and the controls' edges reset;
    // StowHands also puts the mechanism away (switching weapons, lost
    // controls). Normalize and ReleaseInactiveHeld add their own state.
    void ReleaseHands();
    void StowHands();
    // From the weapon hand to where letting go puts the weapon back, with
    // that place's reach; false without a hand or a holster.
    bool HolsterDistance(CActor& actor, float& distance, float& range) const;
    // At rest: idle or misfired, with nothing queued or pending.
    bool WeaponReady() const;
    // A pistol-class weapon (one hand); everything else is a long gun.
    bool Handgun() const { return m_handgun; }
    // A CWeaponPistol that is a handgun: pistol magazine, evidence, sounds.
    bool PistolHandgun() const { return m_pistol_handgun; }
    // Both read once from the weapon's class and config (LoadProfile).
    bool m_handgun=false, m_pistol_handgun=false;
    // The hand has been clear of the holster since it took the weapon, or has
    // held it for vr::holster_arm_ms; since when it is in the hand; whether its grip has
    // held it since (a weapon drawn by a key stays until gripped once).
    vr::HolsterState m_holster;
    u32 m_active_since=0;
    // The rounds the pouch gives (-1: the weapon's own type) and when the
    // player last changed them.
    u8 m_pouch_ammo=u8(-1);
    // Inventory scans asked several times a frame (per eye, the scope pass,
    // the update), kept for the frame: [frame, pouch choice] -> result.
    mutable u32 m_reserve_frame=u32(-1), m_preview_frame=u32(-1), m_pouch_frame=u32(-1);
    mutable u8 m_pouch_for=u8(-1), m_pouch_result=0;
    mutable bool m_reserve_result=false;
    mutable unsigned m_preview_result=0;
    u32 m_pouch_swap_time=0;
    bool m_restore_empty_chamber=false;
    float m_cycle_visual_slide=0.f;
    bool m_trigger=false, m_grip[2]{}, m_button=false, m_secondary=false;
    bool m_mag_trigger=false;
    bool m_racking=false, m_racked=false, m_inserting=false;
    bool m_pouch_preview=false, m_supporting=false;
    float m_rack_start=0;
    // Where the hand was across the bore when it took the handle (an HK's
    // is lifted into its notch).
    float m_rack_lift_start=0.f;
    u32 m_debug_time=0;
    // The held magazine once the well has taken it: it slides on the
    // well's path at the hand's depth (VrMagazineContact.h).
    vr::MagazineGuide m_guide;
    // Seats the held magazine: fills it, puts it in, click and haptics.
    void SeatMagazine(const Fvector& sound_at);
    std::shared_ptr<const VrWeaponContact> m_contact, m_world_contact;
    std::shared_ptr<struct VrContactJob> m_contact_job;
    const IRenderVisual* m_world_contact_visual=nullptr;
    // The weapon's surfaces a magazine meets (receiver and cycling part).
    void MagazineScene(vr::ContactScene& scene) const;
    // The held magazine's pose in the world: on the well's path while the
    // well has it, else where the hand holds it (`world` if neither).
    Fmatrix SolvedMagazine(const Fmatrix& world) const;
    // Whether that pose is known (the well has it, or the hand is tracked).
    bool HeldMagazineKnown() const;
    // Where the separate charging handle is drawn: with the slide while
    // racked, springing home when let go; never with a shot's cycling.
    float m_handle_slide=0.f;
    // The racking hand's own pull from where it closed on the handle: a
    // separate handle travels with the hand even while the bolt is locked
    // back (an AR-15's handle is pulled back to meet a held-open carrier).
    float m_rack_hand_z=0.f, m_handle_pull=0.f;
    bool m_handle_hooked=false;  // an HK's handle in its notch
    bool m_launcher_toggle=false;  // switched at the start of the next Interact
    // What the weapon is (classification and overrides), and where its
    // manual action is along its path (0 closed and turned down, 1 open).
    vr::WeaponMechanics m_mechanics;
    float m_action_u=0.f;
    Fvector m_action_offset{};
    bool m_dust_open=false;
    // The grab point of the cycling part where it is now (model space).
    Fvector CycleGrabLocal() const;
    float CycleGrabTravel() const;
    // Of the places to take the cycling part (its mesh's point, a second
    // knob, the reload's hand on it), the one nearest the hand (model space).
    Fvector NearestCycleGrab(const Fvector& hand) const;
    // This weapon's log lines: "* [VR weapon] <profile section>: ..." and,
    // for Warn, the "! " form. printf-style, as Msg.
    void Note(LPCSTR format, ...) const;
    void Warn(LPCSTR format, ...) const;
    // The handle's second knob, across the bore (a P90's), if it has one.
    std::optional<Fvector> CycleGrabMirror() const;
    // The bore line across the model: the trigger's x (most models are
    // built on x=0; some, such as the Mosin's, about 15 cm aside).
    float BoreX() const;
    // A point mirrored across the bore line: a left hand's place from a
    // right hand's, a knob on the other side.
    Fvector AcrossBore(Fvector point) const { point.x=2.f*BoreX()-point.x; return point; }
    // Classifies the weapon from the profile's evidence, applies the
    // overrides file and sets the profile's mechanism from the result.
    void ClassifyMechanics(IKinematics& model);
    // The first-person animation holds the support hand behind the weapon
    // hand (an RPG-7's rear grip): it supports from behind.
    bool RearSupport() const;
    // Records a manual action's path from a motion (knob: the bone whose
    // point grab the hand holds; receiver: the frame it is measured in).
    void RecordActionPath(VrClipSampler& clips, IKinematics& model,
        const xr_vector<Fmatrix>& bind, LPCSTR key, u16 knob, const Fvector& grab, u16 receiver, u16 moving_bolt, bool knob_rear);
    // Poses the manual action's bones at m_action_u and the dust cover.
    bool PoseActionPath(IKinematics& model, xr_vector<Fmatrix>& poses) const;
    // A magazine taken out of the weapon (or caught sliding out) by hand is
    // held where the hand closed on it (m_mag_in_hand), not moved so its
    // base is in the fist; cleared when it goes back or a fresh one is drawn.
    bool m_mag_grabbed=false;
    // The held magazine came from the world: it seats with what it holds,
    // and is filled from the pouch only once it has been in the pouch.
    bool m_mag_from_world=false;
    // A magazine taken out by hand stays exactly where the hand closed on
    // it: its pose relative to the hand's grip at that moment.
    Fmatrix m_mag_in_hand=Fidentity;
    // How far a hand at this point is from the seated magazine's exposed
    // body or floorplate, flt_max when it would not close on it.
    float MagazineGrabDistance(const Fmatrix& world, const Fvector& hand) const;
    // Travel at the last friction tick of a magazine sliding in or out.
    float m_slide_tick=0.f;
    unsigned m_hand_epoch=0;
    bool m_have_hand[2]{};
    Fmatrix m_previous_hand[2];
    Fvector m_hand_velocity[2]{}, m_hand_angular[2]{};
    vr::PistolInputGate m_input;
    // The weapon hand's trigger, with a reset on partial release.
    vr::FireTrigger m_fire_trigger;
    vr::MagazineMechanism<xr_vector<CCartridge>> m_mechanism;
    IRenderVisual* m_visual=nullptr;
    IRenderVisual* m_mag_visual=nullptr;
    unsigned m_mag_visual_count=unsigned(-1);
    xr_vector<Fmatrix> m_poses[2];
    PoseCache m_pose_cache[2];
    u32 m_posed_frame=u32(-1);
    int m_posed_inserted=-2;
    xr_list<DropProp> m_props;
    // Break-action state: 0 empty, 1 live, 2 spent, per barrel.
    u8 m_chambers[2]{};
    bool m_break_open=false, m_break_initialized=false, m_lever_armed=false;
    float m_break_angle=0.f;
    u32 m_lever_time=0, m_break_time=0;
    // The support hand on the fore-end carries the barrels on their hinge;
    // the receiver keeps the weapon hand's aim from when it took them.
    bool m_break_hand=false;
    float m_break_hand_ref=0.f;
    Fmatrix m_break_receiver;
    // The support hand closed on the barrels (ahead of the hinge, judged
    // with them closed) rather than the receiver: it moves with them.
    bool m_support_barrels=false;
    bool BarrelsOpen() const { return m_profile.break_action && m_break_angle>0.f; }
    bool m_slide_back_sounded=false;
    bool m_slide_lock_logged=false; // last slide lock state written to the log
    bool m_shell_held=false;
    IRenderVisual* m_shell_visual=nullptr;
    shared_str m_shell_visual_name;
    xr_vector<Fmatrix> m_shell_poses;
    PoseCache m_shell_pose_cache;
    IRenderVisual* m_pouch_visual=nullptr;
    shared_str m_pouch_section;
    // Pump stroke: travel (m), whether a live shell is chambered, and
    // whether a fired case still waits to be ejected.
    bool m_pumping=false, m_pump_back=false, m_pump_ready=true, m_pump_spent=false;
    float m_pump=0.f, m_pump_start=0.f;
    unsigned m_shell_rounds=0;
    // Model measurements use the same layout here and in the cache. The
    // profile is cached separately, before per-weapon classification.
    struct ModelMeasurements
    {
        // Weapon mesh point clouds (5 mm voxels) for finger contact.
        xr_vector<Fvector> cloud;
        // Sight glass meshes, their kind, and ordinary lens-textured meshes.
        xr_vector<Fbox> glass, lens_meshes;
        bool glass_dot=false, glass_soft=false;
        // Scope mesh and shaders, for finding the eyepiece and for the log.
        xr_vector<Fvector> optic_cloud;
        shared_str mesh_shaders;
        // Scope caps, covers and glass hidden by native aiming.
        shared_str scope_hidden[3];
        // Animation-local part poses and the sampled trigger pull.
        xr_vector<Fmatrix> bind_local, state_local[3];
        xr_vector<u8> state_part;
        u16 trigger_bone[2]{BI_NONE,BI_NONE};
        bool trigger_valid[2]{};
        Fmatrix trigger_pull[2];
    } m_model_data;
    // Both hands of the first-person idle animation in weapon model space
    // (0 left, 1 right), sampled once per first-person hands model.
    struct AuthoredHandRef
    {
        Fvector basis[3]{}, knuckles{}, palm{}, finger[3][3]{};
        bool valid=false;
    };
    // The same sampled data is used by this weapon and the hands cache.
    struct SampledHands
    {
        AuthoredHandRef authored[2];
        // Idle hands and the reload's hand holding the magazine below the well.
        xr_vector<Fmatrix> hands_local, maghand_local;
        Fmatrix hand_in_item[2], maghand_in_root;
        Fvector maghand_basis[3]{}, maghand_palm{};
        // The reload's hand on the bolt or charging handle, furthest back.
        Fvector handle_hand{};
        bool handle_hand_valid=false, palm_trusted=true;
        shared_str visual;
        // Diagnostics (LogAlignment): how the hands were placed on the drawn
        // model. carry: 0 as sampled, 1 carried onto the bind pose
        // (hands_to_drawn), 2 kept as the idle holds them; the weapon hand's
        // knuckles from the mesh without and with the carry (-1 unmeasured).
        int carry=0;
        // The weapon hand's index fingertip from the trigger (-1 unmeasured),
        // and, on a model with no trigger found, the fingertip as its trigger.
        float index_trigger=-1.f;
        std::optional<Fvector> hand_trigger;
        // Taken from the game's own first-person hands on the weapon in play
        // (CaptureLiveHands), not sampled on private copies.
        bool live=false;
        // Flat-screen aiming line: the sight's fallback (MeasureSight).
        Fvector aim_eye{}, aim_dir{0.f,0.f,1.f};
        bool aim_valid=false;
    } m_hands;
    shared_str m_authored_hands;
    bool m_authored_sampled=false;
    // Samples the hands (SampleAuthoredHands) when the first-person hands
    // model has changed or was never sampled for the measured weapon, and
    // measures the sight again when the scope has changed. It loads models
    // and plays animations, so it is called where the weapon is updated or
    // drawn (Interact, Prepare, RenderHolster), never from a pose query: the
    // const queries read the last results, or the measured grip before any.
    void RefreshAuthoredHands();
    void SampleAuthoredHands();
    // One hand (0 left, 1 right) of a first-person hands model as posed,
    // into m_hands.authored[side]: to_drawn maps the hands model's space onto
    // the weapon as VR draws it. index_tip: the right hand's index fingertip.
    // False when the model lacks the hand's bones.
    bool ExtractAuthoredHand(int side, IKinematics& model, const Fmatrix& to_drawn, Fvector* index_tip);
    // Whether the weapon hand holds the weapon (m_hands.palm_trusted), and a
    // trigger from its index fingertip on a model without one.
    void JudgeAuthoredHand(bool right_index, const Fvector& right_index_tip);
    // The hold as the game itself shows it: once the weapon has been idle in
    // the hand half a second, its own first-person hands are read relative to
    // the weapon's receiver and replace the sampled hold (logged with how far
    // they differ), cached for its copies and on disk. The sampled hold, made
    // on private copies, missed the gun on about a quarter of GAMMA models.
    void CaptureLiveHands();
    u32 m_live_idle_since=0;
    // The capture waits for the game's hand to hold still: its last pose on
    // the weapon and for how many frames it has stayed there.
    Fmatrix m_live_last=Fidentity;
    int m_live_still=0;
    // Rejected captures (the hand off the gun); after 20 the sampled hold
    // stays for the session.
    int m_live_rejected=0;
    // The weapon model's receiver bone and its bind pose, found once.
    u16 m_live_receiver=BI_NONE;
    Fmatrix m_live_receiver_bind=Fidentity;
    // A capture on disk outlives builds: it is what the game shows, keyed by
    // the HUD section, its model and the hands (LiveKey), not by this code.
    // Applied as soon as the hold is sampled or restored.
    xr_string LiveKey(bool by_section=false) const;
    bool ApplyLiveHands();
    void SaveLiveHands() const;
    // The sampled hands, shared like Measured by every copy of a derived
    // weapon with the same first-person hands.
    static xr_map<xr_string,std::shared_ptr<const SampledHands>>& SampledHandsCache();
    // Where the hand takes the cycling part, chosen while it is not racking
    // (NearestCycleGrab); the mesh's point until then.
    std::optional<Fvector> m_cycle_grab_at;
    bool MagazineInHand(Fmatrix& root) const;
    // Sights, in weapon space: the line through them and the glass on it
    // (window: the eyepiece or the red dot's glass; eye: a nominal eye rear
    // metres behind it; radius: the glass size). Measured from the model
    // (see MeasureSight); the flat-screen aiming camera is a fallback.
    enum { SightIron, SightCollimator, SightScope };
    struct SightInfo
    {
        bool valid=false;
        int type=SightIron;
        Fvector eye{}, dir{0.f,0.f,1.f}, window{};
        float rear=0.f, front=0.f, radius=0.012f, magnification=1.f, lens_fov=20.f;
    };
    SightInfo m_sight;
    // Measured for this scope state (RefreshAuthoredHands); the glass comes
    // from MeasureOptics, the fallback line from the hands.
    bool m_sight_measured=false, m_sight_scope=false;
    void MeasureSight();
    bool EyeOnSight(const Fvector& eye, Fmatrix& world, Fvector& direction, Fvector& window,
        Fvector& offset, float& behind) const;
    mutable u32 m_scope_refused_time=0;
    void MeasureOptics(IKinematics& model);
    Fvector GripLocal() const;
    // Where the support hand steers and is drawn on the fore-end: the
    // mesh's fore-end when the animation's left hand is not used, else the
    // support offset.
    std::optional<Fvector> ForendPoint() const;
    // How far the animation's support hand is moved, in weapon space, to
    // lie where the real hand closed on the fore-end (m_support_along).
    Fvector SupportSlide() const;
    // Whether a point is round the weapon's solid (a stock's wrist, a
    // fore-end): mesh above and below it within 5 cm, across the bore line
    // and under it. `mesh`: whether there is any mesh there at all.
    bool OnWeapon(const Fvector& point, bool* mesh=nullptr) const;
    // Whether a fist centred there closes on the weapon: mesh above and
    // below it within 4 cm (SupportSlide's places to hold).
    bool FistClosesOn(const Fvector& centre) const;
    // Mesh in a 2.4 cm slice along the bore, 3 cm across and under the bore
    // line, within `up` above the point and `down` below it.
    void MeshAround(const Fvector& point, float up, float down, bool& above, bool& below) const;
    // The squared distance from a point (model space) to the nearest point
    // of the weapon's mesh (flt_max without one).
    float CloudDistanceSq(const Fvector& point) const;
    // The support hand is drawn as the animation holds the weapon (animated
    // grips, its sampled left hand trusted on a measured mesh).
    bool AnimatedSupportHand() const;
    // vr_contact_debug: the drawn fore-end hand against the real one.
    void LogSupportHold(const Fvector& support) const;
    // A hand (world) close enough to the weapon's mesh to hold it: within
    // `reach` of it (true without a measured mesh).
    bool HandOnMesh(const Fmatrix& world, const Fvector& hand, float reach) const;
    // A fore-end carry hold after HandOver, in that hand's frame. Taking the
    // handle instead selects the receiving hand's normal aiming/firing pose.
    bool m_carried=false;
    Fmatrix m_carry;
    bool HandOver(Edges& next);
    // HandOver and TakeBack: trade roles and require a fresh trigger pull.
    void SwapHands(Edges& next, bool support_grip);
    // The support hand takes a magazine out of the well (seated, or still
    // sliding out at `along` on its insert path) into the hand.
    void TakeMagazineIntoHand(const Fmatrix& world, const Fmatrix& hand, const Fmatrix& magazine,
        float along, Fmatrix& held_root);
    void PullTrigger();
    u32 m_fire_queued=0;
    void TakeBack(Edges& next);
    // Short handling sounds (see PlayHandling).
    enum { SoundMagOut, SoundMagIn, SoundSlideBack, SoundSlideForward, SoundPumpBack,
        SoundPumpForward, SoundBreakOpen, SoundBreakClose, SoundShellIn,
        SoundCoverOpen, SoundCoverClose, SoundBeltIn, SoundFireMode, SoundCount };
    ref_sound m_handling[SoundCount];
    bool m_handling_loaded[SoundCount]{};
    void PlayHandling(int sound, const Fvector& position);
    // Parts posed by animation or state (see AnimateParts).
    void PrepareParts(IKinematics& model);
    bool AnimateParts(IKinematics& model, xr_vector<Fmatrix>& poses);
    // Hides on the VR copy what the live first-person model hides (fired
    // rockets, grenades, rounds taken by a reload); true if anything changed.
    bool CopyNativeVisibility(IKinematics& model, std::vector<unsigned char>& counted) const;
    void MeasureParts(IKinematics& model);
    float m_trigger_value=0.f;   // weapon hand's trigger travel, 0..1
    unsigned m_break_state=2;    // loaded barrels the gun shows (anm_idle_N)
    bool m_break_state_set=false;
    xr_vector<u16> m_overlay_order;
    xr_vector<u8> m_overlay_receiver, m_overlay_owned;
    // The cycling part (bolt, slide or charging handle) found from the
    // weapon's first-person shot or reload animation.
    void MeasureCycle(IKinematics& model);
    // A revolver's cylinder the engine class does not show (evidence.cylinder_part).
    void MeasureCylinder(IKinematics& model, VrClipSampler& clips, const xr_vector<Fmatrix>& bind);
    void MeasureThrownParts(IKinematics& model);
    // Moving parts (see Profile::parts): their state this session, never
    // saved; the belt; the magazine slot the belt was laid for.
    vr::PartState m_parts[vr::max_parts];
    vr::BeltState m_belt;
    // A shot's feed step: 1 at the shot, down to 0 as the belt advances one
    // link toward the tray and settles back (PoseBeltFeed).
    float m_belt_feed=0.f;
    bool m_belt_more=false;  // a shot came during the step: another follows it
    void PoseBeltFeed(IKinematics& model, xr_vector<Fmatrix>& poses) const;
    bool m_parts_ready=false;
    int m_parts_inserted=-1;
    u32 m_rule_logged=0;
    void ResetParts();
    vr::ReloadFacts Facts() const;
    // Whether the rules let this happen now; refused attempts are logged
    // with the part that blocks them (at most once a second).
    bool RuleAllows(const vr::GateList& gates, LPCSTR what);
    // The support hand on a part or the belt: true while it holds one.
    bool UpdateParts(const Fmatrix& world, bool left_valid, const Fmatrix& left, bool grip, bool grip_edge);
    // Turns or slides each part's bones by its value, and swings a carried
    // belt end toward the hand; true when anything moved.
    bool PoseParts(IKinematics& model, xr_vector<Fmatrix>& poses) const;
    bool BeltHidden() const;
    // Hinge axis, pivot and opening angle from the reload animation.
    void MeasurePartMotion(IKinematics& model);
    bool m_strategy_logged=false;
    // The weapon hand opened during a native shot: logged once a wait.
    bool m_release_waiting=false;
    // Measured by DeriveDump only: no contact geometry (about 1 MB a weapon,
    // kept for the session), which no derived value depends on.
    bool m_dump=false;
    // DeriveDump: the profile's final values.
    void LogProfileSummary() const;
    // DeriveDump: the animation's hands and the magazine against the mesh.
    void LogAlignment();


    bool LoadProfile();
    void AutoProfile(IKinematics& model);
    void CacheReloadMagazines(IKinematics& model);
    // What the weapon's own reload animation shows (no profile needed, so
    // modded weapons work): the magazine is the part it takes out and puts
    // back, leaving the well along its insertion path; a part it turns in
    // place (a top cover) is a hinge the hand works, required open while
    // the magazine is out and closed to fire; a chain of rounds hanging from
    // the magazine whose end reaches under that part is a belt. Returns the
    // magazine bone found this way, or BI_NONE.
    u16 DeriveReload(IKinematics& model, u16 trigger);
    void MeasureGeometry(IKinematics& model);
    void DetectShells(IKinematics& model, u16 trigger);
    void BuildCloud(IKinematics& model, xr_vector<Fvector>& cloud) const;
    // Contact geometry for this visual and profile (cached across weapons).
    void BuildContact(IKinematics& model);
    void PollContact();
    // The magazine's way into the well, from the reload animation's last
    // approach to the seat; false when the animation shows none.
    bool SampleInsertPath(IKinematics& model, u16 magazine, vr::InsertPath& path) const;
    void HingePose(Fmatrix& hinge) const;
    // World-space motion of the hinged barrels (world * hinge * world^-1).
    void HingeWorld(const Fmatrix& world, Fmatrix& moved) const;
    // Angle of a point about the hinge axis, + below the bore.
    float HingeAngle(const Fmatrix& world, const Fvector& point) const;
    Fvector ChamberWorld(const Fmatrix& world, int chamber) const;
    // A round (shell or cartridge) in the support hand: its middle in the
    // closed fist, nose where the hand points; false without a tracked hand.
    bool ShellHandPose(Fmatrix& pose) const;
    // The held round's surface, for the fingers to close on, and its middle,
    // in the space of the model it is drawn from (shared per round model).
    struct HeldRound;
    std::shared_ptr<const HeldRound> m_round;
    xr_string m_round_key;
    // Measures the round drawn from `visual` (named `name`): `bone`'s subtree,
    // or all of it without one. Once per round model for the session.
    void MeasureHeldRound(IRenderVisual* visual, const shared_str& name, LPCSTR bone);
    // The middle of the held round in its model's space.
    Fvector RoundCentre() const;
    void SyncChambers();
    void InteractShells(CActor& actor, const Fmatrix& world, bool left_valid,
        const Fmatrix& left, bool mag_hold, bool left_grip, bool left_trigger, bool secondary);
    void EjectShell(int chamber, const Fmatrix& world);
    void DropCase(const Fmatrix& world, const Fvector& velocity);
    int SupportNear(const Fvector& grip, const Fvector& muzzle, const Fvector& forward,
        const Fvector& support) const;
    static float SupportAlong(const Fvector& grip, const Fvector& muzzle, const Fvector& support);
    bool EnsureVisual();
    // The motions sampled for measuring (VrClipSampler): EnsureVisual's for
    // the pass it measures in, else one in `local` for the caller.
    std::unique_ptr<VrClipSampler>* m_clip_scope=nullptr;
    VrClipSampler& Clips(std::unique_ptr<VrClipSampler>& local) const;
    // What measuring a derived weapon's model gives (the profile before its
    // classification, the mesh cloud, optics and part poses), shared by every
    // copy of the same section, HUD model and magazine size for the session:
    // a second AK-74 picked up, or one measured again after a drop or save,
    // copies it instead of sampling its animations again. The classification
    // runs per weapon (fire-mode upgrades, ammunition).
    struct Measured;
    static xr_map<xr_string,std::shared_ptr<const Measured>>& MeasuredCache();
    xr_string MeasuredKey() const;
    void StoreMeasured(const xr_string& key) const;
    bool RestoreMeasured(IKinematics& model, const xr_string& key);
    // The same on disk (appdata\vr_cache), across sessions: a derived
    // weapon's measurements (the Measured triple) and its sampled hands, each
    // keyed by everything they are derived from (DiskKey: its sections, model
    // and motion files, hands, the override file and this build), so a
    // changed mod, outfit or build measures again. Read when the session has
    // none; written once measured (DeriveDump writes them for a whole pack).
    xr_string DiskKey(bool hands) const;
    bool LoadMeasuredFromDisk(const xr_string& memory_key);
    void SaveMeasuredToDisk() const;
    bool LoadHandsFromDisk();
    void SaveHandsToDisk() const;
    bool EnsureMagazineVisual(unsigned rounds);
    xr_string MagazineVisualName(unsigned rounds) const;
    unsigned PreviewRoundCount() const;
    Fmatrix HolsterPose(CActor& actor) const;
    // Its slot holster (VrSlotHolster): the point and how far from it the
    // hand draws or puts the weapon back. False out of the weapon slots.
    bool SlotHolster(CActor& actor, Fvector& point, float& range) const;
    bool ValidateModel(IKinematics& model);
    void Pose(IKinematics& model, xr_vector<Fmatrix>& poses, bool magazine_only, bool rounds,
        PoseCache* storage=nullptr);
    // Poses the weapon's model once per frame (and when the seated magazine changes).
    void EnsurePosed(IKinematics& model);

    void ReleaseModels();
    void ReleaseTransientModels();
    void ReleaseProps();
    void RetireProp(DropProp& prop);
    // Adds a loose prop for 20 s, retiring the oldest beyond 8.
    void PushProp(DropProp prop);
    void ReturnCartridges(const xr_vector<CCartridge>& rounds);
    void MeasureLauncher(IKinematics& model);
    // keep_measured: a save snapshot, the weapon stays as it is (only what
    // the mechanism holds is released).
    void Normalize(bool keep_measured=false);
    // m_mechanism's stroke and hold-open from the profile.
    void ApplyMechanismLimits();
    void FillMagazine(int index);
    bool ReserveAvailable() const;
    // as_drawn: as one from the pouch, even when the held one was taken
    // where the hand closed on it.
    bool MagazineHandPose(int hand, Fmatrix& root, bool as_drawn=false) const;
    // MagazineHandPose's hold point on the magazine body (the middle of its
    // lowest 40% along the well axis), kept per contact geometry and axis.
    mutable const void* m_hold_body_contact=nullptr;
    mutable Fvector m_hold_body_axis{}, m_hold_body{};
    CPhysicsShell* CreateShell(int index, const Fmatrix& pose, int source_hand);
    void DropMagazine(int source_hand);
    void DropInserted(const Fmatrix& world);
    // The released magazine first slides out along the well (index into
    // m_mechanism.magazines, -1 none), drawn on the weapon; clear of it, it
    // becomes a falling prop. Physics never moves it inside the grip, where
    // the weapon's collision box shoved it sideways and it caught.
    int m_ejecting=-1;
    float m_eject_s=0.f, m_eject_speed=0.f;
    Fmatrix m_eject_world=Fidentity;
    IRenderVisual* m_eject_visual=nullptr;
    xr_vector<Fmatrix> m_eject_poses;
    PoseCache m_eject_pose_cache;
    void StepEject();
    // A released magazine still sliding out of the well falls now, from
    // `along` its way out (0 seated, 1 clear).
    void DropEjecting(float along);
    // A magazine out of the well, as a prop at `pose` moving at `velocity`;
    // its rounds leave the inventory.
    void DropFromWell(int index, const Fmatrix& pose, const Fvector& velocity);
    void FinishDrop(int index, CPhysicsShell* shell);
    // Leaves magazines[index] in the world on `shell` (a VrFloorMagazine,
    // its rounds with it), or returns them if it cannot be shown.
    void LeaveOnFloor(int index, CPhysicsShell* shell);
};

void UpdateVrWeapons(CActor& actor);
// Game-thread work that can stall a frame (a model load, a measurement), for
// `object`: logged with its time when it took 100 ms or more, or 20 ms with
// vr_contact_debug. A frame of 100 ms or more is logged too (with
// vr_contact_debug), so a freeze can be put down to what ran in it.
void VrNoteWork(const char* what, const char* object, float ms);
// As VrNoteWork, also saying how many textures the work loaded: pass
// VrTextureBytes() taken before it (0 in VR off).
u32 VrTextureBytes();
void VrNoteWorkLoaded(const char* what, const char* object, float ms, u32 textures_before);
// HUD models prepared before a hand takes their weapon (VrWeaponUpdate.cpp):
// made ahead, their textures loaded on the renderer's worker thread, so the
// grab does not load them (a GAMMA model's took up to 0.65 s, 87 MB).
// VrPrewarmVisual starts it (at most one new model a frame) and keeps it
// wanted; true once its textures are in. VrPrewarmWait, before the model is
// made for the hand: waits for textures still loading. VrPrewarmNear, each
// frame: weapons the pointer names or within reach of a hand.
bool VrPrewarmVisual(const shared_str& visual, LPCSTR object);
void VrPrewarmWait(LPCSTR visual, LPCSTR object);
void VrPrewarmNear(CActor& actor, bool controls, const bool tracked[2], const Fmatrix hands[2]);
void VrPrewarmRelease();
// A HUD model and the motions it names all exist (a missing one is fatal).
bool VrHudModelLoadable(LPCSTR visual);
// The player can use the hands in the world: in gameplay, alive, on foot,
// not remote or talking, in the first-person view, with no menu on top.
bool VrControlsFree(CActor& actor);
// The vr_derive_dump console command (CVrWeaponInteraction::DeriveDump).
void VrDeriveDump(LPCSTR filter);
// Contact solve timings (vr_contact_debug 1): what (0 magazine, 1 a hand's
// fingers) took this long; finger solves also report whether the grip cache
// served them. Logged as averages and maxima every 120 frames.
void VrContactTiming(int what, float microseconds, bool cached=false);
// Throws the bolt a hand holds from the pouch: a bolt leaves the inventory
// at the hand with this velocity (see VrMissileUpdate).
void VrThrowHandBolt(CActor& actor, CInventoryItem& slot_bolt, const Fmatrix& hand, const Fvector& velocity, int side);
// Items held in a hand are put back at the hand after every object has
// updated: their own update copies the physics pose, a step behind, so a
// held item trailed the hand while walking. Once per frame, before drawing.
void VrSnapHeldItems(CActor& actor);
// Physical hands (see UpdateVrWeapons); released with the actor.
void ReleaseVrHandBodies(CActor& actor);
// Magazines left in the world exist for this session only: never saved,
// gone with their rounds when they expire or the game is loaded. A hand
// carries one; it goes back into a weapon that takes it, or its rounds into
// the backpack at the shoulder or the pouch.
void RenderVrFloorMagazines();
void ReleaseVrFloorMagazines();
// The hand carries a floor magazine.
bool VrFloorMagazineHand(int hand);
// Where an empty hand's physics body is (it stops on surfaces).
bool VrHandBodyPosition(CActor& actor, int side, Fvector& position);
// The player's torso: yaw-only, k its forward (vr::TorsoForward, which
// follows the real body turning in the room; the actor's heading without
// tracking), j up, c the actor's position. Belt, holsters, pouch and
// pockets hang from it.
Fmatrix VrTorsoFrame(CActor& actor);
// A tracked hand's real motion between two of its poses in the body's
// frame (vr::HandInBody), in world space: the body's turning (snap or smooth)
// and placement are not in it, what the body carries it with (`carried`,
// the character's physics velocity) is.
void VrHandMotion(const Fmatrix& previous, const Fmatrix& current, const Fmatrix& body,
    const Fvector& carried, float dt, Fvector& linear, Fvector& angular);
// The character's physics velocity (what carries the hands while walking).
Fvector VrCarriedVelocity(CActor& actor);
// Left/right hip point for holsters and the ammunition pouch.
// pouch: the ammunition pouch's place on that hip (forward on the belt when
// the body is shown), not the holster's.
Fvector VrHipPoint(CActor& actor, bool right, bool pouch=false);
// A slot weapon's holster on one side of the body (a handgun on that hip, a
// long gun over that shoulder) and its reach; false for other slots.
bool VrSlotHolster(CActor& actor, CWeapon& weapon, u16 slot, bool right, Fvector& point, float& range);
// The shoulder a slot's long gun hangs on (true: the right): where it was put
// away by hand, else its slot's (the second long gun on the off-hand
// shoulder, otherwise the hand's side), and never the other long gun's.
// `right`: the hand holding it is the right.
bool VrSlotShoulderRight(CActor& actor, CWeapon& weapon, u16 slot, bool right);
// The weapon slot (INV_SLOT_2 or INV_SLOT_3) a slot weapon taken by hand
// goes into: the one it is in, its own when free, else the other when free
// (either takes a long gun, as the inventory lets it); NO_ACTIVE_SLOT when
// both hold other weapons. Taking one never pushes a holstered weapon into
// the backpack while a holster is free.
u16 VrFreeWeaponSlot(CActor& actor, CWeapon& weapon);
// A pistol-class weapon held in one hand (its class and kind); everything
// else is a long gun.
bool VrHandgun(CWeapon& weapon);
// The ammunition pouch: its reach is its ring's radius.
constexpr float vr_pouch_reach=0.11f;
// The pouch, on the hip of the hand that supports the weapon.
Fvector VrPouchPoint(CActor& actor);
// Left/right shoulder: long guns are stowed and picked-up items stored there.
Fvector VrShoulderPoint(CActor& actor, bool right);
// Over either shoulder: what a hand brings there goes into the backpack.
bool VrAtBackpack(CActor& actor, const Fvector& point);
// How far a hand is from a holster (or the backpack) as it reaches it: a
// shoulder's is reached over and behind it, so distance ahead of its point
// counts twice; a hip's is a sphere. Compare with the holster's range.
float VrHolsterDistance(CActor& actor, const Fvector& holster, const Fvector& hand);
// A point a hand put through a wall (a muzzle, a carried or thrown item) is
// brought back to the head's side of the static world, `margin` metres
// short of it. False when nothing was in the way. Used only with physical
// weapon (the muzzle) or physical hands (carried and thrown items) on.
bool VrNearSide(Fvector& point, float margin);
void RenderVrPouch(CActor& actor);
// Release the right-hand weapon: holster it at its holster, otherwise throw
// it with the tracked hand velocity.
// holster: 1 back to its holster, 0 out of the hand, -1 by where the hand is
// now. settled: the weapon is at rest (put away at once), else it is put away
// through its own hide (interrupting a shot's animation cleanly).
void VrReleaseHeldWeapon(CActor& actor, CWeapon& weapon, int holster=-1, bool settled=true);
void PrepareVrWeaponSave(CActor& actor);
