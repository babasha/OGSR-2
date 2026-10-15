// xrRenderVulkan - the R4 sky half-cube direction mapping, shared.
//
// X-Ray sky cubemaps are NOT authored against a plain cube: R4 draws them on the
// `hbox_verts` half-cube (dxEnvironmentRender.cpp), whose lower hemisphere is
// squashed flat at y_vis = -0.01 and whose side faces run tc.y from -1 (mid-ring)
// to +1 (top). Net effect: the sky gradient occupies almost the full V range and
// the HORIZON LINE sits at tc.y ~ -0.9999, i.e. the very bottom edge of the side
// faces. A raw `texture(cube, worldDir)` therefore does NOT return "the sky in
// that direction" — the warm sunset band at the horizon is unreachable for any
// upward-facing direction.
//
// This header is the single source of truth for that mapping. It is included by
// BOTH the sky dome draw (sky.frag) and the light-probe prefilter
// (ibl_prefilter.comp), which is the whole point: a probe built with the same
// remap + the same sky_rotation lives in TRUE WORLD SPACE, so everything
// downstream (specular reflection, diffuse SH) can index it with a plain world
// vector and agree with what the player actually sees on the dome.
//
// Historical note: the prefilter used to sample RAW ("the half-cube remap is a
// draw-only thing; the light probe is raw"). It is not draw-only — it is the
// mapping the texture is authored in, so skipping it put the probe's azimuth on
// a different sky than the visible one.
#ifndef SKY_HALFCUBE_GLSL
#define SKY_HALFCUBE_GLSL

// Undo the skybox's geometric spin: R4 rotates the dome by sky_rotation, so
// sampling at R(-theta)*d lands on the texel that geometry would have exposed to
// this view ray. Every consumer of the sky cubes must apply this — a probe that
// skips it pins the sunset glow to a fixed azimuth of the TEXTURE, drifting away
// from both the drawn sky and the sun as the weather rotates the dome.
vec3 SkyUnrotate(vec3 d, float rot)
{
    float c = cos(rot);
    float s = sin(rot);
    return vec3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
}

// Map a normalized world direction to the cubemap sample direction the R4
// half-cube geometry would have produced for it.
vec3 SampleDirHalfCube(vec3 d)
{
    float ax = abs(d.x);
    float az = abs(d.z);
    float m  = max(ax, az);

    // Top cap: d.y dominates. Pass through; textureCube picks +Y face.
    if (d.y > m) {
        return d;
    }

    // Side face — y_vis at the wall is d.y / m, valid in [-0.01, 1].
    // tc.y = lerp(-1, 1, (y_vis + 0.01) / 1.01) = 2*y_vis/1.01 + (0.02/1.01) - 1.
    float yAtSide = d.y / m;
    if (yAtSide >= -0.01) {
        float ty = (yAtSide + 0.01) * (2.0 / 1.01) - 1.0;
        return vec3(d.x / m, ty, d.z / m);
    }

    // Skirt / bottom cap: tc.y ~ -1. Original geometry samples the bottom edge of
    // the side face (X-Ray sky cubemaps put the horizon line there). We must keep
    // the side face dominant — landing on the -Y face instead collapses to
    // whatever uniform/empty content is on that face and visually freezes the
    // sky. Set tc.y just above -1 so |x|=1 or |z|=1 still wins the dominance test.
    return vec3(d.x / m, -0.9999, d.z / m);
}

// Convenience: world direction -> cube sample direction, rotation included.
vec3 SkySampleDir(vec3 worldDir, float rot)
{
    return SampleDirHalfCube(SkyUnrotate(worldDir, rot));
}

#endif // SKY_HALFCUBE_GLSL
