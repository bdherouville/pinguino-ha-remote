# ESP32-S3 bridge — Wi-Fi front-end for the Ganymede emulator

ESP32-S3 = network brain (the BLE lives on the nRF52840). Provisioning + a web UI of the
De'Longhi remote whose buttons are forwarded over **UART** to the nRF emulator.

**Status (2026-06-02):** scaffold **compiles** under ESP-IDF v6.0.1 (binary ~889 KB).
Implemented: AP-fallback provisioning, Wi-Fi scan/connect web UI, RGB status LED, web
remote with clickable buttons → UART. Home Assistant / MQTT and the framed UART protocol
are the next steps.

## Features

- **Wi-Fi**: connects in STA using saved creds (NVS). If none / it fails, raises a
  **provisioning AP** `Ganymede-Bridge` (open) — connect to it, open `http://192.168.4.1`,
  **scan** networks, pick one, enter the password, **connect**. Then reach the UI on the
  LAN IP.
- **RGB status LED** (WS2812 on **GPIO48**):

  Wi-Fi phase (until connected):

  | Colour | Meaning |
  |---|---|
  | white dim | booting |
  | blue pulse | provisioning AP up, waiting for Wi-Fi config |
  | yellow blink | connecting to Wi-Fi |
  | red slow blink | Wi-Fi connect failed |

  Once Wi-Fi is connected the LED shows the **nRF link** state instead:

  | Colour | Meaning |
  |---|---|
  | red slow blink | nRF emulator not alive (no heartbeat) |
  | cyan pulse | advertising / waiting for the AC |
  | yellow steady | AC connected/bonded, not ready yet |
  | **green steady** | bonded + HID subscribed — can relay |
  | red fast blink | error |

- **Web UI** (`/`): the remote rendered in pure **HTML/CSS/SVG** (no bitmap) with 9
  clickable keys (flap/silent/fan/timer/eco/mode/down/up/power) → `POST /api/press` → UART
  `press <btn>`. Below the keys, a live **"screen"** shows the BME280 readings (°C / %RH /
  hPa) and Wi-Fi / MQTT / nRF status dots, refreshed from `GET /api/status` every 3 s.
- **Home Assistant / MQTT**: broker configured in the web UI (stored in NVS). On connect,
  publishes **MQTT-Discovery for 9 `button` entities** under device "Ganymede Bridge"
  (`homeassistant/button/ganymede_<btn>/config`, retained) + 3 env `sensor`s + an **`nRF Link`
  diagnostic sensor** (`ganymede/nrf`) + availability `ganymede/status` (online/offline LWT).
  HA button → `ganymede/cmd/<btn>` → UART `press <btn>`. (Buttons, not a `climate` entity:
  the remote is stateless fire-per-press with no AC feedback, so assumed-state would drift.)
- **UART to nRF**: `UART1`, **TX=GPIO4 → nRF RX**, **RX=GPIO5 ← nRF TX**, GND common,
  115200 8N1. ESP→nRF: `press <btn>\n`, `env <t> <h> <p>\n`.

### nRF link status contract (the nRF emulator must implement this)
- **Rich state over UART** — the emulator prints `status <token>\n` on every BLE state change.
  Tokens: `boot`, `advertising` (`adv`/`pairing`), `connected` (`conn`), `bonded`, `ready`,
  `error` (`err`). The bridge maps these to the LED / `nRF Link` sensor.
- **Hardware heartbeat** — the emulator toggles **GPIO6** (ESP input, pulled-down) at ~1 Hz.
  No edge **and** no UART for 3 s ⇒ the bridge reports `offline` (overrides the last token).
  A floating/absent nRF therefore reads `offline`, never a stale "ready".

## API
`GET /api/status` (incl. `mqtt`/`mqtt_host`) · `GET /api/scan` · `POST /api/connect` (ssid,pass) ·
`POST /api/mqtt` (host,port,user,pass) · `POST /api/press` (btn) · `GET /`

## Build / flash (ESP-IDF v6.0.1)
```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3                                  # first time
idf.py fullclean && idf.py erase-flash && idf.py build flash monitor
```
Console is on the native USB-Serial/JTAG. The web UI (`main/www/index.html`, self-contained
HTML/CSS/SVG) is embedded via `EMBED_FILES`.

## Next
- **Framed UART** `[SOF][LEN][SEQ][TYPE][CRC16]` + ACK (replaces plain text; needs the
  matching parser on the nRF emulator). The `status`/heartbeat contract above folds into it.
- Fine-tune the button hotspot coordinates in `index.html`; optionally drop the AP after a
  successful STA connect.
- WS2812 caveat: some S3 super-mini revisions wire a plain LED on GPIO48 — then colours
  won't render (harmless). See `docs/re/ESP32-S3-SuperMini-BOARD.md`.
