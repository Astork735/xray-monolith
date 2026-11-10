#include "stdafx.h"
#pragma hdrstop

#include "r_backend_hemi.h"

R_hemi::R_hemi()
{
	unmap();
}

void R_hemi::unmap()
{
	c_pos_faces = nullptr;
	c_neg_faces = nullptr;
	c_material = nullptr;
	c_hotness = nullptr; //--DSR-- HeatVision
	c_glowing = nullptr; //--DSR-- SilencerOverheat
	c_blood_count = nullptr;
	c_blood_wounds = nullptr;
}

// Blood on bodies: the drawn object's wounds, two rows each; none clears.
void R_hemi::set_blood(const Fvector4* rows, u32 wounds)
{
	if (c_blood_count) RCache.set_c(c_blood_count, float(wounds), 0.f, 0.f, 0.f);
	if (c_blood_wounds)
		for (u32 i = 0; i < wounds * 2; ++i) RCache.set_ca(c_blood_wounds, i, rows[i]);
}

void R_hemi::set_pos_faces(float posx, float posy, float posz)
{
	if (c_pos_faces) RCache.set_c(c_pos_faces, posx, posy, posz, 0);
}

void R_hemi::set_neg_faces(float negx, float negy, float negz)
{
	if (c_neg_faces) RCache.set_c(c_neg_faces, negx, negy, negz, 0);
}

void R_hemi::set_material(float x, float y, float z, float w)
{
	if (c_material) RCache.set_c(c_material, x, y, z, w);
}

//--DSR-- HeatVision_start
void R_hemi::set_hotness(float x, float y, float z, float w)
{
	if (c_hotness) RCache.set_c(c_hotness, x, y, z, w);
}
//--DSR-- HeatVision_end

//--DSR-- SilencerOverheat_start
void R_hemi::set_glowing(float x, float y, float z, float w)
{
	if (c_glowing) RCache.set_c(c_glowing, x, y, z, w);
}
//--DSR-- SilencerOverheat_end