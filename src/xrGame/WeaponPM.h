#pragma once
#include "WeaponPistol.h"
#include "script_export_space.h"

class CActor;
class CWeaponPM : public CWeaponPistol
{
    using inherited=CWeaponPistol;
protected:
    void state_Fire(float dt) override;
    void AddShotEffector() override;
public:
    CWeaponPM();
    virtual ~CWeaponPM();
    DECLARE_SCRIPT_REGISTER_FUNCTION
};
