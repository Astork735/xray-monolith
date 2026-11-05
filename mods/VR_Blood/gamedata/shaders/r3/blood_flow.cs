#include "blood_common.h"

// One fluid step on one blood page. R = fluid amount, G = wet amount.
// Floors: blood runs downhill over the page's surface relief and pools in
// its hollows. Walls: wet blood runs down in rivulets. Every page dries.
// Only wet blood above a thin film moves (surface tension), so stains settle.

cbuffer BloodFlow : register(b0)
{
	float origin_u;		// page tile coordinates
	float origin_v;
	float tile;			// metres per page
	float texel;		// 1 / page size
	float flow_dt;
	float dry_rate;		// step time / dry time
	float flow_on;		// 0: drying only
	float page_axis;
	float page_size_bits;	// uint bits: the engine parses float constants only
	float flow_seed;		// differs per step
	float2 flow_pad;
};
#define page_size asuint(page_size_bits)

Texture2DArray<float4> src : register(t0);	// the page's slice
RWTexture2D<unorm float4> dst : register(u0);	// scratch page, copied back

#define TILE 16
#define APRON 2
#define WIDE (TILE + 2 * APRON)
#define RING (TILE + 2)

groupshared float2 g_fluid[WIDE * WIDE];
groupshared float g_head[WIDE * WIDE];
groupshared float g_share[RING * RING];

static const int2 neighbours[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};

int at(int2 l) { return l.x + l.y * WIDE; }
int ring_at(int2 l) { return (l.x - 1) + (l.y - 1) * RING; }

float mobile(float2 f)
{
	float wet = f.y / max(f.x, 1e-3);
	return max(f.x - 0.04, 0.0) * smoothstep(0.15, 0.5, wet);
}

float rate()
{
	return saturate(flow_dt * 4.0) * 0.25;
}

float floor_flux(int2 from, int2 to)
{
	return max(g_head[at(from)] - g_head[at(to)], 0.0) * rate();
}

float rivulet(int x)
{
	float column = (origin_u + (float(x) + 0.5) * texel) * tile;
	return 0.5 + blood_noise(float2(column * 37.0, page_axis * 3.1));
}

[numthreads(TILE, TILE, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	int2 base = int2(gid.xy) * TILE - APRON;
	int size = int(page_size);
	for (uint k = gi; k < WIDE * WIDE; k += TILE * TILE)
	{
		int2 p = base + int2(k % WIDE, k / WIDE);
		bool inside = all(p >= 0) && all(p < size);
		float2 f = inside ? src.Load(int4(p, 0, 0)).rg : float2(0.0, 0.0);
		float2 tc = (float2(origin_u, origin_v) + (float2(p) + 0.5) * texel) * tile;
		g_fluid[k] = f;
		// Outside the page is a wall: nothing flows across page edges.
		g_head[k] = inside ? blood_surface_height(tc, page_axis) * blood_relief + f.x : 1e3;
	}
	GroupMemoryBarrierWithGroupSync();

	bool floor_page = int(page_axis + 0.5) == 2;
	bool flowing = flow_on > 0.5;
	if (flowing && floor_page)
	{
		// Share of its potential outflow each cell can actually give.
		for (uint k = gi; k < RING * RING; k += TILE * TILE)
		{
			int2 l = int2(k % RING, k / RING) + 1;
			float out_total = 0.0;
			[unroll] for (int n = 0; n < 4; ++n) out_total += floor_flux(l, l + neighbours[n]);
			g_share[k] = out_total > 1e-6 ? min(1.0, mobile(g_fluid[at(l)]) / out_total) : 0.0;
		}
	}
	GroupMemoryBarrierWithGroupSync();

	int2 l = int2(gtid.xy) + APRON;
	int2 p = base + l;
	if (any(p >= size)) return;
	float2 me = g_fluid[at(l)];
	float amount = me.x;
	float wet = me.y;

	if (flowing && floor_page)
	{
		float my_share = g_share[ring_at(l)];
		float my_wet = me.y / max(me.x, 1e-3);
		[unroll] for (int n = 0; n < 4; ++n)
		{
			int2 o = l + neighbours[n];
			float out_n = floor_flux(l, o) * my_share;
			float in_n = floor_flux(o, l) * g_share[ring_at(o)];
			float2 other = g_fluid[at(o)];
			amount += in_n - out_n;
			wet += in_n * (other.y / max(other.x, 1e-3)) - out_n * my_wet;
		}
	}
	else if (flowing && !floor_page)
	{
		// Row y + 1 is higher on a wall; blood runs to lower rows.
		float k_wall = saturate(flow_dt * 2.5);
		float out_down = mobile(me) * k_wall * rivulet(p.x);
		float2 above = g_fluid[at(l + int2(0, 1))];
		float in_above = mobile(above) * k_wall * rivulet(p.x);
		amount += in_above - out_down;
		wet += in_above * (above.y / max(above.x, 1e-3)) - out_down * (me.y / max(me.x, 1e-3));
	}

	wet *= exp(-dry_rate);
	// Half a step of dither: on 8 bits a slow decay would otherwise round back
	// to the same value and the stain would stop drying.
	if (wet > 0.0) wet += (blood_hash2(float2(p) * texel + flow_seed) - 0.5) / 255.0;
	if (amount < 1.5 / 255.0) amount = wet = 0.0;
	dst[p] = float4(saturate(amount), saturate(min(wet, amount)), 0.0, 0.0);
}
