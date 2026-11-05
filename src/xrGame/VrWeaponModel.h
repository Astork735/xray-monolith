#pragma once
// Bone, mesh and motion helpers for measuring VR weapon models: shared by
// VrWeaponProfile.cpp (derivation) and VrWeaponInteraction.cpp (handling).
// Not for use outside the VR weapon files.
#include "VrWeaponInteraction.h"
#include "VrWeaponNames.h"
#include "VrContactField.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xrEngine/bone.h"
#include "../xrEngine/EnnumerateVertices.h"

namespace vrwi
{
using vr::NameHas; using vr::NameWord; using vr::ShellName; using vr::AttachmentName; using vr::RoundName;
using vr::CartridgeEnergy; using vr::ClipSteps;
// How far behind the controller's handle centre a held magazine sits.
constexpr float kMagazineHoldBack=0.03f;
void _BCL BonePose(CBoneInstance* bone);
struct BoneBounds : public SEnumVerticesCallback
{
    Fbox box;
    unsigned count=0;
    BoneBounds() { box.invalidate(); }
    void operator()(const Fvector& p) override { box.modify(p); ++count; }
};
bool Descends(IKinematics& model, u16 id, u16 root);
// Model-space pose of a bone below a part VR draws at its bind place (a
// magazine): the part as bound, what hangs below it as the weapon's own
// idle lays it out (rest_local, parent-relative). A GAMMA RPD's
// belt links are bound half a metre behind the gun; only the idle
// brings them to the drum.
Fmatrix RestPose(IKinematics& model, const xr_vector<Fmatrix>& bind, const xr_vector<Fmatrix>& rest_local,
    u16 id, u16 root);
// Each bone's bind pose relative to its parent's (a root's as bound).
xr_vector<Fmatrix> BindLocal(IKinematics& model, const xr_vector<Fmatrix>& bind);
struct SkinnedVertices : public SEnumVerticesCallback
{
    SEnumVerticesCallback& out;
    Fmatrix skin;
    explicit SkinnedVertices(SEnumVerticesCallback& to) : out(to) { skin.identity(); }
    void operator()(const Fvector& p) override { Fvector q; skin.transform_tiny(q,p); out(q); }
};
// A bone's vertices at rest below root; bind-space without rest data.
void EnumRestVertices(IKinematics& model, SEnumVerticesCallback& out, u16 id, u16 root,
    const xr_vector<Fmatrix>* rest_local, const xr_vector<Fmatrix>& bind);
// The magazine bone's own mesh height (InMagazine's spare test).
float MagazineHeight(IKinematics& model, u16 magazine);
// height: MagazineHeight, measured here when not given (loops pass it).
bool InMagazine(IKinematics& model, u16 id, u16 magazine, float height=-1.f);
// Bind-space mesh bounds of the magazine (without nested spares), its
// contents at rest when rest_local is given.
// bind: the model's bind transforms, fetched here when not given.
bool MeasureMagazine(IKinematics& model, u16 magazine, Fbox& box, const xr_vector<Fmatrix>* rest_local=nullptr,
    const xr_vector<Fmatrix>* bind=nullptr);
// Bind-space mesh bounds of a bone, optionally with its whole subtree; with
// rest_local, what hangs below rest_root (the bone itself by default) at rest.
bool MeasureBone(IKinematics& model, u16 bone, Fbox& box, bool subtree,
    const xr_vector<Fmatrix>* rest_local=nullptr, u16 rest_root=BI_NONE, const xr_vector<Fmatrix>* bind=nullptr);
u16 FindTrigger(IKinematics& model);
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
inline vr::V3 V(const Fvector& v)
{ return vr::V3(v.x,v.y,v.z); }
inline Fvector F(const vr::V3& v)
{ Fvector r; r.set(v.x,v.y,v.z); return r; }
inline vr::Frame ToFrame(const Fmatrix& m)
{
    vr::Frame f;
    f.i=V(m.i); f.j=V(m.j); f.k=V(m.k); f.c=V(m.c);
    return f;
}
inline Fmatrix ToMatrix(const vr::Frame& f)
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
Fvector GrabPoint(IKinematics& model, u16 part, bool pistol, float centre_x);
// A private copy of a hud model, for sampling its animations.
IRenderVisual* HudCopy(LPCSTR visual);
// Plays a motion on every partition of a model, from its first frame.
void PlayOnAllParts(IKinematicsAnimated& animated, MotionID motion);
// Advances a model's animation by dt and evaluates its bones.
void EvaluateBones(IKinematicsAnimated& animated, IKinematics& model, float dt);
// A private copy of a hud model (HudCopy), deleted with this.
struct HudSample
{
    IRenderVisual* visual;
    IKinematicsAnimated* animated;
    IKinematics* sample;
    explicit HudSample(LPCSTR name): visual(HudCopy(name)),
        animated(visual ? visual->dcast_PKinematicsAnimated() : nullptr),
        sample(visual ? visual->dcast_PKinematics() : nullptr) {}
    ~HudSample() { if (visual) ::Render->model_Delete(visual); }
    HudSample(const HudSample&)=delete;
    HudSample& operator=(const HudSample&)=delete;
    // Advances its animation by dt and evaluates the bones.
    void Evaluate(float dt) { EvaluateBones(*animated,*sample,dt); }
};
// Plays a hud section's animation on a private model from its first frame.
bool PlayHudMotion(IKinematicsAnimated& animated, LPCSTR hud, LPCSTR key, float* length=nullptr);
// How far a bone is back from its bind place, measured in the receiver's
// frame (so the receiver's own recoil kick in a shot is not counted).
float RearInReceiver(const Fmatrix& receiver_now, const Fmatrix& bone_now, const xr_vector<Fmatrix>& bind,
    u16 bone, u16 receiver);
float RearInReceiver(IKinematics& sample, const xr_vector<Fmatrix>& bind, u16 bone, u16 receiver);
// The lowest part of a bone's mesh (a lever's loop), averaged over its
// bottom centimetre.
Fvector LowestPoint(IKinematics& model, u16 part);
// The weapon at rest, as its idle shows it: the item's own "idle" when the
// hud section names no item motion for it, as player_hud plays it.
bool PlayHudIdle(IKinematicsAnimated& animated, LPCSTR hud);
// Plays the weapon's reload (the empty one first); returns the key it played.
LPCSTR StartReloadMotion(IKinematicsAnimated& animated, LPCSTR hud, float* length=nullptr);
void CollapseHiddenBones(IKinematics* model);
void ReleaseModel(IRenderVisual*& visual);
// --- Names derivation reads (VrWeaponProfile.cpp) ----------------------------
// configs\vr\vr_weapon_mechanics.ltx, read once; null when not installed.
CInifile* VrMechanicsOverrides();
// A loose round's mesh for ammunition whose clip-fed model has no single
// live round to show in the hand.
struct VrLooseRound
{
    xr_string ammo, visual;  // part of the ammunition section, round mesh
    float half_length=0.028f;
};
// xr_vector has no initializer-list constructor.
inline xr_vector<xr_string> NameList(std::initializer_list<LPCSTR> parts)
{
    xr_vector<xr_string> list;
    for (LPCSTR part:parts) list.push_back(part);
    return list;
}
xr_vector<VrLooseRound> DefaultLooseRounds();
// The names derivation looks for in a model, its ammunition and its
// shaders: [vr_derivation] in that file, else these defaults.
struct VrNameTables
{
    xr_vector<xr_string> spent_case_bones=NameList({"gilza"});
    xr_vector<VrLooseRound> loose_rounds=DefaultLooseRounds();
    xr_vector<xr_string> red_dot_shaders=NameList({"light_dot","reflex"});
    xr_vector<xr_string> red_dot_textures=NameList({"red_lens","crosshair_dot"});
    xr_vector<xr_string> soft_lens_shaders=NameList({"black_soft","scope_reticle"});
    xr_vector<xr_string> glass_shaders=NameList({"lfo_","scope_color","scope_lens"});
    xr_vector<xr_string> glass_textures=NameList({"crosshair","_lense"});
};
// [vr_derivation] from the overrides file over the defaults, read once.
const VrNameTables& VrNames();
// Any of the parts in a bone or section name, ignoring case (NameHas).
bool NameHasAny(LPCSTR name, const xr_vector<xr_string>& parts);
// Any of the parts in a shader or texture name, exactly.
bool ContainsAny(LPCSTR text, const xr_vector<xr_string>& parts);

}

