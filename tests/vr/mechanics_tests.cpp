#include "../../src/xrGame/VrWeaponMechanics.h"
#include "../../src/xrGame/VrActionPath.h"
#include "../../src/xrGame/VrWeaponNames.h"
#include "ak12_fixture.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "check.h"
using namespace vr;

static MechanicsEvidence Rifle(bool magazine=true)
{
    MechanicsEvidence e; e.cycling_part=true; e.detachable_magazine=magazine; e.capacity=10; return e;
}

// Model and section names (VrWeaponNames.h): case-free parts, whole words,
// calibres and clip lengths.
static void Names()
{
    CHECK(NameHas("WPN_Scope_Cap","scope") && !NameHas("wpn_body","scope") && !NameHas(nullptr,"scope"));
    // The part is matched as given: lower case.
    CHECK(!NameHas("wpn_scope","Scope"));
    CHECK(NameWord("wpn_scope_cap1","cap") && NameWord("scope_caps","cap") && NameWord("SCOPE_CAP","cap"));
    CHECK(!NameWord("wpn_scope_capsule","cap") && !NameWord("handcap","cap") && !NameWord(nullptr,"cap"));
    CHECK(ShellName("wpn_gilza") && ShellName("Bullet_01") && !ShellName("wpn_mag"));
    CHECK(AttachmentName("wpn_silencer") && AttachmentName("grenade_launcher") && !AttachmentName("wpn_body"));
    CHECK(RoundName("vog25") && RoundName("cartridge") && RoundName("40mm_he") && !RoundName("wpn_bolt"));
    // Longer, more specific calibres first; unknown is 5.45x39.
    CHECK(Near(CartridgeEnergy("ammo_12.7x108"),18000.f) && Near(CartridgeEnergy("ammo_7.62x54_7h1"),3600.f));
    CHECK(Near(CartridgeEnergy("ammo_9x18_fmj"),300.f) && Near(CartridgeEnergy("ammo_9x19_pbp"),500.f));
    CHECK(Near(CartridgeEnergy("ammo_strange"),1350.f));
    // One play at 60 Hz, capped at 20 s; unknown length uses the given count.
    CHECK(ClipSteps(1.f,99)==61 && ClipSteps(0.5f,99)==31 && ClipSteps(0.f,99)==99 && ClipSteps(60.f,99)==1200);
}

