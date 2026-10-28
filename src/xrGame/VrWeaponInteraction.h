#pragma once

#include "VrPistolState.h"
#include "weaponammo.h"

class CWeapon;
class CActor;

// A hand pose in world space: anatomical frame (wrist to knuckles, little
// finger to index, and their cross product), the midpoint of the index and
// middle knuckles, and the direction of each joint's segment for the thumb,
// index and remaining fingers (three joints each).
struct VrHandBones
{
    Fmatrix hand;
    const xr_vector<Fmatrix>* locals=nullptr;
    shared_str visual;
};

struct VrHandPose
{
    Fvector basis[3];
    Fvector knuckles;
    Fvector finger[3][3];
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

    bool Supported() const { return m_supported; }
    bool Mode() const;
    bool Transform(Fmatrix& world) const;
    // A shot kicks the weapon up, a little aside and back in the hands; it
    // springs back over about a quarter of a second (vr_recoil).
    void OnShot();
    // Tracked placement of an unprofiled weapon's world model in the hand.
    bool GenericTransform(Fmatrix& world) const;
    bool AimTransform(Fmatrix& world) const { return Transform(world) || GenericTransform(world); }
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
    bool ShellLoading() const { return m_profile.break_action || m_profile.tube; }
    // Weapon mesh points near which fingers rest, with the weapon's world
    // transform and the bore height the thumb must stay below (model space).
    bool GripCloud(const xr_vector<Fvector>*& points, Fmatrix& world, float& ceiling);
    bool HolsterPointFor(CActor& actor, Fvector& point, float& range) const;
    // Grip frame the support hand should follow instead of its controller.
    bool SupportHandOverride(Fmatrix& grip) const;
    // Hands are symmetric: the weapon is held by whichever hand drew it and
    // the other hand supports. Role 1 is the weapon hand, role 0 support.
    int WeaponHand() const { return m_hand; }
    void SetWeaponHand(int hand) { m_hand=hand ? 1 : 0; m_carried=false; m_hand_assigned=Device.dwTimeGlobal; }
    // Anything left in the pouch for this weapon.
    bool PouchHasAmmo() const;
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
    bool HolsterGrabDistance(CActor& actor, const Fvector& hand, float& distance) const;
    void ReleaseInactiveHeld();
    bool LeftHandBusy() const;
    float DetachedRoundWeight() const;
    void PrepareSave();
    void PrepareSnapshot();
    bool CanFire() const;
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

private:
    struct Profile
    {
        shared_str section, visual, mag_bone, cycle_bone, rounds_bone;
        shared_str magazine_visual_prefix;
        unsigned magazine_visual_max_rounds=0;
        shared_str mag_extra_bones[2];
        Fvector grip_offset{}, support_offset{}, mag_grip{}, mag_base{}, cycle_grab{};
        Fvector muzzle{}, shell_point{};
        Fvector mag_box_half{}, mag_box_center{};
        Fvector expected_mag_bind{}, expected_cycle_bind{};
        float bind_tolerance=0.005f;
        bool check_mag_bind=false, check_cycle_bind=false;
        float cycle_travel=0, extraction=0, grab_radius=0.08f, guide_radius=0.015f;
        float hand_grip_angle=18.f, hand_grip_radius=0.015f;
        Fvector hand_grip_center{}, trigger_point{};
        bool has_hand_grip_center=false, has_trigger_point=false;
        // Profile generated from the HUD model's own bones and meshes.
        bool automatic=false;
        // Break-action (hinged barrels) loaded one shell at a time.
        bool break_action=false;
        shared_str hinge_bone, shell_bones[2];
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
        // Bore height in model space; the thumb stays below it.
        float bore_height=0.f;
        bool two_handed=false, detachable=false, chamber=false, slide_stop=false;
    } m_profile;
    struct DropProp
    {
        CPhysicsShell* shell=nullptr;
        IRenderVisual* visual=nullptr;
        xr_vector<Fmatrix> poses;
        u32 expires=0;
        bool rounds=false;
        unsigned round_count=0;
        bool proxy=false;
        shared_str part; // only this bone (and children) is shown
    };
    CWeapon& m_weapon;
    int m_hand=1;
    // When a hand last took this weapon (holster grab, pickup); a weapon
    // drawn any other way goes to the dominant hand.
    u32 m_hand_assigned=0;
    bool m_support_cup=false;
    float m_support_along=0.f; // fore-end hold point along the barrel line
    bool m_seating=false; // a committed magazine finishing its insertion
    bool m_supported=false, m_holstered=false, m_filling=false;
    // Recoil still to return (radians up and aside, metres back), as of
    // m_recoil_time; it decays as it is read.
    mutable float m_recoil_pitch=0.f, m_recoil_yaw=0.f, m_recoil_back=0.f;
    mutable u32 m_recoil_time=0;
    void DecayRecoil() const;
    bool m_reported_generic=false;
    bool m_basic_magazine=false;
    unsigned m_basic_reload_rounds=0;
    bool m_release_pending=false;
    bool m_restore_empty_chamber=false;
    float m_cycle_visual_slide=0.f;
    bool m_trigger=false, m_grip[2]{}, m_button=false, m_secondary=false;
    bool m_mag_trigger=false;
    bool m_racking=false, m_racked=false, m_extracting=false, m_auto_eject=false, m_inserting=false;
    bool m_catch_released=false, m_pouch_preview=false, m_supporting=false;
    float m_rack_start=0, m_extract_start=0, m_extract_progress=0;
    unsigned m_hand_epoch=0;
    bool m_have_hand[2]{};
    Fmatrix m_previous_hand[2], m_guide_pose;
    Fvector m_hand_velocity[2]{}, m_hand_angular[2]{};
    vr::PistolInputGate m_input;
    vr::MagazineMechanism<xr_vector<CCartridge>> m_mechanism;
    IRenderVisual* m_visual=nullptr;
    IRenderVisual* m_mag_visual=nullptr;
    unsigned m_mag_visual_count=unsigned(-1);
    xr_vector<Fmatrix> m_poses[2];
    u32 m_posed_frame=u32(-1);
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
    bool m_shell_held=false;
    IRenderVisual* m_shell_visual=nullptr;
    xr_vector<Fmatrix> m_shell_poses;
    IRenderVisual* m_pouch_visual=nullptr;
    shared_str m_pouch_section;
    // Depth of a guided magazine below its seat, along the well axis.
    float m_guide_travel=0.f;
    // Pump stroke: travel (m), whether a live shell is chambered, and
    // whether a fired case still waits to be ejected.
    bool m_pumping=false, m_pump_back=false, m_pump_ready=true, m_pump_spent=false;
    float m_pump=0.f, m_pump_start=0.f;
    unsigned m_shell_rounds=0;
    // Weapon mesh point clouds (5 mm voxels) for finger contact.
    xr_vector<Fvector> m_cloud, m_world_cloud;
    const IRenderVisual* m_world_cloud_visual=nullptr;
    // Both hands of the first-person idle animation in weapon model space
    // (0 left, 1 right), sampled once per first-person hands model.
    struct AuthoredHandRef
    {
        Fvector basis[3]{}, knuckles{}, palm{}, finger[3][3]{};
        bool valid=false;
    };
    mutable AuthoredHandRef m_authored[2];
    mutable shared_str m_authored_hands;
    mutable bool m_authored_sampled=false;
    mutable bool m_authored_scope=false;
    void SampleAuthoredHands() const;
    // Bone poses of the first-person hands (see AuthoredBones), sampled from
    // the idle (hands on the weapon) and the reload (left hand holding the
    // magazine just below the well).
    mutable xr_vector<Fmatrix> m_hands_local, m_maghand_local;
    mutable Fmatrix m_hand_in_item[2], m_maghand_in_root;
    mutable Fvector m_maghand_basis[3]{}, m_maghand_palm{};
    mutable shared_str m_hands_visual_name;
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
    mutable SightInfo m_sight;
    void MeasureSight() const;
    bool EyeOnSight(const Fvector& eye, Fmatrix& world, Fvector& direction, Fvector& window,
        Fvector& offset, float& behind) const;
    // The model's own sight glass meshes (Anomaly's LFO lens, reticle and
    // dot meshes, 3D scope lenses; see MeasureOptics), whether one of them
    // is a red dot or a scope's soft black (exit pupil) lens, and ordinary
    // meshes with a lens texture (an integrated scope's glass, FN2000).
    xr_vector<Fbox> m_glass, m_lens_meshes;
    bool m_glass_dot=false, m_glass_soft=false;
    // Scope parts' mesh (left out of m_cloud) and the model's shader names,
    // for finding the eyepiece and for the log.
    xr_vector<Fvector> m_optic_cloud;
    shared_str m_mesh_shaders;
    mutable u32 m_scope_refused_time=0;
    void MeasureOptics(IKinematics& model);
    Fvector GripLocal() const;
    // A weapon handed to the support hand (see HandOver), in that hand's frame.
    bool m_carried=false;
    Fmatrix m_carry;
    bool HandOver();
    void PullTrigger();
    u32 m_fire_queued=0;
    void TakeBack();
    // Short handling sounds (see PlayHandling).
    enum { SoundMagOut, SoundMagIn, SoundSlideBack, SoundSlideForward, SoundPumpBack,
        SoundPumpForward, SoundBreakOpen, SoundBreakClose, SoundShellIn, SoundCount };
    ref_sound m_handling[SoundCount];
    bool m_handling_loaded[SoundCount]{};
    void PlayHandling(int sound, const Fvector& position);
    // Parts posed by animation or state (see AnimateParts).
    void PrepareParts(IKinematics& model);
    bool AnimateParts(IKinematics& model, xr_vector<Fmatrix>& poses);
    void MeasureParts(IKinematics& model);
    xr_vector<Fmatrix> m_bind_local, m_state_local[3];
    xr_vector<u8> m_state_part;
    u16 m_trigger_bone[2]{BI_NONE,BI_NONE};
    bool m_trigger_valid[2]{};
    Fmatrix m_trigger_pull[2];
    float m_trigger_value=0.f;   // weapon hand's trigger travel, 0..1
    unsigned m_break_state=2;    // loaded barrels the gun shows (anm_idle_N)
    bool m_break_state_set=false;
    xr_vector<u16> m_overlay_order;
    xr_vector<u8> m_overlay_receiver, m_overlay_owned;
    // The cycling part (bolt, slide or charging handle) found from the
    // weapon's first-person shot or reload animation.
    void MeasureCycle(IKinematics& model);

