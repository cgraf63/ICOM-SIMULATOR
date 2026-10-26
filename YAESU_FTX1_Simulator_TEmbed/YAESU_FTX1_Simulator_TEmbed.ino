/*
  Yaesu FTX-1 CAT-Simulator für LilyGO T-Embed (ESP32-S3)
  -------------------------------------------------------
  Simuliert einen Yaesu FTX-1 am USB-C-Port. Der PC (z. B. Shack-Server)
  sieht einen normalen COM-Port und spricht Yaesu-CAT wie mit einem echten
  Gerät: ASCII-Befehle, jeder mit ";" abgeschlossen, z. B. "FA014074000;".

  Bedienung:
    Knopf drücken  schaltet weiter, was der Drehknopf verstellt:
                   FREQ -> SCHRITT -> MODE -> BAND -> FREQ
    Knopf drehen   FREQ:    Frequenz
                   SCHRITT: Schrittweite 10 Hz ... 100 kHz
                   MODE:    LSB, USB, DATA-U, DATA-L, CW-U, AM, FM, FM-N, RTTY-L, PSK, C4FM
                   BAND:    160 m ... 6 m, 2 m, 70 cm
    Nach 8 s ohne Bedienung springt der Knopf zurück auf FREQ.
    Unten rechts erscheint "KNOPF", solange der Knopf gedrückt ist.
    Änderungen am Knopf meldet der Simulator von sich aus, wenn das Programm
    Auto-Information eingeschaltet hat (AI1;), wie ein echter FTX-1.
  LED-Ring:  grün = RX, rot = TX, kurz blau = CAT-Befehl empfangen

  Arduino IDE – Einstellungen (Werkzeuge):
    Boardpaket: "esp32 by Espressif Systems" Version 3.x
    Board: "ESP32S3 Dev Module"
    USB CDC On Boot: Enabled
    USB Mode: Hardware CDC and JTAG
    Flash Size: 16MB, PSRAM: OPI PSRAM
  Bibliotheken (Bibliotheksverwalter):
    "GFX Library for Arduino" (moononournation), Version 1.5 oder neuer
    "FastLED"

  Unterstützte CAT-Befehle (FTX-1-Format):
    ID  Geräte-ID            PS  Einschaltzustand     AI  Auto-Information
    FA/FB  Frequenz MAIN/SUB (9 Stellen)              MD0/MD1  Mode MAIN/SUB
    IF/OI  Statusinfo MAIN/SUB (30 Zeichen)           VS  VFO-Auswahl
    FT  Sende-VFO (Split)    ST  Split                TX  Senden/Empfangen
    SM0/SM1  S-Meter         RM  Messwerte            PC  Sendeleistung
    AG/RG/SQ  NF/HF/Squelch  NA  Schmal               BS  Bandwahl
  Unbekannte oder falsch aufgebaute Befehle werden mit "?;" beantwortet.

  Hinweis: Der USB-Port ist exklusiv für CAT. Keine Serial.print()-Ausgaben
  einbauen, sonst landet Text im CAT-Datenstrom.
*/

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FastLED.h>

// Prüfen, ob die Board-Einstellungen stimmen
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "ESP32-Boardpaket 3.x nötig: Boards Manager -> esp32 by Espressif Systems -> Version 3.x installieren"
#endif
#if !ARDUINO_USB_CDC_ON_BOOT
#error "Werkzeuge -> USB CDC On Boot -> Enabled einstellen (sonst ist Serial nicht der USB-Port)"
#endif

// ===================== Konfiguration =====================
#define CAT_SERIAL   Serial     // USB-C-Port (native USB)
#define CAT_BAUD     38400      // bei nativem USB ohne Bedeutung
#define RADIO_BRAND  "YAESU"
#define RADIO_NAME   "FTX-1"
// Antwort auf "ID;". UNGEPRÜFT: den richtigen Wert im FTX-1 CAT-Handbuch
// unter Befehl "ID" nachsehen und hier eintragen (4 Ziffern).
#define RADIO_ID     "0840"
#define PO_MAX_W     100        // Sendeleistung bei 100 % (SPA-1 = 100 W, Field-Head = 10 W)

