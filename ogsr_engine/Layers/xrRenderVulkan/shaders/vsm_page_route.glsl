// xrRenderVulkan — VSM page routing: the tail every VSM caster VS ends with.
//
// Takes a WORLD-space position and a physical page slot, and produces the
// clip-space position inside that page's atlas sub-rect plus the four clip
// planes that stop the triangle bleeding into neighbouring sub-rects.
//
// This is a STATEMENT block, not a function, and is #included INSIDE main() —
// the same "body include" idiom the tree/world shaders already use. It writes
// gl_Position / gl_ClipDistance directly, so the compiled SPIR-V is identical
// to the copy it replaces (no function boundary introduced).
//
// It was copy-pasted into EIGHT casters — tree page/meshlet/hull/impostor, grass
// page, static page, static page (alpha-tested) and skinned page — each with its
// own atlas macros. The clip-plane and sub-rect maths is subtle and shared: a fix
// to it had to land in eight files or the atlases would disagree.
//
// CONTRACT at the include site:
//   in scope   uint slot   physical page slot (already range-checked)
//   declared   pageList[]  (SSBO, uvec4 per page: x=level, yz=page coord)
//              vsm         (VsmParams UBO: view, level[], zparams)
//              out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; }
//   #defined   VSM_ROUTE_WP       the in-scope world-space position to route
//              VSM_ROUTE_ATLAS_W / VSM_ROUTE_ATLAS_H — target atlas grid, in pages
//              (TV_*, GP_*, VSM_*_S or VSM_* depending on which atlas this pass writes)

    uvec4 pg   = pageList[slot];
    int   L    = int(pg.x);
    ivec2 page = ivec2(pg.yz);
    vec3  lp   = (vsm.view * vec4(VSM_ROUTE_WP, 1.0)).xyz;              // light space
    vec2  origin = vsm.level[L].xy;
    float pw     = vsm.level[L].z / float(VSM_PAGES_AXIS);    // page world size
    vec2  pmin   = origin + vec2(page) * pw;
    vec2  pmax   = pmin + vec2(pw);
    vec2  nxy    = (lp.xy - pmin) / pw * 2.0 - 1.0;           // page-local NDC [-1,1]
    float nz     = (lp.z - vsm.zparams.x) * vsm.zparams.y;    // global clipmap depth [0,1]

    // Clip to the page rect (no bleed into adjacent atlas sub-rects).
    gl_ClipDistance[0] = lp.x - pmin.x;
    gl_ClipDistance[1] = pmax.x - lp.x;
    gl_ClipDistance[2] = lp.y - pmin.y;
    gl_ClipDistance[3] = pmax.y - lp.y;

    // Place into the physical page's atlas sub-rect (slot -> grid cell).
    uint  ax = slot % uint(VSM_ROUTE_ATLAS_W);
    uint  ay = slot / uint(VSM_ROUTE_ATLAS_W);
    float hX = 1.0 / float(VSM_ROUTE_ATLAS_W);
    float hY = 1.0 / float(VSM_ROUTE_ATLAS_H);
    float cx = (float(ax) + 0.5) * 2.0 * hX - 1.0;
    float cy = (float(ay) + 0.5) * 2.0 * hY - 1.0;
    gl_Position = vec4(cx + nxy.x * hX, cy + nxy.y * hY, nz, 1.0);
