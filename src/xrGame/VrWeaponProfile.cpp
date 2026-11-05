// A weapon's VR profile: loaded from the overrides or derived from its model,
// animations and config (never its rate of fire), measured once it is the player's.
#include "stdafx.h"
#include "VrWeaponInteraction.h"
#include "VrWeaponPose.h"
#include "VrReloadDerivation.h"
#include "VrMagazineWayIn.h"
#include "VrSupportAction.h"
#include "VrWeaponNames.h"
#include "VrBodySlots.h"
#include "VrSaveLedger.h"
#include "Bolt.h"
#include "WeaponAmmo.h"
#include "ai_object_location.h"
#include "Weapon.h"
#include "WeaponMagazined.h"
#include "WeaponPistol.h"
#include "WeaponRevolver.h"
#include "WeaponKnife.h"
#include "WeaponBinoculars.h"
#include "xr_level_controller.h"
#include "Actor.h"
#include "inventory.h"
#include "PhysicsShellHolder.h"
#include "Level.h"
#include "game_cl_base.h"
#include "player_hud.h"
#include "HUDManager.h"
#include "UIGameCustom.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_game_object.h"
#include "../xrEngine/CameraBase.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/Vr/vr_gameplay.h"
#include "../xrEngine/Vr/vr_stereo.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/DrawUtils.h"
#include "../xrEngine/Vr/vr_overlay_draw.h"
#include "../xrEngine/bone.h"
#include "../xrEngine/EnnumerateVertices.h"
#include "WeaponShotgun.h"
#include "WeaponBM16.h"
#include "WeaponRG6.h"
#include "WeaponRPG7.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "ClimableObject.h"
#include "../xrphysics/Extendedgeom.h"
#include "../xrPhysics/PhysicsShell.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "xrServer_Objects_ALife_Items.h"
#include "VrWeaponInteractionInternal.h"
using namespace vrwi;

bool CVrWeaponInteraction::LoadProfile()
{
    // Binoculars are a magazine weapon to the game, but no gun: deriving a
    // gun from their model held them reversed in the hand and took them off
    // the belt-item path that looks through them (VrBodySlots.cpp).
    if (!m_weapon.cast_weapon_magazined() || smart_cast<CWeaponBinoculars*>(&m_weapon)) return false;
    // Whether the weapon is held in one hand (VrHandgun).
    m_handgun=VrHandgun(m_weapon);
    m_pistol_handgun=m_handgun && smart_cast<CWeaponPistol*>(&m_weapon)!=nullptr;
    xr_string section="vr_wpn_";
    section+=m_weapon.cNameSect().c_str();
    if (!pSettings->section_exist(m_weapon.HudSection()) ||
        !pSettings->line_exist(m_weapon.HudSection(),"item_visual"))
    {
        Msg("! [VR weapon] %s: HUD visual is unavailable",section.c_str());
        return false;
    }
    m_profile.section=section.c_str();
    m_profile.visual=pSettings->r_string(m_weapon.HudSection(),"item_visual");
    if (pSettings->line_exist(m_weapon.HudSection(),"fire_point"))
        m_profile.muzzle=pSettings->r_fvector3(m_weapon.HudSection(),"fire_point");
    if (pSettings->line_exist(m_weapon.HudSection(),"shell_point"))
    {
        m_profile.shell_point=pSettings->r_fvector3(m_weapon.HudSection(),"shell_point");
        // A point parked far off the model (the MP-133's -50,-50,-50) hides
        // the native shell particles because the shot animation shows the
        // case: it is no ejection port.
        if (m_profile.shell_point.magnitude()>1.5f)
        {
            Msg("* [VR weapon] %s: shell_point (%.1f %.1f %.1f) is off the model; not used as the ejection port",
                section.c_str(),m_profile.shell_point.x,m_profile.shell_point.y,m_profile.shell_point.z);
            m_profile.shell_point.set(0.f,0.f,0.f);
        }
    }
    const auto basic_profile=[&]() -> bool
    {
        // Without an audited profile, derive grip, muzzle and magazine from
        // the HUD model's own bones and meshes when it is first loaded (see
        // AutoProfile). Slides are not inferred: no chamber or cycling.
        m_profile.automatic=true;
        m_profile.grip_offset.set(0.f,0.f,0.f);
        m_profile.detachable=m_profile.chamber=m_profile.slide_stop=false;
        m_profile.cycle_travel=0.f;
        m_basic_magazine=PistolHandgun();
        if (m_basic_magazine)
        {
            m_profile.mag_box_center.set(0.f,0.f,0.f);
            m_profile.mag_box_half.set(0.018f,0.053f,0.012f);
            m_profile.mag_base.set(0.f,-0.045f,0.065f);
        }
        m_profile.locks_on_empty=false;
        Msg("* [VR weapon] %s: no audited profile; deriving one from the HUD model",
            section.c_str());
        return true;
    };
    // Parsed once for every weapon.
    static CInifile* const weapons_ini=[]() -> CInifile*
    {
        string_path path;
        FS.update_path(path,"$game_config$","vr\\vr_weapons.ltx");
        return FS.exist(path) ? xr_new<CInifile>(path,TRUE) : nullptr;
    }();
    if (!weapons_ini) return basic_profile();
    CInifile& ini=*weapons_ini;
    if (!ini.section_exist(section.c_str())) return basic_profile();
    const auto text=[&](LPCSTR key) -> LPCSTR
    { return ini.line_exist(section.c_str(),key) ? ini.r_string(section.c_str(),key) : ""; };
    const auto flag=[&](LPCSTR key) -> bool
    { return ini.line_exist(section.c_str(),key) && !!ini.r_bool(section.c_str(),key); };
    const auto number=[&](LPCSTR key, float fallback) -> float
    { return ini.line_exist(section.c_str(),key) ? ini.r_float(section.c_str(),key) : fallback; };
    const auto vector=[&](LPCSTR key, Fvector& value)
    { if (ini.line_exist(section.c_str(),key)) value=ini.r_fvector3(section.c_str(),key); };
    if (!ini.line_exist(section.c_str(),"expected_visual") ||
        xr_strcmp(text("expected_visual"),m_profile.visual.c_str())!=0)
    {
        Msg("! [VR weapon] %s: HUD visual differs from audited profile; physical interactions disabled",section.c_str());
        return basic_profile();
    }
    m_profile.mag_bone=text("mag_bone");
    m_profile.magazine_visual_prefix=text("magazine_visual_prefix");
    m_profile.magazine_visual_max_rounds=unsigned(std::max(0.f,
        number("magazine_visual_max_rounds",0.f)));
    m_profile.cycle_bone=text("cycle_bone");
    m_profile.rounds_bone=text("rounds_bone");
    m_profile.mag_extra_bones[0]=text("mag_extra_bone_1");
    m_profile.mag_extra_bones[1]=text("mag_extra_bone_2");
    vector("grip_offset",m_profile.grip_offset);
    vector("support_offset",m_profile.support_offset);
    vector("mag_grip",m_profile.mag_grip);
    vector("mag_base",m_profile.mag_base);
    vector("cycle_grab",m_profile.cycle_grab);
    vector("muzzle_offset",m_profile.muzzle);
    vector("shell_offset",m_profile.shell_point);
    vector("mag_box_half",m_profile.mag_box_half);
    vector("mag_box_center",m_profile.mag_box_center);
    m_profile.check_mag_bind=ini.line_exist(section.c_str(),"expected_mag_bind");
    m_profile.check_cycle_bind=ini.line_exist(section.c_str(),"expected_cycle_bind");
    vector("expected_mag_bind",m_profile.expected_mag_bind);
    vector("expected_cycle_bind",m_profile.expected_cycle_bind);
    m_profile.bind_tolerance=number("bind_tolerance",0.005f);
    m_profile.cycle_travel=number("cycle_travel",0);
    m_profile.extraction=number("extraction",0);
    m_profile.grab_radius=number("grab_radius",0.08f);
    m_profile.guide_radius=number("guide_radius",0.015f);
    m_profile.hand_grip_angle=number("hand_grip_angle",m_profile.hand_grip_angle);
    m_profile.has_hand_grip_center=ini.line_exist(section.c_str(),"hand_grip_center");
    vector("hand_grip_center",m_profile.hand_grip_center);
    m_profile.hand_grip_radius=number("hand_grip_radius",m_profile.hand_grip_radius);
    m_profile.has_trigger_point=ini.line_exist(section.c_str(),"trigger_point");
    vector("trigger_point",m_profile.trigger_point);
    m_profile.break_action=!xr_strcmp(text("mechanism"),"break_action");
    m_profile.hinge_bone=text("hinge_bone");
    m_profile.hinge_angle=deg2rad(number("hinge_angle",35.f));
    m_profile.shell_bones[0]=text("shell_bone_1");
    m_profile.shell_bones[1]=text("shell_bone_2");
    m_profile.shell_box_half.set(0.011f,0.011f,0.034f);
    vector("shell_box_half",m_profile.shell_box_half);
    m_profile.two_handed=flag("two_handed");
    m_profile.detachable=flag("detachable_magazine");
    m_profile.chamber=flag("chamber");
    m_profile.slide_stop=flag("slide_stop");
    if (m_profile.detachable &&
        (!m_profile.mag_bone.size() || m_profile.extraction<=0 ||
         m_profile.mag_box_half.x<=0 || m_profile.mag_box_half.y<=0 || m_profile.mag_box_half.z<=0))
    {
        Msg("! [VR weapon] %s: invalid magazine profile; physical magazine disabled",section.c_str());
        m_profile.detachable=false;
    }
    if (m_profile.cycle_travel<=0 || !m_profile.cycle_bone.size())
    {
        m_profile.cycle_travel=0;
        m_profile.cycle_bone="";
    }
    m_profile.travel_limit=m_profile.cycle_travel>0 ? m_profile.cycle_travel :
        vr::MagazineMechanism<xr_vector<CCartridge>>::travel;
    m_profile.locks_on_empty=m_profile.slide_stop;
    if (ini.line_exist(section.c_str(),"well_axis"))
    {
        vector("well_axis",m_profile.well_axis);
        m_profile.well_axis.normalize_safe();
        m_profile.explicit_well_axis=true;
    }
    // Moving parts: part_N_name, _bone, _kind (hinge|slider), _axis,
    // _pivot, _min/_max (degrees for a hinge, metres for a slider),
    // _open_at/_closed_at, _grab, _grab_radius, _release_distance, _speed.
    // A hinge without an axis takes axis, pivot and opening from the
    // weapon's reload animation (MeasurePartMotion).
    const char* part_names[vr::max_parts]{};
    for (int n=0;n<vr::max_parts;++n)
    {
        const auto key=[&](LPCSTR name) { static string64 k; xr_sprintf(k,"part_%d_%s",n+1,name); return k; };
        if (!ini.line_exist(section.c_str(),key("bone"))) break;
        vr::PartDef& def=m_profile.parts[n];
        def=vr::PartDef{};
        const bool hinge=xr_strcmp(text(key("kind")),"slider")!=0;
        def.kind=hinge ? vr::PartHinge : vr::PartSlider;
        const float unit=hinge ? PI/180.f : 1.f;
        m_profile.part_bones[n]=text(key("bone"));
        m_profile.part_names[n]=ini.line_exist(section.c_str(),key("name")) ? text(key("name")) : text(key("bone"));
        part_names[n]=m_profile.part_names[n].c_str();
        Fvector v;
        m_profile.part_from_animation[n]=hinge && !ini.line_exist(section.c_str(),key("axis"));
        if (ini.line_exist(section.c_str(),key("axis"))) { v=ini.r_fvector3(section.c_str(),key("axis")); v.normalize_safe(); def.axis={v.x,v.y,v.z}; }
        if (ini.line_exist(section.c_str(),key("pivot"))) { v=ini.r_fvector3(section.c_str(),key("pivot")); def.pivot={v.x,v.y,v.z}; }
        if (ini.line_exist(section.c_str(),key("grab"))) { v=ini.r_fvector3(section.c_str(),key("grab")); def.grab={v.x,v.y,v.z}; }
        def.min=number(key("min"),0.f)*unit;
        def.max=number(key("max"),hinge ? 70.f : 0.05f)*unit;
        def.open_at=number(key("open_at"),(hinge ? 0.8f*def.max/unit : 0.8f*def.max))*unit;
        def.closed_at=number(key("closed_at"),hinge ? 3.f : 0.003f)*unit;
        def.grab_radius=number(key("grab_radius"),0.06f);
        def.release_distance=number(key("release_distance"),0.07f);
        def.max_speed=number(key("speed"),hinge ? 6.f : 1.5f);
        def.falls_closed=!ini.line_exist(section.c_str(),key("falls_closed")) || flag(key("falls_closed"));
        m_profile.part_count=n+1;
    }
    // Reload rules: rule_magazine, rule_belt, rule_fire, rule_chamber, as
    // "cover:open, belt:seated, magazine:in" (VrReloadRules.h).
    const auto rule=[&](LPCSTR key, vr::GateList& gates)
    {
        if (!ini.line_exist(section.c_str(),key)) return;
        const char* bad=nullptr;
        gates=vr::ParseGates(text(key),part_names,m_profile.part_count,&bad);
        if (bad) Msg("! [VR weapon] %s: %s has an unknown condition: %s",section.c_str(),key,bad);
    };
    rule("rule_magazine",m_profile.rules.magazine);
    rule("rule_belt",m_profile.rules.belt);
    rule("rule_fire",m_profile.rules.fire);
    rule("rule_chamber",m_profile.rules.chamber);
    // A belt laid by hand from the box mouth onto the feed tray; its loose
    // end (belt_free_bone and below) hangs hidden until it is.
    if (ini.line_exist(section.c_str(),"belt_tray") && ini.line_exist(section.c_str(),"belt_box_end"))
    {
        Fvector v;
        v=ini.r_fvector3(section.c_str(),"belt_tray"); m_profile.belt.tray={v.x,v.y,v.z};
        v=ini.r_fvector3(section.c_str(),"belt_box_end"); m_profile.belt.box_end={v.x,v.y,v.z};
        m_profile.belt.tolerance=number("belt_tolerance",0.035f);
        m_profile.belt.grab_radius=number("belt_grab_radius",0.06f);
        m_profile.belt_free_bone=text("belt_free_bone");
        m_profile.belt.used=true;
    }
    Msg("* [VR weapon] %s: profile loaded for %s; magazine=%d cycle=%d break=%d parts=%d belt=%d",
        section.c_str(),m_profile.visual.c_str(),m_profile.detachable,m_profile.cycle_travel>0,
        m_profile.break_action,m_profile.part_count,m_profile.belt.used);
    return true;
}