#define SCREEN_ROTATION  1      // 1 oder 3, je nachdem wie herum das Display stehen soll
#define ENC_REVERSE      false  // true, falls die Drehrichtung vertauscht ist
#define ENC_DIV          4      // Zustandswechsel pro Rastung (4 oder 2)
#define LED_BRIGHTNESS   40     // 0..255

// ===================== Pins T-Embed =====================
#define PIN_POWER_ON   46
#define PIN_LCD_BL     15
#define PIN_LCD_MOSI   11
#define PIN_LCD_SCLK   12
#define PIN_LCD_CS     10
#define PIN_LCD_DC     13
#define PIN_LCD_RST     9
#define PIN_ENC_A       2
#define PIN_ENC_B       1
#define PIN_ENC_BTN     0
#define USE_LEDS        1       // 0 = LED-Ring nicht ansteuern
#define PIN_LED_DATA   42
#define PIN_LED_CLK    45
#define NUM_LEDS        7

// ===================== Display =====================
Arduino_DataBus* bus   = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, GFX_NOT_DEFINED);
Arduino_GFX*     panel = new Arduino_ST7789(bus, PIN_LCD_RST, 0, true, 170, 320, 35, 0, 35, 0);
Arduino_Canvas*  gfx   = new Arduino_Canvas(170, 320, panel, 0, 0, SCREEN_ROTATION);   // flimmerfrei

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
const uint16_t C_BG     = RGB(0, 0, 0);
const uint16_t C_TEXT   = RGB(255, 255, 255);
const uint16_t C_DIM    = RGB(120, 120, 120);
const uint16_t C_DARK   = RGB(40, 40, 40);
const uint16_t C_ACCENT = RGB(255, 160, 0);
const uint16_t C_GREEN  = RGB(40, 200, 80);
const uint16_t C_RED    = RGB(230, 40, 40);
const uint16_t C_BLUE   = RGB(40, 120, 230);

CRGB leds[NUM_LEDS];

// Frequenzbereich, den FA/FB annehmen
const uint32_t FREQ_LOW  = 30000UL;
const uint32_t FREQ_HIGH = 470000000UL;

// ===================== Gerätezustand =====================
struct Vfo {
  uint32_t freq;
  char     mode;    // Yaesu-Modecode, siehe modeName()
};

Vfo vfo[2] = {
  {14074000UL, 'C'},   // MAIN / VFO A: 20 m FT8, DATA-U
  { 7074000UL, '2'}    // SUB  / VFO B: 40 m, USB
};
uint8_t  curVfo  = 0;       // VS: 0 = MAIN, 1 = SUB
uint8_t  txVfo   = 0;       // FT: 0 = MAIN sendet, 1 = SUB sendet (Split)
bool     ptt     = false;
uint8_t  txState = 0;       // TX: 0 = RX, 1 = TX per CAT, 2 = TX
bool     autoInfo = false;  // AI
uint8_t  narrow[2] = {0, 0};
uint16_t afGain[2] = {100, 100}, rfGain[2] = {255, 255}, sqLevel[2] = {0, 0};
uint16_t powerW  = PO_MAX_W;
uint16_t sValue  = 70;      // S-Meter intern 0..241 (120 = S9)

// Für die Anzeige
char     lastCmd[20] = "";
uint32_t cmdCount = 0;
volatile bool dirty = true;
unsigned long flashUntil = 0;

const uint32_t bandFreqs[] = {
  1840000UL, 3573000UL, 7074000UL, 10136000UL, 14074000UL, 18100000UL,
  21074000UL, 24915000UL, 28074000UL, 50313000UL, 144174000UL, 432174000UL
};
const int NUM_BANDS = sizeof(bandFreqs) / sizeof(bandFreqs[0]);

