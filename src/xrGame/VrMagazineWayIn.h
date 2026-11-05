#pragma once
#include "VrMagazineContact.h"

// The magazine's way into its well, made physical. The reload animation's
// last approach (InsertPath from SampleInsertPath) is the animator's motion:
// pistols swing the fresh magazine in from the side, clipping through the
// grip, so its outer end ran sideways and the zone's ring at the mouth sat
// on that swing. Here the way in is chosen by what a magazine in a well can
// do:
// - a magazine seated on the weapon's centre plane moves in that plane;
// - a pistol's box magazine slides straight up its grip (the magazine's own
//   axis);
// - a long gun's slides straight along the animation's push, or rocks in
//   (an AK's) when the animation turns it about the weapon's cross axis;
// - the mouth is where the magazine starts entering the receiver; when the
//   path never clears the receiver, how deep the receiver covers the seated
//   magazine;
// - the path reaches past the mouth by the zone and a margin, straight
//   along its way in, so the lead always starts outside the receiver.
// A magazine off the centre plane (a PKM's box on the side) keeps its path.
// Engine-free, so the choice is unit tested.
namespace vr
{
struct WayInInput
{
    InsertPath raw;          // poses and lead, from the animation or straight
    bool animated=false;     // raw comes from the reload animation
    V3 axis{0,1,0};          // the magazine's own axis, into the well (unit)
    float centre_x=0.f;      // the bore's line across the model
    bool handgun=false;
    // A magazine found by its motion alone (a drum, a belt box hung on the
    // side): its animated path is kept as it is.
    bool keep=false;
    float extra=0.055f;      // reach past the mouth: zone reach + re-arm + 3 cm
};
enum WayInKind { WayInRaw, WayInRock, WayInPush, WayInMagazine };
struct WayInResult
{
    InsertPath path;
    WayInKind kind=WayInRaw;
    bool centred=false;
    float enclosed=0.f;       // how deep the receiver covers the seated magazine (m)
    float pitch_deg=0.f;      // the animation's turn about the cross axis inside the well
    float deviation=0.f;      // the animation's lead off a straight line inside the well (m)
    float raw_lateral=0.f;    // the raw path's lead across the centre plane inside the well (m)
    bool mouth_from_depth=false;
    bool extended=false;
};
inline const char* WayInName(WayInKind k)
{
    static const char* names[]={"the animation's path","rocked in as animated (in plane)",
        "straight along the animation's push","straight along the magazine"};
    return names[k];
}

// Straight out of the seat (its pose `seat`) along -axis (axis points into
// the well): 32 poses over `length` of lead travel.
inline InsertPath StraightPath(const Frame& seat, const V3& lead, const V3& axis, float length)
{
    InsertPath p;
    p.lead=lead;
    for (int n=0;n<32;++n)
    {
        Frame f=seat;
        f.c=seat.c+axis*(-length*float(n)/31.f);
        p.poses.push_back(f);
    }
    p.Finish();
    return p;
}
// Re-spaced to 32 poses evenly along the lead's path (InsertPath::At
// assumes even spacing).
inline InsertPath Resample(const std::vector<Frame>& poses, const V3& lead)
{
    InsertPath p;
    p.lead=lead;
    if (poses.size()<2) { p.poses=poses; p.Finish(); return p; }
    std::vector<float> along(poses.size(),0.f);
    for (size_t n=1;n<poses.size();++n)
        along[n]=along[n-1]+Length(poses[n].Apply(lead)-poses[n-1].Apply(lead));
    size_t segment=0;
    for (int i=0;i<32;++i)
    {
        const float at=along.back()*float(i)/31.f;
        while (segment+2<poses.size() && along[segment+1]<at) ++segment;
        const float length=along[segment+1]-along[segment];
        const float u=length>1e-6f ? std::max(0.f,std::min(1.f,(at-along[segment])/length)) : 0.f;
        p.poses.push_back(Blend(poses[segment],poses[segment+1],u));
    }
    p.Finish();
    return p;
}
// The pose turned from the seat's only about the weapon's cross (x) axis,
// by its turn of the magazine's axis in the y-z plane, its lead kept on the
// seat's plane. `axis` is in weapon space at the seat.
inline Frame PitchOnly(const Frame& f, const Frame& seat, const V3& axis, const V3& lead)
{
    const V3 v=f.Rotate(seat.InverseRotate(axis));
    const float angle=std::atan2(v.z,v.y)-std::atan2(axis.z,axis.y);
    const float c=std::cos(angle), s=std::sin(angle);
    // The turn in weapon space, after the seat's own orientation.
    Frame turn;
    turn.i=V3(1,0,0); turn.j=V3(0,c,s); turn.k=V3(0,-s,c);
    Frame r;
    r.i=turn.Rotate(seat.i); r.j=turn.Rotate(seat.j); r.k=turn.Rotate(seat.k);
    V3 at=f.Apply(lead);
    at.x=seat.Apply(lead).x;
    r.c=at-r.Rotate(lead);
    return r;
}
inline float PitchOf(const Frame& f, const Frame& seat, const V3& axis)
{
    const V3 v=f.Rotate(seat.InverseRotate(axis));
    float a=std::atan2(v.z,v.y)-std::atan2(axis.z,axis.y);
    while (a>3.14159265f) a-=6.2831853f;
    while (a<-3.14159265f) a+=6.2831853f;
    return std::abs(a);
}
// The principal axis of points in the y-z plane, turned to agree with
// `along`.
inline V3 PlaneAxis(const std::vector<V3>& points, const V3& along)
{
    if (points.size()<8) return along;
    // Only the body between the floorplate (lowest 20%) and the feed lips
    // (top 15%), re-cut along each new estimate.
    V3 axis=Normalized(V3(0.f,along.y,along.z));
    if (Length(axis)<0.5f) return along;
    for (int pass=0;pass<4;++pass)
    {
        float lo=1e30f, hi=-1e30f;
        for (const V3& p:points) { const float t=Dot(p,axis); lo=std::min(lo,t); hi=std::max(hi,t); }
        const float from=lo+0.2f*(hi-lo), to=hi-0.15f*(hi-lo);
        float my=0.f, mz=0.f; unsigned n=0;
        for (const V3& p:points) { const float t=Dot(p,axis); if (t>=from && t<=to) { my+=p.y; mz+=p.z; ++n; } }
        if (n<8) return along;
        my/=float(n); mz/=float(n);
        float yy=0.f, yz=0.f, zz=0.f;
        for (const V3& p:points)
        {
            const float t=Dot(p,axis);
            if (t<from || t>to) continue;
            const float y=p.y-my, z=p.z-mz;
            yy+=y*y; yz+=y*z; zz+=z*z;
        }
        // Not a long box (no clear long axis): keep the slab line.
        const float trace=yy+zz, det=yy*zz-yz*yz;
        const float gap=std::sqrt(std::max(0.f,trace*trace*0.25f-det));
        const float largest=0.5f*trace+gap, least=0.5f*trace-gap;
        if (largest<2.f*std::max(least,1e-12f)) return along;
        // Larger eigenvector of [[yy yz][yz zz]].
        const float angle=0.5f*std::atan2(2.f*yz,yy-zz);
        V3 next(0.f,std::cos(angle),std::sin(angle));
        if (Dot(next,axis)<0.f) next=-next;
        axis=next;
    }
    // Far from the slab line, something else than a box: keep that line.
    return Dot(axis,Normalized(along))>=std::cos(25.f/57.29578f) ? axis : along;
}
// How deep the receiver covers the seated magazine: the deepest sample
// (along the axis from the lead) within `touch` of the receiver.
inline float EnclosedDepth(const ContactField& receiver, const std::vector<V3>& samples, const V3& lead,
    const V3& axis, float touch=0.006f)
{
    float depth=0.f;
    for (const V3& p:samples)
        if (receiver.Distance(p)<touch) depth=std::max(depth,Dot(lead-p,axis));
    return depth;
}
// The lead's path as far as `travel` from the seat (path order).
inline void InWell(const InsertPath& p, float travel, std::vector<V3>& out)
{
    out.clear();
    float along=0.f;
    V3 previous=p.poses.front().Apply(p.lead);
    out.push_back(previous);
    for (size_t n=1;n<p.poses.size();++n)
    {
        const V3 q=p.poses[n].Apply(p.lead);
        along+=Length(q-previous);
        previous=q;
        if (along>travel+1e-4f) break;
        out.push_back(q);
    }
}
// Straight on along the path's outer tangent until it reaches `extra` past
// the mouth; the mouth keeps its place along the lead's path.
inline bool ExtendPast(InsertPath& p, float extra)
{
    if (!p.Valid()) return false;
    const float beyond=(1.f-p.mouth)*p.length;
    if (beyond>=extra) return false;
    const float mouth_travel=p.mouth*p.length;
    const V3 out=p.Tangent(1.f);
    std::vector<Frame> poses=p.poses;
    const Frame end=poses.back();
    const float more=extra-beyond;
    for (int n=1;n<=8;++n)
    {
        Frame f=end;
        f.c=end.c+out*(more*float(n)/8.f);
        poses.push_back(f);
    }
    p=Resample(poses,p.lead);
    p.mouth=p.length>1e-6f ? std::min(1.f,mouth_travel/p.length) : 1.f;
    return true;
}

// Chooses the path (the caller measures the mouth on it, then FinishWayIn).
inline WayInResult ChooseWayIn(const WayInInput& in, const ContactField& receiver, const std::vector<V3>& samples)
{
    WayInResult r;
    r.path=in.raw;
    if (!in.raw.Valid() || samples.empty()) return r;
    // Samples, lead and poses are in the magazine's own space (the seat's
    // pose is the identity in the game; the tests place it).
    const V3& lead=in.raw.lead;
    const Frame seat=in.raw.poses.front();
    const V3 seated_lead=seat.Apply(lead);
    std::vector<V3> seated;
    seated.reserve(samples.size());
    V3 mean;
    for (const V3& p:samples) { seated.push_back(seat.Apply(p)); mean=mean+seated.back(); }
    mean=mean*(1.f/float(samples.size()));
    r.centred=std::abs(mean.x-in.centre_x)<0.015f;
    r.enclosed=EnclosedDepth(receiver,seated,seated_lead,in.axis);
    if (in.keep) r.centred=false;
    if (!r.centred) return r;
    // The magazine's own axis, in the centre plane.
    V3 axis=Normalized(V3(0.f,in.axis.y,in.axis.z));
    if (Length(axis)<0.5f) axis=V3(0,1,0);
    // A handgun's box magazine slides along its long axis: the principal
    // axis of its surface in the plane. The line between its end slabs
    // leans with the floorplate and the top round's nose (a Glock's by
    // several degrees), and the magazine then slid aslant of itself.
    if (in.handgun) axis=PlaneAxis(seated,axis);
    const float well=std::max(r.enclosed,0.02f);
    const float length=std::max(in.raw.length,r.enclosed)+in.extra;
    // How far the raw path strays across the plane inside the well.
    std::vector<V3> leads;
    InWell(in.raw,well,leads);
    for (const V3& q:leads) r.raw_lateral=std::max(r.raw_lateral,std::abs(q.x-seated_lead.x));
    if (in.handgun || !in.animated)
    {
        r.path=StraightPath(seat,lead,axis,length);
        r.kind=WayInMagazine;
        return r;
    }
    // The animation's path in the plane.
    std::vector<Frame> flat;
    for (const Frame& f:in.raw.poses) flat.push_back(PitchOnly(f,seat,axis,lead));
    const InsertPath projected=Resample(flat,lead);
    InWell(projected,well,leads);
    float pitch=0.f;
    for (size_t n=0;n<projected.poses.size() && n<leads.size();++n)
        pitch=std::max(pitch,PitchOf(projected.poses[n],seat,axis));
    r.pitch_deg=pitch*57.29578f;
    const V3 top=leads.front(), deep=leads.back();
    const V3 chord=top-deep;
    const float chord_length=Length(chord);
    for (const V3& q:leads)
    {
        const V3 d=q-deep;
        const V3 off=chord_length>1e-5f ? d-chord*(Dot(d,chord)/(chord_length*chord_length)) : d;
        r.deviation=std::max(r.deviation,Length(off));
    }
    // A rigid box deep in its well cannot turn in it: a rock-in is a
    // shallow well's (an AK's lug and catch).
    if (r.pitch_deg>=4.f && r.enclosed<0.05f)
    {
        r.path=projected;
        r.kind=WayInRock;
        return r;
    }
    if (chord_length>=0.015f && r.deviation<=0.003f)
    {
        const V3 push=chord*(1.f/chord_length);
        // A push far off the magazine's own axis is no well line.
        if (Dot(push,axis)>=std::cos(25.f/57.29578f))
        {
            r.path=StraightPath(seat,lead,push,length);
            r.kind=WayInPush;
            return r;
        }
    }
    r.path=StraightPath(seat,lead,axis,length);
    r.kind=WayInMagazine;
    return r;
}
// After InsertPath::MeasureMouth on the chosen path: a mouth at the very end
// (the path never cleared the receiver) is put where the receiver stops
// covering the seated magazine, and the path is carried past it.
inline void FinishWayIn(WayInResult& r, float extra)
{
    InsertPath& p=r.path;
    if (!r.centred || !p.Valid()) return;
    // MeasureMouth steps 1/64 of the path: within two steps of the end.
    if ((1.f-p.mouth)*p.length<=2.f*p.length/64.f+1e-4f && r.enclosed>0.01f)
    {
        p.mouth=std::min(1.f,r.enclosed/p.length);
        r.mouth_from_depth=true;
    }
    r.extended=ExtendPast(p,extra);
}
}
