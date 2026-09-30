#include "okgf.h"
#include "okgf_internal.h"
#include <stddef.h>
_Static_assert(sizeof(OkgfRleHeader) == 16, "Serialized RLE header must be 16 bytes");

enum BuildMode { KEYED_WORD, OPAQUE_BGRA, PREMULT_BGRA, INVERSE_ALPHA };

static int is_literal(const uint8_t *p, enum BuildMode mode, uint16_t key) {
    if (mode == KEYED_WORD)
        return okgf_load16(p) != key;
    unsigned a = p[3] >> 2;
    return mode == OPAQUE_BGRA ? a == 63 : a != 0 && a != 63;
}
static void emit(uint8_t *dest, int32_t *size, uint8_t v) {
    if (dest)
        dest[*size] = v;
    ++*size;
}
static int32_t build(const void *source, int32_t pitch, int32_t width, int32_t height, void *dest,
                     enum BuildMode mode, uint16_t key, int is555) {
    uint8_t *d = dest;
    const uint8_t *s = source;
    int32_t size = 16;
    int bpp = mode == KEYED_WORD ? 2 : 4;
    if (d) {
        okgf_store32(d + 4, (uint32_t)width);
        okgf_store32(d + 8, (uint32_t)height);
        /* The DLL stores a temporary payload stride before reading pixels,
         * then clears the full field at completion. Preserve the intermediate
         * byte when input overlaps the output header. */
        d[12] = mode == INVERSE_ALPHA ? 1 : 2;
    }
    for (int32_t y = 0; y < height; ++y, s += pitch) {
        int blank = 0;
        int pending_literal = -1;
        for (int32_t x = 0; x < width;) {
            int literal =
                pending_literal < 0 ? is_literal(s + bpp * x, mode, key) : pending_literal;
            int32_t n = 1;
            pending_literal = -1;
            while (n < 127 && x + n < width) {
                int next_literal = is_literal(s + bpp * (x + n), mode, key);
                if (next_literal != literal) {
                    /* A transition sample is classified before the preceding
                     * run is emitted. Emission may overwrite that sample when
                     * source and destination overlap. */
                    pending_literal = next_literal;
                    break;
                }
                ++n;
            }
            /* A full 127-sample run is flushed immediately in the DLL; only
             * a shorter entirely transparent row uses the 0x80 shortcut. */
            if (!literal && x == 0 && n == width && n < 127) {
                emit(d, &size, 128);
                blank = 1;
                break;
            }
            emit(d, &size, (uint8_t)(n | (literal ? 128 : 0)));
            if (literal) {
                for (int32_t i = 0; i < n; ++i) {
                    const uint8_t *p = s + bpp * (x + i);
                    if (mode == INVERSE_ALPHA)
                        emit(d, &size, (uint8_t)(63 - (p[3] >> 2)));
                    else {
                        uint16_t v;
                        if (mode == KEYED_WORD)
                            v = okgf_load16(p);
                        else {
                            unsigned r = p[2] >> 3, g = p[1] >> (is555 ? 3 : 2), b = p[0] >> 3;
                            if (mode == PREMULT_BGRA) {
                                unsigned a = p[3] >> 2;
                                r = okgf_scale63_truncated(r, a);
                                g = okgf_scale63_truncated(g, a);
                                b = okgf_scale63_truncated(b, a);
                            }
                            v = (uint16_t)(r << (is555 ? 10 : 11) | g << 5 | b);
                        }
                        emit(d, &size, (uint8_t)v);
                        emit(d, &size, (uint8_t)(v >> 8));
                    }
                }
            }
            x += n;
        }
        if (!blank)
            emit(d, &size, 0);
    }
    if (d) {
        okgf_store32(d, (uint32_t)(size - 16));
        okgf_store32(d + 12, 0);
    }
    return size;
}
int32_t OKGF_CALL OKGR_TransBuf_Build_WORD(const void *s, int32_t p, int32_t w, int32_t h, void *d,
                                           uint16_t key) {
    return build(s, p, w, h, d, KEYED_WORD, key, 0);
}
int32_t OKGF_CALL OKGR_TransBuf_BuildFromRGBA_16(const void *s, int32_t p, int32_t w, int32_t h,
                                                 void *d) {
    return build(s, p, w, h, d, OPAQUE_BGRA, 0, 0);
}
int32_t OKGF_CALL OKGR_TransAlphaBuf_BuildFromRGBA_16(const void *s, int32_t p, int32_t w,
                                                      int32_t h, void *d) {
    return build(s, p, w, h, d, PREMULT_BGRA, 0, 0);
}
int32_t OKGF_CALL OKGR_AlphaBuf_BuildFromRGBA(const void *s, int32_t p, int32_t w, int32_t h,
                                              void *d) {
    return build(s, p, w, h, d, INVERSE_ALPHA, 0, 0);
}

