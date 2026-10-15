# Linux build of the vendored libtheora. Source list copied from
# win32/VS2010/libtheora_static.vcxproj — keep in sync — minus lib/x86_vc/*: that is MSVC
# inline asm (__asm blocks), and the vcxproj doesn't define OC_X86_ASM anyway, so those files
# compile to nothing there too. Portable C only (also what arm64 gets).
cmake_minimum_required(VERSION 3.18)
project(libtheora C)

set(T ${CMAKE_CURRENT_SOURCE_DIR}/lib)
add_library(theora STATIC
    ${T}/analyze.c ${T}/apiwrapper.c ${T}/bitpack.c ${T}/decapiwrapper.c ${T}/decinfo.c
    ${T}/decode.c ${T}/dequant.c ${T}/encapiwrapper.c ${T}/encfrag.c ${T}/encinfo.c
    ${T}/encode.c ${T}/enquant.c ${T}/fdct.c ${T}/fragment.c ${T}/huffdec.c ${T}/huffenc.c
    ${T}/idct.c ${T}/info.c ${T}/internal.c ${T}/mathops.c ${T}/mcenc.c ${T}/quant.c
    ${T}/rate.c ${T}/state.c ${T}/tokenize.c
)
target_include_directories(theora PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# huffenc.c declares its qsort comparator __cdecl (MSVC keyword) — same fix as libvorbis.
target_compile_definitions(theora PRIVATE NDEBUG __cdecl=)
target_link_libraries(theora PUBLIC ogg)
