/**
 * File: gameui.c
 * Description: The game on the ST. See gameui.h.
 */

#include "gameui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "debug.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "game.h"
#include "game_table.h"
#include "palette.h"
#include "pico/time.h"
#include "player.h"
#include "settings.h"

extern const struct FB_FONT font8x8;

enum {
  MODE_OFF = 0,
  MODE_ATTRACT,   // the movie, a line over it
  MODE_MENU,      // the start menu
  MODE_PICK,      // the scene to start at
  MODE_PLAY,      // a game
  MODE_PAUSE,     // the game stopped, its status shown
  MODE_CONTINUE,  // after game over: a countdown
  MODE_ENTRY,     // a high score's initials
  MODE_SCORES,    // the high scores
};

// Our own screens' colours.
enum { C_BACK = 0, C_TEXT, C_TITLE, C_GOOD, C_BAD, C_DIM, C_VALUE, C_ON };

static const uint16_t gameui_palette[16] = {
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 6, 1),
    PALETTE_RGB(2, 7, 2), PALETTE_RGB(7, 2, 2), PALETTE_RGB(3, 3, 4),
    PALETTE_RGB(3, 6, 7), PALETTE_RGB(7, 4, 1), PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 7, 7),
    PALETTE_RGB(7, 7, 7)};

#define MENU_IDLE_US 20000000u      // the menu back to the movie after this
#define CONTINUE_SECONDS 10
#define SCORES_US 10000000u
#define BANNER_US 3000000u          // the score and lives at a scene's start
#define HISCORES 10
#define HISCORE_LEN 10              // "DRK0300000"

static struct {
  gameui_host_t host;
  int mode;
  bool dirty;            // our screen to draw again
  uint32_t mode_t0;      // when the mode began
  uint32_t options;      // GAMEUI_OPT_*
  uint8_t lives;
  uint16_t start_scene;  // GAME_SCENE_NONE: from the start
  int pick;              // the picker's line: 0 the start, 1.. a scene
  // Input.
  uint32_t keys_held;    // GAME_BIT()s from the keyboard
  // The engine and its clip.
  game_t game;
  bool clip_on;          // a clip playing (or holding its end)
  bool held;             // a single picture held: the clock is the RP's
  bool ended;            // the clip played to its end, its last picture kept
  uint32_t base_ms;      // the clip time before the clip playing now
  uint32_t end_ms;
  uint32_t clock_t0;     // the RP's time when held or ended
  uint8_t clip;          // game_clips' index playing
  uint8_t then_clip;
  uint32_t paused_ms;    // the clip time at a pause
  uint32_t paused_frame;
  uint16_t scene_seen;   // the last scene started, for its banner
  uint32_t scene_t0;
  // After a game.
  uint32_t last_score;
  char initials[4];
  int initial;           // the letter being entered
  char scores[HISCORES][HISCORE_LEN + 1];
} s_ui;

// --- Text ---------------------------------------------------------------------------

static void text(int col, int row, int color, const char *str) {
  font_set_color((unsigned)color);
  font_move((unsigned)(col * 8), (unsigned)(row * 8));
  font_print(str);
}

