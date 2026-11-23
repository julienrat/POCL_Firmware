// =====================================================================
//  POCL — firmware générique pour Wemos D1 mini (ESP8266)
//  (et pour carte ESP32 DevKit à module WROOM-32 : voir hal.h)
//  Relie des flux de données ouvertes (Opendatasoft, JSON…) à des
//  actionneurs (servo, pas-à-pas, relais, NeoPixel, OLED, LED, buzzer).
//  Toute la configuration se fait depuis la page web hébergée par l'ESP.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "hal.h"

#define POCL_VERSION "1.0.0"

// Limites volontairement basses pour ne pas surcharger l'ESP8266
#if defined(ESP32)
#define MAX_SOURCES 16  // flux de données (l'ESP32 a beaucoup plus de mémoire)
#define MAX_CONFIG  32000
#else
#define MAX_SOURCES 8   // flux de données
#define MAX_CONFIG  16000
#endif
#define MAX_OUTPUTS 8   // actionneurs
#define MAX_BUTTONS 6   // boutons câblés
#define MAX_POINTS  6   // points de la courbe de réglage
#define MAX_PAGES   4   // pages de l'écran OLED
#define MAX_NETS    3   // réseaux WiFi mémorisés
#define MAX_PINS    6   // broches par sortie (écran Nokia : 6)

#define MIN_EVERY_S 30  // intervalle mini entre deux mises à jour d'un flux

// ---------------------------------------------------------------------
//  Sources de données
// ---------------------------------------------------------------------
enum SrcType : uint8_t { SRC_URL = 0, SRC_ANALOG = 1, SRC_MANUAL = 2, SRC_CLOCK = 3 };  // CLOCK : heure, date, lune (calculées par l'objet)
enum SrcKind : uint8_t { KIND_NUM = 0, KIND_TEXT = 1, KIND_MINUTES = 2 };

struct Source {
  String id;              // nom court utilisé partout : "velos", "temp"…
  SrcType type = SRC_URL;
  SrcKind kind = KIND_NUM;
  String url;             // adresse complète (construite par l'assistant)
  String path;            // chemin dans le JSON : "results.0.v" (SRC_CLOCK : ce qui est calculé)
  String auth;            // en-tête Authorization (clé d'API, ex. SNCF) ; jamais exporté par la page
  String tmap;            // correspondances texte -> nombre "OUVERT=1;FERME=0"
  float mul = 1;          // multiplicateur (conversion d'unités : 0.001 pour mm -> m…)
  uint32_t every = 300;   // secondes entre deux mises à jour
  uint8_t dec = 0;        // décimales à l'affichage
  // --- état ---
  float num = NAN;
  String text;
  time_t when = 0;        // pour KIND_MINUTES : instant lu dans la donnée
  bool ok = false;
  uint32_t lastOk = 0, nextAt = 0;
  uint8_t fails = 0;
  String err;
  bool sim = false;       // valeur simulée depuis la page web
  float simVal = 0;
};

// ---------------------------------------------------------------------
//  Sorties (actionneurs)
// ---------------------------------------------------------------------
enum OutType : uint8_t {
  OUT_SERVO, OUT_STEPPER, OUT_STEPDIR, OUT_RELAY,
  OUT_NEOPIXEL, OUT_LED, OUT_BUZZER, OUT_OLED, OUT_NOKIA, OUT_MQTT, OUT__COUNT
};

//  Options (o1..o4) selon le type :
//   SERVO    : o1 = impulsion mini (µs), o2 = impulsion maxi (µs), o3 = relâcher à l'arrêt
//   STEPPER/ : o1 = pas par tour, o2 = vitesse maxi (pas/s),
//   STEPDIR    o3 = mode (0 = position en °, 1 = vitesse en tr/min), o4 = sens inversé
//   RELAY    : o1 = module actif à l'état BAS
//   NEOPIXEL : o1 = nombre de LED, o2 = luminosité 0-255,
//              o3 = mode (0 jauge, 1 couleur, 2 point), c1..c3 = couleurs
//   BUZZER   : o1 = fréquence (Hz), o2 = mode (0 = 3 bips au déclenchement, 1 = bip régulier)
//   OLED     : o1 = modèle (0 SSD1306 128x64, 1 SH1106 128x64, 2 SSD1306 128x32),
//              o2 = rotation 180°, o3 = changement de page auto (s), o4 = contraste
//   NOKIA    : écran Nokia 5110 (PCD8544, 84x48), broches CLK, DIN, DC, CE, RST, BL (facultative)
//              o1 = rétroéclairage actif à l'état bas, o2..o4 comme l'OLED
//   MQTT     : envoi des données à un serveur MQTT (host, user, pass, topic),
//              o1 = port, o2 = intervalle mini (s), o3 = 1 envoi périodique / 0 à chaque changement,
//              o4 = options (1 = retenu, 2 = JSON), all = toutes les données
struct Output {
  OutType type = OUT_SERVO;
  String name;
  int8_t pins[MAX_PINS] = {-1, -1, -1, -1, -1, -1};
  int8_t src = -1;                      // index de la source
  // courbe de réglage : valeur de la donnée (x) -> valeur de sortie (y)
  float px[MAX_POINTS], py[MAX_POINTS];
  uint8_t npts = 0;
  bool steps = false;                   // paliers au lieu d'une interpolation
  float smooth = 1.0;                   // durée de transition (s)
  // tout-ou-rien (relais, buzzer)
  char cond[3] = ">";
  String thr;
  float thrNum = 0;
  bool thrIsNum = true;
  float hyst = 0;
  bool inv = false;
  // options
  int32_t o1 = 0, o2 = 0, o3 = 0, o4 = 0;
  uint32_t c1 = 0, c2 = 0, c3 = 0;
  String pages[MAX_PAGES];
  uint8_t npages = 0;
  String host, user, pass, topic;   // MQTT
  bool all = false;
  // --- état ---
  void *drv = nullptr;
  float cur = NAN, target = NAN, input = NAN;
  bool raw = false, state = false, enabled = true;
  bool testOn = false;
  float testVal = 0;
  uint32_t testUntil = 0, t0 = 0, t1 = 0;
  int32_t last = INT32_MIN;
};

