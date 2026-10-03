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
//   dlconv cadence CLIP.MPG [-v]      the clip at 25 frames a second: its
//                                     length, and the frames each I and P
//                                     picture covers (-v: one line each)
//   dlconv convert CLIP.MPG [--st] [--keep N] [-v] [OUT.rgb]
//                                     the clip as the cartridge converts it
//                                     (convert.h; --st: 512 colours, else
//                                     4,096; --keep N: palette stability,
//                                     N %): each picture's CRC-32 (-v) and
//                                     the clip's, the indices and the
//                                     palette slots that change from one
//                                     picture to the next; OUT.rgb its
//                                     frames at 25 a second, 320x200 RGB,
//                                     for ffmpeg -f rawvideo -pix_fmt rgb24
//                                     -s 320x200 -r 25 (--truecolor: the
//                                     scaled picture before its palette, at
//                                     the same cadence)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cadence.h"
#include "convert.h"
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

// The cadence of a clip, from its picture headers alone: which output
// frames each I and P picture covers, as the converter will show them.
static int cadence(const char *clip, bool verbose) {
  FILE *in = fopen(clip, "rb");
  if (in == NULL) {
    perror(clip);
    return 1;
  }
  static mpeg_ps_t cps;
  static mpeg1_t cdec;
  mpeg_ps_init(&cps, read_file, in);
  mpeg1_init(&cdec, &cps);
  static uint32_t shown[1u << 16];  // display index of each I and P picture
  static char shown_type[1u << 16];
  uint32_t count = 0;
  uint32_t last = 0;  // the last display position
  static bool used[1u << 16];
  int counts[5] = {0};
  int refused = 0;  // P pictures whose vectors the frame store cannot hold
  int type;
  while ((type = mpeg1_next_picture(&cdec)) > 0) {
    if (type <= 4) {
      counts[type]++;
    }
    bool keep = type == MPEG1_PICTURE_I ||
                (type == MPEG1_PICTURE_P && cdec.forward_f_code <= 2);
    refused += type == MPEG1_PICTURE_P && !keep;
    if (cdec.display_index > last) {
      last = cdec.display_index;
    }
    if (cdec.display_index < (1u << 16)) {
      used[cdec.display_index] = true;
    }
    if (keep && count < (1u << 16)) {
      shown_type[count] = type == MPEG1_PICTURE_I ? 'I' : 'P';
      shown[count++] = cdec.display_index;
    }
    mpeg1_skip_picture(&cdec);
  }
  fclose(in);
  uint32_t num = 0;
  uint32_t den = 0;
  if (count == 0 || !cadence_picture_rate(cdec.picture_rate, &num, &den)) {
    fprintf(stderr, "%s: no pictures, or picture rate code %d\n", clip,
            cdec.picture_rate);
    return 1;
  }
  uint32_t length = cadence_first_frame(last + 1u, num, den);
  uint32_t missing = 0;
  for (uint32_t k = 0; k <= last && k < (1u << 16); k++) {
    missing += !used[k];
  }
  uint32_t histogram[8] = {0};
  uint32_t out_of_order = 0;
  for (uint32_t i = 0; i < count; i++) {
    // Frames before the first picture shown show it too.
    uint32_t first = (i == 0) ? 0 : cadence_first_frame(shown[i], num, den);
    uint32_t end = (i + 1 < count) ? cadence_first_frame(shown[i + 1], num, den)
                                   : length;
    out_of_order += i > 0 && shown[i] <= shown[i - 1];
    uint32_t frames = end > first ? end - first : 0;
    histogram[frames < 7 ? frames : 7]++;
    if (verbose) {
      printf("%c %5u  frames %5u..%5u  (%u)\n", shown_type[i],
             (unsigned)shown[i], (unsigned)first, (unsigned)end,
             (unsigned)frames);
    }
  }
  printf("%u pictures (%d I, %d P, %d B; %d P refused; %u display positions "
         "missing) at %u/%u a second: %u frames at %u; %u pictures shown, "
         "first at %u; frames each:",
         (unsigned)cdec.stats.pictures, counts[1], counts[2], counts[3],
         refused, (unsigned)missing, (unsigned)num, (unsigned)den,
         (unsigned)length,
         (unsigned)CADENCE_FPS, (unsigned)count, (unsigned)shown[0]);
  for (int f = 0; f < 8; f++) {
    if (histogram[f] != 0) {
      printf(" %d%s x %u", f, f == 7 ? "+" : "", (unsigned)histogram[f]);
    }
  }
  printf("%s\n", out_of_order ? " OUT OF ORDER" : "");
  return out_of_order ? 1 : 0;
}

