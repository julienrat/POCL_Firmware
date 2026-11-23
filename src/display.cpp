// Écran : OLED I2C (SSD1306 / SH1106) ou Nokia 5110 (PCD8544, SPI), bibliothèque U8g2 (accents OK)
//
// Chaque page est un petit texte, une ligne par ligne d'écran :
//   {velos}      -> valeur de la source « velos »   {heure} {date} {ip} {nom}
//   #texte       -> grande ligne centrée (chiffres géants)
//   *texte       -> ligne en gras centrée
//   =velos 0 20  -> barre de progression de la source velos entre 0 et 20
//   texte        -> ligne normale
// Les polices s'adaptent à la largeur de l'écran (128 ou 84 pixels).
#include "pocl.h"
#include <U8g2lib.h>
#include <time.h>

static U8G2 *disp = nullptr;
static int8_t oledIdx = -1;            // index de la sortie « écran »
static int16_t curKind = -1;           // 0..2 = OLED (modèle), 10 = Nokia 5110
static int8_t curPins[MAX_PINS];
static int32_t curFlip = -1;
static int8_t blPin = -1;              // rétroéclairage du Nokia (facultatif)
static bool blLow = false;
static uint8_t page = 0, rows = 64, cols = 128;
static uint32_t pageAt = 0, lastDraw = 0, infoUntil = 0, msgUntil = 0;
static String msg;
static bool saving = false;
static const uint8_t *fNormal = u8g2_font_helvR08_tf, *fBold = u8g2_font_helvB10_tf, *fBig = u8g2_font_logisoso22_tf;
static uint8_t hNormal = 11, hBold = 14, hBig = 26;

static void setBacklight(bool on) {
  if (blPin >= 0) digitalWrite(blPin, (on != blLow) ? HIGH : LOW);
}

void displayEnd() {
  if (!disp) return;
  disp->clearDisplay();
  disp->setPowerSave(1);
  setBacklight(false);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdelete-non-virtual-dtor"
  switch (curKind) {  // on détruit avec le type exact (U8g2 n'a pas de destructeur virtuel)
    case 1: delete static_cast<U8G2_SH1106_128X64_NONAME_F_HW_I2C *>(disp); break;
    case 2: delete static_cast<U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C *>(disp); break;
    case 10: delete static_cast<U8G2_PCD8544_84X48_F_4W_SW_SPI *>(disp); break;
    default: delete static_cast<U8G2_SSD1306_128X64_NONAME_F_HW_I2C *>(disp); break;
  }
#pragma GCC diagnostic pop
  disp = nullptr;
  curKind = -1;
  curFlip = -1;
}

