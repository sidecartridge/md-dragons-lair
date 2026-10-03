/**
 * File: iso9660.c
 * Description: Read-only ISO 9660 reader on top of FatFs. See iso9660.h.
 *
 * The layout read here (ECMA-119):
 *   - Volume descriptors from block 16 on, 2,048 bytes each: a type byte,
 *     "CD001", version 1. Type 1 is the primary descriptor, type 2 a
 *     supplementary one (Joliet when its escape sequence is %/@, %/C or
 *     %/E), type 255 ends the list. The root directory's record is at byte
 *     156 of a descriptor, the logical block size at 128, the volume's size
 *     in blocks at 80, the volume identifier at 40.
 *   - A directory is a run of records that never cross a block boundary; a
 *     length byte of 0 pads to the next block. A record: length, extended
 *     attribute length, the extent's block and size (both byte orders; the
 *     little-endian half is read), flags (bit 1 directory, bit 7 more
 *     extents follow), interleave unit and gap, the name's length and the
 *     name. Names 0x00 and 0x01 are "." and "..".
 */

#include "iso9660.h"

#include <string.h>

#define VD_FIRST_BLOCK 16u
#define VD_MAX 64u  // descriptors read before giving up on a terminator
#define VD_PRIMARY 1u
#define VD_SUPPLEMENTARY 2u
#define VD_TERMINATOR 255u
#define VD_VOLUME_BLOCKS 80u
#define VD_ESCAPES 88u
#define VD_BLOCK_SIZE 128u
#define VD_ROOT_RECORD 156u
#define VD_VOLUME_ID 40u
#define VD_VOLUME_ID_LEN 32u

#define DR_LEN 0u
#define DR_EXT_ATTR_LEN 1u
#define DR_EXTENT 2u
#define DR_SIZE 10u
#define DR_FLAGS 25u
#define DR_UNIT_SIZE 26u
#define DR_GAP 27u
#define DR_NAME_LEN 32u
#define DR_NAME 33u
#define DR_MIN_LEN 34u  // a record with a one-byte name

#define DR_FLAG_DIR 0x02u
#define DR_FLAG_MORE_EXTENTS 0x80u

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static char ascii_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Reads `len` bytes of the image at `offset` with FatFs.
static int read_image(iso9660_t *iso, uint64_t offset, void *buf, UINT len) {
  if (offset + len > iso->image_size) {
    return ISO9660_ERR_TRUNCATED;
  }
  FRESULT fres = f_lseek(&iso->fil, (FSIZE_t)offset);
  UINT got = 0;
  if (fres == FR_OK) {
    fres = f_read(&iso->fil, buf, len, &got);
  }
  if (fres != FR_OK) {
    iso->fres = fres;
    return ISO9660_ERR_READ;
  }
  return (got == len) ? ISO9660_OK : ISO9660_ERR_TRUNCATED;
}

// Brings block `lba` into iso->sector, unless it is there already.
static int read_block(iso9660_t *iso, uint32_t lba) {
  if (iso->cached_lba == lba) {
    return ISO9660_OK;
  }
  iso->cached_lba = UINT32_MAX;
  int result = read_image(iso, (uint64_t)lba * ISO9660_SECTOR_SIZE,
                          iso->sector, ISO9660_SECTOR_SIZE);
  if (result == ISO9660_OK) {
    iso->cached_lba = lba;
  }
  return result;
}

static bool is_joliet_escape(const uint8_t *esc) {
  return esc[0] == '%' && esc[1] == '/' &&
         (esc[2] == '@' || esc[2] == 'C' || esc[2] == 'E');
}

// Drops a ";<digits>" version suffix and then one trailing '.', as
// "NAME.;1" or "NAME;1" are both "NAME". Returns the new length.
static size_t strip_version(const char *name, size_t len) {
  for (size_t i = len; i > 0; i--) {
    if (name[i - 1] == ';') {
      len = i - 1;
      break;
    }
    if (name[i - 1] < '0' || name[i - 1] > '9') {
      break;
    }
  }
  if (len > 1 && name[len - 1] == '.') {
    len--;
  }
  return len;
}

