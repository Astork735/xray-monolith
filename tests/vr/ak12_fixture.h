#pragma once

// Minimal numeric regression evidence, read from stock Anomaly archives on
// 2026-11-01. No game installation or extracted model is needed by tests.
// configs.db0: configs/items/weapons/w_ak12.ltx
// SHA256 673b326f627dcf35302973071848af90c8ad202a3047bc7c22e4e1c8fd3c53d7
// meshes_weapons.db0: meshes/anomaly_weapons/wpn_ak12/wpn_ak12_hud.ogf
// SHA256 c662be1d87f080723889c03eb0c880e1c72434cebdd6d45d9d3683e7201e44f7
// Motion reference: anomaly_weapons/hud_animation/wpn_ak12_hud_animation.
// meshes_weapons.db0: meshes/anomaly_weapons/hud_animation/wpn_ak12_hud_animation.omf
// SHA256 db08a61f3ce36054b3da023b75ac1a7a2f72e626eb6e3e67dbf8ff25c5ffc2ae
// Both idle (2 frames) and shoot (10 frames) keep each magazine's local
// quaternion constant. These are the stored signed-short quaternion keys.
namespace ak12_fixture
{
constexpr int fire_modes[]={1,3,-1};
constexpr short canonical_rotation[]={24467,1835,21578,2452};
constexpr short reload_rotation[]={22873,3692,22873,-3692};
constexpr float reload_bind_position[]={-0.2977941334f,-0.3607078195f,-0.186069876f};
constexpr float reload_idle_position[]={0.0032f,-0.0794f,0.164f};
constexpr const char* sections[]={"wpn_ak12","wpn_ak12_ac10632","wpn_ak12_acog","wpn_ak12_eot"};

// Decode the retained key using the engine quaternion-to-matrix convention
// (xrCore/vector.h). This helper constructs data, not the measured policy.
template<class Matrix>
Matrix Pose(const short (&key)[4])
{
    const float x=key[0]/32767.f, y=key[1]/32767.f;
    const float z=key[2]/32767.f, w=key[3]/32767.f;
    Matrix out; out.identity();
    out.i.set(1.f-2.f*(y*y+z*z),2.f*(x*y-w*z),2.f*(x*z+w*y));
    out.j.set(2.f*(x*y+w*z),1.f-2.f*(x*x+z*z),2.f*(y*z-w*x));
    out.k.set(2.f*(x*z-w*y),2.f*(y*z+w*x),1.f-2.f*(x*x+y*y));
    return out;
}
}
