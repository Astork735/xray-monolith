#pragma once
#include <cmath>
#include <algorithm>
namespace vr
{
// The UI panel's height over its width in the headset. The game lays its UI
// out on a 1024x768 grid scaled separately in x and y to the (nearly square)
// eye-sized UI texture, so the panel shows that texture at 4:3: layout and
// pictures keep their shape. ui_core makes no widescreen correction in VR
// and narrows glyphs to match (see ui_core::VrGlyphWidth).
constexpr float kUiPanelAspect = 0.75f;
inline bool QuadRayUv(const Fvector& origin, const Fvector& direction,
    float distance, float width, float height, float& u, float& v)
{
    if (!(direction.z > 0.000001f && width > 0 && height > 0)) return false;
    const float t = (distance-origin.z)/direction.z;
    if (!(t > 0) || !std::isfinite(t)) return false;
    u = 0.5f + (origin.x+t*direction.x)/width;
    v = 0.5f - (origin.y+t*direction.y)/height;
    return std::isfinite(u) && std::isfinite(v) && u >= 0 && u <= 1 && v >= 0 && v <= 1;
}
// Keep a tracked cursor moving at the panel edge even when a controller ray
// lands just outside the quad. The inside flag still gates actual clicks.
inline bool QuadRayCursor(const Fvector& origin, const Fvector& direction,
    float distance, float width, float height, float& u, float& v, bool& inside)
{
    inside=false;
    u=v=-1.f;
    if (!(direction.z>0.000001f && width>0 && height>0)) return false;
    const float t=(distance-origin.z)/direction.z;
    if (!(t>0) || !std::isfinite(t)) return false;
    u=0.5f+(origin.x+t*direction.x)/width;
    v=0.5f-(origin.y+t*direction.y)/height;
    if (!std::isfinite(u) || !std::isfinite(v)) return false;
    inside=u>=0.f && u<=1.f && v>=0.f && v<=1.f;
    u=std::max(0.f,std::min(1.f,u));
    v=std::max(0.f,std::min(1.f,v));
    return true;
}
// Each available control arms independently on release: a held grip must not
// suppress a fresh trigger/A press, and reconnecting held controls must not skip.
struct UiConfirmState
{
    unsigned armed=0;
    bool Step(bool valid, unsigned buttons, unsigned available=~0u)
    {
        if (!valid) { armed=0; return false; }
        armed=(armed | ~buttons) & available;
        const bool pressed=(buttons & armed)!=0;
        armed &= ~buttons;
        return pressed;
    }
};
struct UiConsumedControls
{
    unsigned suppressed=0;
    void Consume(unsigned buttons) { suppressed |= buttons; }
    unsigned Filter(unsigned buttons)
    {
        suppressed &= buttons; // release ends suppression for that control
        return buttons & ~suppressed;
    }
    bool Blocked(unsigned bit) const { return (suppressed & bit)!=0; }
};
// Process a menu toggle on the rising edge of fresh input. A missed XR frame
// is not a release, so holding X cannot toggle the inventory repeatedly.
struct UiToggleState
{
    bool down=false;
    bool Step(bool fresh, bool pressed)
    {
        if (!fresh) return false;
        const bool click=pressed && !down;
        down=pressed;
        return click;
    }
};
struct UiClickState
{
    bool held = false, armed = false;
    // -1 release/cancel, 0 movement only, +1 press. Require release on entry.
    int Step(bool valid, bool down)
    {
        if (!valid) { const int event = held ? -1 : 0; held = armed = false; return event; }
        if (!down) { armed = true; const int event = held ? -1 : 0; held = false; return event; }
        if (!held && armed) { held = true; armed = false; return 1; }
        return 0;
    }
};
// A new hand may take over a stationary cursor and press in the same frame.
// Keep the armed release state unless a click is already being held.
inline bool CancelUiClickOnPointerChange(bool surface_changed, bool hand_changed,
    bool click_held)
{
    return surface_changed || (hand_changed && click_held);
}
}
