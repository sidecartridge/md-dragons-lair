/**
 * File: convjob.h
 * Description: A clip of the CD-ROM image converted on the cartridge into
 *              the app's clip file on the SD card (clip.h), the same bytes
 *              as dlconv encode, one picture a call so that the main loop
 *              keeps running between them.
 *
 * Memory: the frame store's rows and every large buffer come from the
 * caller (convjob_memory_t), wherever it has room; the converter's passes
 * work in the store's spare rows and two small buffers (convert.h). Every
 * picture is stored whole (clip_writer_begin()'s `whole`), so the picture
 * before is not needed: there is no room to keep it, and keeping it on the
 * card took more time than converting. The file is written in pieces of
 * CONVJOB_OUT_BYTES at offsets of that size, the card's fast way: a few
 * hundred bytes at a time took a quarter of the time.
 */

#ifndef CONVJOB_H
#define CONVJOB_H

#include <stdbool.h>
#include <stdint.h>

#include "clip.h"
#include "convert.h"
#include "ff.h"
#include "iso9660.h"
#include "mp2_audio.h"
#include "mpeg1_video.h"
#include "mpeg_ps.h"

enum {
  CONVJOB_ERR_OPEN = -30,  // the clip or the output
};

// The sound decoded ahead of the frames: a frame's samples and an MP2
// frame's.
#define CONVJOB_SOUND_BYTES (CLIP_SAMPLES + MP2_FRAME_SAMPLES)

// The clip file's pieces: 4 KB writes run at about 1 MB/s on the card.
#define CONVJOB_OUT_BYTES 4096

typedef struct {
  // The frame store's rows, MPEG1_SLOT_BYTES each, 4-byte aligned: the
  // clip's macroblock rows and 2 (17 for the game).
  uint8_t *rows[MPEG1_MAX_SLOTS];
  int row_count;
  uint8_t *ring_c;   // PICTURE16_RING_C_BYTES, 4-byte aligned
  uint8_t *lines_y;  // PICTURE16_LINES_Y_BYTES, 4-byte aligned
  mpeg1_t *dec;
  mpeg_ps_t *video;
  mpeg_ps_t *audio;
  mp2_t *mp2;
  clip_writer_t *writer;
  int8_t *sound;  // CONVJOB_SOUND_BYTES
  int16_t *pcm;   // MP2_FRAME_SAMPLES
} convjob_memory_t;

// Where the time goes, in microseconds (and the passes' cycles).
typedef struct {
  uint64_t total;
  uint64_t read;     // the clip from the image: pictures and sound
  uint64_t sound;    // MP2 decoded and turned to 8 bits
  uint64_t write;    // the clip file
  uint64_t histogram_cycles;
  uint64_t palette_cycles;
  uint64_t dither_cycles;
} convjob_times_t;

typedef struct {
  convjob_memory_t m;
  convert_t conv;
  iso9660_file_t video_file;
  iso9660_file_t audio_file;
  FIL out;
  int gun_bits;
  uint32_t source_crc;     // of the clip as the pictures are read
  uint32_t sound_samples;  // decoded and not yet in a record
  bool sound_end;
  uint32_t frame;          // records written
  uint32_t pictures;
  bool started;
  int result;              // 1 running, 0 done, negative an error
  uint32_t out_len;        // bytes in out_buf
  uint8_t out_buf[CONVJOB_OUT_BYTES];
  picture16_profile_t profile;
  convjob_times_t times;
} convjob_t;

// Starts converting the clip `entry` of `iso` into `out_path`, for a
// palette of `gun_bits` (3: an ST, 4: an STE). `run2` puts work on the
// second core (NULL: one core),
// `cycles` (or NULL) is a free-running cycle counter for the passes'
// profile. Returns 0 or a negative error.
int convjob_start(convjob_t *j, const convjob_memory_t *memory,
                  iso9660_t *iso, const iso9660_entry_t *entry,
                  const char *out_path, int gun_bits,
                  picture16_run2_fn run2,
                  uint32_t (*cycles)(void));

// Converts the next picture and writes its records. Returns 1 while the
// clip goes on, 0 when its file is complete, or a negative error (the
// files are closed then).
int convjob_step(convjob_t *j);

// Closes the file of a job not finished.
void convjob_abort(convjob_t *j);

#endif  // CONVJOB_H
