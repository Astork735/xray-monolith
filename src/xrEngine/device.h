#ifndef xr_device
#define xr_device
#pragma once

// Note:
// ZNear - always 0.0f
// ZFar - always 1.0f

//class ENGINE_API CResourceManager;
//class ENGINE_API CGammaControl;

#include "pure.h"
//#include "hw.h"
#include "../xrcore/ftimer.h"
#include "stats.h"
//#include "shader.h"
//#include "R_Backend.h"

#include "../build_config_defines.h"

#define VIEWPORT_NEAR  Device.ViewportNear //0.2f
#define R_VIEWPORT_NEAR 0.005f

#define DEVICE_RESET_PRECACHE_FRAME_COUNT 10

// demonized: toggle bone optimization
#define OPTIMIZE_CALCULATE_BONES

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/RenderDeviceRender.h"
#include "imgui_base.h"

#ifdef INGAME_EDITOR
# include "../Include/editor/interfaces.hpp"
#endif // #ifdef INGAME_EDITOR

class engine_impl;

#pragma pack(push,4)

class IRenderDevice
{
public:
	virtual CStatsPhysics* _BCL StatPhysics() = 0;
	virtual void _BCL AddSeqFrame(pureFrame* f, bool mt) = 0;
	virtual void _BCL RemoveSeqFrame(pureFrame* f) = 0;
};

class ENGINE_API CRenderDeviceData
{
public:
	u32 dwWidth;
	u32 dwHeight;
	u32 clientWidth;
	u32 clientHeight;

	u32 dwPrecacheFrame;
	BOOL b_is_Ready;
	BOOL b_is_Active;
	BOOL b_hide_cursor;
public:

	// Engine flow-control
	u32 dwFrame;

	float fTimeDelta;
	float fTimeGlobal;
	u32 dwTimeDelta;
	u32 dwTimeGlobal;
	u32 dwTimeContinual;

	Fvector vCameraPosition;
	Fvector vCameraDirection;
	Fvector vCameraTop;
	Fvector vCameraRight;

	Fmatrix mView;
	Fmatrix mProject;
	Fmatrix mProjectHud;
	Fmatrix mFullTransform;
	Fmatrix mFullTransformHud;

	Fmatrix mView_prev;
	Fmatrix mProject_prev;

	Fvector4 wind_anim_prev;
	Fvector4 wind_anim_saved;

	// Copies of corresponding members. Used for synchronization.
	Fvector vCameraPosition_saved;

	Fmatrix mView_saved;
	Fmatrix mProject_saved;
	Fmatrix mFullTransform_saved;

	float fFOV;
	float fASPECT;
	float ViewportNear = 0.2f;
protected:

	u32 Timer_MM_Delta;
	CTimer_paused Timer;
	CTimer_paused TimerGlobal;

	//AVO: 
	CTimer frame_timer; //TODO: ïðîâåðèòü, íå äóáëèðóåòñÿ-ëè ñõîæèé òàéìåð (alpet)
	//-AVO

public:

	// Registrators
	CRegistrator<pureRender> seqRender;
	CRegistrator<pureAppActivate> seqAppActivate;
	CRegistrator<pureAppDeactivate> seqAppDeactivate;
	CRegistrator<pureAppStart> seqAppStart;
	CRegistrator<pureAppEnd> seqAppEnd;
	CRegistrator<pureFrame> seqFrame;
	CRegistrator<pureScreenResolutionChanged> seqResolutionChanged;

	HWND m_hWnd;
	// CStats* Statistic;
};

class ENGINE_API CRenderDeviceBase :
	public IRenderDevice,
	public CRenderDeviceData
{
public:
};

#pragma pack(pop)
// refs
class ENGINE_API CRenderDevice : public CRenderDeviceBase
{
public:
	class ENGINE_API CSecondVPParams //--#SM+#-- +SecondVP+
	{
		bool isActive; // Oeaa aeoeaaoee ?aiaa?a ai aoi?ie au?ii?o
		u8 frameDelay;  // Ia eaeii eaa?a n iiiaioa i?ioeiai ?aiaa?a ai aoi?ie au?ii?o iu ia?i?i iiaue
						  //(ia ii?ao auou iaiuoa 2 - ea?aue aoi?ie eaa?, ?ai aieuoa oai aieaa ieceee FPS ai aoi?ii au?ii?oa)