void CVrWeaponInteraction::AutoProfile(IKinematics& model)
{
    m_profile.automatic=false;
    m_profile.derived=true;
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    LPCSTR hud=m_weapon.HudSection().c_str();
    // Muzzle and ejection port: HUD offsets are relative to their bones.
    const auto bone_point=[&](LPCSTR bone_key, LPCSTR point_key, Fvector& out)
    {
        if (!pSettings->line_exist(hud,bone_key)) return;
        const u16 id=model.LL_BoneID(pSettings->r_string(hud,bone_key));
        if (id==BI_NONE || id>=bind.size()) return;
        Fvector offset{};
        if (pSettings->line_exist(hud,point_key)) offset=pSettings->r_fvector3(hud,point_key);
        bind[id].transform_tiny(out,offset);
    };
    bone_point("fire_bone","fire_point",m_profile.muzzle);
    bone_point("shell_bone","shell_point",m_profile.shell_point);
    // Every audited HUD gun has its grip about 2.5 cm below and 5.5 cm
    // behind the trigger blade; place the controller and palm there.
    const u16 trigger=FindTrigger(model);
    Fvector grip; grip.set(0.f,-0.03f,-0.05f);
    if (trigger!=BI_NONE)
    {
        Fbox box;
        if (MeasureBone(model,trigger,box,false)) box.getcenter(m_profile.trigger_point);
        else m_profile.trigger_point=bind[trigger].c;
        m_profile.has_trigger_point=true;
        grip.add(m_profile.trigger_point,Fvector().set(0.f,-0.028f,-0.058f));
    }
    m_profile.grip_offset=grip;
    m_profile.hand_grip_center=grip;
    m_profile.has_hand_grip_center=true;
    // Magazine: a bone named like "magazin"/"magazine"/"mag_*" whose subtree
    // has a magazine-sized mesh. Insertion follows the gun's +Y axis, which
    // holds for every bottom-fed HUD model in the game.
    u16 magazine=BI_NONE;
    for (u16 id=0;magazine==BI_NONE && id<count;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        if (vr::MagazineBoneName(name))
            magazine=id;
    }
    // Unnamed magazines: a separate part hanging well below the receiver
    // just ahead of the trigger, magazine-sized, holding a minority of the
    // mesh. Finds the Saiga's "joint13", for example.
    if (magazine==BI_NONE && trigger!=BI_NONE && m_weapon.iMagazineSize>2)
    {
        BoneBounds all;
        for (u16 id=0;id<count;++id) model.EnumBoneVertices(all,id);
        for (u16 id=0;id<count && magazine==BI_NONE;++id)
        {
            if (model.LL_GetData(id).GetParentID()==BI_NONE || ShellName(model.LL_BoneName_dbg(id))) continue;
            BoneBounds part;
            for (u16 child=0;child<count;++child)
                if (Descends(model,child,id)) model.EnumBoneVertices(part,child);
            if (!part.count || part.count*5>all.count*2) continue;
            const Fbox& b=part.box;
            const float centre_z=(b.min.z+b.max.z)*0.5f;
            const Fvector& t=m_profile.trigger_point;
            if (b.min.y<t.y-0.06f && centre_z>t.z && centre_z<t.z+0.25f &&
                b.max.y-b.min.y>0.05f && b.max.x-b.min.x<0.06f) magazine=id;
        }
    }
    // The reload animation knows the magazine whatever its name or shape (a
    // drum, a belt box), and which way it leaves the well. A magazine the
    // name and shape already find keeps the measured well and extraction
    // the stock weapons were checked with.
    const u16 moved=m_weapon.iMagazineSize>1 ? DeriveReload(model,trigger) : u16(BI_NONE);
    bool shaped=false;
    {
        Fbox found;
        if (magazine!=BI_NONE && MeasureMagazine(model,magazine,found,&m_profile.rest_local,&bind))
        {
            Fvector size; found.getsize(size);
            shaped=size.y>0.03f && size.y<0.45f && size.x<0.12f && size.z<0.3f;
        }
    }
    const bool by_motion=moved!=BI_NONE && !shaped;
    if (by_motion) magazine=moved;
    else m_profile.explicit_well_axis=false;
    // Loaded a round at a time (tri_state_reload with its open, add and
    // close motions): an internal magazine, never a detachable one.
    if (m_weapon.IsTriStateReload() && pSettings->line_exist(hud,"anm_open") &&
        pSettings->line_exist(hud,"anm_add_cartridge") && pSettings->line_exist(hud,"anm_close"))
    {
        magazine=BI_NONE;
        m_profile.explicit_well_axis=false;
    }
    Fbox box;
    if (m_weapon.iMagazineSize>1 && MeasureMagazine(model,magazine,box,&m_profile.rest_local,&bind))
    {
        Fvector size; box.getsize(size);
        if (by_motion || (size.y>0.03f && size.y<0.45f && size.x<0.12f && size.z<0.3f))
        {
            m_profile.mag_bone=model.LL_BoneName_dbg(magazine);
            m_profile.rounds_bone="";
            m_profile.mag_extra_bones[0]=m_profile.mag_extra_bones[1]="";
            const float magazine_height=MagazineHeight(model,magazine);
            int extra=0;
            for (u16 id=0;id<count;++id)
            {
                if (id>=bind.size() || model.LL_GetData(id).GetParentID()!=magazine ||
                    !InMagazine(model,id,magazine,magazine_height)) continue;
                Fbox part;
                if (!MeasureBone(model,id,part,true)) continue;
                LPCSTR name=model.LL_BoneName_dbg(id);
                if (!m_profile.rounds_bone.size() && (NameHas(name,"bullet") ||
                    NameHas(name,"patron") || NameHas(name,"round") || NameHas(name,"cartr")))
                    m_profile.rounds_bone=name;
                else if (extra<2) m_profile.mag_extra_bones[extra++]=name;
            }
            box.getcenter(m_profile.mag_box_center);
            m_profile.mag_box_half.set(std::max(size.x*0.5f,0.005f),size.y*0.5f,std::max(size.z*0.5f,0.005f));
            m_profile.mag_base.set(m_profile.mag_box_center.x,box.min.y,m_profile.mag_box_center.z);
            m_profile.mag_grip.invert(m_profile.mag_base);
            // A magazine found by its motion may be a drum or a box hanging
            // on a rail: it clears its seat in half its height or less.
            m_profile.extraction=by_motion ? clampr(size.y*0.5f,0.04f,0.12f) : clampr(size.y*0.95f,0.05f,0.25f);
            m_profile.guide_radius=0.015f;
            m_profile.detachable=true;
            m_profile.chamber=m_profile.slide_stop=false;
            m_basic_magazine=false;
            // Its contents, parents first, if the idle lays any out otherwise
            // than bound (DeriveReload): VR then poses them so.
            if (m_profile.rest_local.size()==count)
            {
                xr_vector<u16> depth(count,0);
                bool away=false;
                for (u16 id=0;id<count;++id)
                {
                    if (id==magazine || !InMagazine(model,id,magazine,magazine_height)) continue;
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    Fmatrix bound; bound.mul_43(Fmatrix().invert(bind[parent]),bind[id]);
                    const Fmatrix& rest=m_profile.rest_local[id];
                    away=away || bound.c.distance_to(rest.c)>=0.0005f || bound.i.dotproduct(rest.i)<0.9999f ||
                        bound.j.dotproduct(rest.j)<0.9999f;
                    for (u16 a=parent;a<count && a!=magazine;a=model.LL_GetData(a).GetParentID()) ++depth[id];
                    m_profile.rest_contents.push_back(id);
                }
                if (!away) m_profile.rest_contents.clear();
                std::sort(m_profile.rest_contents.begin(),m_profile.rest_contents.end(),
                    [&](u16 a, u16 b) { return depth[a]<depth[b]; });
                if (m_profile.rest_contents.size())
                    Msg("* [VR weapon] %s: magazine parts rest away from their bind place; %u posed at rest",
                        m_profile.section.c_str(),unsigned(m_profile.rest_contents.size()));
            }
        }
    }
    CacheReloadMagazines(model);
    if (!Handgun())
    {
        // A fire point not ahead of the grip (a launcher's tube) put the
        // support hand's place behind the grip: the front of the mesh is the
        // muzzle then, at the fire point's height.
        if (m_profile.muzzle.z<grip.z+0.1f)
        {
            BoneBounds all;
            for (u16 id=0;id<count;++id) model.EnumBoneVertices(all,id);
            if (all.count && all.box.max.z>grip.z+0.15f)
            {
                Msg("* [VR weapon] %s: fire point (%.2f %.2f %.2f) not ahead of the grip; muzzle at the model's front, %.2f m",
                    m_profile.section.c_str(),m_profile.muzzle.x,m_profile.muzzle.y,m_profile.muzzle.z,all.box.max.z);
                m_profile.muzzle.set(grip.x,m_profile.muzzle.y,all.box.max.z);
            }
        }
        // The bore runs over the trigger: a fire point off to the side (a
        // model built off its centreline, as the Mosin's is 15 cm) would skew
        // the fore-end line the support hand steers by.
        if (m_profile.has_trigger_point && _abs(m_profile.muzzle.x-m_profile.trigger_point.x)>0.03f)
        {
            Msg("* [VR weapon] %s: fire point %.3f m aside of the trigger; muzzle moved onto its line",
                m_profile.section.c_str(),m_profile.muzzle.x-m_profile.trigger_point.x);
            m_profile.muzzle.x=m_profile.trigger_point.x;
        }
        Fvector muzzle=m_profile.muzzle;
        if (muzzle.z<grip.z+0.1f) muzzle.set(grip.x,grip.y,grip.z+0.5f);
        m_profile.two_handed=true;
        m_profile.support_offset.lerp(grip,muzzle,0.5f);
        m_profile.support_offset.x=grip.x;
        m_profile.support_offset.y=grip.y+0.02f;
    }
    if (!m_profile.detachable && m_weapon.VrShellLoader()) DetectShells(model,trigger);
    m_profile.locks_on_empty=false;
    if (!m_profile.break_action && !m_profile.pump_bone.size()) MeasureCycle(model);
    Msg("* [VR weapon] %s: derived profile; grip=(%.3f %.3f %.3f) magazine=%s two_handed=%d shells=%s",
        m_profile.section.c_str(),grip.x,grip.y,grip.z,
        m_profile.detachable ? m_profile.mag_bone.c_str() : "none",m_profile.two_handed,
        m_profile.break_action ? "break-action" : m_profile.tube ? "tube" : "none");
}

void CVrWeaponInteraction::CacheReloadMagazines(IKinematics& model)
{
    m_profile.reload_magazines.clear();
    const u16 count=model.LL_BoneCount();
    m_profile.reload_magazine_mask.assign(count,0);
    if (!m_profile.detachable || !m_profile.mag_bone.size()) return;
    const u16 magazine=model.LL_BoneID(m_profile.mag_bone.c_str());
    // The seated magazine as drawn: its contents at rest (Pose), like the
    // idle boxes compared with it below.
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    const xr_vector<Fmatrix>* rest=m_profile.MagazineRest();
    Fbox seated;
    if (magazine>=count || !MeasureMagazine(model,magazine,seated,rest,&bind)) return;
    const float minimum=std::max(0.03f,(seated.max.y-seated.min.y)*0.5f);
    const float magazine_height=MagazineHeight(model,magazine);
    xr_vector<u16> candidates;
    xr_vector<Fbox> boxes;
    for (u16 id=0;id<count;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        if (!vr::MagazineBoneName(name) || Descends(model,magazine,id) ||
            (Descends(model,id,magazine) && InMagazine(model,id,magazine,magazine_height))) continue;
        // Explicit authored components (followers, rounds, floor plates)
        // remain owned by the canonical magazine, even outside its subtree.
        bool component=false;
        for (const shared_str* part:{&m_profile.rounds_bone,&m_profile.mag_extra_bones[0],&m_profile.mag_extra_bones[1]})
            if (part->size()) component=component || Descends(model,id,model.LL_BoneID(part->c_str()));
        Fbox box;
        if (component || !MeasureBone(model,id,box,false) || box.max.y-box.min.y<minimum) continue;
        candidates.push_back(id);
        boxes.push_back(box);
    }
    if (candidates.empty()) return;
    // Where the weapon's idle shows each of them (ReloadOnlyMagazine): a
    // second magazine it shows on the gun is part of the model.
    LPCSTR hud=m_weapon.HudSection().c_str();
    (void)hud;
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    const VrSampledClip* idle_clip=clips.Bones()==count && bind.size()>=count ? clips.Idle() : nullptr;
    const bool idle=idle_clip!=nullptr;
    // A bone's bind-space mesh as the idle places it.
    const auto at_idle=[&](u16 id) { return Fmatrix().mul_43(idle_clip->At(0,id),Fmatrix().invert(bind[id])); };
    Fmatrix to_seat;
    if (idle) to_seat.invert(at_idle(magazine));
    Fvector seat; seated.getcenter(seat);
    for (size_t n=0;n<candidates.size();++n)
    {
        const u16 id=candidates[n];
        LPCSTR name=model.LL_BoneName_dbg(id);
        bool scaled=false;
        float distance=0.f, overlap=0.f;
        if (idle)
        {
            const Fmatrix& pose=idle_clip->At(0,id);
            scaled=pose.i.magnitude()<0.5f || pose.j.magnitude()<0.5f || pose.k.magnitude()<0.5f;
            // Its idle box in the seated magazine's frame.
            const Fmatrix to=Fmatrix().mul_43(to_seat,at_idle(id));
            Fbox placed; placed.xform(boxes[n],to);
            Fvector centre; placed.getcenter(centre);
            distance=centre.distance_to(seat);
            Fvector low, high, size;
            low.max(placed.min,seated.min); high.min(placed.max,seated.max);
            placed.getsize(size);
            const float volume=size.x*size.y*size.z;
            overlap=high.x>low.x && high.y>low.y && high.z>low.z && volume>EPS_S ?
                (high.x-low.x)*(high.y-low.y)*(high.z-low.z)/volume : 0.f;
        }
        if (!vr::ReloadOnlyMagazine(idle,scaled,distance,overlap))
        {
            Msg("* [VR weapon] %s: second magazine %s shown by the idle (%.0f mm from %s); kept",
                m_profile.section.c_str(),name,distance*1000.f,m_profile.mag_bone.c_str());
            continue;
        }
        m_profile.reload_magazines.push_back(name);
        for (u16 child=0;child<count;++child)
            if (Descends(model,child,id)) m_profile.reload_magazine_mask[child]=1;
        Msg("* [VR weapon] %s: reload-only magazine %s hidden; canonical %s",m_profile.section.c_str(),
            name,m_profile.mag_bone.c_str());
    }
}

