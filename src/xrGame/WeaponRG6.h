#pragma once

#include "rocketlauncher.h"
#include "weaponShotgun.h"
#include "script_export_space.h"

class CWeaponRG6 : public CRocketLauncher,
                   public CWeaponShotgun
{
	typedef CRocketLauncher inheritedRL;
	typedef CWeaponShotgun inheritedSG;

public:
	// Loaded by the game's own reload like the RPG-7, never shell by shell:
	// VR still draws its first-person model, so the drum and its grenades
	// follow the native animations (not the static third-person model).
	bool VrPhysicalMechanismAvailable() const override { return true; }
	virtual ~CWeaponRG6();
	virtual BOOL net_Spawn(CSE_Abstract* DC);
	virtual void Load(LPCSTR section);
	virtual void OnEvent(NET_Packet& P, u16 type);

#ifdef CROCKETLAUNCHER_CHANGE
	virtual void UnloadRocket();
#endif

protected:
	virtual void FireStart();
	virtual u8 AddCartridge(u8 cnt);

DECLARE_SCRIPT_REGISTER_FUNCTION
};
