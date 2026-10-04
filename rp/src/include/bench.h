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
#define DEVHOOKS_APP_WRITE_TEST 6  // [WORD: chunk in sectors] the card's
                                   // write rate (a 2 MB scratch file in the
                                   // folder, then deleted)
#define DEVHOOKS_APP_CONVERT_ALL 9  // the whole game for the machine
                                    // plugged in (as C)
#define DEVHOOKS_APP_PLAY 8        // WORD N [WORD 3|4 [WORD F]]: scene
                                   // clip N's clip file played (the ST or
                                   // the STE set; from frame F); no word:
                                   // stopped (a soak run too), then back
#define DEVHOOKS_APP_SOAK 10       // [WORD N [WORD 3|4]]: every clip from
                                   // N to the last, one after the other
#define DEVHOOKS_APP_GAME 12       // the game, or back to the bench
#define DEVHOOKS_APP_NO_IMAGE 11   // [WORD 1|2] the image forgotten, as
                                   // if the card had none: the start
                                   // without it (1: and no set, 2: and an
                                   // old set)
#define DEVHOOKS_APP_CONVERT 7     // WORD N [WORD 3|4]: scene clip N
                                   // converted into BENCH_FOLDER/STE or
                                   // /ST, <clip>.DLC
                                   // (for an ST or an STE; the machine
                                   // plugged in's by default); no word: back

// At boot, before the main loop.
void bench_init(void);

// At boot, after bench_init(), where md-microfirmware-template starts the SD
// card: mounts it, creates the app's folder when it is missing, finds the
// image and reads its root directory.
void bench_start_sd(void);

// A new ST session: draw the screen again (a conversion still running is
// stopped).
void bench_restart(void);

// Before the RP restarts: the card left idle, no file being written (a
// restart during the card's writes leaves it unanswering until it loses
// power: its chip select is fixed on this board).
void bench_stop_card_work(void);

void bench_handle_key(const ikbd_key_event_t *key);

// Runs a slice of the read test, or draws and publishes the screen when it
// changed. Never blocks for more than a frame.
void bench_frame(void);

uint32_t bench_devhook(uint16_t command_id, const uint16_t *payload,
                       uint16_t payload_size);

#endif  // BENCH_H
