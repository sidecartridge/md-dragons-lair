/**
 * File: crc32.c
 * Description: CRC-32 (zlib's), one table lookup a byte. See crc32.h.
 */

#include "crc32.h"

#include <stdbool.h>

// Built on first use: 1 KB of RAM instead of flash, which the RP2040 reads
// through its XIP cache.
static uint32_t crc_table[256];
static bool crc_table_ready;

static void crc32_build_table(void) {
  for (uint32_t n = 0; n < 256u; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    crc_table[n] = c;
  }
  crc_table_ready = true;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
  if (!crc_table_ready) {
    crc32_build_table();
  }
  const uint8_t *p = (const uint8_t *)data;
  uint32_t c = crc ^ 0xFFFFFFFFu;
  while (len--) {
    c = crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}
