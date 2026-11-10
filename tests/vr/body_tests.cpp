#include "../../src/xrEngine/Vr/vr_crouch_policy.h"
#include "../../src/xrGame/VrBodySlotPolicy.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "check.h"

int main()
{
    // Physical crouch thresholds (0.30 crouch, 0.60 low) with hysteresis.
    const float c=0.30f, l=0.60f;
    CHECK(vr::PhysicalStance(true,0.f,0,c,l)==0);
    CHECK(vr::PhysicalStance(true,0.1f,0,c,l)==0);            // standing taller
    CHECK(vr::PhysicalStance(true,-0.29f,0,c,l)==0);
    CHECK(vr::PhysicalStance(true,-0.30f,0,c,l)==1);
    CHECK(vr::PhysicalStance(true,-0.28f,1,c,l)==1);          // held inside hysteresis
    CHECK(vr::PhysicalStance(true,-0.25f,1,c,l)==0);          // released past it
    CHECK(vr::PhysicalStance(true,-0.59f,1,c,l)==1);
    CHECK(vr::PhysicalStance(true,-0.60f,1,c,l)==2);
    CHECK(vr::PhysicalStance(true,-0.57f,2,c,l)==2);
    CHECK(vr::PhysicalStance(true,-0.55f,2,c,l)==1);
    CHECK(vr::PhysicalStance(true,-0.10f,2,c,l)==0);          // straight up
    CHECK(vr::PhysicalStance(true,-0.70f,0,c,l)==2);          // straight down
    // Invalid tracking or a NaN sample keeps the previous stance.
    CHECK(vr::PhysicalStance(false,0.f,2,c,l)==2);
    CHECK(vr::PhysicalStance(true,NAN,1,c,l)==1);
    // A low depth configured above the crouch depth keeps the bands apart.
    CHECK(vr::PhysicalStance(true,-0.33f,0,0.30f,0.20f)==1);
    CHECK(vr::PhysicalStance(true,-0.40f,0,0.30f,0.20f)==2);
    // The deeper of stick and head wins.
    CHECK(vr::CombinedStance(0,1)==1 && vr::CombinedStance(2,1)==2 && vr::CombinedStance(0,0)==0);
    // A head flickering around one threshold changes stance at most once.
    int stance=0, changes=0;
    for (int i=0;i<200;++i)
    {
        const int next=vr::PhysicalStance(true,-0.30f+0.015f*std::sin(float(i)),stance,c,l);
        changes+=next!=stance; stance=next;
    }
    CHECK(changes<=1);

    // Body-slot frame: forward +Z puts "right" at +X; turned to face +X,
    // right is -Z (X-Ray is left-handed).
    const vr::V3 head{1.f,1.7f,2.f};
    vr::V3 p=vr::BodyToWorld(head,0.f,1.f,{0.2f,-0.5f,0.1f});
    CHECK(Near(p.x,1.2f) && Near(p.y,1.2f) && Near(p.z,2.1f));
    p=vr::BodyToWorld(head,1.f,0.f,{0.2f,-0.5f,0.1f});
    CHECK(Near(p.x,1.1f) && Near(p.y,1.2f) && Near(p.z,1.8f));
    p=vr::BodyToWorld(head,0.f,3.f,{0.2f,0.f,0.f});           // unnormalised forward
    CHECK(Near(p.x,1.2f) && Near(p.z,2.f));
    p=vr::BodyToWorld(head,0.f,0.f,{0.2f,0.f,0.f});           // degenerate forward
    CHECK(std::isfinite(p.x) && std::isfinite(p.z));
    // Carried items ride the belt above the holster height and spread with the
    // hip width; the shoulder (inventory) place does not move with the hips.
    for (int k=vr::BodyKnife;k<=vr::BodyDetector;++k)
    {
        if (k==vr::BodyBinoculars) continue;
        CHECK(Near(vr::BodySlotLocal(k,0.72f).y,-0.72f+vr::belt_raise));
        CHECK(Near(vr::BodySlotLocal(k,0.90f).y,-0.90f+vr::belt_raise));
    }
    CHECK(Near(vr::BodySlotLocal(vr::BodyKnife,0.72f,0.30f).x,0.21f*0.30f/0.22f));
    CHECK(Near(vr::BodySlotLocal(vr::BodyInventory,0.72f).y,vr::BodySlotLocal(vr::BodyInventory,0.9f).y));
    // The binoculars hang on the chest opposite the PDA, at its height, and
    // neither widens with the hips.
    const vr::V3 binoculars=vr::BodySlotLocal(vr::BodyBinoculars,0.72f);
    CHECK(binoculars.x>0.f && Near(binoculars.x,-vr::BodySlotLocal(vr::BodyPda,0.72f).x) &&
        Near(binoculars.y,vr::BodySlotLocal(vr::BodyPda,0.72f).y));
    CHECK(Near(vr::BodySlotLocal(vr::BodyBinoculars,0.72f,0.30f).x,binoculars.x));
    // Left-handed: every place but the pockets mirrors left to right.
    for (int k=0;k<=vr::BodyPda;++k)
    {
        const vr::V3 a=vr::BodySlotLocal(k,0.72f,0.22f,false), b=vr::BodySlotLocal(k,0.72f,0.22f,true);
        CHECK(Near(a.x,-b.x) && Near(a.y,b.y) && Near(a.z,b.z));
    }
    // Pockets keep quick slot 1 on the left for either hand, sit
    // above the belt and follow the hip height.
    for (int k=vr::BodyPocket1;k<=vr::BodyPocket4;++k)
    {
        CHECK(vr::IsPocket(k));
        const vr::V3 a=vr::BodySlotLocal(k,0.72f,0.22f,false), b=vr::BodySlotLocal(k,0.72f,0.30f,true);
        CHECK(Near(a.x,b.x) && Near(a.y,b.y) && Near(a.z,b.z));
        CHECK(a.y>-0.5f && a.y<-0.3f);
        if (k>vr::BodyPocket1) CHECK(a.x>vr::BodySlotLocal(k-1,0.72f).x);
        CHECK(vr::BodySlotLocal(k,0.9f).y<a.y);
    }
    CHECK(!vr::IsPocket(vr::BodyPda) && !vr::IsPocket(vr::BodySlotCount));
    // The PDA rides the chest on the off-hand side, above the pockets.
    const vr::V3 pda=vr::BodySlotLocal(vr::BodyPda,0.72f);
    CHECK(pda.x<0.f && pda.y>vr::BodySlotLocal(vr::BodyPocket1,0.72f).y && pda.y<-0.15f && pda.z>-0.05f);
    // Distinct places, the pistol holster and the ammunition pouch (hips at
    // +/-0.22, 3 cm back) are at least 14 cm apart, so a hand is clearly
    // at one, and none is further forward than 2 cm (a hand reaching out
    // in front of the stomach is at none of them).
    vr::V3 places[vr::BodySlotCount+2];
    int place_count=0;
    for (int k=0;k<vr::BodySlotCount;++k)
        places[place_count++]=vr::BodySlotLocal(k,0.72f);
    places[place_count++]={0.22f,-0.72f,-0.03f};
    places[place_count++]={-0.22f,-0.72f,-0.03f};
    for (int a=0;a<place_count;++a)
        for (int b=a+1;b<place_count;++b)
            CHECK(vr::BodyDistance(places[a],places[b])>0.135f);
    for (int a=0;a<place_count;++a) CHECK(places[a].z<=0.0201f);
    // The mouth is below and in front of the eyes, turning with the head;
    // the chest pockets are well out of its reach.
    vr::V3 mouth=vr::MouthPoint(head,0.f,1.f);
    CHECK(Near(mouth.x,head.x) && Near(mouth.y,head.y-0.08f) && Near(mouth.z,head.z+0.08f));
    mouth=vr::MouthPoint(head,-1.f,0.f);
    CHECK(Near(mouth.x,head.x-0.08f) && Near(mouth.z,head.z));
    for (int k=vr::BodyPocket1;k<=vr::BodyPocket4;++k)
        CHECK(vr::BodyDistance(vr::MouthPoint(head,0.f,1.f),
            vr::BodyToWorld(head,0.f,1.f,vr::BodySlotLocal(k,0.72f)))>2.5f*vr::mouth_reach);

    // With the body shown: every place but the inventory sits within 75
    // degrees of forward, mirrors for the left hand (pockets excepted), and
    // on a 20 cm waist plus the margin they, the pistol holster (right hip,
    // 22 cm out) and the pouch are at least 13.5 cm apart.
    {
        vr::BodyPlace place, other;
        CHECK(!vr::BodySlotOnBody(vr::BodyInventory,false,place));
        vr::V3 on_body[vr::BodySlotCount+2];
        int count=0;
        const float waist=0.20f+vr::body_slot_margin;
        for (int k=0;k<=vr::BodyPouch;++k)
        {
            if (!vr::BodySlotOnBody(k,false,place)) continue;
            CHECK(std::fabs(place.angle)<=75.f*3.14159265f/180.f+1e-4f);
            CHECK(vr::BodySlotOnBody(k,true,other));
            CHECK(Near(other.height,place.height));
            CHECK(vr::IsPocket(k) ? Near(other.angle,place.angle) : Near(other.angle,-place.angle));
            on_body[count++]={std::sin(place.angle)*waist,place.height,std::cos(place.angle)*waist};
        }
        on_body[count++]={0.22f,0.f,-0.03f};
        for (int a=0;a<count;++a)
            for (int b=a+1;b<count;++b)
                CHECK(vr::BodyDistance(on_body[a],on_body[b])>0.135f);
    }

    // Nearest available slot within reach.
    vr::V3 points[vr::BodySlotCount];
    bool available[vr::BodySlotCount];
    for (int k=0;k<vr::BodySlotCount;++k)
    { points[k]=vr::BodyToWorld(head,0.f,1.f,vr::BodySlotLocal(k,0.72f)); available[k]=true; }
    float distance=0.f;
    vr::V3 hand=points[vr::BodyKnife]; hand.x+=0.03f;
    CHECK(vr::NearestBodySlot(points,available,vr::BodySlotCount,hand,0.10f,distance)==vr::BodyKnife);
    CHECK(Near(distance,0.03f));
    available[vr::BodyKnife]=false;                           // empty slot is skipped
    CHECK(vr::NearestBodySlot(points,available,vr::BodySlotCount,hand,0.10f,distance)!=vr::BodyKnife);
    hand={head.x+2.f,head.y,head.z};                          // out of reach
    CHECK(vr::NearestBodySlot(points,available,vr::BodySlotCount,hand,0.10f,distance)==-1);
    // Binoculars against a headset: eyepieces held to the shell sit 8-12 cm
    // ahead of the eyes, which the old 11 cm gate rarely let through.
    CHECK(vr::BinocularsAtEyesPolicy(0.12f,0.95f,false));
    CHECK(!vr::BinocularsAtEyesPolicy(0.12f,0.5f,false));         // looking elsewhere
    CHECK(!vr::BinocularsAtEyesPolicy(0.20f,0.95f,false));        // at the chest
    CHECK(vr::BinocularsAtEyesPolicy(0.20f,0.70f,true));          // kept while raised
    CHECK(!vr::BinocularsAtEyesPolicy(0.25f,0.95f,true));         // lowered
    // The eyepiece end faces the first-person camera, whichever way the
    // model was authored.
    CHECK(vr::BinocularEyepieceEnd(0.9f)==-1);                    // +Z looks away: eyepieces at -Z
    CHECK(vr::BinocularEyepieceEnd(-0.9f)==1);                    // +Z toward the camera
    CHECK(vr::BinocularEyepieceEnd(0.1f)==0);                     // across the view: nearer end
    CHECK(vr::BinocularEyepieceEnd(0.f)==0);                      // no first-person pose
    // Long guns: where put by hand while holstered wins (it is drawn from
    // there); otherwise the hand's shoulder, except slot 3's second long gun,
    // over the off-hand shoulder of the handedness setting.
    CHECK(vr::LongGunShoulderRight(0,false,false,true,true)==false);  // put on the left, right hand
    CHECK(vr::LongGunShoulderRight(1,true,true,false,true)==true);    // put on the right
    CHECK(vr::LongGunShoulderRight(-1,false,true,true,true)==true);   // drawn: follows the right hand
    CHECK(vr::LongGunShoulderRight(-1,false,false,false,true)==false);// drawn left-handed: left
    CHECK(vr::LongGunShoulderRight(-1,true,true,true,true)==false);   // slot 3 second: off-hand
    CHECK(vr::LongGunShoulderRight(-1,true,true,false,false)==true);  // left-handed player
    CHECK(vr::LongGunShoulderRight(-1,true,false,false,true)==false); // slot 3 alone: the hand
    std::printf("vr body tests passed\n");
    return 0;
}
