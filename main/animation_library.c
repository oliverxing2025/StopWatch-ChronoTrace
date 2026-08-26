#include "animation_library.h"

#include <math.h>
#include <string.h>

#define ARROW_UNITS_PER_CELL 18.5f
#define ARROW_CANVAS_SCALE 7.0f
#define ARROW_CANVAS_HALF (31.5f / ARROW_UNITS_PER_CELL)
#define ARROW_REAR (-1.10f)
#define ARROW_TIP 0.70f

static const char *const kNamesCn[ANIMATION_LIBRARY_COUNT] = {
    "上箭头", "下箭头", "左箭头", "右箭头",
    "呼吸心跳", "DNA 双螺旋", "烟花", "粒子雨",
};

static const char *const kNamesEn[ANIMATION_LIBRARY_COUNT] = {
    "Arrow Up", "Arrow Down", "Arrow Left", "Arrow Right",
    "Heartbeat", "DNA Helix", "Firework", "Particle Rain",
};

static void raw_pixel(uint8_t *bitmap, int x, int y)
{
    if (x < 0 || x >= HANDWRITING_W || y < 0 || y >= HANDWRITING_H) return;
    const int bit = y * HANDWRITING_W + x;
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

static void raw_disc(uint8_t *bitmap, int cx, int cy, int radius)
{
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius + 1) {
                raw_pixel(bitmap, x, y);
            }
        }
    }
}

static void raw_line(uint8_t *bitmap, float x0, float y0,
                     float x1, float y1, int radius)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const int steps = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)) * 1.5f);
    for (int i = 0; i <= steps; i++) {
        const float t = steps > 0 ? (float)i / (float)steps : 0.0f;
        raw_disc(bitmap, (int)lroundf(x0 + dx * t),
                 (int)lroundf(y0 + dy * t), radius);
    }
}

static void draw_heartbeat(uint8_t *bitmap, float phase)
{
    const float cycle = phase - floorf(phase);
    // One complete inhale/exhale per cycle. The 29% size range is deliberate:
    // the whole heart is visibly small, then visibly large, instead of beads
    // chasing one another around an almost static contour.
    const float breath = 0.5f - 0.5f * cosf(cycle * 6.28318530718f);
    const float eased = breath * breath * (3.0f - 2.0f * breath);
    const float scale = 0.93f + eased * 0.27f;
    float previous_x = 0.0f;
    float previous_y = 0.0f;
    for (int i = 0; i <= 180; i++) {
        const float a = (float)i * 6.28318530718f / 180.0f;
        const float sa = sinf(a);
        const float x = 31.5f + scale * 0.92f * 16.0f * sa * sa * sa;
        const float y = 31.0f - scale * 0.88f *
            (13.0f * cosf(a) - 5.0f * cosf(2.0f * a) -
             2.0f * cosf(3.0f * a) - cosf(4.0f * a));
        if (i > 0) raw_line(bitmap, previous_x, previous_y, x, y, 1);
        previous_x = x;
        previous_y = y;
    }
}

static void draw_dna_helix(uint8_t *bitmap, float phase)
{
    const float motion = phase * 6.28318530718f;
    float prev_ax = 31.5f, prev_bx = 31.5f;
    float prev_y = 7.0f;
    for (int y = 7; y <= 56; y++) {
        // Two clean strands rotate through one another. Fixed rung heights
        // preserve the familiar minimal DNA ladder while the strand endpoints
        // glide horizontally through them.
        const float wave = (float)(y - 7) * 0.205f + motion;
        const float ax = 31.5f + sinf(wave) * 12.0f;
        const float bx = 31.5f - sinf(wave) * 12.0f;
        if (y > 7) {
            raw_line(bitmap, prev_ax, prev_y, ax, (float)y, 0);
            raw_line(bitmap, prev_bx, prev_y, bx, (float)y, 0);
        }
        if ((y - 7) % 7 == 0) {
            raw_line(bitmap, ax, (float)y, bx, (float)y, 0);
            raw_disc(bitmap, (int)lroundf(ax), y, 1);
            raw_disc(bitmap, (int)lroundf(bx), y, 1);
        }
        prev_ax = ax;
        prev_bx = bx;
        prev_y = (float)y;
    }
}

