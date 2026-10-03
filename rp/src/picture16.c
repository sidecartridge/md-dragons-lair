/**
 * File: picture16.c
 * Description: A picture to 16 colours of its own. See picture16.h.
 *
 * Two passes over the picture. The first converts every 2 x 2 block (the
 * pixels of one chroma sample, luma averaged) to RGB and counts it in a
 * 16 x 16 x 16 histogram (4 bits a gun). Median cut then
 * splits the histogram's colour box until there are 16 boxes: each time the
 * box whose pixels are farthest from their mean (the largest sum of squared
 * errors), along its longest side, at the median of its pixels. A box's
 * colour is the mean of its pixels; three rounds of k-means (each colour of
 * the histogram to its nearest entry, each entry to the mean of its colours)
 * then move the entries to where the colours are. A table gives
 * every 4-bit-a-gun colour its nearest palette entry (or, for the mixing
 * dither, the pair of entries that mixes into it). The second pass adds the
 * ordered dither to each pixel's 8-bit RGB, looks the result up and writes
 * the index over the luma.
 *
 * The palette's entries are levels the target shows: 16 a gun on an STE (8-bit
 * value level x 17), 8 on an ST (level x 255 / 7). Box means and k-means
 * centres are computed in 8-bit and rounded to the nearest such level, and
 * the table's distances are measured in 8-bit, so the dither aims at the
 * colours the machine has.
 *
 * YCbCr to RGB as ITU-R BT.601 with MPEG-1's ranges (Y 16..235, Cb and Cr
 * 16..240), in 8.8 fixed point: R = 1.164 (Y - 16) + 1.596 (Cr - 128),
 * G = 1.164 (Y - 16) - 0.391 (Cb - 128) - 0.813 (Cr - 128),
 * B = 1.164 (Y - 16) + 2.018 (Cb - 128).
 */

#pragma GCC optimize("O3")

#include "picture16.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "scaler_tables.h"

#if defined(__ARM_ARCH_6M__)
#include "pico.h"
// In RAM, and never inlined into a caller that is not (it would run from
// flash then).
#define HOT(f) __attribute__((noinline)) __not_in_flash_func(f)
#else
#define HOT(f) f
#endif

#define BINS 4096
#define MAX_COLOURS 16

// clamp255() as a table, for the per-pixel loops: no branch (GCC lays the
// in-range case out of line, two taken branches a channel). It covers every
// value they produce: a colour channel's (c + t) >> 8 is -277..535, plus a
// dither offset of -16..+15; a Lanczos tap sum >> 14, -64..319.
#define CLAMP_LOW 320
#define CLAMP_SIZE 896
static const uint8_t clamp_table[CLAMP_SIZE] = {
    [0 ... CLAMP_LOW - 1] = 0,
    [CLAMP_LOW] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
    57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
    75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92,
    93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108,
    109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123,
    124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
    139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168,
    169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183,
    184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198,
    199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213,
    214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228,
    229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243,
    244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
    [CLAMP_LOW + 256 ... CLAMP_SIZE - 1] = 255,
};
#define CLAMP(v) (clamp_table[CLAMP_LOW + (v)])

// The 8x8 Bayer matrix of ffmpeg's paletteuse (dither_value() in
// vf_paletteuse.c), 0..63, and its offsets at bayer_scale 1 (halved and
// centred, -16..+15) and 2 (-8..+7).
static uint8_t bayer_rank[64];
static int8_t bayer[64];
static int8_t bayer_soft[64];
static bool bayer_ready;

static void build_bayer(void) {
  for (int p = 0; p < 64; p++) {
    int q = p ^ (p >> 3);
    int v = ((p & 4) >> 2) | ((q & 4) >> 1) | ((p & 2) << 1) |
            ((q & 2) << 2) | ((p & 1) << 4) | ((q & 1) << 5);
    bayer_rank[p] = (uint8_t)v;
    bayer[p] = (int8_t)((v >> 1) - 16);
    bayer_soft[p] = (int8_t)((v >> 2) - 8);
  }
  bayer_ready = true;
}



// A multiplication the compiler must not turn into shifts and adds (GCC's
// Cortex-M0+ tuning assumes a slow multiplier; the RP2040's takes a cycle).
#if defined(__ARM_ARCH_6M__)
static inline int mul(int a, int b) {
  __asm__("mul %0, %1" : "+l"(a) : "l"(b) : "cc");
  return a;
}
#else
static inline int mul(int a, int b) { return a * b; }
#endif

// A pixel's colour in 8.8 fixed point: the luma's term, 298 x (luma - 16)
// + 128, and its chroma sample's three (shared by the 2 x 2 pixels of the
// sample).
typedef struct {
  int r;
  int g;
  int b;
} chroma_t;

static inline chroma_t chroma_terms(int cb, int cr) {
  int d = cb - 128;
  int e = cr - 128;
  chroma_t t = {mul(409, e), -mul(100, d) - mul(208, e), mul(516, d)};
  return t;
}

static inline uint32_t now(const picture16_profile_t *prof) {
  return (prof != NULL && prof->cycles != NULL) ? prof->cycles() : 0;
}

static inline uint32_t since(const picture16_profile_t *prof, uint32_t t0) {
  return (prof != NULL && prof->cycles != NULL)
             ? ((prof->cycles() - t0) & 0xFFFFFFu)
             : 0;
}

// Runs job(a) and job(b), on two cores when run2 is given.
static void run_two(picture16_run2_fn run2, picture16_job_fn job, void *a,
                    void *b) {
  if (run2 != NULL) {
    run2(job, a, b);
  } else {
    job(a);
    job(b);
  }
}

// --- Histogram ------------------------------------------------------------------

// One pixel's grid colour into the histogram. `k` is the luma's weight
// (298), `r`, `g`, `b` the chroma terms with the luma's offset folded in, `ct`
// the clamp table's 0.
static inline void count_pixel(uint16_t *hist, const uint8_t *ct, int luma,
                               int k, int r, int g, int b) {
  int c = mul(k, luma);
  int bin = ((ct[(c + r) >> 8] & 0xF0) << 4) | (ct[(c + g) >> 8] & 0xF0) |
            (ct[(c + b) >> 8] >> 4);
  hist[bin]++;
}

