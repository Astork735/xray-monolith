#pragma once
#include "VrContactField.h"

// Magazine insertion: a held
// magazine follows the hand exactly and never collides with the gun in the
// other hand. When its feed lips enter the zone just below the well's mouth,
// lined up within about 28 degrees, the well takes it: it eases onto the
// well's line and from then on slides in and out at the hand's depth
// (MagazineGuide), seating two thirds of the way in or on a quick push.
// Measured per weapon from its model and reload animation (the well's way
// in and its mouth).
namespace vr
{
// Released magazines follow only the start of the insertion path, then fall.
// Shared by the simulation and its debug drawing.
constexpr float kMagazineEjectShare = 0.1f;

inline Frame Orthonormal(Frame f)
{
    f.i=Normalized(f.i);
    f.j=Normalized(f.j-f.i*Dot(f.j,f.i));
    f.k=Cross(f.i,f.j);
    return f;
}
inline V3 RotateVector(const V3& v, const V3& axis, float angle)
{
    // Rodrigues; axis is unit.
    const float c=std::cos(angle), s=std::sin(angle);
    return v*c+Cross(axis,v)*s+axis*(Dot(axis,v)*(1.f-c));
}
// Rotates a frame's basis by rotation vector w (axis*angle) about `pivot`.
inline void RotateFrame(Frame& f, const V3& w, const V3& pivot)
{
    const float angle=Length(w);
    if (angle<1e-9f) return;
    const V3 axis=w*(1.f/angle);
    f.i=RotateVector(f.i,axis,angle); f.j=RotateVector(f.j,axis,angle); f.k=RotateVector(f.k,axis,angle);
    f.c=pivot+RotateVector(f.c-pivot,axis,angle);
}
// Rotation vector taking frame a's basis onto b's.
inline V3 RotationBetween(const Frame& a, const Frame& b)
{
    // R = sum b_n (a_n)^T maps a's axes onto b's.
    const V3* A[3]={&a.i,&a.j,&a.k};
    const V3* B[3]={&b.i,&b.j,&b.k};
    float r[3][3]={};
    for (int n=0;n<3;++n)
    {
        const float av[3]={A[n]->x,A[n]->y,A[n]->z}, bv[3]={B[n]->x,B[n]->y,B[n]->z};
        for (int row=0;row<3;++row) for (int col=0;col<3;++col) r[row][col]+=bv[row]*av[col];
    }
    const float trace=r[0][0]+r[1][1]+r[2][2];
    const float angle=std::acos(std::max(-1.f,std::min(1.f,(trace-1.f)*0.5f)));
    V3 axis(r[2][1]-r[1][2],r[0][2]-r[2][0],r[1][0]-r[0][1]);
    const float s=Length(axis);
    if (s<1e-7f)
    {
        if (angle<1e-3f) return V3(0,0,0);
        // Half turn: the axis is the column of R+I with the largest norm.
        int best=0;
        for (int n=1;n<3;++n) if (r[n][n]>r[best][best]) best=n;
        axis=V3(r[0][best]+(best==0),r[1][best]+(best==1),r[2][best]+(best==2));
        return Normalized(axis)*angle;
    }
    return axis*(angle/s);
}
// Interpolates two rigid frames (u in 0..1), turning about `pivot` (local
// space) while it moves in a straight line: no point of a body within r of
// the pivot moves further than u*(pivot travel + angle*r).
inline Frame Blend(const Frame& a, const Frame& b, float u, const V3& pivot=V3(0,0,0))
{
    Frame f=a;
    const V3 from=a.Apply(pivot), to=b.Apply(pivot);
    RotateFrame(f,RotationBetween(a,b)*u,from);
    f=Orthonormal(f);
    f.c=f.c+(Lerp(from,to,u)-f.Apply(pivot));
    return f;
}

// The magazine's surface samples in its own (bone) space.
struct MagazineBody
{
    std::vector<V3> samples;
    void Build(const std::vector<V3>& points) { samples=points; }
};

// The magazine's way into the well, from the weapon's reload animation (or
// a straight line along the measured well axis): magazine poses in weapon
// space from seated (s=0) to just clear of the mouth (s=1).
struct InsertPath
{
    std::vector<Frame> poses;
    V3 lead;              // magazine-space point that enters first (feed lips)
    float length=0.f;     // lead travel from mouth to seat
    bool Valid() const { return poses.size()>=2 && length>0.005f; }
    void Finish()
    {
        length=0.f;
        for (size_t n=1;n<poses.size();++n)
            length+=Length(poses[n].Apply(lead)-poses[n-1].Apply(lead));
    }
    Frame At(float s) const
    {
        s=std::max(0.f,std::min(1.f,s));
        const float f=s*float(poses.size()-1);
        const size_t n=std::min(poses.size()-2,size_t(f));
        return Blend(poses[n],poses[n+1],f-float(n));
    }
    // Outward unit direction of the lead at s.
    V3 Tangent(float s) const
    {
        const float h=1.f/float(poses.size()-1);
        const float a=std::max(0.f,s-h*0.5f), b=std::min(1.f,a+h);
        return Normalized(At(b).Apply(lead)-At(a).Apply(lead));
    }
    // Path parameter of the point nearest p (the lead's position), extended
    // past both ends along the end tangents; `lateral` is the offset from it.
    float Project(const V3& p, float& lateral) const
    {
        float best=1e30f, s=0.f;
        const float step=1.f/float(poses.size()-1);
        for (size_t n=0;n+1<poses.size();++n)
        {
            const V3 a=poses[n].Apply(lead), b=poses[n+1].Apply(lead), ab=b-a;
            const float len2=Dot(ab,ab);
            float t=len2>1e-12f ? Dot(p-a,ab)/len2 : 0.f;
            if (n>0) t=std::max(t,0.f);
            if (n+2<poses.size()) t=std::min(t,1.f);
            const V3 q=a+ab*t;
            const float d=Dot(p-q,p-q);
            if (d<best) { best=d; s=(float(n)+t)*step; lateral=std::sqrt(d); }
        }
        return s;
    }
    float mouth=1.f;        // where the magazine starts entering the receiver (path parameter)
    // Finds `mouth`: walking in from the outer end, the first place the
    // magazine's samples come within 4 mm of the receiver.
    void MeasureMouth(const ContactField& receiver, const std::vector<V3>& samples)
    {
        mouth=1.f;
        if (!Valid() || samples.empty()) return;
        for (int i=0;i<=64;++i)
        {
            const float s=1.f-float(i)/64.f;
            const Frame f=At(s);
            float d=receiver.Band();
            for (const V3& p:samples) d=std::min(d,receiver.Distance(f.Apply(p)));
            if (d<0.004f) { mouth=std::min(1.f,s+1.f/64.f); return; }
        }
    }
};

// The insertion zone, relative to the well's mouth (InsertPath::mouth).
struct InsertZone
{
    float reach=0.01f;   // how far below the mouth the zone starts (m)
    float radius=0.018f; // how far across the well's line it reaches (m)
    float tilt=0.49f;    // how far off the well's line a magazine may be (~28 degrees)
    float rearm=0.015f;  // a magazine just taken out must first leave the zone by this much
};
enum InsertState { InsertOutside, InsertNear, InsertTaken };
// Where a magazine (its pose in weapon space) is relative to the zone:
// outside it (with the re-arm margin), near it, or in it and lined up.
inline InsertState InsertCheck(const InsertPath& path, const Frame& mag, const InsertZone& zone)
{
    if (!path.Valid()) return InsertOutside;
    float lateral=0.f;
    const float s=path.Project(mag.Apply(path.lead),lateral);
    // Metres below the mouth (negative: inside the well); a magazine pushed
    // 2 cm past its seat has gone through the gun, not into it.
    const float below=(s-path.mouth)*path.length;
    const float deepest=-path.mouth*path.length-0.02f;
    if (below>zone.reach+zone.rearm || below<deepest-zone.rearm || lateral>zone.radius+zone.rearm)
        return InsertOutside;
    if (below>zone.reach || below<deepest || lateral>zone.radius) return InsertNear;
    // Lined up: its way in within `tilt` of the well's, facing the right way.
    const float at=std::min(1.f,std::max(0.f,path.mouth));
    const Frame seat=path.At(at);
    const V3 in=-path.Tangent(at);
    if (Dot(mag.Rotate(seat.InverseRotate(in)),in)<std::cos(zone.tilt)) return InsertNear;
    if (Dot(mag.k,seat.k)<0.f) return InsertNear;
    return InsertTaken;
}

// A magazine the well has taken, riding the well's path at the hand's depth.
struct MagazineGuide
{
    bool active=false;
    bool armed=false;   // may seat itself (a magazine drawn out of its seat must first come half out)
    bool from_seat=false; // drawn from the seat: the hand drives it by how far it has moved
    V3 grab_offset;     // where the lead rides relative to the hand, when from_seat
    float s=1.f;        // where it is on the path (0 seated)
    float last=1.f;     // where the hand drove it last frame
    float blend=1.f;    // 0 when taken, 1 once eased onto the well's line
    Frame hand;         // where the hand holds it now (weapon space)
    float seat_at=0.67f;   // the share of the way in at which it seats
    float slap_speed=0.4f; // or pushed in this fast (m/s), once a fifth in
    float ease=0.08f;      // seconds to ease onto the line when taken
    float stays_at=0.33f;  // let go this far in or more, it goes home

