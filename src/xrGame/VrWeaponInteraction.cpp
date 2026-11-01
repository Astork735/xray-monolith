#include "stdafx.h"
#include "VrWeaponInteraction.h"
#include "VrWeaponPose.h"
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
#include "UIGameCustom.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_game_object.h"
#include "../xrEngine/CameraBase.h"
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

namespace
{
// How far behind the controller's handle centre a held magazine sits.
constexpr float kMagazineHoldBack=0.03f;
Fvector Point(const Fmatrix& transform, const Fvector& local)
{
    Fvector result=local;
    transform.transform_tiny(result);
    return result;
}
bool Near(const Fvector& a, const Fvector& b, float radius)
{
    return a.distance_to_sqr(b)<=radius*radius;
}
void _BCL BonePose(CBoneInstance* bone)
{
    bone->mTransform=*static_cast<const Fmatrix*>(bone->callback_param());
}
struct BoneBounds : public SEnumVerticesCallback
{
    Fbox box;
    unsigned count=0;
    BoneBounds() { box.invalidate(); }
    void operator()(const Fvector& p) override { box.modify(p); ++count; }
};
bool Descends(IKinematics& model, u16 id, u16 root)
{
    for (u16 step=0;id!=BI_NONE && id<model.LL_BoneCount() && step<model.LL_BoneCount();++step)
    {
        if (id==root) return true;
        id=model.LL_GetData(id).GetParentID();
    }
    return false;
}
bool NameHas(LPCSTR name, LPCSTR part);
// A bone of the magazine itself: below it, but not inside a spare magazine
// nested under it (an AKS-74U's "magazin_2", the one its reload brings in).
bool MeasureBone(IKinematics& model, u16 bone, Fbox& box, bool subtree);
bool InMagazine(IKinematics& model, u16 id, u16 magazine)
{
    if (!Descends(model,id,magazine)) return false;
    // A spare: a magazine-named part at least half the magazine's height. A
    // smaller one (a follower, the rounds) is part of the magazine.
    Fbox own;
    const float height=MeasureBone(model,magazine,own,false) ? own.max.y-own.min.y : 0.f;
    for (u16 bone=id;bone!=magazine && bone<model.LL_BoneCount();bone=model.LL_GetData(bone).GetParentID())
    {
        Fbox part;
        if (NameHas(model.LL_BoneName_dbg(bone),"mag") && MeasureBone(model,bone,part,false) &&
            part.max.y-part.min.y>=0.5f*height) return false;
    }
    return true;
}
// Bind-space mesh bounds of the magazine (without nested spares).
bool MeasureMagazine(IKinematics& model, u16 magazine, Fbox& box)
{
    if (magazine==BI_NONE) return false;
    BoneBounds bounds;
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (InMagazine(model,id,magazine)) model.EnumBoneVertices(bounds,id);
    if (!bounds.count) return false;
    box=bounds.box;
    return true;
}
// Bind-space mesh bounds of a bone, optionally with its whole subtree.
bool MeasureBone(IKinematics& model, u16 bone, Fbox& box, bool subtree)
{
    if (bone==BI_NONE) return false;
    BoneBounds bounds;
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (id==bone || (subtree && Descends(model,id,bone)))
            model.EnumBoneVertices(bounds,id);
    if (!bounds.count) return false;
    box=bounds.box;
    return true;
}
bool NameHas(LPCSTR name, LPCSTR part);
u16 FindTrigger(IKinematics& model)
{
    u16 trigger=model.LL_BoneID("trigger");
    for (u16 id=0;trigger==BI_NONE && id<model.LL_BoneCount();++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        if (name && (strstr(name,"trigger") || strstr(name,"spust")) && !strstr(name,"_gl")) trigger=id;
    }
    return trigger;
}
// Centroids of the lowest and highest 15% of a bone subtree's vertices.
struct SlabCentroids : public SEnumVerticesCallback
{
    float low=0.f, high=0.f;
    Fvector bottom{}, top{};
    unsigned bottom_count=0, top_count=0;
    void operator()(const Fvector& p) override
    {
        if (p.y<=low) { bottom.add(p); ++bottom_count; }
        else if (p.y>=high) { top.add(p); ++top_count; }
    }
};
// Rear and front walls of a magazine body: the rearmost and foremost vertex
// in each height bin of a band that leaves out the floorplate and the feed
// lips with their cartridge.
struct MagazineWalls : public SEnumVerticesCallback
{
    static constexpr int bins=10;
    float low=0.f, high=0.f;
    float rear[bins], front[bins];
    MagazineWalls() { for (int i=0;i<bins;++i) { rear[i]=FLT_MAX; front[i]=-FLT_MAX; } }
    void operator()(const Fvector& p) override
    {
        if (p.y<low || p.y>high || high<=low) return;
        const int bin=std::min(bins-1,int(float(bins)*(p.y-low)/(high-low)));
        rear[bin]=std::min(rear[bin],p.z);
        front[bin]=std::max(front[bin],p.z);
    }
    // Slope dz/dy of a least-squares line through one wall's bins.
    bool Slope(bool use_rear, float& slope) const
    {
        float sy=0.f, sz=0.f, syy=0.f, syz=0.f; int n=0;
        for (int i=0;i<bins;++i)
        {
            const float z=use_rear ? rear[i] : front[i];
            if (_abs(z)>=FLT_MAX) continue;
            const float y=low+(float(i)+0.5f)*(high-low)/float(bins);
            sy+=y; sz+=z; syy+=y*y; syz+=y*z; ++n;
        }
        const float d=float(n)*syy-sy*sy;
        if (n<5 || _abs(d)<1e-9f) return false;
        slope=(float(n)*syz-sy*sz)/d;
        return true;
    }
};
// 5 mm voxel point cloud, deduplicated, of selected bones.
struct CloudCollector : public SEnumVerticesCallback
{
    xr_vector<Fvector>& points;
    xr_unordered_set<u64> voxels;
    explicit CloudCollector(xr_vector<Fvector>& out) : points(out) {}
    void operator()(const Fvector& p) override
    {
        const auto cell=[](float v) { return u64(s64(floorf(v*200.f))+(1<<20))&0x1FFFFF; };
        const u64 key=cell(p.x)|(cell(p.y)<<21)|(cell(p.z)<<42);
        if (voxels.insert(key).second) points.push_back(p);
    }
};
// Engine and contact-core types share the row layout (VrContactField.h).
vr::V3 V(const Fvector& v) { return vr::V3(v.x,v.y,v.z); }
Fvector F(const vr::V3& v) { Fvector r; r.set(v.x,v.y,v.z); return r; }
vr::Frame ToFrame(const Fmatrix& m)
{
    vr::Frame f;
    f.i=V(m.i); f.j=V(m.j); f.k=V(m.k); f.c=V(m.c);
    return f;
}
Fmatrix ToMatrix(const vr::Frame& f)
{
    Fmatrix m; m.identity();
    m.i=F(f.i); m.j=F(f.j); m.k=F(f.k); m.c=F(f.c);
    return m;
}
// Bind-pose triangles of selected bones (EnumBoneVertices gives each face's
// three corners in turn).
struct TriangleCollector : public SEnumVerticesCallback
{
    std::vector<vr::Tri>& tris;
    Fvector corner[3];
    int filled=0;
    explicit TriangleCollector(std::vector<vr::Tri>& out) : tris(out) {}
    void operator()(const Fvector& p) override
    {
        corner[filled++]=p;
        if (filled<3) return;
        tris.push_back({V(corner[0]),V(corner[1]),V(corner[2])});
        filled=0;
    }
};
// Bind-pose vertices of selected bones.
struct PointCollector : public SEnumVerticesCallback
{
    xr_vector<Fvector>& points;
    explicit PointCollector(xr_vector<Fvector>& out) : points(out) {}
    void operator()(const Fvector& p) override { points.push_back(p); }
};
// Where a hand takes a cycling part: a pistol's slide by its rear
// serrations (the top of its rear quarter); a long gun's bolt or handle by
// its knob, the part of it standing furthest out from the bore's centre line
// (an AK's handle on the right of its carrier).
// `centre_x`: the bore's line across the model (most models are built on
// x=0; some, such as the Mosin's, about 15 cm aside).
Fvector GrabPoint(IKinematics& model, u16 part, bool pistol, float centre_x)
{
    xr_vector<Fvector> points;
    PointCollector collector(points);
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (Descends(model,id,part)) model.EnumBoneVertices(collector,id);
    if (points.empty())
    {
        xr_vector<Fmatrix> bind;
        model.LL_GetBindTransform(bind);
        return part<bind.size() ? bind[part].c : Fvector().set(0.f,0.f,0.f);
    }
    Fbox box; box.invalidate();
    for (const Fvector& p:points) box.modify(p);
    Fvector centre;
    box.getcenter(centre);
    Fvector sum; sum.set(0,0,0);
    unsigned used=0;
    if (pistol && box.max.x-box.min.x>0.035f)
    {
        const float rear=box.min.z+0.25f*(box.max.z-box.min.z);
        for (const Fvector& p:points)
            if (p.z<=rear && p.y>=centre.y) { sum.add(p); ++used; }
    }
    else
    {
        float widest=0.f;
        for (const Fvector& p:points) widest=std::max(widest,_abs(p.x-centre_x));
        for (const Fvector& p:points)
            if (_abs(p.x-centre_x)>=0.8f*widest) { sum.add(p); ++used; }
    }
    if (!used) return centre;
    sum.div(float(used));
    return sum;
}
// A private copy of a hud model, for sampling its animations.
IRenderVisual* HudCopy(LPCSTR visual)
{
    ::Render->hud_loading=true;
    IRenderVisual* copy=::Render->model_Create(visual);
    ::Render->hud_loading=false;
    return copy;
}
// Plays a hud section's animation on a private model from its first frame.
bool PlayHudMotion(IKinematicsAnimated& animated, LPCSTR hud, LPCSTR key, float* length=nullptr)
{
    if (!pSettings->line_exist(hud,key)) return false;
    LPCSTR value=pSettings->r_string(hud,key);
    string128 hands_name, item_name;
    _GetItem(value,0,hands_name);
    if (_GetItemCount(value)>1) _GetItem(value,1,item_name);
    else xr_strcpy(item_name,hands_name);
    MotionID motion=animated.ID_Cycle_Safe(item_name);
    if (!motion.valid()) motion=animated.ID_Cycle_Safe(hands_name);
    if (!motion.valid()) return false;
    for (u16 part=0;part<animated.partitions().count();++part)
    {
        CBlend* blend=animated.PlayCycle(part,motion,FALSE,0,0,0,1.f);
        if (length && blend) *length=std::max(*length,blend->timeTotal);
    }
    return true;
}
// How far a part turned about one of the weapon's axes (0 x across, 2 z
// along the bore) between two poses relative to its parent, whose
// model-space pose is parent: the turn of its own axes seen along that one.
float TurnAbout(const Fmatrix& rest, const Fmatrix& now, const Fmatrix& parent, int axis)
{
    float turn=0.f;
    for (int own=0;own<3;++own)
    {
        Fvector a=own==0 ? rest.i : own==1 ? rest.j : rest.k;
        Fvector b=own==0 ? now.i : own==1 ? now.j : now.k;
        parent.transform_dir(a); parent.transform_dir(b);
        if (axis==2) a.z=b.z=0.f; else a.x=b.x=0.f;
        const float la=a.magnitude(), lb=b.magnitude();
        if (la<0.5f || lb<0.5f) continue;
        turn=std::max(turn,acosf(clampr(a.dotproduct(b)/(la*lb),-1.f,1.f)));
    }
    return turn;
}
// How far a bone is back from its bind place, measured in the receiver's
// frame (so the receiver's own recoil kick in a shot is not counted).
float RearInReceiver(IKinematics& sample, const xr_vector<Fmatrix>& bind, u16 bone, u16 receiver)
{
    Fmatrix rel; rel.invert(sample.LL_GetTransform(receiver)); rel.mulA_43(bind[receiver]);
    Fvector now; rel.transform_tiny(now,sample.LL_GetTransform(bone).c);
    return bind[bone].c.z-now.z;
}
// The lowest part of a bone's mesh (a lever's loop), averaged over its
// bottom centimetre.
Fvector LowestPoint(IKinematics& model, u16 part)
{
    xr_vector<Fvector> points;
    PointCollector collector(points);
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (Descends(model,id,part)) model.EnumBoneVertices(collector,id);
    if (points.empty()) { xr_vector<Fmatrix> bind; model.LL_GetBindTransform(bind); return bind[part].c; }
    float low=flt_max;
    for (const Fvector& p:points) low=std::min(low,p.y);
    Fvector sum; sum.set(0,0,0); unsigned used=0;
    for (const Fvector& p:points) if (p.y<=low+0.01f) { sum.add(p); ++used; }
    return sum.div(float(used));
}
// Plays the weapon's reload (the empty one first); returns the key it played.
LPCSTR StartReloadMotion(IKinematicsAnimated& animated, LPCSTR hud)
{
    for (LPCSTR key:{"anm_reload_empty","anm_reload"})
        if (PlayHudMotion(animated,hud,key)) return key;
    return nullptr;
}
bool ShellName(LPCSTR name)
{
    return NameHas(name,"cartridge") || NameHas(name,"patron") || NameHas(name,"shell") ||
        NameHas(name,"gilza") || NameHas(name,"bullet");
}
bool NameHas(LPCSTR name, LPCSTR part)
{
    if (!name) return false;
    string128 lower;
    xr_strcpy(lower,sizeof(lower),name);
    _strlwr_s(lower,sizeof(lower));
    return strstr(lower,part)!=nullptr;
}
void CollapseHiddenBones(IKinematics* model)
{
    vr::CollapseHiddenSkinning(*model);
}
void ReleaseModel(IRenderVisual*& visual)
{
    if (!visual) return;
    if (auto* model=visual->dcast_PKinematics())
    {
        model->Callback(nullptr,nullptr);
        for (u16 i=0;i<model->LL_BoneCount();++i)
            model->LL_GetBoneInstance(i).reset_callback();
        model->LL_SetBonesVisible(~u64(0));
        model->CalculateBones_Invalidate();
    }
    ::Render->model_Delete(visual);
}
}

CVrWeaponInteraction::CVrWeaponInteraction(CWeapon* weapon) : m_weapon(*weapon)
{
    m_supported=LoadProfile();
    m_hand=vr::DominantHand();
    // A derived profile is measured once the weapon is the player's (see
    // Interact), never for every weapon in the level.
}
CVrWeaponInteraction::~CVrWeaponInteraction() { Destroy(); }

