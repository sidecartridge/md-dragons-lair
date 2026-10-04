/**
 * File: dlweb.c
 * Description: The converter in a browser or in Node: the cartridge's own
 *              conversion job (convjob.c) over the JavaScript host's files,
 *              so the clip files are the bytes the cartridge writes.
 *
 * The host gives the module, when it creates it:
 *   dlSize()            the CD-ROM image's size in bytes
 *   dlRead(pos, len)    a Uint8Array of the image's bytes from `pos` (fewer
 *                       only at its end)
 *   dlWrite(pos, bytes) the clip file's bytes at `pos`: a view of the
 *                       module's memory, to copy; 0, or nonzero on an error
 * and calls dl_mount(), then for a clip dl_convert_start() and
 * dl_convert_step() until it returns 0 (the file complete) or less (an
 * error). One clip at a time per module: a worker each for several.
 */

#include <emscripten.h>
#include <string.h>

#include "convjob.h"
#include "ff.h"
#include "iso9660.h"

EM_JS(double, js_size, (void), { return Module.dlSize(); });

EM_JS(int, js_read, (double pos, uint8_t *dst, uint32_t len), {
  var data = Module.dlRead(pos, len);
  HEAPU8.set(data, dst);
  return data.length;
});

EM_JS(int, js_write, (double pos, const uint8_t *src, uint32_t len), {
  return Module.dlWrite(pos, HEAPU8.subarray(src, src + len));
});

// --- FatFs over the host ------------------------------------------------------
//
// The image is read through a cache of a few large blocks: the converter's
// picture and sound demultiplexers read the same clip at two places, in
// small pieces, and every call into JavaScript costs.

#define CACHE_BLOCK (256u * 1024u)
#define CACHE_SLOTS 2

static uint8_t s_cache[CACHE_SLOTS][CACHE_BLOCK];
static FSIZE_t s_cache_pos[CACHE_SLOTS];
static uint32_t s_cache_len[CACHE_SLOTS];
static uint32_t s_cache_used[CACHE_SLOTS];  // 0: empty
static uint32_t s_cache_tick;

// The slot holding the image's byte `pos`, read in when none does; -1 on an
// error or past the image's end.
static int cache_slot(FSIZE_t pos) {
  int lru = 0;
  for (int s = 0; s < CACHE_SLOTS; s++) {
    if (s_cache_used[s] != 0 && pos >= s_cache_pos[s] &&
        pos < s_cache_pos[s] + s_cache_len[s]) {
      s_cache_used[s] = ++s_cache_tick;
      return s;
    }
    if (s_cache_used[s] < s_cache_used[lru]) {
      lru = s;
    }
  }
  FSIZE_t block = pos - pos % CACHE_BLOCK;
  int got = js_read((double)block, s_cache[lru], CACHE_BLOCK);
  if (got <= 0 || block + (FSIZE_t)got <= pos) {
    s_cache_used[lru] = 0;
    return -1;
  }
  s_cache_pos[lru] = block;
  s_cache_len[lru] = (uint32_t)got;
  s_cache_used[lru] = ++s_cache_tick;
  return lru;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)path;  // the image, or the clip file the host has ready
  memset(fp, 0, sizeof(*fp));
  fp->writing = (mode & FA_WRITE) != 0;
  if (!fp->writing) {
    fp->size = (FSIZE_t)js_size();
    memset(s_cache_used, 0, sizeof(s_cache_used));
  }
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  uint8_t *out = (uint8_t *)buff;
  *br = 0;
  while (btr > 0 && fp->fptr < fp->size) {
    int s = cache_slot(fp->fptr);
    if (s < 0) {
      return FR_DISK_ERR;
    }
    uint32_t at = (uint32_t)(fp->fptr - s_cache_pos[s]);
    uint32_t n = s_cache_len[s] - at;
    n = n < btr ? n : btr;
    memcpy(out, s_cache[s] + at, n);
    out += n;
    btr -= n;
    *br += n;
    fp->fptr += n;
  }
  return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
  *bw = 0;
  if (!fp->writing || js_write((double)fp->fptr, buff, btw) != 0) {
    return FR_DISK_ERR;
  }
  fp->fptr += btw;
  fp->size = fp->fptr > fp->size ? fp->fptr : fp->size;
  *bw = btw;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  fp->fptr = ofs;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  (void)fp;
  return FR_OK;
}

// --- The image and its clips --------------------------------------------------

static iso9660_t s_iso;

