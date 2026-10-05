/**
 * File: crc32.h
 * Description: CRC-32 as zlib, gzip and PNG compute it (reflected,
 *              polynomial 0xEDB88320), to check a file read on the RP
 *              against the same file on a PC (`crc32`, Python's
 *              zlib.crc32()).
 */

#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

// Continues `crc` (0 to start) over `len` bytes; the result of the last
// call is the CRC-32 of everything passed, as zlib's crc32().
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif  // CRC32_H
