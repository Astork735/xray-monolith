////////////////////////////////////////////////////////////////////////////
//	Module 		: script_render_device_script.cpp
//	Created 	: 28.06.2004
//  Modified 	: 28.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Script render device script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_render_device.h"
#include "ui_base.h"

using namespace luabind;

bool is_device_paused(CRenderDevice* d)
{
	return !!Device.Paused();
}

void set_device_paused(CRenderDevice* d, bool b)
{
	Device.Pause(b, TRUE, FALSE, "set_device_paused_script");
}

extern ENGINE_API BOOL bShowPauseString;

void set_device_paused_ex(CRenderDevice* d, bool b)
{
	Device.Pause(b, TRUE, TRUE, "set_device_paused_ex_script");
	bShowPauseString = FALSE;
}

extern ENGINE_API BOOL g_appLoaded;

bool is_app_ready()
{
	return !!g_appLoaded;
}

// The screen scripts lay their UI out for: in VR its 16:9 UI panel, as wide as
// the UI texture (ui_core::screen_aspect). The texture's own nearly square
// size made utils_xml.screen_ratio stretch script windows 1.45 times and
// GAMMA's body health HUD take its unfinished 4:3 layout.
u32 ui_width(const CRenderDevice* self)
{
	return self->dwWidth;
}

u32 ui_height(const CRenderDevice* self)
{
	return g_vr_owns_present ? u32(float(self->dwWidth) / ui_core::screen_aspect() + 0.5f) : self->dwHeight;
}

u32 time_global(const CRenderDevice* self)
{
	THROW(self);
	return (self->dwTimeGlobal);
}

u32 time_continual(const CRenderDevice* self)
{
	THROW(self);
	return (self->dwTimeContinual);
}

extern ENGINE_API float psCamAttachedFOV;

void set_cam_attached_fov(CRenderDevice* self, float deg)
{
	psCamAttachedFOV = deg > 0.f ? _min(_max(deg, 5.f), 170.f) : 83.f;
}

float get_cam_attached_fov(const CRenderDevice* self)
{
	return psCamAttachedFOV;
}

#pragma optimize("s",on)
void CScriptRenderDevice::script_register(lua_State* L)
{
	module(L)
	[
		class_<CRenderDevice>("render_device")
		.property("width", &ui_width)
		.property("height", &ui_height)
		.def_readonly("time_delta", &CRenderDevice::dwTimeDelta)
		.def_readonly("f_time_delta", &CRenderDevice::fTimeDelta)
		.def_readonly("cam_pos", &CRenderDevice::vCameraPosition)
		.def_readonly("cam_dir", &CRenderDevice::vCameraDirection)
		.def_readonly("cam_top", &CRenderDevice::vCameraTop)
		.def_readonly("cam_right", &CRenderDevice::vCameraRight)
		//			.def_readonly("view",					&CRenderDevice::mView)
		//			.def_readonly("projection",				&CRenderDevice::mProject)
		//			.def_readonly("full_transform",			&CRenderDevice::mFullTransform)
		.def_readonly("fov", &CRenderDevice::fFOV)
		.def_readonly("aspect_ratio", &CRenderDevice::fASPECT)
		.def("time_global", &time_global)
		.def("time_continual", &time_continual)
		.def("set_cam_attached_fov", &set_cam_attached_fov)
		.def("get_cam_attached_fov", &get_cam_attached_fov)
		.def_readonly("precache_frame", &CRenderDevice::dwPrecacheFrame)
		.def_readonly("frame", &CRenderDevice::dwFrame)
		.def("is_paused", &is_device_paused)
		.def("pause", &set_device_paused)
		.def("pause_ex", &set_device_paused_ex),
		def("app_ready", &is_app_ready)
	];
}
