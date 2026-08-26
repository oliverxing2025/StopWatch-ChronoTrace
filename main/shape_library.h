#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "handwriting.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHAPE_LIBRARY_COUNT 40
#define SHAPE_PICKER_PAGE_SIZE 12

const char *shape_library_name(uint8_t index, bool english);
uint8_t shape_library_color(uint8_t index);
bool shape_library_bitmap(uint8_t index, uint8_t bitmap[HANDWRITING_BYTES]);

#ifdef __cplusplus
}
#endif