enum DrawMode {
    COPY_WORD,
    COPY_BGRA,
    HALF_WORD,
    ADD_WORD,
    UNPREMULT_BGRA,
    SCALE_WORD,
    ALPHA_BGRA,
    FILL_ALPHA_WORD,
    FILL_ALPHA_BGRA,
    MASK_WORD,
    MASK_DWORD,
    HALF_555,
    SCALE_555,
    FILL_ALPHA_555,
    COPY_PACKED,
    UNPREMULT_PACKED565,
    UNPREMULT_PACKED555,
    ALPHA_PACKED
};
static int output_size(enum DrawMode mode) {
    if (mode >= COPY_PACKED)
        return 3;
    return mode == COPY_BGRA || mode == UNPREMULT_BGRA || mode == ALPHA_BGRA ||
                   mode == FILL_ALPHA_BGRA || mode == MASK_DWORD
               ? 4
               : 2;
}
static int input_size(enum DrawMode mode) {
    if (mode == MASK_WORD || mode == MASK_DWORD)
        return 0;
    if (mode == SCALE_555 || mode == FILL_ALPHA_555 || mode == ALPHA_PACKED)
        return 1;
    if (mode == HALF_555 || mode >= COPY_PACKED)
        return 2;
    return mode >= SCALE_WORD ? 1 : 2;
}
static void literal_run(uint8_t *d, const uint8_t *s, int n, enum DrawMode mode, uint32_t color,
                        int clipped) {
    if (mode == COPY_WORD) {
        /* MOVSD reads a complete pair before writing it, which matters when
         * literal data overlaps the destination. The clipped decoder copies
         * a two-pixel fragment as one DWORD even at an unaligned address; the
         * sequential decoder always performs its alignment WORD first. */
        if (n && (!clipped || n > 2) && ((uintptr_t)d & 3)) {
            okgf_store16(d, okgf_load16(s));
            d += 2;
            s += 2;
            --n;
        }
        while (n >= 2) {
            okgf_store32(d, okgf_load32(s));
            d += 4;
            s += 4;
            n -= 2;
        }
        if (n)
            okgf_store16(d, okgf_load16(s));
        return;
    }
    if (mode == ADD_WORD) {
        /* DWORD addition can carry from one RGB565 pixel into the next. Two-pixel runs always use a
         * DWORD; longer runs align the destination first. */
        if (n > 2 && ((uintptr_t)d & 3)) {
            okgf_store16(d, (uint16_t)(okgf_load16(d) + okgf_load16(s)));
            d += 2;
            s += 2;
            --n;
        }
        while (n >= 2) {
            okgf_store32(d, okgf_load32(d) + okgf_load32(s));
            d += 4;
            s += 4;
            n -= 2;
        }
        if (n)
            okgf_store16(d, (uint16_t)(okgf_load16(d) + okgf_load16(s)));
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (mode == FILL_ALPHA_555) {
            OKGR_PixelAlpha_15(d, (uint16_t)color, *s++);
            d += 2;
        } else if (mode == SCALE_555) {
            uint16_t v = okgf_load16(d);
            unsigned a = *s++;
            /* SR1 uses 0x07e0 for the green index into a 32-entry row:
             * red bit 10 advances the alpha row. Keep the original mistake. */
            unsigned r = okgf_scale63_truncated((v >> 10) & 31, a);
            unsigned g = okgf_scale63_truncated((v >> 5) & 31, a + ((v >> 10) & 1));
            unsigned b = okgf_scale63_truncated(v & 31, a);
            okgf_store16(d, (uint16_t)(r << 10 | g << 5 | b));
            d += 2;
        } else if (mode == ALPHA_PACKED) {
            d[2] = (uint8_t)(252 - 4 * (unsigned)*s++);
            d += 3;
        } else if (mode == COPY_PACKED) {
            okgf_store16(d, okgf_load16(s));
            d[2] = 255;
            s += 2;
            d += 3;
        } else if (mode == UNPREMULT_PACKED565 || mode == UNPREMULT_PACKED555) {
            int is555 = mode == UNPREMULT_PACKED555;
            uint16_t v = okgf_load16(s);
            unsigned b = (v & 31) << 3;
            unsigned g = (v >> (is555 ? 2 : 3)) & (is555 ? 248 : 252);
            unsigned r = (v >> (is555 ? 7 : 8)) & 248;
            if (d[2]) {
                b = (uint8_t)(b * 255 / d[2]);
                g = (uint8_t)(g * 255 / d[2]);
                r = (uint8_t)(r * 255 / d[2]);
            }
            okgf_store16(d, (uint16_t)((r & 248) << (is555 ? 7 : 8) |
                                       (g & (is555 ? 248 : 252)) << (is555 ? 2 : 3) | b >> 3));
            s += 2;
            d += 3;
        } else if (mode == FILL_ALPHA_WORD) {
            OKGR_PixelAlpha_16(d, (uint16_t)color, *s++);
            d += 2;
        } else if (mode == FILL_ALPHA_BGRA) {
#if OKGF_GAME_RELEASE == OKGF_GAME_SR1
            unsigned a = *s++;
            for (unsigned k = 0; k < 3; ++k) {
                unsigned front = (color >> (8 * k)) & 255;
                d[k] = (uint8_t)((front * a + 127) / 255 + (d[k] * (255 - a) + 127) / 255);
            }
            unsigned opacity = d[3] + a;
            d[3] = (uint8_t)(opacity > 255 ? 255 : opacity);
#else
            okgf_store32(d, (color & UINT32_C(0xFFFFFF)) | (uint32_t)*s++ << 24);
#endif
            d += 4;
        } else if (mode == MASK_WORD) {
            okgf_store16(d, (uint16_t)color);
            d += 2;
        } else if (mode == MASK_DWORD) {
            okgf_store32(d, color);
            d += 4;
        } else if (mode == SCALE_WORD) {
            uint16_t v = okgf_load16(d);
            unsigned a = *s++;
            unsigned r = ((a * ((v >> 10) & 62) + 31) / 63) >> 1;
            unsigned g = (a * ((v >> 5) & 63) + 31) / 63;
            unsigned b = ((a * ((v & 31) * 2) + 31) / 63) >> 1;
            okgf_store16(d, (uint16_t)(r << 11 | g << 5 | b));
            d += 2;
        } else if (mode == ALPHA_BGRA) {
            d[3] = (uint8_t)(252 - 4 * (unsigned)*s++);
            d += 4;
        } else {
            uint16_t v = okgf_load16(s);
            s += 2;
            if (mode == COPY_WORD) {
                okgf_store16(d, v);
                d += 2;
            } else if (mode == HALF_WORD || mode == HALF_555) {
                unsigned mask = mode == HALF_555 ? 0x3def : 0x7bef;
                okgf_store16(d, (uint16_t)(((v >> 1) & mask) + ((okgf_load16(d) >> 1) & mask)));
                d += 2;
            } else {
                unsigned b = (v & 31) << 3, g = (v >> 3) & 252, r = (v >> 8) & 248;
                if (mode == UNPREMULT_BGRA && d[3]) {
                    unsigned a = d[3];
                    b = b * 255 / a;
                    g = g * 255 / a;
                    r = r * 255 / a;
                }
                d[0] = (uint8_t)b;
                d[1] = (uint8_t)g;
                d[2] = (uint8_t)r;
                if (mode == COPY_BGRA)
                    d[3] = 255;
                d += 4;
            }
        }
    }
}

