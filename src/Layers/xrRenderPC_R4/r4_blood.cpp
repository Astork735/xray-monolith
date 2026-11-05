#include "stdafx.h"

#include "r4_blood.h"
#include "blender_blood.h"
#include "../xrRender/dxRenderDeviceRender.h"

// Frame structure (see docs/VR.md, "Blood system"):
//   FrameUpdate (first eye)  residency -> land due splats -> clear/replay
//                            restored pages -> stamp splats -> pools and
//                            drying -> droplet emission and simulation
//   RenderParticles (eye)    droplets into the G-buffer, indirect draw
//   RenderDecals (eye)       resident pages projected onto the albedo
// Nothing is read back from the GPU except optional statistics, three frames
// late and without waiting.

namespace
{
const u32 kMaxEmitters = 512;
const int kMaxStampsPerFrame = 256;
const int kMaxRestoresPerFrame = 2;
const int kMaxFlowPagesPerFrame = 6;
const u32 kMaxQuadsPerDraw = 1024;
const float kFlowTime = 15.f;		// seconds a page keeps spreading after a deposit
const float kFlowStep = 1.f / 10.f;	// fluid step interval per page
const float kDryStep = 3.f;			// drying-only step interval per page
const float kResidencyMargin = 5.f;	// metres of hysteresis around the resident set
const int kMaxMipsPerFrame = 8;		// deferred mip rebuilds per frame
const float kGravity = 9.81f;
const float kDrag = 0.35f;
const float kBoxMargin = 0.06f;		// decal box beyond the stained surface extent

struct GpuParticle { float pos[3]; float age; float vel[3]; float life; float size; u32 color; u32 seed; float pad; };
struct GpuEmitter { float pos[3]; u32 first; float vel[3]; u32 count; float spread; float life; float size; u32 seed; float pad[4]; };
struct SimConstants
{
	float dt, gravity, drag, time;
	float cam[3], range_sq;
	u32 max_particles, emit_total, emitter_count, frame;
};
struct FlowConstants
{
	float origin_u, origin_v, tile, texel;		// page origin (tiles), tile size, 1/page size
	float dt, dry_rate, flow, axis;				// flow: 0 drying only, 1 spreading
	u32 size;
	float seed;									// differs per step: dithers the drying
	u32 pad1, pad2;
};
static_assert(sizeof(GpuParticle) == 48, "blood particle layout");
static_assert(sizeof(GpuEmitter) == 64, "blood emitter layout");
static_assert(sizeof(SimConstants) % 16 == 0, "blood constants layout");
static_assert(sizeof(FlowConstants) % 16 == 0, "blood constants layout");

D3DVERTEXELEMENT9 stamp_decl[] =
{
	{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},	// page uv, local xy
	{0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},	// amount, wet, seed, kind
	{0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},	// elongation, major, minor scale
	D3DDECL_END()
};
struct StampVertex { Fvector4 p, a, b; };

D3DVERTEXELEMENT9 decal_decl[] =
{
	{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},	// world
	{0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},	// axis, slot, tile u, tile v
	{0, 28, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},	// slab, lo, hi, tile size
	D3DDECL_END()
};
struct DecalVertex { Fvector p; Fvector4 page, extent; };

blood::Vec ToVec(const Fvector& v) { return {v.x, v.y, v.z}; }
Fvector ToF(const blood::Vec& v) { Fvector r; r.set(v.x, v.y, v.z); return r; }

template <class T>
void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3D11Buffer* MakeBuffer(UINT bytes, UINT bind, UINT misc, UINT stride, D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                         UINT cpu = 0, const void* init = nullptr)
{
	D3D11_BUFFER_DESC d = {};
	d.ByteWidth = bytes; d.BindFlags = bind; d.MiscFlags = misc; d.StructureByteStride = stride;
	d.Usage = usage; d.CPUAccessFlags = cpu;
	D3D11_SUBRESOURCE_DATA data = {init, 0, 0};
	ID3D11Buffer* b = nullptr;
	if (FAILED(HW.pDevice->CreateBuffer(&d, init ? &data : nullptr, &b))) return nullptr;
	return b;
}

// Gives a "$user$" texture our resource and view, so blenders bind it by name.
// The texture holds its own references; it is re-bound if the engine dropped them.
void GiveToTexture(ref_texture& t, ID3D11Resource* res, ID3D11ShaderResourceView* srv)
{
	if (!t._get() || t->get_SRView() == srv) return;
	if (res) res->AddRef();
	if (srv) srv->AddRef();
	ID3DBaseTexture* surface = res;
	ID3DShaderResourceView* view = srv;
	t->surface_swap(surface, view);
	Release(surface);
	Release(view);
}
}

// ------------------------------------------------------------------ GPU timer
// Timestamp pairs per section, read four frames later without waiting.
struct CBlood::GpuTimer
{
	enum { Update, Particles, Decals, Sections };
	static const int kFrames = 4, kPairs = 8;
	struct Frame
	{
		ID3D11Query* disjoint = nullptr;
		ID3D11Query* ts[kPairs * 2] = {};
		int section[kPairs] = {};
		int pairs = 0;
		bool issued = false;
	};
	Frame frames[kFrames];
	int current = -1;
	float ms[Sections] = {};
	bool fresh = false;