const uint32_t steps[] = {10, 100, 1000, 10000, 100000};
const char*    stepNames[] = {"10 Hz", "100 Hz", "1 kHz", "10 kHz", "100 kHz"};
const int      NUM_STEPS = 5;
int stepIdx = 2;

const char modeCycle[] = {'1', '2', 'C', '8', '3', '5', '4', 'B', '6', 'E', 'H'};
const int  NUM_MODES = sizeof(modeCycle);

Vfo& cur() { return vfo[curVfo]; }

const char* modeName(char m) {
  switch (m) {
    case '1': return "LSB";     case '2': return "USB";     case '3': return "CW-U";
    case '4': return "FM";      case '5': return "AM";      case '6': return "RTTY-L";
    case '7': return "CW-L";    case '8': return "DATA-L";  case '9': return "RTTY-U";
    case 'A': return "DATA-FM"; case 'B': return "FM-N";    case 'C': return "DATA-U";
    case 'D': return "AM-N";    case 'E': return "PSK";     case 'F': return "DATA-FMN";
    case 'H': return "C4FM-DN"; case 'I': return "C4FM-VW";
    default:  return nullptr;
  }
}

// ===================== Senden =====================
void sendStr(const char* s) { CAT_SERIAL.write((const uint8_t*)s, strlen(s)); }
void err() { sendStr("?;"); }

void sendFreq(uint8_t side) {
  char b[16];
  sprintf(b, "F%c%09lu;", side ? 'B' : 'A', (unsigned long)vfo[side].freq);
  sendStr(b);
}

void sendMode(uint8_t side) {
  char b[8];
  sprintf(b, "MD%u%c;", side, vfo[side].mode);
  sendStr(b);
}

// IF (MAIN) bzw. OI (SUB): 30 Zeichen
// IF P1(5) P2(9) P3(5) P4 P5 P6 P7 P8 P9(2) P10 ;
void sendInfo(const char* cmd, uint8_t side) {
  char b[40];
  // Kanal 00000, Frequenz, Clarifier +0000, RX-Clar 0, TX-Clar 0,
  // Mode, VFO/Memory 0, CTCSS 0, 00, Shift 0
  sprintf(b, "%s00000%09lu+000000%c00000;", cmd, (unsigned long)vfo[side].freq, vfo[side].mode);
  sendStr(b);
}

// Auto-Information: Änderungen von selbst melden, wenn das Programm AI1 gesetzt hat
void autoFreq() { if (autoInfo) sendFreq(curVfo); }
void autoMode() { if (autoInfo) sendMode(curVfo); }

// ===================== Simulierte Messwerte (0..255) =====================
uint16_t sMeter255() { return ptt ? 0 : (uint16_t)((uint32_t)sValue * 255 / 241); }
uint16_t poMeter255() { return ptt ? (uint16_t)((uint32_t)powerW * 255 / PO_MAX_W) : 0; }

uint16_t meterValue(char p1) {
  switch (p1) {
    case '1': return sMeter255();                          // S MAIN
    case '2': return sMeter255();                          // S SUB
    case '3': return 0;                                    // COMP
    case '4': return ptt ? random(0, 60) : 0;              // ALC
    case '5': return poMeter255();                         // PO
    case '6': return ptt ? 20 + random(0, 10) : 0;         // SWR
    case '7': return ptt ? 120 + random(0, 20) : 10;       // IDD
    case '8': return 190 + random(0, 3);                   // VDD
    default:  return 0;
  }
}