bool CVrWeaponInteraction::LoadProfile()
{
    // Binoculars are a magazine weapon to the game, but no gun: deriving a
    // gun from their model held them reversed in the hand and took them off
    // the belt-item path that looks through them (VrBodySlots.cpp).
    if (!m_weapon.cast_weapon_magazined() || smart_cast<CWeaponBinoculars*>(&m_weapon)) return false;
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
        m_profile.shell_point=pSettings->r_fvector3(m_weapon.HudSection(),"shell_point");
    const auto basic_profile=[&]() -> bool
    {
        // Without an audited profile, derive grip, muzzle and magazine from
        // the HUD model's own bones and meshes when it is first loaded (see
        // AutoProfile). Slides are not inferred: no chamber or cycling.
        m_profile.automatic=true;
        m_profile.grip_offset.set(0.f,0.f,0.f);
        m_profile.detachable=m_profile.chamber=m_profile.slide_stop=false;
        m_profile.cycle_travel=0.f;
        m_basic_magazine=smart_cast<CWeaponPistol*>(&m_weapon)!=nullptr;
        if (m_basic_magazine)
        {
            m_profile.mag_box_center.set(0.f,0.f,0.f);
            m_profile.mag_box_half.set(0.018f,0.053f,0.012f);
            m_profile.mag_base.set(0.f,-0.045f,0.065f);
        }
        m_mechanism.locks_on_empty=false;
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
    m_mechanism.travel_limit=m_profile.cycle_travel>0 ? m_profile.cycle_travel :
        vr::MagazineMechanism<xr_vector<CCartridge>>::travel;
    m_mechanism.locks_on_empty=m_profile.slide_stop;
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
        if (NameHas(name,"magaz") || NameHas(name,"wpn_mag") ||
            (name && (!xr_strcmp(name,"mag") || !strncmp(name,"mag_",4))))
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
        if (magazine!=BI_NONE && MeasureMagazine(model,magazine,found))
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
    if (m_weapon.iMagazineSize>1 && MeasureMagazine(model,magazine,box))
    {
        Fvector size; box.getsize(size);
        if (by_motion || (size.y>0.03f && size.y<0.45f && size.x<0.12f && size.z<0.3f))
        {
            m_profile.mag_bone=model.LL_BoneName_dbg(magazine);
            m_profile.rounds_bone="";
            m_profile.mag_extra_bones[0]=m_profile.mag_extra_bones[1]="";
            int extra=0;
            for (u16 id=0;id<count;++id)
            {
                if (id>=bind.size() || model.LL_GetData(id).GetParentID()!=magazine ||
                    !InMagazine(model,id,magazine)) continue;
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
        }
    }
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
    m_mechanism.locks_on_empty=false;
    if (!m_profile.break_action && !m_profile.pump_bone.size()) MeasureCycle(model);
    Msg("* [VR weapon] %s: derived profile; grip=(%.3f %.3f %.3f) magazine=%s two_handed=%d shells=%s",
        m_profile.section.c_str(),grip.x,grip.y,grip.z,
        m_profile.detachable ? m_profile.mag_bone.c_str() : "none",m_profile.two_handed,
        m_profile.break_action ? "break-action" : m_profile.tube ? "tube" : "none");
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
        excluded[id]=model.LL_GetData(id).GetParentID()==BI_NONE || ShellName(name) ||
            NameHas(name,"scope") || NameHas(name,"silencer") || NameHas(name,"launcher") ||
            NameHas(name,"grenade");
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
    IRenderVisual* visual=HudCopy(m_profile.visual.c_str());
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* sample=visual ? visual->dcast_PKinematics() : nullptr;
    if (animated && sample && sample->LL_BoneCount()==count)
        if (LPCSTR key=StartReloadMotion(*animated,hud))
        {
            for (int step=0;step<360;++step)
            {
                animated->LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
                sample->CalculateBones_Invalidate();
                sample->CalculateBones(TRUE);
                for (u16 id=0;id<count;++id)
                {
                    // Rounds and clips too: only for the clip evidence below.
                    if (excluded[id] && !(loose[id] && model.LL_GetData(id).GetParentID()<count)) continue;
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    Fmatrix rest, now;
                    rest.mul_43(Fmatrix().invert(bind[parent]),bind[id]);
                    now.mul_43(Fmatrix().invert(sample->LL_GetTransform(parent)),sample->LL_GetTransform(id));
                    Fvector moved; moved.sub(now.c,rest.c);
                    bind[parent].transform_dir(moved);
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
    if (visual) ::Render->model_Delete(visual);
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
        if (!MeasureBone(model,id,box,true)) return false;
        Fvector centre; box.getcenter(centre);
        const Fvector at=m_profile.has_trigger_point ? m_profile.trigger_point : Fvector().set(0.f,0.f,0.f);
        if (centre.distance_to(at)>=0.3f) return true;
        return m_profile.has_trigger_point && centre.y>=m_profile.trigger_point.y+0.03f;
    };
    for (u16 id=0;id<count && !m_profile.evidence.top_loaded;++id)
        if ((!excluded[id] || loose[id]) && travel[id]>=0.04f && travel[id]<=0.6f && leave[id].y>0.5f && rests_above(id))
        {
            m_profile.evidence.top_loaded=true;
            Msg("* [VR weapon] %s: %s comes in from above in %s (a clip or rounds): internal magazine",
                m_profile.section.c_str(),model.LL_BoneName_dbg(id),source);
        }
    // The magazine: the part that travels furthest (4 cm or more), not
    // straight back along the bore (that is the bolt or charging handle).
    u16 magazine=BI_NONE;
    for (u16 id=0;id<count;++id)
    {
        Fbox box;
        // Not a part that leaves upward: a stripper clip or the rounds of an
        // internal magazine come in from above.
        if (excluded[id] || travel[id]<0.04f || travel[id]>0.6f || leave[id].z<-0.8f ||
            (leave[id].y>0.5f && rests_above(id)) || !MeasureBone(model,id,box,true)) continue;
        if (magazine==BI_NONE || travel[id]>travel[magazine]) magazine=id;
    }
    // A parent carried along with it is the magazine, not the child.
    while (magazine!=BI_NONE)
    {
        const u16 parent=model.LL_GetData(magazine).GetParentID();
        if (parent>=count || excluded[parent] || travel[parent]<0.75f*travel[magazine]) break;
        magazine=parent;
    }
    if (magazine!=BI_NONE)
    {
        // Into the well: against the way it first left it.
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
        Fbox end_box, cover;
        if (end==BI_NONE || depth<4 || !MeasureBone(model,end,end_box,false)) continue;
        // 2.5 cm round it: a belt fed from the side (RPD) ends beside the cover.
        cover=box; cover.grow(0.025f);
        Fvector tray; end_box.getcenter(tray);
        if (!cover.contains(tray)) continue;
        xr_vector<u16> chain;
        for (u16 a=end; a<count && a!=magazine; a=model.LL_GetData(a).GetParentID()) chain.push_back(a);
        std::reverse(chain.begin(),chain.end());
        const size_t loose=std::max<size_t>(1,chain.size()*3/4-1);
        Fbox mouth;
        if (!MeasureBone(model,chain[loose-1],mouth,false)) continue;
        Fvector box_end; mouth.getcenter(box_end);
        m_profile.belt.used=true;
        m_profile.belt.tray={tray.x,tray.y,tray.z};
        m_profile.belt.box_end={box_end.x,box_end.y,box_end.z};
        m_profile.belt_free_bone=model.LL_BoneName_dbg(chain[loose]);
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
            (magazine!=BI_NONE && Descends(model,id,magazine)) || ShellName(name) ||
            NameHas(name,"scope") || NameHas(name,"silencer") || NameHas(name,"launcher") ||
            NameHas(name,"grenade");
    }
    // The trigger and the receiver chain carrying it: a shot animation that
    // kicks the receiver back must not make the whole gun the bolt.
    for (u16 id=trigger; id<count; id=model.LL_GetData(id).GetParentID()) excluded[id]=1;
    IRenderVisual* visual=HudCopy(m_profile.visual.c_str());
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* sample=visual ? visual->dcast_PKinematics() : nullptr;
    // How far each part moves straight back relative to its parent, in the
    // shot animation and in the reload: the first of each that moves one.
    xr_vector<float> rear_shot(count,0.f), rear_reload(count,0.f);
    // And how far each turns about the bore (a bolt-action's handle) and
    // across the weapon (a lever) in the shot animation.
    xr_vector<float> turn_bore(count,0.f), turn_side(count,0.f);
    const auto sample_motion=[&](std::initializer_list<LPCSTR> keys, xr_vector<float>& rear, bool turns) -> LPCSTR
    {
        if (!animated || !sample || sample->LL_BoneCount()!=count) return nullptr;
        for (LPCSTR key:keys)
        {
            if (!PlayHudMotion(*animated,hud,key)) continue;
            std::fill(rear.begin(),rear.end(),0.f);
            for (int step=0;step<240;++step)
            {
                animated->LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
                sample->CalculateBones_Invalidate();
                sample->CalculateBones(TRUE);
                for (u16 id=0;id<count;++id)
                {
                    if (excluded[id]) continue;
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    Fmatrix to_parent; to_parent.invert(sample->LL_GetTransform(parent));
                    Fvector now; to_parent.transform_tiny(now,sample->LL_GetTransform(id).c);
                    Fmatrix bind_parent; bind_parent.invert(bind[parent]);
                    Fvector rest; bind_parent.transform_tiny(rest,bind[id].c);
                    // Travel in the weapon's frame, using the parent's bind axes.
                    Fvector moved; moved.sub(now,rest);
                    bind[parent].transform_dir(moved);
                    const float back=-moved.z;
                    if (back>0.f && back>2.f*_sqrt(moved.x*moved.x+moved.y*moved.y))
                        rear[id]=std::max(rear[id],back);
                    if (turns)
                    {
                        Fmatrix now_local, rest_local;
                        now_local.mul_43(to_parent,sample->LL_GetTransform(id));
                        rest_local.mul_43(bind_parent,bind[id]);
                        turn_bore[id]=std::max(turn_bore[id],TurnAbout(rest_local,now_local,bind[parent],2));
                        turn_side[id]=std::max(turn_side[id],TurnAbout(rest_local,now_local,bind[parent],0));
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
    if (chosen!=BI_NONE && animated && sample && sample->LL_BoneCount()==count)
    {
        float length=0.f;
        evidence.has_shot_l=PlayHudMotion(*animated,hud,"anm_shot_l",&length);
        if (evidence.has_shot_l && length>0.f)
        {
            animated->LL_UpdateTracks(0.f,true,true);
            animated->LL_UpdateTracks(length*0.97f,true,true);
            sample->CalculateBones_Invalidate();
            sample->CalculateBones(TRUE);
            const float full=std::max(rear_shot[chosen],rear_reload[chosen]);
            evidence.shot_l_holds_back=full>0.005f && RearInReceiver(*sample,bind,chosen,receiver)>=0.6f*full;
        }
        // Worked by hand along the path its own shot animation moves it.
        if (shot_source && evidence.bolt_turn_deg>=vr::bolt_action_turn_deg)
        {
            // The knob rides the deepest of the turning part and the bolt.
            const u16 knob=turner!=BI_NONE && Descends(model,turner,chosen) ? turner : chosen;
            RecordActionPath(*animated,*sample,model,bind,shot_source,knob,GrabPoint(model,knob,false,centre_x),receiver,chosen,true);
        }
        else if (shot_source && evidence.lever_part)
            RecordActionPath(*animated,*sample,model,bind,shot_source,lever,LowestPoint(model,lever),receiver,chosen,false);
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
                if (!PlayHudMotion(*animated,hud,shot_source)) break;
                // Its widest swing over the shot is its open pose.
                float widest=-1.f; Fmatrix open=bind[id];
                for (int step=0;step<120;++step)
                {
                    animated->LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
                    sample->CalculateBones_Invalidate();
                    sample->CalculateBones(TRUE);
                    Fmatrix rel; rel.invert(sample->LL_GetTransform(receiver)); rel.mulA_43(bind[receiver]);
                    Fmatrix pose; pose.mul_43(rel,sample->LL_GetTransform(id));
                    const float off=pose.k.distance_to(bind[id].k)+pose.j.distance_to(bind[id].j);
                    if (off>widest) { widest=off; open=pose; }
                }
                evidence.dust_cover_part=true;
                m_profile.dust_cover_bone=model.LL_BoneName_dbg(id);
                m_profile.dust_cover_open=open;
                m_profile.dust_cover_grab=centre;
            }
    }
    if (visual) ::Render->model_Delete(visual);
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
        m_mechanism.travel_limit=m_profile.cycle_travel;
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
    if (m_profile.action_path.Valid())
    {
        const vr::V3 start=m_profile.action_path.GrabAt(0.f);
        m_profile.cycle_grab.set(start.x,start.y,start.z);
        m_profile.cycle_travel=clampr(m_profile.action_path.RearMax(),0.01f,0.15f);
    }
    m_profile.check_cycle_bind=false;
    m_mechanism.travel_limit=m_profile.cycle_travel;
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
    if (m_profile.action_path.Valid())
        Msg("* [VR weapon] %s: worked along its shot animation: %.0f deg turn, %u keys, %.0f mm path, %.0f mm back",
            m_profile.section.c_str(),evidence.bolt_turn_deg,unsigned(m_profile.action_path.grab.size()),
            m_profile.action_path.length*1000.f,m_profile.action_path.RearMax()*1000.f);
}

void CVrWeaponInteraction::RecordActionPath(IKinematicsAnimated& animated, IKinematics& sample, IKinematics& model,
    const xr_vector<Fmatrix>& bind, LPCSTR key, u16 knob, const Fvector& grab, u16 receiver, u16 moving_bolt, bool knob_rear)
{
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    float length=0.f;
    if (!PlayHudMotion(animated,hud,key,&length)) return;
    const int steps=std::max(2,std::min(240,int(length*60.f)+1));
    Fmatrix unbind; unbind.invert(bind[knob]);
    Fvector grab_local; unbind.transform_tiny(grab_local,grab);
    // Every bone's model-space pose in the receiver's frame, per step.
    xr_vector<Fvector> grabs; xr_vector<float> rears; xr_vector<Fmatrix> frames;
    grabs.reserve(steps); rears.reserve(steps); frames.reserve(size_t(steps)*count);
    for (int step=0;step<steps;++step)
    {
        animated.LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
        sample.CalculateBones_Invalidate();
        sample.CalculateBones(TRUE);
        Fmatrix rel; rel.invert(sample.LL_GetTransform(receiver)); rel.mulA_43(bind[receiver]);
        Fmatrix at; at.mul_43(rel,sample.LL_GetTransform(knob));
        Fvector g; at.transform_tiny(g,grab_local);
        grabs.push_back(g);
        // Back along the bore: the knob's own travel for a bolt (its lift
        // turns about the bore and moves nothing back); the bolt's for a
        // lever, whose knob swings down.
        rears.push_back(knob_rear ? std::max(0.f,grab.z-g.z) :
            (moving_bolt<count ? std::max(0.f,RearInReceiver(sample,bind,moving_bolt,receiver)) : 0.f));
        for (u16 id=0;id<count;++id) { Fmatrix pose; pose.mul_43(rel,sample.LL_GetTransform(id)); frames.push_back(pose); }
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
        if (vr::AnimatedGrip() && !m_cloud.empty()) SampleAuthoredHands();
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

void CVrWeaponInteraction::ClassifyMechanics(IKinematics& model)
{
    vr::MechanicsEvidence& e=m_profile.evidence;
    e.pistol_class=smart_cast<CWeaponPistol*>(&m_weapon)!=nullptr;
    e.revolver_class=smart_cast<CWeaponRevolver*>(&m_weapon)!=nullptr;
    e.shotgun_class=m_weapon.VrShellLoader() && (!pSettings->line_exist(m_profile.section,"weapon_class") ||
        !xr_strcmp(pSettings->r_string(m_profile.section,"weapon_class"),"shotgun"));
    e.bm16_class=smart_cast<CWeaponBM16*>(&m_weapon)!=nullptr;
    e.launcher_class=smart_cast<CWeaponRG6*>(&m_weapon)!=nullptr || smart_cast<CWeaponRPG7*>(&m_weapon)!=nullptr;
    LPCSTR hud=m_weapon.HudSection().c_str();
    e.tri_state=m_weapon.IsTriStateReload() && pSettings->line_exist(hud,"anm_open") &&
        pSettings->line_exist(hud,"anm_add_cartridge") && pSettings->line_exist(hud,"anm_close");
    e.smg_class=pSettings->line_exist(m_profile.section,"weapon_class") &&
        !xr_strcmp(pSettings->r_string(m_profile.section,"weapon_class"),"smg");
    e.detachable_magazine=m_profile.detachable;
    e.belt=m_profile.belt.used;
    e.pump_part=m_profile.pump_bone.size()!=0;
    e.break_hinge=m_profile.break_action;
    e.capacity=unsigned(std::max(0,m_weapon.iMagazineSize));
    e.barrels=m_profile.break_action ? (m_profile.shell_bones[0]!=m_profile.shell_bones[1] ? 2u : 1u) : 0u;
    m_mechanics=vr::Classify(e);
    // Overrides: [section] in configs\vr\vr_weapon_mechanics.ltx, else its
    // parent section's.
    static CInifile* overrides=nullptr;
    static bool loaded=false;
    if (!loaded)
    {
        loaded=true;
        string_path path;
        if (FS.exist(path,"$game_config$","vr\\vr_weapon_mechanics.ltx")) overrides=xr_new<CInifile>(path,TRUE,TRUE,FALSE);
        else Msg("! [VR mechanics] configs\\vr\\vr_weapon_mechanics.ltx is not installed: no bolt catches or hold-open from it");
    }
    shared_str section=m_weapon.cNameSect();
    if (overrides && !overrides->section_exist(section))
    {
        char names[8][64];
        const shared_str own=section;
        shared_str parent=pSettings->line_exist(own,"parent_section") ? pSettings->r_string(own,"parent_section") : own;
        for (const shared_str& base:{own,parent})
        {
            const int n=vr::OverrideNames(base.c_str(),names,8);
            for (int i=0;i<n && !overrides->section_exist(section);++i)
                if (overrides->section_exist(names[i])) section=names[i];
            if (overrides->section_exist(section)) break;
        }
    }
    if (overrides && overrides->section_exist(section))
        for (const auto& item:overrides->r_section(section).Data)
            if (!vr::ApplyMechanicsOverride(m_mechanics,item.first.c_str(),item.second.c_str()))
                Msg("! [VR mechanics] %s: unknown override %s = %s",section.c_str(),item.first.c_str(),item.second.c_str());
    // Not loaded in VR yet: a revolver's cylinder and a muzzle-loaded
    // launcher use the game's own reload.
    if (m_mechanics.feed==vr::FeedKind::Cylinder || m_mechanics.feed==vr::FeedKind::Muzzle)
        m_mechanics.vr_reload_supported=false;
    // An audited profile (configs\vr\vr_weapons.ltx) keeps its own mechanism.
    if (!m_profile.derived)
    {
        Msg("* [VR mechanics] %s: audited profile; %s",m_profile.section.c_str(),vr::FamilyName(m_mechanics.family));
        return;
    }
    // The profile follows the classification.
    m_profile.manual_cycle=m_mechanics.action==vr::ActionKind::ManualBolt || m_mechanics.action==vr::ActionKind::Lever;
    if (!m_profile.manual_cycle) m_profile.action_path=vr::ActionPath();
    m_profile.slide_stop=m_mechanics.has_slide_stop || m_mechanics.has_bolt_catch;
    m_mechanism.locks_on_empty=m_mechanics.supports_last_round_hold_open;
    if (!m_mechanics.has_dust_cover) m_profile.dust_cover_bone="";
    // A bolt-action's internal magazine loads a round at a time through
    // the open action, at the ejection port.
    m_profile.internal_rounds=m_mechanics.feed==vr::FeedKind::InternalRounds && m_mechanics.vr_reload_supported &&
        m_profile.cycle_bone.size();
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
        // A round of the model's own (a cartridge bone) to show in the hand.
        for (u16 id=0;id<model.LL_BoneCount() && !m_profile.shell_bones[1].size();++id)
        {
            LPCSTR name=model.LL_BoneName_dbg(id);
            Fbox box;
            if (!ShellName(name) || NameHas(name,"gilza") || !MeasureBone(model,id,box,true)) continue;
            m_profile.shell_bones[1]=name;
            box.getcenter(m_profile.shell_center[1]);
            m_profile.shell_box_half.set(0.006f,0.006f,0.04f);
        }
        // No loose round (an SKS's are one mesh with their clip): its one
        // case stands in for a single round.
        for (u16 id=0;id<model.LL_BoneCount() && !m_profile.shell_bones[1].size();++id)
        {
            LPCSTR name=model.LL_BoneName_dbg(id);
            Fbox box;
            if (!NameHas(name,"gilza") || !MeasureBone(model,id,box,true)) continue;
            m_profile.shell_bones[1]=name;
            box.getcenter(m_profile.shell_center[1]);
            m_profile.shell_box_half.set(0.006f,0.006f,0.04f);
        }
    }
    if (m_profile.manual_cycle || m_profile.pump_bone.size()) m_weapon.VrDropActionShotLayers();
    // Left to the game's own reload: no VR loader stays on to block it.
    if (!m_mechanics.vr_reload_supported) m_profile.tube=m_profile.internal_rounds=false;
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
    Msg("* [VR mechanics] %s: %s, %s, %s(%u)%s%s%s%s%s%s; bolt turn %.0f deg%s%s",m_profile.section.c_str(),
        vr::FamilyName(m_mechanics.family),vr::ActionName(m_mechanics.action),vr::FeedName(m_mechanics.feed),
        m_mechanics.capacity,
        m_mechanics.supports_last_round_hold_open ? ", holds open" : "",
        m_mechanics.has_bolt_catch ? ", bolt catch" : "",
        m_mechanics.has_slide_stop ? ", slide stop" : "",
        m_mechanics.has_charging_handle ? ", charging handle" : "",
        m_mechanics.has_dust_cover ? ", dust cover" : m_mechanics.handle_notch ? ", handle notch" : "",
        m_mechanics.requires_manual_charging ? ", charged by hand" : "",
        e.bolt_turn_deg,e.has_shot_l ? (e.shot_l_holds_back ? ", last shot holds open" : ", last shot closes") : "",
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
    IRenderVisual* visual=HudCopy(m_profile.visual.c_str());
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* sample=visual ? visual->dcast_PKinematics() : nullptr;
    if (animated && sample && sample->LL_BoneCount()==count && bind.size()>=count)
        for (LPCSTR key:{"anm_shots","anm_shot","anm_shot_1","anm_shot_2","anm_pump","anm_open","anm_close",
            "anm_close_empty","anm_add_cartridge","anm_reload","anm_reload_1","anm_reload_2","anm_reload_empty"})
        {
            if (!PlayHudMotion(*animated,hud,key)) continue;
            sampled=true;
            for (int step=0;step<180;++step)
            {
                animated->LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
                sample->CalculateBones_Invalidate();
                sample->CalculateBones(TRUE);
                for (u16 id=0;id<count;++id)
                {
                    const u16 parent=model.LL_GetData(id).GetParentID();
                    if (parent>=count) continue;
                    Fmatrix rest, now;
                    rest.mul_43(Fmatrix().invert(bind[parent]),bind[id]);
                    now.mul_43(Fmatrix().invert(sample->LL_GetTransform(parent)),sample->LL_GetTransform(id));
                    Fvector moved; moved.sub(now.c,rest.c);
                    bind[parent].transform_dir(moved);
                    if (-moved.z>2.f*_sqrt(moved.x*moved.x+moved.y*moved.y)) back[id]=std::max(back[id],-moved.z);
                    Fquaternion a, b; a.set(rest); b.set(now);
                    const float dot=_abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);
                    turn[id]=std::max(turn[id],2.f*acosf(clampr(dot,0.f,1.f)));
                }
            }
        }
    if (visual) ::Render->model_Delete(visual);
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
        const auto round_name=[](LPCSTR name)
        {
            return ShellName(name) || NameHas(name,"grenade") || NameHas(name,"granat") || NameHas(name,"vog") ||
                NameHas(name,"40mm");
        };
        for (u16 id=0;id<count && shells<2;++id)
            if (id!=hinge && Descends(model,id,hinge) && round_name(model.LL_BoneName_dbg(id)) &&
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
                if (Descends(model,id,barrels) || !MeasureBone(model,id,box,true) ||
                    !(ShellName(name) || NameHas(name,"grenade") || NameHas(name,"granat") ||
                    NameHas(name,"vog") || NameHas(name,"40mm"))) continue;
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
        if (!m_profile.shell_bones[1].size() && !NameHas(name,"gilza"))
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
        bool skip=magazine!=BI_NONE && Descends(model,id,magazine);
        for (u16 bone=id;!skip && bone!=BI_NONE && bone<model.LL_BoneCount();
             bone=model.LL_GetData(bone).GetParentID())
        {
            LPCSTR name=model.LL_BoneName_dbg(bone);
            skip=ShellName(name) || NameHas(name,"scope") || NameHas(name,"silencer") ||
                NameHas(name,"launcher") || NameHas(name,"grenade");
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
    IRenderVisual* visual=HudCopy(m_profile.visual.c_str());
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* sample=visual ? visual->dcast_PKinematics() : nullptr;
    if (animated && sample && sample->LL_BoneCount()==count && StartReloadMotion(*animated,hud))
    {
        Fmatrix to_seat; to_seat.invert(bind[magazine]);
        track.reserve(360);
        for (int step=0;step<360;++step)
        {
            animated->LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
            sample->CalculateBones_Invalidate();
            sample->CalculateBones(TRUE);
            Fmatrix relative, now, root;
            relative.mul_43(Fmatrix().invert(sample->LL_GetTransform(parent)),sample->LL_GetTransform(magazine));
            now.mul_43(bind[parent],relative);
            root.mul_43(now,to_seat);
            track.push_back(root);
        }
    }
    if (visual) ::Render->model_Delete(visual);
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
namespace
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
    for (u16 id=0;id<count;++id)
    {
        // As BuildCloud: no loose shells or optional attachments. Hinged
        // parts (covers) are left out: posed open they would leave a
        // phantom closed surface.
        bool skip=false;
        for (u16 bone=id;!skip && bone!=BI_NONE && bone<count;bone=model.LL_GetData(bone).GetParentID())
        {
            LPCSTR name=model.LL_BoneName_dbg(bone);
            skip=ShellName(name) || NameHas(name,"scope") || NameHas(name,"silencer") ||
                NameHas(name,"launcher") || NameHas(name,"grenade");
            for (int part=0;!skip && part<m_profile.part_count;++part)
                skip=m_profile.part_bones[part].size() && !xr_strcmp(m_profile.part_bones[part].c_str(),name);
            if (model.LL_GetData(bone).GetParentID()==bone) break;
        }
        if (skip) continue;
        if (magazine!=BI_NONE && InMagazine(model,id,magazine)) model.EnumBoneVertices(to_magazine,id);
        else if (magazine!=BI_NONE && Descends(model,id,magazine)) continue;  // a spare
        else if (moving!=BI_NONE && Descends(model,id,moving)) model.EnumBoneVertices(to_cycle,id);
        else model.EnumBoneVertices(to_receiver,id);
    }
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
    g_contact_jobs.fetch_add(1);
    std::thread([job,contact,timer,axis,section=std::string(m_profile.section.c_str()),
        receiver=std::move(receiver),cycle=std::move(cycle),held=std::move(held)]() mutable
    {
    if (held.size()>=4) contact->magazine_surface.Build(held,0.0025f,0.012f);
    contact->receiver.Build(receiver,0.0025f,0.012f);
    // Where the magazine really enters the receiver.
    contact->path.MeasureMouth(contact->receiver,contact->magazine.samples);
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
            m_profile.hand_grip_center=centre;
            m_profile.hand_grip_angle=rake;
            m_profile.hand_grip_radius=clampr(spread[size_t(0.9f*float(spread.size()-1))],0.008f,0.03f);
            m_profile.has_hand_grip_center=true;
            if (m_profile.automatic || !m_profile.grip_offset.square_magnitude())
                m_profile.grip_offset=centre;
        }
    }
    // Magazine well axis from the magazine mesh itself: the line from the
    // centre of its lowest slab to the centre of its highest (feed) slab.
    // Insertion and the held pose follow it; nothing is assumed per weapon.
    const u16 magazine=m_profile.detachable ? model.LL_BoneID(m_profile.mag_bone.c_str()) : u16(BI_NONE);
    Fbox box;
    if (magazine!=BI_NONE && MeasureMagazine(model,magazine,box))
    {
        SlabCentroids slabs;
        const float height=box.max.y-box.min.y;
        slabs.low=box.min.y+height*0.15f;
        slabs.high=box.max.y-height*0.15f;
        for (u16 id=0;id<model.LL_BoneCount();++id)
            if (InMagazine(model,id,magazine)) model.EnumBoneVertices(slabs,id);
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
                        if (InMagazine(model,id,magazine)) model.EnumBoneVertices(walls,id);
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
                if (m_profile.explicit_well_axis) axis=m_profile.well_axis;
                else m_profile.well_axis=axis;
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

bool CVrWeaponInteraction::Mode() const
{
    // Asked many times a frame: the local actor by pointer, without a cast.
    const CObject* parent=m_weapon.H_Parent();
    return m_supported && g_vr_owns_present && IsGameTypeSingle() && g_actor && parent &&
        parent==static_cast<const CObject*>(g_actor) && parent==Level().CurrentControlEntity() &&
        m_weapon.VrPhysicalMechanismAvailable();
}

Fvector CVrWeaponInteraction::GripLocal() const
{
    // Where the controller sits on the weapon: the palm of the first-person
    // animation's weapon hand when there is one (mirrored for a left hand),
    // so the drawn hand lies on the real hand; otherwise the measured grip.
    if (vr::AnimatedGrip() && !m_cloud.empty())
    {
        SampleAuthoredHands();
        if (m_authored[1].valid && !m_palm_checked)
        {
            // Against the measured grip, or the estimate from the trigger.
            const Fvector& measured=m_profile.has_hand_grip_center ? m_profile.hand_grip_center : m_profile.grip_offset;
            m_palm_checked=true;
            m_palm_trusted=m_authored[1].palm.distance_to(measured)<=0.08f;
            Msg("* [VR weapon] %s: hand from the animation (%.3f %.3f %.3f), grip measured (%.3f %.3f %.3f)%s",
                m_profile.section.c_str(),m_authored[1].palm.x,m_authored[1].palm.y,m_authored[1].palm.z,
                measured.x,measured.y,measured.z,
                m_palm_trusted ? "" : "; too far apart, the measured grip is used");
        }
        if (m_authored[1].valid && m_palm_trusted)
        {
            // Mirrored about the bore, which need not be the model's x=0.
            Fvector palm=m_authored[1].palm;
            const float centre=m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f;
            if (!m_hand) palm.x=2.f*centre-palm.x;
            return palm;
        }
    }
    return m_profile.grip_offset;
}

bool CVrWeaponInteraction::Transform(Fmatrix& world) const
{
    if (!Mode() || m_holstered) return false;
    if (m_carried)
    {
        // Passed to the other hand: it keeps the weapon where it took it.
        Fmatrix hand;
        if (!vr::HandTransform(HandOf(1),false,hand)) return false;
        world.mul_43(hand,m_carry);
        return true;
    }
    Fmatrix grip,aim;
    if (!vr::HandTransform(HandOf(1),false,grip) || !vr::HandTransform(HandOf(1),true,aim)) return false;
    world=aim;
    world.c.set(0,0,0);
    if (m_break_hand)
    {
        // The support hand works the barrels; it no longer steers the gun.
        Fmatrix rotated; rotated.mul_43(world,m_break_receiver);
        world=rotated;
        world.c.set(0,0,0);
    }
    else if (m_supporting && !m_support_cup && !BarrelsOpen())
    {
        // Fore-end hold: the weapon hand stays put and the weapon's own
        // grip-to-fore-end line passes through the support hand. A cup grip
        // below the weapon hand leaves the aim to the weapon hand.
        Fmatrix left;
        Fvector have;
        have.sub(m_profile.support_offset,GripLocal());
        if (RearSupport()) have.sub(m_authored[0].palm,GripLocal());
        if (have.square_magnitude()<0.01f) have.set(0.f,0.f,1.f);
        if (vr::HandTransform(HandOf(0),false,left))
        {
            Fvector want; want.sub(left.c,grip.c);
            world.transform_dir(have);
            if (want.square_magnitude()>0.01f && have.square_magnitude()>EPS_S)
            {
                want.normalize(); have.normalize();
                Fvector axis; axis.crossproduct(have,want);
                const float sine=axis.magnitude();
                if (sine>0.0001f)
                {
                    axis.div(sine);
                    Fmatrix rotation, rotated;
                    rotation.rotation(axis,atan2f(sine,have.dotproduct(want)));
                    rotated.mul_43(rotation,world);
                    world=rotated;
                    world.c.set(0,0,0);
                }
            }
        }
    }
    // Recoil turns the weapon about the grip, muzzle up and aside, and
    // pushes it back along the bore.
    DecayRecoil();
    if (m_recoil_pitch>1e-5f || _abs(m_recoil_yaw)>1e-5f)
    {
        const Fvector i=world.i, j=world.j, k=world.k;
        world.k.set(k).mul(_cos(m_recoil_pitch)).mad(j,_sin(m_recoil_pitch));
        world.j.set(j).mul(_cos(m_recoil_pitch)).mad(k,-_sin(m_recoil_pitch));
        world.k.mul(_cos(m_recoil_yaw)).mad(i,_sin(m_recoil_yaw)).normalize();
        world.i.crossproduct(world.j,world.k).normalize();
        world.j.crossproduct(world.k,world.i).normalize();
    }
    Fvector offset=GripLocal();
    world.transform_dir(offset);
    world.c.sub(grip.c,offset);
    world.c.mad(world.k,-m_recoil_back);
    return true;
}

void CVrWeaponInteraction::DecayRecoil() const
{
    const u32 now=Device.dwTimeGlobal;
    if (now==m_recoil_time) return;
    const float dt=_min(float(now-m_recoil_time)*0.001f,0.25f);
    m_recoil_time=now;
    const float keep=expf(-m_recoil_settle*dt);
    m_recoil_pitch*=keep; m_recoil_yaw*=keep; m_recoil_back*=keep;
}

namespace
{
// A cartridge's muzzle energy (joules), from the calibre in its section
// name; 5.45x39 when it is not recognised. Longer, more specific names first.
float CartridgeEnergy(const char* ammo)
{
    static const struct { const char* key; float joules; } calibres[]={
        {"12.7x108",18000.f},{"14.5x114",30000.f},{"23x75",7000.f},{".338",6500.f},{"338lapua",6500.f},
        {"12x76",4500.f},{"12x70",4000.f},{"20x70",2500.f},{"7.92x57",3800.f},{"7.62x54",3600.f},
        {"pkm",3600.f},{"7.62x51",3400.f},{".308",3400.f},{"12.7x55",2500.f},{"6.8x",2400.f},
        {".300",2200.f},{"7.62x39",2050.f},{"5.56x45",1750.f},{"5.45x39",1350.f},{"9x39",700.f},
        {".44",1100.f},{".357",800.f},{"9x21",600.f},{"5.45x18",150.f},{"9x19",500.f},{"11.43x23",550.f},
        {".45",550.f},{"7.62x25",500.f},{"5.7x28",400.f},{"4.6x30",400.f},{"7.62x38",300.f},{"9x18",300.f},
    };
    for (const auto& calibre:calibres)
        if (strstr(ammo,calibre.key)) return calibre.joules;
    return 1350.f;
}
}

void CVrWeaponInteraction::OnShot()
{
    // 120% of the setting is the base kick (the setting 1 kicks as 1.5 did).
    const float scale=1.2f*vr::Recoil();
    if (!Mode() || scale<=0.f) return;
    DecayRecoil();
    // The kick follows the cartridge's momentum (the square root of its
    // energy, against 5.45x39) and the weapon's weight (against the AK74's
    // 3.3 kg): an AK74 kicks 1.2 degrees, a PM about 1.5, an SVD 1.8, a
    // 12-gauge 2.1. Without the support hand on it a weapon kicks harder, the
    // more so the longer it is (the lever on the wrist): a pistol 20% more,
    // a 0.6 m SMG 2 times its supported kick, a 0.9 m rifle 2.5 times, a
    // 1.2 m one 3 times;
    // a long gun also throws further aside, climbs further and settles more
    // slowly in one hand. A suppressor takes 15% off.
    const char* ammo=m_weapon.m_ammoTypes.empty() ? "" :
        m_weapon.m_ammoTypes[std::min<size_t>(m_weapon.m_ammoType,m_weapon.m_ammoTypes.size()-1)].c_str();
    const float calibre=clampr(_sqrt(CartridgeEnergy(ammo)/1350.f),0.4f,3.f);
    const float mass=clampr(_sqrt(3.3f/_max(m_weapon.Weight(),0.3f)),0.55f,2.f);
    const bool two_handed=!Handgun();
    float length=0.f;
    if (IRenderVisual* visual=m_weapon.Visual())
    {
        Fvector size; visual->getVisData().box.getsize(size);
        length=_max(size.x,_max(size.y,size.z));
    }
    const float one_hand=two_handed ? 0.67f*(2.f+clampr((length-0.6f)/0.6f,0.f,1.f)) : 1.2f;
    // A two-handed weapon held in both hands kicks as before the stronger
    // base (two thirds of it); held in one hand it takes the full base.
    const float hands=m_supporting ? (two_handed ? 0.67f : 0.87f) : (two_handed ? one_hand : 0.87f*one_hand);
    const float kick=calibre*mass*hands*(m_weapon.IsSilencerAttached() ? 0.85f : 1.f);
    const float up=scale*deg2rad(1.5f)*kick;
    if (vr::ContactDebug()>0)
        Msg("* [VR recoil] %s: %s, %s, length %.2f m, hands x%.2f, kick %.2f, climb %.1f deg",
            m_profile.section.c_str(),two_handed ? "two-handed" : "one-handed",
            m_supporting ? "supported" : "one hand",length,hands,kick,rad2deg(up));
    const bool unsteady=!m_supporting && two_handed;
    const float aside=scale*deg2rad(unsteady ? 0.7f : 0.4f)*kick*::Random.randF(-1.f,1.f);
    m_recoil_settle=unsteady ? 5.f : m_supporting ? 14.f : 12.f;
    m_recoil_pitch=_min(m_recoil_pitch+up,deg2rad(unsteady ? 60.f : 20.f)*scale);
    const float wander=deg2rad(unsteady ? 20.f : 6.f)*scale;
    m_recoil_yaw=clampr(m_recoil_yaw+aside,-wander,wander);
    m_recoil_back=_min(m_recoil_back+0.006f*kick*scale,(unsteady ? 0.12f : 0.06f)*scale);
    vr::HandHaptic(HandOf(1),clampr(0.15f+0.25f*kick,0.f,1.f));
    if (m_supporting) vr::HandHaptic(HandOf(0),clampr(0.1f+0.15f*kick,0.f,0.7f));
}

bool CVrWeaponInteraction::GenericTransform(Fmatrix& world) const
{
    if (Mode() || !g_vr_owns_present || !IsGameTypeSingle() || !vr::GameplayActive()) return false;
    auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
    if (!actor || actor!=Level().CurrentControlEntity() ||
        actor->inventory().ActiveItem()!=&m_weapon ||
        actor->cam_Active()!=actor->cam_FirstEye()) return false;
    if (m_carried)
    {
        Fmatrix hand;
        if (!vr::HandTransform(HandOf(1),false,hand)) return false;
        world.mul_43(hand,m_carry);
        return true;
    }
    Fmatrix grip;
    if (!vr::HandTransform(HandOf(1),false,grip) || !vr::HandTransform(HandOf(1),true,world)) return false;
    Fmatrix hand, finger;
    // NPC weapon offsets are authored for the right hand bone; a left-held
    // world model uses the controller placement below instead.
    if (HandOf(1)==1 && actor->m_legs_controller.vr_weapon_hand(1,hand,finger))
    {
        // Hold the world model as an NPC does: frame the tracked right hand
        // bone like CWeapon::UpdateXForm, then apply the weapon's own
        // position/orientation offset so its grip sits in the palm. One-hand
        // weapons point along hand-to-finger; two-hand weapons, whose offset
        // assumes the left hand on the fore-end, point along the aim ray.
        Fvector D;
        Fmatrix left;
        // Aim through the support controller itself: the drawn support hand
        // snaps onto the barrel line, so it cannot steer the weapon.
        if (m_supporting && !m_support_cup && vr::HandTransform(HandOf(0),false,left))
            D.sub(left.c,hand.c);
        else if (m_weapon.HandDependence()==hd1Hand) D.sub(finger.c,hand.c);
        else D=world.k;
        if (D.square_magnitude()>EPS_S)
        {
            D.normalize();
            Fvector R; R.crossproduct(hand.j,D);
            if (R.square_magnitude()>EPS_S)
            {
                R.normalize();
                Fvector N; N.crossproduct(D,R); N.normalize_safe();
                Fmatrix attach; attach.identity();
                attach.i=R; attach.j=N; attach.k=D; attach.c=hand.c;
                world.mul_43(attach,m_weapon.m_Offset);
                return true;
            }
        }
    }
    // No outfit arms: approximate the grip from the controller.
    Fvector offset; offset.set(0.f,-0.025f,0.08f);
    world.transform_dir(offset);
    world.c.add(grip.c,offset);
    return true;
}
bool CVrWeaponInteraction::Aiming() const
{
    Fmatrix head, world;
    if (!vr::HeadTransform(head) || !AimTransform(world)) return false;
    const Fvector muzzle=Mode() ? Point(world,m_profile.muzzle) : Point(world,m_weapon.vLoadedFirePoint);
    Fvector to; to.sub(head.c,muzzle);
    // Behind the muzzle (a pistol at arm's length to a rifle's cheek weld),
    // within a sight's height of the bore, and looking along it.
    const float along=to.dotproduct(world.k);
    if (along>-0.15f || along<-1.2f) return false;
    Fvector across; across.mad(to,world.k,-along);
    return across.magnitude()<0.11f && head.k.dotproduct(world.k)>0.9f;
}

bool CVrWeaponInteraction::HandFrame(Fmatrix& frame) const
{
    Fmatrix world, grip;
    if (m_carried || !vr::HandTransform(HandOf(1),false,grip) || !Transform(world)) return false;
    // Pistol grips rake back from the bore; the hand wraps the grip axis.
    const float angle=deg2rad(m_profile.hand_grip_angle);
    frame.identity();
    frame.j.set(world.j).mul(_cos(angle)).mad(world.k,_sin(angle));
    frame.k.set(world.k).mul(_cos(angle)).mad(world.j,-_sin(angle));
    frame.i.crossproduct(frame.j,frame.k);
    // A measured grip centre keeps the palm on the mesh's grip regardless of
    // where the controller point (grip_offset) was calibrated.
    frame.c=m_profile.has_hand_grip_center ? Point(world,m_profile.hand_grip_center) : grip.c;
    return true;
}

bool CVrWeaponInteraction::TriggerPoint(Fvector& world) const
{
    Fmatrix weapon;
    if (!m_profile.has_trigger_point || !Transform(weapon)) return false;
    world=Point(weapon,m_profile.trigger_point);
    return true;
}
void CVrWeaponInteraction::PullTrigger()
{
    // As the desktop fire key: in safe mode the first pull only leaves it.
    if (auto* actor=smart_cast<CActor*>(m_weapon.H_Parent()); actor && actor->is_safemode())
    {
        actor->set_safemode(false);
        return;
    }
    // The action still cycling from the last shot: fire when it is in
    // battery instead of clicking on an empty chamber.
    if (m_profile.chamber && m_mechanism.cycling) { m_fire_queued=Device.dwTimeGlobal; return; }
    m_weapon.FireStart();
}

bool CVrWeaponInteraction::HandOver()
{
    // The weapon hand let go while the other hand holds the weapon: that hand
    // now carries it where it is, and the free hand can work a bolt or slide
    // and take the grip again (TakeBack).
    Fmatrix world, support;
    if (!AimTransform(world) || !vr::HandTransform(HandOf(0),false,support)) return false;
    Fmatrix inverse; inverse.invert(support);
    m_carry.mul_43(inverse,world);
    m_carried=true;
    m_hand=1-m_hand;
    m_supporting=m_support_cup=m_break_hand=false;
    // Velocities are tracked per role; the roles just changed hands.
    m_have_hand[0]=m_have_hand[1]=false;
    m_hand_epoch=0;
    vr::HandHaptic(m_hand,0.15f);
    return true;
}

void CVrWeaponInteraction::TakeBack()
{
    // The free hand closed on the grip again: it holds the weapon, and the
    // hand that carried it stays where it is as the support hand.
    m_carried=false;
    m_hand=1-m_hand;
    m_supporting=true;
    m_support_cup=m_support_barrels=false;
    m_have_hand[0]=m_have_hand[1]=false;
    m_hand_epoch=0;
    Fmatrix world, weapon_hand, support;
    if (AimTransform(world) && vr::HandTransform(HandOf(1),false,weapon_hand) &&
        vr::HandTransform(HandOf(0),false,support))
    {
        const Fvector muzzle=Mode() ? Point(world,m_profile.muzzle) : Point(world,m_weapon.vLoadedFirePoint);
        m_support_along=SupportAlong(weapon_hand.c,muzzle,support.c);
    }
    vr::HandHaptic(m_hand,0.15f);
}

bool CVrWeaponInteraction::CollisionBoxes(Fmatrix& world, const xr_vector<Fobb>*& boxes)
{
    // The measured mesh (weapon without magazine, shells and attachments),
    // 95% of its extent: the HUD model's bounds also span hands and
    // animation room. A long gun gets a receiver box and a barrel box ahead
    // of the trigger, so it does not sweep one fat box.
    if (Transform(world))
    {
        if (m_collision_boxes.empty() && !m_cloud.empty())
        {
            const float split=m_profile.has_trigger_point ? m_profile.trigger_point.z+0.08f : flt_max;
            Fbox parts[2]; parts[0].invalidate(); parts[1].invalidate();
            for (const Fvector& point:m_cloud) parts[point.z>split ? 1 : 0].modify(point);
            for (Fbox& part:parts)
            {
                if (!part.is_valid()) continue;
                Fvector size; part.getsize(size);
                if (size.x<0.005f || size.y<0.005f || size.z<0.005f) continue;
                Fobb box; box.m_rotate.identity();
                part.getcenter(box.m_translate);
                box.m_halfsize.set(size).mul(0.475f);
                m_collision_boxes.push_back(box);
            }
        }
        boxes=&m_collision_boxes;
        return !m_collision_boxes.empty();
    }
    if (!GenericTransform(world) || !m_weapon.Visual()) return false;
    Fbox bounds=m_weapon.Visual()->getVisData().box;
    Fvector size; bounds.getsize(size);
    if (size.x<EPS_S || size.y<EPS_S || size.z<EPS_S) return false;
    m_generic_box.resize(1);
    m_generic_box[0].m_rotate.identity();
    bounds.getcenter(m_generic_box[0].m_translate);
    m_generic_box[0].m_halfsize.set(size).mul(0.475f);
    boxes=&m_generic_box;
    return true;
}

bool CVrWeaponInteraction::Prepare()
{
    if (!g_player_hud || !Mode() || !EnsureVisual()) return false;
    const bool ready=m_authored_sampled && m_authored_hands==g_player_hud->section_name();
    SampleAuthoredHands();
    return !ready;
}

bool CVrWeaponInteraction::PouchHasAmmo() const
{
    // Nothing left to draw: no rounds of the weapon's calibres in the
    // inventory and no loaded spare magazine stowed.
    if (ReserveAvailable()) return true;
    for (int index=0;index<2;++index)
        if (index!=m_mechanism.inserted && index!=m_mechanism.held && !m_mechanism.magazines[index].empty())
            return true;
    return false;
}

void CVrWeaponInteraction::PlayHandling(int sound, const Fvector& position)
{
    // Stock weapons only carry whole-reload sounds. Magazine and bolt steps
    // use clips cut from the PM (pistols) and AK-74 (everything else) reload
    // sounds (gamedata/sounds/vr); the rest short generic sounds. A weapon
    // section can name its own (snd_vr_mag_in, snd_vr_mag_out,
    // snd_vr_slide_back, snd_vr_slide_forward, snd_vr_pump_back,
    // snd_vr_pump_forward, snd_vr_break_open, snd_vr_break_close,
    // snd_vr_shell_in, snd_vr_cover_open, snd_vr_cover_close, snd_vr_belt_in).
    static LPCSTR const keys[SoundCount]={"mag_out","mag_in","slide_back","slide_forward",
        "pump_back","pump_forward","break_open","break_close","shell_in",
        "cover_open","cover_close","belt_in"};
    static LPCSTR const rifle[SoundCount]={"vr\\rifle_mag_out","vr\\rifle_mag_in",
        "vr\\rifle_bolt_back","vr\\rifle_bolt_forward",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\gen_grenload",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close","vr\\rifle_mag_in"};
    static LPCSTR const pistol[SoundCount]={"vr\\pistol_mag_out","vr\\pistol_mag_in",
        "weapons\\winchester1300\\w1300_open","weapons\\pm\\pm_close",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\gen_grenload",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close","vr\\pistol_mag_in"};
    if (sound<0 || sound>=SoundCount) return;
    if (!m_handling_loaded[sound])
    {
        m_handling_loaded[sound]=true;
        string64 key; xr_sprintf(key,"snd_vr_%s",keys[sound]);
        const shared_str& section=m_weapon.cNameSect();
        const auto& defaults=smart_cast<CWeaponPistol*>(&m_weapon) ? pistol : rifle;
        LPCSTR name=pSettings->line_exist(section,key) ? pSettings->r_string(section,key) : defaults[sound];
        // Stock sounds, if the VR clips (gamedata/sounds/vr) are not installed.
        static LPCSTR const stock[SoundCount]={"weapons\\gen_grenload","weapons\\generic_close",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\gen_grenload",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\generic_close"};
        string_path path;
        if (!name || !*name || !FS.exist(path,"$game_sounds$",name,".ogg"))
        {
            Msg("! [VR weapon] %s: handling sound %s not found, using %s",section.c_str(),
                name ? name : "",stock[sound]);
            name=stock[sound];
        }
        if (FS.exist(path,"$game_sounds$",name,".ogg"))
            m_handling[sound].create(name,st_Effect,sg_SourceType);
    }
    if (m_handling[sound]._handle()) m_handling[sound].play_at_pos(m_weapon.H_Parent(),position);
}

void CVrWeaponInteraction::SampleAuthoredHands() const
{
    // The first-person animations already hold every weapon the way an
    // animator posed it: index on the trigger, thumb along the frame, the
    // other hand on the fore-end or cupping a pistol grip. Play the weapon's
    // idle on a private copy of the first-person hands at its first frame and
    // record both hands relative to the weapon, as player_hud places it
    // (anchor bone, then item_position/item_orientation).
    if (!g_player_hud) return;
    const shared_str& hands=g_player_hud->section_name();
    const bool scope_now=m_weapon.IsScopeAttached();
    if (m_authored_sampled && m_authored_hands==hands && m_authored_scope==scope_now) return;
    m_authored_scope=scope_now;
    m_authored_sampled=true;
    m_authored_hands=hands;
    m_authored[0].valid=m_authored[1].valid=false;
    const shared_str& section=m_weapon.HudSection();
    if (!hands.size() || !section.size() || !pSettings->section_exist(section) ||
        !pSettings->line_exist(hands,"visual") || !pSettings->line_exist(section,"item_position") ||
        !pSettings->line_exist(section,"item_orientation")) return;
    player_hud_motion_container* motions=g_player_hud->get_hand_motions(section.c_str());
    player_hud_motion* idle=motions ? motions->find_motion("anm_idle") : nullptr;
    if (!idle || idle->m_animations.empty()) return;
    xr_vector<shared_str> anchors;
    for (const auto& item:pSettings->r_section(hands).Data)
        if (strstr(item.first.c_str(),"ancor_")==item.first.c_str()) anchors.push_back(item.second);
    const bool lead=READ_IF_EXISTS(pSettings,r_bool,section,"lh_lead_gun",false);
    const u16 place=READ_IF_EXISTS(pSettings,r_u16,section,"attach_place_idx",0);
    const size_t anchor_index=lead ? 0 : place;
    if (anchor_index>=anchors.size()) return;
    IRenderVisual* visual=::Render->model_Create(pSettings->r_string(hands,"visual"));
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* model=visual ? visual->dcast_PKinematics() : nullptr;
    const u16 anchor=model ? model->LL_BoneID(anchors[anchor_index]) : BI_NONE;
    Fmatrix to_item, offset; to_item.identity(); offset.identity();
    if (animated && model && anchor!=BI_NONE)
    {
        for (u16 part=0;part<animated->partitions().count();++part)
            animated->PlayCycle(part,idle->m_animations[0].mid,FALSE,0,0,0,1.f);
        animated->LL_UpdateTracks(0.f,true,true);
        model->CalculateBones_Invalidate();
        model->CalculateBones(TRUE);
        xr_vector<Fmatrix> bind;
        model->LL_GetBindTransform(bind);
        Fvector ypr=pSettings->r_fvector3(section,"item_orientation");
        ypr.mul(PI/180.f);
        offset.setHPB(ypr.x,ypr.y,ypr.z);
        offset.translate_over(pSettings->r_fvector3(section,"item_position"));
        Fmatrix item; item.mul_43(model->LL_GetTransform(anchor),offset);
        to_item.invert(item);
        // A model built off its origin (the Mosin, 15 cm aside and 29 ahead)
        // is held as a whole away from where it is drawn: the first pass
        // finds the hand, and the hold moves onto the measured grip.
        for (int pass=0;pass<2;++pass)
        {
        bool right_found=false;
        for (int side=0;side<2;++side)
        {
            const char s=side ? 'r' : 'l';
            // Thumb, index and middle finger joints of the first-person hands.
            string64 names[3][3];
            for (int joint=0;joint<3;++joint)
            {
                const char* suffix[3]={"","1","2"};
                xr_sprintf(names[0][joint],"%c_finger0%s",s,suffix[joint]);
                xr_sprintf(names[1][joint],"bip01_%c_finger1%s",s,suffix[joint]);
                xr_sprintf(names[2][joint],"bip01_%c_finger2%s",s,suffix[joint]);
            }
            u16 ids[3][3];
            bool found=true;
            for (int f=0;f<3;++f)
                for (int j=0;j<3;++j)
                    found=(ids[f][j]=model->LL_BoneID(names[f][j]))!=BI_NONE && found;
            string16 hand_name; xr_sprintf(hand_name,"%c_hand",s);
            const u16 hand=model->LL_BoneID(hand_name);
            if (!found || hand==BI_NONE || ids[0][0]>=bind.size()) continue;
            const auto at=[&](u16 id) { Fvector p; to_item.transform_tiny(p,model->LL_GetTransform(id).c); return p; };
            AuthoredHandRef& ref=m_authored[side];
            const Fvector wrist=at(hand), index=at(ids[1][0]), middle=at(ids[2][0]);
            ref.knuckles.add(index,middle).mul(0.5f);
            Fvector knuckles; knuckles.sub(ref.knuckles,wrist);
            Fvector index_side; index_side.sub(index,middle);
            knuckles.normalize_safe();
            index_side.mad(knuckles,-index_side.dotproduct(knuckles)).normalize_safe();
            ref.basis[0]=knuckles; ref.basis[1]=index_side;
            ref.basis[2].crossproduct(knuckles,index_side);
            // The handle centre in the fist: behind the knuckles toward the
            // wrist and in from the palm (basis[2] is the palm normal of a
            // right hand, the back of a left one).
            ref.palm=ref.knuckles;
            ref.palm.mad(knuckles,-0.045f).mad(ref.basis[2],side ? 0.025f : -0.025f);
            for (int f=0;f<3;++f)
                for (int j=0;j<3;++j)
                {
                    Fvector& dir=ref.finger[f][j];
                    if (j<2) dir.sub(at(ids[f][j+1]),at(ids[f][j]));
                    else
                    {
                        // The last segment has no child joint: it continues
                        // the bone's own bind direction from its parent.
                        Fmatrix bind_rotation=bind[ids[f][2]]; bind_rotation.c.set(0,0,0);
                        Fmatrix bind_inverse; bind_inverse.invert(bind_rotation);
                        Fvector local, along;
                        along.sub(bind[ids[f][2]].c,bind[ids[f][1]].c);
                        bind_inverse.transform_dir(local,along);
                        Fmatrix posed=model->LL_GetTransform(ids[f][2]);
                        posed.transform_dir(along,local);
                        to_item.transform_dir(dir,along);
                    }
                    dir.normalize_safe();
                }
            // A hand the animation keeps off the weapon (a one-handed idle)
            // gives no reference for it.
            float nearest=flt_max;
            for (const Fvector& point:m_cloud) nearest=std::min(nearest,point.distance_to_sqr(ref.knuckles));
            ref.valid=nearest<0.12f*0.12f;
            right_found=right_found || side==1;
        }
        if (pass || !right_found) break;
        const Fvector& measured=m_profile.has_hand_grip_center ? m_profile.hand_grip_center : m_profile.grip_offset;
        Fvector shift; shift.sub(measured,m_authored[1].palm);
        if (shift.magnitude()<0.12f || shift.magnitude()>0.6f) break;
        to_item.c.add(shift);
        Msg("* [VR weapon] %s: animated hold moved (%.3f %.3f %.3f) onto the grip",
            m_profile.section.c_str(),shift.x,shift.y,shift.z);
        }
    }
    // Keep every bone's local pose and each hand's place on the weapon: the
    // arms model uses the same skeleton (see player_legs_controller).
    m_hands_local.clear(); m_maghand_local.clear();
    m_hands_visual_name=pSettings->r_string(hands,"visual");
    const auto locals=[&](xr_vector<Fmatrix>& out)
    {
        const u16 count=model->LL_BoneCount();
        out.resize(count);
        for (u16 id=0;id<count;++id)
        {
            const u16 parent=model->LL_GetData(id).GetParentID();
            out[id]=model->LL_GetTransform(id);
            if (parent<count)
            {
                Fmatrix inverse; inverse.invert(model->LL_GetTransform(parent));
                out[id].mulA_43(inverse);
            }
        }
    };
    if (animated && model && anchor!=BI_NONE && (m_authored[0].valid || m_authored[1].valid))
    {
        locals(m_hands_local);
        for (int side=0;side<2;++side)
        {
            const u16 hand=model->LL_BoneID(side ? "r_hand" : "l_hand");
            if (hand!=BI_NONE) m_hand_in_item[side].mul_43(to_item,model->LL_GetTransform(hand));
        }
        // The sight line: where the flat-screen camera is when aiming down
        // the sights, in weapon space. player_hud places the weapon at
        // camera * aim offset * hands attach * anchor bone * item offset,
        // with the hands in their aiming idle.
        m_sight.valid=false;
        player_hud_motion* aim=motions->find_motion("anm_idle_aim");
        if (aim && !aim->m_animations.empty())
        {
            for (u16 part=0;part<animated->partitions().count();++part)
                animated->PlayCycle(part,aim->m_animations[0].mid,FALSE,0,0,0,1.f);
            animated->LL_UpdateTracks(0.f,true,true);
            model->CalculateBones_Invalidate();
            model->CalculateBones(TRUE);
        }
        if (attachable_hud_item* hud=m_weapon.HudItemData())
        {
            const hud_item_measures& measures=hud->m_measures;
            Fvector turn=measures.m_hands_attach[1]; turn.mul(PI/180.f);
            Fmatrix attach; attach.setHPB(turn.x,turn.y,turn.z);
            attach.translate_over(measures.m_hands_attach[0]);
            const Fvector& aim_rot=measures.m_hands_offset[1][1];
            Fmatrix aim_offset; aim_offset.identity(); aim_offset.rotateX(aim_rot.x);
            Fmatrix axis_turn; axis_turn.identity(); axis_turn.rotateY(aim_rot.y); aim_offset.mulA_43(axis_turn);
            axis_turn.identity(); axis_turn.rotateZ(aim_rot.z); aim_offset.mulA_43(axis_turn);
            aim_offset.translate_over(measures.m_hands_offset[0][1]);
            Fmatrix item_aim; item_aim.mul_43(model->LL_GetTransform(anchor),offset);
            Fmatrix in_hands; in_hands.mul_43(attach,item_aim);
            Fmatrix in_camera; in_camera.mul_43(aim_offset,in_hands);
            Fmatrix camera; camera.invert(in_camera);
            Fvector direction=camera.k; direction.normalize_safe();
            if (direction.z>0.9f)
            {
                m_sight.eye=camera.c;
                m_sight.dir=direction;
                m_sight.valid=true;
            }
        }
        MeasureSight();
        // The magazine hold: play the reload on the hands and the weapon
        // together and take the frame where the left hand carries the
        // magazine about 4 cm below its seat, on its way into the well.
        player_hud_motion* reload=motions->find_motion("anm_reload");
        if (!reload || reload->m_animations.empty()) reload=motions->find_motion("anm_reload_empty");
        const u16 left=model->LL_BoneID("l_hand");
        if (m_profile.detachable && m_profile.mag_bone.size() && reload && !reload->m_animations.empty() &&
            left!=BI_NONE)
        {
            IRenderVisual* item_visual=::Render->model_Create(m_profile.visual.c_str());
            auto* item_animated=item_visual ? item_visual->dcast_PKinematicsAnimated() : nullptr;
            auto* item_model=item_visual ? item_visual->dcast_PKinematics() : nullptr;
            const u16 mag=item_model ? item_model->LL_BoneID(m_profile.mag_bone.c_str()) : BI_NONE;
            const shared_str item_name=reload->m_base_name!=reload->m_additional_name ?
                reload->m_additional_name : reload->m_animations[0].name;
            const MotionID item_motion=item_animated ? item_animated->ID_Cycle_Safe(item_name) : MotionID();
            if (item_animated && mag!=BI_NONE && item_motion.valid())
            {
                xr_vector<Fmatrix> item_bind;
                item_model->LL_GetBindTransform(item_bind);
                Fmatrix unbind; unbind.invert(item_bind[mag]);
                // The magazine the hand carries may be a spare the reload
                // brings in (an AKS-74U's "magazin_2"), the seated one being
                // hidden meanwhile: any magazine-named part about its size
                // counts, and the held magazine is drawn where that one is.
                xr_vector<u16> carried;
                carried.push_back(mag);
                {
                    Fbox own;
                    Fvector own_size; own_size.set(0,0,0);
                    if (MeasureMagazine(*item_model,mag,own)) own.getsize(own_size);
                    for (u16 id=0;id<item_model->LL_BoneCount();++id)
                    {
                        Fbox box;
                        if (id==mag || !NameHas(item_model->LL_BoneName_dbg(id),"mag") ||
                            !MeasureBone(*item_model,id,box,false)) continue;
                        Fvector size; box.getsize(size);
                        if (_abs(size.y-own_size.y)<=0.5f*own_size.y) carried.push_back(id);
                    }
                }
                for (u16 part=0;part<animated->partitions().count();++part)
                    animated->PlayCycle(part,reload->m_animations[0].mid,FALSE,0,0,0,1.f);
                for (u16 part=0;part<item_animated->partitions().count();++part)
                    item_animated->PlayCycle(part,item_motion,FALSE,0,0,0,1.f);
                float best=flt_max;
                for (int frame=0;frame<480;++frame)
                {
                    const float dt=frame ? 1.f/60.f : 0.f;
                    animated->LL_UpdateTracks(dt,true,true);
                    model->CalculateBones_Invalidate(); model->CalculateBones(TRUE);
                    item_animated->LL_UpdateTracks(dt,true,true);
                    item_model->CalculateBones_Invalidate(); item_model->CalculateBones(TRUE);
                    Fmatrix item_now; item_now.mul_43(model->LL_GetTransform(anchor),offset);
                    const Fmatrix& hand_now=model->LL_GetTransform(left);
                    Fmatrix root;
                    float error=flt_max;
                    for (u16 part:carried)
                    {
                        const Fmatrix& mag_now=item_model->LL_GetTransform(part);
                        Fvector moved; moved.sub(mag_now.c,item_bind[mag].c);
                        const float depth=moved.dotproduct(m_profile.well_axis);
                        Fmatrix root_item; root_item.mul_43(mag_now,unbind);
                        Fmatrix candidate; candidate.mul_43(item_now,root_item);
                        Fvector centre; candidate.transform_tiny(centre,m_profile.mag_box_center);
                        if (depth< -0.09f || depth> -0.015f || hand_now.c.distance_to(centre)>0.15f) continue;
                        // Nearest the hand first, then nearest 4 cm below the seat.
                        const float score=_abs(depth+0.04f)+hand_now.c.distance_to(centre);
                        if (score<error) { error=score; root=candidate; }
                    }
                    if (error>=best) continue;
                    best=error;
                    Fmatrix to_root; to_root.invert(root);
                    m_maghand_in_root.mul_43(to_root,hand_now);
                    locals(m_maghand_local);
                    // The hand's anatomical frame and palm, in hand-bone space.
                    Fmatrix to_hand; to_hand.invert(hand_now);
                    Fvector index, middle;
                    to_hand.transform_tiny(index,model->LL_GetTransform(model->LL_BoneID("bip01_l_finger1")).c);
                    to_hand.transform_tiny(middle,model->LL_GetTransform(model->LL_BoneID("bip01_l_finger2")).c);
                    Fvector knuckles; knuckles.add(index,middle).mul(0.5f);
                    Fvector forward=knuckles; forward.normalize_safe();
                    Fvector side; side.sub(index,middle);
                    side.mad(forward,-side.dotproduct(forward)).normalize_safe();
                    m_maghand_basis[0]=forward; m_maghand_basis[1]=side;
                    m_maghand_basis[2].crossproduct(forward,side);
                    m_maghand_palm=knuckles;
                    m_maghand_palm.mad(forward,-0.045f).mad(m_maghand_basis[2],-0.025f);
                }
                if (best==flt_max) m_maghand_local.clear();
            }
            if (item_visual) ::Render->model_Delete(item_visual);
        }
    }
    // Where the reload's left hand is when it has the cycling part (or a
    // separate charging handle) furthest back: it is on the handle there.
    m_handle_hand_valid=false;
    const shared_str& worked=m_profile.handle_bone.size() ? m_profile.handle_bone : m_profile.cycle_bone;
    if (animated && model && anchor!=BI_NONE && motions && worked.size() && model->LL_BoneID("l_hand")!=BI_NONE)
    {
        player_hud_motion* empty=motions->find_motion("anm_reload_empty");
        if (!empty || empty->m_animations.empty()) empty=motions->find_motion("anm_reload");
        IRenderVisual* item_visual=empty && !empty->m_animations.empty() ? ::Render->model_Create(m_profile.visual.c_str()) : nullptr;
        auto* item_animated=item_visual ? item_visual->dcast_PKinematicsAnimated() : nullptr;
        auto* item_model=item_visual ? item_visual->dcast_PKinematics() : nullptr;
        const u16 part=item_model ? item_model->LL_BoneID(worked.c_str()) : BI_NONE;
        const u16 trigger=item_model ? FindTrigger(*item_model) : BI_NONE;
        const u16 receiver=trigger!=BI_NONE && item_model->LL_GetData(trigger).GetParentID()<item_model->LL_BoneCount() ?
            item_model->LL_GetData(trigger).GetParentID() : u16(0);
        const shared_str item_name=empty && empty->m_base_name!=empty->m_additional_name ?
            empty->m_additional_name : (empty ? empty->m_animations[0].name : shared_str());
        const MotionID item_motion=item_animated ? item_animated->ID_Cycle_Safe(item_name) : MotionID();
        if (item_animated && part!=BI_NONE && item_motion.valid())
        {
            xr_vector<Fmatrix> item_bind;
            item_model->LL_GetBindTransform(item_bind);
            const u16 left=model->LL_BoneID("l_hand");
            const u16 index=model->LL_BoneID("bip01_l_finger1"), middle=model->LL_BoneID("bip01_l_finger2");
            for (u16 p=0;p<animated->partitions().count();++p)
                animated->PlayCycle(p,empty->m_animations[0].mid,FALSE,0,0,0,1.f);
            for (u16 p=0;p<item_animated->partitions().count();++p)
                item_animated->PlayCycle(p,item_motion,FALSE,0,0,0,1.f);
            float furthest=0.02f;
            for (int frame=0;frame<480;++frame)
            {
                const float dt=frame ? 1.f/60.f : 0.f;
                animated->LL_UpdateTracks(dt,true,true);
                model->CalculateBones_Invalidate(); model->CalculateBones(TRUE);
                item_animated->LL_UpdateTracks(dt,true,true);
                item_model->CalculateBones_Invalidate(); item_model->CalculateBones(TRUE);
                const float back=RearInReceiver(*item_model,item_bind,part,receiver);
                if (back<=furthest) continue;
                furthest=back;
                // The palm: behind the knuckles toward the wrist.
                Fvector wrist=model->LL_GetTransform(left).c, knuckles=wrist;
                if (index!=BI_NONE && middle!=BI_NONE)
                    knuckles.add(model->LL_GetTransform(index).c,model->LL_GetTransform(middle).c).mul(0.5f);
                Fvector along; along.sub(knuckles,wrist);
                Fvector palm=knuckles;
                if (along.square_magnitude()>EPS_S) palm.mad(along.normalize(),-0.03f);
                Fmatrix item_now; item_now.mul_43(model->LL_GetTransform(anchor),offset);
                Fmatrix to_item; to_item.invert(item_now);
                Fvector in_item; to_item.transform_tiny(in_item,palm);
                // Into the receiver's rest frame (the reload may tilt it).
                Fmatrix rel; rel.invert(item_model->LL_GetTransform(receiver)); rel.mulA_43(item_bind[receiver]);
                rel.transform_tiny(m_handle_hand,in_item);
                // Back where the handle rests: the hand is followed back by
                // the slide as it is pulled.
                m_handle_hand.z+=back;
                m_handle_hand_valid=true;
            }
        }
        if (item_visual) ::Render->model_Delete(item_visual);
        if (m_handle_hand_valid)
            Msg("* [VR weapon] %s: the reload's hand takes the %s at (%.3f %.3f %.3f); the mesh's point is (%.3f %.3f %.3f)",
                m_weapon.cNameSect().c_str(),worked.c_str(),m_handle_hand.x,m_handle_hand.y,m_handle_hand.z,
                m_profile.cycle_grab.x,m_profile.cycle_grab.y,m_profile.cycle_grab.z);
    }
    if (visual) ::Render->model_Delete(visual);
    Msg("* [VR weapon] %s: magazine hold from the reload animation: %s",m_weapon.cNameSect().c_str(),
        m_maghand_local.empty() ? "no" : "yes");
    Msg("* [VR weapon] %s: first-person grip %s%s", m_weapon.cNameSect().c_str(),
        m_authored[1].valid ? "right" : "-", m_authored[0].valid ? " left" : "");
}

namespace
{
// A sight's glass mesh in a weapon model: Anomaly's LFO lens, reticle and
// dot shaders (models\lfo_*), reticle and red-lens textures, 3D scope
// lenses. dot: the mesh is a red dot.
bool SightGlass(LPCSTR shader, LPCSTR texture, bool& dot, bool& soft)
{
    shader=shader ? shader : ""; texture=texture ? texture : "";
    dot=strstr(shader,"light_dot") || strstr(shader,"reflex") || strstr(texture,"red_lens") ||
        strstr(texture,"crosshair_dot");
    // The soft black lens is a magnified scope's exit-pupil shadow (AUG,
    // SUSAT, G36); red dots have none.
    soft=strstr(shader,"black_soft")!=nullptr;
    return dot || soft || strstr(shader,"lfo_") || strstr(shader,"scope_color") || strstr(shader,"scope_lens") ||
        strstr(texture,"crosshair") || strstr(texture,"_lense");
}
// A scope part's eyepiece: the rear 4 cm of its mesh, the upper 5 cm of
// each 1 cm slice (a mount hangs below the tube), the median slice centre.
// Checked offline on the PSO-1 of the AK-74M and the G3SG1's scope.
bool ScopeEyepiece(const xr_vector<Fvector>& points, Fvector& centre, float& radius, float& front)
{
    if (points.size()<50) return false;
    float back=flt_max; front=-flt_max;
    for (const Fvector& point:points) { back=std::min(back,point.z); front=std::max(front,point.z); }
    if (front-back<0.04f) return false;
    xr_vector<float> xs, ys;
    float size=flt_max;
    for (int slice=0;slice<4;++slice)
    {
        const float low=back+0.01f*slice, high=low+0.01f;
        float top=-flt_max;
        for (const Fvector& point:points) if (point.z>=low && point.z<high) top=std::max(top,point.y);
        if (top==-flt_max) continue;
        float x1=flt_max, x2=-flt_max, y1=flt_max, y2=-flt_max;
        for (const Fvector& point:points)
            if (point.z>=low && point.z<high && point.y>top-0.05f)
            {
                x1=std::min(x1,point.x); x2=std::max(x2,point.x);
                y1=std::min(y1,point.y); y2=std::max(y2,point.y);
            }
        xs.push_back(0.5f*(x1+x2)); ys.push_back(0.5f*(y1+y2));
        size=std::min(size,std::min(x2-x1,y2-y1));
    }
    if (xs.empty()) return false;
    std::sort(xs.begin(),xs.end()); std::sort(ys.begin(),ys.end());
    centre.set(xs[xs.size()/2],ys[ys.size()/2],back);
    radius=0.5f*size;
    return true;
}
}

void CVrWeaponInteraction::MeasureOptics(IKinematics& model)
{
    // The sight's own geometry: its glass meshes (by shader and texture)
    // and the scope parts' mesh (wpn_scope bones, left out of the grip
    // cloud). VR leaves the glass meshes out of the picture (they are
    // drawn for the flat-screen camera) and draws its own lens and dot.
    m_glass.clear(); m_lens_meshes.clear(); m_glass_dot=m_glass_soft=false;
    m_optic_cloud.clear();
    CloudCollector collector(m_optic_cloud);
    for (u16 id=0;id<model.LL_BoneCount();++id)
    {
        bool scope=false;
        for (u16 bone=id;!scope && bone!=BI_NONE && bone<model.LL_BoneCount();
             bone=model.LL_GetData(bone).GetParentID())
        {
            scope=NameHas(model.LL_BoneName_dbg(bone),"scope");
            if (model.LL_GetData(bone).GetParentID()==bone) break;
        }
        if (scope) model.EnumBoneVertices(collector,id);
    }
    m_mesh_shaders="";
    IRenderVisual* visual=model.dcast_RenderVisual();
    xr_vector<IRenderVisual*>* children=visual ? visual->get_children() : nullptr;
    if (!children) return;
    string512 shaders="";
    for (IRenderVisual* child:*children)
    {
        LPCSTR shader=child ? child->getDebugShader() : nullptr;
        if (!shader || !*shader) continue;
        if (!strstr(shaders,shader) && xr_strlen(shaders)+xr_strlen(shader)+3<sizeof(shaders))
        {
            if (*shaders) xr_strcat(shaders,", ");
            xr_strcat(shaders,shader);
        }
        bool dot=false, soft=false;
        LPCSTR texture=child->getDebugTexture();
        if (!SightGlass(shader,texture,dot,soft))
        {
            // An ordinary mesh with a lens texture: the glass of a scope
            // built into the model (wpn_fn2000_lens). Only used for a
            // weapon the config or glass already makes a scope.
            if (texture && strstr(texture,"lens")) m_lens_meshes.push_back(child->getVisData().box);
            continue;
        }
        m_glass.push_back(child->getVisData().box);
        m_glass_dot=m_glass_dot || dot;
        m_glass_soft=m_glass_soft || soft;
    }
    m_mesh_shaders=shaders;
}

void CVrWeaponInteraction::MeasureSight() const
{
    // What sight the weapon has and where. Anomaly builds most scoped and
    // red-dot weapons as their own sections (wpn_ak74m_ps01, wpn_ak74m_pka,
    // wpn_p90_eot), usually with scope_status 0; integrated sights (AUG)
    // only set a zoom. The place comes from the model: its glass meshes,
    // else the rear of its scope part. Sights look along the bore (+z).
    // The flat-screen aiming line (eye/dir from SampleAuthoredHands) is
    // only a fallback: it is the HUD camera, not where an eye would be.
    m_sight.type=SightIron;
    const shared_str section=m_weapon.cNameSect();
    shared_str addon;
    if (m_weapon.get_ScopeStatus()==ALife::eAddonAttachable && m_weapon.IsScopeAttached())
        addon=m_weapon.GetScopeName();
    else if (pSettings->line_exist(section,"scopes"))
    {
        LPCSTR list=pSettings->r_string(section,"scopes");
        const size_t length=xr_strlen(section.c_str());
        for (int index=0;index<_GetItemCount(list);++index)
        {
            string128 item; _GetItem(list,index,item);
            const size_t size=xr_strlen(item);
            if (size && length>size+1 && section.c_str()[length-size-1]=='_' &&
                !xr_strcmp(section.c_str()+length-size,item)) { addon=item; break; }
        }
    }
    // Weapons that come with a red dot, by name (wpn_ak74m_pka, wpn_p90_eot).
    static LPCSTR const reflex_names[]={"_pka","_kobra","_eot","_ekp","_aimpoint","_compm","_okp",
        "_rmr","_holo","_reflex","_1p87","_srs","_deltapoint","_docter"};
    bool reflex_named=false;
    for (LPCSTR name:reflex_names) reflex_named=reflex_named || strstr(section.c_str(),name)!=nullptr;
    float zoom=READ_IF_EXISTS(pSettings,r_float,section,"scope_zoom_factor",0.f);
    if (zoom<=0.f && addon.size() && pSettings->section_exist(addon))
        zoom=READ_IF_EXISTS(pSettings,r_float,addon,"scope_zoom_factor",0.f);
    const bool lens=m_weapon.IsSecondVPZoomPresent();
    const bool fixed=m_weapon.get_ScopeStatus()==ALife::eAddonPermanent;
    // The glass: all of it, and the rearmost mesh (a scope's eyepiece).
    Fbox glass; glass.invalidate();
    const Fbox* rear_glass=nullptr;
    for (const Fbox& box:m_glass)
    {
        glass.merge(box);
        if (!rear_glass || box.min.z<rear_glass->min.z) rear_glass=&box;
    }
    const float span=rear_glass ? glass.max.z-glass.min.z : 0.f;
    Fvector cup; float cup_radius=0.f, part_front=0.f;
    const bool part=ScopeEyepiece(m_optic_cloud,cup,cup_radius,part_front);
    // The zoom factor is the zoomed field of view (x0.75 in the camera):
    // under 60 degrees, or a 3D lens, is a magnified scope. Glass along
    // 5 cm or more is a scope tube, and a soft black lens a scope's exit
    // pupil (the G36's has no zoom in its config); a red dot otherwise.
    const bool zoomed=lens || (zoom>0.f && zoom<60.f);
    const bool scope=rear_glass ? span>=0.05f || m_glass_soft || (zoomed && !m_glass_dot) : zoomed;
    // A scope built into the model: its lens-textured glass, the rearmost.
    const Fbox* lens_mesh=nullptr;
    for (const Fbox& box:m_lens_meshes)
        if (!lens_mesh || box.min.z<lens_mesh->min.z) lens_mesh=&box;
    if (scope)
    {
        m_sight.type=SightScope;
        m_sight.lens_fov=lens ? m_weapon.GetSecondVPFov() : (zoom>0.f ? zoom*0.75f : g_fov/4.f);
        m_sight.magnification=std::max(1.f,g_fov/std::max(m_sight.lens_fov,1.f));
    }
    else if (rear_glass || reflex_named || addon.size() || fixed || zoom>=60.f)
    {
        m_sight.type=SightCollimator;
        m_sight.magnification=1.f;
    }
    else return;
    m_sight.rear=scope ? 0.07f : 0.15f;
    LPCSTR source;
    float front=-flt_max;
    if (rear_glass)
    {
        // A scope's eyepiece is its rearmost glass; a red dot's glass is
        // all of its meshes together.
        const Fbox& box=scope ? *rear_glass : glass;
        box.getcenter(m_sight.window);
        if (scope) m_sight.window.z=box.min.z;
        m_sight.dir.set(0.f,0.f,1.f);
        const float half=0.5f*std::min(box.max.x-box.min.x,box.max.y-box.min.y);
        m_sight.radius=clampr(half,scope ? 0.008f : 0.012f,0.03f);
        front=glass.max.z;
        if (part) front=std::max(front,part_front);
        source="glass meshes";
    }
    else if (lens_mesh && scope)
    {
        lens_mesh->getcenter(m_sight.window);
        m_sight.window.z=lens_mesh->min.z;
        m_sight.dir.set(0.f,0.f,1.f);
        const float half=0.5f*std::min(lens_mesh->max.x-lens_mesh->min.x,lens_mesh->max.y-lens_mesh->min.y);
        m_sight.radius=clampr(half,0.008f,0.025f);
        front=lens_mesh->max.z;
        if (part) front=std::max(front,part_front);
        source="lens mesh";
    }
    else if (part && scope)
    {
        m_sight.window=cup;
        m_sight.dir.set(0.f,0.f,1.f);
        m_sight.radius=clampr(cup_radius*0.8f,0.008f,0.025f);
        front=part_front;
        source="scope part";
    }
    else if (m_sight.valid)
    {
        m_sight.window=m_sight.eye; m_sight.window.mad(m_sight.dir,m_sight.rear);
        m_sight.radius=scope ? 0.016f : 0.015f;
        source="flat-screen aiming line";
    }
    else { m_sight.type=SightIron; return; }
    m_sight.eye=m_sight.window; m_sight.eye.mad(m_sight.dir,-m_sight.rear);
    m_sight.valid=true;
    // How far the weapon reaches along the line (the scope's camera sits
    // past it), from the nominal eye.
    float reach=front>-flt_max ? front-m_sight.eye.z : 0.f;
    for (const Fvector& point:m_cloud)
    {
        Fvector to; to.sub(point,m_sight.eye);
        const float along=to.dotproduct(m_sight.dir);
        to.mad(m_sight.dir,-along);
        if (to.magnitude()<0.05f) reach=std::max(reach,along);
    }
    m_sight.front=std::max(reach,m_sight.rear+0.15f);
    Msg("* [VR weapon] %s: sight %s (%s), zoom %.0f, %.1fx, glass %.1f cm wide at %.3f %.3f %.3f, from the %s",
        section.c_str(),scope ? "scope" : "collimator",addon.size() ? addon.c_str() : "built in",
        zoom,m_sight.magnification,m_sight.radius*200.f,m_sight.window.x,m_sight.window.y,m_sight.window.z,source);
    if (!rear_glass && !part && !lens_mesh)
        Msg("* [VR weapon] %s: no sight glass or scope part found; mesh shaders: %s",section.c_str(),
            m_mesh_shaders.size() ? m_mesh_shaders.c_str() : "none");
}

bool CVrWeaponInteraction::EyeOnSight(const Fvector& eye, Fmatrix& world, Fvector& direction,
    Fvector& window, Fvector& offset, float& behind) const
{
    // Where an eye is relative to the sight's glass: how far behind it
    // along the sight, and its offset across the sight.
    if (!m_sight.valid || m_sight.type==SightIron || !Transform(world)) return false;
    world.transform_dir(direction,m_sight.dir);
    window=Point(world,m_sight.window);
    Fvector to; to.sub(window,eye);
    behind=to.dotproduct(direction);
    offset.mul(direction,behind).sub(to);
    return true;
}

void CVrWeaponInteraction::RenderSight()
{
    // Drawn into each eye (Device holds that eye's camera).
    if (!vr::GameplayActive()) return;
    Fmatrix world; Fvector direction, window, offset; float behind=0.f;
    const Fvector& eye=Device.vCameraPosition;
    if (!EyeOnSight(eye,world,direction,window,offset,behind) || behind>0.6f) return;
    if (Device.vCameraDirection.dotproduct(direction)<0.8f) return;
    Fvector right, up;
    right.crossproduct(world.j,direction).normalize_safe();
    up.crossproduct(direction,right);
    const float lateral=offset.magnitude();
    if (m_sight.type==SightCollimator)
    {
        // A collimator's dot is focused at infinity along the sight: from
        // anywhere behind the glass it is seen in that same direction, so
        // it stays on the target as the head moves, and only by an eye
        // whose view along the sight passes through the glass. Drawn 10 m
        // out over the scene (the glass is in front of everything else).
        // Anywhere behind the weapon's front: the glass position along the
        // sight does not change what a collimator shows. The window gets
        // 4 mm of slack for tracking and the headset's eye position.
        const float window_radius=m_sight.radius+0.004f;
        if (lateral>window_radius || behind<m_sight.rear-m_sight.front+0.05f) return;
        const float edge=clampr((window_radius-lateral)/0.004f,0.f,1.f);
        // Drawn where this eye's view along the sight crosses the glass,
        // at the same 4 mrad size, and depth tested there: the frame or a
        // hand in front of the glass hides it (the glass meshes themselves
        // are left out of the VR picture). An eye at or past the glass
        // sees it 10 m out over everything.
        if (behind>0.035f)
        {
            Fvector dot=eye; dot.mad(direction,behind-0.002f);
            const float half=0.004f*(behind-0.002f);
            vr::DrawReflexDot(dot,Fvector().mul(right,half),Fvector().mul(up,half),edge,true);
        }
        else
        {
            Fvector dot=eye; dot.mad(direction,10.f);
            vr::DrawReflexDot(dot,Fvector().mul(right,0.04f),Fvector().mul(up,0.04f),edge,false);
        }
        return;
    }
    // A scope passes its image through an exit pupil behind the eyepiece:
    // on axis the image fills the lens, off axis a shadow closes in from
    // that side (gone 2.5 cm off), and right at the glass it goes dark.
    // Further back the lens simply looks smaller and, at the same
    // magnification, shows less of the scene (RequestScopeView), which is
    // the tunnel a real scope gives; a headset cannot get an eye as close
    // as a cheek weld does, so there is no extra eye-relief cut.
    if (behind<0.015f) return;
    const float brightness=clampr((behind-0.015f)/0.015f,0.f,1.f);
    // The image is drawn vr_scope_size times the glass (a headset keeps
    // the eye further back than a cheek weld would).
    const float radius=m_sight.radius*vr::ScopeLensScale();
    vr::DrawScopeLens(window,Fvector().mul(right,radius),Fvector().mul(up,radius),
        brightness,offset.dotproduct(right)/0.025f,-offset.dotproduct(up)/0.025f);
}

void CVrWeaponInteraction::RequestScopeView() const
{
    // Ask the stereo renderer for the magnified view while the head is
    // behind the eyepiece and looking along the sight: from just past the
    // weapon, as wide as the lens appears from the eye divided by the
    // magnification (vr_scope_zoom scales the game's flat-screen zoom).
    if (m_sight.type!=SightScope) return;
    Fmatrix head_world;
    if (!vr::HeadTransform(head_world)) return;
    const Fvector head=head_world.c, looking=head_world.k;
    Fmatrix world; Fvector direction, window, offset; float behind=0.f;
    if (!EyeOnSight(head,world,direction,window,offset,behind)) return;
    if (behind<=0.f || behind>0.4f || offset.magnitude()>0.07f || looking.dotproduct(direction)<0.8f)
    {
        // Why not, when the head is near the scope (every 3 s at most).
        if (head.distance_to(window)<0.5f && Device.dwTimeGlobal-m_scope_refused_time>3000)
        {
            m_scope_refused_time=Device.dwTimeGlobal;
            Msg("* [VR weapon] %s: no scope view: head %.0f cm behind the eyepiece (0-40), %.1f cm off the line (<7), looking %.2f along it (>0.80)",
                m_weapon.cNameSect().c_str(),behind*100.f,offset.magnitude()*100.f,looking.dotproduct(direction));
        }
        return;
    }
    const float radius=m_sight.radius*vr::ScopeLensScale();
    const float apparent=rad2deg(2.f*atanf(radius/std::max(behind,0.03f)));
    const float magnification=std::max(m_sight.magnification*vr::ScopeZoomScale(),1.f);
    Fmatrix camera; camera.identity();
    camera.k=direction;
    camera.i.crossproduct(world.j,direction).normalize_safe();
    camera.j.crossproduct(direction,camera.i);
    camera.c=window; camera.c.mad(direction,m_sight.front-m_sight.rear+0.03f);
    Frect part;
    const shared_str reticle=m_weapon.VrScopeReticle(&part);
    const float rect[4]={part.x1,part.y1,part.x2,part.y2};
    vr::SetScopeView(camera,apparent/magnification,apparent,reticle.size() ? reticle.c_str() : nullptr,
        part.width()>0.f ? rect : nullptr);
}

bool CVrWeaponInteraction::MagazineInHand(Fmatrix& root) const
{
    // The magazine the support hand has: guided on the well, or held.
    Fmatrix world;
    if (!m_profile.detachable || !Transform(world)) return false;
    if (m_mechanism.held>=0)
    {
        root=SolvedMagazine(world);
        return true;
    }
    return false;
}

bool CVrWeaponInteraction::AuthoredBones(int side, VrHandBones& bones) const
{
    // Right-handed holds only: a pose cannot be mirrored bone for bone.
    Fmatrix world;
    if (!vr::AnimatedGrip() || !Mode() || m_hand!=1 || m_carried || !Transform(world) || m_cloud.empty())
        return false;
    SampleAuthoredHands();
    if (m_hands_local.empty()) return false;
    // An animated hand far from the grip is drawn by the arm IK instead.
    GripLocal();
    if (!m_palm_trusted) return false;
    bones.visual=m_hands_visual_name;
    if (side)
    {
        if (!m_authored[1].valid) return false;
        bones.hand.mul_43(world,m_hand_in_item[1]);
        bones.locals=&m_hands_local;
        return true;
    }
    // The left hand holds a magazine as the reload does, or the weapon as
    // the idle does.
    Fmatrix root;
    if (!m_maghand_local.empty() && MagazineInHand(root))
    {
        bones.hand.mul_43(root,m_maghand_in_root);
        bones.locals=&m_maghand_local;
        return true;
    }
    if (!m_supporting || !m_authored[0].valid) return false;
    // The authored hand is on the fore-end: with the barrels open, only a
    // hand that holds them is drawn there, riding the hinge.
    if (BarrelsOpen() && !m_support_barrels) return false;
    if (BarrelsOpen())
    {
        Fmatrix moved, barrels;
        HingeWorld(world,moved);
        barrels.mul_43(moved,world);
        bones.hand.mul_43(barrels,m_hand_in_item[0]);
    }
    else bones.hand.mul_43(world,m_hand_in_item[0]);
    if (m_pumping) bones.hand.c.mad(world.k,-m_pump);
    bones.locals=&m_hands_local;
    return true;
}

bool CVrWeaponInteraction::AuthoredHand(int side, VrHandPose& pose) const
{
    // Only measured weapons are drawn with the first-person model the
    // animation was made for; NPC world models use a different mesh.
    Fmatrix world;
    const bool weapon_hand=side==m_hand;
    if (!vr::AnimatedGrip() || !Mode() || (!weapon_hand && !m_supporting) || !Transform(world) ||
        m_cloud.empty()) return false;
    SampleAuthoredHands();
    if (!m_palm_trusted) return false;
    if (!weapon_hand && m_pumping) world.c.mad(world.k,-m_pump);
    // The fore-end hand rides the hinged barrels; one on the receiver is
    // not drawn on them.
    if (!weapon_hand && BarrelsOpen())
    {
        if (!m_support_barrels) return false;
        Fmatrix moved, barrels;
        HingeWorld(world,moved);
        barrels.mul_43(moved,world);
        world=barrels;
    }
    // A weapon passed to the support hand stays held the way that hand held it.
    const int source=weapon_hand && !m_carried ? 1 : 0;
    const AuthoredHandRef& ref=m_authored[source];
    if (!ref.valid) return false;
    const bool mirror=side!=source;
    const auto direction=[&](Fvector v) { if (mirror) v.x=-v.x; world.transform_dir(v); return v; };
    Fvector knuckles=ref.knuckles;
    if (mirror) knuckles.x=2.f*(m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f)-knuckles.x;
    pose.knuckles=Point(world,knuckles);
    pose.basis[0]=direction(ref.basis[0]);
    pose.basis[1]=direction(ref.basis[1]);
    pose.basis[2].crossproduct(pose.basis[0],pose.basis[1]);
    for (int f=0;f<3;++f)
        for (int j=0;j<3;++j) pose.finger[f][j]=direction(ref.finger[f][j]);
    return true;
}

bool CVrWeaponInteraction::RearSupport() const
{
    if (!vr::AnimatedGrip() || m_cloud.empty()) return false;
    SampleAuthoredHands();
    if (!m_authored[0].valid || !m_authored[1].valid) return false;
    return m_authored[0].palm.z<m_authored[1].palm.z-0.08f;
}
float CVrWeaponInteraction::SupportAlong(const Fvector& grip, const Fvector& muzzle,
    const Fvector& support)
{
    Fvector axis; axis.sub(muzzle,grip);
    if (axis.square_magnitude()<0.15f*0.15f) return 0.f;
    axis.normalize();
    return Fvector().sub(support,grip).dotproduct(axis);
}
int CVrWeaponInteraction::SupportNear(const Fvector& grip, const Fvector& muzzle,
    const Fvector& forward, const Fvector& support) const
{
    // Any weapon can be held in two hands, with no per-weapon data: the
    // support hand closing at the grip cups the weapon hand (2); anywhere on
    // the barrel line ahead of it, it takes the fore-end (1).
    if (support.distance_to(grip)<0.12f) return 2;
    Fvector axis; axis.sub(muzzle,grip);
    float length=axis.magnitude();
    if (length<0.15f) { axis=forward; length=0.6f; }
    else axis.div(length);
    Fvector to; to.sub(support,grip);
    const float along=to.dotproduct(axis);
    // A weapon held from behind (an RPG-7's rear grip) takes the support
    // hand up to 50 cm behind the weapon hand instead of ahead of it.
    if (RearSupport())
    {
        if (along>-0.08f || along<-0.5f) return 0;
        to.mad(axis,-along);
        return to.magnitude()<0.12f ? 1 : 0;
    }
    if (along<0.1f || along>length+0.05f) return 0;
    to.mad(axis,-along);
    return to.magnitude()<0.12f ? 1 : 0;
}
void CVrWeaponInteraction::HingePose(Fmatrix& hinge) const
{
    hinge.identity();
    if (!m_profile.break_action || m_break_angle<=0.f) return;
    // Barrels drop about the hinge's lateral (+X) axis: +angle turns +Z to -Y.
    Fvector axis; axis.set(1.f,0.f,0.f);
    hinge.rotation(axis,m_break_angle);
    Fvector moved; hinge.transform_dir(moved,m_profile.hinge_point);
    hinge.c.sub(m_profile.hinge_point,moved);
}
void CVrWeaponInteraction::HingeWorld(const Fmatrix& world, Fmatrix& moved) const
{
    Fmatrix hinge, inverse, local;
    HingePose(hinge);
    inverse.invert(world);
    local.mul_43(hinge,inverse);
    moved.mul_43(world,local);
}
float CVrWeaponInteraction::HingeAngle(const Fmatrix& world, const Fvector& point) const
{
    Fmatrix inverse; inverse.invert(world);
    Fvector local; inverse.transform_tiny(local,point);
    local.sub(m_profile.hinge_point);
    return atan2f(-local.y,local.z);
}
void CVrWeaponInteraction::ResetParts()
{
    // At rest: parts closed, the belt on the tray when the weapon holds
    // rounds. Nothing of this is saved; a loaded save starts here.
    for (int i=0;i<m_profile.part_count;++i) vr::ResetPart(m_profile.parts[i],m_parts[i]);
    m_belt=vr::BeltState{};
    m_belt.seated=!m_weapon.m_magazine.empty();
    m_parts_inserted=m_mechanism.inserted;
    m_parts_ready=true;
}
vr::ReloadFacts CVrWeaponInteraction::Facts() const
{
    vr::ReloadFacts facts;
    for (int i=0;i<m_profile.part_count;++i)
    { facts.open[i]=m_parts[i].open; facts.closed[i]=m_parts[i].closed; }
    facts.belt_seated=!m_profile.belt.used || m_belt.seated;
    facts.magazine_in=!m_profile.detachable || m_mechanism.inserted>=0;
    return facts;
}
bool CVrWeaponInteraction::RuleAllows(const vr::GateList& gates, LPCSTR what)
{
    int blocked=-1;
    if (vr::GatesPass(gates,Facts(),&blocked)) return true;
    if (Device.dwTimeGlobal-m_rule_logged>1000 && blocked>=0)
    {
        m_rule_logged=Device.dwTimeGlobal;
        const vr::Gate& gate=gates.gates[blocked];
        LPCSTR need=gate.kind==vr::GateBeltSeated ? "the belt on the feed tray" :
            gate.kind==vr::GateMagazineIn ? "the magazine in" : nullptr;
        string128 part;
        if (!need)
        {
            xr_sprintf(part,"the %s %s",m_profile.part_names[gate.part].c_str(),
                gate.kind==vr::GatePartOpen ? "open" : "closed");
            need=part;
        }
        Msg("* [VR weapon] %s: %s needs %s",m_profile.section.c_str(),what,need);
        vr::HandHaptic(HandOf(0),0.05f);
    }
    return false;
}
bool CVrWeaponInteraction::BeltHidden() const
{
    // The loose end hangs out of sight in the box until it is laid on the
    // tray or carried there.
    return m_profile.belt.used && m_profile.belt_free_bone.size() && !m_belt.seated && m_belt.hand<0;
}
bool CVrWeaponInteraction::UpdateParts(const Fmatrix& world, bool left_valid, const Fmatrix& left,
    bool grip, bool grip_edge)
{
    if (!m_profile.part_count && !m_profile.belt.used) return false;
    const int hand=HandOf(0);
    Fmatrix inverse; inverse.invert(world);
    Fvector local; inverse.transform_tiny(local,left.c);
    vr::PartInput in;
    in.tracked=left_valid; in.grip=grip;
    // A hand holding a magazine, racking or steadying the weapon takes nothing.
    in.grip_edge=grip_edge && m_mechanism.held<0 && !m_racking && !m_supporting && !m_shell_held;
    in.hand={local.x,local.y,local.z};
    in.dt=clampr(Device.fTimeDelta,0.f,0.1f);
    bool holding=false;
    for (int i=0;i<m_profile.part_count;++i)
    {
        const vr::PartDef& def=m_profile.parts[i];
        vr::PartState& state=m_parts[i];
        // The hand first, then the part settles (in that order).
        unsigned events=vr::StepPart(def,state,hand,in);
        events|=vr::SettlePart(def,state,in.dt);
        const vr::PartVec a=vr::PartAnchor(def,state);
        const Fvector at=Point(world,Fvector().set(a.x,a.y,a.z));
        if (events&vr::PartGrabbed) vr::HandHaptic(hand,0.12f);
        if (events&vr::PartSlipped) vr::HandHaptic(hand,0.2f);
        if (events&vr::PartOpened) { PlayHandling(SoundCoverOpen,at); vr::HandHaptic(hand,0.2f); }
        if (events&vr::PartClosed) { PlayHandling(SoundCoverClose,at); vr::HandHaptic(hand,0.3f); }
        if (events&(vr::PartOpened|vr::PartClosed))
            Msg("* [VR weapon] %s: %s %s",m_profile.section.c_str(),m_profile.part_names[i].c_str(),
                events&vr::PartOpened ? "open" : "closed");
        if (state.hand==hand) { holding=true; in.grip_edge=false; }
    }
    if (m_profile.belt.used)
    {
        const bool may=vr::GatesPass(m_profile.rules.belt,Facts());
        const unsigned events=vr::StepBelt(m_profile.belt,m_belt,hand,in,may,may);
        const Fvector tray=Point(world,Fvector().set(m_profile.belt.tray.x,m_profile.belt.tray.y,m_profile.belt.tray.z));
        if (events&vr::BeltTaken) vr::HandHaptic(hand,0.12f);
        if (events&vr::BeltDropped) vr::HandHaptic(hand,0.06f);
        if (events&vr::BeltSeated)
        {
            PlayHandling(SoundBeltIn,tray);
            vr::HandHaptic(hand,0.25f);
            Msg("* [VR weapon] %s: belt on the feed tray",m_profile.section.c_str());
        }
        if (m_belt.hand==hand) holding=true;
    }
    return holding;
}
bool CVrWeaponInteraction::PoseParts(IKinematics& model, xr_vector<Fmatrix>& poses) const
{
    const u16 count=model.LL_BoneCount();
    bool moved=false;
    const auto apply=[&](const vr::PartDef& def, float value, u16 bone)
    {
        float m[3][3];
        vr::PartRotation(def,value,m);
        Fmatrix motion; motion.identity();
        motion.i.set(m[0][0],m[0][1],m[0][2]);
        motion.j.set(m[1][0],m[1][1],m[1][2]);
        motion.k.set(m[2][0],m[2][1],m[2][2]);
        const vr::PartVec at=vr::PartMove(def,value,def.pivot);
        Fvector pivot; pivot.set(def.pivot.x,def.pivot.y,def.pivot.z);
        Fvector turned; motion.transform_dir(turned,pivot);
        motion.c.set(at.x,at.y,at.z).sub(turned);
        for (u16 id=0;id<count && id<poses.size();++id)
            if (Descends(model,id,bone))
            {
                Fmatrix pose; pose.mul_43(motion,poses[id]);
                poses[id]=pose;
            }
        moved=true;
    };
    for (int i=0;i<m_profile.part_count;++i)
    {
        if (_abs(m_parts[i].value)<1e-5f) continue;
        const u16 bone=model.LL_BoneID(m_profile.part_bones[i].c_str());
        if (bone<count) apply(m_profile.parts[i],m_parts[i].value,bone);
    }
    // A carried belt end swings about its first link toward the hand.
    const u16 free=m_profile.belt_free_bone.size() ? model.LL_BoneID(m_profile.belt_free_bone.c_str()) : BI_NONE;
    if (m_belt.hand>=0 && free<count && free<poses.size())
    {
        const Fvector joint=poses[free].c;
        Fvector from, to;
        from.set(m_profile.belt.tray.x,m_profile.belt.tray.y,m_profile.belt.tray.z).sub(joint);
        to.set(m_belt.carried.x,m_belt.carried.y,m_belt.carried.z).sub(joint);
        if (from.square_magnitude()>EPS_S && to.square_magnitude()>EPS_S)
        {
            from.normalize(); to.normalize();
            Fvector axis; axis.crossproduct(from,to);
            const float sine=axis.magnitude();
            if (sine>1e-4f)
            {
                vr::PartDef swing;
                swing.kind=vr::PartHinge;
                swing.pivot={joint.x,joint.y,joint.z};
                // PartRotation turns right-handed about its axis: the cross
                // product here follows the engine's left-handed rule.
                axis.div(sine);
                const float angle=atan2f(sine,from.dotproduct(to));
                swing.axis={axis.x,axis.y,axis.z};
                const vr::PartVec test=vr::PartMove(swing,angle,{joint.x+from.x,joint.y+from.y,joint.z+from.z});
                Fvector reached; reached.set(test.x,test.y,test.z).sub(joint);
                apply(swing,reached.dotproduct(to)>0.99f ? angle : -angle,free);
            }
        }
    }
    return moved;
}
void CVrWeaponInteraction::MeasurePartMotion(IKinematics& model)
{
    // A hinge without an axis in the profile takes it from the weapon's own
    // reload: play anm_reload (or anm_reload_empty) on a private copy and
    // find the frame that moves the part's grab point furthest. Points of
    // the part off its pivot give the axis (each moves square to it; the
    // bone's own axes, so they are never on one line), the bone's origin the
    // pivot, and the grab point's turn the opening angle.
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    for (int i=0;i<m_profile.part_count;++i)
    {
        if (!m_profile.part_from_animation[i]) continue;
        m_profile.part_from_animation[i]=false;
        vr::PartDef& def=m_profile.parts[i];
        const u16 bone=model.LL_BoneID(m_profile.part_bones[i].c_str());
        if (bone>=count || bone>=bind.size())
        {
            // Rules naming a missing part could never pass and would stop
            // the weapon firing: without its parts, it has no rules either.
            Msg("! [VR weapon] %s: part %s: bone %s missing; moving parts and reload rules disabled",
                m_profile.section.c_str(),m_profile.part_names[i].c_str(),m_profile.part_bones[i].c_str());
            m_profile.part_count=0;
            m_profile.rules=vr::ReloadRules{};
            m_profile.belt.used=false;
            // Nor are their bones VR's to pose (PrepareParts).
            for (auto& name:m_profile.part_bones) name=nullptr;
            m_profile.belt_free_bone=nullptr;
            break;
        }
        const Fvector grab{def.grab.x,def.grab.y,def.grab.z};
        def.pivot={bind[bone].c.x,bind[bone].c.y,bind[bone].c.z};
        // The grab point and three points along the bone's own axes, 10 cm
        // from its origin, in model space at rest.
        Fvector rest_points[4], local_points[4];
        rest_points[0]=grab;
        for (int a=0;a<3;++a)
        {
            Fvector offset{}; offset[a]=0.1f;
            bind[bone].transform_tiny(rest_points[a+1],offset);
        }
        {
            Fmatrix to_bind; to_bind.invert(bind[bone]);
            for (int n=0;n<4;++n) to_bind.transform_tiny(local_points[n],rest_points[n]);
        }
        IRenderVisual* visual=HudCopy(m_profile.visual.c_str());
        auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
        auto* sample=visual ? visual->dcast_PKinematics() : nullptr;
        float best=0.f;
        Fvector best_points[4]{};
        LPCSTR source="";
        if (animated && sample && sample->LL_BoneCount()==count)
            for (LPCSTR key:{"anm_reload","anm_reload_empty"})
            {
                if (!PlayHudMotion(*animated,hud,key)) continue;
                for (int step=0;step<360;++step)
                {
                    animated->LL_UpdateTracks(step ? 1.f/60.f : 0.f,true,true);
                    sample->CalculateBones_Invalidate();
                    sample->CalculateBones(TRUE);
                    // Relative to the receiver: the whole gun moves in the animation.
                    Fmatrix placed=sample->LL_GetTransform(bone);
                    const u16 parent=model.LL_GetData(bone).GetParentID();
                    if (parent<count)
                    {
                        Fmatrix rest; rest.mul_43(bind[parent],Fmatrix().invert(sample->LL_GetTransform(parent)));
                        placed.mulA_43(rest);
                    }
                    Fvector moved[4];
                    for (int n=0;n<4;++n) placed.transform_tiny(moved[n],local_points[n]);
                    const float d=moved[0].distance_to(grab);
                    if (d>best) { best=d; for (int n=0;n<4;++n) best_points[n]=moved[n]; source=key; }
                }
                if (best>0.02f) break;
            }
        if (visual) ::Render->model_Delete(visual);
        vr::PartVec before[4], after[4];
        for (int n=0;n<4;++n)
        {
            before[n]={rest_points[n].x,rest_points[n].y,rest_points[n].z};
            after[n]={best_points[n].x,best_points[n].y,best_points[n].z};
        }
        const vr::PartVec axis=vr::AxisFromMotion(before,after,4);
        if (best>0.02f && vr::PartLength(axis)>0.5f)
        {
            def.axis=axis;
            // The sign that opens it the way the animation does.
            float angle=vr::PartWrap(vr::PartCoordinate(def,after[0])-vr::PartCoordinate(def,def.grab));
            if (angle<0.f) { def.axis=vr::PartScale(axis,-1.f); angle=-angle; }
            def.max=clampr(angle,deg2rad(20.f),deg2rad(120.f));
            def.open_at=0.8f*def.max;
            Msg("* [VR weapon] %s: part %s: hinge axis (%.2f %.2f %.2f) pivot (%.3f %.3f %.3f) open %.0f deg, from %s",
                m_profile.section.c_str(),m_profile.part_names[i].c_str(),def.axis.x,def.axis.y,def.axis.z,
                def.pivot.x,def.pivot.y,def.pivot.z,rad2deg(def.max),source);
        }
        else
            Msg("* [VR weapon] %s: part %s: no motion in the reload animation; hinge about +X at its bone, %.0f deg",
                m_profile.section.c_str(),m_profile.part_names[i].c_str(),rad2deg(def.max));
    }
}
Fvector CVrWeaponInteraction::ChamberWorld(const Fmatrix& world, int chamber) const
{
    Fmatrix hinge; HingePose(hinge);
    Fvector local; hinge.transform_tiny(local,m_profile.shell_center[chamber]);
    return Point(world,local);
}
bool CVrWeaponInteraction::ShellHandPose(Fmatrix& pose) const
{
    // Hold the shell in the left fingertips, oriented like the pistol grip,
    // with its centre slightly above the controller grip point.
    Fmatrix grip;
    if (!vr::HandTransform(HandOf(0),false,grip) || !vr::HandTransform(HandOf(0),true,pose)) return false;
    pose.c.set(0,0,0);
    Fvector centre=m_profile.shell_center[1];
    pose.transform_dir(centre);
    pose.c.mad(grip.c,pose.j,0.02f);
    pose.c.sub(centre);
    return true;
}
void CVrWeaponInteraction::SyncChambers()
{
    const unsigned rounds=std::min(unsigned(m_weapon.m_magazine.size()),2u);
    if (!m_break_initialized)
    {
        m_break_initialized=true;
        for (unsigned c=0;c<2;++c) m_chambers[c]=c<rounds ? 1 : 0;
        return;
    }
    // A native or scripted reload filled barrels; a shot spent the last
    // live barrel. Only the count is authoritative, so saves stay ordinary.
    unsigned live=unsigned(m_chambers[0]==1)+unsigned(m_chambers[1]==1);
    for (int c=0;c<2 && live<rounds;++c) if (m_chambers[c]!=1) { m_chambers[c]=1; ++live; }
    for (int c=1;c>=0 && live>rounds;--c) if (m_chambers[c]==1) { m_chambers[c]=2; --live; }
}
void CVrWeaponInteraction::EjectShell(int chamber, const Fmatrix& world)
{
    if (!m_profile.shell_bones[chamber].size()) return;
    Fmatrix hinge; HingePose(hinge);
    Fmatrix pose; pose.mul_43(world,hinge);
    CPhysicsShell* shell=P_create_Shell();
    CPhysicsElement* element=P_create_Element();
    Fobb box; box.m_rotate.identity();
    box.m_translate=m_profile.shell_center[chamber];
    box.m_halfsize=m_profile.shell_box_half;
    element->add_Box(box); shell->add_Element(element);
    shell->setMass(0.03f);
    shell->set_PhysicsRefObject(nullptr);
    // The ejector throws the case up and back out of the open breech.
    Fvector velocity=m_hand_velocity[1];
    velocity.mad(world.j,0.9f).mad(world.k,-1.6f);
    shell->Activate(pose,velocity,m_hand_angular[1]);
    shell->SetAirResistance(0.002f,0.01f);
    shell->set_DynamicLimits(5.f,20.f);
    shell->SetSmall(); shell->EnableCollision();
    if (m_props.size()>=8) { RetireProp(m_props.front()); m_props.pop_front(); }
    DropProp prop;
    prop.shell=shell;
    prop.part=m_profile.shell_bones[chamber];
    prop.expires=Device.dwTimeGlobal+20000;
    m_props.push_back(prop);
}

bool VrNearSide(Fvector& point, float margin)
{
    Fvector head;
    if (!vr::HeadPosition(head)) return false;
    Fvector direction; direction.sub(point,head);
    const float distance=direction.magnitude();
    collide::rq_result wall;
    if (distance<EPS_S || !Level().ObjectSpace.RayPick(head,direction.div(distance),distance,
        collide::rqtStatic,wall,nullptr)) return false;
    point.mad(head,direction,_max(0.f,wall.range-margin));
    return true;
}

bool CVrWeaponInteraction::FireDependencies(firedeps& deps) const
{
    Fmatrix world;
    Fvector direction;
    if (Transform(world))
    {
        deps.vLastFP=Point(world,m_profile.muzzle);
        deps.vLastFP2=deps.vLastFPSilencer=deps.vLastFP;
        deps.vLastSP=Point(world,m_profile.shell_point);
        // Along the bore: zeroing waits for a hands-on sight adjustment.
        direction=world.k;
    }
    else if (GenericTransform(world))
    {
        // Unprofiled weapons fire from their tracked world model, using the
        // same third-person points as an NPC rather than the hidden HUD.
        deps.vLastFP=Point(world,m_weapon.vLoadedFirePoint);
        deps.vLastFP2=Point(world,m_weapon.vLoadedFirePoint2);
        deps.vLastFPSilencer=Point(world,m_weapon.vLoadedFirePointSilencer);
        deps.vLastSP=Point(world,m_weapon.vLoadedShellPoint);
        direction=world.k;
    }
    else return false;
    // With a physical weapon, a barrel pushed into a wall fires into that
    // wall, not from its far side.
    if (vr::PhysicalWeapon())
    {
        VrNearSide(deps.vLastFP,0.02f);
        VrNearSide(deps.vLastFP2,0.02f);
        VrNearSide(deps.vLastFPSilencer,0.02f);
    }
    deps.vLastFD=direction;
    deps.m_FireParticlesXForm=world;
    deps.m_FireParticlesXForm.c=deps.vLastFP;
    return true;
}

bool CVrWeaponInteraction::ValidateModel(IKinematics& model)
{
    if (!m_profile.detachable && !m_profile.cycle_travel && !m_profile.break_action) return true;
    if (m_profile.detachable && model.LL_BoneID(m_profile.mag_bone.c_str())==BI_NONE)
    {
        Msg("! [VR weapon] %s: missing magazine bone %s; physical magazine disabled",
            m_profile.section.c_str(),m_profile.mag_bone.c_str());
        m_profile.detachable=false;
    }
    if (m_profile.cycle_travel && m_profile.cycle_bone.size() &&
        model.LL_BoneID(m_profile.cycle_bone.c_str())==BI_NONE)
    {
        Msg("! [VR weapon] %s: missing cycling bone %s; cycling disabled",
            m_profile.section.c_str(),m_profile.cycle_bone.c_str());
        m_profile.cycle_travel=0;
        m_profile.detachable=false;
        m_profile.chamber=false;
    }
    for (const auto& name:m_profile.mag_extra_bones)
        if (name.size() && model.LL_BoneID(name.c_str())==BI_NONE)
        {
            Msg("! [VR weapon] %s: missing magazine part %s; physical magazine disabled",
                m_profile.section.c_str(),name.c_str());
            m_profile.detachable=false;
        }
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (m_profile.break_action)
    {
        const u16 hinge=model.LL_BoneID(m_profile.hinge_bone.c_str());
        bool shells=true;
        for (int c=0;c<2;++c)
        {
            Fbox box;
            // Barrels found by shape have no shell parts of their own.
            if (!m_profile.shell_bones[c].size()) continue;
            const u16 id=model.LL_BoneID(m_profile.shell_bones[c].c_str());
            if (id==BI_NONE || !MeasureBone(model,id,box,true)) { shells=false; continue; }
            box.getcenter(m_profile.shell_center[c]);
        }
        if (hinge!=BI_NONE && hinge<bind.size()) m_profile.hinge_point=bind[hinge].c;
        if (hinge==BI_NONE || !shells || m_profile.hinge_angle<=0.f)
        {
            Msg("! [VR weapon] %s: break-action bones missing; interactive loading disabled",
                m_profile.section.c_str());
            m_profile.break_action=false;
        }
    }
    if (m_profile.check_mag_bind && m_profile.detachable)
    {
        const u16 id=model.LL_BoneID(m_profile.mag_bone.c_str());
        if (id==BI_NONE || id>=bind.size() ||
            !Near(bind[id].c,m_profile.expected_mag_bind,m_profile.bind_tolerance))
        {
            Msg("! [VR weapon] %s: magazine bind differs from audited asset; native HUD retained",
                m_profile.section.c_str());
            return false;
        }
    }
    if (m_profile.check_cycle_bind && m_profile.cycle_travel)
    {
        const u16 id=model.LL_BoneID(m_profile.cycle_bone.c_str());
        if (id==BI_NONE || id>=bind.size() ||
            !Near(bind[id].c,m_profile.expected_cycle_bind,m_profile.bind_tolerance))
        {
            Msg("! [VR weapon] %s: cycling bind differs from audited asset; native HUD retained",
                m_profile.section.c_str());
            return false;
        }
    }
    return true;
}

void CVrWeaponInteraction::Pose(IKinematics& model, xr_vector<Fmatrix>& poses,
    bool magazine_only, bool rounds)
{
    const bool modular=m_weapon.VrModularAttachments();
    const bool scope=!modular && m_weapon.get_ScopeStatus()!=ALife::eAddonDisabled &&
        (!m_weapon.ScopeAttachable() || m_weapon.IsScopeAttached());
    const bool silencer=m_weapon.get_SilencerStatus()!=ALife::eAddonDisabled &&
        (!m_weapon.SilencerAttachable() || m_weapon.IsSilencerAttached());
    const bool launcher=m_weapon.get_GrenadeLauncherStatus()!=ALife::eAddonDisabled &&
        (!m_weapon.GrenadeLauncherAttachable() || m_weapon.IsGrenadeLauncherAttached());
    model.Callback(CollapseHiddenBones,nullptr);
    const bool pump=!m_profile.cycle_bone.size() && m_profile.pump_bone.size();
    vr::ApplyWeaponPose(model,poses,BonePose,bctCustom,
        pump ? m_profile.pump_bone.c_str() : m_profile.cycle_bone.c_str(),
        pump ? m_pump : (m_profile.cycle_from_reload ? m_handle_slide :
            std::max(m_mechanism.slide,m_cycle_visual_slide)),m_profile.rounds_bone.c_str(),
        {m_profile.mag_bone.c_str(),m_profile.rounds_bone.c_str()},magazine_only,
        m_mechanism.inserted>=0 || !m_profile.detachable,rounds,nullptr,
        {scope ? nullptr : "wpn_scope",silencer ? nullptr : "wpn_silencer",
            launcher ? nullptr : "wpn_launcher",
            modular && m_weapon.IsScopeAttached() ? "wpn_scoped_hide" : nullptr,
            modular && !m_weapon.IsScopeAttached() ? "wpn_scoped_unhide" : nullptr,
            m_profile.break_action && !m_chambers[0] ? m_profile.shell_bones[0].c_str() : nullptr,
            m_profile.break_action && !m_chambers[1] && m_profile.shell_bones[1]!=m_profile.shell_bones[0] ?
                m_profile.shell_bones[1].c_str() : nullptr,
            m_profile.hidden_parts[0].c_str(),m_profile.hidden_parts[1].c_str(),
            m_profile.hidden_parts[2].c_str(),BeltHidden() ? m_profile.belt_free_bone.c_str() : nullptr},
        {m_profile.mag_extra_bones[0].c_str(),m_profile.mag_extra_bones[1].c_str()},
        pump ? nullptr : m_profile.handle_bone.c_str(),m_handle_slide);
    // What the native first-person model hides now (a fired rocket or
    // launcher grenade, a round or shell its reload took away) is hidden on
    // this copy too: it kept showing them, as it never sees native changes.
    const bool hidden=!magazine_only && CopyNativeVisibility(model);
    const bool animated=!magazine_only && AnimateParts(model,poses);
    const u16 hinge_bone=magazine_only || !m_profile.break_action || m_break_angle<=0.f ?
        BI_NONE : model.LL_BoneID(m_profile.hinge_bone.c_str());
    const bool parts=!magazine_only && PoseParts(model,poses);
    const bool path=!magazine_only && PoseActionPath(model,poses);
    if (!animated && hinge_bone==BI_NONE && !parts && !path)
    {
        if (hidden)
        {
            model.CalculateBones_Invalidate();
            model.CalculateBones(true);
            vr::CollapseHiddenSkinning(model);
        }
        return;
    }
    if (hinge_bone!=BI_NONE)
    {
        Fmatrix hinge; HingePose(hinge);
        // Rounds not on the barrels' bone move with them too.
        const u16 round0=m_profile.shell_bones[0].size() ? model.LL_BoneID(m_profile.shell_bones[0].c_str()) : u16(BI_NONE);
        const u16 round1=m_profile.shell_bones[1].size() ? model.LL_BoneID(m_profile.shell_bones[1].c_str()) : u16(BI_NONE);
        for (u16 id=0;id<model.LL_BoneCount() && id<poses.size();++id)
            if (Descends(model,id,hinge_bone) || (round0!=BI_NONE && Descends(model,id,round0)) ||
                (round1!=BI_NONE && Descends(model,id,round1)))
            {
                Fmatrix moved; moved.mul_43(hinge,poses[id]);
                poses[id]=moved;
            }
    }
    model.CalculateBones_Invalidate();
    model.CalculateBones(true);
    vr::CollapseHiddenSkinning(model);
}

bool CVrWeaponInteraction::CopyNativeVisibility(IKinematics& model) const
{
    attachable_hud_item* item=m_weapon.IsAttachedToHUD() ? m_weapon.HudItemData() : nullptr;
    IKinematics* hud=item ? item->m_model : nullptr;
    const u16 count=model.LL_BoneCount();
    if (!hud || hud==&model || hud->LL_BoneCount()!=count) return false;
    // Parts VR shows itself keep its state: the magazine, its rounds and
    // shells loaded by hand.
    u16 owned[6]={BI_NONE,BI_NONE,BI_NONE,BI_NONE,BI_NONE,BI_NONE};
    const shared_str* names[6]={&m_profile.mag_bone,&m_profile.mag_extra_bones[0],&m_profile.mag_extra_bones[1],
        &m_profile.rounds_bone,&m_profile.shell_bones[0],&m_profile.shell_bones[1]};
    for (int n=0;n<6;++n)
        if (names[n]->size()) owned[n]=model.LL_BoneID(names[n]->c_str());
    bool changed=false;
    for (u16 id=0;id<count;++id)
    {
        if (hud->LL_GetBoneVisible(id) || !model.LL_GetBoneVisible(id)) continue;
        bool vr_part=false;
        for (u16 a=id; a<count && !vr_part; a=model.LL_GetData(a).GetParentID())
            for (u16 o:owned) vr_part=vr_part || a==o;
        if (vr_part) continue;
        model.LL_SetBoneVisible(id,FALSE,FALSE);
        changed=true;
    }
    return changed;
}

void CVrWeaponInteraction::PrepareParts(IKinematics& model)
{
    const u16 count=model.LL_BoneCount();
    if (m_overlay_order.size()==count) return;
    m_overlay_receiver.assign(count,0);
    m_overlay_owned.assign(count,0);
    for (u16 id=FindTrigger(model); id<count; id=model.LL_GetData(id).GetParentID())
        m_overlay_receiver[id]=true;
    m_overlay_receiver[model.LL_GetBoneRoot()]=true;
    // VR moves the magazine, shells and hinged barrels itself.
    for (const shared_str* name:{&m_profile.mag_bone,&m_profile.rounds_bone,&m_profile.mag_extra_bones[0],
        &m_profile.mag_extra_bones[1],&m_profile.shell_bones[0],&m_profile.shell_bones[1],
        &m_profile.hidden_parts[0],&m_profile.hidden_parts[1],&m_profile.hidden_parts[2],
        &m_profile.hinge_bone,&m_profile.belt_free_bone,&m_profile.part_bones[0],&m_profile.part_bones[1],
        &m_profile.part_bones[2],&m_profile.part_bones[3]})
        if (name->size())
        {
            const u16 id=model.LL_BoneID(name->c_str());
            if (id<count) m_overlay_owned[id]=true;
        }
    // Parents before children.
    xr_vector<u16> depth(count,0);
    m_overlay_order.clear();
    for (u16 id=0;id<count;++id)
    {
        for (u16 a=model.LL_GetData(id).GetParentID(); a<count && depth[id]<count; a=model.LL_GetData(a).GetParentID())
            ++depth[id];
        m_overlay_order.push_back(id);
    }
    std::sort(m_overlay_order.begin(),m_overlay_order.end(),[&](u16 a, u16 b) { return depth[a]<depth[b]; });
}

bool CVrWeaponInteraction::AnimateParts(IKinematics& model, xr_vector<Fmatrix>& poses)
{
    // Parts are posed relative to their parents, parents first, so each
    // rides on the VR pose of everything above it:
    // - triggers follow the finger (pulled pose sampled from the shot);
    // - a break-action shows the state of its loaded barrels the way its own
    //   idle animations do (cocking indicators, extractors), updated when the
    //   barrels close and as each barrel fires;
    // - other parts the first-person animation moves (bolt carrier, hammer,
    //   charging handle, dust cover) copy it, except the receiver chain that
    //   carries the trigger (the gun never kicks out of the hand) and what VR
    //   moves itself (magazine, shells, barrels, a known slide, bolt or pump).
    const u16 count=model.LL_BoneCount();
    if (poses.size()<count) return false;
    PrepareParts(model);
    IKinematics* hud=nullptr;
    if (m_weapon.IsAttachedToHUD())
        if (attachable_hud_item* item=m_weapon.HudItemData())
            if (item->m_model && item->m_model->LL_BoneCount()==count) hud=item->m_model;
    const u16 cycle=m_profile.cycle_bone.size() ? model.LL_BoneID(m_profile.cycle_bone.c_str()) : BI_NONE;
    const u16 pump=m_profile.pump_bone.size() ? model.LL_BoneID(m_profile.pump_bone.c_str()) : BI_NONE;
    const u16 magazine=m_profile.detachable ? model.LL_BoneID(m_profile.mag_bone.c_str()) : BI_NONE;
    const bool seated=m_mechanism.inserted>=0 && !m_inserting &&
        (!m_profile.belt.used || m_belt.seated);
    const bool states=m_profile.break_action && m_state_local[0].size()==count &&
        m_state_local[1].size()==count && m_state_local[2].size()==count;
    // Until the barrels are first closed or fired, show what is loaded.
    const unsigned state=std::min(m_break_state_set ? m_break_state : unsigned(m_weapon.m_magazine.size()),2u);
    // The trigger the next shot uses: the second of a double gun once the
    // first barrel is spent.
    const int pulled=m_profile.break_action && m_chambers[0]!=1 && m_trigger_bone[1]!=BI_NONE ? 1 : 0;
    bool changed=false;
    for (u16 id:m_overlay_order)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        if (parent>=count) continue;
        int trigger=-1;
        for (int t=0;t<2;++t) if (id==m_trigger_bone[t] && m_trigger_valid[t]) trigger=t;
        if (trigger>=0)
        {
            const float amount=trigger==pulled ? m_trigger_value : 0.f;
            Fquaternion rest, full, blend;
            rest.set(m_bind_local[id]); full.set(m_trigger_pull[trigger]);
            blend.slerp(rest,full,amount);
            Fmatrix local; local.rotation(blend);
            local.c.lerp(m_bind_local[id].c,m_trigger_pull[trigger].c,amount);
            poses[id].mul_43(poses[parent],local);
            changed=true;
            continue;
        }
        if (states && m_state_part[id])
        {
            poses[id].mul_43(poses[parent],m_state_local[state][id]);
            changed=true;
            continue;
        }
        if (!hud || m_overlay_receiver[id]) continue;
        bool vr_driven=false;
        for (u16 a=id; a<count && !vr_driven; a=model.LL_GetData(a).GetParentID())
        {
            // Parts inside a seated magazine (a belt's links, a drum's
            // follower) move as the weapon's own animation moves them, as a
            // belt feeding on each shot; the magazine itself stays VR's.
            const bool inside_seated=a==magazine && a!=id && seated;
            vr_driven=(m_overlay_owned[a] && !inside_seated) || a==cycle || a==pump;
        }
        if (vr_driven) continue;
        Fmatrix to_parent; to_parent.invert(hud->LL_GetTransform(parent));
        Fmatrix local; local.mul_43(to_parent,hud->LL_GetTransform(id));
        poses[id].mul_43(poses[parent],local);
        changed=true;
    }
    return changed;
}

void CVrWeaponInteraction::MeasureParts(IKinematics& model)
{
    // Poses for parts VR moves from state rather than from the live
    // first-person animation, sampled from the weapon's own animations on a
    // private copy of its HUD model: each trigger at its furthest point in
    // the shot, and a break-action's idle for 0, 1 and 2 loaded barrels.
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (bind.size()<count) return;
    m_bind_local.assign(count,Fidentity);
    for (u16 id=0;id<count;++id)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        if (parent>=count) { m_bind_local[id]=bind[id]; continue; }
        Fmatrix inverse; inverse.invert(bind[parent]);
        m_bind_local[id].mul_43(inverse,bind[id]);
    }
    m_trigger_bone[0]=FindTrigger(model);
    m_trigger_bone[1]=model.LL_BoneID("trigger2");
    m_trigger_valid[0]=m_trigger_valid[1]=false;
    m_break_state=unsigned(m_weapon.m_magazine.size());
    LPCSTR hud=m_weapon.HudSection().c_str();
    IRenderVisual* visual=HudCopy(m_profile.visual.c_str());
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* sample=visual ? visual->dcast_PKinematics() : nullptr;
    const auto play=[&](LPCSTR key) { return PlayHudMotion(*animated,hud,key); };
    const auto local=[&](u16 id) -> Fmatrix
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        Fmatrix result=sample->LL_GetTransform(id);
        if (parent<count)
        {
            Fmatrix inverse; inverse.invert(sample->LL_GetTransform(parent));
            result.mulA_43(inverse);
        }
        return result;
    };
    const auto step=[&](float dt)
    {
        animated->LL_UpdateTracks(dt,true,true);
        sample->CalculateBones_Invalidate();
        sample->CalculateBones(TRUE);
    };
    if (animated && sample && sample->LL_BoneCount()==count)
    {
        float deviation[2]={0.002f,0.002f};
        for (LPCSTR key:{"anm_shots","anm_shot","anm_shot_1","anm_shot_2","anm_shot_l"})
        {
            if (!play(key)) continue;
            for (int frame=0;frame<60;++frame)
            {
                step(frame ? 1.f/60.f : 0.f);
                for (int t=0;t<2;++t)
                {
                    const u16 id=m_trigger_bone[t];
                    if (id>=count) continue;
                    const Fmatrix posed=local(id);
                    const Fmatrix& rest=m_bind_local[id];
                    const float turn=3.f-posed.i.dotproduct(rest.i)-posed.j.dotproduct(rest.j)-
                        posed.k.dotproduct(rest.k);
                    if (turn>deviation[t]) { deviation[t]=turn; m_trigger_pull[t]=posed; m_trigger_valid[t]=true; }
                }
            }
        }
        if (m_profile.break_action)
        {
            bool sampled=true;
            for (unsigned loaded=0;loaded<3 && sampled;++loaded)
            {
                string32 key; xr_sprintf(key,"anm_idle_%u",loaded);
                sampled=play(key);
                if (!sampled) break;
                step(0.f);
                m_state_local[loaded].resize(count);
                for (u16 id=0;id<count;++id) m_state_local[loaded][id]=local(id);
            }
            m_state_part.assign(count,0);
            const u16 hinge=model.LL_BoneID(m_profile.hinge_bone.c_str());
            int parts=0;
            for (u16 id=0;sampled && id<count;++id)
            {
                if (model.LL_GetData(id).GetParentID()>=count || id==hinge ||
                    id==m_trigger_bone[0] || id==m_trigger_bone[1]) continue;
                for (unsigned a=0;a<3 && !m_state_part[id];++a)
                    for (unsigned b=a+1;b<3 && !m_state_part[id];++b)
                    {
                        const Fmatrix& p=m_state_local[a][id];
                        const Fmatrix& q=m_state_local[b][id];
                        m_state_part[id]=p.c.distance_to(q.c)>0.0005f ||
                            3.f-p.i.dotproduct(q.i)-p.j.dotproduct(q.j)-p.k.dotproduct(q.k)>0.0005f;
                    }
                parts+=m_state_part[id];
            }
            if (!sampled) for (auto& states:m_state_local) states.clear();
            Msg("* [VR weapon] %s: %d parts follow the loaded barrels",m_profile.section.c_str(),parts);
        }
    }
    if (visual) ::Render->model_Delete(visual);
}

void CVrWeaponInteraction::MeasureLauncher(IKinematics& model)
{
    // Where the support trigger selects an underbarrel launcher: its own
    // mesh, else under the barrel ahead of the trigger.
    BoneBounds part;
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (NameHas(model.LL_BoneName_dbg(id),"launcher")) model.EnumBoneVertices(part,id);
    if (part.count) part.box.getcenter(m_profile.launcher_grab);
    else
    {
        m_profile.launcher_grab=m_profile.has_trigger_point ? m_profile.trigger_point : m_profile.muzzle;
        if (m_profile.has_trigger_point) m_profile.launcher_grab.add(Fvector().set(0.f,-0.03f,0.22f));
    }
}

bool CVrWeaponInteraction::EnsureVisual()
{
    if (m_visual) return true;
    m_visual=::Render->model_Create(m_profile.visual.c_str());
    if (!m_visual) { m_supported=false; return false; }
    if (auto* skeleton=m_visual->dcast_PKinematics())
    {
        CTimer measure; measure.Start();
        if (m_profile.automatic) AutoProfile(*skeleton);
        MeasureLauncher(*skeleton);
        if (!ValidateModel(*skeleton))
        { ReleaseModels(); m_supported=false; return false; }
        MeasureGeometry(*skeleton);
        MeasureParts(*skeleton);
        MeasurePartMotion(*skeleton);
        Msg("* [VR weapon] %s: measured on first draw in %.1f ms",m_profile.section.c_str(),
            measure.GetElapsed_sec()*1000.f);
        // What the weapon is mechanically, each time it is measured (the
        // profile it sets is measured again with the model).
        ClassifyMechanics(*skeleton);
        if (!m_strategy_logged)
        {
            // How this weapon reloads in VR, decided once per model.
            m_strategy_logged=true;
            LPCSTR strategy=m_profile.part_count || m_profile.belt.used || !m_profile.rules.Empty() ?
                "interactive procedure" : m_profile.detachable ? "physical magazine" :
                (m_profile.break_action || m_profile.tube) ? "physical shells" : "native reload";
            Msg("* [VR weapon] %s: reload strategy: %s (parts %d, belt %d)",m_profile.section.c_str(),
                strategy,m_profile.part_count,m_profile.belt.used);
        }
    }
    else
    {
        Msg("! [VR weapon] %s: model is not skeletal",m_profile.section.c_str());
        ReleaseModels(); m_supported=false; return false;
    }
    return true;
}

xr_string CVrWeaponInteraction::MagazineVisualName(unsigned rounds) const
{
    if (!m_profile.magazine_visual_prefix.size()) return m_profile.visual.c_str();
    string_path name;
    xr_sprintf(name,"%s%u.ogf",m_profile.magazine_visual_prefix.c_str(),
        std::min(rounds,m_profile.magazine_visual_max_rounds));
    return name;
}

bool CVrWeaponInteraction::EnsureMagazineVisual(unsigned rounds)
{
    const unsigned variant=m_profile.magazine_visual_prefix.size() ?
        std::min(rounds,m_profile.magazine_visual_max_rounds) : 0;
    if (m_mag_visual && m_mag_visual_count==variant) return true;
    ReleaseModel(m_mag_visual);
    m_mag_visual_count=unsigned(-1);
    const xr_string name=MagazineVisualName(rounds);
    m_mag_visual=::Render->model_Create(name.c_str());
    if (!m_mag_visual || !m_mag_visual->dcast_PKinematics())
    {
        Msg("! [VR weapon] %s: magazine visual %s unavailable",m_profile.section.c_str(),name.c_str());
        ReleaseModel(m_mag_visual);
        return false;
    }
    m_mag_visual_count=variant;
    Msg("* [VR weapon] %s: magazine mesh %s for %u rounds",m_profile.section.c_str(),name.c_str(),rounds);
    return true;
}

unsigned CVrWeaponInteraction::PreviewRoundCount() const
{
    if (m_weapon.unlimited_ammo()) return unsigned(std::max(0,m_weapon.iMagazineSize));
    if (!m_weapon.m_pInventory) return 0;
    // The pouch's rounds fill it (FillMagazine); any type when there are none.
    unsigned available=0, chosen=0;
    const shared_str pouch=m_weapon.m_ammoTypes.empty() ? shared_str() : m_weapon.m_ammoTypes[PouchAmmo()];
    for (auto* item:m_weapon.m_pInventory->m_all)
        if (auto* box=smart_cast<CWeaponAmmo*>(item); box && box->m_boxCurr &&
            std::find(m_weapon.m_ammoTypes.begin(),m_weapon.m_ammoTypes.end(),
                box->cNameSect())!=m_weapon.m_ammoTypes.end())
        {
            available+=box->m_boxCurr;
            if (box->cNameSect()==pouch) chosen+=box->m_boxCurr;
        }
    return std::min(chosen ? chosen : available,unsigned(std::max(0,m_weapon.iMagazineSize)));
}

bool CVrWeaponInteraction::Render()
{
    if (!Mode())
    {
        // An unprofiled weapon must not fall through to the flat desktop HUD
        // arms. Render its third-person, arm-free model from the tracked grip
        // while preserving normal gameplay and ammunition handling.
        // Not the knife or binoculars: their first-person model is drawn as
        // the idle holds it (CHUDManager, VrKnifePose), the pose the knife's
        // strikes use. Their NPC models here pointed out of the fist the
        // wrong way.
        if (!g_vr_owns_present || !IsGameTypeSingle() || smart_cast<CWeaponKnife*>(&m_weapon) ||
            smart_cast<CWeaponBinoculars*>(&m_weapon) ||
            !vr::GameplayActive() || !m_weapon.Visual()) return false;
        auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
        if (!actor || actor!=Level().CurrentControlEntity() ||
            actor->inventory().ActiveItem()!=&m_weapon ||
            actor->cam_Active()!=actor->cam_FirstEye()) return false;
        if (!m_reported_generic)
        {
            Msg("* [VR weapon] %s: displaying tracked world model (physical VR interaction unavailable)",
                m_weapon.cNameSect().c_str());
            m_reported_generic=true;
        }
        Fmatrix world;
        if (!GenericTransform(world)) return false;
        vr::HideHandForFrame(HandOf(1));
        ::Render->set_Transform(&world);
        ::Render->add_Visual(m_weapon.Visual());
        return true;
    }
    Fmatrix world;
    if (!Transform(world)) return true;
    vr::HideHandForFrame(HandOf(1));
    if (m_mechanism.held>=0 || m_racking || m_supporting || m_shell_held)
        vr::HideHandForFrame(HandOf(0));
    if (!EnsureVisual()) return false;
    auto* model=m_visual->dcast_PKinematics();
    if (!model) return true;
    const bool rounds=m_weapon.m_magazine.size()>unsigned(m_mechanism.chamber);
    // Both eyes draw the same pose.
    if (m_posed_frame!=Device.dwFrame) { Pose(*model,m_poses[0],false,rounds); m_posed_frame=Device.dwFrame; }
    ::Render->set_Transform(&world);
    ::Render->add_Visual(m_visual);
    Fmatrix shell_pose;
    if (ShellLoading() && m_shell_held && m_profile.shell_bones[1].size() && ShellHandPose(shell_pose))
    {
        if (!m_shell_visual) m_shell_visual=::Render->model_Create(m_profile.visual.c_str());
        if (auto* shell=m_shell_visual ? m_shell_visual->dcast_PKinematics() : nullptr)
        {
            shell->Callback(CollapseHiddenBones,nullptr);
            vr::ApplyWeaponPose(*shell,m_shell_poses,BonePose,bctCustom,nullptr,0.f,nullptr,
                {m_profile.shell_bones[1].c_str()},true,true,true);
            auto& vis=m_shell_visual->getVisData();
            vis.box.min.sub(m_profile.shell_center[1],m_profile.shell_box_half);
            vis.box.max.add(m_profile.shell_center[1],m_profile.shell_box_half);
            vis.box.getsphere(vis.sphere.P,vis.sphere.R);
            ::Render->set_Transform(&shell_pose);
            ::Render->add_Visual(m_shell_visual);
        }
    }
    else if (ShellLoading() && m_shell_held &&
        (m_pouch_visual || (m_weapon.m_ammoTypes.size() && [&]()
        {
            const shared_str& ammo=m_weapon.m_ammoTypes[PouchAmmo()];
            if (pSettings->line_exist(ammo,"visual"))
            {
                m_pouch_section=ammo;
                m_pouch_visual=::Render->model_Create(pSettings->r_string(ammo,"visual"));
            }
            return m_pouch_visual!=nullptr;
        }())))
    {
        // No round in the weapon's model (a launcher's grenade): the
        // ammunition's own model, in the support hand.
        Fmatrix hand;
        if (vr::HandTransform(HandOf(0),false,hand))
        {
            Fmatrix pose=hand;
            pose.c.mad(hand.k,0.09f);
            ::Render->set_Transform(&pose);
            ::Render->add_Visual(m_pouch_visual);
        }
    }
    if (m_profile.detachable && m_mechanism.held>=0)
    {
        // Placed on this frame's weapon pose (its pose is kept in weapon
        // space), so it cannot trail a moving gun.
        if (!HeldMagazineKnown()) return true;  // the hand is not tracked: not drawn
        Fmatrix pose=SolvedMagazine(world);
        const unsigned mag_count=m_mechanism.magazines[m_mechanism.held].empty() && m_pouch_preview ?
            PreviewRoundCount() : unsigned(m_mechanism.magazines[m_mechanism.held].size());
        if (!EnsureMagazineVisual(mag_count)) return true;
        auto* magazine=m_mag_visual->dcast_PKinematics();
        if (!magazine) return true;
        // The extracted PM mesh retains the stock skeletal hierarchy. Apply
        // the same audited bind pose as the equipped pistol: using its default
        // animation pose offsets the loose magazine from the well.
        Pose(*magazine,m_poses[1],true,mag_count>0);
        auto& vis=m_mag_visual->getVisData();
        vis.box.min.set(m_profile.mag_box_center.x-m_profile.mag_box_half.x,
            m_profile.mag_box_center.y-m_profile.mag_box_half.y,
            m_profile.mag_box_center.z-m_profile.mag_box_half.z);
        vis.box.max.set(m_profile.mag_box_center.x+m_profile.mag_box_half.x,
            m_profile.mag_box_center.y+m_profile.mag_box_half.y,
            m_profile.mag_box_center.z+m_profile.mag_box_half.z);
        vis.box.getsphere(vis.sphere.P,vis.sphere.R);
        ::Render->set_Transform(&pose);
        ::Render->add_Visual(m_mag_visual);
    }
    // A released magazine sliding out of the well.
    if (m_ejecting>=0 && m_eject_visual && m_contact && m_contact->path.Valid())
        if (auto* magazine=m_eject_visual->dcast_PKinematics())
        {
            Fmatrix pose; pose.mul_43(world,ToMatrix(m_contact->path.At(m_eject_s)));
            Pose(*magazine,m_eject_poses,true,!m_mechanism.magazines[m_ejecting].empty());
            auto& vis=m_eject_visual->getVisData();
            vis.box.min.sub(m_profile.mag_box_center,m_profile.mag_box_half);
            vis.box.max.add(m_profile.mag_box_center,m_profile.mag_box_half);
            vis.box.getsphere(vis.sphere.P,vis.sphere.R);
            ::Render->set_Transform(&pose);
            ::Render->add_Visual(m_eject_visual);
        }
    return true;
}

bool CVrWeaponInteraction::HolsterRight() const
{
    return m_weapon.CurrSlot()!=INV_SLOT_3;
}
Fmatrix CVrWeaponInteraction::HolsterPose(CActor& actor) const
{
    const Fmatrix body=VrTorsoFrame(actor);
    Fmatrix pose; pose.identity();
    pose.i=body.i; pose.j=body.k; pose.k.set(0,-1,0);
    pose.c=HolsterPoint(actor);
    Fvector grip=m_profile.grip_offset;
    pose.transform_dir(grip);
    pose.c.sub(grip);
    return pose;
}
void CVrWeaponInteraction::RenderHolster()
{
    if (!g_vr_owns_present || !IsGameTypeSingle()) return;
    auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
    if (!actor || actor!=Level().CurrentControlEntity() ||
        (m_weapon.CurrSlot()!=INV_SLOT_2 && m_weapon.CurrSlot()!=INV_SLOT_3) ||
        actor->inventory().ActiveItem()==&m_weapon ||
        actor->inventory().ItemFromSlot(m_weapon.CurrSlot())!=&m_weapon) return;
    Fmatrix world=HolsterPose(*actor);
    // A gun without an audited physical profile still gets a visible world
    // model in its slot; only the advanced magazine/slide posing needs one.
    if (!Mode())
    {
        if (m_weapon.Visual())
        {
            ::Render->set_Transform(&world);
            ::Render->add_Visual(m_weapon.Visual());
        }
        return;
    }
    if (!EnsureVisual()) return;
    auto* model=m_visual->dcast_PKinematics();
    if (!model) return;
    const bool rounds=m_weapon.m_magazine.size()>unsigned(m_mechanism.chamber);
    if (m_posed_frame!=Device.dwFrame) { Pose(*model,m_poses[0],false,rounds); m_posed_frame=Device.dwFrame; }
    ::Render->set_Transform(&world);
    ::Render->add_Visual(m_visual);
}

void CVrWeaponInteraction::ReleaseModels()
{
    m_posed_frame=u32(-1);
    m_authored_sampled=false;
    m_overlay_order.clear();
    m_bind_local.clear();
    for (auto& states:m_state_local) states.clear();
    ReleaseModel(m_visual);
    ReleaseModel(m_mag_visual);
    ReleaseModel(m_eject_visual);
    ReleaseModel(m_shell_visual);
    ReleaseModel(m_pouch_visual);
    m_pouch_section=nullptr;
    m_shell_poses.clear();
    m_mag_visual_count=unsigned(-1);
    m_poses[0].clear(); m_poses[1].clear();
}
// A dropped magazine's rounds were only ever lying on the ground for show:
// they go back to reserve as the prop goes.
void CVrWeaponInteraction::RetireProp(DropProp& prop, bool give_back)
{
    if (give_back && !prop.cartridges.empty()) ReturnCartridges(prop.cartridges);
    prop.cartridges.clear();
    if (prop.shell) destroy_physics_shell(prop.shell);
    prop.shell=nullptr;
    ReleaseModel(prop.visual);
}
void CVrWeaponInteraction::ReleaseProps(bool give_back)
{
    for (auto& prop:m_props) RetireProp(prop,give_back);
    m_props.clear();
}
void CVrWeaponInteraction::Destroy()
{
    ReleaseProps(false); ReleaseModels();
}

Fvector CVrWeaponInteraction::Hip(CActor& actor, bool right) const
{
    return VrHipPoint(actor,right);
}
Fvector CVrWeaponInteraction::Shoulder(CActor& actor) const
{
    return VrShoulderPoint(actor,!vr::LeftHanded());
}
Fvector CVrWeaponInteraction::HolsterPoint(CActor& actor) const
{
    // Non-physical long guns are stowed over the shoulder; handguns retain
    // the pair of hip holsters used by the physical pistol interactions.
    // Pistols ride the right hip; long guns go over the right shoulder, so
    // neither overlaps the ammunition pouch on the left hip.
    if (m_weapon.CurrSlot()==INV_SLOT_3) return Shoulder(actor);
    return Hip(actor,!vr::LeftHanded());
}
namespace
{
// How far from its holster a hand draws a weapon or puts it back: the hip
// holster no further than the hand would be on the pistol's grip, so the
// belt beside it and a hand hanging at the side stay clear of it.
float HolsterRange(u16 slot) { return slot==INV_SLOT_3 ? 0.30f : 0.18f; }
}
bool CVrWeaponInteraction::HolsterGrabDistance(CActor& actor, const Fvector& hand,
    float& distance) const
{
    if (m_weapon.H_Parent()!=&actor ||
        (m_weapon.CurrSlot()!=INV_SLOT_2 && m_weapon.CurrSlot()!=INV_SLOT_3)) return false;
    distance=hand.distance_to(HolsterPoint(actor));
    return distance<=HolsterRange(m_weapon.CurrSlot());
}
bool CVrWeaponInteraction::HolsterPointFor(CActor& actor, Fvector& point, float& range) const
{
    // A knife or binoculars held as a weapon goes back to its body slot.
    if (VrBodySlotHolster(actor,m_weapon.CurrSlot(),point,range)) return true;
    if (m_weapon.CurrSlot()!=INV_SLOT_2 && m_weapon.CurrSlot()!=INV_SLOT_3) return false;
    point=HolsterPoint(actor);
    range=HolsterRange(m_weapon.CurrSlot());
    return true;
}
bool CVrWeaponInteraction::Handgun() const
{
    // Whether the weapon is held in one hand: its class, not its
    // hand_dependence, which many rifles (the AK-74M among them) set to 1.
    // Revolvers share their base with shotguns and launchers, not pistols.
    return smart_cast<CWeaponPistol*>(&m_weapon)!=nullptr || smart_cast<CWeaponRevolver*>(&m_weapon)!=nullptr;
}
bool CVrWeaponInteraction::KeptWithoutGrip() const
{
    // Every weapon is held only while its grip is (hold, never toggle): a
    // pistol kept in a relaxed hand behaved as a toggle.
    return false;
}
bool CVrWeaponInteraction::ReleaseWanted(CActor& actor, bool opened, bool open)
{
    // A weapon just drawn stays in the hand until the hand has been clear of
    // its holster: relaxing the grip on the way out no longer put it straight
    // back. Then a long gun is let go where the hand opens (back over the
    // shoulder, or dropped), as before. A pistol is held with the grip
    // relaxed too, and goes back only when its hand opens at the holster,
    // comes to rest there open for 0.3 s, or tosses it (let go moving faster
    // than 3 m/s, the body's own motion aside).
    Fmatrix hand;
    Fvector point;
    float range=0.f;
    const float distance=vr::HandTransform(HandOf(1),false,hand) && HolsterPointFor(actor,point,range) ?
        hand.c.distance_to(point) : FLT_MAX;
    if (distance>range+0.04f) m_holster_armed=true;
    const bool at=m_holster_armed && distance<=range;
    if (!at || !open) m_holster_since=0;
    else if (!m_holster_since) m_holster_since=Device.dwTimeGlobal|1;
    if (!KeptWithoutGrip()) return opened && (at || distance>range);
    if (opened && at) return true;
    if (m_holster_since && Device.dwTimeGlobal-m_holster_since>=300) return true;
    if (!opened) return false;
    Fvector toss; toss.sub(actor.m_vr_pickup.hand_velocity[HandOf(1)],VrCarriedVelocity(actor));
    return toss.magnitude()>3.f;
}
bool CVrWeaponInteraction::MagazineGrab(const Fmatrix& world, const Fvector& hand, float& along) const
{
    // The magazine's exposed surface, measured from its mesh (a curved or
    // raked magazine is taken where it is), within 6.5 cm of the hand's
    // grip centre: the fingers are round it.
    if (m_contact && !m_contact->grab_samples.empty())
    {
        Fmatrix inverse; inverse.invert(world);
        Fvector local; inverse.transform_tiny(local,hand);
        const vr::V3 at=V(local);
        float nearest=flt_max;
        for (const vr::V3& p:m_contact->grab_samples) nearest=std::min(nearest,vr::Dot(p-at,p-at));
        if (nearest>0.065f*0.065f) return false;
        local.sub(m_profile.mag_base);
        along=clampr(local.dotproduct(m_profile.well_axis),-0.05f,2.f*m_profile.mag_box_half.y);
        return true;
    }
    // Around the magazine's body below where it enters the weapon (a
    // pistol's grip hides all but the floorplate, taken 5 cm up it; a
    // rifle's magazine its whole length), from 10 cm under the floorplate:
    // long along the magazine, not wider. Within the controller's
    // grip centre's distance of the body when the fingers are round it. The
    // sphere round the floorplate this replaces sat inside the receiver of
    // many rifles, so a hand had to reach up through the weapon to take
    // their magazine.
    Fmatrix inverse; inverse.invert(world);
    Fvector local; inverse.transform_tiny(local,hand);
    local.sub(m_profile.mag_base);
    const Fvector& axis=m_profile.well_axis;
    along=local.dotproduct(axis);
    Fvector across=local; across.mad(axis,-along);
    const float length=2.f*m_profile.mag_box_half.y;
    const float exposed=Handgun() ? 0.05f : std::max(length,0.04f);
    const float girth=std::max(m_profile.mag_box_half.x,m_profile.mag_box_half.z)+0.055f;
    if (along<-0.10f || along>exposed || across.magnitude()>girth) return false;
    along=clampr(along,-0.05f,exposed);
    return true;
}
u8 CVrWeaponInteraction::PouchAmmo() const
{
    const size_t count=m_weapon.m_ammoTypes.size();
    if (!count) return 0;
    if (m_pouch_ammo<count && (m_weapon.unlimited_ammo() || !m_weapon.m_pInventory ||
        m_weapon.m_pInventory->GetAny(m_weapon.m_ammoTypes[m_pouch_ammo].c_str())))
        return m_pouch_ammo;
    const u8 next=m_weapon.m_set_next_ammoType_on_reload;
    return u8(std::min<size_t>(next<count ? next : m_weapon.m_ammoType,count-1));
}
bool CVrWeaponInteraction::SwapPouchAmmo()
{
    // As the desktop's next-ammunition key: the next type carried.
    const auto& types=m_weapon.m_ammoTypes;
    if (types.size()<2 || !m_weapon.m_pInventory) return false;
    const u8 current=PouchAmmo();
    for (size_t step=1;step<types.size();++step)
    {
        const u8 type=u8((current+step)%types.size());
        if (!m_weapon.unlimited_ammo() && !m_weapon.m_pInventory->GetAny(types[type].c_str())) continue;
        m_pouch_ammo=type;
        m_pouch_swap_time=Device.dwTimeGlobal;
        // A loaded spare magazine waiting in the pouch goes back into the
        // boxes: the next one taken is filled with the new rounds.
        for (int index=0;index<2;++index)
            if (index!=m_mechanism.inserted && index!=m_mechanism.held && !m_mechanism.magazines[index].empty())
            {
                ReturnCartridges(m_mechanism.magazines[index]);
                m_mechanism.magazines[index].clear();
            }
        // Shells loaded one by one and native reloads take the type the
        // desktop key would have set.
        if (!Mode() || !m_profile.detachable) m_weapon.m_set_next_ammoType_on_reload=type;
        Msg("* [VR weapon] %s: pouch now gives %s",m_weapon.cNameSect().c_str(),types[type].c_str());
        return true;
    }
    return false;
}
bool CVrWeaponInteraction::PouchAmmoShown(shared_str& section) const
{
    if (m_weapon.m_ammoTypes.empty() || !m_pouch_swap_time || Device.dwTimeGlobal-m_pouch_swap_time>1500) return false;
    section=m_weapon.m_ammoTypes[PouchAmmo()];
    return true;
}
bool CVrWeaponInteraction::GripField(int side, vr::ContactScene& scene, Fmatrix& world, float& ceiling,
    const void*& object, vr::GripKind& kind)
{
    scene.Clear();
    if (Transform(world))
    {
        if (!m_contact) return false;
        // A support hand holding a magazine closes on the magazine.
        if (side==HandOf(0) && m_mechanism.held>=0)
        {
            if (!HeldMagazineKnown()) return false;
            if (m_contact->magazine_surface.Empty()) return false;
            scene.Add(&m_contact->magazine_surface,ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),SolvedMagazine(world))));
            ceiling=flt_max;
            object=&m_contact->magazine_surface;
            kind=vr::GripKind::Magazine;
            return true;
        }
        MagazineScene(scene);
        ceiling=m_profile.bore_height;
        object=m_contact.get();
        kind=side==HandOf(1) ? vr::GripKind::PistolGrip :
            m_racking ? vr::GripKind::ChargingHandle : vr::GripKind::ForeEnd;
        return scene.count>0;
    }
    // An unmeasured weapon is held as its world model; its surface is
    // measured once per visual.
    if (!GenericTransform(world) || !m_weapon.Visual()) return false;
    if (m_world_contact_visual!=m_weapon.Visual())
    {
        static xr_map<shared_str,std::shared_ptr<const VrWeaponContact>> cache;
        m_world_contact_visual=m_weapon.Visual();
        const shared_str name=m_weapon.cNameVisual();
        auto found=cache.find(name);
        if (found!=cache.end()) m_world_contact=found->second;
        else if (auto* model=m_weapon.Visual()->dcast_PKinematics())
        {
            auto contact=std::make_shared<VrWeaponContact>();
            std::vector<vr::Tri> tris;
            TriangleCollector collector(tris);
            for (u16 id=0;id<model->LL_BoneCount();++id) model->EnumBoneVertices(collector,id);
            contact->receiver.Build(tris,0.0025f,0.012f,4);
            m_world_contact=cache[name]=contact;
        }
    }
    if (!m_world_contact || m_world_contact->receiver.Empty()) return false;
    scene.Add(&m_world_contact->receiver,vr::Frame());
    ceiling=m_weapon.vLoadedFirePoint.y-0.004f;
    object=m_world_contact.get();
    kind=side==HandOf(1) ? vr::GripKind::PistolGrip : vr::GripKind::ForeEnd;
    return true;
}
bool CVrWeaponInteraction::MagazineHandPose(int hand, Fmatrix& root) const
{
    // The magazine stands upright in the fist: its well axis follows the
    // controller's up and its front the pointing direction (aim pose), so a
    // hand held like the weapon hand presents it straight to the well, without
    // the forward lean of the controller handle. Its base sits at the handle
    // centre, so the whole magazine is above the fist and reaches the well
    // without lifting the hand into the weapon.
    Fmatrix grip, aim;
    if (!vr::HandTransform(hand,false,grip) || !vr::HandTransform(hand,true,aim)) return false;
    // Taken out of the weapon by hand: exactly where the hand closed on it,
    // held rigidly (a curved or raked magazine does not jump to a hold
    // point 10-20 cm off the fingers).
    if (m_mag_grabbed && hand==HandOf(0)) { root.mul_43(grip,m_mag_in_hand); return true; }
    // Taken out by its body, it stays where it was in the hand: moved along
    // its axis so the point the hand closed on is in the fist.
    const auto where_grabbed=[&]()
    {
        if (!m_mag_grabbed) return;
        Fmatrix inverse; inverse.invert(root);
        Fvector local; inverse.transform_tiny(local,grip.c);
        local.sub(m_profile.mag_base);
        Fvector up; root.transform_dir(up,m_profile.well_axis);
        root.c.mad(up,local.dotproduct(m_profile.well_axis)-m_mag_grab_along);
    };
    if (vr::AnimatedGrip() && hand==0 && m_hand==1 && !m_maghand_local.empty())
    {
        // The controller is the reload animation's left hand: its knuckles
        // face the controller's front (-Y), its index side up the handle
        // (+Z), its palm on the handle centre; the magazine sits in that hand
        // exactly as animated.
        Fvector frame[3];
        frame[0].invert(grip.j);
        frame[1]=grip.k;
        frame[1].mad(frame[0],-frame[1].dotproduct(frame[0])).normalize_safe();
        frame[2].crossproduct(frame[0],frame[1]);
        Fmatrix held; held.identity();
        Fvector* axes[3]={&held.i,&held.j,&held.k};
        for (int a=0;a<3;++a)
        {
            axes[a]->set(0,0,0);
            for (int b=0;b<3;++b) axes[a]->mad(frame[b],m_maghand_basis[b][a]);
        }
        Fvector palm; held.transform_dir(palm,m_maghand_palm);
        held.c.sub(grip.c,palm);
        Fmatrix to_hand; to_hand.invert(m_maghand_in_root);
        root.mul_43(held,to_hand);
        where_grabbed();
        // The animation's hold is only used if the magazine is really in the
        // fist: its nearest surface within 7 cm of the hand's grip centre.
        // Some reloads carry a spare or swap parts (an AKS-74U's), and the
        // sampled hold then left the magazine floating off the hand.
        if (!m_contact || m_contact->magazine.samples.empty()) return true;
        float nearest=flt_max;
        for (const vr::V3& sample:m_contact->magazine.samples)
            nearest=std::min(nearest,Point(root,F(sample)).distance_to_sqr(grip.c));
        if (nearest<=0.035f*0.035f) return true;
    }
    const Fvector& axis=m_profile.well_axis;
    Fvector front; front.set(0.f,0.f,1.f);
    front.mad(axis,-front.dotproduct(axis)).normalize_safe();
    Fvector side; side.crossproduct(axis,front);
    Fvector up=aim.j, forward=aim.k;
    Fvector across; across.crossproduct(up,forward);
    root.identity();
    Fvector* rows[3]={&root.i,&root.j,&root.k};
    for (int row=0;row<3;++row)
    {
        rows[row]->set(0,0,0);
        rows[row]->mad(across,side[row]).mad(up,axis[row]).mad(forward,front[row]);
    }
    // Held by its body: the middle of its lowest 40% (a curved AK magazine's
    // base point, carried down its axis, lies off the magazine).
    Fvector body=m_profile.mag_base;
    if (m_contact && !m_contact->magazine.samples.empty())
    {
        float low=flt_max, high=-flt_max;
        for (const vr::V3& s:m_contact->magazine.samples)
        {
            const float along=F(s).dotproduct(axis);
            low=std::min(low,along); high=std::max(high,along);
        }
        Fvector sum; sum.set(0,0,0); unsigned used=0;
        for (const vr::V3& s:m_contact->magazine.samples)
            if (F(s).dotproduct(axis)<=low+0.4f*(high-low)) { sum.add(F(s)); ++used; }
        if (used) body=sum.div(float(used));
    }
    Fvector hold; root.transform_dir(hold,body);
    root.c.sub(grip.c,hold);
    // Held a little behind the handle centre, where the fingers close on it.
    root.c.mad(forward,-kMagazineHoldBack);
    where_grabbed();
    return true;
}
void CVrWeaponInteraction::MagazineScene(vr::ContactScene& scene) const
{
    scene.Clear();
    if (!m_contact) return;
    scene.Add(&m_contact->receiver,vr::Frame());
    // The slide, bolt or pump where it is now (it moves back along -z).
    const bool pump=!m_profile.cycle_bone.size() && m_profile.pump_bone.size();
    vr::Frame moved;
    moved.c=vr::V3(0.f,0.f,-(pump ? m_pump : std::max(m_mechanism.slide,m_cycle_visual_slide)));
    scene.Add(&m_contact->cycle,moved);
}
bool CVrWeaponInteraction::HeldMagazineKnown() const
{
    Fmatrix pose;
    return (m_guide.active && m_contact) || MagazineHandPose(HandOf(0),pose);
}
Fmatrix CVrWeaponInteraction::SolvedMagazine(const Fmatrix& world) const
{
    Fmatrix pose;
    if (m_guide.active && m_contact) pose.mul_43(world,ToMatrix(m_guide.Pose(m_contact->path)));
    else if (!MagazineHandPose(HandOf(0),pose)) pose=world;
    return pose;
}
void CVrWeaponInteraction::SeatMagazine(const Fvector& sound_at)
{
    m_guide.active=false;
    m_inserting=false;
    if (m_mechanism.held<0) return;
    // A released magazine still in the well falls clear first.
    if (m_ejecting>=0)
    {
        const int earlier=m_ejecting;
        m_ejecting=-1;
        Fmatrix sliding=m_eject_world;
        if (m_contact && m_contact->path.Valid()) sliding.mul_43(m_eject_world,ToMatrix(m_contact->path.At(1.f)));
        DropFromWell(earlier,sliding,m_hand_velocity[1]);
    }
    // One that never came half out goes back as it was: a tap on an empty
    // magazine is not a reload.
    if (m_guide.armed) FillMagazine(m_mechanism.held);
    if (!m_mechanism.magazines[m_mechanism.held].empty())
        m_weapon.m_ammoType=m_mechanism.magazines[m_mechanism.held].back().m_LocalAmmoType;
    if (m_mechanism.Insert(m_weapon.m_magazine))
    {
        m_pouch_preview=false;
        PlayHandling(SoundMagIn,sound_at);
        vr::HandHaptic(HandOf(0),0.25f);
        vr::HandHaptic(HandOf(1),0.15f);
    }
}
void VrContactTiming(int what, float microseconds, bool cached)
{
    struct Totals { double sum=0.0; float max=0.f; unsigned count=0, cached=0; };
    static Totals totals[2];
    static u32 since=0;
    Totals& t=totals[what ? 1 : 0];
    t.sum+=microseconds; t.max=std::max(t.max,microseconds); ++t.count; t.cached+=cached;
    if (!since) since=Device.dwFrame;
    if (Device.dwFrame-since<120) return;
    Msg("* [VR contact] over %u frames: magazine %u solves avg %.1f max %.1f us; fingers %u solves "
        "avg %.1f max %.1f us, %u%% cached",Device.dwFrame-since,
        totals[0].count,totals[0].count ? float(totals[0].sum/totals[0].count) : 0.f,totals[0].max,
        totals[1].count,totals[1].count ? float(totals[1].sum/totals[1].count) : 0.f,totals[1].max,
        totals[1].count ? totals[1].cached*100/totals[1].count : 0u);
    totals[0]=totals[1]=Totals();
    since=Device.dwFrame;
}
bool CVrWeaponInteraction::SupportHandOverride(Fmatrix& grip) const
{
    Fmatrix world, pose;
    for (int i=0;i<m_profile.part_count;++i)
        if (m_parts[i].hand==HandOf(0) && Transform(world) && vr::HandTransform(HandOf(0),false,grip))
        {
            // The hand stays on the handle, which moves on its own path; the
            // wrist keeps the tracked hand's turn, so taking it never snaps.
            const vr::PartVec a=vr::PartAnchor(m_profile.parts[i],m_parts[i]);
            grip.c=Point(world,Fvector().set(a.x,a.y,a.z));
            return true;
        }
    if (m_racking && Transform(world) && vr::HandTransform(HandOf(0),false,grip))
    {
        // The hand stays on the slide or charging handle, moving with it; on
        // a bolt-action's knob, along its path (up through the turn, back).
        grip.c=Point(world,CycleGrabLocal());
        grip.c.mad(world.k,-CycleGrabTravel());
        return true;
    }
    if (m_supporting && AimTransform(world))
    {
        // The support hand stays on the weapon: around the grip below the
        // weapon hand (cup), or under the bore at the nearest point of the
        // barrel line (fore-end). Frames are controller-equivalent: +K up
        // the handle, -J toward the knuckles.
        Fmatrix hand;
        if (!vr::HandTransform(HandOf(1),false,hand)) return false;
        Fvector muzzle=Mode() ? Point(world,m_profile.muzzle) : Point(world,m_weapon.vLoadedFirePoint);
        Fvector axis; axis.sub(muzzle,hand.c);
        float length=axis.magnitude();
        if (length<0.15f) { axis=world.k; length=0.6f; }
        else axis.div(length);
        grip.identity();
        if (m_support_cup)
        {
            grip.k=world.j;
            grip.j.invert(world.k);
            grip.c=hand.c;
            grip.c.mad(world.j,-0.03f);
        }
        else
        {
            Fmatrix support;
            if (!vr::HandTransform(HandOf(0),false,support)) return false;
            // Where the hand closed on the barrel line; it stays there, the
            // palm cannot slide along the weapon while held.
            float along=RearSupport() ? clampr(m_support_along,-0.5f,-0.08f) : clampr(m_support_along,0.12f,length);
            // Riding the fore-end while it is pumped.
            if (m_pumping) along-=m_pump;
            grip.k=axis;
            grip.j.invert(world.j);
            grip.c=hand.c;
            grip.c.mad(axis,along).mad(world.j,-0.035f);
        }
        grip.i.crossproduct(grip.j,grip.k);
        if (m_support_barrels && !m_support_cup && BarrelsOpen())
        {
            // The fore-end hand rides the hinged barrels.
            Fmatrix moved, barrels;
            HingeWorld(world,moved);
            barrels.mul_43(moved,grip);
            grip=barrels;
        }
        return true;
    }
    // A hand holding a magazine is the controller itself (the magazine
    // follows it exactly), reported so its fingers close on the magazine;
    // while the well has the magazine the hand rides it on the well's line.
    if (!m_profile.detachable || m_mechanism.held<0 || !vr::HandTransform(HandOf(0),false,grip)) return false;
    Fmatrix target;
    if (m_guide.active && Transform(world) && MagazineHandPose(HandOf(0),target))
    {
        Fmatrix correction, controller=grip;
        correction.mul_43(SolvedMagazine(world),Fmatrix().invert(target));
        grip.mul_43(correction,controller);
    }
    return true;
}

CPhysicsShell* CVrWeaponInteraction::CreateShell(int index, const Fmatrix& pose, int source_hand)
{
    if (index<0 || index>1) return nullptr;
    CPhysicsShell* shell=P_create_Shell();
    CPhysicsElement* element=P_create_Element();
    Fobb box; box.m_rotate.identity();
    box.m_translate=m_profile.mag_box_center;
    box.m_halfsize=m_profile.mag_box_half;
    element->add_Box(box); shell->add_Element(element);
    shell->setMass(0.08f+0.01f*float(m_mechanism.magazines[index].size()));
    shell->set_PhysicsRefObject(nullptr);
    source_hand=clampr(source_hand,0,1);
    shell->Activate(pose,m_hand_velocity[source_hand],m_hand_angular[source_hand]);
    shell->SetAirResistance(0.002f,0.01f);
    shell->set_DynamicLimits(5.f,20.f);
    shell->SetSmall(); shell->EnableCollision();
    return shell;
}
void CVrWeaponInteraction::DropInserted(const Fmatrix& world)
{
    // The catch releases: the magazine leaves the weapon at once, then
    // slides out along its way into the well (StepEject) and falls when
    // clear. Its rounds leave the inventory; the chamber round stays.
    if (m_ejecting>=0)
    {
        // A second release before the first cleared: that one falls now.
        const int earlier=m_ejecting;
        m_ejecting=-1;
        Fmatrix sliding=m_eject_world;
        if (m_contact && m_contact->path.Valid()) sliding.mul_43(m_eject_world,ToMatrix(m_contact->path.At(m_eject_s)));
        DropFromWell(earlier,sliding,m_hand_velocity[1]);
    }
    const int index=m_mechanism.EjectInserted(m_weapon.m_magazine);
    if (index<0) return;
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    PlayHandling(SoundMagOut,Point(world,m_profile.mag_base));
    vr::HandHaptic(HandOf(1),0.2f);
    if (m_contact && m_contact->path.Valid())
    {
        m_ejecting=index;
        m_eject_s=0.f;
        m_eject_speed=0.4f;
        m_eject_world=world;
        ReleaseModel(m_eject_visual);
        m_eject_visual=::Render->model_Create(MagazineVisualName(unsigned(m_mechanism.magazines[index].size())).c_str());
        return;
    }
    // No measured way out: it falls from its seat.
    Fvector out; out.invert(m_profile.well_axis);
    world.transform_dir(out);
    DropFromWell(index,world,Fvector().mad(m_hand_velocity[1],out.normalize_safe(),1.5f));
}

// How much of the way out a released magazine slides before it falls.
constexpr float eject_share=0.1f;
void CVrWeaponInteraction::StepEject()
{
    if (m_ejecting<0) return;
    const int index=m_ejecting;
    Fmatrix world;
    if (!m_contact || !m_contact->path.Valid() || !Transform(world))
    {
        // Put away or switched mid-slide: it falls where it was.
        m_ejecting=-1;
        Fmatrix pose=m_eject_world;
        if (m_contact && m_contact->path.Valid()) pose.mul_43(m_eject_world,ToMatrix(m_contact->path.At(m_eject_s)));
        DropFromWell(index,pose,Fvector().set(0.f,0.f,0.f));
        return;
    }
    m_eject_world=world;
    // Its spring pushes it out and gravity pulls it along the well; it
    // never stalls in it.
    const vr::InsertPath& path=m_contact->path;
    // Straight out of the well: the reload's path can curve away further on.
    Fvector out=F(path.Tangent(0.f));
    world.transform_dir(out);
    out.normalize_safe();
    const float dt=clampr(Device.fTimeDelta,0.f,0.05f);
    m_eject_speed=std::max(0.4f,m_eject_speed+(2.f-out.y*9.81f)*dt);
    m_eject_s+=m_eject_speed*dt/path.length;
    if (m_eject_s<eject_share) return;
    m_ejecting=-1;
    Fmatrix pose; pose.mul_43(world,ToMatrix(path.At(eject_share)));
    DropFromWell(index,pose,Fvector().mad(m_hand_velocity[1],out,m_eject_speed));
}

void CVrWeaponInteraction::DropFromWell(int index, const Fmatrix& pose, const Fvector& velocity)
{
    ReleaseModel(m_eject_visual);
    if (index<0 || index>1) return;
    CPhysicsShell* shell=CreateShell(index,pose,HandOf(1));
    if (shell)
    {
        shell->set_LinearVel(velocity);
        shell->set_AngularVel(Fvector().set(0.f,0.f,0.f));
        if (m_props.size()>=8) { RetireProp(m_props.front()); m_props.pop_front(); }
        DropProp prop;
        prop.shell=shell;
        prop.proxy=m_basic_magazine;
        prop.round_count=unsigned(m_mechanism.magazines[index].size());
        prop.rounds=prop.round_count>0;
        prop.expires=Device.dwTimeGlobal+20000;
        prop.cartridges.assign(m_mechanism.magazines[index].begin(),m_mechanism.magazines[index].end());
        m_props.push_back(prop);
    }
    else ReturnCartridges(m_mechanism.magazines[index]);
    Msg("* [VR weapon] %s: magazine dropped from the well; %u rounds go back to reserve when it expires",
        m_profile.section.c_str(),unsigned(m_mechanism.magazines[index].size()));
    m_mechanism.magazines[index].clear();
    // The emptied magazine is the next one refilled from the pouch.
    m_mechanism.next_pouch=index;
}

void CVrWeaponInteraction::FinishDrop(int index, CPhysicsShell* shell)
{
    if (index<0 || index>1 || !shell)
    {
        if (shell) destroy_physics_shell(shell);
        m_mechanism.Stow();
        return;
    }
    if (m_props.size()>=8) { RetireProp(m_props.front()); m_props.pop_front(); }
    DropProp prop;
    prop.shell=shell;
    prop.proxy=m_basic_magazine;
    prop.round_count=unsigned(m_mechanism.magazines[index].size());
    prop.rounds=prop.round_count>0;
    prop.expires=Device.dwTimeGlobal+20000;
    prop.cartridges.assign(m_mechanism.magazines[index].begin(),m_mechanism.magazines[index].end());
    m_props.push_back(prop);
    const unsigned discarded=unsigned(m_mechanism.magazines[index].size());
    m_mechanism.DiscardHeld();
    m_inserting=false; m_pouch_preview=false; m_guide.active=false;
    Msg("* [VR weapon] %s: magazine dropped as temporary prop; %u rounds go back to reserve when it expires",
        m_profile.section.c_str(),discarded);
}
void CVrWeaponInteraction::DropMagazine(int source_hand)
{
    const int index=m_mechanism.held;
    if (index<0 || index>1) return;
    Fmatrix root, world;
    if (!HeldMagazineKnown()) { m_mechanism.Stow(); return; }
    if (Transform(world)) root=SolvedMagazine(world);
    else if (!MagazineHandPose(source_hand,root)) { m_mechanism.Stow(); return; }
    CPhysicsShell* shell=CreateShell(index,root,source_hand);
    if (!shell) { m_mechanism.Stow(); return; }
    shell->SetTransform(root,mh_not_clear);
    shell->set_LinearVel(m_hand_velocity[source_hand]);
    shell->set_AngularVel(m_hand_angular[source_hand]);
    FinishDrop(index,shell);
}
void CVrWeaponInteraction::RenderLooseMagazines()
{
    if (!g_vr_owns_present) return;
    for (auto& prop:m_props)
    {
        if (!prop.shell) continue;
        if (prop.proxy) continue; // rendered by the VR eye overlay
        if (!prop.visual)
        {
            const xr_string name=prop.part.size() ? xr_string(m_profile.visual.c_str()) :
                MagazineVisualName(prop.round_count);
            prop.visual=::Render->model_Create(name.c_str());
        }
        if (!prop.visual) continue;
        auto* model=prop.visual->dcast_PKinematics();
        if (!model) continue;
        Fvector centre=m_profile.mag_box_center, half=m_profile.mag_box_half;
        if (prop.part.size())
        {
            // A loose shell: the weapon's own HUD mesh showing only that part.
            model->Callback(CollapseHiddenBones,nullptr);
            vr::ApplyWeaponPose(*model,prop.poses,BonePose,bctCustom,nullptr,0.f,nullptr,
                {prop.part.c_str()},true,true,true);
            const int chamber=prop.part==m_profile.shell_bones[0] ? 0 : 1;
            centre=m_profile.shell_center[chamber];
            half=m_profile.shell_box_half;
        }
        else Pose(*model,prop.poses,true,prop.rounds);
        auto& vis=prop.visual->getVisData();
        vis.box.min.sub(centre,half);
        vis.box.max.add(centre,half);
        vis.box.getsphere(vis.sphere.P,vis.sphere.R);
        Fmatrix world; prop.shell->InterpolateGlobalTransform(&world);
        ::Render->set_Transform(&world);
        ::Render->add_Visual(prop.visual);
    }
}
void CVrWeaponInteraction::RenderBasicMagazines()
{
    if (!g_vr_owns_present) return;
    Fmatrix world;
    if (vr::ContactDebug()>=2 && Mode() && Transform(world))
    {
        // Where the hand takes the bolt or handle (white star), and the
        // magazine's surface it takes the seated magazine by (cyan dots).
        const Fvector grab=Point(world,m_profile.cycle_grab);
        const Fvector axes[3]={world.i,world.j,world.k};
        for (const Fvector& a:axes)
            vr::OverlayDraw().DrawLine(Fvector().mad(grab,a,-0.015f),Fvector().mad(grab,a,0.015f),0xffffffff);
        if (m_contact && m_mechanism.inserted>=0)
            for (const vr::V3& p:m_contact->grab_samples)
            {
                const Fvector at=Point(world,F(p));
                vr::OverlayDraw().DrawLine(at,Fvector().mad(at,world.j,0.002f),0xff40ffff);
            }
    }
    if (vr::ContactDebug()>=2 && m_contact && m_contact->path.Valid() && Mode() && Transform(world))
    {
        // The magazine's way in (its feed lips' path) and the insertion
        // zone's ring at the well's mouth: blue, amber while the held
        // magazine is near it, green while it is in and lined up.
        const vr::InsertPath& path=m_contact->path;
        vr::InsertState state=m_guide.active ? vr::InsertTaken : vr::InsertOutside;
        if (m_mechanism.held>=0 && !m_guide.active)
        {
            Fmatrix held=SolvedMagazine(world);
            state=vr::InsertCheck(path,ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),held)),vr::InsertZone());
        }
        const u32 colour=state==vr::InsertTaken ? 0xff40ff40 : state==vr::InsertNear ? 0xffffb000 : 0xff40a0ff;
        Fvector previous=Point(world,F(path.poses[0].Apply(path.lead)));
        for (size_t n=1;n<path.poses.size();++n)
        {
            const Fvector next=Point(world,F(path.poses[n].Apply(path.lead)));
            vr::OverlayDraw().DrawLine(previous,next,colour);
            previous=next;
        }
        const float at=std::min(1.f,std::max(0.f,path.mouth));
        const vr::V3 tangent=path.Tangent(at);
        vr::V3 across=vr::Normalized(vr::Cross(tangent,std::abs(tangent.y)<0.9f ? vr::V3(0,1,0) : vr::V3(1,0,0)));
        const vr::V3 up=vr::Cross(tangent,across), mouth=path.At(at).Apply(path.lead);
        const float radius=vr::InsertZone().radius;
        for (int n=0;n<24;++n)
        {
            const float a=PI_MUL_2*n/24.f, b=PI_MUL_2*(n+1)/24.f;
            const vr::V3 p=mouth+(across*_cos(a)+up*_sin(a))*radius;
            const vr::V3 q=mouth+(across*_cos(b)+up*_sin(b))*radius;
            vr::OverlayDraw().DrawLine(Point(world,F(p)),Point(world,F(q)),colour);
        }
    }
    for (auto& prop:m_props)
    {
        if (!prop.proxy || !prop.shell) continue;
        Fmatrix world; prop.shell->InterpolateGlobalTransform(&world);
        Fvector corners[8];
        for (unsigned i=0;i<8;++i)
        {
            corners[i].set((i&1 ? 1.f:-1.f)*m_profile.mag_box_half.x,
                (i&2 ? 1.f:-1.f)*m_profile.mag_box_half.y,
                (i&4 ? 1.f:-1.f)*m_profile.mag_box_half.z);
            world.transform_tiny(corners[i]);
        }
        for (unsigned i=0;i<8;++i)
            for (unsigned axis=1;axis<=4;axis<<=1)
                if (!(i&axis)) vr::OverlayDraw().DrawLine(corners[i],corners[i|axis],0xffd0c0a0);
    }
}
void CVrWeaponInteraction::UpdateProps()
{
    StepEject();
    m_handle_hooked=m_handle_hooked && m_mechanism.locked;
    // A handle pulled on a locked bolt stays where it is: the slight pull
    // releases the catch.
    if ((m_racking && !m_mechanism.locked) || m_handle_hooked) m_handle_slide=m_mechanism.slide;
    else m_handle_slide=std::max(0.f,m_handle_slide-m_profile.cycle_travel*Device.fTimeDelta/0.08f);
    if (m_cycle_visual_slide>0.f && m_profile.cycle_travel>0.f)
        m_cycle_visual_slide=std::max(0.f,m_cycle_visual_slide-
            m_profile.cycle_travel*Device.fTimeDelta/0.11f);
    for (auto it=m_props.begin();it!=m_props.end();)
    {
        if (int(Device.dwTimeGlobal-it->expires)<0) { ++it; continue; }
        RetireProp(*it);
        it=m_props.erase(it);
    }
    if (Mode() && vr::GameplayActive() && m_mechanism.initialized)
        m_mechanism.Advance(Device.fTimeDelta,m_weapon.m_magazine,[&]()
        {
            Fvector velocity; m_weapon.PHGetLinearVell(velocity);
            m_weapon.OnShellDrop(m_weapon.get_LastSP(),velocity);
        });
    // Each lock and release of the slide, with what was there, in the log.
    if (m_mechanism.initialized && m_mechanism.locked!=m_slide_lock_logged)
    {
        m_slide_lock_logged=m_mechanism.locked;
        Msg("* [VR weapon] %s: slide %s (%u rounds in the gun, magazine %s, stop %d)",
            m_weapon.cNameSect().c_str(),m_mechanism.locked ? "locked open" : "released",
            unsigned(m_weapon.m_magazine.size()),m_mechanism.inserted>=0 ? "in" : "out",
            int(m_profile.slide_stop));
    }
}
bool CVrWeaponInteraction::LeftHandBusy() const
{
    return m_mechanism.held>=0 || m_racking ||
        m_supporting || m_shell_held || m_pumping;
}
float CVrWeaponInteraction::DetachedRoundWeight() const
{
    float weight=0;
    for (const auto& magazine:m_mechanism.magazines)
        for (const auto& round:magazine) weight+=round.Weight();
    return weight;
}
void CVrWeaponInteraction::ReleaseInactiveHeld()
{
    if (!Mode()) return;
    auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
    if (!actor || actor->inventory().ActiveItem()==&m_weapon) return;
    if (m_holstered && m_mechanism.held<0 && !m_racking &&
        !m_inserting && !m_supporting && !m_shell_held && !m_break_open) return;
    // Switching weapons ends every hand constraint. Keep detached cartridges
    // in their pouch and return a pulled slide from its current position
    // (a manual action stays where it is, as when let go).
    m_weapon.FireEnd();
    if (m_racking && !m_profile.manual_cycle) m_mechanism.ReleaseManualSlide(m_weapon.m_magazine);
    m_mechanism.Stow(true);
    m_holstered=true;
    m_inserting=m_racking=m_supporting=false;
    m_guide.active=false;
    m_pouch_preview=false;
    // A holstered break-action is carried closed; an unloaded shell goes back.
    m_break_open=m_lever_armed=m_shell_held=m_break_hand=false;
    m_break_angle=0.f;
    m_pumping=m_pump_back=false;
    m_pump=0.f;
    m_input.Reset();
    m_trigger=m_grip[0]=m_grip[1]=m_button=m_secondary=m_mag_trigger=false;
    m_release_pending=false;
    m_hand_epoch=0; m_have_hand[0]=m_have_hand[1]=false;
}

bool CVrWeaponInteraction::CanFire() const
{
    return !Mode() || (m_input.Armed() && !m_holstered && !m_racking &&
        vr::GameplayActive() && (!m_profile.chamber || m_mechanism.CanFire()) &&
        (!m_profile.break_action || (!m_break_open && m_break_angle<0.01f)) &&
        (!m_profile.pump_bone.size() || m_profile.cycle_bone.size() || (m_pump_ready && m_pump<0.01f)) &&
        // A manual action fires only closed and turned down.
        (!m_profile.manual_cycle || !m_profile.action_path.Valid() || m_action_u<0.02f) &&
        vr::GatesPass(m_profile.rules.fire,Facts()));
}
bool CVrWeaponInteraction::BlockReload() const
{
    return Mode() && m_profile.detachable && m_mechanism.initialized && !m_filling;
}
void CVrWeaponInteraction::CartridgeFired()
{
    // Break-actions do not cycle; fired cases stay until the gun is opened.
    if (m_profile.break_action)
    {
        m_break_state=unsigned(m_weapon.m_magazine.size());
        m_break_state_set=true;
        return;
    }
    // A pump gun needs a manual stroke to eject the case and chamber again.
    if (m_profile.tube && m_profile.pump_bone.size() && !m_profile.cycle_bone.size())
    {
        m_pump_ready=false;
        m_pump_spent=true;
        m_shell_rounds=unsigned(m_weapon.m_magazine.size());
        return;
    }
    // The first movement of the bolt swings a dust cover open.
    m_dust_open=true;
    // A bolt-action keeps its fired case until the hand works the bolt.
    if (m_profile.manual_cycle && m_mechanism.initialized)
    {
        m_mechanism.AfterShotManual();
        return;
    }
    if (DeferShellDrop() && m_mechanism.initialized)
    {
        m_mechanism.AfterShot(m_weapon.m_magazine);
        // Keep the shot's rearward slide travel visible for several headset
        // frames even when a long simulation step completes the native cycle.
        m_cycle_visual_slide=m_profile.bolt_travel>0.f ? m_profile.bolt_travel : m_profile.cycle_travel;
    }
}
bool CVrWeaponInteraction::ReserveAvailable() const
{
    if (m_weapon.unlimited_ammo()) return true;
    if (!m_weapon.m_pInventory) return false;
    for (auto* item:m_weapon.m_pInventory->m_all)
        if (auto* box=smart_cast<CWeaponAmmo*>(item); box && box->m_boxCurr)
            if (std::find(m_weapon.m_ammoTypes.begin(),m_weapon.m_ammoTypes.end(),box->cNameSect())!=m_weapon.m_ammoTypes.end())
                return true;
    return false;
}
void CVrWeaponInteraction::FillMagazine(int index)
{
    if (index<0 || index>1 || !m_mechanism.magazines[index].empty()) return;
    auto* weapon=m_weapon.cast_weapon_magazined();
    if (!weapon) return;
    xr_vector<CCartridge> live;
    live.swap(m_weapon.m_magazine);
    m_weapon.iAmmoElapsed=0;
    const u8 ammo_type=m_weapon.m_ammoType, next_type=m_weapon.m_set_next_ammoType_on_reload;
    const bool misfire=m_weapon.bMisfire, clear_jam=m_weapon.bClearJamOnly;
    const bool need_reload=weapon->m_needReload;
    m_weapon.m_set_next_ammoType_on_reload=CWeapon::undefined_ammo_type;
    // Filled with the rounds the pouch gives (SwapPouchAmmo); native loading
    // falls back on any other type carried when there are none of them.
    m_weapon.m_ammoType=PouchAmmo();
    // Borrow native cartridge loading without treating this magazine fill as
    // the player's jam-clearing reload. Cycling the action clears that state.
    m_weapon.bMisfire=m_weapon.bClearJamOnly=false;
    m_filling=true;
    weapon->ReloadMagazine();
    m_filling=false;
    m_mechanism.magazines[index].swap(m_weapon.m_magazine);
    live.swap(m_weapon.m_magazine);
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    m_weapon.m_ammoType=ammo_type;
    m_weapon.m_set_next_ammoType_on_reload=next_type;
    m_weapon.bMisfire=misfire;
    m_weapon.bClearJamOnly=clear_jam;
    weapon->m_needReload=need_reload;
}

void CVrWeaponInteraction::ReturnCartridges(const xr_vector<CCartridge>& rounds)
{
    // The owner's inventory: a weapon being dropped has already left it.
    auto* owner=smart_cast<CInventoryOwner*>(m_weapon.H_Parent());
    xr_vector<std::pair<shared_str,u32>> left;
    for (const CCartridge& cartridge:rounds)
    {
        bool placed=false;
        if (owner)
            for (auto* item:owner->inventory().m_all)
                if (auto* box=smart_cast<CWeaponAmmo*>(item);
                    box && box->cNameSect()==cartridge.m_ammoSect && box->m_boxCurr<box->m_boxSize)
                { ++box->m_boxCurr; placed=true; break; }
        if (placed) continue;
        auto it=std::find_if(left.begin(),left.end(),[&](const auto& entry) { return entry.first==cartridge.m_ammoSect; });
        if (it!=left.end()) ++it->second;
        else left.push_back({cartridge.m_ammoSect,1});
    }
    // One spawn per ammunition type: it splits by box size. With no owner
    // they land where the weapon is.
    const Fvector position=m_weapon.Position();
    for (const auto& entry:left)
        if (owner) m_weapon.SpawnAmmo(entry.second,entry.first.c_str());
        else m_weapon.SpawnAmmo(entry.second,entry.first.c_str(),0xffff,&position);
}
void CVrWeaponInteraction::Normalize()
{
    if (!m_mechanism.initialized) return;
    m_restore_empty_chamber=false;
    // Save and desktop weapon formats contain ordinary cartridges and ammo
    // boxes only. Prefer filling the weapon; return any excess to ammo boxes.
    // Spare magazines fill the weapon; one ammo type; and no more than the
    // desktop magazine holds (a chambered round on a full magazine is one
    // over): everything else goes back to ammo boxes (VrSaveLedger.h).
    xr_vector<CCartridge> back;
    const unsigned returned=vr::NormalizeRounds(m_weapon.m_magazine,m_mechanism.magazines,2,
        m_weapon.iMagazineSize>0 ? size_t(m_weapon.iMagazineSize) : size_t(-1),
        [](const CCartridge& a, const CCartridge& b) { return a.m_ammoSect==b.m_ammoSect; },
        [&](const CCartridge& c) { back.push_back(c); });
    ReturnCartridges(back);
    if (!m_weapon.m_magazine.empty()) m_weapon.m_ammoType=m_weapon.m_magazine.back().m_LocalAmmoType;
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    m_parts_ready=false;
    if (returned) Msg("* [VR weapon] %s: %u spare rounds normalized",m_profile.section.c_str(),returned);
    ReleaseProps(); ReleaseModels();
    m_mechanism={};
    m_action_u=0.f;
    m_handle_hooked=false;
    m_mechanism.travel_limit=m_profile.cycle_travel>0 ? m_profile.cycle_travel :
        vr::MagazineMechanism<xr_vector<CCartridge>>::travel;
    m_mechanism.locks_on_empty=m_profile.slide_stop;
    m_cycle_visual_slide=0.f;
    m_input.Reset();
    m_trigger=m_grip[0]=m_grip[1]=m_button=m_secondary=m_mag_trigger=false;
    m_release_pending=false;
    m_racking=m_racked=m_inserting=m_supporting=false;
    m_holstered=false; m_have_hand[0]=m_have_hand[1]=false;
}
void CVrWeaponInteraction::PrepareSave()
{
    Normalize();
    m_restore_empty_chamber=false;
}
void CVrWeaponInteraction::PrepareSnapshot()
{
    if (!m_mechanism.initialized) return;
    const bool empty_chamber=!m_mechanism.chamber;
    Normalize();
    // The save contains only ordinary ammo state. A snapshot must not chamber
    // a round in the continuing VR session when the player had not racked it.
    m_restore_empty_chamber=empty_chamber && Mode();
}

void CVrWeaponInteraction::InteractShells(CActor& actor, const Fmatrix& world,
    bool left_valid, const Fmatrix& left, bool mag_hold, bool left_grip, bool left_trigger, bool secondary)
{
    if (m_profile.break_action)
    {
        SyncChambers();
        // B releases the top lever; a downward wrist flick within 0.6 s
        // breaks the barrels open. An upward flick snaps them shut.
        if (secondary && !m_secondary && !m_break_open)
        {
            m_lever_armed=true;
            m_lever_time=Device.dwTimeGlobal;
            vr::HandHaptic(HandOf(1),0.08f);
        }
        if (m_lever_armed && Device.dwTimeGlobal-m_lever_time>600) m_lever_armed=false;
        // A support hand holding the barrels carries them once the lever is
        // released or they are open: pulling it down breaks them open,
        // lifting it closes them. Where it holds is judged when it closes
        // (the palm cannot slide), so it keeps them until it lets go.
        const bool forend=m_supporting && !m_support_cup && m_support_barrels && left_valid && left_grip;
        if (forend && !m_break_hand && (m_lever_armed || m_break_open))
        {
            Fmatrix aim;
            if (vr::HandTransform(HandOf(1),true,aim))
            {
                aim.c.set(0,0,0);
                Fmatrix receiver=world; receiver.c.set(0,0,0);
                Fmatrix inverse; inverse.invert(aim);
                m_break_receiver.mul_43(inverse,receiver);
                m_break_hand=true;
                m_break_hand_ref=HingeAngle(world,left.c)-m_break_angle;
            }
        }
        if (m_break_hand && !forend) m_break_hand=false;
        const bool settled=Device.dwTimeGlobal-m_break_time>500;
        if (m_break_hand)
        {
            float angle=clampr(HingeAngle(world,left.c)-m_break_hand_ref,0.f,m_profile.hinge_angle);
            if (!m_break_open && angle<0.03f) angle=0.f;
            // The lever stays released while the hand is pulling the barrels.
            if (angle>0.f) m_lever_time=Device.dwTimeGlobal;
            m_break_angle=angle;
            if (!m_break_open && angle>m_profile.hinge_angle*0.15f)
            {
                m_break_open=true; m_lever_armed=false;
                m_break_time=Device.dwTimeGlobal;
                m_weapon.FireEnd();
                PlayHandling(SoundBreakOpen,Point(world,m_profile.hinge_point));
                vr::HandHaptic(HandOf(0),0.25f);
            }
            else if (m_break_open && angle<0.03f && settled)
            {
                // Lifted shut: the lever catches.
                m_break_open=false;
                m_break_angle=0.f;
                m_break_hand=false;
                m_break_state=unsigned(m_weapon.m_magazine.size());
                m_break_state_set=true;
                m_break_time=Device.dwTimeGlobal;
                PlayHandling(SoundBreakClose,Point(world,m_profile.hinge_point));
                vr::HandHaptic(HandOf(0),0.3f);
                vr::HandHaptic(HandOf(1),0.2f);
            }
            else if (!m_break_open && !m_lever_armed) m_break_hand=false;
        }
        // The wrist's rebound after a flick turns the other way; the barrels
        // ignore flicks for 0.5 s after moving, and close only once fully open.
        // Flicks only work while no hand carries the barrels.
        const float pitch=m_hand_angular[1].dotproduct(world.i);
        if (m_break_hand) {}
        else if (!m_break_open && m_lever_armed && settled && pitch>5.f)
        {
            m_break_open=true; m_lever_armed=false;
            m_break_time=Device.dwTimeGlobal;
            m_weapon.FireEnd();
            PlayHandling(SoundBreakOpen,Point(world,m_profile.hinge_point));
            vr::HandHaptic(HandOf(1),0.25f);
        }
        else if (m_break_open && settled && pitch< -6.f && m_break_angle>=m_profile.hinge_angle*0.95f)
        {
            m_break_open=false;
            m_break_state=unsigned(m_weapon.m_magazine.size());
            m_break_state_set=true;
            m_break_time=Device.dwTimeGlobal;
            PlayHandling(SoundBreakClose,Point(world,m_profile.hinge_point));
            vr::HandHaptic(HandOf(1),0.3f);
        }
        if (!m_break_hand)
        {
            const float target=m_break_open ? m_profile.hinge_angle : 0.f;
            const float step=Device.fTimeDelta*(m_break_open ? 7.f : 12.f);
            m_break_angle+=clampr(target-m_break_angle,-step,step);
        }
        // Fully open: the ejectors throw fired cases clear; live shells stay.
        if (m_break_open && m_break_angle>=m_profile.hinge_angle*0.9f)
            for (int c=0;c<2;++c)
                if (m_chambers[c]==2) { EjectShell(c,world); m_chambers[c]=0; }
    }
    else if (m_profile.pump_bone.size() && !m_profile.cycle_bone.size())
    {
        // A native reload refilled the tube and cycled the action.
        if (m_weapon.m_magazine.size()>m_shell_rounds) m_pump_ready=true;
        if (m_weapon.m_magazine.empty()) m_pump_ready=false;
        // Pump: the left hand closes on the fore-end, pulls it back the full
        // stroke (ejecting a fired case) and pushes it home (chambering).
        Fmatrix inverse; inverse.invert(world);
        Fvector local; inverse.transform_tiny(local,left.c);
        if (left_valid && left_grip && !m_pumping && Near(left.c,Point(world,m_profile.pump_grab),0.12f))
        {
            m_pumping=m_supporting=true;
            m_support_cup=false;
            m_pump_start=local.z+m_pump;
            Fmatrix weapon_hand;
            if (vr::HandTransform(HandOf(1),false,weapon_hand))
                m_support_along=SupportAlong(weapon_hand.c,Point(world,m_profile.muzzle),left.c)+m_pump;
        }
        if (m_pumping && left_valid && left_grip)
        {
            m_pump=clampr(m_pump_start-local.z,0.f,m_profile.pump_travel);
            if (vr::ContactDebug()>0 && Device.dwTimeGlobal-m_debug_time>250)
            {
                m_debug_time=Device.dwTimeGlobal;
                Msg("* [VR pump] %s: pump %.0f mm, hand on the barrel line at %.0f mm, supporting %d cup %d",
                    m_weapon.cNameSect().c_str(),m_pump*1000.f,m_support_along*1000.f,int(m_supporting),int(m_support_cup));
            }
            if (!m_pump_back && m_pump>=m_profile.pump_travel*0.85f)
            {
                m_pump_back=true;
                PlayHandling(SoundPumpBack,Point(world,m_profile.pump_grab));
                if (m_pump_spent)
                {
                    Fvector velocity=m_hand_velocity[1];
                    velocity.mad(world.i,0.8f);
                    m_weapon.OnShellDrop(Point(world,m_profile.shell_point),velocity);
                    m_pump_spent=false;
                }
                m_pump_ready=false;
                vr::HandHaptic(HandOf(0),0.15f);
            }
            if (m_pump_back && m_pump<=0.008f)
            {
                m_pump_back=false;
                PlayHandling(SoundPumpForward,Point(world,m_profile.pump_grab));
                m_pump_ready=!m_weapon.m_magazine.empty();
                vr::HandHaptic(HandOf(0),0.2f);
            }
        }
        else if (m_pumping)
        {
            // Let go mid-stroke: the fore-end returns, completing a stroke
            // that already reached the back.
            m_pumping=false;
            if (m_pump_back) m_pump_ready=!m_weapon.m_magazine.empty();
            m_pump_back=false;
            m_pump=0.f;
        }
    }
    // One shell at a time from the pouch, with either support-hand control,
    // and only while the weapon has room for it.
    // A press of either control in the pouch (the other may still be held:
    // a hand that was squeezing the weapon's grip).
    const bool press=(left_trigger && !m_mag_trigger) || (left_grip && !m_grip[0]);
    if (!m_shell_held && !m_pumping && left_valid && press &&
        Near(left.c,Hip(actor,HandOf(0)==1),vr_pouch_reach) && !VrHandHoldsItem(actor,HandOf(0)))
    {
        const bool has=m_weapon.VrHasShell(), room=int(m_weapon.m_magazine.size())<m_weapon.iMagazineSize;
        if (has && room) { m_shell_held=true; vr::HandHaptic(HandOf(0),0.12f); }
        else vr::HandHaptic(HandOf(0),0.04f);
        if (vr::ContactDebug()>0)
            Msg("* [VR shells] %s: pouch press: rounds in inventory %d, room %d (%u/%d)%s",m_weapon.cNameSect().c_str(),
                int(has),int(room),unsigned(m_weapon.m_magazine.size()),m_weapon.iMagazineSize,
                has && room ? "; round taken" : "");
    }
    if (m_shell_held && (!left_valid || !mag_hold)) m_shell_held=false;
    if (!m_shell_held) { m_shell_rounds=unsigned(m_weapon.m_magazine.size()); return; }
    Fmatrix shell_pose;
    const bool posed=m_profile.shell_bones[1].size() ? ShellHandPose(shell_pose) : false;
    const Fvector held=posed ? Point(shell_pose,m_profile.shell_center[1]) : left.c;
    if (m_profile.break_action)
    {
        if (m_break_open && m_break_angle>=m_profile.hinge_angle*0.8f)
            for (int c=0;c<2;++c)
            {
                if (m_chambers[c] || !Near(held,ChamberWorld(world,c),0.045f)) continue;
                m_chambers[c]=1;
                if (m_weapon.VrLoadShell())
                {
                    if (!m_weapon.VrPlayShellSound()) PlayHandling(SoundShellIn,held);
                    m_shell_held=false;
                    vr::HandHaptic(HandOf(0),0.25f);
                }
                else m_chambers[c]=0;
                break;
            }
    }
    else if (vr::ContactDebug()>0 && Device.dwTimeGlobal-m_debug_time>500 &&
        held.distance_to(Point(world,m_profile.loading_port))<0.15f && [&]()
        {
            m_debug_time=Device.dwTimeGlobal;
            Msg("* [VR shells] %s: round %.0f mm from the port; action open %d (bolt u %.2f, locked %d, slide %.0f mm)",
                m_weapon.cNameSect().c_str(),held.distance_to(Point(world,m_profile.loading_port))*1000.f,
                int(!m_profile.internal_rounds || (m_profile.manual_cycle ? m_action_u>=0.75f :
                    (m_mechanism.locked || m_mechanism.slide>=0.9f*m_profile.cycle_travel))),
                m_action_u,int(m_mechanism.locked),m_mechanism.slide*1000.f);
            return false;
        }()) {}
    if (!m_profile.break_action && int(m_weapon.m_magazine.size())<m_weapon.iMagazineSize &&
        // Through the open action: a bolt-action's bolt drawn fully back, a
        // self-loader's held open by the follower (or by the hand).
        (!m_profile.internal_rounds || (m_profile.manual_cycle ? m_action_u>=0.75f :
            (m_mechanism.locked || m_mechanism.slide>=0.9f*m_profile.cycle_travel))) &&
        Near(held,Point(world,m_profile.loading_port),0.07f) &&
        m_weapon.VrLoadShell())
    {
        if (!m_weapon.VrPlayShellSound()) PlayHandling(SoundShellIn,held);
        // Pressed into the loading port: one round from inventory joins the
        // tube. An empty chamber still needs a pump stroke.
        m_shell_held=false;
        vr::HandHaptic(HandOf(0),0.2f);
    }
    m_shell_rounds=unsigned(m_weapon.m_magazine.size());
}

void VrHandMotion(const Fmatrix& previous, const Fmatrix& current, const Fmatrix& body,
    const Fvector& carried, float dt, Fvector& linear, Fvector& angular)
{
    linear.sub(current.c,previous.c).div(dt);
    body.transform_dir(linear);
    linear.add(carried);
    Fvector axis;
    angular.crossproduct(previous.i,current.i);
    axis.crossproduct(previous.j,current.j); angular.add(axis);
    axis.crossproduct(previous.k,current.k); angular.add(axis);
    angular.mul(0.5f/dt);
    body.transform_dir(angular);
}

Fvector VrCarriedVelocity(CActor& actor)
{
    Fvector velocity; velocity.set(0,0,0);
    if (CCharacterPhysicsSupport* physics=actor.character_physics_support())
        if (physics->movement()) physics->movement()->GetCharacterVelocity(velocity);
    return velocity;
}

void CVrWeaponInteraction::Interact(CActor& actor)
{
    PollContact();
    // Between frames of handling: the switch normalizes the mechanism and
    // swaps the weapon's magazine with the launcher's.
    if (m_launcher_toggle)
    {
        m_launcher_toggle=false;
        if (m_weapon.IsGrenadeLauncherAttached() && m_weapon.GetState()==CWeapon::eIdle && !m_weapon.IsPending())
        {
            if (actor.is_safemode()) actor.set_safemode(false);
            m_weapon.ToggleGrenadeLauncher();
            vr::HandHaptic(HandOf(0),0.2f);
        }
    }
    // A derived profile decides the mechanism (break-action or tube loading)
    // that VrPhysicalMechanismAvailable reports: measure the model as soon as
    // the weapon is the player's.
    if (m_supported && m_profile.automatic && g_vr_owns_present && m_weapon.H_Parent()==&actor)
        EnsureVisual();
    if (!m_supported || !m_weapon.VrPhysicalMechanismAvailable())
    {
        // Generic slot weapons may have no audited physical VR profile. They
        // can still be drawn and stowed with a right-hand hip grip, retaining
        // their normal weapon behavior rather than fabricating magazine data.
        Fmatrix right;
        const bool tracked=g_vr_owns_present && IsGameTypeSingle() &&
            m_weapon.H_Parent()==&actor && vr::GameplayActive() &&
            actor.g_Alive() && !actor.Holder() && !actor.Remote() &&
            !actor.IsTalking() && actor.cam_Active()==actor.cam_FirstEye() &&
            !(CurrentGameUI() && CurrentGameUI()->TopInputReceiver()) &&
            (m_weapon.CurrSlot()==INV_SLOT_2 || m_weapon.CurrSlot()==INV_SLOT_3) &&
            vr::HandTransform(HandOf(1),false,right);
        const bool grip=tracked && vr::GripHeld(vr::HandSqueeze(HandOf(1)),m_grip[1]);
        const bool active=actor.inventory().ActiveItem()==&m_weapon;
        // The support trigger on the barrel line goes back to the rifle (or
        // to the launcher on a weapon without physical handling). At the
        // pouch, in launcher mode, it loads a grenade by the game's reload,
        // which keeps the launcher's rocket and ammunition in step.
        const bool support_trigger=tracked && active && grip && !m_carried &&
            m_weapon.IsGrenadeLauncherAttached() && vr::HandTrigger(HandOf(0))>0.65f;
        Fmatrix support_hand, weapon_pose;
        if (support_trigger && !m_mag_trigger && !VrHandHoldsItem(actor,HandOf(0)) &&
            vr::HandTransform(HandOf(0),false,support_hand))
        {
            if (Near(support_hand.c,Hip(actor,HandOf(0)==1),vr_pouch_reach))
            {
                if (!m_weapon.VrPhysicalMechanismAvailable() && !m_weapon.IsPending())
                {
                    actor.IR_OnKeyboardPress(kWPN_RELOAD);
                    actor.IR_OnKeyboardRelease(kWPN_RELOAD);
                    vr::HandHaptic(HandOf(0),0.15f);
                }
            }
            else if (GenericTransform(weapon_pose) &&
                SupportNear(right.c,Point(weapon_pose,m_weapon.vLoadedFirePoint),weapon_pose.k,support_hand.c)==1)
                m_launcher_toggle=true;
        }
        m_mag_trigger=support_trigger;
        // Squeezing the support hand on the barrel line of a two-handed
        // weapon steadies it; releasing that grip returns to a one-handed hold.
        const bool left_grip=tracked && vr::GripHeld(vr::HandSqueeze(HandOf(0)),m_grip[0]);
        Fmatrix left_hand, held;
        if (!left_grip || !active) m_supporting=false;
        else if (m_carried && !m_grip[0] && vr::HandTransform(HandOf(0),false,left_hand) &&
            GenericTransform(held) && Near(left_hand.c,held.c,0.15f))
        {
            TakeBack();
            m_grip[0]=m_grip[1]=true;
            return;
        }
        else if (!m_carried && !m_grip[0] && vr::HandTransform(HandOf(0),false,left_hand) && GenericTransform(held))
        {
            const Fvector muzzle=Point(held,m_weapon.vLoadedFirePoint);
            const int kind=SupportNear(right.c,muzzle,held.k,left_hand.c);
            m_supporting=kind!=0;
            m_support_cup=kind==2;
            m_support_along=SupportAlong(right.c,muzzle,left_hand.c);
            if (m_supporting) vr::HandHaptic(HandOf(0),0.12f);
        }
        if (!active) { m_carried=false; m_holster_armed=false; }
        if (tracked && active && m_grip[1] && !grip && left_grip && m_supporting && HandOver())
        {
            if (m_trigger) actor.IR_OnKeyboardRelease(kWPN_FIRE);
            m_trigger=m_release_pending=false;
            m_grip[0]=false; m_grip[1]=true;
            return;
        }
        m_grip[0]=left_grip;
        if (tracked && active && ReleaseWanted(actor,m_grip[1] && !grip,!grip)) m_release_pending=true;
        if (grip || !tracked) m_release_pending=false;
        if (tracked && active && m_release_pending && !m_weapon.IsPending())
        {
            m_weapon.FireEnd();
            VrReleaseHeldWeapon(actor,m_weapon);
            m_release_pending=false;
            m_trigger=false;
            m_grip[1]=grip;
            return;
        }
        const bool firing=tracked && active && (grip || KeptWithoutGrip()) && !m_carried &&
            vr::HandTrigger(HandOf(1))>0.65f;
        // Shotguns retain their native Action(kWPN_FIRE) state machine,
        // including the double-barrel and interrupted-reload behaviours.
        if (firing && !m_trigger) actor.IR_OnKeyboardPress(kWPN_FIRE);
        else if (!firing && m_trigger) actor.IR_OnKeyboardRelease(kWPN_FIRE);
        m_trigger=firing;
        m_grip[1]=grip;
        return;
    }
    if (!Mode()) { Normalize(); m_restore_empty_chamber=false; return; }
    // Derived profiles need the HUD model's measurements before handling.
    if (m_profile.automatic && !EnsureVisual()) return;
    if (actor.inventory().ActiveItem()==&m_weapon) RequestScopeView();
    m_mechanism.Initialize(m_weapon.m_magazine);
    if (!m_parts_ready) ResetParts();
    // A box taken out or a new one put in leaves no belt on the tray.
    if (m_profile.belt.used && m_mechanism.inserted!=m_parts_inserted)
    {
        m_parts_inserted=m_mechanism.inserted;
        m_belt.seated=false;
        m_belt.hand=-1;
    }
    // A full bolt or slide cycle fits inside the weapon's shot interval.
    m_mechanism.cycle_time=clampr(m_weapon.fOneShotTime*0.7f,0.03f,0.09f);
    // Firing the chambered round after a proxy drop lowers the baseline, so
    // any later native reload is recognised as a new inserted magazine.
    m_basic_reload_rounds=std::min(m_basic_reload_rounds,unsigned(m_weapon.m_magazine.size()));
    if (m_basic_magazine && m_mechanism.inserted<0 &&
        m_weapon.m_magazine.size()>m_basic_reload_rounds)
    {
        // Native reload replenished ammunition after a proxy magazine drop.
        // Treat this as a new inserted magazine for the next B release.
        m_mechanism.inserted=0;
    }
    if (m_restore_empty_chamber)
    {
        m_mechanism.chamber=false;
        m_restore_empty_chamber=false;
    }
    if (m_weapon.m_magazine.empty()) m_mechanism.chamber=false;
    else if (m_basic_magazine && m_mechanism.inserted>=0 && m_profile.cycle_travel<=0)
        m_mechanism.chamber=true;
    const bool ui=CurrentGameUI() && CurrentGameUI()->TopInputReceiver();
    Fmatrix right,left,world;
    right.identity(); left.identity();
    const bool valid=vr::GameplayActive() && !ui && actor.g_Alive() &&
        !actor.Holder() && !actor.Remote() && !actor.IsTalking() &&
        actor.cam_Active()==actor.cam_FirstEye() && vr::HandTransform(HandOf(1),false,right);
    if (!valid)
    {
        m_weapon.FireEnd();
        if (m_racking && !m_profile.manual_cycle) m_mechanism.ReleaseManualSlide(m_weapon.m_magazine);
        m_mechanism.Stow(true);
        m_input.Reset();
        m_racking=m_racked=m_inserting=m_supporting=false;
        m_guide.active=false;
        m_fire_queued=0;
        m_trigger=m_grip[0]=m_grip[1]=m_button=m_secondary=m_mag_trigger=false;
        m_release_pending=false;
        m_hand_epoch=0; m_have_hand[0]=m_have_hand[1]=false;
        return;
    }
    const bool left_valid=vr::HandTransform(HandOf(0),false,left);
    const bool hand_valid[2]={left_valid,true};
    const Fmatrix hand_pose[2]={left,right};
    const unsigned epoch=vr::GameplayInputEpoch();
    // Velocities from the hands' poses in the body's frame: a snap turn or
    // walking moves the hands in the world, not the player's real hands.
    Fmatrix body;
    const bool body_valid=vr::BodyTransform(body);
    const Fvector carried=VrCarriedVelocity(actor);
    for (int hand=0;hand<2;++hand)
    {
        m_hand_velocity[hand].set(0,0,0);
        m_hand_angular[hand].set(0,0,0);
        Fmatrix local;
        const bool local_valid=hand_valid[hand] && body_valid && vr::HandInBody(HandOf(hand),false,local);
        if (local_valid && m_have_hand[hand] && m_hand_epoch==epoch && Device.fTimeDelta>0.001f)
        {
            VrHandMotion(m_previous_hand[hand],local,body,carried,Device.fTimeDelta,
                m_hand_velocity[hand],m_hand_angular[hand]);
            // A jump (tracking regained) is no motion.
            if (Fvector().sub(local.c,m_previous_hand[hand].c).square_magnitude()>=0.25f*0.25f)
                m_hand_velocity[hand]=carried;
            if (m_hand_angular[hand].square_magnitude()>400.f) m_hand_angular[hand].set(0,0,0);
        }
        if (local_valid) m_previous_hand[hand]=local;
        m_have_hand[hand]=local_valid;
    }
    m_hand_epoch=epoch;
    const bool grip[2]={left_valid && vr::GripHeld(vr::HandSqueeze(HandOf(0)),m_grip[0]),
        vr::GripHeld(vr::HandSqueeze(HandOf(1)),m_grip[1])};
    const bool active=actor.inventory().ActiveItem()==&m_weapon;
    // A pistol is held with the grip relaxed too (ReleaseWanted).
    const bool held=(grip[1] || (active && KeptWithoutGrip())) && !m_carried;
    const bool trigger=held && vr::HandTrigger(HandOf(1))>0.65f;
    m_trigger_value=held ? clampr(vr::HandTrigger(HandOf(1))/0.65f,0.f,1.f) : 0.f;
    const bool mag_trigger=left_valid && vr::HandTrigger(HandOf(0))>0.65f;
    const bool mag_hold=mag_trigger || grip[0];
    // Slide release / fire mode stay on right A: left X toggles the
    // inventory, so a left-held weapon uses the support hand's A.
    const bool button=vr::HandButton(1) && !VrHeadGesture(1);
    const bool secondary=vr::HandSecondaryButton(HandOf(1));
    if (!active) { m_carried=false; m_holster_armed=false; }
    if (active && m_grip[1] && !grip[1] && grip[0] && m_supporting && !m_pumping && !m_racking &&
        m_mechanism.held<0 && HandOver())
    {
        m_weapon.FireEnd();
        m_trigger=m_release_pending=false;
        m_grip[0]=false; m_grip[1]=true;
        m_button=button; m_secondary=secondary; m_mag_trigger=false;
        return;
    }
    if (active && ReleaseWanted(actor,m_grip[1] && !grip[1],!grip[1])) m_release_pending=true;
    if (grip[1]) m_release_pending=false;
    if (active && m_release_pending)
    {
        m_weapon.FireEnd();
        const bool ready=(m_weapon.GetState()==CWeapon::eIdle ||
            m_weapon.GetState()==CWeapon::eMisfire) &&
            m_weapon.GetNextState()==m_weapon.GetState() && !m_weapon.IsPending();
        if (ready)
        {
            VrReleaseHeldWeapon(actor,m_weapon);
            m_release_pending=false;
        }
        m_trigger=trigger; m_button=button; m_secondary=secondary;
        m_grip[0]=grip[0]; m_grip[1]=grip[1]; m_mag_trigger=mag_trigger;
        return;
    }
    if (!m_input.Step(epoch,true,trigger || button || secondary))
    {
        m_weapon.FireEnd();
        m_trigger=trigger; m_button=button; m_secondary=secondary;
        m_grip[0]=grip[0]; m_grip[1]=grip[1]; m_mag_trigger=mag_trigger;
        return;
    }
    if (!active)
    {
        m_weapon.FireEnd(); m_holstered=true;
        // Drawing is arbitrated once per actor in UpdateVrWeapons. A held
        // grip passing another holster must never switch slot weapons.
    }
    else
    {
        // Drawn by key or wheel rather than by a hand: the dominant hand.
        if (m_holstered && Device.dwTimeGlobal-m_hand_assigned>2000)
        { m_hand=vr::DominantHand(); m_carried=false; }
        m_holstered=false;
        const bool ready=(m_weapon.GetState()==CWeapon::eIdle ||
            m_weapon.GetState()==CWeapon::eMisfire) &&
            m_weapon.GetNextState()==m_weapon.GetState() && !m_weapon.IsPending();
        // A trigger held through focus recovery can still draw a spare once
        // the hand reaches the pouch; Draw() cannot duplicate a held mag.
        if (m_profile.detachable && m_mechanism.held<0 && m_ejecting<0 && left_valid &&
            mag_trigger && PouchHasAmmo() && Near(left.c,Hip(actor,HandOf(0)==1),vr_pouch_reach) && !VrHandHoldsItem(actor,HandOf(0)))
        {
            int index=m_mechanism.next_pouch;
            if (index==m_mechanism.inserted) index=1-index;
            if (index!=m_mechanism.inserted)
            {
                m_mechanism.next_pouch=index;
                if (m_mechanism.Draw())
                { m_pouch_preview=true; vr::HandHaptic(HandOf(0),0.15f); }
            }
        }
        // Hands work the weapon whatever it is doing (a shot or its
        // animation included); only drawing, holstering and the native
        // reload or switch keep them off. A press then is never lost.
        const u32 state=m_weapon.GetState();
        const bool handling=state!=CHUDState::eShowing && state!=CHUDState::eHiding &&
            state!=CHUDState::eHidden && state!=CWeapon::eReload && state!=CWeapon::eSwitch;
        if (handling && Transform(world))
        {
            Fmatrix inverse; inverse.invert(world);
            Fvector local; if (left_valid) inverse.transform_tiny(local,left.c);
            // A magazine back in the weapon or the pouch is no longer held
            // where the hand closed on it.
            if (m_mechanism.held<0) m_mag_grabbed=false;
            if (m_mechanism.held<0) { m_guide.active=false; m_inserting=false; }
            Fmatrix held_root=left;
            const bool held_valid=left_valid && MagazineHandPose(HandOf(0),held_root);
            if (left_valid && !m_racking && m_handle_hand_valid)
                m_cycle_use_hand=local.distance_to(m_handle_hand)<local.distance_to(m_profile.cycle_grab);
            Fvector cycle_point=Point(world,CycleGrabLocal());
            cycle_point.mad(world.k,-CycleGrabTravel());
            const Fvector mag_base=Point(world,m_profile.mag_base);
            // A support hand on a part (the cover) or the belt does nothing else.
            // Parts are taken with the trigger, as every other handling is.
            const bool on_part=UpdateParts(world,left_valid,left,mag_trigger,mag_trigger && !m_mag_trigger);
            if (left_valid && grip[0] && !m_grip[0] && m_carried &&
                Near(left.c,Point(world,GripLocal()),0.12f))
            {
                TakeBack();
                m_grip[0]=m_grip[1]=true;
                m_trigger=false; m_button=button; m_secondary=secondary; m_mag_trigger=mag_trigger;
                return;
            }
            // The support hand's trigger, closed on the slide or charging
            // handle, pulls it; the grip stays for holding the weapon.
            if (left_valid && mag_trigger && !m_mag_trigger && !m_racking && m_mechanism.held<0 && !on_part &&
                m_profile.cycle_travel>0 && Near(left.c,cycle_point,m_profile.grab_radius) &&
                RuleAllows(m_profile.rules.chamber,"charging the weapon"))
            {
                m_rack_start=local.z+m_mechanism.slide;
                m_rack_lift_start=local.y;
                // Along a path the hand keeps its offset from the knob.
                const vr::V3 knob=m_profile.action_path.GrabAt(m_action_u);
                m_action_offset.set(local.x-knob.x,local.y-knob.y,local.z-knob.z);
                m_dust_open=true;
                m_racking=true; m_racked=false;
                // A bolt left open is taken where it is: back already.
                m_slide_back_sounded=m_profile.manual_cycle && m_mechanism.slide>=m_profile.cycle_travel*0.8f;
            }
            // The support hand steadies the weapon whenever its grip is closed
            // at the fore-end or round the weapon hand, however it got there:
            // closing it on the way, then reaching the weapon, counts too.
            if (left_valid && grip[0] && !m_supporting && !on_part && !VrHandHoldsItem(actor,HandOf(0)))
            {
                if (m_mechanism.held<0 && !m_shell_held && !m_racking)
                {
                    const Fvector muzzle=Point(world,m_profile.muzzle);
                    // Open barrels are taken where they hang: test the hand
                    // where it would be with them closed.
                    Fvector support=left.c;
                    if (m_profile.break_action && m_break_angle>0.f)
                    {
                        Fmatrix moved, inverse;
                        HingeWorld(world,moved);
                        inverse.invert(moved);
                        inverse.transform_tiny(support,left.c);
                    }
                    const int kind=SupportNear(right.c,muzzle,world.k,support);
                    m_supporting=kind!=0;
                    m_support_cup=kind==2;
                    m_support_along=SupportAlong(right.c,muzzle,support);
                    m_support_barrels=false;
                    if (m_profile.break_action && kind==1)
                    {
                        Fmatrix inverse; inverse.invert(world);
                        Fvector local; inverse.transform_tiny(local,support);
                        m_support_barrels=local.z>m_profile.hinge_point.z+0.03f;
                    }
                    if (m_supporting) vr::HandHaptic(HandOf(0),0.12f);
                }
            }
            // A hand holding a magazine is not on the weapon.
            if (m_supporting && (!grip[0] || m_mechanism.held>=0)) m_supporting=false;
            if (m_racking)
            {
                if (left_valid && mag_trigger)
                {
                    // A manual action follows its animated path (a bolt lifts
                    // through its turn, then slides); a slide or handle goes
                    // straight back.
                    float position=m_rack_start-local.z;
                    if (m_profile.manual_cycle && m_profile.action_path.Valid())
                    {
                        m_action_u=m_profile.action_path.Project(vr::V3(local.x-m_action_offset.x,
                            local.y-m_action_offset.y,local.z-m_action_offset.z),m_action_u);
                        position=m_profile.action_path.RearAt(m_action_u);
                    }
                    m_mechanism.MoveManualSlide(position,m_weapon.m_magazine,[&]()
                    {
                        if (!m_weapon.m_magazine.empty())
                        {
                            const Fvector port=Point(world,m_profile.shell_point);
                            m_weapon.SpawnAmmo(1,m_weapon.m_magazine.back().m_ammoSect.c_str(),0xffff,&port);
                            m_weapon.m_magazine.pop_back();
                            m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
                        }
                    },[&]()
                    {
                        Fvector velocity; m_weapon.PHGetLinearVell(velocity);
                        m_weapon.OnShellDrop(Point(world,m_profile.shell_point),velocity);
                    });
                    m_racked=m_racked || m_mechanism.slide>=m_profile.cycle_travel*0.8f ||
                        m_mechanism.manual_unlock_ready;
                    // Every full stroke clicks, back and forward, not only the first.
                    if (!m_slide_back_sounded && m_mechanism.slide>=m_profile.cycle_travel*0.8f)
                    { m_slide_back_sounded=true; PlayHandling(SoundSlideBack,cycle_point); }
                    else if (m_slide_back_sounded && m_mechanism.slide<=m_profile.cycle_travel*0.2f)
                    { m_slide_back_sounded=false; PlayHandling(SoundSlideForward,cycle_point); }
                }
                else
                {
                    // An HK's handle lifted 2 cm into its notch at full travel
                    // stays back; a slide springs home; a bolt stays where it
                    // is let go.
                    const bool notched=m_mechanics.handle_notch && left_valid && local.y-m_rack_lift_start>=0.02f &&
                        m_mechanism.LockOpen();
                    if (notched)
                    {
                        m_handle_hooked=true;
                        if (vr::ContactDebug()>0) Msg("* [VR weapon] %s: charging handle hooked into its notch",
                            m_weapon.cNameSect().c_str());
                    }
                    else if (!m_profile.manual_cycle) m_mechanism.ReleaseManualSlide(m_weapon.m_magazine);
                    if (m_racked) { m_weapon.bMisfire=false; m_weapon.bClearJamOnly=false; }
                    if (m_slide_back_sounded && !m_profile.manual_cycle && !notched) PlayHandling(SoundSlideForward,cycle_point);
                    m_slide_back_sounded=false;
                    m_racking=m_racked=false;
                    vr::HandHaptic(HandOf(0),0.15f);
                }
            }
            // Right upper face button (B on Touch) releases the magazine.
            if (m_basic_magazine && secondary && !m_secondary &&
                m_mechanism.inserted>=0 && m_mechanism.held<0 &&
                m_mechanism.Detach(m_weapon.m_magazine))
            {
                const int index=m_mechanism.held;
                // Unprofiled pistols replace native reload: like it, keep the
                // released magazine's rounds in inventory. The prop is empty.
                ReturnCartridges(m_mechanism.magazines[index]);
                m_mechanism.magazines[index].clear();
                Fmatrix pose=world;
                pose.c=Point(world,m_profile.mag_base);
                CPhysicsShell* shell=CreateShell(index,pose,1);
                if (shell) FinishDrop(index,shell);
                else m_mechanism.Stow();
                m_basic_reload_rounds=unsigned(m_weapon.m_magazine.size());
                m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
                PlayHandling(SoundMagOut,mag_base);
                vr::HandHaptic(HandOf(1),0.2f);
            }
            // With a fresh magazine already in the support hand, the release
            // drops the one in the well (tactical reload).
            if (m_profile.detachable && secondary && !m_secondary && m_mechanism.inserted>=0 &&
                m_mechanism.held>=0 && !m_inserting && RuleAllows(m_profile.rules.magazine,"releasing the magazine"))
                DropInserted(world);
            // The support hand takes the magazine straight out into the
            // hand: on it (trigger or grip) when the release is pressed, or
            // by pulling its trigger on the magazine, thumbing the release as
            // the hand closes (not next to the charging handle, nor from a
            // hand steadying the weapon, which is often within reach of it).
            // On it: round its body or under its floorplate (MagazineGrab).
            float grab_along=0.f;
            const bool on_magazine=left_valid && MagazineGrab(world,left.c,grab_along);
            const bool thumb_release=(!m_supporting || !grip[0]) && mag_trigger && !m_mag_trigger && !m_racking &&
                !(m_profile.cycle_travel>0 && Near(left.c,cycle_point,m_profile.grab_radius));
            // Only the support trigger takes it: from a free hand, or one
            // opened on the magazine; a hand still squeezing to steady the
            // weapon pulls its trigger while shooting and must not rip it
            // out. The release (B) always drops it, as in H3VR.
            if (m_profile.detachable && m_mechanism.inserted>=0 && m_mechanism.held<0 && on_magazine && !on_part &&
                thumb_release &&
                RuleAllows(m_profile.rules.magazine,"taking out the magazine") &&
                m_mechanism.Detach(m_weapon.m_magazine))
            {
                m_supporting=m_support_cup=false;
                m_mag_grabbed=true; m_mag_grab_along=grab_along;
                // Seated, its pose is the weapon's own.
                m_mag_in_hand.mul_43(Fmatrix().invert(left),world);
                // It slides out along the well with the hand, from its seat;
                // it seats itself again only once drawn half out and pushed
                // back. The pose is taken again: the hand now holds it where
                // it closed on it.
                if (m_contact && m_contact->path.Valid() && MagazineHandPose(HandOf(0),held_root))
                {
                    m_guide.TakeFromSeat(ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),held_root)),m_contact->path);
                    m_inserting=true;
                    m_slide_tick=0.f;
                }
                PlayHandling(SoundMagOut,mag_base);
                vr::HandHaptic(HandOf(0),0.2f);
                vr::HandHaptic(HandOf(1),0.12f);
            }
            if (m_profile.detachable && secondary && !m_secondary &&
                m_mechanism.inserted>=0 && m_mechanism.held<0 &&
                RuleAllows(m_profile.rules.magazine,"releasing the magazine"))
            {
                // The catch releases: the magazine slides out and falls
                // (DropInserted sounds it). Taken with the trigger, it came
                // out into the hand above.
                DropInserted(world);
            }
            // A released magazine still sliding out is caught by the support
            // hand closing (trigger or grip) on it, and drawn out by hand from
            // there.
            if (m_ejecting>=0 && m_mechanism.held<0 && left_valid && !on_part && m_contact && m_contact->path.Valid() &&
                ((mag_trigger && !m_mag_trigger) || (grip[0] && !m_grip[0])))
            {
                Fmatrix sliding; sliding.mul_43(world,ToMatrix(m_contact->path.At(m_eject_s)));
                if (left.c.distance_to(Point(sliding,m_profile.mag_box_center))<0.08f &&
                    m_mechanism.TakeLoose(m_ejecting))
                {
                    Fmatrix inverse; inverse.invert(sliding);
                    Fvector local; inverse.transform_tiny(local,left.c);
                    local.sub(m_profile.mag_base);
                    m_mag_grabbed=true;
                    m_mag_grab_along=clampr(local.dotproduct(m_profile.well_axis),-0.05f,2.f*m_profile.mag_box_half.y);
                    m_mag_in_hand.mul_43(Fmatrix().invert(left),sliding);
                    m_supporting=m_support_cup=false;
                    if (MagazineHandPose(HandOf(0),held_root))
                    {
                        m_guide.TakeFromSeat(ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),held_root)),
                            m_contact->path,m_eject_s);
                        m_inserting=true;
                        m_slide_tick=0.f;
                    }
                    m_ejecting=-1;
                    ReleaseModel(m_eject_visual);
                    vr::HandHaptic(HandOf(0),0.15f);
                }
            }
            if (ShellLoading())
                InteractShells(actor,world,left_valid,left,mag_hold,grip[0],mag_trigger,secondary);
            // The support trigger on the bolt catch or slide stop releases a
            // locked action, on weapons that have one.
            const bool has_release=m_mechanics.has_bolt_catch || m_mechanics.has_slide_stop ||
                m_mechanics.has_bolt_release || m_mechanics.has_slide_release;
            if (has_release && m_mechanism.locked && left_valid && mag_trigger && !m_mag_trigger && !m_racking &&
                m_mechanism.held<0 && Near(left.c,Point(world,m_profile.release_point),0.04f))
            {
                m_mechanism.ReleaseSlide(m_weapon.m_magazine);
                PlayHandling(SoundSlideForward,cycle_point);
                vr::HandHaptic(HandOf(0),0.15f);
            }
            // An HK's handle in its notch: a slap down (1.2 m/s or more)
            // past it releases the bolt.
            if (m_mechanics.handle_notch && m_mechanism.locked && left_valid && m_slap_previous_valid &&
                Device.fTimeDelta>0.f && !m_racking)
            {
                const float down=(m_slap_previous.y-left.c.y)/Device.fTimeDelta;
                const Fvector handle=cycle_point;
                if (down>=1.2f && Near(left.c,handle,0.07f))
                {
                    m_mechanism.ReleaseSlide(m_weapon.m_magazine);
                    PlayHandling(SoundSlideForward,handle);
                    vr::HandHaptic(HandOf(0),0.3f);
                    if (vr::ContactDebug()>0) Msg("* [VR weapon] %s: slapped the handle out of its notch (%.1f m/s)",
                        m_weapon.cNameSect().c_str(),down);
                }
            }
            m_slap_previous=left.c;
            m_slap_previous_valid=left_valid;
            // The dust cover is pushed shut with the support trigger.
            if (m_dust_open && m_profile.dust_cover_bone.size() && left_valid && mag_trigger && !m_mag_trigger &&
                !m_racking && Near(left.c,Point(world,m_profile.dust_cover_grab),0.035f))
            {
                m_dust_open=false;
                vr::HandHaptic(HandOf(0),0.08f);
            }
            // The support trigger on the underbarrel launcher selects it.
            if (m_weapon.IsGrenadeLauncherAttached() && left_valid && mag_trigger && !m_mag_trigger &&
                !m_racking && !m_inserting && m_mechanism.held<0 && !VrHandHoldsItem(actor,HandOf(0)) &&
                Near(left.c,Point(world,m_profile.launcher_grab),0.07f))
                m_launcher_toggle=true;
            // A releases a locked action on a weapon with a release (as a
            // backup to pressing it); otherwise it cycles firing modes.
            if (button && !m_button && !m_racking)
            {
                if (has_release && m_mechanism.locked)
                {
                    m_mechanism.ReleaseSlide(m_weapon.m_magazine);
                    PlayHandling(SoundSlideForward,cycle_point);
                }
                else
                {
                    actor.IR_OnKeyboardPress(kWPN_FIREMODE_NEXT);
                    actor.IR_OnKeyboardRelease(kWPN_FIREMODE_NEXT);
                }
                vr::HandHaptic(HandOf(1),0.10f);
            }
            if (m_profile.detachable && m_mechanism.held>=0 && m_mechanism.inserted<0 && held_valid && m_contact)
            {
                // The hand puts the magazine in (VrMagazineContact.h): its
                // feed lips at the well's mouth, lined up within about 28
                // degrees, and the well takes it onto its line; it then slides
                // in with the hand and seats two thirds of the way in, or at
                // once on a quick push. Drawn back out of the mouth, it is the
                // hand's again.
                const vr::InsertPath& path=m_contact->path;
                const vr::InsertZone zone;
                const vr::Frame held=ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),held_root));
                if (!m_guide.active && vr::InsertCheck(path,held,zone)==vr::InsertTaken &&
                    RuleAllows(m_profile.rules.magazine,"inserting the magazine"))
                {
                    float lateral=0.f;
                    m_guide.Take(held,path.Project(held.Apply(path.lead),lateral));
                    m_inserting=true;
                    m_slide_tick=0.f;
                    vr::HandHaptic(HandOf(0),0.08f);
                }
                if (m_guide.active)
                {
                    const vr::GuideEvent event=vr::StepGuide(m_guide,path,zone,held,Device.fTimeDelta);
                    // Friction as it slides: a faint tick every 12 mm.
                    const float depth=m_guide.In(path)*path.mouth*path.length;
                    if (_abs(depth-m_slide_tick)>=0.012f) { m_slide_tick=depth; vr::HandHaptic(HandOf(0),0.04f); }
                    if (event==vr::GuideSeated) SeatMagazine(mag_base);
                    else if (event==vr::GuideLeft) { m_inserting=false; vr::HandHaptic(HandOf(0),0.06f); }
                }
            }
            if (m_mechanism.held>=0 && left_valid && !mag_hold && (m_mag_trigger || m_grip[0]))
            {
                // Let go a third of the way in or more, the magazine goes home;
                // short of that it falls (or is stowed at the hip).
                if (m_contact && vr::OnRelease(m_guide,m_contact->path)==vr::ReleaseSeats)
                    SeatMagazine(mag_base);
                else
                {
                    if (Near(left.c,Hip(actor,HandOf(0)==1),0.22f))
                    { m_mechanism.Stow(true); m_pouch_preview=false; }
                    // Still guided here, so it falls from where it was drawn.
                    else DropMagazine(0);
                    m_guide.active=false; m_inserting=false;
                }
            }
            if (!m_racking && trigger && !m_trigger && (ready || state==CWeapon::eFire)) PullTrigger();
        }
        // A pull while the last shot is still in its fire state (a full shot
        // animation, the end of a burst) fires as on the desktop; the
        // readiness gate above would otherwise swallow quick taps.
        else if (!m_racking && trigger && !m_trigger && m_weapon.GetState()==CWeapon::eFire)
            PullTrigger();
        // A pull that arrived while the bolt was still cycling: fire now that
        // it is in battery, a single shot if the trigger is already released.
        if (m_fire_queued && !m_mechanism.cycling)
        {
            if (Device.dwTimeGlobal-m_fire_queued<=250 && !m_racking)
            {
                m_weapon.FireStart();
                if (!trigger) m_weapon.FireEnd();
            }
            m_fire_queued=0;
        }
        // Released every frame, not only when idle: an automatic weapon is in
        // its fire state while shooting, which the readiness gate excludes.
        if (!trigger && m_trigger) m_weapon.FireEnd();
    }
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    m_trigger=trigger; m_button=button; m_secondary=secondary;
    m_grip[0]=grip[0]; m_grip[1]=grip[1]; m_mag_trigger=mag_trigger;
}

