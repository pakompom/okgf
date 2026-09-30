#include "okgf.h"
#include "okgf_internal.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(OkgfRotationScanline) == 32, "Rotation scanline layout must remain 32 bytes");

/* The loop uses the signed high half of step * (int32_t)(sample << 16).
 * Preserve both downward rounding and the accumulator's wrap at sample 32768. */
static int32_t sample_step(int32_t step, int32_t sample) {
    int64_t product = (int64_t)step * okgf_signed32((uint32_t)sample << 16);
    /* Original: 0x10062A02..0x10062A0E. IMUL returns the signed high word in EDX. */
    return okgf_signed32((uint32_t)((uint64_t)product >> 32));
}

void OKGF_CALL OKGR_RotateBuf_Free(OkgfRotationBuffer *buffer) {
    if (!buffer)
        return;
    for (unsigned i = 0; i < 256; ++i)
        free(buffer->frames[i].scanlines);
    free(buffer);
}

void OKGF_CALL OKGR_RotateBuf_Size(int32_t dest_x, int32_t dest_y, uint8_t angle,
                                   const OkgfRotationBuffer *rotation, OkgfRect *bounds) {
    const OkgfRotationFrame *frame = &rotation->frames[angle];
    OkgfRect result;
    result.left = okgf_add32(dest_x, frame->min_x);
    result.right = okgf_add32(dest_x, frame->max_x);
    result.top = okgf_add32(dest_y, frame->dest_y);
    result.bottom = okgf_add32(okgf_add32(result.top, frame->scanline_count), -1);
    memcpy(bounds, &result, sizeof(result));
}

static void draw(OKGF_ROTATE_ARGS, const void *clip, int pixel_step, int sample_bytes,
                 int transparent) {
    const OkgfRotationFrame *frame = &rotation->frames[angle];
    int64_t first_y = (int64_t)dest_y + frame->dest_y;
    OkgfRect bounds;
    if (clip) {
        memcpy(&bounds, clip, sizeof(bounds));
        if ((int64_t)dest_x + frame->min_x > bounds.right ||
            (int64_t)dest_x + frame->max_x < bounds.left || first_y > bounds.bottom ||
            first_y + frame->scanline_count - 1 < bounds.top)
            return;
        if ((int64_t)dest_x + frame->min_x >= bounds.left &&
            (int64_t)dest_x + frame->max_x <= bounds.right && first_y >= bounds.top &&
            first_y + frame->scanline_count - 1 <= bounds.bottom)
            clip = NULL;
    }
    for (int32_t row = 0; row < frame->scanline_count; ++row) {
        int64_t y = first_y + row;
        /* Original 0x10062C20 caches vertical trimming before drawing, but
         * reloads horizontal bounds for each row. The clip may alias output. */
        if (clip && (y < bounds.top || y > bounds.bottom))
            continue;
        const OkgfRotationScanline *line = &frame->scanlines[row];
        int64_t x = (int64_t)dest_x + line->dest_x;
        int64_t first = 0, end = line->pixel_count;
        if (clip) {
            OkgfRect row_bounds;
            memcpy(&row_bounds, clip, sizeof(row_bounds));
            if (x < row_bounds.left)
                first = (int64_t)row_bounds.left - x;
            if (x + end - 1 > row_bounds.right)
                end = (int64_t)row_bounds.right - x + 1;
        }
        /* Aliased bounds can move completely past the row. Reject before
         * narrowing so a large distance cannot wrap into a negative sample. */
        if (end <= 0 || first >= end)
            continue;
        for (int32_t sample = (int32_t)first; sample < end; ++sample) {
            int32_t sx =
                okgf_add32(line->source_x_start, sample_step(line->source_dx_16_16, sample));
            int32_t sy =
                okgf_add32(line->source_y_start, sample_step(line->source_dy_16_16, sample));
            const uint8_t *s =
                (const uint8_t *)source + (ptrdiff_t)sy * source_pitch + (ptrdiff_t)sx * pixel_step;
            uint8_t *d =
                (uint8_t *)dest + (ptrdiff_t)y * dest_pitch + (ptrdiff_t)(x + sample) * pixel_step;
            /* Read the complete sample before writing. BYTE transparency stores two bytes, so
             * consecutive writes overlap. */
            uint32_t value = 0;
            for (int byte = 0; byte < sample_bytes; ++byte)
                value |= (uint32_t)s[byte] << (8 * byte);
            if (!transparent || value) {
                for (int byte = 0; byte < sample_bytes; ++byte)
                    d[byte] = (uint8_t)(value >> (8 * byte));
            }
        }
    }
}

void OKGF_CALL OKGR_RotateBuf_Draw_BYTE(OKGF_ROTATE_ARGS) {
    draw(dest, dest_pitch, source, source_pitch, dest_x, dest_y, angle, rotation, NULL, 1, 1, 0);
}
void OKGF_CALL OKGR_RotateBuf_Draw_DWORD(OKGF_ROTATE_ARGS) {
    draw(dest, dest_pitch, source, source_pitch, dest_x, dest_y, angle, rotation, NULL, 4, 4, 0);
}
void OKGF_CALL OKGR_RotateBuf_DrawTrans_WORD(OKGF_ROTATE_ARGS) {
    draw(dest, dest_pitch, source, source_pitch, dest_x, dest_y, angle, rotation, NULL, 2, 2, 1);
}
void OKGF_CALL OKGR_RotateBuf_DrawTrans_BYTE(OKGF_ROTATE_ARGS) {
    draw(dest, dest_pitch, source, source_pitch, dest_x, dest_y, angle, rotation, NULL, 1, 2, 1);
}
void OKGF_CALL OKGR_RotateBuf_DrawTransClip_WORD(OKGF_ROTATE_ARGS, const OkgfRect *clip) {
    draw(dest, dest_pitch, source, source_pitch, dest_x, dest_y, angle, rotation, clip, 2, 2, 1);
}
void OKGF_CALL OKGR_RotateBuf_DrawTransClip_BYTE(OKGF_ROTATE_ARGS, const OkgfRect *clip) {
    draw(dest, dest_pitch, source, source_pitch, dest_x, dest_y, angle, rotation, clip, 1, 2, 1);
}
