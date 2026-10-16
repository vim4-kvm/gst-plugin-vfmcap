/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VUY24_H
#define VUY24_H

#include <stddef.h>
#include <stdint.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

/* Buffers may be identical; each row contains width packed three-byte pixels.
 * Keep both chroma samples; only reorder V-U-Y into GStreamer's Y-U-V. */
static inline void vuy24_to_v308(uint8_t *dst, const uint8_t *src,
                               size_t width, size_t height, size_t stride)
{
	for (size_t y = 0; y < height; y++) {
		size_t x = 0;
#ifdef __aarch64__
		for (; x + 16 <= width; x += 16) {
			uint8x16x3_t pixels = vld3q_u8(src + x * 3);
			uint8x16_t v = pixels.val[0];
			pixels.val[0] = pixels.val[2];
			pixels.val[2] = v;
			vst3q_u8(dst + x * 3, pixels);
		}
#endif
		for (; x < width; x++) {
			uint8_t v = src[x * 3];
			dst[x * 3] = src[x * 3 + 2];
			dst[x * 3 + 1] = src[x * 3 + 1];
			dst[x * 3 + 2] = v;
		}
		src += stride;
		dst += stride;
	}
}
#endif
