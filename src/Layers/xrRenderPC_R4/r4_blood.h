#pragma once
// Blood system for the DX11 renderer: GPU droplets, contact stains, and
// persistent blood in a world-space sparse virtual texture (see
// xrEngine/Blood/blood_logic.h). Created per level; one simulation step per
// frame, drawn in every eye.
#include "../../xrEngine/Blood/blood_logic.h"

class CBlood
{
public:
	CBlood();
	~CBlood();

	bool Ready() const { return m_ready; }
	void Emit(const BloodEmit& e);
	void Splat(const BloodSplatRequest& s);
	void Clear();

	// Once per frame (the first eye calls it, the others return at once):
	// lands due splats, restores pages, simulates droplets and pools.
	void FrameUpdate();
	// G-buffer phase: droplets as lit opaque geometry.
	void RenderParticles();
	// After the wallmarks: resident pages projected onto the scene.
	void RenderDecals();

private:
	struct Config
	{
		int particles = 0, page_size = 0, pages = 0;
		bool fluid = false;
		float tile = 1.f;
		bool operator==(const Config& o) const
		{
			return particles == o.particles && page_size == o.page_size && pages == o.pages && tile == o.tile;
		}
	};
	struct PendingSplat { BloodSplatRequest request; float due = 0; };
	struct PendingEmit { BloodEmit emit; float due = 0; };
	struct Stamp { int slot; blood::Splat splat; blood::PageKey key; float wet; };
	struct GpuTimer;

	static Config Wanted();
	bool Create(const Config& c);
	void Destroy();
	void BindUserTextures();

	void LandSplats(float now);
	void UpdateResidency(float now);
	void DrawStamps(float now);
	void RunFlow(float now);
	void BuildMips();
	void RunParticles(float dt);
	void RunBench();
	void Report(float cpu_ms);

	bool m_ready = false;
	u32 m_frame = u32(-1);
	Config m_config;
	blood::PageStore m_store;
	// Each steps down what costs: stains and page work shrink the stain
	// range, droplet drawing the droplet count and range.
	blood::Backoff m_backoff, m_backoff_drops;

	xrCriticalSection m_lock;
	xr_vector<PendingEmit> m_emits;		// waiting for their delay
	xr_vector<PendingSplat> m_pending;
	xr_vector<BloodEmit> m_frame_emits;
	xr_vector<Stamp> m_stamps;
	xr_vector<int> m_restore;		// slots to clear and replay
	xr_vector<int> m_visible;		// records drawn this frame
	xr_vector<float> m_slot_flow_time;	// last fluid step of each slot
	xr_vector<std::pair<float, int>> m_candidates;	// per-frame scratch
	xr_vector<int> m_margin;		// resident pages kept only by the residency margin
	xr_vector<PendingSplat> m_due;
	xr_vector<u8> m_restoring, m_dirty;
	xr_vector<u8> m_mips_due;		// slices changed since their mips were built
	int m_mips_cursor = 0;
	int m_flow_cursor = 0;
	float m_last_time = 0;

	// Physical page cache: one texture array, a view per slice.
	ID3D11Texture2D* m_pages = nullptr;
	ID3D11ShaderResourceView* m_pages_srv = nullptr;
	xr_vector<ID3D11RenderTargetView*> m_slice_rtv;
	xr_vector<ID3D11ShaderResourceView*> m_slice_srv;
	ID3D11Texture2D* m_scratch = nullptr;
	ID3D11UnorderedAccessView* m_scratch_uav = nullptr;

	// Droplets.
	ID3D11Buffer* m_particles = nullptr;
	ID3D11UnorderedAccessView* m_particles_uav = nullptr;
	ID3D11Buffer* m_dead = nullptr;
	ID3D11UnorderedAccessView* m_dead_uav = nullptr;
	ID3D11Buffer* m_render = nullptr;
	ID3D11ShaderResourceView* m_render_srv = nullptr;
	ID3D11UnorderedAccessView* m_render_uav = nullptr;
	ID3D11Buffer* m_args = nullptr;
	ID3D11UnorderedAccessView* m_args_uav = nullptr;
	ID3D11Buffer* m_emitters = nullptr;
	ID3D11ShaderResourceView* m_emitters_srv = nullptr;
	ID3D11Buffer* m_cb = nullptr;
	ID3D11Buffer* m_cb_dead = nullptr;
	ID3D11Buffer* m_args_readback[3] = {};
	bool m_dead_initialised = false;
	u32 m_alive_estimate = 0;

	ref_cs m_cs_emit, m_cs_sim, m_cs_flow;
	IBlender* m_b_stamp = nullptr;
	IBlender* m_b_decal = nullptr;
	IBlender* m_b_particle = nullptr;
	ref_shader m_s_stamp, m_s_decal, m_s_particle;
	ref_geom m_g_stamp, m_g_decal, m_g_particle;
	ref_texture m_t_pages, m_t_render;
	STextureList m_no_textures;		// binding it unbinds our views from every stage

	GpuTimer* m_timer = nullptr;
	float m_cpu_ms = 0;
	int m_bench_left = 0, m_bench_frames = 0;
	float m_bench_cpu = 0, m_bench_gpu = 0;
	float m_report_time = 0;
	u32 m_stats_splats = 0, m_stats_stamps = 0, m_stats_restores = 0;
};
