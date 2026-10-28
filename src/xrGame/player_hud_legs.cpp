#include "stdafx.h"
#include "player_hud_legs.h"
#include "player_hud.h"
#include "actor.h"
#include "inventory_item.h"
#include "Inventory.h"
#include "Weapon.h"
#include "VrWeaponInteraction.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xrEngine/FDemoRecord.h"
#include "../xrEngine/CameraBase.h"
#include "../xrEngine/Vr/vr_gameplay.h"
#include "ai_space.h"
#include "script_engine.h"

BOOL g_legs_enabled = FALSE;
float legs_fwd_offset = -0.5f;
BOOL legs_attach_to_camera = TRUE;
extern int showActorBody;
extern xr_unordered_set<CDemoRecord*> pDemoRecords;

void player_legs_controller::destroy()
{
    for (IKinematics** model : { &m_model, &m_shadow_model })
    {
        if (!*model)
            continue;

        if (*model == m_model && m_vr_outfit_arms)
        {
            for (u16 id = 0; id < (*model)->LL_BoneCount(); ++id)
                (*model)->LL_GetBoneInstance(id).reset_callback();
            (*model)->Callback(nullptr, nullptr);
        }

        if (IRenderVisual* v = (*model)->dcast_RenderVisual())
            ::Render->model_Delete(v);

        *model = nullptr;
    }

    m_visual_name = "";
    m_shadow_name = "";
    m_vr_outfit_arms = false;
}

bool player_legs_controller::resolve_config(CActor* actor, shared_str& sect, shared_str& model)
{
    PIItem outfit = actor->inventory().ItemFromSlot(OUTFIT_SLOT);
    shared_str current_outfit = outfit
        ? outfit->object().cNameSect()
        : shared_str("actor");

    // The actor visual already reflects outfit and skin overrides.
    // Keyed to the VR session, not to momentary tracking or focus, so a lost
    // sample does not recreate the model and its arm IK setup.
    const bool vr_body = g_vr_owns_present;
    if (vr_body && actor->Visual() && actor->cNameVisual().size())
    {
        sect=current_outfit;
        // The arms are the current outfit's first-person hands: the look the
        // player knows, and the skeleton every weapon animation holds weapons
        // with. The full body visual still casts the shadow.
        model=actor->cNameVisual();
        m_vr_body_visual=actor->cNameVisual();
        if (vr::AnimatedGrip() && g_player_hud && pSettings->line_exist(g_player_hud->section_name(),"visual"))
            model=pSettings->r_string(g_player_hud->section_name(),"visual");
        m_last_outfit_sect=sect;
        m_last_model=model;
        m_last_vr_mode=true;
        return true;
    }

    if (!m_last_vr_mode && m_last_outfit_sect == current_outfit)
    {
        sect = m_last_outfit_sect;
        model = m_last_model;
        return true;
    }

    m_last_outfit_sect = current_outfit;
    m_last_vr_mode=false;

    if (outfit)
    {
        if (pSettings->line_exist(m_last_outfit_sect, "legs_visual"))
        {
            sect = m_last_outfit_sect;
            m_last_model = pSettings->r_string(m_last_outfit_sect, "legs_visual");
            model = m_last_model;
            return true;
        }

        if (pSettings->line_exist(m_last_outfit_sect, "actor_visual"))
        {
            sect = m_last_outfit_sect;
            m_last_model = pSettings->r_string(m_last_outfit_sect, "actor_visual");
            model = m_last_model;
            return true;
        }

        warn_once("outfit [%s] has no legs_visual or actor_visual, fallback to default", m_last_outfit_sect.c_str());
    }

    // default
    if (pSettings->line_exist("actor", "legs_visual"))
    {
        sect = m_last_outfit_sect;
        m_last_model = pSettings->r_string(m_last_outfit_sect, "legs_visual");
        model = m_last_model;
        return true;
    }

    if (pSettings->line_exist(m_last_outfit_sect, "visual"))
    {
        sect = m_last_outfit_sect;
        m_last_model = pSettings->r_string(m_last_outfit_sect, "visual");
        model = m_last_model;
        return true;
    }

    warn_once("actor has no legs_visual or visual");

    return false;
}

void player_legs_controller::warn_once(const char* fmt, ...)
{
    string512 buf;
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, sizeof(buf), fmt, args);
    va_end(args);

    xr_string s = make_string("! [player_legs] %s", buf).c_str();
    static xr_unordered_flat_set<xr_string> warnings;
    if (warnings.find(s) == warnings.end())
    {
        warnings.insert(s);
        Msg(s.c_str());
    }
}

bool player_legs_controller::ensure_model(const shared_str& sect, const shared_str& model)
{
    const shared_str shadow=g_vr_owns_present && m_vr_body_visual.size() ? m_vr_body_visual : model;
    if (m_model && m_visual_name == model && m_shadow_name == shadow && m_vr_model_mode == !!g_vr_owns_present)
        return true;

    destroy();

    // Separate instance for the shadow pass: the full body shadow must not touch the hidden bones
    // of the main model, which is drawn again after shadows (emissive, forward passes)
    for (IKinematics** target : { &m_model, &m_shadow_model })
    {
        IRenderVisual* raw = ::Render->model_Create(target == &m_shadow_model ? shadow.c_str() : model.c_str());
        if (!raw)
        {
            destroy();
            warn_once("failed to create model [%s]", model.c_str());
            return false;
        }

        IKinematics* K = smart_cast<IKinematics*>(raw);
        if (!K)
        {
            ::Render->model_Delete(raw);
            destroy();
            warn_once("model [%s] is not a skeleton", model.c_str());
            return false;
        }

        *target = K;
    }

    m_visual_name = model;
    m_shadow_name = shadow;
    m_vr_model_mode=!!g_vr_owns_present;
    m_vr_outfit_arms=false;
    const char* names[2][3]={{"bip01_l_upperarm","bip01_l_forearm","bip01_l_hand"},
                             {"bip01_r_upperarm","bip01_r_forearm","bip01_r_hand"}};
    for (int side=0;side<2;++side)
        for (int bone=0;bone<3;++bone)
        {
            m_vr_arm_bones[side][bone]=m_model->LL_BoneID(names[side][bone]);
            // First-person hands name them without the biped prefix.
            if (m_vr_arm_bones[side][bone]==BI_NONE)
                m_vr_arm_bones[side][bone]=m_model->LL_BoneID(names[side][bone]+6);
        }
    if (g_vr_owns_present)
    {
        m_vr_outfit_arms=true;
        for (const auto& arm:m_vr_arm_bones)
            for (u16 id:arm) if (id==BI_NONE) m_vr_outfit_arms=false;
        if (!m_vr_outfit_arms)
            warn_once("outfit visual [%s] lacks arm IK bones; using VR hand fallback",model.c_str());
        else
            prepare_vr_arms();
    }

    if (pSettings->line_exist(sect, "legs_fwd_offset"))
        m_fwd_offset = pSettings->r_float(sect, "legs_fwd_offset");
    else
        m_fwd_offset = std::nullopt;

    ::luabind::functor<void> funct;
    if (ai().script_engine().functor("_G.player_legs_controller__OnCreateLegs", funct))
        funct(sect.c_str());

    return true;
}

