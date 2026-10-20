// Parallax: a surface's height seen in depth up close, without moving its geometry (docs/desert-plan.md).
//
// The eye's ray through a pixel is followed down into the surface until it meets the height; the textures are read
// where it meets, so stones and bricks stand out of their texture, the troughs between sand ripples hide behind their
// crests, and from that point a short walk towards the sun says whether the point is in its own relief's shadow.
// No depth is written: the outline stays the surface's, the depth test (and the ground's lighting pass, equal to its
// base pass) as before. Both ground passes march alike, from the same inputs, so they meet the same point.
//
// Two kinds of height:
// - a side-loaded height map (_h.png, rvk/material_maps.h): in its normal map's alpha, white the surface and black
//   FL.terrain3.y (RVK_Parallax) of a texture repeat below it - on meshes (D.mat.z MAT_HEIGHT) and on the native
//   ground's cells' tiles;
// - the sand's ripples (FL.sand.x, RVK_SandRipple), a formula over the world laid across the playfield's wind, on the
//   native ground's sand (NativeSand).
//
// The surface's frame comes from the screen derivatives of the position and the texture coordinates (as
// NormalMapNormal), so no vertex tangents are needed.

// A world offset in the surface's plane as a change of the texture coordinates: the offset as a px + b py over the
// plane the two pixel steps span (least squares), then a ux + b uy.
vec2 ParallaxUvStep(vec3 offsetW, vec3 px, vec3 py, vec2 ux, vec2 uy)
{
    const float xx = dot(px, px), xy = dot(px, py), yy = dot(py, py);
    const float det = xx * yy - xy * xy;
    if (abs(det) < 1e-24) return vec2(0.0);
    const vec2 r = vec2(dot(offsetW, px), dot(offsetW, py));
    const vec2 ab = vec2(yy * r.x - xy * r.y, xx * r.y - xy * r.x) / det;
    return ab.x * ux + ab.y * uy;
}

// How far out parallax is followed (world units): fully to kParallaxNear, gone by kParallaxFar.
const float kParallaxNear = 14.0, kParallaxFar = 22.0;
const float kRippleParallaxNear = 30.0, kRippleParallaxFar = 45.0;   // (the sand's ripples: see GroundParallax)

// A march's step count n times RVK_DepthSteps (FL.terrain3.z): more is finer - edges sharper at flat views, no
// stepped bands - and slower. At least 2.
int ParallaxSteps(float n)
{
    return max(int(n * (FL.terrain3.z > 0.0 ? FL.terrain3.z : 1.0)), 2);
}

// The direction towards the sun (0 below the horizon or without one).
vec3 ParallaxSunDir()
{
    if (dot(FL.sunDir.xyz, FL.sunDir.xyz) <= 0.0) return vec3(0.0);
    const vec3 L = -normalize(FL.sunDir.xyz);
    return L.y > 0.02 ? L : vec3(0.0);
}

// ---- meshes: a side-loaded height map

