#include "../../src/xrGame/VrReloadRules.h"
#include "../../src/xrGame/VrSupportAction.h"
#include "../../src/xrGame/VrReloadDerivation.h"
#include "../../src/xrGame/VrWeaponMechanics.h"
#include "rpd_fixture.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include "check.h"
static bool NearV(const vr::V3& a, const vr::V3& b, float e=1e-4f)
{ return Near(a.x,b.x,e) && Near(a.y,b.y,e) && Near(a.z,b.z,e); }
static const float pi=3.14159265f;

// A PKM-like top cover: hinged at its front about the lateral axis, the
// latch 29 cm behind the hinge; it opens rear end up to 75 degrees.
static vr::PartDef Cover()
{
    vr::PartDef d;
    d.kind=vr::PartHinge;
    d.pivot={0.f,0.09f,0.25f};
    d.axis={1.f,0.f,0.f};
    d.min=0.f; d.max=75.f*pi/180.f;
    d.open_at=60.f*pi/180.f; d.closed_at=3.f*pi/180.f;
    d.grab={0.f,0.09f,-0.04f};
    d.grab_radius=0.06f; d.release_distance=0.06f; d.max_speed=8.f;
    return d;
}
static vr::PartInput Hand(const vr::V3& p, bool grip, bool edge, float dt=1.f/90.f)
{
    vr::PartInput in; in.tracked=true; in.grip=grip; in.grip_edge=edge; in.hand=p; in.dt=dt; return in;
}

