/**
 * File: player.h
 * Description: A clip file played on the ST with its sound: the pictures on
 *              the screen in step with the sound the ST plays, from any
 *              frame, with a pause and a volume. The bench's clip list and
 *              the game both play through it.
 *
 * The sound leads: the reader pushes each record's samples into a ring the
 * audio takes as a stream (audio_set_pcm_stream()), and the samples the ST
 * has played (audio_source_played()) are the clock. A picture is decoded
 * into fb_chunked_buffer and converted (fb_publish_prepare()) while the one
 * before waits, so the reader runs two records ahead of the screen. A
 * converted picture goes on the ST (fb_publish_commit()) when the sound
 * reaches its frame less the screen's own delay (the ST takes it at its
 * next frame, copies it and flips); one more than a frame late is dropped.
 * A clip's sound starts once 120 ms of it are read; its first picture goes
 * on the ST meanwhile. A pause gives the audio no samples: the output holds
 * its last one and the clock stops with it, the pictures too.
 *
 * The caller sets the card's speed and the IKBD's mode around it.
 */

#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#define PLAYER_STOPPED 1  // a result: stopped before its end

// The last clip played, readable over SWD as well as on the screen. Times
// in microseconds: minimum, mean and maximum, a record (reading, decoding)
// or a picture (converting and committing). The drift: how far from its
// due sample count the sound was when a picture went on the ST, at worst.
typedef struct {
  int result;  // 0: to the end, PLAYER_STOPPED, or negative: an error
  uint32_t frames;    // records read
  uint32_t pictures;  // shown
  uint32_t dropped;   // more than a frame late
  uint32_t pieces;
  uint32_t read_us[3];
  uint32_t decode_us[3];
  uint32_t publish_us[3];
  uint32_t total_ms;
  int32_t drift_us[2];  // the earliest and the latest commit, against due
                        // (the first picture waits for the sound to begin)
  int volume_db;
  uint32_t underruns;   // of the sound until its last sample (audioUnderruns)
  uint32_t late_slices; // audioLateSlices, the same
  int32_t lead_ms;      // the sound read ahead of what was heard, at least
                        // (from the first picture shown)
  uint32_t start_ms;    // from the start to the first sample heard
  uint32_t open_us;     // the file opened and its header read
  uint32_t seek_us;     // to the frame asked for: the key, then the records
  uint32_t slow_reads;  // records read in over 10 ms
  uint32_t late;        // pictures committed over 40 ms late
} play_results_t;

extern play_results_t playResults;

// Drawn over each picture just before it is converted: the picture's frame
// in the clip and its palette's brightest entry (its black is entry 0).
typedef void (*player_overlay_fn)(uint32_t frame, uint8_t bright);

// Starts the clip file at `path` from `frame` (its first picture shown at
// once; paused: the sound held until player_pause(false)). 0, or a
// negative error (-100: no memory; CLIPPLAY_ERR_*: the file).
int player_start(const char *path, uint32_t frame, bool paused);

// Every pass of the main loop while a clip plays: 1 while it plays, 0 once
// its last sample is heard (its last picture stays), or a negative error.
int player_frame(void);

// Stops the clip (playResults keeps how it went), the sound silent.
void player_close(int result);

bool player_active(void);
void player_pause(bool paused);
bool player_paused(void);

// The clip's sound heard since its start frame, in milliseconds.
uint32_t player_heard_ms(void);
// The clip's frame on the screen; its first frame; its frames.
uint32_t player_frame_shown(void);
uint32_t player_first_frame(void);
uint32_t player_frames(void);
// The last record's reading and decoding and the last picture's
// publishing, in microseconds; the card's rate so far, in KB/s.
void player_last_times(uint32_t us[3]);
uint32_t player_card_kbs(void);

// The volume by 3 dB steps (`step` +1 or -1), -18 to +18 dB, shown on the
// pictures for 1.5 s and kept for each sound output when the clip stops.
void player_volume(int step);

// The overlay drawn over the pictures (NULL: none).
void player_set_overlay(player_overlay_fn fn);

#endif  // PLAYER_H
