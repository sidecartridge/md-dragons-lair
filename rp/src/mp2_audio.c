/**
 * File: mp2_audio.c
 * Description: MPEG-1 Layer II audio decoder. See mp2_audio.h.
 *
 * A frame is a 32-bit header, an optional CRC, the bit allocation of every
 * subband (one for both channels from the joint-stereo bound on), the scale
 * factor selection and the scale factors of the allocated subbands, then 12
 * granules of 3 samples a subband. A sample is the level v (of L) of its
 * code, (2v - L + 1) / L, times its scale factor, 2^(1 - i/3) for factor i.
 *
 * Fixed point: a subband sample is Q12 (|s| < 2), the matrixed values Q26
 * then Q11 (|V| < 16, kept as 16-bit), the window Q14, the output Q15.
 */

#pragma GCC optimize("O3")

#include "mp2_audio.h"

#include <string.h>

#include "mp2_tables.h"

#define MODE_JOINT 1
#define MODE_MONO 3
#define OUT_SUBBANDS 16  // the subbands synthesised

void mp2_init(mp2_t *a, mpeg_ps_t *ps) {
  memset(a, 0, sizeof(*a));
  a->ps = ps;
}

// --- Input ----------------------------------------------------------------------

static int next_byte(mp2_t *a) {
  if (a->in_pos == a->in_len) {
    if (a->end) {
      return -1;
    }
    a->in_len = mpeg_ps_read(a->ps, a->in, MP2_IN_BUFFER);
    a->in_pos = 0;
    if (a->in_len == 0) {
      a->end = true;
      return -1;
    }
  }
  return a->in[a->in_pos++];
}

// Reads the next frame, its header included, into a->frame and returns its
// length in bytes; 0 at the end of the stream.
static uint32_t read_frame(mp2_t *a) {
  uint32_t h = 0;
  int got = 0;
  for (;;) {
    int b = next_byte(a);
    if (b < 0) {
      return 0;
    }
    h = (h << 8) | (uint32_t)b;
    if (++got < 4) {
      continue;
    }
    // Sync (12 ones), MPEG-1, Layer II.
    if ((h & 0xFFFE0000u) != 0xFFFC0000u) {
      continue;
    }
    uint32_t rate_index = (h >> 12) & 15u;
    uint32_t sr_index = (h >> 10) & 3u;
    if (rate_index == 0 || rate_index == 15 || sr_index == 3) {
      a->stats.bad_headers++;  // free format or reserved: keep looking
      continue;
    }
    uint32_t len = 144000u * mp2_bit_rates[rate_index - 1] /
                       mp2_sample_rates[sr_index] +
                   ((h >> 9) & 1u);
    a->frame[0] = (uint8_t)(h >> 24);
    a->frame[1] = (uint8_t)(h >> 16);
    a->frame[2] = (uint8_t)(h >> 8);
    a->frame[3] = (uint8_t)h;
    for (uint32_t i = 4; i < len; i++) {
      int c = next_byte(a);
      if (c < 0) {
        return 0;  // a frame cut short ends the stream
      }
      a->frame[i] = (uint8_t)c;
    }
    memset(a->frame + len, 0, 3);
    return len;
  }
}

// The next n bits (n <= 16) of the frame; zeros past its end.
static inline uint32_t get_bits(mp2_t *a, uint32_t limit, int n) {
  uint32_t bit = a->bit;
  a->bit = bit + (uint32_t)n;
  if (bit >= limit) {
    return 0;
  }
  const uint8_t *p = a->frame + (bit >> 3);
  uint32_t w = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
  w = (w << (bit & 7u)) & 0xFFFFFFu;
  return w >> (24 - n);
}

// --- Samples --------------------------------------------------------------------

// A subband's allocation: its quantisation class (1..17), 0 for none.
// `table`: 0 the low-rate tables, 1 the high-rate ones.
static int read_allocation(mp2_t *a, uint32_t limit, int table, int sb) {
  int entry = mp2_step_3[table][sb];
  return mp2_step_4[entry & 15][get_bits(a, limit, entry >> 4)];
}

