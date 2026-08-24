#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t codepoint;
    uint32_t bitmap_offset;
    uint16_t bitmap_size;
    uint8_t width;
    uint8_t height;
    uint8_t advance;
    int8_t x_offset;
    int8_t y_offset;
} ui_glyph_t;

typedef struct {
    const ui_glyph_t *glyphs;
    const uint8_t *bitmap;
    uint16_t glyph_count;
    uint32_t bitmap_size;
    uint32_t fallback_codepoint;
    uint8_t pixel_size;
    uint8_t ascent;
    uint8_t descent;
    uint8_t line_height;
    uint8_t letter_spacing;
} ui_font_t;

// The current firmware has one Chinese UI role: the centered operation
// message. Keeping the role behind this accessor prevents render pages from
// hard-coding a concrete generated font.
const ui_font_t *ui_font_message(void);
const ui_font_t *ui_font_timer_label(void);
const ui_font_t *ui_font_number_medium(void);
const ui_font_t *ui_font_number_large(void);
const ui_font_t *ui_font_brand_title(void);
const ui_font_t *ui_font_brand_subtitle(void);
const ui_font_t *ui_font_brand_english(void);

// Binary-searches the subset and returns the replacement-box glyph when a
// codepoint is absent. The function never returns NULL for a valid font.
const ui_glyph_t *ui_font_glyph(const ui_font_t *font, uint32_t codepoint);
bool ui_font_has_glyph(const ui_font_t *font, uint32_t codepoint);

uint32_t ui_font_utf8_next(const char **text);
int ui_font_measure_utf8(const ui_font_t *font, const char *text);
