#pragma once
#include <cmath>
#include "VrVec.h"

namespace vr
{
// Moving parts of a weapon a hand works directly: a top cover on its hinge,
// a slider. Engine-free (see tests/vr/mechanism_tests.cpp); positions are in
// the weapon model's space, angles in radians and travel in metres.
//
// A part has one degree of freedom, its value: the angle about its axis
// through its pivot (hinge) or the travel along its axis (slider), between
// its limits. A hand closing within reach of its grab point takes it and
// keeps the offset it took it with; the part follows the hand along its own
// path only, never faster than its speed limit, so a fast hand or a tracking
// jump cannot snap it. A hand that ends up too far from where the part lets
// it be (the part reached a limit, or the hand pulled away) lets go, as does
// one that loses tracking for a moment; nothing is stretched or teleported.
// Unit length; along x when there is no direction (an axis must have one).
inline V3 PartNormal(const V3& a)
{ const float l=Length(a); return l>1e-6f ? a*(1.f/l) : V3{1.f,0.f,0.f}; }

enum PartKind { PartHinge, PartSlider };

struct PartDef
{
    PartKind kind=PartHinge;
    V3 pivot{}, axis{1.f,0.f,0.f};
    float min=0.f, max=1.f;
    // Open once the value reaches open_at, closed once it is back at or
    // below closed_at; between the two each keeps its last state.
    float open_at=0.8f, closed_at=0.05f;
    // Where a hand takes it, with the part at its minimum.
    V3 grab{};
    float grab_radius=0.06f, release_distance=0.06f;
    float max_speed=8.f;  // rad/s or m/s
    // Free, it falls back to its minimum from below open_at (a cover that
    // was not lifted all the way drops shut); held open it stays open.
    bool falls_closed=true;
    float lost_grace=0.2f; // seconds of lost tracking before letting go
};

struct PartState
{
    float value=0.f;
    int hand=-1;       // the hand holding it, or -1
    float offset=0.f;  // hand's angle or travel minus the value when taken
    float slack=0.f;   // how far the hand was from the grab point when it took it
    bool open=false, closed=true;
    float lost=0.f;
};

enum PartEvent : unsigned
{
    PartGrabbed=1u, PartReleased=2u, PartSlipped=4u, PartOpened=8u, PartClosed=16u
};

// Rotates p about the part's axis through its pivot by angle (right-handed),
// or moves it along the axis by travel.
inline V3 PartMove(const PartDef& def, float value, const V3& p)
{
    const V3 a=PartNormal(def.axis);
    if (def.kind==PartSlider) return p+a*value;
    const V3 v=p-def.pivot;
    const float c=std::cos(value), s=std::sin(value);
    // Rodrigues: v c + (a x v) s + a (a.v)(1-c)
    V3 r=v*c;
    r=r+Cross(a,v)*s;
    r=r+a*(Dot(a,v)*(1.f-c));
    return def.pivot+r;
}
// The 3x3 rotation PartMove applies to directions (rows: images of x, y, z).
inline void PartRotation(const PartDef& def, float value, float m[3][3])
{
    const V3 basis[3]={{1.f,0.f,0.f},{0.f,1.f,0.f},{0.f,0.f,1.f}};
    PartDef origin=def; origin.pivot={};
    for (int r=0;r<3;++r)
    {
        const V3 moved=def.kind==PartSlider ? basis[r] : PartMove(origin,value,basis[r]);
        m[r][0]=moved.x; m[r][1]=moved.y; m[r][2]=moved.z;
    }
}

// The hand's own coordinate on the part's path: its angle about the axis
// (measured in the plane square to it) or its travel along the axis.
inline float PartCoordinate(const PartDef& def, const V3& hand)
{
    const V3 a=PartNormal(def.axis);
    const V3 d=hand-def.pivot;
    if (def.kind==PartSlider) return Dot(d,a);
    // A fixed basis square to the axis, right-handed with it.
    const V3 helper=std::fabs(a.x)<0.9f ? V3{1.f,0.f,0.f} : V3{0.f,1.f,0.f};
    const V3 u=PartNormal(Cross(helper,a));
    const V3 v=Cross(a,u);
    return std::atan2(Dot(d,v),Dot(d,u));
}
inline float PartWrap(float angle)
{
    const float pi=3.14159265f;
    while (angle>pi) angle-=2.f*pi;
    while (angle<-pi) angle+=2.f*pi;
    return angle;
}
inline V3 PartAnchor(const PartDef& def, const PartState& state)
{
    return PartMove(def,state.value,def.grab);
}

struct PartInput
{
    bool tracked=false, grip=false, grip_edge=false;
    V3 hand{};   // the hand in weapon model space
    float dt=0.f;
    bool allowed=true; // the reload rules let it move now
};

// One hand's update of one part; returns PartEvent bits.
inline unsigned StepPart(const PartDef& def, PartState& state, int hand, const PartInput& in)
{
    unsigned events=0;
    if (state.hand==hand)
    {
        if (!in.tracked)
        {
            state.lost+=in.dt;
            if (state.lost>def.lost_grace) { state.hand=-1; events|=PartReleased|PartSlipped; }
        }
        else if (!in.grip) { state.hand=-1; events|=PartReleased; }
        else
        {
            state.lost=0.f;
            float coordinate=PartCoordinate(def,in.hand);
            float target=coordinate-state.offset;
            if (def.kind==PartHinge) target=state.value+PartWrap(target-state.value);
            if (target<def.min) target=def.min;
            if (target>def.max) target=def.max;
            if (in.allowed)
            {
                const float step=def.max_speed*in.dt;
                const float delta=target-state.value;
                state.value+=delta>step ? step : (delta<-step ? -step : delta);
            }
            // Pulled away from where the part can be: let go. The distance
            // it was taken from counts as held, not as pulled away.
            if (Length(PartAnchor(def,state)-in.hand)>state.slack+def.release_distance)
            { state.hand=-1; events|=PartReleased|PartSlipped; }
        }
    }
    else if (state.hand<0 && in.tracked && in.grip_edge &&
        Length(PartAnchor(def,state)-in.hand)<=def.grab_radius)
    {
        state.hand=hand;
        state.lost=0.f;
        float coordinate=PartCoordinate(def,in.hand);
        state.offset=def.kind==PartHinge ? PartWrap(coordinate-state.value) : coordinate-state.value;
        state.slack=Length(PartAnchor(def,state)-in.hand);
        events|=PartGrabbed;
    }
    return events;
}

// Once per frame after the hands: a free part settles, and the open and
// closed states follow the value (with hysteresis between them).
inline unsigned SettlePart(const PartDef& def, PartState& state, float dt)
{
    unsigned events=0;
    if (state.hand<0 && def.falls_closed && state.value<def.open_at && state.value>def.min)
    {
        const float step=def.max_speed*dt;
        state.value=state.value-step<def.min ? def.min : state.value-step;
    }
    if (!state.open && state.value>=def.open_at) { state.open=true; events|=PartOpened; }
    else if (state.open && state.value<=def.closed_at) state.open=false;
    if (!state.closed && state.value<=def.closed_at) { state.closed=true; events|=PartClosed; }
    // Closed means shut: anything past the closed point by 2% of the travel
    // is no longer closed (rules gating fire and chambering read this).
    else if (state.closed && state.value>def.closed_at+0.02f*(def.max-def.min)) state.closed=false;
    return events;
}

// The axis of a rotation from how points moved under it: each point moves
// square to the axis, so the cross product of two displacements lies along
// it. Of the pairs given, the one least parallel is used (points on one line
// through the pivot all move the same way). Zero when nothing turned.
inline V3 AxisFromMotion(const V3* before, const V3* after, int count)
{
    V3 best{};
    float best_length=0.f;
    for (int a=0;a<count;++a)
        for (int b=a+1;b<count;++b)
        {
            const V3 axis=Cross(after[a]-before[a],after[b]-before[b]);
            const float length=Length(axis);
            if (length>best_length) { best_length=length; best=axis; }
        }
    return best_length>1e-8f ? best*(1.f/best_length) : V3{};
}

// Puts a part back at rest (closed), as on load or before a save.
inline void ResetPart(const PartDef& def, PartState& state)
{
    state=PartState{};
    state.value=def.min;
}
}