// The 3 levels of a subband's next codes (each below the class's levels).
static void read_levels(mp2_t *a, uint32_t limit, const mp2_quant_t *q,
                        int v[3]) {
  int levels = q->levels;
  if (q->group) {
    uint32_t c = get_bits(a, limit, q->bits);
    v[0] = (int)(c % (uint32_t)levels);
    c /= (uint32_t)levels;
    v[1] = (int)(c % (uint32_t)levels);
    v[2] = (int)(c / (uint32_t)levels);
  } else {
    v[0] = (int)get_bits(a, limit, q->bits);
    v[1] = (int)get_bits(a, limit, q->bits);
    v[2] = (int)get_bits(a, limit, q->bits);
  }
  for (int i = 0; i < 3; i++) {
    if (v[i] >= levels) {
      v[i] = levels - 1;  // no code of a good stream
    }
  }
}

// A level as a fraction of 1, Q15: (2v - L + 1) / L.
static inline int32_t fraction(int v, const mp2_quant_t *q) {
  int32_t f30 = (2 * v - q->levels + 1) * q->mul;
  return (f30 + (1 << 14)) >> 15;
}

// A fraction times scale factor `sf`, Q12.
static inline int32_t scale(int32_t f15, int sf) {
  int shift = 17 + sf / 3;
  if (sf >= 63 || shift > 30) {
    return 0;  // factor 63 does not exist; the smallest are below Q12
  }
  int32_t x = f15 * mp2_scale_base[sf % 3];  // Q29
  return (x + (1 << (shift - 1))) >> shift;
}

// --- Synthesis ------------------------------------------------------------------

