#pragma once
#include "VrMechanismParts.h"
#include <cstring>

namespace vr
{
// The order a weapon's reload must follow, as conditions on its parts rather
// than a fixed script, so any mechanically valid order works and a wrong one
// is refused with a reason: a belt-fed machine gun takes its box only with
// the top cover open, the belt only onto an open feed tray, and fires and
// chambers only with the cover shut on a seated belt. Engine-free (see
// tests/vr/mechanism_tests.cpp).
constexpr int max_parts=4;
constexpr int max_gates=4;

enum GateKind { GatePartOpen, GatePartClosed, GateBeltSeated, GateMagazineIn };
struct Gate { GateKind kind=GatePartOpen; int part=0; };
struct GateList
{
    Gate gates[max_gates];
    int count=0;
    bool Add(GateKind kind, int part=0)
    {
        if (count>=max_gates) return false;
        gates[count].kind=kind; gates[count].part=part; ++count;
        return true;
    }
};

struct ReloadRules
{
    GateList magazine;  // taking out or putting in the magazine (box, drum)
    GateList belt;      // laying the belt on the feed tray
    GateList fire;      // firing
    GateList chamber;   // working the charging handle to chamber a round
    bool Empty() const { return !magazine.count && !belt.count && !fire.count && !chamber.count; }
};

struct ReloadFacts
{
    bool open[max_parts]{}, closed[max_parts]{};
    bool belt_seated=true, magazine_in=true;
};

// Parses "cover:open, belt:seated, magazine:in" (part names from names[]).
// Unknown words are skipped and reported through *bad (may be null).
inline GateList ParseGates(const char* text, const char* const* names, int name_count, const char** bad=nullptr)
{
    GateList list;
    if (!text) return list;
    char word[64];
    const char* at=text;
    while (*at)
    {
        while (*at==' ' || *at==',' || *at=='\t') ++at;
        int length=0;
        while (at[length] && at[length]!=',' && length<63) ++length;
        if (!length) break;
        std::memcpy(word,at,size_t(length)); word[length]=0;
        while (length && (word[length-1]==' ' || word[length-1]=='\t')) word[--length]=0;
        at+=length; while (*at && *at!=',') ++at;
        char* colon=std::strchr(word,':');
        const char* state=colon ? colon+1 : "";
        if (colon) *colon=0;
        bool known=false;
        if (!std::strcmp(word,"belt") && !std::strcmp(state,"seated")) known=list.Add(GateBeltSeated);
        else if (!std::strcmp(word,"magazine") && !std::strcmp(state,"in")) known=list.Add(GateMagazineIn);
        else
            for (int i=0;i<name_count && !known;++i)
                if (names[i] && !std::strcmp(word,names[i]))
                {
                    if (!std::strcmp(state,"open")) known=list.Add(GatePartOpen,i);
                    else if (!std::strcmp(state,"closed")) known=list.Add(GatePartClosed,i);
                }
        if (!known && bad) *bad=text;
    }
    return list;
}

// True when every condition holds; otherwise *blocked names the first that
// does not (a gate index into the list).
inline bool GatesPass(const GateList& list, const ReloadFacts& facts, int* blocked=nullptr)
{
    for (int i=0;i<list.count;++i)
    {
        const Gate& g=list.gates[i];
        bool ok=true;
        switch (g.kind)
        {
        case GatePartOpen: ok=g.part<max_parts && facts.open[g.part]; break;
        case GatePartClosed: ok=g.part<max_parts && facts.closed[g.part]; break;
        case GateBeltSeated: ok=facts.belt_seated; break;
        case GateMagazineIn: ok=facts.magazine_in; break;
        }
        if (!ok) { if (blocked) *blocked=i; return false; }
    }
    return true;
}

// A belt fed from the box: its end, at the box mouth while the box is in,
// is taken by a hand and laid on the feed tray. It is seated once the hand
// brings it within tolerance of the tray while the rules allow it; let go
// anywhere else it falls back to the box. Taking the box out unseats it.
struct BeltDef
{
    PartVec box_end{}, tray{};   // model space, at rest
    float grab_radius=0.06f, tolerance=0.035f;
    bool used=false;
};
struct BeltState
{
    bool seated=true;
    int hand=-1;
    PartVec carried{}; // where the held end is (model space)
};
enum BeltEvent : unsigned { BeltTaken=1u, BeltDropped=2u, BeltSeated=4u };

inline unsigned StepBelt(const BeltDef& def, BeltState& state, int hand, const PartInput& in,
    bool may_take, bool may_seat)
{
    unsigned events=0;
    if (!def.used) return 0;
    if (state.hand==hand)
    {
        if (!in.tracked || !in.grip) { state.hand=-1; events|=BeltDropped; return events; }
        state.carried=in.hand;
        if (may_seat && PartLength(PartSub(in.hand,def.tray))<=def.tolerance)
        {
            state.hand=-1; state.seated=true; events|=BeltSeated;
        }
    }
    else if (state.hand<0 && !state.seated && may_take && in.tracked && in.grip_edge &&
        PartLength(PartSub(in.hand,def.box_end))<=def.grab_radius)
    {
        state.hand=hand; state.carried=in.hand; events|=BeltTaken;
    }
    return events;
}
}
