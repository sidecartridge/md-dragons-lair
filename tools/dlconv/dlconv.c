// dlconv: the converter's code on a PC. For now it decodes an MPEG-1
// program stream with the firmware's decoder (rp/src/mpeg1_video.c) and
// writes the pictures as raw YUV 4:2:0, for comparison with a reference
// decoder:
//
//   dlconv iframes CLIP.MPG OUT.yuv   intra pictures only
//   dlconv ipframes CLIP.MPG OUT.yuv  I and P pictures, decoded in place in
//                                     mb_rows + 2 rows, as on the RP
//   ffmpeg -i CLIP.MPG -vf "select='eq(pict_type,I)'" -fps_mode passthrough \
//          -f rawvideo -pix_fmt yuv420p REF.yuv
//   (select='not(eq(pict_type,B))' for ipframes)
//   dlconv ipcrc CLIP.MPG             the CRC-32 of each I and P picture (its
//                                     rows' Y, Cb and Cr in order) and of the
//                                     clip (of those CRCs), as the RP prints
//   dlconv preview CLIP.MPG OUT N...  I pictures N... (from 1) through
//                                     picture16 at 320x200, for an STE (4,096
//                                     colours) and an ST (512), with each
//                                     dither: OUT_N_{ste,st}_DITHER.ppm

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"
#include "mp2_audio.h"
#include "mpeg1_video.h"
#include "mpeg_ps.h"
#include "picture16.h"

static int read_file(void *ctx, uint8_t *buf, uint32_t len) {
  size_t got = fread(buf, 1, len, (FILE *)ctx);
  return ferror((FILE *)ctx) ? -1 : (int)got;
}

typedef struct {
  uint8_t *frame;  // Y, then Cb, then Cr
  int width;
  int height;
} frame_t;

static void store_row(void *ctx, int mb_row, const uint8_t *y,
                      const uint8_t *cb, const uint8_t *cr, int stride) {
  frame_t *f = ctx;
  int cw = f->width / 2;
  int ch = f->height / 2;
  uint8_t *fy = f->frame;
  uint8_t *fcb = fy + f->width * f->height;
  uint8_t *fcr = fcb + cw * ch;
  for (int r = 0; r < 16 && mb_row * 16 + r < f->height; r++) {
    memcpy(fy + (mb_row * 16 + r) * f->width, y + r * stride, f->width);
  }
  for (int r = 0; r < 8 && mb_row * 8 + r < ch; r++) {
    memcpy(fcb + (mb_row * 8 + r) * cw, cb + r * stride / 2, cw);
    memcpy(fcr + (mb_row * 8 + r) * cw, cr + r * stride / 2, cw);
  }
}

static uint32_t picture_crc;

static void crc_row(void *ctx, int mb_row, const uint8_t *y, const uint8_t *cb,
                    const uint8_t *cr, int stride) {
  (void)ctx;
  (void)mb_row;
  picture_crc = crc32_update(picture_crc, y, (size_t)stride * 16);
  picture_crc = crc32_update(picture_crc, cb, (size_t)stride * 4);
  picture_crc = crc32_update(picture_crc, cr, (size_t)stride * 4);
}

static uint8_t p_y[320 * 200], p_cb[160 * 100], p_cr[160 * 100];
static _Alignas(4) uint8_t p_ring[PICTURE16_SCALER_BYTES];
static uint8_t p_work[PICTURE16_WORK_BYTES];
static picture16_scaler_t scaler;

static void scale_row(void *ctx, int mb_row, const uint8_t *y,
                      const uint8_t *cb, const uint8_t *cr, int stride) {
  (void)ctx;
  picture16_scaler_mb_row(&scaler, mb_row, y, cb, cr, stride);
}

static void write_ppm(const char *path, const uint8_t *indices,
                      const picture16_palette_t *pal, int bits) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    perror(path);
    return;
  }
  fprintf(f, "P6\n320 200\n255\n");
  for (int i = 0; i < 320 * 200; i++) {
    uint16_t c = pal->rgb444[indices[i]];
    for (int shift = 8; shift >= 0; shift -= 4) {
      int gun = (c >> shift) & 15;
      int v = (bits == 4) ? gun * 17 : (gun >> 1) * 255 / 7;
      fputc(v, f);
    }
  }
  fclose(f);
}

