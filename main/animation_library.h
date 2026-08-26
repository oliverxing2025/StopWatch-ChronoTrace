#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "handwriting.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANIMATION_ARROW_COUNT 4
#define ANIMATION_LIBRARY_COUNT 8

const char *animation_library_name(uint8_t index, bool english);
bool animation_library_bitmap(uint8_t index, float phase,
                              uint8_t bitmap[HANDWRITING_BYTES]);
bool animation_library_is_continuous(uint8_t index);
// Returns the arrow centre's screen-space displacement in the fixed animation
// canvas. The simulator uses this after the arrow is fully assembled so the
// locked particle cohort translates rigidly instead of being remapped every
// time the raster crosses a bitmap row or column.
void animation_library_screen_offset(uint8_t index, float phase,
                                     float *x, float *y);

#ifdef __cplusplus
}
#endif
