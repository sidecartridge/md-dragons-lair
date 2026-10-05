// units: rp/src/clipplay.c rp/src/clip.c rp/src/crc32.c
/* The clip reader: clips written by the clip writer (every picture whole,
 * and as deltas) played back frame by frame give every picture, palette and
 * sound they were written with, the pieces read whole and in order, a held
 * frame decoding nothing. Starting at any frame stands at the last key at
 * or before it, from which the frames play on as written. A file cut short
 * anywhere, or with a byte changed, ends with an error or plays on: never a
 * hang, never a read or write outside the buffers (the sanitizers). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clipplay.h"
#include "test.h"

#define W CLIP_WIDTH
#define H CLIP_HEIGHT
#define PICTURES 40

static uint32_t rng = 7;
static uint32_t next(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 16;
}

// --- The source: pictures, palettes, sound -----------------------------------

static uint8_t pictures[PICTURES][H][W];
static uint16_t palettes[PICTURES][16];
static uint32_t frames_of[PICTURES];
static uint32_t first_of[PICTURES];
static uint32_t total;

static void sound_of(uint32_t frame, int8_t out[CLIP_SAMPLES]) {
  for (int i = 0; i < CLIP_SAMPLES; i++) {
    out[i] = (int8_t)(frame * 3 + (uint32_t)i * 5);
  }
}

static void make_source(void) {
  total = 0;
  for (int p = 0; p < PICTURES; p++) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        uint8_t v = (p == 0 || p % 13 == 0) ? (uint8_t)((x / 7 + y / 5 + p) & 15)
                                            : pictures[p - 1][y][x];
        if (next() % 9 == 0) {
          v = (uint8_t)(next() & 15u);
        }
        pictures[p][y][x] = v;
      }
    }
    for (int e = 0; e < 16; e++) {
      palettes[p][e] = (p == 0 || p % 3 == 0) ? (uint16_t)(next() & 0xFFFu)
                                              : palettes[p - 1][e];
    }
    frames_of[p] = 1 + next() % 3;
    first_of[p] = total;
    total += frames_of[p];
  }
}

// --- The file, in memory -----------------------------------------------------

static uint8_t file[2u << 20];
static uint32_t file_len;

static int mem_write(void *ctx, const void *data, uint32_t len) {
  (void)ctx;
  if (file_len + len > sizeof(file)) {
    return -1;
  }
  memcpy(file + file_len, data, len);
  file_len += len;
  return 0;
}

static int mem_header(void *ctx, const uint8_t header[CLIP_HEADER_BYTES]) {
  (void)ctx;
  memcpy(file, header, CLIP_HEADER_BYTES);
  return 0;
}

static void write_clip(bool whole) {
  clip_io_t io = {mem_write, mem_header, NULL};
  clip_header_t h = {0};
  h.gun_bits = 4;
  h.converter = 1;
  static clip_writer_t w;
  file_len = 0;
  CHECK_EQ(clip_writer_begin(&w, &io, &h, whole), 0);
  uint32_t frame = 0;
  int8_t sound[CLIP_SAMPLES];
  for (int p = 0; p < PICTURES; p++) {
    sound_of(frame++, sound);
    clip_writer_picture(&w, frames_of[p], palettes[p], false, sound);
    for (int y = 0; y < H; y++) {
      clip_writer_row(&w, pictures[p][y], p > 0 ? pictures[p - 1][y] : NULL);
    }
    for (uint32_t f = 1; f < frames_of[p]; f++) {
      sound_of(frame++, sound);
      clip_writer_held(&w, sound);
    }
  }
  CHECK_EQ(clip_writer_finish(&w), 0);
}

// The reader's view of the file: its first `len` bytes, with `damage` (if
// not ~0) changed.
typedef struct {
  uint32_t len;
  uint32_t pos;
  uint32_t damage;
  uint32_t reads;
  uint32_t misaligned;  // reads that did not start at a piece's offset
} view_t;

static int view_read(void *ctx, uint8_t *buf, uint32_t len) {
  view_t *v = (view_t *)ctx;
  if (v->pos % CLIPPLAY_PIECE != 0) {
    v->misaligned++;
  }
  v->reads++;
  uint32_t n = v->pos < v->len ? v->len - v->pos : 0;
  n = n < len ? n : len;
  memcpy(buf, file + v->pos, n);
  if (v->damage >= v->pos && v->damage < v->pos + n) {
    buf[v->damage - v->pos] ^= 0x5Au;
  }
  v->pos += n;
  return (int)n;
}

static int view_seek(void *ctx, uint32_t offset) {
  view_t *v = (view_t *)ctx;
  if (offset > v->len) {
    return -1;
  }
  v->pos = offset;
  return 0;
}

// --- Playing it ----------------------------------------------------------------

static uint8_t *buf;  // CLIPPLAY_BUFFER_BYTES, on the heap: the sanitizers
                      // see a read past it
static uint8_t pixels[H][W];

// The picture shown at `frame`.
static int picture_at(uint32_t frame) {
  int p = 0;
  while (p + 1 < PICTURES && first_of[p + 1] <= frame) {
    p++;
  }
  return p;
}

// Plays from `start` to the end; every frame checked. Returns the frames.
static uint32_t play(bool whole, uint32_t start) {
  view_t v = {file_len, 0, ~0u, 0, 0};
  clipplay_io_t io = {view_read, view_seek, &v};
  static clipplay_t p;
  CHECK_EQ(clipplay_open(&p, &io, buf), 0);
  CHECK_EQ(p.header.frames, total);
  uint32_t frame = 0;
  if (start > 0) {
    int key = clipplay_start(&p, start);
    CHECK(key >= 0 && (uint32_t)key <= start);
    if (key < 0) {
      return 0;
    }
    frame = (uint32_t)key;
    // A key starts a picture: the frame before it shows another.
    CHECK(first_of[picture_at(frame)] == frame);
    if (!whole) {
      // Deltas from a key on: the pixels before it do not matter.
      memset(pixels, 0xEE, sizeof(pixels));
    }
  }
  clipplay_frame_t f;
  int r;
  uint32_t played = 0;
  while ((r = clipplay_next(&p, &pixels[0][0], &f)) == 1) {
    int pic = picture_at(frame);
    bool first = first_of[pic] == frame;
    CHECK_EQ(f.kind != CLIP_HELD, first);
    if (whole && first) {
      CHECK_EQ(f.kind, CLIP_KEY);
    }
    CHECK(memcmp(f.palette, palettes[pic], sizeof(palettes[pic])) == 0);
    int8_t want[CLIP_SAMPLES];
    sound_of(frame, want);
    CHECK(memcmp(f.sound, want, CLIP_SAMPLES) == 0);
    CHECK(memcmp(pixels, pictures[pic], sizeof(pixels)) == 0);
    frame++;
    played++;
  }
  CHECK_EQ(r, 0);
  CHECK_EQ(frame, total);
  CHECK_EQ(v.misaligned, start > 0 ? 1u : 0u);  // the index's read alone
  return played;
}

// The file cut at `len`, or with byte `damage` changed: plays to its end or
// stops with an error, within as many calls as the clip has frames.
static void play_damaged(uint32_t len, uint32_t damage) {
  view_t v = {len, 0, damage, 0, 0};
  clipplay_io_t io = {view_read, view_seek, &v};
  static clipplay_t p;
  if (clipplay_open(&p, &io, buf) != 0) {
    return;
  }
  clipplay_frame_t f;
  uint32_t calls = 0;
  int r;
  while ((r = clipplay_next(&p, &pixels[0][0], &f)) == 1) {
    CHECK(++calls <= p.header.frames);
    if (calls > p.header.frames) {
      return;
    }
  }
  CHECK(r == 0 || r == CLIPPLAY_ERR_RECORD);
  if (len < file_len && r == 0) {
    // Cut in the index alone: every record was there.
    CHECK(len >= p.header.index_offset);
  }
}

int main(void) {
  buf = malloc(CLIPPLAY_BUFFER_BYTES);
  make_source();
  for (int whole = 1; whole >= 0; whole--) {
    write_clip(whole != 0);
    CHECK(file_len > 4u * CLIPPLAY_PIECE);  // many pieces
    CHECK_EQ(play(whole != 0, 0), total);
    for (uint32_t start = 1; start < total; start += 7) {
      play(whole != 0, start);
    }
    for (uint32_t cut = 0; cut < file_len; cut += 1 + next() % 16381) {
      play_damaged(cut, ~0u);
    }
    for (int t = 0; t < 30; t++) {
      play_damaged(file_len, (uint32_t)((next() << 16 | next()) % file_len));
    }
  }
  // Not a clip file.
  memset(file, 0, 4096);
  file_len = 4096;
  view_t v = {file_len, 0, ~0u, 0, 0};
  clipplay_io_t io = {view_read, view_seek, &v};
  static clipplay_t p;
  CHECK_EQ(clipplay_open(&p, &io, buf), CLIPPLAY_ERR_HEADER);
  free(buf);
  TEST_END();
}
