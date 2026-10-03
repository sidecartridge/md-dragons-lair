/**
 * File: mpeg1_video.c
 * Description: MPEG-1 video decoder. See mpeg1_video.h.
 *
 * The syntax followed here is ISO/IEC 11172-2 section 2.4: sequence header
 * (0x000001B3), group of pictures (0xB8), picture (0x00), slices
 * (0x01..0xAF, the start code's last byte is the slice's macroblock row + 1)
 * and their macroblocks. Intra blocks: a DC size code and the DC difference
 * against the previous block of the same component (reset to 128 at each
 * slice), then run/level codes until end of block. Coefficients are
 * dequantised as the standard says: (2 * level * quantiser_scale *
 * matrix) / 16, made odd towards zero, clipped to -2048..2047; non-intra
 * blocks ((2 * level + sign) * quantiser_scale * matrix) / 16. The inverse
 * DCT is the integer Chen-Wang algorithm with 11-bit constants
 * (W_k = 2048 * sqrt(2) * cos(k * pi / 16)), rows then columns. P pictures:
 * forward motion vectors in half pels, predicted from the previous
 * macroblock's within a slice (reset at a slice, an intra or a skipped
 * macroblock), the chroma vector half the luma's (towards zero); skipped
 * macroblocks are the reference's, unmoved.
 */

#pragma GCC optimize("O3")

#include "mpeg1_video.h"

#include <string.h>

#include "mpeg1_tables.h"

#if defined(__ARM_ARCH_6M__)
#include "pico.h"
// In RAM, and never inlined into a caller that is not (it would run from
// flash then).
#define HOT(f) __attribute__((noinline)) __not_in_flash_func(f)
#else
#define HOT(f) f
#endif

#define SC_PICTURE 0x00
#define SC_SLICE_FIRST 0x01
#define SC_SLICE_LAST 0xAF
#define SC_SEQUENCE 0xB3
#define SC_SEQUENCE_END 0xB7
#define SC_GROUP 0xB8

