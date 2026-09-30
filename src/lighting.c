#include "okgf.h"
#include "okgf_internal.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

/* Compute pair and tail counts from the first row's alignment. Reuse them if later rows change
 * alignment, preserving the resulting cursor drift. Rows must be WORD aligned. */
static void shift_light(void *pixels, int32_t pitch, int32_t width, int32_t height, int32_t shift,
                        int is555) {
    if (width <= 0 || height <= 0)
        return;
    uint8_t *d = pixels;
    unsigned n = shift < 0 ? 0u - (uint32_t)shift : (uint32_t)shift;
    /* Original: 0x1005C53C..0x1005C54F. NEG leaves INT32_MIN negative, selecting the
     * all-ones mask. Its shift count is zero, so the complement path clears pixels. */
    uint32_t m =
        shift == INT32_MIN ? UINT16_MAX : (is555 ? okgf_shift_mask555(n) : okgf_shift_mask565(n));
    m |= m << 16;
    unsigned tail = (unsigned)(((uintptr_t)d >> 1) ^ (uint32_t)width) & 1;
    unsigned pairs = ((unsigned)width - tail) >> 1;
    int complement = shift < -1;
    for (int32_t y = 0; y < height; ++y) {
        if ((uintptr_t)d & 3) {
            uint32_t v = okgf_load32(d), part = (v >> (n & 31)) & m;
            okgf_store16(d, (uint16_t)(complement ? v - part : part));
            d += 2;
        }
        for (unsigned x = 0; x < pairs; ++x) {
            uint32_t v = okgf_load32(d), part = (v >> (n & 31)) & m;
            okgf_store32(d, complement ? v - part : part);
            d += 4;
        }
        if (tail) {
            uint32_t v = okgf_load32(d), part = (v >> (n & 31)) & m;
            okgf_store16(d, (uint16_t)(complement ? v - part : part));
            d += 2;
        }
        d += pitch - 2 * width;
    }
}
void OKGF_CALL OKGR_HalfLight_16(void *p, int32_t pitch, int32_t w, int32_t h) {
    shift_light(p, pitch, w, h, 1, 0);
}
void OKGF_CALL OKGR_ShrLight_16(void *p, int32_t pitch, int32_t w, int32_t h, int32_t shift) {
    shift_light(p, pitch, w, h, shift, 0);
}

static uint8_t mul256[256 * 256];
static atomic_int mul_state = 0;
uint8_t *OKGF_CALL OKGF_MulTable256x256(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&mul_state, &expected, 1)) {
        for (unsigned a = 0; a < 256; ++a)
            for (unsigned b = 0; b < 256; ++b)
                mul256[a * 256 + b] = (uint8_t)((a * b + 127) / 255);
        atomic_store(&mul_state, 2);
    } else {
        while (atomic_load(&mul_state) != 2) {
        }
    }
    return mul256;
}
void OKGF_CALL OKGR_Light_BYTE(void *pixels, int32_t pixel_step, int32_t row_skip, int32_t width,
                               int32_t height, uint8_t factor) {
    uint8_t *p = pixels, *table = OKGF_MulTable256x256() + 256u * factor;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x, p += pixel_step)
            *p = table[*p];
        p += row_skip;
    }
}
void OKGF_CALL OKGR_MulLightMask_16(void *pixels, int32_t pitch, const void *mask,
                                    int32_t mask_pitch, int32_t width, int32_t height) {
    uint8_t *d = pixels;
    const uint8_t *s = mask;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            uint16_t v = okgf_load16(d + 2 * x);
            unsigned a = s[x] >> 2;
            unsigned r = okgf_scale63_rounded((v >> 10) & 62u, a) >> 1;
            unsigned g = okgf_scale63_rounded((v >> 5) & 63u, a);
            unsigned b = okgf_scale63_rounded((v & 31u) * 2, a) >> 1;
            okgf_store16(d + 2 * x, (uint16_t)(r << 11 | g << 5 | b));
        }
        d += pitch;
        s += mask_pitch;
    }
}
/* Reuse the first row's pair and tail counts even when later rows have different DWORD
 * alignment. */
