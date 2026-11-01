#include "stdafx.h"
#include "VrBodySlots.h"
#include "Actor.h"
#include "inventory.h"
#include "Weapon.h"
#include "Level.h"
#include "xr_level_controller.h"
#include "HudItem.h"
#include "CustomDevice.h"
#include "player_hud.h"
#include "ActorEffector.h"
#include "Missile.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "UIGameCustom.h"
#include "ui/UIPdaWnd.h"
#include "VrWeaponInteraction.h"
#include "WeaponKnife.h"
#include "WeaponBinoculars.h"
#include "Flashlight.h"
#include "../xrEngine/Vr/vr_gameplay.h"
#include "../xrEngine/vis_common.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"

namespace
{
u16 InventorySlot(int kind)
{
    switch (kind)
    {
    case vr::BodyKnife: return KNIFE_SLOT;
    case vr::BodyGrenade: return GRENADE_SLOT;
    case vr::BodyBolt: return BOLT_SLOT;
    case vr::BodyBinoculars: return BINOCULAR_SLOT;
    case vr::BodyDetector: return DETECTOR_SLOT;
    case vr::BodyPda: return PDA_SLOT;
    default: return NO_ACTIVE_SLOT;
    }
}
bool Draws(int kind) { return kind<=vr::BodyBinoculars; }
// The item in a quick slot (0-3), if the player carries one. Each lookup
// walks the belt and backpack comparing names.
PIItem PocketItem(CActor& actor, int index)
{
    const char* section=ACTOR_DEFS::g_quick_use_slots[index];
    return section[0] ? actor.inventory().GetAny(section) : nullptr;
}
// The same, looked up once per frame for the update (every slot is checked
// for both hands each frame). Only for the update: an item used there can
// be destroyed before the frame is drawn, so drawing looks it up afresh.
PIItem PocketItemThisFrame(CActor& actor, int index)
{
    static u32 frame=u32(-1);
    static const CActor* owner=nullptr;
    static PIItem items[4];
    if (frame!=Device.dwFrame || owner!=&actor)
    {
        frame=Device.dwFrame;
        owner=&actor;
        for (int i=0; i<4; ++i) items[i]=PocketItem(actor,i);
    }
    return items[index];
}
// The hand is free: not holding the active weapon or busy with its
// magazine, slide or foregrip.
bool HandFree(CActor& actor, int hand)
{
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    if (!weapon || !weapon->VrMode()) return true;
    return hand!=weapon->VrWeaponHand() && !weapon->VrLeftHandBusy();
}
bool Available(CActor& actor, int kind, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    if (!HandFree(actor,hand) || pickup.pocket_held[hand]>=0 || pickup.bolt_hand==hand ||
        pickup.grabbed_id[hand]!=u16(-1) || pickup.slot_hold_hand==hand ||
        (pickup.device_hand==hand && kind!=vr::BodyDetector)) return false;
    // A bolt goes into a free hand whatever the other holds; one at a time.
    if (kind==vr::BodyBolt)
        // Not the bolt that is the active item (drawn by a key): throwing it
        // from the pouch would reject the weapon in use.
        return pickup.bolt_hand<0 && !actor.inventory_disabled() && hand!=VrMissileHand(actor) &&
            actor.inventory().ItemFromSlot(BOLT_SLOT)!=nullptr &&
            actor.inventory().ItemFromSlot(BOLT_SLOT)!=actor.inventory().ActiveItem();
    if (vr::IsPocket(kind))
        return !actor.inventory_disabled() && hand!=VrMissileHand(actor) &&
            PocketItemThisFrame(actor,kind-vr::BodyPocket1)!=nullptr;
    if (Draws(kind))
    {
        // Drawing swaps the active item; a weapon held in the hands is
        // holstered first.
        auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
        if (weapon && weapon->VrMode() && weapon->CurrSlot()!=InventorySlot(kind)) return false;
        PIItem item=actor.inventory().ItemFromSlot(InventorySlot(kind));
        // A grenade or bolt already in a hand is not taken again, nor a
        // knife or binoculars the other hand holds: they are not on the belt.
        if (item && item==actor.inventory().ActiveItem() && VrMissileHand(actor)>=0) return false;
        if (item && pickup.slot_hold_hand>=0 && item->object().ID()==pickup.slot_hold_id) return false;
        return item!=nullptr;
    }
    // In VR a detector or flashlight goes beside any weapon, one in each
    // hand (CCustomDevice::CheckCompatibilityInt).
    if (kind==vr::BodyDetector) return actor.inventory().ItemFromSlot(DETECTOR_SLOT)!=nullptr;
    return !actor.inventory_disabled();
}
void PressAction(CActor& actor, EGameActions action)
{
    // As a key press, so script handlers and UI see what a desktop player does.
    if (const int dik=get_action_dik(action))
    {
        Level().IR_OnKeyboardPress(dik);
        Level().IR_OnKeyboardRelease(dik);
        return;
    }
    actor.IR_OnKeyboardPress(action);
    actor.IR_OnKeyboardRelease(action);
}
}

Fvector VrOnBodySurface(CActor& actor, float x, float height, float z)
{
    // An item placed inside the visible body is moved straight out to 5 cm
    // beyond its surface (its outline measured from the mesh).
    const float surface=actor.m_legs_controller.vr_body_surface(height,x,z);
    const float reach=_sqrt(x*x+z*z), wanted=surface+0.05f;
    if (surface>0.f && reach<wanted)
    {
        if (reach<1e-3f) { x=0.f; z=wanted; }
        else { x*=wanted/reach; z*=wanted/reach; }
    }
    return Fvector().set(x,height,z);
}

Fvector VrOnBodyPlace(CActor& actor, const Fmatrix& pelvis, float angle, float height)
{
    const float x=_sin(angle), z=_cos(angle);
    const float surface=actor.m_legs_controller.vr_body_surface(height,x,z);
    const float reach=(surface>0.f ? surface : 0.15f)+vr::body_slot_margin;
    Fvector point=pelvis.c;
    point.mad(pelvis.i,x*reach).mad(pelvis.j,height).mad(pelvis.k,z*reach);
    return point;
}

bool VrBodySlotPoint(CActor& actor, int kind, Fvector& point)
{
    // With the body shown (vr_body), the places hang from its pelvis and face
    // as its torso does: crouching lowers them, leaning or looking down
    // leaves them on the hips instead of following the head.
    Fmatrix pelvis;
    if (vr::BodyVisible() && actor.m_legs_controller.vr_body_anchor(pelvis))
    {
        // On the front and sides of the body, where looking down sees them.
        vr::BodyPlace place;
        if (vr::BodySlotOnBody(kind,vr::LeftHanded(),place))
        {
            point=VrOnBodyPlace(actor,pelvis,place.angle,place.height);
            return true;
        }
        const vr::BodyVec local=vr::BodySlotLocal(kind,vr::HipHeight(),vr::HipWidth(),vr::LeftHanded());
        Fvector at=VrOnBodySurface(actor,local.x,local.y+vr::HipHeight(),local.z);
        point=pelvis.c;
        point.mad(pelvis.i,at.x).mad(pelvis.j,at.y).mad(pelvis.k,at.z);
        return true;
    }
    Fvector head;
    if (!vr::HeadPosition(head)) { head=actor.Position(); head.y+=1.67f; }
    const Fvector forward=VrTorsoFrame(actor).k;
    const vr::BodyVec world=vr::BodyToWorld({head.x,head.y,head.z},forward.x,forward.z,
        vr::BodySlotLocal(kind,vr::HipHeight(),vr::HipWidth(),vr::LeftHanded()));
    point.set(world.x,world.y,world.z);
    return true;
}

