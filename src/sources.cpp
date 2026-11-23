// Récupération des données
//  Pour ne pas surcharger l'ESP8266 :
//   - une seule requête à la fois, au moins 1,5 s entre deux requêtes ;
//   - l'URL ne demande qu'UNE valeur (ex. Opendatasoft : select=...as v&limit=1) ;
//   - le JSON est lu en flux avec un filtre : seul le champ utile est gardé en mémoire ;
//   - en cas d'erreur on réessaie de plus en plus tard (15 s, 30 s, 1 min…).
#include "pocl.h"
#include <time.h>

static uint32_t lastAnalog = 0, lastFetchEnd = 0;
static float analogAvg = NAN;

int8_t findSource(const String &id) {
  if (!id.length()) return -1;
  for (uint8_t i = 0; i < nSources; i++)
    if (sources[i].id.equalsIgnoreCase(id)) return i;
  return -1;
}

String fmtNum(float v, uint8_t dec) {
  if (isnan(v)) return "--";
  float lim = 0.5f;
  for (uint8_t i = 0; i < dec; i++) lim /= 10;
  if (fabsf(v) < lim) v = 0;  // évite « -0 »
  return String(v, (unsigned int)dec);
}

// ---------------------------------------------------------------------
//  Dates ISO 8601 -> secondes depuis 1970 (UTC)
// ---------------------------------------------------------------------
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool parseIsoTime(const char *s, time_t &out) {
  int Y, M, D, h = 0, m = 0, sec = 0;
  if (!s) return false;
  // Format compact de Navitia (API SNCF) : 20261119T160500, heure locale
  if (strlen(s) >= 15 && s[8] == 'T' && sscanf(s, "%4d%2d%2dT%2d%2d%2d", &Y, &M, &D, &h, &m, &sec) == 6) {
    struct tm tmv = {};
    tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
    tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = sec; tmv.tm_isdst = -1;
    out = mktime(&tmv);
    return true;
  }
  if (sscanf(s, "%d-%d-%d", &Y, &M, &D) != 3) return false;
  if (M < 1 || M > 12 || D < 1 || D > 31) return false;
  const char *t = strchr(s, 'T');
  if (!t) t = strchr(s, ' ');
  bool hasTz = false;
  long off = 0;
  if (t) {
    sscanf(t + 1, "%d:%d:%d", &h, &m, &sec);
    const char *z = t + 1;
    while (*z && (isdigit((unsigned char)*z) || *z == ':' || *z == '.')) z++;
    if (*z == 'Z') hasTz = true;
    else if (*z == '+' || *z == '-') {
      int oh = 0, om = 0;
      if (strchr(z, ':')) sscanf(z + 1, "%d:%d", &oh, &om);
      else { int v = atoi(z + 1); oh = v / 100; om = v % 100; }
      off = (oh * 60L + om) * 60L * (*z == '-' ? -1 : 1);
      hasTz = true;
    }
  }
  if (!hasTz) {  // heure locale (Paris)
    struct tm tmv = {};
    tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
    tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = sec; tmv.tm_isdst = -1;
    out = mktime(&tmv);
    return true;
  }
  out = (time_t)(daysFromCivil(Y, M, D) * 86400LL + h * 3600L + m * 60L + sec - off);
  return true;
}

// ---------------------------------------------------------------------
//  Client HTTP / HTTPS économe en mémoire
// ---------------------------------------------------------------------
#if !defined(ESP32)
struct MflnEntry { String host; int8_t ok = -1; };
static MflnEntry mfln[4];
static uint8_t mflnNext = 0;
#endif

static bool splitHost(const String &url, String &host, uint16_t &port) {
  int p = url.indexOf("://");
  if (p < 0) return false;
  int start = p + 3;
  int end = url.indexOf('/', start);
  if (end < 0) end = url.length();
  host = url.substring(start, end);
  port = url.startsWith("https") ? 443 : 80;
  int c = host.indexOf(':');
  if (c >= 0) {
    port = host.substring(c + 1).toInt();
    host = host.substring(0, c);
  }
  return host.length() > 0;
}

#if !defined(ESP32)
static uint32_t lastRx = 16384;  // tampon de réception de la dernière connexion HTTPS (ESP8266)
#endif

