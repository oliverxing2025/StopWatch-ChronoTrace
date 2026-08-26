#include "render.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "animation_library.h"
#include "boot_logo.h"
#include "countdown_play_icon.h"
#include "countdown_timer_icon.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "handwriting.h"
#include "shape_library.h"
#include "ui_font.h"

// The panel takes RGB565 with the bytes the other way round from how the CPU
// stores a uint16, so every colour is byte swapped once, up front.
#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static uint16_t s_color_lut[DEPTH_LEVELS * SPEED_LEVELS];
static uint16_t s_highlight_lut[DEPTH_LEVELS * SPEED_LEVELS];
static uint16_t s_background_row[LCD_V_RES];

#define THEME_COUNT 8
#define THEME_STOPS 6
#define THEME_DIAMOND 7

typedef struct {
    uint8_t color[THEME_STOPS][3];
} visual_theme_t;

static const visual_theme_t s_themes[THEME_COUNT] = {
    // Deep sea
    {{{5, 24, 100}, {8, 55, 170}, {20, 110, 230}, {80, 190, 255}, {175, 235, 255}, {255, 255, 255}}},
    // Cyber
    {{{48, 8, 105}, {105, 20, 190}, {210, 30, 235}, {255, 70, 190}, {90, 225, 255}, {255, 255, 255}}},
    // Lava
    {{{80, 3, 0}, {155, 14, 0}, {235, 50, 3}, {255, 125, 8}, {255, 220, 75}, {255, 255, 230}}},
    // Aurora
    {{{0, 48, 70}, {0, 110, 125}, {0, 205, 160}, {65, 250, 180}, {155, 160, 255}, {245, 250, 255}}},
    // Mercury
    {{{32, 38, 48}, {70, 82, 98}, {125, 145, 165}, {185, 205, 220}, {225, 238, 246}, {255, 255, 255}}},
    // Rainbow prism / speed spectrum
    {{{80, 25, 220}, {15, 95, 255}, {0, 220, 235}, {30, 235, 95}, {255, 220, 20}, {255, 70, 55}}},
    // Golden particles / bronze body through champagne highlights
    {{{72, 38, 2}, {122, 68, 4}, {178, 110, 10}, {224, 162, 30}, {255, 215, 92}, {255, 249, 194}}},
    // Diamond / graphite-blue body through ice and pale-violet facets
    {{{8, 13, 25}, {22, 39, 64}, {52, 91, 126}, {105, 170, 207}, {194, 224, 255}, {255, 246, 255}}},
};

static volatile uint8_t s_requested_theme;
static uint8_t s_theme;
static volatile bool s_reactive;
static volatile float s_audio_mid;
static volatile float s_audio_treble;
static bool s_force_full;
static volatile bool s_countdown_menu;
static volatile uint8_t s_countdown_minutes = 5;
static volatile bool s_countdown_dragging;
static volatile bool s_countdown_runtime_active;
static volatile bool s_countdown_runtime_paused;
static volatile int s_countdown_remaining_seconds;
static volatile float s_countdown_progress;
static volatile uint32_t s_countdown_revision;
static volatile uint8_t s_ui_language;
static volatile bool s_settings_visible;
static volatile bool s_settings_bluetooth;
static volatile bool s_settings_bluetooth_connected;
static volatile bool s_settings_time_calibrating;
static volatile bool s_settings_time_calibrated;
static volatile uint8_t s_settings_volume;
static volatile uint8_t s_settings_brightness;
static volatile bool s_settings_haptic;
static volatile uint8_t s_settings_page;
static volatile bool s_settings_wifi_enabled;
static volatile uint8_t s_settings_wifi_state;
static volatile bool s_settings_city_automatic = true;
static volatile uint8_t s_settings_wifi_notice;
static volatile int64_t s_settings_wifi_notice_until;
static volatile uint32_t s_settings_revision;
static volatile uint8_t s_wifi_editor_mode;
static char s_wifi_editor_ssids[6][33];
static volatile uint8_t s_wifi_editor_ssid_count;
static volatile uint8_t s_wifi_editor_selected;
static char s_wifi_editor_password[65];
// Common external SSID glyph seed. Unknown future SSID characters are drawn
// as a readable question mark rather than the font's replacement square.
static const char s_wifi_common_glyph_seed[] __attribute__((unused)) = "加速器";
static volatile bool s_wifi_editor_uppercase;
static volatile bool s_wifi_editor_symbols;
static volatile bool s_wifi_editor_reveal;
static volatile uint32_t s_wifi_editor_revision;
static volatile bool s_operation_guide_visible;
static volatile uint8_t s_operation_guide_page;
static volatile uint32_t s_operation_guide_revision;
static volatile bool s_shape_picker_visible;
static volatile uint8_t s_shape_picker_page;
static volatile uint8_t s_shape_picker_custom_count;
static volatile uint8_t s_shape_picker_selection_count;
static volatile bool s_shape_picker_animation;
static volatile bool s_animation_picker_selected;
static volatile uint8_t s_animation_picker_item;
static uint8_t s_shape_picker_rank[SHAPE_LIBRARY_COUNT + HANDWRITING_MAX_GLYPHS];
static volatile uint32_t s_shape_picker_revision;
static volatile bool s_weather_visible;
static volatile uint8_t s_weather_state;
static volatile bool s_weather_valid;
static char s_weather_city[49];
static volatile float s_weather_temperature;
static volatile float s_weather_apparent;
static volatile float s_weather_wind;
static volatile uint8_t s_weather_humidity;
static volatile uint8_t s_weather_code;
static volatile int64_t s_weather_updated;
static volatile uint32_t s_weather_revision;
static volatile bool s_network_busy;

typedef struct {
    int center_x;
    int center_y;
    int ring_inner_radius;
    int ring_outer_radius;
    int marker_radius;
    int marker_dot_radius;
    int hourglass_y;
    int number_center_y;
    int label_center_y;
    int button_y;
    int button_visual_radius;
} timer_ui_layout_t;

static const timer_ui_layout_t kTimerUI = {
    .center_x = 233,
    .center_y = 233,
    .ring_inner_radius = 174,
    .ring_outer_radius = 205,
    .marker_radius = 190,
    .marker_dot_radius = 8,
    .hourglass_y = 140,
    .number_center_y = 215,
    .label_center_y = 272,
    .button_y = 334,
    .button_visual_radius = 33,
};

typedef struct {
    int16_t glow_x[4];
    int16_t glow_y[4];
    int16_t core_x[4];
    int16_t core_y[4];
    uint16_t color;
} timer_tick_geometry_t;

static timer_tick_geometry_t s_timer_ticks[180];
static void build_timer_tick_geometry(void);

#define MESSAGE_MAX_BYTES 64
#define MESSAGE_DURATION_US 1500000

static portMUX_TYPE s_message_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_message[MESSAGE_MAX_BYTES];
static int64_t s_message_until_us;

// Half-width of a filled disc, indexed by radius then by row offset.
static uint8_t s_disc_span[DISC_MAX_R + 1][2 * DISC_MAX_R + 1];

// Which bands any particle falls in, this frame and last. A band that is empty
// now and was empty last frame is already black on the panel, so it needs
// neither clearing nor transmitting. With the fluid pooled in the bottom third
// of the screen that removes most of the frame, and since the panel transfer is
// what paces the loop, the frame rate goes up by roughly the same proportion.
static bool s_band_used[BAND_COUNT];
static bool s_band_used_prev[BAND_COUNT];

