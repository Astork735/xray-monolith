#include "../../src/xrEngine/Blood/blood_logic.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static bool Near(float a, float b, float e=1e-4f) { return std::fabs(a-b)<e; }
using namespace blood;

static bool Touches(const PageTouch* t, int n, int axis, int u, int v, int w)
{
    for (int i=0;i<n;++i) if (t[i].key.axis==axis && t[i].key.u==u && t[i].key.v==v && t[i].key.w==w) return true;
    return false;
}

static Splat At(float x, float y, float z, float time=0.f, float radius=0.05f)
{
    Splat s; s.pos={x,y,z}; s.normal={0,1,0}; s.dir={1,0,0}; s.radius=radius; s.amount=0.7f; s.time=time;
    return s;
}

int main()
{
    const float tile=1.f;
    // Page addressing: floors, ceilings and both faces of a wall are apart.
    {
        const Vec p{2.5f,1.2f,-0.5f};
        const PageKey floor=KeyAt(p,AxisPosY,tile), ceiling=KeyAt(p,AxisNegY,tile);
        CHECK(!(floor==ceiling));
        CHECK(floor.u==2 && floor.v==-1 && floor.w==4);
        // A table over a floor is on another page.
        CHECK(!(KeyAt({0.5f,0.8f,0.5f},AxisPosY,tile)==KeyAt({0.5f,0.1f,0.5f},AxisPosY,tile)));
        const PageKey east=KeyAt(p,AxisPosX,tile), west=KeyAt(p,AxisNegX,tile);
        CHECK(!(east==west) && east.u==west.u && east.v==west.v);
        float u,v; PageUv(p,floor,tile,u,v);
        CHECK(Near(u,0.5f) && Near(v,0.5f));
        // Walls: the second texel axis is world height.
        const PageKey wall=KeyAt(p,AxisPosZ,tile);
        PageUv(p,wall,tile,u,v);
        CHECK(Near(v,0.2f));
        PageKeyHash h; CHECK(h(floor)!=h(ceiling));
    }
    // Axis weights: dominant axis wins, 45-degree surfaces share.
    {
        float w[3];
        AxisWeights({0,1,0},w); CHECK(Near(w[1],1.f) && Near(w[0],0.f));
        AxisWeights(Normalize({1,1,0}),w); CHECK(Near(w[0],0.5f,1e-3f) && Near(w[1],0.5f,1e-3f));
        AxisWeights(Normalize({0.2f,1,0}),w); CHECK(w[1]>0.99f);
    }
    // A splat touches every tile its footprint overlaps, on its face only.
    {
        PageTouch t[16];
        int n=PagesForSplat({0.98f,0.6f,0.5f},{0,1,0},0.05f,tile,t,16);
        CHECK(n==2 && Touches(t,n,AxisPosY,0,0,1) && Touches(t,n,AxisPosY,1,0,1));
        n=PagesForSplat({0.98f,0.6f,0.99f},{0,1,0},0.05f,tile,t,16);
        CHECK(n==4);
        // Floors at whole, half and quarter metres sit inside one slab: one
        // page, drawn once.
        for (float y : {0.f, 0.25f, 0.5f, 0.75f, 1.f, 2.f, -1.f})
            CHECK(PagesForSplat({0.5f,y,0.5f},{0,1,0},0.05f,tile,t,16)==1);
        // A floor on a slab boundary is written to both slabs, so either
        // page draws the same stain.
        const float boundary=SlabOrigin(tile);
        n=PagesForSplat({0.5f,boundary,0.5f},{0,1,0},0.05f,tile,t,16);
        CHECK(n==2 && Touches(t,n,AxisPosY,0,0,0) && Touches(t,n,AxisPosY,0,0,-1));
        // Ceiling normal: negative Y pages.
        n=PagesForSplat({0.5f,2.9f,0.5f},{0,-1,0},0.05f,tile,t,16);
        CHECK(n==1 && t[0].key.axis==AxisNegY);
        // A 45-degree ramp writes to both main axes.
        n=PagesForSplat({0.5f,0.5f,0.5f},Normalize({1,1,0}),0.05f,tile,t,16);
        bool x=false,y=false; for (int i=0;i<n;++i){x|=t[i].key.axis==AxisPosX;y|=t[i].key.axis==AxisPosY;}
        CHECK(x && y);
        // Output is bounded.
        CHECK(PagesForSplat({0.5f,0.5f,0.5f},Normalize({1,1,1}),3.f,tile,t,16)==16);
    }
    // Shapes: head-on drops are round, grazing fast drops stretch along
    // their travel, large volumes on floors pool, walls never pool.
    {
        const Splat round=ShapeFromImpact({0,0,0},{0,1,0},{0,-5,0},0.5f,1,0.f);
        CHECK(Near(round.elongation,1.f,1e-3f));
        const Splat graze=ShapeFromImpact({0,0,0},{0,1,0},{6,-1,0},0.5f,1,0.f);
        CHECK(graze.elongation>2.f && graze.dir.x>0.99f && graze.kind==SplatSplash);
        CHECK(graze.radius<round.radius*1.6f);
        const Splat pool=ShapeFromImpact({0,0,0},{0,1,0},{0,-1,0},2.f,1,0.f);
        CHECK(pool.kind==SplatPool && pool.amount==1.f);
        // Ordinary spatter hitting a wall behind the victim splashes.
        CHECK(ShapeFromImpact({0,0,0},{1,0,0},{-4,0,0},0.1f,1,0.f).kind==SplatSplash);
        const Splat wall=ShapeFromImpact({0,0,0},{1,0,0},{-1,0,0},2.f,1,0.f);
        CHECK(wall.kind!=SplatPool);
        const Splat drop=ShapeFromImpact({0,0,0},{0,1,0},{0,-1,0},0.05f,1,0.f);
        CHECK(drop.kind==SplatDroplet && drop.radius>=0.004f);
        // Stamp quad: centred on the splat, major axis along travel.
        PageKey k=KeyAt({0.5f,0,0.5f},AxisPosY,tile);
        Splat s=graze; s.pos={0.5f,0,0.5f};
        const StampQuad q=QuadInPage(s,k,tile);
        CHECK(Near(q.cu,0.5f) && Near(q.cv,0.5f) && q.au>0.f && Near(q.av,0.f) && std::fabs(q.au)>std::fabs(q.bv));
    }
    // Store: deposits, merging, extent along the axis, residency and LRU.
    {
        StoreLimits l; l.slots=2; l.records=3; l.log_capacity=4;
        PageStore st(l);
        const Vec cam{0,0,0};
        int ev=-1;
        const PageKey a=KeyAt({0.5f,0,0.5f},AxisPosY,tile), b=KeyAt({1.5f,0,0.5f},AxisPosY,tile),
            c=KeyAt({2.5f,0,0.5f},AxisPosY,tile), d=KeyAt({9.5f,0,0.5f},AxisPosY,tile);
        const int ia=st.Deposit(a,At(0.5f,0.f,0.5f,1.f),tile,cam,ev);
        CHECK(ia>=0 && ev==-1 && st.Live()==1);
        // A second hit on the same spot thickens it instead of growing the log.
        st.Deposit(a,At(0.51f,0.f,0.5f,2.f),tile,cam,ev);
        CHECK(st.Record(ia).log.size()==1 && st.Record(ia).log[0].amount>0.7f && Near(st.Record(ia).last_deposit,2.f));
        // ...and grows it: drips on one spot become a small pool.
        CHECK(st.Record(ia).log[0].radius>0.05f*1.2f);
        for (int i=0;i<200;++i) st.Deposit(a,At(0.5f,0.f,0.5f,2.f),tile,cam,ev);
        CHECK(st.Record(ia).log[0].radius<=ShapeParams().max_radius+1e-5f);
        // A pool request carries its own accumulated size: no extra growth.
        {
            PageStore pools;
            Splat p=At(0.5f,0.f,0.5f,1.f,0.2f); p.kind=SplatPool;
            const int ip=pools.Deposit(a,p,tile,cam,ev);
            pools.Deposit(a,p,tile,cam,ev);
            CHECK(Near(pools.Record(ip).log[0].radius,0.2f));
        }
        // Log overflow coalesces the oldest splat.
        for (int i=0;i<6;++i) st.Deposit(a,At(0.1f+0.12f*i,0.f,0.2f,3.f+i),tile,cam,ev);
        CHECK(int(st.Record(ia).log.size())==4);
        // Extent: flat floor stains stay at the floor height.
        CHECK(Near(st.Record(ia).lo,0.f) && Near(st.Record(ia).hi,0.f));

        const int ib=st.Deposit(b,At(1.5f,0.f,0.5f,1.f),tile,cam,ev);
        const int ic=st.Deposit(c,At(2.5f,0.f,0.5f,1.f),tile,cam,ev);
        bool fresh=false;
        CHECK(st.Acquire(ia,1,cam,tile,fresh)>=0 && fresh);
        CHECK(st.Acquire(ia,1,cam,tile,fresh)>=0 && !fresh);
        CHECK(st.Acquire(ib,1,cam,tile,fresh)>=0 && fresh);
        // Both slots drawn this frame: no room.
        CHECK(st.Acquire(ic,1,cam,tile,fresh)==-1);
        // Next frame, the farther page gives up its slot; its log stays.
        st.Acquire(ia,2,cam,tile,fresh);
        const int slot=st.Acquire(ic,2,cam,tile,fresh);
        CHECK(slot>=0 && fresh && st.Record(ib).slot==-1 && !st.Record(ib).log.empty());
        // Coming back restores it from its log.
        CHECK(st.Acquire(ib,3,cam,tile,fresh)>=0 && fresh);
        // Record limit: the farthest, oldest page is forgotten (slot freed).
        const int keep_slot=st.Record(ib).slot;
        const int id=st.Deposit(d,At(9.5f,0.f,0.5f,5.f),tile,{2.5f,0,0.5f},ev);
        CHECK(st.Live()==3 && st.Find(d)==id);
        CHECK(st.Find(a)<0 || st.Find(b)<0 || st.Find(c)<0);
        (void)keep_slot;
        // Release keeps logs, drops residency.
        st.ReleaseSlots(4);
        for (int i=0;i<st.RecordCount();++i) CHECK(st.Record(i).slot==-1);
    }
    // The record limit holds however late in the session: a large deposit
    // time must not put every page beyond eviction.
    {
        StoreLimits l; l.slots=2; l.records=2; l.log_capacity=4;
        PageStore st(l);
        int ev=-1;
        for (int i=0;i<8;++i)
            st.Deposit(KeyAt({i+0.5f,0,0.5f},AxisPosY,tile),At(i+0.5f,0.f,0.5f,100000.f+i),tile,{0,0,0},ev);
        CHECK(st.Live()==2);
    }
    // Tilted stains extend their box along the axis.
    {
        PageStore st;
        int ev;
        Splat s=At(0.5f,1.f,0.5f,0.f,0.1f); s.normal=Normalize({0.5f,1,0});
        const int id=st.Deposit(KeyAt(s.pos,AxisPosY,tile),s,tile,{0,0,0},ev);
        CHECK(st.Record(id).lo<0.99f && st.Record(id).hi>1.01f);
    }
    // Hit filter: one spray per wound per frame.
    {
        HitFilter f;
        CHECK(f.Accept(7,3,10,{0,0,0}));
        CHECK(!f.Accept(7,3,10,{0,0.1f,0}));
        CHECK(!f.Accept(7,4,10,{0,0,0.01f}));
        CHECK(f.Accept(7,4,10,{0,0.2f,0}));
        CHECK(f.Accept(7,3,11,{0,0,0}));
        CHECK(f.Accept(8,3,10,{0,0,0}));
    }
    // Backoff steps down after sustained overload, back up after recovery.
    {
        Backoff b;
        for (int i=0;i<200;++i) b.Push(3.f,1.f);
        CHECK(b.level>=2 && b.Scale()<0.5f);
        for (int i=0;i<2000;++i) b.Push(0.1f,1.f);
        CHECK(b.level==0 && Near(b.Scale(),1.f));
        // Near the budget nothing changes.
        Backoff c;
        for (int i=0;i<500;++i) c.Push(0.8f,1.f);
        CHECK(c.level==0);
        // No budget: never steps.
        Backoff d;
        for (int i=0;i<500;++i) d.Push(9.f,0.f);
        CHECK(d.level==0);
    }
    // Quality presets grow monotonically and clamp.
    {
        for (int i=1;i<4;++i) CHECK(QualityPreset(i).particles>QualityPreset(i-1).particles);
        CHECK(QualityPreset(-3).particles==QualityPreset(0).particles);
        CHECK(QualityPreset(9).page_size==QualityPreset(3).page_size);
        CHECK(!QualityPreset(0).fluid && QualityPreset(1).fluid);
    }
    // Ballistic arc: a drop thrown sideways from 1 m lands on the floor
    // after about the free-fall time, ahead of its start, moving down.
    {
        const auto floor_ray=[](const Vec& o,const Vec& d,float len,float& dist,Vec& n,int& user)
        {
            if (d.y>=0.f) return false;
            const float t=-o.y/d.y;
            if (t<0.f || t>len) return false;
            dist=t; n={0,1,0}; user=2; return true;
        };
        const Contact c=TraceArc({0,1,0},{2,0,0},9.81f,0.f,1.5f,8,floor_ray);
        CHECK(c.hit && c.user==2 && Near(c.point.y,0.f,1e-3f));
        CHECK(Near(c.time,std::sqrt(2.f/9.81f),0.03f));
        CHECK(c.point.x>0.8f && c.point.x<1.0f && c.velocity.y<-3.f);
        // Drag shortens the throw.
        const Contact dragged=TraceArc({0,1,0},{2,0,0},9.81f,0.5f,1.5f,8,floor_ray);
        CHECK(dragged.hit && dragged.point.x<c.point.x);
        // Nothing in reach: no contact, followed for the full time.
        const auto none=[](const Vec&,const Vec&,float,float&,Vec&,int&){return false;};
        const Contact miss=TraceArc({0,1,0},{1,0,0},9.81f,0.f,1.f,4,none);
        CHECK(!miss.hit && Near(miss.time,1.f));
    }
    std::puts("blood tests passed");
    return 0;
}
