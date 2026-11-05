#include "stdafx.h"

#include "blender_blood.h"

void CBlender_blood_stamp::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	// Element 0, additive: amount and wet amount accumulate where splats
	// overlap. Element 1, maximum: a growing pool is stamped again and again
	// at a slightly larger size; adding would pile its soft rim up into a hard
	// edge. Alpha holds the stained surface's height (blood::kHeightTolerance):
	// the highest stain's, never a sum.
	C.r_Pass("blood_stamp", "blood_stamp", false, FALSE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_ONE);
	if (C.iElement == 1) C.RS.SetRS(D3DRS_BLENDOP, D3DBLENDOP_MAX);
	C.RS.SetRS(D3DRS_BLENDOPALPHA, D3DBLENDOP_MAX);
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
