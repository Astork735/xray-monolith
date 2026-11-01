// Splat quads in page texture coordinates (see CBlood::DrawStamps).
struct v_stamp
{
	float4 P : POSITION;	// page uv, local corner
	float4 A : TEXCOORD0;	// amount, wet amount, seed, kind
	float4 B : TEXCOORD1;	// elongation, major scale, minor scale
};

struct p_stamp
{
	float4 hpos : SV_Position;
	float2 local : TEXCOORD0;
	float4 A : TEXCOORD1;
	float4 B : TEXCOORD2;
};

p_stamp main(v_stamp I)
{
	p_stamp O;
	// Row 0 of the page is uv.y = 0 (on walls, the lowest world height).
	O.hpos = float4(I.P.x * 2.0 - 1.0, 1.0 - I.P.y * 2.0, 0.0, 1.0);
	O.local = I.P.zw;
	O.A = I.A;
	O.B = I.B;
	return O;
}
