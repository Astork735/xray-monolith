#include "../stdafx.h"
#ifdef VR_ENABLED
#include "vr_camera.h"
#include "vr_system.h"
namespace vr
{
bool HeadPoseEngine(Fvector& position, Fvector& direction, Fvector& up)
{ return CVrSystem::Get().HeadPose(position, direction, up); }
}
#endif
