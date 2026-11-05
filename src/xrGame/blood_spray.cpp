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
// Base blood volume. Real wounds put far less on the floor than the first
// tuning did: everything a hit, a drip or a corpse leaves is scaled by it.
const float kBaseVolume = 0.6f;
// Drops per hit grow with the quality preset; the blood does not. Volumes
// are tuned at the highest preset's count (32 traced drops per hit).
const float kReferenceCarriers = 32.f;

blood::HitFilter g_filter;

blood::Vec ToVec(const Fvector& v) { return {v.x, v.y, v.z}; }
Fvector ToF(const blood::Vec& v) { Fvector r; r.set(v.x, v.y, v.z); return r; }

// Droplets in the air this far away would not be seen: skip them.
bool TooFar(const Fvector& p, float range = 60.f) { return p.distance_to_sqr(Device.vCameraPosition) > _sqr(range); }
// Stains are planned as far as the renderer keeps them (r__blood_range), so
// a body shot or bled out further off still shows its blood when reached.
float StainRange()
{
	float lo, hi;
	return _max(Console->GetFloat("r__blood_range", lo, hi), 60.f);
}

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

// Stain volume of a drop from its nominal volume.
float Volume(float nominal, const Sliders& sliders) { return nominal * kBaseVolume * sliders.amount; }

enum Surface { NoStain, Body, Stain };

// The surface a static ray met: its normal facing the ray, and whether it
// takes blood. Every solid surface does (the stock blood-mark flag alone
// left many floors clean); not water, vegetation or surfaces that refuse
// all marks.
bool StaticSurface(const collide::rq_result& r, const Fvector& dir, Fvector& normal)
{
	const CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + r.element;
	const Fvector* verts = Level().ObjectSpace.GetStaticVerts();
	normal.mknormal(verts[tri->verts[0]], verts[tri->verts[1]], verts[tri->verts[2]]);
	// Rays can meet a triangle from behind; stains face the ray.
	if (normal.dotproduct(dir) > 0.f) normal.invert();
	const SGameMtl* material = GMLib.GetMaterialByIdx(tri->material);
	const bool clean = !material || material->Flags.test(SGameMtl::flLiquid | SGameMtl::flPassable |
		SGameMtl::flSuppressWallmarks);
	return !clean || (material && material->Flags.is(SGameMtl::flBloodmark));
}

// Static geometry for the blood planners (blood::TraceArc, blood::PlanRun).
bool StaticRay(const blood::Vec& o, const blood::Vec& d, float len, float& dist, blood::Vec& n, int& user)
{
	collide::rq_result r;
	if (!Level().ObjectSpace.RayPick(ToF(o), ToF(d), len, collide::rqtStatic, r, nullptr)) return false;
	Fvector normal;
	StaticSurface(r, ToF(d), normal);
	dist = r.range; n = ToVec(normal); user = r.element;
	return true;
}

void Drop(CEntityAlive& victim, const Fvector& from, const Fvector& velocity, float volume, float size, u32 droplets,
          float delay = 0.f, int kind = -1);

