/**
 * File: player.c
 * Description: A clip file played on the ST with its sound. See player.h.
 */

#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "audio.h"
#include "clipplay.h"
#include "debug.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "mp2_audio.h"
#include "palette.h"
#include "picture16.h"
#include "pico/time.h"
#include "settings.h"

extern const struct FB_FONT font8x8;

#define PLAY_RING 4096u  // the sound read ahead, a power of two
// Silence after the clip's sound, so that its last samples fill the
// audio's last unit and are heard, and the writer, which writes ahead of
// the ST (74 ms on the DMA path), finds samples to the end.
#define PLAY_TAIL 2048u

// The volume, in 3 dB steps from -18 to +18 dB, kept in the app's settings
// for each output (saved when the clip stops: writing the flash holds the
// RP). 0 dB plays the samples as the conversion leveled them; quieter
// scales them down; louder goes through MP2's soft limiter, as the
// conversion's level did (mp2_to_pcm8()).
static const int play_gains[] = {32,  45,  64,  91,   128,  181, 256,
                                 362, 512, 724, 1024, 1448, 2048};
#define PLAY_GAINS ((int)(sizeof(play_gains) / sizeof(play_gains[0])))
#define PLAY_GAIN_0DB 6
#define PLAY_GAIN_PIECE 98  // samples scaled at a time (882 = 9 x 98)
#define PLAY_OSD_US 1500000u  // a volume change shown on the pictures
// Samples between a picture's commit and its showing: 1.5 frames.
#define PLAY_SCREEN_DELAY (CLIP_SAMPLES * 3u / 2u)
// The sound starts once this much is read (120 ms): the writer, which writes
// ahead of the ST (74 ms on the DMA path), then finds it. Started with the
// first record alone (40 ms), it held the last sample in the clip's first
// frames: a hole in the sound.
#define PLAY_PRIME (3u * CLIP_SAMPLES)
// A picture this late, against its due time, is counted (and logged).
#define PLAY_LATE_US 40000
// A record's reading this slow is counted.
#define PLAY_SLOW_READ_US 10000u

typedef struct {
  FIL f;
  clipplay_t p;
  uint8_t buf[CLIPPLAY_BUFFER_BYTES];
  int8_t ring[PLAY_RING];
} play_mem_t;

static struct {
  bool active;
  bool ended;      // the reader has read the last record
  uint32_t sound_end;  // the clip's samples, once ended
  char path[40];
  play_mem_t *m;   // on the heap while it plays
  uint32_t called;  // player_start()'s time
  uint32_t t0;
  uint32_t read_us;  // of the record being read
  uint64_t sum[3];
  uint32_t last_us[3];  // the last record's reading and decoding, the last
                        // picture's publishing
  volatile uint32_t ring_in;   // samples pushed
  volatile uint32_t ring_out;  // samples taken by the audio
  uint32_t first_frame;  // the clip's frame the sound starts at
  bool first;      // no picture read yet: the first record's is shown
                   // whatever its kind (held, after a start at a frame)
  bool next;       // fb_chunked_buffer holds the picture of next_frame
  uint32_t next_frame;
  uint16_t next_palette[16];
  bool ready;      // a converted picture waits, for ready_frame
  uint32_t ready_frame;
  uint32_t shown_frame;  // the last picture committed
  bool heard;      // the clip's sound has begun
  bool paused;
  bool priming;    // the sound held back until PLAY_PRIME samples are read
  uint32_t pause_underruns, pause_late;  // the counters when it paused
  uint32_t underruns0, late0;
  int gain;          // play_gains' index
  bool gain_changed; // to be saved
  uint32_t osd_until;  // the volume shown on the pictures until then
  bool osd_up;
  uint8_t next_bright;  // next picture's brightest entry, for the text
  player_overlay_fn overlay;
  // Where the sound's underruns and the late pictures fall.
  uint32_t last_pass;   // the previous pass of player_frame()
  uint32_t last_heard;
  uint32_t seen_underruns;
  uint32_t due_at;      // when the picture waiting became due (0: not yet)
  bool ack_waited;      // and the ST had not copied the one before
} s_play;