// Copies a record's name into entry->name: ISO bytes as they are, Joliet
// UCS-2 (big-endian) as UTF-8.
static void decode_name(const iso9660_t *iso, const uint8_t *raw,
                        size_t raw_len, iso9660_entry_t *entry) {
  size_t out = 0;
  entry->truncated = false;
  if (iso->joliet) {
    for (size_t i = 0; i + 1 < raw_len; i += 2) {
      uint16_t u = (uint16_t)((raw[i] << 8) | raw[i + 1]);
      if (u >= 0xD800u && u <= 0xDFFFu) {
        u = '?';  // a surrogate: UCS-2 has none
      }
      size_t need = (u < 0x80u) ? 1u : (u < 0x800u) ? 2u : 3u;
      if (out + need > ISO9660_NAME_MAX) {
        entry->truncated = true;
        break;
      }
      if (need == 1u) {
        entry->name[out++] = (char)u;
      } else if (need == 2u) {
        entry->name[out++] = (char)(0xC0u | (u >> 6));
        entry->name[out++] = (char)(0x80u | (u & 0x3Fu));
      } else {
        entry->name[out++] = (char)(0xE0u | (u >> 12));
        entry->name[out++] = (char)(0x80u | ((u >> 6) & 0x3Fu));
        entry->name[out++] = (char)(0x80u | (u & 0x3Fu));
      }
    }
  } else {
    out = raw_len;
    if (out > ISO9660_NAME_MAX) {
      out = ISO9660_NAME_MAX;
      entry->truncated = true;
    }
    memcpy(entry->name, raw, out);
  }
  if (!entry->truncated) {
    out = strip_version(entry->name, out);
  }
  entry->name[out] = '\0';
}

int iso9660_mount(iso9660_t *iso, const char *path, bool use_joliet) {
  if (iso == NULL || path == NULL) {
    return ISO9660_ERR_ARG;
  }
  memset(iso, 0, sizeof(*iso));
  iso->cached_lba = UINT32_MAX;
  iso->fres = f_open(&iso->fil, path, FA_READ);
  if (iso->fres != FR_OK) {
    return (iso->fres == FR_NO_FILE || iso->fres == FR_NO_PATH)
               ? ISO9660_ERR_NOT_FOUND
               : ISO9660_ERR_READ;
  }
  iso->mounted = true;
  iso->image_size = (uint64_t)f_size(&iso->fil);

  int result = ISO9660_OK;
  if (iso->image_size < (uint64_t)(VD_FIRST_BLOCK + 1u) * ISO9660_SECTOR_SIZE) {
    result = ISO9660_ERR_NOT_ISO;
    goto fail;
  }

#if defined(FF_USE_FASTSEEK) && FF_USE_FASTSEEK
  // The cluster map makes every f_lseek() a table lookup. An image in too
  // many fragments for the map is still read, the slow way.
  iso->fil.cltbl = iso->clmt;
  iso->clmt[0] = ISO9660_CLMT_WORDS;
  FRESULT fres = f_lseek(&iso->fil, CREATE_LINKMAP);
  if (fres == FR_OK || fres == FR_NOT_ENOUGH_CORE) {
    iso->fragments = (iso->clmt[0] - 1u) / 2u;
    iso->fast_seek = (fres == FR_OK);
  }
  if (fres != FR_OK) {
    iso->fil.cltbl = NULL;
    if (fres != FR_NOT_ENOUGH_CORE) {
      iso->fres = fres;
      result = ISO9660_ERR_READ;
      goto fail;
    }
  }
#endif

  bool have_primary = false;
  uint32_t joliet_lba = 0;
  uint32_t joliet_size = 0;
  for (uint32_t i = 0; i < VD_MAX; i++) {
    uint32_t lba = VD_FIRST_BLOCK + i;
    if ((uint64_t)(lba + 1u) * ISO9660_SECTOR_SIZE > iso->image_size) {
      break;
    }
    result = read_block(iso, lba);
    if (result != ISO9660_OK) {
      goto fail;
    }
    const uint8_t *vd = iso->sector;
    if (memcmp(vd + 1, "CD001", 5) != 0 || vd[6] != 1u ||
        vd[0] == VD_TERMINATOR) {
      break;
    }
    const uint8_t *root = vd + VD_ROOT_RECORD;
    if (vd[0] == VD_PRIMARY && !have_primary) {
      if (le16(vd + VD_BLOCK_SIZE) != ISO9660_SECTOR_SIZE) {
        result = ISO9660_ERR_BLOCK_SIZE;
        goto fail;
      }
      if (root[DR_LEN] < DR_MIN_LEN) {
        result = ISO9660_ERR_CORRUPT;
        goto fail;
      }
      have_primary = true;
      iso->volume_blocks = le32(vd + VD_VOLUME_BLOCKS);
      iso->root_lba = le32(root + DR_EXTENT);
      iso->root_size = le32(root + DR_SIZE);
      size_t n = VD_VOLUME_ID_LEN;
      while (n > 0 && (vd[VD_VOLUME_ID + n - 1] == ' ' ||
                       vd[VD_VOLUME_ID + n - 1] == '\0')) {
        n--;
      }
      memcpy(iso->volume_id, vd + VD_VOLUME_ID, n);
      iso->volume_id[n] = '\0';
    } else if (vd[0] == VD_SUPPLEMENTARY && joliet_lba == 0 &&
               is_joliet_escape(vd + VD_ESCAPES) &&
               le16(vd + VD_BLOCK_SIZE) == ISO9660_SECTOR_SIZE &&
               root[DR_LEN] >= DR_MIN_LEN) {
      joliet_lba = le32(root + DR_EXTENT);
      joliet_size = le32(root + DR_SIZE);
    }
  }
  if (!have_primary) {
    result = ISO9660_ERR_NOT_ISO;
    goto fail;
  }
  if ((uint64_t)iso->volume_blocks * ISO9660_SECTOR_SIZE > iso->image_size) {
    result = ISO9660_ERR_TRUNCATED;
    goto fail;
  }
  if (use_joliet && joliet_lba != 0) {
    iso->root_lba = joliet_lba;
    iso->root_size = joliet_size;
    iso->joliet = true;
  }
  if (iso->root_size == 0 ||
      (uint64_t)iso->root_lba * ISO9660_SECTOR_SIZE + iso->root_size >
          iso->image_size) {
    result = ISO9660_ERR_CORRUPT;
    goto fail;
  }
  return ISO9660_OK;

fail:
  f_close(&iso->fil);
  iso->mounted = false;
  return result;
}

