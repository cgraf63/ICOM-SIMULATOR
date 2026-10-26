/*
  CI-V Simulator für LilyGO T-Embed (ESP32-S3)
  --------------------------------------------
  Simuliert ein Icom-Funkgerät (Standard: IC-7300) am USB-C-Port.
  Der PC (z. B. Shack-Server) sieht einen normalen COM-Port und spricht CI-V
  wie mit einem echten Gerät.

  Bedienung:
    Knopf drücken  schaltet weiter, was der Drehknopf verstellt:
                   FREQ -> SCHRITT -> MODE -> BAND -> FREQ
    Knopf drehen   FREQ:    Frequenz (wird per Transceive gemeldet)
                   SCHRITT: Schrittweite 10 Hz ... 100 kHz
                   MODE:    LSB, USB, USB-D, CW, AM, FM, RTTY
                   BAND:    160 m ... 6 m
    Nach 8 s ohne Bedienung springt der Knopf zurück auf FREQ.
    Unten rechts erscheint "KNOPF", solange der Knopf gedrückt ist.
  LED-Ring:  grün = RX, rot = TX, kurz blau = CI-V-Frame empfangen

  Arduino IDE – Einstellungen (Werkzeuge):
    Boardpaket: "esp32 by Espressif Systems" Version 3.x
    Board: "ESP32S3 Dev Module"
    USB CDC On Boot: Enabled
    USB Mode: Hardware CDC and JTAG
    Flash Size: 16MB, PSRAM: OPI PSRAM
  Bibliotheken (Bibliotheksverwalter):
    "GFX Library for Arduino" (moononournation), Version 1.5 oder neuer
    "FastLED"

  Unterstützte CI-V-Befehle:
    00/05 Frequenz setzen, 03 lesen   01/06 Mode setzen, 04 lesen
    02 Bandgrenzen   07 VFO   0F Split   11 Abschwächer
    14 Pegel   15 Messwerte   16 Funktionen   19 00 Geräte-ID
    1A 06 Data-Mode   1C 00 PTT   1C 01 Tuner   25/26 Frequenz/Mode je VFO
  Unbekannte Befehle werden mit FA (NG) beantwortet.

  Hinweis: Der USB-Port ist exklusiv für CI-V. Keine Serial.print()-Ausgaben
  einbauen, sonst landet Text im CI-V-Datenstrom.
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
#define CIV_SERIAL   Serial     // USB-C-Port (native USB)
#define CIV_BAUD     19200      // bei nativem USB ohne Bedeutung
#define RADIO_ADDR   0x94       // IC-7300=0x94, IC-705=0xA4, IC-9700=0xA2, IC-7610=0x98
#define RADIO_NAME   "IC-7300"
#define CIV_ECHO     false      // true = empfangene Bytes zurücksenden (wie echter Eindraht-Bus)
#define TRANSCEIVE   true       // Änderungen am Knopf an Adresse 0x00 melden

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

// Diagnose: zeigt unten rechts alle freien Pins an, die auf LOW liegen.
// Knopf drücken und schauen, welche Nummer dazukommt. Danach auf 0 setzen.
#define BTN_SCAN        1
#define USE_LEDS        1       // 0 = LED-Ring nicht ansteuern (zum Testen)
// LED-Ring APA102: laut LilyGO-Beispielcode 42/45. Die LilyGO-Wiki nennt 39/40.
// Falls die LEDs dunkel bleiben, hier die anderen Werte eintragen.
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

// Bandgrenzen, die bei Befehl 02 gemeldet werden (IC-7300)
const uint64_t BAND_LOW  = 30000ULL;
const uint64_t BAND_HIGH = 74800000ULL;

// ===================== Gerätezustand =====================
struct Vfo {
  uint64_t freq;
  uint8_t  mode;    // 00 LSB, 01 USB, 02 AM, 03 CW, 04 RTTY, 05 FM, 07 CW-R, 08 RTTY-R
  uint8_t  filter;  // 01 FIL1, 02 FIL2, 03 FIL3
  uint8_t  data;    // 00 aus, 01..03 Data-Mode an (D1..D3)
};

Vfo vfo[2] = {
  {14074000ULL, 0x01, 0x01, 0x01},   // VFO A: 20 m FT8, USB-D
  { 7074000ULL, 0x01, 0x01, 0x00}    // VFO B: 40 m, USB
};
uint8_t  curVfo = 0;
bool     split  = false;
bool     ptt    = false;
uint8_t  att    = 0x00;
uint16_t levels[256];   // Befehl 14, Werte 0..255
uint8_t  funcs[256];    // Befehl 16
uint16_t sValue = 70;   // aktueller S-Meter-Wert (0120 = S9)

// Für die Anzeige
uint8_t  lastCmd    = 0xFF;
int      lastSub    = -1;
uint32_t frameCount = 0;
volatile bool dirty = true;
unsigned long flashUntil = 0;

const uint64_t bandFreqs[] = {
  1840000ULL, 3573000ULL, 7074000ULL, 10136000ULL, 14074000ULL,
  18100000ULL, 21074000ULL, 24915000ULL, 28074000ULL, 50313000ULL
};
const int NUM_BANDS = sizeof(bandFreqs) / sizeof(bandFreqs[0]);

const uint32_t steps[] = {10, 100, 1000, 10000, 100000};
const char*    stepNames[] = {"10 Hz", "100 Hz", "1 kHz", "10 kHz", "100 kHz"};
const int      NUM_STEPS = 5;
int stepIdx = 2;

struct ModeEntry { uint8_t mode; uint8_t data; };
const ModeEntry modeCycle[] = { {0x00,0}, {0x01,0}, {0x01,1}, {0x03,0}, {0x02,0}, {0x05,0}, {0x04,0} };
const int NUM_MODES = sizeof(modeCycle) / sizeof(modeCycle[0]);

Vfo& cur()   { return vfo[curVfo]; }
Vfo& other() { return vfo[1 - curVfo]; }

// ===================== BCD-Hilfsfunktionen =====================
void freqToBcd(uint64_t f, uint8_t* out, int n = 5) {
  for (int i = 0; i < n; i++) {
    uint8_t lo = f % 10; f /= 10;
    uint8_t hi = f % 10; f /= 10;
    out[i] = (hi << 4) | lo;
  }
}

uint64_t bcdToFreq(const uint8_t* in, int n = 5) {
  uint64_t f = 0, mul = 1;
  for (int i = 0; i < n; i++) {
    f += (uint64_t)(in[i] & 0x0F) * mul; mul *= 10;
    f += (uint64_t)(in[i] >> 4)   * mul; mul *= 10;
  }
  return f;
}

void levelToBcd(uint16_t v, uint8_t* out) {
  out[0] = (((v / 1000) % 10) << 4) | ((v / 100) % 10);
  out[1] = (((v / 10)   % 10) << 4) | (v % 10);
}

uint16_t bcdToLevel(const uint8_t* in) {
  return (in[0] >> 4) * 1000 + (in[0] & 0x0F) * 100 +
         (in[1] >> 4) * 10   + (in[1] & 0x0F);
}

// ===================== Senden =====================
uint8_t replyTo = 0xE0;
bool    silent  = false;   // bei Broadcast-Frames nicht antworten

void sendFrame(uint8_t to, const uint8_t* d, int n) {
  CIV_SERIAL.write(0xFE);
  CIV_SERIAL.write(0xFE);
  CIV_SERIAL.write(to);
  CIV_SERIAL.write(RADIO_ADDR);
  CIV_SERIAL.write(d, n);
  CIV_SERIAL.write(0xFD);
}

void reply(const uint8_t* d, int n) { if (!silent) sendFrame(replyTo, d, n); }
void ok() { uint8_t r = 0xFB; reply(&r, 1); }
void ng() { uint8_t r = 0xFA; reply(&r, 1); }

void broadcastFreq() {
  if (!TRANSCEIVE) return;
  uint8_t r[6];
  r[0] = 0x00; freqToBcd(cur().freq, r + 1);
  sendFrame(0x00, r, 6);
}

void broadcastMode() {
  if (!TRANSCEIVE) return;
  uint8_t m[3] = {0x01, cur().mode, cur().filter};
  sendFrame(0x00, m, 3);
}

// ===================== Simulierte Messwerte =====================
uint16_t meterValue(uint8_t sub) {
  switch (sub) {
    case 0x02: return ptt ? 0 : sValue;                                      // S-Meter (0120 = S9)
    case 0x11: return ptt ? map(levels[0x0A], 0, 255, 0, 213) : 0;          // Po (0213 = 100 %)
    case 0x12: return ptt ? 20 + random(0, 10) : 0;                          // SWR (0048 = 1.5)
    case 0x13: return ptt ? random(0, 60) : 0;                               // ALC
    case 0x14: return 0;                                                     // COMP
    case 0x15: return 180 + random(0, 3);                                    // Vd ~13.8 V
    case 0x16: return ptt ? 120 + random(0, 20) : 10;                        // Id
    default:   return 0;
  }
}

// ===================== Befehlsverarbeitung =====================
void handleFrame(const uint8_t* f, int n) {
  if (n < 3) return;
  uint8_t to = f[0], from = f[1], cmd = f[2];
  const uint8_t* p = f + 3;
  int pl = n - 3;

  if (from == RADIO_ADDR) return;                    // eigenes Echo
  if (to != RADIO_ADDR && to != 0x00) return;        // nicht für uns
  replyTo = from;
  silent  = (to == 0x00);
  lastCmd = cmd;
  lastSub = (pl >= 1 && (cmd == 0x14 || cmd == 0x15 || cmd == 0x16 || cmd == 0x1A ||
                         cmd == 0x1C || cmd == 0x25 || cmd == 0x26)) ? p[0] : -1;
  frameCount++;
  dirty = true;
  flashUntil = millis() + 60;

  uint8_t r[16];

  switch (cmd) {
    case 0x00:   // Frequenz (Transceive, ohne Quittung)
    case 0x05:   // Frequenz setzen
      if (pl >= 5) {
        uint64_t nf = bcdToFreq(p, 5);
        if (nf >= BAND_LOW && nf <= BAND_HIGH) { cur().freq = nf; if (cmd == 0x05) ok(); }
        else if (cmd == 0x05) ng();
      } else if (cmd == 0x05) ng();
      break;

    case 0x01:   // Mode (Transceive, ohne Quittung)
    case 0x06:   // Mode setzen
      if (pl >= 1) {
        cur().mode = p[0];
        if (pl >= 2) cur().filter = p[1];
        if (cmd == 0x06) ok();
      } else if (cmd == 0x06) ng();
      break;

    case 0x02:   // Bandgrenzen
      r[0] = 0x02; freqToBcd(BAND_LOW, r + 1); r[6] = 0x2D; freqToBcd(BAND_HIGH, r + 7);
      reply(r, 12);
      break;

    case 0x03:   // Frequenz lesen
      r[0] = 0x03; freqToBcd(cur().freq, r + 1);
      reply(r, 6);
      break;

    case 0x04:   // Mode lesen
      r[0] = 0x04; r[1] = cur().mode; r[2] = cur().filter;
      reply(r, 3);
      break;

    case 0x07:   // VFO
      if (pl == 0) { ok(); break; }
      switch (p[0]) {
        case 0x00: curVfo = 0; ok(); break;
        case 0x01: curVfo = 1; ok(); break;
        case 0xA0: other() = cur(); ok(); break;
        case 0xB0: { Vfo t = vfo[0]; vfo[0] = vfo[1]; vfo[1] = t; ok(); } break;
        default: ng();
      }
      break;

    case 0x0F:   // Split
      if (pl == 0) { r[0] = 0x0F; r[1] = split ? 0x01 : 0x00; reply(r, 2); }
      else { split = (p[0] == 0x01); ok(); }
      break;

    case 0x11:   // Abschwächer
      if (pl == 0) { r[0] = 0x11; r[1] = att; reply(r, 2); }
      else { att = p[0]; ok(); }
      break;

    case 0x14:   // Pegel
      if (pl == 1)      { r[0] = 0x14; r[1] = p[0]; levelToBcd(levels[p[0]], r + 2); reply(r, 4); }
      else if (pl >= 3) { levels[p[0]] = min((int)bcdToLevel(p + 1), 255); ok(); }
      else ng();
      break;

    case 0x15:   // Messwerte
      if (pl != 1) { ng(); break; }
      if (p[0] == 0x01 || p[0] == 0x05) {            // Squelch-/Over-Status, 1 Byte
        r[0] = 0x15; r[1] = p[0]; r[2] = (p[0] == 0x01) ? 0x01 : 0x00; reply(r, 3);
      } else {
        r[0] = 0x15; r[1] = p[0]; levelToBcd(meterValue(p[0]), r + 2); reply(r, 4);
      }
      break;

    case 0x16:   // Funktionen
      if (pl == 1)      { r[0] = 0x16; r[1] = p[0]; r[2] = funcs[p[0]]; reply(r, 3); }
      else if (pl >= 2) { funcs[p[0]] = p[1]; ok(); }
      else ng();
      break;

    case 0x19:   // Geräte-ID
      if (pl >= 1 && p[0] == 0x00) { r[0] = 0x19; r[1] = 0x00; r[2] = RADIO_ADDR; reply(r, 3); }
      else ng();
      break;

    case 0x1A:
      if (pl >= 1 && p[0] == 0x06) {                 // Data-Mode
        if (pl == 1) {
          r[0] = 0x1A; r[1] = 0x06; r[2] = cur().data;
          r[3] = cur().data ? cur().filter : 0x00;
          reply(r, 4);
        } else {
          cur().data = p[1];
          if (pl >= 3 && p[1] != 0x00) cur().filter = p[2];
          ok();
        }
      } else ng();
      break;

    case 0x1C:
      if (pl >= 1 && p[0] == 0x00) {                 // PTT
        if (pl == 1) { r[0] = 0x1C; r[1] = 0x00; r[2] = ptt ? 0x01 : 0x00; reply(r, 3); }
        else { ptt = (p[1] == 0x01); ok(); }
      } else if (pl >= 1 && p[0] == 0x01) {          // Tuner
        if (pl == 1) { r[0] = 0x1C; r[1] = 0x01; r[2] = 0x00; reply(r, 3); }
        else ok();
      } else ng();
      break;

    case 0x25:   // Frequenz aktiver (00) / inaktiver (01) VFO
      if (pl >= 1 && p[0] <= 0x01) {
        Vfo& v = (p[0] == 0x00) ? cur() : other();
        if (pl == 1)      { r[0] = 0x25; r[1] = p[0]; freqToBcd(v.freq, r + 2); reply(r, 7); }
        else if (pl >= 6) { v.freq = bcdToFreq(p + 1, 5); ok(); }
        else ng();
      } else ng();
      break;

    case 0x26:   // Mode aktiver (00) / inaktiver (01) VFO
      if (pl >= 1 && p[0] <= 0x01) {
        Vfo& v = (p[0] == 0x00) ? cur() : other();
        if (pl == 1) {
          r[0] = 0x26; r[1] = p[0]; r[2] = v.mode; r[3] = v.data; r[4] = v.filter;
          reply(r, 5);
        } else {
          v.mode = p[1];
          if (pl >= 3) v.data = p[2];
          if (pl >= 4) v.filter = p[3];
          ok();
        }
      } else ng();
      break;

    default:
      ng();
  }
}

// ===================== Empfang =====================
uint8_t rx[64];
int rxLen = 0;
int preamble = 0;

void feedByte(uint8_t b) {
  if (b == 0xFE) {
    if (rxLen > 0) { rxLen = 0; preamble = 1; }    // abgebrochener Frame, neu beginnen
    else if (preamble < 2) preamble++;
    return;
  }
  if (preamble < 2) return;
  if (b == 0xFD) {
    handleFrame(rx, rxLen);
    rxLen = 0; preamble = 0;
    return;
  }
  if (rxLen < (int)sizeof(rx)) rx[rxLen++] = b;
  else { rxLen = 0; preamble = 0; }
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
  if (dir > 0) {                 // nächstes Band oberhalb, nach 6 m wieder 160 m
    idx = 0;
    for (int i = 0; i < NUM_BANDS; i++)
      if (bandFreqs[i] > cur().freq + 500000ULL) { idx = i; break; }
  } else {                       // nächstes Band unterhalb, vor 160 m wieder 6 m
    idx = NUM_BANDS - 1;
    for (int i = NUM_BANDS - 1; i >= 0; i--)
      if (bandFreqs[i] + 500000ULL < cur().freq) { idx = i; break; }
  }
  cur().freq = bandFreqs[idx];
  cur().mode = (cur().freq < 10000000ULL) ? 0x00 : 0x01;   // unter 10 MHz LSB
  cur().data = 0;
  broadcastFreq();
  broadcastMode();
  dirty = true;
}

void nextMode(int dir) {
  int idx = 0;
  for (int i = 0; i < NUM_MODES; i++)
    if (modeCycle[i].mode == cur().mode && modeCycle[i].data == (cur().data ? 1 : 0)) { idx = i; break; }
  idx = (idx + (dir > 0 ? 1 : NUM_MODES - 1)) % NUM_MODES;
  cur().mode = modeCycle[idx].mode;
  cur().data = modeCycle[idx].data;
  broadcastMode();
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
      if (f < (int64_t)BAND_LOW)  f = BAND_LOW;
      if (f > (int64_t)BAND_HIGH) f = BAND_HIGH;
      cur().freq = (uint64_t)f;
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
const char* modeName(uint8_t m) {
  switch (m) {
    case 0x00: return "LSB";  case 0x01: return "USB";  case 0x02: return "AM";
    case 0x03: return "CW";   case 0x04: return "RTTY"; case 0x05: return "FM";
    case 0x07: return "CW-R"; case 0x08: return "RTTY-R";
    default:   return "?";
  }
}

const char* bandName(uint64_t f) {
  if (f >=  1810000 && f <=  2000000) return "160m";
  if (f >=  3500000 && f <=  3800000) return "80m";
  if (f >=  5351500 && f <=  5366500) return "60m";
  if (f >=  7000000 && f <=  7200000) return "40m";
  if (f >= 10100000 && f <= 10150000) return "30m";
  if (f >= 14000000 && f <= 14350000) return "20m";
  if (f >= 18068000 && f <= 18168000) return "17m";
  if (f >= 21000000 && f <= 21450000) return "15m";
  if (f >= 24890000 && f <= 24990000) return "12m";
  if (f >= 28000000 && f <= 29700000) return "10m";
  if (f >= 50000000 && f <= 52000000) return "6m";
  if (f >= 70000000 && f <= 70500000) return "4m";
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

// ===================== Diagnose: Pin-Suche =====================
#if BTN_SCAN
// Freie Pins des T-Embed (ohne Display, Drehgeber, LEDs, SD-Karte, Flash/PSRAM, USB)
const uint8_t scanPins[] = {0, 3, 4, 5, 6, 7, 8, 14, 16, 17, 18, 21, 47, 48};
const int NUM_SCAN = sizeof(scanPins) / sizeof(scanPins[0]);
uint32_t scanLowMask = 0;
bool     scanInit = false;
uint16_t scanChanges[NUM_SCAN];       // Anzahl Wechsel je Pin seit dem Start

void initScan() {
  for (int i = 0; i < NUM_SCAN; i++) { pinMode(scanPins[i], INPUT_PULLUP); scanChanges[i] = 0; }
}

void updateScan() {
  static unsigned long last = 0;
  if (millis() - last < 10) return;    // alle 10 ms, entprellt genug für die Zählung
  last = millis();
  uint32_t m = 0;
  for (int i = 0; i < NUM_SCAN; i++)
    if (digitalRead(scanPins[i]) == LOW) m |= (1UL << i);
  if (!scanInit) { scanLowMask = m; scanInit = true; dirty = true; return; }
  if (m != scanLowMask) {
    uint32_t changed = m ^ scanLowMask;
    for (int i = 0; i < NUM_SCAN; i++)
      if ((changed & (1UL << i)) && scanChanges[i] < 999) scanChanges[i]++;
    scanLowMask = m;
    dirty = true;
  }
}

// GPIO 0 nach jedem Startschritt: Start, Display, LED-Init, erste LED-Ausgabe, Ende
int g0Stage[5] = {-1, -1, -1, -1, -1};

void drawScan() {
  char g0[40];
  sprintf(g0, "G0 Start/Disp/LED/Show/Ende: %d %d %d %d %d",
          g0Stage[0], g0Stage[1], g0Stage[2], g0Stage[3], g0Stage[4]);
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLUE);
  gfx->setCursor(4, 136); gfx->print(g0);

  char buf[64] = "LOW:";
  for (int i = 0; i < NUM_SCAN; i++)
    if (scanLowMask & (1UL << i)) {
      char n[6]; sprintf(n, " %d", scanPins[i]);
      if (strlen(buf) + strlen(n) < sizeof(buf)) strcat(buf, n);
    }
  if (!scanLowMask) strcat(buf, " -");
  strcat(buf, "  Wechsel:");
  bool any = false;
  for (int i = 0; i < NUM_SCAN; i++)
    if (scanChanges[i]) {
      char n[14]; sprintf(n, " %d(%dx)", scanPins[i], scanChanges[i]);
      if (strlen(buf) + strlen(n) < sizeof(buf)) strcat(buf, n);
      any = true;
    }
  if (!any) strcat(buf, " -");
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLUE);
  gfx->setCursor(4, 146); gfx->print(buf);
}
#endif

void drawDisplay() {
  char buf[32];
  gfx->fillScreen(C_BG);

  // --- Kopfzeile: VFO, Mode, Split, Band, RX/TX
  sprintf(buf, "VFO %c", curVfo ? 'B' : 'A');
  pill(2, 2, buf, C_DARK, C_TEXT);
  sprintf(buf, "%s%s", modeName(cur().mode), cur().data ? "-D" : "");
  pill(78, 2, buf, C_ACCENT, C_BG);
  if (split) pill(160, 2, "SPL", C_BLUE, C_TEXT);
  gfx->setTextColor(C_DIM); gfx->setTextSize(2);
  gfx->setCursor(214, 5); gfx->print(bandName(cur().freq));
  pill(276, 2, ptt ? "TX" : "RX", ptt ? C_RED : C_GREEN, C_TEXT);

  // --- Frequenz (Festbreitenschrift, 30 px pro Zeichen), rechtsbündig
  uint32_t f = (uint32_t)cur().freq;
  sprintf(buf, "%lu.%03lu.%03lu", (unsigned long)(f / 1000000), (unsigned long)((f / 1000) % 1000), (unsigned long)(f % 1000));
  const int right = 314, cw = 30, fy = 32;
  int len = strlen(buf);
  gfx->setTextSize(5);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(right - len * cw, fy);
  gfx->print(buf);

  // Markierung unter der Stelle, die der Knopf verstellt
  const int stepPos[] = {1, 2, 4, 5, 6};        // Zeichen von rechts (0 = 1-Hz-Stelle)
  int cx = right - (stepPos[stepIdx] + 1) * cw;
  gfx->fillRect(cx, fy + 42, 25, 3, C_ACCENT);

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
  sprintf(buf, "%s  0x%02X", RADIO_NAME, RADIO_ADDR);
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

  uint16_t v   = ptt ? meterValue(0x11) : sValue;
  uint16_t vmx = ptt ? 213 : 241;
  int lit = (int)v * (mw / seg) / vmx;
  for (int i = 0; i < mw / seg; i++) {
    uint16_t c;
    if (i >= lit) c = C_DARK;
    else if (ptt) c = C_RED;
    else c = (i * seg * 241 / mw <= 120) ? C_GREEN : C_RED;
    gfx->fillRect(mx + i * seg, my, seg - 2, mh, c);
  }

  if (!ptt) sText(v, buf); else sprintf(buf, "%d W", (int)(v * 100 / 213));
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(316 - strlen(buf) * 12, my + 20);
  gfx->print(buf);

  // --- Statuszeile
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  if (lastCmd == 0xFF) strcpy(buf, "warte auf CI-V ...");
  else if (lastSub >= 0) sprintf(buf, "Cmd %02X %02X", lastCmd, lastSub);
  else sprintf(buf, "Cmd %02X", lastCmd);
  gfx->setCursor(4, 160); gfx->print(buf);
  sprintf(buf, "Frames %lu", (unsigned long)frameCount);
  gfx->setCursor(4 + 22 * 6, 160); gfx->print(buf);

  // Kontrollanzeige: leuchtet, solange der Knopf gedrückt ist
  if (btnRaw) {
    gfx->setTextColor(C_ACCENT);
    gfx->setCursor(316 - 5 * 6, 160); gfx->print("KNOPF");
  }
#if BTN_SCAN
  drawScan();
#endif

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

  CIV_SERIAL.begin(CIV_BAUD);
#if ARDUINO_USB_MODE == 1
  CIV_SERIAL.setTxTimeoutMs(0);           // nicht blockieren, wenn kein PC verbunden ist
#endif

  for (int i = 0; i < 256; i++) { levels[i] = 128; funcs[i] = 0; }
  levels[0x01] = 100;   // AF
  levels[0x02] = 255;   // RF-Gain
  levels[0x03] = 0;     // Squelch
  levels[0x0A] = 255;   // RF-Power 100 %
  funcs[0x12]  = 0x02;  // AGC MID

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);
#if BTN_SCAN
  delay(5);
  g0Stage[0] = digitalRead(PIN_ENC_BTN);
#endif

  gfx->begin();
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
#if BTN_SCAN
  g0Stage[1] = digitalRead(PIN_ENC_BTN);
#endif

#if USE_LEDS
  FastLED.addLeds<APA102, PIN_LED_DATA, PIN_LED_CLK, BGR>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
#endif
#if BTN_SCAN
  g0Stage[2] = digitalRead(PIN_ENC_BTN);
#endif
#if USE_LEDS
  FastLED.show();
#endif
#if BTN_SCAN
  delay(5);
  g0Stage[3] = digitalRead(PIN_ENC_BTN);
#endif

  randomSeed(esp_random());

  // Knopf-Pin nach dem Start der Bibliotheken noch einmal einrichten,
  // falls eine davon GPIO 0 umkonfiguriert hat.
#if BTN_SCAN
  initScan();
#endif
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
#if BTN_SCAN
  delay(5);
  g0Stage[4] = digitalRead(PIN_ENC_BTN);
#endif

  drawDisplay();
}

void loop() {
  while (CIV_SERIAL.available()) {
    uint8_t b = CIV_SERIAL.read();
    if (CIV_ECHO) CIV_SERIAL.write(b);
    feedByte(b);
  }

  handleEncoder();
  handleButton();
#if BTN_SCAN
  updateScan();
#endif

  // Frequenzänderungen vom Knopf höchstens alle 100 ms melden
  if (pendingFreqBroadcast && millis() - lastBroadcast >= 100) {
    broadcastFreq();
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
