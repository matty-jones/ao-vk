// Shared between rvk's implementation files.
#pragma once

#include "rvk.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

#include <algorithm>
#include <string>

namespace rvk::detail {

// Per-draw constants, std430; must match shaders/constants.glsl. Every member is vec4/mat4/uvec4, so std140 and
// std430 lay them out identically.
struct GpuLight {
    float diffuse[4], specular[4], ambient[4], position[4], direction[4], atten[4], spot[4];
};
struct DrawConstants {
    d3d::Matrix view, proj, texMatrix[2];
    float viewport[4];
    float matDiffuse[4], matAmbient[4], matSpecular[4], matEmissive[4];
    float ambient[4];
    float fogColor[4];
    float fogParams[4];
    float tfactor[4];
    float misc[4];
    float eyePos[4];
    float eyeDir[4];
    uint32_t vtx[4];
    uint32_t flags[4];
    uint32_t matSources[4];
    uint32_t stageA[2][4];
    uint32_t stageB[2][4];
    uint32_t lightInfo[4];
    GpuLight lights[Device::kMaxLights];
};
static_assert(sizeof(DrawConstants) % 16 == 0, "std140 block size");
struct DrawTransform {
    d3d::Matrix world;
    d3d::Matrix prevWorld;         // motion vectors: the object's world matrix last frame
    float motion[4];               // x: 1 = world camera; y: 1 = previous positions (binding 8); z: base vertex
    float sway[4];                 // plants: model y of the base, 1 / model height, tip sway (world units), 1 = on
    uint32_t lightMask[4];         // frame lights (bits 0-63 of x, y) that reach the draw's bounding box; z: pushers
                                   // (FrameLights.pushers) near it
    float tess[4];                 // Phong tessellation (characters): level (0 = off), shape, base vertex of binding 10;
                                   // the native ground (D.terrain.y set): the most a cell edge, shape, the distance it
                                   // fades out by, the rock's depth out of the cliffs
    uint32_t texIdx[4];            // bindless (M1): image slots for stage 0, stage 1, bump base, normal map
    uint32_t sampIdx[4];           // bindless (M1): sampler slots for stage 0, stage 1, bump, normal
    uint32_t mat[4];               // PBR material: occlusion/roughness/metallic image slot, the maps' sampler slot,
                                   // kMat* bits, emissive image slot
    float leaf[4];                 // a canopy with leaves (leaves.cpp): its crown's centre (model space), radius (0 = none)
    uint32_t leafSet[4];           // ... its leaves in the pool: first, drawn now | falling-leaf slots << 16, all; the
                                   // ground under it (world y, float bits: where its fallen leaves lie)
    float terrain[4];              // the ground (terrain.cpp, terrain.glsl): x its tile's repeats per world unit; y the
                                   // native ground's tile-code texture's bindless image + 1 (the float's bits; 0 = the
                                   // game's ground); z its normal maps' (lighting pass; 0 = none); w its sky visibility's
                                   // (lighting pass; 0 = none)
    float terrainAt[4];            // ... xyz: its tile's mean colour (the cliffs' rock is tinted to it; -1 unknown) - the
                                   // native ground's base pass: x its cells' mean colours' bindless image + 1 (bits); its
                                   // rocks: y 2, its pebbles: y 3 and z kPebbleRange; w: kTerrainBase / kTerrainLight
                                   // (0 = not mapped)
};
constexpr float kNativeTessDistance = 60.0f;   // world units: the native ground's tessellation gone from here on
// The native ground's pebbles (ground_pebbles.cpp): drawn within this of the eye at most (each size its own range,
// sunk into the ground over its last quarter: ffp.vert, DrawTransform terrainAt.z).
constexpr float kPebbleRange = 40.0f;
enum : uint32_t { kTerrainBase = 1, kTerrainLight = 2 };   // DrawTransform terrainAt[3]: the ground's base pass; its
                                                           // lighting pass, which reads the base's maps (F_BUMPBASE, MAT_BASE)
constexpr float kSkySceneryHaze = 2000.0f;     // the sky's scenery's haze: its colour left e^-(distance / this)
enum : uint32_t { kMatPbr = 1, kMatBase = 2, kMatAlbedo = 4, kMatEmissive = 8, kMatPicked = 16, kMatSky = 32, kMatCloudSheet = 64,
                  kMatHeight = 128 };   // DrawTransform
                                                 // mat[2]: has an ORM map; it is the ground base's; stage 0 draws an
                                                 // albedo map; has an emissive map; its texture was picked
// GPU-driven M2: one record a draw (binding 12). `constIndex` selects the shared DrawConstants it draws with; the
// record is picked by a push constant until M3 replaces that with gl_DrawID.
struct DrawRecord {
    uint32_t constIndex;
    uint32_t pad[3];
    DrawTransform d;
};
static_assert(sizeof(DrawRecord) == 16 + sizeof(DrawTransform), "record layout");
static_assert(sizeof(DrawRecord) % 16 == 0, "record stride");

// One shadow caster's data (shadow.cpp; shadow.vert reads it by gl_InstanceIndex), made once a frame and shared by
// every cascade and cube face it is drawn into: the pass's light view-projection is a push constant.
struct ShadowRecord {
    d3d::Matrix world;
    float alpha[4];
    float sway[4];       // plants: model y of the base, 1 / model height, tip sway, on
    float windModel[4];  // the wind in model space
    float origin[4];     // world x, z of the object; wind time; RVK_LeafCore (a canopy with leaves)
    float leaf[4];       // a canopy with leaves (leaves.cpp): its crown's centre (model space), radius (0 = none)
    float leafWind[4];   // ... the wind's direction (world x, z), branch sway amount, gusts
};
static_assert(sizeof(ShadowRecord) == 160, "shadow record");

enum : uint32_t { F_LIGHTING = 1, F_COLORVERTEX = 2, F_SPECULAR = 4, F_NORMALIZE = 8, F_FOG = 16, F_RANGEFOG = 32,
                  F_LOCALVIEWER = 64, F_TEX0 = 128, F_TEX1 = 256, F_ALPHATEST = 512,
                  F_PERPIXEL = 1024, F_DEBUGLIGHT = 2048, F_LIGHTOVERRIDE = 4096,
                  F_SHADOW = 8192, F_SHADOWCOMP = 16384,
                  F_SHADOWTEX = 32768, F_OVERBRIGHT = 65536, F_OVERBRIGHT2X = 131072,
                  F_HDR = 262144, F_GLOW = 524288, F_GLOWALPHA = 1048576,
                  F_BUMP = 2097152, F_BUMPBASE = 4194304, F_FOLIAGE = 8388608, F_EMISSIVE = 16777216,
                  F_CUTOUT = 33554432, F_SHADOWCHEAP = 67108864, F_VERTEXSUN = 134217728,
                  F_NORMALMAP = 268435456, F_CHARACTER = 536870912,
                  F_NOALBEDO = 1073741824,     // the albedo attachment isn't written (a multiplying pass keeps it)
                  F_BLENDED = 2147483648u };   // blended SRCALPHA / INVSRCALPHA (the cut-out pre-pass: opaque at 1)

constexpr uint32_t kFrameLights = 64;
constexpr uint32_t kPushers = 16;            // info.y of them used
struct FrameLights {               // binding 4: per-frame data (constants.glsl FrameLights)
    uint32_t info[4];
    d3d::Matrix shadowViewProj[4]; // world -> each sun shadow cascade
    float cascadeTexel[4];         // world size of a texel of each cascade
    float cascadeDepth[4];         // world units per unit of each cascade's depth (soft shadows)
    float effects[4];              // light through leaves, night glow (x darkness), sun shadow softness, plant push
    float wind[4];                 // plants' sway: direction x, z, time (s), strength
    float taa[4];                  // temporal anti-aliasing: this frame's jitter (clip x, y per w), noise offset; wind time last frame
    float shadowParams[4];         // enabled, strength, cascade count, point light shadow strength
    float sunDir[4];               // w: light headroom (F_OVERBRIGHT)
    float sunColor[4];             // the shadow-casting sun's colour (0 = none)
    float pbr[4];                  // PBR materials: direct specular, ambient specular, reflections, occlusion map strength
    float pbr2[4];                 // x: debug view (0 = off), y: 1 = PBR on, z: emissive maps' brightness, w: the
                                   // environment probe (bits: draw.cpp FillFrameLights)
    d3d::Matrix prevView, prevProj; // motion vectors: the world camera last frame (applied like view and proj, so a
                                   // still camera's motion is exactly zero)
    float pushers[kPushers][4];    // what plants bend away from (characters' feet and trails): world x, y, z, seconds
                                   // since a character was there
    float pusherBorn[kPushers];    // seconds since each point was made (a character walking on makes new ones)
    float leaves[4];               // leaves (leaves.cpp): branch sway, sprig flutter, gusts, canopy core cut
    float leafView[4];             // ... their distance (none beyond; thinning out from half of it), on, falling leaves
    float clouds[4];               // the clouds' shadow map (clouds.cpp): its corner x, z (world), 1 / its size, its
                                   // bindless image + 1 | sampler << 20 (as the float's bits; 0 = none)
    float clouds2[4];              // ... the cloud layer's height (world y), the shadow's strength
    float cloudTint[4];            // the game's cloud sheets, drawn with the layer's light (cloud_light.glsl): the
    float cloudSky[4];             // clouds' colour (rgb, w brightness); the sky's light on them (rgb, w the sheets'
    float cloudAir[4];             // strength: 0 = none); the air's colour (rgb, w: the horizon's haze)
    float moon[4];                 // the moonlight (night.cpp): its colour on the ground and grass (0 = none); w: 1 =
                                   // the shadow map is the moon's (sunDir: the moon's light), not the sun's
    float sunTrue[4];              // towards the sun, below the horizon too (the clouds' light: sunset, afterglow);
                                   // w: how much it is night (0 day .. 1)
    float terrainSun[4];           // the terrain's shade from a low sun (terrain_sun.cpp): its map's corner x, z, 1 / its
                                   // size, its bindless image + 1 | sampler << 20 (the float's bits; 0 = none)
    float terrainSun2[4];          // ... x: the eye's own (0 shaded .. 1), y: its weight (0 high sun .. 1 low)
    float windFrame[4];            // xy: the wind's patterns' frame (Device::WindFrame); z: a sandstorm's spreading of the sun
    float flash[4];                // lightning (rain.cpp Lightning): its light on the land (rgb), w: the clouds' glow
    float flashView[4];            // ... towards the strike (view space), w: how far
    float flashProj[4];            // ... the projection's m[0][0], m[1][1], m[2][2], m[3][2]
    float flashWorld[4];           // ... the strike (world; the storm's fog lit round it)
    float rainNear[4];             // x: 1 = the tone mapping's rain layer (binding 16) holds this frame's rain; y: how much
                                   // a thunderstorm hides the sun (0..1: Device::StormOvercast); z: a storm's blowing
                                   // (leaves torn off and carried - ffp.vert FallingLeaf)
    float wet[4];                  // the wet ground (rain.cpp): x how soaked, y the puddles, z the rain's clock (their
                                   // ripples), w the cover map's heights' reference (world y)
    float cover[4];                // the rain's cover map (sand_map.cpp RainCoverMap): corner x, z, 1 / size, bindless
                                   // image + 1 | sampler << 20 (the float's bits; 0 = none)
    float neon[4];                 // the tone mapping's neon look (tonemap.frag): x how much saturated light keeps
                                   // its colour (RVK_NeonSat); yzw a rainbow (towards the point opposite the sun,
                                   // world camera view, x its strength: sky.cpp UpdateRainbow)
    float sky[4][4];               // the sky's model (sky.cpp UpdateSkyModel; sky_common.glsl s0..s3)
    float skyView[3][4];           // the world camera's rotation: row i . a view-space direction = its world part i
    float dawn[2][4];              // low sun's light (sky.cpp): the sun's tint (rgb), w how much; the sky's (shade)
                                   // tint, w the golden hour's grade (tonemap.frag)
    float phase[4];                // the dusk's phases (sky.cpp UpdateSunClock): golden hour, afterglow, blue hour, night
    float skyRing[8][4];           // our sky's horizon all round (atmosphere.cpp): from +x towards +z, w 1 when known
    float skyUp[4];                // our sky over the land (overhead and 35 degrees up), w 1 when known
    float dusk[4];                 // x: RVK_Afterglow (the clouds' sunset colours), y the blue hour's grade, z w the sun on
                                   // the world camera's screen (NDC; 1e3 behind it)
    float terrain[4];              // the ground's texturing (terrain.glsl): cliffs (RVK_CliffTex), the faces' repeats
                                   // hidden (RVK_TileHide), large-scale variation (RVK_GroundVary), normal maps' strength
                                   // (0 off: the native ground's tiles')
    float terrain2[4];             // ... x: the native ground's sky visibility's strength (RVK_GroundAo); y the hour's
                                   // lightmap before this one (its bindless image + 1, the float's bits; 0 none), z how
                                   // far it has faded into this one (0..1); w the grain up close (RVK_GroundGrain)
    float terrainRock[4];          // the cliffs' rock (terrain.cpp; RVK_CliffRock): its albedo, normal and ORM maps'
                                   // bindless images + 1 (the floats' bits; 0 = none), world units a repeat
    float terrainRock2[4];         // ... its mean colour (rgb), its normal map's strength
    float terrainLight[4];         // the native ground's lightmap as a + b.n of its slope (native_ground.cpp
                                   // FitNativeLight): xyz b turned to the sun's bearing now (b when there is none), w a (0: no fit)
    float terrainLight2[4];        // ... xyz b as baked, w how much the slopes follow the sun (RVK_GroundSun; 0 none)
    float terrain3[4];             // ... x: the native ground's tiles blended across their cells' edges
                                   // (RVK_GroundBlend); y the side-loaded height maps' depth, in repeats (RVK_Parallax;
                                   // 0 off), z its marches' step counts' scale (RVK_DepthSteps)
    float sand[4];                 // the sand (terrain.glsl): x its ripples (RVK_SandRipple), y the dunes lit by the sun
                                   // now (RVK_DuneLight), z its glints (RVK_SandGlint), w the wind's bearing (radians)
    float desert[4];               // a desert's air (Device::DesertAir; tonemap.frag): x its dust (RVK_DesertAir), y the
                                   // heat shimmer (RVK_HeatShimmer), each as much as now; z, w the projection's m00, m11
    float sand2[4];                // the sand, more (parallax.glsl): x its ripples smoothed (RVK_RippleSoft); y the
                                   // footprints' map (sand_prints.cpp): its bindless image + 1 | sampler << 20 (the
                                   // float's bits; 0 = none), z, w its corner (world x, z)
    float pick[4];                 // texture picking (Ctrl+Shift+I): x the native ground's tile flashing + 1 (0: none);
                                   // y the light grid's bindless image + 1 | sampler << 20 (the float's bits; 0 none),
                                   // z, w the lights too large for it (mask: low, high word's bits)
    float sand3[4];                // the sand, more: x its ripples' depth in the march (RVK_RippleDepth); y the sun the
                                   // clouds let through (its glints); z, w unused
    float fogBanks[4];             // fog banks (mist.cpp FogBankParams): their noise at the eye (x, z), how deep, how much
    float lightGrid[4];            // the frame lights' grid (Device::LightGridLights): its corner x, z, cells across, down
    float cond[4];                 // the world's conditions (rvk.h Conditions): x the clouds' night; y, z, w unused
    float screen[4];               // x, y scene pixels per back-buffer pixel (RVK_RenderScale: the motion vectors); z, w unused
    GpuLight lights[kFrameLights];
};

// Vertex layout of a D3D flexible vertex format.
struct FvfLayout {
    uint32_t stride = 0;
    int offset[6] = {-1, -1, -1, -1, -1, -1};   // position, normal, diffuse, specular, tex0, tex1
    VkFormat format[6] = {};
};
FvfLayout DecodeFvf(uint32_t fvf);

// A ground chunk's texture coordinates fitted to the world (terrain.cpp; TerrainFit in rvk.h).
TerrainFit FitTerrain(const void* vertices, uint32_t vertexCount, const FvfLayout& layout, const uint16_t* indices,
                      uint32_t indexCount);

// Vulkan format and view swizzle for a texture format.
struct FormatInfo {
    VkFormat vk;
    VkComponentMapping swizzle;
    uint32_t blockBytes;    // bytes per pixel, or per 4x4 block for compressed formats
    bool compressed;
};
const FormatInfo& GetFormatInfo(Format format);

bool Check(VkResult r, const char* what, std::string* error);
uint64_t HashBytes(const void* data, size_t size, uint64_t h);   // fast 64-bit hash of a byte range
void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess);
d3d::Matrix Identity();
d3d::Matrix MulMatrix(const d3d::Matrix& a, const d3d::Matrix& b);   // D3D order: v * a * b
bool InvertMatrix(const d3d::Matrix& m, d3d::Matrix* out);
// A model-space box through a matrix: the world box around it (shadow.cpp).
void TransformBounds(const float mn[3], const float mx[3], const d3d::Matrix& m, float outMin[3], float outMax[3]);
VkPrimitiveTopology TopologyOf(uint32_t d3dPrimitive);
uint32_t TopologyClassOf(uint32_t d3dPrimitive);   // 0 points, 1 lines, 2 triangles
// Whether a mesh is one plane (a sign, a poster: draw.cpp), positions first in its vertices.
bool OnePlane(uint32_t primitive, uint32_t stride, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
              uint32_t indexCount);
