#ifndef BLOOD_COMMON_H
#define BLOOD_COMMON_H
// Shared by the blood stamp, decal and flow shaders. Page addressing and the
// axis weights must match xrEngine/Blood/blood_logic.h.

static const float3 blood_fresh = float3(0.30, 0.012, 0.010);
static const float3 blood_dried = float3(0.10, 0.028, 0.020);
static const float blood_relief = 0.15;	// surface relief in fluid-amount units
// Fluid amount that stays put (blood_flow.cs). The wall value must match
// blood::kWallMobile: the game makes small drops, mist and runs thinner than it.
static const float blood_floor_film = 0.04;
static const float blood_wall_film = 0.45;

float blood_hash1(float n)
{
	return frac(sin(n) * 43758.5453);
}

float blood_hash2(float2 p)
{
	p = frac(p * float2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return frac(p.x * p.y);
}

float blood_noise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	float a = blood_hash2(i);
	float b = blood_hash2(i + float2(1, 0));
	float c = blood_hash2(i + float2(0, 1));
	float d = blood_hash2(i + float2(1, 1));
	return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Roughness and bump grid of a surface: the same relief for the pools that
// collect in it (blood_flow.cs) and for thin films drawn on it (blood_decal.ps).
// tc: world tangent coordinates of the page (metres).
float blood_surface_height(float2 tc, float axis)
{
	float2 o = float2(axis * 17.13, axis * 5.71);
	float h = blood_noise(tc * 7.0 + o) * 0.5;
	h += blood_noise(tc * 19.0 + o * 1.7) * 0.3;
	h += blood_noise(tc * 53.0 + o * 2.3) * 0.2;
	return h;
}

int blood_main_axis(int axis)
{
	return axis >> 1;
}

void blood_tangent_axes(int main_axis, out int t1, out int t2)
{
	if (main_axis == 0) { t1 = 2; t2 = 1; }
	else if (main_axis == 1) { t1 = 0; t2 = 2; }
	else { t1 = 0; t2 = 1; }
}

// Weight of a page's axis for a world normal; zero on the opposite face.
float blood_axis_weight(float3 n, int axis)
{
	float3 w = max(abs(n) - 0.35, 0.0);
	w *= w;
	float sum = w.x + w.y + w.z;
	int main_axis = blood_main_axis(axis);
	float along = n[main_axis];
	bool positive = (axis & 1) == 0;
	if (sum <= 1e-8 || (positive ? along < 0.0 : along > 0.0)) return 0.0;
	return w[main_axis] / sum;
}

#endif
