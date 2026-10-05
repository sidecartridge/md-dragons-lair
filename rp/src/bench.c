/**
 * File: bench.c
 * Description: The bench screen: the CD-ROM image on the SD card, listed and
 *              read. See bench.h.
 */

#include "bench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "hardware/spi.h"
#include "hardware/structs/systick.h"
#include "cart_shared.h"
#include "constants.h"
#include "aconfig.h"
#include "audio.h"
#include "clipplay.h"
#include "convjob.h"
#include "fb_blit.h"
#include "game_table.h"
#include "gameui.h"
#include "iso9660.h"
#include "manifest.h"
#include "mp2_audio.h"
#include "mpeg1_video.h"
#include "mpeg_ps.h"
#include "palette.h"
#include "picture16.h"
#include "player.h"
#include "qrcodegen.h"
#include "pico/time.h"
#include "sdcard.h"
#include "st_session.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

// The clip read by the test, and what it must read back.
#define BENCH_CLIP "S01.MPG"
#define BENCH_CLIP_CRC32 0xD9658DDFu

// Each timed pass reads this much of the clip; the CRC pass reads all of it.
#define BENCH_PASS_BYTES (2u * 1024u * 1024u)
#define BENCH_FAST_KHZ 24000u  // the SD card's SPI limit (SDCARD_MAX_KHZ)
#define BENCH_SLICE_US 20000u  // reading per bench_frame() call

#define BENCH_CHUNKS 4
static const uint32_t bench_chunk_bytes[BENCH_CHUNKS] = {512u, 2048u, 8192u,
                                                         32768u};
#define BENCH_SPEEDS 2
#define BENCH_PASSES (BENCH_SPEEDS * BENCH_CHUNKS + 1)  // the last: CRC

// Screen: 40 x 25 characters of the 8x8 font.
#define COLS 40
#define LIST_ROW 10
#define LIST_ROWS 8

enum {
  C_BACK = 0,
  C_TEXT = 1,
  C_TITLE = 2,
  C_GOOD = 3,
  C_BAD = 4,
  C_DIM = 5,
  C_VALUE = 6,
};

static const uint16_t bench_palette[16] = {
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 6, 1),
    PALETTE_RGB(2, 7, 2), PALETTE_RGB(7, 2, 2), PALETTE_RGB(3, 3, 4),
    PALETTE_RGB(3, 6, 7), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7)};

// What the bench found, readable over SWD as well as on the screen.
typedef struct {
  uint32_t sd_start_ms;   // after bench_init()
  bool sd_ok;
  int sd_result;       // sdcard_initFilesystem()'s
  bool image_found;
  int image_result;   // iso9660_mount() of the last candidate
  uint32_t entries;   // in the image's root directory
  uint32_t clips;     // scene clips: S<digit><digit>*.MPG
  uint64_t clip_bytes;
  uint32_t spi_hz[BENCH_SPEEDS];               // the SPI clock of each speed
  uint32_t kbytes_per_s[BENCH_SPEEDS][BENCH_CHUNKS];  // 0: not run or failed
  uint32_t crc;
  uint32_t crc_kbytes_per_s;
  int crc_state;      // 0 not run, 1 match, -1 mismatch, -2 read error
  int read_error;     // the last iso9660 error of the test, 0 if none
} bench_results_t;

__attribute__((used)) bench_results_t benchResults;

static FATFS s_fs;  // static: f_mount keeps a pointer to it
static uint32_t s_boot_us;
static iso9660_t s_iso;
static char s_image_name[64];
static uint32_t s_configured_hz;
static uint32_t s_list_top;
static bool s_dirty = true;

static struct {
  bool pending;   // start once the screen says so
  bool running;
  bool redraw;    // a pass ended: show it before the next one
  int pass;       // 0..BENCH_PASSES-1
  iso9660_file_t file;
  uint32_t done;  // bytes of this pass
  uint64_t read_us;
  uint32_t crc;
} s_test;

// Without the image: a complete set of clips on the card (see "A set
// without the image").
static struct {
  bool checked;  // at this session's start
  bool ready;    // a complete set, of `gun_bits`
  bool stale;    // none, but a set of an older converter is there
  int gun_bits;
} s_set;

static void need_draw(void);
static void text2x(int x, int y, int color, const char *str);

// --- SD card clock ----------------------------------------------------------

static spi_inst_t *bench_spi(void) {
  sd_card_t *card = sd_get_by_num(0);
  if (card == NULL || card->spi_if_p == NULL || card->spi_if_p->spi == NULL) {
    return NULL;
  }
  return card->spi_if_p->spi->hw_inst;
}

// Sets the SD card's SPI clock now and for any later re-initialisation.
static uint32_t bench_set_spi_hz(uint32_t hz) {
  spi_inst_t *spi = bench_spi();
  if (spi == NULL) {
    return 0;
  }
  sd_get_by_num(0)->spi_if_p->spi->baud_rate = hz;
  return spi_set_baudrate(spi, hz);
}

// --- Finding the image ------------------------------------------------------

static bool is_scene_clip(const char *name) {
  size_t n = strlen(name);
  return n >= 7 && name[0] == 'S' && name[1] >= '0' && name[1] <= '9' &&
         name[2] >= '0' && name[2] <= '9' &&
         strcmp(name + n - 4, ".MPG") == 0;
}

// Mounts `name` in the folder; keeps it if it is the game's image.
static bool bench_try_image(const char *name) {
  char path[sizeof(BENCH_FOLDER) + 1 + 255];
  snprintf(path, sizeof(path), "%s/%s", BENCH_FOLDER, name);
  int result = iso9660_mount(&s_iso, path, true);
  benchResults.image_result = result;
  if (result != ISO9660_OK) {
    DPRINTF("%s: %s\n", path, iso9660_strerror(result));
    return false;
  }
  iso9660_entry_t entry;
  if (iso9660_find(&s_iso, BENCH_CLIP, &entry) != ISO9660_OK) {
    DPRINTF("%s: an ISO without %s\n", path, BENCH_CLIP);
    iso9660_unmount(&s_iso);
    return false;
  }
  snprintf(s_image_name, sizeof(s_image_name), "%s", name);
  return true;
}

static bool bench_find_image(void) {
  if (bench_try_image(BENCH_IMAGE_NAME)) {
    return true;
  }
  static DIR dir;
  static FILINFO info;
  if (f_opendir(&dir, BENCH_FOLDER) != FR_OK) {
    return false;
  }
  bool found = false;
  while (!found && f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
    if ((info.fattrib & AM_DIR) == 0 &&
        info.fsize >= 17u * ISO9660_SECTOR_SIZE &&
        strcmp(info.fname, BENCH_IMAGE_NAME) != 0) {
      found = bench_try_image(info.fname);
    }
  }
  f_closedir(&dir);
  return found;
}

static void bench_count_root(void) {
  iso9660_dir_t dir;
  iso9660_entry_t entry;
  benchResults.entries = 0;
  benchResults.clips = 0;
  benchResults.clip_bytes = 0;
  if (iso9660_opendir_root(&s_iso, &dir) != ISO9660_OK) {
    return;
  }
  int result;
  while ((result = iso9660_readdir(&dir, &entry)) == 1) {
    benchResults.entries++;
    if (is_scene_clip(entry.name)) {
      benchResults.clips++;
      benchResults.clip_bytes += entry.size;
    }
    DPRINTF("  %-32s %10lu%s\n", entry.name, (unsigned long)entry.size,
            entry.is_dir ? " <DIR>" : "");
  }
  if (result < 0) {
    DPRINTF("Root directory: %s\n", iso9660_strerror(result));
  }
}

// --- The read test ----------------------------------------------------------

static uint32_t bench_pass_chunk(int pass) {
  return (pass < BENCH_SPEEDS * BENCH_CHUNKS)
             ? bench_chunk_bytes[pass % BENCH_CHUNKS]
             : bench_chunk_bytes[BENCH_CHUNKS - 1];
}

static uint32_t bench_pass_limit(int pass) {
  uint32_t size = iso9660_fsize(&s_test.file);
  if (pass < BENCH_SPEEDS * BENCH_CHUNKS && size > BENCH_PASS_BYTES) {
    return BENCH_PASS_BYTES;
  }
  return size;
}

static void bench_start_pass(int pass) {
  s_test.pass = pass;
  s_test.done = 0;
  s_test.read_us = 0;
  s_test.crc = 0;
  iso9660_fseek(&s_test.file, 0);
  if (pass == 0) {
    benchResults.spi_hz[0] = bench_set_spi_hz(s_configured_hz);
  } else if (pass == BENCH_CHUNKS) {
    benchResults.spi_hz[1] = bench_set_spi_hz(BENCH_FAST_KHZ * 1000u);
  }
}

static void bench_end_test(void) {
  s_test.running = false;
  bench_set_spi_hz(s_configured_hz);
  s_dirty = true;
}

static void bench_start_test(void) {
  if (!benchResults.image_found || s_test.running) {
    return;
  }
  memset(benchResults.kbytes_per_s, 0, sizeof(benchResults.kbytes_per_s));
  benchResults.crc_state = 0;
  benchResults.crc_kbytes_per_s = 0;
  benchResults.read_error = 0;
  int result = iso9660_fopen(&s_iso, &s_test.file, BENCH_CLIP);
  if (result != ISO9660_OK) {
    benchResults.read_error = result;
    s_dirty = true;
    return;
  }
  s_test.pending = true;  // the screen says "running" before it starts
  s_dirty = true;
}

static void bench_finish_pass(void) {
  int pass = s_test.pass;
  uint32_t kbps = (s_test.read_us == 0)
                      ? 0
                      : (uint32_t)((uint64_t)s_test.done * 1000000u /
                                   s_test.read_us / 1024u);
  if (pass < BENCH_SPEEDS * BENCH_CHUNKS) {
    benchResults.kbytes_per_s[pass / BENCH_CHUNKS][pass % BENCH_CHUNKS] = kbps;
    DPRINTF("Read %lu B chunks at %lu Hz: %lu bytes in %lu us, %lu KB/s\n",
            (unsigned long)bench_pass_chunk(pass),
            (unsigned long)benchResults.spi_hz[pass / BENCH_CHUNKS],
            (unsigned long)s_test.done, (unsigned long)s_test.read_us,
            (unsigned long)kbps);
  } else {
    benchResults.crc = s_test.crc;
    benchResults.crc_kbytes_per_s = kbps;
    benchResults.crc_state = (s_test.crc == BENCH_CLIP_CRC32) ? 1 : -1;
    DPRINTF("CRC-32 of %s at %lu Hz: %08lX (%s), %lu KB/s\n", BENCH_CLIP,
            (unsigned long)benchResults.spi_hz[1], (unsigned long)s_test.crc,
            benchResults.crc_state == 1 ? "match" : "MISMATCH",
            (unsigned long)kbps);
  }
  if (pass + 1 < BENCH_PASSES) {
    bench_start_pass(pass + 1);
    s_test.redraw = true;
  } else {
    bench_end_test();
  }
}

// Reads for up to BENCH_SLICE_US into fb_chunked_buffer, which nothing draws
// into while the test runs (the ST keeps showing the last frame published).
static void bench_test_slice(void) {
  uint8_t *buf = fb_chunked_buffer;
  uint32_t chunk = bench_pass_chunk(s_test.pass);
  uint32_t limit = bench_pass_limit(s_test.pass);
  bool with_crc = s_test.pass >= BENCH_SPEEDS * BENCH_CHUNKS;
  uint32_t slice_end = time_us_32() + BENCH_SLICE_US;
  while ((int32_t)(time_us_32() - slice_end) < 0) {
    if (s_test.done >= limit) {
      bench_finish_pass();
      return;
    }
    UINT want = (limit - s_test.done < chunk) ? limit - s_test.done : chunk;
    UINT got = 0;
    uint32_t t0 = time_us_32();
    int result = iso9660_fread(&s_test.file, buf, want, &got);
    s_test.read_us += time_us_32() - t0;
    if (result != ISO9660_OK || got != want) {
      benchResults.read_error =
          (result != ISO9660_OK) ? result : ISO9660_ERR_TRUNCATED;
      if (with_crc) {
        benchResults.crc_state = -2;
      }
      DPRINTF("Read test: %s (FatFs %d) in pass %d\n",
              iso9660_strerror(benchResults.read_error), (int)s_iso.fres,
              s_test.pass);
      bench_end_test();
      return;
    }
    if (with_crc) {
      s_test.crc = crc32_update(s_test.crc, buf, got);
    }
    s_test.done += got;
  }
}

// --- The write test ---------------------------------------------------------
//
// The card's write rate, for a frame store on it (a B picture's second
// reference): a 2 MB scratch file in the folder written in 32 KB chunks, at
// the card's clock as it is created, then rewritten in place (as a reused
// store would be) at that clock and at BENCH_FAST_KHZ, then read back and
// deleted. Each pass ends with f_sync(), counted in its time. Results to the
// console; the data is whatever fb_chunked_buffer holds. The chunk can be
// set (a multiple of 512 up to 32 KB), for the rate of smaller writes.

#define WRITE_PATH BENCH_FOLDER "/BENCH.TMP"
#define WRITE_BYTES (2u * 1024u * 1024u)
#define WRITE_CHUNK 32768u
#define WRITE_PASSES 4

static const char *const write_pass_names[WRITE_PASSES] = {
    "created", "rewritten", "rewritten", "read back"};

static struct {
  bool active;
  int pass;
  FIL f;
  uint32_t chunk;
  uint32_t done;
  uint64_t us;
} s_write;

static void write_end(FRESULT fr) {
  if (fr != FR_OK) {
    DPRINTF("Write test: FatFs %d in pass %d\n", (int)fr, s_write.pass);
  }
  f_close(&s_write.f);
  f_unlink(WRITE_PATH);
  bench_set_spi_hz(s_configured_hz);
  s_write.active = false;
}

