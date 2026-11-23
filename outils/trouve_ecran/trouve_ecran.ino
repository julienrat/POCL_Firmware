// =====================================================================
//  trouve_ecran — outil TEMPORAIRE : retrouver le câblage d'un écran
//  Nokia 5110 branché sur une Wemos D1 mini, sans ouvrir la boîte.
//
//  1. Rétroéclairage : chaque broche clignote à tour de rôle ; réponds « o »
//     quand la lumière de l'écran clignote. Elle reste ensuite allumée.
//  2. Câblage : les combinaisons CLK / DIN / DC / CE sont essayées une par une ;
//     l'écran devient NOIR un court instant quand c'est la bonne
//     -> appuie sur Entrée dès que tu le vois noircir.
//  3. Les derniers essais sont rejoués un par un, puis on cherche RST.
//
//  D1 et D2 (SDA / SCL) ne sont pas utilisées par l'outil.
//
//  Lancement :  ./flash.sh esp8266 --croquis outils/trouve_ecran
//  Ensuite :    ./flash.sh   (remet le firmware POCL, la configuration est conservée)
//
//  ATTENTION : les broches sont activées à tour de rôle. Si un relais (surtout
//  en 230 V), un moteur ou un ruban de LED est aussi branché, coupe leur
//  alimentation pendant le test.
// =====================================================================
#include <Arduino.h>
#ifndef ESP8266
#error "Outil prévu pour la Wemos D1 mini (ESP8266)"
#endif

struct Broche { const char *nom; uint8_t gpio; };
static const Broche P[] = {  // D1 et D2 (I2C) volontairement absentes
  {"D0", 16}, {"D3", 0}, {"D4", 2}, {"D5", 14}, {"D6", 12}, {"D7", 13}, {"D8", 15},
};
static const int NP = sizeof(P) / sizeof(P[0]);

static const uint16_t NOIR_MS = 150;   // durée du flash noir (augmenter si on le rate)
static const uint16_t BLANC_MS = 60;   // pause entre deux essais
static const uint16_t CLIGNE_MS = 350; // demi-période du clignotement du rétroéclairage

struct Essai { int8_t clk, din, dc, ce; };
static Essai hist[16];
static uint32_t nh = 0;
static Essai bon;
static uint8_t gClk, gDin, gDc, gCe;
static int blPin = -1;        // index de la broche du rétroéclairage (-1 = pas trouvée)
static uint8_t blOn = LOW;    // niveau qui allume le rétroéclairage

// ---------------------------------------------------------------------
//  Dialogue dans le moniteur série
// ---------------------------------------------------------------------
static void vide() {
  delay(30);
  while (Serial.available()) Serial.read();
}

// Attend une réponse ; renvoie sa première lettre ('\n' si juste Entrée).
// Marche aussi avec un moniteur qui n'envoie pas de fin de ligne (fin = 0,3 s de silence).
// Si cligne >= 0, la broche P[cligne] clignote pendant l'attente.
static char attendEn(int cligne) {
  String l;
  bool recu = false, etat = false;
  uint32_t t = 0, tc = millis();
  for (;;) {
    if (Serial.available()) {
      char c = Serial.read();
      recu = true;
      t = millis();
      if (c == '\n' || c == '\r') break;
      l += c;
    } else {
      if (recu && millis() - t > 300) break;
      if (cligne >= 0 && millis() - tc >= CLIGNE_MS) {
        tc = millis();
        etat = !etat;
        digitalWrite(P[cligne].gpio, etat ? HIGH : LOW);
      }
      delay(5);
    }
  }
  vide();
  l.trim();
  return l.length() ? tolower(l[0]) : '\n';
}
static char attend() { return attendEn(-1); }

// ---------------------------------------------------------------------
//  1. Rétroéclairage
// ---------------------------------------------------------------------
static void chercheRetroeclairage() {
  Serial.println(F("\n1) Rétroéclairage : chaque broche clignote à tour de rôle."));
  Serial.println(F("   Réponds « o » puis Entrée quand la lumière de l'écran clignote, sinon juste Entrée."));
  for (int i = 0; i < NP; i++) pinMode(P[i].gpio, INPUT);
  for (int p = 0; p < NP && blPin < 0; p++) {
    pinMode(P[p].gpio, OUTPUT);
    Serial.printf("  %s clignote : la lumière clignote-t-elle ? (o/Entrée) ", P[p].nom);
    if (attendEn(p) == 'o') blPin = p;
    else pinMode(P[p].gpio, INPUT);
  }
  if (blPin < 0) {
    Serial.println(F("   Pas de broche de rétroéclairage : BL est sans doute reliée à GND ou 3V3."));
    return;
  }
  digitalWrite(P[blPin].gpio, LOW);
  Serial.printf("   Rétroéclairage sur %s. Est-il ALLUMÉ maintenant ? (o/Entrée) ", P[blPin].nom);
  blOn = attend() == 'o' ? LOW : HIGH;
  digitalWrite(P[blPin].gpio, blOn);
  Serial.printf("   OK : il s'allume avec un niveau %s, on le laisse allumé.\n", blOn == LOW ? "BAS" : "HAUT");
}

