// Bone, mesh and motion helpers for measuring VR weapon models (VrWeaponModel.h).
#include "stdafx.h"
#include "VrWeaponModel.h"
#include "VrWeaponPose.h"

namespace vrwi
{
void _BCL BonePose(CBoneInstance* bone)
{
    bone->mTransform=*static_cast<const Fmatrix*>(bone->callback_param());
}
bool Descends(IKinematics& model, u16 id, u16 root)
{
    for (u16 step=0;id!=BI_NONE && id<model.LL_BoneCount() && step<model.LL_BoneCount();++step)
    {
        if (id==root) return true;
        id=model.LL_GetData(id).GetParentID();
    }
    return false;
}
// Model-space pose of a bone below a part VR draws at its bind place (a
// magazine): the part as bound, what hangs below it as the weapon's own
// idle lays it out (rest_local, parent-relative). A GAMMA RPD's
// belt links are bound half a metre behind the gun; only the idle
// brings them to the drum.
Fmatrix RestPose(IKinematics& model, const xr_vector<Fmatrix>& bind, const xr_vector<Fmatrix>& rest_local,
    u16 id, u16 root)
{
    if (id>=bind.size() || root>=bind.size()) return Fidentity;
    u16 chain[64]; int n=0;
    for (u16 a=id; a!=root; a=model.LL_GetData(a).GetParentID())
        if (a>=bind.size() || n>=64) return bind[id];
        else chain[n++]=a;
    Fmatrix pose=bind[root];
    while (n) pose.mulB_43(rest_local[chain[--n]]);
    return pose;
}
// Each bone's bind pose relative to its parent's (a root's as bound).
xr_vector<Fmatrix> BindLocal(IKinematics& model, const xr_vector<Fmatrix>& bind)
{
    xr_vector<Fmatrix> local(bind);
    for (u16 id=0;id<bind.size();++id)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        if (parent<bind.size()) local[id].mul_43(Fmatrix().invert(bind[parent]),bind[id]);
    }
    return local;
}
// A bone's vertices at rest below root; bind-space without rest data.
void EnumRestVertices(IKinematics& model, SEnumVerticesCallback& out, u16 id, u16 root,
    const xr_vector<Fmatrix>* rest_local, const xr_vector<Fmatrix>& bind)
{
    if (!rest_local || rest_local->size()!=model.LL_BoneCount() || bind.size()<model.LL_BoneCount() ||
        id==root || root>=bind.size())
    { model.EnumBoneVertices(out,id); return; }
    SkinnedVertices skinned(out);
    skinned.skin.mul_43(RestPose(model,bind,*rest_local,id,root),Fmatrix().invert(bind[id]));
    model.EnumBoneVertices(skinned,id);
}
// The magazine bone's own mesh height (InMagazine's spare test).
float MagazineHeight(IKinematics& model, u16 magazine)
{
    Fbox own;
    return MeasureBone(model,magazine,own,false) ? own.max.y-own.min.y : 0.f;
}
// height: MagazineHeight, measured here when not given (loops pass it).
bool InMagazine(IKinematics& model, u16 id, u16 magazine, float height)
{
    if (!Descends(model,id,magazine)) return false;
    // A spare: a magazine-named part at least half the magazine's height. A
    // smaller one (a follower, the rounds) is part of the magazine.
    if (height<0.f) height=MagazineHeight(model,magazine);
    for (u16 bone=id;bone!=magazine && bone<model.LL_BoneCount();bone=model.LL_GetData(bone).GetParentID())
    {
        Fbox part;
        if (NameHas(model.LL_BoneName_dbg(bone),"mag") && MeasureBone(model,bone,part,false) &&
            part.max.y-part.min.y>=0.5f*height) return false;
    }
    return true;
}
// Bind-space mesh bounds of the magazine (without nested spares), its
// contents at rest when rest_local is given.
// bind: the model's bind transforms, fetched here when not given.
bool MeasureMagazine(IKinematics& model, u16 magazine, Fbox& box, const xr_vector<Fmatrix>* rest_local,
    const xr_vector<Fmatrix>* bind)
{
    if (magazine==BI_NONE) return false;
    xr_vector<Fmatrix> own;
    if (rest_local && !bind) { model.LL_GetBindTransform(own); bind=&own; }
    const float height=MagazineHeight(model,magazine);
    BoneBounds bounds;
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (InMagazine(model,id,magazine,height)) EnumRestVertices(model,bounds,id,magazine,rest_local,bind ? *bind : own);
    if (!bounds.count) return false;
    box=bounds.box;
    return true;
}
// Bind-space mesh bounds of a bone, optionally with its whole subtree; with
// rest_local, what hangs below rest_root (the bone itself by default) at rest.
bool MeasureBone(IKinematics& model, u16 bone, Fbox& box, bool subtree,
    const xr_vector<Fmatrix>* rest_local, u16 rest_root, const xr_vector<Fmatrix>* bind)
{
    if (bone==BI_NONE) return false;
    xr_vector<Fmatrix> own;
    if (rest_local && !bind) { model.LL_GetBindTransform(own); bind=&own; }
    const u16 root=rest_root!=BI_NONE ? rest_root : bone;
    BoneBounds bounds;
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (id==bone || (subtree && Descends(model,id,bone)))
            EnumRestVertices(model,bounds,id,root,rest_local,bind ? *bind : own);
    if (!bounds.count) return false;
    box=bounds.box;
    return true;
}
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
// Plays a motion on every partition of a model, from its first frame.
void PlayOnAllParts(IKinematicsAnimated& animated, MotionID motion)
{
    for (u16 part=0;part<animated.partitions().count();++part)
        animated.PlayCycle(part,motion,FALSE,0,0,0,1.f);
}
// Advances a model's animation by dt and evaluates its bones.
void EvaluateBones(IKinematicsAnimated& animated, IKinematics& model, float dt)
{
    animated.LL_UpdateTracks(dt,true,true);
    model.CalculateBones_Invalidate();
    model.CalculateBones(TRUE);
}
// Plays a hud section's animation on a private model from its first frame.
bool PlayHudMotion(IKinematicsAnimated& animated, LPCSTR hud, LPCSTR key, float* length)
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
// How far a bone is back from its bind place, measured in the receiver's
// frame (so the receiver's own recoil kick in a shot is not counted).
float RearInReceiver(const Fmatrix& receiver_now, const Fmatrix& bone_now, const xr_vector<Fmatrix>& bind,
    u16 bone, u16 receiver)
{
    Fmatrix rel; rel.invert(receiver_now); rel.mulA_43(bind[receiver]);
    Fvector now; rel.transform_tiny(now,bone_now.c);
    return bind[bone].c.z-now.z;
}
float RearInReceiver(IKinematics& sample, const xr_vector<Fmatrix>& bind, u16 bone, u16 receiver)
{
    return RearInReceiver(sample.LL_GetTransform(receiver),sample.LL_GetTransform(bone),bind,bone,receiver);
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
// The weapon at rest, as its idle shows it: the item's own "idle" when the
// hud section names no item motion for it, as player_hud plays it.
bool PlayHudIdle(IKinematicsAnimated& animated, LPCSTR hud)
{
    if (PlayHudMotion(animated,hud,"anm_idle")) return true;
    const MotionID idle=animated.ID_Cycle_Safe("idle");
    if (!idle.valid()) return false;
    PlayOnAllParts(animated,idle);
    return true;
}
// Plays the weapon's reload (the empty one first); returns the key it played.
LPCSTR StartReloadMotion(IKinematicsAnimated& animated, LPCSTR hud, float* length)
{
    for (LPCSTR key:{"anm_reload_empty","anm_reload"})
        if (PlayHudMotion(animated,hud,key,length)) return key;
    return nullptr;
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
