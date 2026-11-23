// Chargement / enregistrement de la configuration (LittleFS)
//  /device.json : nom, WiFi, point d'accès (propre à chaque objet)
//  /config.json : la « recette » (sources, sorties, boutons) — partageable
#include "pocl.h"
#include <LittleFS.h>
#include <memory>
#include <utility>

Device dev;
Source sources[MAX_SOURCES];
uint8_t nSources = 0;
Output outputs[MAX_OUTPUTS];
uint8_t nOutputs = 0;
Button buttons[MAX_BUTTONS];
uint8_t nButtons = 0;
bool nightOn = false;
uint8_t nightFrom = 22, nightTo = 7;

// Noms des broches tels qu'ils sont imprimés sur la carte
#if defined(CONFIG_IDF_TARGET_ESP32C3)
static const struct { const char *l; int8_t g; } PINMAP[] = {
  {"RX", 20}, {"TX", 21}, {"A0", 3},  // la Super Mini porte seulement les numéros : « 4 » = GPIO4
};
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
static const struct { const char *l; int8_t g; } PINMAP[] = {
  {"RX", 37}, {"TX", 39}, {"A0", 3},  // la S2 Mini porte seulement les numéros : « 7 » = GPIO7
};
#elif defined(ESP32)
static const struct { const char *l; int8_t g; } PINMAP[] = {
  {"TX", 1}, {"RX", 3}, {"VP", 36}, {"VN", 39}, {"A0", 34},
};
#else
static const struct { const char *l; int8_t g; } PINMAP[] = {
  {"D0", 16}, {"D1", 5}, {"D2", 4}, {"D3", 0}, {"D4", 2}, {"D5", 14},
  {"D6", 12}, {"D7", 13}, {"D8", 15}, {"RX", 3}, {"TX", 1}, {"A0", 17},
};
#endif

static const char *const OUT_NAMES[] = {"servo", "stepper", "stepdir", "relay", "neopixel", "led", "buzzer", "oled", "nokia", "mqtt"};
static const char *const ACT_NAMES[] = {"refresh", "page", "toggle", "sleep", "zero", "info"};

int8_t pinFromLabel(const char *s) {
  if (!s || !*s) return -1;
  for (auto &p : PINMAP)
    if (strcasecmp(s, p.l) == 0) return p.g;
#if defined(ESP32)
  // ESP32 : « D23 », « IO23 », « GPIO23 » ou « 23 » désignent tous le GPIO 23
  if (strncasecmp(s, "GPIO", 4) == 0) s += 4;
  else if (strncasecmp(s, "IO", 2) == 0) s += 2;
  else if (toupper((unsigned char)s[0]) == 'D') s += 1;
#endif
  if (isdigit((unsigned char)s[0])) {
    int g = atoi(s);
    if (halPinInput(g)) return g;
  }
  return -1;
}

String pinLabel(int8_t g) {
  for (auto &p : PINMAP)
    if (p.g == g) return p.l;
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  return String(g);
#elif defined(ESP32)
  return "D" + String(g);
#else
  return "?";
#endif
}

bool pinUsed(int8_t g) {
  for (uint8_t i = 0; i < nOutputs; i++)
    for (int8_t p : outputs[i].pins)
      if (p == g) return true;
  for (uint8_t i = 0; i < nButtons; i++)
    if (buttons[i].pin == g) return true;
  return false;
}

uint32_t parseColor(const char *s) {
  if (!s || !*s) return 0;
  if (*s == '#') s++;
  return strtoul(s, nullptr, 16) & 0xFFFFFF;
}

static int findName(const char *const *tab, int n, const char *s) {
  if (!s) return -1;
  for (int i = 0; i < n; i++)
    if (strcmp(tab[i], s) == 0) return i;
  return -1;
}

String defaultName() {
  char b[12];
  snprintf(b, sizeof b, "POCL-%04X", (unsigned)(halChipId() & 0xFFFF));
  return b;
}

