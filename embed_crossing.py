# Turn the MP3 clips into src/crossing_mp3.h before each build.
# The header is generated, so it stays out of git.

Import("env")

from pathlib import Path

project = Path(env["PROJECT_DIR"])
header = project / "src" / "crossing_mp3.h"
clips = [
    ("audio/crossing.mp3", "kCrossingMp3", "kCrossingMp3Len"),
    ("audio/error.mp3", "kErrorMp3", "kErrorMp3Len"),
    ("audio/octopus.mp3", "kOctopusMp3", "kOctopusMp3Len"),
]

lines = ["#pragma once", "#include <stdint.h>", ""]
for filename, symbol, length_symbol in clips:
    data = (project / filename).read_bytes()
    lines.append("// Generated from %s. Do not edit." % filename)
    lines.append("static const uint8_t %s[] = {" % symbol)
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("  " + ", ".join("0x%02x" % byte for byte in chunk) + ",")
    lines.append("};")
    lines.append("static const unsigned %s = %d;" % (length_symbol, len(data)))
    lines.append("")
text = "\n".join(lines)

if not header.exists() or header.read_text() != text:
    header.write_text(text)
