#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <cstdint>
#include <vector>
#include "VrVec.h"

// Geometry-aware VR contact, engine-free so tests and benchmarks run on the
// production code. Weapon parts are represented by a sparse, banded distance
// field built once per visual from its bind-pose triangles: a query is a
// trilinear lookup of eight bytes whatever the mesh density, and gives both
// clearance and the push-out direction. Nothing here allocates per query.
namespace vr
{
// Rigid transform: rotation rows i, j, k (the engine's row-vector Fmatrix
// layout) and translation c. Apply = p.x*i + p.y*j + p.z*k + c.
struct Frame
{
    V3 i{1,0,0}, j{0,1,0}, k{0,0,1}, c{0,0,0};
    V3 Apply(const V3& p) const { return i*p.x+j*p.y+k*p.z+c; }
    V3 Rotate(const V3& d) const { return i*d.x+j*d.y+k*d.z; }
    V3 InverseApply(const V3& p) const { const V3 d=p-c; return {Dot(d,i),Dot(d,j),Dot(d,k)}; }
    V3 InverseRotate(const V3& d) const { return {Dot(d,i),Dot(d,j),Dot(d,k)}; }
    // This frame followed by `outer`: apply this first, then outer.
    Frame Then(const Frame& outer) const
    {
        Frame r;
        r.i=outer.Rotate(i); r.j=outer.Rotate(j); r.k=outer.Rotate(k); r.c=outer.Apply(c);
        return r;
    }
    Frame Inverse() const
    {
        Frame r;
        r.i={i.x,j.x,k.x}; r.j={i.y,j.y,k.y}; r.k={i.z,j.z,k.z};
        r.c=-r.Rotate(c);
        return r;
    }
};

struct Tri { V3 a, b, c; };

// Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5).
inline V3 ClosestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c)
{
    const V3 ab=b-a, ac=c-a, ap=p-a;
    const float d1=Dot(ab,ap), d2=Dot(ac,ap);
    if (d1<=0 && d2<=0) return a;
    const V3 bp=p-b;
    const float d3=Dot(ab,bp), d4=Dot(ac,bp);
    if (d3>=0 && d4<=d3) return b;
    const float vc=d1*d4-d3*d2;
    if (vc<=0 && d1>=0 && d3<=0) return a+ab*(d1/(d1-d3));
    const V3 cp=p-c;
    const float d5=Dot(ab,cp), d6=Dot(ac,cp);
    if (d6>=0 && d5<=d6) return c;
    const float vb=d5*d2-d1*d6;
    if (vb<=0 && d2>=0 && d6<=0) return a+ac*(d2/(d2-d6));
    const float va=d3*d6-d5*d4;
    if (va<=0 && (d4-d3)>=0 && (d5-d6)>=0) return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)));
    const float denom=1.f/(va+vb+vc);
    return a+ab*(vb*denom)+ac*(vc*denom);
}
inline float TriangleDistanceSq(const V3& p, const Tri& t)
{
    const V3 d=p-ClosestOnTriangle(p,t.a,t.b,t.c);
    return Dot(d,d);
}

// Sparse banded unsigned distance field. Space is split into bricks of 8^3
// cells; only bricks within `band` of a triangle exist. Each brick stores its
// 9^3 corner samples (one shared apron layer) so trilinear lookups never
// cross into a neighbour. Distances are quantised to a byte over the band,
// so beyond the band a query returns `band` ("far").
class ContactField
{
public:
    static constexpr int kBrick=8, kSamples=kBrick+1, kBrickBytes=kSamples*kSamples*kSamples;

    bool Empty() const { return m_bricks==0; }
    float Cell() const { return m_cell; }
    float Band() const { return m_band; }
    size_t Bricks() const { return m_bricks; }
    size_t Bytes() const { return m_index.size()*sizeof(uint16_t)+m_data.size(); }
    V3 BoundsMin() const { return m_origin; }
    V3 BoundsMax() const
    {
        const float side=m_cell*kBrick;
        return m_origin+V3(m_dims[0]*side,m_dims[1]*side,m_dims[2]*side);
    }