    bool LoadProfile();
    void AutoProfile(IKinematics& model);
    void MeasureGeometry(IKinematics& model);
    void DetectShells(IKinematics& model, u16 trigger);
    void BuildCloud(IKinematics& model, xr_vector<Fvector>& cloud) const;
    bool GuidePose(const Fmatrix& world, Fmatrix& pose) const;
    float WellDepth(const Fmatrix& world, const Fvector& root, float& lateral) const;
    void HingePose(Fmatrix& hinge) const;
    // World-space motion of the hinged barrels (world * hinge * world^-1).
    void HingeWorld(const Fmatrix& world, Fmatrix& moved) const;
    // Angle of a point about the hinge axis, + below the bore.
    float HingeAngle(const Fmatrix& world, const Fvector& point) const;
    Fvector ChamberWorld(const Fmatrix& world, int chamber) const;
    bool ShellHandPose(Fmatrix& pose) const;
    void SyncChambers();
    void InteractShells(CActor& actor, const Fmatrix& world, bool left_valid,
        const Fmatrix& left, bool mag_hold, bool left_grip, bool secondary);
    void EjectShell(int chamber, const Fmatrix& world);
    int SupportNear(const Fvector& grip, const Fvector& muzzle, const Fvector& forward,
        const Fvector& support) const;
    static float SupportAlong(const Fvector& grip, const Fvector& muzzle, const Fvector& support);
    bool EnsureVisual();
    bool EnsureMagazineVisual(unsigned rounds);
    xr_string MagazineVisualName(unsigned rounds) const;
    unsigned PreviewRoundCount() const;
    bool HolsterRight() const;
    Fmatrix HolsterPose(CActor& actor) const;
    bool ValidateModel(IKinematics& model);
    void Pose(IKinematics& model, xr_vector<Fmatrix>& poses, bool magazine_only, bool rounds);
    void ReleaseModels();
    void ReleaseProps();
    void Normalize();
    void ReturnCartridge(const CCartridge& cartridge);
    void FillMagazine(int index);
    bool ReserveAvailable() const;
    Fvector Hip(CActor& actor, bool right) const;
    Fvector Shoulder(CActor& actor) const;
    Fvector HolsterPoint(CActor& actor) const;
    bool MagazineHandPose(int hand, Fmatrix& root) const;
    CPhysicsShell* CreateShell(int index, const Fmatrix& pose, int source_hand);
    void DropMagazine(int source_hand);
    void DropInserted(const Fmatrix& world);
    void FinishDrop(int index, CPhysicsShell* shell);
};

