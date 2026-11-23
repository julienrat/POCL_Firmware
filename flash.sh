#!/usr/bin/env bash
# =====================================================================
#  flash.sh — compile et flashe le firmware POCL
#  Cartes : Wemos D1 mini (ESP8266), ESP32 DevKit (WROOM-32), ESP32-C3 Super Mini, ESP32-S2 Mini (LOLIN)
#
#  Utilisation :
#    ./flash.sh                  détecte le port et la carte, compile, flashe
#    ./flash.sh esp32            force la carte : esp8266, esp32, esp32c3 (Super Mini) ou esp32s2 (S2 Mini)
#    ./flash.sh -p /dev/ttyUSB1  choisit le port série
#    ./flash.sh --efface         efface toute la mémoire avant (WiFi et recette compris)
#    ./flash.sh --moniteur       ouvre le moniteur série après le flash (Ctrl+C pour quitter)
#    ./flash.sh --moniteur-seul  ouvre seulement le moniteur série (sans compiler ni flasher)
#    ./flash.sh --compile        compile seulement (binaires copiés dans bin/)
#    ./flash.sh --bin            flashe le binaire déjà prêt de bin/ sans compiler (rapide)
#    ./flash.sh --croquis outils/trouve_ecran
#    ./flash.sh --croquis outils/teste_servo
#                                flashe un autre croquis (outil) puis ouvre le moniteur série
#    ./flash.sh --aide
#
#  Au premier lancement, le script télécharge arduino-cli, le core de la carte
#  et les bibliothèques dans ~/.cache/pocl-arduino (≈ 200 Mo pour l'ESP8266,
#  ≈ 1 Go pour l'ESP32). Votre installation Arduino habituelle n'est pas modifiée.
#
#  Variables avancées :
#    POCL_HOME=dossier        emplacement des outils (défaut ~/.cache/pocl-arduino)
#    POCL_ARDUINO_DIR=~/Arduino      utiliser une installation Arduino existante
#    POCL_ARDUINO_DATA=~/.arduino15   (cores et bibliothèques déjà présents : rien n'est téléchargé)
#    ARDUINO_CLI=chemin       arduino-cli à utiliser
#    POCL_COMPILE_OPTS="…"    options supplémentaires pour arduino-cli compile
#    POCL_ESPTOOL=chemin      esptool à utiliser pour --bin (sinon : celui d'Arduino, du système,
#                             ou l'esptool officiel d'Espressif installé dans POCL_HOME)
# =====================================================================
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$DIR"
POCL_HOME="${POCL_HOME:-$HOME/.cache/pocl-arduino}"

ESP8266_CORE="esp8266:esp8266@3.1.2"
ESP32_CORE="esp32:esp32@3.3.13"
LIBS=("ArduinoJson@7.2.1" "Adafruit NeoPixel@1.12.3" "U8g2" "AccelStepper@1.64" "PubSubClient@2.8")  # testé avec U8g2 2.36.18
URLS=(
  "https://arduino.esp8266.com/stable/package_esp8266com_index.json"
  "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
)

# --- affichage -------------------------------------------------------
if [ -t 1 ]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; C=$'\e[36m'; N=$'\e[0m'; else B=; G=; Y=; R=; C=; N=; fi
etape() { echo; echo "${B}${C}==> $*${N}"; }
ok()    { echo "${G}✔ $*${N}"; }
alerte(){ echo "${Y}⚠ $*${N}"; }
stop()  { echo "${R}✖ $*${N}" >&2; exit 1; }

aide() { awk '/^# =====/{n++; next} n==1' "$0" | sed 's/^# \{0,2\}//'; exit 0; }

# --- options ---------------------------------------------------------
BOARD="auto"; PORT=""; ERASE=0; MONITOR=0; MODE="flash"; CROQUIS=""
while [ $# -gt 0 ]; do
  case "$1" in
    esp8266|d1mini|wemos) BOARD="esp8266" ;;
    esp32|wroom)          BOARD="esp32" ;;
    esp32c3|c3|supermini|super-mini) BOARD="esp32c3" ;;
    esp32s2|s2|s2mini|s2-mini) BOARD="esp32s2" ;;
    -p|--port)            PORT="${2:-}"; shift ;;
    -e|--efface|--erase)  ERASE=1 ;;
    -m|--moniteur|--monitor) MONITOR=1 ;;
    --moniteur-seul)      MODE="moniteur" ;;
    -c|--compile)         MODE="compile" ;;
    --bin)                MODE="bin" ;;
    --croquis|--sketch)   CROQUIS="${2:-}"; shift ;;
    -h|--aide|--help)     aide ;;
    *) stop "option inconnue : $1 (./flash.sh --aide)" ;;
  esac
  shift