	public:
		bool isCamReady; // Oeaa aioiaiinoe eaia?u (FOV, iiceoey, e o.i) e ?aiaa?o aoi?iai au?ii?oa

		IC bool IsSVPActive() { return isActive; }
		void SetSVPActive(bool bState);
		bool    IsSVPFrame();

		IC u8 GetSVPFrameDelay() { return frameDelay; }
		void  SetSVPFrameDelay(u8 iDelay)
		{
			frameDelay = iDelay;
			clamp<u8>(frameDelay, 2, u8(-1));
		}
	};	
	
private:
	// Main objects used for creating and rendering the 3D scene
	u32 m_dwWindowStyle;
	RECT m_rcWindowBounds;
	RECT m_rcWindowClient;

	//u32 Timer_MM_Delta;
	//CTimer_paused Timer;
	//CTimer_paused TimerGlobal;
	CTimer TimerMM;

	void _Create(LPCSTR shName);
	void _Destroy(BOOL bKeepTextures);
	void _SetupStates();
public:
	// HWND m_hWnd;
	LRESULT MsgProc(HWND, UINT, WPARAM, LPARAM);

	// u32 dwFrame;
	// u32 dwPrecacheFrame;
	u32 dwPrecacheTotal;

	// u32 dwWidth, dwHeight;
	float fWidth_2, fHeight_2;
	// BOOL b_is_Ready;
	// BOOL b_is_Active;
	void OnWM_Activate(WPARAM wParam, LPARAM lParam);
public:
	//ref_shader m_WireShader;
	//ref_shader m_SelectionShader;

	IRenderDeviceRender* m_pRender;

	BOOL m_bNearer;

	void SetNearer(BOOL enabled)
	{
		if (enabled && !m_bNearer)
		{
			m_bNearer = TRUE;
			mProject._43 -= EPS_L;
		}
		else if (!enabled && m_bNearer)
		{
			m_bNearer = FALSE;
			mProject._43 += EPS_L;
		}
		m_pRender->SetCacheXform(mView, mProject);
		//R_ASSERT(0);
		// TODO: re-implement set projection
		//RCache.set_xform_project (mProject);
	}

	void DumpResourcesMemoryUsage() { m_pRender->ResourcesDumpMemoryUsage(); }
public:
	// Registrators
	//CRegistrator <pureRender > seqRender;
	// CRegistrator <pureAppActivate > seqAppActivate;
	// CRegistrator <pureAppDeactivate > seqAppDeactivate;
	// CRegistrator <pureAppStart > seqAppStart;
	// CRegistrator <pureAppEnd > seqAppEnd;
	//CRegistrator <pureFrame > seqFrame;
	CRegistrator<pureFrame> seqFrameMT;
	CRegistrator<pureDeviceReset> seqDeviceReset;
	xr_vector<fastdelegate::FastDelegate0<>> seqParallel;

	bool isRendering;

	// LuaGC
	int LuaGCCount;
	bool LuaGCDone;
	fastdelegate::FastDelegate0<int> LuaGC;
	fastdelegate::FastDelegate0<void> LuaGCDebug;

	// Dependent classes
	//CResourceManager* Resources;

	CStats* Statistic;

	// Engine flow-control
	//float fTimeDelta;
	//float fTimeGlobal;
	//u32 dwTimeDelta;
	//u32 dwTimeGlobal;
	//u32 dwTimeContinual;

	// Cameras & projection
	//Fvector vCameraPosition;
	//Fvector vCameraDirection;
	//Fvector vCameraTop;
	//Fvector vCameraRight;

	//Fmatrix mView;
	//Fmatrix mProject;
	//Fmatrix mFullTransform;

