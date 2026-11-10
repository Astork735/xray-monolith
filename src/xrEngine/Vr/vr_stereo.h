#pragma once
#include "vr_common.h"
#ifdef VR_ENABLED
namespace vr
{
bool StereoActive();
bool ShouldEnterVr();
bool PrepareVr(unsigned long& low, long& high, unsigned& feature);
bool StartVr(ID3D11Device*, unsigned, unsigned, int);
void StopVr();
void PollVrEvents();
void PrepareStereoSimulation();
void BeginStereoFrame();
void SubmitPendingStereoFrame();
bool RenderStereoScene();
void FinishStereoFrame();
void Recenter();
// Takes the head's height now as the eye height (vr_calibrate_height).
void CalibrateHeight();
}
#endif
