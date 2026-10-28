#pragma once
#include "../../Include/xrRender/DrawUtils.h"

namespace vr
{
// Lines and faces drawn into the eyes over the world (after vr_world_overlay).
// Its shader (editor\wire) colours from the tfactor constant, not from the
// vertices, so each primitive sets its colour first; through DU directly
// everything comes out white.
struct OverlayDraw
{
    void DrawLine(const Fvector& a, const Fvector& b, u32 color) const
    {
        vr_world_overlay_color(color);
        DU->DrawLine(a, b, color);
    }
    void DrawFace(const Fvector& a, const Fvector& b, const Fvector& c, u32 solid_color, u32 wire_color,
        bool solid, bool wire) const
    {
        if (solid) { vr_world_overlay_color(solid_color); DU->DrawFace(a, b, c, solid_color, wire_color, TRUE, FALSE); }
        if (wire) { vr_world_overlay_color(wire_color); DU->DrawFace(a, b, c, solid_color, wire_color, FALSE, TRUE); }
    }
    void DrawCross(const Fvector& p, float size, u32 color) const
    {
        vr_world_overlay_color(color);
        DU->DrawCross(p, size, color);
    }
};
}
