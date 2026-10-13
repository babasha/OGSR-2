# Linux build of the OGSR engine. The Windows build stays on the .sln/.vcxproj tree in
# this same directory (MSVC ignores this file); this is a parallel, Linux-only target.
#
# Each module's source list is generated FROM its .vcxproj (see cmake/gen_module_sources.sh),
# so the Linux file list can't silently drift from what MSBuild compiles. Re-run the generator
# after adding/removing/excluding a source file on the Windows side.
#
# Status (2026-09-24): the whole engine links into one Linux executable (xr_3da) on g++ and
# clang++, every third-party lib built from the vendored source (GameNetworkingSockets: vendored
# source + distro OpenSSL/protobuf). Streamline and FidelityFX are Windows-only (reported unavailable).
cmake_minimum_required(VERSION 3.24)  # CMAKE_FIND_PACKAGE_REDIRECTS_DIR (freetype for RmlUi)

if(NOT UNIX AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    message(FATAL_ERROR "ogsr_engine/CMakeLists.txt is the Linux port build; use the .sln on Windows.")
endif()

project(ogsr_engine_linux CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OGSR_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(OGSR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(OGSR_3RD_PARTY_DIR ${OGSR_ROOT_DIR}/3rd_party/Src)

set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# xrCore/xrMemory.cpp defines the global operator new/delete itself (on top of mi_malloc);
# mimalloc's own override would define them a second time (multiple definition at link).
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
if(ANDROID)
    # The engine is a dlopen()ed libmain.so there: bionic refuses initial-exec TLS in it ("TLS symbol ... using IE
    # access model"), and mimalloc's per-thread heap pointer is initial-exec by default.
    set(MI_LOCAL_DYNAMIC_TLS ON CACHE BOOL "" FORCE)
endif()
if(OGSR_SWITCH)
    # Whole-program build options for the NRO (from the nfsmw-nx port: LTO + PGO + hot-function order ~ +12 % fps there).
    # All OFF by default: a plain configure builds exactly as before. They go before every add_subdirectory so the
    # third-party libraries get them too.
    option(NX_LTO "Link-time optimization of the whole NRO" OFF)
    option(NX_PGO_GEN "Instrument for PGO: the console writes .gcda files to sdmc:/ogsr_pgo every 180 s" OFF)
    set(NX_PGO_USE "" CACHE PATH "Directory with the .gcda files copied back from sdmc:/ogsr_pgo")
    set(NX_SECTION_ORDER "" CACHE FILEPATH "ld --section-ordering-file (hot functions first; starts with KEEP(*(.crt0)))")
    option(NX_FRAME_POINTERS "Keep x29 frame records so nx_prof.txt stack samples walk past the first frame" OFF)
    if(NX_LTO)
        add_compile_options(-flto=2 -fno-fat-lto-objects)
        add_link_options(-flto=2 -flto-partition=balanced -ffunction-sections)
        # xxHash (zstd, lz4) picks the type-punning read mode for GCC; inlined across units by LTO+PGO it read keys
        # before they were written in the nfsmw-nx port. Use the memcpy mode.
        add_compile_definitions(XXH_FORCE_MEMORY_ACCESS=0)
    endif()
    if(NX_PGO_GEN)
        # -fprofile-update=single and no value profiling: the atomic/TLS counter variants crash before main on Horizon
        # (tpidr_el0 is 0). The gcda path keeps only the part below the build dir, so NX_PGO_USE can find them.
        add_compile_options(-fprofile-generate=sdmc:/ogsr_pgo -fprofile-update=single -fno-profile-values
                            -fprofile-prefix-path=${CMAKE_BINARY_DIR})
        add_link_options(-fprofile-generate=sdmc:/ogsr_pgo)
        add_compile_definitions(NX_PGO_GEN=1)
    elseif(NX_PGO_USE)
        add_compile_options(-fprofile-use=${NX_PGO_USE} -fprofile-partial-training -fprofile-correction
                            -Wno-missing-profile -fprofile-prefix-path=${CMAKE_BINARY_DIR})
        add_link_options(-fprofile-use=${NX_PGO_USE})
    endif()
    if(NX_FRAME_POINTERS)
        add_compile_options(-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
    endif()
    # libc gaps of newlib/libnx (pread, sysconf, ...): see cmake/nx_compat/nx_libc_shims.c
    add_library(xr_nx_libc STATIC ${CMAKE_CURRENT_SOURCE_DIR}/cmake/nx_compat/nx_libc_shims.c)
endif()
if(NOT OGSR_SWITCH) # Horizon has no mmap: see xrCore/xrMemory.cpp
    add_subdirectory(${OGSR_3RD_PARTY_DIR}/mimalloc/mimalloc ${CMAKE_BINARY_DIR}/mimalloc)
endif()
if(ANDROID)
    # Android (NDK, arm64-v8a; configure with the NDK's android.toolchain.cmake, see cmake/android/build.sh).
    # The distro packages the Linux build takes (SDL2, OpenAL) don't exist here: build both from their
    # release sources. SDL2 stays a shared library -- SDLActivity loads libSDL2.so before libmain.so
    # (the engine), and its JNI entry points must not be dropped by a static link.
    # Headers carry `#pragma comment(lib, "Luabind.lib")` (MSVC); clang turns it into ELF .deplibs and lld fails on
    # "Luabind.lib not found" -- same as cmake/toolchain-aarch64-linux-clang.cmake.
    add_link_options(-Wl,--no-dependent-libraries -Wl,--error-limit=0)
    set(OGSR_SDL2_DIR "" CACHE PATH "SDL2 release source tree (2.30.x)")
    set(OGSR_OPENAL_DIR "" CACHE PATH "OpenAL Soft release source tree (1.24.x)")
    if(NOT EXISTS "${OGSR_SDL2_DIR}/CMakeLists.txt" OR NOT EXISTS "${OGSR_OPENAL_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Android: set OGSR_SDL2_DIR and OGSR_OPENAL_DIR to the SDL2 / OpenAL Soft source trees.")
    endif()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${OGSR_SDL2_DIR} ${CMAKE_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)
    # OpenAL Soft: static, OpenSL ES backend (Oboe would need its own source tree), no tools.
    set(LIBTYPE STATIC)
    foreach(al_opt ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_INSTALL_CONFIG ALSOFT_INSTALL_HRTF_DATA
                   ALSOFT_INSTALL_AMBDEC_PRESETS ALSOFT_INSTALL_EXAMPLES ALSOFT_INSTALL_UTILS ALSOFT_BACKEND_OBOE)
        set(${al_opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(ALSOFT_REQUIRE_OPENSL ON CACHE BOOL "" FORCE)
    add_subdirectory(${OGSR_OPENAL_DIR} ${CMAKE_BINARY_DIR}/openal EXCLUDE_FROM_ALL)
endif()
add_subdirectory(${OGSR_3RD_PARTY_DIR}/Xiph/libogg ${CMAKE_BINARY_DIR}/libogg)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/Xiph/libvorbis ${CMAKE_BINARY_DIR}/libvorbis)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/Xiph/libtheora ${CMAKE_BINARY_DIR}/libtheora)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/zstd ${CMAKE_BINARY_DIR}/zstd)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/lz4 ${CMAKE_BINARY_DIR}/lz4)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/lzo ${CMAKE_BINARY_DIR}/lzo)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/libsquashfs ${CMAKE_BINARY_DIR}/libsquashfs)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/ode ${CMAKE_BINARY_DIR}/ode)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/imgui ${CMAKE_BINARY_DIR}/imgui)

