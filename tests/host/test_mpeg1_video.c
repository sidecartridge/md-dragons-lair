// units: rp/src/mpeg_ps.c
/* The MPEG-1 video decoder, mpeg1_video.c, included here: its inverse DCT is
 * static. First the inverse DCT against a double-precision reference, as
 * IEEE 1180-1990 measures one (random blocks in three ranges, both signs:
 * peak error, mean and mean square error per pixel and overall), and its
 * shortcuts for blocks whose last rows are zero, which must give the full
 * transform's bytes. Then a small clip whose picture moves at every edge
 * (data/edge_64x48.mpg), its I and P pictures decoded with the frame store
 * against ffmpeg's decoding of them (data/edge_64x48.ref.yuv): no pixel more
 * than 1 away, the inverse DCTs rounding differently. Then the
 * demultiplexer on that clip cut at every byte (every byte of its first
 * packs, then every 31st): what it hands out is always the start of the
 * stream; and the decoder on it cut every 97 bytes, which stops, reading
 * and writing only its own buffers (the sanitizers). */

#include "../../rp/src/mpeg1_video.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

// --- The inverse DCT -----------------------------------------------------------

// IEEE 1180-1990's random numbers, in [-low, high].
static long ieee_seed = 1;
static long ieee_rand(long low, long high) {
  ieee_seed = (ieee_seed * 1103515245L + 12345L) & 0xFFFFFFFFL;
  long i = ieee_seed & 0x7FFFFFFEL;
  double x = (double)i / (double)0x7FFFFFFFL * (double)(low + high + 1);
  return (long)x - low;
}

#define PI 3.14159265358979323846

static double cosine[8][8];  // C(u) / 2 * cos((2x + 1) u pi / 16)

static void reference_init(void) {
  for (int u = 0; u < 8; u++) {
    double c = (u == 0) ? sqrt(0.125) : 0.5;
    for (int x = 0; x < 8; x++) {
      cosine[u][x] = c * cos((2 * x + 1) * u * PI / 16.0);
    }
  }
}

static long clamp_long(double v, long low, long high) {
  long r = lround(v);
  return r < low ? low : r > high ? high : r;
}

static void forward_dct(const long in[64], long out[64]) {
  for (int v = 0; v < 8; v++) {
    for (int u = 0; u < 8; u++) {
      double s = 0;
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
          s += cosine[v][y] * cosine[u][x] * (double)in[y * 8 + x];
        }
      }
      out[v * 8 + u] = clamp_long(s, -2048, 2047);
    }
  }
}

static void inverse_dct(const long in[64], long out[64]) {
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      double s = 0;
      for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
          s += cosine[v][y] * cosine[u][x] * (double)in[v * 8 + u];
        }
      }
      out[y * 8 + x] = clamp_long(s, -256, 255);
    }
  }
}

// The rows holding a coefficient, as the decoder marks them.
static uint32_t rows_of(const int16_t b[64]) {
  uint32_t rows = 0;
  for (int i = 0; i < 64; i++) {
    if (b[i] != 0) {
      rows |= 1u << (i / 8);
    }
  }
  return rows;
}

// The decoder's inverse DCT of `coefficients` added to `prediction`, as the
// decoder calls it (put when the prediction is NULL).
static void decoder_idct(const long coefficients[64],
                         const uint8_t *prediction, uint8_t out[64],
                         uint32_t rows) {
  int16_t b[64];
  for (int i = 0; i < 64; i++) {
    b[i] = (int16_t)coefficients[i];
  }
  if (prediction == NULL) {
    idct_put(b, out, 8, rows);
  } else {
    memcpy(out, prediction, 64);
    idct_add(b, out, 8, rows);
  }
  for (int i = 0; i < 64; i++) {
    CHECK(b[i] == 0 || !(rows & (1u << (i / 8))));  // its rows left clear
  }
}

// The transform's own value at each pixel, in [-256, 255] as IEEE 1180
// clamps it: put (a prediction of 0) gives 0..255; what it clamps to 0
// comes back from a prediction of 255 down to -254. Below that it is -255
// or -256, the transform's output clamped: the reference's value is taken
// then (either is within 1 of the other; in the range of 300 most such
// pixels are far below, where both clamp to -256); returns how many pixels
// that was.
static int decoder_signed(const long coefficients[64], long out[64],
                          const long reference[64]) {
  static uint8_t high[64];
  memset(high, 255, sizeof(high));
  uint8_t put[64], low[64];
  int16_t probe[64];
  for (int i = 0; i < 64; i++) {
    probe[i] = (int16_t)coefficients[i];
  }
  uint32_t rows = rows_of(probe);
  decoder_idct(coefficients, NULL, put, rows);
  decoder_idct(coefficients, high, low, rows);
  int guessed = 0;
  for (int i = 0; i < 64; i++) {
    if (put[i] > 0) {
      out[i] = put[i];
    } else if (low[i] > 0) {
      out[i] = (long)low[i] - 255;
    } else {
      out[i] = reference[i] <= -255 ? reference[i] : -255;
      guessed++;
    }
  }
  return guessed;
}

