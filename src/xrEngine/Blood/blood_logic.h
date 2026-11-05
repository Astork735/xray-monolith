#pragma once
// Blood system logic shared by the game (spray planning) and the DX11
// renderer (persistent surface pages). No engine types: unit-tested in
// tests/vr/blood_tests.cpp.
//
// Persistent blood lives in a world-space sparse virtual texture. The
// virtual address of a texel is a world tile on one of six signed
// projection axes (+X,-X,+Y,-Y,+Z,-Z) plus a slab index along that axis, so
// floors, ceilings, the two faces of a thin wall and surfaces stacked closer
// than a tile (a table over a floor) do not share a page.
// Pages are cached in a fixed set of physical slots (a texture array);
// every page keeps a bounded log of the splats written to it, which rebuilds
// the page when it becomes resident again after eviction.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace blood
{
struct Vec { float x=0, y=0, z=0; };
inline Vec Add(const Vec& a, const Vec& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec Sub(const Vec& a, const Vec& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec Mul(const Vec& a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float Dot(const Vec& a, const Vec& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float Length(const Vec& a) { return std::sqrt(Dot(a, a)); }
inline Vec Normalize(const Vec& a, const Vec& fallback = {0, 1, 0})
{
    const float l = Length(a);
    return l > 1e-6f ? Mul(a, 1.f/l) : fallback;
}
inline float Comp(const Vec& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline float Hash01(uint32_t x) { return float(Hash(x) & 0xffffff) / float(0x1000000); }

// ---------------------------------------------------------------- pages
enum Axis { AxisPosX, AxisNegX, AxisPosY, AxisNegY, AxisPosZ, AxisNegZ, AxisCount };
inline int MainAxis(int axis) { return axis >> 1; }
// Texel axes of a page. The second one is world +Y on walls, so "down" on a
// wall page is always the lower texel row (see blood_flow.cs).
inline void TangentAxes(int main, int& t1, int& t2)
{
    if (main == 0) { t1 = 2; t2 = 1; }
    else if (main == 1) { t1 = 0; t2 = 2; }
    else { t1 = 0; t2 = 1; }
}

struct PageKey
{
    int axis = 0, u = 0, v = 0, w = 0;
    bool operator==(const PageKey& o) const { return axis == o.axis && u == o.u && v == o.v && w == o.w; }
};
struct PageKeyHash
{
    size_t operator()(const PageKey& k) const
    {
        return Hash(uint32_t(k.u)*73856093U ^ uint32_t(k.v)*19349663U ^ uint32_t(k.w)*83492791U ^ uint32_t(k.axis));
    }
};

inline int TileIndex(float c, float tile) { return int(std::floor(c/tile)); }
// Slabs are thinner than a tile: a page's box may only hold one surface.
inline float SlabSize(float tile) { return tile*0.25f; }
// Slab boundaries sit off the whole, half and quarter metres level floors
// are usually built at; a floor on a boundary would be stored and drawn by
// two pages.
inline float SlabOrigin(float tile) { return SlabSize(tile)*0.548f; }
inline int SlabIndex(float c, float tile) { return TileIndex(c - SlabOrigin(tile), SlabSize(tile)); }

// Weight of each main axis for a surface normal. Shaders use the same rule
// (blood_common.h) so the pages a splat is written to are the pages a pixel
// reads from.
inline void AxisWeights(const Vec& n, float w[3])
{
    float sum = 0;
    for (int i = 0; i < 3; ++i)
    {
        const float a = std::max(std::fabs(Comp(n, i)) - 0.35f, 0.f);
        w[i] = a*a; sum += w[i];
    }
    if (sum <= 1e-8f) { w[0] = 0; w[1] = 1; w[2] = 0; return; }
    for (int i = 0; i < 3; ++i) w[i] /= sum;
}

inline PageKey KeyAt(const Vec& p, int axis, float tile)
{
    int t1, t2; TangentAxes(MainAxis(axis), t1, t2);
    PageKey k;
    k.axis = axis;
    k.u = TileIndex(Comp(p, t1), tile);
    k.v = TileIndex(Comp(p, t2), tile);
    k.w = SlabIndex(Comp(p, MainAxis(axis)), tile);
    return k;
}

// Page-local texture coordinates of a world point (0..1 inside the page).
inline void PageUv(const Vec& p, const PageKey& k, float tile, float& u, float& v)
{
    int t1, t2; TangentAxes(MainAxis(k.axis), t1, t2);
    u = Comp(p, t1)/tile - float(k.u);
    v = Comp(p, t2)/tile - float(k.v);
}

// Surface height of the stains. A page is a flat projection: without it, a
// pool on a board would also show on the floor beside it, a few centimetres
// lower but inside the same page box. Every stained texel records the height
// (along the page's main axis) of the surface its stain landed on, and is
// not drawn on surfaces more than kHeightTolerance behind it (in front of
// it, grass blades and litter still take the blood). Stored in the page's
// alpha as 1..255 over the slab; 0 means unknown (drawn anywhere in the box).
constexpr float kHeightTolerance = 0.03f;
// Position of the slab's lower boundary along the main axis.
inline float SlabLow(const PageKey& k, float tile) { return float(k.w)*SlabSize(tile) + SlabOrigin(tile); }
// 0..1 across the page's slab (outside: the plane leaves the slab there).
inline float SlabHeight01(float along, const PageKey& k, float tile) { return (along - SlabLow(k, tile))/SlabSize(tile); }
inline float EncodeHeight(float h01) { return h01 < 0.f || h01 > 1.f ? 0.f : (1.f + 254.f*h01)/255.f; }

struct PageTouch { PageKey key; float weight = 0; };

// Pages a splat covers: every axis carrying weight for its normal, every
// tile its footprint overlaps, and both slabs where it straddles a boundary.
inline int PagesForSplat(const Vec& p, const Vec& n, float radius, float tile, PageTouch* out, int max_out)
{
    float w[3]; AxisWeights(n, w);
    int count = 0;
    for (int main = 0; main < 3; ++main)
    {
        if (w[main] < 0.05f) continue;
        const float along = Comp(n, main);
        const int axis = main*2 + (along >= 0 ? 0 : 1);
        int t1, t2; TangentAxes(main, t1, t2);
        // On a tilted surface the footprint also spans the main axis.
        const float slope = std::sqrt(std::max(0.f, 1.f - along*along));
        const float c1 = Comp(p, t1), c2 = Comp(p, t2), ca = Comp(p, main);
        const int u0 = TileIndex(c1 - radius, tile), u1 = TileIndex(c1 + radius, tile);
        const int v0 = TileIndex(c2 - radius, tile), v1 = TileIndex(c2 + radius, tile);
        const int w0 = SlabIndex(ca - radius*slope - 0.01f, tile), w1 = SlabIndex(ca + radius*slope + 0.01f, tile);
        for (int u = u0; u <= u1; ++u)
            for (int v = v0; v <= v1; ++v)
                for (int s = w0; s <= w1; ++s)
                {
                    if (count >= max_out) return count;
                    out[count].key.axis = axis; out[count].key.u = u; out[count].key.v = v; out[count].key.w = s;
                    out[count].weight = w[main];
                    ++count;
                }
    }
    return count;
}

// ---------------------------------------------------------------- splats
// Droplet, splash and pool come from a drop's impact; the others are asked
// for by the game: fine high-velocity spatter (mist), blood wiped onto a
// surface by a bleeding body (smear), and blood running down a wall (run).
// The stamp shader (blood_stamp.ps) draws each kind by this number.
enum SplatKind : uint8_t { SplatDroplet, SplatSplash, SplatPool, SplatMist, SplatSmear, SplatRun, SplatKinds };
enum SplatFlags : uint8_t { SplatBead = 1 };   // a run's lower end: the bead of blood that stopped there

// Fluid amounts above this run down walls (blood_flow.cs, wall_mobile).
// Small drops and specks stay below it and dry where they land; drops piling
// up on one spot add up past it and run.
constexpr float kWallMobile = 0.45f;
// Thickness of one mist speck (blood_stamp.ps, KIND_MIST): never runs.
constexpr float kMistAmount = 0.38f;

struct ShapeParams
{
    float droplet_volume = 0.08f; // below: a single droplet
    float pool_volume = 1.2f;     // above, on a floor: a spreading pool
    float splash_speed = 2.0f;    // m/s at impact for a splash with satellites
    float radius_scale = 0.045f;  // radius (m) of a unit volume (about 2 mL)
    float max_radius = 0.45f;
    float max_elongation = 4.f;
};

// What the game asks for beyond the impact itself (BloodSplatRequest).
struct ShapeHint
{
    int kind = -1;          // forced SplatKind; -1 derives it from volume and impact
    float length = 0;       // runs and smears: streak length (m)
    float radius = 0;       // mist, runs and smears: half-width (m)
    uint8_t flags = 0;
    uint32_t group = 0;     // runs: segments of one run
};

struct Splat
{
    Vec pos, normal;
    Vec dir;                  // tangent direction of travel (world)
    float radius = 0;         // metres, across the travel direction
    float elongation = 1;     // length/width
    float amount = 0;         // 0..1 fluid deposited at the centre
    float time = 0;           // deposit time (s, engine global time)
    uint32_t seed = 0;
    uint32_t group = 0;
    uint8_t kind = SplatDroplet;
    uint8_t flags = 0;
};

inline Vec AnyTangent(const Vec& n)
{
    const Vec ref = std::fabs(n.y) < 0.9f ? Vec{0, 1, 0} : Vec{1, 0, 0};
    const Vec t = Sub(ref, Mul(n, Dot(ref, n)));
    return Normalize(t, {1, 0, 0});
}

// Thickness a drop leaves: small drops make thin stains, larger ones thicker.
inline float AmountForVolume(float volume)
{
    return Clamp(0.2f + 0.65f*std::sqrt(std::max(volume, 0.f)/0.5f), 0.2f, 0.85f);
}

// Shape of a stain from how a drop met the surface: grazing, fast drops
// stretch along their tangential velocity; large volumes on floors pool.
// A hint can force the kind: a pool only where the surface is a floor.
inline Splat ShapeFromImpact(const Vec& pos, const Vec& normal, const Vec& velocity, float volume, uint32_t seed,
                             float time, const ShapeHint& hint = ShapeHint(), const ShapeParams& sp = ShapeParams())
{
    Splat s;
    s.pos = pos; s.normal = Normalize(normal); s.seed = seed; s.time = time;
    s.flags = hint.flags; s.group = hint.group;
    const float speed = Length(velocity);
    const Vec vdir = Normalize(velocity, Mul(s.normal, -1.f));
    const float head_on = std::fabs(Dot(vdir, s.normal)); // 1 = perpendicular impact
    const Vec tangential = Sub(velocity, Mul(s.normal, Dot(velocity, s.normal)));
    s.dir = Length(tangential) > 1e-3f ? Normalize(tangential) : AnyTangent(s.normal);
    const float speed_k = Clamp(speed/6.f, 0.f, 1.f);
    s.elongation = 1.f + (1.f - head_on)*speed_k*(sp.max_elongation - 1.f);
    // Spreading grows with speed; area is kept when the stain stretches.
    const float area_radius = sp.radius_scale*std::sqrt(std::max(volume, 0.f))*(1.f + 0.6f*speed_k);
    s.radius = Clamp(area_radius/std::sqrt(s.elongation), 0.004f, sp.max_radius);
    const bool floor = s.normal.y > 0.7f;

    if (hint.kind == SplatMist || hint.kind == SplatSmear || hint.kind == SplatRun)
    {
        s.kind = uint8_t(hint.kind);
        s.radius = Clamp(hint.radius > 0 ? hint.radius : area_radius, 0.002f, sp.max_radius);
        if (hint.length > 0) s.elongation = std::max(1.f, 0.5f*hint.length/s.radius);
        else if (hint.kind != SplatMist) s.elongation = 1.f;
        // A run is a thin trail: below the wall threshold, so the GPU step
        // leaves it where it was planned (its bead may sag a little).
        // Mist specks are thin and never run.
        // Mist: the same droplets spread over a wider pattern further from
        // the wound, so it thins out. Its amount carries that density; each
        // speck is drawn kMistAmount thick (blood_stamp.ps).
        if (hint.kind == SplatRun) s.amount = 0.42f;
        else if (hint.kind == SplatMist) s.amount = kMistAmount*Clamp(0.2f + 0.05f/s.radius, 0.3f, 1.f);
        else s.amount = std::min(AmountForVolume(volume), 0.6f);
        return s;
    }
    if ((hint.kind == SplatPool || (hint.kind < 0 && volume >= sp.pool_volume)) && floor)
    {
        s.kind = SplatPool; s.elongation = 1.f + 0.3f*(s.elongation - 1.f); s.amount = 1.f;
        s.radius = Clamp(area_radius, 0.004f, sp.max_radius);
    }
    else if (hint.kind != SplatDroplet && volume >= sp.droplet_volume && speed >= sp.splash_speed)
    {
        s.kind = SplatSplash; s.amount = AmountForVolume(volume);
    }
    else { s.kind = SplatDroplet; s.amount = AmountForVolume(volume); }
    return s;
}

// Extent of a splat's stamp from its centre (m): along the travel direction,
// with room for splash satellites, a run's bead and a smear's ragged tail;
// and across it. The stamp shader works in radii of the same quad.
inline float MajorExtent(const Splat& s)
{
    switch (s.kind)
    {
    case SplatSplash: return s.radius*s.elongation*1.8f;
    case SplatMist: return s.radius*s.elongation*1.2f;
    case SplatSmear: return s.radius*(s.elongation + 0.6f);
    case SplatRun: return s.radius*(s.elongation + 2.f);
    default: return s.radius*s.elongation*1.3f;
    }
}
inline float MinorExtent(const Splat& s)
{
    switch (s.kind)
    {
    case SplatMist: return s.radius*1.2f;
    case SplatRun: return s.radius*2.f;
    default: return s.radius*1.3f;
    }
}

// Stamp rectangle of a splat in a page: centre, major half-axis (along the
// travel direction) and minor half-axis, all in page texture coordinates.
struct StampQuad { float cu, cv, au, av, bu, bv; };
inline StampQuad QuadInPage(const Splat& s, const PageKey& k, float tile)
{
    StampQuad q;
    PageUv(s.pos, k, tile, q.cu, q.cv);
    int t1, t2; TangentAxes(MainAxis(k.axis), t1, t2);
    float du = Comp(s.dir, t1), dv = Comp(s.dir, t2);
    const float l = std::sqrt(du*du + dv*dv);
    if (l < 1e-3f) { du = 1; dv = 0; } else { du /= l; dv /= l; }
    const float major = MajorExtent(s)/tile;
    const float minor = MinorExtent(s)/tile;
    q.au = du*major; q.av = dv*major;
    q.bu = -dv*minor; q.bv = du*minor;
    return q;
}

// Height along a page's main axis of the plane a splat landed on, at world
// tangent coordinates (c1, c2) of that page: a tilted floor or ramp keeps its
// slope across the stain. Surfaces nearly edge-on to the axis keep the
// splat's own height (they barely draw on this page).
inline float PlaneAlong(const Splat& s, int main, float c1, float c2)
{
    int t1, t2; TangentAxes(main, t1, t2);
    const float nm = Comp(s.normal, main);
    float along = Comp(s.pos, main);
    if (std::fabs(nm) > 0.2f)
        along -= (Comp(s.normal, t1)*(c1 - Comp(s.pos, t1)) + Comp(s.normal, t2)*(c2 - Comp(s.pos, t2)))/nm;
    return along;
}

// A run that grew by one more segment: one logged splat spans both.
inline void ExtendRun(Splat& o, const Splat& s)
{
    const float ho = o.radius*o.elongation, hs = s.radius*s.elongation;
    const float c = Dot(Sub(s.pos, o.pos), o.dir);
    const float lo = std::min(-ho, c - hs), hi = std::max(ho, c + hs);
    o.pos = Add(o.pos, Mul(o.dir, 0.5f*(lo + hi)));
    o.elongation = std::max(1.f, 0.5f*(hi - lo)/std::max(o.radius, 1e-4f));
    o.flags |= s.flags;
    o.time = std::max(o.time, s.time);
}

// ---------------------------------------------------------------- page store
// Margin round a page's stained rectangle (m): room for the fluid step to
// spread blood beyond the stamps (a pool keeps spreading while its corpse
// oozes; a wall dribble runs about its own length).
constexpr float kStainMargin = 0.08f;

struct PageRecord
{
    PageKey key;
    std::vector<Splat> log;
    int slot = -1;
    float lo = 0, hi = 0;          // extent of the stained surface along the main axis
    // Page coordinates (0..1) holding every stamp written to the page; empty
    // while u1 < u0. The fluid step and the decal work inside it (plus
    // kStainMargin), not over the whole page.
    float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    float last_deposit = 0;        // engine time of the latest splat
    uint32_t last_visible = 0;     // frame the page was last drawn
    bool used = false;
};

struct StoreLimits
{
    int slots = 128;        // physical pages
    int records = 4096;     // pages remembered (resident or not)
    int log_capacity = 48;  // splats kept per page for restoration
};

class PageStore
{
public:
    explicit PageStore(const StoreLimits& limits = StoreLimits()) { Reset(limits); }

    void Reset(const StoreLimits& limits)
    {
        m_limits = limits;
        m_index.clear(); m_records.clear(); m_free.clear();
        m_slot_owner.assign(std::max(limits.slots, 0), -1);
        m_live = 0;
    }
    const StoreLimits& Limits() const { return m_limits; }
    int Live() const { return m_live; }
    PageRecord& Record(int id) { return m_records[id]; }
    const PageRecord& Record(int id) const { return m_records[id]; }
    int RecordCount() const { return int(m_records.size()); }
    int SlotOwner(int slot) const { return m_slot_owner[slot]; }

    int Find(const PageKey& key) const
    {
        const auto it = m_index.find(key);
        return it == m_index.end() ? -1 : it->second;
    }

    // Records a splat on a page, creating the page record if needed.
    // `evicted_slot` reports a resident slot freed to make room (-1 if none).
    int Deposit(const PageKey& key, const Splat& s, float tile, const Vec& camera, int& evicted_slot)
    {
        evicted_slot = -1;
        int id = Find(key);
        if (id < 0)
        {
            if (m_live >= m_limits.records) evicted_slot = DropOldest(camera, tile);
            id = Allocate(key);
            PageRecord& r = m_records[id];
            const float along = Comp(s.pos, MainAxis(key.axis));
            r.lo = r.hi = along;
        }
        PageRecord& r = m_records[id];
        AddToLog(r, s);
        const float along = Comp(s.pos, MainAxis(key.axis));
        const float n_along = std::fabs(Comp(s.normal, MainAxis(key.axis)));
        const float spread = s.radius*s.elongation*std::sqrt(std::max(0.f, 1.f - n_along*n_along));
        r.lo = std::min(r.lo, along - spread);
        r.hi = std::max(r.hi, along + spread);
        r.last_deposit = std::max(r.last_deposit, s.time);
        GrowStained(r, s, tile);
        return id;
    }

    // The page rectangle (0..1) round its stamps, widened by `margin` metres;
    // false if nothing has been stamped.
    static bool StainedRect(const PageRecord& r, float tile, float margin, float& u0, float& v0, float& u1, float& v1)
    {
        if (r.u1 < r.u0 || r.v1 < r.v0) return false;
        const float m = margin/tile;
        u0 = Clamp(r.u0 - m, 0.f, 1.f); v0 = Clamp(r.v0 - m, 0.f, 1.f);
        u1 = Clamp(r.u1 + m, 0.f, 1.f); v1 = Clamp(r.v1 + m, 0.f, 1.f);
        return u1 > u0 && v1 > v0;
    }

    // Physical slot for a page. A page not resident takes a free slot, or the
    // slot of the resident page least worth keeping that was not drawn this
    // frame. `fresh` asks the caller to clear the slot and replay the log.
    int Acquire(int id, uint32_t frame, const Vec& camera, float tile, bool& fresh)
    {
        fresh = false;
        PageRecord& r = m_records[id];
        r.last_visible = frame;
        if (r.slot >= 0) return r.slot;
        int slot = -1;
        for (int i = 0; i < int(m_slot_owner.size()); ++i)
            if (m_slot_owner[i] < 0) { slot = i; break; }
        if (slot < 0)
        {
            float worst = -1;
            for (int i = 0; i < int(m_slot_owner.size()); ++i)
            {
                const PageRecord& o = m_records[m_slot_owner[i]];
                if (o.last_visible == frame) continue;
                const float score = EvictionScore(o, frame, camera, tile);
                if (score > worst) { worst = score; slot = i; }
            }
            if (slot < 0) return -1;
            m_records[m_slot_owner[slot]].slot = -1;
        }
        m_slot_owner[slot] = id;
        r.slot = slot;
        fresh = true;
        return slot;
    }

    // Releases every resident slot (device reset or resize). Logs remain.
    void ReleaseSlots(int slots)
    {
        for (PageRecord& r : m_records) r.slot = -1;
        m_limits.slots = slots;
        m_slot_owner.assign(std::max(slots, 0), -1);
    }

private:
    // Grows the page's stained rectangle to the splat's stamp quad.
    static void GrowStained(PageRecord& r, const Splat& s, float tile)
    {
        const StampQuad q = QuadInPage(s, r.key, tile);
        const float eu = std::fabs(q.au) + std::fabs(q.bu), ev = std::fabs(q.av) + std::fabs(q.bv);
        r.u0 = std::min(r.u0, Clamp(q.cu - eu, 0.f, 1.f)); r.u1 = std::max(r.u1, Clamp(q.cu + eu, 0.f, 1.f));
        r.v0 = std::min(r.v0, Clamp(q.cv - ev, 0.f, 1.f)); r.v1 = std::max(r.v1, Clamp(q.cv + ev, 0.f, 1.f));
    }

    int Allocate(const PageKey& key)
    {
        int id;
        if (!m_free.empty()) { id = m_free.back(); m_free.pop_back(); }
        else { id = int(m_records.size()); m_records.emplace_back(); }
        PageRecord& r = m_records[id];
        r = PageRecord();
        r.key = key; r.used = true;
        r.log.reserve(size_t(m_limits.log_capacity));
        m_index[key] = id;
        ++m_live;
        return id;
    }

    static float EvictionScore(const PageRecord& r, uint32_t frame, const Vec& camera, float tile)
    {
        int t1, t2; TangentAxes(MainAxis(r.key.axis), t1, t2);
        Vec c;
        float* comps[3] = {&c.x, &c.y, &c.z};
        *comps[t1] = (float(r.key.u) + 0.5f)*tile;
        *comps[t2] = (float(r.key.v) + 0.5f)*tile;
        *comps[MainAxis(r.key.axis)] = 0.5f*(r.lo + r.hi);
        return Length(Sub(c, camera)) + 0.01f*float(frame - r.last_visible);
    }

    // Forgets the page least worth keeping; returns its slot if resident.
    int DropOldest(const Vec& camera, float tile)
    {
        int victim = -1;
        float worst = -1e30f;
        uint32_t newest = 0;
        for (const PageRecord& r : m_records) if (r.used) newest = std::max(newest, r.last_visible);
        for (int i = 0; i < int(m_records.size()); ++i)
        {
            const PageRecord& r = m_records[i];
            if (!r.used) continue;
            const float score = EvictionScore(r, newest, camera, tile) - 0.05f*r.last_deposit;
            if (score > worst) { worst = score; victim = i; }
        }
        if (victim < 0) return -1;
        PageRecord& r = m_records[victim];
        const int slot = r.slot;
        if (slot >= 0) m_slot_owner[slot] = -1;
        m_index.erase(r.key);
        r.used = false; r.slot = -1; r.log.clear();
        m_free.push_back(victim);
        --m_live;
        return slot;
    }

    void AddToLog(PageRecord& r, const Splat& s)
    {
        const float max_radius = ShapeParams().max_radius;
        for (Splat& o : r.log)
        {
            // The next segment of a run extends the run already logged.
            if (s.kind == SplatRun)
            {
                if (o.kind == SplatRun && s.group != 0 && o.group == s.group) { ExtendRun(o, s); return; }
                continue;
            }
            // Every wipe is its own mark; runs only grow by their segments.
            if (s.kind == SplatSmear || o.kind == SplatSmear || o.kind == SplatRun) continue;
            const float d = Length(Sub(o.pos, s.pos));
            // A drop falling into a pool joins it.
            if (o.kind == SplatPool && s.kind == SplatDroplet && d < std::max(0.03f, 0.8f*o.radius))
            {
                o.radius = std::min(std::sqrt(o.radius*o.radius + s.radius*s.radius*s.elongation), max_radius);
                o.time = std::max(o.time, s.time);
                return;
            }
            // A splat landing on an earlier one merges into it (repeated hits
            // on one spot thicken the stain instead of filling the log).
            if (d < 0.03f && o.kind == s.kind)
            {
                o.amount = std::min(1.f, o.amount + 0.5f*s.amount);
                // Drops on one spot add their area; a pool request already
                // carries the accumulated volume (a corpse's ooze), so it
                // keeps the larger size.
                o.radius = s.kind == SplatPool ? std::max(o.radius, s.radius)
                    : std::min(std::sqrt(o.radius*o.radius + 0.5f*s.radius*s.radius), max_radius);
                o.time = std::max(o.time, s.time);
                // Drips piling up on a floor (a bleeder standing still) pool.
                if (o.kind == SplatDroplet && o.normal.y > 0.7f && o.radius > 0.05f)
                {
                    o.kind = SplatPool; o.amount = 1.f; o.elongation = 1.f;
                }
                return;
            }
        }
        if (int(r.log.size()) >= m_limits.log_capacity && !r.log.empty()) Coalesce(r);
        r.log.push_back(s);
    }

    // Log full: the oldest splat merges with its nearest neighbour into one
    // wider stain of the same total area, shaped like the larger of the two.
    static void Coalesce(PageRecord& r)
    {
        size_t oldest = 0;
        for (size_t i = 1; i < r.log.size(); ++i) if (r.log[i].time < r.log[oldest].time) oldest = i;
        size_t nearest = oldest;
        float best = 1e30f;
        for (size_t i = 0; i < r.log.size(); ++i)
        {
            if (i == oldest) continue;
            const float d = Length(Sub(r.log[i].pos, r.log[oldest].pos));
            if (d < best) { best = d; nearest = i; }
        }
        if (nearest == oldest) { r.log.erase(r.log.begin() + long(oldest)); return; }
        const Splat& a = r.log[nearest];
        const Splat& b = r.log[oldest];
        const float wa = a.radius*a.radius*a.elongation, wb = b.radius*b.radius*b.elongation;
        const float wsum = std::max(wa + wb, 1e-8f);
        Splat merged = wb > wa ? b : a;
        merged.pos = Add(Mul(a.pos, wa/wsum), Mul(b.pos, wb/wsum));
        merged.radius = std::sqrt((wa + wb)/std::max(merged.elongation, 1.f)) + 0.25f*std::min(best, a.radius + b.radius);
        merged.amount = std::max(a.amount, b.amount);
        merged.time = std::max(a.time, b.time);
        r.log[nearest] = merged;
        r.log.erase(r.log.begin() + long(oldest));
    }

    StoreLimits m_limits;
    std::unordered_map<PageKey, int, PageKeyHash> m_index;
    std::vector<PageRecord> m_records;
    std::vector<int> m_free;
    std::vector<int> m_slot_owner;
    int m_live = 0;
};

// ---------------------------------------------------------------- hits
// Rejects a repeated report of one wound (same victim and bone in the same
// frame, or the same spot on the same victim) so one shot sprays once.
class HitFilter
{
public:
    bool Accept(uint32_t victim, int bone, uint32_t frame, const Vec& p)
    {
        for (const Entry& e : m_recent)
        {
            if (e.victim != victim || e.frame != frame) continue;
            if (e.bone == bone || Length(Sub(e.pos, p)) < 0.03f) return false;
        }
        m_recent[m_next] = {victim, bone, frame, p};
        m_next = (m_next + 1) % kSize;
        return true;
    }
private:
    struct Entry { uint32_t victim = 0xffffffffU; int bone = -1; uint32_t frame = 0xffffffffU; Vec pos; };
    static const int kSize = 32;
    Entry m_recent[kSize];
    int m_next = 0;
};

// ---------------------------------------------------------------- budget
// Steps quality down while blood GPU time stays over budget and back up when
// it has stayed well under it; the hysteresis keeps it from oscillating.
struct Backoff
{
    int level = 0;
    float average = 0;
    int over = 0, under = 0;
    static const int kLevels = 4;
    void Push(float ms, float budget)
    {
        average = average*0.9f + ms*0.1f;
        if (budget <= 0) { over = under = 0; return; }
        if (average > budget) { ++over; under = 0; }
        else if (average < budget*0.6f) { ++under; over = 0; }
        else { over = under = 0; }
        if (over >= 30 && level < kLevels - 1) { ++level; over = 0; }
        if (under >= 120 && level > 0) { --level; under = 0; }
    }
    float Scale() const { static const float s[kLevels] = {1.f, 0.6f, 0.35f, 0.2f}; return s[level]; }
};

struct Quality
{
    int particles;      // GPU particle pool
    int page_size;      // texels per page side
    int pages;          // physical pages
    int carriers;       // traced drops per hit at nominal damage
    int per_carrier;    // visual droplets per traced drop
    bool fluid;         // spreading pools and drips
};
inline Quality QualityPreset(int level)
{
    // Page cache memory: about 8, 45, 90 and 180 MB.
    static const Quality q[4] = {
        {16384, 128, 96, 8, 12, false},
        {65536, 256, 128, 16, 24, true},
        {131072, 256, 256, 24, 32, true},
        {262144, 512, 128, 32, 48, true},
    };
    return q[level < 0 ? 0 : (level > 3 ? 3 : level)];
}

// ---------------------------------------------------------------- ballistics
struct Contact
{
    bool hit = false;
    Vec point, normal, velocity;
    float time = 0;
    int user = -1;      // what was hit, as reported by the ray function
};

// Follows a drop under gravity and quadratic drag in straight segments,
// asking `ray(origin, dir, length, distance, normal, user)` for the first
// surface on each. The contact is where and when the drop lands.
template <class RayFn>
Contact TraceArc(Vec p, Vec v, float gravity, float drag, float max_time, int segments, RayFn&& ray)
{
    Contact c;
    const int sub = 4;
    const float seg_dt = max_time/float(std::max(segments, 1));
    const float dt = seg_dt/float(sub);
    float t = 0;
    for (int s = 0; s < segments; ++s)
    {
        Vec q = p, w = v;
        for (int i = 0; i < sub; ++i)
        {
            // Mean of old and new velocity: exact under gravity alone.
            const Vec before = w;
            w.y -= gravity*dt;
            const float speed = Length(w);
            w = Mul(w, 1.f/(1.f + drag*speed*dt));
            q = Add(q, Mul(Add(before, w), 0.5f*dt));
        }
        const Vec d = Sub(q, p);
        const float len = Length(d);
        if (len > 1e-5f)
        {
            float dist = 0; Vec normal; int user = -1;
            const Vec dir = Mul(d, 1.f/len);
            if (ray(p, dir, len, dist, normal, user))
            {
                const float f = Clamp(dist/len, 0.f, 1.f);
                c.hit = true;
                c.point = Add(p, Mul(dir, dist));
                c.normal = normal;
                c.velocity = Add(Mul(v, 1.f - f), Mul(w, f));
                c.time = t + seg_dt*f;
                c.user = user;
                return c;
            }
        }
        p = q; v = w; t += seg_dt;
    }
    c.point = p; c.velocity = v; c.time = t;
    return c;
}

// ---------------------------------------------------------------- runs
// Blood thick enough to run down a wall: how far it goes, where it stops and
// when it gets there. Planned on the CPU so a run crosses page edges, reaches
// the floor or drips off a ledge, and survives its page being evicted.
constexpr float kRunStep = 0.05f;       // metres per planned segment
constexpr float kRunSpeed = 0.08f;      // m/s at the start; it slows as it thins
constexpr float kRunVolume = 0.25f;     // smallest wall stain that runs
constexpr float kRunMaxLength = 1.f;

inline float RunLength(float volume)
{
    return volume < kRunVolume ? 0.f : Clamp(0.15f*volume/kRunVolume, 0.f, kRunMaxLength);
}
// Time to run `dist` of a run `length` long.
inline float RunTime(float dist, float length)
{
    return dist/kRunSpeed*(1.f + dist/std::max(length, 1e-3f));
}
// Down along a surface: gravity with the part into the surface removed.
inline Vec DownAlong(const Vec& n)
{
    const Vec g{0, -1, 0};
    return Normalize(Sub(g, Mul(n, Dot(g, n))), g);
}

struct RunPlan
{
    enum End { Stopped, Floor, Ledge };
    static const int kMaxPoints = 24;   // kRunMaxLength / kRunStep, the start and the end
    int end = Stopped;
    int count = 0;                  // points along the run, [0] = where the blood landed
    Vec points[kMaxPoints];
    Vec normals[kMaxPoints];        // the wall at each point
    float times[kMaxPoints] = {};   // seconds after landing
    Vec end_point;                  // floor: where the run meets it; ledge: the edge
};

// Walks down the wall in short steps, asking `ray(origin, dir, length,
// distance, normal, user)` (normal facing the ray) for the floor below each
// step and for the wall under the next point. The wall may tilt or bend; a
// step that finds no wall is a ledge, anything else in the way stops it.
template <class RayFn>
RunPlan PlanRun(const Vec& start, const Vec& normal, float volume, RayFn&& ray)
{
    RunPlan plan;
    const float length = RunLength(volume);
    Vec p = start, n = Normalize(normal);
    plan.points[0] = p; plan.normals[0] = n; plan.count = 1;
    plan.end_point = p;
    float travelled = 0;
    while (travelled < length - 1e-4f && plan.count < RunPlan::kMaxPoints)
    {
        const Vec d = DownAlong(n);
        const float step = std::min(kRunStep, length - travelled);
        float dist = 0; Vec hn; int user = -1;
        const Vec off = Add(p, Mul(n, 0.01f));
        if (ray(off, d, step + 0.01f, dist, hn, user))
        {
            const Vec hit = Add(off, Mul(d, dist));
            travelled += dist;
            plan.points[plan.count] = hit; plan.normals[plan.count] = n;
            plan.times[plan.count] = RunTime(travelled, length);
            ++plan.count;
            plan.end = hn.y > 0.7f ? RunPlan::Floor : RunPlan::Stopped;
            plan.end_point = hit;
            return plan;
        }
        const Vec q = Add(p, Mul(d, step));
        const Vec probe = Add(q, Mul(n, 0.02f));
        if (ray(probe, Mul(n, -1.f), 0.04f, dist, hn, user) && std::fabs(hn.y) < 0.7f)
        {
            p = Add(probe, Mul(n, -dist));
            n = Normalize(hn);
            travelled += step;
            plan.points[plan.count] = p; plan.normals[plan.count] = n;
            plan.times[plan.count] = RunTime(travelled, length);
            ++plan.count;
            plan.end_point = p;
            continue;
        }
        // The wall ends inside this step: the blood gathers at the edge.
        const Vec edge = Add(p, Mul(d, 0.5f*step));
        travelled += 0.5f*step;
        plan.points[plan.count] = edge; plan.normals[plan.count] = n;
        plan.times[plan.count] = RunTime(travelled, length);
        ++plan.count;
        plan.end = RunPlan::Ledge;
        plan.end_point = edge;
        return plan;
    }
    return plan;
}

// ---------------------------------------------------------------- pool edges
// Where a pool spreading on a floor runs out of that floor: an edge it pours
// over (a board, a table, a step). `same(p)` says whether the surface under
// world point p is still the pool's own (within kHeightTolerance of it).
// For each of `dirs` directions round `centre`, edge_dist gets the distance to
// the edge (found within a centimetre or so), or -1 if the floor reaches the
// pool's radius. Returns how many directions met an edge.
constexpr int kPoolEdgeDirs = 8;
inline Vec PoolEdgeDir(int i, int dirs = kPoolEdgeDirs)
{
    const float a = 6.2831853f*float(i)/float(dirs);
    return {std::cos(a), 0.f, std::sin(a)};
}
template <class SameFn>
int PoolEdges(const Vec& centre, float radius, SameFn&& same, float* edge_dist, int dirs = kPoolEdgeDirs)
{
    int found = 0;
    for (int i = 0; i < dirs; ++i)
    {
        const Vec d = PoolEdgeDir(i, dirs);
        edge_dist[i] = -1.f;
        if (same(Add(centre, Mul(d, radius)))) continue;
        float lo = 0.f, hi = radius;
        for (int k = 0; k < 5; ++k)
        {
            const float mid = 0.5f*(lo + hi);
            if (same(Add(centre, Mul(d, mid)))) lo = mid; else hi = mid;
        }
        edge_dist[i] = lo;
        ++found;
    }
    return found;
}

// ---------------------------------------------------------------- smears
// Blood transferred from a bleeding body touching a surface: a blotted
// imprint where it rests, a wipe along the surface where it slides (from the
// previous contact point to this one).
struct SmearShape { Vec pos, dir; float length = 0; };
inline SmearShape SmearFromContact(const Vec& prev, const Vec& cur, const Vec& normal, float max_length = 0.6f)
{
    SmearShape s;
    const Vec n = Normalize(normal);
    const Vec move = Sub(cur, prev);
    const Vec along = Sub(move, Mul(n, Dot(move, n)));
    const float len = Length(along);
    if (len < 0.01f) { s.pos = cur; s.dir = AnyTangent(n); return s; }
    s.dir = Mul(along, 1.f/len);
    s.length = std::min(len, max_length);
    // The wipe ends where the body is now.
    s.pos = Sub(cur, Mul(s.dir, 0.5f*s.length));
    return s;
}
}