static FRESULT write_start_pass(int pass) {
  s_write.pass = pass;
  s_write.done = 0;
  s_write.us = 0;
  bench_set_spi_hz(pass >= 2 ? BENCH_FAST_KHZ * 1000u : s_configured_hz);
  if (pass == 0) {
    return f_open(&s_write.f, WRITE_PATH,
                  FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
  }
  return f_lseek(&s_write.f, 0);
}

static void write_start(uint32_t chunk) {
  if (!benchResults.sd_ok || s_write.active || s_test.running) {
    return;
  }
  s_write.chunk =
      (chunk >= 512u && chunk <= WRITE_CHUNK && chunk % 512u == 0) ? chunk
                                                                   : WRITE_CHUNK;
  s_write.active = true;
  FRESULT fr = write_start_pass(0);
  if (fr != FR_OK) {
    write_end(fr);
  }
}

static void write_slice(void) {
  uint32_t slice_end = time_us_32() + BENCH_SLICE_US;
  bool reading = s_write.pass == WRITE_PASSES - 1;
  while ((int32_t)(time_us_32() - slice_end) < 0) {
    FRESULT fr = FR_OK;
    UINT n = 0;
    uint32_t t0 = time_us_32();
    if (s_write.done < WRITE_BYTES) {
      fr = reading
               ? f_read(&s_write.f, fb_chunked_buffer, s_write.chunk, &n)
               : f_write(&s_write.f, fb_chunked_buffer, s_write.chunk, &n);
    } else if (!reading) {
      fr = f_sync(&s_write.f);
    }
    s_write.us += time_us_32() - t0;
    if (fr != FR_OK || (s_write.done < WRITE_BYTES && n != s_write.chunk)) {
      write_end(fr != FR_OK ? fr : FR_DISK_ERR);
      return;
    }
    if (s_write.done < WRITE_BYTES) {
      s_write.done += n;
      continue;
    }
    spi_inst_t *spi = bench_spi();
    DPRINTF("Write test: %lu bytes %s in %lu B chunks at %lu Hz: %llu us, "
            "%lu KB/s\n",
            (unsigned long)s_write.done, write_pass_names[s_write.pass],
            (unsigned long)s_write.chunk,
            (unsigned long)(spi != NULL ? spi_get_baudrate(spi) : 0),
            (unsigned long long)s_write.us,
            (unsigned long)(s_write.us ? (uint64_t)s_write.done * 1000000u /
                                             s_write.us / 1024u
                                       : 0));
    if (s_write.pass + 1 == WRITE_PASSES) {
      write_end(FR_OK);
      return;
    }
    fr = write_start_pass(s_write.pass + 1);
    if (fr != FR_OK) {
      write_end(fr);
      return;
    }
  }
}

// --- Drawing ----------------------------------------------------------------

static void text(int col, int row, int color, const char *str) {
  font_set_color((unsigned)color);
  font_move((unsigned)(col * 8), (unsigned)(row * 8));
  font_print(str);
}

static void textf(int col, int row, int color, const char *fmt, ...) {
  char buf[COLS + 1];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  text(col, row, color, buf);
}

static void rule(int row) {
  for (int x = 4; x < 316; x++) {
    fb_chunked_buffer[(row * 8 + 4) * FB_CHUNKED_W + x] = C_DIM;
  }
}

static void draw_listing(void) {
  iso9660_dir_t dir;
  iso9660_entry_t entry;
  if (iso9660_opendir_root(&s_iso, &dir) != ISO9660_OK) {
    return;
  }
  uint32_t index = 0;
  int row = 0;
  while (row < LIST_ROWS && iso9660_readdir(&dir, &entry) == 1) {
    if (index++ < s_list_top) {
      continue;
    }
    char name[28];
    snprintf(name, sizeof(name), "%s", entry.name);
    int color = is_scene_clip(entry.name) ? C_VALUE : C_TEXT;
    textf(0, LIST_ROW + row, C_DIM, "%3lu", (unsigned long)index);
    text(4, LIST_ROW + row, color, name);
    if (entry.is_dir) {
      text(31, LIST_ROW + row, C_DIM, "   <DIR>");
    } else {
      textf(30, LIST_ROW + row, C_TEXT, "%10lu", (unsigned long)entry.size);
    }
    row++;
  }
}

static void draw_results(void) {
  static const char *const labels[BENCH_CHUNKS] = {"512B", "  2K", "  8K",
                                                   " 32K"};
  text(0, 19, C_TITLE, "READ " BENCH_CLIP " KB/S");
  for (int c = 0; c < BENCH_CHUNKS; c++) {
    text(19 + c * 5, 19, C_DIM, labels[c]);
  }
  for (int s = 0; s < BENCH_SPEEDS; s++) {
    uint32_t hz = (benchResults.spi_hz[s] != 0)
                      ? benchResults.spi_hz[s]
                      : (s == 0 ? s_configured_hz : BENCH_FAST_KHZ * 1000u);
    textf(0, 20 + s, C_TEXT, "SPI %2lu.%01lu MHZ", (unsigned long)(hz / 1000000u),
          (unsigned long)((hz / 100000u) % 10u));
    for (int c = 0; c < BENCH_CHUNKS; c++) {
      int pass = s * BENCH_CHUNKS + c;
      uint32_t v = benchResults.kbytes_per_s[s][c];
      if (s_test.running && pass == s_test.pass) {
        text(19 + c * 5, 20 + s, C_TITLE, " ...");
      } else if (v != 0) {
        textf(19 + c * 5, 20 + s, C_VALUE, "%4lu", (unsigned long)v);
      } else {
        text(19 + c * 5, 20 + s, C_DIM, "   -");
      }
    }
  }
  if (s_test.running && s_test.pass == BENCH_PASSES - 1) {
    text(0, 22, C_TITLE, "CRC-32 ...");
  } else if (benchResults.crc_state == 1) {
    textf(0, 22, C_GOOD, "CRC-32 %08lX OK  %lu KB/S",
          (unsigned long)benchResults.crc,
          (unsigned long)benchResults.crc_kbytes_per_s);
  } else if (benchResults.crc_state == -1) {
    textf(0, 22, C_BAD, "CRC-32 %08lX BAD, WANT %08lX",
          (unsigned long)benchResults.crc, (unsigned long)BENCH_CLIP_CRC32);
  } else if (benchResults.read_error != 0) {
    textf(0, 22, C_BAD, "READ ERROR: %s",
          iso9660_strerror(benchResults.read_error));
  } else {
    text(0, 22, C_DIM, "CRC-32 -");
  }
}

static bool draw_sound_line(int row);

// The machine and the TOS the ST reported at its last boot, at the right of
// `row`. The machine byte does not tell a Mega ST from an ST.
static void draw_machine(int row) {
  if (st_session_hellos() == 0) {
    return;
  }
  const char *name;
  switch (st_session_machine()) {
    case ST_MACHINE_ST:
      name = "ST";
      break;
    case ST_MACHINE_STE:
      name = "STE";
      break;
    case ST_MACHINE_MEGASTE:
      name = "MEGA STE";
      break;
    case ST_MACHINE_TT:
      name = "TT";
      break;
    case ST_MACHINE_FALCON:
      name = "FALCON";
      break;
    default:
      name = "?";
      break;
  }
  uint16_t tos = st_session_tos_version();
  char buf[COLS + 1];
  int n = snprintf(buf, sizeof(buf), "%s  TOS %X.%02X", name,
                   (unsigned)(tos >> 8), (unsigned)(tos & 0xFFu));
  text(COLS - n, row, C_DIM, buf);
}

static void bench_draw(void) {
  if (benchResults.sd_ok && !benchResults.image_found && s_set.checked &&
      !s_set.ready) {
    need_draw();  // nothing to play: what to do
    return;
  }
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text(0, 0, C_TITLE, "DRAGON'S LAIR  SD + ISO BENCH");
  text(31, 0, C_DIM, RELEASE_VERSION);
  rule(1);

  if (benchResults.sd_result == SDCARD_CREATE_FOLDER_ERROR) {
    text(0, 2, C_BAD, "CANNOT CREATE THE FOLDER " BENCH_FOLDER);
  } else if (!benchResults.sd_ok) {
    text(0, 2, C_BAD, "NO SD CARD");
  } else {
    textf(0, 2, C_TEXT, "CARD    MOUNTED, SPI %lu KHZ",
          (unsigned long)(s_configured_hz / 1000u));
    textf(0, 3, C_TEXT, "FOLDER  %s", BENCH_FOLDER);
  }
  draw_machine(3);
  if (benchResults.sd_ok && !benchResults.image_found) {
    text(0, 4, C_BAD, "NO CD-ROM IMAGE IN THE FOLDER");
    text(0, 5, C_TEXT, "COPY DL_CDROM_V31.ISO INTO " BENCH_FOLDER);
    if (benchResults.image_result != ISO9660_OK) {
      textf(0, 6, C_DIM, "LAST TRIED: %s",
            iso9660_strerror(benchResults.image_result));
    }
    if (s_set.ready) {
      textf(0, 8, C_GOOD, "SET     %s, %lu CLIPS, COMPLETE",
            s_set.gun_bits == 4 ? "STE" : "ST",
            (unsigned long)benchResults.clips);
      text(8, 9, C_TEXT, "V: THE CLIPS");
    } else if (s_set.checked) {
      text(0, 8, C_BAD, "NO COMPLETE SET OF CLIPS EITHER");
    }
  }
  if (benchResults.image_found) {
    char name[33];
    snprintf(name, sizeof(name), "%s", s_image_name);
    text(0, 4, C_TEXT, "IMAGE");
    text(8, 4, C_VALUE, name);
    textf(8, 5, C_TEXT, "%llu B, %lu FRAGS, %s",
          (unsigned long long)s_iso.image_size,
          (unsigned long)s_iso.fragments,
          s_iso.fast_seek ? "FAST SEEK" : "SLOW SEEK");
    textf(0, 6, C_TEXT, "VOLUME  %s%s", s_iso.volume_id,
          s_iso.joliet ? "  JOLIET" : "");
    textf(0, 7, C_TEXT, "ROOT    %lu ENTRIES, %lu SCENE CLIPS",
          (unsigned long)benchResults.entries,
          (unsigned long)benchResults.clips);
    textf(8, 8, C_TEXT, "%llu BYTES OF CLIPS",
          (unsigned long long)benchResults.clip_bytes);
    rule(9);
    draw_listing();
    rule(18);
    draw_results();
  }
  rule(23);
  if (!draw_sound_line(24)) {
    text(0, 24, C_DIM,
         benchResults.image_found ? "R READ I/P PICTS A SND X BOOSTER ESC GEM"
         : s_set.ready            ? "V CLIPS  X BOOSTER  ESC GEM"
                                  : "X BOOSTER  ESC GEM");
  }
}

// --- Intra picture slideshow ----------------------------------------------------
//
// The intra (I) pictures of a scene clip, decoded from the image by
// mpeg1_video.c, scaled from 352x240 to 320x200 (Lanczos-3; the chroma
// planes to 160x100) and turned into 16 colours of their own with an ordered
// dither by picture16.c, with the time each step took. The colours are the
// machine's (512 on an ST, 4,096 on an STE); C switches them, W switches the
// palette's weighting, D the dither. P and B
// pictures are skipped. Every picture is held SHOW_HOLD_US so it can be seen.
//
// Memory: the luma goes to fb_chunked_buffer, where picture16 writes the
// colour indices over it; the chroma planes to the publish's planar scratch,
// read before fb_publish() uses it; the scaler's line rings (10 KB) to
// APP_FREE in the cartridge window, where the second core's histogram goes
// once a picture is scaled; picture16's 8 KB of work from the heap.

#define SHOW_HOLD_US 500000u
#define SHOW_TEXT_LINES 16  // two text rows over the picture
#define SHOW_CW (FB_CHUNKED_W / 2)
#define SHOW_CH (FB_CHUNKED_H / 2)

static mpeg_ps_t s_ps;
static mpeg1_t s_dec;

static struct {
  bool active;
  bool done;        // the clip has ended
  bool hold;        // a picture is on screen
  int clip_index;   // among the scene clips, in directory order
  char clip[16];
  iso9660_file_t file;
  uint32_t read_us;  // inside the demultiplexer's reads, since reset
  uint32_t rows_us;  // inside the row callback (scaling), since reset
  uint32_t shown_at;
  uint32_t pictures;
  bool text;         // the timings over the picture
  void *work;        // picture16's work memory
  picture16_palette_t palette;
  picture16_options_t options;
  picture16_profile_t profile;
  picture16_scaler_t scaler;  // its line rings in APP_FREE
  uint8_t ink;       // the palette's brightest entry
  uint8_t paper;     // its darkest
  // The last picture.
  uint32_t decode_us, idct_us, decode_read_us, skip_us, skip_read_us;
  // The clip so far.
  uint32_t convert_us;
  uint64_t sum_decode_us, sum_idct_us, sum_read_us, sum_rows_us,
      sum_skip_us, sum_skip_read_us, sum_convert_us;
} s_show;

// picture16's halves on both cores: core 1 through the framebuffer's
// dispatcher (idle while the bench converts), core 0 here.
static void bench_run2(picture16_job_fn job, void *a, void *b) {
  fb_core1_dispatch(job, a);
  job(b);
  fb_core1_wait();
}

// SysTick as a free-running 24-bit cycle counter, counting up.
static uint32_t bench_cycles(void) { return ~systick_hw->cvr & 0xFFFFFFu; }

#define CYCLES_PER_US (RP2040_CLOCK_FREQ_KHZ / 1000u)

static void bench_start_cycles(void) {
  systick_hw->rvr = 0xFFFFFFu;
  systick_hw->cvr = 0;
  systick_hw->csr = 0x5u;  // enabled, the processor's clock, no interrupt
}

static uint16_t s_show_words[16];

static const uint16_t *show_palette(void) {
  for (int i = 0; i < 16; i++) {
    s_show_words[i] = picture16_ste_word(s_show.palette.rgb444[i]);
  }
  return s_show_words;
}

static int show_read(void *ctx, uint8_t *buf, uint32_t len) {
  (void)ctx;
  UINT got = 0;
  uint32_t t0 = time_us_32();
  int result = iso9660_fread(&s_show.file, buf, len, &got);
  s_show.read_us += time_us_32() - t0;
  return (result == ISO9660_OK) ? (int)got : -1;
}

// A decoded macroblock row: luma into fb_chunked_buffer, chroma into the
// planar scratch, scaled by picture16.
static void show_row(void *ctx, int mb_row, const uint8_t *y,
                     const uint8_t *cb, const uint8_t *cr, int stride) {
  (void)ctx;
  uint32_t t0 = time_us_32();
  picture16_scaler_mb_row(&s_show.scaler, mb_row, y, cb, cr, stride);
  s_show.rows_us += time_us_32() - t0;
}

// The `index`-th scene clip of the image's root directory.
static bool show_find_clip(int index, iso9660_entry_t *entry) {
  iso9660_dir_t dir;
  if (iso9660_opendir_root(&s_iso, &dir) != ISO9660_OK) {
    return false;
  }
  int n = 0;
  while (iso9660_readdir(&dir, entry) == 1) {
    if (is_scene_clip(entry->name) && n++ == index) {
      return true;
    }
  }
  return false;
}

static void show_start(int index) {
  if (!benchResults.image_found || s_test.running || s_test.pending) {
    return;
  }
  if (index < 0) {
    index = (int)benchResults.clips - 1;
  }
  if (index >= (int)benchResults.clips) {
    index = 0;
  }
  iso9660_entry_t entry;
  iso9660_file_t file;
  if (!show_find_clip(index, &entry) ||
      iso9660_fopen_entry(&s_iso, &file, &entry) != ISO9660_OK) {
    return;
  }
  void *work = s_show.work;
  memset(&s_show, 0, sizeof(s_show));
  s_show.work = (work != NULL) ? work : malloc(PICTURE16_WORK_BYTES);
  if (s_show.work == NULL) {
    return;
  }
  s_show.file = file;
  s_show.active = true;
  s_show.text = true;
  s_show.profile.cycles = bench_cycles;
  // The machine's colours: 512 on an ST (family 0 of the hello's machine
  // byte), 4,096 on the STE, the TT and the Falcon.
  s_show.options.gun_bits = (st_session_machine() >> 4) == 0 ? 3 : 4;
  s_show.options.weighting = PICTURE16_WEIGHT_SQRT;
  s_show.options.dither = PICTURE16_DITHER_MIX;
  s_show.options.run2 = bench_run2;
  // The scaler's rings are idle while a picture is converted: the second
  // core's histogram.
  s_show.options.work2 =
      (uint8_t *)__rom_in_ram_start__ + CART_APP_FREE_OFFSET;
  s_show.clip_index = index;
  snprintf(s_show.clip, sizeof(s_show.clip), "%s", entry.name);
  mpeg_ps_init(&s_ps, show_read, NULL);
  bench_start_cycles();
  s_dec.cycles = bench_cycles;
  s_dec.run2 = bench_run2;
  mpeg1_init(&s_dec, &s_ps);
  fb_chunked_clear(0);
  DPRINTF("Slideshow: %s, %lu bytes\n", s_show.clip,
          (unsigned long)entry.size);
}

static void show_stop(void) {
  s_show.active = false;
  free(s_show.work);
  s_show.work = NULL;
  palette_set(bench_palette);
  s_dirty = true;
}

// The dither's name as `dlconv preview` writes it.
static const char *show_dither_name(int dither) {
  static const char *const names[PICTURE16_DITHERS] = {"bayer", "soft",
                                                       "mix", "none"};
  return (dither >= 0 && dither < PICTURE16_DITHERS) ? names[dither] : "?";
}

static void show_draw_text(void) {
  if (!s_show.text) {
    return;
  }
  memset(fb_chunked_buffer, s_show.paper, SHOW_TEXT_LINES * FB_CHUNKED_W);
  font_set_font(&font8x8);
  uint32_t n = s_show.pictures;
  if (s_show.done) {
    textf(0, 0, s_show.ink, "%s END: %lu I PICTURES", s_show.clip,
          (unsigned long)n);
    if (n > 0) {
      uint64_t pure = s_show.sum_decode_us - s_show.sum_read_us -
                      s_show.sum_rows_us;
      textf(0, 1, s_show.ink, "AVG DECODE %lu MS CONVERT %lu MS",
            (unsigned long)(pure / n / 1000u),
            (unsigned long)(s_show.sum_convert_us / n / 1000u));
    }
    return;
  }
  uint32_t pure = s_show.decode_us - s_show.decode_read_us - s_show.rows_us;
  textf(0, 0, s_show.ink, "%s I#%lu DEC %lu.%lu CONV %lu.%lu MS",
        s_show.clip, (unsigned long)n, (unsigned long)(pure / 1000u),
        (unsigned long)(pure / 100u % 10u),
        (unsigned long)(s_show.convert_us / 1000u),
        (unsigned long)(s_show.convert_us / 100u % 10u));
  const picture16_profile_t *pr = &s_show.profile;
  textf(0, 1, s_show.ink, "%s %s %s H%lu P%lu T%lu D%lu SC%lu",
        s_show.options.gun_bits == 3 ? "512" : "4096",
        s_show.options.weighting ? "SQRT" : "PIX",
        s_show.options.dither == PICTURE16_DITHER_BAYER        ? "BAYER"
        : s_show.options.dither == PICTURE16_DITHER_BAYER_SOFT ? "SOFT"
        : s_show.options.dither == PICTURE16_DITHER_MIX        ? "MIX"
                                                               : "FLAT",
        (unsigned long)(pr->histogram / CYCLES_PER_US / 100u),
        (unsigned long)(pr->palette / CYCLES_PER_US / 100u),
        (unsigned long)(pr->table / CYCLES_PER_US / 100u),
        (unsigned long)(pr->dither / CYCLES_PER_US / 100u),
        (unsigned long)(s_show.rows_us / 100u));
}

// The palette's darkest and brightest entries, for the text.
static void show_pick_ink(void) {
  int lo = 1 << 30, hi = -1;
  for (int i = 0; i < 16; i++) {
    uint16_t c = s_show.palette.rgb444[i];
    int l = 3 * ((c >> 8) & 15) + 6 * ((c >> 4) & 15) + (c & 15);
    if (l < lo) {
      lo = l;
      s_show.paper = (uint8_t)i;
    }
    if (l > hi) {
      hi = l;
      s_show.ink = (uint8_t)i;
    }
  }
}

// Shows the next intra picture once the current one has been held.
static void show_frame(void) {
  if (s_show.done ||
      (s_show.hold && time_us_32() - s_show.shown_at < SHOW_HOLD_US)) {
    fb_publish();
    return;
  }
  // Skip to the next I picture: the P and B pictures are only scanned.
  s_show.read_us = 0;
  uint32_t t0 = time_us_32();
  int type;
  while ((type = mpeg1_next_picture(&s_dec)) > 0 &&
         type != MPEG1_PICTURE_I) {
    mpeg1_skip_picture(&s_dec);
  }
  s_show.skip_us = time_us_32() - t0;
  s_show.skip_read_us = s_show.read_us;
  if (type <= 0) {
    s_show.done = true;
    show_draw_text();
    DPRINTF("Slideshow %s: %lu I pictures, decode %llu us, IDCT %llu us, "
            "read %llu us, rows %llu us, skip %llu us (read %llu us); "
            "errors %lu, end %d\n",
            s_show.clip, (unsigned long)s_show.pictures,
            (unsigned long long)s_show.sum_decode_us,
            (unsigned long long)s_show.sum_idct_us,
            (unsigned long long)s_show.sum_read_us,
            (unsigned long long)s_show.sum_rows_us,
            (unsigned long long)s_show.sum_skip_us,
            (unsigned long long)s_show.sum_skip_read_us,
            (unsigned long)s_dec.stats.errors, type);
    fb_publish();
    return;
  }
  s_show.read_us = 0;
  s_show.rows_us = 0;
  uint8_t *chroma = fb_chunked_scratch();
  picture16_scaler_init(&s_show.scaler, s_dec.width, s_dec.height,
                        (uint8_t *)__rom_in_ram_start__ + CART_APP_FREE_OFFSET,
                        fb_chunked_buffer, chroma, chroma + SHOW_CW * SHOW_CH,
                        FB_CHUNKED_W, FB_CHUNKED_H);
  s_show.scaler.run2 = bench_run2;
  uint64_t idct0 = s_dec.stats.idct_cycles;
  t0 = time_us_32();
  mpeg1_decode_picture(&s_dec, show_row, NULL);
  s_show.decode_us = time_us_32() - t0;
  s_show.idct_us =
      (uint32_t)((s_dec.stats.idct_cycles - idct0) / CYCLES_PER_US);
  s_show.decode_read_us = s_show.read_us;
  uint8_t *cb = fb_chunked_scratch();
  // CRC-32s to compare with `dlconv preview` on a PC: the scaled planes,
  // then the indices and the palette.
  uint32_t scaled_crc =
      crc32_update(0, fb_chunked_buffer, FB_CHUNKED_W * FB_CHUNKED_H);
  scaled_crc = crc32_update(scaled_crc, cb, 2u * SHOW_CW * SHOW_CH);
  t0 = time_us_32();
  picture16_convert(fb_chunked_buffer, cb, cb + SHOW_CW * SHOW_CH,
                    FB_CHUNKED_W, FB_CHUNKED_H, s_show.work, &s_show.options,
                    &s_show.palette, &s_show.profile);
  s_show.convert_us = time_us_32() - t0;
  uint32_t picture_crc =
      crc32_update(0, fb_chunked_buffer, FB_CHUNKED_W * FB_CHUNKED_H);
  picture_crc = crc32_update(picture_crc, s_show.palette.rgb444,
                             sizeof(s_show.palette.rgb444));
  s_show.sum_convert_us += s_show.convert_us;
  show_pick_ink();
  palette_set(show_palette());
  s_show.pictures++;
  s_show.sum_decode_us += s_show.decode_us;
  s_show.sum_idct_us += s_show.idct_us;
  s_show.sum_read_us += s_show.decode_read_us;
  s_show.sum_rows_us += s_show.rows_us;
  s_show.sum_skip_us += s_show.skip_us;
  s_show.sum_skip_read_us += s_show.skip_read_us;
  DPRINTF("%s I#%lu: decode %lu us (IDCT %lu, read %lu, rows %lu), "
          "skip %lu us (read %lu), convert %lu us (hist %lu, palette %lu "
          "(refine %lu), table %lu, dither %lu cycles), %d colours; "
          "CRC scaled %08lX, "
          "%s %s %08lX\n",
          s_show.clip, (unsigned long)s_show.pictures,
          (unsigned long)s_show.decode_us, (unsigned long)s_show.idct_us,
          (unsigned long)s_show.decode_read_us,
          (unsigned long)s_show.rows_us, (unsigned long)s_show.skip_us,
          (unsigned long)s_show.skip_read_us,
          (unsigned long)s_show.convert_us,
          (unsigned long)s_show.profile.histogram,
          (unsigned long)s_show.profile.palette,
          (unsigned long)s_show.profile.refine,
          (unsigned long)s_show.profile.table,
          (unsigned long)s_show.profile.dither, s_show.palette.colours,
          (unsigned long)scaled_crc,
          s_show.options.gun_bits == 4 ? "ste" : "st",
          show_dither_name(s_show.options.dither),
          (unsigned long)picture_crc);
  show_draw_text();
  fb_publish();
  s_show.hold = true;
  s_show.shown_at = time_us_32();
}

// --- I and P pictures, decoded in place -----------------------------------------
//
// A whole scene clip's I and P pictures (B pictures skipped), with the
// reference held in 17 macroblock rows of 8,448 bytes lent for the run:
// 7 in fb_chunked_buffer, 3 in the publish's planar scratch, 3 in the
// cartridge window's framebuffer, 1 in APP_FREE, the decoder's own row and
// 2 from the heap. Nothing is published while a reference lives in them;
// before each I picture the old reference is dead, and a progress screen is
// published and shown first. Every picture's CRC-32 (its rows' Y, Cb and Cr
// in order) goes to the console; the clip's CRC-32 (of those CRCs) to the
// screen, to compare with `dlconv ipcrc` on a PC.

#define IP_SLOTS 17
#define IP_SHOWN_TIMEOUT_US 200000u

static struct {
  bool active;
  bool done;
  int clip_index;
  char clip[16];
  iso9660_file_t file;
  uint8_t *heap[2];
  uint32_t read_us;  // inside the demultiplexer's reads, since reset
  uint32_t row_us;   // inside the row callback (the CRC), since reset
  uint32_t picture_crc;
  uint32_t clip_crc;
  uint32_t count[4];  // pictures by type: decoded I, P; skipped B
  uint64_t us[4];     // decode time by type, reads and callback excluded
  uint64_t idct_cycles[4];
  uint64_t mc_cycles[4];
  uint64_t read_total_us[4];
  uint32_t started_at;
  uint32_t total_us;
  int result;
} s_ip;

static int ip_read(void *ctx, uint8_t *buf, uint32_t len) {
  (void)ctx;
  UINT got = 0;
  uint32_t t0 = time_us_32();
  int result = iso9660_fread(&s_ip.file, buf, len, &got);
  s_ip.read_us += time_us_32() - t0;
  return (result == ISO9660_OK) ? (int)got : -1;
}

static void ip_row(void *ctx, int mb_row, const uint8_t *y, const uint8_t *cb,
                   const uint8_t *cr, int stride) {
  (void)ctx;
  (void)mb_row;
  uint32_t t0 = time_us_32();
  s_ip.picture_crc = crc32_update(s_ip.picture_crc, y, (size_t)stride * 16);
  s_ip.picture_crc = crc32_update(s_ip.picture_crc, cb, (size_t)stride * 4);
  s_ip.picture_crc = crc32_update(s_ip.picture_crc, cr, (size_t)stride * 4);
  s_ip.row_us += time_us_32() - t0;
}

static void ip_release_heap(void) {
  for (int i = 0; i < 2; i++) {
    free(s_ip.heap[i]);
    s_ip.heap[i] = NULL;
  }
}

static void ip_draw_progress(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text(0, 0, C_TITLE, "I+P PICTURES, DECODED IN PLACE");
  textf(0, 2, C_TEXT, "CLIP    %s", s_ip.clip);
  uint32_t n = s_ip.count[MPEG1_PICTURE_I] + s_ip.count[MPEG1_PICTURE_P];
  textf(0, 3, C_TEXT, "DONE    %lu I, %lu P, %lu B SKIPPED",
        (unsigned long)s_ip.count[MPEG1_PICTURE_I],
        (unsigned long)s_ip.count[MPEG1_PICTURE_P],
        (unsigned long)s_ip.count[MPEG1_PICTURE_B]);
  for (int t = MPEG1_PICTURE_I; t <= MPEG1_PICTURE_P; t++) {
    uint32_t c = s_ip.count[t];
    uint32_t avg = c ? (uint32_t)(s_ip.us[t] / c) : 0;
    uint32_t idct =
        c ? (uint32_t)(s_ip.idct_cycles[t] / c / CYCLES_PER_US) : 0;
    textf(0, 5 + t, C_VALUE, "%c       %lu.%lu MS, IDCT %lu.%lu MS",
          t == MPEG1_PICTURE_I ? 'I' : 'P', (unsigned long)(avg / 1000u),
          (unsigned long)(avg / 100u % 10u), (unsigned long)(idct / 1000u),
          (unsigned long)(idct / 100u % 10u));
  }
  uint32_t skip = s_ip.count[MPEG1_PICTURE_B]
                      ? (uint32_t)(s_ip.us[MPEG1_PICTURE_B] /
                                   s_ip.count[MPEG1_PICTURE_B])
                      : 0;
  textf(0, 8, C_VALUE, "B SKIP  %lu.%lu MS", (unsigned long)(skip / 1000u),
        (unsigned long)(skip / 100u % 10u));
  uint64_t reads = 0;
  for (int t = 1; t <= 3; t++) {
    reads += s_ip.read_total_us[t];
  }
  textf(0, 9, C_VALUE, "READS   %lu MS IN ALL",
        (unsigned long)(reads / 1000u));
  if (s_ip.done) {
    textf(0, 11, C_GOOD, "END     %lu PICTURES IN %lu.%lu S",
          (unsigned long)n, (unsigned long)(s_ip.total_us / 1000000u),
          (unsigned long)(s_ip.total_us / 100000u % 10u));
    textf(0, 12, C_GOOD, "CRC-32  %08lX", (unsigned long)s_ip.clip_crc);
    textf(0, 13, s_dec.stats.errors ? C_BAD : C_TEXT,
          "ERRORS  %lu SLICES, %lu CLAMPED VECTORS",
          (unsigned long)s_dec.stats.errors,
          (unsigned long)s_dec.stats.clamped_vectors);
    if (s_ip.result < 0) {
      textf(0, 14, C_BAD, "STOPPED: %d", s_ip.result);
    }
    text(0, 24, C_DIM, "SPACE: BACK");
  } else {
    text(0, 24, C_DIM, "RUNNING...");
  }
}

static void ip_finish(int result) {
  s_ip.done = true;
  s_ip.result = result;
  s_ip.total_us = time_us_32() - s_ip.started_at;
  ip_release_heap();
  DPRINTF("In place %s: %lu I (%llu us, IDCT %llu, MC %llu), %lu P (%llu "
          "us, IDCT %llu, MC %llu), %lu B skipped (%llu us), reads %llu us, "
          "total %lu us, CRC-32 %08lX, %lu slice errors, result %d\n",
          s_ip.clip, (unsigned long)s_ip.count[1],
          (unsigned long long)s_ip.us[1],
          (unsigned long long)(s_ip.idct_cycles[1] / CYCLES_PER_US),
          (unsigned long long)(s_ip.mc_cycles[1] / CYCLES_PER_US),
          (unsigned long)s_ip.count[2], (unsigned long long)s_ip.us[2],
          (unsigned long long)(s_ip.idct_cycles[2] / CYCLES_PER_US),
          (unsigned long long)(s_ip.mc_cycles[2] / CYCLES_PER_US),
          (unsigned long)s_ip.count[3],
          (unsigned long long)s_ip.us[3],
          (unsigned long long)(s_ip.read_total_us[1] + s_ip.read_total_us[2] +
                               s_ip.read_total_us[3]),
          (unsigned long)s_ip.total_us, (unsigned long)s_ip.clip_crc,
          (unsigned long)s_dec.stats.errors, result);
  palette_set(bench_palette);
}

static void ip_start(int index, bool profile) {
  if (!benchResults.image_found || s_test.running || s_test.pending ||
      s_show.active) {
    return;
  }
  if (index < 0) {
    index = (int)benchResults.clips - 1;
  }
  if (index >= (int)benchResults.clips) {
    index = 0;
  }
  iso9660_entry_t entry;
  iso9660_file_t file;
  if (!show_find_clip(index, &entry) ||
      iso9660_fopen_entry(&s_iso, &file, &entry) != ISO9660_OK) {
    return;
  }
  ip_release_heap();
  memset(&s_ip, 0, sizeof(s_ip));
  s_ip.file = file;
  s_ip.clip_index = index;
  snprintf(s_ip.clip, sizeof(s_ip.clip), "%s", entry.name);
  s_ip.heap[0] = malloc(MPEG1_SLOT_BYTES);
  s_ip.heap[1] = malloc(MPEG1_SLOT_BYTES);
  s_ip.active = true;
  if (s_ip.heap[0] == NULL || s_ip.heap[1] == NULL) {
    ip_finish(-100);
    return;
  }
  mpeg_ps_init(&s_ps, ip_read, NULL);
  // Profiling reads the cycle counter around every block and macroblock.
  bench_start_cycles();
  s_dec.cycles = profile ? bench_cycles : NULL;
  s_dec.run2 = bench_run2;
  mpeg1_init(&s_dec, &s_ps);
  uint8_t *window = (uint8_t *)__rom_in_ram_start__;
  uint8_t *cart_fb = window + CART_FRAMEBUFFER_OFFSET;
  uint8_t *scratch = fb_chunked_scratch();
  uint8_t *slots[IP_SLOTS];
  int n = 0;
  for (int i = 0; i < 7; i++) {
    slots[n++] = fb_chunked_buffer + i * MPEG1_SLOT_BYTES;
  }
  for (int i = 0; i < 3; i++) {
    slots[n++] = scratch + i * MPEG1_SLOT_BYTES;
  }
  for (int i = 0; i < 3; i++) {
    slots[n++] = cart_fb + i * MPEG1_SLOT_BYTES;
  }
  slots[n++] = window + CART_APP_FREE_OFFSET;
  slots[n++] = s_dec.own_row;
  slots[n++] = s_ip.heap[0];
  slots[n++] = s_ip.heap[1];
  mpeg1_set_slots(&s_dec, slots, n);
  s_ip.started_at = time_us_32();
  ip_draw_progress();
  fb_publish();
  fb_wait_shown(IP_SHOWN_TIMEOUT_US);
  DPRINTF("In place: %s, %lu bytes\n", s_ip.clip, (unsigned long)entry.size);
}

// One picture a call: the main loop runs between them.
static void ip_frame(void) {
  if (s_ip.done) {
    if (s_dirty) {
      ip_draw_progress();
      s_dirty = false;
    }
    fb_publish();
    return;
  }
  s_ip.read_us = 0;
  uint32_t t0 = time_us_32();
  int type = mpeg1_next_picture(&s_dec);
  if (type <= 0) {
    ip_finish(type);
    s_dirty = true;
    return;
  }
  if (type == MPEG1_PICTURE_I &&
      s_ip.count[MPEG1_PICTURE_I] + s_ip.count[MPEG1_PICTURE_P] > 0) {
    // The reference is dead until this picture is decoded: show progress
    // and wait until the ST has copied it, so that the window's
    // framebuffer can be written again.
    uint32_t t_pub = time_us_32();
    ip_draw_progress();
    fb_publish();
    fb_wait_shown(IP_SHOWN_TIMEOUT_US);
    t0 += time_us_32() - t_pub;
  }
  s_ip.picture_crc = 0;
  s_ip.row_us = 0;
  uint64_t idct0 = s_dec.stats.idct_cycles;
  uint64_t mc0 = s_dec.stats.mc_cycles;
  int result = mpeg1_decode_picture(&s_dec, ip_row, NULL);
  uint32_t spent = time_us_32() - t0;
  if (result < 0) {
    ip_finish(result);
    s_dirty = true;
    return;
  }
  int bucket = (result > 0) ? result : MPEG1_PICTURE_B;
  if (bucket > MPEG1_PICTURE_B) {
    bucket = MPEG1_PICTURE_B;
  }
  s_ip.count[bucket]++;
  s_ip.us[bucket] += spent - s_ip.read_us - s_ip.row_us;
  s_ip.idct_cycles[bucket] += s_dec.stats.idct_cycles - idct0;
  s_ip.mc_cycles[bucket] += s_dec.stats.mc_cycles - mc0;
  s_ip.read_total_us[bucket] += s_ip.read_us;
  if (result > 0) {
    s_ip.clip_crc = crc32_update(s_ip.clip_crc, &s_ip.picture_crc, 4);
    DPRINTF("%c %08lX %lu us\n", result == MPEG1_PICTURE_I ? 'I' : 'P',
            (unsigned long)s_ip.picture_crc,
            (unsigned long)(spent - s_ip.read_us - s_ip.row_us));
  }
}

static void ip_stop(void) {
  ip_release_heap();
  s_ip.active = false;
  palette_set(bench_palette);
  s_dirty = true;
}

// --- Sound ----------------------------------------------------------------------
//
// A scene clip's sound: its MP2 decoded by mp2_audio.c into mono at half the
// stream's rate (22,050 Hz). Timed: the whole clip decoded in slices of the
// main loop; the decoding's cycles (reads apart) and the CRC-32 of the
// 16-bit samples, to compare with `dlconv audio` on a PC, go to the console
// and the bench's bottom line. Played: decoded as it plays, from the PCM
// callback (audio.h), 8-bit, until the clip ends. Its demultiplexer and
// decoder take about 6 KB of heap while it runs.

#define SOUND_SLICE_US 15000u

typedef struct {
  iso9660_file_t file;
  mpeg_ps_t ps;
  mp2_t mp2;
  int16_t pcm[MP2_FRAME_SAMPLES];
  uint32_t pcm_pos;
  uint32_t pcm_len;
} sound_dec_t;

// The gain of the 8-bit step while a clip plays, in 3 dB steps (Q8).
static const int sound_gains[] = {256, 362, 512, 724, 1024, 1448, 2048, 2896};
#define SOUND_GAINS ((int)(sizeof(sound_gains) / sizeof(sound_gains[0])))
#define SOUND_GAIN_DEFAULT 3  // +9 dB
static int s_sound_gain = SOUND_GAIN_DEFAULT;

static struct {
  bool active;
  bool play;
  bool done;
  bool shown;        // on the bottom line
  int clip_index;
  char clip[16];
  uint32_t rate;     // of the samples, Hz
  sound_dec_t *d;
  uint32_t samples;
  uint64_t cycles;   // in mp2_decode_frame(), reads included
  uint64_t read_us;  // of which reading the image
  uint32_t crc;
  uint32_t started_at;
  uint32_t total_us;
} s_sound;

static int sound_read(void *ctx, uint8_t *buf, uint32_t len) {
  UINT got = 0;
  uint32_t t0 = time_us_32();
  int result = iso9660_fread((iso9660_file_t *)ctx, buf, len, &got);
  s_sound.read_us += time_us_32() - t0;
  return (result == ISO9660_OK) ? (int)got : -1;
}

// The next frame's samples into d->pcm; false at the end of the clip.
static bool sound_decode(void) {
  sound_dec_t *d = s_sound.d;
  uint32_t t0 = bench_cycles();
  int n = mp2_decode_frame(&d->mp2, d->pcm);
  s_sound.cycles += (bench_cycles() - t0) & 0xFFFFFFu;
  if (n <= 0) {
    return false;
  }
  s_sound.crc = crc32_update(s_sound.crc, d->pcm, (size_t)n * sizeof(int16_t));
  s_sound.samples += (uint32_t)n;
  s_sound.rate = (uint32_t)mp2_output_rate(&d->mp2);
  d->pcm_pos = 0;
  d->pcm_len = (uint32_t)n;
  return true;
}

static void sound_finish(void) {
  s_sound.done = true;
  s_sound.total_us = time_us_32() - s_sound.started_at;
  uint32_t rate = s_sound.rate;
  uint64_t pure = s_sound.cycles - s_sound.read_us * CYCLES_PER_US;
  DPRINTF("Sound %s: %lu frames, %lu samples at %lu Hz (%lu ms), decode "
          "%llu us (%llu us a second of sound), reads %llu us, total %lu us, "
          "CRC-32 %08lX\n",
          s_sound.clip, (unsigned long)s_sound.d->mp2.stats.frames,
          (unsigned long)s_sound.samples, (unsigned long)rate,
          (unsigned long)(rate ? s_sound.samples * 1000ull / rate : 0),
          (unsigned long long)(pure / CYCLES_PER_US),
          (unsigned long long)(s_sound.samples
                                   ? pure / CYCLES_PER_US * rate /
                                         s_sound.samples
                                   : 0),
          (unsigned long long)s_sound.read_us,
          (unsigned long)s_sound.total_us, (unsigned long)s_sound.crc);
  s_dirty = true;
}

static void sound_stop(void) {
  if (s_sound.play) {
    audio_set_fill_callback(NULL);
  }
  free(s_sound.d);
  s_sound.d = NULL;
  s_sound.active = false;
  s_dirty = true;
}

// The PCM callback while a clip plays: decoded as it goes, 8-bit through
// mp2_to_pcm8() at the chosen gain.
static void sound_pcm(int8_t *buf, uint32_t samples) {
  sound_dec_t *d = s_sound.d;
  uint32_t i = 0;
  while (i < samples) {
    if (d->pcm_pos == d->pcm_len && (s_sound.done || !sound_decode())) {
      if (!s_sound.done) {
        sound_finish();
      }
      memset(buf + i, 0, samples - i);
      return;
    }
    uint32_t n = d->pcm_len - d->pcm_pos;
    if (n > samples - i) {
      n = samples - i;
    }
    mp2_to_pcm8(d->pcm + d->pcm_pos, buf + i, n,
                sound_gains[s_sound_gain]);
    d->pcm_pos += n;
    i += n;
  }
}

static void sound_start(int index, bool play) {
  if (!benchResults.image_found || s_test.running || s_test.pending) {
    return;
  }
  if (s_sound.active) {
    sound_stop();
  }
  iso9660_entry_t entry;
  memset(&s_sound, 0, sizeof(s_sound));
  sound_dec_t *d = malloc(sizeof(sound_dec_t));
  if (d == NULL || !show_find_clip(index, &entry) ||
      iso9660_fopen_entry(&s_iso, &d->file, &entry) != ISO9660_OK) {
    free(d);
    return;
  }
  s_sound.d = d;
  s_sound.active = true;
  s_sound.shown = true;
  s_sound.play = play;
  s_sound.clip_index = index;
  snprintf(s_sound.clip, sizeof(s_sound.clip), "%s", entry.name);
  mpeg_ps_init_stream(&d->ps, sound_read, &d->file, MPEG_PS_AUDIO);
  mp2_init(&d->mp2, &d->ps);
  d->pcm_pos = d->pcm_len = 0;
  bench_start_cycles();
  s_sound.started_at = time_us_32();
  if (play) {
    // The first frame gives the rate.
    if (!sound_decode()) {
      sound_stop();
      return;
    }
    audio_set_pcm_callback(sound_pcm, (uint32_t)mp2_output_rate(&d->mp2));
  }
  DPRINTF("Sound: %s, %s\n", s_sound.clip, play ? "played" : "timed");
  s_dirty = true;
}

// The sound test's state on a text row; false when there is none to show.
static bool draw_sound_line(int row) {
  if (!s_sound.shown) {
    return false;
  }
  if (!s_sound.done) {
    if (s_sound.play) {
      textf(0, row, C_TITLE, "SOUND %s +%d DB  +/- <>", s_sound.clip,
            3 * s_sound_gain);
    } else {
      textf(0, row, C_TITLE, "SOUND %s DECODING...", s_sound.clip);
    }
    return true;
  }
  uint32_t tenths = s_sound.rate ? s_sound.samples * 10u / s_sound.rate : 0;
  textf(0, row, C_VALUE, "SOUND %s %lu.%luS CRC %08lX", s_sound.clip,
        (unsigned long)(tenths / 10u), (unsigned long)(tenths % 10u),
        (unsigned long)s_sound.crc);
  return true;
}

// Timed: frames for a slice of the main loop.
static void sound_slice(void) {
  uint32_t t0 = time_us_32();
  while (time_us_32() - t0 < SOUND_SLICE_US) {
    if (!sound_decode()) {
      sound_finish();
      sound_stop();
      return;
    }
  }
}

// --- Conversion -----------------------------------------------------------------
//
// A scene clip converted on the cartridge into the app's clip file
// (convjob.h), <clip>.DLC in BENCH_FOLDER/STE or /ST, for the palette of
// the machine plugged in (3 bits a gun for an ST, 4 for an STE, a TT or a
// Falcon). The frame store takes the in-place test's 17 rows; the
// converter's other buffers go where those leave room: the scaler's chroma
// ring and the sound after the rows in fb_planar_scratch, the scaled luma
// lines and the MP2 samples after the rows in the cartridge window's
// framebuffer, the MP2 decoder after its row in APP_FREE, the writer and the
// sound's demultiplexer after the rows in fb_chunked_buffer. The ST keeps the screen
// shown when the conversion started; its bar fills by changing the palette
// alone.

#define CONV_SEGMENTS 9  // the bar's segments: colours 7 to 15
#define CONV_FIRST_SEGMENT 7
#define ALIGN4(n) (((n) + 3u) & ~3u)

_Static_assert(3 * MPEG1_SLOT_BYTES + PICTURE16_RING_C_BYTES +
                       CONVJOB_SOUND_BYTES <=
                   CART_FRAMEBUFFER_SIZE,
               "fb_planar_scratch: 3 rows, the chroma ring and the sound");
_Static_assert(3 * MPEG1_SLOT_BYTES + PICTURE16_LINES_Y_BYTES +
                       MP2_FRAME_SAMPLES * sizeof(int16_t) <=
                   CART_FRAMEBUFFER_SIZE,
               "the window's framebuffer: 3 rows, the luma lines, MP2's");
_Static_assert(CART_APP_FREE_OFFSET + MPEG1_SLOT_BYTES + sizeof(mp2_t) <=
                   CART_FRAMEBUFFER_OFFSET,
               "APP_FREE: a row and the MP2 decoder");
_Static_assert(7 * MPEG1_SLOT_BYTES + ALIGN4(sizeof(clip_writer_t)) +
                       sizeof(mpeg_ps_t) <=
                   320 * 200,
               "fb_chunked_buffer: 7 rows, the writer and a demultiplexer");

// The whole game: every scene clip converted into BENCH_FOLDER/ST or
// BENCH_FOLDER/STE (the machine's palette), clip after clip; those already
// complete and current are passed over (conv_file_current()), so a run
// stopped anywhere carries on at the next start. It starts by itself when
// an ST says hello and a clip is missing (bench_restart()), or with C.
// While a clip converts only the palette moves on the screen (the clip's
// bar, the title's glow); the whole game's bar and the time left are drawn
// again between clips. One clip alone (the debug hook) shows its times.

// Before a clip of the run is done, the time left at the rate measured on
// the cartridge: 115 s for S01's 6.45 MB.
#define CONV_US_PER_KB 17800u

// How long a clip's screen is published again for an ST still starting.
#define CONV_SHOWN_TIMEOUT_US 6000000u

// The title's glow, a step a picture.
static const uint16_t conv_glow[] = {
    PALETTE_RGB(7, 6, 1), PALETTE_RGB(7, 7, 2), PALETTE_RGB(7, 7, 4),
    PALETTE_RGB(7, 7, 2), PALETTE_RGB(7, 6, 1), PALETTE_RGB(6, 5, 1),
    PALETTE_RGB(5, 4, 0), PALETTE_RGB(6, 5, 1)};
#define CONV_GLOWS ((int)(sizeof(conv_glow) / sizeof(conv_glow[0])))

static bool bench_busy(void);

static struct {
  bool active;
  bool done;
  bool all;       // the whole game, clip after clip
  bool checking;  // the clips being checked, before the run
  bool stopped;   // SPACE stopped the run
  int clip_index;
  char clip[16];
  char out_path[40];
  int gun_bits;
  convjob_t *job;     // on the heap while it runs
  uint8_t *rows;      // and two of the frame store's rows, in one block:
                      // newlib grows the heap in 4 KB steps from each
                      // request, and two of 8.4 KB take 24 KB
  int lit;  // segments lit
  int glow;
  int result;
  // The whole game's run.
  int total;       // scene clips
  int todo;        // to convert when the run began
  int converted;   // this run
  int failed;
  int ready;       // complete and current when the run began
  uint64_t total_bytes;  // the clips' sources
  uint64_t done_bytes;   // complete, converted or failed
  uint64_t run_bytes;    // converted or failed this run
  uint64_t out_bytes;    // written this run
  uint32_t clip_bytes;   // the clip converting
  uint64_t run_t0;       // 64 bits: a run can outlast time_us_32()'s 71 min
} s_cv;

// The last conversion, readable over SWD as well as on the screen.
typedef struct {
  int result;
  uint32_t gun_bits;
  uint32_t frames;
  uint32_t pictures;
  uint32_t keys;  // in the index
  uint32_t bytes;
  uint32_t source_bytes;
  uint32_t source_crc;
  uint32_t file_crc;
  uint32_t total_ms;
  uint32_t read_ms;
  uint32_t decode_ms;
  uint32_t convert_ms;  // what the others leave: scaling, histogram, ...
  uint32_t sound_ms;
  uint32_t encode_ms;
  uint32_t write_ms;
  uint32_t histogram_ms;  // of the conversion, from the cycle counter
  uint32_t palette_ms;
  uint32_t dither_ms;
} conv_results_t;

__attribute__((used)) conv_results_t convResults;

static const uint16_t *conv_palette(int lit) {
  static uint16_t words[16];
  memcpy(words, bench_palette, sizeof(words));
  words[C_TITLE] = conv_glow[s_cv.glow % CONV_GLOWS];
  for (int s = 0; s < CONV_SEGMENTS; s++) {
    words[CONV_FIRST_SEGMENT + s] =
        s < lit ? PALETTE_RGB(2, 7, 2) : PALETTE_RGB(1, 1, 2);
  }
  return words;
}

static int conv_machine_bits(void) {
  return (st_session_machine() >> 4) == 0 ? 3 : 4;  // an ST: 3 bits a gun
}

static const char *conv_folder(int gun_bits) {
  return gun_bits == 4 ? BENCH_FOLDER "/STE" : BENCH_FOLDER "/ST";
}

// `out`: the clip file of the scene clip `name` for a palette of `gun_bits`.
static void conv_path(char *out, size_t n, int gun_bits, const char *name) {
  char base[16];
  snprintf(base, sizeof(base), "%s", name);
  char *dot = strchr(base, '.');
  if (dot != NULL) {
    *dot = '\0';
  }
  snprintf(out, n, "%s/%s.DLC", conv_folder(gun_bits), base);
}

// Whether `path` holds the clip file the cartridge would write now for
// `entry`: complete (its index ends it; the header is written last), of the
// converter's version and stability, for `gun_bits`, of a source this size.
static bool conv_file_current(const char *path, const iso9660_entry_t *entry,
                              int gun_bits) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    return false;
  }
  uint8_t bytes[CLIP_HEADER_BYTES];
  UINT got = 0;
  clip_header_t h;
  bool ok = f_read(&f, bytes, sizeof(bytes), &got) == FR_OK &&
            got == sizeof(bytes) && clip_header_read(&h, bytes) == 0 &&
            h.converter == CONVERT_VERSION &&
            h.keep_percent == CONVERT_KEEP_PERCENT && h.gun_bits == gun_bits &&
            h.source_bytes == entry->size && h.frames > 0 &&
            h.index_count > 0 &&
            (FSIZE_t)h.index_offset + 8u * h.index_count == f_size(&f);
  f_close(&f);
  return ok;
}

