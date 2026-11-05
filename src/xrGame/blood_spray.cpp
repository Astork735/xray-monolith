#include "stdafx.h"
#include "blood_spray.h"
#include "entity_alive.h"
#include "Level.h"
#include "../xrEngine/gamemtllib.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/Blood/blood_logic.h"
#include "../Include/xrRender/Kinematics.h"

namespace
{
const float kGravity = 9.81f;
const float kDrag = 0.35f;
const float kMaxFlight = 1.5f;	// seconds a drop is followed
const int kSegments = 4;

blood::HitFilter g_filter;

blood::Vec ToVec(const Fvector& v) { return {v.x, v.y, v.z}; }
Fvector ToF(const blood::Vec& v) { Fvector r; r.set(v.x, v.y, v.z); return r; }

// Sprays this far away would not be seen: skip their traces and droplets.
bool TooFar(const Fvector& p) { return p.distance_to_sqr(Device.vCameraPosition) > _sqr(60.f); }

// Player sliders (MCM): r__blood_amount, r__blood_spray, r__blood_density.
struct Sliders { float amount, spray, density; };
Sliders Read()
{
	float lo, hi;
	Sliders s;
	s.amount = Console->GetFloat("r__blood_amount", lo, hi);
	s.spray = Console->GetFloat("r__blood_spray", lo, hi);
	s.density = Console->GetFloat("r__blood_density", lo, hi);
	return s;
}

blood::Quality Preset()
{
	int lo = 0, hi = 3;
	return blood::QualityPreset(Console->GetInteger("r__blood_quality", lo, hi));
}

enum Surface { NoStain, Body, Stain };

// One drop: traced, drawn as droplets until it lands, stained on arrival.
void Drop(CEntityAlive& victim, const Fvector& from, const Fvector& velocity, float volume, float size, u32 droplets)
{
	const Sliders sliders = Read();
	volume *= sliders.amount;
	droplets = _max(u32(float(droplets) * sliders.density + 0.5f), 1u);
	CObject* object_hit = nullptr;
	const blood::Contact contact = blood::TraceArc(ToVec(from), ToVec(velocity), kGravity, kDrag, kMaxFlight, kSegments,
		[&](const blood::Vec& o, const blood::Vec& d, float len, float& dist, blood::Vec& n, int& surface)
		{
			collide::rq_result r;
			if (!Level().ObjectSpace.RayPick(ToF(o), ToF(d), len, collide::rqtBoth, r, &victim)) return false;
			dist = r.range;
			if (r.O)
			{
				object_hit = r.O;
				n = blood::Mul(d, -1.f);
				surface = Body;
				return true;
			}
			const CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + r.element;
			const Fvector* verts = Level().ObjectSpace.GetStaticVerts();
			Fvector normal;
			normal.mknormal(verts[tri->verts[0]], verts[tri->verts[1]], verts[tri->verts[2]]);
			// Rays can meet a triangle from behind; stains face the drop.
			if (normal.dotproduct(ToF(d)) > 0.f) normal.invert();
			n = ToVec(normal);
			// Every solid surface takes blood; the stock blood-mark flag alone
			// left many floors clean. Not water, vegetation or surfaces that
			// refuse all marks.
			const SGameMtl* material = GMLib.GetMaterialByIdx(tri->material);
			const bool clean = !material || material->Flags.test(SGameMtl::flLiquid | SGameMtl::flPassable |
				SGameMtl::flSuppressWallmarks);
			surface = clean && !(material && material->Flags.is(SGameMtl::flBloodmark)) ? NoStain : Stain;
			return true;
		});

	BloodEmit e;
	e.pos = from;
	e.vel = velocity;
	// A tight stream around the traced drop: dense, not a mist.
	e.spread = 0.025f * sliders.spray;
	e.life = contact.hit ? contact.time : kMaxFlight;
	e.size = size;
	e.count = droplets;
	::Render->blood_emit(e);

	if (!contact.hit) return;
	if (contact.user == Stain)
	{
		BloodSplatRequest s;
		s.pos = ToF(contact.point);
		s.normal = ToF(contact.normal);
		s.vel = ToF(contact.velocity);
		s.volume = volume;
		s.delay = contact.time;
		s.seed = u32(::Random.randI(0x7fffffff));
		::Render->blood_splat(s);
	}
	else if (contact.user == Body && object_hit && object_hit->Visual() && CEntityAlive::BloodMarks() &&
		object_hit != Level().CurrentEntity())
	{
		// Bodies and props move: the stock skeleton wallmark follows them.
		IKinematics* kinematics = PKinematics(object_hit->Visual());
		Fvector dir = ToF(contact.velocity);
		dir.normalize_safe();
		if (kinematics)
			::Render->add_SkeletonWallmark(&object_hit->XFORM(), kinematics, CEntityAlive::BloodMarks(),
			                               ToF(contact.point), dir, 0.04f + 0.04f * volume);
	}
}
}

