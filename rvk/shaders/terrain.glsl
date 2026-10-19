// The ground's own texturing (rvk terrain.cpp), on both of its passes - the base (its texture) and the lighting pass
// (the lightmap, with the base texture's normal and material maps, F_BUMPBASE / MAT_BASE, and the cliffs' rock relief):
// - Cliffs: the game lays the ground's texture from above (each 4 x 4 cell its own copy of the tile, turned or flipped),
//   so down a steep face it stretches into streaks. Where the ground is steep the face gets its own lay instead, along
//   x or z with the height, in world space - one lay down the whole cliff, no seams between its cells - blended by the
//   slope (FL.terrain.x): rock (FL.terrainRock: a photo of a layered cliff, tinted to the cell's tile's mean colour,
//   D.terrainAt.xyz, so a sand cliff is sand-coloured rock), or without it the cell's own tile at its repeats per unit
//   (D.terrain.x, rvk FitTerrain). Flat ground keeps the game's coordinates and so its layout: its tiles are placed by
//   hand - transitions, edges, patches of another ground in the middle of a tile - and turned per cell against
//   repeats already.
// - Repeats: the faces are read through a grid of hexagons laid on the world, each with its own offset into the
//   texture, blended by their weights and the texture's brightness (Mikkelsen, "Practical Real-Time Hex-Tiling",
//   2022), so a cliff isn't one picture repeated in rows (FL.terrain.y). The rock's offsets are only sideways - offset
//   up or down too, its layers were chopped into short dashes, a camouflage pattern - and its layers bend gently up and
//   down along the cliff instead, as strata do, so its bands don't stack into the same rows up a high wall.
// - Variation: big lighter and darker patches over the land and up cliffs (FL.terrain.z).
// - The native ground (rvk native_ground.cpp, D.terrain.y): rvk's own patches, the game's coordinates set 0 the cell
//   coordinates (world / cell size); each pixel's cell's tile and turn from the tile-code texture, read with the
//   continuous coordinates' derivatives (no seams in the mip levels between cells), the game's clamped sampler.
// The base pass and its lighting pass compute the same weights from the same vertices: the relief and highlights stay
// on the texture they belong to. Derivatives are taken once (TerrainMapAt, in uniform control flow); every read
// after it is a textureGrad, so the reads may be skipped where their weight is 0.

const float kTerrainBase = 1.0, kTerrainLight = 2.0;

// The draw is a ground chunk whose base texture lies on the world from above (rvk FillTerrainRecord).
bool TerrainMapped() { return D.terrainAt.w > 0.5; }

#include "terrain_rock.glsl"

struct TerrainMap {
    vec2 uv[3];         // the game's (from above), a face along x (it faces z: x and the height), along z (faces x)
    vec2 dx[3], dy[3];  // ... their screen derivatives
    vec3 w;             // ... their weights
    vec2 hexOff[9];     // each face's three hexagons' offsets into the texture (3 i + cell; none from above)
    vec3 hexW[3];       // ... their weights (TerrainSettle adds the texture's brightness)
    float hex;          // how much of the hexagons (0: the faces' coordinates as they are)
    vec3 px, py;        // the position's screen derivatives (the normal maps' relief)
    float side;         // how steep (0 .. 1): the cliffs' share
    bool rock;          // the faces are the rock's (else the cell's tile)
};

