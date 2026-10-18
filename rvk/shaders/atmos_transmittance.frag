#version 450
// The transmittance table (rvk atmosphere.cpp; atmos_common.glsl): for each height and angle, how much light gets
// through the air to its top - the sun's light reddened through much air at a low sun.
#include "atmos_common.glsl"

layout(push_constant) uniform Push {
    vec4 size;          // the table's width, height; z w: the air's tune (Mie, ozone)
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    float r, mu;
    AtmosTransFromUv(gl_FragCoord.xy / P.size.xy, r, mu);
    vec3 o = vec3(0.0, r, 0.0), d = vec3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
    float t = AtmosSphere(o, d, kTopR);
    const int kSteps = 40;
    vec3 depth = vec3(0.0);
    float dt = max(t, 0.0) / float(kSteps);
    for (int i = 0; i < kSteps; ++i) {
        vec3 p = o + d * ((float(i) + 0.5) * dt);
        depth += AtmosAt(length(p) - kGroundR, P.size.zw).extinction * dt;
    }
    outColor = vec4(exp(-depth), 1.0);
}
