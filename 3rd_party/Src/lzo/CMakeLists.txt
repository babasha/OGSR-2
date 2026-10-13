# Linux build of the vendored lzo. Sources = every .c listed in lzo.vcxproj (all .c in this
# directory) — keep in sync.
cmake_minimum_required(VERSION 3.18)
project(lzo C)

file(GLOB LZO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_library(lzo STATIC ${LZO_SOURCES})
# Consumers include "lzo/lzo1x.h" (headers live in ./lzo/).
target_include_directories(lzo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Do NOT define _WINDOWS: lzodefs.h would treat the target as Windows (LLP64) and trip its
# ULONG_MAX check on LP64 Linux — see ogsr_engine/xrCore/CMakeLists.txt.
target_compile_definitions(lzo PRIVATE NDEBUG)