Fmatrix VrTorsoFrame(CActor& actor)
{
    Fmatrix body; body.identity();
    if (!vr::TorsoForward(body.k))
    {
        body.k=actor.XFORM().k; body.k.y=0; body.k.normalize_safe();
        if (body.k.square_magnitude()<EPS_S) body.k.set(0,0,1);
    }
    body.i.crossproduct(body.j,body.k).normalize_safe();
    body.c=actor.Position();
    return body;
}

Fvector VrShoulderPoint(CActor& actor, bool right)
{
    // With the body shown, from its pelvis (see VrBodySlotPoint).
    Fmatrix pelvis;
    if (vr::BodyVisible() && actor.m_legs_controller.vr_body_anchor(pelvis))
    {
        const Fvector at=VrOnBodySurface(actor,right ? 0.24f : -0.24f,vr::HipHeight()-0.20f,-0.20f);
        Fvector point=pelvis.c;
        point.mad(pelvis.i,at.x).mad(pelvis.j,at.y).mad(pelvis.k,at.z);
        return point;
    }
    const Fmatrix body=VrTorsoFrame(actor);
    Fvector origin;
    if (vr::HeadPosition(origin)) origin.y-=0.20f;
    else { origin=actor.Position(); origin.y+=1.44f; }
    Fvector offset; offset.set(right ? 0.24f : -0.24f,0.f,-0.20f);
    body.transform_dir(offset);
    origin.add(offset);
    return origin;
}

