// Frame dump: one line per 3D draw of a frame with the state that decides how it is lit, its world
// position and its screen rectangle, plus the lights it uses. For finding out why a surface is (not) lit.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

void Device::RequestFrameDump(const std::string& path, int frames)
{
    m_dumpPath = path;
    m_dumpBurstLeft = m_dumpBurstIndex = 0;
    if (frames > 1) {                            // (a run: <base>-00.txt, -01.txt ... - what changes frame to frame)
        const size_t dot = path.rfind('.');
        m_dumpBurstBase = dot != std::string::npos ? path.substr(0, dot) : path;
        m_dumpBurstLeft = frames - 1;
        m_dumpBurstIndex = 0;
        m_dumpPath = m_dumpBurstBase + "-00.txt";
    }
}

void Device::BeginFrameDump()
{
    m_dumpFile = std::fopen(m_dumpPath.c_str(), "w");
    m_dumpOpenPath = m_dumpPath;
    if (m_sandMap.valid && m_dumpBurstIndex == 0) {                       // (the sand's shelter map beside it: <dump>-sand.bmp)
        std::string sand = m_dumpPath;
        const size_t dot = sand.rfind('.');
        DumpSandMap((dot != std::string::npos ? sand.substr(0, dot) : sand) + "-sand.bmp");
    }
    m_dumpPath.clear();
    if (!m_dumpFile) return;
    m_dumpDraw = 0;
    m_dumpGroundTextures.clear();
    m_dumpLightmaps.clear();
    m_dumpedLights.assign(m_lights.size(), {});
    m_dumpedLightValid.assign(m_lights.size(), false);
    const auto& v = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    std::fprintf(m_dumpFile, "# rvk frame dump; eye (%.2f %.2f %.2f); pixel lighting %d; render scale %u%% (scene %ux%u)\n"
                             "# D draw: prim fvf verts idx | lit lights(idx:nearest vertex distance) | material diffuse/ambient/emissive,"
                             " sources d/a/s/e, colorvertex, global ambient | stages op/arg1/arg2 | tex | blend src/dst | z test/write/func |"
                             " world translation | screen rect\n"
                             "# L index: type position direction range atten0/1/2 diffuse ambient\n",
                 eye[0], eye[1], eye[2], m_pixelLighting ? 1 : 0, m_renderScale, m_scene ? m_scene->m_width : 0u,
                 m_scene ? m_scene->m_height : 0u);
}

// GPU memory in one line: each memory heap's usage against the driver's budget for it (with VK_EXT_memory_budget the
// driver's own figures for this process; else what rvk has allocated there), then what rvk's allocations are for - the
// game's textures, render targets (rvk's own and the game's), the frame rings, the static-geometry and shadow-caster
// arenas, the grass and leaf pools - and the rest (shadow maps, the HDR scene's targets, particles, skinning, water...).
std::string Device::GpuMemoryReport() const
{
    if (!m_allocator)
        return "gpu memory: no allocator";
    constexpr double kMB = 1.0 / (1024.0 * 1024.0);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(m_allocator, &memory);
    const VkPhysicalDeviceMemoryProperties& props = *memory;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(m_allocator, budgets);
    std::string out = m_memoryBudget ? "gpu memory (driver's figures):" : "gpu memory (rvk's allocations only):";
    char buf[512];
    VkDeviceSize total = 0;
    for (uint32_t h = 0; h < props.memoryHeapCount; ++h) {
        const bool local = (props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        total += budgets[h].statistics.allocationBytes;
        if (!budgets[h].usage && !budgets[h].statistics.blockBytes)
            continue;
        std::snprintf(buf, sizeof(buf), " heap %u (%s, %.0f MB): used %.0f MB of %.0f MB budget, rvk %.0f MB in %.0f MB "
                      "of blocks |", h, local ? "GPU" : "system", double(props.memoryHeaps[h].size) * kMB,
                      double(budgets[h].usage) * kMB, double(budgets[h].budget) * kMB,
                      double(budgets[h].statistics.allocationBytes) * kMB, double(budgets[h].statistics.blockBytes) * kMB);
        out += buf;
    }
    VkDeviceSize rings = 0, staticArena = 0, casterArena = 0;
    for (const Frame& f : m_frames) rings += f.ringSize;
    for (const auto& c : m_staticArena) staticArena += c->size;
    for (const auto& c : m_casterArena) casterArena += c->size;
    const VkDeviceSize grass = VkDeviceSize(m_grassPoolBlades) * 48, leaves = VkDeviceSize(m_leafPoolLeaves) * 32;
    auto bytesOf = [&](VmaAllocation_T* a) -> VkDeviceSize {
        if (!a) return 0;
        VmaAllocationInfo info;
        vmaGetAllocationInfo(m_allocator, a, &info);
        return info.size;
    };
    const VkDeviceSize sunShadow = bytesOf(m_shadowAllocation), pointShadow = bytesOf(m_cubeAllocation),
                       depth = bytesOf(m_depthAllocation);
    VkDeviceSize skin = 0, particles = bytesOf(m_particleStateAllocation) + bytesOf(m_particleIndicesAllocation);
    for (const Frame& f : m_frames) skin += f.skinArenaSize;
    for (VmaAllocation_T* a : m_particleQuadsAllocation) particles += bytesOf(a);
    const VkDeviceSize listed = m_textureBytes[0] + m_textureBytes[1] + rings + staticArena + casterArena + grass + leaves +
                                sunShadow + pointShadow + depth + skin + particles;
    std::snprintf(buf, sizeof(buf),
                  " rvk: textures %.0f MB (%u), render targets %.0f MB (%u), depth %.0f MB, sun shadow map %.0f MB, point "
                  "shadow maps %.0f MB, rings %.0f MB, static arena %.0f MB, caster arena %.0f MB, skinning %.0f MB, "
                  "particles %.0f MB, grass %.1f MB, leaves %.1f MB, other %.0f MB",
                  double(m_textureBytes[0]) * kMB, m_textureCount[0], double(m_textureBytes[1]) * kMB, m_textureCount[1],
                  double(depth) * kMB, double(sunShadow) * kMB, double(pointShadow) * kMB,
                  double(rings) * kMB, double(staticArena) * kMB, double(casterArena) * kMB, double(skin) * kMB,
                  double(particles) * kMB, double(grass) * kMB,
                  double(leaves) * kMB, double(total > listed ? total - listed : 0) * kMB);
    out += buf;
    return out;
}

// The ground's base textures drawn this frame, in the order first drawn: listed in the dump, their colour grids side by
// side in <dump>-ground.bmp (12 a row, each 16 x 16 shown 4 times as big, in the listed order from the top left).
void Device::DumpGroundTextures(const std::string& path)
{
    const std::vector<const Texture*>& ts = m_dumpGroundTextures;
    for (size_t k = 0; k < ts.size(); ++k)
        std::fprintf(m_dumpFile, "# ground texture %zu: [%p] %ux%u\n", k, static_cast<const void*>(ts[k]),
                     ts[k]->Width(), ts[k]->Height());
    if (ts.empty()) return;
    constexpr int kTile = 64, kGap = 4, kPerRow = 12;
    const int rows = int((ts.size() + kPerRow - 1) / kPerRow);
    const int w = kPerRow * (kTile + kGap), h = rows * (kTile + kGap);
    std::vector<uint8_t> px(size_t(w) * h * 3, 40);
    for (size_t k = 0; k < ts.size(); ++k) {
        const Texture* t = ts[k];
        const int ox = int(k % kPerRow) * (kTile + kGap), oy = int(k / kPerRow) * (kTile + kGap);
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x) {
                const uint32_t tx = uint32_t(x) * t->m_thumbW / kTile, ty = uint32_t(y) * t->m_thumbH / kTile;
                const uint8_t* c = t->m_thumb.data() + (size_t(ty) * t->m_thumbW + tx) * 3;
                uint8_t* o = px.data() + (size_t(oy + y) * w + ox + x) * 3;
                o[0] = c[2]; o[1] = c[1]; o[2] = c[0];   // (BMP: blue, green, red)
            }
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    const uint32_t rowBytes = (uint32_t(w) * 3 + 3) & ~3u, imageSize = rowBytes * uint32_t(h);
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + imageSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    header[26] = 1;
    header[28] = 24;
    put32(34, imageSize);
    std::fwrite(header, 1, 54, file);
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = h - 1; y >= 0; --y) {           // (BMP rows bottom-up)
        std::memcpy(row.data(), px.data() + size_t(y) * w * 3, size_t(w) * 3);
        std::fwrite(row.data(), 1, rowBytes, file);
    }
    std::fclose(file);
}