int VrBodySlotCandidate(CActor& actor, int hand, const Fvector& position, float& distance)
{
    distance=FLT_MAX;
    if (!vr::BodySlots()) return -1;
    vr::BodyVec points[vr::BodySlotCount];
    bool available[vr::BodySlotCount];
    for (int kind=0; kind<vr::BodySlotCount; ++kind)
    {
        Fvector point;
        available[kind]=Available(actor,kind,hand) && VrBodySlotPoint(actor,kind,point);
        points[kind]=available[kind] ? vr::BodyVec{point.x,point.y,point.z} : vr::BodyVec{0,0,0};
    }
    return vr::NearestBodySlot(points,available,vr::BodySlotCount,{position.x,position.y,position.z},
        vr::BodySlotRadius(),distance);
}

bool VrBodySlotTake(CActor& actor, int kind, int hand)
{
    vr::HandHaptic(hand,0.2f);
    if (vr::IsPocket(kind))
    {
        auto& pickup=actor.m_vr_pickup;
        pickup.pocket_held[hand]=kind-vr::BodyPocket1;
        pickup.pocket_mouth_since[hand]=0;
        pickup.pocket_trigger_down[hand]=vr::HandTrigger(hand)>0.65f;
        return false;
    }
    if (kind==vr::BodyBolt)
    {
        // Held in this hand; the active item (a weapon in the other hand)
        // stays as it is.
        actor.m_vr_pickup.bolt_hand=hand;
        return false;
    }
    if (Draws(kind))
    {
        const u16 slot=InventorySlot(kind);
        PIItem item=actor.inventory().ItemFromSlot(slot);
        if (!item) return false;
        auto& pickup=actor.m_vr_pickup;
        if (smart_cast<CMissile*>(item))
        {
            // In the hand while the grip is held; at once when it is already
            // out, waiting on the belt.
            pickup.missile_hand=hand;
            pickup.missile_stowed=false;
            if (actor.inventory().ActiveItem()==item) return false;
        }
        if (auto* weapon=smart_cast<CWeapon*>(item))
        {
            weapon->VrSetWeaponHand(hand);
            // Without a VR profile nothing else lets it go: held by the grip.
            if (!weapon->VrSupported()) { pickup.slot_hold_id=weapon->ID(); pickup.slot_hold_hand=hand; }
        }
        // Never a toggle: one already out (drawn by a key) is taken into
        // this hand where it is, and goes back when the hand opens.
        if (actor.inventory().ActiveItem()==item) return false;
        actor.inventory().Activate(slot,true);
        return true;
    }
    switch (kind)
    {
    case vr::BodyDetector:
    {
        // Taken into this hand while its grip is held; one already out is
        // put away.
        auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
        const bool out=device && !device->IsHidden();
        actor.m_vr_pickup.device_hand=out ? -1 : hand;
        actor.m_vr_pickup.device_taken_time=Device.dwTimeGlobal;
        actor.m_vr_pickup.device_trigger_down=vr::HandTrigger(hand)>0.65f;
        actor.m_vr_pickup.device_button_down=vr::HandSecondaryButton(hand);
        PressAction(actor,kDETECTOR);
        break;
    }
    case vr::BodyPda: PressAction(actor,kACTIVE_JOBS); break;
    case vr::BodyInventory: Level().VrToggleInventory(); break;
    }
    return false;
}

bool VrPocketHeld(CActor& actor, int hand)
{
    return actor.m_vr_pickup.pocket_held[hand]>=0;
}

void VrPocketUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands, const bool* grip)
{
    auto& pickup=actor.m_vr_pickup;
    Fvector head, looking;
    const bool head_valid=vr::HeadPosition(head);
    Fmatrix head_world;
    looking=vr::HeadTransform(head_world) ? head_world.k : actor.XFORM().k;
    const vr::BodyVec mouth=vr::MouthPoint({head.x,head.y,head.z},looking.x,looking.z);
    for (int h=0; h<2; ++h)
    {
        int& held=pickup.pocket_held[h];
        if (held<0) continue;
        // Opening the hand puts the item back; so does losing it.
        PIItem item=PocketItemThisFrame(actor,held);
        if (!tracked[h] || !grip[h] || !item || actor.inventory_disabled() || !HandFree(actor,h))
        {
            held=-1;
            continue;
        }
        const bool trigger=vr::HandTrigger(h)>0.65f;
        const bool pulled=trigger && !pickup.pocket_trigger_down[h];
        pickup.pocket_trigger_down[h]=trigger;
        bool at_mouth=false;
        if (head_valid)
            at_mouth=vr::BodyDistance(mouth,{hands[h].c.x,hands[h].c.y,hands[h].c.z})<=vr::mouth_reach;
        u32& since=pickup.pocket_mouth_since[h];
        if (!at_mouth) since=0;
        else if (!since) since=Device.dwTimeGlobal|1;
        if (!pulled && !(since && Device.dwTimeGlobal-since>=vr::mouth_dwell_ms)) continue;
        // Used as the quick-use key would, so scripts can play their
        // animation, refuse it, or count it.
        Msg("* [VR body] quick slot %d: using %s",held+1,item->object().cNameSect().c_str());
        PressAction(actor,EGameActions(kQUICK_USE_1+held));
        vr::HandHaptic(h,0.3f);
        held=-1;
    }
}

int VrMissileHand(CActor& actor)
{
    if (!g_vr_owns_present || !smart_cast<CMissile*>(actor.inventory().ActiveItem()) ||
        actor.m_vr_pickup.missile_stowed) return -1;
    const int hand=actor.m_vr_pickup.missile_hand;
    return hand>=0 ? hand : vr::DominantHand();
}

void VrMissileUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands, const bool* grip)
{
    auto& pickup=actor.m_vr_pickup;
    Fvector body; body.set(0,0,0);
    if (CCharacterPhysicsSupport* physics=actor.character_physics_support())
        if (physics->movement()) physics->movement()->GetCharacterVelocity(body);
    for (int h=0; h<2; ++h)
    {
        // The missile also takes the body's velocity when it is let go.
        Fvector relative; relative.sub(pickup.hand_velocity[h],body);
        if (!tracked[h]) relative.set(0,0,0);
        if (relative.square_magnitude()>=pickup.throw_peak[h].square_magnitude() ||
            Device.dwTimeGlobal-pickup.throw_peak_time[h]>100)
        { pickup.throw_peak[h]=relative; pickup.throw_peak_time[h]=Device.dwTimeGlobal; }
    }
    // A bolt held from the pouch: let go faster than 1.5 m/s it flies with
    // the hand's velocity (times vr_throw_scale) plus the body's; let go
    // gently it is back in the pouch.
    if (const int b=pickup.bolt_hand; b>=0)
    {
        PIItem bolt=actor.inventory().ItemFromSlot(BOLT_SLOT);
        if (!bolt || !tracked[b] || actor.inventory_disabled()) pickup.bolt_hand=-1;
        else if (!grip[b])
        {
            pickup.bolt_hand=-1;
            Fvector velocity=pickup.throw_peak[b];
            const float speed=velocity.magnitude();
            if (speed>1.5f)
            {
                velocity.mul(vr::ThrowScale());
                auto* missile=smart_cast<CMissile*>(bolt);
                const float limit=missile ? 1.25f*missile->GetMaxForce() : 0.f;
                if (limit>0.f && velocity.magnitude()>limit) velocity.normalize_safe().mul(limit);
                velocity.add(body);
                VrThrowHandBolt(actor,*bolt,hands[b],velocity,b);
                Msg("* [VR body] bolt thrown from the hand at %.1f m/s (hand %.1f m/s)",velocity.magnitude(),speed);
            }
            else vr::HandHaptic(b,0.1f);
        }
    }
    auto* missile=smart_cast<CMissile*>(actor.inventory().ActiveItem());
    if (!missile)
    {
        // Kept while a grenade or bolt taken by a hand is still being drawn.
        const u16 next=actor.inventory().GetNextActiveSlot();
        if (next!=GRENADE_SLOT && next!=BOLT_SLOT) { pickup.missile_hand=-1; pickup.missile_stowed=false; }
        pickup.missile_grip=pickup.missile_pending=false;
        return;
    }
    // A prime still on its way when the missile went back on the belt (let
    // go a frame after the squeeze) is undone once it lands, or a readied
    // grenade would wait there.
    if (pickup.missile_stowed && missile->VrPrimed() && !missile->VrThrown()) missile->VrUnprime();
    // Drawn by a key, the wheel or a script, not taken by a hand: on the belt.
    // The dominant hand may be holding something else, and readying it there
    // would throw it when that hand opens.
    if (pickup.missile_hand<0 && !missile->VrThrown() && !pickup.missile_pending)
    {
        if (missile->VrPrimed()) missile->VrUnprime();
        pickup.missile_stowed=true;
    }
    const int h=VrMissileHand(actor);
    if (h<0) return;
    const bool held=tracked[h] && grip[h];
    // In the hand only while the hand holds it: one let go while it was
    // still being drawn (or drawn by a key, never held) goes on the belt.
    if (!held && !pickup.missile_grip && !pickup.missile_pending && !missile->VrPrimed() &&
        !missile->VrThrown() && tracked[h])
    {
        pickup.missile_stowed=true;
        return;
    }
    const bool released=pickup.missile_grip && !held && tracked[h];
    if (held && !pickup.missile_grip) pickup.missile_grip_since=Device.dwTimeGlobal;
    pickup.missile_grip=held;
    // Keeping the hand closed on it readies it (the pin, for a grenade).
    if (held && !missile->VrThrown())
    {
        pickup.missile_pending=false;
        missile->VrPrime();
        return;
    }
    if (released)
    {
        Fvector velocity=pickup.throw_peak[h];
        const float speed=velocity.magnitude();
        if (speed>1.5f)
        {
            velocity.mul(vr::ThrowScale());
            const float limit=1.25f*missile->GetMaxForce();
            if (limit>0.f && velocity.magnitude()>limit) velocity.normalize_safe().mul(limit);
            pickup.missile_pending=true;
            pickup.missile_pending_until=Device.dwTimeGlobal+1000;
            pickup.missile_matrix=hands[h];
            // With physical hands, let go through a wall it leaves on this side.
            if (vr::PhysicalHands()) VrNearSide(pickup.missile_matrix.c,0.05f);
            pickup.missile_velocity=velocity;
            Msg("* [VR body] %s: thrown at %.1f m/s (hand %.1f m/s)",missile->cNameSect().c_str(),
                velocity.magnitude(),speed);
        }
    }
    if (pickup.missile_pending)
    {
        // Let go during the throw start: it flies as soon as it is ready.
        if (missile->VrThrow(pickup.missile_matrix,pickup.missile_velocity))
        {
            vr::HandHaptic(h,0.25f);
            pickup.missile_pending=false;
            // The hand is empty; the next one is taken from the belt.
            pickup.missile_stowed=true;
        }
        else if (int(Device.dwTimeGlobal-pickup.missile_pending_until)>0) pickup.missile_pending=false;
        return;
    }
    // Opened gently: not readied, and back on the belt.
    if (!held)
    {
        missile->VrUnprime();
        if (released)
        {
            pickup.missile_stowed=true;
            vr::HandHaptic(h,0.1f);
        }
    }
}

bool VrHandHolding(CActor& actor, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    // The active weapon's hand too: the knife, binoculars or a gun. Guns hid
    // it from their own render; the knife no longer goes through that path.
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    return hand==pickup.device_hand || hand==pickup.slot_hold_hand || hand==pickup.bolt_hand ||
        hand==VrMissileHand(actor) || pickup.pocket_held[hand]>=0 || pickup.grabbed_id[hand]!=u16(-1) ||
        (weapon && weapon->VrWeaponHand()==hand);
}

bool VrHandAtHead(const Fvector& position)
{
    // Beside or above the eyes, within 20 cm of the head's centre; below
    // them is the mouth (pockets) and in front of them the sights.
    Fmatrix head;
    if (!vr::HeadTransform(head)) return false;
    Fvector to; to.sub(position,head.c);
    return to.magnitude()<0.20f && to.dotproduct(head.j)>-0.02f && to.dotproduct(head.k)<0.08f;
}

namespace
{
bool head_trigger_down[2]{}, head_button_down[2]{};
// The lower face button (X or A) held 1 s at the head: quick save.
bool head_save_down[2]{}, head_save_done[2]{true,true}, head_at[2]{};
u32 head_save_since[2]{};
}

bool VrHeadGesture(int hand)
{
    return hand>=0 && hand<2 && head_at[hand];
}

// Both hands at the head: both stick clicks step the debug level there, and
// neither sprints nor jumps.
bool head_debug_hands=false;
bool VrDebugGesture() { return head_debug_hands; }

void VrDebugGestureUpdate(const bool* tracked, const Fmatrix* hands)
{
    static bool down=true;
    static u32 flushed=0;
    head_debug_hands=tracked[0] && tracked[1] && VrHandAtHead(hands[0].c) && VrHandAtHead(hands[1].c);
    const bool both=head_debug_hands && vr::SprintButton() && vr::JumpButton();
    if (both && !down)
    {
        const int level=(vr::ContactDebug()+1)%3;
        string64 command;
        xr_sprintf(command,"vr_contact_debug %d",level);
        Console->Execute(command);
        Msg("* [VR debug] level %d",level);
        FlushLog();
        for (int h=0;h<2;++h) vr::HandHaptic(h,level ? 0.3f+0.3f*float(level) : 0.15f);
    }
    down=both;
    // A session in debug survives a crash or a killed game.
    if (vr::ContactDebug()>0 && Device.dwTimeGlobal-flushed>5000)
    {
        flushed=Device.dwTimeGlobal;
        FlushLog();
    }
}

void VrHeadGestureUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands)
{
    VrDebugGestureUpdate(tracked,hands);
    for (int h=0; h<2; ++h)
    {
        const bool at=tracked[h] && HandFree(actor,h) && actor.m_vr_pickup.pocket_held[h]<0 &&
            actor.m_vr_pickup.bolt_hand!=h && h!=VrMissileHand(actor) &&
            h!=actor.m_vr_pickup.slot_hold_hand && h!=actor.m_vr_pickup.device_hand && VrHandAtHead(hands[h].c);
        const bool trigger=at && vr::HandTrigger(h)>0.65f;
        const bool button=at && vr::HandSecondaryButton(h);
        if (trigger && !head_trigger_down[h])
        {
            PressAction(actor,kTORCH);
            vr::HandHaptic(h,0.12f);
        }
        if (button && !head_button_down[h])
        {
            PressAction(actor,kNIGHT_VISION);
            vr::HandHaptic(h,0.12f);
        }
        // X or A: a tick when pressed, a pulse and the save after 1 s.
        const bool save=at && vr::HandButton(h);
        if (save && !head_save_down[h])
        {
            head_save_since[h]=Device.dwTimeGlobal;
            head_save_done[h]=false;
            vr::HandHaptic(h,0.06f);
        }
        if (save && !head_save_done[h] && Device.dwTimeGlobal-head_save_since[h]>=1000)
        {
            head_save_done[h]=true;
            vr::RequestQuickSave();
            vr::HandHaptic(h,0.5f);
        }
        if (!at) head_save_done[h]=true;
        head_at[h]=at;
        // Edges only from a hand already at the head: one arriving with
        // the trigger held does nothing until it is pulled again.
        head_trigger_down[h]=at ? trigger : true;
        head_button_down[h]=at ? button : true;
        head_save_down[h]=at ? save : true;
        if (!tracked[h]) head_trigger_down[h]=head_button_down[h]=head_save_down[h]=false;
    }
}

