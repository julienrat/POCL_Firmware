// =====================================================================
//  teste_servo — outil : vérifier un servomoteur et retrouver sa broche
//
//  1. Balayage : chaque broche, à tour de rôle, envoie au servo 0° puis
//     180° puis 90°. Le moniteur série affiche la broche en cours :
//     note celle qui fait bouger le servo.
//       - aucune broche ne le fait bouger : câblage, alimentation ou servo ;
//       - il bouge sur une autre broche que prévu : câblage à corriger
//         (ou broche à changer dans la page web de POCL) ;
//       - il bouge sur la bonne broche : le servo et le câblage sont bons.
//  2. Commandes (taper puis Entrée) :
//       D5 (ou p 14)   garder le servo sur cette broche
//       90             angle de 0 à 180°
//       1500           impulsion en µs (500 à 2500)
//       tout           relancer le balayage de toutes les broches
//
//  Lancement :  ./flash.sh --croquis outils/teste_servo
//  Ensuite :    ./flash.sh   (remet le firmware POCL, la configuration est conservée)
//
//  ATTENTION : les broches envoient des impulsions à tour de rôle. Si un relais
//  (surtout en 230 V), un moteur ou un ruban de LED est aussi branché, coupe
//  son alimentation pendant le test.
// =====================================================================
#include <Arduino.h>

struct Broche { const char *nom; uint8_t gpio; };

#if defined(ESP8266)
#include <Servo.h>
static const char *CARTE = "Wemos D1 mini (ESP8266)";
static const Broche P[] = {  // TX et RX servent au moniteur série
  {"D0", 16}, {"D1", 5}, {"D2", 4}, {"D3", 0}, {"D4", 2}, {"D5", 14}, {"D6", 12}, {"D7", 13}, {"D8", 15},
};
static Servo servo;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
static const char *CARTE = "ESP32-C3 Super Mini";
static const Broche P[] = {
  {"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}, {"10", 10}, {"20", 20}, {"21", 21},
};
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
static const char *CARTE = "ESP32-S2 Mini";
static const Broche P[] = {
  {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}, {"9", 9}, {"10", 10},
  {"11", 11}, {"12", 12}, {"13", 13}, {"14", 14}, {"15", 15}, {"16", 16}, {"17", 17}, {"18", 18}, {"21", 21},
  {"33", 33}, {"34", 34}, {"35", 35}, {"36", 36}, {"37", 37}, {"38", 38}, {"39", 39}, {"40", 40},
};
#elif defined(ESP32)
static const char *CARTE = "ESP32 (WROOM)";
static const Broche P[] = {
  {"D2", 2}, {"D4", 4}, {"D5", 5}, {"D12", 12}, {"D13", 13}, {"D14", 14}, {"D15", 15}, {"D16", 16}, {"D17", 17},
  {"D18", 18}, {"D19", 19}, {"D21", 21}, {"D22", 22}, {"D23", 23}, {"D25", 25}, {"D26", 26}, {"D27", 27},
  {"D32", 32}, {"D33", 33},
};
#else
#error "Outil prévu pour les cartes POCL (ESP8266, ESP32, ESP32-C3, ESP32-S2)"
#endif
static const int NP = sizeof(P) / sizeof(P[0]);

static int actif = -1;  // broche qui envoie les impulsions (indice dans P)

static void lache() {
  if (actif < 0) return;
#if defined(ESP8266)
  servo.detach();
#else
  ledcDetach(P[actif].gpio);
#endif
  pinMode(P[actif].gpio, INPUT);
  actif = -1;
}
static void impulsion(int i, int us) {
  us = constrain(us, 500, 2500);
  if (actif != i) {
    lache();
#if defined(ESP8266)
    servo.attach(P[i].gpio, 500, 2500, us);
#else
    if (!ledcAttach(P[i].gpio, 50, 14)) Serial.printf("   (broche %s : impossible de créer le signal)\n", P[i].nom);
#endif
    actif = i;
  }
#if defined(ESP8266)
  servo.writeMicroseconds(us);
#else
  ledcWrite(P[i].gpio, (uint32_t)us * 16383UL / 20000UL);
#endif
}
static int angleUs(int deg) { return 544 + (2400 - 544) * constrain(deg, 0, 180) / 180; }

// Lecture d'une ligne tapée au moniteur série (sans bloquer)
static String ligne;
static bool lireLigne(String &out) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!ligne.length()) continue;
      out = ligne;
      out.trim();
      ligne = "";
      return true;
    }
    if (ligne.length() < 30) ligne += c;
  }
  return false;
}