	Fmatrix mInvView;
	Fmatrix mInvProject;
	Fmatrix mInvProjectHud;
	Fmatrix mInvFullTransform;

	CSecondVPParams m_SecondViewport;	//--#SM+#-- +SecondVP+

	//float fFOV;
	//float fASPECT;

	CRenderDevice()
		:
		m_pRender(0)
#ifdef INGAME_EDITOR
        , m_editor_module(0),
        m_editor_initialize(0),
        m_editor_finalize(0),
        m_editor(0),
        m_engine(0)
#endif // #ifdef INGAME_EDITOR
#ifdef PROFILE_CRITICAL_SECTIONS
        ,mt_csEnter(MUTEX_PROFILE_ID(CRenderDevice::mt_csEnter))
        ,mt_csLeave(MUTEX_PROFILE_ID(CRenderDevice::mt_csLeave))
#endif // #ifdef PROFILE_CRITICAL_SECTIONS
	{
		m_hWnd = NULL;
		b_is_Active = FALSE;
		b_is_Ready = FALSE;
		b_hide_cursor = FALSE;
		Timer.Start();
		m_bNearer = FALSE;
		
		m_SecondViewport.SetSVPActive(false);
		m_SecondViewport.SetSVPFrameDelay(2);
		m_SecondViewport.isCamReady = false;			
	};

	void Pause(BOOL bOn, BOOL bTimer, BOOL bSound, LPCSTR reason);
	bool Paused();

	// Scene control
	void PreCache(u32 amount, bool b_draw_loadscreen, bool b_wait_user_input);
	BOOL Begin();
	void Clear();
	void End();
	void FrameMove();

	void overdrawBegin();
	void overdrawEnd();

	//Console Screenshot
	void Screenshot();

	// Mode control
	void DumpFlags();
	IC CTimer_paused* GetTimerGlobal() { return &TimerGlobal; }
	u32 TimerAsync() { return TimerGlobal.GetElapsed_ms(); }
	u32 TimerAsync_MMT() { return TimerMM.GetElapsed_ms() + Timer_MM_Delta; }

	// Creation & Destroying
	void ConnectToRender();
	void Create(void);
	void Run(void);
	void Destroy(void);
	void Reset(bool precache = true);

	bool ChangeOutputMonitor(HMONITOR hTargetMon);

	void Initialize(void);
	void ShutDown(void);

public:
	void time_factor(const float& time_factor)
	{
		Timer.time_factor(time_factor);
		TimerGlobal.time_factor(time_factor);
	}

	IC const float& time_factor() const
	{
		VERIFY(Timer.time_factor() == TimerGlobal.time_factor());
		return (Timer.time_factor());
	}

	Fvector& hud_to_world(Fvector& v, const Fmatrix& p)
	{
		mView.transform_tiny(v);
		p.transform_tiny(v);

		v.z -= ViewportNear;

		mInvProject.transform_tiny(v);
		mInvView.transform_tiny(v);

		return v;
	}

	Fvector& hud_to_world(Fvector& v)
	{
		return hud_to_world(v, mProjectHud);
	}

	Fvector& hud_to_world_dir(Fvector& v, const Fmatrix& p)
	{
		mView.transform_dir(v);
		p.transform_dir(v);

		mInvProject.transform_dir(v);
		mInvView.transform_dir(v);

		return v;
	}

	Fvector& hud_to_world_dir(Fvector& v)
	{
		return hud_to_world_dir(v, mProjectHud);
	}

	Fmatrix& hud_to_world(Fmatrix& m, const Fmatrix& p)
	{
		hud_to_world(m.c, p);
		hud_to_world_dir(m.i, p).normalize();
		hud_to_world_dir(m.j, p).normalize();
		hud_to_world_dir(m.k, p).normalize();
		return m;
	}

	Fmatrix& hud_to_world(Fmatrix& m)
	{
		return hud_to_world(m, mProjectHud);
	}