Fvector VrPouchPoint(CActor& actor)
{
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const int support=weapon ? 1-weapon->VrWeaponHand() : 1-vr::DominantHand();
    return VrHipPoint(actor,support==1);
}

Fvector VrHipPoint(CActor& actor, bool right)
{
    // With the body shown, on its hips (see VrBodySlotPoint).
    Fmatrix pelvis;
    if (vr::BodyVisible() && actor.m_legs_controller.vr_body_anchor(pelvis))
    {
        // The off-hand hip carries the pouch, forward where it can be seen.
        vr::BodyPlace place;
        if (right==vr::LeftHanded() && vr::BodySlotOnBody(vr::BodyPouch,vr::LeftHanded(),place))
            return VrOnBodyPlace(actor,pelvis,place.angle,place.height);
        const Fvector at=VrOnBodySurface(actor,right ? vr::HipWidth() : -vr::HipWidth(),0.f,-0.03f);
        Fvector point=pelvis.c;
        point.mad(pelvis.i,at.x).mad(pelvis.j,at.y).mad(pelvis.k,at.z);
        return point;
    }
    const Fmatrix body=VrTorsoFrame(actor);
    Fvector origin;
    Fvector local;
    if (vr::HeadPosition(origin))
    {
        // Hip space must follow the tracked player's actual height. The
        // actor object's origin can lag the HMD or use a different stance.
        origin.y-=vr::HipHeight();
        local.set(right ? vr::HipWidth() : -vr::HipWidth(),0.f,-0.03f);
    }
    else
    {
        origin=actor.Position();
        local.set(right ? vr::HipWidth() : -vr::HipWidth(),1.67f-vr::HipHeight(),-0.03f);
    }
    body.transform_dir(local);
    origin.add(local);
    return origin;
}

