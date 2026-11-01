#pragma once
#include <cmath>
#include <algorithm>

namespace vr
{
// Use the dynamic DrawFace/DrawLine path. The legacy DrawAABB/DrawIdentBox
// buffers have no DX11 implementation and their draw delegates are null.
template<class Drawer>
void DrawHandProxy(Drawer& drawer, const Fmatrix& grip, int hand, float curl, unsigned color)
{
    if (!std::isfinite(curl)) curl = 0;
    if (curl < 0) curl = 0;
    if (curl > 1) curl = 1;
    Fvector corners[8];
    for (unsigned vertex = 0; vertex < 8; ++vertex)
    {
        corners[vertex].set(vertex & 1 ? 0.035f : -0.035f,
            vertex & 2 ? 0.018f : -0.018f, vertex & 4 ? 0.045f : -0.045f);
        grip.transform_tiny(corners[vertex]);
    }
    static const unsigned faces[12][3] = {
        {0,2,3}, {0,3,1}, {4,5,7}, {4,7,6},
        {0,1,5}, {0,5,4}, {2,6,7}, {2,7,3},
        {0,4,6}, {0,6,2}, {1,3,7}, {1,7,5}
    };
    for (const auto& face : faces)
        drawer.DrawFace(corners[face[0]], corners[face[1]], corners[face[2]], color, 0, true, false);
    for (unsigned vertex = 0; vertex < 8; ++vertex)
        for (unsigned axis = 1; axis <= 4; axis <<= 1)
            if (!(vertex & axis)) drawer.DrawLine(corners[vertex], corners[vertex | axis], 0xff303030);
    for (int finger = 0; finger < 4; ++finger)
    {
        Fvector start{(finger-1.5f)*0.017f,0,0.04f};
        Fvector joint{start.x,-0.025f*curl,0.075f-0.02f*curl};
        Fvector tip{start.x,-0.055f*curl,0.1f-0.05f*curl};
        grip.transform_tiny(start); grip.transform_tiny(joint); grip.transform_tiny(tip);
        drawer.DrawLine(start, joint, color); drawer.DrawLine(joint, tip, color);
    }
    Fvector thumb{hand ? -0.045f : 0.045f,0,0.02f};
    grip.transform_tiny(thumb); drawer.DrawLine(grip.c, thumb, color);
}

// Lightweight two-bone arm IK for the VR body proxy. The torso follows the
// flattened head/body heading; both wrist targets are tracked controller
// poses. Keep the upper and lower arm at plausible lengths and solve the
// elbow on an outward/backwards bend plane. No actor skeleton is modified.
template<class Drawer>
void DrawBodyIK(Drawer& drawer, const Fmatrix& body, const Fmatrix hand[2],
    const bool tracked[2])
{
    const unsigned torso_color=0xff526578, arm_color=0xff7799a8;
    Fvector chest=body.c; chest.y-=0.34f;
    Fvector waist=body.c; waist.y-=0.72f;
    Fvector shoulder[2], hip[2];
    for (int side=0;side<2;++side)
    {
        const float sign=side ? 1.f : -1.f;
        shoulder[side]=chest; shoulder[side].mad(body.i,sign*0.19f);
        hip[side]=waist; hip[side].mad(body.i,sign*0.14f);
        drawer.DrawLine(shoulder[side],hip[side],torso_color);
        if (!tracked[side]) continue;
        Fvector direction; direction.sub(hand[side].c,shoulder[side]);
        const float reach=direction.magnitude();
        if (reach<0.0001f) continue;
        direction.div(reach);
        const float bounded=std::min(reach,0.66f);
        Fvector pole=body.i; pole.mul(sign);
        pole.mad(body.k,-0.55f); pole.y-=0.20f;
        pole.mad(direction,-pole.dotproduct(direction));
        if (pole.square_magnitude()<0.0001f) pole=body.j;
        pole.normalize_safe();
        const float half=bounded*0.5f;
        const float bend=std::sqrt(std::max(0.f,0.33f*0.33f-half*half));
        Fvector elbow=shoulder[side];
        elbow.mad(direction,half); elbow.mad(pole,bend);
        drawer.DrawLine(shoulder[side],elbow,arm_color);
        drawer.DrawLine(elbow,hand[side].c,arm_color);
        drawer.DrawCross(elbow,0.023f,arm_color);
    }
    drawer.DrawLine(shoulder[0],shoulder[1],torso_color);
    drawer.DrawLine(hip[0],hip[1],torso_color);
    drawer.DrawLine(chest,waist,torso_color);
}
// A world point in homogeneous clip space (x, y, z, w) for the rasterizer,
// which divides by w itself. Fmatrix::transform into an Fvector4 already
// divides x, y and z, so feeding that to a vertex shader divides twice.
inline void ClipPosition(const Fmatrix& m, const Fvector& p, float* clip)
{
    clip[0]=p.x*m._11+p.y*m._21+p.z*m._31+m._41;
    clip[1]=p.x*m._12+p.y*m._22+p.z*m._32+m._42;
    clip[2]=p.x*m._13+p.y*m._23+p.z*m._33+m._43;
    clip[3]=p.x*m._14+p.y*m._24+p.z*m._34+m._44;
}
}