void iso9660_unmount(iso9660_t *iso) {
  if (iso != NULL && iso->mounted) {
    f_close(&iso->fil);
    iso->mounted = false;
    iso->cached_lba = UINT32_MAX;
  }
}

int iso9660_opendir_root(iso9660_t *iso, iso9660_dir_t *dir) {
  if (iso == NULL || dir == NULL || !iso->mounted) {
    return ISO9660_ERR_ARG;
  }
  dir->iso = iso;
  dir->lba = iso->root_lba;
  dir->size = iso->root_size;
  dir->pos = 0;
  return ISO9660_OK;
}

int iso9660_opendir(iso9660_t *iso, const iso9660_entry_t *entry,
                    iso9660_dir_t *dir) {
  if (iso == NULL || entry == NULL || dir == NULL || !iso->mounted) {
    return ISO9660_ERR_ARG;
  }
  if (!entry->is_dir) {
    return ISO9660_ERR_NOT_DIR;
  }
  dir->iso = iso;
  dir->lba = (uint32_t)(entry->offset / ISO9660_SECTOR_SIZE);
  dir->size = entry->size;
  dir->pos = 0;
  return ISO9660_OK;
}

int iso9660_readdir(iso9660_dir_t *dir, iso9660_entry_t *entry) {
  if (dir == NULL || entry == NULL || dir->iso == NULL) {
    return ISO9660_ERR_ARG;
  }
  iso9660_t *iso = dir->iso;
  while (dir->pos < dir->size) {
    uint32_t in_block = dir->pos % ISO9660_SECTOR_SIZE;
    int result = read_block(iso, dir->lba + dir->pos / ISO9660_SECTOR_SIZE);
    if (result != ISO9660_OK) {
      return result;
    }
    const uint8_t *rec = iso->sector + in_block;
    uint32_t len = rec[DR_LEN];
    if (len == 0) {
      // Padding: the rest of this block is empty.
      dir->pos += ISO9660_SECTOR_SIZE - in_block;
      continue;
    }
    uint32_t name_len = rec[DR_NAME_LEN];
    if (len < DR_MIN_LEN || in_block + len > ISO9660_SECTOR_SIZE ||
        dir->pos + len > dir->size || name_len == 0 ||
        DR_NAME + name_len > len) {
      return ISO9660_ERR_CORRUPT;
    }
    dir->pos += len;
    if (name_len == 1 && rec[DR_NAME] <= 1u) {
      continue;  // "." or ".."
    }
    uint8_t flags = rec[DR_FLAGS];
    entry->offset =
        ((uint64_t)le32(rec + DR_EXTENT) + rec[DR_EXT_ATTR_LEN]) *
        ISO9660_SECTOR_SIZE;
    entry->size = le32(rec + DR_SIZE);
    if (entry->offset + entry->size > iso->image_size) {
      return ISO9660_ERR_CORRUPT;
    }
    entry->is_dir = (flags & DR_FLAG_DIR) != 0;
    entry->contiguous = (flags & DR_FLAG_MORE_EXTENTS) == 0 &&
                        rec[DR_UNIT_SIZE] == 0 && rec[DR_GAP] == 0;
    decode_name(iso, rec + DR_NAME, name_len, entry);
    return 1;
  }
  return 0;
}

