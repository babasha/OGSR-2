# Linux build of Luabind. Source list is generated from Luabind.vcxproj — see
# CMakeSources.cmake and cmake/gen_module_sources.sh. Keep in sync with the Windows project;
# do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

add_library(Luabind STATIC ${LUABIND_SOURCES})

target_include_directories(Luabind PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(Luabind PUBLIC
    LUABIND_STATIC
)
target_compile_definitions(Luabind PRIVATE
    NDEBUG
)
# _WINDOWS/_USRDLL are in the vcxproj but unused anywhere in Luabind itself (no grep hits),
# same call as xrCore/xrCDB/xrParticles/xrSound: left undefined for the Linux target.

target_link_libraries(Luabind PUBLIC
    xrCore
    luajit
)