// TODO : clean up later
float legs_spine_offset_y = 0.1f;
namespace
{
void _BCL VrArmBonePose(CBoneInstance* bone)
{
    bone->mTransform=*static_cast<const Fmatrix*>(bone->callback_param());
}

void VrCollapseBody(IKinematics* model)
{
    if (auto* owner=static_cast<player_legs_controller*>(model->GetUpdateCallbackParam()))
        owner->collapse_vr_body();
}

// Rotate a model-space bone about its own joint.
void RotateAbout(Fmatrix& pose, Fvector axis, float angle)
{
    if (axis.square_magnitude()<EPS_S || std::abs(angle)<EPS_S) return;
    axis.normalize_safe();
    Fmatrix rotation; rotation.rotation(axis,angle);
    Fmatrix rotated; rotated.mul_43(rotation,pose);
    rotated.c=pose.c;
    pose=rotated;
}

// Anatomical hand frame: wrist-to-knuckles, pinky-to-index, and their cross
// product (palm normal for a right hand, back of the hand for a left one).
void HandBasis(Fvector knuckles, Fvector index_side, Fvector out[3])
{
    knuckles.normalize_safe();
    index_side.mad(knuckles,-index_side.dotproduct(knuckles));
    index_side.normalize_safe();
    out[0]=knuckles; out[1]=index_side;
    out[2].crossproduct(knuckles,index_side);
}

Fmatrix AimArmBone(const Fmatrix& original, Fvector from, Fvector to,
    const Fvector& position)
{
    Fmatrix result=original;
    if (from.square_magnitude()>EPS_S && to.square_magnitude()>EPS_S)
    {
        from.normalize_safe(); to.normalize_safe();
        Fvector axis; axis.crossproduct(from,to);
        const float sine=axis.magnitude();
        const float cosine=clampr(from.dotproduct(to),-1.f,1.f);
        if (sine>0.0001f)
        {
            axis.div(sine);
            Fmatrix rotation; rotation.rotation(axis,atan2f(sine,cosine));
            result.mul_43(rotation,original);
        }
    }
    result.c=position;
    return result;
}
}

