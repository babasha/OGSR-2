// newlib (devkitA64) has BSD's <sys/endian.h> but no glibc-style <endian.h>. libsquashfs's compat.h
// includes <endian.h> for htole*/le*toh on anything that is not BSD/macOS; this forwards it.
// Only put on the include path for the Switch build (see libsquashfs/CMakeLists.txt).
#pragma once
#include <sys/endian.h>
