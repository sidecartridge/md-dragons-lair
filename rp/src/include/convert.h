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
 * picture's decode starts. No scaled picture is kept. Between the two, the
 * next I or P picture's header is read (the B pictures before it skipped,
 * which leaves the store alone), so a picture's frames are known before its
 * second pass; a picture shown for no frame is decoded (the next one needs
 * it) but not converted.
 *
 * Palette stability (keep_percent >= 0), so that what did not move costs
 * nothing from one frame to the next: a picture is dithered with the
 * palette in use before it when that palette's error on the picture is
 * within keep_percent of the picture's own palette's (a still or slowly
 * changing scene keeps its palette, and an ordered dither then gives a
 * still area the same indices); otherwise with its own palette, whose
 * entries take the slots of the nearest colours shown before. A cut gets
 * its own palette. The palette in use keeps the entry order of the picture
 * that chose it (the dither then repeats itself on what did not change),
 * each entry mapped to its slot; the slots it does not use keep their
 * colours, unused.
 *
 * Plain C, integers only, no allocation: the cartridge and the PC tool
 * convert a clip to the same bytes.
 */

#ifndef CONVERT_H
#define CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#include "mpeg1_video.h"
#include "picture16.h"

// The game's palette stability (keep_percent): measured on the whole game
// against ffmpeg's recipe held to the machine's colours, and chosen by eye.
#define CONVERT_KEEP_PERCENT 10

enum {
  CONVERT_ERR_RATE = -20,  // a picture rate MPEG-1 does not define
  CONVERT_ERR_SIZE = -21,  // not 352x240
};

// A picture handed out.
typedef struct {
  int type;                // MPEG1_PICTURE_I or MPEG1_PICTURE_P
  uint32_t display_index;  // its place in the clip's display order
  uint32_t first_frame;    // shown from this output frame
  uint32_t frames;         // for this many, one at least
  // Its palette: with palette stability, all 16 slots as shown.
  const picture16_palette_t *palette;
} convert_picture_t;

typedef struct {
  // Before the picture's lines.
  void (*begin)(void *ctx, const convert_picture_t *picture);
  // Its index lines, two at a time (as picture16_lines_fn).
  picture16_lines_fn lines;
  // After them.
  void (*end)(void *ctx, const convert_picture_t *picture);
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
  uint32_t hidden;        // decoded, shown for no frame
  bool started;
  int pending;            // the next I or P picture's type, 0: the end
  uint32_t pending_display;
  picture16_palette_t palette;  // the picture's own
  // Palette stability: -1 (convert_init's) off, each picture with its own
  // palette in its own order, as picture16_convert() gives it.
  int keep_percent;
  picture16_palette_t shown;   // the 16 slots, as shown
  picture16_palette_t in_use;  // the palette dithered to, in its order
  uint8_t map[16];             // in_use's entry -> its slot
  bool remap;                 // map is not the identity
  uint32_t kept;              // pictures dithered with the palette in use
  uint32_t evolved;           // with it refined on them, entries in place
  uint8_t line_pair[2 * 320];
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