__attribute__((used)) play_results_t playResults;
extern uint32_t audioUnderruns, audioLateSlices;

static int play_read(void *ctx, uint8_t *buf, uint32_t len) {
  UINT got = 0;
  uint32_t t0 = time_us_32();
  FRESULT fr = f_read((FIL *)ctx, buf, len, &got);
  s_play.read_us += time_us_32() - t0;
  return fr == FR_OK ? (int)got : -1;
}

static int play_seek(void *ctx, uint32_t offset) {
  return f_lseek((FIL *)ctx, offset) == FR_OK ? 0 : -1;
}

// The audio's stream: the ring's samples (taken by audio_render_frame(), on
// the main loop: never during a push); none while paused or priming.
static uint32_t play_avail(void) {
  return (s_play.paused || s_play.priming) ? 0u
                                           : s_play.ring_in - s_play.ring_out;
}

static void play_take(int8_t *buf, uint32_t n) {
  uint32_t out = s_play.ring_out;
  for (uint32_t i = 0; i < n; i++) {
    buf[i] = (out + i != s_play.ring_in)
                 ? s_play.m->ring[(out + i) & (PLAY_RING - 1u)]
                 : 0;
  }
  s_play.ring_out = out + n;
}

static void play_stat(uint32_t stat[3], uint64_t *sum, uint32_t count,
                      uint32_t us) {
  if (count == 1 || us < stat[0]) {
    stat[0] = us;
  }
  if (us > stat[2]) {
    stat[2] = us;
  }
  *sum += us;
  stat[1] = (uint32_t)(*sum / count);
}

static const char *play_volume_key(void) {
  return audio_uses_dma() ? ACONFIG_PARAM_VOLUME_DMA : ACONFIG_PARAM_VOLUME_YM;
}

// The sound's counters, up to now (its last sample, or a stop; while
// paused, up to the pause: the output held its last sample since).
static void play_sound_counters(void) {
  bool held = s_play.paused || s_play.priming;
  uint32_t underruns = held ? s_play.pause_underruns : audioUnderruns;
  uint32_t late = held ? s_play.pause_late : audioLateSlices;
  playResults.underruns = underruns - s_play.underruns0;
  playResults.late_slices = late - s_play.late0;
}

void player_close(int result) {
  if (!s_play.active) {
    return;
  }
  if (result != 0) {
    play_sound_counters();  // at the end they were taken at its last sample
  }
  audio_set_fill_callback(NULL);  // the stream's ring is about to go
  playResults.volume_db = 3 * (s_play.gain - PLAY_GAIN_0DB);
  if (s_play.gain_changed) {
    settings_put_integer(aconfig_getContext(), play_volume_key(),
                         playResults.volume_db);
    settings_save(aconfig_getContext(), true);
    s_play.gain_changed = false;
  }
  playResults.result = result;
  playResults.total_ms = (time_us_32() - s_play.t0) / 1000u;
  if (s_play.m != NULL) {
    playResults.pieces = s_play.m->p.pieces;
    f_close(&s_play.m->f);
    free(s_play.m);
    s_play.m = NULL;
  }
  s_play.active = false;
  DPRINTF("Play %s: result %d, %lu frames, %lu shown, %lu dropped, drift "
          "%ld..%ld us, %lu late pictures, %lu underruns, %lu late slices, "
          "start %lu ms; read %lu/%lu/%lu us (%lu slow), decode %lu/%lu/%lu, "
          "publish %lu/%lu/%lu\n",
          s_play.path, result, (unsigned long)playResults.frames,
          (unsigned long)playResults.pictures,
          (unsigned long)playResults.dropped, (long)playResults.drift_us[0],
          (long)playResults.drift_us[1], (unsigned long)playResults.late,
          (unsigned long)playResults.underruns,
          (unsigned long)playResults.late_slices,
          (unsigned long)playResults.start_ms,
          (unsigned long)playResults.read_us[0],
          (unsigned long)playResults.read_us[1],
          (unsigned long)playResults.read_us[2],
          (unsigned long)playResults.slow_reads,
          (unsigned long)playResults.decode_us[0],
          (unsigned long)playResults.decode_us[1],
          (unsigned long)playResults.decode_us[2],
          (unsigned long)playResults.publish_us[0],
          (unsigned long)playResults.publish_us[1],
          (unsigned long)playResults.publish_us[2]);
}

