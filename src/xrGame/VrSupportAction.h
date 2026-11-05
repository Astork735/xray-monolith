#pragma once
// What the support hand's trigger works on a held weapon when it is pulled:
// the part the hand is nearest, among those in reach. Each part is offered
// only when it can be worked now (a locked action's release, an open dust
// cover, a seated magazine...), and only the chosen one is worked: a pull
// near two parts no longer works both, or whichever came first in the
// update. Exact ties keep the order they are offered in. Engine-free, so the
// choice is unit tested.
namespace vr
{
enum class SupportAction { None, Rack, TakeMagazine, ReleaseAction, CloseDustCover, SelectLauncher };

struct SupportCandidate
{
    SupportAction action=SupportAction::None;
    // Metres from the hand to the part's grab point (or the magazine's
    // surface), and how far it reaches; a candidate out of reach is none.
    float distance=0.f, reach=0.f;
};

inline SupportAction ChooseSupportAction(const SupportCandidate* candidates, int count)
{
    SupportAction chosen=SupportAction::None;
    float best=0.f;
    for (int i=0;i<count;++i)
    {
        const SupportCandidate& candidate=candidates[i];
        if (candidate.action==SupportAction::None || !(candidate.distance<=candidate.reach)) continue;
        if (chosen==SupportAction::None || candidate.distance<best)
        {
            chosen=candidate.action;
            best=candidate.distance;
        }
    }
    return chosen;
}
}