static void draw(void *dest, int32_t pitch, const OkgfRleHeader *source, enum DrawMode mode,
                 uint32_t color) {
    const uint8_t *header = (const uint8_t *)source, *s = header + 16;
    int32_t remaining = (int32_t)okgf_load32(header), width = (int32_t)okgf_load32(header + 4);
    int input_bpp = input_size(mode), output_bpp = output_size(mode);
    ptrdiff_t offset = 0;
    while (remaining > 0) {
        unsigned command = *s++;
        --remaining;
        if (!command)
            offset += pitch - (ptrdiff_t)width * output_bpp;
        else if (command == 128)
            offset += pitch;
        else if (command < 128)
            offset += (ptrdiff_t)command * output_bpp;
        else {
            int n = command & 127;
            literal_run((uint8_t *)dest + offset, s, n, mode, color, 0);
            s += n * input_bpp;
            remaining -= n * input_bpp;
            offset += (ptrdiff_t)n * output_bpp;
        }
    }
}

static void draw_clip(OKGF_RLE_CLIP_ARGS, enum DrawMode mode, uint32_t color) {
    const uint8_t *header = (const uint8_t *)source, *s = header + 16;
    int32_t width = (int32_t)okgf_load32(header + 4), height = (int32_t)okgf_load32(header + 8);
    int output_bpp = output_size(mode);
    if (x >= clip->left && (int64_t)x + width <= clip->right && y >= clip->top &&
        (int64_t)y + height <= clip->bottom) {
        draw((uint8_t *)dest + (ptrdiff_t)y * pitch + output_bpp * (ptrdiff_t)x, pitch, source,
             mode, color);
        return;
    }
    if (x > clip->right || (int64_t)x + width <= clip->left || y > clip->bottom ||
        (int64_t)y + height <= clip->top)
        return;
    int input_bpp = input_size(mode);
    /* Rows end with 0 or the empty-row marker 0x80. The slow clipping path includes the right and
     * bottom edges. */
    for (int32_t row = 0; row < height; ++row) {
        int64_t py = (int64_t)y + row, px = x;
        if (py > clip->bottom)
            break;
        for (;;) {
            unsigned command = *s++;
            if (!command || command == 128)
                break;
            int n = command & 127;
            if (command & 128) {
                if (py >= clip->top) {
                    int lo = px < clip->left ? (int)((int64_t)clip->left - px) : 0;
                    int hi = px + n - 1 > clip->right ? (int)((int64_t)clip->right - px + 1) : n;
                    if (lo < hi)
                        literal_run((uint8_t *)dest + (ptrdiff_t)py * pitch +
                                        output_bpp * (ptrdiff_t)(px + lo),
                                    s + input_bpp * lo, hi - lo, mode, color, 1);
                }
                s += n * input_bpp;
            }
            px += n;
        }
    }
}
#define RLE_DRAW(name, mode)                                                                       \
    void OKGF_CALL name(OKGF_RLE_ARGS) {                                                           \
        draw(dest, pitch, source, mode, 0);                                                        \
    }