void displayBegin() {
  oledIdx = -1;
  for (uint8_t i = 0; i < nOutputs; i++)
    if (outputs[i].type == OUT_OLED || outputs[i].type == OUT_NOKIA) {
      oledIdx = i;
      break;
    }
  if (oledIdx < 0) {
    displayEnd();
    blPin = -1;
    return;
  }
  Output &o = outputs[oledIdx];
  bool nokia = o.type == OUT_NOKIA;
  int16_t kind = nokia ? 10 : o.o1;
  bool same = disp && kind == curKind && o.o2 == curFlip && memcmp(curPins, o.pins, MAX_PINS) == 0;
  if (disp && !same) displayEnd();
  if (!disp) {
    const u8g2_cb_t *rot = o.o2 ? U8G2_R2 : U8G2_R0;
    if (nokia) {
      // Nokia 5110 : CLK, DIN, DC, CE obligatoires, RST conseillée (SPI logiciel : n'importe quelles broches)
      for (int i = 0; i < 4; i++)
        if (!halPinOutput(o.pins[i])) { oledIdx = -1; return; }
      uint8_t rst = halPinOutput(o.pins[4]) ? o.pins[4] : U8X8_PIN_NONE;
      disp = new U8G2_PCD8544_84X48_F_4W_SW_SPI(rot, o.pins[0], o.pins[1], o.pins[3], o.pins[2], rst);
    } else {
      int8_t sda = o.pins[0], scl = o.pins[1];
      if (!halPinOutput(sda) || !halPinOutput(scl)) { oledIdx = -1; return; }
      switch (o.o1) {
        case 1: disp = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(rot, U8X8_PIN_NONE, scl, sda); break;
        case 2: disp = new U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C(rot, U8X8_PIN_NONE, scl, sda); break;
        default: disp = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(rot, U8X8_PIN_NONE, scl, sda); break;
      }
    }
    disp->begin();
    disp->enableUTF8Print();
    curKind = kind;
    curFlip = o.o2;
    memcpy(curPins, o.pins, MAX_PINS);
    saving = false;
  }
  rows = disp->getDisplayHeight();
  cols = disp->getDisplayWidth();
  if (cols < 128) {  // petit écran (Nokia 84x48)
    fNormal = u8g2_font_5x8_tf; hNormal = 9;
    fBold = u8g2_font_helvB08_tf; hBold = 11;
    fBig = u8g2_font_logisoso16_tf; hBig = 19;
  } else {
    fNormal = u8g2_font_helvR08_tf; hNormal = 11;
    fBold = u8g2_font_helvB10_tf; hBold = 14;
    fBig = u8g2_font_logisoso22_tf; hBig = 26;
  }
  blPin = (nokia && halPinOutput(o.pins[5])) ? o.pins[5] : -1;
  blLow = nokia && o.o1;
  if (blPin >= 0) {
    pinMode(blPin, OUTPUT);
    setBacklight(!saving);
  }
  disp->setContrast(o.o4);
  page = 0;
  pageAt = millis();
  lastDraw = 0;
}

void displayNextPage() {
  if (oledIdx < 0) return;
  Output &o = outputs[oledIdx];
  if (o.npages) page = (page + 1) % o.npages;
  pageAt = millis();
  lastDraw = 0;
}

void displayInfo(uint32_t ms) {
  infoUntil = millis() + ms;
  lastDraw = 0;
}

void displayMessage(const String &m, uint32_t ms) {
  msg = m;
  msgUntil = millis() + ms;
  lastDraw = 0;
}

