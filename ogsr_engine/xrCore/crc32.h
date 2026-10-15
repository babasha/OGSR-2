// //////////////////////////////////////////////////////////
// Crc32.h
// Copyright (c) 2011-2019 Stephan Brumme. All rights reserved.
// Slicing-by-16 contributed by Bulat Ziganshin
// Tableless bytewise CRC contributed by Hagai Gold
// see http://create.stephan-brumme.com/disclaimer.html
//

// Crc32Lookup is sized by these (crc32_16bytes, the only variant kept, needs all of them):
#define CRC32_USE_LOOKUP_TABLE_BYTE
#define CRC32_USE_LOOKUP_TABLE_SLICING_BY_4
#define CRC32_USE_LOOKUP_TABLE_SLICING_BY_8
#define CRC32_USE_LOOKUP_TABLE_SLICING_BY_16

// uint8_t, uint32_t, int32_t
#include <stdint.h>
// size_t
#include <cstddef>

/// compute CRC32 using the fastest algorithm for large datasets on modern CPUs
uint32_t crc32_fast(const void* data, size_t length, uint32_t previousCrc32 = 0);

#ifdef CRC32_USE_LOOKUP_TABLE_SLICING_BY_16
/// compute CRC32 (Slicing-by-16 algorithm)
uint32_t crc32_16bytes(const void* data, size_t length, uint32_t previousCrc32 = 0);
#endif