# Turn audio/crossing.mp3 into src/crossing_mp3.h before each build.
# The header is generated, so it stays out of git.

Import("env")

from pathlib import Path

project = Path(env["PROJECT_DIR"])
source = project / "audio" / "crossing.mp3"
header = project / "src" / "crossing_mp3.h"

data = source.read_bytes()
lines = [
    "#pragma once",
    "#include <stdint.h>",
    "",
    "// Generated from audio/crossing.mp3. Do not edit.",
    "static const uint8_t kCrossingMp3[] = {",
]
for offset in range(0, len(data), 16):
    chunk = data[offset : offset + 16]
    lines.append("  " + ", ".join("0x%02x" % byte for byte in chunk) + ",")
lines.append("};")
lines.append("static const unsigned kCrossingMp3Len = %d;" % len(data))
lines.append("")
text = "\n".join(lines)

if not header.exists() or header.read_text() != text:
    header.write_text(text)
