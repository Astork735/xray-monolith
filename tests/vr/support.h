#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
constexpr float EPS_S = 0.000001f;
struct Fvector
{
    float x,y,z;
    Fvector& set(float a,float b,float c) {x=a;y=b;z=c;return *this;}
    Fvector& mul(float s){x*=s;y*=s;z*=s;return *this;}
    float square_magnitude()const{return x*x+y*y+z*z;}
    Fvector& normalize(){return mul(1/std::sqrt(square_magnitude()));}
    Fvector& crossproduct(const Fvector&a,const Fvector&b){return set(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
    Fvector& add(const Fvector&v){x+=v.x;y+=v.y;z+=v.z;return *this;}
    Fvector& add(const Fvector&a,const Fvector&b){return set(a.x+b.x,a.y+b.y,a.z+b.z);}
    Fvector& mad(const Fvector&v,float s){x+=v.x*s;y+=v.y*s;z+=v.z*s;return *this;}
    float dotproduct(const Fvector&v)const{return x*v.x+y*v.y+z*v.z;}
};
// Test carrier matches Fmatrix's row-vector layout. Rendering builds use the
// real engine types; tests exercise the production conversion/state code.
struct Fmatrix
{
    union
    {
        float m[4][4];
        struct {float _11,_12,_13,_14,_21,_22,_23,_24,_31,_32,_33,_34,_41,_42,_43,_44;};
        struct {Fvector i;float a;Fvector j;float b;Fvector k;float d;Fvector c;float e;};
    };
    void identity(){std::memset(m,0,sizeof(m));for(int n=0;n<4;++n)m[n][n]=1;}
    void scale(float x,float y,float z){identity();_11=x;_22=y;_33=z;}
    void invert(const Fmatrix&v)
    {
        identity();for(int r=0;r<3;++r)for(int s=0;s<3;++s)m[r][s]=v.m[s][r];
        for(int s=0;s<3;++s)m[3][s]=-(v.c.x*m[0][s]+v.c.y*m[1][s]+v.c.z*m[2][s]);
    }
    void mul_43(const Fmatrix&A,const Fmatrix&B)
    {
        Fmatrix out;out.identity();
        for(int r=0;r<4;++r)for(int s=0;s<4;++s){out.m[r][s]=0;for(int t=0;t<4;++t)out.m[r][s]+=B.m[r][t]*A.m[t][s];}
        *this=out;
    }
    void build_camera_dir(const Fvector&p,const Fvector&f,const Fvector&u)
    {Fmatrix w;w.identity();w.k=f;w.j=u;w.i.crossproduct(u,f);w.c=p;invert(w);}
    void transform_tiny(Fvector& v) const
    {
        const Fvector source = v;
        v.set(source.x*i.x+source.y*j.x+source.z*k.x+c.x,
            source.x*i.y+source.y*j.y+source.z*k.y+c.y,
            source.x*i.z+source.y*j.z+source.z*k.z+c.z);
    }
};
struct TestDevice {unsigned dwFrame=0;float ViewportNear=0.2f;bool b_is_Active=true;};
extern TestDevice Device;
extern bool g_vr_owns_present;
struct TestEnvironment {struct Descriptor{float far_plane=1000;};Descriptor descriptor;Descriptor*CurrentEnv=&descriptor;};
struct TestPersistent {TestEnvironment environment;TestEnvironment&Environment(){return environment;}};
extern TestPersistent* g_pGamePersistent;
#ifdef XR_VR_PERF_TEST
void Msg(const char*,...);
#else
inline void Msg(const char*,...) {}
#endif
template<size_t N>void xr_strcpy(char(&dst)[N],const char*src){strncpy_s(dst,src,_TRUNCATE);}
template<class T>void clamp(T&v,T lo,T hi){v=std::max(lo,std::min(hi,v));}

template<size_t N,class...Args>void xr_sprintf(char(&dst)[N],const char*fmt,Args...args){sprintf_s(dst,fmt,args...);}
