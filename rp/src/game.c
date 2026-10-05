/**
 * File: game.c
 * Description: The game's engine. See game.h. The scene manager is
 *              DirkSimple's (game.lua's start_sequence, start_scene,
 *              choose_next_scene, start_game, kill_player, check_actions,
 *              check_timeout, tick), ported to C over game_table.h.
 */

#include "game.h"

#include <string.h>

#define DIAGONALS                                                       \
  (GAME_BIT(GAME_IN_UPLEFT) | GAME_BIT(GAME_IN_UPRIGHT) |               \
   GAME_BIT(GAME_IN_DOWNLEFT) | GAME_BIT(GAME_IN_DOWNRIGHT))
#define MOVES                                                           \
  (GAME_BIT(GAME_IN_UP) | GAME_BIT(GAME_IN_DOWN) | GAME_BIT(GAME_IN_LEFT) | \
   GAME_BIT(GAME_IN_RIGHT) | GAME_BIT(GAME_IN_ACTION))

static void choose_next_scene(game_t *g, bool resurrection, game_out_t *out);

static const game_sequence_t *seq_of(const game_t *g) {
  return &game_sequences[g->sequence];
}

static bool kills(uint16_t seq) {
  return seq != GAME_SEQ_NONE &&
         (game_sequences[seq].flags & GAME_SEQ_KILLS) != 0;
}

// --- Sequences and scenes ------------------------------------------------------

// A move that passes: it leads on, not to a death.
static bool passes(const game_action_t *act) {
  return act->next != GAME_SEQ_NONE && !kills(act->next);
}

// Where a move's window ends for good: its end, or its sequence's (the
// earlier: a timeout ends the window), plus the grace.
static uint32_t grace_end(const game_t *g, const game_sequence_t *s,
                          const game_action_t *act) {
  uint32_t end = act->to_ms < s->timeout_ms ? act->to_ms : s->timeout_ms;
  return end + g->options.grace_ms;
}

// The sequence's end: its timeout, or later while a move that passes is
// in its grace (none taken yet).
static uint32_t timeout_of(const game_t *g, const game_sequence_t *s) {
  uint32_t t = s->timeout_ms;
  if (g->options.grace_ms == 0 || t == 0 || !g->playing ||
      g->accepted != GAME_SEQ_NONE) {
    return t;
  }
  uint32_t end = t;
  for (uint16_t i = 0; i < s->action_count; i++) {
    const game_action_t *act = &game_actions[s->first_action + i];
    if (passes(act) && grace_end(g, s, act) > end) {
      end = grace_end(g, s, act);
    }
  }
  return end;
}

static void start_sequence(game_t *g, uint16_t seq, game_out_t *out) {
  const game_sequence_t *s = &game_sequences[seq];
  const game_sequence_t *before = &game_sequences[g->sequence];
  g->sequence = seq;
  g->accepted = GAME_SEQ_NONE;
  if (s->clip == GAME_CLIP_NONE && (s->flags & GAME_SEQ_NOT_ON_CD) == 0) {
    // No seek: the clip plays on, the sequence's time from here: where the
    // one before ended, at its timeout at most (a grace past it is not the
    // clip's, and the sequences after keep their place).
    uint32_t ran = g->sequence_ms;
    if (g->options.grace_ms > 0 && before->timeout_ms > 0 &&
        ran > before->timeout_ms) {
      ran = before->timeout_ms;
    }
    g->offset_ms += ran;
  } else {
    // A seek: the player starts the clip; its time starts at 0.
    out->seek = (s->flags & GAME_SEQ_SINGLE_FRAME) == 0;
    out->single_frame = !out->seek;
    out->clip = s->clip;
    out->then_clip = s->then_clip;
    out->frame = s->frame;
    g->offset_ms = 0;
  }
  g->sequence_ms = 0;
}

static void start_scene(game_t *g, uint16_t scene, bool resurrection,
                        game_out_t *out) {
  const game_scene_t *sc = &game_scenes[scene];
  g->scene = scene;
  uint16_t entry = resurrection && sc->start_dead != GAME_SEQ_NONE
                       ? sc->start_dead
                       : sc->start_alive;
  start_sequence(g, entry, out);
}

static void start_attract_mode(game_t *g, bool after_game_over,
                               game_out_t *out) {
  g->playing = false;
  start_scene(g, game_attract_scene, after_game_over, out);
}

