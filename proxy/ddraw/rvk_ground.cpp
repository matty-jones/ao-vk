// The native ground's game side (docs/native-ground-plan.md, RVK_GroundOn): each playfield's ground read from the
// game's own AnarchyGroundData_t when the game first tessellates it (rnative::ground), its tiles' textures made by the
// game's own loader a few a frame, then handed to rvk (Device::SetNativeGround), which draws it instead of the game's;
// and the hour's baked lightmap as Gamecode hands it to the ground (AnarchyGround_t::SetShadowMap, its import patched).
//
// AnarchyGroundData_t (docs/ground-format.md; re/ds_ground_format.c): +0x31 16-bit heights somewhere, +0x34 / +0x38
// samples across / down, +0x40 cells a patch side, +0x50 / +0x54 patches across / down, +0x58 the patches (0x54 bytes
// each: +0x08 16-bit heights, +0x0C cells a side, +0x10 / +0x14 the heights' zlib size / bytes, +0x28 / +0x2C the
// tile codes'), +0x60 the tile codes' mask, +0x64..+0x68 the tiles' textures (a vector), +0xD8..+0xDC their surface
// types (a vector of u16). AnarchyGround_t +0x198 /
// +0x19C world units a cell, +0x1A0 a height unit (SetScale; byte heights are x 256).
#include "rvk_backend.h"
#include "rvk_settings.h"

#include "native/ground.h"
#include "rvk.h"
#include "threaded.h"

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace rvkproxy {

using rvk::NativeGround;

namespace {

bool g_setting = true;              // RVK_GroundOn
bool g_shadowHooked = false;
int g_shadowAttempts = 0;

// The ground being read: its data and what is decoded so far; its tiles' textures still to make.
struct Reading {
    void* data = nullptr;
    std::shared_ptr<NativeGround> ground;
    std::vector<void*> textures;    // RTexture_t per tile (null: not made yet)
    uint32_t nextTile = 0;
    bool active = false;
} g_reading;
bool g_sent = false;
bool g_haveLightmap = false;        // the hour's lightmap handed over for this ground (SetShadowMap since it was read)

bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

template <typename T>
T At(const void* base, uint32_t offset)
{
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(T));
    return v;
}

// zlib-inflated into exactly `bytes`.
bool Inflate(const void* in, int inBytes, std::vector<uint8_t>* out, size_t bytes)
{
    out->assign(bytes, 0);
    if (!in || inBytes <= 0 || !Readable(in, size_t(inBytes)))
        return false;
    const int n = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out->data()), int(bytes),
                                          static_cast<const char*>(in), inBytes);
    return n == int(bytes);
}