// --- The set's manifest -------------------------------------------------------
//
// A set (the game's clips for an ST or an STE, in its folder) gets its
// manifest (manifest.h) once every clip is there: at the end of a run, or
// at a start that finds them all but no current manifest. A run that has
// clips to convert deletes the old one first. Without the image, the
// manifest is what tells a complete set.

static void manifest_path(char *out, size_t n, int gun_bits) {
  snprintf(out, n, "%s/%s", conv_folder(gun_bits), MANIFEST_FILE);
}

// Reads clip file `path`'s header and size with `f`. True when it reads.
static bool clip_file_header(FIL *f, const char *path, clip_header_t *h,
                             uint32_t *size) {
  if (f_open(f, path, FA_READ) != FR_OK) {
    return false;
  }
  uint8_t bytes[CLIP_HEADER_BYTES];
  UINT got = 0;
  bool ok = f_read(f, bytes, sizeof(bytes), &got) == FR_OK &&
            got == sizeof(bytes) && clip_header_read(h, bytes) == 0;
  *size = (uint32_t)f_size(f);
  f_close(f);
  return ok;
}

// The set's manifest header, when it reads and is this converter's for
// `gun_bits`.
static bool manifest_current(int gun_bits, manifest_header_t *h) {
  char path[40];
  manifest_path(path, sizeof(path), gun_bits);
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    return false;
  }
  uint8_t bytes[MANIFEST_HEADER_BYTES];
  UINT got = 0;
  bool ok = f_read(&f, bytes, sizeof(bytes), &got) == FR_OK &&
            got == sizeof(bytes) && manifest_header_read(h, bytes) == 0 &&
            h->converter == CONVERT_VERSION &&
            h->keep_percent == CONVERT_KEEP_PERCENT &&
            h->gun_bits == gun_bits;
  f_close(&f);
  return ok;
}

