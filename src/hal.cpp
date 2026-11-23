// Implémentation de la couche matérielle (voir hal.h)
#include "hal.h"
#include <LittleFS.h>
#include <time.h>

#if defined(ESP32)
// =====================================================================
//  ESP32 (WROOM-32), ESP32-C3 (Super Mini) et ESP32-S2 (S2 Mini)
// =====================================================================
uint32_t halChipId() {
  uint64_t m = ESP.getEfuseMac();  // octets de l'adresse MAC, du premier au dernier
  return ((uint32_t)((m >> 24) & 0xFF) << 16) | ((uint32_t)((m >> 32) & 0xFF) << 8) | (uint32_t)((m >> 40) & 0xFF);
}
uint32_t halMaxBlock() { return ESP.getMaxAllocHeap(); }

#if defined(CONFIG_IDF_TARGET_ESP32C3)
// ESP32-C3 Super Mini : GPIO0 à 10, 20 et 21 (11 à 17 = mémoire flash, 18 et 19 = USB)
bool halPinOutput(int p) { return (p >= 0 && p <= 10) || p == 20 || p == 21; }
bool halPinInput(int p) { return halPinOutput(p); }
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
// S2 Mini : GPIO1 à 18, 21, 33 à 40 sur les connecteurs, et GPIO0 = bouton « 0 » de la carte
// (19 et 20 = USB, 26 à 32 = mémoire flash et PSRAM)
bool halPinOutput(int p) { return (p >= 0 && p <= 18) || p == 21 || (p >= 33 && p <= 40); }
bool halPinInput(int p) { return halPinOutput(p); }
#else
bool halPinOutput(int p) {
  switch (p) {
    case 0: case 1: case 2: case 3: case 4: case 5:
    case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19:
    case 21: case 22: case 23: case 25: case 26: case 27: case 32: case 33:
      return true;
    default:
      return false;
  }
}
bool halPinInput(int p) { return halPinOutput(p) || p == 34 || p == 35 || p == 36 || p == 39; }
#endif

bool halFsBegin() { return LittleFS.begin(true); }
void halAnalogSetup() { analogReadResolution(10); }

void halWifiSetup(const String &host) {
  WiFi.persistent(false);
  WiFi.setHostname(host.c_str());
  WiFi.setSleep(false);
}
void halSoftAP(const String &ssid, const char *pass, uint8_t ch) {
  WiFi.softAP(ssid.c_str(), pass, ch);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
}
int halScanAll() { return WiFi.scanNetworks(false, true, false, 120); }
void halTimeSetup(const char *tz) { configTzTime(tz, "pool.ntp.org", "time.google.com"); }
void halMdnsUpdate() {}

void halPwmBegin(int pin) {
  ledcAttach(pin, 1000, 10);
  ledcWrite(pin, 0);
}
void halPwmWrite(int pin, uint16_t v) { ledcWrite(pin, v); }
void halPwmEnd(int pin) { ledcDetach(pin); }

void halButtonMode(int pin, bool &activeHigh) {
  // 34, 35, 36, 39 : entrées sans résistance interne -> résistance de 10 kΩ vers 3V3 à ajouter
  pinMode(pin, (pin >= 34) ? INPUT : INPUT_PULLUP);
  activeHigh = false;
}

bool PoclServo::attach(int pin, int, int, int us) {
  _pin = pin;
  _att = ledcAttach(pin, 50, 14);  // 50 Hz, 14 bits : pas de 1,2 µs
  if (_att) writeMicroseconds(us);
  return _att;
}
void PoclServo::writeMicroseconds(int us) {
  if (_att) ledcWrite(_pin, (uint32_t)us * 16383UL / 20000UL);
}
void PoclServo::detach() {
  if (_att) ledcDetach(_pin);
  _att = false;
}

#else
// =====================================================================
//  ESP8266 (Wemos D1 mini)
// =====================================================================
uint32_t halChipId() { return ESP.getChipId(); }
uint32_t halMaxBlock() { return ESP.getMaxFreeBlockSize(); }

bool halPinOutput(int p) { return p >= 0 && p <= 16 && !(p >= 6 && p <= 11); }
bool halPinInput(int p) { return halPinOutput(p); }

bool halFsBegin() {
  if (LittleFS.begin()) return true;
  Serial.println(F("Formatage de la memoire..."));
  LittleFS.format();
  return LittleFS.begin();
}
void halAnalogSetup() {}

void halWifiSetup(const String &host) {
  WiFi.persistent(false);
  WiFi.hostname(host);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
}
void halSoftAP(const String &ssid, const char *pass, uint8_t ch) {
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid.c_str(), pass, ch);
}
int halScanAll() { return WiFi.scanNetworks(false, true); }
void halTimeSetup(const char *tz) { configTime(tz, "pool.ntp.org", "time.google.com"); }
void halMdnsUpdate() { MDNS.update(); }

void halPwmBegin(int pin) {
  analogWriteRange(1023);
  analogWriteFreq(1000);
  pinMode(pin, OUTPUT);
  analogWrite(pin, 0);
}
void halPwmWrite(int pin, uint16_t v) { analogWrite(pin, v); }
void halPwmEnd(int pin) { analogWrite(pin, 0); }

void halButtonMode(int pin, bool &activeHigh) {
  if (pin == 16) {  // D0 : pas de tirage vers le haut -> bouton vers 3V3
    pinMode(16, INPUT_PULLDOWN_16);
    activeHigh = true;
  } else if (pin == 15) {  // D8 : 10 kΩ vers GND sur la carte -> bouton vers 3V3
    pinMode(15, INPUT);
    activeHigh = true;
  } else {
    pinMode(pin, INPUT_PULLUP);
    activeHigh = false;
  }
}

bool PoclServo::attach(int pin, int minUs, int maxUs, int us) {
  _pin = pin;
  _s.attach(pin, minUs, maxUs, us);  // renvoie le n° de broche : 0 pour D3, d'où attached()
  _att = _s.attached();
  return _att;
}
void PoclServo::writeMicroseconds(int us) {
  if (_att) _s.writeMicroseconds(us);
}
void PoclServo::detach() {
  if (_att) _s.detach();
  _att = false;
}
#endif