u16 CVrWeaponInteraction::DeriveReload(IKinematics& model, u16 trigger)
{
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (bind.size()<count) return BI_NONE;
    // Not the receiver chain carrying the trigger (the animation moves the
    // whole gun), loose shells or attachments.
    xr_vector<u8> excluded(count,0), loose(count,0);
    for (u16 id=0;id<count;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        loose[id]=ShellName(name) || NameHas(name,"clip") || NameHas(name,"obojm");
        excluded[id]=model.LL_GetData(id).GetParentID()==BI_NONE || ShellName(name) || AttachmentName(name);
    }
    for (u16 id=trigger; id<count; id=model.LL_GetData(id).GetParentID()) excluded[id]=1;
    // Each bone relative to its parent, in the weapon's frame: its largest
    // move and turn, the frame of the largest move, which way it first moved
    // (5 mm out), and its turn at every frame.
    xr_vector<float> travel(count,0.f), turn(count,0.f);
    xr_vector<Fvector> leave(count,Fvector().set(0.f,0.f,0.f));
    xr_vector<int> peak(count,0);
    xr_vector<xr_vector<float>> turns(count);
    LPCSTR source="";
    m_profile.rest_local.clear();
    m_profile.rest_contents.clear();
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    // The weapon at rest, as its idle shows it, relative to each parent: a
    // magazine's contents are measured, drawn and touched laid out so (a
    // GAMMA RPD's belt links are bound half a metre behind the gun, a stock
    // RPD's standing straight out of the drum). The reloads' first frames
    // are no rest: the GAMMA RPD's empty one starts without its belt. Parts
    // the idle scales (hides) keep their bind place.
    const VrSampledClip* idle=clips.Bones()==count ? clips.Idle() : nullptr;
    if (idle)
    {
        m_profile.rest_local.assign(bind.begin(),bind.begin()+count);
        for (u16 id=0;id<count;++id)
        {
            const u16 parent=model.LL_GetData(id).GetParentID();
            if (parent>=count) continue;
            Fmatrix& rest=m_profile.rest_local[id];
            rest.mul_43(Fmatrix().invert(idle->At(0,parent)),idle->At(0,id));
            const bool unscaled=fsimilar(rest.i.magnitude(),1.f,0.01f) && fsimilar(rest.j.magnitude(),1.f,0.01f) &&
                fsimilar(rest.k.magnitude(),1.f,0.01f);
            if (!unscaled) rest.mul_43(Fmatrix().invert(bind[parent]),bind[id]);
        }
    }
    // Movement is measured from that rest, along the parent's axes there: a
    // part parked in its bind place (a GAMMA M249's jmbt, PKM's rounds) did
    // not move when the reload's first frame shows it on the gun.
    const bool rested=m_profile.rest_local.size()==count;
    xr_vector<Fmatrix> rest_world(count);
    for (u16 id=0;id<count;++id)
    {
        u16 root=id;
        while (model.LL_GetData(root).GetParentID()<count) root=model.LL_GetData(root).GetParentID();
        rest_world[id]=rested ? RestPose(model,bind,m_profile.rest_local,id,root) : bind[id];
    }
    const xr_vector<Fmatrix> bind_local=BindLocal(model,bind);
    LPCSTR key=nullptr;
    if (clips.Bones()==count)
        if (const VrSampledClip* reload=clips.Reload(360,key))
        {
            const int steps=ClipSteps(reload->length,360);
            for (int step=0;step<steps;++step)
            {
                for (u16 id=0;id<count;++id)
                {
                    // Rounds and clips too: only for the clip evidence below.
                    if (excluded[id] && !(loose[id] && model.LL_GetData(id).GetParentID()<count)) continue;
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    const Fmatrix& rest=rested ? m_profile.rest_local[id] : bind_local[id];
                    Fmatrix now;
                    now.mul_43(Fmatrix().invert(reload->At(step,parent)),reload->At(step,id));
                    Fvector moved; moved.sub(now.c,rest.c);
                    rest_world[parent].transform_dir(moved);
                    const float distance=moved.magnitude();
                    if (distance>travel[id]) { travel[id]=distance; peak[id]=step; }
                    if (leave[id].square_magnitude()<0.5f && distance>0.005f) leave[id].div(moved,distance);
                    Fquaternion a, b; a.set(rest); b.set(now);
                    const float dot=_abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);
                    const float angle=2.f*acosf(clampr(dot,0.f,1.f));
                    turns[id].push_back(angle);
                    turn[id]=std::max(turn[id],angle);
                }
            }
            source=key;
        }
    if (!*source) return BI_NONE;
    // A stripper clip or rounds coming in from above (4 cm or more): an
    // internal magazine charged through the open action.
    m_profile.evidence.top_loaded=false;
    // Resting above the trigger, or parked well away from the gun until
    // the reload brings it in (an SKS's clip): a magazine or belt box rests
    // below the receiver (an RPD's box first lifts off its hook, upward).
    const auto rests_above=[&](u16 id)
    {
        Fbox box;
        if (!MeasureBone(model,id,box,true,&m_profile.rest_local,BI_NONE,&bind)) return false;
        Fvector centre; box.getcenter(centre);
        const Fvector at=m_profile.has_trigger_point ? m_profile.trigger_point : Fvector().set(0.f,0.f,0.f);
        if (centre.distance_to(at)>=0.3f) return true;
        return m_profile.has_trigger_point && centre.y>=m_profile.trigger_point.y+0.03f;
    };
    // The magazine: the part that travels furthest (4 cm or more), not
    // straight back along the bore (that is the bolt or charging handle),
    // unless it is named a magazine and rests at the seat (a GAMMA RPD's
    // drum first slides back off its hook). Only the outermost such part:
    // what moves inside it is its contents (a GAMMA PKM's link chain swings
    // further inside its box than the box moves).
    xr_vector<u8> candidate(count,0);
    for (u16 id=0;id<count;++id)
    {
        Fbox box;
        // Not a part that leaves upward: a stripper clip or the rounds of an
        // internal magazine come in from above.
        // Keep cheap rejection ahead of mesh enumeration on first draw.
        if (excluded[id] || travel[id]<0.04f ||
            (leave[id].z<-0.8f && !vr::MagazineBoneName(model.LL_BoneName_dbg(id))) ||
            !MeasureBone(model,id,box,true,&m_profile.rest_local,BI_NONE,&bind)) continue;
        Fvector centre; box.getcenter(centre);
        const bool named_near_seat=m_profile.has_trigger_point &&
            vr::MagazineBoneName(model.LL_BoneName_dbg(id)) && centre.distance_to(m_profile.trigger_point)<0.3f;
        if (!vr::ReloadMagazineMotion(travel[id],leave[id].y,leave[id].z,
            leave[id].y>0.5f && rests_above(id),named_near_seat)) continue;
        candidate[id]=1;
    }
    u16 magazine=BI_NONE;
    for (u16 id=0;id<count;++id)
    {
        if (!candidate[id]) continue;
        bool inside=false;
        for (u16 a=model.LL_GetData(id).GetParentID(); a<count && !inside; a=model.LL_GetData(a).GetParentID())
            inside=candidate[a]!=0;
        if (!inside && (magazine==BI_NONE || travel[id]>travel[magazine])) magazine=id;
    }
    // A parent carried along with it is the magazine, not the child.
    while (magazine!=BI_NONE)
    {
        const u16 parent=model.LL_GetData(magazine).GetParentID();
        if (parent>=count || excluded[parent] || travel[parent]<0.75f*travel[magazine]) break;
        magazine=parent;
    }
    // Select the detachable root first: its belt links/rounds can move up
    // above the trigger during reload without being a stripper clip.
    for (u16 id=0;id<count && !m_profile.evidence.top_loaded;++id)
        if ((!excluded[id] || loose[id]) && travel[id]>=0.04f && travel[id]<=0.6f && leave[id].y>0.5f &&
            vr::ReloadClipMotion(travel[id],leave[id].y,rests_above(id),
            magazine!=BI_NONE && Descends(model,id,magazine)))
        {
            m_profile.evidence.top_loaded=true;
            Msg("* [VR weapon] %s: %s comes in from above in %s (a clip or rounds): internal magazine",
                m_profile.section.c_str(),model.LL_BoneName_dbg(id),source);
        }
    // Without a detachable magazine, also by where the parts go in the
    // receiver's frame: a clip or rounds carried by a group bone (the SKS's
    // mesh-less "zaladunek" with its "lodka" clip and "naboje" rounds) do not
    // move against their parent, and a clip parked off the gun first moves
    // toward it, not upward. A small part whose mesh drops 5 cm or more,
    // mostly straight down, above the trigger and into the action, after
    // coming from 8 cm or more away, is charged through the open action.
    if (!m_profile.evidence.top_loaded && magazine==BI_NONE && m_profile.has_trigger_point && clips.Bones()==count)
        if (const VrSampledClip* reload=clips.Reload(360,key))
        {
            const u16 receiver=trigger<count && model.LL_GetData(trigger).GetParentID()<count ?
                model.LL_GetData(trigger).GetParentID() : model.LL_GetBoneRoot();
            const Fvector& t=m_profile.trigger_point;
            const int steps=ClipSteps(reload->length,360);
            float best=0.f; u16 best_id=BI_NONE;
            for (u16 id=0;id<count && !m_profile.evidence.top_loaded;++id)
            {
                bool chain=false;
                for (u16 a=trigger; a<count && !chain; a=model.LL_GetData(a).GetParentID()) chain=a==id;
                Fbox box;
                if (chain || AttachmentName(model.LL_BoneName_dbg(id)) || !MeasureBone(model,id,box,false)) continue;
                Fvector size; box.getsize(size);
                if (std::max(size.x,std::max(size.y,size.z))>0.12f) continue;
                Fvector centre; box.getcenter(centre);
                Fvector local; Fmatrix().invert(bind[id]).transform_tiny(local,centre);
                Fvector first{}, high{};
                float away=0.f;
                for (int step=0;step<steps && !m_profile.evidence.top_loaded;++step)
                {
                    Fmatrix rel; rel.invert(reload->At(step,receiver)); rel.mulA_43(bind[receiver]);
                    Fvector now; reload->At(step,id).transform_tiny(now,local);
                    rel.transform_tiny(now);
                    if (!step) { first=now; high=now; continue; }
                    away=std::max(away,now.distance_to(first));
                    if (now.y>high.y) high=now;
                    Fvector drop; drop.sub(high,now);
                    const float length=drop.magnitude();
                    best=std::max(best,drop.y); if (best==drop.y) best_id=id;
                    if (away>=0.08f && drop.y>=0.05f && drop.y>=0.7f*length && now.y>=t.y+0.03f &&
                        now.distance_to(t)<0.3f)
                    {
                        m_profile.evidence.top_loaded=true;
                        Msg("* [VR weapon] %s: %s drops %.0f mm into the action in %s (a clip or rounds): internal magazine",
                            m_profile.section.c_str(),model.LL_BoneName_dbg(id),drop.y*1000.f,source);
                    }
                }
            }
            if (!m_profile.evidence.top_loaded && best_id!=BI_NONE)
                Msg("* [VR weapon] %s: no clip comes in from above (largest drop %.0f mm, %s)",
                    m_profile.section.c_str(),best*1000.f,model.LL_BoneName_dbg(best_id));
        }
    if (magazine!=BI_NONE)
    {
        // Into the well: against the way it first left it. Kept only for a
        // magazine found by its motion alone (a drum, a belt box): one the
        // name and shape find takes its axis from its mesh (AutoProfile,
        // MeasureGeometry), as the bone's origin moves sideways or down with
        // any turn of the magazine (an AK's rock-out, a pistol's swing).
        m_profile.well_axis.invert(leave[magazine]);
        m_profile.well_axis.normalize_safe();
        m_profile.explicit_well_axis=m_profile.well_axis.square_magnitude()>0.5f;
        Msg("* [VR weapon] %s: magazine %s from %s: out %.0f mm, well axis (%.2f %.2f %.2f)",
            m_profile.section.c_str(),model.LL_BoneName_dbg(magazine),source,travel[magazine]*1000.f,
            m_profile.well_axis.x,m_profile.well_axis.y,m_profile.well_axis.z);
    }
    // Hinged parts: turned 25 degrees or more about their own origin. Only
    // beside a magazine the animation takes out (a cover over its feed), and
    // never on a shell-loaded gun: a break-action's barrels turn on their
    // hinge too, and InteractShells already works them.
    const bool covers=magazine!=BI_NONE && !m_weapon.VrShellLoader();
    for (u16 id=0;covers && id<count && m_profile.part_count<vr::max_parts;++id)
    {
        Fbox box;
        if (excluded[id] || turn[id]<deg2rad(25.f) || travel[id]>0.03f || !MeasureBone(model,id,box,true)) continue;
        // A cover or lid, not a small lever or latch the animation flicks.
        Fvector size; box.getsize(size);
        if (std::max(size.x,std::max(size.y,size.z))<0.08f) continue;
        if (magazine!=BI_NONE && (Descends(model,id,magazine) || Descends(model,magazine,id))) continue;
        // A child turning with its parent is part of the parent's part.
        const u16 parent=model.LL_GetData(id).GetParentID();
        bool inside=false;
        for (int p=0;p<m_profile.part_count;++p)
            if (model.LL_BoneID(m_profile.part_bones[p].c_str())==parent) inside=true;
        if (inside) continue;
        const int n=m_profile.part_count++;
        vr::PartDef& def=m_profile.parts[n];
        def=vr::PartDef{};
        def.kind=vr::PartHinge;
        def.pivot={bind[id].c.x,bind[id].c.y,bind[id].c.z};
        // Taken at its end farthest from the hinge (a cover's latch).
        Fvector centre; box.getcenter(centre);
        Fvector end_point; end_point.sub(centre,bind[id].c).mul(2.f).add(bind[id].c);
        end_point.x=clampr(end_point.x,box.min.x,box.max.x);
        end_point.y=clampr(end_point.y,box.min.y,box.max.y);
        end_point.z=clampr(end_point.z,box.min.z,box.max.z);
        def.grab={end_point.x,end_point.y,end_point.z};
        def.max=turn[id];
        def.open_at=0.8f*def.max;
        def.closed_at=deg2rad(3.f);
        def.grab_radius=0.07f;
        def.release_distance=0.07f;
        def.max_speed=6.f;
        m_profile.part_bones[n]=model.LL_BoneName_dbg(id);
        m_profile.part_names[n]=model.LL_BoneName_dbg(id);
        m_profile.part_from_animation[n]=true;
        // Open while the magazine comes out: the magazine needs it open,
        // and the weapon fires and chambers only with it closed.
        const bool gates=magazine!=BI_NONE && size_t(peak[magazine])<turns[id].size() &&
            turns[id][size_t(peak[magazine])]>0.5f*turn[id];
        if (gates)
        {
            const bool kept=m_profile.rules.magazine.Add(vr::GatePartOpen,n) &
                m_profile.rules.fire.Add(vr::GatePartClosed,n) & m_profile.rules.chamber.Add(vr::GatePartClosed,n);
            if (!kept) Msg("! [VR weapon] %s: too many reload conditions; %s not required",
                m_profile.section.c_str(),model.LL_BoneName_dbg(id));
        }
        Msg("* [VR weapon] %s: hinged part %s from %s, opens %.0f deg%s",m_profile.section.c_str(),
            model.LL_BoneName_dbg(id),source,rad2deg(turn[id]),gates ? "; open for the magazine" : "");
        // A belt: the longest chain of single links hanging from the
        // magazine whose end reaches under this part.
        if (!gates || m_profile.belt.used) continue;
        u16 end=BI_NONE; int depth=0;
        for (u16 bone=0;bone<count;++bone)
        {
            if (bone==magazine || !Descends(model,bone,magazine)) continue;
            int d=0;
            for (u16 a=bone; a<count && a!=magazine; a=model.LL_GetData(a).GetParentID()) ++d;
            if (d>depth) { depth=d; end=bone; }
        }
        // Where a link rests on the gun: its mesh, or the joint itself (a
        // GAMMA RPD's belt_N joints carry no mesh; their plr_N rounds do).
        const auto link_at=[&](u16 bone)
        {
            Fbox mesh; Fvector at;
            if (MeasureBone(model,bone,mesh,false,&m_profile.rest_local,magazine,&bind)) mesh.getcenter(at);
            else at=m_profile.rest_local.size()==count ?
                RestPose(model,bind,m_profile.rest_local,bone,magazine).c : bind[bone].c;
            return at;
        };
        if (end==BI_NONE || depth<4) continue;
        // 2.5 cm round it: a belt fed from the side (RPD) ends beside the cover.
        Fbox cover=box; cover.grow(0.025f);
        const Fvector tray=link_at(end);
        if (!cover.contains(tray)) continue;
        xr_vector<u16> chain;
        for (u16 a=end; a<count && a!=magazine; a=model.LL_GetData(a).GetParentID()) chain.push_back(a);
        std::reverse(chain.begin(),chain.end());
        const size_t loose=std::max<size_t>(1,chain.size()*3/4-1);
        const Fvector box_end=link_at(chain[loose-1]);
        m_profile.belt.used=true;
        m_profile.belt.tray={tray.x,tray.y,tray.z};
        m_profile.belt.box_end={box_end.x,box_end.y,box_end.z};
        m_profile.belt_free_bone=model.LL_BoneName_dbg(chain[loose]);
        m_profile.belt_chain.clear();
        for (u16 bone:chain) m_profile.belt_chain.push_back(model.LL_BoneName_dbg(bone));
        const bool kept=m_profile.rules.belt.Add(vr::GatePartOpen,n) & m_profile.rules.belt.Add(vr::GateMagazineIn) &
            m_profile.rules.fire.Add(vr::GateBeltSeated) & m_profile.rules.chamber.Add(vr::GateBeltSeated);
        if (!kept) Msg("! [VR weapon] %s: too many reload conditions; the belt is not fully required",
            m_profile.section.c_str());
        Msg("* [VR weapon] %s: belt %s..%s, loose from %s",m_profile.section.c_str(),
            model.LL_BoneName_dbg(chain.front()),model.LL_BoneName_dbg(end),m_profile.belt_free_bone.c_str());
    }
    return magazine;
}

