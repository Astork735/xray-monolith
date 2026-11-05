#pragma once
// Synthetic weapon geometry shared by the contact tests and the benchmark:
// a pistol with a hollow grip whose bottom is closed (as game meshes often
// are), a trigger guard, and a box magazine; plus rifle-sized meshes for
// build timing. Dimensions are in metres, weapon space +y up, +z forward.
#include "../../src/xrGame/VrFingerContact.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace fixture
{
using namespace vr;
struct Box { V3 lo, hi; };
inline void AddQuad(std::vector<Tri>& out, const V3& o, const V3& u, const V3& v, int n)
{
    for (int a=0;a<n;++a) for (int b=0;b<n;++b)
    {
        const V3 p00=o+u*(float(a)/n)+v*(float(b)/n), p10=o+u*(float(a+1)/n)+v*(float(b)/n);
        const V3 p01=o+u*(float(a)/n)+v*(float(b+1)/n), p11=o+u*(float(a+1)/n)+v*(float(b+1)/n);
        out.push_back({p00,p10,p11}); out.push_back({p00,p11,p01});
    }
}
inline void AddBox(std::vector<Tri>& out, const Box& b, int n)
{
    const V3 d=b.hi-b.lo;
    const V3 X(d.x,0,0), Y(0,d.y,0), Z(0,0,d.z);
    AddQuad(out,b.lo,Y,X,n); AddQuad(out,b.lo+Z,X,Y,n);   // back, front
    AddQuad(out,b.lo,X,Z,n); AddQuad(out,b.lo+Y,Z,X,n);   // bottom, top
    AddQuad(out,b.lo,Z,Y,n); AddQuad(out,b.lo+X,Y,Z,n);   // left, right
}
inline bool Inside(const Box& b, const V3& p, float margin)
{
    return p.x>b.lo.x+margin && p.y>b.lo.y+margin && p.z>b.lo.z+margin &&
        p.x<b.hi.x-margin && p.y<b.hi.y-margin && p.z<b.hi.z-margin;
}
inline void AddCylinder(std::vector<Tri>& out, const V3& centre, float radius, float half, int sides, int rings)
{
    for (int s=0;s<sides;++s)
    {
        const float a0=6.2831853f*s/sides, a1=6.2831853f*(s+1)/sides;
        const V3 r0(0,std::sin(a0)*radius,std::cos(a0)*radius), r1(0,std::sin(a1)*radius,std::cos(a1)*radius);
        for (int r=0;r<rings;++r)
        {
            const float x0=-half+2*half*r/rings, x1=-half+2*half*(r+1)/rings;
            const V3 p00=centre+r0+V3(x0,0,0), p01=centre+r1+V3(x0,0,0);
            const V3 p10=centre+r0+V3(x1,0,0), p11=centre+r1+V3(x1,0,0);
            out.push_back({p00,p10,p11}); out.push_back({p00,p11,p01});
        }
        out.push_back({centre+V3(-half,0,0),centre+r1+V3(-half,0,0),centre+r0+V3(-half,0,0)});
        out.push_back({centre+V3(half,0,0),centre+r0+V3(half,0,0),centre+r1+V3(half,0,0)});
    }
}

// Cuts a hole through the faces that close a passage: triangles facing along
// `axis` (|normal.axis| > 0.5) where `inside` holds, a region within the box
// lo..hi. Those are split until each piece is entirely in or out, or smaller
// than `resolution`, and pieces whose centroid is inside are dropped. Faces
// running along the passage (its walls) are kept. Opens the magazine well in
// receivers modelled as closed shells without losing the well's sides.
template<class Inside>
void CarveTriangles(std::vector<Tri>& tris, Inside inside, const V3& lo, const V3& hi,
    const V3& axis, float resolution=0.002f)
{
    std::vector<Tri> out, stack;
    out.reserve(tris.size());
    for (const Tri& t:tris)
    {
        const V3 tlo=Min(t.a,Min(t.b,t.c)), thi=Max(t.a,Max(t.b,t.c));
        if (tlo.x>hi.x || tlo.y>hi.y || tlo.z>hi.z || thi.x<lo.x || thi.y<lo.y || thi.z<lo.z ||
            std::abs(Dot(Normalized(Cross(t.b-t.a,t.c-t.a)),axis))<=0.5f)
        {
            out.push_back(t);
            continue;
        }
        stack.clear();
        stack.push_back(t);
        while (!stack.empty())
        {
            const Tri p=stack.back(); stack.pop_back();
            const bool a=inside(p.a), b=inside(p.b), c=inside(p.c);
            const V3 centre=(p.a+p.b+p.c)*(1.f/3.f);
            const float edge=std::max(Length(p.b-p.a),std::max(Length(p.c-p.b),Length(p.a-p.c)));
            if (a==b && b==c && inside(centre)==a && edge<4*resolution)
            {
                if (!a) out.push_back(p);
                continue;
            }
            if (edge<resolution)
            {
                if (!inside(centre)) out.push_back(p);
                continue;
            }
            const V3 ab=Lerp(p.a,p.b,0.5f), bc=Lerp(p.b,p.c,0.5f), ca=Lerp(p.c,p.a,0.5f);
            stack.push_back({p.a,ab,ca}); stack.push_back({ab,p.b,bc});
            stack.push_back({ca,bc,p.c}); stack.push_back({ab,bc,ca});
        }
    }
    tris.swap(out);
}

// Pistol: frame/slide block, a hollow grip with 2.5 mm walls and a closed
// bottom (the well), and a trigger guard.
struct Pistol
{
    Box frame{{-0.012f,0.0f,-0.08f},{0.012f,0.03f,0.10f}};
    Box grip{{-0.015f,-0.11f,-0.065f},{0.015f,0.0f,-0.015f}};
    Box well{{-0.0125f,-0.1075f,-0.0625f},{0.0125f,-0.0025f,-0.0175f}};
    Box guard{{-0.006f,-0.035f,-0.010f},{0.006f,-0.028f,0.035f}};
    // Magazine in its own space: lead (feed lips) at the top centre.
    Box mag{{-0.011f,0.0f,-0.015f},{0.011f,0.11f,0.015f}};
    V3 seat{0.f,-0.115f,-0.04f};   // magazine origin when seated
    std::vector<Tri> receiver, magazine;
    MagazineBody body;
    InsertPath path;
    ContactField field;

    explicit Pistol(int density=6, bool curved=false)
    {
        AddBox(receiver,frame,density);
        AddBox(receiver,grip,density);
        AddBox(receiver,well,density);
        AddBox(receiver,guard,density);
        AddBox(magazine,mag,4);
        body.Build(SurfaceSamples(magazine,0.004f,1024));
        path.lead=V3(0,0.11f,0);
        const int n=32;
        for (int i=0;i<n;++i)
        {
            const float s=float(i)/(n-1);
            Frame f;
            if (curved)
            {
                // Rock-in: the magazine turns 20 degrees about the lead's
                // lateral axis over the travel, AK-style.
                const float angle=0.35f*s;
                RotateFrame(f,V3(angle,0,0),path.lead);
            }
            f.c=f.c+seat-V3(0,0.12f*s,0);
            path.poses.push_back(f);
        }
        path.Finish();
        // Open the well: the faces across it within 2.5 mm of the swept magazine.
        std::vector<Tri> carved=receiver;
        V3 lo(1e9f,1e9f,1e9f), hi(-1e9f,-1e9f,-1e9f);
        for (const Frame& f:path.poses)
            for (const V3& p:body.samples) { lo=Min(lo,f.Apply(p)); hi=Max(hi,f.Apply(p)); }
        const float grow=0.0025f;
        lo=lo-V3(grow,grow,grow); hi=hi+V3(grow,grow,grow);
        CarveTriangles(carved,[&](const V3& p)
        {
            for (const Frame& f:path.poses)
            {
                const V3 q=f.InverseApply(p);
                if (q.x>mag.lo.x-grow && q.y>mag.lo.y-grow && q.z>mag.lo.z-grow &&
                    q.x<mag.hi.x+grow && q.y<mag.hi.y+grow && q.z<mag.hi.z+grow) return true;
            }
            return false;
        },lo,hi,Normalized(path.poses.back().Apply(path.lead)-path.poses.front().Apply(path.lead)));
        field.Build(carved);
        path.MeasureMouth(field,body.samples);
    }
};

// A rifle-sized receiver for build timing: many boxes, dense tessellation.
inline std::vector<Tri> Rifle(int density)
{
    std::vector<Tri> out;
    AddBox(out,{{-0.02f,0.0f,-0.25f},{0.02f,0.06f,0.20f}},density);    // receiver
    AddBox(out,{{-0.01f,0.02f,0.20f},{0.01f,0.04f,0.62f}},density);    // barrel
    AddBox(out,{{-0.025f,-0.02f,0.05f},{0.025f,0.05f,0.35f}},density); // fore-end
    AddBox(out,{{-0.02f,-0.12f,-0.12f},{0.02f,0.0f,-0.07f}},density);  // grip
    AddBox(out,{{-0.02f,-0.06f,-0.55f},{0.02f,0.05f,-0.25f}},density); // stock
    AddCylinder(out,{0,0.08f,0},0.02f,0.12f,48,24);                    // optic
    return out;
}

// A planar finger: joints curl about +x, the chain starts along +z.
struct Finger
{
    V3 base{0,0,0};
    float length[3]={0.040f,0.025f,0.020f};
    float radius=0.008f;
    void Joints(const float* curl, V3* out) const
    {
        out[0]=base;
        float phi=0.f;
        for (int j=0;j<3;++j)
        {
            phi+=curl[j];
            out[j+1]=out[j]+V3(0,-std::sin(phi),std::cos(phi))*length[j];
        }
    }
};
// Solves one finger's three joints against `scene`, knuckle outward.
inline void SolveFinger(const ContactScene& scene, const Finger& f, float* curl, const float* warm, bool has_warm)
{
    static const float limit[3]={1.6f,1.7f,1.2f};
    for (int j=0;j<3;++j) curl[j]=0.f;
    FitChain(curl,3,limit,warm,has_warm,[&](int j, const float* c)
    {
        V3 p[4]; f.Joints(c,p);
        float clear=1e9f;
        for (int k=j;k<3;++k) clear=std::min(clear,CapsuleClearance(scene,p[k],p[k+1],f.radius));
        return clear;
    });
}
}
