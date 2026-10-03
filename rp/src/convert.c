/**
 * File: convert.c
 * Description: A clip converted for the app. See convert.h.
 */

#include "convert.h"

#include <string.h>

#include "cadence.h"

// The store's spare rows hold the rest of the passes' memory.
_Static_assert(PICTURE16_RING_Y_BYTES + PICTURE16_LINES_C_BYTES <=
                   MPEG1_SLOT_BYTES,
               "the luma ring and the scaled chroma lines fit a row");
_Static_assert(PICTURE16_WORK_BYTES <= MPEG1_SLOT_BYTES,
               "the histogram fits a row");

void convert_init(convert_t *c, mpeg1_t *dec, uint8_t *ring_c,
                  uint8_t *lines_y, const picture16_options_t *options,
                  const convert_out_t *out) {
  memset(c, 0, sizeof(*c));
  c->dec = dec;
  c->options = *options;
  c->ring_c = ring_c;
  c->lines_y = lines_y;
  c->out = *out;
  c->keep_percent = -1;
}

// The index lines, relabelled to the slots when the dithered palette's
// entries are not in them.
static void out_lines(void *ctx, int line, const uint8_t *indices,
                      int width) {
  convert_t *c = (convert_t *)ctx;
  if (!c->remap) {
    c->out.lines(c->out.ctx, line, indices, width);
    return;
  }
  for (int i = 0; i < 2 * width; i++) {
    c->line_pair[i] = c->map[indices[i]];
  }
  c->out.lines(c->out.ctx, line, c->line_pair, width);
}

static int distance(uint16_t a, uint16_t b) {
  int d = 0;
  for (int shift = 0; shift <= 8; shift += 4) {
    int e = (int)((a >> shift) & 15u) - (int)((b >> shift) & 15u);
    d += e * e;
  }
  return d;
}

// The picture's own palette into use: each entry, nearest pairs first, to
// the slot of the colour shown nearest to it.
static void take_slots(convert_t *c) {
  const picture16_palette_t *own = &c->palette;
  bool entry_done[16] = {false};
  bool slot_done[16] = {false};
  for (int k = 0; k < own->colours; k++) {
    int best_e = 0;
    int best_s = 0;
    int best_d = 1 << 30;
    for (int e = 0; e < own->colours; e++) {
      for (int s = 0; s < 16 && !entry_done[e]; s++) {
        int d = slot_done[s] ? best_d
                             : distance(own->rgb444[e], c->shown.rgb444[s]);
        if (d < best_d) {
          best_d = d;
          best_e = e;
          best_s = s;
        }
      }
    }
    entry_done[best_e] = true;
    slot_done[best_s] = true;
    c->map[best_e] = (uint8_t)best_s;
    c->shown.rgb444[best_s] = own->rgb444[best_e];
  }
  c->in_use = *own;
  c->remap = false;
  for (int e = 0; e < own->colours; e++) {
    c->remap |= c->map[e] != e;
  }
}

// The palette to dither with: the one shown, or the picture's own.
static const picture16_palette_t *stable_palette(convert_t *c) {
  if (c->keep_percent < 0) {
    c->remap = false;
    return &c->palette;
  }
  if (c->pictures == 0) {
    memset(&c->shown, 0, sizeof(c->shown));
    c->shown.colours = 16;
    for (int e = 0; e < c->palette.colours; e++) {
      c->shown.rgb444[e] = c->palette.rgb444[e];
      c->map[e] = (uint8_t)e;
    }
    c->in_use = c->palette;
    c->remap = false;
    return &c->in_use;
  }
  uint64_t own = picture16_passes_error(&c->passes, &c->palette);
  uint64_t in_use = picture16_passes_error(&c->passes, &c->in_use);
  if (in_use * 100u <= own * (uint64_t)(100 + c->keep_percent)) {
    c->kept++;
    return &c->in_use;
  }
  // The palette in use refined on the picture: its colours move, each in
  // its slot, instead of the picture's own palette taking new slots.
  picture16_palette_t evolved = c->in_use;
  picture16_passes_refine(&c->passes, &evolved);
  if (picture16_passes_error(&c->passes, &evolved) * 100u <=
      own * (uint64_t)(100 + c->keep_percent)) {
    c->evolved++;
    c->in_use = evolved;
    for (int e = 0; e < evolved.colours; e++) {
      c->shown.rgb444[c->map[e]] = evolved.rgb444[e];
    }
    return &c->in_use;
  }
  take_slots(c);
  return &c->in_use;
}

