#pragma once
// What a weapon is, mechanically, for VR handling: its family, how its action
// is worked, how it is fed, and which controls it really has. Derived from
// the engine class, the weapon's ltx, its first-person animations and its
// model (VrWeaponMechanicsDerive), never from its rate of fire; one optional
// ltx file can override any property per weapon section. Engine-free, so the
// rules are unit tested.
#include <cstring>
#include <cstdlib>
#include <type_traits>

namespace vr
{
enum class WeaponFamily { Unknown, Pistol, Revolver, BoltAction, LeverAction, PumpShotgun,
    BreakAction, SemiShotgun, Rifle, Smg, MachineGun, Launcher };
enum class ActionKind { None, SelfLoading, ManualBolt, Lever, Pump, Break, Revolver, Launcher };
enum class FeedKind { None, Detachable, InternalRounds, Tube, Belt, Cylinder, Chambers, Muzzle };

struct WeaponMechanics
{
    WeaponFamily family = WeaponFamily::Unknown;
    ActionKind action = ActionKind::None;
    FeedKind feed = FeedKind::None;
    unsigned capacity = 0;
    // Releases that physically exist: an AR-15's bolt catch, a separate bolt
    // release, a pistol's slide stop lever.
    bool has_bolt_catch = false, has_bolt_release = false;
    bool has_slide_stop = false, has_slide_release = false;
    // A locked action is only released by working it (an AK).
    bool requires_manual_charging = false;
    bool supports_last_round_hold_open = false;
    bool has_charging_handle = false, has_dust_cover = false;
    // An HK roller-delayed weapon (MP5, G3): the charging handle hooks into
    // a notch to hold the bolt back, and a slap down releases it.
    bool handle_notch = false;
    bool magazine_removable = false;
    // False: the game's own reload (no VR reload is offered).
    bool vr_reload_supported = false;
};

// What derivation measured on the weapon.
struct MechanicsEvidence
{
    // Engine class (it differs between mods for the same gun: a hint only).
    bool pistol_class = false, revolver_class = false, shotgun_class = false;
    bool bm16_class = false, launcher_class = false;
    // ltx: tri_state_reload with the open/add/close motions; an automatic
    // fire mode; weapon_class smg.
    bool tri_state = false, automatic_mode = false, smg_class = false;
    // anm_shot_l exists, and leaves the bolt or slide back on its last frame.
    bool has_shot_l = false, shot_l_holds_back = false;
    // Model and animations.
    bool detachable_magazine = false, belt = false, pump_part = false, break_hinge = false;
    bool cylinder_part = false, lever_part = false, cycling_part = false;
    // The reload brings a stripper clip or rounds in from above: an
    // internal magazine loaded through the open action (SKS, Mosin).
    bool top_loaded = false;
    bool separate_handle = false, dust_cover_part = false;
    // The largest turn about the bore of the cycling part or its handle over
    // the shot animation (a bolt-action's handle lifts ~90 degrees).
    float bolt_turn_deg = 0.f;
    unsigned capacity = 0, barrels = 0;
};

constexpr float bolt_action_turn_deg = 60.f;

inline WeaponMechanics Classify(const MechanicsEvidence& e)
{
    WeaponMechanics m;
    m.capacity = e.capacity;
    const bool manual_bolt = e.cycling_part && e.bolt_turn_deg >= bolt_action_turn_deg;
    if (e.launcher_class)
    {
        m.family = WeaponFamily::Launcher; m.action = ActionKind::Launcher;
        m.feed = e.break_hinge ? FeedKind::Chambers : FeedKind::Muzzle;
    }
    else if (e.break_hinge || e.bm16_class)
    {
        m.family = WeaponFamily::BreakAction; m.action = ActionKind::Break; m.feed = FeedKind::Chambers;
        if (e.barrels) m.capacity = e.barrels;
    }
    else if (e.revolver_class || e.cylinder_part)
    {
        m.family = WeaponFamily::Revolver; m.action = ActionKind::Revolver; m.feed = FeedKind::Cylinder;
    }
    else if (e.pump_part)
    {
        m.family = WeaponFamily::PumpShotgun; m.action = ActionKind::Pump;
        m.feed = e.detachable_magazine ? FeedKind::Detachable : FeedKind::Tube;
    }
    else if (manual_bolt)
    {
        m.family = WeaponFamily::BoltAction; m.action = ActionKind::ManualBolt;
        m.feed = e.detachable_magazine ? FeedKind::Detachable : FeedKind::InternalRounds;
    }
    else if (e.lever_part)
    {
        m.family = WeaponFamily::LeverAction; m.action = ActionKind::Lever; m.feed = FeedKind::Tube;
    }
    else if (e.shotgun_class && !e.tri_state && !e.detachable_magazine && e.cycling_part)
    {
        m.family = WeaponFamily::Rifle; m.action = ActionKind::SelfLoading; m.feed = FeedKind::InternalRounds;
    }
    else if (e.shotgun_class)
    {
        m.family = WeaponFamily::SemiShotgun; m.action = ActionKind::SelfLoading;
        m.feed = e.detachable_magazine ? FeedKind::Detachable : e.tri_state ? FeedKind::Tube : FeedKind::None;
    }
    else if (e.belt)
    {
        m.family = WeaponFamily::MachineGun; m.action = ActionKind::SelfLoading; m.feed = FeedKind::Belt;
    }
    else if (e.pistol_class)
    {
        m.family = WeaponFamily::Pistol; m.action = ActionKind::SelfLoading;
        m.feed = e.detachable_magazine ? FeedKind::Detachable : FeedKind::None;
    }
    else
    {
        m.family = e.smg_class ? WeaponFamily::Smg : WeaponFamily::Rifle;
        m.action = e.cycling_part || e.detachable_magazine ? ActionKind::SelfLoading : ActionKind::None;
        m.feed = e.detachable_magazine ? FeedKind::Detachable :
            (e.tri_state || e.top_loaded) ? FeedKind::InternalRounds : FeedKind::None;
    }
    m.magazine_removable = m.feed == FeedKind::Detachable || m.feed == FeedKind::Belt;
    const bool self_loading = m.action == ActionKind::SelfLoading;
    m.has_charging_handle = self_loading && m.family != WeaponFamily::Pistol && e.cycling_part;
    // A pistol without a last-shot motion still locks back, as nearly all
    // do; a self-loader with an internal magazine (SKS) too: its follower
    // holds the bolt open on the empty magazine for loading.
    m.supports_last_round_hold_open = self_loading &&
        (e.shot_l_holds_back || (m.family == WeaponFamily::Pistol && !e.has_shot_l) ||
         m.feed == FeedKind::InternalRounds);
    m.has_slide_stop = m.family == WeaponFamily::Pistol && m.supports_last_round_hold_open;
    m.has_bolt_catch = m.family != WeaponFamily::Pistol && m.supports_last_round_hold_open &&
        m.feed != FeedKind::InternalRounds;
    m.requires_manual_charging = self_loading && !m.has_bolt_catch && !m.has_slide_stop;
    m.has_dust_cover = e.dust_cover_part;
    m.vr_reload_supported = m.feed != FeedKind::None;
    return m;
}

inline const char* FamilyName(WeaponFamily f)
{
    static const char* names[] = {"unknown", "pistol", "revolver", "bolt_action", "lever_action",
        "pump_shotgun", "break_action", "semi_shotgun", "rifle", "smg", "machine_gun", "launcher"};
    return names[int(f)];
}
inline const char* ActionName(ActionKind a)
{
    static const char* names[] = {"none", "self_loading", "manual_bolt", "lever", "pump", "break",
        "revolver", "launcher"};
    return names[int(a)];
}
inline const char* FeedName(FeedKind f)
{
    static const char* names[] = {"none", "detachable", "internal_rounds", "tube", "belt", "cylinder",
        "chambers", "muzzle"};
    return names[int(f)];
}

// The override section names to try for a weapon section, most specific
// first: the section, then it trimmed of one "_suffix" at a time down to
// "wpn_x" (wpn_ak74u_m1_isg: itself, wpn_ak74u_m1, wpn_ak74u). Writes up to
// `capacity` names of up to 64 chars; returns how many.
inline int OverrideNames(const char* section, char (*names)[64], int capacity)
{
    int count = 0;
    char name[64];
    std::strncpy(name, section, 63); name[63] = 0;
    while (count < capacity)
    {
        std::strcpy(names[count++], name);
        char* cut = std::strrchr(name, '_');
        // Never down to the bare "wpn" prefix.
        if (!cut || cut == name || !std::strchr(name, '_') || std::strchr(name, '_') == cut) break;
        *cut = 0;
    }
    return count;
}
// One override (key = value) from the VR mechanics ltx. False for an unknown
// key or value (the caller logs it).
inline bool ApplyMechanicsOverride(WeaponMechanics& m, const char* key, const char* value)
{
    const auto is = [&](const char* k) { return !std::strcmp(key, k); };
    const auto flag = [&](bool& out)
    {
        out = !std::strcmp(value, "true") || !std::strcmp(value, "on") || !std::strcmp(value, "1") ||
            !std::strcmp(value, "yes");
        return true;
    };
    const auto pick = [&](auto& out, auto name_of, int count)
    {
        using Kind = std::remove_reference_t<decltype(out)>;
        for (int i = 0; i < count; ++i)
            if (!std::strcmp(value, name_of(Kind(i)))) { out = Kind(i); return true; }
        return false;
    };
    if (is("family")) return pick(m.family, FamilyName, 12);
    if (is("action")) return pick(m.action, ActionName, 8);
    if (is("feed")) return pick(m.feed, FeedName, 8);
    if (is("capacity")) { m.capacity = unsigned(std::atoi(value)); return true; }
    if (is("has_bolt_catch")) return flag(m.has_bolt_catch);
    if (is("has_bolt_release")) return flag(m.has_bolt_release);
    if (is("has_slide_stop")) return flag(m.has_slide_stop);
    if (is("has_slide_release")) return flag(m.has_slide_release);
    if (is("requires_manual_charging")) return flag(m.requires_manual_charging);
    if (is("supports_last_round_hold_open")) return flag(m.supports_last_round_hold_open);
    if (is("has_charging_handle")) return flag(m.has_charging_handle);
    if (is("has_dust_cover")) return flag(m.has_dust_cover);
    if (is("handle_notch")) return flag(m.handle_notch);
    if (is("magazine_removable")) return flag(m.magazine_removable);
    if (is("vr_reload_supported")) return flag(m.vr_reload_supported);
    return false;
}
}