// Where the eye's ray through this pixel meets the height in img's alpha (uv, with its derivatives ux, uy: the
// pixel's own, kept for every read so the shift doesn't change the mip level). sunVis: how much of the sun reaches that
// point past the relief round it (1 lit; for the sunlight only).
vec2 ParallaxUv(texture2D img, sampler smp, vec2 uv, vec2 ux, vec2 uy, vec3 posW, vec3 px, vec3 py, vec3 n,
                out float sunVis)
{
    sunVis = 1.0;
    const float depthRepeats = FL.terrain3.y;
    if (depthRepeats <= 0.0 || dot(n, n) <= 1e-8) return uv;
    n = normalize(n);
    // Faded by distance, and where a texel is more than a few pixels small (its relief can't show; the march would
    // only shimmer).
    const vec2 size = vec2(textureSize(sampler2D(img, smp), 0));
    const float texelsPerPixel = max(length(ux * size), length(uy * size));
    const float fade = (1.0 - smoothstep(kParallaxNear, kParallaxFar, length(posW - C.eyePos.xyz))) *
                       (1.0 - smoothstep(1.5, 4.0, texelsPerPixel));
    if (fade <= 0.0) return uv;
    // A texture repeat's length in the world (as NormalMapNormal), and the full depth in world units.
    const float worldPerUv = sqrt(max(dot(px, px), dot(py, py)) / max(max(dot(ux, ux), dot(uy, uy)), 1e-12));
    const float depthW = depthRepeats * worldPerUv * fade;
    const vec3 V = normalize(posW - C.eyePos.xyz);
    const float cosV = -dot(V, n);
    if (cosV <= 0.0) return uv;
    // The texture coordinates' change from the surface down to the full depth along the ray (grazing rays limited: at
    // the horizon they would slide a whole repeat).
    const vec2 full = ParallaxUvStep((V + cosV * n) * (depthW / max(cosV, 0.25)), px, py, ux, uy);
    const int steps = ParallaxSteps(mix(24.0, 6.0, cosV));
    float layer = 0.0, prevLayer = 0.0, prevGap = 0.0, hit = 0.0;
    for (int i = 0; i <= steps; ++i) {
        layer = float(i) / float(steps);
        const float below = 1.0 - textureGrad(sampler2D(img, smp), uv + full * layer, ux, uy).a;
        const float gap = layer - below;           // >= 0: the ray is under the surface here
        if (gap >= 0.0) {
            hit = i == 0 ? 0.0 : mix(prevLayer, layer, prevGap / (prevGap - gap));
            break;
        }
        prevLayer = layer;
        prevGap = gap;
        hit = layer;
    }
    const vec2 at = uv + full * hit;
    // In its own relief's shadow: from the point met, up towards the sun, is the height above the ray anywhere?
    const vec3 L = ParallaxSunDir();
    const float cosL = dot(L, n);
    if (hit > 0.002 && cosL > 0.02) {
        const vec2 up = ParallaxUvStep((L - cosL * n) * (depthW / max(cosL, 0.2)), px, py, ux, uy);
        float over = 0.0;
        const int lookUp = ParallaxSteps(6.0);
        for (int k = 1; k <= lookUp; ++k) {
            const float t = hit * float(k) / float(lookUp);   // up this much of the depth
            const float below = 1.0 - textureGrad(sampler2D(img, smp), at + up * t, ux, uy).a;
            over = max(over, (hit - t) - below);
        }
        sunVis = 1.0 - smoothstep(0.0, 0.12, over) * fade;
    }
    return at;
}

// ---- the native ground: its cells' tiles' heights and the sand's ripples

// The sand's ripples: crests across the playfield's wind (FL.sand.w), wandering, forking and breaking off (warped
// noise); each a gentle slope into the wind and a steep one away from it. Two sizes - small ripples, and the wind's
// larger sculpting that carries them far out. The noise is read once a pixel (SandRipplesAt), with its slope: over the
// few centimetres a parallax march or a normal's slope looks round a pixel, the ripples' phase is that plus the slope
// times the step - each step a fract, not four noise reads.
const float kRippleSmall = 0.36, kRippleBig = 5.5;   // wavelengths (world units)
const float kRippleSmallDepth = 0.028, kRippleBigDepth = 0.06;

// Value noise (as TerrainNoise) with its gradient: xy the gradient, z the value (0..1).
vec3 RippleNoiseD(vec2 p)
{
    const vec2 i = floor(p), f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
    const uvec2 c = uvec2(ivec2(i) + 0x4000);
    const float a = float(TerrainHash(c) & 0xFFFFu) / 65535.0, b = float(TerrainHash(c + uvec2(1, 0)) & 0xFFFFu) / 65535.0;
    const float d = float(TerrainHash(c + uvec2(0, 1)) & 0xFFFFu) / 65535.0;
    const float e = float(TerrainHash(c + uvec2(1, 1)) & 0xFFFFu) / 65535.0;
    const float k = a - b - d + e;
    return vec3(du * vec2(b - a + k * u.y, d - a + k * u.x), a + (b - a) * u.x + (d - a) * u.y + k * u.x * u.y);
}

struct SandRipples {
    float small, big;          // the phase at the pixel: world units along the wind, warped
    vec2 smallGrad, bigGrad;   // ... its gradient over world x, z
    float smallDepth, bigDepth;   // how deep each is here (world units: the sand, the fade, broken-off patches)
};

