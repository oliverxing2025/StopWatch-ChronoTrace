#include "sim.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Uniform grid used for neighbour lookup. Each cell must be at least one
// smoothing radius across in every axis, so that a particle's neighbours can
// only ever lie in the 3x3x3 block of cells around it. These are as fine as
// SMOOTH_RADIUS 28 permits: 368/13 = 28.3, 448/16 = 28.0 and 75/2 = 37.5. Raise
// the radius and they have to come down, or particles will miss neighbours in
// the next cell but one. init() checks this at runtime.
#define GRID_CX 13
#define GRID_CY 16
#define GRID_CZ 2
#define GRID_CELLS (GRID_CX * GRID_CY * GRID_CZ)

// Cell index is z-major, so sorting particles by cell also sorts them by
// depth. The renderer walks the result backwards to draw back-to-front.
#define CELL_INDEX(cx, cy, cz) ((((cz) * GRID_CY) + (cy)) * GRID_CX + (cx))

#define WALL_MARGIN 5.0f

static const char *TAG = "sim";

// Coordinates are interleaved rather than kept in parallel arrays: the inner
// loop reads all three components of a neighbour together, so keeping them on
// one cache line matters far more than vectorisation would.
typedef struct {
    float pos[PARTICLE_MAX][3];
    float vel[PARTICLE_MAX][3];
    float old[PARTICLE_MAX][3];
} particles_t;

static particles_t s_pool[2];
static particles_t *s_p = &s_pool[0];
static particles_t *s_alt = &s_pool[1];
static int16_t s_slot_pool[2][PARTICLE_MAX];
static int16_t *s_slot = s_slot_pool[0];
static int16_t *s_slot_alt = s_slot_pool[1];

static int s_count = PARTICLE_COUNT;

static float s_density[PARTICLE_MAX];
static float s_density_near[PARTICLE_MAX];
// Viscosity offsets, accumulated during the density pass and applied after it.
// Moving a particle mid-walk would corrupt the densities still being summed.
static float s_visc_delta[PARTICLE_MAX][3];

// Pressure and near-pressure, kept side by side so reading a neighbour's pair
// touches one cache line instead of two.
static float s_press[PARTICLE_MAX][2];

static uint16_t s_cell_of[PARTICLE_MAX];
static uint16_t s_order[PARTICLE_MAX];
static uint16_t s_cell_start[GRID_CELLS + 1];

// Neighbour pairs found by the density pass, for the relaxation pass to reuse.
//
// Both passes need exactly the same pairs, and finding them is far more
// expensive than using them: the 3x3x3 block of cells around a particle holds
// about 54 candidates with j > i, of which only about 9 are actually within the
// smoothing radius. Recording the survivors the first time lets relaxation skip
// the other 45 entirely, which measured at roughly a third of the solver's total
// runtime.
//
// Stored compressed-row style: pairs for particle i are s_pair[s_pair_off[i] ..
// s_pair_off[i + 1]). The density pass visits i in ascending order, because
// particles are sorted by cell and cells are numbered in order, so the offsets
// can be filled in as it goes without a second pass.
//
// Sized for 27 pairs per particle, about twice the worst compression measured.
// If that is ever not enough the pass says so rather than quietly dropping
// pairs, and relaxation falls back to walking the grid itself.
#define PAIR_MAX 24576
static uint16_t s_pair[PAIR_MAX];
static uint32_t s_pair_off[PARTICLE_MAX + 1];
static bool s_pairs_valid;
static uint16_t s_cursor[GRID_CELLS];

static float s_rest_density;
static float s_cell_scale_x, s_cell_scale_y, s_cell_scale_z;

// Published particle state, swapped under a mutex so the render task never
// reads a half-updated frame.
static EXT_RAM_BSS_ATTR sim_particle_view_t s_view_pool[2][PARTICLE_MAX];
static sim_particle_view_t *s_view_work = s_view_pool[0];
static sim_particle_view_t *s_view_pub = s_view_pool[1];
static int s_view_pub_count;
static SemaphoreHandle_t s_view_lock;

static sim_stats_t s_stats;

static uint32_t s_rng = 0x2545F491u;

#define FORM_DURATION_US 5200000
#define FORM_CELL_X 14.5f
#define FORM_CELL_Y 15.5f
#define FORM_PARTICLE_OFFSET 3.5f
#define FORM_FLAT_ENTER 0.10f
#define FORM_FLAT_EXIT 0.16f
#define FORM_GYRO_DEADZONE 0.025f
#define FORM_GRAVITY_CORRECTION_HZ 4.0f

