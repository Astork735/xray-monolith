#include "../../src/xrGame/VrBodyPose.h"
#include <cstdio>
#include <cstdlib>
#include "check.h"
using namespace vr;
int main()
{
    const BodyLean lean;
    // The torso stays behind the head looking up as well as down: past the
    // switch to the head's up (72.5 degrees) nothing jumps.
    const auto neck_at=[&](float degrees)
    {
        const float a=degrees*3.14159265f/180.f;
        return NeckTarget(V3(0,1.7f,0),V3(0,std::sin(a),std::cos(a)),V3(0,std::cos(a),-std::sin(a)),lean);
    };
    CHECK(std::abs(neck_at(72.f).z-neck_at(73.f).z)<0.02f);
    CHECK(std::abs(neck_at(-72.f).z-neck_at(-73.f).z)<0.02f);
    CHECK(neck_at(85.f).z<0.06f);  // behind the neck's pivot, about 10 cm ahead of the eyes here

    // Knee: a 45+45 cm leg with the foot 60 cm below the hip bends forward
    // toward the pole, both segments keeping their lengths.
    V3 knee;
    CHECK(KneePosition(V3(0,1,0),V3(0,0.4f,0),V3(0,0.7f,0.5f),0.45f,0.45f,knee));
    CHECK(Near(Length(knee-V3(0,1,0)),0.45f) && Near(Length(V3(0,0.4f,0)-knee),0.45f));
    CHECK(knee.z>0.2f && Near(knee.y,0.7f,1e-3f));
    // Out of reach: straight toward the foot.
    CHECK(!KneePosition(V3(0,1,0),V3(0,-0.2f,0),V3(0,0.5f,0.5f),0.45f,0.45f,knee));
    CHECK(Near(knee.x,0.f) && Near(knee.z,0.f,1e-2f) && knee.y<0.56f);
    // No pole off the line: forward.
    KneePosition(V3(0,1,0),V3(0,0.4f,0),V3(0,0.5f,0),0.45f,0.45f,knee);
    CHECK(knee.z>0.2f);

    // Yaw blending takes the short way round.
    CHECK(Near(BlendYaw(3.0f,-3.0f,0.5f),3.14159f,2e-3f) || Near(BlendYaw(3.0f,-3.0f,0.5f),-3.14159f,2e-3f));
    CHECK(Near(BlendYaw(0.f,1.f,0.3f),0.3f));
    // Torso facing: hands out in front to the right turn it a third toward them.
    const V3 hands[2]={V3(0.3f,1.2f,0.4f),V3(0.4f,1.2f,0.3f)};
    const float toward=YawOf(V3(0.35f,0,0.35f));
    CHECK(Near(TorsoFacing(0.f,V3(0,1.5f,0),hands,2),0.3f*toward));
    // Hands at the sides (close to the body) or behind: the head's estimate.
    const V3 close[2]={V3(0.05f,1.0f,0.0f),V3(-0.05f,1.0f,0.05f)};
    CHECK(Near(TorsoFacing(0.5f,V3(0,1.5f,0),close,2),0.5f));
    const V3 behind[2]={V3(0,1.2f,-0.4f),V3(0.1f,1.2f,-0.4f)};
    CHECK(Near(TorsoFacing(0.f,V3(0,1.5f,0),behind,2),0.f));
    CHECK(Near(TorsoFacing(0.2f,V3(0,1.5f,0),hands,1),0.2f));
    // The neck the torso hangs from: 15 cm behind and 20 cm below the eyes
    // along the level forward; looking straight down does not swing it
    // forward under the eyes.
    // Level gaze: 15 cm behind and 20 cm below the eyes.
    const V3 eye(0.f,1.7f,0.f);
    V3 neck=NeckTarget(eye,V3(0.f,0.f,1.f),V3(0.f,1.f,0.f),lean);
    CHECK(Near(neck.z,-0.15f) && Near(neck.y,1.5f) && Near(neck.x,0.f));
    // Looking straight down, the eyes swing about 10 cm forward round the
    // neck: the torso stays behind where they were, it does not follow.
    // The pivot stays at (0, 1.59, -0.09); the eyes, swung down round it,
    // are 9 cm along the new forward (down) and 11 cm along the new up
    // (the old forward) from it.
    const V3 eye_down(0.f,1.59f-0.09f,-0.09f+0.11f);
    neck=NeckTarget(eye_down,V3(0.f,-1.f,0.f),V3(0.f,0.f,1.f),lean);
    CHECK(neck.z<=-0.14f && neck.z>=-0.16f && Near(neck.y,1.5f,0.01f));
    std::printf("PASS: VR body pose\n");
    return 0;
}
