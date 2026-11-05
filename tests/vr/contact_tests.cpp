#include "contact_fixture.h"
#include "../../src/xrGame/VrMagazineWayIn.h"
#include <atomic>
#include <cstring>
#include <new>
#include <random>
#include "check.h"

// Per-frame solves must not touch the heap.
static std::atomic<long> g_allocations{0};
void* operator new(size_t size) { ++g_allocations; if (void* p=std::malloc(size?size:1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using namespace vr;
using namespace fixture;

static Frame At(const V3& c, const V3& rotation=V3(0,0,0))
{
    Frame f; RotateFrame(f,rotation,V3(0,0,0)); f.c=c; return f;
}
int main()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(-1.f,1.f);

    // --- Field accuracy against exact triangle distances --------------------
    {
        std::vector<Tri> tris;
        AddBox(tris,{{-0.02f,-0.02f,-0.05f},{0.02f,0.02f,0.05f}},5);
        AddCylinder(tris,{0,0.05f,0},0.015f,0.04f,32,8);
        ContactField field; field.Build(tris);
        CHECK(!field.Empty());
        float worst=0.f;
        for (int n=0;n<20000;++n)
        {
            const V3 p(unit(rng)*0.04f,unit(rng)*0.06f+0.015f,unit(rng)*0.07f);
            float exact=1e9f;
            for (const Tri& t:tris) exact=std::min(exact,TriangleDistanceSq(p,t));
            exact=std::sqrt(exact);
            if (exact>field.Band()-2*field.Cell()) continue;
            worst=std::max(worst,std::abs(field.Distance(p)-exact));
        }
        // Trilinear sampling of an unsigned distance errs by at most half a
        // cell, at the surface itself; quantisation adds 0.05 mm.
        std::printf("field error max %.3f mm (cell %.1f mm)\n",worst*1000,field.Cell()*1000);
        CHECK(worst<=field.Cell()*0.5f+0.0001f);
        // Far from everything: exactly the band.
        CHECK(field.Distance(V3(1,1,1))==field.Band());
        // Push-out direction off a flat face is the face normal.
        const V3 g=field.Gradient(V3(0,0.025f,0));
        CHECK(g.y>0.95f);
    }

    Pistol pistol;
    ContactScene scene; scene.Add(&pistol.field,Frame());
    const InsertPath& path=pistol.path;
    const MagazineBody& body=pistol.body;
    CHECK(path.Valid() && std::abs(path.length-0.12f)<0.001f);

    // The carved well is open along the path.
    {
        float clear=1e9f;
        for (int i=0;i<=20;++i)
            for (const V3& p:body.samples) clear=std::min(clear,scene.Distance(path.At(i/20.f).Apply(p)));
        std::printf("well: field clearance along path %.2f mm\n",clear*1000);
        CHECK(clear>0.001f);
        CHECK(scene.Distance(V3(0.0145f,-0.11f,-0.04f))<0.001f);  // lip beside the hole
    }

    // --- Insertion zone (H3VR-style) ----------------------------------------
    {
        const InsertZone zone;
        // The mouth is found where the magazine starts entering the grip.
        const float mouth_y=path.At(path.mouth).Apply(path.lead).y;
        std::printf("well mouth: lead at y %.1f mm (grip bottom %.1f mm)\n",mouth_y*1000,pistol.grip.lo.y*1000);
        CHECK(std::abs(mouth_y-pistol.grip.lo.y)<0.01f);
        const auto below=[&](float metres) { Frame f=path.At(path.mouth); f.c=f.c+V3(0,-metres,0); return f; };
        // Far below, beside the gun or above it: outside.
        CHECK(InsertCheck(path,below(0.2f),zone)==InsertOutside);
        Frame beside=below(0.f); beside.c=beside.c+V3(0.1f,0,0);
        CHECK(InsertCheck(path,beside,zone)==InsertOutside);
        // Lined up, 5 mm below the mouth: taken; 3 cm below: not yet.
        CHECK(InsertCheck(path,below(0.005f),zone)==InsertTaken);
        CHECK(InsertCheck(path,below(0.03f),zone)!=InsertTaken);
        // Forgiving: 1 cm aside and 20 degrees off is still taken.
        Frame sloppy=below(0.003f);
        RotateFrame(sloppy,V3(0.35f,0,0),sloppy.Apply(path.lead));
        sloppy.c=sloppy.c+V3(0.01f,0,0);
        CHECK(InsertCheck(path,sloppy,zone)==InsertTaken);
        // Already in the well, or at the seat: taken.
        CHECK(InsertCheck(path,path.At(0.5f*path.mouth),zone)==InsertTaken);
        CHECK(InsertCheck(path,path.At(0.f),zone)==InsertTaken);
        // Crossways (60 degrees) or turned round: near, not taken.
        Frame crossways=below(0.01f);
        RotateFrame(crossways,V3(1.05f,0,0),crossways.Apply(path.lead));
        CHECK(InsertCheck(path,crossways,zone)==InsertNear);
        Frame backwards=below(0.01f);
        RotateFrame(backwards,V3(0,3.14159f,0),backwards.Apply(path.lead));
        CHECK(InsertCheck(path,backwards,zone)==InsertNear);
        // Pushed right through the gun (5 cm past its seat): not taken.
        Frame through=path.At(0.f); through.c=through.c+V3(0,0.05f,0);
        CHECK(InsertCheck(path,through,zone)!=InsertTaken);
        // Just outside the zone is "near" (the re-arm margin), then outside.
        CHECK(InsertCheck(path,below(0.02f),zone)==InsertNear);
        CHECK(InsertCheck(path,below(0.04f),zone)==InsertOutside);
        // A rock-in (AK) path: following its entry angle is taken.
        Pistol ak(6,true);
        CHECK(InsertCheck(ak.path,ak.path.At(ak.path.mouth),zone)==InsertTaken);
        const long before=g_allocations;
        for (int n=0;n<1000;++n) InsertCheck(path,below(0.001f*n),zone);
        CHECK(g_allocations==before);
    }

    // --- The hand-driven slide ------------------------------------------------
    {
        const InsertZone zone;
        const float dt=1.f/90.f;
        const auto at=[&](float in) { return path.At(path.mouth*(1.f-in)); };  // in: 0 mouth, 1 seat
        // Taken crooked at the mouth, it eases onto the well's line.
        Frame crooked=at(0.f);
        RotateFrame(crooked,V3(0.3f,0,0),crooked.Apply(path.lead));
        MagazineGuide guide;
        float lateral=0.f;
        guide.Take(crooked,path.Project(crooked.Apply(path.lead),lateral));
        CHECK(Length(RotationBetween(guide.Pose(path),crooked))<1e-3f);  // no jump when taken
        for (int f=0;f<10;++f) CHECK(StepGuide(guide,path,zone,crooked,dt)==GuideNone);
        CHECK(Length(RotationBetween(guide.Pose(path),path.At(guide.s)))<1e-3f);  // on the line
        // Pushed in slowly, it follows the hand and seats two thirds in.
        GuideEvent event=GuideNone;
        float seated_at=0.f;
        for (int f=0;f<=100 && event==GuideNone;++f)
        {
            seated_at=0.01f*f;
            event=StepGuide(guide,path,zone,at(seated_at),dt);
            if (event==GuideNone) CHECK(std::abs(guide.In(path)-seated_at)<0.02f);
        }
        CHECK(event==GuideSeated && seated_at>=0.66f && seated_at<=0.69f);
        // A quick push (1 cm in one frame) from a third of the way seats it.
        guide.Take(at(0.f),path.mouth);
        for (int f=0;f<=30;++f) StepGuide(guide,path,zone,at(0.3f*f/30.f),dt);
        CHECK(guide.active);
        CHECK(StepGuide(guide,path,zone,path.At(guide.s-0.01f/path.length),dt)==GuideSeated);
        // Drawn back out of the mouth, it is the hand's again.
        guide.Take(at(0.f),path.mouth);
        for (int f=0;f<=30;++f) StepGuide(guide,path,zone,at(0.2f*f/30.f),dt);
        event=GuideNone;
        for (int f=0;f<=60 && event==GuideNone;++f)
        {
            Frame out=at(0.f); out.c=out.c+V3(0,-0.001f*f,0);
            event=StepGuide(guide,path,zone,out,dt);
        }
        CHECK(event==GuideLeft && !guide.active);
        // Drawn from its seat: pushing straight back does not re-seat it;
        // half out and pushed back two thirds in, it does.
        guide.TakeFromSeat(path.At(0.f),path);
        CHECK(StepGuide(guide,path,zone,at(0.95f),dt)==GuideNone);
        for (int f=0;f<=30;++f) CHECK(StepGuide(guide,path,zone,at(0.95f-0.55f*f/30.f),dt)==GuideNone);
        event=GuideNone;
        for (int f=0;f<=60 && event==GuideNone;++f) event=StepGuide(guide,path,zone,at(0.4f+0.005f*f),dt);
        CHECK(event==GuideSeated);
        // Letting go: from the seat or anywhere past a third of the way in it
        // goes home, armed or not; short of that it falls.
        const auto released=[&](float in, bool armed)
        {
            MagazineGuide g;
            g.TakeFromSeat(path.At(0.f),path);
            g.armed=armed; g.s=path.mouth*(1.f-in);
            return OnRelease(g,path);
        };
        CHECK(released(1.f,false)==ReleaseSeats);
        CHECK(released(0.9f,false)==ReleaseSeats);
        CHECK(released(0.4f,true)==ReleaseSeats);
        CHECK(released(0.2f,true)==ReleaseDrops);
        CHECK(OnRelease(MagazineGuide(),path)==ReleaseDrops);
        // The hand closes on a seated magazine 5 cm aside and turned: nothing
        // jumps, and the magazine moves by what the hand does from there.
        Frame grabbed=At(path.At(0.f).c+V3(0.05f,0.f,0.02f),V3(0.4f,0.f,0.f));
        MagazineGuide offset;
        offset.TakeFromSeat(grabbed,path);
        CHECK(StepGuide(offset,path,zone,grabbed,dt)==GuideNone);
        CHECK(offset.active && offset.In(path)>0.99f);
        const float down=0.3f*path.mouth*path.length;
        Frame lower=grabbed; lower.c=lower.c+V3(0,-down,0);
        CHECK(StepGuide(offset,path,zone,lower,dt)==GuideNone);
        CHECK(offset.active && std::abs(offset.In(path)-0.7f)<0.02f);
    }

    // --- Fingers -------------------------------------------------------------
    {
        // A grip cylinder below and ahead of a straight finger.
        std::vector<Tri> tris;
        AddCylinder(tris,{0,-0.03f,0.035f},0.017f,0.05f,48,10);
        ContactField grip; grip.Build(tris);
        ContactScene hand; hand.Add(&grip,Frame());
        Finger finger;
        float curl[3];
        const long before=g_allocations;
        SolveFinger(hand,finger,curl,nullptr,false);
        CHECK(g_allocations==before);
        V3 p[4]; finger.Joints(curl,p);
        // Exact clearance from the analytic cylinder, sampled densely.
        float worst=1e9f;
        for (int k=0;k<3;++k)
            for (int i=0;i<=20;++i)
            {
                const V3 q=Lerp(p[k],p[k+1],i/20.f)-V3(0,-0.03f,0.035f);
                worst=std::min(worst,std::sqrt(q.y*q.y+q.z*q.z)-0.017f-finger.radius);
            }
        std::printf("finger wrap: curls %.2f %.2f %.2f, clearance %.2f mm\n",curl[0],curl[1],curl[2],worst*1000);
        CHECK(worst>-0.0015f);       // never sinks in
        CHECK(worst<0.004f);         // and actually reaches the grip
        CHECK(curl[0]>0.f && curl[0]<1.6f);
        // Warm start from the answer lands on the same pose.
        float again[3];
        SolveFinger(hand,finger,again,curl,true);
        for (int j=0;j<3;++j) CHECK(std::abs(again[j]-curl[j])<0.03f);
        // A finger meeting nothing keeps the hold's shape on every joint.
        {
            const float limit[3]={1.6f,1.7f,1.2f};
            float shape[3]={0.9f,1.1f,0.7f};
            FitChain(shape,3,limit,nullptr,false,[](int,const float*) { return 1.f; });
            CHECK(shape[0]==0.9f && shape[1]==1.1f && shape[2]==0.7f);
            // So does one already touching where it starts.
            float pressed[3]={0.9f,1.1f,0.7f};
            FitChain(pressed,3,limit,nullptr,false,[](int,const float*) { return -1.f; });
            CHECK(pressed[0]==0.9f && pressed[1]==1.1f && pressed[2]==0.7f);
        }
        // Each joint is fitted from where the one before it ended, not from
        // that joint's last try.
        {
            const float limit[3]={1.6f,1.7f,1.2f};
            float curl[3]={0.9f,1.1f,0.7f}, built[3]={};
            bool settled=true;
            FitChain(curl,3,limit,nullptr,false,[&](int j, const float* angles)
            {
                if (j) settled=settled && built[j-1]==angles[j-1];
                for (int k=j;k<3;++k) built[k]=angles[k];
                return j==1 ? 0.6f-angles[1] : 1.f;
            });
            CHECK(settled);
        }
        // The cache reuses a still grip.
        GripCache cache;
        Frame h=At(V3(0.01f,0,0));
        CHECK(!cache.Check(h,&grip,1.f));
        cache.Store(h,&grip,1.f);
        Frame jitter=h; jitter.c=jitter.c+V3(0.0004f,0,0);
        CHECK(cache.Check(jitter,&grip,1.f));
        CHECK(!cache.Check(jitter,&grip,0.5f));
        jitter.c=jitter.c+V3(0.003f,0,0);
        CHECK(!cache.Check(jitter,&grip,1.f));
        // A palm pressed 5 mm into a face is pushed back out along its normal.
        std::vector<Tri> plate;
        AddBox(plate,{{-0.05f,-0.02f,-0.05f},{0.05f,0.0f,0.05f}},4);
        ContactField face; face.Build(plate);
        ContactScene palm_scene; palm_scene.Add(&face,Frame());
        const V3 palm[1]={V3(0,0.005f,0)};
        const float radius[1]={0.01f};
        const V3 out=PalmPushOut(palm_scene,palm,radius,1);
        CHECK(out.y>0.0035f && out.y<0.0065f && std::abs(out.x)<0.001f);
    }
    // --- Way in (VrMagazineWayIn.h) -----------------------------------------
    {
        const Frame seat=path.poses.front();
        const V3 grip_bottom(0.f,pistol.grip.lo.y,0.f);
        // The animator's approach: straight in for `straight` metres of lead
        // travel, then swung out sideways and rolled (clipping the grip).
        const auto swung=[&](float straight)
        {
            InsertPath raw; raw.lead=path.lead;
            for (int i=0;i<32;++i)
            {
                const float travel=0.12f*float(i)/31.f;
                Frame f=seat;
                f.c=f.c-V3(0,std::min(travel,straight),0);
                const float beyond=std::max(0.f,travel-straight);
                if (beyond>0.f)
                {
                    RotateFrame(f,V3(0,0,6.f*beyond),f.Apply(raw.lead));
                    f.c=f.c+V3(-0.9f*beyond,-0.4f*beyond,0);
                }
                raw.poses.push_back(f);
            }
            raw.Finish();
            return raw;
        };
        const auto finish=[&](WayInResult& r, float extra)
        {
            r.path.MeasureMouth(pistol.field,body.samples);
            FinishWayIn(r,extra);
        };
        const auto across=[](const InsertPath& p) { return std::abs(p.Tangent(std::min(1.f,p.mouth)).x); };
        // A pistol: swung in from the side over the last 6 cm. The old path's
        // ring ran sideways; the way in is straight up its grip.
        {
            WayInInput in; in.raw=swung(0.06f); in.animated=true; in.handgun=true;
            InsertPath old=in.raw; old.MeasureMouth(pistol.field,body.samples);
            std::printf("way in: swung path's mouth tangent across %.2f\n",across(old));
            CHECK(across(old)>0.3f);
            WayInResult r=ChooseWayIn(in,pistol.field,body.samples);
            CHECK(r.centred && r.kind==WayInMagazine);
            finish(r,in.extra);
            const InsertPath& p=r.path;
            const float mouth_y=p.At(p.mouth).Apply(p.lead).y;
            std::printf("way in: pistol straight, mouth y %.1f mm (grip bottom %.1f), covered %.0f mm, across %.3f\n",
                mouth_y*1000,pistol.grip.lo.y*1000,r.enclosed*1000,across(p));
            CHECK(across(p)<0.02f);
            CHECK(std::abs(mouth_y-pistol.grip.lo.y)<0.012f);
            CHECK((1.f-p.mouth)*p.length>=in.extra-0.002f);
            // The seat is unchanged and the zone takes a magazine lined up below the mouth.
            CHECK(Length(p.At(0.f).c-seat.c)<1e-4f);
            Frame below=p.At(p.mouth); below.c=below.c+V3(0,-0.005f,0);
            CHECK(InsertCheck(p,below,InsertZone())==InsertTaken);
        }
        // A long gun pushed straight in, swinging only once clear: the push
        // is kept and the tail straightened.
        {
            WayInInput in; in.raw=swung(0.108f); in.animated=true; in.handgun=false;
            WayInResult r=ChooseWayIn(in,pistol.field,body.samples);
            std::printf("way in: long gun %s, turn %.1f deg, off line %.1f mm\n",WayInName(r.kind),r.pitch_deg,
                r.deviation*1000);
            CHECK(r.kind==WayInPush || r.kind==WayInMagazine);
            finish(r,in.extra);
            CHECK(across(r.path)<0.02f);
        }
        // The fixture's straight path is kept (equal within 1 mm).
        {
            WayInInput in; in.raw=path; in.animated=true;
            WayInResult r=ChooseWayIn(in,pistol.field,body.samples);
            finish(r,in.extra);
            for (int i=0;i<=10;++i)
            {
                const float travel=std::min(path.length,r.path.length)*float(i)/10.f;
                CHECK(Length(r.path.At(travel/r.path.length).Apply(path.lead)-path.At(travel/path.length).Apply(path.lead))<0.001f);
            }
        }
        // An AK in a shallow well: rocked in as animated.
        {
            std::vector<Tri> socket;
            // Walls round the magazine's top 3 cm, 2.5 mm clear, open below.
            const Box m=pistol.mag; const float g=0.0025f, w=0.0025f;
            const V3 top=seat.Apply(V3(0,m.hi.y,0));
            const float lo=top.y-0.03f, hi=top.y+0.005f;
            const float x0=seat.c.x+m.lo.x-g, x1=seat.c.x+m.hi.x+g, z0=seat.c.z+m.lo.z-g, z1=seat.c.z+m.hi.z+g;
            AddBox(socket,{{x0-w,lo,z0-w},{x0,hi,z1+w}},4);
            AddBox(socket,{{x1,lo,z0-w},{x1+w,hi,z1+w}},4);
            AddBox(socket,{{x0,lo,z0-w},{x1,hi,z0}},4);
            AddBox(socket,{{x0,lo,z1},{x1,hi,z1+w}},4);
            AddBox(socket,{{x0-w,hi,z0-w},{x1+w,hi+w,z1+w}},4);
            ContactField shallow; shallow.Build(socket);
            InsertPath rock; rock.lead=path.lead;
            for (int i=0;i<32;++i)
            {
                const float s=float(i)/31.f;
                Frame f=seat;
                RotateFrame(f,V3(0.35f*std::min(1.f,s*4.f),0,0),seat.Apply(rock.lead));
                f.c=f.c-V3(0,0.12f*s,0);
                rock.poses.push_back(f);
            }
            rock.Finish();
            WayInInput in; in.raw=rock; in.animated=true;
            WayInResult r=ChooseWayIn(in,shallow,body.samples);
            std::printf("way in: shallow rock-in %s, covered %.0f mm, turn %.1f deg\n",WayInName(r.kind),
                r.enclosed*1000,r.pitch_deg);
            CHECK(r.kind==WayInRock);
            for (int i=0;i<=10;++i)
                CHECK(Length(r.path.At(i/10.f).Apply(rock.lead)-rock.At(i/10.f).Apply(rock.lead))<0.002f);
        }
        // A box hung on the side (a PKM's): kept as animated.
        {
            WayInInput in; in.raw=swung(0.06f); in.animated=true; in.centre_x=0.06f;
            WayInResult r=ChooseWayIn(in,pistol.field,body.samples);
            CHECK(!r.centred && r.kind==WayInRaw);
            for (size_t n=0;n<in.raw.poses.size();++n) CHECK(Length(r.path.poses[n].c-in.raw.poses[n].c)<1e-6f);
        }
    }
    // A handgun magazine raked 20 degrees, with a floorplate sticking out
    // forward at its base: its long axis, not the slab line.
    {
        std::vector<Tri> mag;
        AddBox(mag,{{-0.011f,0.f,-0.015f},{0.011f,0.11f,0.015f}},4);
        AddBox(mag,{{-0.013f,-0.008f,-0.017f},{0.013f,0.f,0.03f}},4);
        std::vector<V3> samples=SurfaceSamples(mag,0.004f,1024);
        const float rake=0.349f;
        for (V3& p:samples) p=V3(p.x,p.y*std::cos(rake)-p.z*std::sin(rake),p.y*std::sin(rake)+p.z*std::cos(rake));
        const V3 axis=PlaneAxis(samples,V3(0,1,0));
        const float angle=std::atan2(axis.z,axis.y);
        std::printf("way in: principal axis %.1f deg (raked 20.0)\n",angle*57.29578f);
        CHECK(std::abs(angle-rake)<0.035f);
        // A kept (motion-found) magazine's path is its animation's.
        WayInInput in; in.raw=path; in.animated=true; in.keep=true;
        WayInResult r=ChooseWayIn(in,pistol.field,body.samples);
        CHECK(r.kind==WayInRaw && !r.centred);
    }
    std::printf("PASS: VR contact (field, insertion zone, fingers, way in)\n");
    return 0;
}