void player_legs_controller::prepare_vr_arms()
{
    const u16 count=m_model->LL_BoneCount();
    xr_vector<Fmatrix> bind;
    m_model->LL_GetBindTransform(bind);
    m_vr_bone_group.assign(count,0);
    m_vr_collapse_side.assign(count,0);
    m_vr_finger.assign(count,u8(-1));
    m_vr_digit.assign(count,u8(-1));
    m_vr_finger_joint.assign(count,0);
    m_vr_bind_local.assign(count,Fidentity);
    m_vr_segment_local.assign(count,Fvector().set(0,0,0));
    m_vr_bone_poses.assign(count,Fidentity);
    xr_vector<u16> depth(count,0);
    const Fvector& left=bind[m_vr_arm_bones[0][0]].c;
    const Fvector& right=bind[m_vr_arm_bones[1][0]].c;
    for (u16 id=0;id<count;++id)
    {
        const u16 parent=m_model->LL_GetData(id).GetParentID();
        if (parent!=BI_NONE && parent<count)
        {
            Fmatrix inverse; inverse.invert(bind[parent]);
            m_vr_bind_local[id].mul_43(inverse,bind[id]);
        }
        u16 ancestor=id;
        for (u16 step=0;ancestor!=BI_NONE && step<count;++step)
        {
            if (ancestor==m_vr_arm_bones[0][0]) { m_vr_bone_group[id]=1; break; }
            if (ancestor==m_vr_arm_bones[1][0]) { m_vr_bone_group[id]=2; break; }
            ancestor=m_model->LL_GetData(ancestor).GetParentID();
            ++depth[id];
        }
        // Collapsed body skin closes into the nearer shoulder joint.
        m_vr_collapse_side[id]=bind[id].c.distance_to_sqr(left)<=bind[id].c.distance_to_sqr(right) ? 0 : 1;
    }
    // A joint's segment runs toward its first child.
    for (u16 id=0;id<count;++id)
    {
        const u16 parent=m_model->LL_GetData(id).GetParentID();
        if (parent!=BI_NONE && parent<count && m_vr_segment_local[parent].square_magnitude()<EPS_S)
            m_vr_segment_local[parent]=m_vr_bind_local[id].c;
    }
    for (int side=0;side<2;++side)
    {
        m_vr_arm_order[side].clear();
        const u16 hand=m_vr_arm_bones[side][2];
        for (u16 id=0;id<count;++id)
        {
            if (m_vr_bone_group[id]!=side+1) continue;
            bool chain=false;
            for (u16 bone:m_vr_arm_bones[side]) chain=chain || bone==id;
            if (chain) continue;
            m_vr_arm_order[side].push_back(id);
            // Stalker hands use finger0 (thumb), finger1 (index) and finger2
            // (remaining fingers), each with three joints below the hand.
            LPCSTR name=m_model->LL_BoneName_dbg(id);
            LPCSTR finger=name ? strstr(name,"_finger") : nullptr;
            if (finger && finger[7]>='0' && finger[7]<='4')
            {
                m_vr_finger[id]=u8(std::min(finger[7]-'0',2));
                m_vr_digit[id]=u8(finger[7]-'0');
                u16 joint=0;
                for (u16 parent=m_model->LL_GetData(id).GetParentID();
                     parent!=BI_NONE && parent!=hand && joint<4;
                     parent=m_model->LL_GetData(parent).GetParentID()) ++joint;
                m_vr_finger_joint[id]=u8(std::min<u16>(joint,2));
            }
        }
        std::sort(m_vr_arm_order[side].begin(),m_vr_arm_order[side].end(),
            [&](u16 a, u16 b) { return depth[a]<depth[b]; });
        // Measure the hand's own frame from its bind pose so a controller
        // frame maps onto the mesh regardless of the bone's axis convention.
        char name[]="bip01_l_finger1";
        name[6]=side ? 'r' : 'l';
        const u16 index=m_model->LL_BoneID(name);
        name[14]='2';
        const u16 others=m_model->LL_BoneID(name);
        m_vr_hand_basis_valid[side]=index!=BI_NONE && others!=BI_NONE;
        if (m_vr_hand_basis_valid[side])
        {
            Fmatrix inverse; inverse.invert(bind[hand]);
            Fvector a,b,knuckles,index_side;
            inverse.transform_tiny(a,bind[index].c);
            inverse.transform_tiny(b,bind[others].c);
            knuckles.add(a,b).mul(0.5f);
            m_vr_knuckle_local[side]=knuckles;
            index_side.sub(a,b);
            HandBasis(knuckles,index_side,m_vr_hand_basis[side]);
            m_vr_finger2[side]=others;
            m_vr_index[side]=index;
            Fvector palm=m_vr_hand_basis[side][2];
            if (!side) palm.invert();
            for (auto& axis:m_vr_curl_axis[side]) axis.set(0,0,0);
            for (u16 id:m_vr_arm_order[side])
            {
                if (m_vr_finger[id]>2 || m_vr_finger_joint[id] ||
                    m_vr_segment_local[id].square_magnitude()<EPS_S) continue;
                Fvector segment,local;
                bind[id].transform_dir(segment,m_vr_segment_local[id]);
                inverse.transform_dir(local,segment);
                Fvector& axis=m_vr_curl_axis[side][m_vr_finger[id]];
                axis.crossproduct(local,palm);
                axis.normalize_safe();
            }
        }
    }
    // Bind skeletons do not share the animated facing (stalker binds face
    // -Z). Map the bind shoulder line onto +X right, +Y up, +Z forward.
    Fmatrix body; body.identity();
    body.i.sub(bind[m_vr_arm_bones[1][0]].c,bind[m_vr_arm_bones[0][0]].c);
    body.i.y=0;
    if (body.i.square_magnitude()<EPS_S) body.i.set(1,0,0);
    body.i.normalize_safe();
    body.j.set(0,1,0);
    body.k.crossproduct(body.i,body.j);
    body.c.add(bind[m_vr_arm_bones[0][0]].c,bind[m_vr_arm_bones[1][0]].c).mul(0.5f);
    Fmatrix canonical; canonical.invert(body);
    for (int side=0;side<2;++side)
        for (int bone=0;bone<3;++bone)
            m_vr_rest[side][bone].mul_43(canonical,bind[m_vr_arm_bones[side][bone]]);
    // Upper-arm bones: in an arm, but not the forearm or below it.
    m_vr_upper_arm.assign(count,0);
    for (int side=0;side<2;++side)
        for (u16 id=0;id<count;++id)
        {
            if (m_vr_bone_group[id]!=side+1) continue;
            bool forearm=false;
            for (u16 a=id; a!=BI_NONE && a<count && !forearm; a=m_model->LL_GetData(a).GetParentID())
                forearm=a==m_vr_arm_bones[side][1];
            if (!forearm) m_vr_upper_arm[id]=u8(side+1);
        }
    m_model->Callback(VrCollapseBody,this);
    m_vr_solved=false;
}

bool player_legs_controller::refresh_vr_arms(CActor* actor)
{
    // The arms are solved once per tracked body placement, with the head and
    // controller poses that frame is drawn with. The held weapon (and the
    // NPC-style attachment of an unmeasured one) reads the same solution, so
    // hands and weapon never trail one another while the body moves.
    if (!actor || !m_model || !m_vr_outfit_arms || m_vr_solving) return false;
    const unsigned epoch=vr::BodyEpoch();
    if (m_vr_solved && epoch==m_vr_solved_epoch) return false;
    m_vr_solving=true;
    anchor_vr_arms();
    pose_vr_arms(actor);
    m_vr_solving=false;
    m_vr_solved=true;
    m_vr_solved_epoch=epoch;
    return true;
}

void player_legs_controller::apply_vr_arm_bones()
{
    // Recompute the arm chains from the stored solution (their callbacks).
    for (int side=0;side<2;++side)
    {
        const u16 upper=m_vr_arm_bones[side][0];
        const u16 parent=m_model->LL_GetData(upper).GetParentID();
        if (parent==BI_NONE) continue;
        Fmatrix parent_pose=m_model->LL_GetTransform(parent);
        m_model->Bone_Calculate(&m_model->LL_GetData(upper),&parent_pose);
    }
}

bool player_legs_controller::vr_weapon_hand(int side, Fmatrix& hand, Fmatrix& finger) const
{
    const_cast<player_legs_controller*>(this)->refresh_vr_arms(Actor());
    if (!m_model || !m_vr_outfit_arms || side<0 || side>1 ||
        Device.dwFrame-m_vr_weapon_hand_frame[side]>1) return false;
    hand=m_vr_weapon_hand[side];
    finger=m_vr_weapon_finger[side];
    return true;
}

