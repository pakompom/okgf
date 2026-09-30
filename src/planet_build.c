#include "okgf.h"
#include "math/backend.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void store_byte_count(void *byte_count, int32_t value) {
    memcpy(byte_count, &value, sizeof(value));
}

static int32_t round_coordinate(double value) {
    OkgfFloat v;
    fp_set(v, value);
    return fp_round_coordinate(v, 1);
}

void OKGF_CALL OKGR_Planet2_TemplDel(OkgfPlanetTemplate *template_data) {
    if (!template_data)
        return;
    if (template_data->scanlines) {
        for (int32_t row = 0; row < template_data->height; ++row)
            free(template_data->scanlines[row].samples);
        free(template_data->scanlines);
    }
    free(template_data);
}

/* Rows with equal span widths share the same projection. Compute it once and copy the
 * coordinates into each row's sample array. */
static int project_span(OkgfPlanetSample *samples, int32_t count, int32_t texture_width) {
    OkgfFloat a, b;
    fp_set(a, count);
    fp_mul_d(a, a, 0.5);
    double half_span = fp_double(a);
    fp_mul_d(a, a, 3.1415926);
    double scaled_pi = fp_double(a);
    fp_set(a, texture_width);
    fp_mul_d(a, a, 0.5);
    double half_texture = fp_double(a);
    fp_set(a, half_span);
    fp_sub_d(a, a, 0.5);
    OkgfFloat radius = {*a};
    for (double t = 0; t < half_texture; t += 1.0) {
        /* Original: 0x1003461F..0x10034631.
         * Round basic operations at the caller's precision. FCOS returns an extended-precision
         * result. */
        fp_set(a, t);
        fp_div_d(a, a, half_texture);
        fp_mul_d(a, a, scaled_pi);
        fp_div_d(a, a, half_span);
        fp_cos(b, a);
        fp_mul(a, b, radius);
        fp_sub(a, radius, a);
        int32_t i = fp_round_coordinate(a, 1);
        if (i < 0 || i >= count)
            return 0;
        samples[i].source_x = (uint16_t)(uint32_t)t;
        if (i > 0 && samples[i - 1].source_x == 0)
            samples[i - 1].source_x = (uint16_t)(uint32_t)t;
    }
    return 1;
}

OkgfPlanetTemplate *OKGF_CALL OKGR_Planet2_TemplBuild(const void *source, int32_t source_pitch,
                                                      int32_t diameter, int32_t texture_width,
                                                      int32_t texture_height, int32_t *byte_count) {
    store_byte_count(byte_count, 0);
    if (diameter <= 0 || texture_width <= 0 || texture_height <= 0 ||
        diameter > (INT32_MAX - 20) / 28)
        return NULL;
    OkgfPlanetTemplate *result = calloc(1, sizeof(*result));
    if (!result)
        return NULL;
    store_byte_count(byte_count, 20);
    OkgfMathState saved = fp_enter();
    result->width = result->height = diameter;
    result->origin_x = result->origin_y = 1 - diameter / 2;
    OkgfPlanetSample **projected = calloc((size_t)diameter + 1, sizeof(*projected));
    result->scanlines = calloc((size_t)diameter, sizeof(*result->scanlines));
    if (!result->scanlines || !projected)
        goto failure;
    int64_t bytes = 20 + (int64_t)28 * diameter;
    store_byte_count(byte_count, (int32_t)bytes);
    double source_y = 0.0;
    OkgfFloat a;
    fp_set(a, texture_height);
    fp_div_d(a, a, diameter);
    const double source_dy = fp_double(a);
    const uint8_t *row_pixels = source;
    for (int32_t row = 0; row < diameter; ++row) {
        OkgfPlanetScanline *line = &result->scanlines[row];
        int32_t x, first = -1, last = -1;
        for (x = 0; x < diameter; ++x) {
            const uint8_t *a = row_pixels + (ptrdiff_t)4 * x + 3;
            unsigned bucket = *a >> 2;
            if (bucket == 63)
                break;
            if (bucket) {
                if (first == -1)
                    first = x;
                ++line->left_alpha_count;
            }
            if (a[4] < *a)
                break;
        }
        if (first == -1) {
            first = x;
            line->left_alpha_count = 0;
        }
        int32_t equal_run = 0;
        for (x = diameter - 1; x >= 0; --x) {
            const uint8_t *a = row_pixels + (ptrdiff_t)4 * x + 3;
            unsigned bucket = *a >> 2;
            if (bucket == 63) {
                equal_run = 0;
                break;
            }
            if (bucket) {
                if (last == -1)
                    last = x;
                ++line->right_alpha_count;
            }
            if (a[-4] < *a) {
                --line->right_alpha_count;
                break;
            }
            equal_run = a[-4] == *a ? equal_run + 1 : 0;
        }
        line->right_alpha_count -= equal_run;
        if (last == -1) {
            last = x;
            line->right_alpha_count = 0;
        }
        line->source_y = round_coordinate(source_y);
        line->dest_x = first + result->origin_x;
        line->pixel_count = last - first + 1;
        line->opaque_count = line->pixel_count - line->left_alpha_count - line->right_alpha_count;
        if (line->pixel_count <= 0 || line->left_alpha_count > line->pixel_count ||
            line->right_alpha_count > line->pixel_count)
            goto failure;
        bytes += (int64_t)4 * line->pixel_count;
        if (bytes > INT32_MAX)
            goto failure;
        line->samples = calloc((size_t)line->pixel_count, sizeof(*line->samples));
        if (!line->samples)
            goto failure;
        /* Original 0x10034310 updates this output before sampling each row.
         * It may alias source alpha bytes, including those read below. */
        store_byte_count(byte_count, (int32_t)bytes);
        if (projected[line->pixel_count]) {
            for (int32_t i = 0; i < line->pixel_count; ++i)
                line->samples[i].source_x = projected[line->pixel_count][i].source_x;
        } else {
            if (!project_span(line->samples, line->pixel_count, texture_width))
                goto failure;
            projected[line->pixel_count] = line->samples;
        }
        for (int32_t i = 0; i < line->left_alpha_count; ++i)
            line->samples[i].edge_alpha63 = row_pixels[(ptrdiff_t)4 * (first + i) + 3] >> 2;
        for (int32_t i = 0; i < line->right_alpha_count; ++i)
            line->samples[line->pixel_count - 1 - i].edge_alpha63 =
                row_pixels[(ptrdiff_t)4 * (last - i) + 3] >> 2;
        row_pixels += source_pitch;
        /* The original stores the accumulator to binary64 after every row. */
        fp_set(a, source_y);
        fp_add_d(a, a, source_dy);
        source_y = fp_double(a);
    }
    free(projected);
    fp_leave(saved);
    return result;
failure:
    free(projected);
    OKGR_Planet2_TemplDel(result);
    store_byte_count(byte_count, 0);
    fp_leave(saved);
    return NULL;
}
