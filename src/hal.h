// Couche d'abstraction matérielle : le même firmware compile pour
//   - ESP8266  : Wemos D1 mini (core esp8266 3.1.x)
//   - ESP32    : carte ESP32 DevKit avec module WROOM-32 (core esp32 3.x)
//   - ESP32-C3 : carte « ESP32-C3 Super Mini » (core esp32 3.x, USB natif)
//   - ESP32-S2 : carte « S2 Mini » de LOLIN / Wemos (core esp32 3.x, USB natif, format D1 mini)
#pragma once
#include <Arduino.h>

#if defined(ESP32)
  #include <esp_arduino_version.h>
  #if ESP_ARDUINO_VERSION_MAJOR < 3
    #error "POCL a besoin du core ESP32 version 3.x (Gestionnaire de cartes : « esp32 » par Espressif, version 3.0 ou plus)"
  #endif
  #include <WiFi.h>
  #include <WebServer.h>
  #include <HTTPUpdateServer.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
  #include <ESPmDNS.h>
  typedef WebServer PoclWebServer;
  typedef HTTPUpdateServer PoclUpdateServer;
  typedef WiFiClientSecure PoclSecureClient;
  #define POCL_WIFI_OPEN WIFI_AUTH_OPEN
  #if defined(CONFIG_IDF_TARGET_ESP32C3)
    #define POCL_BOARD "esp32c3"
    #define POCL_ANALOG_PIN 3    // GPIO3 (ADC1, utilisable avec le WiFi)
    #define POCL_LED_PIN 8       // LED bleue de la Super Mini, allumée à l'état bas
    #define POCL_LED_ON LOW
    #define POCL_SDA 5           // comme les Super Mini à écran 0,42" intégré
    #define POCL_SCL 6
  #elif defined(CONFIG_IDF_TARGET_ESP32S2)
    #define POCL_BOARD "esp32s2"
    #define POCL_ANALOG_PIN 3    // GPIO3 = place de A0 sur la D1 mini (ADC1)
    #define POCL_LED_PIN 15      // LED bleue de la S2 Mini, allumée à l'état haut
    #define POCL_LED_ON HIGH
    #define POCL_SDA 33          // places de D2 / D1 : compatible avec les shields D1 mini
    #define POCL_SCL 35
  #elif defined(CONFIG_IDF_TARGET_ESP32)
    #define POCL_BOARD "esp32"
    #define POCL_ANALOG_PIN 34   // entrée analogique (ADC1, utilisable avec le WiFi)
    #define POCL_LED_PIN 2       // LED bleue des cartes DevKit
    #define POCL_LED_ON HIGH
    #define POCL_SDA 21
    #define POCL_SCL 22
  #else
    #error "Cartes prévues : ESP8266 (Wemos D1 mini), ESP32 (WROOM-32), ESP32-C3 (Super Mini) et ESP32-S2 (S2 Mini)"
  #endif
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266HTTPUpdateServer.h>
  #include <ESP8266HTTPClient.h>
  #include <WiFiClientSecureBearSSL.h>
  #include <ESP8266mDNS.h>
  #include <Servo.h>
  #define POCL_BOARD "esp8266"
  typedef ESP8266WebServer PoclWebServer;
  typedef ESP8266HTTPUpdateServer PoclUpdateServer;
  typedef BearSSL::WiFiClientSecure PoclSecureClient;
  #define POCL_ANALOG_PIN A0
  #define POCL_LED_PIN 2       // D4 : LED bleue de la Wemos
  #define POCL_LED_ON LOW
  #define POCL_SDA 4           // D2
  #define POCL_SCL 5           // D1
  #define POCL_WIFI_OPEN ENC_TYPE_NONE
#endif

uint32_t halChipId();                    // identifiant court (3 derniers octets de l'adresse MAC)
uint32_t halMaxBlock();                  // plus grand bloc de mémoire libre
bool halPinOutput(int p);                // broche utilisable en sortie ?
bool halPinInput(int p);                 // broche utilisable en entrée ?
bool halFsBegin();                       // monte LittleFS (formate si besoin)
void halAnalogSetup();                   // lecture analogique sur 0..1023 sur les deux cartes
void halWifiSetup(const String &host);   // avant WiFi.mode() : nom réseau, pas de mise en veille
void halSoftAP(const String &ssid, const char *pass);
void halTimeSetup(const char *tz);
void halMdnsUpdate();
void halPwmBegin(int pin);               // LED : PWM 1 kHz, valeurs 0..1023
void halPwmWrite(int pin, uint16_t v);
void halPwmEnd(int pin);
void halButtonMode(int pin, bool &activeHigh);

// Servomoteur : bibliothèque Servo sur ESP8266, périphérique LEDC sur ESP32
class PoclServo {
 public:
  bool attach(int pin, int minUs, int maxUs, int us);
  void writeMicroseconds(int us);
  bool attached() const { return _att; }
  void detach();
 private:
  int8_t _pin = -1;
  bool _att = false;
#if !defined(ESP32)
  Servo _s;
#endif
};
