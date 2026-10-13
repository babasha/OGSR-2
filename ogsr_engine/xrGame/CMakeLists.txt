# Linux build of xrGame. Source list is generated from xrGame.vcxproj — see
# CMakeSources.cmake and cmake/gen_module_sources.sh. Keep in sync with the Windows project;
# do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

add_library(xrGame STATIC ${XRGAME_SOURCES})

target_include_directories(xrGame PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(xrGame PRIVATE
    ${OGSR_ENGINE_DIR}/COMMON_AI
    ${OGSR_ENGINE_DIR}/COMMON_AI/PATH
    ${OGSR_ENGINE_DIR}/COMMON_AI/GRAPH
    ${OGSR_ENGINE_DIR}/COMMON_AI/ALGORITHMS
    # Engine headers. xr_3da is the executable, not a library, so there's no target to take
    # these from; on Windows they arrive via the solution-wide include paths.
    ${OGSR_ENGINE_DIR}/xr_3da
    ${OGSR_ENGINE_DIR}/xrOnlineNet
    ${OGSR_ENGINE_DIR}/Include
    # Headers only here (not yet CMake targets); the libraries are resolved at the xr_3da link.
    ${OGSR_ROOT_DIR}/3rd_party/GameNetworkingSockets/include
    # eax.h's OPENAL branch includes <al.h> (the embedded sound-env editor).
    ${OGSR_3RD_PARTY_DIR}/OpenAL_Soft/include/AL
)

target_compile_definitions(xrGame PUBLIC
    XRGAME_STATIC
)
target_compile_definitions(xrGame PRIVATE
    NDEBUG
    XRGAME_EXPORTS
    RMLUI_STATIC_LIB
    # ODE is built single-precision (also PUBLIC on the ode target).
    dSINGLE
)
# Deliberately NOT carried over from the vcxproj:
#  - MSVC: tells ODE's headers the compiler is MSVC (config selection); wrong on g++/clang.
#  - _WINDOWS/_USRDLL: same call as the other modules.

target_link_libraries(xrGame PUBLIC
    xrCore
    xrCDB
    xrParticles
    xrSound
    Luabind
    ode
    zstd
    imgui
    RmlUi::Core
)
if(ANDROID)
    # xr_input.h is the SDL CInput there (xr_input_sdl.h includes <SDL2/SDL.h>).
    target_link_libraries(xrGame PUBLIC SDL2::SDL2)
endif()
