# ESP32 bridge — Wi-Fi front-end

The network brain (the BLE lives on the nRF52840). Wi-Fi/MQTT/HTTP provisioning + a web UI of
the De'Longhi remote whose buttons are forwarded over **UART** to the nRF emulator. Working
end-to-end.

Two release flavors are built:

- `ganymede-bridge-headless-esp32s3.bin` for the existing ESP32-S3 headless bridge.
- `ganymede-bridge-touchscreen-esp32.bin` for the ESP32 touchscreen bridge.

---

## Flash it — the fast path (no toolchain)

### From your browser
Open the **[web flasher](https://bdherouville.github.io/pinguino-ha-remote/flash/)** in Chrome
or Edge, plug in the board, click **Install**, and pick the board's serial port. The existing
ESP32-S3 headless board appears as USB-Serial/JTAG; many plain ESP32 touchscreen boards appear
as a USB-UART adapter such as CP210x or CH340.

### From the command line
Grab the right image from the
[latest release](https://github.com/bdherouville/pinguino-ha-remote/releases/latest):

```bash
pip install esptool      # if you don't have it
esptool --chip esp32s3 -p <PORT> write_flash 0x0 ganymede-bridge-headless-esp32s3.bin
esptool --chip esp32 -p <PORT> write_flash 0x0 ganymede-bridge-touchscreen-esp32.bin
```
`<PORT>` is e.g. `/dev/ttyACM0` (Linux) or `COMx` (Windows). This single image contains the
bootloader + partition table + app, and **preserves NVS** (your saved Wi-Fi/MQTT). A full
`esptool erase-flash` wipes that provisioning.

### First boot
No Wi-Fi configured → the board raises an open AP **`Ganymede-Bridge`**. Connect to it, open
`http://192.168.4.1`, scan + pick your network, enter the password. Then reach the web UI on
the board's LAN IP. Set the **MQTT broker** there too for Home Assistant.

## Build it locally (ESP-IDF v6.0.1)

```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py -B build-headless-esp32s3 -DSDKCONFIG=sdkconfig.headless \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.headless' build
idf.py -B build-touchscreen-esp32 -DSDKCONFIG=sdkconfig.touchscreen \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.touchscreen' build
```
The web UI (`main/www/index.html`, self-contained HTML/CSS/SVG) is embedded via `EMBED_FILES`.

---

## Firmware flavors and wiring

All hardware-specific defaults live in `main/board_config.h`. Runtime pin overrides from the
web UI still apply on the next reboot, preserving the existing headless behavior.

### Existing ESP32-S3 headless bridge

| Existing ESP32-S3 headless | → | nRF52840 / sensor |
|---|---|---|
| GPIO4 (TX) | → | nRF **P0.20** (RX) |
| GPIO5 (RX) | ← | nRF **P0.22** (TX) |
| GPIO6 (HB) | ← | nRF **P0.24** (heartbeat ~1 Hz) |
| GPIO1 (SCL) | → | BME280 SCL |
| GPIO2 (SDA) | ↔ | BME280 SDA |

UART is 115200 8N1, GND common. ESP→nRF: `press <btn>\n`, `env <t> <h> <p>\n`.

### ESP32 touchscreen bridge

The touchscreen flavor targets a plain ESP32, not ESP32-S3. The configured board is the
JC2432W328: ST7789 TFT, CST820 capacitive touch and CH340-style USB-UART.

| ESP32 touchscreen default | → | Module |
|---|---|---|
| GPIO17 (TX) | → | nRF52840 RX |
| GPIO16 (RX) | ← | nRF52840 TX |
| disabled (`nrf_hb=-1`) | ← | optional nRF52840 heartbeat |
| GPIO33 (SDA) | ↔ | CST820 touch / optional BME680 SDA |
| GPIO32 (SCL) | → | CST820 touch / optional BME680 SCL |
| GPIO12 (MISO), GPIO13 (MOSI), GPIO14 (SCLK), GPIO15 (CS), GPIO2 (DC) | → | ST7789 TFT |
| GPIO27 (PWM) | → | ST7789 backlight |
| GPIO25 (RST), GPIO21 (INT) | →/← | CST820 touch |
| GPIO19 (TX) | → | LD2410 RX |
| GPIO18 (RX) | ← | LD2410 TX |

BME680 uses I2C address auto-detection (`0x76`, then `0x77`) at 400 kHz on the same I2C bus as
the CST820 (`0x15`). The current hardware bring-up expects BME680 and nRF52840 may be absent:
network, HTTP, MQTT and the local touchscreen UI still boot, with nRF commands disabled until
recognized `status <token>` UART messages arrive.

## Status LED (WS2812 on GPIO48)

Headless ESP32-S3 only. Wi-Fi phase (until connected): white dim = booting · blue pulse = provisioning AP · yellow
blink = connecting · red slow = Wi-Fi failed. Once Wi-Fi is up the LED shows the **nRF link**:
red slow = nRF not alive · cyan pulse = advertising/waiting · yellow = bonded, not ready ·
**green = ready to relay** · red fast = error.

## Home Assistant / MQTT

Broker configured in the web UI (stored in NVS). On connect, publishes MQTT-Discovery for **9
`button` entities** under device "Ganymede Bridge", environmental sensors, display config
entities, diagnostics and availability LWT. HA button -> `ganymede/cmd/<btn>` -> command queue
-> UART `press <btn>`. The existing open-loop AC model and discovery remain available; it is
still fire-per-press with no direct AC feedback.

## HTTP API

`GET /api/status` · `GET /api/scan` · `POST /api/connect` (ssid,pass) · `POST /api/mqtt`
(host,port,user,pass) · `POST /api/press` (btn/button) · `POST /api/ui` · `GET /`

## nRF link status contract

The nRF emulator prints `status <token>\n` over UART (`boot` / `advertising` / `connected` /
`bonded` / `ready` / `error`) and re-pushes it every ~2 s. The existing headless ESP32-S3 board
also uses **GPIO6** as a ~1 Hz hardware heartbeat; the touchscreen flavor leaves heartbeat
disabled (`nrf_hb=-1`) until the actual board wiring is confirmed. No heartbeat edge **and** no
UART for 3 s ⇒ the bridge reports `offline` (so a floating/absent nRF never shows a stale
"ready"). Board-specific notes:
[`../../docs/HARDWARE.md`](../../docs/HARDWARE.md).
