// The cliffs' rock as every stage lays it: the fragment shader's texturing (terrain.glsl) and the native ground's
// tessellation (ffp.tese), which pushes the rock out of the faces where the texture has it - the same coordinates, so
// the stone that stands out is the stone that is drawn.

// The cliffs' rock (rvk terrain.cpp; RVK_CliffRock): in, and on. Its maps' bindless images (0 albedo, 1 normal, 2 ORM).
bool TerrainRockOn() { return floatBitsToUint(FL.terrainRock.x) != 0u && FL.terrainRock.w > 0.0; }
uint TerrainRockImage(int k) { return floatBitsToUint(FL.terrainRock[k]) - 1u; }

uint TerrainHash(uvec2 p)
{
    uint h = p.x * 0x8DA6B343u ^ p.y * 0xD8163841u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// Smooth value noise, 0..1.
float TerrainNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    uvec2 c = uvec2(ivec2(i) + 0x4000);
    float a = float(TerrainHash(c) & 0xFFFFu), b = float(TerrainHash(c + uvec2(1, 0)) & 0xFFFFu);
    float d = float(TerrainHash(c + uvec2(0, 1)) & 0xFFFFu), e = float(TerrainHash(c + uvec2(1, 1)) & 0xFFFFu);
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y) / 65535.0;
}

// The rock's two lays at a point, in repeats: along x (facing z; xy) and along z (facing x; zw), with the height down,
// their layers bent by a slow wave along the cliff (strata, not rows).
vec4 TerrainRockLays(vec3 posW)
{
    const float s = 1.0 / FL.terrainRock.w;
    vec4 uv = s * vec4(posW.x, -posW.y, posW.z, -posW.y);
    uv.y += 0.45 * (TerrainNoise(vec2(posW.x / 70.0, 3.7)) - 0.5) + 0.15 * (TerrainNoise(vec2(posW.x / 19.0, 8.1)) - 0.5);
    uv.w += 0.45 * (TerrainNoise(vec2(posW.z / 70.0, 5.3)) - 0.5) + 0.15 * (TerrainNoise(vec2(posW.z / 19.0, 1.9)) - 0.5);
    return uv;
}

// How far the rock stands out of the ground here, along n (its smooth normal), at most depth units: its brightness
// against its mean, stretched (a photo's crevices are its dark), on steep ground only - flat ground, where feet stand and grass
// grows, keeps its height. A coarse level of the albedo: the rock's shapes, not its grain.
vec3 TerrainRockRelief(vec3 posW, vec3 n, float depth)
{
    if (!TerrainRockOn() || depth <= 0.0 || dot(n, n) <= 1e-8) return vec3(0.0);
    n = normalize(n);
    // (From ~45 degrees, all of it by ~60: ground one can stand on keeps its height - characters stand on the game's.)
    const float side = smoothstep(0.7, 0.5, abs(n.y)) * clamp(FL.terrain.x, 0.0, 1.0);
    if (side <= 0.0) return vec3(0.0);
    const vec4 uv = TerrainRockLays(posW);
    vec2 across = pow(abs(n.zx) + 1e-4, vec2(4.0));
    across /= across.x + across.y;
    const vec3 luma = vec3(0.3, 0.59, 0.11);
    const float a = dot(textureLod(sampler2D(texImages[TerrainRockImage(0)], bindlessSamplers[D.sampIdx.w]), uv.xy, 3.5).rgb, luma);
    const float b = dot(textureLod(sampler2D(texImages[TerrainRockImage(0)], bindlessSamplers[D.sampIdx.w]), uv.zw, 3.5).rgb, luma);
    const float h = (a * across.x + b * across.y) / max(dot(FL.terrainRock2.rgb, luma), 0.02) - 1.0;
    return n * (clamp(3.0 * h, -1.0, 1.0) * depth * side);   // (a photo's brightness: +-30 % about its mean)
}