    // `cell` in metres; `band` is the largest clearance a query must resolve
    // (finger capsule radius plus margin). Meant for a loading thread; it may
    // fan out to `threads` threads.
    void Build(const std::vector<Tri>& tris, float cell=0.0025f, float band=0.012f, int threads=1)
    {
        *this=ContactField();
        m_cell=cell; m_band=band; m_scale=255.f/band;
        if (tris.empty()) return;
        V3 lo=tris[0].a, hi=lo;
        for (const Tri& t:tris) { lo=Min(lo,Min(t.a,Min(t.b,t.c))); hi=Max(hi,Max(t.a,Max(t.b,t.c))); }
        const V3 pad(band+cell,band+cell,band+cell);
        lo=lo-pad; hi=hi+pad;
        const float side=cell*kBrick;
        m_origin=lo;
        for (int a=0;a<3;++a)
        {
            const float extent=a==0 ? hi.x-lo.x : a==1 ? hi.y-lo.y : hi.z-lo.z;
            m_dims[a]=std::max(1,int(std::ceil(extent/side)));
        }
        const size_t slots=size_t(m_dims[0])*m_dims[1]*m_dims[2];
        if (slots>4000000) return;  // absurd bounds: refuse rather than exhaust memory
        // Bin triangles into every brick their band-expanded box touches.
        std::vector<std::vector<uint32_t>> bins(slots);
        for (uint32_t n=0;n<tris.size();++n)
        {
            const Tri& t=tris[n];
            const V3 tlo=Min(t.a,Min(t.b,t.c))-V3(band,band,band)-m_origin;
            const V3 thi=Max(t.a,Max(t.b,t.c))+V3(band,band,band)-m_origin;
            const int x0=Clampi(int(tlo.x/side),0,m_dims[0]-1), x1=Clampi(int(thi.x/side),0,m_dims[0]-1);
            const int y0=Clampi(int(tlo.y/side),0,m_dims[1]-1), y1=Clampi(int(thi.y/side),0,m_dims[1]-1);
            const int z0=Clampi(int(tlo.z/side),0,m_dims[2]-1), z1=Clampi(int(thi.z/side),0,m_dims[2]-1);
            for (int z=z0;z<=z1;++z) for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x)
                bins[Slot(x,y,z)].push_back(n);
        }
        m_index.assign(slots,0);
        std::vector<uint32_t> occupied;
        for (uint32_t n=0;n<slots;++n) if (!bins[n].empty()) occupied.push_back(n);
        // Bricks are independent: computed in parallel into staging, then
        // packed in slot order so the result does not depend on threads.
        std::vector<uint8_t> staged(occupied.size()*kBrickBytes), used(occupied.size(),0);
        std::atomic<size_t> next{0};
        const auto work=[&]
        {
            BrickScratch scratch;
            for (size_t n=next++;n<occupied.size();n=next++)
                used[n]=BuildBrick(tris,bins[occupied[n]],occupied[n],&staged[n*kBrickBytes],scratch);
        };
        std::vector<std::thread> pool;
        for (int n=1;n<threads;++n) pool.emplace_back(work);
        work();
        for (std::thread& t:pool) t.join();
        for (size_t n=0;n<occupied.size() && m_bricks<65535;++n)
        {
            if (!used[n]) continue;
            m_index[occupied[n]]=uint16_t(++m_bricks);
            m_data.insert(m_data.end(),staged.begin()+n*kBrickBytes,staged.begin()+(n+1)*kBrickBytes);
        }
    }

    // Clearance from the nearest surface, capped at Band().
    float Distance(const V3& p) const
    {
        if (!m_bricks) return m_band;
        const float side=m_cell*kBrick;
        const V3 local=p-m_origin;
        const float bx=local.x/side, by=local.y/side, bz=local.z/side;
        if (bx<0 || by<0 || bz<0) return m_band;
        const int x=int(bx), y=int(by), z=int(bz);
        if (x>=m_dims[0] || y>=m_dims[1] || z>=m_dims[2]) return m_band;
        const uint16_t brick=m_index[Slot(x,y,z)];
        if (!brick) return m_band;
        const uint8_t* data=&m_data[size_t(brick-1)*kBrickBytes];
        float fx=(bx-x)*kBrick, fy=(by-y)*kBrick, fz=(bz-z)*kBrick;
        int i=std::min(int(fx),kBrick-1), j=std::min(int(fy),kBrick-1), k=std::min(int(fz),kBrick-1);
        fx-=i; fy-=j; fz-=k;
        const uint8_t* c=data+(k*kSamples+j)*kSamples+i;
        const int sy=kSamples, sz=kSamples*kSamples;
        const float c00=c[0]+(c[1]-c[0])*fx, c10=c[sy]+(c[sy+1]-c[sy])*fx;
        const float c01=c[sz]+(c[sz+1]-c[sz])*fx, c11=c[sz+sy]+(c[sz+sy+1]-c[sz+sy])*fx;
        const float c0=c00+(c10-c00)*fy, c1=c01+(c11-c01)*fy;
        return (c0+(c1-c0)*fz)/m_scale;
    }
    // Direction of increasing clearance (unit, or zero far from surfaces).
    V3 Gradient(const V3& p) const
    {
        const float h=m_cell*0.5f;
        const V3 g(Distance(p+V3(h,0,0))-Distance(p-V3(h,0,0)),
            Distance(p+V3(0,h,0))-Distance(p-V3(0,h,0)),
            Distance(p+V3(0,0,h))-Distance(p-V3(0,0,h)));
        return Normalized(g);
    }

