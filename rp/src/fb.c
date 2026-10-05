/**
 * File: fb.c
 * Description: Framebuffer module — owns `fb_screen` and brings up the
 *              single 32 KB cartridge framebuffer the m68k reads each
 *              VBL (single-FB design).
 *
 * The framebuffer base is derived from the linker symbol
 * `__rom_in_ram_start__` plus `CART_FRAMEBUFFER_OFFSET`, so the
 * layout stays the single source of truth — apps must never hard-code
 * the address.
 */

#include "fb.h"

#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "font8x8.h"            /* defines `font8x8` (FB_FONT instance) */
#include "ikbd.h"
#include "palette.h"
#include "pico/time.h"          /* time_us_32 for the publish's timeout */
#include "profile.h"
#include "st_session.h"

/* Blit-done ack. The m68k blits only a frame it has not blitted yet (the
 * frame counter's low word changed; see FB_FRAME_COUNTER_ADDR in userfw.s) and
 * then does a cart-bus read at $FB8400 (VBLSYNC_ADDR); the commemul ring
 * captures it with low-16 = 0x84xx. fb_pump_rom3 routes ROM3 samples to
 * the IKBD demux, the ST's hello and this detector. fb_publish() waits for
 * the ack of the frame it published last before overwriting the cart FB:
 * after it, the m68k reads nothing until the counter changes again, so the
 * publish can happen at any moment. The timeout keeps the RP from hanging
 * when the m68k is not running (e.g. before it boots, or in GEM). */
#define FB_VBLSYNC_HIBYTE   0x8400u
#define FB_VBLSYNC_HIMASK   0xFF00u
/* Safety net only: must comfortably exceed the worst-case latency from
 * "counter bumped" to "m68k VBLSYNC" (up to ~2 VBLs = 40 ms when c2p
 * overruns the slack and the m68k skips a frame). Firing early would
 * let the next c2p race the blit -- so keep it generous; it should
 * never fire while the m68k is actually running. */
#define FB_VSYNC_TIMEOUT_US 60000u
/* A frame not copied this long after its publish never will be: an ST that
 * boots counts the frame it finds as seen, without an ack. A running ST
 * acks within 2 VBLs and a copy (about 70 ms at 25 fps, a VBL more with
 * the mouse moved fast). Without this, an app that commits only once the
 * ST has copied the frame before (fb_publish_ready()) waited for good. */
#define FB_ACK_LOST_US 200000u

static volatile uint32_t s_vbl_seen;
static uint32_t s_vbl_published;
static uint32_t s_published_at;  /* time_us_32() of the last publish */
/* Publishes that gave up waiting for the ack: expected until the ST runs
 * userfw, never while it does. Readable over SWD by its symbol. */
uint32_t fbAckTimeouts = 0;

const struct FB_MODE fb_mode_320x200 = {320, 200, 4};

struct FB_SCREEN fb_screen;

/* RP-incremented dirty-frame counter at $FA400C. The m68k userfw reads
 * this each VBL and only blits cart->ST screen when the value differs
 * from what it saw last iteration. Set in fb_init, bumped at the end of
 * fb_publish() after all FB writes commit. */
static volatile uint32_t *fb_frame_counter;

int fb_init(const struct FB_MODE *mode) {
  if (mode == NULL) {
    return -1;
  }
  fb_screen.framebuffer =
      (unsigned int *)((unsigned int)&__rom_in_ram_start__ +
                       CART_FRAMEBUFFER_OFFSET);
  fb_screen.width = mode->h_pixels;
  fb_screen.height = mode->v_pixels;
  fb_screen.color_bits = mode->color_bits;
  fb_frame_counter =
      (volatile uint32_t *)((uint8_t *)&__rom_in_ram_start__ +
                            CART_FB_FRAME_COUNTER_OFFSET);

  /* Zero the dirty-frame counter so the m68k userfw VBL loop sees a
   * clean baseline. The full shared region is already zeroed by
   * emul_start's ERASE_FIRMWARE_IN_RAM, so this is defensive against
   * future callers that might re-init fb without erasing. (Used to
   * live in chandler_init; relocated when chandler was removed.) */
  *fb_frame_counter = 0;

  /* The app's profile (profile.h): userfw reads it once, at its boot, for
   * the frames a VBL and the sound's rate. */
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_PROFILE_OFFSET)) = PROFILE_ID;

  /* Launch Core 1 with the chunky-to-planar bottom-half worker. */
  fb_chunked_init();

  /* A black frame, published once, so the cart framebuffer is in a
   * deterministic state before the main loop spins up. */
  fb_chunked_clear(15);
  fb_publish();

  DPRINTF("FB: %dx%d, %d bpp at %p (size=%u)\n", fb_screen.width,
          fb_screen.height, fb_screen.color_bits, fb_screen.framebuffer,
          (unsigned int)CART_FRAMEBUFFER_SIZE);
  return 0;
}

