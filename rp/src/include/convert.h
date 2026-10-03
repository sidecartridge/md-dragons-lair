/**
 * File: convert.h
 * Description: A clip converted for the app. Its I and P pictures are
 *              decoded in place (the decoder's frame store), each scaled to
 *              320x200 and given 16 colours of its own in two passes over
 *              the decoded picture (picture16_passes_*), and placed on the
 *              app's 25 frames a second (cadence.h). B pictures are skipped.
 *
 * The first pass runs in the decoder's row callback, as the rows come out;
 * the second reads the picture back from the frame store before the next
 * picture's decode starts. No scaled picture is kept.
 *
 * Plain C, integers only, no allocation: the cartridge and the PC tool
 * convert a clip to the same bytes.
 */

#ifndef CONVERT_H
#define CONVERT_H

#include <stdint.h>

#include "mpeg1_video.h"
#include "picture16.h"

enum {
  CONVERT_ERR_RATE = -20,  // a picture rate MPEG-1 does not define
  CONVERT_ERR_SIZE = -21,  // not 352x240
};

typedef struct {
  // The picture being converted, two index lines at a time (as
  // picture16_lines_fn).
  picture16_lines_fn lines;
  // After its lines: the picture is shown from output frame `first_frame`
  // until the next picture's first frame (or the clip's length). Its type
  // and display position are in the decoder.
  void (*picture)(void *ctx, const picture16_palette_t *palette,
                  uint32_t first_frame);
  void *ctx;
} convert_out_t;

typedef struct {
  mpeg1_t *dec;
  picture16_passes_t passes;
  picture16_options_t options;
  picture16_profile_t *profile;  // NULL, or each picture's stage times
  uint8_t *ring;
  uint8_t *lines;
  void *work;
  convert_out_t out;
  uint32_t num;  // the clip's picture rate, num / den a second
  uint32_t den;
  uint32_t last_display;  // the last display position seen so far
  uint32_t pictures;      // pictures converted
  picture16_palette_t palette;
} convert_t;

// The decoder `dec` has its frame store (mpeg1_set_slots()). `ring`,
// `lines` and `work` (and options->work2) as picture16_passes_init()'s.
void convert_init(convert_t *c, mpeg1_t *dec, uint8_t *ring, uint8_t *lines,
                  void *work, const picture16_options_t *options,
                  const convert_out_t *out);

// Converts the clip's next I or P picture. Returns its type, 0 at the end
// of the clip, or a negative MPEG1_ERR_* or CONVERT_ERR_*.
int convert_next(convert_t *c);

// The clip's length in output frames, once convert_next() returned 0.
uint32_t convert_length(const convert_t *c);

#endif  // CONVERT_H
