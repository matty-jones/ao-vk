#version 450
// One triangle covering the target. RVK_FAR (fullscreen_far.vert): at the far plane, for a pass drawn only where
// nothing nearer is (the sky into a street portal's opening: portal.cpp).
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
#ifdef RVK_FAR
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
#else
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
#endif
}