// A scene clip, as the cartridge counts them: S<digit><digit>*.MPG.
static int is_scene_clip(const char *name) {
  size_t n = strlen(name);
  return n >= 7 && name[0] == 'S' && name[1] >= '0' && name[1] <= '9' &&
         name[2] >= '0' && name[2] <= '9' && strcmp(name + n - 4, ".MPG") == 0;
}

// Scene clip `index` of the image's root, in its order.
static int find_clip(int index, iso9660_entry_t *entry) {
  iso9660_dir_t dir;
  if (iso9660_opendir_root(&s_iso, &dir) != ISO9660_OK) {
    return 0;
  }
  int n = 0;
  while (iso9660_readdir(&dir, entry) == 1) {
    if (is_scene_clip(entry->name) && n++ == index) {
      return 1;
    }
  }
  return 0;
}

// Opens the image, with its Joliet names as the cartridge does. 0, or a
// negative iso9660 error.
EMSCRIPTEN_KEEPALIVE int dl_mount(void) {
  return iso9660_mount(&s_iso, "image", true);
}

// The image's scene clips.
EMSCRIPTEN_KEEPALIVE int dl_clip_count(void) {
  iso9660_dir_t dir;
  iso9660_entry_t entry;
  if (iso9660_opendir_root(&s_iso, &dir) != ISO9660_OK) {
    return 0;
  }
  int n = 0;
  while (iso9660_readdir(&dir, &entry) == 1) {
    n += is_scene_clip(entry.name);
  }
  return n;
}

static iso9660_entry_t s_named;

// Scene clip `index`'s name ("S01.MPG"), or NULL.
EMSCRIPTEN_KEEPALIVE const char *dl_clip_name(int index) {
  return find_clip(index, &s_named) ? s_named.name : NULL;
}

// Scene clip `index`'s size in bytes, or 0.
EMSCRIPTEN_KEEPALIVE uint32_t dl_clip_size(int index) {
  return find_clip(index, &s_named) ? s_named.size : 0u;
}

// --- The conversion -------------------------------------------------------------

static convjob_t s_job;
static mpeg1_t s_dec;
static mpeg_ps_t s_video;
static mpeg_ps_t s_audio;
static mp2_t s_mp2;
static clip_writer_t s_writer;
static uint8_t s_rows[MPEG1_MAX_SLOTS][MPEG1_SLOT_BYTES]
    __attribute__((aligned(4)));
static uint8_t s_ring_c[PICTURE16_RING_C_BYTES] __attribute__((aligned(4)));
static uint8_t s_lines_y[PICTURE16_LINES_Y_BYTES] __attribute__((aligned(4)));
static int8_t s_sound[CONVJOB_SOUND_BYTES];
static int16_t s_pcm[MP2_FRAME_SAMPLES];

// Starts converting scene clip `index` for a palette of `gun_bits` (3: an
// ST, 4: an STE); the clip file goes to dlWrite(). 0, or a negative error.
EMSCRIPTEN_KEEPALIVE int dl_convert_start(int index, int gun_bits) {
  iso9660_entry_t entry;
  if (!find_clip(index, &entry)) {
    return CONVJOB_ERR_OPEN;
  }
  convjob_memory_t m;
  memset(&m, 0, sizeof(m));
  for (unsigned i = 0; i < MPEG1_MAX_SLOTS; i++) {
    m.rows[i] = s_rows[i];
  }
  m.row_count = (int)MPEG1_MAX_SLOTS;
  m.ring_c = s_ring_c;
  m.lines_y = s_lines_y;
  m.dec = &s_dec;
  m.video = &s_video;
  m.audio = &s_audio;
  m.mp2 = &s_mp2;
  m.writer = &s_writer;
  m.sound = s_sound;
  m.pcm = s_pcm;
  return convjob_start(&s_job, &m, &s_iso, &entry, "clip", gun_bits, NULL,
                       NULL);
}

// The next picture: 1 while the clip goes on, 0 when its file is complete,
// or a negative error.
EMSCRIPTEN_KEEPALIVE int dl_convert_step(void) {
  return convjob_step(&s_job);
}

// How far the clip is read, in thousandths.
EMSCRIPTEN_KEEPALIVE int dl_convert_progress(void) {
  uint32_t size = s_job.video_file.size;
  return size ? (int)((uint64_t)s_job.video_file.pos * 1000u / size) : 0;
}

// The clip file's CRC-32 (its header's), once complete.
EMSCRIPTEN_KEEPALIVE uint32_t dl_convert_crc(void) {
  return s_writer.header.crc;
}
