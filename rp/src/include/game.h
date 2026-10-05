/**
 * File: game.h
 * Description: The game's engine: Dragon's Lair played over the game's
 *              table (game_table.h), as DirkSimple plays it
 *              (https://github.com/icculus/DirkSimple, zlib licence, Ryan
 *              C. Gordon: its scene manager in game.lua, ported to C).
 *
 * A tick a frame, with the time since the current clip started (the clip
 * the engine last asked for) and the inputs: the engine checks the moves'
 * windows, then the sequence's timeout, and says what the player must do
 * (game_out_t): start a clip at a frame, show one picture, make a sound.
 * The scenes come in the arcade's order (13 rows of 3, a scene of a row at
 * random in each of 3 cycles, the failed ones replayed before the dragon's
 * lair) or a fixed one; lives, the score, game over and a continue.
 *
 * Beside DirkSimple: the diagonals count (a stick's, or two arrow keys),
 * only to pass (DirkSimple's front end never reports one, so its few
 * windows where a diagonal kills never fire; a stick going through one
 * must not kill),
 * the options (infinite lives, starting lives, watch mode, the order, a
 * scene to start from, continue) and the hint (the move that passes). Plain
 * C, no SDK, no allocation: the host tests play it.
 */

#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "game_table.h"

#define GAME_FAILED_MAX 8  // the arcade queues up to 8 failed scenes
#define GAME_SCENE_NONE 0xFFFFu

// Inputs, as bits of GAME_IN_* (game_table.h).
#define GAME_BIT(in) (1u << (in))

// Sounds the player makes after a tick.
#define GAME_SOUND_ACCEPT 0x01u  // a move taken
#define GAME_SOUND_REJECT 0x02u  // a move with no window open

typedef struct {
  uint8_t starting_lives;  // 1 to 5 (the arcade's 5)
  bool infinite_lives;
  bool watch;              // the game makes every right move itself
  bool fixed_order;        // the first scene of each row, not one at random
  uint16_t start_scene;    // a scene to start the game at, or GAME_SCENE_NONE
  // A move pressed this long after its window still passes, unless a window
  // of its input is open then (a death that follows stays one); the
  // sequence's end waits for it. 0: DirkSimple's windows exactly.
  uint16_t grace_ms;
  bool retry;              // a death replays its scene, not the next row's
} game_options_t;

typedef struct {
  bool seek;           // start clip `clip` at `frame` (then_clip after it)
  bool single_frame;   // show the picture at `frame` and hold it
  uint8_t clip;        // game_clips' index, or GAME_CLIP_NONE (not on the CD)
  uint8_t then_clip;
  uint16_t frame;
  uint8_t sounds;      // GAME_SOUND_*
  uint16_t taken;      // game_actions' index of the move taken, or GAME_SEQ_NONE
  bool game_over;      // the game ended (`won`): the attract mode follows
  bool won;
  bool started;        // a game started (the attract mode's start)
} game_out_t;

typedef struct {
  game_options_t options;
  bool playing;             // a game, not the attract mode
  uint16_t scene;           // game_scenes' index
  uint16_t sequence;        // game_sequences' index
  uint32_t sequence_ms;     // its time
  uint32_t offset_ms;       // the clip's time at the sequence's start
  uint16_t accepted;        // game_actions' index taken, or GAME_SEQ_NONE
  uint8_t lives;
  uint32_t score;
  bool infinite;            // the arcade's secret (up and left at the start)
  bool intro_done;
  uint8_t row;              // 0-based
  uint8_t cycle;            // 0-based: the column of the last row
  bool chosen[16][GAME_ROW_SCENES];
  uint16_t failed[GAME_FAILED_MAX];
  uint8_t total_failed;
  bool rerunning;           // the failed scenes replayed, before the lair
  uint16_t lost_in;         // the scene the last game was lost in
  uint32_t random;          // the order's pick (the arcade's: the ticks)
  uint32_t pressed_before;  // inputs held at the last tick
  bool start_pending;       // the attract mode's start, at the next tick
} game_t;

// Sets the engine up in the attract mode. Call game_tick() at once: the
// attract mode's first clip comes out of it.
void game_init(game_t *g, const game_options_t *options);

// One tick: `clip_ms` the time since the clip the engine asked for last
// started (it goes on across a then_clip), `held` the inputs held now
// (GAME_BIT()s; a press is an input held now and not at the last tick),
// `random` any number that changes (the order's pick). Fills `out`.
void game_tick(game_t *g, uint32_t clip_ms, uint32_t held, uint32_t random,
               game_out_t *out);

// Starts a game at once, as the attract mode's start does (`held`: the
// inputs held, for the arcade's secret). Fills `out`.
void game_start(game_t *g, uint32_t held, game_out_t *out);

// After a game over, when the options allow it: the game goes on from the
// scene lost in (with retry, that scene again), with the starting lives
// and the score at 0 (`held`: the inputs held, the fire that continued it
// no move).
void game_continue(game_t *g, uint32_t held, game_out_t *out);

// The move that passes now or next in the current sequence (a window not
// yet over that leads on, not to a death; an open one first, else the one
// that opens first): its input, and whether its window is open. False when
// there is none.
bool game_hint(const game_t *g, uint8_t *input, bool *open);

// The same move as game_actions' index (its window), or GAME_SEQ_NONE.
uint16_t game_hint_action(const game_t *g);

#endif  // GAME_H