int main()
{
    {
        // The support trigger works the part the hand is nearest, of those
        // in reach, and only that one.
        using vr::SupportAction; using vr::SupportCandidate;
        const float out=std::numeric_limits<float>::max();
        // An AK: the hand on the magazine's top, 6 cm from the charging
        // handle (in its 9 cm reach): the magazine, not the handle.
        SupportCandidate ak[]={{SupportAction::Rack,0.06f,0.09f},{SupportAction::TakeMagazine,0.015f,1.f}};
        CHECK(vr::ChooseSupportAction(ak,2)==SupportAction::TakeMagazine);
        // On the handle, the magazine 5 cm below: the handle.
        SupportCandidate handle[]={{SupportAction::Rack,0.01f,0.09f},{SupportAction::TakeMagazine,0.05f,1.f}};
        CHECK(vr::ChooseSupportAction(handle,2)==SupportAction::Rack);
        // An AR-15's catch beside its open dust cover: whichever the hand is on.
        SupportCandidate ar[]={{SupportAction::ReleaseAction,0.012f,0.04f},{SupportAction::CloseDustCover,0.03f,0.035f}};
        CHECK(vr::ChooseSupportAction(ar,2)==SupportAction::ReleaseAction);
        ar[0].distance=0.039f;
        CHECK(vr::ChooseSupportAction(ar,2)==SupportAction::CloseDustCover);
        // Nothing in reach, or not on the magazine at all.
        SupportCandidate none[]={{SupportAction::Rack,0.1f,0.09f},{SupportAction::TakeMagazine,out,1.f},
            {SupportAction::SelectLauncher,0.071f,0.07f}};
        CHECK(vr::ChooseSupportAction(none,3)==SupportAction::None);
        CHECK(vr::ChooseSupportAction(none,0)==SupportAction::None);
        // A distance not a number is never in reach.
        SupportCandidate nan[]={{SupportAction::Rack,std::numeric_limits<float>::quiet_NaN(),0.09f}};
        CHECK(vr::ChooseSupportAction(nan,1)==SupportAction::None);
        // Exact ties keep the order offered (the rack is offered first).
        SupportCandidate tie[]={{SupportAction::Rack,0.02f,0.09f},{SupportAction::SelectLauncher,0.02f,0.07f}};
        CHECK(vr::ChooseSupportAction(tie,2)==SupportAction::Rack);
    }
    // RPD's full removed-box animation exceeds 60 cm; its seated mesh is
    // near the trigger and named magazin. A legacy shape-only fallback also
    // fails because the drum is wider than its 12 cm magazine limit.
    {
        const auto delta=(vr::V3{rpd_fixture::magazine_center.x,rpd_fixture::magazine_center.y,rpd_fixture::magazine_center.z}-vr::V3{rpd_fixture::trigger.x,rpd_fixture::trigger.y,rpd_fixture::trigger.z});
        const float seat_distance=vr::Length(delta);
        CHECK(Near(seat_distance,0.153889992f) && seat_distance<0.3f);
        CHECK(rpd_fixture::magazine_width>0.12f && rpd_fixture::magazine_travel>0.6f);
        const bool rests_above=rpd_fixture::magazine_center.y>=rpd_fixture::trigger.y+0.03f;
        const bool magazine=vr::ReloadMagazineMotion(rpd_fixture::magazine_travel,
            rpd_fixture::magazine_first.y,rpd_fixture::magazine_first.z,rests_above,seat_distance<0.3f);
        CHECK(magazine);
        // The exception needs independent seated/name evidence, not merely
        // a long animation path (remote or unnamed long parts retain fallback).
        CHECK(!vr::ReloadMagazineMotion(rpd_fixture::magazine_travel,
            rpd_fixture::magazine_first.y,rpd_fixture::magazine_first.z,rests_above,false));
        // Stock wpn_pkm and wpn_pkp share this retained reload OMF.
        CHECK(vr::ReloadMagazineMotion(rpd_fixture::pkm_pkp_magazine_travel,
            rpd_fixture::pkm_pkp_magazine_first.y,rpd_fixture::pkm_pkp_magazine_first.z,false,false));
        // A belt child above the trigger was falsely identified as a clip.
        const bool belt_above=rpd_fixture::belt_center.y>=rpd_fixture::trigger.y+0.03f;
        CHECK(belt_above);
        CHECK(vr::ReloadClipMotion(rpd_fixture::belt_travel,rpd_fixture::belt_first.y,belt_above,false));
        CHECK(!vr::ReloadClipMotion(rpd_fixture::belt_travel,rpd_fixture::belt_first.y,belt_above,magazine));
        CHECK(!vr::ReloadMagazineMotion(rpd_fixture::belt_travel,rpd_fixture::belt_first.y,
            rpd_fixture::belt_first.z,belt_above,true));
        // Actual cover and belt proximity prerequisites remain valid once
        // the drum is accepted; these are numeric evidence, not engine execution.
        CHECK(rpd_fixture::cover_origin_travel<0.03f && rpd_fixture::cover_max_degrees>25.f);
        CHECK(rpd_fixture::cover_at_magazine_peak_degrees>0.5f*rpd_fixture::cover_max_degrees);
        CHECK(rpd_fixture::belt_end.x>=rpd_fixture::cover_min.x-0.025f && rpd_fixture::belt_end.x<=rpd_fixture::cover_max.x+0.025f);
        CHECK(rpd_fixture::belt_end.y>=rpd_fixture::cover_min.y-0.025f && rpd_fixture::belt_end.y<=rpd_fixture::cover_max.y+0.025f);
        CHECK(rpd_fixture::belt_end.z>=rpd_fixture::cover_min.z-0.025f && rpd_fixture::belt_end.z<=rpd_fixture::cover_max.z+0.025f);
        // GAMMA RPD: measured at bind, its parked belt rounds pull the drum's
        // box past the 30 cm seat limit; at rest (its own mesh) it is seated.
        {
            namespace g=rpd_fixture::gamma;
            const auto at=[](const rpd_fixture::Point& a, const rpd_fixture::Point& b)
            { return vr::Length(vr::V3{a.x,a.y,a.z}-vr::V3{b.x,b.y,b.z}); };
            CHECK(at(g::magazine_bind_subtree_center,g::trigger)>=0.3f);
            const bool seated=at(g::magazine_own_center,g::trigger)<0.3f;
            CHECK(seated && g::magazine_width>0.12f);
            const bool above=g::magazine_own_center.y>=g::trigger.y+0.03f;
            // Its first move is straight back, which alone marks a bolt...
            CHECK(g::magazine_first.z<-0.8f);
            CHECK(!vr::ReloadMagazineMotion(g::magazine_travel,g::magazine_first.y,g::magazine_first.z,above,false));
            // ...but not a named magazine resting at the seat.
            CHECK(vr::ReloadMagazineMotion(g::magazine_travel,g::magazine_first.y,g::magazine_first.z,above,seated));
        }
        vr::MechanicsEvidence e; e.automatic_mode=true; e.capacity=100;
        e.detachable_magazine=magazine; e.belt=true; e.cycling_part=true;
        const auto mechanics=vr::Classify(e);
        CHECK(mechanics.family==vr::WeaponFamily::MachineGun && mechanics.action==vr::ActionKind::SelfLoading);
        CHECK(mechanics.feed==vr::FeedKind::Belt && mechanics.magazine_removable && mechanics.vr_reload_supported);
    }
    // Existing travel/direction guards and genuine top loading survive.
    // These non-stock controls deliberately use synthetic coordinates.
    {
        CHECK(vr::ReloadMagazineMotion(0.6f,0.f,0.f,false,false));
        CHECK(!vr::ReloadMagazineMotion(0.6001f,0.f,0.f,false,false));
        CHECK(!vr::ReloadMagazineMotion(0.039f,0.f,0.f,false,true));
        CHECK(vr::ReloadMagazineMotion(0.04f,0.f,0.f,false,false));
        CHECK(!vr::ReloadMagazineMotion(0.12f,0.f,-1.f,false,true)); // charging handle
        CHECK(!vr::ReloadMagazineMotion(std::numeric_limits<float>::infinity(),0.f,0.f,false,true));
        CHECK(!vr::ReloadMagazineMotion(std::numeric_limits<float>::quiet_NaN(),0.f,0.f,false,true));
        // SKS-style clip/rounds above or parked away from the receiver:
        // absent a detachable owner they keep the internal-loading evidence.
        CHECK(vr::ReloadClipMotion(0.12f,0.9f,true,false));
        CHECK(!vr::ReloadMagazineMotion(0.12f,0.9f,0.f,true,false));
        CHECK(!vr::ReloadClipMotion(0.12f,0.9f,false,false));
        CHECK(!vr::ReloadClipMotion(0.7f,0.9f,true,false));
        CHECK(!vr::ReloadClipMotion(0.12f,-0.9f,true,false));
    }
    // Reload-only magazine copies: hidden, parked or stacked at idle. A
    // second magazine the idle shows beside the seated one stays visible.
    {
        CHECK(vr::ReloadOnlyMagazine(false,false,0.05f,0.f));     // no idle: as before
        CHECK(vr::ReloadOnlyMagazine(true,true,0.05f,0.f));       // scaled away
        CHECK(vr::ReloadOnlyMagazine(true,false,0.3f,0.f));       // parked off the gun
        CHECK(vr::ReloadOnlyMagazine(true,false,0.01f,0.8f));     // stacked (AK-12 magazin_2)
        CHECK(vr::ReloadOnlyMagazine(true,false,std::numeric_limits<float>::quiet_NaN(),0.f));
        CHECK(!vr::ReloadOnlyMagazine(true,false,0.04f,0.f));     // taped beside it
        CHECK(!vr::ReloadOnlyMagazine(true,false,0.12f,0.49f));   // drum housing, partly overlapping
    }
    // The RPD-range cover opens beyond 90 degrees. Interrupted handling
    // below its open detent must not grant magazine access or firing.
    {
        vr::PartDef d=Cover(); d.max=rpd_fixture::cover_max_degrees*pi/180.f; d.open_at=0.8f*d.max;
        vr::PartState s; vr::ResetPart(d,s);
        const char* names[1]={"joint9"};
        const auto magazine_gate=vr::ParseGates("joint9:open",names,1);
        const auto fire_gate=vr::ParseGates("joint9:closed",names,1);
        vr::StepPart(d,s,0,Hand(d.grab,true,true));
        for (int i=1;i<=85;++i)
        {
            vr::StepPart(d,s,0,Hand(vr::PartMove(d,i*pi/180.f,d.grab),true,false));
            vr::SettlePart(d,s,0.f);
        }
        vr::ReloadFacts facts; facts.open[0]=s.open; facts.closed[0]=s.closed;
        CHECK(!facts.open[0] && !facts.closed[0]);
        CHECK(!vr::GatesPass(magazine_gate,facts) && !vr::GatesPass(fire_gate,facts));
        vr::StepPart(d,s,0,Hand(vr::PartAnchor(d,s),false,false));
        for (int i=0;i<300;++i) vr::SettlePart(d,s,1.f/90.f);
        CHECK(s.closed && !s.open);
        vr::StepPart(d,s,0,Hand(d.grab,true,true));
        for (int i=1;i<=114;++i)
        {
            vr::StepPart(d,s,0,Hand(vr::PartMove(d,std::fmin(i*pi/180.f,d.max),d.grab),true,false));
            vr::SettlePart(d,s,0.f);
        }
        CHECK(s.open && !s.closed && Near(s.value,d.max));
        facts.open[0]=s.open; facts.closed[0]=s.closed;
        CHECK(vr::GatesPass(magazine_gate,facts) && !vr::GatesPass(fire_gate,facts));
    }
    // Geometry: a positive angle lifts the rear (latch) end of the cover.
    {
        const vr::PartDef d=Cover();
        const vr::V3 up=vr::PartMove(d,30.f*pi/180.f,d.grab);
        CHECK(up.y>d.grab.y+0.1f && up.z>d.grab.z);
        CHECK(Near(vr::Length(up-d.pivot),vr::Length(d.grab-d.pivot)));
        // PartRotation agrees with PartMove for directions.
        float m[3][3]; vr::PartRotation(d,0.7f,m);
        const vr::V3 dir={0.f,0.f,-1.f};
        vr::PartDef o=d; o.pivot={};
        const vr::V3 moved=vr::PartMove(o,0.7f,dir);
        const vr::V3 viaRows={dir.x*m[0][0]+dir.y*m[1][0]+dir.z*m[2][0],
            dir.x*m[0][1]+dir.y*m[1][1]+dir.z*m[2][1],dir.x*m[0][2]+dir.y*m[1][2]+dir.z*m[2][2]};
        CHECK(NearV(moved,viaRows));
        // The coordinate of a moved point is its angle: hand-to-value mapping.
        const float a0=vr::PartCoordinate(d,d.grab), a1=vr::PartCoordinate(d,vr::PartMove(d,0.5f,d.grab));
        CHECK(Near(vr::PartWrap(a1-a0),0.5f,1e-3f));
    }
    // Grab only within reach, with a fresh squeeze.
    {
        const vr::PartDef d=Cover();
        vr::PartState s; vr::ResetPart(d,s);
        CHECK(!vr::StepPart(d,s,0,Hand({0.f,0.2f,-0.04f},true,true)));   // 11 cm away
        CHECK(!vr::StepPart(d,s,0,Hand(d.grab,true,false)) && s.hand<0); // no new squeeze
        CHECK(vr::StepPart(d,s,0,Hand(d.grab,true,true))&vr::PartGrabbed);
        CHECK(s.hand==0);
        // The other hand cannot take a held part.
        CHECK(!vr::StepPart(d,s,1,Hand(d.grab,true,true)) && s.hand==0);
    }
    // Following the hand along the arc, never faster than the speed limit,
    // clamped at the limits; opened with hysteresis; the hand that pulls past
    // the end of travel lets go instead of dragging the cover off its hinge.
    {
        const vr::PartDef d=Cover();
        vr::PartState s; vr::ResetPart(d,s);
        vr::StepPart(d,s,0,Hand(d.grab,true,true));
        const float dt=1.f/90.f;
        // A hand jumping to 70 degrees moves the held cover by 5*max_speed*dt
        // only (a held part keeps up with a quick flick, not a teleport).
        const vr::V3 far=vr::PartMove(d,70.f*pi/180.f,d.grab);
        vr::StepPart(d,s,0,Hand(far,true,false,dt));
        CHECK(Near(s.value,5.f*d.max_speed*dt,1e-4f));
        CHECK(s.hand<0); // the anchor lags 6+ cm behind: let go, not dragged
        // Steady motion is followed exactly.
        vr::ResetPart(d,s);
        vr::StepPart(d,s,0,Hand(d.grab,true,true));
        unsigned events=0;
        for (int i=1;i<=90;++i)
        {
            const float angle=std::fmin(i*1.f*pi/180.f,80.f*pi/180.f);
            events|=vr::StepPart(d,s,0,Hand(vr::PartMove(d,std::fmin(angle,d.max),d.grab),true,false,dt));
            events|=vr::SettlePart(d,s,dt);
        }
        CHECK(Near(s.value,d.max,1e-3f) && s.open && !s.closed && (events&vr::PartOpened));
        CHECK(s.hand==0);
        // Let go fully open: it stays open.
        CHECK(vr::StepPart(d,s,0,Hand(vr::PartAnchor(d,s),false,false))&vr::PartReleased);
        for (int i=0;i<90;++i) vr::SettlePart(d,s,dt);
        CHECK(s.open && Near(s.value,d.max,1e-3f));
        // Taken again and pushed shut: closed and latched.
        vr::StepPart(d,s,0,Hand(vr::PartAnchor(d,s),true,true));
        events=0;
        for (int i=74;i>=0;--i)
        {
            events|=vr::StepPart(d,s,0,Hand(vr::PartMove(d,i*pi/180.f,d.grab),true,false,dt));
            events|=vr::SettlePart(d,s,dt);
        }
        CHECK(s.closed && !s.open && (events&vr::PartClosed) && s.value<d.closed_at);
    }
    // Let go half open it drops shut; tracking lost briefly keeps the grip,
    // lost longer lets go.
    {
        const vr::PartDef d=Cover();
        vr::PartState s; vr::ResetPart(d,s);
        s.value=30.f*pi/180.f;
        for (int i=0;i<200;++i) vr::SettlePart(d,s,1.f/90.f);
        CHECK(Near(s.value,0.f) && s.closed);
        vr::StepPart(d,s,0,Hand(d.grab,true,true));
        vr::PartInput lost; lost.dt=0.15f;
        CHECK(!vr::StepPart(d,s,0,lost) && s.hand==0);
        CHECK(vr::StepPart(d,s,0,lost)&vr::PartSlipped);
        CHECK(s.hand<0);
    }
    // Partly open is not closed: a cover lifted 10 or 25 degrees blocks fire.
    {
        const vr::PartDef d=Cover();
        const char* names[1]={"cover"};
        const vr::GateList fire=vr::ParseGates("cover:closed",names,1);
        for (float degrees:{10.f,25.f})
        {
            vr::PartState s; vr::ResetPart(d,s);
            vr::StepPart(d,s,0,Hand(d.grab,true,true));
            for (int i=1;i<=90;++i)
            {
                const float angle=std::fmin(i*0.5f,degrees)*pi/180.f;
                vr::StepPart(d,s,0,Hand(vr::PartMove(d,angle,d.grab),true,false));
                vr::SettlePart(d,s,0.f);
            }
            vr::ReloadFacts f; f.closed[0]=s.closed; f.open[0]=s.open;
            CHECK(Near(s.value,degrees*pi/180.f,1e-3f) && !s.closed && !s.open && !vr::GatesPass(fire,f));
        }
    }
    // Taken at the edge of reach (6 cm off the latch) and lifted steadily:
    // the hand keeps it all the way.
    {
        const vr::PartDef d=Cover();
        vr::PartState s; vr::ResetPart(d,s);
        const vr::V3 off={0.06f,0.f,0.f};
        CHECK(vr::StepPart(d,s,0,Hand((d.grab+off),true,true))&vr::PartGrabbed);
        for (int i=1;i<=60;++i)
            vr::StepPart(d,s,0,Hand((vr::PartMove(d,i*pi/180.f,d.grab)+off),true,false));
        CHECK(s.hand==0 && Near(s.value,60.f*pi/180.f,1e-3f));
    }
    // The axis from how a part's points moved, including two on one line
    // through the pivot (which alone give no axis).
    {
        vr::PartDef d=Cover();
        d.axis=vr::PartNormal({0.3f,0.1f,0.95f});
        const vr::V3 before[4]={d.grab,(d.pivot+((d.grab-d.pivot)*0.5f)),
            (d.pivot+vr::V3{0.1f,0.f,0.f}),(d.pivot+vr::V3{0.f,0.1f,0.f})};
        vr::V3 after[4];
        for (int n=0;n<4;++n) after[n]=vr::PartMove(d,0.9f,before[n]);
        const vr::V3 axis=vr::AxisFromMotion(before,after,4);
        CHECK(std::fabs(std::fabs(vr::Dot(axis,d.axis))-1.f)<1e-3f);
        // The two collinear points alone are degenerate.
        CHECK(vr::Length(vr::AxisFromMotion(before,after,2))<0.5f);
    }
    // A slider (charging handle, bolt) follows the hand along its axis.
    {
        vr::PartDef d; d.kind=vr::PartSlider; d.pivot={}; d.axis={0.f,0.f,-1.f};
        d.min=0.f; d.max=0.12f; d.grab={0.05f,0.f,0.f}; d.max_speed=4.f;
        vr::PartState s; vr::ResetPart(d,s);
        vr::StepPart(d,s,1,Hand(d.grab,true,true));
        for (int i=1;i<=60;++i)
            vr::StepPart(d,s,1,Hand({0.05f,0.f,-0.002f*i},true,false,1.f/90.f));
        CHECK(Near(s.value,0.12f,1e-3f) && s.hand==1); // at the end of travel, still held
        for (int i=61;i<=100;++i)
            vr::StepPart(d,s,1,Hand({0.05f,0.f,-0.002f*i},true,false,1.f/90.f));
        CHECK(Near(s.value,0.12f,1e-3f) && s.hand<0); // pulled 6 cm past the end: let go
    }
    // Rules: a PKM refuses the box with the cover shut, the belt without the
    // box or with the cover shut, and fire with the cover open or no belt.
    {
        const char* names[1]={"cover"};
        const char* bad=nullptr;
        vr::ReloadRules rules;
        rules.magazine=vr::ParseGates("cover:open",names,1,&bad);
        rules.belt=vr::ParseGates("cover:open, magazine:in",names,1,&bad);
        rules.fire=vr::ParseGates("cover:closed, belt:seated",names,1,&bad);
        rules.chamber=vr::ParseGates("cover:closed, belt:seated",names,1,&bad);
        CHECK(!bad && rules.magazine.count==1 && rules.belt.count==2 && rules.fire.count==2);
        CHECK(vr::ParseGates("lid:open, belt:loose",names,1,&bad).count==0 && bad);
        vr::ReloadFacts f; f.closed[0]=true; f.open[0]=false; f.belt_seated=true; f.magazine_in=true;
        int blocked=-1;
        CHECK(vr::GatesPass(rules.fire,f) && !vr::GatesPass(rules.magazine,f,&blocked) && blocked==0);
        // Cover opened: box may come out; firing refused.
        f.closed[0]=false; f.open[0]=true;
        CHECK(vr::GatesPass(rules.magazine,f) && !vr::GatesPass(rules.fire,f,&blocked) && blocked==0);
        // Box out: belt cannot be laid.
        f.magazine_in=false; f.belt_seated=false;
        CHECK(!vr::GatesPass(rules.belt,f,&blocked) && blocked==1);
        // New box in, belt laid, cover shut: fires and chambers.
        f.magazine_in=true;
        CHECK(vr::GatesPass(rules.belt,f));
        f.belt_seated=true; f.open[0]=false; f.closed[0]=true;
        CHECK(vr::GatesPass(rules.fire,f) && vr::GatesPass(rules.chamber,f));
        // Shut on an unseated belt: no fire, no chambering.
        f.belt_seated=false;
        CHECK(!vr::GatesPass(rules.fire,f,&blocked) && blocked==1 && !vr::GatesPass(rules.chamber,f));
        // No rules: everything allowed (every existing weapon).
        vr::ReloadRules none;
        CHECK(none.Empty() && vr::GatesPass(none.fire,f) && vr::GatesPass(none.magazine,f));
    }
    // The belt: taken at the box mouth, seated at the tray only when allowed,
    // dropped anywhere else.
    {
        vr::BeltDef b; b.used=true; b.box_end={0.05f,0.07f,0.2f}; b.tray={0.015f,0.097f,0.2f};
        vr::BeltState s; s.seated=false;
        CHECK(!vr::StepBelt(b,s,0,Hand(b.box_end,true,true),false,true)); // not allowed: cover shut
        CHECK(vr::StepBelt(b,s,0,Hand(b.box_end,true,true),true,true)&vr::BeltTaken);
        CHECK(!vr::StepBelt(b,s,0,Hand(b.tray,true,false),true,false) && !s.seated); // cover shut again
        CHECK(vr::StepBelt(b,s,0,Hand({0.f,0.3f,0.f},false,false),true,true)&vr::BeltDropped);
        CHECK(!s.seated && s.hand<0);
        vr::StepBelt(b,s,0,Hand(b.box_end,true,true),true,true);
        CHECK(vr::StepBelt(b,s,0,Hand({0.02f,0.095f,0.2f},true,false),true,true)&vr::BeltSeated);
        CHECK(s.seated && s.hand<0);
        // A seated belt is not taken again.
        CHECK(!vr::StepBelt(b,s,0,Hand(b.box_end,true,true),true,true));
    }
    std::printf("PASS: reload derivation fixture, part geometry, grab and release, speed limit, detents, tracking loss, slider, rule gating, belt\n");
    return 0;
}