void CVrWeaponInteraction::MeasureCycle(IKinematics& model)
{
    // The weapon's own first-person animations show which part cycles: the
    // bolt or slide in the shot animation, else the charging handle in the
    // empty reload. Play them on a private copy of the HUD model and take the
    // part that travels furthest straight back relative to its parent.
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (bind.size()<count) return;
    xr_vector<u8> excluded(count,0);
    const u16 trigger=FindTrigger(model);
    const u16 magazine=m_profile.mag_bone.size() ? model.LL_BoneID(m_profile.mag_bone.c_str()) : BI_NONE;
    for (u16 id=0;id<count;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        excluded[id]=model.LL_GetData(id).GetParentID()==BI_NONE ||
            m_profile.ReloadMagazineBone(id,count) ||
            (magazine!=BI_NONE && Descends(model,id,magazine)) || ShellName(name) || AttachmentName(name);
    }
    // The trigger and the receiver chain carrying it: a shot animation that
    // kicks the receiver back must not make the whole gun the bolt.
    for (u16 id=trigger; id<count; id=model.LL_GetData(id).GetParentID()) excluded[id]=1;
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    // anm_shot_l is read at 97% of its length, off the 60 Hz grid.
    HudSample& copy=clips.Copy();
    auto* animated=copy.animated;
    auto* sample=copy.sample;
    // How far each part moves straight back relative to its parent, in the
    // shot animation and in the reload: the first of each that moves one.
    xr_vector<float> rear_shot(count,0.f), rear_reload(count,0.f);
    // And how far each turns about the bore (a bolt-action's handle) and
    // across the weapon (a lever) in the shot animation.
    xr_vector<float> turn_bore(count,0.f), turn_side(count,0.f);
    xr_vector<Fmatrix> first_local(count);
    // Each part's way back and turn about the bore at every shot frame: how
    // long it stays back, and whether a handle lifts before it comes back.
    xr_vector<xr_vector<float>> shot_back(count), shot_turn(count);
    const xr_vector<Fmatrix> bind_local=BindLocal(model,bind);
    const auto sample_motion=[&](std::initializer_list<LPCSTR> keys, xr_vector<float>& rear, bool turns) -> LPCSTR
    {
        if (clips.Bones()!=count) return nullptr;
        for (LPCSTR key:keys)
        {
            const VrSampledClip* clip=clips.Clip(key,240);
            if (!clip) continue;
            std::fill(rear.begin(),rear.end(),0.f);
            if (turns)
            {
                std::fill(turn_bore.begin(),turn_bore.end(),0.f);
                std::fill(turn_side.begin(),turn_side.end(),0.f);
                for (auto& frames:shot_back) frames.clear();
                for (auto& frames:shot_turn) frames.clear();
            }
            // One play of the clip: a cycling clip repeated would count its
            // time back twice.
            const int frames=ClipSteps(clip->length,240);
            for (int step=0;step<frames;++step)
            {
                for (u16 id=0;id<count;++id)
                {
                    if (excluded[id]) continue;
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    Fmatrix to_parent; to_parent.invert(clip->At(step,parent));
                    Fvector now; to_parent.transform_tiny(now,clip->At(step,id).c);
                    const Fvector& rest=bind_local[id].c;
                    // Travel in the weapon's frame, using the parent's bind axes.
                    Fvector moved; moved.sub(now,rest);
                    bind[parent].transform_dir(moved);
                    const float back=-moved.z;
                    const bool straight=back>0.f && back>2.f*_sqrt(moved.x*moved.x+moved.y*moved.y);
                    if (straight)
                        rear[id]=std::max(rear[id],back);
                    if (turns)
                    {
                        shot_back[id].push_back(straight ? back : 0.f);
                        Fmatrix now_local;
                        now_local.mul_43(to_parent,clip->At(step,id));
                        if (!step) first_local[id]=now_local;
                        const float bore=vr::TurnAbout(first_local[id],now_local,bind[parent],2);
                        shot_turn[id].push_back(bore);
                        turn_bore[id]=std::max(turn_bore[id],bore);
                        turn_side[id]=std::max(turn_side[id],vr::TurnAbout(first_local[id],now_local,bind[parent],0));
                    }
                }
            }
            for (u16 id=0;id<count;++id)
                if (rear[id]>=0.01f) return key;
        }
        return nullptr;
    };
    LPCSTR shot_source=sample_motion({"anm_shots","anm_shot"},rear_shot,true);
    LPCSTR reload_source=sample_motion({"anm_reload_empty","anm_reload"},rear_reload,false);
    const auto best=[&](const xr_vector<float>& rear, auto accept)
    {
        u16 found=BI_NONE; float most=0.f;
        for (u16 id=0;id<count;++id)
        {
            Fbox box;
            if (rear[id]>most && rear[id]>=0.01f && rear[id]<=0.2f && accept(id) && MeasureBone(model,id,box,true))
            { found=id; most=rear[id]; }
        }
        return found;
    };
    const auto any=[](u16) { return true; };
    // The bolt or slide: what the shot cycles (else what the reload pulls).
    u16 chosen=best(rear_shot,any);
    LPCSTR source=shot_source ? shot_source : "";
    m_profile.cycle_from_reload=false;
    if (chosen==BI_NONE)
    {
        chosen=best(rear_reload,any); source=reload_source ? reload_source : "";
        // Only a handle stays still under fire: a bolt the shots do not
        // animate (an MP5's zatvor) still cycles.
        LPCSTR part=chosen!=BI_NONE ? model.LL_BoneName_dbg(chosen) : "";
        m_profile.cycle_from_reload=chosen!=BI_NONE && !NameHas(part,"zatvor") && !NameHas(part,"bolt") &&
            !NameHas(part,"carrier");
    }
    // A charging handle of its own: a part the reload pulls straight back
    // that the shots leave where it is (an MP5's on its front tube, a P90's
    // under the receiver, an AR-15's T). An AK's handle rides its carrier
    // and is found as the carrier.
    u16 handle=BI_NONE;
    if (chosen!=BI_NONE && shot_source)
        handle=best(rear_reload,[&](u16 id)
        {
            return id!=chosen && rear_shot[id]<0.3f*rear_reload[id] &&
                !Descends(model,id,chosen) && !Descends(model,chosen,id);
        });
    // Not shown by the animations (an AR-15's reload drops the bolt with its
    // release, never pulling the handle): a part named as a charging handle.
    if (handle==BI_NONE)
        for (u16 id=0;id<count && handle==BI_NONE;++id)
        {
            LPCSTR name=model.LL_BoneName_dbg(id);
            Fbox box;
            if (excluded[id] || id==chosen || !MeasureBone(model,id,box,true)) continue;
            // Not a bare "handle": that also names carry handles.
            if (NameHas(name,"charg") || NameHas(name,"cocking") || NameHas(name,"rukoyat"))
            {
                handle=id;
                if (chosen!=BI_NONE && (Descends(model,id,chosen) || Descends(model,chosen,id))) handle=BI_NONE;
                if (rear_reload[id]<0.01f) rear_reload[id]=0.07f;
            }
        }
    if (chosen==BI_NONE && handle!=BI_NONE) { chosen=handle; handle=BI_NONE; source="its name"; }
    float travel=chosen!=BI_NONE ? std::max(rear_shot[chosen],rear_reload[chosen]) : 0.f;
    // Mechanics evidence (VrWeaponMechanics): how far the cycling part, a
    // handle on it or the part carrying it turns about the bore over the
    // shots (a bolt-action's handle lifts ~90 degrees; a self-loader's
    // carrier only slides), a lever swinging below the receiver, and
    // whether the last shot leaves the action back (holds open).
    vr::MechanicsEvidence& evidence=m_profile.evidence;
    evidence.cycling_part=chosen!=BI_NONE;
    evidence.separate_handle=handle!=BI_NONE;
    evidence.bolt_turn_deg=0.f;
    evidence.lever_part=evidence.has_shot_l=evidence.shot_l_holds_back=false;
    m_profile.action_path=vr::ActionPath();
    m_profile.path_bones.clear(); m_profile.path_poses.clear();
    const float centre_x=m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f;
    const u16 trigger_parent=trigger!=BI_NONE ? model.LL_GetData(trigger).GetParentID() : BI_NONE;
    const u16 receiver=trigger_parent<count ? trigger_parent : (trigger!=BI_NONE ? trigger : u16(0));
    u16 turner=BI_NONE, lever=BI_NONE;
    if (chosen!=BI_NONE)
    {
        float turned=0.f;
        for (u16 id=0;id<count;++id)
            if (!excluded[id] && turn_bore[id]>turned && (Descends(model,id,chosen) || Descends(model,chosen,id)))
            { turned=turn_bore[id]; turner=id; }
        evidence.bolt_turn_deg=rad2deg(turned);
        // A lever: a part below the trigger that the shot swings 30 degrees
        // or more across the weapon (the Winchester's loop).
        float swung=deg2rad(30.f);
        for (u16 id=0;id<count;++id)
            if (!excluded[id] && turn_side[id]>swung && m_profile.has_trigger_point &&
                bind[id].c.y<m_profile.trigger_point.y+0.01f && !Descends(model,id,chosen))
            { swung=turn_side[id]; lever=id; }
        evidence.lever_part=lever!=BI_NONE && evidence.bolt_turn_deg<vr::bolt_action_turn_deg;
    }
    // Back half its stroke or more for at most 0.15 s of the shot: the bolt
    // cycles by itself. A hand-worked bolt or lever stays back while the
    // hand works it (0.4 s or more).
    evidence.fast_return=false;
    if (chosen!=BI_NONE && shot_source && rear_shot[chosen]>=0.01f)
    {
        unsigned back_frames=0;
        for (float back:shot_back[chosen]) back_frames+=back>=0.5f*rear_shot[chosen];
        evidence.fast_return=vr::FastReturn(float(back_frames)/60.f);
    }
    evidence.turn_leads=false;
    if (chosen!=BI_NONE && turner!=BI_NONE && shot_source && shot_turn[turner].size()==shot_back[chosen].size())
        evidence.turn_leads=vr::TurnLeadsStroke(shot_turn[turner].data(),shot_back[chosen].data(),
            shot_back[chosen].size(),turn_bore[turner],rear_shot[chosen]);
    if (chosen!=BI_NONE && animated && sample && sample->LL_BoneCount()==count)
    {
        float length=0.f;
        evidence.has_shot_l=PlayHudMotion(*animated,hud,"anm_shot_l",&length);
        if (evidence.has_shot_l && length>0.f)
        {
            animated->LL_UpdateTracks(0.f,true,true);
            copy.Evaluate(length*0.97f);
            const float full=std::max(rear_shot[chosen],rear_reload[chosen]);
            evidence.shot_l_holds_back=full>0.005f && RearInReceiver(*sample,bind,chosen,receiver)>=0.6f*full;
        }
        // Worked by hand along the path its own shot animation moves it.
        if (shot_source && evidence.bolt_turn_deg>=vr::bolt_action_turn_deg)
        {
            // The knob rides the deepest of the turning part and the bolt.
            const u16 knob=turner!=BI_NONE && Descends(model,turner,chosen) ? turner : chosen;
            RecordActionPath(clips,model,bind,shot_source,knob,GrabPoint(model,knob,false,centre_x),receiver,chosen,true);
        }
        else if (shot_source && evidence.lever_part)
            RecordActionPath(clips,model,bind,shot_source,lever,LowestPoint(model,lever),receiver,chosen,false);
        // A dust cover: hinged beside the ejection port, swung open by the
        // shots and left open (its pose at the end of the shot).
        m_profile.dust_cover_bone="";
        evidence.dust_cover_part=false;
        if (shot_source && m_profile.shell_point.square_magnitude()>EPS_S)
            for (u16 id=0;id<count && !evidence.dust_cover_part;++id)
            {
                if (excluded[id] || Descends(model,id,chosen) || Descends(model,chosen,id) || id==lever) continue;
                if (turn_side[id]<deg2rad(30.f) && turn_bore[id]<deg2rad(30.f)) continue;
                Fbox box;
                if (!MeasureBone(model,id,box,true)) continue;
                Fvector centre; box.getcenter(centre);
                if (centre.distance_to(m_profile.shell_point)>0.06f) continue;
                const VrSampledClip* clip=clips.Frames(shot_source,120);
                if (!clip) break;
                // Its widest swing over the shot is its open pose.
                float widest=-1.f; Fmatrix open=bind[id];
                for (int step=0;step<120;++step)
                {
                    Fmatrix rel; rel.invert(clip->At(step,receiver)); rel.mulA_43(bind[receiver]);
                    Fmatrix pose; pose.mul_43(rel,clip->At(step,id));
                    const float off=pose.k.distance_to(bind[id].k)+pose.j.distance_to(bind[id].j);
                    if (off>widest) { widest=off; open=pose; }
                }
                evidence.dust_cover_part=true;
                m_profile.dust_cover_bone=model.LL_BoneName_dbg(id);
                m_profile.dust_cover_open=open;
                m_profile.dust_cover_grab=centre;
            }
    }
    if (chosen==BI_NONE)
    {
        // No moving part in the animations: still a closed bolt. A charging
        // point on the receiver above the trigger cycles it (no visible part).
        if (m_weapon.iMagazineSize<2 || m_profile.tube || !m_profile.has_trigger_point) return;
        m_profile.cycle_bone="";
        m_profile.cycle_travel=0.04f;
        // On the receiver's right side ahead of the trigger, where most such
        // guns (PK, RPD, AK family) carry it: the mesh's right extent there.
        m_profile.cycle_grab=m_profile.trigger_point;
        m_profile.cycle_grab.y+=0.03f;
        m_profile.cycle_grab.z+=0.08f;
        {
            xr_vector<Fvector> points;
            PointCollector collector(points);
            for (u16 id=0;id<count;++id) model.EnumBoneVertices(collector,id);
            const Fvector& t=m_profile.trigger_point;
            float right=-flt_max;
            for (const Fvector& p:points)
                if (p.z>t.z+0.03f && p.z<t.z+0.15f && p.y>t.y && p.y<t.y+0.07f) right=std::max(right,p.x);
            m_profile.cycle_grab.x=right>-flt_max ? right+0.015f : t.x+0.04f;
        }
        m_profile.grab_radius=0.08f;
        m_profile.travel_limit=m_profile.cycle_travel;
        m_profile.chamber=true;
        Msg("* [VR weapon] %s: no cycling part found; charging point on the right ahead of the trigger (%.3f %.3f %.3f)",
            m_profile.section.c_str(),m_profile.cycle_grab.x,m_profile.cycle_grab.y,m_profile.cycle_grab.z);
        return;
    }
    m_profile.cycle_bone=model.LL_BoneName_dbg(chosen);
    m_profile.handle_bone=handle!=BI_NONE ? model.LL_BoneName_dbg(handle) : "";
    m_profile.bolt_travel=0.f;
    if (handle!=BI_NONE)
    {
        m_profile.bolt_travel=clampr(std::max(rear_shot[chosen],rear_reload[chosen]),0.01f,0.15f);
        travel=rear_reload[handle];
    }
    m_profile.cycle_travel=clampr(travel,0.01f,0.15f);
    m_profile.cycle_grab=GrabPoint(model,handle!=BI_NONE ? handle : chosen,Handgun(),centre_x);
    m_profile.grab_radius=0.07f;
    m_profile.check_cycle_bind=false;
    m_profile.travel_limit=m_profile.cycle_travel;
    // A magazine weapon with a working bolt chambers like the audited ones:
    // a fresh magazine (or a reload) in an empty gun needs the bolt pulled.
    m_profile.chamber=true;
    // Holding open on the last round, a slide stop or bolt catch, and a
    // manually worked action come from the mechanics classification
    // (ClassifyMechanics), never from the rate of fire.
    Msg("* [VR weapon] %s: cycling part %s, %.0f mm travel (from %s); %s%s, grabbed at (%.3f %.3f %.3f)",
        m_profile.section.c_str(),m_profile.cycle_bone.c_str(),m_profile.cycle_travel*1000.f,source,
        handle!=BI_NONE ? "charging handle " : "racked by the same part",
        handle!=BI_NONE ? m_profile.handle_bone.c_str() : "",
        m_profile.cycle_grab.x,m_profile.cycle_grab.y,m_profile.cycle_grab.z);
}