// Projected particles for the current frame.
// Projection metadata is accessed sequentially and does not participate in
// DMA, so it can live in PSRAM. The solver state and DMA bands remain in fast
// internal SRAM; this preserves runtime heap for tasks and panel drivers.
static EXT_RAM_BSS_ATTR sim_particle_view_t s_snapshot[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR int16_t s_sx[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR int16_t s_sy[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_sr[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR uint16_t s_sc[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR uint16_t s_sh[PARTICLE_MAX];

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void speed_color(float t, int *r, int *g, int *b)
{
    const visual_theme_t *theme = &s_themes[s_theme];

    if (t <= 0.0f) {
        *r = theme->color[0][0]; *g = theme->color[0][1]; *b = theme->color[0][2];
        return;
    }
    if (t >= 1.0f) {
        *r = theme->color[THEME_STOPS - 1][0];
        *g = theme->color[THEME_STOPS - 1][1];
        *b = theme->color[THEME_STOPS - 1][2];
        return;
    }

    const float scaled = t * (THEME_STOPS - 1);
    int s = (int)scaled;
    if (s > THEME_STOPS - 2) s = THEME_STOPS - 2;
    const float f = scaled - s;
    for (int c = 0; c < 3; c++) {
        const int value = (int)(theme->color[s][c] +
                          f * (theme->color[s + 1][c] - theme->color[s][c]));
        if (c == 0) *r = value;
        else if (c == 1) *g = value;
        else *b = value;
    }
}

static void build_background(void)
{
    // True AMOLED black for every theme. Theme selection changes only the
    // particle palette; music reactivity never lifts the background pixels.
    memset(s_background_row, 0, sizeof(s_background_row));
}

static void fill_background(uint16_t *buf, int band_y0)
{
    for (int row = 0; row < BAND_ROWS; row++) {
        const int y = band_y0 + row;
        const uint16_t color = s_background_row[y < LCD_V_RES ? y : LCD_V_RES - 1];
        uint16_t *line = buf + row * LCD_H_RES;
        for (int x = 0; x < LCD_H_RES; x++) {
            line[x] = color;
        }
    }
}

void render_set_theme(uint8_t theme)
{
    s_requested_theme = theme % THEME_COUNT;
}

uint8_t render_get_theme(void)
{
    return s_requested_theme;
}

void render_set_reactive(bool enabled)
{
    s_reactive = enabled;
    s_force_full = true;
}

void render_set_audio_levels(float bass, float mid, float treble)
{
    (void)bass;
    s_audio_mid = mid;
    s_audio_treble = treble;
}

void render_set_countdown_menu(bool visible)
{
    s_countdown_menu = visible;
    s_countdown_revision++;
    s_force_full = true;
}

void render_set_countdown_selector(uint8_t minutes, bool dragging)
{
    const uint8_t new_minutes = minutes > 60 ? 60 : minutes;
    if (s_countdown_minutes == new_minutes &&
        s_countdown_dragging == dragging) return;
    s_countdown_minutes = new_minutes;
    s_countdown_dragging = dragging;
    s_countdown_revision++;
    s_force_full = true;
}

void render_set_countdown_runtime(bool active, int remaining_seconds,
                                  float progress, bool paused)
{
    if (remaining_seconds < 0) remaining_seconds = 0;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    if (s_countdown_runtime_active == active &&
        s_countdown_runtime_paused == paused &&
        s_countdown_remaining_seconds == remaining_seconds &&
        fabsf(s_countdown_progress - progress) < 0.000001f) return;
    const bool visibility_changed = s_countdown_runtime_active != active;
    s_countdown_runtime_active = active;
    s_countdown_runtime_paused = paused;
    s_countdown_remaining_seconds = remaining_seconds;
    s_countdown_progress = progress;
    s_countdown_revision++;
    if (visibility_changed) s_force_full = true;
}

void render_set_language(uint8_t language)
{
    s_ui_language = language ? 1 : 0;
    s_force_full = true;
}

void render_set_settings(bool visible, uint8_t language, bool bluetooth_on,
                         bool bluetooth_connected, bool time_calibrating,
                         bool time_calibrated,
                         uint8_t volume, uint8_t brightness,
                         bool haptic_enabled, uint8_t page,
                         bool wifi_enabled, uint8_t wifi_state,
                         bool city_automatic)
{
    s_ui_language = language ? 1 : 0;
    s_settings_visible = visible;
    s_settings_bluetooth = bluetooth_on;
    s_settings_bluetooth_connected = bluetooth_connected;
    s_settings_time_calibrating = time_calibrating;
    s_settings_time_calibrated = time_calibrated;
    s_settings_volume = volume > 100 ? 100 : volume;
    s_settings_brightness = brightness > 100 ? 100 : brightness;
    s_settings_haptic = haptic_enabled;
    s_settings_page = page ? 1 : 0;
    s_settings_wifi_enabled = wifi_enabled;
    s_settings_wifi_state = wifi_state;
    s_settings_city_automatic = city_automatic;
    s_settings_revision++;
    s_force_full = true;
}

void render_set_weather(bool visible, uint8_t language, uint8_t state,
                        bool valid, const char *city, float temperature_c,
                        float apparent_c, uint8_t humidity, float wind_kmh,
                        uint8_t weather_code, int64_t updated_unix)
{
    s_ui_language = language ? 1 : 0;
    s_weather_visible = visible;
    s_weather_state = state;
    s_weather_valid = valid;
    strlcpy(s_weather_city, city ? city : "", sizeof(s_weather_city));
    s_weather_temperature = temperature_c;
    s_weather_apparent = apparent_c;
    s_weather_humidity = humidity;
    s_weather_wind = wind_kmh;
    s_weather_code = weather_code;
    s_weather_updated = updated_unix;
    s_weather_revision++;
    s_force_full = true;
}

void render_set_network_busy(bool busy)
{
    if (s_network_busy == busy) return;
    s_network_busy = busy;
    if (!busy) s_force_full = true;
}

void render_set_wifi_notice(uint8_t state)
{
    s_settings_wifi_notice = state <= 3 ? state : 0;
    s_settings_wifi_notice_until = state == 0 ? 0 :
                                   esp_timer_get_time() + 2200000LL;
    s_settings_revision++;
    s_force_full = true;
}

void render_set_wifi_editor(uint8_t mode, uint8_t language,
                            const char (*ssids)[33], uint8_t ssid_count,
                            uint8_t selected, const char *password,
                            bool uppercase, bool symbols, bool reveal)
{
    s_ui_language = language ? 1 : 0;
    if (ssid_count > 6) ssid_count = 6;
    for (uint8_t i = 0; i < ssid_count; i++) {
        strncpy(s_wifi_editor_ssids[i], ssids[i], 32);
        s_wifi_editor_ssids[i][32] = '\0';
    }
    s_wifi_editor_ssid_count = ssid_count;
    s_wifi_editor_selected = selected < ssid_count ? selected : 0;
    strncpy(s_wifi_editor_password, password ? password : "", 64);
    s_wifi_editor_password[64] = '\0';
    s_wifi_editor_uppercase = uppercase;
    s_wifi_editor_symbols = symbols;
    s_wifi_editor_reveal = reveal;
    s_wifi_editor_mode = mode > 3 ? 0 : mode;
    s_wifi_editor_revision++;
    s_force_full = true;
}

void render_set_operation_guide(bool visible, uint8_t language, uint8_t page)
{
    s_ui_language = language ? 1 : 0;
    s_operation_guide_visible = visible;
    s_operation_guide_page = page ? 1 : 0;
    s_operation_guide_revision++;
    s_force_full = true;
}

void render_set_shape_picker(bool visible, uint8_t language, uint8_t page,
                             uint8_t custom_count,
                             const uint8_t *selection_rank,
                             uint8_t selection_count)
{
    s_ui_language = language ? 1 : 0;
    if (custom_count > HANDWRITING_MAX_GLYPHS) custom_count = HANDWRITING_MAX_GLYPHS;
    s_shape_picker_visible = visible;
    s_shape_picker_page = page;
    s_shape_picker_custom_count = custom_count;
    s_shape_picker_selection_count = selection_count;
    memset(s_shape_picker_rank, 0, sizeof(s_shape_picker_rank));
    if (selection_rank) {
        memcpy(s_shape_picker_rank, selection_rank,
               SHAPE_LIBRARY_COUNT + custom_count);
    }
    s_shape_picker_revision++;
    s_force_full = true;
}

void render_set_shape_picker_category(bool animation, bool selected,
                                      uint8_t animation_item)
{
    s_shape_picker_animation = animation;
    s_animation_picker_selected = selected;
    s_animation_picker_item = animation_item < ANIMATION_LIBRARY_COUNT ?
                              animation_item : 0;
    s_shape_picker_revision++;
    s_force_full = true;
}

void render_show_message(const char *message)
{
    if (message == NULL) return;
    portENTER_CRITICAL(&s_message_lock);
    strncpy(s_message, message, sizeof(s_message) - 1);
    s_message[sizeof(s_message) - 1] = '\0';
    s_message_until_us = esp_timer_get_time() + MESSAGE_DURATION_US;
    portEXIT_CRITICAL(&s_message_lock);
}

static bool message_snapshot(char *message, size_t capacity)
{
    const int64_t now = esp_timer_get_time();
    bool active;
    portENTER_CRITICAL(&s_message_lock);
    active = s_message[0] != '\0' && now < s_message_until_us;
    if (active) {
        strncpy(message, s_message, capacity - 1);
        message[capacity - 1] = '\0';
    } else {
        s_message[0] = '\0';
        message[0] = '\0';
    }
    portEXIT_CRITICAL(&s_message_lock);
    return active;
}

static uint16_t blend_white_4bpp(uint16_t swapped_destination, uint8_t alpha)
{
    if (alpha == 0) return swapped_destination;
    if (alpha >= 15) return SWAP16(0xFFFF);
    const uint16_t destination = SWAP16(swapped_destination);
    int r = (destination >> 11) & 0x1F;
    int g = (destination >> 5) & 0x3F;
    int b = destination & 0x1F;
    r += ((31 - r) * alpha + 7) / 15;
    g += ((63 - g) * alpha + 7) / 15;
    b += ((31 - b) * alpha + 7) / 15;
    return SWAP16((uint16_t)((r << 11) | (g << 5) | b));
}

static uint16_t blend_color_4bpp(uint16_t swapped_destination,
                                 uint16_t swapped_source, uint8_t alpha)
{
    if (alpha == 0) return swapped_destination;
    if (alpha >= 15) return swapped_source;
    const uint16_t destination = SWAP16(swapped_destination);
    const uint16_t source = SWAP16(swapped_source);
    const int dr = (destination >> 11) & 0x1F;
    const int dg = (destination >> 5) & 0x3F;
    const int db = destination & 0x1F;
    const int sr = (source >> 11) & 0x1F;
    const int sg = (source >> 5) & 0x3F;
    const int sb = source & 0x1F;
    const int r = dr + ((sr - dr) * alpha + (sr >= dr ? 7 : -7)) / 15;
    const int g = dg + ((sg - dg) * alpha + (sg >= dg ? 7 : -7)) / 15;
    const int b = db + ((sb - db) * alpha + (sb >= db ? 7 : -7)) / 15;
    return SWAP16((uint16_t)((r << 11) | (g << 5) | b));
}

static void draw_rgba_icon(uint16_t *buf, int band_y0, const uint8_t *rgba,
                           int width, int height, int center_x, int center_y)
{
    const int left = center_x - width / 2;
    const int top = center_y - height / 2;
    for (int iy = 0; iy < height; iy++) {
        const int screen_y = top + iy;
        if (screen_y < band_y0 || screen_y >= band_y0 + BAND_ROWS ||
            screen_y < 0 || screen_y >= LCD_V_RES) continue;
        uint16_t *row = buf + (screen_y - band_y0) * LCD_H_RES;
        for (int ix = 0; ix < width; ix++) {
            const int screen_x = left + ix;
            if (screen_x < 0 || screen_x >= LCD_H_RES) continue;
            const uint8_t *pixel = rgba + (iy * width + ix) * 4;
            const uint8_t alpha = (uint8_t)((pixel[3] * 15U + 127U) / 255U);
            if (!alpha) continue;
            const uint16_t source = SWAP16(rgb565(pixel[0], pixel[1], pixel[2]));
            row[screen_x] = blend_color_4bpp(row[screen_x], source, alpha);
        }
    }
}

static void draw_glyph(uint16_t *buf, int band_y0, int pen_x, int baseline,
                       const ui_font_t *font, const ui_glyph_t *glyph)
{
    const uint8_t *bitmap = font->bitmap + glyph->bitmap_offset;
    for (int y = 0; y < glyph->height; y++) {
        const int screen_y = baseline + glyph->y_offset + y;
        if (screen_y < band_y0 || screen_y >= band_y0 + BAND_ROWS) continue;
        uint16_t *row = buf + (screen_y - band_y0) * LCD_H_RES;
        for (int x = 0; x < glyph->width; x++) {
            const int pixel = y * glyph->width + x;
            const uint8_t packed = bitmap[pixel >> 1];
            const uint8_t alpha = (pixel & 1) ? (packed & 0x0F) : (packed >> 4);
            if (alpha == 0) continue;
            const int screen_x = pen_x + glyph->x_offset + x;
            if (screen_x >= 0 && screen_x < LCD_H_RES) {
                row[screen_x] = blend_white_4bpp(row[screen_x], alpha);
            }
        }
    }
}

static void draw_glyph_color(uint16_t *buf, int band_y0, int pen_x,
                             int baseline, const ui_font_t *font,
                             const ui_glyph_t *glyph, uint16_t color)
{
    const uint8_t *bitmap = font->bitmap + glyph->bitmap_offset;
    for (int y = 0; y < glyph->height; y++) {
        const int screen_y = baseline + glyph->y_offset + y;
        if (screen_y < band_y0 || screen_y >= band_y0 + BAND_ROWS) continue;
        uint16_t *row = buf + (screen_y - band_y0) * LCD_H_RES;
        for (int x = 0; x < glyph->width; x++) {
            const int pixel = y * glyph->width + x;
            const uint8_t packed = bitmap[pixel >> 1];
            const uint8_t alpha = (pixel & 1) ? (packed & 0x0F) : (packed >> 4);
            if (alpha == 0) continue;
            const int screen_x = pen_x + glyph->x_offset + x;
            if (screen_x >= 0 && screen_x < LCD_H_RES) {
                row[screen_x] = blend_color_4bpp(row[screen_x], color, alpha);
            }
        }
    }
}

static void draw_text_centered_spaced(uint16_t *buf, int band_y0,
                                      const char *text, int center_x,
                                      int line_top, const ui_font_t *font,
                                      int spacing, uint16_t color)
{
    const char *cursor = text;
    int width = 0;
    int count = 0;
    while (*cursor) {
        const ui_glyph_t *glyph = ui_font_glyph(font, ui_font_utf8_next(&cursor));
        width += glyph->advance;
        count++;
    }
    if (count > 1) width += (count - 1) * spacing;
    int pen_x = center_x - width / 2;
    const int baseline = line_top + font->ascent;
    while (*text) {
        const ui_glyph_t *glyph = ui_font_glyph(font, ui_font_utf8_next(&text));
        draw_glyph_color(buf, band_y0, pen_x, baseline, font, glyph, color);
        pen_x += glyph->advance + spacing;
    }
}

// Center against the glyphs' actual illuminated bounds rather than the font's
// line box. This matters for compact buttons because Source Han Sans reserves
// extra ascent/descent space that makes mathematically centered text look low.
static void draw_text_centered_in_rect(uint16_t *buf, int band_y0,
                                       const char *text,
                                       int x0, int y0, int x1, int y1,
                                       const ui_font_t *font, int spacing,
                                       uint16_t color)
{
    const char *cursor = text;
    int pen = 0;
    int min_x = 32767, min_y = 32767;
    int max_x = -32768, max_y = -32768;
    while (*cursor) {
        const ui_glyph_t *glyph = ui_font_glyph(font,
                                                ui_font_utf8_next(&cursor));
        if (glyph->width > 0 && glyph->height > 0) {
            const int gx0 = pen + glyph->x_offset;
            const int gy0 = glyph->y_offset;
            const int gx1 = gx0 + glyph->width - 1;
            const int gy1 = gy0 + glyph->height - 1;
            if (gx0 < min_x) min_x = gx0;
            if (gy0 < min_y) min_y = gy0;
            if (gx1 > max_x) max_x = gx1;
            if (gy1 > max_y) max_y = gy1;
        }
        pen += glyph->advance + spacing;
    }
    if (max_x < min_x || max_y < min_y) return;

    const int origin_x = (x0 + x1 - min_x - max_x) / 2;
    const int baseline = (y0 + y1 - min_y - max_y) / 2;
    pen = 0;
    while (*text) {
        const ui_glyph_t *glyph = ui_font_glyph(font,
                                                ui_font_utf8_next(&text));
        draw_glyph_color(buf, band_y0, origin_x + pen, baseline,
                         font, glyph, color);
        pen += glyph->advance + spacing;
    }
}

static const ui_glyph_t *external_text_glyph(const ui_font_t *font,
                                              uint32_t codepoint)
{
    return ui_font_glyph(font,
                         ui_font_has_glyph(font, codepoint) ? codepoint : '?');
}

// SSIDs are external data and can contain characters outside the deliberately
// small Source Han subset. Keep all known glyphs and use '?' for only the
// unknown codepoints so an arbitrary network name can never show empty boxes.
static void draw_external_text_centered_in_rect(uint16_t *buf, int band_y0,
                                                 const char *text,
                                                 int x0, int y0, int x1, int y1,
                                                 const ui_font_t *font,
                                                 uint16_t color)
{
    const char *cursor = text;
    int pen = 0;
    int min_x = 32767, min_y = 32767;
    int max_x = -32768, max_y = -32768;
    while (*cursor) {
        const ui_glyph_t *glyph = external_text_glyph(
            font, ui_font_utf8_next(&cursor));
        if (glyph->width > 0 && glyph->height > 0) {
            const int gx0 = pen + glyph->x_offset;
            const int gy0 = glyph->y_offset;
            const int gx1 = gx0 + glyph->width - 1;
            const int gy1 = gy0 + glyph->height - 1;
            if (gx0 < min_x) min_x = gx0;
            if (gy0 < min_y) min_y = gy0;
            if (gx1 > max_x) max_x = gx1;
            if (gy1 > max_y) max_y = gy1;
        }
        pen += glyph->advance;
    }
    if (max_x < min_x || max_y < min_y) return;

    const int origin_x = (x0 + x1 - min_x - max_x) / 2;
    const int baseline = (y0 + y1 - min_y - max_y) / 2;
    cursor = text;
    pen = 0;
    while (*cursor) {
        const ui_glyph_t *glyph = external_text_glyph(
            font, ui_font_utf8_next(&cursor));
        draw_glyph_color(buf, band_y0, origin_x + pen, baseline,
                         font, glyph, color);
        pen += glyph->advance;
    }
}

static void draw_message(uint16_t *buf, int band_y0, const char *message)
{
    const ui_font_t *font = ui_font_message();
    const int text_width = ui_font_measure_utf8(font, message);
    int x = (LCD_H_RES - text_width) / 2;
    const int line_top = (LCD_V_RES - font->line_height) / 2;
    const int baseline = line_top + font->ascent;
    while (*message) {
        const uint32_t codepoint = ui_font_utf8_next(&message);
        const ui_glyph_t *glyph = ui_font_glyph(font, codepoint);
        draw_glyph(buf, band_y0, x, baseline, font, glyph);
        x += glyph->advance + font->letter_spacing;
    }
}

static void draw_text_centered_font(uint16_t *buf, int band_y0, const char *text,
                                    int center_x, int line_top,
                                    const ui_font_t *font)
{
    int x = center_x - ui_font_measure_utf8(font, text) / 2;
    const int baseline = line_top + font->ascent;
    while (*text) {
        const uint32_t codepoint = ui_font_utf8_next(&text);
        const ui_glyph_t *glyph = ui_font_glyph(font, codepoint);
        draw_glyph(buf, band_y0, x, baseline, font, glyph);
        x += glyph->advance + font->letter_spacing;
    }
}

static void draw_text_centered(uint16_t *buf, int band_y0, const char *text,
                               int center_x, int line_top)
{
    draw_text_centered_font(buf, band_y0, text, center_x, line_top,
                            ui_font_message());
}

static void draw_text_centered_rotated(uint16_t *buf, int band_y0,
                                       const char *text, int center_x,
                                       int line_top, float angle)
{
    const ui_font_t *font = ui_font_message();
    const ui_glyph_t *glyphs[8];
    int pens[8];
    int glyph_count = 0;
    const int text_width = ui_font_measure_utf8(font, text);
    int pen = -text_width / 2;
    const char *cursor = text;
    while (*cursor && glyph_count < 8) {
        const uint32_t codepoint = ui_font_utf8_next(&cursor);
        const ui_glyph_t *glyph = ui_font_glyph(font, codepoint);
        glyphs[glyph_count] = glyph;
        pens[glyph_count] = pen;
        glyph_count++;
        pen += glyph->advance + font->letter_spacing;
    }

    const float cs = cosf(angle);
    const float sn = sinf(angle);
    const float center_y = line_top + font->line_height * 0.5f;
    const float baseline = -font->line_height * 0.5f + font->ascent;
    const int coverage_y0 = line_top - 24;
    const int coverage_y1 = line_top + font->line_height + 24;
    if (band_y0 > coverage_y1 || band_y0 + BAND_ROWS <= coverage_y0) return;

    // Forward-map only the font's real ink pixels. Bilinear splatting keeps
    // the angled strokes smooth without scanning a large rotated rectangle
    // for every display band.
    for (int i = 0; i < glyph_count; i++) {
        const ui_glyph_t *glyph = glyphs[i];
        for (int gy = 0; gy < glyph->height; gy++) for (int gx = 0; gx < glyph->width; gx++) {
            const int pixel = gy * glyph->width + gx;
            const uint8_t packed = font->bitmap[glyph->bitmap_offset + (pixel >> 1)];
            const uint8_t alpha = (pixel & 1) ? (packed & 0x0F) : (packed >> 4);
            if (!alpha) continue;
            const float lx = pens[i] + glyph->x_offset + gx;
            const float ly = baseline + glyph->y_offset + gy;
            const float mapped_x = center_x + cs * lx - sn * ly;
            const float mapped_y = center_y + sn * lx + cs * ly;
            const int x0 = (int)floorf(mapped_x);
            const int y0 = (int)floorf(mapped_y);
            const float fx = mapped_x - x0;
            const float fy = mapped_y - y0;
            const uint8_t weights[4] = {
                (uint8_t)(alpha * (1.0f - fx) * (1.0f - fy) + 0.5f),
                (uint8_t)(alpha * fx * (1.0f - fy) + 0.5f),
                (uint8_t)(alpha * (1.0f - fx) * fy + 0.5f),
                (uint8_t)(alpha * fx * fy + 0.5f),
            };
            const int px[4] = {x0, x0 + 1, x0, x0 + 1};
            const int py[4] = {y0, y0, y0 + 1, y0 + 1};
            for (int sample = 0; sample < 4; sample++) {
                if (!weights[sample] || px[sample] < 0 || px[sample] >= LCD_H_RES ||
                    py[sample] < band_y0 || py[sample] >= band_y0 + BAND_ROWS ||
                    py[sample] < 0 || py[sample] >= LCD_V_RES) continue;
                uint16_t *row = buf + (py[sample] - band_y0) * LCD_H_RES;
                row[px[sample]] = blend_white_4bpp(row[px[sample]], weights[sample]);
            }
        }
    }
}

static bool rounded_rect_contains(int x, int y, int x0, int y0,
                                  int x1, int y1, int radius)
{
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    int cx = x;
    int cy = y;
    if (x < x0 + radius) cx = x0 + radius;
    else if (x > x1 - radius) cx = x1 - radius;
    if (y < y0 + radius) cy = y0 + radius;
    else if (y > y1 - radius) cy = y1 - radius;
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void draw_writing_frame(uint16_t *buf, int band_y0)
{
    const uint16_t color = SWAP16(rgb565(34, 91, 108));
    const int y0 = band_y0 > 63 ? band_y0 : 63;
    const int y1 = band_y0 + BAND_ROWS - 1 < 365 ?
                   band_y0 + BAND_ROWS - 1 : 365;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
        for (int x = 82; x <= 384; x++) {
            const bool outer = rounded_rect_contains(x, y, 82, 63, 384, 365, 20);
            const bool inner = rounded_rect_contains(x, y, 84, 65, 382, 363, 18);
            if (outer && !inner) row[x] = color;
        }
    }
}

static void draw_practice_guides(uint16_t *buf, int band_y0)
{
    const uint16_t color = SWAP16(rgb565(20, 58, 69));
    // 8 px ink / 8 px gap. The two axes sit at the exact centre of the
    // enlarged 288 px writing field and remain deliberately dimmer than its
    // outline and the live handwriting.
    for (int y = 70; y < 358; y++) {
        if (y < band_y0 || y >= band_y0 + BAND_ROWS || ((y - 70) & 15) >= 8) continue;
        buf[(y - band_y0) * LCD_H_RES + 233] = color;
    }
    if (214 >= band_y0 && 214 < band_y0 + BAND_ROWS) {
        uint16_t *row = buf + (214 - band_y0) * LCD_H_RES;
        for (int x = 89; x < 377; x++) {
            if (((x - 89) & 15) < 8) row[x] = color;
        }
    }
}

static void draw_color_palette(uint16_t *buf, int band_y0, uint8_t selected)
{
    const int centers_x[2] = {60, 406};
    const int centers_y[4] = {112, 180, 248, 316};
    const uint8_t color_at[2][4] = {{0, 1, 2, 7}, {3, 4, 5, 6}};
    for (int side = 0; side < 2; side++) for (int row_index = 0; row_index < 4; row_index++) {
        const uint8_t index = color_at[side][row_index];
        uint8_t r, g, b;
        handwriting_color_rgb(index, &r, &g, &b);
        const uint16_t fill = SWAP16(rgb565(r, g, b));
        const uint16_t outline = SWAP16(index == selected ? rgb565(245, 250, 255)
                                                          : rgb565(35, 55, 65));
        const int cx = centers_x[side], cy = centers_y[row_index];
        for (int y = cy - 16; y <= cy + 16; y++) {
            if (y < band_y0 || y >= band_y0 + BAND_ROWS) continue;
            uint16_t *line = buf + (y - band_y0) * LCD_H_RES;
            for (int x = cx - 16; x <= cx + 16; x++) {
                const int dx = x - cx, dy = y - cy;
                const int distance2 = dx * dx + dy * dy;
                if (distance2 <= 12 * 12) {
                    line[x] = fill;
                } else if (distance2 <= (index == selected ? 16 * 16 : 14 * 14)) {
                    line[x] = outline;
                }
            }
        }
    }
}

static void render_handwriting(const handwriting_view_t *view,
                               const bool dirty[BAND_COUNT])
{
    char title[20];
    snprintf(title, sizeof(title), s_ui_language ? "Draw %u/%u" : "自绘%u/%u",
             (unsigned)view->page + 1, (unsigned)HANDWRITING_MAX_GLYPHS);
    uint8_t ink_r, ink_g, ink_b;
    handwriting_color_rgb(view->color, &ink_r, &ink_g, &ink_b);
    const uint16_t ink = SWAP16(rgb565(ink_r, ink_g, ink_b));
    for (int band = 0; band < BAND_COUNT; band++) {
        if (!dirty[band]) continue;
        const int band_y0 = band * BAND_ROWS;
        uint16_t *buf = display_acquire_band();
        fill_background(buf, band_y0);
        draw_writing_frame(buf, band_y0);
        draw_practice_guides(buf, band_y0);
        draw_color_palette(buf, band_y0, view->color);
        // Bilinear filtering removes the 4-5 px stair steps produced by
        // nearest-neighbour expansion while leaving the stored bitmap intact.
        const int draw_y0 = band_y0 > 70 ? band_y0 : 70;
        const int draw_y1 = band_y0 + BAND_ROWS < 358 ? band_y0 + BAND_ROWS : 358;
        for (int sy = draw_y0; sy < draw_y1; sy++) {
            const float source_y = ((float)(sy - 70) + 0.5f) *
                                   HANDWRITING_H / 288.0f - 0.5f;
            const int y0 = (int)floorf(source_y);
            const float fy = source_y - y0;
            uint16_t *row = buf + (sy - band_y0) * LCD_H_RES;
            for (int sx = 89; sx < 377; sx++) {
                const float source_x = ((float)(sx - 89) + 0.5f) *
                                       HANDWRITING_W / 288.0f - 0.5f;
                const int x0 = (int)floorf(source_x);
                const float fx = source_x - x0;
                float coverage = 0.0f;
                for (int oy = 0; oy < 2; oy++) for (int ox = 0; ox < 2; ox++) {
                    const int px = x0 + ox, py = y0 + oy;
                    if (px < 0 || px >= HANDWRITING_W ||
                        py < 0 || py >= HANDWRITING_H) continue;
                    const int bit = py * HANDWRITING_W + px;
                    if ((view->bitmap[bit >> 3] & (1U << (bit & 7))) == 0) continue;
                    coverage += (ox ? fx : 1.0f - fx) * (oy ? fy : 1.0f - fy);
                }
                coverage = (coverage - 0.10f) * (1.0f / 0.78f);
                if (coverage <= 0.0f) continue;
                if (coverage > 1.0f) coverage = 1.0f;
                coverage = coverage * (2.0f - coverage);
                const uint8_t alpha = (uint8_t)(coverage * 15.0f + 0.5f);
                row[sx] = blend_color_4bpp(row[sx], ink, alpha);
            }
        }
        draw_text_centered(buf, band_y0, title, LCD_H_RES / 2, 21);
        draw_text_centered_rotated(buf, band_y0,
                                   s_ui_language ? "Return" : "返回",
                                   100, 385, 0.67f);
        draw_text_centered_rotated(buf, band_y0,
                                   s_ui_language ? "Delete" : "删除",
                                   182, 424, 0.24f);
        draw_text_centered_rotated(buf, band_y0,
                                   s_ui_language ? "Next" : "下一个",
                                   284, 424, -0.24f);
        draw_text_centered_rotated(buf, band_y0,
                                   s_ui_language ? "Done" : "确定",
                                   366, 385, -0.67f);
        display_flush_band(band, buf);
    }
}

// One table covering every combination of depth and speed, so per-particle
// colouring is a single array read with no float maths.
static void build_color_lut(void)
{
    for (int d = 0; d < DEPTH_LEVELS; d++) {
        // Particles further into the case are darker, which is most of what
        // sells the depth on a flat screen.
        const float depth_t = (DEPTH_LEVELS > 1) ? (float)d / (float)(DEPTH_LEVELS - 1) : 0.0f;
        const float dim = DEPTH_DIM_MIN + (1.0f - DEPTH_DIM_MIN) * (1.0f - depth_t);

        for (int s = 0; s < SPEED_LEVELS; s++) {
            const float linear =
                (SPEED_LEVELS > 1) ? (float)s / (float)(SPEED_LEVELS - 1) : 0.0f;
            // The curve lives in the table, so the per-particle lookup stays a
            // plain multiply.
            const float speed_t = powf(linear, SPEED_COLOR_GAMMA);

            int r, g, b;
            speed_color(speed_t, &r, &g, &b);
            s_color_lut[d * SPEED_LEVELS + s] =
                SWAP16(rgb565((int)(r * dim), (int)(g * dim), (int)(b * dim)));

            // The specular dot: the same colour lifted towards white, then
            // dimmed by depth like everything else.
            const float lift = HIGHLIGHT_LIFT;
            int hr = (int)((r + (255 - r) * lift) * dim);
            int hg = (int)((g + (255 - g) * lift) * dim);
            int hb = (int)((b + (255 - b) * lift) * dim);
            if (s_theme == THEME_DIAMOND) {
                // A stable speed-indexed cyan/violet bias reads as a crystal
                // facet without random twinkle or frame-to-frame flicker.
                const float facet = 0.5f + 0.5f * sinf(speed_t * 18.8495559f);
                hr = (int)((220 + 35 * facet) * dim);
                hg = (int)((246 - 18 * facet) * dim);
                hb = (int)(255 * dim);
            }
            s_highlight_lut[d * SPEED_LEVELS + s] = SWAP16(rgb565(hr, hg, hb));
        }
    }
}

static void build_disc_spans(void)
{
    for (int r = 0; r <= DISC_MAX_R; r++) {
        for (int dy = -r; dy <= r; dy++) {
            const float w = sqrtf((float)(r * r - dy * dy));
            s_disc_span[r][dy + r] = (uint8_t)(w + 0.5f);
        }
    }
}

static inline void project(float x, float y, float z, int *out_x, int *out_y, float *out_scale)
{
    const float s = PROJ_FOCAL / (PROJ_FOCAL + z);
    *out_x = (int)((BOX_W * 0.5f) + (x - BOX_W * 0.5f) * s + 0.5f);
    *out_y = (int)((BOX_H * 0.5f) + (y - BOX_H * 0.5f) * s + 0.5f);
    *out_scale = s;
}

void render_init(void)
{
    build_color_lut();
    build_background();
    build_disc_spans();
    build_timer_tick_geometry();

    // The panel's contents are undefined until we have written every band once,
    // so the first frame must not skip any.
    for (int b = 0; b < BAND_COUNT; b++) {
        s_band_used_prev[b] = true;
    }
    s_force_full = true;
}

static inline void draw_disc(uint16_t *buf, int band_y0, int cx, int cy, int r, uint16_t color)
{
    if (r < 1) r = 1;
    if (r > DISC_MAX_R) r = DISC_MAX_R;

    const uint8_t *spans = s_disc_span[r];

    int dy0 = -r;
    int dy1 = r;
    // Clip vertically to the band before touching any pixels.
    if (cy + dy0 < band_y0) dy0 = band_y0 - cy;
    if (cy + dy1 >= band_y0 + BAND_ROWS) dy1 = band_y0 + BAND_ROWS - 1 - cy;

    for (int dy = dy0; dy <= dy1; dy++) {
        const int hw = spans[dy + r];
        int x0 = cx - hw;
        int x1 = cx + hw;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
        if (x0 > x1) {
            continue;
        }

        uint16_t *row = buf + (cy + dy - band_y0) * LCD_H_RES;
        for (int x = x0; x <= x1; x++) {
            row[x] = color;
        }
    }
}

static void draw_large_disc(uint16_t *buf, int band_y0, int cx, int cy, int r,
                            uint16_t color)
{
    const int y0 = cy - r > band_y0 ? cy - r : band_y0;
    const int y1 = cy + r < band_y0 + BAND_ROWS - 1 ?
                   cy + r : band_y0 + BAND_ROWS - 1;
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= LCD_V_RES) continue;
        const int dy = y - cy;
        const int half = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - half, x1 = cx + half;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
        uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
        for (int x = x0; x <= x1; x++) row[x] = color;
    }
}

typedef struct {
    float position;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} timer_color_stop_t;

static const timer_color_stop_t kTimerColorStops[] = {
    {0.000f, 255, 225, 59},
    {0.125f, 94, 223, 69},
    {0.250f, 0, 215, 232},
    {0.375f, 23, 119, 255},
    {0.500f, 0, 189, 251},
    {0.625f, 94, 223, 69},
    {0.700f, 255, 225, 59},
    {0.760f, 255, 83, 37},
    {0.875f, 255, 153, 26},
    {1.000f, 255, 225, 59},
};

static uint16_t timer_color(float position, float brightness)
{
    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    const int count = sizeof(kTimerColorStops) / sizeof(kTimerColorStops[0]);
    int upper = 1;
    while (upper < count - 1 && position > kTimerColorStops[upper].position) upper++;
    const timer_color_stop_t *a = &kTimerColorStops[upper - 1];
    const timer_color_stop_t *b = &kTimerColorStops[upper];
    const float width = b->position - a->position;
    const float mix = width > 0.0f ? (position - a->position) / width : 0.0f;
    const int red = (int)((a->red + (b->red - a->red) * mix) * brightness);
    const int green = (int)((a->green + (b->green - a->green) * mix) * brightness);
    const int blue = (int)((a->blue + (b->blue - a->blue) * mix) * brightness);
    return SWAP16(rgb565(red, green, blue));
}

static uint16_t scale_timer_color(uint16_t swapped, float brightness)
{
    const uint16_t native = SWAP16(swapped);
    const int red = (int)(((native >> 11) & 0x1f) * 255.0f / 31.0f * brightness);
    const int green = (int)(((native >> 5) & 0x3f) * 255.0f / 63.0f * brightness);
    const int blue = (int)((native & 0x1f) * 255.0f / 31.0f * brightness);
    return SWAP16(rgb565(red, green, blue));
}

static void timer_quad_points(float angle, float half_width,
                              int inner_radius, int outer_radius,
                              int16_t x[4], int16_t y[4])
{
    const float a0 = angle - half_width;
    const float a1 = angle + half_width;
    x[0] = (int16_t)(kTimerUI.center_x + lroundf(sinf(a0) * inner_radius));
    y[0] = (int16_t)(kTimerUI.center_y - lroundf(cosf(a0) * inner_radius));
    x[1] = (int16_t)(kTimerUI.center_x + lroundf(sinf(a1) * inner_radius));
    y[1] = (int16_t)(kTimerUI.center_y - lroundf(cosf(a1) * inner_radius));
    x[2] = (int16_t)(kTimerUI.center_x + lroundf(sinf(a1) * outer_radius));
    y[2] = (int16_t)(kTimerUI.center_y - lroundf(cosf(a1) * outer_radius));
    x[3] = (int16_t)(kTimerUI.center_x + lroundf(sinf(a0) * outer_radius));
    y[3] = (int16_t)(kTimerUI.center_y - lroundf(cosf(a0) * outer_radius));
}

static void build_timer_tick_geometry(void)
{
    const float tau = 6.28318530718f;
    for (int tick = 0; tick < 180; tick++) {
        const float position = (float)tick / 180.0f;
        const float angle = position * tau;
        timer_quad_points(angle, 0.0068f,
                          kTimerUI.ring_inner_radius - 1,
                          kTimerUI.ring_outer_radius + 1,
                          s_timer_ticks[tick].glow_x,
                          s_timer_ticks[tick].glow_y);
        timer_quad_points(angle, 0.0052f,
                          kTimerUI.ring_inner_radius,
                          kTimerUI.ring_outer_radius,
                          s_timer_ticks[tick].core_x,
                          s_timer_ticks[tick].core_y);
        s_timer_ticks[tick].color = timer_color(position, 1.0f);
    }
}

static int triangle_edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void draw_filled_triangle(uint16_t *buf, int band_y0,
                                 int x0, int y0, int x1, int y1,
                                 int x2, int y2, uint16_t color)
{
    int min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x) min_x = x2;
    int max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x) max_x = x2;
    int min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y) min_y = y2;
    int max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y) max_y = y2;
    if (min_x < 0) min_x = 0;
    if (max_x >= LCD_H_RES) max_x = LCD_H_RES - 1;
    if (min_y < band_y0) min_y = band_y0;
    if (max_y >= band_y0 + BAND_ROWS) max_y = band_y0 + BAND_ROWS - 1;
    if (min_x > max_x || min_y > max_y) return;

    const int area = triangle_edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;
    for (int y = min_y; y <= max_y; y++) {
        uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
        for (int x = min_x; x <= max_x; x++) {
            const int e0 = triangle_edge(x0, y0, x1, y1, x, y);
            const int e1 = triangle_edge(x1, y1, x2, y2, x, y);
            const int e2 = triangle_edge(x2, y2, x0, y0, x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (e0 <= 0 && e1 <= 0 && e2 <= 0)) row[x] = color;
        }
    }
}

