// What a free VR hand takes on a squeeze: weapons, items and the pointer (UpdateVrPickup).
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

bool VrActiveItemLeaving(CActor& actor)
{
    PIItem active=actor.inventory().ActiveItem();
    if (!active) return false;
    if (actor.inventory().GetNextActiveSlot()!=actor.inventory().GetActiveSlot()) return true;
    const CHudItem* hud=active->cast_hud_item();
    return hud && (hud->IsHidden() || hud->GetState()==CHUDState::eHiding || hud->GetNextState()==CHUDState::eHiding);
}



// Nothing of the static world between two points. Items, characters and the
// player's own weapon are no walls.
static bool VrClearLine(CActor& actor, const Fvector& from, const Fvector& to)
{
    Fvector dir; dir.sub(to,from);
    const float range=dir.magnitude();
    if (range<EPS_S) return true;
    dir.div(range);
    collide::rq_result hit;
    return !Level().ObjectSpace.RayPick(from,dir,range,collide::rqtStatic,hit,&actor);
}

// A hand within use range of the eyes (2 m: one reaching to the floor is
// about 1.7 m from them), not pushed through a wall from the head. Only the
// static world is a wall (an item or a character between is not).
bool VrHandReachable(CActor& actor, const Fvector& hand)
{
    const Fvector& eye=actor.cam_FirstEye()->vPosition;
    return eye.distance_to(hand)<=2.f && VrClearLine(actor,eye,hand);
}

// A loose world item the given hand may take. Asked every frame for every
// candidate: CInventory::CanTakeItem adds nothing for the actor (it always
// may carry more) to CanTake and getDestroy, and scans the whole inventory.
bool VrTakeable(CActor& actor, int side, CPhysicsShellHolder* object)
{
    auto* item=object ? object->cast_inventory_item() : nullptr;
    return item && item->CanTake() && object->PPhysicsShell() && object->PPhysicsShell()->isActive() &&
        !object->H_Parent() && object->getVisible() && !object->getDestroy() && object->Visual() &&
        object->ID()!=actor.m_vr_pickup.grabbed_id[1-side];
}

// How far a point (in the box's frame) is outside the box; 0 inside.
float BoxDistance(const Fvector& local, const Fbox& box)
{
    Fvector inside; inside.set(clampr(local.x,box.min.x,box.max.x),
        clampr(local.y,box.min.y,box.max.y),clampr(local.z,box.min.z,box.max.z));
    return inside.distance_to(local);
}