// Every `step`th chroma row from `cy0` into `hist`.
static void HOT(count_colours)(const uint8_t *y, const uint8_t *cb,
                               const uint8_t *cr, int width, int height,
                               int cy0, int step, uint16_t *hist) {
  // A count cannot overflow: one per 2 x 2 block, 16,000 at 320 x 200.
  memset(hist, 0, BINS * sizeof(uint16_t));
  const uint8_t *ct = clamp_table + CLAMP_LOW;
  const int k = 298;
  int cw = width / 2;
  for (int cy = cy0; cy < height / 2; cy += step) {
    const uint8_t *y0 = y + (2 * cy) * width;
    const uint8_t *y1 = y0 + width;
    const uint8_t *pcb = cb + cy * cw;
    const uint8_t *pcr = cr + cy * cw;
    for (int cx = 0; cx < cw; cx++) {
      chroma_t t = chroma_terms(pcb[cx], pcr[cx]);
      const int off = 128 - 298 * 16;
      int r = t.r + off, g = t.g + off, b = t.b + off;
      // The colour of the 2 x 2 block that shares this chroma sample: the
      // palette is chosen at the chroma's resolution, where the colour is
      // (as good as all four pixels, measured over 5 clips, and 4x less).
      int avg = (y0[2 * cx] + y0[2 * cx + 1] + y1[2 * cx] + y1[2 * cx + 1] +
                 2) >> 2;
      count_pixel(hist, ct, avg, k, r, g, b);
    }
  }
}

// --- Median cut -----------------------------------------------------------------

typedef struct {
  uint8_t lo[3];
  uint8_t hi[3];
  uint32_t count;
  uint64_t error;  // sum of the squared distances to the mean, 4-bit units
} box_t;

static inline int bin_index(int r, int g, int b) { return (r << 8) | (g << 4) | b; }

// Tightens the box to its populated cells and measures them; returns its
// pixel count.
static uint32_t shrink(const uint16_t *hist, box_t *box) {
  uint8_t lo[3] = {15, 15, 15};
  uint8_t hi[3] = {0, 0, 0};
  // 32 bits hold the sums: a histogram counts 16,000 blocks, or by square
  // root at most 4 x sqrt(4,096 x 16,000) < 32,400; a square is 675 at most.
  uint32_t count = 0;
  uint32_t sum[3] = {0, 0, 0};
  uint32_t squares = 0;
  for (int r = box->lo[0]; r <= box->hi[0]; r++) {
    for (int g = box->lo[1]; g <= box->hi[1]; g++) {
      for (int b = box->lo[2]; b <= box->hi[2]; b++) {
        uint32_t n = hist[bin_index(r, g, b)];
        if (n == 0) {
          continue;
        }
        count += n;
        sum[0] += n * (uint32_t)r;
        sum[1] += n * (uint32_t)g;
        sum[2] += n * (uint32_t)b;
        squares += n * (uint32_t)(r * r + g * g + b * b);
        int c[3] = {r, g, b};
        for (int a = 0; a < 3; a++) {
          if (c[a] < lo[a]) {
            lo[a] = (uint8_t)c[a];
          }
          if (c[a] > hi[a]) {
            hi[a] = (uint8_t)c[a];
          }
        }
      }
    }
  }
  if (count > 0) {
    memcpy(box->lo, lo, 3);
    memcpy(box->hi, hi, 3);
    uint64_t s0 = sum[0], s1 = sum[1], s2 = sum[2];
    box->error = squares - (s0 * s0 + s1 * s1 + s2 * s2) / count;
  } else {
    box->error = 0;
  }
  box->count = count;
  return count;
}

// Splits `box` along its longest side at the median of its pixels.
static void split(const uint16_t *hist, box_t *box, box_t *other) {
  int axis = 0;
  for (int a = 1; a < 3; a++) {
    if (box->hi[a] - box->lo[a] > box->hi[axis] - box->lo[axis]) {
      axis = a;
    }
  }
  uint32_t marginal[16] = {0};
  for (int r = box->lo[0]; r <= box->hi[0]; r++) {
    for (int g = box->lo[1]; g <= box->hi[1]; g++) {
      for (int b = box->lo[2]; b <= box->hi[2]; b++) {
        int c[3] = {r, g, b};
        marginal[c[axis]] += hist[bin_index(r, g, b)];
      }
    }
  }
  uint32_t half = box->count / 2;
  uint32_t cum = 0;
  int at = box->lo[axis];
  for (; at < box->hi[axis]; at++) {
    cum += marginal[at];
    if (cum >= half) {
      break;
    }
  }
  if (at >= box->hi[axis]) {
    at = box->hi[axis] - 1;
  }
  *other = *box;
  box->hi[axis] = (uint8_t)at;
  other->lo[axis] = (uint8_t)(at + 1);
  shrink(hist, box);
  shrink(hist, other);
}

#define REFINE_ROUNDS 3

// A gun's 8-bit value: of a 4-bit histogram bin (its centre), and of a
// palette level (a 4-bit gun as stored in rgb444) on the target.
static inline int bin8(int bin) { return bin * 16 + 8; }

static inline int level8(int gun4, int gun_bits) {
  return (gun_bits == 3) ? (gun4 >> 1) * 255 / 7 : gun4 * 17;
}

// The target's level nearest to an 8-bit value, as a 4-bit gun.
static inline int to_level(int v8, int gun_bits) {
  if (gun_bits == 3) {
    return ((v8 * 7 + 127) / 255) << 1;
  }
  return (v8 + 8) / 17;
}

static inline uint16_t pack(const int c4[3]) {
  return (uint16_t)((c4[0] << 8) | (c4[1] << 4) | c4[2]);
}