bool VrHandAtEquipment(CActor& actor, const Fvector& position)
{
    // The helmet: headlamp and night vision (VrHeadGestureUpdate).
    if (VrHandAtHead(position)) return true;
    if (vr::BodySlots())
        for (int kind=0; kind<vr::BodySlotCount; ++kind)
        {
            Fvector point;
            if (VrBodySlotPoint(actor,kind,point) && point.distance_to(position)<=vr::BodySlotReach(kind,vr::BodySlotRadius()))
                return true;
        }
    // A holster with its weapon in it, as far as a hand draws from it, and
    // the ammunition pouch while a weapon is in the hand. Not the whole hip:
    // a hand low in front of the body grabs and uses what it points at.
    PIItem active=actor.inventory().ActiveItem();
    for (u16 slot:{INV_SLOT_2,INV_SLOT_3})
    {
        auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot));
        float distance;
        if (weapon && weapon!=active && weapon->VrHolsterGrabDistance(actor,position,distance)) return true;
    }
    return smart_cast<CWeapon*>(active) && VrPouchPoint(actor).distance_to(position)<=vr_pouch_reach;
}

void VrPdaUpdate(CActor& actor)
{
    auto& pickup=actor.m_vr_pickup;
    CUIGameCustom* ui=CurrentGameUI();
    const bool open=ui && ui->GetPdaMenu().IsShown() && vr::BodySlots() && vr::GameplayActive();
    Fvector point;
    for (int h=0; h<2; ++h)
    {
        // A new squeeze only: the one that opened it may still be held.
        Fmatrix hand;
        const bool squeezed=vr::HandTransform(h,false,hand) && vr::HandSqueeze(h)>0.65f;
        if (open && squeezed && !pickup.pda_grip[h] && VrBodySlotPoint(actor,vr::BodyPda,point) &&
            hand.c.distance_to(point)<=vr::BodySlotRadius())
        {
            ui->HidePdaMenu();
            vr::HandHaptic(h,0.2f);
        }
        pickup.pda_grip[h]=squeezed;
    }
}

void ReleaseVrBodySlots(CActor& actor)
{
    auto& pickup=actor.m_vr_pickup;
    if (pickup.bolt_pouch) ::Render->model_Delete(pickup.bolt_pouch);
    pickup.bolt_pouch=nullptr;
    pickup.bolt_pouch_missing=false;
}

int VrDetectorHand(CActor& actor)
{
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    if (!g_vr_owns_present || !device || device->IsHidden()) return -1;
    // The hand that took it from the belt, else the hand without the weapon.
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const int weapon_hand=weapon && weapon->VrMode() ? weapon->VrWeaponHand() : -1;
    const int taken=actor.m_vr_pickup.device_hand;
    if (taken>=0 && taken!=weapon_hand) return taken;
    // Not taken by a hand: the free one, unless it is working the weapon.
    if (weapon_hand>=0 && weapon->VrLeftHandBusy()) return -1;
    return weapon_hand>=0 ? 1-weapon_hand : 1-vr::DominantHand();
}

bool VrHandHoldsItem(CActor& actor, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    return pickup.grabbed_id[hand]!=u16(-1) || pickup.pocket_held[hand]>=0 || pickup.bolt_hand==hand ||
        pickup.slot_hold_hand==hand || hand==VrMissileHand(actor) ||
        (pickup.device_hand==hand && VrDetectorHand(actor)==hand);
}

namespace
{
// The body slot the item in this hand goes back to, or -1: the bolt to its
// pouch, the knife, binoculars, a grenade or the detector to their places.
int ReturnSlot(CActor& actor, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    if (pickup.bolt_hand==hand) return vr::BodyBolt;
    if (pickup.device_hand==hand) return vr::BodyDetector;
    if (pickup.slot_hold_hand==hand)
        for (int kind:{vr::BodyKnife,vr::BodyBinoculars})
            if (PIItem item=actor.inventory().ItemFromSlot(InventorySlot(kind));
                item && item->object().ID()==pickup.slot_hold_id) return kind;
    PIItem active=actor.inventory().ActiveItem();
    if (VrMissileHand(actor)==hand && active && active->CurrSlot()==GRENADE_SLOT) return vr::BodyGrenade;
    return -1;
}
}

void VrBodySlotHover(CActor& actor, const bool* tracked, const Fmatrix* hands)
{
    for (int h=0; h<2; ++h)
    {
        float distance=0.f;
        int kind=tracked[h] ? VrBodySlotCandidate(actor,h,hands[h].c,distance) : -1;
        // A hand holding a belt item shows that item's place, where letting
        // go puts it back.
        Fvector home;
        const int back=tracked[h] ? ReturnSlot(actor,h) : -1;
        if (back>=0 && VrBodySlotPoint(actor,back,home) &&
            home.distance_to(hands[h].c)<=vr::BodySlotReach(back,vr::BodySlotRadius())) kind=back;
        int& hover=actor.m_vr_pickup.body_hover[h];
        if (kind>=0 && kind!=hover) vr::HandHaptic(h,0.08f);
        hover=kind;
    }
}

namespace
{
// How the game shows each detector in first person, relative to the HUD
// camera, measured once it has settled in its idle (see RenderVrDetector).
xr_map<shared_str,Fmatrix> g_detector_holds;

void RenderVrDetector(CActor& actor)
{
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    Fmatrix pose;
    if (!device || !VrDevicePose(actor,pose)) return;
    IRenderVisual* visual=device->HudItemData()->m_model->dcast_RenderVisual();
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(visual);
}
}

