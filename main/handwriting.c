#include "handwriting.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "settings.h"

#define PAD_X0 89
#define PAD_Y0 70
#define PAD_SIZE 288
#define ACTION_Y 375

static const char *TAG = "handwriting";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR uint8_t s_saved[HANDWRITING_MAX_GLYPHS][HANDWRITING_BYTES];
static EXT_RAM_BSS_ATTR uint8_t s_draft[HANDWRITING_MAX_GLYPHS][HANDWRITING_BYTES];
static uint8_t s_saved_colors[HANDWRITING_MAX_GLYPHS];
static uint8_t s_draft_colors[HANDWRITING_MAX_GLYPHS];
static uint8_t s_saved_count;
static uint8_t s_page;
static uint8_t s_committed;
static bool s_active;
static bool s_was_pressed;
static bool s_palette_gesture;
static uint32_t s_revision;
static int s_last_x, s_last_y;
static uint16_t s_touch_x, s_touch_y;
static char s_hint[32];
static uint8_t s_language;

static const uint8_t s_palette[HANDWRITING_COLOR_COUNT][3] = {
    {70, 220, 255},   // cyan
    {55, 115, 255},   // blue
    {165, 85, 255},   // violet
    {255, 80, 175},   // pink
    {255, 185, 45},   // amber
    {65, 235, 135},   // mint
    {255, 70, 65},    // red
    {235, 242, 255},  // pearl white
};

static int palette_hit(uint16_t x, uint16_t y)
{
    const int centers_x[2] = {60, 406};
    const int centers_y[4] = {112, 180, 248, 316};
    const uint8_t color_at[2][4] = {{0, 1, 2, 7}, {3, 4, 5, 6}};
    for (int side = 0; side < 2; side++) for (int row = 0; row < 4; row++) {
        const int dx = (int)x - centers_x[side];
        const int dy = (int)y - centers_y[row];
        // The visible disc is intentionally compact, but the edge of a round
        // touch panel needs a larger forgiving target. Radius 28 remains just
        // outside the writing pad (x=89..376) on both sides.
        if (dx * dx + dy * dy <= 28 * 28) {
            return color_at[side][row];
        }
    }
    return -1;
}

static bool has_ink(const uint8_t *bitmap)
{
    for (int i = 0; i < HANDWRITING_BYTES; i++) if (bitmap[i]) return true;
    return false;
}

