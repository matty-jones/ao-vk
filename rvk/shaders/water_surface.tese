#version 450
// The water's surface (water_surface.tesc): a point of the game's triangle, moved by the waves - as tall as its body of
// water is big, calming towards the shores and falls (calm: rvk/water_surface.cpp) and in the shallows - with what
// water.frag reads, as water.vert's grid gives it.
#include "water_common.glsl"

// Lower-left domain origin, counter-clockwise; fractional spacing: points slide in as the camera nears rather than
// popping in at each whole level (as the ground's: ffp.tese).
layout(triangles, fractional_odd_spacing, ccw) in;

layout(location = 0) in vec3 tPos[];
layout(location = 1) in vec3 tInfo[];
layout(location = 2) patch in float tSpacing;

layout(location = 0) out vec3 vPos;         // world position (displaced)
layout(location = 1) out vec3 vNormal;      // the waves' normal (world)
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec3 vRest;        // where the surface is at rest
layout(location = 5) out float vFold;       // the waves' compression (foam on the crests)
layout(location = 6) out float vSpacing;    // the mesh's spacing here (world)
layout(location = 7) out float vAlpha;      // 1 (the grid's: the game's shore fade is for its steps, not its surface)
layout(location = 8) out vec3 vFlowN;       // (a fall's: unused)
layout(location = 9) out vec4 vFlow;        // (a fall's: unused)

void main()
{
    const vec3 b = gl_TessCoord;
    const vec3 rest = b.x * tPos[0] + b.y * tPos[1] + b.z * tPos[2];
    const vec3 info = b.x * tInfo[0] + b.y * tInfo[1] + b.z * tInfo[2];
    const float calm = info.x, body = info.y;
    // A pond stays calm, the open sea has a swell: the waves' height and length by the body's size (as the grid's by
    // the map's), and calmer in the shallows.
    const float scale = W.surface.x * (0.12 + 0.88 * body * body) * calm * mix(0.25, 1.0, Shallow(rest));
    const float wavelength = 0.35 + 0.65 * body;
    vec3 n, prevN;
    float fold, prevFold;
    const vec3 p = rest + Waves(rest.xz, W.time.x, tSpacing, scale, wavelength, n, fold);
    const vec3 prevP = rest + Waves(rest.xz, W.time.y, tSpacing, scale, wavelength, prevN, prevFold);
    vPos = p;
    vNormal = n;
    vRest = rest;
    vFold = fold;
    vSpacing = tSpacing;
    vAlpha = 1.0;
    vFlowN = vec3(0.0, 1.0, 0.0);
    vFlow = vec4(0.0);
    vClip = W.viewProj * vec4(p, 1.0);
    vPrevClip = W.prevViewProj * vec4(prevP, 1.0);
    gl_Position = vClip;
    if (W.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;   // the TAA's sub-pixel offset, as every scene draw has
}