static void game_over(game_t *g, bool won, game_out_t *out) {
  if (won) {
    out->game_over = true;
    out->won = true;
    start_attract_mode(g, false, out);
    return;
  }
  const game_scene_t *sc = &game_scenes[g->scene];
  if (sc->game_over != GAME_SEQ_NONE) {
    start_sequence(g, sc->game_over, out);  // its end: game_over_complete
  } else {
    out->game_over = true;
    start_attract_mode(g, true, out);
  }
}

// The scenes' order, as the arcade's: a row at a time, a scene of the row
// not chosen before (at random, or the first with a fixed order), three
// cycles through the rows; the last row's scene is the cycle's own; before
// the dragon's lair, the failed scenes again until each is passed.
static void choose_next_scene(game_t *g, bool resurrection, game_out_t *out) {
  uint16_t lair = game_rows[game_row_count - 1][GAME_ROW_SCENES - 1];
  uint16_t current = g->scene;
  if (g->playing && current != GAME_SCENE_NONE &&
      current != game_attract_scene) {
    if (current == lair) {
      // the lair is replayed until it is passed (below)
    } else if (current == game_intro_scene) {
      g->intro_done = true;
    } else if (g->rerunning) {
      if (!resurrection && g->total_failed > 0) {
        memmove(&g->failed[0], &g->failed[1],
                (size_t)(g->total_failed - 1) * sizeof(g->failed[0]));
        g->total_failed--;
      }
      if (g->total_failed == 0) {
        g->rerunning = false;
      }
    } else {
      if (resurrection && g->total_failed < GAME_FAILED_MAX) {
        g->failed[g->total_failed++] = current;
      }
      g->row++;
      if (g->row >= game_row_count) {
        g->row = 0;
        if (g->cycle + 1 < GAME_ROW_SCENES) {
          g->cycle++;
        }
      }
    }
  }

  if (!g->intro_done) {
    start_scene(g, game_intro_scene, resurrection, out);
  } else if (g->rerunning && g->total_failed > 0) {
    start_scene(g, g->failed[0], resurrection, out);
  } else if (current == lair) {
    if (resurrection) {
      start_scene(g, lair, true, out);
    } else {
      game_over(g, true, out);  // the lair passed: the game is won
    }
  } else if (g->row == game_row_count - 1) {
    if (g->cycle == GAME_ROW_SCENES - 1 && g->total_failed > 0) {
      g->rerunning = true;
      start_scene(g, g->failed[0], resurrection, out);
    } else {
      start_scene(g, game_rows[g->row][g->cycle], resurrection, out);
    }
  } else {
    uint8_t eligible[GAME_ROW_SCENES];
    int n = 0;
    for (int c = 0; c < GAME_ROW_SCENES; c++) {
      if (!g->chosen[g->row][c]) {
        eligible[n++] = (uint8_t)c;
      }
    }
    if (n == 0) {  // a row chosen through (not in the arcade's three cycles)
      memset(g->chosen[g->row], 0, sizeof(g->chosen[g->row]));
      for (int c = 0; c < GAME_ROW_SCENES; c++) {
        eligible[n++] = (uint8_t)c;
      }
    }
    int pick = g->options.fixed_order ? 0 : (int)(g->random % (uint32_t)n);
    g->chosen[g->row][eligible[pick]] = true;
    start_scene(g, game_rows[g->row][eligible[pick]], resurrection, out);
  }
}

static void start_game(game_t *g, uint32_t held, game_out_t *out) {
  game_options_t o = g->options;
  memset(g, 0, sizeof(*g));
  g->options = o;
  g->playing = true;
  g->lives = o.starting_lives >= 1 && o.starting_lives <= 5 ? o.starting_lives
                                                              : 5;
  g->accepted = GAME_SEQ_NONE;
  g->scene = GAME_SCENE_NONE;
  // The arcade's secret: up and left held at the start, infinite lives.
  g->infinite = (held & GAME_BIT(GAME_IN_UP)) && (held & GAME_BIT(GAME_IN_LEFT));
  out->started = true;
  if (o.start_scene != GAME_SCENE_NONE && o.start_scene < game_scene_count &&
      o.start_scene != game_attract_scene) {
    // A scene to start at: the order goes on from its row.
    g->intro_done = true;
    for (uint8_t r = 0; r < game_row_count; r++) {
      for (uint8_t c = 0; c < GAME_ROW_SCENES; c++) {
        if (game_rows[r][c] == o.start_scene) {
          g->row = r;
          g->cycle = r == game_row_count - 1 ? c : 0;
          g->chosen[r][c] = true;
          r = (uint8_t)game_row_count;  // found
          break;
        }
      }
    }
    start_scene(g, o.start_scene, false, out);
    return;
  }
  choose_next_scene(g, false, out);
}