// What a free hand takes on a squeeze, and what its pointer names:
// - the item the hand is on: within 12 cm of its box (the controller sits a
//   hand's width from what the fingers close on), seen from the eyes or the
//   hand, so a hand dipped into the ground still takes what lies on it;
// - else, unless `near_only`, what its pointer rests on (VrPointerPick, the
//   desktop pick through foliage and grates);
// - else the item nearest its pointer ray within 2 m and 4 degrees (wider by
//   the item's size), in clear line from the hand.
// `touching` tells the first case. `why` is a reason when nothing is found.
// `pointer_missed`: the caller's own VrPointerPick this frame found nothing
// takeable, so the second case is not tried again.
CPhysicsShellHolder* VrGrabTarget(CActor& actor, int side, bool near_only, bool& touching, const char*& why,
    bool pointer_missed)
{
    touching=false; why="no tracked hand";
    Fmatrix hand, aim;
    if (!vr::HandTransform(side,false,hand) || !vr::HandTransform(side,true,aim)) return nullptr;
    const Fvector eye=actor.cam_FirstEye()->vPosition;
    // Kept between calls (game thread only): this runs every frame.
    static xr_vector<ISpatial*> nearby;
    nearby.clear();
    g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,hand.c,0.5f);
    CPhysicsShellHolder* best_object=nullptr;
    float best=0.12f;
    for (ISpatial* spatial:nearby)
    {
        auto* candidate=smart_cast<CPhysicsShellHolder*>(spatial->dcast_CObject());
        if (!VrTakeable(actor,side,candidate)) continue;
        Fmatrix to_item; to_item.invert(candidate->XFORM());
        Fvector local; to_item.transform_tiny(local,hand.c);
        const float distance=BoxDistance(local,candidate->Visual()->getVisData().box);
        if (distance>=best) continue;
        Fvector centre; candidate->Center(centre);
        if (!VrClearLine(actor,eye,centre) && !VrClearLine(actor,hand.c,centre)) continue;
        best=distance; best_object=candidate;
    }
    if (best_object) { touching=true; return best_object; }
    if (near_only) { why="no item within 12 cm of the hand"; return nullptr; }
    // Pointing through a wall the hand was pushed into takes nothing.
    if (!VrHandReachable(actor,hand.c)) { why="hand behind a wall from the eyes"; return nullptr; }
    collide::rq_result hit;
    if (!pointer_missed && VrPointerPick(actor,side,hit))
        if (auto* pointed=smart_cast<CPhysicsShellHolder*>(hit.O); VrTakeable(actor,side,pointed)) return pointed;
    // The pointer's ray missed (a thin or small item, grass in the way): the
    // item nearest it, by angle.
    nearby.clear();
    Fvector middle; middle.mad(aim.c,aim.k,1.f);
    g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,middle,1.1f);
    float best_angle=FLT_MAX;
    for (ISpatial* spatial:nearby)
    {
        auto* candidate=smart_cast<CPhysicsShellHolder*>(spatial->dcast_CObject());
        if (!VrTakeable(actor,side,candidate)) continue;
        Fvector centre; candidate->Center(centre);
        float along=0.f, angle=0.f;
        if (!vr::PointerCone(Pick(aim.c),Pick(aim.k),Pick(centre),candidate->Visual()->getVisData().sphere.R,along,angle))
            continue;
        if (angle>=best_angle || !VrClearLine(actor,aim.c,centre)) continue;
        best_angle=angle; best_object=candidate;
    }
    if (!best_object) why="nothing takeable under the hand or along its pointer";
    return best_object;
}