// Writes the set's manifest from its clip files, every scene clip of the
// image in its order, each file's header read back. 0, or -1 (a clip file
// missing or unreadable, or the card: then no manifest).
static int manifest_write_set(int gun_bits) {
  typedef struct {
    FIL out;
    FIL clip;
    uint8_t bytes[MANIFEST_ENTRY_BYTES];
  } manifest_mem_t;
  manifest_mem_t *m = malloc(sizeof(manifest_mem_t));
  if (m == NULL) {
    return -1;
  }
  uint32_t t0 = time_us_32();
  char path[40];
  manifest_path(path, sizeof(path), gun_bits);
  bool ok = f_open(&m->out, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
  if (!ok) {
    free(m);
    return -1;
  }
  uint8_t header[MANIFEST_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  UINT done = 0;
  ok = f_write(&m->out, header, sizeof(header), &done) == FR_OK;
  uint32_t crc = 0;
  uint16_t count = 0;
  iso9660_dir_t dir;
  iso9660_entry_t entry;
  ok = ok && iso9660_opendir_root(&s_iso, &dir) == ISO9660_OK;
  while (ok && iso9660_readdir(&dir, &entry) == 1) {
    if (!is_scene_clip(entry.name)) {
      continue;
    }
    char clip_path[40];
    conv_path(clip_path, sizeof(clip_path), gun_bits, entry.name);
    clip_header_t h;
    uint32_t size = 0;
    ok = clip_file_header(&m->clip, clip_path, &h, &size);
    if (ok) {
      manifest_entry_t e;
      manifest_entry_of(&e, entry.name, &h, size);
      manifest_entry_write(&e, m->bytes);
      crc = crc32_update(crc, m->bytes, MANIFEST_ENTRY_BYTES);
      ok = f_write(&m->out, m->bytes, MANIFEST_ENTRY_BYTES, &done) == FR_OK &&
           done == MANIFEST_ENTRY_BYTES;
      count++;
    }
  }
  if (ok) {
    manifest_header_t mh = {count, (uint8_t)gun_bits, CONVERT_VERSION,
                            CONVERT_KEEP_PERCENT, crc};
    manifest_header_write(&mh, header);
    ok = f_lseek(&m->out, 0) == FR_OK &&
         f_write(&m->out, header, sizeof(header), &done) == FR_OK &&
         done == sizeof(header);
  }
  ok = f_close(&m->out) == FR_OK && ok;
  free(m);
  if (!ok) {
    f_unlink(path);
  }
  DPRINTF("Manifest %s: %s, %u clips, entries CRC-32 %08lX, %lu ms\n", path,
          ok ? "written" : "failed", (unsigned)count, (unsigned long)crc,
          (unsigned long)((time_us_32() - t0) / 1000u));
  return ok ? 0 : -1;
}

// --- A set without the image ---------------------------------------------------
//
// With no CD-ROM image on the card, the clips come from a complete set
// alone (the web page's, or the cartridge's with the image gone): the
// machine's, else the other's, checked against its manifest at every start.
// The clip list and the player then take the clips' names from it.

// Whether the set of `gun_bits` is complete: its manifest current, its
// entries' CRC-32 right, and every clip file there as its entry says.
static bool set_complete(int gun_bits, uint16_t *count) {
  manifest_header_t mh;
  if (!manifest_current(gun_bits, &mh) || mh.count == 0) {
    return false;
  }
  typedef struct {
    FIL list;
    FIL clip;
  } set_mem_t;
  set_mem_t *m = malloc(sizeof(set_mem_t));
  if (m == NULL) {
    return false;
  }
  char path[40];
  manifest_path(path, sizeof(path), gun_bits);
  bool ok = f_open(&m->list, path, FA_READ) == FR_OK &&
            f_lseek(&m->list, MANIFEST_HEADER_BYTES) == FR_OK;
  uint32_t crc = 0;
  for (uint16_t i = 0; ok && i < mh.count; i++) {
    uint8_t bytes[MANIFEST_ENTRY_BYTES];
    UINT got = 0;
    manifest_entry_t e;
    ok = f_read(&m->list, bytes, sizeof(bytes), &got) == FR_OK &&
         got == sizeof(bytes) && manifest_entry_read(&e, bytes) == 0;
    if (ok) {
      crc = crc32_update(crc, bytes, sizeof(bytes));
      char clip_path[40];
      conv_path(clip_path, sizeof(clip_path), gun_bits, e.name);
      clip_header_t h;
      uint32_t size = 0;
      ok = clip_file_header(&m->clip, clip_path, &h, &size) &&
           manifest_matches(&mh, &e, &h, size);
    }
  }
  f_close(&m->list);
  free(m);
  *count = mh.count;
  return ok && crc == mh.entries_crc;
}

// Whether the set of `gun_bits` has a manifest of another converter.
static bool set_stale(int gun_bits) {
  char path[40];
  manifest_path(path, sizeof(path), gun_bits);
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    return false;
  }
  uint8_t bytes[MANIFEST_HEADER_BYTES];
  UINT got = 0;
  manifest_header_t h;
  bool stale = f_read(&f, bytes, sizeof(bytes), &got) == FR_OK &&
               got == sizeof(bytes) && manifest_header_read(&h, bytes) == 0 &&
               (h.converter != CONVERT_VERSION ||
                h.keep_percent != CONVERT_KEEP_PERCENT);
  f_close(&f);
  return stale;
}

// At a start with no image: the machine's set, else the other's.
static void set_check(void) {
  uint32_t t0 = time_us_32();
  int bits = conv_machine_bits();
  uint16_t count = 0;
  s_set.checked = true;
  s_set.ready = false;
  s_set.stale = false;
  for (int k = 0; k < 2 && !s_set.ready; k++, bits = 7 - bits) {
    if (set_complete(bits, &count)) {
      s_set.ready = true;
      s_set.gun_bits = bits;
      benchResults.clips = count;
    } else {
      s_set.stale |= set_stale(bits);
    }
  }
  DPRINTF("No image: %s set %s, %lu clips, %lu ms\n",
          s_set.ready ? (s_set.gun_bits == 4 ? "the STE" : "the ST") : "no",
          s_set.ready ? "complete" : "found",
          (unsigned long)(s_set.ready ? benchResults.clips : 0u),
          (unsigned long)((time_us_32() - t0) / 1000u));
  s_dirty = true;
}

// --- No clips on the card -------------------------------------------------------
//
// A start that finds neither the image nor a complete set (or only a set of
// an older converter) says what to do: copy the CD-ROM image into the
// folder (the cartridge converts it), or convert it on a computer with the
// web page, which the QR code opens for this converter's version and this
// machine's set.

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define WEB_CONVERTER_URL \
  "https://md-dragons-lair.sidecartridge.com/v" STRINGIFY(CONVERT_VERSION) "/"
#define QR_VERSION_MAX 4  // 33 modules: a URL of up to 62 bytes at ECC M
#define QR_MODULE_PX 3  // 123 pixels for 33 modules: the text keeps 22 columns
#define QR_QUIET 4  // modules of white around the code

static void need_draw(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text2x((320 - 16 * 13) / 2, 6, C_TITLE, "DRAGON'S LAIR");
  static const char *const missing[] = {"THE GAME'S CLIPS ARE",
                                        "NOT ON THE CARD YET."};
  static const char *const stale[] = {"THE CLIPS ON THE CARD",
                                      "ARE FOR AN OLDER", "VERSION."};
  static const char *const how[] = {
      "EITHER COPY YOUR",   "CD-ROM IMAGE INTO",  "/DLAIR ON THE CARD:",
      "THE CARTRIDGE TURNS", "IT INTO CLIPS IN",   "ABOUT AN HOUR.",
      "",                   "OR SCAN THE CODE,",  "OR GO TO",
      "MD-DRAGONS-LAIR.",   "SIDECARTRIDGE.COM,", "TO DO IT ON A",
      "COMPUTER IN A",      "MINUTE OR TWO."};
  int row = 4;
  int n = s_set.stale ? 3 : 2;
  for (int i = 0; i < n; i++) {
    text(1, row++, C_VALUE, s_set.stale ? stale[i] : missing[i]);
  }
  row++;
  for (unsigned i = 0; i < sizeof(how) / sizeof(how[0]); i++) {
    text(1, row++, C_TEXT, how[i]);
  }
  text(1, 23, C_DIM, "THEN SWITCH THE ST OFF AND ON.");
  text(0, 24, C_DIM, "X BOOSTER  ESC GEM");

  // The QR code, for this machine's set, black on white.
  char url[80];
  snprintf(url, sizeof(url), "%s?set=%s", WEB_CONVERTER_URL,
           conv_machine_bits() == 4 ? "ste" : "st");
  uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
  uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
  if (!qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM, 1,
                            QR_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
    return;
  }
  int size = qrcodegen_getSize(qr);
  int side = (size + 2 * QR_QUIET) * QR_MODULE_PX;
  int x0 = 316 - side;
  int y0 = 36;
  fb_fill_rect(x0, y0, side, side, C_TEXT);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcodegen_getModule(qr, x, y)) {
        fb_fill_rect(x0 + (QR_QUIET + x) * QR_MODULE_PX,
                     y0 + (QR_QUIET + y) * QR_MODULE_PX, QR_MODULE_PX,
                     QR_MODULE_PX, C_BACK);
      }
    }
  }
}