static void kill_player(game_t *g, game_out_t *out) {
  if (!g->options.infinite_lives && !g->infinite && g->lives > 0) {
    g->lives--;
  }
  if (g->lives == 0) {
    g->lost_in = g->scene;
    game_over(g, false, out);
  } else if (g->options.retry) {
    start_scene(g, g->scene, true, out);  // the same scene again
  } else {
    choose_next_scene(g, true, out);
  }
}

// --- A tick ------------------------------------------------------------------------

static void interrupt(game_t *g, uint8_t which, uint32_t held,
                      game_out_t *out) {
  if (which == GAME_INT_START_GAME) {
    start_game(g, held, out);
  } else if (which == GAME_INT_GAME_OVER_COMPLETE) {
    out->game_over = true;
    start_attract_mode(g, true, out);
  }
}

// The moves: one taken in its window ends the checks until the sequence
// ends. A diagonal pressed is tried before its directions.
static void check_actions(game_t *g, uint32_t pressed, game_out_t *out) {
  if (g->accepted != GAME_SEQ_NONE) {
    return;
  }
  const game_sequence_t *s = seq_of(g);
  for (int pass = 0; pass < 2; pass++) {
    for (uint16_t i = 0; i < s->action_count; i++) {
      uint16_t a = (uint16_t)(s->first_action + i);
      const game_action_t *act = &game_actions[a];
      if (g->sequence_ms < act->from_ms || g->sequence_ms > act->to_ms) {
        continue;
      }
      bool diagonal = (GAME_BIT(act->input) & DIAGONALS) != 0;
      if (pass == 0 && !diagonal && (pressed & DIAGONALS)) {
        continue;  // a diagonal pressed: its own moves first
      }
      if (diagonal && kills(act->next)) {
        continue;  // a diagonal only passes: DirkSimple never reports one
      }
      if (g->options.watch && g->playing && act->next != GAME_SEQ_NONE &&
          !kills(act->next)) {
        g->accepted = a;
        out->taken = a;
        return;
      }
      if (pressed & GAME_BIT(act->input)) {
        g->accepted = a;
        out->taken = a;
        if (act->input != GAME_IN_START) {
          out->sounds |= GAME_SOUND_ACCEPT;
        }
        return;
      }
    }
    if ((pressed & DIAGONALS) == 0) {
      break;
    }
  }
  // The grace: a move that passes, pressed just after its window, when no
  // window of its input is open (one was, it was taken above).
  if (g->options.grace_ms > 0 && g->playing) {
    for (uint16_t i = 0; i < s->action_count; i++) {
      uint16_t a = (uint16_t)(s->first_action + i);
      const game_action_t *act = &game_actions[a];
      if (passes(act) && (pressed & GAME_BIT(act->input)) &&
          g->sequence_ms > act->from_ms &&
          g->sequence_ms <= grace_end(g, s, act)) {
        g->accepted = a;
        out->taken = a;
        out->sounds |= GAME_SOUND_ACCEPT;
        return;
      }
    }
  }
  if ((pressed & MOVES) && g->playing) {
    out->sounds |= GAME_SOUND_REJECT;
  }
}

// The sequence's end: its time over, or a move taken that interrupts or
// leads to another clip. A sequence of no time ends at once in turn.
static void check_timeout(game_t *g, uint32_t held, game_out_t *out) {
  for (int guard = 0; guard < 16; guard++) {
    const game_sequence_t *s = seq_of(g);
    const game_action_t *acc =
        g->accepted != GAME_SEQ_NONE ? &game_actions[g->accepted] : NULL;
    bool done = g->sequence_ms >= timeout_of(g, s);
    if (!done && acc != NULL) {
      done = acc->interrupt != GAME_INT_NONE ||
             (acc->next != GAME_SEQ_NONE &&
              (game_sequences[acc->next].clip != GAME_CLIP_NONE ||
               (game_sequences[acc->next].flags & GAME_SEQ_NOT_ON_CD)));
    }
    if (!done) {
      return;
    }
    uint8_t which = acc ? acc->interrupt : s->timeout_interrupt;
    uint16_t next = acc ? acc->next : s->timeout_next;
    g->score += acc ? acc->points : s->timeout_points;
    bool was_kill = (s->flags & GAME_SEQ_KILLS) != 0;
    if (which != GAME_INT_NONE) {
      interrupt(g, which, held, out);
    } else if (next != GAME_SEQ_NONE) {
      start_sequence(g, next, out);
    } else if (was_kill) {
      kill_player(g, out);
    } else {
      choose_next_scene(g, false, out);
    }
    if (seq_of(g)->timeout_ms != 0) {
      return;
    }
  }
}

