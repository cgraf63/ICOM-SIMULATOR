# ICOM-SIMULATOR

**Deutsch** | [English](README.en.md)

Ein CI-V-Simulator für das **LilyGO T-Embed (ESP32-S3)**. Damit kannst du Software wie Shack-Server, WSJT-X, flrig oder ein Logbuch testen, ohne ein Icom-Funkgerät zu besitzen.

Das T-Embed gibt sich über USB-C als **Icom IC-7300** aus (CI-V-Adresse `0x94`). Am Rechner erscheint es als normaler serieller Port. Über den Drehknopf bedienst du es wie den VFO-Knopf eines echten Geräts.

Das Repository enthält zwei Sketches:

| Ordner | Simuliert | Protokoll |
|---|---|---|
| `CIV_Simulator_TEmbed` | Icom IC-7300 (und andere Icom-Geräte) | CI-V |
| `YAESU_FTX1_Simulator_TEmbed` | Yaesu FTX-1 | Yaesu-CAT, siehe [Yaesu FTX-1](#yaesu-ftx-1) |

Hardware, Installation, Bedienung und Fehlersuche sind bei beiden gleich.

## Funktionen

- Beantwortet die gängigen CI-V-Befehle: Frequenz, Mode, Data-Mode, VFO A/B, Split, PTT, Pegel, Messwerte (S-Meter, Leistung, SWR, ALC, Spannung, Strom), Funktionen und Geräte-ID.
- **Transceive:** Wenn du am Knopf drehst, meldet das Gerät die Änderung von sich aus, wie ein echtes Funkgerät.
- **Farbdisplay:** zeigt VFO, Mode, Split, Band, RX/TX, die Frequenz, ein S-Meter mit Skala (bei TX die Leistung) sowie den letzten CI-V-Befehl und einen Frame-Zähler.
- **LED-Ring:** grün bei RX, rot bei TX, blitzt blau bei jedem empfangenen CI-V-Frame.
- Unbekannte Befehle beantwortet der Simulator mit `FA` (NG), wie ein echtes Gerät.

## Bedienung

Jeder **Druck** auf den Knopf schaltet weiter, was der Drehknopf verstellt: **FREQ → SCHRITT → MODE → BAND → FREQ**. Die aktive Funktion steht orange unter der Frequenz.

| Funktion | Knopf drehen |
|---|---|
| FREQ | Frequenz verstellen (wird per Transceive gemeldet) |
| SCHRITT | Schrittweite 10 Hz / 100 Hz / 1 kHz / 10 kHz / 100 kHz |
| MODE | LSB, USB, USB-D, CW, AM, FM, RTTY |
| BAND | 160 m bis 6 m |

Nach 8 Sekunden ohne Bedienung springt der Knopf automatisch zurück auf FREQ. Solange der Knopf gedrückt ist, erscheint unten rechts **KNOPF**.

## Hardware

- [LilyGO T-Embed ESP32-S3](https://wiki.lilygo.cc/products/t-embed-series/t-embed/)
- USB-C-Kabel mit Datenleitungen (reine Ladekabel funktionieren nicht)

Weitere Hardware brauchst du nicht.

## Installation

1. **Arduino IDE** installieren und im Boardverwalter **esp32 by Espressif Systems** in **Version 3.x** installieren. Mit Version 2.x lässt sich der Sketch nicht kompilieren.
2. Im Bibliotheksverwalter installieren:
   - **GFX Library for Arduino** (moononournation), Version 1.5 oder neuer
   - **FastLED**
3. Unter *Werkzeuge* einstellen:

   | Einstellung | Wert |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | USB Mode | Hardware CDC and JTAG |
   | Flash Size | 16MB |
   | PSRAM | OPI PSRAM |

4. `CIV_Simulator_TEmbed/CIV_Simulator_TEmbed.ino` öffnen und hochladen.

## Verbindung mit Shack-Server (Raspberry Pi)

1. Das T-Embed per USB-C an eine USB-Buchse des Raspberry Pi anschließen.
2. Den Port suchen:
   ```
   ls /dev/ttyACM*
   ```
   Meist heißt er `/dev/ttyACM0`.
3. In Shack-Server einstellen:
   - Gerät: **IC-7300**
   - Port: **/dev/ttyACM0**
   - CI-V-Adresse: **94h**
   - Baudrate: **19200** (bei nativem USB ohne Bedeutung)
4. Falls der Port nicht geöffnet werden darf, einmalig ausführen und danach neu starten:
   ```
   sudo usermod -aG dialout $USER
   ```

Unter Windows erscheint das T-Embed als COM-Port. Welcher es ist, siehst du im Geräte-Manager.

## Anpassungen

Alle Einstellungen findest du oben im Sketch:

| Einstellung | Zweck |
|---|---|
| `RADIO_ADDR` / `RADIO_NAME` | anderes Gerät simulieren, z. B. IC-705 = `0xA4`, IC-9700 = `0xA2`, IC-7610 = `0x98` |
| `TRANSCEIVE` | Änderungen am Knopf unaufgefordert melden (`true`/`false`) |
| `CIV_ECHO` | empfangene Bytes zurücksenden, wie auf einem echten Eindraht-Bus |
| `SCREEN_ROTATION` | Display drehen (`1` oder `3`) |
| `ENC_REVERSE` | Drehrichtung des Knopfs umkehren |
| `ENC_DIV` | `4` oder `2`, falls der Knopf pro Rastung zwei Schritte macht oder nur jede zweite Rastung zählt |
| `LED_BRIGHTNESS` | Helligkeit des LED-Rings |

## Fehlersuche

| Problem | Lösung |
|---|---|
| Fehler `esp32-hal-periman.h: No such file` | ESP32-Boardpaket auf Version 3.x aktualisieren |
| Fehler `has no member named 'setTxTimeoutMs'` oder `USB CDC On Boot` | Unter *Werkzeuge* **USB CDC On Boot** auf **Enabled** stellen |
| Erstes Kompilieren dauert sehr lange | Normal nach einem Update des Boardpakets (10–20 Min.). Unter Windows hilft es, `%LOCALAPPDATA%\Arduino15` und `%LOCALAPPDATA%\Temp\arduino` vom Virenscanner auszuschließen. |
| Knopfdruck bewirkt nichts, **KNOPF** steht dauerhaft unten rechts | FastLED belegt GPIO 0 (Knopf) bei der ersten LED-Ausgabe. Der Sketch richtet den Pin deshalb danach neu ein. Bei eigenen Änderungen darauf achten, dass `FastLED.show()` in `setup()` **vor** `pinMode(PIN_ENC_BTN, INPUT_PULLUP)` steht. |
| Display steht auf dem Kopf | `SCREEN_ROTATION` auf `3` ändern |
| Frequenz läuft falsch herum | `ENC_REVERSE` auf `true` setzen |
| LED-Ring bleibt dunkel | `PIN_LED_DATA` / `PIN_LED_CLK` von `42`/`45` auf `39`/`40` ändern. Die Quellen widersprechen sich bei diesen Pins. |
| Kein Port am Rechner | anderes USB-Kabel versuchen, *USB CDC On Boot* muss auf *Enabled* stehen |
| Software meldet Fehler | Zeigt das Display unten einen Befehl an, kommt Verkehr an. Bleibt der Zähler bei 0, Port und Adresse in der Software prüfen. |

## Unterstützte CI-V-Befehle

| Befehl | Funktion |
|---|---|
| `00` / `05` | Frequenz melden / setzen |
| `01` / `06` | Mode melden / setzen |
| `02` | Bandgrenzen lesen |
| `03` / `04` | Frequenz / Mode lesen |
| `07` | VFO A/B, A=B, A↔B |
| `0F` | Split |
| `11` | Abschwächer |
| `14 xx` | Pegel (AF, RF, Squelch, Sendeleistung …) |
| `15 xx` | Messwerte (S-Meter, Po, SWR, ALC, Vd, Id) |
| `16 xx` | Funktionen (Preamp, AGC, NB, NR …) |
| `19 00` | Geräte-ID |
| `1A 06` | Data-Mode |
| `1C 00` / `1C 01` | PTT / Tuner |
| `25` / `26` | Frequenz / Mode je VFO |

## Yaesu FTX-1

Der Sketch `YAESU_FTX1_Simulator_TEmbed/YAESU_FTX1_Simulator_TEmbed.ino` simuliert einen **Yaesu FTX-1**. Yaesu verwendet kein CI-V, sondern **CAT**: Textbefehle, die mit `;` enden, zum Beispiel `FA014074000;` für 14,074 MHz.

**Einstellungen im Programm (z. B. Shack-Server):** Gerät **Yaesu FTX-1** (Hamlib-Modell 1051), den Port des T-Embed, **38400** Baud (bei nativem USB ohne Bedeutung), 8N1.

**Unterschiede zum Icom-Sketch:**
- Oben links steht **MAIN** oder **SUB** statt VFO A/B, unter dem S-Meter **YAESU FTX-1**.
- Modes: LSB, USB, DATA-U, DATA-L, CW-U, AM, FM, FM-N, RTTY-L, PSK, C4FM.
- Bänder 160 m bis 6 m sowie **2 m** und **70 cm**. Frequenzen ab 100 MHz werden etwas kleiner dargestellt.
- Änderungen am Knopf meldet der Simulator nur von selbst, wenn das Programm **Auto-Information** einschaltet (`AI1;`). Rechts neben der Schrittweite steht dann „AI ein“.
- Unten links steht der zuletzt empfangene Befehl im Klartext, z. B. `FA;`.

**Unterstützte CAT-Befehle:**

| Befehl | Funktion |
|---|---|
| `ID` | Geräte-ID, Antwort `ID0840;` wie im FTX-1-Handbuch |
| `PS` | Einschaltzustand (immer an) |
| `AI` | Auto-Information ein/aus |
| `FA` / `FB` | Frequenz MAIN / SUB (9 Stellen) |
| `MD0` / `MD1` | Mode MAIN / SUB |
| `IF` / `OI` | Statusinformation MAIN / SUB (30 Zeichen) |
| `VS` | MAIN/SUB auswählen |
| `FT` / `ST` | Sende-VFO (Split) |
| `TX` | Senden / Empfangen |
| `SM0` / `SM1` | S-Meter |
| `RM` | Messwerte (S, COMP, ALC, PO, SWR, IDD, VDD) |
| `PC` | Sendeleistung |
| `AG` / `RG` / `SQ` | NF-Pegel / HF-Verstärkung / Squelch |
| `NA` | Schmal-Filter |
| `BS` | Bandwahl (1,8–50 MHz) |

Unbekannte oder falsch aufgebaute Befehle beantwortet der Simulator mit `?;`, wie ein echter FTX-1.

## Hinweis

Der USB-Port wird ausschließlich für CI-V verwendet. Baue keine `Serial.print()`-Ausgaben ein, sonst landet Text im CI-V-Datenstrom.

## Lizenz

GNU General Public License v3.0, siehe [LICENSE](LICENSE).