// The hexagon grid over a lay (st: in repeats of the texture, on the world): the three cells around st, the weights
// of their centres (Mikkelsen's TriangleGrid, sharpened) and each cell's offset into the texture (down: this much of a
// repeat at most). A cell is about two thirds of the texture across; each lay has its own grid (seed).
void TerrainHexGrid(vec2 st, uint seed, float down, out vec3 w, out vec2 off0, out vec2 off1, out vec2 off2)
{
    st *= 2.0 * sqrt(3.0) * 0.45;
    vec2 skewed = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    ivec2 base = ivec2(floor(skewed));
    vec3 t = vec3(fract(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z), s2 = 2.0 * s - 1.0;
    w = pow(vec3(-t.z * s2, s - t.y * s2, s - t.x * s2), vec3(7.0));
    w /= w.x + w.y + w.z;
    ivec2 is = ivec2(s);
    uint h0 = TerrainHash(uvec2(base + is + 0x4000) + seed);
    uint h1 = TerrainHash(uvec2(base + ivec2(is.x, 1 - is.y) + 0x4000) + seed);
    uint h2 = TerrainHash(uvec2(base + ivec2(1 - is.x, is.y) + 0x4000) + seed);
    const vec2 scale = vec2(1.0, down) / 65536.0;
    off0 = vec2(float(h0 & 0xFFFFu), float(h0 >> 16)) * scale;
    off1 = vec2(float(h1 & 0xFFFFu), float(h1 >> 16)) * scale;
    off2 = vec2(float(h2 & 0xFFFFu), float(h2 >> 16)) * scale;
}

// Where and how the ground's texture lies at this pixel. uv: the game's coordinates; n: the interpolated normal.
TerrainMap TerrainMapAt(vec3 posW, vec2 uv, vec3 n)
{
    TerrainMap m;
    m.px = dFdx(posW);
    m.py = dFdy(posW);
    m.uv[0] = uv;
    m.dx[0] = dFdx(uv);
    m.dy[0] = dFdy(uv);
    // The faces: along x or z, and down - in world space, the rock's size or the tile's repeats per unit.
    m.rock = TerrainRockOn();
    const float s = m.rock ? 1.0 / FL.terrainRock.w : D.terrain.x;
    m.uv[1] = s * vec2(posW.x, -posW.y);
    m.dx[1] = s * vec2(m.px.x, -m.px.y);
    m.dy[1] = s * vec2(m.py.x, -m.py.y);
    m.uv[2] = s * vec2(posW.z, -posW.y);
    m.dx[2] = s * vec2(m.px.z, -m.px.y);
    m.dy[2] = s * vec2(m.py.z, -m.py.y);
    if (m.rock) {                       // (the strata bend gently enough to leave the derivatives as they are)
        const vec4 lays = TerrainRockLays(posW);
        m.uv[1] = lays.xy;
        m.uv[2] = lays.zw;
    }
    // How steep: the triangle's own slope (from the position's derivatives), not the interpolated normal - the game's
    // cells are 4 units across, a cliff one or two of them high, and its corners' normals, shared with the flat ground
    // round it, make the face look far less steep than it is; the stretch is the triangle's.
    vec3 face = cross(m.px, m.py);
    float len = length(face);
    vec3 nu = len > 1e-12 ? face / len : vec3(0.0, 1.0, 0.0);
    // Steeper than ~32 degrees starts the faces (the texture from above already a fifth longer down the slope); past
    // ~57 they have it all.
    m.side = smoothstep(0.85, 0.55, abs(nu.y)) * clamp(FL.terrain.x, 0.0, 1.0);
    // Which way it faces (the lay along x or along z): the interpolated normal, smooth from one triangle to the next -
    // each triangle's own made the lays change at its edges, seams across a wall at an angle. (Its own where the
    // interpolated one has no length.)
    vec3 nf = dot(n, n) > 0.0 ? normalize(n) : nu;
    vec2 across = pow(abs(nf.zx) + 1e-4, vec2(4.0));
    across /= across.x + across.y;
    m.w = vec3(1.0 - m.side, m.side * across);
    m.hex = clamp(FL.terrain.y, 0.0, 1.0);
    m.hexW[0] = vec3(0.0);
    for (int i = 1; i < 3; ++i) {
        m.hexW[i] = vec3(0.0);
        if (m.hex > 0.0 && m.w[i] > 0.002)
            TerrainHexGrid(m.uv[i], uint(i) * 0x9E3779B9u, m.rock ? 0.0 : 1.0, m.hexW[i], m.hexOff[3 * i],
                           m.hexOff[3 * i + 1], m.hexOff[3 * i + 2]);
    }
    return m;
}

// ---- the native ground: each cell's own tile (docs/native-ground-plan.md)

bool TerrainNative() { return floatBitsToUint(D.terrain.y) != 0u; }

// The cell's tile's bindless image and its turn: the tile-code texture's texel (R, G the image, B the turn).
uvec2 NativeCell(vec2 cell)
{
    const uint codes = floatBitsToUint(D.terrain.y) - 1u;
    const ivec2 size = textureSize(sampler2D(texImages[codes], bindlessSamplers[D.sampIdx.w]), 0);
    const vec4 t = texelFetch(sampler2D(texImages[codes], bindlessSamplers[D.sampIdx.w]),
                              clamp(ivec2(floor(cell)), ivec2(0), size - 1), 0);
    const uvec4 b = uvec4(t * 255.0 + 0.5);
    return uvec2(b.r | b.g << 8, b.b);
}

// Within a cell (a along x, b along z, 0..1) its tile's coordinates as the game lays them, by the turn
// (docs/ground-format.md): 0 (a, 1 - b), 1 (1 - b, 1 - a), 2 (1 - a, b), 3 (b, a). And a step's.
vec2 NativeTurn(vec2 f, uint turn)
{
    return turn == 0u ? vec2(f.x, 1.0 - f.y) : turn == 1u ? vec2(1.0 - f.y, 1.0 - f.x)
         : turn == 2u ? vec2(1.0 - f.x, f.y) : f.yx;
}
vec2 NativeTurnStep(vec2 d, uint turn)
{
    return turn == 0u ? vec2(d.x, -d.y) : turn == 1u ? vec2(-d.y, -d.x) : turn == 2u ? vec2(-d.x, d.y) : d.yx;
}

// The ground from above: this pixel's cell's tile. smp: the game's (clamped).
vec4 NativeTop(vec2 cell, vec2 dx, vec2 dy, sampler smp)
{
    const uvec2 c = NativeCell(cell);
    return textureGrad(sampler2D(texImages[nonuniformEXT(c.x)], smp), NativeTurn(fract(cell), c.y),
                       NativeTurnStep(dx, c.y), NativeTurnStep(dy, c.y));
}

// The lighting pass: the cells' tiles' normal maps (D.terrain.z: their texture's bindless image + 1; R, G each cell's
// tile's normal map's image, 0xFFFF none).
bool TerrainNativeMaps() { return TerrainNative() && floatBitsToUint(D.terrain.z) != 0u; }
uint NativeNormalImage(vec2 cell)
{
    const uint maps = floatBitsToUint(D.terrain.z) - 1u;
    const ivec2 size = textureSize(sampler2D(texImages[maps], bindlessSamplers[D.sampIdx.w]), 0);
    const vec4 t = texelFetch(sampler2D(texImages[maps], bindlessSamplers[D.sampIdx.w]),
                              clamp(ivec2(floor(cell)), ivec2(0), size - 1), 0);
    const uvec2 b = uvec2(t.rg * 255.0 + 0.5);
    return b.r | b.g << 8;
}

// How much of the sky this point of the native ground sees (rvk native_ground.cpp SkyVisibility: the horizon all round
// from the heights), as a factor on its light: 1 - strength x the hidden part (FL.terrain2.x, RVK_GroundAo). 1 without.
float NativeSky(vec3 posW)
{
    const uint sky = floatBitsToUint(D.terrain.w);
    if (!TerrainNative() || sky == 0u || FL.terrain2.x <= 0.0) return 1.0;
    const ivec2 size = textureSize(sampler2D(texImages[sky - 1u], bindlessSamplers[D.sampIdx.w]), 0);
    const vec2 cell = posW.xz / vec2(1.0 / max(D.terrain.x, 1e-6));   // (D.terrain.x: a cell's repeats a unit)
    const float v = textureLod(sampler2D(texImages[sky - 1u], bindlessSamplers[D.sampIdx.w]), (cell + 0.5) / vec2(size), 0.0).r;
    return 1.0 - FL.terrain2.x * (1.0 - v);
}

// The cells' tiles' mean colours filtered between cells at this point: what the rock on a cliff is tinted to - per
// cell, a cliff not quite upright crossed cells corner to corner and the tint jumped at each (grass tiles: green
// wedges up the face). From the means' texture (D.terrainAt.x on the native base pass), else the cell's tile's
// smallest level.
vec3 NativeMean(vec2 cell)
{
    const uint means = floatBitsToUint(D.terrainAt.x);
    if (means != 0u && means < 65537u) {
        const vec2 size = vec2(textureSize(sampler2D(texImages[means - 1u], bindlessSamplers[D.sampIdx.w]), 0));
        return textureLod(sampler2D(texImages[means - 1u], bindlessSamplers[D.sampIdx.w]), cell / size, 0.0).rgb;
    }
    const uvec2 c = NativeCell(cell);
    return textureLod(sampler2D(texImages[nonuniformEXT(c.x)], bindlessSamplers[D.sampIdx.w]), vec2(0.5), 14.0).rgb;
}

#define TERRAIN_READ(img, smp, uv, dx, dy) textureGrad(sampler2D(img, smp), uv, dx, dy)
// The faces read past the tile's 0..1 (world space, offsets): tiled, whatever the draw's own sampler - the game reads
// each cell's tile clamped, which smears its edge into stripes there. (The normal maps' sampler: tiled, mipmapped,
// anisotropic; every draw has it.) The game's coordinates from above keep the draw's: tiled, a transition tile's
// filtering would bleed its far edge along the cell's borders.
#define TERRAIN_FACE_READ(img, uv, dx, dy) textureGrad(sampler2D(img, bindlessSamplers[D.sampIdx.w]), uv, dx, dy)

// One lay: from above, the game's coordinates as they are (top, read with smp); a face (face) through its hexagons
// (their weights m.hexW), blended with its plain coordinates by m.hex.
vec4 TerrainLay(texture2D top, sampler smp, texture2D face, TerrainMap m, int i)
{
    if (i == 0) return TERRAIN_READ(top, smp, m.uv[0], m.dx[0], m.dy[0]);   // (the native ground: TerrainBase)
    vec4 c = vec4(0.0);
    if (m.hex < 1.0) c = TERRAIN_FACE_READ(face, m.uv[i], m.dx[i], m.dy[i]) * (1.0 - m.hex);
    if (m.hex > 0.0)
        for (int k = 0; k < 3; ++k)
            if (m.hexW[i][k] > 0.002)
                c += TERRAIN_FACE_READ(face, m.uv[i] + m.hexOff[3 * i + k], m.dx[i], m.dy[i]) * (m.hexW[i][k] * m.hex);
    return c;
}

// All the lays blended, the faces' part times faceScale, with the weights already settled (TerrainSettle).
vec4 TerrainTexture(texture2D top, sampler smp, texture2D face, TerrainMap m, vec4 faceScale)
{
    vec4 c = m.w.x > 0.002 ? TerrainLay(top, smp, face, m, 0) * m.w.x : vec4(0.0);
    for (int i = 1; i < 3; ++i)
        if (m.w[i] > 0.002) c += TerrainLay(top, smp, face, m, i) * (faceScale * m.w[i]);
    return c;
}

// The faces' hexagons' weights take their texture's brightness in (the brighter cell wins where they meet - a clean
// seam rather than a blur); every map read after it uses the same. face: the faces' colour (the rock's albedo or the
// cell's tile) - both passes settle them from the same texture.
void TerrainSettle(texture2D face, inout TerrainMap m)
{
    if (m.hex <= 0.0) return;
    for (int i = 1; i < 3; ++i) {
        if (m.w[i] <= 0.002) continue;
        vec3 lum;
        for (int k = 0; k < 3; ++k)
            lum[k] = Luma(TERRAIN_FACE_READ(face, m.uv[i] + m.hexOff[3 * i + k], m.dx[i], m.dy[i]).rgb);
        vec3 w = mix(vec3(1.0), lum, 0.6) * m.hexW[i];
        m.hexW[i] = w / max(w.x + w.y + w.z, 1e-6);
    }
}

// The rock's colour made the cell's: its tile's mean colour over the rock's (a sand cliff is sand-coloured rock, a dark
// one dark) - its brightness whole, its hue two thirds (a grass tile on a steep face made it bright green rock: now
// a mossy hint); 1 without the rock or with the tile's colour not known yet.
vec4 TerrainRockTint(TerrainMap m)
{
    if (!m.rock || (!TerrainNative() && D.terrainAt.x < 0.0)) return vec4(1.0);
    const vec3 mean = TerrainNative() ? NativeMean(m.uv[0]) : D.terrainAt.xyz;
    const vec3 tint = clamp(mean / max(FL.terrainRock2.rgb, vec3(0.02)), vec3(0.2), vec3(5.0));
    return vec4(mix(vec3(dot(tint, vec3(0.3, 0.59, 0.11))), tint, 0.66), 1.0);
}

// The base texture as the ground lays it (the base pass's colour; the lighting pass's PBR albedo): settles the weights.
// image: the base texture's bindless image; the faces' is the rock's albedo, or the same. The rock's contrast is a
// photo's - harsher than the game's soft ground beside it: a quarter of the way to its own mean.
vec4 TerrainBase(uint image, sampler smp, inout TerrainMap m)
{
    if (TerrainNative()) {
        // The native ground: its cell's tile from above; the faces rock, or the cell's tile laid on them (no
        // hexagons: one cell's tile in each pixel's own).
        const vec4 top = m.w.x > 0.002 ? NativeTop(m.uv[0], m.dx[0], m.dy[0], smp) * m.w.x : vec4(0.0);
        if (m.w.x >= 0.998) return top;
        if (m.rock) {
            TerrainSettle(texImages[TerrainRockImage(0)], m);
            vec4 faces = vec4(0.0);
            for (int i = 1; i < 3; ++i)
                if (m.w[i] > 0.002) faces += TerrainLay(texImages[image], smp, texImages[TerrainRockImage(0)], m, i) * m.w[i];
            const float side = m.w.y + m.w.z;
            faces.rgb = mix(FL.terrainRock2.rgb * side, faces.rgb, 0.75);
            return top + faces * TerrainRockTint(m);
        }
        const uvec2 c = NativeCell(m.uv[0]);
        vec4 faces = vec4(0.0);
        for (int i = 1; i < 3; ++i)
            if (m.w[i] > 0.002)
                faces += textureGrad(sampler2D(texImages[nonuniformEXT(c.x)], bindlessSamplers[D.sampIdx.w]), m.uv[i],
                                     m.dx[i], m.dy[i]) * m.w[i];
        return top + faces;
    }
    const uint faceImage = m.rock ? TerrainRockImage(0) : image;
    TerrainSettle(texImages[faceImage], m);
    if (!m.rock) return TerrainTexture(texImages[image], smp, texImages[faceImage], m, vec4(1.0));
    vec4 top = m.w.x > 0.002 ? TerrainLay(texImages[image], smp, texImages[faceImage], m, 0) * m.w.x : vec4(0.0);
    vec4 faces = TerrainTexture(texImages[image], smp, texImages[faceImage], m, vec4(1.0)) - top;
    const float side = m.w.y + m.w.z;
    if (side > 0.002) faces.rgb = mix(FL.terrainRock2.rgb * side, faces.rgb, 0.75);
    return top + faces * TerrainRockTint(m);
}

// The relief of normal maps laid like the texture: from above the base's (top, strength), on the faces the rock's
// (or the base's), each lay's relief through its own coordinates' derivatives (as NormalMapNormal), blended.
vec3 TerrainNormalMap(vec3 n, texture2D top, float topStrength, texture2D face, float faceStrength, TerrainMap m)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (m.w[i] <= 0.002) continue;
        vec2 uvx = m.dx[i], uvy = m.dy[i];
        vec3 t;
        if (i == 0 && TerrainNativeMaps()) {
            // (The native ground from above: the cell's tile's own normal map, turned as its tile is.)
            const uvec2 c = NativeCell(m.uv[0]);
            const uint img = NativeNormalImage(m.uv[0]);
            if (img == 0xFFFFu) { sum += n * m.w[0]; continue; }
            uvx = NativeTurnStep(m.dx[0], c.y);
            uvy = NativeTurnStep(m.dy[0], c.y);
            t = textureGrad(sampler2D(texImages[nonuniformEXT(img)], bindlessSamplers[D.sampIdx.w]),
                            NativeTurn(fract(m.uv[0]), c.y), uvx, uvy).xyz * 2.0 - 1.0;
        } else {
            t = TerrainLay(top, bindlessSamplers[D.sampIdx.w], face, m, i).xyz * 2.0 - 1.0;
        }
        t.z = max(t.z, 0.05);
        vec2 slope = vec2(-t.x, t.y) / t.z * (i == 0 ? topStrength : faceStrength);
        float worldPerUv = sqrt(max(dot(m.px, m.px), dot(m.py, m.py)) / max(max(dot(uvx, uvx), dot(uvy, uvy)), 1e-12));
        sum += BendNormal(n, m.px, m.py, dot(slope, uvx) * worldPerUv, dot(slope, uvy) * worldPerUv) * m.w[i];
    }
    float len2 = dot(sum, sum);
    return len2 > 0.0 ? sum * (length(n) * inversesqrt(len2)) : n;
}

