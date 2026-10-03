/**
 * File: mpeg1_video.h
 * Description: MPEG-1 video decoder (ISO/IEC 11172-2) for the RP2040: a
 *              stream from mpeg_ps.h in, pictures out one macroblock row at
 *              a time.
 *
 * Written for a machine with no FPU and little RAM: integer arithmetic
 * only, no allocation. A picture is handled as macroblock rows of 16 luma
 * lines and 8 lines of each chroma plane, MPEG1_SLOT_BYTES each, and leaves
 * the decoder one row at a time through a callback.
 *
 * Two modes:
 *   - Intra only (no frame store): I pictures are decoded through one
 *     internal row; P and B pictures are skipped.
 *   - With a frame store (mpeg1_set_slots(), at least mb_rows + 2 slots):
 *     I and P pictures are decoded, the reference held in the slots. A
 *     P picture is decoded in place: its rows go to free slots, and a
 *     reference row is released as soon as no later row can reach it.
 *     That works because a motion vector here moves a block by 16 lines at
 *     most (forward_f_code 1 or 2, which every clip of the game uses); a
 *     stream with larger vectors is refused (MPEG1_ERR_UNSUPPORTED). B
 *     pictures are skipped.
 *
 * Plain C, no SDK: it builds for the firmware and for host tests. An
 * optional cycle counter (mpeg1_t.cycles) splits the decoding time between
 * motion compensation, the inverse DCT and the rest (the bitstream).
 */

#ifndef MPEG1_VIDEO_H
#define MPEG1_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#include "mpeg_ps.h"

#define MPEG1_MAX_WIDTH 352u   // the game's clips; wider streams are refused
#define MPEG1_MAX_HEIGHT 288u  // SIF, both systems
#define MPEG1_MAX_MB_ROWS (MPEG1_MAX_HEIGHT / 16u)
#define MPEG1_ES_BUFFER 2048u

// One macroblock row: 16 lines of Y, then 8 of Cb, then 8 of Cr, each line
// `stride` (Y) or `stride / 2` bytes, stride = 16 * the picture's columns.
#define MPEG1_SLOT_BYTES (MPEG1_MAX_WIDTH * 16u + MPEG1_MAX_WIDTH * 8u)
#define MPEG1_MAX_SLOTS (MPEG1_MAX_MB_ROWS + 2u)

enum {
  MPEG1_PICTURE_I = 1,
  MPEG1_PICTURE_P = 2,
  MPEG1_PICTURE_B = 3,
  MPEG1_PICTURE_D = 4,
};

enum {
  MPEG1_END = 0,
  MPEG1_ERR_STREAM = -1,  // a header that cannot be right
  MPEG1_ERR_SIZE = -2,    // larger than MPEG1_MAX_WIDTH x MPEG1_MAX_HEIGHT
  MPEG1_ERR_NO_SEQUENCE = -3,
  MPEG1_ERR_UNSUPPORTED = -4,  // motion vectors too long for the store
};

// One decoded macroblock row, rows in order from 0: `y` holds 16 lines of
// the picture's width, `cb` and `cr` 8 lines of half of it; lines are
// `stride` (y) and `stride / 2` (cb, cr) bytes apart.
typedef void (*mpeg1_row_fn)(void *ctx, int mb_row, const uint8_t *y,
                             const uint8_t *cb, const uint8_t *cr,
                             int stride);

typedef struct {
  uint32_t pictures;    // picture headers seen
  uint32_t decoded;     // pictures decoded
  uint32_t skipped;     // pictures skipped
  uint32_t macroblocks;
  uint32_t skipped_macroblocks;  // P: copied from the reference
  uint32_t blocks;      // coded blocks through the IDCT
  uint32_t dc_only;     // of them, with only coefficient 0
  uint32_t errors;      // slices abandoned on a bad code
  uint32_t clamped_vectors;  // motion that reached outside the picture
  // With a cycle counter: cycles in motion compensation and in the IDCT
  // (the flat-block shortcut included; on two cores, the time they take).
  uint64_t mc_cycles;
  uint64_t idct_cycles;
} mpeg1_stats_t;

