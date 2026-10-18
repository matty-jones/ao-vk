// N3.dll's statel rings, for RVK_DetailRange (re/n3_ring.c, re/n3_popin*.c).
//
// N3 shows a playfield's placed objects (statels) zone by zone: a grid of square zones, each holding lists of statels
// by kind. When the game's camera moves onto another tile, each loaded zone is given a ring by how far its middle is
// from the camera (FUN_10028ab7) - radii of the view distance x 0.5 x the grid's factors at +0x2C..+0x3C (0.10, 0.15,
// 0.30, 0.40, 0.55; none under 40 m) - and the ring switches each list on or off (FUN_100286a9). The small things
// (rocks, shrubs) go at the second ring: with the view distance near 2000 that is 150 m, a whole zone's rocks at once
// as the camera crosses it - while the renderer's statel shadows (RVK_StaticShad) still cast them (user dumps, 2026-10-18).
//
// The ring function is wrapped: while it runs the two inner factors - the rings the small things go at - are scaled,
// then put back, so N3's own code picks the ring. The outer rings (trees, buildings) stay the game's, only moved out
// as far as the inner ones now reach: scaling all five drew about four times the land's objects and cost a lot of
// frame rate (user, 2026-10-18). Each is kept within the zones N3 keeps loaded at all (the view distance x the grid's
// +0x28, FUN_10028cf4: the rings' radii take half the view distance, so a factor up to twice +0x28) - past that there
// is nothing to show.
#include "native/rings.h"

#include "native/native.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace rnative::rings {

namespace {

using RingFn = int(__fastcall*)(void* grid, void* edx, int zone, const void* camera);
RingFn g_ring;
std::atomic<float> g_scale{1.0f};
bool g_logged;

constexpr uint32_t kRingRva = 0x28AB7;              // FUN_10028ab7 in N3.dll (image base 0x10000000)
constexpr uint32_t kKept = 0x28, kFactors = 0x2C;   // the grid: zones kept to the view distance x this; ring factors

// FUN_10028ab7 (thiscall: the grid in ecx; the zone's index and the n3Camera_t on the stack): the zone's ring.
int __fastcall RingHook(uint8_t* grid, void* edx, int zone, const void* camera)
{
    const float scale = g_scale.load(std::memory_order_relaxed);
    if (scale == 1.0f)
        return g_ring(grid, edx, zone, camera);
    float* factors = reinterpret_cast<float*>(grid + kFactors);
    float kept;
    std::memcpy(&kept, grid + kKept, 4);
    const float limit = kept * 2.0f;
    float saved[5];
    std::memcpy(saved, factors, sizeof(saved));
    for (int i = 0; i < 5; ++i) {
        const float wanted = i < 2 ? saved[i] * scale : std::max(saved[i], factors[i - 1]);   // (the rings in order)
        factors[i] = limit > 0.0f ? std::max(saved[i], std::min(wanted, limit)) : wanted;
    }
    if (!g_logged) {
        g_logged = true;
        Log("statel rings: x%.2f - factors %.3f %.3f %.3f %.3f %.3f (the game's %.3f %.3f %.3f %.3f %.3f), zones kept to "
            "%.3f of the view distance", scale, factors[0], factors[1], factors[2], factors[3], factors[4], saved[0],
            saved[1], saved[2], saved[3], saved[4], kept);
    }
    const int ring = g_ring(grid, edx, zone, camera);
    std::memcpy(factors, saved, sizeof(saved));
    return ring;
}

}  // namespace

void SetScale(float scale)
{
    g_scale.store(scale, std::memory_order_relaxed);
    g_logged = false;
}

void Install()
{
    static int attempts;
    if (g_ring || attempts >= 600)
        return;
    ++attempts;
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (!n3)
        return;                                     // not loaded yet: next frame
    attempts = 600;
    // push ebp; mov ebp, esp; sub esp, 10h | mov eax, [ebp+8]; push esi; mov esi, ecx; cdq; fld [esi+10h];
    // idiv [esi+0Ch]; fmul qword [<0.5>] - the stolen six bytes and what follows, checked (another N3.dll is left alone).
    static const uint8_t kExpected[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x8B, 0x45, 0x08, 0x56, 0x8B, 0xF1,
                                        0x99, 0xD9, 0x46, 0x10, 0xF7, 0x7E, 0x0C, 0xDC, 0x0D};
    auto* at = reinterpret_cast<uint8_t*>(n3) + kRingRva;
    if (std::memcmp(at, kExpected, sizeof(kExpected)) != 0) {
        Log("statel rings: N3.dll's ring function not as expected (another build?) - RVK_DetailRange does nothing");
        return;
    }
    g_ring = reinterpret_cast<RingFn>(HookAt(at, 6, nullptr, 0, reinterpret_cast<void*>(&RingHook), "N3 statel rings"));
    Log("statel rings: N3.dll %s", g_ring ? "hooked (RVK_DetailRange)" : "not hooked");
}

}  // namespace rnative::rings