static void draw_timer_quad(uint16_t *buf, int band_y0,
                            const int16_t x[4], const int16_t y[4],
                            uint16_t color)
{
    draw_filled_triangle(buf, band_y0, x[0], y[0], x[1], y[1],
                         x[2], y[2], color);
    draw_filled_triangle(buf, band_y0, x[0], y[0], x[2], y[2],
                         x[3], y[3], color);
}

static void draw_radial_capsule(uint16_t *buf, int band_y0, float angle,
                                float inner_radius, float outer_radius,
                                float half_width, int offset_x, int offset_y,
                                uint16_t color)
{
    // The capsule follows the radius while its round caps follow the tangent.
    // Unlike the old circular handle, this reads as a raised physical slider
    // tab and still leaves the selected colour visible around both sides.
    const float radial_x = sinf(angle);
    const float radial_y = -cosf(angle);
    const float tangent_x = cosf(angle);
    const float tangent_y = sinf(angle);
    const float inner_x = kTimerUI.center_x + radial_x * inner_radius + offset_x;
    const float inner_y = kTimerUI.center_y + radial_y * inner_radius + offset_y;
    const float outer_x = kTimerUI.center_x + radial_x * outer_radius + offset_x;
    const float outer_y = kTimerUI.center_y + radial_y * outer_radius + offset_y;
    int16_t x[4] = {
        (int16_t)lroundf(inner_x - tangent_x * half_width),
        (int16_t)lroundf(inner_x + tangent_x * half_width),
        (int16_t)lroundf(outer_x + tangent_x * half_width),
        (int16_t)lroundf(outer_x - tangent_x * half_width),
    };
    int16_t y[4] = {
        (int16_t)lroundf(inner_y - tangent_y * half_width),
        (int16_t)lroundf(inner_y + tangent_y * half_width),
        (int16_t)lroundf(outer_y + tangent_y * half_width),
        (int16_t)lroundf(outer_y - tangent_y * half_width),
    };
    draw_timer_quad(buf, band_y0, x, y, color);
    draw_disc(buf, band_y0, (int)lroundf(inner_x), (int)lroundf(inner_y),
              (int)lroundf(half_width), color);
    draw_disc(buf, band_y0, (int)lroundf(outer_x), (int)lroundf(outer_y),
              (int)lroundf(half_width), color);
}