// The playfield's ground from the game's data: heights, tile codes. Null if anything isn't as expected.
std::shared_ptr<NativeGround> ReadGround(const uint8_t* ground, const uint8_t* data)
{
    if (!Readable(ground, 0x1A4) || !Readable(data, 0x70))
        return nullptr;
    const int mw = At<int>(data, 0x34), mh = At<int>(data, 0x38), P = At<int>(data, 0x40);
    const int pw = At<int>(data, 0x50), ph = At<int>(data, 0x54);
    const uint8_t* patches = At<const uint8_t*>(data, 0x58);
    const uint32_t mask = At<uint32_t>(data, 0x60);
    if (P < 1 || P > 64 || pw < 1 || ph < 1 || pw * P != mw - 1 || ph * P != mh - 1 || mw > 8193 || mh > 8193 ||
        !Readable(patches, size_t(pw) * ph * 0x54)) {
        RvkLog("native ground: the playfield's ground isn't one it reads (%d x %d, patches of %d, %d x %d) - the game's",
               mw, mh, P, pw, ph);
        return nullptr;
    }
    auto g = std::make_shared<NativeGround>();
    g->cellsX = uint32_t(mw - 1);
    g->cellsZ = uint32_t(mh - 1);
    g->patch = uint32_t(P);
    g->cellX = At<float>(ground, 0x198);
    g->cellZ = At<float>(ground, 0x19C);
    const float unit = At<float>(ground, 0x1A0);
    if (!(g->cellX > 0.0f && g->cellX < 100.0f && g->cellZ > 0.0f && g->cellZ < 100.0f && unit > 0.0f)) {
        RvkLog("native ground: the ground's scale isn't known yet (%.3f %.3f %.6f) - the game's", g->cellX, g->cellZ, unit);
        return nullptr;
    }
    g->heights.assign(size_t(mw) * mh, 0.0f);
    g->codes.assign(size_t(g->cellsX) * g->cellsZ, 0);
    std::vector<uint8_t> raw;
    for (int pz = 0; pz < ph; ++pz)
        for (int px = 0; px < pw; ++px) {
            const uint8_t* p = patches + size_t(pz * pw + px) * 0x54;
            const bool wide = p[8] != 0;
            const int n = P + 1;
            if (!Inflate(At<const void*>(p, 0x14), At<int>(p, 0x10), &raw, size_t(n) * n * (wide ? 2 : 1))) {
                RvkLog("native ground: patch %d, %d's heights unreadable - the game's", px, pz);
                return nullptr;
            }
            // Deltas: summed down each column, then along each row (wrapping as the game's bytes or shorts do).
            std::vector<uint32_t> h(size_t(n) * n);
            for (int k = 0; k < n * n; ++k)
                h[k] = wide ? uint32_t(raw[2 * k] | raw[2 * k + 1] << 8) : raw[k];
            const uint32_t wrap = wide ? 0xFFFFu : 0xFFu;
            for (int c = 0; c < n; ++c)
                for (int r = 1; r < n; ++r) h[r * n + c] = (h[r * n + c] + h[(r - 1) * n + c]) & wrap;
            for (int r = 0; r < n; ++r)
                for (int c = 1; c < n; ++c) h[r * n + c] = (h[r * n + c] + h[r * n + c - 1]) & wrap;
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c)
                    g->heights[size_t(pz * P + r) * mw + (px * P + c)] = float(wide ? h[r * n + c] : h[r * n + c] << 8) * unit;
            if (!Inflate(At<const void*>(p, 0x2C), At<int>(p, 0x28), &raw, size_t(P) * P * 2)) {
                RvkLog("native ground: patch %d, %d's tiles unreadable - the game's", px, pz);
                return nullptr;
            }
            for (int r = 0; r < P; ++r)
                for (int c = 0; c < P; ++c) {
                    const uint16_t code = uint16_t(raw[2 * (r * P + c)] | raw[2 * (r * P + c) + 1] << 8);
                    g->codes[size_t(pz * P + r) * g->cellsX + (px * P + c)] = uint16_t((code & mask & 0x3FFFu) | (code & 0xC000u));
                }
        }
    return g;
}

// rnative::ground: a playfield's ground, first tessellated (game thread).
void OnNewGround(void* ground, void* data)
{
    if (g_rvk.device) g_rvk.device->SetNativeGround(nullptr);     // (the last playfield's: its textures go)
    rnative::ground::SetNative(false);
    g_sent = false;
    g_haveLightmap = false;
    g_reading = Reading{};
    auto g = ReadGround(static_cast<const uint8_t*>(ground), static_cast<const uint8_t*>(data));
    if (!g)
        return;
    const auto* d = static_cast<const uint8_t*>(data);
    const uint8_t* first = At<const uint8_t*>(d, 0x64);
    const uint8_t* last = At<const uint8_t*>(d, 0x68);
    const size_t tiles = first && last > first ? size_t(last - first) / 4 : 0;
    if (!tiles || tiles > 16384) {
        RvkLog("native ground: the ground has no tiles (%zu) - the game's", tiles);
        return;
    }
    // Each tile's surface type (+0xD8 / +0xDC: tile_type_data, a vector of u16; with it the codes' mask is 0x3FFF):
    // which ground gets pebbles (rvk ground_pebbles.cpp). None without.
    if (Readable(d, 0xE0) && At<uint32_t>(d, 0x60) == 0x3FFFu) {
        const uint8_t* tb = At<const uint8_t*>(d, 0xD8);
        const uint8_t* te = At<const uint8_t*>(d, 0xDC);
        if (tb && te > tb && size_t(te - tb) == tiles * 2 && Readable(tb, tiles * 2)) {
            g->types.resize(tiles);
            std::memcpy(g->types.data(), tb, tiles * 2);
        }
    }
    if (g->types.empty()) RvkLog("native ground: the tiles' surface types aren't there - no pebbles");
    g->id = uint32_t(reinterpret_cast<uintptr_t>(data));
    g_reading.data = data;
    g_reading.ground = g;
    g_reading.textures.assign(tiles, nullptr);
    g_reading.active = true;
    RvkLog("native ground: a playfield's ground read - %u x %u cells of %.1f, %zu tiles to make", g->cellsX, g->cellsZ,
           g->cellX, tiles);
}

