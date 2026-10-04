// units: rp/src/game.c rp/src/game_table.c
/* The game's engine over the game's table, against a simulated player that
 * starts the clips it is asked for and counts their time, 40 ms a tick:
 * the attract mode's loop and its start; watch mode winning the game (in
 * the fixed order and at random) without losing a life; a player doing
 * nothing losing five lives and reaching game over through the scene's
 * game over clip; a player following the hint (oldies mode's move) winning
 * too; infinite lives (the option and the arcade's secret) never ending;
 * a game started at a chosen scene; a continue after game over; the hint
 * showing an open window when one is; and a diagonal pressed taking a
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
      checked++;
    }
  }
  CHECK(checked > 0);
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
  check_diagonal();
  TEST_END();
}