static void textf(int col, int row, int color, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void textf(int col, int row, int color, const char *fmt, ...) {
  char buf[41];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  text(col, row, color, buf);
}

static void centred(int row, int color, const char *str) {
  text((40 - (int)strlen(str)) / 2, row, color, str);
}

// The font at twice its size: the title.
static void text2x(int x, int y, int color, const char *str) {
  for (; *str != '\0'; str++, x += 16) {
    int ch = (unsigned char)*str;
    if (ch < font8x8.first_char || ch >= font8x8.first_char + font8x8.num_chars) {
      continue;
    }
    const unsigned char *glyph = &font8x8.data[(ch - font8x8.first_char) * 8];
    for (int r = 0; r < 8; r++) {
      for (int c = 0; c < 8; c++) {
        if (glyph[r] & (1u << c)) {
          fb_fill_rect(x + 2 * c, y + 2 * r, 2, 2, color);
        }
      }
    }
  }
}

static void title(void) {
  fb_chunked_clear(C_BACK);
  font_set_font(&font8x8);
  text2x((320 - 16 * 13) / 2, 6, C_TITLE, "DRAGON'S LAIR");
}

// --- Settings -----------------------------------------------------------------------

static int setting_int(const char *key, int dflt) {
  SettingsConfigEntry *e = settings_find_entry(aconfig_getContext(), key);
  return e != NULL ? atoi(e->value) : dflt;
}

static void settings_load(void) {
  s_ui.options = (uint32_t)setting_int(ACONFIG_PARAM_GAME_OPTIONS, 48);
  int lives = setting_int(ACONFIG_PARAM_GAME_LIVES, 5);
  s_ui.lives = (uint8_t)(lives >= 1 && lives <= 5 ? lives : 5);
  s_ui.start_scene = GAME_SCENE_NONE;
  SettingsConfigEntry *e =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_GAME_START);
  if (e != NULL && e->value[0] != '\0') {
    for (uint16_t i = 0; i < game_scene_count; i++) {
      if (strcmp(game_scenes[i].name, e->value) == 0) {
        s_ui.start_scene = i;
      }
    }
  }
  const char *keys[2] = {ACONFIG_PARAM_HISCORES1, ACONFIG_PARAM_HISCORES2};
  for (int k = 0; k < 2; k++) {
    e = settings_find_entry(aconfig_getContext(), keys[k]);
    for (int i = 0; i < HISCORES / 2; i++) {
      char *slot = s_ui.scores[k * (HISCORES / 2) + i];
      if (e != NULL && strlen(e->value) >= (size_t)(i + 1) * HISCORE_LEN) {
        memcpy(slot, e->value + i * HISCORE_LEN, HISCORE_LEN);
      } else {
        memcpy(slot, "---0000000", HISCORE_LEN);
      }
      slot[HISCORE_LEN] = '\0';
    }
  }
}

static void settings_store(void) {
  SettingsContext *ctx = aconfig_getContext();
  settings_put_integer(ctx, ACONFIG_PARAM_GAME_OPTIONS, (int)s_ui.options);
  settings_put_integer(ctx, ACONFIG_PARAM_GAME_LIVES, s_ui.lives);
  settings_put_string(ctx, ACONFIG_PARAM_GAME_START,
                      s_ui.start_scene != GAME_SCENE_NONE
                          ? game_scenes[s_ui.start_scene].name
                          : "");
  char half[HISCORES / 2 * HISCORE_LEN + 1];
  for (int k = 0; k < 2; k++) {
    half[0] = '\0';
    for (int i = 0; i < HISCORES / 2; i++) {
      strcat(half, s_ui.scores[k * (HISCORES / 2) + i]);
    }
    settings_put_string(ctx, k == 0 ? ACONFIG_PARAM_HISCORES1
                                    : ACONFIG_PARAM_HISCORES2,
                        half);
  }
  settings_save(ctx, true);
}

static uint32_t hiscore_of(int i) { return (uint32_t)atol(s_ui.scores[i] + 3); }

// --- The hints (oldies mode) -----------------------------------------------------
//
// The move that passes, as DirkSimple places its hints: an arrow at the
// screen's edge for a direction (at a corner for a diagonal), the sword in
// the middle. Drawn in the picture's brightest colour every other pixel,
// with a solid black edge, so that the scene shows through; only the edge
// while the move is still to come.

#define HINT_PX 2  // each of the 16x16 shapes' pixels
static const uint16_t arrow_up[16] = {
    0x0180, 0x03C0, 0x07E0, 0x0FF0, 0x1FF8, 0x3FFC, 0x7FFE, 0xFFFF,
    0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0};
static const uint16_t arrow_upright[16] = {
    0x00FF, 0x007F, 0x003F, 0x003F, 0x007F, 0x00FF, 0x01FB, 0x03F3,
    0x07E1, 0x0FC0, 0x1F80, 0x3F00, 0x7E00, 0xFC00, 0xF800, 0x7000};
static const uint16_t sword[16] = {
    0x0180, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0,
    0x03C0, 0x03C0, 0x3FFC, 0x3FFC, 0x0180, 0x0180, 0x03C0, 0x03C0};

// Pixel (x, y) of `shape` turned `quarter` quarter turns clockwise.
static bool shape_at(const uint16_t *shape, int quarter, int x, int y) {
  for (int q = 0; q < quarter; q++) {
    int t = x;
    x = y;
    y = 15 - t;
  }
  return (shape[y] >> (15 - x)) & 1u;
}