// How much of the ripples shows at a pixel whose footprint across them is `foot` (world units): each size fades out as
// its wavelength shrinks to a few pixels. x the small ones, y the big ones.
vec2 SandRippleFade(vec2 foot)
{
    return vec2(1.0 - smoothstep(kRippleSmall / 7.0, kRippleSmall / 3.0, foot.x),
                1.0 - smoothstep(kRippleBig / 7.0, kRippleBig / 3.0, foot.y));
}

// The ripples round a pixel at xz with screen derivatives px, py; fade gets how much of each size shows. The footprint
// is measured across the crests (along each size's phase gradient), not as the pixel's longest side: looking along
// the ground a pixel is a long thin strip, and ripples whose crests run away from the eye stay many pixels apart long
// after the strip's length passes their wavelength. A share of the longest side stays in it, for the crests' own
// wander and forks, which do squeeze along that strip.
SandRipples SandRipplesAt(vec2 xz, float sand, vec3 px, vec3 py, out vec2 fade)
{
    SandRipples r;
    const float a = FL.sand.w;
    const vec2 along = vec2(cos(a), sin(a)), across = vec2(-sin(a), cos(a));
    const vec2 q = vec2(dot(along, xz), dot(across, xz));   // x along the wind
    const mat2 toWorld = mat2(along, across);                  // a gradient over q -> over xz (columns: along, across)
    // Small: warped by noise at two sizes; broken off in patches.
    const vec2 s1 = vec2(0.6, 0.35);
    const vec3 n1 = RippleNoiseD(q * s1), n2 = RippleNoiseD(q * 2.1 + 7.3), nb = RippleNoiseD(q * 0.11 + 3.1);
    // (The finer warp less as RVK_RippleSoft is more - FL.sand2.x: near the ripples' own spacing, at 0.18 it kinked
    // their crests, noisy - the user's rvk-frame-163429.)
    const float fine = 0.18 * (1.0 - clamp(FL.sand2.x, 0.0, 1.0));
    r.small = q.x + 0.55 * n1.z + fine * n2.z;
    r.smallGrad = toWorld * (vec2(1.0, 0.0) + 0.55 * n1.xy * s1 + fine * n2.xy * 2.1);
    // Big: warped more, and slowly; only in patches (a whole hillside of them in rows read as corduroy).
    const vec2 s3 = vec2(0.05, 0.03);
    const vec3 n3 = RippleNoiseD(q * s3), n4 = RippleNoiseD(q * 0.21 + 1.7), np = RippleNoiseD(q * 0.023 + 5.9);
    r.big = q.x + 2.6 * n3.z + 1.2 * n4.z;
    r.bigGrad = toWorld * (vec2(1.0, 0.0) + 2.6 * n3.xy * s3 + 1.2 * n4.xy * 0.21);
    const float strip = 0.3 * max(length(px), length(py));
    fade = SandRippleFade(vec2(max(max(abs(dot(r.smallGrad, px.xz)), abs(dot(r.smallGrad, py.xz))), strip),
                               max(max(abs(dot(r.bigGrad, px.xz)), abs(dot(r.bigGrad, py.xz))), strip)));
    // (Deeper and shallower in patches; down to 0.3 the shallow ones barely shadowed, and read as the shadows
    // switching off along a line - the user's rvk-frame-162907.)
    r.smallDepth = kRippleSmallDepth * mix(0.7, 1.0, smoothstep(0.25, 0.65, nb.z)) * sand * fade.x;
    r.bigDepth = kRippleBigDepth * smoothstep(0.35, 0.75, np.z) * sand * fade.y;
    return r;
}

// One ripple's shape over its phase f (0..1): x how far below its crest (0 crest .. 1 trough), y its slope d/df. Up the
// gentle side (share `gentle` of it), down the steep one.
vec2 RippleProfile(float f, float gentle)
{
    const bool up = f < gentle;
    const float rise = up ? f / gentle : (1.0 - f) / (1.0 - gentle);
    const float dRise = up ? 1.0 / gentle : -1.0 / (1.0 - gentle);
    return vec2(1.0 - rise * rise * (3.0 - 2.0 * rise), -6.0 * rise * (1.0 - rise) * dRise);
}

