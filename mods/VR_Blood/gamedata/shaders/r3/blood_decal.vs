#include "common.h"

// One box per resident blood page, fitted to the stained surface.
struct v_blood_decal
{
	float3 P : POSITION;
	float4 page : TEXCOORD0;	// axis, slot, tile u, tile v
	float4 extent : TEXCOORD1;	// slab, lo, hi, tile size
};

struct p_blood_decal
{
	float4 hpos : SV_Position;
	nointerpolation float4 page : TEXCOORD0;
	nointerpolation float4 extent : TEXCOORD1;
};

p_blood_decal main(v_blood_decal I)
{
	p_blood_decal O;
	O.hpos = mul(m_VP, float4(I.P, 1.0));
	O.page = I.page;
	O.extent = I.extent;
	return O;
}
