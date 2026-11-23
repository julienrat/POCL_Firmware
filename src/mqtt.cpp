// Sortie « Envoi MQTT » : publie les données sur un serveur MQTT
// (domotique, tableau de bord, autre objet…). Bibliothèque PubSubClient.
//  - Un seul serveur : celui de la première sortie MQTT de la recette.
//  - Sujet : {nom} = nom de l'objet, {id} = nom court de la donnée.
//  - Envoi à chaque changement (au plus toutes les N s) ou périodique.
//  - Reconnexion automatique, de plus en plus espacée en cas d'échec.
#include "pocl.h"
#include <PubSubClient.h>
#include <time.h>

static WiFiClient mqNet;
static PubSubClient mq(mqNet);
static int8_t cfgIdx = -1;
static String curHost, curUser, curPass;
static uint16_t curPort = 0;
static uint32_t nextTry = 0, backoff = 5000;

int mqttState() {
  if (cfgIdx < 0) return -10;
  if (!netOnline()) return -11;
  return mq.connected() ? 0 : mq.state();
}

void mqttBegin() {
  cfgIdx = -1;
  for (uint8_t i = 0; i < nOutputs; i++)
    if (outputs[i].type == OUT_MQTT) {
      if (cfgIdx < 0) cfgIdx = i;
      outputs[i].t0 = 0;
      outputs[i].last = INT32_MIN;
      outputs[i].cur = 0;
      outputs[i].input = NAN;
    }
  if (cfgIdx < 0) {
    if (mq.connected()) mq.disconnect();
    curHost = "";
    return;
  }
  Output &o = outputs[cfgIdx];
  if (o.host != curHost || o.o1 != curPort || o.user != curUser || o.pass != curPass) {
    if (mq.connected()) mq.disconnect();
    curHost = o.host;  // PubSubClient garde un pointeur vers ce texte : il doit rester en vie
    curPort = o.o1;
    curUser = o.user;
    curPass = o.pass;
    mq.setServer(curHost.c_str(), curPort);
    nextTry = 0;
    backoff = 5000;
  }
  mq.setBufferSize(512);
  mq.setKeepAlive(30);
  mq.setSocketTimeout(5);
}

static String topicFor(const Output &o, int8_t i) {
  String t = o.topic;
  t.replace("{nom}", hostName());
  t.replace("{id}", i >= 0 ? sources[i].id : String("valeur"));
  return t;
}

static String payloadFor(const Output &o, int8_t i, bool test, float tv) {
  uint8_t dec = i >= 0 ? sources[i].dec : 2;
  String txt = test ? fmtNum(tv, dec) : sourceDisplay(i);
  if (!(o.o4 & 2)) return txt;
  JsonDocument d;
  d["objet"] = dev.name;
  if (i >= 0) d["id"] = sources[i].id;
  float v;
  if (test) d["valeur"] = tv;
  else if (sourceNum(i, v)) d["valeur"] = v;
  d["texte"] = txt;
  if (timeValid()) d["heure"] = (uint32_t)time(nullptr);
  String s;
  serializeJson(d, s);
  return s;
}

static uint32_t hashStr(const String &s, uint32_t h) {
  for (char c : s) h = (h ^ (uint8_t)c) * 16777619u;
  return h;
}

static void publishOutput(Output &o, uint32_t now) {
  bool retain = o.o4 & 1;
  bool test = o.testOn && (int32_t)(o.testUntil - now) > 0;
  if (!test) o.testOn = false;

  // Valeur d'essai (curseur de la page) : envoyée tout de suite quand elle change
  if (test) {
    if (o.input == o.testVal) return;
    int8_t i = o.all ? (nSources ? 0 : -1) : o.src;
    if (mq.publish(topicFor(o, i).c_str(), payloadFor(o, i, true, o.testVal).c_str(), retain)) o.cur += 1;
    o.input = o.testVal;
    o.t0 = now;
    return;
  }
  o.input = NAN;
  if (o.t0 && now - o.t0 < (uint32_t)o.o2 * 1000UL) return;

  int8_t first = o.all ? 0 : o.src, last = o.all ? (int8_t)nSources - 1 : o.src;
  if (first < 0 || last < first) return;
  // texte de chaque donnée disponible -> empreinte pour détecter un changement
  uint32_t h = 2166136261u;
  for (int8_t i = first; i <= last; i++)
    if (sources[i].ok || sources[i].sim) h = hashStr(sources[i].id + "=" + sourceDisplay(i), h);
  if (o.o3 == 0 && (int32_t)h == o.last && o.t0) return;  // mode « changement » : rien de neuf
  for (int8_t i = first; i <= last; i++) {
    if (!sources[i].ok && !sources[i].sim) continue;
    if (mq.publish(topicFor(o, i).c_str(), payloadFor(o, i, false, 0).c_str(), retain)) o.cur += 1;
  }
  o.last = (int32_t)h;
  o.t0 = now;
}

void mqttLoop() {
  if (cfgIdx < 0 || !netOnline()) return;
  uint32_t now = millis();
  if (!mq.connected()) {
    for (uint8_t i = 0; i < nOutputs; i++)
      if (outputs[i].type == OUT_MQTT) outputs[i].state = false;
    if ((int32_t)(now - nextTry) < 0) return;
    String id = hostName();
    bool ok = curUser.length() ? mq.connect(id.c_str(), curUser.c_str(), curPass.c_str()) : mq.connect(id.c_str());
    if (ok) {
      backoff = 5000;
      Serial.printf("MQTT connecté à %s:%u\n", curHost.c_str(), curPort);
      for (uint8_t i = 0; i < nOutputs; i++)
        if (outputs[i].type == OUT_MQTT) outputs[i].t0 = 0;  // tout renvoyer
    } else {
      backoff = backoff * 2 > 120000UL ? 120000UL : backoff * 2;
      Serial.printf("MQTT : connexion à %s impossible (%d)\n", curHost.c_str(), mq.state());
    }
    nextTry = millis() + backoff;
    return;
  }
  mq.loop();
  for (uint8_t i = 0; i < nOutputs; i++) {
    Output &o = outputs[i];
    if (o.type != OUT_MQTT) continue;
    o.state = true;
    if (o.enabled) publishOutput(o, now);
  }
}
