#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Embed a little-endian SPIR-V module as aligned C uint32_t words."""
import pathlib
import struct
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: embed-spirv.py INPUT.spv OUTPUT.h")
    data = pathlib.Path(sys.argv[1]).read_bytes()
    if len(data) < 20 or len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
        raise SystemExit("input is not a little-endian SPIR-V module")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    symbol = pathlib.Path(sys.argv[1]).stem.replace("-", "_")
    if not symbol.isascii() or not symbol.isidentifier():
        raise SystemExit("input filename must form a C identifier")
    lines = [f"/* Generated from {pathlib.Path(sys.argv[1]).stem}.comp; do not edit. */",
             "#include <stdint.h>",
             f"static const uint32_t {symbol}_spirv[] = {{"]
    for offset in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[offset:offset + 8]) + ",")
    lines.append("};\n")
    pathlib.Path(sys.argv[2]).write_text("\n".join(lines), encoding="ascii")


if __name__ == "__main__":
    main()
