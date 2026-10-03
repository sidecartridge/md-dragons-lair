/**
 * File: convert.c
 * Description: A clip converted for the app. See convert.h.
 */

#include "convert.h"

#include <string.h>

#include "cadence.h"

void convert_init(convert_t *c, mpeg1_t *dec, uint8_t *ring, uint8_t *lines,
                  void *work, const picture16_options_t *options,
                  const convert_out_t *out) {
  memset(c, 0, sizeof(*c));
  c->dec = dec;
  c->options = *options;
  c->ring = ring;
  c->lines = lines;
  c->work = work;
  c->out = *out;
}

// The decoder's rows, as they come out: the first pass.
static void first_pass_row(void *ctx, int mb_row, const uint8_t *y,
                           const uint8_t *cb, const uint8_t *cr, int stride) {
  convert_t *c = (convert_t *)ctx;
  picture16_passes_mb_row(&c->passes, mb_row, y, cb, cr, stride);
}

int convert_next(convert_t *c) {
  mpeg1_t *dec = c->dec;
  for (;;) {
    int type = mpeg1_next_picture(dec);
    if (type == MPEG1_END) {
      return 0;
    }
    if (type < 0) {
      return type;
    }
    if (dec->display_index > c->last_display) {
      c->last_display = dec->display_index;
    }
    if (type != MPEG1_PICTURE_I && type != MPEG1_PICTURE_P) {
      mpeg1_skip_picture(dec);
      continue;
    }
    if (c->num == 0 &&
        !cadence_picture_rate(dec->picture_rate, &c->num, &c->den)) {
      return CONVERT_ERR_RATE;
    }
    if (!picture16_passes_init(&c->passes, dec->width, dec->height, c->ring,
                               c->lines, c->work, &c->options, c->profile)) {
      return CONVERT_ERR_SIZE;
    }
    int decoded = mpeg1_decode_picture(dec, first_pass_row, c);
    if (decoded < 0) {
      return decoded;
    }
    if (decoded == 0) {
      continue;  // a P picture the frame store cannot follow: not shown
    }
    picture16_passes_palette(&c->passes, &c->palette, c->out.lines,
                             c->out.ctx);
    for (int row = 0; row < dec->mb_rows; row++) {
      const uint8_t *y;
      const uint8_t *cb;
      const uint8_t *cr;
      if (mpeg1_reference_row(dec, row, &y, &cb, &cr)) {
        picture16_passes_mb_row(&c->passes, row, y, cb, cr, dec->stride);
      }
    }
    // Frames before the clip's first picture shown show it too.
    uint32_t first =
        (c->pictures == 0)
            ? 0
            : cadence_first_frame(dec->display_index, c->num, c->den);
    c->pictures++;
    c->out.picture(c->out.ctx, &c->palette, first);
    return decoded;
  }
}

uint32_t convert_length(const convert_t *c) {
  if (c->pictures == 0) {
    return 0;
  }
  return cadence_first_frame(c->last_display + 1u, c->num, c->den);
}