void player_legs_controller::collapse_vr_body()
{
    if (!m_model || !m_vr_outfit_arms || m_vr_bone_group.size()!=m_model->LL_BoneCount()) return;
    const Fvector shoulder[2]={m_model->LL_GetBoneInstance(m_vr_arm_bones[0][0]).mTransform.c,
        m_model->LL_GetBoneInstance(m_vr_arm_bones[1][0]).mTransform.c};
    for (u16 id=0;id<m_model->LL_BoneCount();++id)
    {
        if (m_vr_bone_group[id]) continue;
        // Hide the torso, legs and head by skinning only. Their hierarchy
        // poses stay intact so the shoulders keep a valid parent; the
        // visibility mask would collapse them toward the floor instead.
        auto& bone=m_model->LL_GetBoneInstance(id);
        bone.mRenderTransform.scale(0.f,0.f,0.f);
        bone.mRenderTransform.c=shoulder[m_vr_collapse_side[id]];
    }
    // Upper arms stay attached to the shoulders, but one passing right in
    // front of the eyes (a shouldered weapon) closes its skin at the elbow.
    Fvector eye;
    if (!vr::HeadPosition(eye)) return;
    Fmatrix to_arms; to_arms.invert(m_vr_arms_transform);
    to_arms.transform_tiny(eye);
    for (int side=0;side<2;++side)
    {
        const Fvector& from=m_model->LL_GetBoneInstance(m_vr_arm_bones[side][0]).mTransform.c;
        const Fvector elbow=m_model->LL_GetBoneInstance(m_vr_arm_bones[side][1]).mTransform.c;
        Fvector segment; segment.sub(elbow,from);
        const float length=segment.square_magnitude();
        const float t=length>EPS_S ? clampr(Fvector().sub(eye,from).dotproduct(segment)/length,0.f,1.f) : 0.f;
        Fvector closest=from; closest.mad(segment,t);
        if (closest.distance_to(eye)>0.13f) continue;
        for (u16 id=0;id<m_model->LL_BoneCount() && id<m_vr_upper_arm.size();++id)
        {
            if (m_vr_upper_arm[id]!=side+1) continue;
            auto& bone=m_model->LL_GetBoneInstance(id);
            bone.mRenderTransform.scale(0.f,0.f,0.f);
            bone.mRenderTransform.c=elbow;
        }
    }
}

void player_legs_controller::anchor_vr_arms()
{
    // The arms model space is centred on the shoulder line, which sits under
    // the neck of the tracked head and faces the actor's yaw.
    m_vr_arms_transform=m_legs_transform;
    Fvector head;
    if (!vr::HeadPosition(head))
    {
        m_vr_arms_transform.c.mad(m_legs_transform.j,1.45f);
        return;
    }
    m_vr_arms_transform.c=head;
    m_vr_arms_transform.c.mad(Device.vCameraDirection,-0.09f).mad(Device.vCameraTop,-0.10f);
    m_vr_arms_transform.c.y-=0.09f;
}

