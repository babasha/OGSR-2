// meshoptimizer (c) zeux — vendored at 3rd_party/Src/meshoptimizer, MIT.
// Amalgamated build: only the translation units the renderer actually uses.
// Phase 1 of the cluster-LOD system needs the clusterizer (meshlet split +
// bounds); Phase 2 adds simplifier.cpp (cluster DAG). Compiled without the
// PCH (see the vcxproj entry, same as vma_impl.cpp).
#include "../../../3rd_party/Src/meshoptimizer/src/allocator.cpp"
#include "../../../3rd_party/Src/meshoptimizer/src/clusterizer.cpp"
#include "../../../3rd_party/Src/meshoptimizer/src/meshletutils.cpp"   // meshopt_computeMeshletBounds/SphereBounds
#include "../../../3rd_party/Src/meshoptimizer/src/simplifier.cpp"     // meshopt_simplify* (cluster DAG)
#include "../../../3rd_party/Src/meshoptimizer/src/partition.cpp"      // meshopt_partitionClusters (DAG grouping)
#include "../../../3rd_party/Src/meshoptimizer/src/indexgenerator.cpp" // meshopt_generatePositionRemap (seam welding)
#include "../../../3rd_party/Src/meshoptimizer/src/spatialorder.cpp"   // meshopt_spatialSortRemap (partition ordering)

// clusterlod — the reference Nanite-style hierarchy builder shipped with
// meshoptimizer (demo/clusterlod.h, "intended to be used as is"). Handles what
// our hand-rolled DAG got wrong: position-welded boundary locks (lmap-UV seam
// duplicates!), permissive simplification with UV-seam protect bits, sloppy
// fallback for stuck groups, tuned monotonic error propagation.
#define CLUSTERLOD_IMPLEMENTATION
#include "../../../3rd_party/Src/meshoptimizer/demo/clusterlod.h"
