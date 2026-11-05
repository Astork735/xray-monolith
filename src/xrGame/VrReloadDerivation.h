#pragma once
#include <cmath>

namespace vr
{
// Full animation excursion includes the hand carrying a removed magazine.
// Keep the remote/unnamed-part limit, but do not reject a named magazine
// whose seated mesh was independently found beside the trigger. Such a
// magazine may also first slide straight back (a drum unhooking) when it
// then travels further than any bolt stroke (20 cm; VR caps those at 15).
inline bool ReloadMagazineMotion(float travel, float first_y, float first_z,
    bool rests_above, bool named_near_seat)
{
    const bool unhooks=named_near_seat && travel>0.2f;
    return std::isfinite(travel) && travel>=0.04f && (travel<=0.6f || named_near_seat) &&
        (first_z>=-0.8f || unhooks) && !(first_y>0.5f && rests_above);
}

// Belt links and rounds carried by an accepted detachable magazine cannot
// also establish an internal, top-loaded magazine.
inline bool ReloadClipMotion(float travel, float first_y, bool rests_above, bool in_magazine)
{
    return !in_magazine && travel>=0.04f && travel<=0.6f && first_y>0.5f && rests_above;
}

// A magazine-sized, magazine-named part outside the canonical magazine is a
// reload-only copy (VR hides it) when the weapon's idle hides it (scaled),
// parks it off the gun (30 cm or more from the seated magazine) or stacks it
// on the seated one (half or more of it inside). One the idle shows
// elsewhere on the gun (a taped pair, a drum housing) stays visible. Without
// an idle sample nothing tells them apart, and it is hidden.
inline bool ReloadOnlyMagazine(bool idle_sampled, bool scaled, float distance, float overlap)
{
    return !idle_sampled || scaled || !std::isfinite(distance) || distance>=0.3f || overlap>=0.5f;
}
}