// The ground's lightmaps drawn this frame (<dump>-lightmaps.bin, little endian): "RVKLM1\0\0", u32 count; each: f32
// u[4], v[4] (its coordinates set 1 = x, y, z, 1 times these), u32 width, height, then width x height RGB bytes.
void Device::DumpLightmaps(const std::string& path)
{
    if (m_dumpLightmaps.empty()) return;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    std::fwrite("RVKLM1\0\0", 1, 8, file);
    const uint32_t count = uint32_t(m_dumpLightmaps.size());
    std::fwrite(&count, 4, 1, file);
    for (const DumpedLightmap& l : m_dumpLightmaps) {
        std::fwrite(l.u, 4, 4, file);
        std::fwrite(l.v, 4, 4, file);
        const uint32_t wh[2] = {l.texture->m_thumbW, l.texture->m_thumbH};
        std::fwrite(wh, 4, 2, file);
        std::fwrite(l.texture->m_thumb.data(), 1, l.texture->m_thumb.size(), file);
    }
    std::fclose(file);
    std::fprintf(m_dumpFile, "# lightmaps: %u in %s\n", count, path.c_str());
}

void Device::EndFrameDump()
{
    if (!m_dumpFile) return;
    {
        const size_t dot = m_dumpOpenPath.rfind('.');
        const std::string base = dot != std::string::npos ? m_dumpOpenPath.substr(0, dot) : m_dumpOpenPath;
        DumpGroundTextures(base + "-ground.bmp");
        DumpLightmaps(base + "-lightmaps.bin");
    }
    std::fprintf(m_dumpFile, "# %s\n", GpuMemoryReport().c_str());
    std::fprintf(m_dumpFile, "# end: %u draws; %zu casters this frame, %zu remembered (%u drawn out of view last frame);"
                             " forgotten since start: %u in view but not drawn, %u far away\n",
                 m_dumpDraw, m_casters.size(), m_casterCache.size(), m_cachedCastersDrawn, m_forgottenInView, m_forgottenFar);
    {
        // The remembered casters the shadow passes draw though the game didn't this frame (out of view), biggest first:
        // where, how big, what mesh, how long since the game drew it, its recorded sway (a remembered wall's shadow
        // changing while the camera looked away - user, 2026-10-13).
        std::vector<std::pair<float, std::pair<uint64_t, const CachedCaster*>>> out;
        for (const auto& [key, e] : m_casterCache) {
            if (e.lastSeen == m_frameNumber)
                continue;
            float size = 0.0f;
            for (int j = 0; j < 3; ++j) size = std::max(size, e.boundsMax[j] - e.boundsMin[j]);
            out.push_back({size, {key, &e}});
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::fprintf(m_dumpFile, "# remembered casters out of view: %zu (biggest first)\n", out.size());
        for (size_t i = 0; i < out.size() && i < 300; ++i) {
            const CachedCaster& e = *out[i].second.second;
            std::fprintf(m_dumpFile,
                         "R %016llx: size %.1f box (%.1f %.1f %.1f)..(%.1f %.1f %.1f) at (%.2f %.2f %.2f) v %u i %u kind %u "
                         "unseen %llu frames, sway %zu%s\n",
                         (unsigned long long)out[i].second.first, double(out[i].first), double(e.boundsMin[0]),
                         double(e.boundsMin[1]), double(e.boundsMin[2]), double(e.boundsMax[0]), double(e.boundsMax[1]),
                         double(e.boundsMax[2]), double(e.world.m[3][0]), double(e.world.m[3][1]), double(e.world.m[3][2]),
                         e.vertexCount, e.indexCount, e.kind, (unsigned long long)(m_frameNumber - e.lastSeen),
                         e.sway.size(), e.texture ? " cut-out" : "");
        }
    }
    {
        // Every sun shadow item near the eye (within 120 units): this frame's or remembered, shadow-only or drawn, its
        // key, where, its box, and the cascades it went into (bits: 1 nearest .. 8 widest) - which casters a shadow had.
        std::fprintf(m_dumpFile, "# placed objects (statels.cpp): playfield %u, %zu pieces, %u cast this frame, %u drawn ones not "
                                 "remembered; mode %u, placement %s\n",
                     m_statelPlayfield, m_statelItems.size(), m_statelItemsThisFrame, m_statelCoveredLast, m_statelMode,
                     StatelPlacementState().c_str());
        DumpRainCover(m_dumpFile, m_frameEye[0], m_frameEye[2]);
        for (const WaterBody& b : m_waterBodies)        // (the characters near: the player under the camera among them)
            if (std::hypot(b.x - m_frameEye[0], b.z - m_frameEye[2]) < 6.0f)
                DumpRainCover(m_dumpFile, b.x, b.z);
        std::fprintf(m_dumpFile, "# sun shadow items within 120 of the eye (T this frame, M remembered, P placed; cascades bits):\n");
        for (const ShadowItem& it : m_shadowItems) {
            float d2 = 0.0f;
            for (int j = 0; j < 3; ++j) {
                const float d = std::max(std::max(it.boundsMin[j] - m_frameEye[j], m_frameEye[j] - it.boundsMax[j]), 0.0f);
                d2 += d * d;
            }
            if (d2 > 120.0f * 120.0f)
                continue;
            std::fprintf(m_dumpFile, "%c %016llx: at (%.2f %.2f %.2f) box (%.1f %.1f %.1f)..(%.1f %.1f %.1f) v %u i %u%s cascades %x\n",
                         !it.cached ? 'T' : (it.cacheKey >> 48) == 0x5354 ? 'P' : 'M', (unsigned long long)it.cacheKey, double(it.world.m[3][0]),
                         double(it.world.m[3][1]), double(it.world.m[3][2]), double(it.boundsMin[0]), double(it.boundsMin[1]),
                         double(it.boundsMin[2]), double(it.boundsMax[0]), double(it.boundsMax[1]), double(it.boundsMax[2]),
                         it.vertexCount, it.indexCount, it.texture ? " cut-out" : "", it.cascades);
        }
    }
    std::fprintf(m_dumpFile, "# sun shadow pass: %u items (%u static, %u animated); drew %u item-cascades (%u static,"
                             " %u animated), culled %u; %.2f ms collect, %.2f cull, %.2f draw; M4 %u calls\n",
                 m_shadowItemsCount, m_shadowStaticItems, m_shadowAnimatedItems, m_shadowDrawn, m_shadowStaticDrawn,
                 m_shadowAnimatedDrawn, m_shadowCulled, m_shadowCollectMs, m_shadowCullMs, m_shadowDrawMs,
                 m_shadowGroupCalls);
    std::fprintf(m_dumpFile, "# instancing: %u runs of >=2 identical static draws; %u draws could merge away (longest run"
                             " %u)\n", m_batchRuns, m_batchMerged, m_batchMaxRun);
    std::fprintf(m_dumpFile, "# M3 batching: %u indirect calls for %u draws (%u single); %u indexed draws left direct\n",
                 m_groupCalls, m_groupDraws, m_singleDraws, m_frameDraw - m_groupDraws);
    {
        static const char* const rewhy[kRewhy] = {"state set", "new arena", "fvf", "textures", "terrain", "label",
                                                  "carried light", "bump base", "foliage lod", "normal map", "character",
                                                  "opaque texture", "texture holes", "sky scenery", "blend"};
        static const char* const split[kSplitParts + 2] = {"constants", "pipeline", "primitive", "fvf", "vb", "ib",
                                                           "prev positions", "smooth normals", "frame lights",
                                                           "prepassed", "backdrop", "cull", "z", "blend", "discard/label",
                                                           "terrain pass", "ended elsewhere", "not batchable"};
        std::fprintf(m_dumpFile, "# constant blocks made again, by what changed:");
        for (int i = 0; i < kRewhy; ++i)
            if (m_rewriteWhy[i]) std::fprintf(m_dumpFile, " %s %u", rewhy[i], m_rewriteWhy[i]);
        std::fprintf(m_dumpFile, "\n# batches ended, by what differed:");
        for (int i = 0; i < kSplitParts + 2; ++i)
            if (m_splitWhy[i]) std::fprintf(m_dumpFile, " %s %u", split[i], m_splitWhy[i]);
        std::fprintf(m_dumpFile, "\n");
    }
    std::fprintf(m_dumpFile, "# mesh info: %u draws identified by static snapshot, %u by a content hash\n",
                 m_meshStaticDraws, m_meshHashedDraws);
    if (m_lightMaskVerify)
        std::fprintf(m_dumpFile, "# light mask verify: %u differences from every-light\n", m_lightMaskDiff);
    if (m_hdr)
        std::fprintf(m_dumpFile, "# hdr: scene phase ended at draw %u (fvf 0x%X)%s; %u additive draws fed the glow\n",
                     m_sceneEndDraw, m_sceneEndFvf,
                     m_sceneEndDraw ? "" : " - no interface draw (tone mapped at the end of the frame)", m_glowDraws);
    std::fprintf(m_dumpFile, "# point shadows (rendered last frame, %u caster draws; daylight %.2f -> strength %.2f):",
                 m_pointShadowDraws, m_daylight, PointShadowStrength());
    for (uint32_t i = 0; i < m_pointShadowCount; ++i) {
        const PointShadowLight& l = m_pointShadowLights[i];
        std::fprintf(m_dumpFile, " cube %u at (%.1f %.1f %.1f) r%.1f;", i + 1, l.position[0], l.position[1], l.position[2], l.range);
    }
    std::fprintf(m_dumpFile, "\n");
    // Carrier detection this frame: each light's carrier and the small casters under it (g = run of draws).
    for (const CapturedLight& c : m_lightsCur) {
        if (c.light.range < 1.0f)
            continue;
        bool cube = false;
        for (uint32_t i = 0; i < m_pointShadowCount; ++i)
            cube |= m_pointShadowLights[i].range == c.light.range &&
                    std::fabs(m_pointShadowLights[i].position[0] - c.light.position.x) < 1.0f &&
                    std::fabs(m_pointShadowLights[i].position[2] - c.light.position.z) < 1.0f;
        if (!c.hasCarrier && !cube)
            continue;
        const d3d::Vector& l = c.light.position;
        std::fprintf(m_dumpFile, "# light (%.1f %.1f %.1f) r%.1f carrier%s:", l.x, l.y, l.z, c.light.range,
                     c.owner ? " (the scene's, exact)" : "");
        if (c.hasCarrier)
            std::fprintf(m_dumpFile, " g%u origin (%.2f %.2f %.2f), %.2f under the light;", c.carrierGroup,
                         c.carrier[0], c.carrier[1], c.carrier[2], l.y - c.carrier[1]);
        else
            std::fprintf(m_dumpFile, " none;");
        std::fprintf(m_dumpFile, " casters within 1.5 sideways (g dy sideways extent):");
        for (const ShadowItem& it : m_shadowItems) {
            float dx = it.world.m[3][0] - l.x, dz = it.world.m[3][2] - l.z, dy = it.world.m[3][1] - l.y;
            float extent = std::max({it.boundsMax[0] - it.boundsMin[0], it.boundsMax[1] - it.boundsMin[1],
                                     it.boundsMax[2] - it.boundsMin[2]});
            if (dx * dx + dz * dz >= 1.5f * 1.5f || dy < -3.5f || dy > 1.0f || extent >= 3.0f) continue;
            std::fprintf(m_dumpFile, " (%s%u %.2f %.2f %.2f%s)", it.cached ? "c" : "g", it.cached ? 0u : it.group, dy,
                         std::sqrt(dx * dx + dz * dz), extent, IsCarrierItem(c, it) ? " carrier" : "");
        }
        std::fprintf(m_dumpFile, "\n");
    }
    uint32_t effects = 0;
    for (const ParticleBlock& b : m_particleBlocks)
        if (b.key && b.lastSeen == m_frameNumber) ++effects;
    std::fprintf(m_dumpFile, "# particles: %u effects announced, %u particle draws, %zu textures kept for fading particles;"
                             " fading particles drawn at: %s\n", effects, m_particleDraws, m_particleHeldTextures.size(),
                 m_particleOrphanTrigger);
    double now = 0.0;
    {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        now = double(t.QuadPart) / double(f.QuadPart);
    }
    for (uint32_t i = 0; i < m_particleBlocks.size(); ++i) {
        const ParticleBlock& b = m_particleBlocks[i];
        if (!b.key) continue;
        std::fprintf(m_dumpFile, "#   effect %llx block %u: announced %llu frames ago, sprite alive %.2f s ago, simulated %d,"
                                 " drawn this frame %d, draw state %d (target %s), may live %d, fading %d (%u draws)\n",
                     (unsigned long long)b.key, i, (unsigned long long)(m_frameNumber - b.lastSeen),
                     b.lastAliveTime > 0.0 ? now - b.lastAliveTime : -1.0, b.simulated ? 1 : 0,
                     b.drawnFrame == m_frameNumber ? 1 : 0, b.haveState ? 1 : 0,
                     !b.haveState ? "-" : b.state.target == m_scene ? "scene" : b.state.target == m_ldrMain ? "main" : "other",
                     ParticlesMayLive(b, now) ? 1 : 0, b.fading ? 1 : 0, b.fadingDraws);
    }
    std::fclose(m_dumpFile);
    m_dumpFile = nullptr;
    if (m_dumpBurstLeft > 0) {
        --m_dumpBurstLeft;
        char n[16];
        std::snprintf(n, sizeof(n), "-%02d.txt", ++m_dumpBurstIndex);
        m_dumpPath = m_dumpBurstBase + n;
    }
}

static void Mul(const d3d::Matrix& m, const float in[4], float out[4])
{
    for (int c = 0; c < 4; ++c)
        out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
}

// How a ground chunk's texture coordinates follow the world (terrain.cpp FitTerrain): each set as a x + b y + c z + d
// with the fit's error, and its normals' range. A texture laid from above has b = 0 and a tiny error; a and c are its
// repeats per world unit.
void Device::DumpTerrainMapping(FILE* f, const void* vertices, uint32_t vertexCount, const FvfLayout& layout,
                                const uint16_t* indices, uint32_t indexCount)
{
    TerrainFit fit = FitTerrain(vertices, vertexCount, layout, indices, indexCount);
    if (!fit.vertices) return;
    // (docs/ground-format.md's open points: a base pass's first two cells as drawn - its tile's RDB id, each corner's
    // world position and coordinates - against the playfield's tile codes; a lighting pass's lightmap, its texels and
    // how it lies on the world, against the baked lightmaps: <dump>-lightmaps.bin.)
    const Texture* tex = m_textures[0];
    if (!m_rs[d3d::RS_LIGHTING] && layout.offset[4] >= 0) {
        std::fprintf(f, "  C: rdb %u", tex ? tex->m_rdbId : 0u);
        const uint8_t* v = static_cast<const uint8_t*>(vertices);
        const uint32_t count = std::min(indices ? indexCount : vertexCount, 12u);
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t i = indices ? indices[k] : k;
            if (i >= vertexCount) break;
            float pos[3], uv[2];
            std::memcpy(pos, v + size_t(i) * layout.stride + layout.offset[0], 12);
            std::memcpy(uv, v + size_t(i) * layout.stride + layout.offset[4], 8);
            std::fprintf(f, "%s(%.2f %.2f %.2f | %.3f %.3f)", k % 6 == 0 ? " | " : " ", pos[0], pos[1], pos[2], uv[0], uv[1]);
        }
        std::fprintf(f, "\n");
    } else if (m_rs[d3d::RS_LIGHTING] && tex && tex->m_thumbValid && tex->m_thumbW == tex->Width() &&
               m_dumpLightmaps.size() < 512) {
        bool seen = false;
        for (const DumpedLightmap& l : m_dumpLightmaps) seen = seen || l.texture == tex;
        if (!seen) {
            DumpedLightmap l;
            l.texture = tex;
            std::memcpy(l.u, fit.coef[2], sizeof(l.u));
            std::memcpy(l.v, fit.coef[3], sizeof(l.v));
            m_dumpLightmaps.push_back(l);
        }
    }
    std::fprintf(f, "  T: %s: %u triangles at %.5f repeats per unit, %.0f%% a tile turned or flipped; as one plane (%u "
                 "vertices):", fit.tiled ? "tiled" : "NOT tiled", fit.triangles, fit.cellScale, 100.0f * fit.cellShare,
                 fit.vertices);
    const char* names[4] = {"u0", "v0", "u1", "v1"};
    for (int j = 0; j < 4; ++j)
        std::fprintf(f, " %s = %.5f x %+.5f y %+.5f z %+.3f (rms %.4f)%s", names[j], fit.coef[j][0], fit.coef[j][1],
                     fit.coef[j][2], fit.coef[j][3], fit.rms[j], j == 3 ? "" : ";");
    std::fprintf(f, " | normals length %.3f-%.3f, y %.2f-%.2f\n", fit.normalLength[0], fit.normalLength[1],
                 fit.normalY[0], fit.normalY[1]);
}

