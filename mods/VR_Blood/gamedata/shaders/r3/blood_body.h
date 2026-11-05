#ifndef BLOOD_BODY_H
#define BLOOD_BODY_H
// Blood running from the wounds of the object being drawn (r__blood_body),
// set per object by the engine (CEntityAlive::GetBloodWounds); zero wounds
// for everything else, which then costs one uniform branch.
//
// Not included by any shader directly: the renderer appends it to whatever
// gbuffer_stage.h the installed shader set has, after renaming that file's
// pack_gbuffer to pack_gbuffer_base (includer::Open in r4.cpp). The stock
// header and shader packs that keep its pack_gbuffer signatures both work.

uniform float4 L_blood_count;		// x: wounds
uniform float4 L_blood_wounds[16];	// per wound: world centre, radius / world run vector, wetness

float gbuf_blood_hash(float3 p)
{
	p = frac(p * 0.3183099 + 0.1);
	p *= 17.0;
	return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float gbuf_blood_noise(float3 x)
{
	float3 i = floor(x);
	float3 f = frac(x);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(gbuf_blood_hash(i + float3(0, 0, 0)), gbuf_blood_hash(i + float3(1, 0, 0)), f.x),
	                 lerp(gbuf_blood_hash(i + float3(0, 1, 0)), gbuf_blood_hash(i + float3(1, 1, 0)), f.x), f.y),
	            lerp(lerp(gbuf_blood_hash(i + float3(0, 0, 1)), gbuf_blood_hash(i + float3(1, 0, 1)), f.x),
	                 lerp(gbuf_blood_hash(i + float3(0, 1, 1)), gbuf_blood_hash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

// Each wound is a soaked patch at the wound and a run below it that narrows
// and breaks up towards its end. Fresh blood is red and glossy; it dries
// dark brown and matte.
void gbuf_apply_blood(float3 pos_view, inout float4 col)
{
	int count = int(L_blood_count.x + 0.5);
	[branch] if (count <= 0) return;
	float3 Pw = mul(m_inv_V, float4(pos_view, 1.0));
	float n = gbuf_blood_noise(Pw * 45.0);
	float mask = 0.0;
	float wet = 0.0;
	[loop] for (int i = 0; i < count; ++i)
	{
		float4 a = L_blood_wounds[i * 2];
		float4 b = L_blood_wounds[i * 2 + 1];
		float len2 = dot(b.xyz, b.xyz);
		float t = len2 > 1e-6 ? saturate(dot(Pw - a.xyz, b.xyz) / len2) : 0.0;
		float r = a.w * lerp(1.0, 0.4, t) * (0.7 + 0.6 * n);
		float d = length(Pw - (a.xyz + b.xyz * t));
		float m = saturate((r - d) / max(r * 0.4, 1e-3));
		mask = max(mask, m);
		wet = max(wet, m * b.w);
	}
	[branch] if (mask <= 0.0) return;
	float w = wet / max(mask, 1e-3);
	float3 blood = lerp(float3(0.10, 0.028, 0.020), float3(0.30, 0.012, 0.010), w) * lerp(0.85, 1.15, n);
	col.rgb = lerp(col.rgb, blood, mask * 0.9);
	col.a = lerp(col.a, lerp(0.25, 0.9, w), mask);
}

#ifndef EXTEND_F_DEFFER
f_deffer pack_gbuffer(float4 norm, float4 pos, float4 col)
{
	gbuf_apply_blood(pos.xyz, col);
	return pack_gbuffer_base(norm, pos, col);
}
#else
f_deffer pack_gbuffer(float4 norm, float4 pos, float4 col, uint imask)
{
	gbuf_apply_blood(pos.xyz, col);
	return pack_gbuffer_base(norm, pos, col, imask);
}
#endif

#endif