#define MB_INTRA 0x01u
#define MB_PATTERN 0x02u
#define MB_FORWARD 0x08u
#define MB_QUANT 0x10u
#define MBA_STUFFING 34
#define MBA_ESCAPE 35

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The default intra matrix, natural order (ISO/IEC 11172-2 2.4.3.2).
static const uint8_t default_intra_q[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// --- Bit reader ---------------------------------------------------------------

static void br_refill(mpeg1_t *m) {
  // The last 4 bytes stay in front of the new data: a start-code search
  // gives back the whole bytes held in the cache, at most 4.
  memmove(m->es, m->end - 4, 4);
  uint32_t got = mpeg_ps_read(m->ps, m->es + 4, MPEG1_ES_BUFFER);
  m->p = m->es + 4;
  m->end = m->es + 4 + got;
  if (got == 0) {
    m->eos = true;
  }
}

// The next byte; zeros past the end of the stream, counted in `fake` so
// that a start-code search never hands them back as stream bytes.
static inline uint32_t br_byte(mpeg1_t *m) {
  if (m->p == m->end) {
    if (!m->eos) {
      br_refill(m);
    }
    if (m->p == m->end) {
      m->fake++;
      return 0;
    }
  }
  return *m->p++;
}

static inline void br_fill(mpeg1_t *m) {
  while (m->bits <= 24) {
    m->cache |= br_byte(m) << (24 - m->bits);
    m->bits += 8;
  }
}

// The next n bits (1..24) without consuming them.
static inline uint32_t br_peek(mpeg1_t *m, int n) {
  br_fill(m);
  return m->cache >> (32 - n);
}

static inline void br_skip(mpeg1_t *m, int n) {
  m->cache <<= n;
  m->bits -= n;
}

static inline uint32_t br_get(mpeg1_t *m, int n) {
  uint32_t v = br_peek(m, n);
  br_skip(m, n);
  return v;
}

// The next start code's value (the byte after 00 00 01), with the reader
// left after it; -1 at the end of the stream.
static int next_start_code(mpeg1_t *m) {
  if (m->pending_code >= 0) {
    int code = m->pending_code;
    m->pending_code = -1;
    return code;
  }
  // Back to a byte boundary, then hand the whole cached bytes back (the
  // zeros read past the end are not stream bytes).
  br_skip(m, m->bits & 7);
  int back = (m->bits >> 3) - m->fake;
  m->p -= (back > 0) ? back : 0;
  m->fake = 0;
  m->bits = 0;
  m->cache = 0;
  uint32_t shift = 0xFFFFFFFFu;
  for (;;) {
    if (m->p == m->end) {
      if (m->eos) {
        return -1;
      }
      br_refill(m);
      if (m->p == m->end) {
        return -1;
      }
    }
    uint32_t b = *m->p++;
    if ((shift & 0xFFFFFFu) == 0x000001u) {
      return (int)b;
    }
    shift = (shift << 8) | b;
  }
}

// --- Inverse DCT ----------------------------------------------------------------

#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

// A multiplication the compiler must not turn into shifts and adds: GCC's
// Cortex-M0+ tuning assumes a slow multiplier, and the RP2040's takes one
// cycle. The host build multiplies plainly; the result is the same.
#if defined(__ARM_ARCH_6M__)
static inline int mul(int a, int b) {
  // Divided syntax, as GCC emits Thumb-1: the 16-bit MULS (flags set).
  __asm__("mul %0, %1" : "+l"(a) : "l"(b) : "cc");
  return a;
}
#else
static inline int mul(int a, int b) { return a * b; }
#endif

static inline uint8_t clamp255(int v) {
  return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

#if defined(__ARM_ARCH_6M__)
// The transforms below in Thumb assembly (mpeg1_idct_asm.S): the same
// integers.
void mpeg1_idct_rows(int16_t *b, uint32_t rows);
void mpeg1_idct_cols_put(const int16_t *b, uint8_t *d, int stride);
void mpeg1_idct_cols_add(const int16_t *b, uint8_t *d, int stride);
void mpeg1_idct_half_put(const int16_t *b, uint8_t *d, int stride);
void mpeg1_idct_half_add(const int16_t *b, uint8_t *d, int stride);
#else
static void HOT(idct_row)(int16_t *b) {
  int x1 = b[4] * 2048, x2 = b[6], x3 = b[2], x4 = b[1], x5 = b[7],
      x6 = b[5], x7 = b[3];
  if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
    int16_t v = (int16_t)(b[0] * 8);
    b[0] = b[1] = b[2] = b[3] = b[4] = b[5] = b[6] = b[7] = v;
    return;
  }
  int x0 = b[0] * 2048 + 128;
  int x8 = mul(W7, (x4 + x5));
  x4 = x8 + mul(W1 - W7, x4);
  x5 = x8 - mul(W1 + W7, x5);
  x8 = mul(W3, (x6 + x7));
  x6 = x8 - mul(W3 - W5, x6);
  x7 = x8 - mul(W3 + W5, x7);
  x8 = x0 + x1;
  x0 -= x1;
  x1 = mul(W6, (x3 + x2));
  x2 = x1 - mul(W2 + W6, x2);
  x3 = x1 + mul(W2 - W6, x3);
  x1 = x4 + x6;
  x4 -= x6;
  x6 = x5 + x7;
  x5 -= x7;
  x7 = x8 + x3;
  x8 -= x3;
  x3 = x0 + x2;
  x0 -= x2;
  x2 = (mul(181, (x4 + x5)) + 128) >> 8;
  x4 = (mul(181, (x4 - x5)) + 128) >> 8;
  b[0] = (int16_t)((x7 + x1) >> 8);
  b[1] = (int16_t)((x3 + x2) >> 8);
  b[2] = (int16_t)((x0 + x4) >> 8);
  b[3] = (int16_t)((x8 + x6) >> 8);
  b[4] = (int16_t)((x8 - x6) >> 8);
  b[5] = (int16_t)((x0 - x4) >> 8);
  b[6] = (int16_t)((x3 - x2) >> 8);
  b[7] = (int16_t)((x7 - x1) >> 8);
}

// A column of the row-transformed block, stored as pixels.
static void HOT(idct_col_put)(const int16_t *b, uint8_t *d, int stride) {
  int x1 = b[32] * 256, x2 = b[48], x3 = b[16], x4 = b[8], x5 = b[56],
      x6 = b[40], x7 = b[24];
  if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
    uint8_t v = clamp255((b[0] + 32) >> 6);
    for (int i = 0; i < 8; i++) {
      d[i * stride] = v;
    }
    return;
  }
  int x0 = b[0] * 256 + 8192;
  int x8 = mul(W7, (x4 + x5)) + 4;
  x4 = (x8 + mul(W1 - W7, x4)) >> 3;
  x5 = (x8 - mul(W1 + W7, x5)) >> 3;
  x8 = mul(W3, (x6 + x7)) + 4;
  x6 = (x8 - mul(W3 - W5, x6)) >> 3;
  x7 = (x8 - mul(W3 + W5, x7)) >> 3;
  x8 = x0 + x1;
  x0 -= x1;
  x1 = mul(W6, (x3 + x2)) + 4;
  x2 = (x1 - mul(W2 + W6, x2)) >> 3;
  x3 = (x1 + mul(W2 - W6, x3)) >> 3;
  x1 = x4 + x6;
  x4 -= x6;
  x6 = x5 + x7;
  x5 -= x7;
  x7 = x8 + x3;
  x8 -= x3;
  x3 = x0 + x2;
  x0 -= x2;
  x2 = (mul(181, (x4 + x5)) + 128) >> 8;
  x4 = (mul(181, (x4 - x5)) + 128) >> 8;
  d[0 * stride] = clamp255((x7 + x1) >> 14);
  d[1 * stride] = clamp255((x3 + x2) >> 14);
  d[2 * stride] = clamp255((x0 + x4) >> 14);
  d[3 * stride] = clamp255((x8 + x6) >> 14);
  d[4 * stride] = clamp255((x8 - x6) >> 14);
  d[5 * stride] = clamp255((x0 - x4) >> 14);
  d[6 * stride] = clamp255((x3 - x2) >> 14);
  d[7 * stride] = clamp255((x7 - x1) >> 14);
}