	Fvector& world_to_hud(Fvector& v, const Fmatrix& p)
	{
		mInvView.transform_tiny(v);
		mInvProject.transform_tiny(v);

		v.z += ViewportNear;

		p.transform_tiny(v);
		mView.transform_tiny(v);
		return v;
	}

	Fvector& world_to_hud(Fvector& v)
	{
		return world_to_hud(v, mProjectHud);
	}

	Fvector& world_to_hud_dir(Fvector& v, const Fmatrix& p)
	{
		mInvView.transform_dir(v);
		mInvProject.transform_dir(v);

		p.transform_dir(v);
		mView.transform_dir(v);

		return v;
	}

	Fvector& world_to_hud_dir(Fvector& v)
	{
		return world_to_hud_dir(v, mProjectHud);
	}

	Fmatrix& world_to_hud(Fmatrix& m, const Fmatrix& p)
	{
		world_to_hud(m.c, p);
		world_to_hud_dir(m.i, p).normalize();
		world_to_hud_dir(m.j, p).normalize();
		world_to_hud_dir(m.k, p).normalize();
		return m;
	}

	Fmatrix& world_to_hud(Fmatrix& m)
	{
		return world_to_hud(m, mProjectHud);
	}

	// Multi-threading
	xrCriticalSection mt_csEnter;
	xrCriticalSection mt_csLeave;
	volatile BOOL mt_bMustExit;

	ICF void remove_from_seq_parallel(const fastdelegate::FastDelegate0<>& delegate)
	{
		xr_vector<fastdelegate::FastDelegate0<>>::iterator I = std::find(
			seqParallel.begin(),
			seqParallel.end(),
			delegate
		);
		if (I != seqParallel.end())
			seqParallel.erase(I);
	}

	//AVO: elapsed famed counter (by alpet)
	IC u32 frame_elapsed()
	{
		return frame_timer.GetElapsed_ms();
	}

	// demonized: Perceivable distance depending on FOV, so that objects will behave normal in binoculars
	IC float GetPerceivedDist(const Fvector& p, float* real_dist = nullptr)
	{
		float dist = vCameraPosition.distance_to(p);
		float fov_rad = deg2rad(fFOV);
		float perceived_dist = dist * tanf(fov_rad * 0.5f);
		if (real_dist) *real_dist = dist;
		return perceived_dist;
	}

	IC float CalcSSADynamic(const Fvector& C, float R)
	{
		Fvector4 v_res1, v_res2;
		mFullTransform.transform(v_res1, C);
		mFullTransform.transform(v_res2, Fvector(C).mad(vCameraRight, R));
		return v_res1.sub(v_res2).magnitude();
	}

public:
	void xr_stdcall on_idle();
	bool xr_stdcall on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
	void message_loop();
	virtual void _BCL AddSeqFrame(pureFrame* f, bool mt);
	virtual void _BCL RemoveSeqFrame(pureFrame* f);
	virtual CStatsPhysics* _BCL StatPhysics() { return Statistic; }

private:
	xr_imgui::ide m_imgui;

public:
	xr_imgui::ide& imgui() { return m_imgui; }
	bool imgui_shown() const { return m_imgui.is_shown(); }
#ifdef INGAME_EDITOR
public:
    IC editor::ide* editor() const { return m_editor; }

private:
    void initialize_editor();
    void message_loop_editor();

private:
    typedef editor::initialize_function_ptr initialize_function_ptr;
    typedef editor::finalize_function_ptr finalize_function_ptr;

private:
    HMODULE m_editor_module;
    initialize_function_ptr m_editor_initialize;
    finalize_function_ptr m_editor_finalize;
    editor::ide* m_editor;
    engine_impl* m_engine;
#endif // #ifdef INGAME_EDITOR
};

extern ENGINE_API CRenderDevice Device;

// Set while native OpenXR VR owns the headset and render resolution.
// The desktop swapchain remains a nonblocking mirror. False outside VR.
extern ENGINE_API bool g_vr_owns_present;

