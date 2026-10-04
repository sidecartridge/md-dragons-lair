/**
 * File: clipplay.c
 * Description: A clip file played back. See clipplay.h.
 */

#include "clipplay.h"

#include <string.h>

// A record's start: its kind, a palette when it has one, its sound.
#define RECORD_START (1u + CLIP_PALETTE_BYTES + CLIP_SAMPLES)

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// The file's offset of the next byte in buf.
static uint32_t offset_of_pos(const clipplay_t *p) {
  return p->file_end - (p->end - p->pos);
}

// At least `n` bytes (at most CLIPPLAY_FRONT) from pos in buf, fewer only at
// the file's end: what is left moves before the piece area and the next
// piece is read. Returns the bytes there, or CLIPPLAY_ERR_IO.
static int ensure(clipplay_t *p, uint32_t n) {
  while (p->end - p->pos < n && !p->eof) {
    uint32_t left = p->end - p->pos;
    memmove(p->buf + CLIPPLAY_FRONT - left, p->buf + p->pos, left);
    int got = p->io.read(p->io.ctx, p->buf + CLIPPLAY_FRONT, CLIPPLAY_PIECE);
    if (got < 0) {
      return CLIPPLAY_ERR_IO;
    }
    p->pos = CLIPPLAY_FRONT - left;
    p->end = CLIPPLAY_FRONT + (uint32_t)got;
    p->file_end += (uint32_t)got;
    p->pieces++;
    if ((uint32_t)got < CLIPPLAY_PIECE) {
      p->eof = true;
    }
  }
  return (int)(p->end - p->pos);
}

// The bytes from pos that belong to the records (the index follows them),
// of the `there` in buf.
static uint32_t records_left(const clipplay_t *p, int there) {
  uint32_t at = offset_of_pos(p);
  uint32_t left = (at < p->header.index_offset) ? p->header.index_offset - at
                                                 : 0;
  return ((uint32_t)there < left) ? (uint32_t)there : left;
}

// Stands at the file's `offset`: the piece holding it read, pos on it.
static int stand_at(clipplay_t *p, uint32_t offset) {
  uint32_t piece = offset - offset % CLIPPLAY_PIECE;
  if (p->io.seek(p->io.ctx, piece) < 0) {
    return CLIPPLAY_ERR_IO;
  }
  p->pos = CLIPPLAY_FRONT;
  p->end = CLIPPLAY_FRONT;
  p->file_end = piece;
  p->eof = false;
  int there = ensure(p, 1);
  if (there < 0) {
    return there;
  }
  if ((uint32_t)there <= offset - piece) {
    return CLIPPLAY_ERR_RECORD;  // the file ends before it
  }
  p->pos += offset - piece;
  return 0;
}

int clipplay_open(clipplay_t *p, const clipplay_io_t *io, uint8_t *buf) {
  memset(p, 0, sizeof(*p));
  p->io = *io;
  p->buf = buf;
  int r = stand_at(p, 0);
  if (r < 0) {
    return r == CLIPPLAY_ERR_RECORD ? CLIPPLAY_ERR_HEADER : r;
  }
  r = ensure(p, CLIP_HEADER_BYTES);
  if (r < 0) {
    return r;
  }
  if ((uint32_t)r < CLIP_HEADER_BYTES ||
      clip_header_read(&p->header, p->buf + p->pos) != 0 ||
      p->header.index_offset < CLIP_HEADER_BYTES) {
    return CLIPPLAY_ERR_HEADER;
  }
  p->pos += CLIP_HEADER_BYTES;
  return 0;
}

int clipplay_start(clipplay_t *p, uint32_t frame) {
  uint32_t bytes = p->header.index_count * 8u;
  if (bytes == 0 || bytes > CLIPPLAY_PIECE) {
    return CLIPPLAY_ERR_HEADER;
  }
  if (p->io.seek(p->io.ctx, p->header.index_offset) < 0) {
    return CLIPPLAY_ERR_IO;
  }
  uint8_t *index = p->buf + CLIPPLAY_FRONT;
  for (uint32_t have = 0; have < bytes;) {
    int got = p->io.read(p->io.ctx, index + have, bytes - have);
    if (got <= 0) {
      return got < 0 ? CLIPPLAY_ERR_IO : CLIPPLAY_ERR_HEADER;
    }
    have += (uint32_t)got;
  }
  // The last key at or before `frame`: the index is in frame order.
  uint32_t key_frame = get32(index);
  uint32_t key_offset = get32(index + 4);
  for (uint32_t e = 1; e < p->header.index_count; e++) {
    uint32_t f = get32(index + 8u * e);
    if (f > frame) {
      break;
    }
    key_frame = f;
    key_offset = get32(index + 8u * e + 4u);
  }
  if (key_offset < CLIP_HEADER_BYTES || key_offset >= p->header.index_offset ||
      key_frame >= p->header.frames) {
    return CLIPPLAY_ERR_HEADER;
  }
  int r = stand_at(p, key_offset);
  if (r < 0) {
    return r;
  }
  p->frame = key_frame;
  return (int)key_frame;
}

int clipplay_next(clipplay_t *p, uint8_t *pixels, clipplay_frame_t *out) {
  if (p->frame >= p->header.frames) {
    return 0;
  }
  int there = ensure(p, RECORD_START);
  if (there < 0) {
    return there;
  }
  uint32_t avail = records_left(p, there);
  if (avail < 1) {
    return CLIPPLAY_ERR_RECORD;
  }
  const uint8_t *in = p->buf + p->pos;
  uint8_t kind = in[0];
  int k = kind & CLIP_KIND_MASK;
  bool has_palette = (kind & CLIP_KIND_PALETTE) != 0;
  uint32_t n = 1u + (has_palette ? CLIP_PALETTE_BYTES : 0u);
  if ((kind & ~(CLIP_KIND_MASK | CLIP_KIND_PALETTE)) != 0 || k > CLIP_KEY ||
      avail < n + CLIP_SAMPLES) {
    return CLIPPLAY_ERR_RECORD;
  }
  if (has_palette) {
    for (int e = 0; e < 16; e++) {
      p->palette[e] = (uint16_t)(in[1 + 2 * e] | (in[2 + 2 * e] << 8));
    }
  }
  memcpy(p->sound, in + n, CLIP_SAMPLES);
  p->pos += n + CLIP_SAMPLES;
  if (k != CLIP_HELD) {
    for (int y = 0; y < CLIP_HEIGHT; y++) {
      there = ensure(p, CLIP_ROW_MAX);
      if (there < 0) {
        return there;
      }
      size_t used = clip_decode_row(p->buf + p->pos, records_left(p, there),
                                    pixels + (size_t)y * CLIP_WIDTH);
      if (used == 0) {
        return CLIPPLAY_ERR_RECORD;
      }
      p->pos += (uint32_t)used;
    }
  }
  p->frame++;
  out->kind = k;
  out->palette_changed = has_palette;
  out->palette = p->palette;
  out->sound = p->sound;
  return 1;
}
