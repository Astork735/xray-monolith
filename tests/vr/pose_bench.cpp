#include "support.h"
#include "../../src/xrGame/VrWeaponPose.h"
#include <chrono>
#include <cstdlib>
#include <new>

namespace
{
bool count_allocations=false;
std::size_t allocations=0;
}
void* operator new(std::size_t size)
{
    if (count_allocations) ++allocations;
    if (void* memory=std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
struct Bone
{
    Fmatrix mTransform{}, mTransformHidden{}, mRenderTransform{};
    void (*callback)(Bone*)=nullptr;
    void* parameter=nullptr;
    void* callback_param() { return parameter; }
    void set_callback(unsigned, void (*fn)(Bone*), void* value, bool)
    { callback=fn; parameter=value; }
};
struct Data
{
    unsigned short parent=65535;
    unsigned short GetParentID() const { return parent; }
};
struct Model
{
    static constexpr unsigned short count=96;
    Bone bones[count];
    Data data[count];
    bool visible[count]{};
    std::vector<Fmatrix> bind, poses;
    vr::WeaponPoseCache<std::vector<Fmatrix>> pose_cache;
    unsigned bind_queries=0, calculations=0;
    Model()
    {
        bind.resize(count);
        for (unsigned short id=0;id<count;++id)
        {
            bind[id].identity(); bind[id].c.z=id*0.001f;
            data[id].parent=id ? (id-1)/2 : 65535;
        }
    }
    unsigned short LL_BoneCount() const { return count; }
    Data& LL_GetData(unsigned short id) { return data[id]; }
    unsigned short LL_BoneID(const char* name) const
    {
        if (!std::strcmp(name,"magazine")) return 7;
        if (!std::strcmp(name,"rounds")) return 15;
        if (!std::strcmp(name,"cycle")) return 2;
        if (!std::strcmp(name,"handle")) return 5;
        if (!std::strcmp(name,"reload_copy")) return 11;
        return 65535;
    }
    void LL_GetBindTransform(std::vector<Fmatrix>& out) { ++bind_queries; out=bind; }
    bool LL_GetBoneVisible(unsigned short id) const { return visible[id]; }
    void LL_SetBoneVisible(unsigned short id, bool shown, bool) { visible[id]=shown; }
    Bone& LL_GetBoneInstance(unsigned short id) { return bones[id]; }
    void CalculateBones_Invalidate() {}
    void CalculateBones(bool)
    {
        ++calculations;
        for (auto& bone:bones)
        {
            if (bone.callback) bone.callback(&bone);
            bone.mRenderTransform=bone.mTransform;
        }
    }
};
void PoseBone(Bone* bone) { bone->mTransform=*static_cast<Fmatrix*>(bone->callback_param()); }

void Run(const char* name, unsigned loose)
{
    constexpr unsigned frames=3000, repeats=5;
    Model models[9];
    const auto draw=[&](unsigned frame)
    {
        for (unsigned eye=0;eye<2;++eye)
            for (unsigned n=0;n<=loose;++n)
            {
                if (!n && eye) continue; // existing main-weapon frame guard
                auto& model=models[n];
                vr::ApplyWeaponPose(model,model.poses,PoseBone,2,"cycle",0.01f,"rounds",
                    {"magazine","rounds"},n!=0,true,frame%2==0,nullptr,{}, {},
                    "handle",0.005f,{"reload_copy"},true,&model.pose_cache,n ? frame : ~0u);
            }
    };
    draw(0); // exclude initial buffer allocations
    allocations=0;
    unsigned before_bind=0, before_calc=0;
    for (const auto& model:models) { before_bind+=model.bind_queries; before_calc+=model.calculations; }
    count_allocations=true;
    const auto begin=std::chrono::steady_clock::now();
    for (unsigned repeat=0;repeat<repeats;++repeat)
        for (unsigned frame=1;frame<=frames;++frame) draw(repeat*frames+frame);
    const auto end=std::chrono::steady_clock::now();
    count_allocations=false;
    unsigned after_bind=0, after_calc=0;
    for (const auto& model:models) { after_bind+=model.bind_queries; after_calc+=model.calculations; }
    const double samples=double(frames)*repeats;
    const double ms=std::chrono::duration<double,std::milli>(end-begin).count()/samples;
    std::printf("%s: %.6f ms/frame; %.1f allocations/frame; %.1f bind queries/frame; %.1f calculations/frame\n",
        name,ms,allocations/samples,(after_bind-before_bind)/samples,(after_calc-before_calc)/samples);
}
}
int main()
{
    puts("Synthetic 96-bone models, two views, five warmed 3000-frame batches; no renderer/GPU/physics.");
    Run("weapon",0);
    Run("weapon + held magazine",1);
    Run("weapon + 8 loose shells",8);
}
