#pragma once
#include "VrWeaponPose.h"

namespace vr
{
// Private model instances retain these override callbacks through any later
// renderer bone calculation. The backing pose buffer must outlive the model.
template<class Model, class Poses, class Callback>
void ApplyPistolPose(Model& model, Poses& poses, Callback callback, unsigned callback_type,
    float slide_travel, bool magazine_only, bool magazine_inserted, bool rounds_visible)
{
    ApplyWeaponPose(model,poses,callback,callback_type,"slide",slide_travel,"bullets",
        {"magazin","sprezyna","siodelko","bullets"},magazine_only,
        magazine_inserted,rounds_visible,"wpn_silencer");
}
}
