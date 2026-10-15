#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 4 alpha sort, pass 2/3: EXCLUSIVE PREFIX SUM over the 512-bucket
// histogram (one workgroup, shared-memory Hillis-Steele). After this,
// sortData[b] = first output slot of bucket b; the scatter atomically
// increments it per particle.

#include "gp_common.glsl"

layout(local_size_x = GP_SORT_BUCKETS) in;

shared uint s_scan[GP_SORT_BUCKETS];

void main()
{
    uint i = gl_LocalInvocationID.x;
    s_scan[i] = sortData[i];
    barrier();

    // Inclusive Hillis-Steele scan...
    for (uint off = 1u; off < uint(GP_SORT_BUCKETS); off <<= 1u) {
        uint v = (i >= off) ? s_scan[i - off] : 0u;
        barrier();
        s_scan[i] += v;
        barrier();
    }

    // ...converted to exclusive on store.
    sortData[i] = (i > 0u) ? s_scan[i - 1u] : 0u;
}