// `rows`: bit r set when row r of the block has a coefficient. Rows without
// any stay zero through the row pass; with row 0 alone every column is flat.
// The block is left zero.
// A column whose rows 4..7 are zero (x1, x2, x5, x6 = 0): the same integers
// as idct_col_put / idct_col_add with those terms dropped. `add`: added to
// the prediction in `d` instead of stored.
static void HOT(idct_col_half)(const int16_t *b, uint8_t *d, int stride,
                               bool add) {
  int x3 = b[16], x4 = b[8], x7 = b[24];
  int x0 = b[0] * 256 + 8192;
  int t4 = (mul(W1, x4) + 4) >> 3;
  int t5 = (mul(W7, x4) + 4) >> 3;
  int t6 = (mul(W3, x7) + 4) >> 3;
  int t7 = (4 - mul(W5, x7)) >> 3;
  int t2 = (mul(W6, x3) + 4) >> 3;
  int t3 = (mul(W2, x3) + 4) >> 3;
  int s1 = t4 + t6;
  int s4 = t4 - t6;
  int s6 = t5 + t7;
  int s5 = t5 - t7;
  int s7 = x0 + t3;
  int s8 = x0 - t3;
  int s3 = x0 + t2;
  int s0 = x0 - t2;
  int s2 = (mul(181, s4 + s5) + 128) >> 8;
  s4 = (mul(181, s4 - s5) + 128) >> 8;
  int v[8] = {(s7 + s1) >> 14, (s3 + s2) >> 14, (s0 + s4) >> 14,
              (s8 + s6) >> 14, (s8 - s6) >> 14, (s0 - s4) >> 14,
              (s3 - s2) >> 14, (s7 - s1) >> 14};
  for (int i = 0; i < 8; i++) {
    uint8_t *p = d + i * stride;
    *p = clamp255(add ? *p + v[i] : v[i]);
  }
}

// As idct_col_put, adding the column to the prediction already in `d`.
static void HOT(idct_col_add)(const int16_t *b, uint8_t *d, int stride);

// The passes over a whole block: the rows set in `rows`, then the 8 columns.
static void mpeg1_idct_rows(int16_t *b, uint32_t rows) {
  for (int r = 0; r < 8; r++) {
    if (rows & (1u << r)) {
      idct_row(b + r * 8);
    }
  }
}

static void mpeg1_idct_cols_put(const int16_t *b, uint8_t *d, int stride) {
  for (int c = 0; c < 8; c++) {
    idct_col_put(b + c, d + c, stride);
  }
}

static void mpeg1_idct_cols_add(const int16_t *b, uint8_t *d, int stride) {
  for (int c = 0; c < 8; c++) {
    idct_col_add(b + c, d + c, stride);
  }
}

static void mpeg1_idct_half_put(const int16_t *b, uint8_t *d, int stride) {
  for (int c = 0; c < 8; c++) {
    idct_col_half(b + c, d + c, stride, false);
  }
}

static void mpeg1_idct_half_add(const int16_t *b, uint8_t *d, int stride) {
  for (int c = 0; c < 8; c++) {
    idct_col_half(b + c, d + c, stride, true);
  }
}
#endif

static void HOT(idct_put)(int16_t *b, uint8_t *d, int stride, uint32_t rows) {
  mpeg1_idct_rows(b, rows);
  if (rows == 1u) {
    uint8_t v[8];
    for (int c = 0; c < 8; c++) {
      v[c] = clamp255((b[c] + 32) >> 6);
    }
    for (int r = 0; r < 8; r++) {
      memcpy(d + r * stride, v, 8);
    }
  } else if ((rows & 0xF0u) == 0) {
    mpeg1_idct_half_put(b, d, stride);
  } else {
    mpeg1_idct_cols_put(b, d, stride);
  }
  for (int r = 0; r < 8; r++) {
    if (rows & (1u << r)) {
      memset(b + r * 8, 0, 8 * sizeof(int16_t));
    }
  }
}

#if !defined(__ARM_ARCH_6M__)
static void HOT(idct_col_add)(const int16_t *b, uint8_t *d, int stride) {
  int x1 = b[32] * 256, x2 = b[48], x3 = b[16], x4 = b[8], x5 = b[56],
      x6 = b[40], x7 = b[24];
  if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
    int v = (b[0] + 32) >> 6;
    for (int i = 0; i < 8; i++) {
      d[i * stride] = clamp255(d[i * stride] + v);
    }
    return;
  }
  int x0 = b[0] * 256 + 8192;
  int x8 = mul(W7, (x4 + x5)) + 4;
  x4 = (x8 + mul(W1 - W7, x4)) >> 3;
  x5 = (x8 - mul(W1 + W7, x5)) >> 3;
  x8 = mul(W3, (x6 + x7)) + 4;
  x6 = (x8 - mul(W3 - W5, x6)) >> 3;
  x7 = (x8 - mul(W3 + W5, x7)) >> 3;
  x8 = x0 + x1;
  x0 -= x1;
  x1 = mul(W6, (x3 + x2)) + 4;
  x2 = (x1 - mul(W2 + W6, x2)) >> 3;
  x3 = (x1 + mul(W2 - W6, x3)) >> 3;
  x1 = x4 + x6;
  x4 -= x6;
  x6 = x5 + x7;
  x5 -= x7;
  x7 = x8 + x3;
  x8 -= x3;
  x3 = x0 + x2;
  x0 -= x2;
  x2 = (mul(181, (x4 + x5)) + 128) >> 8;
  x4 = (mul(181, (x4 - x5)) + 128) >> 8;
  d[0 * stride] = clamp255(d[0 * stride] + ((x7 + x1) >> 14));
  d[1 * stride] = clamp255(d[1 * stride] + ((x3 + x2) >> 14));
  d[2 * stride] = clamp255(d[2 * stride] + ((x0 + x4) >> 14));
  d[3 * stride] = clamp255(d[3 * stride] + ((x8 + x6) >> 14));
  d[4 * stride] = clamp255(d[4 * stride] + ((x8 - x6) >> 14));
  d[5 * stride] = clamp255(d[5 * stride] + ((x0 - x4) >> 14));
  d[6 * stride] = clamp255(d[6 * stride] + ((x3 - x2) >> 14));
  d[7 * stride] = clamp255(d[7 * stride] + ((x7 - x1) >> 14));
}
#endif