void CVrWeaponInteraction::MeasureThrownParts(IKinematics& model)
{
    // A hand-worked gun's shot animation works the action itself and throws
    // the spent case (the stock Remington 870's gilza flies 0.5 m out). The
    // hand works it in VR, which ejects its own case; copied from the native
    // animation, a case flew from every shot. A part the shot takes 15 cm or
    // more from its place in the receiver is such a part. Its place is where
    // the shot's first frame has it, not its bind place: a clip or rounds
    // bound away from the gun and laid on it by the animation stay put.
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    const u16 trigger=FindTrigger(model);
    const u16 receiver=trigger<count && model.LL_GetData(trigger).GetParentID()<count ?
        model.LL_GetData(trigger).GetParentID() : model.LL_GetBoneRoot();
    if (bind.size()<count || receiver>=count) return;
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    xr_vector<u8> thrown(count,0);
    xr_vector<Fvector> first(count);
    // Never the parts the hand works (a long bolt's knob, a lever's loop).
    xr_vector<u8> worked(count,0);
    for (const shared_str* part:{&m_profile.cycle_bone,&m_profile.pump_bone,&m_profile.handle_bone})
    {
        const u16 bone=part->size() ? model.LL_BoneID(part->c_str()) : u16(BI_NONE);
        for (u16 id=0;bone<count && id<count;++id)
            if (Descends(model,id,bone)) worked[id]=1;
    }
    bool any=false;
    if (clips.Bones()==count)
        for (LPCSTR key:{"anm_shots","anm_shot"})
        {
            const VrSampledClip* clip=clips.Clip(key,120);
            if (!clip) continue;
            const int steps=ClipSteps(clip->length,120);
            for (int step=0;step<steps;++step)
            {
                Fmatrix rel; rel.invert(clip->At(step,receiver)); rel.mulA_43(bind[receiver]);
                for (u16 id=0;id<count;++id)
                {
                    if (thrown[id] || worked[id] || Descends(model,receiver,id)) continue;
                    Fvector now; rel.transform_tiny(now,clip->At(step,id).c);
                    if (!step) first[id]=now;
                    else if (now.distance_to(first[id])>=0.15f) { thrown[id]=1; any=true; }
                }
            }
            break;
        }
    if (!any) return;
    m_profile.thrown_mask.swap(thrown);
    for (u16 id=0;id<count;++id)
    {
        if (!m_profile.thrown_mask[id] || (model.LL_GetData(id).GetParentID()<count &&
            m_profile.thrown_mask[model.LL_GetData(id).GetParentID()])) continue;
        // DropCase throws it from its bind place: only a case bound where
        // the shot starts it (in the chamber), else the shell particles.
        Fbox box;
        if (!m_profile.case_bone.size() && first[id].distance_to(bind[id].c)<0.03f && MeasureBone(model,id,box,true))
        {
            m_profile.case_bone=model.LL_BoneName_dbg(id);
            box.getcenter(m_profile.case_center);
            box.getsize(m_profile.case_half);
            m_profile.case_half.mul(0.5f);
        }
        Msg("* [VR weapon] %s: %s thrown by the shot animation; kept in place while worked by hand",
            m_profile.section.c_str(),model.LL_BoneName_dbg(id));
    }
}

void CVrWeaponInteraction::RecordActionPath(VrClipSampler& clips, IKinematics& model,
    const xr_vector<Fmatrix>& bind, LPCSTR key, u16 knob, const Fvector& grab, u16 receiver, u16 moving_bolt, bool knob_rear)
{
    const u16 count=model.LL_BoneCount();
    const VrSampledClip* clip=clips.Clip(key,240);
    if (!clip || clip->bones!=count) return;
    const int steps=std::max(2,std::min(240,int(clip->length*60.f)+1));
    clip=clips.Frames(key,steps);
    if (!clip) return;
    Fmatrix unbind; unbind.invert(bind[knob]);
    Fvector grab_local; unbind.transform_tiny(grab_local,grab);
    // Every bone's model-space pose in the receiver's frame, per step.
    xr_vector<Fvector> grabs; xr_vector<float> rears; xr_vector<Fmatrix> frames;
    grabs.reserve(steps); rears.reserve(steps); frames.reserve(size_t(steps)*count);
    for (int step=0;step<steps;++step)
    {
        Fmatrix rel; rel.invert(clip->At(step,receiver)); rel.mulA_43(bind[receiver]);
        Fmatrix at; at.mul_43(rel,clip->At(step,knob));
        Fvector g; at.transform_tiny(g,grab_local);
        grabs.push_back(g);
        // Back along the bore: the knob's own travel for a bolt (its lift
        // turns about the bore and moves nothing back); the bolt's for a
        // lever, whose knob swings down.
        rears.push_back(knob_rear ? std::max(0.f,grab.z-g.z) :
            (moving_bolt<count ? std::max(0.f,RearInReceiver(clip->At(step,receiver),clip->At(step,moving_bolt),
                bind,moving_bolt,receiver)) : 0.f));
        for (u16 id=0;id<count;++id) { Fmatrix pose; pose.mul_43(rel,clip->At(step,id)); frames.push_back(pose); }
    }
    // From the knob's first movement to the action fully worked (the bolt
    // furthest back, or a lever's widest swing).
    size_t start=0, end=0;
    while (start<grabs.size() && grabs[start].distance_to(grab)<0.002f) ++start;
    float furthest=0.f;
    for (size_t i=0;i<grabs.size();++i)
    {
        const float measure=moving_bolt<count && rears[i]>0.003f ? rears[i]+1.f : grabs[i].distance_to(grab);
        if (measure>furthest+1e-4f) { furthest=measure; end=i; }
    }
    if (start>=grabs.size() || end<=start) return;
    // The bones that move on the path (the bolt, its handle, a lever and
    // what it drives); all the rest keep their place.
    xr_vector<u16> moving;
    for (u16 id=0;id<count;++id)
        for (size_t i=start;i<=end;++i)
        {
            const Fmatrix& p=frames[i*count+id];
            if (p.c.distance_to(bind[id].c)>0.001f || p.k.distance_to(bind[id].k)>0.01f || p.j.distance_to(bind[id].j)>0.01f)
            { moving.push_back(id); break; }
        }
    if (moving.empty()) return;
    // Up to 64 keys: the closed rest pose, then the worked span.
    xr_vector<size_t> keys;
    const size_t span=end-start+1, stride=std::max<size_t>(1,(span+62)/63);
    for (size_t i=start;i<end;i+=stride) keys.push_back(i);
    keys.push_back(end);
    std::vector<vr::V3> points; std::vector<float> backs;
    points.push_back(vr::V3(grab.x,grab.y,grab.z)); backs.push_back(0.f);
    m_profile.path_bones=moving;
    m_profile.path_poses.clear();
    for (u16 id:moving) m_profile.path_poses.push_back(bind[id]);
    for (size_t i:keys)
    {
        points.push_back(vr::V3(grabs[i].x,grabs[i].y,grabs[i].z)); backs.push_back(rears[i]);
        for (u16 id:moving) m_profile.path_poses.push_back(frames[i*count+id]);
    }
    m_profile.action_path.Build(points,backs);
}

float CVrWeaponInteraction::CycleGrabTravel() const
{
    // How far back the part the hand takes is now: a slide or bolt with the
    // action, a separate handle by itself (it stays home while the bolt is
    // locked). A worked path already follows its knob.
    if (m_profile.manual_cycle && m_profile.action_path.Valid()) return 0.f;
    return m_profile.handle_bone.size() || m_profile.cycle_from_reload ? m_handle_slide : m_mechanism.slide;
}
Fvector CVrWeaponInteraction::CycleGrabLocal() const
{
    if (!m_profile.manual_cycle || !m_profile.action_path.Valid())
    {
        // The reload's hand on the handle, when the mesh's point is 6 cm or
        // more from it (a bullpup's bolt in the stock, a handle the bolt
        // mesh does not show). That hand may be nowhere near the part (a
        // pistol's slide runs home by its stop), so it is only a second
        // place to take it: the nearer of the two (m_cycle_use_hand).
        // Sampled by RefreshAuthoredHands, never here.
        if (m_cycle_use_hand && m_handle_hand_valid && m_handle_hand.distance_to(m_profile.cycle_grab)>=0.06f)
            return m_handle_hand;
        return m_profile.cycle_grab;
    }
    const vr::V3 knob=m_profile.action_path.GrabAt(m_action_u);
    return Fvector().set(knob.x,knob.y,knob.z);
}

bool CVrWeaponInteraction::PoseActionPath(IKinematics& model, xr_vector<Fmatrix>& poses) const
{
    bool posed=false;
    const vr::ActionPath& path=m_profile.action_path;
    const size_t bones=m_profile.path_bones.size();
    if (m_profile.manual_cycle && path.Valid() && bones && m_action_u>0.f &&
        m_profile.path_poses.size()==path.grab.size()*bones)
    {
        size_t key; float t;
        path.Locate(m_action_u,key,t);
        for (size_t b=0;b<bones;++b)
        {
            const u16 id=m_profile.path_bones[b];
            if (id>=poses.size()) continue;
            const Fmatrix& a=m_profile.path_poses[key*bones+b];
            const Fmatrix& c=m_profile.path_poses[(key+1)*bones+b];
            Fquaternion qa, qc, q; qa.set(a); qc.set(c); q.slerp(qa,qc,t);
            Fmatrix pose; pose.rotation(q);
            pose.c.lerp(a.c,c.c,t);
            poses[id]=pose;
        }
        posed=true;
    }
    if (m_dust_open && m_profile.dust_cover_bone.size())
    {
        const u16 id=model.LL_BoneID(m_profile.dust_cover_bone.c_str());
        if (id<poses.size()) { poses[id]=m_profile.dust_cover_open; posed=true; }
    }
    return posed;
}