// True when `name` is the path component comp[0..len), which may carry a
// version suffix of its own.
static bool name_matches(const char *name, const char *comp, size_t len) {
  len = strip_version(comp, len);
  size_t i = 0;
  for (; i < len; i++) {
    if (name[i] == '\0' || ascii_upper(name[i]) != ascii_upper(comp[i])) {
      return false;
    }
  }
  return name[i] == '\0';
}

int iso9660_find(iso9660_t *iso, const char *path, iso9660_entry_t *entry) {
  if (iso == NULL || path == NULL || entry == NULL) {
    return ISO9660_ERR_ARG;
  }
  iso9660_dir_t dir;
  int result = iso9660_opendir_root(iso, &dir);
  if (result != ISO9660_OK) {
    return result;
  }
  const char *comp = path;
  while (*comp == '/') {
    comp++;
  }
  if (*comp == '\0') {
    entry->name[0] = '\0';
    entry->offset = (uint64_t)iso->root_lba * ISO9660_SECTOR_SIZE;
    entry->size = iso->root_size;
    entry->is_dir = true;
    entry->contiguous = true;
    entry->truncated = false;
    return ISO9660_OK;
  }
  for (;;) {
    const char *end = comp;
    while (*end != '\0' && *end != '/') {
      end++;
    }
    bool found = false;
    while ((result = iso9660_readdir(&dir, entry)) == 1) {
      if (!entry->truncated &&
          name_matches(entry->name, comp, (size_t)(end - comp))) {
        found = true;
        break;
      }
    }
    if (result < 0) {
      return result;
    }
    if (!found) {
      return ISO9660_ERR_NOT_FOUND;
    }
    while (*end == '/') {
      end++;
    }
    if (*end == '\0') {
      return ISO9660_OK;
    }
    result = iso9660_opendir(iso, entry, &dir);
    if (result != ISO9660_OK) {
      return result;
    }
    comp = end;
  }
}

int iso9660_fopen_entry(iso9660_t *iso, iso9660_file_t *fp,
                        const iso9660_entry_t *entry) {
  if (iso == NULL || fp == NULL || entry == NULL || !iso->mounted) {
    return ISO9660_ERR_ARG;
  }
  if (entry->is_dir) {
    return ISO9660_ERR_IS_DIR;
  }
  if (!entry->contiguous) {
    return ISO9660_ERR_UNSUPPORTED;
  }
  fp->iso = iso;
  fp->offset = entry->offset;
  fp->size = entry->size;
  fp->pos = 0;
  return ISO9660_OK;
}

int iso9660_fopen(iso9660_t *iso, iso9660_file_t *fp, const char *path) {
  iso9660_entry_t entry;
  int result = iso9660_find(iso, path, &entry);
  if (result != ISO9660_OK) {
    return result;
  }
  return iso9660_fopen_entry(iso, fp, &entry);
}

int iso9660_fread(iso9660_file_t *fp, void *buff, UINT btr, UINT *br) {
  if (fp == NULL || fp->iso == NULL || br == NULL ||
      (buff == NULL && btr != 0)) {
    return ISO9660_ERR_ARG;
  }
  *br = 0;
  uint32_t left = fp->size - fp->pos;
  if (btr > left) {
    btr = left;
  }
  if (btr == 0) {
    return ISO9660_OK;
  }
  int result = read_image(fp->iso, fp->offset + fp->pos, buff, btr);
  if (result != ISO9660_OK) {
    return result;
  }
  fp->pos += btr;
  *br = btr;
  return ISO9660_OK;
}

int iso9660_fseek(iso9660_file_t *fp, uint32_t pos) {
  if (fp == NULL || fp->iso == NULL) {
    return ISO9660_ERR_ARG;
  }
  fp->pos = (pos > fp->size) ? fp->size : pos;
  return ISO9660_OK;
}

const char *iso9660_strerror(int result) {
  switch (result) {
    case ISO9660_OK:
      return "ok";
    case ISO9660_ERR_ARG:
      return "bad argument";
    case ISO9660_ERR_READ:
      return "read error";
    case ISO9660_ERR_NOT_ISO:
      return "not an ISO 9660 image";
    case ISO9660_ERR_BLOCK_SIZE:
      return "unsupported block size";
    case ISO9660_ERR_TRUNCATED:
      return "image truncated";
    case ISO9660_ERR_CORRUPT:
      return "corrupt directory";
    case ISO9660_ERR_NOT_FOUND:
      return "not found";
    case ISO9660_ERR_NOT_DIR:
      return "not a directory";
    case ISO9660_ERR_IS_DIR:
      return "is a directory";
    case ISO9660_ERR_UNSUPPORTED:
      return "multi-extent or interleaved file";
    default:
      return "unknown error";
  }
}
