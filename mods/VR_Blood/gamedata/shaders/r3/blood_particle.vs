#include "common.h"

// Droplets drawn this frame, written by blood_sim.cs: per droplet
// (position, radius) and (velocity, life fraction).
Buffer<float4> s_blood_render;

struct p_blood_drop
{
	float4 hpos : SV_Position;
	float3 pv : TEXCOORD0;		// view-space position
	float2 local : TEXCOORD1;	// -1..1 across the droplet
	float2 axis : TEXCOORD2;	// view-space direction of the stretched axis
};

p_blood_drop main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	static const float2 corners[6] = {
		float2(-1, -1), float2(-1, 1), float2(1, -1),
		float2(1, -1), float2(-1, 1), float2(1, 1)};
	float4 a = s_blood_render.Load(iid * 2);
	float4 b = s_blood_render.Load(iid * 2 + 1);
	float2 c = corners[vid];

	float3 pv = mul(m_V, float4(a.xyz, 1.0));
	float3 vv = mul(m_V, float4(b.xyz, 0.0));
	float size = a.w;
	float len = length(vv.xy);
	float2 dir = len > 1e-4 ? vv.xy / len : float2(1, 0);
	// Motion stretch over roughly one display frame, so fast spray reads as
	// streaks; droplets shrink a little as they fly.
	float stretch = 1.0 + min(len * 0.011 / max(size, 1e-3), 4.0);
	size *= lerp(1.0, 0.7, b.w);
	float3 corner = pv + float3(dir * size * stretch * c.x + float2(-dir.y, dir.x) * size * c.y, 0.0);

	p_blood_drop O;
	O.hpos = mul(m_P, float4(corner, 1.0));
	O.pv = corner;
	O.local = c;
	O.axis = dir;
	return O;
}
