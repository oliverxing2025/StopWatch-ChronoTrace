#include "shape_library.h"

#include <math.h>
#include <string.h>

#include "esp_random.h"

#define TAU 6.28318530718f

typedef struct { float x, y; } point_t;

static const char *const kNamesCn[SHAPE_LIBRARY_COUNT] = {
    "爱心", "星形", "五角星", "圆形", "方形", "三角形", "菱形", "六边形",
    "八边形", "月亮", "无限", "十字", "波浪", "五瓣花", "六瓣花", "八瓣花",
    "蝴蝶", "四叶草", "叶子", "水滴", "闪电", "云朵", "太阳", "彩虹",
    "小鱼", "飞鸟", "猫咪", "兔子", "笑脸", "音符", "皇冠", "火箭",
    "星球", "雪花", "雨伞", "房子", "大树", "群山", "苹果", "沙漏",
};

static const char *const kNamesEn[SHAPE_LIBRARY_COUNT] = {
    "Heart", "Star", "Pentagram", "Circle", "Square", "Triangle", "Diamond", "Hexagon",
    "Octagon", "Moon", "Infinity", "Cross", "Wave", "Flower 5", "Flower 6", "Flower 8",
    "Butterfly", "Clover", "Leaf", "Drop", "Lightning", "Cloud", "Sun", "Rainbow",
    "Fish", "Bird", "Cat", "Rabbit", "Smile", "Music", "Crown", "Rocket",
    "Planet", "Snowflake", "Umbrella", "House", "Tree", "Mountains", "Apple", "Hourglass",
};

static void pixel(uint8_t *bitmap, int x, int y)
{
    if (x < 1 || x >= HANDWRITING_W - 1 || y < 1 || y >= HANDWRITING_H - 1) return;
    for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
        const int px = x + ox, py = y + oy;
        const int bit = py * HANDWRITING_W + px;
        bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
    }
}

static int sx(float x) { return (int)lroundf(32.0f + x * 26.0f); }
static int sy(float y) { return (int)lroundf(32.0f + y * 26.0f); }

static void line(uint8_t *bitmap, float x0, float y0, float x1, float y1)
{
    const int steps = 96;
    for (int i = 0; i <= steps; i++) {
        const float t = (float)i / (float)steps;
        pixel(bitmap, sx(x0 + (x1 - x0) * t), sy(y0 + (y1 - y0) * t));
    }
}

static void cubic(uint8_t *bitmap, point_t p0, point_t p1,
                  point_t p2, point_t p3)
{
    for (int i = 0; i <= 180; i++) {
        const float t = (float)i / 180.0f;
        const float u = 1.0f - t;
        const float x = u*u*u*p0.x + 3.0f*u*u*t*p1.x +
                        3.0f*u*t*t*p2.x + t*t*t*p3.x;
        const float y = u*u*u*p0.y + 3.0f*u*u*t*p1.y +
                        3.0f*u*t*t*p2.y + t*t*t*p3.y;
        pixel(bitmap, sx(x), sy(y));
    }
}

