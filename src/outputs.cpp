// Sorties : chaque sortie lit une source, applique sa courbe de réglage
// (points + paliers + lissage) puis pilote son actionneur.
#include "pocl.h"
#include <AccelStepper.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

bool sleepManual = false;
uint32_t wakeUntil = 0;
static uint32_t lastTick = 0;

// Positions des moteurs pas à pas conservées quand on réapplique la config
struct KeepPos { bool valid = false; uint8_t type; int8_t pins[MAX_PINS]; long pos; };
static KeepPos keep[MAX_OUTPUTS];

// ---------------------------------------------------------------------
//  Courbe de réglage
// ---------------------------------------------------------------------
float mapValue(const Output &o, float x) {
  if (o.npts == 0) return x;
  if (o.npts == 1 || x <= o.px[0]) return o.py[0];
  if (x >= o.px[o.npts - 1]) return o.py[o.npts - 1];
  for (uint8_t i = 0; i + 1 < o.npts; i++) {
    if (x < o.px[i + 1]) {
      if (o.steps) return o.py[i];
      float dx = o.px[i + 1] - o.px[i];
      if (dx <= 0) return o.py[i + 1];
      return o.py[i] + (o.py[i + 1] - o.py[i]) * (x - o.px[i]) / dx;
    }
  }
  return o.py[o.npts - 1];
}

bool outputsAsleep() {
  if (sleepManual) return true;
  if ((int32_t)(wakeUntil - millis()) > 0) return false;
  if (nightOn && nightFrom != nightTo && timeValid()) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int h = tmv.tm_hour;
    if (nightFrom > nightTo ? (h >= nightFrom || h < nightTo) : (h >= nightFrom && h < nightTo)) return true;
  }
  return false;
}

static bool outInput(Output &o, float &x) {
  if (o.testOn) {
    if ((int32_t)(o.testUntil - millis()) > 0) {
      x = o.testVal;
      return true;
    }
    o.testOn = false;
  }
  return sourceNum(o.src, x);
}

// Condition des sorties tout-ou-rien, avec hystérésis
static bool evalCond(Output &o) {
  bool r;
  if (!o.thrIsNum && !o.testOn) {  // comparaison de texte (ex. etat = "OUVERT")
    if (o.src < 0 || o.src >= nSources) return false;
    Source &s = sources[o.src];
    if (!s.ok && !s.sim) return false;
    String t = s.sim ? fmtNum(s.simVal, s.dec) : s.text;
    t.trim();
    bool eq = t.equalsIgnoreCase(o.thr);
    r = strcmp(o.cond, "!=") == 0 ? !eq : eq;
  } else {
    float x;
    if (!outInput(o, x)) return false;
    o.input = x;
    float t = o.thrNum, h = fabsf(o.hyst);
    if (strcmp(o.cond, ">") == 0) r = o.raw ? x > t - h : x > t;
    else if (strcmp(o.cond, "<") == 0) r = o.raw ? x < t + h : x < t;
    else if (strcmp(o.cond, ">=") == 0) r = o.raw ? x >= t - h : x >= t;
    else if (strcmp(o.cond, "<=") == 0) r = o.raw ? x <= t + h : x <= t;
    else {
      bool eq = fabsf(x - t) <= (h > 0 ? h : 0.001f);
      r = strcmp(o.cond, "!=") == 0 ? !eq : eq;
    }
  }
  o.raw = r;
  return r != o.inv;
}

// ---------------------------------------------------------------------
//  Démarrage / arrêt des pilotes
// ---------------------------------------------------------------------
static bool samePins(const int8_t *a, const int8_t *b) {
  for (int i = 0; i < MAX_PINS; i++)
    if (a[i] != b[i]) return false;
  return true;
}

