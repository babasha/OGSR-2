# Linux build of libvorbis + libvorbisfile for the OGSR engine's Linux port (see
# ogsr_engine/xrSound). Source lists mirror win32/VS2010/{libvorbis,libvorbisfile}/*.vcxproj
# (the Windows build OGSR ships); this is the vendored upstream source, not fetched from
# anywhere else.
add_library(vorbis STATIC
    lib/analysis.c
    lib/bitrate.c
    lib/block.c
    lib/codebook.c
    lib/envelope.c
    lib/floor0.c
    lib/floor1.c
    lib/info.c
    lib/lookup.c
    lib/lpc.c
    lib/lsp.c
    lib/mapping0.c
    lib/mdct.c
    lib/psy.c
    lib/registry.c
    lib/res0.c
    lib/sharedbook.c
    lib/smallft.c
    lib/synthesis.c
    lib/vorbisenc.c
    lib/window.c
)
target_include_directories(vorbis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(vorbis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
# floor1.c/psy.c/sharedbook.c use __cdecl (an MSVC calling-convention keyword) on their local
# qsort comparators. These are plain C files with no dependency on xrCore's own
# xr_platform_win32_shim.h (which already #defines __cdecl away for other modules), so define
# it away here directly instead of dragging engine headers into a vendored third-party lib.
target_compile_definitions(vorbis PRIVATE __cdecl=)
target_link_libraries(vorbis PUBLIC ogg)

add_library(vorbisfile STATIC
    lib/vorbisfile.c
)
target_include_directories(vorbisfile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vorbisfile PUBLIC vorbis)