static void draw_shape(const uint16_t *shape, int quarter, int x0, int y0,
                       uint8_t bright, bool filled) {
  for (int y = -1; y <= 16; y++) {
    for (int x = -1; x <= 16; x++) {
      bool in = x >= 0 && x < 16 && y >= 0 && y < 16 &&
                shape_at(shape, quarter, x, y);
      bool edge = false;
      if (!in) {
        for (int d = 0; d < 4 && !edge; d++) {
          int nx = x + (d == 0) - (d == 1);
          int ny = y + (d == 2) - (d == 3);
          edge = nx >= 0 && nx < 16 && ny >= 0 && ny < 16 &&
                 shape_at(shape, quarter, nx, ny);
        }
      }
      int px = x0 + x * HINT_PX;
      int py = y0 + y * HINT_PX;
      if (edge) {
        fb_fill_rect(px, py, HINT_PX, HINT_PX, 0);
      } else if (in) {
        bool border = false;
        for (int d = 0; d < 4 && !border; d++) {
          int nx = x + (d == 0) - (d == 1);
          int ny = y + (d == 2) - (d == 3);
          border = !(nx >= 0 && nx < 16 && ny >= 0 && ny < 16 &&
                     shape_at(shape, quarter, nx, ny));
        }
        if (border || (filled && ((x + y) & 1) == 0)) {
          fb_fill_rect(px, py, HINT_PX, HINT_PX, bright);
        }
      }
    }
  }
}

static void draw_hint(uint8_t bright) {
  uint8_t input;
  bool open;
  if (!game_hint(&s_ui.game, &input, &open)) {
    return;
  }
  const int w = 16 * HINT_PX;
  const int left = 6, top = 6, right = 320 - w - 6, bottom = 200 - w - 6;
  const int mx = (320 - w) / 2, my = (200 - w) / 2;
  switch (input) {
    case GAME_IN_UP: draw_shape(arrow_up, 0, mx, top, bright, open); break;
    case GAME_IN_RIGHT: draw_shape(arrow_up, 1, right, my, bright, open); break;
    case GAME_IN_DOWN: draw_shape(arrow_up, 2, mx, bottom, bright, open); break;
    case GAME_IN_LEFT: draw_shape(arrow_up, 3, left, my, bright, open); break;
    case GAME_IN_UPRIGHT: draw_shape(arrow_upright, 0, right, top, bright, open); break;
    case GAME_IN_DOWNRIGHT: draw_shape(arrow_upright, 1, right, bottom, bright, open); break;
    case GAME_IN_DOWNLEFT: draw_shape(arrow_upright, 2, left, bottom, bright, open); break;
    case GAME_IN_UPLEFT: draw_shape(arrow_upright, 3, left, top, bright, open); break;
    case GAME_IN_ACTION: draw_shape(sword, 0, mx, my, bright, open); break;
    default: break;
  }
}

// --- Over the pictures ---------------------------------------------------------------

static void line_over(int y, uint8_t bright, const char *str) {
  int w = 8 * (int)strlen(str) + 8;
  int x = (320 - w) / 2;
  fb_fill_rect(x, y - 2, w, 12, 0);
  font_set_font(&font8x8);
  font_set_color(bright);
  font_move((unsigned)(x + 4), (unsigned)y);
  font_print(str);
}

static void overlay(uint32_t frame, uint8_t bright) {
  (void)frame;
  if (s_ui.mode == MODE_ATTRACT) {
    line_over(186, bright, "FIRE: START   M: MENU   T: SCORES");
    return;
  }
  if (s_ui.mode != MODE_PLAY) {
    return;
  }
  if ((s_ui.options & GAMEUI_OPT_HINTS) && !s_ui.game.options.watch) {
    draw_hint(bright);
  }
  if (time_us_32() - s_ui.scene_t0 < BANNER_US) {
    char line[41];
    if (s_ui.options & GAMEUI_OPT_INFINITE || s_ui.game.infinite) {
      snprintf(line, sizeof(line), "SCORE %06lu   LIVES -",
               (unsigned long)s_ui.game.score);
    } else {
      snprintf(line, sizeof(line), "SCORE %06lu   LIVES %u",
               (unsigned long)s_ui.game.score, (unsigned)s_ui.game.lives);
    }
    line_over(186, bright, line);
  }
}

