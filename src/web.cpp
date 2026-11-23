// Serveur web : page de configuration (compressée en mémoire flash) + API JSON
#include "pocl.h"
#include <LittleFS.h>
#include <time.h>
#include "web_index.h"

static PoclWebServer server(80);
static PoclUpdateServer updater;
static bool rebootPending = false;
static uint32_t rebootAt = 0;

void requestReboot() {
  rebootPending = true;
  rebootAt = millis() + 800;
}

static void sendJson(int code, JsonDocument &d) {
  String out;
  serializeJson(d, out);
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(code, F("application/json"), out);
}

static void sendOk() { server.send(200, F("application/json"), F("{\"ok\":true}")); }

static void sendErr(int code, const String &msg) {
  JsonDocument d;
  d["ok"] = false;
  d["err"] = msg;
  sendJson(code, d);
}

// Portail captif : toute adresse inconnue renvoie vers la page de config
static bool captiveRedirect() {
  if (!apActive()) return false;
  String host = server.hostHeader();
  int c = host.indexOf(':');
  if (c >= 0) host = host.substring(0, c);
  if (!host.length() || host == "192.168.4.1" || host == WiFi.localIP().toString() ||
      host.startsWith(hostName()))
    return false;
  server.sendHeader(F("Location"), F("http://192.168.4.1/"), true);
  server.send(302, F("text/plain"), "");
  return true;
}

