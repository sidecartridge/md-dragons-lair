/**
 * File: gameui.h
 * Description: The game on the ST: the attract mode and the start menu,
 *              the game played (game.h's engine over player.h), the hints,
 *              the pause, game over, the continue and the high scores.
 *
 * The start menu replaces the arcade's "insert coins": every option on a
 * key of its own, kept in the app's settings. In the game, the stick in
 * port 1 (the mouse is port 0's) or the cursor keys and space.
 */

#ifndef GAMEUI_H
#define GAMEUI_H

#include <stdbool.h>
#include <stdint.h>

#include "ikbd.h"

// The options' bits (ACONFIG_PARAM_GAME_OPTIONS).
#define GAMEUI_OPT_HINTS 0x01u     // oldies mode: the move that passes
#define GAMEUI_OPT_INFINITE 0x02u  // infinite lives
#define GAMEUI_OPT_FIXED 0x04u     // the scenes in a fixed order
#define GAMEUI_OPT_WATCH 0x08u     // the game plays itself
#define GAMEUI_OPT_CONTINUE 0x10u  // a continue after game over
#define GAMEUI_OPT_SOUNDS 0x20u    // the input sounds
#define GAMEUI_OPT_ARCADE 0x40u    // the arcade's timing: no grace
#define GAMEUI_OPT_RETRY 0x80u     // a death replays its scene

// The bench's part: the clip file of `clip` ("S05D2") in the set the game
// plays, and the card at its fast speed or its own.
typedef struct {
  void (*clip_path)(char *out, unsigned size, const char *clip);
  void (*card_fast)(bool fast);
  void (*to_bench)(void);  // B in the menu
} gameui_host_t;

// Starts the attract mode (after the clips are found ready).
void gameui_start(const gameui_host_t *host);
void gameui_stop(void);
bool gameui_active(void);

// Every pass of the main loop.
void gameui_frame(void);

// Every key event, presses and releases.
void gameui_key(const ikbd_key_event_t *key);

// Debug builds: a game played by a bot through the IKBD decoder, as the
// keyboard (or the stick in port 1) would: each move the hint shows pressed
// `offset_ms` into its window, infinite lives, every press and death on the
// console (as in any game of a debug build). A negative offset stops it.
// Release builds: nothing.
void gameui_bot(int offset_ms, bool stick);

#endif  // GAMEUI_H