// Complète un message d'erreur après un échec HTTPS (vide si rien de particulier).
String httpsHint(WiFiClient *c, bool secure) {
#if defined(ESP32)
  (void)c;
  (void)secure;
  return "";
#else
  if (!secure) return "";
  int e = static_cast<PoclSecureClient *>(c)->getLastSSLError();
  if (e == -1000) return F(" — mémoire insuffisante pour HTTPS");
  if ((e & 0xFF) == BR_ERR_TOO_LARGE && lastRx < 16384)
    return String(F(" — ce serveur envoie des blocs de 16 Ko et l'ESP8266 n'a que ")) + String(lastRx / 1024) +
           F(" Ko de libre pour les recevoir (enlever une sortie ou le MQTT libère de la mémoire ; l'ESP32 n'a pas ce souci)");
  if (e) return String(F(" [TLS ")) + String(e) + "]";  // code BearSSL, pour le diagnostic
  return "";
#endif
}

// Renvoie un client prêt à l'emploi. Sur ESP8266, pour HTTPS, on demande au
// serveur des blocs de 1 Ko (MFLN) quand il l'accepte : 15 Ko de RAM économisés.
WiFiClient *httpClientFor(const String &url, bool &secure, String &err) {
  secure = false;
  if (url.startsWith("http://")) return new WiFiClient();
  if (!url.startsWith("https://")) {
    err = "l'adresse doit commencer par http:// ou https://";
    return nullptr;
  }
  String host;
  uint16_t port;
  if (!splitHost(url, host, port)) {
    err = "adresse invalide";
    return nullptr;
  }
#if defined(ESP32)
  if (halMaxBlock() < 40000) {
    err = "mémoire insuffisante pour HTTPS";
    return nullptr;
  }
  PoclSecureClient *c = new PoclSecureClient();
  c->setInsecure();  // données publiques en lecture seule : pas de vérification du certificat
  secure = true;
  return c;
#else
  int8_t ok = -1;
  for (auto &e : mfln)
    if (e.host == host) ok = e.ok;
  if (ok < 0) {
    ok = BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, port, 1024) ? 1 : 0;
    mfln[mflnNext].host = host;
    mfln[mflnNext].ok = ok;
    mflnNext = (mflnNext + 1) % 4;
  }
  // Mémoire prise par une connexion HTTPS (core 3.1.2) : pile BearSSL 6,2 Ko, contexte
  // ≈ 4,6 Ko, tampon d'envoi 512 + 85, tampon de réception taille + 325. Seul ce dernier
  // est gros, et lui seul doit tenir d'un seul bloc.
  // Réserve pour le WiFi : le système alloue de la mémoire à chaque paquet reçu et, s'il
  // n'en trouve plus, la carte redémarre (exception 29). On lui laisse 6 Ko, 8 Ko quand le
  // point d'accès est ouvert (un téléphone connecté consomme aussi).
  const uint32_t reserve = (WiFi.getMode() & WIFI_AP) ? 8192 : 6144;
  const uint32_t fixed = 6200 + 4600 + 512 + 85 + 325 + reserve;
  uint32_t blk = halMaxBlock(), freeH = ESP.getFreeHeap();
  uint32_t room = freeH > fixed ? freeH - fixed : 0;  // place pour les données reçues
  if (blk < 6400) room = 0;                          // la pile BearSSL doit tenir aussi
  else if (blk < room + 325 + 256) room = blk > 325 + 256 ? blk - 325 - 256 : 0;
  uint32_t rx = ok ? 1024 : 16384;
  // Serveur sans MFLN et mémoire juste : tampon réduit. Ça marche quand le serveur envoie
  // de petits blocs TLS (réponses courtes) ; sinon httpsHint() explique l'échec.
  if (room < rx) rx = room & ~511u;
  if (rx < (ok ? 1024u : 4096u)) {
    err = String(F("mémoire insuffisante pour HTTPS : ")) + String(freeH / 1024) + F(" Ko libres (plus grand bloc ") +
          String(blk / 1024) + F(" Ko), il en faut au moins ") + String((fixed + (ok ? 1024 + 325 : 4096 + 325) + 1023) / 1024);
    return nullptr;
  }
  lastRx = rx;
  PoclSecureClient *c = new PoclSecureClient();
  c->setInsecure();  // données publiques en lecture seule : pas de vérification du certificat
  c->setBufferSizes(rx, ok ? 1024 : 512);
  secure = true;
  return c;
