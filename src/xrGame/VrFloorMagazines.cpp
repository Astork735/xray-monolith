// Magazines left in the world, and a VR hand carrying them.
#include "stdafx.h"
#include "VrWeaponInteraction.h"
#include "VrWeaponPose.h"
#include "VrReloadDerivation.h"
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

bool VrFloorMagazineHand(int hand)
{
    return FloorMagazineIn(hand)!=nullptr;
}
void RenderVrFloorMagazines()
{
    if (!g_vr_owns_present) return;
    for (auto& magazine:g_floor_magazines)
    {
        if (!magazine.visual || !magazine.shell) continue;
        // Carried, it is drawn where the hand is now, not where physics
        // last put it.
        Fmatrix world, grip;
        if (magazine.hand>=0 && vr::HandTransform(magazine.hand,false,grip)) world.mul_43(grip,magazine.in_hand);
        else magazine.shell->InterpolateGlobalTransform(&world);
        ::Render->set_Transform(&world);
        ::Render->add_Visual(magazine.visual);
    }
}
void ReleaseVrFloorMagazines()
{
    for (auto& magazine:g_floor_magazines) RetireFloorMagazine(magazine);
    g_floor_magazines.clear();
}
static void LetGoFloorMagazine(VrFloorMagazine& magazine, const Fvector& velocity, const Fvector& angular)
{
    magazine.hand=-1;
    magazine.expires=Device.dwTimeGlobal+floor_magazine_life;
    magazine.shell->EnableCollision();
    magazine.shell->set_LinearVel(velocity);
    magazine.shell->set_AngularVel(angular);
}
// The floor magazine a hand's pointer ray rests on within use range (2 m),
// not behind static geometry, the hand not reaching through a wall from the
// head (as VrPointerPick). Its box is widened by 3 cm: a magazine is small.
static bool FloorMagazinePointed(CActor& actor, int side, VrFloorMagazine*& taken, Fmatrix& taken_world)
{
    Fmatrix hand, aim;
    if (!vr::HandTransform(side,false,hand) || !vr::HandTransform(side,true,aim)) return false;
    if (!VrHandReachable(actor,hand.c)) return false;
    float best=2.f;
    for (auto& magazine:g_floor_magazines)
    {
        if (magazine.hand>=0 || !magazine.shell) continue;
        Fmatrix world; magazine.shell->GetGlobalTransformDynamic(&world);
        Fmatrix inverse; inverse.invert(world);
        Fvector start, dir;
        inverse.transform_tiny(start,aim.c);
        inverse.transform_dir(dir,aim.k);
        // The widened box in the magazine's frame, nearer than the best so far.
        Fvector lo, hi;
        lo.sub(magazine.box_center,magazine.box_half).sub(0.03f);
        hi.add(magazine.box_center,magazine.box_half).add(0.03f);
        float enter=0.f;
        if (!vr::RayBoxEnter(Pick(start),Pick(dir),Pick(lo),Pick(hi),best,enter)) continue;
        collide::rq_result wall;
        if (enter>EPS_S && Level().ObjectSpace.RayPick(aim.c,aim.k,enter,collide::rqtStatic,wall,&actor)) continue;
        best=enter; taken=&magazine; taken_world=world;
    }
    return taken!=nullptr;
}

