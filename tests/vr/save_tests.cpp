#include "../../src/xrGame/VrSaveLedger.h"
#include "../../src/xrGame/VrPistolState.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "check.h"

// A cartridge: its ammo type and a serial number, so every round is traced.
struct Round { int type; int serial; };
using Rounds=std::vector<Round>;

int main()
{
    const auto same=[](const Round& a, const Round& b) { return a.type==b.type; };
    // Policy-only reload -> normalization -> transient reinitialization:
    // detached rounds are real owners, and the chamber is already included
    // in live (never an extra serial). This does not read/write a save file
    // or demonstrate native desktop/VR runtime compatibility.
    {
        Rounds live, returned;
        for (int id=0;id<31;++id) live.push_back({0,id});
        vr::MagazineMechanism<Rounds> rifle; rifle.locks_on_empty=false; rifle.Initialize(live);
        for (int id=31;id<61;++id) rifle.magazines[1].push_back({id%2,id});
        const auto trace=[&]
        {
            std::vector<int> seen(61,0);
            const auto owner=[&](const Rounds& rounds)
            { for (const auto& r:rounds) { CHECK(r.serial>=0 && r.serial<61); ++seen[r.serial]; } };
            owner(live); owner(rifle.magazines[0]); owner(rifle.magazines[1]); owner(returned);
            for (int count:seen) CHECK(count==1);
        };
        const int out=rifle.EjectInserted(live);
        CHECK(out==0 && live.size()==1 && live.back().serial==30);
        CHECK(rifle.EjectInserted(live)==-1 && rifle.chamber); trace();
        // A second magazine can be inserted without losing the retained
        // chamber cartridge, even while the first remains a detached owner.
        CHECK(rifle.Draw() && rifle.held==1 && rifle.Insert(live));
        CHECK(live.size()==31 && live.back().serial==30); trace();
        vr::NormalizeRounds(live,rifle.magazines,2,30,same,[&](const Round& r){returned.push_back(r);});
        // Native policy processes detached owners before removing mixed
        // types; it may return rounds rather than refill the freed space.
        CHECK(live.size()==16 && live.back().serial==30 && returned.size()==45);
        CHECK(rifle.magazines[0].empty() && rifle.magazines[1].empty()); trace();
        const auto normalized=live; const size_t returned_count=returned.size();
        for (int repeat=0;repeat<10;++repeat)
        {
            CHECK(vr::NormalizeRounds(live,rifle.magazines,2,30,same,[&](const Round& r){returned.push_back(r);})==0);
            CHECK(returned.size()==returned_count && live.size()==normalized.size());
            for (size_t n=0;n<live.size();++n) CHECK(live[n].serial==normalized[n].serial && live[n].type==0);
            // The load-side policy derives one chamber from native rounds;
            // it must not insert an additional round or mutate their order.
            vr::MagazineMechanism<Rounds> restored; restored.Initialize(live);
            CHECK(restored.chamber && restored.CanFire() && restored.Total(live)==normalized.size());
            CHECK(restored.magazines[0].empty() && restored.magazines[1].empty()); trace();
        }
    }
    // A chambered round on a full 30-round magazine: 31 in the gun. The
    // desktop holds 30; one goes back, the chambered round stays on top.
    {
        Rounds live, spare[2]; std::vector<Round> returned;
        for (int i=0;i<31;++i) live.push_back({0,i});
        const unsigned n=vr::NormalizeRounds(live,spare,2,30,same,[&](const Round& r){ returned.push_back(r); });
        CHECK(n==1 && live.size()==30 && returned.size()==1);
        CHECK(live.back().serial==30 && returned[0].serial==0);
    }
    // Spare magazines fill the weapon before anything goes back; an empty
    // weapon takes its spares.
    {
        Rounds live, spare[2]; std::vector<Round> returned;
        for (int i=0;i<5;++i) live.push_back({0,i});
        for (int i=0;i<8;++i) spare[0].push_back({0,100+i});
        vr::NormalizeRounds(live,spare,2,10,same,[&](const Round& r){ returned.push_back(r); });
        CHECK(live.size()==10 && returned.size()==3 && live.back().serial==4);
        Rounds empty, spares[2]; returned.clear();
        for (int i=0;i<4;++i) spares[1].push_back({0,200+i});
        vr::NormalizeRounds(empty,spares,2,10,same,[&](const Round& r){ returned.push_back(r); });
        CHECK(empty.size()==4 && returned.empty());
    }
    // Mixed types: the chambered type is kept, the rest returned.
    {
        Rounds live, spare[2]; std::vector<Round> returned;
        live={{1,0},{1,1},{2,2},{1,3}};
        vr::NormalizeRounds(live,spare,2,30,same,[&](const Round& r){ returned.push_back(r); });
        CHECK(live.size()==3 && returned.size()==1 && returned[0].type==2);
    }
    // Conservation over random states: rounds out of the gun (in the hand, in
    // pouch magazines) and in it, any capacity and mix. Nothing created or
    // lost; the weapon never holds more than its capacity or two types.
    {
        unsigned seed=12345;
        const auto next=[&]() { seed=seed*1103515245u+12345u; return (seed>>16)&0x7fff; };
        for (int run=0;run<10000;++run)
        {
            const size_t capacity=1+next()%100;
            Rounds live, spare[2]; std::vector<Round> returned;
            int serial=0;
            const size_t live_count=next()%(capacity+2);
            for (size_t i=0;i<live_count;++i) live.push_back({int(next()%3==0 ? 1 : 0),serial++});
            for (auto& m:spare) { const size_t c=next()%(capacity+1); for (size_t i=0;i<c;++i) m.push_back({int(next()%4==0),serial++}); }
            const int total=serial;
            vr::NormalizeRounds(live,spare,2,capacity,same,[&](const Round& r){ returned.push_back(r); });
            CHECK(int(live.size()+returned.size())==total);
            CHECK(live.size()<=capacity && spare[0].empty() && spare[1].empty());
            for (const Round& r:live) CHECK(r.type==live.back().type);
            std::vector<int> seen(size_t(total),0);
            for (const Round& r:live) ++seen[size_t(r.serial)];
            for (const Round& r:returned) ++seen[size_t(r.serial)];
            for (int s:seen) CHECK(s==1);
        }
    }
    std::printf("PASS: chambered overflow clamp, mixed types returned, 10000 conservation cases\n");
    return 0;
}
