#pragma once

#include <stdbool.h>
#include <stdint.h>

// Per-particle state handed to the renderer. Positions are in simulation
// space: x and y in screen pixels, z in pixels of depth into the case.
typedef struct {
    float x, y, z;
    float speed;  // px/s, used to pick a colour
    uint8_t color;  // 0..7 for handwritten ink, 255 for theme colour
    uint8_t formed_digit;  // 1=fine ink, 2=hidden, 3=bold weather, 4=weather digits
} sim_particle_view_t;

// External accelerations for one step, expressed in simulation axes.
typedef struct {
    float gravity[3];  // px/s^2, gravity plus the shake pseudo-force
    float down[3];     // stable unit gravity direction, without shake
    float omega[3];    // rad/s, box angular velocity
    float alpha[3];    // rad/s^2, box angular acceleration
} sim_forces_t;

// Diagnostics, logged periodically so the solver can be tuned from the serial
// console without watching the screen. The timings are microseconds spent in
// each pass of the most recent substep.
typedef struct {
    float mean_density;
    float rest_density;
    float mean_speed;
    float max_speed;
    int us_grid;
    int us_density;  // includes viscosity; the two share one pass
    int us_relax;

    // Front and back fillet bands compared against each other. The fluid
    // settles against the back of the case but not against the glass, and these
    // are here to say why: whether the front is denser, moving faster, being
    // pushed further by the wall, or saturating the per-pair displacement cap.
    float front_density, back_density;
    float front_speed, back_speed;
    float front_push, back_push;  // mean wall projection distance, px
    int front_count, back_count;
    int front_hits, back_hits;    // particles the wall had to move
    int clamped;                  // pairs that saturated MAX_DISPLACEMENT
    int pairs;                    // neighbour pairs cached for relaxation
} sim_stats_t;

void sim_init(void);

// Re-seeds the fluid as a settled block at the bottom of the box.
void sim_reset(void);

// 0=light (650), 1=standard (900), 2=dense (1000); resets the pool.
int sim_set_density_mode(uint8_t mode);

// Injects a short radial velocity pulse at a screen-space touch point.
void sim_touch_impulse(float x, float y);

// Pulls a subset of the existing liquid particles into an orientation-aware
// HH:MM formation for a few seconds. Gravity corrects portrait/landscape pose
// and the gyro preserves upright text during flat in-plane rotation. When
// flat, the remainder forms a halo; under tilt it settles under real gravity.
void sim_show_time(uint8_t hours, uint8_t minutes);

// Holds HH:MM until sim_reset() is called. Repeated calls update the minute
// targets without releasing the particles or changing their stable slot IDs.
void sim_hold_time(uint8_t hours, uint8_t minutes);
void sim_show_analog_time(uint8_t hours, uint8_t minutes, uint8_t seconds);
void sim_hold_analog_time(uint8_t hours, uint8_t minutes, uint8_t seconds);
bool sim_clock_active(void);
void sim_show_random_shape(void);
bool sim_random_shape_active(void);

// Holds MM:SS while gradually releasing the unused halo. remaining_ratio is
// clamped to 0..1 and updated by the application once per second.
void sim_show_countdown(uint8_t minutes, uint8_t seconds, float remaining_ratio);

// Forms a hollow battery outline, fills its interior from left to right and
// shows a centered 1..100 particle level above it. While charging, a body-wide
// bead rail below it repeatedly assembles, scatters and reforms.
void sim_show_battery(uint8_t percent, bool charging);

// Holds a minimal particle weather view: a WMO-derived icon above today's
// low-to-high range. When valid is false, only a neutral cloud is formed.
void sim_show_weather(int16_t minimum_c, int16_t maximum_c,
                      uint8_t weather_code, bool valid);

// Holds a 64x64 one-bit handwritten glyph. The actual stroke shape is used,
// so Chinese, Latin letters, numbers, and simple symbols need no OCR/font.
void sim_show_handwriting(const uint8_t *bitmap, int width, int height, uint8_t color);
// Updates the targets of an already visible glyph without announcing a new
// formation. Used by smooth particle animations whose topology evolves frame
// by frame while keeping the same held formation.
void sim_update_handwriting(const uint8_t *bitmap, int width, int height,
                            uint8_t color);
// Animation bitmaps use the complete 64x64 canvas as a stable screen-space
// coordinate system, so a moving outline travels instead of being re-centred
// and re-scaled independently on every frame.
void sim_show_animation_bitmap(const uint8_t *bitmap, int width, int height,
                               uint8_t color, uint16_t fixed_particle_count,
                               bool preserve_particle_cohort);
void sim_update_animation_bitmap(const uint8_t *bitmap, int width, int height,
                                 uint8_t color, bool allow_recruitment,
                                 bool preserve_local_mapping,
                                 bool preserve_stream_ownership,
                                 uint16_t fixed_particle_count,
                                 float motion_x, float motion_y);
void sim_show_owned_animation_bitmap(const uint8_t *bitmap,
                                     const uint8_t *owners,
                                     int width, int height, uint8_t color);
void sim_update_owned_animation_bitmap(const uint8_t *bitmap,
                                       const uint8_t *owners,
                                       int width, int height, uint8_t color);
void sim_end_formation(void);
void sim_release_formation(void);
bool sim_formation_active(void);

// Touch-only visual gestures, deliberately separate from button functions.
void sim_center_burst(void);
void sim_directional_gust(float x, float y);

// Music-reactive gestures use real particles from the current liquid surface.
void sim_audio_pulse(float strength);
void sim_audio_wave(float strength, bool reverse);
void sim_audio_side_pulse(float strength, bool right_side);
void sim_audio_sparks(float strength);

// Advances the fluid by dt_real seconds of wall-clock time.
void sim_step(float dt_real, const sim_forces_t *forces);

// Copies the most recently published particle state. Returns the count.
int sim_snapshot(sim_particle_view_t *out, int max);

void sim_stats(sim_stats_t *out);
