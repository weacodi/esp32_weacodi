# AGENTS.md

Use this guide to configure, build and install Weacodi on the user's ESP32. The
sketch entry point is [esp32_weacodi.ino](esp32_weacodi.ino). Its enclosing
directory must be named `esp32_weacodi` for Arduino CLI.

## Tested hardware

The prototype was developed, flashed and tested on the **Elegoo ESP-32
2.8 Inch Touch LCD**. This unit has an ESP32-D0WD-V3 with 4 MB of flash,
an ILI9341 SPI TFT and an XPT2046 resistive touch controller. The display
is 240 × 320 in portrait and runs at 320 × 240 with rotation 1. The tested
Arduino board target is `esp32:esp32:esp32` (ESP32 Dev Module). On the
development Linux host, the USB serial bridge appeared as `/dev/ttyUSB0`.
A board reported as `Unknown` by Arduino CLI can still use this target
when its chip and wiring are confirmed.

| Component | Verified settings |
| --- | --- |
| ILI9341 TFT | `ILI9341_2_DRIVER`, HSPI, 40 MHz |
| TFT pins | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, backlight 21 active high. No reset GPIO in the sketch configuration. |
| XPT2046 touch | VSPI, SCLK 25, MOSI 32, MISO 39, CS 33 |
| Serial console | 115200 baud |

Identify the user's board, display controller, resolution, touch controller
and pinout before choosing a target. The wiring above is the tested
reference. For different TFT hardware, edit [config/User_Setup.h](config/User_Setup.h).
For different XPT2046 wiring, update the chip-select constructor and
`touchSpi.begin(...)` in the sketch. A resolution or orientation change also
requires checking the drawing coordinates in the sketch. Keep `SMOOTH_FONT`
and the built-in fonts enabled in TFT_eSPI. For an ESP32-S3, ESP32-C3 or a
different type of touch controller, use the board documentation to adapt the
target and driver before treating the device as compatible.

## Wi-Fi and location

Use the user's existing `wifi.local.h` or have the user create it locally
next to the sketch. Git ignores this file. Keep the real values out of
tracked files, command output and replies. The reference ESP32 needs a
2.4 GHz Wi-Fi network with access to DNS, NTP and the Weacodi HTTPS API.

```cpp
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
```

The first release uses a fixed location, Munich by default. The tap gesture
switches the number of forecast days. There is no settings menu, automatic
location detection or city search in this release. To change the city, update
all location values together in `esp32_weacodi.ino`, then rebuild and flash.

- Set `cityName` to the name to display. Use a Latin spelling supported by
  the current city font.
- Set `latitude` and `longitude` in `forecastUrl` to decimal coordinates.
  South and west use negative values.
- Set the API's `timezone` query parameter to the location's URL-encoded IANA
  time zone. For example, Munich uses `Europe%2FBerlin`.
- Set the local `timezone` C++ constant to a matching POSIX rule for
  `configTzTime`. Munich uses `CET-1CEST,M3.5.0,M10.5.0/3`. Check that the
  API timestamps and the device clock agree, including any seasonal offset.

Keep `past_days=2` in the forecast query for the wet-road and ice calculations.
The location is compiled into the firmware, so it remains selected when the
board restarts.

## Build with Arduino CLI

Run from the sketch directory. Install Arduino CLI first if it is absent.
The versions below are the tested baseline.

```sh
esp32_index=https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index --additional-urls "$esp32_index"
arduino-cli core install esp32:esp32@2.0.6 --additional-urls "$esp32_index"
arduino-cli lib install 'TFT_eSPI@2.5.43' 'ArduinoJson@7.4.2' 'XPT2046_Touchscreen@1.4.0'
```

Build using a private copy of TFT_eSPI under the ignored `build/` directory
so that other sketches keep their display configuration. These commands use
a POSIX shell. On Windows, use the equivalent directory and copy commands
or run them under Git Bash.

```sh
user_dir=$(arduino-cli config get directories.user)
tft_dir="$PWD/build/libraries/TFT_eSPI"
mkdir -p "$tft_dir"
cp -R "$user_dir/libraries/TFT_eSPI/." "$tft_dir/"
cp config/User_Setup.h "$tft_dir/User_Setup.h"
arduino-cli compile --fqbn esp32:esp32:esp32 --library "$tft_dir" --output-dir build/firmware .
```

A build without `wifi.local.h` is useful for checking compilation. It displays
`Wi-Fi not configured` on startup. Build again after adding the local Wi-Fi
settings or changing the city. [Espressif setup guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)

## Flash and verify on the device

When the user requests installation, identify the target USB port with
`arduino-cli board list`. Compare the list before and after plugging in the
board if several devices are connected. Upload replaces the existing firmware
on that board. Close serial monitors or other software using the port before
uploading. Replace the example port with the one detected on the user's host.

```sh
arduino-cli board list
port=/dev/ttyUSB0
arduino-cli upload --fqbn esp32:esp32:esp32 -p "$port" --input-dir build/firmware .
arduino-cli monitor -p "$port" --config baudrate=115200
```

Use `/dev/cu.*` on macOS or `COMx` on Windows as appropriate. If no port
appears, check the USB data cable and serial bridge driver. For a Linux
permission error, verify the account's access to the serial port. On
Debian/Ubuntu this is commonly the `dialout` group. If the bootloader fails to
connect and the board has a BOOT button, use the board's documented bootloader
sequence and retry. A lit but blank display after a successful upload usually
warrants checking the TFT driver, pinout and library copy selected at build
time.

Check that the serial log shows `Connecting to WiFi - connected`,
`Forecast HTTP 200` and `Rendered 48 hours:` for a typical startup. Confirm
the city and local time on the screen. With live data on the tested board,
taps cycle 2 → 3 → 1 → 2 days. If a test can only read serial logs, report
that separately from observing the display or touching the panel.

Useful startup messages are `Wi-Fi not configured`, `Wi-Fi unavailable`,
`Clock sync failed`, `Weacodi® API unavailable` and
`Forecast data missing`. Use the message to check local credentials and
connectivity before changing the code. For display inspection, send ASCII
`s` over serial. The reply starts with `RGB565 320 240` and contains 153600
bytes of big-endian RGB565 pixels in row order.

Keep local credentials, build output and serial captures outside Git. Keep the
firmware and diagram licensing scopes in [LICENSE](LICENSE),
[LICENSE_WEACODI.md](LICENSE_WEACODI.md) and [NOTICE](NOTICE).
