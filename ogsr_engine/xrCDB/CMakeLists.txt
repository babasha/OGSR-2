# Linux build of xrCDB. Source list is generated from xrCDB.vcxproj — see CMakeSources.cmake
# and cmake/gen_module_sources.sh. Keep in sync with the Windows project; do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

add_library(xrCDB STATIC ${XRCDB_SOURCES})

target_include_directories(xrCDB PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(xrCDB PUBLIC
    XRCDB_STATIC
)
target_compile_definitions(xrCDB PRIVATE
    NDEBUG
    ICE_NO_DLL
    OPCODE_EXPORTS
)
# _WINDOWS/_USRDLL are in the vcxproj (MSVC subsystem/DLL-project markers); nothing in xrCDB
# or the vendored OPCODE/ICE sources needs them on Linux (OPCODE checks _WIN32, not _WINDOWS —
# see OpCoDe/Ice/IcePreprocessor.h), so left undefined here, same call as xrCore.

target_link_libraries(xrCDB PUBLIC
    xrCore
)
