#include "stdafx.h"
#include "VrWristDisplay.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "VrBodySlots.h"
#include "../xrEngine/Vr/vr_gameplay.h"
#include "../xrEngine/Vr/vr_overlay_draw.h"

namespace
{
// Whether the display shows is decided once per frame, so both eyes agree.
u32 g_wrist_frame=u32(-1);
bool g_wrist_visible=false;

struct Panel
{
    // centre, the viewer's right and up, and the direction toward the viewer.
    Fvector centre, right, up, facing;
    void Quad(float x0, float y0, float x1, float y1, float lift, u32 color) const
    {
        const auto point=[&](float x, float y)
        {
            Fvector p=centre;
            p.mad(right,x).mad(up,y).mad(facing,lift);
            return p;
        };
        const Fvector a=point(x0,y0), b=point(x1,y0), c=point(x1,y1), d=point(x0,y1);
        const vr::OverlayDraw draw;
        draw.DrawFace(a,b,c,color,0,true,false);
        draw.DrawFace(a,c,d,color,0,true,false);
    }
};
}

void RenderVrWristDisplay(CActor& actor)
{
    if (!vr::WristDisplay() || !actor.g_Alive()) return;
    const int hand=1-vr::DominantHand();
    // Not while that hand holds the detector up to read it.
    if (VrDetectorHand(actor)==hand) return;
    Fmatrix grip;
    Fvector head;
    if (!vr::HandTransform(hand,false,grip) || !vr::HeadPosition(head)) return;
    // On the back of the wrist behind the controller grip. The grip's +Y
    // runs back along the forearm; the back of the hand faces -X on the left
    // hand and +X on the right (as player_hud_legs.cpp poses the hands).
    Fvector back=grip.i;
    if (!hand) back.invert();
    Fvector wrist; wrist.mad(grip.c,grip.j,0.07f).mad(back,0.03f);
    Fvector to_wrist; to_wrist.sub(wrist,head);
    const float distance=to_wrist.magnitude();
    if (g_wrist_frame!=Device.dwFrame)
    {
        g_wrist_frame=Device.dwFrame;
        bool visible=false;
        if (distance>0.1f && distance<0.65f && vr::HandSqueeze(hand)<0.5f)
        {
            Fvector direction=to_wrist; direction.div(distance);
            // Looking at the wrist, the forearm across the view rather than
            // pointing along it (a supporting hand under a rifle), and the
            // back of the wrist turned toward the eyes, as a watch is read.
            const float look=Device.vCameraDirection.dotproduct(direction);
            const float across=_abs(grip.j.dotproduct(direction));
            const float facing=-back.dotproduct(direction);
            visible=look>_cos(deg2rad(g_wrist_visible ? 28.f : 22.f)) && across<0.6f && facing>0.2f;
        }
        g_wrist_visible=visible;
    }
    if (!g_wrist_visible) return;
    // Facing the head, upright in the world. X-Ray is left-handed: the
    // viewer's right is up x forward, forward pointing away from the viewer.
    Panel panel;
    panel.facing.sub(head,wrist).normalize_safe();
    panel.centre.mad(wrist,panel.facing,0.035f);
    Fvector forward; forward.invert(panel.facing);
    Fvector up; up.set(0.f,1.f,0.f);
    panel.right.crossproduct(up,forward);
    if (panel.right.square_magnitude()<EPS_S) return;
    panel.right.normalize();
    panel.up.crossproduct(forward,panel.right).normalize();
    CActorCondition& condition=actor.conditions();
    struct Row { float value; u32 color; };
    Row rows[6];
    int count=0;
    rows[count++]={condition.GetHealth(),0xFFD03A30};
    rows[count++]={condition.GetPower(),0xFFE0C040};
    if (condition.GetRadiation()>0.005f) rows[count++]={condition.GetRadiation(),0xFF50D040};
    if (condition.GetPsyHealth()<0.995f) rows[count++]={condition.GetPsyHealth(),0xFF5080F0};
    const float bleeding=condition.BleedingSpeed();
    if (bleeding>0.f) rows[count++]={_min(bleeding,1.f),0xFF800010};
    const float satiety=condition.GetSatiety();
    if (satiety<0.5f) rows[count++]={satiety,0xFFB07030};
    const float half_width=0.045f, row_height=0.008f, gap=0.004f, margin=0.005f;
    const float height=float(count)*row_height+float(count-1)*gap;
    panel.Quad(-half_width-margin,-height*0.5f-margin,half_width+margin,height*0.5f+margin,0.f,0xFF141414);
    float top=height*0.5f;
    for (int i=0;i<count;++i)
    {
        const float value=clampr(rows[i].value,0.f,1.f);
        const float bottom=top-row_height;
        panel.Quad(-half_width,bottom,half_width,top,0.001f,0xFF3A3A3A);
        if (value>0.f)
            panel.Quad(-half_width,bottom,-half_width+2.f*half_width*value,top,0.002f,rows[i].color);
        top=bottom-gap;
    }
}
