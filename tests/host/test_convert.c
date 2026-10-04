// units: rp/src/convert.c rp/src/cadence.c rp/src/mpeg1_video.c rp/src/mpeg_ps.c rp/src/picture16.c rp/src/crc32.c rp/src/clip.c
/* The clip converter on a synthetic clip with B pictures and open groups
 * (data/ibp_352x240.mpg): every I and P picture converted in two passes over
 * the decoder's frame store gives the indices and the palette that the whole
 * decoded picture, scaled in full and converted by picture16_convert(),
 * gives; the pictures come at their cadence's frames. With palette
 * stability, each picture keeps the palette in use, or has it refined with
 * its entries in their slots, or shows the same colours as with its own
 * palette, pixel for pixel; and a still
 * clip (data/still_352x240.mpg) keeps its palette and its indices from its
 * first picture on. Every palette's entry 0, the ST's border, is black. Last, the clip converted into a clip file as the
 * cartridge writes it, for an ST and an STE: the file's CRC-32 is the one
 * dlconv encode gives built by Apple clang and by GCC, so the converter's
 * integers are the same on every host and compiler (it changes with the
 * converter's output or the format: then dlconv encode gives the new one). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cadence.h"
#include "convert.h"
#include "clip.h"
#include "crc32.h"
#include "test.h"

#define W 352
#define H 240
#define OUT_W 320
#define OUT_H 200
#define MAX_PICTURES 64

static int read_file(void *ctx, uint8_t *buf, uint32_t len) {
  size_t got = fread(buf, 1, len, (FILE *)ctx);
  return ferror((FILE *)ctx) ? -1 : (int)got;
}

static void run_ab(picture16_job_fn job, void *a, void *b) {
  job(a);
  job(b);
}

static void run_ba(picture16_job_fn job, void *a, void *b) {
  job(b);
  job(a);
}

static uint8_t *slots[MPEG1_MAX_SLOTS];

static void give_slots(mpeg1_t *m) {
  for (int i = 0; i < 15 + 2; i++) {
    if (slots[i] == NULL) {
      slots[i] = malloc(MPEG1_SLOT_BYTES);
    }
    memset(slots[i], 0xAA, MPEG1_SLOT_BYTES);
  }
  mpeg1_set_slots(m, slots, 15 + 2);
}

static uint32_t picture_crc(const uint8_t *indices,
                            const picture16_palette_t *palette) {
  uint32_t crc = crc32_update(0, indices, (size_t)OUT_W * OUT_H);
  return crc32_update(crc, palette->rgb444, sizeof(palette->rgb444));
}

// --- The reference: whole pictures -------------------------------------------

static uint8_t frame_y[H][W];
static uint8_t frame_cb[H / 2][W / 2];
static uint8_t frame_cr[H / 2][W / 2];

static void store_row(void *ctx, int mb_row, const uint8_t *y,
                      const uint8_t *cb, const uint8_t *cr, int stride) {
  (void)ctx;
  for (int l = 0; l < 16; l++) {
    memcpy(&frame_y[mb_row * 16 + l][0], y + l * stride, W);
  }
  for (int l = 0; l < 8; l++) {
    memcpy(&frame_cb[mb_row * 8 + l][0], cb + l * (stride / 2), W / 2);
    memcpy(&frame_cr[mb_row * 8 + l][0], cr + l * (stride / 2), W / 2);
  }
}

// The I and P pictures' CRCs and first frames, the plain way.
static int reference(const picture16_options_t *options, uint32_t *crcs,
                     uint32_t *firsts) {
  static uint8_t ring[PICTURE16_SCALER_BYTES];
  static uint8_t out_y[OUT_H][OUT_W];
  static uint8_t out_cb[OUT_H / 2][OUT_W / 2];
  static uint8_t out_cr[OUT_H / 2][OUT_W / 2];
  static uint8_t work[PICTURE16_WORK_BYTES];
  static mpeg_ps_t ps;
  static mpeg1_t m;
  FILE *f = fopen("data/ibp_352x240.mpg", "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return 0;
  }
  mpeg_ps_init(&ps, read_file, f);
  mpeg1_init(&m, &ps);
  give_slots(&m);
  int count = 0;
  int type;
  while ((type = mpeg1_next_picture(&m)) > 0) {
    if (type != MPEG1_PICTURE_I && type != MPEG1_PICTURE_P) {
      mpeg1_skip_picture(&m);
      continue;
    }
    CHECK(mpeg1_decode_picture(&m, store_row, NULL) == type);
    picture16_scaler_t s;
    picture16_scaler_init(&s, W, H, ring, &out_y[0][0], &out_cb[0][0],
                          &out_cr[0][0], OUT_W, OUT_H);
    for (int r = 0; r < H / 16; r++) {
      picture16_scaler_mb_row(&s, r, &frame_y[r * 16][0], &frame_cb[r * 8][0],
                              &frame_cr[r * 8][0], W);
    }
    picture16_options_t one = *options;
    one.run2 = NULL;
    one.work2 = NULL;
    picture16_palette_t palette;
    picture16_convert(&out_y[0][0], &out_cb[0][0], &out_cr[0][0], OUT_W,
                      OUT_H, work, &one, &palette, NULL);
    CHECK(count < MAX_PICTURES);
    crcs[count] = picture_crc(&out_y[0][0], &palette);
    firsts[count] = count == 0 ? 0 : cadence_first_frame(m.display_index,
                                                         30000, 1001);
    count++;
  }
  fclose(f);
  return count;
}

// --- The converter -------------------------------------------------------------

typedef struct {
  uint8_t indices[OUT_H][OUT_W];
  uint32_t crcs[MAX_PICTURES];
  uint32_t firsts[MAX_PICTURES];
  uint32_t frames[MAX_PICTURES];
  int count;
  int lines;  // index lines received for the current picture
  // With palette stability: each picture's colours, its palette, and
  // whether it kept the palette shown before it.
  const convert_t *c;
  uint32_t kept_before;
  uint32_t evolved_before;
  bool evolved[MAX_PICTURES];
  uint8_t maps[MAX_PICTURES][16];
  uint16_t rgb[MAX_PICTURES][OUT_H * OUT_W / 8];  // every 8th pixel
  uint16_t palettes[MAX_PICTURES][16];
  uint8_t previous[OUT_H][OUT_W];
  bool kept[MAX_PICTURES];
  bool same_indices[MAX_PICTURES];  // the picture before's indices
} sink_t;

static void sink_lines(void *ctx, int line, const uint8_t *indices,
                       int width) {
  sink_t *k = (sink_t *)ctx;
  CHECK_EQ(width, OUT_W);
  CHECK(line >= 0 && line + 1 < OUT_H && (line & 1) == 0);
  memcpy(&k->indices[line][0], indices, 2u * OUT_W);
  k->lines += 2;
}

static void sink_begin(void *ctx, const convert_picture_t *picture) {
  sink_t *k = (sink_t *)ctx;
  CHECK_EQ(k->lines, 0);
  CHECK(picture->frames >= 1);
  CHECK(picture->type == MPEG1_PICTURE_I || picture->type == MPEG1_PICTURE_P);
  CHECK_EQ(picture->palette->rgb444[0], 0);  // the ST's border: black
}

static void sink_end(void *ctx, const convert_picture_t *picture) {
  sink_t *k = (sink_t *)ctx;
  const picture16_palette_t *palette = picture->palette;
  uint32_t first_frame = picture->first_frame;
  CHECK_EQ(k->lines, OUT_H);
  if (k->count > 0) {
    CHECK_EQ(k->firsts[k->count - 1] + k->frames[k->count - 1], first_frame);
  }
  k->frames[k->count] = picture->frames;
  k->lines = 0;
  CHECK(k->count < MAX_PICTURES);
  k->crcs[k->count] = picture_crc(&k->indices[0][0], palette);
  k->firsts[k->count] = first_frame;
  const uint8_t *idx = &k->indices[0][0];
  for (int i = 0; i < OUT_H * OUT_W / 8; i++) {
    k->rgb[k->count][i] = palette->rgb444[idx[8 * i]];
  }
  memcpy(k->palettes[k->count], palette->rgb444, sizeof(palette->rgb444));
  if (k->c != NULL) {
    k->kept[k->count] = k->c->kept != k->kept_before;
    k->kept_before = k->c->kept;
    k->evolved[k->count] = k->c->evolved != k->evolved_before;
    k->evolved_before = k->c->evolved;
    memcpy(k->maps[k->count], k->c->map, sizeof(k->c->map));
  }
  k->same_indices[k->count] =
      k->count > 0 &&
      memcmp(k->previous, k->indices, sizeof(k->indices)) == 0;
  memcpy(k->previous, k->indices, sizeof(k->indices));
  k->count++;
  memset(k->indices, 0xEE, sizeof(k->indices));
}

static void check_options(const picture16_options_t *options) {
  static uint32_t ref_crcs[MAX_PICTURES];
  static uint32_t ref_firsts[MAX_PICTURES];
  int ref = reference(options, ref_crcs, ref_firsts);
  CHECK_EQ(ref, 21);

  static _Alignas(4) uint8_t ring_c[PICTURE16_RING_C_BYTES];
  static _Alignas(4) uint8_t lines_y[PICTURE16_LINES_Y_BYTES];
  static mpeg_ps_t ps;
  static mpeg1_t m;
  static sink_t sink;
  FILE *f = fopen("data/ibp_352x240.mpg", "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  mpeg_ps_init(&ps, read_file, f);
  mpeg1_init(&m, &ps);
  give_slots(&m);
  memset(&sink, 0, sizeof(sink));
  convert_out_t out = {sink_begin, sink_lines, sink_end, &sink};
  convert_t c;
  convert_init(&c, &m, ring_c, lines_y, options, &out);
  int type;
  while ((type = convert_next(&c)) > 0) {
    CHECK(type == MPEG1_PICTURE_I || type == MPEG1_PICTURE_P);
  }
  fclose(f);
  CHECK_EQ(type, 0);
  CHECK_EQ(sink.count, ref);
  for (int i = 0; i < sink.count && i < ref; i++) {
    CHECK_EQ(sink.crcs[i], ref_crcs[i]);
    CHECK_EQ(sink.firsts[i], ref_firsts[i]);
    if (i > 0) {
      CHECK(sink.firsts[i] > sink.firsts[i - 1]);
    }
  }
  CHECK_EQ(convert_length(&c), 51u);
  CHECK_EQ(sink.firsts[sink.count - 1] + sink.frames[sink.count - 1], 51u);
}

// The clip at `path` through the converter, its pictures into `sink`.
static uint32_t run(const char *path, const picture16_options_t *options,
                    int keep_percent, sink_t *sink) {
  static _Alignas(4) uint8_t ring_c[PICTURE16_RING_C_BYTES];
  static _Alignas(4) uint8_t lines_y[PICTURE16_LINES_Y_BYTES];
  static mpeg_ps_t ps;
  static mpeg1_t m;
  static convert_t c;
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return 0;
  }
  mpeg_ps_init(&ps, read_file, f);
  mpeg1_init(&m, &ps);
  give_slots(&m);
  memset(sink, 0, sizeof(*sink));
  convert_out_t out = {sink_begin, sink_lines, sink_end, sink};
  convert_init(&c, &m, ring_c, lines_y, options, &out);
  c.keep_percent = keep_percent;
  sink->c = &c;
  int type;
  while ((type = convert_next(&c)) > 0) {
  }
  fclose(f);
  CHECK_EQ(type, 0);
  return c.kept;
}

static void check_stability(void) {
  static sink_t plain;
  static sink_t stable;
  for (int bits = 3; bits <= 4; bits++) {
    picture16_options_t o = {bits, PICTURE16_WEIGHT_SQRT,
                             PICTURE16_DITHER_MIX, NULL, NULL};
    run("data/ibp_352x240.mpg", &o, -1, &plain);
    uint32_t kept = run("data/ibp_352x240.mpg", &o, 10, &stable);
    CHECK_EQ(stable.count, plain.count);
    uint32_t counted = 0;
    uint32_t evolved = 0;
    for (int i = 0; i < stable.count; i++) {
      CHECK(!(stable.kept[i] && stable.evolved[i]));
      if (stable.kept[i]) {
        counted++;
        CHECK(i > 0);
        if (i > 0) {
          CHECK(memcmp(stable.palettes[i], stable.palettes[i - 1],
                       sizeof(stable.palettes[i])) == 0);
        }
      } else if (stable.evolved[i]) {
        // Refined in place: every entry in the slot it had.
        evolved++;
        CHECK(i > 0);
        if (i > 0) {
          CHECK(memcmp(stable.maps[i], stable.maps[i - 1], 16) == 0);
        }
      } else {
        // Its own palette, the indices relabelled: the same colours.
        int diff = 0;
        for (int p = 0; p < OUT_H * OUT_W / 8; p++) {
          diff += stable.rgb[i][p] != plain.rgb[i][p];
        }
        if (diff != 0) {
          fprintf(stderr, "picture %d: %d of %d sampled pixels differ\n", i,
                  diff, OUT_H * OUT_W / 8);
        }
        CHECK_EQ(diff, 0);
      }
    }
    CHECK_EQ(counted, kept);
    // The test pattern moves: a palette kept on some pictures and refined
    // on others, not all.
    CHECK(kept > 0 && kept < (uint32_t)stable.count - 1);
    CHECK(evolved > 0);

    // A still clip: the first P picture refines the I picture, and every
    // picture after it is the same: it keeps the palette, and the ordered
    // dither gives it the indices of the picture before.
    kept = run("data/still_352x240.mpg", &o, 0, &stable);
    CHECK_EQ(stable.count, 11);
    CHECK_EQ(kept, (uint32_t)stable.count - 2);
    for (int i = 2; i < stable.count; i++) {
      CHECK(stable.kept[i]);
      CHECK(stable.same_indices[i]);
    }
  }
}

// --- The clip file ---------------------------------------------------------------

static uint8_t clip_file[256u << 10];
static uint32_t clip_len;

static int file_write(void *ctx, const void *data, uint32_t len) {
  (void)ctx;
  if (clip_len + len > sizeof(clip_file)) {
    return -1;
  }
  memcpy(clip_file + clip_len, data, len);
  clip_len += len;
  return 0;
}

static int file_header(void *ctx, const uint8_t header[CLIP_HEADER_BYTES]) {
  (void)ctx;
  memcpy(clip_file, header, CLIP_HEADER_BYTES);
  return 0;
}

static const int8_t silence[CLIP_SAMPLES];  // the clip has no sound

static void file_begin(void *ctx, const convert_picture_t *picture) {
  clip_writer_picture((clip_writer_t *)ctx, picture->frames,
                      picture->palette->rgb444, false, silence);
}

static void file_lines(void *ctx, int line, const uint8_t *indices,
                       int width) {
  (void)line;
  clip_writer_row((clip_writer_t *)ctx, indices, NULL);
  clip_writer_row((clip_writer_t *)ctx, indices + width, NULL);
}

static void file_end(void *ctx, const convert_picture_t *picture) {
  for (uint32_t f = 1; f < picture->frames; f++) {
    clip_writer_held((clip_writer_t *)ctx, silence);
  }
}

static void check_clip_file(int bits, uint32_t expected) {
  static _Alignas(4) uint8_t ring_c[PICTURE16_RING_C_BYTES];
  static _Alignas(4) uint8_t lines_y[PICTURE16_LINES_Y_BYTES];
  static mpeg_ps_t ps;
  static mpeg1_t m;
  static convert_t c;
  static clip_writer_t w;
  static uint8_t source[64u << 10];
  FILE *f = fopen("data/ibp_352x240.mpg", "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  size_t source_bytes = fread(source, 1, sizeof(source), f);
  CHECK(source_bytes < sizeof(source));
  rewind(f);
  clip_len = 0;
  clip_io_t io = {file_write, file_header, NULL};
  clip_header_t h = {0};
  h.gun_bits = (uint8_t)bits;
  h.converter = CONVERT_VERSION;
  h.keep_percent = CONVERT_KEEP_PERCENT;
  h.source_bytes = (uint32_t)source_bytes;
  h.source_crc = crc32_update(0, source, source_bytes);
  CHECK_EQ(clip_writer_begin(&w, &io, &h, true), 0);
  mpeg_ps_init(&ps, read_file, f);
  mpeg1_init(&m, &ps);
  give_slots(&m);
  picture16_options_t o = {bits, PICTURE16_WEIGHT_SQRT, PICTURE16_DITHER_MIX,
                           NULL, NULL};
  convert_out_t out = {file_begin, file_lines, file_end, &w};
  convert_init(&c, &m, ring_c, lines_y, &o, &out);
  c.keep_percent = CONVERT_KEEP_PERCENT;
  int type;
  while ((type = convert_next(&c)) > 0) {
  }
  fclose(f);
  CHECK_EQ(type, 0);
  CHECK_EQ(clip_writer_finish(&w), 0);
  uint32_t crc = crc32_update(0, clip_file, clip_len);
  printf("The clip file for %d bits a gun: %u bytes, CRC-32 %08X (expected "
         "%08X)\n",
         bits, (unsigned)clip_len, (unsigned)crc, (unsigned)expected);
  CHECK_EQ(crc, expected);
}

int main(void) {
  static uint8_t work2[PICTURE16_WORK_BYTES];
  picture16_run2_fn runs[3] = {NULL, run_ab, run_ba};
  for (int bits = 3; bits <= 4; bits++) {
    for (int i = 0; i < 3; i++) {
      picture16_options_t o = {bits, PICTURE16_WEIGHT_SQRT,
                               PICTURE16_DITHER_MIX, runs[i],
                               runs[i] != NULL ? work2 : NULL};
      check_options(&o);
    }
  }
  check_stability();
  check_clip_file(3, 0xD3AF8385u);
  check_clip_file(4, 0xE2726DE7u);
  for (int i = 0; i < (int)MPEG1_MAX_SLOTS; i++) {
    free(slots[i]);
  }
  TEST_END();
}