static int preview(int argc, char **argv) {
  FILE *in = fopen(argv[2], "rb");
  if (in == NULL) {
    perror(argv[2]);
    return 1;
  }
  static mpeg_ps_t pps;
  static mpeg1_t pdec;
  mpeg_ps_init(&pps, read_file, in);
  mpeg1_init(&pdec, &pps);
  int type;
  int index = 0;
  while ((type = mpeg1_next_picture(&pdec)) > 0) {
    if (type != MPEG1_PICTURE_I) {
      mpeg1_skip_picture(&pdec);
      continue;
    }
    index++;
    bool wanted = false;
    for (int a = 4; a < argc; a++) {
      wanted |= atoi(argv[a]) == index;
    }
    if (!wanted) {
      mpeg1_skip_picture(&pdec);
      continue;
    }
    picture16_scaler_init(&scaler, pdec.width, pdec.height, p_ring, p_y, p_cb,
                          p_cr, 320, 200);
    mpeg1_decode_picture(&pdec, scale_row, NULL);
    // The CRC-32s the cartridge's slideshow prints: the scaled planes, then
    // each conversion's indices and palette.
    uint32_t scaled = crc32_update(0, p_y, sizeof(p_y));
    scaled = crc32_update(scaled, p_cb, sizeof(p_cb));
    scaled = crc32_update(scaled, p_cr, sizeof(p_cr));
    printf("I picture %d: scaled %08X\n", index, (unsigned)scaled);
    static uint8_t indices[320 * 200];
    static const char *const dithers[PICTURE16_DITHERS] = {"bayer", "soft",
                                                           "mix", "none"};
    for (int bits = 3; bits <= 4; bits++) {
      for (int d = 0; d < PICTURE16_DITHERS; d++) {
        picture16_options_t opt = {bits, PICTURE16_WEIGHT_SQRT, d, NULL, NULL};
        picture16_palette_t pal;
        memcpy(indices, p_y, sizeof(indices));
        picture16_convert(indices, p_cb, p_cr, 320, 200, p_work, &opt, &pal,
                          NULL);
        char path[512];
        snprintf(path, sizeof(path), "%s_%d_%s_%s.ppm", argv[3], index,
                 bits == 4 ? "ste" : "st", dithers[d]);
        write_ppm(path, indices, &pal, bits);
        uint32_t crc = crc32_update(0, indices, sizeof(indices));
        crc = crc32_update(crc, pal.rgb444, sizeof(pal.rgb444));
        printf("  %s %s: %08X\n", bits == 4 ? "ste" : "st", dithers[d],
               (unsigned)crc);
      }
    }
  }
  fclose(in);
  return 0;
}

// The clip's sound as the cartridge converts it: mono, half the stream's
// rate, 16-bit little-endian, into OUT (none: only the CRC-32).
static int audio(const char *clip, const char *out_path) {
  FILE *in = fopen(clip, "rb");
  FILE *out = (out_path != NULL) ? fopen(out_path, "wb") : NULL;
  if (in == NULL || (out_path != NULL && out == NULL)) {
    perror("dlconv");
    return 1;
  }
  static mpeg_ps_t aps;
  static mp2_t mp2;
  mpeg_ps_init_stream(&aps, read_file, in, MPEG_PS_AUDIO);
  mp2_init(&mp2, &aps);
  int16_t pcm[MP2_FRAME_SAMPLES];
  uint32_t crc = 0;
  uint32_t samples = 0;
  int n;
  while ((n = mp2_decode_frame(&mp2, pcm)) > 0) {
    crc = crc32_update(crc, pcm, (size_t)n * sizeof(int16_t));
    samples += (uint32_t)n;
    if (out != NULL) {
      fwrite(pcm, sizeof(int16_t), (size_t)n, out);
    }
  }
  printf("%u frames (stereo %u, joint %u, dual %u, mono %u; %u bad headers), "
         "%u samples at %d Hz, CRC-32 %08X\n",
         (unsigned)mp2.stats.frames, (unsigned)mp2.stats.mode_frames[0],
         (unsigned)mp2.stats.mode_frames[1],
         (unsigned)mp2.stats.mode_frames[2],
         (unsigned)mp2.stats.mode_frames[3],
         (unsigned)mp2.stats.bad_headers, (unsigned)samples,
         mp2_output_rate(&mp2), (unsigned)crc);
  if (out != NULL) {
    fclose(out);
  }
  fclose(in);
  return 0;
}

