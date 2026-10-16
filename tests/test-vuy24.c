/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <string.h>
#include "vuy24.h"

int main(void)
{
	/* Exercise SIMD blocks, tails, padded rows and unaligned input. */
	for (size_t width = 1; width <= 65; width++) {
		uint8_t src[2048], dst[2048];
		size_t stride = width * 3 + 7;
		memset(src, 0xab, sizeof(src));
		memset(dst, 0xcd, sizeof(dst));
		for (size_t y = 0; y < 3; y++)
			for (size_t x = 0; x < width; x++) {
				size_t i = 1 + y * stride + x * 3;
				src[i] = (x & 1) ? 16 : 240;
				src[i + 1] = (y & 1) ? 154 : 103;
				src[i + 2] = (uint8_t)(x + y);
			}
		vuy24_to_v308(dst + 1, src + 1, width, 3, stride);
		assert(dst[0] == 0xcd);
		for (size_t y = 0; y < 3; y++) {
			for (size_t x = 0; x < width; x++) {
				size_t i = 1 + y * stride + x * 3;
				assert(dst[i] == src[i + 2]);
				assert(dst[i + 1] == src[i + 1]);
				assert(dst[i + 2] == src[i]);
			}
			for (size_t x = width * 3; x < stride; x++)
				assert(dst[1 + y * stride + x] == 0xcd);
		}
		assert(dst[1 + 3 * stride] == 0xcd);
		vuy24_to_v308(src + 1, src + 1, width, 3, stride);
		for (size_t y = 0; y < 3; y++)
			assert(memcmp(src + 1 + y * stride, dst + 1 + y * stride,
			              width * 3) == 0);
	}
	return 0;
}
