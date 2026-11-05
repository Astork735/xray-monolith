#pragma once
// A manual action (a bolt-action's bolt, a lever) worked along the path its
// own animation moves it: the grab point (the bolt knob, the lever tip)
// sampled over the shot animation, re-parameterised by arc length (u, 0
// closed to 1 fully worked). A hand drags the knob along that path: a
// Mosin's handle lifts through its 90-degree turn before the bolt slides
// back. Each key also knows how far back the bolt is (its rear travel), so
// a turn phase extracts and feeds nothing. Engine-free (unit tested).
#include "VrContactField.h"
#include <vector>
#include <algorithm>

namespace vr
{
struct ActionPath
{
    std::vector<V3> grab;      // grab point per key, weapon space
    std::vector<float> rear;   // bolt rear travel per key (m)
    std::vector<float> arc;    // cumulative arc length per key
    float length = 0.f;

    bool Valid() const { return grab.size() >= 2 && length > 1e-4f; }
    // Keys in order from closed to fully worked; the caller trims the
    // animation to that span. Every key is kept (a key that does not move
    // the knob is a zero-length step), so per-key data stays aligned.
    void Build(const std::vector<V3>& points, const std::vector<float>& rears)
    {
        grab.clear(); rear.clear(); arc.clear(); length = 0.f;
        for (size_t i = 0; i < points.size() && i < rears.size(); ++i)
        {
            if (!grab.empty()) length += Length(points[i] - grab.back());
            grab.push_back(points[i]); rear.push_back(rears[i]); arc.push_back(length);
        }
    }
    // The key segment holding u and the fraction along it.
    void Locate(float u, size_t& index, float& t) const
    {
        index = 0; t = 0.f;
        if (!Valid()) return;
        const float s = std::max(0.f, std::min(1.f, u))*length;
        while (index + 2 < arc.size() && arc[index + 1] < s) ++index;
        const float span = arc[index + 1] - arc[index];
        t = span > 1e-6f ? std::max(0.f, std::min(1.f, (s - arc[index])/span)) : 0.f;
    }
    V3 GrabAt(float u) const
    {
        size_t i; float t; Locate(u, i, t);
        return Valid() ? grab[i] + (grab[i + 1] - grab[i])*t : V3();
    }
    float RearAt(float u) const
    {
        size_t i; float t; Locate(u, i, t);
        return Valid() ? rear[i] + (rear[i + 1] - rear[i])*t : 0.f;
    }
    float RearMax() const { return rear.empty() ? 0.f : *std::max_element(rear.begin(), rear.end()); }
    // Where the turn ends: the first u at which the bolt has come back 3 mm.
    float TurnEnd() const
    {
        for (size_t i = 0; i < rear.size(); ++i)
            if (rear[i] >= 0.003f) return length > 0.f ? arc[i]/length : 0.f;
        return 0.f;
    }
    // The hand (weapon space) dragged along the path from current: only a
    // stretch of `window` arc length either side is searched, so a hand
    // cannot jump the knob from closed to fully back through the receiver.
    float Project(const V3& hand, float current, float window = 0.06f) const
    {
        if (!Valid()) return 0.f;
        const float from = std::max(0.f, current*length - window), to = std::min(length, current*length + window);
        float best_s = current*length, best_d = 1e30f;
        for (size_t i = 0; i + 1 < grab.size(); ++i)
        {
            if (arc[i + 1] < from || arc[i] > to) continue;
            const V3 a = grab[i], d = grab[i + 1] - grab[i];
            const float dd = Dot(d, d);
            float t = dd > 1e-12f ? Dot(hand - a, d)/dd : 0.f;
            t = std::max(0.f, std::min(1.f, t));
            const float s = std::max(from, std::min(to, arc[i] + t*(arc[i + 1] - arc[i])));
            const float dist = Length(GrabAt(s/length) - hand);
            if (dist < best_d) { best_d = dist; best_s = s; }
        }
        return best_s/length;
    }
};
}
