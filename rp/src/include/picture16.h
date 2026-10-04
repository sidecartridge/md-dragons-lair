/**
 * File: picture16.h
 * Description: A picture to 16 colours of its own: a palette chosen for it
 *              (median cut over a 4,096-colour histogram) and an ordered 8x8
 *              Bayer dither, the look of ffmpeg's
 *              `palettegen=stats_mode=single:max_colors=16` +
 *              `paletteuse=dither=bayer:bayer_scale=1`.
 *
 * The input is the picture at its output size: luma one byte a pixel, and
 * the two chroma planes at half the width and half the height (YCbCr with
 * MPEG-1's ranges). The palette entries are 4 bits a gun, the STE's colour
 * depth (an ST shows the 3 high bits). The colour indices are written over
 * the luma, so the conversion needs no picture buffer of its own: only
 * PICTURE16_WORK_BYTES of work memory, which the caller lends.
 *
 * Entry 0 of every palette is pure black: on an ST it is also the border's
 * colour, which stays black whatever the pictures. The picture's darkest
 * colour becomes that black when it is near-black (as in most pictures),
 * still chosen for the pixels of that colour (picture16_palette_t.dark): its
 * areas turn flat black, a little deeper. Otherwise the picture gets 15
 * colours and black.
 *
 * Plain C, integers only, no allocation: the same bytes on the RP and on a
 * PC.
 */

#ifndef PICTURE16_H
#define PICTURE16_H

#include <stdbool.h>
#include <stdint.h>

// The histogram (4,096 bins of 16 bits); later the colour-to-index table.
#define PICTURE16_WORK_BYTES (4096u * 2u)

typedef struct {
  // 0x0RGB, a gun 0..15. For an ST (3 bits a gun) the guns are even: the
  // 3-bit level shifted up, so that picture16_ste_word() gives the ST its
  // level (an STE showing that palette shows level x 2).
  uint16_t rgb444[16];
  int colours;  // entries in use (the rest black)
  // The colour a black entry 0 stands for when pixels are chosen: the
  // near-black it replaced (0: black itself). Shown black, it takes the
  // pixels of that colour, flat, not a pattern of black and the next one.
  uint16_t dark;
} picture16_palette_t;

// How the histogram's colours count when the palette is chosen.
enum {
  PICTURE16_WEIGHT_PIXELS = 0,  // by their pixels: large areas rule
  PICTURE16_WEIGHT_SQRT = 1,    // by the square root of their pixels: small
                                // distinct areas (a moon, fire) keep colours
};

// How a pixel's colour becomes an entry. All are ordered (the pattern stays
// in place from one frame to the next).
enum {
  // ffmpeg's paletteuse bayer, bayer_scale 1: the 8x8 Bayer matrix as an
  // offset of -16..+15 on R, G and B, then the nearest entry.
  PICTURE16_DITHER_BAYER = 0,
  // The same at bayer_scale 2: -8..+7.
  PICTURE16_DITHER_BAYER_SOFT = 1,
  // Mixing (after Joel Yliluoma's ordered dithering for arbitrary palettes):
  // each colour as the mix of two entries that best approaches it, the
  // Bayer matrix as the threshold that picks one of the two at each pixel.
  PICTURE16_DITHER_MIX = 2,
  // None: the nearest entry.
  PICTURE16_DITHER_NONE = 3,
};
#define PICTURE16_DITHERS 4

// Runs job(a) and job(b) at once, on two cores, returning when both are
// done. NULL: one after the other. The results are the same either way.
typedef void (*picture16_job_fn)(void *arg);
typedef void (*picture16_run2_fn)(picture16_job_fn job, void *a, void *b);

typedef struct {
  int gun_bits;   // 4: STE, TT, Falcon (4,096 colours); 3: ST (512)
  int weighting;  // PICTURE16_WEIGHT_*
  int dither;     // PICTURE16_DITHER_*
  picture16_run2_fn run2;  // stages on two cores (NULL: one)
  // PICTURE16_WORK_BYTES more, 2-byte aligned, for the second core's half
  // of the histogram (NULL: the histogram on one core).
  void *work2;
} picture16_options_t;

