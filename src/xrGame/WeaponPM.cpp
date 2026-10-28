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
    // PM keeps its established one-shot and camera-to-muzzle obstruction rule.
    // The shared component owns chamber, slide, magazine, pose, and cleanup.
    Fmatrix world;
    if ((!IsWorking() && !m_bFireSingleShot) || !vr::GameplayActive() ||
        !VrCanFire() || !VrTransform(world) || m_magazine.empty())
    {
        StopShooting(); SwitchState(eIdle); return;
    }
    fShotTimeCounter-=dt;
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
        vr::HandHaptic(1,0.35f);
    }
    else OnEmptyClick();
    m_bFireSingleShot=false;
    fShotTimeCounter=fOneShotTime;
    StopShooting(); SwitchState(eIdle);
}