static void setupStepper(Output &o, uint8_t idx) {
  AccelStepper *st;
  if (o.type == OUT_STEPPER) {
    for (int i = 0; i < 4; i++)
      if (!halPinOutput(o.pins[i])) return;
    // ULN2003 : IN1, IN3, IN2, IN4 (ordre attendu par AccelStepper pour le 28BYJ-48)
    st = new AccelStepper(AccelStepper::HALF4WIRE, o.pins[0], o.pins[2], o.pins[1], o.pins[3]);
  } else {
    if (!halPinOutput(o.pins[0]) || !halPinOutput(o.pins[1])) return;
    st = new AccelStepper(AccelStepper::DRIVER, o.pins[0], o.pins[1]);
    if (halPinOutput(o.pins[2])) {
      st->setEnablePin(o.pins[2]);
      st->setPinsInverted(false, false, true);  // EN actif à l'état bas (A4988, DRV8825)
    }
  }
  st->setMaxSpeed(o.o2);
  st->setAcceleration(o.o2 * 1.5f);
  long pos = 0;
  if (keep[idx].valid && keep[idx].type == o.type && samePins(keep[idx].pins, o.pins)) pos = keep[idx].pos;
  st->setCurrentPosition(pos);
  st->disableOutputs();
  o.drv = st;
}

void outputsBegin() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < nOutputs; i++) {
    Output &o = outputs[i];
    o.drv = nullptr;
    o.cur = o.target = o.input = NAN;
    o.raw = o.state = false;
    o.t0 = o.t1 = now;
    o.last = INT32_MIN;
    int8_t p = o.pins[0];
    bool pinOk = halPinOutput(p);
    switch (o.type) {
      case OUT_SERVO:
        if (pinOk) o.drv = new PoclServo();
        break;
      case OUT_STEPPER:
      case OUT_STEPDIR:
        setupStepper(o, i);
        break;
      case OUT_RELAY:
        if (pinOk) {
          pinMode(p, OUTPUT);
          digitalWrite(p, o.o1 ? HIGH : LOW);
        }
        break;
      case OUT_NEOPIXEL:
        if (pinOk) {
          Adafruit_NeoPixel *px = new Adafruit_NeoPixel(o.o1, p, NEO_GRB + NEO_KHZ800);
          px->begin();
          px->setBrightness(o.o2);
          px->clear();
          px->show();
          o.drv = px;
        }
        break;
      case OUT_LED:
        if (pinOk) halPwmBegin(p);
        break;
      case OUT_BUZZER:
        if (pinOk) {
          pinMode(p, OUTPUT);
          digitalWrite(p, LOW);
        }
        break;
      default:
        break;
    }
  }
  for (auto &k : keep) k.valid = false;
}

void outputsEnd() {
  for (uint8_t i = 0; i < nOutputs; i++) {
    Output &o = outputs[i];
    int8_t p = o.pins[0];
    switch (o.type) {
      case OUT_SERVO:
        if (o.drv) {
          PoclServo *s = (PoclServo *)o.drv;
          s->detach();
          delete s;
        }
        break;
      case OUT_STEPPER:
      case OUT_STEPDIR:
        if (o.drv) {
          AccelStepper *st = (AccelStepper *)o.drv;
          keep[i].valid = true;
          keep[i].type = o.type;
          memcpy(keep[i].pins, o.pins, MAX_PINS);
          keep[i].pos = st->currentPosition();
          st->disableOutputs();
          delete st;
        }
        break;
      case OUT_NEOPIXEL:
        if (o.drv) {
          Adafruit_NeoPixel *px = (Adafruit_NeoPixel *)o.drv;
          px->clear();
          px->show();
          delete px;
        }
        break;
      case OUT_RELAY:
        if (halPinOutput(p)) digitalWrite(p, o.o1 ? HIGH : LOW);
        break;
      case OUT_LED:
        if (halPinOutput(p)) halPwmEnd(p);
        break;
      case OUT_BUZZER:
        if (halPinOutput(p)) noTone(p);
        break;
      default:
        break;
    }
    o.drv = nullptr;
    if (o.type != OUT_OLED && o.type != OUT_NOKIA)  // écrans : gérés par display.cpp
      for (int8_t q : o.pins)
        if (halPinOutput(q)) pinMode(q, INPUT);
  }
}

// ---------------------------------------------------------------------
//  Réglages manuels des moteurs pas à pas (page web / bouton)
// ---------------------------------------------------------------------
void outputJog(int8_t i, float deg) {
  if (i < 0 || i >= nOutputs || !outputs[i].drv) return;
  Output &o = outputs[i];
  if (o.type != OUT_STEPPER && o.type != OUT_STEPDIR) return;
  deg = constrain(deg, -90.0f, 90.0f);
  AccelStepper *st = (AccelStepper *)o.drv;
  long keepPos = st->currentPosition(), keepTgt = st->targetPosition();
  long steps = lroundf(deg / 360.0f * o.o1) * (o.o4 ? -1 : 1);
  st->enableOutputs();
  st->setCurrentPosition(0);
  st->moveTo(steps);
  uint32_t t0 = millis();
  while (st->distanceToGo() != 0 && millis() - t0 < 6000) {
    st->run();
    yield();
  }
  // On décale physiquement l'aiguille sans changer la position « logique »
  st->setCurrentPosition(keepPos);
  st->moveTo(keepTgt);
}