// Reads records while the ring has room for one more's sound and the
// picture buffer is free: their samples into the ring, a picture decoded
// into fb_chunked_buffer (then it waits as `next`). Returns 0, or a
// negative error.
static int play_read_ahead(void) {
  play_results_t *r = &playResults;
  while (!s_play.ended && !s_play.next &&
         PLAY_RING - (s_play.ring_in - s_play.ring_out) >= CLIP_SAMPLES) {
    s_play.read_us = 0;
    uint32_t t0 = time_us_32();
    clipplay_frame_t f;
    uint32_t frame = s_play.m->p.frame;
    int got = clipplay_next(&s_play.m->p, fb_chunked_buffer, &f);
    uint32_t us = time_us_32() - t0;
    if (got < 0) {
      return got;
    }
    if (got == 0) {
      s_play.ended = true;
      s_play.sound_end = s_play.ring_in;
      break;
    }
    r->frames++;
    s_play.last_us[0] = s_play.read_us;
    s_play.last_us[1] = us - s_play.read_us;
    play_stat(r->read_us, &s_play.sum[0], r->frames, s_play.last_us[0]);
    if (s_play.last_us[0] > PLAY_SLOW_READ_US) {
      r->slow_reads++;
    }
    play_stat(r->decode_us, &s_play.sum[1], r->frames, s_play.last_us[1]);
    uint32_t in = s_play.ring_in;
    int gain = play_gains[s_play.gain];
    for (uint32_t i = 0; i < CLIP_SAMPLES; i += PLAY_GAIN_PIECE) {
      int8_t piece[PLAY_GAIN_PIECE];
      if (gain > 256) {
        int16_t wide[PLAY_GAIN_PIECE];
        for (int k = 0; k < PLAY_GAIN_PIECE; k++) {
          wide[k] = (int16_t)(f.sound[i + k] * 256);
        }
        mp2_to_pcm8(wide, piece, PLAY_GAIN_PIECE, gain);
      } else {
        for (int k = 0; k < PLAY_GAIN_PIECE; k++) {
          piece[k] = (int8_t)((f.sound[i + k] * gain) >> 8);
        }
      }
      for (int k = 0; k < PLAY_GAIN_PIECE; k++) {
        s_play.m->ring[(in + i + k) & (PLAY_RING - 1u)] = piece[k];
      }
    }
    s_play.ring_in = in + CLIP_SAMPLES;
    if (f.kind != CLIP_HELD || s_play.first) {
      int bright = 0;
      int bright_luma = -1;
      for (int e = 0; e < 16; e++) {
        uint16_t c = f.palette[e];
        int luma = 2 * ((c >> 8) & 15) + 5 * ((c >> 4) & 15) + (c & 15);
        if (luma > bright_luma) {
          bright = e;
          bright_luma = luma;
        }
        s_play.next_palette[e] = picture16_ste_word(c);
      }
      s_play.next_bright = (uint8_t)bright;
      s_play.next = true;
      s_play.next_frame = frame;
      s_play.first = false;
    }
  }
  return 0;
}