// --- The engine's clip --------------------------------------------------------------

static void start_clip(uint8_t clip, uint32_t frame, bool paused) {
  char path[48];
  s_ui.host.clip_path(path, sizeof(path), game_clips[clip].name);
  s_ui.clip = clip;
  s_ui.clip_on = true;
  s_ui.ended = false;
  int r = player_start(path, frame, paused);
  if (r < 0) {
    DPRINTF("Game: %s from %lu failed (%d)\n", path, (unsigned long)frame, r);
    s_ui.ended = true;  // the clock goes on: the engine moves past it
    s_ui.end_ms = s_ui.base_ms;
    s_ui.clock_t0 = time_us_32();
  }
}

// The clip's time for the engine: the sound heard, or the RP's clock while
// a picture is held or after the clip's end.
static uint32_t clip_ms(void) {
  uint32_t now = time_us_32();
  if (s_ui.held) {
    return (now - s_ui.clock_t0) / 1000u;
  }
  if (s_ui.ended) {
    return s_ui.end_ms + (now - s_ui.clock_t0) / 1000u;
  }
  return s_ui.base_ms + player_heard_ms();
}

static void enter_mode(int mode) {
  s_ui.mode = mode;
  s_ui.mode_t0 = time_us_32();
  s_ui.dirty = true;
  if (mode != MODE_ATTRACT && mode != MODE_PLAY) {
    if (player_active()) {
      player_close(PLAYER_STOPPED);
    }
    palette_set(gameui_palette);
  }
}

static void to_scores(void);

static void game_over(bool won) {
  s_ui.last_score = s_ui.game.score;
  DPRINTF("Game over: %s, score %lu\n", won ? "won" : "lost",
          (unsigned long)s_ui.last_score);
  if (!won && (s_ui.options & GAMEUI_OPT_CONTINUE)) {
    enter_mode(MODE_CONTINUE);
    return;
  }
  to_scores();
}

static void apply(const game_out_t *out) {
  if (out->seek) {
    s_ui.held = false;
    s_ui.base_ms = 0;
    s_ui.then_clip = out->then_clip;
    start_clip(out->clip, out->frame, false);
  } else if (out->single_frame) {
    s_ui.held = true;
    s_ui.clock_t0 = time_us_32();
    s_ui.then_clip = GAME_CLIP_NONE;
    if (out->clip == GAME_CLIP_NONE) {
      // The arcade's "insert coins": the start menu instead.
      if (s_ui.mode == MODE_ATTRACT) {
        enter_mode(MODE_MENU);
      }
    } else {
      start_clip(out->clip, out->frame, true);
    }
  }
  if (out->started) {
    s_ui.mode = MODE_PLAY;
    s_ui.scene_seen = GAME_SCENE_NONE;
  }
  if (out->game_over && s_ui.mode == MODE_PLAY) {
    game_over(out->won);
  }
  if (s_ui.game.playing && s_ui.game.scene != s_ui.scene_seen) {
    s_ui.scene_seen = s_ui.game.scene;
    s_ui.scene_t0 = time_us_32();
    DPRINTF("Game: scene %s, score %lu, lives %u\n",
            game_scenes[s_ui.game.scene].name,
            (unsigned long)s_ui.game.score, (unsigned)s_ui.game.lives);
  }
}

static uint32_t held_inputs(void) {
  ikbd_joystick_t joy;
  ikbd_read_joystick(1, &joy);
  uint32_t held = s_ui.keys_held;
  if (joy.state & IKBD_JOY_UP) held |= GAME_BIT(GAME_IN_UP);
  if (joy.state & IKBD_JOY_DOWN) held |= GAME_BIT(GAME_IN_DOWN);
  if (joy.state & IKBD_JOY_LEFT) held |= GAME_BIT(GAME_IN_LEFT);
  if (joy.state & IKBD_JOY_RIGHT) held |= GAME_BIT(GAME_IN_RIGHT);
  if (joy.state & IKBD_JOY_FIRE) held |= GAME_BIT(GAME_IN_ACTION);
  return held;
}