done
NAME="pocl_firmware"
if [ -n "$CROQUIS" ]; then  # autre croquis (outil) : on le flashe et on ouvre le moniteur
  [ -d "$CROQUIS" ] || CROQUIS="$DIR/$CROQUIS"
  [ -d "$CROQUIS" ] || stop "croquis introuvable : $CROQUIS"
  SKETCH="$(cd "$CROQUIS" && pwd)"; NAME="$(basename "$SKETCH")"
  [ -f "$SKETCH/$NAME.ino" ] || stop "il manque $SKETCH/$NAME.ino"
  [ "$MODE" = "bin" ] && stop "--bin ne s'utilise pas avec --croquis"
  [ "$MODE" = "flash" ] && MONITOR=1
fi
[ "$MODE" = "compile" ] && [ "$BOARD" = "auto" ] && BOARD="toutes"

# --- arduino-cli -----------------------------------------------------
ACLI="${ARDUINO_CLI:-}"
trouver_acli() {
  [ -n "$ACLI" ] && [ -x "$ACLI" ] && return 0
  if command -v arduino-cli >/dev/null 2>&1; then ACLI="$(command -v arduino-cli)"; return 0; fi
  if [ -x "$POCL_HOME/bin/arduino-cli" ]; then ACLI="$POCL_HOME/bin/arduino-cli"; return 0; fi
  return 1
}
installer_acli() {
  trouver_acli && return 0
  etape "Installation d'arduino-cli dans $POCL_HOME/bin"
  command -v curl >/dev/null 2>&1 || stop "curl est nécessaire (sudo apt install curl)"
  mkdir -p "$POCL_HOME/bin"
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR="$POCL_HOME/bin" sh >/dev/null
  ACLI="$POCL_HOME/bin/arduino-cli"
  [ -x "$ACLI" ] || stop "installation d'arduino-cli impossible"
  ok "arduino-cli $("$ACLI" version | awk '{print $3}')"
}

# Configuration isolée : on ne touche pas aux cartes/bibliothèques de l'utilisateur
CONFIG="$POCL_HOME/arduino-cli.yaml"
preparer_config() {
  mkdir -p "$POCL_HOME"
  local data="$POCL_HOME/data" user="$POCL_HOME/user"
  if [ -n "${POCL_ARDUINO_DIR:-}" ]; then user="$POCL_ARDUINO_DIR"; fi
  {
    echo "board_manager:"
    echo "  additional_urls:"
    for u in "${URLS[@]}"; do echo "    - $u"; done
    echo "directories:"
    echo "  data: ${POCL_ARDUINO_DATA:-$data}"
    echo "  downloads: $POCL_HOME/staging"
    echo "  user: $user"
    echo "updater:"
    echo "  enable_notification: false"
  } > "$CONFIG"
}
acli() { "$ACLI" --config-file "$CONFIG" "$@"; }

installer_core() {  # $1 = esp8266 | esp32 | esp32c3 | esp32s2
  [ -n "${POCL_ARDUINO_DIR:-}" ] && return 0
  local spec id ver installed
  if [ "$1" = "esp8266" ]; then spec="$ESP8266_CORE"; else spec="$ESP32_CORE"; fi
  id="${spec%@*}"; ver="${spec#*@}"
  installed="$(acli core list 2>/dev/null | awk -v id="$id" '$1==id {print $2}')"
  if [ "$installed" = "$ver" ]; then ok "core $id $ver déjà installé"; return 0; fi
  etape "Installation du core $id $ver (une seule fois, patience…)"
  acli core update-index
  acli core install "$spec"
  ok "core $id $ver installé"
}

installer_libs() {
  [ -n "${POCL_ARDUINO_DIR:-}" ] && return 0
  local manque=0 l nom
  local liste; liste="$(acli lib list 2>/dev/null || true)"
  for l in "${LIBS[@]}"; do
    nom="${l%@*}"
    if [ "$nom" = "$l" ]; then echo "$liste" | grep -q "^${nom} " || manque=1
    else echo "$liste" | grep -q "^${nom} *${l#*@} " || manque=1; fi
  done
  if [ $manque -eq 0 ]; then ok "bibliothèques déjà installées"; return 0; fi
  etape "Installation des bibliothèques"
  acli lib update-index
  acli lib install "${LIBS[@]}"
  ok "bibliothèques installées"
}