void player_legs_controller::pose_vr_arms(CActor* actor)
{
    if (!actor || !m_model || !vr::GameplayActive()) return;
    Fmatrix inverse; inverse.invert(m_vr_arms_transform);
    // The weapon hand is solved first: the support hand and an unmeasured
    // weapon's NPC-style attachment follow it.
    auto* held_weapon=smart_cast<CWeapon*>(actor->inventory().ActiveItem());
    const int first=held_weapon && held_weapon->VrWeaponHand()==0 ? 0 : 1;
    for (int pass=0;pass<2;++pass)
    {
        const int side=pass ? 1-first : first;
        Fmatrix tracked;
        auto* active=held_weapon;
        // Either hand may hold the weapon; the other supports it. A support
        // hand on the weapon, or carrying a guided magazine, follows the
        // frame the weapon gives it.
        const int weapon_side=active ? active->VrWeaponHand() : -1;
        const bool support_side=active && side!=weapon_side;
        const bool guided=support_side && active->VrSupportHandOverride(tracked);
        const bool tracked_ok=guided || vr::HandTransform(side,false,tracked);
        // An empty hand is drawn where its physics body is: on a surface it
        // presses into, not inside it.
        Fvector body_position;
        if (tracked_ok && !guided && side!=weapon_side && !(support_side && active->VrSupporting()) &&
            VrHandBodyPosition(*actor,side,body_position))
            tracked.c=body_position;
        if (!tracked_ok)
        {
            for (u16 id:m_vr_arm_bones[side])
                m_model->LL_GetBoneInstance(id).reset_callback();
            for (u16 id:m_vr_arm_order[side])
                m_model->LL_GetBoneInstance(id).reset_callback();
            continue;
        }
        const u16 upper=m_vr_arm_bones[side][0];
        const u16 lower=m_vr_arm_bones[side][1];
        const u16 wrist=m_vr_arm_bones[side][2];
        const Fmatrix& shoulder_pose=m_vr_rest[side][0];
        const Fmatrix& elbow_pose=m_vr_rest[side][1];
        const Fmatrix& hand_pose=m_vr_rest[side][2];
        Fmatrix grip; grip.mul_43(inverse,tracked);
        Fvector frame[3];
        Fmatrix held, weapon_world;
        auto* weapon=side==weapon_side ? active : nullptr;
        // The support hand also holds when it steadies a two-handed weapon.
        const bool supporting=support_side && active->VrSupporting();
        const bool holding=weapon ? weapon->VrAimTransform(weapon_world) : supporting || guided;
        const bool framed=holding && weapon && weapon->VrHandFrame(held);
        // Measured weapons are held as their first-person animation holds
        // them: the weapon hand's grip and trigger finger, and the support
        // hand fixed on the fore-end or around the pistol grip.
        // On the same first-person hands skeleton, the animation's own hand:
        // the hand bone where the animation has it, the fingers as posed.
        VrHandBones exact_bones;
        const bool exact=active && (side==weapon_side || support_side) &&
            active->VrAuthoredBones(side,exact_bones) && exact_bones.visual==m_visual_name &&
            exact_bones.locals && exact_bones.locals->size()==m_model->LL_BoneCount();
        VrHandPose authored;
        CWeapon* holder=weapon ? weapon : (supporting ? active : nullptr);
        const bool animated=!exact && holding && holder && holder->VrAuthoredHand(side,authored);
        if (animated)
            for (int axis=0;axis<3;++axis) inverse.transform_dir(frame[axis],authored.basis[axis]);
        else if (framed)
        {
            // Wrap the weapon's grip so the hand matches the rendered gun
            // rather than the controller handle's steeper tilt.
            grip.mul_43(inverse,held);
            HandBasis(grip.k,grip.j,frame);
        }
        else
        {
            // OpenXR grip: -Z runs up the handle from little finger to thumb
            // and +Y points back toward the wrist, so the knuckles face -Y.
            HandBasis(Fvector().invert(grip.j),grip.k,frame);
        }
        Fvector palm=frame[2];
        if (!side) palm.invert();
        // Hand orientation: the mesh's measured hand frame onto the target.
        Fmatrix hand_rotation=grip;
        hand_rotation.c.set(0,0,0);
        if (m_vr_hand_basis_valid[side])
        {
            const Fvector* local=m_vr_hand_basis[side];
            Fvector* axes[3]={&hand_rotation.i,&hand_rotation.j,&hand_rotation.k};
            for (int axis=0;axis<3;++axis)
            {
                axes[axis]->set(0,0,0);
                for (int basis=0;basis<3;++basis)
                    axes[axis]->mad(frame[basis],local[basis][axis]);
            }
        }
        // The handle sits in the palm, in front of the wrist; on a measured
        // weapon grip the palm rests on its surface.
        const float grip_radius=framed ? weapon->VrHandGripRadius() : 0.015f;
        Fvector wrist_goal=grip.c;
        wrist_goal.mad(palm,-(grip_radius+0.015f)).mad(frame[0],-0.065f);
        // With a known trigger, place the hand by its index knuckle instead:
        // just behind and above the trigger blade on the hand's own side, as
        // in a firing grip. The index then reaches the trigger and the other
        // fingers fall onto the grip below, for any weapon.
        Fvector trigger_world;
        const bool anchored=!animated && framed && m_vr_index[side]!=BI_NONE && weapon->VrTriggerPoint(trigger_world);
        if (anchored)
        {
            Fvector offset;
            offset.set((side ? 1.f : -1.f)*(grip_radius+0.008f),0.012f,-0.022f);
            weapon_world.transform_dir(offset);
            Fvector knuckle; knuckle.add(trigger_world,offset);
            inverse.transform_tiny(knuckle);
            Fvector knuckle_offset;
            hand_rotation.transform_dir(knuckle_offset,m_vr_bind_local[m_vr_index[side]].c);
            wrist_goal.sub(knuckle,knuckle_offset);
        }
        if (animated)
        {
            // The animation's knuckles: hand sizes differ, so the knuckles,
            // not the wrist, are what must sit where the animator put them.
            Fvector knuckles; inverse.transform_tiny(knuckles,authored.knuckles);
            Fvector knuckle_offset;
            hand_rotation.transform_dir(knuckle_offset,m_vr_knuckle_local[side]);
            wrist_goal.sub(knuckles,knuckle_offset);
            // Press the palm onto the weapon: the animation's hand is not
            // this one, so its knuckles alone can leave a gap at the palm.
            const xr_vector<Fvector>* points=nullptr;
            Fmatrix points_world;
            float unused=0.f;
            if (holder->VrGripCloud(points,points_world,unused) && points && !points->empty())
            {
                Fmatrix to_weapon; to_weapon.invert(points_world);
                to_weapon.mulB_43(m_vr_arms_transform);
                Fvector normal=m_vr_hand_basis[side][2];
                if (!side) normal.invert();
                Fvector palm_local=m_vr_knuckle_local[side];
                palm_local.mad(m_vr_hand_basis[side][0],-0.045f).mad(normal,0.012f);
                Fvector centre; hand_rotation.transform_dir(centre,palm_local);
                centre.add(wrist_goal);
                Fvector centre_weapon; to_weapon.transform_tiny(centre_weapon,centre);
                m_vr_contact.clear();
                for (const Fvector& point:*points)
                    if (point.distance_to_sqr(centre_weapon)<0.08f*0.08f) m_vr_contact.push_back(point);
                float best=flt_max, shift=0.f;
                for (int step=-5;step<=15 && !m_vr_contact.empty();++step)
                {
                    const float s=0.002f*float(step);
                    Fvector p=centre; p.mad(palm,s);
                    to_weapon.transform_tiny(p);
                    float nearest=flt_max;
                    for (const Fvector& q:m_vr_contact) nearest=std::min(nearest,q.distance_to_sqr(p));
                    const float error=_abs(_sqrt(nearest)-0.008f);
                    if (error<best) { best=error; shift=s; }
                }
                wrist_goal.mad(palm,shift);
            }
        }
        if (exact)
        {
            Fmatrix target; target.mul_43(inverse,exact_bones.hand);
            hand_rotation=target;
            hand_rotation.c.set(0,0,0);
            wrist_goal=target.c;
        }
        Fvector shoulder=shoulder_pose.c;
        const float a=shoulder.distance_to(elbow_pose.c);
        const float b=elbow_pose.c.distance_to(hand_pose.c);
        if (a<0.04f || b<0.04f || a>0.7f || b>0.7f) continue;
        Fvector direction; direction.sub(wrist_goal,shoulder);
        float distance=direction.magnitude();
        if (distance<0.01f) continue;
        direction.div(distance);
        // A hand just beyond the arm's reach (a fore-end at full extension)
        // draws the shoulder forward, as a real shoulder does, instead of
        // leaving the hand short of the weapon.
        const float excess=distance-(a+b-0.002f);
        if (excess>0.f)
        {
            shoulder.mad(direction,std::min(excess,0.08f));
            distance=std::max(0.01f,distance-std::min(excess,0.08f));
        }
        const float reach=clampr(distance,std::abs(a-b)+0.002f,a+b-0.002f);
        Fvector pole; pole.set(side ? 1.f : -1.f,-0.2f,-0.45f);
        pole.mad(direction,-pole.dotproduct(direction));
        if (pole.square_magnitude()<EPS_S) pole.set(0,1,0);
        pole.normalize_safe();
        const float along=(a*a-b*b+reach*reach)/(2.f*reach);
        const float bend=std::sqrt(std::max(0.f,a*a-along*along));
        Fvector elbow=shoulder; elbow.mad(direction,along); elbow.mad(pole,bend);
        Fvector wrist_target=shoulder; wrist_target.mad(direction,reach);
        auto& poses=m_vr_bone_poses;
        poses[upper]=AimArmBone(shoulder_pose,
            Fvector().sub(elbow_pose.c,shoulder),Fvector().sub(elbow,shoulder),shoulder);
        poses[lower]=AimArmBone(elbow_pose,
            Fvector().sub(hand_pose.c,elbow_pose.c),Fvector().sub(wrist_target,elbow),elbow);
        Fmatrix& hand=poses[wrist];
        hand=hand_rotation;
        hand.c=wrist_target;
        // Roll the forearm most of the way toward the hand so the wrist skin
        // does not wrap when the controller is turned.
        {
            Fvector axis; axis.sub(wrist_target,elbow); axis.normalize_safe();
            Fmatrix rest_inverse; rest_inverse.invert(elbow_pose);
            Fmatrix relative; relative.mul_43(rest_inverse,hand_pose);
            Fmatrix predicted; predicted.mul_43(poses[lower],relative);
            const Fvector* from[3]={&predicted.i,&predicted.j,&predicted.k};
            const Fvector* to[3]={&hand.i,&hand.j,&hand.k};
            int best=0;
            for (int i=1;i<3;++i)
                if (_abs(from[i]->dotproduct(axis))<_abs(from[best]->dotproduct(axis))) best=i;
            Fvector a=*from[best], b=*to[best];
            a.mad(axis,-a.dotproduct(axis)); b.mad(axis,-b.dotproduct(axis));
            if (a.square_magnitude()>EPS_S && b.square_magnitude()>EPS_S)
            {
                a.normalize(); b.normalize();
                const float angle=atan2f(axis.dotproduct(Fvector().crossproduct(a,b)),a.dotproduct(b));
                RotateAbout(poses[lower],axis,0.6f*angle);
            }
        }
        // Fingers start from the open bind pose and curl toward the palm with
        // the analog trigger (index) and grip (other fingers, thumb), or
        // around a held weapon's mesh (below).
        const float trigger=vr::HandTrigger(side), squeeze=holding ? 1.f : vr::HandSqueeze(side);
        static const float free_relaxed[3][3]={{0.05f,0.10f,0.10f},{0.20f,0.25f,0.20f},{0.25f,0.30f,0.25f}};
        static const float free_closed[3][3]={{0.35f,0.55f,0.45f},{1.25f,1.45f,1.00f},{1.35f,1.50f,1.05f}};
        static const float hold_relaxed[3][3]={{0.45f,0.60f,0.50f},{0.15f,0.15f,0.10f},{1.10f,1.30f,0.90f}};
        static const float hold_closed[3][3]={{0.45f,0.60f,0.50f},{0.85f,0.95f,0.65f},{1.10f,1.30f,0.90f}};
        const auto& relaxed=holding ? hold_relaxed : free_relaxed;
        const auto& closed=holding ? hold_closed : free_closed;
        const auto build=[&](u16 id, float curl)
        {
            Fmatrix& pose=poses[id];
            pose.mul_43(poses[m_model->LL_GetData(id).GetParentID()],m_vr_bind_local[id]);
            if (curl==0.f || m_vr_finger[id]>2) return;
            Fvector axis;
            poses[wrist].transform_dir(axis,m_vr_curl_axis[side][m_vr_finger[id]]);
            RotateAbout(pose,axis,curl);
        };
        for (u16 id:m_vr_arm_order[side])
            if (m_vr_finger[id]>2) build(id,0.f);
        if (m_vr_finger2[side]!=BI_NONE)
        {
            // Published before the fingers are fitted: an unmeasured weapon is
            // attached to this hand and its mesh is what the fingers wrap.
            Fmatrix finger_base;
            finger_base.mul_43(poses[m_model->LL_GetData(m_vr_finger2[side]).GetParentID()],
                m_vr_bind_local[m_vr_finger2[side]]);
            m_vr_weapon_hand[side].mul_43(m_vr_arms_transform,poses[wrist]);
            m_vr_weapon_finger[side].mul_43(m_vr_arms_transform,finger_base);
            m_vr_weapon_hand_frame[side]=Device.dwFrame;
        }
        // Universal grip: around a held weapon every finger joint, from the
        // knuckle outward, curls until its segment touches the weapon's own
        // mesh, so fingers wrap whatever grip, fore-end or stock is in the
        // hand. The thumb also stops before rising above the bore, so it can
        // never cover the sights. No per-weapon data is involved.
        const xr_vector<Fvector>* cloud=nullptr;
        Fmatrix cloud_world, to_weapon;
        float ceiling=0.f;
        CWeapon* gripped=weapon ? weapon : (supporting ? active : nullptr);
        Fvector trigger_local{};
        if (anchored) { inverse.transform_tiny(trigger_local,trigger_world); }
        bool fitted=false;
        if (!animated && holding && gripped && gripped->VrGripCloud(cloud,cloud_world,ceiling) && cloud)
        {
            Fmatrix weapon_inverse; weapon_inverse.invert(cloud_world);
            to_weapon.mul_43(weapon_inverse,m_vr_arms_transform);
            Fvector palm_centre; palm_centre.mad(poses[wrist].c,frame[0],0.05f);
            to_weapon.transform_tiny(palm_centre);
            m_vr_contact.clear();
            for (const Fvector& point:*cloud)
                if (point.distance_to_sqr(palm_centre)<0.12f*0.12f) m_vr_contact.push_back(point);
            fitted=!m_vr_contact.empty();
        }
        static const float joint_limit[3]={1.6f,1.7f,1.2f};
        for (int digit=0;digit<5;++digit)
        {
            // Body models have three chains (thumb, index, the rest); the
            // first-person hands have five. Each chain is posed on its own,
            // the last three as "the rest".
            const int finger=std::min(digit,2);
            u16 bones[4]; int count=0;
            for (u16 id:m_vr_arm_order[side])
                if (m_vr_digit[id]==digit && count<4) bones[count++]=id;
            if (!count) continue;
            if (exact)
            {
                // The animator's finger exactly (same skeleton).
                for (int j=0;j<count;++j)
                {
                    const u16 parent=m_model->LL_GetData(bones[j]).GetParentID();
                    poses[bones[j]].mul_43(poses[parent],(*exact_bones.locals)[bones[j]]);
                    // Squeezing the trigger presses the index pad further.
                    if (weapon && finger==1 && j<2)
                    {
                        Fvector axis; poses[wrist].transform_dir(axis,m_vr_curl_axis[side][1]);
                        RotateAbout(poses[bones[j]],axis,trigger*0.25f);
                    }
                }
                continue;
            }
            if (animated)
            {
                // Each joint turns its segment onto the animation's.
                for (int j=0;j<count;++j)
                {
                    build(bones[j],0.f);
                    Fmatrix& pose=poses[bones[j]];
                    Fvector target; inverse.transform_dir(target,authored.finger[finger][std::min(j,2)]);
                    Fvector local=m_vr_segment_local[bones[j]];
                    if (local.square_magnitude()<EPS_S)
                    {
                        // A last joint continues its bind direction from its parent.
                        Fmatrix rotation=m_vr_bind_local[bones[j]]; rotation.c.set(0,0,0);
                        Fmatrix to_local; to_local.invert(rotation);
                        to_local.transform_dir(local,m_vr_bind_local[bones[j]].c);
                    }
                    Fvector current; pose.transform_dir(current,local);
                    pose=AimArmBone(pose,current,target,pose.c);
                    // Squeezing the trigger presses the index pad further.
                    if (weapon && finger==1 && j<2)
                    {
                        Fvector axis; poses[wrist].transform_dir(axis,m_vr_curl_axis[side][1]);
                        RotateAbout(pose,axis,trigger*0.3f);
                    }
                }
                continue;
            }
            float curl[4]={};
            for (int j=0;j<count;++j)
            {
                const int joint=std::min(j,2);
                const float amount=finger==1 ? trigger : squeeze;
                curl[j]=fitted ? closed[finger][joint] :
                    relaxed[finger][joint]+(closed[finger][joint]-relaxed[finger][joint])*amount;
            }
            // Weapon-hand index: the curl that brings its pad (middle and end
            // segments) nearest the trigger blade.
            if (fitted && anchored && finger==1)
            {
                static const float index_shape[3]={0.9f,1.1f,0.7f};
                float best=flt_max, chosen=0.f;
                for (int step=0;step<=16;++step)
                {
                    const float amount=0.1f*float(step);
                    for (int k=0;k<count;++k) build(bones[k],index_shape[std::min(k,2)]*amount);
                    const Fvector& tip_base=poses[bones[count-1]].c;
                    Fvector tip=tip_base;
                    if (count>1) tip.mad(Fvector().sub(tip_base,poses[bones[count-2]].c),0.6f);
                    const float error=std::min(tip.distance_to_sqr(trigger_local),
                        tip_base.distance_to_sqr(trigger_local));
                    if (error<best) { best=error; chosen=amount; }
                }
                for (int k=0;k<count;++k) curl[k]=index_shape[std::min(k,2)]*chosen;
            }
            // Thumb: the lowest pose that stays clear of the mesh, so it lies
            // along the frame instead of standing over the sights.
            else if (fitted && finger==0)
            {
                float best_height=flt_max, best_clearance=-1.f, chosen=0.f, clearest=0.f;
                for (int step=0;step<=16;++step)
                {
                    const float amount=0.1f*float(step);
                    for (int k=0;k<count;++k) build(bones[k],amount);
                    float height=-flt_max, clearance=flt_max;
                    for (int k=0;k<count;++k)
                    {
                        Fvector point; to_weapon.transform_tiny(point,poses[bones[k]].c);
                        height=std::max(height,point.y);
                        for (const Fvector& q:m_vr_contact)
                            clearance=std::min(clearance,point.distance_to_sqr(q));
                    }
                    if (clearance>best_clearance) { best_clearance=clearance; clearest=amount; }
                    if (clearance>0.006f*0.006f && height<best_height) { best_height=height; chosen=amount; }
                }
                const float amount=best_height<flt_max ? chosen : clearest;
                for (int k=0;k<count;++k) curl[k]=amount;
            }
            for (int j=0;fitted && !(anchored && finger==1) && finger!=0 && j<count;++j)
            {
                const int joint=std::min(j,2);
                const float fallback=curl[j];
                for (int k=j+1;k<count;++k) curl[k]=0.f;
                for (int step=1;step<=10;++step)
                {
                    curl[j]=joint_limit[joint]*0.1f*float(step);
                    for (int k=j;k<count;++k) build(bones[k],curl[k]);
                    // The segment this joint moves: to the next joint or tip.
                    const Fvector& from=poses[bones[j]].c;
                    Fvector to=from;
                    if (j+1<count) to=poses[bones[j+1]].c;
                    else if (j) to.mad(Fvector().sub(from,poses[bones[j-1]].c),0.9f);
                    Fvector samples[2];
                    samples[0].lerp(from,to,0.5f); samples[1]=to;
                    bool hit=false;
                    for (const Fvector& sample:samples)
                    {
                        Fvector point; to_weapon.transform_tiny(point,sample);
                        if (finger==0 && point.y>ceiling) hit=true;
                        for (size_t i=0;!hit && i<m_vr_contact.size();++i)
                            hit=point.distance_to_sqr(m_vr_contact[i])<0.009f*0.009f;
                        if (hit) break;
                    }
                    // A segment already touching at the start (palm pressed into
                    // the mesh) keeps the closed hold instead of staying straight.
                    if (hit) { curl[j]=step>1 ? joint_limit[joint]*0.1f*float(step-1) : fallback; break; }
                    if (step==10) curl[j]=fallback;
                }
            }
            // Squeezing the trigger presses the index pad a little further.
            if (fitted && weapon && finger==1)
            {
                curl[0]+=trigger*0.3f;
                if (count>1) curl[1]+=trigger*0.3f;
            }
            for (int j=0;j<count;++j) build(bones[j],curl[j]);
        }
        for (u16 id:m_vr_arm_bones[side])
            m_model->LL_GetBoneInstance(id).set_callback(bctCustom,VrArmBonePose,&poses[id],true);
        for (u16 id:m_vr_arm_order[side])
            m_model->LL_GetBoneInstance(id).set_callback(bctCustom,VrArmBonePose,&poses[id],true);
    }
    apply_vr_arm_bones();
}

