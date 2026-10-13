# Cross toolchain: clang++ -> aarch64 Linux (glibc). A rehearsal target for the Nintendo Switch
# port: same architecture (A57 / ARMv8-A), a real debugger and libc, none of the newlib limits.
# What it flushes out: MSVC-isms, SEH, SSE intrinsics, unaligned access, strict aliasing, x86-only libs.
#
# The sysroot is not shipped: unpack Arch Linux ARM packages (glibc, linux-api-headers, gcc,
# libgcc, libstdc++, libunwind) into it. Point OGSR_A64_SYSROOT at it, and OGSR_A64_GCC_VERSION
# at the libstdc++ version inside (usr/lib/gcc/aarch64-unknown-linux-gnu/<ver>).
#
#   cmake -S ogsr_engine -B build_a64 -DCMAKE_TOOLCHAIN_FILE=ogsr_engine/cmake/toolchain-aarch64-linux-clang.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{OGSR_A64_SYSROOT})
    set(_ogsr_a64_sysroot "$ENV{OGSR_A64_SYSROOT}")
else()
    set(_ogsr_a64_sysroot "/root/sysroot-a64")
endif()
if(DEFINED ENV{OGSR_A64_GCC_VERSION})
    set(_ogsr_a64_gcc_ver "$ENV{OGSR_A64_GCC_VERSION}")
else()
    set(_ogsr_a64_gcc_ver "16.1.1")
endif()

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
# The ASM language has its own compiler/target variables: without these the generated
# LuaJIT VM (lj_vm.s) is assembled as x86 ("invalid instruction mnemonic 'bl'").
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_ASM_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_SYSROOT "${_ogsr_a64_sysroot}")

set(_ogsr_a64_gcc_dir "${_ogsr_a64_sysroot}/usr/lib/gcc/aarch64-unknown-linux-gnu/${_ogsr_a64_gcc_ver}")
set(_ogsr_a64_flags "--gcc-install-dir=${_ogsr_a64_gcc_dir}")
set(CMAKE_C_FLAGS_INIT "${_ogsr_a64_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_ogsr_a64_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_ogsr_a64_flags}")
# --no-dependent-libraries: headers carry `#pragma comment(lib, "Luabind.lib")` (MSVC). Clang turns it
# into ELF .deplibs and lld, unlike GNU ld, treats "Luabind.lib not found" as an ERROR.
set(_ogsr_a64_ld "-fuse-ld=lld -Wl,--no-dependent-libraries -Wl,--error-limit=0")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ogsr_a64_ld}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ogsr_a64_ld}")

# Libraries and headers come from the sysroot only; build tools (protoc, ...) from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
