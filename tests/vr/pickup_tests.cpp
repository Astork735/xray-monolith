#include "../../src/xrGame/VrPickupPolicy.h"
#include "../../src/xrGame/VrPistolState.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "check.h"

using vr::Release;
using vr::ReleaseDecision;

static void HandTransfers()
{
    const vr::V3 grip{3.f,2.f,1.f};
    // Both receiving hands, with the weapon facing each cardinal direction.
    // A close hand behind/at the handle wields; a fore-end remains a carry.
    for (const vr::V3 forward:{vr::V3{0,0,1},vr::V3{0,0,-1},vr::V3{1,0,0},vr::V3{-1,0,0}})
        for (float side:{-1.f,1.f})
        {
            const vr::V3 across{forward.z,0,-forward.x};
            CHECK(vr::HandAtWeaponGrip(grip,grip,forward));
            CHECK(vr::HandAtWeaponGrip(grip+across*(side*0.06f)-forward*0.02f,grip,forward));
            CHECK(vr::HandAtWeaponGrip(grip+forward*0.039f,grip,forward));
            CHECK(!vr::HandAtWeaponGrip(grip+forward*0.041f,grip,forward));
            // A nearby magazine well is within the reach sphere but ahead
            // of the handle; a rifle's fore-end/rear support is farther away.
            CHECK(!vr::HandAtWeaponGrip(grip+forward*0.08f,grip,forward));
            CHECK(!vr::HandAtWeaponGrip(grip+forward*0.35f,grip,forward));
            CHECK(!vr::HandAtWeaponGrip(grip-forward*0.3f,grip,forward));
            CHECK(!vr::HandAtWeaponGrip(grip+across*(side*0.121f),grip,forward));
            CHECK(vr::HandAtWeaponGrip(grip-forward*0.02f,grip,forward*2.f));
        }
    CHECK(!vr::HandAtWeaponGrip(grip,grip,{0,0,0}));

    // The same FireTrigger block used by SwapHands: neither an already held
    // receiving trigger nor the outgoing shot state fires through a transfer.
    for (bool firing_before:{false,true})
    {
        vr::FireTrigger trigger;
        CHECK(trigger.Step(firing_before ? 1.f : 0.f)==firing_before);
        trigger.Block();
        for (float held:{1.f,0.95f,1.f,0.9f}) CHECK(!trigger.Step(held));
        CHECK(!trigger.Step(0.3f) && trigger.Step(0.8f));
        // Take-back is also a role change and requires another fresh pull.
        trigger.Block();
        CHECK(!trigger.Step(1.f) && !trigger.Step(0.f) && trigger.Step(0.8f));
    }
    // Observe the receiving hand at the transfer itself: neutral means the
    // very next fresh pull works, while a held trigger must first reset.
    for (bool receiving_held:{false,true})
    {
        vr::FireTrigger trigger;
        vr::PistolInputGate input;
        const float current=receiving_held ? 1.f : 0.f;
        trigger.Block();
        CHECK(!trigger.Step(current));
        input.Reset();
        CHECK(!input.Step(7,true,vr::TriggerPulled(current)));
        CHECK(input.Armed()==!receiving_held);
        if (receiving_held)
        {
            CHECK(!trigger.Step(1.f) && !input.Step(7,true,true));
            CHECK(!trigger.Step(0.f) && !input.Step(7,true,false));
        }
        CHECK(trigger.Step(0.8f) && input.Step(7,true,true));
    }
    // Wait through the native fire state and same-state queued restarts.
    // A rocket can exist before its later native ammunition completion.
    CHECK(vr::NativeFirePending(false,true));
    CHECK(vr::NativeFirePending(true,true));
    CHECK(vr::NativeFirePending(true,false));
    CHECK(!vr::NativeFirePending(false,false));
    CHECK(!vr::ReleaseNow(true,false,true)); // transaction wins over readiness
    CHECK(!vr::ReleaseNow(false,false,vr::NativeFirePending(false,true)));
    CHECK(vr::ReleaseNow(false,false,vr::NativeFirePending(false,false)));

    // Wait retains the outgoing release edge. If support lets go before
    // completion, cancellation drops once; regripping cancels without a drop.
    for (bool regrip:{false,true})
    {
        vr::HolsterState state;
        CHECK(ReleaseDecision(state,0.6f,0.18f,400,false,false)==Release::None);
        bool previous_grip=true; // retained by the Wait return, not swapped
        const bool current_grip=regrip;
        const auto cancelled=ReleaseDecision(state,0.6f,0.18f,410,
            !current_grip,previous_grip && !current_grip);
        CHECK(cancelled==(regrip ? Release::None : Release::Drop));
        CHECK(!vr::ReleaseNow(false,false,true)); // keep ownership until commit
        CHECK(vr::ReleaseNow(false,false,false));
        previous_grip=current_grip;
        CHECK(ReleaseDecision(state,0.6f,0.18f,420,
            !current_grip,previous_grip && !current_grip)==Release::None);
    }
}

