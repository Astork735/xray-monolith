// The per-frame VR hands update for weapons and held items (UpdateVrWeapons).
#include "stdafx.h"
#include "VrWeaponInteractionInternal.h"
#include "VrBodySlots.h"
#include "Bolt.h"
#include "Weapon.h"
#include "xr_level_controller.h"
#include "Actor.h"
#include "inventory.h"
#include "PhysicsShellHolder.h"
#include "Level.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/CameraBase.h"
#include "../xrEngine/Vr/vr_gameplay.h"
#include "../xrPhysics/PhysicsShell.h"
using namespace vrwi;

void VrSnapHeldItems(CActor& actor)
{
    if (!g_vr_owns_present) return;
    auto& state=actor.m_vr_pickup;
    for (int side=0;side<2;++side)
    {
        if (state.grabbed_id[side]==u16(-1)) continue;
        auto* held=smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(state.grabbed_id[side]));
        Fmatrix hand;
        if (!held || held->getDestroy() || held->H_Parent() || !vr::HandTransform(side,false,hand)) continue;
        held->XFORM().mul_43(hand,state.grab_offset[side]);
        // Held at the near side of a wall, as UpdateVrPickup put its body.
        if (vr::PhysicalHands()) VrNearSide(held->XFORM().c,0.05f);
        held->spatial_move();
    }
}

// The use action as the bound key, to whatever takes keys now as a key press
// would reach it: the level, so script key handlers (doors, special and
// generic actions) see it as they do on the desktop and the actor gets it
// (talking, pickup); or an input-grabbing tutorial prompt in front of it (the
// campfire's "use to ignite", tutorial_campfire_ignite), which acts on its
// guard key and passes the key on to the level. Sent to the level directly,
// the prompt never heard it. Without a binding, to the actor only.
static void VrUseKey(CActor& actor, bool press)
{
    if (const int dik=get_action_dik(kUSE))
    {
        IInputReceiver* receiver=pInput ? pInput->CurrentIR() : nullptr;
        if (!receiver) receiver=&Level();
        if (press) receiver->IR_OnKeyboardPress(dik); else receiver->IR_OnKeyboardRelease(dik);
        return;
    }
    if (press) actor.IR_OnKeyboardPress(kUSE); else actor.IR_OnKeyboardRelease(kUSE);
}

// A weapon taken by hand (UpdateVrPickup): wielded in that hand once the
// pickup lands, in its slot (making room there if another weapon holds it);
// or, stowed, put in its holster on that hand's side. An item still being put
// away is waited for, never forced aside (a forced activation would skip its
// hide); meanwhile the new weapon is shown in the hand that took it
// (RenderHolster). The decision is vr::WieldDecision (VrPickupPolicy.h).
static void UpdateVrWield(CActor& actor)
{
    auto& pickup=actor.m_vr_pickup;
    if (pickup.wield_id==u16(-1)) return;
    auto* weapon=smart_cast<CWeapon*>(Level().Objects.net_Find(pickup.wield_id));
    if (!weapon || int(Device.dwTimeGlobal-pickup.wield_expires)>0) { pickup.wield_id=u16(-1); return; }
    const vr::ActiveItem state=VrActiveItemState(actor,weapon);
    const u16 base=weapon->BaseSlot();
    const bool slot_weapon=base==INV_SLOT_2 || base==INV_SLOT_3;
    const vr::WieldStep step=vr::WieldDecision(pickup.wield_stow,weapon->H_Parent()==&actor,state);
    if (vr::ContactDebug()>0 && step!=vr::WieldStep::Wait && step!=vr::WieldStep::HoldHand)
    {
        static const char* const steps[]={"wait","into its holster","into its slot and drawn","drawn by the game",
            "shown in the hand","given up (another weapon is out or on its way)"};
        Msg("* [VR hands] %s taken by the %s hand: %s",weapon->cNameSect().c_str(),
            pickup.wield_hand ? "right" : "left",steps[int(step)]);
    }
    switch (step)
    {
    case vr::WieldStep::Wait:
        return;
    case vr::WieldStep::Cancel:
        // Its claim (set while it waited in the hand) lapses: UpdateHandClaim.
        break;
    case vr::WieldStep::HoldHand:
        weapon->VrSetWeaponHand(pickup.wield_hand);
        return;
    case vr::WieldStep::Stow:
        // Left in its holster (the free one: VrFreeWeaponSlot); the weapon in
        // the other hand stays where it is.
        if (slot_weapon)
        {
            const u16 slot=VrFreeWeaponSlot(actor,*weapon);
            if (slot!=NO_ACTIVE_SLOT && weapon->CurrSlot()!=slot) actor.inventory().Slot(slot,weapon,true);
        }
        // Holstered on that hand's side, not drawn: no claim on the hand.
        if (CVrWeaponInteraction* vr=weapon->VrInteraction()) vr->SetWeaponHand(pickup.wield_hand,false);
        break;
    case vr::WieldStep::Adopt:
        // The game's own pickup already drew it (a free slot is activated,
        // CInventory::Slot): in the hand that took it.
        weapon->VrSetWeaponHand(pickup.wield_hand);
        break;
    case vr::WieldStep::Activate:
        if (slot_weapon)
        {
            // Into a free weapon slot, leaving a holstered weapon on its
            // shoulder: pushing it into the backpack took it off the body.
            // With both holding weapons, the base slot's goes to the backpack.
            u16 slot=VrFreeWeaponSlot(actor,*weapon);
            if (slot==NO_ACTIVE_SLOT)
            {
                if (PIItem occupant=actor.inventory().ItemFromSlot(base)) actor.inventory().Ruck(occupant);
                slot=base;
            }
            if (weapon->CurrSlot()!=slot) actor.inventory().Slot(slot,weapon,true);
        }
        if (weapon->CurrSlot()==INV_SLOT_2 || weapon->CurrSlot()==INV_SLOT_3)
        {
            weapon->VrSetWeaponHand(pickup.wield_hand);
            actor.inventory().Activate(weapon->CurrSlot(),true);
            vr::HandHaptic(pickup.wield_hand,0.2f);
        }
        break;
    }
    pickup.wield_id=u16(-1);
}