# --- port série ------------------------------------------------------
lister_ports() {
  PORTS=()
  local p
  for p in /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem*; do
    [ -e "$p" ] && PORTS+=("$p")
  done
  return 0
}

# Ce que Linux voit sur l'USB (sans être root) : aide à comprendre pourquoi il n'y a pas de port
diagnostic_usb() {
  [ -d /sys/bus/usb/devices ] || return 0
  local d v p vus=""
  for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] || continue
    v="$(cat "$d/idVendor")"; p="$(cat "$d/idProduct" 2>/dev/null)"
    case "$v:$p" in
      303a:*) vus="$vus esp" ;;   # Espressif : USB natif (ESP32-C3, S3…)
      1a86:*) vus="$vus ch340" ;;
      10c4:*) vus="$vus cp210x" ;;
      0403:*) vus="$vus ftdi" ;;
    esac
  done
  echo
  case "$vus" in
    *esp*)
      alerte "Une carte Espressif à USB natif (ESP32-C3…) est branchée, mais Linux n'a pas créé /dev/ttyACM0."
      echo "   → charge le pilote : ${B}sudo modprobe cdc_acm${N} puis relance" ;;
    *ch340*|*cp210x*|*ftdi*)
      alerte "Un adaptateur USB-série (${vus# }) est branché, mais aucun /dev/ttyUSB n'existe."
      echo "   → sous Ubuntu, brltty « vole » souvent le port : ${B}sudo apt remove brltty${N}, puis débranche et rebranche"
      echo "   → sinon : ${B}sudo modprobe ch341${N} (ou cp210x), puis rebranche" ;;
    *)
      alerte "Linux ne voit aucune carte sur l'USB :"
      echo "   - le câble ne transmet peut-être que le courant (très courant en USB-C) : essaie un autre câble ;"
      echo "   - essaie une autre prise USB de l'ordinateur (pas de hub) ;"
      echo "   - ESP32-C3 Super Mini : force le mode téléchargement : ${B}maintiens BOOT, appuie sur RESET${N}"
      echo "     (ou branche le câble en maintenant BOOT), puis relâche BOOT : /dev/ttyACM0 doit apparaître ;"
      echo "   - ESP32-S2 Mini : même chose avec les boutons ${B}0${N} et ${B}RST${N}." ;;
  esac
}