static void draw_timer_tapered_quad(uint16_t *buf, int band_y0,
                                    const int16_t x[4], const int16_t y[4],
                                    uint16_t color, float brightness)
{
    // Six shared-boundary radial slices approximate the reference's luminous
    // inner root and progressively smoked outer tip. Their total filled area
    // is unchanged from one quad, keeping selector redraws responsive.
    static const float kFade[6] = {1.00f, 0.84f, 0.67f, 0.49f, 0.32f, 0.17f};
    for (int slice = 0; slice < 6; slice++) {
        int16_t sx[4];
        int16_t sy[4];
        // Original ordering is inner-left, inner-right, outer-right,
        // outer-left. Interpolating both sides produces a gap-free sub-quad.
        sx[0] = (int16_t)(x[0] + (x[3] - x[0]) * slice / 6);
        sy[0] = (int16_t)(y[0] + (y[3] - y[0]) * slice / 6);
        sx[1] = (int16_t)(x[1] + (x[2] - x[1]) * slice / 6);
        sy[1] = (int16_t)(y[1] + (y[2] - y[1]) * slice / 6);
        sx[2] = (int16_t)(x[1] + (x[2] - x[1]) * (slice + 1) / 6);
        sy[2] = (int16_t)(y[1] + (y[2] - y[1]) * (slice + 1) / 6);
        sx[3] = (int16_t)(x[0] + (x[3] - x[0]) * (slice + 1) / 6);
        sy[3] = (int16_t)(y[0] + (y[3] - y[0]) * (slice + 1) / 6);
        draw_timer_quad(buf, band_y0, sx, sy,
                        scale_timer_color(color, brightness * kFade[slice]));
    }
}

static void draw_timer_background(uint16_t *buf)
{
    const uint16_t background = SWAP16(rgb565(3, 5, 6));
    for (int pixel = 0; pixel < BAND_PIXELS; pixel++) buf[pixel] = background;
}

static void draw_rainbow_ticks(uint16_t *buf, int band_y0,
                               bool active, float elapsed_progress)
{
    for (int tick = 0; tick < 180; tick++) {
        const float position = (float)tick / 180.0f;
        const bool elapsed = active && position < elapsed_progress;
        const float brightness = elapsed ? 0.18f : 1.0f;
        draw_timer_tapered_quad(buf, band_y0, s_timer_ticks[tick].glow_x,
                                s_timer_ticks[tick].glow_y,
                                s_timer_ticks[tick].color,
                                brightness * 0.20f);
        draw_timer_tapered_quad(buf, band_y0, s_timer_ticks[tick].core_x,
                                s_timer_ticks[tick].core_y,
                                s_timer_ticks[tick].color, brightness);
    }
}

static void draw_progress_marker(uint16_t *buf, int band_y0, float progress)
{
    const float angle = progress * 6.28318530718f;
    const uint16_t marker_color = timer_color(progress, 1.0f);

    // Soft offset shadow gives the selector height above the luminous scale.
    draw_radial_capsule(buf, band_y0, angle, 176.0f, 207.0f, 8.0f, 2, 3,
                        SWAP16(rgb565(1, 8, 11)));
    // A coloured halo binds the handle to the exact hue underneath it.
    draw_radial_capsule(buf, band_y0, angle, 175.0f, 208.0f, 8.0f, 0, 0,
                        scale_timer_color(marker_color, 0.34f));
    // Raised colour body, deliberately longer and wider than one scale tick.
    draw_radial_capsule(buf, band_y0, angle, 178.0f, 205.0f, 6.0f, 0, 0,
                        marker_color);
    // Short specular streak: enough to suggest curved glass without turning
    // the handle back into a white dot.
    draw_radial_capsule(buf, band_y0, angle, 180.0f, 191.0f, 1.5f, -1, -1,
                        SWAP16(rgb565(224, 255, 250)));
}

static void draw_timer_minute_labels(uint16_t *buf, int band_y0)
{
    // A conventional minute bezel: twelve o'clock is labelled 0 even though
    // dragging to that same physical point selects the full 60 minutes.
    // Keeping the labels just inside the colour band makes them readable
    // without covering either the ticks or the progress handle.
    const ui_font_t *font = ui_font_timer_label();
    const uint16_t color = SWAP16(rgb565(154, 166, 174));
    const int radius = 155;
    char label[4];
    for (int minute = 0; minute < 60; minute += 5) {
        const float angle = (float)minute * 6.28318530718f / 60.0f;
        const int x = kTimerUI.center_x +
                      (int)lroundf(sinf(angle) * radius);
        const int y = kTimerUI.center_y -
                      (int)lroundf(cosf(angle) * radius);
        snprintf(label, sizeof(label), "%d", minute);
        draw_text_centered_spaced(buf, band_y0, label, x,
                                  y - font->line_height / 2,
                                  font, 0, color);
    }
}

static void draw_line_round(uint16_t *buf, int band_y0, int x0, int y0,
                            int x1, int y1, int radius, uint16_t color)
{
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps < 1) steps = 1;
    for (int step = 0; step <= steps; step++) {
        const int x = x0 + dx * step / steps;
        const int y = y0 + dy * step / steps;
        draw_disc(buf, band_y0, x, y, radius, color);
    }
}

static void draw_round_rect(uint16_t *buf, int band_y0, int x0, int y0,
                            int x1, int y1, int radius, uint16_t color)
{
    int first_y = y0 > band_y0 ? y0 : band_y0;
    int last_y = y1 < band_y0 + BAND_ROWS - 1 ? y1 : band_y0 + BAND_ROWS - 1;
    if (first_y < 0) first_y = 0;
    if (last_y >= LCD_V_RES) last_y = LCD_V_RES - 1;
    for (int y = first_y; y <= last_y; y++) {
        int inset = 0;
        if (y < y0 + radius) {
            const int dy = y0 + radius - y;
            inset = radius - (int)sqrtf((float)(radius * radius - dy * dy));
        } else if (y > y1 - radius) {
            const int dy = y - (y1 - radius);
            inset = radius - (int)sqrtf((float)(radius * radius - dy * dy));
        }
        int first_x = x0 + inset;
        int last_x = x1 - inset;
        if (first_x < 0) first_x = 0;
        if (last_x >= LCD_H_RES) last_x = LCD_H_RES - 1;
        uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
        for (int x = first_x; x <= last_x; x++) row[x] = color;
    }
}

static void draw_setup_steps(uint16_t *buf, int band_y0, int active)
{
    for (int step = 0; step < 4; step++) {
        const uint16_t color = step <= active ? SWAP16(rgb565(32, 213, 255)) :
                                                SWAP16(rgb565(38, 46, 55));
        draw_disc(buf, band_y0, 212 + step * 14, 48, step == active ? 4 : 3,
                  color);
    }
}

static void face_target(int expression, int index, float *x, float *y)
{
    const float tau = 6.28318530718f;
    if (index < 72) {
        const float angle = tau * index / 72.0f;
        *x = 233.0f + cosf(angle) * 112.0f;
        *y = 202.0f + sinf(angle) * 112.0f;
        return;
    }

    const bool right_eye = index >= 96 && index < 120;
    if (index < 120) {
        const int local = right_eye ? index - 96 : index - 72;
        const float t = (float)local / 23.0f;
        const float cx = right_eye ? 276.0f : 190.0f;
        if ((expression == 1 && !right_eye) || expression == 2) {
            const float u = t * 2.0f - 1.0f;
            *x = cx + u * 19.0f;
            *y = 171.0f - (1.0f - u * u) * (expression == 2 ? 11.0f : 7.0f);
        } else if (expression == 3) {
            const float u = t * 2.0f - 1.0f;
            *x = cx + u * 18.0f;
            *y = 168.0f - (1.0f - u * u) * 10.0f;
        } else {
            const float angle = tau * t;
            *x = cx + cosf(angle) * 11.0f;
            *y = 169.0f + sinf(angle) * 14.0f;
        }
        return;
    }

    const int local = index - 120;
    const float t = (float)local / 39.0f;
    if (expression == 3) {
        const float angle = tau * t;
        *x = 233.0f + cosf(angle) * 25.0f;
        *y = 232.0f + sinf(angle) * 31.0f;
    } else {
        const float u = t * 2.0f - 1.0f;
        const float width = expression == 2 ? 58.0f : 48.0f;
        const float depth = expression == 2 ? 39.0f : 30.0f;
        *x = 233.0f + u * width + (expression == 1 ? u * 4.0f : 0.0f);
        *y = 209.0f + (1.0f - u * u) * depth;
    }
}

static void draw_face_band(uint16_t *buf, int band_y0, int expression,
                           int next_expression, float morph, float phase)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    for (int index = 0; index < 160; index++) {
        float x0, y0, x1, y1;
        face_target(expression, index, &x0, &y0);
        face_target(next_expression, index, &x1, &y1);
        const float ease = morph * morph * (3.0f - 2.0f * morph);
        const float drift = sinf(phase + index * 1.731f) * 1.4f;
        const int x = (int)lroundf(x0 + (x1 - x0) * ease + drift);
        const int y = (int)lroundf(y0 + (y1 - y0) * ease +
                                   cosf(phase * 0.8f + index) * 1.0f);
        const bool accent = (index % 13) == 0 || (index > 119 && index % 7 == 0);
        const uint16_t glow = accent ? SWAP16(rgb565(47, 30, 115)) :
                                       SWAP16(rgb565(0, 58, 76));
        const uint16_t core = accent ? SWAP16(rgb565(133, 105, 255)) :
                                       SWAP16(rgb565(25, 221, 255));
        draw_disc(buf, band_y0, x, y, 4, glow);
        draw_disc(buf, band_y0, x, y, 2, core);
        if ((index & 5) == 0) {
            draw_disc(buf, band_y0, x - 1, y - 1, 1,
                      SWAP16(rgb565(225, 252, 255)));
        }
    }
}

void render_show_onboarding_faces(void)
{
    for (int expression = 0; expression < 4; expression++) {
        const int next = (expression + 1) % 4;
        for (int frame = 0; frame < 11; frame++) {
            const float morph = (float)frame / 10.0f;
            const float phase = (float)(expression * 11 + frame) * 0.24f;
            for (int band = 0; band < BAND_COUNT; band++) {
                uint16_t *buf = display_acquire_band();
                draw_face_band(buf, band * BAND_ROWS, expression, next,
                               morph, phase);
                display_flush_band(band, buf);
            }
            vTaskDelay(pdMS_TO_TICKS(42));
        }
        vTaskDelay(pdMS_TO_TICKS(180));
    }
}

static void draw_language_band(uint16_t *buf, int band_y0)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    draw_setup_steps(buf, band_y0, 1);
    draw_text_centered_spaced(buf, band_y0, "选择语言", 233, 82,
                              ui_font_message(), 2,
                              SWAP16(rgb565(245, 247, 250)));
    draw_text_centered_spaced(buf, band_y0, "Choose Language", 233, 116,
                              ui_font_brand_subtitle(), 1,
                              SWAP16(rgb565(116, 127, 139)));

    draw_round_rect(buf, band_y0, 64, 158, 402, 230, 24,
                    SWAP16(rgb565(12, 20, 27)));
    draw_round_rect(buf, band_y0, 66, 160, 400, 228, 22,
                    SWAP16(rgb565(8, 12, 17)));
    draw_large_disc(buf, band_y0, 105, 194, 21, SWAP16(rgb565(0, 116, 146)));
    draw_text_centered_spaced(buf, band_y0, "中文", 233, 181,
                              ui_font_message(), 3,
                              SWAP16(rgb565(243, 248, 250)));
    draw_line_round(buf, band_y0, 362, 187, 369, 194, 1,
                    SWAP16(rgb565(86, 204, 226)));
    draw_line_round(buf, band_y0, 369, 194, 362, 201, 1,
                    SWAP16(rgb565(86, 204, 226)));

    draw_round_rect(buf, band_y0, 64, 252, 402, 324, 24,
                    SWAP16(rgb565(24, 32, 43)));
    draw_round_rect(buf, band_y0, 66, 254, 400, 322, 22,
                    SWAP16(rgb565(8, 12, 17)));
    draw_large_disc(buf, band_y0, 105, 288, 21, SWAP16(rgb565(57, 45, 147)));
    draw_text_centered_spaced(buf, band_y0, "English", 233, 276,
                              ui_font_brand_subtitle(), 1,
                              SWAP16(rgb565(243, 248, 250)));
    draw_line_round(buf, band_y0, 362, 281, 369, 288, 1,
                    SWAP16(rgb565(124, 111, 255)));
    draw_line_round(buf, band_y0, 369, 288, 362, 295, 1,
                    SWAP16(rgb565(124, 111, 255)));
    draw_text_centered_spaced(buf, band_y0, "Tap to continue", 233, 373,
                              ui_font_brand_subtitle(), 1,
                              SWAP16(rgb565(85, 95, 106)));
}

void render_show_language_selection(void)
{
    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        draw_language_band(buf, band * BAND_ROWS);
        display_flush_band(band, buf);
    }
}

static uint16_t boot_fade_color(uint8_t red, uint8_t green, uint8_t blue,
                                float opacity)
{
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    return SWAP16(rgb565((uint8_t)lroundf(red * opacity),
                         (uint8_t)lroundf(green * opacity),
                         (uint8_t)lroundf(blue * opacity)));
}

static uint16_t boot_fade_rgb565(uint16_t source, float opacity)
{
    const uint8_t red = (uint8_t)(((source >> 11) & 0x1F) * 255 / 31);
    const uint8_t green = (uint8_t)(((source >> 5) & 0x3F) * 255 / 63);
    const uint8_t blue = (uint8_t)((source & 0x1F) * 255 / 31);
    return boot_fade_color(red, green, blue, opacity);
}

static void draw_boot_logo(uint16_t *buf, int band_y0, float opacity)
{
    const int logo_x = (LCD_H_RES - BOOT_LOGO_WIDTH) / 2;
    const int logo_y = (LCD_V_RES - BOOT_LOGO_HEIGHT) / 2;
    const int first_y = band_y0 > logo_y ? band_y0 : logo_y;
    const int last_y = band_y0 + BAND_ROWS < logo_y + BOOT_LOGO_HEIGHT ?
                       band_y0 + BAND_ROWS : logo_y + BOOT_LOGO_HEIGHT;
    for (int screen_y = first_y; screen_y < last_y; screen_y++) {
        const int source_y = screen_y - logo_y;
        uint16_t *destination = buf + (screen_y - band_y0) * LCD_H_RES + logo_x;
        const uint16_t *source = g_boot_logo_rgb565 + source_y * BOOT_LOGO_WIDTH;
        for (int x = 0; x < BOOT_LOGO_WIDTH; x++) {
            destination[x] = boot_fade_rgb565(source[x], opacity);
        }
    }
}