static void draw_firework(uint8_t *bitmap, float phase)
{
    const float cycle = fminf(fmaxf(phase, 0.0f), 1.0f);
    if (cycle < 0.38f) {
        // A compact five-bead fireball rises from the pool. Only three sparse
        // beads trail behind it; there is no full-height launch line.
        const float t = cycle / 0.38f;
        const float eased = t * t * (3.0f - 2.0f * t);
        const int head_y = (int)lroundf(56.0f - eased * 37.0f);
        raw_pixel(bitmap, 32, head_y);
        raw_pixel(bitmap, 31, head_y);
        raw_pixel(bitmap, 33, head_y);
        raw_pixel(bitmap, 32, head_y - 1);
        raw_pixel(bitmap, 32, head_y + 1);
        raw_pixel(bitmap, 32, head_y + 3);
        raw_pixel(bitmap, 32, head_y + 6);
        if (head_y + 9 < HANDWRITING_H) {
            raw_pixel(bitmap, 32, head_y + 9);
        }
        return;
    }

    const float burst = (cycle - 0.38f) / 0.62f;
    const float radius = 2.0f + 27.0f *
        (1.0f - (1.0f - burst) * (1.0f - burst));
    const float gravity_drop = burst * burst * 14.0f;
    const int rays = 18;
    for (int ray = 0; ray < rays; ray++) {
        const float angle = (float)ray * 6.28318530718f / (float)rays +
                            (float)((ray * 7) % 5 - 2) * 0.025f;
        const float ray_scale = 0.78f + (float)((ray * 11) % 7) * 0.045f;
        const int trail_count = burst < 0.72f ? 3 : 2;
        for (int trail = 0; trail < trail_count; trail++) {
            const float distance = fmaxf(0.0f, radius * ray_scale -
                                                (float)trail * 3.2f);
            const float x = 31.5f + cosf(angle) * distance;
            const float y = 19.0f + sinf(angle) * distance + gravity_drop;
            raw_disc(bitmap, (int)lroundf(x), (int)lroundf(y),
                     trail == 0 && (ray % 6) == 0 ? 1 : 0);
        }
    }
}

static float wrap_unit(float value)
{
    value -= floorf(value);
    return value < 0.0f ? value + 1.0f : value;
}

static void draw_particle_rain(uint8_t *bitmap, float phase)
{
    // Each stream has its own fixed phase, speed, length and spacing. Keeping
    // these values deterministic avoids allocating or mutating per-frame
    // objects while still producing the asynchronous Matrix-like offset.
    static const uint8_t x_positions[] = {
        5, 10, 15, 21, 27, 33, 38, 44, 49, 54, 59,
    };
    static const uint8_t speed_steps[] = {
        59, 91, 68, 113, 77, 96, 63, 106, 72, 86, 121,
    };
    static const uint8_t phase_steps[] = {
        7, 71, 139, 31, 203, 111, 239, 167, 53, 191, 97,
    };
    static const uint8_t lengths[] = {
        4, 7, 5, 8, 4, 6, 9, 5, 7, 4, 6,
    };
    static const uint8_t spacings[] = {
        3, 4, 3, 5, 4, 3, 4, 5, 3, 4, 3,
    };
    const int stream_count = (int)(sizeof(x_positions) /
                                   sizeof(x_positions[0]));
    for (int column = 0; column < stream_count; column++) {
        const float speed = (float)speed_steps[column] / 72.0f;
        const float offset = (float)phase_steps[column] / 255.0f;
        const float head_cycle = wrap_unit(offset + phase * speed);
        const float travel = 76.0f;
        const bool downward = (column & 1) == 0;
        // Adjacent columns run in opposite vertical directions. Their phases
        // and speeds remain independent, so the field never looks like one
        // rigid dotted picture sliding as a whole.
        const float head_y = downward ?
            (-6.0f + head_cycle * travel) :
            (69.0f - head_cycle * travel);
        const int spacing = spacings[column];
        const int length = lengths[column];
        for (int trail = 0; trail < length; trail++) {
            float y = head_y + (downward ? -1.0f : 1.0f) *
                               (float)trail * (float)spacing;
            // Wrap inside the visible canvas so every stream always owns the
            // same number of beads. A changing cell count would otherwise let
            // the global formation allocator transfer a bead between columns.
            while (y < 1.0f) y += 62.0f;
            while (y > 62.0f) y -= 62.0f;
            raw_pixel(bitmap, x_positions[column], (int)lroundf(y));
        }
    }
}

const char *animation_library_name(uint8_t index, bool english)
{
    if (index >= ANIMATION_LIBRARY_COUNT) return english ? "Animation" : "动画";
    return english ? kNamesEn[index] : kNamesCn[index];
}

