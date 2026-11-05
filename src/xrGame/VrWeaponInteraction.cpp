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
xr_list<VrFloorMagazine> g_floor_magazines;
void RetireFloorMagazine(VrFloorMagazine& magazine)
{
    magazine.cartridges.clear();
    if (magazine.shell) destroy_physics_shell(magazine.shell);
    magazine.shell=nullptr;
    ReleaseModel(magazine.visual);
}
VrFloorMagazine* FloorMagazineIn(int hand)
{
    for (auto& magazine:g_floor_magazines)
        if (magazine.hand==hand) return &magazine;
    return nullptr;
}
// Rounds that fit go into the owner's part-filled boxes of their type; the
// rest are left in `left` per type.
void TopUpBoxes(CInventoryOwner* owner, const xr_vector<CCartridge>& rounds,
    xr_vector<std::pair<shared_str,u32>>& left)
{
    for (const CCartridge& cartridge:rounds)
    {
        bool placed=false;
        if (owner)
            for (auto* item:owner->inventory().m_all)
                if (auto* box=smart_cast<CWeaponAmmo*>(item);
                    box && box->cNameSect()==cartridge.m_ammoSect && box->m_boxCurr<box->m_boxSize)
                { ++box->m_boxCurr; placed=true; break; }
        if (placed) continue;
        auto it=std::find_if(left.begin(),left.end(),[&](const auto& entry) { return entry.first==cartridge.m_ammoSect; });
        if (it!=left.end()) ++it->second;
        else left.push_back({cartridge.m_ammoSect,1});
    }
}
// A floor magazine's rounds into the actor's backpack: no weapon is needed.
// The rest are new boxes in the inventory, split by box size, saved as
// CWeapon::SpawnAmmo's boxes are (no stored name: the game names each box
// by its section either way).
void ReturnRoundsToActor(CActor& actor, const xr_vector<CCartridge>& rounds)
{
    xr_vector<std::pair<shared_str,u32>> left;
    TopUpBoxes(&actor,rounds,left);
    for (const auto& entry:left)
    {
        CSE_Abstract* entity=Level().spawn_item(entry.first.c_str(),actor.Position(),
            actor.ai_location().level_vertex_id(),actor.ID(),true);
        auto* ammo=smart_cast<CSE_ALifeItemAmmo*>(entity);
        if (!ammo) { F_entity_Destroy(entity); continue; }
        entity->set_name_replace("");
        // A pack's ammunition without box_size: the box the server entity
        // made (CSE_ALifeItemAmmo reads it the same way), else one box.
        ammo->m_boxSize=(u16)READ_IF_EXISTS(pSettings,r_s32,entry.first,"box_size",ammo->m_boxSize);
        for (u32 count=entry.second;count;)
        {
            const u32 box=std::min<u32>(count,ammo->m_boxSize ? ammo->m_boxSize : count);
            ammo->a_elapsed=u16(box);
            NET_Packet P;
            entity->Spawn_Write(P,TRUE);
            Level().Send(P,net_flags(TRUE));
            count-=box;
        }
        F_entity_Destroy(entity);
    }
}
}

bool VrHandgun(CWeapon& weapon)
{
    // Whether the weapon is held in one hand: its class, not its
    // hand_dependence, which many rifles (the AK-74M among them) set to 1.
    // Revolvers share their base with shotguns and launchers, not pistols.
    // A pistol class with a long gun's kind (GAMMA's PKM and PKP are WP_PM,
    // kind w_rifle) is a long gun. Any other kind keeps the class: GAMMA's
    // pistol-class Kedr, PPSh and SR-2M are w_pistol, a machine pistol may be
    // w_smg.
    LPCSTR kind=READ_IF_EXISTS(pSettings,r_string,weapon.cNameSect(),"kind","");
    const bool long_kind=!xr_strcmp(kind,"w_rifle") || !xr_strcmp(kind,"w_sniper") || !xr_strcmp(kind,"w_shotgun");
    return !long_kind && (smart_cast<CWeaponPistol*>(&weapon)!=nullptr || smart_cast<CWeaponRevolver*>(&weapon)!=nullptr);
}

CVrWeaponInteraction::CVrWeaponInteraction(CWeapon* weapon) : m_weapon(*weapon)
{
    m_supported=LoadProfile();
    m_hand=vr::DominantHand();
    // A derived profile is measured once the weapon is the player's (see
    // Interact), never for every weapon in the level.
}
CVrWeaponInteraction::~CVrWeaponInteraction() { Destroy(); }

bool CVrWeaponInteraction::Mode() const
{
    // Asked many times a frame: the local actor by pointer, without a cast.
    const CObject* parent=m_weapon.H_Parent();
    return m_supported && g_vr_owns_present && IsGameTypeSingle() && g_actor && parent &&
        parent==static_cast<const CObject*>(g_actor) && parent==Level().CurrentControlEntity() &&
        m_weapon.VrPhysicalMechanismAvailable();
}

Fvector CVrWeaponInteraction::GripLocal() const
{
    // Where the controller sits on the weapon: the palm of the first-person
    // animation's weapon hand when there is one (mirrored for a left hand),
    // so the drawn hand lies on the real hand; otherwise the measured grip.
    // The hands are sampled by RefreshAuthoredHands, never here.
    if (vr::AnimatedGrip() && !m_cloud.empty())
    {
        if (m_authored[1].valid && m_palm_trusted)
        {
            // Mirrored about the bore, which need not be the model's x=0.
            Fvector palm=m_authored[1].palm;
            const float centre=m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f;
            if (!m_hand) palm.x=2.f*centre-palm.x;
            return palm;
        }
    }
    return m_profile.grip_offset;
}

bool CVrWeaponInteraction::SectionCentre(const Fvector& point, float& centre, bool& inside) const
{
    // Mesh points in a 2.4 cm slice along the bore and 3 cm across, below the
    // bore line (not the barrels or sights above it).
    const float top=m_profile.muzzle.square_magnitude()>EPS_S ? m_profile.muzzle.y-0.005f : point.y+0.08f;
    float above=flt_max, below=-flt_max;
    for (const Fvector& p:m_cloud)
    {
        if (_abs(p.z-point.z)>0.012f || _abs(p.x-point.x)>0.03f || p.y>top || _abs(p.y-point.y)>0.08f) continue;
        if (p.y>point.y) above=std::min(above,p.y); else below=std::max(below,p.y);
    }
    if (above==flt_max) { inside=false; return false; }
    inside=below>-flt_max && point.y-below<0.05f;
    if (inside) { centre=0.5f*(above+below); return true; }
    // Under the mesh: the solid just above, up to 6 cm thick.
    float high=above;
    for (const Fvector& p:m_cloud)
        if (_abs(p.z-point.z)<=0.012f && _abs(p.x-point.x)<=0.03f && p.y<=top && p.y>=above && p.y<=above+0.06f)
            high=std::max(high,p.y);
    centre=0.5f*(above+high);
    return true;
}
Fvector CVrWeaponInteraction::SupportSlide() const
{
    Fvector none; none.set(0.f,0.f,0.f);
    if (!m_supporting || m_support_cup || !m_authored[0].valid || RearSupport() || m_cloud.empty()) return none;
    if (m_slide_along==m_support_along) return m_slide;
    m_slide_along=m_support_along;
    m_slide=none;
    // Where the hand closed: along the line from the weapon hand's grip to
    // the muzzle, in weapon space.
    const Fvector grip=GripLocal();
    Fvector dir; dir.sub(m_profile.muzzle,grip);
    if (dir.square_magnitude()<0.15f*0.15f) return m_slide;
    dir.normalize();
    const Fvector& palm=m_authored[0].palm;
    const float target=grip.z+dir.z*m_support_along;
    float dz=target-palm.z;
    if (_abs(dz)<0.03f) return m_slide;
    // Not off the end of the weapon: back toward the animation's place until
    // there is mesh to hold.
    float base=0.f; bool inside=false;
    const bool based=SectionCentre(palm,base,inside);
    for (int step=0;step<40 && _abs(dz)>=0.03f;++step)
    {
        Fvector at=palm; at.z+=dz;
        float centre=0.f;
        if (SectionCentre(at,centre,inside))
        {
            m_slide.set(0.f,based && _abs(centre-base)<0.06f ? centre-base : 0.f,dz);
            return m_slide;
        }
        dz-=dz>0.f ? 0.01f : -0.01f;
    }
    return m_slide;
}
bool CVrWeaponInteraction::ForendPoint(Fvector& point) const
{
    if (!m_profile.has_forend) return false;
    // The animation's left hand holds the weapon where its idle does; only
    // without it (a stock-wrist gun whose animated hand is far from the
    // measured grip, or animated grips off) is the mesh's fore-end used.
    if (vr::AnimatedGrip() && !m_cloud.empty() && m_palm_trusted && m_authored[0].valid) return false;
    point=m_profile.forend_point;
    return true;
}
bool CVrWeaponInteraction::Transform(Fmatrix& world) const
{
    if (!Mode() || m_holstered) return false;
    return HeldPose(world);
}
bool CVrWeaponInteraction::CarriedPose(Fmatrix& world) const
{
    Fmatrix hand;
    if (!vr::HandTransform(HandOf(1),false,hand)) return false;
    world.mul_43(hand,m_carry);
    return true;
}
// Where the weapon hand holds it, whether or not it is drawn yet (a weapon
// taken by hand is shown there while its activation is on its way).
bool CVrWeaponInteraction::HeldPose(Fmatrix& world) const
{
    // Passed to the other hand: it keeps the weapon where it took it.
    if (m_carried) return CarriedPose(world);
    Fmatrix grip,aim;
    if (!vr::HandTransform(HandOf(1),false,grip) || !vr::HandTransform(HandOf(1),true,aim)) return false;
    world=aim;
    world.c.set(0,0,0);
    if (m_break_hand)
    {
        // The support hand works the barrels; it no longer steers the gun.
        Fmatrix rotated; rotated.mul_43(world,m_break_receiver);
        world=rotated;
        world.c.set(0,0,0);
    }
    else if (m_supporting && !m_support_cup && !BarrelsOpen())
    {
        // Fore-end hold: the weapon hand stays put and the weapon's own
        // grip-to-fore-end line passes through the support hand. A cup grip
        // below the weapon hand leaves the aim to the weapon hand.
        Fmatrix left;
        Fvector have;
        Fvector forend;
        have.sub(ForendPoint(forend) ? forend : m_profile.support_offset,GripLocal());
        if (RearSupport()) have.sub(m_authored[0].palm,GripLocal());
        if (have.square_magnitude()<0.01f) have.set(0.f,0.f,1.f);
        if (vr::HandTransform(HandOf(0),false,left))
        {
            Fvector want; want.sub(left.c,grip.c);
            world.transform_dir(have);
            if (want.square_magnitude()>0.01f && have.square_magnitude()>EPS_S)
            {
                want.normalize(); have.normalize();
                Fvector axis; axis.crossproduct(have,want);
                const float sine=axis.magnitude();
                if (sine>0.0001f)
                {
                    axis.div(sine);
                    Fmatrix rotation, rotated;
                    rotation.rotation(axis,atan2f(sine,have.dotproduct(want)));
                    rotated.mul_43(rotation,world);
                    world=rotated;
                    world.c.set(0,0,0);
                }
            }
        }
    }
    // Recoil turns the weapon about the grip, muzzle up and aside, and
    // pushes it back along the bore.
    DecayRecoil();
    if (m_recoil_pitch>1e-5f || _abs(m_recoil_yaw)>1e-5f)
    {
        const Fvector i=world.i, j=world.j, k=world.k;
        world.k.set(k).mul(_cos(m_recoil_pitch)).mad(j,_sin(m_recoil_pitch));
        world.j.set(j).mul(_cos(m_recoil_pitch)).mad(k,-_sin(m_recoil_pitch));
        world.k.mul(_cos(m_recoil_yaw)).mad(i,_sin(m_recoil_yaw)).normalize();
        world.i.crossproduct(world.j,world.k).normalize();
        world.j.crossproduct(world.k,world.i).normalize();
    }
    Fvector offset=GripLocal();
    world.transform_dir(offset);
    world.c.sub(grip.c,offset);
    world.c.mad(world.k,-m_recoil_back);
    return true;
}

void CVrWeaponInteraction::DecayRecoil() const
{
    const u32 now=Device.dwTimeGlobal;
    if (now==m_recoil_time) return;
    const float dt=_min(float(now-m_recoil_time)*0.001f,0.25f);
    m_recoil_time=now;
    const float keep=expf(-m_recoil_settle*dt);
    m_recoil_pitch*=keep; m_recoil_yaw*=keep; m_recoil_back*=keep;
}

void CVrWeaponInteraction::OnShot()
{
    // 120% of the setting is the base kick (the setting 1 kicks as 1.5 did).
    const float scale=1.2f*vr::Recoil();
    if (!Mode() || scale<=0.f) return;
    DecayRecoil();
    // The kick follows the cartridge's momentum (the square root of its
    // energy, against 5.45x39) and the weapon's weight (against the AK74's
    // 3.3 kg): an AK74 kicks 1.2 degrees, a PM about 1.5, an SVD 1.8, a
    // 12-gauge 2.1. Without the support hand on it a weapon kicks harder, the
    // more so the longer it is (the lever on the wrist): a pistol 20% more,
    // a 0.6 m SMG 2 times its supported kick, a 0.9 m rifle 2.5 times, a
    // 1.2 m one 3 times;
    // a long gun also throws further aside, climbs further and settles more
    // slowly in one hand. A suppressor takes 15% off.
    const char* ammo=m_weapon.m_ammoTypes.empty() ? "" :
        m_weapon.m_ammoTypes[std::min<size_t>(m_weapon.m_ammoType,m_weapon.m_ammoTypes.size()-1)].c_str();
    const float calibre=clampr(_sqrt(CartridgeEnergy(ammo)/1350.f),0.4f,3.f);
    const float mass=clampr(_sqrt(3.3f/_max(m_weapon.Weight(),0.3f)),0.55f,2.f);
    const bool two_handed=!Handgun();
    float length=0.f;
    if (IRenderVisual* visual=m_weapon.Visual())
    {
        Fvector size; visual->getVisData().box.getsize(size);
        length=_max(size.x,_max(size.y,size.z));
    }
    const float one_hand=two_handed ? 0.67f*(2.f+clampr((length-0.6f)/0.6f,0.f,1.f)) : 1.2f;
    // A two-handed weapon held in both hands kicks as before the stronger
    // base (two thirds of it); held in one hand it takes the full base.
    const float hands=m_supporting ? (two_handed ? 0.67f : 0.87f) : (two_handed ? one_hand : 0.87f*one_hand);
    const float kick=calibre*mass*hands*(m_weapon.IsSilencerAttached() ? 0.85f : 1.f);
    const float up=scale*deg2rad(1.5f)*kick;
    if (vr::ContactDebug()>0)
        Msg("* [VR recoil] %s: %s, %s, length %.2f m, hands x%.2f, kick %.2f, climb %.1f deg",
            m_profile.section.c_str(),two_handed ? "two-handed" : "one-handed",
            m_supporting ? "supported" : "one hand",length,hands,kick,rad2deg(up));
    const bool unsteady=!m_supporting && two_handed;
    const float aside=scale*deg2rad(unsteady ? 0.7f : 0.4f)*kick*::Random.randF(-1.f,1.f);
    m_recoil_settle=unsteady ? 5.f : m_supporting ? 14.f : 12.f;
    m_recoil_pitch=_min(m_recoil_pitch+up,deg2rad(unsteady ? 60.f : 20.f)*scale);
    const float wander=deg2rad(unsteady ? 20.f : 6.f)*scale;
    m_recoil_yaw=clampr(m_recoil_yaw+aside,-wander,wander);
    m_recoil_back=_min(m_recoil_back+0.006f*kick*scale,(unsteady ? 0.12f : 0.06f)*scale);
    vr::HandHaptic(HandOf(1),clampr(0.15f+0.25f*kick,0.f,1.f));
    if (m_supporting) vr::HandHaptic(HandOf(0),clampr(0.1f+0.15f*kick,0.f,0.7f));
}

bool CVrWeaponInteraction::GenericTransform(Fmatrix& world) const
{
    if (Mode() || !g_vr_owns_present || !IsGameTypeSingle() || !vr::GameplayActive()) return false;
    auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
    if (!actor || actor!=Level().CurrentControlEntity() ||
        actor->inventory().ActiveItem()!=&m_weapon ||
        actor->cam_Active()!=actor->cam_FirstEye()) return false;
    if (m_carried) return CarriedPose(world);
    Fmatrix grip;
    if (!vr::HandTransform(HandOf(1),false,grip) || !vr::HandTransform(HandOf(1),true,world)) return false;
    Fmatrix hand, finger;
    // NPC weapon offsets are authored for the right hand bone; a left-held
    // world model uses the controller placement below instead.
    if (HandOf(1)==1 && actor->m_legs_controller.vr_weapon_hand(1,hand,finger))
    {
        // Hold the world model as an NPC does: frame the tracked right hand
        // bone like CWeapon::UpdateXForm, then apply the weapon's own
        // position/orientation offset so its grip sits in the palm. One-hand
        // weapons point along hand-to-finger; two-hand weapons, whose offset
        // assumes the left hand on the fore-end, point along the aim ray.
        Fvector D;
        Fmatrix left;
        // Aim through the support controller itself: the drawn support hand
        // snaps onto the barrel line, so it cannot steer the weapon.
        if (m_supporting && !m_support_cup && vr::HandTransform(HandOf(0),false,left))
            D.sub(left.c,hand.c);
        else if (m_weapon.HandDependence()==hd1Hand) D.sub(finger.c,hand.c);
        else D=world.k;
        if (D.square_magnitude()>EPS_S)
        {
            D.normalize();
            Fvector R; R.crossproduct(hand.j,D);
            if (R.square_magnitude()>EPS_S)
            {
                R.normalize();
                Fvector N; N.crossproduct(D,R); N.normalize_safe();
                Fmatrix attach; attach.identity();
                attach.i=R; attach.j=N; attach.k=D; attach.c=hand.c;
                world.mul_43(attach,m_weapon.m_Offset);
                return true;
            }
        }
    }
    // No outfit arms: approximate the grip from the controller.
    Fvector offset; offset.set(0.f,-0.025f,0.08f);
    world.transform_dir(offset);
    world.c.add(grip.c,offset);
    return true;
}
const Fvector& CVrWeaponInteraction::MuzzleLocal() const
{
    return Mode() ? m_profile.muzzle : m_weapon.vLoadedFirePoint;
}
bool CVrWeaponInteraction::Aiming() const
{
    Fmatrix head, world;
    if (!vr::HeadTransform(head) || !AimTransform(world)) return false;
    const Fvector muzzle=Point(world,MuzzleLocal());
    Fvector to; to.sub(head.c,muzzle);
    // Behind the muzzle (a pistol at arm's length to a rifle's cheek weld),
    // within a sight's height of the bore, and looking along it.
    const float along=to.dotproduct(world.k);
    if (along>-0.15f || along<-1.2f) return false;
    Fvector across; across.mad(to,world.k,-along);
    return across.magnitude()<0.11f && head.k.dotproduct(world.k)>0.9f;
}

bool CVrWeaponInteraction::HandFrame(Fmatrix& frame) const
{
    Fmatrix world, grip;
    if (m_carried || !vr::HandTransform(HandOf(1),false,grip) || !Transform(world)) return false;
    // Pistol grips rake back from the bore; the hand wraps the grip axis.
    const float angle=deg2rad(m_profile.hand_grip_angle);
    frame.identity();
    frame.j.set(world.j).mul(_cos(angle)).mad(world.k,_sin(angle));
    frame.k.set(world.k).mul(_cos(angle)).mad(world.j,-_sin(angle));
    frame.i.crossproduct(frame.j,frame.k);
    // A measured grip centre keeps the palm on the mesh's grip regardless of
    // where the controller point (grip_offset) was calibrated.
    frame.c=m_profile.has_hand_grip_center ? Point(world,m_profile.hand_grip_center) : grip.c;
    return true;
}

bool CVrWeaponInteraction::TriggerPoint(Fvector& world) const
{
    Fmatrix weapon;
    if (!m_profile.has_trigger_point || !Transform(weapon)) return false;
    world=Point(weapon,m_profile.trigger_point);
    return true;
}
void CVrWeaponInteraction::PullTrigger()
{
    // As the desktop fire key: in safe mode the first pull only leaves it.
    if (auto* actor=smart_cast<CActor*>(m_weapon.H_Parent()); actor && actor->is_safemode())
    {
        actor->set_safemode(false);
        return;
    }
    // The action still cycling from the last shot: fire when it is in
    // battery instead of clicking on an empty chamber.
    if (m_profile.chamber && m_mechanism.cycling) { m_fire_queued=Device.dwTimeGlobal; return; }
    m_weapon.FireStart();
}

void CVrWeaponInteraction::SwapHands()
{
    // The weapon hand and the support hand trade roles. Velocities are
    // tracked per role, so they start afresh.
    m_hand=1-m_hand;
    m_have_hand[0]=m_have_hand[1]=false;
    m_hand_epoch=0;
    vr::HandHaptic(m_hand,0.15f);
}
bool CVrWeaponInteraction::HandOver()
{
    // The weapon hand let go while the other hand holds the weapon: that hand
    // now carries it where it is, and the free hand can work a bolt or slide
    // and take the grip again (TakeBack).
    Fmatrix world, support;
    if (!AimTransform(world) || !vr::HandTransform(HandOf(0),false,support)) return false;
    Fmatrix inverse; inverse.invert(support);
    m_carry.mul_43(inverse,world);
    // The hand keeps the hold it was drawn with: the weapon stays where it
    // is against the controller, so the drawn hand does not move.
    m_carry_authored=m_supporting && !m_support_cup && !BarrelsOpen() && vr::AnimatedGrip() && m_hand==1 &&
        m_authored[0].valid && m_palm_trusted && !m_hands_local.empty();
    m_carry_slide=m_carry_authored ? SupportSlide() : Fvector().set(0.f,0.f,0.f);
    m_carried=true;
    m_supporting=m_support_cup=m_break_hand=false;
    SwapHands();
    return true;
}

void CVrWeaponInteraction::TakeBack()
{
    // The free hand closed on the grip again: it holds the weapon, and the
    // hand that carried it stays where it is as the support hand.
    m_carried=false;
    m_supporting=true;
    m_support_cup=m_support_barrels=false;
    SwapHands();
    Fmatrix world, weapon_hand, support;
    if (AimTransform(world) && vr::HandTransform(HandOf(1),false,weapon_hand) &&
        vr::HandTransform(HandOf(0),false,support))
    {
        const Fvector muzzle=Point(world,MuzzleLocal());
        m_support_along=SupportAlong(weapon_hand.c,muzzle,support.c);
    }
    // The hand that carried it stays where it held the fore-end.
    if (m_carry_authored)
    {
        m_slide_along=m_support_along;
        m_slide=m_carry_slide;
    }
    m_carry_authored=false;
}

bool CVrWeaponInteraction::CollisionBoxes(Fmatrix& world, const xr_vector<Fobb>*& boxes)
{
    // The measured mesh (weapon without magazine, shells and attachments),
    // 95% of its extent: the HUD model's bounds also span hands and
    // animation room. A long gun gets a receiver box and a barrel box ahead
    // of the trigger, so it does not sweep one fat box.
    if (Transform(world))
    {
        if (m_collision_boxes.empty() && !m_cloud.empty())
        {
            const float split=m_profile.has_trigger_point ? m_profile.trigger_point.z+0.08f : flt_max;
            Fbox parts[2]; parts[0].invalidate(); parts[1].invalidate();
            for (const Fvector& point:m_cloud) parts[point.z>split ? 1 : 0].modify(point);
            for (Fbox& part:parts)
            {
                if (!part.is_valid()) continue;
                Fvector size; part.getsize(size);
                if (size.x<0.005f || size.y<0.005f || size.z<0.005f) continue;
                Fobb box; box.m_rotate.identity();
                part.getcenter(box.m_translate);
                box.m_halfsize.set(size).mul(0.475f);
                m_collision_boxes.push_back(box);
            }
        }
        boxes=&m_collision_boxes;
        return !m_collision_boxes.empty();
    }
    if (!GenericTransform(world) || !m_weapon.Visual()) return false;
    Fbox bounds=m_weapon.Visual()->getVisData().box;
    Fvector size; bounds.getsize(size);
    if (size.x<EPS_S || size.y<EPS_S || size.z<EPS_S) return false;
    m_generic_box.resize(1);
    m_generic_box[0].m_rotate.identity();
    bounds.getcenter(m_generic_box[0].m_translate);
    m_generic_box[0].m_halfsize.set(size).mul(0.475f);
    boxes=&m_generic_box;
    return true;
}

bool CVrWeaponInteraction::Prepare()
{
    if (!g_player_hud || !Mode() || !EnsureVisual()) return false;
    const bool ready=m_authored_sampled && m_authored_hands==g_player_hud->section_name();
    RefreshAuthoredHands();
    return !ready;
}

bool CVrWeaponInteraction::PouchHasAmmo() const
{
    // Nothing left to draw: no rounds of the weapon's calibres in the
    // inventory and no loaded spare magazine stowed.
    if (ReserveAvailable()) return true;
    for (int index=0;index<2;++index)
        if (index!=m_mechanism.inserted && index!=m_mechanism.held && !m_mechanism.magazines[index].empty())
            return true;
    return false;
}

void CVrWeaponInteraction::PlayHandling(int sound, const Fvector& position)
{
    // Stock weapons only carry whole-reload sounds. Magazine and bolt steps
    // use clips cut from the PM (pistols) and AK-74 (everything else) reload
    // sounds (gamedata/sounds/vr); the rest short generic sounds. A pump's
    // two strokes are the Winchester 1300's own open and close pair (a
    // weapon's snd_close_weapon is often its draw or whole reload end). A weapon
    // section can name its own (snd_vr_mag_in, snd_vr_mag_out,
    // snd_vr_slide_back, snd_vr_slide_forward, snd_vr_pump_back,
    // snd_vr_pump_forward, snd_vr_break_open, snd_vr_break_close,
    // snd_vr_shell_in, snd_vr_cover_open, snd_vr_cover_close, snd_vr_belt_in).
    static LPCSTR const keys[SoundCount]={"mag_out","mag_in","slide_back","slide_forward",
        "pump_back","pump_forward","break_open","break_close","shell_in",
        "cover_open","cover_close","belt_in"};
    static LPCSTR const rifle[SoundCount]={"vr\\rifle_mag_out","vr\\rifle_mag_in",
        "vr\\rifle_bolt_back","vr\\rifle_bolt_forward",
        "weapons\\winchester1300\\w1300_open","weapons\\winchester1300\\w1300_close",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\gen_grenload",
        "weapons\\winchester1300\\w1300_open","weapons\\generic_close","vr\\rifle_mag_in"};
    // A pistol's differ only in its magazine and slide.
    static LPCSTR const pistol[SoundBeltIn+1]={"vr\\pistol_mag_out","vr\\pistol_mag_in",
        "weapons\\winchester1300\\w1300_open","weapons\\pm\\pm_close",
        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,"vr\\pistol_mag_in"};
    if (sound<0 || sound>=SoundCount) return;
    if (!m_handling_loaded[sound])
    {
        m_handling_loaded[sound]=true;
        string64 key; xr_sprintf(key,"snd_vr_%s",keys[sound]);
        const shared_str& section=m_weapon.cNameSect();
        LPCSTR fallback=PistolHandgun() && pistol[sound] ? pistol[sound] : rifle[sound];
        LPCSTR name=pSettings->line_exist(section,key) ? pSettings->r_string(section,key) : fallback;
        // Stock sounds, if the VR clips (gamedata/sounds/vr) are not installed.
        static LPCSTR const stock[SoundCount]={"weapons\\gen_grenload","weapons\\generic_close",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close",
            "weapons\\winchester1300\\w1300_open","weapons\\winchester1300\\w1300_close",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\gen_grenload",
            "weapons\\winchester1300\\w1300_open","weapons\\generic_close","weapons\\generic_close"};
        string_path path;
        if (!name || !*name || !FS.exist(path,"$game_sounds$",name,".ogg"))
        {
            Msg("! [VR weapon] %s: handling sound %s not found, using %s",section.c_str(),
                name ? name : "",stock[sound]);
            name=stock[sound];
        }
        if (FS.exist(path,"$game_sounds$",name,".ogg"))
            m_handling[sound].create(name,st_Effect,sg_SourceType);
    }
    if (m_handling[sound]._handle()) m_handling[sound].play_at_pos(m_weapon.H_Parent(),position);
}

struct CVrWeaponInteraction::SampledHands
{
    AuthoredHandRef authored[2];
    xr_vector<Fmatrix> hands_local, maghand_local;
    Fmatrix hand_in_item[2], maghand_in_root;
    Fvector maghand_basis[3]{}, maghand_palm{}, handle_hand{}, aim_eye{}, aim_dir{0.f,0.f,1.f};
    bool handle_hand_valid=false, palm_trusted=true, aim_valid=false;
    shared_str visual;
};

xr_map<xr_string,std::shared_ptr<const CVrWeaponInteraction::SampledHands>>& CVrWeaponInteraction::SampledHandsCache()
{
    static xr_map<xr_string,std::shared_ptr<const SampledHands>> cache;
    return cache;
}

void CVrWeaponInteraction::RefreshAuthoredHands()
{
    // Measured first (MeasureGeometry): the hands are checked against the
    // weapon's mesh and grip.
    if (!g_player_hud || !m_visual || m_cloud.empty()) return;
    bool sampled=false;
    if (!m_authored_sampled || m_authored_hands!=g_player_hud->section_name())
    {
        CTimer timer; timer.Start();
        // A derived weapon's copies share them (an audited profile is one of
        // three stock weapons; it samples its own).
        xr_string key;
        if (m_profile.derived)
            key=MeasuredKey().append("|").append(g_player_hud->section_name().c_str());
        const auto found=key.size() ? SampledHandsCache().find(key) : SampledHandsCache().end();
        if (found!=SampledHandsCache().end())
        {
            const SampledHands& hands=*found->second;
            m_authored_sampled=true;
            m_authored_hands=g_player_hud->section_name();
            m_authored[0]=hands.authored[0]; m_authored[1]=hands.authored[1];
            m_hands_local=hands.hands_local; m_maghand_local=hands.maghand_local;
            m_hand_in_item[0]=hands.hand_in_item[0]; m_hand_in_item[1]=hands.hand_in_item[1];
            m_maghand_in_root=hands.maghand_in_root;
            for (int i=0;i<3;++i) m_maghand_basis[i]=hands.maghand_basis[i];
            m_maghand_palm=hands.maghand_palm;
            m_handle_hand=hands.handle_hand; m_handle_hand_valid=hands.handle_hand_valid;
            m_palm_trusted=hands.palm_trusted;
            m_hands_visual_name=hands.visual;
            m_aim_eye=hands.aim_eye; m_aim_dir=hands.aim_dir; m_aim_valid=hands.aim_valid;
            Msg("* [VR weapon] %s: first-person hands %s as an earlier copy's",m_profile.section.c_str(),
                m_authored_hands.c_str());
        }
        else
        {
            SampleAuthoredHands();
            Msg("* [VR weapon] %s: first-person hands %s sampled in %.1f ms",m_profile.section.c_str(),
                m_authored_hands.c_str(),timer.GetElapsed_sec()*1000.f);
            if (key.size())
            {
                auto hands=std::make_shared<SampledHands>();
                hands->authored[0]=m_authored[0]; hands->authored[1]=m_authored[1];
                hands->hands_local=m_hands_local; hands->maghand_local=m_maghand_local;
                hands->hand_in_item[0]=m_hand_in_item[0]; hands->hand_in_item[1]=m_hand_in_item[1];
                hands->maghand_in_root=m_maghand_in_root;
                for (int i=0;i<3;++i) hands->maghand_basis[i]=m_maghand_basis[i];
                hands->maghand_palm=m_maghand_palm;
                hands->handle_hand=m_handle_hand; hands->handle_hand_valid=m_handle_hand_valid;
                hands->palm_trusted=m_palm_trusted;
                hands->visual=m_hands_visual_name;
                hands->aim_eye=m_aim_eye; hands->aim_dir=m_aim_dir; hands->aim_valid=m_aim_valid;
                SampledHandsCache()[key]=hands;
            }
        }
        sampled=true;
    }
    // The sight does not depend on the hands, only on the scope fitted (and
    // the hands' aiming line as its last fallback).
    const bool scope_now=m_weapon.IsScopeAttached();
    if (sampled || !m_sight_measured || m_sight_scope!=scope_now)
    {
        m_sight_measured=true;
        m_sight_scope=scope_now;
        MeasureSight();
    }
}

