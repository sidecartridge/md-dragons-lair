// units: rp/src/game_table.c
/* The game's table (game_table.h), generated from DirkSimple's game.lua:
 * every scene's sequences in one block, its entries inside it; every next
 * sequence (a move's, a timeout's) inside the same scene; every move's
 * window ordered and its input known; every sequence that seeks placed in
 * a clip, its start inside the clip, and one that runs past its clip
 * playing on into another; the deaths ending their scene (at once or after
 * the sequences they lead to); the rows' scenes
 * there; every playable scene's sequences reached from its entries; and
 * only the attract mode's two pictures not on the CD. */

#include <string.h>

#include "game_table.h"
#include "test.h"

static int scene_of(uint16_t seq) {
  for (int s = 0; s < game_scene_count; s++) {
    const game_scene_t *sc = &game_scenes[s];
    if (seq >= sc->first_sequence &&
        seq < sc->first_sequence + sc->sequence_count) {
      return s;
    }
  }
  return -1;
}

static void check_next(int scene, uint16_t next) {
  if (next != GAME_SEQ_NONE) {
    CHECK(next < game_sequence_count);
    CHECK_EQ(scene_of(next), scene);
  }
}

int main(void) {
  CHECK_EQ(game_scene_count, 40);
  CHECK_EQ(game_row_count, 13);
  CHECK(game_clip_count > 0 && game_clip_count < GAME_CLIP_NONE);

  // The scenes' blocks cover the sequences, in order, once.
  uint16_t next_first = 0;
  for (int s = 0; s < game_scene_count; s++) {
    const game_scene_t *sc = &game_scenes[s];
    CHECK_EQ(sc->first_sequence, next_first);
    next_first = (uint16_t)(sc->first_sequence + sc->sequence_count);
    CHECK(sc->start_alive != GAME_SEQ_NONE);
    check_next(s, sc->start_alive);
    check_next(s, sc->start_dead);
    check_next(s, sc->game_over);
    CHECK(sc->name != NULL && sc->title != NULL);
  }
  CHECK_EQ(next_first, game_sequence_count);

  int not_on_cd = 0;
  for (uint16_t q = 0; q < game_sequence_count; q++) {
    const game_sequence_t *seq = &game_sequences[q];
    int s = scene_of(q);
    check_next(s, seq->timeout_next);
    CHECK(seq->timeout_interrupt <= GAME_INT_GAME_OVER_COMPLETE);
    // Its moves.
    CHECK(seq->first_action + seq->action_count <= game_action_count);
    for (uint16_t a = 0; a < seq->action_count; a++) {
      const game_action_t *act = &game_actions[seq->first_action + a];
      CHECK(act->input < GAME_INPUTS);
      CHECK(act->from_ms <= act->to_ms);
      check_next(s, act->next);
    }
    // Its clip.
    if (seq->flags & GAME_SEQ_NOT_ON_CD) {
      not_on_cd++;
      CHECK_EQ(s, game_attract_scene);
      continue;
    }
    if (seq->clip != GAME_CLIP_NONE) {
      CHECK(seq->clip < game_clip_count);
      const game_clip_t *clip = &game_clips[seq->clip];
      CHECK(seq->frame < clip->frames);
      uint32_t frames = (seq->flags & GAME_SEQ_SINGLE_FRAME)
                            ? 1u
                            : (seq->timeout_ms * 25u + 999u) / 1000u;
      if (seq->then_clip != GAME_CLIP_NONE) {
        CHECK(seq->then_clip < game_clip_count);
        CHECK(seq->frame + frames > clip->frames);
      } else {
        CHECK(seq->frame + frames <= clip->frames + 3u);
      }
    } else {
      CHECK_EQ(seq->then_clip, GAME_CLIP_NONE);
    }
    // A death ends its scene, at once or after the sequences its timeouts
    // lead to (two deaths go on in a second one): the next scene is the
    // game's choice.
    if (seq->flags & GAME_SEQ_KILLS) {
      uint16_t at = q;
      int steps = 0;
      while (game_sequences[at].timeout_next != GAME_SEQ_NONE && steps < 8) {
        at = game_sequences[at].timeout_next;
        steps++;
      }
      CHECK(game_sequences[at].timeout_next == GAME_SEQ_NONE);
    }
  }
  CHECK_EQ(not_on_cd, 2);

  // The rows' scenes.
  for (int r = 0; r < game_row_count; r++) {
    for (int c = 0; c < GAME_ROW_SCENES; c++) {
      CHECK(game_rows[r][c] < game_scene_count);
      CHECK(game_rows[r][c] != game_attract_scene);
    }
  }

  // Every sequence of a playable scene is reached from its entries.
  for (int s = 0; s < game_scene_count; s++) {
    if (s == game_attract_scene) {
      continue;
    }
    const game_scene_t *sc = &game_scenes[s];
    uint8_t reached[64];
    CHECK(sc->sequence_count <= sizeof(reached));
    memset(reached, 0, sizeof(reached));
    uint16_t todo[64];
    int n = 0;
    const uint16_t entries[3] = {sc->start_alive, sc->start_dead,
                                 sc->game_over};
    for (int e = 0; e < 3; e++) {
      if (entries[e] != GAME_SEQ_NONE &&
          !reached[entries[e] - sc->first_sequence]) {
        reached[entries[e] - sc->first_sequence] = 1;
        todo[n++] = entries[e];
      }
    }
    while (n > 0) {
      const game_sequence_t *seq = &game_sequences[todo[--n]];
      uint16_t nexts[1 + 32];
      int k = 0;
      nexts[k++] = seq->timeout_next;
      for (uint16_t a = 0; a < seq->action_count && k < 33; a++) {
        nexts[k++] = game_actions[seq->first_action + a].next;
      }
      for (int i = 0; i < k; i++) {
        if (nexts[i] != GAME_SEQ_NONE &&
            !reached[nexts[i] - sc->first_sequence]) {
          reached[nexts[i] - sc->first_sequence] = 1;
          todo[n++] = nexts[i];
        }
      }
    }
    for (uint16_t i = 0; i < sc->sequence_count; i++) {
      if (!reached[i]) {
        fprintf(stderr, "%s.%s is never reached\n", sc->name,
                game_sequence_names[sc->first_sequence + i]);
      }
      CHECK(reached[i]);
    }
  }
  TEST_END();
}