// The ripples' depth (world units) a world offset d from the pixel; small only (the march), or both.
float SandRippleDepth(SandRipples r, vec2 d, bool big)
{
    float depth = 0.0;
    if (r.smallDepth > 0.0)
        depth += RippleProfile(fract((r.small + dot(r.smallGrad, d)) / kRippleSmall), 0.72).x * r.smallDepth;
    if (big && r.bigDepth > 0.0)
        depth += RippleProfile(fract((r.big + dot(r.bigGrad, d)) / kRippleBig), 0.65).x * r.bigDepth;
    return depth;
}

// ... and its slope over world x, z there (both sizes).
vec2 SandRippleSlope(SandRipples r, vec2 d)
{
    vec2 slope = vec2(0.0);
    if (r.smallDepth > 0.0)
        slope += RippleProfile(fract((r.small + dot(r.smallGrad, d)) / kRippleSmall), 0.72).y * r.smallDepth /
                 kRippleSmall * r.smallGrad;
    if (r.bigDepth > 0.0)
        slope += RippleProfile(fract((r.big + dot(r.bigGrad, d)) / kRippleBig), 0.65).y * r.bigDepth / kRippleBig *
                 r.bigGrad;
    return slope;
}

// ---- the footprints (rvk sand_prints.cpp, RVK_SandPrints): a map round the eye, laid toroidally (FL.sand2)

const float kPrintCell = 0.02, kPrintSize = 2048.0 * 0.02;   // (sand_prints.cpp)
// A print's floor below the ripples' crests (~2.3 cm under the sand's mean, the ripples' troughs already 2.8 cm under
// their crests: at 2.5 its floor lay no lower than a trough, and only its middle showed; at 4.5 a low sun across it
// left all its floor in its wall's shadow, a hole), its rim's rise.
const float kPrintDepth = 0.037, kPrintRim = 0.01;

// Whether the map holds the point at world xz (a unit's margin: it fades out over it).
bool SandPrintsAt(vec2 xz)
{
    if ((floatBitsToUint(FL.sand2.y) & 0xFFFFFu) == 0u) return false;
    const vec2 rel = xz - FL.sand2.zw;
    return min(min(rel.x, rel.y), kPrintSize - max(rel.x, rel.y)) > 0.0;
}

// x how far a print is pressed (0..1), y its rim (0..1) at world xz; nothing outside the map, fading over its last unit.
vec2 SandPrintAt(vec2 xz)
{
    const uint slot = floatBitsToUint(FL.sand2.y);
    if ((slot & 0xFFFFFu) == 0u) return vec2(0.0);
    const vec2 rel = xz - FL.sand2.zw;
    const float inside = smoothstep(0.0, 1.0, min(min(rel.x, rel.y), kPrintSize - max(rel.x, rel.y)));
    if (inside <= 0.0) return vec2(0.0);
    return textureLod(sampler2D(texImages[(slot & 0xFFFFFu) - 1u], bindlessSamplers[(slot >> 20) & 0xFFu]),
                      xz / kPrintSize, 0.0).rg * inside;
}

// A print's height over the ripples' crests (world units): its floor below, its rim a little above.
float SandPrintHeight(vec2 p)
{
    return -kPrintDepth * p.x + kPrintRim * p.y * (1.0 - p.x);
}

// What the ground march needs, worked out once per pixel.
struct GroundRelief {
    float sand;       // NativeSand here (0..1) x RVK_SandRipple, slip faces (past about 32 degrees) left smooth
    vec2 fade;        // SandRippleFade, across the crests
    bool tiles;       // any of the playfield's tiles has a height (FL.terrain3.w)
    vec2 cell;        // the pixel's cell coordinates (before the parallax moves them)
    vec2 xz;          // ... its world x, z
    float prints;     // how much the footprints show here (0: none; sand only)
    // The pixel's cell's tile, for the march (NativeTileHeight): its height map's image (0xFFFF none) and turn, looked
    // up once - a pixel's march seldom leaves its 4-unit cell, and two table reads a step were half its cost.
    vec2 tileCell;
    uint tileImg, tileTurn;
    SandRipples rip;  // the ripples round the pixel (when sand)
};

