#version 450
// Fullscreen-triangle tonemap pass. No vertex buffer — gl_VertexIndex 0..2
// expands to a triangle that covers the screen. The fragment uses gl_FragCoord
// + texelFetch, so no UVs / no viewport-orientation worries.
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