// As idct_put, adding to the prediction in `d`.
static void HOT(idct_add)(int16_t *b, uint8_t *d, int stride, uint32_t rows) {
  mpeg1_idct_rows(b, rows);
  if (rows == 1u) {
    int v[8];
    for (int c = 0; c < 8; c++) {
      v[c] = (b[c] + 32) >> 6;
    }
    for (int r = 0; r < 8; r++) {
      uint8_t *line = d + r * stride;
      for (int c = 0; c < 8; c++) {
        line[c] = clamp255(line[c] + v[c]);
      }
    }
  } else if ((rows & 0xF0u) == 0) {
    mpeg1_idct_half_add(b, d, stride);
  } else {
    mpeg1_idct_cols_add(b, d, stride);
  }
  for (int r = 0; r < 8; r++) {
    if (rows & (1u << r)) {
      memset(b + r * 8, 0, 8 * sizeof(int16_t));
    }
  }
}

// --- Headers --------------------------------------------------------------------

static int parse_sequence_header(mpeg1_t *m) {
  int width = (int)br_get(m, 12);
  int height = (int)br_get(m, 12);
  br_get(m, 4);   // pel aspect ratio
  int picture_rate = (int)br_get(m, 4);
  br_get(m, 18);  // bit rate
  br_get(m, 1);   // marker
  br_get(m, 10);  // VBV buffer size
  br_get(m, 1);   // constrained parameters
  if (br_get(m, 1)) {
    for (int i = 0; i < 64; i++) {
      m->intra_q[zigzag[i]] = (uint8_t)br_get(m, 8);
    }
  } else {
    memcpy(m->intra_q, default_intra_q, 64);
  }
  if (br_get(m, 1)) {
    for (int i = 0; i < 64; i++) {
      m->non_intra_q[zigzag[i]] = (uint8_t)br_get(m, 8);
    }
  } else {
    memset(m->non_intra_q, 16, 64);
  }
  if (width <= 0 || height <= 0) {
    return MPEG1_ERR_STREAM;
  }
  if (width > (int)MPEG1_MAX_WIDTH || height > (int)MPEG1_MAX_HEIGHT) {
    return MPEG1_ERR_SIZE;
  }
  m->width = width;
  m->height = height;
  m->mb_cols = (width + 15) / 16;
  m->mb_rows = (height + 15) / 16;
  m->stride = m->mb_cols * 16;
  m->picture_rate = picture_rate;
  m->have_sequence = true;
  return 0;
}

static void parse_picture_header(mpeg1_t *m) {
  m->temporal_reference = (int)br_get(m, 10);
  m->picture_type = (int)br_get(m, 3);
  br_get(m, 16);  // VBV delay
  m->forward_f_code = 0;
  m->full_pel_forward = false;
  if (m->picture_type == MPEG1_PICTURE_P ||
      m->picture_type == MPEG1_PICTURE_B) {
    m->full_pel_forward = br_get(m, 1) != 0;
    m->forward_f_code = (int)br_get(m, 3);
  }
  if (m->picture_type == MPEG1_PICTURE_B) {
    br_get(m, 4);  // full_pel_backward_vector, backward_f_code
  }
  while (br_get(m, 1)) {  // extra_information_picture
    br_get(m, 8);
  }
}

// --- Frame store ----------------------------------------------------------------

// slot[MPEG1_MAX_SLOTS] is own_row: the only row of the intra-only mode.
#define OWN_SLOT ((int)MPEG1_MAX_SLOTS)

static bool reference_mode(const mpeg1_t *m) {
  return m->slot_count >= m->mb_rows + 2;
}

static int alloc_slot(mpeg1_t *m) {
  if (!reference_mode(m)) {
    return OWN_SLOT;
  }
  if (m->free_slots == 0) {
    return -1;
  }
  int s = __builtin_ctz(m->free_slots);
  m->free_slots &= ~(1u << s);
  return s;
}

static void release_slot(mpeg1_t *m, int s) {
  if (s >= 0 && s < OWN_SLOT) {
    m->free_slots |= 1u << s;
  }
}

static void emit_row(mpeg1_t *m, int row) {
  if (m->row_fn == NULL || m->new_row[row] < 0) {
    return;
  }
  uint8_t *base = m->slot[m->new_row[row]];
  uint8_t *cb = base + 16 * m->stride;
  m->row_fn(m->row_ctx, row, base, cb, cb + 8 * (m->stride / 2), m->stride);
}