    // Taken at the mouth, from the hand.
    void Take(const Frame& held, float at)
    {
        active=true; armed=true; from_seat=false; s=last=at; hand=held; blend=0.f;
    }
    // The hand closed on a seated magazine, usually nowhere near lined up with
    // it: the hand's movement from here drives it, so it stays on the line.
    // `at`: where on the path it is when taken (a released magazine caught
    // on its way out); 0 at the seat.
    void TakeFromSeat(const Frame& held, const InsertPath& path, float at=0.f)
    {
        active=true; armed=false; from_seat=true; s=last=at; hand=held; blend=1.f;
        grab_offset=path.At(at).Apply(path.lead)-held.c;
    }
    // How far in it is: 0 at the mouth, 1 seated.
    float In(const InsertPath& path) const
    {
        return path.mouth>1e-4f ? std::max(0.f,std::min(1.f,(path.mouth-s)/path.mouth)) : 1.f;
    }
    Frame Pose(const InsertPath& path) const
    {
        const Frame on=path.At(s);
        if (blend>=1.f) return on;
        const float t=blend*blend*(3.f-2.f*blend);
        return Blend(hand,on,t,path.lead);
    }
};
enum GuideEvent { GuideNone, GuideSeated, GuideLeft };
enum ReleaseOutcome { ReleaseDrops, ReleaseSeats };
// What letting go of the magazine does. This does not depend on `armed`,
// which only stops it seating itself while it is being drawn out.
inline ReleaseOutcome OnRelease(const MagazineGuide& g, const InsertPath& path)
{
    return g.active && g.In(path)>=g.stays_at ? ReleaseSeats : ReleaseDrops;
}
// One frame of the slide: the hand's magazine pose `held` (weapon space).
inline GuideEvent StepGuide(MagazineGuide& g, const InsertPath& path, const InsertZone& zone,
    const Frame& held, float dt)
{
    if (!g.active || !path.Valid()) return GuideNone;
    g.hand=held;
    float lateral=0.f;
    const float wanted=path.Project(g.from_seat ? held.c+g.grab_offset : held.Apply(path.lead),lateral);
    // Drawn back out of the zone (or pulled well off the line): it is the
    // hand's again.
    const float top=path.mouth+zone.reach/path.length;
    if (wanted>top+zone.rearm/path.length || lateral>zone.radius+2.f*zone.rearm)
    {
        g.active=false;
        return GuideLeft;
    }
    const float drive=(g.last-wanted)*path.length/std::max(dt,1e-3f);
    g.last=wanted;
    g.s=std::max(0.f,std::min(top,wanted));
    g.blend=std::min(1.f,g.blend+dt/g.ease);
    const float in=g.In(path);
    if (in<0.5f) g.armed=true;
    if (g.armed && (in>=g.seat_at || (drive>=g.slap_speed && in>=0.2f)))
    {
        g.active=false;
        g.s=0.f;
        return GuideSeated;
    }
    return GuideNone;
}
}
