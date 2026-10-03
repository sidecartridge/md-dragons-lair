/**
 * File: clip.c
 * Description: The app's converted clip file, version 1. See clip.h and
 *              docs/clip-format.md.
 */

#include "clip.h"

#include <string.h>

#include "crc32.h"

// Row opcodes: the two high bits, then a count less one (1..64).
#define OP_SKIP 0x00u     // pixels unchanged
#define OP_LITERAL 0x40u  // pixels follow, two a byte, the first high
#define OP_REPEAT 0x80u   // one colour, in the next byte's low nibble
#define OP_REST 0xC0u     // the rest of the row unchanged (no count)
#define OP_COUNT_MAX 64

// --- Header ----------------------------------------------------------------------

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
  return get16(p) | ((uint32_t)get16(p + 2) << 16);
}

void clip_header_write(const clip_header_t *h,
                       uint8_t out[CLIP_HEADER_BYTES]) {
  memset(out, 0, CLIP_HEADER_BYTES);
  memcpy(out, CLIP_MAGIC, 4);
  put16(out + 4, CLIP_VERSION);
  put16(out + 6, CLIP_HEADER_BYTES);
  put32(out + 8, h->frames);
  put16(out + 12, CLIP_FPS);
  put16(out + 14, CLIP_SAMPLE_RATE);
  put16(out + 16, CLIP_SAMPLES);
  out[18] = h->gun_bits;
  out[19] = h->flags;
  put16(out + 20, h->converter);
  put16(out + 22, h->keep_percent);
  put32(out + 24, h->source_bytes);
  put32(out + 28, h->source_crc);
  put32(out + 32, h->index_offset);
  put32(out + 36, h->index_count);
  put32(out + 40, h->largest_record);
  put32(out + 44, h->crc);
}

int clip_header_read(clip_header_t *h, const uint8_t in[CLIP_HEADER_BYTES]) {
  if (memcmp(in, CLIP_MAGIC, 4) != 0) {
    return -1;
  }
  if (get16(in + 4) != CLIP_VERSION || get16(in + 6) != CLIP_HEADER_BYTES ||
      get16(in + 12) != CLIP_FPS || get16(in + 14) != CLIP_SAMPLE_RATE ||
      get16(in + 16) != CLIP_SAMPLES) {
    return -2;
  }
  h->frames = get32(in + 8);
  h->gun_bits = in[18];
  h->flags = in[19];
  h->converter = get16(in + 20);
  h->keep_percent = get16(in + 22);
  h->source_bytes = get32(in + 24);
  h->source_crc = get32(in + 28);
  h->index_offset = get32(in + 32);
  h->index_count = get32(in + 36);
  h->largest_record = get32(in + 40);
  h->crc = get32(in + 44);
  return 0;
}

// --- Rows ----------------------------------------------------------------------

static size_t put_counts(uint8_t *out, size_t n, uint8_t op, int count) {
  while (count > 0) {
    int k = count > OP_COUNT_MAX ? OP_COUNT_MAX : count;
    out[n++] = (uint8_t)(op | (k - 1));
    count -= k;
  }
  return n;
}

static size_t put_literal(uint8_t *out, size_t n, const uint8_t *px,
                          int count) {
  out[n++] = (uint8_t)(OP_LITERAL | (count - 1));
  for (int i = 0; i < count; i += 2) {
    out[n++] = (uint8_t)((px[i] << 4) | (i + 1 < count ? px[i + 1] : 0));
  }
  return n;
}