static EXT_RAM_BSS_ATTR float s_form_target[PARTICLE_MAX][3];
static int s_form_clock_count;
static int s_form_halo_count;
static int s_form_visible_count = PARTICLE_MAX;
static int64_t s_form_start_us;
static int64_t s_form_until_us;
static bool s_form_flat;
static bool s_form_handwriting;
static bool s_form_weather;
static bool s_form_time;
static bool s_form_analog;
static bool s_form_random_shape;
static int s_analog_second_begin;
static int s_analog_second_end;
static uint8_t s_analog_initial_second;
static int s_form_weather_digit_begin = -1;
static uint8_t s_form_handwriting_color;
static bool s_form_pose_needs_snap;
static float s_form_down_x;
static float s_form_down_y = 1.0f;
static uint8_t s_time_digits[4];
static int s_time_digit_begin[4];
static int s_time_digit_end[4];
static bool s_time_digits_valid;
static EXT_RAM_BSS_ATTR uint8_t s_digit_transition_slot[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR int16_t s_digit_slot_remap[PARTICLE_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_digit_new_slot_used[PARTICLE_MAX];
static int64_t s_digit_transition_start_us;

#define DIGIT_RELEASE_US 240000

// Five-column, seven-row clock digits. A set bit becomes a compact 2x2 group
// of real simulation particles, so the result remains visibly liquid.
static const uint8_t s_digit_rows[10][7] = {
    // Plain hollow zero. The former diagonal slash placed particle clusters
    // through the counter; at the physical particle radius those clusters
    // merged and made 0 read as a solid blob.
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // A narrow modern 1 avoids the heavy five-column foot of the old glyph.
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
};

static inline float rand_unit(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) * (1.0f / 16777215.0f);
}

// The rest density is whatever the kernel sums to for a perfect lattice at the
// target spacing. Measuring it here means REST_SPACING and SMOOTH_RADIUS can
// be changed independently without hand-retuning a magic number.
static void calibrate_rest_density(void)
{
    const float h = SMOOTH_RADIUS;
    const int reach = (int)ceilf(h / REST_SPACING);
    float rho = 0.0f;

    for (int i = -reach; i <= reach; i++) {
        for (int j = -reach; j <= reach; j++) {
            for (int k = -reach; k <= reach; k++) {
                if (i == 0 && j == 0 && k == 0) {
                    continue;
                }
                const float d = REST_SPACING * sqrtf((float)(i * i + j * j + k * k));
                if (d >= h) {
                    continue;
                }
                const float q = 1.0f - d / h;
                rho += q * q;
            }
        }
    }
    s_rest_density = rho;
}

void sim_reset(void)
{
    // Seed from the bottom of the circular face upward on a relaxed lattice.
    // The rectangular upstream seed would place most of its lowest row outside
    // a round display and collapse those particles onto one boundary point.
    const float spacing = REST_SPACING;
    int nz = (int)((BOX_D - 2.0f * WALL_MARGIN) / spacing);
    if (nz < 1) nz = 1;
    const float z0 = 0.5f * (BOX_D - (float)(nz - 1) * spacing);
    const float cx = BOX_W * 0.5f;
    const float cy = BOX_H * 0.5f;
    const float radius = BOX_CORNER_R - WALL_MARGIN - spacing * 0.5f;
    const int row_count = (int)((2.0f * radius) / spacing) + 1;

    int particle = 0;
    for (int row = 0; particle < s_count && row < row_count; row++) {
        const float y = cy + radius - (float)row * spacing;
        const float dy = y - cy;
        const float half_width = sqrtf(fmaxf(0.0f, radius * radius - dy * dy));
        const int nx = (int)((2.0f * half_width) / spacing) + 1;
        const float x0 = cx - 0.5f * (float)(nx - 1) * spacing;

        for (int ix = 0; ix < nx && particle < s_count; ix++) {
            const float x = x0 + (float)ix * spacing;
            for (int iz = 0; iz < nz && particle < s_count; iz++) {
                const int i = particle++;
                s_p->pos[i][0] = x + (rand_unit() - 0.5f) * spacing * 0.16f;
                s_p->pos[i][1] = y + (rand_unit() - 0.5f) * spacing * 0.16f;
                s_p->pos[i][2] = z0 + (float)iz * spacing +
                                 (rand_unit() - 0.5f) * spacing * 0.16f;

                for (int a = 0; a < 3; a++) {
                    s_p->vel[i][a] = 0.0f;
                    s_p->old[i][a] = s_p->pos[i][a];
                }
            }
        }
    }

    ESP_LOGI(TAG, "seeded %d particles in circular pool (%d depth layers)",
             particle, nz);
    memset(s_slot, 0xFF, sizeof(int16_t) * (size_t)s_count);
    s_form_until_us = 0;
    s_form_visible_count = s_count;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = false;
    s_form_pose_needs_snap = false;
    s_time_digits_valid = false;
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
}

int sim_set_density_mode(uint8_t mode)
{
    static const int counts[3] = {650, PARTICLE_COUNT, PARTICLE_MAX};
    if (mode > 2) mode = 1;
    s_count = counts[mode];
    sim_reset();
    return s_count;
}

void sim_touch_impulse(float x, float y)
{
    const float radius = 105.0f;
    const float strength = 1150.0f;
    for (int i = 0; i < s_count; i++) {
        float dx = s_p->pos[i][0] - x;
        float dy = s_p->pos[i][1] - y;
        const float dist2 = dx * dx + dy * dy;
        if (dist2 >= radius * radius) continue;
        float dist = sqrtf(dist2);
        if (dist < 1.0f) {
            dx = 1.0f;
            dy = 0.0f;
            dist = 1.0f;
        }
        const float kick = strength * (1.0f - dist / radius);
        s_p->vel[i][0] += dx / dist * kick;
        s_p->vel[i][1] += dy / dist * kick;
        s_p->vel[i][2] += (rand_unit() - 0.35f) * kick * 0.45f;
    }
}

static void add_time_pixel(int column, int row, int *target_count)
{
    static const float offsets[2] = {-FORM_PARTICLE_OFFSET, FORM_PARTICLE_OFFSET};
    const float cx = ((float)column - 12.0f) * FORM_CELL_X;
    const float cy = ((float)row - 3.0f) * FORM_CELL_Y;
    for (int oy = 0; oy < 2; oy++) {
        for (int ox = 0; ox < 2; ox++) {
            if (*target_count >= s_count) return;
            s_form_target[*target_count][0] = cx + offsets[ox];
            s_form_target[*target_count][1] = cy + offsets[oy];
            // Stable staggered depth gives the formed ink the same blue/cyan
            // layering as the free liquid without random per-frame twinkle.
            const int layer = ((*target_count) * 7 + (*target_count) / 3) & 3;
            s_form_target[*target_count][2] = WALL_MARGIN + 4.0f + layer * 5.5f;
            (*target_count)++;
        }
    }
}

static void add_time_dot(int column, int row, int *target_count)
{
    // Three tightly overlapping particles read as one soft circular dot,
    // unlike the old 2x2 cell which looked like a tiny square of four lights.
    static const float offsets[3][2] = {
        {-2.8f, 1.8f}, {2.8f, 1.8f}, {0.0f, -2.4f},
    };
    const float cx = ((float)column - 12.0f) * FORM_CELL_X;
    const float cy = ((float)row - 3.0f) * FORM_CELL_Y;
    for (int point = 0; point < 3; point++) {
        if (*target_count >= s_count) return;
        s_form_target[*target_count][0] = cx + offsets[point][0];
        s_form_target[*target_count][1] = cy + offsets[point][1];
        const int layer = ((*target_count) * 7 + (*target_count) / 3) & 3;
        s_form_target[*target_count][2] = WALL_MARGIN + 4.0f + layer * 5.5f;
        (*target_count)++;
    }
}

static void add_time_digit(uint8_t digit, int first_column, int *target_count)
{
    digit %= 10;
    for (int row = 0; row < 7; row++) {
        const uint8_t bits = s_digit_rows[digit][row];
        for (int column = 0; column < 5; column++) {
            if (bits & (1U << (4 - column))) {
                add_time_pixel(first_column + column, row, target_count);
            }
        }
    }
}

static void configure_time(uint8_t hours, uint8_t minutes, bool persistent)
{
    s_form_visible_count = s_count;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = true;
    s_form_analog = false;
    s_form_random_shape = false;
    const bool already_active = s_form_until_us != 0;
    const bool had_time_digits = already_active && s_time_digits_valid;
    const int previous_clock_count = s_form_clock_count;
    const uint8_t next_digits[4] = {
        (uint8_t)(hours / 10), (uint8_t)(hours % 10),
        (uint8_t)(minutes / 10), (uint8_t)(minutes % 10),
    };
    bool changed[4] = {false, false, false, false};
    int previous_begin[4];
    int previous_end[4];
    for (int digit = 0; digit < 4; digit++) {
        changed[digit] = had_time_digits &&
                         s_time_digits[digit] != next_digits[digit];
        previous_begin[digit] = s_time_digit_begin[digit];
        previous_end[digit] = s_time_digit_end[digit];
    }
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
    int target_count = 0;
    s_time_digit_begin[0] = target_count;
    add_time_digit(hours / 10, 0, &target_count);
    s_time_digit_end[0] = target_count;
    s_time_digit_begin[1] = target_count;
    add_time_digit(hours % 10, 6, &target_count);
    s_time_digit_end[1] = target_count;
    add_time_dot(12, 2, &target_count);
    add_time_dot(12, 4, &target_count);
    s_time_digit_begin[2] = target_count;
    add_time_digit(minutes / 10, 14, &target_count);
    s_time_digit_end[2] = target_count;
    s_time_digit_begin[3] = target_count;
    add_time_digit(minutes % 10, 20, &target_count);
    s_time_digit_end[3] = target_count;
    s_form_clock_count = target_count;
    s_form_halo_count = s_count - target_count;

    bool any_digit_changed = false;
    for (int digit = 0; digit < 4; digit++) {
        if (!changed[digit]) continue;
        any_digit_changed = true;
        // Transition flags use the new slot layout. Particle ownership is
        // remapped below before the release impulse is applied.
        int begin = s_time_digit_begin[digit];
        int end = s_time_digit_end[digit];
        if (begin < 0) begin = 0;
        if (end > s_count) end = s_count;
        for (int slot = begin; slot < end; slot++) {
            s_digit_transition_slot[slot] = 1;
        }
    }
    for (int digit = 0; digit < 4; digit++) s_time_digits[digit] = next_digits[digit];
    s_time_digits_valid = true;

    // Targets after the clock belong to the flat-only liquid star ring. Four
    // softly separated radii plus three depth layers prevent a dense hard rim.
    const float golden_angle = 2.39996323f;
    for (int i = target_count; i < s_count; i++) {
        const int ring_index = i - target_count;
        const float radius = 146.0f + 17.0f * (float)(ring_index & 3);
        const float angle = golden_angle * (float)ring_index;
        s_form_target[i][0] = cosf(angle) * radius;
        s_form_target[i][1] = sinf(angle) * radius;
        s_form_target[i][2] = 43.0f + 8.0f * (float)(ring_index % 3);
    }
    if (!already_active) {
        for (int i = 0; i < s_count; i++) s_slot[i] = (int16_t)i;
        s_form_pose_needs_snap = true;
    } else if (had_time_digits) {
        // Keep every particle with its logical digit instead of allowing the
        // variable glyph sizes to shift the continuous slot array. Without
        // this remap, a wide replacement glyph borrows beads from the digit
        // beside it, which looks like the two digits exchange particles.
        memset(s_digit_slot_remap, 0xFF, sizeof(s_digit_slot_remap));
        memset(s_digit_new_slot_used, 0, sizeof(s_digit_new_slot_used));

        for (int digit = 0; digit < 4; digit++) {
            const int old_count = previous_end[digit] - previous_begin[digit];
            const int new_count = s_time_digit_end[digit] - s_time_digit_begin[digit];
            const int common = old_count < new_count ? old_count : new_count;
            for (int offset = 0; offset < common; offset++) {
                const int old_slot = previous_begin[digit] + offset;
                const int new_slot = s_time_digit_begin[digit] + offset;
                s_digit_slot_remap[old_slot] = (int16_t)new_slot;
                s_digit_new_slot_used[new_slot] = 1;
            }
        }

        // The colon is a stable fifth group between the second and third
        // digits. It must also retain ownership when the left pair changes
        // width, otherwise its six particles get pulled into a numeral.
        const int previous_colon_begin = previous_end[1];
        const int previous_colon_end = previous_begin[2];
        const int new_colon_begin = s_time_digit_end[1];
        const int new_colon_end = s_time_digit_begin[2];
        const int previous_colon_count = previous_colon_end - previous_colon_begin;
        const int new_colon_count = new_colon_end - new_colon_begin;
        const int common_colon = previous_colon_count < new_colon_count ?
                                 previous_colon_count : new_colon_count;
        for (int offset = 0; offset < common_colon; offset++) {
            const int old_slot = previous_colon_begin + offset;
            const int new_slot = new_colon_begin + offset;
            s_digit_slot_remap[old_slot] = (int16_t)new_slot;
            s_digit_new_slot_used[new_slot] = 1;
        }

        // A glyph that grows recruits only from the liquid halo, never from
        // its neighbour. This is the key to independent digit reformation.
        int old_halo_cursor = previous_clock_count;
        for (int digit = 0; digit < 4; digit++) {
            const int old_count = previous_end[digit] - previous_begin[digit];
            const int new_count = s_time_digit_end[digit] - s_time_digit_begin[digit];
            for (int offset = old_count; offset < new_count; offset++) {
                while (old_halo_cursor < s_count &&
                       s_digit_slot_remap[old_halo_cursor] >= 0) {
                    old_halo_cursor++;
                }
                if (old_halo_cursor >= s_count) break;
                const int new_slot = s_time_digit_begin[digit] + offset;
                s_digit_slot_remap[old_halo_cursor++] = (int16_t)new_slot;
                s_digit_new_slot_used[new_slot] = 1;
                s_digit_transition_slot[new_slot] = 1;
            }
        }

        // Surplus beads from a shrinking glyph return to the first available
        // halo slots; remaining halo beads keep a one-to-one permutation.
        int new_slot_cursor = 0;
        for (int old_slot = 0; old_slot < s_count; old_slot++) {
            if (s_digit_slot_remap[old_slot] >= 0) continue;
            while (new_slot_cursor < s_count &&
                   s_digit_new_slot_used[new_slot_cursor]) {
                new_slot_cursor++;
            }
            if (new_slot_cursor >= s_count) break;
            s_digit_slot_remap[old_slot] = (int16_t)new_slot_cursor;
            s_digit_new_slot_used[new_slot_cursor] = 1;
            for (int digit = 0; digit < 4; digit++) {
                if (changed[digit] && old_slot >= previous_begin[digit] &&
                    old_slot < previous_end[digit]) {
                    s_digit_transition_slot[new_slot_cursor] = 1;
                    break;
                }
            }
            new_slot_cursor++;
        }

        for (int i = 0; i < s_count; i++) {
            const int old_slot = s_slot[i];
            if (old_slot >= 0 && old_slot < s_count &&
                s_digit_slot_remap[old_slot] >= 0) {
                s_slot[i] = s_digit_slot_remap[old_slot];
            }
        }
    }

    s_form_start_us = esp_timer_get_time();
    if (any_digit_changed) {
        s_digit_transition_start_us = s_form_start_us;
        for (int i = 0; i < s_count; i++) {
            const int slot = s_slot[i];
            if (slot < 0 || slot >= s_count || !s_digit_transition_slot[slot]) continue;
            // Let changed glyphs loosen and fall before their new attractors
            // take over. The unequal impulse prevents a mechanical curtain.
            const float side = (rand_unit() - 0.5f) * 42.0f;
            s_p->vel[i][0] += side;
            s_p->vel[i][1] += 48.0f + rand_unit() * 46.0f;
            s_p->vel[i][2] += (rand_unit() - 0.5f) * 34.0f;
        }
    }
    s_form_until_us = persistent ? INT64_MAX : s_form_start_us + FORM_DURATION_US;
    ESP_LOGI(TAG, "particle clock %02u:%02u using %d particles, remainder %s, %s",
             hours, minutes, target_count, s_form_flat ? "halo" : "settling",
             persistent ? "held" : "timed");
}

void sim_show_time(uint8_t hours, uint8_t minutes)
{
    configure_time(hours, minutes, false);
}

void sim_hold_time(uint8_t hours, uint8_t minutes)
{
    configure_time(hours, minutes, true);
}

static void add_analog_hand(float angle, float start_radius, float end_radius,
                            int columns, int count, int *target_count)
{
    if (count <= 0 || columns <= 0) return;
    const float dx = sinf(angle);
    const float dy = -cosf(angle);
    const float px = cosf(angle);
    const float py = sinf(angle);
    for (int i = 0; i < count && *target_count < s_count; i++) {
        const int column = i % columns;
        const int along = i / columns;
        const int along_count = (count + columns - 1) / columns;
        const float t = along_count <= 1 ? 1.0f :
                        (float)along / (float)(along_count - 1);
        const float radius = start_radius + (end_radius - start_radius) * t;
        const float side = ((float)column - (float)(columns - 1) * 0.5f) * 5.2f;
        const int slot = (*target_count)++;
        s_form_target[slot][0] = dx * radius + px * side;
        s_form_target[slot][1] = dy * radius + py * side;
        s_form_target[slot][2] = WALL_MARGIN + 5.0f + (float)(slot % 3) * 5.0f;
    }
}

static void configure_analog_time(uint8_t hours, uint8_t minutes,
                                  uint8_t seconds, bool persistent)
{
    const float tau = 6.28318530718f;
    // Keep the same bead radius as the rest of the analogue face. A lower
    // particle count makes the second hand lighter without shrinking beads.
    const int second_count = 24;
    const int minute_count = 54;
    const int hour_count = 48;
    const int centre_count = 18;
    // Eight ordinary hour marks use four beads each. The four cardinal marks
    // at 0/15/30/45 minutes use ten beads so their much longer strokes stay
    // joined and remain obvious through the particle glow on the AMOLED.
    const int tick_count = 72;
    int ring_count = s_count - second_count - minute_count - hour_count -
                     centre_count - tick_count;
    if (ring_count < 120) ring_count = 120;
    int target_count = 0;

    // A three-layer liquid bezel consumes the otherwise unused particles, so
    // the analogue view is a complete transformation rather than a clock over
    // a leftover pool.
    for (int i = 0; i < ring_count && target_count < s_count; i++) {
        const int layer = i % 3;
        const int around = i / 3;
        const int around_count = (ring_count + 2) / 3;
        const float angle = tau * (float)around / (float)around_count +
                            (float)layer * 0.010f;
        // Compensate for the renderer's depth projection. The outer layer is
        // placed close to the physical circular wall so the projected bezel
        // visually fills the AMOLED instead of occupying only its centre.
        const float radius = 208.0f + (float)layer * 8.5f;
        s_form_target[target_count][0] = sinf(angle) * radius;
        s_form_target[target_count][1] = -cosf(angle) * radius;
        s_form_target[target_count][2] = WALL_MARGIN + 6.0f + (float)layer * 6.0f;
        target_count++;
    }

    // Twelve hour marks remain legible within the particle bezel. The four
    // cardinal directions are longer for immediate quarter-hour orientation.
    for (int mark = 0; mark < 12 && target_count < s_count; mark++) {
        const float angle = tau * (float)mark / 12.0f;
        const bool cardinal = (mark % 3) == 0;
        add_analog_hand(angle, cardinal ? 154.0f : 184.0f, 204.0f,
                        2, cardinal ? 10 : 4, &target_count);
    }

    const float minute_value = (float)minutes + (float)seconds / 60.0f;
    const float hour_value = (float)(hours % 12) + minute_value / 60.0f;
    add_analog_hand(tau * hour_value / 12.0f, -12.0f, 126.0f,
                    3, hour_count, &target_count);
    add_analog_hand(tau * minute_value / 60.0f, -16.0f, 181.0f,
                    2, minute_count, &target_count);

    s_analog_second_begin = target_count;
    // Static storage keeps the straight-line sampling radii; formation_target
    // rotates the entire one-particle-wide column continuously every frame.
    for (int i = 0; i < second_count && target_count < s_count; i++) {
        const float t = second_count <= 1 ? 1.0f :
                        (float)i / (float)(second_count - 1);
        s_form_target[target_count][0] = -18.0f + t * 223.0f;
        s_form_target[target_count][1] = 0.0f;
        s_form_target[target_count][2] = WALL_MARGIN + 4.0f;
        target_count++;
    }
    s_analog_second_end = target_count;

    for (int i = 0; i < centre_count && target_count < s_count; i++) {
        const float radius = 3.0f + 10.0f * sqrtf((float)i / (float)centre_count);
        const float angle = (float)i * 2.39996323f;
        s_form_target[target_count][0] = cosf(angle) * radius;
        s_form_target[target_count][1] = sinf(angle) * radius;
        s_form_target[target_count][2] = WALL_MARGIN + 4.0f + (float)(i % 3) * 4.0f;
        target_count++;
    }

    // Defensive fill for non-standard density modes: every final slot joins
    // the outer bezel instead of remaining as free liquid.
    while (target_count < s_count) {
        const int i = target_count;
        const float angle = tau * (float)i / (float)s_count;
        s_form_target[target_count][0] = sinf(angle) * 218.0f;
        s_form_target[target_count][1] = -cosf(angle) * 218.0f;
        s_form_target[target_count][2] = WALL_MARGIN + 12.0f;
        target_count++;
    }

    for (int i = 0; i < s_count; i++) s_slot[i] = (int16_t)i;
    s_form_clock_count = s_count;
    s_form_halo_count = 0;
    s_form_visible_count = s_count;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = true;
    s_form_analog = true;
    s_form_random_shape = false;
    s_analog_initial_second = seconds % 60;
    s_time_digits_valid = false;
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
    s_form_pose_needs_snap = true;
    s_form_start_us = esp_timer_get_time();
    s_form_until_us = persistent ? INT64_MAX : s_form_start_us + FORM_DURATION_US;
    ESP_LOGI(TAG, "particle analogue clock %02u:%02u:%02u using all %d particles, %s",
             hours, minutes, seconds, s_count, persistent ? "held" : "timed");
}

void sim_show_analog_time(uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    configure_analog_time(hours, minutes, seconds, false);
}

void sim_hold_analog_time(uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    configure_analog_time(hours, minutes, seconds, true);
}

bool sim_clock_active(void)
{
    return s_form_time && sim_formation_active();
}

void sim_show_random_shape(void)
{
    const float tau = 6.28318530718f;
    const int harmonic_a = 2 + (int)(rand_unit() * 5.0f);
    int harmonic_b = 3 + (int)(rand_unit() * 6.0f);
    if (harmonic_b == harmonic_a) harmonic_b++;
    const float phase_a = rand_unit() * tau;
    const float phase_b = rand_unit() * tau;
    const float rotation = rand_unit() * tau;
    const float amplitude_a = 0.10f + rand_unit() * 0.10f;
    const float amplitude_b = 0.04f + rand_unit() * 0.07f;
    const float base_radius = 132.0f + rand_unit() * 24.0f;
    const float aspect_x = 0.92f + rand_unit() * 0.14f;
    const float curl = (rand_unit() - 0.5f) * 0.24f;
    int outline_count = (int)((float)s_count * 0.28f + 0.5f);
    if (outline_count < 168) outline_count = 168;
    if (outline_count > 240) outline_count = 240;
    if (outline_count > s_count) outline_count = s_count;
    outline_count &= ~1;
    const int around_count = outline_count / 2;

    // Two closely spaced contours form a crisp luminous stroke. Nothing is
    // placed inside the closed path, so every generated result is genuinely
    // hollow instead of a filled blob with a token centre opening.
    for (int i = 0; i < outline_count; i++) {
        const int band = i & 1;
        const int around = i >> 1;
        const float base_angle = tau * (float)around / (float)around_count;
        const float theta = rotation + base_angle +
                            curl * sinf(2.0f * base_angle + phase_b);
        float boundary = 1.0f +
            amplitude_a * sinf((float)harmonic_a * theta + phase_a) +
            amplitude_b * cosf((float)harmonic_b * theta + phase_b);
        if (boundary < 0.72f) boundary = 0.72f;
        const float radius = base_radius * boundary +
                             (band == 0 ? -4.3f : 4.3f);
        s_form_target[i][0] = cosf(theta) * radius * aspect_x;
        s_form_target[i][1] = sinf(theta) * radius;
        s_form_target[i][2] = WALL_MARGIN + 5.0f + (float)band * 5.0f;
    }
    for (int i = 0; i < s_count; i++) {
        s_slot[i] = (int16_t)i;
    }

    s_form_clock_count = outline_count;
    s_form_halo_count = 0;
    s_form_visible_count = s_count;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = true;
    s_time_digits_valid = false;
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
    s_form_pose_needs_snap = true;
    s_form_start_us = esp_timer_get_time();
    s_form_until_us = s_form_start_us + FORM_DURATION_US;
    ESP_LOGI(TAG,
             "random hollow line shape: %d outline + %d liquid, harmonics %d/%d",
             outline_count, s_count - outline_count, harmonic_a, harmonic_b);
}

bool sim_random_shape_active(void)
{
    return s_form_random_shape && sim_formation_active();
}

void sim_show_countdown(uint8_t minutes, uint8_t seconds, float remaining_ratio)
{
    if (remaining_ratio < 0.0f) remaining_ratio = 0.0f;
    if (remaining_ratio > 1.0f) remaining_ratio = 1.0f;
    const bool already_active = s_form_until_us != 0;
    const int64_t motion_start_us = s_form_start_us;
    configure_time(minutes, seconds, true);
    s_form_time = false;
    if (already_active) s_form_start_us = motion_start_us;
    const int halo_total = s_count - s_form_clock_count;
    s_form_halo_count = (int)(halo_total * remaining_ratio + 0.5f);
}

static void add_battery_bead(float x, float y, int *target_count)
{
    if (*target_count >= s_count) return;
    const int slot = *target_count;
    s_form_target[slot][0] = x;
    s_form_target[slot][1] = y;
    // Repeatable depth layers give the icon the same faceted liquid colour as
    // the clock while keeping its outline stable and free of flicker.
    s_form_target[slot][2] = WALL_MARGIN + 5.0f + (float)((slot * 5) & 3) * 5.0f;
    (*target_count)++;
}

static void add_battery_frame_bead(float x, float y, int *target_count)
{
    if (*target_count >= s_count) return;
    const int slot = *target_count;
    s_form_target[slot][0] = x;
    s_form_target[slot][1] = y;
    // Keep the complete frame on one depth plane. Varying Z is attractive in
    // the liquid fill, but makes a nominally square corner project as a soft,
    // uneven curve.
    s_form_target[slot][2] = WALL_MARGIN + 9.0f;
    (*target_count)++;
}

static void add_battery_digit_pixel(float x, float y, int *target_count)
{
    // One bead per matrix pixel preserves the black counters and corners of
    // small digits. The former 2 x 2 bead cluster merged neighbouring pixels
    // into a bright blob, especially on 3, 4, 8 and 9.
    add_battery_bead(x, y, target_count);
}

static void add_battery_digit(uint8_t digit, int first_column,
                              float start_x, float top_y, int *target_count)
{
    const float cell = 11.5f;
    digit %= 10;
    for (int row = 0; row < 7; row++) {
        const uint8_t bits = s_digit_rows[digit][row];
        for (int column = 0; column < 5; column++) {
            if (bits & (1U << (4 - column))) {
                add_battery_digit_pixel(start_x +
                                            (float)(first_column + column) * cell,
                                        top_y + (float)row * cell,
                                        target_count);
            }
        }
    }
}

void sim_show_battery(uint8_t percent)
{
    s_form_visible_count = s_count;
    if (percent > 100) percent = 100;
    int target_count = 0;
    const float left = -146.0f;
    const float top = -35.0f;
    const float x_step = 9.6f;
    const float y_step = 9.6f;
    const int body_last_column = 29;
    const int body_last_row = 9;

    // A longer, calmer 30 x 10 body gives the icon a recognisable modern
    // battery proportion. Keeping all four corner targets preserves crisp
    // right-angle corners even though the individual particles are round.
    for (int row = 0; row <= body_last_row; row++) {
        for (int column = 0; column <= body_last_column; column++) {
            if (row == 0 || row == body_last_row ||
                column == 0 || column == body_last_column) {
                add_battery_frame_bead(left + (float)column * x_step,
                                       top + (float)row * y_step,
                                       &target_count);
            }
        }
    }
    const int filled_columns = (percent * 28 + 50) / 100;
    for (int column = 1; column <= filled_columns; column++) {
        for (int row = 1; row < body_last_row; row++) {
            add_battery_bead(left + (float)column * x_step,
                             top + (float)row * y_step, &target_count);
        }
    }
    // A compact inner bead at every corner turns each pair of perpendicular
    // rails into a filled L-shaped joint. This preserves an obvious 90-degree
    // corner instead of letting four round particle caps read as a fillet.
    const float right = left + (float)body_last_column * x_step;
    const float bottom = top + (float)body_last_row * y_step;
    const float corner_inset = 6.8f;
    add_battery_frame_bead(left + corner_inset, top + corner_inset,
                           &target_count);
    add_battery_frame_bead(right - corner_inset, top + corner_inset,
                           &target_count);
    add_battery_frame_bead(left + corner_inset, bottom - corner_inset,
                           &target_count);
    add_battery_frame_bead(right - corner_inset, bottom - corner_inset,
                           &target_count);

    // The terminal is deliberately smaller than the frame and offset by half
    // a bead. The tiny visual neck makes it read as a separate metal contact
    // instead of an irregular extension of the battery body.
    const float terminal_left = right + 13.0f;
    for (int column = 0; column < 2; column++) {
        for (int row = 3; row <= 6; row++) {
            add_battery_frame_bead(terminal_left + (float)column * x_step,
                                   top + (float)row * y_step, &target_count);
        }
    }

    // Particle percentage above the icon, without a percent sign. Clamp the
    // displayed range to the requested 1..100 while the body fill still uses
    // the raw measured value.
    const uint8_t display_percent = percent == 0 ? 1 : percent;
    const int digit_count = display_percent >= 100 ? 3 :
                            display_percent >= 10 ? 2 : 1;
    const int total_columns = digit_count * 5 + (digit_count - 1);
    const float digit_cell = 11.5f;
    const float digit_start_x =
        -0.5f * (float)(total_columns - 1) * digit_cell;
    // A generous gap prevents the lowest digit particles from merging with
    // the top battery rail while the formation is still settling.
    const float digit_top_y = -145.0f;
    if (digit_count == 3) {
        add_battery_digit(1, 0, digit_start_x, digit_top_y, &target_count);
        add_battery_digit(0, 6, digit_start_x, digit_top_y, &target_count);
        add_battery_digit(0, 12, digit_start_x, digit_top_y, &target_count);
    } else if (digit_count == 2) {
        add_battery_digit(display_percent / 10, 0,
                          digit_start_x, digit_top_y, &target_count);
        add_battery_digit(display_percent % 10, 6,
                          digit_start_x, digit_top_y, &target_count);
    } else {
        add_battery_digit(display_percent, 0,
                          digit_start_x, digit_top_y, &target_count);
    }

    s_form_clock_count = target_count;
    s_form_halo_count = 0;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = false;
    s_time_digits_valid = false;
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
    for (int i = 0; i < s_count; i++) s_slot[i] = (int16_t)i;
    s_form_pose_needs_snap = true;
    s_form_start_us = esp_timer_get_time();
    s_form_until_us = s_form_start_us + FORM_DURATION_US;
    ESP_LOGI(TAG, "particle battery %u%% using %d particles",
             (unsigned)percent, target_count);
}

static void add_weather_bead(float x, float y, int *target_count)
{
    if (*target_count >= s_count) return;
    const int slot = *target_count;
    s_form_target[slot][0] = x;
    s_form_target[slot][1] = y;
    s_form_target[slot][2] = WALL_MARGIN + 5.0f + (float)((slot * 7) & 3) * 4.5f;
    (*target_count)++;
}

static void clear_weather_target_circle(float cx, float cy, float radius,
                                        int *target_count)
{
    const float radius_sq = radius * radius;
    int write = 0;
    for (int read = 0; read < *target_count; read++) {
        const float dx = s_form_target[read][0] - cx;
        const float dy = s_form_target[read][1] - cy;
        if (dx * dx + dy * dy < radius_sq) continue;
        if (write != read) {
            s_form_target[write][0] = s_form_target[read][0];
            s_form_target[write][1] = s_form_target[read][1];
            s_form_target[write][2] = s_form_target[read][2];
        }
        write++;
    }
    *target_count = write;
}

static void add_weather_line(float x0, float y0, float x1, float y1,
                             float spacing, int *target_count)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = sqrtf(dx * dx + dy * dy);
    int steps = (int)ceilf(length / spacing);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        const float t = (float)i / (float)steps;
        add_weather_bead(x0 + dx * t, y0 + dy * t, target_count);
    }
}

