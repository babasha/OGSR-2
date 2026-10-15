# Host-side LuaJIT build tools (minilua + buildvm), used only when the engine is cross-compiled.
# They RUN on the build machine, so they must be built with the host compiler — but buildvm has
# to see the TARGET architecture's layout, hence LJ_TARGET_ARCH (-DLUAJIT_TARGET=..., ...) and the
# target's DynASM source/flags. Driven from ../CMakeLists.txt through ExternalProject; the
# parameters come in as -D options (lists arrive ';'-separated).
cmake_minimum_required(VERSION 3.20)
project(lj_hosttools C)

foreach(v LJ_SRC LJ_DASC LJ_DASM_FLAGS LJ_TARGET_ARCH)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "hosttools: ${v} not set")
    endif()
endforeach()

add_executable(lj_minilua ${LJ_SRC}/host/minilua.c)
target_link_libraries(lj_minilua PRIVATE m)

set(ARCH_H ${CMAKE_CURRENT_BINARY_DIR}/host/buildvm_arch.h)
add_custom_command(
    OUTPUT ${ARCH_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/host
    COMMAND $<TARGET_FILE:lj_minilua> ${LJ_SRC}/../dynasm/dynasm.lua ${LJ_DASM_FLAGS} -o ${ARCH_H} ${LJ_DASC}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS lj_minilua ${LJ_SRC}/${LJ_DASC} ${LJ_SRC}/lj_arch.h ${LJ_SRC}/lua.h ${LJ_SRC}/luaconf.h
)

add_executable(lj_buildvm
    ${LJ_SRC}/host/buildvm.c
    ${LJ_SRC}/host/buildvm_asm.c
    ${LJ_SRC}/host/buildvm_peobj.c
    ${LJ_SRC}/host/buildvm_lib.c
    ${LJ_SRC}/host/buildvm_fold.c
    ${ARCH_H}
)
target_include_directories(lj_buildvm PRIVATE ${LJ_SRC} ${CMAKE_CURRENT_BINARY_DIR}/host)
target_compile_definitions(lj_buildvm PRIVATE ${LJ_TARGET_ARCH})