// Runs job(a) and job(b) at once, on two cores, returning when both are
// done (the same type as picture16's).
typedef void (*mpeg1_job_fn)(void *arg);
typedef void (*mpeg1_run2_fn)(mpeg1_job_fn job, void *a, void *b);

// A coded block of the macroblock being decoded, parsed and waiting for its
// inverse DCT.
typedef struct {
  int16_t *coef;    // its coefficients, natural order
  uint8_t *d;       // its pixels
  int stride;
  uint32_t rows;    // bit r: row r has a coefficient; 0: flat, at `flat`
  int flat;
  bool intra;       // stored, or added to the prediction in `d`
} mpeg1_block_t;

typedef struct {
  mpeg_ps_t *ps;
  // Optional profiling: a free-running cycle counter of at least 24 bits
  // (SysTick on the RP2040); only differences modulo 2^24 are used.
  uint32_t (*cycles)(void);
  // Optional: a macroblock's inverse DCTs in two halves on two cores (NULL:
  // on this one). The pictures are the same either way.
  mpeg1_run2_fn run2;

  // Bit reader over the elementary stream: 4 bytes kept before the buffer
  // so that the bytes held in `cache` can always be given back.
  uint8_t es[4 + MPEG1_ES_BUFFER];
  const uint8_t *p;
  const uint8_t *end;
  uint32_t cache;  // the next bits, left-aligned
  int bits;        // valid bits in cache
  bool eos;        // the stream ended; zeros are read from then on
  int fake;        // zeros read past the end still in the cache
  int pending_code;  // a start code read but not yet handled, or -1

  // Sequence.
  bool have_sequence;
  int width;
  int height;
  int mb_cols;
  int mb_rows;
  int stride;  // 16 * mb_cols
  uint8_t intra_q[64];      // natural order
  uint8_t non_intra_q[64];  // natural order

  // Picture.
  int picture_type;
  int temporal_reference;
  int forward_f_code;
  bool full_pel_forward;

  // Slice and macroblock.
  int quant;
  int dc_pred[3];
  int mv_x;  // forward motion vector predictor, half pels
  int mv_y;
  int mb_address;
  int row;  // the macroblock row being written, -1 before the first

  // Frame store. slot[0..slot_count) are the caller's rows,
  // slot[MPEG1_MAX_SLOTS] is `own_row`; ref_row and new_row give each
  // picture row's slot, -1 for none.
  uint8_t *slot[MPEG1_MAX_SLOTS + 1];
  int slot_count;
  uint32_t free_slots;  // bit i: slot i holds nothing in use
  int8_t ref_row[MPEG1_MAX_MB_ROWS];
  int8_t new_row[MPEG1_MAX_MB_ROWS];
  bool have_reference;
  uint8_t own_row[MPEG1_SLOT_BYTES];
  // A macroblock's coefficients, parsed first and transformed together.
  // Word-aligned: the RP's inverse DCT reads two coefficients a word.
  _Alignas(4) int16_t block[6][64];
  mpeg1_block_t pending[6];
  int pending_count;

  mpeg1_row_fn row_fn;
  void *row_ctx;

  mpeg1_stats_t stats;
} mpeg1_t;

void mpeg1_init(mpeg1_t *m, mpeg_ps_t *ps);

// Hands the decoder `count` rows of MPEG1_SLOT_BYTES (count 0: intra only).
// Call after mpeg1_init(), before the first picture.
void mpeg1_set_slots(mpeg1_t *m, uint8_t *const *slots, int count);

// Reads up to the next picture header (sequence and group headers on the
// way). Returns its type (MPEG1_PICTURE_*), MPEG1_END at the end of the
// stream or a negative MPEG1_ERR_*.
int mpeg1_next_picture(mpeg1_t *m);

// Decodes the picture whose header mpeg1_next_picture() returned, handing
// its rows to `row` (may be NULL). Pictures the mode cannot decode are
// skipped. Returns the picture type, 0 when it was skipped, or a negative
// MPEG1_ERR_*.
int mpeg1_decode_picture(mpeg1_t *m, mpeg1_row_fn row, void *ctx);

// Skips the rest of the current picture without decoding it.
void mpeg1_skip_picture(mpeg1_t *m);

#endif  // MPEG1_VIDEO_H