#endif
}

void httpClientFree(WiFiClient *c, bool secure) {
  if (secure) delete static_cast<PoclSecureClient *>(c);
  else delete c;
}

// ---------------------------------------------------------------------
//  Chemin JSON : "results.0.v", "current.temperature_2m", "list[2].main.temp"
// ---------------------------------------------------------------------
static void splitPath(const String &path, String *tok, uint8_t &n, uint8_t max) {
  n = 0;
  String p = path;
  p.replace("[", ".");
  p.replace("]", "");
  int start = 0;
  while (start <= (int)p.length() && n < max) {
    int dot = p.indexOf('.', start);
    if (dot < 0) dot = p.length();
    String t = p.substring(start, dot);
    t.trim();
    if (t.length()) tok[n++] = t;
    start = dot + 1;
  }
}

static bool isIndex(const String &t) {
  if (!t.length()) return false;
  for (char c : t)
    if (!isdigit((unsigned char)c)) return false;
  return true;
}

static bool lookupMap(const String &tmap, const String &text, float &out) {
  int start = 0;
  bool hasDef = false;
  float def = 0;
  while (start < (int)tmap.length()) {
    int sc = tmap.indexOf(';', start);
    if (sc < 0) sc = tmap.length();
    String pair = tmap.substring(start, sc);
    int eq = pair.lastIndexOf('=');
    if (eq > 0) {
      String k = pair.substring(0, eq);
      k.trim();
      String v = pair.substring(eq + 1);
      v.trim();
      v.replace(',', '.');
      if (k == "*") { hasDef = true; def = v.toFloat(); }
      else if (k.equalsIgnoreCase(text)) { out = v.toFloat(); return true; }
    }
    start = sc + 1;
  }
  if (hasDef) out = def;
  return hasDef;
}

// Ajoute un chemin au filtre ArduinoJson (plusieurs chemins = un seul filtre fusionné)
static void addFilter(JsonDocument &filter, const String &path) {
  String tok[12];
  uint8_t n;
  splitPath(path, tok, n, 12);
  if (!n) {  // racine : tout garder
    filter.set(true);
    return;
  }
  JsonVariant node = filter.as<JsonVariant>();
  for (uint8_t i = 0; i < n; i++) {
    if (node.is<bool>()) return;  // ce niveau est déjà gardé en entier
    JsonVariant child;
    if (isIndex(tok[i])) {  // un tableau dans le filtre s'applique à tous ses éléments
      JsonArray arr = node.is<JsonArray>() ? node.as<JsonArray>() : node.to<JsonArray>();
      if (arr.size()) child = arr[0];
      else child = arr.add<JsonVariant>();
    } else {
      JsonObject obj = node.is<JsonObject>() ? node.as<JsonObject>() : node.to<JsonObject>();
      child = obj[tok[i]];
      if (child.isNull()) child = obj[tok[i]].to<JsonVariant>();  // crée le membre
    }
    node = child;
  }
  node.set(true);
}

// Lit la valeur d'un chemin dans le document déjà filtré
static bool extractAt(JsonVariantConst root, const String &path, float &num, String &text, String &err) {
  String tok[12];
  uint8_t n;
  splitPath(path, tok, n, 12);
  JsonVariantConst v = root;
  for (uint8_t i = 0; i < n && !v.isNull(); i++) {
    if (isIndex(tok[i]) && v.is<JsonArrayConst>()) v = v[(size_t)tok[i].toInt()];
    else v = v[tok[i]];
  }
  if (v.isNull()) {
    err = F("valeur introuvable (vérifie le chemin)");
    return false;
  }
  if (v.is<bool>()) {
    num = v.as<bool>() ? 1 : 0;
    text = v.as<bool>() ? "oui" : "non";
  } else if (v.is<double>()) {
    num = v.as<double>();
  } else if (v.is<const char *>()) {
    text = v.as<const char *>();
    if (text.length() > 63) text = text.substring(0, 63);
    String t = text;
    t.trim();
    t.replace(',', '.');
    char *end = nullptr;
    double d = strtod(t.c_str(), &end);
    if (t.length() && end && *end == 0) num = d;
  } else if (v.is<JsonArrayConst>()) {
    num = v.size();
    text = String(v.size()) + F(" éléments");
  } else {
    num = v.size();
    text = F("{objet}");
  }
  return true;
}

