/**
 * File: clip.h
 * Description: The app's converted clip file, version 1 (specified in
 *              docs/clip-format.md): a header, one record per output frame
 *              (its sound, and its picture when a new one starts, as row
 *              runs against the picture before), then an index of the key
 *              pictures. It is written and read in one pass, front to back;
 *              the header alone is rewritten when the clip is finished.
 *
 * Plain C, integers only, no allocation: the cartridge and the PC tool
 * write the same bytes, and the player reads them with the same code.
 */

#ifndef CLIP_H
#define CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLIP_MAGIC "DLCL"
#define CLIP_VERSION 1
#define CLIP_HEADER_BYTES 64

#define CLIP_WIDTH 320
#define CLIP_HEIGHT 200
#define CLIP_FPS 25
#define CLIP_SAMPLE_RATE 22050
#define CLIP_SAMPLES (CLIP_SAMPLE_RATE / CLIP_FPS)  // a frame's sound: 882

// A row's encoding is never longer than its pixels stored as literals.
#define CLIP_ROW_MAX 165
#define CLIP_PICTURE_MAX (CLIP_HEIGHT * CLIP_ROW_MAX)  // 33,000
#define CLIP_PALETTE_BYTES 32
// The longest record: its kind, a palette, its sound and a picture.
#define CLIP_RECORD_MAX \
  (1 + CLIP_PALETTE_BYTES + CLIP_SAMPLES + CLIP_PICTURE_MAX)

// A key picture at least every CLIP_KEY_FRAMES frames (2 s).
#define CLIP_KEY_FRAMES 50
// Key pictures a clip can index: the game's longest clip has 49.
#define CLIP_MAX_KEYS 256

// A record's kind byte.
enum {
  CLIP_HELD = 0,   // the picture before, shown again
  CLIP_DELTA = 1,  // a picture, as row runs against the picture before
  CLIP_KEY = 2,    // a picture, its rows alone (a key: playable from here)
};
#define CLIP_KIND_MASK 0x03u
#define CLIP_KIND_PALETTE 0x80u  // a palette follows the kind byte

// Header flags.
#define CLIP_FLAG_MIRRORED 0x01u  // reserved: 0 in version 1

typedef struct {
  uint32_t frames;          // records
  uint8_t gun_bits;         // the palette's depth: 3 (ST) or 4 (STE)
  uint8_t flags;            // CLIP_FLAG_*
  uint16_t converter;       // the converter's version (convert.h)
  uint16_t keep_percent;    // its palette stability, 0xFFFF for none
  uint32_t source_bytes;    // the source clip's size
  uint32_t source_crc;      // and CRC-32: a stale conversion, a duplicate
  uint32_t index_offset;    // the key index, from the file's start
  uint32_t index_count;     // its entries
  uint32_t largest_record;  // in bytes: sizes a reader's buffer
  uint32_t crc;             // CRC-32 of everything after the header
} clip_header_t;

void clip_header_write(const clip_header_t *h,
                       uint8_t out[CLIP_HEADER_BYTES]);

// 0, or -1 for another file, -2 for another version.
int clip_header_read(clip_header_t *h, const uint8_t in[CLIP_HEADER_BYTES]);

// --- Rows ----------------------------------------------------------------------

// A row of CLIP_WIDTH colour indices (0..15) against the row before it in
// the picture before (NULL: a key picture's row). Writes at most
// CLIP_ROW_MAX bytes to `out`; returns how many.
size_t clip_encode_row(const uint8_t *row, const uint8_t *before,
                       uint8_t *out);

// Applies an encoded row to `row`, which holds the picture before's row
// (pixels a key row does not set are not left: it sets them all). Returns
// the bytes it took from `in`, or 0 for a malformed row or one longer than
// `available`.
size_t clip_decode_row(const uint8_t *in, size_t available, uint8_t *row);

// --- Records --------------------------------------------------------------------

// A record parsed where it lies.
typedef struct {
  int kind;                // CLIP_HELD, CLIP_DELTA or CLIP_KEY
  const uint8_t *palette;  // 16 little-endian 0x0RGB words, or NULL
  const int8_t *sound;     // CLIP_SAMPLES samples
  const uint8_t *picture;  // its rows, or NULL (a held frame)
  size_t picture_bytes;
} clip_record_t;

// Parses the record at `in`, applying its picture to `pixels`
// (CLIP_WIDTH x CLIP_HEIGHT indices, the picture before). Returns the
// record's size, or 0 when it is malformed or longer than `available`.
size_t clip_read_record(const uint8_t *in, size_t available,
                        clip_record_t *r, uint8_t *pixels);

// --- Writing -------------------------------------------------------------------

typedef struct {
  // Appends bytes to the file; 0, or negative on an error.
  int (*write)(void *ctx, const void *data, uint32_t len);
  // Writes the header again at the file's start; 0, or negative.
  int (*rewrite_header)(void *ctx, const uint8_t header[CLIP_HEADER_BYTES]);
  void *ctx;
} clip_io_t;

typedef struct {
  clip_io_t io;
  clip_header_t header;
  int error;              // the first error, sticky
  uint32_t offset;        // bytes written
  uint32_t record_start;  // the record being written's offset
  uint32_t frame;         // the next record's frame
  uint32_t last_key;      // frame of the last key picture
  bool key;               // the picture being written is a key
  uint32_t frames_left;   // of the picture being written, after its first
  uint16_t palette[16];   // the palette in the file
  bool have_palette;
  uint32_t keys[CLIP_MAX_KEYS][2];  // key frames and their records' offsets
  uint8_t row[CLIP_ROW_MAX];
} clip_writer_t;

// Starts a clip: writes a header to be completed by clip_writer_finish().
// `header` gives its gun_bits, converter, keep_percent and source fields.
int clip_writer_begin(clip_writer_t *w, const clip_io_t *io,
                      const clip_header_t *header);

// Starts the record of a picture shown for `frames` frames from the next
// one, with `palette` (16 0x0RGB words). Returns whether it is a key
// picture (the clip's first, every CLIP_KEY_FRAMES frames, or `force_key`):
// its rows are then encoded alone. `sound` is the record's first frame's.
bool clip_writer_picture(clip_writer_t *w, uint32_t frames,
                         const uint16_t palette[16], bool force_key,
                         const int8_t sound[CLIP_SAMPLES]);

// The picture's rows, in order: `before` the picture before's row (ignored
// for a key picture).
void clip_writer_row(clip_writer_t *w, const uint8_t *row,
                     const uint8_t *before);

// After the picture's rows: the held records of its other frames, one
// sound each (frames - 1 of them).
void clip_writer_held(clip_writer_t *w, const int8_t sound[CLIP_SAMPLES]);

// Ends the clip: the key index, then the completed header. Returns 0 or the
// first error.
int clip_writer_finish(clip_writer_t *w);

#endif  // CLIP_H
