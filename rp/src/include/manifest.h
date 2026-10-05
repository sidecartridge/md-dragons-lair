/**
 * File: manifest.h
 * Description: A set's manifest: the list of a set's clip files (the game
 *              converted for an ST or for an STE, in its folder), with each
 *              clip's source and file, so that a player can tell a complete
 *              set without the CD-ROM image the clips came from. The format
 *              is in docs/clip-format.md.
 *
 * Whoever converts a set writes it last, the cartridge or the web page:
 * the same bytes from the same clips. Plain C, no allocation.
 */

#ifndef MANIFEST_H
#define MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#include "clip.h"

#define MANIFEST_FILE "SET.DLM"
#define MANIFEST_MAGIC "DLMF"
#define MANIFEST_VERSION 1u
#define MANIFEST_HEADER_BYTES 32u
#define MANIFEST_ENTRY_BYTES 32u
#define MANIFEST_NAME_BYTES 16u  // the source's name, NUL-padded

typedef struct {
  uint16_t count;         // entries: the set's clips
  uint8_t gun_bits;       // the palette's depth: 3 (ST) or 4 (STE)
  uint16_t converter;     // the converter's version (convert.h)
  uint16_t keep_percent;  // its palette stability
  uint32_t entries_crc;   // CRC-32 of the entries, in order
} manifest_header_t;

typedef struct {
  char name[MANIFEST_NAME_BYTES];  // the source clip's, "S01.MPG"
  uint32_t source_bytes;
  uint32_t source_crc;
  uint32_t file_bytes;  // the clip file's size
  uint32_t file_crc;    // and its header's CRC-32
} manifest_entry_t;

void manifest_header_write(const manifest_header_t *h,
                           uint8_t out[MANIFEST_HEADER_BYTES]);

// 0, or -1 for another file, -2 for another version.
int manifest_header_read(manifest_header_t *h,
                         const uint8_t in[MANIFEST_HEADER_BYTES]);

void manifest_entry_write(const manifest_entry_t *e,
                          uint8_t out[MANIFEST_ENTRY_BYTES]);

// 0, or -1 for an entry with no name or one not NUL-terminated.
int manifest_entry_read(manifest_entry_t *e,
                        const uint8_t in[MANIFEST_ENTRY_BYTES]);

// The entry of a clip file: the source's `name`, the file's header and
// size.
void manifest_entry_of(manifest_entry_t *e, const char *name,
                       const clip_header_t *clip, uint32_t file_bytes);

// Whether the clip file with header `clip` and size `file_bytes` is the
// one `e` lists, made as `h` says (converter, stability, depth), and
// whole (its index ends the file).
bool manifest_matches(const manifest_header_t *h, const manifest_entry_t *e,
                      const clip_header_t *clip, uint32_t file_bytes);

#endif  // MANIFEST_H
