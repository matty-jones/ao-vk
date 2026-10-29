#version 450
// RVK_SsrMode 1 (sssr.cpp): AMD's reflections (FidelityFX SSSR - what each pixel's surface reflects, traced through
// the depth buffer's hierarchy, the probe where the screen doesn't show it, denoised over frames) made into what
// ssr.frag gives the tone mapping: the reflected colour and how much of it shows - reflectivity x Fresnel x strength,
// as ours has it, so RVK_Ssr, RVK_SsrWater, RVK_SsrGloss and the wet look keep their say.
layout(set = 0, binding = 0) uniform sampler2D reflected;       // SSSR's output
layout(set = 0, binding = 1) uniform sampler2D surface;         // gbuffer.frag: world normal, roughness
layout(set = 0, binding = 2) uniform sampler2D reflectivity;    // G
layout(set = 0, binding = 3) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 size;          // width, height; strength
    vec4 view[3];       // the view matrix's rotation, rows (world -> view)
} P;
layout(location = 0) out vec4 outSsr;

void main()
{
    ivec2 ipix = ivec2(gl_FragCoord.xy);
    float r = texelFetch(reflectivity, ipix, 0).g * P.size.z;
    float d = texelFetch(depthTex, ipix, 0).r;
    if (r < 0.004 || d >= 1.0) { outSsr = vec4(0.0); return; }
    float z = P.proj.y / (min(d, 0.999999) - P.proj.x);
    vec2 ndc = vec2((float(ipix.x) + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (float(ipix.y) + 0.5) / P.size.y * 2.0);
    vec3 v = normalize(vec3(ndc.x * z / P.proj.z, ndc.y * z / P.proj.w, z));
    vec3 nw = texelFetch(surface, ipix, 0).xyz;
    vec3 n = normalize(vec3(dot(nw, vec3(P.view[0].x, P.view[1].x, P.view[2].x)),
                            dot(nw, vec3(P.view[0].y, P.view[1].y, P.view[2].y)),
                            dot(nw, vec3(P.view[0].z, P.view[1].z, P.view[2].z))));
    // Fresnel: surfaces reflect more seen at a grazing angle (ssr.frag's).
    float cosView = clamp(dot(-v, n), 0.0, 1.0);
    float amount = r * (0.25 + 0.75 * pow(1.0 - cosView, 3.0));
    vec3 c = texelFetch(reflected, ipix, 0).rgb;
    outSsr = vec4(max(c, vec3(0.0)), clamp(amount, 0.0, 1.0));
}