static mpeg_ps_t ps;
static mpeg1_t dec;

int main(int argc, char **argv) {
  if (argc >= 5 && strcmp(argv[1], "preview") == 0) {
    return preview(argc, argv);
  }
  if ((argc == 3 || argc == 4) && strcmp(argv[1], "audio") == 0) {
    return audio(argv[2], argc == 4 ? argv[3] : NULL);
  }
  bool crc_only = argc == 3 && strcmp(argv[1], "ipcrc") == 0;
  bool with_p = crc_only || (argc == 4 && strcmp(argv[1], "ipframes") == 0);
  if (!crc_only &&
      (argc != 4 || (!with_p && strcmp(argv[1], "iframes") != 0))) {
    fprintf(stderr,
            "usage: dlconv iframes|ipframes CLIP.MPG OUT.yuv\n"
            "       dlconv ipcrc CLIP.MPG\n"
            "       dlconv preview CLIP.MPG OUT N...\n"
            "       dlconv audio CLIP.MPG [OUT.s16]\n");
    return 2;
  }
  FILE *in = fopen(argv[2], "rb");
  FILE *out = crc_only ? fopen("/dev/null", "wb") : fopen(argv[3], "wb");
  uint32_t clip_crc = 0;
  if (in == NULL || out == NULL) {
    perror("dlconv");
    return 1;
  }
  mpeg_ps_init(&ps, read_file, in);
  mpeg1_init(&dec, &ps);
  frame_t f = {NULL, 0, 0};
  int counts[5] = {0};
  int written = 0;
  int type;
  uint8_t *slots[MPEG1_MAX_SLOTS];
  while ((type = mpeg1_next_picture(&dec)) > 0) {
    if (f.frame == NULL) {
      f.width = dec.width;
      f.height = dec.height;
      f.frame = calloc((size_t)f.width * f.height * 3 / 2, 1);
      if (with_p) {
        int n = dec.mb_rows + 2;
        for (int i = 0; i < n; i++) {
          slots[i] = malloc(MPEG1_SLOT_BYTES);
          memset(slots[i], 0xAA, MPEG1_SLOT_BYTES);
        }
        mpeg1_set_slots(&dec, slots, n);
      }
    }
    if (type <= 4) {
      counts[type]++;
    }
    if (type == MPEG1_PICTURE_I || (with_p && type == MPEG1_PICTURE_P)) {
      picture_crc = 0;
      int r = mpeg1_decode_picture(&dec, crc_only ? crc_row : store_row, &f);
      if (r > 0 && crc_only) {
        clip_crc = crc32_update(clip_crc, &picture_crc, 4);
        printf("%c %08X\n", r == MPEG1_PICTURE_I ? 'I' : 'P',
               (unsigned)picture_crc);
        written++;
      } else if (r > 0) {
        fwrite(f.frame, 1, (size_t)f.width * f.height * 3 / 2, out);
        written++;
      } else {
        fprintf(stderr, "picture %u (type %d) not decoded: %d\n",
                (unsigned)dec.stats.pictures, type, r);
      }
    } else {
      mpeg1_skip_picture(&dec);
    }
  }
  fclose(out);
  fclose(in);
  if (crc_only) {
    printf("clip CRC-32 %08X\n", (unsigned)clip_crc);
  }
  printf("%dx%d: %d I, %d P, %d B pictures, %d written; %u macroblocks "
         "(%u skipped), %u blocks (%u DC only), %u slice errors, %u clamped "
         "vectors; stream end %d\n",
         f.width, f.height, counts[1], counts[2], counts[3], written,
         (unsigned)dec.stats.macroblocks,
         (unsigned)dec.stats.skipped_macroblocks, (unsigned)dec.stats.blocks,
         (unsigned)dec.stats.dc_only, (unsigned)dec.stats.errors,
         (unsigned)dec.stats.clamped_vectors, type);
  free(f.frame);
  return type < 0 ? 1 : 0;
}
