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
#include <dinput.h>
#include "Missile.h"
#include "Grenade.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "UIGameCustom.h"
#include "ui/UIPdaWnd.h"
#include "VrWeaponInteraction.h"
#include "VrWeaponModel.h"
#include "VrWeaponPose.h"
#include "WeaponKnife.h"
#include "WeaponBinoculars.h"
// The active item is being put away (VrPickup.cpp).
bool VrActiveItemLeaving(CActor& actor);
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
// The hand is free: not holding the active weapon, whatever implements it
// (an unprofiled weapon or a launcher in its native mode is in a hand too),
// or busy with its magazine, slide or foregrip. A weapon on its way out (let
// go at the holster) holds no hand.
bool HandFree(CActor& actor, int hand)
{
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    if (!weapon || VrActiveItemLeaving(actor)) return true;
    return hand!=weapon->VrWeaponHand() && !(weapon->VrMode() && weapon->VrLeftHandBusy());
}
// The hand holds something other than a device: an item it grabbed, a
// magazine from the floor, a pocket item, the bolt, a knife or binoculars,
// a grenade (VrHandHoldsItem adds a device in that hand).
bool HandHoldsOther(CActor& actor, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    return pickup.grabbed_id[hand]!=u16(-1) || VrFloorMagazineHand(hand) ||
        pickup.pocket_held[hand]>=0 || pickup.bolt_hand==hand ||
        pickup.slot_hold_hand==hand || hand==VrMissileHand(actor);
}
bool Available(CActor& actor, int kind, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    // A hand holding anything takes nothing more; the hand that took the
    // detector may still reach for its slot (to put it back).
    if (!HandFree(actor,hand) || HandHoldsOther(actor,hand) ||
        (VrDetectorHand(actor)==hand && kind!=vr::BodyDetector)) return false;
    // A bolt goes into a free hand whatever the other holds; one at a time.
    if (kind==vr::BodyBolt)
    {
        PIItem bolt=actor.inventory().ItemFromSlot(BOLT_SLOT);
        if (!bolt || pickup.bolt_hand>=0 || actor.inventory_disabled() || hand==VrMissileHand(actor)) return false;
        // The active bolt (drawn by a key) is taken only while it waits on
        // the belt (VrBodySlotTake): throwing it from the pouch would reject
        // the weapon in use.
        return bolt!=actor.inventory().ActiveItem() || pickup.missile_stowed;
    }
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
// A hand let go of a thrown item at its peak velocity: faster than 1.5 m/s
// it flies with that velocity times vr_throw_scale, no faster than 1.25
// times the item's own throw (when it has one); slower it is not thrown.
bool ThrowVelocity(const Fvector& peak, CMissile* missile, Fvector& velocity, float& speed)
{
    velocity=peak;
    speed=velocity.magnitude();
    if (speed<=1.5f) return false;
    velocity.mul(vr::ThrowScale());
    const float limit=missile ? 1.25f*missile->GetMaxForce() : 0.f;
    if (limit>0.f && velocity.magnitude()>limit) velocity.normalize_safe().mul(limit);
    return true;
}
// A model drawn this frame at pose, its bones posed first.
void DrawModel(IRenderVisual* visual, Fmatrix pose)
{
    if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(visual);
}
// The same, oriented as pose with its bounding sphere's centre (as posed)
// at point.
void DrawModelAt(IRenderVisual* visual, Fmatrix pose, const Fvector& point)
{
    if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
    Fvector centre; pose.transform_dir(centre,visual->getVisData().sphere.P);
    pose.c.sub(point,centre);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(visual);
}
// The same, scaled by `scale`, resting against the body: the side toward
// it at `rest`, its bulk out along `out` (unit), its box centred there.
void DrawModelResting(IRenderVisual* visual, Fmatrix pose, float scale, const Fvector& rest, const Fvector& out)
{
    if (IKinematics* skeleton=visual->dcast_PKinematics()) skeleton->CalculateBones(TRUE);
    pose.i.mul(scale); pose.j.mul(scale); pose.k.mul(scale);
    const Fbox& box=visual->getVisData().box;
    Fvector size; box.getsize(size);
    const float depth=0.5f*(_abs(pose.i.dotproduct(out))*size.x+_abs(pose.j.dotproduct(out))*size.y+
        _abs(pose.k.dotproduct(out))*size.z);
    Fvector centre; box.getcenter(centre); pose.transform_dir(centre);
    Fvector at; at.mad(rest,out,depth);
    pose.c.sub(at,centre);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(visual);
}
// The scale that fits a model's longest side within `size` metres (never
// enlarged).
float FitScale(IRenderVisual* visual, float size)
{
    Fvector extent; visual->getVisData().box.getsize(extent);
    const float longest=_max(extent.x,_max(extent.y,extent.z));
    return longest>size && longest>EPS_L ? size/longest : 1.f;
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

float VrBodyHipShift()
{
    return vr::default_hip_height-vr::HipHeight();
}

Fvector VrOnBodyPlace(CActor& actor, const Fmatrix& pelvis, float angle, float height)
{
    const float x=_sin(angle), z=_cos(angle);
    const float surface=actor.m_legs_controller.vr_body_surface(height,x,z);
    const float reach=(surface>0.f ? surface : 0.15f)+vr::body_slot_margin;
    Fvector point=pelvis.c;
    point.mad(pelvis.i,x*reach).mad(pelvis.j,height+VrBodyHipShift()).mad(pelvis.k,z*reach);
    return point;
}

// With the body shown: where a place's item rests on it. The point on the
// body's axis at the place's height, the direction out from the body there
// (level) and that place; the item drawn there (DrawModelResting) is
// pushed out until its own footprint clears the outfit (the reach point,
// VrOnBodyPlace, is further out). False without the body or for a place
// not on it.
struct VrBodyRest
{
    Fvector axis, out;
    float x, z, height;
};
static bool VrOnBodyRest(CActor& actor, int kind, VrBodyRest& rest)
{
    Fmatrix pelvis;
    vr::BodyPlace place;
    if (!vr::BodyVisible() || !actor.m_legs_controller.vr_body_anchor(pelvis) ||
        !vr::BodySlotOnBody(kind,vr::LeftHanded(),place)) return false;
    rest.x=_sin(place.angle); rest.z=_cos(place.angle); rest.height=place.height;
    rest.out.set(0,0,0).mad(pelvis.i,rest.x).mad(pelvis.k,rest.z);
    rest.out.y=0.f;
    if (rest.out.square_magnitude()<EPS_S) return false;
    rest.out.normalize();
    rest.axis=pelvis.c;
    rest.axis.mad(pelvis.j,place.height+VrBodyHipShift());
    return true;
}
// The model scaled by `scale`, resting against the body at `rest`: its box
// centred on the place's height and direction, its side toward the body
// 1 cm clear of the outfit within the box's own width and height (a flat
// PDA lies on the chest, not out at the width of the flanks beside it).
static void DrawModelResting(CActor& actor, IRenderVisual* visual, Fmatrix pose, float scale, const VrBodyRest& rest)
{
    const Fvector& out=rest.out;
    Fvector up; up.set(0.f,1.f,0.f);
    Fvector across; across.crossproduct(up,out);
    pose.i.mul(scale); pose.j.mul(scale); pose.k.mul(scale);
    Fvector size; visual->getVisData().box.getsize(size);
    const auto half=[&](const Fvector& d)
    {
        return 0.5f*(_abs(pose.i.dotproduct(d))*size.x+_abs(pose.j.dotproduct(d))*size.y+
            _abs(pose.k.dotproduct(d))*size.z);
    };
    const float tall=half(up), wide=half(across);
    const float contact=actor.m_legs_controller.vr_body_contact(rest.height-tall,rest.height+tall,
        rest.x,rest.z,wide);
    Fvector at; at.mad(rest.axis,out,(contact>0.f ? contact : 0.15f)+0.01f);
    DrawModelResting(visual,pose,1.f,at,out);
}
// A level frame facing `out`: i across, j up, k out.
static Fmatrix OutwardFrame(const Fvector& out)
{
    Fmatrix frame; frame.identity();
    frame.k=out; frame.j.set(0,1,0);
    frame.i.crossproduct(frame.j,frame.k).normalize_safe();
    return frame;
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
        const vr::V3 local=vr::BodySlotLocal(kind,vr::HipHeight(),vr::HipWidth(),vr::LeftHanded());
        Fvector at=VrOnBodySurface(actor,local.x,local.y+vr::HipHeight(),local.z);
        point=pelvis.c;
        point.mad(pelvis.i,at.x).mad(pelvis.j,at.y+VrBodyHipShift()).mad(pelvis.k,at.z);
        return true;
    }
    Fvector head;
    if (!vr::HeadPosition(head)) { head=actor.Position(); head.y+=1.67f; }
    const Fvector forward=VrTorsoFrame(actor).k;
    vr::V3 local=vr::BodySlotLocal(kind,vr::HipHeight(),vr::HipWidth(),vr::LeftHanded());
    // The places on the chest, over the shoulder and in the pockets spread
    // with the player's shoulders (measured by body tracking, or
    // vr_shoulder_width); the belt's follow vr_hip_width.
    if (vr::IsPocket(kind) || kind==vr::BodyBinoculars || kind==vr::BodyPda || kind==vr::BodyInventory)
        local.x*=vr::ShoulderWidthScale();
    const vr::V3 world=vr::BodyToWorld({head.x,head.y,head.z},forward.x,forward.z,local);
    point.set(world.x,world.y,world.z);
    return true;
}

int VrBodySlotCandidate(CActor& actor, int hand, const Fvector& position, float& distance)
{
    distance=FLT_MAX;
    if (!vr::BodySlots()) return -1;
    vr::V3 points[vr::BodySlotCount];
    bool available[vr::BodySlotCount];
    for (int kind=0; kind<vr::BodySlotCount; ++kind)
    {
        Fvector point;
        available[kind]=Available(actor,kind,hand) && VrBodySlotPoint(actor,kind,point);
        points[kind]=available[kind] ? vr::V3{point.x,point.y,point.z} : vr::V3{0,0,0};
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
        pickup.pocket_trigger_down[hand]=vr::TriggerPulled(vr::HandTrigger(hand));
        return false;
    }
    if (kind==vr::BodyBolt)
    {
        auto& pickup=actor.m_vr_pickup;
        // The active bolt, drawn by a key and waiting on the belt: into
        // this hand as the active missile (VrMissileUpdate), as a grenade
        // already out is.
        PIItem bolt=actor.inventory().ItemFromSlot(BOLT_SLOT);
        if (bolt && bolt==actor.inventory().ActiveItem() && pickup.missile_stowed)
        {
            pickup.missile_hand=hand;
            pickup.missile_stowed=false;
            return false;
        }
        // Held in this hand; the active item (a weapon in the other hand)
        // stays as it is.
        pickup.bolt_hand=hand;
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
        actor.m_vr_pickup.device_trigger_down=vr::TriggerPulled(vr::HandTrigger(hand));
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
    head.set(0.f,0.f,0.f);
    const bool head_valid=vr::HeadPosition(head);
    Fmatrix head_world;
    looking=vr::HeadTransform(head_world) ? head_world.k : actor.XFORM().k;
    const vr::V3 mouth=vr::MouthPoint({head.x,head.y,head.z},looking.x,looking.z);
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
        const bool trigger=vr::TriggerPulled(vr::HandTrigger(h));
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

namespace
{
// A grenade's own first-person model in the hand, its pin a part of its
// own: two copies posed at their bind place (vrwi::ApplyWeaponPose), one
// showing all but the pin, the other only the pin ([vr_derivation]
// grenade_pin_bones: a stock grenade's "eka", its ring "jopa" under it).
struct GrenadeModels
{
    shared_str visual;
    IRenderVisual* body=nullptr;
    IRenderVisual* pin=nullptr;
    xr_vector<Fmatrix> body_poses, pin_poses;
    vr::WeaponPoseCache<xr_vector<Fmatrix>> body_cache, pin_cache;
    shared_str pin_bone;
    // The ring's middle, where a hand takes it (the model's space).
    Fvector ring{0.f,0.06f,0.f};
    bool failed=false;
};
GrenadeModels g_grenade;

void ReleaseGrenadeModels()
{
    if (g_grenade.body) ::Render->model_Delete(g_grenade.body);
    if (g_grenade.pin) ::Render->model_Delete(g_grenade.pin);
    g_grenade.body=g_grenade.pin=nullptr;
    g_grenade.visual=nullptr;
    g_grenade.pin_bone=nullptr;
    g_grenade.body_poses.clear(); g_grenade.pin_poses.clear();
    g_grenade.body_cache=vr::WeaponPoseCache<xr_vector<Fmatrix>>{};
    g_grenade.pin_cache=vr::WeaponPoseCache<xr_vector<Fmatrix>>{};
    g_grenade.ring.set(0.f,0.06f,0.f);
    g_grenade.failed=false;
}

// The copies of this grenade's first-person model, made once per model.
GrenadeModels* GrenadeCopies(CMissile& missile)
{
    const shared_str& section=missile.HudSection();
    if (!section.size() || !pSettings->line_exist(section,"item_visual")) return nullptr;
    const shared_str visual=pSettings->r_string(section,"item_visual");
    if (g_grenade.visual!=visual)
    {
        ReleaseGrenadeModels();
        g_grenade.visual=visual;
        g_grenade.body=::Render->model_Create(visual.c_str());
        g_grenade.pin=::Render->model_Create(visual.c_str());
        IKinematics* model=g_grenade.body ? g_grenade.body->dcast_PKinematics() : nullptr;
        g_grenade.failed=!model || !g_grenade.pin || !g_grenade.pin->dcast_PKinematics();
        if (!g_grenade.failed)
        {
            const auto& names=vrwi::VrNames().grenade_pin_bones;
            for (u16 id=0;id<model->LL_BoneCount() && !g_grenade.pin_bone.size();++id)
                if (vrwi::NameHasAny(model->LL_BoneName_dbg(id),names)) g_grenade.pin_bone=model->LL_BoneName_dbg(id);
            if (g_grenade.pin_bone.size())
            {
                xr_vector<Fvector> points;
                vrwi::PointCollector collector(points);
                vrwi::EnumSubtreeVertices(*model,model->LL_BoneID(g_grenade.pin_bone.c_str()),collector);
                Fbox box; box.invalidate();
                for (const Fvector& p:points) box.modify(p);
                if (!points.empty()) box.getcenter(g_grenade.ring);
                Msg("* [VR body] %s: pin %s, taken at (%.3f %.3f %.3f)",visual.c_str(),g_grenade.pin_bone.c_str(),
                    g_grenade.ring.x,g_grenade.ring.y,g_grenade.ring.z);
            }
            else Msg("! [VR body] %s: no pin found ([vr_derivation] grenade_pin_bones); it is pulled unseen",
                visual.c_str());
        }
    }
    return g_grenade.failed ? nullptr : &g_grenade;
}

// The grenade's pin: taken by the other hand's trigger on the ring (or by
// the teeth: the grenade held at the mouth as long as eating takes), and
// carried in that hand while its trigger is held. Clear of its hole it is
// out and the grenade is readied (its own checkout sound); brought back
// into the hole it is in again. Let go out of the hole, it is gone, as is
// one bitten out. Opening the grenade's hand then lets it go live.
enum PinPlace { PinSeated, PinHand, PinTeeth, PinGone };
struct GrenadePin
{
    u16 missile=u16(-1);
    PinPlace where=PinSeated;
    int hand=-1;
    // The pin copy's model transform in the hand's frame, as taken.
    Fmatrix in_hand=Fidentity;
    bool out=false;
    bool trigger_down[2]{};
    u32 mouth_since=0;
    float teeth_from=0.f;
};
GrenadePin g_pin;
constexpr float pin_reach=0.06f, pin_clear=0.03f, pin_home=0.015f, pin_bite_pull=0.06f;

bool StepGrenadePin(CActor& actor, CMissile& missile, int h, const bool* tracked, const Fmatrix* hands)
{
    Fmatrix pose;
    if (!VrMissilePose(actor,pose)) return g_pin.out;
    const GrenadeModels* models=GrenadeCopies(missile);
    const Fvector ring_local=models ? models->ring : Fvector().set(0.f,0.06f,0.f);
    Fvector seat; pose.transform_tiny(seat,ring_local);
    const int o=1-h;
    const bool trigger=tracked[o] && vr::TriggerPulled(vr::HandTrigger(o));
    const bool pressed=trigger && !g_pin.trigger_down[o];
    g_pin.trigger_down[o]=trigger;
    LPCSTR name=missile.cNameSect().c_str();
    switch (g_pin.where)
    {
    case PinSeated:
    {
        if (pressed && HandFree(actor,o) && !VrHandHoldsItem(actor,o) && hands[o].c.distance_to(seat)<=pin_reach)
        {
            g_pin.where=PinHand;
            g_pin.hand=o;
            g_pin.in_hand.mul_43(Fmatrix().invert(hands[o]),pose);
            vr::HandHaptic(o,0.08f);
            break;
        }
        // The teeth on the ring: the grenade held at the mouth.
        Fvector head; Fmatrix head_world;
        if (!vr::HeadPosition(head) || !vr::HeadTransform(head_world)) break;
        const vr::V3 mouth=vr::MouthPoint({head.x,head.y,head.z},head_world.k.x,head_world.k.z);
        const float at_mouth=vr::BodyDistance(mouth,{seat.x,seat.y,seat.z});
        if (at_mouth>vr::mouth_reach) { g_pin.mouth_since=0; break; }
        if (!g_pin.mouth_since) g_pin.mouth_since=Device.dwTimeGlobal|1;
        else if (Device.dwTimeGlobal-g_pin.mouth_since>=vr::mouth_dwell_ms)
        {
            g_pin.where=PinTeeth;
            g_pin.teeth_from=at_mouth;
            vr::HandHaptic(h,0.08f);
        }
        break;
    }
    case PinHand:
    {
        if (!tracked[o]) break;
        Fmatrix pin_world; pin_world.mul_43(hands[o],g_pin.in_hand);
        Fvector ring; pin_world.transform_tiny(ring,ring_local);
        const float off=ring.distance_to(seat);
        if (!g_pin.out && off>=pin_clear)
        {
            g_pin.out=true;
            for (int side=0;side<2;++side) vr::HandHaptic(side,0.25f);
            Msg("* [VR body] %s: pin pulled by the %s hand",name,o ? "right" : "left");
        }
        else if (g_pin.out && off<=pin_home)
        {
            g_pin.out=false;
            for (int side=0;side<2;++side) vr::HandHaptic(side,0.2f);
            Msg("* [VR body] %s: pin pushed back in",name);
        }
        if (!trigger)
        {
            g_pin.where=g_pin.out ? PinGone : PinSeated;
            if (g_pin.out) Msg("* [VR body] %s: pin let go",name);
        }
        break;
    }
    case PinTeeth:
    {
        Fvector head; Fmatrix head_world;
        if (!vr::HeadPosition(head) || !vr::HeadTransform(head_world)) break;
        const vr::V3 mouth=vr::MouthPoint({head.x,head.y,head.z},head_world.k.x,head_world.k.z);
        if (vr::BodyDistance(mouth,{seat.x,seat.y,seat.z})-g_pin.teeth_from>=pin_bite_pull)
        {
            g_pin.where=PinGone;
            g_pin.out=true;
            for (int side=0;side<2;++side) vr::HandHaptic(side,0.25f);
            Msg("* [VR body] %s: pin pulled with the teeth",name);
        }
        break;
    }
    case PinGone:
        break;
    }
    return g_pin.out;
}

// Detectors and flashlights on the belt: their first-person model, as in
// the hand (the world model of a flashlight is another one), posed at its
// bind place. Made once per model.
struct BeltCopy
{
    IRenderVisual* visual=nullptr;
    xr_vector<Fmatrix> poses;
    vr::WeaponPoseCache<xr_vector<Fmatrix>> cache;
    bool tried=false;
};
xr_map<shared_str,BeltCopy> g_belt_copies;

IRenderVisual* BeltVisual(PIItem item)
{
    auto* hud_item=smart_cast<CHudItem*>(item);
    const shared_str section=hud_item ? hud_item->HudSection() : shared_str();
    if (!section.size() || !pSettings->section_exist(section) || !pSettings->line_exist(section,"item_visual"))
        return item->object().Visual();
    BeltCopy& copy=g_belt_copies[section];
    if (!copy.tried)
    {
        copy.tried=true;
        copy.visual=::Render->model_Create(pSettings->r_string(section,"item_visual"));
    }
    IKinematics* model=copy.visual ? copy.visual->dcast_PKinematics() : nullptr;
    if (!model) return item->object().Visual();
    model->Callback(vrwi::CollapseHiddenBones,nullptr);
    vr::ApplyWeaponPose(*model,copy.poses,vrwi::BonePose,bctCustom,nullptr,0.f,nullptr,{},false,true,true,
        nullptr,{},{},nullptr,0.f,{},true,&copy.cache,Device.dwFrame);
    return copy.visual;
}

void ReleaseBeltCopies()
{
    for (auto& entry:g_belt_copies)
        if (entry.second.visual) ::Render->model_Delete(entry.second.visual);
    g_belt_copies.clear();
}
}

bool VrRenderGrenade(CActor& actor)
{
    auto* missile=smart_cast<CMissile*>(actor.inventory().ActiveItem());
    Fmatrix pose;
    if (!missile || !smart_cast<CGrenade*>(missile) || !VrMissilePose(actor,pose)) return false;
    GrenadeModels* models=GrenadeCopies(*missile);
    if (!models) return false;
    const bool ours=g_pin.missile==missile->ID();
    const bool seated=!ours || g_pin.where==PinSeated || g_pin.where==PinTeeth;
    IKinematics& body=*models->body->dcast_PKinematics();
    body.Callback(vrwi::CollapseHiddenBones,nullptr);
    vr::ApplyWeaponPose(body,models->body_poses,vrwi::BonePose,bctCustom,nullptr,0.f,nullptr,{},false,true,true,
        seated ? nullptr : models->pin_bone.c_str(),{},{},nullptr,0.f,{},true,&models->body_cache,Device.dwFrame);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(models->body);
    Fmatrix hand;
    if (ours && g_pin.where==PinHand && models->pin_bone.size() && vr::HandTransform(g_pin.hand,false,hand))
    {
        IKinematics& pin=*models->pin->dcast_PKinematics();
        pin.Callback(vrwi::CollapseHiddenBones,nullptr);
        vr::ApplyWeaponPose(pin,models->pin_poses,vrwi::BonePose,bctCustom,nullptr,0.f,nullptr,
            {models->pin_bone.c_str()},true,true,true,nullptr,{},{},nullptr,0.f,{},true,&models->pin_cache,Device.dwFrame);
        Fmatrix pin_pose; pin_pose.mul_43(hand,g_pin.in_hand);
        ::Render->set_Transform(&pin_pose);
        ::Render->add_Visual(models->pin);
    }
    return true;
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
            Fvector velocity;
            float speed=0.f;
            if (ThrowVelocity(pickup.throw_peak[b],smart_cast<CMissile*>(bolt),velocity,speed))
            {
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
    // A new grenade, or this one put away or gone: its pin is in.
    if (g_pin.missile!=missile->ID() || pickup.missile_stowed || (missile->VrThrown() && !pickup.missile_pending))
    {
        g_pin=GrenadePin{};
        g_pin.missile=missile->ID();
    }
    if (h<0) return;
    const bool needs_pin=smart_cast<CGrenade*>(missile)!=nullptr;
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
    // Keeping the hand closed on it readies it; a grenade once its pin is
    // pulled.
    if (held && !missile->VrThrown())
    {
        pickup.missile_pending=false;
        // Pushed back in, it is safe again.
        if (!needs_pin || StepGrenadePin(actor,*missile,h,tracked,hands)) missile->VrPrime();
        else if (missile->VrPrimed()) missile->VrUnprime();
        return;
    }
    if (released)
    {
        Fvector velocity;
        float speed=0.f;
        bool thrown=ThrowVelocity(pickup.throw_peak[h],missile,velocity,speed);
        // Its pin in, a grenade is not thrown: back on the belt.
        if (needs_pin && !g_pin.out)
        {
            if (thrown) Msg("* [VR body] %s: not thrown, its pin is in",missile->cNameSect().c_str());
            thrown=false;
        }
        // Its pin out, one let go slowly drops from the hand, live.
        else if (needs_pin && !thrown)
        {
            velocity=pickup.throw_peak[h];
            speed=velocity.magnitude();
            thrown=true;
        }
        if (thrown)
        {
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
    // What VrHandHoldsItem counts, plus the hand that took a device (shown
    // or not) and the active weapon's hand: the knife, binoculars or a gun.
    // Guns hid it from their own render; the knife no longer goes through
    // that path.
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    return VrHandHoldsItem(actor,hand) || hand==actor.m_vr_pickup.device_hand ||
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

// Both hands at the head: both stick clicks press the debug menu key there
// (custom17, F7 by default; scripts open the menu only with -dbg), and
// neither sprints nor jumps.
bool head_debug_hands=false;
bool VrDebugGesture() { return head_debug_hands; }

void VrDebugGestureUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands)
{
    static bool down=true;
    static u32 flushed=0;
    head_debug_hands=tracked[0] && tracked[1] && VrHandAtHead(hands[0].c) && VrHandAtHead(hands[1].c);
    const bool both=head_debug_hands && vr::SprintButton() && vr::JumpButton();
    if (both && !down && strstr(Core.Params,"-dbg"))
    {
        PressAction(actor,kCUSTOM17);
        Msg("* [VR debug] debug menu key (custom17)");
        for (int h=0;h<2;++h) vr::HandHaptic(h,0.5f);
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
    VrDebugGestureUpdate(actor,tracked,hands);
    for (int h=0; h<2; ++h)
    {
        // A free hand only: one holding anything uses its trigger for that.
        const bool at=tracked[h] && HandFree(actor,h) && !HandHoldsOther(actor,h) &&
            h!=VrDetectorHand(actor) && VrHandAtHead(hands[h].c);
        const bool trigger=at && vr::TriggerPulled(vr::HandTrigger(h));
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

bool VrHandAtEquipment(CActor& actor, const Fvector& position, int hand)
{
    // The helmet: headlamp and night vision (VrHeadGestureUpdate).
    if (VrHandAtHead(position)) return true;
    // With a hand, only a body slot that would act on its squeeze (one with
    // an item to take, or a place to put back what the hand holds): the slots
    // hang from the head, so bending to the floor brings empty ones down to
    // the items there, and they took the squeeze from the grab.
    if (vr::BodySlots())
        for (int kind=0; kind<vr::BodySlotCount; ++kind)
        {
            if (hand>=0 && !Available(actor,kind,hand)) continue;
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
        const bool squeezed=vr::HandTransform(h,false,hand) && vr::HandSqueeze(h)>vr::pull_threshold;
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
    ReleaseGrenadeModels();
    ReleaseBeltCopies();
}

int VrDetectorHand(CActor& actor)
{
    auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
    if (!g_vr_owns_present || !device || device->IsHidden()) return -1;
    // The hand that took it from the belt, else the hand without the weapon
    // (any weapon in a hand: an unprofiled one was not counted, and the
    // detector was shown in its hand).
    auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const int weapon_hand=weapon && !VrActiveItemLeaving(actor) ? weapon->VrWeaponHand() : -1;
    const int taken=actor.m_vr_pickup.device_hand;
    if (taken>=0 && taken!=weapon_hand) return taken;
    // Not taken by a hand: the free one, unless it is working the weapon.
    if (weapon_hand>=0 && weapon->VrMode() && weapon->VrLeftHandBusy()) return -1;
    return weapon_hand>=0 ? 1-weapon_hand : 1-vr::DominantHand();
}

bool VrHandHoldsItem(CActor& actor, int hand)
{
    // The device in whichever hand it is shown in (VrDetectorHand), taken by
    // that hand or opened by a key: one owner for its place, its controls and
    // what else the hand may do.
    return HandHoldsOther(actor,hand) || (hand>=0 && VrDetectorHand(actor)==hand);
}

namespace
{
// The body slot the item in this hand goes back to, or -1: the bolt to its
// pouch, the knife, binoculars, a grenade or the detector to their places.
int ReturnSlot(CActor& actor, int hand)
{
    const auto& pickup=actor.m_vr_pickup;
    if (pickup.bolt_hand==hand) return vr::BodyBolt;
    if (VrDetectorHand(actor)==hand) return vr::BodyDetector;
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
// the animation's own hand and fingers around it (VrItemBones).
struct AuthoredGrip
{
    bool valid=false;
    // Taken from the game's own hands holding it (CaptureItemGrip): final.
    bool live=false;
    Fvector basis[3];
    Fvector palm;
    Fmatrix hand_in_item;
    xr_vector<Fmatrix> locals;
    // The same hold in the other hand (MirrorGrip): its hand bone in the
    // mirrored item, and every bone's local pose; empty when the hands
    // model has no counterpart bones.
    Fmatrix mirror_hand_in_item;
    xr_vector<Fmatrix> mirror_locals;
    shared_str visual;
};
xr_map<shared_str,AuthoredGrip> g_authored_grips;

// The bone on the other side ("l_hand" <-> "r_hand", "bip01_l_finger1" <->
// "bip01_r_finger1"); itself for a bone on the centre line.
u16 Counterpart(IKinematics& model, u16 id)
{
    xr_string name=model.LL_BoneName_dbg(id);
    for (size_t at=0; at+1<name.size(); ++at)
    {
        if ((at && name[at-1]!='_') || (name[at]!='l' && name[at]!='r') || name[at+1]!='_') continue;
        name[at]=name[at]=='l' ? 'r' : 'l';
        const u16 other=model.LL_BoneID(name.c_str());
        return other!=BI_NONE ? other : id;
    }
    return id;
}

// The hold mirrored across the centre plane (x) into the other hand. Each
// bone of the other side is the mirror image of its counterpart, turned by
// how its own axes differ from its counterpart's mirrored ones in the bind
// pose (C = inverse(mirror x counterpart's bind) x own bind, its turn only):
// in the item, hand = mirror x hand x C(hand); a local pose is
// inverse(C(parent)) x counterpart's local x C(bone). The item is mirrored
// the same way (AuthoredHoldPose), which for a grenade, a detector or a
// knife is the item itself.
void MirrorGrip(AuthoredGrip& grip, IKinematics& model, u16 wrist)
{
    grip.mirror_locals.clear();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (bind.size()<count || grip.locals.size()!=count || wrist>=count) return;
    Fmatrix flip; flip.identity(); flip.i.x=-1.f;
    xr_vector<u16> other(count);
    xr_vector<Fmatrix> fix(count);
    for (u16 id=0;id<count;++id)
    {
        other[id]=Counterpart(model,id);
        Fmatrix mirrored; mirrored.mul_43(flip,bind[other[id]]);
        fix[id].invert(mirrored);
        fix[id].mulB_43(bind[id]);
        fix[id].c.set(0.f,0.f,0.f);
    }
    if (other[wrist]==wrist) return;
    grip.mirror_locals=grip.locals;
    for (u16 id=0;id<count;++id)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        if (other[id]==id || parent>=count) continue;
        Fmatrix local; local.mul_43(Fmatrix().invert(fix[parent]),grip.locals[other[id]]);
        grip.mirror_locals[id].mul_43(local,fix[id]);
    }
    grip.mirror_hand_in_item.mul_43(flip,grip.hand_in_item);
    grip.mirror_hand_in_item.mulB_43(fix[other[wrist]]);
}

shared_str GripKey(const shared_str& hands, const shared_str& section, bool left)
{
    string512 key; xr_sprintf(key,"%s|%s|%c",hands.c_str(),section.c_str(),left ? 'l' : 'r');
    return key;
}

// The hand's axes (toward the knuckles, toward the index side, and their
// cross), the centre of the fist and every bone's local pose, from a hands
// model whose bones are posed; to_item maps its space into the item's.
bool FillGrip(AuthoredGrip& grip, IKinematics& model, const Fmatrix& to_item, bool left, const shared_str& visual)
{
    const u16 wrist=model.LL_BoneID(left ? "l_hand" : "r_hand");
    const u16 index=model.LL_BoneID(left ? "bip01_l_finger1" : "bip01_r_finger1");
    const u16 middle=model.LL_BoneID(left ? "bip01_l_finger2" : "bip01_r_finger2");
    if (wrist==BI_NONE || index==BI_NONE || middle==BI_NONE) return false;
    const auto at=[&](u16 id) { Fvector p; to_item.transform_tiny(p,model.LL_GetTransform(id).c); return p; };
    const Fvector w=at(wrist), i=at(index), m=at(middle);
    Fvector knuckles; knuckles.add(i,m).mul(0.5f);
    Fvector forward; forward.sub(knuckles,w);
    Fvector side; side.sub(i,m);
    if (forward.square_magnitude()<=EPS_S || side.square_magnitude()<=EPS_S) return false;
    forward.normalize();
    side.mad(forward,-side.dotproduct(forward)).normalize_safe();
    grip.basis[0]=forward; grip.basis[1]=side;
    grip.basis[2].crossproduct(forward,side);
    // The fist's centre: behind the knuckles, in from the palm (the cross of
    // knuckles and index side is the right palm's normal, the back of a left
    // hand).
    grip.palm=knuckles;
    grip.palm.mad(forward,-0.045f).mad(grip.basis[2],left ? -0.025f : 0.025f);
    grip.hand_in_item.mul_43(to_item,model.LL_GetTransform(wrist));
    const u16 count=model.LL_BoneCount();
    grip.locals.resize(count);
    for (u16 id=0;id<count;++id)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        grip.locals[id]=model.LL_GetTransform(id);
        if (parent<count)
        {
            Fmatrix inverse; inverse.invert(model.LL_GetTransform(parent));
            grip.locals[id].mulA_43(inverse);
        }
    }
    grip.visual=visual;
    grip.valid=true;
    MirrorGrip(grip,model,wrist);
    return true;
}
// left: the animation's left hand holds it (detectors and flashlights are
// held in the left hand in first person; a knife in the right).
const AuthoredGrip* SampleAuthoredGrip(const shared_str& section, bool left=false)
{
    if (!g_player_hud || !section.size()) return nullptr;
    const shared_str& hands=g_player_hud->section_name();
    // Asked several times a frame (each hand's arms, each eye): the last
    // answer per side is kept by its shared strings, so the key below (a
    // format and a string-table insert) is built only on a change.
    struct Last { shared_str hands, section; const AuthoredGrip* grip=nullptr; bool set=false; };
    static Last last[2];
    Last& recent=last[left ? 1 : 0];
    if (recent.set && recent.hands==hands && recent.section==section) return recent.grip;
    const auto remember=[&](const AuthoredGrip* grip)
    {
        recent.hands=hands; recent.section=section; recent.grip=grip; recent.set=true;
        return grip;
    };
    // The cache's entries never move (a map), so a remembered pointer stays valid.
    const shared_str name=GripKey(hands,section,left);
    auto found=g_authored_grips.find(name);
    if (found!=g_authored_grips.end()) return remember(found->second.valid ? &found->second : nullptr);
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
    if (animated && anchor!=BI_NONE)
    {
        vrwi::PlayHandsMotion(*animated,idle->m_animations[0].mid);
        vrwi::EvaluateBones(*animated,*model,0.f);
        Fvector ypr=pSettings->r_fvector3(section,"item_orientation");
        ypr.mul(PI/180.f);
        Fmatrix offset; offset.setHPB(ypr.x,ypr.y,ypr.z);
        offset.translate_over(pSettings->r_fvector3(section,"item_position"));
        Fmatrix item, to_item;
        item.mul_43(model->LL_GetTransform(anchor),offset);
        to_item.invert(item);
        if (FillGrip(grip,*model,to_item,left,pSettings->r_string(hands,"visual")))
            Msg("* [VR body] %s: held as the first-person idle's %s hand holds it",section.c_str(),
                left ? "left" : "right");
    }
    if (visual) ::Render->model_Delete(visual);
    return grip.valid ? &grip : nullptr;
}

// The item placed so the animation's hand lies on the controller: its axes
// onto the controller hand's (knuckles forward, index side up the handle,
// as player_hud_legs.cpp poses the hands) and its fist on the grip centre.
// mirror: the controller is the other hand from the animation's. The hold
// is mirrored across the item's centre plane (x): a mirror image of the
// hand holds a mirror image of the item, which for a grenade, a detector
// or a knife is the item itself. Taken unmirrored, a left hand held a
// grenade with the right hand's grip, through its fingers.
bool AuthoredHoldPose(int hand, const AuthoredGrip& sampled, Fmatrix& pose, bool mirror=false)
{
    AuthoredGrip mirrored;
    if (mirror)
    {
        for (int n=0;n<3;++n) mirrored.basis[n].set(-sampled.basis[n].x,sampled.basis[n].y,sampled.basis[n].z);
        // The cross of two mirrored axes turns the other way.
        mirrored.basis[2].invert();
        mirrored.palm.set(-sampled.palm.x,sampled.palm.y,sampled.palm.z);
    }
    const AuthoredGrip& authored=mirror ? mirrored : sampled;
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
    const AuthoredGrip* authored=SampleAuthoredGrip(device.HudSection(),true);
    return authored && AuthoredHoldPose(hand,*authored,pose,hand!=0);
}
}

namespace
{
// An item's hold as the game's own first-person hands show it, once they
// rest in its idle: the idle sampled on a private copy (SampleAuthoredGrip)
// put about a quarter of GAMMA's guns off their hands, which the same
// capture from the live hands fixed (CVrWeaponInteraction::CaptureLiveHands).
// Taken when the hand has kept still on the item for two frames (the
// idle's blend in is over), at 0.4 s whatever it shows; once per hands and
// item section for the session.
struct GripCapture { shared_str key; Fmatrix last=Fidentity; int still=0; u32 since=0; };
GripCapture g_grip_capture[2];

void CaptureItemGrip(CHudItem& item, bool left)
{
    GripCapture& capture=g_grip_capture[left ? 1 : 0];
    attachable_hud_item* hud=item.HudItemData();
    if (!g_player_hud || !hud || !hud->m_model || !g_player_hud->m_model || !g_player_hud->m_model_2)
    { capture=GripCapture{}; return; }
    const shared_str key=GripKey(g_player_hud->section_name(),item.HudSection(),left);
    if (capture.key!=key) { capture=GripCapture{}; capture.key=key; }
    // Sampled first: the capture replaces that sample.
    if (!SampleAuthoredGrip(item.HudSection(),left)) return;
    AuthoredGrip& grip=g_authored_grips[key];
    if (grip.live) return;
    if (item.GetState()!=CHUDState::eIdle || g_player_hud->script_anim_part!=u8(-1))
    { capture.since=0; capture.still=0; return; }
    if (!capture.since) { capture.since=Device.dwTimeGlobal; return; }
    // The item where the game places it now, from the hands model it hangs
    // from (player_hud::calc_transform), not its transform (VR's, for the
    // device, once VrUpdateHudItems has run).
    const bool second=hud->m_attach_place_idx!=0;
    IKinematics* hands=(second ? g_player_hud->m_model_2 : g_player_hud->m_model)->dcast_PKinematics();
    const Fmatrix& hands_world=second ? g_player_hud->m_transform_2 : g_player_hud->m_transform;
    const u16 wrist=hands ? hands->LL_BoneID(left ? "l_hand" : "r_hand") : u16(BI_NONE);
    if (wrist==BI_NONE) return;
    Fmatrix item_world;
    g_player_hud->calc_transform(hud->m_attach_place_idx,hud->m_attach_offset,item_world,
        hud->m_measures.m_bLeadGunLeftHand);
    Fmatrix to_item; to_item.invert(item_world); to_item.mulB_43(hands_world);
    Fmatrix now; now.mul_43(to_item,hands->LL_GetTransform(wrist));
    const bool still=now.c.distance_to(capture.last.c)<=0.0015f &&
        now.k.dotproduct(capture.last.k)>=_cos(deg2rad(0.5f)) && now.j.dotproduct(capture.last.j)>=_cos(deg2rad(0.5f));
    capture.last=now;
    capture.still=still ? capture.still+1 : 0;
    if (capture.still<2 && Device.dwTimeGlobal-capture.since<400) return;
    AuthoredGrip live;
    if (!FillGrip(live,*hands,to_item,left,grip.visual)) return;
    const float moved=live.palm.distance_to(grip.palm);
    live.live=true;
    grip=live;
    Msg("* [VR body] %s: hold taken from the game's own %s hand (the sampled one was %.0f mm off)",
        item.HudSection().c_str(),left ? "left" : "right",moved*1000.f);
}
}

void VrUpdateHudItems(CActor& actor)
{
    auto& pickup=actor.m_vr_pickup;
    pickup.device_pose_valid=pickup.knife_pose_valid=false;
    if (!g_vr_owns_present || !vr::GameplayActive()) return;
    if (vr::AnimatedGrip())
    {
        // The device in the left hand's animation, a grenade or knife in
        // the right's, while it is out.
        auto* shown=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
        if (shown && VrDetectorHand(actor)>=0) CaptureItemGrip(*shown,true);
        PIItem active=actor.inventory().ActiveItem();
        auto* missile=smart_cast<CMissile*>(active);
        if (missile && VrMissileHand(actor)>=0 && !missile->VrThrown()) CaptureItemGrip(*missile,false);
        else if (CWeapon* belt=BeltItemInHand(actor)) CaptureItemGrip(*belt,false);
    }
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

bool VrMissilePose(CActor& actor, Fmatrix& pose)
{
    auto* missile=smart_cast<CMissile*>(actor.inventory().ActiveItem());
    const int hand=VrMissileHand(actor);
    if (!missile || hand<0 || missile->VrThrown()) return false;
    attachable_hud_item* hud=missile->HudItemData();
    if (!hud || !hud->m_model) return false;
    // Held as the knife is: where the first-person animation's hand holds
    // it. The world model at a generic hand pose sat through the fingers.
    const AuthoredGrip* authored=SampleAuthoredGrip(missile->HudSection());
    return authored && AuthoredHoldPose(hand,*authored,pose,hand==0);
}

bool VrKnifePose(CActor& actor, Fmatrix& pose)
{
    bool blade=false;
    CWeapon* item=BeltItemInHand(actor,&blade);
    if (!actor.m_vr_pickup.knife_pose_valid || !item) return false;
    // As the flat-screen idle animation's hand holds it, when that is known.
    if (const AuthoredGrip* authored=SampleAuthoredGrip(item->HudSection()))
        if (AuthoredHoldPose(item->VrWeaponHand(),*authored,pose,item->VrWeaponHand()==0)) return true;
    if (!HandHoldPose(item->VrWeaponHand(),actor.m_vr_pickup.knife_hold,
        item->HudItemData()->m_model->dcast_RenderVisual(),blade,pose)) pose=actor.m_vr_pickup.knife_pose;
    return true;
}

bool VrItemBones(CActor& actor, int side, VrHandBones& bones)
{
    // The idle animation's own hand, bone for bone, where the item is drawn
    // (the knife, binoculars, a grenade, the detector or flashlight); in the
    // other hand, its mirror image. Drawn by the controller's pose with the
    // fingers closing on contact, the hand missed the item: a grenade
    // through the palm, a detector above the fingers.
    if (!vr::AnimatedGrip()) return false;
    const AuthoredGrip* authored=nullptr;
    bool left_animation=false;
    Fmatrix pose;
    auto* missile=smart_cast<CMissile*>(actor.inventory().ActiveItem());
    if (CWeapon* knife=BeltItemInHand(actor); knife && knife->VrWeaponHand()==side)
    {
        authored=SampleAuthoredGrip(knife->HudSection());
        if (!authored || !VrKnifePose(actor,pose)) return false;
    }
    else if (missile && VrMissileHand(actor)==side)
    {
        authored=SampleAuthoredGrip(missile->HudSection());
        if (!authored || !VrMissilePose(actor,pose)) return false;
    }
    else if (VrDetectorHand(actor)==side)
    {
        auto* device=smart_cast<CCustomDevice*>(actor.inventory().ItemFromSlot(DETECTOR_SLOT));
        // A flashlight turned over in the fist is not as the animation holds it.
        if (!device || (actor.m_vr_pickup.device_flipped && smart_cast<CFlashlight*>(device)) ||
            !DeviceInPalm(side,*device,pose)) return false;
        authored=SampleAuthoredGrip(device->HudSection(),true);
        left_animation=true;
    }
    if (!authored) return false;
    const bool mirror=(side==0)!=left_animation;
    const xr_vector<Fmatrix>& locals=mirror ? authored->mirror_locals : authored->locals;
    if (locals.empty()) return false;
    bones.hand.mul_43(pose,mirror ? authored->mirror_hand_in_item : authored->hand_in_item);
    bones.locals=&locals;
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
// Which end of the model is the eyepiece end, from how the game shows the
// binoculars in first person (their eyepieces toward the camera): logged
// once per HUD section. The mesh alone cannot tell; the stock model's
// objectives measured narrower than its eyepieces.
int EyepieceEnd(CActor& actor, CWeaponBinoculars& binoculars)
{
    const auto& pickup=actor.m_vr_pickup;
    const int end=pickup.knife_pose_valid ? vr::BinocularEyepieceEnd(pickup.knife_hold.k.z) : 0;
    // A plain copy: a static shared_str would outlive the string container.
    static string128 logged="";
    static int logged_end=2;
    LPCSTR section=binoculars.HudSection().c_str();
    if (!section) section="";
    if (end!=logged_end || xr_strcmp(logged,section))
    {
        xr_strcpy(logged,section);
        logged_end=end;
        Msg("* [VR binoculars] %s: eyepieces at the %s end (model +Z %.2f along the first-person view)",section,
            end>0 ? "+Z" : end<0 ? "-Z" : "nearer the head",pickup.knife_pose_valid ? pickup.knife_hold.k.z : 0.f);
    }
    return end;
}
// The eyepieces: the middle of the eyepiece end's face of the model in the
// hand (EyepieceEnd), looking along the model's length to the
// other end (vr::BinocularsAtEyesPolicy).
bool BinocularsRaised(CActor& actor, bool raised, float& distance, float& alignment)
{
    distance=flt_max; alignment=-1.f;
    auto* binoculars=smart_cast<CWeaponBinoculars*>(BeltItemInHand(actor));
    Fmatrix head, pose;
    if (!binoculars || !vr::HeadTransform(head) || !VrKnifePose(actor,pose)) return false;
    IRenderVisual* visual=binoculars->HudItemData()->m_model->dcast_RenderVisual();
    if (!visual) return false;
    const Fbox& box=visual->getVisData().box;
    Fvector rear, front; box.getcenter(rear); front=rear;
    rear.z=box.min.z; front.z=box.max.z;
    const int end=EyepieceEnd(actor,*binoculars);
    if (end>0) std::swap(front,rear);
    pose.transform_tiny(rear); pose.transform_tiny(front);
    if (!end && front.distance_to(head.c)<rear.distance_to(head.c)) std::swap(front,rear);
    Fvector forward; forward.sub(front,rear); forward.normalize_safe();
    distance=rear.distance_to(head.c);
    alignment=forward.dotproduct(head.k);
    return vr::BinocularsAtEyesPolicy(distance,alignment,raised);
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
    float distance=flt_max, alignment=-1.f;
    g_binoculars_raised=g_vr_owns_present && vr::GameplayActive() && BinocularsRaised(actor,was,distance,alignment);
    if (g_binoculars_raised!=was)
    {
        Msg("* [VR binoculars] %s: eyepieces %.0f mm from the eyes, %.0f deg off the view",
            g_binoculars_raised ? "raised" : "lowered",distance<flt_max ? distance*1000.f : -1.f,
            rad2deg(acosf(clampr(alignment,-1.f,1.f))));
        if (auto* binoculars=smart_cast<CWeaponBinoculars*>(actor.inventory().ActiveItem()))
            vr::HandHaptic(binoculars->VrWeaponHand(),0.06f);
    }
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

namespace
{
// A device its script tunes with the mouse buttons ([vr_derivation]
// mouse_tuned_devices; GAMMA's RF receiver: left button up, right down,
// Shift ten at a time, with nothing else in the hands): the trigger tunes
// up and the upper face button down, a step a press; held half a second,
// ten at a time every 0.15 s. Sent as those keys' releases, which is what
// its script reads.
void TuneDevice(CActor& actor, CCustomDevice& device, int hand, bool up, bool down, bool up_was, bool down_was)
{
    static u32 next_step=0;
    static bool told=false;
    if (!vrwi::NameHasAny(device.cNameSect().c_str(),vrwi::VrNames().mouse_tuned_devices)) return;
    const bool pressed=(up && !up_was) || (down && !down_was);
    if (!pressed && !up && !down) return;
    // The receiver's script tunes only with nothing in the hands; the left
    // button's release would also reach a weapon as its trigger.
    if (actor.inventory().ActiveItem())
    {
        if (pressed && !told)
            Msg("* [VR body] %s: tuned with no weapon in the hands (its script's rule)",device.cNameSect().c_str());
        told=told || pressed;
        return;
    }
    const auto step=[&](bool upward, bool ten)
    {
        if (ten) Level().IR_OnKeyboardHold(DIK_LSHIFT);
        Level().IR_OnKeyboardRelease(upward ? MOUSE_1 : MOUSE_2);
        vr::HandHaptic(hand,0.04f);
    };
    if (pressed)
    {
        step(up && !up_was,false);
        next_step=Device.dwTimeGlobal+500;
    }
    else if (int(Device.dwTimeGlobal-next_step)>=0)
    {
        step(up,true);
        next_step=Device.dwTimeGlobal+150;
    }
}
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
            // Binoculars let go away from their place on the chest are
            // dropped with the hand's motion; at the chest, and always for
            // the knife, they go back.
            auto* binoculars=held ? smart_cast<CWeaponBinoculars*>(active) : nullptr;
            Fvector home; float range=0.f; Fmatrix hand;
            const bool at_slot=VrBodySlotHolster(actor,BINOCULAR_SLOT,home,range) &&
                vr::HandTransform(h,false,hand) && hand.c.distance_to(home)<=range;
            pickup.slot_hold_hand=-1;
            if (binoculars && !at_slot) VrReleaseHeldWeapon(actor,*binoculars);
            else
            {
                actor.inventory().Activate(NO_ACTIVE_SLOT,true);
                vr::HandHaptic(h,0.15f);
            }
        }
    }
    // Binoculars made active by the inventory, a key or a script, not taken
    // by a hand: back to their place on the chest, where a hand takes them
    // and holds them by its grip. Out with no hand holding them, they were
    // drawn in a hand and only a second grab at the chest put them back.
    // Not when already on its way out or to another item, nor without body
    // slots: no chest to take them from, they stay out as the game drew them.
    if (pickup.slot_hold_hand<0 && vr::BodySlots())
        if (auto* binoculars=smart_cast<CWeaponBinoculars*>(active))
            if (!binoculars->VrSupported() && actor.inventory().GetNextActiveSlot()==binoculars->CurrSlot())
            {
                actor.inventory().Activate(NO_ACTIVE_SLOT,true);
                Msg("* [VR binoculars] made active without a hand: back on the chest");
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
    // One owner: the hand it is shown in (VrDetectorHand), whose trigger and
    // button work it. One the hand that took it gave up for a weapon (a key
    // drew it there) moved to the other hand, which does not grip it: held
    // there as one opened by a key, until put back at its slot or by a key.
    // Its controls followed the hand that took it, the weapon hand.
    const int shown=device ? VrDetectorHand(actor) : -1;
    if (shown>=0 && pickup.device_hand>=0 && shown!=pickup.device_hand) pickup.device_hand=-1;
    // Also after this update has not run (a load, a menu) or for another
    // actor: kept across a load, a trigger held through it toggled the light.
    static int controls_hand=-1;
    static u32 controls_frame=0;
    static const CActor* controls_actor=nullptr;
    const bool continuous=controls_actor==&actor && Device.dwFrame-controls_frame<=1;
    controls_frame=Device.dwFrame; controls_actor=&actor;
    if (shown!=controls_hand || !continuous)
    {
        // A trigger or button already held in the new hand is not a press.
        controls_hand=shown;
        pickup.device_trigger_down=shown>=0 && vr::TriggerPulled(vr::HandTrigger(shown));
        pickup.device_button_down=shown>=0 && vr::HandSecondaryButton(shown);
    }
    const bool out=device && !(device->IsHidden() && !coming);
    if (const int h=shown; out && h>=0 && tracked[h])
    {
        if (pickup.device_hand==h && !grip[h])
        {
            PressAction(actor,kDETECTOR);
            vr::HandHaptic(h,0.12f);
            pickup.device_hand=-1;
        }
        else
        {
            const bool trigger=vr::TriggerPulled(vr::HandTrigger(h));
            if (trigger && !pickup.device_trigger_down)
                if (auto* light=smart_cast<CFlashlight*>(device))
                {
                    light->VrToggleLight();
                    vr::HandHaptic(h,0.08f);
                    Msg("* [VR body] %s: light %s by the %s trigger",device->cNameSect().c_str(),
                        light->torch_active() ? "on" : "off",h ? "right" : "left");
                }
            TuneDevice(actor,*device,h,trigger,vr::HandSecondaryButton(h),pickup.device_trigger_down,
                pickup.device_button_down);
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
    // With the body shown, flat against its chest where it is, facing out
    // from it there.
    VrBodyRest rest;
    const bool resting=VrOnBodyRest(actor,vr::BodyPda,rest);
    const Fmatrix frame=resting ? OutwardFrame(rest.out) : body;
    // Nearly flat on the chest: a steeper lean keeps the top on it and
    // swings the bottom edge out.
    const float lean=deg2rad(8.f);
    Fvector out, up;
    out.set(frame.k).mul(_cos(lean)).mad(frame.j,_sin(lean));
    up.set(frame.j).mul(_cos(lean)).mad(frame.k,-_sin(lean));
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
    axes[thin]=out; axes[longest]=up; axes[3-thin-longest]=frame.i;
    // Keep the frame's handedness (i = j x k).
    Fvector check; check.crossproduct(axes[1],axes[2]);
    if (check.dotproduct(axes[0])<0.f) axes[3-thin-longest].invert();
    Fmatrix pose; pose.identity();
    pose.i=axes[0]; pose.j=axes[1]; pose.k=axes[2];
    if (resting) { DrawModelResting(actor,visual,pose,1.f,rest); return; }
    // Its back against the chest.
    Fvector centre; box.getcenter(centre);
    pose.transform_dir(centre);
    pose.c.sub(point,centre).mad(out,0.5f*extent[thin]);
    DrawModel(visual,pose);
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
    // With the body shown, upright against it, facing out where it hangs.
    VrBodyRest rest;
    if (VrOnBodyRest(actor,vr::BodyBolt,rest))
    {
        DrawModelResting(actor,visual,OutwardFrame(rest.out),scale,rest);
        return true;
    }
    // Upright, facing out from the body, centred on the slot.
    Fmatrix pose; pose.identity();
    pose.i.set(body.i).mul(scale); pose.j.set(body.j).mul(scale); pose.k.set(body.k).mul(scale);
    Fvector centre; box.getcenter(centre); pose.transform_dir(centre);
    pose.c.sub(point,centre);
    DrawModel(visual,pose);
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
        IRenderVisual* visual=kind==vr::BodyDetector ? BeltVisual(item) : item->object().Visual();
        Fvector point;
        if (!visual || !VrBodySlotPoint(actor,kind,point)) continue;
        // With the body shown, against it, facing out where it hangs.
        VrBodyRest rest;
        if (VrOnBodyRest(actor,kind,rest))
        {
            const Fmatrix frame=OutwardFrame(rest.out);
            Fmatrix pose; pose.identity();
            pose.i=frame.i; pose.j=frame.k; pose.k.set(0,-1,0);
            DrawModelResting(actor,visual,pose,1.f,rest);
            continue;
        }
        Fmatrix pose; pose.identity();
        pose.i=body.i; pose.j=body.k; pose.k.set(0,-1,0);
        DrawModelAt(visual,pose,point);
    }
    RenderVrChestPda(actor,body);
    // The quick-use items in their pockets, always shown, upright, small
    // enough to fit a pocket ([vr_body_slots] pocket_size, metres along the
    // longest side); none for a pocket whose item is in a hand.
    const float pocket_size=READ_IF_EXISTS(pSettings,r_float,"vr_body_slots","pocket_size",0.10f);
    for (int pocket=0; pocket<4; ++pocket)
    {
        const auto& held=actor.m_vr_pickup.pocket_held;
        if (held[0]==pocket || held[1]==pocket) continue;
        PIItem item=PocketItem(actor,pocket);
        IRenderVisual* visual=item ? item->object().Visual() : nullptr;
        const int kind=vr::BodyPocket1+pocket;
        Fvector point;
        if (!visual || !VrBodySlotPoint(actor,kind,point)) continue;
        const float scale=FitScale(visual,pocket_size);
        VrBodyRest rest;
        if (VrOnBodyRest(actor,kind,rest))
        {
            DrawModelResting(actor,visual,OutwardFrame(rest.out),scale,rest);
            continue;
        }
        Fmatrix pose=body;
        pose.i.mul(scale); pose.j.mul(scale); pose.k.mul(scale);
        DrawModelAt(visual,pose,point);
    }
    const auto draw=[](PIItem item, Fmatrix pose, const Fvector& point)
    {
        if (IRenderVisual* visual=item ? item->object().Visual() : nullptr) DrawModelAt(visual,pose,point);
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
    }
}