void CVrWeaponInteraction::RenderPouch(CActor& actor)
{
    if (!g_vr_owns_present || !vr::GameplayActive() || m_weapon.m_ammoTypes.empty() || !PouchHasAmmo()) return;
    // The pouch shows the ammunition the support hand will draw for this
    // weapon (SwapPouchAmmo changes it).
    const shared_str& ammo=m_weapon.m_ammoTypes[PouchAmmo()];
    if (m_pouch_section!=ammo)
    {
        ReleaseModel(m_pouch_visual);
        m_pouch_section=ammo;
        if (pSettings->line_exist(ammo,"visual"))
            m_pouch_visual=::Render->model_Create(pSettings->r_string(ammo,"visual"));
    }
    if (!m_pouch_visual) return;
    Fmatrix pose=VrTorsoFrame(actor);
    pose.c=VrHipPoint(actor,HandOf(0)==1);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(m_pouch_visual);
}

namespace
{
// A free entry, otherwise the one that has waited longest.
CActor::VrPickupState::VrThrown& VrThrownEntry(CActor::VrPickupState& pickup)
{
    auto* oldest=&pickup.thrown[0];
    for (auto& entry:pickup.thrown)
    {
        if (entry.id==u16(-1)) return entry;
        if (int(entry.expires-oldest->expires)<0) oldest=&entry;
    }
    return *oldest;
}
}

