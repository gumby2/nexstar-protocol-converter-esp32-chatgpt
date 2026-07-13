#!/usr/bin/env python3
from pathlib import Path

sketch = Path(
    "firmware/Nexstar_Protocol_Converter/"
    "Nexstar_Protocol_Converter.ino"
)
header = sketch.with_name("https_credentials.h")

start_marker = "#if defined(ESP32)\nconst uint16_t HTTPS_SETUP_PORT"
end_marker = "#endif\n\nWiFiUDP discoveryUdp;"
include_line = '#include "https_credentials.h"'

text = sketch.read_text(encoding="utf-8")

if include_line in text and header.exists():
    print("Already modularized.")
    raise SystemExit(0)

start = text.find(start_marker)
end = text.find(end_marker, start)

if start < 0:
    print("ERROR: HTTPS credentials start marker not found.")
    raise SystemExit(2)

if end < 0:
    print("ERROR: HTTPS credentials end marker not found.")
    raise SystemExit(3)

block = text[start:end + len("#endif")].rstrip() + "\n"

required = (
    "HTTPS_SETUP_PORT",
    "HTTPS_SETUP_CERT",
    "HTTPS_SETUP_KEY",
    "httpsSetupServerHandle",
)

missing = [name for name in required if name not in block]

if missing:
    print("ERROR: Missing expected symbols:", ", ".join(missing))
    raise SystemExit(4)

header_text = (
    "#pragma once\n\n"
    "#include <Arduino.h>\n\n"
    "#if defined(ESP32)\n"
    'extern "C" {\n'
    '  #include "esp_https_server.h"\n'
    "}\n"
    "#endif\n\n"
    "// HTTPS setup certificate, key, and related state.\n"
    "// Extracted from the main sketch without behavior changes.\n\n"
    + block
)

replacement = (
    "// HTTPS setup credentials and state are stored separately.\n"
    + include_line
    + "\n\n"
)

header.write_text(header_text, encoding="utf-8", newline="\n")
sketch.write_text(
    text[:start] + replacement + text[end + len("#endif"):],
    encoding="utf-8",
    newline="\n",
)

print("Created:", header)
print("Updated:", sketch)
