#include "../../src/xrGame/VrPistolState.h"
#include "../../src/xrGame/VrWeaponMechanics.h"
#include "ak12_fixture.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "check.h"
using Rounds=std::vector<int>;
using State=vr::PistolState<Rounds>;
static void AutomaticRifleReloads()
{
    vr::MechanicsEvidence evidence;
    evidence.cycling_part=evidence.detachable_magazine=evidence.lever_part=true;
    evidence.capacity=30;
    for (int mode:ak12_fixture::fire_modes) evidence.automatic_mode|=vr::HasAutomaticFireMode(mode);
    const auto mechanics=vr::Classify(evidence);
    CHECK(mechanics.action==vr::ActionKind::SelfLoading);
    Rounds live, fired;
    for (int id=0;id<30;++id) live.push_back(id);
    State rifle; rifle.travel_limit=.1272f; rifle.cycle_time=.1f; rifle.locks_on_empty=false;
    rifle.Initialize(live);
    for (int id=30;id<60;++id) rifle.magazines[1].push_back(id);
    const auto conserve=[&]
    {
        std::vector<int> seen(60,0);
        const auto trace=[&](const Rounds& owner)
        { for (int id:owner) { CHECK(id>=0 && id<60); ++seen[id]; } };
        trace(live); trace(rifle.magazines[0]); trace(rifle.magazines[1]); trace(fired);
        for (int count:seen) CHECK(count==1);
        CHECK(rifle.Total(live)+fired.size()==60);
    };
    const auto pull_trigger=[&]
    {
        CHECK(rifle.CanFire() && !live.empty());
        fired.push_back(live.back()); live.pop_back();
        // Same action decision as CartridgeFired; a false lever used to
        // leave the following trigger start waiting for a manual stroke.
        if (mechanics.action==vr::ActionKind::SelfLoading) rifle.AfterShot(live);
        else rifle.AfterShotManual();
        rifle.Advance(.2f,live,[]{});
        conserve();
    };
    conserve();
    for (int trigger=0;trigger<3;++trigger) { pull_trigger(); CHECK(rifle.CanFire()); }
    const int out=rifle.EjectInserted(live);
    CHECK(out==0 && rifle.inserted==-1 && live==Rounds{26});
    CHECK(rifle.EjectInserted(live)==-1); conserve();
    CHECK(rifle.TakeLoose(out)); rifle.Suspend(); // Interrupted insertion.
    CHECK(rifle.held==-1 && rifle.inserted==-1); conserve();
    CHECK(rifle.Draw() && rifle.held==1 && rifle.Insert(live));
    CHECK(live.size()==31 && live.back()==26 && !rifle.Insert(live));
    conserve();
    while (!live.empty()) pull_trigger();
    CHECK(!rifle.CanFire() && !rifle.chamber && !rifle.locked);
    CHECK(rifle.Detach(live) && rifle.magazines[rifle.held].empty());
    rifle.Suspend(); CHECK(rifle.Draw() && rifle.held==0);
    CHECK(rifle.Insert(live) && !rifle.chamber && !rifle.CanFire());
    int unexpected=0;
    rifle.MoveManualSlide(rifle.travel_limit,live,[&]{++unexpected;});
    rifle.MoveManualSlide(0.f,live,[&]{++unexpected;});
    CHECK(unexpected==0 && rifle.CanFire()); conserve();
    for (int trigger=0;trigger<3;++trigger) { pull_trigger(); CHECK(rifle.CanFire()); }
}
int main()
{
    AutomaticRifleReloads();
    // A held grip survives a relaxed squeeze; only an opened hand releases it.
    CHECK(!vr::GripHeld(0.5f,false) && vr::GripHeld(0.7f,false));
    CHECK(vr::GripHeld(0.5f,true) && vr::GripHeld(0.36f,true) && !vr::GripHeld(0.3f,true));
    {
        // A full pull and release breaks and resets at the threshold, as before.
        vr::FireTrigger t;
        CHECK(!t.Step(0.f) && !t.Step(0.6f) && t.Step(0.7f) && t.Step(1.f) && t.Step(0.9f) && !t.Step(0.6f));
        CHECK(!t.Step(0.3f) && t.Step(0.66f));
        // Quick taps that never let the trigger back past the break: each
        // one is a release and a new pull, not one long pull (full auto).
        // Blocked (the controls came back, the weapon was switched): a held
        // trigger does not fire; let go and pulled again, it does.
        t.Block();
        CHECK(!t.Step(1.f) && !t.Step(0.9f) && !t.Step(0.3f) && t.Step(0.8f));
        t.Block();
        CHECK(!t.Step(0.f) && t.Step(0.7f));
        t.Reset();
        int pulls=0; bool was=false;
        const float taps[]={0.f,1.f,1.f,0.75f,0.76f,1.f,1.f,0.78f,0.97f,1.f,0.74f,0.95f};
        for (float v:taps) { const bool now=t.Step(v); pulls+=now && !was; was=now; }
        CHECK(pulls==4);
        // A firm hold wobbling under recoil stays one pull.
        t.Reset();
        CHECK(t.Step(1.f));
        for (float v:{0.95f,0.88f,0.99f,0.85f,1.f,0.9f}) CHECK(t.Step(v));
        // A partial release must travel the reset distance back in again.
        CHECK(!t.Step(0.78f) && !t.Step(0.8f) && !t.Step(0.95f) && t.Step(0.99f));
        // Never pulled at or below the break.
        t.Reset();
        CHECK(!t.Step(0.65f) && !t.Step(0.5f));
    }
    bool holster_armed=false;
    CHECK(!vr::HolsterGrab(holster_armed,false,true,false,true));
    CHECK(!vr::HolsterGrab(holster_armed,true,true,false,true));
    CHECK(!vr::HolsterGrab(holster_armed,true,false,false,false) && holster_armed);
    CHECK(!vr::HolsterGrab(holster_armed,true,true,false,false) && holster_armed);
    CHECK(vr::HolsterGrab(holster_armed,true,true,false,true) && !holster_armed);
    CHECK(!vr::HolsterGrab(holster_armed,true,true,false,true));
    CHECK(!vr::HolsterGrab(holster_armed,true,false,true,true) && holster_armed);
    CHECK(!vr::HolsterGrab(holster_armed,true,true,true,true) && !holster_armed);
    CHECK(!vr::HolsterGrab(holster_armed,true,true,false,true));
    CHECK(!vr::HolsterGrab(holster_armed,true,false,false,true) && holster_armed);
    CHECK(vr::HolsterGrab(holster_armed,true,true,false,true));
    // Per-hand latches: the left hand draws; the right hand, squeezed the
    // whole time, cannot take the other holster once a weapon is active, and
    // needs a fresh squeeze after it is put away.
    bool hand_armed[2]={false,false};
    CHECK(!vr::HolsterGrab(hand_armed[0],true,false,false,false) && hand_armed[0]);
    CHECK(!vr::HolsterGrab(hand_armed[1],true,false,false,false) && hand_armed[1]);
    CHECK(vr::HolsterGrab(hand_armed[0],true,true,false,true) && !hand_armed[0]);
    CHECK(!vr::HolsterGrab(hand_armed[1],true,true,true,true) && !hand_armed[1]);
    CHECK(!vr::HolsterGrab(hand_armed[1],true,true,false,true));
    CHECK(!vr::HolsterGrab(hand_armed[1],true,false,false,true) && hand_armed[1]);
    CHECK(vr::HolsterGrab(hand_armed[1],true,true,false,true));
    vr::PistolInputGate gate;
    CHECK(!gate.Step(1,true,true) && !gate.Armed());
    CHECK(!gate.Step(1,true,false) && gate.Armed());
    CHECK(gate.Step(1,true,true));
    CHECK(!gate.Step(1,false,true) && !gate.Armed());
    CHECK(!gate.Step(1,true,true));
    CHECK(!gate.Step(1,true,false) && gate.Armed());
    // Many render frames may elapse between actor updates in the same epoch.
    CHECK(gate.Step(1,true,true)); CHECK(gate.Step(1,true,true));
    CHECK(!gate.Step(2,true,true) && !gate.Armed()); // menu/focus epoch changed
    CHECK(!gate.Step(2,true,false)); CHECK(gate.Step(2,true,true));
    Rounds live{1,2,3,4,5,6,7,8}; State state;
    state.Initialize(live); CHECK(state.CanFire() && state.Total(live)==8);
    CHECK(state.Detach(live)); CHECK(live==Rounds{8});
    CHECK(state.held==0 && state.magazines[0]==Rounds({1,2,3,4,5,6,7}));
    CHECK(state.CanFire()); live.pop_back(); state.AfterShot(live); state.Advance(0.2f,live,[]{});
    // Magazine out, last round fired: the action holds open.
    CHECK(!state.CanFire() && state.locked && state.Total(live)==7);
    state.Suspend(); CHECK(state.held==-1 && state.inserted==-1 && state.Total(live)==7);
    CHECK(state.Draw() && state.held==1);
    state.magazines[1]={9,10,11,12,13,14,15,16}; // eight consumed from reserve
    CHECK(state.Insert(live)); CHECK(!state.CanFire() && state.inserted==1);
    state.Rack(live); CHECK(state.CanFire());
    state.slide=State::travel; CHECK(!state.CanFire()); state.Suspend(); CHECK(state.CanFire());
    CHECK(state.Detach(live)); CHECK(live==Rounds{16});
    state.Stow(); CHECK(state.Draw() && state.held==0);
    CHECK(state.Insert(live)); CHECK(live==Rounds({1,2,3,4,5,6,7,16}));
    CHECK(state.Total(live)==15); // original 8 + reserve 8 - fired 1
    CHECK(!state.Insert(live)); CHECK(state.Detach(live)); CHECK(!state.Detach(live));
    CHECK(state.Total(live)==15); state.Suspend();
    // A discarded spare frees its pouch slot; the dropped body is cosmetic.
    // Its rounds leave the player's ammunition total and a replacement can
    // be drawn from that slot without reclaiming the old physics body.
    State dropped; Rounds dropped_live{1,2,3}; dropped.Initialize(dropped_live);
    CHECK(dropped.Draw() && dropped.held==1);
    dropped.magazines[1]={4,5};
    CHECK(dropped.Total(dropped_live)==5 && dropped.DiscardHeld());
    CHECK(dropped.held==-1 && dropped.next_pouch==1 && dropped.magazines[1].empty());
    CHECK(dropped.Total(dropped_live)==3 && dropped.Draw() && dropped.held==1);
    CHECK(dropped.magazines[1].empty());
    State returned; Rounds returned_live{1,2}; returned.Initialize(returned_live);
    CHECK(returned.Draw() && returned.held==1);
    returned.magazines[1]={3,4};
    returned.Stow(true);
    CHECK(returned.held==-1 && returned.next_pouch==1 && returned.Total(returned_live)==4);
    CHECK(returned.Draw() && returned.held==1 && returned.magazines[1]==Rounds({3,4}));
    // A magazine taken from the floor goes in the place not in the well; the
    // pouch spare there gives its rounds back, and none are made or lost.
    State floor; Rounds floor_live{1,2}; floor.Initialize(floor_live);
    floor.magazines[1]={3,4};
    Rounds picked{20,21,22}, displaced;
    CHECK(floor.TakeFromWorld(picked,displaced) && floor.held==1 && picked.empty());
    CHECK(floor.magazines[1]==Rounds({20,21,22}) && displaced==Rounds({3,4}));
    CHECK(floor.Total(floor_live)+displaced.size()==7);
    Rounds second{30};
    CHECK(!floor.TakeFromWorld(second,displaced) && second==Rounds{30} && displaced.size()==2);
    // Seated after the one in the well comes out (the chamber round stays).
    CHECK(floor.EjectInserted(floor_live)==0 && floor.magazines[0]==Rounds{1});
    CHECK(floor.Insert(floor_live) && floor_live==Rounds({20,21,22,2}));
    CHECK(floor.Total(floor_live)+displaced.size()==7);
    // With the well empty it takes the next pouch place.
    State open; Rounds open_live{}; open.Initialize(open_live);
    CHECK(open.Detach(open_live) && open.DiscardHeld() && open.inserted==-1);
    Rounds loose{5,6}, no_spare;
    CHECK(open.TakeFromWorld(loose,no_spare) && no_spare.empty() && open.held==open.next_pouch &&
        open.magazines[open.held]==Rounds({5,6}));
    State ejected; Rounds ejected_live{10,11,12}; ejected.Initialize(ejected_live);
    CHECK(ejected.Detach(ejected_live) && ejected.held==0);
    CHECK(ejected.DiscardHeld() && ejected.inserted==-1 && ejected_live==Rounds{12});
    CHECK(ejected.Draw() && ejected.held==0 && ejected.magazines[0].empty());
    State spare; Rounds spare_live{20,21}; spare.Initialize(spare_live);
    spare.magazines[1]={30,31};
    CHECK(spare.Detach(spare_live) && spare.DiscardHeld());
    CHECK(spare.next_pouch==1 && spare.Draw() && spare.held==1);
    CHECK(spare.magazines[1]==Rounds({30,31}));
    State repeated_drop; Rounds repeated_live{50}; repeated_drop.Initialize(repeated_live);
    for (int i=0;i<100;++i)
    {
        CHECK(repeated_drop.Draw() && repeated_drop.held==1);
        repeated_drop.magazines[1]={i+100};
        CHECK(repeated_drop.DiscardHeld() && repeated_drop.Total(repeated_live)==1);
    }
    // Repeated interruptions never duplicate, discard or silently merge rounds.
    for (int i=0;i<10000;++i)
    {
        CHECK(state.Draw()); const int identity=state.held;
        const auto rounds=state.magazines[identity];
        state.Suspend(); CHECK(state.magazines[identity]==rounds && state.Total(live)==15);
        CHECK(state.Draw()); CHECK(state.Insert(live));
        CHECK(state.Total(live)==15); CHECK(state.Detach(live)); state.Stow();
        CHECK(state.Total(live)==15);
    }
    // Tactical reload supports eight in the replacement magazine plus the
    // retained, individually identified chamber cartridge.
    State tactical; Rounds tactical_live{1,2,3,4,5,6,7,8}; tactical.Initialize(tactical_live);
    CHECK(tactical.Detach(tactical_live)); tactical.Stow(); CHECK(tactical.Draw());
    tactical.magazines[tactical.held]={9,10,11,12,13,14,15,16};
    CHECK(tactical.Insert(tactical_live)); CHECK(tactical_live.size()==9 && tactical_live.back()==8);
    CHECK(tactical.CanFire() && tactical.Total(tactical_live)==16);
    tactical_live.pop_back(); tactical.AfterShot(tactical_live); tactical.Advance(0.2f,tactical_live,[]{});
    CHECK(tactical_live.back()==16 && tactical.chamber && tactical.Total(tactical_live)==15);
    State empty; Rounds none; empty.Initialize(none); CHECK(!empty.CanFire());
    empty.Rack(none); CHECK(empty.locked && empty.slide==State::travel);
    CHECK(empty.Detach(none)); empty.Stow(); CHECK(empty.Draw());
    empty.magazines[empty.held]={42}; CHECK(empty.Insert(none)); CHECK(!empty.CanFire());
    empty.Rack(none); CHECK(empty.CanFire()); none.pop_back(); empty.AfterShot(none); empty.Advance(0.2f,none,[]{});
    CHECK(!empty.CanFire() && empty.locked && empty.Total(none)==0);
    State cycle; Rounds feed{1,2}; cycle.Initialize(feed);
    feed.pop_back(); cycle.AfterShot(feed);
    CHECK(!cycle.chamber && !cycle.CanFire() && cycle.cycling);
    int ejections=0;
    cycle.Advance(0.01f,feed,[&]{++ejections;});
    CHECK(cycle.slide>0 && !cycle.chamber && ejections==0);
    cycle.Advance(0.02f,feed,[&]{++ejections;});
    CHECK(ejections==1 && !cycle.chamber);
    cycle.Advance(0.1f,feed,[&]{++ejections;});
    CHECK(ejections==1 && cycle.CanFire() && cycle.slide==0 && cycle.Total(feed)==1);
    cycle.Advance(1.f,feed,[&]{++ejections;}); CHECK(ejections==1);
    // Removing the feed source during recoil cannot produce a chamber round.
    State interrupted; Rounds interrupted_rounds{11,12}; interrupted.Initialize(interrupted_rounds);
    interrupted_rounds.pop_back(); interrupted.AfterShot(interrupted_rounds);
    CHECK(interrupted.Detach(interrupted_rounds));
    CHECK(interrupted_rounds.empty() && interrupted.Total(interrupted_rounds)==1);
    int spent=0; interrupted.Advance(0.2f,interrupted_rounds,[&]{++spent;});
    CHECK(spent==1 && !interrupted.chamber && !interrupted.CanFire());
    CHECK(interrupted.Total(interrupted_rounds)==1);
    // The fixed-step path is independent of how render deltas are partitioned.
    for (int partition=1;partition<=64;++partition)
    {
        State partitioned; Rounds rounds{20,21}; partitioned.Initialize(rounds);
        rounds.pop_back(); partitioned.AfterShot(rounds); int emitted=0;
        for (int n=0;n<partition;++n)
            partitioned.Advance(0.2f/partition,rounds,[&]{++emitted;});
        CHECK(emitted==1 && partitioned.CanFire() && partitioned.Total(rounds)==1);
    }
    State stop; Rounds stop_rounds; stop.Initialize(stop_rounds);
    CHECK(stop.locked && stop.ReleaseSlide(stop_rounds));
    CHECK(!stop.ReleaseSlide(stop_rounds));
    int stop_ejections=0;
    stop.Advance(.2f,stop_rounds,[&]{++stop_ejections;});
    CHECK(!stop.locked && !stop.chamber && !stop.CanFire() && stop_ejections==0);
    stop.Rack(stop_rounds); CHECK(stop.locked);
    CHECK(stop.Detach(stop_rounds)); stop.Stow(); CHECK(stop.Draw());
    stop.magazines[stop.held]={90}; CHECK(stop.Insert(stop_rounds));
    CHECK(stop.locked && !stop.chamber && stop.ReleaseSlide(stop_rounds));
    stop.Advance(.2f,stop_rounds,[&]{++stop_ejections;});
    CHECK(stop.CanFire() && stop.Total(stop_rounds)==1 && stop_ejections==0);
    // Manual cycling ejects on rearward threshold crossing while still held,
    // does not repeat around the threshold, and feeds on the forward stroke.
    State manual; Rounds manual_rounds{101,102,103}; manual.Initialize(manual_rounds);
    int manual_ejections=0;
    manual.MoveManualSlide(.027f,manual_rounds,[&]{++manual_ejections; manual_rounds.pop_back();});
    CHECK(manual_ejections==0 && manual.chamber);
    manual.MoveManualSlide(.029f,manual_rounds,[&]{++manual_ejections; manual_rounds.pop_back();});
    CHECK(manual_ejections==1 && !manual.chamber && manual_rounds==Rounds({101,102}));
    manual.MoveManualSlide(.028f,manual_rounds,[&]{++manual_ejections; manual_rounds.pop_back();});
    manual.MoveManualSlide(.034f,manual_rounds,[&]{++manual_ejections; manual_rounds.pop_back();});
    CHECK(manual_ejections==1);
    manual.MoveManualSlide(State::travel,manual_rounds,[&]{++manual_ejections;});
    CHECK(manual_ejections==1 && !manual.locked);
    manual.MoveManualSlide(0.f,manual_rounds,[&]{++manual_ejections;});
    CHECK(manual.chamber && manual.CanFire() && manual.Total(manual_rounds)==2);
    manual.MoveManualSlide(State::travel,manual_rounds,[&]{++manual_ejections; manual_rounds.pop_back();});
    CHECK(manual_ejections==2 && !manual.chamber);
    manual.ReleaseManualSlide(manual_rounds); manual.Advance(.2f,manual_rounds,[&]{++manual_ejections;});
    CHECK(manual.chamber && manual.CanFire() && !manual.manual_ejection_done);
    manual.MoveManualSlide(State::travel,manual_rounds,[&]{++manual_ejections; manual_rounds.pop_back();});
    CHECK(manual_ejections==3 && manual_rounds.empty());
    CHECK(manual.locked && manual.slide==State::travel);
    CHECK(manual.ReleaseSlide(manual_rounds)); manual.Advance(.2f,manual_rounds,[&]{++manual_ejections;});
    CHECK(!manual.chamber && !manual.locked && manual.slide==0);
    // Removing the last live cartridge before a full rack engages the stop.
    manual_rounds.clear(); manual.chamber=false; manual.slide=0; manual.locked=false;
    manual.manual_ejection_done=false;
    manual.MoveManualSlide(State::travel,manual_rounds,[&]{++manual_ejections;});
    CHECK(manual.locked && manual.slide==State::travel);
    CHECK(manual.ReleaseSlide(manual_rounds));
    manual.Advance(.2f,manual_rounds,[&]{++manual_ejections;});
    CHECK(!manual.locked && !manual.chamber && manual.slide==0);
    // Empty pistol, locked slide, fresh eight-round magazine: a physical
    // over-pull and release must chamber one without ejecting or losing ammo.
    State first_rack; Rounds first_live; first_rack.Initialize(first_live);
    CHECK(first_rack.locked && first_rack.Detach(first_live));
    first_rack.Stow(); CHECK(first_rack.Draw() && first_rack.held>=0);
    first_rack.magazines[first_rack.held]={1,2,3,4,5,6,7,8};
    CHECK(first_rack.Insert(first_live) && first_live.size()==8 && !first_rack.chamber);
    int first_ejections=0;
    first_rack.MoveManualSlide(State::travel+0.002f,first_live,[&]{++first_ejections;});
    first_rack.ReleaseManualSlide(first_live);
    CHECK(first_rack.locked && first_ejections==0);
    first_rack.MoveManualSlide(State::travel+0.006f,first_live,[&]{++first_ejections;});
    first_rack.ReleaseManualSlide(first_live);
    first_rack.Advance(.2f,first_live,[&]{++first_ejections;});
    CHECK(first_rack.CanFire() && first_rack.chamber && first_live.size()==8 && first_ejections==0);
    // The magazine dropped with a round chambered: firing that last round
    // locks the action, with no magazine in.
    {
        State last; Rounds last_live{1,2,3}; last.Initialize(last_live);
        const int out=last.EjectInserted(last_live);
        last.magazines[out].clear();
        CHECK(last_live.size()==1 && last.chamber && last.inserted<0);
        last_live.pop_back();
        last.AfterShot(last_live);
        last.Advance(.5f,last_live,[]{});
        CHECK(last.locked && last.slide==State::travel && !last.chamber);
        // Released by hand it closes on nothing.
        CHECK(last.ReleaseSlide(last_live));
        last.Advance(.5f,last_live,[]{});
        CHECK(!last.locked && !last.chamber && last.slide==0);
        // With rounds still in the gun it does not lock.
        State more; Rounds more_live{1,2,3}; more.Initialize(more_live);
        more_live.pop_back();
        more.AfterShot(more_live);
        more.Advance(.5f,more_live,[]{});
        CHECK(!more.locked && more.chamber);
    }
    // Reproduce the headset sequence: last-round lock, remove empty mag,
    // close and cycle the slide with no mag, then insert eight and rack.
    State dry; Rounds dry_live; dry.Initialize(dry_live);
    CHECK(dry.locked && dry.Detach(dry_live));
    dry.Stow();
    int phantom_ejections=0;
    dry.MoveManualSlide(State::travel+0.006f,dry_live,[&]{++phantom_ejections;});
    dry.ReleaseManualSlide(dry_live);
    dry.Advance(.2f,dry_live,[&]{++phantom_ejections;});
    CHECK(!dry.locked && !dry.chamber && dry_live.empty());
    CHECK(dry.Draw());
    dry.magazines[dry.held]={1,2,3,4,5,6,7,8};
    CHECK(dry.Insert(dry_live) && dry_live.size()==8 && !dry.chamber);
    dry.MoveManualSlide(State::travel,dry_live,[&]{++phantom_ejections; dry_live.pop_back();});
    dry.ReleaseManualSlide(dry_live);
    dry.Advance(.2f,dry_live,[&]{++phantom_ejections;});
    CHECK(phantom_ejections==0 && dry_live.size()==8 && dry.chamber && dry.CanFire());
    // The same mechanism must honor a rifle profile's longer charging stroke.
    vr::MagazineMechanism<Rounds> rifle;
    rifle.travel_limit=.055f;
    rifle.locks_on_empty=false;
    Rounds rifle_live{1,2,3}; rifle.Initialize(rifle_live);
    int rifle_ejections=0;
    rifle.MoveManualSlide(.030f,rifle_live,[&]{++rifle_ejections; rifle_live.pop_back();});
    CHECK(rifle_ejections==0 && rifle.chamber);
    rifle.MoveManualSlide(.045f,rifle_live,[&]{++rifle_ejections; rifle_live.pop_back();});
    CHECK(rifle_ejections==1 && rifle_live.size()==2 && !rifle.chamber);
    rifle.MoveManualSlide(0.f,rifle_live,[&]{++rifle_ejections;});
    CHECK(rifle.chamber && rifle.CanFire() && rifle.Total(rifle_live)==2);
    CHECK(rifle.Detach(rifle_live) && rifle_live.size()==1);
    CHECK(rifle.Total(rifle_live)==2 && rifle.Insert(rifle_live));
    CHECK(rifle.Total(rifle_live)==2);
    State burst; Rounds burst_live{1,2,3}; burst.Initialize(burst_live);
    burst_live.pop_back(); burst.AfterShot(burst_live);
    burst_live.pop_back(); burst.AfterShot(burst_live);
    int burst_cases=0;
    burst.Advance(.2f,burst_live,[&]{++burst_cases;});
    CHECK(burst_cases==2 && burst.Total(burst_live)==1);
    rifle_live.clear(); rifle.chamber=false;
    rifle.Rack(rifle_live);
    CHECK(!rifle.locked && rifle.slide==0);
    // A long rifle bolt cycles within the shot interval it is given: with 12 cm
    // of travel and 60 ms, it is home and chambered after 70 ms, where the
    // fixed speeds would still have it moving.
    State bolt; Rounds bolt_live{1,2,3}; bolt.travel_limit=0.12f; bolt.locks_on_empty=false;
    bolt.cycle_time=0.06f; bolt.Initialize(bolt_live);
    bolt_live.pop_back(); bolt.AfterShot(bolt_live);
    bolt.Advance(0.07f,bolt_live,[]{});
    CHECK(!bolt.cycling && bolt.chamber && bolt.CanFire());
    State slow; Rounds slow_live{1,2,3}; slow.travel_limit=0.12f; slow.locks_on_empty=false;
    slow.Initialize(slow_live); slow_live.pop_back(); slow.AfterShot(slow_live);
    slow.Advance(0.07f,slow_live,[]{});
    CHECK(slow.cycling && !slow.CanFire());
    // Shot dry, empty magazine out, fresh one in, charging handle pulled:
    // nothing is ejected (the chamber is empty) and the stroke chambers a
    // round. A short pull (not past extraction) changes nothing.
    State redo; Rounds redo_live{1}; redo.travel_limit=0.1f; redo.locks_on_empty=false; redo.Initialize(redo_live);
    redo_live.pop_back(); redo.AfterShot(redo_live); redo.Advance(0.5f,redo_live,[]{});
    CHECK(!redo.chamber && !redo.cycling);
    CHECK(redo.Detach(redo_live)); redo.magazines[redo.held].clear();
    redo.magazines[redo.held]={5,6,7}; CHECK(redo.Insert(redo_live) && !redo.chamber);
    int redo_ejected=0;
    redo.MoveManualSlide(0.f,redo_live,[&]{++redo_ejected;});
    CHECK(!redo.chamber);                                   // first frame of the pull
    redo.MoveManualSlide(0.03f,redo_live,[&]{++redo_ejected;});
    redo.ReleaseManualSlide(redo_live); redo.Advance(0.5f,redo_live,[]{});
    CHECK(!redo.chamber && redo_ejected==0);                 // short pull: still empty
    redo.MoveManualSlide(0.f,redo_live,[&]{++redo_ejected;});
    redo.MoveManualSlide(0.1f,redo_live,[&]{++redo_ejected;});
    redo.MoveManualSlide(0.f,redo_live,[&]{++redo_ejected;});
    CHECK(redo.chamber && redo_ejected==0 && redo_live.size()==3);
    // Pulled again with a round chambered: that round is ejected.
    redo.MoveManualSlide(0.1f,redo_live,[&]{++redo_ejected; redo_live.pop_back();});
    CHECK(redo_ejected==1);
    // Tactical reload: a magazine in the hand, the release drops the one in
    // the well (rounds out, chamber kept); the held one then seats.
    State tactical_drop; Rounds drop_live{1,2,3,4}; tactical_drop.Initialize(drop_live);
    // Nothing held: the release still drops it, and it can be taken loose.
    {
        State plain; Rounds plain_live{1,2,3,4}; plain.Initialize(plain_live);
        const int out=plain.EjectInserted(plain_live);
        CHECK(out==0 && plain.inserted<0 && plain.held<0 && plain_live.size()==1 && plain.magazines[0].size()==3);
        CHECK(plain.EjectInserted(plain_live)==-1);  // the well is empty now
        CHECK(plain.TakeLoose(out) && plain.held==out && !plain.TakeLoose(1));
    }
    tactical_drop.magazines[1]={7,8}; tactical_drop.held=1;
    const int dropped_index=tactical_drop.EjectInserted(drop_live);
    CHECK(dropped_index==0 && tactical_drop.inserted<0 && drop_live.size()==1 && drop_live.back()==4);
    CHECK(tactical_drop.magazines[0].size()==3 && tactical_drop.chamber);
    CHECK(tactical_drop.Insert(drop_live) && drop_live.size()==3 && drop_live.back()==4);
    auto finish=[](State& s, const Rounds& r)
    {
        for (int i=0;i<500 && s.cycling;++i) s.Advance(1.f/120.f,r,[]{});
        return !s.cycling;
    };
    // HK notch: pulled fully back (the chambered round comes out), hooked
    // into the notch it stays open; a slap releases it and it chambers.
    {
        Rounds rounds{1,2,3}; State m; m.locks_on_empty=false; m.travel_limit=0.08f; m.Initialize(rounds);
        CHECK(!m.LockOpen());                        // not from closed
        m.MoveManualSlide(0.04f,rounds,[&]{});
        CHECK(!m.LockOpen());                        // nor half way
        int out=0;
        m.MoveManualSlide(0.08f,rounds,[&]{++out; rounds.pop_back();});
        CHECK(out==1 && !m.chamber && m.LockOpen() && m.locked && !m.CanFire());
        CHECK(m.ReleaseSlide(rounds) && finish(m,rounds) && m.chamber && m.CanFire() && rounds.size()==2);
    }
    // Empty pistol, magazine out: the slide comes off the stop without
    // ejecting anything; a loaded magazine in, a full rack chambers a round.
    {
        Rounds rounds; State m; m.Initialize(rounds);
        CHECK(m.locked && !m.chamber && m.Detach(rounds) && m.inserted<0);
        int ejections=0;
        m.MoveManualSlide(m.travel_limit+0.005f,rounds,[&]{++ejections;});
        m.ReleaseManualSlide(rounds);
        CHECK(finish(m,rounds) && !m.chamber && ejections==0);
        m.magazines[m.held]={1,2,3,4,5,6,7,8};
        CHECK(m.Insert(rounds) && rounds.size()==8 && !m.chamber);
        m.MoveManualSlide(m.travel_limit,rounds,[&]{++ejections;});
        CHECK(ejections==0);
        m.ReleaseManualSlide(rounds);
        CHECK(finish(m,rounds) && m.chamber && rounds.size()==8 && ejections==0);
    }
    // A magazine seated while the slide is already returning does not feed.
    {
        Rounds rounds; State m; m.Initialize(rounds); m.Detach(rounds);
        m.MoveManualSlide(m.travel_limit+0.005f,rounds,[]{});
        m.ReleaseManualSlide(rounds);
        m.Advance(0.01f,rounds,[]{});
        m.magazines[m.held]={1,2,3};
        CHECK(m.Insert(rounds));
        CHECK(finish(m,rounds) && !m.chamber);
        int unexpected=0;
        m.MoveManualSlide(m.travel_limit,rounds,[&]{++unexpected;});
        m.ReleaseManualSlide(rounds);
        CHECK(finish(m,rounds) && m.chamber && unexpected==0);
    }
    // A shot ejects one spent case; a rack ejects the chambered live round
    // once, however far the slide is held back.
    {
        Rounds rounds={1,2}; State m; m.Initialize(rounds);
        rounds.pop_back();
        int spent=0;
        m.AfterShot(rounds);
        for (int i=0;i<500 && m.cycling;++i) m.Advance(1.f/120.f,rounds,[&]{++spent;});
        CHECK(spent==1 && m.chamber);
        int live=0;
        m.MoveManualSlide(m.travel_limit,rounds,[&]{++live; rounds.pop_back();});
        m.MoveManualSlide(m.travel_limit,rounds,[&]{++live;});
        CHECK(live==1 && !m.chamber && rounds.empty());
        m.ReleaseManualSlide(rounds);
        CHECK(finish(m,rounds) && !m.chamber);
    }
    // Press-check with the magazine out: a short pull keeps the chambered
    // round, which a full rack then ejects.
    {
        Rounds rounds{1,2,3}; State m; m.Initialize(rounds);
        CHECK(m.Detach(rounds) && rounds==Rounds{3} && m.chamber);
        int ejected=0;
        m.MoveManualSlide(0.01f,rounds,[&]{++ejected;});
        m.ReleaseManualSlide(rounds);
        CHECK(finish(m,rounds) && m.chamber && m.CanFire() && ejected==0 && rounds.size()==1);
        m.MoveManualSlide(m.travel_limit,rounds,[&]{++ejected; rounds.pop_back();});
        CHECK(ejected==1 && !m.chamber && rounds.empty());
    }
    // Bolt-action: the shot leaves the case and the bolt home; the gun cannot
    // fire until a full stroke throws the case out and chambers the next.
    {
        Rounds rounds{1,2,3}; State m; m.travel_limit=0.117f; m.locks_on_empty=false; m.Initialize(rounds);
        rounds.pop_back();  // fired
        m.AfterShotManual();
        CHECK(!m.CanFire() && !m.cycling && m.slide==0.f);
        int live=0, cases=0;
        m.MoveManualSlide(0.05f,rounds,[&]{++live;},[&]{++cases;});
        CHECK(cases==0 && !m.CanFire());
        m.MoveManualSlide(0.117f,rounds,[&]{++live;},[&]{++cases;});
        CHECK(cases==1 && live==0 && rounds.size()==2);
        // Let go open: it stays open and cannot fire.
        CHECK(m.slide>0.1f && !m.CanFire());
        m.MoveManualSlide(0.f,rounds,[&]{++live;},[&]{++cases;});
        CHECK(m.chamber && m.CanFire() && cases==1 && live==0 && rounds.size()==2);
        // Worked again without firing: the live round comes out.
        m.MoveManualSlide(0.117f,rounds,[&]{++live; rounds.pop_back();},[&]{++cases;});
        m.MoveManualSlide(0.f,rounds,[&]{++live;},[&]{++cases;});
        CHECK(live==1 && cases==1 && m.chamber && rounds.size()==1);
    }
    puts("PASS: chamber, magazine identity, interrupted reloads, last round, empty lock, 10000 conservation cycles");
}
