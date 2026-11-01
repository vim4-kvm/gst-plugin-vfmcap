#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compare the actual SPIR-V with a double-precision colour reference.

Explicit offline execution: python3 tests/pq-sdr-compare.py BUILD_DIR OUTPUT_DIR.
Select a software Vulkan ICD in the environment. This tests arithmetic/storage,
not DMA imports, GPU stack qualification, real-time capture or throughput.
"""
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys


def pq(code):
    value = min(1, max(0, code)) ** (32 / 2523)
    return 10000 * (max(value - 3424 / 4096, 0) /
                    (2413 / 128 - 2392 / 128 * value)) ** (16384 / 2610)


def srgb(value):
    value = min(1, max(0, value))
    return value * 12.92 if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055


def linear(value):
    return value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4


def reference(samples, width, height, stride, rows, pitch, white, peak):
    knee = linear(.9)
    span = peak / white - knee
    slope = 1.055 / (2.4 * ((.9 + .055) / 1.055) ** 1.4)
    low, high = 0, 1
    while .1 * high / math.log1p(high * span) < slope:
        high *= 2
    for _ in range(80):
        middle = (low + high) / 2
        if .1 * middle / math.log1p(middle * span) < slope:
            low = middle
        else:
            high = middle
    shoulder = (low + high) / 2
    result = bytearray([0xa5] * (pitch * height * 3 // 2))
    quantize = lambda value: min(255, max(0, math.floor(value + .5)))
    pixels = []
    for row in range(height):
        for x in range(width):
            y = (samples[row * stride // 2 + x] / 64 - 64) / 876
            uv = rows * stride // 2 + (row // 2) * stride // 2 + x // 2 * 2
            cb, cr = ((samples[uv + i] / 64 - 512) / 896 for i in (0, 1))
            r, b = y + 1.4746 * cr, y + 1.8814 * cb
            rgb2020 = [pq(v) for v in (r, (y - .2627 * r - .0593 * b) / .678, b)]
            matrix = ((1.660491, -.587641, -.072850), (-.124550, 1.132900, -.008349),
                      (-.018151, -.100579, 1.118730))
            rgb709 = [sum(a * b for a, b in zip(coefficients, rgb2020)) / white
                      for coefficients in matrix]
            level = max(rgb709)
            if level > knee:
                encoded = .9 + .1 * math.log1p(shoulder * (min(level, peak / white) - knee)) / math.log1p(shoulder * span)
                rgb709 = [v * linear(encoded) / level for v in rgb709]
            r, g, b = map(srgb, rgb709)
            y709 = .2126 * r + .7152 * g + .0722 * b
            pixels.append((16 + 219 * y709, 128 + 224 * (b - y709) / 1.8556,
                           128 + 224 * (r - y709) / 1.5748))
            result[row * pitch + x] = quantize(pixels[-1][0])
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            block = [pixels[(y + dy) * width + x + dx] for dy in (0, 1) for dx in (0, 1)]
            start = pitch * height + (y // 2) * pitch + x
            result[start:start + 2] = bytes(quantize(sum(pixel[c] for pixel in block) / 4) for c in (1, 2))
    return result


def main():
    build, output = map(Path, sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for width, height, kind, white, peak in ((2, 2, 'black', 203, 1000),
            (16, 4, 'gray', 203, 1000), (30, 18, 'random', 203, 1000),
            (64, 48, 'random', 160, 10000), (64, 48, 'random', 1000, 1001),
            (640, 360, 'gray', 203, 1000)):
        stride = (width * 2 + 63) & ~63
        rows = (height + 15) & ~15
        pitch = (width + 63) & ~63
        samples = [0xa5a5] * (stride * rows * 3 // 4)
        rng = random.Random(73)
        for y in range(height):
            for x in range(width):
                code = 64 if kind == 'black' else 512 if kind == 'gray' else rng.randrange(64, 941)
                samples[y * stride // 2 + x] = code << 6
        for y in range(height // 2):
            for x in range(width):
                samples[rows * stride // 2 + y * stride // 2 + x] = (rng.randrange(64, 961) if kind == 'random' else 512) << 6
        name = f'{width}x{height}-{kind}-{white}-{peak}'
        source, target = output / (name + '.p010'), output / (name + '.nv12')
        source.write_bytes(struct.pack('<' + 'H' * len(samples), *samples))
        subprocess.run([str(build / 'pq-sdr-probe'), str(build / 'p010-to-sdr.spv'),
            *map(str, (width, height, stride, rows, pitch, white, peak)), str(source), str(target)], check=True)
        actual = target.read_bytes()
        expected = reference(samples, width, height, stride, rows, pitch, white, peak)
        errors = [abs(actual[y * pitch + x] - expected[y * pitch + x])
                  for y in range(height * 3 // 2) for x in range(width)]
        # A final 4-byte store may touch the last pair of row padding. The
        # remainder must retain its poison, proving pitch/UV bounds are honored.
        stored_width = (width + 3) & ~3
        assert all(actual[y * pitch + x] == 0xa5 for y in range(height * 3 // 2)
                   for x in range(stored_width, pitch)), name
        assert max(errors) <= 2, (name, max(errors))
        if kind == 'black':
            assert actual[0] == 16 and actual[height * pitch] == 128
        results.append(dict(fixture=name, max_error=max(errors), visible_samples=len(errors)))
    (output / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results))


if __name__ == '__main__':
    main()
