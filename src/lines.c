#include "okgf.h"
#include "okgf_internal.h"
#include <stddef.h>
#include <string.h>

/* Clipping uses x86 integer wrap and shift rules. Signed division truncates toward zero;
 * inputs must avoid zero divisors and quotient overflow. */
static int32_t mul32(int32_t a, int32_t b) {
    return okgf_signed32((uint32_t)a * (uint32_t)b);
}
static int32_t sar16(int32_t x) {
    return okgf_signed32(((uint32_t)x >> 16) | (x < 0 ? UINT32_C(0xFFFF0000) : 0));
}
static unsigned outcode(int32_t x, int32_t y, const OkgfRect *r) {
    return (x < r->left ? 1u : 0u) | (y < r->top ? 2u : 0u) | (x >= r->right ? 4u : 0u) |
           (y >= r->bottom ? 8u : 0u);
}

/* Packed unsigned arithmetic allows carries between channels. Clipping can swap coordinates
 * without swapping endpoint colors. */
static uint32_t interpolate_color(uint32_t a, uint32_t b, int32_t t) {
    uint32_t f = (uint32_t)t;
    uint32_t blue = (a & 255) + ((f * ((b & 255) - (a & 255))) >> 16);
    uint32_t green = (a & 0xFF00) + (((f * (((b >> 8) & 255) - ((a >> 8) & 255))) >> 8) & 0xFFFF00);
    uint32_t red = (a & 0xFF0000) + ((f * (((b >> 16) & 255) - ((a >> 16) & 255))) & 0xFFFF0000);
    uint32_t alpha = (a & 0xFF000000) + (((f * ((b >> 24) - (a >> 24))) & 0xFF0000) << 8);
    return blue | green | red | alpha;
}

static int32_t clip_line(int32_t *px1, int32_t *py1, int32_t *px2, int32_t *py2, uint32_t *pc1,
                         uint32_t *pc2, const OkgfRect *r) {
    int32_t x, y, x2, y2;
    uint32_t c = 0, c2 = 0;
    OkgfRect bounds;
    memcpy(&x, (const void *)px1, sizeof(x));
    memcpy(&y, (const void *)py1, sizeof(y));
    memcpy(&x2, (const void *)px2, sizeof(x2));
    memcpy(&y2, (const void *)py2, sizeof(y2));
    memcpy(&bounds, (const void *)r, sizeof(bounds));
    r = &bounds;
    if (pc1) {
        memcpy(&c, (const void *)pc1, sizeof(c));
        memcpy(&c2, (const void *)pc2, sizeof(c2));
    }
    for (;;) {
        unsigned first = outcode(x, y, r), second = outcode(x2, y2, r);
        if (first & second)
            return 0;
        if (!(first | second))
            break;
        if (!first) {
            int32_t tmp = x;
            x = x2;
            x2 = tmp;
            tmp = y;
            y = y2;
            y2 = tmp;
            first = second;
        }
        int horizontal = (first & 1) || (!(first & 2) && (first & 4));
        int32_t edge = first & 1   ? r->left
                       : first & 2 ? r->top
                       : first & 4 ? okgf_sub32(r->right, 1)
                                   : okgf_sub32(r->bottom, 1);
        int32_t distance = okgf_sub32(edge, horizontal ? x : y);
        int32_t divisor = horizontal ? okgf_sub32(x2, x) : okgf_sub32(y2, y);
        int32_t other = horizontal ? okgf_sub32(y2, y) : okgf_sub32(x2, x);
        int32_t delta;
        if (pc1) {
            int32_t t = okgf_signed32((uint32_t)distance << 16) / divisor;
            delta = sar16(mul32(t, other));
            c = interpolate_color(c, c2, t);
        } else {
            delta = mul32(other, distance) / divisor;
        }
        if (horizontal) {
            x = edge;
            y = okgf_add32(y, delta);
        } else {
            y = edge;
            x = okgf_add32(x, delta);
        }
    }
    memcpy((void *)px1, &x, sizeof(x));
    memcpy((void *)py1, &y, sizeof(y));
    memcpy((void *)px2, &x2, sizeof(x2));
    memcpy((void *)py2, &y2, sizeof(y2));
    if (pc1) {
        memcpy((void *)pc1, &c, sizeof(c));
        memcpy((void *)pc2, &c2, sizeof(c2));
    }
    return 1;
}
int32_t OKGF_CALL OKGR_Line_Clip(int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2,
                                 const OkgfRect *clip) {
    return clip_line(x1, y1, x2, y2, NULL, NULL, clip);
}
int32_t OKGF_CALL OKGR_LineColor_Clip(int32_t *x1, int32_t *y1, uint32_t *c1, int32_t *x2,
                                      int32_t *y2, uint32_t *c2, const OkgfRect *clip) {
    return clip_line(x1, y1, x2, y2, c1, c2, clip);
}

