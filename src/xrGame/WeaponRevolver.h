#pragma once
#include "weaponcustompistol.h"

class CWeaponRevolver :
	public CWeaponCustomPistol
{
	typedef CWeaponCustomPistol inherited;
public:
	CWeaponRevolver();
    bool VrPhysicalMechanismAvailable() const override { return false; }
	virtual ~CWeaponRevolver();

	virtual void Load(LPCSTR section);
	virtual void PlayAnimReload();

protected:
	virtual bool AllowFireWhileWorking() { return true; }
	ESoundTypes m_eSoundClose;
};