// Whether there are clips to list and play: the image's, or a set's.
static bool clips_available(void) {
  return benchResults.clips > 0 && (benchResults.image_found || s_set.ready);
}

// Scene clip `index`'s name ("S01.MPG"): from the image, else from the
// set's manifest.
static bool clip_name_at(int index, char name[MANIFEST_NAME_BYTES]) {
  if (benchResults.image_found) {
    iso9660_entry_t entry;
    if (!show_find_clip(index, &entry)) {
      return false;
    }
    snprintf(name, MANIFEST_NAME_BYTES, "%s", entry.name);
    return true;
  }
  if (!s_set.ready || index < 0 || index >= (int)benchResults.clips) {
    return false;
  }
  char path[40];
  manifest_path(path, sizeof(path), s_set.gun_bits);
  FIL f;
  uint8_t bytes[MANIFEST_ENTRY_BYTES];
  UINT got = 0;
  manifest_entry_t e;
  bool ok = f_open(&f, path, FA_READ) == FR_OK &&
            f_lseek(&f, MANIFEST_HEADER_BYTES +
                            (FSIZE_t)index * MANIFEST_ENTRY_BYTES) == FR_OK &&
            f_read(&f, bytes, sizeof(bytes), &got) == FR_OK &&
            got == sizeof(bytes) && manifest_entry_read(&e, bytes) == 0;
  f_close(&f);
  if (ok) {
    memcpy(name, e.name, MANIFEST_NAME_BYTES);
  }
  return ok;
}

// Text at twice the font's size, from (x, y) in pixels.
static void text2x(int x, int y, int color, const char *str) {
  for (; *str != '\0'; str++, x += 16) {
    int ch = (unsigned char)*str;
    if (ch < font8x8.first_char ||
        ch >= font8x8.first_char + font8x8.num_chars) {
      continue;
    }
    const unsigned char *glyph = &font8x8.data[(ch - font8x8.first_char) * 8];
    for (int r = 0; r < 8; r++) {
      for (int c = 0; c < 8; c++) {
        if (glyph[r] & (1u << c)) {
          fb_fill_rect(x + 2 * c, y + 2 * r, 2, 2, color);
        }
      }
    }
  }
}

static void text_centred(int row, int color, const char *str) {
  text((40 - (int)strlen(str)) / 2, row, color, str);
}

// Minutes, rounded up, of `us`.
static unsigned long conv_minutes(uint64_t us) {
  return (unsigned long)((us + 59999999u) / 60000000u);
}

// The whole game's screen: checking, converting a clip, or the end.
static void conv_draw_all(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text2x((320 - 16 * 13) / 2, 8, C_TITLE, "DRAGON'S LAIR");
  bool ste = s_cv.gun_bits == 4;
  text_centred(4, C_TEXT, ste ? "PREPARING THE GAME FOR AN STE"
                              : "PREPARING THE GAME FOR AN ST");
  text_centred(5, C_DIM, ste ? "4,096 COLOURS, ON THE SD CARD"
                             : "512 COLOURS, ON THE SD CARD");
  if (s_cv.checking) {
    text_centred(12, C_TEXT, "CHECKING THE CLIPS ON THE CARD");
    return;
  }
  uint64_t run_us = time_us_64() - s_cv.run_t0;
  if (s_cv.done) {
    if (s_cv.stopped) {
      text_centred(9, C_TEXT, "STOPPED");
      text_centred(11, C_DIM, "THE NEXT START CARRIES ON");
      text_centred(12, C_DIM, "WHERE IT STOPPED");
    } else if (s_cv.failed == 0) {
      text_centred(9, C_GOOD, "THE GAME IS READY");
    } else {
      text_centred(9, C_BAD, "READY, BUT SOME CLIPS FAILED");
    }
    textf(4, 15, C_TEXT, "CLIPS CONVERTED   %4d", s_cv.converted);
    textf(4, 16, C_TEXT, "ALREADY THERE     %4d", s_cv.ready);
    if (s_cv.failed > 0) {
      textf(4, 17, C_BAD, "FAILED            %4d", s_cv.failed);
    }
    textf(4, 18, C_TEXT, "WRITTEN           %4lu MB",
          (unsigned long)(s_cv.out_bytes / 1000000u));
    textf(4, 19, C_TEXT, "TIME              %4lu MIN", conv_minutes(run_us));
    text_centred(24, C_DIM, "SPACE: CONTINUE");
    return;
  }
  int n = s_cv.converted + s_cv.failed + 1;
  textf(2, 8, C_TEXT, "THIS CLIP  %-10s  %3d OF %d", s_cv.clip, n, s_cv.todo);
  for (int seg = 0; seg < CONV_SEGMENTS; seg++) {
    fb_fill_rect(16 + seg * 32, 80, 30, 12, CONV_FIRST_SEGMENT + seg);
  }
  uint64_t total = s_cv.total_bytes ? s_cv.total_bytes : 1u;
  int percent = (int)(s_cv.done_bytes * 100u / total);
  textf(2, 13, C_TEXT, "THE WHOLE GAME  %3d %%", percent);
  fb_fill_rect(16, 116, 288, 12, C_DIM);
  fb_fill_rect(16, 116, (int)(288u * s_cv.done_bytes / total), 12, C_GOOD);
  uint64_t left = s_cv.total_bytes - s_cv.done_bytes;
  uint64_t left_us = s_cv.run_bytes > 0
                         ? left * run_us / s_cv.run_bytes
                         : left / 1024u * CONV_US_PER_KB;
  textf(2, 17, C_VALUE, "TIME LEFT       ABOUT %lu MIN",
        conv_minutes(left_us));
  textf(2, 18, C_DIM, "ELAPSED         %lu MIN",
        (unsigned long)(run_us / 60000000u));
  text_centred(22, C_DIM, "THE ST CAN BE LEFT ALONE.");
  text_centred(23, C_DIM, "SPACE STOPS; THE NEXT START");
  text_centred(24, C_DIM, "CARRIES ON WHERE IT STOPPED.");
}

// One clip's screen and times (the debug hook's).
static void conv_draw(void) {
  if (s_cv.all) {
    conv_draw_all();
    return;
  }
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text(0, 0, C_TITLE, "CONVERTING A CLIP ON THE CARTRIDGE");
  rule(1);
  textf(0, 3, C_TEXT, "CLIP    %s", s_cv.clip);
  textf(0, 4, C_TEXT, "INTO    %s", s_cv.out_path);
  textf(0, 5, C_TEXT, "FOR     %s",
        s_cv.gun_bits == 4 ? "AN STE (4,096 COLOURS)" : "AN ST (512 COLOURS)");
  for (int s = 0; s < CONV_SEGMENTS; s++) {
    fb_fill_rect(16 + s * 32, 80, 30, 16, CONV_FIRST_SEGMENT + s);
  }
  if (!s_cv.done) {
    text(0, 14, C_DIM, "THE SCREEN STAYS AS IT IS: THE BAR");
    text(0, 15, C_DIM, "FILLS AS THE CLIP IS READ");
    text(0, 24, C_DIM, "SPACE: STOP");
    return;
  }
  const conv_results_t *r = &convResults;
  if (r->result != 0) {
    textf(0, 14, C_BAD, "STOPPED: %d", r->result);
  } else {
    textf(0, 14, C_GOOD, "DONE    %lu FRAMES, %lu PICTURES",
          (unsigned long)r->frames, (unsigned long)r->pictures);
    textf(0, 15, C_TEXT, "        %lu INDEXED, %lu BYTES",
          (unsigned long)r->keys, (unsigned long)r->bytes);
    uint32_t clip_ms = r->frames * 40u;
    textf(0, 16, C_VALUE, "TIME    %lu.%lu S, %lu.%02lu X THE CLIP",
          (unsigned long)(r->total_ms / 1000u),
          (unsigned long)(r->total_ms / 100u % 10u),
          (unsigned long)(clip_ms ? r->total_ms / clip_ms : 0),
          (unsigned long)(clip_ms ? r->total_ms * 100u / clip_ms % 100u : 0));
    textf(0, 18, C_TEXT, "READ    %6lu MS DECODE  %6lu MS",
          (unsigned long)r->read_ms, (unsigned long)r->decode_ms);
    textf(0, 19, C_TEXT, "CONVERT %6lu MS SOUND   %6lu MS",
          (unsigned long)r->convert_ms, (unsigned long)r->sound_ms);
    textf(0, 20, C_TEXT, "ENCODE  %6lu MS WRITE   %6lu MS",
          (unsigned long)r->encode_ms, (unsigned long)r->write_ms);
    textf(0, 21, C_DIM, "  HIST  %6lu MS PALETTE %6lu MS",
          (unsigned long)r->histogram_ms, (unsigned long)r->palette_ms);
    textf(0, 22, C_DIM, "  DITHER%6lu MS", (unsigned long)r->dither_ms);
  }
  text(0, 24, C_DIM, "SPACE: BACK");
}

static void conv_release_heap(void) {
  free(s_cv.rows);
  s_cv.rows = NULL;
  free(s_cv.job);
  s_cv.job = NULL;
}

