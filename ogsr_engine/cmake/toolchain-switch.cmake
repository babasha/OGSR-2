# Nintendo Switch (homebrew): devkitA64 + libnx, via devkitPro's own Switch.cmake.
# Target = Horizon OS, newlib libc (NOT glibc): no mmap, dlfcn, epoll, /proc. Everything the
# engine needs from the OS goes through the platform shim (xrCore/xr_platform_*).
#
#   export DEVKITPRO=/opt/devkitpro
#   cmake -S ogsr_engine -B build_nx -G Ninja -DCMAKE_TOOLCHAIN_FILE=ogsr_engine/cmake/toolchain-switch.cmake
if(NOT DEFINED ENV{DEVKITPRO})
    set(ENV{DEVKITPRO} /opt/devkitpro)
endif()
include($ENV{DEVKITPRO}/cmake/Switch.cmake)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(OGSR_SWITCH ON CACHE BOOL "Building for Horizon (Nintendo Switch homebrew)" FORCE)
