#include "blood_particles.h"

// Free droplet slots at the start of this frame (CopyStructureCount).
cbuffer BloodDead : register(b1)
{
	float dead_count_bits;	// a uint written by CopyStructureCount
	float3 dead_pad;
};
#define dead_count asuint(dead_count_bits)

StructuredBuffer<BloodEmitter> emitters : register(t0);
RWStructuredBuffer<BloodParticle> particles : register(u0);
ConsumeStructuredBuffer<uint> dead : register(u1);

// One thread per new droplet: takes a free slot and starts it on a cone
// around its traced drop's velocity, so it lands near the traced contact.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint g = id.x;
	if (g >= sim_emit_total || g >= dead_count) return;

	uint lo = 0, hi = sim_emitter_count - 1;
	while (lo < hi)
	{
		uint mid = (lo + hi + 1) / 2;
		if (emitters[mid].first <= g) lo = mid; else hi = mid - 1;
	}
	BloodEmitter e = emitters[lo];

	uint s = e.seed ^ (g * 2654435761u);
	float speed = length(e.vel);
	float3 d = speed > 1e-4 ? e.vel / speed : float3(0, -1, 0);
	float3 r = float3(blood_rand(s), blood_rand(s), blood_rand(s)) * 2.0 - 1.0;
	d = normalize(d + r * e.spread);

	BloodParticle p;
	p.pos = e.pos + r * 0.01;
	// Close to the traced drop's speed, and gone just as it lands: a droplet
	// ends where the stain appears, never past the surface.
	p.vel = d * speed * (0.95 + 0.1 * blood_rand(s));
	p.age = 0;
	p.life = max(e.life * (0.94 + 0.06 * blood_rand(s)), 0.05);
	p.size = e.size * (0.5 + blood_rand(s));
	p.colour = 0;
	p.seed = s;
	p.pad = 0;
	particles[dead.Consume()] = p;
}
