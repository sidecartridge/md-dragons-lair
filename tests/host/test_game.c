// units: rp/src/game.c rp/src/game_table.c
/* The game's engine over the game's table, against a simulated player that
 * starts the clips it is asked for and counts their time, 40 ms a tick:
 * the attract mode's loop and its start; watch mode winning the game (in
 * the fixed order and at random) without losing a life; a player doing
 * nothing losing five lives and reaching game over through the scene's
 * game over clip; a player following the hint (oldies mode's move) winning
 * too; infinite lives (the option and the arcade's secret) never ending;
 * a game started at a chosen scene; a continue after game over; the hint
 * showing an open window when one is; a late press passing with a grace;
 * a death replaying its scene with retry; and a diagonal pressed taking a
 * diagonal's move before its directions'. */

#include <string.h>

#include "game.h"
#include "test.h"

#define TICK_MS 40u

typedef struct {
  game_t g;
  uint32_t clip_ms;
  uint8_t clip;
  uint32_t ticks;
  uint32_t seeks;
  uint32_t scenes_started;
  uint16_t last_scene;
  bool over;
  bool won;
  uint32_t lives_lost;
  uint8_t lives_before;
  uint32_t taken;  // moves the engine reported taken
} sim_t;

static uint32_t rng = 12345;
static uint32_t next_random(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

static void apply(sim_t *s, const game_out_t *out) {
  if (out->seek || out->single_frame) {
    s->clip_ms = 0;
    s->clip = out->clip;
    s->seeks++;
    if (out->seek) {
      CHECK(out->clip < game_clip_count);
      CHECK(out->frame < game_clips[out->clip].frames);
    }
  }
  if (out->taken != GAME_SEQ_NONE) {
    CHECK(out->taken < game_action_count);
    s->taken++;
  }
  if (out->game_over) {
    s->over = true;
    s->won = out->won;
  }
}

static void sim_tick(sim_t *s, uint32_t held) {
  game_out_t out;
  game_tick(&s->g, s->clip_ms, held, next_random(), &out);
  apply(s, &out);
  if (s->g.playing && s->g.scene != s->last_scene) {
    s->last_scene = s->g.scene;
    s->scenes_started++;
  }
  if (s->g.playing && s->g.lives < s->lives_before) {
    s->lives_lost += (uint32_t)(s->lives_before - s->g.lives);
  }
  s->lives_before = s->g.lives;
  s->clip_ms += TICK_MS;
  s->ticks++;
}

static void sim_init(sim_t *s, const game_options_t *o) {
  memset(s, 0, sizeof(*s));
  s->last_scene = GAME_SCENE_NONE;
  game_init(&s->g, o);
  sim_tick(s, 0);
}

// Presses start until a game starts.
static void sim_start(sim_t *s, uint32_t also_held) {
  for (int i = 0; i < 2000 && !s->g.playing; i++) {
    sim_tick(s, (i & 1) ? GAME_BIT(GAME_IN_START) | also_held : also_held);
  }
  CHECK(s->g.playing);
  s->lives_before = s->g.lives;
}

static game_options_t options(void) {
  game_options_t o;
  memset(&o, 0, sizeof(o));
  o.starting_lives = 5;
  o.start_scene = GAME_SCENE_NONE;
  return o;
}

// Plays until the game ends or `max_ticks`; `player` gives the inputs held.
typedef uint32_t (*player_fn)(sim_t *s);

static void sim_play(sim_t *s, player_fn player, uint32_t max_ticks) {
  for (uint32_t i = 0; i < max_ticks && !s->over; i++) {
    sim_tick(s, player ? player(s) : 0u);
  }
}

// Follows the hint: presses its input while its window is open (a press,
// then a release, so that each is a new press).
static uint32_t follow_hint(sim_t *s) {
  uint8_t input;
  bool open;
  if ((s->ticks & 1) == 0 && game_hint(&s->g, &input, &open) && open) {
    uint32_t bits = GAME_BIT(input);
    switch (input) {  // a diagonal is its two directions held
      case GAME_IN_UPLEFT: bits = GAME_BIT(GAME_IN_UP) | GAME_BIT(GAME_IN_LEFT); break;
      case GAME_IN_UPRIGHT: bits = GAME_BIT(GAME_IN_UP) | GAME_BIT(GAME_IN_RIGHT); break;
      case GAME_IN_DOWNLEFT: bits = GAME_BIT(GAME_IN_DOWN) | GAME_BIT(GAME_IN_LEFT); break;
      case GAME_IN_DOWNRIGHT: bits = GAME_BIT(GAME_IN_DOWN) | GAME_BIT(GAME_IN_RIGHT); break;
      default: break;
    }
    return bits;
  }
  return 0;
}

#define GAME_TICKS (2u * 60u * 60u * 25u)  // two hours

static void check_attract(void) {
  sim_t s;
  game_options_t o = options();
  sim_init(&s, &o);
  CHECK(!s.g.playing);
  CHECK_EQ(s.g.scene, game_attract_scene);
  // The movie seeks into the first clip, then the frame not on the CD (the
  // menu's), then the movie again.
  CHECK_EQ(s.seeks, 1);
  CHECK(strcmp(game_clips[s.clip].name, "S01") == 0);
  uint32_t seeks = s.seeks;
  sim_play(&s, NULL, 60u * 25u);
  CHECK(s.seeks >= seeks + 2);
  CHECK(!s.g.playing);
}

static void check_watch(bool fixed) {
  sim_t s;
  game_options_t o = options();
  o.watch = true;
  o.fixed_order = fixed;
  sim_init(&s, &o);
  sim_start(&s, 0);
  sim_play(&s, NULL, GAME_TICKS);
  CHECK(s.over);
  CHECK(s.won);
  CHECK_EQ(s.lives_lost, 0);
  CHECK(s.g.score > 0);
  CHECK(s.taken > 0);
  // The intro, then 13 rows three times over, the lair last.
  CHECK(s.scenes_started >= 1u + 13u * 3u);
  printf("watch (%s): won in %u scenes, %.1f min, score %u\n",
         fixed ? "fixed order" : "arcade order", s.scenes_started,
         s.ticks * TICK_MS / 60000.0, s.g.score);
}

static void check_idle(void) {
  sim_t s;
  game_options_t o = options();
  sim_init(&s, &o);
  sim_start(&s, 0);
  sim_play(&s, NULL, GAME_TICKS);
  CHECK(s.over);
  CHECK(!s.won);
  CHECK_EQ(s.lives_lost, 5);
  CHECK(!s.g.playing);
  // A continue: the game goes on with its lives back.
  game_out_t out;
  game_continue(&s.g, GAME_BIT(GAME_IN_ACTION), &out);
  CHECK(s.g.playing);
  CHECK_EQ(s.g.lives, 5);
  CHECK(out.seek || out.single_frame);
  // The fire that continued it, still held, is no move.
  CHECK_EQ(s.g.pressed_before, GAME_BIT(GAME_IN_ACTION));
}

static void check_hints(void) {
  sim_t s;
  game_options_t o = options();
  o.fixed_order = true;
  sim_init(&s, &o);
  sim_start(&s, 0);
  sim_play(&s, follow_hint, GAME_TICKS);
  CHECK(s.over);
  CHECK(s.won);
  printf("following the hints: %s in %u scenes, %u lives lost, score %u\n",
         s.won ? "won" : "lost", s.scenes_started, s.lives_lost, s.g.score);
  CHECK_EQ(s.lives_lost, 0);
  CHECK(s.taken > 0);
}

static void check_infinite(void) {
  sim_t s;
  game_options_t o = options();
  o.infinite_lives = true;
  sim_init(&s, &o);
  sim_start(&s, 0);
  sim_play(&s, NULL, 30u * 60u * 25u);
  CHECK(!s.over);
  CHECK_EQ(s.g.lives, 5);
  CHECK(s.g.total_failed > 0);
  // The arcade's secret: up and left held at the start.
  sim_t t;
  game_options_t p = options();
  sim_init(&t, &p);
  sim_start(&t, GAME_BIT(GAME_IN_UP) | GAME_BIT(GAME_IN_LEFT));
  CHECK(t.g.infinite);
  sim_play(&t, NULL, 30u * 60u * 25u);
  CHECK(!t.over);
}

static void check_start_scene(void) {
  for (uint16_t sc = 0; sc < game_scene_count; sc++) {
    if (sc == game_attract_scene || sc == game_intro_scene) {
      continue;
    }
    sim_t s;
    game_options_t o = options();
    o.start_scene = sc;
    o.watch = true;
    sim_init(&s, &o);
    sim_start(&s, 0);
    CHECK_EQ(s.g.scene, sc);
    // It plays on from there to a scene after it.
    sim_play(&s, NULL, 5u * 60u * 25u);
    CHECK(s.scenes_started >= 2 || s.over);
  }
}

// The hint shows an open window whenever a move that passes is open (the
// rapids list a later move before an earlier one).
static void check_hint_open(void) {
  int checked = 0;
  for (uint16_t q = 0; q < game_sequence_count; q++) {
    const game_sequence_t *seq = &game_sequences[q];
    for (uint16_t i = 0; i < seq->action_count; i++) {
      const game_action_t *a = &game_actions[seq->first_action + i];
      if (a->next == GAME_SEQ_NONE ||
          (game_sequences[a->next].flags & GAME_SEQ_KILLS)) {
        continue;
      }
      game_t g;
      game_options_t o = options();
      game_init(&g, &o);
      g.playing = true;
      g.sequence = q;
      g.accepted = GAME_SEQ_NONE;
      g.sequence_ms = a->from_ms;
      uint8_t input;
      bool open = false;
      CHECK(game_hint(&g, &input, &open));
      CHECK(open);
      uint16_t h = game_hint_action(&g);
      CHECK(h != GAME_SEQ_NONE && game_actions[h].input == input);
      checked++;
    }
  }
  CHECK(checked > 0);
}

// The grace: a move that passes, pressed 100 ms after its window, passes
// with a grace of 250 ms and not without, also where a window of its input
// that kills has opened since (that one takes it without the grace).
static void check_grace(void) {
  int passed = 0, killed = 0;
  for (uint16_t q = 0; q < game_sequence_count; q++) {
    const game_sequence_t *seq = &game_sequences[q];
    if (seq->timeout_ms == 0) {
      continue;
    }
    for (uint16_t i = 0; i < seq->action_count; i++) {
      uint16_t a = (uint16_t)(seq->first_action + i);
      const game_action_t *act = &game_actions[a];
      if (act->next == GAME_SEQ_NONE ||
          (game_sequences[act->next].flags & GAME_SEQ_KILLS) ||
          act->input > GAME_IN_ACTION) {
        continue;
      }
      uint32_t end = act->to_ms < seq->timeout_ms ? act->to_ms : seq->timeout_ms;
      uint32_t t = end + 100u;
      // The window of its input open at t, if any.
      const game_action_t *open = NULL;
      for (uint16_t j = 0; j < seq->action_count; j++) {
        const game_action_t *o = &game_actions[seq->first_action + j];
        if (o->input == act->input && o->from_ms <= t && t <= o->to_ms) {
          open = o;
        }
      }
      for (int grace = 0; grace <= 1; grace++) {
        game_t g;
        game_options_t o = options();
        o.grace_ms = grace ? 250u : 0u;
        game_init(&g, &o);
        g.playing = true;
        g.sequence = q;
        g.accepted = GAME_SEQ_NONE;
        g.offset_ms = 0;
        g.start_pending = false;
        if (grace) {
          // The hint shows a move open as long as one can still pass.
          uint8_t in;
          bool op = false;
          g.sequence_ms = t;
          CHECK(game_hint(&g, &in, &op) && op);
        }
        game_out_t out;
        game_tick(&g, t, GAME_BIT(act->input), 1, &out);
        if (open != NULL && (game_sequences[open->next].flags & GAME_SEQ_KILLS)) {
          CHECK(grace ? out.taken == a : out.taken != a);  // the grace wins
          killed += grace;
        } else if (open == NULL && t <= seq->timeout_ms + 250u) {
          CHECK(grace ? out.taken == a : out.taken != a);
          passed += grace;
        }
      }
    }
  }
  CHECK(passed > 0);
  CHECK(killed > 0);
  // A game with the grace: followed by the hints it is won, idle it is lost.
  sim_t s;
  game_options_t o = options();
  o.fixed_order = true;
  o.grace_ms = 250u;
  sim_init(&s, &o);
  sim_start(&s, 0);
  sim_play(&s, follow_hint, GAME_TICKS);
  CHECK(s.over && s.won);
  CHECK_EQ(s.lives_lost, 0);
  sim_t idle;
  sim_init(&idle, &o);
  sim_start(&idle, 0);
  sim_play(&idle, NULL, GAME_TICKS);
  CHECK(idle.over && !idle.won);
  CHECK_EQ(idle.lives_lost, 5);
  printf("grace: %d late presses passed, %d of them over a death's window\n",
         passed + killed, killed);
}

// Retry: a player doing nothing dies five times in the first scene after
// the introduction, never in another; a continue replays that scene.
static void check_retry(void) {
  sim_t s;
  game_options_t o = options();
  o.retry = true;
  sim_init(&s, &o);
  sim_start(&s, 0);
  uint16_t first = GAME_SCENE_NONE;
  for (uint32_t i = 0; i < GAME_TICKS && !s.over; i++) {
    sim_tick(&s, 0);
    if (s.g.playing && s.g.scene != game_intro_scene) {
      if (first == GAME_SCENE_NONE) {
        first = s.g.scene;
      }
      CHECK_EQ(s.g.scene, first);
    }
  }
  CHECK(s.over && !s.won);
  CHECK_EQ(s.lives_lost, 5);
  CHECK_EQ(s.g.lost_in, first);
  game_out_t out;
  game_continue(&s.g, 0, &out);
  CHECK_EQ(s.g.scene, first);
  CHECK(out.seek || out.single_frame);
}

// A diagonal never kills: its two directions pressed inside a window where
// the diagonal leads to a death do not take that move.
static void check_diagonal_never_kills(void) {
  static const uint8_t dirs[4][2] = {{GAME_IN_UP, GAME_IN_LEFT},
                                     {GAME_IN_UP, GAME_IN_RIGHT},
                                     {GAME_IN_DOWN, GAME_IN_LEFT},
                                     {GAME_IN_DOWN, GAME_IN_RIGHT}};
  int checked = 0;
  for (uint16_t q = 0; q < game_sequence_count; q++) {
    const game_sequence_t *seq = &game_sequences[q];
    for (uint16_t i = 0; i < seq->action_count; i++) {
      uint16_t a = (uint16_t)(seq->first_action + i);
      const game_action_t *act = &game_actions[a];
      if (act->input < GAME_IN_UPLEFT || act->input > GAME_IN_DOWNRIGHT ||
          act->next == GAME_SEQ_NONE ||
          !(game_sequences[act->next].flags & GAME_SEQ_KILLS)) {
        continue;
      }
      game_t g;
      game_options_t o = options();
      game_init(&g, &o);
      g.playing = true;
      g.sequence = q;
      g.accepted = GAME_SEQ_NONE;
      g.offset_ms = 0;
      g.start_pending = false;
      const uint8_t *d = dirs[act->input - GAME_IN_UPLEFT];
      game_out_t out;
      game_tick(&g, act->from_ms, GAME_BIT(d[0]) | GAME_BIT(d[1]), 1, &out);
      CHECK(out.taken != a);
      checked++;
    }
  }
  CHECK(checked > 0);  // the table has some
}

// A diagonal pressed: a window open for both a diagonal and one of its
// directions takes the diagonal's move.
static void check_diagonal(void) {
  for (uint16_t q = 0; q < game_sequence_count; q++) {
    const game_sequence_t *seq = &game_sequences[q];
    for (uint16_t i = 0; i < seq->action_count; i++) {
      const game_action_t *a = &game_actions[seq->first_action + i];
      if (a->input != GAME_IN_UPLEFT) {
        continue;
      }
      game_t g;
      game_options_t o = options();
      game_init(&g, &o);
      g.playing = true;
      g.sequence = q;
      g.accepted = GAME_SEQ_NONE;
      g.offset_ms = 0;
      game_out_t out;
      uint32_t at = a->from_ms;
      game_tick(&g, at, 0, 1, &out);
      g.sequence = q;  // the tick may have moved on: put it back
      g.accepted = GAME_SEQ_NONE;
      g.pressed_before = 0;
      game_tick(&g, at, GAME_BIT(GAME_IN_UP) | GAME_BIT(GAME_IN_LEFT), 1, &out);
      if (g.accepted != GAME_SEQ_NONE) {
        CHECK_EQ(game_actions[g.accepted].input, GAME_IN_UPLEFT);
      }
      return;  // one is enough
    }
  }
  CHECK(false);  // the table has diagonals
}

int main(void) {
  check_attract();
  check_watch(true);
  check_watch(false);
  check_idle();
  check_hints();
  check_infinite();
  check_start_scene();
  check_hint_open();
  check_diagonal_never_kills();
  check_grace();
  check_retry();
  check_diagonal();
  TEST_END();
}
