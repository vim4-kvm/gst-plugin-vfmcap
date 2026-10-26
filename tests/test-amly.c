/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <string.h>
#include "amly.h"

int main(void)
{
    /* Full-scale extrema and low bits survive packing and vertical averaging.
     * Expected bytes are independently hand-packed, including padding. */
    const uint8_t raw[] = {
        0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0x01,0xfc,0xff,0x3f,0x00,
        0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xfe,0xfb,0xef,0xbf,0x00};
    uint16_t y[8], uv[4];
    memset(y, 0xcc, sizeof(y));
    memset(uv, 0xcc, sizeof(uv));
    amly_to_p010(y, uv, raw, 2, 2, 16, 4);
    assert(y[0] == 0 && y[1] == (1023 << 6));
    assert(y[4] == (2 << 6) && y[5] == (1022 << 6));
    assert(uv[0] == (512 << 6) && uv[1] == (1023 << 6));
    assert(y[2] == 0xcccc && y[6] == 0xcccc && uv[2] == 0xcccc);
    /* Five-byte groups cross 128-bit words; test both tails and padded rows. */
    for (size_t width = 2; width <= 66; width += 2) {
        uint8_t input[1024];
        uint16_t luma[132], chroma[66];
        size_t stride = (width * 5 / 2 + 15) & ~(size_t)15;
        memset(input, 0xaa, sizeof(input));
        for (size_t row = 0; row < 2; row++) {
            for (size_t x = 0; x < width; x += 2) {
                uint64_t packed = ((uint64_t)(x + row) << 30) |
                    ((uint64_t)(1023 - x - row) << 20) |
                    ((uint64_t)(x + row + 1) << 10) | (511 - x + row);
                for (size_t byte = 0; byte < 5; byte++) {
                    size_t index = x * 5 / 2 + byte;
                    input[row * stride + (index ^ 15)] = packed >> (32 - byte * 8);
                }
            }
        }
        amly_to_p010(luma, chroma, input, width, 2, stride, width);
        for (size_t x = 0; x < width; x++) {
            assert(luma[x] == x << 6);
            assert(luma[width + x] == (x + 1) << 6);
        }
        for (size_t x = 0; x < width; x += 2) {
            assert(chroma[x] == (512 - x) << 6);
            assert(chroma[x + 1] == (1023 - x) << 6);
        }
    }
    return 0;
}
