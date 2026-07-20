#!/usr/bin/env python3
"""Convert a binary file to a C array for linking into the kernel."""
import sys

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <input.bin> <symbol_name>", file=sys.stderr)
    sys.exit(1)

infile, sym = sys.argv[1], sys.argv[2]

with open(infile, "rb") as f:
    data = f.read()

print("#include <stdint.h>")
print(f"const uint8_t {sym}_data[] = {{")
for i in range(0, len(data), 16):
    chunk = data[i : i + 16]
    print("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
print("};")
print(f"const uint32_t {sym}_size = {len(data)};")
