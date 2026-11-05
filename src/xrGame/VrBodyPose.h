#pragma once
#include "VrContactField.h"

// The player's visible body in VR, estimated from the headset and hands
// (engine-free so it is tested). The game's own walk, run and crouch
// animation poses the legs only; the torso is upright and unanimated, hung
// well behind and below the eyes. The body stands with its neck under the
// tracked neck: a lower head drops the pelvis and bends the knees with the
// feet kept where the animation put them, and the torso faces between the
// head and the hands. Units are metres, +y up.
namespace vr
{
struct BodyLean
{
    float rise_max=0.03f;   // highest the pelvis rises above the animation (on tiptoe)
    float drop_max=0.60f;   // lowest it drops below it (a deep real crouch)
    // The neck's pivot, in the head's own frame: behind and below the eyes.
    // The eyes swing forward round it when looking down; it stays put.
    float pivot_back=0.09f, pivot_down=0.11f;
    // The torso hangs from below and behind that pivot (level forward), so
    // the chest stays out of the view: 15 cm back and 20 cm down in all.
    float neck_back=0.06f, neck_down=0.09f;
};

// The tracked neck the torso hangs from. From the eyes along the head's own
// back and down axes to the neck's pivot (so looking down, which swings the
// eyes forward, leaves it where it is), then behind it along the level
// forward and below it.
inline V3 NeckTarget(const V3& eye, const V3& eye_forward, const V3& eye_up, const BodyLean& lean)
{
    const V3 pivot=eye-eye_forward*lean.pivot_back-eye_up*lean.pivot_down;
    V3 flat(eye_forward.x,0.f,eye_forward.z);
    // Looking straight down the head's up is the level forward; looking
    // straight up, its reverse.
    if (Length(flat)<0.3f)
    {
        const float sign=eye_forward.y>0.f ? -1.f : 1.f;
        flat=V3(eye_up.x*sign,0.f,eye_up.z*sign);
    }
    flat=Length(flat)>1e-4f ? Normalized(flat) : V3(0,0,0);
    return pivot-flat*lean.neck_back-V3(0.f,lean.neck_down,0.f);
}

// Two-bone limb: where the knee (or elbow) goes for the hip at `hip`, the
// foot at `foot`, bending toward `pole` (a point the knee should face, such
// as the animated knee). False when the foot is out of reach (the leg is
// straight toward it; the knee is then on that line).
inline bool KneePosition(const V3& hip, const V3& foot, const V3& pole, float thigh, float calf, V3& knee)
{
    V3 to=foot-hip;
    float distance=Length(to);
    if (distance<1e-5f) { knee=hip+V3(0,-thigh,0); return false; }
    const V3 dir=to*(1.f/distance);
    const bool reached=distance<=thigh+calf-1e-4f;
    distance=std::max(std::abs(thigh-calf)+1e-4f,std::min(thigh+calf-1e-4f,distance));
    const float along=(thigh*thigh-calf*calf+distance*distance)/(2.f*distance);
    const float bend=std::sqrt(std::max(0.f,thigh*thigh-along*along));
    V3 side=pole-hip;
    side=side-dir*Dot(side,dir);
    if (Dot(side,side)<1e-10f)
    {
        // No pole off the line: bend forward (+z), or else up.
        side=V3(0,0,1)-dir*dir.z;
        if (Dot(side,side)<1e-10f) side=V3(0,1,0)-dir*dir.y;
    }
    knee=hip+dir*along+Normalized(side)*bend;
    return reached;
}

// Yaw (radians about +y; 0 faces +z) halfway `weight` from a to b, the short way round.
inline float BlendYaw(float a, float b, float weight)
{
    float d=b-a;
    while (d>3.14159265f) d-=6.2831853f;
    while (d<-3.14159265f) d+=6.2831853f;
    return a+d*weight;
}
inline float YawOf(const V3& forward) { return std::atan2(forward.x,forward.z); }

// Where the torso faces: the head's torso estimate (which already follows
// the head with a deadband and settles when looking down) turned part of
// the way toward the hands when both are out in front of the body, as when
// holding a weapon or working with both hands.
inline float TorsoFacing(float head_torso_yaw, const V3& neck, const V3* hands, int hand_count)
{
    if (hand_count<2) return head_torso_yaw;
    V3 middle=(hands[0]+hands[1])*0.5f-neck;
    middle.y=0.f;
    if (Length(middle)<0.15f) return head_torso_yaw;
    const float toward=YawOf(middle);
    const V3 facing(std::sin(head_torso_yaw),0.f,std::cos(head_torso_yaw));
    if (Dot(Normalized(middle),facing)<0.3f) return head_torso_yaw;  // behind or far aside
    return BlendYaw(head_torso_yaw,toward,0.3f);
}
}
