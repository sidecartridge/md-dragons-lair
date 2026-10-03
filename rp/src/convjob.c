/**
 * File: convjob.c
 * Description: A clip converted on the cartridge into the app's clip file.
 *              See convjob.h.
 */

#include "convjob.h"

#include <string.h>

#include "crc32.h"
#include "pico/time.h"

// --- Reading the clip ---------------------------------------------------------

static int video_read(void *ctx, uint8_t *buf, uint32_t len) {
  convjob_t *j = (convjob_t *)ctx;
  UINT got = 0;
  uint32_t t0 = time_us_32();
  int r = iso9660_fread(&j->video_file, buf, len, &got);
  j->times.read += time_us_32() - t0;
  if (r != ISO9660_OK) {
    return -1;
  }
  j->source_crc = crc32_update(j->source_crc, buf, got);
  return (int)got;
}

static int audio_read(void *ctx, uint8_t *buf, uint32_t len) {
  convjob_t *j = (convjob_t *)ctx;
  UINT got = 0;
  uint32_t t0 = time_us_32();
  int r = iso9660_fread(&j->audio_file, buf, len, &got);
  j->times.read += time_us_32() - t0;
  return (r == ISO9660_OK) ? (int)got : -1;
}

// --- The sound ------------------------------------------------------------------

// The next frame's sound: decoded as far as needed; the samples there are,
// the rest silent.
static const int8_t *frame_sound(convjob_t *j) {
  uint32_t t0 = time_us_32();
  uint64_t read0 = j->times.read;
  while (!j->sound_end && j->sound_samples < CLIP_SAMPLES) {
    int n = mp2_decode_frame(j->m.mp2, j->m.pcm);
    if (n <= 0) {
      j->sound_end = true;
      break;
    }
    mp2_to_pcm8(j->m.pcm, j->m.sound + j->sound_samples, (uint32_t)n,
                MP2_GAIN_GAME);
    j->sound_samples += (uint32_t)n;
  }
  if (j->sound_samples < CLIP_SAMPLES) {
    memset(j->m.sound + j->sound_samples, 0,
           CLIP_SAMPLES - j->sound_samples);
  }
  j->times.sound += (time_us_32() - t0) - (j->times.read - read0);
  return j->m.sound;
}

// A record took the frame's sound.
static void sound_taken(convjob_t *j) {
  if (j->sound_samples > CLIP_SAMPLES) {
    memmove(j->m.sound, j->m.sound + CLIP_SAMPLES,
            j->sound_samples - CLIP_SAMPLES);
    j->sound_samples -= CLIP_SAMPLES;
  } else {
    j->sound_samples = 0;
  }
  j->frame++;
}

// --- The clip file --------------------------------------------------------------

// The bytes gathered, written: a whole piece at an offset of its size but
// for the file's end.
static int out_flush(convjob_t *j) {
  UINT done = 0;
  uint32_t t0 = time_us_32();
  FRESULT fr = f_write(&j->out, j->out_buf, j->out_len, &done);
  j->times.write += time_us_32() - t0;
  bool ok = fr == FR_OK && done == j->out_len;
  j->out_len = 0;
  return ok ? 0 : -1;
}

static int out_write(void *ctx, const void *data, uint32_t len) {
  convjob_t *j = (convjob_t *)ctx;
  const uint8_t *p = (const uint8_t *)data;
  while (len > 0) {
    uint32_t n = CONVJOB_OUT_BYTES - j->out_len;
    n = n < len ? n : len;
    memcpy(j->out_buf + j->out_len, p, n);
    j->out_len += n;
    p += n;
    len -= n;
    if (j->out_len == CONVJOB_OUT_BYTES && out_flush(j) < 0) {
      return -1;
    }
  }
  return 0;
}

// Called once, after the index: the file's end first.
static int out_header(void *ctx, const uint8_t header[CLIP_HEADER_BYTES]) {
  convjob_t *j = (convjob_t *)ctx;
  if (out_flush(j) < 0) {
    return -1;
  }
  FSIZE_t end = f_tell(&j->out);
  UINT done = 0;
  bool ok = f_lseek(&j->out, 0) == FR_OK &&
            f_write(&j->out, header, CLIP_HEADER_BYTES, &done) == FR_OK &&
            done == CLIP_HEADER_BYTES && f_lseek(&j->out, end) == FR_OK;
  return ok ? 0 : -1;
}

// --- The converter's pictures ---------------------------------------------------

static void job_begin(void *ctx, const convert_picture_t *picture) {
  convjob_t *j = (convjob_t *)ctx;
  clip_writer_picture(j->m.writer, picture->frames, picture->palette->rgb444,
                      false, frame_sound(j));
  sound_taken(j);
}