choisir_port() {
  [ -n "$PORT" ] && { [ -e "$PORT" ] || stop "le port $PORT n'existe pas"; return 0; }
  lister_ports
  if [ ${#PORTS[@]} -eq 0 ]; then
    diagnostic_usb
    if [ -t 0 ]; then  # dans un terminal : on attend que la carte apparaisse (30 s)
      echo
      echo "   ${B}J'attends la carte pendant 30 s${N} : branche-la (ou, pour une Super Mini ou une S2 Mini, BOOT/0 + RESET)… (Ctrl+C pour arrêter)"
      local t
      for t in $(seq 60); do
        sleep 0.5; lister_ports
        [ ${#PORTS[@]} -gt 0 ] && break
        [ $((t % 2)) -eq 0 ] && printf "\r   %2d s " $((30 - t / 2))
      done
      echo
    fi
    if [ ${#PORTS[@]} -eq 0 ]; then
      echo "   - le pilote CH340 / CP2102 (Windows, macOS) ;"
      echo "   - vérifie avec : ${B}ls /dev/ttyACM* /dev/ttyUSB*${N} et ${B}lsusb${N} juste après avoir branché la carte."
      stop "pas de port série"
    fi
    sleep 1  # laisse le temps au système de régler les droits du nouveau port
  fi
  if [ ${#PORTS[@]} -eq 1 ]; then
    PORT="${PORTS[0]}"
  else
    echo "Plusieurs ports série :"
    local i=1 p; for p in "${PORTS[@]}"; do echo "   $i) $p"; i=$((i + 1)); done
    read -rp "Numéro du port : " i
    [[ "$i" =~ ^[0-9]+$ ]] && [ "$i" -ge 1 ] && [ "$i" -le ${#PORTS[@]} ] || stop "choix invalide"
    PORT="${PORTS[$((i - 1))]}"
  fi
  ok "port $PORT"
  if [ "$(uname)" = "Linux" ] && [ ! -w "$PORT" ]; then
    alerte "Pas le droit d'écrire sur $PORT. Une fois pour toutes :"
    echo "   sudo usermod -aG dialout $USER     (puis fermer la session et se reconnecter)"
    stop "accès au port refusé"
  fi
}

# --- esptool (détection de la puce, flash du binaire tout prêt) -------
# L'esptool des paquets Debian / Ubuntu (« 4.7.0+dfsg ») n'a pas ses « stubs », les petits
# programmes de flash qu'esptool envoie à la puce : il plante (stub_flasher_….json introuvable).
# On préfère donc celui d'Arduino, ou l'esptool officiel d'Espressif installé dans POCL_HOME ;
# à défaut, le flash passe par le chargeur intégré à la puce (--no-stub), plus lent.
ESPTOOL=()
NOSTUB=()
ESPTOOL_VER="v4.12.0"

# 0 si l'esptool en Python lancé par « $@ » n'a pas ses stubs (paquet Debian / Ubuntu)
sans_stubs() {
  local dir
  dir="$("$@" -c 'import esptool, os; print(os.path.dirname(esptool.__file__))' 2>/dev/null)" || return 1
  [ -n "$dir" ] && [ -d "$dir" ] || return 1
  [ -z "$(find "$dir" -path '*stub_flasher*' -name '*.json' 2>/dev/null | head -1)" ]
}
# 0 si la commande esptool $1 (dans le PATH) n'a pas ses stubs ; un binaire autonome les contient
commande_sans_stubs() {
  local c py; c="$(command -v "$1")" || return 1
  [ "$(head -c 2 "$c" 2>/dev/null)" = "#!" ] || return 1
  read -ra py <<< "$(head -1 "$c" | sed 's/^#! *//')"
  sans_stubs "${py[@]}"
}

# esptool officiel déjà installé dans POCL_HOME (et qui fonctionne)
esptool_pocl() {
  local e
  [ -d "$POCL_HOME/esptool" ] || return 1
  e="$(find "$POCL_HOME/esptool" -type f \( -name esptool -o -name esptool.py \) 2>/dev/null | head -1)"
  [ -n "$e" ] || return 1
  chmod +x "$e" 2>/dev/null || true
  "$e" version >/dev/null 2>&1 || return 1
  echo "$e"
}

# Télécharge l'esptool officiel d'Espressif (binaire autonome, stubs compris)
installer_esptool() {
  local arch tmp
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) arch="linux-amd64" ;;
    Linux-aarch64|Linux-arm64) arch="linux-aarch64" ;;
    Linux-armv7l|Linux-armv6l) arch="linux-armv7" ;;
    Darwin-x86_64) arch="macos-amd64" ;;
    Darwin-arm64) arch="macos-arm64" ;;
    *) return 1 ;;
  esac
  command -v curl >/dev/null 2>&1 || return 1
  etape "Installation de l'esptool officiel d'Espressif ($ESPTOOL_VER) dans $POCL_HOME/esptool"
  tmp="$(mktemp -d)"
  if curl -fsSL "${POCL_ESPTOOL_URL:-https://github.com/espressif/esptool/releases/download/$ESPTOOL_VER/esptool-$ESPTOOL_VER-$arch.tar.gz}" -o "$tmp/esptool.tar.gz" \
     && mkdir -p "$tmp/x" && tar xzf "$tmp/esptool.tar.gz" -C "$tmp/x"; then
    mkdir -p "$POCL_HOME"; rm -rf "$POCL_HOME/esptool"; mv "$tmp/x" "$POCL_HOME/esptool"; rm -rf "$tmp"
    esptool_pocl >/dev/null && { ok "esptool $ESPTOOL_VER installé"; return 0; }
  fi
  rm -rf "$tmp"
  alerte "Téléchargement de l'esptool officiel impossible."
  return 1
}

trouver_esptool() {
  [ ${#ESPTOOL[@]} -gt 0 ] && return 0
  local data e d c incomplet=()
  if [ -n "${POCL_ESPTOOL:-}" ]; then ESPTOOL=("$POCL_ESPTOOL"); return 0; fi
  # 1) l'esptool officiel installé par ce script
  if e="$(esptool_pocl)"; then ESPTOOL=("$e"); return 0; fi
  # 2) celui du paquet ESP32 d'Arduino (complet)
  for data in "${POCL_ARDUINO_DATA:-$POCL_HOME/data}" "$HOME/.arduino15" "$HOME/Library/Arduino15"; do
    for e in "$data"/packages/esp32/tools/esptool_py/*/esptool; do
      [ -x "$e" ] && { ESPTOOL=("$e"); return 0; }
    done
  done
  # 3) celui du système, s'il a ses stubs
  for c in esptool esptool.py; do
    command -v "$c" >/dev/null 2>&1 || continue
    if commande_sans_stubs "$c"; then [ ${#incomplet[@]} -eq 0 ] && incomplet=("$c")
    else ESPTOOL=("$c"); return 0; fi
  done
  if command -v python3 >/dev/null 2>&1 && python3 -c "import esptool" 2>/dev/null; then
    if sans_stubs python3; then [ ${#incomplet[@]} -eq 0 ] && incomplet=(python3 -m esptool)
    else ESPTOOL=(python3 -m esptool); return 0; fi
  fi
  # 4) sinon, l'esptool officiel d'Espressif
  if installer_esptool && e="$(esptool_pocl)"; then ESPTOOL=("$e"); return 0; fi
  # 5) en dernier recours, l'esptool incomplet, avec le chargeur intégré à la puce
  if [ ${#incomplet[@]} -gt 0 ]; then
    ESPTOOL=("${incomplet[@]}"); NOSTUB=(--no-stub)
    alerte "esptool incomplet (paquet Debian / Ubuntu sans « stubs ») : flash plus lent, avec --no-stub."
    echo "   Pour un esptool complet : sudo apt install pipx && pipx install esptool"
    return 0
  fi
  # 6) le vieil esptool.py du paquet ESP8266 d'Arduino
  for data in "${POCL_ARDUINO_DATA:-$POCL_HOME/data}" "$HOME/.arduino15" "$HOME/Library/Arduino15"; do
    for d in "$data"/packages/esp8266/hardware/esp8266/*/tools; do
      [ -f "$d/esptool/esptool.py" ] && command -v python3 >/dev/null 2>&1 && {
        ESPTOOL=(env "PYTHONPATH=$d/pyserial:$d/esptool" python3 "$d/esptool/esptool.py"); return 0; }
    done
  done
  return 1
}
# esptool v5 écrit les commandes avec des tirets (write-flash), les versions précédentes avec _
cmd_esptool() { if "${ESPTOOL[@]}" version 2>/dev/null | grep -q "v5"; then echo "${1//_/-}"; else echo "$1"; fi; }

# Identifiant USB « vendeur:produit » d'un port série (Linux seulement), ex. 303a:0002
usb_id() {
  local d; d="$(readlink -f "/sys/class/tty/$(basename "$1")/device" 2>/dev/null)" || return 0
  d="$(dirname "$d")"
  [ -f "$d/idVendor" ] && echo "$(cat "$d/idVendor"):$(cat "$d/idProduct")"
  return 0
}

detecter_carte() {
  [ "$BOARD" != "auto" ] && return 0
  etape "Détection de la carte"
  # ESP32-S2 : USB natif géré par le programme ; esptool la ferait redémarrer, on la reconnaît à son identifiant USB
  case "$(usb_id "$PORT")" in
    303a:0002|303a:80c1|303a:80c2|303a:80c3) BOARD="esp32s2"; ok "carte : esp32s2 (S2 Mini)"; return 0 ;;
  esac
  if trouver_esptool; then
    local sortie
    sortie="$("${ESPTOOL[@]}" ${NOSTUB[@]+"${NOSTUB[@]}"} --port "$PORT" --baud 115200 "$(cmd_esptool chip_id)" 2>&1 || true)"
    case "$sortie" in
      *ESP8266*) BOARD="esp8266" ;;
      *ESP32-C3*) BOARD="esp32c3" ;;
      *ESP32-S2*) BOARD="esp32s2" ;;
      *ESP32-S*|*ESP32-C*|*ESP32-H*|*ESP32-P*)
        stop "carte $(echo "$sortie" | grep -oE 'ESP32-[A-Z][0-9]+' | head -1) détectée : cartes prévues = ESP8266, ESP32 (WROOM-32), ESP32-C3 et ESP32-S2" ;;
      *ESP32*) BOARD="esp32" ;;
    esac
    MAC="$(echo "$sortie" | grep -oiE 'MAC:[[:space:]]*([0-9a-f]{2}:){5}[0-9a-f]{2}' | head -1 | grep -oiE '([0-9a-f]{2}:){5}[0-9a-f]{2}' || true)"
  fi
  if [ "$BOARD" = "auto" ]; then
    alerte "Détection automatique impossible (esptool absent ou carte muette)."
    echo "   1) Wemos D1 mini (ESP8266)"
    echo "   2) ESP32 DevKit (WROOM-32)"
    echo "   3) ESP32-C3 Super Mini"
    echo "   4) ESP32-S2 Mini (LOLIN / Wemos)"
    local r; read -rp "Quelle carte ? [1/2/3/4] : " r
    case "$r" in 1) BOARD="esp8266" ;; 2) BOARD="esp32" ;; 3) BOARD="esp32c3" ;; 4) BOARD="esp32s2" ;; *) stop "choix invalide" ;; esac
  fi
  ok "carte : $BOARD"
}

