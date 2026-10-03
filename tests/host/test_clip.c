// units: rp/src/clip.c rp/src/crc32.c
/* The clip file, version 1: rows encoded and decoded back to the same
 * pixels, never longer than CLIP_ROW_MAX, malformed rows refused; the
 * header both ways; and a whole clip written and read back, with its
 * pictures against the ones before and with every picture whole: the same
 * pictures, palettes and sounds, a palette only when it changed or on a
 * key, every key picture decoded alone, one in the index every
 * CLIP_KEY_FRAMES frames and the forced one, the CRC-32 of the file in its
 * header. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clip.h"
#include "crc32.h"
#include "test.h"

#define W CLIP_WIDTH
#define H CLIP_HEIGHT

static uint32_t rng = 1;
static uint32_t next(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 16;
}

// Encodes `row` against `before` and decodes it onto a copy of `before`
// (onto garbage for a key row): the same pixels, the same bytes taken.
static void round_trip(const uint8_t *row, const uint8_t *before) {
  uint8_t enc[CLIP_ROW_MAX + 8];
  memset(enc, 0x77, sizeof(enc));
  size_t n = clip_encode_row(row, before, enc);
  CHECK(n >= 1 && n <= CLIP_ROW_MAX);
  CHECK_EQ(enc[CLIP_ROW_MAX], 0x77);  // nothing written past the bound
  uint8_t out[W];
  if (before != NULL) {
    memcpy(out, before, W);
  } else {
    memset(out, 0xEE, W);
  }
  CHECK_EQ(clip_decode_row(enc, n, out), n);
  CHECK(memcmp(out, row, W) == 0);
  // One byte short is not a row.
  if (n > 1) {
    CHECK_EQ(clip_decode_row(enc, n - 1, out), (size_t)0);
  }
}

static void check_rows(void) {
  uint8_t row[W], before[W];
  for (int t = 0; t < 4000; t++) {
    int kind = t % 8;
    for (int x = 0; x < W; x++) {
      before[x] = (uint8_t)(next() & 15u);
    }
    for (int x = 0; x < W; x++) {
      switch (kind) {
        case 0:  // noise
          row[x] = (uint8_t)(next() & 15u);
          break;
        case 1:  // a few pixels changed
          row[x] = (next() % 16 == 0) ? (uint8_t)(next() & 15u) : before[x];
          break;
        case 2:  // runs of one colour
          row[x] = (x == 0 || next() % 12 == 0) ? (uint8_t)(next() & 15u)
                                                : row[x - 1];
          break;
        case 3:  // unchanged
          row[x] = before[x];
          break;
        case 4:  // pairs: no repeat of 3, no skip of 2
          row[x] = (uint8_t)((x / 2) & 1 ? 5 : 9);
          before[x] = (uint8_t)((x & 1) ? row[x] : 15 - row[x]);
          break;
        case 5:  // the end unchanged, the start not
          row[x] = x < W / 2 ? (uint8_t)(next() & 15u) : before[x];
          break;
        case 6:  // one colour
          row[x] = 7;
          break;
        default:  // a single changed pixel at either end
          row[x] = before[x];
          if (x == 0 || x == W - 1) {
            row[x] = (uint8_t)(before[x] ^ 1u);
          }
          break;
      }
    }
    round_trip(row, before);
    round_trip(row, NULL);
  }
  // The worst row: alternating so nothing compresses, against a row equal
  // at every other pixel; capped at its literal size.
  for (int x = 0; x < W; x++) {
    row[x] = (uint8_t)(x & 15);
    before[x] = (x % 2) ? row[x] : (uint8_t)(15 - row[x]);
  }
  round_trip(row, before);
  uint8_t enc[CLIP_ROW_MAX];
  CHECK_EQ(clip_encode_row(row, NULL, enc), (size_t)CLIP_ROW_MAX);

  // Malformed: a skip past the row's end, a repeat with no colour, the
  // reserved opcodes.
  uint8_t out[W] = {0};
  uint8_t bad1[] = {0x3F, 0x3F, 0x3F, 0x3F, 0x30, 0x3F};  // 305 + 64 > 320
  CHECK_EQ(clip_decode_row(bad1, sizeof(bad1), out), (size_t)0);
  uint8_t bad2[] = {0xBF};
  CHECK_EQ(clip_decode_row(bad2, sizeof(bad2), out), (size_t)0);
  uint8_t bad3[] = {0xC1};
  CHECK_EQ(clip_decode_row(bad3, sizeof(bad3), out), (size_t)0);
  uint8_t rest[] = {0xC0};
  CHECK_EQ(clip_decode_row(rest, sizeof(rest), out), (size_t)1);
}

static void check_header(void) {
  clip_header_t h = {1079, 3, 0, 1, 10, 6605532, 0x12345678u, 999, 22, 12345,
                     0xCAFEF00Du};
  uint8_t bytes[CLIP_HEADER_BYTES];
  clip_header_write(&h, bytes);
  CHECK(memcmp(bytes, "DLCL", 4) == 0);
  clip_header_t back;
  CHECK_EQ(clip_header_read(&back, bytes), 0);
  CHECK_EQ(back.frames, h.frames);
  CHECK_EQ(back.gun_bits, h.gun_bits);
  CHECK_EQ(back.flags, h.flags);
  CHECK_EQ(back.converter, h.converter);
  CHECK_EQ(back.keep_percent, h.keep_percent);
  CHECK_EQ(back.source_bytes, h.source_bytes);
  CHECK_EQ(back.source_crc, h.source_crc);
  CHECK_EQ(back.index_offset, h.index_offset);
  CHECK_EQ(back.index_count, h.index_count);
  CHECK_EQ(back.largest_record, h.largest_record);
  CHECK_EQ(back.crc, h.crc);
  bytes[4] = 2;  // another version
  CHECK_EQ(clip_header_read(&back, bytes), -2);
  bytes[0] = 'X';  // another file
  CHECK_EQ(clip_header_read(&back, bytes), -1);
}

// --- A whole clip, in memory ------------------------------------------------

static uint8_t file[4u << 20];
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

#define PICTURES 60

static uint8_t pictures[PICTURES][H][W];
static uint16_t palettes[PICTURES][16];
static uint32_t frames_of[PICTURES];

static void sound_of(uint32_t frame, int8_t out[CLIP_SAMPLES]) {
  for (int i = 0; i < CLIP_SAMPLES; i++) {
    out[i] = (int8_t)(frame * 7 + (uint32_t)i);
  }
}

static void make_pictures(void) {
  // Pictures that change a little from one to the next, a cut now and then;
  // palettes that change on some pictures; 1 to 3 frames each.
  for (int p = 0; p < PICTURES; p++) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        uint8_t v = (p == 0 || p % 17 == 0) ? (uint8_t)(next() & 15u)
                                            : pictures[p - 1][y][x];
        if (next() % 10 == 0) {
          v = (uint8_t)(next() & 15u);
        }
        pictures[p][y][x] = v;
      }
    }
    for (int e = 0; e < 16; e++) {
      palettes[p][e] = (p == 0 || p % 4 == 0) ? (uint16_t)(next() & 0xFFFu)
                                              : palettes[p - 1][e];
    }
    frames_of[p] = 1 + next() % 3;
  }
}

static void check_clip(bool whole) {
  uint32_t total = 0;
  for (int p = 0; p < PICTURES; p++) {
    total += frames_of[p];
  }
  clip_io_t io = {mem_write, mem_header, NULL};
  clip_header_t h = {0};
  h.gun_bits = 4;
  h.converter = 1;
  h.keep_percent = 10;
  h.source_bytes = 1234;
  h.source_crc = 0xABCDu;
  static clip_writer_t w;
  file_len = 0;
  CHECK_EQ(clip_writer_begin(&w, &io, &h, whole), 0);
  uint32_t frame = 0;
  int8_t sound[CLIP_SAMPLES];
  for (int p = 0; p < PICTURES; p++) {
    sound_of(frame++, sound);
    bool key = clip_writer_picture(&w, frames_of[p], palettes[p], p == 30,
                                   sound);
    CHECK(key || (!whole && p != 30));
    for (int y = 0; y < H; y++) {
      clip_writer_row(&w, pictures[p][y], p > 0 ? pictures[p - 1][y] : NULL);
    }
    for (uint32_t f = 1; f < frames_of[p]; f++) {
      sound_of(frame++, sound);
      clip_writer_held(&w, sound);
    }
  }
  CHECK_EQ(clip_writer_finish(&w), 0);

  // Read it back.
  clip_header_t back;
  CHECK_EQ(clip_header_read(&back, file), 0);
  CHECK_EQ(back.frames, total);
  CHECK_EQ(back.gun_bits, 4);
  CHECK_EQ(back.source_crc, 0xABCDu);
  CHECK_EQ(back.crc,
           crc32_update(0, file + CLIP_HEADER_BYTES,
                        file_len - CLIP_HEADER_BYTES));
  CHECK_EQ(back.index_offset + back.index_count * 8u, file_len);
  static uint8_t pixels[H][W];
  uint32_t at = CLIP_HEADER_BYTES;
  uint32_t largest = 0;
  uint32_t keys = 0;
  uint32_t last_key = 0;
  int p = -1;
  uint32_t held_left = 0;
  uint16_t palette[16] = {0};
  for (frame = 0; frame < back.frames; frame++) {
    clip_record_t r;
    size_t n = clip_read_record(file + at, back.index_offset - at, &r,
                                &pixels[0][0]);
    CHECK(n > 0);
    if (n == 0) {
      return;
    }
    largest = n > largest ? (uint32_t)n : largest;
    int8_t want[CLIP_SAMPLES];
    sound_of(frame, want);
    CHECK(memcmp(r.sound, want, CLIP_SAMPLES) == 0);
    if (r.palette != NULL) {
      for (int e = 0; e < 16; e++) {
        palette[e] = (uint16_t)(r.palette[2 * e] | (r.palette[2 * e + 1] << 8));
      }
    }
    if (r.kind == CLIP_HELD) {
      CHECK(held_left > 0);
      held_left--;
    } else {
      CHECK_EQ(held_left, 0u);
      p++;
      held_left = frames_of[p] - 1;
      CHECK(memcmp(pixels, pictures[p], sizeof(pixels)) == 0);
      CHECK(memcmp(palette, palettes[p], sizeof(palette)) == 0);
      // A palette only when it changed, and on every key picture.
      bool changed = p == 0 || memcmp(palettes[p], palettes[p - 1],
                                      sizeof(palettes[p])) != 0;
      CHECK((r.palette != NULL) == (changed || r.kind == CLIP_KEY));
      // The index's next entry, if it is this record.
      bool indexed = false;
      if (keys < back.index_count) {
        const uint8_t *e = file + back.index_offset + keys * 8u;
        uint32_t kf =
            e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24);
        uint32_t ko =
            e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
        if (kf == frame) {
          CHECK_EQ(ko, at);
          indexed = true;
          keys++;
          last_key = frame;
        }
      }
      CHECK(indexed || frame - last_key < CLIP_KEY_FRAMES);
      CHECK(indexed || p != 30);
      CHECK_EQ(r.kind, (indexed || whole) ? CLIP_KEY : CLIP_DELTA);
      // A key alone: onto garbage, the same picture.
      if (r.kind == CLIP_KEY) {
        static uint8_t alone[H][W];
        memset(alone, 0xEE, sizeof(alone));
        clip_record_t r2;
        CHECK_EQ(clip_read_record(file + at, back.index_offset - at, &r2,
                                  &alone[0][0]),
                 n);
        CHECK(memcmp(alone, pictures[p], sizeof(alone)) == 0);
      }
    }
    at += (uint32_t)n;
  }
  CHECK_EQ(at, back.index_offset);
  CHECK_EQ(keys, back.index_count);
  CHECK_EQ(p, PICTURES - 1);
  CHECK_EQ(largest, back.largest_record);
  CHECK(largest <= CLIP_RECORD_MAX);
}

int main(void) {
  check_rows();
  check_header();
  make_pictures();
  check_clip(false);
  check_clip(true);
  TEST_END();
}
