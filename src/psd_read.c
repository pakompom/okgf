#include "okgf.h"
#include "okgf_internal.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void reverse(uint8_t *p, unsigned count) {
    for (unsigned i = 0; i < count / 2; ++i) {
        uint8_t t = p[i];
        p[i] = p[count - i - 1];
        p[count - i - 1] = t;
    }
}
void OKGF_CALL okgf_cancel_read_psd(OkgfPsdReadContext *context) {
    if (!context)
        return;
    if (context->owns_source)
        free((void *)context->source_data);
    free(context->header);
    free(context);
}
static OkgfPsdReadContext *begin(const uint8_t *source, int32_t size, int32_t *width,
                                 int32_t *height, int32_t *palette_count) {
    if (!source || size < 26 || memcmp(source, "8BPS", 4) ||
        (palette_count ? be16(source + 24) != 2 : be16(source + 24) == 2 || be16(source + 22) != 8))
        return NULL;
    OkgfPsdReadContext *context = calloc(1, sizeof(*context));
    if (!context)
        return NULL;
    context->header = malloc(26);
    if (!context->header) {
        free(context);
        return NULL;
    }
    memcpy(context->header, source, 26);
    const unsigned word_offsets[] = {4, 12, 22, 24};
    for (unsigned i = 0; i < 4; ++i)
        reverse(context->header + word_offsets[i], 2);
    reverse(context->header + 14, 4);
    reverse(context->header + 18, 4);
    uint32_t w = be32(source + 18), h = be32(source + 14), count = 256;
    memcpy(width, &w, 4);
    memcpy(height, &h, 4);
    if (palette_count)
        memcpy(palette_count, &count, 4);
    context->source_data = source;
    context->source_size = size;
    return context;
}
OkgfPsdReadContext *OKGF_CALL OKGR_ReadStart_PSD_Buf(const uint8_t *source, int32_t source_size,
                                                     int32_t *width, int32_t *height) {
    return begin(source, source_size, width, height, NULL);
}
OkgfPsdReadContext *OKGF_CALL OKGR_ReadStart_PSDPAL_Buf(const uint8_t *source, int32_t source_size,
                                                        int32_t *width, int32_t *height,
                                                        int32_t *palette_count) {
    return begin(source, source_size, width, height, palette_count);
}

static const uint8_t *take(const uint8_t **cursor, const uint8_t *end, size_t count) {
    if (count > (size_t)(end - *cursor))
        return NULL;
    const uint8_t *p = *cursor;
    *cursor += count;
    return p;
}
static int32_t decode(OkgfPsdReadContext *context, void *pixels, int32_t pitch, uint8_t *palette) {
    const uint8_t *source = context->source_data;
    int32_t width = (int32_t)be32(source + 18), height = (int32_t)be32(source + 14);
    unsigned channels = be16(source + 12);
    if (context->source_size < 26 || width <= 0 || height <= 0 || !channels || channels > 4 ||
        be16(source + 22) != 8 || (palette && be16(source + 24) != 2))
        return 0;
    size_t plane_size = (size_t)width * (size_t)height;
    if (plane_size / (size_t)height != (size_t)width || plane_size > SIZE_MAX / channels)
        return 0;
    /* The bitmap loader reparses the borrowed source, but the exported reader
     * copies the row size captured by begin. Preserve that distinction when a
     * caller changes the source header between calls, without reading past the
     * newly decoded row when the cached row is larger. The cached multiplication
     * itself wraps at 32 bits, as in the original reader. */
    uint32_t copy_bytes = okgf_load32(context->header + 18) * okgf_load16(context->header + 12);
    if (copy_bytes > (uint64_t)(uint32_t)width * channels)
        return 0;
    const uint8_t *cursor = source + 26, *end = source + context->source_size;
    const uint8_t *color_data = NULL;
    uint32_t color_size = 0;
    for (int section = 0; section < 3; ++section) {
        const uint8_t *length = take(&cursor, end, 4);
        if (!length)
            return 0;
        uint32_t size = be32(length);
        const uint8_t *data = take(&cursor, end, size);
        if (!data)
            return 0;
        if (!section) {
            color_data = data;
            color_size = size;
        }
    }
    if (palette && color_size != 768 && color_size != 1024)
        return 0;
    const uint8_t *compression = take(&cursor, end, 2);
    if (!compression || be16(compression) > 1)
        return 0;
    uint8_t *decoded = malloc(plane_size * channels);
    if (!decoded)
        return 0;
    if (!be16(compression)) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            const uint8_t *plane = take(&cursor, end, plane_size);
            if (!plane)
                goto failure;
            for (size_t i = 0; i < plane_size; ++i)
                decoded[i * channels + channel] = plane[i];
        }
    } else {
        const uint8_t *lengths = take(&cursor, end, 2 * (size_t)height * channels);
        if (!lengths)
            goto failure;
        for (unsigned channel = 0; channel < channels; ++channel) {
            for (int32_t y = 0; y < height; ++y) {
                unsigned size = be16(lengths + 2 * ((size_t)channel * height + y));
                const uint8_t *row = take(&cursor, end, size);
                if (!row)
                    goto failure;
                unsigned used = 0;
                size_t x = 0;
                while (used < size) {
                    unsigned command = row[used++];
                    unsigned count = command < 128 ? command + 1 : 257 - command;
                    unsigned input = command < 128 ? count : 1;
                    if (input > size - used || count > (size_t)width - x)
                        goto failure;
                    for (unsigned i = 0; i < count; ++i)
                        decoded[((size_t)y * width + x++) * channels + channel] =
                            row[used + (command < 128 ? i : 0)];
                    used += input;
                }
                if (x != (size_t)width)
                    goto failure;
            }
        }
    }
    if (palette) {
        for (unsigned i = 0; i < 256; ++i) {
            palette[4 * i] = color_data[i];
            palette[4 * i + 1] = color_data[i + 256];
            palette[4 * i + 2] = color_data[i + 512];
            palette[4 * i + 3] = color_size == 1024 ? color_data[i + 768] : 255;
        }
    }
    for (int32_t y = 0; y < height; ++y)
        memcpy((uint8_t *)pixels + (ptrdiff_t)y * pitch, decoded + (size_t)y * width * channels,
               (size_t)copy_bytes);
    free(decoded);
    okgf_cancel_read_psd(context);
    return 1;
failure:
    free(decoded);
    return 0;
}
int32_t OKGF_CALL OKGF_Read_PSD(OkgfPsdReadContext *context, void *pixels, int32_t pitch_bytes) {
    return decode(context, pixels, pitch_bytes, NULL);
}
int32_t OKGF_CALL OKGF_Read_PSDPAL(OkgfPsdReadContext *context, void *pixels, int32_t pitch_bytes,
                                   void *palette_rgba) {
    return decode(context, pixels, pitch_bytes, palette_rgba);
}