void CVrWeaponInteraction::SampleAuthoredHands()
{
    // The first-person animations already hold every weapon the way an
    // animator posed it: index on the trigger, thumb along the frame, the
    // other hand on the fore-end or cupping a pistol grip. Play the weapon's
    // idle on a private copy of the first-person hands at its first frame and
    // record both hands relative to the weapon, as player_hud places it
    // (anchor bone, then item_position/item_orientation).
    if (!g_player_hud) return;
    const shared_str& hands=g_player_hud->section_name();
    m_authored_sampled=true;
    m_authored_hands=hands;
    m_authored[0].valid=m_authored[1].valid=false;
    m_aim_valid=false;
    m_palm_trusted=true;
    const shared_str& section=m_weapon.HudSection();
    if (!hands.size() || !section.size() || !pSettings->section_exist(section) ||
        !pSettings->line_exist(hands,"visual") || !pSettings->line_exist(section,"item_position") ||
        !pSettings->line_exist(section,"item_orientation")) return;
    player_hud_motion_container* motions=g_player_hud->get_hand_motions(section.c_str());
    player_hud_motion* idle=motions ? motions->find_motion("anm_idle") : nullptr;
    if (!idle || idle->m_animations.empty()) return;
    xr_vector<shared_str> anchors;
    for (const auto& item:pSettings->r_section(hands).Data)
        if (strstr(item.first.c_str(),"ancor_")==item.first.c_str()) anchors.push_back(item.second);
    const bool lead=READ_IF_EXISTS(pSettings,r_bool,section,"lh_lead_gun",false);
    const u16 place=READ_IF_EXISTS(pSettings,r_u16,section,"attach_place_idx",0);
    const size_t anchor_index=lead ? 0 : place;
    if (anchor_index>=anchors.size()) return;
    IRenderVisual* visual=::Render->model_Create(pSettings->r_string(hands,"visual"));
    auto* animated=visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    auto* model=visual ? visual->dcast_PKinematics() : nullptr;
    const u16 anchor=model ? model->LL_BoneID(anchors[anchor_index]) : BI_NONE;
    Fmatrix to_item, offset; to_item.identity(); offset.identity();
    if (animated && model && anchor!=BI_NONE)
    {
        PlayOnAllParts(*animated,idle->m_animations[0].mid);
        EvaluateBones(*animated,*model,0.f);
        xr_vector<Fmatrix> bind;
        model->LL_GetBindTransform(bind);
        Fvector ypr=pSettings->r_fvector3(section,"item_orientation");
        ypr.mul(PI/180.f);
        offset.setHPB(ypr.x,ypr.y,ypr.z);
        offset.translate_over(pSettings->r_fvector3(section,"item_position"));
        Fmatrix item; item.mul_43(model->LL_GetTransform(anchor),offset);
        to_item.invert(item);
        // A model built off its origin (the Mosin, 15 cm aside and 29 ahead)
        // is held as a whole away from where it is drawn: the first pass
        // finds the hand, and the hold moves onto the measured grip.
        for (int pass=0;pass<2;++pass)
        {
        bool right_found=false;
        for (int side=0;side<2;++side)
        {
            const char s=side ? 'r' : 'l';
            // Thumb, index and middle finger joints of the first-person hands.
            string64 names[3][3];
            for (int joint=0;joint<3;++joint)
            {
                const char* suffix[3]={"","1","2"};
                xr_sprintf(names[0][joint],"%c_finger0%s",s,suffix[joint]);
                xr_sprintf(names[1][joint],"bip01_%c_finger1%s",s,suffix[joint]);
                xr_sprintf(names[2][joint],"bip01_%c_finger2%s",s,suffix[joint]);
            }
            u16 ids[3][3];
            bool found=true;
            for (int f=0;f<3;++f)
                for (int j=0;j<3;++j)
                    found=(ids[f][j]=model->LL_BoneID(names[f][j]))!=BI_NONE && found;
            string16 hand_name; xr_sprintf(hand_name,"%c_hand",s);
            const u16 hand=model->LL_BoneID(hand_name);
            if (!found || hand==BI_NONE || ids[0][0]>=bind.size()) continue;
            const auto at=[&](u16 id) { Fvector p; to_item.transform_tiny(p,model->LL_GetTransform(id).c); return p; };
            AuthoredHandRef& ref=m_authored[side];
            const Fvector wrist=at(hand), index=at(ids[1][0]), middle=at(ids[2][0]);
            ref.knuckles.add(index,middle).mul(0.5f);
            Fvector knuckles; knuckles.sub(ref.knuckles,wrist);
            Fvector index_side; index_side.sub(index,middle);
            knuckles.normalize_safe();
            index_side.mad(knuckles,-index_side.dotproduct(knuckles)).normalize_safe();
            ref.basis[0]=knuckles; ref.basis[1]=index_side;
            ref.basis[2].crossproduct(knuckles,index_side);
            // The handle centre in the fist: behind the knuckles toward the
            // wrist and in from the palm (basis[2] is the palm normal of a
            // right hand, the back of a left one).
            ref.palm=ref.knuckles;
            ref.palm.mad(knuckles,-0.045f).mad(ref.basis[2],side ? 0.025f : -0.025f);
            for (int f=0;f<3;++f)
                for (int j=0;j<3;++j)
                {
                    Fvector& dir=ref.finger[f][j];
                    if (j<2) dir.sub(at(ids[f][j+1]),at(ids[f][j]));
                    else
                    {
                        // The last segment has no child joint: it continues
                        // the bone's own bind direction from its parent.
                        Fmatrix bind_rotation=bind[ids[f][2]]; bind_rotation.c.set(0,0,0);
                        Fmatrix bind_inverse; bind_inverse.invert(bind_rotation);
                        Fvector local, along;
                        along.sub(bind[ids[f][2]].c,bind[ids[f][1]].c);
                        bind_inverse.transform_dir(local,along);
                        Fmatrix posed=model->LL_GetTransform(ids[f][2]);
                        posed.transform_dir(along,local);
                        to_item.transform_dir(dir,along);
                    }
                    dir.normalize_safe();
                }
            // A hand the animation keeps off the weapon (a one-handed idle)
            // gives no reference for it.
            float nearest=flt_max;
            for (const Fvector& point:m_cloud) nearest=std::min(nearest,point.distance_to_sqr(ref.knuckles));
            ref.valid=nearest<0.12f*0.12f;
            right_found=right_found || side==1;
        }
        if (pass || !right_found) break;
        Fvector measured=m_profile.has_hand_grip_center ? m_profile.hand_grip_center : m_profile.grip_offset;
        Fvector shift; shift.sub(measured,m_authored[1].palm);
        if (shift.magnitude()<0.12f || shift.magnitude()>0.6f) break;
        // The measured grip is the mean of the mesh under the trigger: on a
        // stock's wrist (the Mosin's) that is its underside, and the hands
        // sat 4-5 cm below it. The hold goes to the middle of the wrist.
        {
            // The wrist's whole height at the grip: its lowest point up to
            // 1.5 cm under the bore line (the receiver sits above).
            const float top=m_profile.muzzle.square_magnitude()>EPS_S ? m_profile.muzzle.y-0.015f : measured.y+0.06f;
            float low=flt_max, high=-flt_max;
            for (const Fvector& p:m_cloud)
                if (_abs(p.z-measured.z)<0.012f && _abs(p.x-measured.x)<0.02f && p.y<top && p.y>top-0.15f)
                { low=std::min(low,p.y); high=std::max(high,p.y); }
            const float centre=0.5f*(low+high);
            if (low<high && high-low>=0.015f && _abs(centre-measured.y)<0.06f)
            {
                Msg("* [VR weapon] %s: grip height %.3f moved to the middle of the weapon there, %.3f",
                    m_profile.section.c_str(),measured.y,centre);
                measured.y=centre;
                shift.sub(measured,m_authored[1].palm);
            }
        }
        // Diagnostic (L85A2: both hands 25-40 cm off after this move): is the
        // animated hand already round the weapon's mesh before it, and is the
        // measured grip under the bore?
        float on_mesh=flt_max;
        for (const Fvector& point:m_cloud) on_mesh=std::min(on_mesh,point.distance_to_sqr(m_authored[1].palm));
        to_item.c.add(shift);
        Msg("* [VR weapon] %s: animated hold moved (%.3f %.3f %.3f) onto the grip; before, its palm was %.0f mm "
            "from the mesh; grip %.0f mm under the fire point, trigger at (%.3f %.3f %.3f)",
            m_profile.section.c_str(),shift.x,shift.y,shift.z,_sqrt(on_mesh)*1000.f,
            (m_profile.muzzle.y-measured.y)*1000.f,m_profile.trigger_point.x,m_profile.trigger_point.y,
            m_profile.trigger_point.z);
        }
    }
    // Keep every bone's local pose and each hand's place on the weapon: the
    // arms model uses the same skeleton (see player_legs_controller).
    m_hands_local.clear(); m_maghand_local.clear();
    m_hands_visual_name=pSettings->r_string(hands,"visual");
    const auto locals=[&](xr_vector<Fmatrix>& out)
    {
        const u16 count=model->LL_BoneCount();
        out.resize(count);
        for (u16 id=0;id<count;++id)
        {
            const u16 parent=model->LL_GetData(id).GetParentID();
            out[id]=model->LL_GetTransform(id);
            if (parent<count)
            {
                Fmatrix inverse; inverse.invert(model->LL_GetTransform(parent));
                out[id].mulA_43(inverse);
            }
        }
    };
    if (animated && model && anchor!=BI_NONE && (m_authored[0].valid || m_authored[1].valid))
    {
        locals(m_hands_local);
        for (int side=0;side<2;++side)
        {
            const u16 hand=model->LL_BoneID(side ? "r_hand" : "l_hand");
            if (hand!=BI_NONE) m_hand_in_item[side].mul_43(to_item,model->LL_GetTransform(hand));
        }
        // The sight line: where the flat-screen camera is when aiming down
        // the sights, in weapon space. player_hud places the weapon at
        // camera * aim offset * hands attach * anchor bone * item offset,
        // with the hands in their aiming idle.
        player_hud_motion* aim=motions->find_motion("anm_idle_aim");
        if (aim && !aim->m_animations.empty())
        {
            PlayOnAllParts(*animated,aim->m_animations[0].mid);
            EvaluateBones(*animated,*model,0.f);
        }
        if (attachable_hud_item* hud=m_weapon.HudItemData())
        {
            const hud_item_measures& measures=hud->m_measures;
            Fvector turn=measures.m_hands_attach[1]; turn.mul(PI/180.f);
            Fmatrix attach; attach.setHPB(turn.x,turn.y,turn.z);
            attach.translate_over(measures.m_hands_attach[0]);
            const Fvector& aim_rot=measures.m_hands_offset[1][1];
            Fmatrix aim_offset; aim_offset.identity(); aim_offset.rotateX(aim_rot.x);
            Fmatrix axis_turn; axis_turn.identity(); axis_turn.rotateY(aim_rot.y); aim_offset.mulA_43(axis_turn);
            axis_turn.identity(); axis_turn.rotateZ(aim_rot.z); aim_offset.mulA_43(axis_turn);
            aim_offset.translate_over(measures.m_hands_offset[0][1]);
            Fmatrix item_aim; item_aim.mul_43(model->LL_GetTransform(anchor),offset);
            Fmatrix in_hands; in_hands.mul_43(attach,item_aim);
            Fmatrix in_camera; in_camera.mul_43(aim_offset,in_hands);
            Fmatrix camera; camera.invert(in_camera);
            Fvector direction=camera.k; direction.normalize_safe();
            if (direction.z>0.9f)
            {
                m_aim_eye=camera.c;
                m_aim_dir=direction;
                m_aim_valid=true;
            }
        }
        // The magazine hold: play the reload on the hands and the weapon
        // together and take the frame where the left hand carries the
        // magazine about 4 cm below its seat, on its way into the well.
        player_hud_motion* reload=motions->find_motion("anm_reload");
        if (!reload || reload->m_animations.empty()) reload=motions->find_motion("anm_reload_empty");
        const u16 left=model->LL_BoneID("l_hand");
        if (m_profile.detachable && m_profile.mag_bone.size() && reload && !reload->m_animations.empty() &&
            left!=BI_NONE)
        {
            IRenderVisual* item_visual=::Render->model_Create(m_profile.visual.c_str());
            auto* item_animated=item_visual ? item_visual->dcast_PKinematicsAnimated() : nullptr;
            auto* item_model=item_visual ? item_visual->dcast_PKinematics() : nullptr;
            const u16 mag=item_model ? item_model->LL_BoneID(m_profile.mag_bone.c_str()) : BI_NONE;
            const shared_str item_name=reload->m_base_name!=reload->m_additional_name ?
                reload->m_additional_name : reload->m_animations[0].name;
            const MotionID item_motion=item_animated ? item_animated->ID_Cycle_Safe(item_name) : MotionID();
            if (item_animated && mag!=BI_NONE && item_motion.valid())
            {
                xr_vector<Fmatrix> item_bind;
                item_model->LL_GetBindTransform(item_bind);
                Fmatrix unbind; unbind.invert(item_bind[mag]);
                // The magazine the hand carries may be a spare the reload
                // brings in (an AKS-74U's "magazin_2"), the seated one being
                // hidden meanwhile: any magazine-named part about its size
                // counts, and the held magazine is drawn where that one is.
                xr_vector<u16> carried;
                carried.push_back(mag);
                {
                    Fbox own;
                    Fvector own_size; own_size.set(0,0,0);
                    if (MeasureMagazine(*item_model,mag,own)) own.getsize(own_size);
                    for (u16 id=0;id<item_model->LL_BoneCount();++id)
                    {
                        Fbox box;
                        if (id==mag || !NameHas(item_model->LL_BoneName_dbg(id),"mag") ||
                            !MeasureBone(*item_model,id,box,false)) continue;
                        Fvector size; box.getsize(size);
                        if (_abs(size.y-own_size.y)<=0.5f*own_size.y) carried.push_back(id);
                    }
                }
                PlayOnAllParts(*animated,reload->m_animations[0].mid);
                PlayOnAllParts(*item_animated,item_motion);
                float best=flt_max;
                // Hands that name their fingers otherwise have no hold frame.
                const u16 index_bone=model->LL_BoneID("bip01_l_finger1"), middle_bone=model->LL_BoneID("bip01_l_finger2");
                // The hand must really carry it: its palm on the magazine (within
                // 3 cm of its box) and still against it from the frame before
                // while the magazine moves. The wrist within 15 cm of the
                // magazine's centre took frames where the hand only reached for
                // it (an RPK's, a VSK-94's): held so, it floated off the fingers.
                xr_vector<Fvector> last_palm(carried.size()), last_root(carried.size());
                xr_vector<u8> have_last(carried.size(),0);
                for (int frame=0;frame<480 && index_bone!=BI_NONE && middle_bone!=BI_NONE;++frame)
                {
                    const float dt=frame ? 1.f/60.f : 0.f;
                    EvaluateBones(*animated,*model,dt);
                    EvaluateBones(*item_animated,*item_model,dt);
                    Fmatrix item_now; item_now.mul_43(model->LL_GetTransform(anchor),offset);
                    const Fmatrix& hand_now=model->LL_GetTransform(left);
                    // The palm, as m_maghand_palm below, in the hands' space.
                    Fvector palm;
                    {
                        const Fvector& index=model->LL_GetTransform(index_bone).c;
                        const Fvector& middle=model->LL_GetTransform(middle_bone).c;
                        Fvector knuckles; knuckles.add(index,middle).mul(0.5f);
                        Fvector forward; forward.sub(knuckles,hand_now.c).normalize_safe();
                        Fvector side; side.sub(index,middle);
                        side.mad(forward,-side.dotproduct(forward)).normalize_safe();
                        Fvector normal; normal.crossproduct(forward,side);
                        palm=knuckles; palm.mad(forward,-0.045f).mad(normal,-0.025f);
                    }
                    Fmatrix root;
                    float error=flt_max;
                    for (size_t n=0;n<carried.size();++n)
                    {
                        const u16 part=carried[n];
                        const Fmatrix& mag_now=item_model->LL_GetTransform(part);
                        Fvector moved; moved.sub(mag_now.c,item_bind[mag].c);
                        const float depth=moved.dotproduct(m_profile.well_axis);
                        Fmatrix root_item; root_item.mul_43(mag_now,unbind);
                        Fmatrix candidate; candidate.mul_43(item_now,root_item);
                        Fvector in_root; Fmatrix().invert(candidate).transform_tiny(in_root,palm);
                        const bool rigid=have_last[n] && in_root.distance_to(last_palm[n])<=0.004f &&
                            candidate.c.distance_to(last_root[n])>=0.002f;
                        last_palm[n]=in_root; last_root[n]=candidate.c; have_last[n]=1;
                        // How far the palm is outside the magazine's box.
                        Fvector off;
                        off.sub(in_root,m_profile.mag_box_center);
                        for (int a=0;a<3;++a) off[a]=std::max(0.f,_abs(off[a])-m_profile.mag_box_half[a]);
                        const float reach=off.magnitude();
                        if (depth< -0.09f || depth> -0.015f || reach>0.03f || !rigid) continue;
                        // Nearest 4 cm below the seat, with the palm on it.
                        const float score=_abs(depth+0.04f)+reach;
                        if (score<error) { error=score; root=candidate; }
                    }
                    if (error>=best) continue;
                    best=error;
                    Fmatrix to_root; to_root.invert(root);
                    m_maghand_in_root.mul_43(to_root,hand_now);
                    locals(m_maghand_local);
                    // The hand's anatomical frame and palm, in hand-bone space.
                    Fmatrix to_hand; to_hand.invert(hand_now);
                    Fvector index, middle;
                    to_hand.transform_tiny(index,model->LL_GetTransform(index_bone).c);
                    to_hand.transform_tiny(middle,model->LL_GetTransform(middle_bone).c);
                    Fvector knuckles; knuckles.add(index,middle).mul(0.5f);
                    Fvector forward=knuckles; forward.normalize_safe();
                    Fvector side; side.sub(index,middle);
                    side.mad(forward,-side.dotproduct(forward)).normalize_safe();
                    m_maghand_basis[0]=forward; m_maghand_basis[1]=side;
                    m_maghand_basis[2].crossproduct(forward,side);
                    m_maghand_palm=knuckles;
                    m_maghand_palm.mad(forward,-0.045f).mad(m_maghand_basis[2],-0.025f);
                }
                if (best==flt_max) m_maghand_local.clear();
            }
            if (item_visual) ::Render->model_Delete(item_visual);
        }
    }
    // Where the reload's left hand is when it has the cycling part (or a
    // separate charging handle) furthest back: it is on the handle there.
    m_handle_hand_valid=false;
    const shared_str& worked=m_profile.handle_bone.size() ? m_profile.handle_bone : m_profile.cycle_bone;
    if (animated && model && anchor!=BI_NONE && motions && worked.size() && model->LL_BoneID("l_hand")!=BI_NONE)
    {
        player_hud_motion* empty=motions->find_motion("anm_reload_empty");
        if (!empty || empty->m_animations.empty()) empty=motions->find_motion("anm_reload");
        IRenderVisual* item_visual=empty && !empty->m_animations.empty() ? ::Render->model_Create(m_profile.visual.c_str()) : nullptr;
        auto* item_animated=item_visual ? item_visual->dcast_PKinematicsAnimated() : nullptr;
        auto* item_model=item_visual ? item_visual->dcast_PKinematics() : nullptr;
        const u16 part=item_model ? item_model->LL_BoneID(worked.c_str()) : BI_NONE;
        const u16 trigger=item_model ? FindTrigger(*item_model) : BI_NONE;
        const u16 receiver=trigger!=BI_NONE && item_model->LL_GetData(trigger).GetParentID()<item_model->LL_BoneCount() ?
            item_model->LL_GetData(trigger).GetParentID() : u16(0);
        const shared_str item_name=empty && empty->m_base_name!=empty->m_additional_name ?
            empty->m_additional_name : (empty ? empty->m_animations[0].name : shared_str());
        const MotionID item_motion=item_animated ? item_animated->ID_Cycle_Safe(item_name) : MotionID();
        if (item_animated && part!=BI_NONE && item_motion.valid())
        {
            xr_vector<Fmatrix> item_bind;
            item_model->LL_GetBindTransform(item_bind);
            const u16 left=model->LL_BoneID("l_hand");
            const u16 index=model->LL_BoneID("bip01_l_finger1"), middle=model->LL_BoneID("bip01_l_finger2");
            PlayOnAllParts(*animated,empty->m_animations[0].mid);
            PlayOnAllParts(*item_animated,item_motion);
            float furthest=0.02f;
            for (int frame=0;frame<480;++frame)
            {
                const float dt=frame ? 1.f/60.f : 0.f;
                EvaluateBones(*animated,*model,dt);
                EvaluateBones(*item_animated,*item_model,dt);
                const float back=RearInReceiver(*item_model,item_bind,part,receiver);
                if (back<=furthest) continue;
                furthest=back;
                // The palm: behind the knuckles toward the wrist.
                Fvector wrist=model->LL_GetTransform(left).c, knuckles=wrist;
                if (index!=BI_NONE && middle!=BI_NONE)
                    knuckles.add(model->LL_GetTransform(index).c,model->LL_GetTransform(middle).c).mul(0.5f);
                Fvector along; along.sub(knuckles,wrist);
                Fvector palm=knuckles;
                if (along.square_magnitude()>EPS_S) palm.mad(along.normalize(),-0.03f);
                Fmatrix item_now; item_now.mul_43(model->LL_GetTransform(anchor),offset);
                Fmatrix to_item; to_item.invert(item_now);
                Fvector in_item; to_item.transform_tiny(in_item,palm);
                // Into the receiver's rest frame (the reload may tilt it).
                Fmatrix rel; rel.invert(item_model->LL_GetTransform(receiver)); rel.mulA_43(item_bind[receiver]);
                rel.transform_tiny(m_handle_hand,in_item);
                // Back where the handle rests: the hand is followed back by
                // the slide as it is pulled.
                m_handle_hand.z+=back;
                m_handle_hand_valid=true;
            }
        }
        if (item_visual) ::Render->model_Delete(item_visual);
        if (m_handle_hand_valid)
            Msg("* [VR weapon] %s: the reload's hand takes the %s at (%.3f %.3f %.3f); the mesh's point is (%.3f %.3f %.3f)",
                m_weapon.cNameSect().c_str(),worked.c_str(),m_handle_hand.x,m_handle_hand.y,m_handle_hand.z,
                m_profile.cycle_grab.x,m_profile.cycle_grab.y,m_profile.cycle_grab.z);
    }
    if (visual) ::Render->model_Delete(visual);
    // The animated weapon hand against the measured grip, or the estimate
    // from the trigger: far apart, the measured grip holds the weapon and the
    // arms are posed by IK (GripLocal, AuthoredBones, AuthoredHand).
    if (m_authored[1].valid)
    {
        const Fvector& measured=m_profile.has_hand_grip_center ? m_profile.hand_grip_center : m_profile.grip_offset;
        m_palm_trusted=m_authored[1].palm.distance_to(measured)<=0.08f;
        // An animated palm inside the weapon's solid (round a stock's wrist)
        // holds it, however far the measured grip (the mean of the mesh under
        // the trigger, a stock-wrist gun's underside) is: the BM-16's,
        // Remington 700's and M79's hands were 5-10 cm under their stocks.
        float centre=0.f; bool inside=false;
        if (!m_palm_trusted && m_authored[1].palm.distance_to(measured)<=0.15f &&
            SectionCentre(m_authored[1].palm,centre,inside) && inside)
        {
            m_palm_trusted=true;
            Msg("* [VR weapon] %s: the animation's hand is round the weapon (middle %.3f): it holds it",
                m_profile.section.c_str(),centre);
        }
        Msg("* [VR weapon] %s: hand from the animation (%.3f %.3f %.3f), grip measured (%.3f %.3f %.3f)%s",
            m_profile.section.c_str(),m_authored[1].palm.x,m_authored[1].palm.y,m_authored[1].palm.z,
            measured.x,measured.y,measured.z,
            m_palm_trusted ? "" : "; too far apart, the measured grip is used");
    }
    Msg("* [VR weapon] %s: magazine hold from the reload animation: %s",m_weapon.cNameSect().c_str(),
        m_maghand_local.empty() ? "no" : "yes");
    Msg("* [VR weapon] %s: first-person grip %s%s", m_weapon.cNameSect().c_str(),
        m_authored[1].valid ? "right" : "-", m_authored[0].valid ? " left" : "");
}

namespace vrwi
{
// A sight's glass mesh in a weapon model: Anomaly's LFO lens, reticle and
// dot shaders (models\lfo_*), reticle and red-lens textures, 3D scope
// lenses. dot: the mesh is a red dot.
bool SightGlass(LPCSTR shader, LPCSTR texture, bool& dot, bool& soft)
{
    shader=shader ? shader : ""; texture=texture ? texture : "";
    // The names: [vr_derivation] in configs\vr\vr_weapon_mechanics.ltx.
    const VrNameTables& names=VrNames();
    dot=ContainsAny(shader,names.red_dot_shaders) || ContainsAny(texture,names.red_dot_textures);
    // The soft black lens is a magnified scope's exit-pupil shadow (AUG,
    // SUSAT, G36); red dots have none. GAMMA's 3D shader scopes (3DSS) draw
    // their image on an eyepiece mesh with models\scope_reticle* (its
    // scopelense 3); their scope_back and scope_zwrite meshes are not glass.
    soft=ContainsAny(shader,names.soft_lens_shaders);
    return dot || soft || ContainsAny(shader,names.glass_shaders) || ContainsAny(texture,names.glass_textures);
}
// A scope part's eyepiece: the rear 4 cm of its mesh, the upper 5 cm of
// each 1 cm slice (a mount hangs below the tube), the median slice centre.
// Checked offline on the PSO-1 of the AK-74M and the G3SG1's scope.
bool ScopeEyepiece(const xr_vector<Fvector>& points, Fvector& centre, float& radius, float& front)
{
    if (points.size()<50) return false;
    float back=flt_max; front=-flt_max;
    for (const Fvector& point:points) { back=std::min(back,point.z); front=std::max(front,point.z); }
    if (front-back<0.04f) return false;
    xr_vector<float> xs, ys;
    float size=flt_max;
    for (int slice=0;slice<4;++slice)
    {
        const float low=back+0.01f*slice, high=low+0.01f;
        float top=-flt_max;
        for (const Fvector& point:points) if (point.z>=low && point.z<high) top=std::max(top,point.y);
        if (top==-flt_max) continue;
        float x1=flt_max, x2=-flt_max, y1=flt_max, y2=-flt_max;
        for (const Fvector& point:points)
            if (point.z>=low && point.z<high && point.y>top-0.05f)
            {
                x1=std::min(x1,point.x); x2=std::max(x2,point.x);
                y1=std::min(y1,point.y); y2=std::max(y2,point.y);
            }
        xs.push_back(0.5f*(x1+x2)); ys.push_back(0.5f*(y1+y2));
        size=std::min(size,std::min(x2-x1,y2-y1));
    }
    if (xs.empty()) return false;
    std::sort(xs.begin(),xs.end()); std::sort(ys.begin(),ys.end());
    centre.set(xs[xs.size()/2],ys[ys.size()/2],back);
    radius=0.5f*size;
    return true;
}
}

void CVrWeaponInteraction::MeasureOptics(IKinematics& model)
{
    // The sight's own geometry: its glass meshes (by shader and texture)
    // and the scope parts' mesh (wpn_scope bones, left out of the grip
    // cloud). VR leaves the glass meshes out of the picture (they are
    // drawn for the flat-screen camera) and draws its own lens and dot.
    m_glass.clear(); m_lens_meshes.clear(); m_glass_dot=m_glass_soft=false;
    m_optic_cloud.clear();
    CloudCollector collector(m_optic_cloud);
    xr_vector<u16> fittings;
    for (u16 id=0;id<model.LL_BoneCount();++id)
    {
        bool scope=false;
        for (u16 bone=id;!scope && bone!=BI_NONE && bone<model.LL_BoneCount();
             bone=model.LL_GetData(bone).GetParentID())
        {
            scope=NameHas(model.LL_BoneName_dbg(bone),"scope");
            if (model.LL_GetData(bone).GetParentID()==bone) break;
        }
        if (!scope) continue;
        // Not what sits under or over the tube: a riser or mount (the Gauss
        // rifle's scope_raiser pulled its eyepiece down) or a cap.
        LPCSTR name=model.LL_BoneName_dbg(id);
        const bool fitting=NameWord(name,"raiser") || NameWord(name,"riser") || NameWord(name,"mount") ||
            NameWord(name,"rail") || NameWord(name,"cover") || NameWord(name,"cap");
        if (fitting) fittings.push_back(id);
        else model.EnumBoneVertices(collector,id);
    }
    // A scope modelled on such a bone alone (its tube on wpn_scope_mount) is
    // all fitting: it is the scope then, and none of it is hidden.
    const bool fitted=m_optic_cloud.empty() && fittings.size();
    if (fitted)
        for (u16 id:fittings) model.EnumBoneVertices(collector,id);
    // Caps and covers on the scope, and 3DSS's lens-hide bone (its glass
    // over the eyepiece, hidden by native aiming).
    for (shared_str& name:m_scope_hidden) name="";
    int hidden=0;
    for (u16 id=0;id<model.LL_BoneCount() && hidden<2 && !fitted;++id)
    {
        LPCSTR name=model.LL_BoneName_dbg(id);
        if (NameHas(name,"scope") && (NameWord(name,"cover") || NameWord(name,"cap")))
            m_scope_hidden[hidden++]=name;
    }
    const shared_str& own=m_weapon.cNameSect();
    if (pSettings->line_exist(own,"lewd_lens_hide_bone"))
    {
        LPCSTR bone=pSettings->r_string(own,"lewd_lens_hide_bone");
        if (bone && *bone && model.LL_BoneID(bone)!=BI_NONE) m_scope_hidden[2]=bone;
    }
    m_mesh_shaders="";
    IRenderVisual* visual=model.dcast_RenderVisual();
    xr_vector<IRenderVisual*>* children=visual ? visual->get_children() : nullptr;
    if (!children) return;
    string512 shaders="";
    for (IRenderVisual* child:*children)
    {
        LPCSTR shader=child ? child->getDebugShader() : nullptr;
        if (!shader || !*shader) continue;
        if (!strstr(shaders,shader) && xr_strlen(shaders)+xr_strlen(shader)+3<sizeof(shaders))
        {
            if (*shaders) xr_strcat(shaders,", ");
            xr_strcat(shaders,shader);
        }
        bool dot=false, soft=false;
        LPCSTR texture=child->getDebugTexture();
        if (!SightGlass(shader,texture,dot,soft))
        {
            // An ordinary mesh with a lens texture: the glass of a scope
            // built into the model (wpn_fn2000_lens). Only used for a
            // weapon the config or glass already makes a scope.
            if (texture && strstr(texture,"lens")) m_lens_meshes.push_back(child->getVisData().box);
            continue;
        }
        m_glass.push_back(child->getVisData().box);
        m_glass_dot=m_glass_dot || dot;
        m_glass_soft=m_glass_soft || soft;
    }
    m_mesh_shaders=shaders;
}