void UpdateVrWeapons(CActor& actor);
// Throws the bolt a hand holds from the pouch: a bolt leaves the inventory
// at the hand with this velocity (see VrMissileUpdate).
void VrThrowHandBolt(CActor& actor, CInventoryItem& slot_bolt, const Fmatrix& hand, const Fvector& velocity, int side);
// Items held in a hand are put back at the hand after every object has
// updated: their own update copies the physics pose, a step behind, so a
// held item trailed the hand while walking. Once per frame, before drawing.
void VrSnapHeldItems(CActor& actor);
// Physical hands (see UpdateVrWeapons); released with the actor.
void ReleaseVrHandBodies(CActor& actor);
// Where an empty hand's physics body is (it stops on surfaces).
bool VrHandBodyPosition(CActor& actor, int side, Fvector& position);
// The player's torso: yaw-only, k its forward (vr::TorsoForward, which
// follows the real body turning in the room; the actor's heading without
// tracking), j up, c the actor's position. Belt, holsters, pouch and
// pockets hang from it.
Fmatrix VrTorsoFrame(CActor& actor);
// Left/right hip point for holsters and the ammunition pouch.
Fvector VrHipPoint(CActor& actor, bool right);
// Left/right shoulder: long guns are stowed and picked-up items stored there.
Fvector VrShoulderPoint(CActor& actor, bool right);
// A point a hand put through a wall (a muzzle, a carried or thrown item) is
// brought back to the head's side of the static world, `margin` metres
// short of it. False when nothing was in the way. Used only with physical
// weapon (the muzzle) or physical hands (carried and thrown items) on.
bool VrNearSide(Fvector& point, float margin);
void RenderVrPouch(CActor& actor);
// Release the right-hand weapon: holster it at its holster, otherwise throw
// it with the tracked hand velocity.
void VrReleaseHeldWeapon(CActor& actor, CWeapon& weapon);
void PrepareVrWeaponSave(CActor& actor);
