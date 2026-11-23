#!/usr/bin/env bash
# =====================================================================
#  build_pages.sh — assemble la page de flash en ligne (GitHub Pages)
#
#  Copie docs/ (page + captures), le logo et les binaires de bin/ dans
#  un dossier, et écrit :
#    manifest.json          pour ESP Web Tools (une image complète par puce, à 0x0)
#    firmware/infos.json    version et taille des fichiers, affichées par la page
#
#  Utilisé par .github/workflows/pages.yml à chaque envoi sur main.
#  Pour essayer chez soi :
#    tools/build_pages.sh /tmp/site && python3 -m http.server -d /tmp/site 8000
#  (Web Serial accepte http://localhost : ouvrir http://localhost:8000 dans Chrome)
# =====================================================================
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$DIR/_site}"

rm -rf "$OUT"
mkdir -p "$OUT/firmware"
cp -r "$DIR/docs/." "$OUT/"
cp "$DIR/web/logo.png" "$OUT/logo.png"

n=0
for f in pocl_esp8266.bin pocl_esp32_usb.bin pocl_esp32_ota.bin pocl_esp32c3_usb.bin pocl_esp32c3_ota.bin \
         pocl_esp32s2_usb.bin pocl_esp32s2_ota.bin; do
  if [ -f "$DIR/bin/$f" ]; then cp "$DIR/bin/$f" "$OUT/firmware/"; n=$((n + 1)); fi
done
[ "$n" -gt 0 ] || { echo "✖ aucun binaire dans bin/ : lance d'abord ./flash.sh --compile" >&2; exit 1; }

# Version : date du dernier changement des binaires et commit (sinon : aujourd'hui)
date="$(git -C "$DIR" log -1 --format=%cs -- bin 2>/dev/null || true)"
commit="$(git -C "$DIR" log -1 --format=%h -- bin 2>/dev/null || true)"
[ -n "$date" ] || date="$(date +%F)"

python3 - "$OUT" "$date" "$commit" <<'EOF'
import json, os, sys
out, date, commit = sys.argv[1:4]
fw = os.path.join(out, "firmware")
version = date + (" · " + commit if commit else "")
# chipFamily d'ESP Web Tools -> image complète (chargeur + partitions + programme) écrite à 0x0
images = [("ESP8266", "pocl_esp8266.bin"), ("ESP32", "pocl_esp32_usb.bin"),
          ("ESP32-C3", "pocl_esp32c3_usb.bin"), ("ESP32-S2", "pocl_esp32s2_usb.bin")]
builds = []
for chip, f in images:
    p = os.path.join(fw, f)
    if not os.path.isfile(p):
        continue
    # contrôle : en-tête du chargeur (0xE9) au bon endroit et bon type de puce
    where, chip_id = {"ESP8266": (0, None), "ESP32": (0x1000, 0), "ESP32-C3": (0, 5), "ESP32-S2": (0x1000, 2)}[chip]
    with open(p, "rb") as h:
        d = h.read(0x1010)
    if len(d) <= where + 12 or d[where] != 0xE9 or (chip_id is not None and d[where + 12] != chip_id):
        sys.exit("✖ %s n'est pas une image complète pour %s (à écrire à 0x0)" % (f, chip))
    builds.append({"chipFamily": chip, "parts": [{"path": "firmware/" + f, "offset": 0}]})
manifest = {
    "name": "POCL",
    "version": version,
    "new_install_prompt_erase": True,     # la case « Erase device » est proposée, décochée : la configuration est gardée
    "new_install_improv_wait_time": 0,    # POCL ne parle pas Improv : inutile d'attendre après le flash
    "builds": builds,
}
with open(os.path.join(out, "manifest.json"), "w") as h:
    json.dump(manifest, h, ensure_ascii=False, indent=2)
files = {f: os.path.getsize(os.path.join(fw, f)) for f in sorted(os.listdir(fw)) if f.endswith(".bin")}
with open(os.path.join(fw, "infos.json"), "w") as h:
    json.dump({"version": version, "date": date, "commit": commit, "files": files}, h, ensure_ascii=False, indent=2)
print("✔ site prêt : %s (version %s, %d cartes)" % (out, version, len(builds)))
EOF