void blood_spray::Wound(CEntityAlive& victim, const Fvector& point, const Fvector& dir, float strength, s16 bone)
{
	if (TooFar(point) || !g_filter.Accept(victim.ID(), bone, Device.dwFrame, ToVec(point))) return;
	clamp(strength, 0.25f, 2.5f);
	const blood::Quality q = Preset();
	const Sliders sliders = Read();
	// Fewer, denser streams in a narrow cone: blood leaves a wound in jets,
	// not as a wide mist.
	//
	// Proportions (stain radius = 4.5 cm * sqrt(volume), blood_logic.h):
	// spatter from a gunshot is mostly 1-10 mm dots thrown 0.5-2 m (volume
	// 0.01-0.15), the odd larger splash; the bulk of the blood comes later,
	// as drips while the victim moves and a pool under the body (Bleed).
	const int carriers = _max(iFloor(float(q.carriers) * 0.5f * _min(strength, 1.5f) * sliders.amount + 0.5f), 3);
	const u32 per_carrier = _max(u32(q.per_carrier) / 2, 4u);
	Fvector forward = dir;
	forward.normalize_safe();

	// Blood soaking the clothing round the entry wound; it outlasts the
	// stock hit marks (r__wallmark_ttl).
	if (IKinematics* kinematics = PKinematics(victim.Visual()))
		if (CEntityAlive::BloodMarks() && &victim != Level().CurrentEntity())
		{
			Fvector start = point;
			start.mad(forward, -0.2f);
			::Render->add_SkeletonWallmark(&victim.XFORM(), kinematics, CEntityAlive::BloodMarks(), start, forward,
			                               0.06f + 0.05f * _min(strength, 2.f), 600.f);
		}

	for (int i = 0; i < carriers; ++i)
	{
		// Most of the blood leaves with the bullet; some sprays back out of
		// the entry wound.
		const bool exit = ::Random.randF() < 0.75f;
		Fvector v = forward, jitter;
		if (!exit) v.invert();
		jitter.random_dir();
		v.mad(jitter, (exit ? 0.15f : 0.3f) * sliders.spray).normalize_safe();
		const float speed = (exit ? ::Random.randF(1.5f, 4.f) : ::Random.randF(0.5f, 1.5f)) *
			(0.7f + 0.3f * strength) * (0.5f + 0.5f * sliders.spray);
		v.mul(speed);
		Fvector from = point;
		from.mad(v, 0.01f);
		// Mostly small spatter, one drop in six a larger splash.
		const float volume = (::Random.randF() < 0.17f ? ::Random.randF(0.2f, 0.5f) : ::Random.randF(0.01f, 0.15f)) *
			_sqrt(strength);
		Drop(victim, from, v, volume, ::Random.randF(0.002f, 0.004f), per_carrier);
	}
	// A surface close behind the wound takes what the bullet carries out: a
	// spatter aimed at it, whatever the random jets above happened to do.
	collide::rq_result behind;
	if (Level().ObjectSpace.RayPick(point, forward, 1.5f * (0.5f + 0.5f * sliders.spray), collide::rqtStatic, behind, &victim))
	{
		const int spatter = clampr(iFloor((3.f + 3.f * _min(strength, 1.5f)) * sliders.amount + 0.5f), 2, 10);
		for (int i = 0; i < spatter; ++i)
		{
			Fvector v = forward, jitter;
			jitter.random_dir();
			v.mad(jitter, 0.22f * sliders.spray).normalize_safe().mul(::Random.randF(3.5f, 6.f));
			Fvector from = point;
			from.mad(v, 0.01f);
			// One heart to the pattern, smaller splashes round it.
			const float volume = (i ? ::Random.randF(0.08f, 0.3f) : ::Random.randF(0.35f, 0.7f)) * _sqrt(_max(strength, 0.5f));
			Drop(victim, from, v, volume, ::Random.randF(0.003f, 0.005f), per_carrier);
		}
	}
	// Real wounds also pour down to the floor and pool there.
	if (strength > 0.7f)
	{
		Fvector v;
		v.set(::Random.randF(-0.3f, 0.3f), -0.5f, ::Random.randF(-0.3f, 0.3f));
		Drop(victim, point, v, _max(1.5f * strength, 1.3f), 0.009f, per_carrier);
	}
}

