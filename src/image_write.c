#include "okgf.h"
#include "okgf_internal.h"
#include <png.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

int32_t OKGF_CALL OKGF_Write_BMP_File(const char *filename, const void *pixels, int32_t pitch_bytes,
                                      int32_t bits_per_pixel, uint32_t red_mask,
                                      uint32_t green_mask, uint32_t blue_mask, uint32_t alpha_mask,
                                      int32_t width, int32_t height) {
    (void)red_mask;
    (void)green_mask;
    (void)blue_mask;
    (void)alpha_mask;
    /* The bitmap serializer writes raw rows for every unsigned depth above eight,
     * even nonstandard depths and signed values that wrap to large unsigned ones.
     * Indexed depths instead use uninitialized palettes in the DLL; reject those. */
    if (width <= 0 || height <= 0 || (uint32_t)bits_per_pixel <= 8)
        return 1;
    if (!pitch_bytes) {
        uint32_t row_bytes = ((uint32_t)bits_per_pixel * (uint32_t)width) >> 3;
        pitch_bytes = (int32_t)(((row_bytes - 1) & ~3u) + 4);
    }
    if (pitch_bytes <= 0)
        return 1;
    FILE *file = fopen(filename, "wb");
    if (!file)
        return 1;
    uint8_t header[54] = {'B', 'M'};
    uint32_t data_size = (uint32_t)pitch_bytes * (uint32_t)height;
    okgf_store32(header + 2, data_size + 54);
    okgf_store32(header + 10, 54);
    okgf_store32(header + 14, 40);
    okgf_store32(header + 18, (uint32_t)width);
    okgf_store32(header + 22, (uint32_t)height);
    header[26] = 1;
    okgf_store16(header + 28, (uint16_t)bits_per_pixel);
    okgf_store32(header + 34, data_size);
    (void)fwrite(header, 1, sizeof(header), file);
    for (int32_t y = height; y > 0; --y)
        (void)fwrite((const uint8_t *)pixels + (ptrdiff_t)(y - 1) * pitch_bytes, 1,
                     (size_t)pitch_bytes, file);
    (void)fclose(file);
    return 1;
}

int32_t OKGF_CALL OKGF_Write_PNG_File(const char *filename, const void *pixels, int32_t pitch_bytes,
                                      int32_t width, int32_t height, int32_t has_alpha,
                                      int32_t swap_red_blue) {
    if (width <= 0 || height <= 0)
        return 0;
    FILE *file = fopen(filename, "wb");
    if (!file)
        return 0;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!info) {
        if (png)
            png_destroy_write_struct(&png, NULL);
        fclose(file);
        return 0;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(file);
        return 0;
    }
    png_init_io(png, file);
    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height, 8,
                 has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    if (swap_red_blue)
        png_set_bgr(png);
    for (int32_t y = 0; y < height; ++y)
        png_write_row(png, (png_const_bytep)pixels + (ptrdiff_t)y * pitch_bytes);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(file);
    return 1;
}