fqbn() {  # $1 = carte
  local f
  if [ "$1" = "esp32" ]; then
    f="esp32:esp32:esp32:PartitionScheme=min_spiffs,UploadSpeed=921600"
    if [ $ERASE -eq 1 ]; then f="$f,EraseFlash=all"; fi
  elif [ "$1" = "esp32c3" ]; then  # USB natif : le moniteur série passe par le câble USB-C (CDC)
    f="esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc,UploadSpeed=921600"
    if [ $ERASE -eq 1 ]; then f="$f,EraseFlash=all"; fi
  elif [ "$1" = "esp32s2" ]; then  # S2 Mini : USB natif (TinyUSB), PSRAM de 2 Mo
    f="esp32:esp32:esp32s2:CDCOnBoot=cdc,UploadMode=cdc,PSRAM=enabled,PartitionScheme=min_spiffs"
    if [ $ERASE -eq 1 ]; then f="$f,EraseFlash=all"; fi
  else
    f="esp8266:esp8266:d1_mini:eesz=4M1M,baud=921600"
    if [ $ERASE -eq 1 ]; then f="$f,wipe=all"; fi
  fi
  echo "$f"
}

# --- compilation -----------------------------------------------------
builddir() { if [ "$NAME" = "pocl_firmware" ]; then echo "$DIR/build/$1"; else echo "$DIR/build/$1-$NAME"; fi; }