// An RTexture_t's rvk texture: its surface_t (+0x30), its DirectDraw surface (ours), its level 0's texture.
rvk::Texture* TextureOf(void* rtexture)
{
    if (!Readable(rtexture, 0x34))
        return nullptr;
    void* surface = At<void*>(rtexture, 0x30);
    if (!Readable(surface, 4))
        return nullptr;
    auto* dds = At<IUnknown*>(surface, 0);
    if (!dds)
        return nullptr;
    RSurface* s = nullptr;
    if (FAILED(dds->QueryInterface(IID_RvkSurface, reinterpret_cast<void**>(&s))) || !s)
        return nullptr;
    RSurface* top = s->top ? s->top : s;
    rvk::Texture* t = top->RvkTexture();
    s->Release();
    return t;
}

// Gamecode's call of AnarchyGround_t::SetShadowMap (its import): the game's, then the map copied for rvk.
using SetShadowMapFn = void(__fastcall*)(void* ground, void* edx, const void* data, int w, int h, float shade);
SetShadowMapFn g_setShadowMap;

void __fastcall SetShadowMapHook(void* ground, void* edx, const void* data, int w, int h, float shade)
{
    g_setShadowMap(ground, edx, data, w, h, shade);
    if (!g_rvk.device)
        return;
    const size_t bytes = size_t((w + 1) / 2) * size_t(std::max(h, 0));
    if (!data || w <= 0 || h <= 0 || w > 16384 || h > 16384 || !Readable(data, bytes)) {
        g_rvk.device->SetNativeGroundLightmap(nullptr, 0, 0, 0.0f);
        return;
    }
    auto copy = std::make_shared<std::vector<uint8_t>>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    g_rvk.device->SetNativeGroundLightmap(copy, uint32_t(w), uint32_t(h), shade);
    g_haveLightmap = true;
}

// The hour's lightmap as the ground already has it, for a ground whose SetShadowMap came before the hook (or before it
// was read): its patches' 64 x 64 paletted textures (AnarchyGround_t +0x5CC, 0x54 a patch, +0x190 / +0x194 across /
// down, +0x188 their size; a patch's texture at +0x10) - our surfaces keep their palette indices - packed again, and
// the shade from the palette (entry 15 = 192 x (1 - shade): SetShadowMap's).
void LightmapFromGround(const uint8_t* ground, const NativeGround& g)
{
    if (!Readable(ground, 0x5D0)) return;
    const int P = At<int>(ground, 0x188), across = At<int>(ground, 0x190), down = At<int>(ground, 0x194);
    const uint8_t* patches = At<const uint8_t*>(ground, 0x5CC);
    if (P != int(g.patch) || across * P != int(g.cellsX) || down * P != int(g.cellsZ) ||
        !Readable(patches, size_t(across) * down * 0x54))
        return;
    const uint32_t w = g.cellsX, h = g.cellsZ, rowBytes = (w + 1) / 2;
    auto packed = std::make_shared<std::vector<uint8_t>>(size_t(rowBytes) * h, 0);
    float shade = -1.0f;
    uint32_t found = 0;
    for (int pz = 0; pz < down; ++pz)
        for (int px = 0; px < across; ++px) {
            void* rt = At<void*>(patches + size_t(pz * across + px) * 0x54, 0x10);
            if (!Readable(rt, 0x34)) continue;
            void* surface = At<void*>(rt, 0x30);
            if (!Readable(surface, 4)) continue;
            auto* dds = At<IUnknown*>(surface, 0);
            RSurface* s = nullptr;
            if (!dds || FAILED(dds->QueryInterface(IID_RvkSurface, reinterpret_cast<void**>(&s))) || !s) continue;
            RSurface* top = s->top ? s->top : s;
            const uint32_t pitch = top->Pitch();
            if (top->palettized && top->desc.dwWidth == uint32_t(P) && top->desc.dwHeight == uint32_t(P) &&
                top->shadow.size() >= size_t(pitch) * P) {
                for (int j = 0; j < P; ++j)
                    for (int i = 0; i < P; ++i) {
                        const uint8_t v = top->shadow[size_t(j) * pitch + i] & 15u;
                        const uint32_t x = uint32_t(px * P + i), z = uint32_t(pz * P + j);
                        uint8_t& b = (*packed)[size_t(z) * rowBytes + x / 2];
                        b = (x & 1) ? uint8_t((b & 0xF0u) | v) : uint8_t((b & 0x0Fu) | v << 4);
                    }
                if (shade < 0.0f && top->palette) shade = 1.0f - float(top->palette->entries[15].peRed) / 192.0f;
                ++found;
            }
            s->Release();
        }
    if (found * 2 < uint32_t(across * down))
        return;                                      // (the ground's shadows off, or not made: the stand-in grey)
    g_rvk.device->SetNativeGroundLightmap(packed, w, h, std::clamp(shade < 0.0f ? 0.5f : shade, 0.0f, 1.0f));
    RvkLog("native ground: the hour's lightmap taken from the ground's own (%u patches)", found);
}