// Moves the picture being written on to macroblock row `row`: hands out the
// rows before it, releases the reference rows that no later row can reach
// (a row reads the reference from the row above it on), and gives each new
// row a slot. False when no slot is free.
static bool advance_to_row(mpeg1_t *m, int row) {
  while (m->row < row) {
    if (m->row >= 0) {
      emit_row(m, m->row);
    }
    int next = m->row + 1;
    m->row = next;
    if (next >= m->mb_rows) {
      continue;
    }
    int old = next - 2;
    if (old >= 0 && m->ref_row[old] >= 0) {
      release_slot(m, m->ref_row[old]);
      m->ref_row[old] = -1;
    }
    int s = alloc_slot(m);
    if (s < 0) {
      return false;
    }
    m->new_row[next] = (int8_t)s;
  }
  return true;
}

// --- Motion compensation --------------------------------------------------------

// Line `line` of the reference's plane (0 Y, 1 Cb, 2 Cr), clamped to it.
static inline const uint8_t *ref_line(const mpeg1_t *m, int plane, int line) {
  int lines = plane ? m->mb_rows * 8 : m->mb_rows * 16;
  if (line < 0) {
    line = 0;
  } else if (line >= lines) {
    line = lines - 1;
  }
  int s = plane ? m->ref_row[line >> 3] : m->ref_row[line >> 4];
  const uint8_t *base = m->slot[s >= 0 ? s : OWN_SLOT];
  if (plane == 0) {
    return base + (line & 15) * m->stride;
  }
  int cs = m->stride / 2;
  return base + 16 * m->stride + (plane - 1) * 8 * cs + (line & 7) * cs;
}

// The prediction of a size x size block at (px, py) of `plane`, moved by
// (dx, dy) half pels, into `d`.
static void HOT(predict)(mpeg1_t *m, int plane, int px, int py, int size,
                         int dx, int dy, uint8_t *d, int dstride) {
  int width = plane ? m->stride / 2 : m->stride;
  int hx = dx & 1;
  int hy = dy & 1;
  int x0 = px + (dx >> 1);
  int y0 = py + (dy >> 1);
  if (x0 < 0) {
    x0 = 0;
    hx = 0;
    m->stats.clamped_vectors++;
  } else if (x0 + size + hx > width) {
    x0 = width - size;
    hx = 0;
    m->stats.clamped_vectors++;
  }
  for (int l = 0; l < size; l++, d += dstride) {
    const uint8_t *a = ref_line(m, plane, y0 + l) + x0;
    if (!hy) {
      if (!hx) {
        memcpy(d, a, (size_t)size);
      } else {
        for (int i = 0; i < size; i++) {
          d[i] = (uint8_t)((a[i] + a[i + 1] + 1) >> 1);
        }
      }
    } else {
      const uint8_t *b = ref_line(m, plane, y0 + l + 1) + x0;
      if (!hx) {
        for (int i = 0; i < size; i++) {
          d[i] = (uint8_t)((a[i] + b[i] + 1) >> 1);
        }
      } else {
        for (int i = 0; i < size; i++) {
          d[i] = (uint8_t)((a[i] + a[i + 1] + b[i] + b[i + 1] + 2) >> 2);
        }
      }
    }
  }
}

static void predict_macroblock(mpeg1_t *m, int row, int col, int dx, int dy,
                               uint8_t *y, uint8_t *cb, uint8_t *cr) {
  uint32_t t0 = (m->cycles != NULL) ? m->cycles() : 0;
  int cs = m->stride / 2;
  predict(m, 0, col * 16, row * 16, 16, dx, dy, y, m->stride);
  // Chroma: half the luma vector, towards zero.
  predict(m, 1, col * 8, row * 8, 8, dx / 2, dy / 2, cb, cs);
  predict(m, 2, col * 8, row * 8, 8, dx / 2, dy / 2, cr, cs);
  if (m->cycles != NULL) {
    m->stats.mc_cycles += (m->cycles() - t0) & 0xFFFFFFu;
  }
}

static int decode_motion(mpeg1_t *m, int r_size, int pred, bool *ok) {
  uint32_t e = mpeg1_motion_code[br_peek(m, 11)];
  if (e == 0) {
    *ok = false;
    return pred;
  }
  br_skip(m, (int)(e >> 8));
  int code = (int)(e & 0xFFu) - 16;
  int fscale = 1 << r_size;
  int d = code;
  if (code != 0 && fscale != 1) {
    int r = (int)br_get(m, r_size);
    d = (((code < 0 ? -code : code) - 1) << r_size) + r + 1;
    if (code < 0) {
      d = -d;
    }
  }
  int v = pred + d;
  if (v > fscale * 16 - 1) {
    v -= fscale * 32;
  } else if (v < -fscale * 16) {
    v += fscale * 32;
  }
  return v;
}

// --- Blocks ---------------------------------------------------------------------

