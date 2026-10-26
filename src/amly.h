/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VFMCAP_AMLY_H
#define VFMCAP_AMLY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __aarch64__
#include <arm_neon.h>

typedef struct {
    uint16x8_t even, odd, u, v;
} AmlySamples;

static inline uint8x16_t amly_word(const uint8_t *src)
{
    uint8x16_t reversed = vrev64q_u8(vld1q_u8(src));
    return vextq_u8(reversed, reversed, 8);
}

/* Gather eight five-byte groups after reversing VDIN's 128-bit words. */
static inline AmlySamples amly_samples(uint8x16x3_t bytes)
{
    static const uint8_t offsets[8] = {0, 5, 10, 15, 20, 25, 30, 35};
    uint8x8_t index = vld1_u8(offsets);
    uint16x8_t b0 = vmovl_u8(vqtbl3_u8(bytes, index));
    uint16x8_t b1 = vmovl_u8(vqtbl3_u8(bytes, vadd_u8(index, vdup_n_u8(1))));
    uint16x8_t b2 = vmovl_u8(vqtbl3_u8(bytes, vadd_u8(index, vdup_n_u8(2))));
    uint16x8_t b3 = vmovl_u8(vqtbl3_u8(bytes, vadd_u8(index, vdup_n_u8(3))));
    uint16x8_t b4 = vmovl_u8(vqtbl3_u8(bytes, vadd_u8(index, vdup_n_u8(4))));
    return (AmlySamples) {
        .even = vorrq_u16(vshlq_n_u16(b0, 2), vshrq_n_u16(b1, 6)),
        .odd = vorrq_u16(vshlq_n_u16(vandq_u16(b2, vdupq_n_u16(15)), 6), vshrq_n_u16(b3, 2)),
        .u = vorrq_u16(vshlq_n_u16(vandq_u16(b3, vdupq_n_u16(3)), 8), b4),
        .v = vorrq_u16(vshlq_n_u16(vandq_u16(b1, vdupq_n_u16(63)), 4), vshrq_n_u16(b2, 4)),
    };
}

static inline void amly_store(uint16_t *y0, uint16_t *y1, uint16_t *uv,
                              uint8x16x3_t a, uint8x16x3_t b)
{
    AmlySamples top = amly_samples(a), bottom = amly_samples(b);
    vst2q_u16(y0, (uint16x8x2_t){{vshlq_n_u16(top.even, 6), vshlq_n_u16(top.odd, 6)}});
    vst2q_u16(y1, (uint16x8x2_t){{vshlq_n_u16(bottom.even, 6), vshlq_n_u16(bottom.odd, 6)}});
    vst2q_u16(uv, (uint16x8x2_t){{vshlq_n_u16(vrhaddq_u16(top.u, bottom.u), 6),
                                 vshlq_n_u16(vrhaddq_u16(top.v, bottom.v), 6)}});
}
#endif

/* T7 VDIN no_swap_en=1 writes each 128-bit word in reverse byte order.
 * Undo that to obtain five-byte groups Y0, Cr, Y1, Cb, most significant
 * sample first. P010 stores each sample in bits 15..6.
 * Average adjacent chroma rows when converting 4:2:2 to 4:2:0. */
static inline uint8_t amly_byte(const uint8_t *row, size_t offset)
{
    return row[(offset & ~(size_t)15) | ((offset & 15) ^ 15)];
}

static inline void amly_to_p010(uint16_t *dst_y, uint16_t *dst_uv,
                               const uint8_t *src, size_t width, size_t height,
                               size_t src_stride, size_t dst_stride)
{
    for (size_t row = 0; row < height; row += 2) {
        const uint8_t *a = src + row * src_stride;
        const uint8_t *b = a + src_stride;
        uint16_t *y0 = dst_y + row * dst_stride;
        uint16_t *y1 = y0 + dst_stride;
        uint16_t *uv = dst_uv + (row / 2) * dst_stride;
        size_t x = 0;
#ifdef __aarch64__
        /* 32 pixels consume exactly five complete 128-bit source words.
         * The scalar tail also handles widths smaller than this block. */
        for (; x + 32 <= width; x += 32) {
            size_t offset = x * 5 / 2;
            uint8x16_t aa[5], bb[5];
            for (size_t i = 0; i < 5; i++) {
                aa[i] = amly_word(a + offset + i * 16);
                bb[i] = amly_word(b + offset + i * 16);
            }
            amly_store(y0 + x, y1 + x, uv + x,
                (uint8x16x3_t){{aa[0], aa[1], aa[2]}},
                (uint8x16x3_t){{bb[0], bb[1], bb[2]}});
            amly_store(y0 + x + 16, y1 + x + 16, uv + x + 16,
                (uint8x16x3_t){{vextq_u8(aa[2], aa[3], 8), vextq_u8(aa[3], aa[4], 8), vextq_u8(aa[4], aa[4], 8)}},
                (uint8x16x3_t){{vextq_u8(bb[2], bb[3], 8), vextq_u8(bb[3], bb[4], 8), vextq_u8(bb[4], bb[4], 8)}});
        }
#endif
        for (; x < width; x += 2) {
            size_t offset = x * 5 / 2;
            uint8_t aa[5], bb[5];
            for (size_t i = 0; i < 5; i++) {
                aa[i] = amly_byte(a, offset + i);
                bb[i] = amly_byte(b, offset + i);
            }
            y0[x] = ((aa[0] << 2) | (aa[1] >> 6)) << 6;
            y0[x + 1] = (((aa[2] & 15) << 6) | (aa[3] >> 2)) << 6;
            y1[x] = ((bb[0] << 2) | (bb[1] >> 6)) << 6;
            y1[x + 1] = (((bb[2] & 15) << 6) | (bb[3] >> 2)) << 6;
            unsigned v = ((aa[1] & 63) << 4) | (aa[2] >> 4);
            unsigned u = ((aa[3] & 3) << 8) | aa[4];
            v += ((bb[1] & 63) << 4) | (bb[2] >> 4);
            u += ((bb[3] & 3) << 8) | bb[4];
            uv[x] = ((u + 1) >> 1) << 6;
            uv[x + 1] = ((v + 1) >> 1) << 6;
        }
    }
}
#endif