// A clip ended (or never started): its results, its memory freed.
static void conv_record(int result) {
  s_cv.result = result;
  conv_results_t *r = &convResults;
  memset(r, 0, sizeof(*r));
  r->result = result;
  r->gun_bits = (uint32_t)s_cv.gun_bits;
  if (s_cv.job == NULL) {
    conv_release_heap();
    return;
  }
  const convjob_times_t *t = &s_cv.job->times;
  const clip_header_t *h = &s_cv.job->m.writer->header;
  r->frames = h->frames;
  r->pictures = s_cv.job->pictures;
  r->keys = h->index_count;
  r->bytes = s_cv.job->m.writer->offset;
  r->source_bytes = h->source_bytes;
  r->source_crc = h->source_crc;
  r->file_crc = h->crc;
  r->total_ms = (uint32_t)(t->total / 1000u);
  r->read_ms = (uint32_t)(t->read / 1000u);
  r->decode_ms = (uint32_t)(t->decode / 1000u);
  r->sound_ms = (uint32_t)(t->sound / 1000u);
  r->encode_ms = (uint32_t)(t->encode / 1000u);
  r->write_ms = (uint32_t)(t->write / 1000u);
  r->convert_ms = (uint32_t)((t->total - t->read - t->decode - t->sound -
                              t->encode - t->write) /
                             1000u);
  r->histogram_ms = (uint32_t)(t->histogram_cycles / CYCLES_PER_US / 1000u);
  r->palette_ms = (uint32_t)(t->palette_cycles / CYCLES_PER_US / 1000u);
  r->dither_ms = (uint32_t)(t->dither_cycles / CYCLES_PER_US / 1000u);
  conv_release_heap();
  DPRINTF("Convert %s into %s: result %d; %lu frames, %lu pictures, %lu "
          "indexed, %lu bytes; source %lu bytes CRC-32 %08lX; file CRC-32 "
          "%08lX; %lu ms (read %lu, decode %lu, convert %lu, sound %lu, "
          "encode %lu, write %lu; histogram %lu, palette %lu, dither %lu)\n",
          s_cv.clip, s_cv.out_path, result, (unsigned long)r->frames,
          (unsigned long)r->pictures, (unsigned long)r->keys,
          (unsigned long)r->bytes, (unsigned long)r->source_bytes,
          (unsigned long)r->source_crc, (unsigned long)r->file_crc,
          (unsigned long)r->total_ms, (unsigned long)r->read_ms,
          (unsigned long)r->decode_ms, (unsigned long)r->convert_ms,
          (unsigned long)r->sound_ms, (unsigned long)r->encode_ms,
          (unsigned long)r->write_ms,
          (unsigned long)r->histogram_ms, (unsigned long)r->palette_ms,
          (unsigned long)r->dither_ms);
}

static void conv_all_next(void);

// A clip ended: one clip alone shows its times; in a run, the next begins.
static void conv_finish(int result) {
  conv_record(result);
  if (s_cv.all) {
    if (result == 0) {
      s_cv.converted++;
      s_cv.out_bytes += convResults.bytes;
    } else {
      s_cv.failed++;
    }
    s_cv.done_bytes += s_cv.clip_bytes;
    s_cv.run_bytes += s_cv.clip_bytes;
    conv_all_next();
    return;
  }
  s_cv.done = true;
  palette_set(conv_palette(result == 0 ? CONV_SEGMENTS : s_cv.lit));
  s_dirty = true;
}

// The screen drawn, on the ST before the work behind it starts. At an ST's
// start the hello comes seconds before its loop takes a frame (the IKBD's
// reset: 2.8 s on a Mega ST's power-on), and it counts the frame it finds
// as seen: the screen is published again until the ST shows one.
static void publish_until_shown(void) {
  uint32_t t0 = time_us_32();
  do {
    fb_publish();
  } while (!fb_wait_shown(IP_SHOWN_TIMEOUT_US) &&
           time_us_32() - t0 < CONV_SHOWN_TIMEOUT_US);
}

// Converts `entry` into its clip file for s_cv.gun_bits: the screen first,
// then the memory and the job.
static void conv_begin(const iso9660_entry_t *entry) {
  snprintf(s_cv.clip, sizeof(s_cv.clip), "%s", entry->name);
  conv_path(s_cv.out_path, sizeof(s_cv.out_path), s_cv.gun_bits, entry->name);
  s_cv.clip_bytes = entry->size;
  s_cv.lit = 0;
  f_mkdir(conv_folder(s_cv.gun_bits));  // FR_EXIST when it is there

  // The screen, shown before its memory goes to the converter.
  conv_draw();
  palette_set(conv_palette(0));
  publish_until_shown();

  // The small one first: it fits the heap's free block from the boot.
  s_cv.job = malloc(sizeof(convjob_t));
  s_cv.rows = malloc(2 * MPEG1_SLOT_BYTES);
  if (s_cv.job == NULL || s_cv.rows == NULL) {
    conv_release_heap();
    conv_finish(-100);
    return;
  }
  uint8_t *window = (uint8_t *)__rom_in_ram_start__;
  uint8_t *cart_fb = window + CART_FRAMEBUFFER_OFFSET;
  uint8_t *app_free = window + CART_APP_FREE_OFFSET;
  uint8_t *scratch = fb_chunked_scratch();
  convjob_memory_t m;
  memset(&m, 0, sizeof(m));
  int n = 0;
  for (int i = 0; i < 7; i++) {
    m.rows[n++] = fb_chunked_buffer + i * MPEG1_SLOT_BYTES;
  }
  for (int i = 0; i < 3; i++) {
    m.rows[n++] = scratch + i * MPEG1_SLOT_BYTES;
  }
  for (int i = 0; i < 3; i++) {
    m.rows[n++] = cart_fb + i * MPEG1_SLOT_BYTES;
  }
  m.rows[n++] = app_free;
  m.rows[n++] = s_dec.own_row;
  m.rows[n++] = s_cv.rows;
  m.rows[n++] = s_cv.rows + MPEG1_SLOT_BYTES;
  m.row_count = n;
  m.ring_c = scratch + 3 * MPEG1_SLOT_BYTES;
  m.sound = (int8_t *)(m.ring_c + PICTURE16_RING_C_BYTES);
  m.lines_y = cart_fb + 3 * MPEG1_SLOT_BYTES;
  m.pcm = (int16_t *)(m.lines_y + PICTURE16_LINES_Y_BYTES);
  m.mp2 = (mp2_t *)(app_free + MPEG1_SLOT_BYTES);
  m.writer = (clip_writer_t *)(fb_chunked_buffer + 7 * MPEG1_SLOT_BYTES);
  m.audio =
      (mpeg_ps_t *)((uint8_t *)m.writer + ALIGN4(sizeof(clip_writer_t)));
  m.video = &s_ps;
  m.dec = &s_dec;
  bench_start_cycles();
  DPRINTF("Convert %s into %s for %s\n", s_cv.clip, s_cv.out_path,
          s_cv.gun_bits == 4 ? "an STE" : "an ST");
  // The game's sequence starts in this clip, listed as key pictures.
  int clip = game_clip_find(entry->name);
  const game_clip_t *gc = clip >= 0 ? &game_clips[clip] : NULL;
  int r = convjob_start(s_cv.job, &m, &s_iso, entry, s_cv.out_path,
                        s_cv.gun_bits, gc ? &game_starts[gc->first_start] : NULL,
                        gc ? gc->start_count : 0u, bench_run2, bench_cycles);
  if (r < 0) {
    conv_finish(r);
  }
}

// One clip alone (the debug hook), for `gun_bits` (0: the machine's).
static void conv_start(int index, int gun_bits) {
  if (!benchResults.image_found || bench_busy()) {
    return;
  }
  if (s_sound.active) {
    sound_stop();  // its decoder is on the heap
  }
  if (index < 0) {
    index = (int)benchResults.clips - 1;
  }
  if (index >= (int)benchResults.clips) {
    index = 0;
  }
  iso9660_entry_t entry;
  if (!show_find_clip(index, &entry)) {
    return;
  }
  memset(&s_cv, 0, sizeof(s_cv));
  s_cv.active = true;
  s_cv.clip_index = index;
  s_cv.gun_bits = gun_bits != 0 ? gun_bits : conv_machine_bits();
  conv_begin(&entry);
}

// The run's next clip that is not ready yet, after s_cv.clip_index; none
// left: the run's end.
static void conv_all_next(void) {
  iso9660_entry_t entry;
  for (int i = s_cv.clip_index + 1; i < (int)benchResults.clips; i++) {
    if (!show_find_clip(i, &entry)) {
      continue;
    }
    char path[40];
    conv_path(path, sizeof(path), s_cv.gun_bits, entry.name);
    if (conv_file_current(path, &entry, s_cv.gun_bits)) {
      continue;
    }
    s_cv.clip_index = i;
    conv_begin(&entry);
    return;
  }
  if (s_cv.failed == 0) {
    manifest_write_set(s_cv.gun_bits);
  }
  s_cv.done = true;
  DPRINTF("Convert all for %s: %d converted, %d ready, %d failed, %lu MB, "
          "%lu s\n",
          s_cv.gun_bits == 4 ? "an STE" : "an ST", s_cv.converted,
          s_cv.ready, s_cv.failed, (unsigned long)(s_cv.out_bytes / 1000000u),
          (unsigned long)((time_us_64() - s_cv.run_t0) / 1000000u));
  palette_set(conv_palette(CONV_SEGMENTS));
  s_dirty = true;
}

// The whole game for the machine plugged in: the clips checked, then those
// not ready converted. `quiet` (an ST's start): with every clip ready, the
// bench goes on at once.
static void conv_all_start(bool quiet) {
  if (!benchResults.image_found) {
    if (benchResults.sd_ok && !bench_busy()) {
      // The same screen while the set is checked against its manifest.
      memset(&s_cv, 0, sizeof(s_cv));
      s_cv.all = true;
      s_cv.checking = true;
      s_cv.gun_bits = conv_machine_bits();
      conv_draw();
      palette_set(conv_palette(0));
      publish_until_shown();
      memset(&s_cv, 0, sizeof(s_cv));
      set_check();
      palette_set(bench_palette);
    }
    return;
  }
  if (bench_busy()) {
    return;
  }
  if (s_sound.active) {
    sound_stop();
  }
  memset(&s_cv, 0, sizeof(s_cv));
  s_cv.active = true;
  s_cv.all = true;
  s_cv.checking = true;
  s_cv.gun_bits = conv_machine_bits();
  s_cv.clip_index = -1;
  conv_draw();
  palette_set(conv_palette(0));
  publish_until_shown();  // every start shows the clips checked
  iso9660_dir_t dir;
  iso9660_entry_t entry;
  if (iso9660_opendir_root(&s_iso, &dir) == ISO9660_OK) {
    while (iso9660_readdir(&dir, &entry) == 1) {
      if (!is_scene_clip(entry.name)) {
        continue;
      }
      char path[40];
      conv_path(path, sizeof(path), s_cv.gun_bits, entry.name);
      s_cv.total++;
      s_cv.total_bytes += entry.size;
      if (conv_file_current(path, &entry, s_cv.gun_bits)) {
        s_cv.ready++;
        s_cv.done_bytes += entry.size;
      } else {
        s_cv.todo++;
      }
    }
  }
  s_cv.checking = false;
  DPRINTF("Convert all for %s: %d clips, %d ready, %d to convert\n",
          s_cv.gun_bits == 4 ? "an STE" : "an ST", s_cv.total, s_cv.ready,
          s_cv.todo);
  // The set's manifest: written now when every clip is there and it is
  // missing or old; deleted when clips are to be converted (the run's end
  // writes it again).
  manifest_header_t mh;
  bool listed = manifest_current(s_cv.gun_bits, &mh) &&
                mh.count == (uint16_t)s_cv.total;
  if (s_cv.todo == 0 && !listed) {
    manifest_write_set(s_cv.gun_bits);
  } else if (s_cv.todo > 0) {
    char path[40];
    manifest_path(path, sizeof(path), s_cv.gun_bits);
    f_unlink(path);
  }
  s_cv.run_t0 = time_us_64();
  if (s_cv.todo == 0 && quiet) {
    s_cv.active = false;
    palette_set(bench_palette);
    s_dirty = true;
    return;
  }
  conv_all_next();
}

// One picture a call: the main loop runs between them.
static void conv_frame(void) {
  if (s_cv.done) {
    if (s_dirty) {
      conv_draw();
      s_dirty = false;
    }
    fb_publish();
    return;
  }
  int r = convjob_step(s_cv.job);
  uint32_t size = s_cv.job->video_file.size;
  s_cv.lit = size ? (int)((uint64_t)s_cv.job->video_file.pos * CONV_SEGMENTS /
                          size)
                  : 0;
  s_cv.glow++;
  palette_set(conv_palette(s_cv.lit));
  if (r <= 0) {
    conv_finish(r);
  }
}

static void game_open(void);

// SPACE: a clip converting is stopped (its file closed, left incomplete); a
// run shows where it got to; from the end's screen, the game when the run
// made it ready, else back to the bench.
static void conv_stop(void) {
  if (!s_cv.done && s_cv.job != NULL) {
    convjob_abort(s_cv.job);
  }
  conv_release_heap();
  if (s_cv.all && !s_cv.done) {
    s_cv.done = true;
    s_cv.stopped = true;
    palette_set(conv_palette(s_cv.lit));
    s_dirty = true;
    return;
  }
  bool ready = s_cv.all && !s_cv.stopped && s_cv.failed == 0;
  s_cv.active = false;
  palette_set(bench_palette);
  s_dirty = true;
  if (ready) {
    game_open();  // "THE GAME IS READY": on to it
  }
}

// --- Playing a clip file ----------------------------------------------------
//
// A scene clip's clip file, as C writes it (BENCH_FOLDER/STE or /ST), played
// through player.h with its sound, the card at BENCH_FAST_KHZ and the mouse
// on while it plays (as the game will have it: every packet takes the ST's
// time, which must not touch the sound or the pictures). The pictures shown
// and dropped, the drift, the sound's underruns and the times, on the
// screen at the end and in playResults over SWD.

// The bench's clip: what it plays, and its results on the screen.
static struct {
  bool active;   // a clip playing, or its results on the screen
  bool done;     // its results on the screen
  bool heard;    // its first sample heard
  int index;     // the scene clip
  int bits;      // its set: 3 (ST) or 4 (STE)
  char name[16];
  char path[40];
} s_play;

// The player's thresholds, shown with its counts.
#define PLAY_SLOW_READ_MS 10
#define PLAY_LATE_MS 40

// --- The clip list and the soak run ------------------------------------------
//
// V: the scene clips, four columns of twenty, one set (the machine's at
// first; S the other). Return plays the one chosen; L plays every clip
// from it to the last, one after the other (the soak run: each clip's
// results on the console, the run's in soakResults over SWD and on the
// screen at the end, with the gap between the last sample of a clip and the
// first of the next).

#define CLIPS_ROWS 20
#define CLIPS_COLS 4
#define CLIPS_PAGE (CLIPS_ROWS * CLIPS_COLS)

static struct {
  bool active;   // the list, or the soak run's end
  int sel;
  int bits;      // the set: 3 (ST) or 4 (STE)
  bool overlay;  // the frame and the times over the pictures (O)
} s_clips;

static struct {
  bool active;   // a run going on
  bool done;     // its end on the screen
  bool advance;  // start the next clip at the next pass
  int index;     // the clip playing
  uint32_t t0;
  uint32_t ended_at;  // the last clip's last sample heard (0: none)
  uint64_t gap_sum;
} s_soak;

typedef struct {
  int clips;         // played, to the end or not
  int failed;
  int first_failed;  // the scene clip's index, -1 if none
  bool stopped;
  uint32_t frames;
  uint32_t pictures;
  uint32_t dropped;
  uint32_t underruns;
  uint32_t late_slices;
  int32_t drift_us[2];
  int32_t lead_ms;
  uint32_t read_max_us;
  uint32_t decode_max_us;
  uint32_t publish_max_us;
  uint32_t slow_reads;
  uint32_t late;
  uint32_t gaps;
  uint32_t gap_us[3];  // minimum, mean and maximum
  uint32_t total_s;
} soak_results_t;

__attribute__((used)) soak_results_t soakResults;

static void soak_draw(void) {
  const soak_results_t *r = &soakResults;
  text(0, 0, C_TITLE, "PLAYING EVERY CLIP");
  rule(1);
  textf(0, 3, r->stopped ? C_TEXT : C_GOOD, "%s %d CLIPS, %s SET",
        r->stopped ? "STOPPED AFTER" : "DONE:", r->clips,
        s_clips.bits == 4 ? "STE" : "ST");
  if (r->failed > 0) {
    textf(0, 4, C_BAD, "FAILED  %d, THE FIRST CLIP %d", r->failed,
          r->first_failed);
  }
  textf(0, 6, C_TEXT, "FRAMES  %lu, %lu SHOWN, %lu DROPPED",
        (unsigned long)r->frames, (unsigned long)r->pictures,
        (unsigned long)r->dropped);
  textf(0, 7, C_TEXT, "SOUND   %lu UNDERRUNS, %lu LATE",
        (unsigned long)r->underruns, (unsigned long)r->late_slices);
  textf(0, 8, C_TEXT, "DRIFT   %ld TO %ld MS",
        (long)(r->drift_us[0] / 1000), (long)(r->drift_us[1] / 1000));
  textf(0, 9, C_TEXT, "LEAD    %ld MS AT LEAST", (long)r->lead_ms);
  textf(0, 11, C_TEXT, "WORST   READ %lu.%lu, DECODE %lu.%lu MS",
        (unsigned long)(r->read_max_us / 1000u),
        (unsigned long)(r->read_max_us / 100u % 10u),
        (unsigned long)(r->decode_max_us / 1000u),
        (unsigned long)(r->decode_max_us / 100u % 10u));
  textf(0, 12, C_TEXT, "        PUBLISH %lu.%02lu MS",
        (unsigned long)(r->publish_max_us / 1000u),
        (unsigned long)(r->publish_max_us / 10u % 100u));
  textf(0, 13, C_TEXT, "        %lu READS OVER %lu MS, %lu LATE",
        (unsigned long)r->slow_reads,
        (unsigned long)PLAY_SLOW_READ_MS, (unsigned long)r->late);
  textf(0, 14, C_TEXT, "GAP     %lu GAPS BETWEEN CLIPS, MS",
        (unsigned long)r->gaps);
  textf(0, 15, C_VALUE, "        MIN %lu  MEAN %lu  MAX %lu",
        (unsigned long)(r->gap_us[0] / 1000u),
        (unsigned long)(r->gap_us[1] / 1000u),
        (unsigned long)(r->gap_us[2] / 1000u));
  textf(0, 17, C_TEXT, "TIME    %lu MIN %lu S", (unsigned long)(r->total_s / 60u),
        (unsigned long)(r->total_s % 60u));
  text(0, 24, C_DIM, "SPACE: BACK");
}