// A coded block's coefficients, parsed into the next of the macroblock's
// buffers and queued for transform_block(): its pixels go to `d`, stored
// (intra) or added to the prediction there. False on a code that does not
// exist, the buffer left clear.
static bool HOT(parse_block)(mpeg1_t *m, int comp, bool intra, uint8_t *d,
                             int stride) {
  int16_t *blk = m->block[m->pending_count];
  int n = 0;
  const uint8_t *q = m->non_intra_q;
  if (intra) {
    uint32_t e = (comp == 0) ? mpeg1_dc_size_luminance[br_peek(m, 7)]
                             : mpeg1_dc_size_chrominance[br_peek(m, 8)];
    if (e == 0) {
      return false;
    }
    br_skip(m, (int)(e >> 8));
    int size = (int)(e & 0xFFu);
    if (size > 0) {
      int diff = (int)br_get(m, size);
      if ((diff >> (size - 1)) == 0) {
        diff += 1 - (1 << size);
      }
      m->dc_pred[comp] += diff;
    }
    blk[0] = (int16_t)(m->dc_pred[comp] * 8);
    q = m->intra_q;
    n = 1;
  }
  uint32_t rows = intra ? 1u : 0u;

  int quant = m->quant;
  bool first = !intra;
  for (;;) {
    uint32_t bits16 = br_peek(m, 16);
    int run;
    int level;
    if (bits16 & 0x8000u) {
      if (first) {
        br_skip(m, 1);  // dct_coeff_first "1s": run 0, level 1
      } else if ((bits16 & 0x4000u) == 0) {
        br_skip(m, 2);  // end of block
        break;
      } else {
        br_skip(m, 2);  // "11s": run 0, level 1
      }
      run = 0;
      level = br_get(m, 1) ? -1 : 1;
    } else {
      uint32_t c = (bits16 >= 0x0400u) ? mpeg1_dct_first8[bits16 >> 8]
                                       : mpeg1_dct_zeros6[bits16 & 0x3FFu];
      if (c == 0) {
        memset(blk, 0, sizeof(m->block[0]));
        return false;
      }
      br_skip(m, (int)(c >> 11));
      level = (int)(c & 0x3Fu);
      run = (int)((c >> 6) & 0x1Fu);
      if (level == 0) {  // escape: 6 bits of run, 8 or 16 of level
        run = (int)br_get(m, 6);
        level = (int)br_get(m, 8);
        if (level == 0) {
          level = (int)br_get(m, 8);
        } else if (level == 128) {
          level = (int)br_get(m, 8) - 256;
        } else if (level > 128) {
          level -= 256;
        }
      } else if (br_get(m, 1)) {
        level = -level;
      }
    }
    first = false;
    n += run;
    if (n > 63) {
      memset(blk, 0, sizeof(m->block[0]));
      return false;
    }
    int pos = zigzag[n++];
    rows |= 1u << (pos >> 3);
    int v = intra ? 2 * level : 2 * level + (level > 0 ? 1 : -1);
    v = (v * quant * q[pos]) / 16;
    if (v != 0 && (v & 1) == 0) {
      v += (v > 0) ? -1 : 1;
    }
    blk[pos] = (int16_t)(v > 2047 ? 2047 : (v < -2048 ? -2048 : v));
  }

  mpeg1_block_t *p = &m->pending[m->pending_count++];
  p->coef = blk;
  p->d = d;
  p->stride = stride;
  p->intra = intra;
  p->rows = rows;
  if (n == 1 && blk[1] == 0) {
    // Coefficient 0 alone: a flat block.
    p->rows = 0;
    p->flat = (blk[0] + 4) >> 3;
    blk[0] = 0;
    m->stats.dc_only++;
  }
  m->stats.blocks++;
  return true;
}

// A parsed block's inverse DCT into its pixels; its buffer is left clear.
static void HOT(transform_block)(const mpeg1_block_t *p) {
  uint8_t *d = p->d;
  int stride = p->stride;
  if (p->rows == 0) {
    if (p->intra) {
      uint8_t v = clamp255(p->flat);
      for (int r = 0; r < 8; r++) {
        memset(d + r * stride, v, 8);
      }
    } else {
      for (int r = 0; r < 8; r++) {
        uint8_t *line = d + r * stride;
        for (int i = 0; i < 8; i++) {
          line[i] = clamp255(line[i] + p->flat);
        }
      }
    }
  } else if (p->intra) {
    idct_put(p->coef, d, stride, p->rows);
  } else {
    idct_add(p->coef, d, stride, p->rows);
  }
}

// One core's share of a macroblock's blocks.
typedef struct {
  const mpeg1_block_t *block[6];
  int count;
} transform_job_t;

static void HOT(transform_job)(void *arg) {
  const transform_job_t *j = (const transform_job_t *)arg;
  for (int i = 0; i < j->count; i++) {
    transform_block(j->block[i]);
  }
}

// The macroblock's parsed blocks through the inverse DCT: on two cores,
// alternate blocks each, when there are two and at least two blocks need
// more than a flat fill (fewer are not worth the handover).
static void HOT(transform_blocks)(mpeg1_t *m) {
  int n = m->pending_count;
  if (n == 0) {
    return;
  }
  uint32_t t0 = (m->cycles != NULL) ? m->cycles() : 0;
  int full = 0;
  for (int i = 0; i < n; i++) {
    full += m->pending[i].rows != 0;
  }
  if (m->run2 != NULL && full >= 2) {
    transform_job_t a = {{NULL}, 0};
    transform_job_t b = {{NULL}, 0};
    for (int i = 0; i < n; i++) {
      transform_job_t *j = (i & 1) ? &b : &a;
      j->block[j->count++] = &m->pending[i];
    }
    m->run2(transform_job, &b, &a);
  } else {
    for (int i = 0; i < n; i++) {
      transform_block(&m->pending[i]);
    }
  }
  m->pending_count = 0;
  if (m->cycles != NULL) {
    m->stats.idct_cycles += (m->cycles() - t0) & 0xFFFFFFu;
  }
}

// --- Macroblocks ----------------------------------------------------------------

