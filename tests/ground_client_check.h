// Exercise terrain quality through the supported client's actual exports.
#pragma once
#include "native/ground.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

inline int CheckGroundClient(const char* client)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetDllDirectoryA(client);
    HMODULE display = LoadLibraryExA((std::string(client) + "/DisplaySystem.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!display) { std::printf("ground: client load failed (%lu)\n", GetLastError()); return 1; }
    HMODULE n3 = LoadLibraryExA((std::string(client) + "/N3.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!n3) return 1;
    using Radius = void(__thiscall*)(void*, int);
    using Tessellate = void(__thiscall*)(void*, int, int);
    auto radius = reinterpret_cast<Radius>(GetProcAddress(display, "?SetHQRadius@AnarchyGround_t@@QAEXH@Z"));
    auto tessellate = reinterpret_cast<Tessellate>(GetProcAddress(display, "?Tesselate@AnarchyGround_t@@QAEXHH@Z"));
    auto* bake = reinterpret_cast<uint8_t*>(display) + 0x37BDF;
    if (!radius || !tessellate || *bake != 3) return 1;
    rnative::ground::Install();
    if (*reinterpret_cast<uint8_t*>(radius) != 0xE9 || *reinterpret_cast<uint8_t*>(tessellate) != 0xE9) return 1;
    auto* run = reinterpret_cast<uint8_t*>(n3) + 0x832E;
    if (*run != 0xE9) return 1;
    alignas(16) uint8_t first[0xA80]{}, second[0xA80]{};
    auto write = [](void* ground, size_t offset, const auto& value) { std::memcpy(static_cast<uint8_t*>(ground) + offset, &value, sizeof(value)); };
    auto read = [](void* ground, size_t offset) { int value; std::memcpy(&value, static_cast<uint8_t*>(ground) + offset, sizeof(value)); return value; };
    write(first, 0x17C, static_cast<void*>(first)); write(second, 0x17C, static_cast<void*>(second));
    write(first, 0x1A4, 100); write(second, 0x1A4, 100);
    int failures = 0;
    auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; std::printf("ground: FAIL %s\n", text); } };
    for (int requested : {8, 16, 44, 150}) {
        rnative::ground::SetRange(1);
        radius(first, requested);
        check(read(first, 0x1A8) == requested, "F10 radius reaches the original ground");
        check(read(first, 0x1AC) == requested * requested, "original split thresholds use the requested radius");
        radius(second, 23);
        rnative::ground::SetRange(2);
        tessellate(first, 0, 0); tessellate(second, 0, 0);
        check(read(first, 0x1A8) == requested * 2 && read(second, 0x1A8) == 46, "live grounds retain independent requested radii");
        rnative::ground::SetRange(1);
        tessellate(first, 0, 0);
        check(read(first, 0x1A8) == requested, "restoring the range does not compound the multiplier");
    }
    for (int texels : {8, 16, 32, 8}) {
        rnative::ground::SetFar(texels);
        check(*bake == (texels == 32 ? 5 : texels == 16 ? 4 : 3), "far textures retain the requested quality");
    }
    // Skip the engine-dependent body after its stolen prologue; retain the real frame hook and tessellator.
    uint8_t saved[5]; std::memcpy(saved, run + 5, sizeof(saved));
    DWORD protection; VirtualProtect(run + 5, sizeof(saved), PAGE_EXECUTE_READWRITE, &protection);
    const uint8_t leave[]{0xC9, 0xC3, 0x90, 0x90, 0x90}; std::memcpy(run + 5, leave, sizeof(leave));
    FlushInstructionCache(GetCurrentProcess(), run + 5, sizeof(saved));
    alignas(16) uint8_t renderer[0xC0]{}; write(renderer, 0xB4, static_cast<void*>(first));
    using Update = void(__thiscall*)(void*);
    auto update = reinterpret_cast<Update>(run);
    radius(first, 44); rnative::ground::SetRange(1.5f); write(first, 0xA18, 0);
    update(renderer);
    check(read(first, 0x1A8) == 66 && read(first, 0xA18) == 1, "stationary ground applies range changes and schedules tessellation");
    write(first, 0xA18, 0); radius(first, 44); update(renderer);
    check(read(first, 0xA18) == 0, "unchanged quality does not restart tessellation each frame");
    rnative::ground::SetRange(1); update(renderer);
    check(read(first, 0x1A8) == 44 && read(first, 0xA18) == 1, "stationary restoration retains the original F10 request");
    std::memcpy(run + 5, saved, sizeof(saved));
    VirtualProtect(run + 5, sizeof(saved), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), run + 5, sizeof(saved));
    rnative::ground::ForgetGround();
    write(first, 0x1A8, 37);
    tessellate(first, 0, 0);
    check(read(first, 0x1A8) == 37, "a reused ground does not inherit a previous playfield's radius");
    std::printf("ground quality: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
