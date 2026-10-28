#pragma once
#include <initializer_list>
#include <vector>

namespace vr
{
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

// Shared private-model pose path. The pose buffer must outlive the model.
template<class Model, class Poses, class Callback>
void ApplyWeaponPose(Model& model, Poses& poses, Callback callback, unsigned callback_type,
    const char* moving_bone, float travel, const char* rounds_bone,
    std::initializer_list<const char*> detachable_bones, bool magazine_only,
    bool magazine_inserted, bool rounds_visible, const char* hidden_bone=nullptr,
    std::initializer_list<const char*> hidden_bones={},
    std::initializer_list<const char*> extra_detachable_bones={})
{
    Poses bind;
    model.LL_GetBindTransform(bind);
    poses.resize(bind.size());
    const auto missing=static_cast<unsigned short>(-1);
    const auto moving=moving_bone && *moving_bone ? model.LL_BoneID(moving_bone) : missing;
    const auto bullets=rounds_bone && *rounds_bone ? model.LL_BoneID(rounds_bone) : missing;
    const auto forced_hidden=hidden_bone && *hidden_bone ? model.LL_BoneID(hidden_bone) : missing;
    const unsigned short count=model.LL_BoneCount();
    // Listed bones have an explicit state. Every other bone inherits its
    // parent's, so unlisted sub-bones of a hidden magazine, scope or launcher
    // hide with it, and those of a visible loose magazine stay visible.
    std::vector<signed char> state(count,-1);
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
    const auto visible=[&](unsigned short i)
    {
        for (unsigned short depth=0;depth<count && i<count;++depth)
        {
            if (state[i]>=0) return state[i]>0;
            const unsigned short parent=model.LL_GetData(i).GetParentID();
            if (parent>=count) return !magazine_only;
            i=parent;
        }
        return !magazine_only;
    };
    for (unsigned short i=0;i<count;++i)
    {
        const bool shown=visible(i);
        if (bool(model.LL_GetBoneVisible(i))!=shown)
            model.LL_SetBoneVisible(i,shown,false);
        poses[i]=bind[i];
        // The moving part carries its children (a charging handle on a bolt).
        bool carried=false;
        for (unsigned short a=i;a<count && !carried;a=model.LL_GetData(a).GetParentID())
            carried=a==moving;
        if (carried && !magazine_only) poses[i].c.z-=travel;
        auto& bone=model.LL_GetBoneInstance(i);
        bone.mTransformHidden=bind[i];
        bone.set_callback(callback_type,callback,&poses[i],true);
    }
    model.CalculateBones_Invalidate();
    model.CalculateBones(true);
    CollapseHiddenSkinning(model);
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
