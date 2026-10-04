/**
 * File: clipplay.h
 * Description: A clip file (clip.h) played back: its records read in order,
 *              each frame's sound and palette handed out and its picture
 *              decoded straight into the caller's pixels (fb_chunked_buffer
 *              on the cartridge), one frame a call.
 *
 * The reader never holds a whole record (up to CLIP_RECORD_MAX bytes): it
 * reads the file in pieces of CLIPPLAY_PIECE bytes at offsets of that size
 * (whole sectors, the card's fast way) into a buffer with CLIPPLAY_FRONT
 * bytes before them. What is left of a piece moves there before the next
 * one is read, so a record's start (its kind, palette and sound) and each of
 * its rows lie in one stretch of memory. A held frame decodes nothing.
 *
 * Plain C over read and seek callbacks (a FIL on the cartridge, a FILE or
 * memory on a PC), no allocation.
 */

#ifndef CLIPPLAY_H
#define CLIPPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "clip.h"

#define CLIPPLAY_PIECE 8192u
// A record's start: its kind, a palette and its sound (915 bytes).
#define CLIPPLAY_FRONT 1024u
#define CLIPPLAY_BUFFER_BYTES (CLIPPLAY_FRONT + CLIPPLAY_PIECE)

enum {
  CLIPPLAY_ERR_IO = -40,      // the read or the seek failed
  CLIPPLAY_ERR_HEADER = -41,  // not a clip file, or not version 1
  CLIPPLAY_ERR_RECORD = -42,  // a record malformed, or cut short
};

typedef struct {
  // Reads up to `len` bytes at the file's position; returns how many, 0 at
  // its end, negative on an error.
  int (*read)(void *ctx, uint8_t *buf, uint32_t len);
  // Moves the file's position to `offset`; 0, or negative on an error.
  int (*seek)(void *ctx, uint32_t offset);
  void *ctx;
} clipplay_io_t;

// A frame handed out.
typedef struct {
  int kind;               // CLIP_HELD, CLIP_DELTA or CLIP_KEY
  bool palette_changed;   // a palette came with it
  const uint16_t *palette;  // 16 0x0RGB words, in use from this frame on
  const int8_t *sound;      // CLIP_SAMPLES samples
} clipplay_frame_t;

typedef struct {
  clipplay_io_t io;
  clip_header_t header;
  uint8_t *buf;        // CLIPPLAY_BUFFER_BYTES, the caller's
  uint32_t pos;        // the next byte in buf
  uint32_t end;        // the bytes in buf
  uint32_t file_end;   // the file's offset just after buf's last byte
  bool eof;            // the file has nothing after buf
  uint32_t frame;      // the next frame
  uint16_t palette[16];
  int8_t sound[CLIP_SAMPLES];
  uint32_t pieces;     // read so far
} clipplay_t;

// Reads the header at the file's start and checks it (its fields only, not
// the file's CRC-32), then stands at the first frame. `buf` is
// CLIPPLAY_BUFFER_BYTES. Returns 0 or a negative CLIPPLAY_ERR_*.
int clipplay_open(clipplay_t *p, const clipplay_io_t *io, uint8_t *buf);

// Stands at the last key picture at or before `frame` (from the index at
// the file's end). Returns that key's frame, or a negative CLIPPLAY_ERR_*.
int clipplay_start(clipplay_t *p, uint32_t frame);

// The next frame: its sound and palette in `out`, its picture (if it has
// one) decoded into `pixels` (CLIP_WIDTH x CLIP_HEIGHT indices, which hold
// the picture before). Returns 1, 0 after the clip's last frame, or a
// negative CLIPPLAY_ERR_*.
int clipplay_next(clipplay_t *p, uint8_t *pixels, clipplay_frame_t *out);

#endif  // CLIPPLAY_H
