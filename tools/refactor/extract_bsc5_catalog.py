#!/usr/bin/env python3
from pathlib import Path

sketch = Path(
    "firmware/Nexstar_Protocol_Converter/"
    "Nexstar_Protocol_Converter.ino"
)
header = sketch.with_name("bsc5_catalog_data.h")

start_marker = (
    "// Full BSC5 catalog is served on demand "
    "so the main web page stays small."
)
end_marker = "void handleBsc5DataPage()"
include_line = '#include "bsc5_catalog_data.h"'

text = sketch.read_text(encoding="utf-8")

if include_line in text and header.exists():
    print("Already modularized.")
    raise SystemExit(0)

start = text.find(start_marker)
end = text.find(end_marker, start)

if start < 0:
    print("ERROR: Start marker not found.")
    raise SystemExit(2)

if end < 0:
    print("ERROR: End marker not found.")
    raise SystemExit(3)

block = text[start:end].rstrip() + "\n"

required = (
    "BSC5_ROW_COUNT",
    "BSC5_CHUNK_COUNT",
    "BSC5_CHUNK_0",
    "BSC5_CHUNKS",
)

missing = [name for name in required if name not in block]

if missing:
    print("ERROR: Missing expected symbols:", ", ".join(missing))
    raise SystemExit(4)

header_text = (
    "#pragma once\n\n"
    "#include <Arduino.h>\n\n"
    "// Bright Star Catalog data extracted from the main sketch.\n"
    "// Runtime behavior and catalog contents are unchanged.\n\n"
    + block
)

replacement = (
    "// Bright Star Catalog data is stored separately.\n"
    + include_line
    + "\n\n"
)

header.write_text(header_text, encoding="utf-8", newline="\n")
sketch.write_text(
    text[:start] + replacement + text[end:],
    encoding="utf-8",
    newline="\n",
)

print("Created:", header)
print("Updated:", sketch)