// The entry nearest to an 8-bit colour.
static inline int nearest(const int (*pal8)[3], int n, int r, int g, int b) {
  int best = 0;
  int best_d = 1 << 30;
  for (int i = 0; i < n; i++) {
    int dr = r - pal8[i][0], dg = g - pal8[i][1], db = b - pal8[i][2];
    int d = dr * dr + dg * dg + db * db;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

static void palette_8bit(const picture16_palette_t *palette, int gun_bits,
                         int (*pal8)[3]) {
  for (int i = 0; i < palette->colours; i++) {
    uint16_t c = palette->rgb444[i];
    pal8[i][0] = level8((c >> 8) & 15, gun_bits);
    pal8[i][1] = level8((c >> 4) & 15, gun_bits);
    pal8[i][2] = level8(c & 15, gun_bits);
  }
}

// k-means over the histogram: each populated colour to its nearest entry,
// each entry to the mean of its colours, rounded to the target's levels (an
// entry no colour chose stays). The colours are shared out between two jobs,
// alternate bins each, whose sums are then added.
typedef struct {
  const uint16_t *hist;
  const int (*pal8)[3];
  int n;
  int first;  // the job's bins: first, first + 2, ...
  uint32_t sum[MAX_COLOURS][3];
  uint32_t count[MAX_COLOURS];
} refine_job_t;

static void HOT(refine_job)(void *arg) {
  refine_job_t *j = (refine_job_t *)arg;
  memset(j->sum, 0, sizeof(j->sum));
  memset(j->count, 0, sizeof(j->count));
  for (int i = j->first; i < BINS; i += 2) {
    uint32_t k = j->hist[i];
    if (k == 0) {
      continue;
    }
    int r = bin8(i >> 8), g = bin8((i >> 4) & 15), b = bin8(i & 15);
    int e = nearest(j->pal8, j->n, r, g, b);
    j->sum[e][0] += k * (uint32_t)r;
    j->sum[e][1] += k * (uint32_t)g;
    j->sum[e][2] += k * (uint32_t)b;
    j->count[e] += k;
  }
}

static void refine(const uint16_t *hist, int gun_bits,
                   picture16_palette_t *palette, picture16_run2_fn run2) {
  int n = palette->colours;
  int pal8[MAX_COLOURS][3];
  refine_job_t even = {hist, (const int (*)[3])pal8, n, 0, {{0}}, {0}};
  refine_job_t odd = {hist, (const int (*)[3])pal8, n, 1, {{0}}, {0}};
  for (int round = 0; round < REFINE_ROUNDS; round++) {
    palette_8bit(palette, gun_bits, pal8);
    run_two(run2, refine_job, &odd, &even);
    for (int e = 0; e < n; e++) {
      uint32_t count = even.count[e] + odd.count[e];
      if (count == 0) {
        continue;
      }
      int c[3];
      for (int a = 0; a < 3; a++) {
        uint32_t sum = even.sum[e][a] + odd.sum[e][a];
        c[a] = to_level((int)((sum + count / 2) / count), gun_bits);
      }
      palette->rgb444[e] = pack(c);
    }
  }
}

static int median_cut(const uint16_t *hist, int gun_bits,
                      picture16_palette_t *palette, picture16_run2_fn run2,
                      picture16_profile_t *profile) {
  box_t boxes[MAX_COLOURS];
  boxes[0] = (box_t){{0, 0, 0}, {15, 15, 15}, 0, 0};
  int n = shrink(hist, &boxes[0]) > 0 ? 1 : 0;
  while (n > 0 && n < MAX_COLOURS) {
    int best = -1;
    uint64_t best_error = 0;
    for (int i = 0; i < n; i++) {
      bool splittable = false;
      for (int a = 0; a < 3; a++) {
        splittable |= boxes[i].hi[a] > boxes[i].lo[a];
      }
      if (splittable && boxes[i].error > best_error) {
        best_error = boxes[i].error;
        best = i;
      }
    }
    if (best < 0) {
      break;  // every box is a single colour
    }
    split(hist, &boxes[best], &boxes[n]);
    n++;
  }
  for (int i = 0; i < n; i++) {
    uint32_t sum[3] = {0, 0, 0};
    uint32_t count = 0;
    for (int r = boxes[i].lo[0]; r <= boxes[i].hi[0]; r++) {
      for (int g = boxes[i].lo[1]; g <= boxes[i].hi[1]; g++) {
        for (int b = boxes[i].lo[2]; b <= boxes[i].hi[2]; b++) {
          uint32_t k = hist[bin_index(r, g, b)];
          sum[0] += k * (uint32_t)r;
          sum[1] += k * (uint32_t)g;
          sum[2] += k * (uint32_t)b;
          count += k;
        }
      }
    }
    int c[3];
    for (int a = 0; a < 3; a++) {
      // The mean in 8-bit (bin centres), rounded to the target's levels.
      uint32_t mean16 = count ? (sum[a] * 16u + count / 2) / count : 0;
      c[a] = to_level((int)mean16 + 8, gun_bits);
    }
    palette->rgb444[i] = pack(c);
  }
  for (int i = n; i < MAX_COLOURS; i++) {
    palette->rgb444[i] = 0;
  }
  palette->colours = n;
  uint32_t t0 = now(profile);
  refine(hist, gun_bits, palette, run2);
  if (profile != NULL) {
    profile->refine = since(profile, t0);
  }
  return n;
}

// --- Table and dither -----------------------------------------------------------

static void build_table(const picture16_palette_t *palette, int gun_bits,
                        uint8_t *table) {
  int n = palette->colours > 0 ? palette->colours : 1;
  int pal8[MAX_COLOURS][3] = {{0, 0, 0}};
  palette_8bit(palette, gun_bits, pal8);
  for (int r = 0; r < 16; r++) {
    for (int g = 0; g < 16; g++) {
      for (int b = 0; b < 16; b++) {
        table[bin_index(r, g, b)] =
            (uint8_t)nearest(pal8, n, bin8(r), bin8(g), bin8(b));
      }
    }
  }
}

// The histogram counted by the square root of its pixels (integer, the same
// on every machine).
static void weigh_sqrt(uint16_t *hist) {
  for (int i = 0; i < BINS; i++) {
    uint32_t k = hist[i];
    if (k == 0) {
      continue;
    }
    uint32_t r = 0;
    for (uint32_t bit = 1u << 8; bit != 0; bit >>= 1) {
      if ((r + bit) * (r + bit) <= k) {
        r += bit;
      }
    }
    hist[i] = (uint16_t)(r * 4u);  // 4 x sqrt: a single pixel weighs 4
  }
}

// The colour of every pixel through the table, with an ordered offset
// (`offsets`, 8x8; NULL for none) added first.
static void dither(uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                   int width, int height, const uint8_t *table,
                   const int8_t *offsets) {
  static const int8_t none[64] = {0};
  if (offsets == NULL) {
    offsets = none;
  }
  int cw = width / 2;
  for (int cy = 0; cy < height / 2; cy++) {
    uint8_t *rows[2] = {y + (2 * cy) * width, y + (2 * cy + 1) * width};
    const int8_t *dy[2] = {&offsets[((2 * cy) & 7) * 8],
                           &offsets[((2 * cy + 1) & 7) * 8]};
    for (int cx = 0; cx < cw; cx++) {
      int d = cb[cy * cw + cx] - 128;
      int e = cr[cy * cw + cx] - 128;
      int tr = 409 * e;
      int tg = -100 * d - 208 * e;
      int tb = 516 * d;
      for (int k = 0; k < 4; k++) {
        int x = 2 * cx + (k & 1);
        uint8_t *p = &rows[k >> 1][x];
        int c = 298 * (*p - 16) + 128;
        int t = dy[k >> 1][x & 7];
        int r = CLAMP(((c + tr) >> 8) + t);
        int g = CLAMP(((c + tg) >> 8) + t);
        int b = CLAMP(((c + tb) >> 8) + t);
        *p = table[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
      }
    }
  }
}

// --- Mixing ---------------------------------------------------------------------
//
// For each colour of the 4-bit grid (its centre in 8-bit): the single entry,
// or the pair among its MIX_CANDIDATES nearest entries, whose mix approaches
// it best. A pair's proportion projects the colour on the line between its
// two entries; the cost is the mix's squared error plus a share of the
// pair's own squared distance, so that close entries are mixed rather than
// far ones (a pattern of distant colours reads as noise). The table keeps
// the choice per grid colour; the proportion itself is computed for each
// pixel from its own colour, so a gradient inside one grid cell still moves.
//
// The work memory then holds: the table (a byte per grid colour: a pair, or
// MIX_SINGLE + an entry, MIX_UNKNOWN until the dither first meets that
// colour), then the entries in 8-bit and every pair of them.

#define MIX_CANDIDATES 6
#define MIX_PENALTY_SHIFT 4  // the pair's distance counts 1/16
#define MIX_SINGLE 0x80u

typedef struct {
  int32_t ab[3];  // second - first, 8-bit
  int32_t k;      // first . ab: c . ab - k = (c - first) . ab
  int32_t recip;  // 64 << 16 / |ab|^2: dot * recip >> 16 = sixty-fourths
  uint8_t first;
  uint8_t second;
  uint8_t pad[2];
  uint32_t penalty;  // the pair's own cost: |ab|^2 x 4096 >> MIX_PENALTY_SHIFT
  uint8_t pad2[4];   // 32 bytes: picture16_asm.S finds a pair by a shift
} mix_pair_t;

_Static_assert(sizeof(mix_pair_t) == 32 &&
                   offsetof(mix_pair_t, recip) == 16 &&
                   offsetof(mix_pair_t, first) == 20,
               "picture16_asm.S's pair layout");

#define MIX_PAIRS (MAX_COLOURS * (MAX_COLOURS - 1) / 2)

static int pair_index(int i, int j) {  // i < j
  return i * (2 * MAX_COLOURS - i - 1) / 2 + (j - i - 1);
}

typedef struct {
  int n;                     // entries in use
  int pal8[MAX_COLOURS][3];  // the entries in 8-bit
  mix_pair_t pairs[MIX_PAIRS];
} mix_t;

#define MIX_UNKNOWN 0xFFu

_Static_assert(BINS + sizeof(mix_t) <= PICTURE16_WORK_BYTES,
               "the mixing table and pairs fit the work memory");

static void build_mix(const picture16_palette_t *palette, int gun_bits,
                      uint8_t *table, mix_t *mix) {
  mix->n = palette->colours > 0 ? palette->colours : 1;
  memset(mix->pal8, 0, sizeof(mix->pal8));
  palette_8bit(palette, gun_bits, mix->pal8);
  for (int i = 0; i < mix->n; i++) {
    for (int j = i + 1; j < mix->n; j++) {
      mix_pair_t *pr = &mix->pairs[pair_index(i, j)];
      int len2 = 0;
      pr->k = 0;
      for (int k = 0; k < 3; k++) {
        pr->ab[k] = mix->pal8[j][k] - mix->pal8[i][k];
        pr->k += mix->pal8[i][k] * pr->ab[k];
        len2 += pr->ab[k] * pr->ab[k];
      }
      pr->recip = len2 ? (int32_t)((64 << 16) / len2) : 0;
      pr->penalty = ((uint32_t)len2 * 4096u) >> MIX_PENALTY_SHIFT;
      pr->first = (uint8_t)i;
      pr->second = (uint8_t)j;
    }
  }
  // Filled as the dither meets each colour: a picture uses a few hundred of
  // the 4,096.
  memset(table, MIX_UNKNOWN, BINS);
}

// The choice for one grid colour: a pair, or MIX_SINGLE + an entry.
// In flash: it runs a few hundred times a picture, and RAM is short.
static uint8_t mix_choose(const mix_t *mix, int bin) {
  int c[3] = {bin8(bin >> 8), bin8((bin >> 4) & 15), bin8(bin & 15)};
  // The MIX_CANDIDATES nearest entries, nearest first, a tie to the lower
  // entry: keys of distance << 4 | entry, in order.
  int diff[MAX_COLOURS][3];  // colour - entry
  uint32_t near[MIX_CANDIDATES];
  int found = 0;
  for (int i = 0; i < mix->n; i++) {
    int *d = diff[i];
    d[0] = c[0] - mix->pal8[i][0];
    d[1] = c[1] - mix->pal8[i][1];
    d[2] = c[2] - mix->pal8[i][2];
    uint32_t key =
        ((uint32_t)(mul(d[0], d[0]) + mul(d[1], d[1]) + mul(d[2], d[2])) << 4) |
        (uint32_t)i;
    int at;
    if (found < MIX_CANDIDATES) {
      at = found++;
    } else if (key < near[MIX_CANDIDATES - 1]) {
      at = MIX_CANDIDATES - 1;
    } else {
      continue;
    }
    while (at > 0 && near[at - 1] > key) {
      near[at] = near[at - 1];
      at--;
    }
    near[at] = key;
  }
  // Costs in 4096ths (a mix is computed in 64ths of a level).
  uint32_t best = (near[0] >> 4) * 4096u;
  uint8_t choice = (uint8_t)(MIX_SINGLE | (near[0] & 15u));
  for (int a = 0; a < found; a++) {
    for (int b = a + 1; b < found; b++) {
      int ea = (int)(near[a] & 15u), eb = (int)(near[b] & 15u);
      int i = ea < eb ? ea : eb;
      int j = ea < eb ? eb : ea;
      int index = pair_index(i, j);
      const mix_pair_t *pr = &mix->pairs[index];
      if (pr->recip == 0 || pr->penalty >= best) {
        continue;  // the same entry twice, or a pair that cannot win
      }
      const int *d = diff[i];
      int dot = mul(d[0], pr->ab[0]) + mul(d[1], pr->ab[1]) +
                mul(d[2], pr->ab[2]);
      int t = mul(dot, pr->recip) >> 16;
      if (t <= 0 || t >= 64) {
        continue;  // beyond either entry: a single does that
      }
      int e0 = d[0] * 64 - mul(pr->ab[0], t);
      int e1 = d[1] * 64 - mul(pr->ab[1], t);
      int e2 = d[2] * 64 - mul(pr->ab[2], t);
      uint32_t err = (uint32_t)mul(e0, e0) + (uint32_t)mul(e1, e1) +
                     (uint32_t)mul(e2, e2) + pr->penalty;
      if (err < best) {
        best = err;
        choice = (uint8_t)index;
      }
    }
  }
  return choice;
}

// A chroma sample's share of its 2 x 2 pixels' colours: the chroma terms
// with the luma's offset folded in (a pixel's red is 298 x luma + r, 8.8),
// and the four pixels' thresholds (0..63): the first row's two, then the
// second's.
typedef struct {
  int32_t r;
  int32_t g;
  int32_t b;
  uint8_t th[4];
} mix_terms_t;

_Static_assert(sizeof(mix_terms_t) == 16 && offsetof(mix_terms_t, th) == 12,
               "picture16_asm.S's terms layout");

// Chroma samples per call of the row loop: their terms are on the stack.
#define MIX_CHUNK 32

#if defined(__ARM_ARCH_6M__)
// Both luma rows of a chroma row's samples [y, end) / 2, in picture16_asm.S.
typedef struct {
  uint8_t *y;               // the first row's first pixel, overwritten with
  uint8_t *end;             // its index; the second row is `width` further
  const mix_terms_t *terms;
  uint8_t *table;
  const mix_pair_t *pairs;
  int width;
  const mix_t *mix;  // for choose(), on a colour the table has not met yet
  uint8_t (*choose)(const mix_t *mix, int bin);
} mix_rows_t;

_Static_assert(offsetof(mix_rows_t, width) == 20 &&
                   offsetof(mix_rows_t, choose) == 28,
               "picture16_asm.S's argument layout");

void p16_mix_rows(const mix_rows_t *a);
#else
// One pixel of the mixing dither: its colour from its luma and its sample's
// terms, its choice in the table (made now if the colour is new), and the
// threshold `th` against its proportion.
static inline uint8_t mix_pixel(int luma, const mix_terms_t *t, int th,
                                uint8_t *table, const mix_t *mix) {
  int c = 298 * luma;
  int r = CLAMP((c + t->r) >> 8);
  int g = CLAMP((c + t->g) >> 8);
  int b = CLAMP((c + t->b) >> 8);
  int bin = ((r & 0xF0) << 4) | (g & 0xF0) | (b >> 4);
  uint8_t choice = table[bin];
  if (choice == MIX_UNKNOWN) {
    choice = mix_choose(mix, bin);
    table[bin] = choice;
  }
  if (choice & MIX_SINGLE) {
    return choice & 15u;
  }
  const mix_pair_t *pr = &mix->pairs[choice];
  int dot = r * pr->ab[0] + g * pr->ab[1] + b * pr->ab[2] - pr->k;
  int u = (dot * pr->recip) >> 16;  // 64ths of the second entry
  return (th < u) ? pr->second : pr->first;
}
#endif

// Every `step`th chroma row of [cy0, cy1) of the picture.
static void HOT(dither_mix)(uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                            int width, int cy0, int cy1, int step,
                            uint8_t *table, const mix_t *mix) {
  int cw = width / 2;
  mix_terms_t terms[MIX_CHUNK];
  for (int cy = cy0; cy < cy1; cy += step) {
    uint8_t *y0 = y + (2 * cy) * width;
    const uint8_t *th0 = &bayer_rank[((2 * cy) & 7) * 8];
    const uint8_t *th1 = &bayer_rank[((2 * cy + 1) & 7) * 8];
    const uint8_t *pcb = cb + cy * cw;
    const uint8_t *pcr = cr + cy * cw;
    for (int cx0 = 0; cx0 < cw; cx0 += MIX_CHUNK) {
      int n = (cw - cx0 < MIX_CHUNK) ? cw - cx0 : MIX_CHUNK;
      for (int i = 0; i < n; i++) {
        int cx = cx0 + i;
        int x = 2 * cx;
        chroma_t t = chroma_terms(pcb[cx], pcr[cx]);
        const int off = 128 - 298 * 16;
        terms[i].r = t.r + off;
        terms[i].g = t.g + off;
        terms[i].b = t.b + off;
        terms[i].th[0] = th0[x & 7];
        terms[i].th[1] = th0[(x + 1) & 7];
        terms[i].th[2] = th1[x & 7];
        terms[i].th[3] = th1[(x + 1) & 7];
      }
#if defined(__ARM_ARCH_6M__)
      mix_rows_t rows = {y0 + 2 * cx0, y0 + 2 * (cx0 + n), terms, table,
                         mix->pairs, width, mix, mix_choose};
      p16_mix_rows(&rows);
#else
      for (int i = 0; i < n; i++) {
        uint8_t *p = y0 + 2 * (cx0 + i);
        p[0] = mix_pixel(p[0], &terms[i], terms[i].th[0], table, mix);
        p[1] = mix_pixel(p[1], &terms[i], terms[i].th[1], table, mix);
        p[width] =
            mix_pixel(p[width], &terms[i], terms[i].th[2], table, mix);
        p[width + 1] =
            mix_pixel(p[width + 1], &terms[i], terms[i].th[3], table, mix);
      }
#endif
    }
  }
}

// --- Two cores ------------------------------------------------------------------

typedef struct {
  uint8_t *y;
  const uint8_t *cb;
  const uint8_t *cr;
  int width;
  int cy0;
  int cy1;
  int step;
  uint8_t *table;
  const mix_t *mix;
} mix_job_t;

static void HOT(mix_job)(void *arg) {
  mix_job_t *j = (mix_job_t *)arg;
  dither_mix(j->y, j->cb, j->cr, j->width, j->cy0, j->cy1, j->step, j->table,
             j->mix);
}

typedef struct {
  const uint8_t *y;
  const uint8_t *cb;
  const uint8_t *cr;
  int width;
  int height;
  int cy0;
  uint16_t *hist;
} count_job_t;

static void HOT(count_job)(void *arg) {
  count_job_t *j = (count_job_t *)arg;
  count_colours(j->y, j->cb, j->cr, j->width, j->height, j->cy0, 2, j->hist);
}

// The histogram: on two cores, even and odd chroma rows each into their own
// and then added, when there are two and the memory for the second.
static void histogram(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                      int width, int height,
                      const picture16_options_t *options, uint16_t *hist) {
  uint16_t *hist2 = (options != NULL) ? (uint16_t *)options->work2 : NULL;
  if (hist2 == NULL || options->run2 == NULL) {
    count_colours(y, cb, cr, width, height, 0, 1, hist);
    return;
  }
  count_job_t even = {y, cb, cr, width, height, 0, hist};
  count_job_t odd = {y, cb, cr, width, height, 1, hist2};
  options->run2(count_job, &odd, &even);
  for (int i = 0; i < BINS; i++) {
    hist[i] = (uint16_t)(hist[i] + hist2[i]);
  }
}

// --- Public ---------------------------------------------------------------------

void picture16_convert(uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                       int width, int height, void *work,
                       const picture16_options_t *options,
                       picture16_palette_t *palette,
                       picture16_profile_t *profile) {
  int gun_bits = (options != NULL && options->gun_bits == 3) ? 3 : 4;
  if (!bayer_ready) {
    build_bayer();
  }
  uint16_t *hist = (uint16_t *)work;
  uint32_t t0 = now(profile);
  histogram(y, cb, cr, width, height, options, hist);
  if (profile != NULL) {
    profile->histogram = since(profile, t0);
  }
  t0 = now(profile);
  if (options != NULL && options->weighting == PICTURE16_WEIGHT_SQRT) {
    weigh_sqrt(hist);
  }
  median_cut(hist, gun_bits, palette,
             (options != NULL) ? options->run2 : NULL, profile);
  if (profile != NULL) {
    profile->palette = since(profile, t0);
  }
  // The histogram is no longer needed: its memory holds the table.
  int mode = (options != NULL) ? options->dither : PICTURE16_DITHER_BAYER;
  t0 = now(profile);
  mix_t *mix = (mix_t *)((uint8_t *)work + BINS);
  if (mode == PICTURE16_DITHER_MIX) {
    build_mix(palette, gun_bits, (uint8_t *)work, mix);
  } else {
    build_table(palette, gun_bits, (uint8_t *)work);
  }
  if (profile != NULL) {
    profile->table = since(profile, t0);
  }
  t0 = now(profile);
  if (mode == PICTURE16_DITHER_MIX) {
    // Even and odd chroma rows, on two cores when there are: halves of the
    // picture as alike as can be.
    mix_job_t even = {y, cb, cr, width, 0, height / 2, 2, (uint8_t *)work,
                      mix};
    mix_job_t odd = {y, cb, cr, width, 1, height / 2, 2, (uint8_t *)work,
                     mix};
    run_two(options != NULL ? options->run2 : NULL, mix_job, &odd, &even);
  } else {
    dither(y, cb, cr, width, height, (const uint8_t *)work,
           mode == PICTURE16_DITHER_BAYER        ? bayer
           : mode == PICTURE16_DITHER_BAYER_SOFT ? bayer_soft
                                                 : NULL);
  }
  if (profile != NULL) {
    profile->dither = since(profile, t0);
  }
}

uint16_t picture16_ste_word(uint16_t rgb444) {
  uint16_t word = 0;
  for (int shift = 0; shift <= 8; shift += 4) {
    uint16_t gun = (rgb444 >> shift) & 15u;
    word |= (uint16_t)(((gun >> 1) | ((gun & 1u) << 3)) << shift);
  }
  return word;
}

// --- Scaling --------------------------------------------------------------------

#define SCALE_MAX_OUT_W 640

static uint16_t xmap[SCALE_MAX_OUT_W];
static int xmap_src_w;
static int xmap_out_w;

void picture16_scale_mb_row(int mb_row, const uint8_t *y,
                            const uint8_t *cb, const uint8_t *cr,
                            int stride, int src_w, int src_h,
                            uint8_t *out_y, uint8_t *out_cb,
                            uint8_t *out_cr, int out_w, int out_h) {
  if (out_w > SCALE_MAX_OUT_W) {
    return;
  }
  if (xmap_src_w != src_w || xmap_out_w != out_w) {
    for (int x = 0; x < out_w; x++) {
      xmap[x] = (uint16_t)(x * src_w / out_w);
    }
    xmap_src_w = src_w;
    xmap_out_w = out_w;
  }
  // Luma: the output lines whose source line falls in these 16.
  int first = mb_row * 16;
  for (int oy = (first * out_h + src_h - 1) / src_h; oy < out_h; oy++) {
    int sy = oy * src_h / out_h;
    if (sy >= first + 16) {
      break;
    }
    const uint8_t *src = y + (sy - first) * stride;
    uint8_t *dst = out_y + oy * out_w;
    for (int x = 0; x < out_w; x++) {
      dst[x] = src[xmap[x]];
    }
  }
  // Chroma: half the sizes; the same x map (a chroma pixel per two luma).
  int cw = out_w / 2;
  int ch = out_h / 2;
  int csrc_h = src_h / 2;
  int cfirst = mb_row * 8;
  int cstride = stride / 2;
  for (int oy = (cfirst * ch + csrc_h - 1) / csrc_h; oy < ch; oy++) {
    int sy = oy * csrc_h / ch;
    if (sy >= cfirst + 8) {
      break;
    }
    const uint8_t *scb = cb + (sy - cfirst) * cstride;
    const uint8_t *scr = cr + (sy - cfirst) * cstride;
    uint8_t *dcb = out_cb + oy * cw;
    uint8_t *dcr = out_cr + oy * cw;
    for (int x = 0; x < cw; x++) {
      dcb[x] = scb[xmap[x]];
      dcr[x] = scr[xmap[x]];
    }
  }
}

// --- Lanczos scaler ---------------------------------------------------------------
//
// The ring is column-major: a column of PICTURE16_RING_LINES bytes per output
// pixel across, so that the 8 lines an output line reads down sit at fixed
// offsets. A batch of 8 luma lines goes to slots 8..15 of every column, the
// previous batch moved to 0..7 first; a batch of 4 chroma lines to 12..15,
// the previous three moved to 0..11. An output line's taps then never wrap:
// it is computed in the batch of its last tap, so its first tap is at most
// 7 lines earlier.

#define RING_Y_NEW 8   // a luma batch's first slot
#define RING_C_NEW 12  // a chroma batch's first slot

_Static_assert(SCALER_X_PERIOD_SRC == 11 &&
                   SCALER_X_PERIOD * PICTURE16_RING_LINES == 160 &&
                   PICTURE16_RING_LINES == 16,
               "picture16_asm.S's steps");

// The inner loops: picture16_asm.S on the RP, these elsewhere; the same
// arithmetic, so the same bytes.
#if defined(__ARM_ARCH_6M__)
void p16_down(const uint8_t *src, uint8_t *dst, uint8_t *end,
              const int16_t *w);
void p16_across(const uint8_t *src, uint8_t *dst, uint8_t *end,
                const int16_t *w);
#else
static inline uint8_t fir_clamp(int sum) {
  int v = (sum + (1 << (SCALER_SHIFT - 1))) >> SCALER_SHIFT;
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// An output line down: out[x] from src[16 x + k], k < 8.
static void p16_down(const uint8_t *src, uint8_t *dst, uint8_t *end,
                     const int16_t *w) {
  for (; dst < end; dst++, src += PICTURE16_RING_LINES) {
    *dst = fir_clamp(w[0] * src[0] + w[1] * src[1] + w[2] * src[2] +
                     w[3] * src[3] + w[4] * src[4] + w[5] * src[5] +
                     w[6] * src[6] + w[7] * src[7]);
  }
}

// One phase across: every SCALER_X_PERIOD-th output of a line, 7 taps each,
// down a ring column.
static void p16_across(const uint8_t *src, uint8_t *dst, uint8_t *end,
                       const int16_t *w) {
  for (; dst < end; dst += SCALER_X_PERIOD * PICTURE16_RING_LINES,
                    src += SCALER_X_PERIOD_SRC) {
    *dst = fir_clamp(w[0] * src[0] + w[1] * src[1] + w[2] * src[2] +
                     w[3] * src[3] + w[4] * src[4] + w[5] * src[5] +
                     w[6] * src[6]);
  }
}
#endif

// Outputs [x0, x1) of a line across one by one (the edges, where taps fold
// and the weights do not repeat), into ring slot `col` of their columns.
static void across_edge(const scaler_tap_t *taps, int x0, int x1,
                        const uint8_t *src, uint8_t *col) {
  for (int x = x0; x < x1; x++) {
    const scaler_tap_t *t = &taps[x];
    const uint8_t *p = src + t->first;
    const int16_t *w = t->weight;
    int sum = (1 << (SCALER_SHIFT - 1)) + mul(w[0], p[0]) + mul(w[1], p[1]) +
              mul(w[2], p[2]) + mul(w[3], p[3]) + mul(w[4], p[4]) +
              mul(w[5], p[5]) + mul(w[6], p[6]);
    col[x * PICTURE16_RING_LINES] = CLAMP(sum >> SCALER_SHIFT);
  }
}

// A source line across into its slot of the ring's columns (`col`: the
// slot of column 0); [lo, hi) are the outputs that repeat by phase.
static void scale_across(const scaler_tap_t *taps, int lo, int hi, int out_w,
                         const uint8_t *src, uint8_t *col) {
  across_edge(taps, 0, lo, src, col);
  for (int x = lo; x < lo + SCALER_X_PERIOD; x++) {
    int n = (hi - x + SCALER_X_PERIOD - 1) / SCALER_X_PERIOD;
    uint8_t *dst = col + x * PICTURE16_RING_LINES;
    p16_across(src + taps[x].first, dst,
               dst + n * SCALER_X_PERIOD * PICTURE16_RING_LINES,
               taps[x].weight);
  }
  across_edge(taps, hi, out_w, src, col);
}

// Moves, in every column of a ring, the 4-byte word at slot `from` to slot
// `to`.
static void HOT(ring_move)(uint8_t *ring, int columns, int from, int to) {
  uint32_t *w = (uint32_t *)ring;
  uint32_t *end = w + columns * (PICTURE16_RING_LINES / 4);
  for (int f = from / 4, t = to / 4; w < end; w += PICTURE16_RING_LINES / 4) {
    w[t] = w[f];
  }
}

void picture16_scaler_init(picture16_scaler_t *s, int src_w, int src_h,
                           uint8_t *ring, uint8_t *out_y, uint8_t *out_cb,
                           uint8_t *out_cr, int out_w, int out_h) {
  if (!bayer_ready) {
    build_bayer();
  }
  memset(s, 0, sizeof(*s));
  s->ring_y = ring;
  s->ring_cb = ring + PICTURE16_RING_LINES * out_w;
  s->ring_cr = s->ring_cb + PICTURE16_RING_LINES * (out_w / 2);
  s->out_y = out_y;
  s->out_cb = out_cb;
  s->out_cr = out_cr;
  s->src_w = src_w;
  s->src_h = src_h;
  s->out_w = out_w;
  s->out_h = out_h;
  s->lanczos = src_w == SCALER_SRC_W && src_h == SCALER_SRC_H &&
               out_w == SCALER_OUT_W && out_h == SCALER_OUT_H;
}

// One core's share of a batch of the scaler: source lines to scale across
// (luma, then Cb or Cr), then output lines to compute down.
typedef struct {
  picture16_scaler_t *s;
  const uint8_t *y;  // the macroblock row's luma lines
  const uint8_t *c;  // its Cb or Cr lines
  uint8_t *c_ring;   // the matching ring
  int stride;
  int y_row0;        // the macroblock row's first luma line
  int c_row0;        // and chroma line
  int y0;            // the batch's first luma line
  int c0;            // and chroma line
  int y_first;       // this core's luma lines [y_first, y_last): slots
  int y_last;        // 8..11 or 12..15
  int c_end;         // the batch's chroma lines [c0, c_end), all this core's
  int out_y0;        // output luma lines [out_y0, out_y1)
  int out_y1;
  int out_c0;        // output chroma lines, both planes
  int out_c1;
  int half;          // this core's half of each output line (0: the left)
} scale_job_t;

static void HOT(scale_across_job)(void *arg) {
  scale_job_t *j = (scale_job_t *)arg;
  picture16_scaler_t *s = j->s;
  int cw = s->out_w / 2;
  // The previous batch's lines in the slots this core's lines take move up
  // first: words of the columns no other core touches.
  int slot = RING_Y_NEW + (j->y_first - j->y0);
  ring_move(s->ring_y, s->out_w, slot, slot - RING_Y_NEW);
  for (int line = j->y_first; line < j->y_last; line++) {
    scale_across(scaler_luma_x, SCALER_LUMA_X_LO, SCALER_LUMA_X_HI,
                 s->out_w, j->y + (line - j->y_row0) * j->stride,
                 s->ring_y + RING_Y_NEW + (line - j->y0));
  }
  for (int from = 4; from < PICTURE16_RING_LINES; from += 4) {
    ring_move(j->c_ring, cw, from, from - 4);
  }
  for (int line = j->c0; line < j->c_end; line++) {
    scale_across(scaler_chroma_x, SCALER_CHROMA_X_LO, SCALER_CHROMA_X_HI, cw,
                 j->c + (line - j->c_row0) * (j->stride / 2),
                 j->c_ring + RING_C_NEW + (line - j->c0));
  }
}

static void HOT(scale_down_job)(void *arg) {
  scale_job_t *j = (scale_job_t *)arg;
  picture16_scaler_t *s = j->s;
  int w = s->out_w / 2;  // this core's share of a luma line
  int x = j->half * w;
  for (int o = j->out_y0; o < j->out_y1; o++) {
    const scaler_tap_t *t = &scaler_luma_y[o];
    uint8_t *dst = s->out_y + o * s->out_w + x;
    p16_down(s->ring_y + (x * PICTURE16_RING_LINES) + RING_Y_NEW +
                 (t->first - j->y0),
             dst, dst + w, t->weight);
  }
  int cw = s->out_w / 2;
  int ch = cw / 2;  // and of a chroma line
  int cx = j->half * ch;
  for (int o = j->out_c0; o < j->out_c1; o++) {
    const scaler_tap_t *t = &scaler_chroma_y[o];
    int at = cx * PICTURE16_RING_LINES + RING_C_NEW + (t->first - j->c0);
    uint8_t *dcb = s->out_cb + o * cw + cx;
    uint8_t *dcr = s->out_cr + o * cw + cx;
    p16_down(s->ring_cb + at, dcb, dcb + ch, t->weight);
    p16_down(s->ring_cr + at, dcr, dcr + ch, t->weight);
  }
}

// The first output line, from `next` on, whose last tap is after `line`.
static int ready_until(const scaler_tap_t *down, int out_h, int next,
                       int line) {
  while (next < out_h && down[next].first + SCALER_TAPS_Y - 1 <= line) {
    next++;
  }
  return next;
}

void picture16_scaler_mb_row(picture16_scaler_t *s, int mb_row,
                             const uint8_t *y, const uint8_t *cb,
                             const uint8_t *cr, int stride) {
  if (!s->lanczos) {
    picture16_scale_mb_row(mb_row, y, cb, cr, stride, s->src_w, s->src_h,
                           s->out_y, s->out_cb, s->out_cr, s->out_w,
                           s->out_h);
    return;
  }
  // Two batches of 8 luma lines (4 chroma): the ring holds the lines a
  // waiting output still needs (7 at most) and one batch.
  for (int half = 0; half < 2; half++) {
    int y0 = mb_row * 16 + half * 8;
    int c0 = mb_row * 8 + half * 4;
    int y_end = (y0 + 8 < s->src_h) ? y0 + 8 : s->src_h;
    int c_end = (c0 + 4 < s->src_h / 2) ? c0 + 4 : s->src_h / 2;
    if (y0 >= y_end) {
      break;
    }
    // Across: one core the batch's first 4 luma lines (slots 8..11) and the
    // Cb lines, the other the last 4 (12..15) and the Cr lines.
    int y_mid = y0 + 4;
    scale_job_t a = {s, y, cb, s->ring_cb, stride, mb_row * 16, mb_row * 8,
                     y0, c0, y0, (y_mid < y_end) ? y_mid : y_end, c_end,
                     0, 0, 0, 0, 0};
    scale_job_t b = {s, y, cr, s->ring_cr, stride, mb_row * 16, mb_row * 8,
                     y0, c0, y_mid, y_end, c_end, 0, 0, 0, 0, 1};
    run_two(s->run2, scale_across_job, &b, &a);
    // Down: the output lines now ready, each core half of every line.
    int ny = ready_until(scaler_luma_y, s->out_h, s->next_y, y_end - 1);
    int nc = ready_until(scaler_chroma_y, s->out_h / 2, s->next_c, c_end - 1);
    a.out_y0 = b.out_y0 = s->next_y;
    a.out_y1 = b.out_y1 = ny;
    a.out_c0 = b.out_c0 = s->next_c;
    a.out_c1 = b.out_c1 = nc;
    run_two(s->run2, scale_down_job, &b, &a);
    s->next_y = ny;
    s->next_c = nc;
  }
}
