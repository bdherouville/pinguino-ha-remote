# CLAUDE.md

Guidance for Claude Code in this repo. Keep it short; prune anything Claude can infer
from the code.

## What this project is

Build a **connected remote** that controls a **De'Longhi air-conditioner (AC)** by
**emulating its manual BLE remote** ("Ganymede", a Cypress PSoC 4 BLE / BLE 4.2 HID
keyboard). The emulated remote must: enter **pairing mode**, be **visible**, **accept
the AC's connection** (the AC is the BLE central), then send the **same HID button
reports** as the manual remote.

## `/goal` (the verification that defines "done")

> The AC pairs/bonds with our emulator and a command (`press power`, later MQTT/HTTP)
> **changes the AC's state**, confirmed on-air by the nRF Sniffer.

Always show evidence (serial log, `.pcap`), not just "it should work".

## Architecture — one board for the product, a second radio only for RE

The ESP32 **cannot sniff** this device (its radio never locks onto the remote's sparse
low-power `ADV_IND`), so reverse engineering needs an nRF52840 running the nRF Sniffer.
But **emulating needs no receive path** — the emulator advertises and the AC scans — so
the emulator lives on the ESP32-S3 with everything else.

- **ESP32-S3** = the whole product: Wi-Fi, web UI, MQTT/HA, **and** the BLE emulator
  (`firmware/esp32_bridge/main/ble_emu.c`, NimBLE).
- **nRF52840** = nRF Sniffer, for captures only. The Zephyr emulator under
  `firmware/nrf52_emulator/zephyr/` is the field-verified two-board fallback.

```
HA/LAN ──MQTT/HTTP──► ESP32-S3 (bridge + emulator) ──BLE──► AC (central)
                      nRF52840 (nRF Sniffer) ──► Wireshark
```

**Do not reinstate "the ESP32 can't do this BLE".** It conflated sniffing with
emulating. The AC's pairing gate is the advertising **discoverable-flags byte**
(`0x01` Limited when unbonded), not chip identity and not the address — see the
protocol doc's *Pairing mode* and *Hardware reception*.

## Status

Protocol fully reverse-engineered; the two-board build works end to end. The
single-board ESP32-S3 emulator is **written but not yet confirmed against the AC** —
that is the open item. Its four must-haves are the header comment of `ble_emu.c`.

## Layout

- `docs/ganymede_protocol.md` — **authoritative** protocol & RE findings (advertising,
  GATT, Report Map, 9-button map, the discoverable-flags pairing gate, SMP, connection,
  status). `docs/HARDWARE.md` — board/build notes. `docs/references/` — datasheets + manual.
- `captures/` — **local-only**, git-ignored immutable captures (sniffer/HCI). **Never
  edit raw; never commit unredacted keys (LTK/IRK/CSRK/passkeys).**
- `firmware/esp32_bridge/main/ble_emu.c` — the live emulator (NimBLE).
- `firmware/nrf52_emulator/zephyr/src/main.c` — the field-verified emulator; **source of
  truth for on-air behaviour** when in doubt, since it is the one proven on the AC.
- `firmware/nrf52_emulator/reference/esp-idf-nimble/` — the *broken* first ESP32 attempt.
  Historical only: it advertised General Discoverable and never requested security. Do not
  copy from it.
- `tools/linux-ble/` — Linux/BlueZ central harness to test the emulator before the AC.

## Documentation discipline

Every protocol claim carries a **Status** (`observed`/`inferred`/`partial`/`unknown`)
and a **Source** (a capture under `captures/`). Never upgrade a Status
without a capture. Never hard-code an emulator value whose requirement row is still
`unknown`. Claims derived from *press-counting* a snoop (rather than reading the bytes)
are weak: that is how UP/DOWN ended up inverted in the button table for months.

## Build / flash

- **nRF Sniffer (#1):** flash Nordic `nrf_sniffer_*.uf2` via UF2 (double-tap RESET →
  drag-drop). Wireshark + extcap plugin.
- **ESP32-S3 (bridge + emulator):** ESP-IDF v6.0.1 —
  `source ~/.espressif/v6.0.1/esp-idf/export.sh` then
  `idf.py fullclean && idf.py erase-flash && idf.py build flash monitor`.
  After a pinout/Kconfig change, `fullclean` matters. Watch the boot log for the HID
  report-handle line — it self-checks the GATT layout.
- **nRF emulator (two-board fallback):** nRF Connect SDK / Zephyr,
  `west build -b nice_nano/nrf52840 …` then drag `zephyr.uf2` to `NICENANO`.

## Workflow

Explore → plan → implement → **verify against `/goal`**. Use the Linux harness for a
short feedback loop (validate the emulator's GATT/pairing before risking the real AC).
To capture the AC's reconnect/pairing you need the nRF Sniffer — a controller-based
scanner (ESP32/phone GATT) can't see the remote↔AC link.