GroundRelief GroundReliefAt(TerrainMap m, vec3 posW, vec3 n)
{
    GroundRelief r;
    const float ny = dot(n, n) > 1e-8 ? normalize(n).y : 1.0;
    // (Not on the rocks and pebbles lying on it: their base pass has the cells' sand too.)
    r.sand = m.clutter ? 0.0 : NativeSand(m.uv[0]) * clamp(FL.sand.x, 0.0, 1.0) * smoothstep(0.82, 0.87, ny) * m.w.x;
    r.fade = vec2(0.0);
    r.tiles = FL.terrain3.y > 0.0 && FL.terrain3.w > 0.5 && TerrainNativeMaps();
    r.cell = m.uv[0];
    r.tileCell = floor(m.uv[0]);
    r.tileImg = r.tiles ? NativeNormalImage(m.uv[0]) : 0xFFFFu;
    r.tileTurn = r.tiles && r.tileImg != 0xFFFFu ? NativeCell(m.uv[0]).y : 0u;
    // (No march where the pixel's own tile has no height: any other tile in the playfield with one had every ground
    // pixel step through heights of 1. A height a march reaches over a cell's edge from one without is lost - rarely
    // seen, the steps being centimetres.)
    if (r.tiles && (r.tileImg == 0xFFFFu || !NativeCellHeight(m.uv[0]))) r.tiles = false;
    r.xz = posW.xz;
    r.prints = r.sand > 0.0 && SandPrintsAt(posW.xz) ? smoothstep(0.1, 0.5, r.sand) : 0.0;
    r.rip.smallDepth = r.rip.bigDepth = 0.0;
    // (Past where even the big ones are a few pixels apart across the pixel's short side, there is nothing to read.)
    if (r.sand > 0.0 && min(length(m.px), length(m.py)) < kRippleBig / 3.0)
        r.rip = SandRipplesAt(posW.xz, r.sand, m.px, m.py, r.fade);
    return r;
}

// The cell's tile's height at cell coordinates c (1 its surface, 0 FL.terrain3.y of a repeat below; 1 without one).
float NativeTileHeight(GroundRelief r, vec2 c, vec2 dx, vec2 dy)
{
    const bool same = all(equal(floor(c), r.tileCell));   // (the pixel's own cell: its tile as looked up once)
    const uint img = same ? r.tileImg : NativeNormalImage(c);
    if (img == 0xFFFFu) return 1.0;
    const uint turn = same ? r.tileTurn : NativeCell(c).y;
    return textureGrad(sampler2D(texImages[nonuniformEXT(img)], bindlessSamplers[D.sampIdx.w]), NativeTurn(fract(c), turn),
                       NativeTurnStep(dx, turn), NativeTurnStep(dy, turn)).a;
}

// The ground's depth below its top at cell coordinates c (world units): its tile's height and the sand's ripples.
float GroundDepth(GroundRelief r, vec2 c, vec2 dx, vec2 dy, float tileDepthW, float cellSize, float fine)
{
    float d = 0.0;
    if (tileDepthW > 0.0) d += (1.0 - NativeTileHeight(r, c, dx, dy)) * tileDepthW;
    if (r.rip.smallDepth > 0.0 || r.prints > 0.0) {
        float sand = r.rip.smallDepth > 0.0 ? SandRippleDepth(r.rip, (c - r.cell) * cellSize, false) : 0.0;
        // (In a print the ripples pressed flat to its floor, a rim round it.)
        if (r.prints > 0.0) {
            const vec2 p = SandPrintAt(r.xz + (c - r.cell) * cellSize) * r.prints;
            sand = mix(sand, kPrintDepth, p.x) - kPrintRim * p.y * (1.0 - p.x);
        }
        d += sand * fine;
    }
    return d;
}

