#include "../../src/xrGame/VrPickupPolicy.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "check.h"

using vr::Release;
using vr::ReleaseDecision;

static void Releases()
{
    const float range=0.18f;
    // Drawn at the holster: relaxing the grip on the way out keeps it.
    vr::HolsterState drawn;
    CHECK(ReleaseDecision(drawn,0.05f,range,0,false,false)==Release::None);
    CHECK(ReleaseDecision(drawn,0.10f,range,100,true,true)==Release::None);
    CHECK(!drawn.armed && drawn.gripped);
    // Past its reach by the margin: armed, and opening at the holster puts it back.
    vr::HolsterState cleared;
    CHECK(ReleaseDecision(cleared,0.30f,range,50,false,false)==Release::None && cleared.armed);
    CHECK(ReleaseDecision(cleared,0.10f,range,60,true,true)==Release::Holster);
    // Within the margin it is not yet clear; held long enough it is armed anyway.
    vr::HolsterState near;
    CHECK(ReleaseDecision(near,range+0.03f,range,100,false,false)==Release::None && !near.armed);
    CHECK(ReleaseDecision(near,0.10f,range,vr::holster_arm_ms+1,false,false)==Release::None && near.armed);
    // Opened just short of the holster, then arriving there open: it goes back
    // (it used to float in the open hand).
    vr::HolsterState late;
    CHECK(ReleaseDecision(late,0.50f,range,400,false,false)==Release::None);
    CHECK(ReleaseDecision(late,0.18f+0.001f,range,410,true,true)==Release::Drop); // just outside: a drop
    vr::HolsterState arrive;
    CHECK(ReleaseDecision(arrive,0.50f,range,400,false,false)==Release::None);
    CHECK(ReleaseDecision(arrive,0.12f,range,420,true,false)==Release::Holster);
    // Opened away from the holster: out of the hand; held open there, nothing more.
    vr::HolsterState away;
    CHECK(ReleaseDecision(away,0.60f,range,400,false,false)==Release::None);
    CHECK(ReleaseDecision(away,0.60f,range,410,true,true)==Release::Drop);
    CHECK(ReleaseDecision(away,0.60f,range,420,true,false)==Release::None);
    // Drawn by a key (never gripped): an open hand resting at the holster keeps it.
    vr::HolsterState key;
    CHECK(ReleaseDecision(key,0.10f,range,1000,true,false)==Release::None && key.armed && !key.gripped);
    // Taken by a hand with its trigger (counts as gripped): goes back open.
    vr::HolsterState trigger; trigger.gripped=true;
    CHECK(ReleaseDecision(trigger,0.10f,range,1000,true,false)==Release::Holster);
    // No holster (FLT_MAX): only a drop.
    vr::HolsterState none;
    CHECK(ReleaseDecision(none,FLT_MAX,0.f,1000,true,true)==Release::Drop);
    // Released at once unless the game's own reload runs.
    CHECK(vr::ReleaseNow(true,false) && vr::ReleaseNow(false,false) && vr::ReleaseNow(true,true));
    CHECK(!vr::ReleaseNow(false,true));
}

static void Wields()
{
    using vr::ActiveItem; using vr::WieldStep; using vr::WieldDecision;
    CHECK(WieldDecision(false,false,ActiveItem::None)==WieldStep::Wait);
    CHECK(WieldDecision(false,true,ActiveItem::None)==WieldStep::Activate);
    // The game's pickup drew it already: adopt and finish. Waiting on made the
    // wield draw it again the moment it was holstered.
    CHECK(WieldDecision(false,true,ActiveItem::ThisWeapon)==WieldStep::Adopt);
    CHECK(WieldDecision(false,true,ActiveItem::OtherLeaving)==WieldStep::HoldHand);
    CHECK(WieldDecision(false,false,ActiveItem::OtherLeaving)==WieldStep::Wait);
    CHECK(WieldDecision(false,true,ActiveItem::OtherHeld)==WieldStep::Cancel);
    CHECK(WieldDecision(false,false,ActiveItem::OtherHeld)==WieldStep::Cancel);
    // A carried weapon into its holster: whatever the other hand holds.
    CHECK(WieldDecision(true,false,ActiveItem::OtherHeld)==WieldStep::Wait);
    CHECK(WieldDecision(true,true,ActiveItem::OtherHeld)==WieldStep::Stow);
    CHECK(WieldDecision(true,true,ActiveItem::None)==WieldStep::Stow);
}

