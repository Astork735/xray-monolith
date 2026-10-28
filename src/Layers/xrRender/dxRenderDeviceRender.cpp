#include "stdafx.h"
#include "dxRenderDeviceRender.h"

#include "ResourceManager.h"
#include "../../xrEngine/shader_bus.h"

// a shader bus texture lane's $user$bus_ alias and the texture it shows
struct bus_texture_slot
{
	ref_texture alias;
	ref_texture target;
	ID3DBaseTexture* applied;
};

static xr_vector<bus_texture_slot> bus_texture_slots;
static u32 bus_texture_serial = 0;

// empties every alias so no surface outlives a reset or the device
static void bus_textures_release()
{
	for (u32 i = 0; i < bus_texture_slots.size(); ++i)
	{
		bus_texture_slots[i].alias->surface_set(nullptr);
		bus_texture_slots[i].applied = nullptr;
	}
}

// rebuilds the slots when a lane's texture changed and follows targets whose surface moved
static void bus_textures_update()
{
	const u32 serial = ShaderBus::texture_serial();
	if (serial != bus_texture_serial)
	{
		bus_texture_serial = serial;

		// the old slots keep their textures loaded until the new slots take them
		xr_vector<bus_texture_slot> old_slots;
		old_slots.swap(bus_texture_slots);

		shared_str id, name;
		for (u32 i = 0; ShaderBus::texture_get(i, id, name); ++i)
		{
			if (!name.size())
				continue;

			bus_texture_slot slot;
			slot.target.create(name.c_str());
			if (!slot.target)
				continue;

			string_path alias;
			xr_sprintf(alias, "$user$bus_%s", id.c_str());
			slot.alias.create(alias);
			slot.applied = nullptr;
			bus_texture_slots.push_back(slot);
		}

		for (u32 i = 0; i < old_slots.size(); ++i)
			old_slots[i].alias->surface_set(nullptr);
	}

	for (u32 i = 0; i < bus_texture_slots.size(); ++i)
	{
		bus_texture_slot& slot = bus_texture_slots[i];
		ID3DBaseTexture* surface = slot.target->surface_get();
		if (surface != slot.applied)
		{
			slot.alias->surface_set(surface);
			slot.applied = surface;
		}
		_RELEASE(surface);
	}
}

dxRenderDeviceRender::dxRenderDeviceRender()
	: Resources(0)
{
	;
}

void dxRenderDeviceRender::Copy(IRenderDeviceRender& _in)
{
	*this = *(dxRenderDeviceRender*)&_in;
}

void dxRenderDeviceRender::setGamma(float fGamma)
{
	m_Gamma.Gamma(fGamma);
}

void dxRenderDeviceRender::setBrightness(float fGamma)
{
	m_Gamma.Brightness(fGamma);
}

void dxRenderDeviceRender::setContrast(float fGamma)
{
	m_Gamma.Contrast(fGamma);
}

void dxRenderDeviceRender::updateGamma()
{
	m_Gamma.Update();
}

void dxRenderDeviceRender::OnDeviceDestroy(BOOL bKeepTextures)
{
	vr_engine_stop();
	m_WireShader.destroy();
	m_SelectionShader.destroy();

	bus_textures_release();
	bus_texture_slots.clear();
	bus_texture_serial = 0;
	Resources->OnDeviceDestroy(bKeepTextures);
	RCache.OnDeviceDestroy();
}

void dxRenderDeviceRender::ValidateHW()
{
	HW.Validate();
}

void dxRenderDeviceRender::DestroyHW()
{
	xr_delete(Resources);
	HW.DestroyDevice();
}

void dxRenderDeviceRender::Reset(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2)
{
    vr_engine_stop();
#if defined(USE_DX11)
    unsigned long low = 0; long high = 0; unsigned feature = 0;
    vr_renderer_mode(vr_engine_prepare(low, high, feature));
#endif
#ifdef DEBUG
    _SHOW_REF("*ref -CRenderDevice::ResetTotal: DeviceREF:",HW.pDevice);
#endif // DEBUG

	bus_textures_release();
	Resources->reset_begin();
	Memory.mem_compact();

#if !defined(USE_DX10) && !defined(USE_DX11)
	const bool noTexturesInRAM = RImplementation.o.no_ram_textures;
	if (noTexturesInRAM)
		ResourcesDeferredUnload();
#endif

	HW.Reset(hWnd);

#if defined(USE_DX11)
	dwWidth = HW.m_ChainDesc.Width;
	dwHeight = HW.m_ChainDesc.Height;
#elif defined(USE_DX10)
	dwWidth = HW.m_ChainDesc.BufferDesc.Width;
	dwHeight = HW.m_ChainDesc.BufferDesc.Height;
#else	//	USE_DX10
	if (noTexturesInRAM)
		ResourcesDeferredUpload();

	dwWidth = HW.DevPP.BackBufferWidth;
	dwHeight = HW.DevPP.BackBufferHeight;
#endif	//	USE_DX10

	fWidth_2 = float(dwWidth / 2);
	fHeight_2 = float(dwHeight / 2);
#if defined(USE_DX11)
    if (vr_engine_start(HW.pDevice, HW.m_ChainDesc.Width, HW.m_ChainDesc.Height, int(HW.m_ChainDesc.Format)))
    {
        vr_engine_dimensions(dwWidth, dwHeight);
        fWidth_2 = float(dwWidth) / 2; fHeight_2 = float(dwHeight) / 2;
    }
    else
    {
        const bool hdr_was_disabled = ps_r4_hdr10_on == 0;
        vr_renderer_mode(false);
        if (hdr_was_disabled && ps_r4_hdr10_on) HW.Reset(hWnd);
    }
#endif
	Resources->reset_end();

#ifdef DEBUG
    _SHOW_REF("*ref +CRenderDevice::ResetTotal: DeviceREF:",HW.pDevice);
#endif // DEBUG
}

