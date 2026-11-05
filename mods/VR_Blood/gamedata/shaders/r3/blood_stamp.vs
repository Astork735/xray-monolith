// Splat quads in page texture coordinates (see CBlood::DrawStamps).
struct v_stamp
{
	float4 P : POSITION;	// page uv, local corner
	float4 A : TEXCOORD0;	// amount, wet amount, seed, kind + 8 * flags
	float4 B : TEXCOORD1;	// elongation, major scale, minor scale, radius (m)
	float4 C : TEXCOORD2;	// page tile u, v, tile size, surface height in the slab (0..1)
};

struct p_stamp
{
	float4 hpos : SV_Position;
	float2 local : TEXCOORD0;
	float4 A : TEXCOORD1;
	float4 B : TEXCOORD2;
	float3 W : TEXCOORD3;	// world tangent coordinates of the page (m), surface height
};

p_stamp main(v_stamp I)
{
	p_stamp O;
	// Row 0 of the page is uv.y = 0 (on walls, the lowest world height).
	O.hpos = float4(I.P.x * 2.0 - 1.0, 1.0 - I.P.y * 2.0, 0.0, 1.0);
	O.local = I.P.zw;
	O.A = I.A;
	O.B = I.B;
	// World-anchored detail: a run's segments, the run logged whole and the
	// neighbouring pages all draw the same wobble.
	O.W = float3((I.C.xy + I.P.xy) * I.C.z, I.C.w);
	return O;
}
