#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// Engine-free reading of weapon model and section names for VR profile
// derivation (VrWeaponInteraction.cpp), so the rules are unit tested
// (tests/vr/mechanics_tests.cpp).
namespace vr
{
// part (lower case) anywhere in name, whatever name's case. Names are read
// up to 127 characters.
inline bool NameHas(const char* name, const char* part)
{
    if (!name) return false;
    char lower[128];
    size_t n = 0;
    for (; name[n] && n < sizeof(lower) - 1; ++n) lower[n] = char(std::tolower((unsigned char)name[n]));
    lower[n] = 0;
    return std::strstr(lower, part) != nullptr;
}

// part as a whole word of a bone name, plural or numbered: "wpn_scope_cap1"
// and "scope_caps" have "cap", "wpn_scope_capsule" does not.
inline bool NameWord(const char* name, const char* part)
{
    if (!name) return false;
    const size_t length = std::strlen(part);
    const auto same = [&](const char* at)
    {
        for (size_t i = 0; i < length; ++i)
            if (!at[i] || std::tolower((unsigned char)at[i]) != std::tolower((unsigned char)part[i])) return false;
        return true;
    };
    for (const char* at = name; *at; ++at)
    {
        if ((at != name && std::isalpha((unsigned char)at[-1])) || !same(at)) continue;
        const char* end = at + length;
        if (*end == 's' || *end == 'S') ++end;
        if (!std::isalpha((unsigned char)*end)) return true;
    }
    return false;
}

// A spent case or loose round's part.
inline bool ShellName(const char* name)
{
    return NameHas(name, "cartridge") || NameHas(name, "patron") || NameHas(name, "shell") ||
        NameHas(name, "gilza") || NameHas(name, "casing") || NameHas(name, "bullet");
}
// An optional attachment's part: never the gun itself.
inline bool AttachmentName(const char* name)
{
    return NameHas(name, "scope") || NameHas(name, "silencer") || NameHas(name, "launcher") ||
        NameHas(name, "grenade");
}
// A round of any kind: a cartridge, a shell or a launcher's grenade.
inline bool RoundName(const char* name)
{
    return ShellName(name) || NameHas(name, "grenade") || NameHas(name, "granat") || NameHas(name, "vog") ||
        NameHas(name, "40mm");
}

// A cartridge's muzzle energy (joules), from the calibre in its section
// name; 5.45x39 when it is not recognised. Longer, more specific names first.
inline float CartridgeEnergy(const char* ammo)
{
    static const struct { const char* key; float joules; } calibres[] = {
        {"12.7x108",18000.f},{"14.5x114",30000.f},{"23x75",7000.f},{".338",6500.f},{"338lapua",6500.f},
        {"12x76",4500.f},{"12x70",4000.f},{"20x70",2500.f},{"7.92x57",3800.f},{"7.62x54",3600.f},
        {"pkm",3600.f},{"7.62x51",3400.f},{".308",3400.f},{"12.7x55",2500.f},{"6.8x",2400.f},
        {".300",2200.f},{"7.62x39",2050.f},{"5.56x45",1750.f},{"5.45x39",1350.f},{"9x39",700.f},
        {".44",1100.f},{".357",800.f},{"9x21",600.f},{"5.45x18",150.f},{"9x19",500.f},{"11.43x23",550.f},
        {".45",550.f},{"7.62x25",500.f},{"5.7x28",400.f},{"4.6x30",400.f},{"7.62x38",300.f},{"9x18",300.f},
    };
    for (const auto& calibre : calibres)
        if (std::strstr(ammo, calibre.key)) return calibre.joules;
    return 1350.f;
}

// 60 Hz steps covering one play of a clip (up to 20 s), or a fixed count
// when its length is unknown. A fixed 4 or 6 s missed what long reloads do
// last: the stock USAS-12 pulls its handle 4.1 s into a 5 s reload, and the
// stock RPD's and PKM's reloads run 7 and 8 s.
inline int ClipSteps(float length, int unknown)
{
    return length > 0.f ? std::min(1200, int(std::ceil(length*60.f)) + 1) : unknown;
}
}