static void draw_boot_band(uint16_t *buf, int band_y0, float opacity,
                           uint8_t language)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    draw_boot_logo(buf, band_y0, opacity);
    const bool chinese = language == 0;
    const char *brand_name = chinese ? "时迹" : "ChronoTrace";
    const ui_font_t *brand_font = chinese ? ui_font_brand_title() :
                                           ui_font_brand_english();
    const int brand_y = chinese ? 402 : 407;
    const int brand_spacing = chinese ? 5 : 2;
    // A restrained amber halo gives the serif wordmark a crafted, luminous
    // character without softening its small-screen strokes.
    const uint16_t glow = boot_fade_color(153, 73, 18, opacity * 0.46f);
    draw_text_centered_spaced(buf, band_y0, brand_name, 232, brand_y,
                              brand_font, brand_spacing, glow);
    draw_text_centered_spaced(buf, band_y0, brand_name, 234, brand_y,
                              brand_font, brand_spacing, glow);
    draw_text_centered_spaced(buf, band_y0, brand_name, 233, brand_y - 1,
                              brand_font, brand_spacing, glow);
    draw_text_centered_spaced(buf, band_y0, brand_name, 233, brand_y + 1,
                              brand_font, brand_spacing, glow);
    draw_text_centered_spaced(buf, band_y0,
                              brand_name, 233, brand_y, brand_font,
                              brand_spacing,
                              boot_fade_color(255, 241, 221, opacity));
}

void render_show_boot_splash(uint8_t language)
{
    // A single calm reveal keeps the glass logo crisp against AMOLED black.
    // Smoothstep avoids visible brightness stepping at either end.
    for (int frame = 0; frame <= 28; frame++) {
        const float t = (float)frame / 28.0f;
        const float opacity = t * t * (3.0f - 2.0f * t);
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_boot_band(buf, band * BAND_ROWS, opacity, language);
            display_flush_band(band, buf);
        }
        vTaskDelay(pdMS_TO_TICKS(42));
    }
    vTaskDelay(pdMS_TO_TICKS(420));
    for (int frame = 1; frame <= 16; frame++) {
        const float t = (float)frame / 16.0f;
        const float smooth = t * t * (3.0f - 2.0f * t);
        const float opacity = 1.0f - smooth;
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_boot_band(buf, band * BAND_ROWS, opacity, language);
            display_flush_band(band, buf);
        }
        vTaskDelay(pdMS_TO_TICKS(42));
    }

    // The following fluid frame must repaint every band over the splash.
    for (int band = 0; band < BAND_COUNT; band++) s_band_used_prev[band] = true;
    s_force_full = true;
}

static void draw_bluetooth_icon(uint16_t *buf, int band_y0, int cx, int cy,
                                uint16_t color)
{
    draw_line_round(buf, band_y0, cx, cy - 34, cx, cy + 34, 2, color);
    draw_line_round(buf, band_y0, cx, cy - 34, cx + 20, cy - 16, 2, color);
    draw_line_round(buf, band_y0, cx + 20, cy - 16, cx - 17, cy + 17, 2, color);
    draw_line_round(buf, band_y0, cx - 17, cy - 17, cx + 20, cy + 16, 2, color);
    draw_line_round(buf, band_y0, cx + 20, cy + 16, cx, cy + 34, 2, color);
}

static void draw_bluetooth_band(uint16_t *buf, int band_y0, uint8_t language,
                                bool connected, bool error)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    draw_setup_steps(buf, band_y0, 3);
    draw_large_disc(buf, band_y0, 233, 133, 48, SWAP16(rgb565(5, 30, 42)));
    draw_large_disc(buf, band_y0, 233, 133, 43, SWAP16(rgb565(5, 11, 17)));
    draw_bluetooth_icon(buf, band_y0, 233, 133,
                        SWAP16(rgb565(26, 219, 255)));

    const ui_font_t *font = language == 0 ? ui_font_message() :
                                                 ui_font_brand_subtitle();
    draw_text_centered_spaced(buf, band_y0,
                              language == 0 ? "蓝牙连接" : "Bluetooth",
                              233, 198, font, language == 0 ? 2 : 1,
                              SWAP16(rgb565(244, 247, 250)));
    draw_round_rect(buf, band_y0, 75, 244, 391, 302, 22,
                    SWAP16(rgb565(12, 19, 26)));
    draw_text_centered_spaced(buf, band_y0, "ChronoTrace", 233, 262,
                              ui_font_brand_subtitle(), 2,
                              SWAP16(rgb565(42, 219, 255)));

    const char *status = error ?
        (language == 0 ? "蓝牙启动失败" : "Bluetooth unavailable") :
        connected ? (language == 0 ? "已连接" : "Connected") :
                    (language == 0 ? "正在等待手机连接" : "Waiting for phone");
    draw_text_centered_spaced(buf, band_y0, status, 233, 320,
                              language == 0 ? ui_font_message() :
                                              ui_font_brand_subtitle(),
                              language == 0 ? 1 : 0,
                              connected ? SWAP16(rgb565(76, 231, 157)) :
                                          SWAP16(rgb565(134, 146, 158)));

    draw_round_rect(buf, band_y0, 52, 376, 218, 425, 22,
                    SWAP16(rgb565(18, 23, 30)));
    draw_round_rect(buf, band_y0, 248, 376, 414, 425, 22,
                    SWAP16(rgb565(0, 119, 153)));
    draw_text_centered_spaced(buf, band_y0,
                              language == 0 ? "稍后" : "Later", 135, 389,
                              language == 0 ? ui_font_message() :
                                              ui_font_brand_subtitle(), 1,
                              SWAP16(rgb565(177, 187, 196)));
    draw_text_centered_spaced(buf, band_y0,
                              language == 0 ? "继续" : "Continue", 331, 389,
                              language == 0 ? ui_font_message() :
                                              ui_font_brand_subtitle(), 1,
                              SWAP16(rgb565(242, 251, 253)));
}

void render_show_bluetooth_setup(uint8_t language, bool connected, bool error)
{
    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        draw_bluetooth_band(buf, band * BAND_ROWS, language, connected, error);
        display_flush_band(band, buf);
    }
}

static void draw_settings_particle_icon(uint16_t *buf, int band_y0,
                                        int cx, int cy, int row)
{
    static const uint8_t colors[6][3] = {
        {30, 216, 255}, {115, 105, 255},
        {255, 106, 150}, {255, 196, 65},
        {74, 232, 170}, {194, 112, 255},
    };
    const float now = (float)(esp_timer_get_time() % 12000000LL) / 1000000.0f;
    const uint16_t core = SWAP16(rgb565(colors[row][0], colors[row][1],
                                        colors[row][2]));
    const uint16_t glow = SWAP16(rgb565(colors[row][0] / 3,
                                        colors[row][1] / 3,
                                        colors[row][2] / 3));
    // Nine equally spaced particles form a clean circular status emblem. The
    // ring rotates gently while retaining its shape.
    for (int i = 0; i < 9; i++) {
        const float phase = now * (0.42f + row * 0.035f) +
                            (float)i * 0.6981317f;
        const float radius = 10.0f;
        const int x = cx + (int)lroundf(cosf(phase) * radius);
        const int y = cy + (int)lroundf(sinf(phase) * radius);
        const int dot = ((i + row) % 9 == 0) ? 3 : 2;
        draw_disc(buf, band_y0, x, y, dot + 2, glow);
        draw_disc(buf, band_y0, x, y, dot, core);
        if (dot >= 3) {
            draw_disc(buf, band_y0, x - 1, y - 1, 1,
                      SWAP16(rgb565(235, 252, 255)));
        }
    }
}

static void draw_settings_option(uint16_t *buf, int band_y0,
                                 int x0, int x1, int cy,
                                 const char *text, bool selected, bool enabled,
                                 uint8_t red, uint8_t green, uint8_t blue)
{
    const int y0 = cy - 18;
    const int y1 = cy + 18;
    if (selected) {
        draw_round_rect(buf, band_y0, x0 - 2, y0 - 2, x1 + 2, y1 + 2, 14,
                        SWAP16(rgb565(red / 3, green / 3, blue / 3)));
        draw_round_rect(buf, band_y0, x0, y0, x1, y1, 12,
                        SWAP16(rgb565(red / 5 + 8,
                                     green / 5 + 10,
                                     blue / 5 + 12)));
    } else {
        draw_round_rect(buf, band_y0, x0, y0, x1, y1, 12,
                        SWAP16(rgb565(18, 27, 35)));
    }
    const ui_font_t *font = ui_font_timer_label();
    const int text_top = cy - font->line_height / 2;
    const uint16_t color = selected ? SWAP16(rgb565(red, green, blue)) :
                           enabled ? SWAP16(rgb565(175, 187, 197)) :
                                     SWAP16(rgb565(72, 84, 94));
    draw_text_centered_spaced(buf, band_y0, text, (x0 + x1) / 2,
                              text_top, font, 0, color);
}

static void draw_settings_band(uint16_t *buf, int band_y0)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    const bool english = s_ui_language != 0;
    const bool device_page = s_settings_page != 0;
    draw_text_centered_spaced(buf, band_y0,
                              device_page ? (english ? "Device" : "设备设置") :
                                            (english ? "Connections" : "连接设置"),
                              233, 24, ui_font_message(), 1,
                              SWAP16(rgb565(244, 247, 250)));
    // Two quiet page indicators retain the round-screen hierarchy without
    // consuming another text row.
    draw_disc(buf, band_y0, 224, 67, device_page ? 3 : 5,
              device_page ? SWAP16(rgb565(64, 78, 90)) :
                            SWAP16(rgb565(30, 216, 255)));
    draw_disc(buf, band_y0, 242, 67, device_page ? 5 : 3,
              device_page ? SWAP16(rgb565(74, 232, 170)) :
                            SWAP16(rgb565(64, 78, 90)));
    const char *connection_labels[4] = {
        "Wi-Fi", english ? "Bluetooth" : "蓝牙",
        english ? "City" : "城市", english ? "Language" : "语言"
    };
    const char *device_labels[4] = {
        english ? "Sound" : "声音", english ? "Brightness" : "亮度",
        english ? "Haptic" : "震动",
        english ? "Reactive" : "音乐律动"
    };
    const int sound_choice = s_settings_volume == 0 ? 2 :
                             (s_settings_volume < 68 ? 1 : 0);
    const int brightness_choice = s_settings_brightness >= 78 ? 0 :
                                  (s_settings_brightness >= 37 ? 1 : 2);
    const int row_count = 4;
    for (int row = 0; row < row_count; row++) {
        const int top = 78 + row * 58;
        const int row_height = 48;
        draw_round_rect(buf, band_y0, 48, top, 418, top + row_height,
                        device_page ? 21 : 18,
                        SWAP16(rgb565(11, 18, 25)));
        draw_settings_particle_icon(buf, band_y0, 70, top + row_height / 2,
                                    device_page ? row + 2 : row);
        const ui_font_t *row_font = ui_font_timer_label();
        const int centered_top = top + (row_height - row_font->line_height) / 2;
        draw_text_centered_spaced(buf, band_y0,
                                  device_page ? device_labels[row] :
                                                connection_labels[row],
                                  139, centered_top,
                                  row_font, 0,
                                  SWAP16(rgb565(224, 230, 236)));
        const int cy = top + row_height / 2;
        if (!device_page && row == 0) {
            const char *status = english ? "Not connected" : "未连接";
            bool selected = false;
            if (s_settings_wifi_enabled && s_settings_wifi_state == 1) {
                status = english ? "Connecting" : "连接中";
            } else if (s_settings_wifi_enabled && s_settings_wifi_state == 2) {
                status = english ? "Connected" : "已连接";
                selected = true;
            } else if (s_settings_wifi_enabled && s_settings_wifi_state == 3) {
                status = english ? "Configuring" : "配置中";
            } else if (s_settings_wifi_enabled && s_settings_wifi_state == 4) {
                status = english ? "Failed" : "连接失败";
            }
            draw_settings_option(buf, band_y0, 194, 298, cy,
                                 s_settings_wifi_enabled ?
                                     (english ? "On" : "开启") :
                                     (english ? "Off" : "关闭"),
                                 s_settings_wifi_enabled, true,
                                 30, 216, 255);
            draw_settings_option(buf, band_y0, 306, 412, cy, status,
                                 selected, true, 74, 232, 170);
        } else if (!device_page && row == 1) {
            draw_settings_option(buf, band_y0, 194, 298, cy,
                                 s_settings_bluetooth_connected ?
                                     (english ? "Connected" : "已连接") :
                                 s_settings_bluetooth ?
                                     (english ? "On" : "开启") :
                                     (english ? "Off" : "关闭"),
                                 s_settings_bluetooth,
                                 true, 30, 216, 255);
            draw_settings_option(buf, band_y0, 306, 412, cy,
                                 s_settings_time_calibrating ?
                                     (english ? "Syncing" : "校准中") :
                                 s_settings_time_calibrated ?
                                     (english ? "Synced" : "已校准") :
                                     (english ? "Calibrate" : "校准"),
                                 s_settings_time_calibrating ||
                                     s_settings_time_calibrated,
                                 s_settings_bluetooth_connected,
                                 30, 216, 255);
        } else if (!device_page && row == 2) {
            draw_settings_option(buf, band_y0, 194, 298, cy,
                                 english ? "Auto" : "自动",
                                 s_settings_city_automatic, true,
                                 74, 232, 170);
            draw_settings_option(buf, band_y0, 306, 412, cy,
                                 english ? "Refresh" : "刷新",
                                 false, true,
                                 30, 216, 255);
        } else if (!device_page && row == 3) {
            draw_settings_option(buf, band_y0, 194, 298, cy,
                                 "中文", !english, true,
                                 115, 105, 255);
            draw_settings_option(buf, band_y0, 306, 412, cy,
                                 "English", english, true,
                                 115, 105, 255);
        } else if (device_page && row == 0) {
            const char *choices[3] = {
                english ? "High" : "大", english ? "Low" : "小",
                english ? "Mute" : "静音"
            };
            const int x0[3] = {194, 269, 344};
            const int x1[3] = {262, 337, 412};
            for (int i = 0; i < 3; i++) {
                draw_settings_option(buf, band_y0, x0[i], x1[i], cy,
                                     choices[i], sound_choice == i, true,
                                     255, 106, 150);
            }
        } else if (device_page && row == 1) {
            const char *choices[3] = {
                english ? "High" : "高", english ? "Mid" : "中",
                english ? "Low" : "低"
            };
            const int x0[3] = {194, 269, 344};
            const int x1[3] = {262, 337, 412};
            for (int i = 0; i < 3; i++) {
                draw_settings_option(buf, band_y0, x0[i], x1[i], cy,
                                     choices[i], brightness_choice == i, true,
                                     255, 196, 65);
            }
        } else if (device_page && row == 2) {
            draw_settings_option(buf, band_y0, 194, 298, cy,
                                 english ? "On" : "开", s_settings_haptic,
                                 true, 74, 232, 170);
            draw_settings_option(buf, band_y0, 306, 412, cy,
                                 english ? "Off" : "关", !s_settings_haptic,
                                 true, 74, 232, 170);
        } else {
            draw_settings_option(buf, band_y0, 194, 298, cy,
                                 english ? "On" : "开", s_reactive,
                                 true, 194, 112, 255);
            draw_settings_option(buf, band_y0, 306, 412, cy,
                                 english ? "Off" : "关", !s_reactive,
                                 true, 194, 112, 255);
        }
    }

    // Two balanced actions share the bottom row: leave settings on the left,
    // open the guide on the right. A small center gap makes both touch targets
    // unambiguous without adding particles or arrows.
    // Extend the outer ends beyond the panel. The round AMOLED aperture then
    // clips those ends to the same arc as the display edge, while the two
    // center-facing ends retain their soft independent corners.
    draw_round_rect(buf, band_y0, -28, 322, 230, 388, 24,
                    SWAP16(rgb565(72, 53, 10)));
    draw_round_rect(buf, band_y0, -26, 324, 228, 386, 22,
                    SWAP16(rgb565(247, 185, 36)));
    draw_round_rect(buf, band_y0, 236, 322, 493, 388, 24,
                    SWAP16(rgb565(8, 48, 92)));
    draw_round_rect(buf, band_y0, 238, 324, 491, 386, 22,
                    SWAP16(rgb565(28, 125, 245)));
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Exit" : "退出",
                               48, 324, 228, 386,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(27, 24, 16)));
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Operation Guide" : "操作指南",
                               238, 324, 418, 386,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(247, 250, 255)));
    draw_text_centered_spaced(buf, band_y0,
                              device_page ? (english ? "Swipe for connections" : "右滑进入连接设置") :
                                            (english ? "Swipe for device settings" : "左滑进入设备设置"),
                              233, 414, ui_font_timer_label(), 0,
                              SWAP16(rgb565(100, 116, 130)));
}