// Every bone's model-space pose through a HUD motion at 60 Hz from its first
// frame (step 0 at time 0), as each measurement sampled it on a copy of its
// own (HudSample).
struct VrSampledClip
{
    float length=0.f;  // as PlayHudMotion gives it; 0 when unknown
    u16 bones=0;
    xr_vector<Fmatrix> frames;  // step-major
    int Steps() const { return bones ? int(frames.size()/bones) : 0; }
    const Fmatrix& At(int step, u16 bone) const { return frames[size_t(step)*bones+bone]; }
};
// The motions measured on one weapon model, each played and evaluated once
// however many measurements read it (EnsureVisual owns one for a pass). A
// motion asked for longer than recorded is played again from its start, as
// a fresh copy would play it.
class VrClipSampler
{
public:
    VrClipSampler(LPCSTR visual, LPCSTR hud): m_copy(visual), m_hud(hud) {}
    // Its first vrwi::ClipSteps(length,unknown) steps: one play of the motion.
    const VrSampledClip* Clip(LPCSTR key, int unknown) { return Sample(key,0,unknown); }
    // Its first `steps` steps, however long it is.
    const VrSampledClip* Frames(LPCSTR key, int steps) { return Sample(key,steps,0); }
    // The weapon at rest as PlayHudIdle shows it: one step.
    const VrSampledClip* Idle() { return Sample(nullptr,1,0); }
    // The reload, the empty one first (StartReloadMotion); `key` the one played.
    const VrSampledClip* Reload(int unknown, LPCSTR& key)
    {
        for (LPCSTR candidate:{"anm_reload_empty","anm_reload"})
            if (const VrSampledClip* clip=Clip(candidate,unknown)) { key=candidate; return clip; }
        return nullptr;
    }
    // The private model, for a pose off the 60 Hz grid. Any clip asked for
    // later is played again from its start.
    vrwi::HudSample& Copy() { return m_copy; }
    u16 Bones() const { return m_copy.sample ? m_copy.sample->LL_BoneCount() : u16(0); }
private:
    struct Entry { int state=0; VrSampledClip clip; };  // state: 0 not played, 1 absent, 2 recorded
    vrwi::HudSample m_copy;
    xr_string m_hud;
    xr_map<xr_string,Entry> m_entries;
    const VrSampledClip* Sample(LPCSTR key, int fixed, int unknown)
    {
        if (!m_copy.animated || !m_copy.sample) return nullptr;
        Entry& entry=m_entries[key ? key : "#idle"];
        if (entry.state==1) return nullptr;
        VrSampledClip& clip=entry.clip;
        if (entry.state==2 && clip.Steps()>=(fixed ? fixed : vrwi::ClipSteps(clip.length,unknown))) return &clip;
        float length=0.f;
        const bool played=key ? vrwi::PlayHudMotion(*m_copy.animated,m_hud.c_str(),key,&length) :
            vrwi::PlayHudIdle(*m_copy.animated,m_hud.c_str());
        if (!played) { entry.state=1; return nullptr; }
        const int steps=std::max(clip.Steps(),fixed ? fixed : vrwi::ClipSteps(length,unknown));
        const u16 bones=m_copy.sample->LL_BoneCount();
        clip.length=length;
        clip.bones=bones;
        clip.frames.resize(size_t(steps)*bones);
        for (int step=0;step<steps;++step)
        {
            m_copy.Evaluate(step ? 1.f/60.f : 0.f);
            for (u16 id=0;id<bones;++id) clip.frames[size_t(step)*bones+id]=m_copy.sample->LL_GetTransform(id);
        }
        entry.state=2;
        return &clip;
    }
};