void CVrWeaponInteraction::MeasureSight()
{
    // What sight the weapon has and where. Anomaly builds most scoped and
    // red-dot weapons as their own sections (wpn_ak74m_ps01, wpn_ak74m_pka,
    // wpn_p90_eot), usually with scope_status 0; integrated sights (AUG)
    // only set a zoom. The place comes from the model: its glass meshes,
    // else the rear of its scope part. Sights look along the bore (+z).
    // The flat-screen aiming line (m_aim_eye/m_aim_dir from
    // SampleAuthoredHands) is only a fallback: it is the HUD camera, not
    // where an eye would be. Measured again on a scope change, from scratch.
    m_sight=SightInfo();
    m_sight.type=SightIron;
    const shared_str section=m_weapon.cNameSect();
    shared_str addon;
    if (m_weapon.get_ScopeStatus()==ALife::eAddonAttachable && m_weapon.IsScopeAttached())
        addon=m_weapon.GetScopeName();
    else if (pSettings->line_exist(section,"scopes"))
    {
        LPCSTR list=pSettings->r_string(section,"scopes");
        const size_t length=xr_strlen(section.c_str());
        for (int index=0;index<_GetItemCount(list);++index)
        {
            string128 item; _GetItem(list,index,item);
            const size_t size=xr_strlen(item);
            if (size && length>size+1 && section.c_str()[length-size-1]=='_' &&
                !xr_strcmp(section.c_str()+length-size,item)) { addon=item; break; }
        }
    }
    // Weapons that come with a red dot, by name (wpn_ak74m_pka, wpn_p90_eot).
    static LPCSTR const reflex_names[]={"_pka","_kobra","_eot","_ekp","_aimpoint","_compm","_okp",
        "_rmr","_holo","_reflex","_1p87","_srs","_deltapoint","_docter"};
    bool reflex_named=false;
    for (LPCSTR name:reflex_names) reflex_named=reflex_named || strstr(section.c_str(),name)!=nullptr;
    float zoom=READ_IF_EXISTS(pSettings,r_float,section,"scope_zoom_factor",0.f);
    if (zoom<=0.f && addon.size() && pSettings->section_exist(addon))
        zoom=READ_IF_EXISTS(pSettings,r_float,addon,"scope_zoom_factor",0.f);
    const bool lens=m_weapon.IsSecondVPZoomPresent();
    const bool fixed=m_weapon.get_ScopeStatus()==ALife::eAddonPermanent;
    // The glass: all of it, and the rearmost mesh (a scope's eyepiece).
    Fbox glass; glass.invalidate();
    const Fbox* rear_glass=nullptr;
    for (const Fbox& box:m_glass)
    {
        glass.merge(box);
        if (!rear_glass || box.min.z<rear_glass->min.z) rear_glass=&box;
    }
    const float span=rear_glass ? glass.max.z-glass.min.z : 0.f;
    Fvector cup; float cup_radius=0.f, part_front=0.f;
    const bool part=ScopeEyepiece(m_optic_cloud,cup,cup_radius,part_front);
    // The zoom factor is the zoomed field of view (x0.75 in the camera):
    // under 60 degrees, or a 3D lens, is a magnified scope. Glass along
    // 5 cm or more is a scope tube, and a soft black lens a scope's exit
    // pupil (the G36's has no zoom in its config); a red dot otherwise.
    const bool zoomed=lens || (zoom>0.f && zoom<60.f);
    const bool scope=rear_glass ? span>=0.05f || m_glass_soft || (zoomed && !m_glass_dot) : zoomed;
    // A scope built into the model: its lens-textured glass, the rearmost.
    const Fbox* lens_mesh=nullptr;
    for (const Fbox& box:m_lens_meshes)
        if (!lens_mesh || box.min.z<lens_mesh->min.z) lens_mesh=&box;
    if (scope)
    {
        m_sight.type=SightScope;
        m_sight.lens_fov=lens ? m_weapon.GetSecondVPFov() : (zoom>0.f ? zoom*0.75f : g_fov/4.f);
        m_sight.magnification=std::max(1.f,g_fov/std::max(m_sight.lens_fov,1.f));
    }
    else if (rear_glass || reflex_named || addon.size() || fixed || zoom>=60.f)
    {
        m_sight.type=SightCollimator;
        m_sight.magnification=1.f;
    }
    else return;
    m_sight.rear=scope ? 0.07f : 0.15f;
    LPCSTR source;
    float front=-flt_max;
    // A scope's rear glass far smaller than the eyepiece of its tube is a
    // reticle plane set inside it (an SVD Lynx's 1P59 crosshair), not the
    // window: the image then sat off the tube and much too small.
    const bool inset=scope && rear_glass && part &&
        0.5f*std::min(rear_glass->max.x-rear_glass->min.x,rear_glass->max.y-rear_glass->min.y)<0.4f*cup_radius;
    if (inset)
    {
        m_sight.window=cup;
        m_sight.dir.set(0.f,0.f,1.f);
        m_sight.radius=clampr(cup_radius*0.8f,0.008f,0.025f);
        front=std::max(glass.max.z,part_front);
        source="scope part (its glass is a small inset plane)";
    }
    else if (rear_glass)
    {
        // A scope's eyepiece is its rearmost glass; a red dot's glass is
        // all of its meshes together.
        const Fbox& box=scope ? *rear_glass : glass;
        box.getcenter(m_sight.window);
        if (scope) m_sight.window.z=box.min.z;
        m_sight.dir.set(0.f,0.f,1.f);
        const float half=0.5f*std::min(box.max.x-box.min.x,box.max.y-box.min.y);
        m_sight.radius=clampr(half,scope ? 0.008f : 0.012f,0.03f);
        front=glass.max.z;
        if (part) front=std::max(front,part_front);
        source="glass meshes";
    }
    else if (lens_mesh && scope)
    {
        lens_mesh->getcenter(m_sight.window);
        m_sight.window.z=lens_mesh->min.z;
        m_sight.dir.set(0.f,0.f,1.f);
        const float half=0.5f*std::min(lens_mesh->max.x-lens_mesh->min.x,lens_mesh->max.y-lens_mesh->min.y);
        m_sight.radius=clampr(half,0.008f,0.025f);
        front=lens_mesh->max.z;
        if (part) front=std::max(front,part_front);
        source="lens mesh";
    }
    else if (part && scope)
    {
        m_sight.window=cup;
        m_sight.dir.set(0.f,0.f,1.f);
        m_sight.radius=clampr(cup_radius*0.8f,0.008f,0.025f);
        front=part_front;
        source="scope part";
    }
    else if (m_aim_valid)
    {
        m_sight.dir=m_aim_dir;
        m_sight.window=m_aim_eye; m_sight.window.mad(m_sight.dir,m_sight.rear);
        m_sight.radius=scope ? 0.016f : 0.015f;
        source="flat-screen aiming line";
        // A scope built into the model with no glass, lens or scope part
        // (G43, SIG 550 sniper, TRG, TOZ-34 Mark 4): its tube lies round the
        // aiming line. The flat-screen camera sits well behind the eyepiece,
        // so the window 7 cm ahead of it floated ~15 cm short of the scope.
        // The tube's rear end is the eyepiece: the upper half of the mesh
        // within 3 cm of the line (not a mount or cheek piece below it).
        if (scope && !m_cloud.empty())
        {
            float back=flt_max, front_z=-flt_max;
            unsigned used=0;
            for (const Fvector& p:m_cloud)
            {
                Fvector v; v.sub(p,m_aim_eye);
                const float t=v.dotproduct(m_aim_dir);
                if (t<0.03f || t>0.6f) continue;
                Fvector off=v; off.mad(m_aim_dir,-t);
                if (off.magnitude()>0.03f || off.y<-0.005f) continue;
                back=std::min(back,p.z); front_z=std::max(front_z,p.z);
                ++used;
            }
            if (used>=40 && front_z-back>=0.06f && _abs(m_aim_dir.z)>0.5f)
            {
                float radius=0.f;
                for (const Fvector& p:m_cloud)
                {
                    if (p.z<back || p.z>back+0.01f) continue;
                    Fvector v; v.sub(p,m_aim_eye);
                    const float t=v.dotproduct(m_aim_dir);
                    Fvector off=v; off.mad(m_aim_dir,-t);
                    if (off.magnitude()<=0.03f && off.y>=-0.005f) radius=std::max(radius,off.magnitude());
                }
                m_sight.window=m_aim_eye;
                m_sight.window.mad(m_aim_dir,(back-m_aim_eye.z)/m_aim_dir.z);
                m_sight.radius=clampr(radius*0.8f,0.008f,0.025f);
                front=front_z;
                source="scope tube round the flat-screen aiming line";
            }
        }
    }
    else { m_sight.type=SightIron; return; }
    m_sight.eye=m_sight.window; m_sight.eye.mad(m_sight.dir,-m_sight.rear);
    m_sight.valid=true;
    // How far the weapon reaches along the line (the scope's camera sits
    // past it), from the nominal eye.
    float reach=front>-flt_max ? front-m_sight.eye.z : 0.f;
    for (const Fvector& point:m_cloud)
    {
        Fvector to; to.sub(point,m_sight.eye);
        const float along=to.dotproduct(m_sight.dir);
        to.mad(m_sight.dir,-along);
        if (to.magnitude()<0.05f) reach=std::max(reach,along);
    }
    m_sight.front=std::max(reach,m_sight.rear+0.15f);
    Msg("* [VR weapon] %s: sight %s (%s), zoom %.0f, %.1fx, glass %.1f cm wide at %.3f %.3f %.3f, from the %s",
        section.c_str(),scope ? "scope" : "collimator",addon.size() ? addon.c_str() : "built in",
        zoom,m_sight.magnification,m_sight.radius*200.f,m_sight.window.x,m_sight.window.y,m_sight.window.z,source);
    if (!rear_glass && !part && !lens_mesh)
        Msg("* [VR weapon] %s: no sight glass or scope part found; mesh shaders: %s",section.c_str(),
            m_mesh_shaders.size() ? m_mesh_shaders.c_str() : "none");
}

bool CVrWeaponInteraction::EyeOnSight(const Fvector& eye, Fmatrix& world, Fvector& direction,
    Fvector& window, Fvector& offset, float& behind) const
{
    // Where an eye is relative to the sight's glass: how far behind it
    // along the sight, and its offset across the sight.
    if (!m_sight.valid || m_sight.type==SightIron || !Transform(world)) return false;
    world.transform_dir(direction,m_sight.dir);
    window=Point(world,m_sight.window);
    Fvector to; to.sub(window,eye);
    behind=to.dotproduct(direction);
    offset.mul(direction,behind).sub(to);
    return true;
}

void CVrWeaponInteraction::RenderSight()
{
    // Drawn into each eye (Device holds that eye's camera).
    if (!vr::GameplayActive()) return;
    Fmatrix world; Fvector direction, window, offset; float behind=0.f;
    const Fvector& eye=Device.vCameraPosition;
    // A collimator works from any distance behind its glass: a sight on the
    // handguard or barrel (a tactical one) is easily 0.6 m or more from the
    // eye at arm's length, where its dot was not drawn. A scope's image is
    // too small to use that far back.
    const float reach=m_sight.type==SightCollimator ? 2.f : 0.6f;
    if (!EyeOnSight(eye,world,direction,window,offset,behind) || behind>reach) return;
    if (Device.vCameraDirection.dotproduct(direction)<0.8f) return;
    Fvector right, up;
    right.crossproduct(world.j,direction).normalize_safe();
    up.crossproduct(direction,right);
    const float lateral=offset.magnitude();
    if (m_sight.type==SightCollimator)
    {
        // A collimator's dot is focused at infinity along the sight: from
        // anywhere behind the glass it is seen in that same direction, so
        // it stays on the target as the head moves, and only by an eye
        // whose view along the sight passes through the glass. Drawn 10 m
        // out over the scene (the glass is in front of everything else).
        // Anywhere behind the weapon's front: the glass position along the
        // sight does not change what a collimator shows. The window gets
        // 4 mm of slack for tracking and the headset's eye position.
        const float window_radius=m_sight.radius+0.004f;
        if (lateral>window_radius || behind<m_sight.rear-m_sight.front+0.05f) return;
        const float edge=clampr((window_radius-lateral)/0.004f,0.f,1.f);
        // Drawn where this eye's view along the sight crosses the glass,
        // at the same 4 mrad size, and depth tested there: the frame or a
        // hand in front of the glass hides it (the glass meshes themselves
        // are left out of the VR picture). An eye at or past the glass
        // sees it 10 m out over everything.
        if (behind>0.035f)
        {
            Fvector dot=eye; dot.mad(direction,behind-0.002f);
            const float half=0.004f*(behind-0.002f);
            vr::DrawReflexDot(dot,Fvector().mul(right,half),Fvector().mul(up,half),edge,true);
        }
        else
        {
            Fvector dot=eye; dot.mad(direction,10.f);
            vr::DrawReflexDot(dot,Fvector().mul(right,0.04f),Fvector().mul(up,0.04f),edge,false);
        }
        return;
    }
    // A scope passes its image through an exit pupil behind the eyepiece:
    // on axis the image fills the lens, off axis a shadow closes in from
    // that side (gone 2.5 cm off), and right at the glass it goes dark.
    // Further back the lens simply looks smaller and, at the same
    // magnification, shows less of the scene (RequestScopeView), which is
    // the tunnel a real scope gives; a headset cannot get an eye as close
    // as a cheek weld does, so there is no extra eye-relief cut.
    if (behind<0.015f) return;
    const float brightness=clampr((behind-0.015f)/0.015f,0.f,1.f);
    // The image is drawn vr_scope_size times the glass (a headset keeps
    // the eye further back than a cheek weld would).
    const float radius=m_sight.radius*vr::ScopeLensScale();
    vr::DrawScopeLens(window,Fvector().mul(right,radius),Fvector().mul(up,radius),
        brightness,offset.dotproduct(right)/0.025f,-offset.dotproduct(up)/0.025f);
}

void CVrWeaponInteraction::RequestScopeView() const
{
    // Ask the stereo renderer for the magnified view while the head is
    // behind the eyepiece and looking along the sight: from just past the
    // weapon, as wide as the lens appears from the eye divided by the
    // magnification (vr_scope_zoom scales the game's flat-screen zoom).
    if (m_sight.type!=SightScope) return;
    Fmatrix head_world;
    if (!vr::HeadTransform(head_world)) return;
    const Fvector head=head_world.c, looking=head_world.k;
    Fmatrix world; Fvector direction, window, offset; float behind=0.f;
    if (!EyeOnSight(head,world,direction,window,offset,behind)) return;
    if (behind<=0.f || behind>0.4f || offset.magnitude()>0.07f || looking.dotproduct(direction)<0.8f)
    {
        // Why not, when the head is near the scope (every 3 s at most).
        if (head.distance_to(window)<0.5f && Device.dwTimeGlobal-m_scope_refused_time>3000)
        {
            m_scope_refused_time=Device.dwTimeGlobal;
            Msg("* [VR weapon] %s: no scope view: head %.0f cm behind the eyepiece (0-40), %.1f cm off the line (<7), looking %.2f along it (>0.80)",
                m_weapon.cNameSect().c_str(),behind*100.f,offset.magnitude()*100.f,looking.dotproduct(direction));
        }
        return;
    }
    const float radius=m_sight.radius*vr::ScopeLensScale();
    const float apparent=rad2deg(2.f*atanf(radius/std::max(behind,0.03f)));
    const float magnification=std::max(m_sight.magnification*vr::ScopeZoomScale(),1.f);
    Fmatrix camera; camera.identity();
    camera.k=direction;
    camera.i.crossproduct(world.j,direction).normalize_safe();
    camera.j.crossproduct(direction,camera.i);
    camera.c=window; camera.c.mad(direction,m_sight.front-m_sight.rear+0.03f);
    Frect part;
    const shared_str reticle=m_weapon.VrScopeReticle(&part);
    const float rect[4]={part.x1,part.y1,part.x2,part.y2};
    vr::SetScopeView(camera,apparent/magnification,apparent,reticle.size() ? reticle.c_str() : nullptr,
        part.width()>0.f ? rect : nullptr);
}

bool CVrWeaponInteraction::MagazineInHand(Fmatrix& root) const
{
    // The magazine the support hand has: guided on the well, or held.
    Fmatrix world;
    if (!m_profile.detachable || !Transform(world)) return false;
    if (m_mechanism.held>=0)
    {
        root=SolvedMagazine(world);
        return true;
    }
    return false;
}

bool CVrWeaponInteraction::AuthoredBones(int side, VrHandBones& bones) const
{
    // Right-handed holds only: a pose cannot be mirrored bone for bone.
    Fmatrix world;
    // Carried by the support hand alone after the weapon hand let go: that
    // hand as it held the fore-end (it was drawn open at the controller,
    // under the stock, and jumped back when the weapon hand took the grip).
    if (m_carried && m_carry_authored && side==HandOf(1))
    {
        if (!vr::AnimatedGrip() || !Mode() || !Transform(world) || m_hands_local.empty()) return false;
        Fmatrix hand=m_hand_in_item[0];
        hand.c.add(m_carry_slide);
        bones.visual=m_hands_visual_name;
        bones.hand.mul_43(world,hand);
        bones.locals=&m_hands_local;
        return true;
    }
    if (!vr::AnimatedGrip() || !Mode() || m_hand!=1 || m_carried || !Transform(world) || m_cloud.empty())
        return false;
    if (m_hands_local.empty()) return false;
    // An animated hand far from the grip is drawn by the arm IK instead.
    if (!m_palm_trusted) return false;
    bones.visual=m_hands_visual_name;
    if (side)
    {
        if (!m_authored[1].valid) return false;
        bones.hand.mul_43(world,m_hand_in_item[1]);
        bones.locals=&m_hands_local;
        return true;
    }
    // The left hand holds a magazine as the reload does, or the weapon as
    // the idle does.
    // Not a magazine taken out of the weapon: the hand holds it where it
    // closed on it, and the animation's hold would jump 10-20 cm off it.
    Fmatrix root;
    if (!m_maghand_local.empty() && !m_mag_grabbed && MagazineInHand(root))
    {
        bones.hand.mul_43(root,m_maghand_in_root);
        bones.locals=&m_maghand_local;
        return true;
    }
    if (!m_supporting || !m_authored[0].valid) return false;
    // The authored hand is on the fore-end: with the barrels open, only a
    // hand that holds them is drawn there, riding the hinge.
    if (BarrelsOpen() && !m_support_barrels) return false;
    if (BarrelsOpen())
    {
        Fmatrix moved, barrels;
        HingeWorld(world,moved);
        barrels.mul_43(moved,world);
        bones.hand.mul_43(barrels,m_hand_in_item[0]);
    }
    else
    {
        // Where the real hand closed on the fore-end, not only where the
        // idle holds it: held further forward, the arm was stretched to the
        // animation's place.
        Fmatrix hand=m_hand_in_item[0];
        hand.c.add(SupportSlide());
        bones.hand.mul_43(world,hand);
    }
    if (m_pumping) bones.hand.c.mad(world.k,-m_pump);
    bones.locals=&m_hands_local;
    return true;
}

bool CVrWeaponInteraction::AuthoredHand(int side, VrHandPose& pose) const
{
    // Only measured weapons are drawn with the first-person model the
    // animation was made for; NPC world models use a different mesh.
    Fmatrix world;
    const bool weapon_hand=side==m_hand;
    if (!vr::AnimatedGrip() || !Mode() || (!weapon_hand && !m_supporting) || !Transform(world) ||
        m_cloud.empty()) return false;
    if (!m_palm_trusted) return false;
    if (!weapon_hand && m_pumping) world.c.mad(world.k,-m_pump);
    // The fore-end hand rides the hinged barrels; one on the receiver is
    // not drawn on them.
    if (!weapon_hand && BarrelsOpen())
    {
        if (!m_support_barrels) return false;
        Fmatrix moved, barrels;
        HingeWorld(world,moved);
        barrels.mul_43(moved,world);
        world=barrels;
    }
    // A weapon passed to the support hand stays held the way that hand held it.
    const int source=weapon_hand && !m_carried ? 1 : 0;
    const AuthoredHandRef& ref=m_authored[source];
    if (!ref.valid) return false;
    const bool mirror=side!=source;
    const auto direction=[&](Fvector v) { if (mirror) v.x=-v.x; world.transform_dir(v); return v; };
    Fvector knuckles=ref.knuckles;
    if (mirror) knuckles.x=2.f*(m_profile.has_trigger_point ? m_profile.trigger_point.x : 0.f)-knuckles.x;
    if (!weapon_hand && !BarrelsOpen()) knuckles.add(SupportSlide());
    pose.knuckles=Point(world,knuckles);
    pose.basis[0]=direction(ref.basis[0]);
    pose.basis[1]=direction(ref.basis[1]);
    pose.basis[2].crossproduct(pose.basis[0],pose.basis[1]);
    for (int f=0;f<3;++f)
        for (int j=0;j<3;++j) pose.finger[f][j]=direction(ref.finger[f][j]);
    return true;
}

bool CVrWeaponInteraction::RearSupport() const
{
    if (!vr::AnimatedGrip() || m_cloud.empty()) return false;
    if (!m_authored[0].valid || !m_authored[1].valid) return false;
    return m_authored[0].palm.z<m_authored[1].palm.z-0.08f;
}
float CVrWeaponInteraction::SupportAlong(const Fvector& grip, const Fvector& muzzle,
    const Fvector& support)
{
    Fvector axis; axis.sub(muzzle,grip);
    if (axis.square_magnitude()<0.15f*0.15f) return 0.f;
    axis.normalize();
    return Fvector().sub(support,grip).dotproduct(axis);
}
int CVrWeaponInteraction::SupportNear(const Fvector& grip, const Fvector& muzzle,
    const Fvector& forward, const Fvector& support) const
{
    // Any weapon can be held in two hands, with no per-weapon data: the
    // support hand closing at the grip cups the weapon hand (2); anywhere on
    // the barrel line ahead of it, it takes the fore-end (1).
    if (support.distance_to(grip)<0.12f) return 2;
    Fvector axis; axis.sub(muzzle,grip);
    float length=axis.magnitude();
    if (length<0.15f) { axis=forward; length=0.6f; }
    else axis.div(length);
    Fvector to; to.sub(support,grip);
    const float along=to.dotproduct(axis);
    // A weapon held from behind (an RPG-7's rear grip) takes the support
    // hand up to 50 cm behind the weapon hand instead of ahead of it.
    if (RearSupport())
    {
        if (along>-0.08f || along<-0.5f) return 0;
        to.mad(axis,-along);
        return to.magnitude()<0.12f ? 1 : 0;
    }
    if (along<0.1f || along>length+0.05f) return 0;
    to.mad(axis,-along);
    return to.magnitude()<0.12f ? 1 : 0;
}
void CVrWeaponInteraction::HingePose(Fmatrix& hinge) const
{
    hinge.identity();
    if (!m_profile.break_action || m_break_angle<=0.f) return;
    // Barrels drop about the hinge's lateral (+X) axis: +angle turns +Z to -Y.
    Fvector axis; axis.set(1.f,0.f,0.f);
    hinge.rotation(axis,m_break_angle);
    Fvector moved; hinge.transform_dir(moved,m_profile.hinge_point);
    hinge.c.sub(m_profile.hinge_point,moved);
}
void CVrWeaponInteraction::HingeWorld(const Fmatrix& world, Fmatrix& moved) const
{
    Fmatrix hinge, inverse, local;
    HingePose(hinge);
    inverse.invert(world);
    local.mul_43(hinge,inverse);
    moved.mul_43(world,local);
}
float CVrWeaponInteraction::HingeAngle(const Fmatrix& world, const Fvector& point) const
{
    Fmatrix inverse; inverse.invert(world);
    Fvector local; inverse.transform_tiny(local,point);
    local.sub(m_profile.hinge_point);
    return atan2f(-local.y,local.z);
}
void CVrWeaponInteraction::ResetParts()
{
    // At rest: parts closed, the belt on the tray when the weapon holds
    // rounds. Nothing of this is saved; a loaded save starts here.
    for (int i=0;i<m_profile.part_count;++i) vr::ResetPart(m_profile.parts[i],m_parts[i]);
    m_belt=vr::BeltState{};
    m_belt.seated=!m_weapon.m_magazine.empty();
    m_parts_inserted=m_mechanism.inserted;
    m_parts_ready=true;
}
vr::ReloadFacts CVrWeaponInteraction::Facts() const
{
    vr::ReloadFacts facts;
    for (int i=0;i<m_profile.part_count;++i)
    { facts.open[i]=m_parts[i].open; facts.closed[i]=m_parts[i].closed; }
    facts.belt_seated=!m_profile.belt.used || m_belt.seated;
    facts.magazine_in=!m_profile.detachable || m_mechanism.inserted>=0;
    return facts;
}
bool CVrWeaponInteraction::RuleAllows(const vr::GateList& gates, LPCSTR what)
{
    int blocked=-1;
    if (vr::GatesPass(gates,Facts(),&blocked)) return true;
    if (Device.dwTimeGlobal-m_rule_logged>1000 && blocked>=0)
    {
        m_rule_logged=Device.dwTimeGlobal;
        const vr::Gate& gate=gates.gates[blocked];
        LPCSTR need=gate.kind==vr::GateBeltSeated ? "the belt on the feed tray" :
            gate.kind==vr::GateMagazineIn ? "the magazine in" : nullptr;
        string128 part;
        if (!need)
        {
            xr_sprintf(part,"the %s %s",m_profile.part_names[gate.part].c_str(),
                gate.kind==vr::GatePartOpen ? "open" : "closed");
            need=part;
        }
        Msg("* [VR weapon] %s: %s needs %s",m_profile.section.c_str(),what,need);
        vr::HandHaptic(HandOf(0),0.05f);
    }
    return false;
}
bool CVrWeaponInteraction::BeltHidden() const
{
    // The loose end hangs out of sight in the box until it is laid on the
    // tray or carried there.
    return m_profile.belt.used && m_profile.belt_free_bone.size() && !m_belt.seated && m_belt.hand<0;
}
bool CVrWeaponInteraction::UpdateParts(const Fmatrix& world, bool left_valid, const Fmatrix& left,
    bool grip, bool grip_edge)
{
    if (!m_profile.part_count && !m_profile.belt.used) return false;
    const int hand=HandOf(0);
    Fmatrix inverse; inverse.invert(world);
    Fvector local; inverse.transform_tiny(local,left.c);
    vr::PartInput in;
    in.tracked=left_valid; in.grip=grip;
    // A hand holding a magazine, racking or steadying the weapon takes nothing.
    in.grip_edge=grip_edge && m_mechanism.held<0 && !m_racking && !m_supporting && !m_shell_held;
    in.hand={local.x,local.y,local.z};
    in.dt=clampr(Device.fTimeDelta,0.f,0.1f);
    bool holding=false;
    for (int i=0;i<m_profile.part_count;++i)
    {
        const vr::PartDef& def=m_profile.parts[i];
        vr::PartState& state=m_parts[i];
        // The hand first, then the part settles (in that order).
        unsigned events=vr::StepPart(def,state,hand,in);
        events|=vr::SettlePart(def,state,in.dt);
        const vr::V3 a=vr::PartAnchor(def,state);
        const Fvector at=Point(world,Fvector().set(a.x,a.y,a.z));
        if (events&vr::PartGrabbed) vr::HandHaptic(hand,0.12f);
        if (events&vr::PartSlipped) vr::HandHaptic(hand,0.2f);
        if (events&vr::PartOpened) { PlayHandling(SoundCoverOpen,at); vr::HandHaptic(hand,0.2f); }
        if (events&vr::PartClosed) { PlayHandling(SoundCoverClose,at); vr::HandHaptic(hand,0.3f); }
        if (events&(vr::PartOpened|vr::PartClosed))
            Msg("* [VR weapon] %s: %s %s",m_profile.section.c_str(),m_profile.part_names[i].c_str(),
                events&vr::PartOpened ? "open" : "closed");
        if (state.hand==hand) { holding=true; in.grip_edge=false; }
    }
    if (m_profile.belt.used)
    {
        const bool may=vr::GatesPass(m_profile.rules.belt,Facts());
        const unsigned events=vr::StepBelt(m_profile.belt,m_belt,hand,in,may,may);
        const Fvector tray=Point(world,Fvector().set(m_profile.belt.tray.x,m_profile.belt.tray.y,m_profile.belt.tray.z));
        if (events&vr::BeltTaken) vr::HandHaptic(hand,0.12f);
        if (events&vr::BeltDropped) vr::HandHaptic(hand,0.06f);
        if (events&vr::BeltSeated)
        {
            PlayHandling(SoundBeltIn,tray);
            vr::HandHaptic(hand,0.25f);
            Msg("* [VR weapon] %s: belt on the feed tray",m_profile.section.c_str());
        }
        if (m_belt.hand==hand) holding=true;
    }
    return holding;
}
bool CVrWeaponInteraction::PoseParts(IKinematics& model, xr_vector<Fmatrix>& poses) const
{
    const u16 count=model.LL_BoneCount();
    bool moved=false;
    const auto apply=[&](const vr::PartDef& def, float value, u16 bone)
    {
        float m[3][3];
        vr::PartRotation(def,value,m);
        Fmatrix motion; motion.identity();
        motion.i.set(m[0][0],m[0][1],m[0][2]);
        motion.j.set(m[1][0],m[1][1],m[1][2]);
        motion.k.set(m[2][0],m[2][1],m[2][2]);
        const vr::V3 at=vr::PartMove(def,value,def.pivot);
        Fvector pivot; pivot.set(def.pivot.x,def.pivot.y,def.pivot.z);
        Fvector turned; motion.transform_dir(turned,pivot);
        motion.c.set(at.x,at.y,at.z).sub(turned);
        for (u16 id=0;id<count && id<poses.size();++id)
            if (Descends(model,id,bone))
            {
                Fmatrix pose; pose.mul_43(motion,poses[id]);
                poses[id]=pose;
            }
        moved=true;
    };
    for (int i=0;i<m_profile.part_count;++i)
    {
        if (_abs(m_parts[i].value)<1e-5f) continue;
        const u16 bone=model.LL_BoneID(m_profile.part_bones[i].c_str());
        if (bone<count) apply(m_profile.parts[i],m_parts[i].value,bone);
    }
    // A carried belt end swings about its first link toward the hand.
    const u16 free=m_profile.belt_free_bone.size() ? model.LL_BoneID(m_profile.belt_free_bone.c_str()) : BI_NONE;
    if (m_belt.hand>=0 && free<count && free<poses.size())
    {
        const Fvector joint=poses[free].c;
        Fvector from, to;
        from.set(m_profile.belt.tray.x,m_profile.belt.tray.y,m_profile.belt.tray.z).sub(joint);
        to.set(m_belt.carried.x,m_belt.carried.y,m_belt.carried.z).sub(joint);
        if (from.square_magnitude()>EPS_S && to.square_magnitude()>EPS_S)
        {
            from.normalize(); to.normalize();
            Fvector axis; axis.crossproduct(from,to);
            const float sine=axis.magnitude();
            if (sine>1e-4f)
            {
                vr::PartDef swing;
                swing.kind=vr::PartHinge;
                swing.pivot={joint.x,joint.y,joint.z};
                // PartRotation turns right-handed about its axis: the cross
                // product here follows the engine's left-handed rule.
                axis.div(sine);
                const float angle=atan2f(sine,from.dotproduct(to));
                swing.axis={axis.x,axis.y,axis.z};
                const vr::V3 test=vr::PartMove(swing,angle,{joint.x+from.x,joint.y+from.y,joint.z+from.z});
                Fvector reached; reached.set(test.x,test.y,test.z).sub(joint);
                apply(swing,reached.dotproduct(to)>0.99f ? angle : -angle,free);
            }
        }
    }
    return moved;
}
void CVrWeaponInteraction::MeasurePartMotion(IKinematics& model)
{
    // A hinge without an axis in the profile takes it from the weapon's own
    // reload: play anm_reload (or anm_reload_empty) on a private copy and
    // find the frame that moves the part's grab point furthest. Points of
    // the part off its pivot give the axis (each moves square to it; the
    // bone's own axes, so they are never on one line), the bone's origin the
    // pivot, and the grab point's turn the opening angle.
    LPCSTR hud=m_weapon.HudSection().c_str();
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    std::unique_ptr<VrClipSampler> own_clips;
    for (int i=0;i<m_profile.part_count;++i)
    {
        if (!m_profile.part_from_animation[i]) continue;
        m_profile.part_from_animation[i]=false;
        vr::PartDef& def=m_profile.parts[i];
        const u16 bone=model.LL_BoneID(m_profile.part_bones[i].c_str());
        if (bone>=count || bone>=bind.size())
        {
            // Rules naming a missing part could never pass and would stop
            // the weapon firing: without its parts, it has no rules either.
            Msg("! [VR weapon] %s: part %s: bone %s missing; moving parts and reload rules disabled",
                m_profile.section.c_str(),m_profile.part_names[i].c_str(),m_profile.part_bones[i].c_str());
            m_profile.part_count=0;
            m_profile.rules=vr::ReloadRules{};
            m_profile.belt.used=false;
            // Nor are their bones VR's to pose (PrepareParts).
            for (auto& name:m_profile.part_bones) name=nullptr;
            m_profile.belt_free_bone=nullptr;
            break;
        }
        const Fvector grab{def.grab.x,def.grab.y,def.grab.z};
        def.pivot={bind[bone].c.x,bind[bone].c.y,bind[bone].c.z};
        // The grab point and three points along the bone's own axes, 10 cm
        // from its origin, in model space at rest.
        Fvector rest_points[4], local_points[4];
        rest_points[0]=grab;
        for (int a=0;a<3;++a)
        {
            Fvector offset{}; offset[a]=0.1f;
            bind[bone].transform_tiny(rest_points[a+1],offset);
        }
        {
            Fmatrix to_bind; to_bind.invert(bind[bone]);
            for (int n=0;n<4;++n) to_bind.transform_tiny(local_points[n],rest_points[n]);
        }
        // One sampler for every part (each made a copy of its own).
        VrClipSampler& clips=Clips(own_clips);
        float best=0.f;
        Fvector best_points[4]{};
        LPCSTR source="";
        if (clips.Bones()==count)
            for (LPCSTR key:{"anm_reload","anm_reload_empty"})
            {
                const VrSampledClip* clip=clips.Clip(key,360);
                if (!clip) continue;
                const int steps=ClipSteps(clip->length,360);
                for (int step=0;step<steps;++step)
                {
                    // Relative to the receiver: the whole gun moves in the animation.
                    Fmatrix placed=clip->At(step,bone);
                    const u16 parent=model.LL_GetData(bone).GetParentID();
                    if (parent<count)
                    {
                        Fmatrix rest; rest.mul_43(bind[parent],Fmatrix().invert(clip->At(step,parent)));
                        placed.mulA_43(rest);
                    }
                    Fvector moved[4];
                    for (int n=0;n<4;++n) placed.transform_tiny(moved[n],local_points[n]);
                    const float d=moved[0].distance_to(grab);
                    if (d>best) { best=d; for (int n=0;n<4;++n) best_points[n]=moved[n]; source=key; }
                }
                if (best>0.02f) break;
            }
        vr::V3 before[4], after[4];
        for (int n=0;n<4;++n)
        {
            before[n]={rest_points[n].x,rest_points[n].y,rest_points[n].z};
            after[n]={best_points[n].x,best_points[n].y,best_points[n].z};
        }
        const vr::V3 axis=vr::AxisFromMotion(before,after,4);
        if (best>0.02f && vr::Length(axis)>0.5f)
        {
            def.axis=axis;
            // The sign that opens it the way the animation does.
            float angle=vr::PartWrap(vr::PartCoordinate(def,after[0])-vr::PartCoordinate(def,def.grab));
            if (angle<0.f) { def.axis=(axis*-1.f); angle=-angle; }
            def.max=clampr(angle,deg2rad(20.f),deg2rad(120.f));
            def.open_at=0.8f*def.max;
            Msg("* [VR weapon] %s: part %s: hinge axis (%.2f %.2f %.2f) pivot (%.3f %.3f %.3f) open %.0f deg, from %s",
                m_profile.section.c_str(),m_profile.part_names[i].c_str(),def.axis.x,def.axis.y,def.axis.z,
                def.pivot.x,def.pivot.y,def.pivot.z,rad2deg(def.max),source);
        }
        else
            Msg("* [VR weapon] %s: part %s: no motion in the reload animation; hinge about +X at its bone, %.0f deg",
                m_profile.section.c_str(),m_profile.part_names[i].c_str(),rad2deg(def.max));
    }
}
Fvector CVrWeaponInteraction::ChamberWorld(const Fmatrix& world, int chamber) const
{
    Fmatrix hinge; HingePose(hinge);
    Fvector local; hinge.transform_tiny(local,m_profile.shell_center[chamber]);
    return Point(world,local);
}
bool CVrWeaponInteraction::ShellHandPose(Fmatrix& pose) const
{
    // Hold the shell in the left fingertips, oriented like the pistol grip,
    // with its centre slightly above the controller grip point.
    Fmatrix grip;
    if (!vr::HandTransform(HandOf(0),false,grip) || !vr::HandTransform(HandOf(0),true,pose)) return false;
    pose.c.set(0,0,0);
    Fvector centre=m_profile.shell_center[1];
    pose.transform_dir(centre);
    pose.c.mad(grip.c,pose.j,0.02f);
    pose.c.sub(centre);
    return true;
}
void CVrWeaponInteraction::SyncChambers()
{
    const unsigned rounds=std::min(unsigned(m_weapon.m_magazine.size()),2u);
    if (!m_break_initialized)
    {
        m_break_initialized=true;
        for (unsigned c=0;c<2;++c) m_chambers[c]=c<rounds ? 1 : 0;
        return;
    }
    // A native or scripted reload filled barrels; a shot spent the last
    // live barrel. Only the count is authoritative, so saves stay ordinary.
    unsigned live=unsigned(m_chambers[0]==1)+unsigned(m_chambers[1]==1);
    for (int c=0;c<2 && live<rounds;++c) if (m_chambers[c]!=1) { m_chambers[c]=1; ++live; }
    for (int c=1;c>=0 && live>rounds;--c) if (m_chambers[c]==1) { m_chambers[c]=2; --live; }
}
namespace vrwi
{
// A small loose box (a case, a magazine) thrown into the world from `pose`.
CPhysicsShell* CreateBoxShell(const Fvector& center, const Fvector& half, float mass,
    const Fmatrix& pose, const Fvector& velocity, const Fvector& angular)
{
    CPhysicsShell* shell=P_create_Shell();
    CPhysicsElement* element=P_create_Element();
    Fobb box; box.m_rotate.identity();
    box.m_translate=center;
    box.m_halfsize=half;
    element->add_Box(box); shell->add_Element(element);
    shell->setMass(mass);
    shell->set_PhysicsRefObject(nullptr);
    shell->Activate(pose,velocity,angular);
    shell->SetAirResistance(0.002f,0.01f);
    shell->set_DynamicLimits(5.f,20.f);
    shell->SetSmall(); shell->EnableCollision();
    return shell;
}
}
void CVrWeaponInteraction::PushProp(DropProp prop)
{
    // At most 8 lie about, each for 20 s: the oldest goes first.
    if (m_props.size()>=8) { RetireProp(m_props.front()); m_props.pop_front(); }
    prop.expires=Device.dwTimeGlobal+20000;
    m_props.push_back(prop);
}
void CVrWeaponInteraction::EjectShell(int chamber, const Fmatrix& world)
{
    if (!m_profile.shell_bones[chamber].size()) return;
    Fmatrix hinge; HingePose(hinge);
    Fmatrix pose; pose.mul_43(world,hinge);
    // The ejector throws the case up and back out of the open breech.
    Fvector velocity=m_hand_velocity[1];
    velocity.mad(world.j,0.9f).mad(world.k,-1.6f);
    DropProp prop;
    prop.shell=CreateBoxShell(m_profile.shell_center[chamber],m_profile.shell_box_half,0.03f,
        pose,velocity,m_hand_angular[1]);
    prop.part=m_profile.shell_bones[chamber];
    PushProp(prop);
}

