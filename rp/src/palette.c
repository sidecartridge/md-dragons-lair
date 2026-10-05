/**
 * File: palette.c
 * Description: 16-entry ST hardware-palette publisher.
 *
 * See palette.h for the public API and palette format. This file
 * owns the default 16-colour palette baked into the template and
 * exposes the bulk / single-entry write helpers.
 *
 * The cart slot is just RP RAM (mirrored to the cart shared region
 * via ROM_IN_RAM); writes here become visible to the m68k VBL
 * handler within one cart-bus access. No barrier is needed because
 * uint16_t writes are transparent across the byte-swap.
 */

#include "palette.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "memfunc.h"

/* Default 16-colour palette, inherited from the template: ramps of
 * blue, warm, green and earth shades in one shared palette:
 *
 *   idx 0  = white      -- text foreground (font_set_color(0))
 *                          + shifter border colour.
 *   idx 1..5            -- blue ramp (deep -> light) for sky and
 *                          cool-toned 3D shading.
 *   idx 6..9            -- warm ramp (yellow -> magenta) for sun /
 *                          accent / sprite variety.
 *   idx 10..12          -- green ramp for ground vegetation.
 *   idx 13..14          -- brown / earth for foreground ground.
 *   idx 15 = black      -- background (clear-to-0xFF target). */
static const uint16_t s_default_palette[PALETTE_ENTRIES] = {
    PALETTE_RGB(7, 7, 7),  /* 0  white   - text + border */
    PALETTE_RGB(1, 1, 2),  /* 1  near-black blue */
    PALETTE_RGB(1, 2, 4),  /* 2  dark blue */
    PALETTE_RGB(2, 4, 6),  /* 3  blue */
    PALETTE_RGB(4, 6, 7),  /* 4  light blue (sky) */
    PALETTE_RGB(6, 7, 7),  /* 5  pale cyan (horizon glow) */
    PALETTE_RGB(7, 7, 3),  /* 6  pale yellow (sun) */
    PALETTE_RGB(7, 5, 1),  /* 7  orange */
    PALETTE_RGB(7, 2, 1),  /* 8  red */
    PALETTE_RGB(6, 1, 5),  /* 9  magenta */
    PALETTE_RGB(1, 4, 1),  /* 10 dark green */
    PALETTE_RGB(2, 6, 2),  /* 11 green */
    PALETTE_RGB(4, 7, 2),  /* 12 light green */
    PALETTE_RGB(4, 3, 1),  /* 13 brown */
    PALETTE_RGB(3, 2, 1),  /* 14 dark brown */
    PALETTE_RGB(0, 0, 0),  /* 15 black - background */
};

static uint16_t *s_palette_slot;
static volatile uint16_t *s_gen_slot;
static uint16_t s_gen;
static uint16_t s_now[PALETTE_ENTRIES];    // the palette now
static uint16_t s_frame[PALETTE_ENTRIES];  // palette_set_frame()'s
static bool s_frame_given;
static uint16_t s_prepared[PALETTE_ENTRIES];  // the prepared frame's

static volatile uint16_t *cart_word(uint32_t offset) {
  return (volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ + offset);
}

// The palette now into the cart, bit 15 of its generation set while it
// changes, then a new generation: the ST never takes half of one.
static void write_now(void) {
  *s_gen_slot = (uint16_t)(s_gen | CART_PALETTE_GEN_BUSY);
  __sync_synchronize();
  for (uint32_t i = 0; i < PALETTE_ENTRIES; i++) {
    s_palette_slot[i] = s_now[i];
  }
  s_gen = (uint16_t)((s_gen + 1u) & (CART_PALETTE_GEN_BUSY - 1u));
  __sync_synchronize();
  *s_gen_slot = s_gen;
}

void palette_init(void) {
  s_palette_slot = (uint16_t *)cart_word(CART_PALETTE_OFFSET);
  s_gen_slot = cart_word(CART_PALETTE_GEN_OFFSET);
  palette_set(s_default_palette);
}

void palette_set(const uint16_t entries[PALETTE_ENTRIES]) {
  memcpy(s_now, entries, sizeof(s_now));
  write_now();
}

void palette_set_entry(uint8_t idx, uint16_t color) {
  if (idx >= PALETTE_ENTRIES) {
    return;
  }
  s_now[idx] = color;
  write_now();
}

void palette_set_frame(const uint16_t entries[PALETTE_ENTRIES]) {
  memcpy(s_frame, entries, sizeof(s_frame));
  s_frame_given = true;
}

void palette_prepare_frame(void) {
  memcpy(s_prepared, s_frame_given ? s_frame : s_now, sizeof(s_prepared));
  s_frame_given = false;
}

void palette_write_frame(void) {
  volatile uint16_t *slot = cart_word(CART_FRAME_PALETTE_OFFSET);
  for (uint32_t i = 0; i < PALETTE_ENTRIES; i++) {
    slot[i] = s_prepared[i];
  }
}
