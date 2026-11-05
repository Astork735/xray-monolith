#include "../../src/xrEngine/Blood/blood_logic.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "check.h"
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
        // Log overflow coalesces the oldest splat. (Splashes: drops would
        // fall into the pool that has grown over the page.)
        for (int i=0;i<6;++i)
        {
            Splat sp=At(0.1f+0.12f*i,0.f,0.2f,3.f+i); sp.kind=SplatSplash;
            st.Deposit(a,sp,tile,cam,ev);
        }
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
    // Forced kinds: a pool asked for by the game needs no volume threshold
    // (scaling volumes down cannot remove pools), but only pools on floors.
    {
        ShapeHint pool; pool.kind=SplatPool;
        const Splat small=ShapeFromImpact({0,0,0},{0,1,0},{0,-1,0},0.3f,1,0.f,pool);
        CHECK(small.kind==SplatPool && small.amount==1.f);
        CHECK(ShapeFromImpact({0,0,0},{1,0,0},{-1,0,0},0.3f,1,0.f,pool).kind!=SplatPool);
        // A forced droplet never splashes.
        ShapeHint drop; drop.kind=SplatDroplet;
        CHECK(ShapeFromImpact({0,0,0},{1,0,0},{-5,0,0},0.5f,1,0.f,drop).kind==SplatDroplet);
        // Runs and smears take their size from the hint.
        ShapeHint run; run.kind=SplatRun; run.radius=0.003f; run.length=0.05f; run.flags=SplatBead; run.group=9;
        const Splat r=ShapeFromImpact({0,0,0},{1,0,0},{0,-1,0},0.3f,1,0.f,run);
        CHECK(r.kind==SplatRun && Near(r.radius,0.003f) && Near(2.f*r.radius*r.elongation,0.05f,1e-4f));
        CHECK(r.dir.y<-0.99f && r.flags==SplatBead && r.group==9 && r.amount<kWallMobile);
        ShapeHint mist; mist.kind=SplatMist; mist.radius=0.2f;
        const Splat m=ShapeFromImpact({0,0,0},{1,0,0},{-5,0,0},0.1f,1,0.f,mist);
        CHECK(m.kind==SplatMist && Near(m.radius,0.2f) && m.amount<kWallMobile);
    }
    // Thickness follows volume: specks and small drops stay below the wall
    // threshold and dry where they land; large deposits run.
    {
        const Splat speck=ShapeFromImpact({0,0,0},{1,0,0},{-1,0,0},0.01f,1,0.f);
        const Splat drop=ShapeFromImpact({0,0,0},{1,0,0},{-1,0,0},0.05f,1,0.f);
        const Splat big=ShapeFromImpact({0,0,0},{1,0,0},{-1,0,0},0.5f,1,0.f);
        CHECK(speck.amount<drop.amount && drop.amount<big.amount);
        CHECK(drop.amount<kWallMobile && big.amount>kWallMobile);
    }
    // Stamp extents: the quad holds the kind's whole shape.
    {
        PageKey k=KeyAt({0.5f,0,0.5f},AxisPosY,tile);
        Splat s=At(0.5f,0.f,0.5f,0.f,0.1f); s.kind=SplatSplash; s.elongation=2.f;
        StampQuad q=QuadInPage(s,k,tile);
        CHECK(Near(q.au,MajorExtent(s)/tile) && Near(MajorExtent(s),0.1f*2.f*1.8f));
        s.kind=SplatRun; s.radius=0.003f; s.elongation=10.f;
        q=QuadInPage(s,k,tile);
        CHECK(Near(q.au,0.003f*12.f/tile) && Near(q.bv,0.006f/tile));
    }
    // Drops falling into a pool grow it; a smear never merges.
    {
        PageStore st;
        int ev;
        const PageKey k=KeyAt({0.5f,0,0.5f},AxisPosY,tile);
        Splat pool=At(0.5f,0.f,0.5f,0.f,0.1f); pool.kind=SplatPool;
        const int id=st.Deposit(k,pool,tile,{0,0,0},ev);
        st.Deposit(k,At(0.56f,0.f,0.5f,1.f,0.02f),tile,{0,0,0},ev);
        CHECK(st.Record(id).log.size()==1 && st.Record(id).log[0].radius>0.1f);
        Splat smear=At(0.5f,0.f,0.5f,2.f,0.05f); smear.kind=SplatSmear;
        st.Deposit(k,smear,tile,{0,0,0},ev);
        st.Deposit(k,smear,tile,{0,0,0},ev);
        CHECK(st.Record(id).log.size()==3);
        // On a wall, drips on one spot thicken but do not turn into a pool.
        Splat w=At(0.5f,0.5f,0.5f,0.f,0.06f); w.normal={1,0,0};
        const PageKey wk=KeyAt(w.pos,AxisPosX,tile);
        const int wid=st.Deposit(wk,w,tile,{0,0,0},ev);
        st.Deposit(wk,w,tile,{0,0,0},ev);
        CHECK(st.Record(wid).log[0].kind==SplatDroplet);
    }
    // Run segments of one run log as one splat spanning them; another run
    // stays apart.
    {
        PageStore st;
        int ev;
        Splat a=At(0.5f,0.8f,0.5f,1.f,0.003f); a.normal={1,0,0}; a.dir={0,-1,0}; a.kind=SplatRun; a.group=5;
        a.elongation=0.025f/0.003f;
        Splat b=a; b.pos.y=0.75f; b.flags=SplatBead; b.time=2.f;
        const PageKey k=KeyAt(a.pos,AxisPosX,tile);
        const int id=st.Deposit(k,a,tile,{0,0,0},ev);
        st.Deposit(k,b,tile,{0,0,0},ev);
        const Splat& r=st.Record(id).log[0];
        CHECK(st.Record(id).log.size()==1 && Near(r.pos.y,0.775f,1e-4f));
        CHECK(Near(2.f*r.radius*r.elongation,0.1f,1e-4f) && r.flags==SplatBead && Near(r.time,2.f));
        Splat c=a; c.group=6;
        st.Deposit(k,c,tile,{0,0,0},ev);
        CHECK(st.Record(id).log.size()==2);
    }
    // Coalescing keeps the shape of the larger stain: a pool stays a pool.
    {
        StoreLimits l; l.log_capacity=2;
        PageStore st(l);
        int ev;
        const PageKey k=KeyAt({0.5f,0,0.5f},AxisPosY,tile);
        Splat pool=At(0.3f,0.f,0.3f,0.f,0.2f); pool.kind=SplatPool;
        Splat mist=At(0.7f,0.f,0.7f,1.f,0.02f); mist.kind=SplatMist;
        Splat later=At(0.9f,0.f,0.1f,2.f,0.02f); later.kind=SplatSplash;
        const int id=st.Deposit(k,pool,tile,{0,0,0},ev);
        st.Deposit(k,mist,tile,{0,0,0},ev);
        st.Deposit(k,later,tile,{0,0,0},ev);
        bool has_pool=false;
        for (const Splat& s : st.Record(id).log) has_pool|=s.kind==SplatPool;
        CHECK(st.Record(id).log.size()==2 && has_pool);
    }
    // Runs: down a wall to the floor, off a ledge, or as far as the blood goes.
    {
        // Wall x = 0 facing +x, floor at y = 0.
        const auto room=[](const Vec& o,const Vec& d,float len,float& dist,Vec& n,int& user)
        {
            float best=1e9f; Vec bn;
            if (d.x<-1e-6f){ const float t=-o.x/d.x; if (t>=0 && t<=len && t<best){best=t;bn={1,0,0};} }
            if (d.y<-1e-6f){ const float t=-o.y/d.y; if (t>=0 && t<=len && t<best){best=t;bn={0,1,0};} }
            if (best>len) return false;
            dist=best; n=bn; user=0; return true;
        };
        const RunPlan floor=PlanRun({0,0.2f,0},{1,0,0},0.6f,room);
        CHECK(floor.end==RunPlan::Floor && Near(floor.end_point.y,0.f,1e-3f) && Near(floor.end_point.x,0.01f,1e-3f));
        CHECK(floor.count>=5 && floor.times[floor.count-1]>floor.times[1] && floor.times[1]>0.f);
        for (int i=1;i<floor.count;++i) CHECK(floor.points[i].y<floor.points[i-1].y);
        // The run takes seconds, slowing as it goes.
        CHECK(floor.times[floor.count-1]>1.f && floor.times[floor.count-1]<15.f);
        // Wall only from y = 0.5 up (a table edge): the blood gathers at the edge.
        const auto ledge=[](const Vec& o,const Vec& d,float len,float& dist,Vec& n,int& user)
        {
            if (d.x>=-1e-6f) return false;
            const float t=-o.x/d.x;
            if (t<0 || t>len) return false;
            if (o.y+d.y*t<0.5f) return false;
            dist=t; n={1,0,0}; user=0; return true;
        };
        const RunPlan edge=PlanRun({0,0.6f,0},{1,0,0},1.f,ledge);
        CHECK(edge.end==RunPlan::Ledge && edge.end_point.y<0.56f && edge.end_point.y>0.45f);
        // Too little blood to reach anything: it stops on the wall, at its length.
        const RunPlan stop=PlanRun({0,2.f,0},{1,0,0},0.3f,room);
        CHECK(stop.end==RunPlan::Stopped && Near(2.f-stop.end_point.y,RunLength(0.3f),1e-3f));
        // Specks do not run; the length is capped.
        CHECK(RunLength(0.1f)==0.f && PlanRun({0,2.f,0},{1,0,0},0.1f,room).count==1);
        CHECK(RunLength(100.f)==kRunMaxLength);
        const RunPlan longest=PlanRun({0,5.f,0},{1,0,0},100.f,room);
        CHECK(longest.count<=RunPlan::kMaxPoints && Near(5.f-longest.end_point.y,kRunMaxLength,1e-3f));
    }
    // Smears: an imprint where the body rests, a wipe of the slide's length.
    {
        const SmearShape still=SmearFromContact({0,1,0},{0.003f,1,0},{0,0,1});
        CHECK(still.length==0.f);
        const SmearShape slide=SmearFromContact({0,1,0},{0,0.7f,0.02f},{0,0,1});
        CHECK(Near(slide.length,0.3f,1e-3f) && slide.dir.y<-0.99f && Near(slide.pos.y,0.85f,1e-3f));
        CHECK(Near(SmearFromContact({0,3,0},{0,0,0},{0,0,1}).length,0.6f));
    }
    // Stain heights: a pool on a board does not show on the floor beside it.
    {
        // A floor at y = 0.5 lies inside its slab; heights encode within it.
        const PageKey k=KeyAt({0.5f,0.5f,0.5f},AxisPosY,tile);
        const float h=SlabHeight01(0.5f,k,tile);
        CHECK(h>0.f && h<1.f && EncodeHeight(h)>1.f/255.f);
        CHECK(EncodeHeight(-0.1f)==0.f && EncodeHeight(1.1f)==0.f);
        // Decoding back gives the height to a millimetre (slab 25 cm).
        const float back=(EncodeHeight(h)*255.f-1.f)/254.f*SlabSize(tile)+SlabLow(k,tile);
        CHECK(Near(back,0.5f,0.0011f));
        // A step 5 cm down is beyond the tolerance; the board itself is not.
        CHECK(0.05f>kHeightTolerance);
        // A flat floor keeps its height across the stain; a ramp keeps its slope.
        Splat s=At(0.5f,0.5f,0.5f);
        CHECK(Near(PlaneAlong(s,1,0.9f,0.1f),0.5f));
        s.normal=Normalize({-0.2f,1,0});
        CHECK(Near(PlaneAlong(s,1,0.7f,0.5f),0.5f+0.2f*0.2f,1e-4f));
        // A wall: depth along x stays at the wall.
        Splat w=At(1.f,1.2f,0.3f); w.normal={-1,0,0};
        CHECK(Near(PlaneAlong(w,0,0.6f,1.7f),1.f));
    }
    // Pool edges: a pool on a 40 cm board finds the board's edges, not the
    // floor's far side.
    {
        // Board: x in [-0.2, 0.2], any z.
        const auto board=[](const Vec& p){ return p.x>=-0.2f && p.x<=0.2f; };
        float d[kPoolEdgeDirs];
        const int n=PoolEdges({0,0.5f,0},0.3f,board,d);
        CHECK(n>=2);
        CHECK(Near(d[0],0.2f,0.012f));                     // +x
        CHECK(Near(d[kPoolEdgeDirs/2],0.2f,0.012f));       // -x
        CHECK(d[kPoolEdgeDirs/4]<0.f && d[3*kPoolEdgeDirs/4]<0.f); // along the board
        // A floor wider than the pool: no edges.
        CHECK(PoolEdges({0,0,0},0.3f,[](const Vec&){return true;},d)==0);
        CHECK(Near(PoolEdgeDir(0).x,1.f) && Near(PoolEdgeDir(2).z,1.f,1e-5f));
    }
    // Stained rectangle: the fluid step and the decal only work where blood
    // was stamped, plus a margin; a clean page has none.
    {
        PageStore st;
        int ev;
        const PageKey k=KeyAt({0.5f,0,0.5f},AxisPosY,tile);
        float u0,v0,u1,v1;
        PageRecord empty;
        CHECK(!PageStore::StainedRect(empty,tile,kStainMargin,u0,v0,u1,v1));
        const int id=st.Deposit(k,At(0.3f,0.f,0.6f,0.f,0.05f),tile,{0,0,0},ev);
        CHECK(PageStore::StainedRect(st.Record(id),tile,0.f,u0,v0,u1,v1));
        // The droplet's quad: 1.3 radii round its centre.
        CHECK(Near(u0,0.3f-0.065f,1e-4f) && Near(u1,0.3f+0.065f,1e-4f));
        CHECK(Near(v0,0.6f-0.065f,1e-4f) && Near(v1,0.6f+0.065f,1e-4f));
        // A second stamp grows it; the margin widens it, within the page.
        st.Deposit(k,At(0.95f,0.f,0.1f,1.f,0.05f),tile,{0,0,0},ev);
        CHECK(PageStore::StainedRect(st.Record(id),tile,kStainMargin,u0,v0,u1,v1));
        CHECK(Near(u0,0.235f-kStainMargin,1e-4f) && u1==1.f && v0==0.f && Near(v1,0.665f+kStainMargin,1e-4f));
    }
    std::puts("blood tests passed");
    return 0;
}