static void draw_wifi_key(uint16_t *buf, int band_y0, int x0, int x1,
                          int y0, int y1, const char *label, bool accent)
{
    draw_round_rect(buf, band_y0, x0, y0, x1, y1, 9,
                    accent ? SWAP16(rgb565(15, 83, 91)) :
                             SWAP16(rgb565(18, 27, 35)));
    draw_text_centered_in_rect(buf, band_y0, label, x0, y0, x1, y1,
                               ui_font_timer_label(), 0,
                               accent ? SWAP16(rgb565(91, 239, 222)) :
                                        SWAP16(rgb565(232, 238, 243)));
}

static void draw_wifi_list_actions(uint16_t *buf, int band_y0, bool english)
{
    const uint16_t yellow_edge = SWAP16(rgb565(72, 53, 10));
    const uint16_t yellow = SWAP16(rgb565(247, 185, 36));
    const uint16_t blue_edge = SWAP16(rgb565(8, 48, 92));
    const uint16_t blue = SWAP16(rgb565(28, 125, 245));
    draw_round_rect(buf, band_y0, -28, 390, 230, 448, 24, yellow_edge);
    draw_round_rect(buf, band_y0, -26, 392, 228, 446, 22, yellow);
    draw_round_rect(buf, band_y0, 236, 390, 493, 448, 24, blue_edge);
    draw_round_rect(buf, band_y0, 238, 392, 491, 446, 22, blue);
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Cancel" : "取消",
                               48, 392, 228, 446,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(27, 24, 16)));
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Refresh" : "刷新",
                               238, 392, 418, 446,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(247, 250, 255)));
}

static void draw_wifi_keyboard_actions(uint16_t *buf, int band_y0,
                                        bool english, bool city_editor)
{
    const uint16_t yellow_edge = SWAP16(rgb565(72, 53, 10));
    const uint16_t yellow = SWAP16(rgb565(247, 185, 36));
    const uint16_t blue_edge = SWAP16(rgb565(8, 48, 92));
    const uint16_t blue = SWAP16(rgb565(28, 125, 245));
    draw_round_rect(buf, band_y0, -28, 350, 230, 404, 24, yellow_edge);
    draw_round_rect(buf, band_y0, -26, 352, 228, 402, 22, yellow);
    draw_round_rect(buf, band_y0, 236, 350, 493, 404, 24, blue_edge);
    draw_round_rect(buf, band_y0, 238, 352, 491, 402, 22, blue);
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Cancel" : "取消",
                               48, 352, 228, 402,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(27, 24, 16)));
    draw_text_centered_in_rect(buf, band_y0,
                               city_editor ? (english ? "Confirm" : "确定") :
                                             (english ? "Connect" : "连接"),
                               238, 352, 418, 402,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(247, 250, 255)));
}

static void draw_settings_wifi_notice(uint16_t *buf, int band_y0)
{
    const uint8_t state = s_settings_wifi_notice;
    if (state == 0 || esp_timer_get_time() >= s_settings_wifi_notice_until) return;
    const bool english = s_ui_language != 0;
    const char *label = state == 1 ?
        (english ? "Wi-Fi Connecting" : "Wi-Fi 连接中") : state == 2 ?
        (english ? "Wi-Fi Connected" : "Wi-Fi 已连接") :
        (english ? "Wi-Fi Connection Failed" : "Wi-Fi 连接失败");
    const uint16_t edge = state == 1 ? SWAP16(rgb565(8, 70, 94)) :
                          state == 2 ? SWAP16(rgb565(11, 86, 61)) :
                                       SWAP16(rgb565(106, 31, 43));
    const uint16_t fill = state == 1 ? SWAP16(rgb565(13, 39, 53)) :
                          state == 2 ? SWAP16(rgb565(12, 48, 37)) :
                                       SWAP16(rgb565(55, 22, 29));
    const uint16_t text = state == 1 ? SWAP16(rgb565(85, 222, 255)) :
                          state == 2 ? SWAP16(rgb565(91, 239, 171)) :
                                       SWAP16(rgb565(255, 128, 143));
    draw_round_rect(buf, band_y0, 68, 196, 398, 258, 22, edge);
    draw_round_rect(buf, band_y0, 71, 199, 395, 255, 19, fill);
    draw_text_centered_in_rect(buf, band_y0, label, 80, 199, 386, 255,
                               ui_font_timer_label(), 0, text);
}

static void draw_wifi_keyboard_row(uint16_t *buf, int band_y0,
                                   const char *keys, int count, int y)
{
    const int width = count == 10 ? 36 : 38;
    const int gap = 3;
    const int total = count * width + (count - 1) * gap;
    int x = (LCD_H_RES - total) / 2;
    for (int i = 0; i < count; i++) {
        char label[2] = {keys[i], '\0'};
        draw_wifi_key(buf, band_y0, x, x + width - 1, y, y + 36, label, false);
        x += width + gap;
    }
}

static void draw_wifi_editor_band(uint16_t *buf, int band_y0)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    const bool english = s_ui_language != 0;
    if (s_wifi_editor_mode == 1) {
        draw_text_centered_spaced(buf, band_y0,
                                  english ? "Select Wi-Fi" : "选择 Wi-Fi",
                                  233, 29, ui_font_message(), 0,
                                  SWAP16(rgb565(244, 247, 250)));
        for (uint8_t i = 0; i < s_wifi_editor_ssid_count; i++) {
            const int top = 78 + i * 48;
            draw_round_rect(buf, band_y0, 50, top, 416, top + 40, 14,
                            SWAP16(rgb565(12, 24, 32)));
            draw_disc(buf, band_y0, 77, top + 20, 5,
                      SWAP16(rgb565(74, 232, 170)));
            draw_external_text_centered_in_rect(
                buf, band_y0, s_wifi_editor_ssids[i], 94, top,
                404, top + 40, ui_font_timer_label(),
                SWAP16(rgb565(229, 236, 241)));
        }
        if (s_wifi_editor_ssid_count == 0) {
            draw_text_centered_spaced(buf, band_y0,
                                      english ? "Searching for networks" : "网络搜索中",
                                      233, 207, ui_font_message(), 0,
                                      SWAP16(rgb565(130, 145, 157)));
        }
        draw_wifi_list_actions(buf, band_y0, english);
        return;
    }

    const bool city_editor = s_wifi_editor_mode == 3;
    draw_text_centered_spaced(buf, band_y0,
                              city_editor ? (english ? "Enter City" : "输入城市") :
                                            (english ? "Wi-Fi Password" : "输入 Wi-Fi 密码"),
                              233, 15, ui_font_message(), 0,
                              SWAP16(rgb565(244, 247, 250)));
    if (!city_editor && s_wifi_editor_selected < s_wifi_editor_ssid_count) {
        draw_external_text_centered_in_rect(
            buf, band_y0, s_wifi_editor_ssids[s_wifi_editor_selected],
            48, 48, 418, 76, ui_font_timer_label(),
            SWAP16(rgb565(74, 232, 170)));
    }
    draw_round_rect(buf, band_y0, 48, 80, 418, 116, 12,
                    SWAP16(rgb565(13, 24, 33)));
    char shown[65];
    strlcpy(shown, s_wifi_editor_password, sizeof(shown));
    draw_text_centered_in_rect(buf, band_y0, shown, 58, 80, 408, 116,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(232, 238, 243)));

    static const char lower[4][11] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
    };
    static const char upper[4][11] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };
    static const char symbols[4][11] = {
        "1234567890", "!@#$%^&*()", "-_+=[]{}", ".,:;?/\\|"
    };
    const char (*rows)[11] = s_wifi_editor_symbols ? symbols :
                              (s_wifi_editor_uppercase ? upper : lower);
    const int counts[4] = {10, 10, 9, s_wifi_editor_symbols ? 8 : 7};
    for (int row = 0; row < 4; row++) {
        draw_wifi_keyboard_row(buf, band_y0, rows[row], counts[row], 127 + row * 42);
    }
    draw_wifi_key(buf, band_y0, 45, 122, 299, 337,
                  s_wifi_editor_symbols ? "ABC" : "123", true);
    draw_wifi_key(buf, band_y0, 128, 205, 299, 337,
                  s_wifi_editor_uppercase ? "abc" : "ABC", true);
    draw_wifi_key(buf, band_y0, 211, 298, 299, 337,
                  english ? "Space" : "空格", false);
    draw_wifi_key(buf, band_y0, 304, 421, 299, 337,
                  english ? "Delete" : "删除", false);
    draw_wifi_keyboard_actions(buf, band_y0, english, city_editor);
}

static const char *weather_condition(uint8_t code, bool english)
{
    if (code == 0) return english ? "Clear" : "晴";
    if (code <= 2) return english ? "Partly cloudy" : "多云";
    if (code == 3) return english ? "Cloudy" : "阴";
    if (code == 45 || code == 48) return english ? "Fog" : "雾";
    if (code >= 71 && code <= 77) return english ? "Snow" : "雪";
    if (code >= 95) return english ? "Thunderstorm" : "雷雨";
    return english ? "Rain" : "雨";
}

static void draw_weather_icon(uint16_t *buf, int band_y0, int cx, int cy,
                              uint8_t code)
{
    const uint16_t yellow = SWAP16(rgb565(255, 205, 64));
    const uint16_t cloud = SWAP16(rgb565(188, 218, 236));
    const uint16_t cyan = SWAP16(rgb565(50, 210, 255));
    const bool clear = code == 0;
    const bool snow = code >= 71 && code <= 77;
    const bool rain = (code >= 51 && code <= 67) ||
                      (code >= 80 && code <= 99);
    if (clear || code <= 2) {
        draw_disc(buf, band_y0, cx - (clear ? 0 : 18), cy - 6, 23, yellow);
        for (int i = 0; i < 8; i++) {
            const float angle = (float)i * 0.78539816f;
            draw_line_round(buf, band_y0,
                            cx + (int)(cosf(angle) * 31) - (clear ? 0 : 18),
                            cy - 6 + (int)(sinf(angle) * 31),
                            cx + (int)(cosf(angle) * 39) - (clear ? 0 : 18),
                            cy - 6 + (int)(sinf(angle) * 39), 2, yellow);
        }
    }
    if (!clear) {
        draw_disc(buf, band_y0, cx - 13, cy + 5, 20, cloud);
        draw_disc(buf, band_y0, cx + 12, cy - 3, 27, cloud);
        draw_disc(buf, band_y0, cx + 37, cy + 8, 18, cloud);
        draw_round_rect(buf, band_y0, cx - 33, cy + 3, cx + 55, cy + 25,
                        11, cloud);
    }
    if (rain || snow) {
        for (int i = -1; i <= 1; i++) {
            const int x = cx + i * 24;
            if (snow) {
                draw_line_round(buf, band_y0, x - 5, cy + 36, x + 5, cy + 46, 1, cyan);
                draw_line_round(buf, band_y0, x + 5, cy + 36, x - 5, cy + 46, 1, cyan);
            } else {
                draw_line_round(buf, band_y0, x + 4, cy + 34, x - 4, cy + 48, 2, cyan);
            }
        }
    }
}

static void draw_weather_band(uint16_t *buf, int band_y0)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    const bool english = s_ui_language != 0;
    const uint16_t white = SWAP16(rgb565(241, 246, 250));
    const uint16_t quiet = SWAP16(rgb565(124, 143, 158));
    const uint16_t cyan = SWAP16(rgb565(38, 218, 255));
    const uint16_t green = SWAP16(rgb565(74, 232, 170));
    draw_text_centered_spaced(buf, band_y0,
                              english ? "Weather" : "天气",
                              233, 24, ui_font_message(), 1, white);
    if (!s_weather_valid) {
        draw_weather_icon(buf, band_y0, 233, 175, 2);
        draw_text_centered_spaced(buf, band_y0,
                                  s_weather_state == 1 ?
                                      (english ? "Updating weather" : "天气正在更新") :
                                      (english ? "Connect Wi-Fi first" : "请先连接 Wi-Fi"),
                                  233, 259, ui_font_message(), 0, white);
        draw_text_centered_spaced(buf, band_y0,
                                  english ? "Swipe right to return" : "右滑返回",
                                  233, 407, ui_font_timer_label(), 0, quiet);
        return;
    }

    draw_external_text_centered_in_rect(buf, band_y0,
                                        s_weather_city[0] ? s_weather_city :
                                            (english ? "Current location" : "当前位置"),
                                        70, 59, 396, 91,
                                        ui_font_timer_label(), green);
    draw_weather_icon(buf, band_y0, 141, 157, s_weather_code);
    char temperature[16];
    snprintf(temperature, sizeof(temperature), "%.0f°", (double)s_weather_temperature);
    draw_text_centered_spaced(buf, band_y0, temperature, 310, 112,
                              ui_font_number_large(), 0, white);
    draw_text_centered_spaced(buf, band_y0,
                              weather_condition(s_weather_code, english),
                              310, 190, ui_font_timer_label(), 0, cyan);

    draw_round_rect(buf, band_y0, 52, 246, 414, 330, 22,
                    SWAP16(rgb565(10, 20, 28)));
    char left[32], right[32];
    snprintf(left, sizeof(left), english ? "Feels %.0f°" : "体感 %.0f°",
             (double)s_weather_apparent);
    snprintf(right, sizeof(right), english ? "Humidity %u%%" : "湿度 %u%%",
             (unsigned)s_weather_humidity);
    draw_text_centered_spaced(buf, band_y0, left, 145, 258,
                              ui_font_timer_label(), 0, white);
    draw_text_centered_spaced(buf, band_y0, right, 321, 258,
                              ui_font_timer_label(), 0, white);
    snprintf(left, sizeof(left), english ? "Wind %.0f km/h" : "风速 %.0f km/h",
             (double)s_weather_wind);
    draw_text_centered_spaced(buf, band_y0, left, 233, 300,
                              ui_font_timer_label(), 0, quiet);

    char updated[40];
    struct tm local = {0};
    time_t timestamp = (time_t)s_weather_updated;
    localtime_r(&timestamp, &local);
    snprintf(updated, sizeof(updated), english ? "Updated %02d:%02d" : "更新 %02d:%02d",
             local.tm_hour, local.tm_min);
    draw_text_centered_spaced(buf, band_y0, updated, 233, 357,
                              ui_font_timer_label(), 0, quiet);
    draw_text_centered_spaced(buf, band_y0,
                              english ? "Swipe right to return" : "右滑返回",
                              233, 407, ui_font_timer_label(), 0, quiet);
}