private:
    struct BrickScratch
    {
        std::vector<float> best=std::vector<float>(kBrickBytes);
        std::vector<V3> boxes_lo, boxes_hi;
        std::vector<uint32_t> all, candidates;
    };
    // Exact clearances of one brick's samples, quantised into `out`; false
    // when none is within the band.
    bool BuildBrick(const std::vector<Tri>& tris, const std::vector<uint32_t>& bin, uint32_t slot,
        uint8_t* out, BrickScratch& scratch) const
    {
        const float cell=m_cell, band=m_band, side=cell*kBrick;
        const int x=int(slot%m_dims[0]), y=int(slot/m_dims[0]%m_dims[1]), z=int(slot/(size_t(m_dims[0])*m_dims[1]));
        const V3 base=m_origin+V3(x*side,y*side,z*side);
        std::vector<V3>& boxes_lo=scratch.boxes_lo;
        std::vector<V3>& boxes_hi=scratch.boxes_hi;
        boxes_lo.resize(bin.size()); boxes_hi.resize(bin.size());
        for (size_t n=0;n<bin.size();++n)
        {
            const Tri& t=tris[bin[n]];
            boxes_lo[n]=Min(t.a,Min(t.b,t.c)); boxes_hi[n]=Max(t.a,Max(t.b,t.c));
        }
        const auto nearest=[&](const V3& p, const uint32_t* list, size_t count, float limit)
        {
            float d2=limit*limit;
            for (size_t n=0;n<count;++n)
            {
                // The triangle's box bounds its distance from below.
                const uint32_t t=list[n];
                const V3 q=Max(boxes_lo[t]-p,Max(p-boxes_hi[t],V3(0,0,0)));
                if (Dot(q,q)>=d2) continue;
                d2=std::min(d2,TriangleDistanceSq(p,tris[bin[t]]));
            }
            return std::sqrt(d2);
        };
        scratch.all.resize(bin.size());
        for (uint32_t n=0;n<bin.size();++n) scratch.all[n]=n;
        // The 9^3 samples in 3^3 blocks of 3^3: the exact distance at a
        // block's centre bounds every sample in it (to within the block's
        // half diagonal), so only triangles within that bound are tried.
        bool any=false;
        const float reach=std::sqrt(3.f)*cell;
        const V3 half(cell,cell,cell);
        for (int bz=0;bz<3;++bz) for (int by=0;by<3;++by) for (int bx=0;bx<3;++bx)
        {
            const V3 centre=base+V3((bx*3+1)*cell,(by*3+1)*cell,(bz*3+1)*cell);
            const float limit=std::min(band,nearest(centre,scratch.all.data(),bin.size(),band+reach)+reach);
            scratch.candidates.clear();
            for (uint32_t n=0;n<bin.size();++n)
            {
                // Distance from the block's box to the triangle's box.
                const V3 gap=Max(boxes_lo[n]-(centre+half),Max((centre-half)-boxes_hi[n],V3(0,0,0)));
                if (Dot(gap,gap)<limit*limit) scratch.candidates.push_back(n);
            }
            for (int k=bz*3;k<bz*3+3;++k) for (int j=by*3;j<by*3+3;++j) for (int i=bx*3;i<bx*3+3;++i)
            {
                const int s=(k*kSamples+j)*kSamples+i;
                scratch.best[s]=nearest(base+V3(i*cell,j*cell,k*cell),scratch.candidates.data(),
                    scratch.candidates.size(),limit);
                any|=scratch.best[s]<band;
            }
        }
        for (int n=0;n<kBrickBytes;++n) out[n]=uint8_t(std::min(255.f,scratch.best[n]*m_scale+0.5f));
        return any;
    }
    static int Clampi(int v, int lo, int hi) { return v<lo ? lo : v>hi ? hi : v; }
    size_t Slot(int x, int y, int z) const { return (size_t(z)*m_dims[1]+y)*m_dims[0]+x; }
    V3 m_origin;
    int m_dims[3]={0,0,0};
    float m_cell=0.0025f, m_band=0.012f, m_scale=255.f/0.012f;
    size_t m_bricks=0;
    std::vector<uint16_t> m_index;
    std::vector<uint8_t> m_data;
};