// Blood thick enough to run down a wall: planned segment by segment, each
// stamped when the running blood reaches it. Where the run meets the floor
// the rest gathers there; where the wall ends it drips off the edge.
void Run(CEntityAlive& victim, const Fvector& point, const Fvector& normal, float volume, float delay)
{
	const blood::RunPlan plan = blood::PlanRun(ToVec(point), ToVec(normal), volume, StaticRay);
	if (plan.count < 2) return;
	const u32 group = u32(::Random.randI(1, 0x7fffffff));
	const u32 seed = u32(::Random.randI(0x7fffffff));
	const float width = 0.002f + 0.002f * _min(volume, 1.f);
	for (int i = 1; i < plan.count; ++i)
	{
		const Fvector a = ToF(plan.points[i - 1]), b = ToF(plan.points[i]);
		Fvector seg;
		seg.sub(b, a);
		const float len = seg.magnitude();
		if (len < 1e-3f) continue;
		seg.mul(1.f / len);
		BloodSplatRequest s;
		s.pos.add(a, b).mul(0.5f);
		s.normal = ToF(plan.normals[i]);
		s.vel = seg;
		s.volume = volume;
		s.delay = delay + plan.times[i];
		s.seed = seed;
		s.kind = s8(blood::SplatRun);
		s.radius = width;
		s.length = len;
		s.group = group;
		s.flags = i == plan.count - 1 && plan.end != blood::RunPlan::Floor ? u8(blood::SplatBead) : u8(0);
		::Render->blood_splat(s);
	}
	const float rest = volume * 0.5f;
	const float arrive = delay + plan.times[plan.count - 1];
	if (plan.end == blood::RunPlan::Floor)
	{
		BloodSplatRequest s;
		s.pos = ToF(plan.end_point);
		s.normal.set(0.f, 1.f, 0.f);
		s.vel.set(0.f, -0.1f, 0.f);
		s.volume = rest;
		s.delay = arrive;
		s.seed = u32(::Random.randI(0x7fffffff));
		s.kind = s8(blood::SplatDroplet);
		::Render->blood_splat(s);
	}
	else if (plan.end == blood::RunPlan::Ledge)
	{
		// What reaches the edge falls off it in drops, a little apart.
		const int drips = clampr(1 + iFloor(rest / 0.15f), 1, 3);
		Fvector from = ToF(plan.end_point);
		from.mad(ToF(plan.normals[plan.count - 1]), 0.005f);
		from.y -= 0.01f;
		float when = arrive;
		for (int k = 0; k < drips; ++k)
		{
			Fvector v;
			v.set(0.f, -0.1f, 0.f);
			Drop(victim, from, v, rest / float(drips), 0.003f, 3, when, blood::SplatDroplet);
			when += ::Random.randF(0.8f, 2.f);
		}
	}
}

