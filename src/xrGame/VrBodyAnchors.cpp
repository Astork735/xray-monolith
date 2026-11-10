// Where the VR body carries things: torso frame, shoulders, hips, the pouch and the holsters.
#include "stdafx.h"
#include "VrWeaponInteractionInternal.h"
#include "VrBodySlots.h"
#include "Weapon.h"
#include "Actor.h"
#include "inventory.h"
#include "../xrEngine/Vr/vr_gameplay.h"
using namespace vrwi;

namespace vrwi
{
u16 OtherWeaponSlot(u16 slot) { return slot==INV_SLOT_3 ? INV_SLOT_2 : INV_SLOT_3; }
// The weapon in the other weapon slot, if it is another weapon.
CWeapon* OtherSlotWeapon(CActor& actor, CWeapon& weapon, u16 slot)
{
    auto* other=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(OtherWeaponSlot(slot)));
    return other!=&weapon ? other : nullptr;
}
// How far from its holster a hand draws a weapon or puts it back: the hip
// holster no further than the hand would be on the pistol's grip, so the
// belt beside it and a hand hanging at the side stay clear of it.
// The shoulder holster: 36 cm (30 cm was easy to miss reaching back blind).
float HolsterRange(bool hip) { return hip ? 0.18f : 0.36f; }
// The shoulder holster's height below the tracked eyes, whatever the body's
// pelvis or vr_hip_height (a hand reaches over the top of the shoulder).
constexpr float shoulder_drop=0.15f;
// VrSlotShoulderRight with the long gun in the other weapon slot, if any,
// already found.
bool ShoulderRight(CActor& actor, CWeapon& weapon, u16 slot, bool right, CWeapon* other)
{
    // Put there by hand while in this slot (-1: not).
    const auto put=[&](CWeapon& gun, u16 at) -> int
    {
        const CVrWeaponInteraction* vr=actor.inventory().ItemFromSlot(at)==&gun ? gun.VrInteraction() : nullptr;
        return vr ? vr->HolsterShoulder() : -1;
    };
    // Where put by hand; else the slot's rule. The off-hand shoulder is
    // fixed (vr_left_handed), not the hand that last held the weapon: the
    // second long gun is found there.
    const auto side=[](int put_side, u16 at, bool hand_right, bool second) -> bool
    {
        return vr::LongGunShoulderRight(put_side,at==INV_SLOT_3,second,hand_right,vr::DominantHand()==1);
    };
    const int mine_put=put(weapon,slot);
    const bool mine=side(mine_put,slot,right,other!=nullptr);
    // The other in the hand hangs nowhere yet.
    if (!other || actor.inventory().ActiveItem()==other) return mine;
    const u16 other_slot=OtherWeaponSlot(slot);
    const int theirs_put=put(*other,other_slot);
    const bool theirs=side(theirs_put,other_slot,other->VrWeaponHand()==1,true);
    if (mine!=theirs) return mine;
    // One shoulder for both (one put there by hand, the other following
    // its slot's rule): the one put there keeps it; with neither or both,
    // slot 2's. Drawn on one shoulder, one hid inside the other and was
    // only found again by the hand.
    const bool keep=(mine_put>=0)!=(theirs_put>=0) ? mine_put>=0 : slot==INV_SLOT_2;
    return keep ? mine : !mine;
}
}
bool VrSlotShoulderRight(CActor& actor, CWeapon& weapon, u16 slot, bool right)
{
    CWeapon* other=OtherSlotWeapon(actor,weapon,slot);
    return ShoulderRight(actor,weapon,slot,right,other && !VrHandgun(*other) ? other : nullptr);
}
u16 VrFreeWeaponSlot(CActor& actor, CWeapon& weapon)
{
    const u16 base=weapon.BaseSlot();
    if (base!=INV_SLOT_2 && base!=INV_SLOT_3) return NO_ACTIVE_SLOT;
    CInventory& inventory=actor.inventory();
    const u16 current=weapon.CurrSlot();
    if ((current==INV_SLOT_2 || current==INV_SLOT_3) && inventory.ItemFromSlot(current)==&weapon) return current;
    for (u16 slot:{base,OtherWeaponSlot(base)})
        if (!inventory.ItemFromSlot(slot) && inventory.CanPutInSlot(&weapon,slot)) return slot;
    return NO_ACTIVE_SLOT;
}
bool VrSlotHolster(CActor& actor, CWeapon& weapon, u16 slot, bool right, Fvector& point, float& range)
{
    if (slot!=INV_SLOT_2 && slot!=INV_SLOT_3) return false;
    // Where a slot weapon is carried, by what it is (VrHandgun), not by its
    // slot: a handgun on the hip, a long gun over its shoulder
    // (VrSlotShoulderRight: two long guns, slot 3's over the off-hand one,
    // so an empty hand closing there takes it). Two handguns: slot 3's over
    // the hand's shoulder, as before.
    CWeapon* other=OtherSlotWeapon(actor,weapon,slot);
    const bool handgun=VrHandgun(weapon), other_handgun=other && VrHandgun(*other);
    const bool hip=handgun && !(slot==INV_SLOT_3 && other_handgun);
    if (hip) point=VrHipPoint(actor,right);
    else point=VrShoulderPoint(actor,handgun ? right : ShoulderRight(actor,weapon,slot,right,other_handgun ? nullptr : other));
    range=HolsterRange(hip);
    return true;
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

// The torso's frame for what hangs on the shoulders: the body's facing
// when it is shown (it turns apart from the head), else VrTorsoFrame's.
static Fmatrix VrShoulderFrame(CActor& actor)
{
    Fmatrix body=VrTorsoFrame(actor);
    Fmatrix pelvis;
    if (vr::BodyVisible() && actor.m_legs_controller.vr_body_anchor(pelvis))
    {
        Fvector forward=pelvis.k; forward.y=0.f;
        if (forward.square_magnitude()>EPS_S)
        {
            body.k=forward.normalize();
            body.i.crossproduct(body.j,body.k).normalize_safe();
        }
    }
    return body;
}
Fvector VrShoulderPoint(CActor& actor, bool right)
{
    // From the tracked head: 24 cm aside, 20 cm back and shoulder_drop
    // down, in the shoulders' frame. Hung from the body's pelvis, which sits
    // about 20 cm behind the eyes, the holster was 40 cm behind them, and at
    // a height set by vr_hip_height.
    Fvector head;
    const bool head_tracked=vr::HeadPosition(head);
    const Fmatrix body=VrShoulderFrame(actor);
    // Untracked: 1.64 m eyes.
    Fvector origin;
    if (head_tracked) origin.set(head.x,head.y-shoulder_drop,head.z);
    else { origin=actor.Position(); origin.y+=1.64f-shoulder_drop; }
    // The spread follows the player's shoulders (measured by body tracking,
    // or vr_shoulder_width): 0.85..1.2 times the average's.
    const float spread=0.24f*vr::ShoulderWidthScale();
    Fvector offset; offset.set(right ? spread : -spread,0.f,-0.20f);
    body.transform_dir(offset);
    origin.add(offset);
    return origin;
}
float VrHolsterDistance(CActor& actor, const Fvector& holster, const Fvector& hand)
{
    Fvector to; to.sub(hand,holster);
    // A shoulder's point is 15 cm under the eyes, the hips' some 60 cm.
    Fvector head;
    if (!vr::HeadPosition(head) || holster.y<head.y-0.40f) return to.magnitude();
    // Reached over and behind the shoulder: ahead of the point, along the
    // body's facing, counts twice. A shouldered rifle's grip hand, in front
    // of the face, was within the 36 cm sphere: letting go of the grip to
    // reload put the rifle away, or refused it to the fore-end hand.
    const Fvector forward=VrShoulderFrame(actor).k;
    const float ahead=to.dotproduct(forward);
    if (ahead>0.f) to.mad(forward,ahead);
    return to.magnitude();
}
bool VrAtBackpack(CActor& actor, const Fvector& point)
{
    return VrHolsterDistance(actor,VrShoulderPoint(actor,true),point)<=0.29f ||
        VrHolsterDistance(actor,VrShoulderPoint(actor,false),point)<=0.29f;
}

Fvector VrPouchPoint(CActor& actor)
{
    // On the free hand's hip. The rig mirrors with the weapon hand: the
    // weapon's holster is on the other side (VrSlotHolster), so they never
    // share a hip. Without a weapon, the handedness setting's off-hand hip.
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const int support=weapon ? 1-weapon->VrWeaponHand() : 1-vr::DominantHand();
    return VrHipPoint(actor,support==1,true);
}

Fvector VrHipPoint(CActor& actor, bool right, bool pouch)
{
    // With the body shown, on its hips (see VrBodySlotPoint).
    Fmatrix pelvis;
    if (vr::BodyVisible() && actor.m_legs_controller.vr_body_anchor(pelvis))
    {
        // The pouch is forward on its hip, where it can be seen: the layout's
        // place for it, mirrored to whichever hip it is on.
        vr::BodyPlace place;
        if (pouch && vr::BodySlotOnBody(vr::BodyPouch,right,place))
            return VrOnBodyPlace(actor,pelvis,place.angle,place.height);
        const Fvector at=VrOnBodySurface(actor,right ? vr::HipWidth() : -vr::HipWidth(),0.f,-0.03f);
        Fvector point=pelvis.c;
        point.mad(pelvis.i,at.x).mad(pelvis.j,at.y+VrBodyHipShift()).mad(pelvis.k,at.z);
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

