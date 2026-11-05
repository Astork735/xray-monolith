// Where the VR body carries things: torso frame, shoulders, hips, the pouch and the holsters.
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

namespace vrwi
{
enum class HolsterPlace { Hip, Shoulder, OffShoulder };
// Where a slot weapon is carried, by what it is (VrHandgun), not by its slot:
// a handgun on the hip, a long gun over the shoulder. Two long guns: slot 3's
// over the off-hand shoulder (the left, right-handed), so an empty hand
// closing there takes it; two handguns: slot 3's over the shoulder, as before.
HolsterPlace VrHolsterPlace(CActor& actor, CWeapon& weapon, u16 slot)
{
    auto* other=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot==INV_SLOT_3 ? INV_SLOT_2 : INV_SLOT_3));
    if (other==&weapon) other=nullptr;
    const bool handgun=VrHandgun(weapon), other_handgun=other && VrHandgun(*other);
    if (!handgun) return slot==INV_SLOT_3 && other && !other_handgun ? HolsterPlace::OffShoulder : HolsterPlace::Shoulder;
    return slot==INV_SLOT_3 && other && other_handgun ? HolsterPlace::Shoulder : HolsterPlace::Hip;
}
// How far from its holster a hand draws a weapon or puts it back: the hip
// holster no further than the hand would be on the pistol's grip, so the
// belt beside it and a hand hanging at the side stay clear of it.
float HolsterRange(HolsterPlace place) { return place==HolsterPlace::Hip ? 0.18f : 0.30f; }
}
bool VrSlotHolster(CActor& actor, CWeapon& weapon, u16 slot, bool right, Fvector& point, float& range)
{
    if (slot!=INV_SLOT_2 && slot!=INV_SLOT_3) return false;
    const HolsterPlace place=VrHolsterPlace(actor,weapon,slot);
    // The off-hand shoulder is fixed (vr_left_handed), not the hand that last
    // held the weapon: the second long gun is always found there.
    if (place==HolsterPlace::OffShoulder) point=VrShoulderPoint(actor,vr::DominantHand()!=1);
    else if (place==HolsterPlace::Shoulder) point=VrShoulderPoint(actor,right);
    else point=VrHipPoint(actor,right);
    range=HolsterRange(place);
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
bool VrAtBackpack(CActor& actor, const Fvector& point)
{
    return Near(point,VrShoulderPoint(actor,true),0.29f) || Near(point,VrShoulderPoint(actor,false),0.29f);
}

Fvector VrPouchPoint(CActor& actor)
{
    // On the free hand's hip. The rig mirrors with the weapon hand: the
    // weapon's holster is on the other side (HolsterPoint), so they never
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

