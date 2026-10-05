/**
 * File: cadence.h
 * Description: A clip's pictures at the app's frame rate. Output frame n,
 *              shown at n / CADENCE_FPS seconds, shows the latest decoded
 *              picture whose display time is not after it, so a clip keeps
 *              its length and every picture shows as close to its own time
 *              as the frame rate allows. The game's clips run at
 *              30000 / 1001 pictures a second; with their B pictures left
 *              out, the I and P pictures cover the 25 frames of a second
 *              between them.
 *
 * Plain C, integers only: the cartridge and the PC tool count the same
 * frames.
 */

#ifndef CADENCE_H
#define CADENCE_H

#include <stdbool.h>
#include <stdint.h>

// The app's frame rate (its profile, PROFILE_25FPS).
#define CADENCE_FPS 25u

// The rate of an MPEG-1 picture_rate code, num / den pictures a second;
// false for a code MPEG-1 does not define.
bool cadence_picture_rate(int code, uint32_t *num, uint32_t *den);

// The first output frame that shows the picture at display position `index`
// (0: the clip's first) of a clip at num / den pictures a second. With
// `index` one past the clip's last display position, the clip's length in
// output frames: the last picture shown for its own time. (A clip can lack
// a display position: the game's first clip has 1,292 pictures, its last at
// position 1,292.)
uint32_t cadence_first_frame(uint32_t index, uint32_t num, uint32_t den);

#endif  // CADENCE_H
