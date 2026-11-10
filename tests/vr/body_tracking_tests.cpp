#include "../../src/xrEngine/Vr/vr_body_tracking_policy.h"
#include "check.h"
using namespace vr;

static void Weight()
{
    // Eases in over 0.2 s, out the same, and stays within 0..1.
    float w=0.f;
    w=TrackingWeight(w,true,0.1f);
    CHECK(Near(w,0.5f));
    w=TrackingWeight(w,true,0.1f);
    CHECK(Near(w,1.f));
    w=TrackingWeight(w,true,0.1f);
    CHECK(Near(w,1.f));
    w=TrackingWeight(w,false,0.05f);
    CHECK(Near(w,0.75f));
    // A long frame (a hitch) moves it no more than 0.1 s does.
    CHECK(Near(TrackingWeight(0.f,true,1.f),0.5f));
    CHECK(Near(TrackingWeight(0.3f,false,0.f),0.3f));
    CHECK(Near(TrackingWeight(0.f,false,0.1f),0.f));
}

static void Elbow()
{
    BodyVec pole;
    // Arm reaching forward, elbow dropped and out to the right: the pole
    // is the elbow's offset off the shoulder-wrist line.
    CHECK(ElbowPole(BodyVec(0,0,0),BodyVec(0,0,0.5f),BodyVec(0.1f,-0.1f,0.25f),pole));
    CHECK(Near(pole.z,0.f) && Near(pole.x,0.7071f,1e-3f) && Near(pole.y,-0.7071f,1e-3f));
    CHECK(Near(BodyLength(pole),1.f));
    // Straight arm: none (the caller keeps its own).
    CHECK(!ElbowPole(BodyVec(0,0,0),BodyVec(0,0,0.5f),BodyVec(0.01f,0.f,0.25f),pole));
    // Wrist at the shoulder: none.
    CHECK(!ElbowPole(BodyVec(0,0,0),BodyVec(0,0,0.01f),BodyVec(0.1f,-0.2f,0.f),pole));
}

static void Chest()
{
    float yaw=0.f;
    // Facing +z: right shoulder at +x.
    CHECK(ChestYaw(BodyVec(-0.18f,1.4f,0),BodyVec(0.18f,1.4f,0),yaw) && Near(yaw,0.f));
    // Facing +x (yaw +90): right shoulder toward -z.
    CHECK(ChestYaw(BodyVec(0,1.4f,0.18f),BodyVec(0,1.4f,-0.18f),yaw) && Near(yaw,1.5708f,1e-3f));
    // Facing -x (yaw -90).
    CHECK(ChestYaw(BodyVec(0,1.4f,-0.18f),BodyVec(0,1.4f,0.18f),yaw) && Near(yaw,-1.5708f,1e-3f));
    // Facing -z (yaw 180).
    CHECK(ChestYaw(BodyVec(0.18f,1.4f,0),BodyVec(-0.18f,1.4f,0),yaw) && Near(std::fabs(yaw),3.14159f,1e-3f));
    // Lying on a side: the shoulders one above the other.
    CHECK(!ChestYaw(BodyVec(0,1.0f,0),BodyVec(0.02f,1.36f,0),yaw));
    // Blending the short way round.
    CHECK(Near(std::fabs(BlendAngle(3.0f,-3.0f,0.5f)),3.14159f,2e-3f));
    CHECK(Near(BlendAngle(0.f,1.f,0.25f),0.25f));
    CHECK(Near(BlendAngle(0.4f,1.f,1.f),1.f));
}

static void Size()
{
    BodySizeSampler sampler;
    sampler.Reset();
    // Ten samples a second at most.
    CHECK(sampler.Add(0.36f,0.60f,0.f));
    CHECK(!sampler.Add(0.36f,0.60f,0.05f));
    CHECK(sampler.Add(0.36f,0.60f,0.05f));
    // Outside a person's range: dropped.
    CHECK(!sampler.Add(0.9f,0.60f,0.2f));
    CHECK(!sampler.Add(0.36f,0.2f,0.2f));
    CHECK(sampler.Count()==2 && !sampler.Settled());
    // Settles after three seconds of samples; the median ignores a few slips.
    for (int i=0;i<40;++i) sampler.Add(i%10==0 ? 0.55f : 0.38f, i%10==0 ? 0.85f : 0.62f, 0.1f);
    CHECK(sampler.Settled());
    CHECK(Near(sampler.Width(),0.38f) && Near(sampler.Arm(),0.62f));
    // Keeps the last six seconds: a new wearer replaces the old.
    for (int i=0;i<BodySizeSampler::capacity;++i) sampler.Add(0.30f,0.52f,0.1f);
    CHECK(Near(sampler.Width(),0.30f) && Near(sampler.Arm(),0.52f));
    // The factor for average-sized places: limited to 0.85..1.2, 1 unknown.
    CHECK(Near(BodySizeFactor(0.36f,average_shoulder_width),1.f));
    CHECK(Near(BodySizeFactor(0.20f,average_shoulder_width),0.85f));
    CHECK(Near(BodySizeFactor(0.60f,average_shoulder_width),1.2f));
    CHECK(Near(BodySizeFactor(0.f,average_shoulder_width),1.f));
}

static void Seated()
{
    // Seated: eyes 1.2 m above the real floor, the actor's 1.65 m above the game's.
    const float real=-1.2f, game=-1.65f, hip=0.65f;
    CHECK(SeatedReachApplies(1,real,game) && SeatedReachApplies(2,real,game) && !SeatedReachApplies(0,real,game));
    // Standing: no gap (or the real floor further).
    CHECK(!SeatedReachApplies(1,-1.62f,game) && !SeatedReachApplies(2,-1.7f,game));
    // A short player standing: only "always".
    CHECK(!SeatedReachApplies(1,-1.5f,game) && SeatedReachApplies(2,-1.5f,game));
    const float start=SeatedReachStart(real,0.f,hip);
    CHECK(Near(start,-0.75f));
    // Never above 10 cm under the hips, even with the head down low.
    CHECK(SeatedReachStart(real,-0.3f,hip)<=-0.3f-hip-0.10f+1e-5f);
    // Above the start: unchanged; at it: still (no jump).
    CHECK(SeatedReachDrop(-0.5f,real,game,start)==0.f);
    CHECK(Near(SeatedReachDrop(start,real,game,start),0.f));
    CHECK(Near(SeatedReachDrop(start-0.001f,real,game,start),0.f,0.01f));
    // The hand at the real floor is at the game's.
    CHECK(Near(real+SeatedReachDrop(real,real,game,start),game));
    // Halfway down, halfway taken.
    const float half=0.5f*(start+real);
    CHECK(Near(SeatedReachDrop(half,real,game,start),-0.5f*(real-game)));
    // Below the real floor (tracking noise): no further.
    CHECK(Near(SeatedReachDrop(real-0.1f,real,game,start),game-real));
    // Down steadily all the way: monotonic.
    float last=1.f;
    for (float y=start;y>=real;y-=0.01f)
    {
        const float mapped=y+SeatedReachDrop(y,real,game,start);
        CHECK(mapped<=last+1e-5f);
        last=mapped;
    }
    // No gap, or the real floor further than the game's: nothing.
    CHECK(SeatedReachDrop(-1.5f,-1.65f,game,-1.2f)==0.f);
    CHECK(SeatedReachDrop(-1.5f,-1.8f,game,-1.3f)==0.f);
}

int main()
{
    Weight();
    Elbow();
    Chest();
    Size();
    Seated();
    std::printf("body tracking policy: ok\n");
    return 0;
}
