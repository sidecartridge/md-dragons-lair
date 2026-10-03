// units: rp/src/picture16.c
/* picture16's Lanczos scaler (352x240 to 320x200, chroma 176x120 to
 * 160x100) against a plain one: every source line across into a whole
 * 320x240 picture, then every output line down from it, with the same
 * weights (scaler_tables.h) and rounding. The scaler under test gets the
 * picture as the decoder hands it over, a macroblock row at a time, and
 * keeps only 16 lines in its column-major ring; its two cores' halves run
 * one after the other, in both orders, so that a half reading what the
 * other writes fails. Then the conversion of the scaled picture, on one
 * core and on two, every option: the same indices and palette. And the two
 * passes, which keep only a few scaled lines: the same indices and palette
 * as the conversion of the whole scaled picture. On the RP
 * the inner loops are Thumb assembly (picture16_asm.S) with the C's
 * arithmetic; here they are the C. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "picture16.h"
#include "scaler_tables.h"
#include "test.h"

#define SRC_W SCALER_SRC_W
#define SRC_H SCALER_SRC_H
#define OUT_W SCALER_OUT_W
#define OUT_H SCALER_OUT_H

static uint8_t src_y[SRC_H][SRC_W];
static uint8_t src_cb[SRC_H / 2][SRC_W / 2];
static uint8_t src_cr[SRC_H / 2][SRC_W / 2];

static uint8_t ref_y[OUT_H][OUT_W];
static uint8_t ref_cb[OUT_H / 2][OUT_W / 2];
static uint8_t ref_cr[OUT_H / 2][OUT_W / 2];

static uint8_t out_y[OUT_H][OUT_W];
static uint8_t out_cb[OUT_H / 2][OUT_W / 2];
static uint8_t out_cr[OUT_H / 2][OUT_W / 2];

static _Alignas(4) uint8_t ring[PICTURE16_SCALER_BYTES];

static uint8_t clamp8(int sum) {
  int v = (sum + (1 << (SCALER_SHIFT - 1))) >> SCALER_SHIFT;
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// One plane, the plain way: across into `mid` (out_w x src_h), then down.
static void reference_plane(const uint8_t *src, int src_w, int src_h,
                            const scaler_tap_t *across,
                            const scaler_tap_t *down, uint8_t *out,
                            int out_w, int out_h) {
  uint8_t *mid = malloc((size_t)out_w * (size_t)src_h);
  for (int l = 0; l < src_h; l++) {
    for (int x = 0; x < out_w; x++) {
      const uint8_t *p = src + l * src_w + across[x].first;
      int sum = 0;
      for (int k = 0; k < SCALER_TAPS_X; k++) {
        sum += across[x].weight[k] * p[k];
      }
      CHECK(across[x].first + SCALER_TAPS_X <= src_w);
      mid[l * out_w + x] = clamp8(sum);
    }
  }
  for (int o = 0; o < out_h; o++) {
    CHECK(down[o].first + SCALER_TAPS_Y <= src_h);
    for (int x = 0; x < out_w; x++) {
      int sum = 0;
      for (int k = 0; k < SCALER_TAPS_Y; k++) {
        sum += down[o].weight[k] * mid[(down[o].first + k) * out_w + x];
      }
      out[o * out_w + x] = clamp8(sum);
    }
  }
  free(mid);
}

static void run_ab(picture16_job_fn job, void *a, void *b) {
  job(a);
  job(b);
}

static void run_ba(picture16_job_fn job, void *a, void *b) {
  job(b);
  job(a);
}

// The scaler under test, fed a macroblock row at a time as the decoder
// does (16 luma lines and 8 of each chroma plane, in one slot).
static void scale(picture16_run2_fn run2) {
  enum { STRIDE = SRC_W };
  static uint8_t slot[STRIDE * 16 + STRIDE * 8];
  picture16_scaler_t s;
  // Dirty from a previous picture: the scaler must not read it.
  memset(ring, 0xA5, sizeof(ring));
  memset(out_y, 0, sizeof(out_y));
  memset(out_cb, 0, sizeof(out_cb));
  memset(out_cr, 0, sizeof(out_cr));
  picture16_scaler_init(&s, SRC_W, SRC_H, ring, &out_y[0][0], &out_cb[0][0],
                        &out_cr[0][0], OUT_W, OUT_H);
  s.run2 = run2;
  CHECK(s.lanczos);
  for (int r = 0; r < SRC_H / 16; r++) {
    // Poison, then the row's lines: nothing outside them may be read.
    memset(slot, 0x5A, sizeof(slot));
    memcpy(slot, &src_y[r * 16][0], STRIDE * 16);
    uint8_t *cb = slot + STRIDE * 16;
    uint8_t *cr = cb + STRIDE / 2 * 8;
    memcpy(cb, &src_cb[r * 8][0], STRIDE / 2 * 8);
    memcpy(cr, &src_cr[r * 8][0], STRIDE / 2 * 8);
    picture16_scaler_mb_row(&s, r, slot, cb, cr, STRIDE);
  }
  CHECK_EQ(s.next_y, OUT_H);
  CHECK_EQ(s.next_c, OUT_H / 2);
}

static int count_diff(const void *a, const void *b, size_t n) {
  const uint8_t *p = a, *q = b;
  int d = 0;
  for (size_t i = 0; i < n; i++) {
    d += p[i] != q[i];
  }
  return d;
}

static void check_picture(const char *what) {
  reference_plane(&src_y[0][0], SRC_W, SRC_H, scaler_luma_x, scaler_luma_y,
                  &ref_y[0][0], OUT_W, OUT_H);
  reference_plane(&src_cb[0][0], SRC_W / 2, SRC_H / 2, scaler_chroma_x,
                  scaler_chroma_y, &ref_cb[0][0], OUT_W / 2, OUT_H / 2);
  reference_plane(&src_cr[0][0], SRC_W / 2, SRC_H / 2, scaler_chroma_x,
                  scaler_chroma_y, &ref_cr[0][0], OUT_W / 2, OUT_H / 2);
  picture16_run2_fn runs[3] = {NULL, run_ab, run_ba};
  for (int i = 0; i < 3; i++) {
    scale(runs[i]);
    int dy = count_diff(out_y, ref_y, sizeof(ref_y));
    int dcb = count_diff(out_cb, ref_cb, sizeof(ref_cb));
    int dcr = count_diff(out_cr, ref_cr, sizeof(ref_cr));
    if (dy || dcb || dcr) {
      fprintf(stderr, "%s, run %d: %d Y, %d Cb, %d Cr bytes differ\n", what,
              i, dy, dcb, dcr);
    }
    CHECK_EQ(dy, 0);
    CHECK_EQ(dcb, 0);
    CHECK_EQ(dcr, 0);
  }
}

// picture16_convert on one core and on two (the histogram's halves into two
// memories, the dither's rows split), in both orders: the same indices and
// palette, whatever the options.
static void check_convert(void) {
  static uint8_t work[PICTURE16_WORK_BYTES], work2[PICTURE16_WORK_BYTES];
  static uint8_t one[OUT_H][OUT_W], two[OUT_H][OUT_W];
  for (int bits = 3; bits <= 4; bits++) {
    for (int weighting = 0; weighting <= 1; weighting++) {
      for (int d = 0; d < PICTURE16_DITHERS; d++) {
        picture16_options_t o1 = {bits, weighting, d, NULL, NULL};
        picture16_palette_t p1;
        memcpy(one, out_y, sizeof(one));
        picture16_convert(&one[0][0], &out_cb[0][0], &out_cr[0][0], OUT_W,
                          OUT_H, work, &o1, &p1, NULL);
        for (int order = 0; order < 2; order++) {
          picture16_options_t o2 = {bits, weighting, d,
                                    order ? run_ba : run_ab, work2};
          picture16_palette_t p2;
          memcpy(two, out_y, sizeof(two));
          picture16_convert(&two[0][0], &out_cb[0][0], &out_cr[0][0], OUT_W,
                            OUT_H, work, &o2, &p2, NULL);
          CHECK_EQ(count_diff(one, two, sizeof(one)), 0);
          CHECK_EQ(p1.colours, p2.colours);
          CHECK(memcmp(p1.rgb444, p2.rgb444, sizeof(p1.rgb444)) == 0);
        }
        CHECK(p1.colours > 0 && p1.colours <= 16);
      }
    }
  }
}

// The two passes, the source fed a macroblock row at a time twice: the
// same palette and indices as picture16_convert() on the whole scaled
// picture, whatever the options and the cores.
static uint8_t two_pass_out[OUT_H][OUT_W];

static void take_lines(void *ctx, int line, const uint8_t *indices,
                       int width) {
  (void)ctx;
  CHECK_EQ(width, OUT_W);
  CHECK(line >= 0 && line + 1 < OUT_H);
  memcpy(&two_pass_out[line][0], indices, 2u * OUT_W);
}

static void feed(picture16_passes_t *p) {
  enum { STRIDE = SRC_W };
  static uint8_t slot[STRIDE * 16 + STRIDE * 8];
  for (int r = 0; r < SRC_H / 16; r++) {
    memset(slot, 0x5A, sizeof(slot));
    memcpy(slot, &src_y[r * 16][0], STRIDE * 16);
    uint8_t *cb = slot + STRIDE * 16;
    uint8_t *cr = cb + STRIDE / 2 * 8;
    memcpy(cb, &src_cb[r * 8][0], STRIDE / 2 * 8);
    memcpy(cr, &src_cr[r * 8][0], STRIDE / 2 * 8);
    picture16_passes_mb_row(p, r, slot, cb, cr, STRIDE);
  }
}

static void check_two_pass(const char *what) {
  static uint8_t work[PICTURE16_WORK_BYTES], work2[PICTURE16_WORK_BYTES];
  static uint8_t one[OUT_H][OUT_W];
  static _Alignas(4) uint8_t lines[PICTURE16_LINES_BYTES];
  scale(NULL);  // the whole scaled picture, the reference's input
  picture16_run2_fn runs[3] = {NULL, run_ab, run_ba};
  int diffs = 0;
  for (int bits = 3; bits <= 4; bits++) {
    for (int weighting = 0; weighting <= 1; weighting++) {
      for (int d = 0; d < PICTURE16_DITHERS; d++) {
        picture16_options_t o1 = {bits, weighting, d, NULL, NULL};
        picture16_palette_t p1;
        memcpy(one, out_y, sizeof(one));
        picture16_convert(&one[0][0], &out_cb[0][0], &out_cr[0][0], OUT_W,
                          OUT_H, work, &o1, &p1, NULL);
        for (int i = 0; i < 3; i++) {
          picture16_options_t o2 = {bits, weighting, d, runs[i],
                                    runs[i] != NULL ? work2 : NULL};
          picture16_passes_t p;
          picture16_palette_t p2;
          memset(two_pass_out, 0xEE, sizeof(two_pass_out));
          memset(ring, 0xA5, sizeof(ring));
          memset(lines, 0xA5, sizeof(lines));
          // The memory in pieces, from separate buffers.
          picture16_memory_t mem = {ring, ring + PICTURE16_RING_Y_BYTES,
                                    lines, lines + PICTURE16_LINES_Y_BYTES,
                                    work};
          CHECK(picture16_passes_init(&p, SRC_W, SRC_H, &mem, &o2, NULL));
          feed(&p);
          picture16_passes_choose(&p, &p2);
          picture16_passes_dither(&p, &p2, take_lines, NULL);
          feed(&p);
          int diff = count_diff(one, two_pass_out, sizeof(one));
          diffs += diff != 0;
          CHECK_EQ(diff, 0);
          CHECK_EQ(p1.colours, p2.colours);
          CHECK(memcmp(p1.rgb444, p2.rgb444, sizeof(p1.rgb444)) == 0);
        }
      }
    }
  }
  if (diffs != 0) {
    fprintf(stderr, "%s: %d two-pass conversions differ\n", what, diffs);
  }
  picture16_passes_t p;
  picture16_memory_t mem = {ring, ring + PICTURE16_RING_Y_BYTES, lines,
                            lines + PICTURE16_LINES_Y_BYTES, work};
  CHECK(!picture16_passes_init(&p, 320, 240, &mem, NULL, NULL));
}

static uint32_t rng = 12345;
static uint8_t next_byte(void) {
  rng = rng * 1103515245u + 12345u;
  return (uint8_t)(rng >> 16);
}

int main(void) {
  // Noise: every tap counts.
  for (int l = 0; l < SRC_H; l++) {
    for (int x = 0; x < SRC_W; x++) {
      src_y[l][x] = next_byte();
    }
  }
  for (int l = 0; l < SRC_H / 2; l++) {
    for (int x = 0; x < SRC_W / 2; x++) {
      src_cb[l][x] = next_byte();
      src_cr[l][x] = next_byte();
    }
  }
  check_picture("noise");
  check_two_pass("noise");

  // Hard edges, 0 against 255, both ways: the lobes overshoot past both
  // ends, so the clamp works.
  for (int l = 0; l < SRC_H; l++) {
    for (int x = 0; x < SRC_W; x++) {
      src_y[l][x] = (((x / 3) ^ (l / 5)) & 1) ? 255 : 0;
    }
  }
  for (int l = 0; l < SRC_H / 2; l++) {
    for (int x = 0; x < SRC_W / 2; x++) {
      src_cb[l][x] = ((x / 2) & 1) ? 255 : 0;
      src_cr[l][x] = ((l / 2) & 1) ? 0 : 255;
    }
  }
  check_picture("edges");
  check_two_pass("edges");

  // A picture where every line and column differs from its neighbours'
  // position (catches a line read from the wrong ring slot).
  for (int l = 0; l < SRC_H; l++) {
    for (int x = 0; x < SRC_W; x++) {
      src_y[l][x] = (uint8_t)(l * 7 + x * 3);
    }
  }
  for (int l = 0; l < SRC_H / 2; l++) {
    for (int x = 0; x < SRC_W / 2; x++) {
      src_cb[l][x] = (uint8_t)(l * 11 + x);
      src_cr[l][x] = (uint8_t)(l * 5 + x * 9);
    }
  }
  check_picture("gradients");
  check_two_pass("gradients");
  check_convert();

  TEST_END();
}
