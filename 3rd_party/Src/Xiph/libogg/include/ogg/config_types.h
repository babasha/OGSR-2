#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__

/* Linux/gcc/clang: libogg's ./configure always finds stdint.h on this
   platform and generates exactly this file -- checked in here because the
   Windows-only vendoring of libogg never ran configure, so the file never
   existed (os_types.h's own #if _WIN32 branch covers MSVC directly instead
   and never reaches this #include, see os_types.h line ~27). */
#include <stdint.h>

typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;

#endif
