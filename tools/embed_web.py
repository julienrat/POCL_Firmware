#!/usr/bin/env python3
"""Compresse web/index.html (gzip) dans src/web_index.h, avec les images de web/ intégrées.

- PlatformIO l'exécute automatiquement avant chaque compilation
  (extra_scripts = pre:tools/embed_web.py).
- Avec l'IDE Arduino, lancez-le à la main après avoir modifié la page :
      python3 tools/embed_web.py
"""
import base64
import gzip
import os
import re

try:
    Import("env")  # noqa: F821  (fourni par PlatformIO)
    ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
except NameError:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

src = os.path.join(ROOT, "web", "index.html")
dst = os.path.join(ROOT, "src", "web_index.h")

with open(src, "rb") as f:
    raw = f.read()


# Les images de web/ citées dans la page (url(logo.png)) y sont intégrées : une seule page à servir
def integrer(m):
    nom = m.group(1).decode()
    with open(os.path.join(ROOT, "web", nom), "rb") as f:
        donnees = base64.b64encode(f.read())
    return b"url(data:image/" + nom.rsplit(".", 1)[1].encode() + b";base64," + donnees + b")"


raw = re.sub(rb"url\(([A-Za-z0-9_-]+\.(?:png|jpg|gif|webp))\)", integrer, raw)
gz = gzip.compress(raw, compresslevel=9, mtime=0)

lines = []
for i in range(0, len(gz), 20):
    lines.append("  " + ",".join("0x%02x" % b for b in gz[i:i + 20]) + ",")
out = (
    "// Fichier généré par tools/embed_web.py à partir de web/index.html — ne pas modifier.\n"
    "#pragma once\n#include <Arduino.h>\n"
    "static const size_t INDEX_HTML_GZ_LEN = %d;\n"
    "static const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n%s\n};\n" % (len(gz), "\n".join(lines))
)
old = open(dst).read() if os.path.exists(dst) else ""
if old != out:
    with open(dst, "w") as f:
        f.write(out)
print("Page web : %d octets -> %d octets compressés" % (len(raw), len(gz)))
