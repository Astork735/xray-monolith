#pragma once

class CEntityAlive;

// Plans blood for the DX11 blood system: drops are traced along their arcs
// against the level, so stains appear where and when they land. Used instead
// of the stock blood wallmarks while ::Render->blood_active().
//
// Volumes: 1 unit is about 2 mL (a 9 cm stain on a hard floor). A hit throws
// spatter only; pools build over time, from drips where a bleeder stands
// and from a corpse's ooze. A body only has so much blood (BodyBlood): every
// source draws from it, and a body that has given it all gives no more.
namespace blood_spray
{
// What a wound is touching (Contact), kept with the wound.
struct ContactState
{
	Fvector last = {0.f, 0.f, 0.f};	// where it last left blood on the surface
	float budget = 0.f;		// blood the soaked clothing can still give
	bool touching = false;
};

// A body's blood: what it can still lose outside the body, and its corpse's
// pool. Volumes are nominal (before r__blood_amount and the base volume).
// A person holds about 5 L, but only 1-1.5 L of a fatal bleed ends up
// outside the body; Supply() is that share, scaled by body mass.
struct BodyBlood
{
	float left = -1.f;		// still to lose; below zero until the body is sized
	float pooled = 0.f;		// poured into the corpse's pool so far
	float target = 0.f;		// what the pool will reach; 0 until the body dies
	float last_ooze = 0.f;	// time of the last ooze
	float pool_here = 0.f;	// in the pool where the corpse lies now (dragged: a new one)
	float drawn = 0.f;		// pool_here when the pool was last stamped
	Fvector pool_at = {0.f, 0.f, 0.f};
	// Where the pool lies, found once (again if the corpse is moved): it is
	// stamped many times a second as it grows, always with the same seed, so
	// one outline spreads instead of a new one popping in.
	Fvector floor = {0.f, 0.f, 0.f}, floor_normal = {0.f, 1.f, 0.f};
	bool floor_found = false;
	u32 seed = 0;
	float next_overflow = 0.f;
	u8 edges = 0;			// directions the pool has poured over an edge
	// Takes up to `want` from what is left; returns what it got.
	float Take(float want)
	{
		const float got = _min(_max(want, 0.f), _max(left, 0.f));
		left -= got;
		return got;
	}
};
// External blood of a body of `mass` kg (a person, 80 kg: the worst-case
// pool, about 70 cm across).
float Supply(float mass);
// Mass factor of a body against a person's, for its corpse pool.
float SizeFactor(float mass);

// A wound: exit spray along the hit direction, a little back spray, and the
// fine spatter on a surface close behind the victim.
// strength is the hit relative to the nominal hit (1 = typical bullet).
void Wound(CEntityAlive& victim, const Fvector& point, const Fvector& dir, float strength, s16 bone, BodyBlood& blood);
// An open wound drips onto whatever is below it.
void Drip(CEntityAlive& victim, const Fvector& point, float size, BodyBlood& blood);
// A blast: blood thrown all round the body, mostly away from the explosion.
void Blast(CEntityAlive& victim, const Fvector& centre, const Fvector& dir, float strength, BodyBlood& blood);
// A wound still bleeding while the victim lives: drips (a spurt from a
// severe wound) falling with the body's motion (velocity, m/s). Blood runs
// further down the clothing each time (trickle, metres), alive or dead.
void Bleed(CEntityAlive& victim, const Fvector& point, const Fvector& dir, const Fvector& velocity, float intensity,
           bool alive, float& trickle, BodyBlood& blood);
// A corpse oozing into its one pool, below its lowest wound (point). With no
// heartbeat the blood only drains: the pool grows fast at first, slows and
// stops at blood.target. A pool that reaches an edge pours over it.
// Returns false once it has stopped.
bool Ooze(CEntityAlive& victim, const Fvector& point, BodyBlood& blood);
// A wet wound pressed against a surface leaves a transfer stain there, and a
// smear where it slides along it. wet: 1 while bleeding, 0 once dried.
void Contact(CEntityAlive& victim, const Fvector& point, const Fvector& outward, float wet, float strength,
             ContactState& state, BodyBlood& blood);
}