static inline int16_t to_q11(int32_t q26) {
  int32_t v = (q26 + (1 << 14)) >> 15;
  return (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
}

// One time slot: the 16 lower subbands' mono samples (Q12) to 16 output
// samples.
static void synthesise(mp2_t *a, const int32_t *s, int16_t *out) {
  int slot = a->slot = (a->slot + 1) & 15;
  int16_t *va = a->va[slot];
  int16_t *vb = a->vb[slot];
  for (int m = 0; m < 8; m++) {
    int32_t acc_a = 0;
    int32_t acc_b = 0;
    for (int k = 0; k < OUT_SUBBANDS; k++) {
      acc_a += s[k] * mp2_cos_a[m][k];
      acc_b += s[k] * mp2_cos_b[m][k];
    }
    va[m] = to_q11(acc_a);
    vb[m + 1] = to_q11(acc_b);
  }
  // The rest by symmetry: V[32 - i] = -V[i], V[96 - i] = V[i].
  va[8] = 0;
  for (int m = 1; m < 8; m++) {
    va[16 - m] = (int16_t)-va[m];
    vb[16 - m] = vb[m];
  }
  vb[0] = (int16_t)-va[0];
  for (int m = 0; m < 16; m++) {
    int32_t acc = 0;
    for (int j = 0; j < 8; j++) {
      acc += a->va[(slot - 2 * j) & 15][m] * mp2_window_a[j][m];
      acc += a->vb[(slot - 2 * j - 1) & 15][m] * mp2_window_b[j][m];
    }
    int32_t v = (acc + (1 << 9)) >> 10;  // Q25 to Q15
    out[m] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
  }
}

// --- Frame ----------------------------------------------------------------------

int mp2_decode_frame(mp2_t *a, int16_t *out) {
  uint32_t len = read_frame(a);
  if (len == 0) {
    return 0;
  }
  uint32_t limit = len * 8u;
  const uint8_t *h = a->frame;
  int rate_index = h[2] >> 4;
  int mode = h[3] >> 6;
  a->sample_rate = mp2_sample_rates[(h[2] >> 2) & 3];
  a->mode = mode;
  a->stats.mode_frames[mode]++;
  a->bit = ((h[1] & 1u) == 0) ? 48u : 32u;  // the CRC is not checked

  int channels = (mode == MODE_MONO) ? 1 : 2;
  int bound = (mode == MODE_JOINT) ? (((h[3] >> 4) & 3) + 1) * 4
              : (mode == MODE_MONO) ? 0
                                    : 32;
  int tab = mp2_step_2[mp2_step_1[channels - 1][rate_index - 1]]
                      [(h[2] >> 2) & 3];
  int sblimit = tab & 63;
  int table = tab >> 6;
  if (bound > sblimit) {
    bound = sblimit;
  }

  uint8_t alloc[2][32];
  memset(alloc, 0, sizeof(alloc));
  for (int sb = 0; sb < sblimit; sb++) {
    alloc[0][sb] = (uint8_t)read_allocation(a, limit, table, sb);
    alloc[1][sb] = (sb < bound) ? (uint8_t)read_allocation(a, limit, table, sb)
                                : alloc[0][sb];
  }
  uint8_t scfsi[2][32];
  for (int sb = 0; sb < sblimit; sb++) {
    for (int ch = 0; ch < channels; ch++) {
      if (alloc[ch][sb] != 0) {
        scfsi[ch][sb] = (uint8_t)get_bits(a, limit, 2);
      }
    }
  }
  uint8_t sf[2][32][3];
  for (int sb = 0; sb < sblimit; sb++) {
    for (int ch = 0; ch < channels; ch++) {
      if (alloc[ch][sb] == 0) {
        continue;
      }
      uint8_t *f = sf[ch][sb];
      switch (scfsi[ch][sb]) {
        case 0:
          f[0] = (uint8_t)get_bits(a, limit, 6);
          f[1] = (uint8_t)get_bits(a, limit, 6);
          f[2] = (uint8_t)get_bits(a, limit, 6);
          break;
        case 1:
          f[0] = f[1] = (uint8_t)get_bits(a, limit, 6);
          f[2] = (uint8_t)get_bits(a, limit, 6);
          break;
        case 2:
          f[0] = f[1] = f[2] = (uint8_t)get_bits(a, limit, 6);
          break;
        default:
          f[0] = (uint8_t)get_bits(a, limit, 6);
          f[1] = f[2] = (uint8_t)get_bits(a, limit, 6);
          break;
      }
    }
  }

  int produced = 0;
  for (int part = 0; part < 3; part++) {
    for (int granule = 0; granule < 4; granule++) {
      int32_t s[3][OUT_SUBBANDS];  // mono, Q12: [time slot][subband]
      memset(s, 0, sizeof(s));
      for (int sb = 0; sb < sblimit; sb++) {
        int shared = sb >= bound;  // one code for both channels
        for (int ch = 0; ch < (shared ? 1 : channels); ch++) {
          int c = alloc[ch][sb];
          if (c == 0) {
            continue;
          }
          const mp2_quant_t *q = &mp2_quant[c - 1];
          if (sb >= OUT_SUBBANDS) {
            // Above the output's band: the bits only.
            a->bit += q->group ? q->bits : 3u * q->bits;
            continue;
          }
          int v[3];
          read_levels(a, limit, q, v);
          for (int t = 0; t < 3; t++) {
            int32_t f = fraction(v[t], q);
            if (channels == 1) {
              s[t][sb] = scale(f, sf[0][sb][part]);
            } else if (shared) {
              // Intensity stereo: the code times each channel's factor.
              s[t][sb] = (scale(f, sf[0][sb][part]) +
                          scale(f, sf[1][sb][part])) >> 1;
            } else {
              s[t][sb] += scale(f, sf[ch][sb][part]) >> 1;
            }
          }
        }
      }
      for (int t = 0; t < 3; t++) {
        synthesise(a, s[t], out + produced);
        produced += OUT_SUBBANDS;
      }
    }
  }
  a->stats.frames++;
  return produced;
}

// --- 8-bit output ---------------------------------------------------------------

#define LIMIT_FULL 32767
#define LIMIT_KNEE 16384    // half of full scale
#define LIMIT_MAX 131071    // inputs beyond are held here (4 x full scale)

void mp2_to_pcm8(const int16_t *in, int8_t *out, uint32_t n, int gain) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t v = ((int32_t)in[i] * gain) >> 8;
    int32_t m = (v < 0) ? -v : v;
    if (m > LIMIT_KNEE) {
      // knee + (full - knee) x t / (1 + t), t = (m - knee) / (full - knee):
      // continuous with a slope of 1 at the knee, below full scale for any m.
      if (m > LIMIT_MAX) {
        m = LIMIT_MAX;
      }
      uint32_t over = (uint32_t)(m - LIMIT_KNEE);
      m = LIMIT_KNEE +
          (int32_t)((uint32_t)(LIMIT_FULL - LIMIT_KNEE) * over /
                    (over + (uint32_t)(LIMIT_FULL - LIMIT_KNEE)));
      v = (v < 0) ? -m : m;
    }
    int32_t b = (v + 128) >> 8;
    out[i] = (int8_t)(b > 127 ? 127 : (b < -128 ? -128 : b));
  }
}