RLE_DRAW(OKGR_TransBuf_Draw_WORD, COPY_WORD)
RLE_DRAW(OKGR_TransBuf_Draw_RGBA, COPY_BGRA)
RLE_DRAW(OKGR_TransBuf_HADraw_16, HALF_WORD)
RLE_DRAW(OKGR_TransAlphaBuf_Draw_WORD, ADD_WORD)
RLE_DRAW(OKGR_TransAlphaBuf_Draw_RGBA, UNPREMULT_BGRA)
RLE_DRAW(OKGR_AlphaBuf_Draw_16, SCALE_WORD)
RLE_DRAW(OKGR_AlphaBuf_Draw_RGBA, ALPHA_BGRA)
#define RLE_CLIP(name, mode)                                                                       \
    void OKGF_CALL name(OKGF_RLE_CLIP_ARGS) {                                                      \
        draw_clip(dest, pitch, x, y, source, clip, mode, 0);                                       \
    }
RLE_CLIP(OKGR_TransBuf_DrawClip_WORD, COPY_WORD)
RLE_CLIP(OKGR_TransBuf_HADrawClip_16, HALF_WORD)
RLE_CLIP(OKGR_TransAlphaBuf_DrawClip_WORD, ADD_WORD)
RLE_CLIP(OKGR_AlphaBuf_DrawClip_16, SCALE_WORD)
void OKGF_CALL OKGR_MaskBuf_Draw_WORD(OKGF_RLE_ARGS, uint16_t color) {
    draw(dest, pitch, source, MASK_WORD, color);
}
void OKGF_CALL OKGR_MaskBuf_Draw_DWORD(OKGF_RLE_ARGS, uint32_t color) {
    draw(dest, pitch, source, MASK_DWORD, color);
}
void OKGF_CALL OKGR_MaskBuf_DrawClip_WORD(void *dest, int32_t pitch, int32_t x, int32_t y,
                                          const OkgfRleHeader *source, uint16_t color,
                                          const OkgfRect *clip) {
    draw_clip(dest, pitch, x, y, source, clip, MASK_WORD, color);
}
void OKGF_CALL OKGR_MaskBuf_DrawClip_DWORD(void *dest, int32_t pitch, int32_t x, int32_t y,
                                           const OkgfRleHeader *source, uint32_t color,
                                           const OkgfRect *clip) {
    draw_clip(dest, pitch, x, y, source, clip, MASK_DWORD, color);
}