namespace
{
// How the game holds a first-person item relative to the HUD camera, from
// its flat transform before VR replaces it. Until it settles in its idle
// (the show animation) the current pose is used.
bool FirstPersonHold(CActor& actor, CHudItem& item, Fmatrix& hold)
{
    attachable_hud_item* hud=item.HudItemData();
    if (!hud || !hud->m_model) return false;
    auto found=g_detector_holds.find(hud->m_sect_name);
    if (found!=g_detector_holds.end()) { hold=found->second; return true; }
    Fmatrix camera, inverse;
    actor.Cameras().hud_camera_Matrix(camera);
    inverse.invert(camera);
    hold.mul_43(inverse,hud->m_item_transform);
    hold.c.set(0,0,0);
    if (item.GetState()==CHUDState::eIdle)
    {
        g_detector_holds[hud->m_sect_name]=hold;
        Msg("* [VR body] %s: holding as shown in first person",hud->m_sect_name.c_str());
    }
    return true;
}

// Where the first-person idle animation's right hand grips an item, in the
// item's frame, as SampleAuthoredHands does for guns: the hand's axes
// (toward the knuckles, toward the index side, and their cross) and the
// centre of the fist. Sampled once per hands and item section. Also every
// bone's local pose and the hand bone in the item's frame, so the arms draw
// the animation's own hand and fingers around it (VrKnifeBones).
struct AuthoredGrip
{
    bool valid=false;
    Fvector basis[3];
    Fvector palm;
    Fmatrix hand_in_item;
    xr_vector<Fmatrix> locals;
    shared_str visual;
};
xr_map<shared_str,AuthoredGrip> g_authored_grips;
// left: the animation's left hand holds it (detectors and flashlights are
// held in the left hand in first person; a knife in the right).
const AuthoredGrip* SampleAuthoredGrip(const shared_str& section, bool left=false)
{
    if (!g_player_hud || !section.size()) return nullptr;
    const shared_str& hands=g_player_hud->section_name();
    string512 key; xr_sprintf(key,"%s|%s|%c",hands.c_str(),section.c_str(),left ? 'l' : 'r');
    const shared_str name=key;
    auto found=g_authored_grips.find(name);
    if (found!=g_authored_grips.end()) return found->second.valid ? &found->second : nullptr;
    AuthoredGrip& grip=g_authored_grips[name];
    if (!hands.size() || !pSettings->section_exist(section) || !pSettings->line_exist(hands,"visual") ||
        !pSettings->line_exist(section,"item_position") || !pSettings->line_exist(section,"item_orientation"))
        return nullptr;
    player_hud_motion_container* motions=g_player_hud->get_hand_motions(section.c_str());
    player_hud_motion* idle=motions ? motions->find_motion("anm_idle") : nullptr;
    if (!idle || idle->m_animations.empty()) return nullptr;
    xr_vector<shared_str> anchors;
    for (const auto& item:pSettings->r_section(hands).Data)
        if (strstr(item.first.c_str(),"ancor_")==item.first.c_str()) anchors.push_back(item.second);
    const bool lead=READ_IF_EXISTS(pSettings,r_bool,section,"lh_lead_gun",false);
    const u16 place=READ_IF_EXISTS(pSettings,r_u16,section,"attach_place_idx",0);
    const size_t anchor_index=lead ? 0 : place;
    if (anchor_index>=anchors.size()) return nullptr;
    IRenderVisual* visual=::Render->model_Create(pSettings->r_string(hands,"visual"));
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* model=visual ? visual->dcast_PKinematics() : nullptr;
    const u16 anchor=model ? model->LL_BoneID(anchors[anchor_index]) : BI_NONE;
    const u16 wrist=model ? model->LL_BoneID(left ? "l_hand" : "r_hand") : BI_NONE;
    const u16 index=model ? model->LL_BoneID(left ? "bip01_l_finger1" : "bip01_r_finger1") : BI_NONE;
    const u16 middle=model ? model->LL_BoneID(left ? "bip01_l_finger2" : "bip01_r_finger2") : BI_NONE;
    if (animated && anchor!=BI_NONE && wrist!=BI_NONE && index!=BI_NONE && middle!=BI_NONE)
    {
        for (u16 part=0;part<animated->partitions().count();++part)
            animated->PlayCycle(part,idle->m_animations[0].mid,FALSE,0,0,0,1.f);
        animated->LL_UpdateTracks(0.f,true,true);
        model->CalculateBones_Invalidate();
        model->CalculateBones(TRUE);
        Fvector ypr=pSettings->r_fvector3(section,"item_orientation");
        ypr.mul(PI/180.f);
        Fmatrix offset; offset.setHPB(ypr.x,ypr.y,ypr.z);
        offset.translate_over(pSettings->r_fvector3(section,"item_position"));
        Fmatrix item, to_item;
        item.mul_43(model->LL_GetTransform(anchor),offset);
        to_item.invert(item);
        const auto at=[&](u16 id) { Fvector p; to_item.transform_tiny(p,model->LL_GetTransform(id).c); return p; };
        const Fvector w=at(wrist), i=at(index), m=at(middle);
        Fvector knuckles; knuckles.add(i,m).mul(0.5f);
        Fvector forward; forward.sub(knuckles,w);
        Fvector side; side.sub(i,m);
        if (forward.square_magnitude()>EPS_S && side.square_magnitude()>EPS_S)
        {
            forward.normalize();
            side.mad(forward,-side.dotproduct(forward)).normalize_safe();
            grip.basis[0]=forward; grip.basis[1]=side;
            grip.basis[2].crossproduct(forward,side);
            // The fist's centre: behind the knuckles, in from the palm (the
            // cross of knuckles and index side is the right palm's normal,
            // the back of a left hand).
            grip.palm=knuckles;
            grip.palm.mad(forward,-0.045f).mad(grip.basis[2],left ? -0.025f : 0.025f);
            grip.hand_in_item.mul_43(to_item,model->LL_GetTransform(wrist));
            const u16 count=model->LL_BoneCount();
            grip.locals.resize(count);
            for (u16 id=0;id<count;++id)
            {
                const u16 parent=model->LL_GetData(id).GetParentID();
                grip.locals[id]=model->LL_GetTransform(id);
                if (parent<count)
                {
                    Fmatrix inverse; inverse.invert(model->LL_GetTransform(parent));
                    grip.locals[id].mulA_43(inverse);
                }
            }
            grip.visual=pSettings->r_string(hands,"visual");
            grip.valid=true;
            Msg("* [VR body] %s: held as the first-person idle's %s hand holds it",section.c_str(),
                left ? "left" : "right");
        }
    }
    if (visual) ::Render->model_Delete(visual);
    return grip.valid ? &grip : nullptr;
}

// The item placed so the animation's hand lies on the controller: its axes
// onto the controller hand's (knuckles forward, index side up the handle,
// as player_hud_legs.cpp poses the hands) and its fist on the grip centre.
bool AuthoredHoldPose(int hand, const AuthoredGrip& authored, Fmatrix& pose)
{
    Fmatrix grip;
    if (!vr::HandTransform(hand,false,grip)) return false;
    Fvector frame[3];
    frame[0].invert(grip.j);
    frame[1].set(grip.k).mad(frame[0],-grip.k.dotproduct(frame[0]));
    if (frame[1].square_magnitude()<EPS_S) return false;
    frame[1].normalize();
    frame[2].crossproduct(frame[0],frame[1]);
    pose.identity();
    Fvector* axes[3]={&pose.i,&pose.j,&pose.k};
    for (int axis=0;axis<3;++axis)
    {
        axes[axis]->set(0,0,0);
        for (int n=0;n<3;++n) axes[axis]->mad(frame[n],authored.basis[n][axis]);
    }
    Fvector palm; pose.transform_dir(palm,authored.palm);
    pose.c.sub(grip.c,palm);
    return true;
}

// The first-person model in a hand. The hand frame takes the HUD camera's
// place (knuckles forward, thumb up), so the item faces the player as on a
// flat screen when the hand is held out in front. A device's lower end sits
// in the fist; a blade's back end (the handle) 5 cm behind the grip centre,
// the blade forward of the knuckles.
bool HandHoldPose(int hand, const Fmatrix& hold, IRenderVisual* visual, bool blade, Fmatrix& pose)
{
    Fmatrix grip;
    if (!visual || !vr::HandTransform(hand,false,grip)) return false;
    // Controller grip: +Y points back along the forearm, +Z up the handle
    // from the little finger to the thumb (as player_hud_legs.cpp poses hands).
    Fmatrix frame; frame.identity();
    frame.k.invert(grip.j);
    frame.j.set(grip.k).mad(frame.k,-grip.k.dotproduct(frame.k));
    if (frame.j.square_magnitude()<EPS_S) return false;
    frame.j.normalize();
    frame.i.crossproduct(frame.j,frame.k);
    pose.mul_43(frame,hold);
    const Fbox& box=visual->getVisData().box;
    Fvector low, high;
    low.set(flt_max,flt_max,flt_max); high.set(-flt_max,-flt_max,-flt_max);
    for (int corner=0; corner<8; ++corner)
    {
        Fvector p, world, local;
        p.set(corner&1 ? box.max.x : box.min.x, corner&2 ? box.max.y : box.min.y,
            corner&4 ? box.max.z : box.min.z);
        pose.transform_dir(world,p);
        local.set(world.dotproduct(frame.i),world.dotproduct(frame.j),world.dotproduct(frame.k));
        low.min(local); high.max(local);
    }
    pose.c=grip.c;
    pose.c.mad(frame.i,-0.5f*(low.x+high.x));
    if (blade)
    {
        pose.c.mad(frame.j,-0.5f*(low.y+high.y));
        pose.c.mad(frame.k,-0.05f-low.z);
    }
    else
    {
        pose.c.mad(frame.j,-0.03f-low.y);
        pose.c.mad(frame.k,-0.5f*(low.z+high.z));
    }
    return true;
}
}

namespace
{
// The active knife or binoculars, held in a hand from the belt as the
// first-person idle animation holds it (no VR weapon profile). blade: the
// knife, whose handle sits in the fist with the blade forward.
CWeapon* BeltItemInHand(CActor& actor, bool* blade=nullptr)
{
    PIItem active=actor.inventory().ActiveItem();
    auto* knife=smart_cast<CWeaponKnife*>(active);
    CWeapon* item=knife ? static_cast<CWeapon*>(knife) : smart_cast<CWeaponBinoculars*>(active);
    if (blade) *blade=knife!=nullptr;
    return item && !item->VrSupported() && item->HudItemData() && item->HudItemData()->m_model ? item : nullptr;
}
}

