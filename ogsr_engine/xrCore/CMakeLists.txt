# Linux build of xrCore. Source list is generated from xrCore.vcxproj — see CMakeSources.cmake
# and cmake/gen_module_sources.sh. Keep in sync with the Windows project; do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

if(OGSR_SWITCH)
    # TracyClient.cpp #errors on unknown platforms (no Horizon port); TRACY_ENABLE is off, so the headers are no-ops.
    list(FILTER XRCORE_SOURCES EXCLUDE REGEX "TracyClient\.cpp$")
endif()
add_library(xrCore STATIC ${XRCORE_SOURCES})

target_include_directories(xrCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    # xrCore.h (a public header, transitively included by every other module) pulls in
    # <tracy/Tracy.hpp> unconditionally, so this needs to be PUBLIC, not just used to build
    # xrCore's own .cpp files.
    ${OGSR_3RD_PARTY_DIR}/tracy/public
)
# libsquashfs (LocatorAPI.cpp, arc_sqfs.cpp) and lzo (rt_compressor.cpp) come in through
# target_link_libraries below; their include dirs are PUBLIC on those targets.
# arc_sqfs.cpp includes <../cpputils/cpputils/include/...>: resolved relative to any
# 3rd_party/Src/<lib> include dir (MSVC: concurrentqueue; here: lzo, PUBLIC on its target).

target_compile_definitions(xrCore PUBLIC
    XRCORE_STATIC
    XRENGINE_STATIC
    # xrCore.h's multi-threaded-CRT #error check (`#ifndef _MT`) fires for every translation
    # unit that includes it, in every module — not just xrCore's own .cpp files — so this has
    # to be PUBLIC too.
    _MT
)
target_compile_definitions(xrCore PRIVATE
    NDEBUG
    APPVEYOR_BUILD_VERSION=\"\"
    APPVEYOR_REPO_NAME=\"\"
)
# _WINDOWS/_USRDLL are in the vcxproj (MSVC subsystem/DLL-project markers) but nothing in
# xrCore itself checks them — and defining _WINDOWS actively breaks the vendored lzo headers,
# which take it to mean "we're on Windows" and then fail an ULONG_MAX sanity check against
# Linux's LP64 data model (lzodefs.h ~line 521). Left undefined here on purpose.

target_link_libraries(xrCore PUBLIC
    $<$<NOT:$<BOOL:${OGSR_SWITCH}>>:mimalloc-static>
    squashfs
    lzo
)
