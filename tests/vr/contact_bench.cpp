// Timing of the geometry-aware contact path against the previous finger
// fitting (the 5 mm point cloud scan from player_hud_legs.cpp). Run the
// Release build; results are medians and 99th percentiles in microseconds.
#include "contact_fixture.h"
#include <chrono>
#include <random>
#include <unordered_set>

using namespace vr;
using namespace fixture;
using Clock=std::chrono::steady_clock;

template<class F> static void Measure(const char* name, int runs, int repeat, F body)
{
    std::vector<double> us;
    us.reserve(runs);
    for (int r=0;r<runs;++r)
    {
        const auto t0=Clock::now();
        for (int i=0;i<repeat;++i) body();
        us.push_back(std::chrono::duration<double,std::micro>(Clock::now()-t0).count()/repeat);
    }
    std::sort(us.begin(),us.end());
    std::printf("  %-44s median %9.3f us   p99 %9.3f us\n",name,us[us.size()/2],us[size_t(us.size()*0.99)]);
}
static volatile float g_sink;

// The previous approach, kept verbatim in spirit: a 5 mm voxel cloud of the
// mesh (capped at 6000 points), the points within 12 cm of the palm, and per
// joint up to ten curl steps whose two segment samples are tested against
// every contact point at 9 mm.
struct CloudBaseline
{
    std::vector<V3> cloud, contact;
    explicit CloudBaseline(const std::vector<Tri>& tris)
    {
        std::unordered_set<uint64_t> seen;
        for (const Tri& t:tris) for (const V3* p:{&t.a,&t.b,&t.c})
        {
            const auto cell=[](float v) { return uint64_t(int64_t(std::floor(v*200.f))+(1<<20))&0x1FFFFF; };
            if (seen.insert(cell(p->x)|(cell(p->y)<<21)|(cell(p->z)<<42)).second) cloud.push_back(*p);
        }
        if (cloud.size()>6000)
        {
            const size_t stride=cloud.size()/6000+1; size_t kept=0;
            for (size_t i=0;i<cloud.size();i+=stride) cloud[kept++]=cloud[i];
            cloud.resize(kept);
        }
    }
    void Hand(const V3& palm, const Finger* fingers, int count)
    {
        contact.clear();
        for (const V3& p:cloud) if (Dot(p-palm,p-palm)<0.12f*0.12f) contact.push_back(p);
        static const float limit[3]={1.6f,1.7f,1.2f};
        for (int f=0;f<count;++f)
        {
            float curl[3]={0,0,0};
            for (int j=0;j<3;++j)
            {
                for (int k=j+1;k<3;++k) curl[k]=0.f;
                for (int step=1;step<=10;++step)
                {
                    curl[j]=limit[j]*0.1f*step;
                    V3 p[4]; fingers[f].Joints(curl,p);
                    const V3 samples[2]={Lerp(p[j],p[j+1],0.5f),p[j+1]};
                    bool hit=false;
                    for (const V3& s:samples)
                    {
                        for (size_t i=0;!hit && i<contact.size();++i)
                            hit=Dot(s-contact[i],s-contact[i])<0.009f*0.009f;
                        if (hit) break;
                    }
                    if (hit) { curl[j]=limit[j]*0.1f*(step-1); break; }
                }
            }
            g_sink=curl[0]+curl[1]+curl[2];
        }
    }
};

int main()
{
    std::printf("VR contact benchmark\n");
    // --- Builds ----------------------------------------------------------
    for (int density:{6,12,20})
        for (int threads:{1,4})
        {
            const std::vector<Tri> rifle=Rifle(density);
            ContactField field;
            const auto t0=Clock::now();
            field.Build(rifle,0.0025f,0.012f,threads);
            const double ms=std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
            std::printf("  rifle build: %6zu tris, %d thread(s)  %7.1f ms  %5zu bricks  %7.1f KB\n",
                rifle.size(),threads,ms,field.Bricks(),field.Bytes()/1024.0);
        }
    {
        const auto t0=Clock::now();
        Pistol pistol(12);
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
        std::printf("  pistol build + carve + path: %zu tris  %.1f ms  %zu bricks  %.1f KB, %zu mag samples\n",
            pistol.receiver.size(),ms,pistol.field.Bricks(),pistol.field.Bytes()/1024.0,pistol.body.samples.size());
    }

    // --- Queries ----------------------------------------------------------
    const std::vector<Tri> rifle=Rifle(20);
    ContactField rifle_field; rifle_field.Build(rifle);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> unit(-1.f,1.f);
    std::vector<V3> points(4096);
    for (V3& p:points) p=V3(unit(rng)*0.04f,unit(rng)*0.1f+0.02f,unit(rng)*0.6f);
    size_t next=0;
    Measure("field query (rifle, near surface)",200,4096,[&]
    { g_sink=rifle_field.Distance(points[next++&4095]); });
    Measure("field gradient",200,4096,[&]
    { g_sink=rifle_field.Gradient(points[next++&4095]).x; });

    // --- Magazine insertion zone ---------------------------------------------
    Pistol pistol(12);
    {
        const InsertZone zone;
        Frame held=pistol.path.At(1.f);
        int frame=0;
        Measure("magazine: insertion zone check",200,1000,[&]
        {
            held.c.y+=0.0001f*std::sin(++frame*0.01f);
            g_sink=float(InsertCheck(pistol.path,held,zone));
        });
    }

    // --- Fingers ----------------------------------------------------------
    // Five fingers wrapping the rifle's grip from beside it.
    Finger fingers[5];
    for (int f=0;f<5;++f)
    {
        fingers[f].base=V3(-0.03f+0.012f*f,-0.03f,-0.135f);
        fingers[f].length[0]=f==0 ? 0.035f : 0.040f;
    }
    ContactScene grip; grip.Add(&rifle_field,Frame());
    float curls[5][3], warm[5][3];
    for (int f=0;f<5;++f) SolveFinger(grip,fingers[f],warm[f],nullptr,false);
    Measure("fingers: one hand, cold",200,200,[&]
    { for (int f=0;f<5;++f) SolveFinger(grip,fingers[f],curls[f],nullptr,false); });
    Measure("fingers: one hand, warm start",200,200,[&]
    { for (int f=0;f<5;++f) SolveFinger(grip,fingers[f],curls[f],warm[f],true); });
    GripCache cache;
    Frame hand; hand.c=V3(0,-0.03f,-0.12f);
    cache.Store(hand,&rifle_field,1.f);
    Measure("fingers: one hand, cached (still grip)",200,1000,[&]
    { g_sink=float(cache.Check(hand,&rifle_field,1.f)); });
    const V3 palm[3]={V3(0,-0.04f,-0.11f),V3(0,-0.06f,-0.11f),V3(0,-0.08f,-0.11f)};
    const float radius[3]={0.02f,0.02f,0.02f};
    Measure("palm push-out (3 spheres)",200,1000,[&]
    { g_sink=PalmPushOut(grip,palm,radius,3).x; });

    CloudBaseline baseline(rifle);
    std::printf("  baseline cloud: %zu points\n",baseline.cloud.size());
    Measure("BASELINE fingers: one hand, point cloud",50,20,[&]
    { baseline.Hand(V3(0,-0.06f,-0.1f),fingers,5); });
    return 0;
}