// One drop of `volume` (stain volume, already scaled): traced, drawn as
// droplets until it lands, stained on arrival. `delay` holds it back (a drip
// waiting for the run that feeds it); `kind` forces the stain's kind.
void Drop(CEntityAlive& victim, const Fvector& from, const Fvector& velocity, float volume, float size, u32 droplets,
          float delay, int kind)
{
	const Sliders sliders = Read();
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
			Fvector normal;
			surface = StaticSurface(r, ToF(d), normal) ? Stain : NoStain;
			n = ToVec(normal);
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
	e.delay = delay;
	if (!TooFar(from)) ::Render->blood_emit(e);

	if (!contact.hit) return;
	if (contact.user == Stain)
	{
		BloodSplatRequest s;
		s.pos = ToF(contact.point);
		s.normal = ToF(contact.normal);
		s.vel = ToF(contact.velocity);
		s.volume = volume;
		s.delay = contact.time + delay;
		s.seed = u32(::Random.randI(0x7fffffff));
		s.kind = s8(kind);
		::Render->blood_splat(s);
		// Thick blood on a wall runs down it.
		if (_abs(s.normal.y) < 0.5f && volume >= blood::kRunVolume)
			Run(victim, s.pos, s.normal, volume, s.delay);
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

// High-velocity spatter on a surface close behind the victim: a cone of
// specks a millimetre or a few across, wider and sparser the further the
// surface (blood::SplatMist), with a few larger drops among them.
void Mist(CEntityAlive& victim, const Fvector& point, const Fvector& forward, float strength, u32 per_carrier,
          const Sliders& sliders, blood_spray::BodyBlood& blood)
{
	const float reach = 1.5f * (0.5f + 0.5f * sliders.spray);
	collide::rq_result behind;
	if (!Level().ObjectSpace.RayPick(point, forward, reach, collide::rqtStatic, behind, &victim)) return;
	Fvector normal;
	if (!StaticSurface(behind, forward, normal)) return;
	const float speed = 6.f;
	const float dist = behind.range;
	const float delay = dist / speed;
	const float mist = blood.Take(0.15f + 0.15f * _min(strength, 1.5f));
	if (mist < 0.02f) return;

	BloodSplatRequest s;
	s.pos.mad(point, forward, dist);
	s.normal = normal;
	s.vel.mul(forward, speed);
	s.volume = Volume(mist, sliders);
	s.delay = delay;
	s.seed = u32(::Random.randI(0x7fffffff));
	s.kind = s8(blood::SplatMist);
	// The spatter cone opens about 18 degrees either side of the shot.
	s.radius = clampr(dist * 0.325f * (0.8f + 0.2f * _min(strength, 2.f)) * (0.7f + 0.3f * sliders.spray), 0.05f, 0.4f);
	::Render->blood_splat(s);

	// The fine droplets flying in.
	BloodEmit e;
	e.pos = point;
	e.vel.mul(forward, speed);
	e.spread = 0.3f * sliders.spray;
	e.life = delay;
	e.size = 0.0015f;
	e.count = _max(u32(float(per_carrier * 2) * sliders.density + 0.5f), 4u);
	if (!TooFar(point)) ::Render->blood_emit(e);

	// A few larger drops among the specks.
	const int drops = clampr(iFloor((2.f + 2.f * _min(strength, 1.5f)) * sliders.amount + 0.5f), 2, 5);
	for (int i = 0; i < drops; ++i)
	{
		Fvector v = forward, jitter;
		jitter.random_dir();
		v.mad(jitter, 0.22f * sliders.spray).normalize_safe().mul(::Random.randF(3.5f, 6.f));
		Fvector from = point;
		from.mad(v, 0.01f);
		const float got = blood.Take(::Random.randF(0.01f, 0.06f));
		if (got < 0.002f) break;
		Drop(victim, from, v, Volume(got, sliders), ::Random.randF(0.002f, 0.004f), per_carrier);
	}
}

// A corpse's pool that has spread to an edge (a board, a table, a step)
// pours over it: the first time it runs down the side face (or, with none
// below the edge, drips straight off), and while the pool still grows it
// keeps dripping onto whatever is below. `edges` marks the directions that
// have already poured. The page projection itself stops at the edge
// (blood::kHeightTolerance), so nothing is drawn on the lower surface but
// what actually lands there.
void Overflow(CEntityAlive& victim, const Fvector& from, float pool_volume, blood_spray::BodyBlood& blood,
              const Sliders& sliders)
{
	u8& edges = blood.edges;
	const float radius = _min(blood::ShapeParams().radius_scale * _sqrt(pool_volume) * 1.25f, blood::ShapeParams().max_radius);
	if (radius < 0.05f || TooFar(from, 40.f)) return;
	Fvector down;
	down.set(0.f, -1.f, 0.f);
	collide::rq_result floor;
	if (!Level().ObjectSpace.RayPick(from, down, 1.5f, collide::rqtStatic, floor, &victim)) return;
	Fvector normal;
	StaticSurface(floor, down, normal);
	if (normal.y < 0.7f) return;
	Fvector centre;
	centre.mad(from, down, floor.range);
	const float level = centre.y;
	// Still the pool's floor (or something it is held back by) at p?
	const auto same = [&](const blood::Vec& p)
	{
		Fvector o = ToF(p);
		o.y = level + 0.05f;
		collide::rq_result q;
		return Level().ObjectSpace.RayPick(o, down, 0.05f + blood::kHeightTolerance, collide::rqtStatic, q, &victim);
	};
	float dist[blood::kPoolEdgeDirs];
	if (!blood::PoolEdges(ToVec(centre), radius, same, dist)) return;
	for (int i = 0; i < blood::kPoolEdgeDirs; ++i)
	{
		if (dist[i] < 0.f) continue;
		const Fvector dir = ToF(blood::PoolEdgeDir(i));
		Fvector edge;
		edge.mad(centre, dir, dist[i]);
		const u8 bit = u8(1u << i);
		if (!(edges & bit))
		{
			edges |= bit;
			// The side face under the edge, if there is one: the blood runs
			// down it.
			Fvector probe = edge, back = dir, side_normal;
			probe.mad(dir, 0.05f);
			probe.y -= 0.03f;
			back.invert();
			collide::rq_result side;
			if (Level().ObjectSpace.RayPick(probe, back, 0.08f, collide::rqtStatic, side, &victim) &&
				StaticSurface(side, back, side_normal) && _abs(side_normal.y) < 0.7f)
			{
				Fvector at;
				at.mad(probe, back, side.range);
				const float run = Volume(blood.Take(0.6f), sliders);
				if (run >= blood::kRunVolume) Run(victim, at, side_normal, run, 0.f);
			}
		}
		// What keeps coming over drips off the edge, half the oozes.
		if (::Random.randF() < 0.5f)
		{
			const float got = blood.Take(0.08f);
			if (got < 0.005f) continue;
			Fvector f = edge, v;
			f.mad(dir, 0.01f);
			f.y -= 0.005f;
			v.set(dir.x * 0.05f, -0.1f, dir.z * 0.05f);
			Drop(victim, f, v, Volume(got, sliders), 0.003f, 3);
		}
	}
}
}

// A person (80 kg) loses at most about 1.5 L outside the body: in nominal
// volume, the pool that leaves is about 70 cm across. Blood volume scales
// with body mass, within reason (a small dog, a pseudogiant).
float blood_spray::SizeFactor(float mass) { return clampr(mass / 80.f, 0.15f, 3.f); }
float blood_spray::Supply(float mass) { return 80.f * SizeFactor(mass); }

void blood_spray::Wound(CEntityAlive& victim, const Fvector& point, const Fvector& dir, float strength, s16 bone,
                        BodyBlood& blood)
{
	if (TooFar(point, StainRange()) || !g_filter.Accept(victim.ID(), bone, Device.dwFrame, ToVec(point))) return;
	clamp(strength, 0.25f, 2.5f);
	const blood::Quality q = Preset();
	const Sliders sliders = Read();
	// Fewer, denser streams in a narrow cone: blood leaves a wound in jets,
	// not as a wide mist.
	//
	// Proportions (stain radius = 4.5 cm * sqrt(volume), blood_logic.h):
	// spatter from a gunshot is mostly 1-10 mm dots thrown 0.5-2 m, the odd
	// larger splash. Nothing pools at the moment of the hit: the blood that
	// reaches the floor comes later, as drips while the victim bleeds and a
	// pool under the body (Bleed).
	const int carriers = _max(iFloor(float(q.carriers) * 0.5f * _min(strength, 1.5f) * sliders.amount + 0.5f), 3);
	const u32 per_carrier = _max(u32(q.per_carrier) / 2, 4u);
	const float per_drop = kReferenceCarriers / float(_max(q.carriers, 1));
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
			_sqrt(strength) * per_drop;
		const float got = blood.Take(volume);
		if (got < 0.002f) break;	// bled out
		Drop(victim, from, v, Volume(got, sliders), ::Random.randF(0.002f, 0.004f), per_carrier);
	}
	// A surface close behind the wound takes what the bullet carries out.
	Mist(victim, point, forward, strength, per_carrier, sliders, blood);
}