// World items stay world objects until the player places one over the
// shoulder. The normal pickup event retains inventory and script authority.
// Kept separate so its early returns never swallow the empty-handed use.
// Each free hand grabs, carries and stores its own item.
void UpdateVrPickup(CActor& actor, int side, bool available, bool grip, bool trigger, bool may_grab,
    bool at_weapon)
{
    auto& state=actor.m_vr_pickup;
    u16& grabbed_id=state.grabbed_id[side];
    Fmatrix& grab_offset=state.grab_offset[side];
    Fvector& previous_grab_position=state.previous_grab_position[side];
    bool& pickup_armed=state.pickup_armed[side];
    Fmatrix hand,aim;
    // available already excludes talking (UpdateVrWeapons).
    const bool valid=available && VrControlsFree(actor) &&
        vr::HandTransform(side,false,hand) && vr::HandTransform(side,true,aim);
    auto* grabbed=grabbed_id!=u16(-1) ? smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(grabbed_id)) : nullptr;
    if (!grabbed || grabbed->getDestroy() || grabbed->H_Parent() || !grabbed->PPhysicsShell() ||
        !grabbed->PPhysicsShell()->isActive())
    { grabbed_id=u16(-1); grabbed=nullptr; }
    if (!valid) { grabbed_id=u16(-1); pickup_armed=false; return; }
    // The grip with the same hysteresis as everything else the hand holds.
    const bool down=grip || trigger;
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
        // A carried slot weapon (taken while the other hand held one) brought
        // to its holster on the carrying hand's side goes into that holster,
        // when it is free; otherwise only the backpack takes it.
        if (auto* weapon=smart_cast<CWeapon*>(grabbed))
        {
            const u16 slot=weapon->BaseSlot();
            Fvector holster;
            float range=0.f;
            if ((slot==INV_SLOT_2 || slot==INV_SLOT_3) && !actor.inventory().ItemFromSlot(slot) &&
                VrSlotHolster(actor,*weapon,slot,side==1,holster,range) && hand.c.distance_to(holster)<=range &&
                actor.inventory().CanTakeItem(weapon))
            {
                grabbed_id=u16(-1);
                pickup_armed=false;
                state.wield_id=weapon->ID();
                state.wield_hand=side;
                state.wield_stow=true;
                state.wield_expires=Device.dwTimeGlobal+3000;
                Game().SendPickUpEvent(actor.ID(),weapon->ID());
                vr::HandHaptic(side,0.22f);
                if (vr::ContactDebug()>0) Msg("* [VR pickup] hand %d: %s into its holster",side,weapon->cName().c_str());
                return;
            }
        }
        // Over either shoulder (either hand reaches the backpack), at the
        // tracked head's height.
        const bool to_back=VrAtBackpack(actor,hand.c);
        // Ammunition carried to the ammunition pouch goes into it, as over
        // the shoulder.
        const bool to_pouch=smart_cast<CWeaponAmmo*>(grabbed) && Near(hand.c,VrPouchPoint(actor),vr_pouch_reach);
        if (to_pouch || to_back)
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
        // At the weapon (on it, along its barrel) only an item the hand is on
        // is taken; a squeeze there otherwise is for the weapon.
        bool touching=false;
        const char* why="";
        CPhysicsShellHolder* item_object=VrGrabTarget(actor,side,at_weapon,touching,why);
        if (!item_object)
        {
            if (vr::ContactDebug()>0) Msg("* [VR pickup] hand %d: %s%s",side,why,at_weapon ? " (at the weapon)" : "");
            return;
        }
        // No actor_on_item_before_pickup here: that gates the use key's pickup
        // into the inventory, and taking an item into the hand is not one (it
        // enters the inventory when stowed, by the pickup event, which scripts
        // see as actor_on_item_take). GAMMA's FDDA Redone answers every one
        // with "no" and queues its own animated pickup straight into the
        // backpack: asked, it refused every grab and took the item later.
        // A slot weapon taken with an empty hand goes straight into that
        // hand (see UpdateVrWeapons) rather than being carried, also while
        // the weapon just let go is still being put away, when its slot is
        // free (vr::TakeIntoHand). With a weapon in the other hand it is
        // carried: one weapon is held at a time.
        auto* weapon=smart_cast<CWeapon*>(item_object);
        const bool slot_weapon=weapon && (weapon->BaseSlot()==INV_SLOT_2 || weapon->BaseSlot()==INV_SLOT_3);
        PIItem occupant=slot_weapon ? actor.inventory().ItemFromSlot(weapon->BaseSlot()) : nullptr;
        const vr::ActiveItem hands=!actor.inventory().ActiveItem() ? vr::ActiveItem::None :
            VrActiveItemLeaving(actor) ? vr::ActiveItem::OtherLeaving : vr::ActiveItem::OtherHeld;
        if (slot_weapon && vr::TakeIntoHand(hands,!occupant || occupant==weapon))
        {
            state.wield_id=weapon->ID();
            state.wield_hand=side;
            state.wield_stow=false;
            state.wield_expires=Device.dwTimeGlobal+3000;
            Game().SendPickUpEvent(actor.ID(),weapon->ID());
            vr::HandHaptic(side,0.2f);
            if (vr::ContactDebug()>0) Msg("* [VR pickup] hand %d: wielding %s",side,weapon->cName().c_str());
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
        if (vr::ContactDebug()>0)
        {
            PIItem active=actor.inventory().ActiveItem();
            Msg("* [VR pickup] hand %d: took %s (%s)%s%s",side,item_object->cName().c_str(),
                touching ? "under the hand" : "pointed", weapon ? ", carried: " : "",
                !weapon ? "" : active ? active->object().cName().c_str() : "not a slot weapon");
        }
    }
}

// Hand and weapon bodies push loose physics objects only: never the static
// world, a character (the player's body included), a character's controller,
// or each other.
