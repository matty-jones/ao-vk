// DisplaySystem.dll's ground, for RVK_GroundRange (re/lod_ds.c, re/ds_tess.c, re/ds_ground_hq.c, re/n3_lod.c).
//
// AnarchyGround_t draws the playfield's heightmap as a split tree of triangles. Within the full-quality radius
// (+0x1A8, set by SetHQRadius: entering a playfield the game set 8, 44 - the option DisplayGroundFullQualityRadius, its
// slider 7..44 - and then 150, the one it draws with) the tree's leaves are
// flagged (FUN_10031f3c: +0x20, the leaf's middle nearer than the radius) and drawn with each 4 x 4 cell's own tile
// texture; beyond it a whole patch is one baked texture of 8 texels a cell (CreateHiTexture), 32 times blurrier. The
// radius also lifts the split thresholds (FUN_1003216f: table[i] = (i x quality / 100)^2.25 + radius^2), so the
// ground stays finely split as far out. Its vertex buffers are made as needed (FUN_10034658), so a bigger radius costs
// draws, vertices and the game's own rebuilding as the camera moves (the ring's area: the radius squared), not a fixed
// buffer. At x4 (600) that was 100-230 ms frames as the camera moved and ~900 MB less address space in the 32-bit
// process (user, 2026-10-18): the scale stops at 2.5.
//
// SetHQRadius is wrapped to scale the game's value; Tesselate (called on the live ground as the camera moves) is
// wrapped to take up a changed scale there - no pointer to the ground is kept, it goes with the playfield.
//
// Past the radius, CreateHiTexture bakes each patch into one texture of n x 8 texels a side (n cells; `mov ecx, eax;
// shl ecx, 3` - capped at the device's largest texture, 8192 here), each cell's tile shrunk to 8 x 8 (FUN_100374c5;
// FUN_100365b2 makes a tile at any size, its copies index bytes - up to 256). RVK_GroundFar sets that shift's count:
// 16 or 32 texels a cell, 1024 or 2048 a side. A patch costs its texture and, in the 32-bit process, the CPU copy rvk
// keeps of a texture the game locked (rvk_surface.cpp shadow): 0.5 MB each at the game's 8, 2 MB at 16, 8 MB at 32.
// Patches baked after a change take it; the ones already baked keep theirs until the game bakes them again.
#include "native/ground.h"

#include "native/native.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace rnative::ground {

namespace {

using SetRadiusFn = void(__fastcall*)(void* ground, void* edx, int radius);
using TesselateFn = void(__fastcall*)(void* ground, void* edx, int x, int z);
SetRadiusFn g_setRadius;
TesselateFn g_tesselate;
std::atomic<float> g_scale{1.0f};
std::atomic<int> g_gameRadius{-1};   // the game's own value, last set (-1: not yet)
int g_loggedRadius = -1;

constexpr uint32_t kSetRadiusRva = 0x3255B, kTesselateRva = 0x35348;   // DisplaySystem.dll (image base 0x10000000)
constexpr uint32_t kBakeShiftRva = 0x37BDF;                               // CreateHiTexture: shl ecx, <this>
uint8_t* g_bakeShift;                                                     // (null: not patched - unknown build)
std::atomic<int> g_farTexels{8};
constexpr uint32_t kTileTextureRva = 0x36594;                             // FUN_10036594: (data, tile) -> RTexture_t
constexpr uint32_t kGroundData = 0x17C;                                   // AnarchyGround_t: its AnarchyGroundData_t
using TileTextureFn = void*(__fastcall*)(void* data, void* edx, uint32_t tile);
TileTextureFn g_tileTexture;
void (*g_onNewGround)(void*, void*);
void (*g_onTesselate)(void*, void*);
void* g_lastData;
std::atomic<uint32_t> g_tesselates{0};
std::atomic<bool> g_native{false};
constexpr uint32_t kRadius = 0x1A8;                                      // AnarchyGround_t: the full-quality radius

int Scaled(int radius)
{
    if (g_native.load(std::memory_order_relaxed)) return 8;   // (rvk draws the ground: the game's at its least)
    return std::clamp(int(std::lround(radius * g_scale.load(std::memory_order_relaxed))), 1, 1000);
}

void Note(int game, int used)
{
    if (used == g_loggedRadius) return;
    g_loggedRadius = used;
    Log("ground: full-quality radius %d (the game's %d, RVK_GroundRange x%.2f)", used, game,
        g_scale.load(std::memory_order_relaxed));
}

// AnarchyGround_t::SetHQRadius(int) (thiscall: the ground in ecx).
void __fastcall SetRadiusHook(uint8_t* ground, void* edx, int radius)
{
    g_gameRadius.store(radius, std::memory_order_relaxed);
    const int used = Scaled(radius);
    Note(radius, used);
    g_setRadius(ground, edx, used);
}

// AnarchyGround_t::Tesselate(int, int): a scale changed since the radius was set is taken up first (the original then
// restarts its tessellation, as after the game's own slider).
void __fastcall TesselateHook(uint8_t* ground, void* edx, int x, int z)
{
    void* data;
    std::memcpy(&data, ground + kGroundData, 4);
    if (data != g_lastData) {                       // (a playfield's ground, new: the native ground reads it)
        g_lastData = data;
        if (g_onNewGround && data) g_onNewGround(ground, data);
    }
    g_tesselates.fetch_add(1, std::memory_order_relaxed);
    if (g_onTesselate && data) g_onTesselate(ground, data);
    int current;
    std::memcpy(&current, ground + kRadius, 4);
    int game = g_gameRadius.load(std::memory_order_relaxed);
    if (game < 0 && current > 0) {                  // (hooked after the ground was made: its radius is the game's)
        game = current;
        g_gameRadius.store(game, std::memory_order_relaxed);
    }
    if (game > 0) {
        const int used = Scaled(game);
        if (current != used) {
            Note(game, used);
            g_setRadius(ground, nullptr, used);
        }
    }
    g_tesselate(ground, edx, x, z);
}

// The bake's shift for texels a cell (8, 16, 32 -> 3, 4, 5), written into CreateHiTexture.
void ApplyFar()
{
    if (!g_bakeShift) return;
    const int texels = g_farTexels.load(std::memory_order_relaxed);
    const uint8_t shift = g_native.load(std::memory_order_relaxed) ? 1 : texels >= 32 ? 5 : texels >= 16 ? 4 : 3;
    if (*g_bakeShift == shift) return;
    DWORD protect;
    if (!VirtualProtect(g_bakeShift, 1, PAGE_EXECUTE_READWRITE, &protect)) return;
    *g_bakeShift = shift;                           // (one byte: the game's thread sees the old or the new count)
    VirtualProtect(g_bakeShift, 1, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), g_bakeShift, 1);
    Log("ground: far patches baked at %d texels a cell (the game's 8; RVK_GroundFar)", 1 << shift);
}

}  // namespace