namespace vrwi
{
// configs\vr\vr_weapon_mechanics.ltx, read once; null when not installed.
CInifile* VrMechanicsOverrides()
{
    static CInifile* overrides=nullptr;
    static bool loaded=false;
    if (!loaded)
    {
        loaded=true;
        string_path path;
        if (FS.exist(path,"$game_config$","vr\\vr_weapon_mechanics.ltx")) overrides=xr_new<CInifile>(path,TRUE,TRUE,FALSE);
        else Msg("! [VR mechanics] configs\\vr\\vr_weapon_mechanics.ltx is not installed: no bolt catches or hold-open from it");
        // Sections no game section is named by or trimmed to (OverrideNames):
        // a typo, or a weapon pack that is not installed. Said once, in one line.
        if (overrides)
        {
            xr_set<shared_str> used;
            char names[8][64];
            for (const CInifile::Sect& section:pSettings->sections())
            {
                const int n=vr::OverrideNames(section.Name.c_str(),names,8);
                for (int i=0;i<n;++i)
                    if (overrides->section_exist(names[i])) used.insert(shared_str(names[i]));
            }
            xr_string unused;
            unsigned count=0;
            for (const CInifile::Sect& section:overrides->sections())
                if (xr_strcmp(section.Name.c_str(),"vr_derivation") && !used.count(section.Name))
                {
                    unused.append(count++ ? ", " : "").append(section.Name.c_str());
                }
            if (count)
                Msg("* [VR mechanics] vr_weapon_mechanics.ltx: %u sections name no section of this game "
                    "(a typo, or a weapon pack not installed): %s",count,unused.c_str());
        }
    }
    return overrides;
}
xr_vector<VrLooseRound> DefaultLooseRounds()
{
    xr_vector<VrLooseRound> rounds;
    rounds.push_back({"7.62x39","vr\\round_762x39.ogf",0.028f});
    return rounds;
}
void ReadNameList(CInifile& ini, LPCSTR key, xr_vector<xr_string>& list)
{
    if (!ini.line_exist("vr_derivation",key)) return;
    LPCSTR value=ini.r_string("vr_derivation",key);
    list.clear();
    for (int i=0,n=value ? _GetItemCount(value) : 0;i<n;++i)
    {
        xr_string item;
        _GetItem(value,i,item);
        if (!item.empty()) list.push_back(item);
    }
}
const VrNameTables& VrNames()
{
    static VrNameTables names;
    static bool loaded=false;
    if (loaded) return names;
    loaded=true;
    CInifile* ini=VrMechanicsOverrides();
    if (!ini || !ini->section_exist("vr_derivation")) return names;
    ReadNameList(*ini,"spent_case_bones",names.spent_case_bones);
    ReadNameList(*ini,"red_dot_shaders",names.red_dot_shaders);
    ReadNameList(*ini,"red_dot_textures",names.red_dot_textures);
    ReadNameList(*ini,"soft_lens_shaders",names.soft_lens_shaders);
    ReadNameList(*ini,"glass_shaders",names.glass_shaders);
    ReadNameList(*ini,"glass_textures",names.glass_textures);
    if (ini->line_exist("vr_derivation","loose_rounds"))
    {
        xr_vector<xr_string> entries;
        ReadNameList(*ini,"loose_rounds",entries);
        names.loose_rounds.clear();
        for (const xr_string& entry:entries)
        {
            // ammunition : mesh : half length (m)
            VrLooseRound round;
            xr_string length;
            _GetItem(entry.c_str(),0,round.ammo,':');
            _GetItem(entry.c_str(),1,round.visual,':');
            _GetItem(entry.c_str(),2,length,':');
            if (length.size()) round.half_length=float(atof(length.c_str()));
            if (round.ammo.empty() || round.visual.empty() || round.half_length<=0.f)
                Msg("! [VR mechanics] [vr_derivation] loose_rounds: bad entry %s",entry.c_str());
            else names.loose_rounds.push_back(round);
        }
    }
    return names;
}
// Any of the parts in a bone or section name, ignoring case (NameHas).
bool NameHasAny(LPCSTR name, const xr_vector<xr_string>& parts)
{
    for (const xr_string& part:parts) if (NameHas(name,part.c_str())) return true;
    return false;
}
// Any of the parts in a shader or texture name, exactly.
bool ContainsAny(LPCSTR text, const xr_vector<xr_string>& parts)
{
    for (const xr_string& part:parts) if (strstr(text,part.c_str())) return true;
    return false;
}
}
void CVrWeaponInteraction::ClassifyMechanics(IKinematics& model)
{
    vr::MechanicsEvidence& e=m_profile.evidence;
    // The native active list, including installed fire-mode upgrades (they
    // fill it without marking the weapon as having modes); the selected mode
    // alone and the base config can both be misleading.
    e.automatic_mode=false;
    if (auto* weapon=m_weapon.cast_weapon_magazined())
        for (s8 mode:weapon->m_aFireModes)
            e.automatic_mode=e.automatic_mode || vr::HasAutomaticFireMode(mode);
    e.pistol_class=PistolHandgun();
    e.revolver_class=smart_cast<CWeaponRevolver*>(&m_weapon)!=nullptr;
    // weapon_class is the weapon's own key (m_profile.section is VR's name).
    const shared_str& own=m_weapon.cNameSect();
    LPCSTR weapon_class=READ_IF_EXISTS(pSettings,r_string,own,"weapon_class",nullptr);
    e.shotgun_class=m_weapon.VrShellLoader() && (!weapon_class || !xr_strcmp(weapon_class,"shotgun"));
    e.bm16_class=smart_cast<CWeaponBM16*>(&m_weapon)!=nullptr;
    e.launcher_class=smart_cast<CWeaponRG6*>(&m_weapon)!=nullptr || smart_cast<CWeaponRPG7*>(&m_weapon)!=nullptr;
    LPCSTR hud=m_weapon.HudSection().c_str();
    e.tri_state=m_weapon.IsTriStateReload() && pSettings->line_exist(hud,"anm_open") &&
        pSettings->line_exist(hud,"anm_add_cartridge") && pSettings->line_exist(hud,"anm_close");
    e.smg_class=weapon_class && !xr_strcmp(weapon_class,"smg");
    e.detachable_magazine=m_profile.detachable;
    e.belt=m_profile.belt.used;
    e.pump_part=m_profile.pump_bone.size()!=0;
    e.break_hinge=m_profile.break_action;
    e.capacity=unsigned(std::max(0,m_weapon.iMagazineSize));
    e.barrels=m_profile.break_action ? (m_profile.shell_bones[0]!=m_profile.shell_bones[1] ? 2u : 1u) : 0u;
    m_mechanics=vr::Classify(e);
    // Overrides from configs\vr\vr_weapon_mechanics.ltx: every section that
    // names the weapon or its parent (vr::OverrideLayers), the more specific
    // winning key by key.
    CInifile* overrides=VrMechanicsOverrides();
    const shared_str parent=READ_IF_EXISTS(pSettings,r_string,own,"parent_section",own);
    char layers[16][64];
    const int layer_count=overrides ? vr::OverrideLayers(own.c_str(),parent.c_str(),
        [&](LPCSTR name) { return !!overrides->section_exist(name); },layers,16) : 0;
    // The keys set, for checking below that each one took effect.
    xr_vector<xr_string> applied;
    string512 sources="";
    for (int layer=0;layer<layer_count;++layer)
    {
        xr_strcat(sources,layer ? " < " : "");
        xr_strcat(sources,layers[layer]);
        for (const auto& item:overrides->r_section(layers[layer]).Data)
        {
            LPCSTR key=item.first.c_str(), value=item.second.c_str();
            const char* why=nullptr;
            if (vr::ApplyMechanicsOverride(m_mechanics,key,value,&why))
            {
                if (std::find(applied.begin(),applied.end(),key)==applied.end()) applied.push_back(key);
            }
            else Msg("! [VR mechanics] [%s] %s = %s not applied: %s",layers[layer],key,value,why);
        }
    }
    if (layer_count) Msg("* [VR mechanics] %s: overrides from [%s]",own.c_str(),sources);
    const auto overridden=[&](LPCSTR key) { return std::find(applied.begin(),applied.end(),key)!=applied.end(); };
    // Not loaded in VR (yet): a revolver's cylinder, a muzzle-loaded
    // launcher and a weapon without a feed use the game's own reload.
    vr::SettleMechanics(m_mechanics);
    // An audited profile (configs\vr\vr_weapons.ltx) keeps its own mechanism.
    if (!m_profile.derived)
    {
        Msg("* [VR mechanics] %s: audited profile; %s",m_profile.section.c_str(),vr::FamilyName(m_mechanics.family));
        for (const xr_string& key:applied)
            if (!vr::AuditedMechanicsKey(key.c_str()))
                Msg("! [VR mechanics] %s: %s not applied: an audited profile keeps its own mechanism",
                    own.c_str(),key.c_str());
        return;
    }
    // The profile follows the classification.
    // A launcher has no bolt to charge: it fires whenever the game's own
    // reload has put a round in it. A derived charging point stalled the
    // RG-6's drum after each shot, and racking it took a grenade out while
    // its rocket object stayed attached.
    if (m_mechanics.action==vr::ActionKind::Launcher)
    {
        m_profile.chamber=false;
        m_profile.cycle_travel=0.f;
        m_profile.travel_limit=0.f;
    }
    m_profile.manual_cycle=m_mechanics.action==vr::ActionKind::ManualBolt || m_mechanics.action==vr::ActionKind::Lever;
    if (!m_profile.manual_cycle) m_profile.action_path=vr::ActionPath();
    if (vr::ApplyManualActionPath(m_mechanics,m_profile.action_path,m_profile.cycle_grab,m_profile.cycle_travel))
    {
        m_profile.travel_limit=m_profile.cycle_travel;
        Msg("* [VR weapon] %s: worked along its shot animation: %.0f deg turn, %u keys, %.0f mm path, %.0f mm back",
            m_profile.section.c_str(),e.bolt_turn_deg,unsigned(m_profile.action_path.grab.size()),
            m_profile.action_path.length*1000.f,m_profile.action_path.RearMax()*1000.f);
    }
    m_profile.slide_stop=m_mechanics.has_slide_stop || m_mechanics.has_bolt_catch;
    m_profile.locks_on_empty=m_mechanics.supports_last_round_hold_open;
    if (!m_mechanics.has_dust_cover) m_profile.dust_cover_bone="";
    // A bolt-action's internal magazine loads a round at a time through
    // the open action, at the ejection port.
    m_profile.internal_rounds=m_mechanics.feed==vr::FeedKind::InternalRounds && m_mechanics.vr_reload_supported &&
        m_profile.cycle_bone.size();
    // Chosen again from the current ammunition (a caliber upgrade).
    m_profile.loose_round_visual=nullptr;
    if (m_profile.internal_rounds)
    {
        m_profile.loading_port=m_profile.shell_point.square_magnitude()>EPS_S ? m_profile.shell_point :
            m_profile.cycle_grab;
        // A hand-worked action: the model's own round in the chamber. A
        // config's shell point assumes a gun centred on its origin.
        if (m_profile.manual_cycle && m_profile.has_trigger_point)
        {
            float nearest=0.3f;
            for (u16 id=0;id<model.LL_BoneCount();++id)
            {
                Fbox box; Fvector centre;
                if (!ShellName(model.LL_BoneName_dbg(id)) || !MeasureBone(model,id,box,false)) continue;
                box.getcenter(centre);
                const float away=centre.distance_to(m_profile.trigger_point);
                if (away<nearest) { nearest=away; m_profile.loading_port=centre; }
            }
        }
        m_profile.chamber=true;
        // The spent-case fallback below is chosen again too: kept, it hid
        // the 7.62x39 round after a caliber change.
        const VrNameTables& names=VrNames();
        if (NameHasAny(m_profile.shell_bones[1].c_str(),names.spent_case_bones)) m_profile.shell_bones[1]="";
        // A round of the model's own (a cartridge bone) to show in the hand.
        for (u16 id=0;id<model.LL_BoneCount() && !m_profile.shell_bones[1].size();++id)
        {
            LPCSTR name=model.LL_BoneName_dbg(id);
            Fbox box;
            if (!ShellName(name) || NameHasAny(name,names.spent_case_bones) || !MeasureBone(model,id,box,true)) continue;
            m_profile.shell_bones[1]=name;
            box.getcenter(m_profile.shell_center[1]);
            m_profile.shell_box_half.set(0.006f,0.006f,0.04f);
        }
        // Live rounds sharing a clip bone (the SKS's): one extracted live
        // cartridge for the ammunition (loose_rounds), rather than the
        // spent-case bone.
        string_path round_path;
        if (!m_profile.shell_bones[1].size())
            for (const VrLooseRound& round:names.loose_rounds)
                if (std::any_of(m_weapon.m_ammoTypes.begin(),m_weapon.m_ammoTypes.end(),
                        [&](const shared_str& ammo) { return NameHas(ammo.c_str(),round.ammo.c_str()); }) &&
                    FS.exist(round_path,"$game_meshes$",round.visual.c_str()))
                {
                    m_profile.loose_round_visual=round.visual.c_str();
                    m_profile.shell_center[1].set(0,0,0);
                    m_profile.shell_box_half.set(0.006f,0.006f,round.half_length);
                    break;
                }
        // Other models without a separate live round retain their case.
        for (u16 id=0;id<model.LL_BoneCount() && !m_profile.shell_bones[1].size();++id)
        {
            if (m_profile.loose_round_visual.size()) break;
            LPCSTR name=model.LL_BoneName_dbg(id);
            Fbox box;
            if (!NameHasAny(name,names.spent_case_bones) || !MeasureBone(model,id,box,true)) continue;
            m_profile.shell_bones[1]=name;
            box.getcenter(m_profile.shell_center[1]);
            m_profile.shell_box_half.set(0.006f,0.006f,0.04f);
        }
    }
    m_profile.thrown_mask.clear();
    m_profile.case_bone=nullptr;
    if (m_profile.manual_cycle || m_profile.pump_bone.size())
    {
        m_weapon.VrDropActionShotLayers();
        MeasureThrownParts(model);
    }
    // Left to the game's own reload: no VR loader stays on to block it,
    // whatever the model offered (a magazine, barrels, a tube).
    if (!m_mechanics.vr_reload_supported)
    {
        m_profile.tube=m_profile.internal_rounds=m_profile.break_action=false;
        if (m_profile.detachable)
        {
            m_profile.detachable=false;
            // Its reload's spare magazines stay as the game's animation shows them.
            m_profile.reload_magazines.clear();
            Msg("* [VR mechanics] %s: magazine %s left to the game's own reload",m_profile.section.c_str(),
                m_profile.mag_bone.c_str());
        }
    }
    // A feed or action the overrides ask for that the model does not offer.
    vr::ProfileHandling found;
    found.detachable=m_profile.detachable; found.belt=m_profile.belt.used;
    found.chambers=m_profile.break_action; found.tube=m_profile.tube; found.internal_rounds=m_profile.internal_rounds;
    found.cycling=m_profile.cycle_travel>0.f; found.pump=m_profile.pump_bone.size()!=0;
    if (overridden("feed") && !vr::FeedHonoured(m_mechanics.feed,found))
        Msg("! [VR mechanics] %s: feed = %s not applied: the model shows no such feed",own.c_str(),
            vr::FeedName(m_mechanics.feed));
    if (overridden("action") && !vr::ActionHonoured(m_mechanics.action,found))
        Msg("! [VR mechanics] %s: action = %s not applied: the model shows no part that works so",own.c_str(),
            vr::ActionName(m_mechanics.action));
    // Where the bolt catch (the receiver's left side above the rear of the
    // magazine well) or slide stop (the frame's left side above the
    // trigger) is pressed.
    if (m_mechanics.has_bolt_catch && m_profile.detachable)
        m_profile.release_point.set(m_profile.mag_box_center.x-m_profile.mag_box_half.x-0.012f,
            m_profile.mag_box_center.y+m_profile.mag_box_half.y+0.02f,
            m_profile.mag_box_center.z-m_profile.mag_box_half.z);
    else if (m_profile.has_trigger_point)
        m_profile.release_point.set(m_profile.trigger_point.x-0.015f,m_profile.trigger_point.y+0.025f,
            m_profile.trigger_point.z+0.01f);
    Msg("* [VR mechanics] %s: %s, %s, %s(%u)%s%s%s%s%s%s; bolt turn %.0f deg%s%s%s%s",m_profile.section.c_str(),
        vr::FamilyName(m_mechanics.family),vr::ActionName(m_mechanics.action),vr::FeedName(m_mechanics.feed),
        m_mechanics.capacity,
        m_mechanics.supports_last_round_hold_open ? ", holds open" : "",
        m_mechanics.has_bolt_catch ? ", bolt catch" : "",
        m_mechanics.has_slide_stop ? ", slide stop" : "",
        m_mechanics.has_charging_handle ? ", charging handle" : "",
        m_mechanics.has_dust_cover ? ", dust cover" : m_mechanics.handle_notch ? ", handle notch" : "",
        m_mechanics.requires_manual_charging ? ", charged by hand" : "",
        e.bolt_turn_deg,e.turn_leads ? ", handle lifts first" : "",e.fast_return ? ", cycles itself" : "",
        e.has_shot_l ? (e.shot_l_holds_back ? ", last shot holds open" : ", last shot closes") : "",
        m_mechanics.vr_reload_supported ? "" : "; the game's own reload");
}

