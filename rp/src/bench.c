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
#include "audio.h"
#include "iso9660.h"
#include "mp2_audio.h"
#include "mpeg1_video.h"
#include "mpeg_ps.h"
#include "palette.h"
#include "picture16.h"
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
  bool sd_started;
  uint32_t sd_start_ms;   // after the RP started
  uint32_t sd_start_hellos;  // ST hellos seen when it started: 0, timeout
  bool sd_ok;
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
// console; the data is whatever fb_chunked_buffer holds.

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

static void write_start(void) {
  if (!benchResults.sd_ok || s_write.active || s_test.running) {
    return;
  }
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
      fr = reading ? f_read(&s_write.f, fb_chunked_buffer, WRITE_CHUNK, &n)
                   : f_write(&s_write.f, fb_chunked_buffer, WRITE_CHUNK, &n);
    } else if (!reading) {
      fr = f_sync(&s_write.f);
    }
    s_write.us += time_us_32() - t0;
    if (fr != FR_OK || (s_write.done < WRITE_BYTES && n != WRITE_CHUNK)) {
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
            (unsigned long)WRITE_CHUNK,
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

static void bench_draw(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text(0, 0, C_TITLE, "DRAGON'S LAIR  SD + ISO BENCH");
  text(31, 0, C_DIM, RELEASE_VERSION);
  rule(1);

  if (!benchResults.sd_started) {
    text(0, 2, C_TEXT, "WAITING FOR THE ST, THEN THE SD CARD");
  } else if (!benchResults.sd_ok) {
    text(0, 2, C_BAD, "NO SD CARD");
  } else {
    textf(0, 2, C_TEXT, "CARD    MOUNTED, SPI %lu KHZ",
          (unsigned long)(s_configured_hz / 1000u));
    textf(0, 3, C_TEXT, "FOLDER  %s", BENCH_FOLDER);
  }
  if (benchResults.sd_ok && !benchResults.image_found) {
    text(0, 4, C_BAD, "NO CD-ROM IMAGE IN THE FOLDER");
    text(0, 5, C_TEXT, "COPY DL_CDROM_V31.ISO INTO " BENCH_FOLDER);
    if (benchResults.image_result != ISO9660_OK) {
      textf(0, 6, C_DIM, "LAST TRIED: %s",
            iso9660_strerror(benchResults.image_result));
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
         benchResults.image_found ? "UP/DN R READ I/P PICTS A SOUND ESC GEM"
                                  : "ESC GEM");
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

// --- Public -----------------------------------------------------------------

void bench_init(void) {
  palette_set(bench_palette);
  memset(&benchResults, 0, sizeof(benchResults));
  s_boot_us = time_us_32();
  s_dirty = true;
}

// Mounts the card, finds the image and reads its root directory.
static void bench_start_sd(void) {
  benchResults.sd_started = true;
  benchResults.sd_start_ms = (time_us_32() - s_boot_us) / 1000u;
  benchResults.sd_start_hellos = st_session_hellos();
  benchResults.sd_ok =
      sdcard_initFilesystem(&s_fs, BENCH_FOLDER) == SDCARD_INIT_OK;
  DPRINTF("SD card started %lu ms after boot (%s): %s\n",
          (unsigned long)benchResults.sd_start_ms,
          benchResults.sd_start_hellos ? "the ST said hello" : "timeout",
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

void bench_restart(void) {
  palette_set(s_show.active ? show_palette() : bench_palette);
  s_dirty = true;
}

void bench_handle_key(const ikbd_key_event_t *key) {
  if (!key->is_press || s_test.running || s_test.pending) {
    return;
  }
  if (s_ip.active) {
    if (key->scancode == 0x39 && s_ip.done) {  // space: back to the bench
      ip_stop();
    }
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
    default:
      break;
  }
}

void bench_frame(void) {
  if (!benchResults.sd_started &&
      (st_session_hellos() > 0 ||
       time_us_32() - s_boot_us >= BENCH_SD_WAIT_US)) {
    bench_start_sd();
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
      write_start();
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
