// xrRenderVulkan — froxel MEDIA SPLATTING: deposit a particle's density+colour into
// the smoke accumulation grid. Shared by the two producers that feed the same grid:
//
//   vol_splat.comp       CPU-simulated smoke particles (SmokeParticle pool)
//   gp_media_splat.comp  GPU particles that opted into the media pass
//
// They MUST agree bit-for-bit: both write the SAME `accum` buffer, which vol_inject
// then reads as one field. Two different deposit weightings would make a GPU-particle
// puff and a CPU puff of equal size read as different densities in the same volume.
// It was copy-pasted into both.
//
// CONTRACT: the including shader declares the `accum[]` SSBO (uint, 4 per cell:
// density, r, g, b — fixed-point) and a push block `pc` with vec4 dims (xyz = grid
// dims, w = fixed-point scale).
#ifndef SPLAT_COMMON_GLSL
#define SPLAT_COMMON_GLSL

// Atomic-add a weighted density+colour contribution into one accum cell (bounds-checked).
void depositCell(ivec3 c, ivec3 dim, float dens, vec3 col)
{
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, dim))) return;
    uint cell = (uint(c.z) * uint(dim.y) + uint(c.y)) * uint(dim.x) + uint(c.x);
    float sc = pc.dims.w;
    atomicAdd(accum[cell * 4u + 0u], uint(dens * sc));
    atomicAdd(accum[cell * 4u + 1u], uint(dens * col.r * sc));
    atomicAdd(accum[cell * 4u + 2u], uint(dens * col.g * sc));
    atomicAdd(accum[cell * 4u + 3u], uint(dens * col.b * sc));
}

// FOOTPRINT: spread the particle over a froxel neighbourhood of radius `fr` cells
// (the caller sizes fr from its own notion of particle radius vs the local cell size,
// capped by pc.params.z so cost ~ particles × (2·fr+1)³ stays bounded). fr <= 0 is a
// point splat, the cheap path. Gaussian falloff → soft, non-grainy blobs; NOT
// normalized, so a bigger particle deposits more total mass = denser & larger clouds.
void splatFootprint(vec3 fc, ivec3 dim, int fr, float dens, vec3 col)
{
    ivec3 ci = ivec3(floor(fc));
    if (fr <= 0) {
        depositCell(clamp(ci, ivec3(0), dim - 1), dim, dens, col);
        return;
    }
    float sig2 = max(float(fr) * float(fr) * 0.5, 0.25);
    for (int dz = -fr; dz <= fr; ++dz)
    for (int dy = -fr; dy <= fr; ++dy)
    for (int dx = -fr; dx <= fr; ++dx) {
        ivec3 c   = ci + ivec3(dx, dy, dz);
        vec3  off = (vec3(c) + 0.5) - fc;
        float wgt = exp(-dot(off, off) / sig2);
        if (wgt > 0.02) depositCell(c, dim, dens * wgt, col);
    }
}

#endif // SPLAT_COMMON_GLSL