int main()
{
    Names();
    // Semi-automatic rifles at low GAMMA rates (SVD 60, WA2000 45, M82 100):
    // no turning handle, so self-loading whatever the rate.
    WeaponMechanics m=Classify(Rifle());
    CHECK(m.family==WeaponFamily::Rifle && m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);
    CHECK(m.vr_reload_supported && m.magazine_removable);
    // Stock AK-12 has fire_modes=1,3,-1. A spare magazine's cosmetic
    // rotation must not turn this automatic rifle into a manual lever/bolt.
    MechanicsEvidence ak12=Rifle(); ak12.capacity=30; ak12.automatic_mode=true;
    ak12.lever_part=true; ak12.bolt_turn_deg=180.f;
    m=Classify(ak12);
    CHECK(m.family==WeaponFamily::Rifle && m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);
    CHECK(m.magazine_removable && m.capacity==30);
    // Capability comes from all native configured modes, including when the
    // selected mode is single-shot. Invalid/disabled modes are not automatic.
    CHECK(!HasAutomaticFireMode(1) && !HasAutomaticFireMode(0) && !HasAutomaticFireMode(-2));
    CHECK(HasAutomaticFireMode(-1) && HasAutomaticFireMode(2) && HasAutomaticFireMode(3));
    // Native CWeaponMagazined stores configured modes as signed bytes.
    const signed char native_modes[]={1,3,-1};
    CHECK(!HasAutomaticFireMode(native_modes[0]) && HasAutomaticFireMode(native_modes[1]) && HasAutomaticFireMode(native_modes[2]));
    bool configured_automatic=false;
    for (int mode:ak12_fixture::fire_modes) configured_automatic|=HasAutomaticFireMode(mode);
    CHECK(configured_automatic);
    for (int false_part=0;false_part<3;++false_part)
    {
        MechanicsEvidence evidence=Rifle(); evidence.capacity=30;
        evidence.automatic_mode=configured_automatic;
        evidence.lever_part=false_part!=0;
        evidence.bolt_turn_deg=false_part!=1 ? 180.f : 0.f;
        m=Classify(evidence);
        CHECK(m.family==WeaponFamily::Rifle && m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);
        CHECK(m.magazine_removable && m.vr_reload_supported && m.requires_manual_charging);
    }
    // Explicit mod/user overrides remain authoritative after inference.
    m=Classify(ak12);
    CHECK(ApplyMechanicsOverride(m,"action","manual_bolt") && m.action==ActionKind::ManualBolt);
    // A small turn (an AK's bolt rotating in its carrier) is not a bolt-action.
    MechanicsEvidence ak=Rifle(); ak.bolt_turn_deg=35.f;
    m=Classify(ak);
    CHECK(m.action==ActionKind::SelfLoading && !m.has_bolt_catch && m.requires_manual_charging);
    // Mosin, K98: the handle lifts 90 degrees; internal magazine, tri-state.
    MechanicsEvidence mosin=Rifle(false); mosin.bolt_turn_deg=88.f; mosin.tri_state=true; mosin.capacity=5;
    m=Classify(mosin);
    CHECK(m.family==WeaponFamily::BoltAction && m.action==ActionKind::ManualBolt && m.feed==FeedKind::InternalRounds);
    CHECK(!m.magazine_removable && m.capacity==5 && !m.has_bolt_catch && !m.supports_last_round_hold_open);
    // M700 / SV-98 with a box magazine: a bolt-action with a magazine.
    MechanicsEvidence sv98=Rifle(true); sv98.bolt_turn_deg=70.f;
    m=Classify(sv98);
    CHECK(m.family==WeaponFamily::BoltAction && m.feed==FeedKind::Detachable && m.magazine_removable);
    // GAMMA's SV-98: its handle lifts only 55 degrees, but before the bolt
    // comes back; a rotating bolt head turning as much with its carrier
    // stays a self-loader.
    MechanicsEvidence sv98_gamma=Rifle(true); sv98_gamma.bolt_turn_deg=55.f; sv98_gamma.turn_leads=true;
    CHECK(Classify(sv98_gamma).action==ActionKind::ManualBolt);
    MechanicsEvidence carrier_turn=Rifle(true); carrier_turn.bolt_turn_deg=55.f;
    CHECK(Classify(carrier_turn).action!=ActionKind::ManualBolt);
    // SKS: a self-loader with a 10-round internal magazine charged from the
    // top; its follower holds the bolt open, released by working the bolt
    // (no button). SKS-D / SKS-M take a detachable magazine.
    MechanicsEvidence sks=Rifle(false); sks.top_loaded=true; sks.capacity=10;
    m=Classify(sks);
    CHECK(m.family==WeaponFamily::Rifle && m.action==ActionKind::SelfLoading && m.feed==FeedKind::InternalRounds);
    CHECK(m.supports_last_round_hold_open && !m.has_bolt_catch && m.requires_manual_charging && !m.magazine_removable);
    MechanicsEvidence sks_d=Rifle(true); sks_d.top_loaded=true;
    CHECK(Classify(sks_d).feed==FeedKind::Detachable && Classify(sks_d).magazine_removable);
    // Anomaly's SKS: shotgun class (WP_ASHTG), no tri-state tube, no
    // magazine, a cycling bolt: a rifle with an internal magazine.
    MechanicsEvidence sks_class; sks_class.shotgun_class=true; sks_class.cycling_part=true; sks_class.capacity=10;
    m=Classify(sks_class);
    CHECK(m.family==WeaponFamily::Rifle && m.feed==FeedKind::InternalRounds && m.supports_last_round_hold_open);
    // A rifle whose magazine was not found and that shows no clip: the
    // game's own reload, never a guessed one.
    CHECK(!Classify(Rifle(false)).vr_reload_supported);
    // M4: holds open on the last round and has a separate charging handle.
    MechanicsEvidence m4=Rifle(); m4.has_shot_l=m4.shot_l_holds_back=true; m4.separate_handle=true; m4.dust_cover_part=true;
    m4.automatic_mode=true; m4.lever_part=true; m4.bolt_turn_deg=90.f;
    m=Classify(m4);
    CHECK(m.has_bolt_catch && m.supports_last_round_hold_open && !m.requires_manual_charging && m.has_dust_cover);
    CHECK(m.has_charging_handle && !m.has_slide_stop);
    CHECK(m.family==WeaponFamily::Rifle && m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);
    // An SMG whose bolt holds open (MP5-type) has a release too.
    MechanicsEvidence mp5=Rifle(); mp5.smg_class=true; mp5.shot_l_holds_back=true;
    CHECK(Classify(mp5).family==WeaponFamily::Smg && Classify(mp5).has_bolt_catch);
    // AK-74: no hold-open, so no catch; worked by hand.
    MechanicsEvidence ak74=Rifle(); ak74.has_shot_l=true; ak74.automatic_mode=true; ak74.lever_part=true;
    m=Classify(ak74);
    CHECK(!m.has_bolt_catch && !m.supports_last_round_hold_open && m.requires_manual_charging);
    CHECK(m.family==WeaponFamily::Rifle && m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);
    // PM: a pistol whose slide holds open has a slide stop, no bolt catch.
    MechanicsEvidence pm=Rifle(); pm.pistol_class=true; pm.shot_l_holds_back=true;
    m=Classify(pm);
    CHECK(m.family==WeaponFamily::Pistol && m.has_slide_stop && !m.has_bolt_catch && !m.has_charging_handle);
    // A pistol model without a last-shot motion keeps its slide stop; one
    // whose last shot closes the slide has none.
    MechanicsEvidence usp=Rifle(); usp.pistol_class=true;
    CHECK(Classify(usp).has_slide_stop);
    usp.has_shot_l=true;
    CHECK(!Classify(usp).has_slide_stop && Classify(usp).requires_manual_charging);
    // BM-16, TOZ-34: hinged barrels; never a magazine, pump or bolt.
    MechanicsEvidence toz; toz.break_hinge=true; toz.barrels=2; toz.shotgun_class=true; toz.capacity=2;
    m=Classify(toz);
    CHECK(m.family==WeaponFamily::BreakAction && m.feed==FeedKind::Chambers && m.capacity==2 && !m.magazine_removable);
    MechanicsEvidence bm16; bm16.bm16_class=true;
    CHECK(Classify(bm16).family==WeaponFamily::BreakAction);
    // MP-133: a pump.
    MechanicsEvidence mp133; mp133.shotgun_class=true; mp133.pump_part=true; mp133.tri_state=true;
    m=Classify(mp133);
    CHECK(m.family==WeaponFamily::PumpShotgun && m.action==ActionKind::Pump && m.feed==FeedKind::Tube);
    // A self-loading shotgun: tube or box.
    MechanicsEvidence m1014; m1014.shotgun_class=true; m1014.tri_state=true; m1014.cycling_part=true;
    m=Classify(m1014);
    CHECK(m.family==WeaponFamily::SemiShotgun && m.feed==FeedKind::Tube);
    MechanicsEvidence saiga=m1014; saiga.tri_state=false; saiga.detachable_magazine=true;
    CHECK(Classify(saiga).feed==FeedKind::Detachable);
    // Winchester 1873: a lever.
    MechanicsEvidence lever; lever.lever_part=true; lever.tri_state=true; lever.cycling_part=true;
    CHECK(Classify(lever).family==WeaponFamily::LeverAction);
    // GAMMA semi-only self-loaders (fire_modes = 1: AR-15, SIG 550, Mk 14,
    // SVD). No automatic veto applies; a hammer swinging below the receiver
    // posed as a lever, and a turning part as a bolt handle.
    MechanicsEvidence semi_hammer=Rifle(); semi_hammer.lever_part=true;
    m=Classify(semi_hammer);
    CHECK(m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);   // a box magazine: no lever gun
    MechanicsEvidence semi_turn=Rifle(); semi_turn.bolt_turn_deg=90.f;
    CHECK(Classify(semi_turn).action==ActionKind::ManualBolt);   // turn alone still reads as a bolt-action...
    semi_turn.fast_return=FastReturn(4.f/60.f);
    m=Classify(semi_turn);
    CHECK(m.action==ActionKind::SelfLoading && m.feed==FeedKind::Detachable);    // ...unless the shot cycles it
    // A hand-worked bolt stays back for the hand: no veto (Mosin, SV-98).
    CHECK(!FastReturn(0.f) && !FastReturn(0.4f) && FastReturn(fast_return_seconds));
    MechanicsEvidence slow_bolt; slow_bolt.cycling_part=true; slow_bolt.bolt_turn_deg=90.f; slow_bolt.top_loaded=true;
    slow_bolt.fast_return=FastReturn(30.f/60.f);
    CHECK(Classify(slow_bolt).action==ActionKind::ManualBolt && Classify(slow_bolt).feed==FeedKind::InternalRounds);
    // A quick bolt-action shot animation (back for 0.1 s) whose handle lifts
    // before the bolt comes back stays hand-worked; the automatic veto holds.
    MechanicsEvidence quick_bolt=slow_bolt; quick_bolt.fast_return=FastReturn(6.f/60.f); quick_bolt.turn_leads=true;
    CHECK(Classify(quick_bolt).action==ActionKind::ManualBolt);
    quick_bolt.automatic_mode=true;
    CHECK(Classify(quick_bolt).action!=ActionKind::ManualBolt);
    // Handle up (90 deg by frame 3), then the bolt back: the turn leads.
    const float lift[]={0.f,40.f,80.f,90.f,90.f,90.f,90.f,45.f,0.f};
    const float bolt[]={0.f,0.f,0.f,0.f,0.03f,0.07f,0.08f,0.02f,0.f};
    CHECK(TurnLeadsStroke(lift,bolt,9,90.f,0.08f));
    // A self-loader's carrier moves back first, its bolt turning on the way.
    const float cam[]={0.f,0.f,10.f,80.f,90.f,40.f,0.f,0.f,0.f};
    const float carrier[]={0.f,0.02f,0.05f,0.08f,0.06f,0.02f,0.f,0.f,0.f};
    CHECK(!TurnLeadsStroke(cam,carrier,9,90.f,0.08f));
    CHECK(!TurnLeadsStroke(lift,bolt,9,0.f,0.08f) && !TurnLeadsStroke(nullptr,bolt,9,90.f,0.08f));
    // A lever gun cycling slowly with its tube keeps its lever.
    MechanicsEvidence slow_lever=lever; slow_lever.fast_return=FastReturn(0.5f);
    CHECK(Classify(slow_lever).action==ActionKind::Lever);
    // An explicit override still wins over the derived veto.
    WeaponMechanics forced=Classify(semi_turn);
    CHECK(ApplyMechanicsOverride(forced,"action","manual_bolt") && forced.action==ActionKind::ManualBolt);
    // Revolver: cylinder.
    MechanicsEvidence revolver; revolver.revolver_class=true; revolver.capacity=6;
    m=Classify(revolver);
    CHECK(m.family==WeaponFamily::Revolver && m.feed==FeedKind::Cylinder && !m.magazine_removable);
    // RPG: muzzle; M79: break-open chamber.
    MechanicsEvidence rpg; rpg.launcher_class=true;
    CHECK(Classify(rpg).feed==FeedKind::Muzzle);
    MechanicsEvidence m79; m79.launcher_class=true; m79.break_hinge=true;
    CHECK(Classify(m79).family==WeaponFamily::Launcher && Classify(m79).feed==FeedKind::Chambers);
    // LMG with a belt.
    MechanicsEvidence pkm=Rifle(); pkm.belt=true; pkm.detachable_magazine=true;
    CHECK(Classify(pkm).family==WeaponFamily::MachineGun && Classify(pkm).magazine_removable);   // a belt box
    MechanicsEvidence belt; belt.belt=true; belt.cycling_part=true;
    CHECK(Classify(belt).family==WeaponFamily::MachineGun && Classify(belt).feed==FeedKind::Belt);
    // Unknown (nothing found): the game's own reload.
    m=Classify(MechanicsEvidence());
    CHECK(!m.vr_reload_supported && m.feed==FeedKind::None);
    MechanicsEvidence unknown_automatic; unknown_automatic.automatic_mode=true;
    m=Classify(unknown_automatic);
    CHECK(!m.vr_reload_supported && m.feed==FeedKind::None);
    // Override names: most specific first, trimmed at underscores, never the bare "wpn".
    char names[8][64];
    int n=OverrideNames("wpn_m4a1_custom",names,8);
    CHECK(n==2 && !std::strcmp(names[0],"wpn_m4a1_custom") && !std::strcmp(names[1],"wpn_m4a1"));
    n=OverrideNames("wpn_ak74u_m1_isg",names,8);
    CHECK(n==3 && !std::strcmp(names[2],"wpn_ak74u"));
    n=OverrideNames("wpn_svt40",names,8);
    CHECK(n==1 && !std::strcmp(names[0],"wpn_svt40"));
    // Overrides.
    m=Classify(Rifle());
    CHECK(ApplyMechanicsOverride(m,"has_bolt_catch","true") && m.has_bolt_catch);
    CHECK(ApplyMechanicsOverride(m,"feed","internal_rounds") && m.feed==FeedKind::InternalRounds);
    CHECK(ApplyMechanicsOverride(m,"handle_notch","true") && m.handle_notch);
    CHECK(!m.ambidextrous_handle && ApplyMechanicsOverride(m,"ambidextrous_handle","true") && m.ambidextrous_handle);
    const char* why=nullptr;
    CHECK(!ApplyMechanicsOverride(m,"colour","red",&why) && why && std::strstr(why,"unknown"));
    CHECK(!ApplyMechanicsOverride(m,"action","laser",&why) && why && std::strstr(why,"unknown"));
    // Keys the evidence decides are refused with a reason, never half-applied:
    // nothing at run time read them.
    for (const char* key:{"family","capacity","requires_manual_charging","has_charging_handle","magazine_removable"})
    {
        WeaponMechanics before=Classify(Rifle()), after=before;
        why=nullptr;
        CHECK(!ApplyMechanicsOverride(after,key,"true",&why) && why && DerivedMechanicsKey(key));
        CHECK(after.family==before.family && after.capacity==before.capacity &&
            after.requires_manual_charging==before.requires_manual_charging &&
            after.has_charging_handle==before.has_charging_handle && after.magazine_removable==before.magazine_removable);
    }
    CHECK(!ApplyMechanicsOverride(m,"capacity","7") && m.capacity==10);
    // Settling follows the overridden keys: a released catch is no hand
    // charge; a feed VR does not load by hand is the game's own reload.
    m=Classify(Rifle());
    CHECK(m.requires_manual_charging);
    ApplyMechanicsOverride(m,"has_bolt_release","true");
    SettleMechanics(m);
    CHECK(!m.requires_manual_charging && m.vr_reload_supported && m.magazine_removable);
    for (const char* feed:{"none","cylinder","muzzle"})
    {
        WeaponMechanics native=Classify(Rifle());
        CHECK(ApplyMechanicsOverride(native,"feed",feed));
        SettleMechanics(native);
        CHECK(!native.vr_reload_supported && !native.magazine_removable);
    }
    // Settling changes nothing a classification gave on its own.
    for (const MechanicsEvidence& evidence:{Rifle(),Rifle(false),mosin,sks,toz,revolver,pkm})
    {
        const WeaponMechanics plain=Classify(evidence);
        WeaponMechanics settled=plain;
        SettleMechanics(settled);
        CHECK(settled.vr_reload_supported==(plain.vr_reload_supported && plain.feed!=FeedKind::Cylinder &&
            plain.feed!=FeedKind::Muzzle));
        CHECK(settled.magazine_removable==plain.magazine_removable &&
            settled.requires_manual_charging==plain.requires_manual_charging);
    }
    // Override layers: least specific first, the weapon's own names over its
    // parent's, a shared name once at the weapon's own place.
    {
        const char* present[]={"wpn_m4a1","wpn_m4a1_custom","wpn_mp5","wpn_ak74"};
        const auto exists=[&](const char* name)
        { for (const char* p:present) if (!std::strcmp(p,name)) return true; return false; };
        char layers[16][64];
        int count=OverrideLayers("wpn_m4a1_custom_acog","wpn_m4a1_custom",exists,layers,16);
        CHECK(count==2 && !std::strcmp(layers[0],"wpn_m4a1") && !std::strcmp(layers[1],"wpn_m4a1_custom"));
        count=OverrideLayers("wpn_m4a1_custom","wpn_mp5_sd",exists,layers,16);
        CHECK(count==3 && !std::strcmp(layers[0],"wpn_mp5") && !std::strcmp(layers[1],"wpn_m4a1") &&
            !std::strcmp(layers[2],"wpn_m4a1_custom"));
        count=OverrideLayers("wpn_ak74",nullptr,exists,layers,16);
        CHECK(count==1 && !std::strcmp(layers[0],"wpn_ak74"));
        count=OverrideLayers("wpn_ak74","wpn_ak74",exists,layers,16);
        CHECK(count==1);
        CHECK(OverrideLayers("wpn_svd","wpn_svd_base",exists,layers,16)==0);
        // The most specific layer survives a short capacity.
        count=OverrideLayers("wpn_m4a1_custom","wpn_mp5_sd",exists,layers,1);
        CHECK(count==1 && !std::strcmp(layers[0],"wpn_m4a1_custom"));
        // Applied in order, the specific layer wins its own keys only.
        WeaponMechanics layered=Classify(Rifle());
        ApplyMechanicsOverride(layered,"supports_last_round_hold_open","true");   // [wpn_m4a1]
        ApplyMechanicsOverride(layered,"has_bolt_catch","true");                  // [wpn_m4a1]
        ApplyMechanicsOverride(layered,"has_bolt_catch","false");                 // [wpn_m4a1_custom]
        CHECK(layered.supports_last_round_hold_open && !layered.has_bolt_catch);
    }
    // An override cannot add what the model does not show.
    {
        ProfileHandling rifle; rifle.detachable=true; rifle.cycling=true;
        CHECK(FeedHonoured(FeedKind::Detachable,rifle) && !FeedHonoured(FeedKind::Tube,rifle) &&
            !FeedHonoured(FeedKind::Belt,rifle) && !FeedHonoured(FeedKind::Chambers,rifle));
        CHECK(FeedHonoured(FeedKind::None,rifle) && FeedHonoured(FeedKind::Muzzle,rifle));
        CHECK(ActionHonoured(ActionKind::SelfLoading,rifle) && ActionHonoured(ActionKind::ManualBolt,rifle) &&
            !ActionHonoured(ActionKind::Pump,rifle) && !ActionHonoured(ActionKind::Break,rifle) &&
            !ActionHonoured(ActionKind::None,rifle));
        ProfileHandling belt=rifle; belt.belt=true;
        CHECK(FeedHonoured(FeedKind::Belt,belt) && !FeedHonoured(FeedKind::Detachable,belt));
        ProfileHandling pump; pump.tube=true; pump.pump=true;
        CHECK(FeedHonoured(FeedKind::Tube,pump) && ActionHonoured(ActionKind::Pump,pump) &&
            !ActionHonoured(ActionKind::SelfLoading,pump) && !ActionHonoured(ActionKind::ManualBolt,pump));
        ProfileHandling bare;
        CHECK(!ActionHonoured(ActionKind::Lever,bare) && ActionHonoured(ActionKind::None,bare) &&
            ActionHonoured(ActionKind::Launcher,bare) && !FeedHonoured(FeedKind::InternalRounds,bare));
    }

    // A bolt-action path: the knob lifts through a 90-degree arc of 5 cm
    // radius about the bore, then the bolt slides 10 cm back.
    std::vector<V3> points; std::vector<float> rears;
    for (int i=0;i<=18;++i)
    {
        const float a=0.5f*3.14159265f*float(i)/18.f;
        points.push_back(V3(0.05f*std::cos(a),0.05f*std::sin(a),0.f)); rears.push_back(0.f);
    }
    for (int i=1;i<=20;++i) { points.push_back(V3(0.f,0.05f,-0.005f*float(i))); rears.push_back(0.005f*float(i)); }
    ActionPath path; path.Build(points,rears);
    CHECK(path.Valid() && std::fabs(path.RearMax()-0.1f)<1e-4f);
    // A valid cosmetic lever path can still have been sampled before final
    // classification. It must not replace an automatic rifle's measured
    // charging-handle grab point/travel when the lever inference is rejected.
    const V3 native_grab(0.02f,0.07f,0.26f);
    V3 grab=native_grab; float travel=0.1272f;
    CHECK(!ApplyManualActionPath(Classify(ak12),path,grab,travel));
    CHECK(grab.x==native_grab.x && grab.y==native_grab.y && grab.z==native_grab.z && travel==0.1272f);
    // Accepted manual actions and explicit user/mod mechanics overrides
    // keep their animation-derived path, independently of initial inference.
    for (const MechanicsEvidence& evidence:{mosin,sv98,lever})
    {
        grab=native_grab; travel=0.1272f;
        CHECK(ApplyManualActionPath(Classify(evidence),path,grab,travel));
        CHECK(std::fabs(grab.x-0.05f)<1e-5f && grab.y==0.f && grab.z==0.f);
        CHECK(std::fabs(travel-0.1f)<1e-5f);
    }
    WeaponMechanics explicit_manual=Classify(ak12);
    CHECK(ApplyMechanicsOverride(explicit_manual,"action","manual_bolt"));
    grab=native_grab; travel=0.1272f;
    CHECK(ApplyManualActionPath(explicit_manual,path,grab,travel));
    CHECK(std::fabs(grab.x-0.05f)<1e-5f && std::fabs(travel-0.1f)<1e-5f);
    ActionPath invalid_path;
    grab=native_grab; travel=0.1272f;
    CHECK(!ApplyManualActionPath(explicit_manual,invalid_path,grab,travel));
    CHECK(grab.x==native_grab.x && grab.y==native_grab.y && grab.z==native_grab.z && travel==0.1272f);
    ActionPath short_path=path, long_path=path;
    for (float& rear:short_path.rear) rear=0.001f;
    for (float& rear:long_path.rear) rear=0.4f;
    CHECK(ApplyManualActionPath(explicit_manual,short_path,grab,travel) && travel==0.01f);
    CHECK(ApplyManualActionPath(explicit_manual,long_path,grab,travel) && travel==0.15f);
    const float turn_end=path.TurnEnd();
    CHECK(turn_end>0.4f && turn_end<0.5f);   // the arc is 7.9 cm of 17.9
    // Lifting the knob follows the arc and moves the bolt nowhere.
    float u=0.f;
    u=path.Project(V3(0.045f,0.02f,0.f),u);
    CHECK(u>0.f && u<turn_end && path.RearAt(u)==0.f);
    // Pulling straight back from closed cannot skip the turn.
    float stuck=path.Project(V3(0.05f,0.f,-0.1f),0.f);
    CHECK(path.RearAt(stuck)<0.005f);
    // Up and then back: the bolt comes back.
    u=0.f;
    for (int i=1;i<=10;++i) u=path.Project(V3(0.05f*std::cos(0.157f*i),0.05f*std::sin(0.157f*i),0.f),u);
    for (int i=1;i<=25;++i) u=path.Project(V3(0.f,0.05f,-0.004f*i),u);
    CHECK(u>0.95f && path.RearAt(u)>0.095f);
    std::printf("PASS: VR weapon mechanics, action path, weapon names\n");
    return 0;
}