// ---------------------------------------------------------------------
//  Boutons
// ---------------------------------------------------------------------
enum BtnAction : uint8_t { ACT_REFRESH, ACT_PAGE, ACT_TOGGLE, ACT_SLEEP, ACT_ZERO, ACT_INFO, ACT__COUNT };

struct Button {
  int8_t pin = -1;
  uint8_t action = ACT_REFRESH;
  int8_t target = -1;
  String name;
  // --- état ---
  bool activeHigh = false, stable = false, lastRead = false, longDone = false;
  uint32_t changedAt = 0, pressedAt = 0, count = 0;
};

// ---------------------------------------------------------------------
//  Réglages propres à l'objet (non partagés dans les recettes)
// ---------------------------------------------------------------------
struct Device {
  String name;                    // POCL-1A2B : nom du point d'accès et de l'objet
  String ssid[MAX_NETS], pass[MAX_NETS];
  uint8_t nnets = 0;
  uint8_t apMode = 0;             // 0 = 5 min après démarrage, 1 = toujours, 2 = seulement sans WiFi
  String apPass = "pocl1234";     // vide = point d'accès ouvert
};

// ---------------------------------------------------------------------
//  Variables globales
// ---------------------------------------------------------------------
extern Device dev;
extern Source sources[MAX_SOURCES];
extern uint8_t nSources;
extern Output outputs[MAX_OUTPUTS];
extern uint8_t nOutputs;
extern Button buttons[MAX_BUTTONS];
extern uint8_t nButtons;
extern bool nightOn;
extern uint8_t nightFrom, nightTo;
extern bool sleepManual;
extern uint32_t wakeUntil;

// config.cpp
bool loadDevice();
bool saveDevice();
bool loadConfig();
bool applyConfigJson(const String &json, String &err, bool save);
int8_t pinFromLabel(const char *s);
String pinLabel(int8_t gpio);
bool pinUsed(int8_t gpio);
uint32_t parseColor(const char *s);
String defaultName();

// net.cpp
void netBegin();
void netLoop();
bool netOnline();
bool apActive();
void apForce(uint32_t ms);
uint8_t apClients();
uint8_t apChannel();     // canal du point d'accès (0 s'il est fermé)
uint8_t apChannelWhy();  // 0 box, 1 réseau mémorisé, 2 le moins encombré, 3 scan impossible
String hostName();
bool timeValid();
void wifiConnectNow(int8_t idx);

// sources.cpp
void sourcesReset();
void sourcesLoop();
void sourcesRefreshAll();
bool fetchValue(const String &url, const String &path, SrcKind kind, const String &tmap,
                float &num, String &text, String &err,
                const String &auth = String());
bool sourceNum(int8_t i, float &v);
String sourceDisplay(int8_t i);
int8_t findSource(const String &id);
String fmtNum(float v, uint8_t dec);
bool parseIsoTime(const char *s, time_t &out);
WiFiClient *httpClientFor(const String &url, bool &secure, String &err);
void httpClientFree(WiFiClient *c, bool secure);
String httpsHint(WiFiClient *c, bool secure);  // explication d'un échec HTTPS

// outputs.cpp
void outputsBegin();
void outputsEnd();
void outputsLoop();
float mapValue(const Output &o, float x);
bool outputsAsleep();
void outputJog(int8_t i, float deg);
void outputZero(int8_t i);

// display.cpp
void displayBegin();
void displayEnd();
void displayLoop();
void displayNextPage();
void displayInfo(uint32_t ms);
void displayMessage(const String &m, uint32_t ms);

// mqtt.cpp
void mqttBegin();
void mqttLoop();
int mqttState();

// buttons.cpp
void buttonsBegin();
void buttonsLoop();

// web.cpp
void webBegin();
void webLoop();
void requestReboot();

// main.cpp
void statusLedBegin();