u32 VrTextureBytes()
{
    // Every texture the renderer holds (lightmaps aside): walks its list, so
    // only around rare work (a pickup, a model load), never per frame.
    if (!g_vr_owns_present || !Device.m_pRender) return 0;
    u32 base=0, count=0, lmaps=0, lmap_count=0;
    Device.m_pRender->ResourcesGetMemoryUsage(base,count,lmaps,lmap_count);
    return base;
}
// vr_contact_debug: the frames right after a weapon is taken into a hand log
// every step of 2 ms or more, so what a grab costs is all accounted for.
u32 g_vr_work_detail_until=0;
void VrWorkDetail(u32 frames)
{
    g_vr_work_detail_until=Device.dwFrame+frames;
}
bool VrWorkDetailed()
{
    return vr::ContactDebug()>0 && Device.dwFrame<=g_vr_work_detail_until;
}
void VrNoteWorkLoaded(const char* what, const char* object, float ms, u32 textures_before)
{
    const float least=VrWorkDetailed() ? 2.f : vr::ContactDebug()>0 ? 20.f : 100.f;
    if (!g_vr_owns_present || ms<least) return;
    // What it loaded: a first HUD model's textures are what stall a grab.
    const u32 textures_after=textures_before ? VrTextureBytes() : 0;
    if (textures_after>textures_before)
        Msg("* [VR work] %s: %s took %.0f ms, loading %.1f MB of textures (frame %u)",object && *object ? object : "-",
            what,ms,float(textures_after-textures_before)/(1024.f*1024.f),Device.dwFrame);
    else
        Msg("* [VR work] %s: %s took %.0f ms (frame %u)",object && *object ? object : "-",what,ms,Device.dwFrame);
}
void VrNoteWork(const char* what, const char* object, float ms)
{
    VrNoteWorkLoaded(what,object,ms,0);
}