void fb_clear(void) {
  /* 0xFF on a 4 bpp Atari ST low-res screen with the default TOS
   * palette renders as solid black (all four bitplanes set -> palette
   * index 15). 0x00 would be palette index 0 = white, which is hard
   * to distinguish from "framebuffer never touched". */
  memset(fb_screen.framebuffer, 0xFF, CART_FRAMEBUFFER_SIZE);
}

/* Frames published (the value the frame counter takes), and the last
 * publish's conversion time. */
static uint32_t fb_frame_tick = 0;
static uint32_t last_convert_us = 0;

#if defined(_DEBUG) && (_DEBUG != 0)
/* The ST's stopwatch, when userfw.s reports it (TIME_STUDY): points of its
 * loop with the stopwatch's low 16 bits (MFP Timer-A /10: 10 / 2.4576 us a
 * tick). fbStopwatch keeps the latest FB_STOPWATCH_POINTS of them as point
 * << 16 | ticks and fbStopwatchCount counts them all: tools/dev/swd.py
 * stopwatch reads them. From them, the slack of each frame the ST copied:
 * how long before the VBL that may start the next frame (the profile's VBLs
 * a frame after the one before the copy) the copy ended. A histogram in
 * FB_SLACK_BUCKET_US steps (the last bucket holds the rest), the smallest
 * slack seen, and the copies that ended after that VBL (the next frame
 * waited a VBL more). Readable over SWD. */
#define FB_STOPWATCH_POINTS 128u
#define FB_STOPWATCH_POINT_COPY 1u
#define FB_STOPWATCH_POINT_COPIED 2u
#define FB_STOPWATCH_POINT_VBL 4u
#define FB_STOPWATCH_TICK_NS 4069u /* 10 / 2.4576 MHz */
#define FB_SLACK_BUCKETS 40u
#define FB_SLACK_BUCKET_US 100u
/* A VBL of the PAL ST (32.084988 MHz / 4 / (512 x 313)). */
#define FB_VBL_PERIOD_US 19979u
volatile uint32_t fbStopwatch[FB_STOPWATCH_POINTS];
volatile uint32_t fbStopwatchCount = 0;
volatile uint32_t fbSlackHist[FB_SLACK_BUCKETS];
volatile uint32_t fbSlackMinUs = UINT32_MAX;
volatile uint32_t fbSlackLate = 0;
volatile uint32_t fbSlackReports = 0;
static int s_sw_point = -1;
static int s_sw_high = -1;
static bool s_sw_vbl_seen, s_sw_copying;
static uint16_t s_sw_vbl, s_sw_copy_vbl; /* the last VBL, the VBL before the copy */

static void fb_stopwatch_sample(uint16_t sample) {
  uint8_t value = (uint8_t)sample;
  switch (sample & CART_ROM3_WINDOW_MASK) {
    case CART_ROM3_STUDY_POINT_WINDOW:
      s_sw_point = value;
      s_sw_high = -1;
      return;
    case CART_ROM3_STUDY_HI_WINDOW:
      if (s_sw_point >= 0) s_sw_high = value;
      return;
    case CART_ROM3_STUDY_LO_WINDOW:
      break;
    default:
      return;
  }
  if (s_sw_point < 0 || s_sw_high < 0) return;
  uint32_t point = (uint32_t)s_sw_point;
  uint16_t ticks = (uint16_t)((s_sw_high << 8) | value);
  s_sw_point = s_sw_high = -1;
  fbStopwatch[fbStopwatchCount % FB_STOPWATCH_POINTS] = (point << 16) | ticks;
  fbStopwatchCount++;
  if (point == FB_STOPWATCH_POINT_VBL) {
    s_sw_vbl = ticks;
    s_sw_vbl_seen = true;
  } else if (point == FB_STOPWATCH_POINT_COPY && s_sw_vbl_seen) {
    s_sw_copy_vbl = s_sw_vbl;
    s_sw_copying = true;
  } else if (point == FB_STOPWATCH_POINT_COPIED && s_sw_copying) {
    s_sw_copying = false;
    uint32_t ended_us =
        (uint16_t)(ticks - s_sw_copy_vbl) * FB_STOPWATCH_TICK_NS / 1000u;
    uint32_t deadline_us = PROFILE_VBLS_A_FRAME * FB_VBL_PERIOD_US;
    fbSlackReports++;
    if (ended_us >= deadline_us) {
      fbSlackLate++;
      return;
    }
    uint32_t slack_us = deadline_us - ended_us;
    if (slack_us < fbSlackMinUs) fbSlackMinUs = slack_us;
    uint32_t bucket = slack_us / FB_SLACK_BUCKET_US;
    if (bucket >= FB_SLACK_BUCKETS) bucket = FB_SLACK_BUCKETS - 1u;
    fbSlackHist[bucket]++;
  }
}
#else
#define fb_stopwatch_sample(sample) ((void)0)
#endif