int player_start(const char *path, uint32_t frame, bool paused) {
  if (s_play.active) {
    player_close(PLAYER_STOPPED);
  }
  player_overlay_fn overlay = s_play.overlay;
  memset(&s_play, 0, sizeof(s_play));
  memset(&playResults, 0, sizeof(playResults));
  s_play.overlay = overlay;
  s_play.called = time_us_32();
  s_play.t0 = s_play.called;
  s_play.paused = paused;
  snprintf(s_play.path, sizeof(s_play.path), "%s", path);
  s_play.m = malloc(sizeof(play_mem_t));
  if (s_play.m == NULL) {
    playResults.result = -100;
    return -100;
  }
  if (f_open(&s_play.m->f, path, FA_READ) != FR_OK) {
    free(s_play.m);
    s_play.m = NULL;
    playResults.result = CLIPPLAY_ERR_IO;
    return CLIPPLAY_ERR_IO;
  }
  s_play.active = true;
  SettingsConfigEntry *volume =
      settings_find_entry(aconfig_getContext(), play_volume_key());
  int db = volume != NULL ? atoi(volume->value) : 0;
  s_play.gain = PLAY_GAIN_0DB + db / 3;
  s_play.gain = s_play.gain < 0                ? 0
                : s_play.gain >= PLAY_GAINS ? PLAY_GAINS - 1
                                            : s_play.gain;
  clipplay_io_t io = {play_read, play_seek, &s_play.m->f};
  clipplay_t *p = &s_play.m->p;
  int r = clipplay_open(p, &io, s_play.m->buf);
  playResults.open_us = time_us_32() - s_play.called;
  uint32_t seek_t0 = time_us_32();
  // From a frame: the key before it, then the records up to it decoded.
  if (r == 0 && frame > 0) {
    if (frame >= p->header.frames) {
      frame = p->header.frames - 1u;
    }
    int key = clipplay_start(p, frame);
    r = key < 0 ? key : 0;
    clipplay_frame_t f;
    while (r == 0 && p->frame < frame) {
      int got = clipplay_next(p, fb_chunked_buffer, &f);
      r = got < 0 ? got : (got == 0 ? CLIPPLAY_ERR_RECORD : 0);
    }
  }
  playResults.seek_us = time_us_32() - seek_t0;
  s_play.first_frame = p->frame;
  s_play.first = true;
  if (r == 0) {
    r = play_read_ahead();  // the first picture and its sound
  }
  if (r < 0) {
    player_close(r);
    return r;
  }
  DPRINTF("Play %s from frame %lu: %lu frames; opened in %lu us, at the "
          "frame in %lu us\n",
          s_play.path, (unsigned long)s_play.first_frame,
          (unsigned long)p->header.frames, (unsigned long)playResults.open_us,
          (unsigned long)playResults.seek_us);
  s_play.underruns0 = audioUnderruns;
  s_play.late0 = audioLateSlices;
  s_play.pause_underruns = audioUnderruns;
  s_play.pause_late = audioLateSlices;
  s_play.seen_underruns = audioUnderruns;
  playResults.drift_us[0] = INT32_MAX;
  playResults.drift_us[1] = INT32_MIN;
  playResults.lead_ms = INT32_MAX;
  s_play.t0 = time_us_32();
  s_play.priming = true;
  audio_set_pcm_stream(play_take, play_avail, CLIP_SAMPLE_RATE);
  return 0;
}

bool player_active(void) { return s_play.active; }

void player_pause(bool paused) {
  if (!s_play.active || paused == s_play.paused) {
    return;
  }
  if (paused) {
    s_play.pause_underruns = audioUnderruns;
    s_play.pause_late = audioLateSlices;
  } else {
    // The output held its last sample while paused: not an underrun.
    s_play.underruns0 += audioUnderruns - s_play.pause_underruns;
    s_play.late0 += audioLateSlices - s_play.pause_late;
  }
  s_play.paused = paused;
}

bool player_paused(void) { return s_play.paused; }