static void set_pixel(uint8_t *bitmap, int x, int y)
{
    if (x < 0 || x >= HANDWRITING_W || y < 0 || y >= HANDWRITING_H) return;
    const int bit = y * HANDWRITING_W + x;
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

static void draw_line(uint8_t *bitmap, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        set_pixel(bitmap, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void handwriting_init(void)
{
    uint8_t count = 0;
    const esp_err_t err = settings_load_handwriting(&count, &s_saved[0][0],
                                                     sizeof(s_saved), HANDWRITING_BYTES,
                                                     s_saved_colors, sizeof(s_saved_colors));
    if (err == ESP_OK && count <= HANDWRITING_MAX_GLYPHS) {
        s_saved_count = count;
        for (int i = 0; i < s_saved_count; i++) {
            if (s_saved_colors[i] >= HANDWRITING_COLOR_COUNT) {
                s_saved_colors[i] = HANDWRITING_DEFAULT_COLOR;
            }
        }
    }
    ESP_LOGI(TAG, "loaded %u handwritten glyphs", s_saved_count);
}

void handwriting_set_language(uint8_t language) { s_language = language ? 1 : 0; }

void handwriting_enter(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_draft, 0, sizeof(s_draft));
    memset(s_draft_colors, HANDWRITING_DEFAULT_COLOR, sizeof(s_draft_colors));
    s_page = 0; s_committed = 0; s_hint[0] = 0;
    s_active = true; s_was_pressed = false; s_palette_gesture = false;
    s_revision++;
    portEXIT_CRITICAL(&s_lock);
}

void handwriting_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    s_active = false; s_was_pressed = false; s_palette_gesture = false;
    s_revision++;
    portEXIT_CRITICAL(&s_lock);
}

bool handwriting_active(void) { return s_active; }

handwriting_result_t handwriting_poll(bool pressed, uint16_t x, uint16_t y)
{
    if (!s_active) return HANDWRITING_RESULT_NONE;
    handwriting_result_t result = HANDWRITING_RESULT_NONE;
    int changed_color = -1;
    portENTER_CRITICAL(&s_lock);
    if (pressed) { s_touch_x = x; s_touch_y = y; }
    if (pressed && !s_was_pressed) s_palette_gesture = false;
    const int color = pressed ? palette_hit(x, y) : -1;
    if (pressed && color >= 0) {
        // Accept a finger that lands slightly outside and slides into the
        // target. Once recognised, keep the whole gesture out of handwriting.
        s_palette_gesture = true;
        if (s_draft_colors[s_page] != (uint8_t)color) {
            s_draft_colors[s_page] = (uint8_t)color;
            s_hint[0] = 0;
            s_revision++;
            changed_color = color;
        }
    } else if (pressed && !s_palette_gesture &&
        x >= PAD_X0 && x < PAD_X0 + PAD_SIZE &&
        y >= PAD_Y0 && y < PAD_Y0 + PAD_SIZE) {
        int px = ((int)x - PAD_X0) * HANDWRITING_W / PAD_SIZE;
        int py = ((int)y - PAD_Y0) * HANDWRITING_H / PAD_SIZE;
        if (px >= HANDWRITING_W) px = HANDWRITING_W - 1;
        if (py >= HANDWRITING_H) py = HANDWRITING_H - 1;
        if (s_was_pressed) draw_line(s_draft[s_page], s_last_x, s_last_y, px, py);
        else set_pixel(s_draft[s_page], px, py);
        s_last_x = px; s_last_y = py; s_hint[0] = 0;
        s_revision++;
    } else if (!pressed && s_was_pressed && !s_palette_gesture &&
               s_touch_y >= ACTION_Y) {
        if (s_touch_x < 145) {
            s_active = false;
            result = HANDWRITING_RESULT_CANCELED;
        } else if (s_touch_x < 233) {
            memset(s_draft[s_page], 0, HANDWRITING_BYTES);
            snprintf(s_hint, sizeof(s_hint), "%s", s_language ? "Deleted" : "已删除");
        } else if (s_touch_x < 321) {
            if (!has_ink(s_draft[s_page])) s_hint[0] = 0;
            else if (s_page + 1 >= HANDWRITING_MAX_GLYPHS) snprintf(s_hint, sizeof(s_hint), "%s", s_language ? "Up to 12" : "最多12个图形");
            else {
                s_committed = s_page + 1;
                s_draft_colors[s_page + 1] = s_draft_colors[s_page];
                s_page++;
                snprintf(s_hint, sizeof(s_hint), "%s", s_language ? "Draw next" : "请写下一个");
            }
        } else {
            uint8_t count = s_page;
            if (has_ink(s_draft[s_page])) count++;
            if (count == 0) s_hint[0] = 0;
            else {
                memcpy(s_saved, s_draft, (size_t)count * HANDWRITING_BYTES);
                memcpy(s_saved_colors, s_draft_colors, count);
                s_saved_count = count;
                s_active = false;
                result = HANDWRITING_RESULT_SAVED;
            }
        }
        s_revision++;
    }
    s_was_pressed = pressed;
    if (!pressed) s_palette_gesture = false;
    portEXIT_CRITICAL(&s_lock);
    if (changed_color >= 0) {
        ESP_LOGI(TAG, "glyph %u selected color %d", (unsigned)s_page + 1,
                 changed_color);
    }
    if (result == HANDWRITING_RESULT_SAVED) {
        const esp_err_t err = settings_save_handwriting(s_saved_count, &s_saved[0][0],
                                                        HANDWRITING_BYTES, s_saved_colors);
        ESP_LOGI(TAG, "saved %u glyphs: %s", s_saved_count, esp_err_to_name(err));
    }
    return result;
}

void handwriting_snapshot(handwriting_view_t *view)
{
    portENTER_CRITICAL(&s_lock);
    view->active = s_active; view->revision = s_revision;
    view->page = s_page; view->committed = s_committed;
    view->color = s_draft_colors[s_page];
    memcpy(view->bitmap, s_draft[s_page], HANDWRITING_BYTES);
    strncpy(view->hint, s_hint, sizeof(view->hint) - 1);
    view->hint[sizeof(view->hint) - 1] = 0;
    portEXIT_CRITICAL(&s_lock);
}

uint8_t handwriting_count(void) { return s_saved_count; }
const uint8_t *handwriting_glyph(uint8_t index)
{
    return index < s_saved_count ? s_saved[index] : NULL;
}

uint8_t handwriting_glyph_color(uint8_t index)
{
    return index < s_saved_count ? s_saved_colors[index] : HANDWRITING_DEFAULT_COLOR;
}

uint8_t handwriting_delete_mask(uint16_t mask)
{
    uint8_t removed = 0;
    portENTER_CRITICAL(&s_lock);
    const uint8_t old_count = s_saved_count;
    uint8_t kept = 0;
    for (uint8_t source = 0; source < old_count; source++) {
        if (mask & (uint16_t)(1U << source)) {
            removed++;
            continue;
        }
        if (kept != source) {
            memcpy(s_saved[kept], s_saved[source], HANDWRITING_BYTES);
            s_saved_colors[kept] = s_saved_colors[source];
        }
        kept++;
    }
    if (removed) {
        memset(&s_saved[kept][0], 0,
               (size_t)(old_count - kept) * HANDWRITING_BYTES);
        memset(&s_saved_colors[kept], HANDWRITING_DEFAULT_COLOR,
               old_count - kept);
        s_saved_count = kept;
        s_revision++;
    }
    portEXIT_CRITICAL(&s_lock);

    if (removed) {
        const esp_err_t err = settings_save_handwriting(s_saved_count,
                                                        &s_saved[0][0],
                                                        HANDWRITING_BYTES,
                                                        s_saved_colors);
        ESP_LOGI(TAG, "deleted %u glyphs, %u remain: %s",
                 (unsigned)removed, (unsigned)s_saved_count,
                 esp_err_to_name(err));
    }
    return removed;
}

void handwriting_color_rgb(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (index >= HANDWRITING_COLOR_COUNT) index = HANDWRITING_DEFAULT_COLOR;
    if (r) *r = s_palette[index][0];
    if (g) *g = s_palette[index][1];
    if (b) *b = s_palette[index][2];
}