static void add_weather_stroke(float x0, float y0, float x1, float y1,
                               float spacing, float thickness,
                               int *target_count)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.01f) {
        add_weather_bead(x0, y0, target_count);
        return;
    }
    const float ox = -dy / length * thickness;
    const float oy = dx / length * thickness;
    add_weather_line(x0 - ox, y0 - oy, x1 - ox, y1 - oy,
                     spacing, target_count);
    add_weather_line(x0 + ox, y0 + oy, x1 + ox, y1 + oy,
                     spacing, target_count);
}

static void add_weather_circle(float cx, float cy, float radius,
                               int points, int *target_count)
{
    for (int i = 0; i < points; i++) {
        const float angle = 6.28318531f * (float)i / (float)points;
        add_weather_bead(cx + cosf(angle) * radius,
                         cy + sinf(angle) * radius, target_count);
    }
}

static void add_weather_arc(float cx, float cy, float radius,
                            float start, float end, int points,
                            int *target_count)
{
    if (points < 2) points = 2;
    for (int i = 0; i < points; i++) {
        const float t = (float)i / (float)(points - 1);
        const float angle = start + (end - start) * t;
        add_weather_bead(cx + cosf(angle) * radius,
                         cy + sinf(angle) * radius, target_count);
    }
}

static void add_particle_cloud(float cy, int *target_count)
{
    // One continuous hollow contour; weather rendering gives these beads a
    // larger radius without consuming the liquid reserve with duplicate rails.
    add_weather_arc(-48.0f, cy + 16.0f, 34.0f,
                    3.14159265f, 4.86946861f, 15, target_count);
    add_weather_arc(0.0f, cy + 6.0f, 48.0f,
                    3.61283155f, 5.71769863f, 24, target_count);
    add_weather_arc(52.0f, cy + 18.0f, 31.0f,
                    4.55530935f, 6.28318531f, 15, target_count);
    add_weather_line(-82.0f, cy + 16.0f, -78.0f, cy + 38.0f,
                     6.8f, target_count);
    add_weather_line(-78.0f, cy + 38.0f, 82.0f, cy + 38.0f,
                     6.8f, target_count);
    add_weather_line(82.0f, cy + 38.0f, 83.0f, cy + 18.0f,
                     6.8f, target_count);
}

