#include "stdafx.h"

#include "blender_blood.h"

void CBlender_blood_stamp::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	// Additive: amount and wet amount accumulate where splats overlap.
	C.r_Pass("blood_stamp", "blood_stamp", false, FALSE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_ONE);
	C.r_CullMode(D3DCULL_NONE);
	C.r_End();
}

void CBlender_blood_decal::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	C.r_Pass("blood_decal", "blood_decal", false, TRUE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA);
	C.r_dx10Texture("s_position", r2_RT_P);
	C.r_dx10Texture("s_blood_pages", "$user$blood_pages");
	// Clamped: wrapping, a page's mips would take stains from its far edge.
	C.r_dx10Sampler("smp_rtlinear");
	C.r_End();
}

void CBlender_blood_particle::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	C.r_Pass("blood_particle", "blood_particle", false, TRUE, TRUE);
	C.r_CullMode(D3DCULL_NONE);
	C.r_dx10Texture("s_blood_render", "$user$blood_render");
	C.r_End();
}
