# ESP32-S3 bridge — Wi-Fi front-end

The network brain (the BLE lives on the nRF52840). Wi-Fi/MQTT/HTTP provisioning + a web UI of
the De'Longhi remote whose buttons are forwarded over **UART** to the nRF emulator. Working
end-to-end.

---

## Flash it — the fast path (no toolchain)

### From your browser
Open the **[web flasher](https://bdherouville.github.io/pinguino-ha-remote/flash/)** in Chrome
or Edge, plug in the board, click **Install**, and pick the `USB JTAG/serial` port. Done.

### From the command line
Grab `ganymede-bridge-esp32s3.bin` from the
[latest release](https://github.com/bdherouville/pinguino-ha-remote/releases/latest):

```bash
pip install esptool      # if you don't have it
esptool --chip esp32s3 -p <PORT> write_flash 0x0 ganymede-bridge-esp32s3.bin
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
idf.py set-target esp32s3          # first time only
idf.py -p <PORT> build flash monitor
```
The web UI (`main/www/index.html`, self-contained HTML/CSS/SVG) is embedded via `EMBED_FILES`.

---

## Wiring

| ESP32-S3 | → | nRF52840 / sensor |
|---|---|---|
| GPIO4 (TX) | → | nRF **P0.20** (RX) |
| GPIO5 (RX) | ← | nRF **P0.22** (TX) |
| GPIO6 (HB) | ← | nRF **P0.24** (heartbeat ~1 Hz) |
| GPIO1 (SCL) | → | BME280 SCL |
| GPIO2 (SDA) | ↔ | BME280 SDA |

UART is 115200 8N1, GND common. ESP→nRF: `press <btn>\n`, `env <t> <h> <p>\n`.

## Status LED (WS2812 on GPIO48)

Wi-Fi phase (until connected): white dim = booting · blue pulse = provisioning AP · yellow
blink = connecting · red slow = Wi-Fi failed. Once Wi-Fi is up the LED shows the **nRF link**:
red slow = nRF not alive · cyan pulse = advertising/waiting · yellow = bonded, not ready ·
**green = ready to relay** · red fast = error.

## Home Assistant / MQTT

Broker configured in the web UI (stored in NVS). On connect, publishes MQTT-Discovery for **9
`button` entities** under device "Ganymede Bridge" + 3 env `sensor`s + an `nRF Link` diagnostic
sensor + availability LWT. HA button → `ganymede/cmd/<btn>` → UART `press <btn>`. (Buttons, not
a `climate` entity: the remote is stateless fire-per-press with no AC feedback.)

## HTTP API

`GET /api/status` · `GET /api/scan` · `POST /api/connect` (ssid,pass) · `POST /api/mqtt`
(host,port,user,pass) · `POST /api/press` (btn) · `GET /`

## nRF link status contract

The nRF emulator prints `status <token>\n` over UART (`boot` / `advertising` / `connected` /
`bonded` / `ready` / `error`) and re-pushes it every ~2 s, and toggles **GPIO6** at ~1 Hz as a
hardware heartbeat. No edge **and** no UART for 3 s ⇒ the bridge reports `offline` (so a
floating/absent nRF never shows a stale "ready"). Board-specific notes:
[`../../docs/HARDWARE.md`](../../docs/HARDWARE.md).