void CVrWeaponInteraction::DropCase(const Fmatrix& world, const Fvector& velocity)
{
    // Stock pump and bolt guns (Remington 870, Mosin, MP-133) show the case
    // leaving in their shot animation; some keep shell particles parked off
    // the model (the MP-133's shell_point -50,-50,-50). Worked by hand in VR,
    // the model's own case is thrown from the port whenever it has one.
    static xr_set<xr_string> logged;
    const bool own_case=m_profile.case_bone.size()!=0;
    if (logged.insert(m_profile.section.c_str()).second)
        Msg("* [VR weapon] %s: spent case %s",m_profile.section.c_str(),
            own_case ? "thrown as the model's own case" :
            m_weapon.m_sShellParticles.size() ? "shown by shell particles" : "not shown (no case mesh, no particles)");
    if (!own_case)
    {
        Fvector port=m_profile.shell_point;
        if (port.square_magnitude()<EPS_S) port=m_profile.loading_port;
        m_weapon.OnShellDrop(Point(world,port),velocity);
        return;
    }
    Fvector half;
    half.set(std::max(m_profile.case_half.x,0.004f),std::max(m_profile.case_half.y,0.004f),
        std::max(m_profile.case_half.z,0.004f));
    // Up and out of the right-hand port.
    Fvector thrown=velocity;
    thrown.mad(world.j,0.6f).mad(world.i,0.5f);
    // Born just outside the receiver on the ejection side: inside it, the
    // held weapon's own body (vr_weapon_physics) threw the 30 g case out of
    // sight at once (MP-133).
    Fmatrix start=world;
    start.c.mad(world.i,std::max(0.02f,half.x+0.015f)).mad(world.j,0.01f);
    DropProp prop;
    prop.shell=CreateBoxShell(m_profile.case_center,half,0.03f,start,thrown,m_hand_angular[1]);
    if (!prop.shell)
    {
        Msg("! [VR weapon] %s: the spent case's physics shell was not created",m_profile.section.c_str());
        m_weapon.OnShellDrop(Point(world,m_profile.case_center),velocity);
        return;
    }
    prop.part=m_profile.case_bone;
    prop.own_box=true;
    prop.box_center=m_profile.case_center;
    prop.box_half=half;
    PushProp(prop);
}

bool VrNearSide(Fvector& point, float margin)
{
    Fvector head;
    if (!vr::HeadPosition(head)) return false;
    Fvector direction; direction.sub(point,head);
    const float distance=direction.magnitude();
    collide::rq_result wall;
    if (distance<EPS_S || !Level().ObjectSpace.RayPick(head,direction.div(distance),distance,
        collide::rqtStatic,wall,nullptr)) return false;
    point.mad(head,direction,_max(0.f,wall.range-margin));
    return true;
}

bool CVrWeaponInteraction::FireDependencies(firedeps& deps) const
{
    Fmatrix world;
    Fvector direction;
    if (Transform(world))
    {
        deps.vLastFP=Point(world,m_profile.muzzle);
        deps.vLastFP2=deps.vLastFPSilencer=deps.vLastFP;
        deps.vLastSP=Point(world,m_profile.shell_point);
        // Along the bore: zeroing waits for a hands-on sight adjustment.
        direction=world.k;
    }
    else if (GenericTransform(world))
    {
        // Unprofiled weapons fire from their tracked world model, using the
        // same third-person points as an NPC rather than the hidden HUD.
        deps.vLastFP=Point(world,m_weapon.vLoadedFirePoint);
        deps.vLastFP2=Point(world,m_weapon.vLoadedFirePoint2);
        deps.vLastFPSilencer=Point(world,m_weapon.vLoadedFirePointSilencer);
        deps.vLastSP=Point(world,m_weapon.vLoadedShellPoint);
        direction=world.k;
    }
    else return false;
    // With a physical weapon, a barrel pushed into a wall fires into that
    // wall, not from its far side.
    if (vr::PhysicalWeapon())
    {
        VrNearSide(deps.vLastFP,0.02f);
        VrNearSide(deps.vLastFP2,0.02f);
        VrNearSide(deps.vLastFPSilencer,0.02f);
    }
    deps.vLastFD=direction;
    deps.m_FireParticlesXForm=world;
    deps.m_FireParticlesXForm.c=deps.vLastFP;
    return true;
}

bool CVrWeaponInteraction::ValidateModel(IKinematics& model)
{
    // Parts that only change how the weapon is drawn or worked: a name the
    // model does not have (an audited profile's, on another asset) is
    // dropped with one line, and only that part is lost.
    const auto optional=[&](shared_str& name, LPCSTR what)
    {
        if (!name.size() || model.LL_BoneID(name.c_str())!=BI_NONE) return;
        Msg("! [VR weapon] %s: missing %s bone %s; left as the model shows it",m_profile.section.c_str(),
            what,name.c_str());
        name="";
    };
    optional(m_profile.rounds_bone,"rounds");
    optional(m_profile.handle_bone,"charging handle");
    optional(m_profile.pump_bone,"pump");
    optional(m_profile.dust_cover_bone,"dust cover");
    optional(m_profile.belt_free_bone,"belt end");
    for (shared_str& name:m_profile.hidden_parts) optional(name,"hidden part");
    // A moving part keeps its rules: its grab point still works it, only
    // its mesh does not move.
    for (int part=0;part<m_profile.part_count;++part)
        if (m_profile.part_bones[part].size() && model.LL_BoneID(m_profile.part_bones[part].c_str())==BI_NONE)
            Msg("! [VR weapon] %s: missing part bone %s; the part is worked but not drawn moving",
                m_profile.section.c_str(),m_profile.part_bones[part].c_str());
    if (!m_profile.detachable && !m_profile.cycle_travel && !m_profile.break_action) return true;
    if (m_profile.detachable && model.LL_BoneID(m_profile.mag_bone.c_str())==BI_NONE)
    {
        Msg("! [VR weapon] %s: missing magazine bone %s; physical magazine disabled",
            m_profile.section.c_str(),m_profile.mag_bone.c_str());
        m_profile.detachable=false;
    }
    if (m_profile.cycle_travel && m_profile.cycle_bone.size() &&
        model.LL_BoneID(m_profile.cycle_bone.c_str())==BI_NONE)
    {
        Msg("! [VR weapon] %s: missing cycling bone %s; cycling disabled",
            m_profile.section.c_str(),m_profile.cycle_bone.c_str());
        m_profile.cycle_travel=0;
        m_profile.detachable=false;
        m_profile.chamber=false;
    }
    for (const auto& name:m_profile.mag_extra_bones)
        if (name.size() && model.LL_BoneID(name.c_str())==BI_NONE)
        {
            Msg("! [VR weapon] %s: missing magazine part %s; physical magazine disabled",
                m_profile.section.c_str(),name.c_str());
            m_profile.detachable=false;
        }
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (m_profile.break_action)
    {
        const u16 hinge=model.LL_BoneID(m_profile.hinge_bone.c_str());
        bool shells=true;
        for (int c=0;c<2;++c)
        {
            Fbox box;
            // Barrels found by shape have no shell parts of their own.
            if (!m_profile.shell_bones[c].size()) continue;
            const u16 id=model.LL_BoneID(m_profile.shell_bones[c].c_str());
            if (id==BI_NONE || !MeasureBone(model,id,box,true)) { shells=false; continue; }
            box.getcenter(m_profile.shell_center[c]);
        }
        if (hinge!=BI_NONE && hinge<bind.size()) m_profile.hinge_point=bind[hinge].c;
        if (hinge==BI_NONE || !shells || m_profile.hinge_angle<=0.f)
        {
            Msg("! [VR weapon] %s: break-action bones missing; interactive loading disabled",
                m_profile.section.c_str());
            m_profile.break_action=false;
        }
    }
    if (m_profile.check_mag_bind && m_profile.detachable)
    {
        const u16 id=model.LL_BoneID(m_profile.mag_bone.c_str());
        if (id==BI_NONE || id>=bind.size() ||
            !Near(bind[id].c,m_profile.expected_mag_bind,m_profile.bind_tolerance))
        {
            Msg("! [VR weapon] %s: magazine bind differs from audited asset; native HUD retained",
                m_profile.section.c_str());
            return false;
        }
    }
    if (m_profile.check_cycle_bind && m_profile.cycle_travel)
    {
        const u16 id=model.LL_BoneID(m_profile.cycle_bone.c_str());
        if (id==BI_NONE || id>=bind.size() ||
            !Near(bind[id].c,m_profile.expected_cycle_bind,m_profile.bind_tolerance))
        {
            Msg("! [VR weapon] %s: cycling bind differs from audited asset; native HUD retained",
                m_profile.section.c_str());
            return false;
        }
    }
    return true;
}

void CVrWeaponInteraction::Pose(IKinematics& model, xr_vector<Fmatrix>& poses,
    bool magazine_only, bool rounds)
{
    const bool modular=m_weapon.VrModularAttachments();
    const bool scope=!modular && m_weapon.get_ScopeStatus()!=ALife::eAddonDisabled &&
        (!m_weapon.ScopeAttachable() || m_weapon.IsScopeAttached());
    const bool silencer=m_weapon.get_SilencerStatus()!=ALife::eAddonDisabled &&
        (!m_weapon.SilencerAttachable() || m_weapon.IsSilencerAttached());
    const bool launcher=m_weapon.get_GrenadeLauncherStatus()!=ALife::eAddonDisabled &&
        (!m_weapon.GrenadeLauncherAttachable() || m_weapon.IsGrenadeLauncherAttached());
    model.Callback(CollapseHiddenBones,nullptr);
    const bool pump=!m_profile.cycle_bone.size() && m_profile.pump_bone.size();
    vr::ApplyWeaponPose(model,poses,BonePose,bctCustom,
        pump ? m_profile.pump_bone.c_str() : m_profile.cycle_bone.c_str(),
        pump ? m_pump : (m_profile.cycle_from_reload ? m_handle_slide :
            std::max(m_mechanism.slide,m_cycle_visual_slide)),m_profile.rounds_bone.c_str(),
        {m_profile.mag_bone.c_str(),m_profile.rounds_bone.c_str()},magazine_only,
        m_mechanism.inserted>=0 || !m_profile.detachable,rounds,nullptr,
        {scope ? nullptr : "wpn_scope",silencer ? nullptr : "wpn_silencer",
            launcher ? nullptr : "wpn_launcher",
            modular && m_weapon.IsScopeAttached() ? "wpn_scoped_hide" : nullptr,
            modular && !m_weapon.IsScopeAttached() ? "wpn_scoped_unhide" : nullptr,
            m_profile.break_action && !m_chambers[0] ? m_profile.shell_bones[0].c_str() : nullptr,
            m_profile.break_action && !m_chambers[1] && m_profile.shell_bones[1]!=m_profile.shell_bones[0] ?
                m_profile.shell_bones[1].c_str() : nullptr,
            m_profile.hidden_parts[0].c_str(),m_profile.hidden_parts[1].c_str(),
            m_profile.hidden_parts[2].c_str(),BeltHidden() ? m_profile.belt_free_bone.c_str() : nullptr,
            m_scope_hidden[0].c_str(),m_scope_hidden[1].c_str(),m_scope_hidden[2].c_str()},
        {m_profile.mag_extra_bones[0].c_str(),m_profile.mag_extra_bones[1].c_str()},
        pump ? nullptr : m_profile.handle_bone.c_str(),m_handle_slide,m_profile.reload_magazines,false);
    // Magazine contents laid out as the idle shows them (belt links); a
    // seated magazine's then follow the weapon's own animation (AnimateParts).
    // Only on a skeleton laid out as the weapon's (not a dedicated asset).
    const u16 bones=model.LL_BoneCount();
    if (m_profile.rest_contents.size() && m_profile.rest_local.size()==bones && poses.size()>=bones &&
        Descends(model,m_profile.rest_contents.front(),model.LL_BoneID(m_profile.mag_bone.c_str())))
        for (u16 id:m_profile.rest_contents)
            poses[id].mul_43(poses[model.LL_GetData(id).GetParentID()],m_profile.rest_local[id]);
    // What the native first-person model hides now (a fired rocket or
    // launcher grenade, a round or shell its reload took away) is hidden on
    // this copy too: it kept showing them, as it never sees native changes.
    if (!magazine_only) CopyNativeVisibility(model);
    // The case a hand-worked action shows in its chamber leaves when the
    // hand opens it (DropCase throws it); kept, it sat still in an open
    // GAMMA TOZ-106.
    if (!magazine_only && m_profile.thrown_mask.size()==bones &&
        (m_pump>0.005f || m_mechanism.slide>0.005f || m_action_u>0.02f))
        for (u16 id=0;id<bones;++id)
            if (m_profile.thrown_mask[id] && model.LL_GetBoneVisible(id))
                model.LL_SetBoneVisible(id,FALSE,TRUE);
    if (!magazine_only) AnimateParts(model,poses);
    const u16 hinge_bone=magazine_only || !m_profile.break_action || m_break_angle<=0.f ?
        BI_NONE : model.LL_BoneID(m_profile.hinge_bone.c_str());
    if (!magazine_only) { PoseParts(model,poses); PoseActionPath(model,poses); PoseBeltFeed(model,poses); }
    // The bones are calculated once, after every pose and visibility change.
    if (hinge_bone!=BI_NONE)
    {
        Fmatrix hinge; HingePose(hinge);
        // Rounds not on the barrels' bone move with them too.
        const u16 round0=m_profile.shell_bones[0].size() ? model.LL_BoneID(m_profile.shell_bones[0].c_str()) : u16(BI_NONE);
        const u16 round1=m_profile.shell_bones[1].size() ? model.LL_BoneID(m_profile.shell_bones[1].c_str()) : u16(BI_NONE);
        for (u16 id=0;id<model.LL_BoneCount() && id<poses.size();++id)
            if (Descends(model,id,hinge_bone) || (round0!=BI_NONE && Descends(model,id,round0)) ||
                (round1!=BI_NONE && Descends(model,id,round1)))
            {
                Fmatrix moved; moved.mul_43(hinge,poses[id]);
                poses[id]=moved;
            }
    }
    model.CalculateBones_Invalidate();
    model.CalculateBones(true);
    vr::CollapseHiddenSkinning(model);
}

bool CVrWeaponInteraction::CopyNativeVisibility(IKinematics& model) const
{
    attachable_hud_item* item=m_weapon.IsAttachedToHUD() ? m_weapon.HudItemData() : nullptr;
    IKinematics* hud=item ? item->m_model : nullptr;
    const u16 count=model.LL_BoneCount();
    if (!hud || hud==&model || hud->LL_BoneCount()!=count) return false;
    // Parts VR shows itself keep its state: the magazine, its rounds and
    // shells loaded by hand.
    u16 owned[6]={BI_NONE,BI_NONE,BI_NONE,BI_NONE,BI_NONE,BI_NONE};
    const shared_str* names[6]={&m_profile.mag_bone,&m_profile.mag_extra_bones[0],&m_profile.mag_extra_bones[1],
        &m_profile.rounds_bone,&m_profile.shell_bones[0],&m_profile.shell_bones[1]};
    for (int n=0;n<6;++n)
        if (names[n]->size()) owned[n]=model.LL_BoneID(names[n]->c_str());
    // Except the rounds the game hides by its ammunition count (bullet_bones:
    // a GAMMA belt shortening as it fires), while the magazine is in.
    const bool seated=!m_profile.detachable || (m_mechanism.inserted>=0 && !m_inserting);
    xr_vector<u8> counted;
    if (seated && m_weapon.bHasBulletsToHide)
    {
        counted.assign(count,0);
        for (const shared_str& name:m_weapon.bullets_bones)
        {
            const u16 id=name.size() ? model.LL_BoneID(name.c_str()) : u16(BI_NONE);
            if (id<count) counted[id]=1;
        }
    }
    bool changed=false;
    for (u16 id=0;id<count;++id)
    {
        if (hud->LL_GetBoneVisible(id) || !model.LL_GetBoneVisible(id)) continue;
        bool vr_part=false, by_count=false;
        for (u16 a=id; a<count && !vr_part && !by_count; a=model.LL_GetData(a).GetParentID())
        {
            by_count=counted.size()==count && counted[a];
            for (u16 o:owned) vr_part=vr_part || a==o;
        }
        if (vr_part && !by_count) continue;
        model.LL_SetBoneVisible(id,FALSE,FALSE);
        changed=true;
    }
    return changed;
}

void CVrWeaponInteraction::PrepareParts(IKinematics& model)
{
    const u16 count=model.LL_BoneCount();
    if (m_overlay_order.size()==count) return;
    m_overlay_receiver.assign(count,0);
    m_overlay_owned.assign(count,0);
    for (u16 id=FindTrigger(model); id<count; id=model.LL_GetData(id).GetParentID())
        m_overlay_receiver[id]=true;
    m_overlay_receiver[model.LL_GetBoneRoot()]=true;
    // VR moves the magazine, shells and hinged barrels itself.
    for (const shared_str* name:{&m_profile.mag_bone,&m_profile.rounds_bone,&m_profile.mag_extra_bones[0],
        &m_profile.mag_extra_bones[1],&m_profile.shell_bones[0],&m_profile.shell_bones[1],
        &m_profile.hidden_parts[0],&m_profile.hidden_parts[1],&m_profile.hidden_parts[2],
        &m_profile.hinge_bone,&m_profile.belt_free_bone,&m_profile.part_bones[0],&m_profile.part_bones[1],
        &m_profile.part_bones[2],&m_profile.part_bones[3]})
        if (name->size())
        {
            const u16 id=model.LL_BoneID(name->c_str());
            if (id<count) m_overlay_owned[id]=true;
        }
    // Parents before children.
    xr_vector<u16> depth(count,0);
    m_overlay_order.clear();
    for (u16 id=0;id<count;++id)
    {
        for (u16 a=model.LL_GetData(id).GetParentID(); a<count && depth[id]<count; a=model.LL_GetData(a).GetParentID())
            ++depth[id];
        m_overlay_order.push_back(id);
    }
    std::sort(m_overlay_order.begin(),m_overlay_order.end(),[&](u16 a, u16 b) { return depth[a]<depth[b]; });
}

bool CVrWeaponInteraction::AnimateParts(IKinematics& model, xr_vector<Fmatrix>& poses)
{
    // Parts are posed relative to their parents, parents first, so each
    // rides on the VR pose of everything above it:
    // - triggers follow the finger (pulled pose sampled from the shot);
    // - a break-action shows the state of its loaded barrels the way its own
    //   idle animations do (cocking indicators, extractors), updated when the
    //   barrels close and as each barrel fires;
    // - other parts the first-person animation moves (bolt carrier, hammer,
    //   charging handle, dust cover) copy it, except the receiver chain that
    //   carries the trigger (the gun never kicks out of the hand) and what VR
    //   moves itself (magazine, shells, barrels, a known slide, bolt or pump).
    const u16 count=model.LL_BoneCount();
    if (poses.size()<count) return false;
    PrepareParts(model);
    IKinematics* hud=nullptr;
    if (m_weapon.IsAttachedToHUD())
        if (attachable_hud_item* item=m_weapon.HudItemData())
            if (item->m_model && item->m_model->LL_BoneCount()==count) hud=item->m_model;
    const u16 cycle=m_profile.cycle_bone.size() ? model.LL_BoneID(m_profile.cycle_bone.c_str()) : BI_NONE;
    const u16 pump=m_profile.pump_bone.size() ? model.LL_BoneID(m_profile.pump_bone.c_str()) : BI_NONE;
    const u16 magazine=m_profile.detachable ? model.LL_BoneID(m_profile.mag_bone.c_str()) : BI_NONE;
    // A separate charging handle is VR's: it moves only when worked by hand
    // (ApplyWeaponPose), never with the native idle copied over it.
    const u16 handle=m_profile.handle_bone.size() && !(!m_profile.cycle_bone.size() && m_profile.pump_bone.size()) ?
        model.LL_BoneID(m_profile.handle_bone.c_str()) : BI_NONE;
    const bool seated=m_mechanism.inserted>=0 && !m_inserting &&
        (!m_profile.belt.used || m_belt.seated);
    // A belt laid on the feed tray and let go feeds as the weapon's own shot
    // animation moves it; carried or hanging, its end is VR's.
    const u16 belt_end=m_profile.belt.used && m_profile.belt_free_bone.size() ?
        model.LL_BoneID(m_profile.belt_free_bone.c_str()) : BI_NONE;
    const bool belt_laid=belt_end!=BI_NONE && seated && m_belt.seated && m_belt.hand<0;
    const bool thrown=m_profile.thrown_mask.size()==count;
    const bool states=m_profile.break_action && m_state_local[0].size()==count &&
        m_state_local[1].size()==count && m_state_local[2].size()==count;
    // Until the barrels are first closed or fired, show what is loaded.
    const unsigned state=std::min(m_break_state_set ? m_break_state : unsigned(m_weapon.m_magazine.size()),2u);
    // The trigger the next shot uses: the second of a double gun once the
    // first barrel is spent.
    const int pulled=m_profile.break_action && m_chambers[0]!=1 && m_trigger_bone[1]!=BI_NONE ? 1 : 0;
    bool changed=false;
    for (u16 id:m_overlay_order)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        if (parent>=count) continue;
        int trigger=-1;
        for (int t=0;t<2;++t) if (id==m_trigger_bone[t] && m_trigger_valid[t]) trigger=t;
        if (trigger>=0)
        {
            const float amount=trigger==pulled ? m_trigger_value : 0.f;
            Fquaternion rest, full, blend;
            rest.set(m_bind_local[id]); full.set(m_trigger_pull[trigger]);
            blend.slerp(rest,full,amount);
            Fmatrix local; local.rotation(blend);
            local.c.lerp(m_bind_local[id].c,m_trigger_pull[trigger].c,amount);
            poses[id].mul_43(poses[parent],local);
            changed=true;
            continue;
        }
        if (states && m_state_part[id])
        {
            poses[id].mul_43(poses[parent],m_state_local[state][id]);
            changed=true;
            continue;
        }
        if (!hud || m_overlay_receiver[id]) continue;
        bool vr_driven=false;
        for (u16 a=id; a<count && !vr_driven; a=model.LL_GetData(a).GetParentID())
        {
            // Parts inside a seated magazine (a belt's links, a drum's
            // follower) move as the weapon's own animation moves them, as a
            // belt feeding on each shot; the magazine itself stays VR's.
            const bool inside_seated=(a==magazine && a!=id && seated) || (a==belt_end && belt_laid);
            vr_driven=(m_overlay_owned[a] && !inside_seated) || a==cycle || a==pump || a==handle ||
                (thrown && m_profile.thrown_mask[a]);
        }
        if (vr_driven) continue;
        Fmatrix to_parent; to_parent.invert(hud->LL_GetTransform(parent));
        Fmatrix local; local.mul_43(to_parent,hud->LL_GetTransform(id));
        poses[id].mul_43(poses[parent],local);
        changed=true;
    }
    return changed;
}

void CVrWeaponInteraction::MeasureParts(IKinematics& model)
{
    // Poses for parts VR moves from state rather than from the live
    // first-person animation, sampled from the weapon's own animations on a
    // private copy of its HUD model: each trigger at its furthest point in
    // the shot, and a break-action's idle for 0, 1 and 2 loaded barrels.
    const u16 count=model.LL_BoneCount();
    xr_vector<Fmatrix> bind;
    model.LL_GetBindTransform(bind);
    if (bind.size()<count) return;
    m_bind_local.assign(count,Fidentity);
    for (u16 id=0;id<count;++id)
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        if (parent>=count) { m_bind_local[id]=bind[id]; continue; }
        Fmatrix inverse; inverse.invert(bind[parent]);
        m_bind_local[id].mul_43(inverse,bind[id]);
    }
    m_trigger_bone[0]=FindTrigger(model);
    m_trigger_bone[1]=model.LL_BoneID("trigger2");
    m_trigger_valid[0]=m_trigger_valid[1]=false;
    m_break_state=unsigned(m_weapon.m_magazine.size());
    LPCSTR hud=m_weapon.HudSection().c_str();
    (void)hud;
    std::unique_ptr<VrClipSampler> own_clips;
    VrClipSampler& clips=Clips(own_clips);
    const auto local=[&](const VrSampledClip& clip, int at, u16 id) -> Fmatrix
    {
        const u16 parent=model.LL_GetData(id).GetParentID();
        Fmatrix result=clip.At(at,id);
        if (parent<count)
        {
            Fmatrix inverse; inverse.invert(clip.At(at,parent));
            result.mulA_43(inverse);
        }
        return result;
    };
    if (clips.Bones()==count)
    {
        float deviation[2]={0.002f,0.002f};
        for (LPCSTR key:{"anm_shots","anm_shot","anm_shot_1","anm_shot_2","anm_shot_l"})
        {
            const VrSampledClip* clip=clips.Frames(key,60);
            if (!clip) continue;
            for (int frame=0;frame<60;++frame)
            {
                for (int t=0;t<2;++t)
                {
                    const u16 id=m_trigger_bone[t];
                    if (id>=count) continue;
                    const Fmatrix posed=local(*clip,frame,id);
                    const Fmatrix& rest=m_bind_local[id];
                    const float turn=3.f-posed.i.dotproduct(rest.i)-posed.j.dotproduct(rest.j)-
                        posed.k.dotproduct(rest.k);
                    if (turn>deviation[t]) { deviation[t]=turn; m_trigger_pull[t]=posed; m_trigger_valid[t]=true; }
                }
            }
        }
        if (m_profile.break_action)
        {
            bool sampled=true;
            for (unsigned loaded=0;loaded<3 && sampled;++loaded)
            {
                string32 key; xr_sprintf(key,"anm_idle_%u",loaded);
                const VrSampledClip* clip=clips.Frames(key,1);
                sampled=clip!=nullptr;
                if (!sampled) break;
                m_state_local[loaded].resize(count);
                for (u16 id=0;id<count;++id) m_state_local[loaded][id]=local(*clip,0,id);
            }
            m_state_part.assign(count,0);
            const u16 hinge=model.LL_BoneID(m_profile.hinge_bone.c_str());
            int parts=0;
            for (u16 id=0;sampled && id<count;++id)
            {
                if (model.LL_GetData(id).GetParentID()>=count || id==hinge ||
                    id==m_trigger_bone[0] || id==m_trigger_bone[1]) continue;
                for (unsigned a=0;a<3 && !m_state_part[id];++a)
                    for (unsigned b=a+1;b<3 && !m_state_part[id];++b)
                    {
                        const Fmatrix& p=m_state_local[a][id];
                        const Fmatrix& q=m_state_local[b][id];
                        m_state_part[id]=p.c.distance_to(q.c)>0.0005f ||
                            3.f-p.i.dotproduct(q.i)-p.j.dotproduct(q.j)-p.k.dotproduct(q.k)>0.0005f;
                    }
                parts+=m_state_part[id];
            }
            if (!sampled) for (auto& states:m_state_local) states.clear();
            Msg("* [VR weapon] %s: %d parts follow the loaded barrels",m_profile.section.c_str(),parts);
        }
    }
}

