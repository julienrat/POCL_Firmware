// =====================================================================
//  POCL — firmware générique, programme principal
//
//  Cartes prises en charge :
//    - Wemos D1 mini (ESP8266)          core « esp8266 » 3.1.x
//    - ESP32 DevKit, module WROOM-32    core « esp32 » 3.x (schéma de partition « Minimal SPIFFS »)
//
//  Le plus simple : ./flash.sh (installe tout, compile, détecte la carte et flashe).
//  Avec l'IDE Arduino : installer les bibliothèques ArduinoJson (v7), Adafruit NeoPixel,
//  U8g2 et AccelStepper, puis téléverser ce fichier (le dossier src/ est compilé avec).
//
//  Tout le reste (WiFi, données, sorties, boutons) se règle depuis la page web de l'objet :
//  WiFi « POCL-XXXX », mot de passe « pocl1234 », puis http://192.168.4.1
// =====================================================================
#include "src/pocl.h"

// LED bleue de la carte utilisée comme voyant, si sa broche est libre :
//   clignote vite   = point d'accès ouvert, pas de WiFi
//   clignote lent   = recherche du WiFi
//   double éclair   = WiFi OK mais une donnée est en erreur
//   petit éclair    = tout va bien (toutes les 3 s)
static bool ledOn = false;

void statusLedBegin() {
  ledOn = !pinUsed(POCL_LED_PIN);
  if (ledOn) {
    pinMode(POCL_LED_PIN, OUTPUT);
    digitalWrite(POCL_LED_PIN, !POCL_LED_ON);
  }
}

static void statusLedLoop() {
  if (!ledOn) return;
  uint32_t t = millis();
  bool lit;
  if (!netOnline()) {
    lit = apActive() && !dev.nnets ? (t % 200) < 100 : (t % 1000) < 500;
  } else {
    bool err = false;
    for (uint8_t i = 0; i < nSources; i++)
      if (sources[i].err.length()) err = true;
    uint32_t p = t % 3000;
    lit = err ? (p < 60 || (p > 200 && p < 260)) : p < 30;
  }
  digitalWrite(POCL_LED_PIN, lit ? POCL_LED_ON : !POCL_LED_ON);
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // USB natif (ESP32-C3, S2) : ne pas attendre quand aucun ordinateur n'écoute
#endif
  delay(50);
  Serial.println();
  Serial.println(F("=== POCL " POCL_VERSION " (" POCL_BOARD ") ==="));
  halFsBegin();
  halAnalogSetup();
  loadDevice();
  netBegin();
  loadConfig();
  sourcesReset();
  webBegin();
  displayInfo(10000);  // 10 s d'infos de connexion au démarrage (et encore 10 s dès que le WiFi est connecté)
  Serial.printf("Page de configuration : http://%s.local ou http://192.168.4.1 (WiFi %s)\n",
                hostName().c_str(), dev.name.c_str());
}

void loop() {
  netLoop();
  webLoop();
  sourcesLoop();
  outputsLoop();
  buttonsLoop();
  displayLoop();
  mqttLoop();
  statusLedLoop();
}
