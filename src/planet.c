#include "okgf.h"
#include "okgf_internal.h"
#include <stddef.h>
#include <string.h>

static const uint8_t cosine_light64[4096] = {
#include "tables/planet_light_table.inc"
};

static void draw(OKGF_PLANET_ARGS, const void *clip, int variant, int pixel_bytes, int is555) {
    OkgfRect bounds;
    if (clip)
        memcpy(&bounds, clip, sizeof(bounds));
    int64_t top = (int64_t)dest_y + template_data->origin_y;
    if (clip) {
        int64_t left = (int64_t)dest_x + template_data->origin_x;
        int64_t right = left + template_data->width;
        int64_t bottom = top + template_data->height;
        if (left >= bounds.left && right <= bounds.right && top >= bounds.top &&
            bottom <= bounds.bottom)
            clip = NULL;
        else if (left >= bounds.right || right <= bounds.left || top >= bounds.bottom ||
                 bottom <= bounds.top)
            return;
    }
    for (int32_t row = 0; row < template_data->height; ++row) {
        int64_t y = top + row;
        /* Original 0x100351B0 caches vertical trimming before drawing, but
         * reloads horizontal bounds for each row. The clip may alias output. */
        if (clip && (y < bounds.top || y >= bounds.bottom))
            continue;
        const OkgfPlanetScanline *line = &template_data->scanlines[row];
        int64_t x = (int64_t)dest_x + line->dest_x;
        int64_t first = 0, end = line->pixel_count;
        if (clip) {
            OkgfRect row_bounds;
            memcpy(&row_bounds, clip, sizeof(row_bounds));
            if (x < row_bounds.left)
                first = (int64_t)row_bounds.left - x;
            if (x + end > row_bounds.right)
                end = (int64_t)row_bounds.right - x;
        }
        /* Aliased bounds can move completely past the row. Reject before
         * narrowing so a large distance cannot wrap into a negative sample. */
        if (end <= 0 || first >= end)
            continue;
        const uint8_t *texture_row = texture + (ptrdiff_t)line->source_y * texture_pitch;
        const uint8_t *light_row =
            light->pixels +
            ((ptrdiff_t)light->origin_y + template_data->origin_y + row) * light->pitch_bytes +
            light->origin_x + line->dest_x;
        for (int32_t i = (int32_t)first; i < end; ++i) {
            const OkgfPlanetSample *sample = &line->samples[i];
            uint32_t sx = ((uint32_t)texture_x_offset + sample->source_x) & texture_x_mask;
            const uint8_t *texel = texture_row + (ptrdiff_t)sx * (variant == 2 ? 1 : 2);
            unsigned bucket = light_row[i] >> 2;
            if (variant == 3)
                bucket = cosine_light64[bucket * 64 + (texel[1] >> 2)];
            uint16_t color =
                okgf_load16((const uint8_t *)light_palette + 2 * (texel[0] * 64 + bucket));
            int edge =
                i < line->left_alpha_count || i >= line->left_alpha_count + line->opaque_count;
            unsigned alpha = edge ? sample->edge_alpha63 : 63;
            if (variant == 4)
                alpha = edge ? (alpha * (texel[1] >> 2) + 31) / 63 : texel[1] >> 2;
            uint8_t *pixel =
                (uint8_t *)dest + (ptrdiff_t)y * dest_pitch + (ptrdiff_t)(x + i) * pixel_bytes;
            if (pixel_bytes == 4) {
                pixel[0] = (uint8_t)(color << 3);
                pixel[1] = (uint8_t)((color >> 3) & 252);
                pixel[2] = (uint8_t)((color >> 8) & 248);
                pixel[3] = edge ? (uint8_t)(alpha * 4) : variant == 4 ? texel[1] : 255;
            } else {
                if (edge || variant == 4)
                    color = is555 ? okgf_blend555_truncated(color, okgf_load16(pixel), alpha)
                                  : okgf_blend565_truncated(color, okgf_load16(pixel), alpha);
                pixel[0] = (uint8_t)color;
                pixel[1] = (uint8_t)(color >> 8);
            }
        }
    }
}

#define DRAW_ARGS                                                                                  \
    dest, dest_pitch, template_data, texture, texture_pitch, texture_x_mask, texture_x_offset,     \
        light, light_palette, dest_x, dest_y
#define PLANET_VARIANT(n)                                                                          \
    void OKGF_CALL OKGR_Planet##n##_DrawAndLight_32(OKGF_PLANET_ARGS) {                            \
        draw(DRAW_ARGS, NULL, n, 4, 0);                                                            \
    }                                                                                              \
    void OKGF_CALL OKGR_Planet##n##_DrawAndLight_16(OKGF_PLANET_ARGS) {                            \
        draw(DRAW_ARGS, NULL, n, 2, 0);                                                            \
    }                                                                                              \
    void OKGF_CALL OKGR_Planet##n##_DrawAndLightClip_16(OKGF_PLANET_ARGS, const OkgfRect *clip) {  \
        draw(DRAW_ARGS, clip, n, 2, 0);                                                            \
    }
PLANET_VARIANT(2)
PLANET_VARIANT(3)
PLANET_VARIANT(4)

void OKGF_CALL OKGR_Planet2_DrawAndLightClip_15(OKGF_PLANET_ARGS, const OkgfRect *clip) {
    draw(DRAW_ARGS, clip, 2, 2, 1);
}
void OKGF_CALL OKGR_Planet3_DrawAndLightClip_15(OKGF_PLANET_ARGS, const OkgfRect *clip) {
    draw(DRAW_ARGS, clip, 3, 2, 1);
}
