# Linux build of xrSound. Source list is generated from xrSound.vcxproj — see
# CMakeSources.cmake and cmake/gen_module_sources.sh. Keep in sync with the Windows project;
# do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

# OpenAL-Soft is vendored on Windows only as prebuilt binaries (3rd_party/Src/OpenAL_Soft/bin,
# .../libs — no source), so there's nothing to build from source on Linux. OpenAL-Soft is a
# cross-platform library that Linux distros ship as a system package (Arch: `openal`); link
# against that instead via CMake's bundled FindOpenAL module. The vendored AL/al.h, AL/alc.h
# headers under 3rd_party/Src/OpenAL_Soft/include/AL are used for the build regardless, same
# as on Windows, so the API surface xrSound compiles against doesn't drift from what MSVC sees.
if(NOT ANDROID) # Android: OpenAL::OpenAL is built from source (top-level CMakeLists.txt)
    find_package(OpenAL REQUIRED)
endif()

add_library(xrSound STATIC ${XRSOUND_SOURCES})

target_include_directories(xrSound PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(xrSound PRIVATE
    ${OGSR_3RD_PARTY_DIR}/OpenAL_Soft/include/AL
    ${OGSR_3RD_PARTY_DIR}/eax/Include
)

target_compile_definitions(xrSound PUBLIC
    XRSOUND_STATIC
)
target_compile_definitions(xrSound PRIVATE
    NDEBUG
    # eax.h has a portable OPENAL branch (see guids.cpp) that skips the DirectSound-only path
    # entirely when this is defined — this is how the module avoids <dsound.h> on Linux.
    OPENAL
)
# _WINDOWS/_USRDLL are in the vcxproj but unused anywhere in xrSound itself (no grep hits),
# same call as xrCore/xrCDB/xrParticles: left undefined for the Linux target.

target_link_libraries(xrSound PUBLIC
    xrCore
    xrCDB
    OpenAL::OpenAL
    vorbisfile
    vorbis
    ogg
)
