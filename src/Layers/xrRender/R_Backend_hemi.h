#ifndef r_backend_hemiH
#define r_backend_hemiH
#pragma once

class ECORE_API R_hemi
{
public:
	R_constant* c_pos_faces;
	R_constant* c_neg_faces;
	R_constant* c_material;

	R_constant* c_hotness; //--DSR-- HeatVision
	R_constant* c_glowing; //--DSR-- SilencerOverheat
	R_constant* c_blood_count;
	R_constant* c_blood_wounds;

public:
	R_hemi();
	void unmap();

	void set_c_pos_faces(R_constant* C) { c_pos_faces = C; }
	void set_c_neg_faces(R_constant* C) { c_neg_faces = C; }
	void set_c_material(R_constant* C) { c_material = C; }

	void set_c_hotness(R_constant* C) { c_hotness = C; } //--DSR-- HeatVision
	void set_c_glowing(R_constant* C) { c_glowing = C; } //--DSR-- SilencerOverheat
	void set_c_blood_count(R_constant* C) { c_blood_count = C; }
	void set_c_blood_wounds(R_constant* C) { c_blood_wounds = C; }

	void set_pos_faces(float posx, float posy, float posz);
	void set_neg_faces(float negx, float negy, float negz);
	void set_material(float x, float y, float z, float w);


	void set_hotness(float x, float y, float z, float w); //--DSR-- HeatVision
	void set_glowing(float x, float y, float z, float w); //--DSR-- SilencerOverheat
	void set_blood(const Fvector4* rows, u32 wounds);
};
#endif