// The native ground's parallax: m.uv[0] (its cells' coordinates from above) moved to where the eye's ray meets the
// relief; returns how much of the sun reaches that point past the relief (1 lit). Not on its rocks, pebbles or faces.
// The ripples' depth is RVK_RippleDepth's (FL.sand3.x), and it is followed farther out than the tiles':
// a trough hides behind its crest only where the eye looks along the ground flatter than the crest's lee (about 23
// degrees at 1), which from the game's camera is past about 12 units - the tiles' 22 left almost nothing of it, and
// in game the ripples showed no depth at all (the user's rvk-frame-153557).
float GroundParallax(inout TerrainMap m, GroundRelief r, vec3 posW, vec3 n)
{
    if (m.clutter || (!r.tiles && r.sand <= 0.0) || dot(n, n) <= 1e-8) return 1.0;
    const float dist = length(posW - C.eyePos.xyz);
    const float near = r.tiles ? 1.0 - smoothstep(kParallaxNear, kParallaxFar, dist) : 0.0;
    const float ripNear = r.rip.smallDepth > 0.0 || r.prints > 0.0 ? 1.0 - smoothstep(kRippleParallaxNear, kRippleParallaxFar, dist)
                                                                     : 0.0;
    // (x RVK_RippleDepth, FL.sand3.x - their own, not RVK_Parallax's: textures' height maps drawn shallow left the
    // dunes flat, the user, 2026-10-19.)
    const float ripW = ripNear * FL.sand3.x;
    if (near <= 0.0 && ripW <= 0.0) return 1.0;
    n = normalize(n);
    const float cellSize = 1.0 / max(D.terrain.x, 1e-6);
    const bool tiles = near > 0.0;   // (past their range, the ripples alone)
    const float tileDepthW = tiles ? FL.terrain3.y * cellSize * near : 0.0;
    const float maxW = tileDepthW + max(r.rip.smallDepth, r.prints > 0.0 ? kPrintDepth : 0.0) * ripW;
    if (maxW <= 1e-4) return 1.0;
    const vec3 V = normalize(posW - C.eyePos.xyz);
    const float cosV = -dot(V, n);
    if (cosV <= 0.0) return 1.0;
    // (How flat a ray is followed: the ripples' lee is steep and shallow, so theirs flatter, to about 83 degrees.)
    const vec3 run = (V + cosV * n) * (maxW / max(cosV, tiles ? 0.25 : 0.12));
    const vec2 full = ParallaxUvStep(run, m.px, m.py, m.dx[0], m.dy[0]);
    // Steps: the tiles' by the angle; the ripples' alone a few a wavelength the ray crosses (a crest is not stepped over).
    const int steps = ParallaxSteps(tiles ? mix(16.0, 5.0, cosV) : clamp(6.0 * length(run) / kRippleSmall, 3.0, 10.0));
    float layer = 0.0, prevLayer = 0.0, prevGap = 0.0, hit = 0.0;
    for (int i = 0; i <= steps; ++i) {
        layer = float(i) / float(steps);
        const float below = GroundDepth(r, m.uv[0] + full * layer, m.dx[0], m.dy[0], tileDepthW, cellSize, ripW) / maxW;
        const float gap = layer - below;
        if (gap >= 0.0) {
            hit = i == 0 ? 0.0 : mix(prevLayer, layer, prevGap / (prevGap - gap));
            break;
        }
        prevLayer = layer;
        prevGap = gap;
        hit = layer;
    }
    const vec2 at = m.uv[0] + full * hit;
    float sunVis = 1.0;
    const vec3 L = ParallaxSunDir();
    const float cosL = dot(L, n);
    if (hit > 0.002 && cosL > 0.02) {
        const vec2 up = ParallaxUvStep((L - cosL * n) * (maxW / max(cosL, 0.15)), m.px, m.py, m.dx[0], m.dy[0]);
        float over = 0.0;
        const int lookUp = ParallaxSteps(tiles ? 5.0 : 3.0);
        for (int k = 1; k <= lookUp; ++k) {
            const float t = hit * float(k) / float(lookUp);
            const float below = GroundDepth(r, at + up * t, m.dx[0], m.dy[0], tileDepthW, cellSize, ripW) / maxW;
            over = max(over, (hit - t) - below);
        }
        sunVis = 1.0 - smoothstep(0.0, 0.25, over) * max(near, ripNear);
    }
    m.uv[0] = at;
    return sunVis;
}