static void play_draw(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text(0, 0, C_TITLE, "PLAYING A CLIP FILE");
  rule(1);
  textf(0, 3, C_TEXT, "FILE    %s", s_play.path);
  const play_results_t *r = &playResults;
  if (r->result < 0) {
    textf(0, 5, C_BAD, "STOPPED: %d", r->result);
  } else {
    textf(0, 5, r->result == 0 ? C_GOOD : C_TEXT, "%s %lu FRAMES, %lu SHOWN",
          r->result == 0 ? "DONE   " : "STOPPED", (unsigned long)r->frames,
          (unsigned long)r->pictures);
  }
  textf(0, 6, C_TEXT, "        %lu DROPPED, %lu.%lu S",
        (unsigned long)r->dropped, (unsigned long)(r->total_ms / 1000u),
        (unsigned long)(r->total_ms / 100u % 10u));
  textf(0, 7, C_TEXT, "SOUND   %lu UNDERRUNS, %lu LATE",
        (unsigned long)r->underruns, (unsigned long)r->late_slices);
  textf(0, 9, C_TEXT, "LEAD    %ld MS AT LEAST", (long)r->lead_ms);
  textf(0, 8, C_TEXT, "DRIFT   %ld TO %ld MS, VOLUME %+d DB",
        (long)(r->drift_us[0] / 1000), (long)(r->drift_us[1] / 1000),
        r->volume_db);
  textf(0, 10, C_TEXT, "START   %lu MS TO THE FIRST SOUND",
        (unsigned long)r->start_ms);
  textf(0, 17, C_TEXT, "SLOW    %lu READS OVER %lu MS",
        (unsigned long)r->slow_reads,
        (unsigned long)PLAY_SLOW_READ_MS);
  textf(0, 18, C_TEXT, "LATE    %lu PICTURES OVER %d MS",
        (unsigned long)r->late, PLAY_LATE_MS);
  text(0, 12, C_DIM, "MS          MIN    MEAN     MAX");
  static const char *const names[3] = {"READ    ", "DECODE  ", "PUBLISH "};
  const uint32_t *stats[3] = {r->read_us, r->decode_us, r->publish_us};
  for (int i = 0; i < 3; i++) {
    const uint32_t *v = stats[i];
    textf(0, 13 + i, C_VALUE, "%s %3lu.%02lu %3lu.%02lu %3lu.%02lu", names[i],
          (unsigned long)(v[0] / 1000u), (unsigned long)(v[0] / 10u % 100u),
          (unsigned long)(v[1] / 1000u), (unsigned long)(v[1] / 10u % 100u),
          (unsigned long)(v[2] / 1000u), (unsigned long)(v[2] / 10u % 100u));
  }
  text(0, 24, C_DIM, "SPACE: BACK");
}

// The clip stops: the player closed, the card back at its speed, and the
// keyboard alone unless another clip follows (`chain`).
static void play_close(int result, bool chain) {
  player_close(result);
  if (!chain) {
    ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  }
  bench_set_spi_hz(s_configured_hz);
}

// The clip ends, its results on the screen.
static void play_finish(int result) {
  play_close(result, false);
  s_play.done = true;
  palette_set(bench_palette);
  s_dirty = true;
}

// The soak run's end: its results on the screen (the list's).
static void soak_finish(bool stopped) {
  s_soak.active = false;
  s_soak.advance = false;
  s_soak.done = true;
  soakResults.stopped = stopped;
  soakResults.total_s = (time_us_32() - s_soak.t0) / 1000000u;
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  palette_set(bench_palette);
  s_dirty = true;
  const soak_results_t *r = &soakResults;
  DPRINTF("Soak %s: %d clips, %d failed (first %d), %lu frames, %lu shown, "
          "%lu dropped, %lu late pictures, %lu underruns, %lu late, drift "
          "%ld..%ld us, lead %ld ms, worst read %lu decode %lu publish %lu "
          "us, %lu slow reads, %lu gaps %lu/%lu/%lu us, %lu s\n",
          stopped ? "stopped" : "done", r->clips, r->failed, r->first_failed,
          (unsigned long)r->frames, (unsigned long)r->pictures,
          (unsigned long)r->dropped, (unsigned long)r->late,
          (unsigned long)r->underruns, (unsigned long)r->late_slices,
          (long)r->drift_us[0], (long)r->drift_us[1], (long)r->lead_ms,
          (unsigned long)r->read_max_us, (unsigned long)r->decode_max_us,
          (unsigned long)r->publish_max_us, (unsigned long)r->slow_reads,
          (unsigned long)r->gaps,
          (unsigned long)r->gap_us[0], (unsigned long)r->gap_us[1],
          (unsigned long)r->gap_us[2], (unsigned long)r->total_s);
}

// A clip of the soak run ended: its results added to the run's.
static void soak_record(int result) {
  soak_results_t *s = &soakResults;
  const play_results_t *r = &playResults;
  s->clips++;
  if (result < 0) {
    s->failed++;
    if (s->first_failed < 0) {
      s->first_failed = s_play.index;
    }
  }
  s->frames += r->frames;
  s->pictures += r->pictures;
  s->dropped += r->dropped;
  s->underruns += r->underruns;
  s->late_slices += r->late_slices;
  s->slow_reads += r->slow_reads;
  s->late += r->late;
  if (r->drift_us[0] != INT32_MAX) {
    s->drift_us[0] = r->drift_us[0] < s->drift_us[0] ? r->drift_us[0]
                                                     : s->drift_us[0];
    s->drift_us[1] = r->drift_us[1] > s->drift_us[1] ? r->drift_us[1]
                                                     : s->drift_us[1];
  }
  if (r->frames > 0 && r->lead_ms < s->lead_ms) {
    s->lead_ms = r->lead_ms;
  }
  s->read_max_us = r->read_us[2] > s->read_max_us ? r->read_us[2]
                                                  : s->read_max_us;
  s->decode_max_us = r->decode_us[2] > s->decode_max_us ? r->decode_us[2]
                                                        : s->decode_max_us;
  s->publish_max_us = r->publish_us[2] > s->publish_max_us
                          ? r->publish_us[2]
                          : s->publish_max_us;
}

// The clip ends by itself (its last sample heard) or with an error: its
// results on the screen, or in a soak run the next clip at the next pass.
static void play_end(int result) {
  if (!s_soak.active) {
    play_finish(result);
    return;
  }
  play_close(result, true);
  soak_record(result);
  s_soak.ended_at = result == 0 ? time_us_32() : 0u;
  s_play.active = false;
  s_soak.advance = true;
}

// The clip, its frame and the times, over the picture (O).
static void play_overlay(uint32_t frame, uint8_t bright) {
  if (!s_clips.overlay) {
    return;
  }
  char line[2][COLS + 1];
  int n = snprintf(line[0], sizeof(line[0]), "%s %s %lu/%lu", s_play.name,
                   s_play.bits == 4 ? "STE" : "ST", (unsigned long)frame,
                   (unsigned long)player_frames());
  if (s_soak.active) {
    n += snprintf(line[0] + n, sizeof(line[0]) - (size_t)n, " SOAK %d",
                  soakResults.clips + 1);
  }
  if (player_paused()) {
    snprintf(line[0] + n, sizeof(line[0]) - (size_t)n, " PAUSED");
  }
  uint32_t us[3];
  player_last_times(us);
  snprintf(line[1], sizeof(line[1]), "R%lu.%lu D%lu.%lu P%lu.%lu DROP %lu SD %lu",
           (unsigned long)(us[0] / 1000u), (unsigned long)(us[0] / 100u % 10u),
           (unsigned long)(us[1] / 1000u), (unsigned long)(us[1] / 100u % 10u),
           (unsigned long)(us[2] / 1000u), (unsigned long)(us[2] / 100u % 10u),
           (unsigned long)playResults.dropped,
           (unsigned long)player_card_kbs());
  font_set_font(&font8x8);
  for (int i = 0; i < 2; i++) {
    fb_fill_rect(8, 4 + 10 * i, 8 * (int)strlen(line[i]) + 8, 10, 0);
    font_set_color(bright);
    font_move(12, 5 + 10 * i);
    font_print(line[i]);
  }
}

// Plays scene clip `index` of set `bits` (0: the machine's, else the
// other's) from `frame`, paused or not.
static void play_start(int index, int bits, uint32_t frame, bool paused) {
  if (!benchResults.sd_ok || s_test.running || s_test.pending ||
      s_show.active || s_ip.active || s_cv.active || s_play.active) {
    return;
  }
  if (s_sound.active) {
    sound_stop();
  }
  if (index < 0) {
    index = (int)benchResults.clips - 1;
  }
  if (index >= (int)benchResults.clips) {
    index = 0;
  }
  char clip_name[MANIFEST_NAME_BYTES];
  if (!clips_available() || !clip_name_at(index, clip_name)) {
    return;
  }
  memset(&s_play, 0, sizeof(s_play));
  s_play.active = true;
  s_play.index = index;
  snprintf(s_play.name, sizeof(s_play.name), "%.*s",
           (int)(strlen(clip_name) - 4), clip_name);
  if (s_clips.active) {
    s_clips.sel = index;
  }
  // The set asked for; with none, the machine's, else the other's.
  s_play.bits = bits != 0 ? bits : conv_machine_bits();
  conv_path(s_play.path, sizeof(s_play.path), s_play.bits, clip_name);
  bench_set_spi_hz(BENCH_FAST_KHZ * 1000u);
  player_set_overlay(play_overlay);
  int r = player_start(s_play.path, frame, paused);
  if (r == CLIPPLAY_ERR_IO && bits == 0) {
    s_play.bits = 7 - s_play.bits;
    conv_path(s_play.path, sizeof(s_play.path), s_play.bits, clip_name);
    r = player_start(s_play.path, frame, paused);
  }
  if (r < 0) {
    play_end(r);
    return;
  }
  ikbd_set_input_mode(IKBD_INPUT_MOUSE);
}

// Another clip, or the same in the other set, at once: the one playing
// stops without its results screen.
static void play_switch(int index, int bits, uint32_t frame) {
  bool paused = player_paused();
  play_close(PLAYER_STOPPED, true);
  if (s_soak.active) {
    soak_record(PLAYER_STOPPED);
    s_soak.index = index;
    s_soak.ended_at = 0;
  }
  s_play.active = false;
  play_start(index, bits, frame, paused);
}

// Every pass of the main loop: the player on; the soak run's gap measured
// at the clip's first sample heard.
static void play_frame(void) {
  if (s_play.done) {
    if (s_dirty) {
      play_draw();
      s_dirty = false;
    }
    fb_publish();
    return;
  }
  int r = player_frame();
  if (!s_play.heard && player_heard_ms() > 0) {
    s_play.heard = true;
    if (s_soak.active && s_soak.ended_at != 0) {
      soak_results_t *s = &soakResults;
      uint32_t gap = time_us_32() - s_soak.ended_at;
      s->gaps++;
      s_soak.gap_sum += gap;
      if (s->gaps == 1 || gap < s->gap_us[0]) {
        s->gap_us[0] = gap;
      }
      if (gap > s->gap_us[2]) {
        s->gap_us[2] = gap;
      }
      s->gap_us[1] = (uint32_t)(s_soak.gap_sum / s->gaps);
      s_soak.ended_at = 0;
    }
  }
  if (r <= 0) {
    play_end(r);
  }
}

// Q: the clip stops, to its results (a soak run: to the run's); from the
// results, back.
static void play_stop(void) {
  if (s_soak.active) {
    if (s_play.active) {
      play_close(PLAYER_STOPPED, true);
      soak_record(PLAYER_STOPPED);
      s_play.active = false;
    }
    soak_finish(true);
    return;
  }
  if (!s_play.done) {
    play_finish(PLAYER_STOPPED);  // to its screen
    return;
  }
  s_play.active = false;
  palette_set(bench_palette);
  s_dirty = true;
}

// The soak run's next clip, or its end.
static void soak_next(void) {
  s_soak.advance = false;
  s_soak.index++;
  if (s_soak.index >= (int)benchResults.clips) {
    soak_finish(false);
    return;
  }
  play_start(s_soak.index, s_clips.bits, 0, false);
}

// L: every clip from `index` to the last, in set `bits`.
static void soak_start(int index, int bits) {
  if (s_play.active || !clips_available()) {
    return;
  }
  memset(&s_soak, 0, sizeof(s_soak));
  memset(&soakResults, 0, sizeof(soakResults));
  soakResults.first_failed = -1;
  soakResults.drift_us[0] = INT32_MAX;
  soakResults.drift_us[1] = INT32_MIN;
  soakResults.lead_ms = INT32_MAX;
  s_clips.active = true;
  s_clips.bits = bits != 0                    ? bits
                 : benchResults.image_found ? conv_machine_bits()
                                            : s_set.gun_bits;
  s_soak.active = true;
  s_soak.t0 = time_us_32();
  s_soak.index = index < 0 ? 0 : index;
  DPRINTF("Soak from clip %d, %s set\n", s_soak.index,
          s_clips.bits == 4 ? "STE" : "ST");
  play_start(s_soak.index, s_clips.bits, 0, false);
}

// A clip's name in the list: its place on the page, without ".MPG".
static void clips_draw_name(int slot, bool sel, const char *name) {
  int col = (slot / CLIPS_ROWS) * 10;
  int row = 2 + slot % CLIPS_ROWS;
  char shown[12];
  size_t n = strlen(name);
  snprintf(shown, sizeof(shown), "%.*s", (int)(n > 4 ? n - 4 : n), name);
  text(col, row, sel ? C_TITLE : C_VALUE, sel ? ">" : " ");
  text(col + 1, row, sel ? C_TITLE : C_VALUE, shown);
}

static void clips_draw(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  if (s_soak.done) {
    soak_draw();
    return;
  }
  int first = s_clips.sel / CLIPS_PAGE * CLIPS_PAGE;
  textf(0, 0, C_TITLE, "CLIPS, %s  %d/%d",
        s_clips.bits == 4 ? "STE SET (4,096 COLOURS)" : "ST SET (512 COLOURS)",
        first / CLIPS_PAGE + 1,
        ((int)benchResults.clips + CLIPS_PAGE - 1) / CLIPS_PAGE);
  rule(1);
  int last = first + CLIPS_PAGE < (int)benchResults.clips
                 ? first + CLIPS_PAGE
                 : (int)benchResults.clips;
  if (benchResults.image_found) {
    iso9660_dir_t dir;
    iso9660_entry_t entry;
    if (iso9660_opendir_root(&s_iso, &dir) == ISO9660_OK) {
      int n = 0;
      while (n < last && iso9660_readdir(&dir, &entry) == 1) {
        if (is_scene_clip(entry.name)) {
          if (n >= first) {
            clips_draw_name(n - first, n == s_clips.sel, entry.name);
          }
          n++;
        }
      }
    }
  } else {
    // The page's names from the set's manifest, read in one pass.
    char path[40];
    manifest_path(path, sizeof(path), s_set.gun_bits);
    FIL f;
    if (f_open(&f, path, FA_READ) == FR_OK &&
        f_lseek(&f, MANIFEST_HEADER_BYTES +
                        (FSIZE_t)first * MANIFEST_ENTRY_BYTES) == FR_OK) {
      for (int n = first; n < last; n++) {
        uint8_t bytes[MANIFEST_ENTRY_BYTES];
        UINT got = 0;
        manifest_entry_t e;
        if (f_read(&f, bytes, sizeof(bytes), &got) != FR_OK ||
            got != sizeof(bytes) || manifest_entry_read(&e, bytes) != 0) {
          break;
        }
        clips_draw_name(n - first, n == s_clips.sel, e.name);
      }
      f_close(&f);
    }
  }
  text(0, 22, C_TEXT, "RETURN: PLAY   L: ALL FROM HERE");
  text(0, 23, C_TEXT, "S: OTHER SET   O: OVERLAY   SPACE: BACK");
  text(0, 24, C_DIM, "PLAYING: SPACE PAUSE, Q STOP, < > CLIP");
}

// V: the list.
static void clips_open(void) {
  if (!clips_available()) {
    return;
  }
  if (s_sound.active) {
    sound_stop();
  }
  s_clips.active = true;
  if (s_clips.bits == 0) {
    s_clips.bits =
        benchResults.image_found ? conv_machine_bits() : s_set.gun_bits;
  }
  s_dirty = true;
}

static void clips_frame(void) {
  if (s_soak.advance) {
    soak_next();
    return;
  }
  if (s_dirty) {
    clips_draw();
    s_dirty = false;
  }
  fb_publish();
}

