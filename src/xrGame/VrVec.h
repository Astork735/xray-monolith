#pragma once
#include <algorithm>
#include <cmath>

// The engine-free 3-vector the VR policy headers and their unit tests share
// (contact, pickup, body slots, moving parts).
namespace vr
{
struct V3
{
    float x=0, y=0, z=0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline V3 operator+(const V3& a, const V3& b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline V3 operator-(const V3& a, const V3& b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline V3 operator*(const V3& a, float s) { return {a.x*s,a.y*s,a.z*s}; }
inline V3 operator-(const V3& a) { return {-a.x,-a.y,-a.z}; }
inline float Dot(const V3& a, const V3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline V3 Cross(const V3& a, const V3& b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float Length(const V3& a) { return std::sqrt(Dot(a,a)); }
inline V3 Lerp(const V3& a, const V3& b, float t) { return a+(b-a)*t; }
inline V3 Min(const V3& a, const V3& b) { return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)}; }
inline V3 Max(const V3& a, const V3& b) { return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)}; }
inline V3 Normalized(const V3& a)
{
    const float l=Length(a);
    return l>1e-12f ? a*(1.f/l) : V3(0,0,0);
}
}