void CVrWeaponInteraction::DetectShells(IKinematics& model, u16 trigger)
{
    // A launcher with a drum is neither barrels nor a tube: the game's own
    // reload (only two chambers can be loaded by hand).
    if (smart_cast<CWeaponRG6*>(&m_weapon) && m_weapon.iMagazineSize>2)
    {
        Msg("* [VR weapon] %s: launcher holding %d rounds; the game's own reload",
            m_profile.section.c_str(),m_weapon.iMagazineSize);
        return;
    }
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    // Barrels reach the muzzle, or the model's front: a sawn-off inherits
    // the full-length gun's fire point.
    BoneBounds whole;
    for (u16 id=0;id<count;++id) model.EnumBoneVertices(whole,id);
    const float muzzle_z=whole.count ? std::min(m_profile.muzzle.z,whole.box.max.z) : m_profile.muzzle.z;
    const float trigger_z=trigger!=BI_NONE ? m_profile.trigger_point.z : 0.f;
    // What the weapon's own animations move: how far each part slides
    // straight back and turns, relative to its parent, over its shots, pump
    // strokes, opening, closing and reloads. A pump is a part they slide
    // back; break-action barrels are a part they swing open. A semi-automatic
    // shotgun's fore-end or a tube gun's barrel never moves, so neither is
    // taken for one by its name or shape alone.
    LPCSTR hud=m_weapon.HudSection().c_str();
    xr_vector<float> back(count,0.f), turn(count,0.f);
    bool sampled=false;
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    const xr_vector<Fmatrix> bind_local=BindLocal(model,bind);
    if (clips.Bones()==count && bind.size()>=count)
        for (LPCSTR key:{"anm_shots","anm_shot","anm_shot_1","anm_shot_2","anm_pump","anm_open","anm_close",
            "anm_close_empty","anm_add_cartridge","anm_reload","anm_reload_1","anm_reload_2","anm_reload_empty"})
        {
            const VrSampledClip* clip=clips.Clip(key,180);
            if (!clip) continue;
            sampled=true;
            const int steps=ClipSteps(clip->length,180);
            for (int step=0;step<steps;++step)
            {
                for (u16 id=0;id<count;++id)
                {
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    if (parent>=count) continue;
                    const Fmatrix& rest=bind_local[id];
                    Fmatrix now;
                    now.mul_43(Fmatrix().invert(clip->At(step,parent)),clip->At(step,id));
                    Fvector moved; moved.sub(now.c,rest.c);
                    bind[parent].transform_dir(moved);
                    if (-moved.z>2.f*_sqrt(moved.x*moved.x+moved.y*moved.y)) back[id]=std::max(back[id],-moved.z);
                    Fquaternion a, b; a.set(rest); b.set(now);
                    const float dot=_abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);
                    turn[id]=std::max(turn[id],2.f*acosf(clampr(dot,0.f,1.f)));
                }
            }
        }
    const bool bm16=smart_cast<CWeaponBM16*>(&m_weapon)!=nullptr;
    // Break-action: a bone carrying shell meshes whose subtree reaches the
    // muzzle (the barrels), which the animations swing open 15 degrees or
    // more (or a BM-16-class gun whose animations show none). Its origin is
    // the hinge.
    // A BM-16-class gun whose animations show no turn (some TOZ-34 sawn-off
    // models) still breaks open: a second pass takes the bone carrying the
    // shells to the muzzle without the turn.
    for (int pass=0;pass<2 && !m_profile.break_action;++pass)
    for (u16 hinge=0;hinge<count && !m_profile.break_action;++hinge)
    {
        if (model.LL_GetData(hinge).GetParentID()==BI_NONE) continue;
        if (sampled ? (turn[hinge]<deg2rad(15.f) && !(bm16 && pass==1)) : !bm16) continue;
        int shells=0;
        Fbox box;
        for (u16 id=0;id<count && shells<2;++id)
            if (id!=hinge && Descends(model,id,hinge) && RoundName(model.LL_BoneName_dbg(id)) &&
                MeasureBone(model,id,box,true))
            {
                m_profile.shell_bones[shells]=model.LL_BoneName_dbg(id);
                box.getcenter(m_profile.shell_center[shells++]);
            }
        if (!shells || !MeasureBone(model,hinge,box,true) || box.max.z<muzzle_z-0.12f) continue;
        // The part carrying the trigger is the gun, tilted by its reload.
        if (trigger!=BI_NONE && Descends(model,trigger,hinge))
        {
            m_profile.shell_bones[0]=m_profile.shell_bones[1]="";
            continue;
        }
        if (shells==1)
        {
            m_profile.shell_bones[1]=m_profile.shell_bones[0];
            m_profile.shell_center[1]=m_profile.shell_center[0];
        }
        m_profile.break_action=true;
        m_profile.hinge_bone=model.LL_BoneName_dbg(hinge);
        m_profile.hinge_point=bind[hinge].c;
        m_profile.hinge_angle=sampled && turn[hinge]>=deg2rad(15.f) ? clampr(turn[hinge],deg2rad(25.f),deg2rad(60.f)) :
            deg2rad(35.f);
        Msg("* [VR weapon] %s: break-action, barrels on %s open %.0f deg",m_profile.section.c_str(),
            m_profile.hinge_bone.c_str(),rad2deg(m_profile.hinge_angle));
    }
    const bool launcher=smart_cast<CWeaponRG6*>(&m_weapon)!=nullptr;
    if (!m_profile.break_action && (bm16 || launcher))
    {
        // The barrels: the bone (not the receiver chain carrying the
        // trigger) whose own mesh reaches the muzzle and has the most of it.
        u16 barrels=BI_NONE; unsigned most=0;
        for (u16 id=0;id<count;++id)
        {
            if (model.LL_GetData(id).GetParentID()==BI_NONE || (trigger!=BI_NONE && Descends(model,trigger,id))) continue;
            BoneBounds part;
            for (u16 child=0;child<count;++child)
                if (Descends(model,child,id)) model.EnumBoneVertices(part,child);
            if (!part.count || part.box.max.z<muzzle_z-0.12f || part.count<=most) continue;
            // Turning in its animations (or none sampled at all) when it can.
            if (sampled && turn[id]<deg2rad(5.f) && !bm16) continue;
            most=part.count; barrels=id;
        }
        BoneBounds part;
        if (barrels!=BI_NONE)
            for (u16 child=0;child<count;++child)
                if (Descends(model,child,barrels)) model.EnumBoneVertices(part,child);
        if (barrels!=BI_NONE && part.count)
        {
            // Chambers at the barrels' breech: side by side or over and
            // under, as the barrels are wider or taller (one for a launcher).
            Fvector size, centre; part.box.getsize(size); part.box.getcenter(centre);
            const float breech=part.box.min.z+0.03f;
            if (launcher || m_weapon.iMagazineSize<2)
                for (int c2=0;c2<2;++c2) m_profile.shell_center[c2].set(centre.x,centre.y,breech);
            else if (size.x>size.y)
            {
                m_profile.shell_center[0].set(centre.x-size.x*0.25f,centre.y,breech);
                m_profile.shell_center[1].set(centre.x+size.x*0.25f,centre.y,breech);
            }
            else
            {
                m_profile.shell_center[0].set(centre.x,centre.y+size.y*0.25f,breech);
                m_profile.shell_center[1].set(centre.x,centre.y-size.y*0.25f,breech);
            }
            // A round carried by the receiver's bone (an M79's grenade) is
            // still in the barrels: it opens with them.
            for (u16 id=0;id<count;++id)
            {
                LPCSTR name=model.LL_BoneName_dbg(id);
                Fbox box; Fvector middle;
                if (Descends(model,id,barrels) || !MeasureBone(model,id,box,true) || !RoundName(name)) continue;
                box.getcenter(middle);
                Fbox near_barrels=part.box; near_barrels.grow(0.03f);
                if (!near_barrels.contains(middle)) continue;
                m_profile.shell_bones[0]=m_profile.shell_bones[1]=name;
                m_profile.shell_center[0]=m_profile.shell_center[1]=middle;
                m_profile.shell_box_half.set(0.025f,0.025f,0.05f);
                break;
            }
            m_profile.break_action=true;
            m_profile.hinge_bone=model.LL_BoneName_dbg(barrels);
            m_profile.hinge_point=bind[barrels].c;
            m_profile.hinge_angle=sampled && turn[barrels]>=deg2rad(15.f) ?
                clampr(turn[barrels],deg2rad(25.f),deg2rad(60.f)) : deg2rad(35.f);
            Msg("* [VR weapon] %s: break-action (barrels found by shape), barrels on %s open %.0f deg",
                m_profile.section.c_str(),m_profile.hinge_bone.c_str(),rad2deg(m_profile.hinge_angle));
        }
        else
        {
            Msg("! [VR weapon] %s: break-action gun, but no barrels part found; the game's own reload",
                m_profile.section.c_str());
            return;
        }
    }
    if (m_profile.break_action) return;
    // Tube: one shell at a time through a port ahead of the trigger guard.
    // A shell mesh posed for the reload animation provides the held shell;
    // other loose shells (for example at the ejection port) are hidden.
    m_profile.tube=true;
    m_profile.loading_port=m_profile.trigger_point;
    m_profile.loading_port.add(Fvector().set(0.f,-0.012f,0.07f));
    int hidden=0;
    for (u16 id=0;id<count;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        Fbox box;
        if (!ShellName(name) || !MeasureBone(model,id,box,true)) continue;
        if (!m_profile.shell_bones[1].size() && !NameHasAny(name,VrNames().spent_case_bones))
        {
            m_profile.shell_bones[1]=name;
            box.getcenter(m_profile.shell_center[1]);
            m_profile.shell_box_half.set(0.011f,0.011f,0.034f);
        }
        if (hidden<3) m_profile.hidden_parts[hidden++]=name;
    }
    // Pump: a fore-end the animations slide back 3 cm or more, named as
    // one or else a long part ahead of the trigger. Without animations only
    // a named one counts.
    const auto slides=[&](u16 id) { return sampled ? back[id]>=0.03f : true; };
    u16 pump=BI_NONE;
    for (u16 id=0;id<count && pump==BI_NONE;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        if ((NameHas(name,"pomp") || NameHas(name,"pump") || NameHas(name,"forend") ||
            NameHas(name,"cevie")) && slides(id)) pump=id;
    }
    for (u16 id=0;id<count && pump==BI_NONE && sampled;++id)
    {
        Fbox box;
        if (model.LL_GetData(id).GetParentID()==BI_NONE || !MeasureBone(model,id,box,false)) continue;
        LPCSTR name=model.LL_BoneName_dbg(id);
        if (NameHas(name,"scope") || NameHas(name,"silencer") || NameHas(name,"launcher") ||
            ShellName(name)) continue;
        // Below the bore (round the magazine tube), not a bolt in the receiver.
        Fvector centre; box.getcenter(centre);
        const bool under=m_profile.muzzle.square_magnitude()<EPS_S || centre.y<m_profile.muzzle.y-0.015f;
        if (box.min.z>trigger_z+0.04f && box.max.z-box.min.z>0.12f && box.max.z<muzzle_z && under && slides(id)) pump=id;
    }
    Fbox box;
    if (pump!=BI_NONE && MeasureBone(model,pump,box,false))
    {
        m_profile.pump_bone=model.LL_BoneName_dbg(pump);
        box.getcenter(m_profile.pump_grab);
        xr_vector<Fvector> points;
        PointCollector collector(points);
        model.EnumBoneVertices(collector,pump);
        Fvector sum; sum.set(0,0,0); unsigned used=0;
        const float bore=m_profile.muzzle.square_magnitude()>EPS_S ? m_profile.muzzle.y : box.max.y;
        for (const Fvector& p:points)
            if (p.z>trigger_z+0.08f && p.y<bore-0.01f) { sum.add(p); ++used; }
        if (used>=10) m_profile.pump_grab=sum.div(float(used));
        m_profile.pump_travel=sampled ? clampr(back[pump],0.04f,0.12f) : 0.08f;
        Msg("* [VR weapon] %s: pump %s, %.0f mm stroke, held at (%.3f %.3f %.3f)",m_profile.section.c_str(),
            m_profile.pump_bone.c_str(),m_profile.pump_travel*1000.f,
            m_profile.pump_grab.x,m_profile.pump_grab.y,m_profile.pump_grab.z);
    }
}

void CVrWeaponInteraction::BuildCloud(IKinematics& model, xr_vector<Fvector>& cloud) const
{
    // Static and slide parts only: the magazine, chambered shells and
    // optional attachments do not form the surface the hands grip.
    cloud.clear();
    CloudCollector collector(cloud);
    const u16 magazine=m_profile.detachable ? model.LL_BoneID(m_profile.mag_bone.c_str()) : u16(BI_NONE);
    for (u16 id=0;id<model.LL_BoneCount();++id)
    {
        bool skip=(magazine!=BI_NONE && Descends(model,id,magazine)) ||
            m_profile.ReloadMagazineBone(id,model.LL_BoneCount());
        for (u16 bone=id;!skip && bone!=BI_NONE && bone<model.LL_BoneCount();
             bone=model.LL_GetData(bone).GetParentID())
        {
            LPCSTR name=model.LL_BoneName_dbg(bone);
            skip=ShellName(name) || AttachmentName(name);
            if (model.LL_GetData(bone).GetParentID()==bone) break;
        }
        if (!skip) model.EnumBoneVertices(collector,id);
    }
    // Keep the per-frame finger test bounded on very dense meshes.
    if (cloud.size()>6000)
    {
        const size_t stride=cloud.size()/6000+1;
        size_t kept=0;
        for (size_t i=0;i<cloud.size();i+=stride) cloud[kept++]=cloud[i];
        cloud.resize(kept);
    }
}

bool CVrWeaponInteraction::SampleInsertPath(IKinematics& model, u16 magazine, vr::InsertPath& path) const
{
    // The magazine's pose relative to the receiver through the reload, as a
    // transform of the seated magazine (identity when seated). Its last
    // approach to the seat is the way in: straight for a box magazine,
    // rocked in for an AK's, slid up a rail for a drum.
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (bind.size()<count || magazine>=count) return false;
    const u16 parent=model.LL_GetData(magazine).GetParentID();
    if (parent>=count) return false;
    xr_vector<Fmatrix> track;
    (void)hud;
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    LPCSTR key=nullptr;
    const VrSampledClip* reload=clips.Bones()==count ? clips.Reload(360,key) : nullptr;
    if (reload)
    {
        Fmatrix to_seat; to_seat.invert(bind[magazine]);
        const int steps=ClipSteps(reload->length,360);
        track.reserve(steps);
        for (int step=0;step<steps;++step)
        {
            Fmatrix relative, now, root;
            relative.mul_43(Fmatrix().invert(reload->At(step,parent)),reload->At(step,magazine));
            now.mul_43(bind[parent],relative);
            root.mul_43(now,to_seat);
            track.push_back(root);
        }
    }
    if (track.size()<8) return false;
    const Fvector seat=F(path.lead);
    const auto lead=[&](const Fmatrix& m) { return Point(m,seat); };
    // The last arrival at the seat.
    int arrive=-1;
    for (int n=int(track.size())-1;n>0 && arrive<0;--n)
        if (lead(track[n]).distance_to(seat)<0.002f && lead(track[n-1]).distance_to(seat)>=0.002f) arrive=n;
    if (arrive<0) return false;
    // Back from it until the lead is clear of the mouth, or the animation
    // cuts (a fresh magazine appearing in the hand).
    const float wanted=std::max(m_profile.extraction,0.03f)+0.01f;
    xr_vector<Fmatrix> approach;
    approach.push_back(Fmatrix().identity());
    float reached=0.f;
    for (int n=arrive-1;n>=0 && reached<wanted;--n)
    {
        if (lead(track[n]).distance_to(lead(track[n+1]))>0.03f) break;
        approach.push_back(track[n]);
        reached=std::max(reached,lead(track[n]).distance_to(seat));
    }
    if (reached<std::max(0.6f*m_profile.extraction,0.02f)) return false;
    // 32 poses evenly spaced along the lead's path.
    xr_vector<float> along(approach.size(),0.f);
    for (size_t n=1;n<approach.size();++n)
        along[n]=along[n-1]+lead(approach[n]).distance_to(lead(approach[n-1]));
    path.poses.clear();
    size_t segment=0;
    for (int i=0;i<32;++i)
    {
        const float at=along.back()*float(i)/31.f;
        while (segment+2<approach.size() && along[segment+1]<at) ++segment;
        const float length=along[segment+1]-along[segment];
        const float u=length>EPS_S ? clampr((at-along[segment])/length,0.f,1.f) : 0.f;
        path.poses.push_back(vr::Blend(ToFrame(approach[segment]),ToFrame(approach[segment+1]),u));
    }
    return true;
}

// A weapon's contact fields, built on a thread of their own: the frame only
// collects the triangles and samples the insertion path, which need the
// model, and the weapon picks the result up when it is ready.
struct VrContactJob
{
    std::atomic<bool> ready{false};
    std::shared_ptr<const VrWeaponContact> result;
};
namespace vrwi
{
std::atomic<int> g_contact_jobs{0};
// Quitting right after a first draw: no job may outlive this library.
struct ContactJobsDone
{
    ~ContactJobsDone() { for (int ms=0;ms<3000 && g_contact_jobs.load()>0;++ms) Sleep(1); }
} g_contact_jobs_done;
}

void CVrWeaponInteraction::PollContact()
{
    if (!m_contact_job || !m_contact_job->ready.load(std::memory_order_acquire)) return;
    m_contact=m_contact_job->result;
    m_contact_job.reset();
}