// ---------------------------------------------------------------------
//  2. Écran Nokia 5110 (PCD8544) : SPI logiciel
// ---------------------------------------------------------------------
static void tout(uint8_t niveau) {  // toutes les broches sauf celle du rétroéclairage
  for (int i = 0; i < NP; i++) {
    if (i == blPin) continue;
    pinMode(P[i].gpio, OUTPUT);
    digitalWrite(P[i].gpio, niveau);
  }
}

static void envoie(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(gDin, (b & 0x80) ? HIGH : LOW);
    digitalWrite(gClk, HIGH);
    delayMicroseconds(1);
    digitalWrite(gClk, LOW);
    b <<= 1;
  }
}
static void commande(uint8_t b) { digitalWrite(gDc, LOW); digitalWrite(gCe, LOW); envoie(b); digitalWrite(gCe, HIGH); }
static void donnee(uint8_t b) { digitalWrite(gDc, HIGH); digitalWrite(gCe, LOW); envoie(b); digitalWrite(gCe, HIGH); }

// Toutes les broches à 0 puis à 1 : RST reçoit une impulsion où qu'elle soit,
// puis reste à 1 ; on initialise l'écran avec le câblage supposé.
static void demarre(const Essai &e, uint8_t mode) {
  tout(LOW);
  delay(3);
  tout(HIGH);
  delay(2);
  gClk = P[e.clk].gpio; gDin = P[e.din].gpio; gDc = P[e.dc].gpio; gCe = P[e.ce].gpio;
  digitalWrite(gClk, LOW);
  commande(0x21);  // jeu de commandes étendu
  commande(0xC8);  // contraste assez fort
  commande(0x04);  // coefficient de température
  commande(0x14);  // polarisation 1:48
  commande(0x20);  // jeu de commandes normal
  commande(mode);  // 0x09 = tout noir, 0x0C = affichage normal
}

static void bandes(const Essai &e) {  // bandes verticales de 6 pixels
  demarre(e, 0x0C);
  commande(0x80);
  commande(0x40);
  for (int i = 0; i < 504; i++) donnee((i / 6) % 2 ? 0x00 : 0xFF);
}

static void eteint() { tout(LOW); }  // RST à 0 : écran effacé

static bool utilise(int p, const Essai &e) { return p == e.clk || p == e.din || p == e.dc || p == e.ce || p == blPin; }

static void resultat(int rst) {
  Serial.println(F("\n=== Câblage de l'écran Nokia ==="));
  Serial.printf("  CLK -> %s\n  DIN -> %s\n  DC  -> %s\n  CE  -> %s\n", P[bon.clk].nom, P[bon.din].nom, P[bon.dc].nom, P[bon.ce].nom);
  Serial.printf("  RST -> %s\n", rst >= 0 ? P[rst].nom : "pas trouvée (peut-être reliée à 3V3)");
  if (blPin >= 0) Serial.printf("  BL  -> %s (s'allume avec un niveau %s)\n", P[blPin].nom, blOn == LOW ? "BAS" : "HAUT");
  else Serial.println(F("  BL  -> pas sur une broche (reliée à GND ou 3V3)"));
  Serial.println(F("Recopie ces broches dans la page POCL (Sorties › Écran Nokia 5110) ;"));
  if (blPin >= 0 && blOn == LOW) Serial.println(F("coche « Rétroéclairage actif à l'état BAS »."));
  Serial.println(F("Puis remets le firmware POCL :  ./flash.sh"));
}

