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

## Architecture — two radios, by necessity

The **ESP32 cannot do the BLE** for this device: its radio neither sniffs the real
remote nor gets seen by the AC as an emulator (both verified; Android, a non-ESP
radio, does both). So:

- **nRF52840** = BLE radio for everything (sniffer + emulator). 3 boards: #1 sniffer,
  #2 emulator, #3 spare.
- **ESP32-S3** = Wi-Fi bridge only (MQTT/HTTP ↔ UART to the nRF emulator).

```
HA/LAN ──MQTT/HTTP──► ESP32-S3 ──UART──► nRF52840 (emulator) ──BLE──► AC (central)
                                          nRF52840 (nRF Sniffer) ──► Wireshark
```

## Phases (do in order; M4 onward gated by RE)

1. **Sniffer first** (`tools/nrf_sniffer/`) — resolve the **AC-side SMP contradiction**
   (Just Works vs MITM + LE Secure Connections) and capture keys.
2. **Emulator** (`firmware/nrf52_emulator/`, Bluefruit) — port the reference NimBLE
   emulator with the Phase-1 SMP.
3. **ESP32 Wi-Fi bridge** (`firmware/esp32_bridge/`).

Status: protocol fully reverse-engineered; **blocker = AC-side SMP** (Phase 1).

## Layout

- `docs/ganymede_protocol.md` — **authoritative** protocol & RE findings (advertising,
  GATT, Report Map, 9-button map, link-layer identity gate, SMP, connection, status).
  `docs/HARDWARE.md` — board/build notes. `docs/references/` — datasheets + AC manual.
- `captures/` — **local-only**, git-ignored immutable captures (sniffer/HCI). **Never
  edit raw; never commit unredacted keys (LTK/IRK/CSRK/passkeys).**
- `firmware/nrf52_emulator/reference/esp-idf-nimble/` — the working delonghi NimBLE
  emulator: **source of truth for the port**, not built here.
- `tools/linux-ble/` — Linux/BlueZ central harness to test the emulator before the AC.

## Documentation discipline

Every protocol claim carries a **Status** (`observed`/`inferred`/`partial`/`unknown`)
and a **Source** (a capture under `captures/`). Never upgrade a Status
without a capture. Never hard-code an emulator value whose requirement row is still
`unknown` (today: the AC-side SMP).

## Build / flash

- **nRF Sniffer (#1):** flash Nordic `nrf_sniffer_*.uf2` via UF2 (double-tap RESET →
  drag-drop). Wireshark + extcap plugin.
- **nRF emulator (#2), Bluefruit:** `arduino-cli compile/upload -b adafruit:nrf52:pca10056`
  or UF2. (Escalate to nRF Connect SDK/Zephyr only if Phase 1 shows MITM+SC needs it.)
- **ESP32-S3 bridge:** ESP-IDF v6.0.1 —
  `source ~/.espressif/v6.0.1/esp-idf/export.sh` then
  `idf.py fullclean && idf.py erase-flash && idf.py build flash monitor`.

## Workflow

Explore → plan → implement → **verify against `/goal`**. Use the Linux harness for a
short feedback loop (validate the emulator's GATT/pairing before risking the real AC).
To capture the AC's reconnect/pairing you need the nRF Sniffer — a controller-based
scanner (ESP32/phone GATT) can't see the remote↔AC link.