void CVrWeaponInteraction::MeasureLauncher(IKinematics& model)
{
    // Where the support trigger selects an underbarrel launcher: its own
    // mesh, else under the barrel ahead of the trigger.
    BoneBounds part;
    for (u16 id=0;id<model.LL_BoneCount();++id)
        if (NameHas(model.LL_BoneName_dbg(id),"launcher")) model.EnumBoneVertices(part,id);
    if (part.count) part.box.getcenter(m_profile.launcher_grab);
    else
    {
        m_profile.launcher_grab=m_profile.has_trigger_point ? m_profile.trigger_point : m_profile.muzzle;
        if (m_profile.has_trigger_point) m_profile.launcher_grab.add(Fvector().set(0.f,-0.03f,0.22f));
    }
}

struct CVrWeaponInteraction::Measured
{
    Profile profile;
    bool basic_magazine=false;
    xr_vector<Fvector> cloud, optic_cloud;
    xr_vector<Fbox> glass, lens_meshes;
    bool glass_dot=false, glass_soft=false;
    shared_str mesh_shaders, scope_hidden[3];
    xr_vector<Fmatrix> bind_local, state_local[3];
    xr_vector<u8> state_part;
    u16 trigger_bone[2]{BI_NONE,BI_NONE};
    bool trigger_valid[2]{};
    Fmatrix trigger_pull[2];
};

xr_map<xr_string,std::shared_ptr<const CVrWeaponInteraction::Measured>>& CVrWeaponInteraction::MeasuredCache()
{
    static xr_map<xr_string,std::shared_ptr<const Measured>> cache;
    return cache;
}

xr_string CVrWeaponInteraction::MeasuredKey() const
{
    // Everything measuring reads from the weapon: its section (class, ltx,
    // HUD section and animations), the HUD model and the magazine size,
    // which an upgrade can change.
    string32 size; xr_sprintf(size,"%d",m_weapon.iMagazineSize);
    xr_string key=m_weapon.cNameSect().c_str();
    key.append("|").append(m_profile.visual.c_str()).append("|").append(size);
    return key;
}

void CVrWeaponInteraction::StoreMeasured(const xr_string& key) const
{
    auto measured=std::make_shared<Measured>();
    measured->profile=m_profile;
    measured->basic_magazine=m_basic_magazine;
    measured->cloud=m_cloud; measured->optic_cloud=m_optic_cloud;
    measured->glass=m_glass; measured->lens_meshes=m_lens_meshes;
    measured->glass_dot=m_glass_dot; measured->glass_soft=m_glass_soft;
    measured->mesh_shaders=m_mesh_shaders;
    for (int i=0;i<3;++i) { measured->scope_hidden[i]=m_scope_hidden[i]; measured->state_local[i]=m_state_local[i]; }
    measured->bind_local=m_bind_local;
    measured->state_part=m_state_part;
    for (int i=0;i<2;++i)
    {
        measured->trigger_bone[i]=m_trigger_bone[i];
        measured->trigger_valid[i]=m_trigger_valid[i];
        measured->trigger_pull[i]=m_trigger_pull[i];
    }
    MeasuredCache()[key]=measured;
}

bool CVrWeaponInteraction::RestoreMeasured(IKinematics& model, const xr_string& key)
{
    const auto found=MeasuredCache().find(key);
    if (found==MeasuredCache().end()) return false;
    const Measured& measured=*found->second;
    // Measured pristine, before any classification changed it.
    m_profile=measured.profile;
    m_basic_magazine=measured.basic_magazine;
    m_cloud=measured.cloud; m_optic_cloud=measured.optic_cloud;
    m_glass=measured.glass; m_lens_meshes=measured.lens_meshes;
    m_glass_dot=measured.glass_dot; m_glass_soft=measured.glass_soft;
    m_mesh_shaders=measured.mesh_shaders;
    for (int i=0;i<3;++i) { m_scope_hidden[i]=measured.scope_hidden[i]; m_state_local[i]=measured.state_local[i]; }
    m_bind_local=measured.bind_local;
    m_state_part=measured.state_part;
    for (int i=0;i<2;++i)
    {
        m_trigger_bone[i]=measured.trigger_bone[i];
        m_trigger_valid[i]=measured.trigger_valid[i];
        m_trigger_pull[i]=measured.trigger_pull[i];
    }
    // This weapon's own loaded barrels (MeasureParts).
    m_break_state=unsigned(m_weapon.m_magazine.size());
    // The contact geometry has its own cache: this only takes it up.
    BuildContact(model);
    return true;
}

VrClipSampler& CVrWeaponInteraction::Clips(std::unique_ptr<VrClipSampler>& local) const
{
    // EnsureVisual's sampler for its pass; outside one, the caller's own.
    std::unique_ptr<VrClipSampler>& owner=m_clip_scope ? *m_clip_scope : local;
    if (!owner) owner=std::make_unique<VrClipSampler>(m_profile.visual.c_str(),m_weapon.HudSection().c_str());
    return *owner;
}

bool CVrWeaponInteraction::EnsureVisual()
{
    if (m_visual) return true;
    m_visual=::Render->model_Create(m_profile.visual.c_str());
    if (!m_visual) { m_supported=false; return false; }
    if (auto* skeleton=m_visual->dcast_PKinematics())
    {
        CTimer measure; measure.Start();
        // Every motion measured in this pass is played once (VrClipSampler),
        // made only when something is measured.
        std::unique_ptr<VrClipSampler> clips;
        m_clip_scope=&clips;
        struct EndClips { std::unique_ptr<VrClipSampler>*& scope; ~EndClips() { scope=nullptr; } } end_clips{m_clip_scope};
        // A derived profile: as an earlier copy measured it, if one was.
        const bool derived=m_profile.automatic || m_profile.derived;
        const xr_string key=derived ? MeasuredKey() : xr_string();
        if (derived && RestoreMeasured(*skeleton,key))
            Msg("* [VR weapon] %s: measured as an earlier copy in %.1f ms",m_profile.section.c_str(),
                measure.GetElapsed_sec()*1000.f);
        else
        {
            if (m_profile.automatic) AutoProfile(*skeleton);
            else CacheReloadMagazines(*skeleton);
            MeasureLauncher(*skeleton);
            if (!ValidateModel(*skeleton))
            { ReleaseModels(); m_supported=false; return false; }
            MeasureGeometry(*skeleton);
            MeasureParts(*skeleton);
            MeasurePartMotion(*skeleton);
            Msg("* [VR weapon] %s: measured on first draw in %.1f ms",m_profile.section.c_str(),
                measure.GetElapsed_sec()*1000.f);
            if (derived) StoreMeasured(key);
        }
        // What the weapon is mechanically, each time it is measured (the
        // profile it sets is measured again with the model).
        ClassifyMechanics(*skeleton);
        if (!m_strategy_logged)
        {
            // How this weapon reloads in VR, decided once per model.
            m_strategy_logged=true;
            LPCSTR strategy=m_profile.part_count || m_profile.belt.used || !m_profile.rules.Empty() ?
                "interactive procedure" : m_profile.detachable ? "physical magazine" :
                (m_profile.break_action || m_profile.tube) ? "physical shells" : "native reload";
            Msg("* [VR weapon] %s: reload strategy: %s (parts %d, belt %d)",m_profile.section.c_str(),
                strategy,m_profile.part_count,m_profile.belt.used);
        }
    }
    else
    {
        Msg("! [VR weapon] %s: model is not skeletal",m_profile.section.c_str());
        ReleaseModels(); m_supported=false; return false;
    }
    return true;
}

xr_string CVrWeaponInteraction::MagazineVisualName(unsigned rounds) const
{
    if (!m_profile.magazine_visual_prefix.size()) return m_profile.visual.c_str();
    string_path name;
    xr_sprintf(name,"%s%u.ogf",m_profile.magazine_visual_prefix.c_str(),
        std::min(rounds,m_profile.magazine_visual_max_rounds));
    return name;
}

bool CVrWeaponInteraction::EnsureMagazineVisual(unsigned rounds)
{
    const unsigned variant=m_profile.magazine_visual_prefix.size() ?
        std::min(rounds,m_profile.magazine_visual_max_rounds) : 0;
    if (m_mag_visual && m_mag_visual_count==variant) return true;
    ReleaseModel(m_mag_visual);
    m_mag_visual_count=unsigned(-1);
    const xr_string name=MagazineVisualName(rounds);
    m_mag_visual=::Render->model_Create(name.c_str());
    if (!m_mag_visual || !m_mag_visual->dcast_PKinematics())
    {
        Msg("! [VR weapon] %s: magazine visual %s unavailable",m_profile.section.c_str(),name.c_str());
        ReleaseModel(m_mag_visual);
        return false;
    }
    m_mag_visual_count=variant;
    Msg("* [VR weapon] %s: magazine mesh %s for %u rounds",m_profile.section.c_str(),name.c_str(),rounds);
    return true;
}

unsigned CVrWeaponInteraction::PreviewRoundCount() const
{
    if (m_weapon.unlimited_ammo()) return unsigned(std::max(0,m_weapon.iMagazineSize));
    if (!m_weapon.m_pInventory) return 0;
    // The pouch's rounds fill it (FillMagazine); any type when there are none.
    unsigned available=0, chosen=0;
    const shared_str pouch=m_weapon.m_ammoTypes.empty() ? shared_str() : m_weapon.m_ammoTypes[PouchAmmo()];
    for (auto* item:m_weapon.m_pInventory->m_all)
        if (auto* box=smart_cast<CWeaponAmmo*>(item); box && box->m_boxCurr &&
            std::find(m_weapon.m_ammoTypes.begin(),m_weapon.m_ammoTypes.end(),
                box->cNameSect())!=m_weapon.m_ammoTypes.end())
        {
            available+=box->m_boxCurr;
            if (box->cNameSect()==pouch) chosen+=box->m_boxCurr;
        }
    return std::min(chosen ? chosen : available,unsigned(std::max(0,m_weapon.iMagazineSize)));
}

bool CVrWeaponInteraction::Render()
{
    if (!Mode())
    {
        // An unprofiled weapon must not fall through to the flat desktop HUD
        // arms. Render its third-person, arm-free model from the tracked grip
        // while preserving normal gameplay and ammunition handling.
        // Not the knife or binoculars: their first-person model is drawn as
        // the idle holds it (CHUDManager, VrKnifePose), the pose the knife's
        // strikes use. Their NPC models here pointed out of the fist the
        // wrong way.
        if (!g_vr_owns_present || !IsGameTypeSingle() || smart_cast<CWeaponKnife*>(&m_weapon) ||
            smart_cast<CWeaponBinoculars*>(&m_weapon) ||
            !vr::GameplayActive() || !m_weapon.Visual()) return false;
        auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
        if (!actor || actor!=Level().CurrentControlEntity() ||
            actor->inventory().ActiveItem()!=&m_weapon ||
            actor->cam_Active()!=actor->cam_FirstEye()) return false;
        if (!m_reported_generic)
        {
            Msg("* [VR weapon] %s: displaying tracked world model (physical VR interaction unavailable)",
                m_weapon.cNameSect().c_str());
            m_reported_generic=true;
        }
        Fmatrix world;
        if (!GenericTransform(world)) return false;
        vr::HideHandForFrame(HandOf(1));
        ::Render->set_Transform(&world);
        ::Render->add_Visual(m_weapon.Visual());
        return true;
    }
    Fmatrix world;
    if (!Transform(world)) return true;
    vr::HideHandForFrame(HandOf(1));
    if (m_mechanism.held>=0 || m_racking || m_supporting || m_shell_held)
        vr::HideHandForFrame(HandOf(0));
    if (!EnsureVisual()) return false;
    // A weapon measured here first (an audited profile is measured on its
    // first draw): its hands for the arms drawn after it.
    RefreshAuthoredHands();
    auto* model=m_visual->dcast_PKinematics();
    if (!model) return true;
    EnsurePosed(*model);
    ::Render->set_Transform(&world);
    ::Render->add_Visual(m_visual);
    Fmatrix shell_pose;
    if (ShellLoading() && m_shell_held &&
        (m_profile.shell_bones[1].size() || m_profile.loose_round_visual.size()) && ShellHandPose(shell_pose))
    {
        const bool loose=m_profile.loose_round_visual.size()!=0;
        if (!m_shell_visual) m_shell_visual=::Render->model_Create(
            loose ? m_profile.loose_round_visual.c_str() : m_profile.visual.c_str());
        if (auto* shell=m_shell_visual ? m_shell_visual->dcast_PKinematics() : nullptr)
        {
            shell->Callback(CollapseHiddenBones,nullptr);
            vr::ApplyWeaponPose(*shell,m_shell_poses,BonePose,bctCustom,nullptr,0.f,nullptr,
                {m_profile.shell_bones[1].c_str()},!loose,true,true);
            auto& vis=m_shell_visual->getVisData();
            vis.box.min.sub(m_profile.shell_center[1],m_profile.shell_box_half);
            vis.box.max.add(m_profile.shell_center[1],m_profile.shell_box_half);
            vis.box.getsphere(vis.sphere.P,vis.sphere.R);
            ::Render->set_Transform(&shell_pose);
            ::Render->add_Visual(m_shell_visual);
        }
    }
    else if (ShellLoading() && m_shell_held &&
        (m_pouch_visual || (m_weapon.m_ammoTypes.size() && [&]()
        {
            const shared_str& ammo=m_weapon.m_ammoTypes[PouchAmmo()];
            if (pSettings->line_exist(ammo,"visual"))
            {
                m_pouch_section=ammo;
                m_pouch_visual=::Render->model_Create(pSettings->r_string(ammo,"visual"));
            }
            return m_pouch_visual!=nullptr;
        }())))
    {
        // No round in the weapon's model (a launcher's grenade): the
        // ammunition's own model, in the support hand.
        Fmatrix hand;
        if (vr::HandTransform(HandOf(0),false,hand))
        {
            Fmatrix pose=hand;
            pose.c.mad(hand.k,0.09f);
            ::Render->set_Transform(&pose);
            ::Render->add_Visual(m_pouch_visual);
        }
    }
    if (m_profile.detachable && m_mechanism.held>=0)
    {
        // Placed on this frame's weapon pose (its pose is kept in weapon
        // space), so it cannot trail a moving gun.
        if (!HeldMagazineKnown()) return true;  // the hand is not tracked: not drawn
        Fmatrix pose=SolvedMagazine(world);
        const unsigned mag_count=m_mechanism.magazines[m_mechanism.held].empty() && m_pouch_preview ?
            PreviewRoundCount() : unsigned(m_mechanism.magazines[m_mechanism.held].size());
        if (!EnsureMagazineVisual(mag_count)) return true;
        auto* magazine=m_mag_visual->dcast_PKinematics();
        if (!magazine) return true;
        // The extracted PM mesh retains the stock skeletal hierarchy. Apply
        // the same audited bind pose as the equipped pistol: using its default
        // animation pose offsets the loose magazine from the well.
        Pose(*magazine,m_poses[1],true,mag_count>0);
        auto& vis=m_mag_visual->getVisData();
        vis.box.min.set(m_profile.mag_box_center.x-m_profile.mag_box_half.x,
            m_profile.mag_box_center.y-m_profile.mag_box_half.y,
            m_profile.mag_box_center.z-m_profile.mag_box_half.z);
        vis.box.max.set(m_profile.mag_box_center.x+m_profile.mag_box_half.x,
            m_profile.mag_box_center.y+m_profile.mag_box_half.y,
            m_profile.mag_box_center.z+m_profile.mag_box_half.z);
        vis.box.getsphere(vis.sphere.P,vis.sphere.R);
        ::Render->set_Transform(&pose);
        ::Render->add_Visual(m_mag_visual);
    }
    // A released magazine sliding out of the well.
    if (m_ejecting>=0 && m_eject_visual && m_contact && m_contact->path.Valid())
        if (auto* magazine=m_eject_visual->dcast_PKinematics())
        {
            Fmatrix pose; pose.mul_43(world,ToMatrix(m_contact->path.At(m_eject_s)));
            Pose(*magazine,m_eject_poses,true,!m_mechanism.magazines[m_ejecting].empty());
            auto& vis=m_eject_visual->getVisData();
            vis.box.min.sub(m_profile.mag_box_center,m_profile.mag_box_half);
            vis.box.max.add(m_profile.mag_box_center,m_profile.mag_box_half);
            vis.box.getsphere(vis.sphere.P,vis.sphere.R);
            ::Render->set_Transform(&pose);
            ::Render->add_Visual(m_eject_visual);
        }
    return true;
}

Fmatrix CVrWeaponInteraction::HolsterPose(CActor& actor) const
{
    const Fmatrix body=VrTorsoFrame(actor);
    Fmatrix pose; pose.identity();
    pose.i=body.i; pose.j=body.k; pose.k.set(0,-1,0);
    pose.c=HolsterPoint(actor);
    Fvector grip=m_profile.grip_offset;
    pose.transform_dir(grip);
    pose.c.sub(grip);
    return pose;
}
void CVrWeaponInteraction::RenderHolster()
{
    if (!g_vr_owns_present || !IsGameTypeSingle()) return;
    auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
    if (!actor || actor!=Level().CurrentControlEntity() || actor->inventory().ActiveItem()==&m_weapon) return;
    // Taken by a hand (from the floor or a holster) and not drawn yet: it is
    // already in that hand, never shown on the holster on its way there.
    const bool to_hand=(actor->m_vr_pickup.wield_id==m_weapon.ID() && !actor->m_vr_pickup.wield_stow) ||
        (m_hand_claimed && actor->inventory().GetNextActiveSlot()==m_weapon.CurrSlot());
    Fmatrix world;
    if (to_hand)
    {
        // An unprofiled weapon has no hand pose before it is drawn.
        // Measured first: the grip comes from the model.
        if (!Mode() || !EnsureVisual()) return;
        // Not drawn yet, so not yet updated as the weapon in the hand.
        RefreshAuthoredHands();
        if (!HeldPose(world)) return;
        vr::HideHandForFrame(HandOf(1));
    }
    else
    {
        if ((m_weapon.CurrSlot()!=INV_SLOT_2 && m_weapon.CurrSlot()!=INV_SLOT_3) ||
            actor->inventory().ItemFromSlot(m_weapon.CurrSlot())!=&m_weapon) return;
        world=HolsterPose(*actor);
    }
    // A gun without an audited physical profile still gets a visible world
    // model in its slot; only the advanced magazine/slide posing needs one.
    if (!Mode())
    {
        if (m_weapon.Visual())
        {
            ::Render->set_Transform(&world);
            ::Render->add_Visual(m_weapon.Visual());
        }
        return;
    }
    if (!EnsureVisual()) return;
    auto* model=m_visual->dcast_PKinematics();
    if (!model) return;
    EnsurePosed(*model);
    ::Render->set_Transform(&world);
    ::Render->add_Visual(m_visual);
}
void CVrWeaponInteraction::EnsurePosed(IKinematics& model)
{
    // Both eyes draw the same pose.
    if (m_posed_frame==Device.dwFrame && m_posed_inserted==m_mechanism.inserted) return;
    Pose(model,m_poses[0],false,m_weapon.m_magazine.size()>unsigned(m_mechanism.chamber));
    m_posed_frame=Device.dwFrame; m_posed_inserted=m_mechanism.inserted;
}

void CVrWeaponInteraction::ReleaseModels()
{
    m_authored_sampled=false;
    // Measured again with the model (MeasureOptics gives its glass).
    m_sight_measured=false;
    m_overlay_order.clear();
    m_bind_local.clear();
    for (auto& states:m_state_local) states.clear();
    ReleaseModel(m_visual);
    m_poses[0].clear();
    ReleaseTransientModels();
}
// The models of what the mechanism holds (magazines, the ejecting one, a
// shell, the pouch's rounds); the weapon's own model and everything measured
// on it stay.
void CVrWeaponInteraction::ReleaseTransientModels()
{
    m_posed_frame=u32(-1);
    ReleaseModel(m_mag_visual);
    ReleaseModel(m_eject_visual);
    ReleaseModel(m_shell_visual);
    ReleaseModel(m_pouch_visual);
    m_pouch_section=nullptr;
    m_shell_poses.clear();
    m_mag_visual_count=unsigned(-1);
    m_poses[1].clear();
}
void CVrWeaponInteraction::RetireProp(DropProp& prop)
{
    if (prop.shell) destroy_physics_shell(prop.shell);
    prop.shell=nullptr;
    ReleaseModel(prop.visual);
}
void CVrWeaponInteraction::ReleaseProps()
{
    for (auto& prop:m_props) RetireProp(prop);
    m_props.clear();
}
void CVrWeaponInteraction::Destroy()
{
    ReleaseProps(); ReleaseModels();
}

Fvector CVrWeaponInteraction::HolsterPoint(CActor& actor) const
{
    // Long guns over the shoulder, handguns on the hip, on the side of the
    // hand that holds the weapon: the rig mirrors with the hand, and the
    // ammunition pouch is on the free hand's hip (VrPouchPoint). A holstered
    // weapon stays on the side it was put away and is drawn from there; one
    // never held is on the dominant side. The side is not saved.
    Fvector point;
    float range=0.f;
    VrSlotHolster(actor,m_weapon,m_weapon.CurrSlot()==INV_SLOT_3 ? INV_SLOT_3 : INV_SLOT_2,HandOf(1)==1,point,range);
    return point;
}
float CVrWeaponInteraction::HolsterReach(CActor& actor) const
{
    Fvector point;
    float range=0.f;
    VrSlotHolster(actor,m_weapon,m_weapon.CurrSlot()==INV_SLOT_3 ? INV_SLOT_3 : INV_SLOT_2,HandOf(1)==1,point,range);
    return range;
}
bool CVrWeaponInteraction::HolsterGrabDistance(CActor& actor, const Fvector& hand,
    float& distance) const
{
    if (m_weapon.H_Parent()!=&actor ||
        (m_weapon.CurrSlot()!=INV_SLOT_2 && m_weapon.CurrSlot()!=INV_SLOT_3)) return false;
    distance=hand.distance_to(HolsterPoint(actor));
    return distance<=HolsterReach(actor);
}
bool CVrWeaponInteraction::HolsterPointFor(CActor& actor, Fvector& point, float& range) const
{
    // A knife or binoculars held as a weapon goes back to its body slot.
    if (VrBodySlotHolster(actor,m_weapon.CurrSlot(),point,range)) return true;
    if (m_weapon.CurrSlot()!=INV_SLOT_2 && m_weapon.CurrSlot()!=INV_SLOT_3) return false;
    point=HolsterPoint(actor);
    range=HolsterReach(actor);
    return true;
}
bool CVrWeaponInteraction::HolsterDistance(CActor& actor, float& distance, float& range) const
{
    Fmatrix hand;
    Fvector point;
    range=0.f;
    if (!vr::HandTransform(HandOf(1),false,hand) || !HolsterPointFor(actor,point,range)) return false;
    distance=hand.c.distance_to(point);
    return true;
}
bool CVrWeaponInteraction::WeaponReady() const
{
    const u32 state=m_weapon.GetState();
    return (state==CWeapon::eIdle || state==CWeapon::eMisfire) &&
        m_weapon.GetNextState()==state && !m_weapon.IsPending();
}
bool CVrWeaponInteraction::ReleaseWanted(CActor& actor, bool opened, bool open)
{
    // The rules are vr::ReleaseDecision (VrPickupPolicy.h): armed once clear
    // of the holster or held briefly; then an open hand at the holster puts
    // it back, opened elsewhere it leaves the hand. A weapon drawn without a
    // grip (a key, a loaded save) stays until the grip has held it once.
    // Every weapon is held only while its grip is (hold, never toggle).
    float distance=FLT_MAX, range=0.f;
    if (!HolsterDistance(actor,distance,range)) distance=FLT_MAX;
    if (!m_active_since) m_active_since=Device.dwTimeGlobal|1;
    const int held=int(Device.dwTimeGlobal-m_active_since);
    const vr::Release release=vr::ReleaseDecision(m_holster,distance,range,held>0 ? unsigned(held) : 0u,open,opened);
    if (release==vr::Release::None) return false;
    m_release_holster=release==vr::Release::Holster;
    return true;
}
void CVrWeaponInteraction::ResetHold()
{
    m_carried=false; m_holster={}; m_active_since=0; m_release_pending=false;
}
bool CVrWeaponInteraction::HandAtHolster(CActor& actor) const
{
    float distance=0.f, range=0.f;
    return m_holster.armed && HolsterDistance(actor,distance,range) && distance<=range;
}
bool CVrWeaponInteraction::UpdateRelease(CActor& actor, bool wanted, bool gripped)
{
    // Already on its way out (its hide runs): never let go of it twice.
    if (actor.inventory().GetNextActiveSlot()!=m_weapon.CurrSlot()) { m_release_pending=false; return false; }
    if (wanted && !m_release_pending)
    {
        m_release_since=Device.dwTimeGlobal|1;
        if (vr::ContactDebug()>0)
            Msg("* [VR release] %s: %s (state %u, next %u%s)",m_weapon.cNameSect().c_str(),
                m_release_holster ? "to the holster" : "out of the hand",m_weapon.GetState(),m_weapon.GetNextState(),
                m_weapon.IsPending() ? ", pending" : "");
    }
    if (wanted) m_release_pending=true;
    if (gripped) m_release_pending=false;
    if (!m_release_pending) return false;
    m_weapon.FireEnd();
    // Let go at once: at rest it is put away directly, otherwise through its
    // own hide or drop, which ends a shot's or a draw's animation cleanly.
    // Only the game's own reload or launcher switch is waited for. The
    // destination was fixed when the hand opened.
    const u32 state=m_weapon.GetState();
    const bool ready=WeaponReady();
    const bool native_busy=state==CWeapon::eReload || state==CWeapon::eSwitch ||
        m_weapon.GetNextState()==CWeapon::eReload;
    if (!vr::ReleaseNow(ready,native_busy)) return false;
    VrReleaseHeldWeapon(actor,m_weapon,m_release_holster ? 1 : 0,ready);
    if (vr::ContactDebug()>0)
        Msg("* [VR release] %s: %s after %d ms (%s)",m_weapon.cNameSect().c_str(),
            m_release_holster ? "holstered" : "let go",_max(0,int(Device.dwTimeGlobal-m_release_since)),
            ready ? "at rest" : "through its hide");
    m_release_pending=false;
    return true;
}
float CVrWeaponInteraction::MagazineGrabDistance(const Fmatrix& world, const Fvector& hand) const
{
    // The magazine's exposed surface, measured from its mesh (a curved or
    // raked magazine is taken where it is), within 6.5 cm of the hand's
    // grip centre: the fingers are round it.
    if (m_contact && !m_contact->grab_samples.empty())
    {
        Fmatrix inverse; inverse.invert(world);
        Fvector local; inverse.transform_tiny(local,hand);
        const vr::V3 at=V(local);
        float nearest=flt_max;
        for (const vr::V3& p:m_contact->grab_samples) nearest=std::min(nearest,vr::Dot(p-at,p-at));
        return nearest<=0.065f*0.065f ? _sqrt(nearest) : flt_max;
    }
    // Around the magazine's body below where it enters the weapon (a
    // pistol's grip hides all but the floorplate, taken 5 cm up it; a
    // rifle's magazine its whole length), from 10 cm under the floorplate:
    // long along the magazine, not wider. Within the controller's
    // grip centre's distance of the body when the fingers are round it. The
    // sphere round the floorplate this replaces sat inside the receiver of
    // many rifles, so a hand had to reach up through the weapon to take
    // their magazine.
    Fmatrix inverse; inverse.invert(world);
    Fvector local; inverse.transform_tiny(local,hand);
    local.sub(m_profile.mag_base);
    const Fvector& axis=m_profile.well_axis;
    const float along=local.dotproduct(axis);
    Fvector across=local; across.mad(axis,-along);
    const float length=2.f*m_profile.mag_box_half.y;
    const float exposed=Handgun() ? 0.05f : std::max(length,0.04f);
    const float body=std::max(m_profile.mag_box_half.x,m_profile.mag_box_half.z);
    const float girth=body+0.055f;
    if (along<-0.10f || along>exposed || across.magnitude()>girth) return flt_max;
    // From the body's side (a hand under the floorplate is on its axis).
    return std::max(0.f,across.magnitude()-body);
}
u8 CVrWeaponInteraction::PouchAmmo() const
{
    const size_t count=m_weapon.m_ammoTypes.size();
    if (!count) return 0;
    if (m_pouch_ammo<count && (m_weapon.unlimited_ammo() || !m_weapon.m_pInventory ||
        m_weapon.m_pInventory->GetAny(m_weapon.m_ammoTypes[m_pouch_ammo].c_str())))
        return m_pouch_ammo;
    const u8 next=m_weapon.m_set_next_ammoType_on_reload;
    return u8(std::min<size_t>(next<count ? next : m_weapon.m_ammoType,count-1));
}
bool CVrWeaponInteraction::SwapPouchAmmo()
{
    // As the desktop's next-ammunition key: the next type carried.
    const auto& types=m_weapon.m_ammoTypes;
    if (types.size()<2 || !m_weapon.m_pInventory) return false;
    const u8 current=PouchAmmo();
    for (size_t step=1;step<types.size();++step)
    {
        const u8 type=u8((current+step)%types.size());
        if (!m_weapon.unlimited_ammo() && !m_weapon.m_pInventory->GetAny(types[type].c_str())) continue;
        m_pouch_ammo=type;
        m_pouch_swap_time=Device.dwTimeGlobal;
        // A loaded spare magazine waiting in the pouch goes back into the
        // boxes: the next one taken is filled with the new rounds.
        for (int index=0;index<2;++index)
            if (index!=m_mechanism.inserted && index!=m_mechanism.held && !m_mechanism.magazines[index].empty())
            {
                ReturnCartridges(m_mechanism.magazines[index]);
                m_mechanism.magazines[index].clear();
            }
        // Shells loaded one by one and native reloads take the type the
        // desktop key would have set.
        if (!Mode() || !m_profile.detachable) m_weapon.m_set_next_ammoType_on_reload=type;
        Msg("* [VR weapon] %s: pouch now gives %s",m_weapon.cNameSect().c_str(),types[type].c_str());
        return true;
    }
    return false;
}
bool CVrWeaponInteraction::PouchAmmoShown(shared_str& section) const
{
    if (m_weapon.m_ammoTypes.empty() || !m_pouch_swap_time || Device.dwTimeGlobal-m_pouch_swap_time>1500) return false;
    section=m_weapon.m_ammoTypes[PouchAmmo()];
    return true;
}
bool CVrWeaponInteraction::GripField(int side, vr::ContactScene& scene, Fmatrix& world, float& ceiling,
    const void*& object, vr::GripKind& kind)
{
    scene.Clear();
    if (Transform(world))
    {
        if (!m_contact) return false;
        // A support hand holding a magazine closes on the magazine.
        if (side==HandOf(0) && m_mechanism.held>=0)
        {
            if (!HeldMagazineKnown()) return false;
            if (m_contact->magazine_surface.Empty()) return false;
            scene.Add(&m_contact->magazine_surface,ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),SolvedMagazine(world))));
            ceiling=flt_max;
            object=&m_contact->magazine_surface;
            kind=vr::GripKind::Magazine;
            return true;
        }
        MagazineScene(scene);
        ceiling=m_profile.bore_height;
        object=m_contact.get();
        kind=side==HandOf(1) ? vr::GripKind::PistolGrip :
            m_racking ? vr::GripKind::ChargingHandle : vr::GripKind::ForeEnd;
        return scene.count>0;
    }
    // An unmeasured weapon is held as its world model; its surface is
    // measured once per visual.
    if (!GenericTransform(world) || !m_weapon.Visual()) return false;
    if (m_world_contact_visual!=m_weapon.Visual())
    {
        static xr_map<shared_str,std::shared_ptr<const VrWeaponContact>> cache;
        m_world_contact_visual=m_weapon.Visual();
        const shared_str name=m_weapon.cNameVisual();
        auto found=cache.find(name);
        if (found!=cache.end()) m_world_contact=found->second;
        else if (auto* model=m_weapon.Visual()->dcast_PKinematics())
        {
            auto contact=std::make_shared<VrWeaponContact>();
            std::vector<vr::Tri> tris;
            TriangleCollector collector(tris);
            for (u16 id=0;id<model->LL_BoneCount();++id) model->EnumBoneVertices(collector,id);
            contact->receiver.Build(tris,0.0025f,0.012f,4);
            m_world_contact=cache[name]=contact;
        }
    }
    if (!m_world_contact || m_world_contact->receiver.Empty()) return false;
    scene.Add(&m_world_contact->receiver,vr::Frame());
    ceiling=m_weapon.vLoadedFirePoint.y-0.004f;
    object=m_world_contact.get();
    kind=side==HandOf(1) ? vr::GripKind::PistolGrip : vr::GripKind::ForeEnd;
    return true;
}
bool CVrWeaponInteraction::MagazineHandPose(int hand, Fmatrix& root, bool as_drawn) const
{
    // The magazine stands upright in the fist: its well axis follows the
    // controller's up and its front the pointing direction (aim pose), so a
    // hand held like the weapon hand presents it straight to the well, without
    // the forward lean of the controller handle. Its base sits at the handle
    // centre, so the whole magazine is above the fist and reaches the well
    // without lifting the hand into the weapon.
    Fmatrix grip, aim;
    if (!vr::HandTransform(hand,false,grip) || !vr::HandTransform(hand,true,aim)) return false;
    // Taken out of the weapon by hand: exactly where the hand closed on it,
    // held rigidly (a curved or raked magazine does not jump to a hold
    // point 10-20 cm off the fingers).
    if (m_mag_grabbed && !as_drawn && hand==HandOf(0)) { root.mul_43(grip,m_mag_in_hand); return true; }
    if (vr::AnimatedGrip() && hand==0 && m_hand==1 && !m_maghand_local.empty())
    {
        // The controller is the reload animation's left hand: its knuckles
        // face the controller's front (-Y), its index side up the handle
        // (+Z), its palm on the handle centre; the magazine sits in that hand
        // exactly as animated.
        Fvector frame[3];
        frame[0].invert(grip.j);
        frame[1]=grip.k;
        frame[1].mad(frame[0],-frame[1].dotproduct(frame[0])).normalize_safe();
        frame[2].crossproduct(frame[0],frame[1]);
        Fmatrix held; held.identity();
        Fvector* axes[3]={&held.i,&held.j,&held.k};
        for (int a=0;a<3;++a)
        {
            axes[a]->set(0,0,0);
            for (int b=0;b<3;++b) axes[a]->mad(frame[b],m_maghand_basis[b][a]);
        }
        Fvector palm; held.transform_dir(palm,m_maghand_palm);
        held.c.sub(grip.c,palm);
        Fmatrix to_hand; to_hand.invert(m_maghand_in_root);
        root.mul_43(held,to_hand);
        // The animation's hold is only used if the magazine is really in the
        // fist: its nearest surface within 3.5 cm of the hand's grip centre.
        // Some reloads carry a spare or swap parts (an AKS-74U's), and the
        // sampled hold then left the magazine floating off the hand.
        if (!m_contact || m_contact->magazine.samples.empty()) return true;
        float nearest=flt_max;
        for (const vr::V3& sample:m_contact->magazine.samples)
            nearest=std::min(nearest,Point(root,F(sample)).distance_to_sqr(grip.c));
        if (nearest<=0.035f*0.035f) return true;
    }
    const Fvector& axis=m_profile.well_axis;
    Fvector front; front.set(0.f,0.f,1.f);
    front.mad(axis,-front.dotproduct(axis)).normalize_safe();
    Fvector side; side.crossproduct(axis,front);
    Fvector up=aim.j, forward=aim.k;
    Fvector across; across.crossproduct(up,forward);
    root.identity();
    Fvector* rows[3]={&root.i,&root.j,&root.k};
    for (int row=0;row<3;++row)
    {
        rows[row]->set(0,0,0);
        rows[row]->mad(across,side[row]).mad(up,axis[row]).mad(forward,front[row]);
    }
    // Held by its body: the middle of its lowest 40% (a curved AK magazine's
    // base point, carried down its axis, lies off the magazine).
    Fvector body=m_profile.mag_base;
    if (m_contact && !m_contact->magazine.samples.empty())
    {
        float low=flt_max, high=-flt_max;
        for (const vr::V3& s:m_contact->magazine.samples)
        {
            const float along=F(s).dotproduct(axis);
            low=std::min(low,along); high=std::max(high,along);
        }
        Fvector sum; sum.set(0,0,0); unsigned used=0;
        for (const vr::V3& s:m_contact->magazine.samples)
            if (F(s).dotproduct(axis)<=low+0.4f*(high-low)) { sum.add(F(s)); ++used; }
        if (used) body=sum.div(float(used));
    }
    Fvector hold; root.transform_dir(hold,body);
    root.c.sub(grip.c,hold);
    // Held a little behind the handle centre, where the fingers close on it.
    root.c.mad(forward,-kMagazineHoldBack);
    return true;
}
void CVrWeaponInteraction::MagazineScene(vr::ContactScene& scene) const
{
    scene.Clear();
    if (!m_contact) return;
    scene.Add(&m_contact->receiver,vr::Frame());
    // The slide, bolt or pump where it is now (it moves back along -z).
    const bool pump=!m_profile.cycle_bone.size() && m_profile.pump_bone.size();
    vr::Frame moved;
    moved.c=vr::V3(0.f,0.f,-(pump ? m_pump : std::max(m_mechanism.slide,m_cycle_visual_slide)));
    scene.Add(&m_contact->cycle,moved);
}
bool CVrWeaponInteraction::HeldMagazineKnown() const
{
    Fmatrix pose;
    return (m_guide.active && m_contact) || MagazineHandPose(HandOf(0),pose);
}
Fmatrix CVrWeaponInteraction::SolvedMagazine(const Fmatrix& world) const
{
    Fmatrix pose;
    if (m_guide.active && m_contact) pose.mul_43(world,ToMatrix(m_guide.Pose(m_contact->path)));
    else if (!MagazineHandPose(HandOf(0),pose)) pose=world;
    return pose;
}
void CVrWeaponInteraction::SeatMagazine(const Fvector& sound_at)
{
    m_guide.active=false;
    m_inserting=false;
    if (m_mechanism.held<0) return;
    // A released magazine still in the well falls clear first: from the end
    // of its way out, not inside the well the new one is entering.
    DropEjecting(1.f);
    // One that never came half out goes back as it was: a tap on an empty
    // magazine is not a reload. One from the world holds what it holds.
    if (m_guide.armed && !m_mag_from_world) FillMagazine(m_mechanism.held);
    m_mag_from_world=false;
    if (!m_mechanism.magazines[m_mechanism.held].empty())
        m_weapon.m_ammoType=m_mechanism.magazines[m_mechanism.held].back().m_LocalAmmoType;
    if (m_mechanism.Insert(m_weapon.m_magazine))
    {
        m_pouch_preview=false;
        PlayHandling(SoundMagIn,sound_at);
        vr::HandHaptic(HandOf(0),0.25f);
        vr::HandHaptic(HandOf(1),0.15f);
    }
}
void VrContactTiming(int what, float microseconds, bool cached)
{
    struct Totals { double sum=0.0; float max=0.f; unsigned count=0, cached=0; };
    static Totals totals[2];
    static u32 since=0;
    Totals& t=totals[what ? 1 : 0];
    t.sum+=microseconds; t.max=std::max(t.max,microseconds); ++t.count; t.cached+=cached;
    if (!since) since=Device.dwFrame;
    if (Device.dwFrame-since<120) return;
    Msg("* [VR contact] over %u frames: magazine %u solves avg %.1f max %.1f us; fingers %u solves "
        "avg %.1f max %.1f us, %u%% cached",Device.dwFrame-since,
        totals[0].count,totals[0].count ? float(totals[0].sum/totals[0].count) : 0.f,totals[0].max,
        totals[1].count,totals[1].count ? float(totals[1].sum/totals[1].count) : 0.f,totals[1].max,
        totals[1].count ? totals[1].cached*100/totals[1].count : 0u);
    totals[0]=totals[1]=Totals();
    since=Device.dwFrame;
}
bool CVrWeaponInteraction::SupportHandOverride(Fmatrix& grip) const
{
    Fmatrix world, pose;
    for (int i=0;i<m_profile.part_count;++i)
        if (m_parts[i].hand==HandOf(0) && Transform(world) && vr::HandTransform(HandOf(0),false,grip))
        {
            // The hand stays on the handle, which moves on its own path; the
            // wrist keeps the tracked hand's turn, so taking it never snaps.
            const vr::V3 a=vr::PartAnchor(m_profile.parts[i],m_parts[i]);
            grip.c=Point(world,Fvector().set(a.x,a.y,a.z));
            return true;
        }
    if (m_racking && Transform(world) && vr::HandTransform(HandOf(0),false,grip))
    {
        // The hand stays on the slide or charging handle, moving with it; on
        // a bolt-action's knob, along its path (up through the turn, back).
        grip.c=Point(world,CycleGrabLocal());
        grip.c.mad(world.k,-CycleGrabTravel());
        return true;
    }
    if (m_supporting && AimTransform(world))
    {
        // The support hand stays on the weapon: around the grip below the
        // weapon hand (cup), or under the bore at the nearest point of the
        // barrel line (fore-end). Frames are controller-equivalent: +K up
        // the handle, -J toward the knuckles.
        Fmatrix hand;
        if (!vr::HandTransform(HandOf(1),false,hand)) return false;
        Fvector muzzle=Point(world,MuzzleLocal());
        Fvector axis; axis.sub(muzzle,hand.c);
        float length=axis.magnitude();
        if (length<0.15f) { axis=world.k; length=0.6f; }
        else axis.div(length);
        grip.identity();
        if (m_support_cup)
        {
            grip.k=world.j;
            grip.j.invert(world.k);
            grip.c=hand.c;
            grip.c.mad(world.j,-0.03f);
        }
        else
        {
            Fmatrix support;
            if (!vr::HandTransform(HandOf(0),false,support)) return false;
            // Where the hand closed on the barrel line; it stays there, the
            // palm cannot slide along the weapon while held.
            float along=RearSupport() ? clampr(m_support_along,-0.5f,-0.08f) : clampr(m_support_along,0.12f,length);
            // Riding the fore-end while it is pumped.
            if (m_pumping) along-=m_pump;
            grip.k=axis;
            grip.j.invert(world.j);
            grip.c=hand.c;
            grip.c.mad(axis,along).mad(world.j,-0.035f);
            // Round the mesh's fore-end where the hand closed on it.
            Fvector forend;
            if (ForendPoint(forend))
            {
                Fvector local; Fmatrix().invert(world).transform_tiny(local,Fvector().mad(hand.c,axis,along));
                local.x=forend.x; local.y=forend.y;
                grip.c=Point(world,local);
            }
        }
        grip.i.crossproduct(grip.j,grip.k);
        if (m_support_barrels && !m_support_cup && BarrelsOpen())
        {
            // The fore-end hand rides the hinged barrels.
            Fmatrix moved, barrels;
            HingeWorld(world,moved);
            barrels.mul_43(moved,grip);
            grip=barrels;
        }
        return true;
    }
    // A hand holding a magazine is the controller itself (the magazine
    // follows it exactly), reported so its fingers close on the magazine;
    // while the well has the magazine the hand rides it on the well's line.
    if (!m_profile.detachable || m_mechanism.held<0 || !vr::HandTransform(HandOf(0),false,grip)) return false;
    Fmatrix target;
    if (m_guide.active && Transform(world) && MagazineHandPose(HandOf(0),target))
    {
        Fmatrix correction, controller=grip;
        correction.mul_43(SolvedMagazine(world),Fmatrix().invert(target));
        grip.mul_43(correction,controller);
    }
    return true;
}