// A field placed in a shared space (weapon model space) through the current
// pose of its part. Moving parts (slide, bolt, cover) are rigid in their own
// bone space, so only the pose changes per frame, never the field.
struct PlacedField
{
    const ContactField* field=nullptr;
    Frame to_part;  // shared space -> part space
};
// The surfaces one body can touch, as a set of placed fields.
struct ContactScene
{
    static constexpr int kMax=6;
    PlacedField parts[kMax];
    int count=0;
    float band=0.012f;
    void Clear() { count=0; }
    void Add(const ContactField* field, const Frame& part_to_shared)
    {
        if (!field || field->Empty() || count>=kMax) return;
        parts[count].field=field;
        parts[count].to_part=part_to_shared.Inverse();
        band=count ? std::min(band,field->Band()) : field->Band();
        ++count;
    }
    float Distance(const V3& p) const
    {
        float d=band;
        for (int n=0;n<count;++n)
            d=std::min(d,parts[n].field->Distance(parts[n].to_part.Apply(p)));
        return d;
    }
    // Clearance and the push-out direction (shared space) of the nearest part.
    float Contact(const V3& p, V3& normal) const
    {
        float d=band; int nearest=-1;
        V3 local;
        for (int n=0;n<count;++n)
        {
            const V3 q=parts[n].to_part.Apply(p);
            const float e=parts[n].field->Distance(q);
            if (e<d) { d=e; nearest=n; local=q; }
        }
        normal=V3(0,0,0);
        if (nearest>=0)
        {
            const V3 g=parts[nearest].field->Gradient(local);
            // to_part is the inverse of part->shared, so its rows map back.
            normal=parts[nearest].to_part.InverseRotate(g);
        }
        return d;
    }
};

// Surface points of a mesh, one per `voxel` cell, at most `cap` of them.
inline std::vector<V3> SurfaceSamples(const std::vector<Tri>& tris, float voxel, size_t cap)
{
    std::vector<V3> points;
    std::vector<uint64_t> keys;
    const auto key=[voxel](const V3& p)
    {
        const auto cell=[voxel](float v) { return uint64_t(int64_t(std::floor(v/voxel))+(1<<20))&0x1FFFFF; };
        return cell(p.x)|(cell(p.y)<<21)|(cell(p.z)<<42);
    };
    const auto add=[&](const V3& p)
    {
        const uint64_t k=key(p);
        const auto at=std::lower_bound(keys.begin(),keys.end(),k);
        if (at!=keys.end() && *at==k) return;
        keys.insert(at,k);
        points.push_back(p);
    };
    for (const Tri& t:tris)
    {
        // Vertices and, for large faces, an interior grid so flat walls are
        // sampled as densely as detailed corners.
        const float edge=std::max(Length(t.b-t.a),Length(t.c-t.a));
        const int steps=std::max(1,std::min(32,int(edge/voxel)));
        for (int u=0;u<=steps;++u)
            for (int v=0;u+v<=steps;++v)
                add(t.a+(t.b-t.a)*(float(u)/steps)+(t.c-t.a)*(float(v)/steps));
    }
    if (points.size()>cap)
    {
        const size_t stride=points.size()/cap+1;
        size_t kept=0;
        for (size_t i=0;i<points.size();i+=stride) points[kept++]=points[i];
        points.resize(kept);
    }
    return points;
}
}