void outputZero(int8_t i) {
  if (i < 0 || i >= nOutputs || !outputs[i].drv) return;
  Output &o = outputs[i];
  if (o.type != OUT_STEPPER && o.type != OUT_STEPDIR) return;
  ((AccelStepper *)o.drv)->setCurrentPosition(0);  // la position actuelle devient 0°
}

// ---------------------------------------------------------------------
//  Rendu NeoPixel
// ---------------------------------------------------------------------
static uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  uint8_t r = ((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  uint8_t g = ((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  uint8_t bl = (a & 255) + (((int)(b & 255) - (int)(a & 255)) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

static uint32_t gradient(const Output &o, float t) {
  if (!o.c2 && !o.c3) return o.c1;
  if (!o.c3) return lerpColor(o.c1, o.c2, t);
  return t < 0.5f ? lerpColor(o.c1, o.c2, t * 2) : lerpColor(o.c2, o.c3, (t - 0.5f) * 2);
}

static uint32_t scaleColor(uint32_t c, float k) {
  k = constrain(k, 0.0f, 1.0f);
  return ((uint32_t)(((c >> 16) & 255) * k) << 16) | ((uint32_t)(((c >> 8) & 255) * k) << 8) | (uint32_t)((c & 255) * k);
}

static void renderPixels(Output &o, bool hasValue, bool off) {
  Adafruit_NeoPixel *px = (Adafruit_NeoPixel *)o.drv;
  uint16_t n = px->numPixels();
  uint32_t now = millis();
  px->clear();
  if (off) {
    // rien
  } else if (!hasValue) {
    // en attente des données : un point bleu qui se promène
    px->setPixelColor((now / 250) % n, 0x000030);
  } else {
    float v = constrain(o.cur, 0.0f, 100.0f) / 100.0f;
    if (o.o3 == 1) {  // couleur
      uint32_t c = gradient(o, v);
      for (uint16_t i = 0; i < n; i++) px->setPixelColor(i, c);
    } else if (o.o3 == 2) {  // point
      uint16_t k = lroundf(v * (n - 1));
      px->setPixelColor(k, gradient(o, v));
    } else {  // jauge
      float lit = v * n;
      for (uint16_t i = 0; i < n; i++) {
        float f = lit - i;
        if (f <= 0) break;
        uint32_t c = gradient(o, n > 1 ? (float)i / (n - 1) : 0);
        px->setPixelColor(i, f >= 1 ? c : scaleColor(c, f));
      }
    }
  }
  // n'envoyer que si quelque chose a changé
  uint32_t h = 2166136261u;
  for (uint16_t i = 0; i < n; i++) h = (h ^ px->getPixelColor(i)) * 16777619u;
  if ((int32_t)h != o.last) {
    o.last = (int32_t)h;
    px->show();
  }
}

// ---------------------------------------------------------------------
//  Boucle
// ---------------------------------------------------------------------
static void runSteppers() {
  for (uint8_t i = 0; i < nOutputs; i++) {
    Output &o = outputs[i];
    if ((o.type != OUT_STEPPER && o.type != OUT_STEPDIR) || !o.drv) continue;
    AccelStepper *st = (AccelStepper *)o.drv;
    if (o.o3 == 1) {
      if (st->speed() != 0) st->runSpeed();
    } else {
      st->run();
    }
  }
}

void outputsLoop() {
  runSteppers();
  uint32_t now = millis();
  if (now - lastTick < 20) return;  // 50 fois par seconde
  float dt = (now - lastTick) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  lastTick = now;
  bool asleep = outputsAsleep();

  for (uint8_t i = 0; i < nOutputs; i++) {
    Output &o = outputs[i];
    bool off = asleep || !o.enabled;
    int8_t p = o.pins[0];

    // --- sorties tout-ou-rien ---
    if (o.type == OUT_RELAY || o.type == OUT_BUZZER) {
      bool s = off ? false : evalCond(o);
      bool rising = s && !o.state;
      o.state = s;
      o.cur = s ? 1 : 0;
      if (!halPinOutput(p)) continue;
      if (o.type == OUT_RELAY) {
        digitalWrite(p, (s != (o.o1 != 0)) ? HIGH : LOW);
      } else {
        if (rising) o.t0 = now;
        bool beep = false;
        if (s || (o.o2 == 0 && now - o.t0 < 750)) {
          uint32_t e = now - o.t0;
          if (o.o2 == 0) beep = e < 750 && (e / 125) % 2 == 0;  // 3 bips
          else beep = (e % 2000) < 200;                        // bip toutes les 2 s
        }
        if (beep != (o.t1 != 0)) {
          if (beep) tone(p, o.o1);
          else noTone(p);
          o.t1 = beep;
        }
      }
      continue;
    }
    if (o.type == OUT_OLED || o.type == OUT_NOKIA || o.type == OUT_MQTT) continue;  // display.cpp, mqtt.cpp

    // --- sorties proportionnelles ---
    float x;
    bool has = outInput(o, x);
    if (has) {
      o.input = x;
      o.target = mapValue(o, x);
      if (isnan(o.cur) || o.smooth <= 0.01f) o.cur = o.target;
      else o.cur += (o.target - o.cur) * (1.0f - expf(-dt * 3.0f / o.smooth));
    }

    switch (o.type) {
      case OUT_SERVO: {
        PoclServo *sv = (PoclServo *)o.drv;
        if (!sv) break;
        if (off || isnan(o.cur)) {
          if (sv->attached()) sv->detach();
          o.last = INT32_MIN;
          break;
        }
        int us = o.o1 + (int)lroundf((o.o2 - o.o1) * constrain(o.cur, 0.0f, 180.0f) / 180.0f);
        if (o.last == INT32_MIN || abs(us - o.last) >= 2) {
          if (!sv->attached()) sv->attach(p, o.o1, o.o2, us);
          sv->writeMicroseconds(us);
          o.last = us;
          o.t1 = now;
        } else if (o.o3 && sv->attached() && now - o.t1 > 1500) {
          sv->detach();  // relâché : plus de bourdonnement
        }
        break;
      }
      case OUT_STEPPER:
      case OUT_STEPDIR: {
        AccelStepper *st = (AccelStepper *)o.drv;
        if (!st) break;
        int dir = o.o4 ? -1 : 1;
        if (off) {
          st->setSpeed(0);
          st->moveTo(st->currentPosition());
          st->disableOutputs();
          break;
        }
        if (o.o3 == 1) {  // vitesse (tr/min)
          float sps = has ? o.cur * o.o1 / 60.0f * dir : 0;
          sps = constrain(sps, -(float)o.o2, (float)o.o2);
          if (fabsf(sps) < 0.5f) {
            if (st->speed() != 0) {
              st->setSpeed(0);
              st->disableOutputs();
            }
          } else {
            if (st->speed() == 0) st->enableOutputs();
            st->setSpeed(sps);
          }
        } else if (has) {  // position (degrés)
          long tgt = lroundf(o.cur / 360.0f * o.o1) * dir;
          if (tgt != st->targetPosition()) {
            st->enableOutputs();
            st->moveTo(tgt);
            o.t1 = now;
          } else if (st->distanceToGo() == 0 && st->isRunning() == false && now - o.t1 > 1000) {
            st->disableOutputs();  // bobines au repos : le moteur ne chauffe pas
          }
          if (st->distanceToGo() != 0) o.t1 = now;
        }
        break;
      }
      case OUT_NEOPIXEL:
        if (o.drv) renderPixels(o, has && !isnan(o.cur), off);
        break;
      case OUT_LED: {
        if (!halPinOutput(p)) break;
        float v = (off || isnan(o.cur)) ? 0 : constrain(o.cur, 0.0f, 100.0f) / 100.0f;
        int pwm = lroundf(powf(v, 2.2f) * 1023);  // correction gamma : variation plus naturelle
        if (pwm != o.last) {
          halPwmWrite(p, pwm);
          o.last = pwm;
        }
        break;
      }
      default:
        break;
    }
  }
}
