#version 450
#extension GL_GOOGLE_include_directive : require
#include "froxel.glsl"   // exp-Z slice <-> view-Z mapping (shared with the CPU particle pass)

// GPU particle billboard vertex shader.
// Reads particle data from SSBO via aliveList -> pool, expands to a
// camera-facing quad (6 verts = 2 triangles, TRIANGLE_LIST). Computes the
// atlas frame UV (dfFramed effects) and passes the bindless texture slot to
// the fragment shader.

struct GPUParticle {
    vec4  pos_age;
    vec4  vel_life;
    uint  colorRGBA;
    float rot;
    vec2  size;
    uint  defId;
    float origin_x, origin_y, origin_z;   // emitter origin (unused by the draw)
};

// Per-program texture/atlas/orientation metadata (must match VK::GPUParticles::TexInfo).
struct TexInfo {
    int   layer;        // bindless texture slot (= program), or -1 untextured
    uint  flags;        // bit0 framed, bit1 animated, bit2 random-frame, bit3 align-to-path
    vec2  frameSize;    // UV size of one atlas frame
    uint  frameDimX;    // atlas columns
    uint  frameCount;
    float frameSpeed;
    float _pad;
    vec4  alignDir;     // bit3: billboard T axis for zero-velocity particles
};

// readonly: this stage only READS the three simulation buffers (the compute passes
// own the writes). Without the qualifier the SPIR-V declares them writable and the
// driver must assume a vertex stage may store into them — VUID-RuntimeSpirv-NonWritable-06341
// ×6, caught 16-08. Marking them also lets the compiler treat the loads as invariant.
layout(set = 0, binding = 0) readonly buffer PoolBuf     { GPUParticle pool[]; };
layout(set = 0, binding = 2) readonly buffer AliveBuf    { uint aliveList[]; };
layout(set = 0, binding = 7) readonly buffer TexInfoBuf  { TexInfo texInfo[]; };

layout(push_constant) uniform PC {
    mat4  viewProj;
    vec4  camRight;      // xyz = right; w = froxel near Z
    vec4  camUp;         // xyz = up;    w = log2(far/near)
    vec4  camPosStr;     // xyz = camera pos; w = probe strength
    vec4  camDirClamp;   // xyz = camera forward; w = radiance clamp
};

layout(location = 0) out vec4      fragColor;
layout(location = 1) out vec2      fragUV;
layout(location = 2) flat out int  fragLayer;
layout(location = 3) out float     fragVolW;      // froxel Z slice (volumetric light probe)
layout(location = 4) out vec2      fragScreenUV;  // screen UV for the froxel XY lookup

const vec2 kQuadUV[6] = vec2[](
    vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0)
);

const vec2 kQuadCorner[6] = vec2[](
    vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2( 1.0, -1.0),
    vec2(-1.0,  1.0), vec2( 1.0, 1.0), vec2( 1.0, -1.0)
);

vec4 unpackBGRA8(uint c)
{
    float b = float( c        & 0xFFu) / 255.0;
    float g = float((c >> 8)  & 0xFFu) / 255.0;
    float r = float((c >> 16) & 0xFFu) / 255.0;
    float a = float((c >> 24) & 0xFFu) / 255.0;
    return vec4(r, g, b, a);
}

void main()
{
    uint particleIdx = aliveList[gl_InstanceIndex];
    GPUParticle p = pool[particleIdx];

    if (p.pos_age.w < 0.0) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        fragLayer = -1;
        return;
    }

    float cosa = cos(p.rot);
    float sina = sin(p.rot);
    vec2  corner = kQuadCorner[gl_VertexIndex];

    // defId bit31 = HUD routing flag — mask it off before indexing.
    TexInfo ti = texInfo[p.defId & 0x7FFFFFFFu];

    // Billboard axes. Default = camera-facing. dfAlignToPath (CPU BuildVertices
    // parity): T = velocity dir, or the authored default axis when standing
    // still — campfire flames are UPRIGHT quads (T = world-up) that only yaw
    // toward the camera; drawn camera-facing they lie flat when viewed from
    // above ("fire burns below the logs") and stand up huge from afar.
    vec3 axR = camRight.xyz;
    vec3 axU = camUp.xyz;
    if ((ti.flags & 8u) != 0u) {
        vec3  v  = p.vel_life.xyz;
        float sp = length(v);
        vec3  T  = (sp > 1e-3) ? v / sp : ti.alignDir.xyz;
        vec3  Rx = cross(T, camDirClamp.xyz);
        float rl = length(Rx);
        axU = T;
        axR = (rl > 1e-4) ? Rx / rl : camRight.xyz;
    }

    // PAPI size = full extent; the CPU path's FillSprite uses size*0.5 as the
    // billboard half-radius. Without the 0.5 GPU quads render 2x the CPU size
    // (flames visibly oversized and hanging below their anchor).
    vec3 R = axR * ((p.size.x * corner.x * cosa - p.size.y * corner.y * sina) * 0.5)
           + axU * ((p.size.x * corner.x * sina + p.size.y * corner.y * cosa) * 0.5);

    vec3 worldPos = p.pos_age.xyz + R;
    gl_Position   = viewProj * vec4(worldPos, 1.0);

    // ---- Froxel volumetric light-probe mapping (Stage-0 smoke lighting) -----
    // Same exp-Z slice as the CPU particle pass. Screen UV is derived from the
    // clip position with the flipped-Y viewport convention (vp.height < 0):
    //   framebufferUV = (0.5 + 0.5*ndc.x, 0.5 - 0.5*ndc.y) — matches gl_FragCoord/extent.
    float near   = max(camRight.w, 1e-4);
    float logFN  = max(camUp.w, 1e-4);
    float viewZ  = dot(worldPos - camPosStr.xyz, camDirClamp.xyz);
    fragVolW     = Froxel_SliceFromViewZ(max(viewZ, near), near, logFN);
    vec2 ndc     = gl_Position.xy / max(gl_Position.w, 1e-6);
    fragScreenUV = vec2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);

    // ---- Texture / atlas-frame UV (ti fetched above for the axes) ----------
    vec2 baseUV = kQuadUV[gl_VertexIndex];
    vec2 uv = baseUV;
    if ((ti.flags & 1u) != 0u && ti.frameCount > 1u) {       // dfFramed: pick a frame
        // Mirrors the CPU path: dfAnimated advances at the authored frames/sec
        // (ExecuteAnimate), NOT over the particle lifetime — long-lived flames
        // (life 1000s) would otherwise freeze on one frame. dfRandomFrame adds
        // a stable per-particle offset (CPU randomizes at birth); plain framed
        // stays on its birth frame.
        uint fi = 0u;
        if ((ti.flags & 2u) != 0u)                           // dfAnimated
            fi = uint(max(p.pos_age.w, 0.0) * max(ti.frameSpeed, 0.0));
        if ((ti.flags & 4u) != 0u)                           // dfRandomFrame
            fi += particleIdx * 2654435761u;
        fi = fi % ti.frameCount;
        uint dim = max(ti.frameDimX, 1u);
        vec2 lt = vec2(float(fi % dim), float(fi / dim)) * ti.frameSize;
        uv = lt + baseUV * ti.frameSize;
    }

    fragUV    = uv;
    fragColor = unpackBGRA8(p.colorRGBA);
    fragLayer = ti.layer;
}