static void add_particle_sun(float cx, float cy, int *target_count)
{
    add_weather_circle(cx, cy, 40.0f, 35, target_count);
    for (int ray = 0; ray < 8; ray++) {
        const float angle = 0.78539816f * (float)ray;
        add_weather_line(cx + cosf(angle) * 52.0f,
                         cy + sinf(angle) * 52.0f,
                         cx + cosf(angle) * 70.0f,
                         cy + sinf(angle) * 70.0f,
                         6.5f, target_count);
    }
}

static void add_weather_digit(uint8_t digit, float start_x, float top_y,
                              float cell, int *target_count)
{
    // Clean seven-segment numerals retain open counters when rendered as
    // enlarged liquid beads. The former filled 5x7 bitmap made 6/8/9 merge
    // into bright blocks on the physical AMOLED panel.
    static const uint8_t segments[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f,
    };
    const uint8_t mask = segments[digit % 10];
    const float x0 = start_x;
    const float x1 = start_x + 4.0f * cell;
    const float y0 = top_y;
    const float y1 = top_y + 3.0f * cell;
    const float y2 = top_y + 6.0f * cell;
    const float inset = 0.55f * cell;
    const float spacing = 0.92f * cell;
    if (mask & 0x01) add_weather_line(x0 + inset, y0, x1 - inset, y0,
                                      spacing, target_count);
    if (mask & 0x02) add_weather_line(x1, y0 + inset, x1, y1 - inset,
                                      spacing, target_count);
    if (mask & 0x04) add_weather_line(x1, y1 + inset, x1, y2 - inset,
                                      spacing, target_count);
    if (mask & 0x08) add_weather_line(x0 + inset, y2, x1 - inset, y2,
                                      spacing, target_count);
    if (mask & 0x10) add_weather_line(x0, y1 + inset, x0, y2 - inset,
                                      spacing, target_count);
    if (mask & 0x20) add_weather_line(x0, y0 + inset, x0, y1 - inset,
                                      spacing, target_count);
    if (mask & 0x40) add_weather_line(x0 + inset, y1, x1 - inset, y1,
                                      spacing, target_count);
}

static float weather_value_width(int value, float cell)
{
    if (value < -99) value = -99;
    if (value > 99) value = 99;
    int digits = value <= -10 || value >= 10 ? 2 : 1;
    return ((value < 0 ? 4.0f : 0.0f) + (float)digits * 6.0f) * cell;
}

static float add_weather_value(int value, float cursor, float top_y,
                               float cell, int *target_count)
{
    if (value < -99) value = -99;
    if (value > 99) value = 99;
    char digits[5];
    snprintf(digits, sizeof(digits), "%d", value);
    for (int i = 0; digits[i]; i++) {
        if (digits[i] == '-') {
            add_weather_stroke(cursor, top_y + 3.0f * cell,
                               cursor + 2.8f * cell, top_y + 3.0f * cell,
                               5.5f, 1.8f, target_count);
            cursor += 4.0f * cell;
        } else {
            add_weather_digit((uint8_t)(digits[i] - '0'), cursor, top_y,
                              cell, target_count);
            cursor += 6.0f * cell;
        }
    }
    return cursor;
}

void sim_show_weather(int16_t minimum_c, int16_t maximum_c,
                      uint8_t weather_code, bool valid)
{
    int target_count = 0;
    const bool clear = weather_code == 0;
    const bool partly_cloudy = weather_code == 1 || weather_code == 2;
    const bool fog = weather_code == 45 || weather_code == 48;
    const bool snow = weather_code >= 71 && weather_code <= 77;
    const bool thunder = weather_code >= 95;
    const bool rain = (weather_code >= 51 && weather_code <= 67) ||
                      (weather_code >= 80 && weather_code <= 94);

    if (!valid) {
        add_particle_cloud(-95.0f, &target_count);
    } else if (clear) {
        add_particle_sun(0.0f, -95.0f, &target_count);
    } else if (rain) {
        // Rain is deliberately cloud-free: five long, parallel drops remain
        // recognizable at a glance even when the liquid particles are moving.
        // Alternating vertical offsets avoid the rigid fence-like silhouette
        // produced by five perfectly aligned strokes.
        static const float rain_y_offset[5] = {-10.0f, 8.0f, -4.0f,
                                                12.0f, -7.0f};
        for (int i = -2; i <= 2; i++) {
            const float x = (float)i * 34.0f;
            const float y_offset = rain_y_offset[i + 2];
            add_weather_line(x + 10.0f, -128.0f + y_offset,
                             x - 10.0f, -72.0f + y_offset,
                             6.0f, &target_count);
        }
    } else if (snow) {
        // One large six-armed crystal leaves clear negative space between its
        // branches. Paired side twigs make it read as a snowflake rather than
        // three overlapping asterisks.
        const float cx = 0.0f;
        const float cy = -98.0f;
        const float radius = 56.0f;
        for (int spoke = 0; spoke < 3; spoke++) {
            const float angle = (float)spoke * 1.04719755f;
            const float dx = cosf(angle) * radius;
            const float dy = sinf(angle) * radius;
            add_weather_line(cx - dx, cy - dy, cx + dx, cy + dy,
                             8.5f, &target_count);
        }
        for (int arm = 0; arm < 6; arm++) {
            const float angle = (float)arm * 1.04719755f;
            const float bx = cx + cosf(angle) * 31.0f;
            const float by = cy + sinf(angle) * 31.0f;
            for (int side = -1; side <= 1; side += 2) {
                const float twig_angle = angle + (float)side * 0.72f;
                const float ex = bx + cosf(twig_angle) * 19.0f;
                const float ey = by + sinf(twig_angle) * 19.0f;
                add_weather_line(bx, by, ex, ey, 8.5f, &target_count);
            }
        }
    } else {
        if (partly_cloudy) add_particle_sun(-44.0f, -120.0f, &target_count);
        add_particle_cloud(-96.0f, &target_count);
        if (partly_cloudy) {
            // The foreground cloud contour crosses the sun on a real partly
            // cloudy icon. Punch the overlap out of the inner disc so the
            // physical beads still leave an unmistakably hollow centre.
            clear_weather_target_circle(-44.0f, -120.0f, 31.0f,
                                        &target_count);
        }
        if (thunder) {
            add_weather_line(10.0f, -55.0f, -10.0f, -22.0f,
                             6.0f, &target_count);
            add_weather_line(-10.0f, -22.0f, 13.0f, -26.0f,
                             6.0f, &target_count);
            add_weather_line(13.0f, -26.0f, -11.0f, 11.0f,
                             6.0f, &target_count);
        } else if (fog) {
            // Three broad, staggered mist bands are the standard readable fog
            // cue. Their gentle wave avoids looking like a menu or underline.
            add_weather_line(-84.0f, -47.0f, -30.0f, -51.0f,
                             6.0f, &target_count);
            add_weather_line(-30.0f, -51.0f, 24.0f, -44.0f,
                             6.0f, &target_count);
            add_weather_line(24.0f, -44.0f, 84.0f, -48.0f,
                             6.0f, &target_count);
            add_weather_line(-66.0f, -25.0f, -15.0f, -21.0f,
                             6.0f, &target_count);
            add_weather_line(-15.0f, -21.0f, 36.0f, -28.0f,
                             6.0f, &target_count);
            add_weather_line(36.0f, -28.0f, 68.0f, -24.0f,
                             6.0f, &target_count);
            add_weather_line(-80.0f, -5.0f, -28.0f, -9.0f,
                             6.0f, &target_count);
            add_weather_line(-28.0f, -9.0f, 24.0f, -2.0f,
                             6.0f, &target_count);
            add_weather_line(24.0f, -2.0f, 76.0f, -7.0f,
                             6.0f, &target_count);
        }
    }

    if (valid) {
        s_form_weather_digit_begin = target_count;
        const float cell = 8.0f;
        // Reserve real negative space around the range separator. The line is
        // still long enough to read clearly, but no bead may touch either
        // temperature value on the physical AMOLED panel.
        const float gap = 10.0f * cell;
        const float total_width = weather_value_width(minimum_c, cell) + gap +
                                  weather_value_width(maximum_c, cell);
        float cursor = -total_width * 0.5f;
        const float top_y = 55.0f;
        cursor = add_weather_value(minimum_c, cursor, top_y, cell, &target_count);
        add_weather_line(cursor + 2.0f * cell, top_y + 3.0f * cell,
                         cursor + 8.0f * cell, top_y + 3.0f * cell,
                         7.0f, &target_count);
        cursor += gap;
        add_weather_value(maximum_c, cursor, top_y, cell, &target_count);
    } else {
        s_form_weather_digit_begin = target_count;
    }

    s_form_clock_count = target_count;
    s_form_halo_count = 0;
    // Weather is an overlay formed from the same liquid, not a sparse mode:
    // every unused particle remains visible and settles naturally as before.
    s_form_visible_count = s_count;
    s_form_handwriting = false;
    s_form_weather = true;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = false;
    s_time_digits_valid = false;
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
    for (int i = 0; i < s_count; i++) s_slot[i] = (int16_t)i;
    s_form_pose_needs_snap = true;
    s_form_start_us = esp_timer_get_time();
    s_form_until_us = INT64_MAX;
    ESP_LOGI(TAG, "particle weather code=%u range=%d..%d valid=%d using %d + %d liquid",
             (unsigned)weather_code, (int)minimum_c, (int)maximum_c, valid,
             target_count, s_form_visible_count - target_count);
}