void blood_spray::Blast(CEntityAlive& victim, const Fvector& centre, const Fvector& dir, float strength,
                        BodyBlood& blood)
{
	if (TooFar(centre, StainRange()) || !g_filter.Accept(victim.ID(), -2, Device.dwFrame, ToVec(centre))) return;
	clamp(strength, 0.3f, 3.f);
	const blood::Quality q = Preset();
	const Sliders sliders = Read();
	const int carriers = _max(iFloor(float(q.carriers) * 2.f * _min(strength, 1.5f) * sliders.amount + 0.5f), 1);
	const float per_drop = kReferenceCarriers / float(_max(q.carriers, 1));
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
		// further, but still mostly spatter. The wounds it leaves bleed and
		// pool afterwards (CEntityAlive::Hit adds them).
		const float got = blood.Take(::Random.randF(0.05f, 0.6f) * _min(strength, 2.f) * per_drop);
		if (got < 0.002f) break;	// bled out
		Drop(victim, from, v, Volume(got, sliders), ::Random.randF(0.003f, 0.006f), u32(q.per_carrier));
	}
}

void blood_spray::Bleed(CEntityAlive& victim, const Fvector& point, const Fvector& dir, const Fvector& velocity,
                        float intensity, bool alive, float& trickle, BodyBlood& blood)
{
	clamp(intensity, 0.05f, 2.f);
	const Sliders sliders = Read();

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

	// Drops leave with the body's own motion: a walking victim's drips
	// stretch and point the way it went.
	Fvector moving = velocity;
	const float speed = moving.magnitude();
	if (speed > 6.f) moving.mul(6.f / speed);

	if (!alive) return;	// a corpse oozes into its pool instead (Ooze)
	if (intensity > 1.2f)
	{
		// A severe wound (an artery): one spurt per heartbeat, arcing out.
		Fvector v = dir, jitter;
		jitter.random_dir();
		v.mad(jitter, 0.2f).normalize_safe().mul(::Random.randF(1.f, 2.5f));
		v.add(moving);
		const float got = blood.Take(0.2f + 0.2f * intensity);
		if (got > 0.005f) Drop(victim, point, v, Volume(got, sliders), 0.004f, 8);
	}
	else
	{
		// Most wounds bleed rather than spurt: drops fall from the soaked
		// clothing, faster the worse the wound (CEntityAlive sets the beat),
		// a trail where the victim moves, a puddle where it stands.
		if (::Random.randF() < 0.85f)
		{
			Fvector v;
			v.set(::Random.randF(-0.05f, 0.05f), -0.2f, ::Random.randF(-0.05f, 0.05f));
			v.add(moving);
			const float got = blood.Take(0.04f + 0.06f * intensity);
			if (got > 0.005f) Drop(victim, point, v, Volume(got, sliders), 0.003f, 3);
		}
	}
}

