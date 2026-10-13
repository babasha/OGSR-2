# Linux build of the vendored (X-Ray-modified) ODE. Source list copied from
# contrib/msvc7/ode_default/default.vcxproj — keep in sync with it.
cmake_minimum_required(VERSION 3.18)
project(ode CXX C)

add_library(ode STATIC
    ode/src/array.cpp
    ode/src/Bounder33.cpp
    ode/src/collision_kernel.cpp
    ode/src/collision_quadtreespace.cpp
    ode/src/collision_space.cpp
    ode/src/collision_std.cpp
    ode/src/collision_transform.cpp
    ode/src/collision_util.cpp
    contrib/msvc7/ode_default/de_padf_integration.cpp
    ode/src/error.cpp
    ode/src/export-dif.cpp
    ode/src/fastdot.c
    ode/src/fastldlt.c
    ode/src/fastlsolve.c
    ode/src/fastltsolve.c
    ode/src/joint.cpp
    ode/src/lcp.cpp
    ode/src/mass.cpp
    ode/src/mat.cpp
    ode/src/matrix.cpp
    ode/src/memory.cpp
    ode/src/misc.cpp
    ode/src/obstack.cpp
    ode/src/ode.cpp
    ode/src/odemath.cpp
    ode/src/quickstep.cpp
    ode/src/rotation.cpp
    ode/src/step.cpp
    ode/src/stepfast.cpp
    ode/src/StepJointInternal.cpp
    ode/src/testing.cpp
    ode/src/timer.cpp
    ode/src/util.cpp
)

target_include_directories(ode PUBLIC
    # <ode/common.h> etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # The engine also reaches into ODE internals: "ode/src/joint.h", "include/ode/ode.h".
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# dSINGLE must match every consumer (xrGame defines it too). The vcxproj's MSVC define is
# unused by these sources (they test _MSC_VER / WIN32 directly), so it's not carried over.
target_compile_definitions(ode PUBLIC dSINGLE)
target_compile_definitions(ode PRIVATE NDEBUG dNODEBUG)