static void check_idct_range(long low, long high, int sign) {
  enum { BLOCKS = 10000 };
  static double err_sum[64], err_sq[64];
  memset(err_sum, 0, sizeof(err_sum));
  memset(err_sq, 0, sizeof(err_sq));
  long peak = 0;
  int guessed = 0;
  ieee_seed = 1;
  for (int n = 0; n < BLOCKS; n++) {
    long block[64], coefficients[64], reference[64], test[64];
    for (int i = 0; i < 64; i++) {
      block[i] = sign * ieee_rand(low, high);
    }
    forward_dct(block, coefficients);
    inverse_dct(coefficients, reference);
    guessed += decoder_signed(coefficients, test, reference);
    for (int i = 0; i < 64; i++) {
      long e = test[i] - reference[i];
      peak = labs(e) > peak ? labs(e) : peak;
      err_sum[i] += (double)e;
      err_sq[i] += (double)(e * e);
    }
  }
  double worst_mse = 0, worst_mean = 0, all_sq = 0, all_sum = 0;
  for (int i = 0; i < 64; i++) {
    worst_mse = fmax(worst_mse, err_sq[i] / BLOCKS);
    worst_mean = fmax(worst_mean, fabs(err_sum[i] / BLOCKS));
    all_sq += err_sq[i];
    all_sum += err_sum[i];
  }
  double mse = all_sq / (64.0 * BLOCKS);
  double mean = all_sum / (64.0 * BLOCKS);
  printf("IDCT [-%ld, %ld] x %+d: peak %ld, per pixel mse %.4f mean %.4f, "
         "overall mse %.4f mean %.5f (%d pixels below -254)\n",
         low, high, sign, peak, worst_mse, worst_mean, mse, mean, guessed);
  CHECK(peak <= 1);
  CHECK(worst_mse <= 0.06);
  CHECK(mse <= 0.02);
  CHECK(worst_mean <= 0.015);
  CHECK(fabs(mean) <= 0.0015);
}

// A block whose last rows are zero: the shortcuts give the full
// transform's bytes, put and added. The blocks are a picture's (random
// pixels transformed, as above), some rows cleared: the transform's
// integers hold a picture's coefficients, not any 12-bit values.
static void check_idct_shortcuts(void) {
  static const uint32_t masks[] = {0x01, 0x03, 0x05, 0x0F, 0x0A, 0x08};
  uint8_t prediction[64];
  for (int n = 0; n < 4000; n++) {
    uint32_t mask = masks[n % 6];
    long pixels[64], coefficients[64];
    long range = (n % 2) ? 300 : 5;
    for (int i = 0; i < 64; i++) {
      pixels[i] = ieee_rand(range, range);
      prediction[i] = (uint8_t)ieee_rand(0, 255);
    }
    forward_dct(pixels, coefficients);
    for (int i = 0; i < 64; i++) {
      if (!(mask & (1u << (i / 8))) || (n % 6 == 0 && i > 0)) {
        coefficients[i] = 0;  // the rows left out; at times all but the DC
      }
    }
    int16_t probe[64];
    for (int i = 0; i < 64; i++) {
      probe[i] = (int16_t)coefficients[i];
    }
    uint32_t rows = rows_of(probe);
    uint8_t quick[64], full[64];
    decoder_idct(coefficients, NULL, quick, rows);
    decoder_idct(coefficients, NULL, full, 0xFFu);
    CHECK(memcmp(quick, full, 64) == 0);
    decoder_idct(coefficients, prediction, quick, rows);
    decoder_idct(coefficients, prediction, full, 0xFFu);
    CHECK(memcmp(quick, full, 64) == 0);
  }
  // Nothing in, nothing out.
  long zero[64] = {0};
  uint8_t out[64];
  memset(prediction, 77, sizeof(prediction));
  decoder_idct(zero, prediction, out, 0);
  CHECK(memcmp(out, prediction, 64) == 0);
}

// --- A clip, against ffmpeg ---------------------------------------------------

#define CLIP_W 64
#define CLIP_H 48
#define PICTURE_BYTES (CLIP_W * CLIP_H * 3 / 2)

typedef struct {
  const uint8_t *data;
  uint32_t len;
  uint32_t pos;
} memory_t;

static int read_memory(void *ctx, uint8_t *buf, uint32_t len) {
  memory_t *m = (memory_t *)ctx;
  uint32_t n = m->len - m->pos < len ? m->len - m->pos : len;
  memcpy(buf, m->data + m->pos, n);
  m->pos += n;
  return (int)n;
}

static uint8_t *load(const char *path, uint32_t *len) {
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  *len = (uint32_t)ftell(f);
  rewind(f);
  uint8_t *data = malloc(*len);
  CHECK(fread(data, 1, *len, f) == *len);
  fclose(f);
  return data;
}

static uint8_t picture[PICTURE_BYTES];