void OKGF_CALL OKGR_TransBuf_FillAlpha_16(OKGF_RLE_ARGS, uint16_t color) {
    draw(dest, pitch, source, FILL_ALPHA_WORD, color);
}
void OKGF_CALL OKGR_TransBuf_FillAlpha_RGBA(OKGF_RLE_ARGS, uint32_t color) {
    draw(dest, pitch, source, FILL_ALPHA_BGRA, color);
}
void OKGF_CALL OKGR_TransBuf_FillAlphaClip_16(OKGF_RLE_CLIP_ARGS, uint16_t color) {
    draw_clip(dest, pitch, x, y, source, clip, FILL_ALPHA_WORD, color);
}
void OKGF_CALL OKGR_TransBuf_FillAlphaClip_RGBA(OKGF_RLE_CLIP_ARGS, uint32_t color) {
    draw_clip(dest, pitch, x, y, source, clip, FILL_ALPHA_BGRA, color);
}

int32_t OKGF_CALL OKGR_TransBuf_BuildFromRGBA_15(const void *s, int32_t p, int32_t w, int32_t h,
                                                 void *d) {
    return build(s, p, w, h, d, OPAQUE_BGRA, 0, 1);
}
int32_t OKGF_CALL OKGR_TransAlphaBuf_BuildFromRGBA_15(const void *s, int32_t p, int32_t w,
                                                      int32_t h, void *d) {
    return build(s, p, w, h, d, PREMULT_BGRA, 0, 1);
}
RLE_DRAW(OKGR_TransBuf_Draw_5658, COPY_PACKED)
RLE_DRAW(OKGR_TransAlphaBuf_Draw_5658, UNPREMULT_PACKED565)
RLE_DRAW(OKGR_TransAlphaBuf_Draw_5558, UNPREMULT_PACKED555)
RLE_DRAW(OKGR_AlphaBuf_Draw_5658, ALPHA_PACKED)
RLE_CLIP(OKGR_TransBuf_HADrawClip_15, HALF_555)
RLE_CLIP(OKGR_AlphaBuf_DrawClip_15, SCALE_555)
void OKGF_CALL OKGR_TransBuf_FillAlphaClip_15(OKGF_RLE_CLIP_ARGS, uint16_t color) {
    draw_clip(dest, pitch, x, y, source, clip, FILL_ALPHA_555, color);
}
void OKGF_CALL OKGR_TransBuf_Convert565to555_WORD(OkgfRleHeader *source) {
    uint8_t *s = (uint8_t *)source + 16;
    int32_t remaining = (int32_t)okgf_load32((const uint8_t *)source);
    while (remaining > 0) {
        unsigned command = *s++;
        --remaining;
        if (command > 128) {
            unsigned count = command & 127;
            remaining -= 2 * (int32_t)count;
            for (unsigned i = 0; i < count; ++i, s += 2)
                okgf_store16(s, okgf_565_to555(okgf_load16(s)));
        }
    }
}
