# Linux build of LuaJIT for the OGSR engine's Linux port (see ogsr_engine/Luabind).
#
# There is no vendored Makefile for this (see ogsr_engine/LuaJIT/src/luajit.vcxproj: it's a
# Makefile-type MSBuild project that just shells out to msvcbuild.bat — LuaJIT's own upstream
# src/Makefile was deleted from this repo in commit a3cb3ba3f "LuaJIT 2.1" and never restored).
# This CMakeLists.txt reimplements the relevant parts of LuaJIT's own POSIX Makefile: bootstrap
# two host tools (minilua, buildvm), use them to generate the VM assembly + auto-generated
# headers, then compile everything into a static lib. See msvcbuild.bat for the Windows
# equivalent of every step below.
#
# DynASM/buildvm flags below are for x86-64 Linux specifically, derived by preprocessing this
# exact lj_arch.h and reading its output (not guessed, not copied from a generic reference):
#   gcc -U_FORTIFY_SOURCE -E lj_arch.h -dM | grep -E 'LJ_(ARCH_BITS|HASJIT|HASFFI|DUALNUM|ARCH_HASFPU|ABI_SOFTFP|NO_UNWIND|FR2|GC64)'
# gives: ARCH_BITS 64, HASJIT 1, HASFFI 1, DUALNUM 0, ARCH_HASFPU 1, ABI_SOFTFP 0, NO_UNWIND
# undefined, FR2 1, GC64 1. LJ_FR2=1 (the default on x64 unless LUAJIT_DISABLE_GC64 is defined)
# is why this targets vm_x64.dasc, NOT vm_x86.dasc — vm_x86.dasc is the older, GC64-unaware
# file (see its `.if P64` branch and the total absence of "GC64"/"FR2" in it); using it here
# would silently generate VM assembly with the wrong (pre-GC64) frame/reference layout against
# C code compiled for GC64, an ABI mismatch that compiles fine and corrupts memory at runtime.
set(LJ_DASC vm_x64.dasc)
set(LJ_DASM_FLAGS -D P64 -D JIT -D FFI -D FPU -D HFABI)
set(LJ_TARGET_ARCH -DLJ_ARCH_HASFPU=1 -DLJ_ABI_SOFTFP=0 -DLUAJIT_TARGET=LUAJIT_ARCH_X64)

# arm64 (Linux rehearsal for the Switch). Derived the same way, from lj_arch.h under
#   clang --target=aarch64-linux-gnu -U_FORTIFY_SOURCE -E lj_arch.h -dM
# which gives: ARCH_BITS 64, HASJIT 1, HASFFI 1, DUALNUM 1 (!= x64: ints live as int32),
# ARCH_HASFPU 1, ABI_SOFTFP 0, FR2 1, GC64 1, LE. The endianness goes to DynASM as ENDIAN_LE
# (vm_arm64.dasc tests it), as in LuaJIT's own Makefile.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(LJ_DASC vm_arm64.dasc)
    set(LJ_DASM_FLAGS -D ENDIAN_LE -D P64 -D JIT -D FFI -D DUALNUM -D FPU -D HFABI)
    set(LJ_TARGET_ARCH -DLJ_ARCH_HASFPU=1 -DLJ_ABI_SOFTFP=0 -DLJ_ARCH_ENDIAN=LUAJIT_LE -DLUAJIT_TARGET=LUAJIT_ARCH_ARM64)
endif()

# Horizon (Switch homebrew): interpreter only. Measured 18-09: LuaJIT's JIT bought +0.6% frame time on
# this engine's scripts (pairs()/closures don't compile, luabind calls cost 110ns), and running a JIT
# needs RW/RX dual mappings + a +-128MB branch range that libnx does not give us. The interpreter is
# static assembly, so no executable memory is needed at all.
#  - DISABLE_FFI: nothing in the engine or scripts requires "ffi"; also drops lj_ccallback (exec memory).
#  - USE_SYSMALLOC: newlib has no mmap, lj_alloc.c would refuse to build (legal only because GC64).
# The DynASM flags and buildvm's view of the target must agree with the library build below.
if(OGSR_SWITCH)
    set(LJ_DASM_FLAGS -D ENDIAN_LE -D P64 -D DUALNUM -D FPU -D HFABI)
    list(APPEND LJ_TARGET_ARCH -DLUAJIT_DISABLE_JIT -DLUAJIT_DISABLE_FFI -DLUAJIT_USE_SYSMALLOC
        -DLUAJIT_SECURITY_PRNG=0) # lj_prng.c has no entropy source for Horizon; a time-seeded PRNG is fine for a game
endif()