void VrReleaseHeldWeapon(CActor& actor, CWeapon& weapon)
{
    const int side=weapon.VrWeaponHand();
    Fmatrix hand;
    Fvector holster;
    float range=0.f;
    const bool at_holster=vr::HandTransform(side,false,hand) &&
        weapon.VrHolsterPoint(actor,holster,range) && hand.c.distance_to(holster)<=range;
    // A knife or binoculars taken from the belt goes back there when let go.
    const bool belt=weapon.CurrSlot()==KNIFE_SLOT || weapon.CurrSlot()==BINOCULAR_SLOT;
    if (at_holster || belt || weapon.IsQuestItem())
    {
        actor.inventory().Activate(NO_ACTIVE_SLOT,true);
        vr::HandHaptic(side,0.15f);
        return;
    }
    // Opened away from the holster: the weapon leaves the hand as a world
    // object from its tracked pose and keeps the hand's motion. Detached
    // magazines are normalised first, as for any drop.
    weapon.VrPrepareSave();
    auto& pickup=actor.m_vr_pickup;
    auto& thrown=VrThrownEntry(pickup);
    thrown.id=weapon.ID();
    thrown.velocity=pickup.hand_velocity[side];
    thrown.angular=pickup.hand_angular[side];
    thrown.expires=Device.dwTimeGlobal+1500;
    thrown.place=false;
    actor.g_PerformDrop();
    vr::HandHaptic(side,0.2f);
}