namespace
{
// A HUD model made ahead of a grab, held until it is no longer wanted.
struct VrPrewarmed
{
    shared_str visual;
    IRenderVisual* model=nullptr;
    u32 ticket=0;     // its textures on the renderer's worker (0: none to load)
    u32 started=0;    // Device.dwTimeContinual when made
    u32 wanted=0;     // Device.dwTimeGlobal when last wanted
    bool ready=false;
};
xr_vector<VrPrewarmed> g_vr_prewarmed;
u32 g_vr_prewarm_frame=u32(-1);
// Two models at most (a GAMMA rifle's textures are 10-90 MB), each kept
// 10 s after the last frame a hand or the pointer was on it.
constexpr size_t vr_prewarm_max=2;
constexpr u32 vr_prewarm_keep_ms=10000;
// The pointer must rest on a weapon this long before it counts as intent.
constexpr u32 vr_prewarm_dwell_ms=200;
// An empty hand this near a weapon is reaching for it.
constexpr float vr_prewarm_reach=0.4f;
// One step of preparing (a model made, a measurement) a frame, so each
// costs one frame at most a few tens of ms while the hand is on its way.
bool VrPrewarmTakeStep()
{
    if (g_vr_prewarm_frame==Device.dwFrame) return false;
    g_vr_prewarm_frame=Device.dwFrame;
    return true;
}
VrPrewarmed* VrPrewarmFind(LPCSTR visual)
{
    for (VrPrewarmed& entry:g_vr_prewarmed)
        if (!xr_strcmp(entry.visual.c_str(),visual)) return &entry;
    return nullptr;
}
void VrPrewarmDrop(size_t index)
{
    // Textures still loading stay referenced by the renderer's batch.
    if (g_vr_prewarmed[index].model) ::Render->model_Delete(g_vr_prewarmed[index].model);
    g_vr_prewarmed.erase(g_vr_prewarmed.begin()+index);
}
// Notes the models whose textures came in, and lets go of unwanted ones.
void VrPrewarmUpdate()
{
    for (size_t n=0;n<g_vr_prewarmed.size();)
    {
        VrPrewarmed& entry=g_vr_prewarmed[n];
        if (!entry.ready && Device.m_pRender->ResourcesBackgroundDone(entry.ticket))
        {
            entry.ready=true;
            if (vr::ContactDebug()>0)
                Msg("* [VR prewarm] %s: textures in after %u ms",entry.visual.c_str(),
                    Device.dwTimeContinual-entry.started);
        }
        if (Device.dwTimeGlobal-entry.wanted>vr_prewarm_keep_ms) VrPrewarmDrop(n);
        else ++n;
    }
}
}
bool VrPrewarmVisual(const shared_str& visual, LPCSTR object)
{
    if (!g_vr_owns_present || !visual.size() || !Device.m_pRender) return true;
    if (VrPrewarmed* entry=VrPrewarmFind(visual.c_str()))
    {
        entry->wanted=Device.dwTimeGlobal;
        if (!entry->ready && Device.m_pRender->ResourcesBackgroundDone(entry->ticket)) entry->ready=true;
        return entry->ready;
    }
    if (!VrPrewarmTakeStep()) return false;
    if (g_vr_prewarmed.size()>=vr_prewarm_max)
    {
        size_t oldest=0;
        for (size_t n=1;n<g_vr_prewarmed.size();++n)
            if (g_vr_prewarmed[n].wanted<g_vr_prewarmed[oldest].wanted) oldest=n;
        VrPrewarmDrop(oldest);
    }
    // As the VR copy makes it (EnsureVisual), which has always made it first,
    // before the game's own HUD model: no hud_loading, so its shaders are the
    // ones the copy had.
    // A section the game never spawns may name a missing file (fatal to
    // load): remembered as having nothing to prepare.
    CTimer timer; timer.Start();
    IRenderVisual* model=nullptr;
    u32 ticket=0;
    if (VrHudModelLoadable(visual.c_str()))
    {
        Device.m_pRender->ResourcesBackgroundBegin();
        model=::Render->model_Create(visual.c_str());
        ticket=Device.m_pRender->ResourcesBackgroundEnd();
    }
    VrNoteWork("prepared ahead: HUD model (textures load in the background)",object,timer.GetElapsed_sec()*1000.f);
    VrPrewarmed entry;
    entry.visual=visual;
    entry.model=model;
    entry.ticket=ticket;
    entry.started=Device.dwTimeContinual;
    entry.wanted=Device.dwTimeGlobal;
    entry.ready=!ticket;
    if (vr::ContactDebug()>0)
        Msg("* [VR prewarm] %s: %s prepared in %.0f ms, %s",object && *object ? object : "-",visual.c_str(),
            timer.GetElapsed_sec()*1000.f,ticket ? "textures loading in the background" : "textures already in");
    g_vr_prewarmed.push_back(entry);
    return entry.ready;
}
void VrPrewarmWait(LPCSTR visual, LPCSTR object)
{
    VrPrewarmed* entry=visual ? VrPrewarmFind(visual) : nullptr;
    if (!entry || entry->ready || !Device.m_pRender) return;
    CTimer timer; timer.Start();
    Device.m_pRender->ResourcesBackgroundWait(entry->ticket);
    entry->ready=true;
    VrNoteWork("waited for its textures, still loading in the background",object,timer.GetElapsed_sec()*1000.f);
}
void VrPrewarmNear(CActor& actor, bool controls, const bool tracked[2], const Fmatrix hands[2])
{
    VrPrewarmUpdate();
    // The search every 4th frame (about 20 a second): the pointer must rest
    // 200 ms anyway, and a hand reaching 40 cm takes longer.
    if (!controls || Device.dwFrame%4) return;
    // Only on intent: the weapon the pointer has rested on, and the one an
    // empty hand is closing in on; nothing merely nearby. Nearest first.
    struct Candidate { CWeapon* weapon; float distance; };
    constexpr int most=int(vr_prewarm_max)+1;
    Candidate found[most];
    int count=0;
    const auto consider=[&](CObject* object, float distance)
    {
        auto* weapon=smart_cast<CWeapon*>(object);
        if (!weapon || weapon->H_Parent() || weapon->getDestroy() || !weapon->HudSection().size()) return;
        for (int n=0;n<count;++n)
            if (found[n].weapon==weapon) { found[n].distance=std::min(found[n].distance,distance); return; }
        if (count<most) found[count++]={weapon,distance};
        else
        {
            int far_one=0;
            for (int n=1;n<count;++n) if (found[n].distance>found[far_one].distance) far_one=n;
            if (distance<found[far_one].distance) found[far_one]={weapon,distance};
        }
    };
    const auto& pickup=actor.m_vr_pickup;
    // The pointer's target, once it has rested on it.
    static u16 dwell_id=u16(-1);
    static u32 dwell_since=0;
    if (pickup.pick_id!=dwell_id) { dwell_id=pickup.pick_id; dwell_since=Device.dwTimeGlobal; }
    if (dwell_id!=u16(-1) && Device.dwTimeGlobal-dwell_since>=vr_prewarm_dwell_ms)
        consider(Level().Objects.net_Find(dwell_id),0.f);
    // The weapon nearest each empty tracked hand, within reach of it.
    static xr_vector<ISpatial*> nearby;
    for (int h=0;h<2;++h)
    {
        if (!tracked[h] || pickup.grabbed_id[h]!=u16(-1)) continue;
        nearby.clear();
        g_SpatialSpace->q_sphere(nearby,0,STYPE_COLLIDEABLE,hands[h].c,vr_prewarm_reach);
        CObject* nearest=nullptr;
        float best=flt_max;
        for (ISpatial* spatial:nearby)
            if (CObject* object=spatial->dcast_CObject(); object && smart_cast<CWeapon*>(object))
            {
                const float distance=object->Position().distance_to(hands[h].c);
                if (distance<best) { best=distance; nearest=object; }
            }
        if (nearest) consider(nearest,best);
    }
    std::sort(found,found+count,[](const Candidate& a, const Candidate& b) { return a.distance<b.distance; });
    for (int n=0;n<count;++n)
    {
        CWeapon& weapon=*found[n].weapon;
        const shared_str& hud=weapon.HudSection();
        if (!pSettings->section_exist(hud) || !pSettings->line_exist(hud,"item_visual")) continue;
        VrPrewarmVisual(pSettings->r_string(hud,"item_visual"),weapon.cNameSect().c_str());
    }
}
void VrPrewarmRelease()
{
    while (!g_vr_prewarmed.empty()) VrPrewarmDrop(g_vr_prewarmed.size()-1);
}
void UpdateVrWeapons(CActor& actor)
{
    if (&actor != Level().CurrentControlEntity() || !IsGameTypeSingle()) return;
    {
        // A long frame (real time, not the clamped frame delta), for the
        // [VR work] lines logged before it.
        static u32 last_frame=0, last_time=0;
        const u32 now=Device.dwTimeContinual;
        if (vr::ContactDebug()>0 && last_time && Device.dwFrame==last_frame+1 &&
            now-last_time>=(VrWorkDetailed() ? 20u : 100u))
            Msg("* [VR frame] frame %u took %u ms",last_frame,now-last_time);
        last_frame=Device.dwFrame; last_time=now;
    }
    // Inventory weapons can stop receiving UpdateCL while holstered. Their
    // temporary physics props still need their lifetime serviced every frame.
    for (auto* item : actor.inventory().m_all)
        if (auto* weapon=item->cast_weapon()) weapon->VrUpdateProps();
    // Every weapon's interaction, the backpack's included (they get no
    // VrInteract): a hand's claim lapses once it is no longer on its way.
    VrUpdateHandClaims(actor);
    auto& pickup=actor.m_vr_pickup;
    if (!g_vr_owns_present)
    {
        ReleaseVrHandBodies(actor);
        if (pickup.use_pressed) VrUseKey(actor,false);
        if (pickup.native_reload_down) actor.IR_OnKeyboardRelease(kWPN_RELOAD);
        for (int h=0; h<2; ++h) pickup.use_trigger_down[h]=pickup.weapon_grip_armed[h]=false;
        pickup.pocket_held[0]=pickup.pocket_held[1]=-1;
        pickup.use_pressed=false;
        pickup.native_reload_down=false;
        pickup.native_reload_armed=false;
        pickup.pick_id=u16(-1);
        pickup.climb_hand=-1;
        actor.m_vr_climb_delta=0.f;
        // Off the headset a carried floor magazine falls; the rest still expire.
        const bool none[2]{};
        const Fmatrix idle[2]{Fidentity,Fidentity};
        UpdateVrFloorMagazines(actor,false,none,idle,none,none,nullptr);
        for (auto* item : actor.inventory().m_all)
            if (auto* weapon=item->cast_weapon()) weapon->VrInteract(actor);
        VrPrewarmRelease();
        return;
    }
    VrPdaUpdate(actor);
    const bool controls_valid=VrControlsFree(actor);
    // Either hand may draw, hold and fire; the other supports. Arrays below
    // are indexed by physical hand (0 left, 1 right).
    Fmatrix hands[2];
    bool tracked[2], grip[2], grip_edge[2];
    // The body's placement, to measure hand motion in its frame (the
    // player's real hand motion; see VrHandMotion), and whether it turned
    // this frame (a snap turn swings every world-space hand pose about the
    // head: bodies following the hands are put back rather than flung).
    Fmatrix body;
    const bool body_valid=controls_valid && vr::BodyTransform(body);
    pickup.body_turned=body_valid && pickup.body_k_valid && pickup.body_k.dotproduct(body.k)<_cos(deg2rad(2.f));
    pickup.body_k_valid=body_valid;
    if (body_valid) pickup.body_k=body.k;
    const Fvector carried=VrCarriedVelocity(actor);
    for (int h=0; h<2; ++h)
    {
        tracked[h]=controls_valid && vr::HandTransform(h,false,hands[h]);
        grip[h]=tracked[h] && vr::GripHeld(vr::HandSqueeze(h),pickup.grip_held[h]);
        grip_edge[h]=grip[h] && !pickup.grip_held[h];
        pickup.grip_held[h]=grip[h];
        Fmatrix local;
        const bool local_valid=tracked[h] && body_valid && vr::HandInBody(h,false,local);
        // Smoothed hand velocity, used when a released weapon is thrown.
        if (local_valid && pickup.hand_valid[h] && Device.fTimeDelta>0.001f)
        {
            Fvector velocity, angular;
            VrHandMotion(pickup.hand_previous[h],local,body,carried,Device.fTimeDelta,velocity,angular);
            if (velocity.magnitude()>15.f) velocity.normalize_safe().mul(15.f);
            if (angular.magnitude()>30.f) angular.normalize_safe().mul(30.f);
            pickup.hand_velocity[h].lerp(pickup.hand_velocity[h],velocity,0.5f);
            pickup.hand_angular[h].lerp(pickup.hand_angular[h],angular,0.5f);
        }
        else if (!local_valid) { pickup.hand_velocity[h].set(0,0,0); pickup.hand_angular[h].set(0,0,0); }
        pickup.hand_valid[h]=local_valid;
        if (local_valid) pickup.hand_previous[h]=local;
    }
    UpdateVrWield(actor);
    // A thrown weapon gets the hand's motion once its world physics is live.
    for (auto& entry:pickup.thrown)
    {
        if (entry.id==u16(-1)) continue;
        auto* thrown=smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(entry.id));
        if (!thrown || int(Device.dwTimeGlobal-entry.expires)>0) entry.id=u16(-1);
        else if (!thrown->H_Parent() && thrown->PPhysicsShell() && thrown->PPhysicsShell()->isActive())
        {
            if (entry.place)
            {
                thrown->PPhysicsShell()->SetGlTransformDynamic(entry.matrix);
                thrown->XFORM().set(entry.matrix);
                thrown->spatial_move();
                if (auto* bolt=smart_cast<CBolt*>(thrown)) bolt->VrPrepareThrown(entry.limited);
            }
            thrown->PPhysicsShell()->set_LinearVel(entry.velocity);
            thrown->PPhysicsShell()->set_AngularVel(entry.angular);
            entry.id=u16(-1);
            entry.place=false;
        }
    }
    // A bolt slot emptied by a throw takes the next bolt carried (a new one
    // for unlimited bolts, once it has arrived).
    if (pickup.bolt_refill_until)
    {
        if (actor.inventory().ItemFromSlot(BOLT_SLOT) || int(Device.dwTimeGlobal-pickup.bolt_refill_until)>0)
            pickup.bolt_refill_until=0;
        else
            for (PIItem item:actor.inventory().m_ruck)
                if (smart_cast<CBolt*>(item))
                {
                    NET_Packet P;
                    item->object().u_EventGen(P,GEG_PLAYER_ITEM2SLOT,actor.ID());
                    P.w_u16(item->object().ID());
                    P.w_u16(BOLT_SLOT);
                    P.w_u8(1); // not activated
                    item->object().u_EventSend(P);
                    pickup.bolt_refill_until=0;
                    break;
                }
    }
    // Holster draw: each empty hand closing on a holster takes its weapon,
    // which then belongs to that hand. One held grip claims one weapon.
    bool drawn=false;
    VrBodySlotHover(actor,tracked,hands);
    for (int h=1; h>=0; --h)
    {
        // Activation is deferred: one draw per frame, or both hands could
        // claim a holster in the same frame and one would come up empty. A
        // weapon still on its way (a queued draw, a pending wield) counts as
        // out: the other hand's draw replaced it, and it kept its claim.
        const bool coming=VrWeaponComing(actor);
        const bool active=drawn || coming || smart_cast<CWeapon*>(actor.inventory().ActiveItem())!=nullptr;
        CWeapon* closest=nullptr;
        // A squeeze that starts within a holster's reach draws; a hand
        // arriving there already squeezed does not.
        const bool fresh_squeeze=tracked[h] && grip[h] && grip_edge[h] && pickup.weapon_grip_armed[h] &&
            pickup.grabbed_id[h]==u16(-1) && !VrFloorMagazineHand(h) && pickup.climb_hand!=h;
        float best=FLT_MAX;
        if (fresh_squeeze && !active)
        {
            for (u16 slot : {INV_SLOT_2,INV_SLOT_3})
                if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot)))
                {
                    float distance=0.f;
                    if (weapon->VrHolsterGrabDistance(actor,hands[h].c,distance) && distance<best)
                    { closest=weapon; best=distance; }
                }
        }
        // Body slots (knife, grenade, detector, PDA...) where they are nearer
        // than a holster. Detector, PDA and inventory work beside a held gun.
        float body_distance=FLT_MAX;
        const int body=fresh_squeeze && !drawn ? VrBodySlotCandidate(actor,h,hands[h].c,body_distance) : -1;
        // A long gun over the off-hand shoulder shares it with the
        // inventory: an empty hand closing there takes the gun (X still
        // opens the inventory).
        if (body>=0 && body_distance<best && !(closest && body==vr::BodyInventory))
        {
            pickup.weapon_grip_armed[h]=false;
            if (VrBodySlotTake(actor,body,h)) drawn=true;
            continue;
        }
        if (fresh_squeeze && coming && !drawn && vr::ContactDebug()>0)
            Msg("* [VR hands] hand %d: no draw, a weapon is already on its way to a hand",h);
        if (vr::HolsterGrab(pickup.weapon_grip_armed[h],tracked[h],grip[h],active,closest!=nullptr))
        {
            closest->VrSetWeaponHand(h);
            actor.inventory().Activate(closest->CurrSlot(),true);
            VrWorkDetail(12);
            drawn=true;
            vr::HandHaptic(h,0.2f);
            if (vr::ContactDebug()>0)
                Msg("* [VR hands] %s hand draws %s from slot %u",h ? "right" : "left",
                    closest->cNameSect().c_str(),unsigned(closest->CurrSlot()));
        }
    }
    // Quick-use items taken from the chest pockets; grenades and bolts.
    VrPocketUpdate(actor,tracked,hands,grip);
    VrMissileUpdate(actor,tracked,hands,grip);
    VrHandItemsUpdate(actor,tracked,grip);
    VrBinocularsUpdate(actor);
    if (controls_valid) VrHeadGestureUpdate(actor,tracked,hands);
    // Sample holstered weapons' first-person grips ahead of the draw, one a
    // second, so drawing does not stall on it.
    if (Device.dwFrame%60==0)
        for (u16 slot : {INV_SLOT_2,INV_SLOT_3})
            if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot)))
                if (weapon!=actor.inventory().ActiveItem() && weapon->VrPrepare()) break;
    auto* held_weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const int weapon_hand=held_weapon ? held_weapon->VrWeaponHand() : -1;
    // The support hand's secondary button, or the weapon hand's when the
    // weapon has no physical magazine release, invokes native reload,
    // including tri-state and double-barrel shotguns.
    auto& reload_down=pickup.native_reload_down;
    auto& reload_armed=pickup.native_reload_armed;
    const int support_hand=held_weapon ? 1-weapon_hand : 0;
    // The support hand in the ammunition pouch: its upper face button (Y,
    // or B left-handed) changes the rounds the pouch gives, as the desktop's
    // next-ammunition key does; the next magazine or shell taken (or the
    // next native reload) holds them. A press begun there is not the reload
    // button, even once the hand has left the pouch with it still held.
    const bool in_pouch=held_weapon && tracked[support_hand] && !VrHandHoldsItem(actor,support_hand) &&
        hands[support_hand].c.distance_to(VrPouchPoint(actor))<=vr_pouch_reach;
    const bool support_button=controls_valid && vr::HandSecondaryButton(support_hand);
    if (support_button && in_pouch && !pickup.pouch_swap_down && !reload_down)
    {
        pickup.pouch_swap_down=true;
        vr::HandHaptic(support_hand,held_weapon->VrSwapPouchAmmo() ? 0.15f : 0.04f);
    }
    if (!support_button) pickup.pouch_swap_down=false;
    // Weapons reloaded by hand (magazines, shells) have no button reload.
    // A hand at the helmet uses its button for night vision instead.
    const int device_hand=VrDetectorHand(actor);
    const auto button_free=[&](int h){ return h>=0 && h!=device_hand && !(tracked[h] && VrHandAtHead(hands[h].c)); };
    const bool reload_button=controls_valid && !(held_weapon && held_weapon->VrPhysicalReload()) &&
        ((vr::HandSecondaryButton(support_hand) && button_free(support_hand) && !pickup.pouch_swap_down) ||
        (held_weapon && vr::HandSecondaryButton(weapon_hand) && button_free(weapon_hand) &&
        !held_weapon->VrUsesMagazineButton()));
    if (reload_down && !reload_button)
    { actor.IR_OnKeyboardRelease(kWPN_RELOAD); reload_down=false; }
    if (!controls_valid) reload_armed=false;
    else if (!reload_button) reload_armed=true;
    else if (reload_armed)
    {
        reload_armed=false;
        if (held_weapon && !held_weapon->VrBlockReload())
        {
            Msg("* [VR weapon] %s: reload requested (state=%u pending=%d ammo=%d/%d)",
                held_weapon->cNameSect().c_str(),held_weapon->GetState(),
                held_weapon->IsPending(),held_weapon->GetAmmoElapsed(),
                held_weapon->GetAmmoMagSize());
            actor.IR_OnKeyboardPress(kWPN_RELOAD);
            reload_down=true;
            vr::HandHaptic(support_hand,0.10f);
        }
    }
    // Drawing and putting away a slot weapon are unseen here: it is in the
    // hand or on the body, never in the first-person animation. They are not
    // waited on (as for the knife and binoculars, VrBodySlots), so a weapon
    // taken by hand is usable at once and a hand that let one go is free.
    for (u16 slot : {INV_SLOT_2,INV_SLOT_3})
        if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot)))
        {
            const u32 state=weapon->GetState();
            if ((state==CHUDState::eShowing || state==CHUDState::eHiding) && weapon->GetNextState()==state)
            {
                weapon->StopCurrentAnimWithoutCallback();
                static_cast<CHudItem*>(weapon)->OnAnimationEnd(state);
            }
        }
    for (u16 slot=actor.inventory().FirstSlot(); slot<=actor.inventory().LastSlot(); ++slot)
    {
        auto* weapon=smart_cast<CWeapon*>(actor.inventory().ItemFromSlot(slot));
        if (!weapon) continue;
        weapon->VrReleaseInactiveHeld();
        // Both slot weapons need the grip check even when the other is active.
        // Each interaction owns its state; only the equipped one can fire.
        CTimer interact_timer; interact_timer.Start();
        weapon->VrInteract(actor);
        VrNoteWork("interaction update",weapon->cNameSect().c_str(),interact_timer.GetElapsed_sec()*1000.f);
    }
    // A weapon drawn without a grip (inventory, keyboard, a loaded save, a
    // pickup) stays in its hand; letting go of a held grip releases it (see
    // the interaction).
    auto* held_weapon_now=smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    held_weapon=held_weapon_now;
    const int held_hand=held_weapon ? held_weapon->VrWeaponHand() : 1;
    // A hand is free unless it holds the weapon, or supports it while the
    // weapon needs it (magazine, slide, fore-end, shells). A weapon on its
    // way out (let go at the holster) no longer holds its hand.
    const bool holding=held_weapon && !VrActiveItemLeaving(actor);
    bool available[2], reach[2], grab[2], at_weapon[2], trigger[2];
    for (int h=0; h<2; ++h)
    {
        available[h]=tracked[h] && !actor.IsTalking() &&
            !(holding && (h==held_hand || held_weapon->VrLeftHandBusy()));
        // A hand at the weapon (on its slide, fore-end, magazine) works the
        // weapon: it starts no grab and no use, though it keeps what it carries.
        // A hand holding a pocket item uses its trigger for that item; one
        // holding a grenade or bolt does not point or grab.
        // A hand at the equipment takes from it, and does not grab or use
        // what it points at (unless it is already carrying an item there).
        // Close to the weapon hand it still grabs an item it is on (one on
        // the ground beside the weapon), but points at and uses nothing.
        // `refused` says why not (vr_contact_debug).
        const char* refused=
            !available[h] ? (actor.IsTalking() ? "talking" : h==held_hand ? "it holds the weapon" : "it supports the weapon") :
            VrPocketHeld(actor,h) ? "it holds a pocket item" : h==VrMissileHand(actor) ? "it holds a grenade" :
            h==device_hand ? "it holds the detector or flashlight" : h==pickup.slot_hold_hand ? "it holds a slot item" :
            h==pickup.bolt_hand ? "it holds a bolt" :
            pickup.grabbed_id[h]==u16(-1) && VrHandAtEquipment(actor,hands[h].c,h) ?
                "it is at the equipment (body slot, holster or pouch)" : nullptr;
        grab[h]=!refused;
        // At the weapon: on it or along its bore (fore-end, magazine, slide),
        // where a squeeze is for the weapon. Not anywhere within 35 cm of the
        // gun hand: that blocked the other hand's grabs beside a pistol.
        at_weapon[h]=holding && tracked[held_hand];
        if (at_weapon[h] && h!=held_hand)
        {
            Fmatrix pose;
            if (held_weapon->VrAimTransform(pose))
                at_weapon[h]=vr::AtWeapon(Pick(hands[h].c),Pick(hands[held_hand].c),Pick(pose.k));
            else at_weapon[h]=hands[h].c.distance_to(hands[held_hand].c)<0.35f;
        }
        reach[h]=grab[h] && !at_weapon[h];
        trigger[h]=vr::TriggerPulled(vr::HandTrigger(h));
        // Why a fresh squeeze of a hand is not a grab.
        if (vr::ContactDebug()>0 && grip_edge[h] && tracked[h] && refused && pickup.grabbed_id[h]==u16(-1))
            Msg("* [VR pickup] hand %d: squeeze is not a grab: %s",h,refused);
    }
    // Magazines in the world first: a hand carrying one does nothing else.
    {
        const bool down[2]={grip[0] || trigger[0],grip[1] || trigger[1]};
        UpdateVrFloorMagazines(actor,controls_valid,tracked,hands,down,grab,held_weapon);
        for (int h=0; h<2; ++h)
            if (VrFloorMagazineHand(h)) available[h]=grab[h]=reach[h]=false;
    }
    // Ladders: a free hand closing on one climbs it.
    UpdateVrClimb(actor,available,grip,grip_edge);
    // Physical hands push loose physics objects.
    UpdateVrHandBodies(actor,tracked,hands);
    if (pickup.climb_hand>=0) available[pickup.climb_hand]=false;
    // The desktop F action: a free hand's trigger, on what that hand points
    // at. The last hand that pulled its trigger points; otherwise the right.
    for (int h=0; h<2; ++h)
        if (reach[h] && trigger[h] && !pickup.use_trigger_down[h]) pickup.use_hand=h;
    if (pickup.use_hand<0 || !reach[pickup.use_hand])
        pickup.use_hand=reach[1] ? 1 : (reach[0] ? 0 : -1);
    const int pointer=pickup.use_hand;
    collide::rq_result hit;
    if (pointer>=0 && pickup.grabbed_id[pointer]==u16(-1) && VrPointerPick(actor,pointer,hit))
    { pickup.pick_id=hit.O->ID(); pickup.pick_range=hit.range; pickup.pick_element=hit.element; }
    else pickup.pick_id=u16(-1);
    // The name shown is the item a squeeze takes: when the pointer's own ray
    // misses an item the grab would still take (beside the ray, under the
    // hand), that item is named instead.
    if (pointer>=0 && pickup.grabbed_id[pointer]==u16(-1) && !at_weapon[pointer])
    {
        auto* pointed=smart_cast<CPhysicsShellHolder*>(pickup.pick_id!=u16(-1) ? Level().Objects.net_Find(pickup.pick_id) : nullptr);
        bool touching=false;
        const char* why="";
        if (!VrTakeable(actor,pointer,pointed))
            if (auto* target=VrGrabTarget(actor,pointer,false,touching,why,true))
            {
                Fmatrix aim;
                pickup.pick_id=target->ID();
                pickup.pick_range=vr::HandTransform(pointer,true,aim) ? aim.c.distance_to(target->Position()) : 0.f;
                pickup.pick_element=0;
            }
    }
    const bool can_use=pointer>=0 && pickup.grabbed_id[pointer]==u16(-1);
    const bool use_trigger=can_use && trigger[pointer];
    // Track the physical trigger even while armed to avoid a spurious use
    // event when the weapon is holstered with the trigger still held.
    if (pickup.use_pressed && !use_trigger)
    { VrUseKey(actor,false); pickup.use_pressed=false; }
    const bool pending_use=use_trigger && !pickup.use_trigger_down[pointer];
    for (int h=0; h<2; ++h) pickup.use_trigger_down[h]=trigger[h];
    const u16 wield_before=pickup.wield_id;
    for (int h=0; h<2; ++h) UpdateVrPickup(actor,h,available[h],grip[h],grab[h] && trigger[h],grab[h],at_weapon[h]);
    // A trigger that just took a weapon into its hand (a wield sets no
    // grabbed item) is not also the use key: it pressed F on what lay behind.
    const bool took_weapon=pointer>=0 && pickup.wield_id!=u16(-1) && pickup.wield_id!=wield_before &&
        pickup.wield_hand==pointer;
    if (pickup.wield_id!=u16(-1) && pickup.wield_id!=wield_before) VrWorkDetail(12);
    // A pulled trigger grabs a pointed physical item first. When nothing can
    // be grabbed it retains the ordinary F/use action (doors, people, bodies,
    // stashes). A loose item is never sent there: F put it straight in the
    // backpack instead of in the hand, to be stowed over the shoulder.
    CObject* use_target=pickup.pick_id!=u16(-1) ? Level().Objects.net_Find(pickup.pick_id) : nullptr;
    auto* loose=smart_cast<CPhysicsShellHolder*>(use_target);
    const bool loose_item=loose && loose->cast_inventory_item() && !loose->H_Parent() &&
        loose->PPhysicsShell() && loose->PPhysicsShell()->isActive();
    if (pending_use && pickup.grabbed_id[pointer]==u16(-1) && !loose_item && !took_weapon)
    {
        if (vr::ContactDebug()>0)
        {
            CObject* target=use_target;
            Msg("* [VR use] hand %d: F on %s at %.2f m",pointer,target ? target->cName().c_str() : "nothing",
                target ? pickup.pick_range : 0.f);
        }
        VrUseKey(actor,true); pickup.use_pressed=true;
    }
    // HUD models of weapons a hand is reaching for, loaded ahead.
    VrPrewarmNear(actor,controls_valid,tracked,hands);
}
