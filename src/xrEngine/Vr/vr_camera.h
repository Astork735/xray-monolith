#pragma once
#include "vr_common.h"
#ifdef VR_ENABLED
namespace vr
{
// Tracking-origin-relative head pose. The body camera remains authoritative
// for simulation; the render boundary composes the headset pose once per eye.
bool HeadPoseEngine(Fvector& position, Fvector& direction, Fvector& up);
}
#endif