void sim_show_handwriting(const uint8_t *bitmap, int width, int height, uint8_t color)
{
    if (!bitmap || width <= 0 || height <= 0) return;
    s_form_visible_count = s_count;
    static EXT_RAM_BSS_ATTR uint8_t thinned[64 * 64];
    static EXT_RAM_BSS_ATTR uint8_t remove[64 * 64];
    static EXT_RAM_BSS_ATTR uint16_t ink[64 * 64];
    memset(thinned, 0, sizeof(thinned));
    for (int bit = 0; bit < width * height; bit++) {
        thinned[bit] = (bitmap[bit >> 3] >> (bit & 7)) & 1U;
    }

    // Zhang-Suen thinning turns the three-pixel input brush into a clean,
    // topology-preserving centreline. Loops in 8/0/中 stay open instead of
    // becoming dense particle blobs.
    for (int iteration = 0; iteration < 32; iteration++) {
        bool changed = false;
        for (int phase = 0; phase < 2; phase++) {
            memset(remove, 0, sizeof(remove));
            for (int y = 1; y < height - 1; y++) for (int x = 1; x < width - 1; x++) {
                const int p = y * width + x;
                if (!thinned[p]) continue;
                const int p2 = thinned[p - width];
                const int p3 = thinned[p - width + 1];
                const int p4 = thinned[p + 1];
                const int p5 = thinned[p + width + 1];
                const int p6 = thinned[p + width];
                const int p7 = thinned[p + width - 1];
                const int p8 = thinned[p - 1];
                const int p9 = thinned[p - width - 1];
                const int neighbours = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
                const int transitions = (!p2 && p3) + (!p3 && p4) + (!p4 && p5) +
                                        (!p5 && p6) + (!p6 && p7) + (!p7 && p8) +
                                        (!p8 && p9) + (!p9 && p2);
                if (neighbours < 2 || neighbours > 6 || transitions != 1) continue;
                const bool keep_a = phase == 0 ? (p2 && p4 && p6) : (p2 && p4 && p8);
                const bool keep_b = phase == 0 ? (p4 && p6 && p8) : (p2 && p6 && p8);
                if (!keep_a && !keep_b) remove[p] = 1;
            }
            for (int p = 0; p < width * height; p++) {
                if (remove[p]) { thinned[p] = 0; changed = true; }
            }
        }
        if (!changed) break;
    }

    int ink_count = 0, min_x = width, min_y = height, max_x = 0, max_y = 0;
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
        const int bit = y * width + x;
        if (!thinned[bit]) continue;
        if (ink_count < 64 * 64) ink[ink_count++] = (uint16_t)bit;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    if (ink_count == 0) return;

    const bool already_active = s_form_until_us != 0;
    int stroke_particles = ink_count * 2;
    if (stroke_particles < 160) stroke_particles = 160;
    if (stroke_particles > s_count / 2) stroke_particles = s_count / 2;
    const float span_x = (float)(max_x - min_x + 1);
    const float span_y = (float)(max_y - min_y + 1);
    const float scale = fminf(265.0f / span_x, 265.0f / span_y);
    int target_count = 0;
    for (int i = 0; i < stroke_particles; i++) {
        const int source = (int)(((int64_t)i * ink_count) / stroke_particles);
        const int x = ink[source] % width, y = ink[source] / width;
        // Stable pseudo-jitter: revisiting a glyph produces exactly the same
        // targets instead of a visible sparkle caused by random relocation.
        uint32_t hash = (uint32_t)(source * 2654435761U) ^ (uint32_t)(i * 2246822519U);
        hash ^= hash >> 15;
        const float jitter_x = ((float)(hash & 255U) / 255.0f - 0.5f) * 5.0f;
        const float jitter_y = ((float)((hash >> 8) & 255U) / 255.0f - 0.5f) * 5.0f;
        s_form_target[target_count][0] = ((float)x - (min_x + max_x) * 0.5f) * scale +
                                         jitter_x;
        s_form_target[target_count][1] = ((float)y - (min_y + max_y) * 0.5f) * scale +
                                         jitter_y;
        s_form_target[target_count][2] = WALL_MARGIN + 5.0f + (float)(i % 3) * 2.0f;
        target_count++;
    }
    s_form_clock_count = target_count;
    s_form_halo_count = s_count - target_count;
    const float golden_angle = 2.39996323f;
    for (int i = target_count; i < s_count; i++) {
        const int ring = i - target_count;
        const float radius = 150.0f + 14.0f * (float)(ring & 3);
        const float angle = golden_angle * (float)ring;
        s_form_target[i][0] = cosf(angle) * radius;
        s_form_target[i][1] = sinf(angle) * radius;
        s_form_target[i][2] = 42.0f + 7.0f * (float)(ring % 3);
    }
    if (!already_active) {
        for (int i = 0; i < s_count; i++) s_slot[i] = (int16_t)i;
        s_form_pose_needs_snap = true;
    }
    s_form_start_us = esp_timer_get_time();
    s_form_until_us = INT64_MAX;
    s_form_handwriting = true;
    s_form_weather = false;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = false;
    s_time_digits_valid = false;
    memset(s_digit_transition_slot, 0, sizeof(s_digit_transition_slot));
    s_form_handwriting_color = color;
    ESP_LOGI(TAG, "handwriting formation: %d centreline cells, %d particles",
             ink_count, target_count);
}

void sim_end_formation(void)
{
    s_form_visible_count = s_count;
    s_form_until_us = 0;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = false;
    s_form_pose_needs_snap = false;
}

void sim_release_formation(void)
{
    if (s_form_until_us == 0) return;
    s_form_visible_count = s_count;
    // Release instead of re-seeding: each held stroke particle receives a
    // tiny, non-uniform loosening impulse, then ordinary gravity, collisions,
    // and viscosity take over. The glyph therefore dissolves and falls back
    // into the existing pool rather than disappearing in a reset cut.
    for (int i = 0; i < s_count; i++) {
        const int slot = s_slot[i];
        if (slot < 0 || slot >= s_form_clock_count) continue;
        const float dx = s_p->pos[i][0] - BOX_W * 0.5f;
        const float dy = s_p->pos[i][1] - BOX_H * 0.5f;
        const float length = sqrtf(dx * dx + dy * dy);
        const float nx = length > 1.0f ? dx / length : 0.0f;
        const float ny = length > 1.0f ? dy / length : 0.0f;
        const float loosen = 18.0f + rand_unit() * 34.0f;
        s_p->vel[i][0] += nx * loosen + (rand_unit() - 0.5f) * 32.0f;
        s_p->vel[i][1] += ny * loosen + (rand_unit() - 0.35f) * 28.0f;
        s_p->vel[i][2] += (rand_unit() - 0.5f) * 24.0f;
    }
    s_form_until_us = 0;
    s_form_handwriting = false;
    s_form_weather = false;
    s_form_time = false;
    s_form_analog = false;
    s_form_random_shape = false;
    s_form_pose_needs_snap = false;
    ESP_LOGI(TAG, "formation released into natural fluid motion");
}

bool sim_formation_active(void)
{
    const int64_t until = s_form_until_us;
    return until != 0 && esp_timer_get_time() < until;
}

void sim_center_burst(void)
{
    sim_touch_impulse(BOX_W * 0.5f, BOX_H * 0.5f);
}

void sim_directional_gust(float x, float y)
{
    const float magnitude = sqrtf(x * x + y * y);
    if (magnitude < 0.01f) return;
    x /= magnitude;
    y /= magnitude;
    for (int i = 0; i < s_count; i++) {
        const float spread = (rand_unit() - 0.5f) * 180.0f;
        s_p->vel[i][0] += x * 720.0f - y * spread;
        s_p->vel[i][1] += y * 720.0f + x * spread;
        s_p->vel[i][2] += (rand_unit() - 0.5f) * 220.0f;
    }
}

void sim_audio_pulse(float strength)
{
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    const float kick = 270.0f + strength * 430.0f;
    for (int i = 0; i < s_count; i++) {
        s_p->vel[i][0] += (rand_unit() - 0.5f) * kick * 0.28f;
        s_p->vel[i][1] -= kick * (0.55f + rand_unit() * 0.35f);
        s_p->vel[i][2] += (rand_unit() - 0.5f) * kick * 0.22f;
    }
}

void sim_init(void)
{
    s_view_lock = xSemaphoreCreateMutex();

    s_cell_scale_x = (float)GRID_CX / BOX_W;
    s_cell_scale_y = (float)GRID_CY / BOX_H;
    s_cell_scale_z = (float)GRID_CZ / BOX_D;

    // Neighbour search is only correct while each cell spans at least one
    // smoothing radius.
    const float cw = BOX_W / GRID_CX;
    const float ch = BOX_H / GRID_CY;
    const float cd = BOX_D / GRID_CZ;
    if (cw < SMOOTH_RADIUS || ch < SMOOTH_RADIUS || cd < SMOOTH_RADIUS) {
        ESP_LOGE(TAG, "grid cell %.1fx%.1fx%.1f smaller than radius %.1f",
                 (double)cw, (double)ch, (double)cd, (double)SMOOTH_RADIUS);
    }

    calibrate_rest_density();
    ESP_LOGI(TAG, "rest density %.3f (spacing %.1f, radius %.1f)",
             (double)s_rest_density, (double)REST_SPACING, (double)SMOOTH_RADIUS);

    sim_reset();
}