	bool Create()
	{
		D3D11_QUERY_DESC dj = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts = {D3D11_QUERY_TIMESTAMP, 0};
		for (Frame& f : frames)
		{
			if (FAILED(HW.pDevice->CreateQuery(&dj, &f.disjoint))) return false;
			for (ID3D11Query*& q : f.ts) if (FAILED(HW.pDevice->CreateQuery(&ts, &q))) return false;
		}
		return true;
	}
	void Destroy()
	{
		for (Frame& f : frames) { Release(f.disjoint); for (ID3D11Query*& q : f.ts) Release(q); }
	}
	void BeginFrame()
	{
		if (current >= 0 && frames[current].issued) HW.pContext->End(frames[current].disjoint);
		current = (current + 1) % kFrames;
		Frame& f = frames[current];
		fresh = false;
		if (f.issued) Resolve(f);
		f.pairs = 0;
		f.issued = true;
		HW.pContext->Begin(f.disjoint);
	}
	int Begin(int section)
	{
		if (current < 0) return -1;
		Frame& f = frames[current];
		if (f.pairs >= kPairs) return -1;
		f.section[f.pairs] = section;
		HW.pContext->End(f.ts[f.pairs * 2]);
		return f.pairs++;
	}
	void End(int pair)
	{
		if (pair >= 0) HW.pContext->End(frames[current].ts[pair * 2 + 1]);
	}
	void Resolve(Frame& f)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
		if (HW.pContext->GetData(f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint)
			return;
		float sum[Sections] = {};
		for (int i = 0; i < f.pairs; ++i)
		{
			UINT64 a = 0, b = 0;
			if (HW.pContext->GetData(f.ts[i * 2], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
			if (HW.pContext->GetData(f.ts[i * 2 + 1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
			if (b > a) sum[f.section[i]] += float(double(b - a) * 1000.0 / double(dj.Frequency));
		}
		for (int s = 0; s < Sections; ++s) ms[s] = sum[s];
		fresh = true;
	}
	float Total() const { return ms[Update] + ms[Particles] + ms[Decals]; }
};

// ------------------------------------------------------------------ lifetime
CBlood::CBlood()
{
	m_emits.reserve(kMaxEmitters);
	m_frame_emits.reserve(kMaxEmitters);
	m_pending.reserve(1024);
	m_stamps.reserve(kMaxStampsPerFrame * 2);
	m_b_stamp = xr_new<CBlender_blood_stamp>();
	m_b_decal = xr_new<CBlender_blood_decal>();
	m_b_particle = xr_new<CBlender_blood_particle>();
	m_ready = Create(Wanted());
	if (!m_ready)
	{
		Msg("! [blood] GPU resources unavailable; stock blood stays in use");
		Destroy();
	}
	blood::StoreLimits limits;
	limits.slots = m_config.pages;
	m_store.Reset(limits);
}

CBlood::~CBlood()
{
	Destroy();
	xr_delete(m_b_stamp);
	xr_delete(m_b_decal);
	xr_delete(m_b_particle);
}

CBlood::Config CBlood::Wanted()
{
	const blood::Quality q = blood::QualityPreset(ps_r__blood_quality);
	Config c;
	c.particles = ps_r__blood_particles > 0 ? ps_r__blood_particles : q.particles;
	c.particles = (_max(c.particles, 1024) + 255) / 256 * 256;
	int size = ps_r__blood_page_size > 0 ? ps_r__blood_page_size : q.page_size;
	c.page_size = 64;
	while (c.page_size < size && c.page_size < 1024) c.page_size *= 2;
	c.pages = ps_r__blood_pages > 0 ? ps_r__blood_pages : q.pages;
	clamp(c.pages, 16, 1024);
	c.fluid = q.fluid && ps_r__blood_fluid != 0;
	c.tile = ps_r__blood_tile;
	return c;
}

bool CBlood::Create(const Config& c)
{
	m_config = c;
	// A missing shader would stop the engine at compile time; without the
	// files the stock blood stays in use instead.
	static const char* shaders[][2] = {
		{"blood_common", ".h"}, {"blood_particles", ".h"}, {"blood_stamp", ".vs"}, {"blood_stamp", ".ps"},
		{"blood_decal", ".vs"}, {"blood_decal", ".ps"}, {"blood_particle", ".vs"}, {"blood_particle", ".ps"},
		{"blood_emit", ".cs"}, {"blood_sim", ".cs"}, {"blood_flow", ".cs"}};
	for (const auto& s : shaders)
	{
		string_path name, fn;
		strconcat(sizeof(name), name, ::Render->getShaderPath(), s[0]);
		if (!FS.exist(fn, "$game_shaders$", name, s[1]))
		{
			Msg("! [blood] shader %s%s not found", name, s[1]);
			return false;
		}
	}
	const u32 N = u32(c.particles);
	u32 mips = 1;
	while ((u32(c.page_size) >> mips) >= 4) ++mips;

	// Page cache.
	{
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = c.page_size;
		d.MipLevels = mips;
		d.ArraySize = c.pages;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
		if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &m_pages))) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = d.Format;
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		sv.Texture2DArray.MipLevels = mips;
		sv.Texture2DArray.ArraySize = c.pages;
		if (FAILED(HW.pDevice->CreateShaderResourceView(m_pages, &sv, &m_pages_srv))) return false;
		m_slice_rtv.assign(c.pages, nullptr);
		m_slice_srv.assign(c.pages, nullptr);
		for (int i = 0; i < c.pages; ++i)
		{
			D3D11_RENDER_TARGET_VIEW_DESC rv = {};
			rv.Format = d.Format;
			rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rv.Texture2DArray.FirstArraySlice = i;
			rv.Texture2DArray.ArraySize = 1;
			if (FAILED(HW.pDevice->CreateRenderTargetView(m_pages, &rv, &m_slice_rtv[i]))) return false;
			sv.Texture2DArray.FirstArraySlice = i;
			sv.Texture2DArray.ArraySize = 1;
			if (FAILED(HW.pDevice->CreateShaderResourceView(m_pages, &sv, &m_slice_srv[i]))) return false;
			const FLOAT zero[4] = {0, 0, 0, 0};
			HW.pContext->ClearRenderTargetView(m_slice_rtv[i], zero);
		}
		// Scratch page for the fluid step (typed UAV stores of RGBA8 are
		// guaranteed; typed loads are not, so the step reads the slice).
		D3D11_TEXTURE2D_DESC s = d;
		s.MipLevels = 1; s.ArraySize = 1; s.MiscFlags = 0;
		s.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(HW.pDevice->CreateTexture2D(&s, nullptr, &m_scratch))) return false;
		if (FAILED(HW.pDevice->CreateUnorderedAccessView(m_scratch, nullptr, &m_scratch_uav))) return false;
	}

	// Droplets.
	{
		xr_vector<GpuParticle> zero(N);
		ZeroMemory(zero.data(), N * sizeof(GpuParticle));
		m_particles = MakeBuffer(N * sizeof(GpuParticle), D3D11_BIND_UNORDERED_ACCESS,
		                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(GpuParticle), D3D11_USAGE_DEFAULT, 0, zero.data());
		xr_vector<u32> free_list(N);
		for (u32 i = 0; i < N; ++i) free_list[i] = i;
		m_dead = MakeBuffer(N * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4,
		                    D3D11_USAGE_DEFAULT, 0, free_list.data());
		m_render = MakeBuffer(N * 32, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0);
		const u32 args[4] = {6, 0, 0, 0};
		m_args = MakeBuffer(16, D3D11_BIND_UNORDERED_ACCESS,
		                    D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0,
		                    D3D11_USAGE_DEFAULT, 0, args);
		m_emitters = MakeBuffer(kMaxEmitters * sizeof(GpuEmitter), D3D11_BIND_SHADER_RESOURCE,
		                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(GpuEmitter), D3D11_USAGE_DYNAMIC,
		                        D3D11_CPU_ACCESS_WRITE);
		m_cb = MakeBuffer(256, D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
		m_cb_dead = MakeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, 0, 0);
		for (ID3D11Buffer*& b : m_args_readback)
			b = MakeBuffer(16, 0, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
		if (!m_particles || !m_dead || !m_render || !m_args || !m_emitters || !m_cb || !m_cb_dead) return false;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.Format = DXGI_FORMAT_UNKNOWN;
		uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uv.Buffer.NumElements = N;
		if (FAILED(HW.pDevice->CreateUnorderedAccessView(m_particles, &uv, &m_particles_uav))) return false;
		uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
		if (FAILED(HW.pDevice->CreateUnorderedAccessView(m_dead, &uv, &m_dead_uav))) return false;
		uv.Buffer.Flags = 0;
		uv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		uv.Buffer.NumElements = N * 2;
		if (FAILED(HW.pDevice->CreateUnorderedAccessView(m_render, &uv, &m_render_uav))) return false;
		uv.Format = DXGI_FORMAT_R32_TYPELESS;
		uv.Buffer.NumElements = 4;
		uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(HW.pDevice->CreateUnorderedAccessView(m_args, &uv, &m_args_uav))) return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.FirstElement = 0;
		sv.Buffer.NumElements = N * 2;
		if (FAILED(HW.pDevice->CreateShaderResourceView(m_render, &sv, &m_render_srv))) return false;
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.Buffer.NumElements = kMaxEmitters;
		if (FAILED(HW.pDevice->CreateShaderResourceView(m_emitters, &sv, &m_emitters_srv))) return false;
		m_dead_initialised = false;
	}

	m_timer = xr_new<GpuTimer>();
	if (!m_timer->Create()) return false;

	m_t_pages.create("$user$blood_pages");
	m_t_render.create("$user$blood_render");
	BindUserTextures();

	m_cs_emit = DEV->_CreateCS("blood_emit");
	m_cs_sim = DEV->_CreateCS("blood_sim");
	m_cs_flow = DEV->_CreateCS("blood_flow");
	if (!m_cs_emit->sh || !m_cs_sim->sh || !m_cs_flow->sh) return false;
	m_s_stamp.create(m_b_stamp, "r4\\blood_stamp");
	m_s_decal.create(m_b_decal, "r4\\blood_decal");
	m_s_particle.create(m_b_particle, "r4\\blood_particle");
	m_g_stamp.create(stamp_decl, RCache.Vertex.Buffer(), RCache.QuadIB);
	m_g_decal.create(decal_decl, RCache.Vertex.Buffer(), nullptr);
	m_g_particle.create(FVF::F_L, RCache.Vertex.Buffer(), nullptr);

	m_slot_flow_time.assign(c.pages, 0.f);
	m_mips_due.assign(c.pages, 0);
	m_mips_cursor = 0;
	const float mb = float(c.pages) * float(c.page_size * c.page_size * 4) * 1.33f / (1024.f * 1024.f) +
		float(N) * (48 + 4 + 32) / (1024.f * 1024.f);
	Msg("* [blood] %d droplets, %d pages of %d^2 on %.2f m tiles, fluid %s, %.0f MB",
	    c.particles, c.pages, c.page_size, c.tile, c.fluid ? "on" : "off", mb);
	return true;
}

void CBlood::Destroy()
{
	ID3D11Resource* none = nullptr;
	if (m_t_pages._get()) GiveToTexture(m_t_pages, none, nullptr);
	if (m_t_render._get()) GiveToTexture(m_t_render, none, nullptr);
	m_t_pages = nullptr;
	m_t_render = nullptr;
	m_s_stamp.destroy(); m_s_decal.destroy(); m_s_particle.destroy();
	m_g_stamp.destroy(); m_g_decal.destroy(); m_g_particle.destroy();
	m_cs_emit = nullptr; m_cs_sim = nullptr; m_cs_flow = nullptr;
	if (m_timer) { m_timer->Destroy(); xr_delete(m_timer); }

	for (ID3D11RenderTargetView*& v : m_slice_rtv) Release(v);
	for (ID3D11ShaderResourceView*& v : m_slice_srv) Release(v);
	m_slice_rtv.clear(); m_slice_srv.clear();
	Release(m_pages_srv); Release(m_pages);
	Release(m_scratch_uav); Release(m_scratch);
	Release(m_particles_uav); Release(m_particles);
	Release(m_dead_uav); Release(m_dead);
	Release(m_render_srv); Release(m_render_uav); Release(m_render);
	Release(m_args_uav); Release(m_args);
	Release(m_emitters_srv); Release(m_emitters);
	Release(m_cb); Release(m_cb_dead);
	for (ID3D11Buffer*& b : m_args_readback) Release(b);
}

void CBlood::BindUserTextures()
{
	GiveToTexture(m_t_pages, m_pages, m_pages_srv);
	GiveToTexture(m_t_render, m_render, m_render_srv);
}

// ------------------------------------------------------------------ requests
void CBlood::Emit(const BloodEmit& e)
{
	if (!m_ready || !e.count) return;
	xrCriticalSectionGuard guard(m_lock);
	if (m_emits.size() < kMaxEmitters) m_emits.push_back(e);
}

void CBlood::Splat(const BloodSplatRequest& s)
{
	if (!m_ready) return;
	xrCriticalSectionGuard guard(m_lock);
	if (m_pending.size() >= 8192) return;
	PendingSplat p;
	p.request = s;
	p.due = Device.fTimeGlobal + _max(s.delay, 0.f);
	m_pending.push_back(p);
}

void CBlood::Clear()
{
	xrCriticalSectionGuard guard(m_lock);
	m_emits.clear();
	m_pending.clear();
	m_stamps.clear();
	m_restore.clear();
	m_visible.clear();
	blood::StoreLimits limits;
	limits.slots = m_config.pages;
	m_store.Reset(limits);
	if (!m_ready) return;
	const FLOAT zero[4] = {0, 0, 0, 0};
	for (ID3D11RenderTargetView* v : m_slice_rtv) HW.pContext->ClearRenderTargetView(v, zero);
}

// ------------------------------------------------------------------ frame
void CBlood::FrameUpdate()
{
	if (!m_ready || m_frame == Device.dwFrame) return;
	m_frame = Device.dwFrame;
	if (!ps_r__blood) return;

	const Config wanted = Wanted();
	if (!(wanted == m_config))
	{
		// Size changes keep the stains: the logs replay into the new cache.
		// A new tile size changes every page address, so it starts clean.
		const bool readdress = wanted.tile != m_config.tile;
		Destroy();
		m_ready = Create(wanted);
		if (!m_ready) { Msg("! [blood] could not resize GPU resources; blood disabled"); Destroy(); return; }
		if (readdress) Clear();
		else m_store.ReleaseSlots(wanted.pages);
	}
	m_config.fluid = wanted.fluid;
	BindUserTextures();

	CTimer cpu;
	cpu.Start();
	const float now = Device.fTimeGlobal;
	const float dt = clampr(now - m_last_time, 0.f, 0.1f);
	m_last_time = now;

	m_timer->BeginFrame();
	if (m_timer->fresh)
	{
		const float* ms = m_timer->ms;
		m_backoff.Push(ms[GpuTimer::Decals] + ms[GpuTimer::Update], ps_r__blood_gpu_budget * 0.75f);
		m_backoff_drops.Push(ms[GpuTimer::Particles], ps_r__blood_gpu_budget * 0.25f);
	}

	D3D11_VIEWPORT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT saved_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	HW.pContext->RSGetViewports(&saved_count, saved);

	const int pair = m_timer->Begin(GpuTimer::Update);
	RunBench();
	UpdateResidency(now);
	LandSplats(now);
	DrawStamps(now);
	RunFlow(now);
	BuildMips();
	RunParticles(dt);
	m_timer->End(pair);

	HW.pContext->RSSetViewports(saved_count, saved);
	// Render targets were changed directly; the next phase sets its own.
	RCache.set_RT(nullptr, 1);
	RCache.set_RT(nullptr, 2);

	m_cpu_ms = cpu.GetElapsed_sec() * 1000.f;
	Report(m_cpu_ms);
}

void CBlood::UpdateResidency(float now)
{
	m_visible.clear();
	m_restore.clear();
	const float tile = m_config.tile;
	const float range = ps_r__blood_range * (0.5f + 0.5f * m_backoff.Scale());
	const blood::Vec cam = ToVec(Device.vCameraPosition);

	xr_vector<std::pair<float, int>>& candidates = m_candidates;
	candidates.clear();
	for (int id = 0; id < m_store.RecordCount(); ++id)
	{
		const blood::PageRecord& r = m_store.Record(id);
		if (!r.used) continue;
		int t1, t2;
		blood::TangentAxes(blood::MainAxis(r.key.axis), t1, t2);
		// Distance from the camera to the page box.
		float lo[3], hi[3];
		lo[t1] = float(r.key.u) * tile; hi[t1] = lo[t1] + tile;
		lo[t2] = float(r.key.v) * tile; hi[t2] = lo[t2] + tile;
		lo[blood::MainAxis(r.key.axis)] = r.lo; hi[blood::MainAxis(r.key.axis)] = r.hi;
		float d2 = 0;
		for (int i = 0; i < 3; ++i)
		{
			const float c = blood::Comp(cam, i);
			const float d = c < lo[i] ? lo[i] - c : (c > hi[i] ? c - hi[i] : 0.f);
			d2 += d * d;
		}
		if (d2 <= range * range) candidates.push_back({d2, id});
	}
	std::sort(candidates.begin(), candidates.end());

	// More pages in range than slots: resident pages a little beyond the
	// nearest-N cut stay resident, and only pages clearly nearer than that
	// cut replace them, so walking about does not churn restores.
	const u32 slots = u32(m_config.pages);
	float keep_d2 = range * range, take_d2 = range * range;
	if (candidates.size() > slots)
	{
		const float cut = _sqrt(candidates[slots - 1].first);
		keep_d2 = _sqr(cut + kResidencyMargin);
		take_d2 = _sqr(_max(cut - kResidencyMargin, 0.f));
	}
	for (const auto& c : candidates)
	{
		const blood::PageRecord& r = m_store.Record(c.second);
		if (r.slot < 0 || c.first > keep_d2) continue;
		bool fresh = false;
		m_store.Acquire(c.second, m_frame, cam, tile, fresh);
		m_visible.push_back(c.second);
	}

	int restores = 0;
	for (const auto& c : candidates)
	{
		const blood::PageRecord& r = m_store.Record(c.second);
		if (r.slot >= 0 || c.first > take_d2) continue;
		if (restores >= kMaxRestoresPerFrame) break;
		bool fresh = false;
		const int slot = m_store.Acquire(c.second, m_frame, cam, tile, fresh);
		if (slot < 0) break;
		if (fresh)
		{
			m_restore.push_back(slot);
			m_slot_flow_time[slot] = now;
			++restores;
			++m_stats_restores;
		}
		m_visible.push_back(c.second);
	}
}

void CBlood::LandSplats(float now)
{
	m_stamps.clear();
	xr_vector<PendingSplat>& due = m_due;
	due.clear();
	{
		xrCriticalSectionGuard guard(m_lock);
		for (u32 i = 0; i < m_pending.size();)
		{
			if (m_pending[i].due <= now && due.size() < u32(kMaxStampsPerFrame / 2))
			{
				due.push_back(m_pending[i]);
				m_pending[i] = m_pending.back();
				m_pending.pop_back();
			}
			else ++i;
		}
	}

	const float tile = m_config.tile;
	const blood::Vec cam = ToVec(Device.vCameraPosition);
	for (const PendingSplat& p : due)
	{
		const BloodSplatRequest& q = p.request;
		const blood::Splat s = blood::ShapeFromImpact(ToVec(q.pos), ToVec(q.normal), ToVec(q.vel), q.volume, q.seed, now);
		blood::PageTouch touches[48];
		const float footprint = s.radius * s.elongation * (s.kind == blood::SplatSplash ? 1.8f : 1.3f);
		const int n = blood::PagesForSplat(s.pos, s.normal, footprint, tile, touches, 48);
		for (int i = 0; i < n; ++i)
		{
			int evicted = -1;
			const int id = m_store.Deposit(touches[i].key, s, tile, cam, evicted);
			const blood::PageRecord& r = m_store.Record(id);
			if (r.slot >= 0)
			{
				m_stamps.push_back({r.slot, s, r.key, 1.f});
				m_slot_flow_time[r.slot] = now;
			}
		}
		++m_stats_splats;
	}
}

void CBlood::DrawStamps(float now)
{
	const float tile = m_config.tile;
	const float dry = _max(ps_r__blood_dry_time, 1.f);
	const FLOAT zero[4] = {0, 0, 0, 0};

	// A restored page replays its whole log, which already holds this
	// frame's new splats.
	xr_vector<u8>& restoring = m_restoring;
	restoring.assign(m_config.pages, 0);
	for (int slot : m_restore)
	{
		restoring[slot] = 1;
		HW.pContext->ClearRenderTargetView(m_slice_rtv[slot], zero);
	}
	m_stamps.erase(std::remove_if(m_stamps.begin(), m_stamps.end(),
	                              [&restoring](const Stamp& s) { return restoring[s.slot] != 0; }), m_stamps.end());
	for (int slot : m_restore)
	{
		const int owner = m_store.SlotOwner(slot);
		if (owner < 0) continue;
		const blood::PageRecord& r = m_store.Record(owner);
		for (const blood::Splat& logged : r.log)
		{
			blood::Splat s = logged;
			const float age = _max(now - s.time, 0.f);
			// Pools spread while their page was resident; without the
			// simulated result, replay them at their settled size.
			if (s.kind == blood::SplatPool && m_config.fluid && age > kFlowTime) s.radius *= 1.5f;
			m_stamps.push_back({slot, s, r.key, expf(-age / dry)});
		}
	}
	if (m_stamps.empty()) return;

	std::sort(m_stamps.begin(), m_stamps.end(), [](const Stamp& a, const Stamp& b) { return a.slot < b.slot; });
	m_stats_stamps += m_stamps.size();

	D3D11_VIEWPORT vp = {0, 0, float(m_config.page_size), float(m_config.page_size), 0, 1};
	HW.pContext->RSSetViewports(1, &vp);
	RCache.set_RT(nullptr, 1);
	RCache.set_RT(nullptr, 2);
	RCache.set_ZB(nullptr);
	RCache.set_Stencil(FALSE);
	RCache.set_Element(m_s_stamp->E[0]);
	RCache.set_Geometry(m_g_stamp);

	xr_vector<u8>& dirty = m_dirty;
	dirty.assign(m_config.pages, 0);
	for (u32 begin = 0; begin < m_stamps.size();)
	{
		const int slot = m_stamps[begin].slot;
		u32 end = begin;
		while (end < m_stamps.size() && m_stamps[end].slot == slot && end - begin < kMaxQuadsPerDraw) ++end;
		RCache.set_RT(m_slice_rtv[slot], 0);

		u32 offset = 0;
		const u32 quads = end - begin;
		StampVertex* v = (StampVertex*)RCache.Vertex.Lock(quads * 4, m_g_stamp->vb_stride, offset);
		for (u32 i = begin; i < end; ++i)
		{
			const Stamp& st = m_stamps[i];
			const blood::StampQuad q = blood::QuadInPage(st.splat, st.key, tile);
			const float major = st.splat.kind == blood::SplatSplash ? 1.8f : 1.3f;
			const float seed = float(st.splat.seed & 0xffff) / 65535.f;
			static const float corners[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
			for (int c = 0; c < 4; ++c, ++v)
			{
				const float x = corners[c][0], y = corners[c][1];
				v->p.set(q.cu + q.au * x + q.bu * y, q.cv + q.av * x + q.bv * y, x, y);
				v->a.set(st.splat.amount, st.splat.amount * st.wet, seed, float(st.splat.kind));
				v->b.set(st.splat.elongation, major, 1.3f, 0);
			}
		}
		RCache.Vertex.Unlock(quads * 4, m_g_stamp->vb_stride);
		RCache.Render(D3DPT_TRIANGLELIST, offset, 0, quads * 4, 0, quads * 2);
		dirty[slot] = 1;
		begin = end;
	}
	// Unbind the slices before building their mips (cached state stays in step).
	RCache.set_RT(nullptr, 0);
	HW.pContext->OMSetRenderTargets(0, nullptr, nullptr);
	// A restored slot shows another page's mips until rebuilt: rebuild now.
	// Fresh stamps only add detail; their mips wait their turn (BuildMips).
	for (int slot = 0; slot < m_config.pages; ++slot)
		if (dirty[slot])
		{
			if (restoring[slot]) { HW.pContext->GenerateMips(m_slice_srv[slot]); m_mips_due[slot] = 0; }
			else m_mips_due[slot] = 1;
		}
}

// Mips of changed slices, a few per frame: a page stamped or flowing every
// frame rebuilds them once per round instead of after every change.
void CBlood::BuildMips()
{
	const int pages = m_config.pages;
	int built = 0;
	for (int n = 0; n < pages && built < kMaxMipsPerFrame; ++n)
	{
		const int slot = (m_mips_cursor + n) % pages;
		if (!m_mips_due[slot]) continue;
		HW.pContext->GenerateMips(m_slice_srv[slot]);
		m_mips_due[slot] = 0;
		++built;
	}
	m_mips_cursor = (m_mips_cursor + _max(built, 1)) % _max(pages, 1);
}

// Pools spread and walls drip on recently stained pages; every stained page
// dries. Each step reads the slice, writes the scratch page and copies back.
void CBlood::RunFlow(float now)
{
	const float dry = _max(ps_r__blood_dry_time, 1.f);
	const u32 size = u32(m_config.page_size);
	int steps = 0;
	const int pages = m_config.pages;
	for (int n = 0; n < pages && steps < kMaxFlowPagesPerFrame; ++n)
	{
		const int slot = (m_flow_cursor + n) % pages;
		const int owner = m_store.SlotOwner(slot);
		if (owner < 0) continue;
		const blood::PageRecord& r = m_store.Record(owner);
		const float since_deposit = now - r.last_deposit;
		if (since_deposit > dry * 3.f) continue;	// dry: nothing changes any more
		const bool flowing = m_config.fluid && since_deposit < kFlowTime && r.key.axis != blood::AxisNegY;
		const float interval = flowing ? kFlowStep : kDryStep;
		const float step_dt = now - m_slot_flow_time[slot];
		if (step_dt < interval) continue;
		m_slot_flow_time[slot] = now;

		FlowConstants fc = {};
		fc.origin_u = float(r.key.u); fc.origin_v = float(r.key.v);
		fc.tile = m_config.tile; fc.texel = 1.f / float(size);
		fc.dt = _min(step_dt, 0.25f);
		fc.dry_rate = step_dt / dry;
		fc.flow = flowing ? 1.f : 0.f;
		fc.axis = float(r.key.axis);
		fc.size = size;
		fc.seed = blood::Hash01(m_frame * 31u + u32(slot));
		D3D11_MAPPED_SUBRESOURCE m;
		if (FAILED(HW.pContext->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
		CopyMemory(m.pData, &fc, sizeof(fc));
		HW.pContext->Unmap(m_cb, 0);

		HW.pContext->CSSetShader(m_cs_flow->sh, nullptr, 0);
		HW.pContext->CSSetConstantBuffers(0, 1, &m_cb);
		HW.pContext->CSSetShaderResources(0, 1, &m_slice_srv[slot]);
		HW.pContext->CSSetUnorderedAccessViews(0, 1, &m_scratch_uav, nullptr);
		HW.pContext->Dispatch((size + 15) / 16, (size + 15) / 16, 1);
		ID3D11ShaderResourceView* no_srv = nullptr;
		ID3D11UnorderedAccessView* no_uav = nullptr;
		HW.pContext->CSSetShaderResources(0, 1, &no_srv);
		HW.pContext->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);

		D3D11_TEXTURE2D_DESC d;
		m_pages->GetDesc(&d);
		HW.pContext->CopySubresourceRegion(m_pages, D3D11CalcSubresource(0, slot, d.MipLevels), 0, 0, 0, m_scratch, 0, nullptr);
		m_mips_due[slot] = 1;
		++steps;
	}
	m_flow_cursor = (m_flow_cursor + 1) % _max(pages, 1);
	HW.pContext->CSSetShader(nullptr, nullptr, 0);
}

void CBlood::RunParticles(float dt)
{
	{
		xrCriticalSectionGuard guard(m_lock);
		m_frame_emits.swap(m_emits);
		m_emits.clear();
	}
	const u32 N = u32(m_config.particles);

	// Emitters: total droplets this frame, scaled down while over budget.
	u32 total = 0;
	u32 count = 0;
	if (!m_frame_emits.empty())
	{
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(HW.pContext->Map(m_emitters, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		{
			GpuEmitter* e = (GpuEmitter*)m.pData;
			const float scale = m_backoff_drops.Scale();
			for (const BloodEmit& src : m_frame_emits)
			{
				const u32 n = _max(u32(float(src.count) * scale + 0.5f), 1u);
				if (total + n > N / 2 || count >= kMaxEmitters) break;
				GpuEmitter& g = e[count++];
				g.pos[0] = src.pos.x; g.pos[1] = src.pos.y; g.pos[2] = src.pos.z;
				g.vel[0] = src.vel.x; g.vel[1] = src.vel.y; g.vel[2] = src.vel.z;
				g.first = total; g.count = n;
				g.spread = src.spread; g.life = src.life; g.size = src.size;
				g.seed = blood::Hash(m_frame * 977u + count);
				total += n;
			}
			HW.pContext->Unmap(m_emitters, 0);
		}
	}
	m_frame_emits.clear();

	SimConstants sc = {};
	sc.dt = dt; sc.gravity = kGravity; sc.drag = kDrag; sc.time = Device.fTimeGlobal;
	sc.cam[0] = Device.vCameraPosition.x; sc.cam[1] = Device.vCameraPosition.y; sc.cam[2] = Device.vCameraPosition.z;
	const float range = ps_r__blood_particle_range * m_backoff_drops.Scale();
	sc.range_sq = range * range;
	sc.max_particles = N; sc.emit_total = total; sc.emitter_count = count; sc.frame = m_frame;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(HW.pContext->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
	CopyMemory(m.pData, &sc, sizeof(sc));
	HW.pContext->Unmap(m_cb, 0);

	const u32 args[4] = {6, 0, 0, 0};
	HW.pContext->UpdateSubresource(m_args, 0, nullptr, args, 0, 0);
	HW.pContext->CopyStructureCount(m_cb_dead, 0, m_dead_uav);

	ID3D11Buffer* cbs[2] = {m_cb, m_cb_dead};
	HW.pContext->CSSetConstantBuffers(0, 2, cbs);
	const UINT keep = UINT(-1);
	if (total)
	{
		ID3D11UnorderedAccessView* uavs[2] = {m_particles_uav, m_dead_uav};
		const UINT counts[2] = {keep, m_dead_initialised ? keep : N};
		m_dead_initialised = true;
		HW.pContext->CSSetShader(m_cs_emit->sh, nullptr, 0);
		HW.pContext->CSSetShaderResources(0, 1, &m_emitters_srv);
		HW.pContext->CSSetUnorderedAccessViews(0, 2, uavs, counts);
		HW.pContext->Dispatch((total + 63) / 64, 1, 1);
	}
	{
		ID3D11UnorderedAccessView* uavs[4] = {m_particles_uav, m_dead_uav, m_render_uav, m_args_uav};
		const UINT counts[4] = {keep, m_dead_initialised ? keep : N, keep, keep};
		m_dead_initialised = true;
		HW.pContext->CSSetShader(m_cs_sim->sh, nullptr, 0);
		HW.pContext->CSSetUnorderedAccessViews(0, 4, uavs, counts);
		HW.pContext->Dispatch((N + 255) / 256, 1, 1);
	}
	ID3D11UnorderedAccessView* no_uavs[4] = {};
	ID3D11ShaderResourceView* no_srv = nullptr;
	ID3D11Buffer* no_cbs[2] = {};
	HW.pContext->CSSetUnorderedAccessViews(0, 4, no_uavs, nullptr);
	HW.pContext->CSSetShaderResources(0, 1, &no_srv);
	HW.pContext->CSSetConstantBuffers(0, 2, no_cbs);
	HW.pContext->CSSetShader(nullptr, nullptr, 0);

	// Statistics only: the drawn count from three frames ago, never waited on.
	if (ps_r__blood_stats && m_args_readback[0])
	{
		HW.pContext->CopyResource(m_args_readback[m_frame % 3], m_args);
		D3D11_MAPPED_SUBRESOURCE r;
		ID3D11Buffer* old = m_args_readback[(m_frame + 1) % 3];
		if (HW.pContext->Map(old, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &r) == S_OK)
		{
			m_alive_estimate = ((const u32*)r.pData)[1];
			HW.pContext->Unmap(old, 0);
		}
	}
}

// ------------------------------------------------------------------ drawing
void CBlood::RenderParticles()
{
	// MSAA can come on while a level is loaded (reset_end, leaving VR).
	if (!m_ready || !ps_r__blood || m_frame != Device.dwFrame || RImplementation.o.dx10_msaa) return;
	BindUserTextures();
	const int pair = m_timer->Begin(GpuTimer::Particles);
	RCache.set_xform_world(Fidentity);
	RCache.set_Element(m_s_particle->E[0]);
	RCache.set_Geometry(m_g_particle);
	RCache.RenderInstancedIndirect(D3DPT_TRIANGLELIST, m_args, 0);
	RCache.set_Textures(&m_no_textures);
	m_timer->End(pair);
}

void CBlood::RenderDecals()
{
	if (!m_ready || !ps_r__blood || m_frame != Device.dwFrame || m_visible.empty() || RImplementation.o.dx10_msaa)
		return;
	BindUserTextures();
	const int pair = m_timer->Begin(GpuTimer::Decals);
	const float tile = m_config.tile;

	RCache.set_xform_world(Fidentity);
	RCache.set_xform_view(Device.mView);
	RCache.set_xform_project(Device.mProject);
	RCache.set_Element(m_s_decal->E[0]);
	RCache.set_Geometry(m_g_decal);
	RCache.set_c("blood_params", float(m_config.page_size), ps_r__blood_dry_time, tile, 0.f);
	// Back faces behind the scene surface: the surface lies inside the box
	// whether the camera is inside or outside it. The shader rejects
	// surfaces in front of the box.
	RCache.set_CullMode(CULL_NONE);
	RCache.set_ZFunc(D3DCMP_GREATEREQUAL);
	RCache.set_ColorWriteEnable();

	static const int faces[36] = {
		0, 1, 2, 2, 1, 3, 4, 6, 5, 5, 6, 7, 0, 4, 1, 1, 4, 5,
		2, 3, 6, 6, 3, 7, 0, 2, 4, 4, 2, 6, 1, 5, 3, 3, 5, 7};
	const u32 boxes_per_draw = 256;
	for (u32 begin = 0; begin < m_visible.size(); begin += boxes_per_draw)
	{
		const u32 end = _min(u32(m_visible.size()), begin + boxes_per_draw);
		u32 offset = 0;
		DecalVertex* v = (DecalVertex*)RCache.Vertex.Lock((end - begin) * 36, m_g_decal->vb_stride, offset);
		u32 written = 0;
		for (u32 i = begin; i < end; ++i)
		{
			const blood::PageRecord& r = m_store.Record(m_visible[i]);
			if (r.slot < 0) continue;
			const int main = blood::MainAxis(r.key.axis);
			int t1, t2;
			blood::TangentAxes(main, t1, t2);
			float lo[3], hi[3];
			lo[t1] = float(r.key.u) * tile; hi[t1] = lo[t1] + tile;
			lo[t2] = float(r.key.v) * tile; hi[t2] = lo[t2] + tile;
			const float slab = blood::SlabSize(tile);
			const float slab_lo = float(r.key.w) * slab + blood::SlabOrigin(tile);
			lo[main] = _max(r.lo - kBoxMargin, slab_lo);
			hi[main] = _min(r.hi + kBoxMargin, slab_lo + slab);
			if (hi[main] <= lo[main]) continue;
			Fvector mn, mx, centre;
			mn.set(lo[0], lo[1], lo[2]);
			mx.set(hi[0], hi[1], hi[2]);
			centre.add(mn, mx).mul(0.5f);
			if (!RImplementation.ViewBase.testSphere_dirty(centre, mn.distance_to(mx) * 0.5f)) continue;
			Fvector corners[8];
			for (int c = 0; c < 8; ++c) corners[c].set(c & 1 ? mx.x : mn.x, c & 2 ? mx.y : mn.y, c & 4 ? mx.z : mn.z);
			const Fvector4 page = {float(r.key.axis), float(r.slot), float(r.key.u), float(r.key.v)};
			const Fvector4 extent = {float(r.key.w), lo[main], hi[main], tile};
			for (int k = 0; k < 36; ++k, ++v)
			{
				v->p = corners[faces[k]];
				v->page = page;
				v->extent = extent;
			}
			written += 36;
		}
		RCache.Vertex.Unlock(written, m_g_decal->vb_stride);
		if (written) RCache.Render(D3DPT_TRIANGLELIST, offset, written / 3);
	}

	RCache.set_ZFunc(D3DCMP_LESSEQUAL);
	RCache.set_CullMode(CULL_CCW);
	RCache.set_ColorWriteEnable(D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	RCache.set_Textures(&m_no_textures);
	m_timer->End(pair);
}

// ------------------------------------------------------------------ bench
// Synthetic wounds in front of the view: drops traced against the level and
// sprayed onto whatever surface the view points at.
void CBlood::RunBench()
{
	if (ps_r__blood_bench > 0 && m_bench_left == 0)
	{
		m_bench_left = ps_r__blood_bench;
		ps_r__blood_bench = 0;
		m_bench_frames = 0; m_bench_cpu = m_bench_gpu = 0;
	}
	if (m_bench_left <= 0 || !g_pGameLevel) return;
	++m_bench_frames;
	m_bench_cpu += m_cpu_ms;
	m_bench_gpu += m_timer->Total();

	const u32 per_frame = 4;
	for (u32 k = 0; k < per_frame && m_bench_left > 0; ++k, --m_bench_left)
	{
		Fvector dir = Device.vCameraDirection;
		Fvector jitter;
		jitter.random_dir();
		dir.mad(jitter, 0.35f).normalize_safe();
		collide::rq_result hit;
		if (!g_pGameLevel->ObjectSpace.RayPick(Device.vCameraPosition, dir, 30.f, collide::rqtStatic, hit, nullptr))
			continue;
		Fvector wound;
		wound.mad(Device.vCameraPosition, dir, _max(hit.range - 1.2f, 0.3f));
		const blood::Quality q = blood::QualityPreset(ps_r__blood_quality);
		for (int c = 0; c < q.carriers; ++c)
		{
			Fvector v = dir, r;
			r.random_dir();
			v.mad(r, 0.45f).normalize_safe().mul(::Random.randF(3.f, 7.f));
			const blood::Contact contact = blood::TraceArc(ToVec(wound), ToVec(v), kGravity, kDrag, 1.5f, 4,
				[](const blood::Vec& o, const blood::Vec& d, float len, float& dist, blood::Vec& n, int& user)
				{
					collide::rq_result rq;
					// Static and dynamic, at the cost of a real wound's traces.
					if (!g_pGameLevel->ObjectSpace.RayPick(ToF(o), ToF(d), len, collide::rqtBoth, rq, nullptr))
						return false;
					if (rq.O)
					{
						dist = rq.range; n = blood::Mul(d, -1.f); user = -1;
						return true;
					}
					const CDB::TRI* tri = g_pGameLevel->ObjectSpace.GetStaticTris() + rq.element;
					const Fvector* verts = g_pGameLevel->ObjectSpace.GetStaticVerts();
					Fvector normal;
					normal.mknormal(verts[tri->verts[0]], verts[tri->verts[1]], verts[tri->verts[2]]);
					dist = rq.range; n = ToVec(normal); user = rq.element;
					return true;
				});
			BloodEmit e;
			e.pos = wound; e.vel = v; e.spread = 0.08f; e.life = contact.hit ? contact.time : 1.5f;
			e.size = 0.006f; e.count = u32(q.per_carrier);
			Emit(e);
			if (!contact.hit || contact.user < 0) continue;
			BloodSplatRequest s;
			s.pos = ToF(contact.point); s.normal = ToF(contact.normal); s.vel = ToF(contact.velocity);
			s.volume = ::Random.randF(0.2f, 1.6f); s.delay = contact.time; s.seed = ::Random.randI(0x7fffffff);
			Splat(s);
		}
	}
	if (m_bench_left == 0)
		Msg("- [blood] bench done: %d frames, cpu %.3f ms, gpu %.3f ms per frame (%d droplets, %d pages of %d^2)",
		    m_bench_frames, m_bench_cpu / _max(m_bench_frames, 1), m_bench_gpu / _max(m_bench_frames, 1),
		    m_config.particles, m_config.pages, m_config.page_size);
}

void CBlood::Report(float cpu_ms)
{
	if (!ps_r__blood_stats || Device.fTimeGlobal < m_report_time) return;
	m_report_time = Device.fTimeGlobal + 2.f;
	int resident = 0;
	for (int slot = 0; slot < m_config.pages; ++slot) resident += m_store.SlotOwner(slot) >= 0;
	Msg("- [blood] cpu %.3f ms | gpu update %.3f, droplets %.3f, stains %.3f ms | ~%u droplets drawn | "
	    "pages %d known, %d/%d resident, %d in range | %u splats, %u stamps, %u restores (2 s) | "
	    "backoff stains %d, droplets %d",
	    cpu_ms, m_timer->ms[GpuTimer::Update], m_timer->ms[GpuTimer::Particles], m_timer->ms[GpuTimer::Decals],
	    m_alive_estimate, m_store.Live(), resident, m_config.pages, int(m_visible.size()),
	    m_stats_splats, m_stats_stamps, m_stats_restores, m_backoff.level, m_backoff_drops.level);
	m_stats_splats = m_stats_stamps = m_stats_restores = 0;
}
