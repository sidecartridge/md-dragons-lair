/**
 * File: cadence.c
 * Description: A clip's pictures at the app's frame rate. See cadence.h.
 */

#include "cadence.h"

bool cadence_picture_rate(int code, uint32_t *num, uint32_t *den) {
  // ISO/IEC 11172-2, picture_rate.
  static const uint32_t rates[8][2] = {
      {24000u, 1001u}, {24u, 1u}, {25u, 1u}, {30000u, 1001u},
      {30u, 1u},       {50u, 1u}, {60000u, 1001u}, {60u, 1u},
  };
  if (code < 1 || code > 8) {
    return false;
  }
  *num = rates[code - 1][0];
  *den = rates[code - 1][1];
  return true;
}

uint32_t cadence_first_frame(uint32_t index, uint32_t num, uint32_t den) {
  // Picture `index` is shown at index * den / num seconds; output frame n
  // at n / CADENCE_FPS. The first n with n / CADENCE_FPS >= index * den /
  // num, rounded up in integers.
  uint64_t scaled = (uint64_t)index * den * CADENCE_FPS;
  return (uint32_t)((scaled + num - 1u) / num);
}