// The keys of a clip playing (and of its results), of the list, and of the
// soak run's end.
static void clips_key(uint8_t scancode) {
  if (s_play.active) {
    if (s_play.done) {
      if (scancode == 0x39 || scancode == 0x10) {  // space, Q: back
        play_stop();
      }
      return;
    }
    switch (scancode) {
      case 0x39:  // space: pause, or on again
        player_pause(!player_paused());
        break;
      case 0x10:  // Q: stop
        play_stop();
        break;
      case 0x4D:  // right: the next clip
        play_switch(s_play.index + 1 < (int)benchResults.clips
                        ? s_play.index + 1
                        : 0,
                    s_play.bits, 0);
        break;
      case 0x4B:  // left: the previous clip
        play_switch(s_play.index > 0 ? s_play.index - 1
                                     : (int)benchResults.clips - 1,
                    s_play.bits, 0);
        break;
      case 0x1F:  // S: the other set, at the same frame
        play_switch(s_play.index, 7 - s_play.bits, player_frame_shown());
        break;
      case 0x18:  // O: the overlay, from the next picture
        s_clips.overlay = !s_clips.overlay;
        break;
      case 0x16:  // U: 3 dB louder, from the next samples read
        player_volume(+1);
        break;
      case 0x20:  // D: 3 dB quieter
        player_volume(-1);
        break;
      default:
        break;
    }
    return;
  }
  if (s_soak.done) {
    if (scancode == 0x39) {  // space: back to the list
      s_soak.done = false;
      s_dirty = true;
    }
    return;
  }
  int clips = (int)benchResults.clips;
  switch (scancode) {
    case 0x48:  // up
      s_clips.sel = s_clips.sel > 0 ? s_clips.sel - 1 : clips - 1;
      break;
    case 0x50:  // down
      s_clips.sel = s_clips.sel + 1 < clips ? s_clips.sel + 1 : 0;
      break;
    case 0x4B:  // left: a column back
      s_clips.sel = s_clips.sel >= CLIPS_ROWS ? s_clips.sel - CLIPS_ROWS : 0;
      break;
    case 0x4D:  // right: a column on
      s_clips.sel = s_clips.sel + CLIPS_ROWS < clips ? s_clips.sel + CLIPS_ROWS
                                                     : clips - 1;
      break;
    case 0x1C:  // return, enter: play it
    case 0x72:
      play_start(s_clips.sel, s_clips.bits, 0, false);
      break;
    case 0x26:  // L: every clip from it
      soak_start(s_clips.sel, s_clips.bits);
      break;
    case 0x1F:  // S: the other set
      s_clips.bits = 7 - s_clips.bits;
      break;
    case 0x18:  // O: the overlay
      s_clips.overlay = !s_clips.overlay;
      break;
    case 0x39:  // space: back to the bench
      s_clips.active = false;
      break;
    default:
      break;
  }
  s_dirty = true;
}

// --- The palette test -------------------------------------------------------
//
// Two full-screen pictures, each with a palette of its own, the other one
// every PAL_TEST_FRAMES frames, a frame published every frame: A in colour
// 8 (its palette: 8 red, 9 white), B in colour 9 (9 green, 8 blue). Sent as
// the frame's palette (palette_set_frame()), only red and green may show.
// Sent as the palette now before the frame (palette_set(), the way every
// palette went before frames carried theirs), a picture shows with the
// other's palette for a VBL or two: blue or white flashes.

#define PAL_TEST_FRAMES 12

static struct {
  bool active;
  bool frame_palette;
  uint32_t frame;
} s_pal;

static void pal_start(bool frame_palette) {
  if (s_test.running || s_test.pending || s_show.active || s_ip.active ||
      s_cv.active || s_sound.active) {
    return;
  }
  s_pal.active = true;
  s_pal.frame_palette = frame_palette;
  s_pal.frame = 0;
}

static void pal_frame(void) {
  bool b = ((s_pal.frame / PAL_TEST_FRAMES) & 1u) != 0;
  uint16_t pal[16];
  memcpy(pal, bench_palette, sizeof(pal));
  pal[8] = b ? PALETTE_RGB(0, 0, 7) : PALETTE_RGB(7, 0, 0);
  pal[9] = b ? PALETTE_RGB(0, 7, 0) : PALETTE_RGB(7, 7, 7);
  fb_chunked_clear(b ? 9 : 8);
  font_set_font(&font8x8);
  text(0, 1, C_TEXT,
       s_pal.frame_palette ? "PALETTE WITH THE FRAME: RED, GREEN"
                           : "PALETTE BEFORE THE FRAME: FLASHES");
  text(0, 24, C_TEXT, "SPACE: BACK");
  if (s_pal.frame_palette) {
    palette_set_frame(pal);
  } else {
    palette_set(pal);
  }
  fb_publish();
  s_pal.frame++;
}

static void pal_stop(void) {
  s_pal.active = false;
  palette_set(bench_palette);
  s_dirty = true;
}

// Whether the bench is in the middle of something a conversion must not
// interrupt (the sound test it stops itself).
static bool bench_busy(void) {
  return s_test.running || s_test.pending || s_show.active || s_ip.active ||
         s_cv.active || s_play.active || s_clips.active || s_pal.active ||
         gameui_active();
}

// --- Public -----------------------------------------------------------------

void bench_init(void) {
  palette_set(bench_palette);
  memset(&benchResults, 0, sizeof(benchResults));
  s_boot_us = time_us_32();
  s_dirty = true;
}

void bench_start_sd(void) {
  benchResults.sd_start_ms = (time_us_32() - s_boot_us) / 1000u;
  benchResults.sd_result = sdcard_initFilesystem(&s_fs, BENCH_FOLDER);
  benchResults.sd_ok = benchResults.sd_result == SDCARD_INIT_OK;
  DPRINTF("SD card started %lu ms after bench_init: %s\n",
          (unsigned long)benchResults.sd_start_ms,
          benchResults.sd_ok ? "mounted" : "unavailable");
  spi_inst_t *spi = bench_spi();
  s_configured_hz = (spi != NULL) ? spi_get_baudrate(spi) : 0;
  if (benchResults.sd_ok) {
    benchResults.image_found = bench_find_image();
  }
  if (benchResults.image_found) {
    DPRINTF("Image %s/%s: %llu bytes, %lu fragment(s), fast seek %s, "
            "volume %s%s\n",
            BENCH_FOLDER, s_image_name, (unsigned long long)s_iso.image_size,
            (unsigned long)s_iso.fragments, s_iso.fast_seek ? "on" : "off",
            s_iso.volume_id, s_iso.joliet ? " (Joliet)" : "");
    bench_count_root();
    DPRINTF("Root: %lu entries, %lu scene clips, %llu bytes\n",
            (unsigned long)benchResults.entries,
            (unsigned long)benchResults.clips,
            (unsigned long long)benchResults.clip_bytes);
  }
  s_dirty = true;
}

// --- The game -------------------------------------------------------------------
//
// The game (gameui.h) starts once the machine's clips are there: at an ST's
// start after the clips' check (with the image, every clip current; without
// it, a complete set), or on G. B in its menu comes back to the bench.

static void game_clip_path(char *out, unsigned size, const char *clip) {
  int bits = benchResults.image_found ? conv_machine_bits() : s_set.gun_bits;
  conv_path(out, size, bits, clip);
}

static void game_card_fast(bool fast) {
  bench_set_spi_hz(fast ? BENCH_FAST_KHZ * 1000u : s_configured_hz);
}

static void game_to_bench(void) {
  palette_set(bench_palette);
  s_dirty = true;
}

static const gameui_host_t game_host = {game_clip_path, game_card_fast,
                                        game_to_bench};

static void game_open(void) {
  // Every clip of the set: with the image, its manifest (written once all
  // are converted); without it, the set found complete.
  manifest_header_t mh;
  bool complete = benchResults.image_found
                      ? manifest_current(conv_machine_bits(), &mh)
                      : s_set.ready;
  if (!clips_available() || !complete || bench_busy()) {
    return;
  }
  if (s_sound.active) {
    sound_stop();
  }
  gameui_start(&game_host);
}

void bench_restart(void) {
  // A game going on stops: the session starts again.
  gameui_stop();
  // A conversion still running is stopped: its screen cannot be drawn again
  // (it works in the framebuffers' memory). The check below carries it on.
  if (s_cv.active) {
    if (!s_cv.done && s_cv.job != NULL) {
      convjob_abort(s_cv.job);
    }
    conv_release_heap();
    s_cv.active = false;
  }
  // A clip playing stops: the sound's clock starts again with the session.
  if (s_play.active) {
    if (!s_play.done) {
      play_finish(PLAYER_STOPPED);
    }
    s_play.active = false;
  }
  // The list and a soak run end with it: back to the bench.
  memset(&s_soak, 0, sizeof(s_soak));
  s_clips.active = false;
  palette_set(s_show.active ? show_palette() : bench_palette);
  s_dirty = true;
  // Every start checks the game's clips for this machine: those missing are
  // converted; when all are there, the game.
  conv_all_start(true);
  if (!s_cv.active) {
    game_open();
  }
}

void bench_stop_card_work(void) {
  if (s_cv.active && !s_cv.done && s_cv.job != NULL) {
    convjob_abort(s_cv.job);  // the clip file closed: synced, the card idle
  }
  if (s_write.active) {
    write_end(FR_OK);
  }
}

void bench_handle_key(const ikbd_key_event_t *key) {
  if (gameui_active()) {
    gameui_key(key);  // presses and releases
    return;
  }
  if (!key->is_press || s_test.running || s_test.pending) {
    return;
  }
  if (s_ip.active) {
    if (key->scancode == 0x39 && s_ip.done) {  // space: back to the bench
      ip_stop();
    }
    return;
  }
  if (s_cv.active) {
    if (key->scancode == 0x39) {  // space: stop, or back to the bench
      conv_stop();
    }
    return;
  }
  if (s_pal.active) {
    if (key->scancode == 0x39) {  // space: back to the bench
      pal_stop();
    }
    return;
  }
  if (s_play.active || s_clips.active) {
    clips_key(key->scancode);
    return;
  }
  if (s_sound.active && s_sound.play) {
    switch (key->scancode) {
      case 0x0D:  // = / +: 3 dB more
      case 0x4E:
        if (s_sound_gain + 1 < SOUND_GAINS) {
          s_sound_gain++;
        }
        break;
      case 0x0C:  // -: 3 dB less
      case 0x4A:
        if (s_sound_gain > 0) {
          s_sound_gain--;
        }
        break;
      case 0x4D:  // right: the next clip's sound
        sound_start(s_sound.clip_index + 1 < (int)benchResults.clips
                        ? s_sound.clip_index + 1
                        : 0,
                    true);
        break;
      case 0x4B:  // left: the previous clip's
        sound_start(s_sound.clip_index > 0 ? s_sound.clip_index - 1
                                           : (int)benchResults.clips - 1,
                    true);
        break;
      case 0x39:  // space: stopped
        sound_stop();
        break;
      default:
        break;
    }
    s_dirty = true;
    return;
  }
  if (s_show.active) {
    switch (key->scancode) {
      case 0x39:  // space: back to the bench
        show_stop();
        break;
      case 0x14:  // T: the timings over the picture, or not
        s_show.text = !s_show.text;
        break;
      case 0x11:  // W: palette by pixels or by their square root
        s_show.options.weighting ^= 1;
        break;
      case 0x2E:  // C: 512 or 4,096 colours
        s_show.options.gun_bits = (s_show.options.gun_bits == 3) ? 4 : 3;
        break;
      case 0x20:  // D: the next dither
        s_show.options.dither = (s_show.options.dither + 1) % PICTURE16_DITHERS;
        break;
      case 0x4D:  // right: the next clip
        show_start(s_show.clip_index + 1);
        break;
      case 0x4B:  // left: the previous clip
        show_start(s_show.clip_index - 1);
        break;
      default:
        break;
    }
    return;
  }
  uint32_t entries = benchResults.entries;
  switch (key->scancode) {
    case 0x48:  // up
      if (s_list_top > 0) {
        s_list_top--;
        s_dirty = true;
      }
      break;
    case 0x50:  // down
      if (s_list_top + LIST_ROWS < entries) {
        s_list_top++;
        s_dirty = true;
      }
      break;
    case 0x4B:  // left: a page up
      s_list_top = (s_list_top > LIST_ROWS) ? s_list_top - LIST_ROWS : 0;
      s_dirty = true;
      break;
    case 0x4D:  // right: a page down
      if (entries > LIST_ROWS) {
        s_list_top += LIST_ROWS;
        if (s_list_top + LIST_ROWS > entries) {
          s_list_top = entries - LIST_ROWS;
        }
      }
      s_dirty = true;
      break;
    case 0x13:  // R
      bench_start_test();
      break;
    case 0x17:  // I: the intra pictures of the first scene clip
      show_start(0);
      break;
    case 0x19:  // P: the first scene clip's I and P pictures, in place
      ip_start(0, false);
      break;
    case 0x1E:  // A: the first scene clip's sound, played
      sound_start(0, true);
      break;
    case 0x2E:  // C: the whole game converted, the clips ready passed over
      conv_all_start(false);
      break;
    case 0x2F:  // V: the clips, to play them
      clips_open();
      break;
    case 0x22:  // G: the game
      game_open();
      break;
    case 0x14:  // T: the palette test, palettes with their frames
      pal_start(true);
      break;
    case 0x15:  // Y: the palette test, palettes before their frames
      pal_start(false);
      break;
    case 0x2D:  // X: back to Booster (the ST resets into it)
      st_session_return_to_booster();
      break;
    default:
      break;
  }
}

void bench_frame(void) {
  if (gameui_active()) {
    gameui_frame();
    return;
  }
  if (s_test.running && !s_test.redraw) {
    bench_test_slice();
    return;
  }
  if (s_write.active) {
    write_slice();
  }
  if (s_show.active) {
    show_frame();
    return;
  }
  if (s_ip.active) {
    ip_frame();
    return;
  }
  if (s_cv.active) {
    conv_frame();
    return;
  }
  if (s_pal.active) {
    pal_frame();
    return;
  }
  if (s_play.active) {
    play_frame();
    return;
  }
  if (s_clips.active) {
    clips_frame();
    return;
  }
  if (s_sound.active) {
    if (!s_sound.play) {
      sound_slice();
    } else if (s_sound.done) {
      sound_stop();
    }
  }
  if (s_dirty || s_test.pending || s_test.redraw) {
    bench_draw();
    s_dirty = false;
  }
  // Published every frame, changed or not: an ST that boots takes the frame
  // counter it finds as already shown, so a screen published only once can
  // be missed by an ST reset at the wrong moment.
  fb_publish();
  s_test.redraw = false;
  if (s_test.pending) {
    s_test.pending = false;
    s_test.running = true;
    bench_start_pass(0);
  }
}

uint32_t bench_devhook(uint16_t command_id, const uint16_t *payload,
                       uint16_t payload_size) {
  switch (command_id) {
    case DEVHOOKS_APP_READ_TEST:
      bench_start_test();
      return 1;
    case DEVHOOKS_APP_LIST_TOP:
      if (payload_size >= 2u) {
        s_list_top = payload[0];
        s_dirty = true;
        return 1;
      }
      return 0;
    case DEVHOOKS_APP_IN_PLACE:
      if (payload_size >= 2u) {
        ip_start((int)payload[0], payload_size >= 4u && payload[1] != 0);
      } else if (s_ip.active && s_ip.done) {
        ip_stop();
      }
      return 1;
    case DEVHOOKS_APP_WRITE_TEST:
      write_start(payload_size >= 2u ? payload[0] * 512u : 0u);
      return 1;
    case DEVHOOKS_APP_CONVERT_ALL:
      conv_all_start(false);
      return 1;
    case DEVHOOKS_APP_PLAY:
      if (payload_size >= 2u) {
        play_start((int)payload[0],
                   payload_size >= 4u && (payload[1] == 3 || payload[1] == 4)
                       ? (int)payload[1]
                       : 0,
                   payload_size >= 6u ? payload[2] : 0u, false);
      } else if (s_play.active || s_soak.active) {
        play_stop();
      }
      return 1;
    case DEVHOOKS_APP_NO_IMAGE:
      if (benchResults.image_found && !bench_busy()) {
        iso9660_unmount(&s_iso);
        benchResults.image_found = false;
        benchResults.clips = 0;
      }
      if (!benchResults.image_found && !bench_busy()) {
        set_check();
        if (payload_size >= 2u && payload[0] != 0) {
          s_set.ready = false;  // as if there were no set, or a stale one
          s_set.stale = payload[0] == 2;
          benchResults.clips = 0;
        }
      }
      return 1;
    case DEVHOOKS_APP_GAME:
      if (gameui_active()) {
        gameui_stop();
        game_to_bench();
      } else {
        game_open();
      }
      return 1;
    case DEVHOOKS_APP_GAME_BOT:
      if (payload_size >= 2u) {
        if (!gameui_active()) {
          game_open();
        }
        gameui_bot((int)payload[0], payload_size >= 4u && payload[1] != 0);
      } else {
        gameui_bot(-1, false);
      }
      return 1;
    case DEVHOOKS_APP_SOAK:
      soak_start(payload_size >= 2u ? (int)payload[0] : 0,
                 payload_size >= 4u && (payload[1] == 3 || payload[1] == 4)
                     ? (int)payload[1]
                     : 0);
      return 1;
    case DEVHOOKS_APP_CONVERT:
      if (payload_size >= 2u) {
        conv_start((int)payload[0],
                   payload_size >= 4u && (payload[1] == 3 || payload[1] == 4)
                       ? (int)payload[1]
                       : 0);
      } else if (s_cv.active) {
        conv_stop();
      }
      return 1;
    case DEVHOOKS_APP_SOUND:
      if (payload_size >= 2u) {
        sound_start((int)payload[0], payload_size >= 4u && payload[1] != 0);
      } else if (s_sound.active) {
        sound_stop();
      }
      return 1;
    case DEVHOOKS_APP_SLIDESHOW:
      if (payload_size >= 2u) {
        show_start((int)payload[0]);
      } else {
        show_stop();
      }
      return 1;
    default:
      return 0;
  }
}