size_t clip_encode_row(const uint8_t *row, const uint8_t *before,
                       uint8_t *out) {
  // Greedy: an unchanged run is skipped (the rest of the row in one byte);
  // a colour repeated 3 times or more is a repeat; anything else is
  // literal, until a skip of 2 or a repeat of 3 could start.
  uint8_t ops[2 * CLIP_WIDTH];
  size_t n = 0;
  int x = 0;
  while (x < CLIP_WIDTH) {
    if (before != NULL) {
      int s = 0;
      while (x + s < CLIP_WIDTH && row[x + s] == before[x + s]) {
        s++;
      }
      if (x + s == CLIP_WIDTH && s > 0) {
        ops[n++] = OP_REST;
        break;
      }
      if (s > 0) {
        n = put_counts(ops, n, OP_SKIP, s);
        x += s;
        continue;
      }
    }
    int r = 1;
    while (x + r < CLIP_WIDTH && row[x + r] == row[x]) {
      r++;
    }
    if (r >= 3) {
      for (int left = r; left > 0; left -= OP_COUNT_MAX) {
        int k = left > OP_COUNT_MAX ? OP_COUNT_MAX : left;
        ops[n++] = (uint8_t)(OP_REPEAT | (k - 1));
        ops[n++] = row[x];
      }
      x += r;
      continue;
    }
    int l = 1;
    while (x + l < CLIP_WIDTH && l < OP_COUNT_MAX) {
      int q = x + l;
      if (before != NULL && row[q] == before[q] &&
          (q + 1 == CLIP_WIDTH || row[q + 1] == before[q + 1])) {
        break;
      }
      if (q + 2 < CLIP_WIDTH && row[q] == row[q + 1] &&
          row[q] == row[q + 2]) {
        break;
      }
      l++;
    }
    n = put_literal(ops, n, row + x, l);
    x += l;
  }
  if (n > CLIP_ROW_MAX) {
    // Never longer than the row stored as literals.
    n = 0;
    for (x = 0; x < CLIP_WIDTH; x += OP_COUNT_MAX) {
      n = put_literal(out, n, row + x, OP_COUNT_MAX);
    }
    return n;
  }
  memcpy(out, ops, n);
  return n;
}

size_t clip_decode_row(const uint8_t *in, size_t available, uint8_t *row) {
  size_t i = 0;
  int x = 0;
  while (x < CLIP_WIDTH) {
    if (i >= available) {
      return 0;
    }
    uint8_t op = in[i++];
    int k = (op & (OP_COUNT_MAX - 1)) + 1;
    switch (op & 0xC0u) {
      case OP_SKIP:
        if (x + k > CLIP_WIDTH) {
          return 0;
        }
        break;
      case OP_LITERAL: {
        size_t bytes = (size_t)(k + 1) / 2;
        if (x + k > CLIP_WIDTH || bytes > available - i) {
          return 0;
        }
        for (int j = 0; j < k; j += 2) {
          uint8_t b = in[i + (size_t)j / 2];
          row[x + j] = b >> 4;
          if (j + 1 < k) {
            row[x + j + 1] = b & 15u;
          }
        }
        i += bytes;
        break;
      }
      case OP_REPEAT:
        if (x + k > CLIP_WIDTH || i >= available) {
          return 0;
        }
        memset(row + x, in[i++] & 15u, (size_t)k);
        break;
      default:
        if (op != OP_REST) {
          return 0;
        }
        k = CLIP_WIDTH - x;
        break;
    }
    x += k;
  }
  return i;
}

// --- Records --------------------------------------------------------------------

size_t clip_read_record(const uint8_t *in, size_t available,
                        clip_record_t *r, uint8_t *pixels) {
  if (available < 1) {
    return 0;
  }
  uint8_t kind = in[0];
  size_t n = 1;
  r->kind = kind & CLIP_KIND_MASK;
  if ((kind & ~(CLIP_KIND_MASK | CLIP_KIND_PALETTE)) != 0 || r->kind > 2) {
    return 0;
  }
  r->palette = NULL;
  if (kind & CLIP_KIND_PALETTE) {
    if (available - n < CLIP_PALETTE_BYTES) {
      return 0;
    }
    r->palette = in + n;
    n += CLIP_PALETTE_BYTES;
  }
  if (available - n < CLIP_SAMPLES) {
    return 0;
  }
  r->sound = (const int8_t *)(in + n);
  n += CLIP_SAMPLES;
  r->picture = NULL;
  r->picture_bytes = 0;
  if (r->kind == CLIP_HELD) {
    return n;
  }
  r->picture = in + n;
  for (int y = 0; y < CLIP_HEIGHT; y++) {
    size_t used = clip_decode_row(in + n, available - n,
                                  pixels + (size_t)y * CLIP_WIDTH);
    if (used == 0) {
      return 0;
    }
    n += used;
  }
  r->picture_bytes = n - (size_t)(r->picture - in);
  return n;
}

// --- Writing -------------------------------------------------------------------