static game_options_t options_for_game(void) {
  game_options_t o;
  memset(&o, 0, sizeof(o));
  o.starting_lives = s_ui.lives;
  o.infinite_lives = (s_ui.options & GAMEUI_OPT_INFINITE) != 0;
  o.watch = (s_ui.options & GAMEUI_OPT_WATCH) != 0;
  o.fixed_order = (s_ui.options & GAMEUI_OPT_FIXED) != 0;
  o.start_scene = s_ui.start_scene;
  return o;
}

static void attract(void) {
  game_options_t o = options_for_game();
  game_init(&s_ui.game, &o);
  s_ui.held = false;
  s_ui.ended = false;
  s_ui.base_ms = 0;
  s_ui.mode = MODE_ATTRACT;
  s_ui.mode_t0 = time_us_32();
  game_out_t out;
  game_tick(&s_ui.game, 0, 0, time_us_32(), &out);
  apply(&out);
}

static void start_game(void) {
  game_options_t o = options_for_game();
  game_init(&s_ui.game, &o);
  s_ui.held = false;
  s_ui.ended = false;
  s_ui.base_ms = 0;
  game_out_t out;
  game_start(&s_ui.game, held_inputs(), &out);
  s_ui.mode = MODE_PLAY;
  apply(&out);
}

// The engine and its clip, a pass.
static void run_engine(void) {
  if (s_ui.clip_on && !s_ui.ended) {
    int r = player_frame();
    if (r <= 0) {
      uint32_t heard = player_heard_ms();
      if (r == 0 && s_ui.then_clip != GAME_CLIP_NONE) {
        // On into the next clip, as the laserdisc plays on.
        s_ui.base_ms += heard;
        uint8_t next = s_ui.then_clip;
        s_ui.then_clip = GAME_CLIP_NONE;
        start_clip(next, 0, false);
      } else {
        s_ui.ended = true;
        s_ui.end_ms = s_ui.base_ms + heard;
        s_ui.clock_t0 = time_us_32();
      }
    }
  } else if (s_ui.held && player_active()) {
    player_frame();  // the held picture on the screen
  }
  game_out_t out;
  game_tick(&s_ui.game, clip_ms(), held_inputs(), time_us_32(), &out);
  apply(&out);
}

// --- Our screens --------------------------------------------------------------------

static const char *on_off(uint32_t bit) {
  return (s_ui.options & bit) ? "ON" : "OFF";
}

static void draw_menu(void) {
  title();
  centred(4, C_DIM, "SIDECARTRIDGE MULTI-DEVICE");
  centred(6, C_GOOD, "FIRE OR SPACE: START");
  int r = 8;
  textf(3, r++, C_TEXT, "H  OLDIES MODE (HINTS)      %5s", on_off(GAMEUI_OPT_HINTS));
  textf(3, r++, C_TEXT, "I  INFINITE LIVES           %5s", on_off(GAMEUI_OPT_INFINITE));
  textf(3, r++, C_TEXT, "L  LIVES                    %5u", (unsigned)s_ui.lives);
  textf(3, r++, C_TEXT, "O  SCENE ORDER             %6s",
        (s_ui.options & GAMEUI_OPT_FIXED) ? "FIXED" : "ARCADE");
  textf(3, r++, C_TEXT, "W  WATCH MODE               %5s", on_off(GAMEUI_OPT_WATCH));
  textf(3, r++, C_TEXT, "C  CONTINUE                 %5s", on_off(GAMEUI_OPT_CONTINUE));
  textf(3, r++, C_TEXT, "S  INPUT SOUNDS             %5s", on_off(GAMEUI_OPT_SOUNDS));
  char from[24];
  snprintf(from, sizeof(from), "%.22s",
           s_ui.start_scene == GAME_SCENE_NONE
               ? "THE START"
               : game_scenes[s_ui.start_scene].title);
  textf(3, r++, C_TEXT, "P  START AT");
  textf(36 - (int)strlen(from), r - 1, C_VALUE, "%s", from);
  r++;
  text(3, r++, C_DIM, "T  HIGH SCORES     B  THE BENCH");
  textf(3, 22, C_DIM, "BEST  %.3s  %lu", s_ui.scores[0],
        (unsigned long)hiscore_of(0));
  centred(24, C_DIM, "IN GAME: P PAUSE, U/D VOLUME");
}