static void store_row(void *ctx, int mb_row, const uint8_t *y,
                      const uint8_t *cb, const uint8_t *cr, int stride) {
  (void)ctx;
  for (int l = 0; l < 16; l++) {
    memcpy(picture + (mb_row * 16 + l) * CLIP_W, y + l * stride, CLIP_W);
  }
  uint8_t *pcb = picture + CLIP_W * CLIP_H;
  uint8_t *pcr = pcb + CLIP_W * CLIP_H / 4;
  for (int l = 0; l < 8; l++) {
    memcpy(pcb + (mb_row * 8 + l) * CLIP_W / 2, cb + l * (stride / 2),
           CLIP_W / 2);
    memcpy(pcr + (mb_row * 8 + l) * CLIP_W / 2, cr + l * (stride / 2),
           CLIP_W / 2);
  }
}

static uint8_t *slots[MPEG1_MAX_SLOTS];

// Decodes the clip's first `len` bytes; with `reference`, checks each I
// and P picture against it. Returns the pictures decoded.
static int decode(const uint8_t *data, uint32_t len,
                  const uint8_t *reference, uint32_t reference_len) {
  static mpeg_ps_t ps;
  static mpeg1_t m;
  memory_t mem = {data, len, 0};
  mpeg_ps_init(&ps, read_memory, &mem);
  mpeg1_init(&m, &ps);
  int rows = CLIP_H / 16 + 2;  // the decoder's store: mb_rows + 2
  for (int i = 0; i < rows; i++) {
    if (slots[i] == NULL) {
      slots[i] = malloc(MPEG1_SLOT_BYTES);
    }
    memset(slots[i], 0xAA, MPEG1_SLOT_BYTES);
  }
  mpeg1_set_slots(&m, slots, rows);
  int decoded = 0;
  int type;
  int worst = 0;
  while ((type = mpeg1_next_picture(&m)) > 0) {
    if (type == MPEG1_PICTURE_B) {
      mpeg1_skip_picture(&m);
      continue;
    }
    if (mpeg1_decode_picture(&m, store_row, NULL) <= 0) {
      break;
    }
    if (reference != NULL) {
      uint32_t at = (uint32_t)decoded * PICTURE_BYTES;
      CHECK(at + PICTURE_BYTES <= reference_len);
      if (at + PICTURE_BYTES > reference_len) {
        return decoded;
      }
      for (int i = 0; i < PICTURE_BYTES; i++) {
        int d = abs((int)picture[i] - (int)reference[at + i]);
        worst = d > worst ? d : worst;
      }
    }
    decoded++;
  }
  if (reference != NULL) {
    CHECK_EQ(type, MPEG1_END);
    CHECK_EQ(m.stats.errors, 0u);
    CHECK_EQ(m.stats.clamped_vectors, 0u);
    CHECK_EQ((uint32_t)decoded * PICTURE_BYTES, reference_len);
    CHECK(worst <= 1);
    printf("%d I and P pictures of the edge clip, %u blocks: within %d of "
           "ffmpeg's\n",
           decoded, (unsigned)m.stats.blocks, worst);
  }
  return decoded;
}

// The video stream of the clip's first `len` bytes.
static uint32_t demux(const uint8_t *data, uint32_t len, uint8_t *out,
                      uint32_t cap) {
  static mpeg_ps_t ps;
  memory_t mem = {data, len, 0};
  mpeg_ps_init(&ps, read_memory, &mem);
  uint32_t n = 0, got;
  while (n < cap && (got = mpeg_ps_read(&ps, out + n, cap - n)) > 0) {
    n += got;
  }
  return n;
}

static void check_clip(void) {
  uint32_t len = 0, ref_len = 0;
  uint8_t *clip = load("data/edge_64x48.mpg", &len);
  uint8_t *ref = load("data/edge_64x48.ref.yuv", &ref_len);
  if (clip == NULL || ref == NULL) {
    return;
  }
  int pictures = decode(clip, len, ref, ref_len);

  // The demultiplexer cut anywhere: the start of the stream, never more.
  uint8_t *whole = malloc(len);
  uint8_t *part = malloc(len);
  uint32_t es = demux(clip, len, whole, len);
  CHECK(es > 0);
  uint32_t previous = 0;
  for (uint32_t cut = 0; cut <= len; cut += (cut < 3 * 2048u) ? 1u : 31u) {
    uint32_t n = demux(clip, cut, part, len);
    CHECK(n <= es && n >= previous);
    CHECK(memcmp(part, whole, n) == 0);
    previous = n;
  }
  CHECK_EQ(previous, es);

  // The decoder cut anywhere: fewer pictures, and it stops.
  int last = 0;
  for (uint32_t cut = 0; cut <= len; cut += 97u) {
    int n = decode(clip, cut, NULL, 0);
    CHECK(n >= last - 1 && n <= pictures);
    last = n;
  }
  free(whole);
  free(part);
  free(clip);
  free(ref);
}

int main(void) {
  reference_init();
  check_idct_range(256, 255, 1);
  check_idct_range(256, 255, -1);
  check_idct_range(5, 5, 1);
  check_idct_range(5, 5, -1);
  check_idct_range(300, 300, 1);
  check_idct_range(300, 300, -1);
  check_idct_shortcuts();
  check_clip();
  TEST_END();
}