// Clip-space test of a world-space box's corners against a view-projection: +1 all inside, -1 all outside one
// plane, 0 otherwise. depth: also test the near and far planes.
int BoxInClip(const float mn[3], const float mx[3], const d3d::Matrix& vp, bool depth);
// BoxInClip(mn, mx, vp, false) == -1 for an affine view-projection (orthographic: every point's w is 1) - the box's
// centre and half extents through the matrix, not its eight corners (the sun cascades' cull, every caster each).
bool BoxOutsideAffine(const float mn[3], const float mx[3], const d3d::Matrix& vp);

// GLSL's smoothstep: 0 at a, 1 at b, eased between (a > b runs backwards).
inline float Smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The native ground's cells (native_ground.cpp): corners a (x, z), b (x + 1, z), c (x, z + 1), d (x + 1, z + 1). Each
// is split along b - c as the game splits it - what characters stand on - unless it is steep (a diagonal's ends more
// than a cell apart in height: a cliff, which no one stands on) and the a - d diagonal's ends are the nearer in
// height: a cliff at a slant to the cells then meets the flat in a straight line, not a row of teeth.
inline bool NativeCellAcross(float a, float b, float c, float d, float cell)
{
    const float ad = a > d ? a - d : d - a, bc = b > c ? b - c : c - b;
    return (ad > cell || bc > cell) && ad < bc - 0.01f;
}
// ... its height at (fx, fz) within it (0..1), on the triangles it is drawn with.
inline float NativeCellHeight(float a, float b, float c, float d, float cell, float fx, float fz)
{
    if (NativeCellAcross(a, b, c, d, cell))
        return fz > fx ? a + fz * (c - a) + fx * (d - c) : a + fx * (b - a) + fz * (d - b);
    return fx + fz < 1.0f ? a + fx * (b - a) + fz * (c - a) : d + (1.0f - fx) * (c - d) + (1.0f - fz) * (b - d);
}

}  // namespace rvk::detail