void dxRenderDeviceRender::SetupStates()
{
	HW.Caps.Update();

#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement Resetting of render states into default mode
	//VERIFY(!"dxRenderDeviceRender::SetupStates not implemented.");
	SSManager.SetMaxAnisotropy(ps_r__tf_Anisotropic);
	SSManager.SetMipLODBias(ps_r__tf_Mipbias);
#else	//	USE_DX10
	for (u32 i = 0; i < HW.Caps.raster.dwStages; i++)
	{
		CHK_DX(HW.pDevice->SetSamplerState(i, D3DSAMP_MAXANISOTROPY, ps_r__tf_Anisotropic));
		CHK_DX(HW.pDevice->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, *(LPDWORD)&ps_r__tf_Mipbias));
		CHK_DX(HW.pDevice->SetSamplerState ( i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR ));
		CHK_DX(HW.pDevice->SetSamplerState ( i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR ));
		CHK_DX(HW.pDevice->SetSamplerState ( i, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR ));
	}
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_DITHERENABLE, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_COLORVERTEX, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_ZENABLE, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_SHADEMODE, D3DSHADE_GOURAUD ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_CULLMODE, D3DCULL_CCW ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_ALPHAFUNC, D3DCMP_GREATER ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_LOCALVIEWER, TRUE ));

	CHK_DX(HW.pDevice->SetRenderState( D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_SPECULARMATERIALSOURCE,D3DMCS_MATERIAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_EMISSIVEMATERIALSOURCE,D3DMCS_COLOR1 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_MULTISAMPLEANTIALIAS, FALSE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_NORMALIZENORMALS, TRUE ));

	if (psDeviceFlags.test(rsWireframe))
		RCache.set_FillMode(D3DFILL_WIREFRAME);
	else
		RCache.set_FillMode(D3DFILL_SOLID);

	// ******************** Fog parameters
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGCOLOR, 0 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_RANGEFOGENABLE, FALSE ));
	if (HW.Caps.bTableFog)
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGTABLEMODE, D3DFOG_LINEAR ));
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGVERTEXMODE, D3DFOG_NONE ));
	}
	else
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGTABLEMODE, D3DFOG_NONE ));
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR ));
	}

#endif	//	USE_DX10
}

void dxRenderDeviceRender::OnDeviceCreate(LPCSTR shName)
{
	// Signal everyone - device created
	RCache.OnDeviceCreate();
	m_Gamma.Update();
	Resources->OnDeviceCreate(shName);
	::Render->create();
	Device.Statistic->OnDeviceCreate();

	//#ifndef DEDICATED_SERVER
	if (!g_dedicated_server)
	{
		m_WireShader.create("editor\\wire");
		m_SelectionShader.create("editor\\selection");

		DUImpl.OnDeviceCreate();
	}
	//#endif
}

void dxRenderDeviceRender::Create(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2,
                                  bool move_window)
{
	HW.CreateDevice(hWnd, move_window);
#if defined(USE_DX11)
	dwWidth = HW.m_ChainDesc.Width;
	dwHeight = HW.m_ChainDesc.Height;
#elif defined(USE_DX10)
	dwWidth = HW.m_ChainDesc.BufferDesc.Width;
	dwHeight = HW.m_ChainDesc.BufferDesc.Height;
#else	//	USE_DX10
	dwWidth = HW.DevPP.BackBufferWidth;
	dwHeight = HW.DevPP.BackBufferHeight;
#endif	//	USE_DX10
	fWidth_2 = float(dwWidth / 2);
	fHeight_2 = float(dwHeight / 2);
	Resources = xr_new<CResourceManager>();

#if defined(USE_DX11)
	// The engine's D3D11 device and backbuffer size are known - attempt to enter
	// native OpenXR VR (no-op unless requested with -vr / vr_enabled). The eye
	// swapchains are matched to the backbuffer so it can be copied into them.
	if (vr_engine_start(HW.pDevice, HW.m_ChainDesc.Width, HW.m_ChainDesc.Height, (int)HW.m_ChainDesc.Format))
    {
        vr_engine_dimensions(dwWidth, dwHeight);
        fWidth_2 = float(dwWidth) / 2; fHeight_2 = float(dwHeight) / 2;
    }
    else
    {
        const bool hdr_was_disabled = ps_r4_hdr10_on == 0;
        vr_renderer_mode(false);
        if (hdr_was_disabled && ps_r4_hdr10_on) HW.Reset(hWnd);
    }
#endif
}

void dxRenderDeviceRender::SetupGPU(BOOL bForceGPU_SW, BOOL bForceGPU_NonPure, BOOL bForceGPU_REF)
{
	HW.Caps.bForceGPU_SW = bForceGPU_SW;
	HW.Caps.bForceGPU_NonPure = bForceGPU_NonPure;
	HW.Caps.bForceGPU_REF = bForceGPU_REF;
}

void dxRenderDeviceRender::overdrawBegin()
{
#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement overdrawBegin
	VERIFY(!"dxRenderDeviceRender::overdrawBegin not implemented.");
#else	//	USE_DX10
	// Turn stenciling
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILENABLE, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_ALWAYS ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILREF, 0 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILMASK, 0x00000000 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILWRITEMASK, 0xffffffff ));

	// Increment the stencil buffer for each pixel drawn
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILPASS, D3DSTENCILOP_INCRSAT ));

	if (1 == HW.Caps.SceneMode)
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP ));
	} // Overdraw
	else
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILZFAIL, D3DSTENCILOP_INCRSAT ));
	} // ZB access
#endif	//	USE_DX10
}