/* ROM3 ring dispatch: route each captured cart-bus read to the IKBD
 * demux, the ST's hello, the VBL frame-sync detector and (debug builds) the
 * ST's stopwatch. */
static void fb_rom3_dispatch(uint16_t sample) {
  ikbd_consume_rom3_sample(sample);
  st_session_consume_rom3_sample(sample);
  if ((sample & FB_VBLSYNC_HIMASK) == FB_VBLSYNC_HIBYTE) {
    s_vbl_seen++;
  }
  fb_stopwatch_sample(sample);
}

void fb_pump_rom3(void) { commemul_poll(fb_rom3_dispatch); }

void fb_set_copy_mode(uint8_t mode, uint8_t piece) {
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_BLIT_MODE_OFFSET)) =
      (uint16_t)(mode | (piece << 8));
}

static uint32_t s_transpose_us;

void fb_publish_prepare(void) {
  /* 1. Transpose chunked -> planar SCRATCH (RP RAM, dual-core). This is
   *    the slow part (~1 ms) but it does NOT touch the cart FB, so it
   *    runs unsynchronized and overlaps the m68k's blit of the previous
   *    frame. The frame's palette is taken now, written with the frame. */
  uint32_t t0 = time_us_32();
  fb_transpose();
  s_transpose_us = time_us_32() - t0;
  palette_prepare_frame();
}

void fb_publish_commit(void) {
  /* 2. Block until the m68k has finished blitting the previous frame
   *    (its VBLSYNC ack) so the cart FB is free to overwrite. Drain the
   *    ROM3 ring meanwhile so IKBD / ESC stay live; the timeout is a
   *    safety net for "m68k not running" (e.g. at boot). */
  uint32_t t_wait = time_us_32();
  while (s_vbl_seen == s_vbl_published) {
    fb_pump_rom3();
    uint32_t now = time_us_32();
    if (now - t_wait > FB_VSYNC_TIMEOUT_US ||
        now - s_published_at > FB_ACK_LOST_US) {
      fbAckTimeouts++;
      break;
    }
  }
  s_vbl_published = s_vbl_seen;

  /* 3. Publish: fast chunk-reversed memcpy scratch -> cart FB. This is
   *    the only cart-FB write and it's short (~120 us), so it fits in
   *    the m68k's post-blit slack -- no overrun, no tearing, full 50 Hz.
   *    Mark the frame ready as the last write (barrier first). */
  uint32_t t1 = time_us_32();
  fb_planar_publish((uint16_t *)fb_screen.framebuffer);
  last_convert_us = s_transpose_us + (time_us_32() - t1);

  /* The frame's palette with it (palette.h), before the counter: the ST
   * takes both when it takes the frame. */
  palette_write_frame();

  fb_frame_tick++;
  __sync_synchronize();
  *fb_frame_counter = fb_frame_tick;
  s_published_at = time_us_32();
}

void fb_publish(void) {
  fb_publish_prepare();
  fb_publish_commit();
}

bool fb_publish_ready(void) {
  return s_vbl_seen != s_vbl_published ||
         time_us_32() - s_published_at > FB_ACK_LOST_US;
}

uint32_t fb_last_convert_us(void) { return last_convert_us; }

bool fb_wait_shown(uint32_t timeout_us) {
  uint32_t t0 = time_us_32();
  while (s_vbl_seen == s_vbl_published) {
    fb_pump_rom3();
    if (time_us_32() - t0 > timeout_us) {
      return false;
    }
  }
  return true;
}