// Large-scale variation in the ground's colour: patches a few tens to hundreds of units across, a little lighter or
// darker, more or less saturated (x FL.terrain.z). Laid as the texture is: from above on flat ground, on the face
// along x or z and down where it is steep (m.w) - from above only, a cliff took the same value all the way down and
// the patches ran down it in streaks.
vec3 TerrainPatches(vec2 p)    // tone, saturation (-0.5 .. 0.5)
{
    float big = TerrainNoise(p / 190.0 + 31.7) - 0.5, mid = TerrainNoise(p / 47.0 - 11.3) - 0.5;
    return vec3(0.65 * big + 0.35 * mid, TerrainNoise(p / 113.0 + 77.1) - 0.5, 0.0);
}

vec3 TerrainVariation(vec3 c, vec3 posW, TerrainMap m)
{
    float k = FL.terrain.z;
    if (k <= 0.0) return c;
    vec3 v = TerrainPatches(posW.xz) * m.w.x;
    if (m.w.y > 0.002) v += TerrainPatches(vec2(posW.x, -posW.y)) * m.w.y;
    if (m.w.z > 0.002) v += TerrainPatches(vec2(posW.z, -posW.y)) * m.w.z;
    float l = Luma(c);
    c = mix(vec3(l), c, clamp(1.0 + 0.35 * k * v.y, 0.0, 2.0));
    return c * clamp(1.0 + 0.26 * k * v.x, 0.5, 1.5);
}