void blood_spray::Blast(CEntityAlive& victim, const Fvector& centre, const Fvector& dir, float strength)
{
	if (TooFar(centre) || !g_filter.Accept(victim.ID(), -2, Device.dwFrame, ToVec(centre))) return;
	clamp(strength, 0.3f, 3.f);
	const blood::Quality q = Preset();
	const Sliders sliders = Read();
	const int carriers = _max(iFloor(float(q.carriers) * 2.f * _min(strength, 1.5f) * sliders.amount + 0.5f), 1);
	Fvector away = dir;
	away.normalize_safe();
	for (int i = 0; i < carriers; ++i)
	{
		Fvector v, jitter;
		jitter.random_dir();
		// Mostly away from the blast, but some in every direction.
		if (::Random.randF() < 0.7f) v.mad(away, jitter, 0.9f * _min(sliders.spray, 1.5f));
		else v = jitter;
		v.normalize_safe().mul(::Random.randF(3.f, 9.f) * (0.5f + 0.5f * sliders.spray));
		Fvector from = centre, offset;
		offset.random_dir().mul(::Random.randF(0.f, 0.3f));
		from.add(offset);
		// A blast tears tissue: more and larger drops than a bullet, thrown
		// further, but still mostly spatter.
		Drop(victim, from, v, ::Random.randF(0.05f, 0.6f) * _min(strength, 2.f), ::Random.randF(0.003f, 0.006f),
		     u32(q.per_carrier));
	}
	// What lands below the body pools.
	Fvector down;
	down.set(0.f, -1.f, 0.f);
	Drop(victim, centre, down, 1.5f * _min(strength, 2.f), 0.01f, u32(q.per_carrier));
}

void blood_spray::Bleed(CEntityAlive& victim, const Fvector& point, const Fvector& dir, float intensity, bool alive,
                        float& trickle, float& bled)
{
	clamp(intensity, 0.05f, 2.f);

	// Blood running down the clothing: each beat marks a little further
	// below the wound. The mark is cast onto the body from just outside it.
	// With r__blood_body the body's shaders draw the run (GetBloodWounds).
	int lo = 0, hi = 1;
	const bool body_shader = Console->GetInteger("r__blood_body", lo, hi) != 0;
	IKinematics* kinematics = PKinematics(victim.Visual());
	if (!body_shader && kinematics && CEntityAlive::BloodMarks() && &victim != Level().CurrentEntity() &&
		trickle < 0.5f)
	{
		trickle += alive ? 0.035f : 0.02f;
		Fvector inward = dir, start = point;
		inward.invert();
		start.y -= trickle;
		start.mad(dir, 0.2f);
		::Render->add_SkeletonWallmark(&victim.XFORM(), kinematics, CEntityAlive::BloodMarks(), start, inward,
		                               0.03f + 0.02f * _min(intensity, 1.f), 600.f);
	}

	if (alive && intensity > 1.2f)
	{
		// A severe wound (an artery): one spurt per heartbeat, arcing out.
		Fvector v = dir, jitter;
		jitter.random_dir();
		v.mad(jitter, 0.2f).normalize_safe().mul(::Random.randF(1.f, 2.5f));
		Drop(victim, point, v, 0.2f + 0.2f * intensity, 0.004f, 8);
	}
	else if (alive)
	{
		// Most wounds bleed rather than spurt: a drop falls from the wound
		// now and then, a trail where the victim moves, a small pool where
		// it stands.
		if (::Random.randF() < 0.4f + 0.4f * intensity)
		{
			Fvector v;
			v.set(::Random.randF(-0.05f, 0.05f), -0.2f, ::Random.randF(-0.05f, 0.05f));
			Drop(victim, point, v, 0.04f + 0.06f * intensity, 0.003f, 3);
		}
	}
	else
	{
		// A corpse oozes straight down onto one spot. The volume accumulates,
		// so the stain there is a pool that grows with each ooze (stamped
		// again at the same place, merged into one record, then spread by
		// the floor's fluid step): about 30 cm across after half a minute,
		// 60-90 cm when it stops.
		bled += 0.8f + 0.8f * intensity;
		Fvector v;
		v.set(::Random.randF(-0.02f, 0.02f), -0.2f, ::Random.randF(-0.02f, 0.02f));
		// A body lying on the floor can put the wound at or under its surface.
		Fvector from = point;
		from.y += 0.1f;
		Drop(victim, from, v, _max(bled, 1.3f), 0.006f, 3);
	}
}

void blood_spray::Drip(CEntityAlive& victim, const Fvector& point, float size)
{
	Fvector v;
	v.set(::Random.randF(-0.1f, 0.1f), -0.3f, ::Random.randF(-0.1f, 0.1f));
	// A drip stain is a centimetre or two across; repeated drips on one spot
	// merge and grow into a small pool.
	Drop(victim, point, v, 0.03f + size, 0.003f, 3);
}