// ---------------------------------------------------------------------
static String placeholder(String key) {
  key.trim();
  String k = key;
  k.toLowerCase();
  if (k == "heure" || k == "date") {
    if (!timeValid()) return k == "heure" ? "--:--" : "--/--";
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[16];
    if (k == "heure") snprintf(b, sizeof b, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else snprintf(b, sizeof b, "%02d/%02d", tmv.tm_mday, tmv.tm_mon + 1);
    return b;
  }
  if (k == "ip") return netOnline() ? WiFi.localIP().toString() : String("192.168.4.1");
  if (k == "nom") return dev.name;
  if (k == "wifi") return netOnline() ? WiFi.SSID() : String("-");
  int8_t s = findSource(key);
  return s >= 0 ? sourceDisplay(s) : "?";
}

static String expand(const String &t) {
  String out;
  int i = 0, n = t.length();
  while (i < n) {
    char c = t[i];
    if (c == '{') {
      int j = t.indexOf('}', i);
      if (j > i) {
        out += placeholder(t.substring(i + 1, j));
        i = j + 1;
        continue;
      }
    }
    out += c;
    i++;
  }
  return out;
}

static void drawCentered(const String &s, int y) {
  int w = disp->getUTF8Width(s.c_str());
  disp->drawUTF8(w < cols ? (cols - w) / 2 : 0, y, s.c_str());
}

static void drawPage(const String &tpl) {
  int y = 0, start = 0, n = tpl.length();
  while (start <= n && y < rows) {
    int nl = tpl.indexOf('\n', start);
    if (nl < 0) nl = n;
    String line = tpl.substring(start, nl);
    line.replace("\r", "");
    start = nl + 1;
    if (line.startsWith("#")) {
      disp->setFont(fBig);
      drawCentered(expand(line.substring(1)), y + 1);
      y += hBig;
    } else if (line.startsWith("*")) {
      disp->setFont(fBold);
      drawCentered(expand(line.substring(1)), y);
      y += hBold;
    } else if (line.startsWith("=")) {
      // =source min max
      String a = line.substring(1);
      a.trim();
      int s1 = a.indexOf(' ');
      String id = s1 < 0 ? a : a.substring(0, s1);
      float mn = 0, mx = 100;
      if (s1 >= 0) {
        String rest = a.substring(s1 + 1);
        rest.trim();
        int s2 = rest.indexOf(' ');
        mn = rest.substring(0, s2 < 0 ? rest.length() : s2).toFloat();
        if (s2 >= 0) mx = rest.substring(s2 + 1).toFloat();
      }
      float v;
      float f = 0;
      if (sourceNum(findSource(id), v) && mx != mn) f = constrain((v - mn) / (mx - mn), 0.0f, 1.0f);
      disp->drawFrame(0, y + 1, cols, 9);
      disp->drawBox(2, y + 3, (int)((cols - 4) * f), 5);
      y += 12;
    } else {
      disp->setFont(fNormal);
      disp->drawUTF8(0, y, expand(line).c_str());
      y += hNormal;
    }
  }
}

static void drawInfo() {
  bool narrow = cols < 128;
  int step = narrow ? 8 : 10;
  disp->setFont(u8g2_font_helvB08_tf);
  disp->drawUTF8(0, 0, dev.name.c_str());
  disp->setFont(narrow ? u8g2_font_5x8_tf : u8g2_font_helvR08_tf);
  int y = step + 1;
  auto line = [&](const String &t) {
    if (y < rows) disp->drawUTF8(0, y, t.c_str());
    y += step;
  };
  if (netOnline()) {
    line("WiFi " + WiFi.SSID());
    line(narrow ? "IP " + WiFi.localIP().toString() : "http://" + WiFi.localIP().toString());
  } else {
    line(dev.nnets ? "WiFi : recherche..." : (narrow ? "WiFi à régler" : "WiFi non configuré"));
  }
  if (apActive()) {
    line(narrow ? "WiFi " + dev.name : "Config : WiFi " + dev.name);
    line(dev.apPass.length() >= 8 ? "mdp " + dev.apPass : String("sans mot de passe"));
    line(narrow ? String("puis 192.168.4.1") : String("puis http://192.168.4.1"));
  } else if (netOnline() && !narrow) {
    line("http://" + hostName() + ".local");
  }
}

void displayLoop() {
  if (!disp || oledIdx < 0) return;
  uint32_t now = millis();
  if (now - lastDraw < 250) return;
  lastDraw = now;
  Output &o = outputs[oledIdx];
  bool showMsg = (int32_t)(msgUntil - now) > 0;
  bool showInfo = (int32_t)(infoUntil - now) > 0;
  bool asleep = outputsAsleep() || !o.enabled;
  if (asleep && !showMsg && !showInfo) {
    if (!saving) {
      disp->setPowerSave(1);
      setBacklight(false);
      saving = true;
    }
    return;
  }
  if (saving) {
    disp->setPowerSave(0);
    setBacklight(true);
    saving = false;
  }

  // Tant qu'aucune donnée n'est arrivée et qu'on n'a pas Internet : écran d'aide
  bool anyData = false;
  for (uint8_t i = 0; i < nSources; i++)
    if (sources[i].ok || sources[i].sim) anyData = true;

  if (o.npages && o.o3 > 0 && now - pageAt > (uint32_t)o.o3 * 1000UL) {
    page = (page + 1) % o.npages;
    pageAt = now;
  }
  if (page >= o.npages) page = 0;

  disp->clearBuffer();
  disp->setFontPosTop();
  if (showMsg) {
    drawPage("*" + msg);
  } else if (showInfo || o.npages == 0 || (!anyData && !netOnline())) {
    drawInfo();
  } else {
    drawPage(o.pages[page]);
  }
  disp->sendBuffer();
}