uint32_t player_heard_ms(void) {
  if (!s_play.active) {
    return 0;
  }
  uint32_t heard = audio_source_played();
  return (uint32_t)((uint64_t)heard * 1000u / CLIP_SAMPLE_RATE);
}

uint32_t player_frame_shown(void) {
  return playResults.pictures > 0 ? s_play.shown_frame : s_play.first_frame;
}

uint32_t player_first_frame(void) { return s_play.first_frame; }

uint32_t player_frames(void) {
  return s_play.m != NULL ? s_play.m->p.header.frames : 0u;
}

void player_last_times(uint32_t us[3]) {
  memcpy(us, s_play.last_us, sizeof(s_play.last_us));
}

uint32_t player_card_kbs(void) {
  return s_play.sum[0] > 0 && s_play.m != NULL
             ? (uint32_t)((uint64_t)s_play.m->p.pieces * CLIPPLAY_PIECE *
                          1000000u / 1024u / s_play.sum[0])
             : 0u;
}

void player_volume(int step) {
  if (s_play.active) {
    int gain = s_play.gain + (step > 0 ? 1 : -1);
    if (gain >= 0 && gain < PLAY_GAINS) {
      s_play.gain = gain;
      s_play.gain_changed = true;
    }
  }
  s_play.osd_until = time_us_32() + PLAY_OSD_US;
  s_play.osd_up = step > 0;
}

void player_set_overlay(player_overlay_fn fn) { s_play.overlay = fn; }