// Two rows at a time, each stored alone.
static void job_lines(void *ctx, int line, const uint8_t *indices,
                      int width) {
  convjob_t *j = (convjob_t *)ctx;
  (void)line;
  for (int r = 0; r < 2; r++) {
    clip_writer_row(j->m.writer, indices + r * width, NULL);
  }
}

static void job_end(void *ctx, const convert_picture_t *picture) {
  convjob_t *j = (convjob_t *)ctx;
  for (uint32_t f = 1; f < picture->frames; f++) {
    clip_writer_held(j->m.writer, frame_sound(j));
    sound_taken(j);
  }
  j->pictures++;
}

// --- The job --------------------------------------------------------------------

int convjob_start(convjob_t *j, const convjob_memory_t *memory,
                  iso9660_t *iso, const iso9660_entry_t *entry,
                  const char *out_path, int gun_bits,
                  picture16_run2_fn run2,
                  uint32_t (*cycles)(void)) {
  memset(j, 0, sizeof(*j));
  j->m = *memory;
  j->gun_bits = gun_bits;
  if (iso9660_fopen_entry(iso, &j->video_file, entry) != ISO9660_OK ||
      iso9660_fopen_entry(iso, &j->audio_file, entry) != ISO9660_OK) {
    j->result = CONVJOB_ERR_OPEN;
    return j->result;
  }
  if (f_open(&j->out, out_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    j->result = CONVJOB_ERR_OPEN;
    return j->result;
  }
  mpeg_ps_init(j->m.video, video_read, j);
  mpeg_ps_init_stream(j->m.audio, audio_read, j, MPEG_PS_AUDIO);
  mp2_init(j->m.mp2, j->m.audio);
  j->m.dec->run2 = run2;
  j->m.dec->cycles = NULL;
  mpeg1_init(j->m.dec, j->m.video);
  mpeg1_set_slots(j->m.dec, j->m.rows, j->m.row_count);

  clip_io_t io = {out_write, out_header, j};
  clip_header_t h;
  memset(&h, 0, sizeof(h));
  h.gun_bits = (uint8_t)gun_bits;
  h.converter = CONVERT_VERSION;
  h.keep_percent = CONVERT_KEEP_PERCENT;
  h.source_bytes = entry->size;
  clip_writer_begin(j->m.writer, &io, &h, true);

  picture16_options_t options = {gun_bits, PICTURE16_WEIGHT_SQRT,
                                 PICTURE16_DITHER_MIX, run2, NULL};
  convert_out_t out = {job_begin, job_lines, job_end, j};
  convert_init(&j->conv, j->m.dec, j->m.ring_c, j->m.lines_y, &options,
               &out);
  j->conv.keep_percent = CONVERT_KEEP_PERCENT;
  j->profile.cycles = cycles;
  j->conv.profile = (cycles != NULL) ? &j->profile : NULL;
  j->result = j->m.writer->error != 0 ? j->m.writer->error : 1;
  if (j->result != 1) {
    f_close(&j->out);
  }
  return j->result < 0 ? j->result : 0;
}

// The clip's bytes the pictures did not read, for its CRC-32.
static void read_to_end(convjob_t *j) {
  uint8_t *buf = (uint8_t *)j->m.pcm;
  uint32_t len = MP2_FRAME_SAMPLES * sizeof(int16_t);
  while (video_read(j, buf, len) > 0) {
  }
}

int convjob_step(convjob_t *j) {
  if (j->result != 1) {
    return j->result;
  }
  uint32_t t0 = time_us_32();
  int r = convert_next(&j->conv);
  if (j->conv.profile != NULL) {
    j->times.histogram_cycles += j->profile.histogram;
    j->times.palette_cycles += j->profile.palette + j->profile.table;
    j->times.dither_cycles += j->profile.dither;
  }
  if (r < 0) {
    j->result = r;
    f_close(&j->out);
  } else if (r == 0) {
    read_to_end(j);
    j->m.writer->header.source_crc = j->source_crc;
    int err = clip_writer_finish(j->m.writer);
    if (err == 0 && j->m.writer->header.frames != convert_length(&j->conv)) {
      err = -1;  // the records and the clip's frames disagree
    }
    if (f_close(&j->out) != FR_OK && err == 0) {
      err = -1;
    }
    j->result = err;
  }
  j->times.total += time_us_32() - t0;
  return j->result;
}

void convjob_abort(convjob_t *j) {
  if (j->result == 1) {
    f_close(&j->out);
    j->result = -1;
  }
}