// rnative::ground: the game's frame of its ground (game thread; the ground and its data alive): the tiles' textures
// made a few at a time while reading one, then the ground handed to rvk.
void OnGroundFrame(void* ground, void* data)
{
    if (!g_reading.active || g_sent || data != g_reading.data || !g_rvk.device)
        return;
    for (int made = 0; made < 24 && g_reading.nextTile < g_reading.textures.size(); ++made, ++g_reading.nextTile)
        g_reading.textures[g_reading.nextTile] = rnative::ground::TileTexture(data, g_reading.nextTile);
    if (g_reading.nextTile < g_reading.textures.size())
        return;
    g_reading.ground->tiles.resize(g_reading.textures.size());
    uint32_t missing = 0;
    for (size_t t = 0; t < g_reading.textures.size(); ++t) {
        g_reading.ground->tiles[t] = TextureOf(g_reading.textures[t]);
        missing += !g_reading.ground->tiles[t];
    }
    g_sent = true;
    if (missing * 2 > g_reading.textures.size()) {
        RvkLog("native ground: %u of %zu tiles without a texture - the game's ground", missing, g_reading.textures.size());
        g_reading.active = false;
        return;
    }
    g_rvk.device->SetNativeGround(g_reading.ground);
    if (!g_haveLightmap) LightmapFromGround(static_cast<const uint8_t*>(ground), *g_reading.ground);
    rnative::ground::SetNative(g_setting);
}

}  // namespace

// Each presented frame (game thread): the hooks in place once their DLLs are loaded; a ground the game no longer
// updates (gone: a loading screen, indoors) dropped.
void GroundFrame()
{
    rnative::ground::SetNewGround(&OnNewGround);
    rnative::ground::SetOnGroundFrame(&OnGroundFrame);
    if (!g_shadowHooked && g_shadowAttempts < 6000 && ++g_shadowAttempts % 30 == 1) {
        if (HMODULE gamecode = GetModuleHandleA("Gamecode.dll")) {
            void* old = PatchImport(gamecode, "DisplaySystem.dll", "?SetShadowMap@AnarchyGround_t@@QAEXPBXHHM@Z",
                                    reinterpret_cast<void*>(&SetShadowMapHook));
            g_shadowHooked = true;
            if (old) g_setShadowMap = reinterpret_cast<SetShadowMapFn>(old);
            RvkLog("native ground: Gamecode's SetShadowMap %s", old ? "hooked (the hour's lightmap)" : "not found");
        }
    }
    static uint32_t lastCount = 0, still = 0;
    const uint32_t count = rnative::ground::GroundFrames();
    still = count == lastCount ? still + 1 : 0;
    lastCount = count;
    if (still == 10 && (g_reading.active || g_sent)) {
        if (g_rvk.device) g_rvk.device->SetNativeGround(nullptr);
        rnative::ground::SetNative(false);
        rnative::ground::ForgetGround();
        g_reading = Reading{};
        g_sent = false;
        RvkLog("native ground: the ground is gone (not updated for a while) - dropped");
    }
}

// RVK_GroundOn.
void SetNativeGroundSetting(bool on)
{
    g_setting = on;
    if (g_rvk.device) g_rvk.device->SetNativeGroundOn(on);
    rnative::ground::SetNative(on && g_sent && g_reading.ground);
}

}  // namespace rvkproxy
