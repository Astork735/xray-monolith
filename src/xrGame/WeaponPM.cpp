#include "pch_script.h"
#include "WeaponPM.h"
#include "Actor.h"
#include "Level.h"
#include "../xrEngine/CameraBase.h"
#include "../xrEngine/Vr/vr_gameplay.h"

CWeaponPM::CWeaponPM() : CWeaponPistol() {}
CWeaponPM::~CWeaponPM() = default;

using namespace luabind;
#pragma optimize("s",on)
void CWeaponPM::script_register(lua_State* L)
{
    module(L)[class_<CWeaponPM, CGameObject>("CWeaponPM").def(constructor<>())];
}

void CWeaponPM::AddShotEffector()
{
    if (!VrMode()) inherited::AddShotEffector();
}

void CWeaponPM::state_Fire(float dt)
{
    if (!VrMode()) { inherited::state_Fire(dt); return; }
    // The fire mode as on the desktop (single, burst or automatic, as the APS
    // has), with the camera-to-muzzle obstruction rule. The shared component
    // owns chamber, slide, magazine, pose, and cleanup.
    Fmatrix world;
    if ((!IsWorking() && !m_bFireSingleShot) || !vr::GameplayActive() || !VrTransform(world) ||
        m_magazine.empty() || bMisfire)
    {
        StopShooting(); SwitchState(eIdle); return;
    }
    fShotTimeCounter-=dt;
    // Between shots of a burst the slide is still cycling: wait for it.
    if (!VrCanFire())
    {
        if (m_iShotNum>0 && VrCycling()) return;
        StopShooting(); SwitchState(eIdle); return;
    }
    if (fShotTimeCounter>0) return;
    const Fvector muzzle=get_LastFP();
    const Fvector anchor=smart_cast<CActor*>(H_Parent())->cam_FirstEye()->vPosition;
    Fvector delta; delta.sub(muzzle,anchor);
    const float distance=delta.magnitude();
    collide::rq_result hit;
    if (distance>1.5f || (distance>EPS_S &&
        Level().ObjectSpace.RayPick(anchor,delta.div(distance),distance,collide::rqtBoth,hit,H_Parent())))
    {
        StopShooting(); SwitchState(eIdle); return;
    }
    ++m_iShotNum;
    CheckForMisfire();
    if (!bMisfire)
    {
        OnShot(); FireTrace(muzzle,world.k);
        vr::HandHaptic(VrWeaponHand(),0.35f);
    }
    else OnEmptyClick();
    m_bFireSingleShot=false;
    fShotTimeCounter=GetCurrentFireMode()>=2 || cycleDownCheck() ? fModeShotTime : VrShotTime(fOneShotTime);
    // Automatic fires while the trigger is held (released: FireEnd stops
    // it), a burst up to its length; single fire is one shot per pull.
    if (!bMisfire && IsWorking() && (m_iQueueSize<0 || m_iShotNum<m_iQueueSize)) return;
    StopShooting(); SwitchState(eIdle);
}