CPhysicsShell* CVrWeaponInteraction::CreateShell(int index, const Fmatrix& pose, int source_hand)
{
    if (index<0 || index>1) return nullptr;
    source_hand=clampr(source_hand,0,1);
    return CreateBoxShell(m_profile.mag_box_center,m_profile.mag_box_half,
        0.08f+0.01f*float(m_mechanism.magazines[index].size()),pose,m_hand_velocity[source_hand],m_hand_angular[source_hand]);
}
void CVrWeaponInteraction::DropInserted(const Fmatrix& world)
{
    // The catch releases: the magazine leaves the weapon at once, then
    // slides out along its way into the well (StepEject) and falls when
    // clear. Its rounds leave the inventory; the chamber round stays.
    // A second release before the first cleared: that one falls now, from
    // where it is.
    DropEjecting(m_eject_s);
    const int index=m_mechanism.EjectInserted(m_weapon.m_magazine);
    if (index<0) return;
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    PlayHandling(SoundMagOut,Point(world,m_profile.mag_base));
    vr::HandHaptic(HandOf(1),0.2f);
    if (m_contact && m_contact->path.Valid())
    {
        m_ejecting=index;
        m_eject_s=0.f;
        m_eject_speed=0.4f;
        m_eject_world=world;
        ReleaseModel(m_eject_visual);
        m_eject_visual=::Render->model_Create(MagazineVisualName(unsigned(m_mechanism.magazines[index].size())).c_str());
        return;
    }
    // No measured way out: it falls from its seat.
    Fvector out; out.invert(m_profile.well_axis);
    world.transform_dir(out);
    DropFromWell(index,world,Fvector().mad(m_hand_velocity[1],out.normalize_safe(),1.5f));
}

void CVrWeaponInteraction::DropEjecting(float along)
{
    if (m_ejecting<0) return;
    const int earlier=m_ejecting;
    m_ejecting=-1;
    Fmatrix sliding=m_eject_world;
    if (m_contact && m_contact->path.Valid()) sliding.mul_43(m_eject_world,ToMatrix(m_contact->path.At(along)));
    DropFromWell(earlier,sliding,m_hand_velocity[1]);
}
void CVrWeaponInteraction::StepEject()
{
    if (m_ejecting<0) return;
    const int index=m_ejecting;
    Fmatrix world;
    if (!m_contact || !m_contact->path.Valid() || !Transform(world))
    {
        // Put away or switched mid-slide: it falls where it was.
        m_ejecting=-1;
        Fmatrix pose=m_eject_world;
        if (m_contact && m_contact->path.Valid()) pose.mul_43(m_eject_world,ToMatrix(m_contact->path.At(m_eject_s)));
        DropFromWell(index,pose,Fvector().set(0.f,0.f,0.f));
        return;
    }
    m_eject_world=world;
    // Its spring pushes it out and gravity pulls it along the well; it
    // never stalls in it.
    const vr::InsertPath& path=m_contact->path;
    // Straight out of the well: the reload's path can curve away further on.
    Fvector out=F(path.Tangent(0.f));
    world.transform_dir(out);
    out.normalize_safe();
    const float dt=clampr(Device.fTimeDelta,0.f,0.05f);
    m_eject_speed=std::max(0.4f,m_eject_speed+(2.f-out.y*9.81f)*dt);
    m_eject_s+=m_eject_speed*dt/path.length;
    if (m_eject_s<vr::kMagazineEjectShare) return;
    m_ejecting=-1;
    Fmatrix pose; pose.mul_43(world,ToMatrix(path.At(vr::kMagazineEjectShare)));
    DropFromWell(index,pose,Fvector().mad(m_hand_velocity[1],out,m_eject_speed));
}

void CVrWeaponInteraction::DropFromWell(int index, const Fmatrix& pose, const Fvector& velocity)
{
    ReleaseModel(m_eject_visual);
    if (index<0 || index>1) return;
    CPhysicsShell* shell=CreateShell(index,pose,HandOf(1));
    if (shell)
    {
        shell->set_LinearVel(velocity);
        shell->set_AngularVel(Fvector().set(0.f,0.f,0.f));
        LeaveOnFloor(index,shell);
    }
    else ReturnCartridges(m_mechanism.magazines[index]);
    m_mechanism.magazines[index].clear();
    // The emptied magazine is the next one refilled from the pouch.
    m_mechanism.next_pouch=index;
}

void CVrWeaponInteraction::FinishDrop(int index, CPhysicsShell* shell)
{
    if (index<0 || index>1 || !shell)
    {
        if (shell) destroy_physics_shell(shell);
        m_mechanism.Stow();
        return;
    }
    if (m_basic_magazine)
    {
        // An unprofiled pistol's: empty (its rounds stayed in the
        // inventory), shown as a box for a while.
        DropProp prop;
        prop.shell=shell;
        prop.proxy=true;
        PushProp(prop);
    }
    else LeaveOnFloor(index,shell);
    m_mechanism.DiscardHeld();
    m_inserting=false; m_pouch_preview=false; m_guide.active=false;
}
void CVrWeaponInteraction::LeaveOnFloor(int index, CPhysicsShell* shell)
{
    xr_vector<CCartridge>& rounds=m_mechanism.magazines[index];
    const unsigned count=unsigned(rounds.size());
    IRenderVisual* visual=::Render->model_Create(MagazineVisualName(count).c_str());
    if (!visual || !visual->dcast_PKinematics())
    {
        // Nothing to see or take back: its rounds stay the player's.
        Msg("! [VR weapon] %s: magazine visual %s unavailable; %u rounds back to reserve",
            m_profile.section.c_str(),MagazineVisualName(count).c_str(),count);
        ReleaseModel(visual);
        destroy_physics_shell(shell);
        ReturnCartridges(rounds);
        rounds.clear();
        return;
    }
    if (g_floor_magazines.size()>=floor_magazine_limit)
        for (auto it=g_floor_magazines.begin();it!=g_floor_magazines.end();++it)
            if (it->hand<0)
            {
                Msg("* [VR weapon] floor magazine with %u rounds removed (more than %u)",
                    unsigned(it->cartridges.size()),unsigned(floor_magazine_limit));
                RetireFloorMagazine(*it);
                g_floor_magazines.erase(it);
                break;
            }
    g_floor_magazines.emplace_back();
    VrFloorMagazine& magazine=g_floor_magazines.back();
    magazine.shell=shell;
    magazine.visual=visual;
    magazine.cartridges.swap(rounds);
    rounds.clear();
    magazine.kind=MagazineKind();
    magazine.box_center=m_profile.mag_box_center;
    magazine.box_half=m_profile.mag_box_half;
    {
        Fmatrix grip, root;
        if (vr::HandTransform(HandOf(0),false,grip) && MagazineHandPose(HandOf(0),root,true))
        {
            magazine.hold_hand=HandOf(0);
            magazine.hold.mul_43(Fmatrix().invert(grip),root);
        }
    }
    magazine.expires=Device.dwTimeGlobal+floor_magazine_life;
    // Posed once, here: its bones read magazine.poses, which stays put in
    // the list. The weapon is not needed again.
    Pose(*visual->dcast_PKinematics(),magazine.poses,true,count>0);
    auto& vis=visual->getVisData();
    vis.box.min.sub(magazine.box_center,magazine.box_half);
    vis.box.max.add(magazine.box_center,magazine.box_half);
    vis.box.getsphere(vis.sphere.P,vis.sphere.R);
    Msg("* [VR weapon] %s: magazine left in the world with %u rounds (session only, %u s)",
        m_profile.section.c_str(),count,floor_magazine_life/1000);
}
shared_str CVrWeaponInteraction::MagazineKind() const
{
    if (!m_profile.detachable || m_basic_magazine) return nullptr;
    if (m_profile.magazine_visual_prefix.size()) return m_profile.magazine_visual_prefix;
    string_path kind;
    xr_sprintf(kind,"%s|%s",m_profile.visual.c_str(),m_profile.mag_bone.c_str());
    return kind;
}
bool CVrWeaponInteraction::TakeFloorMagazine(const shared_str& kind, xr_vector<CCartridge>& rounds)
{
    // The kind (a formatted, interned name) last: this is asked every frame.
    if (!Mode() || !m_mechanism.initialized || !kind.size() || m_holstered ||
        m_mechanism.held>=0 || m_ejecting>=0 || m_racking || m_shell_held || kind!=MagazineKind()) return false;
    for (const CCartridge& round:rounds)
        if (std::find(m_weapon.m_ammoTypes.begin(),m_weapon.m_ammoTypes.end(),round.m_ammoSect)==m_weapon.m_ammoTypes.end())
            return false;
    Fmatrix grip;
    if (!vr::HandTransform(HandOf(0),false,grip)) return false;
    xr_vector<CCartridge> displaced;
    if (!m_mechanism.TakeFromWorld(rounds,displaced)) return false;
    // The pouch magazine it takes the place of goes into the backpack.
    ReturnCartridges(displaced);
    // Held as one drawn from the pouch (it floated where the hand had closed
    // near it, up to 12 cm off, or 9 cm ahead when pulled).
    m_mag_grabbed=false;
    m_mag_from_world=true;
    m_supporting=m_support_cup=false;
    m_pouch_preview=false;
    m_guide.active=false; m_inserting=false;
    Msg("* [VR weapon] %s: magazine taken from the world (%u rounds)",m_profile.section.c_str(),
        unsigned(m_mechanism.magazines[m_mechanism.held].size()));
    return true;
}
void CVrWeaponInteraction::DropMagazine(int source_hand)
{
    // source_hand is a role (0 support, 1 weapon), as m_hand_velocity is.
    const int index=m_mechanism.held;
    if (index<0 || index>1) return;
    Fmatrix root, world;
    if (!HeldMagazineKnown()) { m_mechanism.Stow(); return; }
    if (Transform(world)) root=SolvedMagazine(world);
    else if (!MagazineHandPose(HandOf(clampr(source_hand,0,1)),root)) { m_mechanism.Stow(); return; }
    CPhysicsShell* shell=CreateShell(index,root,source_hand);
    if (!shell) { m_mechanism.Stow(); return; }
    shell->SetTransform(root,mh_not_clear);
    shell->set_LinearVel(m_hand_velocity[source_hand]);
    shell->set_AngularVel(m_hand_angular[source_hand]);
    FinishDrop(index,shell);
}
void CVrWeaponInteraction::RenderLooseMagazines()
{
    if (!g_vr_owns_present) return;
    for (auto& prop:m_props)
    {
        // Proxies are rendered by the VR eye overlay.
        if (!prop.shell || prop.proxy || !prop.part.size()) continue;
        if (!prop.visual) prop.visual=::Render->model_Create(m_profile.visual.c_str());
        if (!prop.visual) continue;
        auto* model=prop.visual->dcast_PKinematics();
        if (!model) continue;
        // A loose shell: the weapon's own HUD mesh showing only that part.
        model->Callback(CollapseHiddenBones,nullptr);
        vr::ApplyWeaponPose(*model,prop.poses,BonePose,bctCustom,nullptr,0.f,nullptr,
            {prop.part.c_str()},true,true,true);
        const int chamber=prop.part==m_profile.shell_bones[0] ? 0 : 1;
        const Fvector centre=prop.own_box ? prop.box_center : m_profile.shell_center[chamber];
        const Fvector half=prop.own_box ? prop.box_half : m_profile.shell_box_half;
        auto& vis=prop.visual->getVisData();
        vis.box.min.sub(centre,half);
        vis.box.max.add(centre,half);
        vis.box.getsphere(vis.sphere.P,vis.sphere.R);
        Fmatrix world; prop.shell->InterpolateGlobalTransform(&world);
        ::Render->set_Transform(&world);
        ::Render->add_Visual(prop.visual);
    }
}
void CVrWeaponInteraction::RenderBasicMagazines()
{
    if (!g_vr_owns_present) return;
    Fmatrix world;
    // Only the weapon in the hand: the others in the inventory have a pose
    // from the hand too, and their debug drew where nothing was held.
    const CActor* owner=smart_cast<const CActor*>(m_weapon.H_Parent());
    const bool in_hand=owner && owner->inventory().ActiveItem()==&m_weapon && !m_carried;
    if (vr::ContactDebug()>=2 && in_hand && Mode() && Transform(world))
    {
        // Where the hand takes the bolt or handle (white star), and the
        // magazine's surface it takes the seated magazine by (cyan dots).
        Fvector grab=Point(world,CycleGrabLocal());
        grab.mad(world.k,-CycleGrabTravel());
        const Fvector axes[3]={world.i,world.j,world.k};
        for (const Fvector& a:axes)
            vr::OverlayDraw().DrawLine(Fvector().mad(grab,a,-0.015f),Fvector().mad(grab,a,0.015f),0xffffffff);
        if (m_contact && m_mechanism.inserted>=0)
            for (const vr::V3& p:m_contact->grab_samples)
            {
                const Fvector at=Point(world,F(p));
                vr::OverlayDraw().DrawLine(at,Fvector().mad(at,world.j,0.002f),0xff40ffff);
            }
        // The other derived holds, to compare with the model: the trigger
        // (red), the weapon hand's grip centre (yellow), the support hand's
        // place (green) and the magazine's base (magenta).
        if (m_profile.has_trigger_point)
            vr::OverlayDraw().DrawCross(Point(world,m_profile.trigger_point),0.008f,0xffff3030);
        if (m_profile.has_hand_grip_center)
            vr::OverlayDraw().DrawCross(Point(world,m_profile.hand_grip_center),0.008f,0xffffff30);
        if (m_profile.two_handed)
            vr::OverlayDraw().DrawCross(Point(world,m_profile.support_offset),0.008f,0xff30ff30);
        if (m_profile.detachable)
            vr::OverlayDraw().DrawCross(Point(world,m_profile.mag_base),0.008f,0xffff40ff);
    }
    if (vr::ContactDebug()>=2 && in_hand && m_contact && m_contact->path.Valid() && Mode() && Transform(world))
    {
        // The magazine's way in (its feed lips' path) and the insertion
        // zone's ring at the well's mouth: blue, amber while the held
        // magazine is near it, green while it is in and lined up.
        const vr::InsertPath& path=m_contact->path;
        vr::InsertState state=m_guide.active ? vr::InsertTaken : vr::InsertOutside;
        if (m_mechanism.held>=0 && !m_guide.active)
        {
            Fmatrix held=SolvedMagazine(world);
            state=vr::InsertCheck(path,ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),held)),vr::InsertZone());
        }
        const u32 colour=state==vr::InsertTaken ? 0xff40ff40 : state==vr::InsertNear ? 0xffffb000 : 0xff40a0ff;
        // The full insertion guide only while a magazine can go in (held, or
        // the well empty): drawn over a seated or ejecting magazine it was
        // taken for the way out.
        const bool ejecting=m_ejecting>=0;
        if (!ejecting && (m_mechanism.held>=0 || m_guide.active || m_mechanism.inserted<0))
        {
            Fvector previous=Point(world,F(path.poses[0].Apply(path.lead)));
            for (size_t n=1;n<path.poses.size();++n)
            {
                const Fvector next=Point(world,F(path.poses[n].Apply(path.lead)));
                vr::OverlayDraw().DrawLine(previous,next,colour);
                previous=next;
            }
        }
        // Orange: the released magazine's short way out, ending exactly
        // where StepEject hands it to physics; shown with a magazine seated
        // or on its way out.
        if (ejecting || (m_mechanism.inserted>=0 && !m_guide.active))
        {
            Fvector previous=Point(world,F(path.At(0.f).Apply(path.lead)));
            for (int n=1;n<=12;++n)
            {
                const float s=vr::kMagazineEjectShare*float(n)/12.f;
                const Fvector next=Point(world,F(path.At(s).Apply(path.lead)));
                vr::OverlayDraw().DrawLine(previous,next,0xffff8040);
                previous=next;
            }
            vr::OverlayDraw().DrawCross(previous,0.006f,0xffff8040);
        }
        const float at=std::min(1.f,std::max(0.f,path.mouth));
        const vr::V3 tangent=path.Tangent(at);
        vr::V3 across=vr::Normalized(vr::Cross(tangent,std::abs(tangent.y)<0.9f ? vr::V3(0,1,0) : vr::V3(1,0,0)));
        const vr::V3 up=vr::Cross(tangent,across), mouth=path.At(at).Apply(path.lead);
        const float radius=vr::InsertZone().radius;
        for (int n=0;n<24;++n)
        {
            const float a=PI_MUL_2*n/24.f, b=PI_MUL_2*(n+1)/24.f;
            const vr::V3 p=mouth+(across*_cos(a)+up*_sin(a))*radius;
            const vr::V3 q=mouth+(across*_cos(b)+up*_sin(b))*radius;
            vr::OverlayDraw().DrawLine(Point(world,F(p)),Point(world,F(q)),colour);
        }
    }
    for (auto& prop:m_props)
    {
        if (!prop.proxy || !prop.shell) continue;
        Fmatrix world; prop.shell->InterpolateGlobalTransform(&world);
        Fvector corners[8];
        for (unsigned i=0;i<8;++i)
        {
            corners[i].set((i&1 ? 1.f:-1.f)*m_profile.mag_box_half.x,
                (i&2 ? 1.f:-1.f)*m_profile.mag_box_half.y,
                (i&4 ? 1.f:-1.f)*m_profile.mag_box_half.z);
            world.transform_tiny(corners[i]);
        }
        for (unsigned i=0;i<8;++i)
            for (unsigned axis=1;axis<=4;axis<<=1)
                if (!(i&axis)) vr::OverlayDraw().DrawLine(corners[i],corners[i|axis],0xffd0c0a0);
    }
}
void CVrWeaponInteraction::UpdateProps()
{
    StepEject();
    if (m_belt_feed>0.f)
    {
        m_belt_feed=std::max(0.f,m_belt_feed-Device.fTimeDelta/0.07f);
        // Back at rest: the next shot's step, from rest, with no jump.
        if (m_belt_feed<=0.f && m_belt_more) { m_belt_more=false; m_belt_feed=1.f; }
    }
    m_handle_hooked=m_handle_hooked && m_mechanism.locked;
    // A handle pulled on a locked bolt travels with the hand (the carrier
    // stays held open; the slight pull still releases the catch).
    const bool separate=m_profile.handle_bone.size() || m_profile.cycle_from_reload;
    if ((m_racking && !m_mechanism.locked) || m_handle_hooked) m_handle_slide=m_mechanism.slide;
    else if (m_racking && separate) m_handle_slide=m_handle_pull;
    else m_handle_slide=std::max(0.f,m_handle_slide-m_profile.cycle_travel*Device.fTimeDelta/0.08f);
    if (m_cycle_visual_slide>0.f && m_profile.cycle_travel>0.f)
        m_cycle_visual_slide=std::max(0.f,m_cycle_visual_slide-
            m_profile.cycle_travel*Device.fTimeDelta/0.11f);
    for (auto it=m_props.begin();it!=m_props.end();)
    {
        if (int(Device.dwTimeGlobal-it->expires)<0) { ++it; continue; }
        RetireProp(*it);
        it=m_props.erase(it);
    }
    if (Mode() && vr::GameplayActive() && m_mechanism.initialized)
        m_mechanism.Advance(Device.fTimeDelta,m_weapon.m_magazine,[&]()
        {
            Fvector velocity; m_weapon.PHGetLinearVell(velocity);
            m_weapon.OnShellDrop(m_weapon.get_LastSP(),velocity);
        });
    // Each lock and release of the slide, with what was there, in the log.
    if (m_mechanism.initialized && m_mechanism.locked!=m_slide_lock_logged)
    {
        m_slide_lock_logged=m_mechanism.locked;
        Msg("* [VR weapon] %s: slide %s (%u rounds in the gun, magazine %s, stop %d)",
            m_weapon.cNameSect().c_str(),m_mechanism.locked ? "locked open" : "released",
            unsigned(m_weapon.m_magazine.size()),m_mechanism.inserted>=0 ? "in" : "out",
            int(m_profile.slide_stop));
    }
}
bool CVrWeaponInteraction::LeftHandBusy() const
{
    return m_mechanism.held>=0 || m_racking ||
        m_supporting || m_shell_held || m_pumping;
}
float CVrWeaponInteraction::DetachedRoundWeight() const
{
    float weight=0;
    for (const auto& magazine:m_mechanism.magazines)
        for (const auto& round:magazine) weight+=round.Weight();
    return weight;
}
void CVrWeaponInteraction::ReleaseHands()
{
    // Both hands let go of everything they work on the weapon: the slide,
    // a magazine on its way in, the support grip; the controls' edges start
    // again from released.
    m_input.Reset();
    m_trigger=m_grip[0]=m_grip[1]=m_button=m_secondary=m_mag_trigger=false;
    m_release_pending=false;
    m_racking=m_racked=m_inserting=m_supporting=false;
    m_have_hand[0]=m_have_hand[1]=false;
    // A trigger held through this fires again only once let go.
    m_fire_trigger.Block();
}
void CVrWeaponInteraction::StowHands()
{
    // As ReleaseHands, with the weapon put away: a pulled slide returns
    // from where it is (a manual action stays where it is, as when let go)
    // and detached cartridges go back to their pouch.
    m_weapon.FireEnd();
    if (m_racking && !m_profile.manual_cycle) m_mechanism.ReleaseManualSlide(m_weapon.m_magazine);
    m_mechanism.Stow(true);
    m_guide.active=false;
    m_hand_epoch=0;
    ReleaseHands();
}
void CVrWeaponInteraction::ReleaseInactiveHeld()
{
    if (!Mode()) return;
    auto* actor=smart_cast<CActor*>(m_weapon.H_Parent());
    if (!actor || actor->inventory().ActiveItem()==&m_weapon) return;
    if (m_holstered && m_mechanism.held<0 && !m_racking &&
        !m_inserting && !m_supporting && !m_shell_held && !m_break_open) return;
    // Switching weapons ends every hand constraint. Keep detached cartridges
    // in their pouch and return a pulled slide from its current position
    // (a manual action stays where it is, as when let go).
    StowHands();
    m_holstered=true;
    m_pouch_preview=false;
    // A holstered break-action is carried closed; an unloaded shell goes back.
    m_break_open=m_lever_armed=m_shell_held=m_break_hand=false;
    m_break_angle=0.f;
    m_pumping=m_pump_back=false;
    m_pump=0.f;
}