void dxRenderDeviceRender::overdrawEnd()
{
#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement overdrawEnd
	VERIFY(!"dxRenderDeviceRender::overdrawBegin not implemented.");
#else	//	USE_DX10
	// Set up the stencil states
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILPASS, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_EQUAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILMASK, 0xff ));

	// Set the background to black
	CHK_DX(HW.pDevice->Clear(0,0,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),0,0));

	// Draw a rectangle wherever the count equal I
	RCache.OnFrameEnd();
	CHK_DX(HW.pDevice->SetFVF( FVF::F_TL ));

	// Render gradients
	for (int I = 0; I < 12; I++)
	{
		u32 _c = I * 256 / 13;
		u32 c = D3DCOLOR_XRGB(_c, _c, _c);

		FVF::TL pv[4];
		pv[0].set(float(0), float(Device.dwHeight), c, 0, 0);
		pv[1].set(float(0), float(0), c, 0, 0);
		pv[2].set(float(Device.dwWidth), float(Device.dwHeight), c, 0, 0);
		pv[3].set(float(Device.dwWidth), float(0), c, 0, 0);

		CHK_DX(HW.pDevice->SetRenderState ( D3DRS_STENCILREF, I ));
		CHK_DX(HW.pDevice->DrawPrimitiveUP ( D3DPT_TRIANGLESTRIP, 2, pv, sizeof(FVF::TL) ));
	}
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILENABLE, FALSE ));
#endif	//	USE_DX10
}

void dxRenderDeviceRender::DeferredLoad(BOOL E)
{
	Resources->DeferredLoad(E);
}

void dxRenderDeviceRender::ResourcesDeferredUpload()
{
	Resources->DeferredUpload();
}

void dxRenderDeviceRender::ResourcesDeferredUnload()
{
	Resources->DeferredUnload();
}

void dxRenderDeviceRender::ResourcesPrefetchCreateTexture(LPCSTR name)
{
	Resources->_CreateTexture(name);
}

void dxRenderDeviceRender::ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps)
{
	if (Resources)
		Resources->_GetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);
}

void dxRenderDeviceRender::ResourcesStoreNecessaryTextures()
{
	dxRenderDeviceRender::Instance().Resources->StoreNecessaryTextures();
}

void dxRenderDeviceRender::ResourcesDumpMemoryUsage()
{
	dxRenderDeviceRender::Instance().Resources->_DumpMemoryUsage();
}

dxRenderDeviceRender::DeviceState dxRenderDeviceRender::GetDeviceState()
{
	HW.Validate();
#if defined(USE_DX10) || defined(USE_DX11)
    HRESULT _hr = HW.m_pSwapChain->Present(0, DXGI_PRESENT_TEST);

	if (FAILED(_hr))
	{
		//LV: Check if it's correct. If so - that should be fix for AMD issues. Thank me later. Yo
		// If the device was lost, do not render until we get it back
		if (DXGI_ERROR_DEVICE_REMOVED == _hr)
			return dsLost;

		// Check if the device is ready to be reset
		if (DXGI_ERROR_DEVICE_RESET == _hr)
			return dsNeedReset;
	}
#else	//	USE_DX10
	HRESULT _hr = HW.pDevice->TestCooperativeLevel();

	if (FAILED(_hr))
	{
		// If the device was lost, do not render until we get it back
		if (D3DERR_DEVICELOST == _hr)
			return dsLost;

		// Check if the device is ready to be reset
		if (D3DERR_DEVICENOTRESET == _hr)
			return dsNeedReset;
	}
#endif	//	USE_DX10

	return dsOK;
}

BOOL dxRenderDeviceRender::GetForceGPU_REF()
{
	return HW.Caps.bForceGPU_REF;
}

u32 dxRenderDeviceRender::GetCacheStatPolys()
{
	return RCache.stat.polys;
}

void dxRenderDeviceRender::Begin()
{
#if !defined(USE_DX10) && !defined(USE_DX11)
	CHK_DX(HW.pDevice->BeginScene());
#endif	//	USE_DX10
	RCache.OnFrameBegin();
	RCache.set_CullMode(CULL_CW);
	RCache.set_CullMode(CULL_CCW);
	if (HW.Caps.SceneMode) overdrawBegin();
	bus_textures_update();
}

void dxRenderDeviceRender::Clear()
{
#if defined(USE_DX10) || defined(USE_DX11)
	HW.pContext->ClearDepthStencilView(RCache.get_ZB(),
	                                   D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);

	if (psDeviceFlags.test(rsClearBB))
	{
		FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
	}
#else	//	USE_DX10
	CHK_DX(HW.pDevice->Clear(0,0,
		D3DCLEAR_ZBUFFER|
		(psDeviceFlags.test(rsClearBB)?D3DCLEAR_TARGET:0)|
		(HW.Caps.bStencil?D3DCLEAR_STENCIL:0),
		D3DCOLOR_XRGB(0,0,0),1,0
	));
#endif	//	USE_DX10
}

void DoAsyncScreenshot();

void dxRenderDeviceRender::End()
{
	VERIFY(HW.pDevice);

	if (HW.Caps.SceneMode) overdrawEnd();

	RCache.OnFrameEnd();
	Memory.dbg_check();

	DoAsyncScreenshot();

#if defined(USE_DX10) || defined(USE_DX11)
    UINT present_flags = 0;
	bool use_vsync = !!psDeviceFlags.test(rsVSync);
	UINT present_interval = (!g_vr_owns_present && use_vsync) ? 1 : 0;

# if defined(USE_DX11)
	// NOTE: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
    if (g_vr_owns_present) present_flags |= DXGI_PRESENT_DO_NOT_WAIT;
    BOOL is_windowed = HW.m_ChainDescFullscreen.Windowed;
	if (is_windowed && !use_vsync && HW.m_SupportsVRR) {
        present_flags |= DXGI_PRESENT_ALLOW_TEARING;
	}
# endif

	// The runtime presents the headset. The desktop mirror may skip a busy
	// present rather than pacing the headset against the desktop monitor.
	if (g_vr_owns_present || (!Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady)) {
		HW.m_pSwapChain->Present(present_interval, present_flags);
	}
#else //!USE_DX10 || USE_DX11
	CHK_DX(HW.pDevice->EndScene());

	if (!Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady)
		HW.pDevice->Present(NULL, NULL, NULL, NULL);
#endif //-USE_DX10
	//HRESULT _hr		= HW.pDevice->Present( NULL, NULL, NULL, NULL );
	//if				(D3DERR_DEVICELOST==_hr)	return;			// we will handle this later
}

