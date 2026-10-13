#version 450
// xrRenderVulkan — VSM crown-HULL caster FS: opaque depth-only. No texture fetch,
// no discard → early-Z stays on; this is the whole point of the hull tier (the
// crown pass is alpha-test-fill-bound — see r_vsm_tree_hull / vulkan-vsm memory).
void main() {}