# freetype + RmlUi through their own upstream CMake, with the options the Windows build used
# (3rd_party/Src/{freetype,RmlUi}/build/CMakeCache.txt): static, no optional codecs, core only.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(ft_opt FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI)
    set(${ft_opt} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${OGSR_3RD_PARTY_DIR}/freetype ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
# RmlUi does find_package(Freetype). Point it at the vendored target instead of the system
# library (CMake >= 3.24 checks the redirects dir before any Find module).
add_library(Freetype::Freetype ALIAS freetype)
file(WRITE ${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/freetype-config.cmake "set(Freetype_FOUND TRUE)\nset(FREETYPE_FOUND TRUE)\n")
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
add_subdirectory(${OGSR_3RD_PARTY_DIR}/RmlUi ${CMAKE_BINARY_DIR}/RmlUi EXCLUDE_FROM_ALL)

# Online play (GameNetworkingSockets) is optional. The game code never touches the library: it
# goes through the null-by-default hook table in xrOnlineNet/NET_GNS_Transport.h, so a build
# without it is a stock single-player engine. Off by default on arm64 (Switch: no online play,
# and GNS drags in protobuf + ~60 abseil .so files); ON elsewhere. -DOGSR_WITH_GNS=ON/OFF overrides.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_ogsr_gns_default OFF)
else()
    set(_ogsr_gns_default ON)
endif()
option(OGSR_WITH_GNS "Build and link GameNetworkingSockets (online play)" ${_ogsr_gns_default})

if(OGSR_WITH_GNS)
    # GameNetworkingSockets: the vendored source (3rd_party/GameNetworkingSockets, gitignored —
    # on Windows it is built by hand against vcpkg) with the same feature set as the Windows build
    # (build_static/CMakeCache.txt: static, no ICE, reference ed25519). Differences: OpenSSL instead
    # of BCrypt for AES/SHA (BCrypt is Windows-only), protobuf from the distro (Arch: `protobuf`).
    set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
    set(USE_CRYPTO "OpenSSL" CACHE STRING "" FORCE)
    set(USE_CRYPTO25519 "Reference" CACHE STRING "" FORCE)
    add_subdirectory(${OGSR_ROOT_DIR}/3rd_party/GameNetworkingSockets ${CMAKE_BINARY_DIR}/GameNetworkingSockets EXCLUDE_FROM_ALL)
endif()

# DLSS/NGX: NVIDIA ships a Linux static lib for x86_64 only. Elsewhere (arm64/Switch) the
# DLSS path has to report "unavailable" at compile time instead.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(OGSR_NGX_LIBRARY ${OGSR_3RD_PARTY_DIR}/NVIDIA_DLSS/DLSS/lib/Linux_x86_64/libnvsdk_ngx.a)
endif()

add_subdirectory(xrCore)
add_subdirectory(xrCDB)
add_subdirectory(xrParticles)
add_subdirectory(xrSound)
add_subdirectory(LuaJIT/src ${CMAKE_BINARY_DIR}/LuaJIT)
add_subdirectory(Luabind)
add_subdirectory(Layers/xrRenderVulkan)
add_subdirectory(xrGame)
add_subdirectory(xr_3da)
