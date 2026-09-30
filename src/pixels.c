#include "okgf.h"
#include "okgf_internal.h"
#include <stddef.h>

static uint16_t rgb565(const uint8_t *s) {
    return (uint16_t)((s[0] & 248u) << 8 | (s[1] & 252u) << 3 | s[2] >> 3);
}
static uint16_t rgb555(const uint8_t *s) {
    return (uint16_t)((s[0] & 248u) << 7 | (s[1] & 248u) << 2 | s[2] >> 3);
}

/* Original: 0x1005B4D0 (RGB565), 0x1005B570 (RGB555).
 * Source pitch is implicitly width * 3. */
static void pack_rgb(const void *source, void *dest, int32_t pitch, int32_t width, int32_t height,
                     int is555) {
    if (width <= 0 || height <= 0)
        return;
    const uint8_t *s = source;
    uint8_t *d = dest;
    for (int32_t y = 0; y < height; ++y) {
        int32_t x = 0;
        /* Both releases read two RGB triples before the DWORD store. Preserve
         * that ordering when the packed output overlaps the source. */
        for (; x <= width - 2; x += 2, s += 6) {
            uint32_t first = is555 ? rgb555(s) : rgb565(s);
            uint32_t second = is555 ? rgb555(s + 3) : rgb565(s + 3);
            okgf_store32(d + (ptrdiff_t)2 * x, first | (second << 16));
        }
        if (x < width) {
            okgf_store16(d + (ptrdiff_t)2 * x, is555 ? rgb555(s) : rgb565(s));
            s += 3;
        }
        d += pitch;
    }
}
void OKGF_CALL OKGF_ConvertRGBto565(const void *s, void *d, int32_t p, int32_t w, int32_t h) {
    pack_rgb(s, d, p, w, h, 0);
}
void OKGF_CALL okgf_convert_rgb_to555(const void *s, void *d, int32_t p, int32_t w, int32_t h) {
    pack_rgb(s, d, p, w, h, 1);
}

/* Original converters: 0x1005B610..0x1005B923.
 * Zero-fill low color bits; never replicate bits. */
static void expand(const void *source, int32_t sp, void *dest, int32_t dp, int32_t w, int32_t h,
                   int bgr, int alpha, int source_alpha, int is555) {
    const uint8_t *s = source;
    uint8_t *d = dest;
    const int ss = source_alpha ? 3 : 2, ds = alpha ? 4 : 3;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            uint16_t v = okgf_load16(s + ss * x);
            uint8_t *p = d + ds * x;
            p[bgr ? 2 : 0] = (uint8_t)((v >> (is555 ? 7 : 8)) & 248);
            p[1] = (uint8_t)((v >> (is555 ? 2 : 3)) & (is555 ? 248 : 252));
            p[bgr ? 0 : 2] = (uint8_t)(v << 3);
            if (alpha)
                p[3] = source_alpha ? s[ss * x + 2] : 255;
        }
        s += sp;
        d += dp;
    }
}
#define EXPAND(name, bgr, alpha, sa, is555)                                                        \
    void OKGF_CALL name(const void *s, int32_t sp, void *d, int32_t dp, int32_t w, int32_t h) {    \
        expand(s, sp, d, dp, w, h, bgr, alpha, sa, is555);                                         \
    }
EXPAND(OKGF_Convert565toRGB, 0, 0, 0, 0)
EXPAND(OKGF_Convert565toBGR, 1, 0, 0, 0)
EXPAND(OKGF_Convert565toRGBA, 0, 1, 0, 0)
EXPAND(OKGF_Convert565toBGRA, 1, 1, 0, 0)
EXPAND(OKGF_Convert5658toRGBA, 0, 1, 1, 0)
EXPAND(OKGF_Convert5658toBGRA, 1, 1, 1, 0)
EXPAND(OKGF_Convert555toRGB, 0, 0, 0, 1)
EXPAND(OKGF_Convert555toBGR, 1, 0, 0, 1)
EXPAND(OKGF_Convert555toBGRA, 1, 1, 0, 1)
EXPAND(OKGF_Convert5558toBGRA, 1, 1, 1, 1)

#define RECT_FORWARD                                                                               \
    dest, dest_pitch, dest_x, dest_y, source, source_pitch, source_x, source_y, width, height
static void convert_rect(OKGF_RECT_ARGS, int source_bpp, int dest_bpp, int is555) {
    if (width <= 0 || height <= 0)
        return;
    const uint8_t *s =
        (const uint8_t *)source + (ptrdiff_t)source_pitch * source_y + source_bpp * source_x;
    uint8_t *d = (uint8_t *)dest + (ptrdiff_t)dest_pitch * dest_y + dest_bpp * dest_x;
    for (int32_t y = 0; y < height; ++y) {
        int32_t x = 0;
        if (dest_bpp == 2 && source_bpp != 2) {
            /* HD: 0x1005B930/0x1005BAA0; SR1 555: 0x1000CF10.
             * Read both RGB pixels before storing their packed DWORD. */
            for (; x <= width - 2; x += 2) {
                const uint8_t *p = s + (ptrdiff_t)source_bpp * x;
                uint32_t first = is555 ? rgb555(p) : rgb565(p);
                uint32_t second = is555 ? rgb555(p + source_bpp) : rgb565(p + source_bpp);
                okgf_store32(d + (ptrdiff_t)2 * x, first | (second << 16));
            }
        }
        for (; x < width; ++x) {
            const uint8_t *p = s + source_bpp * x;
            if (dest_bpp == 2)
                okgf_store16(d + 2 * x, source_bpp == 2 ? okgf_565_to555(okgf_load16(p))
                                        : is555         ? (uint16_t)((p[0] & 248u) << 7 |
                                                                     (p[1] & 248u) << 2 | p[2] >> 3)
                                                        : rgb565(p));
            else {
                /* 0x1005BA10 loads the source before either output store.
                 * Only its three RGB bytes affect the result. */
                uint16_t first = okgf_load16(p);
                uint8_t last = p[2];
                okgf_store16(d + 3 * x, first);
                d[3 * x + 2] = last;
            }
        }
        s += source_pitch;
        d += dest_pitch;
    }
}
void OKGF_CALL OKGF_Convert_888to565(OKGF_RECT_ARGS) {
    convert_rect(RECT_FORWARD, 3, 2, 0);
}
void OKGF_CALL OKGF_Convert_8888to565(OKGF_RECT_ARGS) {
    convert_rect(RECT_FORWARD, 4, 2, 0);
}
void OKGF_CALL OKGF_Convert_RGBAtoRGB(OKGF_RECT_ARGS) {
    convert_rect(RECT_FORWARD, 4, 3, 0);
}

void OKGF_CALL OKGR_Fill_WORD(void *pixels, int32_t pitch, int32_t width, int32_t height,
                              uint16_t color) {
    uint8_t *d = pixels;
    for (int32_t y = 0; y < height; ++y, d += pitch)
        for (int32_t x = 0; x < width; ++x)
            okgf_store16(d + 2 * x, color);
}

void OKGF_CALL OKGF_ConvertRGBto555(const void *s, void *d, int32_t p, int32_t w, int32_t h) {
    pack_rgb(s, d, p, w, h, 1);
}
void OKGF_CALL OKGF_Convert_8888to555(OKGF_RECT_ARGS) {
    convert_rect(RECT_FORWARD, 4, 2, 1);
}
void OKGF_CALL OKGF_Convert_565to555(OKGF_RECT_ARGS) {
    convert_rect(RECT_FORWARD, 2, 2, 1);
}