static const uint8_t phase_alpha[360] = {
#include "tables/line_alpha_table.inc"
};
enum LineMode { SOLID, ALPHA, GATHER, SCATTER, ANIMATED, SHADOW, SOLID_DWORD, COPY };

/* Advance the minor coordinate only when error > 0. Visit both endpoints; a zero-length line
 * visits one pixel. */
static int32_t walk_line(void *pixels, int32_t pitch, int32_t x, int32_t y, int32_t x2, int32_t y2,
                         uint32_t color, uint8_t alpha, enum LineMode mode, void *linear,
                         int32_t phase, const OkgfRect *clip, const uint8_t *shadow,
                         int32_t shadow_pitch, int is555) {
    int64_t dx = (int64_t)x2 - x, dy = (int64_t)y2 - y;
    int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    int steep = dy > dx;
    int64_t major = steep ? dy : dx, minor = steep ? dx : dy, error = 2 * minor - major;
    for (int64_t i = 0; i <= major; ++i) {
        if (!clip || !outcode(x, y, clip)) {
            uint8_t *p = (uint8_t *)pixels + (ptrdiff_t)y * pitch +
                         (mode == SOLID_DWORD ? 4 : 2) * (ptrdiff_t)x;
            /* SR1's 555 alpha walker advances X before testing the major axis.
             * On steep lines every blended pixel is displaced from the solid first pixel. */
            if (mode == ALPHA && is555 && steep && i)
                p += 2 * (ptrdiff_t)sx - (ptrdiff_t)sy * pitch;
            if (mode == SOLID_DWORD)
                okgf_store32(p, color);
            else if (mode == COPY)
                okgf_store16(p, okgf_load16((const uint8_t *)linear + (ptrdiff_t)y * shadow_pitch +
                                            2 * (ptrdiff_t)x));
            else if (mode == GATHER)
                okgf_store16((uint8_t *)linear + 2 * i, okgf_load16(p));
            else if (mode == SCATTER)
                okgf_store16(p, okgf_load16((const uint8_t *)linear + 2 * i));
            else if (mode == SOLID || (mode == ALPHA && i == 0))
                okgf_store16(p, color);
            else if (mode == ALPHA) {
                if (is555)
                    OKGR_PixelAlpha_15(p, (uint16_t)color, alpha);
                else
                    OKGR_PixelAlpha_16(p, (uint16_t)color, alpha);
            } else {
                if (is555)
                    OKGR_PixelAlpha_15(p, (uint16_t)color, phase_alpha[phase]);
                else
                    OKGR_PixelAlpha_16(p, (uint16_t)color, phase_alpha[phase]);
                if (mode == SHADOW) {
                    unsigned shift = shadow[(ptrdiff_t)y * shadow_pitch + x];
                    okgf_store16(p, (uint16_t)((okgf_load16(p) >> (shift & 31)) &
                                               (is555 ? okgf_shift_mask555(shift)
                                                      : okgf_shift_mask565(shift))));
                }
            }
        }
        if (i == major)
            break;
        if (steep)
            y += sy;
        else
            x += sx;
        if (error > 0) {
            if (steep)
                x += sx;
            else
                y += sy;
            error += 2 * (minor - major);
        } else
            error += 2 * minor;
        if (mode == ANIMATED || mode == SHADOW) {
            phase += 20;
            if (phase >= 360)
                phase -= 360;
        }
    }
    return (int32_t)(major + 1);
}
#define LINE_PASS pixels, pitch, x1, y1, x2, y2, color
void OKGF_CALL OKGR_Line_Draw_WORD(OKGF_LINE_ARGS) {
    walk_line(LINE_PASS, 0, SOLID, NULL, 0, NULL, NULL, 0, 0);
}
void OKGF_CALL OKGR_Line_Draw_Alpha_16(OKGF_LINE_ARGS, uint8_t alpha) {
    walk_line(LINE_PASS, alpha, ALPHA, NULL, 0, NULL, NULL, 0, 0);
}
void OKGF_CALL OKGR_Line_DrawClip_WORD(OKGF_LINE_ARGS, const OkgfRect *clip) {
    if (OKGR_Line_Clip(&x1, &y1, &x2, &y2, clip))
        OKGR_Line_Draw_WORD(LINE_PASS);
}
void OKGF_CALL OKGR_Line_DrawClip_Alpha_16(OKGF_LINE_ARGS, uint8_t alpha, const OkgfRect *clip) {
    if (OKGR_Line_Clip(&x1, &y1, &x2, &y2, clip))
        OKGR_Line_Draw_Alpha_16(LINE_PASS, alpha);
}
int32_t OKGF_CALL OKGR_Line_CopyToBuf_WORD(void *linear_dest, const void *source, int32_t pitch,
                                           int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    return walk_line((void *)source, pitch, x1, y1, x2, y2, 0, 0, GATHER, linear_dest, 0, NULL,
                     NULL, 0, 0);
}
int32_t OKGF_CALL OKGR_Line_CopyFromBuf_WORD(const void *linear_source, void *dest, int32_t pitch,
                                             int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    return walk_line(dest, pitch, x1, y1, x2, y2, 0, 0, SCATTER, (void *)linear_source, 0, NULL,
                     NULL, 0, 0);
}
void OKGF_CALL OKGR_AnimLine_Draw_16(OKGF_LINE_ARGS, int32_t phase, const OkgfRect *clip) {
    walk_line(LINE_PASS, 0, ANIMATED, NULL, phase, clip, NULL, 0, 0);
}
void OKGF_CALL OKGR_AnimShadowLine_Draw_16(OKGF_LINE_ARGS, int32_t phase, const OkgfRect *clip,
                                           const uint8_t *shadow, int32_t shadow_pitch) {
    walk_line(LINE_PASS, 0, SHADOW, NULL, phase, clip, shadow, shadow_pitch, 0);
}