static void Targets()
{
    const vr::V3 origin{0,0,0}, dir{0,0,1};
    float along=0.f, angle=0.f;
    // On the ray at 1 m: hit, angle 0.
    CHECK(vr::PointerCone(origin,dir,{0,0,1},0.02f,along,angle) && Near(along,1.f) && Near(angle,0.f));
    // 8 cm aside at 1 m: inside radius 2 cm + 7 cm tolerance; 10 cm aside is not.
    CHECK(vr::PointerCone(origin,dir,{0.08f,0,1},0.02f,along,angle) && Near(angle,0.08f));
    CHECK(!vr::PointerCone(origin,dir,{0.10f,0,1},0.02f,along,angle));
    // A larger item is easier to point at.
    CHECK(vr::PointerCone(origin,dir,{0.20f,0,1},0.15f,along,angle));
    // Behind, too close, or beyond 2 m: no.
    CHECK(!vr::PointerCone(origin,dir,{0,0,-1},0.5f,along,angle));
    CHECK(!vr::PointerCone(origin,dir,{0,0,0.02f},0.5f,along,angle));
    CHECK(!vr::PointerCone(origin,dir,{0,0,2.1f},0.5f,along,angle));

    // Ray into a box: enters at its near face, misses beside it, starts inside at 0.
    const vr::V3 lo{-0.05f,-0.05f,0.95f}, hi{0.05f,0.05f,1.05f};
    float enter=0.f;
    CHECK(vr::RayBoxEnter(origin,dir,lo,hi,2.f,enter) && Near(enter,0.95f));
    CHECK(!vr::RayBoxEnter({0.2f,0,0},dir,lo,hi,2.f,enter));
    CHECK(!vr::RayBoxEnter(origin,{0,0,-1},lo,hi,2.f,enter));
    CHECK(!vr::RayBoxEnter(origin,dir,lo,hi,0.9f,enter)); // beyond the nearest so far
    CHECK(vr::RayBoxEnter({0,0,1},dir,lo,hi,2.f,enter) && Near(enter,0.f));
    // Parallel to a face: inside its slab hits, outside misses.
    CHECK(vr::RayBoxEnter({0.04f,0,0},dir,lo,hi,2.f,enter));
    CHECK(!vr::RayBoxEnter({0.06f,0,0},dir,lo,hi,2.f,enter));

    // At the weapon: along the bore (a fore-end 40 cm out), not beside the gun hand.
    const vr::V3 gun{0,1,0}, bore{0,0,1};
    CHECK(vr::AtWeapon({0,1,0.4f},gun,bore));
    CHECK(vr::AtWeapon({0.1f,0.95f,0.3f},gun,bore));
    CHECK(!vr::AtWeapon({0.25f,1,0},gun,bore));   // beside a pistol: free to grab
    CHECK(!vr::AtWeapon({0,1,1.0f},gun,bore));    // past the bore's end
    CHECK(!vr::AtWeapon({0,0.7f,0.3f},gun,bore)); // well below it
}

int main()
{
    Releases();
    Wields();
    {
        using vr::ActiveItem; using vr::TakeIntoHand;
        // Empty hands: the weapon goes into the hand that took it.
        CHECK(TakeIntoHand(ActiveItem::None,true) && TakeIntoHand(ActiveItem::None,false));
        // A weapon still going into its holster: only into a free slot, never
        // pushing the holstered one into the backpack (it is carried instead).
        CHECK(TakeIntoHand(ActiveItem::OtherLeaving,true));
        CHECK(!TakeIntoHand(ActiveItem::OtherLeaving,false));
        // The other hand holds a weapon: carried.
        CHECK(!TakeIntoHand(ActiveItem::OtherHeld,true));
    }
    Targets();
    std::puts("PASS: weapon release, wield steps, pointer cone, ray box and at-weapon tests");
}
