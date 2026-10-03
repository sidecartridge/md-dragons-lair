// units: rp/src/cadence.c rp/src/mpeg1_video.c rp/src/mpeg_ps.c
/* The cadence: which picture each 25 fps output frame shows. The
 * arithmetic first, then a synthetic clip with B pictures and open groups
 * (data/ibp_352x240.mpg, made with ffmpeg: see data/README.md), whose
 * pictures' places in display order must match the order ffprobe reports
 * (data/ibp_352x240.types). */

#include <stdio.h>
#include <string.h>

#include "cadence.h"
#include "mpeg1_video.h"
#include "mpeg_ps.h"
#include "test.h"

static int read_file(void *ctx, uint8_t *buf, uint32_t len) {
  size_t got = fread(buf, 1, len, (FILE *)ctx);
  return ferror((FILE *)ctx) ? -1 : (int)got;
}

static void check_arithmetic(void) {
  uint32_t num = 0, den = 0;
  CHECK(cadence_picture_rate(4, &num, &den));
  CHECK_EQ(num, 30000u);
  CHECK_EQ(den, 1001u);
  CHECK(cadence_picture_rate(3, &num, &den));
  CHECK_EQ(num, 25u);
  CHECK_EQ(den, 1u);
  CHECK(!cadence_picture_rate(0, &num, &den));
  CHECK(!cadence_picture_rate(9, &num, &den));

  // 25 pictures a second: one picture a frame.
  for (uint32_t k = 0; k < 2000; k++) {
    CHECK_EQ(cadence_first_frame(k, 25, 1), k);
  }
  // 30000 / 1001: picture k from frame ceil(k * 1001 / 1200); never two
  // frames apart, and the length kept to within a frame.
  CHECK_EQ(cadence_first_frame(0, 30000, 1001), 0u);
  CHECK_EQ(cadence_first_frame(1, 30000, 1001), 1u);
  CHECK_EQ(cadence_first_frame(6, 30000, 1001), 6u);  // 5.005
  CHECK_EQ(cadence_first_frame(1200, 30000, 1001), 1001u);
  CHECK_EQ(cadence_first_frame(1292, 30000, 1001), 1078u);  // S01's length
  for (uint32_t k = 0; k < 100000; k++) {
    uint32_t a = cadence_first_frame(k, 30000, 1001);
    uint32_t b = cadence_first_frame(k + 1, 30000, 1001);
    CHECK(b - a <= 1u);
    // a / 25 s is the first frame time at or after k * 1001 / 30000 s.
    CHECK((uint64_t)a * 1200u >= (uint64_t)k * 1001u);
    CHECK(a == 0u || (uint64_t)(a - 1u) * 1200u < (uint64_t)k * 1001u);
  }
  // No overflow at the length of the whole game, and far beyond.
  CHECK_EQ(cadence_first_frame(4000000000u, 30000, 1001),
           (uint32_t)((4000000000ull * 1001u + 1199u) / 1200u));
}

static void check_clip(void) {
  char types[128] = {0};
  FILE *t = fopen("data/ibp_352x240.types", "rb");
  CHECK(t != NULL);
  if (t == NULL) {
    return;
  }
  size_t count = fread(types, 1, sizeof(types) - 1, t);
  fclose(t);
  CHECK_EQ(count, 60u);

  FILE *f = fopen("data/ibp_352x240.mpg", "rb");
  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  static mpeg_ps_t ps;
  static mpeg1_t m;
  mpeg_ps_init(&ps, read_file, f);
  mpeg1_init(&m, &ps);
  bool seen[128] = {false};
  uint32_t last = 0;
  uint32_t kept[128];
  int kept_count = 0;
  int type;
  while ((type = mpeg1_next_picture(&m)) > 0) {
    uint32_t k = m.display_index;
    last = k > last ? k : last;
    CHECK(k < count);
    if (k < count) {
      CHECK(!seen[k]);
      seen[k] = true;
      CHECK_EQ(types[k], " IPBD"[type]);
    }
    if (type == MPEG1_PICTURE_I || type == MPEG1_PICTURE_P) {
      kept[kept_count++] = k;
    }
    mpeg1_skip_picture(&m);
  }
  fclose(f);
  CHECK_EQ(type, MPEG1_END);
  CHECK_EQ(m.picture_rate, 4);
  CHECK_EQ(m.stats.pictures, (uint32_t)count);
  for (size_t k = 0; k < count; k++) {
    CHECK(seen[k]);
  }

  // Without the B pictures, the I and P pictures come in display order, and
  // each output frame shows one of them, from the first.
  uint32_t num = 0, den = 0;
  CHECK(cadence_picture_rate(m.picture_rate, &num, &den));
  CHECK_EQ(last + 1u, (uint32_t)count);
  uint32_t length = cadence_first_frame(last + 1u, num, den);
  CHECK_EQ(length, 51u);  // 60 pictures at 29.97 a second: 2.002 s
  CHECK_EQ(kept_count, 21);
  CHECK_EQ(kept[0], 0u);
  uint32_t frames = 0;
  for (int i = 0; i < kept_count; i++) {
    uint32_t first = cadence_first_frame(kept[i], num, den);
    uint32_t end = (i + 1 < kept_count)
                       ? cadence_first_frame(kept[i + 1], num, den)
                       : length;
    if (i > 0) {
      CHECK(kept[i] > kept[i - 1]);
    }
    CHECK(end > first);  // three pictures apart: never hidden
    frames += end - first;
  }
  CHECK_EQ(frames, length);
}

int main(void) {
  check_arithmetic();
  check_clip();
  TEST_END();
}