compiler() {  # $1 = carte
  local b="$1" out; out="$(builddir "$1")"
  etape "Compilation de $NAME pour $b"
  if [ "$NAME" = "pocl_firmware" ]; then
    if command -v python3 >/dev/null 2>&1; then python3 "$DIR/tools/embed_web.py"
    else alerte "python3 absent : la page web déjà intégrée (src/web_index.h) est utilisée"; fi
  fi
  mkdir -p "$out" "$DIR/bin"
  rm -f "$out/$NAME.ino.bin"
  local extra=(); read -ra extra <<< "${POCL_COMPILE_OPTS:-}"
  acli compile --fqbn "$(fqbn "$b")" --warnings none --output-dir "$out" ${extra[@]+"${extra[@]}"} "$SKETCH" \
    || stop "la compilation a échoué"
  [ -f "$out/$NAME.ino.bin" ] || stop "la compilation n'a produit aucun binaire"
  if [ "$NAME" != "pocl_firmware" ]; then ok "$NAME compilé"; return 0; fi
  if [ "$b" != "esp8266" ]; then  # esp32 ou esp32c3 : image complète (0x0) + image de mise à jour
    cp "$out/pocl_firmware.ino.bin" "$DIR/bin/pocl_${b}_ota.bin"
    if command -v python3 >/dev/null 2>&1; then  # on retire le remplissage final (0xFF)
      python3 -c "import sys;d=open(sys.argv[1],'rb').read().rstrip(b'\xff');open(sys.argv[2],'wb').write(d)" \
        "$out/pocl_firmware.ino.merged.bin" "$DIR/bin/pocl_${b}_usb.bin"
    else
      cp "$out/pocl_firmware.ino.merged.bin" "$DIR/bin/pocl_${b}_usb.bin"
    fi
    ok "bin/pocl_${b}_usb.bin (flash USB à l'adresse 0x0) et bin/pocl_${b}_ota.bin (mise à jour par la page web)"
  else
    cp "$out/pocl_firmware.ino.bin" "$DIR/bin/pocl_esp8266.bin"
    ok "bin/pocl_esp8266.bin (flash USB à l'adresse 0x0 et mise à jour par la page web)"
  fi
}

televerser() {  # $1 = carte
  local b="$1" log; log="$(mktemp)"
  etape "Flash de la carte $b sur $PORT"
  set +e
  acli upload -p "$PORT" --fqbn "$(fqbn "$b")" --input-dir "$(builddir "$b")" "$SKETCH" 2>&1 | tee "$log"
  local rc=${PIPESTATUS[0]}
  set -e
  [ -z "${MAC:-}" ] && MAC="$(grep -oiE 'MAC:[[:space:]]*([0-9a-f]{2}:){5}[0-9a-f]{2}' "$log" | head -1 | grep -oiE '([0-9a-f]{2}:){5}[0-9a-f]{2}' || true)"
  rm -f "$log"
  if [ $rc -ne 0 ]; then
    [ "$b" = "esp32c3" ] && stop "le flash a échoué : maintiens le bouton BOOT, appuie sur RESET (ou rebranche l'USB) puis relâche BOOT, et relance ; ou essaie un autre câble"
    [ "$b" = "esp32s2" ] && stop "le flash a échoué : maintiens le bouton 0, appuie sur RST puis relâche 0, et relance ; ou essaie un autre câble"
    stop "le flash a échoué : maintiens le bouton BOOT de la carte au début du flash (ESP32), ou essaie un autre câble"
  fi
}

