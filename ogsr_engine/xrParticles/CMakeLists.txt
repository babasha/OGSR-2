# Linux build of xrParticles. Source list is generated from xrParticles.vcxproj — see
# CMakeSources.cmake and cmake/gen_module_sources.sh. Keep in sync with the Windows project;
# do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

add_library(xrParticles STATIC ${XRPARTICLES_SOURCES})

target_include_directories(xrParticles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(xrParticles PUBLIC
    XR_PARTICLES_STATIC
)
target_compile_definitions(xrParticles PRIVATE
    NDEBUG
)
# _WINDOWS is in the vcxproj but unused anywhere in xrParticles itself (no grep hits), same
# call as xrCore/xrCDB: left undefined for the Linux target.

target_link_libraries(xrParticles PUBLIC
    xrCore
)
