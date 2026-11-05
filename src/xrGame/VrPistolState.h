#pragma once
#include <algorithm>

namespace vr
{
// How far a trigger or grip is pressed (0..1) to count as pulled or closed,
// and how far a held grip must open to let go.
constexpr float pull_threshold=0.65f;
constexpr float grip_release_threshold=0.35f;
// A trigger pulled: firing, using, working a part. No hysteresis: a
// trigger is pressed and released quickly.
inline bool TriggerPulled(float value) { return value>pull_threshold; }
// A weapon's trigger, as a real one: it breaks at pull_threshold and resets
// as the finger lets it forward, not only at rest. Tapping quickly rarely
// lets an analog trigger back past the break (it bounces between about 1.0
// and 0.75), which read as one long pull: full auto. Here moving back
// reset_travel from the deepest point releases it, and moving forward
// reset_travel from the shallowest (or across the break from below) pulls it
// again, so each tap is a pull.
struct FireTrigger
{
    static constexpr float reset_travel=0.2f;
    bool pulled=false;
    float extreme=0.f;  // deepest while pulled, shallowest while released
    void Reset() { pulled=false; extreme=0.f; }
    // Released, but a trigger still held is not a pull: it fires again once
    // let back past the break (or by the reset travel), as after a shot.
    void Block() { pulled=false; extreme=1.f; }
    bool Step(float value)
    {
        if (pulled)
        {
            extreme=std::max(extreme,value);
            if (value<=pull_threshold || extreme-value>=reset_travel) { pulled=false; extreme=value; }
        }
        else
        {
            extreme=std::min(extreme,value);
            if (value>pull_threshold && (extreme<=pull_threshold || value-extreme>=reset_travel))
            { pulled=true; extreme=value; }
        }
        return pulled;
    }
};
// Holding a weapon or magazine follows the physical grip. Release needs a
// clearly opened hand, so a relaxed natural hold cannot flicker the grab.
inline bool GripHeld(float squeeze, bool held)
{
    return squeeze>(held ? grip_release_threshold : pull_threshold);
}
// One held grip may claim one holstered weapon. Crossing another holster
// while still squeezed cannot switch slots; releasing re-arms the next draw.
inline bool HolsterGrab(bool& armed, bool valid, bool squeezed,
    bool weapon_active, bool candidate_in_reach)
{
    if (!valid) { armed=false; return false; }
    if (!squeezed) { armed=true; return false; }
    if (weapon_active) { armed=false; return false; }
    if (!armed || !candidate_in_reach) return false;
    armed=false;
    return true;
}
// A gameplay-input epoch changes on menu/focus/tracking interruption. Actor
// updates can legitimately skip render frames without invalidating input.
class PistolInputGate
{
    unsigned last_epoch=0;
    bool armed=false;
public:
    void Reset() { last_epoch=0; armed=false; }
    bool Armed() const { return armed; }
    bool Step(unsigned epoch, bool valid, bool held)
    {
        if (!valid || (last_epoch && epoch!=last_epoch)) armed=false;
        last_epoch=epoch;
        if (!valid) return false;
        if (!armed) { armed=!held; return false; }
        return true;
    }
};

// The engine's live cartridge vector contains magazine rounds followed by the
// chamber cartridge at back(). Detached magazines own their actual cartridges.
// This is transient interaction state. Saves use only ordinary weapon and
// ammo entities.
template<class Rounds> struct MagazineMechanism
{
    Rounds magazines[2];
    bool initialized = false, chamber = false, locked = false;
    int inserted = 0, held = -1, next_pouch = 1;
    float slide = 0;
    bool cycling = false, returning = false, eject_pending = false;
    // A round can only feed if a loaded magazine was present when the
    // forward stroke began, never because one arrived during that stroke.
    bool feed_on_return = false;
    unsigned spent_pending = 0;
    bool manual_ejection_done = false;
    bool manual_unlock_ready = false;
    float fixed_time = 0;
    static constexpr float travel = 0.035f;
    float travel_limit = travel;
    bool locks_on_empty = true;
    // Seconds for a full automatic cycle (back and home); 0 keeps the
    // fixed 1.4 m/s back, 0.7 m/s home.
    float cycle_time = 0;

    void Initialize(const Rounds& live)
    {
        if (initialized) return;
        initialized = true; chamber = !live.empty(); locked = locks_on_empty && live.empty();
        inserted = 0; held = -1; next_pouch = 1; slide=locked ? travel_limit : 0;
    }
    bool Detach(Rounds& live)
    {
        if (inserted < 0 || held >= 0) return false;
        const auto end = live.end() - (chamber && !live.empty() ? 1 : 0);
        magazines[inserted].assign(live.begin(), end);
        live.erase(live.begin(), end);
        held = inserted; inserted = -1; return true;
    }
    // The release: the magazine in the well comes out, whether or not another
    // is already in the hand (a tactical reload). Its rounds move to
    // magazines[index] for the caller (the chamber stays); returns that
    // index, or -1 with none in the well.
    int EjectInserted(Rounds& live)
    {
        if (inserted < 0) return -1;
        const int index = inserted;
        const auto end = live.end() - (chamber && !live.empty() ? 1 : 0);
        magazines[index].assign(live.begin(), end);
        live.erase(live.begin(), end);
        inserted = -1;
        return index;
    }
    bool Insert(Rounds& live)
    {
        if (inserted >= 0 || held < 0) return false;
        live.insert(live.begin(), magazines[held].begin(), magazines[held].end());
        magazines[held].clear(); inserted = held; held = -1; return true;
    }
    void Stow(bool return_to_same_pouch=false)
    {
        if (held >= 0) { next_pouch = return_to_same_pouch ? held : 1-held; held = -1; }
    }
    // A loose magazine (one released, still in reach) taken into the hand.
    bool TakeLoose(int index)
    {
        if (held >= 0 || index < 0 || index > 1 || index == inserted) return false;
        held = index; return true;
    }
    // A magazine picked up from the world (VrFloorMagazine) into the hand, in
    // the place not in the well. Its rounds move in; the pouch magazine it
    // replaces gives its own to `displaced` (back to reserve for the caller).
    bool TakeFromWorld(Rounds& rounds, Rounds& displaced)
    {
        if (held >= 0) return false;
        const int index = inserted >= 0 ? 1 - inserted : next_pouch;
        displaced.insert(displaced.end(), magazines[index].begin(), magazines[index].end());
        magazines[index].assign(rounds.begin(), rounds.end());
        rounds.clear();
        held = index; return true;
    }
    bool DiscardHeld()
    {
        if (held < 0) return false;
        const int dropped=held;
        magazines[dropped].clear();
        const int other=1-dropped;
        next_pouch = other!=inserted && !magazines[other].empty() ? other : dropped;
        held = -1;
        return true;
    }
    bool Draw()
    {
        if (held >= 0) return false;
        int index = next_pouch;
        if (index == inserted) index = 1 - index;
        held = index; return true;
    }
    bool CanFire() const { return initialized && !cycling && chamber && slide < 0.002f; }
    void AfterShot(const Rounds&)
    {
        // FireTrace has spent the chamber cartridge. Feeding is deferred until
        // the mechanism has completed its return, never inferred at shot time.
        chamber=false; locked=false; cycling=true; returning=false;
        feed_on_return=false;
        eject_pending=true; ++spent_pending; fixed_time=0; slide=0;
    }
    // A manually worked action: the fired case stays in the chamber and the
    // bolt stays home until the hand's stroke extracts it.
    void AfterShotManual()
    {
        chamber=false; locked=false; cycling=false; returning=false;
        feed_on_return=false; eject_pending=false; ++spent_pending; fixed_time=0; slide=0;
        manual_ejection_done=false;
    }
    template<class Eject> void Advance(float dt, const Rounds& live, Eject eject)
    {
        if (!cycling || !(dt>0)) return;
        fixed_time += std::min(dt,0.25f);
        constexpr float step=1.f/240.f;
        while (cycling && fixed_time>=step)
        {
            fixed_time-=step;
            if (!returning)
            {
                slide=std::min(travel_limit,slide+step*(cycle_time>0 ? travel_limit*3.f/cycle_time : 1.4f));
                if (eject_pending && slide>=travel_limit*0.8f)
                {
                    eject_pending=false;
                    while (spent_pending) { --spent_pending; eject(); }
                }
                if (slide>=travel_limit)
                {
                    returning=true;
                    // Nothing left to feed, magazine in or not.
                    locked=locks_on_empty && live.empty();
                    feed_on_return=inserted>=0 && !live.empty();
                    if (locked) { cycling=false; fixed_time=0; }
                }
            }
            else
            {
                slide=std::max(0.f,slide-step*(cycle_time>0 ? travel_limit*1.5f/cycle_time : 0.7f));
                if (slide==0)
                {
                    // A short pull (press-check) keeps the chambered round
                    // even with no magazine in the well.
                    chamber=feed_on_return && (inserted>=0 || chamber) && !live.empty();
                    feed_on_return=false;
                    manual_ejection_done=false;
                    cycling=false; fixed_time=0;
                }
            }
        }
    }
    bool ReleaseSlide(const Rounds& live)
    {
        if (!locked || cycling) return false;
        locked=false; chamber=false; cycling=true; returning=true;
        feed_on_return=inserted>=0 && !live.empty();
        eject_pending=false; spent_pending=0; manual_unlock_ready=false; fixed_time=0; return true;
    }
    // Caller ejects the back cartridge before this transition when chambered.
    void Rack(const Rounds& live)
    {
        chamber = inserted >= 0 && !live.empty();
        locked = locks_on_empty && inserted >= 0 && live.empty();
        slide = locked ? travel_limit : 0;
    }
    // Apply manual slide movement as it happens, not when the hand releases.
    // Crossing the rearward extraction point removes the chambered round once;
    // the forward stroke feeds only when the slide reaches battery.
    template<class Eject> void MoveManualSlide(float position, const Rounds& live, Eject eject)
    {
        MoveManualSlide(position,live,eject,[](){});
    }
    // `spent` throws out each fired case left by AfterShotManual.
    template<class Eject, class Spent> void MoveManualSlide(float position, const Rounds& live, Eject eject, Spent spent)
    {
        if (!initialized || cycling) return;
        if (locked)
        {
            // The stop holds the slide at full travel. Pulling it a few mm
            // farther disengages the stop; release then feeds from a loaded
            // magazine without extracting a nonexistent chamber cartridge.
            if (position>=travel_limit+0.004f) manual_unlock_ready=true;
            return;
        }
        const float previous=slide;
        slide=std::max(0.f,std::min(travel_limit,position));
        const float extraction=travel_limit*0.8f;
        if (!manual_ejection_done && previous<extraction && slide>=extraction)
        {
            if (chamber && !live.empty()) eject();
            while (spent_pending) { --spent_pending; spent(); }
            chamber=false;
            manual_ejection_done=true;
        }
        if (locks_on_empty && slide>=travel_limit && inserted>=0 && live.empty())
        {
            locked=true;
            cycling=false;
        }
        // Back home after a full stroke feeds the next round. A stroke that
        // never reached the extraction point (or the first frames of a pull,
        // before the slide has moved) changes nothing.
        if (slide<=0.002f)
        {
            if (!locked && manual_ejection_done) chamber=inserted>=0 && !live.empty();
            manual_ejection_done=false;
        }
    }
    // The hand lets go of a bolt it has hooked back (an HK's charging
    // handle in its notch): it stays open until released (ReleaseSlide, a
    // slap or a short pull). Only from fully back.
    bool LockOpen()
    {
        if (!initialized || cycling || slide < travel_limit*0.9f) return false;
        locked = true; slide = travel_limit; manual_unlock_ready = false;
        return true;
    }
    void ReleaseManualSlide(const Rounds& live)
    {
        if (locked)
        {
            if (manual_unlock_ready && (inserted<0 || !live.empty())) ReleaseSlide(live);
            manual_unlock_ready=false;
            return;
        }
        if (slide<=0.f || cycling) return;
        // Released after a full stroke the slide feeds; after a short one it
        // returns without touching the chamber.
        feed_on_return=manual_ejection_done ? inserted>=0 && !live.empty() : chamber;
        cycling=true; returning=true; eject_pending=false; spent_pending=0; fixed_time=0;
    }
    void Suspend() { Stow(); if (!cycling) slide = locked ? travel_limit : 0; }
    unsigned Total(const Rounds& live) const
    {
        return unsigned(live.size() + magazines[0].size() + magazines[1].size());
    }
};
template<class Rounds> using PistolState = MagazineMechanism<Rounds>;
}