// Counting sort of every particle into its grid cell. Besides building the
// lookup structure this also reorders the particle arrays, so neighbours end
// up adjacent in memory, which matters a lot on a CPU with a small cache.
static void rebuild_grid(void)
{
    memset(s_cell_start, 0, sizeof(s_cell_start));

    for (int i = 0; i < s_count; i++) {
        int cx = (int)(s_p->pos[i][0] * s_cell_scale_x);
        int cy = (int)(s_p->pos[i][1] * s_cell_scale_y);
        int cz = (int)(s_p->pos[i][2] * s_cell_scale_z);
        cx = cx < 0 ? 0 : (cx >= GRID_CX ? GRID_CX - 1 : cx);
        cy = cy < 0 ? 0 : (cy >= GRID_CY ? GRID_CY - 1 : cy);
        cz = cz < 0 ? 0 : (cz >= GRID_CZ ? GRID_CZ - 1 : cz);

        const uint16_t cell = (uint16_t)CELL_INDEX(cx, cy, cz);
        s_cell_of[i] = cell;
        s_cell_start[cell + 1]++;
    }

    for (int c = 0; c < GRID_CELLS; c++) {
        s_cell_start[c + 1] = (uint16_t)(s_cell_start[c + 1] + s_cell_start[c]);
        s_cursor[c] = s_cell_start[c];
    }

    for (int i = 0; i < s_count; i++) {
        s_order[s_cursor[s_cell_of[i]]++] = (uint16_t)i;
    }

    for (int k = 0; k < s_count; k++) {
        const uint16_t src = s_order[k];
        for (int a = 0; a < 3; a++) {
            s_alt->pos[k][a] = s_p->pos[src][a];
            s_alt->vel[k][a] = s_p->vel[src][a];
            s_alt->old[k][a] = s_p->old[src][a];
        }
        s_slot_alt[k] = s_slot[src];
    }

    particles_t *swap = s_p;
    s_p = s_alt;
    s_alt = swap;
    int16_t *slot_swap = s_slot;
    s_slot = s_slot_alt;
    s_slot_alt = slot_swap;
}

// The contiguous runs of sorted particle indices that make up the 3x3x3
// neighbourhood of a cell. Runs are contiguous because cells differing only in
// x are adjacent in the cell numbering. Every particle in a cell shares the
// same neighbourhood, so this is computed once per cell rather than per
// particle.
typedef struct {
    int start[9];
    int end[9];
    int runs;
} neighbourhood_t;

static void neighbourhood_for_cell(int cx, int cy, int cz, neighbourhood_t *nb)
{
    const int x0 = cx > 0 ? cx - 1 : 0;
    const int x1 = cx < GRID_CX - 1 ? cx + 1 : GRID_CX - 1;

    nb->runs = 0;
    for (int dz = -1; dz <= 1; dz++) {
        const int nz = cz + dz;
        if (nz < 0 || nz >= GRID_CZ) {
            continue;
        }
        for (int dy = -1; dy <= 1; dy++) {
            const int ny = cy + dy;
            if (ny < 0 || ny >= GRID_CY) {
                continue;
            }
            const int begin = s_cell_start[CELL_INDEX(x0, ny, nz)];
            const int stop = s_cell_start[CELL_INDEX(x1, ny, nz) + 1];
            if (stop > begin) {
                nb->start[nb->runs] = begin;
                nb->end[nb->runs] = stop;
                nb->runs++;
            }
        }
    }
}

// The three solver passes all walk the grid the same way. Cells are numbered
// so that incrementing by one steps along x, which keeps the loop free of any
// integer division.
#define FOR_EACH_CELL(cx, cy, cz, cell)                     \
    for (int cz = 0, cell = 0; cz < GRID_CZ; cz++)          \
        for (int cy = 0; cy < GRID_CY; cy++)                \
            for (int cx = 0; cx < GRID_CX; cx++, cell++)

// Densities and Clavet viscosity in a single pass over the grid.
//
// Walking the neighbour grid is the most expensive thing the solver does, and
// these two need exactly the same pairs and the same square root, so they
// share one walk. Viscosity normally acts on velocity before positions are
// integrated; here positions have already moved, so the impulse is applied as
// a position offset of the same size (impulse * dt). Velocity is recovered
// from total displacement at the end of the substep, so the result is
// identical. Offsets go to a separate buffer, otherwise moving a particle
// mid-walk would corrupt the densities still being summed.
static void compute_densities_and_viscosity(float dt)
{
    const float h2 = SMOOTH_RADIUS * SMOOTH_RADIUS;
    const float inv_h = 1.0f / SMOOTH_RADIUS;

    memset(s_density, 0, sizeof(float) * (size_t)s_count);
    memset(s_density_near, 0, sizeof(float) * (size_t)s_count);
    memset(s_visc_delta, 0, sizeof(float) * 3 * (size_t)s_count);

    uint32_t pairs = 0;
    s_pairs_valid = true;

    FOR_EACH_CELL(cx, cy, cz, cell) {
        const int i0 = s_cell_start[cell];
        const int i1 = s_cell_start[cell + 1];
        if (i0 == i1) {
            continue;
        }

        neighbourhood_t nb;
        neighbourhood_for_cell(cx, cy, cz, &nb);

        for (int i = i0; i < i1; i++) {
            const float xi = s_p->pos[i][0], yi = s_p->pos[i][1], zi = s_p->pos[i][2];
            const float vxi = s_p->vel[i][0], vyi = s_p->vel[i][1], vzi = s_p->vel[i][2];

            s_pair_off[i] = pairs;

            float rho = s_density[i];
            float rho_near = s_density_near[i];
            float dvx = 0.0f, dvy = 0.0f, dvz = 0.0f;

            for (int run = 0; run < nb.runs; run++) {
                int j = nb.start[run];
                if (j <= i) {
                    j = i + 1;  // every pair is visited exactly once
                }
                for (; j < nb.end[run]; j++) {
                    const float dx = s_p->pos[j][0] - xi;
                    const float dy = s_p->pos[j][1] - yi;
                    const float dz = s_p->pos[j][2] - zi;
                    const float r2 = dx * dx + dy * dy + dz * dz;
                    if (r2 >= h2 || r2 < 1e-6f) {
                        continue;
                    }

                    if (pairs < PAIR_MAX) {
                        s_pair[pairs++] = (uint16_t)j;
                    } else {
                        s_pairs_valid = false;
                    }

                    const float r = sqrtf(r2);
                    const float inv_r = 1.0f / r;
                    const float q = 1.0f - r * inv_h;
                    const float q2 = q * q;

                    rho += q2;
                    rho_near += q2 * q;
                    s_density[j] += q2;
                    s_density_near[j] += q2 * q;

                    const float ux = dx * inv_r, uy = dy * inv_r, uz = dz * inv_r;
                    const float u = (vxi - s_p->vel[j][0]) * ux +
                                    (vyi - s_p->vel[j][1]) * uy +
                                    (vzi - s_p->vel[j][2]) * uz;
                    if (u <= 0.0f) {
                        continue;  // only damp approach, never pull apart
                    }

                    // Half the impulse goes to each particle. Never remove
                    // more than half the closing speed, or strong viscosity
                    // would push the pair back apart and oscillate.
                    float dv = 0.5f * dt * q * (VISC_SIGMA * u + VISC_BETA * u * u);
                    if (dv > 0.5f * u) {
                        dv = 0.5f * u;
                    }
                    const float imp = dv * dt;

                    dvx -= imp * ux;
                    dvy -= imp * uy;
                    dvz -= imp * uz;
                    s_visc_delta[j][0] += imp * ux;
                    s_visc_delta[j][1] += imp * uy;
                    s_visc_delta[j][2] += imp * uz;
                }
            }

            s_density[i] = rho;
            s_density_near[i] = rho_near;
            s_visc_delta[i][0] += dvx;
            s_visc_delta[i][1] += dvy;
            s_visc_delta[i][2] += dvz;
        }
    }

    s_pair_off[s_count] = pairs;
    s_stats.pairs = (int)pairs;

    for (int i = 0; i < s_count; i++) {
        for (int a = 0; a < 3; a++) {
            s_p->pos[i][a] += s_visc_delta[i][a];
        }
    }
}

// One relaxation pair. Both loop shells below share this so the physics exists
// once; only the way j is arrived at differs.
//
// The r2 test stays even on the cached path. Pushes are applied immediately, so
// a pair recorded by the density pass may already have been driven apart past
// the smoothing radius by the time it is reached, and it has to drop out exactly
// as it would have when rediscovered.
static inline void relax_pair(int i, int j, float xi, float yi, float zi,
                              float p_i, float pn_i, float dt2, float h2,
                              float inv_h, float *mx, float *my, float *mz,
                              int *clamped)
{
    const float dx = s_p->pos[j][0] - xi;
    const float dy = s_p->pos[j][1] - yi;
    const float dz = s_p->pos[j][2] - zi;
    const float r2 = dx * dx + dy * dy + dz * dz;
    if (r2 >= h2 || r2 < 1e-6f) {
        return;
    }

    const float r = sqrtf(r2);
    const float inv_r = 1.0f / r;
    const float q = 1.0f - r * inv_h;
    const float q2 = q * q;

    const float p_j = s_press[j][0];
    const float pn_j = s_press[j][1];

    // Each particle pushes according to its own pressure; summing both halves
    // keeps the pair symmetric, so momentum is conserved and the pair is
    // visited once.
    float d = 0.5f * dt2 * ((p_i + p_j) * q + (pn_i + pn_j) * q2);
    if (d > MAX_DISPLACEMENT) {
        d = MAX_DISPLACEMENT;
        (*clamped)++;
    } else if (d < -MAX_DISPLACEMENT) {
        d = -MAX_DISPLACEMENT;
        (*clamped)++;
    }

    const float sx = dx * inv_r * d;
    const float sy = dy * inv_r * d;
    const float sz = dz * inv_r * d;

    s_p->pos[j][0] += sx;
    s_p->pos[j][1] += sy;
    s_p->pos[j][2] += sz;
    *mx -= sx;
    *my -= sy;
    *mz -= sz;
}

// Double density relaxation. Ordinary pressure is signed, so a particle in a
// too-sparse region is pulled back towards its neighbours, which gives the
// fluid a cohesive surface. The near pressure is always repulsive and is what
// stops particles from piling up into clumps.
//
// Pushes are applied straight away rather than gathered up and applied at the
// end, so later pairs already see the corrected positions. That converges faster
// in dense regions, at the cost of the rare corner blow-up noted in the README.
static void relax_positions(float dt)
{
    const float h2 = SMOOTH_RADIUS * SMOOTH_RADIUS;
    const float inv_h = 1.0f / SMOOTH_RADIUS;
    const float dt2 = dt * dt;

    int clamped = 0;

    // Turn densities into pressures once, rather than for every pair.
    for (int i = 0; i < s_count; i++) {
        s_press[i][0] = K_PRESSURE * (s_density[i] - s_rest_density);
        s_press[i][1] = K_NEAR_PRESSURE * s_density_near[i];
    }

    if (s_pairs_valid) {
        for (int i = 0; i < s_count; i++) {
            const float xi = s_p->pos[i][0], yi = s_p->pos[i][1], zi = s_p->pos[i][2];
            const float p_i = s_press[i][0];
            const float pn_i = s_press[i][1];

            float mx = 0.0f, my = 0.0f, mz = 0.0f;  // accumulated move for i

            const uint32_t k1 = s_pair_off[i + 1];
            for (uint32_t k = s_pair_off[i]; k < k1; k++) {
                relax_pair(i, s_pair[k], xi, yi, zi, p_i, pn_i, dt2, h2, inv_h,
                           &mx, &my, &mz, &clamped);
            }

            s_p->pos[i][0] += mx;
            s_p->pos[i][1] += my;
            s_p->pos[i][2] += mz;
        }
    } else {
        // The pair list overflowed, so find the neighbours the hard way.
        FOR_EACH_CELL(cx, cy, cz, cell) {
            const int i0 = s_cell_start[cell];
            const int i1 = s_cell_start[cell + 1];
            if (i0 == i1) {
                continue;
            }

            neighbourhood_t nb;
            neighbourhood_for_cell(cx, cy, cz, &nb);

            for (int i = i0; i < i1; i++) {
                const float xi = s_p->pos[i][0], yi = s_p->pos[i][1],
                            zi = s_p->pos[i][2];
                const float p_i = s_press[i][0];
                const float pn_i = s_press[i][1];

                float mx = 0.0f, my = 0.0f, mz = 0.0f;

                for (int run = 0; run < nb.runs; run++) {
                    int j = nb.start[run];
                    if (j <= i) {
                        j = i + 1;
                    }
                    for (; j < nb.end[run]; j++) {
                        relax_pair(i, j, xi, yi, zi, p_i, pn_i, dt2, h2, inv_h,
                                   &mx, &my, &mz, &clamped);
                    }
                }

                s_p->pos[i][0] += mx;
                s_p->pos[i][1] += my;
                s_p->pos[i][2] += mz;
            }
        }
    }

    s_stats.clamped = clamped;
}