// The converted pictures: CRCs, and the frames into OUT.
typedef struct {
  uint8_t indices[320 * 200];
  uint8_t previous[320 * 200];  // the indices of the picture before
  uint16_t previous_palette[16];
  uint64_t changed_indices;     // summed over the pictures after the first
  uint64_t changed_groups;      // 16-pixel groups of a line (an ST word)
  uint32_t changed_slots;
  uint32_t still_pictures;      // no index changed
  uint32_t kept_before;
  const convert_t *conv;
  bool truecolor;               // OUT gets the scaled picture, in RGB
  uint8_t rgb[320 * 200 * 3];  // the picture shown, from `shown_from`
  FILE *out;
  int bits;
  bool verbose;
  const mpeg1_t *dec;
  bool have;
  uint32_t shown_from;
  uint32_t clip_crc;
  uint32_t frames;  // written
} conv_sink_t;

static void conv_lines(void *ctx, int line, const uint8_t *indices,
                       int width) {
  conv_sink_t *k = (conv_sink_t *)ctx;
  memcpy(&k->indices[line * width], indices, 2u * (size_t)width);
}

static void conv_write(conv_sink_t *k, uint32_t until) {
  for (; k->have && k->shown_from + k->frames < until; k->frames++) {
    if (k->out != NULL) {
      fwrite(k->rgb, 1, sizeof(k->rgb), k->out);
    }
  }
}

static void conv_picture(void *ctx, const picture16_palette_t *palette,
                         uint32_t first_frame) {
  conv_sink_t *k = (conv_sink_t *)ctx;
  uint32_t crc = crc32_update(0, k->indices, sizeof(k->indices));
  crc = crc32_update(crc, palette->rgb444, sizeof(palette->rgb444));
  k->clip_crc = crc32_update(k->clip_crc, &crc, sizeof(crc));
  uint32_t changed = 0;
  int slots = 0;
  if (k->have) {
    for (int i = 0; i < 320 * 200; i++) {
      changed += k->indices[i] != k->previous[i];
    }
    for (int g = 0; g < 320 * 200; g += 16) {
      k->changed_groups +=
          memcmp(&k->indices[g], &k->previous[g], 16) != 0;
    }
    for (int e = 0; e < 16; e++) {
      slots += palette->rgb444[e] != k->previous_palette[e];
    }
    k->changed_indices += changed;
    k->changed_slots += (uint32_t)slots;
    k->still_pictures += changed == 0;
  }
  bool kept = k->conv->kept != k->kept_before;
  k->kept_before = k->conv->kept;
  memcpy(k->previous, k->indices, sizeof(k->previous));
  memcpy(k->previous_palette, palette->rgb444, sizeof(k->previous_palette));
  if (k->verbose) {
    printf("%c %5u  frame %5u  %08X  %s %5u indices, %2d slots changed\n",
           k->dec->picture_type == MPEG1_PICTURE_I ? 'I' : 'P',
           (unsigned)k->dec->display_index, (unsigned)first_frame,
           (unsigned)crc, kept ? "kept" : "own ", (unsigned)changed, slots);
  }
  if (k->have) {
    conv_write(k, first_frame);
  }
  if (k->truecolor) {
    // The stored picture scaled again, whole, and its colours as the dither
    // sees them (298 (Y - 16), BT.601 terms, a chroma sample per 2 x 2).
    static uint8_t ty[200][320], tcb[100][160], tcr[100][160];
    static _Alignas(4) uint8_t tring[PICTURE16_SCALER_BYTES];
    picture16_scaler_t sc;
    picture16_scaler_init(&sc, k->dec->width, k->dec->height, tring,
                          &ty[0][0], &tcb[0][0], &tcr[0][0], 320, 200);
    for (int row = 0; row < k->dec->mb_rows; row++) {
      const uint8_t *y;
      const uint8_t *cb;
      const uint8_t *cr;
      if (mpeg1_reference_row(k->dec, row, &y, &cb, &cr)) {
        picture16_scaler_mb_row(&sc, row, y, cb, cr, k->dec->stride);
      }
    }
    for (int py = 0; py < 200; py++) {
      for (int px = 0; px < 320; px++) {
        int d = tcb[py / 2][px / 2] - 128;
        int e = tcr[py / 2][px / 2] - 128;
        int c = 298 * (ty[py][px] - 16) + 128;
        int v[3] = {(c + 409 * e) >> 8, (c - 100 * d - 208 * e) >> 8,
                    (c + 516 * d) >> 8};
        for (int g = 0; g < 3; g++) {
          k->rgb[3 * (py * 320 + px) + g] =
              (uint8_t)(v[g] < 0 ? 0 : v[g] > 255 ? 255 : v[g]);
        }
      }
    }
  } else {
    for (int i = 0; i < 320 * 200; i++) {
      uint16_t c = palette->rgb444[k->indices[i]];
      for (int g = 0; g < 3; g++) {
        int gun = (c >> (8 - 4 * g)) & 15;
        k->rgb[3 * i + g] =
            (uint8_t)((k->bits == 4) ? gun * 17 : (gun >> 1) * 255 / 7);
      }
    }
  }
  k->have = true;
  k->shown_from = first_frame;
  k->frames = 0;
}

