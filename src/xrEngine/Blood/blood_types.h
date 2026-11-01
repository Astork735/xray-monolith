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
};

// A traced drop that will land on static geometry: stained when it arrives.
struct BloodSplatRequest
{
	Fvector pos;
	Fvector normal;
	Fvector vel;	// velocity at impact
	float volume;	// relative fluid volume (1 = a typical bullet-wound drop)
	float delay;	// seconds until it lands
	u32 seed;
};
