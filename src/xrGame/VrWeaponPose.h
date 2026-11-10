#pragma once
#include <initializer_list>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace vr
{
inline bool MagazineBoneName(const char* name)
{
    if (!name) return false;
    const auto starts=[](const char* at, const char* term)
    {
        for (; *term; ++term, ++at)
            if (!*at || std::tolower(static_cast<unsigned char>(*at)) != *term) return false;
        return true;
    };
    if (starts(name,"mag") && (name[3] == 0 || name[3] == '_')) return true;
    for (const char* at=name; *at; ++at)
        if (starts(at,"magaz") || starts(at,"wpn_mag")) return true;
    return false;
}

// Compare two sampled local poses, projected onto a weapon axis. Callers
// use the animation's first pose: a fixed bind-to-animation rotation is not
// evidence that a magazine or cosmetic part is a manually operated action.
template<class Matrix>
float TurnAbout(const Matrix& first, const Matrix& now, const Matrix& parent, int axis)
{
    float turn=0.f;
    for (int own=0; own<3; ++own)
    {
        auto a=own==0 ? first.i : own==1 ? first.j : first.k;
        auto b=own==0 ? now.i : own==1 ? now.j : now.k;
        const auto transform=[&](auto& v)
        {
            const auto p=v;
            v.x=p.x*parent.i.x+p.y*parent.j.x+p.z*parent.k.x;
            v.y=p.x*parent.i.y+p.y*parent.j.y+p.z*parent.k.y;
            v.z=p.x*parent.i.z+p.y*parent.j.z+p.z*parent.k.z;
        };
        transform(a); transform(b);
        // The turn about `axis` (0 x, 1 y, 2 z): the axes' components across it.
        if (axis==2) a.z=b.z=0.f; else if (axis==1) a.y=b.y=0.f; else a.x=b.x=0.f;
        const float la=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
        const float lb=std::sqrt(b.x*b.x+b.y*b.y+b.z*b.z);
        if (la<0.5f || lb<0.5f) continue;
        const float dot=(a.x*b.x+a.y*b.y+a.z*b.z)/(la*lb);
        turn=std::max(turn,std::acos(std::max(-1.f,std::min(dot,1.f))));
    }
    return turn;
}

inline const char* PoseBoneName(const char* name) { return name; }
template<class Name> const char* PoseBoneName(const Name& name) { return name.c_str(); }

// The overwrite pose callbacks restore full hierarchy poses, so the renderer
// would still skin a hidden bone that shares a mesh chunk with visible ones
// (the stock PM magazine shares the pistol's chunk). Collapse only the
// skinning matrix; mTransform stays valid for visible children. Also used as
// the model's update callback so a later bone recalculation stays hidden.
template<class Model>
void CollapseHiddenSkinning(Model& model)
{
    for (unsigned short i=0;i<model.LL_BoneCount();++i)
        if (!model.LL_GetBoneVisible(i))
        {
            auto& bone=model.LL_GetBoneInstance(i);
            const auto origin=bone.mTransform.c;
            bone.mRenderTransform.scale(0.f,0.f,0.f);
            bone.mRenderTransform.c=origin;
        }
}

// One actual visual's immutable bind/topology and reusable visibility work.
// Reset after releasing the visual, even if its replacement has the same address.
template<class Poses>
struct WeaponPoseCache
{
    struct Bone
    {
        unsigned short parent=0, depth=0;
        signed char state=-1;
        bool shown=false, reload=false, carried=false, handled=false;
    };
    const void* model=nullptr;
    const void* pose_data=nullptr;
    Poses bind;
    std::vector<Bone> bones;
    std::vector<unsigned short> order;
    std::vector<signed char> requested;
    std::vector<unsigned char> counted;
    unsigned frame=~0u;
    unsigned short moving=0, handle=0;
    float travel=0.f, handle_travel=0.f;
    bool magazine_only=false, calculate=false;

    void Reset()
    {
        model=pose_data=nullptr; frame=~0u;
        bind.clear(); bones.clear(); order.clear(); requested.clear(); counted.clear();
    }
    template<class Model> void Prepare(Model& visual)
    {
        const unsigned short count=visual.LL_BoneCount();
        if (model==&visual && bind.size()==count) return;
        Reset();
        visual.LL_GetBindTransform(bind);
        bones.resize(count); order.resize(count); requested.resize(count);
        for (unsigned short id=0;id<count;++id) bones[id].parent=visual.LL_GetData(id).GetParentID();
        for (unsigned short id=0;id<count;++id)
        {
            order[id]=id;
            for (unsigned short a=bones[id].parent; a<count && bones[id].depth<count; a=bones[a].parent)
                ++bones[id].depth;
        }
        // Dedicated magazine assets need not number parents before children.
        std::sort(order.begin(),order.end(),[&](unsigned short a, unsigned short b)
            { return bones[a].depth<bones[b].depth; });
        model=&visual;
    }
};

// Shared private-model pose path. The pose buffer must outlive the model.
// `calculate` false: the caller recalculates the bones itself afterwards.
// An optional frame suppresses identical preparation between views. Each cache
// belongs to one visual and callback/pose buffer, never to an eye or a weapon type.
// Returns false only when that same frame's pose was already prepared.
template<class Model, class Poses, class Callback, class HiddenNames=std::initializer_list<const char*>>
bool ApplyWeaponPose(Model& model, Poses& poses, Callback callback, unsigned callback_type,
    const char* moving_bone, float travel, const char* rounds_bone,
    std::initializer_list<const char*> detachable_bones, bool magazine_only,
    bool magazine_inserted, bool rounds_visible, const char* hidden_bone=nullptr,
    std::initializer_list<const char*> hidden_bones={},
    std::initializer_list<const char*> extra_detachable_bones={},
    const char* handle_bone=nullptr, float handle_travel=0.f,
    const HiddenNames& reload_magazines={}, bool calculate=true,
    WeaponPoseCache<Poses>* storage=nullptr, unsigned frame=~0u)
{
    WeaponPoseCache<Poses> local;
    auto& cache=storage ? *storage : local;
    cache.Prepare(model);
    const auto missing=static_cast<unsigned short>(-1);
    const auto moving=moving_bone && *moving_bone ? model.LL_BoneID(moving_bone) : missing;
    // A separate charging handle (MP5, P90, AR-15) moves only when racked.
    const auto handle=handle_bone && *handle_bone ? model.LL_BoneID(handle_bone) : missing;
    const auto bullets=rounds_bone && *rounds_bone ? model.LL_BoneID(rounds_bone) : missing;
    const auto forced_hidden=hidden_bone && *hidden_bone ? model.LL_BoneID(hidden_bone) : missing;
    const unsigned short count=model.LL_BoneCount();
    // Listed bones have an explicit state. Every other bone inherits its
    // parent's, so unlisted sub-bones of a hidden magazine, scope or launcher
    // hide with it, and those of a visible loose magazine stay visible.
    auto& state=cache.requested;
    std::fill(state.begin(),state.end(),-1);
    // Each name is looked up once, not once per bone.
    const auto mark=[&](std::initializer_list<const char*> names, signed char value)
    {
        for (const char* name:names)
            if (name && *name)
            {
                const auto id=model.LL_BoneID(name);
                if (id<count) state[id]=value;
            }
    };
    const signed char detached=magazine_only || magazine_inserted;
    mark(detachable_bones,detached);
    mark(extra_detachable_bones,detached);
    if (forced_hidden<count) state[forced_hidden]=0;
    if (bullets<count && !rounds_visible) state[bullets]=0;
    mark(hidden_bones,0);
    // Names are resolved on this visual, not reused as IDs from the weapon
    // skeleton: dedicated loose-magazine assets may have a different layout.
    for (const auto& name:reload_magazines)
    {
        const char* text=PoseBoneName(name);
        const auto id=text && *text ? model.LL_BoneID(text) : missing;
        if (id<count) state[id]=-2;
    }
    if (frame!=~0u && cache.frame==frame && cache.pose_data==poses.data() && poses.size()==count &&
        cache.moving==moving && cache.handle==handle && cache.travel==travel &&
        cache.handle_travel==handle_travel && cache.magazine_only==magazine_only && cache.calculate==calculate &&
        std::equal(state.begin(),state.end(),cache.bones.begin(),
            [](signed char value, const auto& bone) { return value==bone.state; }))
        return false;
    poses.assign(cache.bind.begin(),cache.bind.end());
    // Resolve inheritance once, parents first. Reload-only subtrees override
    // even explicitly visible descendants; moving parts carry their children.
    for (unsigned short id:cache.order)
    {
        auto& bone=cache.bones[id];
        const auto* parent=bone.parent<count ? &cache.bones[bone.parent] : nullptr;
        bone.state=state[id];
        bone.reload=state[id]==-2 || (parent && parent->reload);
        bone.shown=!bone.reload && (state[id]>=0 ? state[id]>0 : parent ? parent->shown : !magazine_only);
        bone.carried=id==moving || (parent && parent->carried);
        bone.handled=id==handle || (parent && parent->handled);
    }
    for (unsigned short i=0;i<count;++i)
    {
        const auto& prepared=cache.bones[i];
        const bool shown=prepared.shown;
        if (bool(model.LL_GetBoneVisible(i))!=shown)
            model.LL_SetBoneVisible(i,shown,false);
        auto& bone=model.LL_GetBoneInstance(i);
        bone.mTransformHidden=poses[i];
        // The moving part carries its children (a charging handle on a bolt).
        if (prepared.carried && !magazine_only) poses[i].c.z-=travel;
        else if (prepared.handled && !magazine_only) poses[i].c.z-=handle_travel;
        bone.set_callback(callback_type,callback,&poses[i],true);
    }
    cache.frame=frame; cache.pose_data=poses.data();
    cache.moving=moving; cache.handle=handle; cache.travel=travel; cache.handle_travel=handle_travel;
    cache.magazine_only=magazine_only; cache.calculate=calculate;
    if (calculate)
    {
        model.CalculateBones_Invalidate();
        model.CalculateBones(true);
        CollapseHiddenSkinning(model);
    }
    return true;
}
// A collimator's reticle is seen only when the line from the eye to it
// passes through the sight's window (a disc of radius around window,
// facing along axis).
template<class Vector>
bool SightWindowHit(const Vector& eye, const Vector& reticle, const Vector& window,
    const Vector& axis, float radius)
{
    const float vx=reticle.x-eye.x, vy=reticle.y-eye.y, vz=reticle.z-eye.z;
    const float facing=vx*axis.x+vy*axis.y+vz*axis.z;
    if (facing<=1e-4f) return false;
    const float t=((window.x-eye.x)*axis.x+(window.y-eye.y)*axis.y+(window.z-eye.z)*axis.z)/facing;
    const float dx=eye.x+vx*t-window.x, dy=eye.y+vy*t-window.y, dz=eye.z+vz*t-window.z;
    return dx*dx+dy*dy+dz*dz<=radius*radius;
}
// A scope's image brightness for an eye `behind` metres behind the eyepiece
// and `lateral` metres off its axis: full 3-10 cm behind and within 1 cm of
// the axis, gone under 2 or past 15 cm behind, or 2.5 cm off axis.
inline float ExitPupilFade(float behind, float lateral)
{
    const auto unit=[](float v) { return v<0.f ? 0.f : (v>1.f ? 1.f : v); };
    return unit((0.15f-behind)/0.05f)*unit((behind-0.02f)/0.01f)*unit(1.f-(lateral-0.01f)/0.015f);
}
}
