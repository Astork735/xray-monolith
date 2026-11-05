#pragma once
// Requests from the game's spray planner to the renderer's blood system
// (DX11 renderer only; others ignore them and the game keeps stock blood).

// Visual droplets that follow one traced drop and vanish when it lands.
struct BloodEmit
{
	Fvector pos;
	Fvector vel;
	float spread;	// cone half-angle of the droplets' velocities (radians)
	float life;		// seconds until the traced drop lands
	float size;		// droplet radius (m)
	u32 count;
	float delay = 0.f;	// seconds before the droplets appear (a drip leaving a ledge)
};

// A traced drop that will land on static geometry: stained when it arrives.
struct BloodSplatRequest
{
	Fvector pos;
	Fvector normal;
	Fvector vel;	// velocity at impact (for runs and smears: direction of travel)
	float volume;	// relative fluid volume (1 = about 2 mL)
	float delay;	// seconds until it lands
	u32 seed;
	// Shape hints (blood::ShapeHint); the defaults derive the shape from the impact.
	s8 kind = -1;		// forced blood::SplatKind, -1: from volume and impact
	u8 flags = 0;		// blood::SplatFlags
	float length = 0.f;	// runs and smears: streak length (m)
	float radius = 0.f;	// mist, runs and smears: half-width (m)
	u32 group = 0;		// runs: segments of one run share it
};