void player_legs_controller::copy_bones_from_actor(CActor* actor)
{
    if (!actor || !m_model || !m_shadow_model)
        return;

    IKinematics* actor_K = actor->Visual()->dcast_PKinematics();
    if (!actor_K)
        return;

    actor_K->CalculateBones(TRUE);

    // Full body for shadows
    m_shadow_model->CopyBonesFrom(actor_K);

    // Legs only for the main pass
    m_model->CopyBonesFrom(actor_K);

    if (g_vr_owns_present)
    {
        // Keep the equipped outfit skin and tracked arm IK, but do not draw a
        // displaced torso, legs or head in the headset.
        if (m_vr_outfit_arms)
        {
            // The copied bounds are the body's, from the feet; the arms live
            // around the shoulders (their model origin).
            if (IRenderVisual* visual=m_model->dcast_RenderVisual())
            {
                auto& vis=visual->getVisData();
                vis.box.set(-1.3f,-1.3f,-1.3f,1.3f,1.3f,1.3f);
                vis.box.getsphere(vis.sphere.P,vis.sphere.R);
            }
            // CopyBonesFrom replaced the arm bones with the body animation:
            // restore the tracked solution, solving it if the body moved.
            if (!refresh_vr_arms(actor)) apply_vr_arm_bones();
            collapse_vr_body();
        }
        else m_vr_arms_transform=m_legs_transform;
        return;
    }

    if (auto BoneID = m_model->LL_BoneID("bip01_spine"); BoneID != BI_NONE)
    {
        auto& BoneInstance = m_model->LL_GetData(BoneID);
        auto& transform = m_model->LL_GetTransform(BoneInstance.GetParentID());
        transform.c.y += legs_spine_offset_y;
        m_model->Bone_Calculate(&BoneInstance, &transform);
    }

    static LPCSTR bonesToHide[] = { "bip01_neck", "bip01_l_upperarm", "bip01_r_upperarm" };
    for (const auto& bone : bonesToHide)
    {
        u16 bone_id = m_model->LL_BoneID(bone);
        if (bone_id != BI_NONE)
        {
            m_model->LL_SetBoneVisible(bone_id, false, true);
        }
    }
}

