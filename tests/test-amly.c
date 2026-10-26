/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <string.h>
#include "amly.h"

int main(void)
{
    /* Full-scale extrema and low bits survive packing and vertical averaging.
     * Expected bytes are independently hand-packed, including padding. */
    const uint8_t raw[] = {
        0xff,0x03,0x10,0xc0,0xff,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,
        0xfe,0x0b,0xe0,0xbf,0xff,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb};
    uint16_t y[8], uv[4];
    memset(y, 0xcc, sizeof(y));
    memset(uv, 0xcc, sizeof(uv));
    amly_to_p010(y, uv, raw, 2, 2, 16, 4);
    assert(y[0] == 0 && y[1] == (1023 << 6));
    assert(y[4] == (2 << 6) && y[5] == (1022 << 6));
    assert(uv[0] == (1023 << 6) && uv[1] == (512 << 6));
    assert(y[2] == 0xcccc && y[6] == 0xcccc && uv[2] == 0xcccc);
    /* Five-byte groups cross 128-bit words; test both tails and padded rows. */
    for (size_t width = 2; width <= 66; width += 2) {
        uint8_t input[1024];
        uint16_t luma[132], chroma[66];
        size_t stride = (width * 5 / 2 + 15) & ~(size_t)15;
        memset(input, 0xaa, sizeof(input));
        for (size_t row = 0; row < 2; row++) {
            for (size_t x = 0; x < width; x += 2) {
                uint64_t packed = (1023 - x - row) |
                    ((uint64_t)(x + row) << 10) |
                    ((uint64_t)(511 - x + row) << 20) |
                    ((uint64_t)(x + row + 1) << 30);
                for (size_t byte = 0; byte < 5; byte++) {
                    size_t index = x * 5 / 2 + byte;
                    input[row * stride + index] = packed >> (byte * 8);
                }
            }
        }
        amly_to_p010(luma, chroma, input, width, 2, stride, width);
        for (size_t x = 0; x < width; x++) {
            assert(luma[x] == x << 6);
            assert(luma[width + x] == (x + 1) << 6);
        }
        for (size_t x = 0; x < width; x += 2) {
            assert(chroma[x] == (1023 - x) << 6);
            assert(chroma[x + 1] == (512 - x) << 6);
        }
    }
    /* Independent bytes from a live T7 black/white marker capture. Reversing
     * 128-bit words creates periodic color stripes inside these flat regions. */
    const uint8_t marker[] = {
        0x00,0x02,0x01,0x20,0x10, 0x00,0xb2,0x0e,0x20,0xeb, 0,0,0,0,0,0,
        0x00,0x02,0x01,0x20,0x10, 0x00,0xb2,0x0e,0x20,0xeb, 0,0,0,0,0,0};
    amly_to_p010(y, uv, marker, 4, 2, 16, 4);
    for (size_t row = 0; row < 2; row++) {
        assert(y[row * 4] == (64 << 6) && y[row * 4 + 1] == (64 << 6));
        assert(y[row * 4 + 2] == (940 << 6) && y[row * 4 + 3] == (940 << 6));
    }
    for (size_t x = 0; x < 4; x++)
        assert(uv[x] == (512 << 6));
    /* Live red and blue blocks also distinguish Cb from Cr: grayscale alone
     * cannot detect a swapped chroma pair. */
    const uint8_t colors[] = {
        0x98,0xe1,0x03,0x3c,0x3f, 0xc0,0x03,0x82,0x1d,0x20, 0,0,0,0,0,0,
        0x98,0xe1,0x03,0x3c,0x3f, 0xc0,0x03,0x82,0x1d,0x20, 0,0,0,0,0,0};
    amly_to_p010(y, uv, colors, 4, 2, 16, 4);
    assert(y[0] == (248 << 6) && y[1] == (252 << 6));
    assert(y[2] == (128 << 6) && y[3] == (128 << 6));
    assert(uv[0] == (408 << 6) && uv[1] == (960 << 6));
    assert(uv[2] == (960 << 6) && uv[3] == (472 << 6));
    return 0;
}