static void handleIndex() {
  if (captiveRedirect()) return;
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send_P(200, PSTR("text/html; charset=utf-8"), (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

// ---------------------------------------------------------------------
//  État en direct (lu toutes les 2 s par la page)
// ---------------------------------------------------------------------
static void handleState() {
  uint32_t now = millis();
  JsonDocument d;
  d["name"] = dev.name;
  d["ver"] = POCL_VERSION;
  d["up"] = now / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["blk"] = halMaxBlock();
  d["board"] = POCL_BOARD;
  JsonObject w = d["wifi"].to<JsonObject>();
  bool on = netOnline();
  w["ok"] = on;
  w["ssid"] = on ? WiFi.SSID() : String("");
  w["ip"] = on ? WiFi.localIP().toString() : String("");
  w["rssi"] = on ? WiFi.RSSI() : 0;
  w["ap"] = apActive();
  w["apSsid"] = dev.name;
  w["clients"] = apClients();
  w["host"] = hostName() + ".local";
  if (timeValid()) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[12];
    snprintf(b, sizeof b, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    d["time"] = b;
  }
  d["asleep"] = outputsAsleep();
  d["sleep"] = sleepManual;

  JsonArray sa = d["src"].to<JsonArray>();
  for (uint8_t i = 0; i < nSources; i++) {
    Source &s = sources[i];
    JsonObject o = sa.add<JsonObject>();
    o["id"] = s.id;
    float v;
    if (sourceNum(i, v) && !isnan(v)) o["v"] = v;
    o["txt"] = sourceDisplay(i);
    if (s.text.length()) o["raw"] = s.text;
    o["ok"] = s.ok;
    o["age"] = s.ok ? (int32_t)((now - s.lastOk) / 1000) : -1;
    if (s.err.length()) o["err"] = s.err;
    o["sim"] = s.sim;
    if (s.type == SRC_URL) {
      int32_t nx = (int32_t)(s.nextAt - now);
      o["next"] = nx > 0 ? nx / 1000 : 0;
    }
  }
  JsonArray oa = d["out"].to<JsonArray>();
  for (uint8_t i = 0; i < nOutputs; i++) {
    Output &o = outputs[i];
    JsonObject j = oa.add<JsonObject>();
    if (!isnan(o.cur)) j["cur"] = o.cur;
    if (!isnan(o.input)) j["in"] = o.input;
    j["on"] = o.state;
    j["en"] = o.enabled;
    j["test"] = o.testOn;
    if (o.type == OUT_MQTT) j["mq"] = mqttState();
  }
  JsonArray ba = d["btn"].to<JsonArray>();
  for (uint8_t i = 0; i < nButtons; i++) {
    JsonObject j = ba.add<JsonObject>();
    j["p"] = buttons[i].stable;
    j["n"] = buttons[i].count;
  }
  sendJson(200, d);
}

// ---------------------------------------------------------------------
//  Recette (sources / sorties / boutons)
// ---------------------------------------------------------------------
static void handleGetConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    server.send(200, F("application/json"), F("{\"sources\":[],\"outputs\":[],\"buttons\":[]}"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.streamFile(f, F("application/json"));
  f.close();
}

static void handlePostConfig() {
  String err;
  bool save = server.arg("save") != "0";
  if (!applyConfigJson(server.arg("plain"), err, save)) {
    sendErr(400, err);
    return;
  }
  if (err.length()) {
    sendErr(500, err);
    return;
  }
  sendOk();
}

// ---------------------------------------------------------------------
//  Réglages de l'objet : nom, WiFi, point d'accès
// ---------------------------------------------------------------------
static void handleGetSystem() {
  JsonDocument d;
  d["name"] = dev.name;
  d["ap"] = dev.apMode;
  d["appass"] = dev.apPass;
  JsonArray a = d["nets"].to<JsonArray>();
  for (uint8_t i = 0; i < dev.nnets; i++) a.add(dev.ssid[i]);
  d["ver"] = POCL_VERSION;
  d["maxSrc"] = MAX_SOURCES;
  d["board"] = POCL_BOARD;
  d["chip"] = String(halChipId(), HEX);
  d["sketch"] = ESP.getSketchSize();
  d["free"] = ESP.getFreeSketchSpace();
  d["mac"] = WiFi.macAddress();
  sendJson(200, d);
}

static void handlePostSystem() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) {
    sendErr(400, "JSON invalide");
    return;
  }
  bool reboot = false;
  int8_t connectIdx = -2;
  if (d["name"].is<const char *>()) {
    String n = d["name"].as<const char *>();
    n.trim();
    if (n.length() > 24) n = n.substring(0, 24);
    if (n.length() && n != dev.name) {
      dev.name = n;
      reboot = true;
    }
  }
  if (d["ap"].is<int>() && d["ap"].as<int>() != dev.apMode) {
    dev.apMode = constrain(d["ap"].as<int>(), 0, 2);
  }
  if (d["appass"].is<const char *>()) {
    String p = d["appass"].as<const char *>();
    if (p.length() && p.length() < 8) {
      sendErr(400, "le mot de passe du point d'accès doit faire au moins 8 caractères (ou être vide)");
      return;
    }
    if (p != dev.apPass) {
      dev.apPass = p;
      reboot = true;
    }
  }
  if (d["del"].is<const char *>()) {
    String s = d["del"].as<const char *>();
    for (uint8_t i = 0; i < dev.nnets; i++)
      if (dev.ssid[i] == s) {
        for (uint8_t j = i; j + 1 < dev.nnets; j++) {
          dev.ssid[j] = dev.ssid[j + 1];
          dev.pass[j] = dev.pass[j + 1];
        }
        dev.nnets--;
        break;
      }
  }
  if (d["add"].is<JsonObject>()) {
    String s = d["add"]["s"] | "";
    String p = d["add"]["p"] | "";
    if (s.length()) {
      int8_t idx = -1;
      for (uint8_t i = 0; i < dev.nnets; i++)
        if (dev.ssid[i] == s) idx = i;
      if (idx < 0) {
        if (dev.nnets >= MAX_NETS) {  // on oublie le plus ancien
          for (uint8_t j = 0; j + 1 < MAX_NETS; j++) {
            dev.ssid[j] = dev.ssid[j + 1];
            dev.pass[j] = dev.pass[j + 1];
          }
          dev.nnets = MAX_NETS - 1;
        }
        idx = dev.nnets++;
      }
      dev.ssid[idx] = s;
      dev.pass[idx] = p;
      connectIdx = idx;
    }
  }
  saveDevice();
  JsonDocument r;
  r["ok"] = true;
  r["reboot"] = reboot;
  sendJson(200, r);
  if (reboot) requestReboot();
  else if (connectIdx >= 0) wifiConnectNow(connectIdx);
}

static void handleScan() {
  int n = WiFi.scanComplete();
  JsonDocument d;
  if (n == WIFI_SCAN_FAILED || server.hasArg("new")) {
    if (n != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true, false);
    }
    d["running"] = true;
  } else if (n == WIFI_SCAN_RUNNING) {
    d["running"] = true;
  } else {
    JsonArray a = d["nets"].to<JsonArray>();
    for (int i = 0; i < n && i < 30; i++) {
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      bool dup = false;
      for (JsonObject o : a)
        if (s == (const char *)o["s"]) dup = true;
      if (dup) continue;
      JsonObject o = a.add<JsonObject>();
      o["s"] = s;
      o["r"] = WiFi.RSSI(i);
      o["lock"] = WiFi.encryptionType(i) != POCL_WIFI_OPEN;
    }
    WiFi.scanDelete();
  }
  sendJson(200, d);
}

// ---------------------------------------------------------------------
//  Relais Internet : quand le navigateur n'a pas Internet (téléphone
//  connecté au point d'accès POCL), l'ESP va chercher la page pour lui.
//  Le contenu est transmis par petits morceaux : rien n'est stocké.
// ---------------------------------------------------------------------
static void handleProxy() {
  String url = server.arg("url");
  if (!netOnline()) {
    sendErr(503, "l'objet n'est pas connecté à Internet (règle d'abord le WiFi)");
    return;
  }
  String err;
  bool secure;
  WiFiClient *cl = httpClientFor(url, secure, err);
  if (!cl) {
    sendErr(400, err);
    return;
  }
  String auth = server.arg("auth");  // clé d'API éventuelle (SNCF…)
  {
    HTTPClient http;
    http.setTimeout(10000);
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent(F("POCL/" POCL_VERSION " (" POCL_BOARD ")"));
    if (!http.begin(*cl, url)) {
      sendErr(400, "adresse invalide");
    } else {
      if (auth.length()) http.addHeader(F("Authorization"), auth);
      int code = http.GET();
      if (code <= 0) {
        sendErr(502, "connexion impossible : " + HTTPClient::errorToString(code) + httpsHint(cl, secure));
      } else {
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(code, F("application/json; charset=utf-8"), "");
        WiFiClient &s = http.getStream();
        uint8_t buf[512];
        size_t total = 0;
        uint32_t last = millis();
        while ((s.connected() || s.available()) && total < 400000) {
          size_t a = s.available();
          if (a) {
            size_t r = s.readBytes(buf, a < sizeof buf ? a : sizeof buf);
            server.sendContent((const char *)buf, r);
            total += r;
            last = millis();
          } else {
            if (millis() - last > 10000) break;
            delay(2);
          }
        }
        server.sendContent("");
      }
      http.end();
    }
  }
  httpClientFree(cl, secure);
}

// L'objet essaie lui-même une source (bouton « Tester sur l'objet »)
static void handleTry() {
  JsonDocument q;
  if (deserializeJson(q, server.arg("plain"))) {
    sendErr(400, "JSON invalide");
    return;
  }
  const char *k = q["kind"] | "num";
  SrcKind kind = strcmp(k, "text") == 0 ? KIND_TEXT : strcmp(k, "minutes") == 0 ? KIND_MINUTES : KIND_NUM;
  float num;
  String text, err;
  uint32_t t0 = millis();
  bool ok = fetchValue(q["url"] | "", q["path"] | "", kind, q["tmap"] | "", num, text, err, q["auth"] | "");
  float mul = q["mul"] | 1.0f;
  if (ok && !isnan(num) && mul != 0) num *= mul;
  JsonDocument d;
  d["ok"] = ok;
  if (!isnan(num)) d["num"] = num;
  d["text"] = text;
  if (!ok) d["err"] = err;
  d["ms"] = millis() - t0;
  d["heap"] = ESP.getFreeHeap();
  sendJson(200, d);
}

static int8_t argIndex(const char *name, uint8_t max) {
  if (!server.hasArg(name)) return -1;
  int i = server.arg(name).toInt();
  return (i >= 0 && i < max) ? i : -1;
}

// Valeur simulée pour une source (vide = on arrête de simuler)
static void handleSim() {
  int8_t i = argIndex("src", nSources);
  if (i < 0) return sendErr(400, "source inconnue");
  String v = server.arg("v");
  v.replace(',', '.');
  sources[i].sim = v.length() > 0;
  sources[i].simVal = v.toFloat();
  sendOk();
}

// Valeur d'essai envoyée à une sortie pendant 60 s (curseur « Tester »)
static void handleTest() {
  int8_t i = argIndex("out", nOutputs);
  if (i < 0) return sendErr(400, "sortie inconnue");
  String v = server.arg("v");
  v.replace(',', '.');
  outputs[i].testOn = v.length() > 0;
  outputs[i].testVal = v.toFloat();
  outputs[i].testUntil = millis() + 60000;
  sendOk();
}

static void handleJog() {
  int8_t i = argIndex("out", nOutputs);
  if (i < 0) return sendErr(400, "sortie inconnue");
  if (server.hasArg("zero")) outputZero(i);
  else outputJog(i, server.arg("deg").toFloat());
  sendOk();
}

static void handleAction() {
  String a = server.arg("do");
  if (a == "refresh") sourcesRefreshAll();
  else if (a == "sleep") sleepManual = true;
  else if (a == "wake") { sleepManual = false; wakeUntil = millis() + 600000; }
  else if (a == "page") displayNextPage();
  else if (a == "info") displayInfo(15000);
  else if (a == "ap") apForce(600000);
  else if (a == "toggle") {
    int8_t i = argIndex("out", nOutputs);
    if (i >= 0) outputs[i].enabled = !outputs[i].enabled;
  } else if (a == "reboot") requestReboot();
  else if (a == "factory") {
    LittleFS.remove("/config.json");
    LittleFS.remove("/device.json");
    requestReboot();
  } else return sendErr(400, "action inconnue");
  sendOk();
}

static void handleNotFound() {
  if (captiveRedirect()) return;
  server.send(404, F("text/plain"), F("Introuvable"));
}

void webBegin() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/system", HTTP_GET, handleGetSystem);
  server.on("/api/system", HTTP_POST, handlePostSystem);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/proxy", HTTP_GET, handleProxy);
  server.on("/api/try", HTTP_POST, handleTry);
  server.on("/api/sim", HTTP_GET, handleSim);
  server.on("/api/test", HTTP_GET, handleTest);
  server.on("/api/jog", HTTP_GET, handleJog);
  server.on("/api/action", HTTP_GET, handleAction);
  updater.setup(&server, "/update");  // mise à jour du firmware (.bin)
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();
  if (rebootPending && (int32_t)(millis() - rebootAt) > 0) {
    Serial.println(F("Redemarrage..."));
    delay(100);
    ESP.restart();
  }
}