void VrThrowHandBolt(CActor& actor, CInventoryItem& slot_bolt, const Fmatrix& hand, const Fvector& velocity, int side)
{
    auto& pickup=actor.m_vr_pickup;
    // GAMMA's limited bolts (CBolt__State) are used up: one from the
    // backpack flies and the slot keeps its own. Unlimited: the slot's flies
    // and a new one is made for the backpack, to go in the slot.
    ::luabind::functor<bool> state;
    const bool limited=ai().script_engine().functor("_G.CBolt__State",state) && state(actor.ID());
    PIItem thrown=limited ? actor.inventory().Same(&slot_bolt,true) : nullptr;
    if (!thrown) thrown=&slot_bolt;
    if (!limited)
        Level().spawn_item(slot_bolt.object().cNameSect().c_str(),actor.Position(),
            actor.ai_location().level_vertex_id(),actor.ID());
    if (thrown==&slot_bolt) pickup.bolt_refill_until=Device.dwTimeGlobal+3000;
    NET_Packet P;
    actor.u_EventGen(P,GE_OWNERSHIP_REJECT,actor.ID());
    P.w_u16(thrown->object().ID());
    actor.u_EventSend(P);
    auto& entry=VrThrownEntry(pickup);
    entry.id=thrown->object().ID();
    entry.velocity=velocity;
    entry.angular=pickup.hand_angular[side];
    entry.expires=Device.dwTimeGlobal+1500;
    entry.place=true;
    entry.matrix=hand;
    if (vr::PhysicalHands()) VrNearSide(entry.matrix.c,0.05f);
    entry.limited=limited;
    vr::HandHaptic(side,0.25f);
}

void RenderVrPouch(CActor& actor)
{
    if (&actor!=Level().CurrentControlEntity() || !g_vr_owns_present) return;
    // Only while a weapon is in the hand.
    if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem())) weapon->VrRenderPouch(actor);
}

void PrepareVrWeaponSave(CActor& actor)
{
    if (!IsGameTypeSingle()) return;
    for (auto* item: actor.inventory().m_all)
        if (auto* weapon=smart_cast<CWeapon*>(item)) weapon->VrPrepareSnapshot();
}

