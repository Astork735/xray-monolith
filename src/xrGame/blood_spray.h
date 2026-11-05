#pragma once

class CEntityAlive;

// Plans blood for the DX11 blood system: drops are traced along their arcs
// against the level, so stains appear where and when they land. Used instead
// of the stock blood wallmarks while ::Render->blood_active().
namespace blood_spray
{
// A wound: exit spray along the hit direction and a little back spray.
// strength is the hit relative to the nominal hit (1 = typical bullet).
void Wound(CEntityAlive& victim, const Fvector& point, const Fvector& dir, float strength, s16 bone);
// An open wound drips onto whatever is below it.
void Drip(CEntityAlive& victim, const Fvector& point, float size);
// A blast: blood thrown all round the body, mostly away from the explosion.
void Blast(CEntityAlive& victim, const Fvector& centre, const Fvector& dir, float strength);
// A wound still bleeding: a pumped spurt while alive, a slow ooze from a
// corpse. Blood runs further down the clothing each time (trickle, metres),
// and a corpse's ooze builds one growing pool below it (bled, volume).
void Bleed(CEntityAlive& victim, const Fvector& point, const Fvector& dir, float intensity, bool alive,
           float& trickle, float& bled);
}