// The interior of the case: a rounded rectangle in x and y, extruded through
// the depth of the box, and curved into the glass at the front and the back
// panel at the back. Every wall, edge and corner is one continuous surface.
//
// It resolves as the same trick applied twice. Clamp a point to the inner
// rectangle joining the four corner-arc centres, and whatever offset remains
// points straight out from the nearest part of the outline; its length is how
// far out the particle is, which collapses x and y into a single radial
// coordinate. That leaves a 2D problem in (radial, depth), which is again a
// rectangle with rounded corners, so the same clamp-and-project applies.
//
// Every case falls out of it with no branching. Against a flat wall one
// component of the offset is zero; in a vertical corner the first stage gives
// the arc; in the fillet the second stage does; where a corner meets the back
// panel both do at once, which is the doubly-curved patch.
static void resolve_walls(void)
{
    s_stats.front_hits = 0;
    s_stats.back_hits = 0;
    s_stats.front_push = 0.0f;
    s_stats.back_push = 0.0f;

    const float lo_z = WALL_MARGIN;
    const float hi_z = BOX_D - WALL_MARGIN;

    // Centres of the four corner arcs.
    const float arc_lo_x = BOX_CORNER_R;
    const float arc_hi_x = BOX_W - BOX_CORNER_R;
    const float arc_lo_y = BOX_CORNER_R;
    const float arc_hi_y = BOX_H - BOX_CORNER_R;

    // Insetting a rounded rectangle by the wall margin shrinks its radius by
    // the same amount, leaving the arc centres where they are.
    const float side_r = BOX_CORNER_R - WALL_MARGIN;

    const float front_f = BOX_FRONT_FILLET;
    const float back_f = BOX_BACK_FILLET;

    for (int i = 0; i < s_count; i++) {
        const float x = s_p->pos[i][0];
        const float y = s_p->pos[i][1];
        const float z = s_p->pos[i][2];

        const float ax = x < arc_lo_x ? arc_lo_x : (x > arc_hi_x ? arc_hi_x : x);
        const float ay = y < arc_lo_y ? arc_lo_y : (y > arc_hi_y ? arc_hi_y : y);
        float ux = x - ax;
        float uy = y - ay;
        float r = sqrtf(ux * ux + uy * uy);
        if (r > 1e-6f) {
            ux /= r;
            uy /= r;
        } else {
            ux = 0.0f;
            uy = 0.0f;
        }

        // Whichever end of the box we are near supplies the fillet. Between
        // them the wall is straight, which is the same maths at zero radius.
        float fillet, cz;
        if (z < lo_z + front_f) {
            fillet = front_f;
            cz = lo_z + front_f;
        } else if (z > hi_z - back_f) {
            fillet = back_f;
            cz = hi_z - back_f;
        } else {
            fillet = 0.0f;
            cz = z;
        }

        const float lim = side_r - fillet;
        const float cr = r < lim ? r : lim;
        const float dr = r - cr;
        const float dz = z - cz;
        const float dd2 = dr * dr + dz * dz;

        if (dd2 <= fillet * fillet) {
            continue;
        }

        const float dd = sqrtf(dd2);
        const float nr = dr / dd;
        const float nz = dz / dd;

        // Land a random fraction of a pixel inside the surface rather than
        // exactly on it, so a corner does not stack particles onto one point.
        // Offsetting along the normal is the same as shrinking the fillet radius
        // by that amount, which also covers the straight walls, where the radius
        // is zero.
        const float inset = fillet - WALL_JITTER * rand_unit();

        s_p->pos[i][0] = ax + ux * (cr + nr * inset);
        s_p->pos[i][1] = ay + uy * (cr + nr * inset);
        s_p->pos[i][2] = cz + nz * inset;

        if (z < lo_z + front_f) {
            s_stats.front_hits++;
            s_stats.front_push += dd - fillet;
        } else if (z > hi_z - back_f) {
            s_stats.back_hits++;
            s_stats.back_push += dd - fillet;
        }

        // The surface normal in three dimensions: the in-plane direction scaled
        // by how much of the offset was radial, plus the depth part.
        const float nx = ux * nr;
        const float ny = uy * nr;

        const float vn = s_p->vel[i][0] * nx + s_p->vel[i][1] * ny + s_p->vel[i][2] * nz;
        if (vn > 0.0f) {
            const float k = (1.0f + WALL_RESTITUTION) * vn;
            s_p->vel[i][0] -= k * nx;
            s_p->vel[i][1] -= k * ny;
            s_p->vel[i][2] -= k * nz;
        }

        // Damp the whole velocity, then put the normal component back, so drag
        // acts only along the surface and never fights the restitution above.
        const float keep =
            s_p->vel[i][0] * nx + s_p->vel[i][1] * ny + s_p->vel[i][2] * nz;
        const float restore = keep * (1.0f - WALL_FRICTION);
        s_p->vel[i][0] = s_p->vel[i][0] * WALL_FRICTION + restore * nx;
        s_p->vel[i][1] = s_p->vel[i][1] * WALL_FRICTION + restore * ny;
        s_p->vel[i][2] = s_p->vel[i][2] * WALL_FRICTION + restore * nz;
    }
}

// Gravity plus the pseudo-forces that appear because the box itself is an
// accelerating, rotating reference frame: centrifugal, Euler (angular
// acceleration) and Coriolis (motion inside a spinning frame).
static void integrate_velocities(float dt, const sim_forces_t *f)
{
    const float gx = f->gravity[0], gy = f->gravity[1], gz = f->gravity[2];
    const float wx = f->omega[0], wy = f->omega[1], wz = f->omega[2];
    const float ax = f->alpha[0], ay = f->alpha[1], az = f->alpha[2];

    const float w2 = wx * wx + wy * wy + wz * wz;
    const bool rotating = (ROTATION_GAIN != 0.0f) &&
                          (w2 > 1e-8f || (ax * ax + ay * ay + az * az) > 1e-8f);

    const float cx = BOX_W * 0.5f, cy = BOX_H * 0.5f, cz = BOX_D * 0.5f;

    for (int i = 0; i < s_count; i++) {
        float accx = gx, accy = gy, accz = gz;

        if (rotating) {
            const float rx = s_p->pos[i][0] - cx;
            const float ry = s_p->pos[i][1] - cy;
            const float rz = s_p->pos[i][2] - cz;

            // Centrifugal: |w|^2 * r_perpendicular, i.e. r*|w|^2 - w*(w.r)
            const float wdotr = wx * rx + wy * ry + wz * rz;
            accx += ROTATION_GAIN * (rx * w2 - wx * wdotr);
            accy += ROTATION_GAIN * (ry * w2 - wy * wdotr);
            accz += ROTATION_GAIN * (rz * w2 - wz * wdotr);

            // Euler: -alpha x r
            accx -= ROTATION_GAIN * (ay * rz - az * ry);
            accy -= ROTATION_GAIN * (az * rx - ax * rz);
            accz -= ROTATION_GAIN * (ax * ry - ay * rx);

            // Coriolis: -2 w x v
            const float vx = s_p->vel[i][0], vy = s_p->vel[i][1], vz = s_p->vel[i][2];
            accx -= ROTATION_GAIN * 2.0f * (wy * vz - wz * vy);
            accy -= ROTATION_GAIN * 2.0f * (wz * vx - wx * vz);
            accz -= ROTATION_GAIN * 2.0f * (wx * vy - wy * vx);
        }

        s_p->vel[i][0] += accx * dt;
        s_p->vel[i][1] += accy * dt;
        s_p->vel[i][2] += accz * dt;
    }
}

static void update_formation_pose(float dt, const sim_forces_t *forces)
{
    // IMU output is already mapped into display coordinates by imu_read():
    // x points right and y points down. Do not rotate it a second time here.
    const float gx = forces->down[0];
    const float gy = forces->down[1];
    const float gz = forces->down[2];
    const float plane = sqrtf(gx * gx + gy * gy);
    const float total = sqrtf(plane * plane + gz * gz);
    if (total < 0.5f) return;

    const float plane_ratio = plane / total;
    bool snapped = false;
    if (s_form_pose_needs_snap) {
        // A new clock uses the best viewer-relative reference available. Tilt
        // gives absolute up/down from gravity. When level, retain the heading
        // continuously tracked by the gyro because acceleration alone cannot
        // reveal rotation around the screen normal.
        const char *pose_source = "gyro-tracked";
        if (plane_ratio > 0.12f && plane > 0.01f) {
            s_form_down_x = gx / plane;
            s_form_down_y = gy / plane;
            pose_source = "gravity";
        }
        s_form_pose_needs_snap = false;
        snapped = true;
        ESP_LOGI(TAG, "clock pose snapped: %s, display down %.2f %.2f",
                 pose_source,
                 (double)s_form_down_x, (double)s_form_down_y);
    }

    // Predict screen-up orientation from rotation about the display normal.
    // Gravity cannot reveal this angle while the watch lies flat, so the gyro
    // keeps HH:MM upright when the user turns a level device on the table.
    if (!snapped) {
        float wz = forces->omega[2];
        if (fabsf(wz) < FORM_GYRO_DEADZONE) wz = 0.0f;
        // Rotate the clock opposite to the enclosure so it stays fixed in the
        // viewer's frame. The StopWatch BMI270 Z sign is enclosure rotation.
        const float angle = -wz * dt;
        if (angle != 0.0f) {
            const float cs = cosf(angle);
            const float sn = sinf(angle);
            const float old_x = s_form_down_x;
            const float old_y = s_form_down_y;
            s_form_down_x = old_x * cs + old_y * sn;
            s_form_down_y = old_y * cs - old_x * sn;
        }
    }
    if (s_form_flat) {
        if (plane_ratio > FORM_FLAT_EXIT) s_form_flat = false;
    } else if (plane_ratio < FORM_FLAT_ENTER) {
        s_form_flat = true;
    }

    // When gravity has a trustworthy in-plane component, blend it back in as
    // an absolute reference. This removes gyro drift without making the clock
    // jump when crossing portrait, landscape and upside-down holds.
    if (plane_ratio > 0.12f && plane > 0.01f) {
        const float measured_x = gx / plane;
        const float measured_y = gy / plane;
        const float confidence = fminf(1.0f, (plane_ratio - 0.12f) / 0.28f);
        const float blend = confidence *
            (1.0f - expf(-6.28318530718f * FORM_GRAVITY_CORRECTION_HZ * dt));
        s_form_down_x += (measured_x - s_form_down_x) * blend;
        s_form_down_y += (measured_y - s_form_down_y) * blend;
    }

    const float orientation_length =
        sqrtf(s_form_down_x * s_form_down_x + s_form_down_y * s_form_down_y);
    if (orientation_length > 0.01f) {
        s_form_down_x /= orientation_length;
        s_form_down_y /= orientation_length;
    } else {
        s_form_down_x = 0.0f;
        s_form_down_y = 1.0f;
    }

}

