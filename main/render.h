#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sim.h"

#ifdef __cplusplus
extern "C" {
#endif

// Draws one frame of the fluid, band by band, straight to the panel.
//
// The renderer is a plain software rasteriser: it projects each particle
// through a pinhole camera, picks a colour from a precomputed ramp, and fills
// a disc. There is no framebuffer for the whole screen; see display.h.

void render_init(void);
void render_set_language(uint8_t language);

// Shows the ChronoTrace brand screen before the simulation tasks start.
// It is rendered from vector primitives and the subset fonts, not a full-screen
// bitmap, keeping the AMOLED image crisp without a large firmware asset.
void render_show_boot_splash(uint8_t language);

// First-use onboarding screens. The face animation is self-timed; the two
// selection screens remain on the AMOLED until the caller redraws them.
void render_show_onboarding_faces(void);
void render_show_language_selection(void);
void render_show_bluetooth_setup(uint8_t language, bool connected, bool error);
void render_set_settings(bool visible, uint8_t language, bool bluetooth_on,
                         bool bluetooth_connected, bool time_calibrating,
                         bool time_calibrated,
                         uint8_t volume, uint8_t brightness,
                         bool haptic_enabled, uint8_t page,
                         bool wifi_enabled, uint8_t wifi_state,
                         uint8_t weather_action);
// Temporary result banner shown above the settings page: 1 connecting,
// 2 connected, 3 failed, 0 hidden.
void render_set_wifi_notice(uint8_t state);
// Full-screen device-side Wi-Fi selector and touch keyboard. Mode 0 hides the
// editor, mode 1 shows the scanned network list, and mode 2 shows the password
// keyboard for the selected network.
void render_set_wifi_editor(uint8_t mode, uint8_t language,
                            const char (*ssids)[33], uint8_t ssid_count,
                            uint8_t selected, const char *password,
                            bool uppercase, bool symbols, bool reveal);
// Full-screen, bilingual operation guide reached from the settings page.
// Page 0 documents the physical buttons; page 1 documents touch gestures.
void render_set_operation_guide(bool visible, uint8_t language, uint8_t page);
// Full-screen product information reached from the device settings page.
void render_set_about(bool visible, uint8_t language);

// Full-screen shape library. selection_rank stores 0 for unselected items and
// 1..N for the user's playback order across 40 built-ins plus custom drawings.
void render_set_shape_picker(bool visible, uint8_t language, uint8_t page,
                             uint8_t custom_count,
                             const uint8_t *selection_rank,
                             uint8_t selection_count);
// Switches the full-screen picker between static shapes and animated forms.
// Vertical swipes are handled by the application and reflected here.
void render_set_shape_picker_category(bool animation, bool selected,
                                      uint8_t animation_item);

// Full-screen cached weather card. It is deliberately independent of the
// particle simulation so the last successful sample stays readable offline.
void render_set_weather(bool visible, uint8_t language, uint8_t state,
                        bool valid, const char *city, float temperature_c,
                        float apparent_c, uint8_t humidity, float wind_kmh,
                        uint8_t weather_code, int64_t updated_unix);
// Pauses panel DMA while the low-memory Wi-Fi HTTP transaction is active.
// AMOLED retains the last frame, and a complete redraw is forced afterwards.
void render_set_network_busy(bool busy);

// Takes a snapshot of the simulation and paints it. Blocks until the last
// band has been handed to the DMA engine.
// Returns true when at least one panel band was actually redrawn. Static
// handwriting frames are retained by the AMOLED and return false.
bool render_frame(void);

// Eight coordinated palettes: deep sea, cyber, lava, aurora, mercury, prism,
// gold and diamond.
void render_set_theme(uint8_t theme);
uint8_t render_get_theme(void);

// In reactive mode the fluid brightness follows mids and highlights follow
// treble. The background remains true AMOLED black for every theme.
void render_set_reactive(bool enabled);
void render_set_audio_levels(float bass, float mid, float treble);

// Displays a short UTF-8 operation message in the visual center of the round
// screen. The renderer owns the lifetime and clears it automatically.
void render_show_message(const char *message);

// Shows/hides the circular 1-60 minute countdown selector.
void render_set_countdown_menu(bool visible);
void render_set_countdown_selector(uint8_t minutes, bool dragging);

// Drives the same timer face after start. Progress is elapsed/total in 0..1.
void render_set_countdown_runtime(bool active, int remaining_seconds,
                                  float progress, bool paused);

#ifdef __cplusplus
}
#endif