void dxRenderDeviceRender::ResourcesDestroyNecessaryTextures()
{
	Resources->DestroyNecessaryTextures();
}

void dxRenderDeviceRender::ClearTarget()
{
#if defined(USE_DX10) || defined(USE_DX11)
	FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
#else	//	USE_DX10
	CHK_DX(HW.pDevice->Clear(0,0,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,0),1,0));
#endif	//	USE_DX10
}

void dxRenderDeviceRender::SetCacheXform(Fmatrix& mView, Fmatrix& mProject)
{
	RCache.set_xform_view(mView);
	RCache.set_xform_project(mProject);
}

void dxRenderDeviceRender::SetCacheXform_prev(Fmatrix& mView, Fmatrix& mProject)
{
	RCache.set_xform_view_prev(mView);
	RCache.set_xform_project_prev(mProject);
}

bool dxRenderDeviceRender::HWSupportsShaderYUV2RGB()
{
	u32 v_dev = CAP_VERSION(HW.Caps.raster_major, HW.Caps.raster_minor);
	u32 v_need = CAP_VERSION(2, 0);
	return (v_dev >= v_need);
}

void dxRenderDeviceRender::OnAssetsChanged()
{
	Resources->m_textures_description.UnLoad();
	Resources->m_textures_description.Load();
}

extern ENGINE_API void SetStartupMonitor(HMONITOR h);
extern XRAPI_API xr_token* vid_mode_token;
#if defined(USE_DX11) || defined(USE_DX10)
void fill_vid_mode_list(CHW* _hw);
void free_vid_mode_list();
#endif

// Windowed: keep user's pick if present in vid_mode_token, else monitor native.
// Borderless/fullscreen: always monitor native.  Moves the window to match.
// Caller must have set HW.m_pOutput / vid_mode_token to reflect the target
// monitor before calling.
static void FinalizeMonitorGeometry(const MONITORINFO& mi, HWND hWnd,
                                    u32 g_screenmode_,
                                    u32& vidModeW, u32& vidModeH)
{
    const int monX = mi.rcMonitor.left;
    const int monY = mi.rcMonitor.top;
    const int monW = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    u32 finalW = (u32)monW;
    u32 finalH = (u32)monH;
    if (g_screenmode_ == 0)
    {
        string32 cur_buf;
        xr_sprintf(cur_buf, sizeof(cur_buf), "%ux%u", vidModeW, vidModeH);
        for (xr_token* t = vid_mode_token; t && t->name; ++t)
        {
            if (!xr_strcmp(t->name, cur_buf))
            {
                finalW = vidModeW;
                finalH = vidModeH;
                break;
            }
        }
    }
    vidModeW = finalW;
    vidModeH = finalH;

    int wx, wy, ww = (int)finalW, wh = (int)finalH;
    if (g_screenmode_ == 0)
    {
        wx = monX + (monW - ww) / 2;
        wy = monY + (monH - wh) / 2;
    }
    else
    {
        wx = monX;
        wy = monY;
    }
    SetWindowPos(hWnd, HWND_TOP, wx, wy, ww, wh,
                 SWP_FRAMECHANGED | SWP_NOCOPYBITS | SWP_DRAWFRAME);
}

bool dxRenderDeviceRender::SwitchOutputMonitor(HMONITOR hTargetMon, HWND hWnd,
                                               u32 g_screenmode_,
                                               u32& vidModeW, u32& vidModeH)
{
    if (hTargetMon == NULL)
        return false;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hTargetMon, &mi))
    {
        Msg("! vid_monitor: GetMonitorInfoA failed for target monitor");
        return false;
    }

#if defined(USE_DX11) || defined(USE_DX10)
    IDXGIOutput* new_output = HW.FindOutputOnCurrentAdapter(hTargetMon);
    if (!new_output)
    {
        Msg("! vid_monitor: target monitor is not on the current adapter, restart to apply");
        return false;
    }

    SetStartupMonitor(hTargetMon);

    if (g_screenmode_ == 2)
        HW.m_pSwapChain->SetFullscreenState(FALSE, NULL); // best-effort

    _RELEASE(HW.m_pOutput);
    HW.m_pOutput = new_output;

    free_vid_mode_list();
    fill_vid_mode_list(&HW);

    FinalizeMonitorGeometry(mi, hWnd, g_screenmode_, vidModeW, vidModeH);
    Msg("* vid_monitor: output swapped, final mode %ux%u", vidModeW, vidModeH);
    return true;

#elif !defined(USE_DX10) && !defined(USE_DX11)
    UINT target_adapter = UINT(-1);
    for (UINT a = 0; a < HW.pD3D->GetAdapterCount(); ++a)
    {
        if (HW.pD3D->GetAdapterMonitor(a) == hTargetMon)
        {
            target_adapter = a;
            break;
        }
    }
    if (target_adapter == UINT(-1))
    {
        Msg("! vid_monitor: target monitor not found among D3D9 adapters, restart to apply");
        return false;
    }

    if (target_adapter != HW.DevAdapter)
    {
        D3DADAPTER_IDENTIFIER9 id_cur, id_new;
        HRESULT hr1 = HW.pD3D->GetAdapterIdentifier(HW.DevAdapter, 0, &id_cur);
        HRESULT hr2 = HW.pD3D->GetAdapterIdentifier(target_adapter,  0, &id_new);
        const bool same_gpu = SUCCEEDED(hr1) && SUCCEEDED(hr2)
                           && memcmp(&id_cur.DeviceIdentifier,
                                     &id_new.DeviceIdentifier,
                                     sizeof(GUID)) == 0;
        if (!same_gpu)
        {
            Msg("! vid_monitor: target monitor is on a different D3D9 adapter, restart to apply");
            return false;
        }
    }
    if (g_screenmode_ == 2)
    {
        Msg("! vid_monitor: DX9 exclusive fullscreen across monitors requires restart to apply");
        return false;
    }

    SetStartupMonitor(hTargetMon);

    FinalizeMonitorGeometry(mi, hWnd, g_screenmode_, vidModeW, vidModeH);
    Msg("* vid_monitor: DX9 output swapped, final mode %ux%u", vidModeW, vidModeH);
    return true;