bool blood_spray::Ooze(CEntityAlive& victim, const Fvector& point, BodyBlood& blood)
{
	// Without a heartbeat blood only drains: the pool closes on its final
	// size exponentially (two-thirds after kDrainTime, nearly all after
	// three times that).
	const float kDrainTime = 40.f;
	const float now = Device.fTimeGlobal;
	const float dt = blood.last_ooze > 0.f ? clampr(now - blood.last_ooze, 0.f, 5.f) : 1.5f;
	blood.last_ooze = now;
	const float remaining = blood.target - blood.pooled;
	const bool draining = remaining >= 0.3f && blood.left > 0.f;
	const float got = draining ? blood.Take(remaining * (1.f - expf(-dt / kDrainTime))) : 0.f;
	blood.pooled += got;
	// A corpse dragged away leaves its pool and starts another where it
	// lies, from the blood that is left.
	if (blood.pool_here > 0.f && blood.pool_at.distance_to_xz(point) > 0.1f)
	{
		blood.pool_here = 0.f;
		blood.drawn = 0.f;
		blood.edges = 0;
		blood.floor_found = false;
	}
	blood.pool_here += got;
	blood.pool_at = point;
	// Out of stain range nothing is drawn; the pool keeps its size for when
	// it is, even if it has stopped growing by then.
	const bool behind = blood.pool_here > blood.drawn + 0.002f;
	if (TooFar(point, StainRange())) return draining || behind;
	if (!behind) return draining;

	// A body lying on the floor can put the wound at or under its surface:
	// look for the floor from just above it, once per pool.
	Fvector from = point;
	from.y += 0.1f;
	if (!blood.floor_found)
	{
		Fvector down;
		down.set(0.f, -1.f, 0.f);
		collide::rq_result r;
		Fvector normal;
		if (!Level().ObjectSpace.RayPick(from, down, 1.5f, collide::rqtStatic, r, &victim) ||
			!StaticSurface(r, down, normal) || normal.y < 0.7f)
			return draining;	// no floor to pool on (on a prop, over water): try again later
		blood.floor.mad(from, down, r.range);
		blood.floor_normal = normal;
		blood.seed = u32(::Random.randI(0x7fffffff));
		blood.floor_found = true;
	}
	blood.drawn = blood.pool_here;
	const Sliders sliders = Read();
	// Stamped again at the same place, same seed, a little larger each time
	// (merged into one record; the floor's fluid step spreads it further).
	BloodSplatRequest s;
	s.pos = blood.floor;
	s.normal = blood.floor_normal;
	s.vel.set(0.f, -1.5f, 0.f);
	s.volume = Volume(blood.pool_here, sliders);
	s.delay = 0.f;
	s.seed = blood.seed;
	s.kind = s8(blood::SplatPool);
	::Render->blood_splat(s);
	// Edges: a few rays each, not every stamp.
	if (Device.fTimeGlobal >= blood.next_overflow)
	{
		blood.next_overflow = Device.fTimeGlobal + 1.5f;
		Overflow(victim, from, Volume(blood.pool_here, sliders), blood, sliders);
	}
	return draining;
}

