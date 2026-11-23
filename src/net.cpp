// WiFi : connexion aux réseaux mémorisés + point d'accès de configuration
//  - Sans réseau mémorisé : point d'accès « POCL-XXXX » + portail captif.
//  - Avec réseaux : on essaie chacun à tour de rôle (15 s par réseau).
//  - Le point d'accès reste ouvert 5 min après chaque démarrage (réglable),
//    ou dès que le WiFi est perdu depuis 20 s, ou après un appui long sur un bouton.
//  - Pendant qu'un téléphone est connecté au point d'accès, on arrête de
//    chercher les autres réseaux (sinon l'ESP change de canal et le décroche).
#include "pocl.h"
#include <DNSServer.h>
#include <time.h>

#define AP_BOOT_WINDOW_MS 300000UL
#define TRY_PER_NET_MS 15000UL
#define LOST_BEFORE_AP_MS 20000UL

static DNSServer dns;
static bool apOn = false;
static uint32_t apForceUntil = 0;
static uint8_t netIdx = 0;
static bool staTrying = false;
static uint32_t tryStart = 0, allowTryUntil = 0, offlineSince = 0, bootMs = 0;
static bool wasConnected = false, mdnsOn = false;
static bool firstConnShown = false;

String hostName() {
  String h;
  for (char c : dev.name) {
    if (isalnum((unsigned char)c)) h += (char)tolower(c);
    else if (c == '-' || c == '_' || c == ' ') h += '-';
  }
  if (!h.length()) h = "pocl";
  return h;
}

bool netOnline() { return WiFi.status() == WL_CONNECTED; }
bool apActive() { return apOn; }
uint8_t apClients() { return apOn ? WiFi.softAPgetStationNum() : 0; }
void apForce(uint32_t ms) { apForceUntil = millis() + ms; }
bool timeValid() { return time(nullptr) > 1700000000; }

static void startAP() {
  if (apOn) return;
  WiFi.mode(dev.nnets ? WIFI_AP_STA : WIFI_AP);
  const char *pass = dev.apPass.length() >= 8 ? dev.apPass.c_str() : nullptr;
  halSoftAP(dev.name, pass);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", IPAddress(192, 168, 4, 1));  // portail captif : tous les noms -> 192.168.4.1
  apOn = true;
  Serial.printf("Point d'acces %s actif (192.168.4.1)\n", dev.name.c_str());
}

static void stopAP() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  Serial.println(F("Point d'acces coupe"));
}

static void beginNet(uint8_t i) {
  if (i >= dev.nnets) return;
  if (!(WiFi.getMode() & WIFI_STA)) WiFi.enableSTA(true);
  WiFi.begin(dev.ssid[i].c_str(), dev.pass[i].c_str());
  staTrying = true;
  tryStart = millis();
  Serial.printf("Connexion a %s...\n", dev.ssid[i].c_str());
}

// Appelé après l'ajout d'un réseau : on tente tout de suite, même si un
// téléphone est connecté au point d'accès.
void wifiConnectNow(int8_t idx) {
  if (!dev.nnets) return;
  netIdx = (idx >= 0 && idx < dev.nnets) ? idx : 0;
  allowTryUntil = millis() + 25000;
  beginNet(netIdx);
}

void netBegin() {
  bootMs = offlineSince = millis();
  halWifiSetup(hostName());  // nom sur le réseau, pas de mise en veille du WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (dev.nnets == 0 || dev.apMode != 2) startAP();
  if (dev.nnets) beginNet(0);
  // Heure de Paris (changement d'heure été/hiver automatique)
  halTimeSetup("CET-1CEST,M3.5.0,M10.5.0/3");
}

void netLoop() {
  uint32_t now = millis();
  if (apOn) dns.processNextRequest();

  bool conn = WiFi.status() == WL_CONNECTED;
  if (conn && !wasConnected) {
    wasConnected = true;
    Serial.printf("WiFi OK : %s  IP %s  (http://%s ou http://%s.local)  signal %d dBm\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str(), hostName().c_str(), WiFi.RSSI());
    if (!firstConnShown) {  // première connexion après le démarrage : adresse IP à l'écran pendant 10 s
      firstConnShown = true;
      displayInfo(10000);
    }
    if (!mdnsOn) {
      mdnsOn = MDNS.begin(hostName());
      if (mdnsOn) MDNS.addService("http", "tcp", 80);
    }
  } else if (!conn && wasConnected) {
    wasConnected = false;
    offlineSince = now;
    tryStart = now;
    Serial.println(F("WiFi perdu"));
  }
  if (mdnsOn) halMdnsUpdate();

  // Recherche des réseaux mémorisés
  if (!conn && dev.nnets) {
    bool client = apOn && WiFi.softAPgetStationNum() > 0;
    bool allowed = !client || (int32_t)(allowTryUntil - now) > 0;
    if (!allowed) {
      if (staTrying) {
        WiFi.disconnect();
        staTrying = false;
      }
    } else if (!staTrying || now - tryStart > TRY_PER_NET_MS) {
      if (staTrying) netIdx = (netIdx + 1) % dev.nnets;
      beginNet(netIdx);
    }
  }

  // Faut-il le point d'accès ?
  bool want;
  if (dev.nnets == 0) want = true;
  else if ((int32_t)(apForceUntil - now) > 0) want = true;
  else if (dev.apMode == 1) want = true;
  else if (!conn && now - offlineSince > LOST_BEFORE_AP_MS) want = true;
  else if (dev.apMode == 0 && now - bootMs < AP_BOOT_WINDOW_MS) want = true;
  else want = false;
  if (!want && apOn && WiFi.softAPgetStationNum() > 0) want = true;  // on ne coupe pas sous les pieds
  if (want && !apOn) startAP();
  else if (!want && apOn) stopAP();
}
