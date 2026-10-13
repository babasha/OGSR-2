# Linux build of libogg for the OGSR engine's Linux port (see ogsr_engine/xrSound). Source
# list mirrors win32/VS2010/libogg_static.vcxproj (the Windows build OGSR ships); this is the
# vendored upstream source, not fetched from anywhere else.
add_library(ogg STATIC
    src/bitwise.c
    src/framing.c
)

target_include_directories(ogg PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