static void draw_shape_picker_miniature(uint16_t *buf, int band_y0,
                                        int item, int cx, int cy)
{
    enum { MINIATURE_SIZE = 26 };
    const int left = cx - MINIATURE_SIZE / 2;
    const int top = cy - MINIATURE_SIZE / 2;
    if (top + MINIATURE_SIZE <= band_y0 || top >= band_y0 + BAND_ROWS) return;

    uint8_t library_bitmap[HANDWRITING_BYTES];
    const uint8_t *bitmap = NULL;
    if (s_shape_picker_animation) {
        if (!animation_library_bitmap((uint8_t)item, 0.5f, library_bitmap)) return;
        bitmap = library_bitmap;
    } else if (item < SHAPE_LIBRARY_COUNT) {
        if (!shape_library_bitmap((uint8_t)item, library_bitmap)) return;
        bitmap = library_bitmap;
    } else {
        const uint8_t custom = (uint8_t)(item - SHAPE_LIBRARY_COUNT);
        bitmap = handwriting_glyph(custom);
        if (!bitmap) return;
    }

    // Text-adjacent picker miniatures use one calm cyan for a cleaner list.
    // Full-size built-in playback remains randomly coloured, while custom
    // drawings still retain their chosen colour outside this picker.
    uint8_t r, g, b;
    handwriting_color_rgb(HANDWRITING_DEFAULT_COLOR, &r, &g, &b);
    const uint16_t color = SWAP16(rgb565(r, g, b));
    for (int dy = 0; dy < MINIATURE_SIZE; dy++) {
        const int screen_y = top + dy;
        if (screen_y < band_y0 || screen_y >= band_y0 + BAND_ROWS) continue;
        const int source_y0 = dy * HANDWRITING_H / MINIATURE_SIZE;
        int source_y1 = (dy + 1) * HANDWRITING_H / MINIATURE_SIZE;
        if (source_y1 <= source_y0) source_y1 = source_y0 + 1;
        uint16_t *row = buf + (screen_y - band_y0) * LCD_H_RES;
        for (int dx = 0; dx < MINIATURE_SIZE; dx++) {
            const int source_x0 = dx * HANDWRITING_W / MINIATURE_SIZE;
            int source_x1 = (dx + 1) * HANDWRITING_W / MINIATURE_SIZE;
            if (source_x1 <= source_x0) source_x1 = source_x0 + 1;
            bool ink = false;
            for (int sy = source_y0; sy < source_y1 && !ink; sy++) {
                for (int sx = source_x0; sx < source_x1; sx++) {
                    const int bit = sy * HANDWRITING_W + sx;
                    if (bitmap[bit >> 3] & (1U << (bit & 7))) {
                        ink = true;
                        break;
                    }
                }
            }
            if (ink) row[left + dx] = color;
        }
    }
}

static void draw_shape_picker_band(uint16_t *buf, int band_y0)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    const bool english = s_ui_language != 0;
    const int total = s_shape_picker_animation ? ANIMATION_LIBRARY_COUNT :
                      SHAPE_LIBRARY_COUNT + s_shape_picker_custom_count;
    const int pages = total > 0 ?
        (total + SHAPE_PICKER_PAGE_SIZE - 1) / SHAPE_PICKER_PAGE_SIZE : 1;
    int page = s_shape_picker_page;
    if (page >= pages) page = pages - 1;
    const uint16_t white = SWAP16(rgb565(239, 246, 250));
    const uint16_t quiet = SWAP16(rgb565(115, 139, 151));
    const uint16_t cyan = SWAP16(rgb565(43, 219, 255));
    const uint16_t green = SWAP16(rgb565(74, 232, 170));

    draw_text_centered_spaced(buf, band_y0,
                              s_shape_picker_animation ?
                              (english ? "Animation Library" : "动画选择") :
                              (english ? "Shape Library" : "图形选择"),
                              233, 4, ui_font_message(), 0, white);
    char status[32];
    if (s_shape_picker_animation) {
        snprintf(status, sizeof(status), english ? "Swipe up/down  Selected %u" :
                 "上下滑动切换  已选%u个",
                 (unsigned)(s_animation_picker_selected ? 1 : 0));
    } else {
        snprintf(status, sizeof(status), english ? "Page %d/%d  Selected %u" :
                 "第%d/%d页  已选%u个", page + 1, pages,
                 (unsigned)s_shape_picker_selection_count);
    }
    draw_text_centered_spaced(buf, band_y0, status, 233, 34,
                              ui_font_timer_label(), 0, quiet);

    for (int cell = 0; cell < SHAPE_PICKER_PAGE_SIZE; cell++) {
        const int item = page * SHAPE_PICKER_PAGE_SIZE + cell;
        if (item >= total) break;
        const int column = cell & 1;
        const int row = cell >> 1;
        // The circular aperture is narrowest across the first row. Inset its
        // two cards symmetrically so wide miniatures such as Wave stay fully
        // visible instead of being clipped by the left edge of the AMOLED.
        const int x0 = column ? 242 : (row == 0 ? 72 : 42);
        const int x1 = column ? (row == 0 ? 394 : 424) : 224;
        const int y0 = 65 + row * 44;
        const int y1 = y0 + 40;
        const uint8_t rank = s_shape_picker_animation ?
                             (s_animation_picker_selected &&
                              item == s_animation_picker_item ? 1 : 0) :
                             s_shape_picker_rank[item];
        draw_round_rect(buf, band_y0, x0, y0, x1, y1, 14,
                        rank ? SWAP16(rgb565(13, 69, 76)) :
                               SWAP16(rgb565(10, 23, 30)));
        if (rank) {
            draw_round_rect(buf, band_y0, x0 + 3, y0 + 3, x1 - 3, y1 - 3, 11,
                            SWAP16(rgb565(8, 33, 40)));
        }
        char custom[20];
        const char *name;
        if (s_shape_picker_animation) {
            name = animation_library_name((uint8_t)item, english);
        } else if (item < SHAPE_LIBRARY_COUNT) {
            name = shape_library_name((uint8_t)item, english);
        } else {
            snprintf(custom, sizeof(custom), english ? "Custom %d" : "自绘%d",
                     item - SHAPE_LIBRARY_COUNT + 1);
            name = custom;
        }
        draw_shape_picker_miniature(buf, band_y0, item, x0 + 30,
                                    (y0 + y1) / 2);
        draw_text_centered_in_rect(buf, band_y0, name, x0 + 50, y0,
                                   x1 - (rank ? 29 : 8), y1,
                                   ui_font_timer_label(), 0,
                                   rank ? green : white);
        if (rank) {
            char number[5];
            snprintf(number, sizeof(number), "%u", (unsigned)rank);
            draw_disc(buf, band_y0, x1 - 16, y0 + 15, 10, cyan);
            draw_text_centered_in_rect(buf, band_y0, number,
                                       x1 - 26, y0 + 5, x1 - 6, y0 + 25,
                                       ui_font_timer_label(), 0,
                                       SWAP16(rgb565(2, 16, 20)));
        }
    }

    // Match the settings screen's strong yellow/blue edge-clipped actions.
    draw_round_rect(buf, band_y0, -28, 340, 230, 396, 22,
                    SWAP16(rgb565(72, 53, 10)));
    draw_round_rect(buf, band_y0, -26, 342, 228, 394, 20,
                    SWAP16(rgb565(247, 185, 36)));
    draw_round_rect(buf, band_y0, 236, 340, 493, 396, 22,
                    SWAP16(rgb565(8, 48, 92)));
    draw_round_rect(buf, band_y0, 238, 342, 491, 394, 20,
                    SWAP16(rgb565(28, 125, 245)));
    draw_text_centered_in_rect(buf, band_y0,
                               s_shape_picker_animation ?
                               (english ? "Shapes" : "图形") :
                               (english ? "Draw" : "自绘"),
                               48, 342, 228, 394,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(27, 24, 16)));
    draw_text_centered_in_rect(buf, band_y0, english ? "Play" : "播放",
                               238, 342, 418, 394,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(247, 250, 255)));
    if (s_shape_picker_animation) {
        // Animation items cannot be deleted. Use the whole circular bottom
        // cap as one generous exit target while retaining the same flat style.
        const uint16_t exit_green = SWAP16(rgb565(35, 205, 110));
        const int cx = LCD_H_RES / 2;
        const int cy = LCD_V_RES / 2;
        const int radius = LCD_H_RES / 2 - 1;
        for (int x = 0; x < LCD_H_RES; x++) {
            const int dx = x - cx;
            const int bottom = cy + (int)lroundf(sqrtf(fmaxf(
                0.0f, (float)(radius * radius - dx * dx))));
            for (int y = 409; y <= bottom; y++) {
                if (y < band_y0 || y >= band_y0 + BAND_ROWS) continue;
                buf[(y - band_y0) * LCD_H_RES + x] = exit_green;
            }
        }
        draw_text_centered_in_rect(buf, band_y0,
                                   english ? "Exit" : "退出",
                                   110, 410, 356, 452,
                                   ui_font_timer_label(), 0,
                                   SWAP16(rgb565(255, 255, 255)));
        return;
    }
    bool delete_enabled = false;
    for (int item = SHAPE_LIBRARY_COUNT; item < total; item++) {
        if (s_shape_picker_rank[item]) {
            delete_enabled = true;
            break;
        }
    }
    // Split the bottom cap with the same five-pixel minimum center gap and
    // 22-pixel inward corner radius as Draw/Play.
    const uint16_t exit_green = SWAP16(rgb565(35, 205, 110));
    const uint16_t action_red = SWAP16(rgb565(255, 45, 55));
    const uint16_t delete_text = delete_enabled ?
        SWAP16(rgb565(255, 255, 255)) : SWAP16(rgb565(255, 238, 240));
    const int bottom_center_x = LCD_H_RES / 2;
    const int bottom_center_y = LCD_V_RES / 2;
    const int bottom_radius = LCD_H_RES / 2 - 1;
    for (int x = 0; x < LCD_H_RES; x++) {
        if (x > 230 && x < 236) continue;
        const int dx = x - bottom_center_x;
        const int bottom = bottom_center_y + (int)lroundf(sqrtf(fmaxf(
            0.0f, (float)(bottom_radius * bottom_radius - dx * dx))));
        int top = 409;
        if (x > 208 && x <= 230) {
            const int corner_dx = x - 208;
            top = 431 - (int)sqrtf((float)(22 * 22 - corner_dx * corner_dx));
        } else if (x >= 236 && x < 258) {
            const int corner_dx = 258 - x;
            top = 431 - (int)sqrtf((float)(22 * 22 - corner_dx * corner_dx));
        }
        if (bottom < top) continue;
        const uint16_t color = x <= 230 ? exit_green : action_red;
        for (int y = top; y <= bottom; y++) {
            if (y < band_y0 || y >= band_y0 + BAND_ROWS) continue;
            buf[(y - band_y0) * LCD_H_RES + x] = color;
        }
    }
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Exit" : "退出",
                               102, 410, 230, 452,
                               ui_font_timer_label(), 0,
                               SWAP16(rgb565(255, 255, 255)));
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Delete" : "删除",
                               236, 410, 364, 452,
                               ui_font_timer_label(), 0, delete_text);
}

static void draw_operation_guide_band(uint16_t *buf, int band_y0)
{
    memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));
    const bool english = s_ui_language != 0;
    const uint8_t page = s_operation_guide_page;
    const uint16_t white = SWAP16(rgb565(238, 243, 247));
    const uint16_t cyan = SWAP16(rgb565(30, 216, 255));
    const uint16_t green = SWAP16(rgb565(74, 232, 170));

    draw_text_centered_spaced(buf, band_y0,
                              english ? "Operation Guide" : "操作指南",
                              233, 25, ui_font_message(), 0, white);
    draw_settings_option(buf, band_y0, 82, 226, 78,
                         english ? "Buttons" : "按键",
                         page == 0, true, 255, 196, 65);
    draw_settings_option(buf, band_y0, 240, 384, 78,
                         english ? "Touch" : "触控",
                         page == 1, true, 74, 232, 170);

    const ui_font_t *font = ui_font_timer_label();
    if (page == 0) {
        draw_round_rect(buf, band_y0, 34, 110, 225, 359, 22,
                        SWAP16(rgb565(22, 20, 9)));
        draw_round_rect(buf, band_y0, 241, 110, 432, 359, 22,
                        SWAP16(rgb565(7, 21, 29)));
        draw_settings_particle_icon(buf, band_y0, 73, 142, 3);
        draw_settings_particle_icon(buf, band_y0, 280, 142, 0);
        draw_text_centered_spaced(buf, band_y0,
                                  english ? "Yellow" : "黄键",
                                  145, 130, font, 0,
                                  SWAP16(rgb565(255, 214, 83)));
        draw_text_centered_spaced(buf, band_y0,
                                  english ? "Blue" : "蓝键",
                                  352, 130, font, 0, cyan);

        const char *yellow_actions[3] = {
            english ? "Tap  Draw" : "单击  手写",
            english ? "Double  Timer" : "双击  倒计时",
            english ? "Hold  Density" : "长按  粒子数量",
        };
        const char *blue_actions[3] = {
            english ? "Tap  Theme" : "单击  主题",
            english ? "Double  Battery" : "双击  电量",
            english ? "Hold  Reactive" : "长按  音乐律动",
        };
        for (int i = 0; i < 3; i++) {
            const int top = 184 + i * 51;
            draw_text_centered_spaced(buf, band_y0, yellow_actions[i],
                                      130, top, font, 0, white);
            draw_text_centered_spaced(buf, band_y0, blue_actions[i],
                                      336, top, font, 0, white);
            if (i < 2) {
                draw_line_round(buf, band_y0, 55, top + 33, 204, top + 33,
                                0, SWAP16(rgb565(55, 53, 34)));
                draw_line_round(buf, band_y0, 262, top + 33, 411, top + 33,
                                0, SWAP16(rgb565(31, 54, 66)));
            }
        }
    } else {
        draw_round_rect(buf, band_y0, 42, 110, 424, 365, 24,
                        SWAP16(rgb565(9, 20, 24)));
        const char *gestures[7] = {
            english ? "Tap  -  Show time" : "点击  ·  显示时间",
            english ? "Hold  -  Clock / Shape picker" : "长按  ·  时钟／图形选择",
            english ? "Double  -  Random / Restore" : "双击  ·  随机图形／恢复手写",
            english ? "Swipe left  -  Weather" : "向左滑动  ·  粒子天气",
            english ? "Swipe right  -  Shape library" : "向右滑动  ·  图形库",
            english ? "Swipe up/down  -  Volume" : "上下滑动  ·  音量大小",
            english ? "Timer  -  Tap pause / Double exit" : "倒计时  ·  点击暂停／双击退出",
        };
        for (int i = 0; i < 7; i++) {
            const int top = 121 + i * 34;
            const int dot_x = 67;
            const uint16_t dot_color = (i & 1) ? green : cyan;
            draw_disc(buf, band_y0, dot_x, top + 11, 5, dot_color);
            draw_text_centered_spaced(buf, band_y0, gestures[i],
                                      246, top, font, 0, white);
        }
    }

    // Match the settings screen's paired edge actions. The outer ends extend
    // beyond the panel so the round AMOLED aperture supplies their arc.
    draw_round_rect(buf, band_y0, -28, 374, 230, 432, 22,
                    SWAP16(rgb565(72, 53, 10)));
    draw_round_rect(buf, band_y0, -26, 376, 228, 430, 20,
                    SWAP16(rgb565(247, 185, 36)));
    draw_round_rect(buf, band_y0, 236, 374, 493, 432, 22,
                    SWAP16(rgb565(8, 48, 92)));
    draw_round_rect(buf, band_y0, 238, 376, 491, 430, 20,
                    SWAP16(rgb565(28, 125, 245)));
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Return" : "返回",
                               48, 376, 228, 430,
                               font, 0, SWAP16(rgb565(27, 24, 16)));
    draw_text_centered_in_rect(buf, band_y0,
                               english ? "Settings" : "设置",
                               238, 376, 418, 430,
                               font, 0, SWAP16(rgb565(247, 250, 255)));
}

static void draw_hourglass(uint16_t *buf, int band_y0)
{
    draw_rgba_icon(buf, band_y0, countdown_timer_icon_rgba,
                   COUNTDOWN_TIMER_ICON_W, COUNTDOWN_TIMER_ICON_H,
                   kTimerUI.center_x, kTimerUI.hourglass_y);
}

static void draw_play_pause_button(uint16_t *buf, int band_y0,
                                   bool show_pause)
{
    const int cx = kTimerUI.center_x;
    const int cy = kTimerUI.button_y;
    if (!show_pause) {
        draw_rgba_icon(buf, band_y0, countdown_play_icon_rgba,
                       COUNTDOWN_PLAY_ICON_W, COUNTDOWN_PLAY_ICON_H, cx, cy);
        return;
    }
    const int radius = kTimerUI.button_visual_radius;
    draw_large_disc(buf, band_y0, cx, cy, radius + 3,
                    SWAP16(rgb565(3, 40, 43)));
    draw_large_disc(buf, band_y0, cx, cy, radius,
                    SWAP16(rgb565(34, 221, 229)));
    draw_large_disc(buf, band_y0, cx, cy, radius - 2,
                    SWAP16(rgb565(7, 11, 13)));
    const uint16_t icon = SWAP16(rgb565(247, 248, 250));
    for (int y = cy - 11; y <= cy + 11; y++) {
        if (y < band_y0 || y >= band_y0 + BAND_ROWS) continue;
        uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
        for (int x = cx - 8; x <= cx - 3; x++) row[x] = icon;
        for (int x = cx + 3; x <= cx + 8; x++) row[x] = icon;
    }
}