void SetNewGround(void (*onNewGround)(void* ground, void* data))
{
    g_onNewGround = onNewGround;
}

void SetOnTesselate(void (*onTesselate)(void* ground, void* data))
{
    g_onTesselate = onTesselate;
}

uint32_t TesselateCount()
{
    return g_tesselates.load(std::memory_order_relaxed);
}

void ForgetGround()
{
    g_lastData = nullptr;
}

void* TileTexture(void* data, uint32_t tile)
{
    return g_tileTexture && data ? g_tileTexture(data, nullptr, tile) : nullptr;
}

void SetNative(bool active)
{
    if (g_native.exchange(active) == active) return;
    ApplyFar();
    Log("ground: %s", active ? "drawn by rvk (the game's own at its least work)" : "the game's own");
}

void SetFar(int texelsPerCell)
{
    g_farTexels.store(texelsPerCell, std::memory_order_relaxed);
    ApplyFar();
}

void SetRange(float scale)
{
    g_scale.store(std::clamp(scale, 1.0f, 2.5f), std::memory_order_relaxed);
}

void Install()
{
    static int attempts;
    if (g_setRadius || attempts >= 600)
        return;
    ++attempts;
    HMODULE ds = GetModuleHandleA("DisplaySystem.dll");
    if (!ds)
        return;                                     // not loaded yet: next frame
    attempts = 600;
    // push ebp; mov ebp, esp; mov eax|edx, [ebp+8] - the six stolen bytes - and what follows, checked (another
    // DisplaySystem.dll is left alone).
    static const uint8_t kSetRadius[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x89, 0x81, 0xA8, 0x01, 0x00, 0x00, 0xE8};
    static const uint8_t kTesselate[] = {0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x08, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7D, 0x0C};
    auto* setRadius = reinterpret_cast<uint8_t*>(ds) + kSetRadiusRva;
    auto* tesselate = reinterpret_cast<uint8_t*>(ds) + kTesselateRva;
    if (std::memcmp(setRadius, kSetRadius, sizeof(kSetRadius)) != 0 ||
        std::memcmp(tesselate, kTesselate, sizeof(kTesselate)) != 0) {
        Log("ground: DisplaySystem.dll's ground not as expected (another build?) - RVK_GroundRange does nothing");
        return;
    }
    g_tesselate = reinterpret_cast<TesselateFn>(
        HookAt(tesselate, 6, nullptr, 0, reinterpret_cast<void*>(&TesselateHook), "DisplaySystem ground tessellation"));
    if (!g_tesselate) {
        Log("ground: not hooked");
        return;
    }
    g_setRadius = reinterpret_cast<SetRadiusFn>(
        HookAt(setRadius, 6, nullptr, 0, reinterpret_cast<void*>(&SetRadiusHook), "DisplaySystem ground radius"));
    Log("ground: DisplaySystem.dll %s", g_setRadius ? "hooked (RVK_GroundRange)" : "radius not hooked");
    // push ebp; mov ebp, esp; movzx eax, word [ebp+8]; mov edx, [ecx+64h] - the tiles' textures (the native ground).
    static const uint8_t kTile[] = {0x55, 0x8B, 0xEC, 0x0F, 0xB7, 0x45, 0x08, 0x8B, 0x51, 0x64};
    auto* tile = reinterpret_cast<uint8_t*>(ds) + kTileTextureRva;
    if (std::memcmp(tile, kTile, sizeof(kTile)) == 0) g_tileTexture = reinterpret_cast<TileTextureFn>(tile);
    else Log("ground: the tile loader not as expected - no native ground");
    // mov eax, [edi+40h]; push eax; mov ecx, eax; shl ecx, 3; push ecx - the bake's size (RVK_GroundFar).
    static const uint8_t kBake[] = {0x8B, 0x47, 0x40, 0x50, 0x8B, 0xC8, 0xC1, 0xE1, 0x03, 0x51};
    auto* bake = reinterpret_cast<uint8_t*>(ds) + kBakeShiftRva - 8;
    if (std::memcmp(bake, kBake, sizeof(kBake)) == 0) {
        g_bakeShift = bake + 8;
        ApplyFar();
    } else {
        Log("ground: CreateHiTexture not as expected - RVK_GroundFar does nothing");
    }
}

}  // namespace rnative::ground