// ===================== Befehlsverarbeitung =====================
bool allDigits(const char* s, int n) {
  for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

// Liest P1 = 0/1 (MAIN/SUB). Gibt -1 bei Fehler zurück.
int side(const char* p) { return (p[0] == '0' || p[0] == '1') ? p[0] - '0' : -1; }

// 3-stelliger Pegel mit MAIN/SUB-Auswahl: XX P1 [nnn]
void level3(const char* cmd, const char* p, int len, uint16_t* val) {
  int s = len >= 1 ? side(p) : -1;
  if (s < 0) { err(); return; }
  char b[12];
  if (len == 1) { sprintf(b, "%s%d%03u;", cmd, s, val[s]); sendStr(b); }
  else if (len == 4 && allDigits(p + 1, 3)) { val[s] = min(atoi(p + 1), 255); dirty = true; }
  else err();
}

void handleCommand(char* c) {
  int n = strlen(c);
  if (n < 2) { err(); return; }
  for (int i = 0; i < n; i++) c[i] = toupper(c[i]);

  strncpy(lastCmd, c, sizeof(lastCmd) - 2);
  lastCmd[sizeof(lastCmd) - 2] = 0;
  strcat(lastCmd, ";");
  cmdCount++;
  dirty = true;
  flashUntil = millis() + 60;

  char cmd[3] = {c[0], c[1], 0};
  const char* p = c + 2;
  int pl = n - 2;
  char b[40];

  if (!strcmp(cmd, "ID")) {
    if (pl == 0) { sprintf(b, "ID%s;", RADIO_ID); sendStr(b); } else err();
  }
  else if (!strcmp(cmd, "PS")) {
    if (pl == 0) sendStr("PS1;");
    else if (pl != 1) err();                 // PS0/PS1: Simulator bleibt eingeschaltet
  }
  else if (!strcmp(cmd, "AI")) {
    if (pl == 0) { sprintf(b, "AI%d;", autoInfo ? 1 : 0); sendStr(b); }
    else if (pl == 1 && (p[0] == '0' || p[0] == '1')) autoInfo = (p[0] == '1');
    else err();
  }
  else if (!strcmp(cmd, "FA") || !strcmp(cmd, "FB")) {
    uint8_t s = (cmd[1] == 'B') ? 1 : 0;
    if (pl == 0) sendFreq(s);
    else if (pl == 9 && allDigits(p, 9)) {
      uint32_t f = strtoul(p, nullptr, 10);
      if (f >= FREQ_LOW && f <= FREQ_HIGH) vfo[s].freq = f;
      else err();
    } else err();
  }
  else if (!strcmp(cmd, "MD")) {
    int s = pl >= 1 ? side(p) : -1;
    if (s < 0) err();
    else if (pl == 1) sendMode(s);
    else if (pl == 2 && modeName(p[1])) vfo[s].mode = p[1];
    else err();
  }
  else if (!strcmp(cmd, "IF")) {
    if (pl == 0) sendInfo("IF", 0); else err();
  }
  else if (!strcmp(cmd, "OI")) {
    if (pl == 0) sendInfo("OI", 1); else err();
  }
  else if (!strcmp(cmd, "VS")) {
    if (pl == 0) { sprintf(b, "VS%d;", curVfo); sendStr(b); }
    else if (pl == 1 && side(p) >= 0) curVfo = side(p);
    else err();
  }
  else if (!strcmp(cmd, "FT")) {
    if (pl == 0) { sprintf(b, "FT%d;", txVfo); sendStr(b); }
    else if (pl == 1 && side(p) >= 0) txVfo = side(p);
    else err();
  }
  else if (!strcmp(cmd, "ST")) {
    if (pl == 0) { sprintf(b, "ST%d;", txVfo); sendStr(b); }
    else if (pl == 1 && side(p) >= 0) txVfo = side(p);
    else err();
  }
  else if (!strcmp(cmd, "TX")) {
    if (pl == 0) { sprintf(b, "TX%d;", txState); sendStr(b); }
    else if (pl == 1 && p[0] >= '0' && p[0] <= '2') { txState = p[0] - '0'; ptt = txState != 0; }
    else err();
  }
  else if (!strcmp(cmd, "SM")) {
    int s = pl == 1 ? side(p) : -1;
    if (s < 0) err();
    else { sprintf(b, "SM%d%03u;", s, sMeter255()); sendStr(b); }
  }
  else if (!strcmp(cmd, "RM")) {
    if (pl == 1 && p[0] >= '1' && p[0] <= '8') { sprintf(b, "RM%c%03u000;", p[0], meterValue(p[0])); sendStr(b); }
    else err();
  }
  else if (!strcmp(cmd, "PC")) {
    // FTX-1: P1 = 1 Field-Head (0.5–10 W), 2 SPA-1 (5–100 W)
    if (pl == 0) {
      if (PO_MAX_W > 10) sprintf(b, "PC2%03u;", powerW);
      else               sprintf(b, "PC1%03u;", powerW * 10);   // Zehntel-Watt
      sendStr(b);
    }
    else if (pl == 4 && (p[0] == '1' || p[0] == '2') && allDigits(p + 1, 3)) {
      int v = atoi(p + 1);
      powerW = constrain(p[0] == '1' ? v / 10 : v, 1, PO_MAX_W);
    }
    else err();
  }
  else if (!strcmp(cmd, "AG")) level3("AG", p, pl, afGain);
  else if (!strcmp(cmd, "RG")) level3("RG", p, pl, rfGain);
  else if (!strcmp(cmd, "SQ")) level3("SQ", p, pl, sqLevel);
  else if (!strcmp(cmd, "NA")) {
    int s = pl >= 1 ? side(p) : -1;
    if (s < 0) err();
    else if (pl == 1) { sprintf(b, "NA%d%d;", s, narrow[s]); sendStr(b); }
    else if (pl == 2 && (p[1] == '0' || p[1] == '1')) narrow[s] = p[1] - '0';
    else err();
  }
  else if (!strcmp(cmd, "BS")) {
    // BS P1 P2P2: Bandwahl MAIN/SUB, 00 = 1.8 MHz ... 10 = 50 MHz
    int s = pl == 3 ? side(p) : -1;
    if (s < 0 || !allDigits(p + 1, 2)) { err(); return; }
    int band = atoi(p + 1);
    const uint8_t bandMap[] = {0, 1, 0xFF, 2, 3, 4, 5, 6, 7, 8, 9};   // 02 = 5 MHz nicht in der Liste
    if (band > 10 || bandMap[band] == 0xFF) { err(); return; }
    vfo[s].freq = bandFreqs[bandMap[band]];
    vfo[s].mode = (vfo[s].freq < 10000000UL) ? '1' : '2';
  }
  else err();
}

// ===================== Empfang =====================
char rxBuf[64];
int  rxLen = 0;

void feedByte(uint8_t b) {
  if (b == ';') {
    rxBuf[rxLen] = 0;
    if (rxLen > 0) handleCommand(rxBuf);
    rxLen = 0;
    return;
  }
  if (b < 0x20 || b > 0x7E) return;          // CR/LF und Steuerzeichen ignorieren
  if (rxLen < (int)sizeof(rxBuf) - 1) rxBuf[rxLen++] = (char)b;
  else rxLen = 0;                            // zu lang: verwerfen
}

// ===================== Drehgeber =====================
volatile int32_t encCount = 0;
volatile uint8_t encState = 0;
const int8_t encTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR encISR() {
  encState = ((encState << 2) | (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B)) & 0x0F;
  encCount += encTable[encState];
}

int32_t encLast = 0;
bool pendingFreqBroadcast = false;
unsigned long lastBroadcast = 0;

// Was der Drehknopf gerade verstellt. Jeder Druck auf den Knopf schaltet weiter.
enum KnobFn { FN_FREQ, FN_STEP, FN_MODE, FN_BAND, NUM_FN };
const char* fnNames[] = {"FREQ", "SCHRITT", "MODE", "BAND"};
int knobFn = FN_FREQ;
unsigned long lastKnobActivity = 0;
#define KNOB_RETURN_MS 8000      // nach 8 s ohne Bedienung zurück auf Frequenz

void nextBand(int dir) {
  int idx;
  if (dir > 0) {                 // nächstes Band oberhalb, nach 70 cm wieder 160 m
    idx = 0;
    for (int i = 0; i < NUM_BANDS; i++)
      if (bandFreqs[i] > cur().freq + 500000UL) { idx = i; break; }
  } else {                       // nächstes Band unterhalb, vor 160 m wieder 70 cm
    idx = NUM_BANDS - 1;
    for (int i = NUM_BANDS - 1; i >= 0; i--)
      if (bandFreqs[i] + 500000UL < cur().freq) { idx = i; break; }
  }
  cur().freq = bandFreqs[idx];
  cur().mode = (cur().freq < 10000000UL) ? '1' : '2';      // unter 10 MHz LSB
  autoFreq();
  autoMode();
  dirty = true;
}

void nextMode(int dir) {
  int idx = 0;
  for (int i = 0; i < NUM_MODES; i++)
    if (modeCycle[i] == cur().mode) { idx = i; break; }
  idx = (idx + (dir > 0 ? 1 : NUM_MODES - 1)) % NUM_MODES;
  cur().mode = modeCycle[idx];
  autoMode();
  dirty = true;
}

void handleEncoder() {
  int32_t c = encCount;
  int32_t det = (c - encLast) / ENC_DIV;
  if (det == 0) return;
  encLast += det * ENC_DIV;
  if (ENC_REVERSE) det = -det;
  lastKnobActivity = millis();
  dirty = true;

  switch (knobFn) {
    case FN_FREQ: {
      int64_t f = (int64_t)cur().freq + (int64_t)det * steps[stepIdx];
      f = (f / steps[stepIdx]) * steps[stepIdx];        // auf Schrittweite runden
      if (f < (int64_t)FREQ_LOW)  f = FREQ_LOW;
      if (f > (int64_t)FREQ_HIGH) f = FREQ_HIGH;
      cur().freq = (uint32_t)f;
      pendingFreqBroadcast = true;
      break;
    }
    case FN_STEP:
      stepIdx = constrain(stepIdx - (int)det, 0, NUM_STEPS - 1);   // rechts drehen = feiner
      break;
    case FN_MODE:
      for (int i = 0; i < abs(det); i++) nextMode(det);
      break;
    case FN_BAND:
      for (int i = 0; i < abs(det); i++) nextBand(det);
      break;
  }
}

// ===================== Taste =====================
bool btnRaw = false, btnStable = false;
unsigned long btnRawChange = 0;

void handleButton() {
  unsigned long now = millis();
  bool raw = (digitalRead(PIN_ENC_BTN) == LOW);
  if (raw != btnRaw) { btnRaw = raw; btnRawChange = now; dirty = true; }

  if (now - btnRawChange > 30 && btnStable != btnRaw) {   // entprellt
    btnStable = btnRaw;
    if (btnStable) {                                       // beim Drücken weiterschalten
      knobFn = (knobFn + 1) % NUM_FN;
      lastKnobActivity = now;
      dirty = true;
    }
  }

  if (knobFn != FN_FREQ && now - lastKnobActivity > KNOB_RETURN_MS) {
    knobFn = FN_FREQ;
    dirty = true;
  }
}

// ===================== Anzeige =====================
const char* bandName(uint32_t f) {
  if (f >=   1810000 && f <=   2000000) return "160m";
  if (f >=   3500000 && f <=   3800000) return "80m";
  if (f >=   5351500 && f <=   5366500) return "60m";
  if (f >=   7000000 && f <=   7200000) return "40m";
  if (f >=  10100000 && f <=  10150000) return "30m";
  if (f >=  14000000 && f <=  14350000) return "20m";
  if (f >=  18068000 && f <=  18168000) return "17m";
  if (f >=  21000000 && f <=  21450000) return "15m";
  if (f >=  24890000 && f <=  24990000) return "12m";
  if (f >=  28000000 && f <=  29700000) return "10m";
  if (f >=  50000000 && f <=  52000000) return "6m";
  if (f >= 144000000 && f <= 146000000) return "2m";
  if (f >= 430000000 && f <= 440000000) return "70cm";
  return "GEN";
}

void sText(uint16_t v, char* out) {          // S-Meter-Wert -> "S7" / "S9+20"
  if (v <= 120) sprintf(out, "S%d", (int)((v + 6) / 13.33f));
  else          sprintf(out, "S9+%d", (int)((v - 120) * 60 / 121));
}

void pill(int x, int y, const char* txt, uint16_t bg, uint16_t fg) {
  int w = strlen(txt) * 12 + 10;
  gfx->fillRoundRect(x, y, w, 20, 5, bg);
  gfx->setTextColor(fg);
  gfx->setTextSize(2);
  gfx->setCursor(x + 5, y + 3);
  gfx->print(txt);
}

void drawDisplay() {
  char buf[32];
  gfx->fillScreen(C_BG);

  // --- Kopfzeile: MAIN/SUB, Mode, Split, Band, RX/TX
  pill(2, 2, curVfo ? "SUB" : "MAIN", C_DARK, C_TEXT);
  const char* mn = modeName(cur().mode);
  pill(68, 2, mn ? mn : "?", C_ACCENT, C_BG);
  if (txVfo != curVfo) pill(178, 2, "SPL", C_BLUE, C_TEXT);
  gfx->setTextColor(C_DIM); gfx->setTextSize(2);
  gfx->setCursor(226, 5); gfx->print(bandName(cur().freq));
  pill(276, 2, ptt ? "TX" : "RX", ptt ? C_RED : C_GREEN, C_TEXT);

  // --- Frequenz, rechtsbündig. Ab 100 MHz kleinere Schrift, damit sie passt.
  uint32_t f = cur().freq;
  sprintf(buf, "%lu.%03lu.%03lu", (unsigned long)(f / 1000000), (unsigned long)((f / 1000) % 1000), (unsigned long)(f % 1000));
  int len = strlen(buf);
  const int ts = (len > 10) ? 4 : 5;
  const int right = 314, cw = 6 * ts, fy = 32 + (5 - ts) * 4;
  gfx->setTextSize(ts);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(right - len * cw, fy);
  gfx->print(buf);

  // Markierung unter der Stelle, die der Knopf verstellt
  const int stepPos[] = {1, 2, 4, 5, 6};        // Zeichen von rechts (0 = 1-Hz-Stelle)
  int cx = right - (stepPos[stepIdx] + 1) * cw;
  gfx->fillRect(cx, fy + 8 * ts, cw - 5, 2, C_ACCENT);

  // --- Infozeile: was der Knopf gerade verstellt
  if (knobFn == FN_FREQ) {
    gfx->setTextSize(1);
    gfx->setTextColor(C_DIM);
    sprintf(buf, "Schritt %s", stepNames[stepIdx]);
    gfx->setCursor(4, 82); gfx->print(buf);
  } else {
    if (knobFn == FN_STEP) sprintf(buf, "%s %s", fnNames[knobFn], stepNames[stepIdx]);
    else                   sprintf(buf, "Knopf: %s", fnNames[knobFn]);
    pill(2, 74, buf, C_ACCENT, C_BG);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  strcpy(buf, autoInfo ? "CAT  AI ein" : "CAT");
  gfx->setCursor(316 - strlen(buf) * 6, 82); gfx->print(buf);

  // --- Meter: S-Meter (RX) oder Leistung (TX)
  const int mx = 30, mw = 280, my = 108, mh = 14, seg = 7;
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(4, my);
  gfx->print(ptt ? "PO" : "S");

  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  if (!ptt) {
    const char* lbl[] = {"1", "3", "5", "7", "9", "+20", "+40", "+60"};
    const int   val[] = {13, 40, 67, 93, 120, 160, 200, 241};
    for (int i = 0; i < 8; i++) {
      int x = (i == 7) ? mx + mw - 18 : mx + val[i] * mw / 241 - 3;
      gfx->setCursor(x, my - 11); gfx->print(lbl[i]);
    }
  } else {
    const char* lbl[] = {"0", "25", "50", "75", "100%"};
    for (int i = 0; i < 5; i++) { gfx->setCursor(mx + i * mw / 4 - (i == 4 ? 24 : 3), my - 11); gfx->print(lbl[i]); }
  }

  uint16_t v   = ptt ? poMeter255() : sValue;
  uint16_t vmx = ptt ? 255 : 241;
  int lit = (int)v * (mw / seg) / vmx;
  for (int i = 0; i < mw / seg; i++) {
    uint16_t c;
    if (i >= lit) c = C_DARK;
    else if (ptt) c = C_RED;
    else c = (i * seg * 241 / mw <= 120) ? C_GREEN : C_RED;
    gfx->fillRect(mx + i * seg, my, seg - 2, mh, c);
  }

  if (!ptt) sText(v, buf); else sprintf(buf, "%u W", powerW);
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(316 - strlen(buf) * 12, my + 20);
  gfx->print(buf);

  // --- Gerätename unter dem S-Meter
  sprintf(buf, "%s %s", RADIO_BRAND, RADIO_NAME);
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(4, my + 26);
  gfx->print(buf);

  // --- Statuszeile
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  if (!cmdCount) strcpy(buf, "warte auf CAT ...");
  else           snprintf(buf, 21, "%s", lastCmd);
  gfx->setCursor(4, 160); gfx->print(buf);
  sprintf(buf, "Befehle %lu", (unsigned long)cmdCount);
  gfx->setCursor(4 + 22 * 6, 160); gfx->print(buf);

  // Kontrollanzeige: leuchtet, solange der Knopf gedrückt ist
  if (btnRaw) {
    gfx->setTextColor(C_ACCENT);
    gfx->setCursor(316 - 5 * 6, 160); gfx->print("KNOPF");
  }

  gfx->flush();
}

// ===================== LED-Ring =====================
void updateLeds() {
  if (!USE_LEDS) return;
  static CRGB lastBase = CRGB::Black;
  static bool lastFlash = false;
  CRGB base   = ptt ? CRGB(255, 0, 0) : CRGB(0, 160, 40);
  bool flash  = millis() < flashUntil;
  if (base == lastBase && flash == lastFlash) return;
  lastBase = base; lastFlash = flash;
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = base;
  if (flash) leds[0] = CRGB(0, 80, 255);
  FastLED.show();
}

// ===================== Setup / Loop =====================
unsigned long nextSmeter = 0, nextDisplay = 0;

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);       // Display und Peripherie einschalten

  CAT_SERIAL.begin(CAT_BAUD);
#if ARDUINO_USB_MODE == 1
  CAT_SERIAL.setTxTimeoutMs(0);           // nicht blockieren, wenn kein PC verbunden ist
#endif

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  gfx->begin();
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

#if USE_LEDS
  FastLED.addLeds<APA102, PIN_LED_DATA, PIN_LED_CLK, BGR>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  // Erste LED-Ausgabe schon hier: FastLED richtet dabei seine Hardware ein
  // und belegt GPIO 0 (Knopf). Der Knopf-Pin wird deshalb danach neu gesetzt.
  FastLED.show();
#endif

  randomSeed(esp_random());

  // Knopf-Pin nach dem Start von FastLED wieder als Eingang einrichten
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);

  drawDisplay();
}

void loop() {
  while (CAT_SERIAL.available()) feedByte(CAT_SERIAL.read());

  handleEncoder();
  handleButton();

  // Frequenzänderungen vom Knopf höchstens alle 100 ms melden
  if (pendingFreqBroadcast && millis() - lastBroadcast >= 100) {
    autoFreq();
    pendingFreqBroadcast = false;
    lastBroadcast = millis();
  }

  // S-Meter langsam schwanken lassen
  if (millis() >= nextSmeter) {
    nextSmeter = millis() + 300;
    sValue = constrain((int)sValue + (int)random(-12, 13), 40, 160);
    dirty = true;
  }

  // Anzeige bei Änderungen neu zeichnen, höchstens ca. 25x pro Sekunde
  if (dirty && millis() >= nextDisplay) {
    dirty = false;
    nextDisplay = millis() + 40;
    drawDisplay();
  }

  updateLeds();
}