bool animation_library_is_continuous(uint8_t index)
{
    return (index == 4 || index == 5 || index == 7);
}

void animation_library_screen_offset(uint8_t index, float phase,
                                     float *x, float *y)
{
    if (index >= ANIMATION_ARROW_COUNT) {
        if (x) *x = 0.0f;
        if (y) *y = 0.0f;
        return;
    }
    const float travel_begin = -ARROW_CANVAS_HALF - ARROW_TIP - 0.04f;
    const float travel_end = ARROW_CANVAS_HALF - ARROW_REAR + 0.04f;
    const float clamped = fminf(fmaxf(phase, 0.0f), 1.0f);
    const float distance = (travel_begin +
                            (travel_end - travel_begin) * clamped) *
                           ARROW_UNITS_PER_CELL * ARROW_CANVAS_SCALE;
    float dx = distance;
    float dy = 0.0f;
    if (index == 0) {
        dx = 0.0f;
        dy = -distance;
    } else if (index == 1) {
        dx = 0.0f;
        dy = distance;
    } else if (index == 2) {
        dx = -distance;
        dy = 0.0f;
    }
    if (x) *x = dx;
    if (y) *y = dy;
}

bool animation_library_bitmap(uint8_t index, float phase,
                              uint8_t bitmap[HANDWRITING_BYTES])
{
    if (!bitmap || index >= ANIMATION_LIBRARY_COUNT) return false;
    memset(bitmap, 0, HANDWRITING_BYTES);

    if (index == 4) {
        draw_heartbeat(bitmap, phase);
        return true;
    }
    if (index == 5) {
        draw_dna_helix(bitmap, phase);
        return true;
    }
    if (index == 6) {
        draw_firework(bitmap, phase);
        return true;
    }
    if (index == 7) {
        draw_particle_rain(bitmap, phase);
        return true;
    }

    // Draw one canonical right-pointing arrow, then rotate its integer bitmap
    // coordinates for the other three directions. This guarantees that the
    // vertical shafts are exact 90-degree copies of the horizontal shaft,
    // rather than separately rasterized approximations with different edges.
    // The fixed animation canvas now spans almost the full 466-pixel display
    // diameter. Keep the arrow's physical size unchanged by using fewer
    // logical units per bitmap cell, while extending its centre far enough
    // that the complete tail starts behind one edge and the complete tip
    // finishes behind the opposite edge.
    const float units_per_cell = ARROW_UNITS_PER_CELL;
    const float canvas_half = ARROW_CANVAS_HALF;
    const float rear = ARROW_REAR;
    const float shoulder = 0.02f;
    // All four directions share one exact silhouette. Direction changes only
    // through the integer rotations below, so head and shaft proportions can
    // never drift between horizontal and vertical arrows.
    const float tip = ARROW_TIP;
    const float travel_begin = -canvas_half - tip - 0.04f;
    const float travel_end = canvas_half - rear + 0.04f;
    const float travel = travel_begin + (travel_end - travel_begin) *
                         fminf(fmaxf(phase, 0.0f), 1.0f);
    const float shaft_half = 0.085f;
    // A broader, slightly longer triangular head remains unmistakable after
    // the silhouette is represented by round liquid beads. The same canonical
    // geometry is still rotated for all four directions.
    const float head_half = 0.50f;
    for (int y = 0; y < HANDWRITING_H; y++) {
        const float ny = ((float)y - 31.5f) / units_per_cell;
        for (int x = 0; x < HANDWRITING_W; x++) {
            const float nx = ((float)x - 31.5f) / units_per_cell;
            const float along = nx - travel;
            const float across = ny;
            const bool in_shaft = along >= rear && along <= shoulder &&
                                  fabsf(across) <= shaft_half;
            const float head_limit = head_half *
                (tip - along) / (tip - shoulder);
            const bool in_head = along >= shoulder && along <= tip &&
                                 fabsf(across) <= head_limit;
            if (!in_shaft && !in_head) continue;
            int out_x = x;
            int out_y = y;
            if (index == 0) {          // right -> up (90 degrees CCW)
                out_x = y;
                out_y = HANDWRITING_W - 1 - x;
            } else if (index == 1) {   // right -> down (90 degrees CW)
                out_x = HANDWRITING_H - 1 - y;
                out_y = x;
            } else if (index == 2) {   // right -> left (180 degrees)
                out_x = HANDWRITING_W - 1 - x;
                out_y = HANDWRITING_H - 1 - y;
            }
            raw_pixel(bitmap, out_x, out_y);
        }
    }
    return true;
}
