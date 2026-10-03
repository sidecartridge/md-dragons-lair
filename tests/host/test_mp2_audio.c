// units: rp/src/mp2_audio.c rp/src/mpeg_ps.c
/* The MP2 decoder on synthetic streams (tests/host/data, made with
 * ffmpeg: see data/README.md), and its 8-bit step.
 *
 * - tone_stereo_192k.mpg / tone_stereo_128k.mpg: 1 kHz on the left, 3 kHz on
 *   the right, 0.5 s at 44,100 Hz (allocation tables 3-B.2b and 3-B.2a).
 *   The decoder's mono at 22,050 Hz against another decoder's reading of
 *   the same stream (ffmpeg's float decoder, mixed and resampled: the
 *   .ref.s16 files): in step, at the same level, 50 dB apart at least.
 * - tone_mono_15k.mpg: a mono 15 kHz tone, above the output's band (11,025
 *   Hz): next to nothing comes out.
 * - The same stream after a run of garbage decodes to the same samples; a
 *   stream cut in the middle of a frame ends cleanly.
 * - mp2_to_pcm8(): plain rounding below the limiter's knee, never past
 *   8 bits, never decreasing, at every gain. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp2_audio.h"
#include "test.h"

#define MAX_SAMPLES 40000

typedef struct {
  FILE *f;
  uint32_t garbage;  // bytes of garbage handed out before the file
  uint32_t limit;    // bytes of the file handed out at most
  uint32_t given;
} source_t;

static int source_read(void *ctx, uint8_t *buf, uint32_t len) {
  source_t *s = (source_t *)ctx;
  uint32_t n = 0;
  while (n < len && s->garbage > 0) {
    buf[n++] = (uint8_t)(0x5A ^ (s->garbage * 37u));
    s->garbage--;
  }
  if (n < len && s->given < s->limit) {
    uint32_t want = len - n;
    if (want > s->limit - s->given) {
      want = s->limit - s->given;
    }
    size_t got = fread(buf + n, 1, want, s->f);
    s->given += (uint32_t)got;
    n += (uint32_t)got;
  }
  return (int)n;
}

static mpeg_ps_t ps;
static mp2_t mp2;
static int16_t pcm[MAX_SAMPLES];

// Decodes `path` (after `garbage` bytes, at most `limit` bytes of it) into
// pcm; returns the samples, or -1 when the file is missing.
static int decode(const char *path, uint32_t garbage, uint32_t limit) {
  source_t src = {fopen(path, "rb"), garbage, limit, 0};
  if (src.f == NULL) {
    fprintf(stderr, "%s: missing\n", path);
    return -1;
  }
  mpeg_ps_init_stream(&ps, source_read, &src, MPEG_PS_AUDIO);
  mp2_init(&mp2, &ps);
  int total = 0;
  int n;
  while (total + (int)MP2_FRAME_SAMPLES <= MAX_SAMPLES &&
         (n = mp2_decode_frame(&mp2, pcm + total)) > 0) {
    total += n;
  }
  fclose(src.f);
  return total;
}

static int16_t ref[MAX_SAMPLES];

// Reads a reference (16-bit little-endian, as the RP and the PC store it);
// returns its samples.
static int load_ref(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "%s: missing\n", path);
    return -1;
  }
  int n = (int)fread(ref, sizeof(int16_t), MAX_SAMPLES, f);
  fclose(f);
  return n;
}

static void check_stereo(const char *stream, const char *reference) {
  int n = decode(stream, 0, UINT32_MAX);
  CHECK_EQ(n, 20 * (int)MP2_FRAME_SAMPLES);
  CHECK_EQ(mp2.stats.frames, 20);
  CHECK_EQ(mp2.stats.mode_frames[0], 20);  // plain stereo
  CHECK_EQ(mp2_output_rate(&mp2), 22050);
  CHECK_EQ(load_ref(reference), n);
  if (n <= 0) {
    return;
  }
  // Past the filters' start, before the end.
  double sig = 0, err = 0;
  for (int i = 1000; i < n - 1000; i++) {
    double d = (double)ref[i] - pcm[i];
    sig += (double)ref[i] * ref[i];
    err += d * d;
  }
  double snr = 10 * log10(sig / err);
  printf("%s: %.1f dB from the reference\n", stream, snr);
  CHECK(snr > 50.0);
}

static void check_above_band(void) {
  int n = decode("data/tone_mono_15k.mpg", 0, UINT32_MAX);
  CHECK_EQ(n, 20 * (int)MP2_FRAME_SAMPLES);
  CHECK_EQ(mp2.stats.mode_frames[3], 20);  // mono
  if (n <= 0) {
    return;
  }
  double sum = 0;
  for (int i = 2000; i < n - 1500; i++) {
    sum += (double)pcm[i] * pcm[i];
  }
  // The tone is at half of full scale; out of the band it is gone.
  double db = 20 * log10(sqrt(sum / (n - 3500)) / 16384.0);
  printf("15 kHz tone: %.1f dB of its level comes out\n", db);
  CHECK(db < -40.0);
}

static void check_damaged(void) {
  static int16_t clean[MAX_SAMPLES];
  const char *path = "data/tone_stereo_192k.mpg";
  int n = decode(path, 0, UINT32_MAX);
  if (n <= 0) {
    CHECK(n > 0);
    return;
  }
  memcpy(clean, pcm, sizeof(int16_t) * (size_t)n);
  // Garbage first: the demultiplexer and the frame sync skip it.
  CHECK_EQ(decode(path, 3000, UINT32_MAX), n);
  CHECK(memcmp(clean, pcm, sizeof(int16_t) * (size_t)n) == 0);
  // Cut in the middle of the file: the frames before the cut, then the end.
  int cut = decode(path, 0, 7000);
  CHECK(cut > 0 && cut < n);
  CHECK(memcmp(clean, pcm, sizeof(int16_t) * (size_t)cut) == 0);
}

static void check_pcm8(void) {
  static const int gains[] = {256, 362, 512, 724, 1024, 1448, 2048, 2896};
  // Unity gain below the knee: rounding only.
  for (int x = -16384; x <= 16384; x++) {
    int16_t in = (int16_t)x;
    int8_t out;
    mp2_to_pcm8(&in, &out, 1, MP2_GAIN_UNITY);
    int want = (x + 128) >> 8;
    want = want > 127 ? 127 : want;
    if (out != want) {
      CHECK_EQ(out, want);
      break;
    }
  }
  // Every gain: within 8 bits, never decreasing, odd (as fair below zero as
  // above, to one step of rounding).
  for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
    int prev = -129;
    bool ok = true;
    for (int x = -32768; x <= 32767; x++) {
      int16_t in = (int16_t)x;
      int8_t out;
      mp2_to_pcm8(&in, &out, 1, gains[g]);
      ok &= out >= prev;
      prev = out;
      if (x > -32768) {
        int16_t neg = (int16_t)-x;
        int8_t mirror;
        mp2_to_pcm8(&neg, &mirror, 1, gains[g]);
        ok &= abs(out + mirror) <= 1;
      }
    }
    CHECK(ok);
  }
}

int main(void) {
  check_stereo("data/tone_stereo_192k.mpg", "data/tone_stereo_192k.ref.s16");
  check_stereo("data/tone_stereo_128k.mpg", "data/tone_stereo_128k.ref.s16");
  check_above_band();
  check_damaged();
  check_pcm8();
  TEST_END();
}