static int convert_clip(int argc, char **argv) {
  const char *clip = argv[2];
  const char *out_path = NULL;
  static conv_sink_t sink;
  sink.bits = 4;
  int keep = -1;
  for (int a = 3; a < argc; a++) {
    if (strcmp(argv[a], "--st") == 0) {
      sink.bits = 3;
    } else if (strcmp(argv[a], "--keep") == 0 && a + 1 < argc) {
      keep = atoi(argv[++a]);
    } else if (strcmp(argv[a], "-v") == 0) {
      sink.verbose = true;
    } else if (strcmp(argv[a], "--truecolor") == 0) {
      sink.truecolor = true;
    } else {
      out_path = argv[a];
    }
  }
  FILE *in = fopen(clip, "rb");
  sink.out = (out_path != NULL) ? fopen(out_path, "wb") : NULL;
  if (in == NULL || (out_path != NULL && sink.out == NULL)) {
    perror("dlconv");
    return 1;
  }
  static mpeg_ps_t cps;
  static mpeg1_t cdec;
  static uint8_t store[MPEG1_MAX_SLOTS][MPEG1_SLOT_BYTES];
  static _Alignas(4) uint8_t ring[PICTURE16_SCALER_BYTES];
  static _Alignas(4) uint8_t lines[PICTURE16_LINES_BYTES];
  static uint8_t work[PICTURE16_WORK_BYTES];
  uint8_t *slots[MPEG1_MAX_SLOTS];
  for (unsigned i = 0; i < MPEG1_MAX_SLOTS; i++) {
    slots[i] = store[i];
  }
  mpeg_ps_init(&cps, read_file, in);
  mpeg1_init(&cdec, &cps);
  mpeg1_set_slots(&cdec, slots, (int)MPEG1_MAX_SLOTS);
  sink.dec = &cdec;
  picture16_options_t options = {sink.bits, PICTURE16_WEIGHT_SQRT,
                                 PICTURE16_DITHER_MIX, NULL, NULL};
  convert_out_t out = {conv_lines, conv_picture, &sink};
  static convert_t c;
  convert_init(&c, &cdec, ring, lines, work, &options, &out);
  c.keep_percent = keep;
  sink.conv = &c;
  int counts[3] = {0};
  int type;
  while ((type = convert_next(&c)) > 0) {
    counts[type == MPEG1_PICTURE_I ? 1 : 2]++;
  }
  uint32_t length = convert_length(&c);
  conv_write(&sink, length);
  printf("%u pictures converted (%d I, %d P) for %s, %u frames; clip CRC-32 "
         "%08X; end %d\n",
         (unsigned)c.pictures, counts[1], counts[2],
         sink.bits == 4 ? "an STE" : "an ST", (unsigned)length,
         (unsigned)sink.clip_crc, type);
  if (c.pictures > 1) {
    uint32_t after = c.pictures - 1;
    printf("palette kept on %u, evolved on %u of %u pictures; per picture "
           "after the first: "
           "%.1f%% of the indices changed, %.1f%% of the 16-pixel groups, "
           "%.2f slots; %u pictures with no index changed\n",
           (unsigned)c.kept, (unsigned)c.evolved, (unsigned)after,
           100.0 * (double)sink.changed_indices / ((double)after * 64000.0),
           100.0 * (double)sink.changed_groups / ((double)after * 4000.0),
           (double)sink.changed_slots / after, (unsigned)sink.still_pictures);
  }
  if (sink.out != NULL) {
    fclose(sink.out);
  }
  fclose(in);
  return type < 0 ? 1 : 0;
}

static mpeg_ps_t ps;
static mpeg1_t dec;

int main(int argc, char **argv) {
  if (argc >= 5 && strcmp(argv[1], "preview") == 0) {
    return preview(argc, argv);
  }
  if (argc >= 3 && strcmp(argv[1], "convert") == 0) {
    return convert_clip(argc, argv);
  }
  if ((argc == 3 || (argc == 4 && strcmp(argv[3], "-v") == 0)) &&
      strcmp(argv[1], "cadence") == 0) {
    return cadence(argv[2], argc == 4);
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
            "       dlconv audio CLIP.MPG [OUT.s16]\n"
            "       dlconv cadence CLIP.MPG [-v]\n"
            "       dlconv convert CLIP.MPG [--st] [--keep N] [--truecolor] [-v] "
            "[OUT.rgb]\n");
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