#else
    return false;
#endif
}

// Blit the current engine backbuffer into a destination texture of identical
// size and format (used to present a rendered eye to OpenXR). Declared in the
// engine's device.h and compiled for every configuration.
#if defined(USE_DX11)
#include <d3dcompiler.h>
namespace
{
    ID3D11RenderTargetView* vr_desktop_rt = nullptr;
    ID3D11DepthStencilView* vr_desktop_depth = nullptr;
    ID3D11DepthStencilView* vr_depth = nullptr;
    unsigned vr_depth_width = 0, vr_depth_height = 0;
    ID3D11VertexShader* vr_blit_vs = nullptr;
    ID3D11PixelShader* vr_blit_ps = nullptr;
    ID3D11SamplerState* vr_blit_sampler = nullptr;
    ID3D11BlendState* vr_blit_blend = nullptr;
    bool vr_settings_saved = false;
    u32 vr_saved_msaa = 0;
    int vr_saved_hdr = 0;
    // The magnified scope's view (rendered like an eye) and its lens.
    ID3D11Texture2D* vr_scope_texture = nullptr;
    ID3D11RenderTargetView* vr_scope_rtv = nullptr;
    ID3D11ShaderResourceView* vr_scope_srv = nullptr;
    unsigned vr_scope_width = 0, vr_scope_height = 0;
    // Its own depth: a scope view smaller than the eye would otherwise
    // recreate the eye's twice a frame.
    ID3D11DepthStencilView* vr_scope_depth = nullptr;
    unsigned vr_scope_depth_width = 0, vr_scope_depth_height = 0;
    ID3D11VertexShader* vr_lens_vs = nullptr;
    ID3D11PixelShader* vr_lens_ps = nullptr;
    ID3D11Buffer* vr_lens_constants = nullptr;
    ID3D11RasterizerState* vr_lens_raster = nullptr;
    ID3D11SamplerState* vr_lens_sampler = nullptr;
    ID3D11BlendState* vr_lens_add = nullptr;
    ID3D11DepthStencilState* vr_lens_depth = nullptr;
    // Comfort vignette: darkens the eye beyond a view angle.
    ID3D11VertexShader* vr_vignette_vs = nullptr;
    ID3D11PixelShader* vr_vignette_ps = nullptr;
    ID3D11Buffer* vr_vignette_constants = nullptr;
    ID3D11BlendState* vr_vignette_blend = nullptr;
    void vr_vignette_release()
    {
        _RELEASE(vr_vignette_vs); _RELEASE(vr_vignette_ps);
        _RELEASE(vr_vignette_constants); _RELEASE(vr_vignette_blend);
    }
    void vr_lens_release()
    {
        _RELEASE(vr_lens_vs); _RELEASE(vr_lens_ps); _RELEASE(vr_lens_constants);
        _RELEASE(vr_lens_raster); _RELEASE(vr_lens_sampler); _RELEASE(vr_lens_add); _RELEASE(vr_lens_depth);
    }
    void vr_scope_release()
    {
        _RELEASE(vr_scope_srv); _RELEASE(vr_scope_rtv); _RELEASE(vr_scope_texture);
        vr_scope_width = vr_scope_height = 0;
        _RELEASE(vr_scope_depth);
        vr_scope_depth_width = vr_scope_depth_height = 0;
        vr_lens_release();
    }
}
void vr_renderer_mode(bool enabled)
{
    if (enabled && !vr_settings_saved)
    {
        vr_saved_msaa = ps_r3_msaa; vr_saved_hdr = ps_r4_hdr10_on;
        ps_r3_msaa = 0; ps_r4_hdr10_on = 0;
        vr_settings_saved = true;
    }
    else if (!enabled && vr_settings_saved)
    {
        ps_r3_msaa = vr_saved_msaa; ps_r4_hdr10_on = vr_saved_hdr;
        vr_settings_saved = false;
    }
}
bool vr_render_target(void* target, unsigned width, unsigned height)
{
    if (!target)
    {
        if (vr_desktop_rt)
        {
            HW.pBaseRT = vr_desktop_rt; HW.pBaseZB = vr_desktop_depth;
            vr_desktop_rt = nullptr; vr_desktop_depth = nullptr;
            HW.pContext->ClearState();
            RCache.ResetExternalState();
            RCache.set_RT(HW.pBaseRT); RCache.set_ZB(HW.pBaseZB);
            RCache.set_xform_view(Device.mView); RCache.set_xform_project(Device.mProject);
            D3D11_VIEWPORT viewport{0,0,float(HW.m_ChainDesc.Width),float(HW.m_ChainDesc.Height),0,1};
            HW.pContext->RSSetViewports(1, &viewport);
        }
        return true;
    }
    const bool scope = target == vr_scope_rtv;
    ID3D11DepthStencilView*& depth_view = scope ? vr_scope_depth : vr_depth;
    unsigned& depth_width = scope ? vr_scope_depth_width : vr_depth_width;
    unsigned& depth_height = scope ? vr_scope_depth_height : vr_depth_height;
    if (!depth_view || depth_width != width || depth_height != height)
    {
        _RELEASE(depth_view);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width; description.Height = height;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* depth = nullptr;
        HRESULT result = HW.pDevice->CreateTexture2D(&description, nullptr, &depth);
        if (SUCCEEDED(result)) result = HW.pDevice->CreateDepthStencilView(depth, nullptr, &depth_view);
        _RELEASE(depth);
        if (FAILED(result)) { Msg("! [VR] eye depth creation failed: %08x", result); return false; }
        depth_width = width; depth_height = height;
    }
    if (!vr_desktop_rt) { vr_desktop_rt = HW.pBaseRT; vr_desktop_depth = HW.pBaseZB; }
    HW.pBaseRT = static_cast<ID3D11RenderTargetView*>(target); HW.pBaseZB = depth_view;
    // Invalidate assumes an empty context, including all shader resource slots.
    HW.pContext->ClearState();
    RCache.ResetExternalState();
    RCache.set_RT(HW.pBaseRT); RCache.set_RT(nullptr, 1); RCache.set_RT(nullptr, 2); RCache.set_RT(nullptr, 3);
    RCache.set_ZB(depth_view);
    HW.pContext->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    HW.pContext->RSSetViewports(1, &viewport);
    return true;
}
void vr_world_overlay()
{
    RCache.set_RT(HW.pBaseRT); RCache.set_RT(nullptr, 1); RCache.set_RT(nullptr, 2); RCache.set_RT(nullptr, 3);
    RCache.set_ZB(HW.pBaseZB);
    RCache.set_xform_world(Fidentity);
    RCache.set_xform_view(Device.mView); RCache.set_xform_project(Device.mProject);
    RCache.set_Shader(dxRenderDeviceRender::Instance().m_WireShader);
    RCache.set_c("tfactor", 1.f, 1.f, 1.f, 1.f);
    RCache.set_Stencil(FALSE); RCache.set_Z(TRUE); RCache.set_ZFunc(D3DCMP_LESSEQUAL);
    RCache.set_CullMode(CULL_NONE); RCache.set_ColorWriteEnable();
    D3D11_VIEWPORT viewport{0,0,float(Device.dwWidth),float(Device.dwHeight),0,1};
    HW.pContext->RSSetViewports(1, &viewport);
}
void vr_world_overlay_color(u32 color)
{
    RCache.set_c("tfactor", float(color_get_R(color))/255.f, float(color_get_G(color))/255.f,
        float(color_get_B(color))/255.f, float(color_get_A(color))/255.f);
}
void vr_mirror(void* target, bool overlay)
{
    if (!target || !vr_desktop_rt) return;
    if (!vr_blit_vs)
    {
        static const char shader[] =
            "Texture2D image:register(t0); SamplerState linearSampler:register(s0);"
            "struct V {float4 p:SV_Position; float2 uv:TEXCOORD0;};"
            "V vs(uint id:SV_VertexID){V o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}"
            "float4 ps(V i):SV_Target{return image.Sample(linearSampler,i.uv);}";
        ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
        HRESULT result = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, nullptr);
        if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, nullptr);
        if (SUCCEEDED(result)) result = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vr_blit_vs);
        if (SUCCEEDED(result)) result = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &vr_blit_ps);
        _RELEASE(vs); _RELEASE(ps);
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateSamplerState(&sampler, &vr_blit_sampler);
        D3D11_BLEND_DESC blend{};
        auto& rt = blend.RenderTarget[0]; rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateBlendState(&blend, &vr_blit_blend);
        if (FAILED(result)) { Msg("! [VR] mirror shader initialization failed: %08x", result); vr_renderer_release(); return; }
    }
    ID3D11Resource* source = nullptr;
    static_cast<ID3D11RenderTargetView*>(target)->GetResource(&source);
    ID3D11ShaderResourceView* texture = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    static_cast<ID3D11RenderTargetView*>(target)->GetDesc(&view);
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = view.Format; description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipLevels = 1;
    HRESULT result = HW.pDevice->CreateShaderResourceView(source, &description, &texture);
    _RELEASE(source);
    if (FAILED(result)) return;
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT previous_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    HW.pContext->RSGetViewports(&viewport_count, previous_viewports);
    HW.pContext->ClearState();
    HW.pContext->OMSetRenderTargets(1, &vr_desktop_rt, nullptr);
    D3D11_VIEWPORT viewport{0,0,float(HW.m_ChainDesc.Width),float(HW.m_ChainDesc.Height),0,1};
    HW.pContext->RSSetViewports(1, &viewport);
    HW.pContext->RSSetState(nullptr);
    HW.pContext->OMSetDepthStencilState(nullptr, 0);
    HW.pContext->OMSetBlendState(overlay ? vr_blit_blend : nullptr, nullptr, 0xffffffff);
    HW.pContext->IASetInputLayout(nullptr);
    HW.pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    HW.pContext->VSSetShader(vr_blit_vs, nullptr, 0);
    HW.pContext->PSSetShader(vr_blit_ps, nullptr, 0);
    HW.pContext->GSSetShader(nullptr, nullptr, 0);
    HW.pContext->HSSetShader(nullptr, nullptr, 0); HW.pContext->DSSetShader(nullptr, nullptr, 0);
    HW.pContext->PSSetSamplers(0, 1, &vr_blit_sampler);
    HW.pContext->PSSetShaderResources(0, 1, &texture);
    HW.pContext->Draw(3, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    HW.pContext->PSSetShaderResources(0, 1, &empty);
    _RELEASE(texture);
    HW.pContext->ClearState();
    RCache.ResetExternalState();
    RCache.set_RT(HW.pBaseRT); RCache.set_ZB(HW.pBaseZB);
    RCache.set_xform_view(Device.mView); RCache.set_xform_project(Device.mProject);
    HW.pContext->RSSetViewports(viewport_count, previous_viewports);
}
bool vr_scope_target(unsigned width, unsigned height)
{
    if (!vr_scope_rtv || vr_scope_width != width || vr_scope_height != height)
    {
        _RELEASE(vr_scope_srv); _RELEASE(vr_scope_rtv); _RELEASE(vr_scope_texture);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width; description.Height = height;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = HW.pDevice->CreateTexture2D(&description, nullptr, &vr_scope_texture);
        if (SUCCEEDED(result)) result = HW.pDevice->CreateRenderTargetView(vr_scope_texture, nullptr, &vr_scope_rtv);
        if (SUCCEEDED(result)) result = HW.pDevice->CreateShaderResourceView(vr_scope_texture, nullptr, &vr_scope_srv);
        if (FAILED(result))
        {
            Msg("! [VR] scope target creation failed: %08x", result);
            _RELEASE(vr_scope_srv); _RELEASE(vr_scope_rtv); _RELEASE(vr_scope_texture);
            return false;
        }
        vr_scope_width = width; vr_scope_height = height;
    }
    return vr_render_target(vr_scope_rtv, width, height);
}
void vr_scope_lens(const float* clip, const float* params)
{
    // Mode 0: the scope's last view on a disc at the eyepiece, drawn into
    // the eye being rendered: a centred square of it (the view's aspect), a
    // duplex crosshair at least a pixel wide, shaded where the eye is off
    // the exit pupil (offset) and by brightness. Mode 1: a red dot, added
    // over the scene. params[3]: test against the scene's depth (no writes).
    const bool dot = params[4] > 0.5f;
    if (!dot && !vr_scope_srv) return;
    // Set up once: a failure is not retried (and logged) every eye of every frame.
    static bool failed = false;
    if (failed) return;
    if (!vr_lens_vs)
    {
        static const char shader[] =
            "cbuffer C:register(b0){float4 corner[4]; float4 params; float4 extra;};"
            "Texture2D image:register(t0); SamplerState linearSampler:register(s0);"
            "struct V {float4 p:SV_Position; float2 uv:TEXCOORD0;};"
            "V vs(uint id:SV_VertexID){V o; o.p=corner[id]; o.uv=float2(id&1,(id>>1)&1); return o;}"
            "float4 ps(V i):SV_Target{"
            " float2 q=i.uv*2-1; float r=length(q);"
            " if(extra.y>0.5){"
            "  float a=params.x*(exp(-r*r/0.12)+0.3*exp(-r*r/0.5))*saturate(1-r);"
            "  return float4(float3(1,0.13,0.08)*a*1.6,a);}"
            " if(r>1) discard;"
            " float3 c=image.Sample(linearSampler,float2(0.5+(i.uv.x-0.5)*extra.x,i.uv.y)).rgb;"
            " float px=max(fwidth(q.x),fwidth(q.y));"
            " float w1=max(0.005,px*0.75), w2=max(0.02,px*1.5);"
            " float thin=(abs(q.x)<w1||abs(q.y)<w1)?1:0;"
            " float thick=((abs(q.x)<w2&&abs(q.y)>0.55)||(abs(q.y)<w2&&abs(q.x)>0.55))?1:0;"
            " c*=1-max(thin,thick);"
            " float pupil=saturate((1.2-length(q+params.yz))/0.25);"
            " c*=params.x*pupil*saturate((1-r)/0.08);"
            " return float4(c,1);}";
        ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
        HRESULT result = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, nullptr);
        if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, nullptr);
        if (SUCCEEDED(result)) result = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vr_lens_vs);
        if (SUCCEEDED(result)) result = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &vr_lens_ps);
        _RELEASE(vs); _RELEASE(ps);
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(float) * 24; buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateBuffer(&buffer, nullptr, &vr_lens_constants);
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateRasterizerState(&raster, &vr_lens_raster);
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateSamplerState(&sampler, &vr_lens_sampler);
        D3D11_BLEND_DESC blend{};
        auto& add = blend.RenderTarget[0]; add.BlendEnable = TRUE;
        add.SrcBlend = add.DestBlend = D3D11_BLEND_ONE; add.BlendOp = D3D11_BLEND_OP_ADD;
        add.SrcBlendAlpha = D3D11_BLEND_ZERO; add.DestBlendAlpha = D3D11_BLEND_ONE; add.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        add.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateBlendState(&blend, &vr_lens_add);
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateDepthStencilState(&depth, &vr_lens_depth);
        if (FAILED(result))
        {
            Msg("! [VR] scope lens initialization failed: %08x", result);
            vr_lens_release();
            failed = true;
            return;
        }
    }
    float constants[24];
    for (int i = 0; i < 16; ++i) constants[i] = clip[i];
    for (int i = 0; i < 4; ++i) constants[16 + i] = params[i];
    constants[20] = vr_scope_width ? float(vr_scope_height) / float(vr_scope_width) : 1.f;
    constants[21] = dot ? 1.f : 0.f;
    constants[22] = constants[23] = 0.f;
    HW.pContext->UpdateSubresource(vr_lens_constants, 0, nullptr, constants, 0, 0);
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT previous_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    HW.pContext->RSGetViewports(&viewport_count, previous_viewports);
    HW.pContext->ClearState();
    const bool depth_test = params[3] > 0.5f && HW.pBaseZB;
    HW.pContext->OMSetRenderTargets(1, &HW.pBaseRT, depth_test ? HW.pBaseZB : nullptr);
    HW.pContext->RSSetViewports(viewport_count, previous_viewports);
    HW.pContext->RSSetState(vr_lens_raster);
    HW.pContext->OMSetDepthStencilState(depth_test ? vr_lens_depth : nullptr, 0);
    HW.pContext->OMSetBlendState(dot ? vr_lens_add : nullptr, nullptr, 0xffffffff);
    HW.pContext->IASetInputLayout(nullptr);
    HW.pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    HW.pContext->VSSetShader(vr_lens_vs, nullptr, 0);
    HW.pContext->VSSetConstantBuffers(0, 1, &vr_lens_constants);
    HW.pContext->PSSetShader(vr_lens_ps, nullptr, 0);
    HW.pContext->PSSetConstantBuffers(0, 1, &vr_lens_constants);
    HW.pContext->PSSetSamplers(0, 1, &vr_lens_sampler);
    if (!dot) HW.pContext->PSSetShaderResources(0, 1, &vr_scope_srv);
    HW.pContext->Draw(4, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    HW.pContext->PSSetShaderResources(0, 1, &empty);
    HW.pContext->ClearState();
    RCache.ResetExternalState();
    RCache.set_RT(HW.pBaseRT); RCache.set_ZB(HW.pBaseZB);
    RCache.set_xform_view(Device.mView); RCache.set_xform_project(Device.mProject);
    HW.pContext->RSSetViewports(viewport_count, previous_viewports);
}
void vr_comfort_vignette(const float* params)
{
    // A full-eye triangle. Each pixel's angle from the eye's optical axis
    // comes from its projection (asymmetric per eye), so both eyes darken
    // the same directions. params: _11, _22, _31, _32 of the projection,
    // the strength (0 none .. 1 a narrow tunnel), and a fade of the whole eye
    // to black (0..1).
    static bool failed = false;
    if (failed) return;
    if (!vr_vignette_vs)
    {
        static const char shader[] =
            "cbuffer C:register(b0){float4 projection; float4 params;};"
            "struct V {float4 p:SV_Position; float2 ndc:TEXCOORD0;};"
            "V vs(uint id:SV_VertexID){V o; float2 uv=float2((id<<1)&2,id&2);"
            " o.ndc=uv*float2(2,-2)+float2(-1,1); o.p=float4(o.ndc,0,1); return o;}"
            "float4 ps(V i):SV_Target{"
            " float2 t=float2((i.ndc.x-projection.z)/projection.x,(i.ndc.y-projection.w)/projection.y);"
            " float angle=atan(length(t)); float s=saturate(params.x);"
            " float inner=radians(55-35*s);"
            " float tunnel=s>0.001?smoothstep(inner,inner+radians(15),angle):0;"
            " return float4(0,0,0,max(tunnel,saturate(params.y)));}";
        ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
        HRESULT result = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, nullptr);
        if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, nullptr);
        if (SUCCEEDED(result)) result = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vr_vignette_vs);
        if (SUCCEEDED(result)) result = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &vr_vignette_ps);
        _RELEASE(vs); _RELEASE(ps);
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(float) * 8; buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateBuffer(&buffer, nullptr, &vr_vignette_constants);
        D3D11_BLEND_DESC blend{};
        auto& darken = blend.RenderTarget[0]; darken.BlendEnable = TRUE;
        darken.SrcBlend = D3D11_BLEND_ZERO; darken.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; darken.BlendOp = D3D11_BLEND_OP_ADD;
        darken.SrcBlendAlpha = D3D11_BLEND_ZERO; darken.DestBlendAlpha = D3D11_BLEND_ONE; darken.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        darken.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) result = HW.pDevice->CreateBlendState(&blend, &vr_vignette_blend);
        if (FAILED(result))
        {
            Msg("! [VR] comfort vignette initialization failed: %08x", result);
            vr_vignette_release();
            failed = true;
            return;
        }
    }
    float constants[8] = {params[0], params[1], params[2], params[3], params[4], params[5], 0.f, 0.f};
    HW.pContext->UpdateSubresource(vr_vignette_constants, 0, nullptr, constants, 0, 0);
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT previous_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    HW.pContext->RSGetViewports(&viewport_count, previous_viewports);
    HW.pContext->ClearState();
    HW.pContext->OMSetRenderTargets(1, &HW.pBaseRT, nullptr);
    HW.pContext->RSSetViewports(viewport_count, previous_viewports);
    HW.pContext->OMSetBlendState(vr_vignette_blend, nullptr, 0xffffffff);
    HW.pContext->IASetInputLayout(nullptr);
    HW.pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    HW.pContext->VSSetShader(vr_vignette_vs, nullptr, 0);
    HW.pContext->PSSetShader(vr_vignette_ps, nullptr, 0);
    HW.pContext->PSSetConstantBuffers(0, 1, &vr_vignette_constants);
    HW.pContext->Draw(3, 0);
    HW.pContext->ClearState();
    RCache.ResetExternalState();
    RCache.set_RT(HW.pBaseRT); RCache.set_ZB(HW.pBaseZB);
    RCache.set_xform_view(Device.mView); RCache.set_xform_project(Device.mProject);
    HW.pContext->RSSetViewports(viewport_count, previous_viewports);
}
void vr_copy_eye_depth(void* texture)
{
    if (!texture || !vr_depth) return;
    ID3D11Resource* source = nullptr;
    vr_depth->GetResource(&source);
    // Same size and the D24S8 format family (the runtime's may be typeless).
    if (source) HW.pContext->CopyResource(static_cast<ID3D11Resource*>(texture), source);
    _RELEASE(source);
}
void vr_ui_blend_release();
void vr_renderer_release()
{
    vr_render_target(nullptr, 0, 0);
    vr_ui_blend_release();
    vr_scope_release();
    vr_vignette_release();
    _RELEASE(vr_depth); _RELEASE(vr_blit_vs); _RELEASE(vr_blit_ps);
    _RELEASE(vr_blit_sampler); _RELEASE(vr_blit_blend);
    vr_renderer_mode(false);
}
bool vr_scope_resize_begin(unsigned size)
{
    return RImplementation.Target && RImplementation.Target->vr_scope_begin(size);
}
void vr_scope_resize_end()
{
    if (RImplementation.Target) RImplementation.Target->vr_scope_end();
}
#else
bool vr_render_target(void*, unsigned, unsigned) { return false; }
bool vr_scope_target(unsigned, unsigned) { return false; }
bool vr_scope_resize_begin(unsigned) { return false; }
void vr_scope_resize_end() {}
void vr_scope_lens(const float*, const float*) {}
void vr_comfort_vignette(const float*) {}
void vr_copy_eye_depth(void*) {}
void vr_mirror(void*, bool) {}
void vr_world_overlay() {}
void vr_world_overlay_color(u32) {}
void vr_renderer_release() {}
void vr_renderer_mode(bool) {}
#endif