// The slot pointers of macroblock (row, col) of the picture being written.
static void macroblock_dest(mpeg1_t *m, int row, int col, uint8_t **y,
                            uint8_t **cb, uint8_t **cr) {
  uint8_t *base = m->slot[m->new_row[row]];
  int cs = m->stride / 2;
  *y = base + col * 16;
  *cb = base + 16 * m->stride + col * 8;
  *cr = *cb + 8 * cs;
}

// A P picture's skipped macroblock: the reference's, unmoved.
static bool skip_macroblock(mpeg1_t *m, int address) {
  int row = address / m->mb_cols;
  int col = address % m->mb_cols;
  if (row < m->row || !advance_to_row(m, row)) {
    return false;
  }
  uint8_t *y, *cb, *cr;
  macroblock_dest(m, row, col, &y, &cb, &cr);
  predict_macroblock(m, row, col, 0, 0, y, cb, cr);
  m->mv_x = m->mv_y = 0;
  m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = 128;
  m->stats.skipped_macroblocks++;
  return true;
}

static bool decode_macroblock(mpeg1_t *m, uint32_t type) {
  if (type & MB_QUANT) {
    m->quant = (int)br_get(m, 5);
  }
  int row = m->mb_address / m->mb_cols;
  int col = m->mb_address % m->mb_cols;
  if (row < m->row || !advance_to_row(m, row)) {
    return false;  // a slice that went back up the picture, no slot
  }
  uint8_t *y, *cb, *cr;
  macroblock_dest(m, row, col, &y, &cb, &cr);
  int stride = m->stride;
  int cs = stride / 2;

  if (type & MB_INTRA) {
    m->mv_x = m->mv_y = 0;
    // The blocks parsed before a bad code are kept.
    bool ok = parse_block(m, 0, true, y, stride) &&
              parse_block(m, 0, true, y + 8, stride) &&
              parse_block(m, 0, true, y + 8 * stride, stride) &&
              parse_block(m, 0, true, y + 8 * stride + 8, stride) &&
              parse_block(m, 1, true, cb, cs) &&
              parse_block(m, 2, true, cr, cs);
    transform_blocks(m);
    if (!ok) {
      return false;
    }
    m->stats.macroblocks++;
    return true;
  }

  m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = 128;
  int dx = 0;
  int dy = 0;
  if (type & MB_FORWARD) {
    bool ok = true;
    int r_size = m->forward_f_code - 1;
    m->mv_x = decode_motion(m, r_size, m->mv_x, &ok);
    m->mv_y = decode_motion(m, r_size, m->mv_y, &ok);
    if (!ok) {
      return false;
    }
    dx = m->full_pel_forward ? m->mv_x * 2 : m->mv_x;
    dy = m->full_pel_forward ? m->mv_y * 2 : m->mv_y;
  } else {
    m->mv_x = m->mv_y = 0;  // no motion: the vector is 0
  }
  predict_macroblock(m, row, col, dx, dy, y, cb, cr);

  int cbp = 0;
  if (type & MB_PATTERN) {
    uint32_t e = mpeg1_coded_block_pattern[br_peek(m, 9)];
    if (e == 0) {
      return false;
    }
    br_skip(m, (int)(e >> 8));
    cbp = (int)(e & 0xFFu);
  }
  uint8_t *dst[6] = {y, y + 8, y + 8 * stride, y + 8 * stride + 8, cb, cr};
  bool ok = true;
  for (int b = 0; b < 6 && ok; b++) {
    if (cbp & (32 >> b)) {
      ok = parse_block(m, b < 4 ? 0 : b - 3, false, dst[b],
                       b < 4 ? stride : cs);
    }
  }
  transform_blocks(m);
  if (!ok) {
    return false;
  }
  m->stats.macroblocks++;
  return true;
}

// After a bad code the slice is lost: in a P picture the rest of its
// macroblock row becomes the reference's, unmoved (the next slice starts a
// row at the earliest). An I picture keeps what its row had.
static void conceal_rest_of_row(mpeg1_t *m) {
  m->stats.errors++;
  if (m->picture_type != MPEG1_PICTURE_P || m->mb_address < 0) {
    return;
  }
  int end = (m->mb_address / m->mb_cols + 1) * m->mb_cols;
  for (int a = m->mb_address + 1; a < end; a++) {
    if (!skip_macroblock(m, a)) {
      return;
    }
  }
}