// Policy-level sequences for the actor's physical-hand sample and a weapon's
// release history. The full actor/weapon integration needs a game session.
static void GripReleaseSequences()
{
    for (int first_hand=0;first_hand<2;++first_hand)
    {
        bool physical_grip[2]{};
        const auto sample=[&](int hand, float squeeze, bool tracked=true)
        { physical_grip[hand]=tracked && vr::GripHeld(squeeze,physical_grip[hand]); };
        int weapon_hand=first_hand;
        bool previous_grip=false;
        vr::HolsterState weapon;
        const auto release=[&](float distance, bool valid=true)
        {
            const bool grip=valid && physical_grip[weapon_hand];
            const auto result=valid ? ReleaseDecision(weapon,distance,0.18f,400,
                !grip,previous_grip && !grip) : Release::None;
            previous_grip=grip;
            return result;
        };

        // A newly drawn/switched weapon adopts a grip already held by the
        // actor, even though its own edge history starts released.
        sample(weapon_hand,0.8f); sample(weapon_hand,0.5f);
        CHECK(!vr::GripHeld(0.5f,previous_grip) && physical_grip[weapon_hand]);
        CHECK(release(0.10f)==Release::None && weapon.gripped);
        sample(weapon_hand,0.36f);
        CHECK(release(0.10f)==Release::None);
        sample(weapon_hand,vr::grip_release_threshold);
        CHECK(release(0.10f)==Release::Holster);

        // Opening away from the holster produces one drop edge, not a drop
        // on every update while the hand remains open.
        weapon={}; previous_grip=false;
        sample(weapon_hand,0.8f); sample(weapon_hand,0.5f);
        CHECK(release(0.60f)==Release::None);
        sample(weapon_hand,0.3f);
        CHECK(release(0.60f)==Release::Drop);
        CHECK(release(0.60f)==Release::None);

        // After a handover, the same physical sample is read under the new
        // weapon-hand role; the receiving hand's relaxed grip stays held.
        const int other=1-weapon_hand;
        sample(other,0.8f); sample(other,0.5f);
        weapon_hand=other;
        weapon={}; previous_grip=true; // the handover's committed role history
        CHECK(physical_grip[weapon_hand] && !physical_grip[1-weapon_hand]);
        CHECK(release(0.60f)==Release::None);

        // Losing one tracked hand clears only that hand's physical latch.
        // A still-reported squeeze cannot keep an untracked support hand held.
        sample(1-weapon_hand,0.8f);
        sample(1-weapon_hand,1.f,false);
        CHECK(!physical_grip[1-weapon_hand] && physical_grip[weapon_hand]);
        CHECK(release(0.60f)==Release::None);
        sample(weapon_hand,1.f,false);
        CHECK(release(0.60f,false)==Release::None && !previous_grip);
        sample(weapon_hand,0.5f);
        CHECK(!physical_grip[weapon_hand] && release(0.60f)==Release::None);
        sample(weapon_hand,0.8f);
        CHECK(release(0.60f)==Release::None && physical_grip[weapon_hand]);
        sample(weapon_hand,0.3f);
        CHECK(release(0.60f)==Release::Drop);
    }
}

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
    // Another weapon drawn into a hand first (by the other hand or a key):
    // the wield gives way.
    CHECK(WieldDecision(false,true,ActiveItem::OtherComing)==WieldStep::Cancel);
    CHECK(WieldDecision(false,false,ActiveItem::OtherComing)==WieldStep::Cancel);
    // A stow does not draw: it goes into its holster whatever is coming.
    CHECK(WieldDecision(true,true,ActiveItem::OtherComing)==WieldStep::Stow);
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
    HandTransfers();
    GripReleaseSequences();
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
        // Another weapon on its way to a hand (a queued draw or wield): one
        // weapon on its way at a time, so this one is carried.
        CHECK(!TakeIntoHand(ActiveItem::OtherComing,true) && !TakeIntoHand(ActiveItem::OtherComing,false));
    }
    {
        using vr::ClaimHolds;
        // In the hand: the weapon's update consumes the claim.
        CHECK(ClaimHolds(true,false,false));
        // Its own slot's draw is queued, or its wield is pending: on its way.
        CHECK(ClaimHolds(false,true,false) && ClaimHolds(false,false,true));
        // In the backpack (no slot, or the stale slot id the inventory keeps),
        // stowed in its holster, or overtaken by another slot's draw: none.
        CHECK(!ClaimHolds(false,false,false));
    }
    Targets();
    std::puts("PASS: weapon release, wield steps, pointer cone, ray box and at-weapon tests");
}
