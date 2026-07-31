# ESP32-S3 bridge — Wi-Fi front-end **and** BLE remote emulator

The whole product on one board: Wi-Fi/MQTT/HTTP provisioning, a web UI of the De'Longhi
remote, and the Ganymede BLE emulator itself (`main/ble_emu.c`, NimBLE) that the AC bonds
with. A button press is a GATT notification from this chip — there is no second board and no
UART link any more.

> **Not yet confirmed against the AC.** The Wi-Fi/UI/MQTT half is unchanged and working; the
> on-chip BLE emulator is a port of the field-verified nRF52840/Zephyr firmware and still needs
> its on-air confirmation. The two-board build in release v0.3.0 remains the known-good unit.
> What has to be right for the AC to pair is documented at the top of `main/ble_emu.c`.

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

**Also check the serial log once.** The emulator self-checks its GATT layout at boot:

```
I bleemu: HID report value handle = 0x003b (target 0x003b), env padding = 15
```

If the handle is *not* `0x003b`, the next line tells you exactly how many entries to add to or
remove from `env_pad[]` in `main/ble_emu.c`. NimBLE assigns handles in registration order and
the exact count depends on the IDF version's built-in GAP/GATT services, so this is a one-time
per-toolchain adjustment. The AC discovers by UUID, so it may well pair regardless — but if it
does not, fix this before looking anywhere else.

## Build it locally (ESP-IDF v6.0.1)

```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32c3          # or esp32s3 / esp32c6 / esp32 — see below
idf.py -p <PORT> build flash monitor
```

After changing target or Kconfig, `idf.py fullclean` first.

The web UI (`main/www/index.html`, self-contained HTML/CSS/SVG) is embedded via `EMBED_FILES`.

### Supported chips

The emulator itself is chip-agnostic; what differs per chip is the status LED, the pin defaults
and how much RAM you have. Both live in code, not in your head: `main/board.h` for pins and the
LED, `sdkconfig.defaults.<target>` for the rest.

| Target | Status LED | I²C SDA/SCL | LD2410 TX/RX | Notes |
|---|---|---|---|---|
| `esp32s3` | WS2812 GPIO48 | 2 / 1 | 17 / 18 | Reference board. 2 MB PSRAM, nothing to trim. |
| `esp32c3` | WS2812 GPIO8 | 5 / 6 | 7 / 10 | ~400 KB SRAM, no PSRAM → see the RAM trims in `sdkconfig.defaults.esp32c3`. |
| `esp32c6` | WS2812 GPIO8 | 7 / 6 | 16 / 17 | Untested. Adjust if your board's LCD owns those pins. |
| `esp32` | plain LED GPIO2 | 21 / 22 | 17 / 16 | Classic WROOM-32. Console is UART0, so GPIO1/3 are reserved. |

`board.h` rejects a chip that cannot do the job at compile time — the **ESP32-S2 has no
Bluetooth**, the H2 no Wi-Fi, the P4 no radio. Run `tools/identify-boards.sh` to see what a
board actually is before building for it.

Three C3-specific traps the defaults already avoid, worth knowing if you re-pin anything:
GPIO11 is VDD_SPI and 12–17 are the SPI flash (touching them kills the board); GPIO18/19 are
the native USB D−/D+, i.e. the port you flash and monitor over; and the C3 has only UART0/1, so
the LD2410 runs on **UART1** on every target (it was UART2 back when UART1 was the nRF link).

---

## Wiring

Only the optional sensors are wired now — `GPIO4/5/6` (the old nRF UART + heartbeat) are free.

| ESP32-S3 | → | Sensor |
|---|---|---|
| GPIO1 (SCL) | → | BME280 SCL |
| GPIO2 (SDA) | ↔ | BME280 SDA |
| GPIO17 (TX) | → | LD2410 RX (optional presence radar) |
| GPIO18 (RX) | ← | LD2410 TX |

GND common. Pins are configurable in the web UI (stored in NVS, applied on reboot).

## Status LED (WS2812 on GPIO48)

Wi-Fi phase (until connected): white dim = booting · blue pulse = provisioning AP · yellow
blink = connecting · red slow = Wi-Fi failed. Once Wi-Fi is up the LED shows the **emulator**:
red slow = BLE host didn't start · cyan pulse = advertising/waiting for the AC · yellow = AC
connected but not encrypted yet · **green = bonded, ready to relay** · red fast = error.

## Home Assistant / MQTT

Broker configured in the web UI (stored in NVS). On connect, publishes MQTT-Discovery for **9
`button` entities** under device "Ganymede Bridge" + 3 env `sensor`s + a `BLE Link` diagnostic
sensor + availability LWT. HA button → `ganymede/cmd/<btn>` → a HID notification to the AC.
(Buttons, not a `climate` entity: the remote is stateless fire-per-press with no AC feedback.)

The diagnostic sensor's topic and `unique_id` are still `ganymede/nrf` / `ganymede_nrf` on
purpose — renaming them would orphan the entity in existing Home Assistant installs.

## HTTP API

`GET /api/status` · `GET /api/scan` · `POST /api/connect` (ssid,pass) · `POST /api/mqtt`
(host,port,user,pass) · `POST /api/press` (btn) · `GET /`

## Emulator status contract

`ble_emu_status()` reports `offline` / `boot` / `advertising` / `connected` / `ready` /
`error`, surfaced in `/api/status` as `ble` and on MQTT. Being in-process, there is no
heartbeat or liveness timeout to get wrong — `offline` now only means the BLE host did not
come up.

There is no separate `bonded` rung. On the nRF it meant "bonded but the AC hasn't subscribed
to our HID report", which needed a force-subscribe hack to escape; here the report is pushed
with `ble_gatts_notify_custom()`, which ignores the CCCD, so bonded *is* relay-capable.

**Pairing:** with no bond the emulator advertises Flags `0x01` (LE *Limited* Discoverable) —
the AC's pairing scan looks for exactly that. Once bonded it switches to `0x06` for the
encrypted reconnect. The web UI's **Unpair** clears the bond and drops back to pairing mode,
the same as holding MODE on a physical remote. Board and radio notes:
[`../../docs/HARDWARE.md`](../../docs/HARDWARE.md).
