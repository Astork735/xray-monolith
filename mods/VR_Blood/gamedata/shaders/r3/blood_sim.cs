#include "blood_particles.h"

RWStructuredBuffer<BloodParticle> particles : register(u0);
AppendStructuredBuffer<uint> dead : register(u1);
RWBuffer<float4> render : register(u2);
RWByteAddressBuffer args : register(u3);	// DrawInstancedIndirect arguments

// Once per frame for both eyes: gravity and air drag, retire droplets whose
// drop has landed, and list the ones in range for drawing.
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint i = id.x;
	if (i >= sim_max_particles) return;
	BloodParticle p = particles[i];
	if (p.life <= 0.0) return;

	p.age += sim_dt;
	if (p.age >= p.life)
	{
		p.life = 0.0;
		particles[i] = p;
		dead.Append(i);
		return;
	}
	// Same integration as the traced drop (blood::TraceArc), so droplets
	// arrive where their drop's stain appears.
	float3 before = p.vel;
	p.vel.y -= sim_gravity * sim_dt;
	p.vel *= 1.0 / (1.0 + sim_drag * length(p.vel) * sim_dt);
	p.pos += (before + p.vel) * 0.5 * sim_dt;
	particles[i] = p;

	float3 d = p.pos - sim_camera;
	if (dot(d, d) > sim_range_sq) return;
	uint slot;
	args.InterlockedAdd(4, 1, slot);
	render[slot * 2] = float4(p.pos, p.size);
	render[slot * 2 + 1] = float4(p.vel, p.age / p.life);
}
