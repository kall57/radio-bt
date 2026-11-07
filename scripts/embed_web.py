"""
Compresse web/index.html (gzip) et l'écrit dans src/web_index.h sous forme de
tableau d'octets. Lancé automatiquement par PlatformIO avant la compilation
(extra_scripts = pre:...), ou à la main : python scripts/embed_web.py
"""
import gzip
import os

try:
    Import("env")  # noqa: F821  (fourni par PlatformIO)
    ROOT = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = os.path.join(ROOT, "web", "index.html")
DST = os.path.join(ROOT, "src", "web_index.h")


def build():
    with open(SRC, "rb") as f:
        raw = f.read()
    data = gzip.compress(raw, compresslevel=9, mtime=0)

    lines = []
    for i in range(0, len(data), 20):
        lines.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 20]) + ",")
    header = (
        "// Fichier généré par scripts/embed_web.py — ne pas modifier.\n"
        "// Source : web/index.html (%d octets, %d octets compressés)\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        "const size_t INDEX_HTML_GZ_LEN = %d;\n"
        "const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n%s\n};\n"
        % (len(raw), len(data), len(data), "\n".join(lines))
    )

    old = None
    if os.path.exists(DST):
        with open(DST, "r", encoding="utf-8") as f:
            old = f.read()
    if old != header:
        with open(DST, "w", encoding="utf-8") as f:
            f.write(header)
        print("embed_web: web/index.html -> src/web_index.h (%d -> %d octets)" % (len(raw), len(data)))


build()