void blood_spray::Drip(CEntityAlive& victim, const Fvector& point, float size, BodyBlood& blood)
{
	const Sliders sliders = Read();
	Fvector v;
	v.set(::Random.randF(-0.1f, 0.1f), -0.3f, ::Random.randF(-0.1f, 0.1f));
	// A drip stain is a centimetre or two across; repeated drips on one spot
	// merge and grow into a small pool.
	const float got = blood.Take(0.03f + size);
	if (got > 0.005f) Drop(victim, point, v, Volume(got, sliders), 0.003f, 3);
}

void blood_spray::Contact(CEntityAlive& victim, const Fvector& point, const Fvector& outward, float wet,
                          float strength, ContactState& state, BodyBlood& blood)
{
	// The player's body bones do not follow the real body in VR.
	if (&victim == Level().CurrentEntity() || TooFar(point, 40.f) || wet <= 0.f)
	{
		state.touching = false;
		return;
	}
	const float soak = _min(strength, 2.f);
	// Soaked clothing gives blood again while the wound keeps bleeding.
	state.budget = _min(state.budget + 0.1f * wet * _max(soak, 0.5f), 1.f);

	// Whatever the wound side of the body rests against: a wall it leans
	// on, the floor under a body lying on it.
	Fvector dirs[3];
	int count = 0;
	Fvector out = outward;
	if (out.square_magnitude() > EPS)
	{
		out.normalize();
		dirs[count++] = out;
		Fvector flat = out;
		flat.y = 0.f;
		if (flat.magnitude() > 0.3f) dirs[count++] = flat.normalize();
	}
	dirs[count++].set(0.f, -1.f, 0.f);
	Fvector hit_point, normal;
	bool hit = false;
	for (int i = 0; i < count && !hit; ++i)
	{
		Fvector from = point;
		from.mad(dirs[i], -0.02f);
		collide::rq_result r;
		if (!Level().ObjectSpace.RayPick(from, dirs[i], 0.09f, collide::rqtStatic, r, &victim)) continue;
		if (!StaticSurface(r, dirs[i], normal)) continue;
		hit_point.mad(from, dirs[i], r.range);
		hit = true;
	}
	if (!hit)
	{
		state.touching = false;
		return;
	}

	blood::SmearShape shape = blood::SmearFromContact(ToVec(state.last), ToVec(hit_point), ToVec(normal));
	const bool first = !state.touching || state.last.distance_to(hit_point) > 0.6f;
	if (first) shape = blood::SmearFromContact(ToVec(hit_point), ToVec(hit_point), ToVec(normal));
	else if (shape.length < 0.03f) return;	// resting: it already left its imprint
	const float cost = 0.3f + 0.5f * shape.length;
	if (state.budget < cost * 0.5f || blood.left < 0.05f) return;
	state.touching = true;
	state.budget = _max(state.budget - cost, 0.f);
	state.last = hit_point;

	const Sliders sliders = Read();
	const float patch = 0.025f + 0.025f * soak;	// the soaked patch (GetBloodWounds)
	BloodSplatRequest s;
	s.pos = ToF(shape.pos);
	s.normal = normal;
	s.vel = ToF(shape.dir);
	s.volume = Volume(_max(blood.Take(0.4f * wet), 0.05f), sliders);
	s.delay = 0.f;
	s.seed = u32(::Random.randI(0x7fffffff));
	s.kind = s8(blood::SplatSmear);
	s.radius = patch;
	s.length = shape.length + 2.f * patch;
	::Render->blood_splat(s);
}