// Une seule requête pour plusieurs données qui partagent la même adresse (et la même clé) :
// chaque donnée a son chemin. Renvoie false si la requête elle-même a échoué (err) ;
// sinon chaque donnée a son résultat (errs[i] vide = réussi).
bool fetchGroup(const String &url, const String &auth, uint8_t n, const String *const *paths,
                float *nums, String *texts, String *errs, String &err) {
  for (uint8_t i = 0; i < n; i++) { nums[i] = NAN; texts[i] = ""; errs[i] = ""; }
  err = "";
  if (WiFi.status() != WL_CONNECTED) {
    err = F("pas de connexion WiFi");
    return false;
  }
  bool secure;
  WiFiClient *cl = httpClientFor(url, secure, err);
  if (!cl) return false;

  bool ok = false;
  {
    HTTPClient http;
    http.setTimeout(8000);
    http.useHTTP10(true);  // pas de « chunked » : on peut lire le flux directement
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent(F("POCL/" POCL_VERSION " (" POCL_BOARD ")"));
    if (!http.begin(*cl, url)) {
      err = F("adresse invalide");
    } else {
      http.addHeader(F("Accept"), F("application/json"));
      if (auth.length()) http.addHeader(F("Authorization"), auth);  // clé d'API (SNCF…)
      int code = http.GET();
      if (code <= 0) {
        err = String(F("connexion impossible (")) + HTTPClient::errorToString(code) + ")" + httpsHint(cl, secure);
      } else if (code < 200 || code > 299 || code == 204) {  // 206 (Hub'Eau : « il y a d'autres résultats ») est valable
        err = String(F("le serveur répond ")) + String(code);
      } else {
        // Filtre : on ne garde en mémoire que les chemins demandés
        JsonDocument filter;
        for (uint8_t i = 0; i < n; i++) addFilter(filter, *paths[i]);
        JsonDocument doc;
        DeserializationError e = deserializeJson(doc, http.getStream(),
                                                 DeserializationOption::Filter(filter),
                                                 DeserializationOption::NestingLimit(30));
        if (e) {
          err = String(F("réponse illisible (")) + e.c_str() + ")" + httpsHint(cl, secure);
        } else {
          ok = true;
          for (uint8_t i = 0; i < n; i++) extractAt(doc.as<JsonVariantConst>(), *paths[i], nums[i], texts[i], errs[i]);
        }
      }
      http.end();
    }
  }
  httpClientFree(cl, secure);
  return ok;
}

// Correspondances texte -> nombre, puis date -> minutes restantes
static bool finishValue(SrcKind kind, const String &tmap, float &num, const String &text, String &err) {
  if (tmap.length() && text.length()) {
    float m;
    if (lookupMap(tmap, text, m)) num = m;
  }
  if (kind == KIND_MINUTES) {
    time_t t;
    if (!text.length() || !parseIsoTime(text.c_str(), t)) {
      err = F("ce n'est pas une date");
      return false;
    }
    if (!timeValid()) {
      err = F("heure pas encore synchronisée");
      return false;
    }
    num = (float)((double)(t - time(nullptr)) / 60.0);
  }
  return true;
}

bool fetchValue(const String &url, const String &path, SrcKind kind, const String &tmap,
                float &num, String &text, String &err, const String &auth) {
  const String *paths[1] = {&path};
  String errs[1];
  if (!fetchGroup(url, auth, 1, paths, &num, &text, errs, err)) return false;
  if (errs[0].length()) {
    err = errs[0];
    return false;
  }
  return finishValue(kind, tmap, num, text, err);
}

// ---------------------------------------------------------------------
//  Planification
// ---------------------------------------------------------------------
void sourcesReset() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < nSources; i++) sources[i].nextAt = now + i * 300;
}