// The native ground's lightmap at lightmap coordinates at: the hour's (stage 0), faded in from the last hour's
// (FL.terrain2.y, z: the game swaps them whole, and the land's light jumped).
vec4 NativeLightmapAt(vec2 at)
{
    const vec4 now = textureLod(TEX0, at, 0.0);
    const uint prev = floatBitsToUint(FL.terrain2.y);
    if (prev == 0u) return now;
    return mix(textureLod(sampler2D(texImages[prev - 1u], bindlessSamplers[D.sampIdx.x]), at, 0.0), now, FL.terrain2.z);
}

// The native ground's slopes lit from where the sun stands now: the lightmap is a + b.n of the slope n as the game
// baked it with the sun of its hour (rvk native_ground.cpp FitNativeLight: FL.terrainLight2.xyz b), and with b turned
// to the sun's bearing now (FL.terrainLight.xyz; its height as baked) the ratio of the two is how much lighter or
// darker this slope is now - flat ground 1, as baked; a slope that turns towards the sun through the hour brightens,
// one that turns away darkens. (In the 0..1 of the texels; floored at the palette's darkest, ratio clamped.)
float NativeSunTurn(vec3 n)
{
    if (FL.terrainLight2.w <= 0.0 || dot(n, n) <= 1e-8) return 1.0;
    n = normalize(n);
    const float baked = FL.terrainLight.w + dot(FL.terrainLight2.xyz, n);
    const float now = FL.terrainLight.w + dot(FL.terrainLight.xyz, n);
    return mix(1.0, clamp(max(now, 0.06) / max(baked, 0.06), 0.4, 2.0), FL.terrainLight2.w);
}
