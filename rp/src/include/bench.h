/**
 * File: bench.h
 * Description: The bench screen. It finds the Dragon's Lair CD-ROM image in
 *              the app's folder on the SD card, shows what the image holds,
 *              and measures how fast the RP reads a clip out of it, at the
 *              configured SPI clock and at the fastest one, checking the
 *              bytes against their known CRC-32. Results go to the ST's
 *              screen and, in debug builds, to the console.
 *
 * The main loop calls bench_frame() once a pass; the read test runs in
 * slices of a few milliseconds from there, so the ROM3 ring, the keyboard
 * and SELECT keep being served while it runs.
 */

#ifndef BENCH_H
#define BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "aconfig.h"
#include "ikbd.h"

#define BENCH_FOLDER APP_FOLDER

// The image's usual name; any ISO in the folder whose root holds S01.MPG
// is taken when this one is not there.
#define BENCH_IMAGE_NAME "DL_CDROM_V31.ISO"

// Debug builds: commands for tools/dev/swd.py `app` (devhooks.h).
#define DEVHOOKS_APP_READ_TEST 1  // run the read test again
#define DEVHOOKS_APP_LIST_TOP 2   // WORD: first entry of the listing shown
#define DEVHOOKS_APP_SLIDESHOW 3  // WORD: the intra pictures of scene clip N;
                                  // no word: back to the bench
#define DEVHOOKS_APP_IN_PLACE 4   // WORD N [WORD 1]: I and P pictures of
                                  // scene clip N decoded in place (1: time
                                  // the IDCT too); no word: back
#define DEVHOOKS_APP_SOUND 5      // WORD N [WORD 1]: scene clip N's sound
                                  // decoded and timed (1: played); no word:
                                  // stopped
#define DEVHOOKS_APP_WRITE_TEST 6  // the card's write rate (a 2 MB scratch
                                   // file in the folder, then deleted)

// At boot, before the main loop.
void bench_init(void);

// At boot, after bench_init(), where md-microfirmware-template starts the SD
// card: mounts it, creates the app's folder when it is missing, finds the
// image and reads its root directory.
void bench_start_sd(void);

// A new ST session: draw the screen again.
void bench_restart(void);

void bench_handle_key(const ikbd_key_event_t *key);

// Runs a slice of the read test, or draws and publishes the screen when it
// changed. Never blocks for more than a frame.
void bench_frame(void);

uint32_t bench_devhook(uint16_t command_id, const uint16_t *payload,
                       uint16_t payload_size);

#endif  // BENCH_H