bool CVrWeaponInteraction::Cycling() const
{
    return Mode() && m_profile.chamber && m_mechanism.cycling;
}
bool CVrWeaponInteraction::PumpSelfLoading() const
{
    return m_profile.tube && m_profile.pump_bone.size() && !m_profile.cycle_bone.size() &&
        vr::HasAutomaticFireMode(m_weapon.GetCurrentFireMode());
}
bool CVrWeaponInteraction::CanFire() const
{
    return !Mode() || (m_input.Armed() && !m_holstered && !m_racking &&
        vr::GameplayActive() && (!m_profile.chamber || m_mechanism.CanFire()) &&
        (!m_profile.break_action || (!m_break_open && m_break_angle<0.01f)) &&
        (!m_profile.pump_bone.size() || m_profile.cycle_bone.size() || (PumpSelfLoading() && m_pump<0.01f) ||
            (m_pump_ready && m_pump<0.01f)) &&
        // A manual action fires only closed and turned down.
        (!m_profile.manual_cycle || !m_profile.action_path.Valid() || m_action_u<0.02f) &&
        vr::GatesPass(m_profile.rules.fire,Facts()));
}
bool CVrWeaponInteraction::BlockReload() const
{
    return Mode() && m_profile.detachable && m_mechanism.initialized && !m_filling;
}
void CVrWeaponInteraction::PoseBeltFeed(IKinematics& model, xr_vector<Fmatrix>& poses) const
{
    // Each shot pulls the belt one link toward the tray: every link moves
    // to the next one's place, the end one a link further, and the belt
    // settles back as the fired round leaves it. The weapons' own shot
    // animations do not move the belt (PKM, RPD, M249).
    if (m_belt_feed<=0.f || !m_profile.belt.used || !m_belt.seated || m_belt.hand>=0) return;
    const u16 count=model.LL_BoneCount();
    const size_t links=m_profile.belt_chain.size();
    if (links<2 || poses.size()<count) return;
    xr_vector<u16> ids(links);
    for (size_t n=0;n<links;++n)
        if ((ids[n]=model.LL_BoneID(m_profile.belt_chain[n].c_str()))>=count) return;
    // A smooth push of half a link and back (no snap, no turn): each link
    // slides toward the next one's place and returns as the shot ends.
    const float u=0.5f*_sin(PI*(1.f-m_belt_feed));
    xr_vector<Fmatrix> delta(links);
    for (size_t n=0;n<links;++n)
    {
        const Fvector& from=poses[ids[n]].c;
        Fvector step=n+1<links ? Fvector().sub(poses[ids[n+1]].c,from) : Fvector().sub(from,poses[ids[n-1]].c);
        // A link far from the next (the belt's turn into the box) does not jump.
        if (step.magnitude()>0.03f) step.set(0.f,0.f,0.f);
        delta[n].identity();
        delta[n].c.mul(step,u);
    }
    // Each bone moves with the nearest link above it (the rounds on a
    // GAMMA RPD's mesh-less joints).
    for (u16 id=0;id<count;++id)
        for (u16 a=id; a<count; a=model.LL_GetData(a).GetParentID())
        {
            const auto at=std::find(ids.begin(),ids.end(),a);
            if (at==ids.end()) continue;
            Fmatrix moved; moved.mul_43(delta[size_t(at-ids.begin())],poses[id]);
            poses[id]=moved;
            break;
        }
}
void CVrWeaponInteraction::CartridgeFired()
{
    if (m_profile.belt.used) { if (m_belt_feed>0.f) m_belt_more=true; else m_belt_feed=1.f; }
    // Break-actions do not cycle; fired cases stay until the gun is opened.
    if (m_profile.break_action)
    {
        m_break_state=unsigned(m_weapon.m_magazine.size());
        m_break_state_set=true;
        return;
    }
    // A pump gun needs a manual stroke to eject the case and chamber again.
    if (m_profile.tube && m_profile.pump_bone.size() && !m_profile.cycle_bone.size())
    {
        // Set to its self-loading mode, the gun throws the case and
        // chambers the next shell by itself.
        Fmatrix world;
        if (PumpSelfLoading() && Transform(world))
        {
            Fvector velocity=m_hand_velocity[1];
            velocity.mad(world.i,0.8f);
            DropCase(world,velocity);
            m_pump_spent=false;
            m_pump_ready=!m_weapon.m_magazine.empty();
            m_shell_rounds=unsigned(m_weapon.m_magazine.size());
            return;
        }
        m_pump_ready=false;
        m_pump_spent=true;
        m_shell_rounds=unsigned(m_weapon.m_magazine.size());
        return;
    }
    // The first movement of the bolt swings a dust cover open.
    m_dust_open=true;
    // A bolt-action keeps its fired case until the hand works the bolt.
    if (m_profile.manual_cycle && m_mechanism.initialized)
    {
        m_mechanism.AfterShotManual();
        return;
    }
    if (DeferShellDrop() && m_mechanism.initialized)
    {
        m_mechanism.AfterShot(m_weapon.m_magazine);
        // Keep the shot's rearward slide travel visible for several headset
        // frames even when a long simulation step completes the native cycle.
        m_cycle_visual_slide=m_profile.bolt_travel>0.f ? m_profile.bolt_travel : m_profile.cycle_travel;
    }
}
bool CVrWeaponInteraction::ReserveAvailable() const
{
    if (m_weapon.unlimited_ammo()) return true;
    if (!m_weapon.m_pInventory) return false;
    for (auto* item:m_weapon.m_pInventory->m_all)
        if (auto* box=smart_cast<CWeaponAmmo*>(item); box && box->m_boxCurr)
            if (std::find(m_weapon.m_ammoTypes.begin(),m_weapon.m_ammoTypes.end(),box->cNameSect())!=m_weapon.m_ammoTypes.end())
                return true;
    return false;
}
void CVrWeaponInteraction::FillMagazine(int index)
{
    if (index<0 || index>1 || !m_mechanism.magazines[index].empty()) return;
    auto* weapon=m_weapon.cast_weapon_magazined();
    if (!weapon) return;
    xr_vector<CCartridge> live;
    live.swap(m_weapon.m_magazine);
    m_weapon.iAmmoElapsed=0;
    const u8 ammo_type=m_weapon.m_ammoType, next_type=m_weapon.m_set_next_ammoType_on_reload;
    const bool misfire=m_weapon.bMisfire, clear_jam=m_weapon.bClearJamOnly;
    const bool need_reload=weapon->m_needReload;
    m_weapon.m_set_next_ammoType_on_reload=CWeapon::undefined_ammo_type;
    // Filled with the rounds the pouch gives (SwapPouchAmmo); native loading
    // falls back on any other type carried when there are none of them.
    m_weapon.m_ammoType=PouchAmmo();
    // Borrow native cartridge loading without treating this magazine fill as
    // the player's jam-clearing reload. Cycling the action clears that state.
    m_weapon.bMisfire=m_weapon.bClearJamOnly=false;
    m_filling=true;
    weapon->ReloadMagazine();
    m_filling=false;
    m_mechanism.magazines[index].swap(m_weapon.m_magazine);
    live.swap(m_weapon.m_magazine);
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    m_weapon.m_ammoType=ammo_type;
    m_weapon.m_set_next_ammoType_on_reload=next_type;
    m_weapon.bMisfire=misfire;
    m_weapon.bClearJamOnly=clear_jam;
    weapon->m_needReload=need_reload;
}

void CVrWeaponInteraction::ReturnCartridges(const xr_vector<CCartridge>& rounds)
{
    // The owner's inventory: a weapon being dropped has already left it.
    auto* owner=smart_cast<CInventoryOwner*>(m_weapon.H_Parent());
    xr_vector<std::pair<shared_str,u32>> left;
    TopUpBoxes(owner,rounds,left);
    // One spawn per ammunition type: it splits by box size. With no owner
    // they land where the weapon is.
    const Fvector position=m_weapon.Position();
    for (const auto& entry:left)
        if (owner) m_weapon.SpawnAmmo(entry.second,entry.first.c_str());
        else m_weapon.SpawnAmmo(entry.second,entry.first.c_str(),0xffff,&position);
}
void CVrWeaponInteraction::ApplyMechanismLimits()
{
    m_mechanism.travel_limit=m_profile.travel_limit;
    m_mechanism.locks_on_empty=m_profile.locks_on_empty;
}
void CVrWeaponInteraction::Normalize(bool keep_measured)
{
    if (!m_mechanism.initialized) return;
    m_restore_empty_chamber=false;
    // Save and desktop weapon formats contain ordinary cartridges and ammo
    // boxes only. Prefer filling the weapon; return any excess to ammo boxes.
    // Spare magazines fill the weapon; one ammo type; and no more than the
    // desktop magazine holds (a chambered round on a full magazine is one
    // over): everything else goes back to ammo boxes (VrSaveLedger.h).
    xr_vector<CCartridge> back;
    const unsigned returned=vr::NormalizeRounds(m_weapon.m_magazine,m_mechanism.magazines,2,
        m_weapon.iMagazineSize>0 ? size_t(m_weapon.iMagazineSize) : size_t(-1),
        [](const CCartridge& a, const CCartridge& b) { return a.m_ammoSect==b.m_ammoSect; },
        [&](const CCartridge& c) { back.push_back(c); });
    ReturnCartridges(back);
    if (!m_weapon.m_magazine.empty()) m_weapon.m_ammoType=m_weapon.m_magazine.back().m_LocalAmmoType;
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
    m_parts_ready=false;
    if (returned) Msg("* [VR weapon] %s: %u spare rounds normalized",m_profile.section.c_str(),returned);
    // A save snapshot keeps the weapon's model and measurements: nothing they
    // come from changes, and releasing them re-measured every slot weapon on
    // the next frame (a hitch after each quick or auto save).
    ReleaseProps();
    if (keep_measured) ReleaseTransientModels();
    else ReleaseModels();
    m_mechanism={};
    ApplyMechanismLimits();
    m_mag_grabbed=m_mag_from_world=false;
    m_action_u=0.f;
    m_handle_hooked=false;
    // A magazine sliding out of the well went back with the rounds above;
    // left set, the next update dropped an empty copy of it.
    m_ejecting=-1;
    m_eject_s=0.f;
    m_cycle_visual_slide=0.f;
    ReleaseHands();
    m_holstered=false;
}
void CVrWeaponInteraction::PrepareSave(bool keep_measured)
{
    Normalize(keep_measured);
    m_restore_empty_chamber=false;
}
void CVrWeaponInteraction::PrepareSnapshot()
{
    if (!m_mechanism.initialized) return;
    const bool empty_chamber=!m_mechanism.chamber;
    Normalize(true);
    // The save contains only ordinary ammo state. A snapshot must not chamber
    // a round in the continuing VR session when the player had not racked it.
    m_restore_empty_chamber=empty_chamber && Mode();
}

void CVrWeaponInteraction::InteractShells(CActor& actor, const Fmatrix& world,
    bool left_valid, const Fmatrix& left, bool mag_hold, bool left_grip, bool left_trigger, bool secondary)
{
    if (m_profile.break_action)
    {
        SyncChambers();
        // B releases the top lever; a downward wrist flick within 0.6 s
        // breaks the barrels open. An upward flick snaps them shut.
        if (secondary && !m_secondary && !m_break_open)
        {
            m_lever_armed=true;
            m_lever_time=Device.dwTimeGlobal;
            vr::HandHaptic(HandOf(1),0.08f);
        }
        if (m_lever_armed && Device.dwTimeGlobal-m_lever_time>600) m_lever_armed=false;
        // A support hand holding the barrels carries them once the lever is
        // released or they are open: pulling it down breaks them open,
        // lifting it closes them. Where it holds is judged when it closes
        // (the palm cannot slide), so it keeps them until it lets go.
        const bool forend=m_supporting && !m_support_cup && m_support_barrels && left_valid && left_grip;
        if (forend && !m_break_hand && (m_lever_armed || m_break_open))
        {
            Fmatrix aim;
            if (vr::HandTransform(HandOf(1),true,aim))
            {
                aim.c.set(0,0,0);
                Fmatrix receiver=world; receiver.c.set(0,0,0);
                Fmatrix inverse; inverse.invert(aim);
                m_break_receiver.mul_43(inverse,receiver);
                m_break_hand=true;
                m_break_hand_ref=HingeAngle(world,left.c)-m_break_angle;
            }
        }
        if (m_break_hand && !forend) m_break_hand=false;
        const bool settled=Device.dwTimeGlobal-m_break_time>500;
        if (m_break_hand)
        {
            float angle=clampr(HingeAngle(world,left.c)-m_break_hand_ref,0.f,m_profile.hinge_angle);
            if (!m_break_open && angle<0.03f) angle=0.f;
            // The lever stays released while the hand is pulling the barrels.
            if (angle>0.f) m_lever_time=Device.dwTimeGlobal;
            m_break_angle=angle;
            if (!m_break_open && angle>m_profile.hinge_angle*0.15f)
            {
                m_break_open=true; m_lever_armed=false;
                // Opened, the barrels free a misfired shell, as a rack does a
                // magazine weapon's (VR-24): a full gun could not be cleared.
                m_weapon.bMisfire=m_weapon.bClearJamOnly=false;
                m_break_time=Device.dwTimeGlobal;
                m_weapon.FireEnd();
                PlayHandling(SoundBreakOpen,Point(world,m_profile.hinge_point));
                vr::HandHaptic(HandOf(0),0.25f);
            }
            else if (m_break_open && angle<0.03f && settled)
            {
                // Lifted shut: the lever catches.
                m_break_open=false;
                m_break_angle=0.f;
                m_break_hand=false;
                m_break_state=unsigned(m_weapon.m_magazine.size());
                m_break_state_set=true;
                m_break_time=Device.dwTimeGlobal;
                PlayHandling(SoundBreakClose,Point(world,m_profile.hinge_point));
                vr::HandHaptic(HandOf(0),0.3f);
                vr::HandHaptic(HandOf(1),0.2f);
            }
            else if (!m_break_open && !m_lever_armed) m_break_hand=false;
        }
        // The wrist's rebound after a flick turns the other way; the barrels
        // ignore flicks for 0.5 s after moving, and close only once fully open.
        // Flicks only work while no hand carries the barrels.
        const float pitch=m_hand_angular[1].dotproduct(world.i);
        if (m_break_hand) {}
        else if (!m_break_open && m_lever_armed && settled && pitch>5.f)
        {
            m_break_open=true; m_lever_armed=false;
            // Opened, the barrels free a misfired shell (VR-24).
            m_weapon.bMisfire=m_weapon.bClearJamOnly=false;
            m_break_time=Device.dwTimeGlobal;
            m_weapon.FireEnd();
            PlayHandling(SoundBreakOpen,Point(world,m_profile.hinge_point));
            vr::HandHaptic(HandOf(1),0.25f);
        }
        else if (m_break_open && settled && pitch< -6.f && m_break_angle>=m_profile.hinge_angle*0.95f)
        {
            m_break_open=false;
            m_break_state=unsigned(m_weapon.m_magazine.size());
            m_break_state_set=true;
            m_break_time=Device.dwTimeGlobal;
            PlayHandling(SoundBreakClose,Point(world,m_profile.hinge_point));
            vr::HandHaptic(HandOf(1),0.3f);
        }
        if (!m_break_hand)
        {
            const float target=m_break_open ? m_profile.hinge_angle : 0.f;
            const float step=Device.fTimeDelta*(m_break_open ? 7.f : 12.f);
            m_break_angle+=clampr(target-m_break_angle,-step,step);
        }
        // Fully open: the ejectors throw fired cases clear; live shells stay.
        if (m_break_open && m_break_angle>=m_profile.hinge_angle*0.9f)
            for (int c=0;c<2;++c)
                if (m_chambers[c]==2) { EjectShell(c,world); m_chambers[c]=0; }
    }
    else if (m_profile.pump_bone.size() && !m_profile.cycle_bone.size())
    {
        // A native reload refilled the tube and cycled the action.
        if (m_weapon.m_magazine.size()>m_shell_rounds) m_pump_ready=true;
        if (m_weapon.m_magazine.empty()) m_pump_ready=false;
        // Pump: the left hand closes on the fore-end, pulls it back the full
        // stroke (ejecting a fired case) and pushes it home (chambering).
        Fmatrix inverse; inverse.invert(world);
        Fvector local; inverse.transform_tiny(local,left.c);
        if (left_valid && left_grip && (!m_grip[0] || m_supporting) && !m_pumping && !PumpSelfLoading() &&
            Near(left.c,Point(world,m_profile.pump_grab),0.12f))
        {
            m_pumping=m_supporting=true;
            m_support_cup=false;
            m_pump_start=local.z+m_pump;
            Fmatrix weapon_hand;
            if (vr::HandTransform(HandOf(1),false,weapon_hand))
                m_support_along=SupportAlong(weapon_hand.c,Point(world,m_profile.muzzle),left.c)+m_pump;
        }
        if (m_pumping && left_valid && left_grip)
        {
            m_pump=clampr(m_pump_start-local.z,0.f,m_profile.pump_travel);
            if (vr::ContactDebug()>0 && Device.dwTimeGlobal-m_debug_time>250)
            {
                m_debug_time=Device.dwTimeGlobal;
                Msg("* [VR pump] %s: pump %.0f mm, hand on the barrel line at %.0f mm, supporting %d cup %d",
                    m_weapon.cNameSect().c_str(),m_pump*1000.f,m_support_along*1000.f,int(m_supporting),int(m_support_cup));
            }
            if (!m_pump_back && m_pump>=m_profile.pump_travel*0.85f)
            {
                m_pump_back=true;
                PlayHandling(SoundPumpBack,Point(world,m_profile.pump_grab));
                // A full stroke clears a jam, as a rack does (VR-24): with a
                // full tube no shell could be added to clear it.
                m_weapon.bMisfire=m_weapon.bClearJamOnly=false;
                if (m_pump_spent)
                {
                    Fvector velocity=m_hand_velocity[1];
                    velocity.mad(world.i,0.8f);
                    DropCase(world,velocity);
                    m_pump_spent=false;
                }
                m_pump_ready=false;
                vr::HandHaptic(HandOf(0),0.15f);
            }
            if (m_pump_back && m_pump<=0.008f)
            {
                m_pump_back=false;
                PlayHandling(SoundPumpForward,Point(world,m_profile.pump_grab));
                m_pump_ready=!m_weapon.m_magazine.empty();
                vr::HandHaptic(HandOf(0),0.2f);
            }
        }
        else if (m_pumping)
        {
            // Let go mid-stroke: the fore-end returns, completing a stroke
            // that already reached the back.
            m_pumping=false;
            if (m_pump_back) m_pump_ready=!m_weapon.m_magazine.empty();
            m_pump_back=false;
            m_pump=0.f;
        }
    }
    // One shell at a time from the pouch, with either support-hand control,
    // and only while the weapon has room for it.
    // A press of either control in the pouch (the other may still be held:
    // a hand that was squeezing the weapon's grip).
    const bool press=(left_trigger && !m_mag_trigger) || (left_grip && !m_grip[0]);
    if (!m_shell_held && !m_pumping && left_valid && press &&
        Near(left.c,VrPouchPoint(actor),vr_pouch_reach) && !VrHandHoldsItem(actor,HandOf(0)))
    {
        const bool has=m_weapon.VrHasShell(), room=int(m_weapon.m_magazine.size())<m_weapon.iMagazineSize;
        if (has && room) { m_shell_held=true; vr::HandHaptic(HandOf(0),0.12f); }
        else vr::HandHaptic(HandOf(0),0.04f);
        if (vr::ContactDebug()>0)
            Msg("* [VR shells] %s: pouch press: rounds in inventory %d, room %d (%u/%d)%s",m_weapon.cNameSect().c_str(),
                int(has),int(room),unsigned(m_weapon.m_magazine.size()),m_weapon.iMagazineSize,
                has && room ? "; round taken" : "");
    }
    if (m_shell_held && (!left_valid || !mag_hold)) m_shell_held=false;
    if (!m_shell_held) { m_shell_rounds=unsigned(m_weapon.m_magazine.size()); return; }
    Fmatrix shell_pose;
    const bool posed=(m_profile.shell_bones[1].size() || m_profile.loose_round_visual.size()) && ShellHandPose(shell_pose);
    const Fvector held=posed ? Point(shell_pose,m_profile.shell_center[1]) : left.c;
    if (m_profile.break_action)
    {
        if (m_break_open && m_break_angle>=m_profile.hinge_angle*0.8f)
            for (int c=0;c<2;++c)
            {
                if (m_chambers[c] || !Near(held,ChamberWorld(world,c),0.045f)) continue;
                m_chambers[c]=1;
                if (m_weapon.VrLoadShell())
                {
                    if (!m_weapon.VrPlayShellSound()) PlayHandling(SoundShellIn,held);
                    m_shell_held=false;
                    vr::HandHaptic(HandOf(0),0.25f);
                }
                else m_chambers[c]=0;
                break;
            }
    }
    else if (vr::ContactDebug()>0 && Device.dwTimeGlobal-m_debug_time>500 &&
        held.distance_to(Point(world,m_profile.loading_port))<0.15f && [&]()
        {
            m_debug_time=Device.dwTimeGlobal;
            Msg("* [VR shells] %s: round %.0f mm from the port; action open %d (bolt u %.2f, locked %d, slide %.0f mm)",
                m_weapon.cNameSect().c_str(),held.distance_to(Point(world,m_profile.loading_port))*1000.f,
                int(!m_profile.internal_rounds || (m_profile.manual_cycle ? m_action_u>=0.75f :
                    (m_mechanism.locked || m_mechanism.slide>=0.9f*m_profile.cycle_travel))),
                m_action_u,int(m_mechanism.locked),m_mechanism.slide*1000.f);
            return false;
        }()) {}
    if (!m_profile.break_action && int(m_weapon.m_magazine.size())<m_weapon.iMagazineSize &&
        // Through the open action: a bolt-action's bolt drawn fully back, a
        // self-loader's held open by the follower (or by the hand).
        (!m_profile.internal_rounds || (m_profile.manual_cycle ? m_action_u>=0.75f :
            (m_mechanism.locked || m_mechanism.slide>=0.9f*m_profile.cycle_travel))) &&
        Near(held,Point(world,m_profile.loading_port),0.07f) &&
        m_weapon.VrLoadShell())
    {
        if (!m_weapon.VrPlayShellSound()) PlayHandling(SoundShellIn,held);
        // Pressed into the loading port: one round from inventory joins the
        // tube. An empty chamber still needs a pump stroke.
        m_shell_held=false;
        vr::HandHaptic(HandOf(0),0.2f);
    }
    m_shell_rounds=unsigned(m_weapon.m_magazine.size());
}

void VrHandMotion(const Fmatrix& previous, const Fmatrix& current, const Fmatrix& body,
    const Fvector& carried, float dt, Fvector& linear, Fvector& angular)
{
    linear.sub(current.c,previous.c).div(dt);
    body.transform_dir(linear);
    linear.add(carried);
    Fvector axis;
    angular.crossproduct(previous.i,current.i);
    axis.crossproduct(previous.j,current.j); angular.add(axis);
    axis.crossproduct(previous.k,current.k); angular.add(axis);
    angular.mul(0.5f/dt);
    body.transform_dir(angular);
}

Fvector VrCarriedVelocity(CActor& actor)
{
    Fvector velocity; velocity.set(0,0,0);
    if (CCharacterPhysicsSupport* physics=actor.character_physics_support())
        if (physics->movement()) physics->movement()->GetCharacterVelocity(velocity);
    return velocity;
}

void CVrWeaponInteraction::TakeMagazineIntoHand(const Fmatrix& world, const Fmatrix& hand,
    const Fmatrix& magazine, float along, Fmatrix& held_root)
{
    // The support hand holds the magazine where it closed on it, and is no
    // longer on the weapon.
    m_supporting=m_support_cup=false;
    m_mag_grabbed=true;
    m_mag_in_hand.mul_43(Fmatrix().invert(hand),magazine);
    // It slides out along the well with the hand, from where it is (along
    // the insert path); it seats itself again only once drawn half out and
    // pushed back. The pose is taken again: the hand now holds it where it
    // closed on it.
    if (m_contact && m_contact->path.Valid() && MagazineHandPose(HandOf(0),held_root))
    {
        m_guide.TakeFromSeat(ToFrame(Fmatrix().mul_43(Fmatrix().invert(world),held_root)),m_contact->path,along);
        m_inserting=true;
        m_slide_tick=0.f;
    }
}
bool VrControlsFree(CActor& actor)
{
    return vr::GameplayActive() && actor.g_Alive() && !actor.Holder() && !actor.Remote() &&
        !actor.IsTalking() && actor.cam_Active()==actor.cam_FirstEye() &&
        !(CurrentGameUI() && CurrentGameUI()->TopInputReceiver());
}
int CVrWeaponInteraction::DetectSupport(const Fvector& weapon_hand, const Fvector& muzzle,
    const Fvector& axis, const Fvector& support)
{
    const int kind=SupportNear(weapon_hand,muzzle,axis,support);
    m_supporting=kind!=0;
    m_support_cup=kind==2;
    m_support_along=SupportAlong(weapon_hand,muzzle,support);
    if (m_supporting) vr::HandHaptic(HandOf(0),0.12f);
    return kind;
}
bool CVrWeaponInteraction::HandOverOnRelease(CActor& actor, bool weapon_grip, bool support_grip)
{
    // The weapon hand opened while the support hand holds the weapon (and
    // not at the holster, where opening puts it back): the support hand
    // carries it.
    return m_grip[1] && !weapon_grip && support_grip && m_supporting && !HandAtHolster(actor) && HandOver();
}
bool CVrWeaponInteraction::StepRelease(CActor& actor, bool grip)
{
    const bool wanted=ReleaseWanted(actor,m_grip[1] && !grip,!grip);
    return UpdateRelease(actor,wanted,grip);
}
CVrWeaponInteraction::CommitEdges::~CommitEdges()
{
    self.m_trigger=next.trigger; self.m_grip[0]=next.grip[0]; self.m_grip[1]=next.grip[1];
    self.m_button=next.button; self.m_secondary=next.secondary; self.m_mag_trigger=next.mag_trigger;
}
void CVrWeaponInteraction::InteractGeneric(CActor& actor)
{
    // Generic slot weapons may have no audited physical VR profile. They
    // can still be drawn and stowed with a right-hand hip grip, retaining
    // their normal weapon behavior rather than fabricating magazine data.
    Fmatrix right;
    const bool tracked=g_vr_owns_present && IsGameTypeSingle() &&
        m_weapon.H_Parent()==&actor && VrControlsFree(actor) &&
        (m_weapon.CurrSlot()==INV_SLOT_2 || m_weapon.CurrSlot()==INV_SLOT_3) &&
        vr::HandTransform(HandOf(1),false,right);
    const bool grip=tracked && vr::GripHeld(vr::HandSqueeze(HandOf(1)),m_grip[1]);
    const bool left_grip=tracked && vr::GripHeld(vr::HandSqueeze(HandOf(0)),m_grip[0]);
    const bool active=actor.inventory().ActiveItem()==&m_weapon;
    // The support trigger on the barrel line goes back to the rifle (or
    // to the launcher on a weapon without physical handling). At the
    // pouch, in launcher mode, it loads a grenade by the game's reload,
    // which keeps the launcher's rocket and ammunition in step.
    const bool support_trigger=tracked && active && grip && !m_carried &&
        m_weapon.IsGrenadeLauncherAttached() && vr::TriggerPulled(vr::HandTrigger(HandOf(0)));
    // The face buttons are not used here: they keep their last state.
    Edges next{m_trigger,{left_grip,grip},m_button,m_secondary,support_trigger};
    CommitEdges commit{*this,next};
    Fmatrix support_hand, weapon_pose;
    if (support_trigger && !m_mag_trigger && !VrHandHoldsItem(actor,HandOf(0)) &&
        vr::HandTransform(HandOf(0),false,support_hand))
    {
        if (Near(support_hand.c,VrPouchPoint(actor),vr_pouch_reach))
        {
            if (!m_weapon.VrPhysicalMechanismAvailable() && !m_weapon.IsPending())
            {
                actor.IR_OnKeyboardPress(kWPN_RELOAD);
                actor.IR_OnKeyboardRelease(kWPN_RELOAD);
                vr::HandHaptic(HandOf(0),0.15f);
            }
        }
        else if (GenericTransform(weapon_pose) &&
            SupportNear(right.c,Point(weapon_pose,MuzzleLocal()),weapon_pose.k,support_hand.c)==1)
            m_launcher_toggle=true;
    }
    // Squeezing the support hand on the barrel line of a two-handed
    // weapon steadies it; releasing that grip returns to a one-handed hold.
    Fmatrix left_hand, held;
    if (!left_grip || !active) m_supporting=false;
    else if (m_carried && !m_grip[0] && vr::HandTransform(HandOf(0),false,left_hand) &&
        GenericTransform(held) && Near(left_hand.c,held.c,0.12f))
    {
        TakeBack();
        next.grip[0]=next.grip[1]=true;
        return;
    }
    else if (!m_carried && !m_grip[0] && vr::HandTransform(HandOf(0),false,left_hand) && GenericTransform(held))
        DetectSupport(right.c,Point(held,MuzzleLocal()),held.k,left_hand.c);
    if (!active) ResetHold();
    // Taken by a hand: as in the profiled path.
    else if (m_hand_claimed) { m_holster.gripped=true; m_hand_claimed=false; }
    if (tracked && active && HandOverOnRelease(actor,grip,left_grip))
    {
        if (m_trigger) actor.IR_OnKeyboardRelease(kWPN_FIRE);
        m_release_pending=false;
        next.trigger=false; next.grip[0]=false; next.grip[1]=true; next.mag_trigger=false;
        return;
    }
    if (tracked && active && StepRelease(actor,grip))
    {
        next.trigger=false;
        return;
    }
    if (!tracked) m_release_pending=false;
    // Each tap a pull (vr::FireTrigger), as on the profiled path.
    const bool firing=m_fire_trigger.Step(tracked && active && grip && !m_carried ?
        vr::HandTrigger(HandOf(1)) : 0.f);
    // Shotguns retain their native Action(kWPN_FIRE) state machine,
    // including the double-barrel and interrupted-reload behaviours.
    if (firing && !m_trigger) actor.IR_OnKeyboardPress(kWPN_FIRE);
    else if (!firing && m_trigger) actor.IR_OnKeyboardRelease(kWPN_FIRE);
    next.trigger=firing;
}
// What a pull of the support trigger works (vr::ChooseSupportAction):
// the part the hand is nearest, of those it can work now and
// within reach of. Only that one is worked, each at its own place
// below; a part's rule refusing it does not hand the pull to
// another. A part or the belt the hand took (on_part) has it first.
vr::SupportAction CVrWeaponInteraction::ChooseSupport(CActor& actor, const Handling& h)
{
    if (!h.left_valid || !h.mag_trigger || m_mag_trigger || m_racking || h.on_part)
        return vr::SupportAction::None;
    vr::SupportCandidate candidates[5];
    int offered=0;
    const auto offer=[&](vr::SupportAction action, float distance, float reach)
    { candidates[offered++]={action,distance,reach}; };
    // A slide or charging handle; the grip stays for holding the weapon.
    if (m_mechanism.held<0 && m_profile.cycle_travel>0)
        offer(vr::SupportAction::Rack,h.left.c.distance_to(h.cycle_point),m_profile.grab_radius);
    // A seated magazine: from a free hand, or one opened on it; a
    // hand still squeezing to steady the weapon pulls its trigger
    // while shooting and must not rip it out.
    if (m_profile.detachable && m_mechanism.inserted>=0 && m_mechanism.held<0 && (!m_supporting || !h.grip[0]))
        offer(vr::SupportAction::TakeMagazine,MagazineGrabDistance(h.world,h.left.c),1.f);
    // The bolt catch or slide stop of a locked action.
    if (h.has_release && m_mechanism.locked && m_mechanism.held<0)
        offer(vr::SupportAction::ReleaseAction,h.left.c.distance_to(Point(h.world,m_profile.release_point)),0.04f);
    if (m_dust_open && m_profile.dust_cover_bone.size())
        offer(vr::SupportAction::CloseDustCover,h.left.c.distance_to(Point(h.world,m_profile.dust_cover_grab)),0.035f);
    if (m_weapon.IsGrenadeLauncherAttached() && !m_inserting && m_mechanism.held<0 &&
        !VrHandHoldsItem(actor,HandOf(0)))
        offer(vr::SupportAction::SelectLauncher,h.left.c.distance_to(Point(h.world,m_profile.launcher_grab)),0.07f);
    return vr::ChooseSupportAction(candidates,offered);
}