// Optional profiling: a free-running cycle counter (differences modulo
// 2^24); NULL for none.
typedef struct {
  uint32_t (*cycles)(void);
  uint32_t histogram;
  uint32_t palette;  // the median cut and the refinement
  uint32_t refine;   // of which the refinement
  uint32_t table;
  uint32_t dither;
} picture16_profile_t;

// Converts the picture in place: `y` (width x height bytes, `width` apart)
// becomes colour indices 0..15. `cb` and `cr` are (width / 2) x (height / 2).
// `work` is PICTURE16_WORK_BYTES, 2-byte aligned. The palette is chosen
// among the colours the target shows (options->gun_bits) and the dither
// aims at them.
void picture16_convert(uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                       int width, int height, void *work,
                       const picture16_options_t *options,
                       picture16_palette_t *palette,
                       picture16_profile_t *profile);

// The STE palette word of an entry (the low bit of each gun in bit 3).
uint16_t picture16_ste_word(uint16_t rgb444);

// Scales one decoded macroblock row of a src_w x src_h picture (16 luma
// lines, `stride` apart; 8 lines of each chroma plane, `stride / 2` apart)
// into the output planes: luma out_w x out_h, chroma half of each, taking
// the nearest source pixel. Rows come in any order.
void picture16_scale_mb_row(int mb_row, const uint8_t *y, const uint8_t *cb,
                            const uint8_t *cr, int stride, int src_w,
                            int src_h, uint8_t *out_y, uint8_t *out_cb,
                            uint8_t *out_cr, int out_w, int out_h);

// The Lanczos-3 downscale of the game's clips, 352x240 to 320x200 (chroma
// 176x120 to 160x100), as ffmpeg's `scale=320:200:flags=lanczos`, from rows
// that arrive in order. Each source line is scaled across into a ring of
// lines, and each output line down from the ring once its source lines are
// in. Other sizes fall back to picture16_scale_mb_row().
#define PICTURE16_RING_LINES 16
#define PICTURE16_SCALER_BYTES (PICTURE16_RING_LINES * 320u * 2u)

typedef struct {
  uint8_t *ring_y;   // out_w columns of PICTURE16_RING_LINES lines
  uint8_t *ring_cb;  // and out_w / 2
  uint8_t *ring_cr;
  uint8_t *out_y;
  uint8_t *out_cb;
  uint8_t *out_cr;
  int src_w;
  int src_h;
  int out_w;
  int out_h;
  int next_y;  // the next output line to produce
  int next_c;
  int lanczos;  // 0: nearest pixel
  picture16_run2_fn run2;  // two cores (NULL: one); set after init
  // Set after init, Lanczos only. Output lines into rings instead of whole
  // planes (0: whole planes): luma line o at out_y + (o % out_lines) *
  // out_w, chroma line c at out_cb / out_cr + (c % (out_lines / 2)) *
  // (out_w / 2). `rows`, when set, gets the chroma rows [c0, c1) completed
  // by each batch (their chroma lines and both their luma lines).
  int out_lines;
  void (*rows)(void *ctx, int c0, int c1);
  void *rows_ctx;
  int done_c;  // chroma rows handed to `rows` so far
} picture16_scaler_t;

// `ring` is PICTURE16_SCALER_BYTES, 4-byte aligned; out_cb and out_cr are
// (out_w / 2) x (out_h / 2).
void picture16_scaler_init(picture16_scaler_t *s, int src_w, int src_h,
                           uint8_t *ring, uint8_t *out_y, uint8_t *out_cb,
                           uint8_t *out_cr, int out_w, int out_h);
void picture16_scaler_mb_row(picture16_scaler_t *s, int mb_row,
                             const uint8_t *y, const uint8_t *cb,
                             const uint8_t *cr, int stride);

// --- Two passes ---------------------------------------------------------------
//
// A picture of the game's size (352x240 to 320x200) converted without
// keeping it scaled. Its source rows are fed twice: as the decoder hands them
// out, then again from the decoder's frame store, which still holds the
// picture. The first pass scales them into the histogram; the palette is
// chosen; the second scales them again and dithers, handing out the index
// lines as they are done. The result is picture16_convert()'s on the whole
// scaled picture, byte for byte.

