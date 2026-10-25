// A captured interior (docs/shop-portal-plan.md): the static draws of a room's playfield as the game made them - their
// geometry, textures, material, render and stage states and lights - and its entrance, written to
// <client>\randy-vk\portals\<playfield>.rvkp by the capture (portal_capture.cpp) and read back to draw it elsewhere
// (the portal; rvk_demo --portal-view). It holds the game's meshes and textures: local data, never committed.
#pragma once

#include "d3d7.h"
#include "rvk.h"

#include <array>
#include <cstdint>
#include <vector>

namespace rvk::portal {

struct CapturedTexture {
    uint32_t rdbId = 0, width = 0, height = 0;
    Format format = Format::A8R8G8B8;
    uint64_t hash = 0;                           // of its level 0 (the same texture in two visits)
    std::vector<uint8_t> level0;                 // FormatRowBytes x FormatRows bytes
};

struct CapturedPiece {
    uint64_t key = 0;                            // its geometry, place and texture (the same piece in two visits)
    uint32_t kind = 0;                           // VisualKind
    uint32_t fvf = 0, primitive = 0, stride = 0, vertexCount = 0;
    d3d::Matrix world{};
    int32_t texture[2] = {-1, -1};               // into Capture::textures
    d3d::Material material{};
    std::array<uint32_t, d3d::RS_COUNT> rs{};
    std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> tss{};
    std::vector<d3d::Light> lights;              // the game's lights enabled for it
    std::vector<uint8_t> vertices;               // vertexCount x stride
    std::vector<uint16_t> indices;               // empty: not indexed
};

struct Capture {
    uint32_t playfield = 0;
    // The entrance: its door's bottom centre, and the way into the room from it (horizontal, unit).
    bool entranceKnown = false;
    float entrance[3] = {}, inward[3] = {};
    std::vector<CapturedTexture> textures;
    std::vector<CapturedPiece> pieces;
};

bool Save(const char* path, const Capture& capture);
bool Load(const char* path, Capture& capture);
uint64_t Hash(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

}  // namespace rvk::portal