flasher_bin() {  # $1 = carte
  local b="$1" f
  trouver_esptool || stop "esptool est nécessaire pour --bin : sudo apt install pipx && pipx install esptool (ou lance une fois ./flash.sh sans --bin)"
  if [ "$b" = "esp8266" ]; then f="$DIR/bin/pocl_esp8266.bin"; else f="$DIR/bin/pocl_${b}_usb.bin"; fi
  [ -f "$f" ] || stop "$f introuvable : lance d'abord ./flash.sh --compile"
  etape "Flash de $(basename "$f") sur $PORT"
  local chip=(--chip "$b") log avant=(); log="$(mktemp)"
  if [ "$b" = "esp32s2" ]; then mode_telechargement_s2; avant=(--before "$(cmd_esptool no_reset)"); fi
  if [ $ERASE -eq 1 ]; then
    if [ ${#NOSTUB[@]} -gt 0 ]; then alerte "effacement complet impossible sans les stubs d'esptool : la mémoire n'est pas effacée"; ERASE=0
    else "${ESPTOOL[@]}" "${chip[@]}" ${avant[@]+"${avant[@]}"} --port "$PORT" "$(cmd_esptool erase_flash)"; fi
  fi
  local rc essai
  for essai in 1 2 3; do
    set +e
    "${ESPTOOL[@]}" "${chip[@]}" ${avant[@]+"${avant[@]}"} ${NOSTUB[@]+"${NOSTUB[@]}"} --port "$PORT" --baud 460800 \
      "$(cmd_esptool write_flash)" 0x0 "$f" 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    set -e
    # esptool sans ses stubs (paquet Debian / Ubuntu) : la puce attend toujours, on recommence avec son chargeur intégré
    if [ $rc -ne 0 ] && [ ${#NOSTUB[@]} -eq 0 ] && grep -q "stub_flasher" "$log"; then
      alerte "Cet esptool n'a pas ses « stubs » (paquet Debian / Ubuntu)."
      local e
      if [ "$essai" = 1 ] && installer_esptool && e="$(esptool_pocl)"; then ESPTOOL=("$e"); continue; fi
      alerte "Nouvel essai avec le chargeur intégré à la puce (--no-stub, plus lent)…"
      echo "   Pour un esptool complet : sudo apt install pipx && pipx install esptool"
      NOSTUB=(--no-stub)
      continue
    fi
    break
  done
  # Puce mise en mode téléchargement avec le bouton 0 / BOOT (USB natif) : esptool ne peut pas la redémarrer
  # et, selon sa version, le signale comme une erreur alors que l'écriture a réussi
  local rst_manuel=0
  if grep -qi "download mode using GPIO0" "$log" && grep -q "Wrote " "$log"; then rc=0; rst_manuel=1; fi
  [ -z "${MAC:-}" ] && MAC="$(grep -oiE 'MAC:[[:space:]]*([0-9a-f]{2}:){5}[0-9a-f]{2}' "$log" | head -1 | grep -oiE '([0-9a-f]{2}:){5}[0-9a-f]{2}' || true)"
  rm -f "$log"
  if [ $rc -ne 0 ]; then
    [ "$b" = "esp32s2" ] && stop "le flash a échoué : maintiens le bouton 0, appuie sur RST, relâche 0, puis relance ./flash.sh --bin"
    [ "$b" = "esp32c3" ] && stop "le flash a échoué : maintiens BOOT, appuie sur RESET, relâche BOOT, puis relance ./flash.sh --bin"
    stop "le flash a échoué"
  fi
  if [ $rst_manuel -eq 1 ]; then
    ok "flash réussi"
    alerte "La carte a été mise en mode téléchargement avec son bouton : esptool ne peut pas la redémarrer."
    if [ -t 0 ]; then
      read -rp "   Appuie sur le bouton RST de la carte, puis sur Entrée… " _
      sleep 2; lister_ports; [ ${#PORTS[@]} -eq 1 ] && PORT="${PORTS[0]}"
    else
      echo "   Appuie sur son bouton RST pour démarrer POCL."
    fi
  elif [ "$b" = "esp32s2" ]; then
    alerte "Si la S2 Mini ne redémarre pas toute seule : appuie sur son bouton RST."
  fi
  return 0
}

# ESP32-S2 : le programme POCL (USB natif) redémarre en mode téléchargement quand on ouvre
# son port à 1200 bauds, comme l'IDE Arduino. Le port disparaît puis revient.
mode_telechargement_s2() {
  echo "   passage en mode téléchargement…"
  if [ "$(uname)" = "Darwin" ]; then stty -f "$PORT" 1200 2>/dev/null || true; else stty -F "$PORT" 1200 2>/dev/null || true; fi
  sleep 1.5
  local t
  for t in $(seq 20); do
    lister_ports
    if [ ${#PORTS[@]} -gt 0 ]; then PORT="${PORTS[0]}"; sleep 0.5; return 0; fi
    sleep 0.5
  done
  alerte "La S2 Mini n'est pas revenue : maintiens le bouton 0, appuie sur RST, relâche 0 (mode téléchargement)."
  for t in $(seq 40); do
    lister_ports
    if [ ${#PORTS[@]} -gt 0 ]; then PORT="${PORTS[0]}"; sleep 0.5; return 0; fi
    sleep 0.5
  done
  stop "pas de port série après le passage en mode téléchargement"
}

# Moniteur série minimal : contrairement à « arduino-cli monitor », il ne bascule pas
# DTR/RTS à l'ouverture, donc il ne redémarre pas la carte (et ne la bloque pas en
# mode « attente de flash »). Ce qu'on tape est envoyé à la carte après Entrée.
moniteur() {
  etape "Moniteur série $PORT à 115200 bauds — tape tes réponses puis Entrée, Ctrl+C pour quitter"
  echo "   (si rien ne s'affiche après 5 s : débranche et rebranche le câble USB)"
  exec 3<>"$PORT" || stop "impossible d'ouvrir $PORT"
  if ! stty 115200 cs8 -cstopb -parenb raw -echo -hupcl clocal <&3 2>/dev/null; then
    exec 3>&-
    alerte "réglage du port impossible avec stty : on passe par arduino-cli"
    installer_acli; preparer_config
    acli monitor -p "$PORT" -c baudrate=115200,dtr=off,rts=off
    return
  fi
  cat <&3 &
  local lecteur=$!
  trap 'kill $lecteur 2>/dev/null; echo; exit 0' INT TERM
  local ligne
  while IFS= read -r ligne; do printf '%s\n' "$ligne" >&3; done
  kill $lecteur 2>/dev/null || true
  exec 3>&-
}

conclusion() {
  echo
  ok "${B}Terminé !${N}"
  if [ "$NAME" != "pocl_firmware" ]; then
    echo "  $NAME est sur la carte. Pour remettre le firmware POCL ensuite : ${B}./flash.sh${N}"
    echo "  (la configuration POCL, WiFi et recette, est conservée)"
    return 0
  fi
  local nom="POCL-XXXX"
  if [ -n "${MAC:-}" ]; then nom="POCL-$(echo "$MAC" | tr -d ':' | tail -c 5 | tr 'a-f' 'A-F')"; fi
  [ $ERASE -eq 1 ] || alerte "La configuration déjà présente sur la carte (WiFi, recette) est conservée."
  echo
  echo "  1. Connecte ton téléphone ou ton ordinateur au WiFi ${B}$nom${N} (mot de passe ${B}pocl1234${N})"
  echo "  2. La page de configuration s'ouvre ; sinon va sur ${B}http://192.168.4.1${N}"
  echo "  3. Réglages › WiFi pour connecter l'objet à Internet"
}

# =====================================================================
# arduino-cli ne sert pas pour flasher un binaire tout prêt ni pour le moniteur série
if [ "$MODE" != "bin" ] && [ "$MODE" != "moniteur" ]; then installer_acli; preparer_config; fi

if [ "$MODE" = "compile" ]; then
  if [ "$BOARD" = "toutes" ]; then cartes=(esp8266 esp32 esp32c3 esp32s2); else cartes=("$BOARD"); fi
  for c in "${cartes[@]}"; do installer_core "$c"; done
  installer_libs
  for c in "${cartes[@]}"; do compiler "$c"; done
  exit 0
fi

choisir_port
if [ "$MODE" = "moniteur" ]; then moniteur; exit 0; fi
if [ "$MODE" = "bin" ]; then
  detecter_carte
  flasher_bin "$BOARD"
else
  detecter_carte
  installer_core "$BOARD"
  installer_libs
  compiler "$BOARD"
  televerser "$BOARD"
fi
conclusion
[ $MONITOR -eq 1 ] && moniteur
exit 0
