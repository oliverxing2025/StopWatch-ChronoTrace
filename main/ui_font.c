#include "ui_font.h"

#include "ui_font_source_han_20.h"
#include "ui_font_source_han_24.h"
#include "ui_font_source_han_48.h"
#include "ui_font_source_han_64.h"
#include "ui_font_source_han_serif_brand_24.h"
#include "ui_font_source_han_serif_brand_30.h"

const ui_font_t *ui_font_message(void)
{
    return &g_source_han_sans_cn_24;
}

const ui_font_t *ui_font_timer_label(void)
{
    return &g_source_han_sans_cn_20;
}

const ui_font_t *ui_font_number_medium(void)
{
    return &g_source_han_sans_cn_48;
}

const ui_font_t *ui_font_number_large(void)
{
    return &g_source_han_sans_cn_64;
}

const ui_font_t *ui_font_brand_title(void)
{
    return &g_source_han_serif_cn_brand_30;
}

const ui_font_t *ui_font_brand_subtitle(void)
{
    return &g_source_han_sans_cn_20;
}

const ui_font_t *ui_font_brand_english(void)
{
    return &g_source_han_serif_cn_brand_24;
}

static const ui_glyph_t *find_exact(const ui_font_t *font, uint32_t codepoint)
{
    int low = 0;
    int high = (int)font->glyph_count - 1;
    while (low <= high) {
        const int middle = low + (high - low) / 2;
        const uint32_t candidate = font->glyphs[middle].codepoint;
        if (candidate == codepoint) return &font->glyphs[middle];
        if (candidate < codepoint) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    return NULL;
}

const ui_glyph_t *ui_font_glyph(const ui_font_t *font, uint32_t codepoint)
{
    const ui_glyph_t *glyph = find_exact(font, codepoint);
    if (glyph != NULL) return glyph;
    glyph = find_exact(font, font->fallback_codepoint);
    return glyph != NULL ? glyph : &font->glyphs[0];
}

bool ui_font_has_glyph(const ui_font_t *font, uint32_t codepoint)
{
    return find_exact(font, codepoint) != NULL;
}

uint32_t ui_font_utf8_next(const char **text)
{
    const uint8_t *s = (const uint8_t *)*text;
    if (*s < 0x80) {
        *text += 1;
        return *s;
    }
    if ((*s & 0xE0) == 0xC0 && s[1] != 0) {
        *text += 2;
        return ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((*s & 0xF0) == 0xE0 && s[1] != 0 && s[2] != 0) {
        *text += 3;
        return ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((*s & 0xF8) == 0xF0 && s[1] != 0 && s[2] != 0 && s[3] != 0) {
        *text += 4;
        return ((uint32_t)(s[0] & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *text += 1;
    return 0x25A1;
}

int ui_font_measure_utf8(const ui_font_t *font, const char *text)
{
    int width = 0;
    int glyph_count = 0;
    while (*text != '\0') {
        const ui_glyph_t *glyph = ui_font_glyph(font, ui_font_utf8_next(&text));
        width += glyph->advance;
        glyph_count++;
    }
    if (glyph_count > 1) width += (glyph_count - 1) * font->letter_spacing;
    return width;
}
