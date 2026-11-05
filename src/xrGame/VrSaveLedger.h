#pragma once
#include <cstddef>

namespace vr
{
// The save format and the desktop game hold a weapon as an ordinary
// magazine: at most its capacity, one ammo type. VR also has a chambered
// round on top of a full magazine, magazines out of the gun and mixed types.
// Before a save these are folded into the weapon up to its capacity and the
// rest go back to the inventory, so no round is created or lost and a save
// loads the same way in VR and on the desktop. Engine-free (see
// tests/vr/save_tests.cpp); Rounds is a vector of cartridges whose live
// order is bottom first, the chambered round last.
//
// same(a, b) tells whether two cartridges are the same ammo type;
// give_back(c) returns one to the inventory.
template <typename Rounds, typename Same, typename GiveBack>
unsigned NormalizeRounds(Rounds& live, Rounds* spare, size_t spare_count, size_t capacity,
    Same same, GiveBack give_back)
{
    unsigned returned=0;
    // Spare magazines fill the weapon from below while it has room and
    // the type matches what is on top.
    for (size_t m=0;m<spare_count;++m)
    {
        for (const auto& round:spare[m])
        {
            if (live.size()<capacity && (live.empty() || same(live.back(),round)))
            {
                live.insert(live.begin(),round);
                continue;
            }
            give_back(round);
            ++returned;
        }
        spare[m].clear();
    }
    // One ammo type: the top (chambered) round's.
    if (!live.empty())
    {
        const auto keep=live.back();
        for (auto it=live.begin();it!=live.end();)
        {
            if (same(*it,keep)) { ++it; continue; }
            give_back(*it);
            ++returned;
            it=live.erase(it);
        }
    }
    // A chambered round on a full magazine is one more than the desktop
    // holds: the bottom rounds go back, the chambered one stays on top.
    while (live.size()>capacity)
    {
        give_back(live.front());
        live.erase(live.begin());
        ++returned;
    }
    return returned;
}
}