void player_legs_controller::update(CActor* actor, bool isShadowPass)
{
    actor->XFORMShadow.set(actor->XFORM());

    if ((!g_legs_enabled && !g_vr_owns_present) || showActorBody != 0 || !actor)
    {
        destroy();
        return;
    }

    if (actor->Holder() != nullptr)
    {
        destroy();
        return;
    }

    shared_str sect;
    shared_str model;
    if (!resolve_config(actor, sect, model))
    {
        destroy();
        return;
    }

    if (!ensure_model(sect, model))
        return;

    m_legs_transform.set(actor->XFORM());
    if (legs_attach_to_camera && pDemoRecords.empty())
        m_legs_transform.c.set(Device.vCameraPosition.x, m_legs_transform.c.y, Device.vCameraPosition.z);

    Fvector fwd = m_legs_transform.k;
    fwd.y = 0.f;
    fwd.normalize_safe();

    float offset = g_vr_owns_present ? 0.f :
        (m_fwd_offset.has_value() ? m_fwd_offset.value() : legs_fwd_offset);
    m_legs_transform.c.mad(fwd, offset);

    // IK uses the final body transform to map controller poses to model space.
    if (!isShadowPass)
        copy_bones_from_actor(actor);

    // Move actor's XFORM for correct shadow placement
    actor->XFORMShadow.translate_over(m_legs_transform.c);
}

void player_legs_controller::render(bool isShadowPass)
{
    if (g_vr_owns_present && !isShadowPass && !m_vr_outfit_arms) return;
    if (g_vr_owns_present && !isShadowPass && m_model)
    {
        // Solve the arms with this frame's tracked body placement, unless
        // the held weapon already did; both eyes share the result.
        if (CActor* actor=Actor())
            if (refresh_vr_arms(actor)) collapse_vr_body();
    }
    IKinematics* model = isShadowPass ? m_shadow_model : m_model;
    if (!model)
        return;

    CActor* actor = Actor();
    if (!actor)
        return;

    u32 move_state = actor->MovingState();
    if ((move_state & mcClimb) && !g_vr_owns_present)
        return;

    IRenderVisual* visual = model->dcast_RenderVisual();
    if (!visual)
        return;

    ::Render->set_Transform(g_vr_owns_present && !isShadowPass ? &m_vr_arms_transform : &m_legs_transform);
    ::Render->add_Visual(visual);
}