static bool formation_target(int slot, int64_t now, float *x, float *y, float *z)
{
    if (slot < 0 || slot >= s_count || now >= s_form_until_us) return false;
    if (!s_form_handwriting && s_digit_transition_slot[slot] &&
        now - s_digit_transition_start_us < DIGIT_RELEASE_US) {
        return false;
    }
    if (slot >= s_form_clock_count &&
        slot >= s_form_clock_count + s_form_halo_count) return false;
    if (slot >= s_form_clock_count && !s_form_flat) return false;

    const float elapsed = (float)(now - s_form_start_us) * 1e-6f;
    float lx = s_form_target[slot][0];
    float ly = s_form_target[slot][1];

    if (s_form_analog && slot >= s_analog_second_begin &&
        slot < s_analog_second_end) {
        // One physical bead column rotates as a continuous sweep second hand.
        // The stored X coordinate is its signed distance from the centre.
        const float second = fmodf((float)s_analog_initial_second + elapsed, 60.0f);
        const float angle = second * 0.10471975512f;
        const float radius = s_form_target[slot][0];
        lx = sinf(angle) * radius;
        ly = -cosf(angle) * radius;
    }

    if (slot < s_form_clock_count) {
        if (!s_form_handwriting && !s_form_analog) {
            const bool weather_digit = s_form_weather &&
                                       slot >= s_form_weather_digit_begin;
            const float breathe_amount = weather_digit ? 0.0f :
                                         (s_form_weather ? 0.004f : 0.012f);
            const float vertical_wave = weather_digit ? 0.0f :
                                        (s_form_weather ? 0.6f : 2.5f);
            const float bead_x = weather_digit ? 0.0f :
                                 (s_form_weather ? 0.28f : 1.35f);
            const float bead_y = weather_digit ? 0.0f :
                                 (s_form_weather ? 0.18f : 0.85f);
            const float breathe = 1.0f + breathe_amount * sinf(elapsed * 2.6f);
            lx *= breathe;
            ly = ly * breathe - vertical_wave * sinf(elapsed * 2.1f);
            // A coherent sub-pixel current moves through the individual beads.
            // Different fixed phases create liquid motion without random jitter
            // or enough displacement to soften the numeral counters.
            const float bead_phase = elapsed * 1.75f + (float)slot * 0.31f;
            lx += bead_x * sinf(bead_phase);
            ly += bead_y * cosf(bead_phase * 0.83f);
        }
    } else {
        // On a level surface the halo drifts slowly while remaining circular.
        const float angle = elapsed * 0.11f;
        const float cs = cosf(angle), sn = sinf(angle);
        const float rx = lx * cs - ly * sn;
        ly = lx * sn + ly * cs;
        lx = rx;
    }

    // Local Y is screen-down and follows measured gravity; local X is its
    // perpendicular. This keeps HH:MM upright through portrait, landscape and
    // upside-down holds. A circular halo is unaffected by this rotation.
    const float right_x = s_form_down_y;
    const float right_y = -s_form_down_x;
    *x = BOX_W * 0.5f + lx * right_x + ly * s_form_down_x;
    *y = BOX_H * 0.5f + lx * right_y + ly * s_form_down_y;
    float target_z = s_form_target[slot][2];
    if (slot < s_form_clock_count && !s_form_handwriting && !s_form_analog) {
        const bool weather_digit = s_form_weather &&
                                   slot >= s_form_weather_digit_begin;
        const float depth_wave = weather_digit ? 0.0f :
                                 (s_form_weather ? 0.7f : 2.8f);
        target_z += depth_wave * sinf(elapsed * 1.55f + (float)slot * 0.37f);
    }
    *z = target_z;
    return true;
}

static void apply_formation(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_form_until_us == 0) return;
    if (now >= s_form_until_us) {
        s_form_until_us = 0;
        ESP_LOGI(TAG, "particle clock released back to fluid");
        return;
    }

    for (int i = 0; i < s_count; i++) {
        float tx, ty, tz;
        const int slot = s_slot[i];
        if (!formation_target(slot, now, &tx, &ty, &tz)) continue;

        const float dx = tx - s_p->pos[i][0];
        const float dy = ty - s_p->pos[i][1];
        const float dz = tz - s_p->pos[i][2];
        const bool clock_particle = slot < s_form_clock_count;
        const bool weather_digit = clock_particle && s_form_weather &&
                                   slot >= s_form_weather_digit_begin;
        const float pull = weather_digit ? 0.23f :
                           (clock_particle ? (s_form_handwriting ? 0.20f : 0.16f) : 0.065f);
        s_p->pos[i][0] += dx * pull;
        s_p->pos[i][1] += dy * pull;
        s_p->pos[i][2] += dz * pull;

        const float velocity_keep = weather_digit ? 0.05f :
                                    (clock_particle ? (s_form_handwriting ? 0.05f : 0.16f) : 0.30f);
        const float target_velocity = weather_digit ? 0.36f :
                                      (s_form_handwriting ? 0.10f : 0.30f);
        s_p->vel[i][0] = s_p->vel[i][0] * velocity_keep + dx * target_velocity;
        s_p->vel[i][1] = s_p->vel[i][1] * velocity_keep + dy * target_velocity;
        s_p->vel[i][2] = s_p->vel[i][2] * velocity_keep + dz * target_velocity;
    }
}

static void substep(float dt, const sim_forces_t *f)
{
    integrate_velocities(dt, f);

    const float inv_dt = 1.0f / dt;

    for (int i = 0; i < s_count; i++) {
        for (int a = 0; a < 3; a++) {
            s_p->old[i][a] = s_p->pos[i][a];
            s_p->pos[i][a] += s_p->vel[i][a] * dt;
        }
    }

    int64_t t0 = esp_timer_get_time();
    rebuild_grid();
    s_stats.us_grid = (int)(esp_timer_get_time() - t0);

    t0 = esp_timer_get_time();
    compute_densities_and_viscosity(dt);
    s_stats.us_density = (int)(esp_timer_get_time() - t0);

    t0 = esp_timer_get_time();
    relax_positions(dt);
    s_stats.us_relax = (int)(esp_timer_get_time() - t0);

    // Recover velocity from the actual motion, so the relaxation displacement
    // shows up as real momentum.
    for (int i = 0; i < s_count; i++) {
        for (int a = 0; a < 3; a++) {
            s_p->vel[i][a] = (s_p->pos[i][a] - s_p->old[i][a]) * inv_dt;
        }
    }

    resolve_walls();
    apply_formation();
}

static void publish(void)
{
    float sum_rho = 0.0f, sum_speed = 0.0f, max_speed = 0.0f;
    const float palette_time = (float)esp_timer_get_time() * 1e-6f;

    // Split the same sums over the two fillet bands, to compare how the fluid
    // behaves against the glass with how it behaves against the back panel.
    const float front_edge = WALL_MARGIN + BOX_FRONT_FILLET;
    const float back_edge = BOX_D - WALL_MARGIN - BOX_BACK_FILLET;
    float f_rho = 0.0f, f_speed = 0.0f, b_rho = 0.0f, b_speed = 0.0f;
    int f_n = 0, b_n = 0;

    for (int i = 0; i < s_count; i++) {
        const float vx = s_p->vel[i][0], vy = s_p->vel[i][1], vz = s_p->vel[i][2];
        const float speed = sqrtf(vx * vx + vy * vy + vz * vz);

        s_view_work[i].x = s_p->pos[i][0];
        s_view_work[i].y = s_p->pos[i][1];
        s_view_work[i].z = s_p->pos[i][2];
        // A held handwritten stroke should look like stable luminous ink.
        // Physics speed still feeds statistics/audio, but not its colour,
        // avoiding per-frame highlight flicker while particles settle.
        const bool stable_ink = s_form_handwriting && s_form_until_us != 0 &&
                                s_slot[i] >= 0 && s_slot[i] < s_form_clock_count;
        const bool hidden_weather_remainder = s_form_until_us != 0 &&
                                               s_slot[i] >= s_form_visible_count;
        const bool formed_digit = !s_form_handwriting && s_form_until_us != 0 &&
                                  s_slot[i] >= 0 && s_slot[i] < s_form_clock_count;
        float display_speed = speed;
        if (formed_digit || stable_ink) {
            // Each stable slot receives a repeatable point along the theme's
            // speed palette. Handwriting uses the same coherent travelling
            // crest as time digits, then render blends it with the glyph's
            // selected base colour. No random value changes between frames.
            uint32_t hash = (uint32_t)(s_slot[i] + 1) * 2654435761u;
            hash ^= hash >> 16;
            const float shade = (float)(hash & 0xffu) * (1.0f / 255.0f);
            // A slow palette crest travels left-to-right across the complete
            // time string. The stable slot tint prevents flat colour bands;
            // the sine phase guarantees continuous motion with no flicker.
            const int slot = s_slot[i];
            const float phase = palette_time * 2.15f +
                                s_form_target[slot][0] * 0.025f +
                                s_form_target[slot][1] * 0.010f;
            const float flow = 0.5f + 0.5f * sinf(phase);
            display_speed = 160.0f + shade * 520.0f + flow * 2150.0f +
                            fminf(speed, 1600.0f) * 0.10f;
        }
        s_view_work[i].speed = display_speed;
        s_view_work[i].color = stable_ink ? s_form_handwriting_color : 255;
        uint8_t formed_style = 0;
        if (formed_digit) {
            if (!s_form_weather) {
                formed_style = 1;
            } else if (s_slot[i] >= s_form_weather_digit_begin) {
                formed_style = 4;
            } else {
                formed_style = 3;
            }
        }
        s_view_work[i].formed_digit = hidden_weather_remainder ? 2 : formed_style;

        sum_rho += s_density[i];
        sum_speed += speed;
        if (speed > max_speed) {
            max_speed = speed;
        }

        const float z = s_p->pos[i][2];
        if (z < front_edge) {
            f_rho += s_density[i];
            f_speed += speed;
            f_n++;
        } else if (z > back_edge) {
            b_rho += s_density[i];
            b_speed += speed;
            b_n++;
        }
    }

    const float inv_n = 1.0f / (float)s_count;
    s_stats.mean_density = sum_rho * inv_n;
    s_stats.rest_density = s_rest_density;
    s_stats.mean_speed = sum_speed * inv_n;
    s_stats.max_speed = max_speed;

    s_stats.front_count = f_n;
    s_stats.back_count = b_n;
    s_stats.front_density = f_n ? f_rho / (float)f_n : 0.0f;
    s_stats.back_density = b_n ? b_rho / (float)b_n : 0.0f;
    s_stats.front_speed = f_n ? f_speed / (float)f_n : 0.0f;
    s_stats.back_speed = b_n ? b_speed / (float)b_n : 0.0f;
    if (s_stats.front_hits) {
        s_stats.front_push /= (float)s_stats.front_hits;
    }
    if (s_stats.back_hits) {
        s_stats.back_push /= (float)s_stats.back_hits;
    }

    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    sim_particle_view_t *swap = s_view_pub;
    s_view_pub = s_view_work;
    s_view_work = swap;
    s_view_pub_count = s_count;
    xSemaphoreGive(s_view_lock);
}

void sim_step(float dt_real, const sim_forces_t *forces)
{
    // Everything runs in slowed-down time, including the angular rates, so
    // gravity, shake and rotation stay in proportion to each other.
    //
    // Capped, because the solver's stability depends on dt but its runtime does
    // not depend on dt at all: a slow step must not also be a stiff one. See
    // SIM_DT_MAX.
    float dt = (dt_real * TIME_SCALE) / (float)SUBSTEPS;
    if (dt > SIM_DT_MAX) {
        dt = SIM_DT_MAX;
    }

    sim_forces_t scaled = *forces;
    // UI orientation follows real time, not the slowed liquid-simulation time.
    update_formation_pose(dt_real, forces);
    for (int a = 0; a < 3; a++) {
        scaled.omega[a] *= TIME_SCALE;
        scaled.alpha[a] *= TIME_SCALE * TIME_SCALE;
    }

    for (int s = 0; s < SUBSTEPS; s++) {
        substep(dt, &scaled);
    }
    publish();
}

int sim_snapshot(sim_particle_view_t *out, int max)
{
    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    int n = s_view_pub_count;
    if (n > max) {
        n = max;
    }
    if (n > 0) {
        memcpy(out, s_view_pub, (size_t)n * sizeof(sim_particle_view_t));
    }
    xSemaphoreGive(s_view_lock);
    return n;
}

void sim_stats(sim_stats_t *out)
{
    *out = s_stats;
}
