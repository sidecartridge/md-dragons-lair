// units: rp/src/iso9660.c rp/src/crc32.c
//
// The ISO 9660 reader against images the test builds in memory: a primary
// and a Joliet tree, a root directory over two blocks, a subdirectory, a
// multi-extent file, and broken variants of each. FatFs is replaced by
// in-memory files (shim/ff.h).
//
// With DL_ISO set to the path of the Dragon's Lair CD-ROM image, the test
// also reads the real image: its root directory, the scene clips and the
// CRC-32 of S01.MPG. Without it that part is skipped. The image is never in
// the repository.

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc32.h"
#include "ff.h"
#include "iso9660.h"
#include "test.h"

// --- FatFs over files in memory -------------------------------------------

typedef struct {
  const char *path;
  const BYTE *data;
  FSIZE_t size;
} mem_file_t;

static mem_file_t mem_files[4];
static int mem_file_count;
static int fail_reads;  // f_read() fails while set

static void mem_files_reset(void) {
  mem_file_count = 0;
  fail_reads = 0;
}

static void mem_file_add(const char *path, const void *data, FSIZE_t size) {
  mem_files[mem_file_count].path = path;
  mem_files[mem_file_count].data = data;
  mem_files[mem_file_count].size = size;
  mem_file_count++;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)mode;
  for (int i = 0; i < mem_file_count; i++) {
    if (strcmp(mem_files[i].path, path) == 0) {
      fp->data = mem_files[i].data;
      fp->size = mem_files[i].size;
      fp->pos = 0;
      return FR_OK;
    }
  }
  return FR_NO_FILE;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  *br = 0;
  if (fail_reads) {
    return FR_DISK_ERR;
  }
  FSIZE_t n = fp->size - fp->pos;
  if (n > btr) {
    n = btr;
  }
  memcpy(buff, fp->data + fp->pos, n);
  fp->pos += n;
  *br = (UINT)n;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  fp->pos = (ofs > fp->size) ? fp->size : ofs;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  (void)fp;
  return FR_OK;
}

// --- An image built in memory -----------------------------------------------

#define BS 2048u
#define IMG_BLOCKS 96u

// Where things are, in blocks.
#define LBA_PVD 16u
#define LBA_SVD 17u
#define LBA_TERM 18u
#define LBA_ROOT 20u  // two blocks
#define LBA_SUBDIR 22u
#define LBA_JROOT 24u
#define LBA_JSUBDIR 25u
#define LBA_FILES 30u  // FILE00..FILE49, one block each
#define FILE_COUNT 50
#define LBA_S01 80u
#define SIZE_S01 (3u * BS + 123u)
#define LBA_INNER 84u
#define SIZE_INNER 5000u
#define LBA_NOEXT 87u
#define LBA_MULTI 88u

static uint8_t img[IMG_BLOCKS * BS];

static uint8_t pattern(uint32_t lba, uint32_t i) {
  return (uint8_t)(lba * 7u + i * 13u + (i >> 8));
}

