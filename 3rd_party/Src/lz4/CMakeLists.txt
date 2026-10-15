# Linux build of the vendored lz4. Source list copied from liblz4.vcxproj — keep in sync.
cmake_minimum_required(VERSION 3.18)
project(lz4 C)

set(L ${CMAKE_CURRENT_SOURCE_DIR}/lz4/lib)
add_library(lz4 STATIC ${L}/lz4.c ${L}/lz4frame.c ${L}/lz4hc.c ${L}/xxhash.c)
target_include_directories(lz4 PUBLIC ${L})
# LZ4_DLL_EXPORT from the vcxproj only selects __declspec(dllexport); meaningless for a static lib.
target_compile_definitions(lz4 PRIVATE NDEBUG)
