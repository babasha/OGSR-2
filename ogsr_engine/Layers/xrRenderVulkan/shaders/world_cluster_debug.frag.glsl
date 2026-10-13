#version 450
// Cluster-LOD debug overlay: stable pseudo-random color per cluster entry id.
// As the camera moves away the DAG cut coarsens → the color patches (and the
// wireframe density in mode 2) visibly merge, UE-Nanite-cluster-view style.
// Mode 3 (0.5 < tint.a <= 1.5): flat PATH color pushed per group (plain /
// per-mesh DAG / component DAG). Mode 4 (tint.a > 1.5): LOD health from the
// vertex stage (red = this entry can never be replaced by a coarser parent).
layout(location = 0) flat in uint vCluster;
layout(location = 1) flat in vec4 vHealth;
layout(location = 0) out vec4 oColor;

layout(push_constant) uniform PC { mat4 mvp; vec4 tint; } pc;

vec3 hashColor(uint n)
{
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28) + 4u)) ^ n) * 277803737u;
    n = (n >> 22) ^ n;
    // Bright, saturated-ish palette: never too dark so edges stay readable.
    return 0.25 + 0.75 * vec3(float((n      ) & 1023u) / 1023.0,
                              float((n >> 10) & 1023u) / 1023.0,
                              float((n >> 20) & 1023u) / 1023.0);
}

void main()
{
    if      (pc.tint.a > 1.5) oColor = vHealth;
    else if (pc.tint.a > 0.5) oColor = vec4(pc.tint.rgb, 1.0);
    else                      oColor = vec4(hashColor(vCluster), 1.0);
}