// ---------------------------------------------------------------------
//  Réglages de l'objet
// ---------------------------------------------------------------------
bool loadDevice() {
  dev = Device();
  dev.name = defaultName();
  File f = LittleFS.open("/device.json", "r");
  if (!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return false;
  String n = d["name"] | "";
  if (n.length()) dev.name = n;
  dev.apMode = d["ap"] | 0;
  dev.apPass = d["appass"] | "pocl1234";
  for (JsonObject o : d["nets"].as<JsonArray>()) {
    if (dev.nnets >= MAX_NETS) break;
    String s = o["s"] | "";
    if (!s.length()) continue;
    dev.ssid[dev.nnets] = s;
    dev.pass[dev.nnets] = o["p"] | "";
    dev.nnets++;
  }
  return true;
}

bool saveDevice() {
  JsonDocument d;
  d["name"] = dev.name;
  d["ap"] = dev.apMode;
  d["appass"] = dev.apPass;
  JsonArray a = d["nets"].to<JsonArray>();
  for (uint8_t i = 0; i < dev.nnets; i++) {
    JsonObject o = a.add<JsonObject>();
    o["s"] = dev.ssid[i];
    o["p"] = dev.pass[i];
  }
  File f = LittleFS.open("/device.json", "w");
  if (!f) return false;
  serializeJson(d, f);
  f.close();
  return true;
}

// ---------------------------------------------------------------------
//  Recette
// ---------------------------------------------------------------------
static String jsonStr(JsonVariantConst v, const char *def) {
  if (v.is<const char *>()) return v.as<const char *>();
  if (v.is<long>()) return String(v.as<long>());
  if (v.is<float>()) return String(v.as<float>(), 4);
  if (v.is<bool>()) return v.as<bool>() ? "1" : "0";
  return def;
}

static void parseSource(JsonObject s, Source &d) {
  d = Source();
  d.id = s["id"] | "";
  d.id.trim();
  if (!d.id.length()) d.id = "s" + String(nSources + 1);
  const char *t = s["type"] | "url";
  d.type = strcmp(t, "analog") == 0 ? SRC_ANALOG : strcmp(t, "manual") == 0 ? SRC_MANUAL : strcmp(t, "clock") == 0 ? SRC_CLOCK : SRC_URL;
  const char *k = s["kind"] | "num";
  d.kind = strcmp(k, "text") == 0 ? KIND_TEXT : strcmp(k, "minutes") == 0 ? KIND_MINUTES : KIND_NUM;
  d.url = s["url"] | "";
  d.path = s["path"] | "";
  if (d.type == SRC_CLOCK) d.path = s["what"] | "heure";
  d.auth = s["auth"] | "";
  d.tmap = s["tmap"] | "";
  d.mul = s["mul"] | 1.0f;
  if (d.mul == 0) d.mul = 1;
  long every = s["every"] | 300;
  d.every = (uint32_t)constrain(every, (long)MIN_EVERY_S, 86400L);
  d.dec = constrain((int)(s["dec"] | 0), 0, 4);
  if (d.type == SRC_MANUAL) {
    d.num = s["value"] | 0.0f;
    d.ok = true;
  }
}

static void defaultPoints(Output &d, float y0, float y1) {
  d.npts = 2;
  d.px[0] = 0; d.py[0] = y0;
  d.px[1] = 100; d.py[1] = y1;
}

static void parseOutput(JsonObject o, Output &d, int type) {
  d = Output();
  d.type = (OutType)type;
  d.name = o["name"] | "";
  uint8_t n = 0;
  for (JsonVariant p : o["pins"].as<JsonArray>()) {
    if (n >= MAX_PINS) break;
    if (p.is<int>()) d.pins[n++] = p.as<int>();
    else d.pins[n++] = pinFromLabel(p.as<const char *>());
  }
  d.src = findSource(o["src"] | "");
  // courbe
  for (JsonArray pt : o["map"]["pts"].as<JsonArray>()) {
    if (d.npts >= MAX_POINTS) break;
    d.px[d.npts] = pt[0] | 0.0f;
    d.py[d.npts] = pt[1] | 0.0f;
    d.npts++;
  }
  // tri par x croissant (tri par insertion, il y a au plus 6 points)
  for (uint8_t i = 1; i < d.npts; i++)
    for (uint8_t j = i; j > 0 && d.px[j] < d.px[j - 1]; j--) {
      std::swap(d.px[j], d.px[j - 1]);
      std::swap(d.py[j], d.py[j - 1]);
    }
  d.steps = o["map"]["steps"] | false;
  d.smooth = constrain((float)(o["smooth"] | 1.0f), 0.0f, 60.0f);
  // tout-ou-rien
  strlcpy(d.cond, o["cond"] | ">", sizeof d.cond);
  d.thr = jsonStr(o["thr"], "0");
  d.thr.trim();
  String t = d.thr;
  t.replace(',', '.');
  char *end = nullptr;
  d.thrNum = strtod(t.c_str(), &end);
  d.thrIsNum = t.length() && end && *end == 0;
  d.hyst = o["hyst"] | 0.0f;
  d.inv = o["inv"] | false;

  JsonObject op = o["opt"];
  switch (d.type) {
    case OUT_SERVO:
      d.o1 = op["pmin"] | 544;
      d.o2 = op["pmax"] | 2400;
      d.o3 = (op["rel"] | true) ? 1 : 0;
      if (!d.npts) defaultPoints(d, 0, 180);
      break;
    case OUT_STEPPER:
    case OUT_STEPDIR:
      d.o1 = op["spr"] | (d.type == OUT_STEPPER ? 4096 : 200);
      d.o2 = op["speed"] | (d.type == OUT_STEPPER ? 600 : 400);
      d.o3 = strcmp(op["mode"] | "pos", "speed") == 0 ? 1 : 0;
      d.o4 = (op["rev"] | false) ? 1 : 0;
      if (d.o1 < 1) d.o1 = 1;
      if (d.o2 < 1) d.o2 = 1;
      if (!d.npts) defaultPoints(d, 0, 180);
      break;
    case OUT_RELAY:
      d.o1 = (op["low"] | false) ? 1 : 0;
      break;
    case OUT_NEOPIXEL: {
      d.o1 = constrain((int)(op["n"] | 8), 1, 300);
      d.o2 = constrain((int)(op["bri"] | 40), 1, 255);
      const char *m = op["mode"] | "gauge";
      d.o3 = strcmp(m, "color") == 0 ? 1 : strcmp(m, "dot") == 0 ? 2 : 0;
      d.c1 = parseColor(op["c1"] | "#00ff00");
      d.c2 = parseColor(op["c2"] | "#ff0000");
      d.c3 = parseColor(op["c3"] | "");
      if (!d.npts) defaultPoints(d, 0, 100);
      break;
    }
    case OUT_LED:
      if (!d.npts) defaultPoints(d, 0, 100);
      break;
    case OUT_BUZZER:
      d.o1 = constrain((int)(op["freq"] | 2000), 50, 10000);
      d.o2 = strcmp(op["mode"] | "alert", "repeat") == 0 ? 1 : 0;
      break;
    case OUT_OLED: {
      const char *c = op["chip"] | "ssd1306";
      d.o1 = strcmp(c, "sh1106") == 0 ? 1 : strcmp(c, "ssd1306_32") == 0 ? 2 : 0;
      d.o2 = (op["flip"] | false) ? 1 : 0;
      d.o3 = op["every"] | 5;
      d.o4 = constrain((int)(op["contrast"] | 200), 0, 255);
      for (JsonVariant p : op["pages"].as<JsonArray>()) {
        if (d.npages >= MAX_PAGES) break;
        d.pages[d.npages++] = p.as<const char *>();
      }
      if (d.pins[0] < 0) d.pins[0] = POCL_SDA;
      if (d.pins[1] < 0) d.pins[1] = POCL_SCL;
      break;
    }
    case OUT_MQTT:
      d.host = op["host"] | "test.mosquitto.org";
      d.host.trim();
      d.o1 = constrain((int)(op["port"] | 1883), 1, 65535);
      d.user = op["user"] | "";
      d.pass = op["pass"] | "";
      d.topic = op["topic"] | "pocl/{nom}/{id}";
      d.o2 = constrain((int)(op["every"] | 10), 1, 86400);
      d.o3 = strcmp(op["mode"] | "change", "periodic") == 0 ? 1 : 0;
      d.o4 = ((op["retain"] | true) ? 1 : 0) | ((op["json"] | false) ? 2 : 0);
      d.all = strcmp(o["src"] | "", "*") == 0;
      break;
    case OUT_NOKIA:
      d.o1 = (op["bllow"] | false) ? 1 : 0;
      d.o2 = (op["flip"] | false) ? 1 : 0;
      d.o3 = op["every"] | 5;
      d.o4 = constrain((int)(op["contrast"] | 128), 0, 255);
      for (JsonVariant p : op["pages"].as<JsonArray>()) {
        if (d.npages >= MAX_PAGES) break;
        d.pages[d.npages++] = p.as<const char *>();
      }
      break;
    default:
      break;
  }
}

bool applyConfigJson(const String &json, String &err, bool save) {
  if (json.length() > MAX_CONFIG) {
    err = "configuration trop grande";
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) {
    err = String("JSON invalide : ") + e.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    err = "la configuration doit être un objet JSON";
    return false;
  }

  // On garde les dernières valeurs des sources inchangées (pas de trou à l'écran)
  std::unique_ptr<Source[]> old(new Source[nSources ? nSources : 1]);
  uint8_t nOld = nSources;
  for (uint8_t i = 0; i < nOld; i++) old[i] = sources[i];

  outputsEnd();

  nSources = 0;
  for (JsonObject s : doc["sources"].as<JsonArray>()) {
    if (nSources >= MAX_SOURCES) break;
    Source &d = sources[nSources];
    parseSource(s, d);
    d.nextAt = millis();  // nouvelle source : première lecture dès que possible
    for (uint8_t i = 0; i < nOld; i++) {
      Source &o = old[i];
      if (o.id == d.id && o.type == d.type && o.url == d.url && o.path == d.path && d.type != SRC_MANUAL) {
        d.num = o.num; d.text = o.text; d.when = o.when; d.ok = o.ok;
        d.lastOk = o.lastOk; d.nextAt = o.nextAt; d.err = o.err;
        d.sim = o.sim; d.simVal = o.simVal;
        if (d.kind != o.kind || d.tmap != o.tmap || d.auth != o.auth || d.mul != o.mul) d.nextAt = millis();  // relire tout de suite
        break;
      }
    }
    nSources++;
  }

  nOutputs = 0;
  for (JsonObject o : doc["outputs"].as<JsonArray>()) {
    if (nOutputs >= MAX_OUTPUTS) break;
    int t = findName(OUT_NAMES, OUT__COUNT, o["type"] | "");
    if (t < 0) continue;
    parseOutput(o, outputs[nOutputs], t);
    nOutputs++;
  }

  nButtons = 0;
  for (JsonObject b : doc["buttons"].as<JsonArray>()) {
    if (nButtons >= MAX_BUTTONS) break;
    Button &d = buttons[nButtons];
    d = Button();
    JsonVariant p = b["pin"];
    d.pin = p.is<int>() ? p.as<int>() : pinFromLabel(p.as<const char *>());
    if (!halPinInput(d.pin)) continue;
    int a = findName(ACT_NAMES, ACT__COUNT, b["action"] | "refresh");
    d.action = a < 0 ? ACT_REFRESH : a;
    d.target = b["target"] | -1;
    d.name = b["name"] | "";
    nButtons++;
  }

  nightOn = doc["night"]["on"] | false;
  nightFrom = constrain((int)(doc["night"]["from"] | 22), 0, 23);
  nightTo = constrain((int)(doc["night"]["to"] | 7), 0, 23);

  if (save) {
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
      err = "écriture impossible dans la mémoire";
    } else {
      f.print(json);
      f.close();
    }
  }

  outputsBegin();
  displayBegin();
  mqttBegin();
  buttonsBegin();
  statusLedBegin();
  return true;
}

bool loadConfig() {
  String json = "{}";
  File f = LittleFS.open("/config.json", "r");
  if (f) {
    json = f.readString();
    f.close();
  }
  String err;
  if (!applyConfigJson(json, err, false)) {
    Serial.printf("Config illisible : %s\n", err.c_str());
    applyConfigJson("{}", err, false);
    return false;
  }
  return true;
}