// One pass over the decoded picture, from the frame store.
static void pass_over_store(convert_t *c) {
  mpeg1_t *dec = c->dec;
  for (int row = 0; row < dec->mb_rows; row++) {
    const uint8_t *y;
    const uint8_t *cb;
    const uint8_t *cr;
    if (mpeg1_reference_row(dec, row, &y, &cb, &cr)) {
      picture16_passes_mb_row(&c->passes, row, y, cb, cr, dec->stride);
    }
  }
}

// Reads up to the next I or P picture's header, the B pictures before it
// skipped: its type into c->pending (0 at the clip's end). Returns a
// negative error, or 0.
static int read_ahead(convert_t *c) {
  mpeg1_t *dec = c->dec;
  for (;;) {
    int type = mpeg1_next_picture(dec);
    if (type == MPEG1_END) {
      c->pending = 0;
      return 0;
    }
    if (type < 0) {
      return type;
    }
    if (dec->display_index > c->last_display) {
      c->last_display = dec->display_index;
    }
    if (type == MPEG1_PICTURE_I || type == MPEG1_PICTURE_P) {
      c->pending = type;
      c->pending_display = dec->display_index;
      return 0;
    }
    mpeg1_skip_picture(dec);
  }
}

int convert_next(convert_t *c) {
  mpeg1_t *dec = c->dec;
  if (!c->started) {
    c->started = true;
    int r = read_ahead(c);
    if (r < 0) {
      return r;
    }
  }
  while (c->pending != 0) {
    int type = c->pending;
    uint32_t display = c->pending_display;
    if (c->num == 0 &&
        !cadence_picture_rate(dec->picture_rate, &c->num, &c->den)) {
      return CONVERT_ERR_RATE;
    }
    int decoded = mpeg1_decode_picture(dec, NULL, NULL);
    if (decoded < 0) {
      return decoded;
    }
    // The next picture's header: how long this one shows.
    int r = read_ahead(c);
    if (r < 0) {
      return r;
    }
    if (decoded == 0) {
      continue;  // a P picture the frame store cannot follow: not shown
    }
    // Frames before the clip's first picture shown show it too.
    uint32_t first =
        (c->pictures == 0) ? 0 : cadence_first_frame(display, c->num, c->den);
    uint32_t end = cadence_first_frame(
        c->pending != 0 ? c->pending_display : c->last_display + 1u, c->num,
        c->den);
    if (end <= first) {
      c->hidden++;  // shown for no frame: only the next one's reference
      continue;
    }
    uint8_t *spare[2];
    if (mpeg1_spare_rows(dec, spare, 2) < 2) {
      return CONVERT_ERR_MEMORY;
    }
    picture16_memory_t memory = {spare[0], c->ring_c, c->lines_y,
                                 spare[0] + PICTURE16_RING_Y_BYTES, spare[1]};
    if (!picture16_passes_init(&c->passes, dec->width, dec->height, &memory,
                               &c->options, c->profile)) {
      return CONVERT_ERR_SIZE;
    }
    pass_over_store(c);
    picture16_passes_choose(&c->passes, &c->palette);
    const picture16_palette_t *dither = stable_palette(c);
    convert_picture_t info = {
        type, display, first, end - first,
        c->keep_percent < 0 ? &c->palette : &c->shown};
    c->out.begin(c->out.ctx, &info);
    picture16_passes_dither(&c->passes, dither, out_lines, c);
    pass_over_store(c);
    c->pictures++;
    c->out.end(c->out.ctx, &info);
    return decoded;
  }
  return 0;
}

uint32_t convert_length(const convert_t *c) {
  if (c->pictures == 0) {
    return 0;
  }
  return cadence_first_frame(c->last_display + 1u, c->num, c->den);
}
