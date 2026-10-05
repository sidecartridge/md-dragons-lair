/**
 * File: iso9660.h
 * Description: A read-only ISO 9660 reader for CD-ROM images stored on the
 *              SD card, on top of FatFs.
 *
 * FatFs opens the image (a plain file on a FAT32 or exFAT card) and does
 * every read; this module parses the ISO 9660 volume inside it and gives
 * its files back with a FatFs-like interface. The image's cluster map is
 * built once with FatFs's fast seek (FF_USE_FASTSEEK), so a seek anywhere
 * in a CD-sized file costs no FAT walk.
 *
 *   static iso9660_t iso;  // ~3.7 KB: the FIL, the cluster map, a sector
 *   iso9660_file_t f;
 *   UINT got;
 *   if (iso9660_mount(&iso, "/DIR/IMAGE.ISO", true) == ISO9660_OK &&
 *       iso9660_fopen(&iso, &f, "/SUBDIR/FILE.BIN") == ISO9660_OK) {
 *     iso9660_fread(&f, buf, sizeof(buf), &got);
 *   }
 *   iso9660_unmount(&iso);
 *
 * Supported: the primary volume descriptor and Joliet names (a supplementary
 * descriptor with the UCS-2 escape sequences), directories at any depth,
 * names compared without regard to ASCII case and with or without the ";1"
 * version suffix. Not supported: Rock Ridge names (the plain ISO or Joliet
 * name is used), files recorded in several extents or interleaved (listed,
 * but refused by iso9660_fopen()), block sizes other than 2,048 bytes.
 *
 * Several files of one image may be open at once: each keeps its own
 * position and every read seeks the image's FIL first. Nothing here is safe
 * to call from two cores at once (FatFs is not, in this firmware).
 */

#ifndef ISO9660_H
#define ISO9660_H

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISO9660_SECTOR_SIZE 2048u

// A name as the reader returns it: an ISO name has at most 31 characters
// (37 for a directory under ISO 9660:1999), a Joliet name 64 UCS-2
// characters, up to 192 bytes of UTF-8. Longer Joliet names, which some
// mastering tools write, are cut and flagged `truncated`.
#define ISO9660_NAME_MAX 192u

// The fast-seek cluster map: 2 words per fragment of the image on the card
// plus one; 1 KB holds 128 fragments (a 596 MB image copied onto a card in
// use was measured in 66). An image in more fragments than that is read
// without the map (iso->fast_seek false), which works but walks the FAT on
// every far seek.
#ifndef ISO9660_CLMT_WORDS
#define ISO9660_CLMT_WORDS 257u
#endif

typedef enum {
  ISO9660_OK = 0,
  ISO9660_ERR_ARG = -1,          // a NULL pointer or a bad argument
  ISO9660_ERR_READ = -2,         // FatFs failed (iso->fres says how)
  ISO9660_ERR_NOT_ISO = -3,      // no ISO 9660 volume descriptor
  ISO9660_ERR_BLOCK_SIZE = -4,   // a logical block size other than 2,048
  ISO9660_ERR_TRUNCATED = -5,    // the image is shorter than its volume
  ISO9660_ERR_CORRUPT = -6,      // a directory record that cannot be right
  ISO9660_ERR_NOT_FOUND = -7,    // no such file or directory
  ISO9660_ERR_NOT_DIR = -8,      // a path goes through a file
  ISO9660_ERR_IS_DIR = -9,       // iso9660_fopen() on a directory
  ISO9660_ERR_UNSUPPORTED = -10,  // a file in several extents, interleaved
} iso9660_result_t;

typedef struct {
  FIL fil;                // the image, open for reading
  FRESULT fres;           // FatFs's result of the last call that failed
  uint64_t image_size;    // bytes
  uint32_t volume_blocks; // the volume's size in blocks
  uint32_t root_lba;      // the root directory of the tree in use
  uint32_t root_size;
  uint32_t cached_lba;    // the block held in `sector`, or UINT32_MAX
  uint32_t fragments;     // the image's fragments on the card, 0: unknown
  bool fast_seek;         // FatFs's cluster map is in use
  bool joliet;            // names come from the Joliet tree
  bool mounted;
  char volume_id[33];     // the primary volume identifier, trimmed
#if defined(FF_USE_FASTSEEK) && FF_USE_FASTSEEK
  DWORD clmt[ISO9660_CLMT_WORDS];
#endif
  uint8_t sector[ISO9660_SECTOR_SIZE];
} iso9660_t;

typedef struct {
  char name[ISO9660_NAME_MAX + 1];  // without ";1" or a trailing '.'
  uint64_t offset;                  // the file's first byte in the image
  uint32_t size;                    // bytes
  bool is_dir;
  bool contiguous;  // false: several extents or interleaved; not readable
  bool truncated;   // the name did not fit in `name`
} iso9660_entry_t;

typedef struct {
  iso9660_t *iso;
  uint32_t lba;   // the directory's first block
  uint32_t size;  // bytes
  uint32_t pos;   // the next record's byte position in the directory
} iso9660_dir_t;

typedef struct {
  iso9660_t *iso;
  uint64_t offset;  // the file's first byte in the image
  uint32_t size;    // bytes
  uint32_t pos;     // the next byte iso9660_fread() returns
} iso9660_file_t;

// Opens the image at `path` with FatFs and reads its volume descriptors.
// With `use_joliet` the Joliet tree is used when the image has one
// (iso->joliet says whether it did). The FatFs volume must be mounted.
int iso9660_mount(iso9660_t *iso, const char *path, bool use_joliet);

// Closes the image. Files opened from it must not be used afterwards.
void iso9660_unmount(iso9660_t *iso);

// Iterates a directory: the root, or an entry with is_dir set.
int iso9660_opendir_root(iso9660_t *iso, iso9660_dir_t *dir);
int iso9660_opendir(iso9660_t *iso, const iso9660_entry_t *entry,
                    iso9660_dir_t *dir);

// The next entry of a directory, "." and ".." skipped. Returns 1 with an
// entry, 0 at the end, a negative iso9660_result_t on error.
int iso9660_readdir(iso9660_dir_t *dir, iso9660_entry_t *entry);

// Finds a file or a directory by path from the root: components separated
// by '/', a leading '/' optional, matched without regard to ASCII case and
// to a ";1" suffix.
int iso9660_find(iso9660_t *iso, const char *path, iso9660_entry_t *entry);

// Opens a file of the image for reading, at position 0.
int iso9660_fopen(iso9660_t *iso, iso9660_file_t *fp, const char *path);
int iso9660_fopen_entry(iso9660_t *iso, iso9660_file_t *fp,
                        const iso9660_entry_t *entry);

// As f_read(): reads up to `btr` bytes at the file's position and advances
// it; *br < btr only at the end of the file.
int iso9660_fread(iso9660_file_t *fp, void *buff, UINT btr, UINT *br);

// As f_lseek(), within the file: a position past its end is clamped to it.
int iso9660_fseek(iso9660_file_t *fp, uint32_t pos);

#define iso9660_fsize(fp) ((fp)->size)
#define iso9660_ftell(fp) ((fp)->pos)

// A short English description of a result code.
const char *iso9660_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif  // ISO9660_H