static void poly(uint8_t *bitmap, const point_t *points, int count, bool closed)
{
    for (int i = 0; i < count - 1; i++) {
        line(bitmap, points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
    }
    if (closed && count > 2) {
        line(bitmap, points[count - 1].x, points[count - 1].y,
             points[0].x, points[0].y);
    }
}

static void ellipse(uint8_t *bitmap, float cx, float cy, float rx, float ry,
                    float begin, float end)
{
    for (int i = 0; i <= 180; i++) {
        const float a = begin + (end - begin) * (float)i / 180.0f;
        pixel(bitmap, sx(cx + cosf(a) * rx), sy(cy + sinf(a) * ry));
    }
}

static void regular(uint8_t *bitmap, int sides, float rotation)
{
    point_t points[12];
    for (int i = 0; i < sides; i++) {
        const float a = rotation + TAU * (float)i / (float)sides;
        points[i] = (point_t){cosf(a) * 0.88f, sinf(a) * 0.88f};
    }
    poly(bitmap, points, sides, true);
}

static void star(uint8_t *bitmap, int points, float inner, bool spokes)
{
    point_t path[24];
    for (int i = 0; i < points * 2; i++) {
        const float radius = (i & 1) ? inner : 0.92f;
        const float a = -1.57079632679f + 3.14159265359f * (float)i / (float)points;
        path[i] = (point_t){cosf(a) * radius, sinf(a) * radius};
    }
    poly(bitmap, path, points * 2, true);
    if (spokes && points == 5) {
        point_t outer[5];
        for (int i = 0; i < 5; i++) {
            const float a = -1.57079632679f + TAU * (float)i / 5.0f;
            outer[i] = (point_t){cosf(a) * 0.92f, sinf(a) * 0.92f};
        }
        const int order[6] = {0, 2, 4, 1, 3, 0};
        for (int i = 0; i < 5; i++) line(bitmap, outer[order[i]].x, outer[order[i]].y,
                                        outer[order[i + 1]].x, outer[order[i + 1]].y);
    }
}

static void flower(uint8_t *bitmap, int petals)
{
    for (int i = 0; i <= 360; i++) {
        const float a = TAU * (float)i / 360.0f;
        const float r = 0.48f + 0.34f * cosf((float)petals * a);
        pixel(bitmap, sx(cosf(a) * r), sy(sinf(a) * r));
    }
    ellipse(bitmap, 0, 0, 0.16f, 0.16f, 0, TAU);
}

const char *shape_library_name(uint8_t index, bool english)
{
    if (index >= SHAPE_LIBRARY_COUNT) return english ? "Custom" : "自绘";
    return english ? kNamesEn[index] : kNamesCn[index];
}

uint8_t shape_library_color(uint8_t index)
{
    // Built-in shapes deliberately have no permanent colour. Each appearance
    // (picker preview, direct switch, random switch, or playlist step) draws a
    // fresh colour from the full bright handwriting palette. Custom drawings
    // keep the colour chosen by their creator.
    (void)index;
    return (uint8_t)(esp_random() % HANDWRITING_COLOR_COUNT);
}

bool shape_library_bitmap(uint8_t index, uint8_t bitmap[HANDWRITING_BYTES])
{
    if (!bitmap || index >= SHAPE_LIBRARY_COUNT) return false;
    memset(bitmap, 0, HANDWRITING_BYTES);
    switch (index) {
        case 0: // Heart.
            for (int i = 0; i <= 240; i++) {
                const float t = TAU * (float)i / 240.0f;
                const float sine = sinf(t);
                const float x = 0.054f * 16.0f * sine * sine * sine;
                const float y = -0.052f * (13.0f * cosf(t) - 5.0f * cosf(2*t) -
                                          2.0f * cosf(3*t) - cosf(4*t));
                pixel(bitmap, sx(x), sy(y + 0.05f));
            }
            break;
        case 1: star(bitmap, 5, 0.44f, false); break;
        case 2: star(bitmap, 5, 0.44f, true); break;
        case 3: ellipse(bitmap, 0, 0, 0.88f, 0.88f, 0, TAU); break;
        case 4: regular(bitmap, 4, 0.78539816f); break;
        case 5: regular(bitmap, 3, -1.57079633f); break;
        case 6: regular(bitmap, 4, 0); break;
        case 7: regular(bitmap, 6, 0); break;
        case 8: regular(bitmap, 8, 0.39269908f); break;
        case 9: // Classic crescent moon, opening to the right.
            ellipse(bitmap, -0.08f, 0, 0.82f, 0.90f,
                    0.63f, TAU - 0.63f);
            cubic(bitmap,
                  (point_t){0.58f, -0.53f},
                  (point_t){-0.10f, -0.28f},
                  (point_t){-0.10f, 0.28f},
                  (point_t){0.58f, 0.53f});
            break;
        case 10:
            for (int i = 0; i <= 240; i++) {
                const float t = TAU * (float)i / 240.0f;
                const float d = 1.0f + sinf(t) * sinf(t);
                pixel(bitmap, sx(0.92f * cosf(t) / d), sy(0.72f * sinf(t) * cosf(t) / d));
            }
            break;
        case 11: {
            const point_t p[] = {
                {-0.25f,-0.92f}, {0.25f,-0.92f}, {0.25f,-0.25f},
                {0.92f,-0.25f}, {0.92f,0.25f}, {0.25f,0.25f},
                {0.25f,0.92f}, {-0.25f,0.92f}, {-0.25f,0.25f},
                {-0.92f,0.25f}, {-0.92f,-0.25f}, {-0.25f,-0.25f},
            };
            poly(bitmap, p, 12, true);
            break;
        }
        case 12:
            for (int i = 0; i <= 240; i++) {
                const float x = -0.92f + 1.84f * (float)i / 240.0f;
                pixel(bitmap, sx(x), sy(0.48f * sinf(x * 6.2f)));
            }
            break;
        case 13: flower(bitmap, 5); break;
        case 14: flower(bitmap, 6); break;
        case 15: flower(bitmap, 8); break;
        case 16:
            ellipse(bitmap, -0.42f, -0.22f, 0.42f, 0.48f, 0, TAU);
            ellipse(bitmap, 0.42f, -0.22f, 0.42f, 0.48f, 0, TAU);
            ellipse(bitmap, -0.33f, 0.42f, 0.30f, 0.34f, 0, TAU);
            ellipse(bitmap, 0.33f, 0.42f, 0.30f, 0.34f, 0, TAU);
            line(bitmap, 0, -0.72f, 0, 0.72f); line(bitmap, 0, -0.60f, -0.18f, -0.90f);
            line(bitmap, 0, -0.60f, 0.18f, -0.90f);
            break;
        case 17:
            ellipse(bitmap, 0, -0.37f, 0.38f, 0.38f, 0, TAU);
            ellipse(bitmap, 0.37f, 0, 0.38f, 0.38f, 0, TAU);
            ellipse(bitmap, 0, 0.37f, 0.38f, 0.38f, 0, TAU);
            ellipse(bitmap, -0.37f, 0, 0.38f, 0.38f, 0, TAU);
            line(bitmap, 0, 0.35f, 0.22f, 0.92f);
            break;
        case 18: { // Canadian maple leaf matching the supplied reference silhouette.
            const point_t p[] = {
                { 0.00f,-0.98f}, { 0.17f,-0.55f}, { 0.42f,-0.73f},
                { 0.34f,-0.32f}, { 0.58f,-0.48f}, { 0.67f,-0.29f},
                { 0.92f,-0.38f}, { 0.84f,-0.07f}, { 0.97f, 0.04f},
                { 0.61f, 0.36f}, { 0.68f, 0.66f}, { 0.10f, 0.55f},
                { 0.06f, 0.61f}, { 0.06f, 0.98f}, {-0.06f, 0.98f},
                {-0.06f, 0.61f}, {-0.10f, 0.55f}, {-0.68f, 0.66f},
                {-0.61f, 0.36f}, {-0.97f, 0.04f}, {-0.84f,-0.07f},
                {-0.92f,-0.38f}, {-0.67f,-0.29f}, {-0.58f,-0.48f},
                {-0.34f,-0.32f}, {-0.42f,-0.73f}, {-0.17f,-0.55f},
            };
            poly(bitmap, p, (int)(sizeof(p) / sizeof(p[0])), true);
            break;
        }
        case 19: {
            const point_t p[] = {{0,-0.95f},{0.66f,0.20f},{0.54f,0.66f},{0,0.92f},
                                 {-0.54f,0.66f},{-0.66f,0.20f}};
            poly(bitmap, p, 6, true); break;
        }
        case 20: {
            const point_t p[] = {{0.10f,-0.94f},{-0.56f,0.08f},{-0.10f,0.08f},
                                 {-0.36f,0.92f},{0.62f,-0.20f},{0.14f,-0.20f}};
            poly(bitmap, p, 6, true); break;
        }
        case 21:
            ellipse(bitmap, -0.48f, 0.08f, 0.34f, 0.34f, 2.7f, 5.9f);
            ellipse(bitmap, -0.12f, -0.20f, 0.46f, 0.48f, 3.1f, 6.1f);
            ellipse(bitmap, 0.38f, 0.02f, 0.38f, 0.38f, 3.5f, 6.5f);
            line(bitmap, 0.66f, 0.25f, -0.66f, 0.25f); break;
        case 22:
            ellipse(bitmap, 0, 0, 0.48f, 0.48f, 0, TAU);
            for (int i = 0; i < 12; i++) { const float a = TAU*i/12.0f;
                line(bitmap, cosf(a)*0.62f, sinf(a)*0.62f, cosf(a)*0.92f, sinf(a)*0.92f); }
            break;
        case 23:
            ellipse(bitmap, 0, 0.38f, 0.92f, 0.82f,
                    3.14159265359f, TAU);
            ellipse(bitmap, 0, 0.38f, 0.68f, 0.58f,
                    3.14159265359f, TAU);
            ellipse(bitmap, 0, 0.38f, 0.44f, 0.34f,
                    3.14159265359f, TAU);
            break;
        case 24: {
            ellipse(bitmap, -0.15f, 0, 0.62f, 0.45f, 0, TAU);
            const point_t p[] = {{0.40f,0},{0.90f,-0.48f},{0.82f,0},{0.90f,0.48f}};
            poly(bitmap, p, 4, false); ellipse(bitmap,-0.48f,-0.10f,0.05f,0.05f,0,TAU); break;
        }
        case 25:
            ellipse(bitmap,-0.36f,0.10f,0.48f,0.34f,3.4f,6.05f);
            ellipse(bitmap,0.36f,0.10f,0.48f,0.34f,3.37f,6.02f); break;
        case 26:
            ellipse(bitmap,0,0.12f,0.72f,0.66f,0,TAU);
            { const point_t p[]={{-0.66f,-0.16f},{-0.58f,-0.86f},{-0.20f,-0.48f},
                                 {0.20f,-0.48f},{0.58f,-0.86f},{0.66f,-0.16f}}; poly(bitmap,p,6,false); }
            ellipse(bitmap,-0.25f,0.02f,0.05f,0.05f,0,TAU); ellipse(bitmap,0.25f,0.02f,0.05f,0.05f,0,TAU);
            break;
        case 27:
            ellipse(bitmap,0,0.28f,0.63f,0.60f,0,TAU);
            ellipse(bitmap,-0.28f,-0.56f,0.20f,0.48f,0,TAU); ellipse(bitmap,0.28f,-0.56f,0.20f,0.48f,0,TAU);
            ellipse(bitmap,-0.20f,0.18f,0.05f,0.05f,0,TAU); ellipse(bitmap,0.20f,0.18f,0.05f,0.05f,0,TAU);
            break;
        case 28:
            ellipse(bitmap,0,0,0.82f,0.82f,0,TAU); ellipse(bitmap,-0.28f,-0.18f,0.06f,0.06f,0,TAU);
            ellipse(bitmap,0.28f,-0.18f,0.06f,0.06f,0,TAU); ellipse(bitmap,0,0.06f,0.48f,0.42f,0.25f,2.89f); break;
        case 29:
            line(bitmap,0.12f,-0.80f,0.12f,0.50f); line(bitmap,0.12f,-0.80f,0.72f,-0.62f);
            line(bitmap,0.72f,-0.62f,0.72f,0.28f); ellipse(bitmap,-0.10f,0.58f,0.30f,0.22f,0,TAU);
            ellipse(bitmap,0.50f,0.36f,0.30f,0.22f,0,TAU); break;
        case 30: { const point_t p[]={{-0.90f,0.55f},{-0.78f,-0.55f},{-0.32f,-0.12f},{0,-0.72f},
                                      {0.32f,-0.12f},{0.78f,-0.55f},{0.90f,0.55f}}; poly(bitmap,p,7,false); line(bitmap,-0.90f,0.55f,0.90f,0.55f); break; }
        case 31: { const point_t p[]={{0,-0.96f},{0.45f,-0.38f},{0.36f,0.48f},{0,0.78f},{-0.36f,0.48f},{-0.45f,-0.38f}}; poly(bitmap,p,6,true);
                   ellipse(bitmap,0,-0.22f,0.16f,0.16f,0,TAU); line(bitmap,-0.35f,0.25f,-0.75f,0.67f); line(bitmap,0.35f,0.25f,0.75f,0.67f); line(bitmap,0,0.78f,0,0.98f); break; }
        case 32:
            ellipse(bitmap,0,0,0.52f,0.52f,0,TAU); ellipse(bitmap,0,0,0.95f,0.30f,-0.15f,3.0f); ellipse(bitmap,0,0,0.95f,0.30f,3.0f,6.13f); break;
        case 33:
            for (int i=0;i<6;i++){float a=TAU*i/6.0f; line(bitmap,0,0,cosf(a)*0.9f,sinf(a)*0.9f);
                line(bitmap,cosf(a)*0.55f,sinf(a)*0.55f,cosf(a)*0.55f+cosf(a+2.45f)*0.25f,sinf(a)*0.55f+sinf(a+2.45f)*0.25f);
                line(bitmap,cosf(a)*0.55f,sinf(a)*0.55f,cosf(a)*0.55f+cosf(a-2.45f)*0.25f,sinf(a)*0.55f+sinf(a-2.45f)*0.25f);} break;
        case 34:
            ellipse(bitmap,0,0.02f,0.88f,0.72f,3.14159f,TAU); line(bitmap,-0.88f,0.02f,0.88f,0.02f);
            line(bitmap,0,-0.70f,0,0.72f); ellipse(bitmap,-0.16f,0.68f,0.16f,0.22f,-0.1f,1.8f); break;
        case 35: { const point_t p[]={{-0.82f,-0.10f},{0,-0.82f},{0.82f,-0.10f},{0.82f,0.78f},{-0.82f,0.78f}}; poly(bitmap,p,5,true);
                   const point_t d[]={{-0.18f,0.78f},{-0.18f,0.22f},{0.18f,0.22f},{0.18f,0.78f}}; poly(bitmap,d,4,false); break; }
        case 36:
            line(bitmap,0,-0.08f,0,0.92f); ellipse(bitmap,0,-0.38f,0.72f,0.55f,0,TAU);
            ellipse(bitmap,-0.42f,-0.08f,0.42f,0.40f,0,TAU); ellipse(bitmap,0.42f,-0.08f,0.42f,0.40f,0,TAU); break;
        case 37: { const point_t p[]={{-0.95f,0.75f},{-0.48f,-0.40f},{-0.12f,0.24f},{0.26f,-0.78f},{0.95f,0.75f}}; poly(bitmap,p,5,false); line(bitmap,-0.95f,0.75f,0.95f,0.75f); break; }
        case 38: // Rounded apple with a clean bent stem and highlight.
            cubic(bitmap,
                  (point_t){ 0.00f,-0.30f}, (point_t){-0.25f,-0.56f},
                  (point_t){-0.68f,-0.58f}, (point_t){-0.80f,-0.20f});
            cubic(bitmap,
                  (point_t){-0.80f,-0.20f}, (point_t){-0.92f, 0.15f},
                  (point_t){-0.68f, 0.72f}, (point_t){-0.25f, 0.74f});
            cubic(bitmap,
                  (point_t){-0.25f, 0.74f}, (point_t){-0.10f, 0.74f},
                  (point_t){-0.05f, 0.67f}, (point_t){ 0.00f, 0.68f});
            cubic(bitmap,
                  (point_t){ 0.00f, 0.68f}, (point_t){ 0.05f, 0.67f},
                  (point_t){ 0.10f, 0.74f}, (point_t){ 0.25f, 0.74f});
            cubic(bitmap,
                  (point_t){ 0.25f, 0.74f}, (point_t){ 0.68f, 0.72f},
                  (point_t){ 0.92f, 0.15f}, (point_t){ 0.80f,-0.20f});
            cubic(bitmap,
                  (point_t){ 0.80f,-0.20f}, (point_t){ 0.68f,-0.58f},
                  (point_t){ 0.25f,-0.56f}, (point_t){ 0.00f,-0.30f});
            // Clean, slightly bent fruit stem; the leaf is intentionally gone.
            line(bitmap, -0.04f, -0.28f, 0.03f, -0.88f);
            line(bitmap,  0.03f, -0.28f, 0.13f, -0.83f);
            line(bitmap,  0.03f, -0.88f, 0.13f, -0.83f);
            // The reference's curved shine mark keeps the apple readable.
            cubic(bitmap,
                  (point_t){-0.55f,-0.12f}, (point_t){-0.66f, 0.10f},
                  (point_t){-0.56f, 0.38f}, (point_t){-0.39f, 0.50f});
            break;
        case 39: { // Framed glass hourglass with visibly falling sand.
            // Distinct top and bottom frame bars.
            line(bitmap, -0.84f, -0.92f, 0.84f, -0.92f);
            line(bitmap, -0.84f, -0.78f, 0.84f, -0.78f);
            line(bitmap, -0.84f, -0.92f,-0.84f, -0.78f);
            line(bitmap,  0.84f, -0.92f, 0.84f, -0.78f);
            line(bitmap, -0.84f,  0.78f, 0.84f,  0.78f);
            line(bitmap, -0.84f,  0.92f, 0.84f,  0.92f);
            line(bitmap, -0.84f,  0.78f,-0.84f,  0.92f);
            line(bitmap,  0.84f,  0.78f, 0.84f,  0.92f);

            // Curved glass sides converge at the narrow centre.
            cubic(bitmap,
                  (point_t){-0.66f,-0.76f}, (point_t){-0.62f,-0.34f},
                  (point_t){-0.20f,-0.16f}, (point_t){ 0.00f, 0.00f});
            cubic(bitmap,
                  (point_t){ 0.00f, 0.00f}, (point_t){-0.20f, 0.16f},
                  (point_t){-0.62f, 0.34f}, (point_t){-0.66f, 0.76f});
            cubic(bitmap,
                  (point_t){ 0.66f,-0.76f}, (point_t){ 0.62f,-0.34f},
                  (point_t){ 0.20f,-0.16f}, (point_t){ 0.00f, 0.00f});
            cubic(bitmap,
                  (point_t){ 0.00f, 0.00f}, (point_t){ 0.20f, 0.16f},
                  (point_t){ 0.62f, 0.34f}, (point_t){ 0.66f, 0.76f});

            // Upper remaining sand, falling stream, and lower sand pile.
            const point_t upper_sand[] = {
                {-0.46f,-0.60f}, {0.46f,-0.60f}, {0.00f,-0.10f},
            };
            poly(bitmap, upper_sand, 3, true);
            line(bitmap, 0.00f, -0.10f, 0.00f, 0.23f);
            const point_t lower_sand[] = {
                {-0.48f,0.62f}, {0.00f,0.23f}, {0.48f,0.62f},
            };
            poly(bitmap, lower_sand, 3, true);
            break;
        }
        default: return false;
    }
    return true;
}
