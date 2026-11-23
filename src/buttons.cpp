// Boutons câblés
//  Câblage normal : une patte sur la broche, l'autre sur GND (résistance de
//  tirage interne). Exceptions gérées automatiquement (voir hal.cpp) :
//   ESP8266 : D0 et D8 -> l'autre patte va sur 3V3.
//   ESP32   : GPIO 34 à 39 -> ajouter une résistance de 10 kΩ vers 3V3.
//  Appui court = action choisie. Appui long (3 s) = mode configuration :
//  point d'accès ouvert 10 min + infos réseau à l'écran.
#include "pocl.h"

static void doAction(Button &b) {
  // La nuit, le premier appui réveille l'objet 2 minutes
  if (outputsAsleep() && !sleepManual && b.action != ACT_SLEEP) {
    wakeUntil = millis() + 120000;
    return;
  }
  switch (b.action) {
    case ACT_REFRESH:
      sourcesRefreshAll();
      displayMessage("Mise à jour...", 1500);
      break;
    case ACT_PAGE:
      displayNextPage();
      break;
    case ACT_TOGGLE:
      if (b.target >= 0 && b.target < nOutputs) outputs[b.target].enabled = !outputs[b.target].enabled;
      break;
    case ACT_SLEEP:
      sleepManual = !sleepManual;
      if (!sleepManual && outputsAsleep()) wakeUntil = millis() + 120000;
      break;
    case ACT_ZERO:
      outputZero(b.target);
      displayMessage("Zéro enregistré", 1500);
      break;
    case ACT_INFO:
      displayInfo(15000);
      break;
  }
}

void buttonsBegin() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < nButtons; i++) {
    Button &b = buttons[i];
    halButtonMode(b.pin, b.activeHigh);
    b.stable = b.lastRead = b.longDone = false;
    b.changedAt = now;
  }
}

void buttonsLoop() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < nButtons; i++) {
    Button &b = buttons[i];
    int v = digitalRead(b.pin);
    bool r = b.activeHigh ? v == HIGH : v == LOW;
    if (r != b.lastRead) {
      b.lastRead = r;
      b.changedAt = now;
    }
    if (now - b.changedAt > 30 && r != b.stable) {  // anti-rebond 30 ms
      b.stable = r;
      if (r) {
        b.pressedAt = now;
        b.longDone = false;
        b.count++;
      } else if (!b.longDone) {
        doAction(b);
      }
    }
    if (b.stable && !b.longDone && now - b.pressedAt > 3000) {
      b.longDone = true;
      apForce(600000);
      displayInfo(30000);
      displayMessage("Mode configuration", 2000);
    }
  }
}
