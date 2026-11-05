#pragma once

#include "VrPistolState.h"
#include "VrReloadRules.h"
#include "VrFingerContact.h"
#include "VrWeaponMechanics.h"
#include "VrActionPath.h"
class IKinematicsAnimated;
#include <memory>
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
// Geometry for physical contact (VrContactField.h), shared by every weapon
// using one HUD visual and profile: the receiver with its magazine well
// opened, the cycling part (slide, bolt or charging handle) on its own, the
// magazine as a body and as a surface, and its way into the well.
struct VrWeaponContact
{
    vr::ContactField receiver, cycle, magazine_surface;
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
    void SetWeaponHand(int hand) { m_hand=hand ? 1 : 0; m_carried=false; m_hand_assigned=Device.dwTimeGlobal; }
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
        shared_str magazine_visual_prefix;
        unsigned magazine_visual_max_rounds=0;
        shared_str mag_extra_bones[2];
        Fvector grip_offset{}, support_offset{}, mag_grip{}, mag_base{}, cycle_grab{};
        Fvector muzzle{}, shell_point{};
    Fvector launcher_grab{};
        Fvector mag_box_half{}, mag_box_center{};
        Fvector expected_mag_bind{}, expected_cycle_bind{};
        float bind_tolerance=0.005f;
        bool check_mag_bind=false, check_cycle_bind=false;
        float cycle_travel=0, extraction=0, grab_radius=0.08f, guide_radius=0.015f;
        float hand_grip_angle=18.f, hand_grip_radius=0.015f;
        Fvector hand_grip_center{}, trigger_point{};
        bool has_hand_grip_center=false, has_trigger_point=false;
        // Profile generated from the HUD model's own bones and meshes:
        // automatic until measured, derived once AutoProfile has run (an
        // audited profile from vr_weapons.ltx is neither).
        bool automatic=false, derived=false;
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
        xr_vector<CCartridge> cartridges; // back to reserve when it goes
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
    bool m_supported=false, m_holstered=false, m_filling=false;
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
    // Whether opening the weapon hand now lets go of the weapon (see
    // ReleaseWanted); updated every frame the weapon is in the hand.
    bool ReleaseWanted(CActor& actor, bool opened, bool open);
    // Held with the grip relaxed: a pistol, in the hand that holds it.
    bool KeptWithoutGrip() const;
    // A pistol-class weapon (one hand); everything else is a long gun.
    bool Handgun() const;
    // The hand has been clear of the holster since it took the weapon, and
    // when it came to rest open at the holster.
    bool m_holster_armed=false;
    u32 m_holster_since=0;
    // The rounds the pouch gives (-1: the weapon's own type) and when the
    // player last changed them.
    u8 m_pouch_ammo=u8(-1);
    u32 m_pouch_swap_time=0;
    bool m_restore_empty_chamber=false;
    float m_cycle_visual_slide=0.f;
    bool m_trigger=false, m_grip[2]{}, m_button=false, m_secondary=false;
    bool m_mag_trigger=false;
    bool m_racking=false, m_racked=false, m_inserting=false;
    bool m_pouch_preview=false, m_supporting=false;
    float m_rack_start=0;
    // Where the hand was across the bore when it took the handle (an HK's
    // is lifted into its notch), and the hand's last place for a slap.
    float m_rack_lift_start=0.f;
    Fvector m_slap_previous{};
    bool m_slap_previous_valid=false;
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
    // Classifies the weapon from the profile's evidence, applies the
    // overrides file and sets the profile's mechanism from the result.
    void ClassifyMechanics(IKinematics& model);
    // The first-person animation holds the support hand behind the weapon
    // hand (an RPG-7's rear grip): it supports from behind.
    bool RearSupport() const;
    // Records a manual action's path from a motion (knob: the bone whose
    // point grab the hand holds; receiver: the frame it is measured in).
    void RecordActionPath(IKinematicsAnimated& animated, IKinematics& sample, IKinematics& model,
        const xr_vector<Fmatrix>& bind, LPCSTR key, u16 knob, const Fvector& grab, u16 receiver, u16 moving_bolt, bool knob_rear);
    // Poses the manual action's bones at m_action_u and the dust cover.
    bool PoseActionPath(IKinematics& model, xr_vector<Fmatrix>& poses) const;
    // A magazine is held where the hand closed on it (m up the well axis
    // from its base), not moved so its base is in the fist: taken out of
    // the weapon by its body it stays where it was in the hand.
    bool m_mag_grabbed=false;
    float m_mag_grab_along=0.f;
    // A magazine taken out by hand stays exactly where the hand closed on
    // it: its pose relative to the hand's grip at that moment.
    Fmatrix m_mag_in_hand=Fidentity;
    // Where along the seated magazine a hand at this point would close on
    // it (m up the well axis from its base); false when it is not at the
    // magazine's exposed body or floorplate.
    bool MagazineGrab(const Fmatrix& world, const Fvector& hand, float& along) const;
    // Travel at the last friction tick of a magazine sliding in or out.
    float m_slide_tick=0.f;
    unsigned m_hand_epoch=0;
    bool m_have_hand[2]{};
    Fmatrix m_previous_hand[2];
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
    bool m_slide_lock_logged=false; // last slide lock state written to the log
    bool m_shell_held=false;
    IRenderVisual* m_shell_visual=nullptr;
    xr_vector<Fmatrix> m_shell_poses;
    IRenderVisual* m_pouch_visual=nullptr;
    shared_str m_pouch_section;
    // Pump stroke: travel (m), whether a live shell is chambered, and
    // whether a fired case still waits to be ejected.
    bool m_pumping=false, m_pump_back=false, m_pump_ready=true, m_pump_spent=false;
    float m_pump=0.f, m_pump_start=0.f;
    unsigned m_shell_rounds=0;
    // Weapon mesh point clouds (5 mm voxels) for finger contact.
    xr_vector<Fvector> m_cloud;
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
    // Where the reload animation's left hand is when it has the bolt or
    // charging handle furthest back (weapon space): on the handle.
    mutable Fvector m_handle_hand{};
    mutable bool m_handle_hand_valid=false;
    bool m_cycle_use_hand=false;
    // The animated palm was checked against the measured grip once.
    mutable bool m_palm_checked=false, m_palm_trusted=true;
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
        SoundPumpForward, SoundBreakOpen, SoundBreakClose, SoundShellIn,
        SoundCoverOpen, SoundCoverClose, SoundBeltIn, SoundCount };
    ref_sound m_handling[SoundCount];
    bool m_handling_loaded[SoundCount]{};
    void PlayHandling(int sound, const Fvector& position);
    // Parts posed by animation or state (see AnimateParts).
    void PrepareParts(IKinematics& model);
    bool AnimateParts(IKinematics& model, xr_vector<Fmatrix>& poses);
    // Hides on the VR copy what the live first-person model hides (fired
    // rockets, grenades, rounds taken by a reload); true if anything changed.
    bool CopyNativeVisibility(IKinematics& model) const;
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
    // Moving parts (see Profile::parts): their state this session, never
    // saved; the belt; the magazine slot the belt was laid for.
    vr::PartState m_parts[vr::max_parts];
    vr::BeltState m_belt;
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

    bool LoadProfile();
    void AutoProfile(IKinematics& model);
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
    bool ShellHandPose(Fmatrix& pose) const;
    void SyncChambers();
    void InteractShells(CActor& actor, const Fmatrix& world, bool left_valid,
        const Fmatrix& left, bool mag_hold, bool left_grip, bool left_trigger, bool secondary);
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
    void ReleaseProps(bool give_back=true);
    void RetireProp(DropProp& prop, bool give_back=true);
    void ReturnCartridges(const xr_vector<CCartridge>& rounds);
    void MeasureLauncher(IKinematics& model);
    void Normalize();
    void FillMagazine(int index);
    bool ReserveAvailable() const;
    Fvector Hip(CActor& actor, bool right) const;
    Fvector Shoulder(CActor& actor) const;
    Fvector HolsterPoint(CActor& actor) const;
    bool MagazineHandPose(int hand, Fmatrix& root) const;
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
    void StepEject();
    // A magazine out of the well, as a prop at `pose` moving at `velocity`;
    // its rounds leave the inventory.
    void DropFromWell(int index, const Fmatrix& pose, const Fvector& velocity);
    void FinishDrop(int index, CPhysicsShell* shell);
};

void UpdateVrWeapons(CActor& actor);
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
Fvector VrHipPoint(CActor& actor, bool right);
// The ammunition pouch: its reach is its ring's radius.
constexpr float vr_pouch_reach=0.11f;
// The pouch, on the hip of the hand that supports the weapon.
Fvector VrPouchPoint(CActor& actor);
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
