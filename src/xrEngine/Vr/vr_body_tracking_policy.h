#pragma once
#include <algorithm>
#include <cmath>

// Meta body tracking (XR_FB_body_tracking) and seated reach, engine-free so
// they are tested. The controllers stay where the hands are; the tracked
// body only bends the elbows, moves the shoulders, faces the torso and
// measures the player. Positions are in one frame with +y up, +x right and
// +z forward (the engine's), in metres unless a function says otherwise.
namespace vr
{
struct BodyVec
{
    float x=0.f, y=0.f, z=0.f;
    BodyVec() {}
    BodyVec(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
inline BodyVec operator-(const BodyVec& a, const BodyVec& b) { return BodyVec(a.x-b.x,a.y-b.y,a.z-b.z); }
inline BodyVec operator+(const BodyVec& a, const BodyVec& b) { return BodyVec(a.x+b.x,a.y+b.y,a.z+b.z); }
inline BodyVec operator*(const BodyVec& a, float s) { return BodyVec(a.x*s,a.y*s,a.z*s); }
inline float BodyDot(const BodyVec& a, const BodyVec& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float BodyLength(const BodyVec& a) { return std::sqrt(BodyDot(a,a)); }

// How far the tracked body is trusted (0..1): toward 1 while it is tracked,
// toward 0 once it is lost, over `seconds` either way, so the arms and the
// torso never jump between it and the headset's estimate.
inline float TrackingWeight(float weight, bool valid, float dt, float seconds=0.2f)
{
    if (!(dt>0.f) || !(seconds>0.f)) return std::min(std::max(weight,0.f),1.f);
    const float step=std::min(dt,0.1f)/seconds;
    weight+=valid ? step : -step;
    return std::min(std::max(weight,0.f),1.f);
}

// The way the tracked elbow bends out of the line from the shoulder to the
// wrist (unit). False when the arm is about straight (the elbow within
// `min_bend` of that line) or the line is too short: the caller keeps its
// own pole then.
inline bool ElbowPole(const BodyVec& shoulder, const BodyVec& wrist, const BodyVec& elbow, BodyVec& pole,
    float min_bend=0.03f)
{
    BodyVec line=wrist-shoulder;
    const float length=BodyLength(line);
    if (!(length>0.05f)) return false;
    line=line*(1.f/length);
    BodyVec side=elbow-shoulder;
    side=side-line*BodyDot(side,line);
    const float bend=BodyLength(side);
    if (!(bend>=min_bend)) return false;
    pole=side*(1.f/bend);
    return true;
}

// The torso's yaw (radians about +y; 0 faces +z, +pi/2 faces +x) from the
// two shoulders: it faces square to the line between them. False when that
// line is nearly vertical (lying on a side) or the shoulders coincide.
inline bool ChestYaw(const BodyVec& left, const BodyVec& right, float& yaw)
{
    const float ax=right.x-left.x, az=right.z-left.z;
    if (!(ax*ax+az*az>0.01f)) return false;
    // Right is up x forward: forward is (-across.z, 0, across.x).
    yaw=std::atan2(-az,ax);
    return std::isfinite(yaw);
}

// Yaw `weight` of the way from a to b, the short way round, in (-pi, pi].
inline float BlendAngle(float a, float b, float weight)
{
    const float pi=3.14159265f;
    const float d=std::remainder(b-a,2.f*pi);
    float out=std::remainder(a+d*weight,2.f*pi);
    return out<=-pi ? out+2.f*pi : out;
}

// The player's shoulder width (between the shoulder joints) and arm length
// (shoulder to wrist), the median of samples taken ten times a second over
// the last six seconds. Samples outside a person's range (a tracking slip)
// are dropped. Settled after three seconds of samples.
class BodySizeSampler
{
public:
    static constexpr int capacity=60, settled_count=30;
    static constexpr float min_width=0.20f, max_width=0.60f, min_arm=0.40f, max_arm=0.90f;
    void Reset() { m_count=m_next=0; m_since=1.f; }
    // dt since the last call; returns whether the sample was taken.
    bool Add(float width, float arm, float dt)
    {
        m_since+=std::max(dt,0.f);
        if (m_since<0.1f) return false;
        if (!(width>=min_width && width<=max_width && arm>=min_arm && arm<=max_arm)) return false;
        m_since=0.f;
        m_width[m_next]=width; m_arm[m_next]=arm;
        m_next=(m_next+1)%capacity;
        m_count=std::min(m_count+1,capacity);
        return true;
    }
    bool Settled() const { return m_count>=settled_count; }
    float Width() const { return Median(m_width); }
    float Arm() const { return Median(m_arm); }
    int Count() const { return m_count; }
private:
    float Median(const float* values) const
    {
        if (!m_count) return 0.f;
        float sorted[capacity];
        std::copy(values,values+m_count,sorted);
        std::nth_element(sorted,sorted+m_count/2,sorted+m_count);
        return sorted[m_count/2];
    }
    float m_width[capacity]{}, m_arm[capacity]{};
    int m_count=0, m_next=0;
    float m_since=1.f;
};

// The measured size against the average one, as a factor for offsets set
// for an average body: no more than 15% smaller or 20% larger.
inline float BodySizeFactor(float measured, float average)
{
    if (!(measured>0.f) || !(average>0.f)) return 1.f;
    return std::min(std::max(measured/average,0.85f),1.20f);
}
constexpr float average_shoulder_width=0.36f; // between the shoulder joints
constexpr float average_arm_length=0.60f;     // shoulder joint to wrist

// Seated reach. Sitting, the real floor is nearer the eyes than the game's
// floor is below the actor's eyes. A hand going down past `start` toward
// the real floor goes down faster in the game, so it reaches the game's
// floor just as it reaches the real one. Above `start` nothing changes, so
// nothing jumps. Heights are in one vertical frame; returns what to add to
// the hand's height (0 or less).
inline float SeatedReachDrop(float hand_y, float real_floor, float game_floor, float start)
{
    const float gap=real_floor-game_floor;
    if (!(gap>0.f) || !(start>real_floor+0.05f) || !(hand_y<start)) return 0.f;
    const float t=std::min((start-hand_y)/(start-real_floor),1.f);
    return -gap*t;
}
// Where seated reach starts: about seat height (45 cm above the real floor),
// and always 10 cm under the hips (`head_y` less `hip_height`), so the hip
// holsters, the pouch and the belt are reached as before.
inline float SeatedReachStart(float real_floor, float head_y, float hip_height)
{
    return std::min(real_floor+0.45f,head_y-hip_height-0.10f);
}
// Seated reach applies (vr_seated_reach): 1 when the real floor is more than
// 25 cm nearer than the game's (seated, or a short player), 2 whenever it is
// nearer at all; 0 never.
inline bool SeatedReachApplies(int mode, float real_floor, float game_floor)
{
    const float gap=real_floor-game_floor;
    if (mode==2) return gap>0.02f;
    if (mode==1) return gap>0.25f;
    return false;
}
}
