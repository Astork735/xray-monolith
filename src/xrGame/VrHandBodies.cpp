// VR hands as physics bodies, and climbing.
#include "stdafx.h"
#include "VrWeaponInteractionInternal.h"
#include "Weapon.h"
#include "Actor.h"
#include "inventory.h"
#include "PhysicsShellHolder.h"
#include "HUDManager.h"
#include "../xrEngine/Vr/vr_gameplay.h"
#include "ClimableObject.h"
#include "../xrphysics/Extendedgeom.h"
#include "../xrPhysics/PhysicsShell.h"
using namespace vrwi;

static void VrHandContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*);
static void VrWeaponContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*);
// What both bodies leave alone; `static_world`: the static world stops it.
static void VrBodyContact(bool& do_colide, bool bo1, dContact& c, bool static_world)
{
    const dGeomID other=bo1 ? c.geom.g2 : c.geom.g1;
    dxGeomUserData* data=retrieveGeomUserData(other);
    if (data && data->object_callbacks && (data->object_callbacks->HasCallback(VrHandContact) ||
        data->object_callbacks->HasCallback(VrWeaponContact)))
    { do_colide=false; return; }
    if (!dGeomGetBody(other)) { if (!static_world) do_colide=false; return; }
    IPhysicsShellHolder* owner=data ? data->ph_ref_object : nullptr;
    if (owner && (owner->IsActor() || owner->IsStalker() || !owner->ObjectPPhysicsShell()))
        do_colide=false;
}
// With vr_physical_hands 2 hands also meet the static world (walls, tables)
// and are drawn where their bodies are, so a hand stops on a surface; with 1
// they push loose objects only and never snag on a door frame.
static void VrHandContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*)
{
    VrBodyContact(do_colide,bo1,c,vr::PhysicalHands()>=2);
}
// The weapon body pushes loose objects only; the gun is drawn at the hand.
static void VrWeaponContact(bool& do_colide, bool bo1, dContact& c, SGameMtl*, SGameMtl*)
{
    VrBodyContact(do_colide,bo1,c,false);
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

// Wakes the loose physics objects within radius, so a hand or weapon body
// pushes them. Runs every frame: the candidate list is kept (game thread).
static void WakeNear(const Fvector& centre, float radius)
{
    static xr_vector<ISpatial*> nearby;
    nearby.clear();
    g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,centre,radius);
    for (ISpatial* spatial:nearby)
        if (auto* holder=smart_cast<CPhysicsShellHolder*>(spatial->dcast_CObject()))
            if (!holder->H_Parent() && holder->PPhysicsShell() && holder->PPhysicsShell()->isActive() &&
                !holder->PPhysicsShell()->isEnabled() && !smart_cast<CActor*>(holder))
                holder->PPhysicsShell()->Enable();
}

// Each tracked hand not carrying anything has a 4.5 cm sphere at the palm,
// and a held weapon a box around its mesh: both push boxes, bottles and
// bodies. Physics objects near the hands are woken so they respond.
void UpdateVrHandBodies(CActor& actor, const bool tracked[2], const Fmatrix hands[2])
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
        WakeNear(hands[side].c,0.35f);
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
        WakeNear(centre,box.m_halfsize.magnitude()+0.2f);
    }
}

// What a hand's pointer ray rests on within use range, provided the hand is
// not reaching through a wall from the head.
bool VrPointerPick(CActor& actor, int side, collide::rq_result& hit)
{
    Fmatrix hand, aim;
    if (!vr::HandTransform(side,false,hand) || !vr::HandTransform(side,true,aim)) return false;
    if (!VrHandReachable(actor,hand.c)) return false;
    // As the desktop pick that scripts read (level.get_target_obj): through
    // glass, grates and foliage to the first object, so the use and the
    // scripts acting on it agree on the target.
    SPickParam pick;
    pick.InitPick();
    pick.defs.start=aim.c; pick.defs.dir=aim.k; pick.defs.range=2.f;
    HUD().DoPick(pick);
    hit=pick.result;
    return hit.O!=nullptr;
}

// Hand-over-hand climbing: a free hand closing on a ladder holds it, and
// while held, pulling the hand down lifts the body (and pushing it up lowers
// it). Returns the vertical speed, or 0 when no hand holds a ladder.
void UpdateVrClimb(CActor& actor, const bool available[2], const bool grip[2],
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

