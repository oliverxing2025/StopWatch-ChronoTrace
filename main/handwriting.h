#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HANDWRITING_MAX_GLYPHS 6
#define HANDWRITING_W 64
#define HANDWRITING_H 64
#define HANDWRITING_BYTES (HANDWRITING_W * HANDWRITING_H / 8)
#define HANDWRITING_COLOR_COUNT 8
#define HANDWRITING_DEFAULT_COLOR 0

typedef enum {
    HANDWRITING_RESULT_NONE = 0,
    HANDWRITING_RESULT_SAVED,
    HANDWRITING_RESULT_CANCELED,
} handwriting_result_t;

typedef struct {
    bool active;
    uint32_t revision;
    uint8_t page;
    uint8_t committed;
    uint8_t color;
    uint8_t bitmap[HANDWRITING_BYTES];
    char hint[32];
} handwriting_view_t;

void handwriting_init(void);
void handwriting_set_language(uint8_t language);
void handwriting_enter(void);
void handwriting_cancel(void);
bool handwriting_active(void);
handwriting_result_t handwriting_poll(bool pressed, uint16_t x, uint16_t y);
void handwriting_snapshot(handwriting_view_t *view);
uint8_t handwriting_count(void);
const uint8_t *handwriting_glyph(uint8_t index);
uint8_t handwriting_glyph_color(uint8_t index);
void handwriting_color_rgb(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b);