static void put32(uint8_t *p, uint32_t v) {  // both byte orders
  for (int i = 0; i < 4; i++) {
    p[i] = (uint8_t)(v >> (8 * i));
    p[7 - i] = (uint8_t)(v >> (8 * i));
  }
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

typedef struct {
  uint8_t *base;  // the directory's first block
  uint32_t pos;
} dir_writer_t;

// Appends a directory record, starting a new block when it would cross one.
static uint8_t *add_record(dir_writer_t *d, const uint8_t *name,
                           uint8_t name_len, uint32_t lba, uint32_t size,
                           uint8_t flags) {
  uint32_t len = 33u + name_len;
  len += len & 1u;
  if (d->pos % BS + len > BS) {
    d->pos = (d->pos / BS + 1u) * BS;
  }
  uint8_t *r = d->base + d->pos;
  r[0] = (uint8_t)len;
  put32(r + 2, lba);
  put32(r + 10, size);
  r[25] = flags;
  put16(r + 28, 1);
  r[32] = name_len;
  memcpy(r + 33, name, name_len);
  d->pos += len;
  return r;
}

static uint8_t *add_iso(dir_writer_t *d, const char *name, uint32_t lba,
                        uint32_t size, uint8_t flags) {
  return add_record(d, (const uint8_t *)name, (uint8_t)strlen(name), lba,
                    size, flags);
}

// A Joliet name from UTF-16 code units.
static uint8_t *add_joliet(dir_writer_t *d, const uint16_t *units, int n,
                           uint32_t lba, uint32_t size, uint8_t flags) {
  uint8_t name[254];
  for (int i = 0; i < n; i++) {
    name[2 * i] = (uint8_t)(units[i] >> 8);
    name[2 * i + 1] = (uint8_t)units[i];
  }
  return add_record(d, name, (uint8_t)(2 * n), lba, size, flags);
}

static uint8_t *add_joliet_ascii(dir_writer_t *d, const char *s, uint32_t lba,
                                 uint32_t size, uint8_t flags) {
  uint16_t units[127];
  int n = (int)strlen(s);
  for (int i = 0; i < n; i++) {
    units[i] = (uint16_t)(unsigned char)s[i];
  }
  return add_joliet(d, units, n, lba, size, flags);
}

static void add_dots(dir_writer_t *d, uint32_t self, uint32_t self_size,
                     uint32_t parent, uint32_t parent_size) {
  const uint8_t dot = 0, dotdot = 1;
  add_record(d, &dot, 1, self, self_size, 0x02);
  add_record(d, &dotdot, 1, parent, parent_size, 0x02);
}

static void fill_extent(uint32_t lba, uint32_t size) {
  for (uint32_t i = 0; i < size; i++) {
    img[lba * BS + i] = pattern(lba, i);
  }
}

static void write_descriptor(uint32_t lba, uint8_t type, uint32_t root_lba,
                             uint32_t root_size, bool joliet) {
  uint8_t *vd = img + lba * BS;
  vd[0] = type;
  memcpy(vd + 1, "CD001", 5);
  vd[6] = 1;
  if (type == 255) {
    return;
  }
  memset(vd + 40, ' ', 32);
  memcpy(vd + 40, "TESTVOL", 7);
  put32(vd + 80, IMG_BLOCKS);
  put16(vd + 128, BS);
  if (joliet) {
    memcpy(vd + 88, "%/E", 3);
  }
  uint8_t *root = vd + 156;
  root[0] = 34;
  put32(root + 2, root_lba);
  put32(root + 10, root_size);
  root[25] = 0x02;
  root[32] = 1;
}

// Where the files' records ended up, for the broken variants.
static uint8_t *file_records[FILE_COUNT];
#define file49_record (file_records[FILE_COUNT - 1])

static void build_image(void) {
  memset(img, 0, sizeof(img));
  write_descriptor(LBA_PVD, 1, LBA_ROOT, 2 * BS, false);
  write_descriptor(LBA_SVD, 2, LBA_JROOT, BS, true);
  write_descriptor(LBA_TERM, 255, 0, 0, false);

  dir_writer_t root = {img + LBA_ROOT * BS, 0};
  add_dots(&root, LBA_ROOT, 2 * BS, LBA_ROOT, 2 * BS);
  for (int i = 0; i < FILE_COUNT; i++) {
    char name[16];
    snprintf(name, sizeof(name), "FILE%02d.TXT;1", i);
    file_records[i] = add_iso(&root, name, LBA_FILES + (uint32_t)i,
                              100u + (uint32_t)i, 0);
    fill_extent(LBA_FILES + (uint32_t)i, 100u + (uint32_t)i);
  }
  add_iso(&root, "NOEXT.;1", LBA_NOEXT, 10, 0);
  add_iso(&root, "S01.MPG;1", LBA_S01, SIZE_S01, 0);
  add_iso(&root, "SUBDIR", LBA_SUBDIR, BS, 0x02);
  add_iso(&root, "MULTI.BIN;1", LBA_MULTI, BS, 0x80);
  fill_extent(LBA_NOEXT, 10);
  fill_extent(LBA_S01, SIZE_S01);

  dir_writer_t sub = {img + LBA_SUBDIR * BS, 0};
  add_dots(&sub, LBA_SUBDIR, BS, LBA_ROOT, 2 * BS);
  add_iso(&sub, "INNER.BIN;1", LBA_INNER, SIZE_INNER, 0);
  fill_extent(LBA_INNER, SIZE_INNER);

  dir_writer_t jroot = {img + LBA_JROOT * BS, 0};
  add_dots(&jroot, LBA_JROOT, BS, LBA_JROOT, BS);
  add_joliet_ascii(&jroot, "Long File Name.txt;1", LBA_FILES, 100, 0);
  add_joliet_ascii(&jroot, "S01.MPG;1", LBA_S01, SIZE_S01, 0);
  add_joliet_ascii(&jroot, "SubDir", LBA_JSUBDIR, BS, 0x02);
  uint16_t accented[3] = {0x00E9, 0x0041, 0x3042};  // e-acute, A, hiragana a
  add_joliet(&jroot, accented, 3, LBA_NOEXT, 10, 0);
  uint16_t long_name[70];
  for (int i = 0; i < 70; i++) {
    long_name[i] = 0x3042;  // 3 bytes of UTF-8 each: 210 > 192
  }
  add_joliet(&jroot, long_name, 70, LBA_NOEXT, 10, 0);

  dir_writer_t jsub = {img + LBA_JSUBDIR * BS, 0};
  add_dots(&jsub, LBA_JSUBDIR, BS, LBA_JROOT, BS);
  add_joliet_ascii(&jsub, "inner.bin;1", LBA_INNER, SIZE_INNER, 0);
}

static iso9660_t iso;

static int mount_built(bool joliet) {
  mem_files_reset();
  mem_file_add("/DLAIR/TEST.ISO", img, sizeof(img));
  return iso9660_mount(&iso, "/DLAIR/TEST.ISO", joliet);
}

// --- Tests ------------------------------------------------------------------

static void test_mount(void) {
  build_image();
  CHECK_EQ(mount_built(false), ISO9660_OK);
  CHECK(strcmp(iso.volume_id, "TESTVOL") == 0);
  CHECK(!iso.joliet);
  CHECK_EQ(iso.volume_blocks, IMG_BLOCKS);
  CHECK_EQ(iso.root_lba, LBA_ROOT);
  CHECK_EQ(iso.root_size, 2 * BS);
  iso9660_unmount(&iso);

  CHECK_EQ(mount_built(true), ISO9660_OK);
  CHECK(iso.joliet);
  CHECK_EQ(iso.root_lba, LBA_JROOT);
  iso9660_unmount(&iso);
}

static void test_list_root(void) {
  build_image();
  CHECK_EQ(mount_built(false), ISO9660_OK);
  iso9660_dir_t dir;
  iso9660_entry_t e;
  CHECK_EQ(iso9660_opendir_root(&iso, &dir), ISO9660_OK);
  int n = 0, files = 0, dirs = 0, rc;
  bool saw_noext = false, saw_file49 = false, saw_multi = false;
  while ((rc = iso9660_readdir(&dir, &e)) == 1) {
    n++;
    if (e.is_dir) {
      dirs++;
      CHECK(strcmp(e.name, "SUBDIR") == 0);
    } else {
      files++;
    }
    if (strcmp(e.name, "NOEXT") == 0) {
      saw_noext = true;
    }
    if (strcmp(e.name, "FILE49.TXT") == 0) {
      saw_file49 = true;  // in the root's second block
      CHECK_EQ(e.size, 149);
      CHECK_EQ(e.offset, (uint64_t)(LBA_FILES + 49) * BS);
    }
    if (strcmp(e.name, "MULTI.BIN") == 0) {
      saw_multi = true;
      CHECK(!e.contiguous);
    }
  }
  CHECK_EQ(rc, 0);
  CHECK_EQ(n, FILE_COUNT + 4);
  CHECK_EQ(files, FILE_COUNT + 3);
  CHECK_EQ(dirs, 1);
  CHECK(saw_noext);
  CHECK(saw_file49);
  CHECK(saw_multi);
  // The listing really spans two blocks.
  CHECK(file49_record >= img + (LBA_ROOT + 1) * BS);
  iso9660_unmount(&iso);
}

static void test_find(void) {
  build_image();
  CHECK_EQ(mount_built(false), ISO9660_OK);
  iso9660_entry_t e;
  const char *same[] = {"S01.MPG", "s01.mpg", "/S01.MPG;1", "//S01.MPG"};
  for (int i = 0; i < 4; i++) {
    CHECK_EQ(iso9660_find(&iso, same[i], &e), ISO9660_OK);
    CHECK_EQ(e.offset, (uint64_t)LBA_S01 * BS);
    CHECK_EQ(e.size, SIZE_S01);
  }
  CHECK_EQ(iso9660_find(&iso, "SUBDIR/INNER.BIN", &e), ISO9660_OK);
  CHECK_EQ(e.size, SIZE_INNER);
  CHECK_EQ(iso9660_find(&iso, "subdir/inner.bin;1", &e), ISO9660_OK);
  CHECK_EQ(iso9660_find(&iso, "SUBDIR/", &e), ISO9660_OK);
  CHECK(e.is_dir);
  CHECK_EQ(iso9660_find(&iso, "/", &e), ISO9660_OK);
  CHECK(e.is_dir);
  CHECK_EQ(e.offset, (uint64_t)LBA_ROOT * BS);
  CHECK_EQ(iso9660_find(&iso, "NOEXT", &e), ISO9660_OK);
  CHECK_EQ(iso9660_find(&iso, "NOEXT.", &e), ISO9660_OK);
  CHECK_EQ(iso9660_find(&iso, "S01", &e), ISO9660_ERR_NOT_FOUND);
  CHECK_EQ(iso9660_find(&iso, "SUBDIR/NOPE", &e), ISO9660_ERR_NOT_FOUND);
  CHECK_EQ(iso9660_find(&iso, "FILE00.TXT/X", &e), ISO9660_ERR_NOT_DIR);

  iso9660_file_t f;
  CHECK_EQ(iso9660_fopen(&iso, &f, "MULTI.BIN"), ISO9660_ERR_UNSUPPORTED);
  CHECK_EQ(iso9660_fopen(&iso, &f, "SUBDIR"), ISO9660_ERR_IS_DIR);
  CHECK_EQ(iso9660_fopen(&iso, &f, "NOPE"), ISO9660_ERR_NOT_FOUND);
  iso9660_unmount(&iso);
}

static void test_read(void) {
  build_image();
  CHECK_EQ(mount_built(false), ISO9660_OK);
  iso9660_file_t f;
  CHECK_EQ(iso9660_fopen(&iso, &f, "S01.MPG"), ISO9660_OK);
  CHECK_EQ(iso9660_fsize(&f), SIZE_S01);

  // Every chunk size from 1 to 3 blocks, whole file, against the pattern.
  static uint8_t buf[3 * BS + 7];
  int bad = 0;
  for (UINT chunk = 1; chunk <= sizeof(buf); chunk += 511) {
    CHECK_EQ(iso9660_fseek(&f, 0), ISO9660_OK);
    uint32_t total = 0;
    UINT got;
    do {
      CHECK_EQ(iso9660_fread(&f, buf, chunk, &got), ISO9660_OK);
      for (UINT i = 0; i < got; i++) {
        bad += buf[i] != pattern(LBA_S01, total + i);
      }
      total += got;
    } while (got == chunk);
    CHECK_EQ(total, SIZE_S01);
    CHECK_EQ(iso9660_ftell(&f), SIZE_S01);
  }
  CHECK_EQ(bad, 0);

  // At the end and past it.
  UINT got;
  CHECK_EQ(iso9660_fseek(&f, SIZE_S01 - 5), ISO9660_OK);
  CHECK_EQ(iso9660_fread(&f, buf, 100, &got), ISO9660_OK);
  CHECK_EQ(got, 5);
  CHECK_EQ(buf[4], pattern(LBA_S01, SIZE_S01 - 1));
  CHECK_EQ(iso9660_fread(&f, buf, 100, &got), ISO9660_OK);
  CHECK_EQ(got, 0);
  CHECK_EQ(iso9660_fseek(&f, SIZE_S01 + 1000), ISO9660_OK);
  CHECK_EQ(iso9660_ftell(&f), SIZE_S01);

  // A file read across a block boundary from an odd position.
  iso9660_file_t inner;
  CHECK_EQ(iso9660_fopen(&iso, &inner, "SUBDIR/INNER.BIN"), ISO9660_OK);
  CHECK_EQ(iso9660_fseek(&inner, BS - 3), ISO9660_OK);
  CHECK_EQ(iso9660_fread(&inner, buf, 10, &got), ISO9660_OK);
  CHECK_EQ(got, 10);
  for (UINT i = 0; i < 10; i++) {
    CHECK_EQ(buf[i], pattern(LBA_INNER, BS - 3 + i));
  }

  // Two files open at once keep their own positions.
  CHECK_EQ(iso9660_fseek(&f, 7), ISO9660_OK);
  CHECK_EQ(iso9660_fread(&inner, buf, 1, &got), ISO9660_OK);
  CHECK_EQ(iso9660_fread(&f, buf, 1, &got), ISO9660_OK);
  CHECK_EQ(buf[0], pattern(LBA_S01, 7));

  // A failing card.
  fail_reads = 1;
  CHECK_EQ(iso9660_fread(&f, buf, 1, &got), ISO9660_ERR_READ);
  CHECK_EQ(got, 0);
  CHECK_EQ(iso.fres, FR_DISK_ERR);
  fail_reads = 0;
  iso9660_unmount(&iso);
}

static void test_joliet(void) {
  build_image();
  CHECK_EQ(mount_built(true), ISO9660_OK);
  iso9660_entry_t e;
  CHECK_EQ(iso9660_find(&iso, "long file name.TXT", &e), ISO9660_OK);
  CHECK_EQ(e.size, 100);
  CHECK_EQ(iso9660_find(&iso, "SubDir/inner.bin", &e), ISO9660_OK);
  CHECK_EQ(e.size, SIZE_INNER);
  CHECK_EQ(iso9660_find(&iso, "S01.MPG", &e), ISO9660_OK);

  iso9660_dir_t dir;
  CHECK_EQ(iso9660_opendir_root(&iso, &dir), ISO9660_OK);
  int n = 0, truncated = 0, rc;
  bool saw_utf8 = false;
  while ((rc = iso9660_readdir(&dir, &e)) == 1) {
    n++;
    if (e.truncated) {
      truncated++;
      CHECK(strlen(e.name) <= ISO9660_NAME_MAX);
      CHECK_EQ(strlen(e.name) % 3, 0);  // never cut inside a character
    }
    if (strcmp(e.name, "\xC3\xA9" "A\xE3\x81\x82") == 0) {
      saw_utf8 = true;
    }
  }
  CHECK_EQ(rc, 0);
  CHECK_EQ(n, 5);
  CHECK_EQ(truncated, 1);
  CHECK(saw_utf8);
  iso9660_unmount(&iso);

  // Asked for Joliet on an image without it: the ISO names are used.
  build_image();
  memset(img + LBA_SVD * BS + 88, 0, 3);
  CHECK_EQ(mount_built(true), ISO9660_OK);
  CHECK(!iso.joliet);
  CHECK_EQ(iso9660_find(&iso, "SUBDIR/INNER.BIN", &e), ISO9660_OK);
  iso9660_unmount(&iso);
}

static void test_refusals(void) {
  iso9660_entry_t e;
  iso9660_dir_t dir;

  CHECK_EQ(iso9660_mount(&iso, NULL, false), ISO9660_ERR_ARG);
  mem_files_reset();
  CHECK_EQ(iso9660_mount(&iso, "/NOPE.ISO", false), ISO9660_ERR_NOT_FOUND);

  memset(img, 0, sizeof(img));
  CHECK_EQ(mount_built(false), ISO9660_ERR_NOT_ISO);

  build_image();
  mem_files_reset();
  mem_file_add("/SHORT.ISO", img, 10 * BS);
  CHECK_EQ(iso9660_mount(&iso, "/SHORT.ISO", false), ISO9660_ERR_NOT_ISO);

  // A volume larger than the file holding it.
  build_image();
  mem_files_reset();
  mem_file_add("/CUT.ISO", img, 90 * BS);
  CHECK_EQ(iso9660_mount(&iso, "/CUT.ISO", false), ISO9660_ERR_TRUNCATED);

  build_image();
  put16(img + LBA_PVD * BS + 128, 512);
  CHECK_EQ(mount_built(false), ISO9660_ERR_BLOCK_SIZE);

  // The root directory outside the image.
  build_image();
  put32(img + LBA_PVD * BS + 156 + 2, IMG_BLOCKS);
  CHECK_EQ(mount_built(false), ISO9660_ERR_CORRUPT);

  // A file whose extent runs past the end of the image.
  build_image();
  put32(file49_record + 2, IMG_BLOCKS - 1);
  put32(file49_record + 10, 2 * BS);
  CHECK_EQ(mount_built(false), ISO9660_OK);
  CHECK_EQ(iso9660_find(&iso, "FILE49.TXT", &e), ISO9660_ERR_CORRUPT);
  iso9660_unmount(&iso);

  // A record crossing the end of its block: the last one of the root's
  // first block, made longer.
  build_image();
  uint8_t *last = file_records[42];
  CHECK_EQ((last - img) / BS, LBA_ROOT);
  CHECK((last - img) % BS + 60 > BS);
  CHECK_EQ(file_records[43] - img, (LBA_ROOT + 1) * BS);
  last[0] = 60;
  CHECK_EQ(mount_built(false), ISO9660_OK);
  CHECK_EQ(iso9660_opendir_root(&iso, &dir), ISO9660_OK);
  int rc;
  while ((rc = iso9660_readdir(&dir, &e)) == 1) {
  }
  CHECK_EQ(rc, ISO9660_ERR_CORRUPT);
  iso9660_unmount(&iso);

  // A name longer than its record.
  build_image();
  file49_record[32] = 200;
  CHECK_EQ(mount_built(false), ISO9660_OK);
  CHECK_EQ(iso9660_find(&iso, "S01.MPG", &e), ISO9660_ERR_CORRUPT);
  iso9660_unmount(&iso);

  // A card that fails while the descriptors are read.
  build_image();
  mem_files_reset();
  mem_file_add("/DLAIR/TEST.ISO", img, sizeof(img));
  fail_reads = 1;
  CHECK_EQ(iso9660_mount(&iso, "/DLAIR/TEST.ISO", false), ISO9660_ERR_READ);
  fail_reads = 0;
}

// --- The real image, when DL_ISO names it -----------------------------------

static int is_scene_clip(const char *name) {
  size_t n = strlen(name);
  return n >= 7 && name[0] == 'S' && name[1] >= '0' && name[1] <= '9' &&
         name[2] >= '0' && name[2] <= '9' &&
         strcmp(name + n - 4, ".MPG") == 0;
}

static void test_real_image(void) {
  const char *path = getenv("DL_ISO");
  if (path == NULL || path[0] == '\0') {
    printf("DL_ISO not set: the real image is skipped\n");
    return;
  }
  int fd = open(path, O_RDONLY);
  struct stat st;
  CHECK(fd >= 0 && fstat(fd, &st) == 0);
  if (fd < 0) {
    return;
  }
  void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(map != MAP_FAILED);
  if (map == MAP_FAILED) {
    close(fd);
    return;
  }
  for (int joliet = 0; joliet <= 1; joliet++) {
    mem_files_reset();
    mem_file_add("/DLAIR/DL_CDROM_V31.ISO", map, (FSIZE_t)st.st_size);
    CHECK_EQ(iso9660_mount(&iso, "/DLAIR/DL_CDROM_V31.ISO", joliet),
             ISO9660_OK);
    CHECK(strcmp(iso.volume_id, "DL_CDROM_V31") == 0);
    CHECK_EQ(iso.joliet, joliet);
    // The file is 152 blocks longer than the volume (padding at the end).
    CHECK((uint64_t)iso.volume_blocks * BS <= (uint64_t)st.st_size);

    iso9660_dir_t dir;
    iso9660_entry_t e;
    CHECK_EQ(iso9660_opendir_root(&iso, &dir), ISO9660_OK);
    int entries = 0, clips = 0, rc;
    uint64_t clip_bytes = 0;
    while ((rc = iso9660_readdir(&dir, &e)) == 1) {
      entries++;
      if (is_scene_clip(e.name)) {
        clips++;
        clip_bytes += e.size;
      }
    }
    CHECK_EQ(rc, 0);
    CHECK_EQ(entries, 235);
    CHECK_EQ(clips, 194);
    CHECK_EQ(clip_bytes, 190372656);

    iso9660_file_t f;
    CHECK_EQ(iso9660_fopen(&iso, &f, "S01.MPG"), ISO9660_OK);
    CHECK_EQ(iso9660_fsize(&f), 6605532);
    static uint8_t buf[32768];
    uint32_t crc = 0;
    UINT got;
    do {
      CHECK_EQ(iso9660_fread(&f, buf, sizeof(buf), &got), ISO9660_OK);
      crc = crc32_update(crc, buf, got);
    } while (got == sizeof(buf));
    CHECK_EQ(crc, 0xD9658DDFu);
    printf("DL_ISO %s: %d entries, %d scene clips, %llu bytes, S01.MPG "
           "CRC-32 %08X\n",
           joliet ? "(Joliet)" : "(ISO)", entries, clips,
           (unsigned long long)clip_bytes, (unsigned)crc);
    iso9660_unmount(&iso);
  }
  munmap(map, (size_t)st.st_size);
  close(fd);
}

static void test_crc32(void) {
  CHECK_EQ(crc32_update(0, "123456789", 9), 0xCBF43926u);
  uint32_t c = crc32_update(0, "1234", 4);
  CHECK_EQ(crc32_update(c, "56789", 5), 0xCBF43926u);
  CHECK_EQ(crc32_update(0, "", 0), 0);
}

int main(void) {
  test_crc32();
  test_mount();
  test_list_root();
  test_find();
  test_read();
  test_joliet();
  test_refusals();
  test_real_image();
  TEST_END();
}