static void draw_pick(void) {
  title();
  centred(4, C_TEXT, "START AT");
  // The start, then every playable scene, two columns of 20.
  int n = 0;
  for (uint16_t i = 0; i <= game_scene_count; i++) {
    const char *name;
    if (i == 0) {
      name = "THE START";
    } else if (i - 1 == game_attract_scene || i - 1 == game_intro_scene) {
      continue;
    } else {
      name = game_scenes[i - 1].title;
    }
    int col = n < 20 ? 0 : 20;
    int row = 6 + n % 20;
    if (row > 23) {
      break;
    }
    bool sel = n == s_ui.pick;
    char shown[20];
    snprintf(shown, sizeof(shown), "%.18s", name);
    text(col, row, sel ? C_TITLE : C_VALUE, sel ? ">" : " ");
    text(col + 1, row, sel ? C_TITLE : C_VALUE, shown);
    n++;
  }
  centred(24, C_DIM, "RETURN: CHOOSE   SPACE: BACK");
}

// The picker's lines: the start, then the playable scenes in the table's
// order. Returns the scene of line `line` (GAME_SCENE_NONE for the start).
static uint16_t pick_scene(int line, int *count) {
  int n = 0;
  uint16_t found = GAME_SCENE_NONE;
  for (uint16_t i = 0; i < game_scene_count; i++) {
    if (i == game_attract_scene || i == game_intro_scene) {
      continue;
    }
    n++;
    if (n == line) {
      found = i;
    }
  }
  if (count != NULL) {
    *count = n + 1;
  }
  return found;
}

static void draw_pause(void) {
  title();
  centred(6, C_TITLE, "PAUSED");
  centred(9, C_TEXT, game_scenes[s_ui.game.scene].title);
  textf(12, 12, C_TEXT, "SCORE  %06lu", (unsigned long)s_ui.game.score);
  if ((s_ui.options & GAMEUI_OPT_INFINITE) || s_ui.game.infinite) {
    text(12, 13, C_TEXT, "LIVES  INFINITE");
  } else {
    textf(12, 13, C_TEXT, "LIVES  %u", (unsigned)s_ui.game.lives);
  }
  centred(20, C_DIM, "P: GO ON");
}

static void draw_continue(void) {
  title();
  centred(7, C_TEXT, "GAME OVER");
  textf(12, 10, C_TEXT, "SCORE  %06lu", (unsigned long)s_ui.last_score);
  uint32_t gone = (time_us_32() - s_ui.mode_t0) / 1000000u;
  int left = CONTINUE_SECONDS - (int)gone;
  textf(11, 14, C_TITLE, "CONTINUE?  %d", left > 0 ? left : 0);
  centred(17, C_DIM, "FIRE OR SPACE");
}

static void draw_entry(void) {
  title();
  centred(6, C_GOOD, "A NEW HIGH SCORE");
  textf(12, 9, C_TEXT, "SCORE  %06lu", (unsigned long)s_ui.last_score);
  for (int i = 0; i < 3; i++) {
    char c[2] = {s_ui.initials[i], '\0'};
    text2x(128 + i * 24, 104, i == s_ui.initial ? C_TITLE : C_TEXT, c);
  }
  centred(18, C_DIM, "UP/DOWN: LETTER  LEFT/RIGHT: MOVE");
  centred(19, C_DIM, "FIRE OR RETURN: DONE");
}

static void draw_scores(void) {
  title();
  centred(4, C_TEXT, "HIGH SCORES");
  for (int i = 0; i < HISCORES; i++) {
    textf(11, 7 + i * 1, i == 0 ? C_TITLE : C_VALUE, "%2d  %.3s  %07lu", i + 1,
          s_ui.scores[i], (unsigned long)hiscore_of(i));
  }
  centred(22, C_DIM, "FIRE: START");
}

static void to_scores(void) {
  int place = HISCORES;
  for (int i = HISCORES - 1; i >= 0; i--) {
    if (s_ui.last_score > hiscore_of(i)) {
      place = i;
    }
  }
  if (place < HISCORES && s_ui.last_score > 0 &&
      !(s_ui.options & GAMEUI_OPT_WATCH)) {
    memcpy(s_ui.initials, "AAA", 4);
    s_ui.initial = 0;
    enter_mode(MODE_ENTRY);
    return;
  }
  enter_mode(MODE_SCORES);
}