int player_frame(void) {
  if (!s_play.active) {
    return 0;
  }
  play_results_t *r = &playResults;
  uint32_t heard = audio_source_played();
  uint32_t now = time_us_32();
  uint32_t pass_us = now - s_play.last_pass;
  uint32_t heard_step = heard - s_play.last_heard;
  s_play.last_pass = now;
  s_play.last_heard = heard;
  // An underrun while it plays: where it falls, and what the reader had.
  if (audioUnderruns != s_play.seen_underruns) {
    if (!s_play.paused && !s_play.priming) {
      DPRINTF("Underrun in %s at frame %lu%s: %ld samples ahead of the ear, "
              "%lu in the ring; last read %lu us, decode %lu us; pass %lu "
              "us\n",
              s_play.path,
              (unsigned long)(s_play.first_frame + heard / CLIP_SAMPLES),
              s_play.heard ? "" : " (before its first sound)",
              (long)(int32_t)(s_play.ring_in - heard),
              (unsigned long)(s_play.ring_in - s_play.ring_out),
              (unsigned long)s_play.last_us[0],
              (unsigned long)s_play.last_us[1], (unsigned long)pass_us);
    }
    s_play.seen_underruns = audioUnderruns;
  }
  if (heard > 0 && !s_play.heard) {
    s_play.heard = true;
    r->start_ms = (now - s_play.called) / 1000u;
  }
  if (r->pictures > 0 && !s_play.ended) {
    int32_t lead = (int32_t)((int64_t)(int32_t)(s_play.ring_in - heard) * 1000 /
                             CLIP_SAMPLE_RATE);
    r->lead_ms = lead < r->lead_ms ? lead : r->lead_ms;
  }
  // The converted picture: on the ST when the sound reaches its frame less
  // the screen's delay (the first at once, while the sound is held back);
  // dropped when the sound is past its frame's end.
  if (s_play.ready &&
      (heard > 0 || ((s_play.paused || s_play.priming) && r->pictures == 0))) {
    uint32_t due = (s_play.ready_frame - s_play.first_frame) * CLIP_SAMPLES;
    if (heard + PLAY_SCREEN_DELAY >= due) {
      if (s_play.due_at == 0) {
        s_play.due_at = now;
      }
      if (!fb_publish_ready()) {
        s_play.ack_waited = true;
      }
    }
    if (heard >= due + CLIP_SAMPLES + PLAY_SCREEN_DELAY) {
      s_play.ready = false;
      r->dropped++;
    } else if (heard + PLAY_SCREEN_DELAY >= due && fb_publish_ready()) {
      // Only once the ST has copied the picture before: waiting for it here
      // would stop the sound's top-up (an ST slowed by the mouse).
      uint32_t t0 = time_us_32();
      fb_publish_commit();
      s_play.last_us[2] = time_us_32() - t0;
      play_stat(r->publish_us, &s_play.sum[2], ++r->pictures,
                s_play.last_us[2]);
      int32_t drift = (int32_t)((int64_t)((int32_t)(heard + PLAY_SCREEN_DELAY -
                                                     due)) *
                                1000000 / CLIP_SAMPLE_RATE);
      if (s_play.ready_frame > s_play.first_frame && drift < r->drift_us[0]) {
        r->drift_us[0] = drift;
      }
      if (s_play.ready_frame > s_play.first_frame && drift > r->drift_us[1]) {
        r->drift_us[1] = drift;
      }
      if (s_play.ready_frame > s_play.first_frame && drift > PLAY_LATE_US) {
        r->late++;
        DPRINTF("Late picture in %s, frame %lu: %ld us; due %lu us before, "
                "%s; pass %lu us, the sound +%lu samples in it\n",
                s_play.path, (unsigned long)s_play.ready_frame, (long)drift,
                (unsigned long)(now - s_play.due_at),
                s_play.ack_waited ? "the ST still copying the one before"
                                  : "the ST ready",
                (unsigned long)pass_us, (unsigned long)heard_step);
      }
      s_play.shown_frame = s_play.ready_frame;
      s_play.ready = false;
    }
  }
  // The decoded picture converted (with the overlay or the volume over it:
  // in the palette's black, entry 0, and its brightest), then the reader
  // on.
  if (!s_play.ready && s_play.next) {
    if (s_play.overlay != NULL) {
      s_play.overlay(s_play.next_frame, s_play.next_bright);
    }
    if ((int32_t)(s_play.osd_until - time_us_32()) > 0) {
      char line[24];
      snprintf(line, sizeof(line), "VOLUME %s %+d DB",
               s_play.osd_up ? "UP" : "DOWN",
               3 * (s_play.gain - PLAY_GAIN_0DB));
      fb_fill_rect(8, 182, 8 * (int)strlen(line) + 8, 12, 0);
      font_set_font(&font8x8);
      font_set_color(s_play.next_bright);
      font_move(12, 184);
      font_print(line);
    }
    palette_set_frame(s_play.next_palette);
    fb_publish_prepare();
    s_play.due_at = 0;
    s_play.ack_waited = false;
    s_play.ready = true;
    s_play.ready_frame = s_play.next_frame;
    s_play.next = false;
  }
  int err = play_read_ahead();
  if (err < 0) {
    return err;
  }
  // The sound starts once PLAY_PRIME samples are read, or all of a shorter
  // clip; the last sample held meanwhile is no underrun, up to the FIFO's
  // first top-up.
  if (s_play.priming &&
      (s_play.ring_in - s_play.ring_out >= PLAY_PRIME || s_play.ended)) {
    s_play.priming = false;
    audio_render_frame();
    s_play.underruns0 = s_play.pause_underruns = audioUnderruns;
    s_play.late0 = s_play.pause_late = audioLateSlices;
    s_play.seen_underruns = audioUnderruns;
  }
  // The end: everything read and shown, the clip's sound heard; silence
  // after it, as the ring has room.
  if (s_play.ended) {
    while (s_play.ring_in - s_play.sound_end < PLAY_TAIL &&
           s_play.ring_in - s_play.ring_out < PLAY_RING) {
      s_play.m->ring[s_play.ring_in & (PLAY_RING - 1u)] = 0;
      s_play.ring_in++;
    }
    if (!s_play.ready && !s_play.next && heard >= s_play.sound_end) {
      play_sound_counters();
      return 0;
    }
  }
  return 1;
}
