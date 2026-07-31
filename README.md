# Pinguino, minus a board 🐧

> A fork of [bdherouville/pinguino-ha-remote](https://github.com/bdherouville/pinguino-ha-remote) —
> same trick, one less chip. **The nRF52840 is gone.** A single ESP32 now pretends to be your
> air-conditioner's BLE remote *and* talks to Home Assistant.

The upstream project pairs an ESP32 (Wi-Fi, web UI, MQTT) with an nRF52840 (the BLE radio) over
a UART, because the AC supposedly refused to pair with an ESP32 emulator. Reasonable conclusion.
It was also wrong.

The original ESP32 attempt failed for three reasons that had nothing to do with the radio: it
advertised **General** Discoverable when the AC's pairing scan looks for **Limited** (`0x01`), it
put the Cypress manufacturer data in the primary advertisement instead of the scan response, and
it never sent an SMP Security Request — this AC waits for the *remote* to ask for security, so
the link just sat there unpaired until it timed out. Nobody retested it after the discoverable
flag was understood.

Two capabilities had also been quietly conflated. An ESP32 genuinely **cannot sniff** this
remote — its radio never locks onto those sparse low-power adverts, which is why reverse
engineering still wants an nRF Sniffer. But **emulating needs no receive path at all**: you
advertise, the AC scans and connects to you. Different job, different requirements.

## Does it actually work?

**Pairing: yes, confirmed on air.** An ESP32-C3 bonded with the AC:

```
I bleemu: advertising as "Ganymede", flags 0x01 (no bond: LIMITED discoverable = pairing mode)
I bleemu: connected (conn=1)
I bleemu: encryption change: status=0 encrypted=1 bonded=1
```

Button relay uses the same report bytes and the same GATT layout as the field-proven nRF
firmware, so it *should* just work — but it's the newest code path here, so treat it as
"believed good" rather than "watched it move the unit a hundred times".

Upstream's two-board build remains the belt-and-braces option:
[release v0.3.0](https://github.com/bdherouville/pinguino-ha-remote/releases/tag/v0.3.0).

## Quick start

```bash
# 0. What have you actually got in that drawer?
tools/identify-boards.sh

# 1. Build for your chip
. ~/esp/esp-idf/export.sh
cd firmware/esp32_bridge
idf.py fullclean && idf.py set-target esp32c3      # or esp32s3 / esp32c6 / esp32
idf.py -p /dev/cu.usbmodemXXXX build flash monitor
```

First boot raises an open AP called **`Ganymede-Bridge`** → connect → `http://192.168.4.1` →
pick your Wi-Fi, and set your MQTT broker while you're there.

Then pair: with the AC **plugged in but switched off**, hold **MODE** for ~10 s until the dot on
its display blinks rapidly. The emulator is already advertising in pairing mode (no bond = Limited
Discoverable, exactly like a real remote after its own MODE-hold), so they bond within ~60 s. LED
goes green, and you're driving an air conditioner from a browser.

> ⚠️ **Only one `Ganymede` may advertise at a time.** If you still have the old nRF board powered
> up nearby, the AC will happily latch onto the wrong one and you'll spend an hour wondering why.

## Which ESP32?

Anything with **both** BLE and Wi-Fi. `board.h` refuses to compile for anything else, which is
kinder than a mystery at runtime:

| Chip | Status LED | I²C SDA/SCL | LD2410 TX/RX | |
|---|---|---|---|---|
| **ESP32-C3** | WS2812 GPIO8 | 5 / 6 | 7 / 10 | ✅ pairing confirmed on this one |
| **ESP32-S3** | WS2812 GPIO48 | 2 / 1 | 17 / 18 | upstream's reference board |
| **ESP32-C6** | WS2812 GPIO8 | 7 / 6 | 16 / 17 | untested, should be fine |
| **ESP32** (classic) | plain LED GPIO2 | 21 / 22 | 17 / 16 | untested |

**The trap:** the **ESP32-S2 has no Bluetooth at all**. It sits between the S1 and S3 in the
naming and looks like a sibling, but it's Wi-Fi only — it will never pair, no matter what you
do. Likewise the **H2** (no Wi-Fi) and the **P4** (no radio whatsoever). Run
`tools/identify-boards.sh` before you burn an evening.

Pins are also editable in the web UI, and the validator now knows each chip's real map — it'll
refuse the SPI-flash pins that brick the board and the USB pins that cost you the console.

## Wiring

One board. The only wire left is the optional sensor:

```
BME280        ESP32-C3        (S3: SDA=2 SCL=1)
  VIN  ──────  3V3            ← 3.3 V, not 5 V
  GND  ──────  GND
  SDA  ──────  GPIO5
  SCL  ──────  GPIO6
```

Both I²C addresses (0x76/0x77) are probed automatically and internal pull-ups are on, so a bare
module with no address jumper works.

## When it doesn't pair

The boot log is designed to tell you. Look for these three lines:

```
I bleemu: GATT: HID service @ 0x0037, report value @ 0x003b (target 0x003b), ENV_PAD_N = 13
I bleemu: address 00:a0:50:xx:xx:xx (type 0)
I bleemu: advertising as "Ganymede", flags 0x01 (...)
```

- **Report handle isn't `0x003b`?** The next log line tells you the exact `ENV_PAD_N` to set in
  `ble_emu.c`. NimBLE numbers GATT handles in registration order and the built-in GAP/GATT
  services differ between IDF versions, so this is a one-time, one-number adjustment. It may well
  pair anyway (the AC discovers by UUID) — if it doesn't, try the suggested value, then try `0`.
- **`flags 0x06` instead of `0x01`?** It thinks it's already bonded. Hit **Unpair** in the web UI.
- **`connected` but never `encryption change`?** The AC declined the security request.
- **`disconnected, reason=62`?** That's HCI `0x3E`, and it's normal here — establishing this link
  is a lottery even for the real remote. Just retry.

## Credit

All the hard work — the reverse engineering, the protocol, the captures, the whole bridge — is
[Bertrand d'Hérouville](https://github.com/bdherouville)'s. This fork deletes a board and fixes
an inverted UP/DOWN in the button table. MIT, same as upstream.

---

<sub>Everything below is the original project README.</sub>

# pinguino-ha-remote

![De'Longhi Pinguino, now connected: web UI, Home Assistant, MQTT/HTTP, ambient sensor, via an emulated BLE remote on a single ESP32](docs/assets/photos/feature-overview.png)

Control a **De'Longhi Pinguino air-conditioner** from your LAN / Home Assistant by
**emulating its manual BLE remote**. Commands from the web UI, MQTT, or Home Assistant bond
with the AC and **change its state on-air** — no cloud, no IR blaster.

**Status:** working end-to-end on the two-board build (`power`, `up`, `down`, `mode`, `eco`,
`timer`, `fan`, `silent`, `flap` all act on the AC). **`main` is now single-board** — the BLE
emulator runs on the ESP32's own radio, and **pairing is confirmed on air** (an ESP32-C3 bonds
with the AC). Button relay reuses the proven report bytes and GATT layout but has had less
mileage. For the most-tested unit, use
[release v0.3.0](https://github.com/bdherouville/pinguino-ha-remote/releases/tag/v0.3.0).

## What you get

One small board, plus an optional sensor:

```
HA / LAN ──MQTT/HTTP──► ESP32-S3 ──BLE──► AC
```

- **ESP32-S3** — everything: Wi-Fi, web UI, MQTT/Home Assistant, *and* the BLE remote
  emulator that bonds with the AC.
- **BME280** — ambient temperature / humidity / pressure (optional), reported to HA.

The nRF52840 used to be here because the AC was thought to reject an ESP32 emulator. That was
an artefact of the old ESP32 build advertising the wrong discoverable flag, not a radio
limitation — the ESP32 still can't *sniff* the remote, but it doesn't need to in order to
*be* one. Full reasoning and the protocol: [`docs/ganymede_protocol.md`](docs/ganymede_protocol.md).

## Bill of materials

| Qty | Part | Role | Source |
|-----|------|------|--------|
| 1 | **ESP32-S3 SuperMini** (ESP32-S3FH4R2) | Wi-Fi bridge **+ BLE emulator** | [AliExpress](https://fr.aliexpress.com/item/1005008807808123.html) |
| 1 | **BME280 / BMP280** module | ambient T/H/P (optional) | [AliExpress](https://fr.aliexpress.com/item/1005007527106667.html) |
| — | USB cables + jumper wires | power & wiring | — |

*(Reverse-engineering the protocol yourself needs an nRF52840 as a sniffer — see
[`tools/nrf_sniffer/`](tools/nrf_sniffer/README.md). A working deployment doesn't: the ESP32
can't sniff, but it doesn't need to.)*

| | |
|---|---|
| ![Mounted boards](docs/assets/photos/mounted-boards.jpg) | ![Close-up of the powered stack](docs/assets/photos/mounted-boards-closeup.jpg) |
| nRF52840 emulator + ESP32-S3 bridge + BME280 | Powered: red = nRF, green = ESP32-S3 |

## Get a working unit

Prebuilt binaries are attached to every
[**release**](https://github.com/bdherouville/pinguino-ha-remote/releases/latest). Releases up
to v0.3.0 are the two-board build; the single-board firmware currently has to be built locally.

### 1 · Flash the ESP32-S3

- **Browser:** open the [web flasher](https://bdherouville.github.io/pinguino-ha-remote/flash/)
  (Chrome/Edge), plug in, **Install**. *(Serves the last release — still two-board.)*
- **CLI:** `esptool --chip esp32s3 -p <PORT> write_flash 0x0 ganymede-bridge-esp32s3.bin`
- **Local build** (needed for the single-board firmware):
  `. ~/esp/esp-idf/export.sh && cd firmware/esp32_bridge && idf.py build flash monitor`

Details → [`firmware/esp32_bridge/README.md`](firmware/esp32_bridge/README.md).

### 2 · Wire the sensor (optional)

```
ESP32-S3        BME280
  GPIO1 (SCL) ─► SCL
  GPIO2 (SDA) ◄► SDA
  GND ───────── GND
```
Pinouts and board quirks: [`docs/HARDWARE.md`](docs/HARDWARE.md).

### 3 · Connect the bridge to Wi-Fi

On first boot the ESP32 raises an open AP **`Ganymede-Bridge`** → connect → open
`http://192.168.4.1` → pick your Wi-Fi → (optional) set your **MQTT broker** for Home Assistant.

### 4 · Pair with the AC

The emulator behaves like a real remote: with **no bond** it automatically advertises in
**pairing mode** (Limited Discoverable) as `Ganymede` — no button needed.

1. Put the **AC** in pairing mode (make sure the **AC is plugged in and turned off**, then
   **hold MODE ~10 s**; its display dot blinks rapidly).
2. They bond within ~60 s. The board's LED goes **green** = ready to relay.

On the first boot of the single-board firmware, check the serial log for the line reporting the
HID report handle — it says whether the GATT layout matched the real remote, and if not, exactly
what to change. See [`firmware/esp32_bridge/README.md`](firmware/esp32_bridge/README.md).

That's it — it **pairs, unpairs, re-pairs, and switches with the original remote exactly like a
physical remote**, so the address doesn't matter (the prebuilt generic binary works as-is).

### 5 · Control it

Use the bridge **web UI**, **Home Assistant** (9 buttons + 3 sensors auto-discovered over
MQTT), or publish to `ganymede/cmd/<button>`.

## Documentation

- [**Protocol & reverse engineering**](docs/ganymede_protocol.md) — identity, advertising,
  GATT, button reports, the link-layer pairing gate, SMP, and how it was reverse-engineered.
- [**Hardware & build notes**](docs/HARDWARE.md) — board gotchas (clock, bootloader, flash
  offset), pinouts, why the BLE lives on the nRF.
- [**Releasing**](RELEASING.md) — how CI builds and publishes the binaries.
- Firmware: [ESP32-S3 bridge](firmware/esp32_bridge/README.md) ·
  [nRF52840 emulator](firmware/nrf52_emulator/README.md) ·
  [nRF Sniffer](tools/nrf_sniffer/README.md).

## License

[MIT](LICENSE) for this project's code and docs. Third-party components (Nordic nRF Sniffer,
ESP-IDF managed components, the NimBLE reference) keep their upstream licenses.