// World items stay world objects until the player places one over the
// shoulder. The normal pickup event retains inventory and script authority.
// Kept separate so its early returns never swallow the empty-handed use.
// Each free hand grabs, carries and stores its own item.
static void UpdateVrPickup(CActor& actor, int side, bool available, bool trigger, bool may_grab, bool at_weapon)
{
    auto& state=actor.m_vr_pickup;
    u16& grabbed_id=state.grabbed_id[side];
    Fmatrix& grab_offset=state.grab_offset[side];
    Fvector& previous_grab_position=state.previous_grab_position[side];
    bool& pickup_armed=state.pickup_armed[side];
    Fmatrix hand,aim;
    const bool valid=available && actor.g_Alive() && !actor.Holder() && !actor.Remote() &&
        actor.cam_Active()==actor.cam_FirstEye() && vr::GameplayActive() &&
        !(CurrentGameUI() && CurrentGameUI()->TopInputReceiver()) &&
        vr::HandTransform(side,false,hand) && vr::HandTransform(side,true,aim);
    auto* grabbed=grabbed_id!=u16(-1) ? smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(grabbed_id)) : nullptr;
    if (!grabbed || grabbed->getDestroy() || grabbed->H_Parent() || !grabbed->PPhysicsShell() ||
        !grabbed->PPhysicsShell()->isActive())
    { grabbed_id=u16(-1); grabbed=nullptr; }
    if (!valid) { grabbed_id=u16(-1); pickup_armed=false; return; }
    const bool down=vr::HandSqueeze(side)>0.65f || trigger;
    if (!down) pickup_armed=true;
    if (grabbed)
    {
        if (!down) { grabbed_id=u16(-1); return; }
        vr::HideHandForFrame(side);
        Fmatrix target; target.mul_43(hand,grab_offset);
        // With physical hands, carried into a wall it stops at the wall
        // rather than passing to the other side to be let go there.
        if (vr::PhysicalHands()) VrNearSide(target.c,0.05f);
        // The hand's real velocity (VrHandMotion): a snap turn moves the
        // item with the hand but must not fling it when let go.
        Fvector velocity=state.hand_velocity[side];
        if (velocity.square_magnitude()>16.f) velocity.normalize_safe().mul(4.f);
        previous_grab_position=target.c;
        grabbed->PPhysicsShell()->SetGlTransformDynamic(target);
        grabbed->PPhysicsShell()->set_LinearVel(velocity);
        grabbed->XFORM().set(target);
        grabbed->spatial_move();
        // Over the shoulder on the hand's side, at the tracked head's height.
        const Fvector shoulder=VrShoulderPoint(actor,side==1);
        // Ammunition carried to the ammunition pouch goes into it, as over
        // the shoulder.
        const bool to_pouch=smart_cast<CWeaponAmmo*>(grabbed) && Near(hand.c,VrPouchPoint(actor),vr_pouch_reach);
        if (to_pouch || Near(hand.c,shoulder,0.29f))
        {
            auto* item=grabbed->cast_inventory_item();
            if (item && actor.inventory().CanTakeItem(item))
            {
                grabbed_id=u16(-1);
                pickup_armed=false;
                Game().SendPickUpEvent(actor.ID(),grabbed->ID());
                vr::HandHaptic(side,0.22f);
            }
        }
        return;
    }
    // A squeeze that starts where grabbing is not allowed (at the weapon or
    // the equipment) grabs nothing, even once the hand moves away.
    if (down && !may_grab) pickup_armed=false;
    if (down && pickup_armed && may_grab)
    {
        pickup_armed=false;
        // Not through a wall. Only the static world blocks: the item under
        // the hand, or the weapon in the other hand, is not a wall.
        Fvector to_hand; to_hand.sub(hand.c,actor.cam_FirstEye()->vPosition);
        const float reach=to_hand.magnitude(); collide::rq_result blocked;
        if (reach>1.5f || (reach>EPS_S && Level().ObjectSpace.RayPick(actor.cam_FirstEye()->vPosition,to_hand.div(reach),reach,collide::rqtStatic,blocked,&actor)))
        {
            if (vr::ContactDebug()>0) Msg("* [VR pickup] hand %d: out of reach (%.2f m) or behind a wall",side,reach);
            return;
        }
        const auto takeable=[&](CPhysicsShellHolder* object)
        {
            auto* item=object ? object->cast_inventory_item() : nullptr;
            return item && item->CanTake() && object->PPhysicsShell() && object->PPhysicsShell()->isActive() &&
                !object->H_Parent() && object->getVisible() && !object->getDestroy() &&
                object->ID()!=state.grabbed_id[1-side] && actor.inventory().CanTakeItem(item);
        };
        // The item the hand is on (within 8 cm of its bounds) first, then
        // what the hand points at.
        CPhysicsShellHolder* item_object=nullptr;
        bool touching=false;
        {
            xr_vector<ISpatial*> nearby;
            g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,hand.c,0.4f);
            float best=0.08f;
            for (ISpatial* spatial:nearby)
            {
                auto* candidate=smart_cast<CPhysicsShellHolder*>(spatial->dcast_CObject());
                if (!takeable(candidate) || !candidate->Visual()) continue;
                Fmatrix to_item; to_item.invert(candidate->XFORM());
                Fvector local; to_item.transform_tiny(local,hand.c);
                const Fbox& box=candidate->Visual()->getVisData().box;
                Fvector inside; inside.set(clampr(local.x,box.min.x,box.max.x),
                    clampr(local.y,box.min.y,box.max.y),clampr(local.z,box.min.z,box.max.z));
                const float distance=inside.distance_to(local);
                if (distance<best) { best=distance; item_object=candidate; }
            }
            touching=item_object!=nullptr;
        }
        // Close to the weapon hand only an item the hand is on is taken; a
        // squeeze there otherwise is for the weapon.
        if (at_weapon && !touching)
        {
            if (vr::ContactDebug()>0) Msg("* [VR pickup] hand %d: at the weapon, no item under the hand",side);
            return;
        }
        collide::rq_result hit;
        if (!item_object && Level().ObjectSpace.RayPick(aim.c,aim.k,2.f,collide::rqtBoth,hit,&actor))
        {
            auto* candidate=smart_cast<CPhysicsShellHolder*>(hit.O);
            if (takeable(candidate)) item_object=candidate;
        }
        if (!item_object)
        {
            if (vr::ContactDebug()>0) Msg("* [VR pickup] hand %d: nothing takeable under or ahead of the hand",side);
            return;
        }
        ::luabind::functor<bool> allow;
        if (ai().script_engine().functor("bind_stalker_ext.actor_on_item_before_pickup",allow) && !allow(item_object->lua_game_object())) return;
        // A slot weapon taken with an empty hand goes straight into that
        // hand (see UpdateVrWeapons) rather than being carried.
        auto* weapon=smart_cast<CWeapon*>(item_object);
        if (weapon && !actor.inventory().ActiveItem() &&
            (weapon->BaseSlot()==INV_SLOT_2 || weapon->BaseSlot()==INV_SLOT_3))
        {
            state.wield_id=weapon->ID();
            state.wield_hand=side;
            state.wield_expires=Device.dwTimeGlobal+3000;
            Game().SendPickUpEvent(actor.ID(),weapon->ID());
            vr::HandHaptic(side,0.2f);
            return;
        }
        // Taken where it is when the hand is on it; pulled into the hand
        // from a distance.
        Fmatrix inverse; inverse.invert(hand);
        grab_offset.mul_43(inverse,item_object->XFORM());
        if (!touching) grab_offset.c.set(0,0,0.09f);
        grabbed_id=item_object->ID();
        previous_grab_position=item_object->Position();
        vr::HandHaptic(side,0.12f);
    }
}

// Hand and weapon bodies push loose physics objects only: never the static
// world, a character (the player's body included), a character's controller,
// or each other.
static void VrWeaponContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*);
// With vr_physical_hands 2 hands also meet the static world (walls, tables)
// and are drawn where their bodies are, so a hand stops on a surface; with 1
// they push loose objects only and never snag on a door frame.
static void VrHandContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*)
{
    const dGeomID other=bo1 ? c.geom.g2 : c.geom.g1;
    dxGeomUserData* data=retrieveGeomUserData(other);
    if (data && data->object_callbacks && (data->object_callbacks->HasCallback(VrHandContact) ||
        data->object_callbacks->HasCallback(VrWeaponContact)))
    { do_colide=false; return; }
    if (!dGeomGetBody(other)) { if (vr::PhysicalHands()<2) do_colide=false; return; }
    IPhysicsShellHolder* owner=data ? data->ph_ref_object : nullptr;
    if (owner && (owner->IsActor() || owner->IsStalker() || !owner->ObjectPPhysicsShell()))
        do_colide=false;
}
// The weapon body pushes loose objects only; the gun is drawn at the hand.
static void VrWeaponContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*)
{
    const dGeomID other=bo1 ? c.geom.g2 : c.geom.g1;
    if (!dGeomGetBody(other)) { do_colide=false; return; }
    dxGeomUserData* data=retrieveGeomUserData(other);
    if (data && data->object_callbacks && (data->object_callbacks->HasCallback(VrHandContact) ||
        data->object_callbacks->HasCallback(VrWeaponContact)))
    { do_colide=false; return; }
    IPhysicsShellHolder* owner=data ? data->ph_ref_object : nullptr;
    if (owner && (owner->IsActor() || owner->IsStalker() || !owner->ObjectPPhysicsShell()))
        do_colide=false;
}

bool VrHandBodyPosition(CActor& actor, int side, Fvector& position)
{
    // Only hands that stop on the world are drawn at their bodies.
    if (vr::PhysicalHands()<2) return false;
    CPhysicsShell* body=side>=0 && side<2 ? actor.m_vr_pickup.hand_body[side] : nullptr;
    if (!body) return false;
    body->GetGlobalPositionDynamic(&position);
    return true;
}

void ReleaseVrHandBodies(CActor& actor)
{
    auto& state=actor.m_vr_pickup;
    for (CPhysicsShell*& body:state.hand_body)
        if (body) { destroy_physics_shell(body); body=nullptr; }
    if (state.weapon_body) { destroy_physics_shell(state.weapon_body); state.weapon_body=nullptr; }
    state.weapon_body_id=u16(-1);
}

// A body that follows a tracked pose by velocity (linear and angular), so
// what it touches is pushed rather than passed through. Far behind (walking,
// a snap turn, a blocked push) it is put back.
static void FollowPose(CPhysicsShell* body, const Fmatrix& target, bool put_back=false)
{
    if (!body->isEnabled()) body->Enable();
    Fmatrix current; body->GetGlobalTransformDynamic(&current);
    const Fvector still{0.f,0.f,0.f};
    Fvector velocity; velocity.sub(target.c,current.c);
    // Put back when the body turned this frame (the hands did not move) or
    // when far behind (a teleport): a hand held against a wall stays there.
    if (put_back || velocity.magnitude()>0.6f || Device.fTimeDelta<0.001f)
    {
        body->SetTransform(target,mh_clear);
        body->set_LinearVel(still);
        body->set_AngularVel(still);
        return;
    }
    velocity.div(Device.fTimeDelta);
    if (velocity.magnitude()>15.f) velocity.normalize_safe().mul(15.f);
    body->set_LinearVel(velocity);
    // The rotation from the current to the target orientation, per second.
    Fmatrix from=current; from.c.set(0,0,0);
    Fmatrix to=target; to.c.set(0,0,0);
    Fmatrix back; back.transpose(from);
    Fmatrix turn; turn.mul_43(to,back);
    Fquaternion q; q.set(turn);
    Fvector axis; float angle=0.f;
    Fvector spin=still;
    if (q.get_axis_angle(axis,angle))
    {
        if (angle>PI) angle-=PI_MUL_2;
        spin.set(axis).mul(clampr(angle/Device.fTimeDelta,-30.f,30.f));
    }
    body->set_AngularVel(spin);
}

static CPhysicsShell* CreateFollower(const Fmatrix& pose, CPhysicsElement* element, float mass,
    ObjectContactCallbackFun* contact)
{
    CPhysicsShell* body=P_create_Shell();
    body->add_Element(element);
    body->setMass(mass);
    body->set_PhysicsRefObject(nullptr);
    const Fvector still{0.f,0.f,0.f};
    body->Activate(pose,still,still);
    body->set_ApplyByGravity(false);
    body->SetAirResistance(0.f,0.f);
    body->set_DynamicLimits(12.f,40.f);
    body->EnableCollision();
    body->set_ObjectContactCallback(contact);
    return body;
}

// Each tracked hand not carrying anything has a 4.5 cm sphere at the palm,
// and a held weapon a box around its mesh: both push boxes, bottles and
// bodies. Physics objects near the hands are woken so they respond.
static void UpdateVrHandBodies(CActor& actor, const bool tracked[2], const Fmatrix hands[2])
{
    auto& state=actor.m_vr_pickup;
    const int mode=vr::PhysicalHands();
    for (int side=0;side<2;++side)
    {
        CPhysicsShell*& body=state.hand_body[side];
        const bool wanted=mode>0 && tracked[side] && state.grabbed_id[side]==u16(-1) && state.climb_hand!=side;
        if (!wanted)
        {
            if (body) { destroy_physics_shell(body); body=nullptr; }
            continue;
        }
        Fmatrix target; target.identity(); target.c=hands[side].c;
        if (!body)
        {
            CPhysicsElement* element=P_create_Element();
            Fsphere sphere; sphere.P.set(0,0,0); sphere.R=0.05f;
            element->add_Sphere(sphere);
            body=CreateFollower(target,element,6.f,VrHandContact);
            continue;
        }
        // A hand held back by the world (a door frame) for a quarter of a
        // second while the real hand is well past it goes back to the hand.
        Fvector at; body->GetGlobalPositionDynamic(&at);
        if (at.distance_to(target.c)>0.12f) state.hand_blocked[side]+=Device.fTimeDelta;
        else state.hand_blocked[side]=0.f;
        if (state.hand_blocked[side]>0.25f)
        {
            body->SetTransform(target,mh_clear);
            state.hand_blocked[side]=0.f;
        }
        FollowPose(body,target,state.body_turned);
        // Wake what the hand reaches.
        xr_vector<ISpatial*> nearby;
        g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,hands[side].c,0.35f);
        for (ISpatial* spatial:nearby)
            if (auto* holder=smart_cast<CPhysicsShellHolder*>(spatial->dcast_CObject()))
                if (!holder->H_Parent() && holder->PPhysicsShell() && holder->PPhysicsShell()->isActive() &&
                    !holder->PPhysicsShell()->isEnabled() && !smart_cast<CActor*>(holder))
                    holder->PPhysicsShell()->Enable();
    }
    // The held weapon: its measured mesh, receiver and barrel as two boxes.
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    Fmatrix world;
    const xr_vector<Fobb>* boxes=nullptr;
    const bool wanted=vr::PhysicalWeapon() && weapon && weapon->VrCollisionBoxes(world,boxes) &&
        boxes && !boxes->empty();
    if (state.weapon_body && (!wanted || state.weapon_body_id!=weapon->ID()))
    {
        destroy_physics_shell(state.weapon_body);
        state.weapon_body=nullptr;
        state.weapon_body_id=u16(-1);
    }
    if (!wanted) return;
    if (!state.weapon_body)
    {
        CPhysicsElement* element=P_create_Element();
        for (const Fobb& box:*boxes) element->add_Box(box);
        state.weapon_body=CreateFollower(world,element,10.f,VrWeaponContact);
        state.weapon_body_id=weapon->ID();
        Msg("* [VR] %s: weapon collision, %u box(es), first %.2f x %.2f x %.2f m",
            weapon->cNameSect().c_str(),unsigned(boxes->size()),(*boxes)[0].m_halfsize.x*2.f,
            (*boxes)[0].m_halfsize.y*2.f,(*boxes)[0].m_halfsize.z*2.f);
        return;
    }
    FollowPose(state.weapon_body,world,state.body_turned);
    // Wake what the weapon reaches, along its length.
    for (const Fobb& box:*boxes)
    {
        Fvector centre; world.transform_tiny(centre,box.m_translate);
        xr_vector<ISpatial*> nearby;
        g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,centre,box.m_halfsize.magnitude()+0.2f);
        for (ISpatial* spatial:nearby)
            if (auto* holder=smart_cast<CPhysicsShellHolder*>(spatial->dcast_CObject()))
                if (!holder->H_Parent() && holder->PPhysicsShell() && holder->PPhysicsShell()->isActive() &&
                    !holder->PPhysicsShell()->isEnabled() && !smart_cast<CActor*>(holder))
                    holder->PPhysicsShell()->Enable();
    }
}

// What a hand's pointer ray rests on within use range, provided the hand is
// not reaching through a wall from the head.
static bool VrPointerPick(CActor& actor, int side, collide::rq_result& hit)
{
    Fmatrix hand, aim;
    if (!vr::HandTransform(side,false,hand) || !vr::HandTransform(side,true,aim)) return false;
    Fvector to_hand; to_hand.sub(hand.c,actor.cam_FirstEye()->vPosition);
    const float reach=to_hand.magnitude(); collide::rq_result blocked;
    if (reach>1.5f || (reach>EPS_S && Level().ObjectSpace.RayPick(actor.cam_FirstEye()->vPosition,
        to_hand.div(reach),reach,collide::rqtBoth,blocked,&actor))) return false;
    return Level().ObjectSpace.RayPick(aim.c,aim.k,2.f,collide::rqtBoth,hit,&actor) && hit.O;
}

// Hand-over-hand climbing: a free hand closing on a ladder holds it, and
// while held, pulling the hand down lifts the body (and pushing it up lowers
// it). Returns the vertical speed, or 0 when no hand holds a ladder.
static void UpdateVrClimb(CActor& actor, const bool available[2], const bool grip[2],
    const bool grip_edge[2])
{
    auto& state=actor.m_vr_pickup;
    int& side=state.climb_hand;
    if (side>=0 && (!available[side] || !grip[side])) side=-1;
    for (int h=1; h>=0; --h)
    {
        if (!available[h] || !grip_edge[h] || h==side || state.grabbed_id[h]!=u16(-1)) continue;
        Fmatrix hand;
        if (!vr::HandTransform(h,false,hand)) continue;
        for (CClimableObject* ladder:ClimableObjects())
        {
            if (!ladder || ladder->Position().distance_to_sqr(hand.c)>
                _sqr(ladder->Axis().magnitude()+ladder->Side().magnitude()+0.5f)) continue;
            // The hand within the ladder's box (Axis, Side and Norm are its
            // half-extents from the centre), with room to reach in front of it.
            Fvector to; to.sub(hand.c,ladder->Position());
            const Fvector* extents[3]={&ladder->Axis(),&ladder->Side(),&ladder->Norm()};
            const float margin[3]={0.3f,0.3f,0.6f};
            bool inside=extents[0]->square_magnitude()>EPS_S;
            for (int e=0;e<3 && inside;++e)
            {
                const float length=extents[e]->magnitude();
                if (length<EPS_S) continue;
                inside=_abs(to.dotproduct(*extents[e]))/length<=length+margin[e];
            }
            if (!inside) continue;
            side=h;
            state.climb_ladder=ladder->ID();
            state.climb_previous=hand.c.y-actor.Position().y;
            vr::HandHaptic(h,0.15f);
            break;
        }
        if (side!=h)
            for (CClimableObject* ladder:ClimableObjects())
                if (ladder && ladder->Position().distance_to(hand.c)<3.f)
                {
                    // A grab near a ladder that missed: where the hand was in
                    // the ladder's frame, against its half-extents.
                    Fvector to; to.sub(hand.c,ladder->Position());
                    Msg("* [VR] ladder %u missed: hand at %.2f/%.2f/%.2f, half %.2f/%.2f/%.2f",
                        ladder->ID(),
                        to.dotproduct(Fvector(ladder->Axis()).normalize_safe()),
                        to.dotproduct(Fvector(ladder->Side()).normalize_safe()),
                        to.dotproduct(Fvector(ladder->Norm()).normalize_safe()),
                        ladder->Axis().magnitude(),ladder->Side().magnitude(),ladder->Norm().magnitude());
                    break;
                }
    }
    if (side<0) return;
    Fmatrix hand;
    if (!vr::HandTransform(side,false,hand) || Device.fTimeDelta<0.001f) return;
    const float height=hand.c.y-actor.Position().y;
    // The body follows the hand's pull exactly (CActor::VrClimbLadder).
    actor.m_vr_climb_delta+=clampr(state.climb_previous-height,-0.15f,0.15f);
    state.climb_previous=height;
}

void VrSnapHeldItems(CActor& actor)
{
    if (!g_vr_owns_present) return;
    auto& state=actor.m_vr_pickup;
    for (int side=0;side<2;++side)
    {
        if (state.grabbed_id[side]==u16(-1)) continue;
        auto* held=smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(state.grabbed_id[side]));
        Fmatrix hand;
        if (!held || held->getDestroy() || held->H_Parent() || !vr::HandTransform(side,false,hand)) continue;
        held->XFORM().mul_43(hand,state.grab_offset[side]);
        held->spatial_move();
    }
}

// The use action as the bound key through the level, so script key handlers
// (doors, special and generic actions) see it as they do on the desktop; the
// level passes it on to the actor (talking, pickup). Without a binding, to
// the actor only.
static void VrUseKey(CActor& actor, bool press)
{
    if (const int dik=get_action_dik(kUSE))
    {
        if (press) Level().IR_OnKeyboardPress(dik); else Level().IR_OnKeyboardRelease(dik);
        return;
    }
    if (press) actor.IR_OnKeyboardPress(kUSE); else actor.IR_OnKeyboardRelease(kUSE);
}

void UpdateVrWeapons(CActor& actor)
{
    if (&actor != Level().CurrentControlEntity() || !IsGameTypeSingle()) return;
    // Inventory weapons can stop receiving UpdateCL while holstered. Their
    // temporary physics props still need their lifetime serviced every frame.
    for (auto* item : actor.inventory().m_all)
        if (auto* weapon=item->cast_weapon()) weapon->VrUpdateProps();
    auto& pickup=actor.m_vr_pickup;
    if (!g_vr_owns_present)
    {
        ReleaseVrHandBodies(actor);
        if (pickup.use_pressed) VrUseKey(actor,false);
        if (pickup.native_reload_down) actor.IR_OnKeyboardRelease(kWPN_RELOAD);
        for (int h=0; h<2; ++h) pickup.use_trigger_down[h]=pickup.weapon_grip_armed[h]=false;
        pickup.pocket_held[0]=pickup.pocket_held[1]=-1;
        pickup.use_pressed=false;
        pickup.native_reload_down=false;
        pickup.native_reload_armed=false;
        pickup.pick_id=u16(-1);
        pickup.climb_hand=-1;
        actor.m_vr_climb_delta=0.f;
        for (auto* item : actor.inventory().m_all)
            if (auto* weapon=item->cast_weapon()) weapon->VrInteract(actor);
        return;
    }
    VrPdaUpdate(actor);
    const bool controls_valid=vr::GameplayActive() && actor.g_Alive() &&
        !actor.Holder() && !actor.Remote() && !actor.IsTalking() &&
        actor.cam_Active()==actor.cam_FirstEye() &&
        !(CurrentGameUI() && CurrentGameUI()->TopInputReceiver());
    // Either hand may draw, hold and fire; the other supports. Arrays below
    // are indexed by physical hand (0 left, 1 right).
    Fmatrix hands[2];
    bool tracked[2], grip[2], grip_edge[2];
    // The body's placement, to measure hand motion in its frame (the
    // player's real hand motion; see VrHandMotion), and whether it turned
    // this frame (a snap turn swings every world-space hand pose about the
    // head: bodies following the hands are put back rather than flung).
    Fmatrix body;
    const bool body_valid=controls_valid && vr::BodyTransform(body);
    pickup.body_turned=body_valid && pickup.body_k_valid && pickup.body_k.dotproduct(body.k)<_cos(deg2rad(2.f));
    pickup.body_k_valid=body_valid;
    if (body_valid) pickup.body_k=body.k;
    const Fvector carried=VrCarriedVelocity(actor);
    for (int h=0; h<2; ++h)
    {
        tracked[h]=controls_valid && vr::HandTransform(h,false,hands[h]);
        grip[h]=tracked[h] && vr::GripHeld(vr::HandSqueeze(h),pickup.grip_held[h]);
        grip_edge[h]=grip[h] && !pickup.grip_held[h];
        pickup.grip_held[h]=grip[h];
        Fmatrix local;
        const bool local_valid=tracked[h] && body_valid && vr::HandInBody(h,false,local);
        // Smoothed hand velocity, used when a released weapon is thrown.
        if (local_valid && pickup.hand_valid[h] && Device.fTimeDelta>0.001f)
        {
            Fvector velocity, angular;
            VrHandMotion(pickup.hand_previous[h],local,body,carried,Device.fTimeDelta,velocity,angular);
            if (velocity.magnitude()>15.f) velocity.normalize_safe().mul(15.f);
            if (angular.magnitude()>30.f) angular.normalize_safe().mul(30.f);
            pickup.hand_velocity[h].lerp(pickup.hand_velocity[h],velocity,0.5f);
            pickup.hand_angular[h].lerp(pickup.hand_angular[h],angular,0.5f);
        }
        else if (!local_valid) { pickup.hand_velocity[h].set(0,0,0); pickup.hand_angular[h].set(0,0,0); }
        pickup.hand_valid[h]=local_valid;
        if (local_valid) pickup.hand_previous[h]=local;
    }
    // A weapon picked up by hand is wielded in that hand once the pickup
    // lands, in its slot (making room there if another weapon holds it).
    if (pickup.wield_id!=u16(-1))
    {
        auto* weapon=smart_cast<CWeapon*>(Level().Objects.net_Find(pickup.wield_id));
        if (!weapon || int(Device.dwTimeGlobal-pickup.wield_expires)>0 || actor.inventory().ActiveItem())
            pickup.wield_id=u16(-1);
        else if (weapon->H_Parent()==&actor)
        {
            const u16 base=weapon->BaseSlot();
            if (weapon->CurrSlot()!=base && (base==INV_SLOT_2 || base==INV_SLOT_3))
            {
                if (PIItem occupant=actor.inventory().ItemFromSlot(base)) actor.inventory().Ruck(occupant);
                actor.inventory().Slot(base,weapon,true);
            }
            if (weapon->CurrSlot()==INV_SLOT_2 || weapon->CurrSlot()==INV_SLOT_3)
            {
                weapon->VrSetWeaponHand(pickup.wield_hand);
                actor.inventory().Activate(weapon->CurrSlot(),true);
                pickup.gripped_weapon_id=weapon->ID();
                vr::HandHaptic(pickup.wield_hand,0.2f);
            }
            pickup.wield_id=u16(-1);
        }
    }
    // A thrown weapon gets the hand's motion once its world physics is live.
    for (auto& entry:pickup.thrown)
    {
        if (entry.id==u16(-1)) continue;
        auto* thrown=smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(entry.id));
        if (!thrown || int(Device.dwTimeGlobal-entry.expires)>0) entry.id=u16(-1);
        else if (!thrown->H_Parent() && thrown->PPhysicsShell() && thrown->PPhysicsShell()->isActive())
        {
            if (entry.place)
            {
                thrown->PPhysicsShell()->SetGlTransformDynamic(entry.matrix);
                thrown->XFORM().set(entry.matrix);
                thrown->spatial_move();
                if (auto* bolt=smart_cast<CBolt*>(thrown)) bolt->VrPrepareThrown(entry.limited);
            }
            thrown->PPhysicsShell()->set_LinearVel(entry.velocity);
            thrown->PPhysicsShell()->set_AngularVel(entry.angular);
            entry.id=u16(-1);
            entry.place=false;
        }
    }
    // A bolt slot emptied by a throw takes the next bolt carried (a new one
    // for unlimited bolts, once it has arrived).
    if (pickup.bolt_refill_until)
    {
        if (actor.inventory().ItemFromSlot(BOLT_SLOT) || int(Device.dwTimeGlobal-pickup.bolt_refill_until)>0)
            pickup.bolt_refill_until=0;
        else
            for (PIItem item:actor.inventory().m_ruck)
                if (smart_cast<CBolt*>(item))
                {
                    NET_Packet P;
                    item->object().u_EventGen(P,GEG_PLAYER_ITEM2SLOT,actor.ID());
                    P.w_u16(item->object().ID());
                    P.w_u16(BOLT_SLOT);
                    P.w_u8(1); // not activated
                    item->object().u_EventSend(P);
                    pickup.bolt_refill_until=0;
                    break;
                }
    }
    // Holster draw: each empty hand closing on a holster takes its weapon,
    // which then belongs to that hand. One held grip claims one weapon.
    bool drawn=false;
    VrBodySlotHover(actor,tracked,hands);
    for (int h=1; h>=0; --h)
    {
        // Activation is deferred: one draw per frame, or both hands could
        // claim a holster in the same frame and one would come up empty.
        const bool active=drawn || smart_cast<CWeapon*>(actor.inventory().ActiveItem())!=nullptr;
        CWeapon* closest=nullptr;
        // A squeeze that starts within a holster's reach draws; a hand
        // arriving there already squeezed does not.
        const bool fresh_squeeze=tracked[h] && grip[h] && grip_edge[h] && pickup.weapon_grip_armed[h] &&
            pickup.grabbed_id[h]==u16(-1) && pickup.climb_hand!=h;
        float best=FLT_MAX;
        if (fresh_squeeze && !active)
        {
            for (u16 slot : {INV_SLOT_2,INV_SLOT_3})
                if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot)))
                {
                    float distance=0.f;
                    if (weapon->VrHolsterGrabDistance(actor,hands[h].c,distance) && distance<best)
                    { closest=weapon; best=distance; }
                }
        }
        // Body slots (knife, grenade, detector, PDA...) where they are nearer
        // than a holster. Detector, PDA and inventory work beside a held gun.
        float body_distance=FLT_MAX;
        const int body=fresh_squeeze && !drawn ? VrBodySlotCandidate(actor,h,hands[h].c,body_distance) : -1;
        if (body>=0 && body_distance<best)
        {
            pickup.weapon_grip_armed[h]=false;
            if (VrBodySlotTake(actor,body,h)) drawn=true;
            continue;
        }
        if (vr::HolsterGrab(pickup.weapon_grip_armed[h],tracked[h],grip[h],active,closest!=nullptr))
        {
            closest->VrSetWeaponHand(h);
            actor.inventory().Activate(closest->CurrSlot(),true);
            drawn=true;
            vr::HandHaptic(h,0.2f);
        }
    }
    // Quick-use items taken from the chest pockets; grenades and bolts.
    VrPocketUpdate(actor,tracked,hands,grip);
    VrMissileUpdate(actor,tracked,hands,grip);
    VrHandItemsUpdate(actor,tracked,grip);
    VrBinocularsUpdate(actor);
    if (controls_valid) VrHeadGestureUpdate(actor,tracked,hands);
    // Sample holstered weapons' first-person grips ahead of the draw, one a
    // second, so drawing does not stall on it.
    if (Device.dwFrame%60==0)
        for (u16 slot : {INV_SLOT_2,INV_SLOT_3})
            if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot)))
                if (weapon!=actor.inventory().ActiveItem() && weapon->VrPrepare()) break;
    auto* held_weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const int weapon_hand=held_weapon ? held_weapon->VrWeaponHand() : -1;
    // The support hand's secondary button, or the weapon hand's when the
    // weapon has no physical magazine release, invokes native reload,
    // including tri-state and double-barrel shotguns.
    auto& reload_down=pickup.native_reload_down;
    auto& reload_armed=pickup.native_reload_armed;
    const int support_hand=held_weapon ? 1-weapon_hand : 0;
    // The support hand in the ammunition pouch: its upper face button (Y,
    // or B left-handed) changes the rounds the pouch gives, as the desktop's
    // next-ammunition key does; the next magazine or shell taken (or the
    // next native reload) holds them. A press begun there is not the reload
    // button, even once the hand has left the pouch with it still held.
    const bool in_pouch=held_weapon && tracked[support_hand] && !VrHandHoldsItem(actor,support_hand) &&
        hands[support_hand].c.distance_to(VrPouchPoint(actor))<=vr_pouch_reach;
    const bool support_button=controls_valid && vr::HandSecondaryButton(support_hand);
    if (support_button && in_pouch && !pickup.pouch_swap_down && !reload_down)
    {
        pickup.pouch_swap_down=true;
        vr::HandHaptic(support_hand,held_weapon->VrSwapPouchAmmo() ? 0.15f : 0.04f);
    }
    if (!support_button) pickup.pouch_swap_down=false;
    // Weapons reloaded by hand (magazines, shells) have no button reload.
    // A hand at the helmet uses its button for night vision instead.
    const auto button_free=[&](int h){ return h>=0 && h!=pickup.device_hand && !(tracked[h] && VrHandAtHead(hands[h].c)); };
    const bool reload_button=controls_valid && !(held_weapon && held_weapon->VrPhysicalReload()) &&
        ((vr::HandSecondaryButton(support_hand) && button_free(support_hand) && !pickup.pouch_swap_down) ||
        (held_weapon && vr::HandSecondaryButton(weapon_hand) && button_free(weapon_hand) &&
        !held_weapon->VrUsesMagazineButton()));
    if (reload_down && !reload_button)
    { actor.IR_OnKeyboardRelease(kWPN_RELOAD); reload_down=false; }
    if (!controls_valid) reload_armed=false;
    else if (!reload_button) reload_armed=true;
    else if (reload_armed)
    {
        reload_armed=false;
        if (held_weapon && !held_weapon->VrBlockReload())
        {
            Msg("* [VR weapon] %s: reload requested (state=%u pending=%d ammo=%d/%d)",
                held_weapon->cNameSect().c_str(),held_weapon->GetState(),
                held_weapon->IsPending(),held_weapon->GetAmmoElapsed(),
                held_weapon->GetAmmoMagSize());
            actor.IR_OnKeyboardPress(kWPN_RELOAD);
            reload_down=true;
            vr::HandHaptic(support_hand,0.10f);
        }
    }
    for (u16 slot=actor.inventory().FirstSlot(); slot<=actor.inventory().LastSlot(); ++slot)
    {
        auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot));
        if (!weapon) continue;
        weapon->VrReleaseInactiveHeld();
        // Both slot weapons need the grip check even when the other is active.
        // Each interaction owns its state; only the equipped one can fire.
        weapon->VrInteract(actor);
    }
    // A weapon drawn without a grip (inventory, keyboard, a loaded save, a
    // pickup) stays in its hand; letting go of a held grip releases it (see
    // the interaction).
    auto* held_weapon_now=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    held_weapon=held_weapon_now;
    const int held_hand=held_weapon ? held_weapon->VrWeaponHand() : 1;
    // A hand is free unless it holds the weapon, or supports it while the
    // weapon needs it (magazine, slide, fore-end, shells).
    bool available[2], reach[2], grab[2], at_weapon[2], trigger[2];
    for (int h=0; h<2; ++h)
    {
        available[h]=tracked[h] && !actor.IsTalking() &&
            !(held_weapon && (h==held_hand || held_weapon->VrLeftHandBusy()));
        // A hand at the weapon (on its slide, fore-end, magazine) works the
        // weapon: it starts no grab and no use, though it keeps what it carries.
        // A hand holding a pocket item uses its trigger for that item; one
        // holding a grenade or bolt does not point or grab.
        // A hand at the equipment takes from it, and does not grab or use
        // what it points at (unless it is already carrying an item there).
        // Close to the weapon hand it still grabs an item it is on (one on
        // the ground beside the weapon), but points at and uses nothing.
        grab[h]=available[h] && !VrPocketHeld(actor,h) && h!=VrMissileHand(actor) &&
            h!=pickup.device_hand && h!=pickup.slot_hold_hand && h!=pickup.bolt_hand &&
            (pickup.grabbed_id[h]!=u16(-1) || !VrHandAtEquipment(actor,hands[h].c));
        at_weapon[h]=held_weapon && tracked[held_hand] &&
            hands[h].c.distance_to(hands[held_hand].c)<0.35f;
        reach[h]=grab[h] && !at_weapon[h];
        trigger[h]=vr::HandTrigger(h)>0.65f;
    }
    // Ladders: a free hand closing on one climbs it.
    UpdateVrClimb(actor,available,grip,grip_edge);
    // Physical hands push loose physics objects.
    UpdateVrHandBodies(actor,tracked,hands);
    if (pickup.climb_hand>=0) available[pickup.climb_hand]=false;
    // The desktop F action: a free hand's trigger, on what that hand points
    // at. The last hand that pulled its trigger points; otherwise the right.
    for (int h=0; h<2; ++h)
        if (reach[h] && trigger[h] && !pickup.use_trigger_down[h]) pickup.use_hand=h;
    if (pickup.use_hand<0 || !reach[pickup.use_hand])
        pickup.use_hand=reach[1] ? 1 : (reach[0] ? 0 : -1);
    const int pointer=pickup.use_hand;
    collide::rq_result hit;
    if (pointer>=0 && pickup.grabbed_id[pointer]==u16(-1) && VrPointerPick(actor,pointer,hit))
    { pickup.pick_id=hit.O->ID(); pickup.pick_range=hit.range; pickup.pick_element=hit.element; }
    else pickup.pick_id=u16(-1);
    const bool can_use=pointer>=0 && pickup.grabbed_id[pointer]==u16(-1);
    const bool use_trigger=can_use && trigger[pointer];
    // Track the physical trigger even while armed to avoid a spurious use
    // event when the weapon is holstered with the trigger still held.
    if (pickup.use_pressed && !use_trigger)
    { VrUseKey(actor,false); pickup.use_pressed=false; }
    const bool pending_use=use_trigger && !pickup.use_trigger_down[pointer];
    for (int h=0; h<2; ++h) pickup.use_trigger_down[h]=trigger[h];
    for (int h=0; h<2; ++h) UpdateVrPickup(actor,h,available[h],grab[h] && trigger[h],grab[h],at_weapon[h]);
    // A pulled trigger grabs a pointed physical item first. When nothing can
    // be grabbed it retains the ordinary F/use action.
    if (pending_use && pickup.grabbed_id[pointer]==u16(-1))
    { VrUseKey(actor,true); pickup.use_pressed=true; }
}