// Floor magazines each frame: they expire, and free hands take them. A
// fresh grip or trigger on one carries it, held as its weapon holds one from
// the pouch (where the hand closed on it if that is unknown). In the support hand of a weapon that takes it, it is that weapon's
// magazine from then on (TakeFloorMagazine). Let go at the shoulder or the
// pouch its rounds go into the backpack; anywhere else it falls.
void UpdateVrFloorMagazines(CActor& actor, bool controls_valid, const bool tracked[2],
    const Fmatrix hands[2], const bool down[2], const bool may_grab[2], CWeapon* held_weapon)
{
    static bool armed[2]{};
    const auto& pickup=actor.m_vr_pickup;
    for (auto it=g_floor_magazines.begin();it!=g_floor_magazines.end();)
    {
        if (it->hand>=0 || int(Device.dwTimeGlobal-it->expires)<0) { ++it; continue; }
        Msg("* [VR weapon] floor magazine expired with %u rounds",unsigned(it->cartridges.size()));
        RetireFloorMagazine(*it);
        it=g_floor_magazines.erase(it);
    }
    const int support=held_weapon ? 1-held_weapon->VrWeaponHand() : -1;
    for (int h=0;h<2;++h)
    {
        const bool valid=controls_valid && tracked[h];
        auto carried=std::find_if(g_floor_magazines.begin(),g_floor_magazines.end(),
            [h](const VrFloorMagazine& magazine) { return magazine.hand==h; });
        if (carried!=g_floor_magazines.end())
        {
            VrFloorMagazine& magazine=*carried;
            armed[h]=false;
            if (!valid) { LetGoFloorMagazine(magazine,Fvector().set(0,0,0),Fvector().set(0,0,0)); continue; }
            Fmatrix world; world.mul_43(hands[h],magazine.in_hand);
            CVrWeaponInteraction* weapon=h==support ? held_weapon->VrInteraction() : nullptr;
            if (weapon && weapon->TakeFloorMagazine(magazine.kind,magazine.cartridges))
            {
                RetireFloorMagazine(magazine);
                g_floor_magazines.erase(carried);
                vr::HandHaptic(h,0.15f);
                continue;
            }
            if (down[h])
            {
                magazine.shell->SetGlTransformDynamic(world);
                magazine.shell->set_LinearVel(pickup.hand_velocity[h]);
                continue;
            }
            if (VrAtBackpack(actor,hands[h].c) ||
                Near(hands[h].c,VrPouchPoint(actor),vr_pouch_reach))
            {
                Msg("* [VR weapon] floor magazine put in the backpack (%u rounds)",unsigned(magazine.cartridges.size()));
                ReturnRoundsToActor(actor,magazine.cartridges);
                RetireFloorMagazine(magazine);
                g_floor_magazines.erase(carried);
                vr::HandHaptic(h,0.22f);
            }
            else LetGoFloorMagazine(magazine,pickup.hand_velocity[h],pickup.hand_angular[h]);
            continue;
        }
        // Only a squeeze that starts here takes one.
        if (!valid) { armed[h]=false; continue; }
        if (!down[h]) { armed[h]=true; continue; }
        if (!armed[h]) continue;
        armed[h]=false;
        if (!may_grab[h] || pickup.grabbed_id[h]!=u16(-1)) continue;
        // The one the hand is on: within 12 cm of its box, as for items.
        VrFloorMagazine* taken=nullptr;
        Fmatrix taken_world;
        float best=0.12f;
        for (auto& magazine:g_floor_magazines)
        {
            if (magazine.hand>=0) continue;
            Fmatrix world; magazine.shell->GetGlobalTransformDynamic(&world);
            Fvector local; Fmatrix().invert(world).transform_tiny(local,hands[h].c);
            Fbox box; box.min.sub(magazine.box_center,magazine.box_half); box.max.add(magazine.box_center,magazine.box_half);
            const float distance=BoxDistance(local,box);
            if (distance<best) { best=distance; taken=&magazine; taken_world=world; }
        }
        // Otherwise the one this hand points at, as the pointer pulls an item
        // (VrPointerPick cannot see these: they are props, not objects).
        // An item the hand is on comes first (UpdateVrPickup takes it).
        bool on_item=false;
        const char* why="";
        const bool pulled=!taken && !VrGrabTarget(actor,h,true,on_item,why) &&
            FloorMagazinePointed(actor,h,taken,taken_world);
        if (!taken) continue;
        taken->hand=h;
        if (taken->hold_hand==h) taken->in_hand=taken->hold;
        else
        {
            taken->in_hand.mul_43(Fmatrix().invert(hands[h]),taken_world);
            // Pulled from a distance it comes into the hand, as an item does.
            if (pulled) taken->in_hand.c.set(0.f,0.f,0.09f);
        }
        // The hand's own body would push it about.
        taken->shell->DisableCollision();
        vr::HandHaptic(h,0.12f);
    }
}