// Called by the render layer (which owns CHW) once the D3D11 device exists, to
// enter native OpenXR VR if it was requested. width/height/dxgi_format describe
// the desktop backbuffer; eye dimensions come from OpenXR. Returns true when a VR
// session was started. Always compiled; returns false otherwise.
extern ENGINE_API bool vr_engine_start(void* d3d11_device, unsigned width, unsigned height, int dxgi_format);

// Renderer/engine boundary, kept free of OpenXR and D3D types for other backends.
extern ENGINE_API bool vr_engine_prepare(unsigned long& low, long& high, unsigned& minimum_feature);
extern ENGINE_API void vr_engine_stop();
extern ENGINE_API void vr_engine_dimensions(unsigned& width, unsigned& height);
extern ENGINE_API bool vr_render_target(void* target, unsigned width, unsigned height);
extern ENGINE_API void vr_mirror(void* target, bool overlay);
extern ENGINE_API void vr_world_overlay();
// The colour (ARGB) of the overlay's next lines and faces: its shader takes
// the colour from a constant rather than the vertices (vr::OverlayDraw).
extern ENGINE_API void vr_world_overlay_color(u32 color);
// Binds the magnified scope's view target (created on demand).
extern ENGINE_API bool vr_scope_target(unsigned width, unsigned height);
// The scope's view renders at size x size between begin and end: the
// renderer's screen-sized targets and the device's size are swapped for
// smaller ones. False when that is not possible (render at eye size).
extern ENGINE_API bool vr_scope_resize_begin(unsigned size);
extern ENGINE_API void vr_scope_resize_end();
// Draws the scope's view on a disc into the current eye: clip holds the four
// corners in clip space (top-left, top-right, bottom-left, bottom-right);
// params: brightness, offset x/y, unused, and mode (0 lens, 1 red dot).
extern ENGINE_API void vr_scope_lens(const float* clip, const float* params);
// Darkens the current eye beyond a view angle: params are the projection's
// _11, _22, _31, _32, the strength (0..1), and a uniform fade to black over
// the whole eye (0..1; a head inside a wall).
extern ENGINE_API void vr_comfort_vignette(const float* params);
// Copies the current eye's depth buffer into the runtime's depth image
// (ID3D11Texture2D of the same size, D24S8 family).
extern ENGINE_API void vr_copy_eye_depth(void* texture);
extern ENGINE_API void vr_renderer_mode(bool enabled);
extern ENGINE_API void vr_renderer_release();
extern ENGINE_API bool g_vr_rendering_eye;
extern ENGINE_API bool g_vr_eye_rendered;
// Sun shadow cascades shared by the eyes (vr_shared_shadows): 0 each pass
// fits and renders its own (desktop, the scope view), 1 the first eye fits
// them to g_vr_shadow_view/project and keeps them, 2 the second eye reuses
// what the first kept this frame.
extern ENGINE_API int g_vr_shadow_share;
// The eye being drawn (0, 1), or -1 (desktop, the scope view). The first eye
// advances the shared exposure (eye adaptation); the rest use its result.
extern ENGINE_API int g_vr_eye_index;
// The VR UI layer is the bound target: UI blending there keeps coverage in
// the alpha channel the compositor blends the layer with.
extern ENGINE_API bool g_vr_ui_pass;
extern ENGINE_API Fmatrix g_vr_shadow_view, g_vr_shadow_project;

#ifndef _EDITOR
#define RDEVICE Device
#else
#define RDEVICE EDevice
#endif

#ifdef ECO_RENDER
extern ENGINE_API float refresh_rate;
#endif // ECO_RENDER

extern ENGINE_API bool g_bBenchmark;

typedef fastdelegate::FastDelegate0<bool> LOADING_EVENT;
extern ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

class ENGINE_API CLoadScreenRenderer : public pureRender
{
public:
	CLoadScreenRenderer();
	void start(bool b_user_input);
	void stop();
	virtual void OnRender();

	bool b_registered;
	bool b_need_user_input;
};

extern ENGINE_API CLoadScreenRenderer load_screen_renderer;
#endif