namespace
{
// Turns an item held in a hand over in the fist (an overhand hold): half a
// turn about the palm's normal, through the fist's centre.
void FlipInHand(int hand, Fmatrix& pose)
{
    Fmatrix grip;
    if (!vr::HandTransform(hand,false,grip)) return;
    Fvector forward; forward.invert(grip.j);
    Fvector handle=grip.k; handle.mad(forward,-handle.dotproduct(forward));
    if (handle.square_magnitude()<EPS_S || forward.square_magnitude()<EPS_S) return;
    handle.normalize(); forward.normalize();
    Fvector axis; axis.crossproduct(handle,forward);
    Fmatrix turn; turn.rotation(axis,PI);
    Fmatrix local=pose; local.c.sub(grip.c);
    pose.mul_43(turn,local);
    pose.c.add(grip.c);
}
}

namespace
{
// The detector or flashlight in its palm and fingers as the first-person
// idle's left hand holds it, on the left controller. In the right hand that
// hold would need a mirrored model: there it keeps the camera-relative hold.
bool DeviceInPalm(int hand, CCustomDevice& device, Fmatrix& pose)
{
    if (hand!=0) return false;
    const AuthoredGrip* authored=SampleAuthoredGrip(device.HudSection(),true);
    return authored && AuthoredHoldPose(hand,*authored,pose);
}
}

void VrUpdateHudItems(CActor& actor)
{
    auto& pickup=actor.m_vr_pickup;
    pickup.device_pose_valid=pickup.knife_pose_valid=false;
    if (!g_vr_owns_present || !vr::GameplayActive()) return;
    Fmatrix hold;
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    const int device_hand=VrDetectorHand(actor);
    if (device && device_hand>=0 && FirstPersonHold(actor,*device,hold) &&
        (DeviceInPalm(device_hand,*device,pickup.device_pose) ||
         HandHoldPose(device_hand,hold,device->HudItemData()->m_model->dcast_RenderVisual(),false,pickup.device_pose)))
    {
        if (pickup.device_flipped && smart_cast<CFlashlight*>(device)) FlipInHand(device_hand,pickup.device_pose);
        pickup.device_pose_valid=true;
        pickup.device_hold=hold;
        // What the device works out from its first-person transform (the
        // Bear's arrow toward an artefact, its lights and display, the
        // flashlight's beam) is then worked out in the hand.
        device->HudItemData()->m_item_transform=pickup.device_pose;
    }
    bool blade=false;
    CWeapon* item=BeltItemInHand(actor,&blade);
    if (item && FirstPersonHold(actor,*item,hold) &&
        HandHoldPose(item->VrWeaponHand(),hold,item->HudItemData()->m_model->dcast_RenderVisual(),blade,
            pickup.knife_pose))
    {
        pickup.knife_pose_valid=true;
        pickup.knife_hold=hold;
    }
}

// Both are placed from the hand as it is now, not as it was at the actor's
// update: the body moves after that, and the item trailed the hand when
// walking.
bool VrDevicePose(CActor& actor, Fmatrix& pose)
{
    const int hand=VrDetectorHand(actor);
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    if (!actor.m_vr_pickup.device_pose_valid || hand<0 || !device || !device->HudItemData()) return false;
    if (DeviceInPalm(hand,*device,pose)) {}
    else if (!HandHoldPose(hand,actor.m_vr_pickup.device_hold,device->HudItemData()->m_model->dcast_RenderVisual(),
        false,pose)) { pose=actor.m_vr_pickup.device_pose; return true; }
    if (actor.m_vr_pickup.device_flipped && smart_cast<CFlashlight*>(device)) FlipInHand(hand,pose);
    return true;
}

bool VrKnifePose(CActor& actor, Fmatrix& pose)
{
    bool blade=false;
    CWeapon* item=BeltItemInHand(actor,&blade);
    if (!actor.m_vr_pickup.knife_pose_valid || !item) return false;
    // As the flat-screen idle animation's hand holds it, when that is known.
    if (const AuthoredGrip* authored=SampleAuthoredGrip(item->HudSection()))
        if (AuthoredHoldPose(item->VrWeaponHand(),*authored,pose)) return true;
    if (!HandHoldPose(item->VrWeaponHand(),actor.m_vr_pickup.knife_hold,
        item->HudItemData()->m_model->dcast_RenderVisual(),blade,pose)) pose=actor.m_vr_pickup.knife_pose;
    return true;
}

bool VrKnifeBones(CActor& actor, int side, VrHandBones& bones)
{
    // The idle animation's right hand, bone for bone: it cannot be mirrored
    // onto the left.
    CWeapon* knife=BeltItemInHand(actor);
    if (!vr::AnimatedGrip() || side!=1 || !knife || knife->VrWeaponHand()!=1)
        return false;
    const AuthoredGrip* authored=SampleAuthoredGrip(knife->HudSection());
    Fmatrix pose;
    if (!authored || authored->locals.empty() || !VrKnifePose(actor,pose)) return false;
    bones.hand.mul_43(pose,authored->hand_in_item);
    bones.locals=&authored->locals;
    bones.visual=authored->visual;
    return true;
}

void RenderVrDeviceUI(CActor& actor)
{
    // Detector displays (the Veles screen) are drawn on the first-person
    // model from its transform, which is the hand's pose (VrUpdateHudItems).
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    Fmatrix pose;
    if (!g_vr_owns_present || !device || !device->render_item_3d_ui_query() || !VrDevicePose(actor,pose)) return;
    attachable_hud_item* hud=device->HudItemData();
    const Fmatrix current=hud->m_item_transform;
    hud->m_item_transform=pose;
    device->render_item_3d_ui();
    hud->m_item_transform=current;
}

bool VrHeldItemPose(int hand, Fmatrix& pose)
{
    if (!vr::GameplayActive() || !vr::HandTransform(hand,false,pose)) return false;
    Fvector offset; offset.set(0.f,-0.025f,0.04f);
    pose.transform_dir(offset);
    pose.c.add(offset);
    return true;
}

namespace
{
// The lens image's apparent size from the eyes (degrees), and how far ahead
// of the head's centre it is drawn.
const float binocular_apparent=50.f, binocular_distance=0.09f;
// Decided once per update (VrBinocularsUpdate), so drawing in both eyes and
// the HUD agree.
bool g_binoculars_raised=false;
// The eyepieces (the middle of the rear face of the model in the hand: the
// first-person model looks along its +Z) within 11 cm of the eyes, the
// binoculars looking where the head does; let go of beyond 16 cm or 45
// degrees, so the view does not flicker at the edge.
bool BinocularsRaised(CActor& actor, bool raised)
{
    auto* binoculars=smart_cast<CWeaponBinoculars*>(BeltItemInHand(actor));
    Fmatrix head, pose;
    if (!binoculars || !vr::HeadTransform(head) || !VrKnifePose(actor,pose)) return false;
    IRenderVisual* visual=binoculars->HudItemData()->m_model->dcast_RenderVisual();
    if (!visual) return false;
    const Fbox& box=visual->getVisData().box;
    Fvector eyepiece; box.getcenter(eyepiece);
    eyepiece.z=box.min.z;
    pose.transform_tiny(eyepiece);
    Fvector forward=pose.k; forward.normalize_safe();
    const float reach=raised ? 0.16f : 0.11f, aligned=raised ? 0.70f : 0.85f;
    return eyepiece.distance_to(head.c)<reach && forward.dotproduct(head.k)>aligned;
}
bool BinocularsAtEyes(CActor& actor, CWeaponBinoculars*& binoculars, Fmatrix& head)
{
    binoculars=smart_cast<CWeaponBinoculars*>(BeltItemInHand(actor));
    return g_binoculars_raised && binoculars && vr::HeadTransform(head);
}
}

bool VrBinocularsAtEyes(CActor& actor)
{
    CWeaponBinoculars* binoculars; Fmatrix head;
    return g_vr_owns_present && BinocularsAtEyes(actor,binoculars,head);
}

