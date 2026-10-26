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

// A room's door (in CapturedPiece::kind, over its VisualKind): drawn with the room seen from the street, but not the one
// at its entrance - the street's own door stands there.
constexpr uint32_t kDoorPiece = 0x100;

struct CapturedPiece {
    uint64_t key = 0;                            // its geometry, place and texture (the same piece in two visits)
    uint32_t kind = 0;                           // VisualKind (| kDoorPiece)
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

// A street's ground (docs/street-portal-plan.md phase 2): the native ground's cells in front of its doorway - a cut-out
// of the playfield's NativeGround - with the hour's lightmap over them and the game's two ground passes' states, to
// draw it again through the native ground's path from inside the shop. Written after the pieces: a file without it
// (a room's, or a street recorded before) has none.
struct CapturedGround {
    uint32_t cellsX = 0, cellsZ = 0;             // 0: none
    uint32_t cellX0 = 0, cellZ0 = 0;             // its first cell in the playfield (at cellX0 x cellX, cellZ0 x cellZ)
    float cellX = 4.0f, cellZ = 4.0f;
    std::vector<float> heights;                  // (cellsX + 1) x (cellsZ + 1), rows along +z
    std::vector<uint16_t> codes;                 // cellsX x cellsZ: the cell's tile (into tiles, low 14 bits), its turn
    std::vector<int32_t> tiles;                  // each tile's texture, into Capture::textures (-1: none)
    std::vector<uint16_t> types;                 // each tile's surface type (empty: not known)
    uint32_t lightW = 0, lightH = 0;             // the lightmap over the cells (a texel a cell; empty: none)
    std::vector<uint32_t> light;                 // (ARGB)
    // The game's ground passes as they were drawn: its base pass (unlit, the tiles) and its lighting pass (lit,
    // multiplying the base by the lightmap and the lights).
    bool baseKnown = false, lightKnown = false;
    std::array<uint32_t, d3d::RS_COUNT> baseRs{}, lightRs{};
    std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> baseTss{}, lightTss{};
    d3d::Material material{};
    std::vector<d3d::Light> lights;              // the lights enabled for the lighting pass
};

struct Capture {
    uint32_t playfield = 0;
    // The entrance: its door's bottom centre, and the way into the room from it (horizontal, unit).
    bool entranceKnown = false;
    float entrance[3] = {}, inward[3] = {};
    std::vector<CapturedTexture> textures;
    std::vector<CapturedPiece> pieces;
    CapturedGround ground;                       // (a street's)
};

// The geometry of a piece (its vertices and indices as stored): the same prop wherever it stands.
uint64_t GeometryHash(const CapturedPiece& piece);
bool Save(const char* path, const Capture& capture);
bool Load(const char* path, Capture& capture);
// Its playfield and entrance only (no pieces or textures read).
bool LoadHeader(const char* path, Capture& capture);
uint64_t Hash(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

}  // namespace rvk::portal