void sourcesRefreshAll() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < nSources; i++) {
    sources[i].nextAt = now;
    sources[i].fails = 0;
  }
}

// Lit une donnée… et en même temps toutes celles qui ont la même adresse et la même clé :
// une seule connexion pour la température, le vent et l'humidité d'Open-Meteo, par exemple.
static void fetchSource(uint8_t first) {
  const Source &s0 = sources[first];
  uint8_t idx[MAX_SOURCES], n = 0;
  for (uint8_t i = 0; i < nSources; i++) {
    const Source &s = sources[i];
    if (s.type == SRC_URL && s.url == s0.url && s.auth == s0.auth) idx[n++] = i;
  }
  const String *paths[MAX_SOURCES];
  float nums[MAX_SOURCES];
  String texts[MAX_SOURCES], errs[MAX_SOURCES], err;
  for (uint8_t k = 0; k < n; k++) paths[k] = &sources[idx[k]].path;
  bool reqOk = fetchGroup(s0.url, s0.auth, n, paths, nums, texts, errs, err);
  uint32_t now = millis();
  for (uint8_t k = 0; k < n; k++) {
    Source &s = sources[idx[k]];
    String e = reqOk ? errs[k] : err;
    float num = nums[k];
    bool ok = reqOk && !e.length() && finishValue(s.kind, s.tmap, num, texts[k], e);
    if (ok) {
      if (!isnan(num)) num *= s.mul;  // conversion d'unités (minutes -> heures ou jours aussi)
      s.num = num;
      s.text = texts[k];
      s.when = 0;
      if (s.kind == KIND_MINUTES) parseIsoTime(texts[k].c_str(), s.when);
      s.ok = true;
      s.lastOk = now;
      s.err = "";
      s.fails = 0;
      s.nextAt = now + s.every * 1000UL;
    } else {
      s.err = e;
      if (s.fails < 10) s.fails++;
      uint32_t back = 15000UL << (s.fails > 5 ? 5 : s.fails - 1);
      if (back > s.every * 1000UL) back = s.every * 1000UL;
      s.nextAt = now + back;
      Serial.printf("[%s] erreur : %s\n", s.id.c_str(), e.c_str());
    }
  }
  lastFetchEnd = millis();
}

// ---------------------------------------------------------------------
//  Sources calculées : heure, date, phase de la lune
// ---------------------------------------------------------------------
static uint32_t lastClock = 0;
// Listes de mots en mémoire flash (pas en RAM) : « mot0|mot1|… »
static const char JOURS[] PROGMEM = "dimanche|lundi|mardi|mercredi|jeudi|vendredi|samedi";
static const char MOIS[] PROGMEM = "janvier|février|mars|avril|mai|juin|juillet|août|septembre|octobre|novembre|décembre";
static const char PHASES[] PROGMEM = "nouvelle lune|premier croissant|premier quartier|lune gibbeuse|pleine lune|lune gibbeuse|dernier quartier|dernier croissant";
static String word(PGM_P list, uint8_t idx) {
  String w;
  char c;
  while ((c = pgm_read_byte(list++)) != 0) {
    if (c == '|') {
      if (idx-- == 0) return w;
      w = "";
    } else if (idx == 0) w += c;
  }
  return w;
}
static bool is(const String &w, PGM_P name) { return strcmp_P(w.c_str(), name) == 0; }