// The scaled lines kept between the scaler and the passes: luma, and half
// as many of each chroma plane.
#define PICTURE16_LINES 16
#define PICTURE16_LINES_BYTES (PICTURE16_LINES * 320u + PICTURE16_LINES * 160u)

// The two passes' memory, in pieces wherever the caller has room for them,
// each 4-byte aligned: the scaler's ring (luma, then both chroma planes),
// the scaled lines (luma, then both chroma planes), the histogram's.
#define PICTURE16_RING_Y_BYTES (PICTURE16_RING_LINES * 320u)       // 5,120
#define PICTURE16_RING_C_BYTES (PICTURE16_RING_LINES * 160u * 2u)  // 5,120
#define PICTURE16_LINES_Y_BYTES (PICTURE16_LINES * 320u)           // 5,120
#define PICTURE16_LINES_C_BYTES (PICTURE16_LINES * 160u)           // 2,560
typedef struct {
  uint8_t *ring_y;   // PICTURE16_RING_Y_BYTES
  uint8_t *ring_c;   // PICTURE16_RING_C_BYTES
  uint8_t *lines_y;  // PICTURE16_LINES_Y_BYTES
  uint8_t *lines_c;  // PICTURE16_LINES_C_BYTES
  void *work;        // PICTURE16_WORK_BYTES (and options->work2)
} picture16_memory_t;

// The pieces carved from one buffer of PICTURE16_SCALER_BYTES +
// PICTURE16_LINES_BYTES and one of PICTURE16_WORK_BYTES.
picture16_memory_t picture16_memory(uint8_t *buffer, void *work);

// Gets index lines `line` and `line + 1`, `width` bytes each and `width`
// apart, colour indices 0..15.
typedef void (*picture16_lines_fn)(void *ctx, int line,
                                   const uint8_t *indices, int width);

typedef struct {
  picture16_scaler_t scaler;
  picture16_options_t options;
  picture16_memory_t memory;
  int pass;  // 1: the histogram, 2: the dither
  picture16_lines_fn lines_fn;
  void *lines_ctx;
  picture16_profile_t *profile;
} picture16_passes_t;

// Starts the first pass in `memory`. `profile` (or NULL) gets each stage's
// time, the histogram's and the dither's added up over their rows. False
// for another size than 352x240.
bool picture16_passes_init(picture16_passes_t *p, int src_w, int src_h,
                           const picture16_memory_t *memory,
                           const picture16_options_t *options,
                           picture16_profile_t *profile);

// One source macroblock row to the current pass, the rows in order (16 luma
// lines `stride` apart, 8 of each chroma plane `stride / 2` apart).
void picture16_passes_mb_row(picture16_passes_t *p, int mb_row,
                             const uint8_t *y, const uint8_t *cb,
                             const uint8_t *cr, int stride);

// Ends the first pass: the picture's own palette, chosen from its histogram.
void picture16_passes_choose(picture16_passes_t *p,
                             picture16_palette_t *palette);

// After picture16_passes_choose(), before picture16_passes_dither():
// `palette` refined on the picture's colours from where it is (the palette
// choice's k-means rounds), its entries in their order.
void picture16_passes_refine(picture16_passes_t *p,
                             picture16_palette_t *palette);

// After picture16_passes_choose(), before picture16_passes_dither(): how far
// `palette` is from the picture's colours. Each colour of the histogram, as
// the palette choice counts it, to its nearest entry: the squared 8-bit
// distances summed. To compare two palettes on one picture.
uint64_t picture16_passes_error(const picture16_passes_t *p,
                                const picture16_palette_t *palette);

// Starts the second pass, dithering to `palette` (the picture's own, or
// another among the target's colours); its index lines go to `lines_fn`.
void picture16_passes_dither(picture16_passes_t *p,
                             const picture16_palette_t *palette,
                             picture16_lines_fn lines_fn, void *lines_ctx);

#endif  // PICTURE16_H