void CVrWeaponInteraction::BuildContact(IKinematics& model)
{
    // Shared by every weapon of this profile and visual for the session.
    static xr_map<xr_string,std::shared_ptr<VrContactJob>> cache;
    xr_string key=m_profile.section.c_str();
    key.append("|").append(m_profile.visual.c_str());
    if (auto found=cache.find(key); found!=cache.end()) { m_contact_job=found->second; PollContact(); return; }
    CTimer timer; timer.Start();
    auto contact=std::make_shared<VrWeaponContact>();
    const u16 count=model.LL_BoneCount();
    const u16 magazine=m_profile.detachable ? model.LL_BoneID(m_profile.mag_bone.c_str()) : u16(BI_NONE);
    const bool pump=!m_profile.cycle_bone.size() && m_profile.pump_bone.size();
    const shared_str& moving_name=pump ? m_profile.pump_bone : m_profile.cycle_bone;
    const u16 moving=moving_name.size() ? model.LL_BoneID(moving_name.c_str()) : u16(BI_NONE);
    std::vector<vr::Tri> receiver, cycle, held;
    TriangleCollector to_receiver(receiver), to_cycle(cycle), to_magazine(held);
    // The magazine as VR draws it: its contents at rest (belt links).
    const xr_vector<Fmatrix>* rest=m_profile.MagazineRest();
    xr_vector<Fmatrix> bind;
    if (rest) model.LL_GetBindTransform(bind);
    const float magazine_height=magazine!=BI_NONE ? MagazineHeight(model,magazine) : 0.f;
    for (u16 id=0;id<count;++id)
    {
        // As BuildCloud: no loose shells or optional attachments. Hinged
        // parts (covers) are left out: posed open they would leave a
        // phantom closed surface.
        bool skip=m_profile.ReloadMagazineBone(id,count);
        for (u16 bone=id;!skip && bone!=BI_NONE && bone<count;bone=model.LL_GetData(bone).GetParentID())
        {
            LPCSTR name=model.LL_BoneName_dbg(bone);
            skip=ShellName(name) || AttachmentName(name);
            for (int part=0;!skip && part<m_profile.part_count;++part)
                skip=m_profile.part_bones[part].size() && !xr_strcmp(m_profile.part_bones[part].c_str(),name);
            if (model.LL_GetData(bone).GetParentID()==bone) break;
        }
        if (skip) continue;
        if (magazine!=BI_NONE && InMagazine(model,id,magazine,magazine_height))
            EnumRestVertices(model,to_magazine,id,magazine,rest,bind);
        else if (magazine!=BI_NONE && Descends(model,id,magazine)) continue;  // a spare
        else if (moving!=BI_NONE && Descends(model,id,moving)) model.EnumBoneVertices(to_cycle,id);
        else model.EnumBoneVertices(to_receiver,id);
    }
    bool path_animated=false;
    if (magazine!=BI_NONE)
    {
        // A magazine without its own mesh still has a way in (its box's),
        // only no surface to collide or grip.
        vr::InsertPath& path=contact->path;
        if (held.size()>=4) contact->magazine.Build(vr::SurfaceSamples(held,0.004f,1024));
        // The lead: the centre of the magazine's top (feed) slab.
        const vr::V3 axis=vr::Normalized(V(m_profile.well_axis));
        float top=-flt_max, bottom=flt_max;
        for (const vr::V3& p:contact->magazine.samples)
        { top=std::max(top,vr::Dot(p,axis)); bottom=std::min(bottom,vr::Dot(p,axis)); }
        vr::V3 lead; int leads=0;
        for (const vr::V3& p:contact->magazine.samples)
            if (vr::Dot(p,axis)>=top-0.15f*(top-bottom)) { lead=lead+p; ++leads; }
        path.lead=leads ? lead*(1.f/float(leads)) :
            V(m_profile.mag_base)+axis*(2.f*m_profile.mag_box_half.y);
        const bool animated=SampleInsertPath(model,magazine,path);
        path_animated=animated;
        if (!animated)
        {
            // Straight out along the measured well axis.
            const float length=std::max(m_profile.extraction,0.03f)+0.01f;
            path.poses.clear();
            for (int i=0;i<32;++i)
            {
                vr::Frame f;
                f.c=axis*(-length*float(i)/31.f);
                path.poses.push_back(f);
            }
        }
        path.Finish();
        Msg("* [VR weapon] %s: magazine path %s, %.0f mm",m_profile.section.c_str(),
            animated ? "from the reload animation" : "along the well axis",path.length*1000.f);
    }
    auto job=std::make_shared<VrContactJob>();
    cache[key]=job;
    m_contact_job=job;
    const vr::V3 axis=vr::Normalized(V(m_profile.well_axis));
    // The way in made physical (VrMagazineWayIn.h): chosen on the thread,
    // where the receiver's field is built.
    vr::WayInInput way_in;
    way_in.animated=path_animated;
    way_in.axis=axis;
    way_in.centre_x=m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f;
    way_in.handgun=Handgun();
    way_in.keep=m_profile.explicit_well_axis;
    const bool choose_way=magazine!=BI_NONE;
    g_contact_jobs.fetch_add(1);
    std::thread([job,contact,timer,axis,way_in,choose_way,section=std::string(m_profile.section.c_str()),
        receiver=std::move(receiver),cycle=std::move(cycle),held=std::move(held)]() mutable
    {
    if (held.size()>=4) contact->magazine_surface.Build(held,0.0025f,0.012f);
    contact->receiver.Build(receiver,0.0025f,0.012f);
    if (choose_way && contact->path.Valid() && !contact->magazine.samples.empty())
    {
        vr::WayInInput in=way_in;
        in.raw=contact->path;
        const float old_length=in.raw.length;
        vr::InsertPath old=in.raw;
        old.MeasureMouth(contact->receiver,contact->magazine.samples);
        vr::WayInResult way=vr::ChooseWayIn(in,contact->receiver,contact->magazine.samples);
        // Where the magazine really enters the receiver.
        way.path.MeasureMouth(contact->receiver,contact->magazine.samples);
        vr::FinishWayIn(way,in.extra);
        contact->path=way.path;
        const vr::InsertPath& p=contact->path;
        const float at=std::min(1.f,std::max(0.f,p.mouth));
        const vr::V3 t=p.Tangent(at), t_old=old.Tangent(std::min(1.f,std::max(0.f,old.mouth)));
        Msg("* [VR way-in] %s: %s%s; mouth %.0f mm of %.0f mm%s%s; covered %.0f mm; in the well: turn %.1f deg, "
            "off line %.1f mm, across %.1f mm; way in (%.2f %.2f %.2f), was (%.2f %.2f %.2f), mouth %.0f of %.0f mm",
            section.c_str(),vr::WayInName(way.kind),way.centred ? "" : " (off the centre plane: kept)",
            p.mouth*p.length*1000.f,p.length*1000.f,way.mouth_from_depth ? " (from the covered depth)" : "",
            way.extended ? ", carried past" : "",way.enclosed*1000.f,way.pitch_deg,way.deviation*1000.f,
            way.raw_lateral*1000.f,-t.x,-t.y,-t.z,-t_old.x,-t_old.y,-t_old.z,
            old.mouth*old_length*1000.f,old_length*1000.f);
    }
    else contact->path.MeasureMouth(contact->receiver,contact->magazine.samples);
    // A hand takes the seated magazine by what shows of it: its surface
    // below where it enters the receiver (at least its bottom 2 cm).
    if (contact->path.Valid() && !contact->magazine.samples.empty())
    {
        const vr::InsertPath& path=contact->path;
        float bottom=flt_max;
        for (const vr::V3& p:contact->magazine.samples) bottom=std::min(bottom,vr::Dot(p-path.lead,axis));
        const float inside=std::min(path.mouth*path.length,-bottom-0.02f);
        for (const vr::V3& p:contact->magazine.samples)
            if (-vr::Dot(p-path.lead,axis)>=inside) contact->grab_samples.push_back(p);
    }
    if (contact->path.Valid())
        Msg("* [VR weapon] %s: magazine enters the receiver %.0f mm along its way in",section.c_str(),
            (1.f-contact->path.mouth)*contact->path.length*1000.f);
    if (!cycle.empty()) contact->cycle.Build(cycle,0.0025f,0.012f);
    contact->build_ms=timer.GetElapsed_sec()*1000.f;
    Msg("* [VR weapon] %s: contact geometry: %u receiver and %u moving triangles, %u bricks (%u KB), "
        "magazine %u samples; %.0f ms",section.c_str(),unsigned(receiver.size()),unsigned(cycle.size()),
        unsigned(contact->receiver.Bricks()+contact->cycle.Bricks()),
        unsigned((contact->receiver.Bytes()+contact->cycle.Bytes()+contact->magazine_surface.Bytes())/1024),
        unsigned(contact->magazine.samples.size()),contact->build_ms);
    job->result=contact;
    job->ready.store(true,std::memory_order_release);
    g_contact_jobs.fetch_sub(1);
    }).detach();
}

void CVrWeaponInteraction::MeasureGeometry(IKinematics& model)
{
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    const u16 trigger=FindTrigger(model);
    if (!m_profile.has_trigger_point && trigger!=BI_NONE)
    {
        Fbox box;
        if (MeasureBone(model,trigger,box,false)) box.getcenter(m_profile.trigger_point);
        else m_profile.trigger_point=bind[trigger].c;
        m_profile.has_trigger_point=true;
    }
    BuildCloud(model,m_cloud);
    MeasureOptics(model);
    // The bore runs through the muzzle; the thumb must stay below it.
    m_profile.bore_height=m_profile.muzzle.square_magnitude()>EPS_S ? m_profile.muzzle.y-0.004f :
        (m_profile.has_trigger_point ? m_profile.trigger_point.y+0.045f : 0.04f);
    // Grip: the mesh points behind the trigger guard and below the trigger.
    // Their mean is the grip centre and their lateral spread its half-width.
    // The rake is the slope between the upper and lower halves; sparse or
    // stock-blended meshes give implausible slopes, so only 0..45 degrees
    // is trusted. Checked offline on 12 stock models (PM, USP, AK-74, MP5,
    // SVD, Saiga, BM16, TOZ-34, 870, MP-133, SPAS-12, Protecta).
    if (m_profile.has_trigger_point && !m_cloud.empty())
    {
        const Fvector& t=m_profile.trigger_point;
        xr_vector<Fvector> region;
        for (const Fvector& p:m_cloud)
            if (p.y<t.y-0.02f && p.y>t.y-0.12f && p.z<t.z-0.03f && p.z>t.z-0.16f && _abs(p.x-t.x)<0.045f)
                region.push_back(p);
        if (region.size()>=8)
        {
            Fvector centre; centre.set(0,0,0);
            for (const Fvector& p:region) centre.add(p);
            centre.div(float(region.size()));
            Fvector upper, lower; upper.set(0,0,0); lower.set(0,0,0);
            unsigned upper_count=0, lower_count=0;
            xr_vector<float> spread;
            for (const Fvector& p:region)
            {
                if (p.y>centre.y) { upper.add(p); ++upper_count; }
                else { lower.add(p); ++lower_count; }
                spread.push_back(_abs(p.x-centre.x));
            }
            float rake=18.f;
            if (upper_count && lower_count)
            {
                upper.div(float(upper_count)); lower.div(float(lower_count));
                const float measured=rad2deg(atan2f(upper.z-lower.z,upper.y-lower.y));
                if (measured>=0.f && measured<=45.f) rake=measured;
            }
            std::sort(spread.begin(),spread.end());
            // A stock's wrist (Mosin, K98, BM-16, hunting rifles) has no
            // mesh low under the trigger but its own underside: that mean
            // sat on the underside and the hand under the stock. Measured on
            // the weapon's whole height there (up to 1.5 cm under the bore),
            // a centre in its lowest quarter goes to the middle. A pistol
            // grip's centre is mid-grip and stays.
            if (m_profile.muzzle.square_magnitude()>EPS_S)
            {
                const float top=m_profile.muzzle.y-0.015f;
                float low=flt_max, high=-flt_max;
                for (const Fvector& p:m_cloud)
                    if (_abs(p.z-centre.z)<0.012f && _abs(p.x-centre.x)<0.02f && p.y<top && p.y>top-0.15f)
                    { low=std::min(low,p.y); high=std::max(high,p.y); }
                if (low<high && high-low>=0.02f && centre.y-low<0.25f*(high-low))
                {
                    Msg("* [VR weapon] %s: grip centre %.3f on the underside of a %.0f mm wrist; middle %.3f",
                        m_profile.section.c_str(),centre.y,(high-low)*1000.f,0.5f*(low+high));
                    centre.y=0.5f*(low+high);
                }
            }
            m_profile.hand_grip_center=centre;
            m_profile.hand_grip_angle=rake;
            m_profile.hand_grip_radius=clampr(spread[size_t(0.9f*float(spread.size()-1))],0.008f,0.03f);
            m_profile.has_hand_grip_center=true;
            if (m_profile.automatic || !m_profile.grip_offset.square_magnitude())
                m_profile.grip_offset=centre;
        }
    }
    // The support hand's place on a derived long gun when the animation's
    // left hand is not used (ForendPoint): round the fore-end halfway from
    // the grip to the muzzle, at the middle of the mesh's height there (the
    // fist closes round it). The grip's height plus 2 cm sat 5-10 cm under
    // the fore-end of guns held by a stock's wrist (BM-16, Remington 700),
    // whose grip is far below the bore.
    m_profile.has_forend=false;
    if (m_profile.automatic && m_profile.two_handed && !Handgun() && !m_cloud.empty() &&
        m_profile.muzzle.square_magnitude()>EPS_S)
    {
        const Fvector grip=m_profile.has_hand_grip_center ? m_profile.hand_grip_center : m_profile.grip_offset;
        const float centre_x=m_profile.has_trigger_point ? m_profile.trigger_point.x : grip.x;
        const float z=0.5f*(grip.z+m_profile.muzzle.z);
        if (m_profile.muzzle.z>grip.z+0.25f)
        {
            float low=flt_max, high=-flt_max;
            unsigned used=0;
            for (const Fvector& p:m_cloud)
                if (_abs(p.z-z)<0.02f && _abs(p.x-centre_x)<0.04f && p.y<m_profile.muzzle.y+0.03f &&
                    p.y>m_profile.muzzle.y-0.08f)
                { low=std::min(low,p.y); high=std::max(high,p.y); ++used; }
            if (used>=8 && high-low>0.01f)
            {
                m_profile.forend_point.set(centre_x,0.5f*(low+high),z);
                m_profile.has_forend=true;
                Msg("* [VR weapon] %s: fore-end at (%.3f %.3f %.3f), mesh %.0f mm deep (support offset %.3f %.3f %.3f)",
                    m_profile.section.c_str(),m_profile.forend_point.x,m_profile.forend_point.y,
                    m_profile.forend_point.z,(high-low)*1000.f,m_profile.support_offset.x,
                    m_profile.support_offset.y,m_profile.support_offset.z);
            }
        }
    }
    // Magazine well axis from the magazine mesh itself: the line from the
    // centre of its lowest slab to the centre of its highest (feed) slab.
    // Insertion and the held pose follow it; nothing is assumed per weapon.
    const u16 magazine=m_profile.detachable ? model.LL_BoneID(m_profile.mag_bone.c_str()) : u16(BI_NONE);
    const xr_vector<Fmatrix>* rest=m_profile.MagazineRest();
    Fbox box;
    if (magazine!=BI_NONE && MeasureMagazine(model,magazine,box,rest,&bind))
    {
        SlabCentroids slabs;
        const float height=box.max.y-box.min.y;
        slabs.low=box.min.y+height*0.15f;
        slabs.high=box.max.y-height*0.15f;
        const float magazine_height=MagazineHeight(model,magazine);
        for (u16 id=0;id<model.LL_BoneCount();++id)
            if (InMagazine(model,id,magazine,magazine_height)) EnumRestVertices(model,slabs,id,magazine,rest,bind);
        if (slabs.bottom_count && slabs.top_count)
        {
            slabs.bottom.div(float(slabs.bottom_count));
            slabs.top.div(float(slabs.top_count));
            Fvector axis; axis.sub(slabs.top,slabs.bottom);
            const float length=axis.magnitude();
            if (length>0.02f)
            {
                axis.div(length);
                if (axis.y<0.8f) axis.set(0.f,1.f,0.f);
                // A pistol magazine is straight, but its floorplate and the
                // top cartridge's nose push the end centroids forward, which
                // stands the axis up and guides the magazine through the front
                // of the grip. Its rear wall gives the true rake.
                if (Handgun())
                {
                    MagazineWalls walls;
                    walls.low=box.min.y+height*0.2f;
                    walls.high=box.max.y-height*0.2f;
                    for (u16 id=0;id<model.LL_BoneCount();++id)
                        if (InMagazine(model,id,magazine,magazine_height)) model.EnumBoneVertices(walls,id);
                    float slope=0.f;
                    if (walls.Slope(true,slope) && _abs(atanf(slope))<deg2rad(35.f))
                    {
                        const float chord=rad2deg(atan2f(axis.z,axis.y));
                        axis.set(0.f,1.f,slope);
                        axis.normalize();
                        Msg("* [VR weapon] %s: magazine well rake %.1f deg from its rear wall (end centres %.1f)",
                            m_profile.section.c_str(),rad2deg(atanf(slope)),chord);
                    }
                }
                // A magazine seated on the bore's plane goes in in that plane.
                const float centre_x=m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f;
                Fvector middle; box.getcenter(middle);
                if (!m_profile.explicit_well_axis && _abs(middle.x-centre_x)<0.015f && _abs(axis.x)>EPS_S)
                {
                    axis.x=0.f;
                    axis.normalize_safe();
                }
                if (m_profile.explicit_well_axis) axis=m_profile.well_axis;
                else m_profile.well_axis=axis;
                Msg("* [VR weapon] %s: magazine axis (%.2f %.2f %.2f) from its mesh%s",m_profile.section.c_str(),
                    axis.x,axis.y,axis.z,m_profile.explicit_well_axis ? " (profile)" : "");
                // Base: the bottom slab centre carried down to the lowest point.
                // (An axis from the reload animation may run sideways, a box
                // slid off a rail: then straight down to the lowest point.)
                Fvector base=slabs.bottom;
                if (axis.y>0.3f) base.mad(axis,-(slabs.bottom.y-box.min.y)/axis.y);
                else base.y=box.min.y;
                m_profile.mag_base=base;
            }
        }
    }
    BuildContact(model);
}

