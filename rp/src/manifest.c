/**
 * File: manifest.c
 * Description: A set's manifest. See manifest.h.
 */

#include "manifest.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
  return get16(p) | ((uint32_t)get16(p + 2) << 16);
}

// Copies `name` into an entry's name, cut to leave its NUL.
static void put_name(char out[MANIFEST_NAME_BYTES], const char *name) {
  for (unsigned i = 0; i + 1u < MANIFEST_NAME_BYTES && name[i] != '\0'; i++) {
    out[i] = name[i];
  }
}

void manifest_header_write(const manifest_header_t *h,
                           uint8_t out[MANIFEST_HEADER_BYTES]) {
  memset(out, 0, MANIFEST_HEADER_BYTES);
  memcpy(out, MANIFEST_MAGIC, 4);
  put16(out + 4, MANIFEST_VERSION);
  put16(out + 6, MANIFEST_HEADER_BYTES);
  put16(out + 8, MANIFEST_ENTRY_BYTES);
  put16(out + 10, h->count);
  out[12] = h->gun_bits;
  put16(out + 14, h->converter);
  put16(out + 16, h->keep_percent);
  put32(out + 20, h->entries_crc);
}

int manifest_header_read(manifest_header_t *h,
                         const uint8_t in[MANIFEST_HEADER_BYTES]) {
  if (memcmp(in, MANIFEST_MAGIC, 4) != 0) {
    return -1;
  }
  if (get16(in + 4) != MANIFEST_VERSION ||
      get16(in + 6) != MANIFEST_HEADER_BYTES ||
      get16(in + 8) != MANIFEST_ENTRY_BYTES) {
    return -2;
  }
  h->count = get16(in + 10);
  h->gun_bits = in[12];
  h->converter = get16(in + 14);
  h->keep_percent = get16(in + 16);
  h->entries_crc = get32(in + 20);
  return 0;
}

void manifest_entry_write(const manifest_entry_t *e,
                          uint8_t out[MANIFEST_ENTRY_BYTES]) {
  memset(out, 0, MANIFEST_ENTRY_BYTES);
  put_name((char *)out, e->name);
  put32(out + 16, e->source_bytes);
  put32(out + 20, e->source_crc);
  put32(out + 24, e->file_bytes);
  put32(out + 28, e->file_crc);
}

int manifest_entry_read(manifest_entry_t *e,
                        const uint8_t in[MANIFEST_ENTRY_BYTES]) {
  if (in[0] == '\0' || in[MANIFEST_NAME_BYTES - 1u] != '\0') {
    return -1;
  }
  memcpy(e->name, in, MANIFEST_NAME_BYTES);
  e->source_bytes = get32(in + 16);
  e->source_crc = get32(in + 20);
  e->file_bytes = get32(in + 24);
  e->file_crc = get32(in + 28);
  return 0;
}

void manifest_entry_of(manifest_entry_t *e, const char *name,
                       const clip_header_t *clip, uint32_t file_bytes) {
  memset(e, 0, sizeof(*e));
  put_name(e->name, name);
  e->source_bytes = clip->source_bytes;
  e->source_crc = clip->source_crc;
  e->file_bytes = file_bytes;
  e->file_crc = clip->crc;
}

bool manifest_matches(const manifest_header_t *h, const manifest_entry_t *e,
                      const clip_header_t *clip, uint32_t file_bytes) {
  return clip->converter == h->converter &&
         clip->keep_percent == h->keep_percent &&
         clip->gun_bits == h->gun_bits &&
         clip->source_bytes == e->source_bytes &&
         clip->source_crc == e->source_crc && clip->crc == e->file_crc &&
         file_bytes == e->file_bytes && clip->frames > 0 &&
         clip->index_count > 0 &&
         (uint64_t)clip->index_offset + 8u * (uint64_t)clip->index_count ==
             file_bytes;
}
