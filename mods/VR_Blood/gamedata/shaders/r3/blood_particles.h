#ifndef BLOOD_PARTICLES_H
#define BLOOD_PARTICLES_H
// Layouts match r4_blood.cpp (GpuParticle, GpuEmitter, SimConstants).

struct BloodParticle
{
	float3 pos;
	float age;
	float3 vel;
	float life;		// 0: free slot
	float size;
	uint colour;
	uint seed;
	float pad;
};

struct BloodEmitter
{
	float3 pos;
	uint first;		// index of its first droplet among this frame's emissions
	float3 vel;
	uint count;
	float spread;
	float life;
	float size;
	uint seed;
	float4 pad;
};

cbuffer BloodSim : register(b0)
{
	float sim_dt;
	float sim_gravity;
	float sim_drag;
	float sim_time;
	float3 sim_camera;
	float sim_range_sq;
	// Integers stored as raw bits: the engine's constant parser accepts only
	// float types in constant buffers. Read them through the defines below.
	float sim_max_particles_bits;
	float sim_emit_total_bits;
	float sim_emitter_count_bits;
	float sim_frame_bits;
};
#define sim_max_particles asuint(sim_max_particles_bits)
#define sim_emit_total asuint(sim_emit_total_bits)
#define sim_emitter_count asuint(sim_emitter_count_bits)

uint blood_hash_u(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}

float blood_rand(inout uint s)
{
	s = blood_hash_u(s);
	return float(s & 0xffffff) / 16777216.0;
}

#endif
