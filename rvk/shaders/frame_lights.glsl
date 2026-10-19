// The per-frame light block (binding 4), shared by the scene shaders (constants.glsl) and the volumetric light.
// Must match FrameLights in rvk/internal.h (std140).
struct Light {
    vec4 diffuse, specular, ambient;
    vec4 position;      // xyz world space, w = D3DLIGHTTYPE
    vec4 direction;     // xyz world space, w = range
    vec4 atten;         // attenuation0, attenuation1, attenuation2, falloff
    vec4 spot;          // cos(theta/2), cos(phi/2), frame lights: point shadow cube + 1 (0 = none), its fade-in (0..1)
};

// The frame's active point / spot lights nearest the camera (Device::SetLightOverride). Replaces the game's
// per-object choice of up to 8 lights, which drops lights on big objects such as the ground.
#ifndef FL_BINDING
#define FL_BINDING 4            // (the tone mapping reads it at 15)
#endif
layout(set = 0, binding = FL_BINDING, std140) uniform FrameLights {
    uvec4 info;                 // light count, pusher count
    mat4 shadowViewProj[4];     // world -> each sun shadow cascade (raw D3DMATRIX memory)
    vec4 cascadeTexel;          // world size of a texel of each cascade
    vec4 cascadeDepth;          // world units per unit of each cascade's depth (soft shadows)
    vec4 effects;               // light through leaves, night glow (x darkness), sun shadow softness, plant push
    vec4 wind;                  // plants' sway: direction x, z, time (s), strength
    vec4 taa;                   // temporal anti-aliasing: this frame's jitter (clip x, y per w), noise offset (0..1), the wind's time last frame
    vec4 shadowParams;          // enabled, strength, cascade count, point light shadow strength
    vec4 sunDir;                // direction the sunlight travels; w = light headroom (F_OVERBRIGHT)
    vec4 sunColor;              // the sun's colour (shadow-casting sun; 0 = none)
    vec4 pbr;                   // PBR materials: direct specular, ambient specular, reflections, occlusion map strength
    vec4 pbr2;                  // x: debug view (0 = off), y: 1 = PBR on, z: emissive maps' brightness, w: the
                                // environment probe (its bits: atlas slot + 1, sampler slot << 20, strength / 15 << 28)
    mat4 prevView, prevProj;    // the world camera last frame (motion vectors; raw D3DMATRIX memory)
    vec4 pushers[16];           // what plants bend away from (characters and their trails): world xyz, seconds since
    vec4 pusherBorn[4];         // ... and seconds since each point was made (4 a vector)
    vec4 leaves;                // leaves (leaves.cpp): branch sway, sprig flutter, gusts, canopy core cut
    vec4 leafView;              // ... their distance (none beyond; thinning out from half of it), on, falling leaves
    vec4 clouds;                // the clouds' shadow map (rvk clouds.cpp): corner x, z, 1 / size, its bindless image + 1 |
                                // sampler << 20 (as the float's bits; 0 = none)
    vec4 clouds2;               // ... the cloud layer's height (world y), the shadow's strength
    vec4 cloudTint;             // the game's cloud sheets with the layer's light (cloud_light.glsl): their colour (w
    vec4 cloudSky;              // brightness); the sky's light (w: the sheets' strength, 0 none); the air's colour (w:
    vec4 cloudAir;              // the horizon's haze)
    vec4 moon;                  // the moonlight (rvk night.cpp): its colour on the ground and grass (0 = none); w: 1 =
                                // the shadow map is the moon's (sunDir: the moon's light), not the sun's
    vec4 sunTrue;               // towards the sun, below the horizon too (the clouds' light); w: how much it is night
    vec4 terrainSun;            // the terrain's shade from a low sun (rvk terrain_sun.cpp): map corner x, z, 1 / size,
                                // bindless image + 1 | sampler << 20 (the float's bits; 0 = none)
    vec4 terrainSun2;           // ... x: the eye's own (0 shaded .. 1), y: its weight (0 high sun .. 1 low)
    vec4 windFrame;             // xy: the wind's patterns' frame (wind.glsl WindAxes, rvk Device::WindFrame); z: a sandstorm's spreading of the sun's light (0 .. 0.75)
    // Lightning (rvk rain.cpp Lightning): this frame's flash - its light on the land (rgb; 0 none), w: the glow in the
    // clouds round the strike, x; towards the strike in view space (xyz), w: how far (world units); the projection's
    // m[0][0], m[1][1], m[2][2], m[3][2] (the tone mapping's view rays).
    vec4 flash;
    vec4 flashView;
    vec4 flashProj;
    vec4 flashWorld;            // ... the strike (world xyz: the storm's fog lit round it)
    vec4 rainNear;              // x: 1 = the tone mapping's rain layer (binding 16) holds this frame's rain; y: how much a
                                // thunderstorm hides the sun (rvk Device::StormOvercast: the land darker, shadows gone); z:
                                // a storm's blowing (ffp.vert FallingLeaf: leaves torn off and carried far); w: 1 = the
                                // screen-space reflections on (ffp_main's wet ground: its own sky sheen lighter)
    vec4 wet;                   // the wet ground (rvk rain.cpp): x how soaked, y the puddles, z the rain's clock, w the
                                // cover map's heights' reference (world y)
    vec4 cover;                 // the rain's cover map (rvk sand_map.cpp RainCoverMap): corner x, z, 1 / size, bindless
                                // image + 1 | sampler << 20 (the float's bits; 0 = none)
    vec4 neon;                  // the tone mapping's neon look: x how much saturated light keeps its colour
                                // (RVK_NeonSat), y the lens streaks' strength (RVK_Streaks), z, w unused
    vec4 sky[4];                // the sky's model (rvk sky.cpp UpdateSkyModel; sky_common.glsl s0..s3)
    vec4 skyView[3];            // the world camera's rotation: dot(skyView[i].xyz, a view-space direction) = its world
                                // part i
    vec4 dawn[2];               // a low sun's light (rvk sky.cpp, RVK_DawnLight): [0] the sun's warm tint (rgb, w how
                                // much), [1] the sky's cool one in the shade (rgb) - 1 1 1 by day -, w the golden
                                // hour's grade (tonemap.frag Grade, RVK_GoldenHour)
    vec4 phase;                 // the dusk's phases (rvk sky.cpp UpdateSunClock), each 0..1: x the golden hour, y the
                                // afterglow, z the blue hour, w night - by the sun's eased height
    vec4 skyRing[8];            // our sky's horizon all round (rvk atmosphere.cpp; RVK_ProcSky): the colour just over the
                                // horizon at azimuths k * 45 degrees from +x towards +z; w 1 when known
    vec4 skyUp;                 // our sky over the land (overhead, 35 degrees up all round); w 1 when known
    vec4 dusk;                  // x: RVK_Afterglow (cloud_light.glsl DuskVivid), y the blue hour's grade (tonemap.frag
                                // Grade; rvk sky.cpp), z w the sun on the world camera's screen (NDC; 1e3 behind it)
    vec4 terrain;               // the ground's texturing (terrain.glsl): cliffs (RVK_CliffTex), the faces' repeats
                                // hidden (RVK_TileHide), large-scale variation (RVK_GroundVary), normal maps' strength
                                // (0 off: the native ground's tiles')
    vec4 terrain2;              // ... x: the native ground's sky visibility's strength (RVK_GroundAo); y the hour's
                                // lightmap before this one (its bindless image + 1, the float's bits; 0 none), z how far
                                // it has faded into this one (0..1); w the grain up close (RVK_GroundGrain)
    vec4 terrainRock;           // the cliffs' rock (rvk terrain.cpp; RVK_CliffRock): its albedo, normal and ORM maps'
                                // bindless images + 1 (the floats' bits; 0 = none), world units a repeat
    vec4 terrainRock2;          // ... its mean colour (rgb), its normal map's strength
    vec4 terrainLight;          // the native ground's lightmap as a + b.n of its slope (rvk native_ground.cpp
                                // FitNativeLight): xyz b turned to the sun's bearing now (b when there is none), w a (0: no fit)
    vec4 terrainLight2;         // ... xyz b as baked, w how much the slopes follow the sun (RVK_GroundSun; 0 none)
    Light lights[64];
} FL;
