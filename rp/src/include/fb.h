/**
 * File: fb.h
 * Description: Framebuffer module — owns the single 32 KB cartridge
 *              framebuffer the m68k reads each VBL.
 *
 * Single-FB design: there is exactly one framebuffer in
 * the shared region (`$FA8300`, 32 KB, 320x200x4bpp). Double-buffering
 * happens on the ST side; the RP just writes into this one buffer.
 *
 * This header is introduced alongside fb_font.* and
 * fb_draw.* so the ported font/draw modules have a stable target
 * type. fb.c defines `fb_screen` and provides
 * the init / clear / address-accessor entry points.
 */

#ifndef FB_H
#define FB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FB_MODE {
  unsigned short h_pixels;
  unsigned short v_pixels;
  unsigned char color_bits; /* bits per pixel */
};

/* Mirrors md-sprites-demo's VGA_SCREEN but with a single framebuffer
 * pointer instead of A/B/current/hidden (we don't keep a back buffer in
 * cartridge). The ported text renderer writes into
 * `framebuffer` directly. */
struct FB_SCREEN {
  unsigned int *framebuffer;
  uint16_t width;
  uint16_t height;
  uint8_t color_bits;
  uint8_t _pad;
};

extern struct FB_SCREEN fb_screen;

/* Built-in 320x200 4 bpp Atari ST low-res mode. Defined in fb.c. */
extern const struct FB_MODE fb_mode_320x200;

/**
 * @brief Populate `fb_screen` from a mode descriptor, build the pixel
 *        mask LUT used by the text/draw primitives, and zero the
 *        framebuffer.
 *
 * @return 0 on success, -1 if `mode` is NULL.
 */
int fb_init(const struct FB_MODE *mode);

/** @brief Fill the cartridge framebuffer with 0xFF (solid black in
 *         the default TOS low-res palette). */
void fb_clear(void);

/** @brief Publish the current chunked buffer to the cart framebuffer,
 *         synchronized to the Atari's 50 Hz VBL, tear-free.
 *
 *         Three steps: (1) transpose chunked -> planar SCRATCH (RP RAM,
 *         the slow ~1 ms part) -- this overlaps the m68k's blit of the
 *         previous frame since it doesn't touch the cart FB; (2) BLOCK
 *         until the m68k acks it finished that blit (a cart-bus read at
 *         $FB8400 captured by commemul) so the cart FB is free; (3) do
 *         the fast ~120 us chunk-reversed copy scratch -> cart FB and
 *         bump FB_FRAME_COUNTER.
 *
 *         Because only the short copy in step 3 writes the cart FB, and
 *         it runs in the m68k's post-blit slack, the RP never writes
 *         the FB while the m68k reads it -- no tearing, full 50 Hz. A
 *         ~60 ms timeout (safety net for "m68k not running", e.g. at
 *         boot) keeps the RP from hanging. Drawing into
 *         `fb_chunked_buffer` (RP RAM) is unsynchronized; call this
 *         once per frame after drawing. */
void fb_publish(void);

/** @brief fb_publish() in two steps, for an app that keeps a frame ready
 *         while it draws the next: fb_publish_prepare() converts
 *         `fb_chunked_buffer` into the planar frame (fb_planar_scratch, about
 *         1 ms) and takes the frame's palette (palette_set_frame() or the
 *         palette now); from then on the app may draw the next picture and
 *         set its palette. fb_publish_commit() puts the prepared frame on the
 *         ST (steps 2 and 3 above). Nothing else may publish, or borrow the
 *         planar scratch, between the two. */
void fb_publish_prepare(void);
void fb_publish_commit(void);

/** @brief Whether fb_publish_commit() would go on at once: the ST has
 *         copied the frame published before (fb_pump_rom3() sees it), or
 *         never will (not copied 200 ms after: an ST that boots counts the
 *         frame it finds as seen). An app that must not stall (its sound to
 *         top up) commits only then. */
bool fb_publish_ready(void);

/** @brief Drain the ROM3 commemul ring once, routing each captured
 *         sample to BOTH the IKBD demux and the VBL frame-sync
 *         detector. Call from the main loop in place of a bare
 *         commemul_poll(); fb_publish() also calls it internally while
 *         waiting for the VBL ack, so IKBD/ESC stay responsive during
 *         the wait. */
void fb_pump_rom3(void);

/** @brief Who copies each frame to the ST's screen, from its next VBL on:
 *         CART_BLIT_MODE_AUTO (the default: the blitter when the sound goes
 *         through the DMA chip, the CPU when Timer-B plays it on the YM),
 *         CART_BLIT_MODE_CPU (the 68000's MOVEM loop) or
 *         CART_BLIT_MODE_BLITTER (the blitter on any sound path: about
 *         1.4 ms of the ST's VBL back, but on the YM it breaks the sound;
 *         for an app without any). The blitter owns the bus `piece` chunks
 *         of 48 bytes at a time (0: the ST's default of 40, about 1 ms). An
 *         ST without a blitter copies with the CPU whatever the mode.
 *         Survives an ST reset. */
void fb_set_copy_mode(uint8_t mode, uint8_t piece);

/** @brief Microseconds the most recent fb_publish() spent in the
 *         chunky-to-planar conversion (dual-core c2p + chunk-reversed
 *         memcpy). Updated every fb_publish(); stale-by-one is fine
 *         for an on-screen timing readout. */
uint32_t fb_last_convert_us(void);

/** @brief Waits, draining the ROM3 ring, until the ST has copied the last
 *         published frame to its screen, or `timeout_us` has passed.
 *         Afterwards the ST reads the cart framebuffer again only after the
 *         next fb_publish(), so it may be borrowed as memory until then.
 *         Returns false on the timeout (no ST running, say). */
bool fb_wait_shown(uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* FB_H */
