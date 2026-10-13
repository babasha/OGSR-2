#version 450
// Dear ImGui vertex shader (vk_imgui backend). ImDrawVert: pos(vec2), uv(vec2),
// col(RGBA8 unorm). Push constant maps screen pixels → clip space.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PC {
    vec2 uScale;
    vec2 uTranslate;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main()
{
    vUV    = aUV;
    vColor = aColor;
    gl_Position = vec4(aPos * pc.uScale + pc.uTranslate, 0.0, 1.0);
}