void Device::DumpDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                      uint32_t indexCount)
{
    uint32_t n = m_dumpDraw++;
    if ((fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW) {
        // 2D - but ProcessVertices output (water, floating text) is drawn in the scene: listed with its state.
        if (IsWater(fvf)) {
            const Texture* t = m_textures[0];
            float z0 = 0.0f, rhw0 = 0.0f;
            if (vertexCount) { std::memcpy(&z0, static_cast<const uint8_t*>(vertices) + 8, 4);
                               std::memcpy(&rhw0, static_cast<const uint8_t*>(vertices) + 12, 4); }
            std::fprintf(m_dumpFile, "P %u: pre-transformed in scene, prim %u fvf 0x%x v %u i %u | tex0 %ux%u fmt %u | "
                         "blend %u %u/%u | alphatest %u | z %u/%u/%u | first vertex z %.4f rhw %.4f | depth %s\n",
                         n, primitive, fvf, vertexCount, indexCount, t ? t->Width() : 0, t ? t->Height() : 0,
                         t ? uint32_t(t->GetFormat()) : 0, m_rs[d3d::RS_ALPHABLENDENABLE], m_rs[d3d::RS_SRCBLEND],
                         m_rs[d3d::RS_DESTBLEND], m_rs[d3d::RS_ALPHATESTENABLE], m_rs[d3d::RS_ZENABLE],
                         m_rs[d3d::RS_ZWRITEENABLE], m_rs[d3d::RS_ZFUNC], z0, rhw0,
                         WaterWritesDepth(fvf) ? "forced (water)" : "as the game sets it");
        } else {                                 // (the rest of 2D: what it is, how it blends - a screen flash shows)
            const Texture* t = m_textures[0];
            // (The texture: its format, uploads, black (RVK_AlienMap), opaque, level 0's non-zero bytes;
            // the alpha test and stage 0's colour and alpha operations - what the PF map's tiles are made of.)
            std::fprintf(m_dumpFile, "Q %u: 2D, prim %u fvf 0x%x v %u i %u | tex0 %ux%u fmt %u up %u black %u "
                         "opaque %u nz %u | blend %u %u/%u | alphatest %u %u/%u | st0 c %u/%u/%u a %u/%u/%u | "
                         "z %u/%u/%u | target %s\n",
                         n, primitive, fvf, vertexCount, indexCount, t ? t->Width() : 0, t ? t->Height() : 0,
                         t ? uint32_t(t->GetFormat()) : 0, t ? t->m_uploads : 0, t ? uint32_t(t->m_blackRgb) : 0,
                         t ? uint32_t(t->m_opaque) : 0, t ? t->m_level0NonZero : 0,
                         m_rs[d3d::RS_ALPHABLENDENABLE], m_rs[d3d::RS_SRCBLEND], m_rs[d3d::RS_DESTBLEND],
                         m_rs[d3d::RS_ALPHATESTENABLE], m_rs[d3d::RS_ALPHAFUNC], m_rs[d3d::RS_ALPHAREF],
                         m_tss[0][d3d::TSS_COLOROP], m_tss[0][d3d::TSS_COLORARG1], m_tss[0][d3d::TSS_COLORARG2],
                         m_tss[0][d3d::TSS_ALPHAOP], m_tss[0][d3d::TSS_ALPHAARG1], m_tss[0][d3d::TSS_ALPHAARG2],
                         m_rs[d3d::RS_ZENABLE], m_rs[d3d::RS_ZWRITEENABLE], m_rs[d3d::RS_ZFUNC],
                         m_target == m_scene ? "scene" : m_target == m_main ? "main" : "other");
        }
        return;
    }
    if (vertices && IsBlobShadow(primitive, fvf, vertices, vertexCount, indexCount)) {
        std::fprintf(m_dumpFile, "D %u: blob shadow (hidden), prim %u v %u i %u\n", n, primitive, vertexCount, indexCount);
        return;
    }
    FILE* f = m_dumpFile;
    FvfLayout layout = DecodeFvf(fvf);
    bool lit = m_rs[d3d::RS_LIGHTING] != 0;

    // Lights first, when new or changed since they were last printed. (SetLight may grow m_lights mid-frame.)
    if (m_dumpedLights.size() < m_lights.size()) {
        m_dumpedLights.resize(m_lights.size());
        m_dumpedLightValid.resize(m_lights.size(), false);
    }
    for (size_t i = 0; lit && i < m_lights.size(); ++i) {
        const LightSlot& s = m_lights[i];
        if (!s.enabled) continue;
        if (m_dumpedLightValid[i] && std::memcmp(&m_dumpedLights[i], &s.light, sizeof(d3d::Light)) == 0) continue;
        m_dumpedLights[i] = s.light;
        m_dumpedLightValid[i] = true;
        const d3d::Light& l = s.light;
        std::fprintf(f, "L %zu: type %u pos (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) range %.2f att %.3f/%.4f/%.5f"
                        " diffuse (%.2f %.2f %.2f) ambient (%.2f %.2f %.2f)\n",
                     i, uint32_t(l.type), l.position.x, l.position.y, l.position.z, l.direction.x, l.direction.y,
                     l.direction.z, l.range, l.attenuation0, l.attenuation1, l.attenuation2, l.diffuse.r, l.diffuse.g,
                     l.diffuse.b, l.ambient.r, l.ambient.g, l.ambient.b);
    }

    // World positions -> nearest distance to each local light, and the screen rectangle.
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    std::vector<float> nearest(m_lights.size(), 1e30f);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    uint32_t step = std::max(1u, vertexCount / 4096);
    for (uint32_t i = 0; base && i < vertexCount; i += step) {   // (none here: skinned on the GPU)
        const float* p = reinterpret_cast<const float*>(base + size_t(i) * layout.stride + layout.offset[0]);
        float o[4] = {p[0], p[1], p[2], 1.0f}, w[4], e[4], c[4];
        Mul(m_world, o, w);
        for (size_t k = 0; lit && k < m_lights.size(); ++k) {
            const LightSlot& s = m_lights[k];
            if (!s.enabled || s.light.type == d3d::LIGHT_DIRECTIONAL) continue;
            float dx = w[0] - s.light.position.x, dy = w[1] - s.light.position.y, dz = w[2] - s.light.position.z;
            nearest[k] = std::min(nearest[k], std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        Mul(m_view, w, e);
        Mul(m_proj, e, c);
        if (c[3] <= 1e-4f) continue;
        float sx = m_viewport.x + (c[0] / c[3] * 0.5f + 0.5f) * m_viewport.width;
        float sy = m_viewport.y + (0.5f - c[1] / c[3] * 0.5f) * m_viewport.height;
        minX = std::min(minX, sx); maxX = std::max(maxX, sx);
        minY = std::min(minY, sy); maxY = std::max(maxY, sy);
    }

    std::fprintf(f, "D %u: prim %u fvf 0x%X v %u i %u | lit %d", n, primitive, fvf, vertexCount, indexCount, lit ? 1 : 0);
    if (m_drawVisualName[0])
        std::fprintf(f, " | visual %s (%u) @%08X owner %08X", m_drawVisualName, m_drawVisualKind, m_drawVisual, m_drawOwner);
    if (lit) {
        std::fprintf(f, " lights");
        for (size_t k = 0; k < m_lights.size(); ++k) {
            if (!m_lights[k].enabled) continue;
            if (m_lights[k].light.type == d3d::LIGHT_DIRECTIONAL) std::fprintf(f, " %zu:dir", k);
            else std::fprintf(f, " %zu:%.1f", k, nearest[k]);
        }
        const d3d::Material& m = m_material;
        std::fprintf(f, " | mat d(%.2f %.2f %.2f %.2f) a(%.2f %.2f %.2f) e(%.2f %.2f %.2f) src %u/%u/%u/%u cv %u amb %08X",
                     m.diffuse.r, m.diffuse.g, m.diffuse.b, m.diffuse.a, m.ambient.r, m.ambient.g, m.ambient.b,
                     m.emissive.r, m.emissive.g, m.emissive.b, m_rs[d3d::RS_DIFFUSEMATERIALSOURCE],
                     m_rs[d3d::RS_AMBIENTMATERIALSOURCE], m_rs[d3d::RS_SPECULARMATERIALSOURCE],
                     m_rs[d3d::RS_EMISSIVEMATERIALSOURCE], m_rs[d3d::RS_COLORVERTEX], m_rs[d3d::RS_AMBIENT]);
    }
    std::fprintf(f, " | st0 %u/%X/%X st1 %u/%X/%X", m_tss[0][d3d::TSS_COLOROP], m_tss[0][d3d::TSS_COLORARG1],
                 m_tss[0][d3d::TSS_COLORARG2], m_tss[1][d3d::TSS_COLOROP], m_tss[1][d3d::TSS_COLORARG1],
                 m_tss[1][d3d::TSS_COLORARG2]);
    for (int s = 0; s < 2; ++s)
        if (const Texture* t = m_textures[s]) {
            std::fprintf(f, " tex%d %ux%u", s, t->Width(), t->Height());
            // (Which texture and what reached it: slot, uploads and the frame of the last, non-zero bytes of level 0,
            // an image or view missing, a side-loaded albedo drawn instead.)
            std::fprintf(f, " [%p s%d up %u@%lld nz %d%s%s%s fmt %u]", static_cast<const void*>(t),
                         t->m_bindless == ~0u ? -1 : int(t->m_bindless), t->m_uploads,
                         (long long)(m_frameNumber - t->m_uploadFrame),
                         t->m_level0NonZero == ~0u ? -1 : int(t->m_level0NonZero), t->m_image ? "" : " NO-IMAGE",
                         t->m_view ? "" : " NO-VIEW", t->m_albedoMap ? " albedo" : "", uint32_t(t->m_format));
            if (!t->m_alphaMask.empty())         // (its alpha: hard edges, soft share - two-pass foliage, its dither)
                std::fprintf(f, " alpha edges %.3f soft %.2f", double(t->m_alphaHardEdges), double(t->m_alphaSoftShare));
        }
    if (m_rs[d3d::RS_ALPHABLENDENABLE]) std::fprintf(f, " | blend %u/%u", m_rs[d3d::RS_SRCBLEND], m_rs[d3d::RS_DESTBLEND]);
    // (Fog: its colour and range - the game's sky takes most of its colour from it.)
    if (m_rs[d3d::RS_FOGENABLE]) {
        float range[2];
        std::memcpy(&range[0], &m_rs[d3d::RS_FOGSTART], 4);
        std::memcpy(&range[1], &m_rs[d3d::RS_FOGEND], 4);
        std::fprintf(f, " | fog %06X %.0f-%.0f", m_rs[d3d::RS_FOGCOLOR] & 0xFFFFFFu, double(range[0]), double(range[1]));
    }
    std::fprintf(f, " | z %u/%u/%u | at (%.1f %.1f %.1f)", m_rs[d3d::RS_ZENABLE], m_rs[d3d::RS_ZWRITEENABLE],
                 m_rs[d3d::RS_ZFUNC], m_world.m[3][0], m_world.m[3][1], m_world.m[3][2]);
    if (IsShadowCaster(primitive, fvf) || ShadowReceiver(fvf) || ShadowInLightmap(fvf) || ShadowCompensated(fvf))
        std::fprintf(f, " | shadow %s%s%s", IsShadowCaster(primitive, fvf) ? "C" : "", ShadowReceiver(fvf) ? "R" : "",
                     ShadowInLightmap(fvf) ? "L" : ShadowCompensated(fvf) ? "M" : "");
    if (IsShadowCaster(primitive, fvf) && vertices) {
        float mn[3], mx[3];
        Texture* cutTex;
        int cutOffset;
        float cutRef;
        ShadowCutout(fvf, &cutTex, &cutOffset, &cutRef);
        uint64_t key = CasterKey(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount, cutTex,
                                 cutOffset, mn, mx);
        auto streak = m_casterStreaks.find(key);
        if (m_casterCache.count(key)) std::fprintf(f, " remembered");
        else std::fprintf(f, " seen %u", streak != m_casterStreaks.end() ? streak->second.count : 0u);
    }
    if (minX <= maxX) std::fprintf(f, " | rect %.0f,%.0f-%.0f,%.0f", minX, minY, maxX, maxY);
    else std::fprintf(f, " | offscreen");
    std::fprintf(f, "\n");
    if (vertices && IsTerrain(fvf) && !m_native.clutter) {   // (the ground's, not the rocks on it)
        DumpTerrainMapping(f, vertices, vertexCount, layout, indices, indexCount);
        const Texture* t = m_textures[0];
        if (!lit && t && t->m_thumbValid &&
            std::find(m_dumpGroundTextures.begin(), m_dumpGroundTextures.end(), t) == m_dumpGroundTextures.end())
            m_dumpGroundTextures.push_back(t);
    }
    // Geometry of draws with the vertex count asked for (RANDYVK_DUMP_VERTS): world-space positions, normals
    // (through the world matrix, not normalised), texture coordinates of set 0; then the indices.
    if (!vertices)
        std::fprintf(f, "  (skinned on the GPU)\n");
    else if (m_dumpVertexCount && vertexCount == m_dumpVertexCount) {
        const uint8_t* v = static_cast<const uint8_t*>(vertices);
        const auto& w = m_world.m;
        for (uint32_t i = 0; i < vertexCount; ++i) {
            const uint8_t* p = v + size_t(i) * layout.stride;
            float pos[3], wp[3], nrm[3] = {}, wn[3] = {}, uv[2] = {};
            std::memcpy(pos, p, 12);
            if (layout.offset[1] >= 0) std::memcpy(nrm, p + layout.offset[1], 12);
            if (layout.offset[4] >= 0) std::memcpy(uv, p + layout.offset[4], 8);
            for (int j = 0; j < 3; ++j) {
                wp[j] = pos[0] * w[0][j] + pos[1] * w[1][j] + pos[2] * w[2][j] + w[3][j];
                wn[j] = nrm[0] * w[0][j] + nrm[1] * w[1][j] + nrm[2] * w[2][j];
            }
            std::fprintf(f, "V %u: %.3f %.3f %.3f n %.3f %.3f %.3f uv %.3f %.3f\n", i, wp[0], wp[1], wp[2], wn[0], wn[1],
                         wn[2], uv[0], uv[1]);
        }
        if (indices) {
            std::fprintf(f, "I");
            for (uint32_t i = 0; i < indexCount; ++i) std::fprintf(f, " %u", indices[i]);
            std::fprintf(f, "\n");
        }
    }
}


namespace {

float DumpHalf(uint16_t h)
{
    const uint32_t s = uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (e == 0) return (h & 0x8000u ? -1.0f : 1.0f) * float(m) / 1024.0f / 16384.0f;
    uint32_t bits = e == 31 ? (s | 0x7F800000u | (m << 13)) : (s | ((e + 112u) << 23) | (m << 13));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// A 24-bit BMP from RGB rows, top row first.
void DumpBmp(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    const uint32_t rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4, size = (rowBytes + pad) * h;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    header[26] = 1;
    header[28] = 24;
    put32(34, size);
    std::fwrite(header, 1, 54, file);
    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (int32_t y = int32_t(h) - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* c = rgb.data() + (size_t(y) * w + x) * 3;
            row[x * 3] = c[2];
            row[x * 3 + 1] = c[1];
            row[x * 3 + 2] = c[0];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

}  // namespace

// Frame dump: the frame's in-between images next to its picture (<dump>-<name>.bmp), as they stood at the scene's end
// (EndScene, after the post passes): the HDR scene before them (tone mapped simply: x / (1 + x), gamma 2.2), the
// scene's other targets (the local lights' share, the surface colour, the glow), the half-size passes' results (ambient
// occlusion, indirect light, scattered light, contact shadows: their first channel), the environment probe, the
// reflections - where a fault first shows is where it comes from.
void Device::DumpTargets(const std::string& base)
{
    struct Item { Texture* t; const char* name; };
    const Item items[] = {{m_scene, "scene"}, {m_localFraction, "local"}, {m_albedo, "albedo"}, {m_glow, "glow"},
                          {m_aoTex[1], "ao"}, {m_giTex[1], "gi"}, {m_volumeTex[1], "volume"},
                          {m_contactTex[1], "contact"}, {m_envAtlas, "env"}, {m_ssrTex, "ssr"}};
    struct Read { const Item* item; uint32_t bpp; VkBuffer buffer; VmaAllocation_T* allocation; const uint8_t* data; };
    std::vector<Read> reads;
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    for (const Item& it : items) {
        Texture* t = it.t;
        if (!t || !t->m_image || !t->m_renderTarget || t->m_layout == VK_IMAGE_LAYOUT_UNDEFINED)
            continue;
        const uint32_t bpp = t->m_format == Format::RGBA16F ? 8u
                           : (t->m_format == Format::RG16F || t->m_format == Format::RGBA8 || t->m_format == Format::A8R8G8B8) ? 4u
                           : 0u;
        if (!bpp)
            continue;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize(t->m_width) * t->m_height * bpp;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        Read r{&it, bpp, VK_NULL_HANDLE, nullptr, nullptr};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bi, &ac, &r.buffer, &r.allocation, &info) != VK_SUCCESS)
            continue;
        r.data = static_cast<const uint8_t*>(info.pMappedData);
        Transition(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {t->m_width, t->m_height, 1};
        vkCmdCopyImageToBuffer(cmd, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &region);
        reads.push_back(r);
    }
    if (reads.empty())
        return;
    SubmitAndWait();
    for (const Read& r : reads) {
        const Texture* t = r.item->t;
        const uint32_t w = t->m_width, h = t->m_height;
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        auto shown = [](float v) { v = std::max(v, 0.0f); return uint8_t(std::pow(v / (1.0f + v), 1.0f / 2.2f) * 255.0f + 0.5f); };
        for (size_t p = 0; p < size_t(w) * h; ++p) {
            uint8_t* o = rgb.data() + p * 3;
            const uint8_t* s = r.data + p * r.bpp;
            if (t->m_format == Format::RGBA16F) {
                uint16_t c[3];
                std::memcpy(c, s, 6);
                float v[3];
                for (int k = 0; k < 3; ++k) v[k] = DumpHalf(c[k]);
                // (Not a number: magenta; negative: cyan - where the faults are.)
                if (std::isnan(v[0]) || std::isnan(v[1]) || std::isnan(v[2]) || std::isinf(v[0]) || std::isinf(v[1]) ||
                    std::isinf(v[2])) {
                    o[0] = 255; o[1] = 0; o[2] = 255;
                    continue;
                }
                if (v[0] < 0.0f || v[1] < 0.0f || v[2] < 0.0f) {
                    o[0] = 0; o[1] = 255; o[2] = 255;
                    continue;
                }
                for (int k = 0; k < 3; ++k) o[k] = shown(v[k]);
            } else if (t->m_format == Format::RG16F) {
                uint16_t c;
                std::memcpy(&c, s, 2);
                o[0] = o[1] = o[2] = uint8_t(std::clamp(DumpHalf(c), 0.0f, 1.0f) * 255.0f + 0.5f);
            } else if (t->m_format == Format::A8R8G8B8) {   // (B G R A)
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0];
            } else {                                        // (R G B A)
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            }
        }
        DumpBmp(base + "-" + r.item->name + ".bmp", w, h, rgb);
        vmaDestroyBuffer(m_allocator, r.buffer, r.allocation);
    }
    Log("frame dump: %u in-between images (%s-*.bmp)", unsigned(reads.size()), base.c_str());
}

}  // namespace rvk
