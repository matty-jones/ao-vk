#version 450
// The zone hand-over's held picture (handover.cpp): the last view of the old zone, slowly pushed in, softened, dimmed
// and drained of a little colour while the new zone loads under it, laid over the frame premultiplied by `alpha` (the
// fade into the new zone).
layout(set = 0, binding = 0) uniform sampler2D held;
layout(push_constant) uniform Params {
    vec4 look;       // x: zoom (1 = as held), y: blur radius (pixels), z: brightness, w: colour taken out (0..1)
    vec4 frame;      // x: alpha, yz: 1 / the target's size
} p;
layout(location = 0) out vec4 outColor;

const vec2 kDisc[12] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
                              vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
                              vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

void main()
{
    vec2 uv = (gl_FragCoord.xy * p.frame.yz - 0.5) / p.look.x + 0.5;
    vec3 c = texture(held, uv).rgb;
    if (p.look.y > 0.05) {
        vec2 r = p.look.y * p.frame.yz;
        for (int i = 0; i < 12; ++i)
            c += texture(held, uv + kDisc[i] * r).rgb;
        c /= 13.0;
    }
    float grey = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(c, vec3(grey), p.look.w) * p.look.z;
    outColor = vec4(c * p.frame.x, p.frame.x);
}