static void decode_slice(mpeg1_t *m, int slice_code) {
  m->quant = (int)br_get(m, 5);
  while (br_get(m, 1)) {  // extra_information_slice
    br_get(m, 8);
  }
  m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = 128;
  m->mv_x = m->mv_y = 0;
  m->mb_address = (slice_code - 1) * m->mb_cols - 1;
  int total = m->mb_cols * m->mb_rows;
  bool slice_start = true;
  do {
    int increment = 0;
    for (;;) {
      uint32_t e = mpeg1_mb_address_increment[br_peek(m, 11)];
      if (e == 0) {
        conceal_rest_of_row(m);
        return;
      }
      br_skip(m, (int)(e >> 8));
      int v = (int)(e & 0xFFu);
      if (v == MBA_STUFFING) {
        continue;
      }
      if (v == MBA_ESCAPE) {
        increment += 33;
        continue;
      }
      increment += v;
      break;
    }
    if (m->mb_address + increment >= total) {
      m->stats.errors++;
      return;
    }
    if (!slice_start && m->picture_type == MPEG1_PICTURE_P) {
      for (int a = m->mb_address + 1; a < m->mb_address + increment; a++) {
        if (!skip_macroblock(m, a)) {
          m->stats.errors++;
          return;
        }
      }
    }
    slice_start = false;
    m->mb_address += increment;
    uint32_t t = (m->picture_type == MPEG1_PICTURE_I)
                     ? mpeg1_mb_type_i[br_peek(m, 2)]
                     : mpeg1_mb_type_p[br_peek(m, 6)];
    if (t == 0) {
      m->mb_address--;  // this macroblock was not decoded either
      conceal_rest_of_row(m);
      return;
    }
    br_skip(m, (int)(t >> 8));
    if (!decode_macroblock(m, t & 0xFFu)) {
      m->mb_address--;
      conceal_rest_of_row(m);
      return;
    }
  } while (br_peek(m, 23) != 0);
}

// --- Public ---------------------------------------------------------------------

void mpeg1_init(mpeg1_t *m, mpeg_ps_t *ps) {
  uint32_t (*cycles)(void) = m->cycles;
  mpeg1_run2_fn run2 = m->run2;
  memset(m, 0, sizeof(*m));
  m->cycles = cycles;
  m->run2 = run2;
  m->ps = ps;
  m->p = m->end = m->es + 4;
  m->pending_code = -1;
  memcpy(m->intra_q, default_intra_q, 64);
  memset(m->non_intra_q, 16, 64);
  m->slot[OWN_SLOT] = m->own_row;
  memset(m->ref_row, -1, sizeof(m->ref_row));
  memset(m->new_row, -1, sizeof(m->new_row));
}

void mpeg1_set_slots(mpeg1_t *m, uint8_t *const *slots, int count) {
  if (count > (int)MPEG1_MAX_SLOTS) {
    count = (int)MPEG1_MAX_SLOTS;
  }
  for (int i = 0; i < count; i++) {
    m->slot[i] = slots[i];
  }
  m->slot_count = count;
  m->free_slots = (count == 32) ? 0xFFFFFFFFu : ((1u << count) - 1u);
  memset(m->ref_row, -1, sizeof(m->ref_row));
  m->have_reference = false;
}

int mpeg1_next_picture(mpeg1_t *m) {
  for (;;) {
    int code = next_start_code(m);
    if (code < 0 || code == SC_SEQUENCE_END) {
      return MPEG1_END;
    }
    if (code == SC_SEQUENCE) {
      int result = parse_sequence_header(m);
      if (result < 0) {
        return result;
      }
    } else if (code == SC_GROUP) {
      // A group's temporal references count from its first picture shown.
      m->group_start += m->group_pictures;
      m->group_pictures = 0;
    } else if (code == SC_PICTURE) {
      if (!m->have_sequence) {
        continue;  // a picture before any sequence header: unusable
      }
      parse_picture_header(m);
      m->display_index = m->group_start + (uint32_t)m->temporal_reference;
      m->group_pictures++;
      m->stats.pictures++;
      return m->picture_type;
    }
    // User data, extensions, slices of a skipped picture: nothing to do.
  }
}

void mpeg1_skip_picture(mpeg1_t *m) {
  for (;;) {
    int code = next_start_code(m);
    if (code < 0) {
      break;
    }
    if (code > SC_SLICE_LAST || code == SC_PICTURE) {
      m->pending_code = code;
      break;
    }
  }
  m->stats.skipped++;
}

int mpeg1_decode_picture(mpeg1_t *m, mpeg1_row_fn row, void *ctx) {
  if (!m->have_sequence) {
    return MPEG1_ERR_NO_SEQUENCE;
  }
  int type = m->picture_type;
  bool reference = reference_mode(m);
  if (type == MPEG1_PICTURE_P && reference && m->forward_f_code > 2) {
    // Vectors could reach two rows away: the store cannot hold that.
    mpeg1_skip_picture(m);
    m->have_reference = false;
    return MPEG1_ERR_UNSUPPORTED;
  }
  bool decode = (type == MPEG1_PICTURE_I) ||
                (type == MPEG1_PICTURE_P && reference && m->have_reference &&
                 m->forward_f_code >= 1);
  if (!decode) {
    mpeg1_skip_picture(m);
    if (type == MPEG1_PICTURE_P) {
      m->have_reference = false;  // later P pictures would build on it
    }
    return 0;
  }

  m->row_fn = row;
  m->row_ctx = ctx;
  m->row = -1;
  memset(m->new_row, -1, sizeof(m->new_row));
  for (;;) {
    int code = next_start_code(m);
    if (code < 0) {
      break;
    }
    if (code >= SC_SLICE_FIRST && code <= SC_SLICE_LAST) {
      decode_slice(m, code);
      continue;
    }
    m->pending_code = code;
    break;
  }
  advance_to_row(m, m->mb_rows);  // hands out the last rows
  if (reference) {
    for (int r = 0; r < m->mb_rows; r++) {
      if (m->ref_row[r] >= 0) {
        release_slot(m, m->ref_row[r]);
      }
      m->ref_row[r] = m->new_row[r];
    }
    m->have_reference = true;
  }
  m->row_fn = NULL;
  m->stats.decoded++;
  return type;
}