static void entry_done(void) {
  int place = HISCORES;
  for (int i = HISCORES - 1; i >= 0; i--) {
    if (s_ui.last_score > hiscore_of(i)) {
      place = i;
    }
  }
  if (place < HISCORES) {
    memmove(s_ui.scores[place + 1], s_ui.scores[place],
            (size_t)(HISCORES - 1 - place) * sizeof(s_ui.scores[0]));
    snprintf(s_ui.scores[place], sizeof(s_ui.scores[place]), "%.3s%07lu",
             s_ui.initials, (unsigned long)(s_ui.last_score % 10000000u));
    settings_store();
  }
  enter_mode(MODE_SCORES);
}

// --- Public -------------------------------------------------------------------------

void gameui_start(const gameui_host_t *host) {
  memset(&s_ui, 0, sizeof(s_ui));
  s_ui.host = *host;
  settings_load();
  s_ui.host.card_fast(true);
  ikbd_set_input_mode(IKBD_INPUT_MOUSE_JOY1);
  player_set_overlay(overlay);
  DPRINTF("Game: the attract mode\n");
  attract();
}

void gameui_stop(void) {
  if (s_ui.mode == MODE_OFF) {
    return;
  }
  if (player_active()) {
    player_close(PLAYER_STOPPED);
  }
  player_set_overlay(NULL);
  s_ui.host.card_fast(false);
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  s_ui.mode = MODE_OFF;
}

bool gameui_active(void) { return s_ui.mode != MODE_OFF; }

void gameui_frame(void) {
  switch (s_ui.mode) {
    case MODE_ATTRACT:
    case MODE_PLAY:
      run_engine();
      return;
    case MODE_MENU:
      if (time_us_32() - s_ui.mode_t0 > MENU_IDLE_US) {
        attract();  // back to the movie
        return;
      }
      break;
    case MODE_CONTINUE:
      if (time_us_32() - s_ui.mode_t0 > CONTINUE_SECONDS * 1000000u) {
        to_scores();
      } else {
        s_ui.dirty = true;  // the countdown
      }
      break;
    case MODE_SCORES:
      if (time_us_32() - s_ui.mode_t0 > SCORES_US) {
        attract();
        return;
      }
      break;
    default:
      break;
  }
  if (s_ui.dirty) {
    s_ui.dirty = false;
    switch (s_ui.mode) {
      case MODE_MENU: draw_menu(); break;
      case MODE_PICK: draw_pick(); break;
      case MODE_PAUSE: draw_pause(); break;
      case MODE_CONTINUE: draw_continue(); break;
      case MODE_ENTRY: draw_entry(); break;
      case MODE_SCORES: draw_scores(); break;
      default: break;
    }
  }
  fb_publish();  // every pass: an ST that boots takes the frame it finds
}

// The keys held, for the engine: the cursor keys and space (the sword).
static void track_keys(const ikbd_key_event_t *key) {
  uint32_t bit = 0;
  switch (key->scancode) {
    case 0x48: bit = GAME_BIT(GAME_IN_UP); break;
    case 0x50: bit = GAME_BIT(GAME_IN_DOWN); break;
    case 0x4B: bit = GAME_BIT(GAME_IN_LEFT); break;
    case 0x4D: bit = GAME_BIT(GAME_IN_RIGHT); break;
    case 0x39: bit = GAME_BIT(GAME_IN_ACTION); break;
    default: return;
  }
  if (key->is_press) {
    s_ui.keys_held |= bit;
  } else {
    s_ui.keys_held &= ~bit;
  }
}

static void toggle(uint32_t bit) {
  s_ui.options ^= bit;
  settings_store();
  s_ui.dirty = true;
}

