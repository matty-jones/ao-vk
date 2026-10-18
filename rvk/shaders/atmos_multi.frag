#version 450
// The multiple scattering table (rvk atmosphere.cpp; atmos_common.glsl; Hillaire 2020 §5.5): at each height and sun
// angle, the light scattered twice or more, as an isotropic source - from 64 directions round the point, the light
// scattered once towards it (L2) and how much of any light the air there passes on (fms); the series of all orders
// summed as L2 / (1 - fms). The ground's light (albedo 0.3) bounces back in.
#include "atmos_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D transmittance;
layout(push_constant) uniform Push {
    vec4 size;          // the table's width, height; z w: the air's tune (Mie, ozone)
} P;
layout(location = 0) out vec4 outColor;

vec3 Trans(float r, float mu)
{
    return textureLod(transmittance, AtmosTransUv(r, mu), 0.0).rgb;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / P.size.xy;
    float muSun = uv.x * 2.0 - 1.0;
    float r = kGroundR + max(uv.y * (kTopR - kGroundR), 0.01);
    vec3 o = vec3(0.0, r, 0.0), sun = vec3(sqrt(max(1.0 - muSun * muSun, 0.0)), muSun, 0.0);
    const int kDirs = 8, kSteps = 20;
    vec3 L2 = vec3(0.0), fms = vec3(0.0);
    for (int a = 0; a < kDirs; ++a) {
        for (int b = 0; b < kDirs; ++b) {
            // (Even over the sphere: cos(theta) evenly spaced, the azimuth too.)
            float ct = 1.0 - 2.0 * (float(a) + 0.5) / float(kDirs), st = sqrt(max(1.0 - ct * ct, 0.0));
            float ph = 2.0 * kAtmosPi * (float(b) + 0.5) / float(kDirs);
            vec3 d = vec3(st * cos(ph), ct, st * sin(ph));
            float tGround = AtmosSphere(o, d, kGroundR), tTop = AtmosSphere(o, d, kTopR);
            float tEnd = tGround > 0.0 ? tGround : tTop;
            float dt = max(tEnd, 0.0) / float(kSteps);
            vec3 through = vec3(1.0), L = vec3(0.0), f = vec3(0.0);
            for (int i = 0; i < kSteps; ++i) {
                vec3 p = o + d * ((float(i) + 0.5) * dt);
                float h = length(p);
                AtmosMedium m = AtmosAt(h - kGroundR, P.size.zw);
                vec3 up = p / h;
                float mu = dot(up, sun);
                vec3 sunT = AtmosSphere(p, sun, kGroundR) > 0.0 ? vec3(0.0) : Trans(h, mu);
                vec3 scat = m.rayleigh + vec3(m.mie);
                vec3 stepT = exp(-m.extinction * dt);
                vec3 inv = 1.0 / max(m.extinction, vec3(1e-6));
                L += through * (scat * sunT / (4.0 * kAtmosPi) - scat * sunT / (4.0 * kAtmosPi) * stepT) * inv;
                f += through * (scat - scat * stepT) * inv;
                through *= stepT;
            }
            if (tGround > 0.0) {
                vec3 p = o + d * tGround, up = normalize(p);
                float mu = dot(up, sun);
                L += through * Trans(kGroundR, mu) * max(mu, 0.0) * 0.3 / kAtmosPi;
            }
            L2 += L;
            fms += f;
        }
    }
    float n = float(kDirs * kDirs);
    L2 /= n;
    fms /= n;
    outColor = vec4(L2 / max(1.0 - fms, vec3(1e-3)), 1.0);
}