void VrBinocularsUpdate(CActor& actor)
{
    const bool was=g_binoculars_raised;
    g_binoculars_raised=g_vr_owns_present && vr::GameplayActive() && BinocularsRaised(actor,was);
    if (g_binoculars_raised!=was)
        if (auto* binoculars=smart_cast<CWeaponBinoculars*>(actor.inventory().ActiveItem()))
            vr::HandHaptic(binoculars->VrWeaponHand(),0.06f);
    CWeaponBinoculars* binoculars; Fmatrix head;
    if (!g_vr_owns_present || !BinocularsAtEyes(actor,binoculars,head)) return;
    // The zoom factor is the zoomed field of view (x0.75 in the camera), as
    // for scopes; vr_scope_zoom scales it the same way.
    const float zoom=binoculars->CurrentZoomFactor()*0.75f;
    if (zoom<=0.f) return;
    const float magnification=std::max(g_fov/zoom*vr::ScopeZoomScale(),1.f);
    Frect part;
    const shared_str reticle=binoculars->VrScopeReticle(&part);
    const float rect[4]={part.x1,part.y1,part.x2,part.y2};
    vr::SetScopeView(head,binocular_apparent/magnification,binocular_apparent,
        reticle.size() ? reticle.c_str() : nullptr,part.width()>0.f ? rect : nullptr);
}

void RenderVrBinoculars(CActor& actor)
{
    CWeaponBinoculars* binoculars; Fmatrix head;
    if (!g_vr_owns_present || !BinocularsAtEyes(actor,binoculars,head)) return;
    const float radius=binocular_distance*tanf(deg2rad(binocular_apparent*0.5f));
    Fvector centre; centre.mad(head.c,head.k,binocular_distance);
    vr::DrawScopeLens(centre,Fvector().mul(head.i,radius),Fvector().mul(head.j,radius),1.f,0.f,0.f);
}

void VrHandItemsUpdate(CActor& actor, const bool* tracked, const bool* grip)
{
    auto& pickup=actor.m_vr_pickup;
    PIItem active=actor.inventory().ActiveItem();
    // Knife or binoculars taken from the belt without a VR profile. Drawing
    // is deferred: until it is the active item it is still on its way, and
    // is held all the same. Dropping the hold then (the active item was not
    // it yet in the frame it was taken) left it out with nothing to put it
    // back: the slot became a toggle.
    if (pickup.slot_hold_hand>=0)
    {
        const int h=pickup.slot_hold_hand;
        const bool held=active && active->object().ID()==pickup.slot_hold_id;
        PIItem next=actor.inventory().ItemFromSlot(actor.inventory().GetNextActiveSlot());
        const bool coming=!held && next && next->object().ID()==pickup.slot_hold_id;
        if (!held && !coming) pickup.slot_hold_hand=-1;
        else if (tracked[h] && !grip[h])
        {
            actor.inventory().Activate(NO_ACTIVE_SLOT,true);
            vr::HandHaptic(h,0.15f);
            pickup.slot_hold_hand=-1;
        }
    }
    // A knife or binoculars without a VR profile: their unseen first-person
    // show and hide are not waited on either.
    for (u16 slot:{KNIFE_SLOT,BINOCULAR_SLOT})
        if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot)))
        {
            const u32 state=weapon->GetState();
            if (g_vr_owns_present && !weapon->VrSupported() && (state==CHUDState::eShowing ||
                state==CHUDState::eHiding) && weapon->GetNextState()==state)
            {
                weapon->StopCurrentAnimWithoutCallback();
                static_cast<CHudItem*>(weapon)->OnAnimationEnd(state);
            }
        }
    // The detector or flashlight: its show and hide are not waited on, it is
    // put away when the hand that took it opens, and a flashlight's trigger
    // switches its light.
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    if (device && g_vr_owns_present) device->VrSkipAnimation();
    // Just taken, its show is still on its way (the state switch arrives as
    // an event): still hidden, but in the hand. Clearing the hand then made
    // the grip a toggle, the trigger dead and the hand's pointer visible.
    const bool coming=device && device->IsHidden() &&
        (device->GetNextState()!=CHUDState::eHidden || Device.dwTimeGlobal-pickup.device_taken_time<400);
    if (!device || (device->IsHidden() && !coming)) pickup.device_hand=-1;
    else if (const int h=pickup.device_hand; h>=0 && tracked[h])
    {
        if (!grip[h])
        {
            PressAction(actor,kDETECTOR);
            vr::HandHaptic(h,0.12f);
            pickup.device_hand=-1;
        }
        else
        {
            const bool trigger=vr::HandTrigger(h)>0.65f;
            if (trigger && !pickup.device_trigger_down)
                if (auto* light=smart_cast<CFlashlight*>(device)) { light->VrToggleLight(); vr::HandHaptic(h,0.08f); }
            pickup.device_trigger_down=trigger;
            // The upper face button (Y/B) turns a flashlight over in the
            // fist: held overhand, the hand raised, or underhand. A
            // detector is read from its face, so it is not turned.
            const bool button=vr::HandSecondaryButton(h);
            if (button && !pickup.device_button_down && smart_cast<CFlashlight*>(device))
            {
                pickup.device_flipped=!pickup.device_flipped;
                vr::HandHaptic(h,0.08f);
            }
            pickup.device_button_down=button;
        }
    }
    // The knife hits what its blade sweeps through when swung faster than
    // 2.5 m/s at the tip, at most every 0.35 s, harder the faster the swing.
    auto* knife=smart_cast<CWeaponKnife*>(active);
    const int knife_hand=knife ? knife->VrWeaponHand() : -1;
    Fmatrix pose;
    attachable_hud_item* knife_hud=knife ? knife->HudItemData() : nullptr;
    IRenderVisual* visual=knife_hud && knife_hud->m_model ? knife_hud->m_model->dcast_RenderVisual() : nullptr;
    if (!knife || knife->VrSupported() || !visual || !tracked[knife_hand] || !VrKnifePose(actor,pose))
    { pickup.knife_valid=false; return; }
    // The blade: the far end of the model's longest axis from the hand.
    const Fbox& box=visual->getVisData().box;
    Fvector size, centre; box.getsize(size); box.getcenter(centre);
    const int axis=size.x>=size.y && size.x>=size.z ? 0 : (size.y>=size.z ? 1 : 2);
    Fvector ends[2]={centre,centre};
    ends[0][axis]=box.min[axis]; ends[1][axis]=box.max[axis];
    pose.transform_tiny(ends[0]); pose.transform_tiny(ends[1]);
    const Fvector tip=ends[0].distance_to_sqr(pose.c)>ends[1].distance_to_sqr(pose.c) ? ends[0] : ends[1];
    // The swing is measured in the body's frame: a snap turn or walking moves
    // the blade through the world without the player's hand moving.
    Fmatrix body;
    if (!vr::BodyTransform(body)) { pickup.knife_valid=false; return; }
    Fmatrix to_body; to_body.invert(body);
    Fvector tip_local; to_body.transform_tiny(tip_local,tip);
    if (pickup.knife_valid && Device.fTimeDelta>0.001f && int(Device.dwTimeGlobal-pickup.knife_ready)>=0)
    {
        // Last frame's tip where the body is now.
        Fvector previous; body.transform_tiny(previous,pickup.knife_tip);
        Fvector motion; motion.sub(tip,previous);
        const float travel=motion.magnitude(), speed=travel/Device.fTimeDelta;
        collide::rq_result hit;
        if (speed>2.5f && travel>EPS_L)
        {
            motion.div(travel);
            // Swept from last frame's tip, a little ahead of where it is.
            if (Level().ObjectSpace.RayPick(previous,motion,travel+0.05f,collide::rqtBoth,hit,&actor))
            {
                Fvector from; from.mad(previous,motion,-0.05f);
                knife->VrStrike(from,motion,clampr(speed/5.f,0.6f,1.5f),hit.range+0.1f);
                vr::HandHaptic(knife_hand,0.35f);
                pickup.knife_ready=Device.dwTimeGlobal+350;
                Msg("* [VR body] knife hit at %.1f m/s",speed);
            }
        }
    }
    pickup.knife_tip=tip_local;
    pickup.knife_valid=true;
}