static void draw_timer_text(uint16_t *buf, int band_y0, bool active,
                            int remaining_seconds)
{
    char value[12];
    const ui_font_t *number_font;
    if (active) {
        if (remaining_seconds < 0) remaining_seconds = 0;
        if (remaining_seconds > 3600) remaining_seconds = 3600;
        const unsigned minutes = (unsigned)remaining_seconds / 60U;
        const unsigned seconds = (unsigned)remaining_seconds % 60U;
        snprintf(value, sizeof(value), "%02u:%02u", minutes, seconds);
        number_font = ui_font_number_medium();
    } else {
        snprintf(value, sizeof(value), "%u", (unsigned)s_countdown_minutes);
        number_font = ui_font_number_large();
    }
    const int number_top = kTimerUI.number_center_y - number_font->line_height / 2;
    if (active) {
        draw_text_centered_font(buf, band_y0, value, kTimerUI.center_x,
                                number_top, number_font);
    } else {
        // Match the selected minute value to the exact colour beneath its
        // progress handle. This uses the same fixed palette interpolation as
        // the dial, so the number changes continuously while dragging.
        const uint16_t selected_color =
            timer_color((float)s_countdown_minutes / 60.0f, 1.0f);
        draw_text_centered_spaced(buf, band_y0, value, kTimerUI.center_x,
                                  number_top, number_font, 0, selected_color);
    }
    if (!active) {
        const ui_font_t *label_font = ui_font_message();
        const int label_top = kTimerUI.label_center_y - label_font->line_height / 2;
        draw_text_centered_spaced(buf, band_y0, s_ui_language ? "MINUTES" : "分钟", kTimerUI.center_x,
                                  label_top, label_font, 1,
                                  SWAP16(rgb565(229, 232, 235)));
    }
}

static void draw_timer_screen(uint16_t *buf, int band_y0)
{
    const bool active = s_countdown_runtime_active;
    const float progress = active ? s_countdown_progress :
                           (float)s_countdown_minutes / 60.0f;
    draw_rainbow_ticks(buf, band_y0, active, progress);
    draw_timer_minute_labels(buf, band_y0);
    draw_progress_marker(buf, band_y0, progress);
    draw_hourglass(buf, band_y0);
    draw_timer_text(buf, band_y0, active, s_countdown_remaining_seconds);
    draw_play_pause_button(buf, band_y0,
                           active && !s_countdown_runtime_paused);
}

static void project_all(int n)
{
    const float speed_scale = (float)(SPEED_LEVELS - 1) / SPEED_COLOR_MAX;
    const float depth_scale = (float)(DEPTH_LEVELS - 1) / BOX_D;

    memset(s_band_used, 0, sizeof(s_band_used));

    for (int i = 0; i < n; i++) {
        if (s_snapshot[i].formed_digit == 2) {
            s_sx[i] = -1000;
            s_sy[i] = -1000;
            s_sr[i] = 1;
            continue;
        }
        int sx, sy;
        float scale;
        project(s_snapshot[i].x, s_snapshot[i].y, s_snapshot[i].z, &sx, &sy, &scale);

        s_sx[i] = (int16_t)sx;
        s_sy[i] = (int16_t)sy;

        int r = (int)(PARTICLE_RADIUS_PX * scale + 0.5f);
        if (s_snapshot[i].formed_digit == 1) {
            // Finer clock/countdown ink leaves stable counters in 0/6/8/9.
            // Halo and freely moving liquid retain the original full body.
            r = (r * 4 + 2) / 5;
        } else if (s_snapshot[i].formed_digit == 3) {
            // Weather uses fewer target particles so the liquid pool stays
            // full. A slightly larger disc joins the sparse icon cells.
            r = (r * 6 + 2) / 5;
        } else if (s_snapshot[i].formed_digit == 4) {
            // Temperature numerals deliberately stay one bead thick. Smaller
            // isolated discs preserve the seven-segment counters and prevent
            // adjacent strokes from merging on the AMOLED panel.
            r = (r * 3 + 2) / 5;
        }
        if (r < 1) r = 1;
        if (r > DISC_MAX_R) r = DISC_MAX_R;
        s_sr[i] = (uint8_t)r;

        int sl = (int)(s_snapshot[i].speed * speed_scale);
        if (s_reactive) {
            sl += (int)(s_audio_mid * 15.0f + s_audio_treble * 8.0f);
        }
        if (sl < 0) sl = 0;
        if (sl > SPEED_LEVELS - 1) sl = SPEED_LEVELS - 1;

        int dl = (int)(s_snapshot[i].z * depth_scale + 0.5f);
        if (dl < 0) dl = 0;
        if (dl > DEPTH_LEVELS - 1) dl = DEPTH_LEVELS - 1;

        const int lut = dl * SPEED_LEVELS + sl;
        if (s_snapshot[i].color < HANDWRITING_COLOR_COUNT) {
            uint8_t base_r, base_g, base_b;
            handwriting_color_rgb(s_snapshot[i].color,
                                  &base_r, &base_g, &base_b);
            int theme_r, theme_g, theme_b;
            const float palette_t = SPEED_LEVELS > 1 ?
                (float)sl / (float)(SPEED_LEVELS - 1) : 0.0f;
            speed_color(palette_t, &theme_r, &theme_g, &theme_b);
            // Keep the per-glyph colour identity while allowing the same
            // slow palette wave used by the particle clock to travel through
            // the handwriting. A 42% theme mix is visible without turning
            // separately selected characters into one uniform theme colour.
            const float theme_mix = 0.42f;
            const int r = (int)((float)base_r * (1.0f - theme_mix) +
                                (float)theme_r * theme_mix);
            const int g = (int)((float)base_g * (1.0f - theme_mix) +
                                (float)theme_g * theme_mix);
            const int b = (int)((float)base_b * (1.0f - theme_mix) +
                                (float)theme_b * theme_mix);
            const float depth_t = (DEPTH_LEVELS > 1) ?
                (float)dl / (float)(DEPTH_LEVELS - 1) : 0.0f;
            const float dim = 0.58f + 0.42f * (1.0f - depth_t);
            s_sc[i] = SWAP16(rgb565((int)(r * dim), (int)(g * dim), (int)(b * dim)));
            s_sh[i] = SWAP16(rgb565((int)(r + (255 - r) * 0.72f),
                                      (int)(g + (255 - g) * 0.72f),
                                      (int)(b + (255 - b) * 0.72f)));
        } else {
            s_sc[i] = s_color_lut[lut];
            s_sh[i] = s_highlight_lut[lut];
        }

        // The highlight sits up and left of the disc by r/3 with radius r/2, so
        // it never reaches beyond the disc's own rows and this extent covers
        // both.
        const int top = sy - r;
        const int bot = sy + r;
        if (bot < 0 || top >= LCD_V_RES) {
            continue;
        }
        const int b0 = (top < 0) ? 0 : top / BAND_ROWS;
        const int b1 = (bot >= LCD_V_RES) ? (BAND_COUNT - 1) : bot / BAND_ROWS;
        for (int b = b0; b <= b1; b++) {
            s_band_used[b] = true;
        }
    }
}

bool render_frame(void)
{
    static bool was_handwriting;
    static uint32_t handwriting_revision;
    static uint8_t handwriting_page;
    static uint8_t handwriting_color;
    static uint8_t handwriting_bitmap[HANDWRITING_BYTES];
    static uint32_t countdown_revision_seen;
    static uint32_t settings_revision_seen;
    static uint32_t settings_animation_seen;
    static uint32_t guide_revision_seen;
    static uint32_t guide_animation_seen;
    static uint32_t shape_picker_revision_seen;
    static uint32_t wifi_editor_revision_seen;
    static uint32_t weather_revision_seen;
    if (s_network_busy) return false;
    if (s_theme != s_requested_theme) {
        s_theme = s_requested_theme;
        build_color_lut();
        s_force_full = true;
    }

    if (s_wifi_editor_mode != 0) {
        const uint32_t revision = s_wifi_editor_revision;
        if (revision == wifi_editor_revision_seen && !s_force_full) return false;
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_wifi_editor_band(buf, band * BAND_ROWS);
            display_flush_band(band, buf);
        }
        wifi_editor_revision_seen = revision;
        s_force_full = false;
        return true;
    }
    if (wifi_editor_revision_seen != 0) {
        wifi_editor_revision_seen = 0;
        s_force_full = true;
    }

    if (s_weather_visible) {
        const uint32_t revision = s_weather_revision;
        if (revision == weather_revision_seen && !s_force_full) return false;
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_weather_band(buf, band * BAND_ROWS);
            display_flush_band(band, buf);
        }
        weather_revision_seen = revision;
        s_force_full = false;
        return true;
    }
    if (weather_revision_seen != 0) {
        weather_revision_seen = 0;
        s_force_full = true;
    }

    if (s_shape_picker_visible) {
        const uint32_t revision = s_shape_picker_revision;
        if (revision == shape_picker_revision_seen && !s_force_full) return false;
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_shape_picker_band(buf, band * BAND_ROWS);
            display_flush_band(band, buf);
        }
        shape_picker_revision_seen = revision;
        s_force_full = false;
        return true;
    }
    if (shape_picker_revision_seen != 0) {
        shape_picker_revision_seen = 0;
        s_force_full = true;
    }

    if (s_operation_guide_visible) {
        const uint32_t revision = s_operation_guide_revision;
        const uint32_t animation = (uint32_t)(esp_timer_get_time() / 120000LL);
        if (revision == guide_revision_seen &&
            animation == guide_animation_seen && !s_force_full) return false;
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_operation_guide_band(buf, band * BAND_ROWS);
            display_flush_band(band, buf);
        }
        guide_revision_seen = revision;
        guide_animation_seen = animation;
        s_force_full = false;
        return true;
    }
    if (guide_revision_seen != 0) {
        guide_revision_seen = 0;
        s_force_full = true;
    }

    if (s_settings_visible) {
        const uint32_t revision = s_settings_revision;
        const uint32_t animation = (uint32_t)(esp_timer_get_time() / 120000LL);
        if (revision == settings_revision_seen &&
            animation == settings_animation_seen && !s_force_full) return false;
        for (int band = 0; band < BAND_COUNT; band++) {
            uint16_t *buf = display_acquire_band();
            draw_settings_band(buf, band * BAND_ROWS);
            draw_settings_wifi_notice(buf, band * BAND_ROWS);
            display_flush_band(band, buf);
        }
        settings_revision_seen = revision;
        settings_animation_seen = animation;
        s_force_full = false;
        return true;
    }
    if (settings_revision_seen != 0) {
        settings_revision_seen = 0;
        s_force_full = true;
        for (int band = 0; band < BAND_COUNT; band++) s_band_used_prev[band] = true;
    }

    handwriting_view_t handwriting;
    handwriting_snapshot(&handwriting);
    if (handwriting.active) {
        if (was_handwriting && handwriting.revision == handwriting_revision) {
            return false;
        }
        bool dirty[BAND_COUNT] = {false};
        bool any_dirty = false;
        if (!was_handwriting || handwriting.page != handwriting_page ||
            handwriting.color != handwriting_color) {
            memset(dirty, 1, sizeof(dirty));
            any_dirty = true;
        } else {
            // Each bitmap row occupies 4-5 display rows. Redraw only the panel
            // bands touched by rows whose bits actually changed; AMOLED keeps
            // the static title, frame, and curved actions without refreshing.
            for (int row = 0; row < HANDWRITING_H; row++) {
                const uint8_t *now = handwriting.bitmap + row * (HANDWRITING_W / 8);
                const uint8_t *before = handwriting_bitmap + row * (HANDWRITING_W / 8);
                if (memcmp(now, before, HANDWRITING_W / 8) == 0) continue;
                // The filtered edge reaches into the neighbouring source row.
                const int y0 = 70 + row * 288 / HANDWRITING_H - 5;
                const int y1 = 70 + (row + 1) * 288 / HANDWRITING_H + 4;
                for (int band = y0 / BAND_ROWS; band <= y1 / BAND_ROWS; band++) {
                    if (band >= 0 && band < BAND_COUNT) dirty[band] = true;
                }
                any_dirty = true;
            }
        }
        if (!any_dirty) {
            handwriting_revision = handwriting.revision;
            return false;
        }
        render_handwriting(&handwriting, dirty);
        handwriting_revision = handwriting.revision;
        handwriting_page = handwriting.page;
        handwriting_color = handwriting.color;
        memcpy(handwriting_bitmap, handwriting.bitmap, sizeof(handwriting_bitmap));
        was_handwriting = true;
        return true;
    }
    if (was_handwriting) {
        s_force_full = true;
        for (int b = 0; b < BAND_COUNT; b++) s_band_used_prev[b] = true;
        was_handwriting = false;
    }

    const bool timer_visible = s_countdown_menu || s_countdown_runtime_active;
    // Static selection frames remain retained by AMOLED. Runtime progress
    // increments the same revision at 10-20 Hz for smooth marker motion.
    if (timer_visible) {
        const uint32_t revision = s_countdown_revision;
        if (revision == countdown_revision_seen && !s_force_full) return false;
        countdown_revision_seen = revision;
    } else {
        countdown_revision_seen = 0;
    }

    int n = 0;
    if (!timer_visible) {
        n = sim_snapshot(s_snapshot, PARTICLE_MAX);
        project_all(n);
    }
    if (timer_visible) {
        for (int band = 0; band < BAND_COUNT; band++) s_band_used[band] = true;
    }

    char message[MESSAGE_MAX_BYTES];
    const bool message_pending = message_snapshot(message, sizeof(message));
    const bool message_active = message_pending && !timer_visible;
    int message_y0 = 0, message_y1 = 0;
    if (message_active) {
        const ui_font_t *font = ui_font_message();
        message_y0 = (LCD_V_RES - font->line_height) / 2;
        message_y1 = message_y0 + font->line_height - 1;
        const int first_band = message_y0 / BAND_ROWS;
        const int last_band = message_y1 / BAND_ROWS;
        for (int band = first_band; band <= last_band; band++) s_band_used[band] = true;
    }

    for (int band = 0; band < BAND_COUNT; band++) {
        // Empty now and empty last time means the panel is already showing black
        // here, so there is nothing to send.
        if (!s_force_full && !s_band_used[band] && !s_band_used_prev[band]) {
            continue;
        }

        const int band_y0 = band * BAND_ROWS;
        const int band_y1 = band_y0 + BAND_ROWS;

        uint16_t *buf = display_acquire_band();

        // Nothing is drawn but the fluid: no box, no wall, no frame. The
        // enclosure exists only in the physics, and reads purely from how the
        // particles pile up against it.
        fill_background(buf, band_y0);

        if (timer_visible) {
            draw_timer_background(buf);
            draw_timer_screen(buf, band_y0);
            display_flush_band(band, buf);
            continue;
        }

        // Reaching here with nothing to draw means the band held fluid last
        // frame and does not now: the clear above is the whole job.
        if (!s_band_used[band]) {
            display_flush_band(band, buf);
            continue;
        }

        // The simulation publishes particles sorted by depth ascending, so
        // walking backwards paints far particles first and near ones over the
        // top of them.
        for (int i = n - 1; i >= 0; i--) {
            const int r = s_sr[i];
            const int cy = s_sy[i];
            if (cy + r < band_y0 || cy - r >= band_y1) {
                continue;
            }
            draw_disc(buf, band_y0, s_sx[i], cy, r, s_sc[i]);

#if HIGHLIGHT_ENABLE
            // Offset up and left, so every particle is lit from the same
            // direction and the whole body of fluid looks rounded.
            if (r >= 3) {
                draw_disc(buf, band_y0, s_sx[i] - r / 3, cy - r / 3, r / 2, s_sh[i]);
            }
#endif
        }

        if (message_active && band_y1 > message_y0 && band_y0 <= message_y1) {
            draw_message(buf, band_y0, message);
        }

        display_flush_band(band, buf);
    }

    memcpy(s_band_used_prev, s_band_used, sizeof(s_band_used));
    s_force_full = false;
    return true;
}