static void clockSource(Source &s) {
  if (!timeValid()) {
    s.ok = false;
    s.err = F("heure pas encore synchronisée");
    return;
  }
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  const String &w = s.path;
  char buf[8];
  float v;
  String txt;
  if (is(w, PSTR("minute"))) {
    v = lt.tm_min;
  } else if (is(w, PSTR("seconde"))) {
    v = lt.tm_sec;
  } else if (is(w, PSTR("jour_semaine"))) {  // 1 = lundi … 7 = dimanche
    v = lt.tm_wday == 0 ? 7 : lt.tm_wday;
    txt = word(JOURS, lt.tm_wday);
  } else if (is(w, PSTR("jour_mois"))) {
    v = lt.tm_mday;
    txt = String(lt.tm_mday) + " " + word(MOIS, lt.tm_mon);
  } else if (is(w, PSTR("mois"))) {
    v = lt.tm_mon + 1;
    txt = word(MOIS, lt.tm_mon);
  } else if (is(w, PSTR("jour_annee"))) {
    v = lt.tm_yday + 1;
  } else if (is(w, PSTR("lune")) || is(w, PSTR("lune_age"))) {
    // Âge de la lune depuis la nouvelle lune du 6 janvier 2000 à 18 h 14 UTC
    const double syn = 29.530588853;
    double age = fmod((double)(t - 947182440L) / 86400.0, syn);
    if (age < 0) age += syn;
    txt = word(PHASES, (int)(age / syn * 8 + 0.5) % 8);
    v = is(w, PSTR("lune")) ? (float)((1 - cos(2 * PI * age / syn)) / 2 * 100) : (float)age;
  } else {  // "heure" : heure décimale (14,5 = 14 h 30)
    v = lt.tm_hour + lt.tm_min / 60.0f + lt.tm_sec / 3600.0f;
    snprintf_P(buf, sizeof buf, PSTR("%02d:%02d"), lt.tm_hour, lt.tm_min);
    txt = buf;
  }
  s.num = v * s.mul;
  s.text = s.kind == KIND_TEXT && txt.length() ? txt : String();
  s.ok = true;
  s.err = "";
  s.lastOk = millis();
}

void sourcesLoop() {
  uint32_t now = millis();

  // Entrée analogique (A0 sur ESP8266, GPIO34 sur ESP32) : potentiomètre, capteur…
  bool hasAnalog = false;
  for (uint8_t i = 0; i < nSources; i++)
    if (sources[i].type == SRC_ANALOG) hasAnalog = true;
  if (hasAnalog && now - lastAnalog >= 100) {
    lastAnalog = now;
    int r = analogRead(POCL_ANALOG_PIN);
    analogAvg = isnan(analogAvg) ? r : analogAvg * 0.8f + r * 0.2f;
    for (uint8_t i = 0; i < nSources; i++)
      if (sources[i].type == SRC_ANALOG) {
        sources[i].num = analogAvg;
        sources[i].ok = true;
        sources[i].lastOk = now;
      }
  }

  // Horloge, calendrier et lune : calculés par l'objet, sans Internet
  if (now - lastClock >= 1000) {
    lastClock = now;
    for (uint8_t i = 0; i < nSources; i++)
      if (sources[i].type == SRC_CLOCK) clockSource(sources[i]);
  }

  if (WiFi.status() != WL_CONNECTED) return;
  if (now - lastFetchEnd < 1500) return;

  int8_t best = -1;
  int32_t bestLate = -1;
  for (uint8_t i = 0; i < nSources; i++) {
    Source &s = sources[i];
    if (s.type != SRC_URL || !s.url.length()) continue;
    int32_t late = (int32_t)(now - s.nextAt);
    if (late >= 0 && late > bestLate) {
      bestLate = late;
      best = i;
    }
  }
  if (best >= 0) fetchSource(best);
}

// ---------------------------------------------------------------------
//  Lecture des valeurs par les sorties
// ---------------------------------------------------------------------
bool sourceNum(int8_t i, float &v) {
  if (i < 0 || i >= nSources) return false;
  Source &s = sources[i];
  if (s.sim) {
    v = s.simVal;
    return true;
  }
  if (!s.ok) return false;
  if (s.kind == KIND_MINUTES && s.when) {
    if (!timeValid()) return false;
    v = (float)((double)(s.when - time(nullptr)) / 60.0) * s.mul;  // compte à rebours en direct
    return true;
  }
  if (isnan(s.num)) return false;
  v = s.num;
  return true;
}

String sourceDisplay(int8_t i) {
  if (i < 0 || i >= nSources) return "?";
  Source &s = sources[i];
  if (s.sim) return fmtNum(s.simVal, s.dec);
  if (!s.ok) return "--";
  if (s.kind == KIND_TEXT && s.text.length()) return s.text;
  float v;
  if (sourceNum(i, v)) return fmtNum(v, s.dec);
  if (s.text.length()) return s.text;
  return "--";
}
