# ICOM-SIMULATOR

[Deutsch](README.md) | **English**

A CI-V simulator for the **LilyGO T-Embed (ESP32-S3)**. It lets you test software such as Shack-Server, WSJT-X, flrig or a logbook without owning an Icom radio.

Over USB-C, the T-Embed identifies itself as an **Icom IC-7300** (CI-V address `0x94`). Your computer sees it as an ordinary serial port. You operate it with the rotary knob, just like the VFO knob of a real radio.

## Features

- Answers the common CI-V commands: frequency, mode, data mode, VFO A/B, split, PTT, levels, meters (S-meter, power, SWR, ALC, voltage, current), functions and transceiver ID.
- **Transceive:** when you turn the knob, the simulator reports the change on its own, like a real radio.
- **Colour display:** shows VFO, mode, split, band, RX/TX, the frequency, an S-meter with scale (output power while transmitting), the last CI-V command and a frame counter.
- **LED ring:** green on RX, red on TX, flashes blue for every CI-V frame received.
- Unknown commands are answered with `FA` (NG), just like a real radio.

## Controls

Each **press** of the knob switches what turning it controls: **FREQ → SCHRITT (step) → MODE → BAND → FREQ**. The active function is shown in orange below the frequency.

| Function | Turning the knob |
|---|---|
| FREQ | Tune frequency (reported via transceive) |
| SCHRITT | Tuning step 10 Hz / 100 Hz / 1 kHz / 10 kHz / 100 kHz |
| MODE | LSB, USB, USB-D, CW, AM, FM, RTTY |
| BAND | 160 m to 6 m |

After 8 seconds without input, the knob returns to FREQ. While the knob is pressed, **KNOPF** appears at the bottom right.

## Hardware

- [LilyGO T-Embed ESP32-S3](https://wiki.lilygo.cc/products/t-embed-series/t-embed/)
- USB-C cable with data lines (charge-only cables will not work)

No other hardware is needed.

## Installation

1. Install the **Arduino IDE** and install **esp32 by Espressif Systems** **version 3.x** in the Boards Manager. The sketch does not compile with version 2.x.
2. Install these libraries in the Library Manager:
   - **GFX Library for Arduino** (moononournation), version 1.5 or newer
   - **FastLED**
3. Set the following under *Tools*:

   | Setting | Value |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | USB Mode | Hardware CDC and JTAG |
   | Flash Size | 16MB |
   | PSRAM | OPI PSRAM |

4. Open `CIV_Simulator_TEmbed/CIV_Simulator_TEmbed.ino` and upload it.

## Connecting to Shack-Server (Raspberry Pi)

1. Plug the T-Embed into a USB port on the Raspberry Pi with a USB-C cable.
2. Find the port:
   ```
   ls /dev/ttyACM*
   ```
   It is usually `/dev/ttyACM0`.
3. Configure Shack-Server:
   - Radio: **IC-7300**
   - Port: **/dev/ttyACM0**
   - CI-V address: **94h**
   - Baud rate: **19200** (has no effect over native USB)
4. If the port cannot be opened, run this once and then reboot:
   ```
   sudo usermod -aG dialout $USER
   ```

On Windows, the T-Embed shows up as a COM port. Device Manager tells you which one.

## Configuration

All settings are at the top of the sketch:

| Setting | Purpose |
|---|---|
| `RADIO_ADDR` / `RADIO_NAME` | Simulate a different radio, e.g. IC-705 = `0xA4`, IC-9700 = `0xA2`, IC-7610 = `0x98` |
| `TRANSCEIVE` | Report knob changes unsolicited (`true`/`false`) |
| `CIV_ECHO` | Echo received bytes back, as on a real single-wire bus |
| `SCREEN_ROTATION` | Rotate the display (`1` or `3`) |
| `ENC_REVERSE` | Reverse the knob direction |
| `ENC_DIV` | `4` or `2`, if the knob moves two steps per detent or only counts every second detent |
| `LED_BRIGHTNESS` | LED ring brightness |

## Troubleshooting

| Problem | Solution |
|---|---|
| Error `esp32-hal-periman.h: No such file` | Update the ESP32 board package to version 3.x |
| Error `has no member named 'setTxTimeoutMs'` or `USB CDC On Boot` | Set **USB CDC On Boot** to **Enabled** under *Tools* |
| First compile takes very long | Normal after a board package update (10–20 min). On Windows, excluding `%LOCALAPPDATA%\Arduino15` and `%LOCALAPPDATA%\Temp\arduino` from the virus scanner helps. |
| Pressing the knob does nothing, **KNOPF** stays on at the bottom right | FastLED claims GPIO 0 (the knob button) on its first LED output. The sketch therefore re-initialises the pin afterwards. If you modify the sketch, keep `FastLED.show()` in `setup()` **before** `pinMode(PIN_ENC_BTN, INPUT_PULLUP)`. |
| Display is upside down | Set `SCREEN_ROTATION` to `3` |
| Frequency moves the wrong way | Set `ENC_REVERSE` to `true` |
| LED ring stays dark | Change `PIN_LED_DATA` / `PIN_LED_CLK` from `42`/`45` to `39`/`40`. Sources disagree on these pins. |
| No port on the computer | Try another USB cable; *USB CDC On Boot* must be *Enabled* |
| Software reports errors | If the display shows a command at the bottom, traffic is arriving. If the counter stays at 0, check the port and address in your software. |

## Supported CI-V commands

| Command | Function |
|---|---|
| `00` / `05` | Report / set frequency |
| `01` / `06` | Report / set mode |
| `02` | Read band edges |
| `03` / `04` | Read frequency / mode |
| `07` | VFO A/B, A=B, A↔B |
| `0F` | Split |
| `11` | Attenuator |
| `14 xx` | Levels (AF, RF, squelch, RF power …) |
| `15 xx` | Meters (S-meter, Po, SWR, ALC, Vd, Id) |
| `16 xx` | Functions (preamp, AGC, NB, NR …) |
| `19 00` | Transceiver ID |
| `1A 06` | Data mode |
| `1C 00` / `1C 01` | PTT / tuner |
| `25` / `26` | Frequency / mode per VFO |

## Note

The USB port is used exclusively for CI-V. Do not add `Serial.print()` output, or text will end up in the CI-V data stream.

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).