static void shift_light_mask(void *pixels, int32_t pitch, const void *mask, int32_t mask_pitch,
                             int32_t width, int32_t height, int is555) {
    if (width <= 0 || height <= 0)
        return;
    uint8_t *d = pixels;
    const uint8_t *s = mask;
    unsigned tail = (unsigned)(((uintptr_t)d >> 1) ^ (uint32_t)width) & 1;
    unsigned pairs = ((unsigned)width - tail) >> 1;
    for (int32_t y = 0; y < height; ++y) {
        if ((uintptr_t)d & 3) {
            unsigned shift = *s++;
            okgf_store16(
                d, (uint16_t)((okgf_load16(d) >> (shift & 31)) &
                              (is555 ? okgf_shift_mask555(shift) : okgf_shift_mask565(shift))));
            d += 2;
        }
        for (unsigned x = 0; x < pairs; ++x) {
            /* HD: 0x1005C75D..0x1005C791. Load both mask
             * bytes and pixels before storing so aliased masks survive. */
            uint16_t shifts = okgf_load16(s);
            if (shifts) {
                unsigned first = shifts & 255, second = shifts >> 8;
                uint32_t value = okgf_load32(d);
                uint32_t low = ((value & 65535) >> (first & 31)) &
                               (is555 ? okgf_shift_mask555(first) : okgf_shift_mask565(first));
                uint32_t high = ((value >> 16) >> (second & 31)) &
                                (is555 ? okgf_shift_mask555(second) : okgf_shift_mask565(second));
                okgf_store32(d, low | (high << 16));
            }
            s += 2;
            d += 4;
        }
        if (tail) {
            unsigned shift = *s++;
            okgf_store16(
                d, (uint16_t)((okgf_load16(d) >> (shift & 31)) &
                              (is555 ? okgf_shift_mask555(shift) : okgf_shift_mask565(shift))));
            d += 2;
        }
        d += pitch - 2 * width;
        s += mask_pitch - width;
    }
}
OkgfLightBuffer *OKGF_CALL OKGR_LightBuf_Create(int32_t width, int32_t height) {
    OkgfLightBuffer *buffer = malloc(sizeof(*buffer));
    if (!buffer)
        return NULL;
    buffer->width = width;
    buffer->height = height;
    buffer->pitch_bytes = width + 16;
    buffer->origin_x = 0;
    buffer->origin_y = 0;
    buffer->pixels = malloc((size_t)height * buffer->pitch_bytes);
    if (!buffer->pixels) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

void OKGF_CALL OKGR_LightBuf_Destroy(OkgfLightBuffer *buffer) {
    free(buffer->pixels);
    free(buffer);
}

void OKGF_CALL OKGR_LightBuf_SetSme(OkgfLightBuffer *buffer, int32_t origin_x, int32_t origin_y) {
    buffer->origin_x = origin_x;
    buffer->origin_y = origin_y;
}

void OKGF_CALL OKGR_LightBuf_Init(OkgfLightBuffer *buffer, uint8_t value) {
    memset(buffer->pixels, value, (size_t)buffer->height * buffer->pitch_bytes);
}

void OKGF_CALL OKGR_LightBuf_LoadFromPalBuf(OkgfLightBuffer *buffer, const uint8_t *source,
                                            int32_t source_width, int32_t source_height,
                                            int32_t source_pitch, const void *palette) {
    int32_t width = source_width < buffer->width ? source_width : buffer->width;
    int32_t height = source_height < buffer->height ? source_height : buffer->height;
    uint8_t *dest = buffer->pixels;
    const uint8_t *colors = palette;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x)
            dest[x] = colors[4 * source[x]];
        dest += buffer->pitch_bytes;
        source += source_pitch;
    }
}

void OKGF_CALL OKGR_LightBuf_Rotate(OkgfLightBuffer *dest, const OkgfLightBuffer *source,
                                    const OkgfRotationBuffer *rotation, uint8_t angle) {
    OkgfRect clip = {0, 0, dest->width - 1, dest->height - 1};
    OKGR_RotateBuf_DrawTransClip_BYTE(dest->pixels, dest->pitch_bytes, source->pixels,
                                      source->pitch_bytes, source->origin_x, source->origin_y,
                                      angle, rotation, &clip);
}

void OKGF_CALL OKGR_ShrLight_15(void *p, int32_t pitch, int32_t w, int32_t h, int32_t shift) {
    shift_light(p, pitch, w, h, shift, 1);
}
void OKGF_CALL OKGR_ShrLightMask_16(void *pixels, int32_t pitch, const void *mask,
                                    int32_t mask_pitch, int32_t width, int32_t height) {
    shift_light_mask(pixels, pitch, mask, mask_pitch, width, height, 0);
}
void OKGF_CALL OKGR_ShrLightMask_15(void *pixels, int32_t pitch, const void *mask,
                                    int32_t mask_pitch, int32_t width, int32_t height) {
    shift_light_mask(pixels, pitch, mask, mask_pitch, width, height, 1);
}
void OKGF_CALL OKGR_MulLightMask_15(void *pixels, int32_t pitch, const void *mask,
                                    int32_t mask_pitch, int32_t width, int32_t height) {
    uint8_t *d = pixels;
    const uint8_t *s = mask;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            uint16_t v = okgf_load16(d + 2 * x);
            unsigned a = s[x] >> 3;
            unsigned r = okgf_scale31_rounded((v >> 10) & 31, a);
            unsigned g = okgf_scale31_rounded((v >> 5) & 31, a);
            unsigned b = okgf_scale31_rounded(v & 31, a);
            okgf_store16(d + 2 * x, (uint16_t)(r << 10 | g << 5 | b));
        }
        d += pitch;
        s += mask_pitch;
    }
}
