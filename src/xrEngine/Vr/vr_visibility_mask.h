#pragma once
#include <cstddef>
#include <cstdint>

namespace vr
{
// XR_KHR_visibility_mask gives each eye's hidden area (the corners the lenses
// never show) as triangles in view space on the z = -1 plane, so x and y are
// the tangents of the view angles: the same units as the eye's XrFovf. These
// place them on the eye image and measure how much of the image they cover.
struct MaskPoint { float x, y; };

// The image position (-1..1 both ways, y up) of a tangent point, for an eye
// whose field of view spans these tangents (left and down are negative).
inline MaskPoint MaskPointToImage(MaskPoint p, float tan_left, float tan_right, float tan_down, float tan_up)
{
    return {(2.f*p.x - (tan_right + tan_left)) / (tan_right - tan_left),
        (2.f*p.y - (tan_up + tan_down)) / (tan_up - tan_down)};
}

// The area of a triangle (image coordinates) that lies inside the image,
// the square -1..1; the whole image is 4.
inline float ClippedTriangleArea(MaskPoint a, MaskPoint b, MaskPoint c)
{
    // Each of the square's four edges can add at most one point.
    MaskPoint in[8] = {a, b, c}, out[8];
    int count = 3;
    for (int edge = 0; edge < 4 && count; ++edge)
    {
        const auto outside = [edge](const MaskPoint& p)
        {
            const float v = edge == 0 ? p.x : edge == 1 ? -p.x : edge == 2 ? p.y : -p.y;
            return v - 1.f; // > 0 beyond this edge
        };
        int kept = 0;
        for (int i = 0; i < count; ++i)
        {
            const MaskPoint& p = in[i];
            const MaskPoint& q = in[(i + 1) % count];
            const float dp = outside(p), dq = outside(q);
            if (dp <= 0.f) out[kept++] = p;
            if ((dp <= 0.f) != (dq <= 0.f))
            {
                const float t = dp / (dp - dq);
                out[kept++] = {p.x + (q.x - p.x)*t, p.y + (q.y - p.y)*t};
            }
        }
        count = kept;
        for (int i = 0; i < count; ++i) in[i] = out[i];
    }
    float twice = 0.f;
    for (int i = 0; i < count; ++i)
    {
        const MaskPoint& p = in[i];
        const MaskPoint& q = in[(i + 1) % count];
        twice += p.x*q.y - q.x*p.y;
    }
    return (twice < 0.f ? -twice : twice) * 0.5f;
}

// The share (0..1) of the eye image the hidden triangles cover. Indices out
// of range and a degenerate field of view count as nothing hidden.
inline float HiddenImageShare(const MaskPoint* vertices, size_t vertex_count, const uint32_t* indices,
    size_t index_count, float tan_left, float tan_right, float tan_down, float tan_up)
{
    if (!(tan_right > tan_left) || !(tan_up > tan_down) || !vertices || !indices) return 0.f;
    float area = 0.f;
    for (size_t i = 0; i + 2 < index_count; i += 3)
    {
        if (indices[i] >= vertex_count || indices[i + 1] >= vertex_count || indices[i + 2] >= vertex_count) continue;
        area += ClippedTriangleArea(
            MaskPointToImage(vertices[indices[i]], tan_left, tan_right, tan_down, tan_up),
            MaskPointToImage(vertices[indices[i + 1]], tan_left, tan_right, tan_down, tan_up),
            MaskPointToImage(vertices[indices[i + 2]], tan_left, tan_right, tan_down, tan_up));
    }
    const float share = area / 4.f;
    return share < 0.f ? 0.f : share > 1.f ? 1.f : share;
}
}