void CVrWeaponInteraction::StartRack(const Handling& h)
{
    // The support hand's trigger on the slide or charging handle pulls it.
    if (h.support_action==vr::SupportAction::Rack && RuleAllows(m_profile.rules.chamber,"charging the weapon"))
    {
        m_rack_start=h.local.z+m_mechanism.slide;
        m_rack_lift_start=h.local.y;
        m_rack_hand_z=h.local.z;
        m_handle_pull=0.f;
        // Along a path the hand keeps its offset from the knob.
        const vr::V3 knob=m_profile.action_path.GrabAt(m_action_u);
        m_action_offset.set(h.local.x-knob.x,h.local.y-knob.y,h.local.z-knob.z);
        m_dust_open=true;
        m_racking=true; m_racked=false;
        // A bolt left open is taken where it is: back already.
        m_slide_back_sounded=m_profile.manual_cycle && m_mechanism.slide>=m_profile.cycle_travel*0.8f;
    }
}

void CVrWeaponInteraction::UpdateSupport(CActor& actor, const Handling& h)
{
    // The support hand takes the weapon when its grip closes at the
    // fore-end or round the weapon hand: a hand already closed and carried
    // onto the weapon does not snap to it.
    if (h.left_valid && h.grip[0] && !m_grip[0] && !m_supporting && !h.on_part && !VrHandHoldsItem(actor,HandOf(0)))
    {
        if (m_mechanism.held<0 && !m_shell_held && !m_racking)
        {
            const Fvector muzzle=Point(h.world,m_profile.muzzle);
            // Open barrels are taken where they hang: test the hand
            // where it would be with them closed.
            Fvector support=h.left.c;
            if (m_profile.break_action && m_break_angle>0.f)
            {
                Fmatrix moved, inverse;
                HingeWorld(h.world,moved);
                inverse.invert(moved);
                inverse.transform_tiny(support,h.left.c);
            }
            const int kind=DetectSupport(h.right.c,muzzle,h.world.k,support);
            m_support_barrels=false;
            if (m_profile.break_action && kind==1)
            {
                Fmatrix inverse; inverse.invert(h.world);
                Fvector barrels; inverse.transform_tiny(barrels,support);
                m_support_barrels=barrels.z>m_profile.hinge_point.z+0.03f;
            }
        }
    }
    // A hand holding a magazine is not on the weapon.
    if (m_supporting && (!h.grip[0] || m_mechanism.held>=0)) m_supporting=false;
}

void CVrWeaponInteraction::MoveRack(const Handling& h)
{
    if (m_racking)
    {
        if (h.left_valid && h.mag_trigger)
        {
            // A manual action follows its animated path (a bolt lifts
            // through its turn, then slides); a slide or handle goes
            // straight back.
            float position=m_rack_start-h.local.z;
            m_handle_pull=clampr(m_rack_hand_z-h.local.z,0.f,std::max(m_profile.cycle_travel,0.f));
            if (m_profile.manual_cycle && m_profile.action_path.Valid())
            {
                m_action_u=m_profile.action_path.Project(vr::V3(h.local.x-m_action_offset.x,
                    h.local.y-m_action_offset.y,h.local.z-m_action_offset.z),m_action_u);
                position=m_profile.action_path.RearAt(m_action_u);
            }
            m_mechanism.MoveManualSlide(position,m_weapon.m_magazine,[&]()
            {
                if (!m_weapon.m_magazine.empty())
                {
                    const Fvector port=Point(h.world,m_profile.shell_point);
                    m_weapon.SpawnAmmo(1,m_weapon.m_magazine.back().m_ammoSect.c_str(),0xffff,&port);
                    m_weapon.m_magazine.pop_back();
                    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
                }
            },[&]()
            {
                Fvector velocity; m_weapon.PHGetLinearVell(velocity);
                DropCase(h.world,velocity);
            });
            m_racked=m_racked || m_mechanism.slide>=m_profile.cycle_travel*0.8f ||
                m_mechanism.manual_unlock_ready;
            // Every full stroke clicks, back and forward, not only the first.
            if (!m_slide_back_sounded && m_mechanism.slide>=m_profile.cycle_travel*0.8f)
            { m_slide_back_sounded=true; PlayHandling(SoundSlideBack,h.cycle_point); }
            else if (m_slide_back_sounded && m_mechanism.slide<=m_profile.cycle_travel*0.2f)
            { m_slide_back_sounded=false; PlayHandling(SoundSlideForward,h.cycle_point); }
        }
        else
        {
            // An HK's handle lifted 2 cm into its notch at full travel
            // stays back; a slide springs home; a bolt stays where it
            // is let go.
            const bool notched=m_mechanics.handle_notch && h.left_valid && h.local.y-m_rack_lift_start>=0.02f &&
                m_mechanism.LockOpen();
            if (notched)
            {
                m_handle_hooked=true;
                if (vr::ContactDebug()>0) Msg("* [VR weapon] %s: charging handle hooked into its notch",
                    m_weapon.cNameSect().c_str());
            }
            else if (!m_profile.manual_cycle) m_mechanism.ReleaseManualSlide(m_weapon.m_magazine);
            if (m_racked) { m_weapon.bMisfire=false; m_weapon.bClearJamOnly=false; }
            if (m_slide_back_sounded && !m_profile.manual_cycle && !notched) PlayHandling(SoundSlideForward,h.cycle_point);
            m_slide_back_sounded=false;
            m_racking=m_racked=false;
            vr::HandHaptic(HandOf(0),0.15f);
        }
    }
}

void CVrWeaponInteraction::WorkMagazine(Handling& h)
{
    // Right upper face button (B on Touch) releases the magazine.
    if (m_basic_magazine && h.secondary && !m_secondary &&
        m_mechanism.inserted>=0 && m_mechanism.held<0 &&
        m_mechanism.Detach(m_weapon.m_magazine))
    {
        const int index=m_mechanism.held;
        // Unprofiled pistols replace native reload: like it, keep the
        // released magazine's rounds in inventory. The prop is empty.
        ReturnCartridges(m_mechanism.magazines[index]);
        m_mechanism.magazines[index].clear();
        Fmatrix pose=h.world;
        pose.c=Point(h.world,m_profile.mag_base);
        CPhysicsShell* shell=CreateShell(index,pose,1);
        if (shell) FinishDrop(index,shell);
        else m_mechanism.Stow();
        m_basic_reload_rounds=unsigned(m_weapon.m_magazine.size());
        m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
        PlayHandling(SoundMagOut,h.mag_base);
        vr::HandHaptic(HandOf(1),0.2f);
    }
    // With a fresh magazine already in the support hand, the release
    // drops the one in the well (tactical reload).
    if (m_profile.detachable && h.secondary && !m_secondary && m_mechanism.inserted>=0 &&
        m_mechanism.held>=0 && !m_inserting && RuleAllows(m_profile.rules.magazine,"releasing the magazine"))
        DropInserted(h.world);
    // The support hand takes the magazine straight out into the hand
    // by pulling its trigger on it, thumbing the release as the hand
    // closes: round its body or under its floorplate
    // (MagazineGrabDistance), nearer it than any other part. The
    // release (B) always drops it, as in H3VR.
    if (h.support_action==vr::SupportAction::TakeMagazine &&
        RuleAllows(m_profile.rules.magazine,"taking out the magazine") &&
        m_mechanism.Detach(m_weapon.m_magazine))
    {
        // Seated, its pose is the weapon's own.
        TakeMagazineIntoHand(h.world,h.left,h.world,0.f,h.held_root);
        PlayHandling(SoundMagOut,h.mag_base);
        vr::HandHaptic(HandOf(0),0.2f);
        vr::HandHaptic(HandOf(1),0.12f);
    }
    if (m_profile.detachable && h.secondary && !m_secondary &&
        m_mechanism.inserted>=0 && m_mechanism.held<0 &&
        RuleAllows(m_profile.rules.magazine,"releasing the magazine"))
    {
        // The catch releases: the magazine slides out and falls
        // (DropInserted sounds it). Taken with the trigger, it came
        // out into the hand above.
        DropInserted(h.world);
    }
    // A released magazine still sliding out is caught by the support
    // hand closing (trigger or grip) on it, and drawn out by hand from
    // there. A support hand holding the weapon never takes it: it stays on
    // the fore-end (a hand beside the magazine snapped onto it).
    if (m_ejecting>=0 && m_mechanism.held<0 && h.left_valid && !h.on_part && !m_supporting && m_contact &&
        m_contact->path.Valid() && ((h.mag_trigger && !m_mag_trigger) || (h.grip[0] && !m_grip[0])))
    {
        Fmatrix sliding; sliding.mul_43(h.world,ToMatrix(m_contact->path.At(m_eject_s)));
        if (h.left.c.distance_to(Point(sliding,m_profile.mag_box_center))<0.08f && m_mechanism.TakeLoose(m_ejecting))
        {
            TakeMagazineIntoHand(h.world,h.left,sliding,m_eject_s,h.held_root);
            m_ejecting=-1;
            ReleaseModel(m_eject_visual);
            vr::HandHaptic(HandOf(0),0.15f);
        }
    }
}

void CVrWeaponInteraction::WorkActionControls(CActor& actor, const Handling& h)
{
    // The support trigger on the bolt catch or slide stop releases a
    // locked action, on weapons that have one.
    if (h.support_action==vr::SupportAction::ReleaseAction && m_mechanism.locked)
    {
        m_mechanism.ReleaseSlide(m_weapon.m_magazine);
        PlayHandling(SoundSlideForward,h.cycle_point);
        vr::HandHaptic(HandOf(0),0.15f);
    }
    // An HK's handle in its notch: a slap down (1.2 m/s or more)
    // past it releases the bolt. Down the weapon, relative to it: the
    // real hands' motion (VrHandMotion), so a turn, a crouch or a fall
    // carrying both hands is no slap.
    if (m_mechanics.handle_notch && m_mechanism.locked && h.left_valid && m_have_hand[0] && m_have_hand[1] &&
        !m_racking)
    {
        const float down=-Fvector().sub(m_hand_velocity[0],m_hand_velocity[1]).dotproduct(h.world.j);
        const Fvector handle=h.cycle_point;
        if (down>=1.2f && Near(h.left.c,handle,0.07f))
        {
            m_mechanism.ReleaseSlide(m_weapon.m_magazine);
            PlayHandling(SoundSlideForward,handle);
            vr::HandHaptic(HandOf(0),0.3f);
            if (vr::ContactDebug()>0) Msg("* [VR weapon] %s: slapped the handle out of its notch (%.1f m/s)",
                m_weapon.cNameSect().c_str(),down);
        }
    }
    // The dust cover is pushed shut with the support trigger.
    if (h.support_action==vr::SupportAction::CloseDustCover)
    {
        m_dust_open=false;
        vr::HandHaptic(HandOf(0),0.08f);
    }
    // The support trigger on the underbarrel launcher selects it.
    if (h.support_action==vr::SupportAction::SelectLauncher) m_launcher_toggle=true;
    // A releases a locked action on a weapon with a release (as a
    // backup to pressing it); otherwise it cycles firing modes.
    if (h.button && !m_button && !m_racking)
    {
        if (h.has_release && m_mechanism.locked)
        {
            m_mechanism.ReleaseSlide(m_weapon.m_magazine);
            PlayHandling(SoundSlideForward,h.cycle_point);
        }
        else
        {
            actor.IR_OnKeyboardPress(kWPN_FIREMODE_NEXT);
            actor.IR_OnKeyboardRelease(kWPN_FIREMODE_NEXT);
        }
        vr::HandHaptic(HandOf(1),0.10f);
    }
}

void CVrWeaponInteraction::InsertMagazine(CActor& actor, Handling& h)
{
    if (m_profile.detachable && m_mechanism.held>=0 && m_mechanism.inserted<0 && h.held_valid && m_contact)
    {
        // The hand puts the magazine in (VrMagazineContact.h): its
        // feed lips at the well's mouth, lined up within about 28
        // degrees, and the well takes it onto its line; it then slides
        // in with the hand and seats two thirds of the way in, or at
        // once on a quick push. Drawn back out of the mouth, it is the
        // hand's again.
        const vr::InsertPath& path=m_contact->path;
        const vr::InsertZone zone;
        const vr::Frame held=ToFrame(Fmatrix().mul_43(Fmatrix().invert(h.world),h.held_root));
        if (!m_guide.active && vr::InsertCheck(path,held,zone)==vr::InsertTaken &&
            RuleAllows(m_profile.rules.magazine,"inserting the magazine"))
        {
            float lateral=0.f;
            m_guide.Take(held,path.Project(held.Apply(path.lead),lateral));
            m_inserting=true;
            m_slide_tick=0.f;
            vr::HandHaptic(HandOf(0),0.08f);
        }
        if (m_guide.active)
        {
            const vr::GuideEvent event=vr::StepGuide(m_guide,path,zone,held,Device.fTimeDelta);
            // Friction as it slides: a faint tick every 12 mm.
            const float depth=m_guide.In(path)*path.mouth*path.length;
            if (_abs(depth-m_slide_tick)>=0.012f) { m_slide_tick=depth; vr::HandHaptic(HandOf(0),0.04f); }
            if (event==vr::GuideSeated) SeatMagazine(h.mag_base);
            else if (event==vr::GuideLeft) { m_inserting=false; vr::HandHaptic(HandOf(0),0.06f); }
        }
    }
    if (m_mechanism.held>=0 && h.left_valid && !h.mag_hold && (m_mag_trigger || m_grip[0]))
    {
        // Let go a third of the way in or more, the magazine goes home;
        // short of that it falls (or is stowed at the hip).
        if (m_contact && vr::OnRelease(m_guide,m_contact->path)==vr::ReleaseSeats)
            SeatMagazine(h.mag_base);
        else
        {
            if (Near(h.left.c,VrPouchPoint(actor),0.22f))
            { m_mechanism.Stow(true); m_pouch_preview=false; }
            // Over the shoulder its rounds go into the backpack and
            // the pouch fills another next time.
            else if (VrAtBackpack(actor,h.left.c))
            {
                const int index=m_mechanism.held;
                ReturnCartridges(m_mechanism.magazines[index]);
                Msg("* [VR weapon] %s: magazine put in the backpack (%u rounds)",
                    m_profile.section.c_str(),unsigned(m_mechanism.magazines[index].size()));
                m_mechanism.magazines[index].clear();
                m_mechanism.DiscardHeld();
                m_pouch_preview=false;
                vr::HandHaptic(HandOf(0),0.22f);
            }
            // Still guided here, so it falls from where it was drawn.
            else DropMagazine(0);
            m_guide.active=false; m_inserting=false;
        }
    }
}
void CVrWeaponInteraction::Interact(CActor& actor)
{
    PollContact();
    // Between frames of handling: the switch normalizes the mechanism and
    // swaps the weapon's magazine with the launcher's.
    if (m_launcher_toggle)
    {
        m_launcher_toggle=false;
        if (m_weapon.IsGrenadeLauncherAttached() && m_weapon.GetState()==CWeapon::eIdle && !m_weapon.IsPending())
        {
            if (actor.is_safemode()) actor.set_safemode(false);
            m_weapon.ToggleGrenadeLauncher();
            vr::HandHaptic(HandOf(0),0.2f);
        }
    }
    // A derived profile decides the mechanism (break-action or tube loading)
    // that VrPhysicalMechanismAvailable reports: measure the model as soon as
    // the weapon is the player's.
    if (m_supported && m_profile.automatic && g_vr_owns_present && m_weapon.H_Parent()==&actor)
        EnsureVisual();
    if (!m_supported || !m_weapon.VrPhysicalMechanismAvailable())
    {
        InteractGeneric(actor);
        return;
    }
    if (!Mode()) { Normalize(); m_restore_empty_chamber=false; return; }
    // Derived profiles need the HUD model's measurements before handling.
    if (m_profile.automatic && !EnsureVisual()) return;
    // The weapon in the hand; holstered ones are sampled ahead, one a
    // second (Prepare, UpdateVrWeapons).
    if (actor.inventory().ActiveItem()==&m_weapon)
    {
        RefreshAuthoredHands();
        RequestScopeView();
    }
    ApplyMechanismLimits();
    m_mechanism.Initialize(m_weapon.m_magazine);
    if (!m_parts_ready) ResetParts();
    // A box taken out or a new one put in leaves no belt on the tray.
    if (m_profile.belt.used && m_mechanism.inserted!=m_parts_inserted)
    {
        m_parts_inserted=m_mechanism.inserted;
        m_belt.seated=false;
        m_belt.hand=-1;
    }
    // A full bolt or slide cycle fits inside the weapon's shot interval.
    m_mechanism.cycle_time=clampr(m_weapon.fOneShotTime*0.7f,0.03f,0.09f);
    // Firing the chambered round after a proxy drop lowers the baseline, so
    // any later native reload is recognised as a new inserted magazine.
    m_basic_reload_rounds=std::min(m_basic_reload_rounds,unsigned(m_weapon.m_magazine.size()));
    if (m_basic_magazine && m_mechanism.inserted<0 &&
        m_weapon.m_magazine.size()>m_basic_reload_rounds)
    {
        // Native reload replenished ammunition after a proxy magazine drop.
        // Treat this as a new inserted magazine for the next B release.
        m_mechanism.inserted=0;
    }
    if (m_restore_empty_chamber)
    {
        m_mechanism.chamber=false;
        m_restore_empty_chamber=false;
    }
    if (m_weapon.m_magazine.empty()) m_mechanism.chamber=false;
    else if (m_basic_magazine && m_mechanism.inserted>=0 && m_profile.cycle_travel<=0)
        m_mechanism.chamber=true;
    Fmatrix right,left,world;
    right.identity(); left.identity();
    const bool valid=VrControlsFree(actor) && vr::HandTransform(HandOf(1),false,right);
    if (!valid)
    {
        StowHands();
        m_fire_queued=0;
        return;
    }
    // A support hand carrying a magazine from the world (one this weapon
    // does not take) is not on the weapon.
    const bool left_valid=vr::HandTransform(HandOf(0),false,left) && !VrFloorMagazineHand(HandOf(0));
    const bool hand_valid[2]={left_valid,true};
    const Fmatrix hand_pose[2]={left,right};
    const unsigned epoch=vr::GameplayInputEpoch();
    // Velocities from the hands' poses in the body's frame: a snap turn or
    // walking moves the hands in the world, not the player's real hands.
    Fmatrix body;
    const bool body_valid=vr::BodyTransform(body);
    const Fvector carried=VrCarriedVelocity(actor);
    for (int hand=0;hand<2;++hand)
    {
        m_hand_velocity[hand].set(0,0,0);
        m_hand_angular[hand].set(0,0,0);
        Fmatrix local;
        const bool local_valid=hand_valid[hand] && body_valid && vr::HandInBody(HandOf(hand),false,local);
        if (local_valid && m_have_hand[hand] && m_hand_epoch==epoch && Device.fTimeDelta>0.001f)
        {
            VrHandMotion(m_previous_hand[hand],local,body,carried,Device.fTimeDelta,
                m_hand_velocity[hand],m_hand_angular[hand]);
            // A jump (tracking regained) is no motion.
            if (Fvector().sub(local.c,m_previous_hand[hand].c).square_magnitude()>=0.25f*0.25f)
                m_hand_velocity[hand]=carried;
            if (m_hand_angular[hand].square_magnitude()>400.f) m_hand_angular[hand].set(0,0,0);
        }
        if (local_valid) m_previous_hand[hand]=local;
        m_have_hand[hand]=local_valid;
    }
    m_hand_epoch=epoch;
    // The grips as UpdateVrWeapons read them this frame, by physical hand:
    // one hysteresis for the whole frame, so a squeeze the holsters and
    // pickup saw is the squeeze the weapon sees.
    const auto& pickup=actor.m_vr_pickup;
    const bool grip[2]={left_valid && pickup.grip_held[HandOf(0)],pickup.grip_held[HandOf(1)]};
    const bool active=actor.inventory().ActiveItem()==&m_weapon;
    const bool held=grip[1] && !m_carried;
    // Released as the finger lets the trigger forward, not only at rest:
    // quick taps on an automatic are separate pulls (vr::FireTrigger).
    const bool trigger=m_fire_trigger.Step(held ? vr::HandTrigger(HandOf(1)) : 0.f);
    m_trigger_value=held ? clampr(vr::HandTrigger(HandOf(1))/vr::pull_threshold,0.f,1.f) : 0.f;
    const bool mag_trigger=left_valid && vr::TriggerPulled(vr::HandTrigger(HandOf(0)));
    const bool mag_hold=mag_trigger || grip[0];
    // Slide release / fire mode stay on right A: left X toggles the
    // inventory, so a left-held weapon uses the support hand's A.
    const bool button=vr::HandButton(1) && !VrHeadGesture(1);
    const bool secondary=vr::HandSecondaryButton(HandOf(1));
    // This frame's controls, kept for the next frame's edges (m_trigger,
    // m_grip, ...): written once, when the update leaves by any path. A path
    // that passes the weapon between hands changes `next` before it leaves.
    Edges next{trigger,{grip[0],grip[1]},button,secondary,mag_trigger};
    CommitEdges commit{*this,next};
    if (!active) ResetHold();
    // Taken by a hand (a holster, the floor, its trigger included): it can
    // go back to the holster or be let go without a grip first, from this
    // frame's release check on.
    else if (m_hand_claimed) m_holster.gripped=true;
    if (active && !m_pumping && !m_racking && m_mechanism.held<0 && HandOverOnRelease(actor,grip[1],grip[0]))
    {
        m_weapon.FireEnd();
        m_release_pending=false;
        // The hand now holding it is already gripped; the one that let go is not.
        next.trigger=false; next.grip[0]=false; next.grip[1]=true; next.mag_trigger=false;
        return;
    }
    if (active && (StepRelease(actor,grip[1]) || m_release_pending)) return;
    if (!m_input.Step(epoch,true,trigger || button || secondary))
    {
        m_weapon.FireEnd();
        return;
    }
    if (!active)
    {
        m_weapon.FireEnd(); m_holstered=true;
        // Drawing is arbitrated once per actor in UpdateVrWeapons. A held
        // grip passing another holster must never switch slot weapons.
        // Activate() only queues the slot: the claim lasts while it is queued.
        if (actor.inventory().GetNextActiveSlot()!=m_weapon.CurrSlot()) m_hand_claimed=false;
    }
    else
    {
        // Drawn by key or wheel rather than by a hand: the dominant hand.
        if (m_holstered && !m_hand_claimed)
        { m_hand=vr::DominantHand(); m_carried=false; }
        m_holstered=false;
        m_hand_claimed=false;
        const bool ready=WeaponReady();
        // A trigger held through focus recovery can still draw a spare once
        // the hand reaches the pouch; Draw() cannot duplicate a held mag.
        if (m_profile.detachable && m_mechanism.held<0 && m_ejecting<0 && left_valid &&
            mag_trigger && PouchHasAmmo() && Near(left.c,VrPouchPoint(actor),vr_pouch_reach) && !VrHandHoldsItem(actor,HandOf(0)))
        {
            int index=m_mechanism.next_pouch;
            if (index==m_mechanism.inserted) index=1-index;
            if (index!=m_mechanism.inserted)
            {
                m_mechanism.next_pouch=index;
                // A fresh magazine is held as drawn, not where an earlier one
                // was taken out of the weapon (stowed by a menu, for one).
                if (m_mechanism.Draw())
                { m_pouch_preview=true; m_mag_grabbed=false; vr::HandHaptic(HandOf(0),0.15f); }
            }
        }
        // Hands work the weapon whatever it is doing (a shot or its
        // animation included); only drawing, holstering and the native
        // reload or switch keep them off. A press then is never lost.
        const u32 state=m_weapon.GetState();
        const bool handling=state!=CHUDState::eShowing && state!=CHUDState::eHiding &&
            state!=CHUDState::eHidden && state!=CWeapon::eReload && state!=CWeapon::eSwitch;
        if (handling && Transform(world))
        {
            Handling h;
            h.world=world; h.left=left; h.right=right; h.left_valid=left_valid;
            h.grip[0]=grip[0]; h.grip[1]=grip[1];
            h.mag_trigger=mag_trigger; h.mag_hold=mag_hold; h.button=button; h.secondary=secondary;
            Fmatrix inverse; inverse.invert(world);
            h.local.set(0,0,0);
            if (left_valid) inverse.transform_tiny(h.local,left.c);
            // A magazine back in the weapon or the pouch is no longer held
            // where the hand closed on it.
            if (m_mechanism.held<0) m_mag_grabbed=m_mag_from_world=false;
            if (m_mechanism.held<0) { m_guide.active=false; m_inserting=false; }
            h.held_root=left;
            h.held_valid=left_valid && MagazineHandPose(HandOf(0),h.held_root);
            if (left_valid && !m_racking && m_handle_hand_valid)
                m_cycle_use_hand=h.local.distance_to(m_handle_hand)<h.local.distance_to(m_profile.cycle_grab);
            h.cycle_point=Point(world,CycleGrabLocal());
            h.cycle_point.mad(world.k,-CycleGrabTravel());
            h.mag_base=Point(world,m_profile.mag_base);
            // A support hand on a part (the cover) or the belt does nothing else.
            // Parts are taken with the trigger, as every other handling is.
            h.on_part=UpdateParts(world,left_valid,left,mag_trigger,mag_trigger && !m_mag_trigger);
            if (left_valid && grip[0] && !m_grip[0] && m_carried &&
                Near(left.c,Point(world,GripLocal()),0.12f))
            {
                TakeBack();
                next.grip[0]=next.grip[1]=true;
                next.trigger=false;
                return;
            }
            h.has_release=m_mechanics.has_bolt_catch || m_mechanics.has_slide_stop ||
                m_mechanics.has_bolt_release || m_mechanics.has_slide_release;
            // What a pull of the support trigger works (vr::ChooseSupportAction):
            // the part the hand is nearest, of those it can work now and
            // within reach of. Only that one is worked, each at its own place
            // below; a part's rule refusing it does not hand the pull to
            // another. A part or the belt the hand took (on_part) has it first.
            h.support_action=ChooseSupport(actor,h);
            // Then each step in turn, in this order: a rack started this
            // frame keeps the support grip from closing, a magazine taken
            // out is inserted from where the hand holds it.
            StartRack(h);
            UpdateSupport(actor,h);
            MoveRack(h);
            WorkMagazine(h);
            if (ShellLoading())
                InteractShells(actor,world,left_valid,left,mag_hold,grip[0],mag_trigger,secondary);
            WorkActionControls(actor,h);
            InsertMagazine(actor,h);
            if (!m_racking && trigger && !m_trigger && (ready || state==CWeapon::eFire)) PullTrigger();
        }
        // A pull while the last shot is still in its fire state (a full shot
        // animation, the end of a burst) fires as on the desktop; the
        // readiness gate above would otherwise swallow quick taps.
        else if (!m_racking && trigger && !m_trigger && m_weapon.GetState()==CWeapon::eFire)
            PullTrigger();
        // A pull that arrived while the bolt was still cycling: fire now that
        // it is in battery, a single shot if the trigger is already released.
        if (m_fire_queued && !m_mechanism.cycling)
        {
            if (Device.dwTimeGlobal-m_fire_queued<=250 && !m_racking)
            {
                m_weapon.FireStart();
                if (!trigger) m_weapon.FireEnd();
            }
            m_fire_queued=0;
        }
        // Released every frame, not only when idle: an automatic weapon is in
        // its fire state while shooting, which the readiness gate excludes.
        if (!trigger && m_trigger) m_weapon.FireEnd();
    }
    m_weapon.iAmmoElapsed=int(m_weapon.m_magazine.size());
}

void CVrWeaponInteraction::RenderPouch(CActor& actor)
{
    if (!g_vr_owns_present || !vr::GameplayActive() || m_weapon.m_ammoTypes.empty() || !PouchHasAmmo()) return;
    // The pouch shows the ammunition the support hand will draw for this
    // weapon (SwapPouchAmmo changes it).
    const shared_str& ammo=m_weapon.m_ammoTypes[PouchAmmo()];
    if (m_pouch_section!=ammo)
    {
        ReleaseModel(m_pouch_visual);
        m_pouch_section=ammo;
        if (pSettings->line_exist(ammo,"visual"))
            m_pouch_visual=::Render->model_Create(pSettings->r_string(ammo,"visual"));
    }
    if (!m_pouch_visual) return;
    Fmatrix pose=VrTorsoFrame(actor);
    pose.c=VrPouchPoint(actor);
    ::Render->set_Transform(&pose);
    ::Render->add_Visual(m_pouch_visual);
}

namespace vrwi
{
// A free entry, otherwise the one that has waited longest.
CActor::VrPickupState::VrThrown& VrThrownEntry(CActor::VrPickupState& pickup)
{
    auto* oldest=&pickup.thrown[0];
    for (auto& entry:pickup.thrown)
    {
        if (entry.id==u16(-1)) return entry;
        if (int(entry.expires-oldest->expires)<0) oldest=&entry;
    }
    return *oldest;
}
}

void VrReleaseHeldWeapon(CActor& actor, CWeapon& weapon, int holster_choice, bool settled)
{
    const int side=weapon.VrWeaponHand();
    Fmatrix hand;
    Fvector holster;
    float range=0.f;
    const bool at_holster=holster_choice>=0 ? holster_choice==1 :
        vr::HandTransform(side,false,hand) && weapon.VrHolsterPoint(actor,holster,range) &&
        hand.c.distance_to(holster)<=range;
    // A knife taken from the belt goes back there when let go. Binoculars
    // are dropped away from the chest, as a gun is (VrHandItemsUpdate).
    const bool belt=weapon.CurrSlot()==KNIFE_SLOT;
    if (at_holster || belt || weapon.IsQuestItem())
    {
        // At rest it is put away at once; mid-animation through its own hide,
        // which leaves no shot or pump state behind (its hide is not waited
        // on in VR: UpdateVrWeapons).
        actor.inventory().Activate(NO_ACTIVE_SLOT,settled);
        vr::HandHaptic(side,0.15f);
        return;
    }
    // Opened away from the holster: the weapon leaves the hand as a world
    // object from its tracked pose and keeps the hand's motion. Detached
    // magazines are normalised first, as for any drop; its measured model
    // stays (it is drawn in the hand until the drop lands, and picked up again).
    weapon.VrPrepareDrop();
    auto& pickup=actor.m_vr_pickup;
    auto& thrown=VrThrownEntry(pickup);
    thrown.id=weapon.ID();
    thrown.velocity=pickup.hand_velocity[side];
    thrown.angular=pickup.hand_angular[side];
    thrown.expires=Device.dwTimeGlobal+1500;
    // Binoculars (no VR profile) are drawn at the hand, not at their own
    // transform: they leave from where they were seen.
    thrown.place=!weapon.VrSupported() && VrKnifePose(actor,thrown.matrix);
    actor.g_PerformDrop();
    vr::HandHaptic(side,0.2f);
}

void VrThrowHandBolt(CActor& actor, CInventoryItem& slot_bolt, const Fmatrix& hand, const Fvector& velocity, int side)
{
    auto& pickup=actor.m_vr_pickup;
    // GAMMA's limited bolts (CBolt__State) are used up: one from the
    // backpack flies and the slot keeps its own. Unlimited: the slot's flies
    // and a new one is made for the backpack, to go in the slot.
    ::luabind::functor<bool> state;
    const bool limited=ai().script_engine().functor("_G.CBolt__State",state) && state(actor.ID());
    PIItem thrown=limited ? actor.inventory().Same(&slot_bolt,true) : nullptr;
    if (!thrown) thrown=&slot_bolt;
    if (!limited)
        Level().spawn_item(slot_bolt.object().cNameSect().c_str(),actor.Position(),
            actor.ai_location().level_vertex_id(),actor.ID());
    if (thrown==&slot_bolt) pickup.bolt_refill_until=Device.dwTimeGlobal+3000;
    NET_Packet P;
    actor.u_EventGen(P,GE_OWNERSHIP_REJECT,actor.ID());
    P.w_u16(thrown->object().ID());
    actor.u_EventSend(P);
    auto& entry=VrThrownEntry(pickup);
    entry.id=thrown->object().ID();
    entry.velocity=velocity;
    entry.angular=pickup.hand_angular[side];
    entry.expires=Device.dwTimeGlobal+1500;
    entry.place=true;
    entry.matrix=hand;
    if (vr::PhysicalHands()) VrNearSide(entry.matrix.c,0.05f);
    entry.limited=limited;
    vr::HandHaptic(side,0.25f);
}

void RenderVrPouch(CActor& actor)
{
    if (&actor!=Level().CurrentControlEntity() || !g_vr_owns_present) return;
    // Only while a weapon is in the hand.
    if (auto* weapon=smart_cast<CWeapon*>(actor.inventory().ActiveItem())) weapon->VrRenderPouch(actor);
}

void PrepareVrWeaponSave(CActor& actor)
{
    if (!IsGameTypeSingle()) return;
    for (auto* item: actor.inventory().m_all)
        if (auto* weapon=smart_cast<CWeapon*>(item)) weapon->VrPrepareSnapshot();
}

// The active item is on its way out (being put away, or another item is
// coming in): no hand holds it any more.