void game_init(game_t *g, const game_options_t *options) {
  memset(g, 0, sizeof(*g));
  g->options = *options;
  g->scene = GAME_SCENE_NONE;
  g->accepted = GAME_SEQ_NONE;
  g->lives = 5;
  g->start_pending = true;
}

void game_tick(game_t *g, uint32_t clip_ms, uint32_t held, uint32_t random,
               game_out_t *out) {
  memset(out, 0, sizeof(*out));
  out->taken = GAME_SEQ_NONE;
  g->random = random;
  // A diagonal: its two directions held.
  static const uint8_t diag[4][3] = {
      {GAME_IN_UPLEFT, GAME_IN_UP, GAME_IN_LEFT},
      {GAME_IN_UPRIGHT, GAME_IN_UP, GAME_IN_RIGHT},
      {GAME_IN_DOWNLEFT, GAME_IN_DOWN, GAME_IN_LEFT},
      {GAME_IN_DOWNRIGHT, GAME_IN_DOWN, GAME_IN_RIGHT}};
  for (int d = 0; d < 4; d++) {
    if ((held & GAME_BIT(diag[d][1])) && (held & GAME_BIT(diag[d][2]))) {
      held |= GAME_BIT(diag[d][0]);
    }
  }
  uint32_t pressed = held & ~g->pressed_before;
  g->pressed_before = held;
  if (g->start_pending) {
    g->start_pending = false;
    start_attract_mode(g, false, out);
    check_timeout(g, held, out);
    return;
  }
  g->sequence_ms = clip_ms > g->offset_ms ? clip_ms - g->offset_ms : 0;
  check_actions(g, pressed, out);
  check_timeout(g, held, out);
}

void game_start(game_t *g, uint32_t held, game_out_t *out) {
  memset(out, 0, sizeof(*out));
  out->taken = GAME_SEQ_NONE;
  g->start_pending = false;
  start_game(g, held, out);
  g->pressed_before = held;
}

void game_continue(game_t *g, uint32_t held, game_out_t *out) {
  memset(out, 0, sizeof(*out));
  out->taken = GAME_SEQ_NONE;
  g->pressed_before = held;
  g->playing = true;
  g->lives = g->options.starting_lives >= 1 && g->options.starting_lives <= 5
                 ? g->options.starting_lives
                 : 5;
  g->score = 0;
  g->scene = g->lost_in;
  if (g->options.retry) {
    start_scene(g, g->lost_in, true, out);
  } else {
    choose_next_scene(g, true, out);
  }
}

uint16_t game_hint_action(const game_t *g) {
  if (!g->playing || g->accepted != GAME_SEQ_NONE) {
    return GAME_SEQ_NONE;
  }
  // A window open now, else the one that opens first (the moves are listed
  // by input, not by time: the rapids pass either way).
  const game_sequence_t *s = seq_of(g);
  uint16_t best = GAME_SEQ_NONE;
  uint32_t best_from = 0;
  for (uint16_t i = 0; i < s->action_count; i++) {
    uint16_t a = (uint16_t)(s->first_action + i);
    const game_action_t *act = &game_actions[a];
    if (act->to_ms < g->sequence_ms || act->next == GAME_SEQ_NONE ||
        kills(act->next)) {
      continue;
    }
    uint32_t from = act->from_ms > g->sequence_ms ? act->from_ms : g->sequence_ms;
    if (best == GAME_SEQ_NONE || from < best_from) {
      best = a;
      best_from = from;
    }
  }
  return best;
}

bool game_hint(const game_t *g, uint8_t *input, bool *open) {
  uint16_t a = game_hint_action(g);
  if (a == GAME_SEQ_NONE) {
    return false;
  }
  *input = game_actions[a].input;
  *open = game_actions[a].from_ms <= g->sequence_ms;
  return true;
}