void OKGF_CALL OKGR_Line_Draw_DWORD(void *pixels, int32_t pitch, int32_t x1, int32_t y1, int32_t x2,
                                    int32_t y2, uint32_t color) {
    walk_line(LINE_PASS, 0, SOLID_DWORD, NULL, 0, NULL, NULL, 0, 0);
}
void OKGF_CALL OKGR_Line_Copy_WORD(void *dest, int32_t dest_pitch, const void *source,
                                   int32_t source_pitch, int32_t x1, int32_t y1, int32_t x2,
                                   int32_t y2) {
    walk_line(dest, dest_pitch, x1, y1, x2, y2, 0, 0, COPY, (void *)source, 0, NULL, NULL,
              source_pitch, 0);
}
void OKGF_CALL OKGR_Line_DrawClip_Alpha_15(OKGF_LINE_ARGS, uint8_t alpha, const OkgfRect *clip) {
    if (OKGR_Line_Clip(&x1, &y1, &x2, &y2, clip))
        walk_line(LINE_PASS, alpha, ALPHA, NULL, 0, NULL, NULL, 0, 1);
}
void OKGF_CALL OKGR_AnimLine_Draw_15(OKGF_LINE_ARGS, int32_t phase, const OkgfRect *clip) {
    walk_line(LINE_PASS, 0, ANIMATED, NULL, phase, clip, NULL, 0, 1);
}
void OKGF_CALL OKGR_AnimShadowLine_Draw_15(OKGF_LINE_ARGS, int32_t phase, const OkgfRect *clip,
                                           const uint8_t *shadow, int32_t shadow_pitch) {
    walk_line(LINE_PASS, 0, SHADOW, NULL, phase, clip, shadow, shadow_pitch, 1);
}