static void emit(clip_writer_t *w, const void *data, uint32_t len) {
  if (w->error != 0 || len == 0) {
    return;
  }
  int r = w->io.write(w->io.ctx, data, len);
  if (r < 0) {
    w->error = r;
    return;
  }
  if (w->offset >= CLIP_HEADER_BYTES) {
    w->header.crc = crc32_update(w->header.crc, data, len);
  }
  w->offset += len;
}

// The record just written: the largest so far?
static void close_record(clip_writer_t *w) {
  uint32_t size = w->offset - w->record_start;
  if (w->record_start >= CLIP_HEADER_BYTES && size > w->header.largest_record) {
    w->header.largest_record = size;
  }
}

int clip_writer_begin(clip_writer_t *w, const clip_io_t *io,
                      const clip_header_t *header) {
  memset(w, 0, sizeof(*w));
  w->io = *io;
  w->header = *header;
  w->header.frames = 0;
  w->header.index_offset = 0;
  w->header.index_count = 0;
  w->header.largest_record = 0;
  w->header.crc = 0;
  uint8_t h[CLIP_HEADER_BYTES];
  clip_header_write(&w->header, h);
  emit(w, h, CLIP_HEADER_BYTES);
  w->record_start = w->offset;
  return w->error;
}

bool clip_writer_picture(clip_writer_t *w, uint32_t frames,
                         const uint16_t palette[16], bool force_key,
                         const int8_t sound[CLIP_SAMPLES]) {
  close_record(w);
  w->record_start = w->offset;
  w->key = force_key || w->frame == 0 ||
           w->frame - w->last_key >= CLIP_KEY_FRAMES;
  bool new_palette =
      w->key || !w->have_palette ||
      memcmp(palette, w->palette, sizeof(w->palette)) != 0;
  if (w->key) {
    w->last_key = w->frame;
    if (w->header.index_count < CLIP_MAX_KEYS) {
      w->keys[w->header.index_count][0] = w->frame;
      w->keys[w->header.index_count][1] = w->offset;
      w->header.index_count++;
    } else if (w->error == 0) {
      w->error = -3;  // more keys than the index holds
    }
  }
  uint8_t kind = (uint8_t)((w->key ? CLIP_KEY : CLIP_DELTA) |
                           (new_palette ? CLIP_KIND_PALETTE : 0u));
  emit(w, &kind, 1);
  if (new_palette) {
    uint8_t bytes[CLIP_PALETTE_BYTES];
    for (int e = 0; e < 16; e++) {
      put16(bytes + 2 * e, palette[e]);
    }
    emit(w, bytes, CLIP_PALETTE_BYTES);
    memcpy(w->palette, palette, sizeof(w->palette));
    w->have_palette = true;
  }
  emit(w, sound, CLIP_SAMPLES);
  w->frame++;
  w->frames_left = frames > 0 ? frames - 1 : 0;
  return w->key;
}

void clip_writer_row(clip_writer_t *w, const uint8_t *row,
                     const uint8_t *before) {
  size_t n = clip_encode_row(row, w->key ? NULL : before, w->row);
  emit(w, w->row, (uint32_t)n);
}

void clip_writer_held(clip_writer_t *w, const int8_t sound[CLIP_SAMPLES]) {
  close_record(w);
  w->record_start = w->offset;
  if (w->frames_left == 0) {
    if (w->error == 0) {
      w->error = -4;  // more frames than the picture has
    }
  } else {
    w->frames_left--;
  }
  uint8_t kind = CLIP_HELD;
  emit(w, &kind, 1);
  emit(w, sound, CLIP_SAMPLES);
  w->frame++;
}

int clip_writer_finish(clip_writer_t *w) {
  close_record(w);
  if (w->frames_left != 0 && w->error == 0) {
    w->error = -5;  // a picture's frames not all written
  }
  w->header.frames = w->frame;
  w->header.index_offset = w->offset;
  for (uint32_t k = 0; k < w->header.index_count; k++) {
    uint8_t entry[8];
    put32(entry, w->keys[k][0]);
    put32(entry + 4, w->keys[k][1]);
    emit(w, entry, sizeof(entry));
  }
  if (w->error == 0) {
    uint8_t h[CLIP_HEADER_BYTES];
    clip_header_write(&w->header, h);
    int r = w->io.rewrite_header(w->io.ctx, h);
    if (r < 0) {
      w->error = r;
    }
  }
  return w->error;
}