// The sand's ripples in its colour (the base pass): the troughs a little darker - less of the sky reaches into them,
// and the heavier grains settle there - the crests a little lighter. They show so whichever way the sun crosses them
// (lit along their crests, the relief alone barely does).
vec3 SandRippleTone(vec3 c, GroundRelief r, vec2 cell)
{
    if (r.rip.smallDepth <= 0.0 && r.rip.bigDepth <= 0.0 && r.prints <= 0.0) return c;
    const vec2 d = (cell - r.cell) / max(D.terrain.x, 1e-6);
    // (A print: the ripples pressed out of it, its floor darker - packed, the grains shaded in it; its rim a little
    // lighter, dry sand thrown up.)
    const vec2 p = r.prints > 0.0 ? SandPrintAt(r.xz + d) * r.prints : vec2(0.0);
    float k = 1.0 - 0.12 * p.x + 0.04 * p.y * (1.0 - p.x);
    if (r.rip.smallDepth > 0.0 || r.rip.bigDepth > 0.0) {
        const float small = RippleProfile(fract((r.rip.small + dot(r.rip.smallGrad, d)) / kRippleSmall), 0.72).x;   // 0 crest
        const float big = RippleProfile(fract((r.rip.big + dot(r.rip.bigGrad, d)) / kRippleBig), 0.65).x;          // .. 1 trough
        k += (r.sand * 0.24 * r.fade.x * (0.45 - small) + 0.07 * (r.rip.bigDepth / kRippleBigDepth) * (0.5 - big)) *
             (1.0 - p.x);
    }
    return c * k;
}

// The sand's ripples as relief on the shading normal n (the lighting pass): the slope of their depth here, both sizes
// as far as each shows. (Beyond the parallax's range the small ones still shade - their crests and troughs lit and
// shaded by the sun - until they are a few pixels apart.)
vec3 SandRippleNormal(vec3 n, GroundRelief r, vec2 cell)
{
    if ((r.rip.smallDepth <= 0.0 && r.rip.bigDepth <= 0.0 && r.prints <= 0.0) || dot(n, n) <= 1e-8) return n;
    // The height is minus the depth: its slope laid onto n (near enough flat: the dunes' faces where there are ripples
    // are under ~35 degrees); a little steeper than the parallax's - the tile's own mottling is strong.
    const vec2 d = (cell - r.cell) / max(D.terrain.x, 1e-6);
    vec2 slope = r.rip.smallDepth > 0.0 || r.rip.bigDepth > 0.0 ? -SandRippleSlope(r.rip, d) * 1.6 : vec2(0.0);
    // A print's: the ripples flattened in it, its own walls and rim (across a texel each way).
    if (r.prints > 0.0) {
        const vec2 xz = r.xz + d, e = vec2(kPrintCell, 0.0);
        const vec2 p = SandPrintAt(xz) * r.prints;
        const float hx = SandPrintHeight(SandPrintAt(xz + e.xy) * r.prints) - SandPrintHeight(SandPrintAt(xz - e.xy) * r.prints);
        const float hz = SandPrintHeight(SandPrintAt(xz + e.yx) * r.prints) - SandPrintHeight(SandPrintAt(xz - e.yx) * r.prints);
        slope = slope * (1.0 - p.x) + vec2(hx, hz) / (2.0 * kPrintCell);
    }
    const float len = length(n);
    return normalize(normalize(n) + vec3(-slope.x, 0.0, -slope.y)) * len;
}

// The sand's glints (the lighting pass; FL.sand.z, RVK_SandGlint): now and then a grain turned just so mirrors the sun
// to the eye. A grid of cells, some of them holding a grain (the setting scales how many, and how bright) with a facet
// turned at random about the ground's normal n; it shines where that facet is near the sun's and the eye's half-way
// direction. The cells are 3 cm up close and twice as big at each step out, so a grain is always a pixel or two across
// (neighbouring steps blended: no popping as the eye moves) - fixed at 3 cm, they had faded out a few units from the
// eye, as the ground's pixels stretch away from it, and in game there were none to see (the user). Far out a cell
// holds a grain less often: a few sparkles, not glitter. Bright (well over the sun's light on the sand): a pixel's
// point has to stand out of the bloom's threshold and the TAA's averaging. Gone by about 60 units, and as the sun
// nears the horizon. cell: the pixel's cell coordinates after the parallax. Returns the sun's multiple it adds.
const float kGlintCell = 0.03;

