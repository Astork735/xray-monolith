#pragma once

// Minimal numeric evidence read from stock Anomaly archives on 2026-11-01.
// No installed game or extracted model is required by the tests.
// configs/items/weapons/w_rpd.ltx
// SHA256 63b94564f5a8d044aa4a1b1810d02c46ebae20cbf97be038efb69b838060c0ff
// meshes/anomaly_weapons/wpn_rpd/wpn_rpd_hud.ogf
// SHA256 e7acd0d27c86904e91ee7bd04ef9f75609cb250cb0ca5e0b93a3af26e85f3178
// meshes/anomaly_weapons/hud_animation/wpn_rpd_hud_animation.omf
// SHA256 9551ab31f5da530a57dd168d7dbef52812acd5b83ca9049a2805e6960e0b834e
// PKM/PKP config: configs/items/weapons/w_pkm.ltx
// SHA256 e6382e0178f8444e19a990c88c63f1ffc0069607b8dbf55532b54ac97cb56b28
// PKM model SHA256 5bf64cb91ab44413fe4232b0b76a26d026e003490f0b3ece420eab5bb8ad1da7
// PKP model SHA256 01dd8035efb480c89e785cc049084ae64e435df9df4014908896c5edd0630024
// Shared wpn_pkm_hud_animation.omf
// SHA256 e5aa82200fa84fab96b9503ac2f64aeb7fad991fec59f6883e6f08889023532e
namespace rpd_fixture
{
struct Point { float x,y,z; };
constexpr Point trigger={0.002170964f,0.012149776f,0.042295180f};
constexpr Point magazine_center={-0.025485620f,-0.011337854f,0.191846423f};
constexpr Point magazine_first={-0.912045247f,-0.409245540f,0.026297442f};
constexpr float magazine_travel=0.626673499f;
constexpr float magazine_width=0.178054973f;
constexpr int magazine_peak_key=64;
// joint10 is a child of magazin. Its initial direction in MODEL space is
// upward despite its differently rotated parent's local Y/Z directions.
constexpr Point belt_center={-0.063428693f,0.056771949f,0.198635280f};
constexpr Point belt_first={-0.000067084f,0.999999997f,-0.000029431f};
constexpr float belt_travel=0.111578774f;
constexpr float cover_max_degrees=112.147889f;
constexpr float cover_at_magazine_peak_degrees=79.651048f;
constexpr float cover_origin_travel=0.000000576f;
constexpr Point cover_min={-0.026637206f,0.083365679f,-0.034671694f};
constexpr Point cover_max={0.029341606f,0.130210996f,0.266917527f};
constexpr Point belt_end={-0.047273833f,0.136228353f,0.198637478f};

// PKP inherits the PKM reload and uses the same animation tracks. Their
// different HUD meshes were audited separately; no PKP geometry is invented.
constexpr float pkm_pkp_magazine_travel=0.455388021f;
constexpr Point pkm_pkp_magazine_first={0.999956576f,0.007656275f,-0.005312944f};
constexpr int pkm_pkp_magazine_peak_key=106;

// GAMMA "353- RPD Reanimation - NickolasNikova" (read-only, 2026-11-01).
// meshes/anomaly_weapons/wpn_rpd/wpn_rpd_hud.ogf
// SHA256 b9950051f21820b910aed5bf08cd085d92ff3fb462d86744a386b2d45a0dd043
// meshes/anomaly_weapons/hud_animation/wpn_rpd_hud_animation.omf
// SHA256 8cb4d717ebe300b9f5578a60692a697a05d3fb29037bbea7998226aefa8541af
// Bind-space mesh centres from tools/vr_measure_weapons.py load(). The belt
// rounds plr_1..14 hang under magazin but are bound about half a metre
// behind the gun; only the animation lays them along the drum and tray.
namespace gamma
{
constexpr Point trigger={-0.001243488f,0.011822920f,0.023569920f};
constexpr Point magazine_own_center={-0.011424720f,-0.058794362f,0.164045162f};
constexpr Point magazine_bind_subtree_center={-0.238053463f,-0.058794362f,-0.163427517f};
constexpr float magazine_width=0.151576787f;
// rpd_reload_empty, decoded during the same investigation: the drum first
// slides straight back off its hook before dropping away.
constexpr Point magazine_first={-0.0399f,0.0598f,-0.9974f};
constexpr float magazine_travel=0.356f;
}
}
