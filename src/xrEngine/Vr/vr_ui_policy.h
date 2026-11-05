#pragma once
#include <cmath>
#include <algorithm>
namespace vr
{
// The UI panel's height over its width in the headset. The game lays its UI
// out on a 1024x768 grid scaled separately in x and y to the (nearly square)
// eye-sized UI texture, so the panel shows that texture at 4:3: layout and
// pictures keep their shape. ui_core makes no widescreen correction in VR
// and the font renderer narrows glyphs drawn on the panel to match
// (dxFontRender, while g_vr_ui_pass).
constexpr float kUiPanelAspect = 0.75f;
// Where a ray from the eye (origin, direction in the head's frame) meets the
// UI panel `distance` ahead: u, v across it, clamped to its edges so a
// tracked cursor keeps moving there when the ray lands just outside. The
// inside flag still gates actual clicks.
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
// Steadies the menu cursor against hand jitter (vr_cursor_smoothing). At
// rest the cursor trails with a time constant of `strength` hundredths of a
// second; the further it trails the real ray, the shorter that gets, so a
// sweep across the panel hardly lags while a still hand's tremor is damped.
struct CursorSmoother
{
    bool valid=false;
    float u=0.f, v=0.f;
    void Reset() { valid=false; }
    void Step(float& cursor_u, float& cursor_v, float dt, float strength)
    {
        if (!valid || !(strength > 0.f) || !(dt > 0.f))
        {
            valid=true; u=cursor_u; v=cursor_v;
            return;
        }
        const float du=cursor_u-u, dv=cursor_v-v;
        // Trailing by 1% of the panel's width (1.5 cm at the menu) halves it.
        const float lag=std::sqrt(du*du+dv*dv);
        const float tau=strength*0.01f/(1.f+lag/0.01f);
        const float k=1.f-std::exp(-std::min(dt,0.1f)/tau);
        u+=du*k; v+=dv*k;
        cursor_u=u; cursor_v=v;
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
// A thumbstick as a mouse wheel: one step as soon as it is pushed (past
// 0.3), then repeating faster the further it is pushed. Step returns the
// steps this frame, + up.
struct UiScrollRepeat
{
    float accumulated = 0.f;
    int Step(float axis, float dt)
    {
        if (std::fabs(axis) < 0.3f) { accumulated = 0.f; return 0; }
        if (accumulated == 0.f) accumulated = axis > 0.f ? 1.f : -1.f;
        else accumulated += axis*std::min(dt, 0.1f)*12.f;
        int steps = 0;
        while (std::fabs(accumulated) >= 1.f)
        {
            const bool up = accumulated > 0.f;
            accumulated += up ? -1.f : 1.f;
            // Keep the direction so the next frame repeats, not restarts.
            if (accumulated == 0.f) accumulated = up ? 1e-4f : -1e-4f;
            steps += up ? 1 : -1;
        }
        return steps;
    }
};
// The HUD drawn on a panel magnified k times over its default angular size
// (vr_hud_scale / vr_hud_distance): the offset (in UI units) that moves an
// element so it keeps its place on the screen and grows inward. Its anchor
// is the point as far across the element as the element is across the free
// space: an element at the right edge keeps its right edge, a centred one
// its centre. k <= 1 moves nothing.
inline void HudInsetOffset(float left, float top, float width, float height,
    float screen_width, float screen_height, float k, float& dx, float& dy)
{
    dx = dy = 0.f;
    if (!(k > 1.f)) return;
    const auto axis = [k](float start, float size, float screen)
    {
        const float free = screen - size;
        const float r = free > 0.f ? std::max(0.f, std::min(1.f, start/free)) : 0.5f;
        const float anchor = start + r*size, centre = 0.5f*screen;
        return (anchor - centre)*(1.f/k - 1.f);
    };
    dx = axis(left, width, screen_width);
    dy = axis(top, height, screen_height);
}
}