set(LJ_SRC ${CMAKE_CURRENT_SOURCE_DIR})
set(LJ_GEN ${CMAKE_CURRENT_BINARY_DIR})

if(CMAKE_CROSSCOMPILING)
    # minilua/buildvm run on THIS machine: build them with the host compiler in a nested project.
    include(ExternalProject)
    find_program(LJ_HOST_CC NAMES gcc cc clang NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(LJ_HOST_DIR ${LJ_GEN}/hosttools)
    set(LJ_MINILUA_EXE ${LJ_HOST_DIR}/lj_minilua)
    set(LJ_BUILDVM_EXE ${LJ_HOST_DIR}/lj_buildvm)
    # Lists cross the ExternalProject boundary as '|' (LIST_SEPARATOR turns it back into ';').
    string(REPLACE ";" "|" LJ_DASM_FLAGS_ARG "${LJ_DASM_FLAGS}")
    string(REPLACE ";" "|" LJ_TARGET_ARCH_ARG "${LJ_TARGET_ARCH}")
    ExternalProject_Add(lj_host_tools
        SOURCE_DIR ${LJ_SRC}/hosttools
        BINARY_DIR ${LJ_HOST_DIR}
        LIST_SEPARATOR |
        CMAKE_ARGS
            -DCMAKE_C_COMPILER=${LJ_HOST_CC}
            -DCMAKE_BUILD_TYPE=Release
            -DLJ_SRC=${LJ_SRC}
            -DLJ_DASC=${LJ_DASC}
            "-DLJ_DASM_FLAGS=${LJ_DASM_FLAGS_ARG}"
            "-DLJ_TARGET_ARCH=${LJ_TARGET_ARCH_ARG}"
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${LJ_MINILUA_EXE} ${LJ_BUILDVM_EXE}
        BUILD_ALWAYS OFF
    )
    set(LJ_BUILDVM_TARGET lj_host_tools)
else()
    # host/minilua: a self-contained, cut-down Lua 5.1 interpreter used only to run dynasm.lua.
    add_executable(lj_minilua host/minilua.c)
    target_link_libraries(lj_minilua PRIVATE m)

    # host/buildvm_arch.h: dynasm.lua's expansion of the .dasc's low-level VM assembly templates.
    set(LJ_BUILDVM_ARCH_H ${LJ_GEN}/host/buildvm_arch.h)
    add_custom_command(
        OUTPUT ${LJ_BUILDVM_ARCH_H}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${LJ_GEN}/host
        COMMAND $<TARGET_FILE:lj_minilua> ${LJ_SRC}/../dynasm/dynasm.lua ${LJ_DASM_FLAGS} -o ${LJ_BUILDVM_ARCH_H} ${LJ_DASC}
        WORKING_DIRECTORY ${LJ_SRC}
        DEPENDS lj_minilua ${LJ_SRC}/${LJ_DASC} ${LJ_SRC}/lj_arch.h ${LJ_SRC}/lua.h ${LJ_SRC}/luaconf.h
        COMMENT "LuaJIT: dynasm ${LJ_DASC} -> buildvm_arch.h"
    )

    # host/buildvm: the tool that turns the .dasc-derived template plus the library sources into
    # lj_vm.s and the lj_*def.h headers below. Needs top-level lj_*.h (bare "lj_obj.h" etc includes)
    # and buildvm_arch.h (bare "buildvm_arch.h" include from host/buildvm.c); dasm_x86.h etc are
    # included by buildvm.c via explicit "../dynasm/..." paths, no extra include dir needed for those.
    add_executable(lj_buildvm
        host/buildvm.c
        host/buildvm_asm.c
        host/buildvm_peobj.c
        host/buildvm_lib.c
        host/buildvm_fold.c
        ${LJ_BUILDVM_ARCH_H}
    )
    target_include_directories(lj_buildvm PRIVATE ${LJ_SRC} ${LJ_GEN}/host)
    set(LJ_BUILDVM_EXE $<TARGET_FILE:lj_buildvm>)
    set(LJ_BUILDVM_TARGET lj_buildvm)
endif()

# The 12 files LuaJIT's own build (msvcbuild.bat's ALL_LIB, matched here) scans for LJLIB_CF/
# LJLIB_ASM markers to generate the bytecode/FFI/library/recorder defs from. Deliberately NOT
# lib_aux.c/lib_init.c (infra, no markers) and NOT a `lib_*.c` glob, matching msvcbuild.bat.
set(LJ_LIBDEF_C
    lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c
    lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c lib_buffer.c
)

set(LJ_VM_S ${LJ_GEN}/lj_vm.s)
add_custom_command(OUTPUT ${LJ_VM_S}
    COMMAND ${LJ_BUILDVM_EXE} -m elfasm -o ${LJ_VM_S}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS ${LJ_BUILDVM_TARGET}
    COMMENT "LuaJIT: buildvm -m elfasm -> lj_vm.s")

set(LJ_BCDEF_H ${LJ_GEN}/lj_bcdef.h)
add_custom_command(OUTPUT ${LJ_BCDEF_H}
    COMMAND ${LJ_BUILDVM_EXE} -m bcdef -o ${LJ_BCDEF_H} ${LJ_LIBDEF_C}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS ${LJ_BUILDVM_TARGET}
    COMMENT "LuaJIT: buildvm -m bcdef -> lj_bcdef.h")

set(LJ_FFDEF_H ${LJ_GEN}/lj_ffdef.h)
add_custom_command(OUTPUT ${LJ_FFDEF_H}
    COMMAND ${LJ_BUILDVM_EXE} -m ffdef -o ${LJ_FFDEF_H} ${LJ_LIBDEF_C}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS ${LJ_BUILDVM_TARGET}
    COMMENT "LuaJIT: buildvm -m ffdef -> lj_ffdef.h")

set(LJ_LIBDEF_H ${LJ_GEN}/lj_libdef.h)
add_custom_command(OUTPUT ${LJ_LIBDEF_H}
    COMMAND ${LJ_BUILDVM_EXE} -m libdef -o ${LJ_LIBDEF_H} ${LJ_LIBDEF_C}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS ${LJ_BUILDVM_TARGET}
    COMMENT "LuaJIT: buildvm -m libdef -> lj_libdef.h")

set(LJ_RECDEF_H ${LJ_GEN}/lj_recdef.h)
add_custom_command(OUTPUT ${LJ_RECDEF_H}
    COMMAND ${LJ_BUILDVM_EXE} -m recdef -o ${LJ_RECDEF_H} ${LJ_LIBDEF_C}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS ${LJ_BUILDVM_TARGET}
    COMMENT "LuaJIT: buildvm -m recdef -> lj_recdef.h")

set(LJ_FOLDDEF_H ${LJ_GEN}/lj_folddef.h)
add_custom_command(OUTPUT ${LJ_FOLDDEF_H}
    COMMAND ${LJ_BUILDVM_EXE} -m folddef -o ${LJ_FOLDDEF_H} lj_opt_fold.c
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS ${LJ_BUILDVM_TARGET} ${LJ_SRC}/lj_opt_fold.c
    COMMENT "LuaJIT: buildvm -m folddef -> lj_folddef.h")

add_custom_target(lj_headers DEPENDS
    ${LJ_VM_S} ${LJ_BCDEF_H} ${LJ_FFDEF_H} ${LJ_LIBDEF_H} ${LJ_RECDEF_H} ${LJ_FOLDDEF_H}
)

# Static lib: every lj_*.c/lib_*.c in the tree (including lib_aux.c/lib_init.c, excluded above
# only from the buildvm def-scanning step) plus the generated VM assembly, matching
# msvcbuild.bat's static-mode wildcard `lj_*.c lib_*.c` with no curated list.
file(GLOB LJ_CORE_C ${LJ_SRC}/lj_*.c)
file(GLOB LJ_LIB_C ${LJ_SRC}/lib_*.c)

enable_language(ASM)
add_library(luajit STATIC ${LJ_VM_S} ${LJ_CORE_C} ${LJ_LIB_C})
add_dependencies(luajit lj_headers)
if(OGSR_SWITCH)
    target_sources(luajit PRIVATE ${LJ_SRC}/nx_profile_stub.c)
endif()
target_include_directories(luajit PUBLIC ${LJ_SRC})
target_include_directories(luajit PRIVATE ${LJ_GEN})
# lj_err.c's own LJ_UNWIND_EXT logic (this fork's version, not stock upstream's — see the
# comment above lj_err.c's #define LJ_UNWIND_EXT) requires this to be defined explicitly on
# GCC/clang, with matching -funwind-tables so the C frames LuaJIT walks during error unwinding
# actually have unwind info; omitting either hits lj_err.c's own
# "#error Broken build system -- only use the provided Makefiles!" for x64.
target_compile_definitions(luajit PRIVATE ${LJ_TARGET_ARCH} LUAJIT_UNWIND_EXTERNAL)
target_compile_options(luajit PRIVATE -funwind-tables)
if(OGSR_SWITCH)
    target_link_libraries(luajit PRIVATE m)   # no libdl on Horizon
else()
    target_link_libraries(luajit PRIVATE m dl)
endif()