// Le câblage est confirmé par des bandes, puis on cherche RST
static bool confirme() {
  bandes(bon);
  Serial.print(F("\nVois-tu des bandes verticales ? (o = oui, Entrée = non) "));
  if (attend() != 'o') return false;

  int rst = -1;
  Serial.println(F("\nRecherche de RST : l'écran va s'effacer quand on touchera la bonne broche."));
  for (int p = 0; p < NP && rst < 0; p++) {
    if (utilise(p, bon)) continue;
    bandes(bon);
    delay(600);
    digitalWrite(P[p].gpio, LOW);
    delay(400);
    digitalWrite(P[p].gpio, HIGH);
    Serial.printf("  %s : les bandes ont-elles disparu ? (o/Entrée) ", P[p].nom);
    if (attend() == 'o') rst = p;
  }
  bandes(bon);
  resultat(rst);
  return true;
}

// On rejoue les derniers essais un par un (du plus récent au plus ancien)
static bool rejoue() {
  uint32_t n = nh < 14 ? nh : 14;
  Serial.printf("\nStop ! On rejoue les %u derniers essais un par un.\n", (unsigned)n);
  for (uint32_t i = 1; i <= n; i++) {
    bon = hist[(nh - i) % 16];
    demarre(bon, 0x09);
    Serial.printf("  Essai %u/%u : l'écran est-il NOIR ? (o = oui, Entrée = non) ", (unsigned)i, (unsigned)n);
    if (attend() == 'o' && confirme()) return true;
  }
  eteint();
  Serial.println(F("Pas retrouvé : on reprend le balayage (appuie sur Entrée dès le flash noir)."));
  return false;
}

static uint32_t numero = 0, total = 0;
static bool essaie(int clk, int din, int dc, int ce) {
  Essai e = {(int8_t)clk, (int8_t)din, (int8_t)dc, (int8_t)ce};
  hist[nh % 16] = e;
  nh++;
  numero++;
  demarre(e, 0x09);
  delay(NOIR_MS);
  eteint();
  delay(BLANC_MS);
  if (numero % 50 == 0) Serial.printf("   %u essais sur %u...\n", (unsigned)numero, (unsigned)total);
  if (Serial.available()) {
    vide();
    return rejoue();
  }
  return false;
}

// passe 0 : câblages les plus courants (CLK sur D5 et DIN sur D7, le SPI de la carte)
// passe 1 : tous les autres
static bool balaye() {
  for (int passe = 0; passe < 2; passe++)
    for (int clk = 0; clk < NP; clk++) {
      if (clk == blPin) continue;
      for (int din = 0; din < NP; din++) {
        if (din == clk || din == blPin) continue;
        bool courant = P[clk].gpio == 14 && P[din].gpio == 13;
        if ((passe == 0) != courant) continue;
        for (int dc = 0; dc < NP; dc++) {
          if (dc == clk || dc == din || dc == blPin) continue;
          for (int ce = 0; ce < NP; ce++) {
            if (ce == clk || ce == din || ce == dc || ce == blPin) continue;
            if (essaie(clk, din, dc, ce)) return true;
          }
        }
      }
    }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  for (int i = 0; i < NP; i++) pinMode(P[i].gpio, INPUT);
  Serial.println(F("\n\n=== Recherche du câblage de l'écran Nokia (outil temporaire) ==="));
  Serial.println(F("Si un relais, un moteur ou des LED sont aussi branchés, coupe leur alimentation."));
  // On répète l'invite : le moniteur série s'ouvre souvent après le démarrage de la carte
  while (!Serial.available()) {
    Serial.println(F("Appuie sur Entrée pour commencer..."));
    for (int i = 0; i < 300 && !Serial.available(); i++) delay(10);
  }
  vide();

  chercheRetroeclairage();

  int n = NP - (blPin >= 0 ? 1 : 0);
  total = n * (n - 1) * (n - 2) * (n - 3);
  Serial.printf("\n2) Câblage : regarde l'écran. Dès qu'il devient NOIR, appuie sur Entrée.\n");
  Serial.printf("   %u combinaisons, les plus courantes d'abord (environ %u s au pire).\n",
                (unsigned)total, (unsigned)(total * (NOIR_MS + BLANC_MS + 10) / 1000));
  for (int tour = 0; tour < 3; tour++) {
    if (balaye()) return;
    Serial.println(F("\nToutes les combinaisons ont été essayées sans réponse. Vérifie que l'écran est"));
    Serial.println(F("bien alimenté (VCC sur 3V3, GND), ou augmente NOIR_MS. Entrée = recommencer."));
    attend();
    numero = 0;
  }
}

void loop() { delay(1000); }