static int trouveBroche(String s) {  // « D5 » = nom écrit sur la carte ; « p 14 » ou « GPIO14 » = numéro de GPIO
  s.trim();
  s.toUpperCase();
  bool gpio = false;
  if (s.startsWith("P ")) {
    s = s.substring(2);
    gpio = true;
  } else if (s.startsWith("GPIO")) {
    s = s.substring(4);
    gpio = true;
  }
  s.trim();
  if (!s.length()) return -1;
  for (int i = 0; i < NP; i++)
    if (gpio ? (isDigit(s[0]) && s.toInt() == P[i].gpio) : s == P[i].nom) return i;
  return -1;
}

static void aide() {
  Serial.println();
  Serial.printf("=== teste_servo — %s ===\n", CARTE);
  Serial.println(F("Servo : marron/noir = GND, rouge = 5V, orange/jaune = signal sur la broche à tester."));
  Serial.println(F("Balayage : chaque broche envoie 0°, 180° puis 90° ; note celle qui fait bouger le servo."));
  Serial.println(F("Commandes (puis Entrée) : D5 ou « p 14 » = rester sur une broche, 90 = angle,"));
  Serial.println(F("1500 = impulsion en µs, tout = relancer le balayage."));
  Serial.println();
}

static bool balayage = true;
static int fixe = -1;     // broche choisie par l'utilisateur
static int etape = 0, courante = 0;
static uint32_t tEtape = 0;

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  delay(1500);
  aide();
}

void loop() {
  String l;
  if (lireLigne(l)) {
    String m = l;
    m.toLowerCase();
    int b = trouveBroche(l);
    bool nombre = l.length() && isDigit(l[0]);
    if (m == "tout" || m == "t") {
      balayage = true;
      fixe = -1;
      etape = 0;
      courante = 0;
      lache();
      Serial.println(F("-> balayage de toutes les broches"));
    } else if (!nombre && b >= 0) {
      balayage = false;
      fixe = b;
      impulsion(b, angleUs(90));
      Serial.printf("-> broche %s (GPIO %d) : 90°. Tape un angle (0 à 180) ou une impulsion (500 à 2500 µs).\n",
                    P[b].nom, P[b].gpio);
    } else if (nombre) {
      int v = l.toInt();
      if (fixe < 0) {
        Serial.println(F("Choisis d'abord une broche (ex. D5 ou « p 14 »)."));
      } else if (v <= 180) {
        impulsion(fixe, angleUs(v));
        Serial.printf("-> %s : %d° (%d µs)\n", P[fixe].nom, v, angleUs(v));
      } else if (v >= 500 && v <= 2500) {
        impulsion(fixe, v);
        Serial.printf("-> %s : %d µs\n", P[fixe].nom, v);
      } else {
        Serial.println(F("Angle de 0 à 180, ou impulsion de 500 à 2500 µs."));
      }
    } else {
      Serial.printf("Commande inconnue : « %s »\n", l.c_str());
      aide();
    }
  }

  if (!balayage) return;
  uint32_t now = millis();
  static const int ANGLES[3] = {0, 180, 90};
  static const uint16_t DUREES[3] = {700, 900, 700};
  if (etape == 0 || now - tEtape >= DUREES[etape - 1]) {
    if (etape == 3) {  // broche suivante
      lache();
      delay(300);
      courante = (courante + 1) % NP;
      if (courante == 0) Serial.println(F("--- fin du tour ; on recommence (tape une broche pour t'arrêter dessus) ---"));
      etape = 0;
    }
    if (etape == 0) Serial.printf("Broche %-4s (GPIO %2d) : 0° … 180° … 90°\n", P[courante].nom, P[courante].gpio);
    impulsion(courante, angleUs(ANGLES[etape]));
    etape++;
    tEtape = now;
  }
}