namespace
{
// The PDA carried flat on the chest, leaning back 20 degrees so its face
// tilts up toward the eyes. Which model axis is its face is not known, so
// the thinnest one faces out and the longest runs up.
void RenderVrChestPda(CActor& actor, const Fmatrix& body)
{
    PIItem pda=actor.inventory().ItemFromSlot(PDA_SLOT);
    if (!pda || actor.inventory().ActiveItem()==pda) return;
    IRenderVisual* visual=pda->object().Visual();
    Fvector point;
    if (!visual || !VrBodySlotPoint(actor,vr::BodyPda,point)) return;
    const float lean=deg2rad(20.f);
    Fvector out, up;
    out.set(body.k).mul(_cos(lean)).mad(body.j,_sin(lean));
    up.set(body.j).mul(_cos(lean)).mad(body.k,-_sin(lean));
    const Fbox& box=visual->getVisData().box;
    Fvector size; box.getsize(size);
    const float extent[3]={size.x,size.y,size.z};
    int thin=0, longest=0;
    for (int axis=1; axis<3; ++axis)
    {
        if (extent[axis]<extent[thin]) thin=axis;
        if (extent[axis]>extent[longest]) longest=axis;
    }
    if (thin==longest) longest=(thin+1)%3;
    Fvector axes[3];
    axes[thin]=out; axes[longest]=up; axes[3-thin-longest]=body.i;
    // Keep the frame's handedness (i = j x k).
    Fvector check; check.crossproduct(axes[1],axes[2]);
    if (check.dotproduct(axes[0])<0.f) axes[3-thin-longest].invert();
    Fmatrix pose; pose.identity();
    pose.i=axes[0]; pose.j=axes[1]; pose.k=axes[2];
    // Its back against the chest.
    Fvector centre; box.getcenter(centre);
    pose.transform_dir(centre);
    pose.c.sub(point,centre).mad(out,0.5f*extent[thin]);
    if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(visual);
}
}

namespace
{
// Bolts ride the belt in a pouch, shown while there are any, whether or not
// one is in a hand. The model is [vr_bolt_pouch] visual in the system
// config, scaled so its longest side is `size` metres.
bool RenderVrBoltPouch(CActor& actor, const Fmatrix& body)
{
    auto& pickup=actor.m_vr_pickup;
    if (!pickup.bolt_pouch && !pickup.bolt_pouch_missing)
    {
        LPCSTR name=READ_IF_EXISTS(pSettings,r_string,"vr_bolt_pouch","visual","dynamics\\equipments\\sumka2");
        string_path file, found;
        xr_strcpy(file,name);
        if (!strext(file)) xr_strcat(file,".ogf");
        if (FS.exist(found,"$game_meshes$",file)) pickup.bolt_pouch=::Render->model_Create(name);
        if (!pickup.bolt_pouch) Msg("! [VR body] bolt pouch model %s not found; drawing the bolt",file);
        pickup.bolt_pouch_missing=!pickup.bolt_pouch;
    }
    IRenderVisual* visual=pickup.bolt_pouch;
    Fvector point;
    if (!visual || !VrBodySlotPoint(actor,vr::BodyBolt,point)) return false;
    const Fbox& box=visual->getVisData().box;
    Fvector extent; box.getsize(extent);
    const float longest=_max(extent.x,_max(extent.y,extent.z));
    if (longest<EPS_L) return false;
    const float scale=READ_IF_EXISTS(pSettings,r_float,"vr_bolt_pouch","size",0.14f)/longest;
    // Upright, facing out from the body, centred on the slot.
    Fmatrix pose; pose.identity();
    pose.i.set(body.i).mul(scale); pose.j.set(body.j).mul(scale); pose.k.set(body.k).mul(scale);
    Fvector centre; box.getcenter(centre); pose.transform_dir(centre);
    pose.c.sub(point,centre);
    if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(visual);
    return true;
}
}

bool VrBodySlotHolster(CActor& actor, u16 inventory_slot, Fvector& point, float& range)
{
    const int kind=inventory_slot==KNIFE_SLOT ? vr::BodyKnife :
        inventory_slot==BINOCULAR_SLOT ? vr::BodyBinoculars : -1;
    if (kind<0 || !VrBodySlotPoint(actor,kind,point)) return false;
    range=1.5f*vr::BodySlotRadius();
    return true;
}

void RenderVrBodySlots(CActor& actor)
{
    if (!g_vr_owns_present || !vr::GameplayActive() ||
        &actor!=Level().CurrentControlEntity() || actor.cam_Active()!=actor.cam_FirstEye()) return;
    RenderVrDetector(actor);
    if (!vr::BodySlots()) return;
    // Carried items hang from the belt, model forward pointing down (a knife
    // blade-down), facing out from the body, centred on their slot.
    const Fmatrix body=VrTorsoFrame(actor);
    for (int kind=vr::BodyKnife; kind<=vr::BodyDetector; ++kind)
    {
        PIItem item=actor.inventory().ItemFromSlot(InventorySlot(kind));
        if (kind==vr::BodyBolt && item && RenderVrBoltPouch(actor,body)) continue;
        if (!item) continue;
        // A detector switched on is in the hand, not the belt. Anything else
        // not active is on the belt, whatever its state: a knife whose unseen
        // hide animation has not ended is no longer drawn in the hand. One
        // still active but on its way out (let go: hiding, or another slot
        // next) is back on the belt already.
        auto* hud=smart_cast<CHudItem*>(item);
        if (actor.inventory().ActiveItem()==item)
        {
            const bool leaving=actor.inventory().GetNextActiveSlot()!=item->CurrSlot() ||
                (hud && (hud->GetState()==CHUDState::eHiding || hud->IsHidden()));
            // An active grenade waiting on the belt (after a throw, or let
            // go) is drawn there: it is in no hand.
            const bool stowed=smart_cast<CMissile*>(item) && actor.m_vr_pickup.missile_stowed;
            if (!leaving && !stowed) continue;
        }
        if (kind==vr::BodyDetector && hud && !hud->IsHidden()) continue;
        IRenderVisual* visual=item->object().Visual();
        Fvector point;
        if (!visual || !VrBodySlotPoint(actor,kind,point)) continue;
        if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
        Fmatrix pose; pose.identity();
        pose.i=body.i; pose.j=body.k; pose.k.set(0,-1,0);
        Fvector centre; pose.transform_dir(centre,visual->getVisData().sphere.P);
        pose.c.sub(point,centre);
        ::Render->set_Transform(&pose);
        ::Render->add_Visual(visual);
    }
    RenderVrChestPda(actor,body);
    const auto draw=[](PIItem item, Fmatrix pose, const Fvector& point)
    {
        IRenderVisual* visual=item ? item->object().Visual() : nullptr;
        if (!visual) return;
        if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
        Fvector centre; pose.transform_dir(centre,visual->getVisData().sphere.P);
        pose.c.sub(point,centre);
        ::Render->set_Transform(&pose);
        ::Render->add_Visual(visual);
    };
    const auto& pickup=actor.m_vr_pickup;
    for (int h=0; h<2; ++h)
    {
        // In the fist, its up along the grip so a bottle stands in the hand.
        Fmatrix hand;
        if (pickup.bolt_hand==h)
        {
            if (VrHeldItemPose(h,hand)) draw(actor.inventory().ItemFromSlot(BOLT_SLOT),hand,hand.c);
            continue;
        }
        if (pickup.pocket_held[h]>=0)
        {
            if (!vr::HandTransform(h,false,hand)) continue;
            Fmatrix pose; pose.identity();
            pose.i=hand.i; pose.j=hand.k; pose.k.invert(hand.j);
            draw(PocketItem(actor,pickup.pocket_held[h]),pose,hand.c);
            continue;
        }
        // Upright in the pocket a free hand reaches into, to show what it holds.
        const int kind=pickup.body_hover[h];
        Fvector point;
        if (!vr::IsPocket(kind) || !VrBodySlotPoint(actor,kind,point)) continue;
        Fmatrix pose; pose.identity();
        pose.i=body.i; pose.j=body.j; pose.k=body.k;
        draw(PocketItem(actor,kind-vr::BodyPocket1),pose,point);
    }
}
