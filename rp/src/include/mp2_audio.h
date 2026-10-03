/**
 * File: mp2_audio.h
 * Description: MPEG-1 Layer II audio decoder (ISO/IEC 11172-3) for the
 *              RP2040: the audio stream of a clip from mpeg_ps.h in, mono
 *              sound at half the stream's sampling rate out (22,050 Hz from
 *              the game's 44,100 Hz), a frame at a time.
 *
 * Mono and half rate come from the subbands: left and right are added per
 * subband (intensity stereo included), only the lower 16 of the 32
 * subbands are synthesised, and only every other output sample: the
 * standard's synthesis, low-passed at a quarter of the sampling rate by its
 * own filter bank and decimated by 2, for about a quarter of the work of a
 * stereo decode. Integer arithmetic only, no allocation, no SDK: the same
 * samples on the RP and on a PC.
 */

#ifndef MP2_AUDIO_H
#define MP2_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "mpeg_ps.h"

#define MP2_FRAME_SAMPLES 576u  // a frame's output: 1,152 / 2
#define MP2_MAX_FRAME 1729u     // bytes: 384 kbit/s at 32 kHz, padded
#define MP2_IN_BUFFER 512u

typedef struct {
  uint32_t frames;        // decoded
  uint32_t bad_headers;   // sync words that were no frame header
  uint32_t mode_frames[4];  // stereo, joint stereo, dual channel, mono
} mp2_stats_t;

typedef struct {
  mpeg_ps_t *ps;
  // The audio elementary stream, read through a small buffer.
  uint8_t in[MP2_IN_BUFFER];
  uint32_t in_pos;
  uint32_t in_len;
  bool end;
  // The frame being decoded, with 3 bytes of zeros after it for the bit
  // reader; its header.
  uint8_t frame[MP2_MAX_FRAME + 3];
  uint32_t bit;
  int sample_rate;  // Hz; the output's is half
  int mode;         // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
  // Synthesis: the last 16 time slots' matrixed values A[0..15] and
  // B[0..15] (V[2m] and V[32 + 2m] of the standard), Q11.
  int16_t va[16][16];
  int16_t vb[16][16];
  int slot;  // the newest
  mp2_stats_t stats;
} mp2_t;

void mp2_init(mp2_t *a, mpeg_ps_t *ps);

// Decodes the next frame into `out`: MP2_FRAME_SAMPLES mono samples, 16-bit,
// at half the stream's sampling rate. Returns their number, 0 at the end of
// the stream.
int mp2_decode_frame(mp2_t *a, int16_t *out);

// The output's sampling rate (Hz) of the frame last decoded.
static inline int mp2_output_rate(const mp2_t *a) { return a->sample_rate / 2; }

// The 8-bit step: `n` samples times `gain` (Q8: 256 is 1), through a soft
// limiter that leaves everything below half of full scale as it is and bends
// what is above towards full scale without reaching it, then rounded to
// signed 8 bits. The game's sound is quiet (-26 dBFS on average) and its
// peaks rare, so a gain above 1 needs the limiter rather than clipping.
#define MP2_GAIN_UNITY 256
void mp2_to_pcm8(const int16_t *in, int8_t *out, uint32_t n, int gain);

#endif  // MP2_AUDIO_H
