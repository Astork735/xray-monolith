#pragma once
#include "VrContactField.h"
#include "VrMagazineContact.h"

// Collision-aware finger posing without simulating the hand. Each finger
// segment is a capsule tested against the held object's distance field; each
// joint curls until its capsule just touches (a bisection on one angle), and
// the palm is pushed out of the surface before the arm is solved. A held
// grip barely moves relative to the hand, so solved curls are cached and
// reused until the hand actually shifts on the object.
namespace vr
{
// Clearance of a capsule a..b of `radius` (negative = penetrating).
inline float CapsuleClearance(const ContactScene& scene, const V3& a, const V3& b, float radius)
{
    // The joint end `a` is the previous segment's tip, already tested.
    float d=scene.Distance(b);
    d=std::min(d,scene.Distance(Lerp(a,b,0.33f)));
    d=std::min(d,scene.Distance(Lerp(a,b,0.67f)));
    return d-radius;
}

// The curl in lo..hi at which the segment first touches (`clear(angle)`
// returns clearance). A fully closed finger may already be past a thin part,
// so the joint closes in steps to the first blocked angle, then bisects. A
// warm start resumes just short of last frame's angle, so a grip that holds
// still converges in a few lookups.
template<class Clear>
float ContactCurl(Clear clear, float lo, float hi, float warm, bool has_warm, int refine=4, float step=0.2f)
{
    float a=lo;
    if (has_warm)
    {
        const float w=std::min(hi,warm-step*0.5f);
        if (w>lo && clear(w)>=0.f) a=w;
    }
    if (a==lo && clear(lo)<0.f) return lo;  // already touching at the start
    float b=a;
    for (;;)
    {
        if (a>=hi) return hi;               // closes fully without meeting anything
        b=std::min(hi,a+step);
        if (clear(b)<0.f) break;
        a=b;
    }
    for (int n=0;n<refine;++n)
    {
        const float m=(a+b)*0.5f;
        if (clear(m)>=0.f) a=m; else b=m;
    }
    return a;
}

// Moves the palm (a few spheres, object space) out of the surface. Returns
// the offset to apply to the hand, at most `limit` long.
inline V3 PalmPushOut(const ContactScene& scene, const V3* centres, const float* radii, int count,
    float limit=0.02f, int passes=3)
{
    V3 offset(0,0,0);
    for (int pass=0;pass<passes;++pass)
    {
        V3 push(0,0,0);
        int touching=0;
        for (int n=0;n<count;++n)
        {
            const V3 p=centres[n]+offset;
            if (scene.Distance(p)>=radii[n]) continue;
            V3 normal;
            const float d=scene.Contact(p,normal);
            push=push+normal*(radii[n]-d);
            ++touching;
        }
        if (!touching) break;
        offset=offset+push*(1.f/float(touching));
        const float len=Length(offset);
        if (len>limit) { offset=offset*(limit/len); break; }
    }
    return offset;
}

// Solved curls of one hand on one object, reused while the hand stays put on
// it. The trigger only adds to them, so it does not disturb them.
struct GripCache
{
    static constexpr int kDigits=5, kJoints=4;
    Frame hand_in_object;
    const void* object=nullptr;
    float squeeze=-1.f;
    float curl[kDigits][kJoints]={};
    bool valid=false;
    unsigned hits=0, misses=0;

    void Invalidate() { valid=false; object=nullptr; }
    // True when the stored curls still fit.
    bool Check(const Frame& hand, const void* on, float squeeze_now, float move=0.001f, float turn=0.0087f)
    {
        const bool reuse=valid && on==object && std::abs(squeeze_now-squeeze)<0.02f &&
            Length(hand.c-hand_in_object.c)<move && Length(RotationBetween(hand_in_object,hand))<turn;
        if (reuse) ++hits; else ++misses;
        return reuse;
    }
    void Store(const Frame& hand, const void* on, float squeeze_now)
    {
        hand_in_object=hand; object=on; squeeze=squeeze_now; valid=true;
    }
};

// Closes one finger's joints in turn, knuckle outward, each until its segment
// touches: `clear(joint, curl)` returns that segment's clearance for the curls.
// `curl` comes in as the hold's shape; a joint that is already touching at the
// start, or closes without meeting anything, keeps its value from that shape.
template<class Clear>
void FitChain(float* curl, int count, const float* limit, const float* warm, bool has_warm, Clear clear)
{
    float shape[GripCache::kJoints];
    for (int j=0;j<count;++j) shape[j]=curl[j];
    for (int j=0;j<count;++j)
    {
        const float most=limit[std::min(j,2)];
        for (int k=j+1;k<count;++k) curl[k]=0.f;
        const float found=ContactCurl([&](float angle)
        {
            curl[j]=angle;
            return clear(j,curl);
        },0.f,most,has_warm ? warm[j] : 0.f,has_warm);
        curl[j]=found<=0.f || found>=most ? shape[j] : found;
        // The search left the joint at its last try: the next joint builds
        // on this one, so put it where it ended.
        clear(j,curl);
    }
}

// What a hand closes on, picked from the regions already measured on the
// weapon. It selects the curl prior only; the contact search does the rest.
enum class GripKind { Free, PistolGrip, ForeEnd, Magazine, ChargingHandle };
// Starting curl shape (thumb, index, rest) x joint, before contact: wraps
// close all fingers; the charging-handle pinch opposes thumb and index and
// leaves the others loosely bent.
using CurlTable=float[3][3];
inline const CurlTable& GripPrior(GripKind kind)
{
    static const CurlTable wrap={{0.45f,0.60f,0.50f},{1.0f,1.2f,0.8f},{1.10f,1.30f,0.90f}};
    static const CurlTable pinch={{0.70f,0.80f,0.60f},{0.90f,1.10f,0.70f},{0.60f,0.80f,0.50f}};
    static const CurlTable open={{0.05f,0.10f,0.10f},{0.20f,0.25f,0.20f},{0.25f,0.30f,0.25f}};
    if (kind==GripKind::ChargingHandle) return pinch;
    if (kind==GripKind::Free) return open;
    return wrap;
}
}