float SandGlintLevel(vec2 xz, int level, mat2 toPixels, float setting, vec3 n, vec3 H)
{
    const float size = kGlintCell * exp2(float(level));
    const vec2 g = xz / size;
    const uint h = TerrainHash(uvec2(ivec2(floor(g)) + 0x40000) ^ uvec2(0x9E37u + uint(level) * 0x3C6Fu, 0x7F4Au));
    if (float(h & 0xFFu) >= 56.0 * setting * exp2(-0.6 * float(level))) return 0.0;
    // The grain: a point somewhere in its cell, round on screen and about a pixel across. Sized on the ground, by the
    // pixel's longest side as the cells are, it was a dash several pixels wide and one high at the game's view along
    // the ground (the user's rvk-frame-152956).
    const vec2 at = 0.25 + 0.5 * vec2((h >> 8) & 0xFFu, (h >> 16) & 0xFFu) / 255.0;
    const vec2 off = fract(g) - at;
    const float grain = (1.0 - smoothstep(0.4, 0.9, length(toPixels * (off * size))))
                      * (1.0 - smoothstep(0.35, 0.45, length(off)));
    if (grain <= 0.0) return 0.0;
    // How well it mirrors: most grains faintly, a few brightly.
    const float shine = float(h >> 24) / 255.0;
    const float bright = 0.2 + 0.8 * shine * shine;
    // Its facet.
    const uint h2 = TerrainHash(uvec2(h, h >> 11));
    const vec3 tilt = vec3(float(h2 & 0x3FFu), float((h2 >> 10) & 0x3FFu), float(h2 >> 20)) / vec3(1023.0, 1023.0, 4095.0);
    const vec3 facet = normalize(n + 0.9 * vec3(tilt.x * 2.0 - 1.0, tilt.z - 0.5, tilt.y * 2.0 - 1.0));
    return grain * bright * smoothstep(0.955, 0.99, dot(facet, H));
}

float SandGlint(GroundRelief r, vec2 cell, vec3 posW, vec3 n, vec3 px, vec3 py)
{
    const float setting = clamp(FL.sand.z, 0.0, 1.0), k = (0.5 + 0.5 * setting) * NativeSand(r.cell);
    if (setting <= 0.0 || k <= 0.0 || dot(FL.sunDir.xyz, FL.sunDir.xyz) <= 0.0 || dot(n, n) <= 1e-8) return 0.0;
    const vec3 L = -normalize(FL.sunDir.xyz);
    const float fade = smoothstep(0.05, 0.3, L.y) * (1.0 - smoothstep(40.0, 60.0, length(posW - C.eyePos.xyz)));
    if (fade <= 0.0) return 0.0;
    const float foot = max(length(px), length(py));
    const float steps = clamp(log2(max(1.5 * foot / kGlintCell, 1.0)), 0.0, 5.0);
    const int level = int(steps);
    const float t = steps - float(level);
    const vec2 xz = posW.xz + (cell - r.cell) / max(D.terrain.x, 1e-6);
    const vec3 H = normalize(L + normalize(C.eyePos.xyz - posW));
    // From a step on the ground to the pixels it crosses on screen (the inverse of the ground's screen derivatives).
    const mat2 J = mat2(px.xz, py.xz);
    const float det = determinant(J);
    if (abs(det) <= 1e-12) return 0.0;
    const mat2 toPixels = inverse(J);
    n = normalize(n);
    float g = SandGlintLevel(xz, level, toPixels, setting, n, H) * (1.0 - t);
    if (t > 0.0 && level < 5) g += SandGlintLevel(xz, level + 1, toPixels, setting, n, H) * t;
    return 20.0 * k * fade * g;
}
