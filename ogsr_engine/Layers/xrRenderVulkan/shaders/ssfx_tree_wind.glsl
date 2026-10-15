// xrRenderVulkan — SSFX tree wind (port of screenspace_wind.h trunk+branches).
// Shared by tree.vert (forward) AND tree_depth.vert (shadow caster) so the bent
// geometry and its shadow match — otherwise a swaying trunk self-shadows and
// shows a black "untextured" band where the rigid shadow falls on the bent mesh.
// Displacement is WORLD-space (VP-independent): both passes add it before their
// own view-proj. Requires the flow map bound at set 0, binding 1.

#ifndef SSFX_TREE_WIND_GLSL
#define SSFX_TREE_WIND_GLSL

layout(set = 0, binding = 1) uniform sampler2D s_waves;   // wind_wave.dds flow map

// xy = trunk XZ bend, z = gust factor (fed to the branch flutter).
vec3 ssfxTrunk(vec3 pos, float H, vec2 dir, float speed, float phase,
               float animZ, float trunkAnimSpeed, float bend)
{
    float sqrtSpeed = clamp(sqrt(speed * 1.66), 0.0, 1.0);
    float Phase  = phase + animZ * trunkAnimSpeed;
    float TWave  = (cos(Phase) * sin(Phase * 5.0) + 0.5) * bend;
    float WSpeed = clamp(sqrtSpeed * 1.5, 0.0, 1.0);
    float Base   = WSpeed * 0.006 * clamp(1.0 - H * 0.005, 0.0, 1.0);
    Base        *= H * H * TWave * WSpeed;
    return vec3(vec2(Base) * dir, clamp((TWave + 1.0) * 0.5, 0.0, 1.0));
}

// Full crown motion (includes the trunk bend), faded toward the base by tc_y.
vec3 ssfxBranches(vec3 pos, float H, float tc_y, vec2 dir, float speed, float phase,
                  vec2 animXY, float animZ, float branchAnimSpeed,
                  float trunkAnimSpeed, float bend, float flutterAmp)
{
    vec2 Offset = -animXY * branchAnimSpeed;
    vec3 Flow   = textureLod(s_waves, (pos.xz + Offset)       * 0.02, 0.0).xyz;
    vec3 Flow2  = textureLod(s_waves, (pos.xz + Offset * 0.2) * 0.1,  0.0).xyz;
    vec3 bm     = vec3(Flow.x, Flow2.y, Flow.y) * 2.0 - 1.0;
    vec3 trunk  = ssfxTrunk(pos, H, dir, speed, phase, animZ, trunkAnimSpeed, bend);
    bm.xz *= trunk.z * clamp(H * 0.1, 1.0, 2.5);
    bm.xz += Flow2.z * dir;
    bm.y  *= clamp(H * 0.1, 0.0, 1.0);
    bm    *= (1.0 - tc_y) * speed * flutterAmp;
    bm.xz += trunk.xy;
    return bm;
}

// windClass: 2 = foliage (bend + flutter), 1 = trunk (gentle bend only),
// 0 = rigid. crownH (r_wind_tree_crown, wind_params.w in every tree push): height (m)
// over which the leaf flutter fades in from the tree base — the low trunk/undergrowth
// stays still; <= 0 = no gate (default). Lives here so ALL passes (forward, cascade
// caster, VSM page/meshlet) gate identically — a shadow must flutter like its crown.
// Returns the world-space displacement to add to worldPos.
vec3 ssfxTreeWind(uint windClass, vec3 worldPos, float H, float tc_y, vec2 dir,
                  float speed, float phase, vec3 anim,
                  float branchAnimSpeed, float trunkAnimSpeed, float bend, float flutterAmp,
                  float crownH)
{
    if (windClass == 2u) {
        if (crownH > 0.0) flutterAmp *= clamp(H / crownH, 0.0, 1.0);
        return ssfxBranches(worldPos, H, tc_y, dir, speed, phase, anim.xy, anim.z,
                            branchAnimSpeed, trunkAnimSpeed, bend, flutterAmp);
    }
    if (windClass == 1u) {
        vec3 tr = ssfxTrunk(worldPos, H, dir, speed, phase, anim.z, trunkAnimSpeed, bend);
        return vec3(tr.x, 0.0, tr.y);
    }
    return vec3(0.0);
}

// Wind setup + apply, in WORLD space. Every tree pass (forward, depth, motion
// vectors, hull/voxel debug, and the VSM page/meshlet/hull casters) needs the
// same four lines before the call — including the wind-direction convention
// (-wind_params.x + pi/2) and the minimum-speed floor. Those were written out
// nine times; the casters MUST agree with the forward pass or a tree bends one
// way and its shadow the other.
//   wp    world-space vertex position
//   H     height above the tree base (wp.y - baseY)
//   tc_y  scaled UV V (fades flutter toward the trunk base); 0.35 for hull/voxel
//         proxies, which carry no UVs
//   cls   wind class from TreeInstance._p0 (2=foliage, 1=trunk, 0=rigid)
//   baseY tree origin world Y = per-tree phase
vec3 ssfxTreeWindWorld(vec3 wp, float H, float tc_y, uint cls, float baseY,
                       vec4 wparams, vec4 wsetup, vec4 wanim)
{
    float r    = -wparams.x + 1.57079;
    vec2  wdir = vec2(cos(r), sin(r));
    float spd  = max(wsetup.w, clamp(wparams.y * 0.001, 0.0, 1.0));
    return ssfxTreeWind(cls, wp, H, tc_y, wdir, spd, baseY,
                        wanim.xyz, wsetup.x, wsetup.y, wsetup.z, wanim.w, wparams.w);
}

#endif // SSFX_TREE_WIND_GLSL