void gameui_key(const ikbd_key_event_t *key) {
  track_keys(key);
  if (!key->is_press) {
    return;
  }
  uint8_t sc = key->scancode;
  bool fire = sc == 0x39 || sc == 0x1C || sc == 0x72;  // space, return
  switch (s_ui.mode) {
    case MODE_ATTRACT:
      if (fire) {
        start_game();
      } else if (sc == 0x32) {  // M
        enter_mode(MODE_MENU);
      } else if (sc == 0x14) {  // T
        enter_mode(MODE_SCORES);
      }
      break;
    case MODE_MENU:
      s_ui.mode_t0 = time_us_32();  // still there
      switch (sc) {
        case 0x23: toggle(GAMEUI_OPT_HINTS); break;     // H
        case 0x17: toggle(GAMEUI_OPT_INFINITE); break;  // I
        case 0x18: toggle(GAMEUI_OPT_FIXED); break;     // O
        case 0x11: toggle(GAMEUI_OPT_WATCH); break;     // W
        case 0x2E: toggle(GAMEUI_OPT_CONTINUE); break;  // C
        case 0x1F: toggle(GAMEUI_OPT_SOUNDS); break;    // S
        case 0x26:                                      // L
          s_ui.lives = (uint8_t)(s_ui.lives % 5 + 1);
          settings_store();
          s_ui.dirty = true;
          break;
        case 0x19:  // P
          s_ui.pick = 0;
          enter_mode(MODE_PICK);
          break;
        case 0x14: enter_mode(MODE_SCORES); break;  // T
        case 0x30:  // B: the bench
          gameui_stop();
          s_ui.host.to_bench();
          break;
        default:
          if (fire) {
            start_game();
          }
          break;
      }
      break;
    case MODE_PICK: {
      int count;
      pick_scene(0, &count);
      if (sc == 0x48) s_ui.pick = s_ui.pick > 0 ? s_ui.pick - 1 : count - 1;
      if (sc == 0x50) s_ui.pick = s_ui.pick + 1 < count ? s_ui.pick + 1 : 0;
      if (sc == 0x4B) s_ui.pick = s_ui.pick >= 20 ? s_ui.pick - 20 : 0;
      if (sc == 0x4D) s_ui.pick = s_ui.pick + 20 < count ? s_ui.pick + 20 : count - 1;
      if (sc == 0x1C || sc == 0x72) {
        s_ui.start_scene = pick_scene(s_ui.pick, NULL);
        settings_store();
        enter_mode(MODE_MENU);
      } else if (sc == 0x39) {
        enter_mode(MODE_MENU);
      }
      s_ui.dirty = true;
      break;
    }
    case MODE_PLAY:
      if (sc == 0x19) {  // P: pause
        s_ui.paused_ms = clip_ms();
        s_ui.paused_frame = player_frame_shown();
        enter_mode(MODE_PAUSE);
      } else if (sc == 0x16) {  // U
        player_volume(+1);
      } else if (sc == 0x20) {  // D
        player_volume(-1);
      }
      break;
    case MODE_PAUSE:
      if (sc == 0x19) {  // P: on again, from the frame shown
        s_ui.mode = MODE_PLAY;
        if (s_ui.held) {
          s_ui.clock_t0 = time_us_32() - s_ui.paused_ms * 1000u;
          start_clip(s_ui.clip, s_ui.paused_frame, true);
        } else if (!s_ui.ended) {
          s_ui.base_ms = s_ui.paused_ms;
          start_clip(s_ui.clip, s_ui.paused_frame, false);
        } else {
          s_ui.clock_t0 = time_us_32();
          s_ui.end_ms = s_ui.paused_ms;
        }
      }
      break;
    case MODE_CONTINUE:
      if (fire) {
        game_out_t out;
        game_continue(&s_ui.game, &out);
        s_ui.mode = MODE_PLAY;
        s_ui.held = false;
        s_ui.ended = false;
        s_ui.base_ms = 0;
        apply(&out);
      }
      break;
    case MODE_ENTRY:
      if (sc == 0x48) {
        s_ui.initials[s_ui.initial] =
            s_ui.initials[s_ui.initial] == 'Z' ? 'A' : s_ui.initials[s_ui.initial] + 1;
      } else if (sc == 0x50) {
        s_ui.initials[s_ui.initial] =
            s_ui.initials[s_ui.initial] == 'A' ? 'Z' : s_ui.initials[s_ui.initial] - 1;
      } else if (sc == 0x4D && s_ui.initial < 2) {
        s_ui.initial++;
      } else if (sc == 0x4B && s_ui.initial > 0) {
        s_ui.initial--;
      } else if (fire) {
        entry_done();
      }
      s_ui.dirty = true;
      break;
    case MODE_SCORES:
      if (fire) {
        start_game();
      } else {
        attract();
      }
      break;
    default:
      break;
  }
}
